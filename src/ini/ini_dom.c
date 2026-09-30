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
 * @file ini_dom.c
 * @brief Building, walking and releasing the INI document tree.
 */

#include "ini_internal.h"

/** Hand back a counted string as a pointer plus an optional length. */
static const char * ini_out(const ini_str * s, size_t * len) {
  if (len) *len = s ? s->len : 0;
  return s ? s->data : NULL;
}

static char ini_lower_byte(char c) {
  return (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c;
}

/**
 * Whether a stored key is the one a caller asked for.
 *
 * Compares against the folded form the parse stored, so a lookup folds only the
 * query and never allocates - which is why the tree carries `canon` at all. A
 * dialect that does not fold has no `canon` and compares the spelling directly.
 */
static bool ini_key_matches(const GTEXT_INI_Dialect * dialect,
    const ini_entry * entry, const char * query, size_t query_len) {
  const ini_str * name = entry->canon.data ? &entry->canon : &entry->key;
  if (name->len != query_len) return false;
  if (!dialect->fold_case) return memcmp(name->data, query, query_len) == 0;
  for (size_t i = 0; i < query_len; i++) {
    if (name->data[i] != ini_lower_byte(query[i])) return false;
  }
  return true;
}

/**
 * Whether a stored group is the one a caller asked for.
 *
 * **The section part folds and the subsection part does not**, which is git's
 * rule for a name on its command line and is measured rather than inferred:
 * over a file holding `[a.SubB]` and `[b "SubB"]`, git answers `a.subb.k` and
 * `b.SubB.k` and refuses `a.SubB.k` and `b.subb.k`. So the dotted header
 * spelling is reachable only by its folded name and the quoted one only by its
 * exact name - the two are not interchangeable, and a lookup that folded the
 * whole query would find the first and lose the second.
 */
static bool ini_group_matches(const GTEXT_INI_Dialect * dialect,
    const GTEXT_INI_Group * group, const char * query, size_t query_len) {
  const ini_str * name = group->canon.data ? &group->canon : &group->name;
  if (name->len != query_len) return false;
  /* gtext_ini_group_names_fold(), not `fold_case`: EditorConfig folds keys and
   * compares section names byte for byte. */
  if (!gtext_ini_group_names_fold(dialect)) {
    return query_len == 0 || memcmp(name->data, query, query_len) == 0;
  }
  size_t dot = 0;
  while (dot < query_len && query[dot] != '.') dot++;
  for (size_t i = 0; i < dot; i++) {
    if (name->data[i] != ini_lower_byte(query[i])) return false;
  }
  if (dot == query_len) return true;
  return memcmp(name->data + dot, query + dot, query_len - dot) == 0;
}

/** The index of the n-th entry carrying @p key, or the count when absent. */
static size_t ini_find_nth(const GTEXT_INI_Group * group, const char * key,
    size_t key_len, size_t n) {
  size_t seen = 0;
  for (size_t e = 0; e < group->count; e++) {
    if (!ini_key_matches(&group->doc->dialect, &group->entries[e], key,
            key_len)) {
      continue;
    }
    if (seen == n) return e;
    seen++;
  }
  return group->count;
}

/** Whether a dialect's single-value lookup answers the last occurrence. */
static bool ini_wants_last(const GTEXT_INI_Dialect * dialect) {
  /* COLLECT joins LAST_WINS here because `git config --get` answers the last of
   * several - measured. The two modes differ in what they promise about the
   * earlier occurrences, not in which one a single-value read selects. */
  return dialect->dupkey == GTEXT_INI_DUPKEY_LAST_WINS ||
         dialect->dupkey == GTEXT_INI_DUPKEY_COLLECT;
}

GTEXT_INI_Document * gtext_ini_new(const GTEXT_INI_Parse_Options * opts) {
  GTEXT_INI_Parse_Options effective = opts ? *opts
                                          : gtext_ini_parse_options_default();
  const GTEXT_Allocator * alloc = effective.allocator
                                      ? effective.allocator
                                      : gtext_allocator_default();
  GTEXT_INI_Document * doc = gtext_allocator_calloc(alloc, 1, sizeof(*doc));
  if (!doc) return NULL;
  doc->alloc = alloc;
  doc->dialect = effective.dialect;
  doc->synthesized = true;
  return doc;
}

void gtext_ini_free(GTEXT_INI_Document * doc) {
  if (!doc) return;
  const GTEXT_Allocator * alloc = doc->alloc;
  for (size_t g = 0; g < doc->count; g++) {
    GTEXT_INI_Group * group = &doc->groups[g];
    for (size_t e = 0; e < group->count; e++) {
      ini_entry * entry = &group->entries[e];
      gtext_ini_str_clear(alloc, &entry->key);
      gtext_ini_str_clear(alloc, &entry->canon);
      gtext_ini_str_clear(alloc, &entry->value);
      gtext_ini_str_clear(alloc, &entry->pre);
      gtext_ini_str_clear(alloc, &entry->sep);
      gtext_ini_str_clear(alloc, &entry->eol);
      gtext_ini_str_clear(alloc, &entry->comment);
    }
    if (group->entries) gtext_allocator_free(alloc, group->entries);
    gtext_ini_str_clear(alloc, &group->name);
    gtext_ini_str_clear(alloc, &group->canon);
    gtext_ini_str_clear(alloc, &group->comment);
    gtext_ini_str_clear(alloc, &group->hdr_pre);
    gtext_ini_str_clear(alloc, &group->hdr_post);
  }
  if (doc->groups) gtext_allocator_free(alloc, doc->groups);
  gtext_ini_str_clear(alloc, &doc->bom);
  gtext_ini_str_clear(alloc, &doc->leading);
  gtext_ini_str_clear(alloc, &doc->trailing);
  gtext_allocator_free(alloc, doc);
}

size_t gtext_ini_document_group_count(const GTEXT_INI_Document * doc) {
  return doc ? doc->count : 0;
}

const GTEXT_INI_Group * gtext_ini_document_group_at(
    const GTEXT_INI_Document * doc, size_t index) {
  if (!doc || index >= doc->count) return NULL;
  return &doc->groups[index];
}

const GTEXT_INI_Group * gtext_ini_document_group(
    const GTEXT_INI_Document * doc, const char * name) {
  if (!doc || !name) return NULL;
  size_t len = strlen(name);
  for (size_t g = 0; g < doc->count; g++) {
    if (ini_group_matches(&doc->dialect, &doc->groups[g], name, len)) {
      return &doc->groups[g];
    }
  }
  return NULL;
}

const char * gtext_ini_document_get(const GTEXT_INI_Document * doc,
    const char * group, const char * key, size_t * len) {
  if (len) *len = 0;
  if (!doc || !group || !key) return NULL;
  size_t glen = strlen(group);
  size_t klen = strlen(key);
  const ini_entry * found = NULL;
  /*
   * Every group of that name, or only the first, which is
   * ::GTEXT_INI_Dialect::merge_duplicate_groups. GLib merges repeated group
   * headers, so a key under the second `[G]` is found under `G`; the Win32 profile
   * API does not, and a key in a second `[a]` is retrievable by no name at all
   * while `GetPrivateProfileSectionNames` still lists that section.
   *
   * The tree keeps the groups apart either way, so that a rewrite reproduces the
   * document; the merge, or its absence, lives here in the lookup where it costs
   * the document nothing.
   */
  for (size_t g = 0; g < doc->count; g++) {
    const GTEXT_INI_Group * grp = &doc->groups[g];
    if (!ini_group_matches(&doc->dialect, grp, group, glen)) continue;
    for (size_t e = 0; e < grp->count; e++) {
      const ini_entry * entry = &grp->entries[e];
      if (!ini_key_matches(&doc->dialect, entry, key, klen)) continue;
      if (ini_wants_last(&doc->dialect) || !found) found = entry;
    }
    if (!doc->dialect.merge_duplicate_groups) break;
  }
  return ini_out(found ? &found->value : NULL, len);
}

GTEXT_INI_Status gtext_ini_document_add_group(GTEXT_INI_Document * doc,
    const char * name, GTEXT_INI_Group ** out) {
  if (out) *out = NULL;
  if (!doc || !name) return GTEXT_INI_E_INVALID;
  size_t len = strlen(name);
  if (!gtext_ini_group_name_ok(&doc->dialect, name, len)) {
    return GTEXT_INI_E_BAD_GROUP;
  }
  if (!doc->dialect.allow_duplicate_groups &&
      gtext_ini_document_group(doc, name)) {
    return GTEXT_INI_E_DUPGROUP;
  }
  if (doc->count == doc->capacity) {
    size_t want = doc->capacity ? doc->capacity * 2 : 4;
    GTEXT_INI_Group * grown = gtext_allocator_realloc(
        doc->alloc, doc->groups, want * sizeof(*grown));
    if (!grown) return GTEXT_INI_E_OOM;
    doc->groups = grown;
    doc->capacity = want;
    /* The array moved, so every group's back-pointer to the document is still
     * right - the document did not move - but a caller's GTEXT_INI_Group* is
     * not. The header says so. */
    for (size_t g = 0; g < doc->count; g++) doc->groups[g].doc = doc;
  }
  GTEXT_INI_Group * group = &doc->groups[doc->count];
  memset(group, 0, sizeof(*group));
  group->doc = doc;
  if (!gtext_ini_str_set(doc->alloc, &group->name, name, len)) {
    return GTEXT_INI_E_OOM;
  }
  /*
   * @p name is the *header* spelling, so for git a subsection comes in as
   * `remote "origin"` rather than as `remote.origin`, and the canonical form is
   * derived from it exactly as the parser derives it. Taking the canonical form
   * as the argument instead would make a caller unable to say which of the two
   * subsection spellings to write.
   */
  /* gtext_ini_group_names_fold(), not `fold_case`: EditorConfig folds keys and
   * not section names, so gtext_ini_canon_group() has nothing to produce for one
   * and returning false is the right answer rather than a failure. Asking
   * `fold_case` here turned every add_group() under that dialect into
   * GTEXT_INI_E_OOM. */
  if (gtext_ini_group_names_fold(&doc->dialect) ||
      doc->dialect.subsection_syntax) {
    char stack[512];
    char * buf = stack;
    char * heap = NULL;
    if (len > sizeof(stack)) {
      heap = gtext_allocator_malloc(doc->alloc, len);
      if (!heap) return GTEXT_INI_E_OOM;
      buf = heap;
    }
    size_t canon_len = 0;
    bool ok = gtext_ini_canon_group(&doc->dialect, name, len, buf, &canon_len) &&
              gtext_ini_str_set(doc->alloc, &group->canon, buf, canon_len);
    if (heap) gtext_allocator_free(doc->alloc, heap);
    if (!ok) {
      gtext_ini_str_clear(doc->alloc, &group->name);
      return GTEXT_INI_E_OOM;
    }
  }
  doc->count++;
  if (out) *out = group;
  return GTEXT_INI_OK;
}

size_t gtext_ini_group_find(const GTEXT_INI_Group * group, const char * key,
    size_t n) {
  if (!group || !key) return SIZE_MAX;
  size_t index = ini_find_nth(group, key, strlen(key), n);
  return index >= group->count ? SIZE_MAX : index;
}

bool gtext_ini_group_value_present_at(const GTEXT_INI_Group * group,
    size_t index) {
  if (!group || index >= group->count) return false;
  return group->entries[index].has_value;
}

const char * gtext_ini_group_canonical_name(const GTEXT_INI_Group * group,
    size_t * len) {
  if (!group) {
    if (len) *len = 0;
    return NULL;
  }
  return ini_out(group->canon.data ? &group->canon : &group->name, len);
}

const char * gtext_ini_group_canonical_key_at(const GTEXT_INI_Group * group,
    size_t index, size_t * len) {
  if (!group || index >= group->count) {
    if (len) *len = 0;
    return NULL;
  }
  const ini_entry * entry = &group->entries[index];
  return ini_out(entry->canon.data ? &entry->canon : &entry->key, len);
}

bool gtext_ini_group_is_preamble(const GTEXT_INI_Group * group) {
  return group ? group->preamble : false;
}

const char * gtext_ini_document_leading_comment(
    const GTEXT_INI_Document * doc, size_t * len) {
  return ini_out(doc ? &doc->leading : NULL, len);
}

const char * gtext_ini_document_trailing_comment(
    const GTEXT_INI_Document * doc, size_t * len) {
  return ini_out(doc ? &doc->trailing : NULL, len);
}

const char * gtext_ini_group_name(const GTEXT_INI_Group * group, size_t * len) {
  return ini_out(group ? &group->name : NULL, len);
}

size_t gtext_ini_group_entry_count(const GTEXT_INI_Group * group) {
  return group ? group->count : 0;
}

const char * gtext_ini_group_key_at(const GTEXT_INI_Group * group,
    size_t index, size_t * len) {
  if (!group || index >= group->count) {
    if (len) *len = 0;
    return NULL;
  }
  return ini_out(&group->entries[index].key, len);
}

const char * gtext_ini_group_value_at(const GTEXT_INI_Group * group,
    size_t index, size_t * len) {
  if (!group || index >= group->count) {
    if (len) *len = 0;
    return NULL;
  }
  return ini_out(&group->entries[index].value, len);
}

const char * gtext_ini_group_get(const GTEXT_INI_Group * group,
    const char * key, size_t * len) {
  if (len) *len = 0;
  if (!group || !key) return NULL;
  size_t key_len = strlen(key);
  size_t count = gtext_ini_group_count_key(group, key);
  if (!count) return NULL;
  size_t want = ini_wants_last(&group->doc->dialect) ? count - 1 : 0;
  size_t index = ini_find_nth(group, key, key_len, want);
  if (index >= group->count) return NULL;
  return ini_out(&group->entries[index].value, len);
}

size_t gtext_ini_group_count_key(const GTEXT_INI_Group * group,
    const char * key) {
  if (!group || !key) return 0;
  size_t key_len = strlen(key);
  size_t count = 0;
  for (size_t e = 0; e < group->count; e++) {
    if (ini_key_matches(&group->doc->dialect, &group->entries[e], key,
            key_len)) {
      count++;
    }
  }
  return count;
}

const char * gtext_ini_group_get_nth(const GTEXT_INI_Group * group,
    const char * key, size_t n, size_t * len) {
  if (len) *len = 0;
  if (!group || !key) return NULL;
  size_t index = ini_find_nth(group, key, strlen(key), n);
  if (index >= group->count) return NULL;
  return ini_out(&group->entries[index].value, len);
}

/** Fill in a fresh entry's key, value and synthesized punctuation. */
static GTEXT_INI_Status ini_entry_init(GTEXT_INI_Group * group,
    ini_entry * entry, const char * key, size_t key_len, const char * value,
    size_t value_len) {
  const GTEXT_Allocator * alloc = group->doc->alloc;
  const GTEXT_INI_Dialect * d = &group->doc->dialect;
  /* Not verbatim: `pre`, `sep` and `eol` below are this function's invention, so
   * a non-normalizing write must synthesize the line rather than claim to be
   * reproducing one. */
  entry->verbatim = false;
  entry->has_value = true;
  bool ok = gtext_ini_str_set(alloc, &entry->key, key, key_len) &&
            gtext_ini_str_set(alloc, &entry->value, value ? value : "",
                value_len) &&
            gtext_ini_str_set(alloc, &entry->pre, "", 0) &&
            gtext_ini_str_set(alloc, &entry->sep, d->separators, 1) &&
            gtext_ini_str_set(alloc, &entry->eol, "\n", 1);
  if (ok && d->fold_case) {
    /* The folded form a lookup will compare against. Built here rather than at
     * lookup time for the same reason the parser builds it. */
    char stack[256];
    char * buf = stack;
    char * heap = NULL;
    if (key_len > sizeof(stack)) {
      heap = gtext_allocator_malloc(alloc, key_len);
      if (!heap) return GTEXT_INI_E_OOM;
      buf = heap;
    }
    size_t canon_len = 0;
    ok = gtext_ini_canon_key(d, key, key_len, buf, &canon_len) &&
         gtext_ini_str_set(alloc, &entry->canon, buf, canon_len);
    if (heap) gtext_allocator_free(alloc, heap);
  }
  return ok ? GTEXT_INI_OK : GTEXT_INI_E_OOM;
}

GTEXT_INI_Status gtext_ini_group_set(GTEXT_INI_Group * group, const char * key,
    const char * value, size_t value_len) {
  if (!group || !key || (!value && value_len)) return GTEXT_INI_E_INVALID;
  size_t key_len = strlen(key);
  if (!gtext_ini_key_ok(&group->doc->dialect, key, key_len)) {
    return GTEXT_INI_E_BAD_KEY;
  }
  size_t index = ini_find_nth(group, key, key_len, 0);
  if (index < group->count) {
    /* Replace in place, keeping the original punctuation so that changing one
     * value in a file does not reformat the line it was on. */
    if (!gtext_ini_str_set(group->doc->alloc, &group->entries[index].value,
            value ? value : "", value_len)) {
      return GTEXT_INI_E_OOM;
    }
    return GTEXT_INI_OK;
  }
  ini_entry * entry = gtext_ini_group_push(group);
  if (!entry) return GTEXT_INI_E_OOM;
  GTEXT_INI_Status status =
      ini_entry_init(group, entry, key, key_len, value, value_len);
  if (status != GTEXT_INI_OK) group->count--;
  return status;
}

GTEXT_INI_Status gtext_ini_group_add(GTEXT_INI_Group * group, const char * key,
    const char * value, size_t value_len) {
  if (!group || !key || (!value && value_len)) return GTEXT_INI_E_INVALID;
  size_t key_len = strlen(key);
  if (!gtext_ini_key_ok(&group->doc->dialect, key, key_len)) {
    return GTEXT_INI_E_BAD_KEY;
  }
  if (group->doc->dialect.dupkey == GTEXT_INI_DUPKEY_ERROR &&
      gtext_ini_group_count_key(group, key)) {
    return GTEXT_INI_E_DUPKEY;
  }
  ini_entry * entry = gtext_ini_group_push(group);
  if (!entry) return GTEXT_INI_E_OOM;
  GTEXT_INI_Status status =
      ini_entry_init(group, entry, key, key_len, value, value_len);
  if (status != GTEXT_INI_OK) group->count--;
  return status;
}

const char * gtext_ini_group_leading_comment(const GTEXT_INI_Group * group,
    size_t * len) {
  return ini_out(group ? &group->comment : NULL, len);
}

const char * gtext_ini_group_entry_comment_at(const GTEXT_INI_Group * group,
    size_t index, size_t * len) {
  if (!group || index >= group->count) {
    if (len) *len = 0;
    return NULL;
  }
  return ini_out(&group->entries[index].comment, len);
}
