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
 * @file toml_writer.c
 * @brief Writing a TOML document, and the sinks it writes through.
 *
 * Two walks, both on the heap. gtext_toml_free() is off the C stack because a
 * document this library parsed must always be releasable; the same argument
 * applies to writing it, and more sharply - the tree in hand came from here,
 * so a writer that recursed would crash on input the parser advertised as
 * accepted.
 */

#include "../text_number_internal.h"
#include "toml_internal.h"
#include <math.h>
#include <string.h>

/*==========================================================================*
 * Sinks
 *
 * The state is opaque: the accessors are the interface, and a caller with a
 * struct definition would be able to reach past them. Which kind of sink a
 * GTEXT_TOML_Sink is, is decided by comparing its write function rather than
 * by a tag inside the state, so an accessor called on a caller's own sink
 * answers "not mine" instead of reading a struct that is not there.
 *==========================================================================*/

/** A buffer sink's bookkeeping, growable or fixed. */
typedef struct {
  char * data;
  size_t size; ///< Bytes of `data`. For a fixed sink, the caller's size.
  size_t used;
  bool truncated;
} toml_sink_state;

static GTEXT_TOML_Status toml_sink_grow_write(
    void * user, const char * bytes, size_t len) {
  toml_sink_state * st = user;
  if (!st) return GTEXT_TOML_E_WRITE;
  if (st->used + len + 1 < st->used) return GTEXT_TOML_E_OOM;
  if (st->used + len + 1 > st->size) {
    size_t want = st->size ? st->size : 256;
    while (want < st->used + len + 1) {
      if (want > SIZE_MAX / 2) return GTEXT_TOML_E_OOM;
      want *= 2;
    }
    char * grown = gtext_allocator_realloc(NULL, st->data, want);
    if (!grown) return GTEXT_TOML_E_OOM;
    st->data = grown;
    st->size = want;
  }
  memcpy(st->data + st->used, bytes, len);
  st->used += len;
  /* Terminated for convenience, never counted. A TOML document may contain a
   * NUL, so gtext_toml_sink_buffer_size() is the authority. */
  st->data[st->used] = '\0';
  return GTEXT_TOML_OK;
}

static GTEXT_TOML_Status toml_sink_fixed_write(
    void * user, const char * bytes, size_t len) {
  toml_sink_state * st = user;
  if (!st) return GTEXT_TOML_E_WRITE;
  if (st->truncated) return GTEXT_TOML_E_WRITE;
  if (len > st->size - st->used) {
    size_t room = st->size - st->used;
    if (room) memcpy(st->data + st->used, bytes, room);
    st->used += room;
    st->truncated = true;
    return GTEXT_TOML_E_WRITE;
  }
  memcpy(st->data + st->used, bytes, len);
  st->used += len;
  return GTEXT_TOML_OK;
}

GTEXT_TOML_Status gtext_toml_sink_buffer(GTEXT_TOML_Sink * sink) {
  if (!sink) return GTEXT_TOML_E_INVALID;
  memset(sink, 0, sizeof(*sink));
  toml_sink_state * st = gtext_allocator_calloc(NULL, 1, sizeof(*st));
  if (!st) return GTEXT_TOML_E_OOM;
  sink->write = toml_sink_grow_write;
  sink->user = st;
  return GTEXT_TOML_OK;
}

const char * gtext_toml_sink_buffer_data(const GTEXT_TOML_Sink * sink) {
  if (!sink || sink->write != toml_sink_grow_write || !sink->user) return NULL;
  const toml_sink_state * st = sink->user;
  /* Nothing written yet means no buffer yet; answer the empty string rather
   * than NULL, so a caller printing the result of a write that produced an
   * empty document - which an empty root table does - needs no special case. */
  return st->data ? st->data : "";
}

size_t gtext_toml_sink_buffer_size(const GTEXT_TOML_Sink * sink) {
  if (!sink || sink->write != toml_sink_grow_write || !sink->user) return 0;
  return ((const toml_sink_state *) sink->user)->used;
}

void gtext_toml_sink_buffer_free(GTEXT_TOML_Sink * sink) {
  if (!sink || sink->write != toml_sink_grow_write) return;
  toml_sink_state * st = sink->user;
  if (st) {
    gtext_allocator_free(NULL, st->data);
    gtext_allocator_free(NULL, st);
  }
  memset(sink, 0, sizeof(*sink));
}

GTEXT_TOML_Status gtext_toml_sink_fixed_buffer(
    GTEXT_TOML_Sink * sink, char * buffer, size_t size) {
  if (!sink) return GTEXT_TOML_E_INVALID;
  memset(sink, 0, sizeof(*sink));
  if (!buffer || size == 0) return GTEXT_TOML_E_INVALID;
  toml_sink_state * st = gtext_allocator_calloc(NULL, 1, sizeof(*st));
  if (!st) return GTEXT_TOML_E_OOM;
  st->data = buffer;
  st->size = size;
  sink->write = toml_sink_fixed_write;
  sink->user = st;
  return GTEXT_TOML_OK;
}

size_t gtext_toml_sink_fixed_buffer_used(const GTEXT_TOML_Sink * sink) {
  if (!sink || sink->write != toml_sink_fixed_write || !sink->user) return 0;
  return ((const toml_sink_state *) sink->user)->used;
}

bool gtext_toml_sink_fixed_buffer_truncated(const GTEXT_TOML_Sink * sink) {
  if (!sink || sink->write != toml_sink_fixed_write || !sink->user) {
    return false;
  }
  return ((const toml_sink_state *) sink->user)->truncated;
}

void gtext_toml_sink_fixed_buffer_free(GTEXT_TOML_Sink * sink) {
  if (!sink || sink->write != toml_sink_fixed_write) return;
  /* The caller's buffer is the caller's. */
  gtext_allocator_free(NULL, sink->user);
  memset(sink, 0, sizeof(*sink));
}

/*==========================================================================*
 * Spelling values
 *==========================================================================*/

/** What a write is doing, threaded through every part of it. */
typedef struct {
  GTEXT_TOML_Sink * sink;
  const GTEXT_Allocator * alloc;
  const GCHRON_WriteOptions * datetime;
  GTEXT_TOML_Table_Style style;
  GTEXT_TOML_Status status;
  toml_buf path;    ///< The current `[header]` path, already quoted.
  toml_buf scratch; ///< One key or one scalar at a time.
  bool wrote_any;   ///< Whether a blank line is wanted before the next header.
} toml_wctx;

/** Send bytes to the sink, remembering the first failure. */
static bool wemit(toml_wctx * w, const char * bytes, size_t len) {
  if (w->status != GTEXT_TOML_OK) return false;
  if (!len) return true;
  GTEXT_TOML_Status s = w->sink->write(w->sink->user, bytes, len);
  if (s != GTEXT_TOML_OK) {
    /* A sink that refuses without saying why is a write failure; a sink that
     * names a code keeps it, which is what lets E_OOM inside a sink stay
     * distinguishable from a full destination. */
    w->status = s;
    return false;
  }
  return true;
}

static bool wemitz(toml_wctx * w, const char * text) {
  return wemit(w, text, strlen(text));
}

/** Append to a scratch buffer, remembering the first failure. */
static bool badd(toml_wctx * w, toml_buf * out, const char * bytes, size_t len) {
  if (w->status != GTEXT_TOML_OK) return false;
  if (!toml_buf_append(w->alloc, out, bytes, len)) {
    w->status = GTEXT_TOML_E_OOM;
    return false;
  }
  return true;
}

static bool baddz(toml_wctx * w, toml_buf * out, const char * text) {
  return badd(w, out, text, strlen(text));
}

/**
 * Spell a basic string, escapes and all.
 *
 * v1.0.0, String: a basic string may hold any Unicode character except the
 * quotation mark, the backslash, and the control characters other than tab -
 * U+0000 to U+0008, U+000A to U+001F, and U+007F. Everything else is written
 * through as the UTF-8 it already is.
 *
 * The UTF-8 is checked while it is copied, and a bad sequence refuses the
 * write. A writer that passed it through would produce a document nothing can
 * read, including this library, and would do it without saying anything.
 */
static bool spell_string(
    toml_wctx * w, toml_buf * out, const char * bytes, size_t len) {
  if (!badd(w, out, "\"", 1)) return false;
  size_t i = 0;
  while (i < len) {
    unsigned char c = (unsigned char) bytes[i];
    const char * escape = NULL;
    switch (c) {
      case '"': escape = "\\\""; break;
      case '\\': escape = "\\\\"; break;
      case '\b': escape = "\\b"; break;
      case '\t': escape = "\\t"; break;
      case '\n': escape = "\\n"; break;
      case '\f': escape = "\\f"; break;
      case '\r': escape = "\\r"; break;
      default: break;
    }
    if (escape) {
      if (!baddz(w, out, escape)) return false;
      ++i;
      continue;
    }
    if (c < 0x20 || c == 0x7F) {
      /* The five without a short escape, U+0000 among them: a TOML string may
       * hold a NUL and this is how it is written. Upper-case hex digits, as
       * the specification's own examples use. */
      static const char hex[] = "0123456789ABCDEF";
      char seq[6] = {'\\', 'u', '0', '0', hex[0], hex[0]};
      seq[4] = hex[(c >> 4) & 0xF];
      seq[5] = hex[c & 0xF];
      if (!badd(w, out, seq, sizeof(seq))) return false;
      ++i;
      continue;
    }
    if (c < 0x80) {
      if (!badd(w, out, bytes + i, 1)) return false;
      ++i;
      continue;
    }
    size_t width = 0;
    if (!toml_utf8_next(bytes, len, i, NULL, &width)) {
      w->status = GTEXT_TOML_E_BAD_UNICODE;
      return false;
    }
    if (!badd(w, out, bytes + i, width)) return false;
    i += width;
  }
  return badd(w, out, "\"", 1);
}

/**
 * Spell a key: bare where TOML allows it, quoted otherwise.
 *
 * v1.0.0, Keys: a bare key may contain only letters A-Z and a-z, digits,
 * underscores and dashes, and may not be empty. Anything else - a dot, a
 * space, a non-ASCII letter, the empty key - becomes a quoted key, which is
 * a basic string and so goes through exactly the same escaping.
 */
static bool spell_key(
    toml_wctx * w, toml_buf * out, const char * key, size_t len) {
  bool bare = len > 0;
  for (size_t i = 0; bare && i < len; ++i) {
    char c = key[i];
    bare = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9') || c == '_' || c == '-';
  }
  if (bare) return badd(w, out, key, len);
  return spell_string(w, out, key, len);
}

/**
 * Spell a float so that reading it back gives the same double.
 *
 * The shortest `%g` precision that round-trips, found by trying and reading
 * back rather than by assuming 17: `%.17g` is always lossless and writes
 * `0.10000000000000001` for a value whose shortest lossless spelling is `0.1`.
 * gtext_number_format_double() and gtext_number_strtod() rather than snprintf
 * and strtod, because both of those read LC_NUMERIC and a document's decimal
 * point is the format's, not the user's.
 */
static bool spell_float(toml_wctx * w, toml_buf * out, double d) {
  if (isnan(d)) {
    /* TOML spells `nan` and `-nan`, so the sign bit is kept rather than
     * flattened. signbit(), because every comparison against a NaN is false. */
    return baddz(w, out, signbit(d) ? "-nan" : "nan");
  }
  if (isinf(d)) return baddz(w, out, d < 0 ? "-inf" : "inf");

  char text[64];
  int len = -1;
  for (int precision = 1; precision <= 17; ++precision) {
    int n = gtext_number_format_double(
        text, sizeof(text), d, GTEXT_NUMBER_GENERAL, precision);
    if (n < 0 || (size_t) n >= sizeof(text)) continue;
    char * end = NULL;
    if (gtext_number_strtod(text, &end) == d && end && *end == '\0') {
      len = n;
      break;
    }
  }
  if (len < 0) {
    /* Unreachable for an IEEE 754 double, where 17 significant digits always
     * round-trip; kept because "always" here is a property of the platform's
     * printf and not of this file. */
    int n = gtext_number_format_double(
        text, sizeof(text), d, GTEXT_NUMBER_GENERAL, 17);
    if (n < 0 || (size_t) n >= sizeof(text)) {
      w->status = GTEXT_TOML_E_WRITE;
      return false;
    }
    len = n;
  }

  bool fractional = false;
  for (int i = 0; i < len; ++i) {
    if (text[i] == '.' || text[i] == 'e' || text[i] == 'E') {
      fractional = true;
      break;
    }
  }
  if (!badd(w, out, text, (size_t) len)) return false;
  /* v1.0.0, Float: a float is an integer part followed by a fraction, an
   * exponent, or both. `%g` spells 1.0 as `1`, which is a valid TOML
   * *integer* - so a document written that way reads back with a different
   * type, and the corpus has cases (valid/float/zero) that say so. */
  if (!fractional) return baddz(w, out, ".0");
  return true;
}

static bool spell_scalar(
    toml_wctx * w, toml_buf * out, const GTEXT_TOML_Value * value) {
  char text[80];
  switch (value->type) {
    case GTEXT_TOML_STRING:
      return spell_string(w, out, value->as.string.data, value->as.string.len);
    case GTEXT_TOML_INTEGER: {
      int n = gtext_number_format_i64(text, sizeof(text), value->as.integer);
      if (n < 0 || (size_t) n >= sizeof(text)) {
        w->status = GTEXT_TOML_E_WRITE;
        return false;
      }
      return badd(w, out, text, (size_t) n);
    }
    case GTEXT_TOML_FLOAT:
      return spell_float(w, out, value->as.floating);
    case GTEXT_TOML_BOOLEAN:
      return baddz(w, out, value->as.boolean ? "true" : "false");
    case GTEXT_TOML_DATETIME: {
      size_t n = 0;
      if (gchron_write_toml(&value->as.datetime, w->datetime, text,
              sizeof(text), &n)
          != GCHRON_OK) {
        /* A date-time chron will not spell. Reachable only through the
         * builder, which takes a GCHRON_TomlValue as it stands rather than
         * deciding here whether TOML can write it - a refusal with the key in
         * hand is more use than one at construction. */
        w->status = GTEXT_TOML_E_DATETIME;
        return false;
      }
      return badd(w, out, text, n);
    }
    default:
      /* A container reached the scalar path, which is a defect in this file
       * rather than in anything a caller did. */
      w->status = GTEXT_TOML_E_STATE;
      return false;
  }
}

/** Spell one scalar into the scratch buffer and send it. */
static bool emit_scalar(toml_wctx * w, const GTEXT_TOML_Value * value) {
  w->scratch.len = 0;
  if (!spell_scalar(w, &w->scratch, value)) return false;
  return wemit(w, w->scratch.data, w->scratch.len);
}

static bool emit_key(toml_wctx * w, const char * key, size_t len) {
  w->scratch.len = 0;
  if (!spell_key(w, &w->scratch, key, len)) return false;
  return wemit(w, w->scratch.data, w->scratch.len);
}

static bool is_container(const GTEXT_TOML_Value * value) {
  return value->type == GTEXT_TOML_TABLE || value->type == GTEXT_TOML_ARRAY;
}

/*==========================================================================*
 * The inline walk: `{ }` and `[ ]`, on an explicit stack
 *==========================================================================*/

typedef struct {
  const GTEXT_TOML_Value * node;
  size_t index;
} toml_iframe;

typedef struct {
  toml_iframe * frames;
  size_t count;
  size_t capacity;
} toml_istack;

static bool istack_push(
    toml_wctx * w, toml_istack * st, const GTEXT_TOML_Value * node) {
  if (st->count == st->capacity) {
    size_t want = st->capacity ? st->capacity * 2 : 16;
    if (want > SIZE_MAX / sizeof(toml_iframe)) {
      w->status = GTEXT_TOML_E_OOM;
      return false;
    }
    toml_iframe * grown = gtext_allocator_realloc(
        w->alloc, st->frames, want * sizeof(toml_iframe));
    if (!grown) {
      w->status = GTEXT_TOML_E_OOM;
      return false;
    }
    st->frames = grown;
    st->capacity = want;
  }
  st->frames[st->count].node = node;
  st->frames[st->count].index = 0;
  st->count++;
  return true;
}

/**
 * Write one value in the inline spelling, however deep it goes.
 *
 * `{ a = 1, b = 2 }` and `[1, 2]`, empty as `{}` and `[]`. Inside here the
 * table style does not apply any more: a table reached through an array or an
 * inline table has no header available to it, so everything below this point
 * is inline whatever it was read as.
 */
static bool emit_inline(
    toml_wctx * w, const GTEXT_TOML_Value * value, toml_istack * st) {
  if (!is_container(value)) return emit_scalar(w, value);

  size_t base = st->count;
  if (!istack_push(w, st, value)) return false;
  if (!wemitz(w, value->type == GTEXT_TOML_TABLE ? "{" : "[")) return false;

  while (st->count > base) {
    toml_iframe * frame = &st->frames[st->count - 1];
    const GTEXT_TOML_Value * node = frame->node;
    bool table = node->type == GTEXT_TOML_TABLE;
    size_t n = table ? node->as.table.count : node->as.array.count;

    if (frame->index >= n) {
      if (!wemitz(w, table ? (n ? " }" : "}") : "]")) return false;
      st->count--;
      continue;
    }

    if (frame->index) {
      if (!wemitz(w, ", ")) return false;
    }
    else if (table) {
      if (!wemitz(w, " ")) return false;
    }

    const GTEXT_TOML_Value * child;
    if (table) {
      const toml_pair * pair = &node->as.table.pairs[frame->index];
      child = pair->value;
      if (!emit_key(w, pair->key, pair->len)) return false;
      if (!wemitz(w, " = ")) return false;
    }
    else {
      child = node->as.array.items[frame->index];
    }
    /* Advanced before descending, so the parent resumes at the next element
     * rather than at this one. */
    frame->index++;

    if (is_container(child)) {
      if (!istack_push(w, st, child)) return false;
      /* `frame` may dangle now: the push can realloc the frame array. Nothing
       * below this point reads it. */
      if (!wemitz(w, child->type == GTEXT_TOML_TABLE ? "{" : "[")) return false;
    }
    else if (!emit_scalar(w, child)) {
      return false;
    }
  }
  return true;
}

/*==========================================================================*
 * The header walk: `[a.b]` and `[[a.b]]`, on an explicit stack
 *==========================================================================*/

/** Whether this value gets a header of its own rather than going inline. */
static bool wants_header(toml_wctx * w, const GTEXT_TOML_Value * value) {
  if (w->style == GTEXT_TOML_TABLE_STYLE_INLINE) return false;
  if (value->type == GTEXT_TOML_TABLE) {
    if (w->style == GTEXT_TOML_TABLE_STYLE_HEADERS) return true;
    return value->origin != (unsigned char) TOML_TABLE_INLINE;
  }
  if (value->type != GTEXT_TOML_ARRAY) return false;
  /* An empty array cannot be spelled with headers at all: zero `[[key]]`
   * lines do not say that `key` exists, so `key = []` is the only spelling
   * that keeps the document's meaning - whatever the style asked for. */
  if (value->as.array.count == 0) return false;
  for (size_t i = 0; i < value->as.array.count; ++i) {
    if (value->as.array.items[i]->type != GTEXT_TOML_TABLE) return false;
  }
  if (w->style == GTEXT_TOML_TABLE_STYLE_HEADERS) return true;
  return value->origin == (unsigned char) TOML_ARRAY_OF_TABLES;
}

/** A table being walked in two passes, or an array of tables being iterated. */
typedef struct {
  const GTEXT_TOML_Value * node;
  size_t index;
  size_t path_len; ///< What to cut the header path back to when this pops.
  bool is_array;
  bool second_pass; ///< Tables: plain keys are done, sub-tables are next.
} toml_hframe;

typedef struct {
  toml_hframe * frames;
  size_t count;
  size_t capacity;
} toml_hstack;

static bool hstack_push(toml_wctx * w, toml_hstack * st,
    const GTEXT_TOML_Value * node, size_t path_len, bool is_array) {
  if (st->count == st->capacity) {
    size_t want = st->capacity ? st->capacity * 2 : 16;
    if (want > SIZE_MAX / sizeof(toml_hframe)) {
      w->status = GTEXT_TOML_E_OOM;
      return false;
    }
    toml_hframe * grown = gtext_allocator_realloc(
        w->alloc, st->frames, want * sizeof(toml_hframe));
    if (!grown) {
      w->status = GTEXT_TOML_E_OOM;
      return false;
    }
    st->frames = grown;
    st->capacity = want;
  }
  st->frames[st->count].node = node;
  st->frames[st->count].index = 0;
  st->frames[st->count].path_len = path_len;
  st->frames[st->count].is_array = is_array;
  st->frames[st->count].second_pass = false;
  st->count++;
  return true;
}

/** Extend the header path by one key, remembering where to cut it back to. */
static bool path_descend(
    toml_wctx * w, const char * key, size_t len, size_t * saved) {
  *saved = w->path.len;
  if (w->path.len && !badd(w, &w->path, ".", 1)) return false;
  return spell_key(w, &w->path, key, len);
}

/** Write `[path]` or `[[path]]`, with a blank line before it if anything
 *  has been written already. */
static bool emit_header(toml_wctx * w, bool array) {
  if (w->wrote_any && !wemitz(w, "\n")) return false;
  if (!wemitz(w, array ? "[[" : "[")) return false;
  if (!wemit(w, w->path.data, w->path.len)) return false;
  if (!wemitz(w, array ? "]]\n" : "]\n")) return false;
  w->wrote_any = true;
  return true;
}

GTEXT_TOML_Status gtext_toml_write(const GTEXT_TOML_Value * root,
    GTEXT_TOML_Sink * sink, const GTEXT_TOML_Write_Options * opts) {
  if (!root || !sink || !sink->write) return GTEXT_TOML_E_INVALID;
  /* v1.0.0, Table: "a TOML document is a table". There is no other shape a
   * document can have, so a scalar root is a caller error rather than a
   * document this writer declines to spell. */
  if (root->type != GTEXT_TOML_TABLE) return GTEXT_TOML_E_INVALID;

  GTEXT_TOML_Write_Options effective = gtext_toml_write_options_default();
  if (opts) effective = *opts;

  toml_wctx w;
  memset(&w, 0, sizeof(w));
  w.sink = sink;
  w.alloc = effective.allocator;
  w.datetime = effective.datetime;
  w.style = effective.table_style;
  w.status = GTEXT_TOML_OK;

  toml_istack inline_stack;
  memset(&inline_stack, 0, sizeof(inline_stack));
  toml_hstack headers;
  memset(&headers, 0, sizeof(headers));

  if (hstack_push(&w, &headers, root, 0, false)) {
    while (w.status == GTEXT_TOML_OK && headers.count) {
      toml_hframe * frame = &headers.frames[headers.count - 1];
      const GTEXT_TOML_Value * node = frame->node;

      if (frame->is_array) {
        if (frame->index >= node->as.array.count) {
          w.path.len = frame->path_len;
          headers.count--;
          continue;
        }
        const GTEXT_TOML_Value * element =
            node->as.array.items[frame->index];
        frame->index++;
        if (!emit_header(&w, true)) break;
        /* Elements of one array of tables share its header path, so the
         * element's frame cuts back to where it already is. */
        if (!hstack_push(&w, &headers, element, w.path.len, false)) break;
        continue;
      }

      size_t n = node->as.table.count;
      if (!frame->second_pass) {
        if (frame->index >= n) {
          frame->second_pass = true;
          frame->index = 0;
          continue;
        }
        const toml_pair * pair = &node->as.table.pairs[frame->index];
        frame->index++;
        if (wants_header(&w, pair->value)) continue;
        if (!emit_key(&w, pair->key, pair->len)) break;
        if (!wemitz(&w, " = ")) break;
        if (!emit_inline(&w, pair->value, &inline_stack)) break;
        if (!wemitz(&w, "\n")) break;
        w.wrote_any = true;
        continue;
      }

      if (frame->index >= n) {
        w.path.len = frame->path_len;
        headers.count--;
        continue;
      }
      const toml_pair * pair = &node->as.table.pairs[frame->index];
      frame->index++;
      if (!wants_header(&w, pair->value)) continue;

      size_t saved = 0;
      if (!path_descend(&w, pair->key, pair->len, &saved)) break;
      if (pair->value->type == GTEXT_TOML_ARRAY) {
        if (!hstack_push(&w, &headers, pair->value, saved, true)) break;
        continue;
      }
      if (!emit_header(&w, false)) break;
      if (!hstack_push(&w, &headers, pair->value, saved, false)) break;
    }
  }

  GTEXT_TOML_Status status = w.status;
  gtext_allocator_free(w.alloc, inline_stack.frames);
  gtext_allocator_free(w.alloc, headers.frames);
  toml_buf_free(w.alloc, &w.path);
  toml_buf_free(w.alloc, &w.scratch);
  return status;
}
