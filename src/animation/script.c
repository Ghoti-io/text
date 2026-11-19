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
 * The animations script makes (animation/world.h): `new Animation(effect, timeline)` and `Element.animate()`, the keyframe effects they stack on an element, the removal of replaced animations, and
 * `commitStyles()`.
 */

#include <ghoti.io/text/animation/world.h>

#include <string.h>

#include "animation_internal.h"
#include "world_internal.h"

/// The ids of script animations start here, so that no id of one is the id of a CSS animation or transition of the tracker (which count up from one): a playback event names its animation by id alone.
#define SCRIPT_ID_BASE ((uint64_t)1 << 40)

const GTEXT_CSS_Registry * anim_world_registry(const GTEXT_Anim_World * world) {
  return css_document_core(world->doc)->registry;
}

css_core * anim_world_core(const GTEXT_Anim_World * world) {
  return css_document_core(world->doc);
}

GTEXT_Anim_Time anim_world_now(const GTEXT_Anim_World * world) {
  return (GTEXT_Anim_Time){true, css_tracker_time(world->tracker)};
}

void anim_world_touch(GTEXT_Anim_World * world) {
  css_core * core = css_document_core(world->doc);
  core->script_version++;
  css_tracker_touch(world->tracker);
}

/* ---- the sampler's source ------------------------------------------------------------------------------------------------------------------------- */

static control_t * script_control(const GTEXT_Anim_World * w, size_t i) {
  return w->controls[i].kind == GTEXT_ANIM_KIND_SCRIPT ? &w->controls[i] : NULL;
}

/// Whether the effect of an animation is part of the effect stacks: "The animation effects of an animation whose replace state is removed are not included in the effect stacks of their target properties".
static bool stacked(const GTEXT_Anim_Animation * a) {
  GTEXT_Anim_Animation_State s;
  return gtext_anim_animation_state(a, (GTEXT_Anim_Time){true, 0.0}, &s) == GTEXT_ANIM_OK && s.replace_state != GTEXT_ANIM_REPLACE_REMOVED;
}

static GTEXT_CSS_Status script_hook(void * user, const void * element, const char * key, size_t key_length, const GTEXT_CSS_Script_Effect ** out, size_t * count) {
  GTEXT_Anim_World * w = user;
  *out = NULL;
  *count = 0;
  const GTEXT_Anim_Time now = anim_world_now(w);
  size_t n = 0;
  for (size_t i = 0; i < w->ncontrols; i++) {
    control_t * c = script_control(w, i);
    if (!c) {
      continue;
    }
    GTEXT_Anim_Effect * e = gtext_anim_animation_effect(c->animation);
    if (!e || anim_effect_target(e) != element) {
      continue;
    }
    size_t kl = 0;
    const char * k = anim_effect_key(e, &kl);
    if (kl != key_length || (kl && memcmp(k, key, kl) != 0)) {
      continue;
    }
    const bool committing = w->commit.active && w->commit.element == element;
    if (committing && c->id > w->commit.limit) {
      continue;   /* "Remove from partialEffectStack any animation effects whose associated animation has a higher composite order than animation" */
    }
    if (!stacked(c->animation) && !(committing && c->animation == w->commit.self)) {
      continue;
    }
    if (n == w->capsample) {
      const size_t cap = w->capsample ? w->capsample * 2 : 8;
      GTEXT_CSS_Script_Effect * bigger = gtext_allocator_realloc(&w->alloc, w->sample, cap * sizeof *bigger);
      if (!bigger) {
        return GTEXT_CSS_E_OOM;
      }
      w->sample = bigger;
      w->capsample = cap;
    }
    bool in_effect = false;
    const GTEXT_Anim_Status st = anim_effect_sample(e, now, committing, &w->sample[n], &in_effect);
    if (st != GTEXT_ANIM_OK) {
      return st == GTEXT_ANIM_OOM ? GTEXT_CSS_E_OOM : GTEXT_CSS_E_INVALID;
    }
    if (in_effect) {
      n++;
    }
  }
  *out = w->sample;
  *count = n;
  return GTEXT_CSS_OK;
}

void anim_script_register(GTEXT_Anim_World * w) {
  css_core * core = css_document_core(w->doc);
  core->script.effects = script_hook;
  core->script.user = w;
  core->script_version++;
}

void anim_script_unregister(GTEXT_Anim_World * w) {
  css_core * core = css_document_core(w->doc);
  if (core->script.user == w) {
    core->script.effects = NULL;
    core->script.user = NULL;
    core->script_version++;
  }
}

void anim_script_free(GTEXT_Anim_World * w) {
  anim_effects_free(w);
  gtext_allocator_free(&w->alloc, w->sample);
  w->sample = NULL;
  w->nsample = w->capsample = 0;
}

/* ---- making them ---------------------------------------------------------------------------------------------------------------------------------- */

static GTEXT_Anim_Status control_push(GTEXT_Anim_World * w, GTEXT_Anim_Animation * a, uint64_t id) {
  if (w->ncontrols == w->capcontrols) {
    const size_t cap = w->capcontrols ? w->capcontrols * 2 : 8;
    control_t * bigger = gtext_allocator_realloc(&w->alloc, w->controls, cap * sizeof *bigger);
    if (!bigger) {
      return GTEXT_ANIM_OOM;
    }
    w->controls = bigger;
    w->capcontrols = cap;
  }
  control_t * c = &w->controls[w->ncontrols++];
  memset(c, 0, sizeof *c);
  c->animation = a;
  c->kind = GTEXT_ANIM_KIND_SCRIPT;
  c->id = id;
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status gtext_anim_world_animation_new(GTEXT_Anim_World * w, GTEXT_Anim_Effect * effect, GTEXT_Anim_Timeline_Kind timeline, uint64_t * id, GTEXT_Anim_Animation ** out) {
  if (!w || !out || !id) {
    return GTEXT_ANIM_INVALID;
  }
  *out = NULL;
  GTEXT_Anim_Animation * a = NULL;
  GTEXT_Anim_Status st = gtext_anim_animation_new(&w->alloc, timeline, &a);
  if (st != GTEXT_ANIM_OK) {
    return st;
  }
  const GTEXT_Anim_Time now = anim_world_now(w);
  /* "Set the timeline of animation ... Set the associated effect of animation" (the constructor steps of Animation) */
  st = gtext_anim_animation_reserve(a);
  if (st == GTEXT_ANIM_OK && effect) {
    st = gtext_anim_animation_set_effect_object(a, effect, now);
  }
  if (st == GTEXT_ANIM_OK) {
    st = control_push(w, a, SCRIPT_ID_BASE + w->next_script_id);
  }
  if (st != GTEXT_ANIM_OK) {
    if (effect && gtext_anim_animation_effect(a) == effect) {
      (void)gtext_anim_animation_set_effect_object(a, NULL, now);
    }
    gtext_anim_animation_free(a);
    return st;
  }
  *id = SCRIPT_ID_BASE + w->next_script_id++;
  *out = a;
  anim_world_touch(w);
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status gtext_anim_world_animate(GTEXT_Anim_World * w, const void * target, const GTEXT_Anim_Keyframes_Argument * keyframes, const GTEXT_Anim_Effect_Options * options,
    GTEXT_Anim_Effect ** effect, uint64_t * id, GTEXT_Anim_Animation ** out) {
  if (!w || !out || !id || !target) {
    return GTEXT_ANIM_INVALID;
  }
  *out = NULL;
  GTEXT_Anim_Effect * e = NULL;
  /* "Construct a new KeyframeEffect object effect ... by using the same procedure as the KeyframeEffect constructor" */
  GTEXT_Anim_Status st = gtext_anim_world_effect_new(w, target, keyframes, options, &e);
  if (st != GTEXT_ANIM_OK) {
    return st;
  }
  GTEXT_Anim_Animation * a = NULL;
  st = gtext_anim_world_animation_new(w, e, GTEXT_ANIM_TIMELINE_DOCUMENT, id, &a);
  if (st == GTEXT_ANIM_OK) {
    /* "Run the procedure to play an animation for animation with the auto-rewind flag set to true" */
    st = gtext_anim_animation_reserve(a);
    if (st == GTEXT_ANIM_OK) {
      st = gtext_anim_animation_play(a, anim_world_now(w));
    }
    if (st == GTEXT_ANIM_OK) {
      anim_world_touch(w);
    }
  }
  if (st != GTEXT_ANIM_OK) {
    return st;   /* the effect stays in the world (owned there); an animation that was made stays in the list, idle */
  }
  if (effect) {
    *effect = e;
  }
  *out = a;
  return GTEXT_ANIM_OK;
}

/* ---- the info of one ------------------------------------------------------------------------------------------------------------------------------- */

bool anim_script_info(GTEXT_Anim_World * w, size_t control, GTEXT_Anim_Info * out, bool * relevant) {
  control_t * c = script_control(w, control);
  if (!c) {
    return false;
  }
  GTEXT_Anim_Effect * e = gtext_anim_animation_effect(c->animation);
  const GTEXT_Anim_Time now = anim_world_now(w);
  memset(out, 0, sizeof *out);
  out->kind = GTEXT_ANIM_KIND_SCRIPT;
  out->id = c->id;
  GTEXT_Anim_Animation_State s;
  (void)gtext_anim_animation_state(c->animation, now, &s);
  out->play_state = s.play_state;
  out->pending = s.pending;
  out->start_time = s.start_time;
  out->current_time = s.current_time;
  *relevant = false;
  if (!e) {
    return true;
  }
  GTEXT_CSS_String pseudo;
  (void)gtext_anim_effect_get_target(e, &out->element, &pseudo);
  out->pseudo_element = pseudo;
  GTEXT_CSS_String easing;
  (void)gtext_anim_effect_get_timing(e, &out->timing, &easing);
  out->easing = easing;
  out->composite = gtext_anim_effect_get_composite(e);
  (void)gtext_anim_effect_get_computed_timing(e, now, &out->computed);
  /* "An animation is relevant if its associated effect is current or in effect, and its replace state is not removed" */
  const GTEXT_Anim_Computed_Timing * c2 = &out->computed;
  const bool in_play = c2->phase == GTEXT_ANIM_PHASE_ACTIVE && s.play_state != GTEXT_ANIM_PLAY_FINISHED;
  const bool current = in_play || (c2->phase == GTEXT_ANIM_PHASE_BEFORE && s.playback_rate > 0) || (c2->phase == GTEXT_ANIM_PHASE_AFTER && s.playback_rate < 0);
  *relevant = (current || c2->in_effect) && s.replace_state != GTEXT_ANIM_REPLACE_REMOVED;
  return true;
}

/* ---- removing replaced animations ------------------------------------------------------------------------------------------------------------------- */

/// Call `fn` for each longhand a keyframe property sets: the property itself, or the longhands (and those the shorthand resets) under a shorthand, nested ones through the stack; a custom property by name.
typedef void (*prop_fn)(void * user, GTEXT_CSS_Property_Id id, const char * name, size_t length);

static void each_longhand(const GTEXT_CSS_Registry * reg, GTEXT_CSS_Property_Id id, const char * name, size_t length, prop_fn fn, void * user) {
  if (length > 2 && name[0] == '-' && name[1] == '-') {
    fn(user, 0, name, length);
    return;
  }
  if (!id) {
    return;
  }
  GTEXT_CSS_Property_Id stack[64];
  size_t top = 0;
  stack[top++] = gtext_css_registry_resolve_alias(reg, id);
  while (top) {
    const GTEXT_CSS_Property_Id cur = stack[--top];
    GTEXT_CSS_Property_Info pi;
    if (!gtext_css_registry_property_info(reg, cur, &pi)) {
      continue;
    }
    if (!(pi.flags & GTEXT_CSS_PROP_SHORTHAND)) {
      if (pi.animation != GTEXT_CSS_ANIM_NOT_ANIMATABLE) {
        fn(user, cur, NULL, 0);
      }
      continue;
    }
    if (pi.name_length == 3 && memcmp(pi.name, "all", 3) == 0) {
      continue;
    }
    for (int pass = 0; pass < 2; pass++) {
      for (size_t i = 0;; i++) {
        const GTEXT_CSS_Property_Id lh = pass == 0 ? gtext_css_registry_longhand(reg, cur, i) : gtext_css_registry_reset_longhand(reg, cur, i);
        if (!lh) {
          break;
        }
        if (top < sizeof stack / sizeof *stack) {
          stack[top++] = lh;
        }
      }
    }
  }
}

/// A set of target properties.
typedef struct {
  GTEXT_CSS_Property_Id * ids;
  size_t n, cap;
  char ** names;
  size_t * name_lengths;
  size_t nnames, capnames;
  bool oom;
  const GTEXT_Allocator * alloc;
} prop_set;

static void set_add(void * user, GTEXT_CSS_Property_Id id, const char * name, size_t length) {
  prop_set * s = user;
  if (s->oom) {
    return;
  }
  if (id) {
    for (size_t i = 0; i < s->n; i++) {
      if (s->ids[i] == id) {
        return;
      }
    }
    if (s->n == s->cap) {
      const size_t cap = s->cap ? s->cap * 2 : 16;
      GTEXT_CSS_Property_Id * bigger = gtext_allocator_realloc(s->alloc, s->ids, cap * sizeof *bigger);
      if (!bigger) {
        s->oom = true;
        return;
      }
      s->ids = bigger;
      s->cap = cap;
    }
    s->ids[s->n++] = id;
    return;
  }
  for (size_t i = 0; i < s->nnames; i++) {
    if (s->name_lengths[i] == length && memcmp(s->names[i], name, length) == 0) {
      return;
    }
  }
  if (s->nnames == s->capnames) {
    const size_t cap = s->capnames ? s->capnames * 2 : 8;
    char ** bn = gtext_allocator_realloc(s->alloc, s->names, cap * sizeof *bn);
    if (!bn) {
      s->oom = true;
      return;
    }
    s->names = bn;
    size_t * bl = gtext_allocator_realloc(s->alloc, s->name_lengths, cap * sizeof *bl);
    if (!bl) {
      s->oom = true;
      return;
    }
    s->name_lengths = bl;
    s->capnames = cap;
  }
  char * copy = gtext_allocator_malloc(s->alloc, length + 1);
  if (!copy) {
    s->oom = true;
    return;
  }
  memcpy(copy, name, length);
  copy[length] = '\0';
  s->names[s->nnames] = copy;
  s->name_lengths[s->nnames++] = length;
}

static void set_free(prop_set * s) {
  for (size_t i = 0; i < s->nnames; i++) {
    gtext_allocator_free(s->alloc, s->names[i]);
  }
  gtext_allocator_free(s->alloc, s->ids);
  gtext_allocator_free(s->alloc, s->names);
  gtext_allocator_free(s->alloc, s->name_lengths);
  memset(s, 0, sizeof *s);
}

static bool set_has(const prop_set * s, GTEXT_CSS_Property_Id id, const char * name, size_t length) {
  if (id) {
    for (size_t i = 0; i < s->n; i++) {
      if (s->ids[i] == id) {
        return true;
      }
    }
    return false;
  }
  for (size_t i = 0; i < s->nnames; i++) {
    if (s->name_lengths[i] == length && memcmp(s->names[i], name, length) == 0) {
      return true;
    }
  }
  return false;
}

/// The target properties of an effect: the longhands its keyframes set.
static GTEXT_Anim_Status effect_properties(GTEXT_Anim_World * w, GTEXT_Anim_Effect * e, prop_set * out) {
  memset(out, 0, sizeof *out);
  out->alloc = &w->alloc;
  GTEXT_Anim_Keyframes * k = NULL;
  GTEXT_Anim_Status st = gtext_anim_effect_get_keyframes(e, &k);
  if (st != GTEXT_ANIM_OK) {
    return st;
  }
  const GTEXT_CSS_Registry * reg = anim_world_registry(w);
  for (size_t i = 0; i < gtext_anim_keyframes_count(k); i++) {
    GTEXT_Anim_Keyframe_Info ki;
    (void)gtext_anim_keyframes_at(k, i, &ki);
    for (size_t j = 0; j < ki.property_count; j++) {
      GTEXT_CSS_String name;
      (void)gtext_anim_keyframes_property_at(k, i, j, &name, NULL, NULL);
      each_longhand(reg, gtext_css_registry_find_property(reg, name.data, name.length), name.data, name.length, set_add, out);
    }
  }
  gtext_anim_keyframes_free(k);
  if (out->oom) {
    set_free(out);
    return GTEXT_ANIM_OOM;
  }
  return GTEXT_ANIM_OK;
}

/// Whether an animation is replaceable apart from the effect stacks ("The animation's play state is finished ... replace state is not removed ... a monotonically increasing timeline ... an associated effect
/// that is in effect and has an effect target").
static bool replaceable(const GTEXT_Anim_Animation * a, GTEXT_Anim_Effect * e, GTEXT_Anim_Time now) {
  if (!e || !anim_effect_target(e)) {
    return false;
  }
  GTEXT_Anim_Animation_State s;
  if (gtext_anim_animation_state(a, now, &s) != GTEXT_ANIM_OK || s.play_state != GTEXT_ANIM_PLAY_FINISHED || s.replace_state == GTEXT_ANIM_REPLACE_REMOVED || s.timeline != GTEXT_ANIM_TIMELINE_DOCUMENT) {
    return false;
  }
  GTEXT_Anim_Computed_Timing c;
  return gtext_anim_effect_get_computed_timing(e, now, &c) == GTEXT_ANIM_OK && c.in_effect;
}

GTEXT_Anim_Status anim_script_reserve_remove(GTEXT_Anim_World * w) {
  for (size_t i = 0; i < w->ncontrols; i++) {
    if (script_control(w, i)) {
      const GTEXT_Anim_Status st = gtext_anim_animation_reserve_calls(w->controls[i].animation, 3);
      if (st != GTEXT_ANIM_OK) {
        return st;
      }
    }
  }
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status anim_script_remove_replaced(GTEXT_Anim_World * w, bool * changed) {
  *changed = false;
  const GTEXT_Anim_Time now = anim_world_now(w);
  GTEXT_Anim_Status result = GTEXT_ANIM_OK;
  for (size_t i = 0; i < w->ncontrols && result == GTEXT_ANIM_OK; i++) {
    control_t * c = script_control(w, i);
    if (!c) {
      continue;
    }
    GTEXT_Anim_Effect * e = gtext_anim_animation_effect(c->animation);
    GTEXT_Anim_Animation_State s;
    if (!replaceable(c->animation, e, now) || gtext_anim_animation_state(c->animation, now, &s) != GTEXT_ANIM_OK || s.replace_state != GTEXT_ANIM_REPLACE_ACTIVE) {
      continue;
    }
    prop_set mine;
    result = effect_properties(w, e, &mine);
    if (result != GTEXT_ANIM_OK) {
      break;
    }
    /* "for which there exists for each target property of every animation effect associated with animation, an animation effect associated with a replaceable animation with a higher composite order that
       includes the same target property": the effects of the same target, above this animation in the stack */
    bool all = mine.n + mine.nnames > 0;
    size_t ek = 0;
    const char * key = anim_effect_key(e, &ek);
    for (size_t pi = 0; pi < mine.n + mine.nnames && all; pi++) {
      const bool is_custom = pi >= mine.n;
      const GTEXT_CSS_Property_Id id = is_custom ? 0 : mine.ids[pi];
      const char * nm = is_custom ? mine.names[pi - mine.n] : NULL;
      const size_t nl = is_custom ? mine.name_lengths[pi - mine.n] : 0;
      bool covered = false;
      for (size_t j = i + 1; j < w->ncontrols && !covered; j++) {
        control_t * d = script_control(w, j);
        if (!d) {
          continue;
        }
        GTEXT_Anim_Effect * f = gtext_anim_animation_effect(d->animation);
        size_t fk = 0;
        const char * fkey = f ? anim_effect_key(f, &fk) : "";
        if (!replaceable(d->animation, f, now) || anim_effect_target(f) != anim_effect_target(e) || fk != ek || (ek && memcmp(fkey, key, ek) != 0)) {
          continue;
        }
        prop_set theirs;
        const GTEXT_Anim_Status ts = effect_properties(w, f, &theirs);
        if (ts != GTEXT_ANIM_OK) {
          result = ts;
          break;
        }
        covered = set_has(&theirs, id, nm, nl);
        set_free(&theirs);
      }
      all = covered && result == GTEXT_ANIM_OK;
    }
    set_free(&mine);
    if (result == GTEXT_ANIM_OK && all) {
      bool removed = false;
      result = gtext_anim_animation_remove_replaced(c->animation, now, &removed);
      *changed = *changed || removed;
    }
  }
  return result;
}

/* ---- commitStyles() ------------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  char * name;
  size_t name_length;
  char * value;
  size_t value_length;
} style_value;

struct GTEXT_Anim_Style_Values {
  GTEXT_Allocator alloc;
  const void * element;
  style_value * items;
  size_t n;
};

size_t gtext_anim_style_values_count(const GTEXT_Anim_Style_Values * list) {
  return list ? list->n : 0;
}

const void * gtext_anim_style_values_element(const GTEXT_Anim_Style_Values * list) {
  return list ? list->element : NULL;
}

bool gtext_anim_style_values_at(const GTEXT_Anim_Style_Values * list, size_t index, GTEXT_CSS_String * name, GTEXT_CSS_String * value) {
  if (!list || index >= list->n) {
    return false;
  }
  if (name) {
    *name = (GTEXT_CSS_String){list->items[index].name, list->items[index].name_length};
  }
  if (value) {
    *value = (GTEXT_CSS_String){list->items[index].value, list->items[index].value_length};
  }
  return true;
}

void gtext_anim_style_values_free(GTEXT_Anim_Style_Values * list) {
  if (!list) {
    return;
  }
  GTEXT_Allocator a = list->alloc;
  for (size_t i = 0; i < list->n; i++) {
    gtext_allocator_free(&a, list->items[i].name);
    gtext_allocator_free(&a, list->items[i].value);
  }
  gtext_allocator_free(&a, list->items);
  gtext_allocator_free(&a, list);
}

static char * copy_text(const GTEXT_Allocator * a, const char * s, size_t n) {
  char * p = gtext_allocator_malloc(a, n + 1);
  if (p) {
    memcpy(p, s, n);
    p[n] = '\0';
  }
  return p;
}

/// The serialized computed value of a longhand in a style (a custom property by name).
static GTEXT_Anim_Status style_value_text(GTEXT_Anim_World * w, const GTEXT_CSS_Computed_Style * style, GTEXT_CSS_Property_Id id, const char * name, size_t name_length, char ** out, size_t * out_length,
    bool * present) {
  *present = false;
  GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
  so.allocator = &w->alloc;
  char * text = NULL;
  size_t n = 0;
  GTEXT_CSS_Status st;
  if (id) {
    const GTEXT_CSS_Value * root = gtext_css_computed_root(style, id);
    if (!root) {
      return GTEXT_ANIM_OK;
    }
    st = gtext_css_serialize_value_buffer(root, &so, &text, &n);
  }
  else {
    const GTEXT_CSS_Custom_Value * cv = NULL;
    if (!gtext_css_computed_custom_find(style, name, name_length, &cv) || !cv) {
      return GTEXT_ANIM_OK;
    }
    st = gtext_css_custom_value_text(cv, &so, &text, &n);
  }
  if (st != GTEXT_CSS_OK) {
    return st == GTEXT_CSS_E_OOM ? GTEXT_ANIM_OOM : GTEXT_ANIM_INVALID;
  }
  *out = copy_text(&w->alloc, text, n);
  gtext_css_serialize_buffer_free(&so, text);
  if (!*out) {
    return GTEXT_ANIM_OOM;
  }
  *out_length = n;
  *present = true;
  return GTEXT_ANIM_OK;
}

/// Whether a style says `display: none`.
static bool style_is_display_none(const GTEXT_CSS_Registry * reg, const GTEXT_CSS_Computed_Style * style) {
  const GTEXT_CSS_Property_Id display = gtext_css_registry_find_property(reg, "display", 7);
  const GTEXT_CSS_Value * root = display ? gtext_css_computed_root(style, display) : NULL;
  return root && root->kind == GTEXT_CSS_VAL_KEYWORD && root->text.length == 4 && memcmp(root->text.data, "none", 4) == 0;
}

/// "Being rendered" for an element ("If, after applying any pending style changes, target is not being rendered"): it has a box, so neither it nor any ancestor has `display: none`; an element with
/// `display: contents` is rendered (the assumption of the procedure). `*rendered` false for an element the style cannot be made for.
static GTEXT_Anim_Status element_rendered(GTEXT_Anim_World * w, const void * element, bool * rendered) {
  *rendered = true;
  css_core * core = anim_world_core(w);
  const GTEXT_CSS_Element_Interface * iface = core->config.element_interface;
  const void * e = element;
  for (size_t guard = 0; e && guard < 1000000; guard++) {
    GTEXT_CSS_Computed_Style * style = NULL;
    const GTEXT_CSS_Status cs = gtext_cssom_document_after_change_style(w->doc, e, NULL, 0, &style);
    if (cs != GTEXT_CSS_OK) {
      return cs == GTEXT_CSS_E_OOM ? GTEXT_ANIM_OOM : GTEXT_ANIM_INVALID;
    }
    const bool none = !style || style_is_display_none(anim_world_registry(w), style);
    gtext_css_computed_free(style);
    if (none) {
      *rendered = false;
      return GTEXT_ANIM_OK;
    }
    const void * up = NULL;
    GTEXT_CSS_Element_Result er;
    memset(&er, 0, sizeof er);
    if (!iface || gtext_css_element_get_parent(iface, e, &up, &er) != GTEXT_CSS_OK) {
      break;
    }
    e = up;
  }
  return GTEXT_ANIM_OK;
}

GTEXT_Anim_Status gtext_anim_world_commit_styles(GTEXT_Anim_World * w, const GTEXT_Anim_Animation * animation, GTEXT_Anim_Style_Values ** out) {
  if (!w || !animation || !out) {
    return GTEXT_ANIM_INVALID;
  }
  *out = NULL;
  const control_t * c = NULL;
  for (size_t i = 0; i < w->ncontrols; i++) {
    if (w->controls[i].animation == animation) {
      c = &w->controls[i];
    }
  }
  if (!c) {
    return GTEXT_ANIM_INVALID;
  }
  GTEXT_Anim_Effect * e = gtext_anim_animation_effect(animation);
  GTEXT_Anim_Style_Values * list = gtext_allocator_calloc(&w->alloc, 1, sizeof *list);
  if (!list) {
    return GTEXT_ANIM_OOM;
  }
  list->alloc = w->alloc;
  const void * element = e ? anim_effect_target(e) : NULL;
  if (!e || !element) {
    *out = list;   /* no effect, or one that targets nothing: the set of targets is empty */
    return GTEXT_ANIM_OK;
  }
  size_t kl = 0;
  (void)anim_effect_key(e, &kl);
  GTEXT_Anim_Status st = GTEXT_ANIM_OK;
  prop_set props;
  memset(&props, 0, sizeof props);
  GTEXT_CSS_Computed_Style * style = NULL;
  if (kl) {
    st = GTEXT_ANIM_NO_MODIFICATION_ALLOWED;   /* "If target is not an element capable of having a style attribute (for example, it is a pseudo-element ...) throw a NoModificationAllowedError" */
  }
  bool rendered = true;
  if (st == GTEXT_ANIM_OK) {
    st = element_rendered(w, element, &rendered);
    if (st == GTEXT_ANIM_OK && !rendered) {
      st = GTEXT_ANIM_INVALID_STATE;   /* "If, after applying any pending style changes, target is not being rendered, throw an InvalidStateError" */
    }
  }
  if (st == GTEXT_ANIM_OK) {
    st = effect_properties(w, e, &props);
  }
  if (st == GTEXT_ANIM_OK) {
    /* "Let |partialEffectStack| be a copy of the effect stack ... remove any effects whose animation has a higher composite order ... endpoint-inclusive": the style is calculated with the hook of the sampler told so */
    w->commit.active = true;
    w->commit.element = element;
    w->commit.limit = c->id;
    w->commit.self = animation;
    const GTEXT_CSS_Status cs = gtext_cssom_document_after_change_style(w->doc, element, NULL, 0, &style);
    w->commit.active = false;
    if (cs != GTEXT_CSS_OK) {
      st = cs == GTEXT_CSS_E_OOM ? GTEXT_ANIM_OOM : GTEXT_ANIM_INVALID;
    }
    else if (!style) {
      st = GTEXT_ANIM_INVALID_STATE;
    }
  }
  if (st == GTEXT_ANIM_OK) {
    const GTEXT_CSS_Registry * reg = anim_world_registry(w);
    const GTEXT_CSS_Flow flow = gtext_css_computed_flow(style);
    const size_t total = props.n + props.nnames;
    list->items = gtext_allocator_calloc(&w->alloc, total ? total : 1, sizeof *list->items);
    if (!list->items) {
      st = GTEXT_ANIM_OOM;
    }
    for (size_t i = 0; i < total && st == GTEXT_ANIM_OK; i++) {
      const bool custom = i >= props.n;
      GTEXT_CSS_Property_Id id = custom ? 0 : props.ids[i];
      const char * name = custom ? props.names[i - props.n] : NULL;
      size_t nl = custom ? props.name_lengths[i - props.n] : 0;
      /* "the set of physical longhand properties": a flow-relative longhand is its physical equivalent in the target's flow */
      GTEXT_CSS_Logical_Member lm;
      GTEXT_CSS_Property_Id physical = 0;
      if (!custom && gtext_css_logical_member(reg, id, &lm) == GTEXT_CSS_OK && lm.logical && gtext_css_logical_map(reg, id, &flow, &physical) == GTEXT_CSS_OK && physical) {
        id = physical;
      }
      GTEXT_CSS_Property_Info pi;
      if (!custom) {
        if (!gtext_css_registry_property_info(reg, id, &pi)) {
          continue;
        }
        name = pi.name;
        nl = pi.name_length;
      }
      bool dup = false;
      for (size_t j = 0; j < list->n; j++) {
        dup = dup || (list->items[j].name_length == nl && memcmp(list->items[j].name, name, nl) == 0);
      }
      if (dup) {
        continue;
      }
      char * text = NULL;
      size_t tl = 0;
      bool present = false;
      st = style_value_text(w, style, id, name, nl, &text, &tl, &present);
      if (st != GTEXT_ANIM_OK || !present) {
        continue;
      }
      style_value * item = &list->items[list->n];
      item->name = copy_text(&w->alloc, name, nl);
      item->name_length = nl;
      item->value = text;
      item->value_length = tl;
      if (!item->name) {
        gtext_allocator_free(&w->alloc, text);
        item->value = NULL;
        st = GTEXT_ANIM_OOM;
        break;
      }
      list->n++;
    }
  }
  set_free(&props);
  gtext_css_computed_free(style);
  if (st != GTEXT_ANIM_OK) {
    gtext_anim_style_values_free(list);
    return st;
  }
  /* the declarations are in the order of the code points of their names (the specification's "ordered set" has no order of its own) */
  for (size_t i = 1; i < list->n; i++) {
    for (size_t j = i; j > 0; j--) {
      style_value * x = &list->items[j - 1];
      style_value * y = &list->items[j];
      const size_t m = x->name_length < y->name_length ? x->name_length : y->name_length;
      const int cmp = memcmp(x->name, y->name, m);
      if (cmp < 0 || (cmp == 0 && x->name_length <= y->name_length)) {
        break;
      }
      const style_value t = *x;
      *x = *y;
      *y = t;
    }
  }
  list->element = element;
  *out = list;
  return GTEXT_ANIM_OK;
}
