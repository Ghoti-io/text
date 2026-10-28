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
 * The properties whose computed value the specification writes as prose (css_computed.h, GTEXT_CSS_COMPUTE_ALGORITHM). Each function here is the
 * sentence of the specification quoted in tools/css/computed_rules.tsv, as code. They read the earlier properties of the element and the
 * parent's style through css_st_get() and css_st_parent_root(), which trap a read of a property that is not yet computed.
 *
 * A computed value is a new set that points into the sets it was made from (the specified value, or the result of computing it by its types) and
 * keeps them alive, so a value that is mostly the specified one costs the nodes that differ.
 */

#include <string.h>

#include <ghoti.io/math/math.h>

#include "css_compute_internal.h"

/* ---- building -------------------------------------------------------------------------------------------- */

typedef struct {
  css_state * st;
  css_vb vb;
} ab;

static GTEXT_CSS_Status ab_open(ab * a, css_state * st) {
  a->st = st;
  if (!css_vb_begin(&a->vb, &st->cc->alloc, st->cc->options.max_value_nodes)) {
    return css_st_fail(st, a->vb.status, NULL, NULL);
  }
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status ab_close(ab * a, const GTEXT_CSS_Value * root, const GTEXT_CSS_Value_Set * dep1, const GTEXT_CSS_Value_Set * dep2,
    GTEXT_CSS_Value_Set ** out) {
  if (a->vb.status == GTEXT_CSS_OK && dep1) {
    css_vb_dep(&a->vb, dep1);
  }
  if (a->vb.status == GTEXT_CSS_OK && dep2) {
    css_vb_dep(&a->vb, dep2);
  }
  if (a->vb.status != GTEXT_CSS_OK) {
    const GTEXT_CSS_Status s = a->vb.status;
    css_vb_abort(&a->vb);
    return css_st_fail(a->st, s, NULL, NULL);
  }
  *out = css_vb_finish(&a->vb, root);
  return *out ? GTEXT_CSS_OK : css_st_fail(a->st, GTEXT_CSS_E_OOM, NULL, NULL);
}

static GTEXT_CSS_Status bytype(css_state * st, GTEXT_CSS_Property_Id id, const GTEXT_CSS_Value_Set * src, GTEXT_CSS_Value_Set ** out) {
  return css_cv_by_type(st, src, id, out);
}

/// A set holding one keyword.
static GTEXT_CSS_Status emit_keyword(css_state * st, const char * word, GTEXT_CSS_Value_Set ** out) {
  ab a;
  GTEXT_CSS_Status s = ab_open(&a, st);
  if (s != GTEXT_CSS_OK) {
    return s;
  }
  GTEXT_CSS_Value * v = css_vb_value(&a.vb);
  if (v) {
    css_v_keyword(&a.vb, v, word);
  }
  return ab_close(&a, v, NULL, NULL, out);
}

static GTEXT_CSS_Status emit_dimension(css_state * st, double number, GTEXT_CSS_Unit unit, GTEXT_CSS_Value_Set ** out) {
  ab a;
  GTEXT_CSS_Status s = ab_open(&a, st);
  if (s != GTEXT_CSS_OK) {
    return s;
  }
  GTEXT_CSS_Value * v = css_vb_value(&a.vb);
  if (v) {
    css_v_dimension(v, number, unit);
  }
  return ab_close(&a, v, NULL, NULL, out);
}

static GTEXT_CSS_Status emit_number(css_state * st, double number, bool integer, GTEXT_CSS_Value_Set ** out) {
  ab a;
  GTEXT_CSS_Status s = ab_open(&a, st);
  if (s != GTEXT_CSS_OK) {
    return s;
  }
  GTEXT_CSS_Value * v = css_vb_value(&a.vb);
  if (v) {
    if (integer) {
      css_v_integer(v, number);
    }
    else {
      css_v_number(v, number);
    }
  }
  return ab_close(&a, v, NULL, NULL, out);
}

static GTEXT_CSS_Status emit_percentage(css_state * st, double number, GTEXT_CSS_Value_Set ** out) {
  ab a;
  GTEXT_CSS_Status s = ab_open(&a, st);
  if (s != GTEXT_CSS_OK) {
    return s;
  }
  GTEXT_CSS_Value * v = css_vb_value(&a.vb);
  if (v) {
    css_v_percentage(v, number);
  }
  return ab_close(&a, v, NULL, NULL, out);
}

static bool is_number(const GTEXT_CSS_Value * v) {
  return v && (v->kind == GTEXT_CSS_VAL_NUMBER || v->kind == GTEXT_CSS_VAL_INTEGER) && !(v->flags & GTEXT_CSS_VFLAG_MATH);
}

static bool is_px(const GTEXT_CSS_Value * v) {
  return v && v->kind == GTEXT_CSS_VAL_DIMENSION && v->unit == GTEXT_CSS_UNIT_PX && !(v->flags & GTEXT_CSS_VFLAG_MATH);
}

static bool is_math(const GTEXT_CSS_Value * v) {
  return v && (v->flags & GTEXT_CSS_VFLAG_MATH) && v->math;
}

static bool is_pct(const GTEXT_CSS_Value * v) {
  return v && v->kind == GTEXT_CSS_VAL_PERCENTAGE;
}

/// Compare a keyword with a lower-case word.
static bool kw(const GTEXT_CSS_Value * v, const char * word) {
  return css_val_is_keyword(v, word);
}

/* ---- calculations resolved against a basis ------------------------------------------------------------------ */

typedef struct {
  css_state * st;
} unit_ctx;

static bool unit_size_cb(void * user, GTEXT_CSS_Unit unit, double * canonical) {
  unit_ctx * c = user;
  return css_st_unit_px(c->st, unit, canonical);
}

/// The value of a calculation in px, with a percentage resolved against `basis` (Values 4, 10.11); false (and the failure set) when it cannot be.
static bool resolve_math_px(css_state * st, const GTEXT_CSS_Value * v, double basis, double * px) {
  unit_ctx uc = {st};
  GTEXT_CSS_Math_Env env;
  memset(&env, 0, sizeof env);
  env.unit_size = unit_size_cb;
  env.user = &uc;
  env.has_percent_basis = true;
  env.percent_basis = basis;
  bool resolved = false;
  double value = 0.0;
  GTEXT_Allocator alloc = st->cc->alloc;
  GTEXT_CSS_Status s = gtext_css_math_resolve(v->math, &env, &alloc, &value, &resolved);
  if (s != GTEXT_CSS_OK) {
    css_st_fail(st, s, NULL, NULL);
    return false;
  }
  if (!resolved) {
    if (st->status == GTEXT_CSS_OK) {
      css_st_fail(st, GTEXT_CSS_E_UNSUPPORTED, NULL, "a calculation holds something this environment cannot resolve");
    }
    return false;
  }
  *px = gtext_css_math_clamp(value, v->range_lo, v->range_hi);
  return true;
}

/* ---- the font properties -------------------------------------------------------------------------------------- */

static GTEXT_CSS_Status algo_font_size(css_state * st, GTEXT_CSS_Property_Id id, const GTEXT_CSS_Value_Set * src, GTEXT_CSS_Value_Set ** out) {
  static const struct { const char * word; double factor; } table[] = {
    {"xx-small", 3.0 / 5.0}, {"x-small", 3.0 / 4.0}, {"small", 8.0 / 9.0}, {"medium", 1.0},
    {"large", 6.0 / 5.0}, {"x-large", 3.0 / 2.0}, {"xx-large", 2.0}, {"xxx-large", 3.0},
  };
  const GTEXT_CSS_Value * v = gtext_css_value_set_root(src);
  const double parent = css_st_parent_font_px(st);
  double px = 0.0;
  if (v && v->kind == GTEXT_CSS_VAL_KEYWORD) {
    bool found = false;
    for (size_t i = 0; i < sizeof table / sizeof *table; i++) {
      if (kw(v, table[i].word)) {
        px = table[i].factor * st->env->default_font_size;
        found = true;
        break;
      }
    }
    if (!found) {
      if (kw(v, "larger")) {
        px = parent * 1.2;
      }
      else if (kw(v, "smaller")) {
        px = parent / 1.2;
      }
      else {
        return css_st_fail(st, GTEXT_CSS_E_UNSUPPORTED, "font-size: math",
            "the scaling rules of font-size: math need the math-depth of the element and the metrics of a math font");
      }
    }
  }
  else if (is_pct(v)) {
    px = v->number / 100.0 * parent;
  }
  else if (is_math(v) && v->math->has_percent) {
    if (!resolve_math_px(st, v, parent, &px)) {
      return st->status;
    }
  }
  else {
    GTEXT_CSS_Value_Set * bt = NULL;
    GTEXT_CSS_Status s = bytype(st, id, src, &bt);
    if (s != GTEXT_CSS_OK) {
      return s;
    }
    const GTEXT_CSS_Value * b = gtext_css_value_set_root(bt);
    if (!is_px(b)) {
      gtext_css_value_set_free(bt);
      return css_st_fail(st, GTEXT_CSS_E_UNSUPPORTED, NULL, "a font-size that is not a keyword, a percentage or a length");
    }
    *out = bt;
    return GTEXT_CSS_OK;
  }
  if (px < 0.0) {
    px = 0.0;
  }
  return emit_dimension(st, px, GTEXT_CSS_UNIT_PX, out);
}

static GTEXT_CSS_Status algo_font_weight(css_state * st, GTEXT_CSS_Property_Id id, const GTEXT_CSS_Value_Set * src, GTEXT_CSS_Value_Set ** out) {
  const GTEXT_CSS_Value * v = gtext_css_value_set_root(src);
  if (is_number(v) || is_math(v)) {
    GTEXT_CSS_Value_Set * bt = NULL;
    GTEXT_CSS_Status s = bytype(st, id, src, &bt);
    if (s != GTEXT_CSS_OK) {
      return s;
    }
    const GTEXT_CSS_Value * b = gtext_css_value_set_root(bt);
    if (b && b->kind == GTEXT_CSS_VAL_NUMBER && b->number >= 1.0 && b->number <= 1000.0) {
      *out = bt;
      return GTEXT_CSS_OK;
    }
    const double n = b && (b->kind == GTEXT_CSS_VAL_NUMBER || b->kind == GTEXT_CSS_VAL_INTEGER) ? b->number : 400.0;
    gtext_css_value_set_free(bt);
    return emit_number(st, n < 1.0 ? 1.0 : n > 1000.0 ? 1000.0 : n, false, out);
  }
  if (kw(v, "normal")) {
    return emit_number(st, 400.0, false, out);
  }
  if (kw(v, "bold")) {
    return emit_number(st, 700.0, false, out);
  }
  /* bolder and lighter against the inherited weight (Fonts 4, 2.2) */
  const GTEXT_CSS_Value * pv = css_st_parent_root(st, id);
  const double w = pv && (pv->kind == GTEXT_CSS_VAL_NUMBER || pv->kind == GTEXT_CSS_VAL_INTEGER) ? pv->number : 400.0;
  double r;
  if (kw(v, "bolder")) {
    r = w < 350 ? 400 : w < 550 ? 700 : w < 750 ? 900 : w < 900 ? 900 : w;
  }
  else if (kw(v, "lighter")) {
    r = w < 100 ? w : w < 550 ? 100 : w < 750 ? 400 : 700;
  }
  else {
    return css_st_fail(st, GTEXT_CSS_E_UNSUPPORTED, NULL, "a font-weight that is not a number or a keyword");
  }
  return emit_number(st, r, false, out);
}

static GTEXT_CSS_Status algo_font_width(css_state * st, GTEXT_CSS_Property_Id id, const GTEXT_CSS_Value_Set * src, GTEXT_CSS_Value_Set ** out) {
  static const struct { const char * word; double pct; } table[] = {
    {"ultra-condensed", 50.0}, {"extra-condensed", 62.5}, {"condensed", 75.0}, {"semi-condensed", 87.5}, {"normal", 100.0},
    {"semi-expanded", 112.5}, {"expanded", 125.0}, {"extra-expanded", 150.0}, {"ultra-expanded", 200.0},
  };
  const GTEXT_CSS_Value * v = gtext_css_value_set_root(src);
  if (v && v->kind == GTEXT_CSS_VAL_KEYWORD) {
    for (size_t i = 0; i < sizeof table / sizeof *table; i++) {
      if (kw(v, table[i].word)) {
        return emit_percentage(st, table[i].pct, out);
      }
    }
    return css_st_fail(st, GTEXT_CSS_E_UNSUPPORTED, NULL, "a font-width keyword that is not in the table");
  }
  return bytype(st, id, src, out);
}

static GTEXT_CSS_Status algo_font_style(css_state * st, GTEXT_CSS_Property_Id id, const GTEXT_CSS_Value_Set * src, GTEXT_CSS_Value_Set ** out) {
  const GTEXT_CSS_Value * v = gtext_css_value_set_root(src);
  if (!v || v->kind != GTEXT_CSS_VAL_LIST || v->child_count == 0 || !kw(&v->children[0], "oblique")) {
    *out = gtext_css_value_set_retain(src);
    return GTEXT_CSS_OK;
  }
  /* Fonts 4: "the keyword specified, plus angle in degrees if specified": a bare `oblique` stays the keyword (the angle it stands for, 14deg, is the
     consumer's) */
  if (v->child_count < 2) {
    *out = gtext_css_value_set_retain(src);
    return GTEXT_CSS_OK;
  }
  double deg = 14.0;
  if (v->child_count >= 2) {
    GTEXT_CSS_Value_Set * bt = NULL;
    GTEXT_CSS_Status s = bytype(st, id, src, &bt);
    if (s != GTEXT_CSS_OK) {
      return s;
    }
    const GTEXT_CSS_Value * b = gtext_css_value_set_root(bt);
    if (b && b->kind == GTEXT_CSS_VAL_LIST && b->child_count >= 2 && b->children[1].kind == GTEXT_CSS_VAL_DIMENSION) {
      deg = b->children[1].number;
    }
    gtext_css_value_set_free(bt);
  }
  if (deg < -90.0) {
    deg = -90.0;
  }
  if (deg > 90.0) {
    deg = 90.0;
  }
  ab a;
  GTEXT_CSS_Status s = ab_open(&a, st);
  if (s != GTEXT_CSS_OK) {
    return s;
  }
  GTEXT_CSS_Value * kids = css_vb_values(&a.vb, 2);
  GTEXT_CSS_Value * root = css_vb_value(&a.vb);
  if (kids && root) {
    css_v_keyword(&a.vb, &kids[0], "oblique");
    css_v_dimension(&kids[1], deg, GTEXT_CSS_UNIT_DEG);
    css_v_list(root, kids, 2, GTEXT_CSS_SEP_SPACE);
  }
  return ab_close(&a, root, NULL, NULL, out);
}

static GTEXT_CSS_Status algo_line_height(css_state * st, GTEXT_CSS_Property_Id id, const GTEXT_CSS_Value_Set * src, GTEXT_CSS_Value_Set ** out) {
  const GTEXT_CSS_Value * v = gtext_css_value_set_root(src);
  const double font = css_st_em(st);
  if (is_math(v) && v->math->has_percent) {
    double px = 0;
    if (!resolve_math_px(st, v, font, &px)) {
      return st->status;
    }
    return emit_dimension(st, px, GTEXT_CSS_UNIT_PX, out);
  }
  GTEXT_CSS_Value_Set * bt = NULL;
  GTEXT_CSS_Status s = bytype(st, id, src, &bt);
  if (s != GTEXT_CSS_OK) {
    return s;
  }
  const GTEXT_CSS_Value * b = gtext_css_value_set_root(bt);
  if (is_pct(b)) {
    const double px = b->number / 100.0 * font;
    gtext_css_value_set_free(bt);
    return emit_dimension(st, px, GTEXT_CSS_UNIT_PX, out);
  }
  *out = bt;
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status algo_font_size_adjust(css_state * st, GTEXT_CSS_Property_Id id, const GTEXT_CSS_Value_Set * src, GTEXT_CSS_Value_Set ** out) {
  const GTEXT_CSS_Value * v = gtext_css_value_set_root(src);
  GTEXT_CSS_Value_Set * bt = NULL;
  GTEXT_CSS_Status s = bytype(st, id, src, &bt);
  if (s != GTEXT_CSS_OK) {
    return s;
  }
  const GTEXT_CSS_Value * b = gtext_css_value_set_root(bt);
  if (b && b->kind == GTEXT_CSS_VAL_LIST && b->child_count == 1) {
    b = &b->children[0];
  }
  if (!is_number(b) || v == NULL) {
    *out = bt;
    return GTEXT_CSS_OK;
  }
  /* a lone number is `ex-height` and the number (Fonts 5) */
  ab a;
  s = ab_open(&a, st);
  if (s != GTEXT_CSS_OK) {
    gtext_css_value_set_free(bt);
    return s;
  }
  GTEXT_CSS_Value * kids = css_vb_values(&a.vb, 2);
  GTEXT_CSS_Value * root = css_vb_value(&a.vb);
  if (kids && root) {
    css_v_keyword(&a.vb, &kids[0], "ex-height");
    kids[1] = *b;
    css_v_list(root, kids, 2, GTEXT_CSS_SEP_SPACE);
  }
  s = ab_close(&a, root, bt, NULL, out);
  gtext_css_value_set_free(bt);
  return s;
}

/* ---- widths ---------------------------------------------------------------------------------------------- */

static double snap_border(css_state * st, double px) {
  if (!(px > 0.0)) {
    return 0.0;
  }
  const double dpr = st->env->device_pixel_ratio;
  double dev = px * dpr;
  if (dev < 1.0) {
    dev = 1.0;
  }
  else {
    dev = gmath_floor(dev); /* down to a whole device pixel (a width of 2^63 px or more is a whole number already: no cast to an integer) */
  }
  return dev / dpr;
}

static bool line_width_keyword(const GTEXT_CSS_Value * v, double * px) {
  if (kw(v, "thin")) {
    *px = 1.0;
  }
  else if (kw(v, "medium")) {
    *px = 3.0;
  }
  else if (kw(v, "thick")) {
    *px = 5.0;
  }
  else {
    return false;
  }
  return true;
}

/// Whether the style that goes with a width makes it 0 (`none`, `hidden`).
static bool style_hides(css_state * st, const css_pmeta * m) {
  if (!m->partner) {
    return false;
  }
  const GTEXT_CSS_Value * sv = css_st_root(st, m->partner);
  return kw(sv, "none") || kw(sv, "hidden");
}

static GTEXT_CSS_Status algo_border_width(css_state * st, GTEXT_CSS_Property_Id id, const css_pmeta * m, const GTEXT_CSS_Value_Set * src,
    GTEXT_CSS_Value_Set ** out) {
  GTEXT_CSS_Value_Set * bt = NULL;
  GTEXT_CSS_Status s = bytype(st, id, src, &bt);
  if (s != GTEXT_CSS_OK) {
    return s;
  }
  const GTEXT_CSS_Value * b = gtext_css_value_set_root(bt);
  double px = 0.0;
  if (!line_width_keyword(b, &px)) {
    if (is_px(b)) {
      px = b->number;
    }
    else {
      gtext_css_value_set_free(bt);
      return css_st_fail(st, GTEXT_CSS_E_UNSUPPORTED, NULL, "a border width that is not a keyword or a length");
    }
  }
  gtext_css_value_set_free(bt);
  if (style_hides(st, m)) {
    px = 0.0;
  }
  else {
    px = snap_border(st, px);
  }
  return emit_dimension(st, px, GTEXT_CSS_UNIT_PX, out);
}

/// Apply `fn` to the value, or to each child of a list of values; share what it leaves.
typedef GTEXT_CSS_Status (*item_fn)(css_state * st, void * ctx, const GTEXT_CSS_Value * in, GTEXT_CSS_Value * out, bool * changed);

static GTEXT_CSS_Status map_top(css_state * st, const GTEXT_CSS_Value_Set * set, bool through_list, item_fn fn, void * ctx, GTEXT_CSS_Value_Set ** out) {
  const GTEXT_CSS_Value * root = gtext_css_value_set_root(set);
  if (!root) {
    *out = gtext_css_value_set_retain(set);
    return GTEXT_CSS_OK;
  }
  ab a;
  GTEXT_CSS_Status s = ab_open(&a, st);
  if (s != GTEXT_CSS_OK) {
    return s;
  }
  GTEXT_CSS_Value * rv = css_vb_value(&a.vb);
  bool any = false;
  if (!rv) {
    return ab_close(&a, NULL, NULL, NULL, out);
  }
  if (through_list && root->kind == GTEXT_CSS_VAL_LIST && root->child_count) {
    if (root->child_count > st->cc->options.max_list_items) {
      css_vb_abort(&a.vb);
      return css_st_fail(st, GTEXT_CSS_E_LIMIT_LIST, NULL, "a list has more items than max_list_items");
    }
    GTEXT_CSS_Value * kids = css_vb_values(&a.vb, root->child_count);
    if (!kids) {
      return ab_close(&a, NULL, NULL, NULL, out);
    }
    for (size_t i = 0; i < root->child_count; i++) {
      bool changed = false;
      kids[i] = root->children[i];
      s = fn(st, ctx, &root->children[i], &kids[i], &changed);
      if (s != GTEXT_CSS_OK) {
        css_vb_abort(&a.vb);
        return s;
      }
      any = any || changed;
    }
    *rv = *root;
    rv->children = kids;
  }
  else {
    bool changed = false;
    *rv = *root;
    s = fn(st, ctx, root, rv, &changed);
    if (s != GTEXT_CSS_OK) {
      css_vb_abort(&a.vb);
      return s;
    }
    any = changed;
  }
  if (!any) {
    css_vb_abort(&a.vb);
    *out = gtext_css_value_set_retain(set);
    return GTEXT_CSS_OK;
  }
  return ab_close(&a, rv, set, NULL, out);
}

typedef struct {
  bool snap;
  bool numbers_are_px;
} width_ctx;

static GTEXT_CSS_Status width_item(css_state * st, void * ctx, const GTEXT_CSS_Value * in, GTEXT_CSS_Value * out, bool * changed) {
  const width_ctx * c = ctx;
  double px;
  (void)st;
  if (line_width_keyword(in, &px)) {
    css_v_dimension(out, c->snap ? snap_border(st, px) : px, GTEXT_CSS_UNIT_PX);
    out->slot = in->slot;
    *changed = true;
  }
  else if (c->numbers_are_px && is_number(in)) {
    css_v_dimension(out, in->number, GTEXT_CSS_UNIT_PX);
    out->slot = in->slot;
    *changed = true;
  }
  else if (c->snap && is_px(in)) {
    const double snapped = snap_border(st, in->number);
    if (snapped != in->number) {
      *out = *in;
      out->number = snapped;
      *changed = true;
    }
  }
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status algo_width_list(css_state * st, GTEXT_CSS_Property_Id id, const GTEXT_CSS_Value_Set * src, bool snap, bool numbers,
    GTEXT_CSS_Value_Set ** out) {
  GTEXT_CSS_Value_Set * bt = NULL;
  GTEXT_CSS_Status s = bytype(st, id, src, &bt);
  if (s != GTEXT_CSS_OK) {
    return s;
  }
  width_ctx c = {snap, numbers};
  s = map_top(st, bt, true, width_item, &c, out);
  gtext_css_value_set_free(bt);
  return s;
}

/* ---- simple scalar properties --------------------------------------------------------------------------------- */

/// The keywords of a display value: a keyword, or a list of one to three of them. False for anything else.
static bool display_words(const GTEXT_CSS_Value * v, const char * words[3], size_t * n) {
  *n = 0;
  if (!v) {
    return false;
  }
  if (v->kind == GTEXT_CSS_VAL_KEYWORD) {
    words[(*n)++] = v->text.data;
    return true;
  }
  if (v->kind == GTEXT_CSS_VAL_LIST && v->child_count >= 1 && v->child_count <= 3) {
    for (size_t i = 0; i < v->child_count; i++) {
      if (v->children[i].kind != GTEXT_CSS_VAL_KEYWORD) {
        return false;
      }
      words[(*n)++] = v->children[i].text.data;
    }
    return true;
  }
  return false;
}

/* Display 3, Appendix B: on an SVG element other than a container that is rendered, a text content child or `use`, "display: contents computes to
   display: none." (on the `svg` element with CSS box layout too); "display: contents on text prevents the entire text element from being
   rendered", which is what `none` is, and so the `text` element is one of those too. The element is not known to a computation without a tree. */
static bool svg_contents_is_none(css_state * st) {
  if (!st->iface || !st->element) {
    return false;
  }
  GTEXT_CSS_String ns, name;
  bool found_ns = false, found_name = false;
  GTEXT_CSS_Element_Result er;
  if (gtext_css_element_get_namespace_uri(st->iface, st->element, &ns, &found_ns, &er) != GTEXT_CSS_OK || !found_ns ||
      ns.length != 26 || memcmp(ns.data, "http://www.w3.org/2000/svg", 26) != 0) {
    return false;
  }
  if (gtext_css_element_get_local_name(st->iface, st->element, &name, &found_name, &er) != GTEXT_CSS_OK || !found_name) {
    return false;
  }
  static const char * const keeps[] = {"a", "g", "switch", "use", "tspan", "textPath"};
  for (size_t i = 0; i < sizeof keeps / sizeof *keeps; i++) {
    if (name.length == strlen(keeps[i]) && memcmp(name.data, keeps[i], name.length) == 0) {
      return false;
    }
  }
  return true;
}

static GTEXT_CSS_Status algo_display(css_state * st, const GTEXT_CSS_Value_Set * src, GTEXT_CSS_Value_Set ** out) {
  const GTEXT_CSS_Compute_Context * cc = st->cc;
  const GTEXT_CSS_Value * v = gtext_css_value_set_root(src);
  const char * words[3] = {NULL, NULL, NULL};
  size_t nw = 0;
  if (!display_words(v, words, &nw)) {
    *out = gtext_css_value_set_retain(src);
    return GTEXT_CSS_OK;
  }
  if (nw == 1 && strcmp(words[0], "contents") == 0 && svg_contents_is_none(st)) {
    return emit_keyword(st, "none", out);
  }
  /* blockification (Display 3, 2.7): a floated, absolutely positioned or root box, and a flex or grid item */
  bool blockify = st->parent == NULL;
  if (!blockify && cc->pid[CSS_P_FLOAT]) {
    const GTEXT_CSS_Value * f = css_st_root(st, cc->pid[CSS_P_FLOAT]);
    blockify = f && f->kind == GTEXT_CSS_VAL_KEYWORD && !kw(f, "none");
  }
  if (!blockify && cc->pid[CSS_P_POSITION]) {
    const GTEXT_CSS_Value * p = css_st_root(st, cc->pid[CSS_P_POSITION]);
    blockify = kw(p, "absolute") || kw(p, "fixed");
  }
  if (!blockify && st->parent && cc->pid[CSS_P_DISPLAY]) {
    const char * pw[3] = {NULL, NULL, NULL};
    size_t pn = 0;
    if (display_words(gtext_css_value_set_root(st->parent->vals[cc->pid[CSS_P_DISPLAY] - 1]), pw, &pn)) {
      for (size_t i = 0; i < pn; i++) {
        blockify = blockify || strcmp(pw[i], "flex") == 0 || strcmp(pw[i], "inline-flex") == 0 || strcmp(pw[i], "grid") == 0 ||
            strcmp(pw[i], "inline-grid") == 0;
      }
    }
  }
  /* the model: outer (0 inline, 1 block, 2 run-in), inner, list-item */
  enum { OUT_NONE = -1, OUT_INLINE = 0, OUT_BLOCK = 1, OUT_RUNIN = 2 };
  static const char * const inner_words[] = {"flow", "flow-root", "table", "flex", "grid", "ruby", "math"};
  int outer = OUT_NONE, inner = -1;
  bool li = false, other = false;
  static const struct { const char * w; int outer; int inner; bool li; } legacy[] = {
    {"inline", OUT_INLINE, 0, false}, {"block", OUT_BLOCK, 0, false}, {"inline-block", OUT_INLINE, 1, false}, {"flow-root", OUT_BLOCK, 1, false},
    {"flex", OUT_BLOCK, 3, false}, {"inline-flex", OUT_INLINE, 3, false}, {"grid", OUT_BLOCK, 4, false}, {"inline-grid", OUT_INLINE, 4, false},
    {"table", OUT_BLOCK, 2, false}, {"inline-table", OUT_INLINE, 2, false}, {"list-item", OUT_BLOCK, 0, true}, {"ruby", OUT_INLINE, 5, false},
    {"math", OUT_BLOCK, 6, false},
    {"run-in", OUT_RUNIN, 0, false},
  };
  for (size_t i = 0; i < nw && !other; i++) {
    const char * w = words[i];
    bool matched = false;
    if (nw == 1) {
      for (size_t k = 0; k < sizeof legacy / sizeof *legacy; k++) {
        if (strcmp(w, legacy[k].w) == 0) {
          outer = legacy[k].outer;
          inner = legacy[k].inner;
          li = legacy[k].li;
          matched = true;
          break;
        }
      }
    }
    else if (strcmp(w, "inline") == 0) {
      outer = OUT_INLINE;
      matched = true;
    }
    else if (strcmp(w, "block") == 0) {
      outer = OUT_BLOCK;
      matched = true;
    }
    else if (strcmp(w, "run-in") == 0) {
      outer = OUT_RUNIN;
      matched = true;
    }
    else if (strcmp(w, "list-item") == 0) {
      li = true;
      matched = true;
    }
    else {
      for (size_t k = 0; k < sizeof inner_words / sizeof *inner_words; k++) {
        if (strcmp(w, inner_words[k]) == 0) {
          inner = (int)k;
          matched = true;
        }
      }
    }
    if (!matched) {
      other = true; /* none, contents, a table-internal or ruby-internal box: not a pair */
    }
  }
  if (other) {
    /* Display 3, 2.7: "If a layout-internal box is blockified, its inner display type converts to flow so that it becomes a block container." */
    static const char * const layout_internal[] = {"table-row-group", "table-header-group", "table-footer-group", "table-row", "table-cell",
        "table-column-group", "table-column", "table-caption", "ruby-base", "ruby-text", "ruby-base-container", "ruby-text-container"};
    if (blockify && nw == 1) {
      for (size_t k = 0; k < sizeof layout_internal / sizeof *layout_internal; k++) {
        if (strcmp(words[0], layout_internal[k]) == 0) {
          return emit_keyword(st, "block", out);
        }
      }
    }
    *out = gtext_css_value_set_retain(src);
    return GTEXT_CSS_OK;
  }
  if (inner < 0) {
    inner = 0;
  }
  if (outer == OUT_NONE) {
    outer = inner == 5 ? OUT_INLINE : OUT_BLOCK;
  }
  if (blockify && outer != OUT_BLOCK) {
    /* Display 3, 2.7: "For legacy reasons, if an inline block box (inline flow-root) is blockified, it becomes a block box (losing its flow-root
       nature)", and "a run-in flow-root box also blockifies to a block box" */
    if (inner == 1) {
      inner = 0;
    }
    outer = OUT_BLOCK;
  }
  /* the shortest spelling */
  static const struct { int outer; int inner; bool li; const char * w; } shortest[] = {
    {OUT_INLINE, 0, false, "inline"}, {OUT_BLOCK, 0, false, "block"}, {OUT_BLOCK, 1, false, "flow-root"}, {OUT_INLINE, 1, false, "inline-block"},
    {OUT_BLOCK, 3, false, "flex"}, {OUT_INLINE, 3, false, "inline-flex"}, {OUT_BLOCK, 4, false, "grid"}, {OUT_INLINE, 4, false, "inline-grid"},
    {OUT_BLOCK, 2, false, "table"}, {OUT_INLINE, 2, false, "inline-table"}, {OUT_INLINE, 5, false, "ruby"}, {OUT_BLOCK, 0, true, "list-item"},
    {OUT_RUNIN, 0, false, "run-in"},
  };
  for (size_t k = 0; k < sizeof shortest / sizeof *shortest; k++) {
    if (shortest[k].outer == outer && shortest[k].inner == inner && shortest[k].li == li) {
      if (nw == 1 && strcmp(words[0], shortest[k].w) == 0) {
        *out = gtext_css_value_set_retain(src);
        return GTEXT_CSS_OK;
      }
      return emit_keyword(st, shortest[k].w, out);
    }
  }
  static const char * const outer_words[] = {"inline", "block", "run-in"};
  ab a;
  GTEXT_CSS_Status s = ab_open(&a, st);
  if (s != GTEXT_CSS_OK) {
    return s;
  }
  const size_t n = li ? 3 : 2;
  GTEXT_CSS_Value * kids = css_vb_values(&a.vb, n);
  GTEXT_CSS_Value * root = css_vb_value(&a.vb);
  if (kids && root) {
    css_v_keyword(&a.vb, &kids[0], outer_words[outer]);
    css_v_keyword(&a.vb, &kids[1], inner_words[inner]);
    if (li) {
      css_v_keyword(&a.vb, &kids[2], "list-item");
    }
    css_v_list(root, kids, n, GTEXT_CSS_SEP_SPACE);
  }
  return ab_close(&a, root, NULL, NULL, out);
}

/* CSS Positioned Layout 3: "A position value of absolute or fixed blockifies the box, causes float to compute to none, and forces the box to establish an
   independent formatting context." */
static GTEXT_CSS_Status algo_float(css_state * st, const GTEXT_CSS_Value_Set * src, GTEXT_CSS_Value_Set ** out) {
  const GTEXT_CSS_Value * p = st->cc->pid[CSS_P_POSITION] ? css_st_root(st, st->cc->pid[CSS_P_POSITION]) : NULL;
  if (p && (kw(p, "absolute") || kw(p, "fixed"))) {
    const GTEXT_CSS_Value * f = gtext_css_value_set_root(src);
    if (f && f->kind == GTEXT_CSS_VAL_KEYWORD && kw(f, "none")) {
      *out = gtext_css_value_set_retain(src);
      return GTEXT_CSS_OK;
    }
    return emit_keyword(st, "none", out);
  }
  *out = gtext_css_value_set_retain(src);
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status algo_color(css_state * st, GTEXT_CSS_Property_Id id, const GTEXT_CSS_Value_Set * src, GTEXT_CSS_Value_Set ** out) {
  const GTEXT_CSS_Value * v = gtext_css_value_set_root(src);
  if (v && v->kind == GTEXT_CSS_VAL_COLOR && v->color && v->color->kind == GTEXT_CSS_COLOR_KEYWORD &&
      v->color->keyword == GTEXT_CSS_COLOR_KW_CURRENTCOLOR) {
    /* `color: currentcolor` is `color: inherit` (Color 4, 4.4) */
    return css_st_inherit(st, id, out);
  }
  return bytype(st, id, src, out);
}

static GTEXT_CSS_Status algo_opacity(css_state * st, GTEXT_CSS_Property_Id id, const GTEXT_CSS_Value_Set * src, GTEXT_CSS_Value_Set ** out) {
  GTEXT_CSS_Value_Set * bt = NULL;
  GTEXT_CSS_Status s = bytype(st, id, src, &bt);
  if (s != GTEXT_CSS_OK) {
    return s;
  }
  const GTEXT_CSS_Value * b = gtext_css_value_set_root(bt);
  double n;
  if (is_pct(b)) {
    n = b->number / 100.0;
  }
  else if (is_number(b)) {
    n = b->number;
  }
  else {
    *out = bt;
    return GTEXT_CSS_OK;
  }
  gtext_css_value_set_free(bt);
  n = gtext_css_math_clamp(n, 0.0, 1.0);
  return emit_number(st, n, false, out);
}

/* Text 3: the computed value of `word-spacing` is "an absolute length", and `normal` is no additional spacing, which is zero. */
static GTEXT_CSS_Status algo_word_spacing(css_state * st, GTEXT_CSS_Property_Id id, const GTEXT_CSS_Value_Set * src, GTEXT_CSS_Value_Set ** out) {
  GTEXT_CSS_Value_Set * bt = NULL;
  GTEXT_CSS_Status s = bytype(st, id, src, &bt);
  if (s != GTEXT_CSS_OK) {
    return s;
  }
  if (kw(gtext_css_value_set_root(bt), "normal")) {
    gtext_css_value_set_free(bt);
    return emit_dimension(st, 0.0, GTEXT_CSS_UNIT_PX, out);
  }
  *out = bt;
  return GTEXT_CSS_OK;
}

/* Text 4: the value `normal` of `letter-spacing` is "no additional spacing" and the browsers and the tests of the pin write a zero length as `normal`. */
static GTEXT_CSS_Status algo_letter_spacing(css_state * st, GTEXT_CSS_Property_Id id, const GTEXT_CSS_Value_Set * src, GTEXT_CSS_Value_Set ** out) {
  GTEXT_CSS_Value_Set * bt = NULL;
  GTEXT_CSS_Status s = bytype(st, id, src, &bt);
  if (s != GTEXT_CSS_OK) {
    return s;
  }
  const GTEXT_CSS_Value * b = gtext_css_value_set_root(bt);
  if (b && b->kind == GTEXT_CSS_VAL_DIMENSION && b->number == 0.0 && !(b->flags & GTEXT_CSS_VFLAG_MATH)) {
    gtext_css_value_set_free(bt);
    return emit_keyword(st, "normal", out);
  }
  *out = bt;
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status algo_zoom(css_state * st, GTEXT_CSS_Property_Id id, const GTEXT_CSS_Value_Set * src, GTEXT_CSS_Value_Set ** out) {
  GTEXT_CSS_Value_Set * bt = NULL;
  GTEXT_CSS_Status s = bytype(st, id, src, &bt);
  if (s != GTEXT_CSS_OK) {
    return s;
  }
  const GTEXT_CSS_Value * b = gtext_css_value_set_root(bt);
  if (is_pct(b)) {
    const double n = b->number / 100.0;
    gtext_css_value_set_free(bt);
    return emit_number(st, n, false, out);
  }
  *out = bt;
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status algo_math_depth(css_state * st, GTEXT_CSS_Property_Id id, const GTEXT_CSS_Value_Set * src, GTEXT_CSS_Value_Set ** out) {
  const GTEXT_CSS_Value * v = gtext_css_value_set_root(src);
  const GTEXT_CSS_Value * pv = css_st_parent_root(st, id);
  const double parent = pv && (pv->kind == GTEXT_CSS_VAL_INTEGER || pv->kind == GTEXT_CSS_VAL_NUMBER) ? pv->number : 0.0;
  if (kw(v, "auto-add")) {
    const GTEXT_CSS_Value * ps = st->cc->pid[CSS_P_MATH_STYLE] ? css_st_parent_root(st, st->cc->pid[CSS_P_MATH_STYLE]) : NULL;
    return emit_number(st, kw(ps, "compact") ? parent + 1.0 : parent, true, out);
  }
  if (v && v->kind == GTEXT_CSS_VAL_FUNCTION && v->child_count >= 1) {
    const GTEXT_CSS_Value * arg = &v->children[0];
    if (is_number(arg)) {
      return emit_number(st, parent + arg->number, true, out);
    }
  }
  if (is_number(v)) {
    return emit_number(st, v->number, true, out);
  }
  return css_st_fail(st, GTEXT_CSS_E_UNSUPPORTED, NULL, "a math-depth that is not auto-add, add() or an integer");
}

static GTEXT_CSS_Status algo_text_align(css_state * st, GTEXT_CSS_Property_Id id, const GTEXT_CSS_Value_Set * src, GTEXT_CSS_Value_Set ** out) {
  const GTEXT_CSS_Value * v = gtext_css_value_set_root(src);
  if (!kw(v, "match-parent")) {
    *out = gtext_css_value_set_retain(src);
    return GTEXT_CSS_OK;
  }
  /* match-parent: the parent's value, with start and end made left and right by the parent's direction (Text 4, 7.1) */
  if (!st->parent) {
    return emit_keyword(st, "start", out);
  }
  const GTEXT_CSS_Value * pv = css_st_parent_root(st, id);
  const bool rtl = st->parent->flow.direction == GTEXT_CSS_DIRECTION_RTL;
  if (kw(pv, "start")) {
    return emit_keyword(st, rtl ? "right" : "left", out);
  }
  if (kw(pv, "end")) {
    return emit_keyword(st, rtl ? "left" : "right", out);
  }
  *out = gtext_css_value_set_retain(st->parent->vals[id - 1]);
  return GTEXT_CSS_OK;
}

static bool voice_keyword_is(const GTEXT_CSS_Value * v, const char * word) {
  return v->kind == GTEXT_CSS_VAL_KEYWORD && strlen(word) == v->text.length && memcmp(v->text.data, word, v->text.length) == 0;
}

/* CSS Speech 1, voice-pitch and voice-range: "one of the predefined pitch keywords if only the keyword is specified by itself, otherwise an absolute
   frequency calculated by converting the keyword value (if any) to a fixed frequency based on the current voice-family and by applying the specified
   relative offset (if any)". The value is a sequence of its components (the grammar is `||` and `&&` of them, so even one keyword is a sequence of
   one). A keyword by itself is its own computed value; a frequency with `absolute` is already an absolute frequency (in hertz, as every frequency
   computes); a keyword or an offset with the other needs the frequency table of the voice, which the environment does not have. */
static GTEXT_CSS_Status algo_voice_pitch(css_state * st, GTEXT_CSS_Property_Id id, const GTEXT_CSS_Value_Set * src, GTEXT_CSS_Value_Set ** out) {
  const GTEXT_CSS_Value * v = gtext_css_value_set_root(src);
  if (v && v->kind == GTEXT_CSS_VAL_KEYWORD) {
    *out = gtext_css_value_set_retain(src);
    return GTEXT_CSS_OK;
  }
  if (v && v->kind == GTEXT_CSS_VAL_DIMENSION) {
    return bytype(st, id, src, out);
  }
  if (v && v->child_count == 1 && v->children[0].kind == GTEXT_CSS_VAL_KEYWORD) {
    *out = gtext_css_value_set_retain(src);   /* the keyword by itself */
    return GTEXT_CSS_OK;
  }
  if (v && v->child_count == 2) {
    const GTEXT_CSS_Value * a = &v->children[0];
    const GTEXT_CSS_Value * b = &v->children[1];
    if ((a->kind == GTEXT_CSS_VAL_DIMENSION && voice_keyword_is(b, "absolute")) || (b->kind == GTEXT_CSS_VAL_DIMENSION && voice_keyword_is(a, "absolute"))) {
      return bytype(st, id, src, out);   /* an absolute frequency */
    }
  }
  return css_st_fail(st, GTEXT_CSS_E_UNSUPPORTED, st->cc->meta[id - 1].name,
      "a pitch keyword with an offset, or an offset by itself, needs the frequency table of the voice");
}

static GTEXT_CSS_Status algo_voice_balance(css_state * st, GTEXT_CSS_Property_Id id, const GTEXT_CSS_Value_Set * src, GTEXT_CSS_Value_Set ** out) {
  const GTEXT_CSS_Value * v = gtext_css_value_set_root(src);
  double n;
  if (kw(v, "left")) {
    n = -100.0;
  }
  else if (kw(v, "center")) {
    n = 0.0;
  }
  else if (kw(v, "right")) {
    n = 100.0;
  }
  else if (kw(v, "leftwards") || kw(v, "rightwards")) {
    const GTEXT_CSS_Value * pv = css_st_parent_root(st, id);
    const double base = is_number(pv) ? pv->number : 0.0;
    n = base + (kw(v, "leftwards") ? -20.0 : 20.0);
  }
  else {
    GTEXT_CSS_Value_Set * bt = NULL;
    GTEXT_CSS_Status s = bytype(st, id, src, &bt);
    if (s != GTEXT_CSS_OK) {
      return s;
    }
    const GTEXT_CSS_Value * b = gtext_css_value_set_root(bt);
    n = is_number(b) ? b->number : 0.0;
    gtext_css_value_set_free(bt);
  }
  return emit_number(st, gtext_css_math_clamp(n, -100.0, 100.0), false, out);
}

static GTEXT_CSS_Status algo_wrap_flow(css_state * st, GTEXT_CSS_Property_Id id, const GTEXT_CSS_Value_Set * src, GTEXT_CSS_Value_Set ** out) {
  (void)id;
  /* as specified, except for an element whose float is not none: auto */
  const GTEXT_CSS_Value * f = st->cc->pid[CSS_P_FLOAT] ? css_st_root(st, st->cc->pid[CSS_P_FLOAT]) : NULL;
  if (f && f->kind == GTEXT_CSS_VAL_KEYWORD && !kw(f, "none")) {
    return emit_keyword(st, "auto", out);
  }
  *out = gtext_css_value_set_retain(src);
  return GTEXT_CSS_OK;
}


/* ---- shapes of values --------------------------------------------------------------------------------------- */

#define FLAT_MAX 64u

/// The leaves of a value in order, whatever lists they are nested in.
static GTEXT_CSS_Status flatten(css_state * st, const GTEXT_CSS_Value * root, const GTEXT_CSS_Value ** leaves, size_t * n) {
  struct { const GTEXT_CSS_Value * node; size_t i; } stack[32];
  size_t sp = 0;
  *n = 0;
  if (!root) {
    return GTEXT_CSS_OK;
  }
  if (root->kind != GTEXT_CSS_VAL_LIST || (root->flags & GTEXT_CSS_VFLAG_MATH)) {
    leaves[(*n)++] = root;
    return GTEXT_CSS_OK;
  }
  stack[sp].node = root;
  stack[sp].i = 0;
  sp++;
  while (sp) {
    const GTEXT_CSS_Value * node = stack[sp - 1].node;
    if (stack[sp - 1].i >= node->child_count) {
      sp--;
      continue;
    }
    const GTEXT_CSS_Value * kid = &node->children[stack[sp - 1].i++];
    if (kid->kind == GTEXT_CSS_VAL_LIST && !(kid->flags & GTEXT_CSS_VFLAG_MATH)) {
      if (sp == 32) {
        return css_st_fail(st, GTEXT_CSS_E_LIMIT_DEPTH, NULL, "a value is nested deeper than the algorithms read");
      }
      stack[sp].node = kid;
      stack[sp].i = 0;
      sp++;
    }
    else {
      if (*n == FLAT_MAX) {
        return css_st_fail(st, GTEXT_CSS_E_LIMIT_LIST, NULL, "a value has more components than the algorithms read");
      }
      leaves[(*n)++] = kid;
    }
  }
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status run_build(css_state * st, GTEXT_CSS_Property_Id id, const GTEXT_CSS_Value_Set * src, unsigned bytype_flags,
    GTEXT_CSS_Status (*fn)(css_state *, ab *, const GTEXT_CSS_Value_Set *, const GTEXT_CSS_Value *, const css_pmeta *, GTEXT_CSS_Value **),
    const css_pmeta * m, GTEXT_CSS_Value_Set ** out) {
  GTEXT_CSS_Value_Set * bt = NULL;
  GTEXT_CSS_Status s = css_cv_by_type_ex(st, src, id, bytype_flags, &bt);
  if (s != GTEXT_CSS_OK) {
    return s;
  }
  ab a;
  s = ab_open(&a, st);
  if (s != GTEXT_CSS_OK) {
    gtext_css_value_set_free(bt);
    return s;
  }
  GTEXT_CSS_Value * root = NULL;
  s = fn(st, &a, bt, gtext_css_value_set_root(bt), m, &root);
  if (s != GTEXT_CSS_OK) {
    css_vb_abort(&a.vb);
    gtext_css_value_set_free(bt);
    return s;
  }
  if (!root) {
    /* nothing to rewrite: the value as computed by its types */
    css_vb_abort(&a.vb);
    *out = bt;
    return GTEXT_CSS_OK;
  }
  s = ab_close(&a, root, bt, NULL, out);
  gtext_css_value_set_free(bt);
  return s;
}

/// Rewrite the items of a comma list (or the one item): `item(a, ctx, in_leaves..)` fills `out`. The list node is copied with the new items.
typedef GTEXT_CSS_Status (*per_item_fn)(css_state * st, ab * a, const css_pmeta * m, const GTEXT_CSS_Value * item, GTEXT_CSS_Value * out, bool * keep);

static GTEXT_CSS_Status build_items(css_state * st, ab * a, const css_pmeta * m, const GTEXT_CSS_Value * root, per_item_fn fn,
    GTEXT_CSS_Value ** out_root) {
  GTEXT_CSS_Value * rv = css_vb_value(&a->vb);
  if (!rv) {
    return a->vb.status;
  }
  if (root->kind == GTEXT_CSS_VAL_LIST && root->separator == GTEXT_CSS_SEP_COMMA) {
    if (root->child_count > st->cc->options.max_list_items) {
      return css_st_fail(st, GTEXT_CSS_E_LIMIT_LIST, NULL, "a list has more items than max_list_items");
    }
    GTEXT_CSS_Value * kids = css_vb_values(&a->vb, root->child_count);
    if (!kids) {
      return a->vb.status;
    }
    for (size_t i = 0; i < root->child_count; i++) {
      bool keep = false;
      GTEXT_CSS_Status s = fn(st, a, m, &root->children[i], &kids[i], &keep);
      if (s != GTEXT_CSS_OK) {
        return s;
      }
      if (keep) {
        kids[i] = root->children[i];
      }
    }
    *rv = *root;
    rv->children = kids;
  }
  else {
    bool keep = false;
    GTEXT_CSS_Status s = fn(st, a, m, root, rv, &keep);
    if (s != GTEXT_CSS_OK) {
      return s;
    }
    if (keep) {
      *rv = *root;
    }
  }
  *out_root = rv;
  return GTEXT_CSS_OK;
}

/* ---- calculations that are made (positions) -------------------------------------------------------------------- */

static void mb_start(mbuilder * mb, ab * a, css_state * st) {
  memset(mb, 0, sizeof *mb);
  mb->arena = &a->vb.set->arena;
  mb->alloc = st->cc->alloc;
  mb->max_depth = 65536;
  mb->max_nodes = st->cc->options.max_value_nodes;
  mb->status = GTEXT_CSS_OK;
}

static const mnode * value_mnode(mbuilder * mb, const GTEXT_CSS_Value * v) {
  if (v->flags & GTEXT_CSS_VFLAG_MATH && v->math) {
    return v->math->root;
  }
  switch (v->kind) {
  case GTEXT_CSS_VAL_PERCENTAGE: return mk_percent(mb, v->number);
  case GTEXT_CSS_VAL_DIMENSION: return mk_dimension(mb, v->number, v->unit);
  case GTEXT_CSS_VAL_ZERO: return mk_dimension(mb, 0.0, GTEXT_CSS_UNIT_PX);
  case GTEXT_CSS_VAL_NUMBER:
  case GTEXT_CSS_VAL_INTEGER: return mk_number(mb, v->number);
  default: return NULL;
  }
}

static GTEXT_CSS_Status emit_mnode(css_state * st, ab * a, mbuilder * mb, const mnode * root, GTEXT_CSS_Value * out) {
  if (!root || mb->status != GTEXT_CSS_OK) {
    return css_st_fail(st, mb->status != GTEXT_CSS_OK ? mb->status : GTEXT_CSS_E_INTERNAL, NULL, "a calculation could not be made");
  }
  memset(out, 0, sizeof *out);
  if (root->op == GTEXT_CSS_MATH_PERCENT) {
    css_v_percentage(out, root->value);
    return GTEXT_CSS_OK;
  }
  if (root->op == GTEXT_CSS_MATH_DIMENSION) {
    css_v_dimension(out, root->value, root->unit);
    return GTEXT_CSS_OK;
  }
  if (root->op == GTEXT_CSS_MATH_NUMBER) {
    css_v_number(out, root->value);
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Math * nm = css_arena_alloc(&a->vb.set->arena, sizeof *nm);
  if (!nm) {
    return css_st_fail(st, GTEXT_CSS_E_OOM, NULL, NULL);
  }
  memset(nm, 0, sizeof *nm);
  nm->root = root;
  nm->type = root->type;
  nm->category = css_math_type_category(&root->type);
  nm->has_percent = true;
  nm->node_count = mb->nodes;
  char * text = NULL;
  size_t tl = 0;
  GTEXT_Allocator alloc = st->cc->alloc;
  GTEXT_CSS_Status s = css_math_serialize_to(nm, &alloc, (size_t)1 << 24, &text, &tl);
  if (s != GTEXT_CSS_OK) {
    return css_st_fail(st, s, NULL, NULL);
  }
  out->kind = GTEXT_CSS_VAL_FUNCTION;
  out->flags = GTEXT_CSS_VFLAG_MATH | GTEXT_CSS_VFLAG_MATH_PERCENT;
  out->interp = GTEXT_CSS_INTERP_LENGTH;
  out->math = nm;
  out->range_lo = -1.0 / 0.0;
  out->range_hi = 1.0 / 0.0;
  out->text.data = css_vb_str(&a->vb, text, tl);
  out->text.length = out->text.data ? tl : 0;
  gtext_allocator_free(&alloc, text);
  return a->vb.status;
}

/* ---- positions ------------------------------------------------------------------------------------------------- */

typedef enum { T_NONE, T_LEFT, T_RIGHT, T_TOP, T_BOTTOM, T_CENTER, T_LOGICAL, T_LP } tok_t;

static tok_t classify_tok(const GTEXT_CSS_Value * v) {
  if (v->kind == GTEXT_CSS_VAL_KEYWORD) {
    if (kw(v, "left")) return T_LEFT;
    if (kw(v, "right")) return T_RIGHT;
    if (kw(v, "top")) return T_TOP;
    if (kw(v, "bottom")) return T_BOTTOM;
    if (kw(v, "center")) return T_CENTER;
    return T_LOGICAL;
  }
  if (v->kind == GTEXT_CSS_VAL_DIMENSION || v->kind == GTEXT_CSS_VAL_PERCENTAGE || v->kind == GTEXT_CSS_VAL_ZERO || ((v->flags & GTEXT_CSS_VFLAG_MATH) && v->math)) {
    return T_LP;
  }
  return T_NONE;
}

/// One axis of a position: the edge it is measured from (0 start, 1 end, 2 center) and an offset, as a length-percentage from the start edge.
static GTEXT_CSS_Status axis_value(css_state * st, ab * a, int edge, const GTEXT_CSS_Value * off, GTEXT_CSS_Value * out) {
  memset(out, 0, sizeof *out);
  if (edge == 2) {
    css_v_percentage(out, 50.0);
    return GTEXT_CSS_OK;
  }
  if (edge == 0) {
    if (off) {
      *out = *off;
    }
    else {
      css_v_percentage(out, 0.0);
    }
    return GTEXT_CSS_OK;
  }
  if (!off) {
    css_v_percentage(out, 100.0);
    return GTEXT_CSS_OK;
  }
  if (off->kind == GTEXT_CSS_VAL_PERCENTAGE && !(off->flags & GTEXT_CSS_VFLAG_MATH)) {
    css_v_percentage(out, 100.0 - off->number);
    return GTEXT_CSS_OK;
  }
  /* calc(100% - offset) */
  mbuilder mb;
  mb_start(&mb, a, st);
  const mnode * on = value_mnode(&mb, off);
  if (!on) {
    return css_st_fail(st, GTEXT_CSS_E_INTERNAL, NULL, "a position offset that is not a length or a percentage");
  }
  const mnode * terms[2];
  terms[0] = mk_percent(&mb, 100.0);
  terms[1] = mk_negate(&mb, on);
  const mnode * sum = terms[0] && terms[1] ? mk_sum(&mb, terms, 2) : NULL;
  return emit_mnode(st, a, &mb, sum, out);
}

static GTEXT_CSS_Status position_pair(css_state * st, ab * a, const GTEXT_CSS_Value ** t, size_t n, bool origin, GTEXT_CSS_Value out[3],
    size_t * nout) {
  tok_t k[FLAT_MAX];
  for (size_t i = 0; i < n; i++) {
    k[i] = classify_tok(t[i]);
    if (k[i] == T_NONE) {
      return css_st_fail(st, GTEXT_CSS_E_UNSUPPORTED, NULL, "a position component that is not a keyword, a length or a percentage");
    }
    if (k[i] == T_LOGICAL) {
      return css_st_fail(st, GTEXT_CSS_E_UNSUPPORTED, t[i]->text.data,
          "a flow-relative position keyword needs the writing mode of the containing block");
    }
  }
  *nout = 2;
  const GTEXT_CSS_Value *xo = NULL, *yo = NULL;
  int xe = 2, ye = 2;
  if (n == 1) {
    switch (k[0]) {
    case T_LEFT: xe = 0; break;
    case T_RIGHT: xe = 1; break;
    case T_TOP: ye = 0; break;
    case T_BOTTOM: ye = 1; break;
    case T_LP: xe = 0; xo = t[0]; break;
    default: break;
    }
  }
  else if (n == 2 || (origin && n == 3)) {
    size_t a0 = 0, a1 = 1;
    if (k[0] == T_TOP || k[0] == T_BOTTOM || k[1] == T_LEFT || k[1] == T_RIGHT) {
      a0 = 1;
      a1 = 0;
    }
    /* x from the token a0, y from a1 */
    switch (k[a0]) {
    case T_LEFT: xe = 0; break;
    case T_RIGHT: xe = 1; break;
    case T_CENTER: xe = 2; break;
    default: xe = 0; xo = t[a0]; break;
    }
    switch (k[a1]) {
    case T_TOP: ye = 0; break;
    case T_BOTTOM: ye = 1; break;
    case T_CENTER: ye = 2; break;
    default: ye = 0; yo = t[a1]; break;
    }
    if (origin && n == 3) {
      if (k[2] != T_LP) {
        return css_st_fail(st, GTEXT_CSS_E_UNSUPPORTED, NULL, "the third component of an origin is a length");
      }
      *nout = 3;
    }
  }
  else {
    /* three or four components: an edge keyword, with an offset or without, for each axis */
    struct { tok_t kt; const GTEXT_CSS_Value * off; } g[2];
    size_t ng = 0, i = 0;
    while (i < n) {
      if (k[i] == T_LP || ng == 2) {
        return css_st_fail(st, GTEXT_CSS_E_UNSUPPORTED, NULL, "a position that is not an edge and an offset for each axis");
      }
      g[ng].kt = k[i];
      g[ng].off = NULL;
      i++;
      if (i < n && k[i] == T_LP) {
        g[ng].off = t[i];
        i++;
      }
      ng++;
    }
    int edge_of[2] = {-1, -1};
    const GTEXT_CSS_Value * off_of[2] = {NULL, NULL};
    for (size_t j = 0; j < ng; j++) {
      if (g[j].kt == T_CENTER) {
        continue;
      }
      const int axis = (g[j].kt == T_LEFT || g[j].kt == T_RIGHT) ? 0 : 1;
      if (edge_of[axis] >= 0) {
        return css_st_fail(st, GTEXT_CSS_E_UNSUPPORTED, NULL, "a position that names an axis twice");
      }
      edge_of[axis] = (g[j].kt == T_RIGHT || g[j].kt == T_BOTTOM) ? 1 : 0;
      off_of[axis] = g[j].off;
    }
    for (size_t j = 0; j < ng; j++) {
      if (g[j].kt != T_CENTER) {
        continue;
      }
      const int axis = edge_of[0] < 0 ? 0 : (edge_of[1] < 0 ? 1 : -1);
      if (axis < 0 || g[j].off) {
        return css_st_fail(st, GTEXT_CSS_E_UNSUPPORTED, NULL, "a position that names an axis twice");
      }
      edge_of[axis] = 2;
    }
    xe = edge_of[0] < 0 ? 2 : edge_of[0];
    ye = edge_of[1] < 0 ? 2 : edge_of[1];
    xo = off_of[0];
    yo = off_of[1];
  }
  GTEXT_CSS_Status s = axis_value(st, a, xe, xo, &out[0]);
  if (s == GTEXT_CSS_OK) {
    s = axis_value(st, a, ye, yo, &out[1]);
  }
  if (s == GTEXT_CSS_OK && *nout == 3) {
    out[2] = *t[2];
  }
  return s;
}

static GTEXT_CSS_Status position_item(css_state * st, ab * a, const css_pmeta * m, const GTEXT_CSS_Value * item, GTEXT_CSS_Value * out, bool * keep) {
  const GTEXT_CSS_Value * leaves[FLAT_MAX];
  size_t n = 0;
  GTEXT_CSS_Status s = flatten(st, item, leaves, &n);
  if (s != GTEXT_CSS_OK) {
    return s;
  }
  if (n == 1 && (kw(leaves[0], "normal") || kw(leaves[0], "auto") || kw(leaves[0], "none"))) {
    *keep = true;
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Value comp[3];
  size_t nc = 0;
  const bool origin = m->name_length == 16 && memcmp(m->name, "transform-origin", 16) == 0;
  s = position_pair(st, a, leaves, n, origin, comp, &nc);
  if (s != GTEXT_CSS_OK) {
    return s;
  }
  GTEXT_CSS_Value * kids = css_vb_values(&a->vb, nc);
  if (!kids) {
    return a->vb.status;
  }
  for (size_t i = 0; i < nc; i++) {
    kids[i] = comp[i];
  }
  memset(out, 0, sizeof *out);
  css_v_list(out, kids, nc, GTEXT_CSS_SEP_SPACE);
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status build_position(css_state * st, ab * a, const GTEXT_CSS_Value_Set * bt, const GTEXT_CSS_Value * root, const css_pmeta * m,
    GTEXT_CSS_Value ** out_root) {
  (void)bt;
  if (!root) {
    return GTEXT_CSS_OK;
  }
  return build_items(st, a, m, root, position_item, out_root);
}

/// `background-position-x` and kin: an offset and the edge it is measured from.
static GTEXT_CSS_Status axis_item(css_state * st, ab * a, const css_pmeta * m, const GTEXT_CSS_Value * item, GTEXT_CSS_Value * out, bool * keep) {
  (void)keep;
  const GTEXT_CSS_Value * leaves[FLAT_MAX];
  size_t n = 0;
  GTEXT_CSS_Status s = flatten(st, item, leaves, &n);
  if (s != GTEXT_CSS_OK) {
    return s;
  }
  const char * name = m->name;
  const size_t nl = m->name_length;
  const char * lo = "left";
  const char * hi = "right";
  if (nl && name[nl - 1] == 'y') {
    lo = "top";
    hi = "bottom";
  }
  else if (nl >= 5 && (memcmp(name + nl - 5, "block", 5) == 0 || (nl >= 6 && memcmp(name + nl - 6, "inline", 6) == 0))) {
    lo = "start";
    hi = "end";
  }
  const GTEXT_CSS_Value *offset = NULL;
  const char * edge = lo;
  bool center = false;
  for (size_t i = 0; i < n; i++) {
    const tok_t k = classify_tok(leaves[i]);
    if (k == T_LP) {
      offset = leaves[i];
    }
    else if (k == T_CENTER) {
      center = true;
    }
    else if (leaves[i]->kind == GTEXT_CSS_VAL_KEYWORD && (kw(leaves[i], lo) || kw(leaves[i], hi))) {
      edge = kw(leaves[i], hi) ? hi : lo;
    }
    else {
      return css_st_fail(st, GTEXT_CSS_E_UNSUPPORTED, leaves[i]->text.data, "a flow-relative position keyword needs the writing mode");
    }
  }
  GTEXT_CSS_Value * kids = css_vb_values(&a->vb, 2);
  if (!kids) {
    return a->vb.status;
  }
  css_v_keyword(&a->vb, &kids[0], edge);
  if (center) {
    css_v_percentage(&kids[1], 50.0);
  }
  else if (offset) {
    kids[1] = *offset;
  }
  else {
    css_v_percentage(&kids[1], 0.0);
  }
  memset(out, 0, sizeof *out);
  css_v_list(out, kids, 2, GTEXT_CSS_SEP_SPACE);
  return a->vb.status;
}

static GTEXT_CSS_Status build_axis(css_state * st, ab * a, const GTEXT_CSS_Value_Set * bt, const GTEXT_CSS_Value * root, const css_pmeta * m,
    GTEXT_CSS_Value ** out_root) {
  (void)bt;
  return root ? build_items(st, a, m, root, axis_item, out_root) : GTEXT_CSS_OK;
}

/* ---- sizes, repeats, lists of four -------------------------------------------------------------------------------- */

static GTEXT_CSS_Status size_item(css_state * st, ab * a, const css_pmeta * m, const GTEXT_CSS_Value * item, GTEXT_CSS_Value * out, bool * keep) {
  (void)m;
  const GTEXT_CSS_Value * leaves[FLAT_MAX];
  size_t n = 0;
  GTEXT_CSS_Status s = flatten(st, item, leaves, &n);
  if (s != GTEXT_CSS_OK) {
    return s;
  }
  if (n == 1 && (kw(leaves[0], "cover") || kw(leaves[0], "contain"))) {
    *keep = true;
    return GTEXT_CSS_OK;
  }
  if (n < 1 || n > 2) {
    *keep = true;
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Value * kids = css_vb_values(&a->vb, 2);
  if (!kids) {
    return a->vb.status;
  }
  kids[0] = *leaves[0];
  if (n == 2) {
    kids[1] = *leaves[1];
  }
  else {
    css_v_keyword(&a->vb, &kids[1], "auto");
  }
  memset(out, 0, sizeof *out);
  css_v_list(out, kids, 2, GTEXT_CSS_SEP_SPACE);
  return a->vb.status;
}

static GTEXT_CSS_Status build_size(css_state * st, ab * a, const GTEXT_CSS_Value_Set * bt, const GTEXT_CSS_Value * root, const css_pmeta * m,
    GTEXT_CSS_Value ** out_root) {
  (void)bt;
  return root ? build_items(st, a, m, root, size_item, out_root) : GTEXT_CSS_OK;
}

static GTEXT_CSS_Status repeat_item(css_state * st, ab * a, const css_pmeta * m, const GTEXT_CSS_Value * item, GTEXT_CSS_Value * out, bool * keep) {
  (void)m;
  const GTEXT_CSS_Value * leaves[FLAT_MAX];
  size_t n = 0;
  GTEXT_CSS_Status s = flatten(st, item, leaves, &n);
  if (s != GTEXT_CSS_OK) {
    return s;
  }
  if (n == 2) {
    *keep = true;
    if (item->kind != GTEXT_CSS_VAL_LIST) {
      *keep = true;
    }
    return GTEXT_CSS_OK;
  }
  if (n != 1) {
    *keep = true;
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Value * kids = css_vb_values(&a->vb, 2);
  if (!kids) {
    return a->vb.status;
  }
  if (kw(leaves[0], "repeat-x")) {
    css_v_keyword(&a->vb, &kids[0], "repeat");
    css_v_keyword(&a->vb, &kids[1], "no-repeat");
  }
  else if (kw(leaves[0], "repeat-y")) {
    css_v_keyword(&a->vb, &kids[0], "no-repeat");
    css_v_keyword(&a->vb, &kids[1], "repeat");
  }
  else {
    kids[0] = *leaves[0];
    kids[1] = *leaves[0];
  }
  memset(out, 0, sizeof *out);
  css_v_list(out, kids, 2, GTEXT_CSS_SEP_SPACE);
  return a->vb.status;
}

static GTEXT_CSS_Status build_repeat(css_state * st, ab * a, const GTEXT_CSS_Value_Set * bt, const GTEXT_CSS_Value * root, const css_pmeta * m,
    GTEXT_CSS_Value ** out_root) {
  (void)bt;
  return root ? build_items(st, a, m, root, repeat_item, out_root) : GTEXT_CSS_OK;
}

/// 1 to 4 values as 4 (top, right, bottom, left): the box rule of CSS 2.
static void box_expand(const GTEXT_CSS_Value * in, size_t n, GTEXT_CSS_Value out[4]) {
  switch (n) {
  case 1: out[0] = out[1] = out[2] = out[3] = in[0]; break;
  case 2: out[0] = out[2] = in[0]; out[1] = out[3] = in[1]; break;
  case 3: out[0] = in[0]; out[1] = out[3] = in[1]; out[2] = in[2]; break;
  default: out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; out[3] = in[3]; break;
  }
}

static GTEXT_CSS_Status build_box4(css_state * st, ab * a, const GTEXT_CSS_Value_Set * bt, const GTEXT_CSS_Value * root, const css_pmeta * m,
    GTEXT_CSS_Value ** out_root) {
  (void)st;
  (void)bt;
  (void)m;
  if (!root || root->kind != GTEXT_CSS_VAL_LIST || root->child_count == 0) {
    return GTEXT_CSS_OK;
  }
  /* either the list of values, or a list of that list and `fill` (border-image-slice) */
  const GTEXT_CSS_Value * inner = root;
  const GTEXT_CSS_Value * fill = NULL;
  if (root->children[0].kind == GTEXT_CSS_VAL_LIST) {
    inner = &root->children[0];
    if (root->child_count > 1) {
      fill = &root->children[1];
    }
  }
  else if (root->child_count >= 2 && kw(&root->children[root->child_count - 1], "fill")) {
    return GTEXT_CSS_OK;
  }
  if (inner->child_count < 1 || inner->child_count > 4) {
    return GTEXT_CSS_OK;
  }
  if (inner->child_count == 4 && inner == root) {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Value * four = css_vb_values(&a->vb, 4);
  GTEXT_CSS_Value * list = css_vb_value(&a->vb);
  if (!four || !list) {
    return a->vb.status;
  }
  box_expand(inner->children, inner->child_count, four);
  css_v_list(list, four, 4, GTEXT_CSS_SEP_SPACE);
  if (inner == root) {
    *out_root = list;
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Value * wrap = css_vb_values(&a->vb, fill ? 2 : 1);
  GTEXT_CSS_Value * top = css_vb_value(&a->vb);
  if (!wrap || !top) {
    return a->vb.status;
  }
  wrap[0] = *list;
  if (fill) {
    wrap[1] = *fill;
  }
  *top = *root;
  top->children = wrap;
  top->child_count = fill ? 2 : 1;
  *out_root = top;
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status build_pair_dup(css_state * st, ab * a, const GTEXT_CSS_Value_Set * bt, const GTEXT_CSS_Value * root, const css_pmeta * m,
    GTEXT_CSS_Value ** out_root) {
  (void)st;
  (void)bt;
  (void)m;
  if (!root) {
    return GTEXT_CSS_OK;
  }
  const GTEXT_CSS_Value * one = root;
  if (root->kind == GTEXT_CSS_VAL_LIST) {
    if (root->child_count != 1) {
      return GTEXT_CSS_OK;
    }
    one = &root->children[0];
  }
  GTEXT_CSS_Value * kids = css_vb_values(&a->vb, 2);
  GTEXT_CSS_Value * list = css_vb_value(&a->vb);
  if (!kids || !list) {
    return a->vb.status;
  }
  kids[0] = kids[1] = *one;
  css_v_list(list, kids, 2, GTEXT_CSS_SEP_SPACE);
  *out_root = list;
  return GTEXT_CSS_OK;
}

static size_t dash_count(const GTEXT_CSS_Value * v) {
  if (v->kind != GTEXT_CSS_VAL_LIST) {
    return 1;
  }
  size_t n = 0;
  for (size_t i = 0; i < v->child_count; i++) {
    n += dash_count(&v->children[i]);
  }
  return n;
}

static void dash_fill(const GTEXT_CSS_Value * v, GTEXT_CSS_Value * dst, size_t * n) {
  if (v->kind != GTEXT_CSS_VAL_LIST) {
    dst[(*n)++] = *v;
    return;
  }
  for (size_t i = 0; i < v->child_count; i++) {
    dash_fill(&v->children[i], dst, n);
  }
}

/* SVG 2: the computed value of `stroke-dasharray` is "as comma separated list of absolute lengths or percentages", whichever way the dashes were separated. */
static GTEXT_CSS_Status build_dasharray(css_state * st, ab * a, const GTEXT_CSS_Value_Set * bt, const GTEXT_CSS_Value * root, const css_pmeta * m,
    GTEXT_CSS_Value ** out_root) {
  (void)st;
  (void)bt;
  (void)m;
  if (!root || root->kind != GTEXT_CSS_VAL_LIST) {
    return GTEXT_CSS_OK;
  }
  const size_t n = dash_count(root);
  GTEXT_CSS_Value * kids = css_vb_values(&a->vb, n);
  GTEXT_CSS_Value * list = css_vb_value(&a->vb);
  if (!kids || !list) {
    return a->vb.status;
  }
  size_t k = 0;
  dash_fill(root, kids, &k);
  css_v_list(list, kids, k, GTEXT_CSS_SEP_COMMA);
  *out_root = list;
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status build_hyphenate(css_state * st, ab * a, const GTEXT_CSS_Value_Set * bt, const GTEXT_CSS_Value * root, const css_pmeta * m,
    GTEXT_CSS_Value ** out_root) {
  (void)st;
  (void)bt;
  (void)m;
  if (!root) {
    return GTEXT_CSS_OK;
  }
  const GTEXT_CSS_Value * items = root;
  size_t n = 1;
  if (root->kind == GTEXT_CSS_VAL_LIST) {
    items = root->children;
    n = root->child_count;
  }
  if (n >= 3 || n == 0) {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Value * kids = css_vb_values(&a->vb, 3);
  GTEXT_CSS_Value * list = css_vb_value(&a->vb);
  if (!kids || !list) {
    return a->vb.status;
  }
  for (size_t i = 0; i < 3; i++) {
    if (i < n) {
      kids[i] = items[i];
    }
    else {
      css_v_keyword(&a->vb, &kids[i], "auto");
    }
  }
  css_v_list(list, kids, 3, GTEXT_CSS_SEP_SPACE);
  *out_root = list;
  return a->vb.status;
}

static GTEXT_CSS_Status build_image_orientation(css_state * st, ab * a, const GTEXT_CSS_Value_Set * bt, const GTEXT_CSS_Value * root,
    const css_pmeta * m, GTEXT_CSS_Value ** out_root) {
  (void)bt;
  (void)m;
  if (!root || root->kind == GTEXT_CSS_VAL_KEYWORD) {
    return GTEXT_CSS_OK;
  }
  const GTEXT_CSS_Value * leaves[FLAT_MAX];
  size_t n = 0;
  GTEXT_CSS_Status s = flatten(st, root, leaves, &n);
  if (s != GTEXT_CSS_OK) {
    return s;
  }
  double deg = 0.0;
  bool flip = false;
  for (size_t i = 0; i < n; i++) {
    if (leaves[i]->kind == GTEXT_CSS_VAL_DIMENSION) {
      deg = leaves[i]->number;
    }
    else if (kw(leaves[i], "flip")) {
      flip = true;
    }
  }
  /* rounded to the nearest quarter turn and normalized to 0 up to 360 (Images 3, 5.2) */
  double q = deg / 90.0 + 0.5;
  const double fl = gmath_floor(q);
  /* the quarter turns, modulo four, in doubles: an angle of 10^30 degrees is no integer to cast */
  double rem = fl - 4.0 * gmath_floor(fl / 4.0);
  if (!(rem >= 0.0 && rem < 4.0)) {
    rem = 0.0;
  }
  const long long quarters = (long long)rem;
  GTEXT_CSS_Value ang;
  memset(&ang, 0, sizeof ang);
  css_v_dimension(&ang, (double)quarters * 90.0, GTEXT_CSS_UNIT_DEG);
  GTEXT_CSS_Value * top = css_vb_value(&a->vb);
  if (!top) {
    return a->vb.status;
  }
  if (!flip) {
    *top = ang;
    *out_root = top;
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Value * kids = css_vb_values(&a->vb, 2);
  if (!kids) {
    return a->vb.status;
  }
  kids[0] = ang;
  css_v_keyword(&a->vb, &kids[1], "flip");
  css_v_list(top, kids, 2, GTEXT_CSS_SEP_SPACE);
  *out_root = top;
  return a->vb.status;
}

/* ---- transforms ------------------------------------------------------------------------------------------------------ */

static GTEXT_CSS_Status build_translate(css_state * st, ab * a, const GTEXT_CSS_Value_Set * bt, const GTEXT_CSS_Value * root, const css_pmeta * m,
    GTEXT_CSS_Value ** out_root) {
  (void)bt;
  (void)m;
  if (!root || root->kind == GTEXT_CSS_VAL_KEYWORD) {
    return GTEXT_CSS_OK;
  }
  const GTEXT_CSS_Value * leaves[FLAT_MAX];
  size_t n = 0;
  GTEXT_CSS_Status s = flatten(st, root, leaves, &n);
  if (s != GTEXT_CSS_OK) {
    return s;
  }
  if (n < 1 || n > 3) {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Value * kids = css_vb_values(&a->vb, 3);
  GTEXT_CSS_Value * list = css_vb_value(&a->vb);
  if (!kids || !list) {
    return a->vb.status;
  }
  for (size_t i = 0; i < 3; i++) {
    if (i < n) {
      kids[i] = *leaves[i];
    }
    else {
      css_v_dimension(&kids[i], 0.0, GTEXT_CSS_UNIT_PX);
    }
  }
  css_v_list(list, kids, 3, GTEXT_CSS_SEP_SPACE);
  *out_root = list;
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status build_scale(css_state * st, ab * a, const GTEXT_CSS_Value_Set * bt, const GTEXT_CSS_Value * root, const css_pmeta * m,
    GTEXT_CSS_Value ** out_root) {
  (void)bt;
  (void)m;
  if (!root || root->kind == GTEXT_CSS_VAL_KEYWORD) {
    return GTEXT_CSS_OK;
  }
  const GTEXT_CSS_Value * leaves[FLAT_MAX];
  size_t n = 0;
  GTEXT_CSS_Status s = flatten(st, root, leaves, &n);
  if (s != GTEXT_CSS_OK) {
    return s;
  }
  if (n < 1 || n > 3) {
    return GTEXT_CSS_OK;
  }
  double v[3] = {1.0, 1.0, 1.0};
  for (size_t i = 0; i < n; i++) {
    v[i] = leaves[i]->kind == GTEXT_CSS_VAL_PERCENTAGE ? leaves[i]->number / 100.0 : leaves[i]->number;
  }
  if (n == 1) {
    v[1] = v[0];
  }
  GTEXT_CSS_Value * kids = css_vb_values(&a->vb, 3);
  GTEXT_CSS_Value * list = css_vb_value(&a->vb);
  if (!kids || !list) {
    return a->vb.status;
  }
  for (size_t i = 0; i < 3; i++) {
    css_v_number(&kids[i], v[i]);
  }
  css_v_list(list, kids, 3, GTEXT_CSS_SEP_SPACE);
  *out_root = list;
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status build_rotate(css_state * st, ab * a, const GTEXT_CSS_Value_Set * bt, const GTEXT_CSS_Value * root, const css_pmeta * m,
    GTEXT_CSS_Value ** out_root) {
  (void)bt;
  (void)m;
  if (!root || root->kind == GTEXT_CSS_VAL_KEYWORD) {
    return GTEXT_CSS_OK;
  }
  const GTEXT_CSS_Value * leaves[FLAT_MAX];
  size_t n = 0;
  GTEXT_CSS_Status s = flatten(st, root, leaves, &n);
  if (s != GTEXT_CSS_OK) {
    return s;
  }
  double axis[3] = {0.0, 0.0, 1.0};
  const GTEXT_CSS_Value * angle = NULL;
  size_t nums = 0;
  for (size_t i = 0; i < n; i++) {
    const GTEXT_CSS_Value * l = leaves[i];
    if (l->kind == GTEXT_CSS_VAL_DIMENSION) {
      angle = l;
    }
    else if (kw(l, "x")) {
      axis[0] = 1; axis[1] = 0; axis[2] = 0;
    }
    else if (kw(l, "y")) {
      axis[0] = 0; axis[1] = 1; axis[2] = 0;
    }
    else if (kw(l, "z")) {
      axis[0] = 0; axis[1] = 0; axis[2] = 1;
    }
    else if (is_number(l) && nums < 3) {
      axis[nums++] = l->number;
    }
  }
  if (!angle) {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Value * nums3 = css_vb_values(&a->vb, 3);
  GTEXT_CSS_Value * axis_list = css_vb_value(&a->vb);
  GTEXT_CSS_Value * kids = css_vb_values(&a->vb, 2);
  GTEXT_CSS_Value * top = css_vb_value(&a->vb);
  if (!nums3 || !axis_list || !kids || !top) {
    return a->vb.status;
  }
  for (size_t i = 0; i < 3; i++) {
    css_v_number(&nums3[i], axis[i]);
  }
  css_v_list(axis_list, nums3, 3, GTEXT_CSS_SEP_SPACE);
  kids[0] = *axis_list;
  kids[1] = *angle;
  css_v_list(top, kids, 2, GTEXT_CSS_SEP_SPACE);
  *out_root = top;
  return GTEXT_CSS_OK;
}

/* ---- shadows ------------------------------------------------------------------------------------------------------- */

static GTEXT_CSS_Status shadow_item(css_state * st, ab * a, const css_pmeta * m, const GTEXT_CSS_Value * item, GTEXT_CSS_Value * out, bool * keep) {
  (void)m;
  const GTEXT_CSS_Value * leaves[FLAT_MAX];
  size_t n = 0;
  GTEXT_CSS_Status s = flatten(st, item, leaves, &n);
  if (s != GTEXT_CSS_OK) {
    return s;
  }
  const GTEXT_CSS_Value * color = NULL;
  const GTEXT_CSS_Value * len[4] = {NULL, NULL, NULL, NULL};
  size_t nl = 0;
  bool inset = false;
  for (size_t i = 0; i < n; i++) {
    const GTEXT_CSS_Value * l = leaves[i];
    if (l->kind == GTEXT_CSS_VAL_COLOR) {
      color = l;
    }
    else if (kw(l, "inset")) {
      inset = true;
    }
    else if (nl < 4 && (l->kind == GTEXT_CSS_VAL_DIMENSION || l->kind == GTEXT_CSS_VAL_ZERO || ((l->flags & GTEXT_CSS_VFLAG_MATH) && l->math))) {
      len[nl++] = l;
    }
    else {
      *keep = true;
      return GTEXT_CSS_OK;
    }
  }
  if (nl < 2) {
    *keep = true;
    return GTEXT_CSS_OK;
  }
  const size_t count = 5 + (inset ? 1 : 0);
  GTEXT_CSS_Value * kids = css_vb_values(&a->vb, count);
  if (!kids) {
    return a->vb.status;
  }
  if (color) {
    kids[0] = *color;
  }
  else {
    /* an omitted color is currentcolor */
    GTEXT_CSS_Property_Options po;
    memset(&po, 0, sizeof po);
    po.allocator = &st->cc->alloc;
    GTEXT_CSS_Property_Value pv;
    memset(&pv, 0, sizeof pv);
    const GTEXT_CSS_Property_Id cid = st->cc->pid[CSS_P_COLOR];
    GTEXT_CSS_Status ps = GTEXT_CSS_E_UNSUPPORTED;
    if (cid) {
      ps = gtext_css_parse_property_text(st->cc->registry, st->cc->meta[cid - 1].name, st->cc->meta[cid - 1].name_length, "currentcolor", 12, &po, &pv);
    }
    if (ps != GTEXT_CSS_OK || !gtext_css_value_set_root(pv.set)) {
      gtext_css_property_value_free(&pv);
      /* a failed allocation is a failed allocation, not a registry without a color */
      return ps == GTEXT_CSS_E_OOM ? css_st_fail(st, GTEXT_CSS_E_OOM, NULL, NULL)
                                   : css_st_fail(st, GTEXT_CSS_E_UNSUPPORTED, "currentcolor", "the registry cannot make a color");
    }
    kids[0] = *gtext_css_value_set_root(pv.set);
    const bool ok = css_vb_dep(&a->vb, pv.set);
    gtext_css_property_value_free(&pv);
    if (!ok) {
      return a->vb.status;
    }
  }
  for (size_t i = 0; i < 4; i++) {
    if (len[i]) {
      kids[1 + i] = *len[i];
    }
    else {
      css_v_dimension(&kids[1 + i], 0.0, GTEXT_CSS_UNIT_PX);
    }
  }
  if (inset) {
    css_v_keyword(&a->vb, &kids[5], "inset");
  }
  memset(out, 0, sizeof *out);
  css_v_list(out, kids, count, GTEXT_CSS_SEP_SPACE);
  return a->vb.status;
}

static GTEXT_CSS_Status build_shadow(css_state * st, ab * a, const GTEXT_CSS_Value_Set * bt, const GTEXT_CSS_Value * root, const css_pmeta * m,
    GTEXT_CSS_Value ** out_root) {
  (void)bt;
  if (!root || root->kind == GTEXT_CSS_VAL_KEYWORD) {
    return GTEXT_CSS_OK;
  }
  return build_items(st, a, m, root, shadow_item, out_root);
}

/* ---- the dispatcher ----------------------------------------------------------------------------------------------------- */

GTEXT_CSS_Status css_algo_compute(css_state * st, GTEXT_CSS_Property_Id id, const css_pmeta * m, const GTEXT_CSS_Value_Set * src,
    GTEXT_CSS_Value_Set ** out) {
  switch ((GTEXT_CSS_Compute_Algorithm)m->algo) {
  case GTEXT_CSS_ALGO_FONT_SIZE: return algo_font_size(st, id, src, out);
  case GTEXT_CSS_ALGO_FONT_WEIGHT: return algo_font_weight(st, id, src, out);
  case GTEXT_CSS_ALGO_FONT_WIDTH: return algo_font_width(st, id, src, out);
  case GTEXT_CSS_ALGO_FONT_STYLE: return algo_font_style(st, id, src, out);
  case GTEXT_CSS_ALGO_LINE_HEIGHT: return algo_line_height(st, id, src, out);
  case GTEXT_CSS_ALGO_FONT_SIZE_ADJUST: return algo_font_size_adjust(st, id, src, out);
  case GTEXT_CSS_ALGO_BORDER_WIDTH: return algo_border_width(st, id, m, src, out);
  case GTEXT_CSS_ALGO_RULE_WIDTH: return algo_width_list(st, id, src, true, false, out);
  case GTEXT_CSS_ALGO_LINE_WIDTH: return algo_width_list(st, id, src, false, true, out);
  case GTEXT_CSS_ALGO_DISPLAY: return algo_display(st, src, out);
  case GTEXT_CSS_ALGO_FLOAT: return algo_float(st, src, out);
  case GTEXT_CSS_ALGO_COLOR: return algo_color(st, id, src, out);
  case GTEXT_CSS_ALGO_OPACITY: return algo_opacity(st, id, src, out);
  case GTEXT_CSS_ALGO_ZOOM: return algo_zoom(st, id, src, out);
  case GTEXT_CSS_ALGO_MATH_DEPTH: return algo_math_depth(st, id, src, out);
  case GTEXT_CSS_ALGO_TEXT_ALIGN: return algo_text_align(st, id, src, out);
  case GTEXT_CSS_ALGO_VOICE_PITCH: return algo_voice_pitch(st, id, src, out);
  case GTEXT_CSS_ALGO_VOICE_BALANCE: return algo_voice_balance(st, id, src, out);
  case GTEXT_CSS_ALGO_WRAP_FLOW: return algo_wrap_flow(st, id, src, out);
  case GTEXT_CSS_ALGO_HYPHENATE_CHARS: return run_build(st, id, src, 0, build_hyphenate, m, out);
  case GTEXT_CSS_ALGO_IMAGE_ORIENTATION: return run_build(st, id, src, 0, build_image_orientation, m, out);
  case GTEXT_CSS_ALGO_PAIR_DUP: return run_build(st, id, src, 0, build_pair_dup, m, out);
  case GTEXT_CSS_ALGO_BOX4: return run_build(st, id, src, 0, build_box4, m, out);
  case GTEXT_CSS_ALGO_REPEAT: return run_build(st, id, src, 0, build_repeat, m, out);
  case GTEXT_CSS_ALGO_POSITION: return run_build(st, id, src, 0, build_position, m, out);
  case GTEXT_CSS_ALGO_POSITION_AXIS: return run_build(st, id, src, 0, build_axis, m, out);
  case GTEXT_CSS_ALGO_BG_SIZE: return run_build(st, id, src, 0, build_size, m, out);
  case GTEXT_CSS_ALGO_TRANSLATE: return run_build(st, id, src, 0, build_translate, m, out);
  case GTEXT_CSS_ALGO_SCALE: return run_build(st, id, src, 0, build_scale, m, out);
  case GTEXT_CSS_ALGO_ROTATE: return run_build(st, id, src, 0, build_rotate, m, out);
  case GTEXT_CSS_ALGO_SHADOW: return run_build(st, id, src, 0, build_shadow, m, out);
  case GTEXT_CSS_ALGO_WORD_SPACING: return algo_word_spacing(st, id, src, out);
  case GTEXT_CSS_ALGO_LETTER_SPACING: return algo_letter_spacing(st, id, src, out);
  case GTEXT_CSS_ALGO_DASHARRAY: return run_build(st, id, src, CV_NUMBERS_TO_PX, build_dasharray, m, out);
  case GTEXT_CSS_ALGO_NUMBER_LENGTH: return css_cv_by_type_ex(st, src, id, CV_NUMBERS_TO_PX, out);
  default:
    return css_st_fail(st, GTEXT_CSS_E_INTERNAL, m->name, "a property has an algorithm that is not implemented");
  }
}
