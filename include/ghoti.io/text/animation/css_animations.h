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
 * @file css_animations.h
 * @brief The CSS Animations of one element (or pseudo-element) as `CSSAnimation` objects of Web Animations: made, updated and cancelled by the style change events, read as a snapshot, and
 * reporting the events of CSS Animations 2, "Event dispatch".
 *
 * Normative text: CSS Animations 2 ("Animations": the correspondence of the `animation-*` properties with the Web Animations concepts, and what a change of the API does to it; "Owning element";
 * "Animation composite order"; "The 'animation-play-state' property"; "Event dispatch": the table of phase changes and the elapsed times), and Web Animations 1 ("Animation effect phases and
 * states", "Play states", "Relevant animations", "Animation events"), as pinned in tools/css/DRAFTS_SHA256. The timing of each animation is the model of timing.h.
 *
 * **Who keeps what.** As the set of transitions (css_transition.h), the library keeps nothing between calls: the set is a value, one for each element and each of its pseudo-elements, that the caller
 * owns and gives the **style change event** (gtext_anim_css_set_update(): the effects of the after-change style, the `@keyframes` that exist, the time) and the **frame** (gtext_anim_css_set_frame():
 * the events the phase changes owe). The time is always an argument. The tracker of the CSSOM (css_cssom_computed.h) is the reference caller.
 *
 * **The state machine of a CSS animation** is the part of the Animation of Web Animations that the style change events reach: an animation is made *pending* (a play task, or a pause task for
 * `animation-play-state: paused`), becomes ready at the next frame (its start time is the time of the style change event that played it, less the time it was held at: the same simplification as
 * the transitions', whose start time is the style change event's), is paused by the property (its current time is held and its start time unresolved) and played again by it, and
 * is cancelled when its name leaves the `animation-name` list or no `@keyframes` has the name. Pending tasks and holds are the model of the pinned text; what changes them from the API
 * (`play()`, `pause()`, `currentTime = ...`) is not here.
 */

#ifndef GHOTI_IO_GTEXT_ANIMATION_CSS_ANIMATIONS_H
#define GHOTI_IO_GTEXT_ANIMATION_CSS_ANIMATIONS_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/animation/animation.h>
#include <ghoti.io/text/animation/events.h>
#include <ghoti.io/text/animation/timing.h>
#include <ghoti.io/text/css/css_animation.h>
#include <ghoti.io/text/css/css_atrule_set.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/css/css_registry.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The CSS Animations of one element or pseudo-element. Opaque; one owner; not shared between threads. */
typedef struct GTEXT_Anim_Css_Set GTEXT_Anim_Css_Set;

/** @brief What one style change event did. */
typedef struct {
  size_t started;     ///< Animations made.
  size_t updated;     ///< Animations whose timing, order or play state changed.
  size_t cancelled;   ///< Animations cancelled (their name left the list, or no `@keyframes` has it).
} GTEXT_Anim_Css_Update;

/**
 * @struct GTEXT_Anim_Css_Info
 * @brief A `CSSAnimation` as the Web Animations interface reads it at a time. Strings and the easing are valid until the set is updated, cleared, or freed.
 */
typedef struct {
  uint64_t id;                          ///< The animation's identity in its set: the same in the events and in the snapshot; never reused by the set.
  GTEXT_CSS_String animation_name;      ///< `animationName`.
  size_t order;                         ///< Its place in the computed `animation-name` list: the composite order of the animations of one element.
  GTEXT_Anim_Effect_Timing timing;      ///< `getTiming()`: the easing is the set's.
  GTEXT_CSS_String easing;              ///< `getTiming().easing`, serialized.
  GTEXT_Anim_Play_State play_state;     ///< `playState`.
  bool pending;                         ///< `pending`: a play or pause task is waiting for the next frame.
  GTEXT_Anim_Time start_time;           ///< `startTime`: unresolved while pending a play and while paused.
  GTEXT_Anim_Time current_time;         ///< `currentTime`.
  bool relevant;                        ///< "Relevant animations": its effect is current or in effect (what `getAnimations()` returns).
  GTEXT_Anim_Computed_Timing computed;  ///< `effect.getComputedTiming()` at the time.
  bool adopted;                         ///< Script has touched it (gtext_anim_css_set_bind()): the Animation holds its timing state, and `animation` is that state.
  GTEXT_Anim_Animation_State animation; ///< The state of the Animation when `adopted`: the playback rate, the replace state, the generations and states of `ready` and `finished`.
} GTEXT_Anim_Css_Info;

/** @brief Make an empty set. @param allocator NULL for the default. @return GTEXT_CSS_OK, GTEXT_CSS_E_INVALID, GTEXT_CSS_E_OOM. */
GTEXT_API GTEXT_CSS_Status gtext_anim_css_set_new(const GTEXT_Allocator * allocator, GTEXT_Anim_Css_Set ** out);

/** @brief Release a set; NULL is ignored. */
GTEXT_API void gtext_anim_css_set_free(GTEXT_Anim_Css_Set * set);

/** @brief Empty a set: every animation is forgotten, with no events (the set of a thing that is gone and has nothing to tell). */
GTEXT_API void gtext_anim_css_set_clear(GTEXT_Anim_Css_Set * set);

/** @brief A number that changes whenever the animations, or what a read shows of them, do (a cache key). NULL is 0. */
GTEXT_API uint64_t gtext_anim_css_set_version(const GTEXT_Anim_Css_Set * set);

/** @brief The last animation id the set has given (0 before any). */
GTEXT_API uint64_t gtext_anim_css_set_last_id(const GTEXT_Anim_Css_Set * set);

/** @brief Make the ids this set gives from now on greater than `id`, so that sets of one owner never give one id to two animations. */
GTEXT_API void gtext_anim_css_set_start_ids_after(GTEXT_Anim_Css_Set * set, uint64_t id);

/** @brief How many animations the set holds (relevant or not). NULL is 0. */
GTEXT_API size_t gtext_anim_css_set_count(const GTEXT_Anim_Css_Set * set);

/**
 * @brief How many events the set dropped unreported: a set holds the cancellations that the next frame owes, at most 4,096 of them; a caller that never takes frames loses the oldest past that, and
 * this counts them.
 */
GTEXT_API uint64_t gtext_anim_css_set_events_lost(const GTEXT_Anim_Css_Set * set);

/**
 * @brief One style change event for one element: "update animations based on the computed values of the `animation-*` properties" (CSS Animations 2, "Animations" and "Owning element").
 *
 * Each effect of the list that uses the document timeline and has a `@keyframes` rule of its name is an animation: a new one is made (pending), an existing one of the same name (the n-th
 * of that name for the n-th effect of it) has its timing updated from the effect (except the members that `gtext_anim_css_set_update_timing()` has taken over), its place in the list
 * recorded, and is paused or played when `animation-play-state` is newly `paused` or `running`; an animation that no effect matches is cancelled (and, if a frame had seen it begin and not end,
 * reports an `animationcancel` at the next frame).
 *
 * All or nothing: on a failure the set is as it was.
 *
 * @param set The element's set. @param list The effects of the after-change style (gtext_css_anim_list_from_style()), or NULL for none. @param keyframes The `@keyframes` rules (their
 * names decide whether an effect is an animation), or NULL for none. @param time_ms The time of the style change event. @param out What was done; may be NULL.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_anim_css_set_update(GTEXT_Anim_Css_Set * set, const GTEXT_CSS_Anim_List * list, const GTEXT_CSS_Atrule_Set * keyframes, double time_ms,
    GTEXT_Anim_Css_Update * out);

/**
 * @brief The element is no longer being rendered or in the document: every animation is cancelled ("terminate running animations with this owning element"), as gtext_anim_css_set_update() with
 * no effects does, and the next frame reports an `animationcancel` for each that a frame had seen begin and not end.
 */
GTEXT_API GTEXT_CSS_Status gtext_anim_css_set_cancel_all(GTEXT_Anim_Css_Set * set, double time_ms);

/** @brief The `CSSAnimation` of animation `index` (in the composite order: the order of the `animation-name` list) at a time. @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID (no such animation); GTEXT_CSS_E_OOM. */
GTEXT_API GTEXT_CSS_Status gtext_anim_css_set_info(const GTEXT_Anim_Css_Set * set, size_t index, double time_ms, GTEXT_Anim_Css_Info * out);

/** @brief The current time at a time of the animation named `name` that has place `order` in the `animation-name` list, for the sampler of a style (`animation_time` of the layout interface). @return false: no such animation, or its current time is unresolved. */
GTEXT_API bool gtext_anim_css_set_current_time(const GTEXT_Anim_Css_Set * set, const GTEXT_CSS_String * name, size_t order, double time_ms, double * ms);

/**
 * @brief `AnimationEffect.updateTiming()` of the effect of animation `id`: the checks and the delta update of timing.h, and then "any subsequent change to a corresponding animation property will not
 * be reflected in that animation" for each member `input` has.
 *
 * @param registry The registry that parses the easing (it has `transition-timing-function`). @param input The members that exist; the `easing` of the struct is ignored and `easing_text` is
 * the member. @param easing_text The `easing` member as text, when GTEXT_ANIM_TIMING_EASING is set (a TypeError when it does not parse). @param easing_length Its length.
 * @return GTEXT_ANIM_OK; GTEXT_ANIM_TYPE_ERROR (nothing changed); GTEXT_ANIM_INVALID (no animation of that id); GTEXT_ANIM_OOM.
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_css_set_update_timing(GTEXT_Anim_Css_Set * set, const GTEXT_CSS_Registry * registry, uint64_t id, const GTEXT_Anim_Optional_Timing * input,
    const char * easing_text, size_t easing_length);

/**
 * @brief Give animation `id` an Animation to hold its timing state from now on (the first time script touches it): the Animation gets the effect's timing and the state the record had (start and
 * hold time, a pending task), and the record reads the current time, the pending task and the finished state from it. A change the style makes later does what the specification says it does to
 * an animation (`animation-play-state` pauses it or plays it again, a changed timing is the effect's new timing, a name that leaves the list cancels it). The Animation is borrowed: the owner keeps it
 * until gtext_anim_css_set_detach(). @return GTEXT_ANIM_OK; GTEXT_ANIM_INVALID (no such animation, or it has one).
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_css_set_bind(GTEXT_Anim_Css_Set * set, uint64_t id, GTEXT_Anim_Animation * animation, double time_ms);

/** @brief The record of animation `id` lets go of its Animation (the owner is about to free it); a no-op for an animation that has none. */
GTEXT_API void gtext_anim_css_set_detach(GTEXT_Anim_Css_Set * set, uint64_t id);

/**
 * @brief The most events (and name bytes) a frame of the set can add, so that a caller that takes the frames of many sets into one list reserves the sum first and no frame can fail.
 */
GTEXT_API void gtext_anim_css_set_frame_bound(const GTEXT_Anim_Css_Set * set, size_t * events, size_t * name_bytes);

/**
 * @brief One animation frame of the set at a time: the animations with a pending task become ready; then the events that moving each animation from the phase it was last seen in to its phase now
 * dispatches (CSS Animations 2, "Event dispatch": `animationstart`, `animationiteration`, `animationend`, `animationcancel`), then those of the animations that were cancelled since the last frame. They
 * are appended to `events` in the composite order of the set; the set's record of each animation's phase moves on, so that the same time asked again reports nothing.
 *
 * "Each time a new animation frame is established and the animation does not have a pending play task or pending pause task": an animation made after the last frame is made ready by this frame and
 * reports the events of its phase at this frame (as the first frame of a transition does).
 *
 * @param events The list to append to; the `element` of each event is NULL and the pseudo-element name empty (the owner of the set fills them). All or nothing.
 * @return GTEXT_CSS_OK; GTEXT_CSS_E_INVALID; GTEXT_CSS_E_OOM.
 */
GTEXT_API GTEXT_CSS_Status gtext_anim_css_set_frame(GTEXT_Anim_Css_Set * set, double time_ms, GTEXT_Anim_Events * events);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_ANIMATION_CSS_ANIMATIONS_H
