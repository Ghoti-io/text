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
 */
static bool ini_value_writable(const ini_str * value,
    const GTEXT_INI_Dialect * dialect, char next) {
  if (!value->len) return true;
  if (value->data[0] == ' ' || value->data[0] == '\t') return false;
  for (size_t i = 0; i < value->len; i++) {
    if (value->data[i] == '\n') return false;
  }
  if (dialect->accept_crlf && value->data[value->len - 1] == '\r' &&
      next == '\n') {
    return false;
  }
  return true;
}

GTEXT_INI_Status gtext_ini_write(const GTEXT_INI_Document * doc,
    GTEXT_INI_Sink * sink, const GTEXT_INI_Write_Options * opts) {
  if (!doc || !sink || !sink->write) return GTEXT_INI_E_INVALID;
  GTEXT_INI_Write_Options effective = opts ? *opts
                                          : gtext_ini_write_options_default();

  if (effective.emit_comments && doc->leading.data) {
    INI_TRY(ini_put(sink, doc->leading.data, doc->leading.len));
  }
  for (size_t g = 0; g < doc->count; g++) {
    const GTEXT_INI_Group * group = &doc->groups[g];
    if (effective.emit_comments && group->comment.data) {
      INI_TRY(ini_put(sink, group->comment.data, group->comment.len));
    }
    if (!effective.normalize && group->hdr_post.data) {
      /* The header as it was read: any leading whitespace, the brackets, and
       * whatever followed the `]` including the terminator. */
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
      /* What follows the value in the output decides whether a trailing CR is
       * safe, so the check has to know which branch below will run. */
      char next = '\0';
      if (!effective.normalize && entry->sep.data) {
        next = entry->eol.len ? entry->eol.data[0] : '\0';
      }
      else {
        next = effective.crlf ? '\r' : '\n';
      }
      if (!ini_value_writable(&entry->value, &doc->dialect, next)) {
        return GTEXT_INI_E_UNREPRESENTABLE;
      }
      if (!effective.normalize && entry->sep.data) {
        INI_TRY(ini_put(sink, entry->pre.data, entry->pre.len));
        INI_TRY(ini_put(sink, entry->key.data, entry->key.len));
        INI_TRY(ini_put(sink, entry->sep.data, entry->sep.len));
        INI_TRY(ini_put(sink, entry->value.data, entry->value.len));
        INI_TRY(ini_put(sink, entry->eol.data, entry->eol.len));
      }
      else {
        INI_TRY(ini_put(sink, entry->key.data, entry->key.len));
        INI_TRY(ini_put(sink, "=", 1));
        INI_TRY(ini_put(sink, entry->value.data, entry->value.len));
        INI_TRY(ini_put_eol(sink, &effective));
      }
    }
  }
  if (effective.emit_comments && doc->trailing.data) {
    INI_TRY(ini_put(sink, doc->trailing.data, doc->trailing.len));
  }
  return GTEXT_INI_OK;
}
