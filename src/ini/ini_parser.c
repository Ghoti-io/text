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
 * measured against a reference rather than assumed:
 *
 *   - empty, or nothing but the dialect's whitespace, is a blank line;
 *   - a comment introducer at the start of the content is a comment;
 *   - `[` starts a group header, which must be `[name]` followed by nothing but
 *     whitespace unless ::GTEXT_INI_Dialect::header_remainder_is_entry, in
 *     which case what follows the `]` is an entry on the same line;
 *   - anything else is an entry;
 *   - anything else at all is GTEXT_INI_E_BAD_LINE.
 *
 * **A logical line is not always a physical one.** Under
 * ::GTEXT_INI_CONTINUATION_JOIN_EMPTY a value may span several, and a value's
 * extent is then decided by gtext_ini_scan_value() rather than by the next LF -
 * so the loop below advances by what the scanner consumed, not by the line it
 * started on. One scanner serves both the parse and gtext_ini_unescape(),
 * because two implementations of git's quoting would disagree about some value
 * while both kept working.
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

/**
 * A space or a tab, and nothing else, whatever the dialect.
 *
 * Distinct from gtext_ini_is_space() on purpose, and the distinction is git's:
 * its top-level loop skips anything its ctype calls space - CR included - while
 * the run between a key and its `=` is matched against `' '` and `'\t'`
 * literally. So `\rk = v` is accepted and `k\r= v` is refused, by the same
 * implementation, on the same line. One predicate for both would have to pick
 * one of those two answers and would get the other wrong.
 */
static bool ini_is_blank(char c) { return c == ' ' || c == '\t'; }

/**
 * The whitespace that may sit between a key and its `=`.
 *
 * Two answers, and which one applies is the dialect's. git's is the literal pair
 * above - `k\r= v` is refused by git while `\rk = v` is accepted, by the same
 * implementation on the same line - so the separator run cannot simply use
 * gtext_ini_is_space(). EditorConfig has no such split: it trims the whole line
 * with one predicate before classifying it, so `k\v= v` is `k` = `v`.
 *
 * Keyed on ::GTEXT_INI_Dialect::ctype_whitespace rather than on an id, and the
 * effect for Desktop Entry is nil either way: its whitespace is exactly space and
 * tab already.
 */
static bool ini_sep_space(const GTEXT_INI_Dialect * dialect, char c) {
  return dialect->ctype_whitespace ? gtext_ini_is_space(dialect, c)
                                   : ini_is_blank(c);
}

/** Whether every byte of [start, end) is whitespace to @p dialect. */
static bool ini_all_space(const GTEXT_INI_Dialect * dialect, const char * bytes,
    size_t start, size_t end) {
  for (size_t i = start; i < end; i++) {
    if (!gtext_ini_is_space(dialect, bytes[i])) return false;
  }
  return true;
}

/**
 * Join the continuations in [start, end) into @p out, which must have room for
 * `end - start` bytes. Returns the joined length, or SIZE_MAX when there is no
 * continuation in the range at all.
 *
 * SIZE_MAX rather than "the same bytes" so that a caller can tell "nothing to do"
 * from "joined to the same length", and store no canonical form in the first case.
 * A join only ever shortens: the backslash and its terminator become one space, or
 * nothing.
 */
static size_t ini_join_continuations(const GTEXT_INI_Dialect * dialect,
    const char * bytes, size_t len, size_t start, size_t end, char * out) {
  size_t w = 0;
  bool joined = false;
  size_t i = start;
  while (i < end) {
    ini_continuation cont = gtext_ini_continuation_at(dialect, bytes, len, i);
    if (cont.span) {
      joined = true;
      if (!cont.ends_line &&
          dialect->continuation == GTEXT_INI_CONTINUATION_JOIN_SPACE) {
        out[w++] = ' ';
      }
      /*
       * `cont.span` can reach past `end` - a continuation at the end of the range
       * consumes its terminator, which is outside it - so clamp rather than trust
       * it. The bytes between are the comment block it skipped, and they are not
       * part of the name.
       */
      i += cont.span;
      continue;
    }
    out[w++] = bytes[i++];
  }
  return joined ? w : SIZE_MAX;
}

/** Where the logical line starting at @p offset ends. */
typedef struct {
  size_t content_end; /* Just past its last content byte. */
  size_t line_end;    /* Just past the whole thing, terminators included. */
} ini_logical_line;

/**
 * Find the logical line starting at @p offset.
 *
 * For a dialect with no continuation this is the physical line and the loop runs
 * once, which is what keeps the other four dialects untouched. For systemd it
 * follows a trailing backslash onto the next line, skipping a comment block, until
 * a line does not end in one.
 *
 * **Only called for a line that is neither blank nor a comment**, and that is
 * measured rather than convenient: `# c\` followed by an entry does *not* continue
 * the comment - the entry is read normally - so a comment's trailing backslash is
 * comment text. systemd classifies the first line, then assembles.
 */
static ini_logical_line ini_logical_line_at(const GTEXT_INI_Dialect * dialect,
    const char * bytes, size_t len, size_t offset) {
  ini_logical_line r;
  size_t i = offset;
  for (;;) {
    size_t term = 0;
    while (i < len && !(term = gtext_ini_terminator_len(dialect, bytes, len, i))) {
      i++;
    }
    r.content_end = i;
    r.line_end = i + term;
    if (i > offset) {
      ini_continuation cont =
          gtext_ini_continuation_at(dialect, bytes, len, i - 1);
      if (cont.span && !cont.ends_line) {
        i = (i - 1) + cont.span;
        continue;
      }
      if (cont.span) {
        /* The logical line ends here and the backslash disappears. It stays inside
         * `content_end` because it is one of the document's bytes and the pieces
         * have to tile; what it is *not* is part of the joined name or value. */
        r.line_end = (i - 1) + cont.span;
        return r;
      }
    }
    return r;
  }
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

/**
 * Store the canonical form of a name beside it, when the dialect has one.
 *
 * Leaves `canon` absent when the dialect does not fold or canonicalize, which is
 * what every lookup reads as "compare the spelling directly".
 */
static bool ini_set_canon(ini_parse * p, ini_str * canon, const char * raw,
    size_t len, bool is_group) {
  const GTEXT_INI_Dialect * d = &p->doc->dialect;
  bool folds = is_group ? (gtext_ini_group_names_fold(d) || d->subsection_syntax)
                        : d->fold_case;
  /*
   * Two independent reasons a name can have a canonical form, and a dialect may
   * want either, both, or neither:
   *
   *   - it **folds** - git lower-cases a section and a key, EditorConfig a key;
   *   - it **continues** - systemd's `[Serv\` + `ice]` is the section `Serv ice`,
   *     so the joined form is canonical and the document's bytes are the name.
   *
   * The join happens first and the fold applies to its result, which is the only
   * order that composes: a folded, continued name would otherwise depend on which
   * step ran.
   */
  bool joins = d->continuation != GTEXT_INI_CONTINUATION_NONE;
  if (!folds && !joins) return true;

  char stack[512];
  char * buf = stack;
  char * heap = NULL;
  /* A canonical form is never longer than the raw one: folding is per byte, the
   * subsection spelling only ever drops characters, and a join turns at least two
   * bytes into at most one. */
  if (len > sizeof(stack)) {
    heap = gtext_allocator_malloc(p->alloc, len ? len : 1);
    if (!heap) return false;
    buf = heap;
  }

  const char * source = raw;
  size_t source_len = len;
  char * joined = NULL;
  if (joins) {
    size_t offset = (size_t) (raw - p->bytes);
    size_t got = ini_join_continuations(&p->doc->dialect, p->bytes, p->len,
        offset, offset + len, buf);
    if (got != SIZE_MAX) {
      /* There was a continuation, so the joined text is the starting point for the
       * fold - and is itself the canonical form when the dialect does not fold. */
      joined = buf;
      source = buf;
      source_len = got;
      if (!is_group) {
        /*
         * A key whose continuation sits at its very end - `Environment\` then
         * `=v` - joins to `Environment ` with a trailing space, because the raw
         * span was trimmed before the join and the injected space came after.
         * systemd trims whitespace before the `=`, so trim it here too. A no-op
         * for every other case: the raw span was already trimmed, and no other
         * dialect can put whitespace in a key this way.
         */
        while (source_len && ini_sep_space(d, source[source_len - 1])) {
          source_len--;
        }
      }
    }
  }

  bool ok = true;
  if (folds) {
    char fold_stack[512];
    char * fold_buf = fold_stack;
    char * fold_heap = NULL;
    if (source_len > sizeof(fold_stack)) {
      fold_heap = gtext_allocator_malloc(p->alloc, source_len ? source_len : 1);
      if (!fold_heap) {
        if (heap) gtext_allocator_free(p->alloc, heap);
        return false;
      }
      fold_buf = fold_heap;
    }
    size_t canon_len = 0;
    ok = is_group
             ? gtext_ini_canon_group(d, source, source_len, fold_buf, &canon_len)
             : gtext_ini_canon_key(d, source, source_len, fold_buf, &canon_len);
    if (ok) ok = gtext_ini_str_set(p->alloc, canon, fold_buf, canon_len);
    if (fold_heap) gtext_allocator_free(p->alloc, fold_heap);
  }
  else if (joined) {
    ok = gtext_ini_str_set(p->alloc, canon, source, source_len);
  }
  if (heap) gtext_allocator_free(p->alloc, heap);
  return ok;
}

/** Grow doc->groups by one and return the fresh, zeroed group. */
static GTEXT_INI_Group * ini_push_group(ini_parse * p) {
  if (p->doc->count == p->doc->capacity) {
    size_t want = p->doc->capacity ? p->doc->capacity * 2 : 8;
    GTEXT_INI_Group * grown = gtext_allocator_realloc(
        p->alloc, p->doc->groups, want * sizeof(*grown));
    if (!grown) return NULL;
    p->doc->groups = grown;
    p->doc->capacity = want;
    for (size_t g = 0; g < p->doc->count; g++) p->doc->groups[g].doc = p->doc;
  }
  GTEXT_INI_Group * group = &p->doc->groups[p->doc->count];
  memset(group, 0, sizeof(*group));
  group->doc = p->doc;
  p->doc->count++;
  return group;
}

/** Add a group header to the document, attaching any pending comments. */
static bool ini_add_group(ini_parse * p, size_t name_start, size_t name_end,
    size_t pre_start, size_t pre_end, size_t post_start, size_t post_end,
    size_t line_start) {
  const char * name = p->bytes + name_start;
  size_t name_len = name_end - name_start;
  if (!gtext_ini_group_name_ok(&p->doc->dialect, name, name_len)) {
    return ini_fail(p, GTEXT_INI_E_BAD_GROUP,
        "a group name is empty or uses a spelling the dialect does not allow",
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
  GTEXT_INI_Group * group = ini_push_group(p);
  if (!group) return ini_fail(p, GTEXT_INI_E_OOM, "out of memory", line_start);
  group->verbatim = true;
  if (!gtext_ini_str_set(p->alloc, &group->name, name, name_len) ||
      !ini_set_canon(p, &group->canon, name, name_len, true) ||
      !gtext_ini_str_set(p->alloc, &group->hdr_pre, p->bytes + pre_start,
          pre_end - pre_start) ||
      !gtext_ini_str_set(p->alloc, &group->hdr_post, p->bytes + post_start,
          post_end - post_start)) {
    return ini_fail(p, GTEXT_INI_E_OOM, "out of memory", line_start);
  }
  if (!ini_buf_take(p->alloc, &p->pending, &group->comment)) {
    return ini_fail(p, GTEXT_INI_E_OOM, "out of memory", line_start);
  }
  p->group_index = p->doc->count - 1;
  return true;
}

/**
 * The group a preamble entry belongs to, created on first need.
 *
 * An empty name and ::GTEXT_INI_Group::preamble set, so the writer emits no
 * header for it. The alternative - naming it, say, `""` in the tree and writing
 * `[]` back - would put a line in the output that was not in the input.
 */
static GTEXT_INI_Group * ini_preamble_group(ini_parse * p, size_t line_start) {
  GTEXT_INI_Group * group = ini_push_group(p);
  if (!group) {
    ini_fail(p, GTEXT_INI_E_OOM, "out of memory", line_start);
    return NULL;
  }
  group->preamble = true;
  group->verbatim = true;
  if (!gtext_ini_str_set(p->alloc, &group->name, "", 0)) {
    ini_fail(p, GTEXT_INI_E_OOM, "out of memory", line_start);
    return NULL;
  }
  p->group_index = p->doc->count - 1;
  return group;
}

/**
 * Add an entry to the current group.
 *
 * @param has_value False for a valueless key, where @p value_start and
 *   @p value_end are ignored and no `sep` is stored.
 */
static bool ini_add_entry(ini_parse * p, size_t line_start, size_t key_start,
    size_t key_end, size_t value_start, size_t value_end, size_t line_end,
    bool has_value) {
  if (p->group_index == INI_NO_GROUP) {
    if (!p->doc->dialect.allow_preamble) {
      /*
       * A dialect that forbids a preamble says so here rather than inventing a
       * group: putting an unnamed group in the tree would be a group the
       * document does not have, and a rewrite would then emit its header.
       */
      return ini_fail(p, GTEXT_INI_E_NO_GROUP,
          "an entry appeared before the first group header", line_start);
    }
    if (!ini_preamble_group(p, line_start)) return false;
  }
  GTEXT_INI_Group * group = &p->doc->groups[p->group_index];
  size_t key_len = key_end - key_start;
  if (!gtext_ini_key_ok(&p->doc->dialect, p->bytes + key_start, key_len)) {
    return ini_fail(p, GTEXT_INI_E_BAD_KEY,
        "a key name is empty or uses a character the dialect does not allow",
        key_start);
  }
  if (p->doc->dialect.dupkey == GTEXT_INI_DUPKEY_ERROR) {
    for (size_t e = 0; e < group->count; e++) {
      if (group->entries[e].key.len == key_len &&
          memcmp(group->entries[e].key.data, p->bytes + key_start, key_len) ==
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
  entry->verbatim = true;
  entry->has_value = has_value;
  bool ok = gtext_ini_str_set(p->alloc, &entry->key, p->bytes + key_start,
                key_len) &&
            ini_set_canon(p, &entry->canon, p->bytes + key_start, key_len,
                false) &&
            gtext_ini_str_set(p->alloc, &entry->pre, p->bytes + line_start,
                key_start - line_start);
  if (ok && has_value) {
    ok = gtext_ini_str_set(p->alloc, &entry->value, p->bytes + value_start,
             value_end - value_start) &&
         gtext_ini_str_set(p->alloc, &entry->sep, p->bytes + key_end,
             value_start - key_end) &&
         gtext_ini_str_set(p->alloc, &entry->eol, p->bytes + value_end,
             line_end - value_end);
  }
  else if (ok) {
    /* No `=`, so no separator and no value: everything from the end of the key
     * to the end of the line is the terminator run. */
    ok = gtext_ini_str_set(p->alloc, &entry->eol, p->bytes + key_end,
        line_end - key_end);
  }
  if (!ok) return ini_fail(p, GTEXT_INI_E_OOM, "out of memory", line_start);
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

/**
 * The offset of the `]` that closes a header, or SIZE_MAX.
 *
 * Quote-aware when the dialect has subsections, because a `]` inside a quoted
 * subsection name is data: measured, `[a "b]c"]` is the subsection `b]c`. A
 * scan for the first `]` would cut that header in the wrong place and then
 * refuse it.
 *
 * **Which `]` closes the header is the dialect's, not a detail.** EditorConfig's
 * names "may contain any characters between the square brackets", so it closes at
 * the *last* `]` on the line and `[a]b]` is one section named `a]b`; for every
 * other dialect here a `]` inside a name is not spellable and the first one ends
 * it. Asking gtext_ini_group_close_is_last() keeps that a property of the name
 * grammar rather than a test against an id.
 *
 * The two modes are one loop rather than two: taking the last match is
 * remembering instead of returning, and a second loop would be a second place
 * for the quoting rules to be got wrong.
 */
static size_t ini_find_header_close(const GTEXT_INI_Dialect * dialect,
    const char * bytes, size_t start, size_t end) {
  bool last = gtext_ini_group_close_is_last(dialect);
  size_t found = SIZE_MAX;
  bool quote = false;
  for (size_t i = start; i < end; i++) {
    char c = bytes[i];
    if (dialect->subsection_syntax) {
      if (c == '"') {
        quote = !quote;
        continue;
      }
      if (quote) {
        /* The subsection's own escape layer: a backslash hides whatever follows
         * it, the closing quote included. */
        if (c == '\\') i++;
        continue;
      }
    }
    if (c == ']') {
      if (!last) return i;
      found = i;
    }
  }
  return found;
}

/** Where one entry ends, and whether it parsed. */
typedef struct {
  size_t next_offset;
  bool ok;
} ini_entry_result;

/**
 * Read one entry, starting at @p key_start, and say where the logical line
 * ended.
 *
 * @param line_start First byte of the line's own bytes - the start of `pre`.
 *   Differs from @p key_start when the line began with whitespace, and when the
 *   entry follows a group header on the same line.
 * @param content_end End of the physical line's content, terminator excluded.
 * @param line_end End of the physical line, terminator included.
 */
static ini_entry_result ini_read_entry(ini_parse * p, size_t line_start,
    size_t key_start, size_t content_end, size_t line_end) {
  const GTEXT_INI_Dialect * d = &p->doc->dialect;
  ini_entry_result r;
  r.next_offset = line_end;
  r.ok = false;

  /*
   * Where the key ends. A dialect whose keys are a closed character set ends the
   * key at the first byte outside it, which is what lets `k` with no `=` be an
   * entry at all; a dialect without one has to find the `=` first and then trim
   * backwards, because its keys may contain spaces.
   */
  size_t key_end;
  if (d->name_style == GTEXT_INI_NAMES_GIT) {
    key_end = key_start;
    while (key_end < content_end &&
           gtext_ini_key_char_ok(d, p->bytes[key_end])) {
      key_end++;
    }
    /*
     * The charset predicate, not gtext_ini_key_ok(): asking the latter one byte
     * at a time would apply git's "must begin with a letter" rule to every byte
     * and stop the scan at the `1` of `ab12`. The positional rule is enforced
     * once, by ini_add_entry(), over the whole name.
     */
    if (key_end == key_start) {
      r.ok = ini_fail(p, GTEXT_INI_E_BAD_KEY,
          "a key name is empty or uses a character the dialect does not allow",
          key_start);
      return r;
    }
  }
  else {
    size_t eq = key_start;
    while (eq < content_end && p->bytes[eq] != '=') eq++;
    if (eq >= content_end) {
      /*
       * A line with no `=` at all. **This is where
       * ::GTEXT_INI_Dialect::valueless_keys stops being reachable** for a dialect
       * whose keys are not a closed character set: there is no key yet to call
       * valueless, because the key's extent is defined by the `=` that is missing.
       * So the flag is dead for this branch, and the header says so - a mutation
       * setting it true on the EditorConfig dialect left both of that dialect's
       * gates green, which is how the coupling got written down.
       */
      r.ok = ini_fail(p, GTEXT_INI_E_BAD_LINE,
          "a line is not blank, a comment, a group header or an entry",
          line_start);
      return r;
    }
    key_end = eq;
    while (key_end > key_start && ini_sep_space(d, p->bytes[key_end - 1])) {
      key_end--;
    }
  }

  /* What may sit between the key and the `=`: see ini_sep_space(). */
  size_t eq = key_end;
  while (eq < content_end && ini_sep_space(d, p->bytes[eq])) eq++;

  if (eq >= content_end || p->bytes[eq] != '=') {
    if (d->valueless_keys && eq >= content_end) {
      /* A key with nothing after it. Measured: `k` alone is an entry with no
       * value, and `k ; c` is *refused* - a trailing comment is not allowed
       * here, which is why this tests for the end of the content rather than
       * skipping a comment first. */
      r.ok = ini_add_entry(p, line_start, key_start, key_end, 0, 0, line_end,
          false);
      return r;
    }
    r.ok = ini_fail(p, GTEXT_INI_E_BAD_LINE,
        "a line is not blank, a comment, a group header or an entry",
        line_start);
    return r;
  }

  /* §3.3: "Space before and after the equals sign should be ignored." The
   * leading run goes into `sep`; what happens to a trailing run is the
   * dialect's, and for a scanning dialect the scanner decides it. */
  size_t value_start = eq + 1;
  while (value_start < content_end &&
         gtext_ini_is_space(d, p->bytes[value_start])) {
    value_start++;
  }

  if (gtext_ini_dialect_scans_values(d)) {
    /*
     * The scanner is given everything to the end of the input, not to the end of
     * this physical line, because a continuation may carry the value onto the
     * next one. What it consumed is what the logical line spans.
     */
    ini_value_scan scan;
    GTEXT_INI_Status status = gtext_ini_scan_value(d, p->bytes + value_start,
        p->len - value_start, NULL, NULL, &scan);
    if (status != GTEXT_INI_OK) {
      const char * message =
          scan.open_quote ? "a quoted value is missing its closing quote"
                          : "a backslash names no escape sequence this dialect "
                            "has";
      r.ok = ini_fail(p, status, message, value_start + scan.fault_offset);
      return r;
    }
    size_t value_end = value_start + scan.content_len;
    size_t logical_end = value_start + scan.logical_len + scan.term_len;
    r.next_offset = logical_end;
    r.ok = ini_add_entry(p, line_start, key_start, key_end, value_start,
        value_end, logical_end, true);
    return r;
  }

  size_t value_end = content_end;
  if (d->trim_trailing_space) {
    while (value_end > value_start &&
           gtext_ini_is_space(d, p->bytes[value_end - 1])) {
      value_end--;
    }
  }
  r.ok = ini_add_entry(p, line_start, key_start, key_end, value_start,
      value_end, line_end, true);
  return r;
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
    /* Skipped, not discarded - the writer puts it back. */
    if (!gtext_ini_str_set(alloc, &doc->bom, bytes, 3)) {
      gtext_ini_set_error(err, GTEXT_INI_E_OOM, "out of memory", bytes, len, 0);
      gtext_ini_free(doc);
      return NULL;
    }
  }

  bool ok = true;
  while (ok && offset < len) {
    /*
     * The physical line's bytes, and the bytes of the line including its
     * terminator, asked through gtext_ini_terminator_len().
     *
     * A CR is part of the terminator only when an LF follows it - unless the
     * dialect ends a line on a lone CR, which only systemd does. Without that
     * distinction a trailing CR at end of input was stripped as though it were a
     * terminator, so the same bytes gave the generic dialect a value one byte
     * shorter than the strict dialect's; that is what fuzz_ini.cpp's parity
     * property is for, and what it found on its first run at 237k executions.
     * Only the last CR before the LF is a terminator; any before that are data.
     */
    size_t content_end = offset;
    size_t term_len = 0;
    while (content_end < len &&
           !(term_len = gtext_ini_terminator_len(&effective.dialect, bytes, len,
                 content_end))) {
      content_end++;
    }
    size_t line_end = content_end + term_len;

    size_t pre_end = offset;
    if (effective.dialect.allow_leading_whitespace) {
      while (pre_end < content_end &&
             gtext_ini_is_space(&effective.dialect, bytes[pre_end])) {
        pre_end++;
      }
    }

    bool blank = ini_all_space(&effective.dialect, bytes, offset, content_end);
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
      offset = line_end;
      continue;
    }

    /*
     * From here the line may be a **logical** one. systemd's continuation works on
     * a group header and on a key as well as in a value, because it assembles the
     * line before classifying it - so this is recomputed after the blank and
     * comment tests rather than before them, which is measured and not merely
     * convenient: `# c\` followed by an entry does not continue the comment.
     *
     * For every other dialect ini_logical_line_at() returns the physical line it
     * was already given, so nothing changes for them.
     */
    {
      ini_logical_line logical =
          ini_logical_line_at(&effective.dialect, bytes, len, offset);
      content_end = logical.content_end;
      line_end = logical.line_end;
    }

    if (bytes[pre_end] == '[') {
      size_t close =
          ini_find_header_close(&effective.dialect, bytes, pre_end + 1,
              content_end);
      if (close == SIZE_MAX) {
        ok = ini_fail(&p, GTEXT_INI_E_BAD_GROUP,
            "a group header is missing its ']'", offset);
        offset = line_end;
        continue;
      }
      /* What follows the `]`: whitespace, and then either nothing or - for a
       * dialect that allows it - an entry on the same line. */
      size_t rest = close + 1;
      while (rest < content_end &&
             gtext_ini_is_space(&effective.dialect, bytes[rest])) {
        rest++;
      }
      bool remainder = rest < content_end;
      if (remainder && effective.dialect.header_remainder_is_entry &&
          ((bytes[rest] == '#' && effective.dialect.comment_hash) ||
              (bytes[rest] == ';' && effective.dialect.comment_semicolon))) {
        /*
         * `[a] ; c` - a comment, not an entry. git accepts it, and its top-level
         * loop is why: once the header is read it goes back to classifying, and a
         * comment introducer is tested before a key. Treating the remainder as an
         * entry unconditionally refused a document git reads, which is what the
         * differential caught.
         *
         * The comment stays in `hdr_post`, which already spans the `]` to the end
         * of the line, so it is preserved without a place of its own.
         */
        remainder = false;
      }
      if (remainder && !effective.dialect.header_remainder_is_entry) {
        /* `[G] junk` - GKeyFile refuses this, and so must anything claiming to
         * read the same documents. */
        ok = ini_fail(&p, GTEXT_INI_E_BAD_LINE,
            "a group header must be followed by nothing but whitespace",
            close + 1);
        offset = line_end;
        continue;
      }
      /*
       * With a remainder, the whitespace after the `]` belongs to the entry's
       * `pre` rather than to the header's `hdr_post`, so that the header ends at
       * the `]` and the pieces still tile the line exactly.
       */
      size_t post_end = remainder ? close + 1 : line_end;
      ok = ini_add_group(&p, pre_end + 1, close, offset, pre_end, close + 1,
          post_end, offset);
      if (ok && remainder) {
        ini_entry_result r =
            ini_read_entry(&p, close + 1, rest, content_end, line_end);
        ok = r.ok;
        offset = r.next_offset;
        continue;
      }
      offset = line_end;
      continue;
    }

    ini_entry_result r =
        ini_read_entry(&p, offset, pre_end, content_end, line_end);
    ok = r.ok;
    offset = r.next_offset;
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
