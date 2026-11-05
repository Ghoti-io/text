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
 * The computed value of a color that is more than a literal (private): `color-mix()`, relative colors, `alpha()`, `contrast-color()`, and any color
 * function whose channels are calculations that need the environment (`lab(calc(50 + sign(2cqw - 10px) * 10) 30 50)`).
 *
 * The result is a color in a space (css_color_space.h) and the text it computes to: Color 5, 11.1 (the form of a `color-mix()` result by its space), 11.3
 * and Color 4, 15 and 16.2.2 (a relative color is `color(srgb ...)` for `rgb()`, `hsl()` and `hwb()`; `hsl()` and `hwb()` keep their function when a
 * channel is `none`). A color that cannot be computed here (a custom color space, a `currentcolor` inside a mix, a system color nobody resolves) is
 * reported as not done and is left as written for the caller to flag.
 */

#ifndef GHOTI_IO_GTEXT_CSS_COLOR_COMPUTE_H
#define GHOTI_IO_GTEXT_CSS_COLOR_COMPUTE_H

#include <stdbool.h>
#include <stddef.h>

#include <ghoti.io/text/macros.h>
#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_color.h>
#include <ghoti.io/text/css/css_element.h>

#include "css_color_space.h"

#ifdef __cplusplus
extern "C" {
#endif

/// A computed color: the color, and whether it is the result of a legacy sRGB syntax (`rgb()`, `hsl()`, `hwb()`, a hex or a named color) that nothing has
/// changed, which is written in the legacy form and not as `color(srgb ...)`.
typedef struct {
  css_cs cs;
  bool legacy;
} css_cc_value;

/// What the computation may ask of its caller.
typedef struct {
  const GTEXT_Allocator * alloc;
  GTEXT_CSS_Color_Scheme scheme;     ///< The scheme `light-dark()` uses.
  bool has_current;                  ///< `current` is the used value of the `color` property.
  css_cc_value current;
  bool current_anywhere;             ///< `currentcolor` is `current` inside a mix or a relative color too (a registered property); else only inside `contrast-color()`.
  /// The size of a unit of a calculation in a channel (px for a length), or false: this environment cannot say (the color then stays as written).
  bool (*unit_size)(void * user, GTEXT_CSS_Unit unit, double * canonical);
  /// A color the library cannot compute (a system color, `device-cmyk()`, `color()` with a custom space): its sRGB value (0 to 255, alpha 0 to 1), or false.
  bool (*resolve)(void * user, const GTEXT_CSS_Color * color, double rgba[4]);
  /// The value of `sibling-index()` (`index`) or `sibling-count()` in a channel's calculation for the element being computed, or false: this environment cannot count the tree
  /// (the color then stays as written). NULL counts nothing.
  bool (*tree_count)(void * user, bool index, double * value);
  void * user;
  size_t work;                       ///< Units of work done (a counter for the cost tests).
} css_color_ctx;

/**
 * Compute `color`. `*done` is true with `out` written, or false (nothing written) when the color has to stay as written.
 * GTEXT_CSS_OK, GTEXT_CSS_E_OOM.
 */
GTEXT_INTERNAL_API GTEXT_CSS_Status css_color_compute(css_color_ctx * ctx, const GTEXT_CSS_Color * color, css_cc_value * out, bool * done);

/// The most bytes css_color_value_text() writes.
#define CSS_CC_TEXT_MAX 640

/// The text a computed color is written as (`color(srgb 0.5 0 0.5 / 0.5)`, `lch(50 20 120)`, `rgb(0 0 0)`), NUL-terminated, in at most CSS_CC_TEXT_MAX bytes; its length. `written`, if not NULL, receives the color as it was written (in the space of the text).
GTEXT_INTERNAL_API size_t css_color_value_text(const css_cc_value * v, char * out, css_cs * written);

#ifdef __cplusplus
}
#endif

#endif
