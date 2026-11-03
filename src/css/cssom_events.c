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
 * The observers of a document, the events they receive and the batches that defer them (css_cssom_document.h).
 *
 * **One seam.** Every mutator ends in css_cssom_commit(), which bumps the generation and calls css_events_emit() here with a @ref css_cssom_event that holds what the mutator already
 * touched. This file turns that into the public @ref GTEXT_CSSOM_Event: it adds the rule's path, its selector list and the dependency summaries, copies what a batch will deliver later,
 * and holds the objects the event points at until it has been delivered. No mutator has code of its own for any of it.
 *
 * **The mutation never fails here.** A mutation is complete before the event is made, so nothing in the making of an event may undo it. What can fail is an allocation (the path, the
 * selector list, the copy of the property ids); the event is then delivered with `summary_complete` false and the dependency bits at GTEXT_CSSOM_DEP_ALL, which is the answer that is
 * right for any consumer (everything may have changed), and `events_degraded` counts it. The one thing that must not fail late is room in a batch's queue, so css_cssom_enter() reserves the
 * slot before the mutator changes anything: a batch that is full or cannot grow refuses the mutation, with nothing changed. A mutator that sends more than one event reserves them all.
 *
 * **Delivery.** Synchronous, in registration order, with the reentrancy flag set so that a call that mutates is GTEXT_CSS_E_REENTRANT. An observer registered during a delivery, or
 * between the mutations of a batch, is called for the events made after it and for none made before. An observer removed during a delivery is not called again. The registry is
 * indexed, never held by pointer across a call, because a callback may add an observer and move the array.
 */

#include <stdlib.h>
#include <string.h>

#include "css_internal.h"
#include "cssom_doc_internal.h"
#include "cssom_internal.h"
#include "cssom_rule_internal.h"
#include "cssom_typed.h"

/// An event with what it owns. In a batch the strings and arrays are copies; for an immediate delivery they are the mutator's, borrowed.
typedef struct css_qevent {
  GTEXT_CSSOM_Event pub;
  css_obj * held[4];                    ///< Sheet, rule, declaration, scope: external holds taken for the delivery.
  size_t * path;
  GTEXT_CSS_Selector_List * sel, * sel_before;
  GTEXT_CSS_Property_Id * props;        ///< A copy (batch), or NULL.
  GTEXT_CSS_String * customs;           ///< A copy in one block with the bytes (batch), or NULL.
} css_qevent;

#define DEFAULT_MAX_OBSERVERS ((size_t)1024)
#define DEFAULT_MAX_QUEUED ((size_t)1 << 20)

/* ---- the summaries ------------------------------------------------------------------------------------------------------------------------------------ */

static void everything(GTEXT_CSS_Selector_Dependencies * d) {
  memset(d, 0, sizeof *d);
  d->kinds = GTEXT_CSSOM_DEP_ALL;
  d->state = UINT64_MAX;
}

static void condition_everything(GTEXT_CSS_Condition_Dependencies * c) {
  memset(c, 0, sizeof *c);
  c->environment = 0x3ffu;
  c->features[0] = c->features[1] = UINT64_MAX;
  c->custom_media = true;
  c->unknown_features = true;
}

/// The dependencies of a media query list's text (`supports` false) or a supports condition's. False if the parse failed for lack of resources.
static bool condition_of_text(css_core * core, const char * text, size_t length, bool supports, GTEXT_CSS_Condition_Dependencies * out) {
  GTEXT_CSS_Media_Options mo = gtext_css_media_options_default();
  mo.allocator = &core->alloc;
  mo.parse.max_input_bytes = core->config.max_input_bytes;
  GTEXT_CSS_Media_List * list = NULL;
  GTEXT_CSS_Media_Result result;
  memset(&result, 0, sizeof result);
  const GTEXT_CSS_Status st = supports ? gtext_css_supports_parse(text, length, false, &mo, &list, &result) : gtext_css_media_list_parse(text, length, &mo, &list, &result);
  gtext_css_media_result_free(&result);
  if (st != GTEXT_CSS_OK || !list) {
    gtext_css_media_list_free(list);
    return false;
  }
  gtext_css_media_list_dependencies(list, out);
  gtext_css_media_list_free(list);
  return true;
}

/// The condition of a rule that has one the summary can read: `@media` and `@import` (their media query list) and `@supports`.
static bool rule_condition(css_core * core, css_rule * rule, GTEXT_CSS_Condition_Dependencies * out, bool * has) {
  *has = false;
  if (rule->kind == GTEXT_CSSOM_RULE_MEDIA || rule->kind == GTEXT_CSSOM_RULE_IMPORT) {
    char * text = NULL;
    size_t n = 0;
    if (css_rule_media_text(rule, &text, &n) != GTEXT_CSS_OK) {
      return false;
    }
    const bool ok = condition_of_text(core, text ? text : "", n, false, out);
    GTEXT_Allocator a = core->alloc;
    gtext_allocator_free(&a, text);
    *has = ok;
    return ok;
  }
  if (rule->kind == GTEXT_CSSOM_RULE_SUPPORTS) {
    GTEXT_CSS_Atrule * a = NULL;
    if (css_rule_typed(core, rule, true, &a) != GTEXT_CSS_OK) {
      return false;
    }
    size_t n = 0;
    const char * p = gtext_css_atrule_prelude_text(a, &n);
    const bool ok = condition_of_text(core, p ? p : "", n, true, out);
    gtext_css_atrule_free(a);
    *has = ok;
    return ok;
  }
  return true;
}

/// The summary of an event about a rule: its selector list and the dependencies of it and of its condition.
static bool rule_summary(css_core * core, const css_cssom_event * e, css_qevent * q) {
  GTEXT_CSSOM_Event * p = &q->pub;
  css_rule * rule = (css_rule *)e->rule;
  bool complete = true;
  p->descendants = rule->descendants;
  const bool structural = e->kind != GTEXT_CSSOM_EVENT_DECLARATIONS_CHANGED;
  if (rule->kind == GTEXT_CSSOM_RULE_STYLE && rule->node) {
    if (css_rule_selector_list(rule, &q->sel) == GTEXT_CSS_OK) {
      p->selectors = q->sel;
      gtext_css_selector_dependencies(q->sel, &p->selector_dependencies);
      if (structural && rule->descendants > 0) {
        /* the rules inside read their selectors relative to this one */
        p->selector_dependencies.kinds = GTEXT_CSSOM_DEP_ALL;
      }
    }
    else {
      everything(&p->selector_dependencies);
      complete = false;
    }
    if (e->selectors_before) {
      q->sel_before = (GTEXT_CSS_Selector_List *)e->selectors_before;
      p->selectors_before = q->sel_before;
      gtext_css_selector_dependencies(q->sel_before, &p->selector_dependencies_before);
      if (rule->descendants > 0) {
        p->selector_dependencies_before.kinds = GTEXT_CSSOM_DEP_ALL;
      }
    }
    else if (e->kind == GTEXT_CSSOM_EVENT_RULE_REPLACED && e->before_failed) {
      everything(&p->selector_dependencies_before);
      complete = false;
    }
  }
  else if (structural) {
    /* a rule that is not a style rule is not found by a selector, and what it changes is read from its kind: anything may have changed */
    everything(&p->selector_dependencies);
  }
  if (structural || rule->kind == GTEXT_CSSOM_RULE_MEDIA) {
    bool has = false;
    if (!rule_condition(core, rule, &p->condition_dependencies, &has)) {
      condition_everything(&p->condition_dependencies);
      p->has_condition = true;
      complete = false;
    }
    else {
      p->has_condition = has;
    }
  }
  return complete;
}

/// The indexes from the sheet's `cssRules` down to the rule (or to where a removed rule was). False if the rule is not in a sheet's tree of lists or memory ran out.
static bool make_path(css_core * core, const css_cssom_event * e, css_qevent * q) {
  const css_rule * base = e->has_position ? (const css_rule *)e->position_parent : (const css_rule *)e->rule;
  size_t depth = e->has_position ? 1 : 0;
  for (const css_rule * x = base; x;) {
    if (!x->list) {
      return true;   /* detached: no path (not a failure) */
    }
    depth++;
    x = x->list->owner->kind == GTEXT_CSSOM_OBJECT_RULE ? (const css_rule *)(uintptr_t)x->list->owner : NULL;
  }
  if (depth == 0) {
    return true;
  }
  size_t * path = gtext_allocator_malloc(&core->alloc, depth * sizeof *path);
  if (!path) {
    return false;
  }
  size_t at = depth;
  if (e->has_position) {
    path[--at] = e->position_index;
  }
  for (const css_rule * x = base; x;) {
    path[--at] = css_rule_list_index_of(x);
    x = x->list->owner->kind == GTEXT_CSSOM_OBJECT_RULE ? (const css_rule *)(uintptr_t)x->list->owner : NULL;
  }
  q->path = path;
  q->pub.path = path;
  q->pub.path_length = depth;
  q->pub.has_path = true;
  return true;
}

/* ---- building and freeing an event -------------------------------------------------------------------------------------------------------------------- */

static css_obj * hold(css_obj * o) {
  if (o) {
    css_obj_retain_external(o);
  }
  return o;
}

static void qevent_free(css_core * core, css_qevent * q) {
  GTEXT_Allocator a = core->alloc;
  gtext_allocator_free(&a, q->path);
  gtext_allocator_free(&a, q->props);
  gtext_allocator_free(&a, q->customs);
  gtext_css_selector_list_free(q->sel);
  gtext_css_selector_list_free(q->sel_before);
  for (int i = 0; i < 4; i++) {
    if (q->held[i]) {
      css_obj_release_external(q->held[i]);
    }
  }
  memset(q, 0, sizeof *q);
}

/// Make the public event of `e`. `copy`: the arrays are copied, because the delivery comes after the mutator returns. Takes `e->selectors_before`.
static void qevent_build(css_core * core, const css_cssom_event * e, bool copy, css_qevent * q) {
  memset(q, 0, sizeof *q);
  GTEXT_CSSOM_Event * p = &q->pub;
  p->size = sizeof *p;
  p->kind = e->kind;
  p->generation = e->generation;
  p->sheet_id = e->sheet_id;
  p->rule_id = e->rule_id;
  q->held[0] = hold((css_obj *)e->sheet);
  q->held[1] = hold((css_obj *)e->rule);
  q->held[2] = hold((css_obj *)e->declaration);
  q->held[3] = hold((css_obj *)e->scope);
  p->sheet = (GTEXT_CSSOM_Sheet *)e->sheet;
  p->rule = (GTEXT_CSSOM_Rule *)e->rule;
  p->declaration = (GTEXT_CSSOM_Decl *)e->declaration;
  p->scope = (GTEXT_CSSOM_Scope *)e->scope;
  p->element = e->element;
  p->attribute_written = e->attribute_written;
  p->all_changed = e->all_changed;
  p->summary_complete = true;
  bool complete = true;

  /* the properties */
  if (e->property_count) {
    if (copy) {
      q->props = gtext_allocator_malloc(&core->alloc, e->property_count * sizeof *q->props);
      if (q->props) {
        memcpy(q->props, e->properties, e->property_count * sizeof *q->props);
        p->properties = q->props;
        p->property_count = e->property_count;
      }
      else {
        complete = false;
      }
    }
    else {
      p->properties = e->properties;
      p->property_count = e->property_count;
    }
  }
  if (e->custom_count) {
    if (copy) {
      size_t bytes = 0;
      for (size_t i = 0; i < e->custom_count; i++) {
        bytes += e->custom_names[i].length;
      }
      char * block = gtext_allocator_malloc(&core->alloc, e->custom_count * sizeof *q->customs + bytes);
      if (block) {
        q->customs = (GTEXT_CSS_String *)(void *)block;
        char * cursor = block + e->custom_count * sizeof *q->customs;
        for (size_t i = 0; i < e->custom_count; i++) {
          if (e->custom_names[i].length) {
            memcpy(cursor, e->custom_names[i].data, e->custom_names[i].length);
          }
          q->customs[i].data = cursor;
          q->customs[i].length = e->custom_names[i].length;
          cursor += e->custom_names[i].length;
        }
        p->custom_names = q->customs;
        p->custom_count = e->custom_count;
      }
      else {
        complete = false;
      }
    }
    else {
      p->custom_names = e->custom_names;
      p->custom_count = e->custom_count;
    }
  }

  /* the rule's place and the affected set */
  if (e->rule) {
    if (!make_path(core, e, q)) {
      complete = false;
    }
    if (!rule_summary(core, e, q)) {
      complete = false;
    }
  }
  else {
    switch (e->kind) {
    case GTEXT_CSSOM_EVENT_SHEET_ADDED:
    case GTEXT_CSSOM_EVENT_SHEET_REMOVED:
    case GTEXT_CSSOM_EVENT_SHEET_DISABLED_CHANGED:
    case GTEXT_CSSOM_EVENT_ADOPTED_CHANGED:
    case GTEXT_CSSOM_EVENT_IMPORT_COMPLETED:
    case GTEXT_CSSOM_EVENT_RULE_INSERTED:
    case GTEXT_CSSOM_EVENT_RULE_REMOVED:
    case GTEXT_CSSOM_EVENT_RULE_REPLACED:
      everything(&p->selector_dependencies);
      break;
    case GTEXT_CSSOM_EVENT_SHEET_MEDIA_CHANGED: {
      everything(&p->selector_dependencies);
      const css_sheet * s = (const css_sheet *)e->sheet;
      const char * text = s && s->media_text ? s->media_text : "";
      p->has_condition = true;
      if (!condition_of_text(core, text, strlen(text), false, &p->condition_dependencies)) {
        condition_everything(&p->condition_dependencies);
        complete = false;
      }
      break;
    }
    case GTEXT_CSSOM_EVENT_CONDITIONS_CHANGED:
      /* the environment is replaced by another: which aspects differ is not known, and every conditional rule may have changed */
      p->has_condition = true;
      condition_everything(&p->condition_dependencies);
      break;
    default:
      break;
    }
  }
  if (!complete) {
    p->summary_complete = false;
    core->counters.events_degraded++;
    if (!p->has_path && e->rule) {
      /* the dependencies are what a consumer relies on: say anything may have changed */
      everything(&p->selector_dependencies);
    }
  }
  /* the list captured before a replace is the event's now */
  if (e->selectors_before && !q->sel_before) {
    gtext_css_selector_list_free((GTEXT_CSS_Selector_List *)e->selectors_before);
  }
}

/* ---- delivery ----------------------------------------------------------------------------------------------------------------------------------------- */

static void compact(css_core * core) {
  size_t out = 0;
  for (size_t i = 0; i < core->nobservers; i++) {
    if (!core->observers[i].dead) {
      core->observers[out++] = core->observers[i];
    }
  }
  core->nobservers = out;
}

static void free_queue(css_core * core) {
  GTEXT_Allocator a = core->alloc;
  const size_t n = core->qcount;
  css_qevent * queue = core->queue;
  core->queue = NULL;
  core->qcount = core->qcap = 0;
  for (size_t i = 0; i < n; i++) {
    qevent_free(core, &queue[i]);
  }
  gtext_allocator_free(&a, queue);
}

static void delivery_begin(css_core * core) {
  css_core_retain(core);
  core->delivering++;
  core->in_callback++;
}

static void delivery_end(css_core * core) {
  core->in_callback--;
  if (--core->delivering == 0) {
    compact(core);
    if (!core->document) {
      GTEXT_Allocator a = core->alloc;
      if (core->queue) {
        free_queue(core);
      }
      gtext_allocator_free(&a, core->observers);
      core->observers = NULL;
      core->nobservers = core->capobservers = 0;
    }
  }
  css_core_release(core);
}

/// Call every observer that was registered before the event was made. The first failing status is `outcome->observer_status`.
static void deliver_one(css_core * core, css_qevent * q, GTEXT_CSSOM_Outcome * outcome) {
  q->pub.detached_since = q->pub.rule != NULL && css_rule_sheet((const css_rule *)q->pub.rule) == NULL;
  core->counters.events_delivered++;
  const size_t n = core->nobservers;
  for (size_t i = 0; i < n && core->document; i++) {
    if (i >= core->nobservers) {
      break;
    }
    const css_observer o = core->observers[i];
    if (o.dead || o.since >= q->pub.generation) {
      continue;
    }
    core->counters.event_calls++;
    const GTEXT_CSS_Status st = o.fn(o.user, &q->pub);
    if (st != GTEXT_CSS_OK && outcome && outcome->observer_status == GTEXT_CSS_OK) {
      outcome->observer_status = st;
    }
  }
}

void css_events_emit(css_core * core, const css_cssom_event * e, GTEXT_CSSOM_Outcome * outcome) {
  const bool queued = core->batch_depth > 0;
  css_qevent q;
  qevent_build(core, e, queued, &q);
  if (queued) {
    if (core->qcount < core->qcap) {
      core->queue[core->qcount++] = q;
    }
    else {
      /* room was reserved by css_cssom_enter(); a mutator that did not is a defect, and the loss is counted, never silent */
      core->counters.events_lost++;
      qevent_free(core, &q);
      if (outcome && outcome->observer_status == GTEXT_CSS_OK) {
        outcome->observer_status = GTEXT_CSS_E_OOM;
      }
    }
    return;
  }
  delivery_begin(core);
  deliver_one(core, &q, outcome);
  /* freed while the delivery still holds the core: the last release of it may be the observer's, and the allocator is the core's */
  qevent_free(core, &q);
  delivery_end(core);
}

bool css_events_wanted(const css_core * core) {
  return core->live_observers > 0;
}

/* ---- the queue ---------------------------------------------------------------------------------------------------------------------------------------- */

GTEXT_CSS_Status css_cssom_reserve_events(css_core * core, size_t n) {
  if (core->batch_depth == 0 || core->live_observers == 0 || n == 0) {
    return GTEXT_CSS_OK;
  }
  const size_t limit = core->config.max_queued_events;
  if (n > limit || core->qcount > limit - n) {
    return GTEXT_CSS_E_LIMIT_CSSOM;
  }
  const size_t need = core->qcount + n;
  if (need <= core->qcap) {
    return GTEXT_CSS_OK;
  }
  size_t cap = core->qcap ? core->qcap : 16;
  while (cap < need) {
    cap = cap > limit / 2 ? limit : cap * 2;
    if (cap >= limit) {
      cap = limit;
      break;
    }
  }
  if (cap < need) {
    cap = need;
  }
  css_qevent * grown = gtext_allocator_realloc(&core->alloc, core->queue, cap * sizeof *grown);
  if (!grown) {
    return GTEXT_CSS_E_OOM;
  }
  core->queue = grown;
  core->qcap = cap;
  return GTEXT_CSS_OK;
}

void css_events_teardown(css_core * core) {
  /* the queue's holds on objects go, so that nothing the batch kept alive keeps the core alive */
  core->batch_depth = 0;
  core->live_observers = 0;
  for (size_t i = 0; i < core->nobservers; i++) {
    core->observers[i].dead = true;
  }
  if (core->delivering > 0) {
    return;   /* the delivery that is running frees the rest when it ends (delivery_end) */
  }
  free_queue(core);
  GTEXT_Allocator a = core->alloc;
  gtext_allocator_free(&a, core->observers);
  core->observers = NULL;
  core->nobservers = core->capobservers = 0;
}

/* ---- the public calls ----------------------------------------------------------------------------------------------------------------------------------- */

GTEXT_CSS_Status gtext_cssom_document_observe(GTEXT_CSSOM_Document * document, GTEXT_CSSOM_Observer_Fn fn, void * user, GTEXT_CSSOM_Observer_Handle * handle) {
  if (!document || !fn) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = css_document_core(document);
  if (core->live_observers >= core->config.max_observers) {
    return GTEXT_CSS_E_LIMIT_CSSOM;
  }
  if (core->nobservers == core->capobservers) {
    const size_t cap = core->capobservers ? core->capobservers * 2 : 4;
    css_observer * grown = gtext_allocator_realloc(&core->alloc, core->observers, cap * sizeof *grown);
    if (!grown) {
      return GTEXT_CSS_E_OOM;
    }
    core->observers = grown;
    core->capobservers = cap;
  }
  css_observer * o = &core->observers[core->nobservers++];
  o->handle = ++core->next_observer;
  o->fn = fn;
  o->user = user;
  o->since = core->generation;
  o->dead = false;
  core->live_observers++;
  if (handle) {
    *handle = o->handle;
  }
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_document_unobserve(GTEXT_CSSOM_Document * document, GTEXT_CSSOM_Observer_Handle handle) {
  if (!document) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = css_document_core(document);
  for (size_t i = 0; i < core->nobservers; i++) {
    css_observer * o = &core->observers[i];
    if (o->handle == handle && !o->dead) {
      core->live_observers--;
      if (core->delivering > 0) {
        o->dead = true;
      }
      else {
        memmove(o, o + 1, (core->nobservers - i - 1) * sizeof *o);
        core->nobservers--;
      }
      return GTEXT_CSS_OK;
    }
  }
  return GTEXT_CSS_E_NOT_FOUND;
}

size_t gtext_cssom_document_observer_count(const GTEXT_CSSOM_Document * document) {
  return document ? css_document_core((GTEXT_CSSOM_Document *)(uintptr_t)document)->live_observers : 0;
}

GTEXT_CSS_Status gtext_cssom_document_begin_batch(GTEXT_CSSOM_Document * document) {
  if (!document) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = css_document_core(document);
  if (core->in_callback || core->in_finalizer) {
    return GTEXT_CSS_E_REENTRANT;
  }
  if (core->batch_depth == (unsigned)-1) {
    return GTEXT_CSS_E_LIMIT_CSSOM;
  }
  core->batch_depth++;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_document_end_batch(GTEXT_CSSOM_Document * document, GTEXT_CSSOM_Outcome * outcome) {
  if (!document) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = css_document_core(document);
  css_outcome_reset(outcome, core);
  if (core->in_callback || core->in_finalizer) {
    return GTEXT_CSS_E_REENTRANT;
  }
  if (core->batch_depth == 0) {
    return GTEXT_CSS_E_INVALID_STATE;
  }
  if (--core->batch_depth > 0) {
    return GTEXT_CSS_OK;
  }
  if (core->qcount == 0) {
    return GTEXT_CSS_OK;
  }
  delivery_begin(core);
  /* the queue cannot grow while it is delivered: nothing mutates inside a callback, and the depth is 0 */
  for (size_t i = 0; i < core->qcount; i++) {
    css_qevent * q = &core->queue[i];
    if (core->document) {
      deliver_one(core, q, outcome);
    }
    qevent_free(core, q);
  }
  core->qcount = 0;
  delivery_end(core);
  return GTEXT_CSS_OK;
}

size_t gtext_cssom_document_batch_depth(const GTEXT_CSSOM_Document * document) {
  return document ? css_document_core((GTEXT_CSSOM_Document *)(uintptr_t)document)->batch_depth : 0;
}
