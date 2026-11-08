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
 * `getComputedStyle()` (css_cssom_computed.h): the computed declaration, the document's session, the style cache.
 *
 * **The declaration is a block of resolved values.** A computed declaration is an ordinary `GTEXT_CSSOM_Decl` whose block is made again, whole, whenever a read finds the pair
 * (generation, tree version) moved: `length`, `item`, `getPropertyValue`, `getPropertyPriority` and the typed value are the code every owner uses, over a block that
 * cssom_resolved.c made. Nothing is added to the read path but the validation at its start.
 *
 * **The chain.** The style of an element is computed from the style of its inheritance parent. A read walks up from the element until it meets a cached style (or the root),
 * then computes down, caching each, so that n reads over a chain of depth d cost d computes the first time and none after (`computed_computes`). The cache is dropped whole when
 * the pair moves, so no entry ever answers for a state it was not made in; it is bounded (`max_cached_styles`), the entry cached first going first, so that what it holds is
 * a function of the sequence of reads and never of a clock, and the answer never depends on it.
 */

#include <string.h>

#include <ghoti.io/text/css/css_container.h>
#include "css_compute_internal.h"
#include "css_internal.h"
#include "cssom_computed_internal.h"
#include "cssom_doc_internal.h"

#define DEFAULT_MAX_CACHED_STYLES 65536u
#define MAX_CHAIN 100000u

/* ---- the session ---------------------------------------------------------------------------------------------------------------------------------- */

css_cmps * css_cmps_get(css_core * core) {
  if (!core->cs) {
    core->cs = gtext_allocator_calloc(&core->alloc, 1, sizeof(css_cmps));
  }
  return core->cs;
}

static size_t max_cached(const css_core * core) {
  return core->config.max_cached_styles ? core->config.max_cached_styles : DEFAULT_MAX_CACHED_STYLES;
}

static size_t hash_element(const void * p) {
  uint64_t x = (uint64_t)(uintptr_t)p;
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  return (size_t)x;
}

static css_rec * rec_find(const css_cmps * cs, const void * element) {
  if (!cs->nbuckets) {
    return NULL;
  }
  for (css_rec * r = cs->buckets[hash_element(element) & (cs->nbuckets - 1)]; r; r = r->hnext) {
    if (r->element == element) {
      return r;
    }
  }
  return NULL;
}

static void rec_grow(css_core * core, css_cmps * cs) {
  const size_t cap = cs->nbuckets ? cs->nbuckets * 2 : 64;
  css_rec ** fresh = gtext_allocator_calloc(&core->alloc, cap, sizeof *fresh);
  if (!fresh) {
    return;   /* the chains just stay longer */
  }
  for (size_t i = 0; i < cs->nbuckets; i++) {
    css_rec * r = cs->buckets[i];
    while (r) {
      css_rec * next = r->hnext;
      const size_t at = hash_element(r->element) & (cap - 1);
      r->hnext = fresh[at];
      fresh[at] = r;
      r = next;
    }
  }
  gtext_allocator_free(&core->alloc, cs->buckets);
  cs->buckets = fresh;
  cs->nbuckets = cap;
}

/// The record of an element, made if there is none; NULL on memory.
static css_rec * rec_get(css_core * core, css_cmps * cs, const void * element) {
  css_rec * r = rec_find(cs, element);
  if (r) {
    return r;
  }
  if (cs->nrecs >= cs->nbuckets * 2) {
    rec_grow(core, cs);
  }
  if (!cs->nbuckets) {
    return NULL;
  }
  r = gtext_allocator_calloc(&core->alloc, 1, sizeof *r);
  if (!r) {
    return NULL;
  }
  r->element = element;
  const size_t at = hash_element(element) & (cs->nbuckets - 1);
  r->hnext = cs->buckets[at];
  cs->buckets[at] = r;
  cs->nrecs++;
  return r;
}

static void rec_unlink_free(css_core * core, css_cmps * cs, css_rec * r) {
  css_rec ** pp = &cs->buckets[hash_element(r->element) & (cs->nbuckets - 1)];
  while (*pp && *pp != r) {
    pp = &(*pp)->hnext;
  }
  if (*pp) {
    *pp = r->hnext;
  }
  cs->nrecs--;
  gtext_allocator_free(&core->alloc, r);
}

static void rec_maybe_free(css_core * core, css_cmps * cs, css_rec * r) {
  if (!r->style && !r->decls) {
    rec_unlink_free(core, cs, r);
  }
}

static void fifo_remove(css_cmps * cs, css_rec * r) {
  if (!r->in_fifo) {
    return;
  }
  if (r->fprev) {
    r->fprev->fnext = r->fnext;
  }
  else {
    cs->fifo_head = r->fnext;
  }
  if (r->fnext) {
    r->fnext->fprev = r->fprev;
  }
  else {
    cs->fifo_tail = r->fprev;
  }
  r->fprev = r->fnext = NULL;
  r->in_fifo = false;
}

static void rec_drop_style(css_cmps * cs, css_rec * r) {
  if (r->style) {
    gtext_css_computed_free(r->style);
    r->style = NULL;
    cs->nstyles--;
  }
  fifo_remove(cs, r);
}

/// Cache a style for an element (one count taken); the oldest entries go past the bound.
static void cache_put(css_core * core, css_cmps * cs, const void * element, GTEXT_CSS_Computed_Style * style) {
  css_rec * r = rec_get(core, cs, element);
  if (!r) {
    return;   /* not cached: the answer does not depend on it */
  }
  rec_drop_style(cs, r);
  r->style = gtext_css_computed_retain(style);
  cs->nstyles++;
  r->in_fifo = true;
  r->fprev = cs->fifo_tail;
  r->fnext = NULL;
  if (cs->fifo_tail) {
    cs->fifo_tail->fnext = r;
  }
  else {
    cs->fifo_head = r;
  }
  cs->fifo_tail = r;
  while (cs->nstyles > max_cached(core) && cs->fifo_head) {
    css_rec * old = cs->fifo_head;
    rec_drop_style(cs, old);
    core->counters.computed_cache_evictions++;
    if (old != r) {
      rec_maybe_free(core, cs, old);
    }
    else {
      break;   /* a bound of one: the entry just made stays only until the next */
    }
  }
}

static void cache_clear(css_core * core, css_cmps * cs) {
  while (cs->fifo_head) {
    css_rec * r = cs->fifo_head;
    rec_drop_style(cs, r);
    rec_maybe_free(core, cs, r);
  }
}

size_t css_computed_cached_styles(const css_core * core) {
  return core->cs ? core->cs->nstyles : 0;
}

/* ---- the registry of computed declarations -------------------------------------------------------------------------------------------------------- */

static void chain_link(css_core * core, css_cmps * cs, GTEXT_CSSOM_Decl * d, css_rec * rec) {
  css_comp * c = d->comp;
  c->rec = rec;
  c->rec_prev = NULL;
  c->rec_next = rec->decls;
  if (rec->decls) {
    rec->decls->comp->rec_prev = d;
  }
  rec->decls = d;
  c->all_prev = NULL;
  c->all_next = cs->all_head;
  if (cs->all_head) {
    cs->all_head->comp->all_prev = d;
  }
  cs->all_head = d;
  (void)core;
}

static void all_unlink(css_cmps * cs, GTEXT_CSSOM_Decl * d) {
  css_comp * c = d->comp;
  if (c->all_prev) {
    c->all_prev->comp->all_next = c->all_next;
  }
  else if (cs->all_head == d) {
    cs->all_head = c->all_next;
  }
  if (c->all_next) {
    c->all_next->comp->all_prev = c->all_prev;
  }
  c->all_prev = c->all_next = NULL;
}

static void rec_unchain(css_core * core, css_cmps * cs, GTEXT_CSSOM_Decl * d) {
  css_comp * c = d->comp;
  css_rec * rec = c->rec;
  if (!rec) {
    return;
  }
  if (c->rec_prev) {
    c->rec_prev->comp->rec_next = c->rec_next;
  }
  else {
    rec->decls = c->rec_next;
  }
  if (c->rec_next) {
    c->rec_next->comp->rec_prev = c->rec_prev;
  }
  c->rec_prev = c->rec_next = NULL;
  c->rec = NULL;
  rec_maybe_free(core, cs, rec);
}

/// Freeze a declaration: it keeps the declarations it has and reads nothing more.
static void comp_detach(css_core * core, css_cmps * cs, GTEXT_CSSOM_Decl * d) {
  css_comp * c = d->comp;
  c->detached = true;
  rec_unchain(core, cs, d);
}

void css_computed_forget(GTEXT_CSSOM_Decl * d) {
  css_comp * c = d->comp;
  if (!c) {
    return;
  }
  css_core * core = d->obj.core;
  css_cmps * cs = core->cs;
  if (cs) {
    rec_unchain(core, cs, d);
    all_unlink(cs, d);
  }
  gtext_css_selector_list_free(c->pseudo);
  gtext_allocator_free(&core->alloc, c->origins);
  gtext_allocator_free(&core->alloc, c->untimed_custom);
  gtext_allocator_free(&core->alloc, c->position);
  gtext_allocator_free(&core->alloc, c);
  d->comp = NULL;
}

static void release_now(css_core * core, css_cmps * cs, const void * element) {
  css_rec * r = rec_find(cs, element);
  if (!r) {
    return;
  }
  rec_drop_style(cs, r);
  while (r->decls) {
    GTEXT_CSSOM_Decl * d = r->decls;
    css_comp * c = d->comp;
    c->detached = true;
    /* unchain without freeing the record while it is walked */
    r->decls = c->rec_next;
    if (c->rec_next) {
      c->rec_next->comp->rec_prev = NULL;
    }
    c->rec_prev = c->rec_next = NULL;
    c->rec = NULL;
  }
  rec_maybe_free(core, cs, r);
}

void css_computed_element_released(css_core * core, const void * element) {
  css_cmps * cs = core->cs;
  if (!cs) {
    return;
  }
  if (core->computing == 0) {
    release_now(core, cs, element);
    return;
  }
  /* a callback of a read: the read holds its place in the records and the cache, and a style it makes after this call would be cached for the old element. It goes when the
     outermost read ends */
  if (cs->nreleased == cs->released_cap) {
    const size_t grown = cs->released_cap ? cs->released_cap * 2 : 8;
    const void ** bigger = gtext_allocator_realloc(&core->alloc, (void *)cs->released, grown * sizeof *bigger);
    if (!bigger) {
      cs->released_lost = true;
      return;
    }
    cs->released = bigger;
    cs->released_cap = grown;
  }
  cs->released[cs->nreleased++] = element;
}

/// The outermost read ended: the releases made during it take effect.
static void flush_released(css_core * core, css_cmps * cs) {
  if (cs->released_lost) {
    /* an element could not be written down: which records to drop is not known, so all of them go (every declaration is frozen, as at the release of the document) */
    cs->released_lost = false;
    cs->nreleased = 0;
    css_computed_teardown(core);
    return;
  }
  for (size_t i = 0; i < cs->nreleased; i++) {
    release_now(core, cs, cs->released[i]);
  }
  cs->nreleased = 0;
}

static void session_free_contexts(css_cmps * cs) {
  gtext_css_selector_match_context_free(cs->match);
  cs->match = NULL;
  if (cs->call_init) {
    gtext_css_condition_call_free(&cs->call);
    cs->call_init = false;
  }
  cs->pair_valid = false;
}

void css_computed_teardown(css_core * core) {
  css_cmps * cs = core->cs;
  if (!cs) {
    return;
  }
  cs->nreleased = 0;
  cs->released_lost = false;
  cache_clear(core, cs);
  while (cs->all_head) {
    GTEXT_CSSOM_Decl * d = cs->all_head;
    all_unlink(cs, d);
    comp_detach(core, cs, d);
  }
  session_free_contexts(cs);
  gtext_css_compute_context_free(cs->own);
  cs->own = NULL;
  cs->own_exported = false;
  cs->ctx = NULL;
}

void css_computed_teardown_soon(css_core * core) {
  if (core->computing > 0) {
    /* a callback of the caller's tree or layout released the handle in the middle of a read: the read goes on with the contexts and the cache it holds */
    core->computed_teardown_pending = true;
    return;
  }
  css_computed_teardown(core);
}

void css_computed_free(css_core * core) {
  css_cmps * cs = core->cs;
  if (!cs) {
    return;
  }
  css_computed_teardown(core);
  for (size_t i = 0; i < cs->nretired; i++) {
    gtext_css_compute_context_free(cs->retired[i].ctx);
    gtext_css_at_property_set_free(cs->retired[i].props);
    gtext_css_function_set_free(cs->retired[i].fns);
  }
  gtext_allocator_free(&core->alloc, cs->retired);
  gtext_allocator_free(&core->alloc, (void *)cs->released);
  gtext_allocator_free(&core->alloc, cs->buckets);
  gtext_allocator_free(&core->alloc, cs);
  core->cs = NULL;
}

GTEXT_CSS_Status gtext_cssom_document_set_compute_context(GTEXT_CSSOM_Document * document, const GTEXT_CSS_Compute_Context * context) {
  if (!document) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = css_document_core(document);
  if (core->computing > 0 || core->in_callback || core->in_finalizer) {
    return GTEXT_CSS_E_REENTRANT;   /* the read that is running holds the contexts this would free */
  }
  core->config.compute_context = context;
  css_cmps * cs = core->cs;
  if (!cs) {
    return GTEXT_CSS_OK;
  }
  /* every cached style belongs to the context that made it; the values a declaration holds are counted values and outlive it */
  cache_clear(core, cs);
  for (GTEXT_CSSOM_Decl * d = cs->all_head; d; d = d->comp->all_next) {
    d->comp->valid = false;
  }
  session_free_contexts(cs);
  gtext_css_compute_context_free(cs->own);
  cs->own = NULL;
  cs->own_exported = false;
  cs->ctx = NULL;
  cs->cepoch++;
  return GTEXT_CSS_OK;
}

void css_computed_properties_changed(css_core * core, GTEXT_CSS_At_Property_Set * props, GTEXT_CSS_Function_Set * fns) {
  GTEXT_CSS_At_Property_Set * old = core->props == props ? NULL : core->props;
  GTEXT_CSS_Function_Set * old_fns = core->fns == fns ? NULL : core->fns;
  core->props = props;
  core->fns = fns;
  css_cmps * cs = core->cs;
  if (!cs || core->config.compute_context) {
    /* no context of the document's own has been made, or the caller's is in use and keeps its own registrations: nothing was computed with the old set */
    gtext_css_at_property_set_free(old);
    gtext_css_function_set_free(old_fns);
    return;
  }
  /* every cached style belongs to the context that made it, and a computed declaration read before holds values computed with the old registrations */
  cache_clear(core, cs);
  for (GTEXT_CSSOM_Decl * d = cs->all_head; d; d = d->comp->all_next) {
    d->comp->valid = false;
  }
  session_free_contexts(cs);
  if (cs->own) {
    core->counters.property_context_rebuilds++;
    if (cs->own_exported && cs->nretired < cs->retired_cap) {
      /* a conditions object may hold the context (gtext_cssom_document_container_interface()): it stays valid, with the registrations it borrows, until the document goes; the room was
         reserved when it was handed out */
      cs->retired[cs->nretired].ctx = cs->own;
      cs->retired[cs->nretired].props = old;
      cs->retired[cs->nretired].fns = old_fns;
      cs->nretired++;
      old = NULL;
      old_fns = NULL;
    }
    else {
      gtext_css_compute_context_free(cs->own);
    }
  }
  gtext_css_at_property_set_free(old);
  gtext_css_function_set_free(old_fns);
  cs->own = NULL;
  cs->own_exported = false;
  cs->ctx = NULL;
  cs->cepoch++;
}

/* ---- the failure note ------------------------------------------------------------------------------------------------------------------------------ */

void css_computed_note_failure(css_core * core, GTEXT_CSS_Status status, const char * callback, size_t callback_length, GTEXT_CSS_Property_Id property) {
  css_cmps * cs = core->cs;
  if (!cs) {
    return;
  }
  cs->failure.status = status;
  cs->failure.property = property;
  size_t n = callback ? callback_length : 0;
  if (n >= sizeof cs->failure.callback) {
    n = sizeof cs->failure.callback - 1;
  }
  if (n) {
    memcpy(cs->failure.callback, callback, n);
  }
  cs->failure.callback[n] = '\0';
}

GTEXT_CSS_Status gtext_cssom_computed_failure(const GTEXT_CSSOM_Document * document, GTEXT_CSSOM_Computed_Failure * out) {
  if (!document || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  memset(out, 0, sizeof *out);
  const css_core * core = css_document_core((GTEXT_CSSOM_Document *)(uintptr_t)document);
  if (core->cs) {
    *out = core->cs->failure;
  }
  return GTEXT_CSS_OK;
}

/* ---- computing ---------------------------------------------------------------------------------------------------------------------------------------- */

static GTEXT_CSS_Tristate environment_condition(void * user, GTEXT_CSS_Condition_Kind kind, const GTEXT_CSS_String * text, GTEXT_CSS_Element_Handle element) {
  (void)element;
  const css_core * core = user;
  return gtext_css_conditions_condition(core->conditions, kind, text);
}

/// The same for the conditional rules of a function body, which can ask for the element the function is called on (the container kinds), with the document's call context (the memo of one
/// snapshot of the tree), and which fail naming the callback they need.
static GTEXT_CSS_Status environment_condition_status(void * user, GTEXT_CSS_Condition_Kind kind, const GTEXT_CSS_String * text, GTEXT_CSS_Element_Handle element, GTEXT_CSS_Tristate * answer,
    const char ** detail) {
  css_core * core = user;
  return gtext_css_conditions_condition_element(core->conditions, kind, text, element, core->cs ? &core->cs->call : NULL, answer, detail);
}

/// The context in use: the caller's, or one the document makes from the registry, the default environment and the element interface.
static GTEXT_CSS_Status ensure_context(css_core * core, css_cmps * cs) {
  if (cs->ctx) {
    return GTEXT_CSS_OK;
  }
  if (core->config.compute_context) {
    cs->ctx = core->config.compute_context;
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Compute_Options co = gtext_css_compute_options_default();
  co.allocator = &core->alloc;
  co.elements = core->config.element_interface;
  /* the registrations of the document's `@property` rules (cssom_index.c keeps them current with the index; a read makes the index current before it asks for the context) */
  co.property_rules = core->props;
  co.function_rules = core->fns;
  /* the environment of a context the document makes answers `if(media())` and `if(supports())` with the conditions object the document evaluates with, as it is now
     (the document, not the object, is the `user`, so that an environment change that replaces the object is followed) */
  GTEXT_CSS_Environment env = gtext_css_environment_default();
  if (core->has_env) {
    env = core->env;   /* the consumer's: its callbacks are its own; the document answers the conditions only where it left none */
  }
  if (!env.condition && !env.user) {
    env.user = core;
    env.condition = environment_condition;
    env.condition_status = environment_condition_status;
  }
  GTEXT_CSS_Compute_Result res;
  memset(&res, 0, sizeof res);
  const GTEXT_CSS_Status st = gtext_css_compute_context_new(core->registry, &env, &co, &cs->own, &res);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  cs->ctx = cs->own;
  return GTEXT_CSS_OK;
}

/// The match context and the condition call of the tree as it is now.
static GTEXT_CSS_Status ensure_match(css_core * core, css_cmps * cs) {
  if (!cs->call_init) {
    gtext_css_condition_call_init(&cs->call);
    cs->call_init = true;
  }
  if (cs->match) {
    return GTEXT_CSS_OK;
  }
  const GTEXT_CSS_Element_Interface * iface = core->config.element_interface;
  if (!iface) {
    css_computed_note_failure(core, GTEXT_CSS_E_UNSUPPORTED, "element_interface", 17, 0);
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  GTEXT_CSS_Selector_Match_Options mo = core->has_match ? core->match : gtext_css_selector_match_options_default();
  mo.allocator = &core->alloc;
  if (!core->has_match) {
    mo.quirks = core->config.quirks;
  }
  return gtext_css_selector_match_context_new(iface, &mo, &cs->match);
}

/// The version of the animation timeline the caller keeps (GTEXT_CSSOM_Layout_Interface::timeline_version), 0 when it keeps none. A caller that gives the time of animations and no version cannot
/// have a style kept across reads while the document has `@keyframes` to sample: the time could be anything, so `*cacheable` is cleared.
static uint64_t timeline_version(const css_core * core, bool * cacheable) {
  if (!core->has_layout) {
    return 0;
  }
  uint64_t v = 0;
  if (core->layout.timeline_version) {
    v = core->layout.timeline_version(core->layout.user);
  }
  else if (core->layout.animation_time && core->keyframes) {
    *cacheable = false;
  }
  /* the version of the transitions is a part of the same key; a layout that gives transitions and no version keeps no style across reads */
  if (core->layout.transition_version) {
    v += 0x9E3779B97F4A7C15ull * (core->layout.transition_version(core->layout.user) + 1);
  }
  else if (core->layout.transitions) {
    *cacheable = false;
  }
  return v;
}

/// The cache and the match context are for one state of the document and the tree: a read in another drops both.
static void sync_pair(css_core * core, css_cmps * cs, bool cacheable, uint64_t tree, uint64_t time) {
  const bool same = cs->pair_valid && cacheable && cs->pgen == core->generation && cs->ptree == tree && cs->pepoch == core->tree_epoch && cs->pcepoch == cs->cepoch && cs->ptime == time;
  if (same) {
    return;
  }
  cache_clear(core, cs);
  gtext_css_selector_match_context_free(cs->match);
  cs->match = NULL;
  if (cs->call_init) {
    gtext_css_condition_call_reset(&cs->call);
  }
  cs->pgen = core->generation;
  cs->ptree = tree;
  cs->pepoch = core->tree_epoch;
  cs->pcepoch = cs->cepoch;
  cs->ptime = time;
  cs->pair_valid = cacheable;
}

/// The time of an animation of the caller's layout, for the sampler of a style (css_style.h).
static GTEXT_CSS_Status animation_time(void * user, GTEXT_CSS_Element_Handle element, const GTEXT_CSS_Selector_List * pseudo, const GTEXT_CSS_Anim_Effect * effect, double * time_ms, bool * known) {
  css_core * core = user;
  *known = false;
  if (!core->has_layout || !core->layout.animation_time) {
    return GTEXT_CSS_OK;
  }
  const GTEXT_CSS_Status st = core->layout.animation_time(core->layout.user, element, pseudo, effect, time_ms, known);
  if (st != GTEXT_CSS_OK) {
    *known = false;
    if (core->cs) {
      core->cs->anim_failed = true;
    }
  }
  return st;
}

static GTEXT_CSS_Status compute_one_hl(css_core * core, css_cmps * cs, const GTEXT_CSS_Rule_Index * index, const void * element, const GTEXT_CSS_Computed_Style * parent,
    const GTEXT_CSS_Selector_List * pseudo, bool highlight, const GTEXT_CSS_Computed_Style * highlight_parent, GTEXT_CSS_Computed_Style ** out) {
  GTEXT_CSS_Style_Options so = gtext_css_style_options_default();
  so.quirks = core->config.quirks;
  so.highlight = highlight;
  so.highlight_parent = highlight_parent;
  so.rules = index;
  so.match = cs->match;
  so.container_call = &cs->call;
  if (pseudo) {
    so.pseudo = pseudo;
    so.sources = 0;   /* the element's own style attribute and presentation attributes are not the pseudo-element's */
  }
  /* the animations of the sheets' `@keyframes`, sampled at the times the caller's layout gives */
  GTEXT_CSS_Style_Animation anim;
  if (core->keyframes) {
    memset(&anim, 0, sizeof anim);
    anim.size = sizeof anim;
    anim.keyframes = core->keyframes;
    anim.user = core;
    anim.current_time = animation_time;
    so.animation = &anim;
  }
  /* the running transitions of the element the caller's layout keeps, at its time (the after-change styles are made without them) */
  GTEXT_CSS_Style_Transitions tr;
  memset(&tr, 0, sizeof tr);
  if (!core->no_transitions && core->has_layout && core->layout.transitions) {
    tr.size = sizeof tr;
    const GTEXT_CSS_Status ts = core->layout.transitions(core->layout.user, element, pseudo, &tr.set, &tr.time_ms);
    if (ts != GTEXT_CSS_OK) {
      css_computed_note_failure(core, ts, "transitions", 11, 0);
      return ts;
    }
    if (tr.set) {
      so.transitions = &tr;
    }
  }
  cs->anim_failed = false;
  GTEXT_CSS_Compute_Result res;
  memset(&res, 0, sizeof res);
  core->counters.computed_computes++;
  const GTEXT_CSS_Status st = gtext_css_style_compute(cs->ctx, element, parent, NULL, 0, &so, out, &res);
  if (st != GTEXT_CSS_OK) {
    if (cs->anim_failed) {
      css_computed_note_failure(core, st, "animation_time", 14, 0);
    }
    else {
      css_computed_note_failure(core, st, res.name, css_name_length(res.name, sizeof res.name), res.property);
    }
  }
  return st;
}

static GTEXT_CSS_Status compute_one(css_core * core, css_cmps * cs, const GTEXT_CSS_Rule_Index * index, const void * element, const GTEXT_CSS_Computed_Style * parent,
    const GTEXT_CSS_Selector_List * pseudo, GTEXT_CSS_Computed_Style ** out) {
  return compute_one_hl(core, cs, index, element, parent, pseudo, false, NULL, out);
}

/// Whether the pseudo-element is a highlight pseudo-element (CSS Pseudo-Elements 4, "Highlight Pseudo-elements"): `::selection`, `::target-text`, `::search-text`, `::spelling-error`,
/// `::grammar-error` or `::highlight()`. The list is one pseudo-element and nothing else (parse_pseudo()).
static bool is_highlight_pseudo(const GTEXT_CSS_Selector_List * pseudo) {
  if (!pseudo || pseudo->count != 1 || pseudo->complex[0].compound_count != 1 || pseudo->complex[0].compounds[0].simple_count != 1) {
    return false;
  }
  const GTEXT_CSS_Simple * s = &pseudo->complex[0].compounds[0].simples[0];
  static const char * const names[] = {"selection", "target-text", "search-text", "spelling-error", "grammar-error", "highlight"};
  if (s->kind != GTEXT_CSS_SIMPLE_PSEUDO_ELEMENT) {
    return false;
  }
  for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
    if (s->name.length == strlen(names[i]) && memcmp(s->name.data, names[i], s->name.length) == 0) {
      return true;
    }
  }
  return false;
}

static GTEXT_CSS_Status parent_of(css_core * core, const void * element, const void ** parent);
static GTEXT_CSS_Status style_of(css_core * core, css_cmps * cs, const GTEXT_CSS_Rule_Index * index, const void * element, GTEXT_CSS_Computed_Style ** out);

/// The style of a pseudo-element of an element. A highlight pseudo-element inherits from the same pseudo-element of the parent element (css_style.h, GTEXT_CSS_Style_Options::highlight), which
/// inherits from its parent's: the styles are made from the root element down, each from the one above, and only the last is kept. The chain is not cached (a style of an element is, and a
/// highlight is read rarely); the cost of a read is the depth of the element times a cascade of the pseudo-element.
static GTEXT_CSS_Status pseudo_style_of(css_core * core, css_cmps * cs, const GTEXT_CSS_Rule_Index * index, const void * element, const GTEXT_CSS_Computed_Style * of_element,
    const GTEXT_CSS_Selector_List * pseudo, GTEXT_CSS_Computed_Style ** out) {
  if (!is_highlight_pseudo(pseudo)) {
    return compute_one(core, cs, index, element, of_element, pseudo, out);
  }
  const void ** chain = NULL;
  size_t n = 0, cap = 0;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  const void * e = element;
  while (e) {
    if (n == cap) {
      const size_t grown = cap ? cap * 2 : 16;
      const void ** bigger = gtext_allocator_realloc(&core->alloc, (void *)chain, grown * sizeof *chain);
      if (!bigger) {
        st = GTEXT_CSS_E_OOM;
        break;
      }
      chain = bigger;
      cap = grown;
    }
    if (n >= MAX_CHAIN) {
      st = GTEXT_CSS_E_LIMIT_ELEMENTS;
      break;
    }
    chain[n++] = e;
    st = parent_of(core, e, &e);
    if (st != GTEXT_CSS_OK) {
      break;
    }
  }
  GTEXT_CSS_Computed_Style * above = NULL;   /* the highlight of the element above the one to compute */
  for (size_t i = n; st == GTEXT_CSS_OK && i-- > 0;) {
    GTEXT_CSS_Computed_Style * made = NULL;
    if (i == 0) {
      st = compute_one_hl(core, cs, index, element, of_element, pseudo, true, above, &made);
    }
    else {
      GTEXT_CSS_Computed_Style * es = NULL;
      st = style_of(core, cs, index, chain[i], &es);
      if (st == GTEXT_CSS_OK) {
        st = compute_one_hl(core, cs, index, chain[i], es, pseudo, true, above, &made);
        gtext_css_computed_free(es);
      }
    }
    gtext_css_computed_free(above);
    above = st == GTEXT_CSS_OK ? made : NULL;
  }
  gtext_allocator_free(&core->alloc, (void *)chain);
  if (st != GTEXT_CSS_OK) {
    gtext_css_computed_free(above);
    return st;
  }
  *out = above;
  return GTEXT_CSS_OK;
}

/// The inheritance parent of an element through the caller's interface: NULL at the root.
static GTEXT_CSS_Status parent_of(css_core * core, const void * element, const void ** parent) {
  GTEXT_CSS_Element_Result er;
  memset(&er, 0, sizeof er);
  const GTEXT_CSS_Status st = gtext_css_element_get_inheritance_parent(core->config.element_interface, element, parent, &er);
  if (st != GTEXT_CSS_OK) {
    css_computed_note_failure(core, st, "parent", 6, 0);
  }
  return st;
}

/// The style of an element: from the cache, or computed down the chain from the nearest cached ancestor. `*out` has one count of the caller's.
static GTEXT_CSS_Status style_of(css_core * core, css_cmps * cs, const GTEXT_CSS_Rule_Index * index, const void * element, GTEXT_CSS_Computed_Style ** out) {
  css_rec * hit = rec_find(cs, element);
  if (hit && hit->style) {
    core->counters.computed_cache_hits++;
    *out = gtext_css_computed_retain(hit->style);
    return GTEXT_CSS_OK;
  }
  const void ** chain = NULL;
  size_t n = 0, cap = 0;
  GTEXT_CSS_Computed_Style * base = NULL;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  for (const void * e = element; e;) {
    css_rec * r = rec_find(cs, e);
    if (e != element && r && r->style) {
      core->counters.computed_cache_hits++;
      base = gtext_css_computed_retain(r->style);
      break;
    }
    if (n == cap) {
      const size_t grown = cap ? cap * 2 : 16;
      const void ** bigger = gtext_allocator_realloc(&core->alloc, (void *)chain, grown * sizeof *chain);
      if (!bigger) {
        st = GTEXT_CSS_E_OOM;
        break;
      }
      chain = bigger;
      cap = grown;
    }
    if (n >= MAX_CHAIN) {
      st = GTEXT_CSS_E_LIMIT_ELEMENTS;
      break;
    }
    chain[n++] = e;
    const void * up = NULL;
    st = parent_of(core, e, &up);
    if (st != GTEXT_CSS_OK) {
      break;
    }
    e = up;
  }
  GTEXT_CSS_Computed_Style * current = base;   /* the style of the element above the one to compute */
  for (size_t i = n; st == GTEXT_CSS_OK && i-- > 0;) {
    GTEXT_CSS_Computed_Style * made = NULL;
    st = compute_one(core, cs, index, chain[i], current, NULL, &made);
    if (st != GTEXT_CSS_OK) {
      break;
    }
    cache_put(core, cs, chain[i], made);
    gtext_css_computed_free(current);
    current = made;
  }
  gtext_allocator_free(&core->alloc, (void *)chain);
  if (st != GTEXT_CSS_OK) {
    gtext_css_computed_free(current);
    return st;
  }
  *out = current;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_computed_parent_style(const css_resolve_in * in, GTEXT_CSS_Computed_Style ** out) {
  *out = NULL;
  if (in->pseudo) {
    *out = gtext_css_computed_retain((GTEXT_CSS_Computed_Style *)(uintptr_t)in->element_style);
    return GTEXT_CSS_OK;
  }
  const void * up = NULL;
  GTEXT_CSS_Status st = parent_of(in->core, in->element, &up);
  if (st != GTEXT_CSS_OK || !up) {
    return st;
  }
  return style_of(in->core, in->cs, in->index, up, out);
}

GTEXT_CSS_Status css_computed_in_display_none(const css_resolve_in * in, bool * yes) {
  *yes = false;
  const GTEXT_CSS_Property_Id display = gtext_css_registry_find_property(in->core->registry, "display", 7);
  if (!display) {
    return GTEXT_CSS_OK;
  }
  const void * e = in->element;
  if (!in->pseudo) {
    GTEXT_CSS_Status st = parent_of(in->core, e, &e);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
  }
  for (size_t steps = 0; e && steps < MAX_CHAIN; steps++) {
    GTEXT_CSS_Computed_Style * s = NULL;
    GTEXT_CSS_Status st = style_of(in->core, in->cs, in->index, e, &s);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    const GTEXT_CSS_Value * root = gtext_css_computed_root(s, display);
    const bool none = root && root->kind == GTEXT_CSS_VAL_KEYWORD && root->text.length == 4 && memcmp(root->text.data, "none", 4) == 0;
    gtext_css_computed_free(s);
    if (none) {
      *yes = true;
      return GTEXT_CSS_OK;
    }
    st = parent_of(in->core, e, &e);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
  }
  return GTEXT_CSS_OK;
}

/* ---- the first steps ---------------------------------------------------------------------------------------------------------------------------------- */

/// "elt is connected, part of the flat tree, and its shadow-including root has a browsing context ... being rendered".
static GTEXT_CSS_Status is_rendered(css_core * core, const css_comp * c, bool * ok) {
  *ok = true;
  if (core->config.element_state) {
    const uint32_t flags = core->config.element_state(core->config.element_state_user, c->element);
    if ((flags & GTEXT_CSSOM_ELEMENT_STATE_ALL) != GTEXT_CSSOM_ELEMENT_STATE_ALL) {
      *ok = false;
      return GTEXT_CSS_OK;
    }
  }
  else if (core->has_match && core->match.root_given) {
    /* no hook: the document element is the one the caller named, and an element is connected when the top of its chain is it (a loose element is its own top) */
    const void * top = c->element;
    for (size_t steps = 0; steps < MAX_CHAIN; steps++) {
      const void * up = NULL;
      const GTEXT_CSS_Status st = parent_of(core, top, &up);
      if (st != GTEXT_CSS_OK) {
        return st;
      }
      if (!up) {
        break;
      }
      top = up;
    }
    if (top != core->match.root) {
      *ok = false;
      return GTEXT_CSS_OK;
    }
  }
  if (core->has_layout && core->layout.is_rendered) {
    bool r = true;
    const GTEXT_CSS_Status st = core->layout.is_rendered(core->layout.user, c->element, c->pseudo, &r);
    if (st != GTEXT_CSS_OK) {
      css_computed_note_failure(core, st, "is_rendered", 11, 0);
      return st;
    }
    *ok = r;
  }
  return GTEXT_CSS_OK;
}

/* ---- the refresh -------------------------------------------------------------------------------------------------------------------------------------- */

/// The computed `background-position` as the computed layer writes it: "a list, each item a pair of offsets (horizontal and vertical) from the top left origin" (Backgrounds 4, 2.7),
/// which is what both browsers answer for the shorthand (`5px 9px`, `0% 0%`, `calc(100% - 10px) 50%`). The declarations of the block are the longhands', which keep the origin keyword (2.6),
/// so the shorthand written from them (`left 5px top 9px`) is not the computed value. NULL where the style has no such property (a registry without it).
static GTEXT_CSS_Status position_text(css_core * core, const GTEXT_CSS_Computed_Style * style, char ** out, size_t * out_length) {
  *out = NULL;
  *out_length = 0;
  const GTEXT_CSS_Property_Id id = gtext_css_registry_find_property(core->registry, "background-position", 19);
  if (!id || !style) {
    return GTEXT_CSS_OK;
  }
  char * text = NULL;
  size_t n = 0;
  const GTEXT_CSS_Status st = gtext_css_computed_shorthand_text(style, id, &text, &n, NULL);
  if (st == GTEXT_CSS_E_UNSUPPORTED) {
    return GTEXT_CSS_OK;   /* a longhand the engine cannot compute: the block's own shorthand stands */
  }
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  *out = css_core_dup(core, text, n);
  gtext_css_serialize_buffer_free(NULL, text);
  if (!*out) {
    return GTEXT_CSS_E_OOM;
  }
  *out_length = n;
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status refresh_read(GTEXT_CSSOM_Decl * d) {
  css_comp * c = d->comp;
  css_core * core = d->obj.core;
  if (!c || c->detached || !css_document_alive(core)) {
    return GTEXT_CSS_OK;
  }
  css_cmps * cs = core->cs;
  bool hook = false;
  const uint64_t tree = css_core_tree_version(core, &hook);
  bool cacheable = hook || core->tree_epoch > 0 || core->config.computed_trust_tree_changed;
  const uint64_t time = timeline_version(core, &cacheable);
  if (c->valid && cacheable && c->gen == core->generation && c->tree == tree && c->epoch == core->tree_epoch && c->cepoch == cs->cepoch && c->time == time) {
    return GTEXT_CSS_OK;
  }
  core->counters.computed_refreshes++;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  GTEXT_CSS_Declaration_Block * block = NULL;
  GTEXT_CSS_Computed_Style * style = NULL;
  uint8_t * origins = NULL;
  size_t norigins = 0;
  char * untimed_custom = NULL;
  char * position = NULL;
  size_t position_length = 0;
  bool empty = c->null_obj;
  core->in_callback++;   /* a mutation attempted from a callback of the caller's tree or layout is refused */
  if (!empty) {
    bool ok = true;
    st = is_rendered(core, c, &ok);
    empty = st == GTEXT_CSS_OK && !ok;
  }
  core->in_callback--;
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (empty) {
    GTEXT_CSS_Declaration_Options o = gtext_css_declaration_options_default();
    o.allocator = &core->alloc;
    o.max_declarations = core->config.max_declarations;
    o.max_output_bytes = core->config.max_output_bytes;
    st = gtext_css_block_new(core->registry, &o, &block);
  }
  else {
    const GTEXT_CSS_Rule_Index * index = NULL;
    /* the index first: building it can replace the context the document made (the registrations of its `@property` rules changed) */
    st = css_index_current(core, &index);
    if (st == GTEXT_CSS_OK) {
      st = ensure_context(core, cs);
    }
    if (st == GTEXT_CSS_OK) {
      core->in_callback++;
      sync_pair(core, cs, cacheable, tree, time);
      st = ensure_match(core, cs);
      GTEXT_CSS_Computed_Style * of_element = NULL;
      if (st == GTEXT_CSS_OK) {
        st = style_of(core, cs, index, c->element, &of_element);
      }
      if (st == GTEXT_CSS_OK && c->pseudo) {
        st = pseudo_style_of(core, cs, index, c->element, of_element, c->pseudo, &style);
      }
      else {
        style = of_element;
      }
      if (st == GTEXT_CSS_OK) {
        css_resolve_in in = {core, cs, c->element, c->pseudo, index, c->pseudo ? of_element : NULL};
        st = css_resolved_build(&in, style, &block, &origins, &norigins, &untimed_custom);
      }
      if (st == GTEXT_CSS_OK) {
        st = position_text(core, style, &position, &position_length);
      }
      if (c->pseudo) {
        gtext_css_computed_free(of_element);
      }
      core->in_callback--;
    }
  }
  if (st != GTEXT_CSS_OK) {
    gtext_css_computed_free(style);
    gtext_css_block_free(block);
    gtext_allocator_free(&core->alloc, origins);
    gtext_allocator_free(&core->alloc, untimed_custom);
    gtext_allocator_free(&core->alloc, position);
    return st;
  }
  gtext_allocator_free(&core->alloc, c->untimed_custom);
  c->untimed_custom = untimed_custom;
  gtext_allocator_free(&core->alloc, c->position);
  c->position = position;
  c->position_length = position_length;
  gtext_css_block_free(d->block);
  d->block = block;
  gtext_allocator_free(&core->alloc, c->origins);
  c->origins = origins;
  c->norigins = norigins;
  gtext_css_computed_free(style);   /* the declarations hold counted values, not the style */
  c->valid = true;
  c->empty = empty;
  c->gen = core->generation;
  c->tree = tree;
  c->epoch = core->tree_epoch;
  c->cepoch = cs->cepoch;
  c->time = time;
  d->version++;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_computed_refresh(GTEXT_CSSOM_Decl * d) {
  css_core * core = d->obj.core;   /* the declaration holds the core, so the core outlives the read whatever the callbacks do to the handle */
  core->computing++;
  const GTEXT_CSS_Status st = refresh_read(d);
  if (--core->computing == 0) {
    if (core->computed_teardown_pending) {
      core->computed_teardown_pending = false;
      css_computed_teardown(core);
    }
    if (core->cs && (core->cs->nreleased || core->cs->released_lost)) {
      flush_released(core, core->cs);
    }
  }
  return st;
}

/* ---- the entry points ----------------------------------------------------------------------------------------------------------------------------------- */

/// "If pseudoElt is provided, is not the empty string, and starts with a colon, then: Parse pseudoElt as a <pseudo-element-selector>, and let type be the result. If type is failure,
/// or is a ::slotted() or ::part() pseudo-element, let obj be null."
static GTEXT_CSS_Status parse_pseudo(css_core * core, const char * text, size_t length, GTEXT_CSS_Selector_List ** list, bool * null_obj) {
  *list = NULL;
  *null_obj = false;
  if (!text || length == 0 || text[0] != ':') {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Selector_Options so = gtext_css_selector_options_default();
  so.allocator = &core->alloc;
  GTEXT_CSS_Selector_List * l = NULL;
  GTEXT_CSS_Selector_Result res;
  memset(&res, 0, sizeof res);
  GTEXT_CSS_Status st = gtext_css_selector_parse(text, length, &so, &l, &res);
  gtext_css_selector_result_free(&res);
  if (st == GTEXT_CSS_E_OOM) {
    return st;
  }
  if (st != GTEXT_CSS_OK || !l) {
    *null_obj = true;   /* failure, or a limit of the parse: not a pseudo-element selector */
    return GTEXT_CSS_OK;
  }
  /* a <pseudo-element-selector> is one pseudo-element and nothing else */
  /* ... and the whole string: white space after a complete selector is not part of it ("::before " is the empty declaration in both browsers; "::highlight( name " is not trailing, the
     unclosed function takes it) */
  bool good = l->count == 1 && l->complex[0].compound_count == 1 && l->complex[0].compounds[0].simple_count == 1 && !l->complex[0].relative && l->complex[0].span.end == length;
  if (good) {
    const GTEXT_CSS_Simple * s = &l->complex[0].compounds[0].simples[0];
    good = s->kind == GTEXT_CSS_SIMPLE_PSEUDO_ELEMENT && !(s->name.length == 7 && memcmp(s->name.data, "slotted", 7) == 0) &&
        !(s->name.length == 4 && memcmp(s->name.data, "part", 4) == 0);
  }
  if (!good) {
    gtext_css_selector_list_free(l);
    *null_obj = true;
    return GTEXT_CSS_OK;
  }
  *list = l;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_computed_style(GTEXT_CSSOM_Document * document, const void * element, const char * pseudo, size_t pseudo_length, GTEXT_CSSOM_Decl ** out) {
  if (!document || !element || !out || (!pseudo && pseudo_length)) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = css_document_core(document);
  if (!core->config.element_interface) {
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  css_cmps * cs = css_cmps_get(core);
  if (!cs) {
    return GTEXT_CSS_E_OOM;
  }
  GTEXT_CSS_Selector_List * list = NULL;
  bool null_obj = false;
  GTEXT_CSS_Status st = parse_pseudo(core, pseudo, pseudo_length, &list, &null_obj);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  css_rec * rec = rec_get(core, cs, element);
  css_comp * c = gtext_allocator_calloc(&core->alloc, 1, sizeof *c);
  GTEXT_CSSOM_Decl * d = NULL;
  if (!rec || !c) {
    st = GTEXT_CSS_E_OOM;
  }
  else {
    st = css_decl_create(core, GTEXT_CSSOM_DECL_COMPUTED, 0, 1, 0, &d);
  }
  if (st != GTEXT_CSS_OK) {
    gtext_allocator_free(&core->alloc, c);
    gtext_css_selector_list_free(list);
    if (rec) {
      rec_maybe_free(core, cs, rec);
    }
    return st;
  }
  c->element = element;
  c->pseudo = list;
  c->null_obj = null_obj;
  d->comp = c;
  chain_link(core, cs, d, rec);
  *out = d;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_document_after_change_style(GTEXT_CSSOM_Document * document, const void * element, const char * pseudo, size_t pseudo_length,
    GTEXT_CSS_Computed_Style ** out) {
  if (!document || !element || !out || (!pseudo && pseudo_length)) {
    return GTEXT_CSS_E_INVALID;
  }
  *out = NULL;
  css_core * core = css_document_core(document);
  if (!core->config.element_interface) {
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  css_cmps * cs = css_cmps_get(core);
  if (!cs) {
    return GTEXT_CSS_E_OOM;
  }
  GTEXT_CSS_Selector_List * list = NULL;
  bool null_obj = false;
  GTEXT_CSS_Status st = parse_pseudo(core, pseudo, pseudo_length, &list, &null_obj);
  if (st != GTEXT_CSS_OK || null_obj) {
    return st;
  }
  core->computing++;
  bool hook = false;
  const uint64_t tree = css_core_tree_version(core, &hook);
  bool cacheable = hook || core->tree_epoch > 0 || core->config.computed_trust_tree_changed;
  const uint64_t time = timeline_version(core, &cacheable);
  const GTEXT_CSS_Rule_Index * index = NULL;
  st = css_index_current(core, &index);
  if (st == GTEXT_CSS_OK) {
    st = ensure_context(core, cs);
  }
  const void ** chain = NULL;
  size_t n = 0, cap = 0;
  GTEXT_CSS_Computed_Style * current = NULL;
  if (st == GTEXT_CSS_OK) {
    core->in_callback++;
    core->no_transitions = true;
    sync_pair(core, cs, cacheable, tree, time);
    st = ensure_match(core, cs);
    for (const void * e = element; st == GTEXT_CSS_OK && e;) {
      if (n == cap) {
        const size_t grown = cap ? cap * 2 : 16;
        const void ** bigger = gtext_allocator_realloc(&core->alloc, (void *)chain, grown * sizeof *chain);
        if (!bigger) {
          st = GTEXT_CSS_E_OOM;
          break;
        }
        chain = bigger;
        cap = grown;
      }
      if (n >= MAX_CHAIN) {
        st = GTEXT_CSS_E_LIMIT_ELEMENTS;
        break;
      }
      chain[n++] = e;
      const void * up = NULL;
      st = parent_of(core, e, &up);
      e = up;
    }
    for (size_t i = n; st == GTEXT_CSS_OK && i-- > 0;) {
      GTEXT_CSS_Computed_Style * made = NULL;
      st = compute_one(core, cs, index, chain[i], current, NULL, &made);
      gtext_css_computed_free(current);
      current = st == GTEXT_CSS_OK ? made : NULL;
    }
    if (st == GTEXT_CSS_OK && list) {
      GTEXT_CSS_Computed_Style * of_pseudo = NULL;
      st = pseudo_style_of(core, cs, index, element, current, list, &of_pseudo);
      gtext_css_computed_free(current);
      current = st == GTEXT_CSS_OK ? of_pseudo : NULL;
    }
    core->no_transitions = false;
    /* what the pass learned (the answers of the container queries it asked, the match context) belongs to the after-change styles and not to the styles a read shows: the next read starts
       from nothing, as it does after the tree changes. A query that could not be answered (a size the layout was not asked for) is answered once, with its status, and the read must ask again. */
    sync_pair(core, cs, false, tree, time);
    core->in_callback--;
  }
  gtext_allocator_free(&core->alloc, (void *)chain);
  gtext_css_selector_list_free(list);
  if (--core->computing == 0 && core->computed_teardown_pending) {
    core->computed_teardown_pending = false;
    css_computed_teardown(core);
  }
  if (st != GTEXT_CSS_OK) {
    gtext_css_computed_free(current);
    return st;
  }
  *out = current;
  return GTEXT_CSS_OK;
}

/* ---- the transition tracker ----------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  const void * element;
  char * key;                        ///< The pseudo-element's canonical name, "" for the element.
  size_t key_length;
  GTEXT_CSS_Transition_Set * set;
  GTEXT_CSS_Computed_Style * before; ///< The after-change style of the previous style change event, or NULL before the first.
} tracker_entry;

struct GTEXT_CSSOM_Transition_Tracker {
  GTEXT_CSSOM_Document * document;
  GTEXT_Allocator alloc;
  tracker_entry * entries;
  size_t count, cap;
  double time_ms;
  uint64_t version;
};

GTEXT_CSS_Status gtext_cssom_transition_tracker_new(GTEXT_CSSOM_Document * document, const GTEXT_Allocator * allocator, GTEXT_CSSOM_Transition_Tracker ** out) {
  if (!document || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  *out = NULL;
  GTEXT_Allocator a = allocator ? *allocator : *gtext_allocator_default();
  GTEXT_CSSOM_Transition_Tracker * t = gtext_allocator_calloc(&a, 1, sizeof *t);
  if (!t) {
    return GTEXT_CSS_E_OOM;
  }
  t->document = document;
  t->alloc = a;
  *out = t;
  return GTEXT_CSS_OK;
}

static void entry_release(GTEXT_CSSOM_Transition_Tracker * t, tracker_entry * e) {
  gtext_css_transition_set_free(e->set);
  gtext_css_computed_free(e->before);
  gtext_allocator_free(&t->alloc, e->key);
  memset(e, 0, sizeof *e);
}

void gtext_cssom_transition_tracker_free(GTEXT_CSSOM_Transition_Tracker * tracker) {
  if (!tracker) {
    return;
  }
  for (size_t i = 0; i < tracker->count; i++) {
    entry_release(tracker, &tracker->entries[i]);
  }
  GTEXT_Allocator a = tracker->alloc;
  gtext_allocator_free(&a, tracker->entries);
  gtext_allocator_free(&a, tracker);
}

void gtext_cssom_transition_tracker_set_time(GTEXT_CSSOM_Transition_Tracker * tracker, double time_ms) {
  if (tracker && tracker->time_ms != time_ms) {
    tracker->time_ms = time_ms;
    tracker->version++;
  }
}

uint64_t gtext_cssom_transition_tracker_version(const GTEXT_CSSOM_Transition_Tracker * tracker) {
  return tracker ? tracker->version : 0;
}

static tracker_entry * entry_find(GTEXT_CSSOM_Transition_Tracker * t, const void * element, const char * key, size_t key_length) {
  for (size_t i = 0; i < t->count; i++) {
    tracker_entry * e = &t->entries[i];
    if (e->element == element && e->key_length == key_length && (key_length == 0 || memcmp(e->key, key, key_length) == 0)) {
      return e;
    }
  }
  return NULL;
}

static GTEXT_CSS_Status entry_get(GTEXT_CSSOM_Transition_Tracker * t, const void * element, const char * key, size_t key_length, tracker_entry ** out) {
  tracker_entry * e = entry_find(t, element, key, key_length);
  if (e) {
    *out = e;
    return GTEXT_CSS_OK;
  }
  if (t->count == t->cap) {
    const size_t cap = t->cap ? t->cap * 2 : 8;
    tracker_entry * bigger = gtext_allocator_realloc(&t->alloc, t->entries, cap * sizeof *bigger);
    if (!bigger) {
      return GTEXT_CSS_E_OOM;
    }
    t->entries = bigger;
    t->cap = cap;
  }
  tracker_entry fresh;
  memset(&fresh, 0, sizeof fresh);
  fresh.element = element;
  fresh.key = gtext_allocator_malloc(&t->alloc, key_length + 1);
  if (!fresh.key) {
    return GTEXT_CSS_E_OOM;
  }
  if (key_length) {
    memcpy(fresh.key, key, key_length);
  }
  fresh.key[key_length] = '\0';
  fresh.key_length = key_length;
  const GTEXT_CSS_Status st = gtext_css_transition_set_new(&t->alloc, &fresh.set);
  if (st != GTEXT_CSS_OK) {
    gtext_allocator_free(&t->alloc, fresh.key);
    return st;
  }
  t->entries[t->count] = fresh;
  *out = &t->entries[t->count++];
  return GTEXT_CSS_OK;
}

size_t gtext_cssom_transition_tracker_forget(GTEXT_CSSOM_Transition_Tracker * tracker, const void * element) {
  size_t gone = 0;
  if (!tracker) {
    return 0;
  }
  for (size_t i = 0; i < tracker->count;) {
    if (tracker->entries[i].element == element) {
      entry_release(tracker, &tracker->entries[i]);
      memmove(&tracker->entries[i], &tracker->entries[i + 1], (tracker->count - i - 1) * sizeof *tracker->entries);
      tracker->count--;
      gone++;
    }
    else {
      i++;
    }
  }
  if (gone) {
    tracker->version++;
  }
  return gone;
}

/// The name that keys a pseudo-element: the canonical name of its one pseudo-element selector.
static void pseudo_key(const GTEXT_CSS_Selector_List * pseudo, const char ** key, size_t * length) {
  *key = "";
  *length = 0;
  if (pseudo && pseudo->count == 1 && pseudo->complex[0].compound_count == 1 && pseudo->complex[0].compounds[0].simple_count == 1) {
    const GTEXT_CSS_Simple * s = &pseudo->complex[0].compounds[0].simples[0];
    *key = s->name.data ? s->name.data : "";
    *length = s->name.data ? s->name.length : 0;
  }
}

GTEXT_CSS_Status gtext_cssom_transition_tracker_lookup(GTEXT_CSSOM_Transition_Tracker * tracker, const void * element, const GTEXT_CSS_Selector_List * pseudo,
    const GTEXT_CSS_Transition_Set ** set, double * time_ms) {
  if (!tracker || !set || !time_ms) {
    return GTEXT_CSS_E_INVALID;
  }
  const char * key;
  size_t kl;
  pseudo_key(pseudo, &key, &kl);
  tracker_entry * e = entry_find(tracker, element, key, kl);
  *set = e && gtext_css_transition_set_running_count(e->set) ? e->set : NULL;
  *time_ms = tracker->time_ms;
  return GTEXT_CSS_OK;
}

/// One style change event of one element or pseudo-element: update its set against the style of the previous event, and keep this one.
static GTEXT_CSS_Status tracker_step(GTEXT_CSSOM_Transition_Tracker * t, const void * element, const char * key, size_t key_length, GTEXT_CSS_Computed_Style * after) {
  tracker_entry * e = NULL;
  GTEXT_CSS_Status st = entry_get(t, element, key, key_length, &e);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  const uint64_t was = gtext_css_transition_set_version(e->set);
  /* a style made in another context (the registrations of the document changed between the two events) is not comparable: it is as if the element had no before-change style */
  st = gtext_css_transitions_update(after->cc, e->set, e->before && e->before->cc == after->cc ? e->before : NULL, after, t->time_ms, NULL);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (gtext_css_transition_set_version(e->set) != was) {
    t->version++;
  }
  gtext_css_computed_free(e->before);
  e->before = gtext_css_computed_retain(after);
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_transition_tracker_flush(GTEXT_CSSOM_Transition_Tracker * tracker, const void * element, const char * pseudo, size_t pseudo_length) {
  if (!tracker || !element || (!pseudo && pseudo_length)) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = css_document_core(tracker->document);
  if (!core->config.element_interface) {
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  /* the chain from the root, each element's after-change style made from its parent's */
  const void ** chain = NULL;
  size_t n = 0, cap = 0;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  core->in_callback++;
  for (const void * e = element; e && st == GTEXT_CSS_OK;) {
    if (n == cap) {
      const size_t grown = cap ? cap * 2 : 16;
      const void ** bigger = gtext_allocator_realloc(&tracker->alloc, (void *)chain, grown * sizeof *chain);
      if (!bigger) {
        st = GTEXT_CSS_E_OOM;
        break;
      }
      chain = bigger;
      cap = grown;
    }
    if (n >= MAX_CHAIN) {
      st = GTEXT_CSS_E_LIMIT_ELEMENTS;
      break;
    }
    chain[n++] = e;
    const void * up = NULL;
    st = parent_of(core, e, &up);
    e = up;
  }
  core->in_callback--;
  for (size_t i = n; st == GTEXT_CSS_OK && i-- > 0;) {
    /* each element's own after-change style: the chain above it is computed again, which keeps this call free of any cache the shown styles use */
    GTEXT_CSS_Computed_Style * after = NULL;
    st = gtext_cssom_document_after_change_style(tracker->document, chain[i], NULL, 0, &after);
    if (st == GTEXT_CSS_OK && after) {
      st = tracker_step(tracker, chain[i], "", 0, after);
    }
    gtext_css_computed_free(after);
  }
  gtext_allocator_free(&tracker->alloc, (void *)chain);
  if (st == GTEXT_CSS_OK && pseudo && pseudo_length) {
    GTEXT_CSS_Computed_Style * after = NULL;
    st = gtext_cssom_document_after_change_style(tracker->document, element, pseudo, pseudo_length, &after);
    if (st == GTEXT_CSS_OK && after) {
      GTEXT_CSS_Selector_List * list = NULL;
      bool null_obj = false;
      st = parse_pseudo(core, pseudo, pseudo_length, &list, &null_obj);
      if (st == GTEXT_CSS_OK && list) {
        const char * key;
        size_t kl;
        pseudo_key(list, &key, &kl);
        st = tracker_step(tracker, element, key, kl, after);
      }
      gtext_css_selector_list_free(list);
    }
    gtext_css_computed_free(after);
  }
  return st;
}

static GTEXT_CSSOM_Decl * require_computed(GTEXT_CSSOM_Decl * decl) {
  return decl && decl->owner_kind == GTEXT_CSSOM_DECL_COMPUTED && decl->comp ? decl : NULL;
}

GTEXT_CSS_Status gtext_cssom_computed_is_empty(GTEXT_CSSOM_Decl * decl, bool * empty) {
  if (!require_computed(decl) || !empty) {
    return GTEXT_CSS_E_INVALID;
  }
  const GTEXT_CSS_Status st = css_computed_refresh(decl);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  *empty = decl->comp->empty || gtext_css_block_count(decl->block) == 0;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_computed_origin(GTEXT_CSSOM_Decl * decl, const char * name, size_t length, GTEXT_CSSOM_Origin * origin, bool * found) {
  if (!require_computed(decl) || !origin || !found || (!name && length)) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Status st = css_computed_refresh(decl);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  css_core * core = decl->obj.core;
  *origin = GTEXT_CSSOM_ORIGIN_COMPUTED;
  *found = false;
  GTEXT_CSS_Declaration_Item probe;
  const css_comp * c = decl->comp;
  const GTEXT_CSS_Property_Id id = gtext_css_registry_find_property(core->registry, name, length);
  if (!id) {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Property_Info info;
  if (!gtext_css_registry_property_info(core->registry, id, &info)) {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Property_Id target = info.flags & GTEXT_CSS_PROP_ALIAS ? info.alias_of : id;
  if (info.flags & GTEXT_CSS_PROP_CUSTOM) {
    *found = gtext_css_block_find(decl->block, 0, name, length, &probe);
    for (const char * u = c->untimed_custom; *found && u && *u; u += strlen(u) + 1) {
      if (strlen(u) == length && memcmp(u, name, length) == 0) {
        *origin = GTEXT_CSSOM_ORIGIN_ABSENT_TIMELINE;
      }
    }
    return GTEXT_CSS_OK;
  }
  if (!gtext_css_registry_property_info(core->registry, target, &info)) {
    return GTEXT_CSS_OK;
  }
  if (!(info.flags & GTEXT_CSS_PROP_SHORTHAND)) {
    *found = gtext_css_block_find(decl->block, target, NULL, 0, &probe);
    if (*found && target < c->norigins) {
      *origin = (GTEXT_CSSOM_Origin)c->origins[target];
    }
    return GTEXT_CSS_OK;
  }
  /* a shorthand: the strongest of its longhands (nested shorthands are walked through an explicit stack) */
  bool any = false;
  GTEXT_CSS_Property_Id stack[128];
  size_t top = 0;
  stack[top++] = target;
  while (top) {
    const GTEXT_CSS_Property_Id sh = stack[--top];
    for (size_t i = 0;; i++) {
      const GTEXT_CSS_Property_Id lh = gtext_css_registry_longhand(core->registry, sh, i);
      if (!lh) {
        break;
      }
      GTEXT_CSS_Property_Info li;
      if (!gtext_css_registry_property_info(core->registry, lh, &li)) {
        continue;
      }
      if (li.flags & GTEXT_CSS_PROP_SHORTHAND) {
        if (top < sizeof stack / sizeof *stack) {
          stack[top++] = lh;
        }
        continue;
      }
      if (!gtext_css_block_find(decl->block, lh, NULL, 0, &probe)) {
        continue;
      }
      any = true;
      if (lh < c->norigins) {
        const GTEXT_CSSOM_Origin o = (GTEXT_CSSOM_Origin)c->origins[lh];
        if (o == GTEXT_CSSOM_ORIGIN_USED || ((o == GTEXT_CSSOM_ORIGIN_ABSENT_LAYOUT || o == GTEXT_CSSOM_ORIGIN_ABSENT_TIMELINE) && *origin == GTEXT_CSSOM_ORIGIN_COMPUTED)) {
          *origin = o;
        }
      }
    }
  }
  *found = any;
  return GTEXT_CSS_OK;
}

/* ---- the container interface --------------------------------------------------------------------------------------------------------------------------- */

static const GTEXT_CSS_Computed_Style * container_computed_style(void * user, GTEXT_CSS_Element_Handle element) {
  const css_core * core = css_document_core((GTEXT_CSSOM_Document *)user);
  const css_cmps * cs = core->cs;
  if (!cs || !element) {
    return NULL;
  }
  const css_rec * r = rec_find(cs, element);
  return r ? r->style : NULL;
}

GTEXT_CSS_Status gtext_cssom_document_container_interface(GTEXT_CSSOM_Document * document, GTEXT_CSS_Container_Interface * out) {
  if (!document || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  memset(out, 0, sizeof *out);
  out->size = sizeof *out;
  out->user = document;
  out->computed_style = container_computed_style;
  css_core * core = css_document_core(document);
  css_cmps * cs = css_cmps_get(core);
  /* the context the document computes in: the caller's, else the one it makes (a style query computes the queried value in the context that made the container's style) */
  if (cs) {
    const GTEXT_CSS_Rule_Index * index = NULL;
    GTEXT_CSS_Status st = css_index_current(core, &index);   /* the registrations of the `@property` rules are current with it */
    if (st == GTEXT_CSS_OK) {
      st = ensure_context(core, cs);
    }
    if (st == GTEXT_CSS_OK && cs->own && cs->ctx == cs->own && !cs->own_exported) {
      /* the context is handed out: a later replacement keeps it, and the room for that is reserved now so that it cannot fail then */
      if (cs->nretired == cs->retired_cap) {
        const size_t grown = cs->retired_cap ? cs->retired_cap * 2 : 4;
        css_retired * bigger = gtext_allocator_realloc(&core->alloc, cs->retired, grown * sizeof *bigger);
        if (!bigger) {
          st = GTEXT_CSS_E_OOM;
        }
        else {
          cs->retired = bigger;
          cs->retired_cap = grown;
        }
      }
      cs->own_exported = st == GTEXT_CSS_OK;
    }
    if (st != GTEXT_CSS_OK) {
      memset(out, 0, sizeof *out);
      return st;
    }
    out->compute_context = cs->ctx;
  }
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_document_property_rules(GTEXT_CSSOM_Document * document, const GTEXT_CSS_At_Property_Set ** set, bool * applied) {
  if (!document || !set) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = css_document_core(document);
  const GTEXT_CSS_Rule_Index * index = NULL;
  const GTEXT_CSS_Status st = css_index_current(core, &index);   /* the set is current with the index */
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  *set = core->props;
  if (applied) {
    *applied = core->config.compute_context == NULL;
  }
  return GTEXT_CSS_OK;
}

bool css_computed_position_text(const GTEXT_CSSOM_Decl * d, const char ** text, size_t * length) {
  if (!d || !d->comp || !d->comp->position) {
    return false;
  }
  *text = d->comp->position;
  *length = d->comp->position_length;
  return true;
}
