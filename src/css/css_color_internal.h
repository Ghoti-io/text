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
 * What the color files share: the entry that reads a color into an arena (used by the
 * public call and by the value matcher), the serializer, and the name tables.
 *
 * css_color.c reads colors (an explicit stack of frames, so colors nested in colors
 * are a limit and not a recursion); css_color_serialize.c writes the declared value
 * (also with an explicit stack); css_color_names.c holds the tables.
 *
 * Reference: CSS Color Modules Levels 4 and 5, Editor's Drafts (served 2026-10-22).
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSS_COLOR_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSS_COLOR_INTERNAL_H

#include <ghoti.io/text/css/css_color.h>
#include <ghoti.io/text/macros.h>

#include "css_internal.h"
#include "css_math_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Whether a component value can be a color at all: an identifier, a hash, or a function of a color function's name.
bool css_color_is_candidate(const GTEXT_CSS_Component_Value * v);

/**
 * Read the color at `value` into `arena`. OK with *valid false when it is not a color. `work` receives the units of work and `nodes` the colors made (each NULL if not wanted), `where`
 * the component value where a limit was reached (NULL if not wanted).
 * `max_nesting`, `max_math_depth` and `max_math_nodes` of 0 select the defaults.
 */
GTEXT_CSS_Status css_color_build(css_arena * arena, const GTEXT_Allocator * scratch, const GTEXT_CSS_Component_Value * value,
    size_t max_nesting, size_t max_math_depth, size_t max_math_nodes, GTEXT_CSS_Color ** out, bool * valid, size_t * work,
    size_t * nodes, GTEXT_CSS_Span * where);

/// Serialize a color into a new buffer (NUL-terminated, from `alloc`).
GTEXT_CSS_Status css_color_serialize_to(const GTEXT_CSS_Color * color, const GTEXT_Allocator * alloc, size_t max_bytes,
    char ** out, size_t * out_len);

/// A named color: its value as 0xRRGGBB and its canonical (lower case) spelling.
bool css_color_named_lookup(const char * name, size_t length, uint32_t * rgb, const char ** canonical);
/// 1 for a system color, 2 for a deprecated one, 0 for neither; `lower` (of `cap` bytes) receives the keyword in lower case.
int css_color_system_lookup(const char * name, size_t length, char * lower, size_t cap);

/// The sRGB value (r, g, b in 0 to 255, alpha 0 to 1) of an hsl or hwb color given as degrees, and percentages as numbers (0 to 100).
void css_color_hsl_to_srgb(double h, double s, double l, double rgb[3]);
void css_color_hwb_to_srgb(double h, double w, double b, double rgb[3]);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_CSS_CSS_COLOR_INTERNAL_H
