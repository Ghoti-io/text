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
 * Serializing the declared value of a color (CSS Color 4 section 16, CSS Color 5 section 11).
 *
 * Colors hold colors (the items of a `color-mix()`, the origin of a relative color), and the
 * nesting is only limited at the other end (by the reader), so the tree is first listed in
 * post order with an explicit stack, and then each node is written from the text of its
 * children, which is already done. Nothing recurses.
 */

#include <ghoti.io/math/basic.h>

#include <float.h>
#include <string.h>

#include "css_color_internal.h"

typedef GTEXT_CSS_Color color_t;
typedef GTEXT_CSS_Color_Component comp_t;

typedef struct {
  GTEXT_Allocator alloc;
  char * data;
  size_t len, cap, max;
  GTEXT_CSS_Status status;
} obuf;

/// Round to a double (see css_units.c): the same bits where intermediates are kept wider (x87).
static inline double rnd(double x) {
  volatile double v = x;
  return v;
}

static bool put(obuf * b, const char * s, size_t n) {
  if (b->status != GTEXT_CSS_OK) {
    return false;
  }
  if (n > b->max - b->len) {
    b->status = GTEXT_CSS_E_LIMIT_OUTPUT;
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

/// The sink the identifier serializer writes through: into the buffer.
static GTEXT_Markup_Status obuf_sink_write(void * user, const char * bytes, size_t len) {
  return put((obuf *)user, bytes, len) ? GTEXT_MARKUP_OK : GTEXT_MARKUP_E_WRITE;
}

/// Write a name as CSSOM "serialize an identifier" does (a custom color space `--a\ b`, a channel keyword): written raw, an escaped character
/// is lost and the text no longer reads back as the same name.
static bool put_ident(obuf * b, const char * s, size_t n) {
  if (b->status != GTEXT_CSS_OK) {
    return false;
  }
  if (n == 0) {
    return true;
  }
  GTEXT_Markup_Sink sink;
  sink.write = obuf_sink_write;
  sink.user = b;
  const GTEXT_CSS_Status st = gtext_css_serialize_identifier(s, n, &sink);
  if (st != GTEXT_CSS_OK && b->status == GTEXT_CSS_OK) {
    b->status = st;
  }
  return st == GTEXT_CSS_OK;
}

static bool puts_(obuf * b, const char * s) {
  return put(b, s, strlen(s));
}

static bool put_num(obuf * b, double v) {
  if (v == 0.0) {
    v = 0.0;
  }
  char t[340];
  return put(b, t, css_cssom_number(v, false, t));
}

static double clamp01(double v) {
  return v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v;
}

/* ---- components ----------------------------------------------------------------------- */

static bool put_math(obuf * b, const comp_t * c) {
  char * s = NULL;
  size_t n = 0;
  const GTEXT_CSS_Status st = css_math_serialize_to(c->math, &b->alloc, b->max, &s, &n);
  if (st != GTEXT_CSS_OK) {
    b->status = st;
    return false;
  }
  const bool ok = put(b, s, n);
  gtext_allocator_free(&b->alloc, s);
  return ok;
}

/// A channel as a number: a percentage by its reference range, a math function as written, a keyword as written.
static bool put_channel(obuf * b, const comp_t * c, double ref) {
  switch (c->kind) {
  case GTEXT_CSS_COMP_NONE:
    return puts_(b, "none");
  case GTEXT_CSS_COMP_MATH:
    return put_math(b, c);
  case GTEXT_CSS_COMP_SYMBOL:
    return put_ident(b, c->text.data, c->text.length);
  case GTEXT_CSS_COMP_PERCENT:
    return put_num(b, c->value * ref / 100.0);
  default:
    return put_num(b, c->value);
  }
}

/// An alpha: nothing when it is 1; ` / a` otherwise (`none`, a math function and a keyword always).
static bool put_alpha(obuf * b, const color_t * c, bool percent_as_written) {
  const comp_t * a = &c->alpha;
  if (!c->alpha_written) {
    return true;
  }
  switch (a->kind) {
  case GTEXT_CSS_COMP_NONE: return puts_(b, " / none");
  case GTEXT_CSS_COMP_MATH:
    /* a calculation is kept as it simplified, a percentage included (`calc(-50% * 3)` is `calc(-150%)`) */
    return puts_(b, " / ") && put_math(b, a);
  case GTEXT_CSS_COMP_SYMBOL: return puts_(b, " / ") && put_ident(b, a->text.data, a->text.length);
  case GTEXT_CSS_COMP_PERCENT: {
    const double v = a->value < 0.0 ? 0.0 : a->value > 100.0 ? 100.0 : a->value;
    if (v == 100.0) {
      return true;
    }
    if (percent_as_written) {
      return puts_(b, " / ") && put_num(b, v) && puts_(b, "%");
    }
    return puts_(b, " / ") && put_num(b, v / 100.0);
  }
  default: {
    const double v = clamp01(a->value);
    return v == 1.0 || (puts_(b, " / ") && put_num(b, v));
  }
  }
}

/// The alpha of a color whose alpha is a plain number: 0 to 1.
static double alpha_value(const color_t * c) {
  const comp_t * a = &c->alpha;
  if (a->kind == GTEXT_CSS_COMP_PERCENT) {
    return clamp01(a->value / 100.0);
  }
  if (a->kind == GTEXT_CSS_COMP_NUMBER) {
    return clamp01(a->value);
  }
  return 1.0;
}

/* ---- the sRGB family ---------------------------------------------------------------------- */

static double round8(double v) {
  if (v < 0.0) {
    return 0.0;
  }
  if (v > 255.0) {
    return 255.0;
  }
  return gmath_floor(v + 0.5 + 1e-9);
}

/// The 8 bit value of channel `i` of an sRGB-family color, or false when it is not a number.
static bool srgb_values(const color_t * c, double rgb[3]) {
  if (c->has_missing || c->has_unresolved || c->origin) {
    return false;
  }
  if (c->kind == GTEXT_CSS_COLOR_HEX || c->kind == GTEXT_CSS_COLOR_KEYWORD) {
    if (c->kind == GTEXT_CSS_COLOR_KEYWORD && c->keyword != GTEXT_CSS_COLOR_KW_NAMED && c->keyword != GTEXT_CSS_COLOR_KW_TRANSPARENT) {
      return false;
    }
    for (int i = 0; i < 3; i++) {
      rgb[i] = c->components[i].value;
    }
    return true;
  }
  if (c->kind != GTEXT_CSS_COLOR_FUNCTION || c->component_count != 3) {
    return false;
  }
  double v[3];
  for (int i = 0; i < 3; i++) {
    const comp_t * k = &c->components[i];
    if (k->kind == GTEXT_CSS_COMP_NUMBER || k->kind == GTEXT_CSS_COMP_ANGLE) {
      v[i] = k->value;
    }
    else if (k->kind == GTEXT_CSS_COMP_PERCENT) {
      v[i] = c->function == GTEXT_CSS_COLOR_FN_RGB ? rnd(k->value * 255.0) / 100.0 : k->value;
    }
    else {
      return false;
    }
  }
  switch (c->function) {
  case GTEXT_CSS_COLOR_FN_RGB:
    memcpy(rgb, v, sizeof v);
    return true;
  case GTEXT_CSS_COLOR_FN_HSL:
    css_color_hsl_to_srgb(v[0], v[1], v[2], rgb);
    break;
  case GTEXT_CSS_COLOR_FN_HWB:
    css_color_hwb_to_srgb(v[0], v[1], v[2], rgb);
    break;
  default:
    return false;
  }
  for (int i = 0; i < 3; i++) {
    rgb[i] = round8(rgb[i]);
  }
  return true;
}

bool gtext_css_color_srgb(const GTEXT_CSS_Color * color, double rgba[4]) {
  double rgb[3];
  if (!color || !rgba || !srgb_values(color, rgb)) {
    return false;
  }
  if (color->alpha.kind != GTEXT_CSS_COMP_NUMBER && color->alpha.kind != GTEXT_CSS_COMP_PERCENT) {
    return false;
  }
  rgba[0] = rgb[0];
  rgba[1] = rgb[1];
  rgba[2] = rgb[2];
  rgba[3] = alpha_value(color);
  return true;
}

/// The alpha of a legacy sRGB color: an 8 bit value is written by the draft's algorithm (16.1.1), any other as a number.
static bool put_legacy_alpha(obuf * b, double a) {
  volatile double scaled = a * 255.0;
  const double x = scaled;
  const double ai = gmath_floor(x + 0.5);
  if (gmath_fabs(x - ai) < 1e-7) {
    /* "If there exists an integer between 0 and 100 inclusive that, when multiplied with 2.55 and rounded to the closest integer (rounding
       up if two values are equally close), equals alpha, let rounded be that integer divided by 100." */
    for (int p = 0; p <= 100; p++) {
      volatile double product = p * 2.55;
      if (gmath_floor(product + 0.5 + 1e-9) == ai) {
        return put_num(b, p / 100.0);
      }
    }
    /* "Otherwise, let rounded be alpha divided by 0.255 and rounded to the closest integer (rounding up if two values are equally close),
       divided by 1000." (The draft's example for 236 says "0.92549"; its algorithm, which Chromium follows, gives 0.925, and an algorithm
       is what an implementation can be tested against.) */
    volatile double quotient = ai / 0.255;
    return put_num(b, gmath_floor(quotient + 0.5) / 1000.0);
  }
  return put_num(b, a);
}

static const char * fn_name(GTEXT_CSS_Color_Function f) {
  switch (f) {
  case GTEXT_CSS_COLOR_FN_RGB: return "rgb";
  case GTEXT_CSS_COLOR_FN_HSL: return "hsl";
  case GTEXT_CSS_COLOR_FN_HWB: return "hwb";
  case GTEXT_CSS_COLOR_FN_LAB: return "lab";
  case GTEXT_CSS_COLOR_FN_LCH: return "lch";
  case GTEXT_CSS_COLOR_FN_OKLAB: return "oklab";
  case GTEXT_CSS_COLOR_FN_OKLCH: return "oklch";
  case GTEXT_CSS_COLOR_FN_COLOR: return "color";
  default: return "";
  }
}

/// The reference range (what 100% is) of channel `i` of a function.
static double ref_of(GTEXT_CSS_Color_Function f, size_t i) {
  switch (f) {
  case GTEXT_CSS_COLOR_FN_RGB: return 255.0;
  case GTEXT_CSS_COLOR_FN_HSL: case GTEXT_CSS_COLOR_FN_HWB: return i == 0 ? 360.0 : 100.0;
  case GTEXT_CSS_COLOR_FN_LAB: return i == 0 ? 100.0 : 125.0;
  case GTEXT_CSS_COLOR_FN_LCH: return i == 0 ? 100.0 : i == 1 ? 150.0 : 360.0;
  case GTEXT_CSS_COLOR_FN_OKLAB: return i == 0 ? 1.0 : 0.4;
  case GTEXT_CSS_COLOR_FN_OKLCH: return i == 0 ? 1.0 : i == 1 ? 0.4 : 360.0;
  default: return 1.0;
  }
}

/// The alpha of a relative color: as written (a number or a percentage, unclamped; the pages of the pinned tests keep `/ 1` and `/ 500`).
static bool put_relative_alpha(obuf * b, const color_t * c) {
  if (!c->alpha_written) {
    return true;
  }
  const comp_t * a = &c->alpha;
  if (!puts_(b, " / ")) {
    return false;
  }
  if (a->kind == GTEXT_CSS_COMP_PERCENT) {
    return put_num(b, a->value) && puts_(b, "%");
  }
  return put_channel(b, a, 1.0);
}

static bool ser_relative(obuf * b, const color_t * c, const char * origin_text) {
  if (c->kind == GTEXT_CSS_COLOR_ALPHA) {
    return puts_(b, "alpha(from ") && puts_(b, origin_text) && put_relative_alpha(b, c) && puts_(b, ")");
  }
  if (c->function == GTEXT_CSS_COLOR_FN_COLOR) {
    if (!puts_(b, "color(from ") || !puts_(b, origin_text) || !puts_(b, " ") || !puts_(b, gtext_css_color_space_name(c->space))) {
      return false;
    }
  }
  else if (!puts_(b, fn_name(c->function)) || !puts_(b, "(from ") || !puts_(b, origin_text)) {
    return false;
  }
  for (size_t i = 0; i < c->component_count; i++) {
    const comp_t * k = &c->components[i];
    if (!puts_(b, " ")) {
      return false;
    }
    if (k->kind == GTEXT_CSS_COMP_PERCENT) {
      if (!put_num(b, k->value) || !puts_(b, "%")) {
        return false;
      }
    }
    else if (k->kind == GTEXT_CSS_COMP_ANGLE) {
      if (!put_num(b, k->value) || !puts_(b, "deg")) {
        return false;
      }
    }
    else if (!put_channel(b, k, 1.0)) {
      return false;
    }
  }
  return put_relative_alpha(b, c) && puts_(b, ")");
}

static bool ser_srgb(obuf * b, const color_t * c) {
  const bool hex_or_name = c->kind == GTEXT_CSS_COLOR_HEX || c->kind == GTEXT_CSS_COLOR_KEYWORD;
  const GTEXT_CSS_Color_Function f = hex_or_name ? GTEXT_CSS_COLOR_FN_RGB : c->function;
  if (c->has_unresolved) {
    /* a channel that needs the environment: the function as specified, the channels as numbers */
    if (!puts_(b, fn_name(f)) || !puts_(b, "(")) {
      return false;
    }
    for (size_t i = 0; i < 3; i++) {
      const comp_t * k = &c->components[i];
      if (i && !puts_(b, " ")) {
        return false;
      }
      if (f == GTEXT_CSS_COLOR_FN_RGB) {
        if (!put_channel(b, k, 255.0)) {
          return false;
        }
      }
      else if (!put_channel(b, k, i == 0 ? 360.0 : 100.0)) {
        return false;
      }
      /* a percentage of hsl() and hwb() is written as its number of percent points */
      if (f != GTEXT_CSS_COLOR_FN_RGB && k->kind == GTEXT_CSS_COMP_PERCENT) {
        /* (put_channel scaled it by ref/100, which for 100 is the identity; the hue is never a percentage) */
      }
    }
    return put_alpha(b, c, false) && puts_(b, ")");
  }
  if (c->has_missing) {
    if (f == GTEXT_CSS_COLOR_FN_RGB) {
      if (!puts_(b, "color(srgb")) {
        return false;
      }
      for (size_t i = 0; i < 3; i++) {
        const comp_t * k = &c->components[i];
        if (!puts_(b, " ")) {
          return false;
        }
        if (k->kind == GTEXT_CSS_COMP_NONE) {
          if (!puts_(b, "none")) {
            return false;
          }
        }
        else if (!put_num(b, k->kind == GTEXT_CSS_COMP_PERCENT ? k->value / 100.0 : k->value / 255.0)) {
          return false;
        }
      }
      return put_alpha(b, c, false) && puts_(b, ")");
    }
    if (!puts_(b, fn_name(f)) || !puts_(b, "(")) {
      return false;
    }
    for (size_t i = 0; i < 3; i++) {
      const comp_t * k = &c->components[i];
      if (i && !puts_(b, " ")) {
        return false;
      }
      if (k->kind == GTEXT_CSS_COMP_NONE) {
        if (!puts_(b, "none")) {
          return false;
        }
      }
      else if (!put_num(b, k->value) || (i > 0 && !puts_(b, "%"))) {
        return false;
      }
    }
    return put_alpha(b, c, false) && puts_(b, ")");
  }
  double rgb[3];
  if (!srgb_values(c, rgb)) {
    return false;
  }
  const double a = alpha_value(c);
  if (!puts_(b, a == 1.0 ? "rgb(" : "rgba(") || !put_num(b, rgb[0]) || !puts_(b, ", ") || !put_num(b, rgb[1]) || !puts_(b, ", ") ||
      !put_num(b, rgb[2])) {
    return false;
  }
  if (a != 1.0 && (!puts_(b, ", ") || !put_legacy_alpha(b, a))) {
    return false;
  }
  return puts_(b, ")");
}

/* ---- the other functions ------------------------------------------------------------------- */

static bool ser_modern(obuf * b, const color_t * c) {
  if (c->function == GTEXT_CSS_COLOR_FN_COLOR) {
    if (!puts_(b, "color(") || !(c->space == GTEXT_CSS_SPACE_CUSTOM ? put_ident(b, c->name.data, c->name.length) : puts_(b, gtext_css_color_space_name(c->space)))) {
      return false;
    }
  }
  else if (!puts_(b, fn_name(c->function)) || !puts_(b, "(")) {
    return false;
  }
  for (size_t i = 0; i < c->component_count; i++) {
    if ((i || c->function == GTEXT_CSS_COLOR_FN_COLOR) && !puts_(b, " ")) {
      return false;
    }
    if (!put_channel(b, &c->components[i], ref_of(c->function, i))) {
      return false;
    }
  }
  return put_alpha(b, c, false) && puts_(b, ")");
}

static bool ser_cmyk(obuf * b, const color_t * c) {
  if (!puts_(b, "device-cmyk(")) {
    return false;
  }
  for (size_t i = 0; i < c->component_count; i++) {
    if (i && !puts_(b, " ")) {
      return false;
    }
    if (!put_channel(b, &c->components[i], 1.0)) {
      return false;
    }
  }
  return put_alpha(b, c, false) && puts_(b, ")");
}

static const char * hue_text(GTEXT_CSS_Hue_Method m) {
  switch (m) {
  case GTEXT_CSS_HUE_LONGER: return " longer hue";
  case GTEXT_CSS_HUE_INCREASING: return " increasing hue";
  case GTEXT_CSS_HUE_DECREASING: return " decreasing hue";
  default: return "";
  }
}

static bool ser_mix(obuf * b, const color_t * c, const char * const * kid_text) {
  if (!puts_(b, "color-mix(")) {
    return false;
  }
  if (c->mix_space != GTEXT_CSS_SPACE_OKLAB) {
    if (!puts_(b, "in ") || !(c->mix_space == GTEXT_CSS_SPACE_CUSTOM ? put_ident(b, c->mix_custom.data, c->mix_custom.length)
                                                                      : puts_(b, gtext_css_color_space_name(c->mix_space))) ||
        !puts_(b, hue_text(c->hue_method)) || !puts_(b, ", ")) {
      return false;
    }
  }
  /* the percentages that are written: those that are not all the equal shares */
  const size_t n = c->item_count;
  bool known = true, any_calc = false;
  double specified_sum = 0.0;
  size_t omitted = 0;
  for (size_t i = 0; i < n; i++) {
    const comp_t * p = &c->percents[i];
    if (p->kind == GTEXT_CSS_COMP_PERCENT) {
      specified_sum += p->value;
    }
    else if (p->kind == GTEXT_CSS_COMP_NONE) {
      omitted++;
    }
    else {
      any_calc = true;
    }
  }
  if (any_calc) {
    known = false;
  }
  bool all_equal_share = known;
  if (known) {
    const double share = 100.0 / (double)n;
    for (size_t i = 0; i < n; i++) {
      const comp_t * p = &c->percents[i];
      const double eff = p->kind == GTEXT_CSS_COMP_PERCENT ? p->value : (100.0 - specified_sum) / (double)omitted;
      if (gmath_fabs(eff - share) > 1e-9) {
        all_equal_share = false;
      }
    }
  }
  for (size_t i = 0; i < n; i++) {
    if (i && !puts_(b, ", ")) {
      return false;
    }
    if (!puts_(b, kid_text[i])) {
      return false;
    }
    const comp_t * p = &c->percents[i];
    if (all_equal_share) {
      continue;
    }
    if (p->kind == GTEXT_CSS_COMP_PERCENT) {
      if (!puts_(b, " ") || !put_num(b, p->value) || !puts_(b, "%")) {
        return false;
      }
    }
    else if (p->kind == GTEXT_CSS_COMP_MATH) {
      if (!puts_(b, " ") || !put_math(b, p)) {
        return false;
      }
    }
    else if (known && omitted) {
      if (!puts_(b, " ") || !put_num(b, (100.0 - specified_sum) / (double)omitted) || !puts_(b, "%")) {
        return false;
      }
    }
  }
  return puts_(b, ")");
}

/// Write one node, given the text of the colors it holds.
static bool ser_one(obuf * b, const color_t * c, const char * const * kid_text) {
  switch (c->kind) {
  case GTEXT_CSS_COLOR_KEYWORD:
    if (c->keyword == GTEXT_CSS_COLOR_KW_NAMED || c->keyword == GTEXT_CSS_COLOR_KW_TRANSPARENT || c->keyword == GTEXT_CSS_COLOR_KW_CURRENTCOLOR ||
        c->keyword == GTEXT_CSS_COLOR_KW_SYSTEM || c->keyword == GTEXT_CSS_COLOR_KW_DEPRECATED) {
      return put_ident(b, c->name.data, c->name.length);
    }
    return false;
  case GTEXT_CSS_COLOR_HEX:
    return ser_srgb(b, c);
  case GTEXT_CSS_COLOR_FUNCTION:
    if (c->origin) {
      return ser_relative(b, c, kid_text[0]);
    }
    if (c->function == GTEXT_CSS_COLOR_FN_RGB || c->function == GTEXT_CSS_COLOR_FN_HSL || c->function == GTEXT_CSS_COLOR_FN_HWB) {
      return ser_srgb(b, c);
    }
    return ser_modern(b, c);
  case GTEXT_CSS_COLOR_ALPHA:
    return ser_relative(b, c, kid_text[0]);
  case GTEXT_CSS_COLOR_MIX:
    return ser_mix(b, c, kid_text);
  case GTEXT_CSS_COLOR_LIGHT_DARK:
    return puts_(b, "light-dark(") && puts_(b, kid_text[0]) && puts_(b, ", ") && puts_(b, kid_text[1]) && puts_(b, ")");
  case GTEXT_CSS_COLOR_CONTRAST:
    return puts_(b, "contrast-color(") && puts_(b, kid_text[0]) && puts_(b, ")");
  case GTEXT_CSS_COLOR_DEVICE_CMYK:
    return ser_cmyk(b, c);
  default:
    return false;
  }
}

/* ---- the walk ------------------------------------------------------------------------------- */

typedef struct {
  const color_t * c;
  size_t kids;       ///< Index into `kidx` of the first of this node's children (post order numbers).
  char * text;
} snode;

typedef struct {
  const color_t * c;
  size_t i;
  size_t kids;
  size_t index;      ///< Reserved for the post-order number of this node's own result (unused until it finishes).
} sframe;

static size_t kid_count(const color_t * c) {
  return (c->origin ? 1u : 0u) + c->item_count;
}

static const color_t * kid_at(const color_t * c, size_t i) {
  if (c->origin) {
    return c->origin;
  }
  return c->items[i];
}

GTEXT_CSS_Status css_color_serialize_to(const GTEXT_CSS_Color * color, const GTEXT_Allocator * alloc, size_t max_bytes, char ** out,
    size_t * out_len) {
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  snode * nodes = NULL;
  size_t nn = 0, capn = 0;
  size_t * kidx = NULL;
  size_t nk = 0, capk = 0;
  sframe * fs = NULL;
  size_t nf = 0, capf = 0;
  GTEXT_Allocator a = *alloc;
  /* post order */
#define GROW(arr, n_, cap_, T, need) \
  do { \
    if ((need) > (cap_)) { \
      size_t c_ = (cap_) ? (cap_) * 2 : 16; \
      while (c_ < (need)) { \
        c_ *= 2; \
      } \
      T * p_ = gtext_allocator_realloc(&a, (arr), c_ * sizeof *p_); \
      if (!p_) { \
        st = GTEXT_CSS_E_OOM; \
        goto done; \
      } \
      (arr) = p_; \
      (cap_) = c_; \
    } \
  } while (0)
  GROW(fs, nf, capf, sframe, nf + 1);
  fs[nf].c = color;
  fs[nf].i = 0;
  fs[nf].kids = nk;
  GROW(kidx, nk, capk, size_t, nk + kid_count(color));
  nk += kid_count(color);
  nf++;
  while (nf > 0) {
    sframe * F = &fs[nf - 1];
    if (F->i < kid_count(F->c)) {
      const color_t * child = kid_at(F->c, F->i);
      F->i++;
      GROW(fs, nf, capf, sframe, nf + 1);
      F = &fs[nf - 1];
      fs[nf].c = child;
      fs[nf].i = 0;
      fs[nf].kids = nk;
      GROW(kidx, nk, capk, size_t, nk + kid_count(child));
      nk += kid_count(child);
      nf++;
      continue;
    }
    GROW(nodes, nn, capn, snode, nn + 1);
    nodes[nn].c = F->c;
    nodes[nn].kids = F->kids;
    nodes[nn].text = NULL;
    const size_t my = nn++;
    nf--;
    if (nf > 0) {
      sframe * P = &fs[nf - 1];
      kidx[P->kids + (P->i - 1)] = my;
    }
  }
  /* write each node from its children */
  for (size_t i = 0; i < nn; i++) {
    const color_t * c = nodes[i].c;
    const size_t nkids = kid_count(c);
    const char ** kt = NULL;
    if (nkids) {
      kt = gtext_allocator_malloc(&a, nkids * sizeof *kt);
      if (!kt) {
        st = GTEXT_CSS_E_OOM;
        goto done;
      }
      for (size_t k = 0; k < nkids; k++) {
        kt[k] = nodes[kidx[nodes[i].kids + k]].text;
      }
    }
    obuf b;
    memset(&b, 0, sizeof b);
    b.alloc = a;
    b.max = max_bytes;
    const bool ok = ser_one(&b, c, kt);
    gtext_allocator_free(&a, kt);
    if (!ok) {
      st = b.status != GTEXT_CSS_OK ? b.status : GTEXT_CSS_E_INTERNAL;
      gtext_allocator_free(&a, b.data);
      goto done;
    }
    if (!b.data) {
      b.data = gtext_allocator_malloc(&a, 1);
      if (!b.data) {
        st = GTEXT_CSS_E_OOM;
        goto done;
      }
      b.data[0] = '\0';
    }
    nodes[i].text = b.data;
    /* the children's texts have been copied into this node's: each color has exactly one parent, so they are not needed again (without
       this a chain of colors nested to depth d holds d texts of up to d pieces each) */
    for (size_t k = 0; k < nkids; k++) {
      snode * kid = &nodes[kidx[nodes[i].kids + k]];
      gtext_allocator_free(&a, kid->text);
      kid->text = NULL;
    }
    if (i + 1 == nn) {
      *out_len = b.len;
    }
  }
  /* the root is last: hand its text over */
  *out = nodes[nn - 1].text;
  nodes[nn - 1].text = NULL;
done:
  for (size_t i = 0; i < nn; i++) {
    gtext_allocator_free(&a, nodes[i].text);
  }
  gtext_allocator_free(&a, nodes);
  gtext_allocator_free(&a, kidx);
  gtext_allocator_free(&a, fs);
#undef GROW
  return st;
}

GTEXT_CSS_Status gtext_css_color_serialize_buffer(const GTEXT_CSS_Color * color, const GTEXT_CSS_Serialize_Options * options,
    char ** out, size_t * out_length) {
  if (!color || !out || !out_length) {
    return GTEXT_CSS_E_INVALID;
  }
  const GTEXT_Allocator * a = options && options->allocator ? options->allocator : gtext_allocator_default();
  const size_t max = options && options->max_output_bytes ? options->max_output_bytes : (size_t)256 << 20;
  return css_color_serialize_to(color, a, max, out, out_length);
}

GTEXT_CSS_Status gtext_css_color_serialize(const GTEXT_CSS_Color * color, const GTEXT_CSS_Serialize_Options * options,
    const GTEXT_Markup_Sink * sink) {
  if (!sink || !sink->write) {
    return GTEXT_CSS_E_INVALID;
  }
  char * buf = NULL;
  size_t len = 0;
  const GTEXT_CSS_Status st = gtext_css_color_serialize_buffer(color, options, &buf, &len);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  const bool wrote = sink->write(sink->user, buf, len) == GTEXT_MARKUP_OK;
  const GTEXT_Allocator * a = options && options->allocator ? options->allocator : gtext_allocator_default();
  gtext_allocator_free(a, buf);
  return wrote ? GTEXT_CSS_OK : GTEXT_CSS_E_WRITE;
}
