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

/** The sink and how many bytes have been given to it: what a rule about *where in the output* a name falls has to know, which only the emission can say. */
typedef struct {
  GTEXT_INI_Sink * sink;
  size_t total;
} ini_emit;

/** Emit bytes, stopping at the first refusal. */
static GTEXT_INI_Status ini_put(ini_emit * out, const char * bytes,
    size_t len) {
  if (!len) return GTEXT_INI_OK;
  GTEXT_INI_Status status = out->sink->write(out->sink->user, bytes, len);
  if (status == GTEXT_INI_OK) out->total += len;
  return status;
}

#define INI_TRY(expr)                                                          \
  do {                                                                         \
    GTEXT_INI_Status status_ = (expr);                                          \
    if (status_ != GTEXT_INI_OK) return status_;                                \
  } while (0)

/** The terminator this write uses. */
static GTEXT_INI_Status ini_put_eol(ini_emit * sink,
    const GTEXT_INI_Write_Options * opts) {
  return ini_put(sink, opts->crlf ? "\r\n" : "\n", opts->crlf ? 2 : 1);
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
static GTEXT_INI_Status ini_put_indented(ini_emit * sink,
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

/**
 * Whether a name's own first bytes are a UTF-8 BOM.
 *
 * A key or group name can hold one: `skip_bom` strips a BOM only at offset 0, so
 * ` <BOM>j=v` - a blank, then the BOM - is an entry whose key really is
 * `<BOM>j`, and that is the correct reading of those bytes.
 *
 * It matters to the **writer**, when such a name would be the first bytes of
 * the output (see ini_key_at_start_ok()), where a reader will strip the BOM and
 * give back a different name.
 */
static bool ini_bom_prefixed(const ini_str * name) {
  return name->len >= 3 && (unsigned char) name->data[0] == 0xEF &&
         (unsigned char) name->data[1] == 0xBB &&
         (unsigned char) name->data[2] == 0xBF;
}

/**
 * Whether a key may be emitted as the next bytes of the output.
 *
 * A key can begin with the bytes of a UTF-8 BOM (` <BOM>j=v` is an entry whose key is `<BOM>j`, because a reader skips a BOM only at offset 0), and
 * a reader that skips one **does** skip it when the key is the first thing in the file, so such a key written there reads back as the key without it.
 * Found by the fuzzer at 663,648 executions on the four-byte input ` <BOM>` under the Win32 dialect, whose keys need no separator; the generic
 * dialect had the same defect for as long as it had `skip_bom`.
 *
 * Whether the key is the first thing in the file is what the emission knows and nothing computed beforehand does: the first version refused such a
 * key wherever it sat, as a normalized write, because working out whether it would land at offset 0 meant a second copy of this function's emission
 * order, and two copies of an order are how the two drift. Counting the bytes already given to the sink is not a copy: it is the emission itself,
 * and it covers every way a key comes to be first (an entry before any header, the comments left out, the blanks of a normalized line dropped).
 * A document that carries its own BOM has written it first, so the key is not at offset 0 and is writable.
 */
static GTEXT_INI_Status ini_key_at_start_ok(const GTEXT_INI_Document * doc,
    const ini_emit * out, const ini_str * key) {
  if (out->total == 0 && doc->dialect.skip_bom && ini_bom_prefixed(key)) {
    return GTEXT_INI_E_UNREPRESENTABLE;
  }
  return GTEXT_INI_OK;
}

GTEXT_INI_Status gtext_ini_write(const GTEXT_INI_Document * doc,
    GTEXT_INI_Sink * user_sink, const GTEXT_INI_Write_Options * opts) {
  if (!doc || !user_sink || !user_sink->write) return GTEXT_INI_E_INVALID;
  ini_emit emit = {user_sink, 0};
  ini_emit * sink = &emit;
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
  /*
   * **The dialect the output is read under, not the one the document was read under.** `crlf` is an option of the write, and a dialect that does not
   * accept a CR before an LF (Desktop Entry: "Desktop Entry §3 specifies linefeed") cannot read what it is asked to write; the output is read by a
   * reader that does, and that reader takes a CR after the `=` for blank and a CR before an LF for the terminator's. Asking whether a value is
   * writable under the document's own dialect said `\r` was fine as a Desktop Entry value, wrote `a=<CR><CR><LF>` and read back an empty value: the
   * same question has to be asked of the reader that will be given the bytes.
   */
  GTEXT_INI_Dialect read_dialect = doc->dialect;
  if (effective.crlf) read_dialect.accept_crlf = true;

  /*
   * A document the parser would refuse is not one to produce. Desktop Entry §5 requires the bare form of a key that has a locale postfix in the same
   * group, and the parser refuses the document that lacks it (an entry made with set() is accepted whatever the group holds, because the bare key may
   * come after it). The same function asks the question here and there.
   */
  if (doc->dialect.require_unlocalized_key && doc->dialect.locale_postfix) {
    for (size_t g = 0; g < doc->count; g++) {
      if (gtext_ini_group_unlocalized_key_missing(&doc->groups[g]) != SIZE_MAX) {
        return GTEXT_INI_E_UNREPRESENTABLE;
      }
    }
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
          !gtext_ini_value_writable(&read_dialect, entry->value.data, entry->value.len, next, verbatim)) {
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
        INI_TRY(ini_key_at_start_ok(doc, sink, &entry->key));
        INI_TRY(ini_put(sink, entry->key.data, entry->key.len));
        if (entry->has_value) {
          INI_TRY(ini_put(sink, entry->sep.data, entry->sep.len));
          INI_TRY(ini_put(sink, entry->value.data, entry->value.len));
        }
        INI_TRY(ini_put(sink, entry->eol.data, entry->eol.len));
      }
      else {
        INI_TRY(ini_key_at_start_ok(doc, sink, &entry->key));
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
