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
 * @file css_keyframes.h
 * @brief `@keyframes`, typed: the name, the blocks as written, and the normalized list.
 *
 * Normative text: CSS Animations 1 (3 Keyframes: "the keyframes are sorted by offset; ... If multiple keyframes share an offset, ... they are cascaded in the order they appear";
 * "!important ... ignored"; the animation properties are ignored), as served 2026-10-29.
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_KEYFRAMES_H
#define GHOTI_IO_GTEXT_CSS_CSS_KEYFRAMES_H

#include <ghoti.io/text/css/css_atrule.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief One property of a keyframe. The strings and the value set belong to the rule, which must outlive the keyframes. */
typedef struct {
  GTEXT_CSS_Property_Id property;
  GTEXT_CSS_String name;
  GTEXT_CSS_String text;                 ///< The value as canonical text.
  const GTEXT_CSS_Value_Set * set;       ///< The typed value (NULL for a custom property).
} GTEXT_CSS_Keyframe_Property;

/** @brief One keyframe of the normalized list: an offset (0 to 1) and its properties, the later of a property winning. */
typedef struct {
  double offset;
  const GTEXT_CSS_Keyframe_Property * properties;
  size_t property_count;
} GTEXT_CSS_Keyframe;

/** @brief What a `@keyframes` rule wrote. */
typedef struct GTEXT_CSS_Keyframes {
  GTEXT_CSS_String name;                 ///< The name (an identifier or a string, decoded).
  bool name_is_string;
  size_t block_count;                    ///< The blocks as written (a block with several selectors counts once).
  const GTEXT_CSS_Keyframe * frames;     ///< The normalized list, offsets ascending, one per offset.
  size_t frame_count;
  const GTEXT_CSS_Atrule * rule;
  void * priv_;
} GTEXT_CSS_Keyframes;

/** @brief Read a `@keyframes` rule. @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM. */
GTEXT_API GTEXT_CSS_Status gtext_css_keyframes_from_atrule(const GTEXT_CSS_Atrule * rule, GTEXT_CSS_Keyframes ** out);

/** @brief Release keyframes; NULL is ignored. Keyframes found in a set belong to it. */
GTEXT_API void gtext_css_keyframes_free(GTEXT_CSS_Keyframes * keyframes);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_KEYFRAMES_H
