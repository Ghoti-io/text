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
  d.strict_key_charset = true;
  d.locale_postfix = true;
  d.require_unlocalized_key = true;
  d.accept_crlf = false;
  d.trim_trailing_space = false;
  d.skip_bom = false;
  d.escapes = ini_desktop_escapes;
  d.list_separator = ';';
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
  d.strict_key_charset = false;
  /* The one normalisation. See above: this changes values, not just which
   * documents are accepted. */
  d.accept_crlf = true;
  d.skip_bom = true;
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
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char) name[i];
    if (c == '[' || c == ']') return false;
    if (c < 0x20 || c == 0x7F) return false;
    /* §3.2 says "all ASCII characters", so a byte above 0x7F is outside the
     * name grammar. `GKeyFile` accepts one; @ref format_ini records that as a
     * deviation, with the reproduction, rather than following it. */
    if (dialect->strict_key_charset && c > 0x7F) return false;
  }
  return true;
}

bool gtext_ini_key_ok(const GTEXT_INI_Dialect * dialect, const char * key,
    size_t len) {
  if (!len) return false;
  if (!dialect->strict_key_charset) {
    /* Still not anything at all: a key may not contain the delimiter or a line
     * terminator, or the document would not read back as itself. */
    for (size_t i = 0; i < len; i++) {
      char c = key[i];
      if (c == '=' || c == '\n' || c == '\r') return false;
    }
    return true;
  }
  size_t base = dialect->locale_postfix ? gtext_ini_key_base_len(key, len)
                                        : len;
  if (!base) return false;
  for (size_t i = 0; i < base; i++) {
    char c = key[i];
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-';
    if (!ok) return false;
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
