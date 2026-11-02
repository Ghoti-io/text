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
 * The CSSOM document and its core (css_cssom_core.h): the configuration and its defaults, the reference counted core every object retains, the generation
 * counter, the internal emit seam and the reentrancy flag, the counters and the tree-version hooks.
 */

#include <stdlib.h>
#include <string.h>

#include "css_internal.h"
#include "css_registry_internal.h"
#include "css_shorthand_internal.h"
#include "cssom_doc_internal.h"
#include "cssom_internal.h"

#define DEFAULT_MAX_LIVE_OBJECTS ((size_t)1 << 24)
#define DEFAULT_MAX_LIVE_BYTES ((size_t)1 << 30)
#define DEFAULT_MAX_INLINE_STYLES ((size_t)1 << 20)
#define DEFAULT_MAX_DECLARATIONS ((size_t)1 << 20)
#define LIMIT_MAX_DECLARATIONS ((size_t)1 << 28)
#define DEFAULT_MAX_INPUT ((size_t)64 << 20)
#define DEFAULT_MAX_OUTPUT ((size_t)64 << 20)
#define DEFAULT_MAX_WORK ((size_t)1 << 24)
#define DEFAULT_MAX_RULES ((size_t)1 << 20)
#define DEFAULT_MAX_RULE_NESTING ((size_t)64)
#define LIMIT_MAX_RULE_NESTING ((size_t)4096)
#define DEFAULT_MAX_DIAGNOSTICS ((size_t)1024)
#define DEFAULT_MAX_CACHED_TEXT ((size_t)64 << 20)
#define LIMIT_MAX_RULES ((size_t)1 << 28)
#define DEFAULT_MAX_SCOPES ((size_t)1 << 16)
#define DEFAULT_MAX_ADOPTED ((size_t)1 << 20)
#define DEFAULT_MAX_MEDIA_QUERY_LISTS ((size_t)1 << 20)
#define DEFAULT_MAX_OBSERVERS ((size_t)1024)
#define DEFAULT_MAX_QUEUED_EVENTS ((size_t)1 << 20)

struct GTEXT_CSSOM_Document {
  css_core * core;
};

GTEXT_CSSOM_Config gtext_cssom_config_default(void) {
  GTEXT_CSSOM_Config c;
  memset(&c, 0, sizeof c);
  c.size = sizeof c;
  c.first_generation = 0;
  c.first_id = 1;
  c.max_live_objects = DEFAULT_MAX_LIVE_OBJECTS;
  c.max_live_bytes = DEFAULT_MAX_LIVE_BYTES;
  c.max_inline_styles = DEFAULT_MAX_INLINE_STYLES;
  c.max_declarations = DEFAULT_MAX_DECLARATIONS;
  c.max_input_bytes = DEFAULT_MAX_INPUT;
  c.max_output_bytes = DEFAULT_MAX_OUTPUT;
  c.max_work = DEFAULT_MAX_WORK;
  c.max_rules = DEFAULT_MAX_RULES;
  c.max_rule_nesting = DEFAULT_MAX_RULE_NESTING;
  c.max_diagnostics = DEFAULT_MAX_DIAGNOSTICS;
  c.max_cached_text_bytes = DEFAULT_MAX_CACHED_TEXT;
  c.max_scopes = DEFAULT_MAX_SCOPES;
  c.max_adopted = DEFAULT_MAX_ADOPTED;
  c.max_media_query_lists = DEFAULT_MAX_MEDIA_QUERY_LISTS;
  c.max_observers = DEFAULT_MAX_OBSERVERS;
  c.max_queued_events = DEFAULT_MAX_QUEUED_EVENTS;
  return c;
}

/// The caller's config over the defaults: a field past the caller's `size` is the default, a field left at 0 is the default (the counters excepted: 0 is a start).
static void config_fill(GTEXT_CSSOM_Config * out, const GTEXT_CSSOM_Config * in) {
  *out = gtext_cssom_config_default();
  const size_t n = in->size < sizeof *in ? in->size : sizeof *in;
  memcpy(out, in, n);
  out->size = sizeof *out;
  const GTEXT_CSSOM_Config d = gtext_cssom_config_default();
  if (n < offsetof(GTEXT_CSSOM_Config, first_id) + sizeof in->first_id) {
    out->first_id = d.first_id;
  }
  if (out->first_id == 0) {
    out->first_id = 1;
  }
  if (!out->max_live_objects) out->max_live_objects = d.max_live_objects;
  if (!out->max_live_bytes) out->max_live_bytes = d.max_live_bytes;
  if (!out->max_inline_styles) out->max_inline_styles = d.max_inline_styles;
  if (!out->max_declarations) out->max_declarations = d.max_declarations;
  if (out->max_declarations > LIMIT_MAX_DECLARATIONS) out->max_declarations = LIMIT_MAX_DECLARATIONS;
  if (!out->max_input_bytes) out->max_input_bytes = d.max_input_bytes;
  if (!out->max_output_bytes) out->max_output_bytes = d.max_output_bytes;
  if (!out->max_work) out->max_work = d.max_work;
  if (!out->max_rules) out->max_rules = d.max_rules;
  if (out->max_rules > LIMIT_MAX_RULES) out->max_rules = LIMIT_MAX_RULES;
  if (!out->max_rule_nesting) out->max_rule_nesting = d.max_rule_nesting;
  if (out->max_rule_nesting > LIMIT_MAX_RULE_NESTING) out->max_rule_nesting = LIMIT_MAX_RULE_NESTING;
  if (!out->max_diagnostics) out->max_diagnostics = d.max_diagnostics;
  if (n < offsetof(GTEXT_CSSOM_Config, max_cached_text_bytes) + sizeof in->max_cached_text_bytes || !out->max_cached_text_bytes) {
    out->max_cached_text_bytes = d.max_cached_text_bytes;
  }
  /* the document's fields: a caller built against the header of C5c1 has none of them */
  if (n < offsetof(GTEXT_CSSOM_Config, max_media_query_lists) + sizeof in->max_media_query_lists) {
    out->conditions = NULL;
    out->element_interface = NULL;
    out->match_options = NULL;
    out->import = NULL;
    out->import_user = NULL;
    out->max_scopes = 0;
    out->max_adopted = 0;
    out->max_media_query_lists = 0;
  }
  if (!out->max_scopes) out->max_scopes = d.max_scopes;
  if (!out->max_adopted) out->max_adopted = d.max_adopted;
  if (!out->max_media_query_lists) out->max_media_query_lists = d.max_media_query_lists;
  /* the observers' fields: a caller built against the header of C5c2 has none of them */
  if (n < offsetof(GTEXT_CSSOM_Config, max_queued_events) + sizeof in->max_queued_events) {
    out->max_observers = 0;
    out->max_queued_events = 0;
  }
  if (!out->max_observers) out->max_observers = d.max_observers;
  if (!out->max_queued_events) out->max_queued_events = d.max_queued_events;
}

GTEXT_CSS_Status gtext_cssom_document_new(const GTEXT_CSSOM_Config * config, GTEXT_CSSOM_Document ** out) {
  if (!config || !out || config->size < offsetof(GTEXT_CSSOM_Config, allocator) || !config->registry) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSSOM_Config cfg;
  config_fill(&cfg, config);
  const GTEXT_Allocator * al = cfg.allocator ? cfg.allocator : css_registry_allocator(cfg.registry);
  GTEXT_Allocator a = *al;
  css_core * core = gtext_allocator_calloc(&a, 1, sizeof *core);
  struct GTEXT_CSSOM_Document * doc = gtext_allocator_calloc(&a, 1, sizeof *doc);
  if (!core || !doc) {
    gtext_allocator_free(&a, core);
    gtext_allocator_free(&a, doc);
    return GTEXT_CSS_E_OOM;
  }
  core->refs = 1;   /* the document handle */
  core->alloc = a;
  core->registry = cfg.registry;
  core->generation = cfg.first_generation;
  core->next_id = cfg.first_id;
  core->next_context = 1;
  core->document = doc;
  doc->core = core;
  /* the conditions: the caller's, retained, or the registry's defaults */
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  if (cfg.conditions) {
    core->conditions = gtext_css_conditions_retain(cfg.conditions);
  }
  else {
    GTEXT_CSS_Conditions_Options co = gtext_css_conditions_options_default();
    co.allocator = &core->alloc;
    co.element_interface = cfg.element_interface;
    st = gtext_css_conditions_new(cfg.registry, &co, &core->conditions);
  }
  if (st == GTEXT_CSS_OK && cfg.match_options) {
    core->match = *cfg.match_options;
    core->has_match = true;
  }
  cfg.allocator = NULL;   /* the core's copy is `alloc`; the pointer the caller gave need not outlive this call */
  cfg.conditions = core->conditions;
  cfg.match_options = core->has_match ? &core->match : NULL;
  core->config = cfg;
  if (st == GTEXT_CSS_OK) {
    css_scope * scope = NULL;
    st = css_scope_create_document(core, &scope);
  }
  if (st != GTEXT_CSS_OK) {
    gtext_cssom_document_release(doc);
    return st;
  }
  *out = doc;
  return GTEXT_CSS_OK;
}

void css_core_release(css_core * core) {
  if (!core || --core->refs != 0) {
    return;
  }
  css_inline_map_free(core);
  css_index_teardown(core);
  gtext_css_conditions_free(core->conditions);
  GTEXT_Allocator a = core->alloc;
  gtext_allocator_free(&a, core->observers);
  gtext_allocator_free(&a, core->queue);
  gtext_allocator_free(&a, core->regs);
  gtext_allocator_free(&a, core->scopes);
  gtext_allocator_free(&a, core);
}

void gtext_cssom_document_release(GTEXT_CSSOM_Document * document) {
  if (!document) {
    return;
  }
  css_core * core = document->core;
  GTEXT_Allocator a = core->alloc;
  core->document = NULL;
  core->tap = NULL;
  core->tap_user = NULL;
  gtext_allocator_free(&a, document);
  css_events_teardown(core);
  css_scope_teardown_all(core);
  css_index_teardown(core);
  css_core_release(core);
}

uint64_t gtext_cssom_document_generation(const GTEXT_CSSOM_Document * document) {
  return document ? document->core->generation : 0;
}

void gtext_cssom_document_tree_changed(GTEXT_CSSOM_Document * document) {
  if (document) {
    document->core->tree_epoch++;
  }
}

GTEXT_CSS_Status gtext_cssom_document_counters(const GTEXT_CSSOM_Document * document, GTEXT_CSSOM_Counters * out) {
  if (!document || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  *out = document->core->counters;
  out->live_scopes = document->core->nscopes;
  return GTEXT_CSS_OK;
}

const GTEXT_Allocator * gtext_cssom_document_allocator(const GTEXT_CSSOM_Document * document) {
  return document ? &document->core->alloc : NULL;
}

void gtext_cssom_buffer_free(const GTEXT_Allocator * allocator, char * buffer) {
  if (!buffer) {
    return;
  }
  GTEXT_Allocator a = *(allocator ? allocator : gtext_allocator_default());
  gtext_allocator_free(&a, buffer);
}

/* ---- internal ---------------------------------------------------------------------------------------------------------------------------------------- */

css_core * css_document_core(GTEXT_CSSOM_Document * document) {
  return document ? document->core : NULL;
}

bool css_document_alive(const css_core * core) {
  return core->document != NULL;
}

GTEXT_CSS_Status css_cssom_enter(css_core * core) {
  if (core->in_callback || core->in_finalizer) {
    return GTEXT_CSS_E_REENTRANT;
  }
  if (core->generation == UINT64_MAX) {
    return GTEXT_CSS_E_LIMIT_CSSOM;
  }
  /* in a batch the event this mutation sends has its place before anything changes */
  return css_cssom_reserve_events(core, 1);
}

void css_cssom_commit(css_core * core, css_cssom_event * event, GTEXT_CSSOM_Outcome * outcome) {
  if (event->silent) {
    /* a rule that no sheet holds changed: nothing that evaluates the document can see it */
    if (outcome) {
      outcome->generation = core->generation;
    }
    gtext_css_selector_list_free((GTEXT_CSS_Selector_List *)event->selectors_before);
    return;
  }
  core->generation++;
  event->generation = core->generation;
  if (outcome) {
    outcome->generation = core->generation;
  }
  if (core->tap) {
    /* the object delivered is held by the delivery, so a release inside the callback cannot free it */
    css_obj * held = event->declaration;
    if (held) {
      css_obj_retain_external(held);
    }
    core->in_callback++;
    const GTEXT_CSS_Status st = core->tap(core->tap_user, event);
    core->in_callback--;
    if (st != GTEXT_CSS_OK && outcome && outcome->observer_status == GTEXT_CSS_OK) {
      outcome->observer_status = st;
    }
    if (held) {
      css_obj_release_external(held);
    }
  }
  if (core->live_observers > 0) {
    css_events_emit(core, event, outcome);
  }
  else {
    gtext_css_selector_list_free((GTEXT_CSS_Selector_List *)event->selectors_before);
  }
}

void css_cssom_set_tap(css_core * core, css_cssom_tap_fn fn, void * user) {
  core->tap = fn;
  core->tap_user = user;
}

char * css_core_dup(css_core * core, const char * s, size_t n) {
  char * d = gtext_allocator_malloc(&core->alloc, n + 1);
  if (d) {
    if (n) {
      memcpy(d, s, n);
    }
    d[n] = '\0';
  }
  return d;
}

void css_outcome_reset(GTEXT_CSSOM_Outcome * o, const css_core * core) {
  if (o) {
    memset(o, 0, sizeof *o);
    o->kind = GTEXT_CSSOM_UNCHANGED;
    o->observer_status = GTEXT_CSS_OK;
    o->generation = core->generation;
  }
}

uint64_t css_core_tree_version(const css_core * core, bool * has_hook) {
  if (has_hook) {
    *has_hook = core->config.tree_version != NULL;
  }
  return core->config.tree_version ? core->config.tree_version(core->config.tree_version_user) : 0;
}
