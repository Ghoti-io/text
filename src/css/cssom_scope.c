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
 * Tree scopes (css_cssom_document.h): the object, the registration with the document, `styleSheets` and `adoptedStyleSheets`.
 *
 * Every change of a scope's membership or of one of its two lists advances the document's structure version, which is part of the tuple the cached index is keyed on; the sheets
 * themselves are not changed by being listed. An operation that can fail does its allocation first, so a failure leaves the lists, the counts and the versions as they were.
 */

#include <stdlib.h>
#include <string.h>

#include "css_internal.h"
#include "cssom_doc_internal.h"

css_scope * css_scope_from_obj(css_obj * o) {
  return (css_scope *)o;
}

/// The core whose document the scope answers to: the one it is registered with, else the one that made it.
static css_core * scope_core(const css_scope * s) {
  return s->home ? s->home : s->obj.core;
}

static void scope_destroy(css_obj * o) {
  css_scope * s = (css_scope *)o;
  GTEXT_Allocator a = o->core->alloc;
  for (size_t i = 0; i < s->nsheets; i++) {
    s->sheets[i]->scope = NULL;
    css_obj_release_internal(&s->sheets[i]->obj);
  }
  for (size_t i = 0; i < s->nadopted; i++) {
    css_obj_release_internal(&s->adopted[i]->obj);
  }
  if (s->parent) {
    css_obj_release_internal(&s->parent->obj);
  }
  gtext_allocator_free(&a, s->sheets);
  gtext_allocator_free(&a, s->adopted);
  gtext_allocator_free(&a, s);
}

/// A scope object with the core's internal count, and `external` references for the caller.
static GTEXT_CSS_Status scope_make(css_core * core, uint64_t external, bool counted, css_scope ** out) {
  if (counted) {
    const GTEXT_CSS_Status st = css_core_room(core, sizeof(css_scope));
    if (st != GTEXT_CSS_OK) {
      return st;
    }
  }
  GTEXT_Allocator a = core->alloc;
  css_scope * s = gtext_allocator_calloc(&a, 1, sizeof *s);
  if (!s) {
    return GTEXT_CSS_E_OOM;
  }
  css_obj_init(&s->obj, core, GTEXT_CSSOM_OBJECT_SCOPE, 1, external, sizeof *s, scope_destroy);
  *out = s;
  return GTEXT_CSS_OK;
}

/// Make room for one more registered scope in the core's array.
static bool scopes_reserve(css_core * core) {
  if (core->nscopes < core->capscopes) {
    return true;
  }
  const size_t cap = core->capscopes ? core->capscopes * 2 : 8;
  css_scope ** grown = gtext_allocator_realloc(&core->alloc, core->scopes, cap * sizeof *grown);
  if (!grown) {
    return false;
  }
  core->scopes = grown;
  core->capscopes = cap;
  return true;
}

GTEXT_CSS_Status css_scope_create_document(css_core * core, css_scope ** out) {
  if (!scopes_reserve(core)) {
    return GTEXT_CSS_E_OOM;
  }
  css_scope * s = NULL;
  const GTEXT_CSS_Status st = scope_make(core, 0, false, &s);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  /* the document's own scope is not an object of the counters and takes no id: a document with nothing in it has no objects, and the ids of everything else are as they were */
  core->counters.live_objects--;
  core->counters.live_bytes -= s->obj.bytes;
  s->obj.bytes = 0;
  s->obj.uncounted = true;
  s->obj.id = 0;
  core->next_id--;
  s->home = core;
  s->is_document = true;
  s->context = 0;
  core->scopes[core->nscopes++] = s;
  core->doc_scope = s;
  *out = s;
  return GTEXT_CSS_OK;
}

void css_scope_teardown_all(css_core * core) {
  css_scope ** held = core->scopes;
  const size_t n = core->nscopes;
  core->scopes = NULL;
  core->capscopes = 0;
  core->nscopes = 0;
  core->doc_scope = NULL;
  for (size_t i = 0; i < n; i++) {
    held[i]->home = NULL;
  }
  for (size_t i = 0; i < n; i++) {
    css_obj_release_internal(&held[i]->obj);
  }
  GTEXT_Allocator a = core->alloc;
  gtext_allocator_free(&a, held);
}

GTEXT_CSSOM_Scope_Options gtext_cssom_scope_options_default(void) {
  GTEXT_CSSOM_Scope_Options o;
  memset(&o, 0, sizeof o);
  o.size = sizeof o;
  return o;
}

/* ---- the events ------------------------------------------------------------------------------------------------------------------------------- */

/// One event about a scope's lists, delivered with the sheet held.
static void emit(css_core * core, GTEXT_CSSOM_Event_Kind kind, css_scope * scope, css_sheet * sheet, GTEXT_CSSOM_Outcome * outcome) {
  css_cssom_event ev;
  memset(&ev, 0, sizeof ev);
  ev.kind = kind;
  ev.scope = scope;
  ev.sheet = sheet;
  ev.sheet_id = sheet ? sheet->obj.id : 0;
  if (sheet) {
    css_obj_retain_external(&sheet->obj);
  }
  if (outcome) {
    outcome->kind = GTEXT_CSSOM_CHANGED;
  }
  css_cssom_commit(core, &ev, outcome);
  if (sheet) {
    css_obj_release_external(&sheet->obj);
  }
}

/* ---- registration ------------------------------------------------------------------------------------------------------------------------- */

GTEXT_CSS_Status gtext_cssom_scope_new(GTEXT_CSSOM_Document * document, const GTEXT_CSSOM_Scope_Options * options, GTEXT_CSSOM_Scope ** out) {
  if (!document || !out || (options && options->size < offsetof(GTEXT_CSSOM_Scope_Options, handle) + sizeof options->handle)) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = css_document_core(document);
  GTEXT_CSSOM_Scope_Options o = gtext_cssom_scope_options_default();
  if (options) {
    const size_t n = options->size < sizeof o ? options->size : sizeof o;
    memcpy(&o, options, n);
    o.size = sizeof o;
  }
  if ((o.shadow && !o.handle) || (o.parent && o.parent->home != core)) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Status st = css_cssom_enter(core);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (core->nscopes >= core->config.max_scopes) {
    return GTEXT_CSS_E_LIMIT_CSSOM;
  }
  if (!scopes_reserve(core)) {
    return GTEXT_CSS_E_OOM;
  }
  css_scope * s = NULL;
  st = scope_make(core, 1, true, &s);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  s->home = core;
  s->handle = o.handle;
  s->shadow = o.shadow;
  s->implicit_root = o.implicit_root;
  if (o.parent && !o.parent->is_document) {
    s->parent = o.parent;
    css_obj_retain_internal(&o.parent->obj);
  }
  if (o.context == 0) {
    s->context = core->next_context++;
  }
  else {
    s->context = o.context;
    if (o.context >= core->next_context) {
      core->next_context = o.context + 1;
    }
  }
  core->scopes[core->nscopes++] = s;
  *out = s;
  return GTEXT_CSS_OK;
}

GTEXT_CSSOM_Scope * gtext_cssom_document_scope(GTEXT_CSSOM_Document * document) {
  return document ? css_document_core(document)->doc_scope : NULL;
}

GTEXT_CSSOM_Scope * gtext_cssom_scope_retain(GTEXT_CSSOM_Scope * scope) {
  if (scope) {
    css_obj_retain_external(&scope->obj);
  }
  return scope;
}

void gtext_cssom_scope_release(GTEXT_CSSOM_Scope * scope) {
  if (scope) {
    css_obj_release_external(&scope->obj);
  }
}

uint64_t gtext_cssom_scope_id(const GTEXT_CSSOM_Scope * scope) {
  return scope ? scope->obj.id : 0;
}

uint32_t gtext_cssom_scope_context(const GTEXT_CSSOM_Scope * scope) {
  return scope ? scope->context : 0;
}

const void * gtext_cssom_scope_handle(const GTEXT_CSSOM_Scope * scope) {
  return scope ? scope->handle : NULL;
}

bool gtext_cssom_scope_shadow(const GTEXT_CSSOM_Scope * scope) {
  return scope && scope->shadow;
}

GTEXT_CSSOM_Scope * gtext_cssom_scope_parent(const GTEXT_CSSOM_Scope * scope) {
  return scope ? scope->parent : NULL;
}

GTEXT_CSSOM_Document * gtext_cssom_scope_document(const GTEXT_CSSOM_Scope * scope) {
  return scope && scope->home ? scope->home->document : NULL;
}

/// Take the scope out of the core's array; true when it was there.
static bool unlist(css_core * core, css_scope * s) {
  for (size_t i = 0; i < core->nscopes; i++) {
    if (core->scopes[i] == s) {
      memmove(&core->scopes[i], &core->scopes[i + 1], (core->nscopes - i - 1) * sizeof *core->scopes);
      core->nscopes--;
      return true;
    }
  }
  return false;
}

GTEXT_CSS_Status gtext_cssom_scope_remove(GTEXT_CSSOM_Scope * scope, GTEXT_CSSOM_Outcome * outcome) {
  if (!scope || scope->is_document) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = scope_core(scope);
  css_outcome_reset(outcome, core);
  GTEXT_CSS_Status st = css_cssom_enter(core);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!scope->home) {
    return GTEXT_CSS_OK;   /* UNCHANGED */
  }
  /* nothing below can fail: the lists leave the scope first, and the events are about sheets held until the end */
  css_obj_retain_external(&scope->obj);
  css_sheet ** sheets = scope->sheets;
  const size_t nsheets = scope->nsheets;
  css_sheet ** adopted = scope->adopted;
  const size_t nadopted = scope->nadopted;
  scope->sheets = NULL;
  scope->nsheets = scope->capsheets = 0;
  scope->adopted = NULL;
  scope->nadopted = scope->capadopted = 0;
  unlist(core, scope);
  scope->home = NULL;
  core->structure_version++;
  for (size_t i = 0; i < nsheets; i++) {
    sheets[i]->scope = NULL;
    sheets[i]->owner_node = NULL;
  }
  for (size_t i = 0; i < nsheets; i++) {
    emit(core, GTEXT_CSSOM_EVENT_SHEET_REMOVED, scope, sheets[i], outcome);
  }
  if (nadopted) {
    emit(core, GTEXT_CSSOM_EVENT_ADOPTED_CHANGED, scope, NULL, outcome);
  }
  for (size_t i = 0; i < nsheets; i++) {
    css_obj_release_internal(&sheets[i]->obj);
  }
  for (size_t i = 0; i < nadopted; i++) {
    css_obj_release_internal(&adopted[i]->obj);
  }
  GTEXT_Allocator a = scope->obj.core->alloc;
  gtext_allocator_free(&a, sheets);
  gtext_allocator_free(&a, adopted);
  css_obj_release_internal(&scope->obj);   /* the core's */
  css_obj_release_external(&scope->obj);
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_scope_reparent(GTEXT_CSSOM_Scope * scope, GTEXT_CSSOM_Document * document, GTEXT_CSSOM_Scope * parent, GTEXT_CSSOM_Outcome * outcome) {
  if (!scope || !document || scope->is_document || !scope->home) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * from = scope->home;
  css_core * to = css_document_core(document);
  css_outcome_reset(outcome, to);
  if (parent && (parent->home != to || parent == scope)) {
    return GTEXT_CSS_E_INVALID;
  }
  if (parent && parent->is_document) {
    parent = NULL;
  }
  for (css_scope * x = parent; x; x = x->parent) {
    if (x == scope) {
      return GTEXT_CSS_E_INVALID;   /* a scope cannot sit under itself */
    }
  }
  GTEXT_CSS_Status st = css_cssom_enter(from);
  if (st == GTEXT_CSS_OK && to != from) {
    st = css_cssom_enter(to);
  }
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (from == to && scope->parent == parent) {
    return GTEXT_CSS_OK;   /* UNCHANGED */
  }
  if (from != to) {
    if (to->nscopes >= to->config.max_scopes) {
      return GTEXT_CSS_E_LIMIT_CSSOM;
    }
    if (!scopes_reserve(to)) {
      return GTEXT_CSS_E_OOM;
    }
  }
  /* nothing below can fail */
  css_obj_retain_external(&scope->obj);
  css_scope * old_parent = scope->parent;
  if (parent) {
    css_obj_retain_internal(&parent->obj);
  }
  scope->parent = parent;
  if (from != to) {
    unlist(from, scope);
    to->scopes[to->nscopes++] = scope;
    scope->home = to;
    if (scope->context >= to->next_context) {
      to->next_context = scope->context + 1;
    }
    from->structure_version++;
  }
  to->structure_version++;
  const bool matters = scope->nsheets || scope->nadopted;
  if (matters) {
    if (from != to) {
      emit(from, GTEXT_CSSOM_EVENT_ADOPTED_CHANGED, scope, NULL, NULL);
    }
    emit(to, GTEXT_CSSOM_EVENT_ADOPTED_CHANGED, scope, NULL, outcome);
  }
  else if (outcome) {
    outcome->kind = GTEXT_CSSOM_CHANGED;
  }
  if (old_parent) {
    css_obj_release_internal(&old_parent->obj);
  }
  css_obj_release_external(&scope->obj);
  return GTEXT_CSS_OK;
}

/* ---- `styleSheets` ---------------------------------------------------------------------------------------------------------------------------- */

size_t gtext_cssom_scope_sheet_count(const GTEXT_CSSOM_Scope * scope) {
  return scope ? scope->nsheets : 0;
}

GTEXT_CSSOM_Sheet * gtext_cssom_scope_sheet_at(const GTEXT_CSSOM_Scope * scope, size_t index) {
  if (!scope || index >= scope->nsheets) {
    return NULL;
  }
  return gtext_cssom_sheet_retain(scope->sheets[index]);
}

css_scope * css_sheet_scope(const css_sheet * sheet) {
  return sheet->scope;
}

/// Make room for `more` entries in a list of sheets.
static bool list_reserve(css_core * core, css_sheet *** list, size_t n, size_t * cap, size_t more) {
  if (n + more <= *cap) {
    return true;
  }
  size_t want = *cap ? *cap : 4;
  while (want < n + more) {
    want *= 2;
  }
  css_sheet ** grown = gtext_allocator_realloc(&core->alloc, *list, want * sizeof *grown);
  if (!grown) {
    return false;
  }
  *list = grown;
  *cap = want;
  return true;
}

GTEXT_CSS_Status gtext_cssom_scope_insert_sheet(GTEXT_CSSOM_Scope * scope, GTEXT_CSSOM_Sheet * sheet, size_t index, GTEXT_CSSOM_Outcome * outcome) {
  if (!scope || !sheet) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = scope_core(scope);
  css_outcome_reset(outcome, core);
  GTEXT_CSS_Status st = css_cssom_enter(core);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (sheet->obj.core != core) {
    return GTEXT_CSS_E_INVALID;   /* a sheet of another document */
  }
  if (sheet->constructed || sheet->owner_rule || sheet->scope) {
    return GTEXT_CSS_E_INVALID_STATE;
  }
  if (index > scope->nsheets) {
    return GTEXT_CSS_E_INDEX_SIZE;
  }
  if (!list_reserve(scope->obj.core, &scope->sheets, scope->nsheets, &scope->capsheets, 1)) {
    return GTEXT_CSS_E_OOM;
  }
  memmove(&scope->sheets[index + 1], &scope->sheets[index], (scope->nsheets - index) * sizeof *scope->sheets);
  scope->sheets[index] = sheet;
  scope->nsheets++;
  css_obj_retain_internal(&sheet->obj);
  sheet->scope = scope;
  core->structure_version++;
  css_obj_retain_external(&scope->obj);
  emit(core, GTEXT_CSSOM_EVENT_SHEET_ADDED, scope, sheet, outcome);
  css_obj_release_external(&scope->obj);
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_scope_remove_sheet(GTEXT_CSSOM_Scope * scope, GTEXT_CSSOM_Sheet * sheet, GTEXT_CSSOM_Outcome * outcome) {
  if (!scope || !sheet) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = scope_core(scope);
  css_outcome_reset(outcome, core);
  GTEXT_CSS_Status st = css_cssom_enter(core);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (sheet->scope != scope) {
    return GTEXT_CSS_E_NOT_FOUND;
  }
  size_t at = 0;
  while (at < scope->nsheets && scope->sheets[at] != sheet) {
    at++;
  }
  if (at == scope->nsheets) {
    return GTEXT_CSS_E_NOT_FOUND;
  }
  /* the sheet stays a valid object: the caller's reference, or the one taken here, outlives the scope's */
  css_obj_retain_external(&sheet->obj);
  css_obj_retain_external(&scope->obj);
  memmove(&scope->sheets[at], &scope->sheets[at + 1], (scope->nsheets - at - 1) * sizeof *scope->sheets);
  scope->nsheets--;
  sheet->scope = NULL;
  sheet->owner_node = NULL;
  core->structure_version++;
  emit(core, GTEXT_CSSOM_EVENT_SHEET_REMOVED, scope, sheet, outcome);
  css_obj_release_internal(&sheet->obj);
  css_obj_release_external(&sheet->obj);
  css_obj_release_external(&scope->obj);
  return GTEXT_CSS_OK;
}

/* ---- `adoptedStyleSheets` -------------------------------------------------------------------------------------------------------------------- */

size_t gtext_cssom_scope_adopted_count(const GTEXT_CSSOM_Scope * scope) {
  return scope ? scope->nadopted : 0;
}

GTEXT_CSSOM_Sheet * gtext_cssom_scope_adopted_at(const GTEXT_CSSOM_Scope * scope, size_t index) {
  if (!scope || index >= scope->nadopted) {
    return NULL;
  }
  return gtext_cssom_sheet_retain(scope->adopted[index]);
}

/// "A constructed sheet of the document the scope belongs to": CSSOM's check for `adoptedStyleSheets` (Constructable Stylesheets, "adoptedStyleSheets": a NotAllowedError otherwise).
static bool adoptable(const css_scope * scope, const css_sheet * sheet) {
  return sheet && sheet->constructed && sheet->obj.core == scope_core(scope);
}

GTEXT_CSS_Status gtext_cssom_scope_adopted_insert(GTEXT_CSSOM_Scope * scope, GTEXT_CSSOM_Sheet * sheet, size_t index, GTEXT_CSSOM_Outcome * outcome) {
  if (!scope || !sheet) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = scope_core(scope);
  css_outcome_reset(outcome, core);
  GTEXT_CSS_Status st = css_cssom_enter(core);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!adoptable(scope, sheet)) {
    return GTEXT_CSS_E_NOT_ALLOWED;
  }
  if (index > scope->nadopted) {
    return GTEXT_CSS_E_INDEX_SIZE;
  }
  if (scope->nadopted >= core->config.max_adopted) {
    return GTEXT_CSS_E_LIMIT_CSSOM;
  }
  if (!list_reserve(scope->obj.core, &scope->adopted, scope->nadopted, &scope->capadopted, 1)) {
    return GTEXT_CSS_E_OOM;
  }
  memmove(&scope->adopted[index + 1], &scope->adopted[index], (scope->nadopted - index) * sizeof *scope->adopted);
  scope->adopted[index] = sheet;
  scope->nadopted++;
  css_obj_retain_internal(&sheet->obj);
  core->structure_version++;
  css_obj_retain_external(&scope->obj);
  emit(core, GTEXT_CSSOM_EVENT_ADOPTED_CHANGED, scope, sheet, outcome);
  css_obj_release_external(&scope->obj);
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_scope_adopted_remove(GTEXT_CSSOM_Scope * scope, size_t index, GTEXT_CSSOM_Outcome * outcome) {
  if (!scope) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = scope_core(scope);
  css_outcome_reset(outcome, core);
  GTEXT_CSS_Status st = css_cssom_enter(core);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (index >= scope->nadopted) {
    return GTEXT_CSS_E_INDEX_SIZE;
  }
  css_sheet * sheet = scope->adopted[index];
  css_obj_retain_external(&sheet->obj);
  css_obj_retain_external(&scope->obj);
  memmove(&scope->adopted[index], &scope->adopted[index + 1], (scope->nadopted - index - 1) * sizeof *scope->adopted);
  scope->nadopted--;
  core->structure_version++;
  emit(core, GTEXT_CSSOM_EVENT_ADOPTED_CHANGED, scope, sheet, outcome);
  css_obj_release_internal(&sheet->obj);
  css_obj_release_external(&sheet->obj);
  css_obj_release_external(&scope->obj);
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_scope_adopted_set(GTEXT_CSSOM_Scope * scope, GTEXT_CSSOM_Sheet * const * sheets, size_t count, GTEXT_CSSOM_Outcome * outcome) {
  if (!scope || (!sheets && count)) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = scope_core(scope);
  css_outcome_reset(outcome, core);
  GTEXT_CSS_Status st = css_cssom_enter(core);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  for (size_t i = 0; i < count; i++) {
    if (!sheets[i]) {
      return GTEXT_CSS_E_INVALID;
    }
    if (!adoptable(scope, sheets[i])) {
      return GTEXT_CSS_E_NOT_ALLOWED;
    }
  }
  if (count > core->config.max_adopted) {
    return GTEXT_CSS_E_LIMIT_CSSOM;
  }
  bool same = count == scope->nadopted;
  for (size_t i = 0; same && i < count; i++) {
    same = scope->adopted[i] == sheets[i];
  }
  if (same) {
    return GTEXT_CSS_OK;   /* UNCHANGED */
  }
  css_sheet ** fresh = NULL;
  if (count) {
    fresh = gtext_allocator_malloc(&scope->obj.core->alloc, count * sizeof *fresh);
    if (!fresh) {
      return GTEXT_CSS_E_OOM;
    }
  }
  /* nothing below can fail; the new entries are held before the old are let go, so a sheet in both is never freed in between */
  css_sheet ** old = scope->adopted;
  const size_t nold = scope->nadopted;
  for (size_t i = 0; i < count; i++) {
    fresh[i] = sheets[i];
    css_obj_retain_internal(&fresh[i]->obj);
  }
  scope->adopted = fresh;
  scope->nadopted = scope->capadopted = count;
  core->structure_version++;
  css_obj_retain_external(&scope->obj);
  emit(core, GTEXT_CSSOM_EVENT_ADOPTED_CHANGED, scope, NULL, outcome);
  for (size_t i = 0; i < nold; i++) {
    css_obj_release_internal(&old[i]->obj);
  }
  GTEXT_Allocator a = scope->obj.core->alloc;
  gtext_allocator_free(&a, old);
  css_obj_release_external(&scope->obj);
  return GTEXT_CSS_OK;
}

/* ---- what the index needs ------------------------------------------------------------------------------------------------------------------ */

bool css_scope_owns(const css_scope * scope, const css_sheet * sheet) {
  return sheet->obj.core == scope_core(scope);
}

GTEXT_CSS_Status css_scope_stamp(const css_scope * scope, uint64_t * stamp, uint64_t * sheets) {
  for (size_t pass = 0; pass < 2; pass++) {
    css_sheet * const * list = pass == 0 ? scope->sheets : scope->adopted;
    const size_t n = pass == 0 ? scope->nsheets : scope->nadopted;
    for (size_t i = 0; i < n; i++) {
      if (!css_scope_owns(scope, list[i])) {
        continue;
      }
      uint64_t v = 0;
      const GTEXT_CSS_Status st = gtext_cssom_sheet_composite_version(list[i], &v);
      if (st != GTEXT_CSS_OK) {
        return st;
      }
      if (v > *stamp) {
        *stamp = v;
      }
      (*sheets)++;
    }
  }
  return GTEXT_CSS_OK;
}

typedef struct {
  uint32_t context;
  size_t order;
  css_scope * scope;
} ctx_entry;

static int ctx_cmp(const void * a, const void * b) {
  const ctx_entry * x = a;
  const ctx_entry * y = b;
  if (x->context != y->context) {
    return x->context < y->context ? -1 : 1;
  }
  return x->order < y->order ? -1 : (x->order > y->order);
}

css_scope ** css_scopes_by_context(css_core * core, size_t * count) {
  const size_t n = core->nscopes;
  *count = n;
  css_scope ** out = gtext_allocator_malloc(&core->alloc, (n ? n : 1) * sizeof *out);
  ctx_entry * tmp = gtext_allocator_malloc(&core->alloc, (n ? n : 1) * sizeof *tmp);
  if (!out || !tmp) {
    gtext_allocator_free(&core->alloc, out);
    gtext_allocator_free(&core->alloc, tmp);
    return NULL;
  }
  for (size_t i = 0; i < n; i++) {
    tmp[i].context = core->scopes[i]->context;
    tmp[i].order = i;
    tmp[i].scope = core->scopes[i];
  }
  qsort(tmp, n, sizeof *tmp, ctx_cmp);
  for (size_t i = 0; i < n; i++) {
    out[i] = tmp[i].scope;
  }
  gtext_allocator_free(&core->alloc, tmp);
  return out;
}
