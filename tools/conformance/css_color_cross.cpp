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
 * The cross-check of the color computation against the `color` library (check-css-color-cross): the matrices of sRGB, Display P3, Adobe RGB (`a98-rgb`), Rec. 2020 and
 * ProPhoto, the Bradford adaptation between D65 and D50, and the transfer functions of sRGB, Adobe RGB and ProPhoto, compared with what `color` computes from its named
 * gamuts and transfer curves. `text` does not depend on `color` (it needs the CSS spaces, which `color` does not have, and its determinism rule forbids the C library's
 * math functions that `color` uses); this program is the test that the two agree where they overlap, and so the test of any later replacement of one by the other.
 *
 * Exit status: 0 when every figure agrees to the stated tolerance, 1 when one does not.
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include <ghoti.io/color/color.h>
#include <ghoti.io/color/math.h>

#include "../../src/css/css_color_space.h"

namespace {

int failures = 0;

/// A tolerance below zero is absolute (against 1); else relative to the figure of `color`.
void check(const char * what, double ours, double theirs, double tolerance) {
  if (tolerance < 0.0) {
    if (!(std::fabs(ours - theirs) <= -tolerance)) {
      std::printf("DISAGREE %s: text %.17g, color %.17g (absolute %.3g, allowed %.3g)\n", what, ours, theirs, std::fabs(ours - theirs), -tolerance);
      failures++;
    }
    return;
  }
  const double scale = std::fabs(theirs) > 1e-300 ? std::fabs(theirs) : 1.0;
  const double rel = std::fabs(ours - theirs) / scale;
  if (!(rel <= tolerance)) {
    std::printf("DISAGREE %s: text %.17g, color %.17g (relative %.3g, allowed %.3g)\n", what, ours, theirs, rel, tolerance);
    failures++;
  }
}

struct Space {
  const char * name;
  GCOL_Primaries named;
  const double (*to_xyz)[3];
  GTEXT_CSS_Color_Space space;
  double tolerance;   ///< 1e-12 (relative) where both are derived from the same chromaticities; negative: absolute.
};

}  // namespace

int main() {
  const Space spaces[] = {
    {"srgb", GCOL_PRIMARIES_SRGB, css_cs_srgb_to_xyz, GTEXT_CSS_SPACE_SRGB, 1e-12},
    {"display-p3", GCOL_PRIMARIES_DISPLAY_P3, css_cs_display_p3_to_xyz, GTEXT_CSS_SPACE_DISPLAY_P3, 1e-12},
    {"a98-rgb", GCOL_PRIMARIES_ADOBE_RGB, css_cs_a98_rgb_to_xyz, GTEXT_CSS_SPACE_A98_RGB, 1e-12},
    {"rec2020", GCOL_PRIMARIES_BT2020, css_cs_rec2020_to_xyz, GTEXT_CSS_SPACE_REC2020, 1e-12},
    /* Color 4 gives ProPhoto's primaries to six digits (0.734699 ...); the standard's, and `color`'s, have four (0.7347 ...): the matrices differ in the fifth digit, which is a difference of at most 1e-5 of the matrix and not a mistake in either */
    {"prophoto-rgb", GCOL_PRIMARIES_PROPHOTO, css_cs_prophoto_rgb_to_xyz, GTEXT_CSS_SPACE_PROPHOTO_RGB, -1e-5},
  };
  for (const Space & s : spaces) {
    GCOL_Gamut g;
    if (!gcol_gamut_named(s.named, &g)) {
      std::printf("color has no gamut named %s\n", s.name);
      failures++;
      continue;
    }
    double m[9];
    if (gcol_gamut_to_xyz(&g, m) != GCOL_OK) {
      std::printf("color cannot make the matrix of %s\n", s.name);
      failures++;
      continue;
    }
    for (int i = 0; i < 3; i++) {
      for (int j = 0; j < 3; j++) {
        char what[96];
        std::snprintf(what, sizeof what, "%s matrix [%d][%d]", s.name, i, j);
        if (std::fabs(m[i * 3 + j]) < 1e-12 && std::fabs(s.to_xyz[i][j]) < 1e-12) {
          continue;   // the zero entries of ProPhoto's third row
        }
        check(what, s.to_xyz[i][j], m[i * 3 + j], s.tolerance);
      }
    }
  }

  /* the Bradford adaptation, both ways, between the D65 and D50 whites of Color 4 */
  const GCOL_Chromaticity d65 = {0.3127, 0.3290}, d50 = {0.3457, 0.3585};
  double a[9], b[9];
  if (gcol_chromatic_adapt(d65, d50, GCOL_CAT_BRADFORD, a) != GCOL_OK || gcol_chromatic_adapt(d50, d65, GCOL_CAT_BRADFORD, b) != GCOL_OK) {
    std::printf("color cannot adapt D65 to D50\n");
    failures++;
  }
  else {
    for (int i = 0; i < 3; i++) {
      for (int j = 0; j < 3; j++) {
        char what[96];
        std::snprintf(what, sizeof what, "bradford D65 to D50 [%d][%d]", i, j);
        check(what, css_cs_d65_to_d50[i][j], a[i * 3 + j], 1e-12);
        std::snprintf(what, sizeof what, "bradford D50 to D65 [%d][%d]", i, j);
        check(what, css_cs_d50_to_d65[i][j], b[i * 3 + j], 1e-12);
      }
    }
  }

  /* the transfer functions: ours are read through a conversion to linear light (srgb-linear has none) and compared with color's decode of the same encoded values */
  GCOL_Color_Info srgb, gamma_a98, gamma_pro;
  std::memset(&srgb, 0, sizeof srgb);
  srgb.transfer = GCOL_TRANSFER_SRGB;
  gamma_a98 = srgb;
  gamma_a98.transfer = GCOL_TRANSFER_GAMMA;
  gamma_a98.gamma_value = 563.0 / 256.0;
  gamma_pro = srgb;
  gamma_pro.transfer = GCOL_TRANSFER_GAMMA;
  gamma_pro.gamma_value = 1.8;
  for (double v : {0.0, 0.001, 0.02, 0.04045, 0.1, 0.25, 0.5, 0.75, 0.9, 1.0}) {
    css_cs in{};
    in.space = GTEXT_CSS_SPACE_SRGB;
    in.c[0] = in.c[1] = in.c[2] = v;
    in.alpha = 1.0;
    css_cs lin{};
    css_cs_convert(&in, GTEXT_CSS_SPACE_SRGB_LINEAR, &lin);
    char what[96];
    std::snprintf(what, sizeof what, "srgb transfer of %g", v);
    check(what, lin.c[0], gcol_transfer_decode(&srgb, v), 1e-12);
    in.space = GTEXT_CSS_SPACE_A98_RGB;
    in.c[0] = in.c[1] = in.c[2] = v;
    css_cs xyz{};
    css_cs_convert(&in, GTEXT_CSS_SPACE_XYZ_D65, &xyz);
    /* a gray's Y is the decoded value, the matrix rows summing to the white's Y of 1 */
    std::snprintf(what, sizeof what, "a98-rgb transfer of %g", v);
    check(what, xyz.c[1], gcol_transfer_decode(&gamma_a98, v), 1e-9);
  }
  if (failures) {
    std::printf("check-css-color-cross: %d figures disagree\n", failures);
    return 1;
  }
  std::printf("check-css-color-cross: matrices, adaptation and transfers agree with color\n");
  return 0;
}
