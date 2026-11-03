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
 * `MediaQueryList` and the environment change (css_cssom_document.h): `matchMedia()`, the cached `matches`, the listeners, and Media Queries 4's "evaluate media queries and report
 * changes" as gtext_cssom_document_set_conditions() runs it, the conditions object the document evaluates with, and `CSS.supports()`.
 *
 * **The environment change is evaluate, then commit, then report.** Every list with a listener is evaluated against the new conditions first (identical queries once), so a failure
 * leaves the document untouched; then the conditions are swapped, the generation advances and the event goes out; then the listeners of the lists whose answer changed are called, in
 * the order the lists were made and, for one list, in the order the listeners were added, with the mutation guard held. A list with no listener is not evaluated: its answer is
 * stamped with the conditions it was made for and found stale when asked.
 */

#include <string.h>

#include "css_internal.h"
#include "cssom_doc_internal.h"

typedef struct {
  GTEXT_CSSOM_Media_Listener fn;
  void * user;
  bool removed;                 ///< Taken out while the list was delivering; dropped when the delivery ends.
} mql_listener;

struct GTEXT_CSSOM_Media_Query_List {
  css_obj obj;
  GTEXT_CSS_Media_List * list;  ///< The parsed query list (an owner held).
  char * text;                  ///< Its serialization.
  size_t text_length;
  bool has_answer;
  bool matches;
  uint64_t epoch;               ///< The conditions epoch `matches` was found for.
  mql_listener * listeners;
  size_t nlisteners, caplisteners;
  unsigned delivering;
  css_mql * prev, * next;       ///< The core's list of live lists, in creation order (weak).
};

static css_mql * mql_from_obj(css_obj * o) {
  return (css_mql *)o;
}

static void mql_destroy(css_obj * o) {
  css_mql * m = mql_from_obj(o);
  css_core * core = o->core;
  if (m->prev) {
    m->prev->next = m->next;
  }
  else {
    core->mql_head = m->next;
  }
  if (m->next) {
    m->next->prev = m->prev;
  }
  else {
    core->mql_tail = m->prev;
  }
  core->nmql--;
  GTEXT_Allocator a = core->alloc;
  gtext_css_media_list_free(m->list);
  gtext_allocator_free(&a, m->text);
  gtext_allocator_free(&a, m->listeners);
  gtext_allocator_free(&a, m);
}

static GTEXT_CSS_Media_Options media_options(css_core * core) {
  GTEXT_CSS_Media_Options mo = gtext_css_media_options_default();
  mo.allocator = &core->alloc;
  mo.parse.max_input_bytes = core->config.max_input_bytes;
  return mo;
}

GTEXT_CSS_Status gtext_cssom_match_media(GTEXT_CSSOM_Document * document, const char * text, size_t length, GTEXT_CSSOM_Media_Query_List ** out) {
  if (!document || !out || (!text && length)) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = css_document_core(document);
  if (length > core->config.max_input_bytes) {
    return GTEXT_CSS_E_LIMIT_INPUT;
  }
  if (core->nmql >= core->config.max_media_query_lists) {
    return GTEXT_CSS_E_LIMIT_CSSOM;
  }
  GTEXT_CSS_Status st = css_core_room(core, sizeof(css_mql));
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSS_Media_Options mo = media_options(core);
  GTEXT_CSS_Media_List * list = NULL;
  st = gtext_css_media_list_parse(text, length, &mo, &list, NULL);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  char * canon = NULL;
  size_t canon_length = 0;
  st = gtext_css_media_list_serialize_buffer(list, &core->alloc, &canon, &canon_length);
  if (st != GTEXT_CSS_OK) {
    gtext_css_media_list_free(list);
    return st;
  }
  GTEXT_Allocator a = core->alloc;
  css_mql * m = gtext_allocator_calloc(&a, 1, sizeof *m);
  if (!m) {
    gtext_css_media_buffer_free(&core->alloc, canon);
    gtext_css_media_list_free(list);
    return GTEXT_CSS_E_OOM;
  }
  css_obj_init(&m->obj, core, GTEXT_CSSOM_OBJECT_MEDIA_QUERY_LIST, 0, 1, sizeof *m, mql_destroy);
  m->list = list;
  m->text = canon;
  m->text_length = canon_length;
  m->prev = core->mql_tail;
  if (core->mql_tail) {
    core->mql_tail->next = m;
  }
  else {
    core->mql_head = m;
  }
  core->mql_tail = m;
  core->nmql++;
  if (!css_obj_charge(&m->obj, canon_length + 1)) {
    css_obj_release_external(&m->obj);   /* unlinked and freed with its text */
    return GTEXT_CSS_E_LIMIT_CSSOM;
  }
  *out = m;
  return GTEXT_CSS_OK;
}

GTEXT_CSSOM_Media_Query_List * gtext_cssom_media_query_list_retain(GTEXT_CSSOM_Media_Query_List * list) {
  if (list) {
    css_obj_retain_external(&list->obj);
  }
  return list;
}

void gtext_cssom_media_query_list_release(GTEXT_CSSOM_Media_Query_List * list) {
  if (list) {
    css_obj_release_external(&list->obj);
  }
}

GTEXT_CSS_Status gtext_cssom_media_query_list_media(const GTEXT_CSSOM_Media_Query_List * list, char ** out, size_t * length) {
  if (!list || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  char * copy = css_core_dup(list->obj.core, list->text, list->text_length);
  if (!copy) {
    return GTEXT_CSS_E_OOM;
  }
  *out = copy;
  if (length) {
    *length = list->text_length;
  }
  return GTEXT_CSS_OK;
}

/// Evaluate the list for `conditions` and count it.
static GTEXT_CSS_Status evaluate(css_core * core, const GTEXT_CSS_Conditions * conditions, const GTEXT_CSS_Media_List * list, bool * matches) {
  const GTEXT_CSS_Status st = gtext_css_conditions_matches_list(conditions, list, NULL, matches);
  if (st == GTEXT_CSS_OK) {
    core->counters.mql_evaluations++;
  }
  return st;
}

/// The answer for the conditions the document has now: the cached one when it is for them.
static GTEXT_CSS_Status current(css_mql * m, bool * matches) {
  css_core * core = m->obj.core;
  if (!m->has_answer || m->epoch != core->conditions_epoch) {
    bool now = false;
    const GTEXT_CSS_Status st = evaluate(core, core->conditions, m->list, &now);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    m->matches = now;
    m->has_answer = true;
    m->epoch = core->conditions_epoch;
  }
  *matches = m->matches;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_media_query_list_matches(GTEXT_CSSOM_Media_Query_List * list, bool * matches) {
  if (!list || !matches) {
    return GTEXT_CSS_E_INVALID;
  }
  return current(list, matches);
}

size_t gtext_cssom_media_query_list_listener_count(const GTEXT_CSSOM_Media_Query_List * list) {
  if (!list) {
    return 0;
  }
  size_t n = 0;
  for (size_t i = 0; i < list->nlisteners; i++) {
    n += !list->listeners[i].removed;
  }
  return n;
}

GTEXT_CSS_Status gtext_cssom_media_query_list_add_listener(GTEXT_CSSOM_Media_Query_List * list, GTEXT_CSSOM_Media_Listener fn, void * user) {
  if (!list || !fn) {
    return GTEXT_CSS_E_INVALID;
  }
  for (size_t i = 0; i < list->nlisteners; i++) {
    if (!list->listeners[i].removed && list->listeners[i].fn == fn && list->listeners[i].user == user) {
      return GTEXT_CSS_OK;   /* the same pair is added once */
    }
  }
  css_core * core = list->obj.core;
  bool ignored = false;
  GTEXT_CSS_Status st = current(list, &ignored);   /* the answer a later change is judged against */
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (list->nlisteners == list->caplisteners) {
    const size_t cap = list->caplisteners ? list->caplisteners * 2 : 2;
    mql_listener * grown = gtext_allocator_realloc(&core->alloc, list->listeners, cap * sizeof *grown);
    if (!grown) {
      return GTEXT_CSS_E_OOM;
    }
    list->listeners = grown;
    list->caplisteners = cap;
  }
  list->listeners[list->nlisteners].fn = fn;
  list->listeners[list->nlisteners].user = user;
  list->listeners[list->nlisteners].removed = false;
  list->nlisteners++;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_media_query_list_remove_listener(GTEXT_CSSOM_Media_Query_List * list, GTEXT_CSSOM_Media_Listener fn, void * user) {
  if (!list || !fn) {
    return GTEXT_CSS_E_INVALID;
  }
  for (size_t i = 0; i < list->nlisteners; i++) {
    mql_listener * l = &list->listeners[i];
    if (l->removed || l->fn != fn || l->user != user) {
      continue;
    }
    if (list->delivering) {
      l->removed = true;
    }
    else {
      memmove(l, l + 1, (list->nlisteners - i - 1) * sizeof *l);
      list->nlisteners--;
    }
    break;
  }
  return GTEXT_CSS_OK;
}

/* ---- the environment change ----------------------------------------------------------------------------------------------------------------- */

/// A memo of answers by the text of the query list: identical queries in one evaluation are evaluated once.
typedef struct {
  size_t * slots;   ///< Index + 1 into the lists, 0 for empty.
  size_t mask;
} memo;

static uint64_t text_hash(const char * s, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; i++) {
    h ^= (unsigned char)s[i];
    h *= 1099511628211ull;
  }
  return h;
}

GTEXT_CSS_Status gtext_cssom_document_set_conditions(GTEXT_CSSOM_Document * document, const GTEXT_CSS_Conditions * conditions, GTEXT_CSSOM_Outcome * outcome) {
  if (!document || !conditions) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = css_document_core(document);
  css_outcome_reset(outcome, core);
  GTEXT_CSS_Status st = css_cssom_enter(core);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (conditions == core->conditions) {
    return GTEXT_CSS_OK;   /* UNCHANGED */
  }
  GTEXT_Allocator a = core->alloc;
  /* the lists with a listener, in creation order, each held for the whole call */
  size_t n = 0;
  for (css_mql * m = core->mql_head; m; m = m->next) {
    n += gtext_cssom_media_query_list_listener_count(m) != 0;
  }
  css_mql ** lists = NULL;
  bool * answers = NULL;
  size_t * table = NULL;
  size_t mask = 0;
  if (n) {
    size_t slots = 4;
    while (slots < n * 2) {
      slots *= 2;
    }
    lists = gtext_allocator_malloc(&a, n * sizeof *lists);
    answers = gtext_allocator_malloc(&a, n * sizeof *answers);
    table = gtext_allocator_calloc(&a, slots, sizeof *table);
    mask = slots - 1;
    if (!lists || !answers || !table) {
      gtext_allocator_free(&a, lists);
      gtext_allocator_free(&a, answers);
      gtext_allocator_free(&a, table);
      return GTEXT_CSS_E_OOM;
    }
    size_t k = 0;
    for (css_mql * m = core->mql_head; m; m = m->next) {
      if (gtext_cssom_media_query_list_listener_count(m) != 0) {
        lists[k++] = m;
      }
    }
  }
  for (size_t i = 0; i < n && st == GTEXT_CSS_OK; i++) {
    css_mql * m = lists[i];
    size_t at = (size_t)text_hash(m->text, m->text_length) & mask;
    bool found = false;
    while (table[at]) {
      css_mql * seen = lists[table[at] - 1];
      if (seen->text_length == m->text_length && memcmp(seen->text, m->text, m->text_length) == 0) {
        answers[i] = answers[table[at] - 1];
        found = true;
        break;
      }
      at = (at + 1) & mask;
    }
    if (!found) {
      st = evaluate(core, conditions, m->list, &answers[i]);
      if (st == GTEXT_CSS_OK) {
        table[at] = i + 1;
      }
    }
  }
  gtext_allocator_free(&a, table);
  if (st != GTEXT_CSS_OK) {
    gtext_allocator_free(&a, lists);
    gtext_allocator_free(&a, answers);
    return st;
  }
  /* commit: nothing below can fail */
  GTEXT_CSS_Conditions * fresh = gtext_css_conditions_retain(conditions);
  GTEXT_CSS_Conditions * old = core->conditions;
  core->conditions = fresh;
  core->config.conditions = fresh;
  core->conditions_epoch++;
  bool * changed = answers;   /* reused: whether the answer is different from the last one */
  for (size_t i = 0; i < n; i++) {
    css_mql * m = lists[i];
    const bool differs = m->has_answer ? m->matches != answers[i] : false;
    css_obj_retain_external(&m->obj);
    m->matches = answers[i];
    m->has_answer = true;
    m->epoch = core->conditions_epoch;
    changed[i] = differs;
  }
  gtext_css_conditions_free(old);
  css_cssom_event ev;
  memset(&ev, 0, sizeof ev);
  ev.kind = GTEXT_CSSOM_EVENT_CONDITIONS_CHANGED;
  if (outcome) {
    outcome->kind = GTEXT_CSSOM_CHANGED;
  }
  css_cssom_commit(core, &ev, outcome);
  /* report: each list whose answer changed calls the listeners it had when the report began */
  for (size_t i = 0; i < n; i++) {
    css_mql * m = lists[i];
    if (changed[i]) {
      const size_t count = m->nlisteners;
      m->delivering++;
      /* as for the observers, nothing is called after the document was released (by an observer of this change or by a listener); the lists are held, so nothing is freed */
      for (size_t j = 0; core->document && j < count && j < m->nlisteners; j++) {
        if (m->listeners[j].removed) {
          continue;
        }
        const mql_listener l = m->listeners[j];
        core->in_callback++;
        const GTEXT_CSS_Status ls = l.fn(l.user, m, m->matches);
        core->in_callback--;
        core->counters.mql_notifications++;
        if (ls != GTEXT_CSS_OK && outcome && outcome->observer_status == GTEXT_CSS_OK) {
          outcome->observer_status = ls;
        }
      }
      if (--m->delivering == 0) {
        size_t w = 0;
        for (size_t j = 0; j < m->nlisteners; j++) {
          if (!m->listeners[j].removed) {
            m->listeners[w++] = m->listeners[j];
          }
        }
        m->nlisteners = w;
      }
    }
  }
  for (size_t i = 0; i < n; i++) {
    css_obj_release_external(&lists[i]->obj);
  }
  gtext_allocator_free(&a, lists);
  gtext_allocator_free(&a, answers);
  return GTEXT_CSS_OK;
}

/* ---- the conditions and `CSS.supports()` --------------------------------------------------------------------------------------------------------- */

const GTEXT_CSS_Conditions * gtext_cssom_document_conditions(const GTEXT_CSSOM_Document * document) {
  return document ? css_document_core((GTEXT_CSSOM_Document *)(uintptr_t)document)->conditions : NULL;
}

GTEXT_CSS_Status gtext_cssom_supports(const GTEXT_CSSOM_Document * document, const char * property, const char * value, bool * result) {
  if (!document || !property || !value || !result) {
    return GTEXT_CSS_E_INVALID;
  }
  return gtext_css_conditions_supports(gtext_cssom_document_conditions(document), property, value, result);
}

GTEXT_CSS_Status gtext_cssom_supports_text(const GTEXT_CSSOM_Document * document, const char * text, size_t length, bool * result) {
  if (!document || (!text && length) || !result) {
    return GTEXT_CSS_E_INVALID;
  }
  return gtext_css_conditions_supports_text(gtext_cssom_document_conditions(document), text, length, result);
}
