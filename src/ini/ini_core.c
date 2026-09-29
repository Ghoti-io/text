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
 * @file ini_core.c
 * @brief Dialect constructors, option defaults, error release, small helpers.
 */

#include "ini_internal.h"
#include <stdlib.h>

/*
 * Desktop Entry §4's escape set, as the letters that may follow a backslash.
 * `;` is absent on purpose: §4 defines it only inside a list, and
 * g_key_file_get_string() refuses a bare `a\;b` that
 * g_key_file_get_string_list() accepts. gtext_ini_value_list() adds it.
 */
static const char ini_desktop_escapes[] = "snrt\\";

/*
 * git's escape set, from `git-config(1)`: "\n", "\t" and "\b" for the control
 * characters, and "\\" and "\"" for themselves. "Other char escape sequences
 * (including octal escape sequences) are invalid" - measured, `k = a\qb` and
 * `k = a\101b` are both refused, and so is `k = a\ b`.
 *
 * Note what is *absent*: `\r`, which Desktop Entry has, and `\s`. git has no
 * spelling for a carriage return in a value at all.
 */
static const char ini_git_escapes[] = "ntb\\\"";

GTEXT_INI_Dialect gtext_ini_dialect_desktop_entry(void) {
  GTEXT_INI_Dialect d;
  memset(&d, 0, sizeof(d));
  d.id = GTEXT_INI_DIALECT_DESKTOP_ENTRY;
  d.comment_hash = true;
  d.comment_semicolon = false;
  d.allow_leading_whitespace = false;
  d.allow_preamble = false;
  d.allow_duplicate_groups = false;
  d.dupkey = GTEXT_INI_DUPKEY_ERROR;
  d.name_style = GTEXT_INI_NAMES_DESKTOP_ENTRY;
  d.locale_postfix = true;
  d.require_unlocalized_key = true;
  d.accept_crlf = false;
  d.trim_trailing_space = false;
  d.skip_bom = false;
  d.escapes = ini_desktop_escapes;
  d.list_separator = ';';
  d.utf8_values = true;
  d.continuation = GTEXT_INI_CONTINUATION_NONE;
  d.inline_comments = false;
  d.quoted_values = false;
  d.fold_case = false;
  d.subsection_syntax = false;
  d.valueless_keys = false;
  d.header_remainder_is_entry = false;
  return d;
}

GTEXT_INI_Dialect gtext_ini_dialect_generic(void) {
  /*
   * Built by relaxing the base rather than by listing fields, so that a field
   * added to the Desktop Entry dialect is inherited here instead of silently
   * defaulting to zero.
   *
   * **Six of the seven assignments are relaxations and one is not.** The six
   * only widen what is accepted, so every document the strict dialect accepts
   * is accepted here with the same values. `accept_crlf` is different: it
   * removes a CR from the *content* of a line the strict dialect already
   * accepted, so a document with a CRLF on an entry line reads with one byte
   * less here. That is a behaviour change dressed as a relaxation, and calling
   * it one cost a defect - fuzz_ini.cpp's parity property found it at 30,209
   * executions, which is exactly what it is for. `skip_bom` travels with it
   * because a BOM is the same kind of thing at the start of the document.
   *
   * `escapes` and `list_separator` are deliberately untouched: a change to how
   * a value is *decoded* would not be a relaxation either, and unlike CRLF
   * there is no reason to want one.
   */
  GTEXT_INI_Dialect d = gtext_ini_dialect_desktop_entry();
  d.id = GTEXT_INI_DIALECT_GENERIC;
  /* The six relaxations. */
  d.comment_semicolon = true;
  d.allow_leading_whitespace = true;
  d.allow_preamble = true;
  d.allow_duplicate_groups = true;
  d.dupkey = GTEXT_INI_DUPKEY_LAST_WINS;
  d.name_style = GTEXT_INI_NAMES_ANY;
  /* The one normalisation. See above: this changes values, not just which
   * documents are accepted. */
  d.accept_crlf = true;
  d.skip_bom = true;
  return d;
}

GTEXT_INI_Dialect gtext_ini_dialect_git_config(void) {
  /*
   * Written out field by field rather than derived from another dialect, which
   * is the opposite of gtext_ini_dialect_generic() and is deliberate.
   *
   * The generic dialect is Desktop Entry plus a list of changes, so deriving it
   * means a field added later is inherited and the list stays short. git config
   * is not a relaxation of Desktop Entry in *either* direction - it accepts a
   * preamble, an inline comment, a continuation, a quoted value, a valueless
   * key, a repeated key and a subsection, and it refuses a key that does not
   * begin with a letter and a group name holding anything but `A-Za-z0-9-.`.
   * Deriving it would hide that by making the differences look like a short
   * list of tweaks, and would silently inherit any future Desktop Entry field
   * whose default happens to be wrong here. A dialect that is its own grammar
   * is spelled as its own grammar.
   */
  GTEXT_INI_Dialect d;
  memset(&d, 0, sizeof(d));
  d.id = GTEXT_INI_DIALECT_GIT_CONFIG;
  d.comment_hash = true;
  d.comment_semicolon = true;
  /* Leading whitespace before a key or a header is skipped; measured, both
   * `  [a]` and `\t k = v` are accepted. */
  d.allow_leading_whitespace = true;
  d.allow_preamble = true;
  /* `[core]` twice is one section to git: `--list` prints both entries and
   * merges them under one name. */
  d.allow_duplicate_groups = true;
  /* Every occurrence is a value. `--get` answers the last and `--get-all` all
   * of them, in order. */
  d.dupkey = GTEXT_INI_DUPKEY_COLLECT;
  d.name_style = GTEXT_INI_NAMES_GIT;
  /* No locale postfix: `k[de]` is not a git key at all, because `[` is not in
   * its key charset. Leaving locale_postfix false is what makes `k[de] = v` a
   * GTEXT_INI_E_BAD_KEY here rather than a localized spelling of `k`. */
  d.locale_postfix = false;
  d.require_unlocalized_key = false;
  /*
   * git converts CRLF to LF as it reads - its get_next_char() peeks after a CR
   * and keeps the CR only when no LF follows. So a lone CR is data, and one
   * before an LF is part of the terminator.
   */
  d.accept_crlf = true;
  /*
   * Trailing whitespace is dropped from a value, but not by trimming the span:
   * git tracks the offset of the last content byte, and an escape counts as
   * content. `k = a\t` therefore keeps the tab. The scanner is what implements
   * that; this flag records the intent for a reader of the dialect.
   */
  d.trim_trailing_space = true;
  /*
   * Measured: a file beginning with a UTF-8 BOM parses, and the first section
   * is found. git does not reject it and does not fold it into the name.
   */
  d.skip_bom = true;
  d.escapes = ini_git_escapes;
  /*
   * **No list separator.** git has multi-valued keys, and they are spelled as
   * repeated lines rather than as one delimited value - which is what
   * GTEXT_INI_DUPKEY_COLLECT is for. Setting a separator here would invent a
   * syntax git does not have, and gtext_ini_value_list() correctly refuses a
   * dialect that has none.
   */
  d.list_separator = 0;
  /* False: `git config --get` returns a value holding a bare 0xFF unchanged,
   * where g_key_file_get_string() refuses one. */
  d.utf8_values = false;
  d.continuation = GTEXT_INI_CONTINUATION_JOIN_EMPTY;
  d.inline_comments = true;
  d.quoted_values = true;
  d.fold_case = true;
  d.subsection_syntax = true;
  d.valueless_keys = true;
  d.header_remainder_is_entry = true;
  return d;
}

GTEXT_INI_Parse_Options gtext_ini_parse_options_default(void) {
  GTEXT_INI_Parse_Options o;
  memset(&o, 0, sizeof(o));
  o.allocator = NULL;
  o.dialect = gtext_ini_dialect_desktop_entry();
  o.max_total_bytes = 0;
  o.max_groups = 0;
  o.max_entries_per_group = 0;
  /* True, unlike the TOML module: Desktop Entry §3 requires that a rewrite
   * preserve comments and unknown fields, so a default that dropped them could
   * not rewrite a file correctly. */
  o.retain_comments = true;
  return o;
}

void gtext_ini_error_free(GTEXT_INI_Error * err) {
  if (!err) return;
  /* The default allocator, not the parse's: an error outlives the parse, and a
   * parse can fail before it has read its options. See
   * gtext_toml_error_free(). */
  if (err->context_snippet) {
    gtext_allocator_free(gtext_allocator_default(), err->context_snippet);
    err->context_snippet = NULL;
  }
  err->context_snippet_len = 0;
  err->caret_offset = 0;
}

bool gtext_ini_str_set(const GTEXT_Allocator * alloc, ini_str * out,
    const char * bytes, size_t len) {
  char * copy = gtext_allocator_malloc(alloc, len + 1);
  if (!copy) return false;
  if (len) memcpy(copy, bytes, len);
  copy[len] = '\0';
  if (out->data) gtext_allocator_free(alloc, out->data);
  out->data = copy;
  out->len = len;
  return true;
}

void gtext_ini_str_clear(const GTEXT_Allocator * alloc, ini_str * s) {
  if (s->data) gtext_allocator_free(alloc, s->data);
  s->data = NULL;
  s->len = 0;
}

/** How many bytes of context an error carries either side of the caret. */
#define INI_SNIPPET_RADIUS 40

void gtext_ini_set_error(GTEXT_INI_Error * err, GTEXT_INI_Status code,
    const char * message, const char * input, size_t input_len,
    size_t offset) {
  if (!err) return;
  memset(err, 0, sizeof(*err));
  err->code = code;
  err->message = message;
  err->offset = offset;

  /* Line and column, counted from the start. Columns are characters, so a
   * continuation byte does not advance one. */
  int line = 1;
  int col = 1;
  for (size_t i = 0; i < offset && i < input_len; i++) {
    if (input[i] == '\n') {
      line++;
      col = 1;
    }
    else if ((input[i] & 0xC0) != 0x80) {
      col++;
    }
  }
  err->line = line;
  err->col = col;

  if (!input || !input_len) return;
  size_t start = offset > INI_SNIPPET_RADIUS ? offset - INI_SNIPPET_RADIUS : 0;
  size_t end = offset + INI_SNIPPET_RADIUS;
  if (end > input_len) end = input_len;
  size_t span = end - start;
  char * snippet = gtext_allocator_malloc(gtext_allocator_default(), span + 1);
  if (!snippet) return; /* An error with no snippet is still the right error. */
  memcpy(snippet, input + start, span);
  snippet[span] = '\0';
  err->context_snippet = snippet;
  err->context_snippet_len = span;
  err->caret_offset = offset - start;
}

size_t gtext_ini_key_base_len(const char * key, size_t len) {
  /* A postfix is a trailing `[...]`, so scan from the end: a key may contain a
   * `[` that is not the postfix's when the charset is not strict. */
  if (len < 3 || key[len - 1] != ']') return len;
  for (size_t i = len - 2; i > 0; i--) {
    if (key[i] == '[') return i;
    if (key[i] == ']') return len; /* `a]b]` is not a postfix. */
  }
  return len;
}

bool gtext_ini_is_space(const GTEXT_INI_Dialect * dialect, char c) {
  if (c == ' ' || c == '\t') return true;
  /*
   * CR is whitespace to git and data to Desktop Entry. Tying it to accept_crlf
   * rather than to the dialect id keeps it a property a caller can set.
   *
   * `\v` and `\f` are deliberately absent for every dialect: git's own ctype
   * table classes them as control characters rather than space - measured, a
   * trailing `\v` stays in the value and a leading one is a syntax error - and
   * Desktop Entry's grammar has no whitespace but space and tab either. No
   * dialect here wants <ctype.h>'s answer.
   */
  if (c == '\r' && dialect->accept_crlf) return true;
  return false;
}

/** Whether @p c may appear in a git section or key name: `A-Za-z0-9-`. */
static bool ini_git_keychar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '-';
}

static char ini_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c;
}

bool gtext_ini_canon_group(const GTEXT_INI_Dialect * dialect, const char * raw,
    size_t len, char * out, size_t * out_len) {
  *out_len = 0;
  if (!dialect->fold_case && !dialect->subsection_syntax) return false;
  size_t w = 0;
  size_t i = 0;
  /* The section part: `A-Za-z0-9-` plus `.`, folded. A `.` here is the
   * deprecated subsection spelling, whose subsection folds too, so the whole
   * run can be folded in one pass. */
  while (i < len && (ini_git_keychar(raw[i]) || raw[i] == '.')) {
    out[w++] = dialect->fold_case ? ini_lower(raw[i]) : raw[i];
    i++;
  }
  if (i == len) {
    /* No quoted subsection. An empty name is refused - measured, `[]` is a
     * syntax error to git while `[a ""]` is not. */
    if (!w) return false;
    *out_len = w;
    return true;
  }
  if (!dialect->subsection_syntax) return false;
  /* `[section "sub"]`: whitespace, then a quoted run, then nothing. */
  if (!gtext_ini_is_space(dialect, raw[i])) return false;
  while (i < len && gtext_ini_is_space(dialect, raw[i])) i++;
  if (i >= len || raw[i] != '"') return false;
  i++;
  if (!w) return false;
  out[w++] = '.';
  bool closed = false;
  while (i < len) {
    char c = raw[i];
    if (c == '"') {
      closed = true;
      i++;
      break;
    }
    if (c == '\\') {
      /*
       * The second escape layer, and the one that surprises: a backslash inside
       * a subsection name is simply dropped. Measured - `[a "x\ty"]` is the
       * subsection `xty`, while `\t` in a value one line later is a tab.
       */
      i++;
      if (i >= len) return false;
      c = raw[i];
    }
    /* Case is preserved here, and that is the whole difference between the two
     * spellings: `[a "SubB"]` is not `[a "subb"]`, but `[a.SubB]` is. */
    out[w++] = c;
    i++;
  }
  if (!closed) return false;
  if (i != len) return false; /* `[a "b" ]` and `[a "b"x]` are refused. */
  *out_len = w;
  return true;
}

bool gtext_ini_canon_key(const GTEXT_INI_Dialect * dialect, const char * raw,
    size_t len, char * out, size_t * out_len) {
  *out_len = 0;
  if (!dialect->fold_case) return false;
  if (!len) return false;
  for (size_t i = 0; i < len; i++) out[i] = ini_lower(raw[i]);
  *out_len = len;
  return true;
}

bool gtext_ini_group_name_ok(const GTEXT_INI_Dialect * dialect,
    const char * name, size_t len) {
  /*
   * Desktop Entry §3.2: any ASCII except `[`, `]` and control characters.
   *
   * The empty name is refused, which the specification does not say and
   * `GKeyFile` does: `g_key_file_load_from_data()` on `[]` fails with "Invalid
   * group name: ". The specification being silent, the reference decides, and
   * @ref format_ini records that this is why.
   */
  if (!len) return false;
  if (dialect->name_style == GTEXT_INI_NAMES_GIT) {
    /*
     * git's section grammar, which is stricter than Desktop Entry's: only
     * `A-Za-z0-9-.`, and then optionally whitespace and a quoted subsection.
     * Measured - `[a_b]` and `[caf\xc3\xa9]` are both refused, `[a-b]`, `[a.b]`,
     * `[1a]` and `[12]` are accepted. A section name, unlike a key, need not
     * begin with a letter.
     *
     * The subsection's own rules are gtext_ini_canon_group()'s, and asking it is
     * how this stays a single implementation: a name is legal exactly when it
     * canonicalizes. A second copy of the quoting rules here is what would
     * drift.
     */
    char stack[512];
    char * buf = stack;
    char * heap = NULL;
    if (len > sizeof(stack)) {
      heap = gtext_allocator_malloc(gtext_allocator_default(), len);
      if (!heap) return false;
      buf = heap;
    }
    size_t canon_len = 0;
    bool ok = gtext_ini_canon_group(dialect, name, len, buf, &canon_len);
    if (heap) gtext_allocator_free(gtext_allocator_default(), heap);
    return ok;
  }
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char) name[i];
    if (c == '[' || c == ']') return false;
    if (c < 0x20 || c == 0x7F) return false;
    /* §3.2 says "all ASCII characters", so a byte above 0x7F is outside the
     * name grammar. `GKeyFile` accepts one; @ref format_ini records that as a
     * deviation, with the reproduction, rather than following it. */
    if (dialect->name_style == GTEXT_INI_NAMES_DESKTOP_ENTRY && c > 0x7F) {
      return false;
    }
  }
  return true;
}

bool gtext_ini_key_char_ok(const GTEXT_INI_Dialect * dialect, char c) {
  switch (dialect->name_style) {
    case GTEXT_INI_NAMES_GIT:
      return ini_git_keychar(c);
    case GTEXT_INI_NAMES_ANY:
      /* Anything that reads back as itself: not the delimiter, not a
       * terminator. */
      return c != '=' && c != '\n' && c != '\r';
    case GTEXT_INI_NAMES_DESKTOP_ENTRY:
    default:
      /* §3.3's `A-Za-z0-9-`. The `[LOCALE]` postfix has its own charset and is
       * checked by gtext_ini_key_ok() rather than here, because `[` and `]` are
       * legal in a key only in that one position. */
      return ini_git_keychar(c);
  }
}

bool gtext_ini_key_ok(const GTEXT_INI_Dialect * dialect, const char * key,
    size_t len) {
  if (!len) return false;
  if (dialect->name_style == GTEXT_INI_NAMES_GIT) {
    /*
     * `A-Za-z0-9-`, and the first byte must be a letter. The first-byte rule is
     * git's and is not Desktop Entry's: measured, `1k = v` and `-k = v` are both
     * refused where `k-1 = v` is not. It is also the one place git is stricter
     * about keys than about sections, where `[12]` is fine.
     */
    char first = key[0];
    bool alpha = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z');
    if (!alpha) return false;
    for (size_t i = 0; i < len; i++) {
      if (!gtext_ini_key_char_ok(dialect, key[i])) return false;
    }
    return true;
  }
  if (dialect->name_style == GTEXT_INI_NAMES_ANY) {
    /* Still not anything at all: a key may not contain the delimiter or a line
     * terminator, or the document would not read back as itself. */
    for (size_t i = 0; i < len; i++) {
      if (!gtext_ini_key_char_ok(dialect, key[i])) return false;
    }
    return true;
  }
  /* Desktop Entry §3.3 from here. */
  size_t base = dialect->locale_postfix ? gtext_ini_key_base_len(key, len)
                                        : len;
  if (!base) return false;
  for (size_t i = 0; i < base; i++) {
    if (!gtext_ini_key_char_ok(dialect, key[i])) return false;
  }
  if (base == len) return true;
  /* The postfix: `[` at base, `]` at the end, and a non-empty locale between
   * it of the shape lang_COUNTRY.ENCODING@MODIFIER (§5). */
  if (key[len - 1] != ']') return false;
  if (len - base < 3) return false;
  for (size_t i = base + 1; i + 1 < len; i++) {
    char c = key[i];
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' ||
              c == '@';
    if (!ok) return false;
  }
  return true;
}

ini_entry * gtext_ini_group_push(GTEXT_INI_Group * group) {
  const GTEXT_Allocator * alloc = group->doc->alloc;
  if (group->count == group->capacity) {
    size_t want = group->capacity ? group->capacity * 2 : 8;
    ini_entry * grown = gtext_allocator_realloc(
        alloc, group->entries, want * sizeof(*grown));
    if (!grown) return NULL;
    group->entries = grown;
    group->capacity = want;
  }
  ini_entry * e = &group->entries[group->count];
  memset(e, 0, sizeof(*e));
  group->count++;
  return e;
}
