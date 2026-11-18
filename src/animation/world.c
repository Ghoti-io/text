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
 * The animations of a document (animation/world.h): `getAnimations()` over the tracker's transitions and CSS animations in the composite order, and the frame that returns the events they owe.
 */

#include <ghoti.io/text/animation/world.h>

#include <string.h>

#include "../css/cssom_computed_internal.h"
#include "../css/cssom_doc_internal.h"

struct GTEXT_Anim_World {
  GTEXT_Allocator alloc;
  GTEXT_CSSOM_Document * doc;
  GTEXT_CSSOM_Transition_Tracker * tracker;
  GTEXT_CSSOM_Transition_Events * stash;   ///< The transition events of a frame that could not be completed: the next frame reports them, so that a frame is whole or not at all.
  GTEXT_Anim_Events * stash_animations;    ///< The same for the events of the CSS animations.
};

struct GTEXT_Anim_List {
  GTEXT_Allocator alloc;
  GTEXT_Anim_Info * items;
  size_t n;
};

/* ---- the list ----------------------------------------------------------------------------------------------------------------------------------- */

size_t gtext_anim_list_count(const GTEXT_Anim_List * list) {
  return list ? list->n : 0;
}

bool gtext_anim_list_at(const GTEXT_Anim_List * list, size_t index, GTEXT_Anim_Info * out) {
  if (!list || !out || index >= list->n) {
    return false;
  }
  *out = list->items[index];
  return true;
}

void gtext_anim_list_free(GTEXT_Anim_List * list) {
  if (!list) {
    return;
  }
  GTEXT_Allocator a = list->alloc;
  gtext_allocator_free(&a, list->items);
  gtext_allocator_free(&a, list);
}

GTEXT_Anim_Status gtext_anim_world_new(GTEXT_CSSOM_Document * document, GTEXT_CSSOM_Transition_Tracker * tracker, const GTEXT_Allocator * allocator, GTEXT_Anim_World ** out) {
  if (!document || !tracker || !out) {
    return GTEXT_ANIM_INVALID;
  }
  *out = NULL;
  GTEXT_Allocator a = allocator ? *allocator : *gtext_allocator_default();
  GTEXT_Anim_World * w = gtext_allocator_calloc(&a, 1, sizeof *w);
  if (!w) {
    return GTEXT_ANIM_OOM;
  }
  w->alloc = a;
  w->doc = document;
  w->tracker = tracker;
  *out = w;
  return GTEXT_ANIM_OK;
}

void gtext_anim_world_free(GTEXT_Anim_World * world) {
  if (!world) {
    return;
  }
  gtext_cssom_transition_events_free(world->stash);
  gtext_anim_events_free(world->stash_animations);
  GTEXT_Allocator a = world->alloc;
  gtext_allocator_free(&a, world);
}

double gtext_anim_world_time(const GTEXT_Anim_World * world) {
  return world ? css_tracker_time(world->tracker) : 0.0;
}

/* ---- getAnimations() ---------------------------------------------------------------------------------------------------------------------------- */

/// One animation with what its place in the composite order needs.
typedef struct {
  GTEXT_Anim_Info info;
  uint64_t generation;      ///< A transition's generation.
  size_t order;             ///< An animation's place in `animation-name`.
  uint64_t seq;             ///< The order the tracker first saw the element in.
  int pseudo_rank;
} rec_t;

typedef struct {
  const GTEXT_CSS_Element_Interface * iface;
} sort_ctx;

/// Whether `a` sorts after `b` in the composite order.
static bool rec_after(const sort_ctx * c, const rec_t * a, const rec_t * b) {
  if (a->info.kind != b->info.kind) {
    return a->info.kind > b->info.kind;   /* "CSS Animations with an owning element have a later composite order than CSS Transitions" */
  }
  if (a->info.element != b->info.element) {
    int cmp = 0;
    GTEXT_CSS_Element_Result er;
    memset(&er, 0, sizeof er);
    if (c->iface && gtext_css_element_compare_order(c->iface, a->info.element, b->info.element, &cmp, &er) == GTEXT_CSS_OK && cmp != 0) {
      return cmp > 0;   /* "by tree order of their corresponding owning elements" */
    }
    return a->seq > b->seq;
  }
  if (a->pseudo_rank != b->pseudo_rank) {
    return a->pseudo_rank > b->pseudo_rank;
  }
  if (a->info.pseudo_element.length != b->info.pseudo_element.length || (a->info.pseudo_element.length && memcmp(a->info.pseudo_element.data, b->info.pseudo_element.data, a->info.pseudo_element.length))) {
    const size_t m = a->info.pseudo_element.length < b->info.pseudo_element.length ? a->info.pseudo_element.length : b->info.pseudo_element.length;
    const int cmp = m ? memcmp(a->info.pseudo_element.data, b->info.pseudo_element.data, m) : 0;
    return cmp ? cmp > 0 : a->info.pseudo_element.length > b->info.pseudo_element.length;
  }
  if (a->info.kind == GTEXT_ANIM_KIND_CSS_ANIMATION) {
    return a->order > b->order;
  }
  if (a->generation != b->generation) {
    return a->generation > b->generation;   /* "sort ... in ascending order by the transition generation" */
  }
  const size_t m = a->info.name.length < b->info.name.length ? a->info.name.length : b->info.name.length;
  const int cmp = m ? memcmp(a->info.name.data, b->info.name.data, m) : 0;
  if (cmp) {
    return cmp > 0;
  }
  return a->info.name.length != b->info.name.length ? a->info.name.length > b->info.name.length : a->info.id > b->info.id;
}

/// A stable merge sort of the records (the temporary array is the caller's).
static void rec_sort(const sort_ctx * c, rec_t * a, rec_t * tmp, size_t n) {
  for (size_t width = 1; width < n; width *= 2) {
    for (size_t lo = 0; lo < n; lo += 2 * width) {
      const size_t mid = lo + width < n ? lo + width : n;
      const size_t hi = lo + 2 * width < n ? lo + 2 * width : n;
      size_t i = lo, j = mid, k = lo;
      while (i < mid && j < hi) {
        tmp[k++] = rec_after(c, &a[i], &a[j]) ? a[j++] : a[i++];
      }
      while (i < mid) {
        tmp[k++] = a[i++];
      }
      while (j < hi) {
        tmp[k++] = a[j++];
      }
    }
    memcpy(a, tmp, n * sizeof *a);
  }
}

/// Whether `element` is `root` or inside it, by the parents the element interface gives.
static bool inside(const GTEXT_CSS_Element_Interface * iface, const void * element, const void * root) {
  const void * e = element;
  for (size_t guard = 0; e && guard < 1000000; guard++) {
    if (e == root) {
      return true;
    }
    const void * up = NULL;
    GTEXT_CSS_Element_Result er;
    memset(&er, 0, sizeof er);
    if (gtext_css_element_get_parent(iface, e, &up, &er) != GTEXT_CSS_OK) {
      return false;
    }
    e = up;
  }
  return false;
}

/// The name of a pseudo-element as an event has it: with two colons.
static size_t normalize_pseudo(const char * pseudo, size_t length, char * buf, size_t cap) {
  size_t extra = length == 0 ? 0 : (pseudo[0] == ':' ? (length > 1 && pseudo[1] == ':' ? 0 : 1) : 2);
  if (length + extra > cap) {
    return (size_t)-1;
  }
  for (size_t i = 0; i < extra; i++) {
    buf[i] = ':';
  }
  if (length) {
    memcpy(buf + extra, pseudo, length);
  }
  return length + extra;
}

static GTEXT_Anim_Status collect(GTEXT_Anim_World * world, const void * element, const char * pseudo, size_t pseudo_length, bool subtree, bool relevant_only, GTEXT_Anim_List ** out) {
  if (!world || !out || (!pseudo && pseudo_length)) {
    return GTEXT_ANIM_INVALID;
  }
  *out = NULL;
  css_core * core = css_document_core(world->doc);
  const GTEXT_CSS_Element_Interface * iface = core->config.element_interface;
  char want[96];
  const size_t want_length = element ? normalize_pseudo(pseudo ? pseudo : "", pseudo_length, want, sizeof want) : 0;
  if (want_length == (size_t)-1) {
    return GTEXT_ANIM_INVALID;
  }
  const double now = css_tracker_time(world->tracker);
  const GTEXT_CSS_Compute_Context * ctx = NULL;
  GTEXT_CSS_Status cs = css_tracker_context(world->tracker, &ctx);
  if (cs != GTEXT_CSS_OK) {
    return cs == GTEXT_CSS_E_OOM ? GTEXT_ANIM_OOM : GTEXT_ANIM_INVALID;
  }
  const size_t entries = css_tracker_entry_count(world->tracker);
  size_t cap = 0, n = 0;
  rec_t * recs = NULL;
  GTEXT_Anim_Status result = GTEXT_ANIM_OK;
  for (size_t ei = 0; ei < entries && result == GTEXT_ANIM_OK; ei++) {
    css_tracker_entry_view v;
    if (!css_tracker_entry_at(world->tracker, ei, &v)) {
      continue;
    }
    if (element) {
      const bool same_target = v.element == element && v.pseudo_length == want_length && (want_length == 0 || memcmp(v.pseudo, want, want_length) == 0);
      const bool below = subtree && want_length == 0 && iface && v.element != element && inside(iface, v.element, element);
      const bool own_pseudo = subtree && want_length == 0 && v.element == element;
      if (!same_target && !below && !own_pseudo) {
        continue;
      }
    }
    const size_t nt = gtext_css_transition_set_running_count(v.transitions);
    const size_t na = gtext_anim_css_set_count(v.animations);
    if (n + nt + na > cap) {
      size_t grown = cap ? cap * 2 : 16;
      while (grown < n + nt + na) {
        grown *= 2;
      }
      rec_t * bigger = gtext_allocator_realloc(&world->alloc, recs, grown * sizeof *bigger);
      if (!bigger) {
        result = GTEXT_ANIM_OOM;
        break;
      }
      recs = bigger;
      cap = grown;
    }
    for (size_t j = 0; j < nt && result == GTEXT_ANIM_OK; j++) {
      GTEXT_CSS_Transition_Animation ta;
      if (gtext_css_transition_set_animation_at(ctx, v.transitions, j, now, &ta) != GTEXT_CSS_OK) {
        result = GTEXT_ANIM_OOM;
        break;
      }
      rec_t * r = &recs[n];
      memset(r, 0, sizeof *r);
      GTEXT_Anim_Effect_Timing t = gtext_anim_timing_default();
      t.delay = ta.delay_ms;
      t.fill = GTEXT_ANIM_FILL_BACKWARDS;   /* "the transition effect has fill backwards and one iteration" */
      t.duration_auto = false;
      t.duration = ta.duration_ms;
      GTEXT_CSS_Easing * easing = NULL;
      const GTEXT_CSS_Registry * registry = gtext_css_compute_context_registry(ctx);
      const GTEXT_Anim_Status es = gtext_anim_easing_parse(registry, ta.easing.data, ta.easing.length, &world->alloc, &easing);
      if (es != GTEXT_ANIM_OK) {
        result = es == GTEXT_ANIM_OOM ? GTEXT_ANIM_OOM : GTEXT_ANIM_INVALID;
        break;
      }
      t.easing = easing;
      const GTEXT_Anim_Time current = {true, ta.current_time_ms};
      (void)gtext_anim_computed_timing(&t, current, 1.0, false, &r->info.computed);
      t.easing = NULL;
      gtext_css_easing_free(easing);
      r->info.kind = GTEXT_ANIM_KIND_CSS_TRANSITION;
      r->info.id = ta.id;
      r->info.element = v.element;
      r->info.pseudo_element = (GTEXT_CSS_String){v.pseudo, v.pseudo_length};
      r->info.name = ta.transition_property;
      r->info.pending = ta.pending;
      r->info.start_time = (GTEXT_Anim_Time){ta.start_time_resolved, ta.start_time_resolved ? ta.start_time_ms : 0.0};
      r->info.current_time = current;
      r->info.timing = t;
      r->info.easing = ta.easing;
      r->info.start_value = ta.start_value;
      r->info.end_value = ta.end_value;
      r->info.play_state = ta.pending || ta.current_time_ms < r->info.computed.end_time ? GTEXT_ANIM_PLAY_RUNNING : GTEXT_ANIM_PLAY_FINISHED;
      r->generation = ta.generation;
      r->seq = v.element_seq;
      r->pseudo_rank = v.pseudo_rank;
      const bool relevant = r->info.computed.in_effect || r->info.computed.phase == GTEXT_ANIM_PHASE_BEFORE;
      if (relevant || !relevant_only) {
        n++;
      }
    }
    for (size_t j = 0; j < na && result == GTEXT_ANIM_OK; j++) {
      GTEXT_Anim_Css_Info ai;
      if (gtext_anim_css_set_info(v.animations, j, now, &ai) != GTEXT_CSS_OK) {
        result = GTEXT_ANIM_INVALID;
        break;
      }
      if (relevant_only && !ai.relevant) {
        continue;
      }
      rec_t * r = &recs[n];
      memset(r, 0, sizeof *r);
      r->info.kind = GTEXT_ANIM_KIND_CSS_ANIMATION;
      r->info.id = ai.id;
      r->info.element = v.element;
      r->info.pseudo_element = (GTEXT_CSS_String){v.pseudo, v.pseudo_length};
      r->info.name = ai.animation_name;
      r->info.play_state = ai.play_state;
      r->info.pending = ai.pending;
      r->info.start_time = ai.start_time;
      r->info.current_time = ai.current_time;
      r->info.timing = ai.timing;
      r->info.timing.easing = NULL;
      r->info.easing = ai.easing;
      r->info.computed = ai.computed;
      r->order = ai.order;
      r->seq = v.element_seq;
      r->pseudo_rank = v.pseudo_rank;
      n++;
    }
  }
  rec_t * tmp = result == GTEXT_ANIM_OK && n > 1 ? gtext_allocator_malloc(&world->alloc, n * sizeof *tmp) : NULL;
  if (result == GTEXT_ANIM_OK && n > 1 && !tmp) {
    result = GTEXT_ANIM_OOM;
  }
  GTEXT_Anim_List * list = NULL;
  GTEXT_Anim_Info * items = NULL;
  if (result == GTEXT_ANIM_OK) {
    list = gtext_allocator_calloc(&world->alloc, 1, sizeof *list);
    items = gtext_allocator_malloc(&world->alloc, (n ? n : 1) * sizeof *items);
    if (!list || !items) {
      result = GTEXT_ANIM_OOM;
    }
  }
  if (result == GTEXT_ANIM_OK) {
    const sort_ctx c = {iface};
    if (n > 1) {
      rec_sort(&c, recs, tmp, n);
    }
    for (size_t i = 0; i < n; i++) {
      items[i] = recs[i].info;
    }
    list->alloc = world->alloc;
    list->items = items;
    list->n = n;
    *out = list;
  }
  else {
    gtext_allocator_free(&world->alloc, list);
    gtext_allocator_free(&world->alloc, items);
  }
  gtext_allocator_free(&world->alloc, tmp);
  gtext_allocator_free(&world->alloc, recs);
  return result;
}

GTEXT_Anim_Status gtext_anim_world_get_animations(GTEXT_Anim_World * world, const void * element, const char * pseudo, size_t pseudo_length, bool subtree, GTEXT_Anim_List ** out) {
  return collect(world, element, pseudo, pseudo_length, subtree, true, out);
}

GTEXT_Anim_Status gtext_anim_world_find(GTEXT_Anim_World * world, GTEXT_Anim_Kind kind, uint64_t id, bool * found, GTEXT_Anim_Info * out) {
  if (!world || !found || !out) {
    return GTEXT_ANIM_INVALID;
  }
  *found = false;
  GTEXT_Anim_List * list = NULL;
  const GTEXT_Anim_Status st = collect(world, NULL, NULL, 0, false, false, &list);
  if (st != GTEXT_ANIM_OK) {
    return st;
  }
  for (size_t i = 0; i < gtext_anim_list_count(list) && !*found; i++) {
    GTEXT_Anim_Info a;
    if (gtext_anim_list_at(list, i, &a) && a.kind == kind && a.id == id) {
      *out = a;
      *found = true;
    }
  }
  gtext_anim_list_free(list);
  return GTEXT_ANIM_OK;
}

/* ---- the frame ---------------------------------------------------------------------------------------------------------------------------------- */

static GTEXT_Anim_Event_Type map_type(GTEXT_CSS_Transition_Event_Type t) {
  switch (t) {
    case GTEXT_CSS_TRANSITION_EVENT_RUN:
      return GTEXT_ANIM_EVENT_TRANSITIONRUN;
    case GTEXT_CSS_TRANSITION_EVENT_START:
      return GTEXT_ANIM_EVENT_TRANSITIONSTART;
    case GTEXT_CSS_TRANSITION_EVENT_END:
      return GTEXT_ANIM_EVENT_TRANSITIONEND;
    default:
      return GTEXT_ANIM_EVENT_TRANSITIONCANCEL;
  }
}

GTEXT_Anim_Status gtext_anim_world_update(GTEXT_Anim_World * world, GTEXT_Anim_Events ** out) {
  if (!world || !out) {
    return GTEXT_ANIM_INVALID;
  }
  *out = NULL;
  /* the transitions' frame moves their sets on, so it is made once: if the animations' cannot be made, its events wait in the world for the next call */
  if (!world->stash) {
    const GTEXT_CSS_Status ts = gtext_cssom_transition_tracker_events(world->tracker, &world->stash);
    if (ts != GTEXT_CSS_OK) {
      return ts == GTEXT_CSS_E_OOM ? GTEXT_ANIM_OOM : GTEXT_ANIM_INVALID;
    }
  }
  if (!world->stash_animations) {
    const GTEXT_CSS_Status as = css_tracker_animation_events(world->tracker, &world->stash_animations);
    if (as != GTEXT_CSS_OK) {
      return as == GTEXT_CSS_E_OOM ? GTEXT_ANIM_OOM : GTEXT_ANIM_INVALID;
    }
  }
  GTEXT_Anim_Events * ae = world->stash_animations;
  GTEXT_Anim_Events * all = NULL;
  GTEXT_Anim_Status st = gtext_anim_events_new(&world->alloc, &all);
  const size_t nt = gtext_cssom_transition_events_count(world->stash), na = gtext_anim_events_count(ae);
  if (st == GTEXT_ANIM_OK) {
    size_t bytes = 0;
    for (size_t i = 0; i < nt; i++) {
      GTEXT_CSSOM_Transition_Event e;
      if (gtext_cssom_transition_events_at(world->stash, i, &e)) {
        bytes += e.pseudo_element.length + e.event.property_name.length;
      }
    }
    for (size_t i = 0; i < na; i++) {
      GTEXT_Anim_Event e;
      if (gtext_anim_events_at(ae, i, &e)) {
        bytes += e.pseudo_element.length + e.name.length;
      }
    }
    st = gtext_anim_events_reserve(all, nt + na, bytes);
  }
  if (st != GTEXT_ANIM_OK) {
    gtext_anim_events_free(all);
    return st;   /* both lists stay in the world: the next call reports them */
  }
  /* the transitions come first: "CSS Animations ... have a later composite order than CSS Transitions" */
  for (size_t i = 0; i < nt; i++) {
    GTEXT_CSSOM_Transition_Event e;
    (void)gtext_cssom_transition_events_at(world->stash, i, &e);
    GTEXT_Anim_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = map_type(e.event.type);
    ev.animation = e.event.transition;
    ev.element = e.element;
    ev.pseudo_element = e.pseudo_element;
    ev.name = e.event.property_name;
    ev.elapsed_time = e.event.elapsed_time;
    ev.scheduled_time = (GTEXT_Anim_Time){true, e.event.scheduled_time_ms};
    (void)gtext_anim_events_append(all, &ev);
  }
  for (size_t i = 0; i < na; i++) {
    GTEXT_Anim_Event ev;
    (void)gtext_anim_events_at(ae, i, &ev);
    (void)gtext_anim_events_append(all, &ev);
  }
  gtext_anim_events_sort(all);
  gtext_anim_events_free(world->stash_animations);
  world->stash_animations = NULL;
  gtext_cssom_transition_events_free(world->stash);
  world->stash = NULL;
  *out = all;
  return GTEXT_ANIM_OK;
}
