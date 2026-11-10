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
 * The document's frozen rule index (css_cssom_document.h): the sheets that take part, the tuple the cached index is keyed on, and the registered user-agent and user sheets.
 *
 * **The key.** The index is a function of: the greatest change stamp among every sheet a scope lists (with the sheets they import), the document's structure version, its registration
 * version and the conditions epoch. Every change of a sheet gives it a stamp greater than any given before it, so the greatest stamp of a set changes whenever a member does; a change of
 * the set itself (a sheet listed, taken out, adopted, a scope made, removed or moved) advances the structure version. The key is computed over every listed sheet, disabled ones and ones
 * whose media is false for the conditions included, because it must change when such a sheet is switched on, and a sheet that leaves the set must not leave the key looking the same.
 *
 * **The build** is all or nothing: the index under construction is freed on any failure and the previous one is kept.
 */

#include <string.h>

#include "css_creg.h"
#include "css_fndef.h"
#include "css_internal.h"
#include "cssom_computed_internal.h"
#include "cssom_doc_internal.h"

void css_index_teardown(css_core * core) {
  gtext_css_rule_index_free(core->index);
  core->index = NULL;
  core->index_valid = false;
  for (size_t i = 0; i < core->nregs; i++) {
    gtext_css_sheet_free(core->regs[i].sheet);
  }
  core->nregs = 0;
}

/* ---- the user-agent and user sheets ------------------------------------------------------------------------------------------------------------- */

GTEXT_CSS_Status gtext_cssom_document_register_sheet(GTEXT_CSSOM_Document * document, GTEXT_CSS_Origin origin, GTEXT_CSS_Sheet * sheet, GTEXT_CSSOM_Outcome * outcome) {
  if (!document || !sheet || (origin != GTEXT_CSS_ORIGIN_USER_AGENT && origin != GTEXT_CSS_ORIGIN_USER)) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = css_document_core(document);
  css_outcome_reset(outcome, core);
  GTEXT_CSS_Status st = css_cssom_enter(core);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  for (size_t i = 0; i < core->nregs; i++) {
    if (core->regs[i].sheet == sheet) {
      return GTEXT_CSS_E_INVALID_STATE;
    }
  }
  if (core->nregs == core->capregs) {
    const size_t cap = core->capregs ? core->capregs * 2 : 4;
    css_registered_sheet * grown = gtext_allocator_realloc(&core->alloc, core->regs, cap * sizeof *grown);
    if (!grown) {
      return GTEXT_CSS_E_OOM;
    }
    core->regs = grown;
    core->capregs = cap;
  }
  core->regs[core->nregs].sheet = gtext_css_sheet_retain(sheet);
  core->regs[core->nregs].origin = origin;
  core->nregs++;
  core->registration_version++;
  css_cssom_event ev;
  memset(&ev, 0, sizeof ev);
  ev.kind = GTEXT_CSSOM_EVENT_SHEET_ADDED;
  if (outcome) {
    outcome->kind = GTEXT_CSSOM_CHANGED;
  }
  css_cssom_commit(core, &ev, outcome);
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_document_unregister_sheet(GTEXT_CSSOM_Document * document, GTEXT_CSS_Sheet * sheet, GTEXT_CSSOM_Outcome * outcome) {
  if (!document || !sheet) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = css_document_core(document);
  css_outcome_reset(outcome, core);
  GTEXT_CSS_Status st = css_cssom_enter(core);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  size_t at = 0;
  while (at < core->nregs && core->regs[at].sheet != sheet) {
    at++;
  }
  if (at == core->nregs) {
    return GTEXT_CSS_E_NOT_FOUND;
  }
  GTEXT_CSS_Sheet * held = core->regs[at].sheet;
  memmove(&core->regs[at], &core->regs[at + 1], (core->nregs - at - 1) * sizeof *core->regs);
  core->nregs--;
  core->registration_version++;
  css_cssom_event ev;
  memset(&ev, 0, sizeof ev);
  ev.kind = GTEXT_CSSOM_EVENT_SHEET_REMOVED;
  if (outcome) {
    outcome->kind = GTEXT_CSSOM_CHANGED;
  }
  css_cssom_commit(core, &ev, outcome);
  gtext_css_sheet_free(held);   /* the caller's pointer stays valid only if the caller holds its own reference */
  return GTEXT_CSS_OK;
}

/* ---- the key ---------------------------------------------------------------------------------------------------------------------------------- */

static GTEXT_CSS_Status compute_stamp(css_core * core, uint64_t * stamp) {
  *stamp = 0;
  uint64_t looked = 0;
  for (size_t i = 0; i < core->nscopes; i++) {
    const GTEXT_CSS_Status st = css_scope_stamp(core->scopes[i], stamp, &looked);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
  }
  core->counters.index_key_sheets += looked;
  return GTEXT_CSS_OK;
}

/* ---- the build -------------------------------------------------------------------------------------------------------------------------------- */

/// Whether the sheet's own state lets it take part: not disabled, and its media query list true for the conditions.
static GTEXT_CSS_Status takes_part(css_core * core, const css_sheet * sheet, bool * yes) {
  *yes = false;
  if (sheet->disabled) {
    return GTEXT_CSS_OK;
  }
  if (!sheet->media_text) {
    *yes = true;
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Media_Options mo = gtext_css_media_options_default();
  mo.allocator = &core->alloc;
  mo.parse.max_input_bytes = core->config.max_input_bytes;
  GTEXT_CSS_Media_List * list = NULL;
  GTEXT_CSS_Status st = gtext_css_media_list_parse(sheet->media_text, strlen(sheet->media_text), &mo, &list, NULL);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  st = gtext_css_conditions_matches_list(core->conditions, list, NULL, yes);
  gtext_css_media_list_free(list);
  return st;
}

/// Add one live sheet as its snapshot.
static GTEXT_CSS_Status add_live(css_core * core, GTEXT_CSS_Rule_Index * index, css_sheet * sheet, const css_scope * scope, const void * implicit_root) {
  bool yes = false;
  GTEXT_CSS_Status st = takes_part(core, sheet, &yes);
  if (st != GTEXT_CSS_OK || !yes) {
    return st;
  }
  GTEXT_CSS_Sheet * snap = NULL;
  st = gtext_cssom_sheet_snapshot(sheet, &snap);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSS_Sheet_Registration reg;
  memset(&reg, 0, sizeof reg);
  reg.origin = GTEXT_CSS_ORIGIN_AUTHOR;
  reg.context = scope->context;
  reg.tree_scope = scope->handle;
  reg.shadow = scope->shadow;
  reg.parent_scope = scope->parent ? scope->parent->handle : NULL;
  reg.implicit_root = implicit_root;
  GTEXT_CSS_Rule_Index_Result res;
  st = gtext_css_rule_index_add_sheet(index, snap, &reg, &res);
  gtext_css_sheet_free(snap);   /* the index retained it */
  return st;
}

/// What is done with each live sheet that is listed, in the document's cascade order.
typedef GTEXT_CSS_Status (*live_fn)(css_core * core, void * user, css_sheet * sheet, const css_scope * scope, const void * implicit_root);

/// Every live sheet that takes part in the document, in the order the cascade reads them: the scopes by context, the sheets each scope owns in its list, then its adopted sheets (a sheet
/// listed more than once takes part once, at its last position). Disabled sheets and ones whose media is false are the callback's to refuse (takes_part()).
static GTEXT_CSS_Status walk_live(css_core * core, live_fn fn, void * user) {
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  size_t nscopes = 0;
  css_scope ** scopes = css_scopes_by_context(core, &nscopes);
  css_sheet ** take = NULL;
  if (!scopes) {
    return GTEXT_CSS_E_OOM;
  }
  for (size_t s = 0; st == GTEXT_CSS_OK && s < nscopes; s++) {
    css_scope * scope = scopes[s];
    for (size_t i = 0; st == GTEXT_CSS_OK && i < scope->nsheets; i++) {
      if (css_scope_owns(scope, scope->sheets[i])) {
        st = fn(core, user, scope->sheets[i], scope, scope->sheets[i]->implicit_root);
      }
    }
    if (st != GTEXT_CSS_OK || scope->nadopted == 0) {
      continue;
    }
    /* a sheet listed more than once takes part once, at its last position: walk from the end and keep the first sight of each */
    take = gtext_allocator_malloc(&core->alloc, scope->nadopted * sizeof *take);
    if (!take) {
      st = GTEXT_CSS_E_OOM;
      break;
    }
    const uint64_t mark = ++core->stamp_counter;   /* a number no sheet has been marked with */
    size_t n = 0;
    for (size_t i = scope->nadopted; i-- > 0;) {
      css_sheet * sh = scope->adopted[i];
      if (sh->adopt_mark != mark && css_scope_owns(scope, sh)) {
        sh->adopt_mark = mark;
        take[n++] = sh;
      }
    }
    for (size_t i = n; i-- > 0 && st == GTEXT_CSS_OK;) {
      st = fn(core, user, take[i], scope, scope->implicit_root);
    }
    gtext_allocator_free(&core->alloc, take);
    take = NULL;
  }
  gtext_allocator_free(&core->alloc, take);
  gtext_allocator_free(&core->alloc, scopes);
  return st;
}

static GTEXT_CSS_Status add_live_fn(css_core * core, void * user, css_sheet * sheet, const css_scope * scope, const void * implicit_root) {
  return add_live(core, (GTEXT_CSS_Rule_Index *)user, sheet, scope, implicit_root);
}

static GTEXT_CSS_Status build(css_core * core, GTEXT_CSS_Rule_Index ** out) {
  GTEXT_CSS_Rule_Index_Options io = gtext_css_rule_index_options_default();
  io.allocator = &core->alloc;
  io.html_document = core->has_match && core->match.html_document;
  io.quirks = core->has_match ? core->match.quirks : core->config.quirks;
  io.conditions = core->conditions;
  GTEXT_CSS_Rule_Index * index = NULL;
  GTEXT_CSS_Status st = gtext_css_rule_index_new(core->registry, &io, &index);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSS_Rule_Index_Result res;
  for (size_t pass = 0; pass < 2 && st == GTEXT_CSS_OK; pass++) {
    for (size_t i = 0; i < core->nregs && st == GTEXT_CSS_OK; i++) {
      if ((core->regs[i].origin == GTEXT_CSS_ORIGIN_USER_AGENT) != (pass == 0)) {
        continue;
      }
      GTEXT_CSS_Sheet_Registration reg;
      memset(&reg, 0, sizeof reg);
      reg.origin = core->regs[i].origin;
      st = gtext_css_rule_index_add_sheet(index, core->regs[i].sheet, &reg, &res);
    }
  }
  if (st == GTEXT_CSS_OK) {
    st = walk_live(core, add_live_fn, index);
  }
  if (st == GTEXT_CSS_OK) {
    st = gtext_css_rule_index_freeze(index, &res);
  }
  if (st != GTEXT_CSS_OK) {
    gtext_css_rule_index_free(index);
    return st;
  }
  *out = index;
  return GTEXT_CSS_OK;
}

/* ---- the registrations of the @property rules ------------------------------------------------------------------------------------------------- */

static GTEXT_CSS_Status collect_live_fn(css_core * core, void * user, css_sheet * sheet, const css_scope * scope, const void * implicit_root) {
  (void)scope;
  (void)implicit_root;
  bool yes = false;
  const GTEXT_CSS_Status st = takes_part(core, sheet, &yes);
  if (st != GTEXT_CSS_OK || !yes) {
    return st;
  }
  return css_sheet_collect_properties(core, sheet, (GTEXT_CSS_At_Property_Set **)user);
}

/// The registrations the rules of the sheets that take part make, in cascade order ("if multiple valid @property rules are defined for the same name, the last one wins"): the sheets
/// of the user-agent and user origins are compiled sheets without their rules, so they register nothing. NULL when no sheet has a rule that can hold one.
static GTEXT_CSS_Status build_properties(css_core * core, GTEXT_CSS_At_Property_Set ** out) {
  GTEXT_CSS_At_Property_Set * set = NULL;
  const GTEXT_CSS_Status st = walk_live(core, collect_live_fn, &set);
  if (st != GTEXT_CSS_OK) {
    gtext_css_at_property_set_free(set);
    return st;
  }
  *out = set;
  return GTEXT_CSS_OK;
}

/* ---- the functions of the @function rules ----------------------------------------------------------------------------------------------------------- */

static GTEXT_CSS_Status collect_functions_live_fn(css_core * core, void * user, css_sheet * sheet, const css_scope * scope, const void * implicit_root) {
  (void)scope;
  (void)implicit_root;
  bool yes = false;
  const GTEXT_CSS_Status st = takes_part(core, sheet, &yes);
  if (st != GTEXT_CSS_OK || !yes) {
    return st;
  }
  return css_sheet_collect_functions(core, sheet, (GTEXT_CSS_Function_Set **)user);
}

/// The functions the `@function` rules of the sheets that take part define, in cascade order ("rules defined later win within the same layer"). NULL when no sheet defines one.
static GTEXT_CSS_Status build_functions(css_core * core, GTEXT_CSS_Function_Set ** out) {
  GTEXT_CSS_Function_Set * set = NULL;
  const GTEXT_CSS_Status st = walk_live(core, collect_functions_live_fn, &set);
  if (st != GTEXT_CSS_OK) {
    gtext_css_function_set_free(set);
    return st;
  }
  *out = set;
  return GTEXT_CSS_OK;
}

/* ---- the @keyframes rules ------------------------------------------------------------------------------------------------------------------------- */

static GTEXT_CSS_Status keyframes_live_fn(css_core * core, void * user, css_sheet * sheet, const css_scope * scope, const void * implicit_root) {
  (void)implicit_root;
  if (scope->context != 0) {
    return GTEXT_CSS_OK;   /* the names of a shadow tree are that tree's: the document's elements are styled by the document's */
  }
  bool yes = false;
  const GTEXT_CSS_Status st = takes_part(core, sheet, &yes);
  if (st != GTEXT_CSS_OK || !yes) {
    return st;
  }
  return css_sheet_collect_keyframes(core, sheet, (GTEXT_CSS_Atrule_Set **)user);
}

/// The `@keyframes` rules of the sheets that take part, in cascade order ("If multiple @keyframes rules are defined with the same name, the last one in the highest layer wins"), frozen.
/// NULL when no sheet has one.
static GTEXT_CSS_Status build_keyframes(css_core * core, GTEXT_CSS_Atrule_Set ** out) {
  GTEXT_CSS_Atrule_Set * set = NULL;
  GTEXT_CSS_Status st = walk_live(core, keyframes_live_fn, &set);
  if (st == GTEXT_CSS_OK && set) {
    st = gtext_css_atrule_set_freeze(set);
  }
  if (st != GTEXT_CSS_OK) {
    gtext_css_atrule_set_free(set);
    return st;
  }
  *out = set;
  return GTEXT_CSS_OK;
}

static bool same_text(const char * a, size_t an, const char * b, size_t bn) {
  return an == bn && (an == 0 || memcmp(a, b, an) == 0);
}

/// Whether two sets make the same registrations and report the same diagnostics (a NULL set has neither).
static bool same_properties(const GTEXT_CSS_At_Property_Set * x, const GTEXT_CSS_At_Property_Set * y) {
  if (x == y) {
    return true;
  }
  if (gtext_css_at_property_count(x) != gtext_css_at_property_count(y) || gtext_css_at_property_diagnostic_total(x) != gtext_css_at_property_diagnostic_total(y) ||
      !css_at_property_private_same(x, y)) {
    return false;
  }
  for (size_t i = 0; i < gtext_css_at_property_count(x); i++) {
    GTEXT_CSS_Custom_Info a, b;
    if (!gtext_css_at_property_at(x, i, &a) || !gtext_css_at_property_at(y, i, &b)) {
      return false;
    }
    if (!same_text(a.name, a.name_length, b.name, b.name_length) || !same_text(a.syntax, a.syntax_length, b.syntax, b.syntax_length) || a.inherits != b.inherits ||
        a.has_initial != b.has_initial || !same_text(a.initial_value, a.initial_length, b.initial_value, b.initial_length)) {
      return false;
    }
  }
  for (size_t i = 0;; i++) {
    GTEXT_CSS_At_Property_Diagnostic a, b;
    const bool ha = gtext_css_at_property_diagnostic(x, i, &a);
    const bool hb = gtext_css_at_property_diagnostic(y, i, &b);
    if (ha != hb) {
      return false;
    }
    if (!ha) {
      return true;
    }
    if (a.reason != b.reason || a.sheet != b.sheet || a.span.offset != b.span.offset || a.span.end != b.span.end || !same_text(a.name.data, a.name.length, b.name.data, b.name.length)) {
      return false;
    }
  }
}

static GTEXT_CSS_Status current(css_core * core, const GTEXT_CSS_Rule_Index ** out) {
  uint64_t stamp = 0;
  GTEXT_CSS_Status st = compute_stamp(core, &stamp);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (core->index_valid && core->index_stamp == stamp && core->index_structure == core->structure_version && core->index_registration == core->registration_version &&
      core->index_epoch == core->conditions_epoch) {
    core->counters.index_hits++;
    *out = core->index;
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Rule_Index * fresh = NULL;
  st = build(core, &fresh);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  /* the registrations are of the same sheets as the index and change with it: made before anything is replaced, so that a failure leaves both as they were */
  GTEXT_CSS_At_Property_Set * props = NULL;
  st = build_properties(core, &props);
  if (st != GTEXT_CSS_OK) {
    gtext_css_rule_index_free(fresh);
    return st;
  }
  /* ... and the custom functions of the `@function` rules, which a compute context is made with as it is with the registrations */
  GTEXT_CSS_Function_Set * fns = NULL;
  st = build_functions(core, &fns);
  if (st != GTEXT_CSS_OK) {
    gtext_css_at_property_set_free(props);
    gtext_css_rule_index_free(fresh);
    return st;
  }
  /* ... and so are the `@keyframes` rules, which the style of an element samples its animations from */
  GTEXT_CSS_Atrule_Set * frames = NULL;
  st = build_keyframes(core, &frames);
  if (st != GTEXT_CSS_OK) {
    gtext_css_at_property_set_free(props);
    gtext_css_function_set_free(fns);
    gtext_css_rule_index_free(fresh);
    return st;
  }
  core->counters.property_collects += props != NULL;
  const bool same_props = same_properties(core->props, props);
  const bool same_fns = css_function_sets_equal(core->fns, fns);
  if (same_props) {
    gtext_css_at_property_set_free(props);   /* nothing a compute context was made with has changed */
    props = core->props;
  }
  if (same_fns) {
    gtext_css_function_set_free(fns);
    fns = core->fns;
  }
  if (!same_props || !same_fns) {
    css_computed_properties_changed(core, props, fns);   /* a set that is the document's own already stays, and is not freed with the old ones */
  }
  gtext_css_atrule_set_free(core->keyframes);
  core->keyframes = frames;
  GTEXT_CSS_Rule_Index * old = core->index;
  core->index = fresh;
  core->index_valid = true;
  core->index_stamp = stamp;
  core->index_structure = core->structure_version;
  core->index_registration = core->registration_version;
  core->index_epoch = core->conditions_epoch;
  core->counters.index_builds++;
  core->counters.index_sheets_added += gtext_css_rule_index_sheet_count(fresh);
  core->counters.index_records_rebuilt += gtext_css_rule_index_record_count(fresh);
  gtext_css_rule_index_free(old);
  *out = fresh;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_index_current(css_core * core, const GTEXT_CSS_Rule_Index ** out) {
  return current(core, out);
}

GTEXT_CSS_Status gtext_cssom_document_index(GTEXT_CSSOM_Document * document, const GTEXT_CSS_Rule_Index ** out) {
  if (!document || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  return current(css_document_core(document), out);
}

GTEXT_CSS_Status gtext_cssom_document_index_retained(GTEXT_CSSOM_Document * document, GTEXT_CSS_Rule_Index ** out) {
  if (!document || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  const GTEXT_CSS_Rule_Index * index = NULL;
  const GTEXT_CSS_Status st = current(css_document_core(document), &index);
  if (st == GTEXT_CSS_OK) {
    *out = gtext_css_rule_index_retain((GTEXT_CSS_Rule_Index *)(uintptr_t)index);
  }
  return st;
}
