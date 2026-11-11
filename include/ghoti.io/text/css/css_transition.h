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
 * @file css_transition.h
 * @brief CSS Transitions: the running and the completed transitions of an element, started by a change of its computed values and sampled into its style.
 *
 * Normative text: CSS Transitions 1 ("Starting of transitions", "Faster reversing of interrupted transitions", "Application of transitions", "Completion of transitions") and CSS
 * Transitions 2 (`transition-behavior`: "allow-discrete"), as pinned in tools/css/DRAFTS_SHA256.
 *
 * **Who keeps what.** The specification says "Implementations must maintain a set of running transitions, each of which applies to a specific element and non-shorthand property" and a set of
 * completed ones. The library keeps nothing between calls: the set is a value (GTEXT_CSS_Transition_Set) that the caller owns, one for each element and each of its pseudo-elements, and the
 * caller says when time passes. A **style change event** of the specification is one call of gtext_css_transitions_update() per element, with
 *
 *  - the *before-change style*: the computed style the caller kept from the previous style change event, and
 *  - the *after-change style*: the computed style of the element now, computed **without** transitions (GTEXT_CSS_Style_Options::transitions NULL) and so inheriting from the after-change
 *    style of the parent (the caller computes the tree top down once for these and once, with the sets, for the styles it shows),
 *  - the time of the event.
 *
 * The update starts, reverses, cancels and completes the element's transitions in the set. The style the caller shows is computed with the set (GTEXT_CSS_Style_Options::transitions) at a time:
 * each running transition adds its *current value* to the cascade at the transition origin, above every other origin, unless a CSS Animation is applying the same property to the element.
 *
 * A property that a CSS Animation of the element is applying (in the before-change or the after-change style) does not start a transition: "transitions do not start when the computed value of a
 * property changes as a result of declarative animation". (The specification gets this by building the after-change style from the *before-change* `animation-*` values; the result is the
 * same for the properties an animation applies in both styles, and this API asks the styles which those are.)
 *
 * **Custom properties.** "When referenced by animations and transitions, custom property values interpolate by computed value, in accordance with the type that they parsed as" (CSS Properties and
 * Values API 1, "Animation Behavior"), and "For custom properties registered ... the animation type is by computed value, derived from the type used in the property's syntax definition. Where there is
 * no computed value type that corresponds to the property's specified syntax (e.g. when the syntax is the universal syntax definition) or when the custom property is not registered, the animation type
 * is discrete" (Web Animations 1, "Custom Properties"). So a custom property transitions when a `transition-property` item names it (`all` names the registered ones with a syntax other than the universal
 * one), by its registered syntax, and a pair of values that has no computed value type to interpolate (an unregistered property, a universal one, a `<custom-ident>`, lists of different lengths) transitions
 * only with `transition-behavior: allow-discrete` ("the property values are transitionable if they have an animation type that is neither not animatable nor discrete", Transitions 1 and 2). The
 * guaranteed-invalid value (the value of an unregistered custom property nothing sets) has no text of its own: a transition from or to it is discrete, and the value is written `initial`, the declaration that gives it. A transition of a custom property has `property`
 * 0 and its name in `custom_name`.
 *
 * **Not here:** the events (transitionrun, transitionstart, transitionend, transitioncancel) and `CSSTransition`; the update reports counts, and the caller that dispatches events can read the
 * set before and after. Shorthands are not transitioned (their longhands are, one transition each).
 */

#ifndef GHOTI_IO_GTEXT_CSS_CSS_TRANSITION_H
#define GHOTI_IO_GTEXT_CSS_CSS_TRANSITION_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/css/css_computed.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_function.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The running and completed transitions of one element (or one pseudo-element). Opaque; one owner; not shared between threads. */
typedef struct GTEXT_CSS_Transition_Set GTEXT_CSS_Transition_Set;

/** @struct GTEXT_CSS_Transition_Info @brief A running transition, valid until the set is updated or freed. */
typedef struct {
  GTEXT_CSS_Property_Id property;           ///< The (non-shorthand) property, or 0 for a custom property.
  GTEXT_CSS_String custom_name;             ///< The custom property's name (`--name`, as written), or empty for a longhand.
  GTEXT_CSS_String start_value;             ///< The "start value": the computed value, serialized.
  GTEXT_CSS_String end_value;               ///< The "end value".
  GTEXT_CSS_String reversing_adjusted_start_value;
  double start_time_ms;                     ///< The "start time": the time of the style change event plus the matching delay.
  double end_time_ms;                       ///< The "end time": the start time plus the matching duration (times the shortening factor for a reversal).
  double reversing_shortening_factor;       ///< 1, or less after a reversal.
  GTEXT_CSS_String timing_function;         ///< The matching `transition-timing-function`, serialized.
  bool allow_discrete;                      ///< `transition-behavior: allow-discrete` at the start.
} GTEXT_CSS_Transition_Info;

/** @brief What one update did. */
typedef struct {
  size_t started;     ///< Transitions started (a reversal, or a retarget, counts: it cancels one and starts one).
  size_t cancelled;   ///< Running transitions cancelled (ones replaced by a new one included).
  size_t completed;   ///< Running transitions that completed (moved to the completed ones).
  size_t forgotten;   ///< Completed transitions removed from the completed ones.
} GTEXT_CSS_Transition_Update;

/** @brief Make an empty set. @param allocator NULL for the default. @param out Receives it. @return GTEXT_CSS_OK, GTEXT_CSS_E_INVALID, GTEXT_CSS_E_OOM. */
GTEXT_API GTEXT_CSS_Status gtext_css_transition_set_new(const GTEXT_Allocator * allocator, GTEXT_CSS_Transition_Set ** out);

/** @brief Release a set; NULL is ignored. */
GTEXT_API void gtext_css_transition_set_free(GTEXT_CSS_Transition_Set * set);

/** @brief Empty a set: every transition is cancelled and forgotten ("If an element is no longer in the document, implementations must cancel any running transitions on it and remove transitions on it from the completed transitions"). */
GTEXT_API void gtext_css_transition_set_clear(GTEXT_CSS_Transition_Set * set);

/** @brief A number that changes whenever the running or the completed transitions do (a cache key for a style computed with the set). NULL is 0. */
GTEXT_API uint64_t gtext_css_transition_set_version(const GTEXT_CSS_Transition_Set * set);

/** @brief How many transitions are running. NULL is 0. */
GTEXT_API size_t gtext_css_transition_set_running_count(const GTEXT_CSS_Transition_Set * set);

/** @brief Running transition `index` (in the order they were started). @return false past the end. */
GTEXT_API bool gtext_css_transition_set_running_at(const GTEXT_CSS_Transition_Set * set, size_t index, GTEXT_CSS_Transition_Info * out);

/** @brief How many transitions are completed. NULL is 0. */
GTEXT_API size_t gtext_css_transition_set_completed_count(const GTEXT_CSS_Transition_Set * set);

/** @brief Completed transition `index`: its property (0 for a custom property, whose name is `custom_name`) and its end value (what a later change is compared with). Either out parameter may be NULL. @return false past the end. */
GTEXT_API bool gtext_css_transition_set_completed_at(const GTEXT_CSS_Transition_Set * set, size_t index, GTEXT_CSS_Property_Id * property, GTEXT_CSS_String * custom_name, GTEXT_CSS_String * end_value);

/**
 * @brief The current value of a running transition at a time, serialized as the declaration `property: <text>` would say it.
 *
 * Before the start time (the delay phase) it is the start value; after it, the interpolation of the start and the end value by the output of the timing function at
 * (time - start time) / (end time - start time).
 *
 * @param context The compute context (css_computed.h). @param style A style of the element, for its `color` (a `currentcolor` among the values is the element's color).
 * @param set The set. @param index A running transition. @param time_ms The time. @param allocator NULL for the context's.
 * @param text Receives the text; release with gtext_css_serialize_buffer_free(). @param length Receives its length.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (no such transition); GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_transition_current_value(const GTEXT_CSS_Compute_Context * context, const GTEXT_CSS_Computed_Style * style, const GTEXT_CSS_Transition_Set * set,
    size_t index, double time_ms, const GTEXT_Allocator * allocator, char ** text, size_t * length);

/**
 * @brief The "Starting of transitions" of one style change event for one element.
 *
 * Running transitions whose end time is at or before `time_ms` complete first (they become completed transitions). Then, for each property: a change from the before-change to the
 * after-change value starts a transition if the values are transitionable and the after-change style has a matching `transition-property` value whose duration plus delay is greater than 0s
 * (and the element has no running transition or completed transition for it with this end value); a running transition whose end value is not the after-change value is cancelled, reversed
 * (when its reversing-adjusted start value is the after-change value) or replaced; a running or completed transition with no matching `transition-property` value is cancelled or forgotten.
 *
 * All or nothing: on a failure the set is as it was.
 *
 * @param context The compute context both styles were made in. Not NULL.
 * @param set The element's set. Not NULL.
 * @param before The before-change style, or NULL when the element was not in the document at the previous style change event (then no transition starts).
 * @param after The after-change style (computed without transitions). Not NULL.
 * @param time_ms The time of the style change event.
 * @param out Receives what was done; may be NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (a NULL argument, styles of another context); GTEXT_CSS_E_UNSUPPORTED (the registry has no `transition-property`); GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_transitions_update(const GTEXT_CSS_Compute_Context * context, GTEXT_CSS_Transition_Set * set, const GTEXT_CSS_Computed_Style * before,
    const GTEXT_CSS_Computed_Style * after, double time_ms, GTEXT_CSS_Transition_Update * out);

/**
 * @brief Whether a CSS Animation of the element is applying the property in this style (the style was computed with animations sampled, and the property was one of the values they gave).
 */
GTEXT_API bool gtext_css_computed_animation_applied(const GTEXT_CSS_Computed_Style * style, GTEXT_CSS_Property_Id property);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_TRANSITION_H
