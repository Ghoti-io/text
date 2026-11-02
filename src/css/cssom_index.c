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

#include "css_internal.h"
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
  size_t nscopes = 0;
  css_scope ** scopes = NULL;
  css_sheet ** take = NULL;
  if (st == GTEXT_CSS_OK) {
    scopes = css_scopes_by_context(core, &nscopes);
    if (!scopes) {
      st = GTEXT_CSS_E_OOM;
    }
  }
  for (size_t s = 0; st == GTEXT_CSS_OK && s < nscopes; s++) {
    css_scope * scope = scopes[s];
    for (size_t i = 0; st == GTEXT_CSS_OK && i < scope->nsheets; i++) {
      if (css_scope_owns(scope, scope->sheets[i])) {
        st = add_live(core, index, scope->sheets[i], scope, scope->sheets[i]->implicit_root);
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
      st = add_live(core, index, take[i], scope, scope->implicit_root);
    }
    gtext_allocator_free(&core->alloc, take);
    take = NULL;
  }
  gtext_allocator_free(&core->alloc, take);
  gtext_allocator_free(&core->alloc, scopes);
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
