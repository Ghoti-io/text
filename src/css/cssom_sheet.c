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
 * The sheet object (css_cssom_sheet.h): its attributes, its creation from a text, its destruction, and the compaction that gives a mostly deleted sheet its memory back.
 */

#include <string.h>

#include "css_internal.h"
#include "cssom_rule_internal.h"

GTEXT_CSSOM_Sheet_Options gtext_cssom_sheet_options_default(void) {
  GTEXT_CSSOM_Sheet_Options o;
  memset(&o, 0, sizeof o);
  o.size = sizeof o;
  o.origin_clean = true;
  return o;
}

css_sheet * css_sheet_from_obj(css_obj * o) {
  return (css_sheet *)o;
}

static void sheet_destroy(css_obj * o) {
  css_sheet * s = (css_sheet *)o;
  GTEXT_Allocator a = o->core->alloc;
  css_rule_list_release_all(&s->rules);
  css_source_release(s->source);
  css_ns_env_release(s->ns);
  gtext_allocator_free(&a, s->href);
  gtext_allocator_free(&a, s->title);
  gtext_allocator_free(&a, s->base_url);
  gtext_allocator_free(&a, s->diags);
  gtext_allocator_free(&a, s);
}

/// A copy of an optional string; `*out` stays NULL for NULL. False on memory.
static bool copy_string(css_core * core, const char * s, char ** out, size_t * bytes) {
  *out = NULL;
  if (!s) {
    return true;
  }
  *out = css_core_dup(core, s, strlen(s));
  *bytes += strlen(s) + 1;
  return *out != NULL;
}

GTEXT_CSS_Status gtext_cssom_sheet_new_parsed(GTEXT_CSSOM_Document * document, const char * text, size_t length, const GTEXT_CSSOM_Sheet_Options * options,
    GTEXT_CSSOM_Sheet ** out) {
  if (!document || !out || (!text && length) || (options && options->size < offsetof(GTEXT_CSSOM_Sheet_Options, href) + sizeof options->href)) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = css_document_core(document);
  if (length > core->config.max_input_bytes) {
    return GTEXT_CSS_E_LIMIT_INPUT;
  }
  GTEXT_CSSOM_Sheet_Options o = gtext_cssom_sheet_options_default();
  if (options) {
    const size_t n = options->size < sizeof o ? options->size : sizeof o;
    memcpy(&o, options, n);
    o.size = sizeof o;
  }
  GTEXT_CSS_Status st = css_core_room(core, sizeof(css_sheet));
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_Allocator a = core->alloc;
  css_sheet * s = gtext_allocator_calloc(&a, 1, sizeof *s);
  if (!s) {
    return GTEXT_CSS_E_OOM;
  }
  css_obj_init(&s->obj, core, GTEXT_CSSOM_OBJECT_SHEET, 0, 1, sizeof *s, sheet_destroy);
  s->rules.owner = &s->obj;
  s->disabled = o.disabled;
  s->alternate = o.alternate;
  s->origin_clean = o.origin_clean;
  s->owner_node = o.owner_node;
  size_t strings = 0;
  if (!copy_string(core, o.href, &s->href, &strings) || !copy_string(core, o.title, &s->title, &strings) || !copy_string(core, o.base_url, &s->base_url, &strings)) {
    css_obj_release_external(&s->obj);
    return GTEXT_CSS_E_OOM;
  }
  if (!css_obj_charge(&s->obj, strings)) {
    css_obj_release_external(&s->obj);
    return GTEXT_CSS_E_LIMIT_CSSOM;
  }
  char * copy = NULL;
  if (length) {
    copy = css_core_dup(core, text, length);
    if (!copy) {
      css_obj_release_external(&s->obj);
      return GTEXT_CSS_E_OOM;
    }
  }
  st = css_sheet_parse_into(core, s, copy, length);
  if (st != GTEXT_CSS_OK) {
    css_obj_release_external(&s->obj);
    return st;
  }
  *out = s;
  return GTEXT_CSS_OK;
}

GTEXT_CSSOM_Sheet * gtext_cssom_sheet_retain(GTEXT_CSSOM_Sheet * sheet) {
  if (sheet) {
    css_obj_retain_external(&sheet->obj);
  }
  return sheet;
}

void gtext_cssom_sheet_release(GTEXT_CSSOM_Sheet * sheet) {
  if (sheet) {
    css_obj_release_external(&sheet->obj);
  }
}

uint64_t gtext_cssom_sheet_id(const GTEXT_CSSOM_Sheet * sheet) {
  return sheet ? sheet->obj.id : 0;
}

uint64_t gtext_cssom_sheet_version(const GTEXT_CSSOM_Sheet * sheet) {
  return sheet ? sheet->version : 0;
}

GTEXT_CSSOM_Document * gtext_cssom_sheet_document(const GTEXT_CSSOM_Sheet * sheet) {
  return sheet ? sheet->obj.core->document : NULL;
}

GTEXT_CSS_Status gtext_cssom_sheet_rules(const GTEXT_CSSOM_Sheet * sheet, const GTEXT_CSSOM_Rule_List ** out) {
  if (!sheet || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  /* CSSOM, `cssRules`: "If the origin-clean flag is unset, throw a SecurityError exception." */
  if (!sheet->origin_clean) {
    return GTEXT_CSS_E_SECURITY;
  }
  *out = &sheet->rules;
  return GTEXT_CSS_OK;
}

const char * gtext_cssom_sheet_type(const GTEXT_CSSOM_Sheet * sheet) {
  (void)sheet;
  return "text/css";
}

static GTEXT_CSS_Status string_out(const GTEXT_CSSOM_Sheet * sheet, const char * s, char ** out, size_t * length) {
  if (!sheet || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  if (!s) {
    *out = NULL;
    if (length) {
      *length = 0;
    }
    return GTEXT_CSS_OK;
  }
  char * copy = css_core_dup(sheet->obj.core, s, strlen(s));
  if (!copy) {
    return GTEXT_CSS_E_OOM;
  }
  *out = copy;
  if (length) {
    *length = strlen(s);
  }
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_sheet_href(const GTEXT_CSSOM_Sheet * sheet, char ** out, size_t * length) {
  return string_out(sheet, sheet ? sheet->href : NULL, out, length);
}

GTEXT_CSS_Status gtext_cssom_sheet_title(const GTEXT_CSSOM_Sheet * sheet, char ** out, size_t * length) {
  return string_out(sheet, sheet ? sheet->title : NULL, out, length);
}

GTEXT_CSS_Status gtext_cssom_sheet_base_url(const GTEXT_CSSOM_Sheet * sheet, char ** out, size_t * length) {
  return string_out(sheet, sheet ? sheet->base_url : NULL, out, length);
}

const void * gtext_cssom_sheet_owner_node(const GTEXT_CSSOM_Sheet * sheet) {
  return sheet ? sheet->owner_node : NULL;
}

bool gtext_cssom_sheet_disabled(const GTEXT_CSSOM_Sheet * sheet) {
  return sheet && sheet->disabled;
}

GTEXT_CSS_Status gtext_cssom_sheet_set_disabled(GTEXT_CSSOM_Sheet * sheet, bool disabled, GTEXT_CSSOM_Outcome * outcome) {
  if (!sheet) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = sheet->obj.core;
  css_outcome_reset(outcome, core);
  GTEXT_CSS_Status st = css_cssom_enter(core);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (sheet->disabled == disabled) {
    return GTEXT_CSS_OK;
  }
  sheet->disabled = disabled;
  sheet->version++;
  css_cssom_event ev;
  memset(&ev, 0, sizeof ev);
  ev.kind = GTEXT_CSSOM_EVENT_SHEET_DISABLED_CHANGED;
  ev.sheet = sheet;
  ev.sheet_id = sheet->obj.id;
  if (outcome) {
    outcome->kind = GTEXT_CSSOM_CHANGED;
  }
  css_cssom_commit(core, &ev, outcome);
  return GTEXT_CSS_OK;
}

bool gtext_cssom_sheet_alternate(const GTEXT_CSSOM_Sheet * sheet) {
  return sheet && sheet->alternate;
}

bool gtext_cssom_sheet_origin_clean(const GTEXT_CSSOM_Sheet * sheet) {
  return sheet && sheet->origin_clean;
}

void gtext_cssom_sheet_set_origin_clean(GTEXT_CSSOM_Sheet * sheet, bool origin_clean) {
  if (sheet) {
    sheet->origin_clean = origin_clean;
  }
}

bool gtext_cssom_sheet_constructed(const GTEXT_CSSOM_Sheet * sheet) {
  return sheet && sheet->constructed;
}

size_t gtext_cssom_sheet_diagnostic_total(const GTEXT_CSSOM_Sheet * sheet) {
  return sheet ? sheet->diag_total : 0;
}

bool gtext_cssom_sheet_diagnostic(const GTEXT_CSSOM_Sheet * sheet, size_t index, GTEXT_CSSOM_Parse_Diagnostic * out) {
  if (!sheet || !out || index >= sheet->ndiags) {
    return false;
  }
  out->kind = sheet->diags[index].kind;
  out->span = sheet->diags[index].span;
  out->reference = sheet->diags[index].reference;
  return true;
}

size_t gtext_cssom_sheet_rule_total(const GTEXT_CSSOM_Sheet * sheet) {
  return sheet ? sheet->rule_total : 0;
}

GTEXT_CSS_Status gtext_cssom_sheet_source_text(const GTEXT_CSSOM_Sheet * sheet, char ** out, size_t * length) {
  if (!sheet || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  if (!sheet->source) {
    return GTEXT_CSS_E_INVALID_STATE;
  }
  char * copy = css_core_dup(sheet->obj.core, sheet->source->text ? sheet->source->text : "", sheet->source->text_length);
  if (!copy) {
    return GTEXT_CSS_E_OOM;
  }
  *out = copy;
  if (length) {
    *length = sheet->source->text_length;
  }
  return GTEXT_CSS_OK;
}

/* ---- compaction ------------------------------------------------------------------------------------------------------------------------------- */

/// One rule of the sheet and where its text is in the new tree.
typedef struct {
  css_rule * rule;
  const GTEXT_CSS_Rule * node;
  const GTEXT_CSS_Declaration * run;
  size_t run_count;
} move;

/// Visit the rules of the sheet, with an explicit stack, copying each rule's own syntax into `tree` (without the rules inside it: each of those has a node of its own, and a rule
/// that was inserted or deleted after the parse has not the children its node has) and recording the move.
static GTEXT_CSS_Status plan_moves(css_sheet * sheet, GTEXT_CSS_Tree * tree, move ** plan_out, size_t * count_out) {
  css_core * core = sheet->obj.core;
  GTEXT_Allocator a = core->alloc;
  const size_t total = sheet->rule_total;
  move * plan = gtext_allocator_calloc(&a, total ? total : 1, sizeof *plan);
  css_rule ** stack = NULL;
  size_t cap = 0, sp = 0, n = 0;
  GTEXT_CSS_Status st = plan ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
  css_rule * top = css_rule_list_first(&sheet->rules);
  while (st == GTEXT_CSS_OK && (top || sp)) {
    css_rule * r = top ? top : stack[--sp];
    top = top ? css_rule_list_next(top) : NULL;
    if (n >= total) {
      st = GTEXT_CSS_E_INTERNAL;
      break;
    }
    move * m = &plan[n++];
    m->rule = r;
    if (r->node) {
      const unsigned flags = css_kind((GTEXT_CSSOM_Rule_Kind)r->kind)->flags;
      const bool lists = (flags & CSS_KIND_LIST_ANY) != 0;
      st = css_tree_copy_rule_own(tree, r->node, lists, (flags & CSS_KIND_LIST_RULES) != 0, &m->node);
    }
    else {
      st = css_tree_copy_run(tree, r->run, r->run_count, &m->run);
      m->run_count = r->run_count;
    }
    if (st != GTEXT_CSS_OK) {
      break;
    }
    /* the rules inside this one, to be visited */
    for (css_rule * c = css_rule_list_first(&r->rules); c; c = css_rule_list_next(c)) {
      if (sp == cap) {
        css_rule ** grown = gtext_allocator_realloc(&a, stack, (cap ? cap * 2 : 16) * sizeof *grown);
        if (!grown) {
          st = GTEXT_CSS_E_OOM;
          break;
        }
        stack = grown;
        cap = cap ? cap * 2 : 16;
      }
      stack[sp++] = c;
    }
  }
  gtext_allocator_free(&a, stack);
  if (st != GTEXT_CSS_OK) {
    gtext_allocator_free(&a, plan);
    return st;
  }
  *plan_out = plan;
  *count_out = n;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_sheet_compact(css_sheet * sheet) {
  css_core * core = sheet->obj.core;
  GTEXT_CSS_Status st = css_cssom_enter(core);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_Allocator a = core->alloc;
  GTEXT_CSS_Tree * tree = NULL;
  st = css_tree_new_empty(&core->alloc, &tree);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  move * plan = NULL;
  size_t n = 0;
  st = plan_moves(sheet, tree, &plan, &n);
  css_source * fresh = NULL;
  if (st == GTEXT_CSS_OK) {
    st = css_source_new(core, tree, NULL, 0, &fresh);
  }
  if (st == GTEXT_CSS_OK) {
    css_source_set_ns(fresh, sheet->ns);   /* the rules' selectors are read in the sheet's namespaces, whatever parse they came from */
  }
  gtext_css_tree_free(tree);   /* the source holds the tree now (or the compaction failed and nothing does) */
  if (st != GTEXT_CSS_OK) {
    gtext_allocator_free(&a, plan);
    return st;
  }
  /* nothing below can fail */
  for (size_t i = 0; i < n; i++) {
    css_rule * r = plan[i].rule;
    css_source * old = r->source;
    r->source = css_source_retain(fresh);
    css_source_release(old);
    r->node = plan[i].node;
    r->run = plan[i].run;
    r->run_count = plan[i].run_count;
    r->source_kind = GTEXT_CSSOM_SOURCE_MODIFIED;
  }
  css_source * old_sheet_source = sheet->source;
  sheet->source = NULL;
  css_source_release(old_sheet_source);
  css_source_release(fresh);
  gtext_allocator_free(&a, plan);
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_sheet_compact(GTEXT_CSSOM_Sheet * sheet) {
  if (!sheet) {
    return GTEXT_CSS_E_INVALID;
  }
  return css_sheet_compact(sheet);
}
