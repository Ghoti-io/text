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
 * @file
 *
 * JSON writer infrastructure implementation.
 *
 * This file implements the sink abstraction for writing JSON output
 * to various destinations.
 */

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/macros.h>
#include "json_internal.h"
#include "../text_number_internal.h"
#include <ghoti.io/text/json/json_core.h>
#include <ghoti.io/text/json/json_dom.h>
#include <ghoti.io/text/json/json_writer.h>

// Internal write callback for growable buffer sink
static int buffer_write_fn(void * user, const char * bytes, size_t len) {
  GTEXT_JSON_Buffer_Sink * buf = (GTEXT_JSON_Buffer_Sink *)user;
  if (!buf || !bytes) {
    return 1; // Error
  }

  // Check for integer overflow in needed calculation
  if (len > SIZE_MAX - buf->used || buf->used > SIZE_MAX - len - 1) {
    return 1; // Overflow
  }

  // Check if we need to grow the buffer
  size_t needed = buf->used + len + 1; // +1 for null terminator
  if (needed > buf->size) {
    // Grow buffer (double size strategy, with minimum growth)
    size_t new_size = buf->size;
    if (new_size == 0) {
      new_size = 256; // Initial size
    }
    while (new_size < needed) {
      // Unreachable on any 64-bit host: new_size only exceeds SIZE_MAX/2 once
      // the buffer is eight exbibytes, and `needed` is a length that was
      // already written.  Kept because it is the arithmetic that makes the
      // doubling below safe to state, not because it can fire.
      if (new_size > SIZE_MAX / 2) {
        return 1; // Overflow - cannot grow further
      }
      new_size *= 2;
    }

    /* allocator-exempt: a buffer sink owns its buffer.  A sink is created
       before any write options are seen - gtext_json_sink_buffer() takes none -
       and it outlives the write, so there is no caller allocator to read here
       and routing it through one would mean freeing through whichever options
       happened to be passed last.  Same line GTEXT_INI_Write_Options draws. */
    char * new_data =
        (char *)realloc(buf->data, new_size); // allocator-exempt
    if (!new_data) {
      return 1; // Out of memory
    }
    buf->data = new_data;
    buf->size = new_size;
  }

  // Verify bounds before copying (defensive check)
  if (buf->used + len > buf->size - 1) {
    return 1; // Should not happen, but be safe
  }

  // Copy data
  memcpy(buf->data + buf->used, bytes, len);
  buf->used += len;
  // Verify bounds before writing null terminator
  if (buf->used < buf->size) {
    buf->data[buf->used] = '\0'; // Null terminate for convenience
  }

  return 0; // Success
}

// Internal write callback for fixed buffer sink
static int fixed_buffer_write_fn(void * user, const char * bytes, size_t len) {
  GTEXT_JSON_Fixed_Buffer_Sink * buf = (GTEXT_JSON_Fixed_Buffer_Sink *)user;
  if (!buf || !bytes) {
    return 1; // Error
  }

  // Calculate how much we can write (leave room for null terminator)
  // We can write up to (size - used - 1) bytes to leave room for null
  // terminator If used >= size, available = 0 (no underflow possible since
  // size_t is unsigned)
  size_t available = 0;
  if (buf->size > buf->used) {
    // Check for underflow: if size - used would underflow, but since size >
    // used, we know size - used is valid. Then check if we can subtract 1.
    if (buf->size - buf->used > 1) {
      available = buf->size - buf->used - 1;
    }
    // else: available remains 0 (only room for null terminator, no data)
  }

  size_t to_write = len;
  int truncated = 0;

  if (to_write > available) {
    to_write = available;
    truncated = 1;
    buf->truncated = 1;
  }

  // Copy data - verify bounds before copying
  if (to_write > 0 && available > 0) {
    // Verify bounds: used + to_write must not exceed size - 1
    // This check ensures we don't write beyond buffer bounds
    if (buf->used <= buf->size - 1 && to_write <= buf->size - 1 - buf->used) {
      memcpy(buf->data + buf->used, bytes, to_write);
      buf->used += to_write;
      // Verify bounds before writing null terminator
      if (buf->used < buf->size) {
        buf->data[buf->used] = '\0';
      }
    }
    else {
      // Should not happen if logic is correct, but be safe
      truncated = 1;
      buf->truncated = 1;
    }
  }

  // Return error if truncation occurred
  return truncated ? 1 : 0;
}

GTEXT_API GTEXT_JSON_Status gtext_json_sink_buffer(GTEXT_JSON_Sink * sink) {
  if (!sink) {
    return GTEXT_JSON_E_INVALID;
  }

  /* allocator-exempt: see buffer_grow().  The sink predates the options. */
  GTEXT_JSON_Buffer_Sink * buf = (GTEXT_JSON_Buffer_Sink *)
      malloc(sizeof(GTEXT_JSON_Buffer_Sink)); // allocator-exempt
  if (!buf) {
    return GTEXT_JSON_E_OOM;
  }

  buf->data = NULL;
  buf->size = 0;
  buf->used = 0;

  sink->write = buffer_write_fn;
  sink->user = buf;

  return GTEXT_JSON_OK;
}

GTEXT_API const char * gtext_json_sink_buffer_data(
    const GTEXT_JSON_Sink * sink) {
  if (!sink || sink->write != buffer_write_fn) {
    return NULL;
  }

  GTEXT_JSON_Buffer_Sink * buf = (GTEXT_JSON_Buffer_Sink *)sink->user;
  if (!buf) {
    return NULL;
  }

  return buf->data ? buf->data : "";
}

GTEXT_API size_t gtext_json_sink_buffer_size(const GTEXT_JSON_Sink * sink) {
  if (!sink || sink->write != buffer_write_fn) {
    return 0;
  }

  GTEXT_JSON_Buffer_Sink * buf = (GTEXT_JSON_Buffer_Sink *)sink->user;
  if (!buf) {
    return 0;
  }

  return buf->used;
}

GTEXT_API void gtext_json_sink_buffer_free(GTEXT_JSON_Sink * sink) {
  if (!sink || sink->write != buffer_write_fn) {
    return;
  }

  GTEXT_JSON_Buffer_Sink * buf = (GTEXT_JSON_Buffer_Sink *)sink->user;
  if (buf) {
    /* allocator-exempt: paired with gtext_json_sink_buffer(). */
    free(buf->data); // allocator-exempt
    free(buf);       // allocator-exempt
    sink->user = NULL;
    sink->write = NULL;
  }
}

GTEXT_API GTEXT_JSON_Status gtext_json_sink_fixed_buffer(
    GTEXT_JSON_Sink * sink, char * buffer, size_t size) {
  if (!sink || !buffer || size == 0) {
    return GTEXT_JSON_E_INVALID;
  }

  /* allocator-exempt: see buffer_grow().  The caller owns the bytes; this is
     only the bookkeeping around them, and it predates the options too. */
  GTEXT_JSON_Fixed_Buffer_Sink * buf = (GTEXT_JSON_Fixed_Buffer_Sink *)
      malloc(sizeof(GTEXT_JSON_Fixed_Buffer_Sink)); // allocator-exempt
  if (!buf) {
    return GTEXT_JSON_E_OOM;
  }

  buf->data = buffer;
  buf->size = size;
  buf->used = 0;
  buf->truncated = 0;

  // Initialize buffer with null terminator
  buffer[0] = '\0';

  sink->write = fixed_buffer_write_fn;
  sink->user = buf;

  return GTEXT_JSON_OK;
}

GTEXT_API size_t gtext_json_sink_fixed_buffer_used(
    const GTEXT_JSON_Sink * sink) {
  if (!sink || sink->write != fixed_buffer_write_fn) {
    return 0;
  }

  GTEXT_JSON_Fixed_Buffer_Sink * buf =
      (GTEXT_JSON_Fixed_Buffer_Sink *)sink->user;
  if (!buf) {
    return 0;
  }

  return buf->used;
}

GTEXT_API bool gtext_json_sink_fixed_buffer_truncated(
    const GTEXT_JSON_Sink * sink) {
  if (!sink || sink->write != fixed_buffer_write_fn) {
    return false;
  }

  GTEXT_JSON_Fixed_Buffer_Sink * buf =
      (GTEXT_JSON_Fixed_Buffer_Sink *)sink->user;
  if (!buf) {
    return false;
  }

  return buf->truncated;
}

GTEXT_API void gtext_json_sink_fixed_buffer_free(GTEXT_JSON_Sink * sink) {
  if (!sink || sink->write != fixed_buffer_write_fn) {
    return;
  }

  GTEXT_JSON_Fixed_Buffer_Sink * buf =
      (GTEXT_JSON_Fixed_Buffer_Sink *)sink->user;
  if (buf) {
    /* allocator-exempt: paired with gtext_json_sink_fixed_buffer(). */
    free(buf); // allocator-exempt
    sink->user = NULL;
    sink->write = NULL;
  }
}

// Helper function to write bytes to sink
static int write_bytes(GTEXT_JSON_Sink * sink, const char * bytes, size_t len) {
  if (!sink || !sink->write || !bytes) {
    return 1;
  }
  return sink->write(sink->user, bytes, len);
}

// Helper function to write a single character
static int write_char(GTEXT_JSON_Sink * sink, char c) {
  return write_bytes(sink, &c, 1);
}

// Helper function to write a string
static int write_string(GTEXT_JSON_Sink * sink, const char * s) {
  if (!s) {
    return 0;
  }
  return write_bytes(sink, s, strlen(s));
}

// Write Unicode escape sequence \uXXXX
static int write_unicode_escape(
    GTEXT_JSON_Sink * sink, unsigned int codepoint) {
  char buf[7];
  int len = snprintf(buf, sizeof(buf), "\\u%04X", codepoint);
  if (len < 0 || (size_t)len >= sizeof(buf)) {
    return 1;
  }
  return write_bytes(sink, buf, (size_t)len);
}

/**
 * A write failed because the string is not UTF-8.
 *
 * The writers' internal helpers return 0 or 1, where 1 is a sink failure. This
 * third value separates "the sink refused the bytes" from "the bytes were not
 * writable", so the public entry points can report GTEXT_JSON_E_BAD_UNICODE -
 * the same status the parser uses for the same input - rather than
 * GTEXT_JSON_E_WRITE, which would send a caller looking at their sink.
 */
#define JSON_WRITE_ERR_UNICODE 2

/* Decode one UTF-8 character, strictly. Returns its length in bytes and writes
 * the codepoint, or 0 if these bytes are not a well-formed character.
 *
 * Deliberately not the lexer's json_utf8_decode(): that one answers "is this
 * white space", so it accepts an overlong encoding and a lone surrogate, and
 * here those are exactly what must be refused. A writer that emits them
 * produces bytes its own parser rejects. */
static size_t writer_utf8_decode(
    const char * p, size_t available, uint32_t * out_cp) {
  const unsigned char * u = (const unsigned char *)p;
  if (available == 0) {
    return 0;
  }
  if (u[0] < 0x80) {
    *out_cp = u[0];
    return 1;
  }
  size_t seq;
  uint32_t cp;
  if ((u[0] & 0xE0) == 0xC0) {
    seq = 2;
    cp = u[0] & 0x1Fu;
  }
  else if ((u[0] & 0xF0) == 0xE0) {
    seq = 3;
    cp = u[0] & 0x0Fu;
  }
  else if ((u[0] & 0xF8) == 0xF0) {
    seq = 4;
    cp = u[0] & 0x07u;
  }
  else {
    return 0; // a continuation byte, or 0xF8..0xFF
  }
  if (available < seq) {
    return 0; // truncated
  }
  for (size_t i = 1; i < seq; i++) {
    if ((u[i] & 0xC0) != 0x80) {
      return 0;
    }
    cp = (cp << 6) | (u[i] & 0x3Fu);
  }
  /* Overlong, surrogate half, and past the last codepoint. Each is a sequence
     that decodes and still is not UTF-8. */
  static const uint32_t lowest[5] = {0, 0, 0x80, 0x800, 0x10000};
  if (cp < lowest[seq] || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
    return 0;
  }
  *out_cp = cp;
  return seq;
}

// Escape and write a string value
static int write_escaped_string(GTEXT_JSON_Sink * sink, const char * str,
    size_t len, const GTEXT_JSON_Write_Options * opt) {
  if (write_char(sink, '"') != 0) {
    return 1;
  }

  const GTEXT_JSON_Write_Options * opts =
      opt ? opt : &(GTEXT_JSON_Write_Options){0};
  int escape_solidus = opts->escape_solidus;
  int escape_unicode = opts->escape_unicode;
  int escape_all_non_ascii = opts->escape_all_non_ascii;

  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)str[i];

    // Standard escape sequences
    switch (c) {
    case '"':
      if (write_string(sink, "\\\"") != 0)
        return 1;
      continue;
    case '\\':
      if (write_string(sink, "\\\\") != 0)
        return 1;
      continue;
    case '/':
      if (escape_solidus) {
        if (write_string(sink, "\\/") != 0)
          return 1;
      }
      else {
        if (write_char(sink, '/') != 0)
          return 1;
      }
      continue;
    case '\b':
      if (write_string(sink, "\\b") != 0)
        return 1;
      continue;
    case '\f':
      if (write_string(sink, "\\f") != 0)
        return 1;
      continue;
    case '\n':
      if (write_string(sink, "\\n") != 0)
        return 1;
      continue;
    case '\r':
      if (write_string(sink, "\\r") != 0)
        return 1;
      continue;
    case '\t':
      if (write_string(sink, "\\t") != 0)
        return 1;
      continue;
    }

    // Control characters (0x00-0x1F) must be escaped as \uXXXX
    if (c < 0x20) {
      if (write_unicode_escape(sink, c) != 0)
        return 1;
      continue;
    }

    /* Non-ASCII. **Decoded as a character, not handled as a byte.**
     *
     * Both of these branches used to escape each byte of a sequence on its own,
     * under a comment saying a more sophisticated implementation would decode
     * the UTF-8 - so `é` (C3 A9) came out as `Ã©`, which is valid
     * JSON holding the two characters `Ã©`. Every non-ASCII string was
     * silently changed by either option, and the output reparsed cleanly as the wrong
     * value, which is the failure a round-trip test over ASCII cannot see.
     *
     * And a sequence that is not UTF-8 was written through verbatim, so the
     * writer reported GTEXT_JSON_OK for bytes this library's own parser
     * refuses. It is refused here instead: a string a caller built through the
     * DOM or handed to gtext_json_writer_string() is the only way such bytes
     * reach a writer, since a parse validates. */
    if (c >= 0x80) {
      uint32_t cp = 0;
      const size_t seq = writer_utf8_decode(str + i, len - i, &cp);
      if (seq == 0) {
        return JSON_WRITE_ERR_UNICODE;
      }

      if (escape_unicode || escape_all_non_ascii) {
        if (cp < 0x10000u) {
          if (write_unicode_escape(sink, cp) != 0) {
            return 1;
          }
        }
        else {
          /* Outside the BMP there is no single \uXXXX, so JSON's escape is the
             surrogate pair - which is why a byte-wise escape could not have
             been right for an emoji even by accident. */
          const uint32_t v = cp - 0x10000u;
          if (write_unicode_escape(sink, 0xD800u + (v >> 10)) != 0
              || write_unicode_escape(sink, 0xDC00u + (v & 0x3FFu)) != 0) {
            return 1;
          }
        }
      }
      else {
        if (write_bytes(sink, str + i, seq) != 0) {
          return 1;
        }
      }
      i += seq - 1; // the loop's own increment accounts for the last byte
      continue;
    }

    // Regular character - write as-is
    if (write_char(sink, (char)c) != 0)
      return 1;
  }

  if (write_char(sink, '"') != 0) {
    return 1;
  }

  return 0;
}

// Write indentation for pretty printing
static int write_indent(
    GTEXT_JSON_Sink * sink, int depth, const GTEXT_JSON_Write_Options * opt) {
  if (!opt || !opt->pretty) {
    return 0;
  }

  const char * newline = opt->newline ? opt->newline : "\n";
  if (write_string(sink, newline) != 0) {
    return 1;
  }

  int spaces = opt->indent_spaces > 0 ? opt->indent_spaces : 2;

  // Check for integer overflow: depth * spaces
  // INT_MAX / spaces gives max safe depth
  // Avoid division by zero and check overflow correctly
  if (spaces > 0 && depth > INT_MAX / spaces) {
    return 1; // Overflow would occur
  }

  int total_spaces = depth * spaces;

  for (int i = 0; i < total_spaces; i++) {
    if (write_char(sink, ' ') != 0) {
      return 1;
    }
  }

  return 0;
}

// Format a double according to the specified strategy
static int format_double(char * buf, size_t buf_size, double d,
    GTEXT_JSON_Float_Format format, int precision) {
  // Null pointer and size checks
  if (!buf || buf_size == 0) {
    return -1;
  }

  switch (format) {
  case GTEXT_JSON_FLOAT_SHORTEST:
    return gtext_number_format_double(
        buf, buf_size, d, GTEXT_NUMBER_GENERAL, 17);

  case GTEXT_JSON_FLOAT_FIXED:
    // Clamp precision to reasonable range
    if (precision < 0)
      precision = 0;
    if (precision > 20)
      precision = 20;
    return gtext_number_format_double(
        buf, buf_size, d, GTEXT_NUMBER_FIXED, precision);

  case GTEXT_JSON_FLOAT_SCIENTIFIC:
    // Clamp precision to reasonable range
    if (precision < 0)
      precision = 0;
    if (precision > 20)
      precision = 20;
    return gtext_number_format_double(
        buf, buf_size, d, GTEXT_NUMBER_SCIENTIFIC, precision);

  default:
    // Fallback to shortest
    return gtext_number_format_double(
        buf, buf_size, d, GTEXT_NUMBER_GENERAL, 17);
  }
}

// Write a number value
static int write_number(GTEXT_JSON_Sink * sink, const GTEXT_JSON_Value * v,
    const GTEXT_JSON_Write_Options * opt) {
  const GTEXT_JSON_Write_Options * opts =
      opt ? opt : &(GTEXT_JSON_Write_Options){0};

  // Check for nonfinite numbers
  if (v->as.number.has_dbl) {
    double d = v->as.number.dbl;
    if (!isfinite(d)) {
      if (!opts->allow_nonfinite_numbers) {
        return 1; // Error: nonfinite not allowed
      }
      if (isnan(d)) {
        return write_string(sink, "NaN");
      }
      else if (isinf(d)) {
        if (d < 0) {
          return write_string(sink, "-Infinity");
        }
        else {
          return write_string(sink, "Infinity");
        }
      }
    }
  }

  // Prefer lexeme if available and canonical_numbers is off
  if (v->as.number.lexeme && v->as.number.lexeme_len > 0 &&
      !opts->canonical_numbers) {
    return write_bytes(sink, v->as.number.lexeme, v->as.number.lexeme_len);
  }

  // Format from available representation
  char num_buf[64];

  // Try int64 first (if available and fits)
  if (v->as.number.has_i64) {
    int64_t i64 = v->as.number.i64;
    int len = gtext_number_format_i64(num_buf, sizeof(num_buf), i64);
    if (len > 0 && (size_t)len < sizeof(num_buf)) {
      return write_bytes(sink, num_buf, (size_t)len);
    }
  }

  // Try uint64 next
  if (v->as.number.has_u64) {
    uint64_t u64 = v->as.number.u64;
    int len = gtext_number_format_u64(num_buf, sizeof(num_buf), u64);
    if (len > 0 && (size_t)len < sizeof(num_buf)) {
      return write_bytes(sink, num_buf, (size_t)len);
    }
  }

  // Use double (or format from lexeme if available)
  if (v->as.number.has_dbl) {
    double d = v->as.number.dbl;
    GTEXT_JSON_Float_Format float_fmt = opts->float_format;
    int float_prec = opts->float_precision > 0 ? opts->float_precision : 6;
    int len = format_double(num_buf, sizeof(num_buf), d, float_fmt, float_prec);
    if (len > 0 && (size_t)len < sizeof(num_buf)) {
      return write_bytes(sink, num_buf, (size_t)len);
    }
  }

  // Fallback: use lexeme if available
  if (v->as.number.lexeme && v->as.number.lexeme_len > 0) {
    return write_bytes(sink, v->as.number.lexeme, v->as.number.lexeme_len);
  }

  // No valid representation
  return 1;
}

/* A scalar, or NULL, which 7.2's equivalent here is: the empty value a
   container holds where nothing was put.  Returns 1 if @p v is a container
   instead, which the caller opens. */
static int write_scalar_value(GTEXT_JSON_Sink * sink,
    const GTEXT_JSON_Value * v, const GTEXT_JSON_Write_Options * opts,
    int * out_is_container, int * out_status) {
  *out_is_container = 0;
  *out_status = 0;
  if (!v) {
    *out_status = write_string(sink, "null");
    return 0;
  }
  switch (v->type) {
  case GTEXT_JSON_NULL:
    *out_status = write_string(sink, "null");
    return 0;

  case GTEXT_JSON_BOOL:
    *out_status = write_string(sink, v->as.boolean ? "true" : "false");
    return 0;

  case GTEXT_JSON_NUMBER:
    *out_status = write_number(sink, v, opts);
    return 0;

  case GTEXT_JSON_STRING:
    // Check for NULL string data (empty string is valid, but NULL pointer is
    // not)
    if (!v->as.string.data && v->as.string.len > 0) {
      *out_status = 1; // Invalid string state
      return 0;
    }
    *out_status =
        write_escaped_string(sink, v->as.string.data, v->as.string.len, opts);
    return 0;

  case GTEXT_JSON_ARRAY:
  case GTEXT_JSON_OBJECT:
    *out_is_container = 1;
    return 0;

  default:
    *out_status = 1; // Unknown type
    return 0;
  }
}

/* The threshold decision, which is the same question for both kinds of
   container asked of two different options. */
static int write_should_inline(
    size_t size, int threshold, const GTEXT_JSON_Write_Options * opts) {
  if (!opts->pretty) {
    return 0;
  }
  /* -1 means always pretty, and so does 0: neither is a width. */
  if (threshold <= 0) {
    return 0;
  }
  return size <= (size_t)threshold;
}

/* The object key order, when one was asked for: indices into pairs[], sorted by
   key.  NULL when the caller did not ask, which means "as stored". */
static int write_object_indices(const GTEXT_JSON_Value * v, size_t size,
    const GTEXT_JSON_Write_Options * opts, size_t ** out_indices) {
  *out_indices = NULL;
  if (!opts->sort_object_keys || size == 0) {
    return 0;
  }
  if (size > SIZE_MAX / sizeof(size_t)) {
    return 1; // Overflow
  }
  size_t * indices =
      (size_t *)gtext_allocator_malloc(opts->allocator, size * sizeof(size_t));
  if (!indices) {
    return 1; // Out of memory
  }
  for (size_t i = 0; i < size; i++) {
    indices[i] = i;
  }
  for (size_t i = 0; i + 1 < size; i++) {
    for (size_t j = 0; j + 1 < size - i; j++) {
      size_t idx_a = indices[j];
      size_t idx_b = indices[j + 1];
      if (idx_a >= size || idx_b >= size || idx_a >= v->as.object.capacity ||
          idx_b >= v->as.object.capacity) {
        gtext_allocator_free(opts->allocator, indices);
        return 1; // Out of bounds
      }
      const char * key_a = v->as.object.pairs[idx_a].key;
      size_t len_a = v->as.object.pairs[idx_a].key_len;
      const char * key_b = v->as.object.pairs[idx_b].key;
      size_t len_b = v->as.object.pairs[idx_b].key_len;
      if (!key_a || !key_b) {
        gtext_allocator_free(opts->allocator, indices);
        return 1; // Invalid key
      }
      size_t min_len = len_a < len_b ? len_a : len_b;
      int cmp = memcmp(key_a, key_b, min_len);
      if (cmp > 0 || (cmp == 0 && len_a > len_b)) {
        size_t tmp = indices[j];
        indices[j] = indices[j + 1];
        indices[j + 1] = tmp;
      }
    }
  }
  *out_indices = indices;
  return 0;
}

/* One container being written: how far through it, how it is being formatted,
   and the key order it was given. */
typedef struct {
  const GTEXT_JSON_Value * v;
  size_t size;
  size_t i;
  int should_inline;
  int depth;
  size_t * indices; /* owned; objects with sort_object_keys only */
} json_write_frame;

/* Write @p root, with the writer's own stack on the heap.
 *
 * This was a recursion, and a value built through the DOM API has no depth
 * limit to bound it: max_depth is a *parser* option and json_core.h says so
 * carefully.  Two hundred thousand levels, assembled through the documented
 * API with nothing unusual asked for, took the process down.  Its `depth`
 * parameter looked like a guard and was not - it was only ever the indentation
 * width.
 *
 * Cycles are not guarded against and do not need to be: a value cannot contain
 * itself, because json_check_no_cycle() refuses the insertion that would do it.
 * Without that there would be nothing to write anyway. */
static int write_value_iterative(GTEXT_JSON_Sink * sink,
    const GTEXT_JSON_Value * root, const GTEXT_JSON_Write_Options * opt) {
  const GTEXT_JSON_Write_Options * opts =
      opt ? opt : &(GTEXT_JSON_Write_Options){0};

  json_write_frame inline_frames[32];
  json_write_frame * frames = inline_frames;
  size_t count = 0;
  size_t capacity = sizeof(inline_frames) / sizeof(inline_frames[0]);
  int status = 0;

  /* Opens @p node as a container: the bracket, then a frame.  Everything the
     frame needs is computed here, so the loop below never asks twice. */
  #define JSON_WRITE_OPEN(node, node_depth)                                    \
    do {                                                                       \
      const GTEXT_JSON_Value * open_v = (node);                                \
      const int is_object = (open_v->type == GTEXT_JSON_OBJECT);                \
      size_t open_size = is_object ? open_v->as.object.count                   \
                                   : open_v->as.array.count;                   \
      if (write_char(sink, is_object ? '{' : '[') != 0) {                      \
        status = 1;                                                            \
        goto done;                                                             \
      }                                                                        \
      /* Defensive: a count with no storage behind it. */                      \
      if (open_size > 0                                                        \
          && !(is_object ? (void *)open_v->as.object.pairs                      \
                         : (void *)open_v->as.array.elems)) {                   \
        status = 1;                                                            \
        goto done;                                                             \
      }                                                                        \
      size_t * open_indices = NULL;                                            \
      if (is_object                                                            \
          && write_object_indices(open_v, open_size, opts, &open_indices)       \
              != 0) {                                                          \
        status = 1;                                                            \
        goto done;                                                             \
      }                                                                        \
      if (count == capacity) {                                                 \
        size_t new_capacity = capacity * 2;                                    \
        json_write_frame * grown;                                              \
        if (frames == inline_frames) {                                         \
          grown = (json_write_frame *)gtext_allocator_malloc(                  \
              opts->allocator, new_capacity * sizeof(*grown));                 \
          if (grown) memcpy(grown, frames, count * sizeof(*grown));            \
        }                                                                      \
        else {                                                                 \
          grown = (json_write_frame *)gtext_allocator_realloc(                 \
              opts->allocator, frames, new_capacity * sizeof(*grown));         \
        }                                                                      \
        if (!grown) {                                                          \
          gtext_allocator_free(opts->allocator, open_indices);                 \
          status = 1;                                                          \
          goto done;                                                           \
        }                                                                      \
        frames = grown;                                                        \
        capacity = new_capacity;                                               \
      }                                                                        \
      frames[count].v = open_v;                                                \
      frames[count].size = open_size;                                          \
      frames[count].i = 0;                                                     \
      frames[count].depth = (node_depth);                                      \
      frames[count].indices = open_indices;                                    \
      frames[count].should_inline = write_should_inline(open_size,              \
          is_object ? opts->inline_object_threshold                             \
                    : opts->inline_array_threshold,                            \
          opts);                                                               \
      count++;                                                                 \
    } while (0)

  {
    int is_container = 0;
    int leaf_status = 0;
    write_scalar_value(sink, root, opts, &is_container, &leaf_status);
    if (!is_container) {
      return leaf_status;
    }
    JSON_WRITE_OPEN(root, 0);
  }

  while (count > 0) {
    json_write_frame * f = &frames[count - 1];
    const int is_object = (f->v->type == GTEXT_JSON_OBJECT);

    if (f->i >= f->size) {
      if (opts->pretty && f->size > 0 && !f->should_inline) {
        if (write_indent(sink, f->depth, opts) != 0) {
          status = 1;
          goto done;
        }
      }
      if (write_char(sink, is_object ? '}' : ']') != 0) {
        status = 1;
        goto done;
      }
      gtext_allocator_free(opts->allocator, f->indices);
      count--;
      continue;
    }

    const size_t i = f->i;
    const int child_depth = f->depth + 1;
    const int should_inline = f->should_inline;

    if (i > 0) {
      if (write_char(sink, ',') != 0) {
        status = 1;
        goto done;
      }
      /* **Not when a newline follows it.** `space_after_comma` is a
         compact-mode option: in pretty mode the comma is followed by an indent that begins
         with a newline, so the space becomes trailing white space at the end of
         every line - which changes nothing a reader sees and which many tools
         object to. The incremental writer had always declined it here (it is
         inside its own `else if (!pretty)`); this writer did not, so the two
         produced different bytes for the same document whenever both options
         were set. Found by tests/fuzz/fuzz_json_writer.cpp once it began
         comparing the two writers' bytes rather than their reparsed values -
         a canonical comparison normalises exactly this away. */
      if (!opts->pretty || should_inline) {
        if (opts->space_after_comma && write_char(sink, ' ') != 0) {
          status = 1;
          goto done;
        }
      }
    }

    if (opts->pretty && !should_inline) {
      if (write_indent(sink, child_depth, opts) != 0) {
        status = 1;
        goto done;
      }
    }
    else if (should_inline && i > 0) {
      // Inline formatting: add space after comma
      if (write_char(sink, ' ') != 0) {
        status = 1;
        goto done;
      }
    }

    const GTEXT_JSON_Value * child = NULL;
    if (is_object) {
      const size_t idx = f->indices ? f->indices[i] : i;
      if (idx >= f->size || idx >= f->v->as.object.capacity) {
        status = 1; // Out of bounds
        goto done;
      }
      if (!f->v->as.object.pairs[idx].key) {
        status = 1; // Invalid key
        goto done;
      }
      if (write_escaped_string(sink, f->v->as.object.pairs[idx].key,
              f->v->as.object.pairs[idx].key_len, opts) != 0) {
        status = 1;
        goto done;
      }
      if (opts->pretty && !should_inline) {
        if (write_string(sink, ": ") != 0) {
          status = 1;
          goto done;
        }
      }
      else {
        if (write_char(sink, ':') != 0) {
          status = 1;
          goto done;
        }
        if (opts->space_after_colon && write_char(sink, ' ') != 0) {
          status = 1;
          goto done;
        }
      }
      child = f->v->as.object.pairs[idx].value;
    }
    else {
      if (!f->v->as.array.elems || i >= f->v->as.array.capacity) {
        status = 1; // Out of bounds
        goto done;
      }
      child = f->v->as.array.elems[i];
    }

    /* Advanced before the child is entered, because the open below can move
       the frame array and nothing may be written through @c f after it. */
    f->i++;

    int is_container = 0;
    int leaf_status = 0;
    write_scalar_value(sink, child, opts, &is_container, &leaf_status);
    if (!is_container) {
      if (leaf_status != 0) {
        status = leaf_status;
        goto done;
      }
      continue;
    }
    JSON_WRITE_OPEN(child, child_depth);
  }

done:
  #undef JSON_WRITE_OPEN
  for (size_t k = 0; k < count; k++) {
    gtext_allocator_free(opts->allocator, frames[k].indices);
  }
  if (frames != inline_frames) {
    gtext_allocator_free(opts->allocator, frames);
  }
  return status;
}

GTEXT_API GTEXT_JSON_Status gtext_json_write_value(GTEXT_JSON_Sink * sink,
    const GTEXT_JSON_Write_Options * opt, const GTEXT_JSON_Value * v,
    GTEXT_JSON_Error * err) {
  if (!sink || !v) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Invalid arguments: sink and value must not be NULL"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  if (!sink->write) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Invalid sink: write callback is NULL"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  /* GTEXT_JSON_Write_Options::records frames one record. This function writes
     a whole value and is called once per record, so - unlike the incremental
     writer - it needs no state to do that: the RS goes before this value and
     the line end after it, and a caller writing N records into one sink gets
     the same bytes the incremental writer would produce for the same N. */
  if (opt && opt->records != GTEXT_JSON_RECORDS_OFF) {
    if (opt->records == GTEXT_JSON_RECORDS_LINE && opt->pretty) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
            .message = "GTEXT_JSON_RECORDS_LINE cannot be combined with "
                       "pretty: one record per line and a value printed "
                       "across lines are contradictory requests"};
      }
      return GTEXT_JSON_E_INVALID;
    }
    if (opt->records == GTEXT_JSON_RECORDS_SEQ) {
      const char rs = (char)0x1E;
      if (write_bytes(sink, &rs, 1) != 0) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_WRITE,
              .message = "Failed to write the record separator"};
        }
        return GTEXT_JSON_E_WRITE;
      }
    }
  }

  int result = write_value_iterative(sink, v, opt);
  if (result != 0) {
    /* Which failure it was. A string that is not UTF-8 is the caller's bytes
       rather than the sink's doing, and reporting GTEXT_JSON_E_WRITE for it
       would send them looking at the wrong thing. */
    const GTEXT_JSON_Status code = (result == JSON_WRITE_ERR_UNICODE)
        ? GTEXT_JSON_E_BAD_UNICODE
        : GTEXT_JSON_E_WRITE;
    if (err) {
      *err = (GTEXT_JSON_Error){.code = code,
          .message = (code == GTEXT_JSON_E_BAD_UNICODE)
              ? "A string is not valid UTF-8 and cannot be written as JSON"
              : "Write operation failed"};
    }
    return code;
  }

  // Add trailing newline if requested
  const GTEXT_JSON_Write_Options * opts =
      opt ? opt : &(GTEXT_JSON_Write_Options){0};

  /* The record's terminator, and not also the trailing newline below: asking
     for both would end a record in two line ends, which reads back as a blank
     line between records. A records mode *is* the statement that every value
     ends in one, so it subsumes trailing_newline rather than adding to it. */
  const bool record_terminator = opts->records != GTEXT_JSON_RECORDS_OFF;
  if (record_terminator || opts->trailing_newline) {
    const char * newline = opts->newline ? opts->newline : "\n";
    if (write_string(sink, newline) != 0) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_WRITE,
            .message = record_terminator
                ? "Failed to write the record terminator"
                : "Failed to write trailing newline"};
      }
      return GTEXT_JSON_E_WRITE;
    }
  }

  return GTEXT_JSON_OK;
}

// ============================================================================
// Streaming Writer Implementation
// ============================================================================

// Default stack capacity
#define JSON_WRITER_DEFAULT_STACK_CAPACITY 32

// Helper to write bytes through writer's sink
static int writer_write_bytes(
    GTEXT_JSON_Writer * w, const char * bytes, size_t len) {
  if (!w || !w->sink.write || !bytes) {
    return 1;
  }
  int result = w->sink.write(w->sink.user, bytes, len);
  if (result != 0) {
    w->error = 1;
  }
  return result;
}

// Helper to write a single character
static int writer_write_char(GTEXT_JSON_Writer * w, char c) {
  return writer_write_bytes(w, &c, 1);
}

// Helper to write a string
static int writer_write_string(GTEXT_JSON_Writer * w, const char * s) {
  if (!s) {
    return 0;
  }
  return writer_write_bytes(w, s, strlen(s));
}

// Write indentation for pretty printing
static int writer_write_indent(GTEXT_JSON_Writer * w, int depth) {
  if (!w->opts.pretty) {
    return 0;
  }

  const char * newline = w->opts.newline ? w->opts.newline : "\n";
  if (writer_write_string(w, newline) != 0) {
    return 1;
  }

  int spaces = w->opts.indent_spaces > 0 ? w->opts.indent_spaces : 2;

  // Check for integer overflow: depth * spaces
  if (spaces > 0 && depth > INT_MAX / spaces) {
    return 1; // Overflow would occur
  }

  int total_spaces = depth * spaces;
  for (int i = 0; i < total_spaces; i++) {
    if (writer_write_char(w, ' ') != 0) {
      return 1;
    }
  }

  return 0;
}

// Ensure stack has capacity for at least one more entry
static int writer_ensure_stack(GTEXT_JSON_Writer * w) {
  if (w->stack_size >= w->stack_capacity) {
    size_t new_capacity = w->stack_capacity == 0
        ? JSON_WRITER_DEFAULT_STACK_CAPACITY
        : w->stack_capacity * 2;

    // Check for overflow
    if (new_capacity < w->stack_capacity) {
      return 1; // Overflow
    }

    // Check for reasonable maximum (prevent excessive allocation)
    if (new_capacity > 1024 * 1024) { // 1M entries is more than enough
      return 1;
    }

    // Check for overflow in multiplication: new_capacity *
    // sizeof(json_writer_stack_entry)
    size_t entry_size = sizeof(json_writer_stack_entry);
    if (entry_size > 0 && new_capacity > SIZE_MAX / entry_size) {
      return 1; // Overflow
    }

    json_writer_stack_entry * new_stack =
        (json_writer_stack_entry *)gtext_allocator_realloc(                     
            w->opts.allocator, w->stack, new_capacity * entry_size);
    if (!new_stack) {
      return 1; // Out of memory
    }

    w->stack = new_stack;
    w->stack_capacity = new_capacity;
  }
  return 0;
}

// Push a stack entry
static int writer_push_stack(
    GTEXT_JSON_Writer * w, json_writer_stack_type type) {
  if (writer_ensure_stack(w) != 0) {
    return 1;
  }

  json_writer_stack_entry entry = {.type = type,
      .has_elements = 0,
      .expecting_key = (type == JSON_WRITER_STACK_OBJECT) ? 1 : 0};

  w->stack[w->stack_size++] = entry;
  return 0;
}

// Pop a stack entry
static int writer_pop_stack(GTEXT_JSON_Writer * w) {
  if (w->stack_size == 0) {
    return 1; // Stack underflow
  }
  w->stack_size--;
  return 0;
}

// Get top stack entry (or NULL if empty)
static json_writer_stack_entry * writer_top_stack(GTEXT_JSON_Writer * w) {
  if (w->stack_size == 0) {
    return NULL;
  }
  return &w->stack[w->stack_size - 1];
}

// Write comma if needed (before next element)
static int writer_write_comma_if_needed(GTEXT_JSON_Writer * w) {
  json_writer_stack_entry * top = writer_top_stack(w);
  if (!top) {
    return 0; // No stack, no comma needed
  }

  /* A separator belongs before an *element*, and in an object the element
     starts at the key - so a value that follows a key must not write one. Every
     writer of a value calls this, including object_begin() and array_begin()
     when the container is itself an object's value, and `has_elements` cannot
     tell them apart: it is a property of the container, where the question is
     about the position.
     Without this an object of two or more members came out as
     {"a":1,"b":,2} - invalid JSON, from a sequence of calls that each returned
     GTEXT_JSON_OK, with gtext_json_writer_finish() reporting OK too. A single
     member was right by luck, because has_elements is still 0 when the first
     value is written, which is why the shape had to be two. */
  if (top->type == JSON_WRITER_STACK_OBJECT && !top->expecting_key) {
    return 0; // after a key: gtext_json_writer_key() wrote the separator
  }

  if (top->has_elements) {
    if (writer_write_char(w, ',') != 0) {
      return 1;
    }
    if (w->opts.pretty) {
      // Write newline and indent for next element
      // Check that stack_size fits in int (defensive, should always be true due
      // to capacity limit)
      if (w->stack_size > (size_t)INT_MAX) {
        return 1; // Stack size too large
      }
      if (writer_write_indent(w, (int)w->stack_size) != 0) {
        return 1;
      }
    }
    else if (w->opts.space_after_comma) {
      // In compact mode, add space after comma only if explicitly requested
      if (writer_write_char(w, ' ') != 0) {
        return 1;
      }
    }
  }
  else {
    // First element - write indent if pretty
    if (w->opts.pretty) {
      // Check that stack_size fits in int (defensive, should always be true due
      // to capacity limit)
      if (w->stack_size > (size_t)INT_MAX) {
        return 1; // Stack size too large
      }
      if (writer_write_indent(w, (int)w->stack_size) != 0) {
        return 1;
      }
    }
  }
  return 0;
}

/*
 * Everything that has to happen before a value is written, as one status.
 *
 * Two things: the comma that separates elements of a container, and - at the
 * top level - the question of whether a second value is allowed here at all.
 *
 * That second question used to have no answer. This writer accepted a second
 * top-level value and wrote `{"a":1}{"b":2}`, which is not JSON, which this
 * library's own parser refuses, and which every call reported
 * GTEXT_JSON_OK for, gtext_json_writer_finish() included. So the default is
 * now a refusal, and a sequence of values is something a caller asks for by
 * naming a framing in GTEXT_JSON_Write_Options::records - at which point the
 * bytes between the records are written rather than left out.
 */
static GTEXT_JSON_Status writer_begin_value(GTEXT_JSON_Writer * w) {
  if (w->stack_size == 0) {
    /* A top-level value. */
    if (w->opts.records == GTEXT_JSON_RECORDS_OFF) {
      if (w->roots_written > 0) {
        w->error = 1;
        return GTEXT_JSON_E_STATE;
      }
    }
    else {
      if (w->opts.records == GTEXT_JSON_RECORDS_LINE && w->opts.pretty) {
        /* One record per line and a value printed across lines are two
           requests that contradict each other. Refused rather than written,
           because the output would be a file this library's own
           GTEXT_JSON_RECORDS_LINE reader could not read back. */
        w->error = 1;
        return GTEXT_JSON_E_INVALID;
      }

      /* The previous record's terminator, deferred to here: writing it when
         that record closed would need a hook in every value writer and in both
         container-end functions, where this is the one place a record boundary
         is actually crossed. gtext_json_writer_finish() writes the last one. */
      if (w->records_pending) {
        const char * newline = w->opts.newline ? w->opts.newline : "\n";
        if (writer_write_string(w, newline) != 0) {
          w->error = 1;
          return GTEXT_JSON_E_WRITE;
        }
        w->records_pending = 0;
      }

      /* RFC 7464 puts its RS before every record, the first included. */
      if (w->opts.records == GTEXT_JSON_RECORDS_SEQ) {
        if (writer_write_char(w, 0x1E) != 0) {
          w->error = 1;
          return GTEXT_JSON_E_WRITE;
        }
      }
      w->records_pending = 1;
    }
    w->roots_written++;
  }

  if (writer_write_comma_if_needed(w) != 0) {
    w->error = 1;
    return GTEXT_JSON_E_WRITE;
  }
  return GTEXT_JSON_OK;
}

GTEXT_API GTEXT_JSON_Writer * gtext_json_writer_new(
    GTEXT_JSON_Sink sink, const GTEXT_JSON_Write_Options * opt) {
  if (!sink.write) {
    return NULL;
  }

  /* **The allocator is read from @p opt and not from w->opts**, because the
     handle itself is allocated before w->opts exists.  Reading it back from
     the copy would be the csv_field_buffer_init() mistake in a new place: an
     allocator assigned after the allocation it is meant to serve.  The copy is
     what gtext_json_writer_free() reads, which is sound because w->opts is set
     here and never cleared. */
  const GTEXT_Allocator * alloc = opt ? opt->allocator : NULL;

  GTEXT_JSON_Writer * w = (GTEXT_JSON_Writer *)gtext_allocator_calloc(
      alloc, 1, sizeof(GTEXT_JSON_Writer));
  if (!w) {
    return NULL;
  }

  w->sink = sink;
  if (opt) {
    w->opts = *opt;
  }
  else {
    w->opts = gtext_json_write_options_default();
  }

  w->stack_capacity = JSON_WRITER_DEFAULT_STACK_CAPACITY;
  w->stack = (json_writer_stack_entry *)gtext_allocator_calloc(
      alloc, w->stack_capacity, sizeof(json_writer_stack_entry));
  if (!w->stack) {
    gtext_allocator_free(alloc, w);
    return NULL;
  }

  w->stack_size = 0;
  w->error = 0;
  w->roots_written = 0;
  w->records_pending = 0;

  return w;
}

GTEXT_API void gtext_json_writer_free(GTEXT_JSON_Writer * w) {
  if (!w) {
    return;
  }

  if (w->stack) {
    gtext_allocator_free(w->opts.allocator, w->stack);
  }
  gtext_allocator_free(w->opts.allocator, w);
}

GTEXT_API GTEXT_JSON_Status gtext_json_writer_object_begin(
    GTEXT_JSON_Writer * w) {
  if (!w) {
    return GTEXT_JSON_E_INVALID;
  }

  if (w->error) {
    return GTEXT_JSON_E_STATE;
  }

  // Check if we're in an object expecting a key (can't write value without key)
  json_writer_stack_entry * top = writer_top_stack(w);
  if (top && top->type == JSON_WRITER_STACK_OBJECT && top->expecting_key) {
    return GTEXT_JSON_E_STATE; // Can't start object as value - need key first
  }

  // Write comma if needed (for arrays) or handle first element
  {
    const GTEXT_JSON_Status begin = writer_begin_value(w);
    if (begin != GTEXT_JSON_OK) {
      return begin;
    }
  }

  // Write opening brace
  if (writer_write_char(w, '{') != 0) {
    w->error = 1;
    return GTEXT_JSON_E_WRITE;
  }

  // Push object onto stack
  if (writer_push_stack(w, JSON_WRITER_STACK_OBJECT) != 0) {
    w->error = 1;
    return GTEXT_JSON_E_OOM;
  }

  return GTEXT_JSON_OK;
}

GTEXT_API GTEXT_JSON_Status gtext_json_writer_object_end(
    GTEXT_JSON_Writer * w) {
  if (!w) {
    return GTEXT_JSON_E_INVALID;
  }

  if (w->error) {
    return GTEXT_JSON_E_STATE;
  }

  json_writer_stack_entry * top = writer_top_stack(w);
  if (!top || top->type != JSON_WRITER_STACK_OBJECT) {
    return GTEXT_JSON_E_STATE; // Not in an object
  }

  if (!top->expecting_key) {
    return GTEXT_JSON_E_STATE; // Incomplete: expecting value after key
  }

  // Write closing brace
  if (w->opts.pretty && top->has_elements) {
    // Indent to parent level (stack_size - 1)
    int indent_depth = (int)w->stack_size - 1;
    if (indent_depth < 0)
      indent_depth = 0;
    if (writer_write_indent(w, indent_depth) != 0) {
      w->error = 1;
      return GTEXT_JSON_E_WRITE;
    }
  }

  if (writer_write_char(w, '}') != 0) {
    w->error = 1;
    return GTEXT_JSON_E_WRITE;
  }

  // Pop object from stack
  if (writer_pop_stack(w) != 0) {
    w->error = 1;
    return GTEXT_JSON_E_STATE;
  }

  // Mark parent as having elements and reset expecting_key if it's an object
  top = writer_top_stack(w);
  if (top) {
    top->has_elements = 1;
    // If parent is an object, we just finished writing a value, so reset to
    // expecting key
    if (top->type == JSON_WRITER_STACK_OBJECT) {
      top->expecting_key = 1;
    }
  }

  return GTEXT_JSON_OK;
}

GTEXT_API GTEXT_JSON_Status gtext_json_writer_array_begin(
    GTEXT_JSON_Writer * w) {
  if (!w) {
    return GTEXT_JSON_E_INVALID;
  }

  if (w->error) {
    return GTEXT_JSON_E_STATE;
  }

  // Check if we're in an object expecting a key (can't write value without key)
  json_writer_stack_entry * top = writer_top_stack(w);
  if (top && top->type == JSON_WRITER_STACK_OBJECT && top->expecting_key) {
    return GTEXT_JSON_E_STATE; // Can't start array as value - need key first
  }

  // Write comma if needed (for arrays) or handle first element
  {
    const GTEXT_JSON_Status begin = writer_begin_value(w);
    if (begin != GTEXT_JSON_OK) {
      return begin;
    }
  }

  // Write opening bracket
  if (writer_write_char(w, '[') != 0) {
    w->error = 1;
    return GTEXT_JSON_E_WRITE;
  }

  // Push array onto stack
  if (writer_push_stack(w, JSON_WRITER_STACK_ARRAY) != 0) {
    w->error = 1;
    return GTEXT_JSON_E_OOM;
  }

  return GTEXT_JSON_OK;
}

GTEXT_API GTEXT_JSON_Status gtext_json_writer_array_end(GTEXT_JSON_Writer * w) {
  if (!w) {
    return GTEXT_JSON_E_INVALID;
  }

  if (w->error) {
    return GTEXT_JSON_E_STATE;
  }

  json_writer_stack_entry * top = writer_top_stack(w);
  if (!top || top->type != JSON_WRITER_STACK_ARRAY) {
    return GTEXT_JSON_E_STATE; // Not in an array
  }

  // Write closing bracket
  if (w->opts.pretty && top->has_elements) {
    // Indent to parent level (stack_size - 1)
    int indent_depth = (int)w->stack_size - 1;
    if (indent_depth < 0)
      indent_depth = 0;
    if (writer_write_indent(w, indent_depth) != 0) {
      w->error = 1;
      return GTEXT_JSON_E_WRITE;
    }
  }

  if (writer_write_char(w, ']') != 0) {
    w->error = 1;
    return GTEXT_JSON_E_WRITE;
  }

  // Pop array from stack
  if (writer_pop_stack(w) != 0) {
    w->error = 1;
    return GTEXT_JSON_E_STATE;
  }

  // Mark parent as having elements and reset expecting_key if it's an object
  top = writer_top_stack(w);
  if (top) {
    top->has_elements = 1;
    // If parent is an object, we just finished writing a value, so reset to
    // expecting key
    if (top->type == JSON_WRITER_STACK_OBJECT) {
      top->expecting_key = 1;
    }
  }

  return GTEXT_JSON_OK;
}

GTEXT_API GTEXT_JSON_Status gtext_json_writer_key(
    GTEXT_JSON_Writer * w, const char * key, size_t len) {
  if (!w || !key) {
    return GTEXT_JSON_E_INVALID;
  }

  if (w->error) {
    return GTEXT_JSON_E_STATE;
  }

  json_writer_stack_entry * top = writer_top_stack(w);
  if (!top || top->type != JSON_WRITER_STACK_OBJECT) {
    return GTEXT_JSON_E_STATE; // Not in an object
  }

  if (!top->expecting_key) {
    return GTEXT_JSON_E_STATE; // Not expecting a key
  }

  // Write comma if needed
  {
    const GTEXT_JSON_Status begin = writer_begin_value(w);
    if (begin != GTEXT_JSON_OK) {
      return begin;
    }
  }

  // Write key
  {
    const int esc = write_escaped_string(&w->sink, key, len, &w->opts);
    if (esc == JSON_WRITE_ERR_UNICODE) {
      w->error = 1;
      return GTEXT_JSON_E_BAD_UNICODE;
    }
    if (esc != 0) {
      w->error = 1;
      return GTEXT_JSON_E_WRITE;
    }
  }

  // Write colon with optional spacing
  if (w->opts.pretty) {
    if (writer_write_string(w, ": ") != 0) {
      w->error = 1;
      return GTEXT_JSON_E_WRITE;
    }
  }
  else {
    if (writer_write_char(w, ':') != 0) {
      w->error = 1;
      return GTEXT_JSON_E_WRITE;
    }
    if (w->opts.space_after_colon) {
      if (writer_write_char(w, ' ') != 0) {
        w->error = 1;
        return GTEXT_JSON_E_WRITE;
      }
    }
  }

  // Now expecting value
  top->expecting_key = 0;

  return GTEXT_JSON_OK;
}

GTEXT_API GTEXT_JSON_Status gtext_json_writer_null(GTEXT_JSON_Writer * w) {
  if (!w) {
    return GTEXT_JSON_E_INVALID;
  }

  if (w->error) {
    return GTEXT_JSON_E_STATE;
  }

  json_writer_stack_entry * top = writer_top_stack(w);

  // If in object, must have written key first
  if (top && top->type == JSON_WRITER_STACK_OBJECT) {
    if (top->expecting_key) {
      w->error = 1;
      return GTEXT_JSON_E_STATE; // Must write key before value
    }
  }

  // Write comma if needed
  {
    const GTEXT_JSON_Status begin = writer_begin_value(w);
    if (begin != GTEXT_JSON_OK) {
      return begin;
    }
  }

  // Write null
  if (writer_write_string(w, "null") != 0) {
    w->error = 1;
    return GTEXT_JSON_E_WRITE;
  }

  // Mark current container as having elements
  if (top) {
    top->has_elements = 1;
  }

  // If in object, reset to expecting key
  if (top && top->type == JSON_WRITER_STACK_OBJECT) {
    top->expecting_key = 1;
  }

  return GTEXT_JSON_OK;
}

GTEXT_API GTEXT_JSON_Status gtext_json_writer_bool(
    GTEXT_JSON_Writer * w, bool b) {
  if (!w) {
    return GTEXT_JSON_E_INVALID;
  }

  if (w->error) {
    return GTEXT_JSON_E_STATE;
  }

  json_writer_stack_entry * top = writer_top_stack(w);

  // If in object, must have written key first
  if (top && top->type == JSON_WRITER_STACK_OBJECT) {
    if (top->expecting_key) {
      w->error = 1;
      return GTEXT_JSON_E_STATE; // Must write key before value
    }
  }

  // Write comma if needed
  {
    const GTEXT_JSON_Status begin = writer_begin_value(w);
    if (begin != GTEXT_JSON_OK) {
      return begin;
    }
  }

  // Write boolean
  if (writer_write_string(w, b ? "true" : "false") != 0) {
    w->error = 1;
    return GTEXT_JSON_E_WRITE;
  }

  // Mark current container as having elements
  if (top) {
    top->has_elements = 1;
  }

  // If in object, reset to expecting key
  if (top && top->type == JSON_WRITER_STACK_OBJECT) {
    top->expecting_key = 1;
  }

  return GTEXT_JSON_OK;
}

GTEXT_API GTEXT_JSON_Status gtext_json_writer_number_lexeme(
    GTEXT_JSON_Writer * w, const char * s, size_t len) {
  if (!w || !s) {
    return GTEXT_JSON_E_INVALID;
  }

  if (w->error) {
    return GTEXT_JSON_E_STATE;
  }

  json_writer_stack_entry * top = writer_top_stack(w);

  // If in object, must have written key first
  if (top && top->type == JSON_WRITER_STACK_OBJECT) {
    if (top->expecting_key) {
      w->error = 1;
      return GTEXT_JSON_E_STATE; // Must write key before value
    }
  }

  // Write comma if needed
  {
    const GTEXT_JSON_Status begin = writer_begin_value(w);
    if (begin != GTEXT_JSON_OK) {
      return begin;
    }
  }

  // Write number lexeme
  if (writer_write_bytes(w, s, len) != 0) {
    w->error = 1;
    return GTEXT_JSON_E_WRITE;
  }

  // Mark current container as having elements
  if (top) {
    top->has_elements = 1;
  }

  // If in object, reset to expecting key
  if (top && top->type == JSON_WRITER_STACK_OBJECT) {
    top->expecting_key = 1;
  }

  return GTEXT_JSON_OK;
}

GTEXT_API GTEXT_JSON_Status gtext_json_writer_number_i64(
    GTEXT_JSON_Writer * w, long long x) {
  if (!w) {
    return GTEXT_JSON_E_INVALID;
  }

  if (w->error) {
    return GTEXT_JSON_E_STATE;
  }

  json_writer_stack_entry * top = writer_top_stack(w);

  // If in object, must have written key first
  if (top && top->type == JSON_WRITER_STACK_OBJECT) {
    if (top->expecting_key) {
      w->error = 1;
      return GTEXT_JSON_E_STATE; // Must write key before value
    }
  }

  // Write comma if needed
  {
    const GTEXT_JSON_Status begin = writer_begin_value(w);
    if (begin != GTEXT_JSON_OK) {
      return begin;
    }
  }

  // Format number (locale-independent)
  char num_buf[64];
  int len = gtext_number_format_i64(num_buf, sizeof(num_buf), (int64_t)x);
  if (len < 0 || (size_t)len >= sizeof(num_buf)) {
    w->error = 1;
    return GTEXT_JSON_E_WRITE;
  }

  if (writer_write_bytes(w, num_buf, (size_t)len) != 0) {
    w->error = 1;
    return GTEXT_JSON_E_WRITE;
  }

  // Mark current container as having elements
  if (top) {
    top->has_elements = 1;
  }

  // If in object, reset to expecting key
  if (top && top->type == JSON_WRITER_STACK_OBJECT) {
    top->expecting_key = 1;
  }

  return GTEXT_JSON_OK;
}

GTEXT_API GTEXT_JSON_Status gtext_json_writer_number_u64(
    GTEXT_JSON_Writer * w, unsigned long long x) {
  if (!w) {
    return GTEXT_JSON_E_INVALID;
  }

  if (w->error) {
    return GTEXT_JSON_E_STATE;
  }

  json_writer_stack_entry * top = writer_top_stack(w);

  // If in object, must have written key first
  if (top && top->type == JSON_WRITER_STACK_OBJECT) {
    if (top->expecting_key) {
      w->error = 1;
      return GTEXT_JSON_E_STATE; // Must write key before value
    }
  }

  // Write comma if needed
  {
    const GTEXT_JSON_Status begin = writer_begin_value(w);
    if (begin != GTEXT_JSON_OK) {
      return begin;
    }
  }

  // Format number (locale-independent)
  char num_buf[64];
  int len = gtext_number_format_u64(num_buf, sizeof(num_buf), (uint64_t)x);
  if (len < 0 || (size_t)len >= sizeof(num_buf)) {
    w->error = 1;
    return GTEXT_JSON_E_WRITE;
  }

  if (writer_write_bytes(w, num_buf, (size_t)len) != 0) {
    w->error = 1;
    return GTEXT_JSON_E_WRITE;
  }

  // Mark current container as having elements
  if (top) {
    top->has_elements = 1;
  }

  // If in object, reset to expecting key
  if (top && top->type == JSON_WRITER_STACK_OBJECT) {
    top->expecting_key = 1;
  }

  return GTEXT_JSON_OK;
}

GTEXT_API GTEXT_JSON_Status gtext_json_writer_number_double(
    GTEXT_JSON_Writer * w, double x) {
  if (!w) {
    return GTEXT_JSON_E_INVALID;
  }

  if (w->error) {
    return GTEXT_JSON_E_STATE;
  }

  json_writer_stack_entry * top = writer_top_stack(w);

  // If in object, must have written key first
  if (top && top->type == JSON_WRITER_STACK_OBJECT) {
    if (top->expecting_key) {
      w->error = 1;
      return GTEXT_JSON_E_STATE; // Must write key before value
    }
  }

  // Write comma if needed
  {
    const GTEXT_JSON_Status begin = writer_begin_value(w);
    if (begin != GTEXT_JSON_OK) {
      return begin;
    }
  }

  // Check for nonfinite numbers
  if (!isfinite(x)) {
    if (!w->opts.allow_nonfinite_numbers) {
      w->error = 1;
      return GTEXT_JSON_E_NONFINITE;
    }
    if (isnan(x)) {
      if (writer_write_string(w, "NaN") != 0) {
        w->error = 1;
        return GTEXT_JSON_E_WRITE;
      }
    }
    else if (isinf(x)) {
      if (x < 0) {
        if (writer_write_string(w, "-Infinity") != 0) {
          w->error = 1;
          return GTEXT_JSON_E_WRITE;
        }
      }
      else {
        if (writer_write_string(w, "Infinity") != 0) {
          w->error = 1;
          return GTEXT_JSON_E_WRITE;
        }
      }
    }
  }
  else {
    // Format finite number (locale-independent, with configurable format)
    char num_buf[64];
    GTEXT_JSON_Float_Format float_fmt = w->opts.float_format;
    int float_prec = w->opts.float_precision > 0 ? w->opts.float_precision : 6;
    int len = format_double(num_buf, sizeof(num_buf), x, float_fmt, float_prec);
    if (len < 0 || (size_t)len >= sizeof(num_buf)) {
      w->error = 1;
      return GTEXT_JSON_E_WRITE;
    }

    if (writer_write_bytes(w, num_buf, (size_t)len) != 0) {
      w->error = 1;
      return GTEXT_JSON_E_WRITE;
    }
  }

  // Mark current container as having elements
  if (top) {
    top->has_elements = 1;
  }

  // If in object, reset to expecting key
  if (top && top->type == JSON_WRITER_STACK_OBJECT) {
    top->expecting_key = 1;
  }

  return GTEXT_JSON_OK;
}

GTEXT_API GTEXT_JSON_Status gtext_json_writer_string(
    GTEXT_JSON_Writer * w, const char * s, size_t len) {
  if (!w || !s) {
    return GTEXT_JSON_E_INVALID;
  }

  if (w->error) {
    return GTEXT_JSON_E_STATE;
  }

  json_writer_stack_entry * top = writer_top_stack(w);

  // If in object, must have written key first
  if (top && top->type == JSON_WRITER_STACK_OBJECT) {
    if (top->expecting_key) {
      w->error = 1;
      return GTEXT_JSON_E_STATE; // Must write key before value
    }
  }

  // Write comma if needed
  {
    const GTEXT_JSON_Status begin = writer_begin_value(w);
    if (begin != GTEXT_JSON_OK) {
      return begin;
    }
  }

  // Write escaped string
  {
    const int esc = write_escaped_string(&w->sink, s, len, &w->opts);
    if (esc == JSON_WRITE_ERR_UNICODE) {
      w->error = 1;
      return GTEXT_JSON_E_BAD_UNICODE;
    }
    if (esc != 0) {
      w->error = 1;
      return GTEXT_JSON_E_WRITE;
    }
  }

  // Mark current container as having elements
  if (top) {
    top->has_elements = 1;
  }

  // If in object, reset to expecting key
  if (top && top->type == JSON_WRITER_STACK_OBJECT) {
    top->expecting_key = 1;
  }

  return GTEXT_JSON_OK;
}

GTEXT_API GTEXT_JSON_Status gtext_json_writer_finish(
    GTEXT_JSON_Writer * w, GTEXT_JSON_Error * err) {
  if (!w) {
    if (err) {
      *err = (GTEXT_JSON_Error){
          .code = GTEXT_JSON_E_INVALID, .message = "Writer is NULL"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  if (w->error) {
    if (err) {
      *err = (GTEXT_JSON_Error){
          .code = GTEXT_JSON_E_STATE, .message = "Writer is in error state"};
    }
    return GTEXT_JSON_E_STATE;
  }

  // Check if stack is empty (structure is complete)
  if (w->stack_size != 0) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INCOMPLETE,
          .message = "Incomplete JSON structure: unclosed containers"};
    }
    return GTEXT_JSON_E_INCOMPLETE;
  }

  /* The last record's terminator. Every record in a records mode is followed
     by one, including the last: a reader of NDJSON expects the final line to
     end, and RFC 7464's grammar puts an LF after each record. This is the
     deferred write writer_begin_value() would otherwise have made at the start
     of a record that never came. */
  if (w->records_pending) {
    const char * newline = w->opts.newline ? w->opts.newline : "\n";
    if (writer_write_string(w, newline) != 0) {
      w->error = 1;
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_WRITE,
            .message = "Failed to write the record terminator"};
      }
      return GTEXT_JSON_E_WRITE;
    }
    w->records_pending = 0;
  }

  /* Not also the trailing newline in a records mode: the record terminator
     above already ended the last record, and writing both would leave the
     output ending in a blank line that a reader of this format reads as one
     more separator. The same subsumption gtext_json_write_value() makes. */
  if (w->opts.trailing_newline
      && w->opts.records == GTEXT_JSON_RECORDS_OFF) {
    const char * newline = w->opts.newline ? w->opts.newline : "\n";
    if (writer_write_string(w, newline) != 0) {
      w->error = 1;
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_WRITE,
            .message = "Failed to write trailing newline"};
      }
      return GTEXT_JSON_E_WRITE;
    }
  }

  return GTEXT_JSON_OK;
}
