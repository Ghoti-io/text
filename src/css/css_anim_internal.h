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
 * The animation layer's private interfaces: the combination of two computed values by their type (css_anim_interp.c) and the sampler that puts the value of an effect stack into the
 * cascade of one element (css_anim_sample.c).
 */

#ifndef GHOTI_IO_GTEXT_SRC_CSS_CSS_ANIM_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_CSS_CSS_ANIM_INTERNAL_H

#include <ghoti.io/text/macros.h>

#include <ghoti.io/text/css.h>
#include <stdbool.h>


#ifdef __cplusplus
extern "C" {
#endif

/// How two values are combined (Web Animations 1, "Combining effects": interpolation, and the addition and accumulation of the composite operations).
typedef enum {
  CSS_BLEND_LERP = 0,      ///< Interpolation by `p`.
  CSS_BLEND_ADD,           ///< "value addition": a + b.
  CSS_BLEND_ACCUMULATE     ///< "value accumulation": the same as addition for every type this layer combines.
} css_blend_op;

/// Whether the animation type of the two values lets them be combined (the same shape, the same units or units that convert, equal keywords and strings). False: the type is
/// discrete for this pair. The property-specific rules are applied by the caller before (css_interp_prepare()).
bool css_interp_compatible(const GTEXT_CSS_Value * a, const GTEXT_CSS_Value * b);

/// The value text of the combination of two computed values of a property, as the declaration `property: <text>` would say it. Interpolation is a pure function of the two values
/// and `p`; a pair whose type is discrete gives `a` below 0.5 and `b` from 0.5 on (for addition and accumulation, `b`, which is "replace"). `*discrete` (may be NULL) says that.
/// `*text` is released with gtext_css_serialize_buffer_free() and options whose allocator is `alloc`. GTEXT_CSS_OK, GTEXT_CSS_E_OOM, or a limit.
/// `current_color` (may be NULL) is the element's computed `color`: a `currentcolor` among the values of a color is the element's color when the two are combined, as both browsers do
/// ("the animation of a color is of its used value": Color 4, "Resolving currentcolor"), so a keyframe of `currentcolor` follows the element's `color`.
GTEXT_CSS_Status css_interp_text(const GTEXT_Allocator * alloc, css_blend_op op, const GTEXT_CSS_Value * a, const GTEXT_CSS_Value * b, double p, const GTEXT_CSS_Value * current_color, char ** text,
    size_t * length, bool * discrete);

/// The properties that effects with keyframes would have animated, had their time been given: the style reports them (a resolved value says "no timeline" for them).
typedef struct {
  GTEXT_CSS_Property_Id * ids;   ///< Longhands.
  size_t nids;
  char ** names;                 ///< Custom properties, NUL-terminated.
  size_t * name_lengths;
  size_t nnames;
} css_anim_untimed;

void css_anim_untimed_free(const GTEXT_Allocator * alloc, css_anim_untimed * untimed);

/**
 * Sample the animations of one element. `base` is the block of the cascade's winners. If an effect applies, `*animated` is a new block (the caller's) that is `base` with the
 * animated values declared after it; else NULL. `untimed` (may be NULL) collects the properties of the effects whose time the caller did not give.
 * GTEXT_CSS_OK, or the status of a compute, the allocator or a callback.
 */
GTEXT_CSS_Status css_anim_sample(const GTEXT_CSS_Compute_Context * cc, const GTEXT_CSS_Style_Options * options, GTEXT_CSS_Element_Handle element,
    const GTEXT_CSS_Computed_Style * parent, const GTEXT_CSS_Declaration_Block * base, GTEXT_CSS_Declaration_Block ** animated, css_anim_untimed * untimed);

#ifdef __cplusplus
}
#endif

#endif
