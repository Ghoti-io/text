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
 * @file events.h
 * @brief The animation events a frame owes the page (Web Animations 1, "Animation events"): the events of CSS Animations and of CSS Transitions, as values, with the *scheduled event time* that orders them.
 *
 * Nothing is dispatched here: "update animations and send events" is a call that returns the events in the order the specification gives ("Perform a stable sort ... Sort the events by their
 * scheduled event time ... events whose scheduled event time is unresolved sort before events with a resolved scheduled event time. Within events with equal scheduled event times, sort by their composite
 * order"), and the host dispatches them.
 */

#ifndef GHOTI_IO_GTEXT_ANIMATION_EVENTS_H
#define GHOTI_IO_GTEXT_ANIMATION_EVENTS_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/animation/timing.h>
#include <ghoti.io/text/css/css_core.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The events of CSS Animations (`AnimationEvent`) and of CSS Transitions (`TransitionEvent`). */
typedef enum {
  GTEXT_ANIM_EVENT_ANIMATIONSTART = 0,
  GTEXT_ANIM_EVENT_ANIMATIONITERATION,
  GTEXT_ANIM_EVENT_ANIMATIONEND,
  GTEXT_ANIM_EVENT_ANIMATIONCANCEL,
  GTEXT_ANIM_EVENT_TRANSITIONRUN,
  GTEXT_ANIM_EVENT_TRANSITIONSTART,
  GTEXT_ANIM_EVENT_TRANSITIONEND,
  GTEXT_ANIM_EVENT_TRANSITIONCANCEL
} GTEXT_Anim_Event_Type;

/** @struct GTEXT_Anim_Event @brief One event to dispatch. Strings are valid until the next call that changes the list (append, reserve, clear, sort) or frees it. */
typedef struct {
  GTEXT_Anim_Event_Type type;
  uint64_t animation;                   ///< The id of the animation (`CSSAnimation` or `CSSTransition`) the event is of: unique among the animations of one kind, so the kind (`type`) and the id name it.
  const void * element;                 ///< "The target for [the] event is the [animation's] owning element" (the tree's own handle); NULL in a list a set made before an owner was known.
  GTEXT_CSS_String pseudo_element;      ///< `pseudoElement`: the name with two colons, or empty for the element.
  GTEXT_CSS_String name;                ///< `animationName`, or `propertyName` of a transition.
  double elapsed_time;                  ///< `elapsedTime`, in seconds.
  GTEXT_Anim_Time scheduled_time;       ///< The scheduled event time on the clock of the document timeline, in milliseconds; unresolved for an animation with no start time.
} GTEXT_Anim_Event;

/** @brief A list of events. Opaque; release with gtext_anim_events_free(). */
typedef struct GTEXT_Anim_Events GTEXT_Anim_Events;

/** @brief Make an empty list. @param allocator NULL for the default. @return GTEXT_ANIM_OK, GTEXT_ANIM_INVALID, GTEXT_ANIM_OOM. */
GTEXT_API GTEXT_Anim_Status gtext_anim_events_new(const GTEXT_Allocator * allocator, GTEXT_Anim_Events ** out);

/** @brief Release a list; NULL is ignored. */
GTEXT_API void gtext_anim_events_free(GTEXT_Anim_Events * events);

/** @brief How many events. NULL is 0. */
GTEXT_API size_t gtext_anim_events_count(const GTEXT_Anim_Events * events);

/** @brief Event `index` in the order to dispatch them. @return false past the end. */
GTEXT_API bool gtext_anim_events_at(const GTEXT_Anim_Events * events, size_t index, GTEXT_Anim_Event * out);

/**
 * @brief Append one event (a copy: the strings are copied into the list). The owner of a set that knows no element passes NULL and an empty pseudo-element name.
 * @return GTEXT_ANIM_OK; GTEXT_ANIM_INVALID; GTEXT_ANIM_OOM (the list is as it was).
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_events_append(GTEXT_Anim_Events * events, const GTEXT_Anim_Event * event);

/** @brief Empty a list, keeping its room. NULL is ignored. */
GTEXT_API void gtext_anim_events_clear(GTEXT_Anim_Events * events);

/**
 * @brief Make room for more events and name bytes, so that appending them cannot fail.
 * @return GTEXT_ANIM_OK; GTEXT_ANIM_INVALID; GTEXT_ANIM_OOM (the list is as it was).
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_events_reserve(GTEXT_Anim_Events * events, size_t more_events, size_t more_name_bytes);

/**
 * @brief The stable sort of "Update animations and send events": by scheduled event time, an unresolved one before every resolved one, and the order the events were appended in (the composite
 * order) between equal times. A sort that cannot fail: with no memory for a merge it falls back to an insertion sort, which is the same order.
 */
GTEXT_API void gtext_anim_events_sort(GTEXT_Anim_Events * events);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_ANIMATION_EVENTS_H
