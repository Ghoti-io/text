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
 * **Events and `CSSTransition`.** The library owns no clock and no event loop, so the events are a *frame* of the set: gtext_css_transition_set_frame() compares the phase each transition was in
 * the last time it was asked with the phase it is in at the time given, and reports what Transitions 2, "Event dispatch", says to dispatch: the transition phase (`before` while
 * the time is before the start time, `active` from the start time to the end time, `after` from the end time on; `idle` until the first frame that sees the transition, and after it is
 * cancelled), the table of changes ("idle -> before: transitionrun", "before -> after: transitionstart, transitionend", "not idle and not after -> idle: transitioncancel" and the rest) and its
 * elapsed times ("interval start" is `max(min(-start delay, active duration), 0)`, "interval end" the active duration, and the time a transitioncancel gives is the active time at the moment of
 * cancellation with a fill of both). Events come in the order of the composite order of the transitions of one element ("sort ... in ascending order by the Unicode codepoints that make up the
 * expanded transition property name", after the transition generation; a transitioncancel "use[s] the owning element and transition generation that were set immediately prior to canceling").
 * Cancellations are queued as they happen (a style change event cancels, replaces or reverses a transition; gtext_css_transition_set_cancel_all() is the element that is no longer being rendered or
 * is no longer in the document) and are reported by the next frame, which is what the table's last row is. A transition that is cancelled before any frame saw it reports nothing ("idle -> idle"); a
 * transition that completed reports the events up to transitionend even if no frame saw it running.
 *
 * `CSSTransition` is gtext_css_transition_set_animation_at(): a snapshot of a running transition as the Web Animations interface reads it at a time (`transitionProperty`, `playState`, `startTime`,
 * `currentTime`, `pending`, and the timing of its effect, `getTiming()` and `getComputedTiming()`, with the start and the end value that `getKeyframes()` gives). It is a value, because the library keeps
 * no objects: the caller that exposes an object holds it by the transition's `id`, which is the same in the snapshot and in the events, and knows the object finished (a transitionend) or was cancelled
 * (a transitioncancel) from the events, since a transition that left the set is not described by it any more. **Not here:** the methods of `Animation` that change a transition (`pause()`, `finish()`,
 * `reverse()`, `currentTime = ...`, `timeline = null`, the `ready` and `finished` promises): they are the Web Animations model, which the library does not implement; the events for a transition
 * that script has seeked are the same table, and a caller that has the model gives a frame the times it wants through gtext_css_transition_set_frame(). Shorthands are not transitioned (their
 * longhands are, one transition each).
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
  uint64_t id;                              ///< The transition's identity in its set: the same in the events and in the snapshot of gtext_css_transition_set_animation_at(); never reused by the set.
  uint64_t generation;                      ///< The "transition generation" (Transitions 2, "The current transition generation"): the number of the style change event that started it, for the set.
  GTEXT_CSS_String property_name;           ///< The "expanded transition property name": the longhand's name (the physical one for a flow-relative property) or the custom property's name.
  double delay_ms;                          ///< The start delay of the effect: the start time minus the time of the style change event (negative for a negative `transition-delay`, scaled by a reversal).
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

/** @brief The last transition id the set has given (0 before any). */
GTEXT_API uint64_t gtext_css_transition_set_last_id(const GTEXT_CSS_Transition_Set * set);

/** @brief Make the ids this set gives from now on greater than `id`, so that sets of one owner (the tracker's: an element forgotten and added again) never give one id to two transitions. */
GTEXT_API void gtext_css_transition_set_start_ids_after(GTEXT_CSS_Transition_Set * set, uint64_t id);

/**
 * @brief How many events the set dropped unreported. A set holds the cancellations and completions that the next frame owes, at most 4,096 of them; a caller that never takes frames (it has no use for
 * the events) loses the oldest past that, and this counts them. A caller that takes a frame between style change events loses none.
 */
GTEXT_API uint64_t gtext_css_transition_set_events_lost(const GTEXT_CSS_Transition_Set * set);

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

/** @brief The transition phase of Transitions 2, "Event dispatch": `idle` until a frame sees the transition, then where the time is relative to the effect (`before`, `active`, `after`). */
typedef enum {
  GTEXT_CSS_TRANSITION_PHASE_IDLE = 0,
  GTEXT_CSS_TRANSITION_PHASE_BEFORE,
  GTEXT_CSS_TRANSITION_PHASE_ACTIVE,
  GTEXT_CSS_TRANSITION_PHASE_AFTER
} GTEXT_CSS_Transition_Phase;

/** @brief The four transition events (Transitions 1, "Types of TransitionEvent"). */
typedef enum {
  GTEXT_CSS_TRANSITION_EVENT_RUN = 0,       ///< `transitionrun`
  GTEXT_CSS_TRANSITION_EVENT_START,         ///< `transitionstart`
  GTEXT_CSS_TRANSITION_EVENT_END,           ///< `transitionend`
  GTEXT_CSS_TRANSITION_EVENT_CANCEL         ///< `transitioncancel`
} GTEXT_CSS_Transition_Event_Type;

/** @struct GTEXT_CSS_Transition_Event @brief One event to dispatch: the members of `TransitionEvent` and the transition (`animation`) it is of. Valid until the list is appended to or freed. */
typedef struct {
  GTEXT_CSS_Transition_Event_Type type;
  uint64_t transition;                      ///< The transition's `id` (GTEXT_CSS_Transition_Info), the `animation` member of the event.
  uint64_t generation;                      ///< Its transition generation (the order of events of one element: generation, then property name).
  GTEXT_CSS_Property_Id property;           ///< The longhand, or 0 for a custom property.
  GTEXT_CSS_String property_name;           ///< `propertyName`: the name of the longhand, or the custom property's name.
  double elapsed_time;                      ///< `elapsedTime`, in seconds.
} GTEXT_CSS_Transition_Event;

/** @brief The events of one or more frames. Opaque; release with gtext_css_transition_events_free(). */
typedef struct GTEXT_CSS_Transition_Events GTEXT_CSS_Transition_Events;

/** @brief Make an empty list of events. @param allocator NULL for the default. @param out Receives it. @return GTEXT_CSS_OK, GTEXT_CSS_E_INVALID, GTEXT_CSS_E_OOM. */
GTEXT_API GTEXT_CSS_Status gtext_css_transition_events_new(const GTEXT_Allocator * allocator, GTEXT_CSS_Transition_Events ** out);

/** @brief Release a list of events; NULL is ignored. */
GTEXT_API void gtext_css_transition_events_free(GTEXT_CSS_Transition_Events * events);

/** @brief How many events. NULL is 0. */
GTEXT_API size_t gtext_css_transition_events_count(const GTEXT_CSS_Transition_Events * events);

/** @brief Event `index`, in the order to dispatch them. @return false past the end. */
GTEXT_API bool gtext_css_transition_events_at(const GTEXT_CSS_Transition_Events * events, size_t index, GTEXT_CSS_Transition_Event * out);

/**
 * @brief The most events (and the most bytes of names) that a frame of the set can add: three events (run, start, end) for each running transition and for each cancelled or completed one that is
 * waiting to be reported. A caller that takes the frames of many sets into one list reserves the sum first, and then no frame can fail.
 * @param set The set (NULL is 0 and 0). @param events Receives the most events; may be NULL. @param name_bytes Receives the most bytes of names; may be NULL.
 */
GTEXT_API void gtext_css_transition_set_frame_bound(const GTEXT_CSS_Transition_Set * set, size_t * events, size_t * name_bytes);

/** @brief Make room in a list for more events and name bytes, so that appending them cannot fail. @return GTEXT_CSS_OK, GTEXT_CSS_E_INVALID, GTEXT_CSS_E_OOM (the list is as it was). */
GTEXT_API GTEXT_CSS_Status gtext_css_transition_events_reserve(GTEXT_CSS_Transition_Events * events, size_t more_events, size_t more_name_bytes);

/**
 * @brief One animation frame of a set: the events that moving each transition from the phase it was last seen in to its phase at `time_ms` dispatches, then those of the transitions that were
 * cancelled or completed since the last frame. They are appended to `events` in the composite order of the set (Transitions 2, "Animation composite order"), and the set's record of each transition's
 * phase moves on, so that the same time asked again reports nothing. A transition's first frame also makes it ready (its `pending` ends).
 *
 * All or nothing: on a failure the set and the list are as they were.
 *
 * @param set The set. @param time_ms The time of the frame; the transitions' times are the ones of the style change events (the same clock). @param events The list to append to. Not NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_transition_set_frame(GTEXT_CSS_Transition_Set * set, double time_ms, GTEXT_CSS_Transition_Events * events);

/**
 * @brief Cancel every running transition and remove every completed one at a time ("implementations must also cancel any running transitions on elements that are no longer being rendered and remove
 * transitions on them from the set of completed transitions", Transitions 2, "Starting of transitions"; and for an element that is no longer in the document, Transitions 1). Each cancelled transition that a
 * frame had seen is reported by the next frame as a transitioncancel; gtext_css_transition_set_clear() is the same without the events (the set of a thing that is gone and has nothing to tell).
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM (the set is as it was).
 */
GTEXT_API GTEXT_CSS_Status gtext_css_transition_set_cancel_all(GTEXT_CSS_Transition_Set * set, double time_ms);

/** @brief `Animation.playState` of a transition: it is `running` until it ends (a transition that has not been seen by a frame is pending, which is `running`) and `finished` from its end time; a transition that left the set is `idle`. */
typedef enum {
  GTEXT_CSS_TRANSITION_PLAY_IDLE = 0,
  GTEXT_CSS_TRANSITION_PLAY_RUNNING,
  GTEXT_CSS_TRANSITION_PLAY_FINISHED
} GTEXT_CSS_Transition_Play_State;

/**
 * @struct GTEXT_CSS_Transition_Animation
 * @brief A `CSSTransition` as the Web Animations interface reads it at a time. Strings are valid until the set is updated, cleared, cancelled, or freed.
 *
 * `start_time_ms` is the time of the style change event (the animation's start: its effect begins `delay_ms` later; it is meaningful only when `start_time_resolved`: until then `startTime` is null), and is unresolved (`start_time_resolved` false, `pending` true) until the first
 * frame after the transition was made ("a start time ... resolved when the animation is ready"); `current_time_ms` is the time since then (0 while pending). The effect's timing is that of the
 * transition: `fill: "backwards"`, one iteration, `direction: "normal"`, `endDelay` 0, the matching `transition-timing-function` as `easing`; its computed timing is `active_duration_ms` (the duration),
 * `end_time_ms` (delay plus duration) and, when the effect is not before it or after it with no fill, `progress` (the output of the timing function at the effect's progress) and `current_iteration` 0.
 */
typedef struct {
  uint64_t id;
  uint64_t generation;
  GTEXT_CSS_String transition_property;     ///< `transitionProperty`: the expanded transition property name.
  GTEXT_CSS_Transition_Play_State play_state;
  GTEXT_CSS_Transition_Phase phase;         ///< The phase of the effect at its local time (`before`, `active` or `after`; never idle).
  bool pending;
  bool start_time_resolved;
  double start_time_ms;
  double current_time_ms;
  double delay_ms;                          ///< `getTiming().delay`
  double end_delay_ms;                      ///< `getTiming().endDelay`: 0.
  double duration_ms;                       ///< `getTiming().duration`
  GTEXT_CSS_String easing;                  ///< `getTiming().easing`
  double active_duration_ms;                ///< `getComputedTiming().activeDuration`
  double end_time_ms;                       ///< `getComputedTiming().endTime`
  double local_time_ms;                     ///< `getComputedTiming().localTime`: the current time (the animation has no hold).
  bool has_progress;                        ///< `getComputedTiming().progress` is not null.
  double progress;                          ///< The transformed progress, when `has_progress`.
  GTEXT_CSS_String start_value;             ///< The offset 0 keyframe's value (serialized computed value).
  GTEXT_CSS_String end_value;               ///< The offset 1 keyframe's value.
} GTEXT_CSS_Transition_Animation;

/**
 * @brief The `CSSTransition` of running transition `index` at a time.
 * @param context The compute context (css_computed.h): its registry reads the timing function. @param set The set. @param index A running transition. @param time_ms The time. @param out Receives it.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (no such transition); GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_css_transition_set_animation_at(const GTEXT_CSS_Compute_Context * context, const GTEXT_CSS_Transition_Set * set, size_t index, double time_ms,
    GTEXT_CSS_Transition_Animation * out);

/**
 * @brief Whether a CSS Animation of the element is applying the property in this style (the style was computed with animations sampled, and the property was one of the values they gave).
 */
GTEXT_API bool gtext_css_computed_animation_applied(const GTEXT_CSS_Computed_Style * style, GTEXT_CSS_Property_Id property);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_CSS_CSS_TRANSITION_H
