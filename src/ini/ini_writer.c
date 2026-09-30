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
 * @file ini_writer.c
 * @brief Writing an INI document back out, byte for byte where it can.
 *
 * Desktop Entry §3 requires that a rewrite preserve fields the implementation
 * does not understand and, for comments, that they "should be preserved across
 * reads and writes". The tree keeps each line in the pieces it was read in, so
 * meeting that is a matter of emitting them in order rather than of
 * reconstructing anything.
 */

#include "ini_internal.h"

/** A growable buffer behind gtext_ini_sink_buffer(). */
typedef struct {
  char * data;
  size_t len;
  size_t capacity;
} ini_sink_buffer;

static GTEXT_INI_Status ini_buffer_write(void * user, const char * bytes,
    size_t len) {
  ini_sink_buffer * buf = user;
  if (buf->len + len + 1 > buf->capacity) {
    size_t want = buf->capacity ? buf->capacity * 2 : 256;
    while (want < buf->len + len + 1) want *= 2;
    /* The default allocator, not a write option's: the sink is created before
     * any options exist and outlives the write. */
    char * grown =
        gtext_allocator_realloc(gtext_allocator_default(), buf->data, want);
    if (!grown) return GTEXT_INI_E_OOM;
    buf->data = grown;
    buf->capacity = want;
  }
  memcpy(buf->data + buf->len, bytes, len);
  buf->len += len;
  buf->data[buf->len] = '\0';
  return GTEXT_INI_OK;
}

GTEXT_INI_Status gtext_ini_sink_buffer(GTEXT_INI_Sink * sink) {
  if (!sink) return GTEXT_INI_E_INVALID;
  ini_sink_buffer * buf =
      gtext_allocator_calloc(gtext_allocator_default(), 1, sizeof(*buf));
  if (!buf) return GTEXT_INI_E_OOM;
  sink->write = ini_buffer_write;
  sink->user = buf;
  return GTEXT_INI_OK;
}

const char * gtext_ini_sink_buffer_data(const GTEXT_INI_Sink * sink) {
  if (!sink || sink->write != ini_buffer_write) return NULL;
  const ini_sink_buffer * buf = sink->user;
  return buf->data ? buf->data : "";
}

size_t gtext_ini_sink_buffer_size(const GTEXT_INI_Sink * sink) {
  if (!sink || sink->write != ini_buffer_write) return 0;
  const ini_sink_buffer * buf = sink->user;
  return buf->len;
}

void gtext_ini_sink_buffer_free(GTEXT_INI_Sink * sink) {
  if (!sink || sink->write != ini_buffer_write) return;
  ini_sink_buffer * buf = sink->user;
  if (buf->data) gtext_allocator_free(gtext_allocator_default(), buf->data);
  gtext_allocator_free(gtext_allocator_default(), buf);
  sink->write = NULL;
  sink->user = NULL;
}

GTEXT_INI_Write_Options gtext_ini_write_options_default(void) {
  GTEXT_INI_Write_Options o;
  memset(&o, 0, sizeof(o));
  o.allocator = NULL;
  o.normalize = false;
  o.emit_comments = true;
  o.crlf = false;
  return o;
}

/** Emit bytes, stopping at the first refusal. */
static GTEXT_INI_Status ini_put(GTEXT_INI_Sink * sink, const char * bytes,
    size_t len) {
  if (!len) return GTEXT_INI_OK;
  return sink->write(sink->user, bytes, len);
}

#define INI_TRY(expr)                                                          \
  do {                                                                         \
    GTEXT_INI_Status status_ = (expr);                                          \
    if (status_ != GTEXT_INI_OK) return status_;                                \
  } while (0)

/** The terminator this write uses. */
static GTEXT_INI_Status ini_put_eol(GTEXT_INI_Sink * sink,
    const GTEXT_INI_Write_Options * opts) {
  return ini_put(sink, opts->crlf ? "\r\n" : "\n", opts->crlf ? 2 : 1);
}

/**
 * Whether a value would read back as itself.
 *
 * Three ways it would not, and the third is why this needs to know what follows
 * the value on the line:
 *
 *   - an LF anywhere in the value ends the line early, under every dialect;
 *   - a leading space or tab comes back missing, because §3.3 discards the run
 *     after `=`, under every dialect;
 *   - a **trailing CR comes back missing only when an LF is written directly
 *     after it**, under a dialect that reads CRLF - because then the value's own
 *     CR becomes the terminator's. A trailing CR at end of file, or one followed
 *     by `\r\n` (where the terminator supplies its own CR), round-trips fine.
 *
 * An interior CR is always data: only the CR immediately before the LF is ever
 * part of a terminator.
 *
 * Getting this wrong in the safe direction is what the first version did -
 * refusing every CR - and it made a document this module reads unwritable. The
 * writer must refuse exactly the values that would read back differently, and
 * no others, or the caller cannot rewrite a file it just read.
 *
 * @param value The raw value.
 * @param dialect The dialect the document was read under.
 * @param next The first byte written after the value, or 0 for end of output.
 * @param verbatim Whether the value's own bytes are what will be emitted. Only
 *   ::GTEXT_INI_CONTINUATION_INDENT cares, and it cares a great deal: there the
 *   writer inserts the indentation a synthesized continuation needs, so the two
 *   emissions require opposite things of the value.
 */
static bool ini_value_writable(const ini_str * value,
    const GTEXT_INI_Dialect * dialect, char next, bool verbatim) {
  if (!value->len) return true;
  if (gtext_ini_dialect_scans_values(dialect)) {
    /*
     * For a dialect whose values are scanned, the question is not which bytes
     * are dangerous but whether re-reading these bytes gives them back. So ask
     * the scanner, which is the same code the parse used: the value is writable
     * exactly when the scan consumes all of it *as content*.
     *
     * That one test subsumes every case a hand-written list would have to
     * enumerate, and gets right several a plausible list would miss:
     *
     *   - a trailing space or an inline comment introducer, which would come
     *     back shorter, fail `content_len == len`;
     *   - a trailing backslash, which the scanner reads as a continuation onto
     *     nothing and so leaves outside the content, fails it too - and that is
     *     the byte that would otherwise silently swallow the terminator written
     *     after it;
     *   - an unbalanced quote fails the scan outright;
     *   - an **LF is fine** when a backslash precedes it, because that is a
     *     continuation and reads back as the same value. A check that refused
     *     every LF would make a document this module reads unwritable, which is
     *     the mistake the non-scanning branch below already made once.
     *
     * `next` is not consulted: any byte that could combine with what follows is
     * one the scan has already declined to count as content.
     */
    if (gtext_ini_is_space(dialect, value->data[0])) return false;
    ini_value_scan scan;
    if (gtext_ini_scan_value(dialect, value->data, value->len, NULL, NULL,
            &scan) != GTEXT_INI_OK) {
      return false;
    }
    if (scan.content_len != value->len || scan.logical_len != value->len) {
      return false;
    }
    /*
     * A value whose last byte is a bare backslash will pair with whichever
     * terminator is written after it and swallow the line beyond, so it is
     * writable only when nothing follows. A parse produces one only where the
     * document itself ran out, and there `eol` is empty; a caller who sets one on
     * a line that has a terminator is refused.
     *
     * A value that *contains* the newline its continuation joined over is not
     * this case and is writable: re-reading performs the same join and stops at
     * the next terminator.
     */
    /*
     * A value whose content *ends* with the newline a continuation joined over is
     * safe, and checking otherwise was a mistake made here once: writing it and
     * then a terminator gives `a\<LF><LF>`, whose re-read performs the same join,
     * stops at the second terminator, and stores the same raw bytes. It is only
     * the **bare** trailing backslash that is unsafe, because it has not yet
     * consumed a terminator and will take whichever one is written next.
     */
    return !scan.trailing_backslash || next == '\0';
  }
  /*
   * gtext_ini_is_space(), not a literal space-or-tab test. The parse skips the
   * dialect's whitespace after the `=`, so whatever that predicate calls
   * whitespace would be eaten on the way back in - a leading CR under the generic
   * dialect, a leading vertical tab under EditorConfig. The literal test said
   * those two were writable and they are not; a value written that way came back
   * shorter.
   */
  if (dialect->continuation == GTEXT_INI_CONTINUATION_INDENT) {
    /*
     * An LF is how this dialect *spells* a continuation, so the blanket refusal
     * below would make every multi-line configparser value unwritable - including
     * one this module had just read, which is the mistake the scanning branch above
     * records having made once. gtext_ini_indent_value_ok() asks the reader's own
     * line rules instead: it is not a list of forbidden bytes but a re-read of the
     * value, line by line.
     */
    return gtext_ini_indent_value_ok(dialect, value->data, value->len, verbatim);
  }
  if (gtext_ini_is_space(dialect, value->data[0])) return false;
  for (size_t i = 0; i < value->len; i++) {
    if (value->data[i] == '\n') return false;
  }
  /*
   * The trailing run, for a dialect that drops it. Desktop Entry keeps it, so
   * `k=v ` round-trips there and must stay writable; EditorConfig trims it, so
   * the same value would read back one byte shorter and is refused. This is the
   * ::GTEXT_INI_Dialect::trim_trailing_space flag being asked about a value rather
   * than about a parse, which is the only place the writer needs it.
   */
  if (dialect->trim_trailing_space &&
      gtext_ini_is_space(dialect, value->data[value->len - 1])) {
    return false;
  }
  if (dialect->accept_crlf && value->data[value->len - 1] == '\r' &&
      next == '\n') {
    return false;
  }
  return true;
}

/**
 * Emit a synthesized value under ::GTEXT_INI_CONTINUATION_INDENT, indenting each
 * line after the first so that reading it back gives the same value.
 *
 * One space is enough and one space is what this uses: the rule is "more indented
 * than the line the entry began on", and a synthesized entry's line begins at
 * column zero. An empty line in the middle is emitted **without** the indent,
 * because an indented empty line is still a blank line to the reader and the
 * bytes would be noise.
 *
 * Only reached for a value ini_value_writable() has already accepted, so there is
 * no line here that would come back different - a comment line, a line with its
 * own leading or trailing whitespace, and a trailing terminator are all refused
 * before this runs.
 */
static GTEXT_INI_Status ini_put_indented(GTEXT_INI_Sink * sink,
    const GTEXT_INI_Write_Options * opts, const ini_str * value) {
  size_t start = 0;
  bool first = true;
  while (start < value->len) {
    size_t i = start;
    while (i < value->len && value->data[i] != '\n') i++;
    size_t content_end = i;
    /* A CR before the LF belongs to the terminator this write chooses, not to the
     * line, so it is not re-emitted here. */
    if (content_end > start && value->data[content_end - 1] == '\r') {
      content_end--;
    }
    if (!first) {
      INI_TRY(ini_put_eol(sink, opts));
      if (content_end > start) INI_TRY(ini_put(sink, " ", 1));
    }
    INI_TRY(ini_put(sink, value->data + start, content_end - start));
    first = false;
    start = (i < value->len) ? i + 1 : i;
  }
  return GTEXT_INI_OK;
}

GTEXT_INI_Status gtext_ini_write(const GTEXT_INI_Document * doc,
    GTEXT_INI_Sink * sink, const GTEXT_INI_Write_Options * opts) {
  if (!doc || !sink || !sink->write) return GTEXT_INI_E_INVALID;
  GTEXT_INI_Write_Options effective = opts ? *opts
                                          : gtext_ini_write_options_default();

  /* Before anything, and not gated on emit_comments: a BOM is not a comment, and
   * a document that had one is not the same document without it. */
  if (doc->bom.data) {
    INI_TRY(ini_put(sink, doc->bom.data, doc->bom.len));
  }
  if (effective.emit_comments && doc->leading.data) {
    INI_TRY(ini_put(sink, doc->leading.data, doc->leading.len));
  }
  for (size_t g = 0; g < doc->count; g++) {
    const GTEXT_INI_Group * group = &doc->groups[g];
    if (effective.emit_comments && group->comment.data) {
      INI_TRY(ini_put(sink, group->comment.data, group->comment.len));
    }
    if (group->preamble) {
      /* Entries that came before any header. Writing a header here - even an
       * empty `[]` - would put a line in the output the document never had, and
       * `[]` is not even a legal header to the one dialect that has a preamble. */
    }
    else if (!effective.normalize && group->verbatim && group->hdr_post.data) {
      /* The header as it was read: any leading whitespace, the brackets, and
       * whatever followed the `]` including the terminator. The name is the
       * document's own spelling, so a git subsection comes back as
       * `[remote "orig in"]` rather than as its canonical `remote.orig in`. */
      INI_TRY(ini_put(sink, group->hdr_pre.data, group->hdr_pre.len));
      INI_TRY(ini_put(sink, "[", 1));
      INI_TRY(ini_put(sink, group->name.data, group->name.len));
      INI_TRY(ini_put(sink, "]", 1));
      INI_TRY(ini_put(sink, group->hdr_post.data, group->hdr_post.len));
    }
    else {
      INI_TRY(ini_put(sink, "[", 1));
      INI_TRY(ini_put(sink, group->name.data, group->name.len));
      INI_TRY(ini_put(sink, "]", 1));
      INI_TRY(ini_put_eol(sink, &effective));
    }
    for (size_t e = 0; e < group->count; e++) {
      const ini_entry * entry = &group->entries[e];
      if (effective.emit_comments && entry->comment.data) {
        INI_TRY(ini_put(sink, entry->comment.data, entry->comment.len));
      }
      bool verbatim = !effective.normalize && entry->verbatim;
      /* What follows the value in the output decides whether a trailing CR is
       * safe, so the check has to know which branch below will run. */
      char next = '\0';
      if (verbatim) {
        next = entry->eol.len ? entry->eol.data[0] : '\0';
      }
      else {
        next = effective.crlf ? '\r' : '\n';
      }
      if (entry->has_value &&
          !ini_value_writable(&entry->value, &doc->dialect, next, verbatim)) {
        return GTEXT_INI_E_UNREPRESENTABLE;
      }
      if (!entry->has_value && !doc->dialect.valueless_keys) {
        /*
         * Guards an invariant rather than a reachable input: `has_value` is false
         * only for a key the parser read with no `=`, and the dialect that parsed
         * it is the one stored on the document, so a dialect without the spelling
         * cannot be holding one. It is here because emitting `k=` would silently
         * change the entry from "true" to "the empty string", and because a fourth
         * dialect could make it reachable.
         */
        return GTEXT_INI_E_UNREPRESENTABLE;
      }
      if (verbatim) {
        INI_TRY(ini_put(sink, entry->pre.data, entry->pre.len));
        INI_TRY(ini_put(sink, entry->key.data, entry->key.len));
        if (entry->has_value) {
          INI_TRY(ini_put(sink, entry->sep.data, entry->sep.len));
          INI_TRY(ini_put(sink, entry->value.data, entry->value.len));
        }
        INI_TRY(ini_put(sink, entry->eol.data, entry->eol.len));
      }
      else {
        INI_TRY(ini_put(sink, entry->key.data, entry->key.len));
        if (entry->has_value) {
          /* The dialect's first separator, not a literal `=`: a dialect can have
           * more than one and the first is the one it writes. */
          INI_TRY(ini_put(sink, doc->dialect.separators, 1));
          if (doc->dialect.continuation == GTEXT_INI_CONTINUATION_INDENT) {
            INI_TRY(ini_put_indented(sink, &effective, &entry->value));
          }
          else {
            INI_TRY(ini_put(sink, entry->value.data, entry->value.len));
          }
        }
        INI_TRY(ini_put_eol(sink, &effective));
      }
    }
  }
  if (effective.emit_comments && doc->trailing.data) {
    INI_TRY(ini_put(sink, doc->trailing.data, doc->trailing.len));
  }
  return GTEXT_INI_OK;
}
