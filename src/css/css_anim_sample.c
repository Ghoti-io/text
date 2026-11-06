/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Text.
 *
 * Ghoti.io Text is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Text is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Sampling the animations of one element at the times a caller gives (css_style.h, GTEXT_CSS_Style_Animation).
 *
 * The cascaded declarations of the element are computed once (the **base style**: "the computed value for that property in the absence of animations", the underlying value of
 * Web Animations 1). The `animation-*` longhands of that style are the effects. For each property a keyframe of an effect with a resolved time names, the value of the keyframe is
 * the property's computed value in the element's own context with the keyframe's declarations added to its cascade (so `em`, `var()`, `inherit` and `sibling-index()` in a keyframe
 * are the element's: "computed keyframes" are live). The effect value of Web Animations 1 is found between the two keyframes that bracket the progress, the effects of a property are
 * composited in the order of the names, and the result is declared in the animation origin, which overrides every normal declaration and nothing important. The style is then computed
 * again from that, so that what depends on the animated value (the children that inherit it, a `var()` that reads an animated custom property, an `em`) sees it.
 *
 * The time is the caller's: an effect whose time is not given is not sampled and its properties are reported (css_anim_untimed), so that a resolved value can say that the underlying
 * value stands for lack of a timeline.
 */

#include <string.h>

#include <ghoti.io/text/css/css_animation.h>
#include <ghoti.io/text/css/css_style.h>

#include "css_anim_internal.h"
#include "css_compute_internal.h"
#include "css_creg.h"
#include "css_pseudo_apply.h"
#include "css_substitute_internal.h"

#define MAX_KEYS 8192u

/* ---- values --------------------------------------------------------------------------------------------------------------------------------------- */

/// One value of one property: a typed computed value (retained), or for an unregistered custom property its text.
typedef struct {
  GTEXT_CSS_Value_Set * set;
  char * text;
  size_t length;
} val;

typedef struct {
  GTEXT_CSS_Property_Id id;   ///< A longhand, or 0 for a custom property.
  char * name;                ///< A custom property's name (owned).
  size_t name_length;
} key;

/// One effect with a resolved time.
typedef struct {
  GTEXT_CSS_Anim_Effect effect;
  const GTEXT_CSS_Keyframes * kf;
  GTEXT_CSS_Anim_Timing timing;
  int * cover;                       ///< By frame and key: the declaration of the frame that sets the key (the last one wins), -1 for none.
  GTEXT_CSS_Easing ** ease;          ///< By frame: the `animation-timing-function` the keyframe declares (owned), or NULL.
  int * comp;                        ///< By frame: the `animation-composition` the keyframe declares, -1 for none.
  GTEXT_CSS_Computed_Style ** frame; ///< By frame: the style computed with the frame's declarations added (owned), or NULL until needed.
} entry;

typedef struct {
  const GTEXT_CSS_Compute_Context * cc;
  const GTEXT_CSS_Registry * reg;
  GTEXT_Allocator al;
  const GTEXT_CSS_Style_Options * options;
  GTEXT_CSS_Element_Handle element;
  const GTEXT_CSS_Computed_Style * parent;
  const GTEXT_CSS_Declaration_Block * base;
  GTEXT_CSS_Computed_Style * base_style;
  GTEXT_CSS_Property_Id id_timing, id_composition;
  unsigned asked_pseudo;
  GTEXT_CSS_Compute_Result * failure;   ///< What the compute that failed said (the style's result carries it: which callback or property was missing).
  key * keys;
  size_t nkeys, capkeys;
  entry * entries;
  size_t nentries;
} sampler;

static void val_free(const GTEXT_Allocator * al, val * v) {
  gtext_css_value_set_free(v->set);
  gtext_allocator_free(al, v->text);
  memset(v, 0, sizeof *v);
}

static const GTEXT_CSS_Value * val_root(const val * v) {
  return v->set ? gtext_css_value_set_root(v->set) : NULL;
}

/* ---- keys ------------------------------------------------------------------------------------------------------------------------------------------ */

static bool is_custom_name(const char * name, size_t n) {
  return n >= 2 && name[0] == '-' && name[1] == '-';
}

static size_t find_key(const sampler * s, GTEXT_CSS_Property_Id id, const char * name, size_t n) {
  for (size_t i = 0; i < s->nkeys; i++) {
    const key * k = &s->keys[i];
    if (id ? k->id == id : (k->id == 0 && k->name_length == n && memcmp(k->name, name, n) == 0)) {
      return i;
    }
  }
  return (size_t)-1;
}

static GTEXT_CSS_Status add_key(sampler * s, GTEXT_CSS_Property_Id id, const char * name, size_t n) {
  if (find_key(s, id, name, n) != (size_t)-1) {
    return GTEXT_CSS_OK;
  }
  if (s->nkeys >= MAX_KEYS) {
    return GTEXT_CSS_E_LIMIT_DECLARATIONS;
  }
  if (s->nkeys == s->capkeys) {
    const size_t grown = s->capkeys ? s->capkeys * 2 : 16;
    key * bigger = gtext_allocator_realloc(&s->al, s->keys, grown * sizeof *bigger);
    if (!bigger) {
      return GTEXT_CSS_E_OOM;
    }
    s->keys = bigger;
    s->capkeys = grown;
  }
  key * k = &s->keys[s->nkeys];
  memset(k, 0, sizeof *k);
  k->id = id;
  if (!id) {
    k->name = gtext_allocator_malloc(&s->al, n + 1);
    if (!k->name) {
      return GTEXT_CSS_E_OOM;
    }
    memcpy(k->name, name, n);
    k->name[n] = '\0';
    k->name_length = n;
  }
  s->nkeys++;
  return GTEXT_CSS_OK;
}

/// Whether an animation of the property is applied at all: it must be animatable, and apply to the pseudo-element the style is of.
static bool animatable(const sampler * s, GTEXT_CSS_Property_Id id) {
  GTEXT_CSS_Property_Info pi;
  if (!gtext_css_registry_property_info(s->reg, id, &pi)) {
    return false;
  }
  if (pi.animation == GTEXT_CSS_ANIM_NOT_ANIMATABLE) {
    return false;
  }
  return !s->asked_pseudo || css_pseudo_applies(s->asked_pseudo, &pi);
}

typedef GTEXT_CSS_Status (*longhand_fn)(sampler * s, void * user, GTEXT_CSS_Property_Id id, const char * name, size_t n);

/// Call `fn` for each longhand a declared property sets (the property itself for a longhand, the longhands of a shorthand, nested ones through the stack) or for the custom
/// property. `all` sets none: a keyframe that wrote it animates nothing here.
static GTEXT_CSS_Status each_longhand(sampler * s, GTEXT_CSS_Property_Id id, const char * name, size_t n, longhand_fn fn, void * user) {
  if (is_custom_name(name, n)) {
    return fn(s, user, 0, name, n);   /* the registry gives the pattern `--*` an id; the key is the name */
  }
  if (!id) {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Property_Id stack[64];
  size_t top = 0;
  stack[top++] = gtext_css_registry_resolve_alias(s->reg, id);
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  while (top && st == GTEXT_CSS_OK) {
    const GTEXT_CSS_Property_Id cur = stack[--top];
    GTEXT_CSS_Property_Info pi;
    if (!gtext_css_registry_property_info(s->reg, cur, &pi)) {
      continue;
    }
    if (!(pi.flags & GTEXT_CSS_PROP_SHORTHAND)) {
      st = fn(s, user, cur, NULL, 0);
      continue;
    }
    if (pi.name_length == 3 && memcmp(pi.name, "all", 3) == 0) {
      continue;
    }
    for (int pass = 0; pass < 2; pass++) {
      for (size_t i = 0;; i++) {
        const GTEXT_CSS_Property_Id lh = pass == 0 ? gtext_css_registry_longhand(s->reg, cur, i) : gtext_css_registry_reset_longhand(s->reg, cur, i);
        if (!lh) {
          break;
        }
        if (top < sizeof stack / sizeof *stack) {
          stack[top++] = lh;
        }
      }
    }
  }
  return st;
}

static GTEXT_CSS_Status collect_fn(sampler * s, void * user, GTEXT_CSS_Property_Id id, const char * name, size_t n) {
  (void)user;
  if (id && !animatable(s, id)) {
    return GTEXT_CSS_OK;
  }
  return add_key(s, id, name, n);
}

typedef struct {
  entry * e;
  size_t frame;
  int index;
} cover_args;

static GTEXT_CSS_Status cover_fn(sampler * s, void * user, GTEXT_CSS_Property_Id id, const char * name, size_t n) {
  cover_args * c = user;
  const size_t k = find_key(s, id, name, n);
  if (k != (size_t)-1) {
    c->e->cover[c->frame * s->nkeys + k] = c->index;
  }
  return GTEXT_CSS_OK;
}

static bool is_frame_attribute(const sampler * s, GTEXT_CSS_Property_Id id) {
  return id && (id == s->id_timing || id == s->id_composition);
}

/* ---- the entries ------------------------------------------------------------------------------------------------------------------------------------ */

static void entry_free(sampler * s, entry * e) {
  const size_t nf = e->kf ? e->kf->frame_count : 0;
  for (size_t f = 0; f < nf; f++) {
    if (e->ease) {
      gtext_css_easing_free(e->ease[f]);
    }
    if (e->frame) {
      gtext_css_computed_free(e->frame[f]);
    }
  }
  gtext_allocator_free(&s->al, e->cover);
  gtext_allocator_free(&s->al, e->ease);
  gtext_allocator_free(&s->al, e->comp);
  gtext_allocator_free(&s->al, e->frame);
}

/// The `animation-timing-function` or `animation-composition` of a keyframe, read through the registry's grammar.
static GTEXT_CSS_Status frame_attributes(sampler * s, entry * e, size_t f) {
  const GTEXT_CSS_Keyframe * fr = &e->kf->frames[f];
  e->comp[f] = -1;
  for (size_t i = 0; i < fr->property_count; i++) {
    const GTEXT_CSS_Keyframe_Property * p = &fr->properties[i];
    if (!is_frame_attribute(s, p->property)) {
      continue;
    }
    GTEXT_CSS_Property_Value pv;
    GTEXT_CSS_Property_Options po;
    memset(&po, 0, sizeof po);
    po.allocator = &s->al;
    GTEXT_CSS_Status st = gtext_css_parse_property_text(s->reg, p->name.data, p->name.length, p->text.data, p->text.length, &po, &pv);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    const GTEXT_CSS_Value * root = pv.outcome == GTEXT_CSS_PROP_RESULT_VALID ? gtext_css_value_set_root(pv.set) : NULL;
    if (root && p->property == s->id_timing) {
      const GTEXT_CSS_Value * first = root->kind == GTEXT_CSS_VAL_LIST && root->separator == GTEXT_CSS_SEP_COMMA && root->child_count ? &root->children[0] : root;
      bool valid = false;
      GTEXT_CSS_Easing * made = NULL;
      st = gtext_css_easing_from_value(first, &s->al, &valid, &made);
      if (st == GTEXT_CSS_OK && valid) {
        gtext_css_easing_free(e->ease[f]);
        e->ease[f] = made;
      }
    }
    else if (root && p->property == s->id_composition) {
      const GTEXT_CSS_Value * first = root->kind == GTEXT_CSS_VAL_LIST && root->separator == GTEXT_CSS_SEP_COMMA && root->child_count ? &root->children[0] : root;
      static const char * const WORDS[] = {"replace", "add", "accumulate"};
      for (int w = 0; w < 3; w++) {
        if (first->kind == GTEXT_CSS_VAL_KEYWORD && first->text.length == strlen(WORDS[w]) && memcmp(first->text.data, WORDS[w], first->text.length) == 0) {
          e->comp[f] = w;
        }
      }
    }
    gtext_css_property_value_free(&pv);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
  }
  return GTEXT_CSS_OK;
}

/// Declare `name: text` at the end of a block; a custom property goes through its own call. `*outcome` says what the parse of the value came to.
static GTEXT_CSS_Status add_declaration(sampler * s, GTEXT_CSS_Declaration_Block * b, const char * name, size_t name_length, const char * text, size_t length,
    GTEXT_CSS_Property_Outcome * outcome) {
  if (!is_custom_name(name, name_length)) {
    return gtext_css_block_add_text(b, name, name_length, text, length, false, NULL, outcome, NULL);
  }
  GTEXT_CSS_Property_Value pv;
  GTEXT_CSS_Property_Options po;
  memset(&po, 0, sizeof po);
  po.allocator = &s->al;
  GTEXT_CSS_Status st = gtext_css_parse_property_text(s->reg, name, name_length, text, length, &po, &pv);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  *outcome = pv.outcome;
  if (pv.outcome == GTEXT_CSS_PROP_RESULT_VALID || pv.outcome == GTEXT_CSS_PROP_RESULT_WIDE || pv.outcome == GTEXT_CSS_PROP_RESULT_PENDING) {
    st = gtext_css_block_add_custom(b, name, name_length, &pv, false);
  }
  gtext_css_property_value_free(&pv);
  return st;
}

/* ---- the style of a frame --------------------------------------------------------------------------------------------------------------------------- */

static GTEXT_CSS_Status compute_block(sampler * s, const GTEXT_CSS_Declaration_Block * block, GTEXT_CSS_Computed_Style ** out) {
  GTEXT_CSS_Compute_Result res;
  const GTEXT_CSS_Status st = gtext_css_compute_style(s->cc, block, s->parent, s->element, out, &res);
  if (st != GTEXT_CSS_OK && s->failure) {
    *s->failure = res;
  }
  return st;
}

/// A CSS-wide keyword that rolls a declaration back (`revert`, `revert-layer`, `revert-rule`): the keyframes have no origin, layer or rule of their own to roll back to, so the value is the
/// one the element would have without the declaration, the underlying value (Cascade 6: "revert-rule" is the value of the declaration of the rule below).
static bool is_revert(const GTEXT_CSS_Keyframe_Property * p) {
  static const char * const WORDS[] = {"revert", "revert-layer", "revert-rule"};
  size_t a = 0, b = p->text.length;
  while (a < b && (p->text.data[a] == ' ' || p->text.data[a] == '\n' || p->text.data[a] == '\t')) {
    a++;
  }
  while (b > a && (p->text.data[b - 1] == ' ' || p->text.data[b - 1] == '\n' || p->text.data[b - 1] == '\t')) {
    b--;
  }
  for (size_t i = 0; i < sizeof WORDS / sizeof *WORDS; i++) {
    const size_t n = strlen(WORDS[i]);
    if (b - a != n) {
      continue;
    }
    size_t k = 0;
    while (k < n && ((p->text.data[a + k] >= 'A' && p->text.data[a + k] <= 'Z') ? p->text.data[a + k] + 32 : p->text.data[a + k]) == WORDS[i][k]) {
      k++;
    }
    if (k == n) {
      return true;
    }
  }
  return false;
}

static GTEXT_CSS_Status frame_style(sampler * s, entry * e, size_t f, const GTEXT_CSS_Computed_Style ** out) {
  if (e->frame[f]) {
    *out = e->frame[f];
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Declaration_Block * b = NULL;
  GTEXT_CSS_Status st = gtext_css_block_clone(s->base, &b);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  const GTEXT_CSS_Keyframe * fr = &e->kf->frames[f];
  for (size_t i = 0; i < fr->property_count && st == GTEXT_CSS_OK; i++) {
    const GTEXT_CSS_Keyframe_Property * p = &fr->properties[i];
    if (is_frame_attribute(s, p->property) || is_revert(p)) {
      continue;
    }
    GTEXT_CSS_Property_Outcome outcome;
    st = add_declaration(s, b, p->name.data, p->name.length, p->text.data, p->text.length, &outcome);
  }
  if (st == GTEXT_CSS_OK) {
    st = compute_block(s, b, &e->frame[f]);
  }
  gtext_css_block_free(b);
  if (st == GTEXT_CSS_OK) {
    *out = e->frame[f];
  }
  return st;
}

/* ---- the value of a key ------------------------------------------------------------------------------------------------------------------------------ */

/// A registered custom property's value text as a typed value of its syntax; *set stays NULL when the property is unregistered or its syntax is universal.
static GTEXT_CSS_Status typed_custom(sampler * s, const key * k, const char * text, size_t n, GTEXT_CSS_Value_Set ** set) {
  *set = NULL;
  const css_ereg * reg = s->cc->nregs ? css_ctx_find_reg(s->cc, k->name, k->name_length, NULL) : NULL;
  if (!reg || reg->creg->syntax->universal) {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  po.allocator = &s->al;
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Status st = gtext_css_parse_component_values(text, n, &po, &tree, NULL);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSS_Match_Options mo = gtext_css_match_options_default();
  mo.allocator = &s->al;
  bool matched = false;
  css_syn_match m;
  memset(&m, 0, sizeof m);
  st = css_syntax_match(reg->creg->syntax, reg->creg->compiled, tree->values, tree->value_count, &mo, &matched, &m);
  if (st == GTEXT_CSS_OK && matched) {
    *set = m.set;   /* NULL for an identifier component */
  }
  else {
    gtext_css_value_set_free(m.set);
  }
  gtext_css_tree_free(tree);
  return st;
}

static GTEXT_CSS_Status val_of_text(sampler * s, const key * k, const char * text, size_t n, val * out) {
  memset(out, 0, sizeof *out);
  out->text = gtext_allocator_malloc(&s->al, n + 1);
  if (!out->text) {
    return GTEXT_CSS_E_OOM;
  }
  memcpy(out->text, text, n);
  out->text[n] = '\0';
  out->length = n;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  if (k->id) {
    GTEXT_CSS_Property_Info pi;
    GTEXT_CSS_Property_Value pv;
    GTEXT_CSS_Property_Options po;
    memset(&po, 0, sizeof po);
    po.allocator = &s->al;
    if (!gtext_css_registry_property_info(s->reg, k->id, &pi)) {
      return GTEXT_CSS_E_INVALID;
    }
    st = gtext_css_parse_property_text(s->reg, pi.name, pi.name_length, text, n, &po, &pv);
    if (st == GTEXT_CSS_OK) {
      if (pv.outcome == GTEXT_CSS_PROP_RESULT_VALID) {
        out->set = gtext_css_value_set_retain(pv.set);
      }
      gtext_css_property_value_free(&pv);
    }
  }
  else {
    st = typed_custom(s, k, text, n, &out->set);
  }
  if (st != GTEXT_CSS_OK) {
    val_free(&s->al, out);
  }
  return st;
}

/// The value of a key in a computed style: the typed computed value of a property, the value text of a custom one.
static GTEXT_CSS_Status val_of_style(sampler * s, const GTEXT_CSS_Computed_Style * style, const key * k, val * out, bool * present) {
  memset(out, 0, sizeof *out);
  *present = false;
  if (k->id) {
    const GTEXT_CSS_Value_Set * set = NULL;
    const GTEXT_CSS_Status st = gtext_css_computed_value(style, k->id, &set);
    if (st == GTEXT_CSS_OK && set && gtext_css_value_set_root(set)) {
      out->set = gtext_css_value_set_retain(set);
      *present = true;
    }
    return GTEXT_CSS_OK;   /* a property the engine cannot compute has no value to animate */
  }
  const GTEXT_CSS_Custom_Value * cv = NULL;
  if (!gtext_css_computed_custom_find(style, k->name, k->name_length, &cv) || !cv) {
    return GTEXT_CSS_OK;
  }
  char * text = NULL;
  size_t n = 0;
  GTEXT_CSS_Serialize_Options so;
  memset(&so, 0, sizeof so);
  so.allocator = &s->al;
  GTEXT_CSS_Status st = gtext_css_custom_value_text(cv, &so, &text, &n);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  st = val_of_text(s, k, text, n, out);
  gtext_css_serialize_buffer_free(&so, text);
  *present = st == GTEXT_CSS_OK;
  return st;
}

/// The element's computed `color` (of the base style): what a `currentcolor` among colors being combined stands for.
static const GTEXT_CSS_Value * current_color(const sampler * s) {
  const GTEXT_CSS_Property_Id id = gtext_css_registry_find_property(s->reg, "color", 5);
  return id ? gtext_css_computed_root(s->base_style, id) : NULL;
}

/// Where a keyword of a property means a number of the other end's type, as its animation type says: `normal` is a zero `letter-spacing` (CSS Text 4: "a zero length is written normal"), and
/// `normal` is `oblique 0deg` for `font-style` (CSS Fonts 4: "normal animates as oblique 0deg"). The keyword is replaced by that value when the other end is not a keyword.
typedef struct {
  const char * property;
  const char * keyword;
  const char * value;
} keyword_value;

static const keyword_value KEYWORD_VALUES[] = {
  {"letter-spacing", "normal", "0px"},
  {"font-style", "normal", "oblique 0deg"},
};

static GTEXT_CSS_Status prepare_pair(sampler * s, const key * k, val * a, val * b) {
  GTEXT_CSS_Property_Info pi;
  if (!k->id || !gtext_css_registry_property_info(s->reg, k->id, &pi)) {
    return GTEXT_CSS_OK;
  }
  for (size_t i = 0; i < sizeof KEYWORD_VALUES / sizeof *KEYWORD_VALUES; i++) {
    const keyword_value * kv = &KEYWORD_VALUES[i];
    if (strlen(kv->property) != pi.name_length || memcmp(kv->property, pi.name, pi.name_length) != 0) {
      continue;
    }
    for (int side = 0; side < 2; side++) {
      val * word = side == 0 ? a : b;
      const val * other = side == 0 ? b : a;
      const GTEXT_CSS_Value * wr = val_root(word);
      const GTEXT_CSS_Value * or_ = val_root(other);
      if (!wr || !or_ || wr->kind != GTEXT_CSS_VAL_KEYWORD || or_->kind == GTEXT_CSS_VAL_KEYWORD || wr->text.length != strlen(kv->keyword) ||
          memcmp(wr->text.data, kv->keyword, wr->text.length) != 0) {
        continue;
      }
      val replaced;
      const GTEXT_CSS_Status st = val_of_text(s, k, kv->value, strlen(kv->value), &replaced);
      if (st != GTEXT_CSS_OK) {
        return st;
      }
      val_free(&s->al, word);
      *word = replaced;
    }
  }
  return GTEXT_CSS_OK;
}

/// The combination of two values of a key as a value: typed values by type, texts (unregistered custom properties) discrete.
static GTEXT_CSS_Status combine(sampler * s, const key * k, css_blend_op op, const val * a, const val * b, double p, val * out) {
  const GTEXT_CSS_Value * ra = val_root(a);
  const GTEXT_CSS_Value * rb = val_root(b);
  char * text = NULL;
  size_t n = 0;
  GTEXT_CSS_Status st;
  if (ra && rb) {
    st = css_interp_text(&s->al, op, ra, rb, p, current_color(s), &text, &n, NULL);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    st = val_of_text(s, k, text, n, out);
    GTEXT_CSS_Serialize_Options so;
    memset(&so, 0, sizeof so);
    so.allocator = &s->al;
    gtext_css_serialize_buffer_free(&so, text);
    return st;
  }
  const val * pick = (op == CSS_BLEND_LERP && p < 0.5) ? a : b;
  return val_of_text(s, k, pick->text ? pick->text : "", pick->text ? pick->length : 0, out);
}

typedef struct {
  double offset;
  int frame;               ///< The frame, or -1 for a keyframe made from the underlying value.
  const GTEXT_CSS_Easing * easing;
  int comp;                ///< The keyframe's composite operation, -1 for the effect's.
} pk;

static GTEXT_CSS_Status keyframe_value(sampler * s, entry * e, const pk * k, const key * ky, const val * under, val * out) {
  memset(out, 0, sizeof *out);
  bool present = false;
  GTEXT_CSS_Status st;
  if (k->frame < 0) {
    if (under->set) {
      out->set = gtext_css_value_set_retain(under->set);
    }
    if (under->text) {
      out->text = gtext_allocator_malloc(&s->al, under->length + 1);
      if (!out->text) {
        gtext_css_value_set_free(out->set);
        out->set = NULL;
        return GTEXT_CSS_E_OOM;
      }
      memcpy(out->text, under->text, under->length + 1);
      out->length = under->length;
    }
    return GTEXT_CSS_OK;
  }
  const GTEXT_CSS_Computed_Style * fs = NULL;
  st = frame_style(s, e, (size_t)k->frame, &fs);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  st = val_of_style(s, fs, ky, out, &present);
  if (st == GTEXT_CSS_OK && !present) {
    /* the keyframe's value for the property is not a value here (an invalid one at computed-value time): the underlying value stands */
    return keyframe_value(s, e, &(pk){k->offset, -1, k->easing, -1}, ky, under, out);
  }
  return st;
}

/// "The effect value of a keyframe effect" of one property (Web Animations 1): `*has` false when the effect has no keyframe for it.
static GTEXT_CSS_Status effect_value(sampler * s, entry * e, size_t key_index, const val * under, val * out, bool * has) {
  const key * ky = &s->keys[key_index];
  const size_t nf = e->kf->frame_count;
  *has = false;
  pk * list = gtext_allocator_malloc(&s->al, (nf + 2) * sizeof *list);
  if (!list) {
    return GTEXT_CSS_E_OOM;
  }
  size_t n = 0;
  for (size_t f = 0; f < nf; f++) {
    if (e->cover[f * s->nkeys + key_index] < 0) {
      continue;
    }
    list[n].offset = e->kf->frames[f].offset;
    list[n].frame = (int)f;
    list[n].easing = e->ease[f] ? e->ease[f] : e->effect.easing;
    list[n].comp = e->comp[f];
    n++;
  }
  if (n == 0) {
    gtext_allocator_free(&s->al, list);
    return GTEXT_CSS_OK;
  }
  *has = true;
  if (list[0].offset != 0.0) {
    memmove(list + 1, list, n * sizeof *list);
    list[0] = (pk){0.0, -1, e->effect.easing, -1};
    n++;
  }
  if (list[n - 1].offset != 1.0) {
    list[n++] = (pk){1.0, -1, e->effect.easing, -1};
  }
  const double t = e->timing.progress;
  size_t a = 0;
  bool found = false;
  for (size_t i = 0; i < n; i++) {
    if (list[i].offset <= t && list[i].offset < 1.0) {
      a = i;
      found = true;
    }
  }
  if (!found) {
    a = 0;   /* "add the last keyframe whose computed keyframe offset is 0" */
  }
  const size_t b = a + 1 < n ? a + 1 : a;
  val va, vb;
  memset(&va, 0, sizeof va);
  memset(&vb, 0, sizeof vb);
  GTEXT_CSS_Status st = keyframe_value(s, e, &list[a], ky, under, &va);
  if (st == GTEXT_CSS_OK) {
    st = keyframe_value(s, e, &list[b], ky, under, &vb);
  }
  /* "If keyframe has a composite operation that is not replace, or no composite operation and the composite operation of this keyframe effect is not replace" */
  for (int side = 0; side < 2 && st == GTEXT_CSS_OK; side++) {
    const pk * k = side == 0 ? &list[a] : &list[b];
    val * v = side == 0 ? &va : &vb;
    const int op = k->comp >= 0 ? k->comp : (int)e->effect.composite;
    if (k->frame < 0 || op == (int)GTEXT_CSS_ANIM_COMPOSITE_REPLACE) {
      continue;
    }
    val combined;
    st = combine(s, ky, op == (int)GTEXT_CSS_ANIM_COMPOSITE_ADD ? CSS_BLEND_ADD : CSS_BLEND_ACCUMULATE, under, v, 0.0, &combined);
    if (st == GTEXT_CSS_OK) {
      val_free(&s->al, v);
      *v = combined;
    }
  }
  if (st == GTEXT_CSS_OK) {
    if (a == b) {
      *out = va;
      memset(&va, 0, sizeof va);
    }
    else {
      st = prepare_pair(s, ky, &va, &vb);
      const double d = (t - list[a].offset) / (list[b].offset - list[a].offset);
      const double eased = gtext_css_easing_output(list[a].easing, d, e->timing.before_flag);
      if (st == GTEXT_CSS_OK) {
        st = combine(s, ky, CSS_BLEND_LERP, &va, &vb, eased, out);
      }
    }
  }
  val_free(&s->al, &va);
  val_free(&s->al, &vb);
  gtext_allocator_free(&s->al, list);
  return st;
}

/* ---- the sampler ------------------------------------------------------------------------------------------------------------------------------------ */

void css_anim_untimed_free(const GTEXT_Allocator * al, css_anim_untimed * u) {
  if (!u) {
    return;
  }
  gtext_allocator_free(al, u->ids);
  for (size_t i = 0; i < u->nnames; i++) {
    gtext_allocator_free(al, u->names[i]);
  }
  gtext_allocator_free(al, u->names);
  gtext_allocator_free(al, u->name_lengths);
  memset(u, 0, sizeof *u);
}

static GTEXT_CSS_Status untimed_add(sampler * s, css_anim_untimed * u, const key * k) {
  if (k->id) {
    for (size_t i = 0; i < u->nids; i++) {
      if (u->ids[i] == k->id) {
        return GTEXT_CSS_OK;
      }
    }
    GTEXT_CSS_Property_Id * bigger = gtext_allocator_realloc(&s->al, u->ids, (u->nids + 1) * sizeof *bigger);
    if (!bigger) {
      return GTEXT_CSS_E_OOM;
    }
    u->ids = bigger;
    u->ids[u->nids++] = k->id;
    return GTEXT_CSS_OK;
  }
  for (size_t i = 0; i < u->nnames; i++) {
    if (u->name_lengths[i] == k->name_length && memcmp(u->names[i], k->name, k->name_length) == 0) {
      return GTEXT_CSS_OK;
    }
  }
  char ** names = gtext_allocator_realloc(&s->al, u->names, (u->nnames + 1) * sizeof *names);
  if (!names) {
    return GTEXT_CSS_E_OOM;
  }
  u->names = names;
  size_t * lens = gtext_allocator_realloc(&s->al, u->name_lengths, (u->nnames + 1) * sizeof *lens);
  if (!lens) {
    return GTEXT_CSS_E_OOM;
  }
  u->name_lengths = lens;
  char * copy = gtext_allocator_malloc(&s->al, k->name_length + 1);
  if (!copy) {
    return GTEXT_CSS_E_OOM;
  }
  memcpy(copy, k->name, k->name_length);
  copy[k->name_length] = '\0';
  u->names[u->nnames] = copy;
  u->name_lengths[u->nnames] = k->name_length;
  u->nnames++;
  return GTEXT_CSS_OK;
}

typedef struct {
  css_anim_untimed * u;
} untimed_args;

static GTEXT_CSS_Status untimed_fn(sampler * s, void * user, GTEXT_CSS_Property_Id id, const char * name, size_t n) {
  untimed_args * a = user;
  if (id && !animatable(s, id)) {
    return GTEXT_CSS_OK;
  }
  key k;
  k.id = id;
  k.name = (char *)(uintptr_t)name;
  k.name_length = n;
  return untimed_add(s, a->u, &k);
}

/// Every property a rule's keyframes set, by `fn`.
static GTEXT_CSS_Status each_kf_property(sampler * s, const GTEXT_CSS_Keyframes * kf, longhand_fn fn, void * user) {
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  for (size_t f = 0; f < kf->frame_count && st == GTEXT_CSS_OK; f++) {
    const GTEXT_CSS_Keyframe * fr = &kf->frames[f];
    for (size_t i = 0; i < fr->property_count && st == GTEXT_CSS_OK; i++) {
      const GTEXT_CSS_Keyframe_Property * p = &fr->properties[i];
      if (is_frame_attribute(s, p->property)) {
        continue;
      }
      st = each_longhand(s, p->property, p->name.data, p->name.length, fn, user);
    }
  }
  return st;
}

static void sampler_free(sampler * s) {
  for (size_t i = 0; i < s->nentries; i++) {
    entry_free(s, &s->entries[i]);
  }
  gtext_allocator_free(&s->al, s->entries);
  for (size_t i = 0; i < s->nkeys; i++) {
    gtext_allocator_free(&s->al, s->keys[i].name);
  }
  gtext_allocator_free(&s->al, s->keys);
  gtext_css_computed_free(s->base_style);
}

static unsigned asked_pseudo(const GTEXT_CSS_Selector_List * l) {
  if (!l || l->count == 0 || l->complex[0].compound_count == 0) {
    return 0;
  }
  const GTEXT_CSS_Compound * pc = &l->complex[0].compounds[0];
  return pc->simple_count ? (unsigned)pc->simples[pc->simple_count - 1].pseudo : 0;
}

/// The declaration (a winner of the cascade) for a property is important: an animation does not override it.
static bool important_in(const GTEXT_CSS_Declaration_Block * base, const key * k) {
  GTEXT_CSS_Declaration_Item it;
  if (!gtext_css_block_find(base, k->id, k->id ? NULL : k->name, k->id ? 0 : k->name_length, &it)) {
    return false;
  }
  return it.important;
}

GTEXT_CSS_Status css_anim_sample(const GTEXT_CSS_Compute_Context * cc, const GTEXT_CSS_Style_Options * options, GTEXT_CSS_Element_Handle element,
    const GTEXT_CSS_Computed_Style * parent, const GTEXT_CSS_Declaration_Block * base, GTEXT_CSS_Declaration_Block ** animated, css_anim_untimed * untimed,
    GTEXT_CSS_Compute_Result * failure) {
  *animated = NULL;
  const GTEXT_CSS_Style_Animation * an = options->animation;
  sampler s;
  memset(&s, 0, sizeof s);
  s.cc = cc;
  s.reg = gtext_css_compute_context_registry(cc);
  s.al = cc->alloc;
  s.options = options;
  s.element = element;
  s.parent = parent;
  s.base = base;
  s.asked_pseudo = asked_pseudo(options->pseudo);
  s.failure = failure;
  const GTEXT_CSS_Property_Id name_id = gtext_css_registry_find_property(s.reg, "animation-name", 14);
  if (!an || !an->keyframes || !name_id || !gtext_css_block_find(base, name_id, NULL, 0, NULL)) {
    return GTEXT_CSS_OK;   /* no animation is named by the cascade: nothing to sample, nothing computed */
  }
  s.id_timing = gtext_css_registry_find_property(s.reg, "animation-timing-function", 25);
  s.id_composition = gtext_css_registry_find_property(s.reg, "animation-composition", 21);
  GTEXT_CSS_Anim_List * list = NULL;
  GTEXT_CSS_Declaration_Block * out_block = NULL;
  GTEXT_CSS_Status st = compute_block(&s, base, &s.base_style);
  if (st == GTEXT_CSS_OK) {
    st = gtext_css_anim_list_from_style(s.base_style, &s.al, &list);
  }
  const size_t neffects = st == GTEXT_CSS_OK ? gtext_css_anim_list_count(list) : 0;
  if (st == GTEXT_CSS_OK && neffects) {
    s.entries = gtext_allocator_calloc(&s.al, neffects, sizeof *s.entries);
    if (!s.entries) {
      st = GTEXT_CSS_E_OOM;
    }
  }
  /* the effects with keyframes whose time is known; the others are only reported */
  for (size_t i = 0; i < neffects && st == GTEXT_CSS_OK; i++) {
    GTEXT_CSS_Anim_Effect ef;
    gtext_css_anim_list_at(list, i, &ef);
    const GTEXT_CSS_Keyframes * kf = gtext_css_atrule_set_keyframes(an->keyframes, ef.name.data, ef.name.length);
    if (!kf || !ef.timeline_auto) {
      continue;
    }
    double ms = 0;
    bool known = false;
    if (an->current_time) {
      st = an->current_time(an->user, element, options->pseudo, &ef, &ms, &known);
      if (st != GTEXT_CSS_OK) {
        break;
      }
    }
    if (!known) {
      if (untimed) {
        untimed_args ua = {untimed};
        st = each_kf_property(&s, kf, untimed_fn, &ua);
      }
      continue;
    }
    entry * e = &s.entries[s.nentries];
    e->effect = ef;
    e->kf = kf;
    gtext_css_anim_timing(&ef, ms, &e->timing);
    if (!e->timing.in_effect) {
      continue;
    }
    s.nentries++;
  }
  /* the properties of all the effects, in the order they are first met */
  for (size_t i = 0; i < s.nentries && st == GTEXT_CSS_OK; i++) {
    st = each_kf_property(&s, s.entries[i].kf, collect_fn, NULL);
  }
  for (size_t i = 0; i < s.nentries && st == GTEXT_CSS_OK; i++) {
    entry * e = &s.entries[i];
    const size_t nf = e->kf->frame_count;
    e->cover = gtext_allocator_malloc(&s.al, (nf * s.nkeys + 1) * sizeof *e->cover);
    e->ease = gtext_allocator_calloc(&s.al, nf + 1, sizeof *e->ease);
    e->comp = gtext_allocator_malloc(&s.al, (nf + 1) * sizeof *e->comp);
    e->frame = gtext_allocator_calloc(&s.al, nf + 1, sizeof *e->frame);
    if (!e->cover || !e->ease || !e->comp || !e->frame) {
      st = GTEXT_CSS_E_OOM;
      break;
    }
    for (size_t c = 0; c < nf * s.nkeys; c++) {
      e->cover[c] = -1;
    }
    for (size_t f = 0; f < nf && st == GTEXT_CSS_OK; f++) {
      const GTEXT_CSS_Keyframe * fr = &e->kf->frames[f];
      for (size_t p = 0; p < fr->property_count && st == GTEXT_CSS_OK; p++) {
        if (is_frame_attribute(&s, fr->properties[p].property)) {
          continue;
        }
        cover_args ca = {e, f, (int)p};
        st = each_longhand(&s, fr->properties[p].property, fr->properties[p].name.data, fr->properties[p].name.length, cover_fn, &ca);
      }
      if (st == GTEXT_CSS_OK) {
        st = frame_attributes(&s, e, f);
      }
    }
  }
  /* the stack of each property, in the order of the names: the last effect that has a value for it wins, composited over the ones before */
  if (st == GTEXT_CSS_OK && s.nkeys) {
    st = gtext_css_block_clone(base, &out_block);
  }
  size_t applied = 0;
  for (size_t k = 0; k < s.nkeys && st == GTEXT_CSS_OK; k++) {
    const key * ky = &s.keys[k];
    if (important_in(base, ky)) {
      continue;
    }
    val under, cur;
    bool present = false;
    st = val_of_style(&s, s.base_style, ky, &under, &present);
    if (st != GTEXT_CSS_OK) {
      break;
    }
    if (!present && ky->id) {
      val_free(&s.al, &under);
      continue;
    }
    bool any = false;
    memset(&cur, 0, sizeof cur);
    for (size_t i = 0; i < s.nentries && st == GTEXT_CSS_OK; i++) {
      val next;
      bool has = false;
      st = effect_value(&s, &s.entries[i], k, any ? &cur : &under, &next, &has);
      if (st == GTEXT_CSS_OK && has) {
        val_free(&s.al, &cur);
        cur = next;
        any = true;
      }
    }
    if (st == GTEXT_CSS_OK && any && cur.text && (ky->id || cur.length)) {
      GTEXT_CSS_Property_Outcome outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
      const char * nm = ky->name;
      size_t nl = ky->name_length;
      GTEXT_CSS_Property_Info pi;
      if (ky->id) {
        gtext_css_registry_property_info(s.reg, ky->id, &pi);
        nm = pi.name;
        nl = pi.name_length;
      }
      st = gtext_css_block_remove(out_block, ky->id, ky->id ? NULL : ky->name, ky->id ? 0 : ky->name_length, NULL);
      if (st == GTEXT_CSS_OK) {
        st = add_declaration(&s, out_block, nm, nl, cur.text, cur.length, &outcome);
      }
      if (st == GTEXT_CSS_OK && outcome != GTEXT_CSS_PROP_RESULT_VALID && outcome != GTEXT_CSS_PROP_RESULT_WIDE && ky->id) {
        /* the number is outside the range of its property: a calculation clamps it as the property says */
        size_t m = cur.length + 8;
        char * wrapped = gtext_allocator_malloc(&s.al, m);
        if (!wrapped) {
          st = GTEXT_CSS_E_OOM;
        }
        else {
          memcpy(wrapped, "calc(", 5);
          memcpy(wrapped + 5, cur.text, cur.length);
          memcpy(wrapped + 5 + cur.length, ")", 2);
          st = add_declaration(&s, out_block, nm, nl, wrapped, cur.length + 6, &outcome);
          gtext_allocator_free(&s.al, wrapped);
        }
      }
      applied++;
    }
    val_free(&s.al, &under);
    val_free(&s.al, &cur);
  }
  if (st == GTEXT_CSS_OK && applied) {
    *animated = out_block;
    out_block = NULL;
  }
  gtext_css_block_free(out_block);
  gtext_css_anim_list_free(list);
  sampler_free(&s);
  return st;
}
