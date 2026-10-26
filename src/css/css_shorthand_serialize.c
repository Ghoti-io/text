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
 * Writing a declaration block as CSSOM does (css_shorthand.h): each shorthand whose longhands are all in the block, with equal importance and no
 * pending value, as the shortest form of the shorthand; every other declaration as `name: value`.
 *
 * The text a shorthand makes of its longhands is the inverse of its expansion (css_shorthand.c): BOX and PAIR compress equal values, ANY and LAYERS
 * leave out the components that are their longhand's initial value (in the order of the pin's longhands, with the `/` of the groups that have
 * one), COPY needs every longhand to agree, and `border-radius`, `flex` and `font` have rules of their own. A shorthand nested in another
 * (`border` holds `border-width`) is written first, and counts only if its four sides are one value.
 */

#include <stdlib.h>
#include <string.h>

#include "css_grid_text.h"
#include "css_shorthand_internal.h"

#define MAX_DIRECT 64u

typedef struct {
  GTEXT_CSS_Property_Id sh;
  char * text;          ///< The shorthand's text, or NULL when its longhands do not make one.
  bool uniform;         ///< A BOX or PAIR whose values are all equal (a nested one counts only then).
  bool all_initial;     ///< Every longhand is its initial value.
  char ** items;        ///< LAYERS: the text of each layer's items; NULL otherwise.
  size_t nitems;
  bool wide;            ///< Every longhand is one CSS-wide keyword (text is it).
  bool pending;         ///< The text is a pending value's.
} stext;

typedef struct {
  const GTEXT_CSS_Declaration_Block * b;
  const GTEXT_CSS_Registry * reg;
  GTEXT_Allocator alloc;
  size_t nprops;
  uint32_t * last;              ///< Property id -> index + 1 of the last item of it.
  stext * texts;                ///< Evaluated shorthands.
  size_t ntexts, captexts;
  bool important;
  bool have_important;
  bool mixed_importance;
  GTEXT_CSS_Status status;
  GTEXT_Allocator base;         ///< The block's allocator; `alloc` is a shim over it that records a failure in `status`, so that none is lost.
  char ** init_cache_text;      ///< Per property: the serialization of its initial value (NULL unknown), lazily.
  signed char * init_cache_state;
} sctx;

static void * shim_malloc(void * ctx, size_t n) {
  sctx * c = ctx;
  void * p = gtext_allocator_malloc(&c->base, n);
  if (!p) {
    c->status = GTEXT_CSS_E_OOM;
  }
  return p;
}

static void * shim_calloc(void * ctx, size_t n, size_t size) {
  sctx * c = ctx;
  void * p = gtext_allocator_calloc(&c->base, n, size);
  if (!p) {
    c->status = GTEXT_CSS_E_OOM;
  }
  return p;
}

static void * shim_realloc(void * ctx, void * old, size_t n) {
  sctx * c = ctx;
  void * p = gtext_allocator_realloc(&c->base, old, n);
  if (!p) {
    c->status = GTEXT_CSS_E_OOM;
  }
  return p;
}

static void shim_free(void * ctx, void * p) {
  sctx * c = ctx;
  gtext_allocator_free(&c->base, p);
}

static bool nm_len(const sctx * c, GTEXT_CSS_Property_Id id, const char * name, size_t len) {
  GTEXT_CSS_Property_Info info;
  return gtext_css_registry_property_info(c->reg, id, &info) && info.name_length == len && memcmp(info.name, name, len) == 0;
}

static bool nm(const sctx * c, GTEXT_CSS_Property_Id id, const char * name) {
  GTEXT_CSS_Property_Info info;
  return gtext_css_registry_property_info(c->reg, id, &info) && strlen(name) == info.name_length && memcmp(info.name, name, info.name_length) == 0;
}

static GTEXT_CSS_Property_Id resolve(const sctx * c, GTEXT_CSS_Property_Id id) {
  return gtext_css_registry_resolve_alias(c->reg, id);
}

static bool sctx_init(sctx * c, const GTEXT_CSS_Declaration_Block * b) {
  memset(c, 0, sizeof *c);
  c->b = b;
  c->reg = b->registry;
  c->base = b->alloc;
  c->alloc.ctx = c;
  c->alloc.malloc_fn = shim_malloc;
  c->alloc.calloc_fn = shim_calloc;
  c->alloc.realloc_fn = shim_realloc;
  c->alloc.free_fn = shim_free;
  c->nprops = gtext_css_registry_property_count(b->registry);
  c->last = gtext_allocator_calloc(&c->alloc, c->nprops + 1, sizeof *c->last);
  c->init_cache_text = gtext_allocator_calloc(&c->alloc, c->nprops + 1, sizeof *c->init_cache_text);
  c->init_cache_state = gtext_allocator_calloc(&c->alloc, c->nprops + 1, 1);
  if (!c->last || !c->init_cache_text || !c->init_cache_state) {
    return false;
  }
  for (size_t i = 0; i < b->count; i++) {
    const blk_item * it = &b->items[i];
    if (it->property && it->property <= c->nprops && !it->custom_name) {
      c->last[it->property] = (uint32_t)(i + 1);
    }
  }
  return true;
}

static void stext_free(sctx * c, stext * t) {
  gtext_allocator_free(&c->alloc, t->text);
  for (size_t i = 0; i < t->nitems; i++) {
    gtext_allocator_free(&c->alloc, t->items ? t->items[i] : NULL);
  }
  gtext_allocator_free(&c->alloc, t->items);
}

static void sctx_free(sctx * c) {
  for (size_t i = 0; i < c->ntexts; i++) {
    stext_free(c, &c->texts[i]);
  }
  gtext_allocator_free(&c->alloc, c->texts);
  if (c->init_cache_text) {
    for (size_t i = 0; i <= c->nprops; i++) {
      gtext_allocator_free(&c->alloc, c->init_cache_text[i]);
    }
  }
  gtext_allocator_free(&c->alloc, c->init_cache_text);
  gtext_allocator_free(&c->alloc, c->init_cache_state);
  gtext_allocator_free(&c->alloc, c->last);
}

/// The CSSOM text of the last declaration of a property: a new string; NULL for none or when the value is not a plain one.
static char * leaf_text(sctx * c, GTEXT_CSS_Property_Id id, const blk_item ** item) {
  const uint32_t at = id <= c->nprops ? c->last[id] : 0;
  if (!at) {
    return NULL;
  }
  const blk_item * it = &c->b->items[at - 1];
  if (item) {
    *item = it;
  }
  char * t = NULL;
  size_t n = 0;
  if (it->outcome == GTEXT_CSS_PROP_RESULT_PENDING) {
    return NULL;
  }
  if (css_item_text(c->reg, &c->alloc, id, it->outcome, it->wide, it->set, &t, &n) != GTEXT_CSS_OK) {
    c->status = GTEXT_CSS_E_OOM;
    return NULL;
  }
  return t;
}

/// The serialization of a property's initial value, cached; NULL when it has none that parses.
static const char * initial_serialized(sctx * c, GTEXT_CSS_Property_Id id) {
  if (id > c->nprops) {
    return NULL;
  }
  if (c->init_cache_state[id]) {
    return c->init_cache_text[id];
  }
  c->init_cache_state[id] = 1;
  const char * text = css_initial_text(c->reg, id);
  if (!text) {
    return NULL;
  }
  GTEXT_CSS_Property_Value pv;
  GTEXT_CSS_Status st = css_parse_longhand(c->reg, &c->alloc, id, text, strlen(text), &pv);
  if (st == GTEXT_CSS_OK && pv.outcome == GTEXT_CSS_PROP_RESULT_VALID) {
    char * t = NULL;
    size_t n = 0;
    if (css_item_text(c->reg, &c->alloc, id, pv.outcome, pv.wide, pv.set, &t, &n) == GTEXT_CSS_OK) {
      c->init_cache_text[id] = t;
    }
  }
  gtext_css_property_value_free(&pv);
  return c->init_cache_text[id];
}

/* ---- the structure of a shorthand --------------------------------------------------------------------------------------- */

typedef struct {
  GTEXT_CSS_Property_Id ids[MAX_DIRECT * 2];   ///< Direct longhands (resolved), then the properties it resets.
  size_t nlong, nreset;
} directs;

static bool get_directs(const sctx * c, GTEXT_CSS_Property_Id sh, directs * d) {
  GTEXT_CSS_Property_Info info;
  if (!gtext_css_registry_property_info(c->reg, sh, &info) || info.longhand_count > MAX_DIRECT || info.reset_longhand_count > MAX_DIRECT) {
    return false;
  }
  d->nlong = info.longhand_count;
  d->nreset = info.reset_longhand_count;
  for (size_t i = 0; i < d->nlong; i++) {
    d->ids[i] = resolve(c, gtext_css_registry_longhand(c->reg, sh, i));
  }
  for (size_t i = 0; i < d->nreset; i++) {
    d->ids[d->nlong + i] = resolve(c, gtext_css_registry_reset_longhand(c->reg, sh, i));
  }
  return true;
}

static bool is_sh(const sctx * c, GTEXT_CSS_Property_Id id) {
  GTEXT_CSS_Property_Info info;
  return gtext_css_registry_property_info(c->reg, id, &info) && (info.flags & GTEXT_CSS_PROP_SHORTHAND);
}

/// Every leaf longhand under a shorthand (and what it resets), without the repeats; false past `cap`.
static bool leaves_under(const sctx * c, GTEXT_CSS_Property_Id sh, GTEXT_CSS_Property_Id * out, size_t cap, size_t * n) {
  GTEXT_CSS_Property_Id stack[256];
  size_t sp = 0;
  *n = 0;
  stack[sp++] = sh;
  while (sp) {
    const GTEXT_CSS_Property_Id cur = stack[--sp];
    directs d;
    if (!get_directs(c, cur, &d)) {
      return false;
    }
    for (size_t i = 0; i < d.nlong + d.nreset; i++) {
      if (is_sh(c, d.ids[i])) {
        if (sp == 256) {
          return false;
        }
        stack[sp++] = d.ids[i];
        continue;
      }
      bool seen = false;
      for (size_t k = 0; k < *n && !seen; k++) {
        seen = out[k] == d.ids[i];
      }
      if (!seen) {
        if (*n == cap) {
          return false;
        }
        out[(*n)++] = d.ids[i];
      }
    }
  }
  return true;
}

/// The shorthands under `sh` in an order where each comes after those it holds (post-order), `sh` last.
static bool eval_order(const sctx * c, GTEXT_CSS_Property_Id sh, GTEXT_CSS_Property_Id * order, size_t cap, size_t * n) {
  struct { GTEXT_CSS_Property_Id id; size_t i; } stack[64];
  size_t sp = 0;
  *n = 0;
  stack[sp].id = sh;
  stack[sp].i = 0;
  sp++;
  while (sp) {
    directs d;
    if (!get_directs(c, stack[sp - 1].id, &d)) {
      return false;
    }
    if (stack[sp - 1].i < d.nlong + d.nreset) {
      const GTEXT_CSS_Property_Id kid = d.ids[stack[sp - 1].i++];
      if (is_sh(c, kid)) {
        bool seen = false;
        for (size_t k = 0; k < *n && !seen; k++) {
          seen = order[k] == kid;
        }
        if (!seen) {
          if (sp == 64) {
            return false;
          }
          stack[sp].id = kid;
          stack[sp].i = 0;
          sp++;
        }
      }
      continue;
    }
    if (*n == cap) {
      return false;
    }
    order[(*n)++] = stack[sp - 1].id;
    sp--;
  }
  return true;
}

static const stext * stext_of(const sctx * c, GTEXT_CSS_Property_Id sh) {
  for (size_t i = 0; i < c->ntexts; i++) {
    if (c->texts[i].sh == sh) {
      return &c->texts[i];
    }
  }
  return NULL;
}

/* ---- the texts a kind makes ------------------------------------------------------------------------------------------------- */

static char * dupn(sctx * c, const char * s, size_t n) {
  char * d = gtext_allocator_malloc(&c->alloc, n + 1);
  if (d) {
    memcpy(d, s, n);
    d[n] = '\0';
  }
  else {
    c->status = GTEXT_CSS_E_OOM;
  }
  return d;
}

static bool texteq(const char * a, const char * b) {
  return a && b && strcmp(a, b) == 0;
}

/// A longhand of a shorthand, as the text of its value: a leaf's, or a nested shorthand's.
typedef struct {
  GTEXT_CSS_Property_Id id;
  char * text;           ///< Owned (leaf) or borrowed from the evaluated table (nested; `borrowed`).
  bool borrowed;
  bool initial;
  bool uniform;
  const stext * sub;
  const blk_item * item;
} lhv;

static void lhv_free(sctx * c, lhv * v, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (!v[i].borrowed) {
      gtext_allocator_free(&c->alloc, v[i].text);
    }
  }
}

static bool lhv_fill(sctx * c, const directs * d, lhv * v) {
  /* all of them first: a longhand with no text (one that is pending, or missing) ends the fill, and the caller frees every entry */
  memset(v, 0, (d->nlong + d->nreset) * sizeof *v);
  for (size_t i = 0; i < d->nlong + d->nreset; i++) {
    v[i].id = d->ids[i];
    if (is_sh(c, d->ids[i])) {
      const stext * t = stext_of(c, d->ids[i]);
      if (!t || !t->text) {
        return false;
      }
      v[i].text = t->text;
      v[i].borrowed = true;
      v[i].initial = t->all_initial;
      v[i].uniform = t->uniform;
      v[i].sub = t;
    }
    else {
      v[i].text = leaf_text(c, d->ids[i], &v[i].item);
      if (!v[i].text) {
        return false;
      }
      v[i].initial = texteq(v[i].text, initial_serialized(c, d->ids[i]));
      v[i].uniform = true;
    }
  }
  return true;
}

static bool box_text(sctx * c, const lhv * v, size_t n, stext * out) {
  if (n != 4) {
    return false;
  }
  const char * t = v[0].text;
  const char * r = v[1].text;
  const char * b = v[2].text;
  const char * l = v[3].text;
  tbuf buf;
  tb_init(&buf, &c->alloc);
  tb_puts(&buf, t);
  if (!texteq(l, r) || !texteq(b, t) || !texteq(r, t)) {
    tb_puts(&buf, " ");
    tb_puts(&buf, r);
    if (!texteq(l, r) || !texteq(b, t)) {
      tb_puts(&buf, " ");
      tb_puts(&buf, b);
      if (!texteq(l, r)) {
        tb_puts(&buf, " ");
        tb_puts(&buf, l);
      }
    }
  }
  out->text = tb_take(&buf, NULL);
  out->uniform = texteq(t, r) && texteq(t, b) && texteq(t, l);
  out->all_initial = v[0].initial && v[1].initial && v[2].initial && v[3].initial;
  return out->text != NULL;
}

static bool pair_text(sctx * c, const lhv * v, size_t n, stext * out) {
  if (n != 2) {
    return false;
  }
  tbuf buf;
  tb_init(&buf, &c->alloc);
  tb_puts(&buf, v[0].text);
  if (!texteq(v[0].text, v[1].text)) {
    tb_puts(&buf, " ");
    tb_puts(&buf, v[1].text);
  }
  out->text = tb_take(&buf, NULL);
  out->uniform = texteq(v[0].text, v[1].text);
  out->all_initial = v[0].initial && v[1].initial;
  return out->text != NULL;
}

/// The slash groups: a longhand written after `/` and the longhand it follows, per shorthand.
typedef struct { const char * sh; const char * after; const char * slash1; const char * slash2; } slash_rule;
static const slash_rule slash_rules[] = {
  {"border-image", "border-image-slice", "border-image-width", "border-image-outset"},
  {"mask-border", "mask-border-slice", "mask-border-width", "mask-border-outset"},
  {"columns", "column-count", "column-height", NULL},
  {"background", "background-position", "background-size", NULL},
  {"mask", "mask-position", "mask-size", NULL},
  {"container", "container-name", "container-type", NULL},
};

/// What the draft says of how some shorthands are written, beyond "leave out the components that are initial": the order of the components (the
/// grammar's, where the pin lists the longhands in another), and the component that stands for the shorthand when every one is initial.
typedef struct { const char * sh; const char * order; const char * all_initial; } ser_rule;
static const ser_rule ser_rules[] = {
  {"list-style", "list-style-position,list-style-image,list-style-type", "list-style-position"},
  {"outline", "outline-color,outline-style,outline-width", "outline-style"},
  {"vertical-align", "baseline-source,alignment-baseline,baseline-shift", "alignment-baseline"},
  {"box-shadow", NULL, "box-shadow-offset"},
  {"animation", "animation-duration,animation-timing-function,animation-delay,animation-iteration-count,animation-direction,animation-fill-mode,"
      "animation-play-state,animation-name,animation-timeline", "animation-name"},
};

/// The order components are written in: `perm[k]` is the index of the k-th. Returns the index of the all-initial component (or -1).
static int serial_order(const sctx * c, GTEXT_CSS_Property_Id sh, const directs * d, size_t n, size_t perm[MAX_DIRECT * 2]) {
  for (size_t i = 0; i < n; i++) {
    perm[i] = i;
  }
  int all_initial = -1;
  for (size_t r = 0; r < sizeof ser_rules / sizeof *ser_rules; r++) {
    if (!nm(c, sh, ser_rules[r].sh)) {
      continue;
    }
    if (ser_rules[r].all_initial) {
      for (size_t i = 0; i < n; i++) {
        if (nm(c, d->ids[i], ser_rules[r].all_initial)) {
          all_initial = (int)i;
        }
      }
    }
    if (ser_rules[r].order) {
      size_t k = 0;
      unsigned char used[MAX_DIRECT * 2];
      memset(used, 0, sizeof used);
      const char * q = ser_rules[r].order;
      while (*q) {
        const char * e = q;
        while (*e && *e != ',') {
          e++;
        }
        for (size_t i = 0; i < n; i++) {
          if (!used[i] && strlen(q) >= (size_t)(e - q) && nm_len(c, d->ids[i], q, (size_t)(e - q))) {
            perm[k++] = i;
            used[i] = 1;
          }
        }
        q = *e ? e + 1 : e;
      }
      for (size_t i = 0; i < n; i++) {
        if (!used[i]) {
          perm[k++] = i;
        }
      }
    }
  }
  return all_initial;
}

/// A component that is its initial value but is written anyway: list-style's position beside a type that reads as a position keyword.
static bool force_text(const sctx * c, GTEXT_CSS_Property_Id sh, const directs * d, const lhv * v, size_t i) {
  if (nm(c, sh, "position-try") && nm(c, d->ids[i], "position-try-fallbacks")) {
    return true;   /* the fallbacks are written even as `none`: `most-width none` is not `most-width` */
  }
  if (!nm(c, sh, "list-style") || !nm(c, d->ids[i], "list-style-position")) {
    return false;
  }
  for (size_t k = 0; k < d->nlong; k++) {
    if (nm(c, d->ids[k], "list-style-type") && (texteq(v[k].text, "inside") || texteq(v[k].text, "outside"))) {
      return true;
    }
  }
  return false;
}

/// The keywords that stand for the values of several longhands, written back (see split_keyword_shorthand in css_shorthand.c).
/// 0: the shorthand is not one of these; 1: `out` holds its keyword form; 2: it is one of these and its longhands have no form (the generic writing would be wrong).
static int keyword_text(sctx * c, GTEXT_CSS_Property_Id sh, const directs * d, const lhv * v, stext * out) {
  if (nm(c, sh, "column-rule-inset") || nm(c, sh, "row-rule-inset")) {
    const char *cs = NULL, *ce = NULL, *js = NULL, *je = NULL;
    for (size_t i = 0; i < d->nlong; i++) {
      if (nm(c, d->ids[i], "column-rule-inset-cap-start") || nm(c, d->ids[i], "row-rule-inset-cap-start")) cs = v[i].text;
      else if (nm(c, d->ids[i], "column-rule-inset-cap-end") || nm(c, d->ids[i], "row-rule-inset-cap-end")) ce = v[i].text;
      else if (nm(c, d->ids[i], "column-rule-inset-junction-start") || nm(c, d->ids[i], "row-rule-inset-junction-start")) js = v[i].text;
      else if (nm(c, d->ids[i], "column-rule-inset-junction-end") || nm(c, d->ids[i], "row-rule-inset-junction-end")) je = v[i].text;
    }
    if (!cs || !ce || !js || !je) {
      return 2;
    }
    tbuf b;
    tb_init(&b, &c->alloc);
    tb_puts(&b, cs);
    if (!texteq(cs, ce)) {
      tb_puts(&b, " ");
      tb_puts(&b, ce);
    }
    if (!texteq(cs, js) || !texteq(ce, je)) {
      tb_puts(&b, " / ");
      tb_puts(&b, js);
      if (!texteq(js, je)) {
        tb_puts(&b, " ");
        tb_puts(&b, je);
      }
    }
    out->text = tb_take(&b, NULL);
    return out->text ? 1 : 2;
  }
  if (nm(c, sh, "text-decoration-skip")) {
    static const char * const names[4] = {"text-decoration-skip-self", "text-decoration-skip-box", "text-decoration-skip-spaces", "text-decoration-skip-ink"};
    static const char * const values[2][4] = {{"no-skip", "none", "none", "none"}, {"auto", "none", "start end", "auto"}};
    const char * got[4] = {NULL, NULL, NULL, NULL};
    for (size_t i = 0; i < d->nlong; i++) {
      for (size_t k = 0; k < 4; k++) {
        if (nm(c, d->ids[i], names[k])) {
          got[k] = v[i].text;
        }
      }
    }
    for (size_t w = 0; w < 2; w++) {
      bool all = true;
      for (size_t k = 0; k < 4; k++) {
        all = all && got[k] && texteq(got[k], values[w][k]);
      }
      if (all) {
        out->text = dupn(c, w == 0 ? "none" : "auto", w == 0 ? 4 : 4);
        out->all_initial = w == 1;
        return out->text ? 1 : 2;
      }
    }
    return 2;
  }
  if (nm(c, sh, "-webkit-line-clamp")) {
    const char * ml = NULL;
    const char * be = NULL;
    const char * ct = NULL;
    for (size_t i = 0; i < d->nlong; i++) {
      if (nm(c, d->ids[i], "max-lines")) ml = v[i].text;
      else if (nm(c, d->ids[i], "block-ellipsis")) be = v[i].text;
      else if (nm(c, d->ids[i], "continue")) ct = v[i].text;
    }
    if (ml && be && ct && texteq(ml, "auto") && texteq(be, "no-ellipsis") && texteq(ct, "normal")) {
      out->text = dupn(c, "none", 4);
      out->all_initial = true;
      return out->text ? 1 : 2;
    }
    if (ml && be && ct && texteq(be, "ellipsis") && texteq(ct, "-webkit-legacy") && !texteq(ml, "auto")) {
      out->text = dupn(c, ml, strlen(ml));
      return out->text ? 1 : 2;
    }
    return 2;   /* `none | <integer>`: any other longhands are no value of it (`line-clamp` has them) */
  }
  static const struct { const char * sh; const char * a; const char * av; const char * b; const char * bv; const char * kw; } table[] = {
    {"white-space", "white-space-collapse", "collapse", "text-wrap-mode", "wrap", "normal"},
    {"white-space", "white-space-collapse", "preserve", "text-wrap-mode", "nowrap", "pre"},
    {"white-space", "white-space-collapse", "preserve", "text-wrap-mode", "wrap", "pre-wrap"},
    {"white-space", "white-space-collapse", "preserve-breaks", "text-wrap-mode", "wrap", "pre-line"},
    {"text-spacing", "text-spacing-trim", "space-all", "text-autospace", "no-autospace", "none"},
    {"text-spacing", "text-spacing-trim", "auto", "text-autospace", "auto", "auto"},
  };
  for (size_t k = 0; k < sizeof table / sizeof *table; k++) {
    if (!nm(c, sh, table[k].sh)) {
      continue;
    }
    const char * a = NULL;
    const char * b = NULL;
    for (size_t i = 0; i < d->nlong; i++) {
      if (nm(c, d->ids[i], table[k].a)) {
        a = v[i].text;
      }
      else if (nm(c, d->ids[i], table[k].b)) {
        b = v[i].text;
      }
    }
    if (a && b && texteq(a, table[k].av) && texteq(b, table[k].bv)) {
      out->text = dupn(c, table[k].kw, strlen(table[k].kw));
      out->all_initial = nm(c, sh, "white-space") && k == 0;
      return out->text ? 1 : 2;
    }
  }
  return 0;
}

static bool any_text(sctx * c, GTEXT_CSS_Property_Id sh, const directs * d, const lhv * v, stext * out) {
  const slash_rule * sr = NULL;
  for (size_t i = 0; i < sizeof slash_rules / sizeof *slash_rules; i++) {
    if (nm(c, sh, slash_rules[i].sh)) {
      sr = &slash_rules[i];
    }
  }
  const int keyword = keyword_text(c, sh, d, v, out);
  if (keyword) {
    return keyword == 1;
  }
  size_t perm[MAX_DIRECT * 2];
  const int all_initial_at = serial_order(c, sh, d, d->nlong, perm);
  const bool soft_after = nm(c, sh, "columns");
  tbuf buf;
  tb_init(&buf, &c->alloc);
  bool any = false, all_initial = true;
  int idx_after = -1, idx_s1 = -1, idx_s2 = -1;
  /* the corner shorthands name the radius and the shape shorthands whole (`<'border-top-radius'>`, `<'corner-top-shape'>`), so what those write is a value of them
     whatever it holds; the others (`border`) name a box of sides, which is a value of them only when all its sides are one */
  GTEXT_CSS_Property_Info shi;
  const bool whole_nested = gtext_css_registry_property_info(c->reg, sh, &shi) && shi.name_length > 6 && memcmp(shi.name, "corner", 6) == 0;
  for (size_t i = 0; i < d->nlong; i++) {
    if (!v[i].uniform && !(whole_nested && v[i].sub)) {
      tb_free(&buf);
      return false;
    }
    if (!v[i].initial) {
      all_initial = false;
    }
    if (sr) {
      if (nm(c, d->ids[i], sr->after)) idx_after = (int)i;
      if (sr->slash1 && nm(c, d->ids[i], sr->slash1)) idx_s1 = (int)i;
      if (sr->slash2 && nm(c, d->ids[i], sr->slash2)) idx_s2 = (int)i;
    }
  }
  for (size_t k = 0; k < d->nlong; k++) {
    const size_t i = perm[k];
    if ((int)i == idx_s1 || (int)i == idx_s2) {
      continue;
    }
    const bool follows = (int)i == idx_after;
    const bool need_s1 = follows && idx_s1 >= 0 && !v[idx_s1].initial;
    const bool need_s2 = follows && idx_s2 >= 0 && !v[idx_s2].initial;
    if (v[i].initial && !(follows && (need_s1 || need_s2)) && !force_text(c, sh, d, v, i)) {
      continue;
    }
    /* columns: the slash follows the width or the count, whichever was written; the count alone is written only when the width was not */
    const bool skip_text = soft_after && follows && v[i].initial && any;
    if (!skip_text) {
      if (any) {
        tb_puts(&buf, " ");
      }
      any = true;
      tb_puts(&buf, v[i].text);
    }
    if (need_s1 || need_s2) {
      tb_puts(&buf, " / ");
      if (need_s1) {
        tb_puts(&buf, v[idx_s1].text);
      }
      if (need_s2) {
        tb_puts(&buf, need_s1 ? " / " : "/ ");
        tb_puts(&buf, v[idx_s2].text);
      }
    }
  }
  if (!any) {
    /* every component is its initial value: the one the draft names stands for them, else the first */
    if (d->nlong) {
      tb_puts(&buf, v[all_initial_at >= 0 ? (size_t)all_initial_at : perm[0]].text);
    }
  }
  out->text = tb_take(&buf, NULL);
  out->all_initial = all_initial;
  return out->text != NULL;
}

static bool copy_text(sctx * c, const lhv * v, size_t n, stext * out) {
  for (size_t i = 1; i < n; i++) {
    if (!texteq(v[0].text, v[i].text)) {
      return false;
    }
  }
  out->text = dupn(c, v[0].text, strlen(v[0].text));
  out->uniform = true;
  out->all_initial = v[0].initial;
  return out->text != NULL;
}

/// The items of a longhand's list value, as text each (a new array); NULL when it is not a list value of this block.
static char ** list_items(sctx * c, GTEXT_CSS_Property_Id id, size_t * n, bool ** defaulted) {
  const uint32_t at = id <= c->nprops ? c->last[id] : 0;
  *n = 0;
  if (!at) {
    return NULL;
  }
  const blk_item * it = &c->b->items[at - 1];
  if (it->outcome != GTEXT_CSS_PROP_RESULT_VALID) {
    return NULL;
  }
  const GTEXT_CSS_Value * root = gtext_css_value_set_root(it->set);
  if (!root) {
    return NULL;
  }
  GTEXT_CSS_Property_Info info;
  gtext_css_registry_property_info(c->reg, id, &info);
  const uint8_t rules = css_canon_rules_of(info.name, info.name_length);
  size_t count = 1;
  const GTEXT_CSS_Value * kids = root;
  if (root->kind == GTEXT_CSS_VAL_LIST && root->separator == GTEXT_CSS_SEP_COMMA) {
    count = root->child_count;
    kids = root->children;
  }
  char ** items = gtext_allocator_calloc(&c->alloc, count ? count : 1, sizeof *items);
  if (!items) {
    c->status = GTEXT_CSS_E_OOM;
    return NULL;
  }
  for (size_t i = 0; i < count; i++) {
    char * raw = NULL;
    size_t rl = 0;
    if (gtext_css_serialize_value_buffer(&kids[i], NULL, &raw, &rl) != GTEXT_CSS_OK) {
      c->status = GTEXT_CSS_E_OOM;
      break;
    }
    if (rules) {
      char * canon = NULL;
      size_t cl = 0;
      if (css_canon_apply(rules, &c->alloc, raw, rl, &canon, &cl) == GTEXT_CSS_OK) {
        items[i] = canon;
      }
      gtext_css_serialize_buffer_free(NULL, raw);
    }
    else {
      items[i] = dupn(c, raw, rl);
      gtext_css_serialize_buffer_free(NULL, raw);
    }
    if (!items[i]) {
      break;
    }
  }
  *n = count;
  if (defaulted) {
    /* which items a shorthand made for a component the author left out */
    *defaulted = gtext_allocator_calloc(&c->alloc, count ? count : 1, 1);
    if (!*defaulted) {
      c->status = GTEXT_CSS_E_OOM;
    }
    else if (kids == root) {
      (*defaulted)[0] = (root->flags & GTEXT_CSS_VFLAG_DEFAULTED) != 0;
    }
    else {
      for (size_t i = 0; i < count; i++) {
        (*defaulted)[i] = (kids[i].flags & GTEXT_CSS_VFLAG_DEFAULTED) != 0;
      }
    }
  }
  return items;
}

static void items_free(sctx * c, char ** items, size_t n) {
  for (size_t i = 0; items && i < n; i++) {
    gtext_allocator_free(&c->alloc, items[i]);
  }
  gtext_allocator_free(&c->alloc, items);
}

/// One layer of `animation-range`: the start without a `0%` after a range name, the end without a `100%` after one, and no end when it is what the
/// start implies (the start's range name; or `normal` for a start with none).
static bool range_layer_text(const char * start, const char * end, tbuf * out) {
  const size_t sn = strlen(start), en = strlen(end);
  size_t s_len = sn, e_len = en;
  const bool s_named = sn && start[0] >= 'a' && start[0] <= 'z' && strncmp(start, "normal", 6) != 0 && !(sn >= 4 && strncmp(start, "calc", 4) == 0);
  const bool e_named = en && end[0] >= 'a' && end[0] <= 'z' && strncmp(end, "normal", 6) != 0 && !(en >= 4 && strncmp(end, "calc", 4) == 0);
  if (s_named && sn > 3 && strcmp(start + sn - 3, " 0%") == 0) {
    s_len = sn - 3;
  }
  if (e_named && en > 5 && strcmp(end + en - 5, " 100%") == 0) {
    e_len = en - 5;
  }
  bool omit;
  if (s_named) {
    const char * sp = memchr(start, ' ', s_len);
    const size_t first = sp ? (size_t)(sp - start) : s_len;
    omit = memchr(end, ' ', e_len) == NULL && first == e_len && memcmp(start, end, e_len) == 0;
  }
  else {
    omit = texteq(end, "normal") || texteq(end, "100%");
  }
  tb_put(out, start, s_len);
  if (!omit) {
    tb_puts(out, " ");
    tb_put(out, end, e_len);
  }
  return !out->oom;
}

static bool layers_text(sctx * c, GTEXT_CSS_Property_Id sh, const directs * d, stext * out) {
  const size_t nh = d->nlong;
  char *** per = gtext_allocator_calloc(&c->alloc, nh ? nh : 1, sizeof *per);
  size_t * cnt = gtext_allocator_calloc(&c->alloc, nh ? nh : 1, sizeof *cnt);
  bool ok = per && cnt;
  size_t layers = 0;
  bool final_only_seen = false;
  const bool is_bg = nm(c, sh, "background");
  const bool is_bs = nm(c, sh, "box-shadow");
  bool ** dfl = gtext_allocator_calloc(&c->alloc, nh ? nh : 1, sizeof *dfl);   /* box-shadow: per longhand, which items the author left out */
  ok = ok && dfl;
  /* each longhand's items: a nested layered shorthand (background-position) gives its own */
  for (size_t h = 0; h < nh && ok; h++) {
    if (is_sh(c, d->ids[h])) {
      const stext * t = stext_of(c, d->ids[h]);
      if (!t || !t->items) {
        ok = false;
        break;
      }
      per[h] = gtext_allocator_calloc(&c->alloc, t->nitems ? t->nitems : 1, sizeof **per);
      if (!per[h]) {
        ok = false;
        break;
      }
      cnt[h] = t->nitems;
      for (size_t k = 0; k < t->nitems && per[h]; k++) {
        per[h][k] = t->items[k];   /* borrowed */
      }
    }
    else {
      per[h] = list_items(c, d->ids[h], &cnt[h], is_bs ? &dfl[h] : NULL);
      if (!per[h]) {
        ok = false;
        break;
      }
    }
  }
  /* an item that could not be made (memory) is a hole in the list: nothing below may read it */
  for (size_t h = 0; h < nh && ok; h++) {
    for (size_t k = 0; ok && per[h] && k < cnt[h]; k++) {
      ok = per[h][k] != NULL;
    }
  }
  unsigned long long final_only = 0;
  for (size_t h = 0; h < nh && ok; h++) {
    if (is_bg && nm(c, d->ids[h], "background-color")) {
      final_only |= 1ull << h;
      final_only_seen = true;
    }
    else if (cnt[h] > layers) {
      layers = cnt[h];
    }
  }
  (void)final_only_seen;
  /* animation-timeline is one value for all the layers when `animation` was written (it resets it): it stands for each layer */
  for (size_t h = 0; h < nh && ok && layers > 1; h++) {
    if (cnt[h] == 1 && !is_sh(c, d->ids[h]) && nm(c, sh, "animation") && nm(c, d->ids[h], "animation-timeline")) {
      char ** grown = gtext_allocator_realloc(&c->alloc, per[h], layers * sizeof *grown);
      if (!grown) {
        ok = false;
        c->status = GTEXT_CSS_E_OOM;
        break;
      }
      per[h] = grown;
      for (size_t k = 1; k < layers; k++) {
        per[h][k] = dupn(c, per[h][0], strlen(per[h][0]));
        cnt[h] = k + 1;
        if (!per[h][k]) {
          ok = false;
          c->status = GTEXT_CSS_E_OOM;
          break;
        }
      }
    }
  }
  for (size_t h = 0; h < nh && ok; h++) {
    if (!((final_only >> h) & 1ull) && cnt[h] != layers) {
      ok = false;
    }
  }
  if (ok && layers == 0) {
    ok = false;
  }
  tbuf buf;
  tb_init(&buf, &c->alloc);
  const slash_rule * sr = NULL;
  for (size_t i = 0; i < sizeof slash_rules / sizeof *slash_rules; i++) {
    if (nm(c, sh, slash_rules[i].sh)) {
      sr = &slash_rules[i];
    }
  }
  char ** layer_text = ok ? gtext_allocator_calloc(&c->alloc, layers, sizeof *layer_text) : NULL;
  if (ok && !layer_text) {
    ok = false;
  }
  bool all_initial = true;
  size_t lperm[MAX_DIRECT * 2];
  const int lall_initial = serial_order(c, sh, d, nh, lperm);
  /* a position is its two offsets: the horizontal alone would mean the vertical is `center` */
  const bool pair_layers = nm(c, sh, "background-position") && nh == 2;
  const bool range_layers = nm(c, sh, "animation-range") && nh == 2;
  for (size_t li = 0; ok && li < layers; li++) {
    tbuf lb;
    tb_init(&lb, &c->alloc);
    bool any = false;
    if (range_layers) {
      range_layer_text(per[0][li], per[1][li], &lb);
      all_initial = all_initial && texteq(per[0][li], "normal") && texteq(per[1][li], "normal");
      layer_text[li] = tb_take(&lb, NULL);
      if (!layer_text[li]) {
        ok = false;
      }
      continue;
    }
    if (pair_layers) {
      const char * ix = initial_serialized(c, d->ids[0]);
      const char * iy = initial_serialized(c, d->ids[1]);
      tb_puts(&lb, per[0][li]);
      tb_puts(&lb, " ");
      tb_puts(&lb, per[1][li]);
      all_initial = all_initial && texteq(per[0][li], ix) && texteq(per[1][li], iy);
      layer_text[li] = tb_take(&lb, NULL);
      if (!layer_text[li]) {
        ok = false;
      }
      continue;
    }
    int idx_after = -1, idx_s1 = -1, box_o = -1, box_c = -1;
    for (size_t h = 0; h < nh; h++) {
      if (nm(c, d->ids[h], "background-origin") || nm(c, d->ids[h], "mask-origin")) box_o = (int)h;
      if (nm(c, d->ids[h], "background-clip") || nm(c, d->ids[h], "mask-clip")) box_c = (int)h;
    }
    if (box_o < 0 || box_c < 0) {
      box_o = box_c = -1;
    }
    for (size_t h = 0; h < nh; h++) {
      if (sr && nm(c, d->ids[h], sr->after)) idx_after = (int)h;
      if (sr && sr->slash1 && nm(c, d->ids[h], sr->slash1)) idx_s1 = (int)h;
    }
    for (size_t kk = 0; kk < nh; kk++) {
      const size_t h = lperm[kk];
      if ((int)h == idx_s1) {
        continue;
      }
      const bool final_only_h = (final_only >> h) & 1ull;
      if (final_only_h && li + 1 != layers) {
        continue;
      }
      const char * txt = final_only_h ? per[h][0] : per[h][li];
      const char * init = is_sh(c, d->ids[h]) ? NULL : initial_serialized(c, d->ids[h]);
      bool is_init = init && texteq(txt, init);
      if (is_bs && dfl[h] && (nm(c, d->ids[h], "box-shadow-blur") || nm(c, d->ids[h], "box-shadow-spread"))) {
        /* a blur or a spread the author wrote is written back, whatever it is (`0px` too); one that was left out is not */
        is_init = dfl[h][final_only_h ? 0 : li];
      }
      if (is_sh(c, d->ids[h])) {
        /* background-position's layer: initial when it is the initial of the sub-shorthand (0% 0%) */
        directs sd;
        get_directs(c, d->ids[h], &sd);
        is_init = true;
        const stext * t = stext_of(c, d->ids[h]);
        is_init = t && t->all_initial;
        if (t && t->items && t->nitems == layers) {
          /* the layer's text is initial when it is the text of the initial values of its longhands */
          GTEXT_CSS_Property_Info si;
          gtext_css_registry_property_info(c->reg, d->ids[h], &si);
          is_init = false;
          const char * ix = initial_serialized(c, sd.ids[0]);
          const char * iy = sd.nlong > 1 ? initial_serialized(c, sd.ids[1]) : NULL;
          if (ix && iy) {
            tbuf tmp;
            tb_init(&tmp, &c->alloc);
            tb_puts(&tmp, ix);
            tb_puts(&tmp, " ");
            tb_puts(&tmp, iy);
            is_init = (tmp.data && texteq(tmp.data, txt)) || texteq(ix, txt);
            tb_free(&tmp);
          }
        }
      }
      if (!is_init) {
        all_initial = false;
      }
      if (box_o >= 0 && ((int)h == box_o || (int)h == box_c)) {
        /* one <box> stands for the origin and the clip both; two are written when they differ, whatever they are */
        const bool eq = texteq(per[box_o][li], per[box_c][li]);
        const bool o_init = texteq(per[box_o][li], initial_serialized(c, d->ids[box_o]));
        const bool c_init = texteq(per[box_c][li], initial_serialized(c, d->ids[box_c]));
        if (eq) {
          if ((int)h == box_c || (o_init && c_init)) {
            continue;
          }
          is_init = false;
        }
        else if (o_init && c_init) {
          continue;
        }
        else if ((int)h == box_o && o_init && texteq(per[box_c][li], "no-clip")) {
          continue;   /* `no-clip` is no origin, and an initial origin is left to the default */
        }
        else {
          is_init = false;
        }
      }
      const bool follows = (int)h == idx_after;
      bool need_s1 = false;
      if (follows && idx_s1 >= 0) {
        const char * ii = initial_serialized(c, d->ids[idx_s1]);
        need_s1 = !(ii && texteq(per[idx_s1][li], ii));
      }
      if (is_init && !(follows && need_s1)) {
        continue;
      }
      if (any) {
        tb_puts(&lb, " ");
      }
      any = true;
      tb_puts(&lb, txt);
      if (need_s1) {
        tb_puts(&lb, " / ");
        tb_puts(&lb, per[idx_s1][li]);
      }
    }
    if (!any) {
      /* every component is initial: the one the draft names stands for them, else the first */
      tb_puts(&lb, per[lall_initial >= 0 ? (size_t)lall_initial : 0][li]);
    }
    layer_text[li] = tb_take(&lb, NULL);
    if (!layer_text[li]) {
      ok = false;
    }
  }
  if (ok) {
    for (size_t li = 0; li < layers; li++) {
      if (li) {
        tb_puts(&buf, ", ");
      }
      tb_puts(&buf, layer_text[li]);
    }
    out->text = tb_take(&buf, NULL);
    out->all_initial = all_initial;
    out->items = layer_text;
    out->nitems = layers;
    ok = out->text != NULL;
    if (!ok) {
      /* the text could not be joined (memory): the layers go with it */
      for (size_t li = 0; li < layers; li++) {
        gtext_allocator_free(&c->alloc, layer_text[li]);
      }
      gtext_allocator_free(&c->alloc, layer_text);
      out->items = NULL;
      out->nitems = 0;
    }
  }
  else {
    tb_free(&buf);
    for (size_t li = 0; layer_text && li < layers; li++) {
      gtext_allocator_free(&c->alloc, layer_text[li]);
    }
    gtext_allocator_free(&c->alloc, layer_text);
  }
  for (size_t h = 0; h < nh; h++) {
    if (per && per[h]) {
      if (is_sh(c, d->ids[h])) {
        gtext_allocator_free(&c->alloc, per[h]);   /* its items are the sub-shorthand's */
      }
      else {
        items_free(c, per[h], cnt[h]);
      }
    }
  }
  for (size_t h = 0; dfl && h < nh; h++) {
    gtext_allocator_free(&c->alloc, dfl[h]);
  }
  gtext_allocator_free(&c->alloc, dfl);
  gtext_allocator_free(&c->alloc, per);
  gtext_allocator_free(&c->alloc, cnt);
  return ok;
}

static bool radius_text(sctx * c, GTEXT_CSS_Property_Id sh, const directs * d, stext * out) {
  (void)sh;
  const size_t n = d->nlong;
  if (n != 4 && n != 2) {
    return false;
  }
  char *h[4] = {0}, *v[4] = {0};
  bool ok = true;
  for (size_t i = 0; i < n && ok; i++) {
    const uint32_t at = c->last[d->ids[i]];
    if (!at || c->b->items[at - 1].outcome != GTEXT_CSS_PROP_RESULT_VALID) {
      ok = false;
      break;
    }
    const GTEXT_CSS_Value * root = gtext_css_value_set_root(c->b->items[at - 1].set);
    const GTEXT_CSS_Value * hv = root;
    const GTEXT_CSS_Value * vv = root;
    if (root && root->kind == GTEXT_CSS_VAL_LIST && root->child_count >= 1) {
      hv = &root->children[0];
      vv = &root->children[root->child_count > 1 ? 1 : 0];
    }
    tbuf a, b2;
    tb_init(&a, &c->alloc);
    tb_init(&b2, &c->alloc);
    css_exact_text(&a, hv);
    css_exact_text(&b2, vv);
    h[i] = tb_take(&a, NULL);
    v[i] = tb_take(&b2, NULL);
    ok = h[i] && v[i];
  }
  tbuf buf;
  tb_init(&buf, &c->alloc);
  if (ok) {
    bool same = true;
    for (size_t i = 0; i < n; i++) {
      same = same && texteq(h[i], v[i]);
    }
    for (int pass = 0; pass < (same ? 1 : 2); pass++) {
      char ** a = pass == 0 ? h : v;
      if (pass == 1) {
        tb_puts(&buf, " / ");
      }
      tb_puts(&buf, a[0]);
      if (n == 2) {
        if (!texteq(a[1], a[0])) {
          tb_puts(&buf, " ");
          tb_puts(&buf, a[1]);
        }
        continue;
      }
      if (!texteq(a[3], a[1]) || !texteq(a[2], a[0]) || !texteq(a[1], a[0])) {
        tb_puts(&buf, " ");
        tb_puts(&buf, a[1]);
        if (!texteq(a[3], a[1]) || !texteq(a[2], a[0])) {
          tb_puts(&buf, " ");
          tb_puts(&buf, a[2]);
          if (!texteq(a[3], a[1])) {
            tb_puts(&buf, " ");
            tb_puts(&buf, a[3]);
          }
        }
      }
    }
    out->text = tb_take(&buf, NULL);
    ok = out->text != NULL;
    /* one radius for every corner is a value a shorthand that nests this one can write as itself */
    bool all_equal = same;
    for (size_t i = 1; i < n; i++) {
      all_equal = all_equal && texteq(h[i], h[0]);
    }
    out->uniform = all_equal;
    out->all_initial = all_equal && (texteq(h[0], "0px") || texteq(h[0], "0"));
  }
  else {
    tb_free(&buf);
  }
  for (size_t i = 0; i < 4; i++) {
    gtext_allocator_free(&c->alloc, h[i]);
    gtext_allocator_free(&c->alloc, v[i]);
  }
  return ok;
}

static bool flex_text(sctx * c, const directs * d, const lhv * v, stext * out) {
  if (d->nlong != 3) {
    return false;
  }
  const char * g = v[0].text;
  const char * s = v[1].text;
  const char * b = v[2].text;
  const char * res;
  tbuf buf;
  tb_init(&buf, &c->alloc);
  (void)res;
  /* the three values always: `none` is `0 0 auto` and `auto` is `1 1 auto` (Flexbox 1, 7.1) */
  tb_puts(&buf, g);
  tb_puts(&buf, " ");
  tb_puts(&buf, s);
  tb_puts(&buf, " ");
  tb_puts(&buf, b);
  out->text = tb_take(&buf, NULL);
  out->all_initial = v[0].initial && v[1].initial && v[2].initial;
  return out->text != NULL;
}

static bool plain_ident(const char * t) {
  if (!t || !t[0] || strcmp(t, "auto") == 0 || strcmp(t, "span") == 0) {
    return false;
  }
  for (const char * q = t; *q; q++) {
    const char c = *q;
    if (c == '\\' && q[1]) {
      /* an escape: hex digits and the one space that ends them, or any other character */
      q++;
      if ((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'f') || (*q >= 'A' && *q <= 'F')) {
        int digits = 1;
        while (digits < 6 && ((q[1] >= '0' && q[1] <= '9') || (q[1] >= 'a' && q[1] <= 'f') || (q[1] >= 'A' && q[1] <= 'F'))) {
          q++;
          digits++;
        }
        if (q[1] == ' ') {
          q++;
        }
      }
      continue;
    }
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-' || (unsigned char)c >= 0x80 || (q != t && c >= '0' && c <= '9');
    if (!ok) {
      return false;
    }
  }
  return !(t[0] >= '0' && t[0] <= '9') && !(t[0] == '-' && t[1] >= '0' && t[1] <= '9');
}

static bool grid_line_text(sctx * c, const directs * d, const lhv * v, stext * out) {
  const size_t n = d->nlong;
  if (n != 2 && n != 4) {
    return false;
  }
  /* the last lines are left out when writing them again would give them back: an identifier is copied, anything else is `auto` */
  size_t keep = n;
  if (n == 2) {
    const bool drop = (plain_ident(v[0].text) && texteq(v[1].text, v[0].text)) || (!plain_ident(v[0].text) && texteq(v[1].text, "auto"));
    keep = drop ? 1 : 2;
  }
  else {
    const char * rs = v[0].text;
    const char * cs = v[1].text;
    const char * re = v[2].text;
    const char * ce = v[3].text;
    const bool ce_drop = (plain_ident(cs) && texteq(ce, cs)) || (!plain_ident(cs) && texteq(ce, "auto"));
    const bool re_drop = (plain_ident(rs) && texteq(re, rs)) || (!plain_ident(rs) && texteq(re, "auto"));
    const bool cs_drop = (plain_ident(rs) && texteq(cs, rs)) || (!plain_ident(rs) && texteq(cs, "auto"));
    keep = 4;
    if (ce_drop) {
      keep = 3;
      if (re_drop) {
        keep = 2;
        if (cs_drop) {
          keep = 1;
        }
      }
    }
  }
  tbuf buf;
  tb_init(&buf, &c->alloc);
  for (size_t i = 0; i < keep; i++) {
    if (i) {
      tb_puts(&buf, " / ");
    }
    tb_puts(&buf, v[i].text);
  }
  out->text = tb_take(&buf, NULL);
  return out->text != NULL;
}

static bool grid_text(sctx * c, const directs * d, const lhv * v, bool whole, stext * out) {
  static const char * const names[6] = {"grid-template-rows", "grid-template-columns", "grid-template-areas", "grid-auto-rows", "grid-auto-columns",
      "grid-auto-flow"};
  const char * texts[6] = {NULL, NULL, NULL, NULL, NULL, NULL};
  for (size_t i = 0; i < d->nlong; i++) {
    GTEXT_CSS_Property_Info info;
    if (!gtext_css_registry_property_info(c->reg, d->ids[i], &info)) {
      return false;
    }
    for (int k = 0; k < 6; k++) {
      if (info.name_length == strlen(names[k]) && memcmp(info.name, names[k], info.name_length) == 0) {
        texts[k] = v[i].text;
      }
    }
  }
  for (int k = 0; k < (whole ? 6 : 3); k++) {
    if (!texts[k]) {
      return false;
    }
  }
  char * res = NULL;
  const css_grid_status st = whole ? css_grid_join(&c->alloc, texts, &res) : css_grid_template_join(&c->alloc, texts, &res);
  if (st == CSS_GRID_OOM) {
    c->status = GTEXT_CSS_E_OOM;
  }
  out->text = res;
  return res != NULL;
}

static bool offset_text(sctx * c, const directs * d, const lhv * v, stext * out) {
  const char *pos = NULL, *path = NULL, *dist = NULL, *rot = NULL, *anc = NULL;
  bool ipos = true, ipath = true, idist = true, irot = true, ianc = true;
  for (size_t i = 0; i < d->nlong; i++) {
    if (nm(c, d->ids[i], "offset-position")) { pos = v[i].text; ipos = v[i].initial; }
    else if (nm(c, d->ids[i], "offset-path")) { path = v[i].text; ipath = v[i].initial; }
    else if (nm(c, d->ids[i], "offset-distance")) { dist = v[i].text; idist = v[i].initial; }
    else if (nm(c, d->ids[i], "offset-rotate")) { rot = v[i].text; irot = v[i].initial; }
    else if (nm(c, d->ids[i], "offset-anchor")) { anc = v[i].text; ianc = v[i].initial; }
  }
  if (!pos || !path || !dist || !rot || !anc) {
    return false;
  }
  tbuf b;
  tb_init(&b, &c->alloc);
  bool any = false;
  if (!ipos) {
    tb_puts(&b, pos);
    any = true;
  }
  if (!ipath || !idist || !irot) {
    if (any) tb_puts(&b, " ");
    tb_puts(&b, path);
    any = true;
  }
  if (!idist) {
    tb_puts(&b, " ");
    tb_puts(&b, dist);
  }
  if (!irot) {
    tb_puts(&b, " ");
    tb_puts(&b, rot);
  }
  if (!any) {
    tb_puts(&b, "none");
  }
  if (!ianc) {
    tb_puts(&b, " / ");
    tb_puts(&b, anc);
  }
  out->all_initial = ipos && ipath && idist && irot && ianc;
  out->text = tb_take(&b, NULL);
  return out->text != NULL;
}

static bool text_align_text(sctx * c, const directs * d, const lhv * v, stext * out) {
  if (d->nlong != 2) {
    return false;
  }
  const char * all = v[0].text;
  const char * last = v[1].text;
  const char * res = NULL;
  if (texteq(last, "auto")) {
    res = all;
  }
  else if (texteq(all, "justify") && texteq(last, "justify")) {
    res = "justify-all";
  }
  if (!res) {
    return false;
  }
  out->text = dupn(c, res, strlen(res));
  return out->text != NULL;
}

static bool font_synthesis_text(sctx * c, const directs * d, const lhv * v, stext * out) {
  static const char * const kinds[] = {"weight", "style", "small-caps", "position"};
  tbuf buf;
  tb_init(&buf, &c->alloc);
  bool any = false;
  for (size_t k = 0; k < sizeof kinds / sizeof *kinds; k++) {
    const size_t kl = strlen(kinds[k]);
    for (size_t i = 0; i < d->nlong; i++) {
      GTEXT_CSS_Property_Info info;
      gtext_css_registry_property_info(c->reg, d->ids[i], &info);
      if (info.name_length > kl + 1 && memcmp(info.name + info.name_length - kl, kinds[k], kl) == 0 && info.name[info.name_length - kl - 1] == '-') {
        if (texteq(v[i].text, "auto")) {
          tb_puts(&buf, any ? " " : "");
          tb_puts(&buf, kinds[k]);
          any = true;
        }
        else if (!texteq(v[i].text, "none")) {
          tb_free(&buf);
          return false;
        }
      }
    }
  }
  if (!any) {
    tb_puts(&buf, "none");
  }
  out->text = tb_take(&buf, NULL);
  return out->text != NULL;
}

static bool font_text(sctx * c, GTEXT_CSS_Property_Id sh, const directs * d, stext * out) {
  /* the longhands by name; font-variant's own longhands must be initial except font-variant-caps: small-caps */
  const char * style = NULL;
  const char * weight = NULL;
  const char * width = NULL;
  const char * size = NULL;
  const char * lh = NULL;
  const char * family = NULL;
  char * texts[8] = {0};
  size_t nt = 0;
  bool small_caps = false;
  bool ok = true;
  for (size_t i = 0; i < d->nlong + d->nreset && ok; i++) {
    const GTEXT_CSS_Property_Id id = d->ids[i];
    if (is_sh(c, id)) {
      /* font-variant: its longhands */
      GTEXT_CSS_Property_Id lv[32];
      size_t nl = 0;
      if (!leaves_under(c, id, lv, 32, &nl)) {
        ok = false;
        break;
      }
      for (size_t k = 0; k < nl && ok; k++) {
        char * t = leaf_text(c, lv[k], NULL);
        if (!t) {
          ok = false;
          break;
        }
        if (nm(c, lv[k], "font-variant-caps")) {
          small_caps = texteq(t, "small-caps");
          ok = small_caps || texteq(t, "normal");
        }
        else {
          ok = texteq(t, initial_serialized(c, lv[k]));
        }
        gtext_allocator_free(&c->alloc, t);
      }
      continue;
    }
    char * t = leaf_text(c, id, NULL);
    if (!t) {
      ok = false;
      break;
    }
    if (nm(c, id, "font-style")) style = texts[nt++] = t;
    else if (nm(c, id, "font-weight")) weight = texts[nt++] = t;
    else if (nm(c, id, "font-width")) width = texts[nt++] = t;
    else if (nm(c, id, "font-size")) size = texts[nt++] = t;
    else if (nm(c, id, "line-height")) lh = texts[nt++] = t;
    else if (nm(c, id, "font-family")) family = texts[nt++] = t;
    else {
      /* a property the shorthand resets: it must be its initial value */
      ok = texteq(t, initial_serialized(c, id));
      gtext_allocator_free(&c->alloc, t);
    }
    if (nt >= 7) {
      ok = false;
    }
  }
  if (ok && (!size || !family)) {
    ok = false;
  }
  tbuf buf;
  tb_init(&buf, &c->alloc);
  if (ok) {
    bool any = false;
    if (style && !texteq(style, "normal")) {
      tb_puts(&buf, style);
      any = true;
    }
    if (small_caps) {
      tb_puts(&buf, any ? " small-caps" : "small-caps");
      any = true;
    }
    if (weight && !texteq(weight, "normal")) {
      tb_puts(&buf, any ? " " : "");
      tb_puts(&buf, weight);
      any = true;
    }
    if (width && !texteq(width, "normal")) {
      tb_puts(&buf, any ? " " : "");
      tb_puts(&buf, width);
      any = true;
    }
    tb_puts(&buf, any ? " " : "");
    tb_puts(&buf, size);
    if (lh && !texteq(lh, "normal")) {
      tb_puts(&buf, "/");
      tb_puts(&buf, lh);
    }
    tb_puts(&buf, " ");
    tb_puts(&buf, family);
    out->text = tb_take(&buf, NULL);
    ok = out->text != NULL;
  }
  else {
    tb_free(&buf);
  }
  for (size_t i = 0; i < nt; i++) {
    gtext_allocator_free(&c->alloc, texts[i]);
  }
  (void)sh;
  return ok;
}

/// Compute one shorthand's text from the block; `t` is filled (text NULL when its longhands make none).
static void compute_stext(sctx * c, GTEXT_CSS_Property_Id sh, stext * t) {
  memset(t, 0, sizeof *t);
  t->sh = sh;
  directs d;
  if (!get_directs(c, sh, &d)) {
    return;
  }
  const GTEXT_CSS_Shorthand_Kind kind = gtext_css_shorthand_kind(c->reg, sh);
  lhv v[MAX_DIRECT * 2];
  size_t nv = d.nlong + d.nreset;
  if (kind == GTEXT_CSS_SHORTHAND_FONT) {
    font_text(c, sh, &d, t);
    return;
  }
  if (kind == GTEXT_CSS_SHORTHAND_BORDER_RADIUS) {
    radius_text(c, sh, &d, t);
    return;
  }
  if (kind == GTEXT_CSS_SHORTHAND_LAYERS) {
    layers_text(c, sh, &d, t);
    return;
  }
  const bool filled = lhv_fill(c, &d, v);
  if (filled) {
    switch (kind) {
    case GTEXT_CSS_SHORTHAND_BOX: box_text(c, v, d.nlong, t); break;
    case GTEXT_CSS_SHORTHAND_PAIR: pair_text(c, v, d.nlong, t); break;
    case GTEXT_CSS_SHORTHAND_COPY: copy_text(c, v, d.nlong, t); break;
    case GTEXT_CSS_SHORTHAND_ANY: any_text(c, sh, &d, v, t); break;
    case GTEXT_CSS_SHORTHAND_FLEX: flex_text(c, &d, v, t); break;
    case GTEXT_CSS_SHORTHAND_GRID_LINE: grid_line_text(c, &d, v, t); break;
    case GTEXT_CSS_SHORTHAND_OFFSET: offset_text(c, &d, v, t); break;
    case GTEXT_CSS_SHORTHAND_GRID_TEMPLATE: grid_text(c, &d, v, false, t); break;
    case GTEXT_CSS_SHORTHAND_GRID: grid_text(c, &d, v, true, t); break;
    case GTEXT_CSS_SHORTHAND_TEXT_ALIGN: text_align_text(c, &d, v, t); break;
    case GTEXT_CSS_SHORTHAND_FONT_SYNTHESIS: font_synthesis_text(c, &d, v, t); break;
    default: break;
    }
    /* what the shorthand resets must be its initial value, or the shorthand cannot say what the block says */
    for (size_t i = d.nlong; t->text && i < nv; i++) {
      if (!v[i].initial) {
        gtext_allocator_free(&c->alloc, t->text);
        t->text = NULL;
      }
    }
  }
  lhv_free(c, v, nv);
}

/* ---- the entry points ----------------------------------------------------------------------------------------------------- */

/// The text of a shorthand from a block: CSS-wide keyword, pending, or its kind's. `out` is a new string (the default allocator's, for the buffer
/// release of css_serialize.h), empty when there is none.
static GTEXT_CSS_Status shorthand_text_in(sctx * c, GTEXT_CSS_Property_Id sh, bool * all_wide, GTEXT_CSS_Wide_Keyword * wide, char ** out,
    size_t * out_len, bool * important, GTEXT_CSS_Shorthand_Result * result) {
  (void)result;
  *out = NULL;
  *out_len = 0;
  *all_wide = false;
  GTEXT_CSS_Property_Id leaves[256];
  size_t nleaf = 0;
  const GTEXT_Allocator * da = gtext_allocator_default();
  GTEXT_Allocator dflt = *da;
  char * empty = gtext_allocator_malloc(&dflt, 1);
  if (!empty) {
    return GTEXT_CSS_E_OOM;
  }
  empty[0] = '\0';
  if (!leaves_under(c, sh, leaves, 256, &nleaf) || nleaf == 0) {
    *out = empty;
    return GTEXT_CSS_OK;
  }
  /* every leaf present, with one importance, and of one sort */
  const blk_item * first = NULL;
  bool same_wide = true, same_pending = true, any_valid = false;
  for (size_t i = 0; i < nleaf; i++) {
    const uint32_t at = leaves[i] <= c->nprops ? c->last[leaves[i]] : 0;
    if (!at) {
      *out = empty;
      return GTEXT_CSS_OK;
    }
    const blk_item * it = &c->b->items[at - 1];
    if (!first) {
      first = it;
    }
    if (it->important != first->important) {
      *out = empty;
      return GTEXT_CSS_OK;
    }
    same_wide = same_wide && it->outcome == GTEXT_CSS_PROP_RESULT_WIDE && it->wide == first->wide && first->outcome == GTEXT_CSS_PROP_RESULT_WIDE;
    same_pending = same_pending && it->outcome == GTEXT_CSS_PROP_RESULT_PENDING && first->outcome == GTEXT_CSS_PROP_RESULT_PENDING &&
        it->shorthand == first->shorthand && it->declaration == first->declaration && it->shorthand != 0;
    any_valid = any_valid || it->outcome == GTEXT_CSS_PROP_RESULT_VALID;
  }
  if (important) {
    *important = first->important;
  }
  if (same_wide) {
    const char * k = gtext_css_wide_keyword_name(first->wide);
    *all_wide = true;
    *wide = first->wide;
    gtext_allocator_free(&dflt, empty);
    char * d = gtext_allocator_malloc(&dflt, strlen(k) + 1);
    if (!d) {
      return GTEXT_CSS_E_OOM;
    }
    memcpy(d, k, strlen(k) + 1);
    *out = d;
    *out_len = strlen(k);
    return GTEXT_CSS_OK;
  }
  if (same_pending && first->shorthand == sh) {
    const GTEXT_CSS_Value * root = gtext_css_value_set_root(first->set);
    const char * txt = root && root->text.data ? root->text.data : "";
    gtext_allocator_free(&dflt, empty);
    char * d = gtext_allocator_malloc(&dflt, strlen(txt) + 1);
    if (!d) {
      return GTEXT_CSS_E_OOM;
    }
    memcpy(d, txt, strlen(txt) + 1);
    *out = d;
    *out_len = strlen(txt);
    return GTEXT_CSS_OK;
  }
  if (!any_valid || gtext_css_shorthand_kind(c->reg, sh) == GTEXT_CSS_SHORTHAND_UNIMPLEMENTED) {
    *out = empty;
    return GTEXT_CSS_OK;
  }
  /* the nested shorthands first, then this one */
  GTEXT_CSS_Property_Id order[64];
  size_t norder = 0;
  if (!eval_order(c, sh, order, 64, &norder)) {
    *out = empty;
    return GTEXT_CSS_OK;
  }
  stext * last_t = NULL;
  for (size_t i = 0; i < norder; i++) {
    if (c->ntexts == c->captexts) {
      const size_t cap = c->captexts ? c->captexts * 2 : 8;
      stext * grown = gtext_allocator_realloc(&c->alloc, c->texts, cap * sizeof *grown);
      if (!grown) {
        gtext_allocator_free(&dflt, empty);
        return GTEXT_CSS_E_OOM;
      }
      c->texts = grown;
      c->captexts = cap;
    }
    stext * t = &c->texts[c->ntexts++];
    compute_stext(c, order[i], t);
    last_t = t;
    if (c->status != GTEXT_CSS_OK) {
      gtext_allocator_free(&dflt, empty);
      return c->status;
    }
    if (!t->text && i + 1 < norder) {
      *out = empty;
      return GTEXT_CSS_OK;
    }
  }
  if (!last_t || !last_t->text) {
    *out = empty;
    return GTEXT_CSS_OK;
  }
  gtext_allocator_free(&dflt, empty);
  const size_t n = strlen(last_t->text);
  char * d = gtext_allocator_malloc(&dflt, n + 1);
  if (!d) {
    return GTEXT_CSS_E_OOM;
  }
  memcpy(d, last_t->text, n + 1);
  *out = d;
  *out_len = n;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_block_shorthand_text(const GTEXT_CSS_Declaration_Block * block, GTEXT_CSS_Property_Id shorthand, bool * all_wide,
    GTEXT_CSS_Wide_Keyword * wide, char ** out, size_t * out_len, bool * important, GTEXT_CSS_Shorthand_Result * result) {
  sctx c;
  if (!sctx_init(&c, block)) {
    sctx_free(&c);
    return GTEXT_CSS_E_OOM;
  }
  GTEXT_CSS_Status st = shorthand_text_in(&c, shorthand, all_wide, wide, out, out_len, important, result);
  if (st == GTEXT_CSS_OK && c.status != GTEXT_CSS_OK) {
    if (*out) {
      GTEXT_Allocator dflt = *gtext_allocator_default();
      gtext_allocator_free(&dflt, *out);
      *out = NULL;
      *out_len = 0;
    }
    st = GTEXT_CSS_E_OOM;
  }
  sctx_free(&c);
  return st;
}

/* ---- cssText ------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  GTEXT_CSS_Property_Id sh;
  size_t leaves;
} cand;

static int cand_cmp(const void * a, const void * b) {
  const cand * x = a;
  const cand * y = b;
  if (x->leaves != y->leaves) {
    return x->leaves > y->leaves ? -1 : 1;
  }
  return x->sh < y->sh ? -1 : x->sh > y->sh ? 1 : 0;
}

static void put_declaration(tbuf * out, const char * name, size_t name_len, const char * value, size_t value_len, bool important, bool first) {
  if (!first) {
    tb_puts(out, " ");
  }
  tb_put(out, name, name_len);
  tb_puts(out, ": ");
  tb_put(out, value, value_len);
  if (important) {
    tb_puts(out, " !important");
  }
  tb_puts(out, ";");
}

GTEXT_CSS_Status css_block_text(const GTEXT_CSS_Declaration_Block * block, tbuf * out) {
  sctx c;
  if (!sctx_init(&c, block)) {
    sctx_free(&c);
    return GTEXT_CSS_E_OOM;
  }
  const size_t n = block->count;
  unsigned char * done = gtext_allocator_calloc(&c.alloc, n ? n : 1, 1);
  if (!done) {
    sctx_free(&c);
    return GTEXT_CSS_E_OOM;
  }
  /* the shorthands each property is under: built once, for the properties the block has */
  const size_t np = c.nprops;
  size_t nshort = 0;
  GTEXT_CSS_Property_Id * shorts = gtext_allocator_calloc(&c.alloc, np ? np : 1, sizeof *shorts);
  if (!shorts) {
    gtext_allocator_free(&c.alloc, done);
    sctx_free(&c);
    return GTEXT_CSS_E_OOM;
  }
  for (size_t id = 1; id <= np; id++) {
    GTEXT_CSS_Property_Info info;
    gtext_css_registry_property_info(c.reg, (GTEXT_CSS_Property_Id)id, &info);
    if ((info.flags & GTEXT_CSS_PROP_SHORTHAND) && !(info.flags & GTEXT_CSS_PROP_ALIAS) && !(info.name_length == 3 && memcmp(info.name, "all", 3) == 0) &&
        gtext_css_shorthand_kind(c.reg, (GTEXT_CSS_Property_Id)id) != GTEXT_CSS_SHORTHAND_UNIMPLEMENTED) {
      shorts[nshort++] = (GTEXT_CSS_Property_Id)id;
    }
  }
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  bool first = true;
  cand cands[128];
  for (size_t i = 0; i < n && status == GTEXT_CSS_OK; i++) {
    if (done[i]) {
      continue;
    }
    const blk_item * it = &block->items[i];
    if (it->custom_name) {
      /* the last declaration of a custom property of this name counts */
      bool shadowed = false;
      for (size_t k = i + 1; k < n && !shadowed; k++) {
        shadowed = block->items[k].custom_name && block->items[k].custom_length == it->custom_length &&
            memcmp(block->items[k].custom_name, it->custom_name, it->custom_length) == 0;
      }
      if (shadowed) {
        continue;
      }
      char * t = NULL;
      size_t tl = 0;
      if (it->outcome == GTEXT_CSS_PROP_RESULT_VALID) {
        const GTEXT_CSS_Value * root = gtext_css_value_set_root(it->set);
        if (root && root->text.data) {
          t = dupn(&c, root->text.data, root->text.length);
          tl = root->text.length;
        }
        else {
          t = dupn(&c, "", 0);
        }
      }
      else {
        status = css_item_text(c.reg, &c.alloc, it->property, it->outcome, it->wide, it->set, &t, &tl);
      }
      if (status == GTEXT_CSS_OK && t) {
        put_declaration(out, it->custom_name, it->custom_length, t, tl, it->important, first);
        first = false;
      }
      gtext_allocator_free(&c.alloc, t);
      continue;
    }
    if (it->property > np || c.last[it->property] != (uint32_t)(i + 1)) {
      continue;   /* an earlier declaration of a property that is declared again */
    }
    /* shorthands that include this property, fewest leaves last */
    size_t ncand = 0;
    for (size_t k = 0; k < nshort && ncand < 128; k++) {
      GTEXT_CSS_Property_Id lv[256];
      size_t nl = 0;
      if (!leaves_under(&c, shorts[k], lv, 256, &nl)) {
        continue;
      }
      bool has = false;
      for (size_t m = 0; m < nl && !has; m++) {
        has = lv[m] == it->property;
      }
      if (has) {
        cands[ncand].sh = shorts[k];
        cands[ncand].leaves = nl;
        ncand++;
      }
    }
    qsort(cands, ncand, sizeof *cands, cand_cmp);
    bool written = false;
    for (size_t k = 0; k < ncand && !written && status == GTEXT_CSS_OK; k++) {
      /* a shorthand that holds a longhand already written would write it twice */
      {
        GTEXT_CSS_Property_Id lv0[256];
        size_t nl0 = 0;
        bool overlaps = false;
        if (leaves_under(&c, cands[k].sh, lv0, 256, &nl0)) {
          for (size_t m = 0; m < nl0 && !overlaps; m++) {
            const uint32_t at = lv0[m] <= np ? c.last[lv0[m]] : 0;
            overlaps = at && done[at - 1];
          }
        }
        if (overlaps) {
          continue;
        }
      }
      bool all_wide = false;
      GTEXT_CSS_Wide_Keyword wide = GTEXT_CSS_WIDE_NONE;
      char * text = NULL;
      size_t tl = 0;
      bool imp = false;
      status = shorthand_text_in(&c, cands[k].sh, &all_wide, &wide, &text, &tl, &imp, NULL);
      if (status != GTEXT_CSS_OK) {
        break;
      }
      if (tl > 0) {
        GTEXT_CSS_Property_Info si;
        gtext_css_registry_property_info(c.reg, cands[k].sh, &si);
        put_declaration(out, si.name, si.name_length, text, tl, imp, first);
        first = false;
        written = true;
        GTEXT_CSS_Property_Id lv[256];
        size_t nl = 0;
        leaves_under(&c, cands[k].sh, lv, 256, &nl);
        for (size_t m = 0; m < nl; m++) {
          for (size_t q = 0; q < n; q++) {
            if (block->items[q].property == lv[m] && !block->items[q].custom_name) {
              done[q] = 1;
            }
          }
        }
      }
      gtext_allocator_free(gtext_allocator_default(), text);
    }
    if (written || status != GTEXT_CSS_OK) {
      continue;
    }
    GTEXT_CSS_Property_Info info;
    gtext_css_registry_property_info(c.reg, it->property, &info);
    char * t = NULL;
    size_t tl = 0;
    if (it->outcome == GTEXT_CSS_PROP_RESULT_PENDING && it->shorthand) {
      /* a longhand left pending by a shorthand that holds `var()` (and not written as that shorthand, one of its longhands having been replaced):
         its own text is not a value of it, so CSSOM writes it empty */
      t = dupn(&c, "", 0);
    }
    else {
      status = css_item_text(c.reg, &c.alloc, it->property, it->outcome, it->wide, it->set, &t, &tl);
    }
    if (status == GTEXT_CSS_OK && t) {
      if (it->outcome == GTEXT_CSS_PROP_RESULT_PENDING && !it->shorthand) {
        tl = strlen(t);
      }
      put_declaration(out, info.name, info.name_length, t, tl, it->important, first);
      first = false;
    }
    gtext_allocator_free(&c.alloc, t);
  }
  if ((out->oom || c.status != GTEXT_CSS_OK) && status == GTEXT_CSS_OK) {
    status = GTEXT_CSS_E_OOM;   /* an allocation failed somewhere that had no way to say so: the text is not the block's */
  }
  gtext_allocator_free(&c.alloc, shorts);
  gtext_allocator_free(&c.alloc, done);
  sctx_free(&c);
  return status;
}
