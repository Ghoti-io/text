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
 * The color spaces of CSS Color 4 and the conversions between them (private). Every conversion goes through XYZ-D65, with a Bradford adaptation to and
 * from D50 where a space is defined against D50 (Lab, LCH, XYZ-D50, ProPhoto), in IEEE double arithmetic and the `math` library's functions only (no
 * `<math.h>`: a color means the same number on every host). A channel that is missing (`none`) is carried as a flag beside the number, never as a NaN.
 */

#ifndef GHOTI_IO_GTEXT_CSS_COLOR_SPACE_H
#define GHOTI_IO_GTEXT_CSS_COLOR_SPACE_H

#include <stdbool.h>

#include <ghoti.io/text/css/css_color.h>

#include <ghoti.io/text/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/// A color in one space: three channels in the space's own units (sRGB 0 to 1, Lab L 0 to 100, a hue in degrees ...), an alpha 0 to 1, and which are missing.
typedef struct {
  GTEXT_CSS_Color_Space space;
  double c[3];
  double alpha;
  bool none[4]; ///< The three channels and the alpha.
} css_cs;

/// The matrices of tools/css/gen-color-tables.py (src/css/css_color_tables.c).
GTEXT_INTERNAL_API extern const double css_cs_srgb_to_xyz[3][3];
GTEXT_INTERNAL_API extern const double css_cs_xyz_to_srgb[3][3];
GTEXT_INTERNAL_API extern const double css_cs_display_p3_to_xyz[3][3];
GTEXT_INTERNAL_API extern const double css_cs_xyz_to_display_p3[3][3];
GTEXT_INTERNAL_API extern const double css_cs_a98_rgb_to_xyz[3][3];
GTEXT_INTERNAL_API extern const double css_cs_xyz_to_a98_rgb[3][3];
GTEXT_INTERNAL_API extern const double css_cs_prophoto_rgb_to_xyz[3][3];
GTEXT_INTERNAL_API extern const double css_cs_xyz_to_prophoto_rgb[3][3];
GTEXT_INTERNAL_API extern const double css_cs_rec2020_to_xyz[3][3];
GTEXT_INTERNAL_API extern const double css_cs_xyz_to_rec2020[3][3];
GTEXT_INTERNAL_API extern const double css_cs_d65_to_d50[3][3];
GTEXT_INTERNAL_API extern const double css_cs_d50_to_d65[3][3];
GTEXT_INTERNAL_API extern const double css_cs_xyz_to_lms[3][3];
GTEXT_INTERNAL_API extern const double css_cs_lms_to_xyz[3][3];
GTEXT_INTERNAL_API extern const double css_cs_lmsp_to_oklab[3][3];
GTEXT_INTERNAL_API extern const double css_cs_oklab_to_lmsp[3][3];

/// Whether the space has a hue channel (LCH, OKLCH, HSL, HWB) and which index it is (0 for HSL and HWB, 2 for LCH and OKLCH); -1 for none.
GTEXT_INTERNAL_API int css_cs_hue_index(GTEXT_CSS_Color_Space space);
/// Whether the space is one of the polar ones.
GTEXT_INTERNAL_API bool css_cs_is_polar(GTEXT_CSS_Color_Space space);
/// Whether the space is one that the legacy `rgb()`, `hsl()` and `hwb()` forms are in (sRGB, HSL, HWB).
GTEXT_INTERNAL_API bool css_cs_is_legacy(GTEXT_CSS_Color_Space space);

/// Convert. `in` and `out` may not be the same object. Missing channels: a channel that is missing in the source stays missing in the target only where the two spaces
/// share it (Color 4, "Missing color components and the none keyword": the analogous components); every other channel of the target is computed from the values
/// in which the missing ones are 0. A hue that is powerless is missing in the result of a conversion into a polar space.
GTEXT_INTERNAL_API void css_cs_convert(const css_cs * in, GTEXT_CSS_Color_Space to, css_cs * out);

/// To XYZ-D65 and from it (the channels as numbers; no flags).
GTEXT_INTERNAL_API void css_cs_to_xyz65(GTEXT_CSS_Color_Space from, const double in[3], double out[3]);
GTEXT_INTERNAL_API void css_cs_from_xyz65(GTEXT_CSS_Color_Space to, const double in[3], double out[3]);

#ifdef __cplusplus
}
#endif

#endif
