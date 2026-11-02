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
 * `MediaList` (css_cssom_media.h): a view of the media query list of a sheet, an `@import` rule or an `@media` rule. The list keeps nothing: the text is read from the owner each time
 * (the sheet's canonical string, the typed prelude of a rule), so it cannot disagree with it, and a change is written back through the owner (the sheet's string and version, or
 * the whole-rule replacement of cssom_setters.c). A medium is compared by its serialization, the one that "serialize a media query list" gives.
 */

#include <string.h>

#include "css_internal.h"
#include "cssom_typed.h"

struct GTEXT_CSSOM_Media_List {
  css_obj obj;
  css_obj * owner;   ///< The sheet or the rule: weak, cleared when it goes.
};

static void media_destroy(css_obj * o) {
  GTEXT_Allocator a = o->core->alloc;
  gtext_allocator_free(&a, o);
}

void css_media_owner_gone(struct GTEXT_CSSOM_Media_List ** list) {
  if (*list) {
    (*list)->owner = NULL;
    css_obj_release_internal(&(*list)->obj);
    *list = NULL;
  }
}

GTEXT_CSSOM_Media_List * gtext_cssom_media_retain(GTEXT_CSSOM_Media_List * list) {
  if (list) {
    css_obj_retain_external(&list->obj);
  }
  return list;
}

void gtext_cssom_media_release(GTEXT_CSSOM_Media_List * list) {
  if (list) {
    css_obj_release_external(&list->obj);
  }
}

/// The list object of an owner: the one already made, or a new one that the owner holds (an internal count) and that keeps the owner reachable while it is held.
static GTEXT_CSS_Status media_of(css_obj * owner, GTEXT_CSSOM_Media_List ** slot, GTEXT_CSSOM_Media_List ** out) {
  if (!*slot) {
    css_core * core = owner->core;
    GTEXT_CSS_Status st = css_core_room(core, sizeof(GTEXT_CSSOM_Media_List));
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    GTEXT_Allocator a = core->alloc;
    GTEXT_CSSOM_Media_List * m = gtext_allocator_calloc(&a, 1, sizeof *m);
    if (!m) {
      return GTEXT_CSS_E_OOM;
    }
    css_obj_init(&m->obj, core, GTEXT_CSSOM_OBJECT_MEDIA_LIST, 1, 0, sizeof *m, media_destroy);
    m->owner = owner;
    css_obj_set_parent(&m->obj, owner);
    *slot = m;
  }
  *out = gtext_cssom_media_retain(*slot);
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_sheet_media(GTEXT_CSSOM_Sheet * sheet, GTEXT_CSSOM_Media_List ** out) {
  if (!sheet || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  return media_of(&sheet->obj, &sheet->media, out);
}

GTEXT_CSS_Status gtext_cssom_rule_media(GTEXT_CSSOM_Rule * rule, GTEXT_CSSOM_Media_List ** out) {
  if (!rule || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  if (rule->kind != GTEXT_CSSOM_RULE_IMPORT && rule->kind != GTEXT_CSSOM_RULE_MEDIA) {
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  return media_of(&rule->obj, &rule->media, out);
}

/* ---- the text ------------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  const char * data;
  size_t length;
} span;

/// The queries of a canonical list: the text between commas outside brackets and quotes, trimmed. The empty text has none.
static size_t split_queries(const char * s, size_t n, span * out, size_t cap) {
  size_t count = 0;
  size_t i = 0;
  bool any = false;
  for (size_t k = 0; k < n; k++) {
    if (s[k] != ' ' && s[k] != '\n' && s[k] != '\t') {
      any = true;
      break;
    }
  }
  if (!any) {
    return 0;
  }
  while (i <= n) {
    size_t j = i;
    int depth = 0;
    char quote = 0;
    while (j < n) {
      const char c = s[j];
      if (quote) {
        if (c == '\\' && j + 1 < n) {
          j++;
        }
        else if (c == quote) {
          quote = 0;
        }
      }
      else if (c == '"' || c == '\'') {
        quote = c;
      }
      else if (c == '(' || c == '[') {
        depth++;
      }
      else if (c == ')' || c == ']') {
        depth--;
      }
      else if (c == ',' && depth <= 0) {
        break;
      }
      j++;
    }
    size_t a = i, b = j;
    while (a < b && (s[a] == ' ' || s[a] == '\n' || s[a] == '\t')) {
      a++;
    }
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\n' || s[b - 1] == '\t')) {
      b--;
    }
    if (count < cap) {
      out[count].data = s + a;
      out[count].length = b - a;
    }
    count++;
    i = j + 1;
  }
  return count;
}

/// The canonical text of a media query list as written: parsed ("a text always parses": a bad query is `not all`) and serialized. `queries` receives the count; `clean` is false when
/// any query was replaced or a diagnostic other than "unknown, kept" was raised.
static GTEXT_CSS_Status canonical(css_core * core, const char * text, size_t length, tbuf * out, size_t * queries, bool * clean) {
  GTEXT_CSS_Media_Options mo = gtext_css_media_options_default();
  mo.allocator = &core->alloc;
  mo.parse.max_input_bytes = core->config.max_input_bytes;
  GTEXT_CSS_Media_List * list = NULL;
  GTEXT_CSS_Media_Result result;
  memset(&result, 0, sizeof result);
  GTEXT_CSS_Status st = gtext_css_media_list_parse(text, length, &mo, &list, &result);
  if (st != GTEXT_CSS_OK) {
    gtext_css_media_result_free(&result);
    return st;
  }
  bool ok = true;
  for (size_t i = 0; i < gtext_css_media_result_diagnostic_count(&result); i++) {
    GTEXT_CSS_Media_Diagnostic d;
    if (gtext_css_media_result_diagnostic(&result, i, &d) && d.kind != GTEXT_CSS_MQDIAG_UNKNOWN) {
      ok = false;
    }
  }
  gtext_css_media_result_free(&result);
  char * buf = NULL;
  size_t n = 0;
  st = gtext_css_media_list_serialize_buffer(list, &core->alloc, &buf, &n);
  if (st == GTEXT_CSS_OK) {
    if (!tb_put(out, buf, n)) {
      st = GTEXT_CSS_E_OOM;
    }
    *queries = gtext_css_media_list_query_count(list);
    gtext_css_media_buffer_free(&core->alloc, buf);
  }
  gtext_css_media_list_free(list);
  if (clean) {
    *clean = ok;
  }
  return st;
}

/// The prefix and the media of an `@import`'s canonical prelude: the pieces that are the url, `layer` and `supports()`, and where the queries begin.
static size_t import_media_start(const char * s, size_t n) {
  size_t i = 0;
  size_t pieces = 0;
  while (i < n) {
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) {
      i++;
    }
    if (i >= n) {
      return n;
    }
    const size_t start = i;
    int depth = 0;
    char quote = 0;
    while (i < n) {
      const char c = s[i];
      if (quote) {
        if (c == '\\' && i + 1 < n) {
          i++;
        }
        else if (c == quote) {
          quote = 0;
        }
      }
      else if (c == '"' || c == '\'') {
        quote = c;
      }
      else if (c == '(') {
        depth++;
      }
      else if (c == ')') {
        depth--;
      }
      else if (depth == 0 && (c == ' ' || c == '\t' || c == '\n')) {
        break;
      }
      i++;
    }
    const size_t len = i - start;
    const bool url = pieces == 0;
    const bool layer = len >= 5 && (s[start] | 32) == 'l' && (s[start + 1] | 32) == 'a' && (s[start + 2] | 32) == 'y' && (s[start + 3] | 32) == 'e' && (s[start + 4] | 32) == 'r' &&
                       (len == 5 || s[start + 5] == '(');
    const bool supports = len >= 9 && memcmp(s + start, "supports(", 9) == 0;
    if (!url && !layer && !supports) {
      return start;
    }
    pieces++;
  }
  return n;
}

/// The media query list of a sheet, an `@import` rule or an `@media` rule as text, appended to `out`.
static GTEXT_CSS_Status owner_obj_text(const css_obj * owner, tbuf * out) {
  if (!owner) {
    return GTEXT_CSS_E_INVALID_STATE;
  }
  if (owner->kind == GTEXT_CSSOM_OBJECT_SHEET) {
    const css_sheet * s = (const css_sheet *)(uintptr_t)owner;
    return !s->media_text || tb_put(out, s->media_text, strlen(s->media_text)) ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
  }
  css_rule * r = (css_rule *)(uintptr_t)owner;
  GTEXT_CSS_Atrule * a = NULL;
  GTEXT_CSS_Status st = css_rule_typed(r->obj.core, r, true, &a);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  size_t n = 0;
  const char * p = gtext_css_atrule_prelude_text(a, &n);
  size_t from = 0;
  if (r->kind == GTEXT_CSSOM_RULE_IMPORT) {
    from = import_media_start(p, n);
  }
  if (n > from && !tb_put(out, p + from, n - from)) {
    st = GTEXT_CSS_E_OOM;
  }
  gtext_css_atrule_free(a);
  return st;
}

static GTEXT_CSS_Status owner_text(GTEXT_CSSOM_Media_List * list, tbuf * out) {
  return owner_obj_text(list->owner, out);
}

GTEXT_CSS_Status css_rule_media_text(css_rule * rule, char ** out, size_t * length) {
  tbuf b;
  tb_init(&b, &rule->obj.core->alloc);
  GTEXT_CSS_Status st = owner_obj_text(&rule->obj, &b);
  if (st != GTEXT_CSS_OK || b.oom) {
    tb_free(&b);
    return st != GTEXT_CSS_OK ? st : GTEXT_CSS_E_OOM;
  }
  *out = tb_take(&b, length);
  return *out ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
}

GTEXT_CSS_Status gtext_cssom_media_text(GTEXT_CSSOM_Media_List * list, char ** out, size_t * length) {
  if (!list || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  tbuf b;
  tb_init(&b, &list->obj.core->alloc);
  GTEXT_CSS_Status st = owner_text(list, &b);
  if (st != GTEXT_CSS_OK || b.oom) {
    tb_free(&b);
    return st != GTEXT_CSS_OK ? st : GTEXT_CSS_E_OOM;
  }
  size_t n = 0;
  char * s = tb_take(&b, &n);
  if (!s) {
    return GTEXT_CSS_E_OOM;
  }
  *out = s;
  if (length) {
    *length = n;
  }
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_cssom_media_length(GTEXT_CSSOM_Media_List * list, size_t * out) {
  if (!list || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  tbuf b;
  tb_init(&b, &list->obj.core->alloc);
  GTEXT_CSS_Status st = owner_text(list, &b);
  if (st == GTEXT_CSS_OK && b.oom) {
    st = GTEXT_CSS_E_OOM;
  }
  if (st == GTEXT_CSS_OK) {
    *out = split_queries(b.data, b.len, NULL, 0);
  }
  tb_free(&b);
  return st;
}

GTEXT_CSS_Status gtext_cssom_media_item(GTEXT_CSSOM_Media_List * list, size_t index, char ** out, size_t * length, bool * is_null) {
  if (!list || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  css_core * core = list->obj.core;
  tbuf b;
  tb_init(&b, &core->alloc);
  GTEXT_CSS_Status st = owner_text(list, &b);
  if (st == GTEXT_CSS_OK && b.oom) {
    st = GTEXT_CSS_E_OOM;
  }
  if (st != GTEXT_CSS_OK) {
    tb_free(&b);
    return st;
  }
  const size_t count = split_queries(b.data, b.len, NULL, 0);
  span one = {"", 0};
  bool found = false;
  if (index < count) {
    span *all = gtext_allocator_malloc(&core->alloc, count * sizeof *all);
    if (!all) {
      tb_free(&b);
      return GTEXT_CSS_E_OOM;
    }
    split_queries(b.data, b.len, all, count);
    one = all[index];
    found = true;
    gtext_allocator_free(&core->alloc, all);
  }
  *out = css_core_dup(core, one.data, one.length);
  tb_free(&b);
  if (!*out) {
    return GTEXT_CSS_E_OOM;
  }
  if (length) {
    *length = one.length;
  }
  if (is_null) {
    *is_null = !found;
  }
  return GTEXT_CSS_OK;
}

/* ---- changing it ---------------------------------------------------------------------------------------------------------------------------- */

static GTEXT_CSS_Status begin(GTEXT_CSSOM_Media_List * list, GTEXT_CSSOM_Outcome * outcome) {
  css_core * core = list->obj.core;
  css_outcome_reset(outcome, core);
  GTEXT_CSS_Status st = css_cssom_enter(core);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!list->owner) {
    return GTEXT_CSS_E_INVALID_STATE;
  }
  if (list->owner->kind == GTEXT_CSSOM_OBJECT_SHEET) {
    const css_sheet * s = (const css_sheet *)(uintptr_t)list->owner;
    if (!s->origin_clean) {
      return GTEXT_CSS_E_SECURITY;
    }
  }
  return GTEXT_CSS_OK;
}

/// Make `canon` (a canonical list) the owner's list. UNCHANGED when it already is.
static GTEXT_CSS_Status store(GTEXT_CSSOM_Media_List * list, const char * canon, size_t n, GTEXT_CSSOM_Outcome * outcome) {
  css_core * core = list->obj.core;
  tbuf now;
  tb_init(&now, &core->alloc);
  GTEXT_CSS_Status st = owner_text(list, &now);
  if (st == GTEXT_CSS_OK && now.oom) {
    st = GTEXT_CSS_E_OOM;
  }
  const bool same = st == GTEXT_CSS_OK && now.len == n && (n == 0 || memcmp(now.data, canon, n) == 0);
  tb_free(&now);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (same) {
    if (outcome) {
      outcome->kind = GTEXT_CSSOM_UNCHANGED;
    }
    return GTEXT_CSS_OK;
  }
  if (list->owner->kind == GTEXT_CSSOM_OBJECT_SHEET) {
    css_sheet * s = (css_sheet *)(uintptr_t)list->owner;
    char * copy = n ? css_core_dup(core, canon, n) : NULL;
    if (n && !copy) {
      return GTEXT_CSS_E_OOM;
    }
    GTEXT_Allocator a = core->alloc;
    gtext_allocator_free(&a, s->media_text);
    s->media_text = copy;
    css_sheet_bump(s);
    css_cssom_event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = GTEXT_CSSOM_EVENT_SHEET_MEDIA_CHANGED;
    ev.sheet = s;
    ev.sheet_id = s->obj.id;
    if (outcome) {
      outcome->kind = GTEXT_CSSOM_CHANGED;
    }
    css_cssom_commit(core, &ev, outcome);
    return GTEXT_CSS_OK;
  }
  css_rule * r = (css_rule *)(uintptr_t)list->owner;
  tbuf header;
  tb_init(&header, &core->alloc);
  if (r->kind == GTEXT_CSSOM_RULE_MEDIA) {
    if (!tb_puts(&header, "@media") || (n && (!tb_puts(&header, " ") || !tb_put(&header, canon, n)))) {
      tb_free(&header);
      return GTEXT_CSS_E_OOM;
    }
  }
  else {
    GTEXT_CSS_Atrule * a = NULL;
    st = css_rule_typed(core, r, true, &a);
    if (st != GTEXT_CSS_OK) {
      tb_free(&header);
      return st;
    }
    size_t pl = 0;
    const char * p = gtext_css_atrule_prelude_text(a, &pl);
    const size_t from = import_media_start(p, pl);
    if (!tb_puts(&header, "@import ") || !tb_put(&header, p, from) || (n && !tb_put(&header, canon, n))) {
      st = GTEXT_CSS_E_OOM;
    }
    gtext_css_atrule_free(a);
    while (st == GTEXT_CSS_OK && header.len && (header.data[header.len - 1] == ' ')) {
      header.len--;
    }
  }
  if (st != GTEXT_CSS_OK || header.oom) {
    tb_free(&header);
    return st != GTEXT_CSS_OK ? st : GTEXT_CSS_E_OOM;
  }
  bool ignored = false;
  st = css_rule_replace_own(r, header.data, header.len, "", 0, r->kind == GTEXT_CSSOM_RULE_IMPORT, NULL, NULL, &ignored, outcome);
  tb_free(&header);
  if (st == GTEXT_CSS_OK && ignored) {
    return GTEXT_CSS_E_INTERNAL;   /* a canonical list is always a valid one */
  }
  return st;
}

GTEXT_CSS_Status gtext_cssom_media_set_text(GTEXT_CSSOM_Media_List * list, const char * text, size_t length, GTEXT_CSSOM_Outcome * outcome) {
  GTEXT_CSSOM_Outcome local;
  GTEXT_CSSOM_Outcome * out = outcome ? outcome : &local;
  if (!list || (!text && length)) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Status st = begin(list, out);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  css_core * core = list->obj.core;
  if (length > core->config.max_input_bytes) {
    return GTEXT_CSS_E_LIMIT_INPUT;
  }
  tbuf canon;
  tb_init(&canon, &core->alloc);
  size_t queries = 0;
  st = canonical(core, text ? text : "", length, &canon, &queries, NULL);
  if (st == GTEXT_CSS_OK && canon.oom) {
    st = GTEXT_CSS_E_OOM;
  }
  if (st == GTEXT_CSS_OK) {
    st = store(list, canon.data ? canon.data : "", canon.len, out);
  }
  tb_free(&canon);
  return st;
}

GTEXT_CSS_Status gtext_cssom_media_append(GTEXT_CSSOM_Media_List * list, const char * medium, size_t length, GTEXT_CSSOM_Outcome * outcome) {
  GTEXT_CSSOM_Outcome local;
  GTEXT_CSSOM_Outcome * out = outcome ? outcome : &local;
  if (!list || (!medium && length)) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Status st = begin(list, out);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  css_core * core = list->obj.core;
  if (length > core->config.max_input_bytes) {
    return GTEXT_CSS_E_LIMIT_INPUT;
  }
  tbuf one, now;
  tb_init(&one, &core->alloc);
  tb_init(&now, &core->alloc);
  size_t queries = 0;
  st = canonical(core, medium ? medium : "", length, &one, &queries, NULL);
  if (st == GTEXT_CSS_OK) {
    st = owner_text(list, &now);
  }
  if (st == GTEXT_CSS_OK && (one.oom || now.oom)) {
    st = GTEXT_CSS_E_OOM;
  }
  if (st == GTEXT_CSS_OK && queries != 1) {
    /* CSSOM, 4.1: "To parse a media query for a given string s means to follow the parse a media query list steps and return null if more than one media query is returned", and the
       empty text returns none: "If m is null, then return." A query that is merely not valid is not null: it "ends up being 'ignored'" and "will turn into 'not all'". */
    out->kind = GTEXT_CSSOM_UNCHANGED;
  }
  else if (st == GTEXT_CSS_OK) {
    const size_t count = split_queries(now.data, now.len, NULL, 0);
    bool present = false;
    if (count) {
      span *all = gtext_allocator_malloc(&core->alloc, count * sizeof *all);
      if (!all) {
        st = GTEXT_CSS_E_OOM;
      }
      else {
        split_queries(now.data, now.len, all, count);
        for (size_t i = 0; i < count; i++) {
          present = present || (all[i].length == one.len && memcmp(all[i].data, one.data, one.len) == 0);
        }
        gtext_allocator_free(&core->alloc, all);
      }
    }
    if (st == GTEXT_CSS_OK && present) {
      out->kind = GTEXT_CSSOM_UNCHANGED;
    }
    else if (st == GTEXT_CSS_OK) {
      tbuf next;
      tb_init(&next, &core->alloc);
      if ((now.len && (!tb_put(&next, now.data, now.len) || !tb_puts(&next, ", "))) || !tb_put(&next, one.data, one.len) || next.oom) {
        st = GTEXT_CSS_E_OOM;
      }
      else {
        st = store(list, next.data, next.len, out);
      }
      tb_free(&next);
    }
  }
  tb_free(&one);
  tb_free(&now);
  return st;
}

GTEXT_CSS_Status gtext_cssom_media_delete(GTEXT_CSSOM_Media_List * list, const char * medium, size_t length, GTEXT_CSSOM_Outcome * outcome) {
  GTEXT_CSSOM_Outcome local;
  GTEXT_CSSOM_Outcome * out = outcome ? outcome : &local;
  if (!list || (!medium && length)) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Status st = begin(list, out);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  css_core * core = list->obj.core;
  if (length > core->config.max_input_bytes) {
    return GTEXT_CSS_E_LIMIT_INPUT;
  }
  tbuf one, now, next;
  tb_init(&one, &core->alloc);
  tb_init(&now, &core->alloc);
  tb_init(&next, &core->alloc);
  size_t queries = 0;
  st = canonical(core, medium ? medium : "", length, &one, &queries, NULL);
  if (st == GTEXT_CSS_OK) {
    st = owner_text(list, &now);
  }
  if (st == GTEXT_CSS_OK && (one.oom || now.oom)) {
    st = GTEXT_CSS_E_OOM;
  }
  if (st == GTEXT_CSS_OK && queries != 1) {
    out->kind = GTEXT_CSSOM_UNCHANGED;   /* CSSOM: "Let m be the result of parsing the given value. If m is null, then return." (none, or more than one, query) */
  }
  else if (st == GTEXT_CSS_OK) {
    const size_t count = split_queries(now.data, now.len, NULL, 0);
    span *all = count ? gtext_allocator_malloc(&core->alloc, count * sizeof *all) : NULL;
    if (count && !all) {
      st = GTEXT_CSS_E_OOM;
    }
    else {
      if (count) {
        split_queries(now.data, now.len, all, count);
      }
      bool found = false;
      for (size_t i = 0; st == GTEXT_CSS_OK && i < count; i++) {
        if (all[i].length == one.len && memcmp(all[i].data, one.data, one.len) == 0) {
          found = true;
          continue;
        }
        if ((next.len && !tb_puts(&next, ", ")) || !tb_put(&next, all[i].data, all[i].length)) {
          st = GTEXT_CSS_E_OOM;
        }
      }
      gtext_allocator_free(&core->alloc, all);
      if (st == GTEXT_CSS_OK && !found) {
        st = GTEXT_CSS_E_NOT_FOUND;   /* "If nothing was removed, then throw a NotFoundError exception." */
      }
      else if (st == GTEXT_CSS_OK && next.oom) {
        st = GTEXT_CSS_E_OOM;
      }
      else if (st == GTEXT_CSS_OK) {
        st = store(list, next.data ? next.data : "", next.len, out);
      }
    }
  }
  tb_free(&one);
  tb_free(&now);
  tb_free(&next);
  return st;
}

GTEXT_CSS_Status css_media_canonical(css_core * core, const char * text, size_t length, tbuf * out) {
  size_t queries = 0;
  const GTEXT_CSS_Status st = canonical(core, text, length, out, &queries, NULL);
  return st == GTEXT_CSS_OK && out->oom ? GTEXT_CSS_E_OOM : st;
}
