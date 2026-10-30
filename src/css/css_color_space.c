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
 * Conversions between the color spaces of CSS Color 4 (css_color_space.h). The formulas are the draft's (Color 4, "Color conversion code", section 17 and the
 * sections it cites): the transfer functions of each RGB space (extended: the sign of a channel is kept), the matrices (src/css/css_color_tables.c,
 * derived from the chromaticities), CIE Lab and LCH against D50, Oklab and Oklch against D65, HSL and HWB through sRGB.
 */

#include <string.h>

#include <ghoti.io/math/math.h>

#include "css_color_space.h"

/* ---- small helpers -------------------------------------------------------------------------------------------- */

static double sgn(double v) {
  return v < 0.0 ? -1.0 : 1.0;
}

static double absd(double v) {
  return v < 0.0 ? -v : v;
}

static void mulv(const double m[3][3], const double v[3], double out[3]) {
  double r[3];
  for (int i = 0; i < 3; i++) {
    r[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
  }
  out[0] = r[0];
  out[1] = r[1];
  out[2] = r[2];
}

static double norm_hue(double h) {
  /* degrees to [0, 360) */
  if (h != h) {
    return 0.0;
  }
  h = gmath_fmod(h, 360.0);
  if (h < 0.0) {
    h += 360.0;
  }
  return h == 0.0 ? 0.0 : h;
}

#define DEG_TO_RAD (3.14159265358979323846 / 180.0)
#define RAD_TO_DEG (180.0 / 3.14159265358979323846)

/* ---- transfer functions (Color 4, 10.2 to 10.6) --------------------------------------------------------------- */

static double srgb_decode(double v) {
  const double a = absd(v);
  return a <= 0.04045 ? v / 12.92 : sgn(v) * gmath_pow((a + 0.055) / 1.055, 2.4);
}

static double srgb_encode(double v) {
  const double a = absd(v);
  return a > 0.0031308 ? sgn(v) * (1.055 * gmath_pow(a, 1.0 / 2.4) - 0.055) : 12.92 * v;
}

static double a98_decode(double v) {
  return sgn(v) * gmath_pow(absd(v), 563.0 / 256.0);
}

static double a98_encode(double v) {
  return sgn(v) * gmath_pow(absd(v), 256.0 / 563.0);
}

static double prophoto_decode(double v) {
  const double a = absd(v);
  return a <= 16.0 / 512.0 ? v / 16.0 : sgn(v) * gmath_pow(a, 1.8);
}

static double prophoto_encode(double v) {
  const double a = absd(v);
  return a >= 1.0 / 512.0 ? sgn(v) * gmath_pow(a, 1.0 / 1.8) : 16.0 * v;
}

/* Color 4, 10.8 (rec2020): "Reference electro-optical transfer function from Rec. ITU-R BT.1886 Annex 1 with b (black lift) = 0 and a (user gain) = 1, defined over
   the extended range, not clamped": a pure power of 2.4 (the sample code's `lin_2020` and `gam_2020`), not the piecewise curve of BT.2020. */
static double rec2020_decode(double v) {
  return sgn(v) * gmath_pow(absd(v), 2.4);
}

static double rec2020_encode(double v) {
  return sgn(v) * gmath_pow(absd(v), 1.0 / 2.4);
}

/* ---- the space table ---------------------------------------------------------------------------------------- */

int css_cs_hue_index(GTEXT_CSS_Color_Space s) {
  switch (s) {
  case GTEXT_CSS_SPACE_LCH: case GTEXT_CSS_SPACE_OKLCH: return 2;
  case GTEXT_CSS_SPACE_HSL: case GTEXT_CSS_SPACE_HWB: return 0;
  default: return -1;
  }
}

bool css_cs_is_polar(GTEXT_CSS_Color_Space s) {
  return css_cs_hue_index(s) >= 0;
}

bool css_cs_is_legacy(GTEXT_CSS_Color_Space s) {
  return s == GTEXT_CSS_SPACE_SRGB || s == GTEXT_CSS_SPACE_HSL || s == GTEXT_CSS_SPACE_HWB;
}

/* ---- Lab (D50) and Oklab (D65) ------------------------------------------------------------------------------- */

/* Color 4, "CIE Lab": the white is D50, kappa = 24389/27 and epsilon = 216/24389. */
static const double D50_WHITE[3] = {0.3457 / 0.3585, 1.0, (1.0 - 0.3457 - 0.3585) / 0.3585};
#define LAB_EPS (216.0 / 24389.0)
#define LAB_KAPPA (24389.0 / 27.0)

static void xyz50_to_lab(const double in[3], double out[3]) {
  double f[3];
  for (int i = 0; i < 3; i++) {
    const double x = in[i] / D50_WHITE[i];
    f[i] = x > LAB_EPS ? gmath_cbrt(x) : (LAB_KAPPA * x + 16.0) / 116.0;
  }
  out[0] = 116.0 * f[1] - 16.0;
  out[1] = 500.0 * (f[0] - f[1]);
  out[2] = 200.0 * (f[1] - f[2]);
}

static void lab_to_xyz50(const double in[3], double out[3]) {
  const double fy = (in[0] + 16.0) / 116.0;
  const double fx = in[1] / 500.0 + fy;
  const double fz = fy - in[2] / 200.0;
  const double x = fx * fx * fx > LAB_EPS ? fx * fx * fx : (116.0 * fx - 16.0) / LAB_KAPPA;
  const double y = in[0] > LAB_KAPPA * LAB_EPS ? fy * fy * fy : in[0] / LAB_KAPPA;
  const double z = fz * fz * fz > LAB_EPS ? fz * fz * fz : (116.0 * fz - 16.0) / LAB_KAPPA;
  out[0] = x * D50_WHITE[0];
  out[1] = y * D50_WHITE[1];
  out[2] = z * D50_WHITE[2];
}

static void xyz65_to_oklab(const double in[3], double out[3]) {
  double lms[3];
  mulv(css_cs_xyz_to_lms, in, lms);
  for (int i = 0; i < 3; i++) {
    lms[i] = gmath_cbrt(lms[i]);
  }
  mulv(css_cs_lmsp_to_oklab, lms, out);
}

static void oklab_to_xyz65(const double in[3], double out[3]) {
  double lms[3];
  mulv(css_cs_oklab_to_lmsp, in, lms);
  for (int i = 0; i < 3; i++) {
    lms[i] = lms[i] * lms[i] * lms[i];
  }
  mulv(css_cs_lms_to_xyz, lms, out);
}

/* rectangular to polar: chroma and hue (degrees in [0, 360)); a hue that is powerless is signalled by the caller (powerless_chroma). */
static void to_polar(const double in[3], double out[3]) {
  out[0] = in[0];
  out[1] = gmath_hypot(in[1], in[2]);
  out[2] = norm_hue(gmath_atan2(in[2], in[1]) * RAD_TO_DEG);
}

static void from_polar(const double in[3], double out[3]) {
  const double h = in[2] * DEG_TO_RAD;
  const double l = in[0], c = in[1];
  out[0] = l;
  out[1] = c * gmath_cos(h);
  out[2] = c * gmath_sin(h);
}

/* ---- HSL and HWB (through sRGB) ------------------------------------------------------------------------------ */

/* Color 4, "HSL to sRGB": hue in degrees, saturation and lightness as numbers 0 to 100 here (the channel's reference range), converted to fractions. */
static void hsl_to_srgb(const double in[3], double out[3]) {
  const double h = norm_hue(in[0]);
  const double s = in[1] / 100.0, l = in[2] / 100.0;
  const double a = s * (l < 1.0 - l ? l : 1.0 - l);
  for (int k = 0; k < 3; k++) {
    const double n = k == 0 ? 0.0 : (k == 1 ? 8.0 : 4.0);
    double kk = gmath_fmod(n + h / 30.0, 12.0);
    const double m1 = kk - 3.0, m2 = 9.0 - kk;
    double t = m1 < m2 ? m1 : m2;
    t = t < 1.0 ? t : 1.0;
    t = t > -1.0 ? t : -1.0;
    out[k] = l - a * t;
  }
}

/* Color 4, 7.2 "Converting sRGB Colors to HSL" (the sample code's rgbToHsl), epsilon 1/100000 in fractions. */
static void srgb_to_hsl(const double in[3], double out[3]) {
  const double r = in[0], g = in[1], b = in[2];
  double mx = r > g ? r : g;
  mx = mx > b ? mx : b;
  double mn = r < g ? r : g;
  mn = mn < b ? mn : b;
  double hue = 0.0, sat = 0.0;
  const double light = (mn + mx) / 2.0;
  const double d = mx - mn;
  const double epsilon = 1.0 / 100000.0;
  if (d != 0.0) {
    sat = (absd(light) <= epsilon || absd(1.0 - light) < epsilon) ? 0.0 : (mx - light) / (light < 1.0 - light ? light : 1.0 - light);
    if (mx == r) {
      hue = (g - b) / d + (g < b ? 6.0 : 0.0);
    }
    else if (mx == g) {
      hue = (b - r) / d + 2.0;
    }
    else {
      hue = (r - g) / d + 4.0;
    }
    hue *= 60.0;
  }
  /* "Very out of gamut colors can produce negative saturation. If so, just rotate the hue by 180 and use a positive saturation" */
  if (sat < 0.0) {
    hue += 180.0;
    sat = absd(sat);
  }
  out[0] = norm_hue(hue);
  out[1] = sat * 100.0;
  out[2] = light * 100.0;
}

static void hwb_to_srgb(const double in[3], double out[3]) {
  double w = in[1] / 100.0, b = in[2] / 100.0;
  if (w + b >= 1.0) {
    const double gray = w / (w + b);
    out[0] = out[1] = out[2] = gray;
    return;
  }
  const double hsl[3] = {in[0], 100.0, 50.0};
  hsl_to_srgb(hsl, out);
  for (int i = 0; i < 3; i++) {
    out[i] = out[i] * (1.0 - w - b) + w;
  }
}

/* Color 4, 8.2 (rgbToHue and rgbToHwb): the hue is that of rgbToHsl without the turn for a negative saturation. */
static void srgb_to_hwb(const double in[3], double out[3]) {
  const double r = in[0], g = in[1], b = in[2];
  double mx = r > g ? r : g;
  mx = mx > b ? mx : b;
  double mn = r < g ? r : g;
  mn = mn < b ? mn : b;
  double hue = 0.0;
  const double d = mx - mn;
  if (d != 0.0) {
    if (mx == r) {
      hue = (g - b) / d + (g < b ? 6.0 : 0.0);
    }
    else if (mx == g) {
      hue = (b - r) / d + 2.0;
    }
    else {
      hue = (r - g) / d + 4.0;
    }
    hue *= 60.0;
  }
  out[0] = norm_hue(hue);
  out[1] = mn * 100.0;
  out[2] = (1.0 - mx) * 100.0;
}

/* ---- to and from XYZ-D65 ------------------------------------------------------------------------------------- */

static void rgb_decode_all(GTEXT_CSS_Color_Space s, const double in[3], double out[3]) {
  double (*f)(double) = srgb_decode;
  switch (s) {
  case GTEXT_CSS_SPACE_A98_RGB: f = a98_decode; break;
  case GTEXT_CSS_SPACE_PROPHOTO_RGB: f = prophoto_decode; break;
  case GTEXT_CSS_SPACE_REC2020: f = rec2020_decode; break;
  case GTEXT_CSS_SPACE_SRGB_LINEAR: case GTEXT_CSS_SPACE_DISPLAY_P3_LINEAR: f = NULL; break;
  default: break;
  }
  for (int i = 0; i < 3; i++) {
    out[i] = f ? f(in[i]) : in[i];
  }
}

static void rgb_encode_all(GTEXT_CSS_Color_Space s, const double in[3], double out[3]) {
  double (*f)(double) = srgb_encode;
  switch (s) {
  case GTEXT_CSS_SPACE_A98_RGB: f = a98_encode; break;
  case GTEXT_CSS_SPACE_PROPHOTO_RGB: f = prophoto_encode; break;
  case GTEXT_CSS_SPACE_REC2020: f = rec2020_encode; break;
  case GTEXT_CSS_SPACE_SRGB_LINEAR: case GTEXT_CSS_SPACE_DISPLAY_P3_LINEAR: f = NULL; break;
  default: break;
  }
  for (int i = 0; i < 3; i++) {
    out[i] = f ? f(in[i]) : in[i];
  }
}

void css_cs_to_xyz65(GTEXT_CSS_Color_Space from, const double in[3], double out[3]) {
  double v[3];
  switch (from) {
  case GTEXT_CSS_SPACE_SRGB: case GTEXT_CSS_SPACE_SRGB_LINEAR:
    rgb_decode_all(from, in, v);
    mulv(css_cs_srgb_to_xyz, v, out);
    return;
  case GTEXT_CSS_SPACE_DISPLAY_P3: case GTEXT_CSS_SPACE_DISPLAY_P3_LINEAR:
    rgb_decode_all(from, in, v);
    mulv(css_cs_display_p3_to_xyz, v, out);
    return;
  case GTEXT_CSS_SPACE_A98_RGB:
    rgb_decode_all(from, in, v);
    mulv(css_cs_a98_rgb_to_xyz, v, out);
    return;
  case GTEXT_CSS_SPACE_REC2020:
    rgb_decode_all(from, in, v);
    mulv(css_cs_rec2020_to_xyz, v, out);
    return;
  case GTEXT_CSS_SPACE_PROPHOTO_RGB:
    rgb_decode_all(from, in, v);
    mulv(css_cs_prophoto_rgb_to_xyz, v, v);
    mulv(css_cs_d50_to_d65, v, out);
    return;
  case GTEXT_CSS_SPACE_XYZ_D65:
    memcpy(out, in, 3 * sizeof(double));
    return;
  case GTEXT_CSS_SPACE_XYZ_D50:
    mulv(css_cs_d50_to_d65, in, out);
    return;
  case GTEXT_CSS_SPACE_LAB:
    lab_to_xyz50(in, v);
    mulv(css_cs_d50_to_d65, v, out);
    return;
  case GTEXT_CSS_SPACE_LCH:
    from_polar(in, v);
    lab_to_xyz50(v, v);
    mulv(css_cs_d50_to_d65, v, out);
    return;
  case GTEXT_CSS_SPACE_OKLAB:
    oklab_to_xyz65(in, out);
    return;
  case GTEXT_CSS_SPACE_OKLCH:
    from_polar(in, v);
    oklab_to_xyz65(v, out);
    return;
  case GTEXT_CSS_SPACE_HSL:
    hsl_to_srgb(in, v);
    css_cs_to_xyz65(GTEXT_CSS_SPACE_SRGB, v, out);
    return;
  case GTEXT_CSS_SPACE_HWB:
    hwb_to_srgb(in, v);
    css_cs_to_xyz65(GTEXT_CSS_SPACE_SRGB, v, out);
    return;
  default:
    memset(out, 0, 3 * sizeof(double));
    return;
  }
}

void css_cs_from_xyz65(GTEXT_CSS_Color_Space to, const double in[3], double out[3]) {
  double v[3];
  switch (to) {
  case GTEXT_CSS_SPACE_SRGB: case GTEXT_CSS_SPACE_SRGB_LINEAR:
    mulv(css_cs_xyz_to_srgb, in, v);
    rgb_encode_all(to, v, out);
    return;
  case GTEXT_CSS_SPACE_DISPLAY_P3: case GTEXT_CSS_SPACE_DISPLAY_P3_LINEAR:
    mulv(css_cs_xyz_to_display_p3, in, v);
    rgb_encode_all(to, v, out);
    return;
  case GTEXT_CSS_SPACE_A98_RGB:
    mulv(css_cs_xyz_to_a98_rgb, in, v);
    rgb_encode_all(to, v, out);
    return;
  case GTEXT_CSS_SPACE_REC2020:
    mulv(css_cs_xyz_to_rec2020, in, v);
    rgb_encode_all(to, v, out);
    return;
  case GTEXT_CSS_SPACE_PROPHOTO_RGB:
    mulv(css_cs_d65_to_d50, in, v);
    mulv(css_cs_xyz_to_prophoto_rgb, v, v);
    rgb_encode_all(to, v, out);
    return;
  case GTEXT_CSS_SPACE_XYZ_D65:
    memcpy(out, in, 3 * sizeof(double));
    return;
  case GTEXT_CSS_SPACE_XYZ_D50:
    mulv(css_cs_d65_to_d50, in, out);
    return;
  case GTEXT_CSS_SPACE_LAB:
    mulv(css_cs_d65_to_d50, in, v);
    xyz50_to_lab(v, out);
    return;
  case GTEXT_CSS_SPACE_LCH:
    mulv(css_cs_d65_to_d50, in, v);
    xyz50_to_lab(v, v);
    to_polar(v, out);
    return;
  case GTEXT_CSS_SPACE_OKLAB:
    xyz65_to_oklab(in, out);
    return;
  case GTEXT_CSS_SPACE_OKLCH:
    xyz65_to_oklab(in, v);
    to_polar(v, out);
    return;
  case GTEXT_CSS_SPACE_HSL:
    css_cs_from_xyz65(GTEXT_CSS_SPACE_SRGB, in, v);
    srgb_to_hsl(v, out);
    return;
  case GTEXT_CSS_SPACE_HWB:
    css_cs_from_xyz65(GTEXT_CSS_SPACE_SRGB, in, v);
    srgb_to_hwb(v, out);
    return;
  default:
    memset(out, 0, 3 * sizeof(double));
    return;
  }
}

/* ---- missing components and powerless hues (Color 4, 4.4, 4.4.1, 11.2 and 13.3) ------------------------------------------ */

/// The category of component `i` of a space, for the analogous components of Color 4, 13.3 (a table of categories, equal numbers are analogous).
enum { CAT_RED = 1, CAT_GREEN, CAT_BLUE, CAT_LIGHTNESS, CAT_COLORFULNESS, CAT_HUE, CAT_OPP_A, CAT_OPP_B, CAT_WHITENESS, CAT_BLACKNESS };

static int category(GTEXT_CSS_Color_Space s, int i) {
  switch (s) {
  case GTEXT_CSS_SPACE_LAB: case GTEXT_CSS_SPACE_OKLAB: return i == 0 ? CAT_LIGHTNESS : i == 1 ? CAT_OPP_A : CAT_OPP_B;
  case GTEXT_CSS_SPACE_LCH: case GTEXT_CSS_SPACE_OKLCH: return i == 0 ? CAT_LIGHTNESS : i == 1 ? CAT_COLORFULNESS : CAT_HUE;
  case GTEXT_CSS_SPACE_HSL: return i == 0 ? CAT_HUE : i == 1 ? CAT_COLORFULNESS : CAT_LIGHTNESS;
  case GTEXT_CSS_SPACE_HWB: return i == 0 ? CAT_HUE : i == 1 ? CAT_WHITENESS : CAT_BLACKNESS;
  default: return i == 0 ? CAT_RED : i == 1 ? CAT_GREEN : CAT_BLUE; /* the RGB spaces, and the XYZ spaces as "super-saturated RGB" */
  }
}

/* Color 4, "Powerless hue epsilon" of each polar space (hsl: S <= 0.001; lch: C <= 0.0015; oklch: C <= 0.000004; hwb: W + B >= 99.999). `c` holds the numbers of the
   channels, a missing one as 0 ("For all other purposes, a missing component behaves as a zero value", 4.4). */
static bool hue_is_powerless(GTEXT_CSS_Color_Space s, const double c[3]) {
  switch (s) {
  case GTEXT_CSS_SPACE_HSL: return c[1] <= 0.001;
  case GTEXT_CSS_SPACE_LCH: return c[1] <= 0.0015;
  case GTEXT_CSS_SPACE_OKLCH: return c[1] <= 0.000004;
  case GTEXT_CSS_SPACE_HWB: return c[1] + c[2] >= 99.999;
  default: return false;
  }
}

/// Make the hue of `v` missing, with the chroma (or the other measure) set to zero where it was a small positive number (Color 4, 4.4.1).
static void set_hue_missing(css_cs * v) {
  const int h = css_cs_hue_index(v->space);
  if (h < 0 || v->none[h]) {
    return;
  }
  if (v->space == GTEXT_CSS_SPACE_HWB) {
    const double sum = (v->none[1] ? 0.0 : v->c[1]) + (v->none[2] ? 0.0 : v->c[2]);
    if (sum >= 99.999 && sum < 100.0) {
      if (!v->none[1] && !v->none[2]) {
        v->c[2] = 100.0 - v->c[1];
      }
      else if (!v->none[1]) {
        v->c[1] = 100.0;
      }
      else if (!v->none[2]) {
        v->c[2] = 100.0;
      }
    }
  }
  else if (!v->none[1] && v->c[1] > 0.0) {
    v->c[1] = 0.0;
  }
  v->none[h] = true;
  v->c[h] = 0.0;
}

/// Turn a powerless hue of a color into a missing one.
static void make_powerless_missing(css_cs * v) {
  if (css_cs_hue_index(v->space) < 0) {
    return;
  }
  double c[3];
  for (int i = 0; i < 3; i++) {
    c[i] = v->none[i] ? 0.0 : v->c[i];
  }
  if (hue_is_powerless(v->space, c)) {
    set_hue_missing(v);
  }
}

static bool srgb_family(GTEXT_CSS_Color_Space s) {
  return s == GTEXT_CSS_SPACE_SRGB || s == GTEXT_CSS_SPACE_HSL || s == GTEXT_CSS_SPACE_HWB;
}

void css_cs_convert(const css_cs * in, GTEXT_CSS_Color_Space to, css_cs * out) {
  if (in->space == to) {
    *out = *in;
    return;
  }
  css_cs s = *in;
  make_powerless_missing(&s);
  double v[3], xyz[3], r[3];
  for (int i = 0; i < 3; i++) {
    v[i] = s.none[i] ? 0.0 : s.c[i];
  }
  if (srgb_family(s.space) && srgb_family(to)) {
    /* sRGB, HSL and HWB are one space written three ways: no detour through XYZ, which would turn white into a color a hair off the gray axis */
    double rgb[3];
    switch (s.space) {
    case GTEXT_CSS_SPACE_HSL: hsl_to_srgb(v, rgb); break;
    case GTEXT_CSS_SPACE_HWB: hwb_to_srgb(v, rgb); break;
    default: memcpy(rgb, v, sizeof rgb); break;
    }
    switch (to) {
    case GTEXT_CSS_SPACE_HSL: srgb_to_hsl(rgb, r); break;
    case GTEXT_CSS_SPACE_HWB: srgb_to_hwb(rgb, r); break;
    default: memcpy(r, rgb, sizeof r); break;
    }
  }
  else {
    css_cs_to_xyz65(s.space, v, xyz);
    css_cs_from_xyz65(to, xyz, r);
  }
  const bool dest_powerless = css_cs_hue_index(to) >= 0 && hue_is_powerless(to, r);
  css_cs o;
  o.space = to;
  o.alpha = s.alpha;
  o.none[3] = s.none[3];
  for (int i = 0; i < 3; i++) {
    o.c[i] = r[i];
    o.none[i] = false;
  }
  /* the analogous components that were missing are carried forward (Color 4, 13.3) */
  int src_rest = 0, src_missing = 0, dst_rest[3], ndst = 0;
  for (int i = 0; i < 3; i++) {
    const int ci = category(to, i);
    int j = 0;
    for (; j < 3; j++) {
      if (category(s.space, j) == ci) {
        break;
      }
    }
    if (j < 3) {
      if (s.none[j]) {
        o.none[i] = true;
      }
    }
    else {
      dst_rest[ndst++] = i;
    }
  }
  for (int j = 0; j < 3; j++) {
    int i = 0;
    for (; i < 3; i++) {
      if (category(to, i) == category(s.space, j)) {
        break;
      }
    }
    if (i == 3) {
      src_rest++;
      if (s.none[j]) {
        src_missing++;
      }
    }
  }
  if (src_rest > 0 && src_rest == src_missing) {
    for (int k = 0; k < ndst; k++) {
      o.none[dst_rest[k]] = true;
    }
  }
  /* a hue that conversion produced with no chroma is powerless, and so missing (Color 4, 4.4.1) */
  if (dest_powerless) {
    set_hue_missing(&o);
  }
  for (int i = 0; i < 3; i++) {
    if (o.none[i]) {
      o.c[i] = 0.0;
    }
  }
  *out = o;
}
