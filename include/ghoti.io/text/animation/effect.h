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
 * @file effect.h
 * @brief Keyframe effects: `KeyframeEffect` of Web Animations 1 (and `iterationComposite` of Web Animations 2) as a value, the keyframes it is made of, and the processing of the `keyframes` argument.
 *
 * Normative text: Web Animations 1, "Keyframe effects" ("Keyframes", "Computing property values", "Calculating computed keyframes", "The effect value of a keyframe effect"), "Combining effects", "The
 * KeyframeEffect interface", "Creating a new KeyframeEffect object", "Property names and IDL names", "Processing a keyframes argument", "The KeyframeEffectOptions dictionary" and "The CompositeOperation
 * and CompositeOperationOrAuto enumerations"; Web Animations 2, "Effect accumulation" and "The KeyframeEffect interfaces"; as pinned in tools/css/DRAFTS_SHA256.
 *
 * **The `keyframes` argument is data.** The type of the argument ("a partially open-ended dictionary") cannot be written in C, and reading an ECMAScript object is the binding's; what the binding
 * hands over is the *result of reading it* (GTEXT_Anim_Keyframes_Argument: the array form as a list of keyframe-like objects, the object form as lists of values by property name), and the procedure
 * "process a keyframes argument" is run here, in C, over that: the keyframe-like object's properties filtered to the animatable ones and sorted by the code points of their names, the lists of the
 * object form turned into keyframes (the offsets, easings and composite modes repeated or truncated as the specification says), the loose sorting and the range of the offsets checked, each value parsed
 * by the syntax of its property (an invalid one discarded), each easing parsed (a failure is a `TypeError`), and the offsets nobody gave computed ("compute missing keyframe offsets").
 *
 * **An effect is the world's.** It belongs to the world that made it (gtext_anim_world_effect_new()) and lives as long as the world: the binding keeps one object for each pointer. An effect is
 * associated with at most one animation. Its timing is its own until an animation holds it, and the animation's from then on; the calls here that read or change the timing do the same either way.
 * Every call that changes what a style would be (the target, the keyframes, the timing, the composite operations) tells the world, so that a style computed before is computed again.
 *
 * **Not here.** The sampling of an effect into a style (the effect stack) is the sampler's (css_style.h, GTEXT_CSS_Style_Animation); this file is what the sampler reads.
 */

#ifndef GHOTI_IO_GTEXT_ANIMATION_EFFECT_H
#define GHOTI_IO_GTEXT_ANIMATION_EFFECT_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/animation/animation.h>
#include <ghoti.io/text/animation/timing.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The animations of one document (world.h). */
typedef struct GTEXT_Anim_World GTEXT_Anim_World;

/** @brief A keyframe effect. Opaque; owned by its world. */
typedef struct GTEXT_Anim_Effect GTEXT_Anim_Effect;

/** @brief `CompositeOperation`: how an effect value is combined with the underlying value ("Effect composition"). */
typedef enum {
  GTEXT_ANIM_COMPOSITE_REPLACE = 0,
  GTEXT_ANIM_COMPOSITE_ADD,
  GTEXT_ANIM_COMPOSITE_ACCUMULATE
} GTEXT_Anim_Composite;

/** @brief `CompositeOperationOrAuto`: the composite operation of one keyframe; `AUTO` uses the effect's. */
typedef enum {
  GTEXT_ANIM_KF_COMPOSITE_AUTO = 0,
  GTEXT_ANIM_KF_COMPOSITE_REPLACE,
  GTEXT_ANIM_KF_COMPOSITE_ADD,
  GTEXT_ANIM_KF_COMPOSITE_ACCUMULATE
} GTEXT_Anim_Keyframe_Composite;

/** @brief `IterationCompositeOperation` (Web Animations 2, "Effect accumulation"). */
typedef enum {
  GTEXT_ANIM_ITERATION_REPLACE = 0,
  GTEXT_ANIM_ITERATION_ACCUMULATE
} GTEXT_Anim_Iteration_Composite;

/** @brief A text the binding read: a `DOMString`, not terminated. */
typedef struct {
  const char * data;
  size_t length;
} GTEXT_Anim_Text;

/** @brief A nullable `double` (`offset`): `is_null` is the value `null`. */
typedef struct {
  bool is_null;
  double value;
} GTEXT_Anim_Offset;

/** @brief One property of a keyframe-like object: the name the object had it under (an IDL attribute name such as `marginLeft` or `cssFloat`, or a custom property's name) and its value as a string. */
typedef struct {
  GTEXT_Anim_Text name;
  GTEXT_Anim_Text value;
} GTEXT_Anim_Keyframe_Property_Input;

/**
 * @struct GTEXT_Anim_Keyframe_Like
 * @brief An element of the array form: "process a keyframe-like object" with `allow lists` false, already read.
 *
 * `null_entry` is an element of the sequence that is `null` or `undefined` (the specification reads such an element as a keyframe-like object with no members: an empty keyframe with the defaults).
 * The defaults of the dictionary `BaseKeyframe` are `offset` null, `easing` `"linear"` (`easing.data` NULL) and `composite` `auto`.
 */
typedef struct {
  bool null_entry;
  GTEXT_Anim_Offset offset;
  GTEXT_Anim_Text easing;                 ///< `data` NULL: the default, `linear`.
  GTEXT_Anim_Keyframe_Composite composite;
  const GTEXT_Anim_Keyframe_Property_Input * properties;   ///< Every own enumerable property of the object, in any order (the call filters and sorts them).
  size_t property_count;
} GTEXT_Anim_Keyframe_Like;

/** @brief One property of the object form: its name and its list of values (a single string is a list of one). */
typedef struct {
  GTEXT_Anim_Text name;
  const GTEXT_Anim_Text * values;
  size_t value_count;
} GTEXT_Anim_Property_List_Input;

/**
 * @struct GTEXT_Anim_Property_Indexed
 * @brief The object form: "process a keyframe-like object" with `allow lists` true, already read. The members `offset`, `easing` and `composite` are lists too (a single value is a list of one; an absent member is
 * the empty list, which is the dictionary's default).
 */
typedef struct {
  const GTEXT_Anim_Offset * offsets;
  size_t offset_count;
  const GTEXT_Anim_Text * easings;
  size_t easing_count;
  const GTEXT_Anim_Keyframe_Composite * composites;
  size_t composite_count;
  const GTEXT_Anim_Property_List_Input * properties;
  size_t property_count;
} GTEXT_Anim_Property_Indexed;

/** @brief Which form the `keyframes` argument has. */
typedef enum {
  GTEXT_ANIM_KEYFRAMES_NULL = 0,          ///< `null`: an empty sequence of keyframes.
  GTEXT_ANIM_KEYFRAMES_ARRAY,             ///< An iterable of keyframe-like objects.
  GTEXT_ANIM_KEYFRAMES_INDEXED            ///< A property-indexed keyframe object.
} GTEXT_Anim_Keyframes_Kind;

/** @struct GTEXT_Anim_Keyframes_Argument @brief The `keyframes` argument of `animate()`, the constructor of `KeyframeEffect` and `setKeyframes()`, as the binding read it. */
typedef struct {
  GTEXT_Anim_Keyframes_Kind kind;
  const GTEXT_Anim_Keyframe_Like * array;
  size_t array_count;
  const GTEXT_Anim_Property_Indexed * indexed;
} GTEXT_Anim_Keyframes_Argument;

/**
 * @struct GTEXT_Anim_Effect_Options
 * @brief `KeyframeEffectOptions` (and, for `animate()`, the members of `KeyframeAnimationOptions` that are the model's): what the options argument said, already read.
 *
 * A number for the options is `timing.present = GTEXT_ANIM_TIMING_DURATION` with that duration. A member the object did not have is absent. `timing.values.easing` is ignored: the easing is the text
 * `easing` (a text that does not parse is a `TypeError`), which the call parses.
 */
typedef struct {
  GTEXT_Anim_Optional_Timing timing;
  GTEXT_Anim_Text easing;
  bool has_composite;
  GTEXT_Anim_Composite composite;
  bool has_iteration_composite;
  GTEXT_Anim_Iteration_Composite iteration_composite;
  bool has_pseudo;                        ///< The `pseudoElement` member exists (it may be `null`: `pseudo.data` NULL).
  GTEXT_Anim_Text pseudo;
} GTEXT_Anim_Effect_Options;

/** @brief A list of keyframes: the keyframes of an effect as `getKeyframes()` returns them, or what the keyframes argument made. Opaque; release with gtext_anim_keyframes_free(). */
typedef struct GTEXT_Anim_Keyframes GTEXT_Anim_Keyframes;

/** @struct GTEXT_Anim_Keyframe_Info @brief One keyframe: the members of `ComputedKeyframe` other than the properties. Strings are valid until the list is freed. */
typedef struct {
  bool offset_null;                       ///< `offset` is `null`.
  double offset;
  double computed_offset;                 ///< `computedOffset`: never null.
  GTEXT_CSS_String easing;                ///< `easing`, serialized.
  GTEXT_Anim_Keyframe_Composite composite;
  size_t property_count;
} GTEXT_Anim_Keyframe_Info;

/** @brief How many keyframes. NULL is 0. */
GTEXT_API size_t gtext_anim_keyframes_count(const GTEXT_Anim_Keyframes * keyframes);

/** @brief Keyframe `index`. @return false past the end. */
GTEXT_API bool gtext_anim_keyframes_at(const GTEXT_Anim_Keyframes * keyframes, size_t index, GTEXT_Anim_Keyframe_Info * out);

/**
 * @brief Property `property` of keyframe `index`: the animation property name as a CSS property (`margin-left`, a custom property's name), the IDL attribute name `getKeyframes()` defines it under
 * ("animation property name to IDL attribute name": `marginLeft`, `cssFloat`, `cssOffset`) and its value, serialized.
 * @return false past the end of either.
 */
GTEXT_API bool gtext_anim_keyframes_property_at(const GTEXT_Anim_Keyframes * keyframes, size_t index, size_t property, GTEXT_CSS_String * name, GTEXT_CSS_String * idl_name, GTEXT_CSS_String * value);

/** @brief Release a list; NULL is ignored. */
GTEXT_API void gtext_anim_keyframes_free(GTEXT_Anim_Keyframes * keyframes);

/**
 * @brief "Process a keyframes argument" for a document (the registry of its properties says which names are animatable and what a value must parse as).
 *
 * @param world The world whose document the keyframes are for. @param argument What the binding read. @param out Receives the keyframes; release with gtext_anim_keyframes_free().
 * @return GTEXT_ANIM_OK; GTEXT_ANIM_TYPE_ERROR (not loosely sorted by offset, an offset outside 0 to 1, or an easing that cannot be parsed; the offsets are not NaN or infinite); GTEXT_ANIM_INVALID; GTEXT_ANIM_OOM.
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_keyframes_process(GTEXT_Anim_World * world, const GTEXT_Anim_Keyframes_Argument * argument, GTEXT_Anim_Keyframes ** out);

/* ---- the effect ----------------------------------------------------------------------------------------------------------------------------------- */

/**
 * @brief `new KeyframeEffect(target, keyframes, options)`: the target (an element and a pseudo-element selector), the keyframes and the options of "Creating a new KeyframeEffect object".
 *
 * Duration, delay and the rest of the timing are the defaults of `EffectTiming` with what `options` says over them (the updating of the timing of an effect); the composite operations are
 * `replace` unless `options` says otherwise; the keyframes are what `keyframes` processes to. Nothing is made if any step throws.
 *
 * @param target The target element (the tree's handle), or NULL for an effect with no target. @param keyframes The argument; NULL is `null`. @param options NULL for none.
 * @param out Receives the effect (owned by the world).
 * @return GTEXT_ANIM_OK; GTEXT_ANIM_TYPE_ERROR; GTEXT_ANIM_SYNTAX_ERROR (a `pseudoElement` that is not a valid pseudo-element selector); GTEXT_ANIM_INVALID; GTEXT_ANIM_OOM.
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_world_effect_new(GTEXT_Anim_World * world, const void * target, const GTEXT_Anim_Keyframes_Argument * keyframes, const GTEXT_Anim_Effect_Options * options,
    GTEXT_Anim_Effect ** out);

/** @brief `new KeyframeEffect(source)`: the target, the keyframes, the composite operations and the timing of `source`. @return GTEXT_ANIM_OK; GTEXT_ANIM_INVALID; GTEXT_ANIM_OOM. */
GTEXT_API GTEXT_Anim_Status gtext_anim_world_effect_copy(GTEXT_Anim_World * world, const GTEXT_Anim_Effect * source, GTEXT_Anim_Effect ** out);

/**
 * @brief `animation.effect`: the keyframe effect an Animation of the world holds. For an animation of CSS (a `CSSAnimation`, a `CSSTransition`) that has an effect the world makes the effect object the first time
 * it is asked for: it reads the timing, the target and the keyframes the style gave the animation (`getTiming()`, `target`, `getComputedTiming()`; `updateTiming()` on a CSS animation); what would
 * change an effect the style owns (`setKeyframes()`, `target =`, `pseudoElement =`, `composite =`) is gtext_anim_effect_set_*()'s GTEXT_ANIM_NOT_SUPPORTED for it.
 * @param out Receives the effect, or NULL for an animation with no effect. @return GTEXT_ANIM_OK; GTEXT_ANIM_INVALID (the Animation is not one of the world's); GTEXT_ANIM_OOM.
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_world_effect_of(GTEXT_Anim_World * world, GTEXT_Anim_Animation * animation, GTEXT_Anim_Effect ** out);

/** @brief The animation an effect is associated with, or NULL. */
GTEXT_API GTEXT_Anim_Animation * gtext_anim_effect_animation(const GTEXT_Anim_Effect * effect);

/** @brief `target` and `pseudoElement`: the element (NULL for none) and the pseudo-element selector with two colons (`::before`; empty for none). The string is valid until the effect changes. */
GTEXT_API GTEXT_Anim_Status gtext_anim_effect_get_target(const GTEXT_Anim_Effect * effect, const void ** element, GTEXT_CSS_String * pseudo);

/** @brief `target = element`: the target element (NULL for none). The pseudo-element selector is kept. @return GTEXT_ANIM_OK; GTEXT_ANIM_INVALID. */
GTEXT_API GTEXT_Anim_Status gtext_anim_effect_set_target(GTEXT_Anim_Effect * effect, const void * element);

/**
 * @brief `pseudoElement = text` ("pseudo-element parsing"): `NULL` is `null`; `:before`, `:after`, `:first-letter` and `:first-line` are the two-colon selectors.
 * @return GTEXT_ANIM_OK; GTEXT_ANIM_SYNTAX_ERROR (an invalid pseudo-element selector: the effect is as it was); GTEXT_ANIM_INVALID; GTEXT_ANIM_OOM.
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_effect_set_pseudo(GTEXT_Anim_Effect * effect, const char * text, size_t length);

/** @brief `composite`. */
GTEXT_API GTEXT_Anim_Composite gtext_anim_effect_get_composite(const GTEXT_Anim_Effect * effect);

/** @brief `composite = value`. @return GTEXT_ANIM_OK; GTEXT_ANIM_INVALID. */
GTEXT_API GTEXT_Anim_Status gtext_anim_effect_set_composite(GTEXT_Anim_Effect * effect, GTEXT_Anim_Composite composite);

/** @brief `iterationComposite`. */
GTEXT_API GTEXT_Anim_Iteration_Composite gtext_anim_effect_get_iteration_composite(const GTEXT_Anim_Effect * effect);

/** @brief `iterationComposite = value`. @return GTEXT_ANIM_OK; GTEXT_ANIM_INVALID. */
GTEXT_API GTEXT_Anim_Status gtext_anim_effect_set_iteration_composite(GTEXT_Anim_Effect * effect, GTEXT_Anim_Iteration_Composite composite);

/**
 * @brief `getTiming()`: the effect's timing (its easing is `timing->easing`, valid until the timing changes) and the easing serialized.
 * @param timing Receives the timing; may be NULL. @param easing Receives the serialized easing (valid until the timing changes); may be NULL.
 * @return GTEXT_ANIM_OK; GTEXT_ANIM_INVALID; GTEXT_ANIM_OOM.
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_effect_get_timing(const GTEXT_Anim_Effect * effect, GTEXT_Anim_Effect_Timing * timing, GTEXT_CSS_String * easing);

/**
 * @brief `updateTiming(timing)`: the members that exist are checked, then assigned; an `easing` text that does not parse is a `TypeError`. All or nothing. The animation's finished state is updated.
 * @param easing The text of the `easing` member when `input->present` has GTEXT_ANIM_TIMING_EASING; else ignored.
 * @return GTEXT_ANIM_OK; GTEXT_ANIM_TYPE_ERROR; GTEXT_ANIM_INVALID; GTEXT_ANIM_OOM.
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_effect_update_timing(GTEXT_Anim_Effect * effect, const GTEXT_Anim_Optional_Timing * input, const GTEXT_Anim_Text * easing, GTEXT_Anim_Time now);

/** @brief `getComputedTiming()` at the animation's current time (unresolved with no animation). @return GTEXT_ANIM_OK; GTEXT_ANIM_INVALID. */
GTEXT_API GTEXT_Anim_Status gtext_anim_effect_get_computed_timing(const GTEXT_Anim_Effect * effect, GTEXT_Anim_Time now, GTEXT_Anim_Computed_Timing * out);

/**
 * @brief `getKeyframes()`: the keyframes of the effect with their computed offsets. For an effect made by script, the keyframes as the argument made them; for an effect of a CSS transition, its two keyframes (the
 * computed values of the property at its start and at its end, easing `linear`, composite `auto`). For an effect of a CSS animation the *computed keyframes* of its `@keyframes` rule (CSS Animations 2, "Keyframes":
 * the declarations grouped by offset, timing function and composite operation, the initial and final keyframes generated, the values computed in the context of the target) are GTEXT_ANIM_NOT_SUPPORTED: the sampler
 * keeps one keyframe for each offset, which is not that model.
 * @return GTEXT_ANIM_OK; GTEXT_ANIM_NOT_SUPPORTED; GTEXT_ANIM_INVALID; GTEXT_ANIM_OOM.
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_effect_get_keyframes(GTEXT_Anim_Effect * effect, GTEXT_Anim_Keyframes ** out);

/**
 * @brief `setKeyframes(keyframes)`: the effect's keyframes become what the argument processes to. If it throws, they are not modified. After it an effect of a CSS animation or transition no longer follows its
 * style.
 * @return GTEXT_ANIM_OK; GTEXT_ANIM_TYPE_ERROR; GTEXT_ANIM_INVALID; GTEXT_ANIM_OOM.
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_effect_set_keyframes(GTEXT_Anim_Effect * effect, const GTEXT_Anim_Keyframes_Argument * keyframes);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_ANIMATION_EFFECT_H
