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
 * What the files of the Web Animations model share (animation/world.h, effect.h): the world, the keyframe effects it owns and the script animations it makes. Private.
 */

#ifndef GHOTI_IO_GTEXT_SRC_ANIMATION_WORLD_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_ANIMATION_WORLD_INTERNAL_H

#include <ghoti.io/text/macros.h>

#include <ghoti.io/text/animation/effect.h>
#include <ghoti.io/text/animation/world.h>
#include <ghoti.io/text/css/css_keyframes.h>
#include <ghoti.io/text/css/css_style.h>

#include "../css/cssom_computed_internal.h"
#include "../css/cssom_doc_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/// An Animation the world holds: one script touched (a CSS animation or a CSS transition) or made (a script animation).
typedef struct {
  GTEXT_Anim_Animation * animation;
  GTEXT_Anim_Kind kind;
  uint64_t id;
  const void * element;
  char * pseudo;
  size_t pseudo_length;
} control_t;

struct GTEXT_Anim_World {
  GTEXT_Allocator alloc;
  control_t * controls;
  size_t ncontrols, capcontrols;
  GTEXT_CSSOM_Document * doc;
  GTEXT_CSSOM_Transition_Tracker * tracker;
  GTEXT_CSSOM_Transition_Events * stash;   ///< The transition events of a frame that could not be completed: the next frame reports them, so that a frame is whole or not at all.
  GTEXT_Anim_Events * stash_animations;    ///< The same for the events of the CSS animations.
  GTEXT_Anim_Effect ** effects;            ///< Every effect the world made (owned), in creation order.
  size_t neffects, capeffects;
  uint64_t next_script_id;                 ///< The id of the next script animation (they are numbered in the order the Animation objects were made: the global animation list).
  /* a commit being calculated ("commit computed styles"): the effects of the target element above the committing animation are left out, and the phases are endpoint-inclusive */
  struct {
    bool active;
    const void * element;
    uint64_t limit;                        ///< The id of the committing animation: effects of a higher composite order are not in the partial effect stack.
    const GTEXT_Anim_Animation * self;     ///< It is in the stack even if its replace state is removed.
  } commit;
  /* the script effects the sampler is given for one element: rebuilt for each call, valid until the next */
  GTEXT_CSS_Script_Effect * sample;
  size_t nsample, capsample;
};

/// The registry of the world's document.
const GTEXT_CSS_Registry * anim_world_registry(const GTEXT_Anim_World * world);

/// The world's core.
css_core * anim_world_core(const GTEXT_Anim_World * world);

/// The world tells the layout that what a style is sampled from changed.
void anim_world_touch(GTEXT_Anim_World * world);

/// The tracker's time as the model's time.
GTEXT_Anim_Time anim_world_now(const GTEXT_Anim_World * world);

/// The effect an animation of the world holds (NULL: none): the effect object or, for an Animation of CSS, a view the world makes on first use.
GTEXT_Anim_Effect * anim_effect_of_animation(const GTEXT_Anim_Animation * animation);

/// Release every effect and script animation of a world (gtext_anim_world_free()).
void anim_script_free(GTEXT_Anim_World * world);

/// The info of a script animation (script control `control` of the world's list), and whether it is relevant. false: that control is not a script animation.
bool anim_script_info(GTEXT_Anim_World * world, size_t control, GTEXT_Anim_Info * out, bool * relevant);

/// "Remove replaced animations" for the document: the script animations whose every target property is overridden by a replaceable animation of a higher composite order become removed, and a `remove`
/// event is owed for each. `*changed` says whether one was.
GTEXT_Anim_Status anim_script_remove_replaced(GTEXT_Anim_World * world, bool * changed);

/// An effect is no longer held by its animation (the animation is going away): effect.c.
void anim_effect_detach(GTEXT_Anim_Effect * effect);

/// Release every keyframe effect the world made (effect.c).
void anim_effects_free(GTEXT_Anim_World * world);

/// The keyframe effect `effect` as the sampler reads it at `now` (with `endpoint_inclusive`, the phases of the active interval include its end: "commit computed styles"); `*in_effect` false when its progress is unresolved (nothing to sample). @return GTEXT_ANIM_OK; GTEXT_ANIM_OOM.
GTEXT_Anim_Status anim_effect_sample(GTEXT_Anim_Effect * effect, GTEXT_Anim_Time now, bool endpoint_inclusive, GTEXT_CSS_Script_Effect * out, bool * in_effect);

/// The pseudo-element key of the effect's target (the name without colons, "" for the element), and its target element.
const char * anim_effect_key(const GTEXT_Anim_Effect * effect, size_t * length);
const void * anim_effect_target(const GTEXT_Anim_Effect * effect);

/// Register the sampler's hook of a world's document (gtext_anim_world_new()), and take it away (gtext_anim_world_free()).
void anim_script_register(GTEXT_Anim_World * world);
void anim_script_unregister(GTEXT_Anim_World * world);

#ifdef __cplusplus
}
#endif

#endif
