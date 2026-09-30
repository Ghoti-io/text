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
 * @file ini_value.c
 * @brief Decoding a raw INI value, and Desktop Entry's locale fallback.
 *
 * This is where everything the parser deliberately did not do happens:
 * escapes, list splitting, typed conversion, UTF-8 validation and locale
 * selection. The split is `GKeyFile`'s, measured rather than guessed - a
 * document with `k=a\qb` or with a stray 0xFF in a value parses there and fails
 * only at `g_key_file_get_string()`.
 */

#include "../text_number_internal.h"
#include "ini_internal.h"
#include <stdint.h>
#include <errno.h>
#include <stdlib.h>

/** Whether [bytes, bytes+len) is well-formed UTF-8, rejecting surrogates and
 *  over-long forms. */
static bool ini_utf8_ok(const char * bytes, size_t len) {
  size_t i = 0;
  while (i < len) {
    unsigned char c = (unsigned char) bytes[i];
    size_t need;
    unsigned long cp;
    if (c < 0x80) {
      i++;
      continue;
    }
    else if ((c & 0xE0) == 0xC0) {
      need = 1;
      cp = c & 0x1Fu;
    }
    else if ((c & 0xF0) == 0xE0) {
      need = 2;
      cp = c & 0x0Fu;
    }
    else if ((c & 0xF8) == 0xF0) {
      need = 3;
      cp = c & 0x07u;
    }
    else {
      return false; /* A continuation byte or 0xF8..0xFF as a lead. */
    }
    if (i + need >= len + 0 && i + need > len - 1) return false;
    for (size_t k = 1; k <= need; k++) {
      unsigned char cc = (unsigned char) bytes[i + k];
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3Fu);
    }
    /* Over-long, surrogate, or beyond U+10FFFF - each of which has a shorter
     * or no legal spelling, so accepting it would accept two spellings of one
     * string. */
    if (need == 1 && cp < 0x80) return false;
    if (need == 2 && cp < 0x800) return false;
    if (need == 3 && cp < 0x10000) return false;
    if (cp > 0x10FFFF) return false;
    if (cp >= 0xD800 && cp <= 0xDFFF) return false;
    i += need + 1;
  }
  return true;
}

/** A space or a tab, the only whitespace an INI line may carry inside it. */
static bool ini_is_space_byte(char c) { return c == ' ' || c == '\t'; }

/** The byte an escape letter stands for, or 0 if the letter names none. */
static char ini_escape_byte(char letter) {
  switch (letter) {
    case 's': return ' ';
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case '\\': return '\\';
    /* git's two additions. `\b` is a backspace, which no other dialect here
     * can spell, and `\"` is the quote that would otherwise toggle a quoted
     * run. Neither is in Desktop Entry §4, and `\r` and `\s` are not in git's
     * set - which is why the letters a dialect allows are a field and the bytes
     * they stand for are a shared table. */
    case 'b': return '\b';
    case '"': return '"';
    /* systemd's four more. `\a` and `\v` are spellable in no other dialect here,
     * and `\'` matters because systemd quotes with `'` as well as `"`. All four
     * measured as bytes, because a diagnostic renders a control character as
     * nothing: 07, 0b, 0c and 27. */
    case 'a': return '\a';
    case 'v': return '\v';
    case 'f': return '\f';
    case '\'': return '\'';
    default: return 0;
  }
}

/** Whether @p letter is in the dialect's escape set. */
static bool ini_escape_allowed(const GTEXT_INI_Dialect * dialect, char letter) {
  if (!dialect->escapes) return false;
  for (const char * e = dialect->escapes; *e; e++) {
    if (*e == letter) return true;
  }
  return false;
}

size_t gtext_ini_terminator_len(const GTEXT_INI_Dialect * dialect,
    const char * bytes, size_t len, size_t at) {
  if (at >= len) return 0;
  if (bytes[at] == '\n') return 1;
  if (bytes[at] != '\r') return 0;
  if (dialect->accept_crlf && at + 1 < len && bytes[at + 1] == '\n') return 2;
  /* A lone CR. Only systemd ends a line on one; measured, and not in its manual. */
  return dialect->lone_cr_terminates ? 1 : 0;
}

/** Whether every byte of [start, end) is whitespace to @p dialect. */
static bool ini_run_is_space(const GTEXT_INI_Dialect * dialect,
    const char * bytes, size_t start, size_t end) {
  for (size_t i = start; i < end; i++) {
    if (!gtext_ini_is_space(dialect, bytes[i])) return false;
  }
  return true;
}

/* ini_line_kind and this function's contract are in ini_internal.h: two callers
 * outside this file need exactly the reader's own answer. */
ini_line_kind gtext_ini_classify_line(const GTEXT_INI_Dialect * dialect,
    const char * bytes, size_t len, size_t at, size_t * line_end) {
  size_t i = at;
  size_t term = 0;
  while (i < len && !(term = gtext_ini_terminator_len(dialect, bytes, len, i))) {
    i++;
  }
  size_t content_end = i;
  *line_end = i + term;
  size_t first = at;
  if (dialect->allow_leading_whitespace) {
    while (first < content_end && gtext_ini_is_space(dialect, bytes[first])) {
      first++;
    }
  }
  if (first >= content_end) return INI_LINE_BLANK;
  if (!ini_run_is_space(dialect, bytes, at, first)) return INI_LINE_CONTENT;
  char c = bytes[first];
  if ((c == '#' && dialect->comment_hash) ||
      (c == ';' && dialect->comment_semicolon)) {
    return INI_LINE_COMMENT;
  }
  return INI_LINE_CONTENT;
}

static int ini_hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/** Encode @p cp as UTF-8 into @p out (at least 4 bytes). Returns the length. */
static size_t ini_utf8_encode(uint32_t cp, char * out) {
  if (cp < 0x80) {
    out[0] = (char) cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char) (0xC0 | (cp >> 6));
    out[1] = (char) (0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char) (0xE0 | (cp >> 12));
    out[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char) (0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char) (0xF0 | (cp >> 18));
  out[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char) (0x80 | (cp & 0x3F));
  return 4;
}

/**
 * A numeric escape at @p at, where `raw[at]` is the backslash.
 *
 * Returns the input bytes it consumes, or 0 when the sequence is not a well-formed
 * one - which the caller turns into ::GTEXT_INI_E_BAD_ESCAPE, because systemd
 * discards the whole setting for `\10`, `\400`, `\u41`, `\U00110000` and `\xZZ`
 * exactly as it does for an unknown letter. Measured against systemd 257 - and
 * measured *twice*, because the first reading had it backwards: see
 * ::GTEXT_INI_Dialect::numeric_escapes for how.
 *
 * All three spellings of NUL return 0 too: systemd will not put a NUL in a value,
 * and refuses the setting rather than dropping the byte.
 *
 * A surrogate is encoded rather than refused - `\ud800` is `ed a0 80`, which is
 * what encoding by codepoint without a surrogate check produces, and is what
 * systemd produces.
 *
 * @param decoded Receives up to 4 bytes, or NULL to measure only.
 * @param decoded_len Receives their count. Must not be NULL.
 */
static size_t ini_numeric_escape(const char * raw, size_t len, size_t at,
    char * decoded, size_t * decoded_len) {
  *decoded_len = 0;
  if (at + 1 >= len) return 0;
  char kind = raw[at + 1];
  size_t digits = 0;
  int base = 16;
  size_t start = at + 2;
  if (kind == 'x') {
    digits = 2;
  }
  else if (kind == 'u') {
    digits = 4;
  }
  else if (kind == 'U') {
    digits = 8;
  }
  else if (kind >= '0' && kind <= '7') {
    /* Exactly three octal digits, the first of them this one. */
    digits = 3;
    base = 8;
    start = at + 1;
  }
  else {
    return 0;
  }
  if (start + digits > len) return 0;
  uint32_t value = 0;
  for (size_t i = 0; i < digits; i++) {
    char c = raw[start + i];
    int d = base == 8 ? ((c >= '0' && c <= '7') ? c - '0' : -1)
                      : ini_hex_digit(c);
    if (d < 0) return 0;
    value = value * (uint32_t) base + (uint32_t) d;
  }
  if (!value) return 0;                       /* No NUL, by any spelling. */
  if (base == 8 && value > 0xFF) return 0;    /* `\400` is data. */
  if (value > 0x10FFFF) return 0;             /* Above Unicode, so data. */
  char buf[4];
  size_t n;
  if (kind == 'x' || base == 8) {
    /* A byte, not a codepoint: `\xC3` is one byte 0xC3, not U+00C3. */
    buf[0] = (char) value;
    n = 1;
  }
  else {
    n = ini_utf8_encode(value, buf);
  }
  if (decoded) memcpy(decoded, buf, n);
  *decoded_len = n;
  return (start + digits) - at;
}

ini_continuation gtext_ini_continuation_at(const GTEXT_INI_Dialect * dialect,
    const char * bytes, size_t len, size_t at) {
  ini_continuation r;
  memset(&r, 0, sizeof(r));
  if (dialect->continuation == GTEXT_INI_CONTINUATION_NONE) return r;
  if (at >= len || bytes[at] != '\\') return r;
  size_t term = gtext_ini_terminator_len(dialect, bytes, len, at + 1);
  if (!term) return r;
  size_t after = at + 1 + term;
  if (dialect->continuation == GTEXT_INI_CONTINUATION_JOIN_EMPTY) {
    /*
     * git: the backslash and its terminator, and nothing else. git does **not**
     * skip a comment block here - a `#` after a continuation is joined on as
     * comment text, which its own scanner then swallows to the end of the line.
     */
    r.span = 1 + term;
    r.ends_line = after >= len;
    return r;
  }
  /*
   * systemd: skip any run of comment lines, then look at what follows. Measured -
   * a continuation jumps over a `#` or `;` block and joins with whatever comes
   * after it, but a **blank** line ends the continuation rather than being
   * skipped. The blank line is left unconsumed for the parser to handle where it
   * handles every other one.
   */
  for (;;) {
    if (after >= len) {
      r.span = after - at;
      r.ends_line = true;
      return r;
    }
    size_t line_end = 0;
    ini_line_kind kind =
        gtext_ini_classify_line(dialect, bytes, len, after, &line_end);
    if (kind == INI_LINE_COMMENT) {
      after = line_end;
      continue;
    }
    r.span = after - at;
    r.ends_line = (kind == INI_LINE_BLANK);
    return r;
  }
}

/**
 * Decode escapes from @p raw into a fresh buffer.
 *
 * @param extra An additional letter to accept, or 0. gtext_ini_value_list()
 *   passes `;`, because Desktop Entry §4 defines `\;` only inside a list and a
 *   bare `a\;b` is refused by g_key_file_get_string().
 */
static GTEXT_INI_Status ini_decode(const GTEXT_INI_Dialect * dialect,
    const char * raw, size_t raw_len, const GTEXT_Allocator * alloc,
    char extra, char ** out, size_t * out_len) {
  if (!dialect || !out) return GTEXT_INI_E_INVALID;
  if (!raw && raw_len) return GTEXT_INI_E_INVALID;
  *out = NULL;
  if (out_len) *out_len = 0;
  if (dialect->utf8_values && !ini_utf8_ok(raw ? raw : "", raw_len)) {
    return GTEXT_INI_E_BAD_UNICODE;
  }
  /* The decoded form is never longer than the raw form: every escape is two
   * bytes in and one out. So one allocation, sized once, and no growth. */
  char * buf = gtext_allocator_malloc(alloc, raw_len + 1);
  if (!buf) return GTEXT_INI_E_OOM;
  size_t w = 0;
  for (size_t i = 0; i < raw_len; i++) {
    /*
     * A dialect with **no escape set at all** has no escape syntax, so a backslash
     * is an ordinary byte - which is what ini_value.h promises ("a dialect with no
     * escapes copies the bytes") and what EditorConfig needs: its specification
     * defines no escaping mechanism, and its conformance suite asserts that
     * `value \; not comment` keeps the backslash.
     *
     * This arm was unreachable until that dialect arrived. Every earlier one had a
     * non-empty set, so a backslash always reached the lookup below and a
     * no-escape dialect would have had every backslash refused as
     * GTEXT_INI_E_BAD_ESCAPE - the documented behaviour with nothing able to
     * exercise it.
     */
    if (raw[i] != '\\' || (!dialect->escapes && !extra)) {
      buf[w++] = raw[i];
      continue;
    }
    if (i + 1 >= raw_len) {
      /* A trailing lone backslash names no sequence. */
      gtext_allocator_free(alloc, buf);
      return GTEXT_INI_E_BAD_ESCAPE;
    }
    if (dialect->numeric_escapes) {
      char decoded[4];
      size_t decoded_len = 0;
      size_t used = ini_numeric_escape(raw, raw_len, i, decoded, &decoded_len);
      if (used) {
        memcpy(buf + w, decoded, decoded_len);
        w += decoded_len;
        i += used - 1;
        continue;
      }
      char next = raw[i + 1];
      if (next == 'x' || next == 'u' || next == 'U' ||
          (next >= '0' && next <= '9')) {
        /*
         * It looks like a numeric escape and is not one, which systemd treats the
         * same way as an unknown letter: the setting is discarded. Falling through
         * to the letter lookup would ask whether `x`, `u`, `U` or a digit is in the
         * escape set, which is a different question with the same answer only by
         * accident.
         */
        gtext_allocator_free(alloc, buf);
        return GTEXT_INI_E_BAD_ESCAPE;
      }
    }
    char letter = raw[i + 1];
    if (extra && letter == extra) {
      buf[w++] = extra;
      i++;
      continue;
    }
    if (!ini_escape_allowed(dialect, letter)) {
      gtext_allocator_free(alloc, buf);
      return GTEXT_INI_E_BAD_ESCAPE;
    }
    buf[w++] = ini_escape_byte(letter);
    i++;
  }
  buf[w] = '\0';
  *out = buf;
  if (out_len) *out_len = w;
  return GTEXT_INI_OK;
}

bool gtext_ini_dialect_scans_values(const GTEXT_INI_Dialect * dialect) {
  /*
   * Asked as a capability rather than as `id == GTEXT_INI_DIALECT_GIT_CONFIG`,
   * so that a caller who switches one of these fields on a dialect of their own
   * gets the scanner rather than a value silently measured the simple way. A
   * `== SOME_DIALECT` test in shared code is a bug waiting for the second
   * dialect that needs the same behaviour.
   */
  return dialect->numeric_escapes || dialect->quoted_values ||
         dialect->inline_comments ||
         (dialect->continuation != GTEXT_INI_CONTINUATION_NONE &&
             dialect->continuation != GTEXT_INI_CONTINUATION_INDENT);
}

bool gtext_ini_names_may_continue(const GTEXT_INI_Dialect * dialect) {
  /*
   * Only a continuation the *line's own bytes* announce can extend a name, and
   * that is the distinction this function exists to draw. A trailing backslash is
   * there to be found wherever the scan happens to be - in a group header, in a
   * key, in a value - so systemd's continuation reaches all three. An indent
   * continuation is a property of the **next** line and needs an entry to already
   * be open, so it can only ever extend a value.
   */
  return dialect->continuation == GTEXT_INI_CONTINUATION_JOIN_EMPTY ||
         dialect->continuation == GTEXT_INI_CONTINUATION_JOIN_SPACE;
}

size_t gtext_ini_indent_width(const GTEXT_INI_Dialect * dialect,
    const char * bytes, size_t len, size_t at) {
  size_t i = at;
  while (i < len && !gtext_ini_terminator_len(dialect, bytes, len, i) &&
         gtext_ini_is_space(dialect, bytes[i])) {
    i++;
  }
  return i - at;
}

/**
 * Whether the trailing whitespace a join strips includes @p c.
 *
 * Python's `str.rstrip()`, which the reference applies to the joined value, strips
 * the line terminators as well - and gtext_ini_is_space() deliberately does not,
 * because a line ends at its terminator before anything trims. So the join needs
 * its own predicate rather than reusing that one, and this is the only place in
 * the module where an LF counts as whitespace.
 */
static bool ini_join_trailing_space(const GTEXT_INI_Dialect * dialect, char c) {
  return c == '\n' || c == '\r' || gtext_ini_is_space(dialect, c);
}

/**
 * Classify one line of a raw indent-continued value.
 *
 * The **first** line is content whatever it looks like, and that is the whole
 * reason this wrapper exists. A raw value starts in the middle of its line - after
 * the key and the separator - so a value of `;black` begins with a comment
 * introducer and is not a comment: the reference tests for one on the whole line,
 * `j = ;black`, which does not start with `;`. Only a continuation line is ever
 * classified.
 *
 * Found by the local corpus, which is the point of having one: not one of the 102
 * probe documents had a value beginning with `;` or `#`, and 331 of the 479 real
 * files the reference reads do - Midnight Commander's skins spell a default colour
 * that way. Without the guard those values came back **empty** and the documents
 * were unwritable.
 */
static ini_line_kind ini_value_line_kind(const GTEXT_INI_Dialect * dialect,
    const char * raw, size_t len, size_t at, bool first, size_t * line_end) {
  ini_line_kind kind = gtext_ini_classify_line(dialect, raw, len, at, line_end);
  return first ? INI_LINE_CONTENT : kind;
}

void gtext_ini_join_indent(const GTEXT_INI_Dialect * dialect, const char * raw,
    size_t len, char * out, size_t * out_len) {
  size_t w = 0;
  size_t i = 0;
  bool first = true;
  while (i < len) {
    size_t line_end = 0;
    ini_line_kind kind =
        ini_value_line_kind(dialect, raw, len, i, first, &line_end);
    size_t content_end = line_end;
    while (content_end > i &&
           gtext_ini_terminator_len(dialect, raw, len, content_end - 1)) {
      content_end--;
    }
    if (kind == INI_LINE_COMMENT) {
      /* Contributes nothing at all - not even an empty line. The reference
       * appends to the value only when the line has no comment on it, which is
       * how a `#` line between two continuation lines vanishes while a blank line
       * between them becomes a newline. */
      i = line_end;
      continue;
    }
    /* Each piece is stripped on both sides before the join, the first included:
     * the reference strips the whole line and then strips the value again. */
    size_t start = i;
    while (start < content_end && gtext_ini_is_space(dialect, raw[start])) {
      start++;
    }
    size_t end = content_end;
    while (end > start && gtext_ini_is_space(dialect, raw[end - 1])) end--;
    if (!first) {
      if (out) out[w] = '\n';
      w++;
    }
    if (out && end > start) memcpy(out + w, raw + start, end - start);
    w += end - start;
    first = false;
    i = line_end;
  }
  /*
   * The reference's final `rstrip()`. It can only bite on a value whose last
   * contributing line is blank, which a parse does not produce - the span ends at
   * the last line that contributed text - but a caller may hand one in, and then
   * this is what configparser would have returned.
   *
   * Skipped when measuring, because there is nothing to look back at. The trim
   * only ever shortens, so a measured length stays the upper bound a caller
   * sizing a buffer needs.
   */
  if (out) {
    while (w && ini_join_trailing_space(dialect, out[w - 1])) w--;
  }
  *out_len = w;
}

bool gtext_ini_indent_value_ok(const GTEXT_INI_Dialect * dialect,
    const char * value, size_t len, bool verbatim) {
  if (!len) return true;
  /*
   * Two emissions and therefore two questions, which is why @p verbatim is here.
   *
   *   - **Verbatim**: the writer emits these bytes unchanged, so the value has to
   *     carry its own indentation - which a parse guarantees, because the span was
   *     assembled from lines that were indented more deeply than the entry's.
   *   - **Synthesized**: gtext_ini_write() indents each line after the first, so
   *     the value must *not* carry one. A leading whitespace byte on a
   *     continuation line would survive the injected indent and then be stripped
   *     on the way back in.
   *
   * The two are exact opposites on that one point and identical on every other,
   * so one function with a flag rather than two that could drift.
   */
  size_t i = 0;
  bool first = true;
  size_t last_content_end = 0;
  while (i < len) {
    size_t line_end = 0;
    ini_line_kind kind =
        ini_value_line_kind(dialect, value, len, i, first, &line_end);
    size_t content_end = line_end;
    while (content_end > i &&
           gtext_ini_terminator_len(dialect, value, len, content_end - 1)) {
      content_end--;
    }
    if (!verbatim && line_end > content_end) {
      /*
       * **A synthesized value's line breaks must be exactly what the writer will
       * emit**, and the writer emits one terminator of its own choosing per break -
       * so a terminator in the value that is not that one is rewritten, and the value
       * does not come back.
       *
       * Two failures, both reachable only through the builder API, which is why
       * neither the corpus nor the differential could see them: `"a<CR>b"` was emitted
       * verbatim and the document then **failed to re-parse at all**, because a CR
       * ends a line here and `b` is an entry with no separator; and `"a<CR><LF>b"` was
       * emitted with the terminator replaced, so the value came back `"a<LF>b"`.
       *
       * The test is on terminator length rather than on the byte, so a dialect where a
       * CR is *data* - one with neither `accept_crlf` nor `lone_cr_terminates` - keeps
       * it, which is the difference between refusing what cannot be spelled and
       * refusing what merely looks dangerous.
       */
      if (line_end - content_end != 1 || value[content_end] != '\n') return false;
    }
    if (!first && kind == INI_LINE_CONTENT) {
      size_t indent = gtext_ini_indent_width(dialect, value, len, i);
      /*
       * Indented, or re-reading would read the line as an entry of its own; and
       * for a synthesized value *not* indented, because the writer supplies that.
       */
      if (verbatim ? indent == 0 : indent != 0) return false;
      /* A trailing whitespace byte on any line but the last is inside the span
       * and survives; on the last line it is stripped. That is the final check
       * below, so nothing more is needed per line here. */
    }
    if (!first && kind == INI_LINE_COMMENT) {
      /*
       * A `#` or `;` line inside the span is fine verbatim - the reader skips it
       * and the bytes stay in the value - but a synthesized one would be given an
       * indent and then read as a comment, so the line would vanish. Measured:
       * an indented `#` is a comment and never a continuation.
       */
      if (!verbatim) return false;
    }
    if (kind == INI_LINE_CONTENT) last_content_end = content_end;
    first = false;
    i = line_end;
  }
  /*
   * The value must end at its last content line. Trailing blank or comment lines
   * are dropped on the way back in, so a value carrying them would come back
   * shorter - and a value that is *only* blank and comment lines would come back
   * empty.
   */
  if (last_content_end != len) return false;
  /*
   * The first line's own leading run is eaten after the separator, and the last line's
   * trailing run is stripped. Both would come back missing.
   *
   * **Unless the first byte begins a line terminator**, which is not leading
   * whitespace at all: a value whose first line is empty starts at its own terminator,
   * and under CRLF that first byte is a CR - which every dialect accepting CRLF calls
   * whitespace. Without the guard `alpha =<CRLF>  value` was read correctly and then
   * declared unwritable, so a document this module had just parsed could not be
   * written back. The LF case never showed it, because an LF is whitespace to no
   * dialect here.
   */
  if (!gtext_ini_terminator_len(dialect, value, len, 0) &&
      gtext_ini_is_space(dialect, value[0])) {
    return false;
  }
  if (gtext_ini_is_space(dialect, value[len - 1])) return false;
  return true;
}

/** Whether @p c introduces a comment for @p dialect. */
static bool ini_comment_intro(const GTEXT_INI_Dialect * dialect, char c) {
  return (c == '#' && dialect->comment_hash) ||
         (c == ';' && dialect->comment_semicolon);
}

GTEXT_INI_Status gtext_ini_scan_value(const GTEXT_INI_Dialect * dialect,
    const char * raw, size_t len, char * out, size_t * out_len,
    ini_value_scan * scan) {
  if (!dialect || !scan) return GTEXT_INI_E_INVALID;
  if (!raw && len) return GTEXT_INI_E_INVALID;
  memset(scan, 0, sizeof(*scan));
  if (out_len) *out_len = 0;

  bool quote = false;
  bool comment = false;
  size_t w = 0;         /* Bytes written to `out`. */
  size_t kept = 0;      /* `w` as of the last content byte - the decoded length. */
  size_t content = 0;   /* Input offset just past the last content byte. */
  size_t i = 0;

  while (i < len) {
    char c = raw[i];

    /*
     * The terminator ends the logical line. A CR is part of it only when an LF
     * follows, which is git's get_next_char() exactly; any other CR is a byte,
     * and a whitespace one at that.
     */
    size_t term = gtext_ini_terminator_len(dialect, raw, len, i);
    if (term) {
      if (quote) {
        scan->open_quote = true;
        scan->fault_offset = i;
        return GTEXT_INI_E_BAD_LINE;
      }
      scan->logical_len = i;
      scan->term_len = term;
      scan->content_len = content;
      if (out_len) *out_len = kept;
      return GTEXT_INI_OK;
    }

    /* Comment text runs to the terminator, and swallows everything - including
     * a backslash that would otherwise have continued the line. */
    if (comment) {
      i++;
      continue;
    }

    if (!quote && gtext_ini_is_space(dialect, c)) {
      /*
       * Written but not counted. A run before any content is dropped outright,
       * and a run after the last content byte is dropped by `kept` not having
       * advanced - which is how one pass drops both the leading and the trailing
       * run without ever scanning backwards. An interior run survives because
       * the next content byte advances `kept` past it.
       */
      if (kept) {
        if (out) out[w] = c;
        w++;
      }
      i++;
      continue;
    }

    if (!quote && dialect->inline_comments && ini_comment_intro(dialect, c)) {
      comment = true;
      i++;
      continue;
    }

    if (c == '"' && dialect->quoted_values) {
      /*
       * A toggle, not a wrapper: `x" mid "y` is one value of `x mid y`. The
       * quote is content for the purpose of `content`, so that the stored span
       * includes the closing one - a span ending just after the last *inner*
       * byte would re-scan as an unterminated run.
       */
      quote = !quote;
      content = i + 1;
      i++;
      continue;
    }

    if (c == '\\') {
      if (i + 1 >= len) {
        /*
         * End of input right after a backslash. git reports EOF as a newline, so
         * this is a continuation onto nothing: the backslash disappears and the
         * value ends. Measured - `k = one\` with no terminator is `one`.
         */
        if (quote) {
          scan->open_quote = true;
          scan->fault_offset = i;
          return GTEXT_INI_E_BAD_LINE;
        }
        /*
         * End of input counts as the newline, so this is a continuation too and
         * fixes the content boundary the same way: `k = false   \` at end of
         * input keeps its spaces.
         */
        kept = w;
        content = i + 1;
        scan->logical_len = len;
        scan->term_len = 0;
        scan->content_len = content;
        scan->trailing_backslash = true;
        if (out_len) *out_len = kept;
        return GTEXT_INI_OK;
      }
      char letter = raw[i + 1];
      ini_continuation cont = gtext_ini_continuation_at(dialect, raw, len, i);
      if (cont.span && cont.ends_line &&
          dialect->continuation == GTEXT_INI_CONTINUATION_JOIN_SPACE) {
        /*
         * systemd: a blank line after a continuation ends the logical line, and the
         * backslash **disappears**. So it is part of the terminator run rather than
         * part of the value - `A=W1  \` before a blank line is the value `W1`, with
         * the spaces trimmed and the backslash in `eol`.
         *
         * Spelling it that way rather than as a trailing backslash is what keeps a
         * parsed document writable: a value ending in a bare backslash is refused by
         * the writer, because a terminator written after it would turn it into a
         * continuation - and a document this module just read must always write
         * back. The bytes still tile, so the rewrite is byte-identical either way;
         * only the *value* differs, and this is the one that round-trips.
         *
         * git does not come here: its continuation neither skips a comment block nor
         * stops at a blank line, and its backslash-at-end-of-input case is measured
         * to fix the trailing-whitespace boundary instead - which the branch below
         * does.
         */
        if (quote) {
          scan->open_quote = true;
          scan->fault_offset = i;
          return GTEXT_INI_E_BAD_LINE;
        }
        scan->logical_len = i;
        scan->term_len = cont.span;
        scan->content_len = content;
        if (out_len) *out_len = kept;
        return GTEXT_INI_OK;
      }
      if (cont.span &&
          dialect->continuation == GTEXT_INI_CONTINUATION_JOIN_SPACE) {
        /*
         * **The backslash becomes a space**, measured by minimal pair: with the
         * continued line flush left, `Environment="W1\` then `W2"` is the single
         * word `W1 W2` - hex 5731205732. Every example in `systemd.syntax(7)`
         * indents the second line, where joining with a space and joining with
         * nothing are indistinguishable.
         *
         * The injected space is written on the same terms as any other whitespace -
         * dropped before the first content byte, kept between two - and then the
         * boundary is fixed the way the JOIN_EMPTY branch below fixes it, so the
         * stored span reaches past the continuation and re-scans to the same value.
         */
        if (kept) {
          if (out) out[w] = ' ';
          w++;
        }
        kept = w;
        content = i + cont.span;
        i += cont.span;
        continue;
      }
      if (dialect->continuation == GTEXT_INI_CONTINUATION_JOIN_EMPTY) {
        size_t span = cont.span;
        if (span) {
          /*
           * **A continuation marks everything written so far as content**, which
           * is measured and is not what a first reading of the rule suggests:
           * `k = false   \` keeps its three trailing spaces, while the same value
           * without the backslash has them trimmed. So the join is not merely
           * "skip these bytes" - it fixes the trailing-whitespace boundary at the
           * point it occurs.
           *
           * Found by the differential at 20,000 documents and not at 500, on a
           * document carrying both a trailing-space value and a continuation. The
           * two axes had to meet in one document for it to show.
           */
          kept = w;
          content = i + span;
          i += span;
          continue;
        }
        /*
         * A backslash before a *lone* CR is not a continuation, and is not an
         * escape either - git has no `\r`. Measured: refused, both at end of
         * input and with a byte following. So it falls through to the escape
         * lookup below and is reported there, which is the right error rather
         * than a convenient one.
         */
      }
      if (!dialect->escapes_in_grammar) {
        /*
         * The escape set is not the grammar's here, so a backslash is an ordinary
         * content byte and the sequence is decoded - or refused - by
         * gtext_ini_unescape() and gtext_ini_value_words(). Treating it as content
         * is also what makes the trailing-whitespace rule come out right without
         * knowing the set: `A=a\s   ` keeps `a\s` because the backslash and the `s`
         * are both content, and the three spaces after them are not.
         */
        if (out) out[w] = c;
        w++;
        kept = w;
        content = i + 1;
        i++;
        continue;
      }
      if (dialect->numeric_escapes) {
        char decoded[4];
        size_t decoded_len = 0;
        size_t used = ini_numeric_escape(raw, len, i, out ? decoded : NULL,
            &decoded_len);
        if (used) {
          if (out) memcpy(out + w, decoded, decoded_len);
          w += decoded_len;
          kept = w;
          content = i + used;
          i += used;
          continue;
        }
        char next = raw[i + 1];
        if (next == 'x' || next == 'u' || next == 'U' ||
            (next >= '0' && next <= '9')) {
          scan->bad_escape = true;
          scan->fault_offset = i;
          return GTEXT_INI_E_BAD_ESCAPE;
        }
      }
      if (!ini_escape_allowed(dialect, letter)) {
        scan->bad_escape = true;
        scan->fault_offset = i;
        return GTEXT_INI_E_BAD_ESCAPE;
      }
      if (out) out[w] = ini_escape_byte(letter);
      w++;
      kept = w;
      content = i + 2;
      i += 2;
      continue;
    }

    if (out) out[w] = c;
    w++;
    kept = w;
    content = i + 1;
    i++;
  }

  /* End of input behaves as a final newline, so an open run is still an error. */
  if (quote) {
    scan->open_quote = true;
    scan->fault_offset = len;
    return GTEXT_INI_E_BAD_LINE;
  }
  scan->logical_len = len;
  scan->term_len = 0;
  scan->content_len = content;
  if (out_len) *out_len = kept;
  return GTEXT_INI_OK;
}

GTEXT_INI_Status gtext_ini_unescape(const GTEXT_INI_Dialect * dialect,
    const char * raw, size_t raw_len, const GTEXT_Allocator * alloc,
    char ** out, size_t * out_len) {
  if (!dialect || !out) return GTEXT_INI_E_INVALID;
  if (!raw && raw_len) return GTEXT_INI_E_INVALID;
  if (!alloc) alloc = gtext_allocator_default();
  if (dialect->continuation == GTEXT_INI_CONTINUATION_INDENT) {
    /*
     * The join, and then the escape pass over its result - the same two-layer
     * shape the scanning dialects have below and in the same order, for the same
     * reason: what a continuation joined over is a line break and not the input to
     * an escape. configparser has no escapes at all, so the second pass is a copy;
     * it runs anyway rather than being skipped, so that a caller who adds an escape
     * set to a copy of the dialect gets both layers instead of neither.
     *
     * This is the non-scanning branch because the *extent* of an indent-continued
     * value is not discoverable from the value's own bytes - it needs the indent of
     * the line the entry started on, which only the parser knows. The bytes stored
     * are already exactly the value's, so the join has nothing to find.
     */
    char stack[512];
    char * buf = stack;
    char * heap = NULL;
    if (raw_len > sizeof(stack)) {
      heap = gtext_allocator_malloc(alloc, raw_len ? raw_len : 1);
      if (!heap) return GTEXT_INI_E_OOM;
      buf = heap;
    }
    size_t joined_len = 0;
    gtext_ini_join_indent(dialect, raw ? raw : "", raw_len, buf, &joined_len);
    GTEXT_INI_Status status =
        ini_decode(dialect, buf, joined_len, alloc, 0, out, out_len);
    if (heap) gtext_allocator_free(alloc, heap);
    return status;
  }
  if (!gtext_ini_dialect_scans_values(dialect)) {
    /*
     * The Win32 wrapper strip, and it belongs here rather than in the parser for
     * the reason §5 of the design gives: the stored bytes are the document's, and
     * what a value *means* is the accessor's question. A caller who wants the
     * quotes can read gtext_ini_entry_value() and get them.
     *
     * Four measured rules, and all four are what separate a wrapper from
     * ::GTEXT_INI_Dialect::quoted_values' toggle: exactly one pair comes off
     * (`""x""` is `"x"`), both ends must be quotes (`"x` and `x"` are
     * themselves), the two must be the *same* quote (`"x'` is unchanged), and the
     * value has already been trimmed, so `"  x  "` keeps its blanks.
     */
    if (dialect->strip_wrapping_quotes && raw_len >= 2 &&
        (raw[0] == '"' || raw[0] == '\'') && raw[raw_len - 1] == raw[0]) {
      raw++;
      raw_len -= 2;
    }
    return ini_decode(dialect, raw, raw_len, alloc, 0, out, out_len);
  }
  /*
   * A dialect whose values are scanned decodes with the same function the parser
   * measured with, over the bytes the parser stored. Anything else would be a
   * second implementation of the quoting, comment and continuation rules, and
   * the two would disagree about some value without either of them crashing.
   *
   * **No UTF-8 validation on this path**, and that is the dialect's call rather
   * than an oversight: ::GTEXT_INI_Dialect::utf8_values is false for git,
   * because `git config --get` hands back a value containing a bare 0xFF
   * unchanged. Validating here would refuse a value the reference returns.
   */
  *out = NULL;
  if (out_len) *out_len = 0;
  if (dialect->utf8_values && !ini_utf8_ok(raw ? raw : "", raw_len)) {
    return GTEXT_INI_E_BAD_UNICODE;
  }
  char * buf = gtext_allocator_malloc(alloc, raw_len + 1);
  if (!buf) return GTEXT_INI_E_OOM;
  ini_value_scan scan;
  size_t decoded_len = 0;
  GTEXT_INI_Status status =
      gtext_ini_scan_value(dialect, raw, raw_len, buf, &decoded_len, &scan);
  if (status != GTEXT_INI_OK) {
    gtext_allocator_free(alloc, buf);
    return status;
  }
  buf[decoded_len] = '\0';
  if (!dialect->escapes_in_grammar && (dialect->escapes ||
          dialect->numeric_escapes)) {
    /*
     * Two passes, because the two jobs belong to different layers. The scan found
     * the value's extent and joined its continuations - the line's structure - and
     * left every backslash alone; this decodes the escape set, which is the
     * caller's question. systemd is the dialect that needs both, and the order
     * matters: a continuation inside `A=a\` + `tb` joins to `a tb` and is *not* a
     * tab, which is what systemd produces.
     */
    char * decoded = NULL;
    size_t final_len = 0;
    GTEXT_INI_Status second =
        ini_decode(dialect, buf, decoded_len, alloc, 0, &decoded, &final_len);
    gtext_allocator_free(alloc, buf);
    if (second != GTEXT_INI_OK) return second;
    *out = decoded;
    if (out_len) *out_len = final_len;
    return GTEXT_INI_OK;
  }
  *out = buf;
  if (out_len) *out_len = decoded_len;
  return GTEXT_INI_OK;
}

GTEXT_INI_Status gtext_ini_escape(const GTEXT_INI_Dialect * dialect,
    const char * text, size_t text_len, const GTEXT_Allocator * alloc,
    char ** out, size_t * out_len) {
  if (!dialect || !out) return GTEXT_INI_E_INVALID;
  if (!text && text_len) return GTEXT_INI_E_INVALID;
  if (!alloc) alloc = gtext_allocator_default();
  *out = NULL;
  if (out_len) *out_len = 0;
  char * buf = gtext_allocator_malloc(alloc, text_len * 2 + 1);
  if (!buf) return GTEXT_INI_E_OOM;
  size_t w = 0;
  for (size_t i = 0; i < text_len; i++) {
    char c = text[i];
    /*
     * A leading space has to be escaped and an interior one does not: §3.3
     * discards the run after `=`, so a value that begins with a space would
     * read back shorter. Everything else here has no other spelling at all.
     */
    bool must = (c == '\n' || c == '\r' || c == '\t' || c == '\\') ||
                (c == ' ' && i == 0);
    if (!must) {
      buf[w++] = c;
      continue;
    }
    char letter = c == '\n'   ? 'n'
                  : c == '\r' ? 'r'
                  : c == '\t' ? 't'
                  : c == '\\' ? '\\'
                              : 's';
    if (!ini_escape_allowed(dialect, letter)) {
      /* The dialect cannot spell this byte, and emitting it raw would produce a
       * document that reads back as something else. */
      gtext_allocator_free(alloc, buf);
      return GTEXT_INI_E_UNREPRESENTABLE;
    }
    buf[w++] = '\\';
    buf[w++] = letter;
  }
  buf[w] = '\0';
  *out = buf;
  if (out_len) *out_len = w;
  return GTEXT_INI_OK;
}

void gtext_ini_string_free(const GTEXT_Allocator * alloc, char * text) {
  if (!text) return;
  gtext_allocator_free(alloc ? alloc : gtext_allocator_default(), text);
}

struct GTEXT_INI_List {
  const GTEXT_Allocator * alloc;
  ini_str * items;
  size_t count;
};

GTEXT_INI_Status gtext_ini_value_list(const GTEXT_INI_Dialect * dialect,
    const char * raw, size_t raw_len, const GTEXT_Allocator * alloc,
    GTEXT_INI_List ** out) {
  if (!dialect || !out) return GTEXT_INI_E_INVALID;
  if (!raw && raw_len) return GTEXT_INI_E_INVALID;
  *out = NULL;
  char sep = dialect->list_separator;
  if (!sep) return GTEXT_INI_E_INVALID;
  if (!alloc) alloc = gtext_allocator_default();

  /*
   * §4: items are separated by the separator, the value "may be optionally
   * terminated by" one, and "trailing empty strings must always be terminated
   * with a semicolon". So `a;b` and `a;b;` are both two items, and `a;b;;` is
   * three with the last empty: one trailing separator is the terminator and a
   * second one introduces an empty item. Counting the separators and then
   * dropping *one* trailing empty item is what produces that, and it is the
   * asymmetry that makes a naive split wrong.
   */
  size_t count = 1;
  for (size_t i = 0; i < raw_len; i++) {
    if (raw[i] == '\\') {
      i++;
      continue;
    }
    if (raw[i] == sep) count++;
  }
  if (raw_len && raw[raw_len - 1] == sep &&
      (raw_len < 2 || raw[raw_len - 2] != '\\')) {
    count--; /* The terminator, not an empty item. */
  }
  if (!raw_len) count = 0;

  GTEXT_INI_List * list = gtext_allocator_calloc(alloc, 1, sizeof(*list));
  if (!list) return GTEXT_INI_E_OOM;
  list->alloc = alloc;
  list->count = count;
  if (count) {
    list->items = gtext_allocator_calloc(alloc, count, sizeof(*list->items));
    if (!list->items) {
      gtext_allocator_free(alloc, list);
      return GTEXT_INI_E_OOM;
    }
  }

  size_t index = 0;
  size_t start = 0;
  GTEXT_INI_Status status = GTEXT_INI_OK;
  for (size_t i = 0; i <= raw_len && index < count; i++) {
    if (i < raw_len && raw[i] == '\\') {
      i++;
      continue;
    }
    if (i != raw_len && raw[i] != sep) continue;
    char * decoded = NULL;
    size_t decoded_len = 0;
    status = ini_decode(dialect, raw + start, i - start, alloc, sep, &decoded,
        &decoded_len);
    if (status != GTEXT_INI_OK) break;
    list->items[index].data = decoded;
    list->items[index].len = decoded_len;
    index++;
    start = i + 1;
  }
  if (status != GTEXT_INI_OK) {
    gtext_ini_list_free(list);
    return status;
  }
  list->count = index;
  *out = list;
  return GTEXT_INI_OK;
}

/**
 * Append one word to a list, growing it. Takes ownership of @p data.
 *
 * The list's capacity is its `count` rounded up by doubling, tracked by the caller
 * through @p capacity, because ::GTEXT_INI_List has no capacity field - the list
 * gtext_ini_value_list() builds knows its length before it starts and this one does
 * not.
 */
static bool ini_words_push(GTEXT_INI_List * list, size_t * capacity, char * data,
    size_t len) {
  if (list->count == *capacity) {
    size_t want = *capacity ? *capacity * 2 : 8;
    ini_str * grown = gtext_allocator_realloc(list->alloc, list->items,
        want * sizeof(*grown));
    if (!grown) return false;
    list->items = grown;
    *capacity = want;
  }
  list->items[list->count].data = data;
  list->items[list->count].len = len;
  list->count++;
  return true;
}

GTEXT_INI_Status gtext_ini_value_words(const GTEXT_INI_Dialect * dialect,
    const char * raw, size_t raw_len, const GTEXT_Allocator * alloc,
    GTEXT_INI_List ** out) {
  if (!dialect || !out) return GTEXT_INI_E_INVALID;
  if (!raw && raw_len) return GTEXT_INI_E_INVALID;
  *out = NULL;
  if (!dialect->word_split) return GTEXT_INI_E_INVALID;
  if (!alloc) alloc = gtext_allocator_default();

  GTEXT_INI_List * list = gtext_allocator_calloc(alloc, 1, sizeof(*list));
  if (!list) return GTEXT_INI_E_OOM;
  list->alloc = alloc;
  size_t capacity = 0;

  /*
   * One word at a time into `buf`, which is sized once for the whole value: a word
   * is never longer than what it was made from, since every step here either copies
   * a byte or shortens a run.
   */
  char * buf = gtext_allocator_malloc(alloc, raw_len + 1);
  if (!buf) {
    gtext_ini_list_free(list);
    return GTEXT_INI_E_OOM;
  }
  size_t w = 0;
  bool in_word = false;
  char quote = 0; /* The quote character of the open run, or 0. */
  GTEXT_INI_Status status = GTEXT_INI_OK;
  size_t i = 0;

  while (i < raw_len) {
    char c = raw[i];
    ini_continuation cont =
        gtext_ini_continuation_at(dialect, raw, raw_len, i);
    if (cont.span) {
      /*
       * The join, using the same primitive the parser and the scanner use. Inside a
       * quoted run the injected space is content; outside one it ends the word,
       * which is the same thing ordinary whitespace does.
       */
      if (quote) {
        buf[w++] = ' ';
      }
      else if (in_word) {
        char * word = gtext_allocator_malloc(alloc, w + 1);
        if (!word) { status = GTEXT_INI_E_OOM; break; }
        memcpy(word, buf, w);
        word[w] = 0;
        if (!ini_words_push(list, &capacity, word, w)) {
          gtext_allocator_free(alloc, word);
          status = GTEXT_INI_E_OOM;
          break;
        }
        w = 0;
        in_word = false;
      }
      i += cont.span;
      continue;
    }
    if (!quote && gtext_ini_is_space(dialect, c)) {
      if (in_word) {
        char * word = gtext_allocator_malloc(alloc, w + 1);
        if (!word) { status = GTEXT_INI_E_OOM; break; }
        memcpy(word, buf, w);
        word[w] = 0;
        if (!ini_words_push(list, &capacity, word, w)) {
          gtext_allocator_free(alloc, word);
          status = GTEXT_INI_E_OOM;
          break;
        }
        w = 0;
        in_word = false;
      }
      i++;
      continue;
    }
    if (c == '"' || c == '\'') {
      /*
       * A toggle, not a wrapper - `x"y z"` is one word `xy z` - and each quote
       * character is ordinary inside a run opened by the other. Opening one starts a
       * word even if it closes immediately, which is how `""` becomes an empty word.
       */
      if (!quote) {
        quote = c;
        in_word = true;
        i++;
        continue;
      }
      if (quote == c) {
        quote = 0;
        i++;
        continue;
      }
      /* The other quote character, inside this run: data. */
    }
    if (c == '\\' && i + 1 >= raw_len) {
      /*
       * A lone backslash at the very end of the value: **dropped**, measured -
       * `Environment=A\` is the single word `A`. It reaches here because the parser
       * stores it: the scanner's end-of-input branch counts it as content so that
       * the stored span and the document's bytes still agree, and dropping it is the
       * value layer's job rather than the line layer's.
       */
      i++;
      continue;
    }
    if (c == '\\' && i + 1 < raw_len) {
      /* Escapes are decoded inside a single-quoted run too, unlike a shell. */
      if (dialect->numeric_escapes) {
        char decoded[4];
        size_t decoded_len = 0;
        size_t used = ini_numeric_escape(raw, raw_len, i, decoded, &decoded_len);
        if (used) {
          memcpy(buf + w, decoded, decoded_len);
          w += decoded_len;
          in_word = true;
          i += used;
          continue;
        }
        char next = raw[i + 1];
        if (next == 'x' || next == 'u' || next == 'U' ||
            (next >= '0' && next <= '9')) {
          /* Malformed, so the setting is refused - see ini_numeric_escape(). */
          status = GTEXT_INI_E_BAD_ESCAPE;
          break;
        }
      }
      if (!ini_escape_allowed(dialect, raw[i + 1])) {
        status = GTEXT_INI_E_BAD_ESCAPE;
        break;
      }
      buf[w++] = ini_escape_byte(raw[i + 1]);
      in_word = true;
      i += 2;
      continue;
    }
    buf[w++] = c;
    in_word = true;
    i++;
  }

  if (status == GTEXT_INI_OK && quote) {
    /* systemd reports "Invalid syntax" and discards the whole setting. */
    status = GTEXT_INI_E_BAD_LINE;
  }
  if (status == GTEXT_INI_OK && in_word) {
    char * word = gtext_allocator_malloc(alloc, w + 1);
    if (!word) {
      status = GTEXT_INI_E_OOM;
    }
    else {
      memcpy(word, buf, w);
      word[w] = 0;
      if (!ini_words_push(list, &capacity, word, w)) {
        gtext_allocator_free(alloc, word);
        status = GTEXT_INI_E_OOM;
      }
    }
  }
  gtext_allocator_free(alloc, buf);
  if (status != GTEXT_INI_OK) {
    gtext_ini_list_free(list);
    return status;
  }
  *out = list;
  return GTEXT_INI_OK;
}

size_t gtext_ini_list_count(const GTEXT_INI_List * list) {
  return list ? list->count : 0;
}

const char * gtext_ini_list_at(const GTEXT_INI_List * list, size_t index,
    size_t * len) {
  if (len) *len = 0;
  if (!list || index >= list->count) return NULL;
  if (len) *len = list->items[index].len;
  return list->items[index].data;
}

void gtext_ini_list_free(GTEXT_INI_List * list) {
  if (!list) return;
  const GTEXT_Allocator * alloc = list->alloc;
  for (size_t i = 0; i < list->count; i++) {
    if (list->items[i].data) gtext_allocator_free(alloc, list->items[i].data);
  }
  if (list->items) gtext_allocator_free(alloc, list->items);
  gtext_allocator_free(alloc, list);
}

/** Whether [raw, raw+len) is exactly @p word. */
static bool ini_word_is(const char * raw, size_t len, const char * word) {
  size_t n = strlen(word);
  return len == n && memcmp(raw, word, n) == 0;
}

/** Whether @p raw is @p word, folding case when @p fold. */
static bool ini_word_is_maybe_folded(const char * raw, size_t raw_len,
    const char * word, bool fold) {
  if (!fold) return ini_word_is(raw, raw_len, word);
  size_t n = strlen(word);
  if (raw_len != n) return false;
  for (size_t i = 0; i < n; i++) {
    char c = raw[i];
    if (c >= 'A' && c <= 'Z') c = (char) (c - 'A' + 'a');
    if (c != word[i]) return false;
  }
  return true;
}

GTEXT_INI_Status gtext_ini_value_bool(const GTEXT_INI_Dialect * dialect,
    const char * raw, size_t raw_len, bool * out) {
  if (!dialect || !out || (!raw && raw_len)) return GTEXT_INI_E_INVALID;
  /*
   * §4: "must either be the string true or false". Nothing else, and no case
   * folding - §3 says case is significant everywhere in the file. systemd's set is
   * wider and is also case-sensitive; configparser's is the same eight words as
   * systemd's and is **not**, because `getboolean()` lower-cases the value before
   * looking it up in `BOOLEAN_STATES`. Measured: `YES` and `TRUE` are accepted
   * there and refused by systemd, and `n`, `t` and `2` are refused by both.
   */
  bool fold = dialect->bool_style == GTEXT_INI_BOOLS_CONFIGPARSER;
  bool wide = dialect->bool_style == GTEXT_INI_BOOLS_SYSTEMD ||
              dialect->bool_style == GTEXT_INI_BOOLS_CONFIGPARSER;
  if (ini_word_is_maybe_folded(raw, raw_len, "true", fold)) {
    *out = true;
    return GTEXT_INI_OK;
  }
  if (ini_word_is_maybe_folded(raw, raw_len, "false", fold)) {
    *out = false;
    return GTEXT_INI_OK;
  }
  if (wide) {
    static const char * const yes[] = {"1", "yes", "on"};
    static const char * const no[] = {"0", "no", "off"};
    for (size_t i = 0; i < sizeof(yes) / sizeof(*yes); i++) {
      if (ini_word_is_maybe_folded(raw, raw_len, yes[i], fold)) {
        *out = true;
        return GTEXT_INI_OK;
      }
      if (ini_word_is_maybe_folded(raw, raw_len, no[i], fold)) {
        *out = false;
        return GTEXT_INI_OK;
      }
    }
  }
  return GTEXT_INI_E_TYPE;
}

/** Copy into a small stack buffer, or the heap when the value is long. */
static char * ini_dup_for_strtod(const char * raw, size_t raw_len,
    char * stack, size_t stack_size, char ** heap) {
  *heap = NULL;
  char * text = stack;
  if (raw_len + 1 > stack_size) {
    /* A number has no bound on how it may be *written* - `0.0000...1` is a
     * valid spelling of a small double - so the buffer cannot be sized for a
     * shortest form. This is the defect the TOML reader had. */
    *heap = gtext_allocator_malloc(gtext_allocator_default(), raw_len + 1);
    if (!*heap) return NULL;
    text = *heap;
  }
  if (raw_len) memcpy(text, raw, raw_len);
  text[raw_len] = '\0';
  return text;
}

GTEXT_INI_Status gtext_ini_value_int(const char * raw, size_t raw_len,
    int64_t * out) {
  if (!out || (!raw && raw_len)) return GTEXT_INI_E_INVALID;
  if (!raw_len) return GTEXT_INI_E_TYPE;
  size_t i = 0;
  bool negative = false;
  if (raw[0] == '-' || raw[0] == '+') {
    negative = raw[0] == '-';
    i = 1;
  }
  if (i >= raw_len) return GTEXT_INI_E_TYPE;
  uint64_t magnitude = 0;
  for (; i < raw_len; i++) {
    if (raw[i] < '0' || raw[i] > '9') return GTEXT_INI_E_TYPE;
    unsigned digit = (unsigned) (raw[i] - '0');
    /* Checked before the multiply, so the overflow never happens rather than
     * being detected after the fact - signed overflow is undefined and an
     * unsigned wrap would make a large value look small. */
    if (magnitude > (UINT64_C(0xFFFFFFFFFFFFFFFF) - digit) / 10) {
      return GTEXT_INI_E_RANGE;
    }
    magnitude = magnitude * 10 + digit;
  }
  if (negative) {
    if (magnitude > (uint64_t) INT64_MAX + 1) return GTEXT_INI_E_RANGE;
    if (magnitude == (uint64_t) INT64_MAX + 1) {
      *out = INT64_MIN;
      return GTEXT_INI_OK;
    }
    *out = -(int64_t) magnitude;
    return GTEXT_INI_OK;
  }
  if (magnitude > (uint64_t) INT64_MAX) return GTEXT_INI_E_RANGE;
  *out = (int64_t) magnitude;
  return GTEXT_INI_OK;
}

GTEXT_INI_Status gtext_ini_value_double(const char * raw, size_t raw_len,
    double * out) {
  if (!out || (!raw && raw_len)) return GTEXT_INI_E_INVALID;
  if (!raw_len) return GTEXT_INI_E_TYPE;
  /*
   * `%f` skips leading whitespace and this does not, deliberately. The parser
   * has already removed the run after `=`, so a stored value never begins with
   * one; accepting it here would accept a spelling no document can produce, and
   * would disagree with gtext_ini_value_int() about the same bytes.
   */
  if (ini_is_space_byte(raw[0])) return GTEXT_INI_E_TYPE;
  /* §4 defines numeric by `%f` in the **C locale**. strtod reads LC_NUMERIC, so
   * a process in a comma-decimal locale would otherwise read `1.5` as 1 and
   * accept `1,5` as one and a half. gtext_text_strtod_c() is the library's
   * locale-independent conversion; see LOCALE-INDEPENDENT-NUMBERS. */
  char stack[64];
  char * heap = NULL;
  char * text = ini_dup_for_strtod(raw, raw_len, stack, sizeof(stack), &heap);
  if (!text) return GTEXT_INI_E_OOM;
  char * end = NULL;
  double value = gtext_number_strtod(text, &end);
  GTEXT_INI_Status status = GTEXT_INI_OK;
  if (end == text || (size_t) (end - text) != raw_len) {
    status = GTEXT_INI_E_TYPE;
  }
  else if (value != value || value > 1.7976931348623157e308 ||
           value < -1.7976931348623157e308) {
    /* `inf` and `nan` are things `%f` reads and a desktop entry has no use for;
     * reporting them as a type error keeps a value the writer cannot spell back
     * out of the tree. An overflowing spelling is a range error. */
    status = errno == ERANGE ? GTEXT_INI_E_RANGE : GTEXT_INI_E_TYPE;
  }
  if (heap) gtext_allocator_free(gtext_allocator_default(), heap);
  if (status == GTEXT_INI_OK) *out = value;
  return status;
}

/** One candidate spelling of a localized key, built into @p buf. */
static const char * ini_locale_try(const GTEXT_INI_Group * group,
    const char * key, size_t key_len, const char * lang, size_t lang_len,
    const char * country, size_t country_len, const char * modifier,
    size_t modifier_len, size_t * len) {
  char buf[256];
  size_t need = key_len + 1 + lang_len + (country_len ? 1 + country_len : 0) +
                (modifier_len ? 1 + modifier_len : 0) + 1 + 1;
  if (need > sizeof(buf)) return NULL; /* No real locale is this long. */
  size_t w = 0;
  memcpy(buf + w, key, key_len);
  w += key_len;
  buf[w++] = '[';
  memcpy(buf + w, lang, lang_len);
  w += lang_len;
  if (country_len) {
    buf[w++] = '_';
    memcpy(buf + w, country, country_len);
    w += country_len;
  }
  if (modifier_len) {
    buf[w++] = '@';
    memcpy(buf + w, modifier, modifier_len);
    w += modifier_len;
  }
  buf[w++] = ']';
  buf[w] = '\0';
  return gtext_ini_group_get(group, buf, len);
}

const char * gtext_ini_group_get_locale(const GTEXT_INI_Group * group,
    const char * key, const char * locale, size_t * len) {
  if (len) *len = 0;
  if (!group || !key) return NULL;
  size_t key_len = strlen(key);
  if (!locale) {
    /* POSIX precedence. A caller wanting none of it passes "". */
    locale = getenv("LC_ALL");
    if (!locale || !*locale) locale = getenv("LC_MESSAGES");
    if (!locale || !*locale) locale = getenv("LANG");
    if (!locale) locale = "";
  }

  /* lang_COUNTRY.ENCODING@MODIFIER, every part after lang optional. The
   * encoding is parsed only so that it can be ignored, which §5 requires. */
  size_t locale_len = strlen(locale);
  size_t lang_len = 0;
  while (lang_len < locale_len && locale[lang_len] != '_' &&
         locale[lang_len] != '.' && locale[lang_len] != '@') {
    lang_len++;
  }
  const char * country = NULL;
  size_t country_len = 0;
  const char * modifier = NULL;
  size_t modifier_len = 0;
  size_t i = lang_len;
  if (i < locale_len && locale[i] == '_') {
    country = locale + i + 1;
    i++;
    while (i < locale_len && locale[i] != '.' && locale[i] != '@') i++;
    country_len = (size_t) (locale + i - country);
  }
  if (i < locale_len && locale[i] == '.') {
    while (i < locale_len && locale[i] != '@') i++;
  }
  if (i < locale_len && locale[i] == '@') {
    modifier = locale + i + 1;
    modifier_len = locale_len - i - 1;
  }

  /*
   * §5's order exactly: lang_COUNTRY@MODIFIER, lang_COUNTRY, lang@MODIFIER,
   * lang, then the unpostfixed key. A candidate whose parts the locale does not
   * have is skipped rather than tried with an empty part - "if LC_MESSAGES does
   * not have a MODIFIER field, then no key with a modifier will be matched".
   */
  const char * found = NULL;
  if (lang_len) {
    if (country_len && modifier_len) {
      found = ini_locale_try(group, key, key_len, locale, lang_len, country,
          country_len, modifier, modifier_len, len);
    }
    if (!found && country_len) {
      found = ini_locale_try(group, key, key_len, locale, lang_len, country,
          country_len, NULL, 0, len);
    }
    if (!found && modifier_len) {
      found = ini_locale_try(group, key, key_len, locale, lang_len, NULL, 0,
          modifier, modifier_len, len);
    }
    if (!found) {
      found = ini_locale_try(group, key, key_len, locale, lang_len, NULL, 0,
          NULL, 0, len);
    }
  }
  if (!found) found = gtext_ini_group_get(group, key, len);
  return found;
}
