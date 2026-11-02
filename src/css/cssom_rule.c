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
 * The rule object (css_cssom_rule.h): its source (one syntax tree and the text it indexes, shared by the rules of one parse), its creation and destruction, the identity and
 * the keep-alive that the object base gives it, the parent chain from which `parent_sheet` is derived, the lazy declaration block and the hook that makes a mutation of it a
 * mutation of the rule and of its sheet.
 */

#include "cssom_typed.h"
#include <string.h>

#include "css_internal.h"
#include "css_shorthand_internal.h"
#include "cssom_rule_internal.h"

/* ---- the source ------------------------------------------------------------------------------------------------------------------------------ */

GTEXT_CSS_Status css_source_new(css_core * core, GTEXT_CSS_Tree * tree, char * text, size_t text_length, css_source ** out) {
  GTEXT_Allocator a = core->alloc;
  const size_t tree_bytes = gtext_css_tree_bytes(tree);
  const size_t held = tree_bytes + (text ? text_length + 1 : 0);
  const size_t bytes = sizeof(css_source) + held;
  GTEXT_CSS_Status st = css_core_room_bytes(core, bytes);
  if (st != GTEXT_CSS_OK) {
    gtext_allocator_free(&a, text);
    return st;
  }
  css_source * s = gtext_allocator_calloc(&a, 1, sizeof *s);
  if (!s) {
    gtext_allocator_free(&a, text);
    return GTEXT_CSS_E_OOM;
  }
  s->refs = 1;
  s->core = core;
  s->tree = gtext_css_tree_retain(tree);
  s->text = text;
  s->text_length = text_length;
  s->id = core->next_id++;
  s->bytes = bytes;
  core->counters.live_bytes += bytes;
  core->counters.tree_bytes += held;
  css_core_retain(core);
  *out = s;
  return GTEXT_CSS_OK;
}

void css_source_set_ns(css_source * source, css_ns_env * env) {
  css_ns_env * old = source->ns;
  source->ns = css_ns_env_retain(env);
  css_ns_env_release(old);
}

css_source * css_source_retain(css_source * source) {
  if (source) {
    source->refs++;
  }
  return source;
}

void css_source_release(css_source * source) {
  if (!source || --source->refs != 0) {
    return;
  }
  css_core * core = source->core;
  GTEXT_Allocator a = core->alloc;
  const size_t held = source->bytes - sizeof(css_source);
  core->counters.live_bytes -= source->bytes;
  core->counters.tree_bytes -= held;
  css_ns_env_release(source->ns);
  gtext_css_tree_free(source->tree);
  gtext_allocator_free(&a, source->text);
  gtext_allocator_free(&a, source);
  css_core_release(core);
}

/* ---- the rule -------------------------------------------------------------------------------------------------------------------------------- */

css_rule * css_rule_from_obj(css_obj * o) {
  return (css_rule *)o;
}

static void rule_destroy(css_obj * o) {
  css_rule * r = (css_rule *)o;
  GTEXT_Allocator a = o->core->alloc;
  css_rule_drop_cache(r);
  if (r->import_sheet) {
    r->import_sheet->owner_rule = NULL;
    css_obj_release_internal(&r->import_sheet->obj);
  }
  css_rule_list_release_all(&r->rules);
  if (r->style) {
    r->style->owner = NULL;
    css_obj_release_internal(&r->style->obj);
  }
  css_media_owner_gone(&r->media);
  css_source_release(r->source);
  gtext_allocator_free(&a, r);
}

GTEXT_CSS_Status css_rule_create(css_core * core, GTEXT_CSSOM_Rule_Kind kind, css_source * source, const GTEXT_CSS_Rule * node, const GTEXT_CSS_Declaration * run, size_t run_count,
    bool nested, css_rule ** out) {
  GTEXT_CSS_Status st = css_core_room(core, sizeof(css_rule));
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_Allocator a = core->alloc;
  css_rule * r = gtext_allocator_calloc(&a, 1, sizeof *r);
  if (!r) {
    return GTEXT_CSS_E_OOM;
  }
  css_obj_init(&r->obj, core, GTEXT_CSSOM_OBJECT_RULE, 1, 0, sizeof *r, rule_destroy);
  r->kind = (uint8_t)kind;
  r->source_kind = GTEXT_CSSOM_SOURCE_PARSED;
  r->has_block = kind == GTEXT_CSSOM_RULE_LAYER && node && node->has_block;
  r->nested = nested;
  r->size = 1;
  r->rules.owner = &r->obj;
  r->source = css_source_retain(source);
  r->node = node;
  r->run = run;
  r->run_count = run_count;
  *out = r;
  return GTEXT_CSS_OK;
}

css_sheet * css_rule_sheet(const css_rule * rule) {
  /* the chain of owners: a rule is in a list, whose owner is a rule or the sheet; a rule in no list ends the chain */
  while (rule->list) {
    const css_obj * owner = rule->list->owner;
    if (owner->kind == GTEXT_CSSOM_OBJECT_SHEET) {
      return (css_sheet *)(uintptr_t)owner;
    }
    rule = (const css_rule *)(uintptr_t)owner;
  }
  return NULL;
}

GTEXT_CSS_Span css_rule_span(const css_rule * rule) {
  if (rule->node) {
    return rule->node->span;
  }
  GTEXT_CSS_Span s;
  memset(&s, 0, sizeof s);
  if (rule->run && rule->run_count) {
    s = rule->run[0].span;
    s.end = rule->run[rule->run_count - 1].span.end;
  }
  return s;
}

bool css_rule_declarations(const css_rule * rule, const GTEXT_CSS_Declaration ** declarations, size_t * count) {
  switch ((GTEXT_CSSOM_Rule_Kind)rule->kind) {
  case GTEXT_CSSOM_RULE_STYLE:
    *declarations = rule->node->declarations;
    *count = rule->node->declaration_count;
    return true;
  case GTEXT_CSSOM_RULE_NESTED_DECLARATIONS:
    if (rule->node) {
      *declarations = rule->node->declarations;
      *count = rule->node->declaration_count;
    }
    else {
      *declarations = rule->run;
      *count = rule->run_count;
    }
    return true;
  default:
    return false;
  }
}

/* ---- the public object ------------------------------------------------------------------------------------------------------------------------ */

GTEXT_CSSOM_Rule * gtext_cssom_rule_retain(GTEXT_CSSOM_Rule * rule) {
  if (rule) {
    css_obj_retain_external(&rule->obj);
  }
  return rule;
}

void gtext_cssom_rule_release(GTEXT_CSSOM_Rule * rule) {
  if (rule) {
    css_obj_release_external(&rule->obj);
  }
}

GTEXT_CSSOM_Rule_Kind gtext_cssom_rule_kind(const GTEXT_CSSOM_Rule * rule) {
  return rule ? (GTEXT_CSSOM_Rule_Kind)rule->kind : GTEXT_CSSOM_RULE_UNKNOWN;
}

unsigned gtext_cssom_rule_type(const GTEXT_CSSOM_Rule * rule) {
  return rule ? css_kind((GTEXT_CSSOM_Rule_Kind)rule->kind)->legacy_type : 0;
}

const char * gtext_cssom_rule_interface_name(const GTEXT_CSSOM_Rule * rule) {
  if (!rule) {
    return "CSSRule";
  }
  const char * name = css_kind((GTEXT_CSSOM_Rule_Kind)rule->kind)->iface;
  if (rule->kind == GTEXT_CSSOM_RULE_LAYER) {
    return rule->has_block ? "CSSLayerBlockRule" : "CSSLayerStatementRule";
  }
  return name[0] ? name : "CSSRule";
}

uint64_t gtext_cssom_rule_id(const GTEXT_CSSOM_Rule * rule) {
  return rule ? rule->obj.id : 0;
}

uint64_t gtext_cssom_rule_version(const GTEXT_CSSOM_Rule * rule) {
  return rule ? rule->version : 0;
}

bool gtext_cssom_rule_has_block(const GTEXT_CSSOM_Rule * rule) {
  return rule && rule->has_block;
}

GTEXT_CSSOM_Rule * gtext_cssom_rule_parent_rule(const GTEXT_CSSOM_Rule * rule) {
  if (!rule || !rule->list || rule->list->owner->kind != GTEXT_CSSOM_OBJECT_RULE) {
    return NULL;
  }
  return gtext_cssom_rule_retain((GTEXT_CSSOM_Rule *)(uintptr_t)rule->list->owner);
}

GTEXT_CSSOM_Sheet * gtext_cssom_rule_parent_sheet(const GTEXT_CSSOM_Rule * rule) {
  css_sheet * s = rule ? css_rule_sheet(rule) : NULL;
  if (s) {
    css_obj_retain_external(&s->obj);
  }
  return s;
}

bool gtext_cssom_rule_detached(const GTEXT_CSSOM_Rule * rule) {
  return !rule || css_rule_sheet(rule) == NULL;
}

const GTEXT_CSSOM_Rule_List * gtext_cssom_rule_css_rules(const GTEXT_CSSOM_Rule * rule) {
  if (!rule) {
    return NULL;
  }
  const unsigned flags = css_kind((GTEXT_CSSOM_Rule_Kind)rule->kind)->flags;
  if (!(flags & CSS_KIND_LIST_ANY)) {
    return NULL;
  }
  if (rule->kind == GTEXT_CSSOM_RULE_LAYER && !rule->has_block) {
    return NULL;
  }
  return &rule->rules;
}

GTEXT_CSS_Status gtext_cssom_rule_source(const GTEXT_CSSOM_Rule * rule, GTEXT_CSSOM_Source_Info * out) {
  if (!rule || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  out->kind = (GTEXT_CSSOM_Source_Kind)rule->source_kind;
  out->source_id = rule->source ? rule->source->id : 0;
  out->span = css_rule_span(rule);
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_rule_source_text(const GTEXT_CSSOM_Rule * rule, char ** out, size_t * length) {
  if (!rule || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  if (rule->source_kind != GTEXT_CSSOM_SOURCE_PARSED || !rule->source || !rule->source->text) {
    return GTEXT_CSS_E_INVALID_STATE;
  }
  const GTEXT_CSS_Span span = css_rule_span(rule);
  if (span.end < span.offset || span.end > rule->source->text_length) {
    return GTEXT_CSS_E_INVALID_STATE;
  }
  char * copy = css_core_dup(rule->obj.core, rule->source->text + span.offset, span.end - span.offset);
  if (!copy) {
    return GTEXT_CSS_E_OOM;
  }
  *out = copy;
  if (length) {
    *length = span.end - span.offset;
  }
  return GTEXT_CSS_OK;
}

/* ---- the list --------------------------------------------------------------------------------------------------------------------------------- */

size_t gtext_cssom_rule_list_length(const GTEXT_CSSOM_Rule_List * list) {
  return list ? list->count : 0;
}

GTEXT_CSSOM_Rule * gtext_cssom_rule_list_at(const GTEXT_CSSOM_Rule_List * list, size_t index) {
  return list ? gtext_cssom_rule_retain(css_rule_list_at(list, index)) : NULL;
}

GTEXT_CSSOM_Rule * gtext_cssom_rule_list_first(const GTEXT_CSSOM_Rule_List * list) {
  return list ? gtext_cssom_rule_retain(css_rule_list_first(list)) : NULL;
}

GTEXT_CSSOM_Rule * gtext_cssom_rule_list_next(const GTEXT_CSSOM_Rule_List * list, const GTEXT_CSSOM_Rule * rule) {
  if (!list || !rule || rule->list != list) {
    return NULL;
  }
  return gtext_cssom_rule_retain(css_rule_list_next(rule));
}

bool gtext_cssom_rule_list_index_of(const GTEXT_CSSOM_Rule_List * list, const GTEXT_CSSOM_Rule * rule, size_t * index) {
  if (!list || !rule || rule->list != list) {
    return false;
  }
  if (index) {
    *index = css_rule_list_index_of(rule);
  }
  return true;
}

/* ---- the declaration block of a rule ------------------------------------------------------------------------------------------------------- */

static GTEXT_CSS_Status rule_prepare(css_obj * owner, GTEXT_CSSOM_Decl * decl) {
  (void)owner;
  (void)decl;
  return GTEXT_CSS_OK;
}

/// After a mutation of the rule's declarations: the rule's version moves, and so does its sheet's; a rule that no sheet holds changes nothing a document can see.
static void rule_commit(css_obj * owner, GTEXT_CSSOM_Decl * decl, css_cssom_event * event) {
  (void)decl;
  css_rule * r = (css_rule *)owner;
  css_rule_touch(r);
  event->rule = r;
  event->rule_id = r->obj.id;
  css_sheet * s = css_rule_sheet(r);
  if (s) {
    css_sheet_bump(s);
    event->sheet = s;
    event->sheet_id = s->obj.id;
  }
  else {
    event->silent = true;
  }
}

static const css_decl_owner_ops rule_ops = {rule_prepare, rule_commit};

GTEXT_CSS_Status css_rule_build_style(css_rule * r) {
  if (r->style) {
    return GTEXT_CSS_OK;
  }
  css_core * core = r->obj.core;
  GTEXT_CSS_Declaration_Block * block = NULL;
  css_desc * descs = NULL;
  size_t ndescs = 0;
  const bool typed = css_typed_kind((GTEXT_CSSOM_Rule_Kind)r->kind);
  GTEXT_CSS_Status st;
  if (typed) {
    st = css_typed_view(r, &block, &descs, &ndescs);
  }
  else {
    const GTEXT_CSS_Declaration * decls = NULL;
    size_t n = 0;
    if (!css_rule_declarations(r, &decls, &n)) {
      return GTEXT_CSS_E_UNSUPPORTED;
    }
    st = css_decl_block_from_declarations(core, decls, n, false, &block);
  }
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSSOM_Decl * d = NULL;
  st = css_decl_create(core, GTEXT_CSSOM_DECL_RULE, 1, 0, r->obj.id, &d);
  if (st != GTEXT_CSS_OK) {
    gtext_css_block_free(block);
    if (typed) {
      GTEXT_Allocator a = core->alloc;
      for (size_t i = 0; i < ndescs; i++) {
        gtext_allocator_free(&a, descs[i].name);
        gtext_allocator_free(&a, descs[i].text);
      }
      gtext_allocator_free(&a, descs);
    }
    return st;
  }
  gtext_css_block_free(d->block);
  d->block = block;
  d->typed = typed;
  d->descs = descs;
  d->ndescs = d->capdescs = ndescs;
  d->owner = &r->obj;
  d->ops = &rule_ops;
  css_obj_set_parent(&d->obj, &r->obj);
  r->style = d;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_rule_style(GTEXT_CSSOM_Rule * rule, GTEXT_CSSOM_Decl ** out) {
  if (!rule || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  if (!rule->style) {
    const GTEXT_CSS_Status st = css_rule_build_style(rule);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
  }
  *out = gtext_cssom_decl_retain(rule->style);
  return GTEXT_CSS_OK;
}
