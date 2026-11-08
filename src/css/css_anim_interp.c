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
 * The combination of two computed values by their type (Web Animations 1, "Combining effects"; CSS Values 4, "Combining values: interpolation, addition, and accumulation").
 *
 * **By computed value type.** Two values of one shape are combined leaf by leaf: a number, an integer, a percentage and a dimension by arithmetic on the number (an integer is
 * rounded), a length with a percentage (or a calculation) by a `calc()` that the computed layer simplifies, a color premultiplied in sRGB (legacy colors) or in Oklab (the others),
 * and a list, a function or a block by its items. A pair of another shape, or one whose keywords, strings or identifiers differ, is discrete: "flips at 50%". The result is written as
 * the text of a value, which the caller declares in the animation origin and the computed layer computes.
 */

#include <string.h>

#include <ghoti.io/math/basic.h>

#include "css_anim_internal.h"
#include "css_compute_internal.h"
#include "css_shorthand_internal.h"

#define MAX_DEPTH 48

typedef struct {
  css_vb vb;
  css_blend_op op;
  double p;
  const GTEXT_CSS_Value * current;   ///< The element's `color`, or NULL.
  GTEXT_CSS_Status status;
} bctx;

/* ---- the shape of two values ---------------------------------------------------------------------------------------------------------------------- */

static bool same_text(const GTEXT_CSS_String * a, const GTEXT_CSS_String * b) {
  if (a->length != b->length) {
    return false;
  }
  for (size_t i = 0; i < a->length; i++) {
    char x = a->data[i], y = b->data[i];
    if (x >= 'A' && x <= 'Z') {
      x = (char)(x + 32);
    }
    if (y >= 'A' && y <= 'Z') {
      y = (char)(y + 32);
    }
    if (x != y) {
      return false;
    }
  }
  return true;
}

static bool exact_text(const GTEXT_CSS_String * a, const GTEXT_CSS_String * b) {
  return a->length == b->length && (a->length == 0 || memcmp(a->data, b->data, a->length) == 0);
}

/// `currentcolor`, as a color value or as a keyword.
static bool is_current(const GTEXT_CSS_Value * v) {
  static const char word[] = "currentcolor";
  if ((v->kind != GTEXT_CSS_VAL_COLOR && v->kind != GTEXT_CSS_VAL_KEYWORD) || v->text.length != sizeof word - 1) {
    return false;
  }
  GTEXT_CSS_String w = {word, sizeof word - 1};
  return same_text(&v->text, &w);
}

static bool color_like(const GTEXT_CSS_Value * v) {
  return v->kind == GTEXT_CSS_VAL_COLOR || is_current(v);
}

static bool is_length_unit(GTEXT_CSS_Unit u) {
  return gtext_css_unit_type(u) == GTEXT_CSS_UNIT_TYPE_LENGTH;
}

/// A value that is a length or a percentage or a calculation of them: the pair that makes `calc()` of two different kinds.
static bool is_lp(const GTEXT_CSS_Value * v) {
  if (v->math) {
    return true;
  }
  return v->kind == GTEXT_CSS_VAL_PERCENTAGE || (v->kind == GTEXT_CSS_VAL_DIMENSION && is_length_unit(v->unit)) || (v->kind == GTEXT_CSS_VAL_ZERO && (v->flags & GTEXT_CSS_VFLAG_ZERO_LENGTH));
}

static bool is_plain_number(const GTEXT_CSS_Value * v) {
  return !v->math && (v->kind == GTEXT_CSS_VAL_NUMBER || v->kind == GTEXT_CSS_VAL_INTEGER || v->kind == GTEXT_CSS_VAL_PERCENTAGE || v->kind == GTEXT_CSS_VAL_ZERO || v->kind == GTEXT_CSS_VAL_DIMENSION);
}

static bool compat(const GTEXT_CSS_Value * a, const GTEXT_CSS_Value * b, int depth) {
  if (depth > MAX_DEPTH) {
    return false;
  }
  if (is_lp(a) && is_lp(b)) {
    return true;
  }
  if (color_like(a) && color_like(b)) {
    return true;
  }
  if (a->math || b->math) {
    return a->kind == b->kind && a->math && b->math;
  }
  if (a->kind != b->kind) {
    /* a unitless zero is a number as well as a length */
    return (a->kind == GTEXT_CSS_VAL_ZERO && b->kind == GTEXT_CSS_VAL_NUMBER) || (a->kind == GTEXT_CSS_VAL_NUMBER && b->kind == GTEXT_CSS_VAL_ZERO);
  }
  switch (a->kind) {
    case GTEXT_CSS_VAL_KEYWORD:
      return same_text(&a->text, &b->text);
    case GTEXT_CSS_VAL_CUSTOM_IDENT:
    case GTEXT_CSS_VAL_DASHED_IDENT:
    case GTEXT_CSS_VAL_STRING:
    case GTEXT_CSS_VAL_URL:
    case GTEXT_CSS_VAL_UNPARSED:
    case GTEXT_CSS_VAL_PENDING:
    case GTEXT_CSS_VAL_TOKEN:
      return exact_text(&a->text, &b->text);
    case GTEXT_CSS_VAL_NUMBER:
    case GTEXT_CSS_VAL_INTEGER:
    case GTEXT_CSS_VAL_PERCENTAGE:
    case GTEXT_CSS_VAL_ZERO:
      return true;
    case GTEXT_CSS_VAL_DIMENSION: {
      if (a->unit == b->unit) {
        return a->unit != GTEXT_CSS_UNIT_NONE;
      }
      const GTEXT_CSS_Unit_Type t = gtext_css_unit_type(a->unit);
      return t != GTEXT_CSS_UNIT_TYPE_NONE && t == gtext_css_unit_type(b->unit) && gtext_css_unit_is_absolute(a->unit) && gtext_css_unit_is_absolute(b->unit) &&
          t != GTEXT_CSS_UNIT_TYPE_FLEX;
    }
    case GTEXT_CSS_VAL_COLOR:
      return true;
    case GTEXT_CSS_VAL_LIST:
      if (a->separator != b->separator || a->child_count != b->child_count) {
        return false;
      }
      for (size_t i = 0; i < a->child_count; i++) {
        if (!compat(&a->children[i], &b->children[i], depth + 1)) {
          return false;
        }
      }
      return true;
    case GTEXT_CSS_VAL_FUNCTION:
    case GTEXT_CSS_VAL_BLOCK:
      if (!same_text(&a->text, &b->text) || a->child_count != b->child_count) {
        return false;
      }
      for (size_t i = 0; i < a->child_count; i++) {
        if (!compat(&a->children[i], &b->children[i], depth + 1)) {
          return false;
        }
      }
      return true;
    default:
      return false;   /* a ratio, a unicode range ...: discrete */
  }
}

bool css_interp_compatible(const GTEXT_CSS_Value * a, const GTEXT_CSS_Value * b) {
  return a && b && compat(a, b, 0);
}

/* ---- arithmetic -------------------------------------------------------------------------------------------------------------------------------- */

static double comb(const bctx * c, double a, double b) {
  return c->op == CSS_BLEND_LERP ? a + (b - a) * c->p : a + b;
}

static double clamp(double v, double lo, double hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

/// A number as the shortest digits that read back as the same double (the math library's), NUL-terminated; `cap` is at least 40.
static size_t num_text(char * buf, size_t cap, double v) {
  (void)cap;
  const size_t n = css_exact_number(v, buf);
  buf[n] = '\0';
  return n;
}

static void free_text(const GTEXT_Allocator * alloc, char * text) {
  GTEXT_CSS_Serialize_Options so;
  memset(&so, 0, sizeof so);
  so.allocator = alloc;
  gtext_css_serialize_buffer_free(&so, text);
}

static char * value_text(const GTEXT_Allocator * alloc, const GTEXT_CSS_Value * v, size_t * length) {
  GTEXT_CSS_Serialize_Options so;
  memset(&so, 0, sizeof so);
  so.allocator = alloc;
  char * out = NULL;
  size_t n = 0;
  if (gtext_css_serialize_value_buffer(v, &so, &out, &n) != GTEXT_CSS_OK) {
    return NULL;
  }
  *length = n;
  return out;
}

/* ---- the leaves -------------------------------------------------------------------------------------------------------------------------------- */

static GTEXT_CSS_Value * text_node(bctx * c, const char * text, size_t n) {
  GTEXT_CSS_Value * v = css_vb_value(&c->vb);
  if (!v) {
    return NULL;
  }
  v->kind = GTEXT_CSS_VAL_UNPARSED;
  v->text.data = css_vb_str(&c->vb, text, n);
  v->text.length = n;
  return v->text.data ? v : NULL;
}

/// A calculation of a pair that is a length and a percentage in some order: `calc((a) * wa + (b) * wb)` for the computed layer to simplify.
static const GTEXT_CSS_Value * calc_node(bctx * c, const GTEXT_CSS_Value * a, const GTEXT_CSS_Value * b) {
  size_t na = 0, nb = 0;
  const GTEXT_Allocator * al = &c->vb.alloc;
  char * ta = value_text(al, a, &na);
  char * tb = value_text(al, b, &nb);
  if (!ta || !tb) {
    free_text(al, ta);
    free_text(al, tb);
    css_vb_fail(&c->vb, GTEXT_CSS_E_OOM);
    return NULL;
  }
  const double wa = c->op == CSS_BLEND_LERP ? 1.0 - c->p : 1.0;
  const double wb = c->op == CSS_BLEND_LERP ? c->p : 1.0;
  char na_[64], nb_[64];
  const size_t la = num_text(na_, sizeof na_, wa), lb = num_text(nb_, sizeof nb_, wb);
  const size_t total = 5 + 2 + na + 4 + la + 3 + 2 + nb + 4 + lb + 2 + 1;
  char * buf = gtext_allocator_malloc(al, total + 1);
  if (!buf) {
    free_text(al, ta);
    free_text(al, tb);
    css_vb_fail(&c->vb, GTEXT_CSS_E_OOM);
    return NULL;
  }
  size_t at = 0;
#define PUT(s, n) (memcpy(buf + at, (s), (n)), at += (n))
  PUT("calc(", 5);
  PUT("(", 1);
  PUT(ta, na);
  PUT(") * ", 4);
  PUT(na_, la);
  PUT(" + ", 3);
  PUT("(", 1);
  PUT(tb, nb);
  PUT(") * ", 4);
  PUT(nb_, lb);
  PUT(")", 1);
#undef PUT
  free_text(al, ta);
  free_text(al, tb);
  GTEXT_CSS_Value * v = text_node(c, buf, at);
  gtext_allocator_free(al, buf);
  return v;
}

/// The color a value stands for when it is combined: the element's own for `currentcolor` ("Interpolating to or from currentcolor is possible. The numerical value used for this
/// purpose is the used value", Color 4).
static const GTEXT_CSS_Value * used_color(const bctx * c, const GTEXT_CSS_Value * v) {
  return is_current(v) && c->current && c->current->kind == GTEXT_CSS_VAL_COLOR && !is_current(c->current) ? c->current : v;
}

static const GTEXT_CSS_Value * color_node(bctx * c, const GTEXT_CSS_Value * a, const GTEXT_CSS_Value * b) {
  double ra[4], rb[4];
  char buf[200];
  a = used_color(c, a);
  b = used_color(c, b);
  if (a->color && b->color && gtext_css_color_srgb(a->color, ra) && gtext_css_color_srgb(b->color, rb)) {
    /* "premultiplied" legacy sRGB interpolation, as both browsers do for colors written as rgb(), hsl(), hex and names */
    const double alpha = clamp(comb(c, ra[3], rb[3]), 0.0, 1.0);
    double ch[3] = {0, 0, 0};
    if (alpha > 0.0) {
      for (int i = 0; i < 3; i++) {
        ch[i] = clamp(comb(c, ra[i] * ra[3], rb[i] * rb[3]) / alpha, 0.0, 255.0);
      }
    }
    memcpy(buf, "rgb(", 4);
    size_t at = 4;
    for (int i = 0; i < 3; i++) {
      at += num_text(buf + at, 40, ch[i]);
      buf[at++] = i < 2 ? ' ' : (alpha < 1.0 ? ' ' : ')');
      if (i == 2 && alpha < 1.0) {
        buf[at++] = '/';
        buf[at++] = ' ';
        at += num_text(buf + at, 40, alpha);
        buf[at++] = ')';
      }
    }
    return text_node(c, buf, at);
  }
  if (c->op != CSS_BLEND_LERP) {
    return b;   /* "replace": addition of colors outside legacy sRGB is not made here */
  }
  /* any other pair is mixed in Oklab, which is what Color 4 gives the animation of colors that are not both legacy */
  const double p = clamp(c->p, 0.0, 1.0);
  char pa[64], pb[64];
  num_text(pa, sizeof pa, (1.0 - p) * 100.0);
  num_text(pb, sizeof pb, p * 100.0);
  const char * head = "color-mix(in oklab, ";
  char * big = gtext_allocator_malloc(&c->vb.alloc, a->text.length + b->text.length + 160);
  if (!big) {
    css_vb_fail(&c->vb, GTEXT_CSS_E_OOM);
    return NULL;
  }
  size_t at = 0;
#define PUT(s, n) (memcpy(big + at, (s), (n)), at += (n))
  PUT(head, strlen(head));
  PUT(a->text.data, a->text.length);
  PUT(" ", 1);
  PUT(pa, strlen(pa));
  PUT("%, ", 3);
  PUT(b->text.data, b->text.length);
  PUT(" ", 1);
  PUT(pb, strlen(pb));
  PUT("%)", 2);
#undef PUT
  GTEXT_CSS_Value * v = text_node(c, big, at);
  gtext_allocator_free(&c->vb.alloc, big);
  return v;
}

/// Whether the function has this name (ASCII case-insensitive).
static bool fn_is(const GTEXT_CSS_Value * f, const char * name) {
  const size_t n = strlen(name);
  if (f->text.length != n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    char c = f->text.data[i];
    if (c >= 'A' && c <= 'Z') {
      c = (char)(c + 32);
    }
    if (c != name[i]) {
      return false;
    }
  }
  return true;
}

/// Clamp the numbers of the lists of a function's arguments that are radii, in the order they are written: `in_radii` starts true for the arguments of circle() and ellipse(), which end at the
/// literal `at`, and false for inset(), rect() and xywh(), whose radii follow the literal `round` (a literal of the grammar is a keyword or a preserved token, inside lists as the grammar
/// nests them).
static void clamp_walk(GTEXT_CSS_Value * list, bool circular, bool rounded, bool * in_radii) {
  for (size_t i = 0; i < list->child_count; i++) {
    /* a node of the value being built here, owned by its builder: not the caller's */
    GTEXT_CSS_Value * k = (GTEXT_CSS_Value *)(uintptr_t)&list->children[i];
    switch (k->kind) {
      case GTEXT_CSS_VAL_KEYWORD:
      case GTEXT_CSS_VAL_TOKEN:
        if (circular && k->text.length == 2 && k->text.data[0] == 'a' && k->text.data[1] == 't') {
          *in_radii = false;
        }
        else if (rounded && k->text.length == 5 && memcmp(k->text.data, "round", 5) == 0) {
          *in_radii = true;
        }
        break;
      case GTEXT_CSS_VAL_LIST:
        clamp_walk(k, circular, rounded, in_radii);
        break;
      case GTEXT_CSS_VAL_DIMENSION:
      case GTEXT_CSS_VAL_PERCENTAGE:
      case GTEXT_CSS_VAL_NUMBER:
        if (*in_radii && k->number < 0) {
          k->number = 0;
        }
        break;
      default:
        break;
    }
  }
}

/// CSS Values 4, "Interpolation": "the result of interpolation is clamped to the allowed range of the value"; in a basic shape (CSS Shapes 1) the radii of circle() and ellipse() (the arguments
/// before `at`) and the corner radii of inset(), rect() and xywh() (those after `round`) are `<length-percentage [0,∞]>`, and a timing function that overshoots (a negative easing) makes them
/// negative: the page basic-shape-interpolation.html expects `circle(0px)` for a radius that the easing took below zero. The offsets of inset() and the position after `at` may be negative.
static void clamp_radii(GTEXT_CSS_Value * f) {
  const bool circular = fn_is(f, "circle") || fn_is(f, "ellipse");
  const bool rounded = fn_is(f, "inset") || fn_is(f, "rect") || fn_is(f, "xywh");
  if (!circular && !rounded) {
    return;
  }
  bool in_radii = circular;
  clamp_walk(f, circular, rounded, &in_radii);
}

static const GTEXT_CSS_Value * blend(bctx * c, const GTEXT_CSS_Value * a, const GTEXT_CSS_Value * b) {
  if (c->vb.status != GTEXT_CSS_OK) {
    return NULL;
  }
  /* the two lengths that differ in kind, or a calculation: one calculation */
  const bool both_plain = is_plain_number(a) && is_plain_number(b);
  if (is_lp(a) && is_lp(b) && !(both_plain && a->kind == b->kind && a->unit == b->unit)) {
    return calc_node(c, a, b);
  }
  if (color_like(a) && color_like(b)) {
    return color_node(c, a, b);
  }
  GTEXT_CSS_Value * v = css_vb_value(&c->vb);
  if (!v) {
    return NULL;
  }
  *v = *a;
  switch (a->kind) {
    case GTEXT_CSS_VAL_NUMBER:
    case GTEXT_CSS_VAL_INTEGER:
    case GTEXT_CSS_VAL_PERCENTAGE:
    case GTEXT_CSS_VAL_ZERO:
    case GTEXT_CSS_VAL_DIMENSION: {
      double x = a->number, y = b->number;
      GTEXT_CSS_Unit unit = a->unit;
      if (a->kind == GTEXT_CSS_VAL_DIMENSION && a->unit != b->unit) {
        if (!gtext_css_unit_to_canonical(a->unit, a->number, &x) || !gtext_css_unit_to_canonical(b->unit, b->number, &y)) {
          css_vb_fail(&c->vb, GTEXT_CSS_E_INTERNAL);
          return NULL;
        }
        unit = gtext_css_unit_canonical(a->unit);
      }
      double r = comb(c, x, y);
      v->kind = a->kind;
      if (a->kind == GTEXT_CSS_VAL_ZERO) {
        v->kind = GTEXT_CSS_VAL_NUMBER;   /* a number between a zero and a number; the other kinds do not reach here */
      }
      if (a->kind == GTEXT_CSS_VAL_INTEGER) {
        r = gmath_floor(r + 0.5);
      }
      v->number = r;
      v->unit = unit;
      v->flags = 0;
      v->component = 0;
      v->text.data = NULL;
      v->text.length = 0;
      return v;
    }
    case GTEXT_CSS_VAL_LIST:
    case GTEXT_CSS_VAL_FUNCTION:
    case GTEXT_CSS_VAL_BLOCK: {
      GTEXT_CSS_Value * kids = css_vb_values(&c->vb, a->child_count);
      if (!kids) {
        return NULL;
      }
      for (size_t i = 0; i < a->child_count; i++) {
        const GTEXT_CSS_Value * k = blend(c, &a->children[i], &b->children[i]);
        if (!k) {
          return NULL;
        }
        kids[i] = *k;
      }
      v->children = kids;
      v->child_count = a->child_count;
      if (a->kind == GTEXT_CSS_VAL_FUNCTION) {
        clamp_radii(v);
      }
      return v;
    }
    default:
      return a;   /* equal keywords, strings, identifiers: the value itself */
  }
}

/* ---- the entry point ---------------------------------------------------------------------------------------------------------------------------- */

static GTEXT_CSS_Status finish(const GTEXT_Allocator * alloc, const GTEXT_CSS_Value * v, char ** text, size_t * length) {
  GTEXT_CSS_Serialize_Options so;
  memset(&so, 0, sizeof so);
  so.allocator = alloc;
  return gtext_css_serialize_value_buffer(v, &so, text, length);
}

GTEXT_CSS_Status css_interp_text(const GTEXT_Allocator * alloc, css_blend_op op, const GTEXT_CSS_Value * a, const GTEXT_CSS_Value * b, double p, const GTEXT_CSS_Value * current_color,
    char ** text, size_t * length, bool * discrete) {
  if (discrete) {
    *discrete = false;
  }
  if (!a || !b || !text || !length) {
    return GTEXT_CSS_E_INVALID;
  }
  if (!alloc) {
    alloc = gtext_allocator_default();
  }
  if (op == CSS_BLEND_LERP && p == 0.0) {
    return finish(alloc, a, text, length);
  }
  if (op == CSS_BLEND_LERP && p == 1.0) {
    return finish(alloc, b, text, length);
  }
  if (!compat(a, b, 0)) {
    if (discrete) {
      *discrete = true;
    }
    return finish(alloc, op == CSS_BLEND_LERP && p < 0.5 ? a : b, text, length);
  }
  bctx c;
  c.op = op;
  c.p = p;
  c.current = current_color;
  c.status = GTEXT_CSS_OK;
  if (!css_vb_begin(&c.vb, alloc, 0)) {
    return c.vb.status;
  }
  const GTEXT_CSS_Value * r = blend(&c, a, b);
  GTEXT_CSS_Status st = c.vb.status;
  if (st == GTEXT_CSS_OK && r) {
    st = finish(alloc, r, text, length);
  }
  else if (st == GTEXT_CSS_OK) {
    st = GTEXT_CSS_E_INTERNAL;
  }
  css_vb_abort(&c.vb);
  return st;
}
