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
#include <ghoti.io/text/animation/animation.h>
#include <ghoti.io/text/animation/css_animations.h>
#include <ghoti.io/text/animation/effect.h>
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
  GTEXT_ANIM_KIND_CSS_ANIMATION,        ///< A `CSSAnimation`.
  GTEXT_ANIM_KIND_SCRIPT                ///< An `Animation` that script made (`new Animation()`, `Element.animate()`): no class, so a higher composite order than the other two; among themselves in the order they were made.
} GTEXT_Anim_Kind;

/** @brief The animations of one document, as a view over its tracker, and the ones script makes. Opaque. */
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
  GTEXT_Anim_Composite composite;       ///< `effect.composite`: of a CSSAnimation its `animation-composition`, of a CSSTransition `replace`.
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

/**
 * @brief The Animation of a CSS animation or a CSS transition, for script that calls its methods or reads its promises: the first call for an animation makes an Animation (animation.h) with the effect's
 * timing and the state the CSS animation or transition had, and from then on the Animation holds the animation's timing state: its current time is what the style samples and what the events of CSS
 * Animations 2 and CSS Transitions 2 compare, a style change event does to it what the two specifications say (for a CSS animation `animation-play-state` pauses it or plays it again, a changed timing is the
 * effect's new timing, a name that leaves the list cancels it; a transition the style cancels, replaces or reverses is cancelled, and one that completes lets go of it), the frame runs its pending task and
 * asks whether it finished, and its `finish`, `cancel` and `remove` events are in the frame's list (gtext_anim_world_update()). The world owns the Animation; it is valid until the world is freed and the same
 * pointer is returned for the same kind and id, even after the animation was cancelled or the transition completed.
 *
 * Every call that changes it takes the tracker's time: pass gtext_anim_world_time() as `now`, and call gtext_anim_world_touch() after it, so that a style computed before is not served after.
 * @return GTEXT_ANIM_OK; GTEXT_ANIM_INVALID (no such animation: a transition is held while it is running, and an id of a transition that left the set is not found); GTEXT_ANIM_OOM.
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_world_control(GTEXT_Anim_World * world, GTEXT_Anim_Kind kind, uint64_t id, GTEXT_Anim_Animation ** out);

/**
 * @brief `new Animation(effect, timeline)`: an animation made by script, idle, in the world's global animation list after every animation made before it (its composite order). The effect may be NULL; its timeline
 * is the document timeline or none. The animation is not played: gtext_anim_animation_play().
 *
 * Call gtext_anim_world_touch() after any call that changes it. The world owns the Animation (and the effect); it is valid until the world is freed. The ids of script animations are never those of the tracker's.
 * @param id Receives its id (with GTEXT_ANIM_KIND_SCRIPT, the key of gtext_anim_world_control()). @param out Receives the Animation.
 * @return GTEXT_ANIM_OK; GTEXT_ANIM_INVALID; GTEXT_ANIM_OOM.
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_world_animation_new(GTEXT_Anim_World * world, GTEXT_Anim_Effect * effect, GTEXT_Anim_Timeline_Kind timeline, uint64_t * id, GTEXT_Anim_Animation ** out);

/**
 * @brief `Element.animate(keyframes, options)`: a keyframe effect on `target` (as the constructor of `KeyframeEffect` makes it) held by a new animation on the document timeline, played with the auto-rewind flag.
 *
 * @param target The element (the tree's handle; not NULL). @param keyframes, options As gtext_anim_world_effect_new(). @param effect Receives the effect; may be NULL. @param id, out As gtext_anim_world_animation_new().
 * @return GTEXT_ANIM_OK; GTEXT_ANIM_TYPE_ERROR; GTEXT_ANIM_SYNTAX_ERROR; GTEXT_ANIM_INVALID; GTEXT_ANIM_OOM. On an error no animation was made (an effect may have been, and stays in the world).
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_world_animate(GTEXT_Anim_World * world, const void * target, const GTEXT_Anim_Keyframes_Argument * keyframes, const GTEXT_Anim_Effect_Options * options,
    GTEXT_Anim_Effect ** effect, uint64_t * id, GTEXT_Anim_Animation ** out);

/** @brief The inline style declarations `commitStyles()` writes: the longhands an animation's effect targets, with their computed values. Opaque; release with gtext_anim_style_values_free(). */
typedef struct GTEXT_Anim_Style_Values GTEXT_Anim_Style_Values;

/**
 * @brief `Animation.commitStyles()` ("commit computed styles"): for each physical longhand the animation's effect targets, the computed value of the effect stack of the target's property up to and including this
 * animation (the effects of a higher composite order are left out, the effect of a removed animation is in) at the world's time, with "the endpoint-inclusive active interval" flag set, so that an animation
 * that has just finished commits its last value though it has no fill. The caller writes each as the declaration of the target's inline style (the style attribute: the library does not own it); the
 * model does not write anything.
 *
 * "Unlike most other methods defined on this interface, calling this method does trigger a style change event": the caller flushes the tracker's style change events after it writes.
 *
 * @param animation An Animation of this world. @param out Receives the declarations (empty for an animation with no effect or no target element).
 * @return GTEXT_ANIM_OK; GTEXT_ANIM_NO_MODIFICATION_ALLOWED (the target is a pseudo-element); GTEXT_ANIM_INVALID_STATE (the target is not being rendered: it, or an ancestor, has `display: none`; an element with
 * `display: contents` is rendered, as Web Animations 1 assumes); GTEXT_ANIM_INVALID; GTEXT_ANIM_OOM.
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_world_commit_styles(GTEXT_Anim_World * world, const GTEXT_Anim_Animation * animation, GTEXT_Anim_Style_Values ** out);

/** @brief How many declarations. NULL is 0. */
GTEXT_API size_t gtext_anim_style_values_count(const GTEXT_Anim_Style_Values * list);

/** @brief The element the declarations are for (the effect's target). */
GTEXT_API const void * gtext_anim_style_values_element(const GTEXT_Anim_Style_Values * list);

/** @brief Declaration `index`: a longhand's name (a custom property's name) and its value, serialized. Valid until the list is freed. @return false past the end. */
GTEXT_API bool gtext_anim_style_values_at(const GTEXT_Anim_Style_Values * list, size_t index, GTEXT_CSS_String * name, GTEXT_CSS_String * value);

/** @brief Release the declarations; NULL is ignored. */
GTEXT_API void gtext_anim_style_values_free(GTEXT_Anim_Style_Values * list);

/** @brief How many Animations the world holds (the CSS animations script has touched), in the order they were first asked for. NULL is 0. */
GTEXT_API size_t gtext_anim_world_control_count(const GTEXT_Anim_World * world);

/** @brief Control `index`: the kind and id of its CSS animation or transition (`kind` may be NULL) and its Animation (so that the host drains the notes of each: gtext_anim_animation_note_at()). @return false past the end. */
GTEXT_API bool gtext_anim_world_control_at(const GTEXT_Anim_World * world, size_t index, GTEXT_Anim_Kind * kind, uint64_t * id, GTEXT_Anim_Animation ** animation);

/**
 * @brief Something an Animation the world holds does reaches a style without the tracker being told (the Animation was played, paused, seeked, finished, reversed, its rate changed): the version that
 * GTEXT_CSSOM_Layout_Interface::transition_version reads moves on, so that a style computed before the call is computed again. Call it after any call that changes an Animation of gtext_anim_world_control().
 * @return GTEXT_ANIM_OK; GTEXT_ANIM_INVALID.
 */
GTEXT_API GTEXT_Anim_Status gtext_anim_world_touch(GTEXT_Anim_World * world);

/** @brief A microtask checkpoint for every Animation the world holds, at the tracker's time (gtext_anim_animation_checkpoint()). */
GTEXT_API GTEXT_Anim_Status gtext_anim_world_checkpoint(GTEXT_Anim_World * world);

/** @brief The time the world reads: the tracker's. */
GTEXT_API double gtext_anim_world_time(const GTEXT_Anim_World * world);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_ANIMATION_WORLD_H
