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
 * @file ini_parser.c
 * @brief The line-oriented INI reader.
 *
 * **The parser decodes nothing.** It finds the lines, classifies each one, and
 * stores the bytes. Escapes are not expanded, lists are not split, and - the
 * one that is easiest to get wrong - **UTF-8 is not validated**, because
 * neither reference validates it here. `g_key_file_load_from_data()` accepts a
 * value containing a bare 0xFF and only `g_key_file_get_string()` refuses it;
 * `desktop-file-validate` exits 0 with a warning that says "There is no
 * guarantee the validator will correctly work". Desktop Entry §3.1 also says a
 * comment line "may contain any character (except for LF)", so a document with
 * a non-UTF-8 comment and clean values is legal and must parse. The validation
 * is in ini_value.c, where a caller has asked a question that depends on it.
 *
 * Line classification, in the order the tests below apply it, each rule
 * measured against `GKeyFile` rather than assumed:
 *
 *   - empty, or nothing but spaces and tabs, is a blank line;
 *   - a comment introducer at the start of the content is a comment;
 *   - `[` starts a group header, which must be `[name]` followed by nothing
 *     but whitespace - `[G]   ` is a group and `[G] junk` is an error;
 *   - anything else must contain `=`, and is an entry;
 *   - anything else at all is GTEXT_INI_E_BAD_LINE.
 */

#include "ini_internal.h"

/** A growable byte buffer for accumulating a run of comment lines. */
typedef struct {
  char * data;
  size_t len;
  size_t capacity;
} ini_buf;

static bool ini_buf_append(const GTEXT_Allocator * alloc, ini_buf * buf,
    const char * bytes, size_t len) {
  if (!len) return true;
  if (buf->len + len > buf->capacity) {
    size_t want = buf->capacity ? buf->capacity * 2 : 128;
    while (want < buf->len + len) want *= 2;
    char * grown = gtext_allocator_realloc(alloc, buf->data, want);
    if (!grown) return false;
    buf->data = grown;
    buf->capacity = want;
  }
  memcpy(buf->data + buf->len, bytes, len);
  buf->len += len;
  return true;
}

/** Move the accumulated comment run into @p out, emptying the buffer. */
static bool ini_buf_take(const GTEXT_Allocator * alloc, ini_buf * buf,
    ini_str * out) {
  if (!buf->len) return true;
  if (!gtext_ini_str_set(alloc, out, buf->data, buf->len)) return false;
  buf->len = 0;
  return true;
}

static bool ini_is_space(char c) { return c == ' ' || c == '\t'; }

/** Whether every byte of [start, end) is a space or a tab. */
static bool ini_all_space(const char * bytes, size_t start, size_t end) {
  for (size_t i = start; i < end; i++) {
    if (!ini_is_space(bytes[i])) return false;
  }
  return true;
}

/** State threaded through one parse. */
typedef struct {
  const char * bytes;
  size_t len;
  const GTEXT_Allocator * alloc;
  const GTEXT_INI_Parse_Options * opts;
  GTEXT_INI_Document * doc;
  GTEXT_INI_Error * err;
  ini_buf pending; /* Comment and blank lines not yet attached to anything. */
  size_t group_index; /* Into doc->groups; SIZE_MAX before the first header. */
} ini_parse;

#define INI_NO_GROUP ((size_t) -1)

/** Record a failure and return false, for the one-line `return ini_fail(...)`. */
static bool ini_fail(ini_parse * p, GTEXT_INI_Status code,
    const char * message, size_t offset) {
  gtext_ini_set_error(p->err, code, message, p->bytes, p->len, offset);
  return false;
}

/** Add a group header to the document, attaching any pending comments. */
static bool ini_add_group(ini_parse * p, size_t name_start, size_t name_end,
    size_t pre_start, size_t pre_end, size_t post_start, size_t post_end,
    size_t line_start) {
  const char * name = p->bytes + name_start;
  size_t name_len = name_end - name_start;
  if (!gtext_ini_group_name_ok(&p->doc->dialect, name, name_len)) {
    return ini_fail(p, GTEXT_INI_E_BAD_GROUP,
        "a group name may not be empty or contain '[', ']' or a control "
        "character",
        name_start);
  }
  if (!p->doc->dialect.allow_duplicate_groups) {
    for (size_t g = 0; g < p->doc->count; g++) {
      if (p->doc->groups[g].name.len == name_len &&
          memcmp(p->doc->groups[g].name.data, name, name_len) == 0) {
        return ini_fail(p, GTEXT_INI_E_DUPGROUP,
            "two groups have the same name", line_start);
      }
    }
  }
  if (p->opts->max_groups && p->doc->count >= p->opts->max_groups) {
    return ini_fail(p, GTEXT_INI_E_LIMIT, "too many groups", line_start);
  }
  if (p->doc->count == p->doc->capacity) {
    size_t want = p->doc->capacity ? p->doc->capacity * 2 : 8;
    GTEXT_INI_Group * grown = gtext_allocator_realloc(
        p->alloc, p->doc->groups, want * sizeof(*grown));
    if (!grown) return ini_fail(p, GTEXT_INI_E_OOM, "out of memory", line_start);
    p->doc->groups = grown;
    p->doc->capacity = want;
    for (size_t g = 0; g < p->doc->count; g++) p->doc->groups[g].doc = p->doc;
  }
  GTEXT_INI_Group * group = &p->doc->groups[p->doc->count];
  memset(group, 0, sizeof(*group));
  group->doc = p->doc;
  if (!gtext_ini_str_set(p->alloc, &group->name, name, name_len) ||
      !gtext_ini_str_set(p->alloc, &group->hdr_pre, p->bytes + pre_start,
          pre_end - pre_start) ||
      !gtext_ini_str_set(p->alloc, &group->hdr_post, p->bytes + post_start,
          post_end - post_start)) {
    return ini_fail(p, GTEXT_INI_E_OOM, "out of memory", line_start);
  }
  if (!ini_buf_take(p->alloc, &p->pending, &group->comment)) {
    return ini_fail(p, GTEXT_INI_E_OOM, "out of memory", line_start);
  }
  p->group_index = p->doc->count;
  p->doc->count++;
  return true;
}

/** Add an entry to the current group. */
static bool ini_add_entry(ini_parse * p, size_t line_start, size_t pre_end,
    size_t eq, size_t value_start, size_t value_end, size_t line_end) {
  if (p->group_index == INI_NO_GROUP) {
    /*
     * A preamble entry needs somewhere to live, and inventing an unnamed group
     * would put a group in the tree the document does not have. The dialects
     * that allow a preamble are the ones this module does not implement yet, so
     * the honest answer is to say what is missing rather than to guess a shape
     * for it.
     */
    return ini_fail(p, GTEXT_INI_E_NO_GROUP,
        "an entry appeared before the first group header", line_start);
  }
  GTEXT_INI_Group * group = &p->doc->groups[p->group_index];
  size_t key_end = eq;
  while (key_end > pre_end && ini_is_space(p->bytes[key_end - 1])) key_end--;
  size_t key_len = key_end - pre_end;
  if (!gtext_ini_key_ok(&p->doc->dialect, p->bytes + pre_end, key_len)) {
    return ini_fail(p, GTEXT_INI_E_BAD_KEY,
        "a key name is empty or uses a character the dialect does not allow",
        pre_end);
  }
  if (p->doc->dialect.dupkey == GTEXT_INI_DUPKEY_ERROR) {
    for (size_t e = 0; e < group->count; e++) {
      if (group->entries[e].key.len == key_len &&
          memcmp(group->entries[e].key.data, p->bytes + pre_end, key_len) ==
              0) {
        return ini_fail(p, GTEXT_INI_E_DUPKEY,
            "two keys in one group have the same name", line_start);
      }
    }
  }
  if (p->opts->max_entries_per_group &&
      group->count >= p->opts->max_entries_per_group) {
    return ini_fail(p, GTEXT_INI_E_LIMIT, "too many entries in one group",
        line_start);
  }
  ini_entry * entry = gtext_ini_group_push(group);
  if (!entry) return ini_fail(p, GTEXT_INI_E_OOM, "out of memory", line_start);
  if (!gtext_ini_str_set(p->alloc, &entry->key, p->bytes + pre_end, key_len) ||
      !gtext_ini_str_set(p->alloc, &entry->value, p->bytes + value_start,
          value_end - value_start) ||
      !gtext_ini_str_set(p->alloc, &entry->pre, p->bytes + line_start,
          pre_end - line_start) ||
      !gtext_ini_str_set(p->alloc, &entry->sep, p->bytes + key_end,
          value_start - key_end) ||
      !gtext_ini_str_set(p->alloc, &entry->eol, p->bytes + value_end,
          line_end - value_end)) {
    return ini_fail(p, GTEXT_INI_E_OOM, "out of memory", line_start);
  }
  if (!ini_buf_take(p->alloc, &p->pending, &entry->comment)) {
    return ini_fail(p, GTEXT_INI_E_OOM, "out of memory", line_start);
  }
  return true;
}

/**
 * Desktop Entry §5: a postfixed key requires its bare form in the same group.
 *
 * Checked after the whole document is read rather than as each entry arrives,
 * because the specification does not say the bare key comes first - `GKeyFile`
 * accepts either order, and so does `desktop-file-validate`.
 */
static bool ini_check_locale_keys(ini_parse * p) {
  for (size_t g = 0; g < p->doc->count; g++) {
    GTEXT_INI_Group * group = &p->doc->groups[g];
    for (size_t e = 0; e < group->count; e++) {
      const ini_str * key = &group->entries[e].key;
      size_t base = gtext_ini_key_base_len(key->data, key->len);
      if (base == key->len) continue;
      bool found = false;
      for (size_t o = 0; o < group->count && !found; o++) {
        const ini_str * other = &group->entries[o].key;
        found = other->len == base && memcmp(other->data, key->data, base) == 0;
      }
      if (!found) {
        return ini_fail(p, GTEXT_INI_E_BAD_KEY,
            "a localized key has no unlocalized key of the same name", 0);
      }
    }
  }
  return true;
}

GTEXT_INI_Document * gtext_ini_parse(const char * bytes, size_t len,
    const GTEXT_INI_Parse_Options * opts, GTEXT_INI_Error * err) {
  if (err) memset(err, 0, sizeof(*err));
  if (!bytes && len) {
    gtext_ini_set_error(err, GTEXT_INI_E_INVALID, "bytes is NULL", NULL, 0, 0);
    return NULL;
  }
  GTEXT_INI_Parse_Options effective = opts ? *opts
                                          : gtext_ini_parse_options_default();
  if (effective.max_total_bytes && len > effective.max_total_bytes) {
    gtext_ini_set_error(err, GTEXT_INI_E_LIMIT, "input exceeds max_total_bytes",
        bytes, len, 0);
    return NULL;
  }
  const GTEXT_Allocator * alloc = effective.allocator
                                      ? effective.allocator
                                      : gtext_allocator_default();
  GTEXT_INI_Document * doc = gtext_allocator_calloc(alloc, 1, sizeof(*doc));
  if (!doc) {
    gtext_ini_set_error(err, GTEXT_INI_E_OOM, "out of memory", bytes, len, 0);
    return NULL;
  }
  doc->alloc = alloc;
  doc->dialect = effective.dialect;

  ini_parse p;
  memset(&p, 0, sizeof(p));
  p.bytes = bytes;
  p.len = len;
  p.alloc = alloc;
  p.opts = &effective;
  p.doc = doc;
  p.err = err;
  p.group_index = INI_NO_GROUP;

  size_t offset = 0;
  if (effective.dialect.skip_bom && len >= 3 &&
      (unsigned char) bytes[0] == 0xEF && (unsigned char) bytes[1] == 0xBB &&
      (unsigned char) bytes[2] == 0xBF) {
    offset = 3;
  }

  bool ok = true;
  while (ok && offset < len) {
    /* The line's bytes, and the bytes of the line including its terminator. */
    size_t content_end = offset;
    while (content_end < len && bytes[content_end] != '\n') content_end++;
    bool has_lf = content_end < len;
    size_t line_end = has_lf ? content_end + 1 : content_end;
    /*
     * A CR is part of the terminator only when an LF follows it. Without the
     * `has_lf` test a trailing CR at end of input was stripped as though it
     * were one, so the same bytes gave the generic dialect a value one byte
     * shorter than the strict dialect's - which is what fuzz_ini.cpp's parity
     * property is for, and what it found on its first run at 237k executions.
     * Only the last CR before the LF is a terminator; any before that are data.
     */
    if (effective.dialect.accept_crlf && has_lf && content_end > offset &&
        bytes[content_end - 1] == '\r') {
      content_end--;
    }

    size_t pre_end = offset;
    if (effective.dialect.allow_leading_whitespace) {
      while (pre_end < content_end && ini_is_space(bytes[pre_end])) pre_end++;
    }

    bool blank = ini_all_space(bytes, offset, content_end);
    bool comment = false;
    if (!blank) {
      char first = bytes[pre_end];
      comment = (first == '#' && effective.dialect.comment_hash) ||
                (first == ';' && effective.dialect.comment_semicolon);
    }

    if (blank || comment) {
      /*
       * A blank line is a comment (§3.1), and both are kept verbatim -
       * terminator included - so that writing them back reproduces the input's
       * bytes rather than a reformatting of them.
       */
      if (effective.retain_comments &&
          !ini_buf_append(alloc, &p.pending, bytes + offset,
              line_end - offset)) {
        ok = ini_fail(&p, GTEXT_INI_E_OOM, "out of memory", offset);
      }
    }
    else if (bytes[pre_end] == '[') {
      size_t close = pre_end + 1;
      while (close < content_end && bytes[close] != ']') close++;
      if (close >= content_end) {
        ok = ini_fail(&p, GTEXT_INI_E_BAD_GROUP,
            "a group header is missing its ']'", offset);
      }
      else if (!ini_all_space(bytes, close + 1, content_end)) {
        /* `[G] junk` - GKeyFile refuses this, and so must anything claiming to
         * read the same documents. */
        ok = ini_fail(&p, GTEXT_INI_E_BAD_LINE,
            "a group header must be followed by nothing but whitespace",
            close + 1);
      }
      else {
        ok = ini_add_group(&p, pre_end + 1, close, offset, pre_end, close + 1,
            line_end, offset);
      }
    }
    else {
      size_t eq = pre_end;
      while (eq < content_end && bytes[eq] != '=') eq++;
      if (eq >= content_end) {
        ok = ini_fail(&p, GTEXT_INI_E_BAD_LINE,
            "a line is not blank, a comment, a group header or an entry",
            offset);
      }
      else {
        size_t value_start = eq + 1;
        /* §3.3: "Space before and after the equals sign should be ignored."
         * The leading run goes; the trailing run is part of the value unless
         * the dialect says otherwise, which is what `GKeyFile` does. */
        while (value_start < content_end && ini_is_space(bytes[value_start])) {
          value_start++;
        }
        size_t value_end = content_end;
        if (effective.dialect.trim_trailing_space) {
          while (value_end > value_start && ini_is_space(bytes[value_end - 1])) {
            value_end--;
          }
        }
        ok = ini_add_entry(&p, offset, pre_end, eq, value_start, value_end,
            line_end);
      }
    }
    offset = line_end;
  }

  if (ok && p.pending.len) {
    /* Trailing comments belong to the document: there is no next line for them
     * to lead, and dropping them would fail §3's preservation requirement. */
    ini_str * where = p.doc->count ? &doc->trailing : &doc->leading;
    ok = ini_buf_take(alloc, &p.pending, where) ||
         ini_fail(&p, GTEXT_INI_E_OOM, "out of memory", len);
  }
  /*
   * The leading block is the pending run at the moment the first group header
   * arrives, which ini_add_group() attached to that group. Move it to the
   * document, because it precedes the header rather than belonging to it, and a
   * writer emitting the group's comment and then the document's would otherwise
   * emit it twice.
   */
  if (ok && doc->count && doc->groups[0].comment.data && !doc->leading.data) {
    doc->leading = doc->groups[0].comment;
    doc->groups[0].comment.data = NULL;
    doc->groups[0].comment.len = 0;
  }
  if (ok && effective.dialect.require_unlocalized_key &&
      effective.dialect.locale_postfix) {
    ok = ini_check_locale_keys(&p);
  }

  if (p.pending.data) gtext_allocator_free(alloc, p.pending.data);
  if (!ok) {
    gtext_ini_free(doc);
    return NULL;
  }
  return doc;
}
