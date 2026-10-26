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
 * The text of the `grid-template` and `grid` shorthands, cut into their longhands and joined again (css_grid_text.c). Private.
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_GRID_TEXT_H
#define GHOTI_IO_GTEXT_SRC_CSS_GRID_TEXT_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  CSS_GRID_OK = 0,
  CSS_GRID_OOM,
  CSS_GRID_UNSUPPORTED   ///< The text is not of a shape this file reads (a value of the shorthand's grammar always is).
} css_grid_status;

/// Split a `grid-template` value into the texts of grid-template-rows, -columns and -areas (new strings). The allocator is the caller's.
css_grid_status css_grid_template_split(const GTEXT_Allocator * a, const char * text, char * out[3]);

/// Split a `grid` value into rows, columns, areas, auto-rows, auto-columns, auto-flow.
css_grid_status css_grid_split(const GTEXT_Allocator * a, const char * text, char * out[6]);

/// Join the three longhands of `grid-template` into its value: `*text` is a new string, or NULL when they make none.
css_grid_status css_grid_template_join(const GTEXT_Allocator * a, const char * const texts[3], char ** text);

/// Join the six longhands of `grid`.
css_grid_status css_grid_join(const GTEXT_Allocator * a, const char * const texts[6], char ** text);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_CSS_GRID_TEXT_H
