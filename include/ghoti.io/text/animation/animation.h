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
 * @file animation.h
 * @brief The `Animation` of Web Animations 1 as a state machine: play state, pending tasks, start time, hold time, playback rate, `play()`, `pause()`, `finish()`, `cancel()`, `reverse()`,
 * `updatePlaybackRate()`, seeking, `persist()`, and the `ready` and `finished` promises and the playback events as state the host drains.
 *
 * Normative text: Web Animations 1, "Animation model" ("Animations": "Calculating the current time of an animation", "Setting the timeline of an animation", "Setting the associated effect of an
 * animation", "Setting the current time of an animation", "Setting the start time of an animation", "The current ready promise", "Playing an animation", "Pausing an animation", "The current
 * finished promise", "Updating the finished state", "Finishing an animation", "Canceling an animation", "Speed control", "Reversing an animation", "Play states", "Animation events",
 * "Animation playback events", "Removing replaced animations"), as pinned in tools/css/DRAFTS_SHA256. Each procedure is named in the code by its section title.
 *
 * **No clock, no promise, no task queue.** An animation never reads a time: each call takes `now`, the current time of its timeline (unresolved for an inactive one; ignored by an animation
 * with no timeline). Everything the specification does "as soon as the animation is ready" (the pending play and pause tasks) is a flag that gtext_anim_animation_run_pending() runs at a time
 * the host chooses (the first resolved tick after the call, which is "as soon as ready" for an animation the user agent has nothing to set up for); everything it does "in a microtask" is a flag
 * that gtext_anim_animation_checkpoint() runs. A promise is a *generation* and a state: the generation goes up when the specification makes a new promise, and the notes drained with
 * gtext_anim_animation_take_notes() say which generation settles when, so a binding keeps one promise for each generation. The events (`finish`, `cancel`, `remove`) are appended to the queue
 * gtext_anim_animation_take_events() drains, with their scheduled event time, in the form the events of the CSS animations have (events.h), so that one list holds a frame.
 *
 * **Not here.** The associated effect is its timing only (the keyframes and the effect stack are Stage W3); the timelines are "none", the document timeline and, for the branches of the
 * procedures that depend on one that is not monotonically increasing, a progress-based timeline whose current time the host gives. Moving an animation from one kind of timeline to another
 * where one is progress-based is Web Animations 2's changed procedure ("Setting the timeline of an animation": auto-aligned start times) and is not supported here (GTEXT_ANIM_NOT_SUPPORTED).
 * An animation has no identity beyond its address; the binding keeps the identity of its object.
 */

#ifndef GHOTI_IO_GTEXT_ANIMATION_ANIMATION_H
#define GHOTI_IO_GTEXT_ANIMATION_ANIMATION_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/animation/events.h>
#include <ghoti.io/text/animation/timing.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The kind of timeline an animation is associated with. */
typedef enum {
  GTEXT_ANIM_TIMELINE_NONE = 0,   ///< No timeline: the animation's current time is held, or unresolved.
  GTEXT_ANIM_TIMELINE_DOCUMENT,   ///< A document timeline: monotonically increasing, associated with a document.
  GTEXT_ANIM_TIMELINE_PROGRESS    ///< A progress-based timeline (scroll, view): not monotonically increasing, "has finite timeline" in the procedures.
} GTEXT_Anim_Timeline_Kind;

/** @brief The replace state of an animation ("Animation playback events"). */
typedef enum {
  GTEXT_ANIM_REPLACE_ACTIVE = 0,
  GTEXT_ANIM_REPLACE_REMOVED,
  GTEXT_ANIM_REPLACE_PERSISTED
} GTEXT_Anim_Replace_State;

/** @brief The state of a promise the host keeps for a generation. */
typedef enum {
  GTEXT_ANIM_PROMISE_PENDING = 0,
  GTEXT_ANIM_PROMISE_RESOLVED,
  GTEXT_ANIM_PROMISE_REJECTED     ///< Rejected with an `AbortError` (the only rejection the model makes).
} GTEXT_Anim_Promise_State;

/** @brief What a note says happened to a promise. The animation is the value a promise resolves with. */
typedef enum {
  GTEXT_ANIM_NOTE_READY_RESOLVE = 0,
  GTEXT_ANIM_NOTE_READY_REJECT,
  GTEXT_ANIM_NOTE_FINISHED_RESOLVE,
  GTEXT_ANIM_NOTE_FINISHED_REJECT
} GTEXT_Anim_Note_Kind;

/** @brief One promise settlement owed to the host, in the order the specification makes them. */
typedef struct {
  GTEXT_Anim_Note_Kind kind;
  uint32_t generation;                  ///< The generation of the promise (`ready_generation` or `finished_generation` of the state at the moment it was current).
} GTEXT_Anim_Note;

/** @brief An animation. Opaque; one owner; not shared between threads. */
typedef struct GTEXT_Anim_Animation GTEXT_Anim_Animation;

/**
 * @struct GTEXT_Anim_Animation_State
 * @brief What `Animation` reads at a time.
 */
typedef struct {
  GTEXT_Anim_Time current_time;         ///< `currentTime`.
  GTEXT_Anim_Time start_time;           ///< `startTime`.
  GTEXT_Anim_Time hold_time;            ///< The hold time (not an IDL member).
  GTEXT_Anim_Play_State play_state;     ///< `playState`.
  GTEXT_Anim_Replace_State replace_state; ///< `replaceState`.
  bool pending;                         ///< `pending`: a play or pause task is waiting.
  bool pending_play;                    ///< A pending play task.
  bool pending_pause;                   ///< A pending pause task.
  double playback_rate;                 ///< `playbackRate`.
  bool has_pending_rate;                ///< A pending playback rate is set.
  double pending_rate;                  ///< The pending playback rate, when set.
  double effective_rate;                ///< The effective playback rate: the pending one if set.
  double effect_end;                    ///< The associated effect end: the end time of the effect, 0 with no effect (infinity allowed).
  bool has_effect;
  GTEXT_Anim_Timeline_Kind timeline;
  uint32_t ready_generation;            ///< The generation of the current ready promise; goes up when the specification replaces it.
  GTEXT_Anim_Promise_State ready;       ///< The state of the current ready promise.
  uint32_t finished_generation;         ///< The generation of the current finished promise.
  GTEXT_Anim_Promise_State finished;    ///< The state of the current finished promise.
} GTEXT_Anim_Animation_State;

/**
 * @brief Make an animation: idle, with no effect, playback rate 1, a resolved ready promise (generation 1) and a pending finished promise (generation 1).
 * @param timeline The timeline it is associated with. @return GTEXT_ANIM_OK; GTEXT_ANIM_INVALID; GTEXT_ANIM_OOM.
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_animation_new(const GTEXT_Allocator * allocator, GTEXT_Anim_Timeline_Kind timeline, GTEXT_Anim_Animation ** out);

/** @brief Release an animation; NULL is ignored. */
GTEXT_API void gtext_anim_animation_free(GTEXT_Anim_Animation * animation);

/** @brief "Set the timeline of an animation" (to `kind`). @return GTEXT_ANIM_OK; GTEXT_ANIM_NOT_SUPPORTED (to or from a progress-based timeline from another kind); GTEXT_ANIM_INVALID. */
GTEXT_API GTEXT_Anim_Status gtext_anim_animation_set_timeline(GTEXT_Anim_Animation * animation, GTEXT_Anim_Timeline_Kind kind, GTEXT_Anim_Time now);

/**
 * @brief "Set the associated effect of an animation": the timing of the new effect, or NULL for none. The easing of the timing is not owned and must outlive the animation.
 * @return GTEXT_ANIM_OK; GTEXT_ANIM_TYPE_ERROR (the timing is invalid); GTEXT_ANIM_INVALID.
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_animation_set_effect(GTEXT_Anim_Animation * animation, const GTEXT_Anim_Effect_Timing * timing, GTEXT_Anim_Time now);

/**
 * @brief `effect.updateTiming()`: the delta update of timing.h on the animation's effect, then "update an animation's finished state" (did seek false, notify false).
 * @return GTEXT_ANIM_OK; GTEXT_ANIM_TYPE_ERROR (nothing changed); GTEXT_ANIM_INVALID (no effect).
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_animation_update_effect_timing(GTEXT_Anim_Animation * animation, const GTEXT_Anim_Optional_Timing * input, GTEXT_Anim_Time now);

/** @brief The state at a time. */
GTEXT_API GTEXT_Anim_Status gtext_anim_animation_state(const GTEXT_Anim_Animation * animation, GTEXT_Anim_Time now, GTEXT_Anim_Animation_State * out);

/** @brief `currentTime = seek` ("set the current time"). @return GTEXT_ANIM_OK; GTEXT_ANIM_TYPE_ERROR (an unresolved time when the current time is resolved). */
GTEXT_API GTEXT_Anim_Status gtext_anim_animation_set_current_time(GTEXT_Anim_Animation * animation, GTEXT_Anim_Time seek, GTEXT_Anim_Time now);

/** @brief `startTime = start` ("set the start time"). */
GTEXT_API GTEXT_Anim_Status gtext_anim_animation_set_start_time(GTEXT_Anim_Animation * animation, GTEXT_Anim_Time start, GTEXT_Anim_Time now);

/** @brief `play()` ("play an animation" with auto-rewind). @return GTEXT_ANIM_OK; GTEXT_ANIM_INVALID_STATE (a negative rate and an infinite effect end; nothing changed). */
GTEXT_API GTEXT_Anim_Status gtext_anim_animation_play(GTEXT_Anim_Animation * animation, GTEXT_Anim_Time now);

/** @brief `pause()` ("pause an animation"). @return GTEXT_ANIM_OK; GTEXT_ANIM_INVALID_STATE (an unresolved current time, a negative rate and an infinite effect end; nothing changed). */
GTEXT_API GTEXT_Anim_Status gtext_anim_animation_pause(GTEXT_Anim_Animation * animation, GTEXT_Anim_Time now);

/** @brief `finish()` ("finish an animation"). @return GTEXT_ANIM_OK; GTEXT_ANIM_INVALID_STATE (an effective rate of 0, or positive with an infinite effect end; nothing changed). */
GTEXT_API GTEXT_Anim_Status gtext_anim_animation_finish(GTEXT_Anim_Animation * animation, GTEXT_Anim_Time now);

/** @brief `cancel()` ("cancel an animation"). */
GTEXT_API GTEXT_Anim_Status gtext_anim_animation_cancel(GTEXT_Anim_Animation * animation, GTEXT_Anim_Time now);

/** @brief `reverse()` ("reverse an animation"). @return GTEXT_ANIM_OK; GTEXT_ANIM_INVALID_STATE (no active timeline, or the play throws; nothing changed). */
GTEXT_API GTEXT_Anim_Status gtext_anim_animation_reverse(GTEXT_Anim_Animation * animation, GTEXT_Anim_Time now);

/** @brief `playbackRate = rate` ("set the playback rate"). @return GTEXT_ANIM_OK; GTEXT_ANIM_TYPE_ERROR (not a finite number). */
GTEXT_API GTEXT_Anim_Status gtext_anim_animation_set_playback_rate(GTEXT_Anim_Animation * animation, double rate, GTEXT_Anim_Time now);

/** @brief `updatePlaybackRate(rate)` ("seamlessly update the playback rate"). @return GTEXT_ANIM_OK; GTEXT_ANIM_TYPE_ERROR (not a finite number); GTEXT_ANIM_INVALID_STATE (the play it may do throws; the pending rate is set). */
GTEXT_API GTEXT_Anim_Status gtext_anim_animation_update_playback_rate(GTEXT_Anim_Animation * animation, double rate, GTEXT_Anim_Time now);

/** @brief `persist()`: the replace state becomes persisted. */
GTEXT_API GTEXT_Anim_Status gtext_anim_animation_persist(GTEXT_Anim_Animation * animation);

/**
 * @brief "Removing replaced animations" for one animation whose other conditions the caller has decided (the effect has a target and every property of it is overridden by a replaceable animation
 * of higher composite order): if the animation is itself replaceable (finished, not removed, a monotonically increasing timeline, an effect that is in effect) its replace state becomes removed
 * and a `remove` event is owed.
 * @param removed Receives whether it was removed; may be NULL.
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_animation_remove_replaced(GTEXT_Anim_Animation * animation, GTEXT_Anim_Time now, bool * removed);

/**
 * @brief The pending play or pause task, if there is one and the timeline is active: "as soon as the animation is ready", with `now` the time the animation became ready at. A no-op for an
 * animation with no pending task or an inactive timeline (the task waits).
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_animation_run_pending(GTEXT_Anim_Animation * animation, GTEXT_Anim_Time now);

/** @brief A microtask checkpoint: the finish notification steps that were queued run (the finished promise is settled and a `finish` event owed if the animation is still finished). */
GTEXT_API GTEXT_Anim_Status gtext_anim_animation_checkpoint(GTEXT_Anim_Animation * animation, GTEXT_Anim_Time now);

/** @brief The step of "update animations and send events" that runs "update an animation's finished state" for the animation (did seek false, notify false). */
GTEXT_API GTEXT_Anim_Status gtext_anim_animation_update_finished(GTEXT_Anim_Animation * animation, GTEXT_Anim_Time now);

/** @brief How many promise settlements are owed. NULL is 0. */
GTEXT_API size_t gtext_anim_animation_note_count(const GTEXT_Anim_Animation * animation);

/** @brief Note `index` of those owed. @return false past the end. */
GTEXT_API bool gtext_anim_animation_note_at(const GTEXT_Anim_Animation * animation, size_t index, GTEXT_Anim_Note * out);

/** @brief The settlements are done: forget them. */
GTEXT_API void gtext_anim_animation_notes_clear(GTEXT_Anim_Animation * animation);

/**
 * @brief Move the playback events the animation owes (`finish`, `cancel`, `remove`) to the end of `events`, in the order they were queued, and forget them. Each has the animation's own `current_time` and
 * `timeline_time` (an unresolved one is `null`), the scheduled event time ("an unresolved time value" for an animation with no document timeline: the host dispatches it at its next task), and
 * `animation` as the id the caller gives. The `element` is NULL and the strings are empty. All or nothing.
 * @param id The value of the `animation` member of each event. @return GTEXT_ANIM_OK; GTEXT_ANIM_INVALID; GTEXT_ANIM_OOM (nothing moved).
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_animation_take_events(GTEXT_Anim_Animation * animation, uint64_t id, GTEXT_Anim_Events * events);

/** @brief How many playback events are owed. NULL is 0. */
GTEXT_API size_t gtext_anim_animation_event_count(const GTEXT_Anim_Animation * animation);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_ANIMATION_ANIMATION_H
