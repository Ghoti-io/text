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
      else if (c == '\\' && j + 1 < n) {
        j++;   /* an escape (`\)` is no parenthesis) */
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

/* ---- the list as it was read ------------------------------------------------------------------------------------------------------------------ */

static void read_free_raw(GTEXT_Allocator * a, css_media_read * r) {
  if (r) {
    for (size_t i = 0; i < r->count; i++) {
      gtext_allocator_free(a, r->text[i]);
    }
    gtext_allocator_free(a, r->text);
    gtext_allocator_free(a, r);
  }
}

void css_media_read_free(css_core * core, css_media_read * r) {
  GTEXT_Allocator a = core->alloc;
  read_free_raw(&a, r);
}

/// A table of `count` queries with none read otherwise. NULL on memory.
static css_media_read * read_new(css_core * core, size_t count) {
  GTEXT_Allocator a = core->alloc;
  css_media_read * r = gtext_allocator_calloc(&a, 1, sizeof *r);
  if (!r) {
    return NULL;
  }
  r->text = gtext_allocator_calloc(&a, count ? count : 1, sizeof *r->text);
  if (!r->text) {
    gtext_allocator_free(&a, r);
    return NULL;
  }
  r->count = count;
  r->bytes = sizeof *r + (count ? count : 1) * sizeof *r->text;
  return r;
}

/// Set entry `i` to a copy of the text. False on memory.
static bool read_set(css_core * core, css_media_read * r, size_t i, const char * text, size_t n) {
  char * copy = css_core_dup(core, text, n);
  if (!copy) {
    return false;
  }
  r->text[i] = copy;
  r->bytes += n + 1;
  return true;
}

/// Whether no entry differs from the canonical text.
static bool read_empty(const css_media_read * r) {
  for (size_t i = 0; i < r->count; i++) {
    if (r->text[i]) {
      return false;
    }
  }
  return true;
}

void css_media_read_drop(css_obj * owner, css_media_read ** slot, bool credit) {
  css_media_read * r = *slot;
  if (!r) {
    return;
  }
  *slot = NULL;
  if (credit) {
    css_obj_credit(owner, r->bytes);
  }
  css_media_read_free(owner->core, r);
}

/// Take `read` for the slot of `owner` (an empty one is none), charging its bytes. False, with `read` freed and the slot untouched, when the limit would be passed.
static bool read_take(css_obj * owner, css_media_read ** slot, css_media_read * read) {
  if (read && read_empty(read)) {
    css_media_read_free(owner->core, read);
    read = NULL;
  }
  if (read && !css_obj_charge(owner, read->bytes)) {
    css_media_read_free(owner->core, read);
    return false;
  }
  css_media_read_drop(owner, slot, true);
  *slot = read;
  return true;
}

bool css_media_read_install_sheet(struct GTEXT_CSSOM_Sheet * sheet, css_media_read * read) {
  return read_take(&sheet->obj, &sheet->media_read, read);
}

/// The canonical text `c` with the queries of `read` put in the places of the ones they replace, appended to `out`.
static GTEXT_CSS_Status read_join(const char * c, size_t n, const css_media_read * read, tbuf * out) {
  if (!read) {
    return !n || tb_put(out, c, n) ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
  }
  const size_t count = split_queries(c, n, NULL, 0);
  if (count != read->count) {
    return GTEXT_CSS_E_INTERNAL;
  }
  span * all = count ? gtext_allocator_malloc(&out->alloc, count * sizeof *all) : NULL;
  if (count && !all) {
    return GTEXT_CSS_E_OOM;
  }
  if (count) {
    split_queries(c, n, all, count);
  }
  size_t at = 0;
  bool ok = true;
  for (size_t i = 0; ok && i < count; i++) {
    const size_t from = (size_t)(all[i].data - c);
    ok = tb_put(out, c + at, from - at);
    if (ok && read->text[i]) {
      ok = tb_put(out, read->text[i], strlen(read->text[i]));
    }
    else if (ok) {
      ok = tb_put(out, all[i].data, all[i].length);
    }
    at = from + all[i].length;
  }
  ok = ok && tb_put(out, c + at, n - at);
  gtext_allocator_free(&out->alloc, all);
  return ok ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
}

/// The canonical text of a media query list as written: parsed ("a text always parses": a bad query is `not all`) and serialized. `queries` receives the count; `clean` is false when
/// any query was replaced or a diagnostic other than "unknown, kept" was raised. When `read` is not NULL it receives the queries as they were read if any differs from its canonical
/// text (an unclosed `<general-enclosed>`), NULL otherwise.
static GTEXT_CSS_Status canonical(css_core * core, const char * text, size_t length, tbuf * out, size_t * queries, bool * clean, css_media_read ** read) {
  if (read) {
    *read = NULL;
  }
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
  if (st == GTEXT_CSS_OK && read && gtext_css_media_list_as_read_differs(list)) {
    const size_t count = gtext_css_media_list_query_count(list);
    css_media_read * r = read_new(core, count);
    if (!r) {
      st = GTEXT_CSS_E_OOM;
    }
    for (size_t i = 0; st == GTEXT_CSS_OK && i < count; i++) {
      char * closed = NULL;
      char * asread = NULL;
      size_t cn = 0, an = 0;
      st = gtext_css_media_list_serialize_query_buffer(list, i, false, &core->alloc, &closed, &cn);
      if (st == GTEXT_CSS_OK) {
        st = gtext_css_media_list_serialize_query_buffer(list, i, true, &core->alloc, &asread, &an);
      }
      if (st == GTEXT_CSS_OK && (cn != an || memcmp(closed, asread, cn) != 0) && !read_set(core, r, i, asread, an)) {
        st = GTEXT_CSS_E_OOM;
      }
      gtext_css_media_buffer_free(&core->alloc, closed);
      gtext_css_media_buffer_free(&core->alloc, asread);
    }
    if (st != GTEXT_CSS_OK) {
      css_media_read_free(core, r);
    }
    else {
      *read = r;
    }
  }
  gtext_css_media_list_free(list);
  if (clean) {
    *clean = ok;
  }
  return st;
}

/// The prefix and the media of an `@import`'s canonical prelude: the pieces that are the url, `layer` and `supports()`, and where the queries begin.
size_t css_import_media_start(const char * s, size_t n) {
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

/// The media query list of a sheet, an `@import` rule or an `@media` rule as text, appended to `out`: the canonical text, or, when `as_read`, the text as it was read.
static GTEXT_CSS_Status owner_obj_text(const css_obj * owner, tbuf * out, bool as_read) {
  if (!owner) {
    return GTEXT_CSS_E_INVALID_STATE;
  }
  if (owner->kind == GTEXT_CSSOM_OBJECT_SHEET) {
    const css_sheet * s = (const css_sheet *)(uintptr_t)owner;
    if (!s->media_text) {
      return GTEXT_CSS_OK;
    }
    return read_join(s->media_text, strlen(s->media_text), as_read ? s->media_read : NULL, out);
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
    from = css_import_media_start(p, n);
  }
  if (n > from) {
    st = read_join(p + from, n - from, as_read ? r->media_read : NULL, out);
  }
  gtext_css_atrule_free(a);
  return st;
}

/// What the user reads of the list: as it was read.
static GTEXT_CSS_Status owner_text(GTEXT_CSSOM_Media_List * list, tbuf * out) {
  return owner_obj_text(list->owner, out, true);
}

GTEXT_CSS_Status css_rule_prelude_read(const css_rule * rule, const char * prelude, size_t length, tbuf * out) {
  if (!rule->media_read || (rule->kind != GTEXT_CSSOM_RULE_MEDIA && rule->kind != GTEXT_CSSOM_RULE_IMPORT)) {
    return !length || tb_put(out, prelude, length) ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
  }
  const size_t from = rule->kind == GTEXT_CSSOM_RULE_IMPORT ? css_import_media_start(prelude, length) : 0;
  if (from && !tb_put(out, prelude, from)) {
    return GTEXT_CSS_E_OOM;
  }
  return read_join(prelude + from, length - from, rule->media_read, out);
}

GTEXT_CSS_Status css_rule_media_text(css_rule * rule, char ** out, size_t * length) {
  tbuf b;
  tb_init(&b, &rule->obj.core->alloc);
  GTEXT_CSS_Status st = owner_obj_text(&rule->obj, &b, false);
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

static css_media_read * owner_read(const css_obj * owner);
static span item_read(const css_media_read * read, size_t i, span item);

GTEXT_CSS_Status gtext_cssom_media_length(GTEXT_CSSOM_Media_List * list, size_t * out) {
  if (!list || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  tbuf b;
  tb_init(&b, &list->obj.core->alloc);
  GTEXT_CSS_Status st = owner_obj_text(list->owner, &b, false);   /* the queries are those of the canonical text */
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
  GTEXT_CSS_Status st = owner_obj_text(list->owner, &b, false);   /* the queries are those of the canonical text */
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
    one = item_read(owner_read(list->owner), index, all[index]);   /* and each is written as it was read */
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

static css_media_read * owner_read(const css_obj * owner) {
  if (!owner) {
    return NULL;
  }
  if (owner->kind == GTEXT_CSSOM_OBJECT_SHEET) {
    return ((const css_sheet *)(uintptr_t)owner)->media_read;
  }
  return ((const css_rule *)(uintptr_t)owner)->media_read;
}

/// Query `i` as it was read: the entry of `read`, or the canonical text `item`.
static span item_read(const css_media_read * read, size_t i, span item) {
  if (read && i < read->count && read->text[i]) {
    span r = {read->text[i], strlen(read->text[i])};
    return r;
  }
  return item;
}

static bool span_eq(span a, span b) {
  return a.length == b.length && (a.length == 0 || memcmp(a.data, b.data, a.length) == 0);
}

/// The queries of the canonical text `now`, in a new array (NULL when there are none), `*count` of them. `*st` is OOM when the array could not be made.
static span * items_of(css_core * core, const tbuf * now, size_t * count, GTEXT_CSS_Status * st) {
  *count = split_queries(now->data, now->len, NULL, 0);
  if (!*count) {
    return NULL;
  }
  span * all = gtext_allocator_malloc(&core->alloc, *count * sizeof *all);
  if (!all) {
    *st = GTEXT_CSS_E_OOM;
    return NULL;
  }
  split_queries(now->data, now->len, all, *count);
  return all;
}

/// Make `canon` (a canonical list) the owner's list, read as `read` (NULL when it is the canonical text; taken in every case). UNCHANGED when it already is.
static GTEXT_CSS_Status store(GTEXT_CSSOM_Media_List * list, const char * canon, size_t n, css_media_read * read, GTEXT_CSSOM_Outcome * outcome) {
  css_core * core = list->obj.core;
  tbuf now, now_read, want_read;
  tb_init(&now, &core->alloc);
  tb_init(&now_read, &core->alloc);
  tb_init(&want_read, &core->alloc);
  GTEXT_CSS_Status st = owner_obj_text(list->owner, &now, false);
  if (st == GTEXT_CSS_OK) {
    st = owner_obj_text(list->owner, &now_read, true);
  }
  if (st == GTEXT_CSS_OK) {
    st = read_join(canon, n, read, &want_read);
  }
  if (st == GTEXT_CSS_OK && (now.oom || now_read.oom || want_read.oom)) {
    st = GTEXT_CSS_E_OOM;
  }
  const bool same = st == GTEXT_CSS_OK && now.len == n && (n == 0 || memcmp(now.data, canon, n) == 0) && now_read.len == want_read.len &&
                    (now_read.len == 0 || memcmp(now_read.data, want_read.data, now_read.len) == 0);
  tb_free(&now);
  tb_free(&now_read);
  tb_free(&want_read);
  if (st != GTEXT_CSS_OK) {
    css_media_read_free(core, read);
    return st;
  }
  if (same) {
    css_media_read_free(core, read);
    if (outcome) {
      outcome->kind = GTEXT_CSSOM_UNCHANGED;
    }
    return GTEXT_CSS_OK;
  }
  if (list->owner->kind == GTEXT_CSSOM_OBJECT_SHEET) {
    css_sheet * s = (css_sheet *)(uintptr_t)list->owner;
    char * copy = n ? css_core_dup(core, canon, n) : NULL;
    if (n && !copy) {
      css_media_read_free(core, read);
      return GTEXT_CSS_E_OOM;
    }
    GTEXT_Allocator a = core->alloc;
    if (!read_take(&s->obj, &s->media_read, read)) {
      gtext_allocator_free(&a, copy);
      return GTEXT_CSS_E_LIMIT_CSSOM;
    }
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
      css_media_read_free(core, read);
      return GTEXT_CSS_E_OOM;
    }
  }
  else {
    GTEXT_CSS_Atrule * a = NULL;
    st = css_rule_typed(core, r, true, &a);
    if (st != GTEXT_CSS_OK) {
      tb_free(&header);
      css_media_read_free(core, read);
      return st;
    }
    size_t pl = 0;
    const char * p = gtext_css_atrule_prelude_text(a, &pl);
    const size_t from = css_import_media_start(p, pl);
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
    css_media_read_free(core, read);
    return st != GTEXT_CSS_OK ? st : GTEXT_CSS_E_OOM;
  }
  /* the list as read goes in with the new prelude: css_own_swap() moves it in the same step that replaces the rule's text, so an event of the change already reads it */
  css_media_read_drop(&r->obj, &r->media_read_next, true);
  if (!read_take(&r->obj, &r->media_read_next, read)) {
    tb_free(&header);
    return GTEXT_CSS_E_LIMIT_CSSOM;
  }
  bool ignored = false;
  st = css_rule_replace_own(r, header.data, header.len, "", 0, r->kind == GTEXT_CSSOM_RULE_IMPORT, NULL, NULL, &ignored, outcome);
  tb_free(&header);
  css_media_read_drop(&r->obj, &r->media_read_next, true);   /* still there only when the rule was not replaced */
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
  css_media_read * read = NULL;
  st = canonical(core, text ? text : "", length, &canon, &queries, NULL, &read);
  if (st == GTEXT_CSS_OK && canon.oom) {
    css_media_read_free(core, read);
    st = GTEXT_CSS_E_OOM;
  }
  if (st == GTEXT_CSS_OK) {
    st = store(list, canon.data ? canon.data : "", canon.len, read, out);
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
  css_media_read * one_read = NULL;
  st = canonical(core, medium ? medium : "", length, &one, &queries, NULL, &one_read);
  if (st == GTEXT_CSS_OK) {
    st = owner_obj_text(list->owner, &now, false);
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
    /* compared as they were read: `screen and (` and `screen and ()` are two media */
    const css_media_read * have = owner_read(list->owner);
    const span mine = item_read(one_read, 0, (span){one.data, one.len});
    size_t count = 0;
    span * all = items_of(core, &now, &count, &st);
    bool present = false;
    for (size_t i = 0; st == GTEXT_CSS_OK && i < count; i++) {
      present = present || span_eq(item_read(have, i, all[i]), mine);
    }
    if (st == GTEXT_CSS_OK && present) {
      out->kind = GTEXT_CSSOM_UNCHANGED;
    }
    else if (st == GTEXT_CSS_OK) {
      tbuf next;
      tb_init(&next, &core->alloc);
      css_media_read * nr = NULL;
      if ((now.len && (!tb_put(&next, now.data, now.len) || !tb_puts(&next, ", "))) || !tb_put(&next, one.data, one.len) || next.oom) {
        st = GTEXT_CSS_E_OOM;
      }
      if (st == GTEXT_CSS_OK && (have || one_read)) {
        nr = read_new(core, count + 1);
        st = nr ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
        for (size_t i = 0; st == GTEXT_CSS_OK && i <= count; i++) {
          const char * t = i < count ? (have && i < have->count ? have->text[i] : NULL) : (one_read ? one_read->text[0] : NULL);
          if (t && !read_set(core, nr, i, t, strlen(t))) {
            st = GTEXT_CSS_E_OOM;
          }
        }
        if (st != GTEXT_CSS_OK) {
          css_media_read_free(core, nr);
          nr = NULL;
        }
      }
      if (st == GTEXT_CSS_OK) {
        st = store(list, next.data, next.len, nr, out);
      }
      tb_free(&next);
    }
    gtext_allocator_free(&core->alloc, all);
  }
  css_media_read_free(core, one_read);
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
  css_media_read * one_read = NULL;
  st = canonical(core, medium ? medium : "", length, &one, &queries, NULL, &one_read);
  if (st == GTEXT_CSS_OK) {
    st = owner_obj_text(list->owner, &now, false);
  }
  if (st == GTEXT_CSS_OK && (one.oom || now.oom)) {
    st = GTEXT_CSS_E_OOM;
  }
  if (st == GTEXT_CSS_OK && queries != 1) {
    out->kind = GTEXT_CSSOM_UNCHANGED;   /* CSSOM: "Let m be the result of parsing the given value. If m is null, then return." (none, or more than one, query) */
  }
  else if (st == GTEXT_CSS_OK) {
    const css_media_read * have = owner_read(list->owner);
    const span mine = item_read(one_read, 0, (span){one.data, one.len});
    size_t count = 0;
    span * all = items_of(core, &now, &count, &st);
    size_t kept = 0;
    for (size_t i = 0; st == GTEXT_CSS_OK && i < count; i++) {
      kept += !span_eq(item_read(have, i, all[i]), mine);
    }
    if (st == GTEXT_CSS_OK && kept == count) {
      st = GTEXT_CSS_E_NOT_FOUND;   /* "If nothing was removed, then throw a NotFoundError exception." */
    }
    css_media_read * nr = NULL;
    if (st == GTEXT_CSS_OK && have) {
      nr = read_new(core, kept);
      st = nr ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
    }
    for (size_t i = 0, k = 0; st == GTEXT_CSS_OK && i < count; i++) {
      if (span_eq(item_read(have, i, all[i]), mine)) {
        continue;
      }
      if ((next.len && !tb_puts(&next, ", ")) || !tb_put(&next, all[i].data, all[i].length)) {
        st = GTEXT_CSS_E_OOM;
      }
      if (st == GTEXT_CSS_OK && nr && have && i < have->count && have->text[i] && !read_set(core, nr, k, have->text[i], strlen(have->text[i]))) {
        st = GTEXT_CSS_E_OOM;
      }
      k++;
    }
    if (st == GTEXT_CSS_OK && next.oom) {
      st = GTEXT_CSS_E_OOM;
    }
    gtext_allocator_free(&core->alloc, all);
    if (st == GTEXT_CSS_OK) {
      st = store(list, next.data ? next.data : "", next.len, nr, out);
    }
    else {
      css_media_read_free(core, nr);
    }
  }
  css_media_read_free(core, one_read);
  tb_free(&one);
  tb_free(&now);
  tb_free(&next);
  return st;
}

GTEXT_CSS_Status css_media_canonical_read(css_core * core, const char * text, size_t length, tbuf * out, css_media_read ** read) {
  size_t queries = 0;
  const GTEXT_CSS_Status st = canonical(core, text, length, out, &queries, NULL, read);
  if (st == GTEXT_CSS_OK && out->oom) {
    if (read) {
      css_media_read_free(core, *read);
      *read = NULL;
    }
    return GTEXT_CSS_E_OOM;
  }
  return st;
}

GTEXT_CSS_Status css_media_canonical(css_core * core, const char * text, size_t length, tbuf * out) {
  return css_media_canonical_read(core, text, length, out, NULL);
}
