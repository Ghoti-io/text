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
 * Images (CSS Images 3 and 4), the <position> type they use, and the color interpolation method of gradients.
 *
 * Reference: CSS Images Module Level 4, Editor's Draft of 8 October 2026 (served 2026-10-22); CSS Values 4 section 8.3; CSS Color 5 section 9.1.
 *
 * The grammars are the drafts' own (Value Definition Syntax), retrieved 2026-10-22; where the
 * served draft differs from an older text the served one is followed and the difference is noted
 * beside the entry.
 * **Implicitly omitted commas are written out.** Values 4, 2.2 omits a comma from the match when the terms around it are omitted
 * (`steps(4)` for `steps( <integer> , <step-position>? )`). The engine keeps a literal comma that separates a possibly empty term
 * as a token, so the grammars here say it with a group (`steps( <integer> [ , <step-position> ]? )`), which matches the same inputs.
 *
 * They are matched by the Value Definition Syntax engine; nothing here is code.
 */

#include "css_function_internal.h"

static const css_fn_type TYPES[] = {
  /* Values 4, 8.3.2: the position, in its four forms (the three-value form is not in it, the draft says so) */
  {"position", GTEXT_CSS_REF_TYPE,
      "[ [ left | center | right | top | bottom | <length-percentage> ] | [ left | center | right ] && [ top | center | bottom ] | [ left | center | right | <length-percentage> ] [ top | center | bottom | <length-percentage> ] | [ [ left | right ] <length-percentage> ] && [ [ top | bottom ] <length-percentage> ] ]"},
  /* Images 4, 2: <image>; paint() is CSS Painting API Level 1; -webkit-image-set() is Images 4 appendix A's alias */
  {"image", GTEXT_CSS_REF_TYPE,
      "<url> | <image()> | <image-set()> | <-webkit-image-set()> | <cross-fade()> | <element()> | <paint()> | <gradient>"},
  /* Images 4, 2.5: the served draft has only this form (the older annotations moved to Images 5) */
  {"image()", GTEXT_CSS_REF_TYPE,
      "image( <color> )"},
  {"image-set()", GTEXT_CSS_REF_TYPE,
      "image-set( <image-set-option># )"},
  /* Images 4, appendix A: accepted as image-set() */
  {"-webkit-image-set()", GTEXT_CSS_REF_TYPE,
      "-webkit-image-set( <image-set-option># )"},
  {"image-set-option", GTEXT_CSS_REF_TYPE,
      "[ <image> | <string> ] [ <resolution> || type( <string> ) ]?"},
  {"cross-fade()", GTEXT_CSS_REF_TYPE,
      "cross-fade( <cf-image># )"},
  {"cf-image", GTEXT_CSS_REF_TYPE,
      "[ <image> | <color> ] && <percentage [0,100]>?"},
  /* Images 4, 3: <id-selector>, an ID selector, which is a hash token */
  {"element()", GTEXT_CSS_REF_TYPE,
      "element( <hash-token> )"},
  /* CSS Painting API 1: paint( <ident>, <declaration-value>? ) */
  {"paint()", GTEXT_CSS_REF_TYPE,
      "paint( <ident> [ , <declaration-value>? ]? )"},
  /* Images 4, 3 */
  {"gradient", GTEXT_CSS_REF_TYPE,
      "<linear-gradient()> | <repeating-linear-gradient()> | <radial-gradient()> | <repeating-radial-gradient()> | <conic-gradient()> | <repeating-conic-gradient()>"},
  {"linear-gradient()", GTEXT_CSS_REF_TYPE,
      "linear-gradient( <linear-gradient-syntax> )"},
  {"repeating-linear-gradient()", GTEXT_CSS_REF_TYPE,
      "repeating-linear-gradient( <linear-gradient-syntax> )"},
  {"linear-gradient-syntax", GTEXT_CSS_REF_TYPE,
      "[ [ [ <angle> | <zero> | to <side-or-corner> ] || <color-interpolation-method> ] , ]? <color-stop-list>"},
  {"side-or-corner", GTEXT_CSS_REF_TYPE,
      "[ left | right ] || [ top | bottom ]"},
  {"radial-gradient()", GTEXT_CSS_REF_TYPE,
      "radial-gradient( <radial-gradient-syntax> )"},
  {"repeating-radial-gradient()", GTEXT_CSS_REF_TYPE,
      "repeating-radial-gradient( <radial-gradient-syntax> )"},
  {"radial-gradient-syntax", GTEXT_CSS_REF_TYPE,
      "[ [ [ [ <radial-shape> || <radial-size> ]? [ at <position> ]? ]! || <color-interpolation-method> ] , ]? <color-stop-list>"},
  {"radial-shape", GTEXT_CSS_REF_TYPE,
      "circle | ellipse"},
  /* Images 4, 3.2 (Images 3 had one extent or one or two lengths) */
  {"radial-size", GTEXT_CSS_REF_TYPE,
      "<radial-extent>{1,2} | <length-percentage [0,∞]>{1,2}"},
  {"radial-extent", GTEXT_CSS_REF_TYPE,
      "closest-corner | closest-side | farthest-corner | farthest-side"},
  {"conic-gradient()", GTEXT_CSS_REF_TYPE,
      "conic-gradient( <conic-gradient-syntax> )"},
  {"repeating-conic-gradient()", GTEXT_CSS_REF_TYPE,
      "repeating-conic-gradient( <conic-gradient-syntax> )"},
  {"conic-gradient-syntax", GTEXT_CSS_REF_TYPE,
      "[ [ [ [ from [ <angle> | <zero> ] ]? [ at <position> ]? ]! || <color-interpolation-method> ] , ]? <angular-color-stop-list>"},
  {"color-stop-list", GTEXT_CSS_REF_TYPE,
      "<linear-color-stop> [ , [ [ <linear-color-hint> , ]? <linear-color-stop> ]# ]?"},
  {"linear-color-stop", GTEXT_CSS_REF_TYPE,
      "<color> <color-stop-length>?"},
  {"linear-color-hint", GTEXT_CSS_REF_TYPE,
      "<length-percentage>"},
  {"color-stop-length", GTEXT_CSS_REF_TYPE,
      "<length-percentage>{1,2}"},
  {"angular-color-stop-list", GTEXT_CSS_REF_TYPE,
      "<angular-color-stop> [ , [ [ <angular-color-hint> , ]? <angular-color-stop> ]# ]?"},
  {"angular-color-stop", GTEXT_CSS_REF_TYPE,
      "<color> <color-stop-angle>?"},
  {"angular-color-hint", GTEXT_CSS_REF_TYPE,
      "<angle-percentage> | <zero>"},
  {"color-stop-angle", GTEXT_CSS_REF_TYPE,
      "[ <angle-percentage> | <zero> ]{1,2}"},
  /* Color 5, 9.1 */
  {"color-interpolation-method", GTEXT_CSS_REF_TYPE,
      "in [ <rectangular-color-space> | <polar-color-space> <hue-interpolation-method>? | <custom-color-space> ]"},
  {"rectangular-color-space", GTEXT_CSS_REF_TYPE,
      "srgb | srgb-linear | display-p3 | display-p3-linear | a98-rgb | prophoto-rgb | rec2020 | lab | oklab | <xyz-space>"},
  {"polar-color-space", GTEXT_CSS_REF_TYPE,
      "hsl | hwb | lch | oklch"},
  {"custom-color-space", GTEXT_CSS_REF_TYPE,
      "<dashed-ident>"},
  {"xyz-space", GTEXT_CSS_REF_TYPE,
      "xyz | xyz-d50 | xyz-d65"},
  {"hue-interpolation-method", GTEXT_CSS_REF_TYPE,
      "[ shorter | longer | increasing | decreasing ] hue"},
};

const css_fn_type * css_image_types(size_t * count) {
  *count = sizeof TYPES / sizeof TYPES[0];
  return TYPES;
}

/* ---- the typed views ------------------------------------------------------------------------ */

#include <string.h>

#include <ghoti.io/text/css/css_color.h>

GTEXT_CSS_Image_Kind gtext_css_image_kind(const GTEXT_CSS_Value * v) {
  if (!v) {
    return GTEXT_CSS_IMAGE_NONE;
  }
  if (v->kind == GTEXT_CSS_VAL_URL) {
    return GTEXT_CSS_IMAGE_URL;
  }
  if (v->kind != GTEXT_CSS_VAL_FUNCTION || v->math) {
    return GTEXT_CSS_IMAGE_NONE;
  }
  if (css_fn_name_is(&v->text, "url") || css_fn_name_is(&v->text, "src")) {
    return GTEXT_CSS_IMAGE_URL;
  }
  if (css_fn_name_is(&v->text, "image")) {
    return GTEXT_CSS_IMAGE_IMAGE;
  }
  if (css_fn_name_is(&v->text, "image-set") || css_fn_name_is(&v->text, "-webkit-image-set")) {
    return GTEXT_CSS_IMAGE_IMAGE_SET;
  }
  if (css_fn_name_is(&v->text, "cross-fade")) {
    return GTEXT_CSS_IMAGE_CROSS_FADE;
  }
  if (css_fn_name_is(&v->text, "element")) {
    return GTEXT_CSS_IMAGE_ELEMENT;
  }
  if (css_fn_name_is(&v->text, "paint")) {
    return GTEXT_CSS_IMAGE_PAINT;
  }
  if (gtext_css_function_family(v->text.data, v->text.length) == GTEXT_CSS_FN_FAMILY_GRADIENT) {
    return GTEXT_CSS_IMAGE_GRADIENT;
  }
  return GTEXT_CSS_IMAGE_NONE;
}

GTEXT_CSS_Status gtext_css_image_set_options(const GTEXT_CSS_Value * value, const GTEXT_Allocator * allocator,
    GTEXT_CSS_Image_Set_Option ** options, size_t * count) {
  if (!value || !options || !count || gtext_css_image_kind(value) != GTEXT_CSS_IMAGE_IMAGE_SET) {
    return GTEXT_CSS_E_INVALID;
  }
  const GTEXT_Allocator * a = allocator ? allocator : gtext_allocator_default();
  css_leafvec lv;
  if (!css_fn_leaves(a, value, &lv)) {
    return GTEXT_CSS_E_OOM;
  }
  size_t segs = 1;
  for (size_t i = 0; i < lv.n; i++) {
    if (!lv.v[i]) {
      segs++;
    }
  }
  GTEXT_CSS_Image_Set_Option * o = gtext_allocator_calloc(a, segs, sizeof *o);
  if (!o) {
    css_fn_leaves_free(a, &lv);
    return GTEXT_CSS_E_OOM;
  }
  size_t n = 0;
  bool have = false;
  for (size_t i = 0; i <= lv.n; i++) {
    if (i == lv.n || !lv.v[i]) {
      if (have) {
        n++;
        have = false;
      }
      continue;
    }
    const GTEXT_CSS_Value * v = lv.v[i];
    GTEXT_CSS_Image_Set_Option * cur = &o[n];
    if (!have) {
      cur->image = v;
      cur->resolution = 1.0;
      have = true;
      continue;
    }
    double x;
    if (v->kind == GTEXT_CSS_VAL_FUNCTION && !v->math && css_fn_name_is(&v->text, "type")) {
      cur->type = v;
    }
    else if (css_fn_leaf_number(v, &x) == 5) {
      cur->resolution = x;
      cur->resolution_written = true;
    }
  }
  css_fn_leaves_free(a, &lv);
  *options = o;
  *count = n;
  return GTEXT_CSS_OK;
}

/* ---- gradients -------------------------------------------------------------------------------- */

GTEXT_CSS_Status gtext_css_gradient_from_value(const GTEXT_CSS_Value * value, size_t max_stops, const GTEXT_Allocator * allocator,
    GTEXT_CSS_Gradient ** out) {
  if (!value || !out || gtext_css_image_kind(value) != GTEXT_CSS_IMAGE_GRADIENT) {
    return GTEXT_CSS_E_INVALID;
  }
  const GTEXT_Allocator * a = allocator ? allocator : gtext_allocator_default();
  if (max_stops == 0) {
    max_stops = 65536;
  }
  GTEXT_CSS_Gradient_Kind kind = GTEXT_CSS_GRADIENT_LINEAR;
  bool repeating = value->text.length > 9 && css_fn_name_is(&(GTEXT_CSS_String){value->text.data, 9}, "repeating");
  if (strstr(value->text.data, "radial")) {
    kind = GTEXT_CSS_GRADIENT_RADIAL;
  }
  else if (strstr(value->text.data, "conic")) {
    kind = GTEXT_CSS_GRADIENT_CONIC;
  }
  css_leafvec lv;
  if (!css_fn_leaves(a, value, &lv)) {
    return GTEXT_CSS_E_OOM;
  }
  /* segments between the commas: the first without a color is the configuration, the others without a color are hints */
  size_t segs = 1;
  for (size_t i = 0; i < lv.n; i++) {
    if (!lv.v[i]) {
      segs++;
    }
  }
  GTEXT_CSS_Gradient * g = gtext_allocator_calloc(a, 1, sizeof *g);
  GTEXT_CSS_Gradient_Stop * stops = gtext_allocator_calloc(a, segs, sizeof *stops);
  if (!g || !stops) {
    gtext_allocator_free(a, g);
    gtext_allocator_free(a, stops);
    css_fn_leaves_free(a, &lv);
    return GTEXT_CSS_E_OOM;
  }
  g->allocator_ = *a;
  g->kind = kind;
  g->repeating = repeating;
  g->value = value;
  g->stops = stops;
  size_t n = 0, seg_index = 0;
  size_t begin = 0;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  for (size_t i = 0; i <= lv.n; i++) {
    if (i < lv.n && lv.v[i]) {
      continue;
    }
    /* the segment is leaves begin..i-1 */
    const GTEXT_CSS_Value * color = NULL;
    const GTEXT_CSS_Value * pos[2] = {NULL, NULL};
    size_t np = 0;
    for (size_t k = begin; k < i; k++) {
      if (lv.v[k]->kind == GTEXT_CSS_VAL_COLOR) {
        color = lv.v[k];
      }
      else if (np < 2) {
        double x;
        if (css_fn_leaf_number(lv.v[k], &x) >= 0) {
          pos[np++] = lv.v[k];
        }
      }
    }
    const bool config = seg_index == 0 && !color;
    if (!config && i > begin) {
      if (n >= max_stops) {
        st = GTEXT_CSS_E_LIMIT_STOPS;
        break;
      }
      stops[n].color = color;
      stops[n].position = pos[0];
      stops[n].position2 = pos[1];
      n++;
    }
    seg_index++;
    begin = i + 1;
  }
  css_fn_leaves_free(a, &lv);
  if (st != GTEXT_CSS_OK) {
    gtext_css_gradient_free(g);
    return st;
  }
  g->stop_count = n;
  *out = g;
  return GTEXT_CSS_OK;
}

void gtext_css_gradient_free(GTEXT_CSS_Gradient * g) {
  if (!g) {
    return;
  }
  GTEXT_Allocator a = g->allocator_;
  gtext_allocator_free(&a, g->stops);
  gtext_allocator_free(&a, g);
}

typedef struct {
  GTEXT_Allocator alloc;
  char * data;
  size_t len, cap;
  GTEXT_CSS_Status status;
} gbuf;

static bool gput(gbuf * b, const char * s, size_t n) {
  if (b->status != GTEXT_CSS_OK) {
    return false;
  }
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->len + n + 1) {
      cap *= 2;
    }
    char * p = gtext_allocator_realloc(&b->alloc, b->data, cap);
    if (!p) {
      b->status = GTEXT_CSS_E_OOM;
      return false;
    }
    b->data = p;
    b->cap = cap;
  }
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
  return true;
}

static bool gvalue(gbuf * b, const GTEXT_CSS_Value * v) {
  GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
  so.allocator = &b->alloc;
  char * t = NULL;
  size_t n = 0;
  const GTEXT_CSS_Status st = gtext_css_serialize_value_buffer(v, &so, &t, &n);
  if (st != GTEXT_CSS_OK) {
    b->status = st;
    return false;
  }
  const bool ok = gput(b, t, n);
  gtext_css_serialize_buffer_free(&so, t);
  return ok;
}

/// A position that is the default of the first stop (0) or of the last (100%).
static bool position_is(const GTEXT_CSS_Value * p, double pct_or_zero) {
  if (!p) {
    return false;
  }
  if (p->kind == GTEXT_CSS_VAL_PERCENTAGE) {
    return p->number == pct_or_zero;
  }
  if (pct_or_zero == 0.0 && (p->kind == GTEXT_CSS_VAL_ZERO || (p->kind == GTEXT_CSS_VAL_DIMENSION && p->number == 0.0 && !p->math))) {
    return true;
  }
  return false;
}

GTEXT_CSS_Status gtext_css_gradient_serialize_buffer(const GTEXT_CSS_Gradient * g, const GTEXT_Allocator * allocator, char ** out,
    size_t * out_length) {
  if (!g || !out || !out_length) {
    return GTEXT_CSS_E_INVALID;
  }
  gbuf b;
  memset(&b, 0, sizeof b);
  b.alloc = *(allocator ? allocator : gtext_allocator_default());
  const GTEXT_Allocator * a = &b.alloc;
  css_leafvec lv;
  if (!css_fn_leaves(a, g->value, &lv)) {
    return GTEXT_CSS_E_OOM;
  }
  gput(&b, g->value->text.data, g->value->text.length);
  gput(&b, "(", 1);
  /* the configuration: everything in the first segment when it has no color */
  size_t first_end = 0;
  while (first_end < lv.n && lv.v[first_end]) {
    first_end++;
  }
  bool config = false;
  for (size_t k = 0; k < first_end; k++) {
    if (lv.v[k]->kind == GTEXT_CSS_VAL_COLOR) {
      config = false;
      first_end = 0;
      break;
    }
    config = true;
  }
  bool wrote_any = false;
  if (config) {
    const bool to_bottom = g->kind == GTEXT_CSS_GRADIENT_LINEAR && first_end == 2 && css_fn_leaf_is_keyword(lv.v[0], "to") &&
        css_fn_leaf_is_keyword(lv.v[1], "bottom");
    if (!to_bottom) {
      for (size_t k = 0; k < first_end; k++) {
        if (k) {
          gput(&b, " ", 1);
        }
        gvalue(&b, lv.v[k]);
      }
      wrote_any = true;
    }
  }
  for (size_t i = 0; i < g->stop_count; i++) {
    const GTEXT_CSS_Gradient_Stop * s = &g->stops[i];
    if (wrote_any) {
      gput(&b, ", ", 2);
    }
    wrote_any = true;
    if (s->color) {
      gvalue(&b, s->color);
      const bool drop = !s->position2 && ((i == 0 && position_is(s->position, 0.0)) ||
          (i + 1 == g->stop_count && position_is(s->position, 100.0)));
      if (s->position && !drop) {
        gput(&b, " ", 1);
        gvalue(&b, s->position);
        if (s->position2) {
          gput(&b, " ", 1);
          gvalue(&b, s->position2);
        }
      }
    }
    else if (s->position) {
      gvalue(&b, s->position);
    }
  }
  gput(&b, ")", 1);
  css_fn_leaves_free(a, &lv);
  if (b.status != GTEXT_CSS_OK) {
    gtext_allocator_free(&b.alloc, b.data);
    return b.status;
  }
  *out = b.data;
  *out_length = b.len;
  return GTEXT_CSS_OK;
}
