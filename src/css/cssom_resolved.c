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
 * Resolved values (CSS Object Model, section 9): the block of a computed declaration.
 *
 * "The resolved value for a given longhand property can be determined as follows": the computed value, except for the properties that tools/css/resolved_value.tsv lists, each with
 * the sentence of the served text that says so. The categories are cssom_computed_internal.h's. A used value is a fact about a laid out box; the layout interface of the document
 * (css_cssom_core.h) answers it, and without an answer the computed value stands in and the origin says `ABSENT_LAYOUT`. The colors need no layout: the used value of a color is the
 * computed value with `currentcolor` replaced by the element's `color`, and is made here.
 *
 * The declarations are the supported longhands in lexicographic order (served step 5), then the custom properties whose computed value is not the guaranteed-invalid value.
 */

#include <stdlib.h>
#include <string.h>

#include "css_color_compute.h"
#include "css_compute_internal.h"
#include "css_internal.h"
#include "cssom_computed_internal.h"
#include "cssom_resolved_tables.h"

css_resolved_category css_resolved_lookup(const char * name, size_t length) {
  size_t lo = 0, hi = CSS_RESOLVED_ROW_COUNT;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const css_resolved_row * r = &css_resolved_rows[mid];
    const size_t n = length < r->length ? length : r->length;
    int c = memcmp(name, r->name, n);
    if (c == 0) {
      c = length < r->length ? -1 : length > r->length ? 1 : 0;
    }
    if (c == 0) {
      return (css_resolved_category)r->category;
    }
    if (c < 0) {
      hi = mid;
    }
    else {
      lo = mid + 1;
    }
  }
  return CSS_RESOLVED_COMPUTED;
}

size_t css_resolved_row_count(void) {
  return CSS_RESOLVED_ROW_COUNT;
}

/* ---- helpers -------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  GTEXT_CSS_Property_Id id;
  const char * name;
  size_t length;
  const GTEXT_CSS_Value_Set * set;
} entry;

/// The order of the declarations: lexicographic, the names of vendor-prefixed properties (they begin with `-`) after the others, as both browsers list them (the probe of the stage:
/// the served text says "lexicographical order" and does not say where a hyphen sorts).
static int entry_cmp(const void * a, const void * b) {
  const entry * x = a;
  const entry * y = b;
  const bool xd = x->length && x->name[0] == '-', yd = y->length && y->name[0] == '-';
  if (xd != yd) {
    return xd ? 1 : -1;
  }
  const size_t n = x->length < y->length ? x->length : y->length;
  const int c = memcmp(x->name, y->name, n);
  if (c) {
    return c;
  }
  return x->length < y->length ? -1 : x->length > y->length ? 1 : 0;
}

/// The computed value of a property written as text; NULL on failure. Free with gtext_css_serialize_buffer_free().
static char * computed_text(const GTEXT_CSS_Computed_Style * style, GTEXT_CSS_Property_Id id, size_t * length, GTEXT_CSS_Status * status) {
  const GTEXT_CSS_Value * root = gtext_css_computed_root(style, id);
  *status = GTEXT_CSS_OK;
  if (!root) {
    return NULL;
  }
  char * out = NULL;
  *status = gtext_css_serialize_value_buffer(root, NULL, &out, length);
  return *status == GTEXT_CSS_OK ? out : NULL;
}

static bool text_is(const char * text, size_t length, const char * word) {
  const size_t n = strlen(word);
  return length == n && memcmp(text, word, n) == 0;
}

static bool ident_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || (unsigned char)c >= 0x80;
}

/// Where `currentcolor` (ASCII case-insensitive, a whole identifier) starts in the text at or after `from`; SIZE_MAX for none.
static size_t find_currentcolor(const char * s, size_t n, size_t from) {
  static const char word[] = "currentcolor";
  const size_t w = sizeof word - 1;
  for (size_t i = from; i + w <= n; i++) {
    if (i > 0 && ident_char(s[i - 1])) {
      continue;
    }
    size_t k = 0;
    while (k < w && (char)(s[i + k] | 0x20) == word[k]) {
      k++;
    }
    if (k == w && (i + w == n || !ident_char(s[i + w]))) {
      return i;
    }
  }
  return SIZE_MAX;
}

typedef struct {
  const css_resolve_in * in;
  const GTEXT_CSS_Computed_Style * style;
  GTEXT_CSS_Property_Id color_id;
  bool display_hidden;     ///< `display` is `none` or `contents`.
  bool positioned;         ///< `position` is not `static`.
  const GTEXT_CSS_Value_Set * color_set;   ///< The computed value of `color`.
  bool have_current;               ///< `cur` and `cur_text` are the used value of the element's `color`.
  bool current_tried;
  css_cc_value cur;
  char cur_text[CSS_CC_TEXT_MAX + 1];
  size_t cur_length;
} rctx;

static void rctx_free(rctx * r) {
  (void)r;
}

static GTEXT_CSS_Status rctx_init(rctx * r, const css_resolve_in * in, const GTEXT_CSS_Computed_Style * style) {
  memset(r, 0, sizeof *r);
  r->in = in;
  r->style = style;
  const GTEXT_CSS_Registry * reg = in->core->registry;
  r->color_id = gtext_css_registry_find_property(reg, "color", 5);
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  const GTEXT_CSS_Property_Id display = gtext_css_registry_find_property(reg, "display", 7);
  if (display) {
    size_t n = 0;
    char * t = computed_text(style, display, &n, &st);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    r->display_hidden = t && (text_is(t, n, "none") || text_is(t, n, "contents"));
    gtext_css_serialize_buffer_free(NULL, t);
  }
  const GTEXT_CSS_Property_Id position = gtext_css_registry_find_property(reg, "position", 8);
  if (position) {
    size_t n = 0;
    char * t = computed_text(style, position, &n, &st);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    r->positioned = t && !text_is(t, n, "static");
    gtext_css_serialize_buffer_free(NULL, t);
  }
  if (r->color_id) {
    if (gtext_css_computed_value(style, r->color_id, &r->color_set) != GTEXT_CSS_OK) {
      r->color_set = NULL;
    }
  }
  return GTEXT_CSS_OK;
}

/// Read `text` as a declaration of the property and compute it, so that a color-mix() or a relative color that held a color now computes with it in its place. `*out` is the computed
/// set (owned), or NULL when the text is not a value of the property (the computed value then stands).
static GTEXT_CSS_Status recompute_text(rctx * r, const GTEXT_CSS_Property_Info * info, const char * text, size_t length, GTEXT_CSS_Value_Set ** out) {
  css_core * core = r->in->core;
  *out = NULL;
  GTEXT_CSS_Property_Value pv;
  memset(&pv, 0, sizeof pv);
  GTEXT_CSS_Status st = gtext_css_parse_property_text(core->registry, info->name, info->name_length, text, length, NULL, &pv);
  if (st != GTEXT_CSS_OK || pv.outcome != GTEXT_CSS_PROP_RESULT_VALID || !pv.set) {
    gtext_css_property_value_free(&pv);
    return st;
  }
  GTEXT_CSS_Declaration_Block * tmp = NULL;
  GTEXT_CSS_Declaration_Options bo = gtext_css_declaration_options_default();
  bo.allocator = &core->alloc;
  st = gtext_css_block_new(core->registry, &bo, &tmp);
  if (st == GTEXT_CSS_OK) {
    st = gtext_css_block_add(tmp, &pv, false, NULL);
  }
  gtext_css_property_value_free(&pv);
  GTEXT_CSS_Computed_Style * made = NULL;
  if (st == GTEXT_CSS_OK) {
    GTEXT_CSS_Compute_Result res;
    memset(&res, 0, sizeof res);
    st = gtext_css_compute_style(r->in->cs->ctx, tmp, NULL, r->in->element, &made, &res);
    if (st != GTEXT_CSS_OK) {
      css_computed_note_failure(core, st, res.name, css_name_length(res.name, sizeof res.name), res.property);
    }
  }
  gtext_css_block_free(tmp);
  if (st == GTEXT_CSS_OK) {
    const GTEXT_CSS_Value_Set * set = NULL;
    if (gtext_css_computed_value(made, info->id, &set) == GTEXT_CSS_OK && set) {
      *out = gtext_css_value_set_retain(set);
    }
  }
  gtext_css_computed_free(made);
  return st;
}

/// The sRGB spelling of a keyword color, as the used value is written ("The computed and used value is the corresponding sRGB color"): `rgb(255, 0, 0)` for `red`,
/// `rgba(0, 0, 0, 0)` for `transparent`. NULL for anything else (a system color stays what the environment could not resolve).
static const char * srgb_of_keyword(const char * word, size_t n, char buf[32]) {
  if (n == 11 && memcmp(word, "transparent", 11) == 0) {
    return "rgba(0, 0, 0, 0)";
  }
  if (n < 3 || n > 20) {
    return NULL;
  }
  const size_t count = gtext_css_color_named_count();
  for (size_t i = 0; i < count; i++) {
    const char * name = NULL;
    uint32_t rgb = 0;
    if (gtext_css_color_named_at(i, &name, &rgb) && strlen(name) == n && memcmp(name, word, n) == 0) {
      char * p = buf;
      const unsigned v[3] = {(rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff};
      memcpy(p, "rgb(", 4);
      p += 4;
      for (int k = 0; k < 3; k++) {
        char digits[4];
        int d = 0;
        unsigned x = v[k];
        do {
          digits[d++] = (char)('0' + x % 10);
          x /= 10;
        } while (x);
        while (d) {
          *p++ = digits[--d];
        }
        if (k < 2) {
          *p++ = ',';
          *p++ = ' ';
        }
      }
      *p++ = ')';
      *p = '\0';
      return buf;
    }
  }
  return NULL;
}

/// The used value of the element's `color`, which `currentcolor` is in every other property (Color 4, 15.5: "In any other property, its used value is the used value of the color
/// property on the same element"): its text, and the color for the mixes that hold `currentcolor`.
static void current_color(rctx * r) {
  if (r->current_tried) {
    return;
  }
  r->current_tried = true;
  const GTEXT_CSS_Value * root = r->color_set ? gtext_css_value_set_root(r->color_set) : NULL;
  if (!root || root->kind != GTEXT_CSS_VAL_COLOR || !root->color || (root->flags & GTEXT_CSS_VFLAG_COLOR_UNRESOLVED)) {
    return;
  }
  css_color_ctx cx;
  memset(&cx, 0, sizeof cx);
  cx.alloc = &r->in->core->alloc;
  cx.scheme = GTEXT_CSS_SCHEME_LIGHT;
  bool done = false;
  if (css_color_compute(&cx, root->color, &r->cur, &done) != GTEXT_CSS_OK || !done) {
    return;
  }
  char kw[32];
  const char * word = NULL;
  if (root->color->kind == GTEXT_CSS_COLOR_KEYWORD) {
    word = srgb_of_keyword(root->color->name.data, root->color->name.length, kw);
  }
  if (word) {
    r->cur_length = strlen(word);
    memcpy(r->cur_text, word, r->cur_length + 1);
  }
  else if (root->text.data && root->text.length <= CSS_CC_TEXT_MAX) {
    r->cur_length = root->text.length;
    memcpy(r->cur_text, root->text.data, r->cur_length);
    r->cur_text[r->cur_length] = '\0';
  }
  else {
    return;
  }
  r->have_current = true;
}

/// Whether a color node is one whose used value is not what the computed value is written as.
static bool color_needs_use(const rctx * r, const GTEXT_CSS_Value * v) {
  const GTEXT_CSS_Color * c = v->color;
  if (v->kind != GTEXT_CSS_VAL_COLOR || !c) {
    return false;
  }
  if (c->kind == GTEXT_CSS_COLOR_KEYWORD) {
    return c->keyword == GTEXT_CSS_COLOR_KW_NAMED || c->keyword == GTEXT_CSS_COLOR_KW_TRANSPARENT || c->keyword == GTEXT_CSS_COLOR_KW_CURRENTCOLOR;
  }
  (void)r;
  return v->text.data && find_currentcolor(v->text.data, v->text.length, 0) != SIZE_MAX;
}

typedef struct {
  const GTEXT_CSS_Value * src;
  size_t i;
  GTEXT_CSS_Value * kids;
  bool changed;
} uc_frame;

static bool has_kids(const GTEXT_CSS_Value * v) {
  return v->children && v->child_count > 0;
}

/// A color node as its used value: a new node with the text of the used color.
static GTEXT_CSS_Status use_color_leaf(rctx * r, css_vb * vb, const GTEXT_CSS_Value * src, GTEXT_CSS_Value * out, bool * changed) {
  const GTEXT_CSS_Color * c = src->color;
  char kw[32];
  char buf[CSS_CC_TEXT_MAX + 1];
  const char * rep = NULL;
  size_t rl = 0;
  if (c->kind == GTEXT_CSS_COLOR_KEYWORD) {
    if (c->keyword == GTEXT_CSS_COLOR_KW_CURRENTCOLOR) {
      current_color(r);
      if (r->have_current) {
        rep = r->cur_text;
        rl = r->cur_length;
      }
    }
    else {
      rep = srgb_of_keyword(c->name.data, c->name.length, kw);
      rl = rep ? strlen(rep) : 0;
    }
  }
  else {
    /* a mix, a relative color or a light-dark() that holds currentcolor: computed again with the element's color in its place */
    current_color(r);
    if (r->have_current) {
      css_color_ctx cx;
      memset(&cx, 0, sizeof cx);
      cx.alloc = &r->in->core->alloc;
      cx.scheme = GTEXT_CSS_SCHEME_LIGHT;
      cx.has_current = true;
      cx.current = r->cur;
      cx.current_anywhere = true;
      css_cc_value v;
      bool done = false;
      const GTEXT_CSS_Status st = css_color_compute(&cx, c, &v, &done);
      if (st != GTEXT_CSS_OK) {
        return st;
      }
      if (done) {
        rl = css_color_value_text(&v, buf, NULL);
        rep = buf;
      }
    }
  }
  if (!rep) {
    return GTEXT_CSS_OK;
  }
  const char * text = css_vb_str(vb, rep, rl);
  if (!text) {
    return vb->status;
  }
  *out = *src;
  out->text.data = text;
  out->text.length = rl;
  out->flags &= ~(uint32_t)GTEXT_CSS_VFLAG_COLOR_UNRESOLVED;
  *changed = true;
  return GTEXT_CSS_OK;
}

/// The used value of the colors in a computed value (Color 4, 15: "The resolved value of a <color> is its used value"; 16.2: "For the computed and used value, the corresponding
/// sRGB value is used"): a keyword (`red`, `transparent`) is its sRGB color, `currentcolor` is the element's color, and a mix or a relative color that held `currentcolor` is
/// computed with it. The tree is copied without recursion, as the computed layer copies one: what does not change is shared with the set it came from. `*out` is the new set
/// (owned), or NULL when no color of the value changes. `auto_is_current` makes the keyword `auto` the current color.
static GTEXT_CSS_Status use_colors(rctx * r, const GTEXT_CSS_Value_Set * set, bool auto_is_current, GTEXT_CSS_Value_Set ** out) {
  css_core * core = r->in->core;
  *out = NULL;
  bool substituted = false;
  if (auto_is_current) {
    const GTEXT_CSS_Value * root = gtext_css_value_set_root(set);
    if (root && root->kind == GTEXT_CSS_VAL_KEYWORD && root->text.length == 4 && memcmp(root->text.data, "auto", 4) == 0 && r->color_set) {
      set = r->color_set;   /* the used value of auto is the current color: both browsers answer so */
      substituted = true;
    }
  }
  const GTEXT_CSS_Value * root = gtext_css_value_set_root(set);
  if (!root) {
    return GTEXT_CSS_OK;
  }
  /* is there anything to change? */
  GTEXT_Allocator alloc = core->alloc;
  size_t cap = 16, top = 0;
  const GTEXT_CSS_Value ** stack = gtext_allocator_malloc(&alloc, cap * sizeof *stack);
  if (!stack) {
    return GTEXT_CSS_E_OOM;
  }
  bool need = false;
  stack[top++] = root;
  while (top && !need) {
    const GTEXT_CSS_Value * v = stack[--top];
    if (color_needs_use(r, v)) {
      need = true;
      break;
    }
    if (has_kids(v)) {
      if (top + v->child_count > cap) {
        size_t grown = cap;
        while (grown < top + v->child_count) {
          grown *= 2;
        }
        const GTEXT_CSS_Value ** bigger = gtext_allocator_realloc(&alloc, (void *)stack, grown * sizeof *stack);
        if (!bigger) {
          gtext_allocator_free(&alloc, (void *)stack);
          return GTEXT_CSS_E_OOM;
        }
        stack = bigger;
        cap = grown;
      }
      for (size_t k = 0; k < v->child_count; k++) {
        stack[top++] = &v->children[k];
      }
    }
  }
  gtext_allocator_free(&alloc, (void *)stack);
  if (!need) {
    if (substituted) {
      *out = gtext_css_value_set_retain(set);   /* the element's color, which needs nothing, is the used value of `auto` */
    }
    return GTEXT_CSS_OK;
  }
  css_vb vb;
  if (!css_vb_begin(&vb, &alloc, 0)) {
    return GTEXT_CSS_E_OOM;
  }
  uc_frame * frames = NULL;
  size_t nframes = 0, capframes = 0;
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  GTEXT_CSS_Value result;
  bool changed = false;
  memset(&result, 0, sizeof result);
  if (!has_kids(root)) {
    status = use_color_leaf(r, &vb, root, &result, &changed);
  }
  else {
    capframes = 16;
    frames = gtext_allocator_malloc(&alloc, capframes * sizeof *frames);
    if (!frames) {
      status = GTEXT_CSS_E_OOM;
    }
    else {
      frames[0] = (uc_frame){root, 0, NULL, false};
      nframes = 1;
    }
    while (status == GTEXT_CSS_OK && nframes) {
      uc_frame * f = &frames[nframes - 1];
      if (f->i < f->src->child_count) {
        const GTEXT_CSS_Value * kid = &f->src->children[f->i];
        if (has_kids(kid)) {
          if (nframes == capframes) {
            if (capframes > SIZE_MAX / 2 / sizeof *frames) {
              status = GTEXT_CSS_E_OOM;
              break;
            }
            uc_frame * grown = gtext_allocator_realloc(&alloc, frames, capframes * 2 * sizeof *frames);
            if (!grown) {
              status = GTEXT_CSS_E_OOM;
              break;
            }
            frames = grown;
            capframes *= 2;
            f = &frames[nframes - 1];
          }
          frames[nframes++] = (uc_frame){kid, 0, NULL, false};
          continue;
        }
        GTEXT_CSS_Value kres;
        bool kchanged = false;
        memset(&kres, 0, sizeof kres);
        if (color_needs_use(r, kid)) {
          status = use_color_leaf(r, &vb, kid, &kres, &kchanged);
          if (status != GTEXT_CSS_OK) {
            break;
          }
        }
        if (kchanged) {
          if (!f->kids) {
            f->kids = css_vb_values(&vb, f->src->child_count);
            if (!f->kids) {
              status = vb.status;
              break;
            }
            memcpy(f->kids, f->src->children, f->src->child_count * sizeof *f->kids);
          }
          f->kids[f->i] = kres;
          f->changed = true;
        }
        f->i++;
        continue;
      }
      GTEXT_CSS_Value done = *f->src;
      const bool node_changed = f->changed;
      if (node_changed) {
        done.children = f->kids;
      }
      nframes--;
      if (nframes == 0) {
        result = done;
        changed = node_changed;
        break;
      }
      uc_frame * parent = &frames[nframes - 1];
      if (node_changed) {
        if (!parent->kids) {
          parent->kids = css_vb_values(&vb, parent->src->child_count);
          if (!parent->kids) {
            status = vb.status;
            break;
          }
          memcpy(parent->kids, parent->src->children, parent->src->child_count * sizeof *parent->kids);
        }
        parent->kids[parent->i] = done;
        parent->changed = true;
      }
      parent->i++;
    }
  }
  gtext_allocator_free(&alloc, frames);
  if (status == GTEXT_CSS_OK && vb.status != GTEXT_CSS_OK) {
    status = vb.status;
  }
  if (status != GTEXT_CSS_OK || !changed) {
    css_vb_abort(&vb);
    return status;
  }
  GTEXT_CSS_Value * rv = css_vb_value(&vb);
  if (!rv || !css_vb_dep(&vb, set)) {
    status = vb.status ? vb.status : GTEXT_CSS_E_OOM;
    css_vb_abort(&vb);
    return status;
  }
  *rv = result;
  GTEXT_CSS_Value_Set * made = css_vb_finish(&vb, rv);
  if (!made) {
    return GTEXT_CSS_E_OOM;
  }
  *out = made;
  return GTEXT_CSS_OK;
}

/// Whether a computed length is the length zero (a calculation that is not a number yet is not).
static bool zero_length(const GTEXT_CSS_Value * v) {
  if (v->flags & GTEXT_CSS_VFLAG_MATH) {
    return false;
  }
  return v->kind == GTEXT_CSS_VAL_ZERO || (v->kind == GTEXT_CSS_VAL_DIMENSION && v->number == 0.0);
}

/// Whether an item of a `text-shadow` list is a color, four lengths and maybe `inset` whose fourth length (the spread) is zero.
static bool shadow_has_zero_spread(const GTEXT_CSS_Value * item) {
  return item->kind == GTEXT_CSS_VAL_LIST && item->child_count >= 5 && item->children[0].kind == GTEXT_CSS_VAL_COLOR && zero_length(&item->children[4]);
}

/// The item without its spread; `*out` is a node of the set under construction.
static void shadow_without_spread(css_vb * vb, const GTEXT_CSS_Value * item, GTEXT_CSS_Value * out) {
  GTEXT_CSS_Value * kids = css_vb_values(vb, item->child_count - 1);
  if (!kids) {
    return;
  }
  memcpy(kids, item->children, 4 * sizeof *kids);
  memcpy(kids + 4, item->children + 5, (item->child_count - 5) * sizeof *kids);
  *out = *item;
  out->children = kids;
  out->child_count = item->child_count - 1;
}

/// The shortest serialization of the computed value (CSSOM, "serialize a CSS value": "If component values can be omitted or replaced with a shorter representation without changing the
/// meaning of the value, omit/replace them."). `text-shadow` has four lengths as a computed value (Text Decoration 4: "four absolute lengths plus a computed color") and a spread of zero
/// is what none written means, so it is left out; `text-emphasis-style` is a pair, and "If a shape keyword is specified but neither of filled nor open is specified, filled is assumed",
/// so `filled` is left out. `*out` is the new set (owned), or NULL when the value is already the shortest.
static GTEXT_CSS_Status shortest_form(rctx * r, const GTEXT_CSS_Property_Info * info, const GTEXT_CSS_Value_Set * set, GTEXT_CSS_Value_Set ** out) {
  *out = NULL;
  const GTEXT_CSS_Value * root = gtext_css_value_set_root(set);
  if (!root || root->kind != GTEXT_CSS_VAL_LIST) {
    return GTEXT_CSS_OK;
  }
  const bool emphasis = info->name_length == 19 && memcmp(info->name, "text-emphasis-style", 19) == 0;
  bool need = false;
  if (emphasis) {
    need = root->child_count == 2 && root->children[0].kind == GTEXT_CSS_VAL_KEYWORD && root->children[0].text.length == 6 && memcmp(root->children[0].text.data, "filled", 6) == 0;
  }
  else if (root->separator == GTEXT_CSS_SEP_COMMA) {
    for (size_t i = 0; i < root->child_count && !need; i++) {
      need = shadow_has_zero_spread(&root->children[i]);
    }
  }
  else {
    need = shadow_has_zero_spread(root);
  }
  if (!need) {
    return GTEXT_CSS_OK;
  }
  css_vb vb;
  if (!css_vb_begin(&vb, &r->in->core->alloc, 0)) {
    return GTEXT_CSS_E_OOM;
  }
  GTEXT_CSS_Value * rv = css_vb_value(&vb);
  if (!rv) {
    const GTEXT_CSS_Status status = vb.status ? vb.status : GTEXT_CSS_E_OOM;
    css_vb_abort(&vb);
    return status;
  }
  if (emphasis) {
    *rv = root->children[1];
  }
  else if (root->separator == GTEXT_CSS_SEP_COMMA) {
    GTEXT_CSS_Value * kids = css_vb_values(&vb, root->child_count);
    for (size_t i = 0; kids && i < root->child_count; i++) {
      if (shadow_has_zero_spread(&root->children[i])) {
        shadow_without_spread(&vb, &root->children[i], &kids[i]);
      }
      else {
        kids[i] = root->children[i];
      }
    }
    *rv = *root;
    rv->children = kids;
  }
  else {
    shadow_without_spread(&vb, root, rv);
  }
  if (vb.status != GTEXT_CSS_OK || !css_vb_dep(&vb, set)) {
    const GTEXT_CSS_Status status = vb.status ? vb.status : GTEXT_CSS_E_OOM;
    css_vb_abort(&vb);
    return status;
  }
  GTEXT_CSS_Value_Set * made = css_vb_finish(&vb, rv);
  if (!made) {
    return GTEXT_CSS_E_OOM;
  }
  *out = made;
  return GTEXT_CSS_OK;
}

/// Ask the layout for the used value. `*out` is the set (owned) when the layout gave one; `*asked` says whether the callback exists.
static GTEXT_CSS_Status ask_layout(rctx * r, const GTEXT_CSS_Property_Info * info, bool * asked, GTEXT_CSS_Value_Set ** out) {
  css_core * core = r->in->core;
  *out = NULL;
  *asked = core->has_layout && core->layout.used_value;
  if (!*asked) {
    return GTEXT_CSS_OK;
  }
  bool applies = false;
  GTEXT_CSS_Value_Set * set = NULL;
  core->counters.computed_layout_calls++;
  const GTEXT_CSS_Status st = core->layout.used_value(core->layout.user, r->in->element, r->in->pseudo, info->id, &applies, &set);
  if (st != GTEXT_CSS_OK) {
    css_computed_note_failure(core, st, "used_value", 10, info->id);
    return st;
  }
  if (applies && set) {
    *out = set;
  }
  else {
    gtext_css_value_set_free(set);
  }
  return GTEXT_CSS_OK;
}

/// Whether the property's computed value is exactly this word.
static bool computed_is(const rctx * r, const char * property, size_t property_length, const char * word, GTEXT_CSS_Status * st) {
  const GTEXT_CSS_Property_Id id = gtext_css_registry_find_property(r->in->core->registry, property, property_length);
  *st = GTEXT_CSS_OK;
  if (!id) {
    return false;
  }
  size_t n = 0;
  char * t = computed_text(r->style, id, &n, st);
  const bool yes = t && text_is(t, n, word);
  gtext_css_serialize_buffer_free(NULL, t);
  return yes;
}

/// "the resolved value of auto for animation-duration is 0s" when `animation-timeline` is `auto`.
static GTEXT_CSS_Status animation_duration(rctx * r, const GTEXT_CSS_Property_Info * info, GTEXT_CSS_Value_Set ** out) {
  css_core * core = r->in->core;
  *out = NULL;
  GTEXT_CSS_Status st;
  if (!computed_is(r, "animation-timeline", 18, "auto", &st) || st != GTEXT_CSS_OK) {
    return st;
  }
  size_t n = 0;
  char * text = computed_text(r->style, info->id, &n, &st);
  if (!text) {
    return st;
  }
  char * made = gtext_allocator_malloc(&core->alloc, n * 2 + 8);
  if (!made) {
    gtext_css_serialize_buffer_free(NULL, text);
    return GTEXT_CSS_E_OOM;
  }
  size_t w = 0, i = 0;
  bool changed = false;
  while (i <= n) {
    size_t j = i;
    while (j < n && text[j] != ',') {
      j++;
    }
    size_t a = i, b = j;
    while (a < b && text[a] == ' ') {
      a++;
    }
    while (b > a && text[b - 1] == ' ') {
      b--;
    }
    if (b - a == 4 && memcmp(text + a, "auto", 4) == 0) {
      memcpy(made + w, text + i, a - i);
      w += a - i;
      memcpy(made + w, "0s", 2);
      w += 2;
      memcpy(made + w, text + b, j - b);
      w += j - b;
      changed = true;
    }
    else {
      memcpy(made + w, text + i, j - i);
      w += j - i;
    }
    if (j < n) {
      made[w++] = ',';
    }
    i = j + 1;
  }
  gtext_css_serialize_buffer_free(NULL, text);
  made[w] = '\0';
  st = changed ? recompute_text(r, info, made, w, out) : GTEXT_CSS_OK;
  gtext_allocator_free(&core->alloc, made);
  return st;
}

/// Whether the display value (a computed one, `flex`, `inline-flex`, `grid`, `inline-grid`) makes the box a flex or grid container.
static bool is_flex_or_grid(const char * t, size_t n) {
  static const char * const words[] = {"flex", "inline-flex", "grid", "inline-grid", "block flex", "inline flex", "block grid", "inline grid"};
  for (size_t i = 0; i < sizeof words / sizeof *words; i++) {
    if (text_is(t, n, words[i])) {
      return true;
    }
  }
  return false;
}

/// "For backwards-compatibility, the resolved value of this keyword is zero for boxes of all [CSS2] display types ... when aspect-ratio is auto. It also resolves to zero when no box is
/// generated." The served text names the display types of the box; both browsers answer 0px for a flex or grid container that is not itself an item, and `auto` for a flex or grid
/// item, so the automatic minimum size stays `auto` for the items of a flex or grid container (a box in the flow of one) and is zero for every other box; and `auto` when `aspect-ratio`
/// is not (the browsers differ there: Firefox follows the sentence).
static GTEXT_CSS_Status min_size_auto(rctx * r, const GTEXT_CSS_Property_Info * info, GTEXT_CSS_Value_Set ** out) {
  *out = NULL;
  GTEXT_CSS_Status st;
  size_t n = 0;
  char * t = computed_text(r->style, info->id, &n, &st);
  const bool is_auto = t && text_is(t, n, "auto");
  gtext_css_serialize_buffer_free(NULL, t);
  if (!is_auto) {
    return st;
  }
  bool zero = r->display_hidden;
  if (!zero) {
    st = css_computed_in_display_none(r->in, &zero);   /* "It also resolves to zero when no box is generated": a box in a display: none subtree has none either */
    if (st != GTEXT_CSS_OK) {
      return st;
    }
  }
  if (!zero && computed_is(r, "aspect-ratio", 12, "auto", &st) && st == GTEXT_CSS_OK) {
    bool item = false;
    const GTEXT_CSS_Registry * reg = r->in->core->registry;
    const GTEXT_CSS_Property_Id position = gtext_css_registry_find_property(reg, "position", 8);
    size_t pn = 0;
    char * pt = position ? computed_text(r->style, position, &pn, &st) : NULL;
    const bool out_of_flow = pt && (text_is(pt, pn, "absolute") || text_is(pt, pn, "fixed"));
    gtext_css_serialize_buffer_free(NULL, pt);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    GTEXT_CSS_Computed_Style * parent = NULL;
    if (!out_of_flow) {
      st = css_computed_parent_style(r->in, &parent);
      if (st != GTEXT_CSS_OK) {
        return st;
      }
    }
    const GTEXT_CSS_Property_Id display = gtext_css_registry_find_property(reg, "display", 7);
    if (parent && display) {
      size_t dn = 0;
      char * d = computed_text(parent, display, &dn, &st);
      item = d && is_flex_or_grid(d, dn);
      gtext_css_serialize_buffer_free(NULL, d);
    }
    gtext_css_computed_free(parent);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    zero = !item;
  }
  return zero ? recompute_text(r, info, "0px", 3, out) : GTEXT_CSS_OK;
}

/// "If the border-style corresponding to a given border-width is none or hidden, then the used width is 0" (Backgrounds 3, 4.3), and "the resolved value for the border-width properties is
/// the used value": the computed value keeps its length, which is what a child inherits, and what getComputedStyle() answers is `0px` when the style of the same side makes the width
/// not drawn. The style is the property whose name is the width's with `-width` replaced by `-style` (a logical side has its logical style).
static GTEXT_CSS_Status border_width(rctx * r, const GTEXT_CSS_Property_Info * info, GTEXT_CSS_Value_Set ** out) {
  *out = NULL;
  static const char suffix[] = "-width";
  const size_t sl = sizeof suffix - 1;
  char buf[160];
  if (info->name_length <= sl || info->name_length - sl + 6 >= sizeof buf || memcmp(info->name + info->name_length - sl, suffix, sl) != 0) {
    return GTEXT_CSS_OK;
  }
  memcpy(buf, info->name, info->name_length - sl);
  memcpy(buf + info->name_length - sl, "-style", 6);
  const GTEXT_CSS_Property_Id style_id = gtext_css_registry_find_property(r->in->core->registry, buf, info->name_length - sl + 6);
  if (!style_id) {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Status st;
  size_t n = 0;
  char * t = computed_text(r->style, style_id, &n, &st);
  const bool hidden = t && (text_is(t, n, "none") || text_is(t, n, "hidden"));
  gtext_css_serialize_buffer_free(NULL, t);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  return hidden ? recompute_text(r, info, "0px", 3, out) : GTEXT_CSS_OK;
}

/// `caret-color` and `outline-color`: the used value of `auto` is the current color, as both browsers answer (the probe of the stage; the served text leaves it to the user agent).
static bool auto_is_current(const GTEXT_CSS_Property_Info * info) {
  return (info->name_length == 11 && memcmp(info->name, "caret-color", 11) == 0) || (info->name_length == 13 && memcmp(info->name, "outline-color", 13) == 0);
}

/// One longhand: the set to put in the block (`*owned` is set when the caller must free it) and its origin.
static GTEXT_CSS_Status resolve_one(rctx * r, const GTEXT_CSS_Property_Info * info, const GTEXT_CSS_Value_Set * computed, css_resolved_category cat, GTEXT_CSS_Value_Set ** owned,
    GTEXT_CSSOM_Origin * origin) {
  *owned = NULL;
  *origin = GTEXT_CSSOM_ORIGIN_COMPUTED;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  switch (cat) {
  case CSS_RESOLVED_COMPUTED:
  case CSS_RESOLVED_USED_COLOR:
    /* every <color> in a value is resolved to its used value ("The resolved value of a <color> is its used value"), in a property CSSOM lists or not */
    st = use_colors(r, computed, auto_is_current(info), owned);
    *origin = *owned ? GTEXT_CSSOM_ORIGIN_USED : GTEXT_CSSOM_ORIGIN_COMPUTED;
    return st;
  case CSS_RESOLVED_SHORTEST: {
    st = use_colors(r, computed, auto_is_current(info), owned);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    *origin = *owned ? GTEXT_CSSOM_ORIGIN_USED : GTEXT_CSSOM_ORIGIN_COMPUTED;
    GTEXT_CSS_Value_Set * made = NULL;
    st = shortest_form(r, info, *owned ? *owned : computed, &made);
    if (st != GTEXT_CSS_OK) {
      gtext_css_value_set_free(*owned);
      *owned = NULL;
      return st;
    }
    if (made) {
      gtext_css_value_set_free(*owned);   /* the new set holds its own count of what it shares */
      *owned = made;
    }
    return GTEXT_CSS_OK;
  }
  case CSS_RESOLVED_ANIMATION_DURATION:
    st = animation_duration(r, info, owned);
    *origin = *owned ? GTEXT_CSSOM_ORIGIN_USED : GTEXT_CSSOM_ORIGIN_COMPUTED;
    return st;
  case CSS_RESOLVED_BORDER_WIDTH:
    st = border_width(r, info, owned);
    *origin = *owned ? GTEXT_CSSOM_ORIGIN_USED : GTEXT_CSSOM_ORIGIN_COMPUTED;
    return st;
  case CSS_RESOLVED_MIN_SIZE_AUTO:
    st = min_size_auto(r, info, owned);
    *origin = *owned ? GTEXT_CSSOM_ORIGIN_USED : GTEXT_CSSOM_ORIGIN_COMPUTED;
    return st;
  case CSS_RESOLVED_USED_IF_APPLIES:
  case CSS_RESOLVED_USED_IF_POSITIONED:
    if (r->display_hidden || (cat == CSS_RESOLVED_USED_IF_POSITIONED && !r->positioned)) {
      return GTEXT_CSS_OK;   /* "Otherwise the resolved value is the computed value" */
    }
    break;
  case CSS_RESOLVED_LINE_HEIGHT: {
    size_t n = 0;
    char * t = computed_text(r->style, info->id, &n, &st);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    const bool normal = t && text_is(t, n, "normal");
    gtext_css_serialize_buffer_free(NULL, t);
    if (normal) {
      return GTEXT_CSS_OK;
    }
    /* an absolute length is its own used value: no box is needed to know it (CSS Inline 3: "Computed value: for <length> the absolute length") */
    const GTEXT_CSS_Value * root = gtext_css_value_set_root(computed);
    if (root && !(root->flags & GTEXT_CSS_VFLAG_MATH) && (root->kind == GTEXT_CSS_VAL_ZERO || (root->kind == GTEXT_CSS_VAL_DIMENSION && root->unit == GTEXT_CSS_UNIT_PX))) {
      *owned = gtext_css_value_set_retain(computed);
      *origin = GTEXT_CSSOM_ORIGIN_USED;
      return GTEXT_CSS_OK;
    }
    break;
  }
  case CSS_RESOLVED_USED:
  default:
    break;
  }
  bool asked = false;
  st = ask_layout(r, info, &asked, owned);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!asked) {
    *origin = GTEXT_CSSOM_ORIGIN_ABSENT_LAYOUT;
  }
  else if (*owned) {
    *origin = GTEXT_CSSOM_ORIGIN_USED;
  }
  (void)computed;
  return GTEXT_CSS_OK;
}

/* ---- the block ------------------------------------------------------------------------------------------------------------------------------------- */

GTEXT_CSS_Status css_resolved_build(const css_resolve_in * in, const GTEXT_CSS_Computed_Style * style, GTEXT_CSS_Declaration_Block ** out_block, uint8_t ** out_origins,
    size_t * out_norigins, char ** out_untimed_custom) {
  css_core * core = in->core;
  const GTEXT_CSS_Registry * reg = core->registry;
  *out_block = NULL;
  *out_origins = NULL;
  *out_norigins = 0;
  *out_untimed_custom = NULL;
  char * untimed_custom = NULL;
  size_t untimed_length = 0;
  rctx r;
  GTEXT_CSS_Status st = rctx_init(&r, in, style);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  const size_t nprops = gtext_css_registry_property_count(reg);
  entry * list = gtext_allocator_malloc(&core->alloc, (nprops + 1) * sizeof *list);
  const size_t norigins = nprops + 2;
  uint8_t * origins = gtext_allocator_calloc(&core->alloc, norigins, 1);
  GTEXT_CSS_Declaration_Block * block = NULL;
  size_t n = 0;
  if (!list || !origins) {
    st = GTEXT_CSS_E_OOM;
  }
  else {
    GTEXT_CSS_Declaration_Options bo = gtext_css_declaration_options_default();
    bo.allocator = &core->alloc;
    bo.max_declarations = core->config.max_declarations;
    bo.max_output_bytes = core->config.max_output_bytes;
    st = gtext_css_block_new(reg, &bo, &block);
  }
  if (st == GTEXT_CSS_OK) {
    /* the supported longhands: those that have a computed value, in the registry's order, which is lexicographic unless the caller registered some (they come after) */
    size_t cursor = 0;
    GTEXT_CSS_Property_Id id;
    const GTEXT_CSS_Value_Set * set;
    bool sorted = true;
    while (gtext_css_computed_next(style, &cursor, &id, &set)) {
      GTEXT_CSS_Property_Info info;
      if (!gtext_css_registry_property_info(reg, id, &info) || (info.flags & (GTEXT_CSS_PROP_SHORTHAND | GTEXT_CSS_PROP_ALIAS | GTEXT_CSS_PROP_CUSTOM)) || !set) {
        continue;
      }
      if (n >= nprops + 1) {
        break;
      }
      list[n].id = id;
      list[n].name = info.name;
      list[n].length = info.name_length;
      list[n].set = set;
      if (n > 0 && entry_cmp(&list[n - 1], &list[n]) > 0) {
        sorted = false;
      }
      n++;
    }
    if (!sorted) {
      qsort(list, n, sizeof *list, entry_cmp);
    }
  }
  for (size_t i = 0; st == GTEXT_CSS_OK && i < n; i++) {
    GTEXT_CSS_Property_Info info;
    gtext_css_registry_property_info(reg, list[i].id, &info);
    GTEXT_CSS_Value_Set * owned = NULL;
    GTEXT_CSSOM_Origin origin = GTEXT_CSSOM_ORIGIN_COMPUTED;
    st = resolve_one(&r, &info, list[i].set, css_resolved_lookup(info.name, info.name_length), &owned, &origin);
    if (st != GTEXT_CSS_OK) {
      break;
    }
    if (origin == GTEXT_CSSOM_ORIGIN_COMPUTED && gtext_css_computed_animation_untimed(style, list[i].id)) {
      origin = GTEXT_CSSOM_ORIGIN_ABSENT_TIMELINE;   /* an animation targets it and the caller gave no time: the underlying value stands */
    }
    GTEXT_CSS_Property_Value pv;
    memset(&pv, 0, sizeof pv);
    pv.outcome = GTEXT_CSS_PROP_RESULT_VALID;
    pv.property = pv.named = list[i].id;
    pv.set = owned ? owned : (GTEXT_CSS_Value_Set *)(uintptr_t)list[i].set;
    st = gtext_css_block_add(block, &pv, false, NULL);
    gtext_css_value_set_free(owned);   /* the block took its own count */
    if (st == GTEXT_CSS_OK && list[i].id < norigins) {
      origins[list[i].id] = (uint8_t)origin;
    }
  }
  /* the custom properties whose computed value is not the guaranteed-invalid value, in the order of their names */
  for (size_t i = 0; st == GTEXT_CSS_OK; i++) {
    GTEXT_CSS_String name;
    const GTEXT_CSS_Custom_Value * value = NULL;
    if (!gtext_css_computed_custom_at(style, i, &name, &value)) {
      break;
    }
    char * text = NULL;
    size_t len = 0;
    GTEXT_CSS_Serialize_Options so;
    memset(&so, 0, sizeof so);
    so.allocator = &core->alloc;
    so.max_output_bytes = core->config.max_output_bytes;
    st = gtext_css_custom_value_text(value, &so, &text, &len);
    if (st != GTEXT_CSS_OK) {
      break;
    }
    GTEXT_CSS_Property_Value pv;
    memset(&pv, 0, sizeof pv);
    st = gtext_css_parse_property_text(reg, name.data, name.length, text, len, NULL, &pv);
    gtext_css_serialize_buffer_free(&so, text);
    if (st == GTEXT_CSS_OK && (pv.outcome == GTEXT_CSS_PROP_RESULT_VALID || pv.outcome == GTEXT_CSS_PROP_RESULT_PENDING || pv.outcome == GTEXT_CSS_PROP_RESULT_WIDE)) {
      st = gtext_css_block_add_custom(block, name.data, name.length, &pv, false);
    }
    gtext_css_property_value_free(&pv);
    if (st == GTEXT_CSS_OK && gtext_css_computed_animation_untimed_custom(style, name.data, name.length)) {
      char * bigger = gtext_allocator_realloc(&core->alloc, untimed_custom, untimed_length + name.length + 2);
      if (!bigger) {
        st = GTEXT_CSS_E_OOM;
        break;
      }
      untimed_custom = bigger;
      memcpy(untimed_custom + untimed_length, name.data, name.length);
      untimed_custom[untimed_length + name.length] = '\0';
      untimed_length += name.length + 1;
      untimed_custom[untimed_length] = '\0';
    }
  }
  rctx_free(&r);
  gtext_allocator_free(&core->alloc, list);
  if (st != GTEXT_CSS_OK) {
    gtext_css_block_free(block);
    gtext_allocator_free(&core->alloc, origins);
    gtext_allocator_free(&core->alloc, untimed_custom);
    return st;
  }
  *out_untimed_custom = untimed_custom;
  *out_block = block;
  *out_origins = origins;
  *out_norigins = norigins;
  return GTEXT_CSS_OK;
}
