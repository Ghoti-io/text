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
 * @file world.h
 * @brief The animations of a document: `Element.getAnimations()`, `Document.getAnimations()` and "update animations and send events" over the CSS Animations and CSS Transitions of the CSSOM's
 * tracker.
 *
 * Normative text: Web Animations 1 ("Relevant animations", "Animation events", "Update animations and send events", and the `Animatable` and `DocumentOrShadowRoot` interfaces), CSS Animations 2 ("Animation
 * composite order") and CSS Transitions 2 ("Animation composite order"), as pinned in tools/css/DRAFTS_SHA256.
 *
 * **The world is a view, and has no clock.** The tracker (css_cssom_computed.h) holds the animations a style change event makes, one set of CSS Transitions and one of CSS Animations for each element and
 * pseudo-element, and the time (gtext_cssom_transition_tracker_set_time(): the host's tick, the only clock). The world answers questions over them in the order of the specification and drains the events
 * a frame owes. An animation is addressed by its kind and its id, which are stable while it exists: the binding makes one object for each pair (identity is the binding's).
 */

#ifndef GHOTI_IO_GTEXT_ANIMATION_WORLD_H
#define GHOTI_IO_GTEXT_ANIMATION_WORLD_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/animation/css_animations.h>
#include <ghoti.io/text/animation/events.h>
#include <ghoti.io/text/animation/timing.h>
#include <ghoti.io/text/css/css_cssom_computed.h>
#include <ghoti.io/text/css/css_cssom_document.h>
#include <ghoti.io/text/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The animation classes of Web Animations 1, "Animation classes", that a document's tracker makes: the order is the composite order between classes (a CSS Transition before a CSS Animation). */
typedef enum {
  GTEXT_ANIM_KIND_CSS_TRANSITION = 1,   ///< A `CSSTransition`.
  GTEXT_ANIM_KIND_CSS_ANIMATION         ///< A `CSSAnimation`.
} GTEXT_Anim_Kind;

/** @brief The animations of one document, as a view over its tracker. Opaque. */
typedef struct GTEXT_Anim_World GTEXT_Anim_World;

/**
 * @struct GTEXT_Anim_Info
 * @brief One animation as the Web Animations interface reads it at the tracker's time: the members of `Animation`, `CSSAnimation` and `CSSTransition`, and of its effect. Strings are valid until the tracker is updated, a frame is taken or the tracker is freed.
 */
typedef struct {
  GTEXT_Anim_Kind kind;
  uint64_t id;                          ///< Unique among the animations of its kind in the tracker, and the same in the events.
  const void * element;                 ///< The owning element (the tree's own handle): the target of the effect.
  GTEXT_CSS_String pseudo_element;      ///< The target pseudo-element with two colons, or empty.
  GTEXT_CSS_String name;                ///< `animationName` of a CSSAnimation, `transitionProperty` of a CSSTransition.
  GTEXT_Anim_Play_State play_state;
  bool pending;
  GTEXT_Anim_Time start_time;
  GTEXT_Anim_Time current_time;
  GTEXT_Anim_Effect_Timing timing;      ///< `effect.getTiming()`; the easing is not given (see `easing`).
  GTEXT_CSS_String easing;              ///< `effect.getTiming().easing`.
  GTEXT_Anim_Computed_Timing computed;  ///< `effect.getComputedTiming()`.
  GTEXT_CSS_String start_value;         ///< A transition's offset 0 value (serialized computed value); empty for an animation.
  GTEXT_CSS_String end_value;           ///< A transition's offset 1 value.
} GTEXT_Anim_Info;

/** @brief A list of animations in the composite order. Opaque; release with gtext_anim_list_free(). */
typedef struct GTEXT_Anim_List GTEXT_Anim_List;

/** @brief How many animations. NULL is 0. */
GTEXT_API size_t gtext_anim_list_count(const GTEXT_Anim_List * list);

/** @brief Animation `index`. @return false past the end. */
GTEXT_API bool gtext_anim_list_at(const GTEXT_Anim_List * list, size_t index, GTEXT_Anim_Info * out);

/** @brief Release a list; NULL is ignored. */
GTEXT_API void gtext_anim_list_free(GTEXT_Anim_List * list);

/**
 * @brief Make a world over a document and its tracker. Neither is owned; both must outlive it.
 * @param allocator NULL for the default. @return GTEXT_ANIM_OK; GTEXT_ANIM_INVALID; GTEXT_ANIM_OOM.
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_world_new(GTEXT_CSSOM_Document * document, GTEXT_CSSOM_Transition_Tracker * tracker, const GTEXT_Allocator * allocator, GTEXT_Anim_World ** out);

/** @brief Release a world; NULL is ignored. */
GTEXT_API void gtext_anim_world_free(GTEXT_Anim_World * world);

/**
 * @brief `Element.getAnimations()` and `Document.getAnimations()`: "the relevant animations ... whose target element is" the element, or with `subtree` the element, its pseudo-elements and its descendants and
 * theirs, in the composite order ("sorted by their composite order": the transitions before the animations, each by the tree order of its owning element, the pseudo-elements of one as CSS Animations 2 and
 * Transitions 2 list them, then the order within the element: a transition's generation and property name, an animation's place in `animation-name`).
 *
 * The tree order is the element interface's (`compare_order`); without it, the order the tracker first saw the elements in.
 *
 * @param element The element, or NULL for the document ("every relevant animation whose target element is in the document"). @param pseudo The target pseudo-element as the effect has it (`::before`), NULL
 * for the element itself; with a pseudo-element, `subtree` has no descendants to add. @param pseudo_length Its length. @param subtree Include the descendants.
 * @param out Receives the list. @return GTEXT_ANIM_OK; GTEXT_ANIM_INVALID; GTEXT_ANIM_OOM.
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_world_get_animations(GTEXT_Anim_World * world, const void * element, const char * pseudo, size_t pseudo_length, bool subtree, GTEXT_Anim_List ** out);

/**
 * @brief The animation of a kind and an id, relevant or not: what a page that holds an `Animation` object reads (`playState`, `currentTime` ...) after the animation has left `getAnimations()`.
 * @param found Receives whether the animation is still in the tracker: an animation that was cancelled (its name left the list, its element left the document) is not, and a caller reads it as idle. Not NULL.
 * @return GTEXT_ANIM_OK; GTEXT_ANIM_INVALID; GTEXT_ANIM_OOM.
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_world_find(GTEXT_Anim_World * world, GTEXT_Anim_Kind kind, uint64_t id, bool * found, GTEXT_Anim_Info * out);

/**
 * @brief "Update animations and send events" for one frame, at the tracker's time: the events of every CSS Transition (css_transition.h, the table of Transitions 2) and every CSS Animation (the table of CSS Animations 2)
 * that moving from the phase each was last seen in to the phase now dispatches, each with its target, in the order Web Animations 1 gives ("a stable sort ... by their scheduled event time ... Within events with equal
 * scheduled event times, sort by their composite order"). The animations with a pending task become ready.
 *
 * All or nothing: on a failure nothing moved. The pseudo-element of an event is its name with two colons.
 * @return GTEXT_ANIM_OK; GTEXT_ANIM_INVALID; GTEXT_ANIM_OOM.
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_world_update(GTEXT_Anim_World * world, GTEXT_Anim_Events ** out);

/** @brief The time the world reads: the tracker's. */
GTEXT_API double gtext_anim_world_time(const GTEXT_Anim_World * world);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_ANIMATION_WORLD_H
