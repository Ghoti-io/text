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
 * @file css_animation.h
 * @brief The animation effects of an element: the `animation-*` longhands of its computed style, read as one list of effects.
 *
 * Normative text: CSS Animations 1 (`animation-name` is "the coordinating list base property" of the `animation-*` properties: each item of the list defines one animation effect; "If
 * multiple animations are attempting to modify the same property, then the animation closest to the end of the list of names wins"), CSS Animations 2 (`animation-delay-start`,
 * `animation-delay-end`, `animation-composition`), and Web Animations 1 (the timing model that gives each field its meaning), as pinned in tools/css/DRAFTS_SHA256.
 *
 * The other longhands are lists that **coordinate** with the names (CSS Values 4, "Linked Properties"): a list shorter than the list of names is repeated to its length, a longer one is
 * truncated. An effect whose name is `none` is not an effect (it keeps its place in the list, so that the lists keep coordinating, and is not in the result).
 *
 * The list is made from the computed values of a style, so it is a pure function of the style. It does not sample anything: the timing model and the keyframes are the sampler's
 * (css_style.h, GTEXT_CSS_Style_Options::animation).
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_ANIMATION_H
#define GHOTI_IO_GTEXT_CSS_CSS_ANIMATION_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_computed.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_function.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief `animation-direction` (the playback direction of Web Animations 1). */
typedef enum {
  GTEXT_CSS_ANIM_DIRECTION_NORMAL = 0,
  GTEXT_CSS_ANIM_DIRECTION_REVERSE,
  GTEXT_CSS_ANIM_DIRECTION_ALTERNATE,
  GTEXT_CSS_ANIM_DIRECTION_ALTERNATE_REVERSE
} GTEXT_CSS_Anim_Direction;

/** @brief `animation-fill-mode` (the fill mode of Web Animations 1). */
typedef enum {
  GTEXT_CSS_ANIM_FILL_NONE = 0,
  GTEXT_CSS_ANIM_FILL_FORWARDS,
  GTEXT_CSS_ANIM_FILL_BACKWARDS,
  GTEXT_CSS_ANIM_FILL_BOTH
} GTEXT_CSS_Anim_Fill;

/** @brief `animation-composition` (the composite operation of Web Animations 1). */
typedef enum {
  GTEXT_CSS_ANIM_COMPOSITE_REPLACE = 0,
  GTEXT_CSS_ANIM_COMPOSITE_ADD,
  GTEXT_CSS_ANIM_COMPOSITE_ACCUMULATE
} GTEXT_CSS_Anim_Composite;

/**
 * @struct GTEXT_CSS_Anim_Effect
 * @brief One animation of an element: the values of its place in each `animation-*` list. Strings and the easing belong to the list.
 */
typedef struct {
  size_t index;                       ///< Its place in the `animation-name` list (an effect with the name `none` is not here, so the indexes may skip).
  GTEXT_CSS_String name;              ///< The name of the `@keyframes` rule (an identifier or a string, decoded).
  bool name_is_string;
  bool duration_auto;                 ///< `animation-duration: auto`. For the document timeline "auto" is 0s; `duration_ms` is 0.
  double duration_ms;                 ///< The iteration duration in milliseconds (0 for `auto`).
  const GTEXT_CSS_Easing * easing;    ///< `animation-timing-function` of the effect (never NULL for an effect of a list).
  double delay_start_ms;              ///< `animation-delay-start` in milliseconds (the start delay).
  double delay_end_ms;                ///< `animation-delay-end` in milliseconds (the end delay).
  bool infinite;                      ///< `animation-iteration-count: infinite`.
  double iteration_count;             ///< The count when it is not `infinite` (0 or more); 0 when infinite.
  GTEXT_CSS_Anim_Direction direction;
  GTEXT_CSS_Anim_Fill fill;
  GTEXT_CSS_Anim_Composite composite;
  bool paused;                        ///< `animation-play-state: paused`.
  bool timeline_auto;                 ///< `animation-timeline: auto`, the document timeline. Any other timeline (a scroll or view timeline, a name, `none`) is not sampled here.
} GTEXT_CSS_Anim_Effect;

/** @brief The effects of one element (the struct is private). */
typedef struct GTEXT_CSS_Anim_List GTEXT_CSS_Anim_List;

/**
 * @brief Read the animation effects from the computed values of a style.
 *
 * @param style The style. Not NULL.
 * @param allocator NULL for the style's.
 * @param out Receives the list (possibly with no effects); release it with gtext_css_anim_list_free(). Written only on success. Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM; GTEXT_CSS_E_UNSUPPORTED (the registry of the style has no `animation-name`, or a value is not the shape the grammar gives).
 */
GTEXT_API GTEXT_CSS_Status gtext_css_anim_list_from_style(const GTEXT_CSS_Computed_Style * style, const GTEXT_Allocator * allocator, GTEXT_CSS_Anim_List ** out);

/** @brief How many effects. NULL is 0. */
GTEXT_API size_t gtext_css_anim_list_count(const GTEXT_CSS_Anim_List * list);

/** @brief Effect `index` in list order (the last has the highest composite order). @return false past the end. */
GTEXT_API bool gtext_css_anim_list_at(const GTEXT_CSS_Anim_List * list, size_t index, GTEXT_CSS_Anim_Effect * out);

/** @brief Release a list and what it holds; NULL is ignored. */
GTEXT_API void gtext_css_anim_list_free(GTEXT_CSS_Anim_List * list);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_ANIMATION_H
