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
 * @file timing.h
 * @brief The timing of an animation effect as a value: `EffectTiming`, `OptionalEffectTiming` and `ComputedEffectTiming` of Web Animations 1, and the timing model that turns the first into the last.
 *
 * Normative text: Web Animations 1, "Timing model" (the active interval, "Animation effect phases and states", "Fill behavior and fill modes", "Repeating", "Direction control", "Calculating
 * the active time", "Calculating the overall progress", "Calculating the simple iteration progress", "Calculating the current iteration index", "Calculating the directed progress",
 * "Calculating the transformed progress") and "The EffectTiming and OptionalEffectTiming dictionaries" with "Updating the timing of an AnimationEffect", as pinned in tools/css/DRAFTS_SHA256.
 *
 * **The model owns no clock.** Every call that depends on a time takes it: a *local time* (an effect's local time is its animation's current time, or unresolved), as a GTEXT_Anim_Time. An
 * unresolved time is a value (`resolved` false), never a NaN, so a caller can pass it and a test can assert it. The result of gtext_anim_computed_timing() is `ComputedEffectTiming` as a value;
 * a member the interface gives as `null` is a flag that says it is absent. An infinity is a real infinity (`iterations`, `duration`, `endTime`, `activeDuration` and `currentIteration` of an
 * `infinite` effect are the `Infinity` the IDL says), so a binding passes the number through.
 *
 * **Exceptions are statuses.** The DOMExceptions of the IDL by name (GTEXT_Anim_Status); the caller turns a status into the exception. Nothing here reads a time, allocates a Promise or calls back
 * into a host.
 *
 * The timing model of the effects of CSS Animations at a playback rate of 1, which css_animation.h has always had, is the same arithmetic: it stays as the fast path of the sampler, and a test
 * holds the two to the same answer on every input the older one takes.
 */

#ifndef GHOTI_IO_GTEXT_ANIMATION_TIMING_H
#define GHOTI_IO_GTEXT_ANIMATION_TIMING_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_function.h>
#include <ghoti.io/text/css/css_registry.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The outcome of a call of the Web Animations model: the exceptions of the IDL by name, so that a binding throws what the specification says.
 */
typedef enum {
  GTEXT_ANIM_OK = 0,
  GTEXT_ANIM_TYPE_ERROR,                ///< A `TypeError`.
  GTEXT_ANIM_RANGE_ERROR,               ///< A `RangeError`.
  GTEXT_ANIM_INVALID_STATE,             ///< An `InvalidStateError`.
  GTEXT_ANIM_NOT_SUPPORTED,             ///< A `NotSupportedError`.
  GTEXT_ANIM_NO_MODIFICATION_ALLOWED,   ///< A `NoModificationAllowedError`.
  GTEXT_ANIM_ABORT,                     ///< An `AbortError`.
  GTEXT_ANIM_INVALID,                   ///< A call that is wrong in C (a NULL argument, an id of no object): not an exception of the IDL.
  GTEXT_ANIM_OOM                        ///< An allocation failed; nothing changed.
} GTEXT_Anim_Status;

/** @brief A time value of Web Animations: a number of milliseconds, or unresolved (`resolved` false; then `ms` is 0). The inactive timeline's current time is unresolved. */
typedef struct {
  bool resolved;
  double ms;
} GTEXT_Anim_Time;

/** @brief `FillMode`. `AUTO` computes to `NONE` ("When performing timing calculations the special string value auto is expanded to none") and is kept so that `getTiming()` can say it. */
typedef enum {
  GTEXT_ANIM_FILL_AUTO = 0,
  GTEXT_ANIM_FILL_NONE,
  GTEXT_ANIM_FILL_FORWARDS,
  GTEXT_ANIM_FILL_BACKWARDS,
  GTEXT_ANIM_FILL_BOTH
} GTEXT_Anim_Fill;

/** @brief `PlaybackDirection`. */
typedef enum {
  GTEXT_ANIM_DIRECTION_NORMAL = 0,
  GTEXT_ANIM_DIRECTION_REVERSE,
  GTEXT_ANIM_DIRECTION_ALTERNATE,
  GTEXT_ANIM_DIRECTION_ALTERNATE_REVERSE
} GTEXT_Anim_Direction;

/** @brief The phase of an effect (Web Animations 1, "Animation effect phases and states"): `IDLE` is "in none of the phases", which an effect with an unresolved local time is. The values are those of `ComputedEffectTiming`'s phase by name. */
typedef enum {
  GTEXT_ANIM_PHASE_IDLE = 0,
  GTEXT_ANIM_PHASE_BEFORE,
  GTEXT_ANIM_PHASE_ACTIVE,
  GTEXT_ANIM_PHASE_AFTER
} GTEXT_Anim_Phase;

/** @brief `AnimationPlayState` (Web Animations 1, "Play states"). */
typedef enum {
  GTEXT_ANIM_PLAY_IDLE = 0,
  GTEXT_ANIM_PLAY_RUNNING,
  GTEXT_ANIM_PLAY_PAUSED,
  GTEXT_ANIM_PLAY_FINISHED
} GTEXT_Anim_Play_State;

/**
 * @struct GTEXT_Anim_Effect_Timing
 * @brief `EffectTiming`: the timing properties of an animation effect.
 *
 * A value: the easing is a pointer the caller keeps alive (NULL is `linear`). `iterations` and `duration` may be infinity (the IDL's `Infinity`); `duration_auto` is the string `auto`, which the
 * timing model reads as 0 and `getTiming()` returns as `auto`.
 */
typedef struct {
  double delay;                         ///< The start delay, in milliseconds (finite).
  double end_delay;                     ///< The end delay (finite).
  GTEXT_Anim_Fill fill;
  double iteration_start;               ///< The iteration start: a finite number, not negative.
  double iterations;                    ///< The iteration count: not negative, not NaN; infinity is allowed.
  bool duration_auto;                   ///< `duration` is `auto` (and `duration` is 0).
  double duration;                      ///< The iteration duration in milliseconds: not negative, not NaN; infinity is allowed.
  GTEXT_Anim_Direction direction;
  const GTEXT_CSS_Easing * easing;      ///< The effect easing function; NULL is `linear`. Not owned.
} GTEXT_Anim_Effect_Timing;

/** @name The members of an `OptionalEffectTiming`
 *  Bits of GTEXT_Anim_Optional_Timing::present: the members that "exist".
 *  @{ */
#define GTEXT_ANIM_TIMING_DELAY 0x01u
#define GTEXT_ANIM_TIMING_END_DELAY 0x02u
#define GTEXT_ANIM_TIMING_FILL 0x04u
#define GTEXT_ANIM_TIMING_ITERATION_START 0x08u
#define GTEXT_ANIM_TIMING_ITERATIONS 0x10u
#define GTEXT_ANIM_TIMING_DURATION 0x20u
#define GTEXT_ANIM_TIMING_DIRECTION 0x40u
#define GTEXT_ANIM_TIMING_EASING 0x80u
#define GTEXT_ANIM_TIMING_ALL 0xFFu
/** @} */

/** @struct GTEXT_Anim_Optional_Timing @brief `OptionalEffectTiming`: the members of `values` that exist are those whose bit of `present` is set. */
typedef struct {
  unsigned present;
  GTEXT_Anim_Effect_Timing values;
} GTEXT_Anim_Optional_Timing;

/**
 * @struct GTEXT_Anim_Computed_Timing
 * @brief `ComputedEffectTiming` as a value, with the intermediate results of the timing model.
 *
 * `timing` is the input as `getComputedTiming()` returns it: `fill` is never `AUTO` (it is `NONE`) and an `auto` duration is the number 0 (`duration_auto` false).
 */
typedef struct {
  GTEXT_Anim_Effect_Timing timing;
  double end_time;                      ///< `endTime`: "max(start delay + active duration + end delay, 0)".
  double active_duration;               ///< `activeDuration`: the iteration duration times the iteration count (0 when either is 0).
  GTEXT_Anim_Time local_time;           ///< `localTime`: the time given (`null` when unresolved).
  GTEXT_Anim_Phase phase;               ///< `IDLE` when the local time is unresolved.
  bool in_effect;                       ///< The active time is resolved: the effect applies a result.
  double active_time;                   ///< "Calculating the active time", when `in_effect`.
  double overall_progress;              ///< "Calculating the overall progress", when `in_effect`.
  double simple_progress;               ///< "Calculating the simple iteration progress", when `in_effect`.
  bool has_current_iteration;           ///< The current iteration index is not unresolved (`in_effect`).
  double current_iteration;             ///< `currentIteration`: infinity for the after phase of an infinite count.
  bool forwards;                        ///< The "current direction" is forwards.
  double directed_progress;             ///< "Calculating the directed progress", when `in_effect`.
  bool before_flag;                     ///< The before flag of the easing functions ("Calculating the transformed progress").
  bool has_progress;                    ///< `progress` is not `null` (`in_effect`).
  double progress;                      ///< `progress`: the transformed progress, the iteration progress that keyframes are indexed by.
} GTEXT_Anim_Computed_Timing;

/** @brief The defaults of `EffectTiming`: delay 0, endDelay 0, fill auto, iterationStart 0, iterations 1, duration auto, direction normal, easing linear (NULL). */
GTEXT_API GTEXT_Anim_Effect_Timing gtext_anim_timing_default(void);

/**
 * @brief The checks of "Updating the timing of an AnimationEffect" and of the IDL types of the members of `EffectTiming`.
 *
 * `iterationStart` less than zero, `iterations` less than zero or NaN, `duration` less than zero or NaN, and (the conversions of `double` in the IDL, which a binding makes before the call) a
 * `delay`, `endDelay` or `iterationStart` that is not finite, are `TypeError`s. An `easing` that cannot be parsed is gtext_anim_easing_parse()'s.
 * @return GTEXT_ANIM_OK; GTEXT_ANIM_TYPE_ERROR; GTEXT_ANIM_INVALID (NULL).
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_timing_validate(const GTEXT_Anim_Effect_Timing * timing);

/**
 * @brief "Update the timing properties of an animation effect": validate the members of `input` that exist, then assign each of them to `timing`. All or nothing: on a `TypeError` `timing` is as it was.
 *
 * The effect's finished state is the animation's (it is not here).
 * @param timing The timing to update. @param input The members that exist (GTEXT_ANIM_TIMING_*) and their values; an `easing` of an existing member is taken as it is (NULL: `linear`).
 * @return GTEXT_ANIM_OK; GTEXT_ANIM_TYPE_ERROR; GTEXT_ANIM_INVALID.
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_timing_update(GTEXT_Anim_Effect_Timing * timing, const GTEXT_Anim_Optional_Timing * input);

/**
 * @brief Read an `<easing-function>` from text, as the `easing` member of the dictionaries does ("cannot be parsed using the <easing-function> production" is a `TypeError`).
 *
 * @param registry The registry that has `transition-timing-function` (the document's). @param text The text. @param length Its length. @param allocator NULL for the default.
 * @param out Receives the easing function; release it with gtext_css_easing_free(). Written only on success.
 * @return GTEXT_ANIM_OK; GTEXT_ANIM_TYPE_ERROR (the text is not one easing function: a list, another keyword, a `steps(0)`); GTEXT_ANIM_INVALID; GTEXT_ANIM_OOM.
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_easing_parse(const GTEXT_CSS_Registry * registry, const char * text, size_t length, const GTEXT_Allocator * allocator, GTEXT_CSS_Easing ** out);

/**
 * @brief The timing model for one effect at one local time: `getComputedTiming()` and everything it is made of.
 *
 * @param timing The effect's timing; not validated here (a value a caller made without gtext_anim_timing_validate() gives a result for the arithmetic it has). Not NULL.
 * @param local_time The effect's local time (its animation's current time), or unresolved.
 * @param playback_rate The playback rate of the associated animation, which says the "animation direction" ("backwards" if it is less than zero); 1 for an effect with no animation.
 * @param endpoint_inclusive The "endpoint-inclusive active interval" flag of the phase calculation ("If not specified, it is assumed to be false").
 * @param out Receives the result. Not NULL.
 * @return GTEXT_ANIM_OK; GTEXT_ANIM_INVALID.
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_computed_timing(const GTEXT_Anim_Effect_Timing * timing, GTEXT_Anim_Time local_time, double playback_rate, bool endpoint_inclusive,
    GTEXT_Anim_Computed_Timing * out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_ANIMATION_TIMING_H
