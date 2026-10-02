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
 * CSV writer infrastructure implementation.
 *
 * This file implements the sink abstraction for writing CSV output
 * to various destinations.
 */

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/macros.h>
#include "csv_internal.h"

#include <ghoti.io/text/csv/csv_core.h>
#include <ghoti.io/text/csv/csv_writer.h>
// Internal write callback for growable buffer sink
static GTEXT_CSV_Status buffer_write_fn(
    void * user, const char * bytes, size_t len) {
  GTEXT_CSV_Buffer_Sink * buf = (GTEXT_CSV_Buffer_Sink *)user;
  if (!buf || !bytes) {
    return GTEXT_CSV_E_INVALID;
  }

  // Check for integer overflow in needed calculation
  if (len > SIZE_MAX - buf->used || buf->used > SIZE_MAX - len - 1) {
    return GTEXT_CSV_E_LIMIT; // Overflow
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
      // Unreachable on any 64-bit host, as in json_writer.c: the buffer would
      // have to be eight exbibytes first.  Kept as the statement of what makes
      // the doubling safe.
      if (new_size > SIZE_MAX / 2) {
        return GTEXT_CSV_E_OOM; // Overflow - cannot grow further
      }
      new_size *= 2;
    }

    /* allocator-exempt: a buffer sink owns its buffer.  A sink is created
       before any write options are seen - gtext_csv_sink_buffer() takes none -
       and it outlives the write, so there is no caller allocator to read here
       and routing it through one would mean freeing through whichever options
       happened to be passed last.  Same line GTEXT_JSON_Write_Options and
       GTEXT_INI_Write_Options draw. */
    char * new_data =
        (char *)realloc(buf->data, new_size); // allocator-exempt
    if (!new_data) {
      return GTEXT_CSV_E_OOM; // Out of memory
    }
    buf->data = new_data;
    buf->size = new_size;
  }

  // Verify bounds before copying (defensive check)
  if (buf->used + len > buf->size - 1) {
    return GTEXT_CSV_E_LIMIT; // Should not happen, but be safe
  }

  // Copy data
  memcpy(buf->data + buf->used, bytes, len);
  buf->used += len;
  // Verify bounds before writing null terminator
  if (buf->used < buf->size) {
    buf->data[buf->used] = '\0'; // Null terminate for convenience
  }

  return GTEXT_CSV_OK; // Success
}

// Internal write callback for fixed buffer sink
static GTEXT_CSV_Status fixed_buffer_write_fn(
    void * user, const char * bytes, size_t len) {
  GTEXT_CSV_Fixed_Buffer_Sink * buf = (GTEXT_CSV_Fixed_Buffer_Sink *)user;
  if (!buf || !bytes) {
    return GTEXT_CSV_E_INVALID;
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
  bool truncated = false;

  if (to_write > available) {
    to_write = available;
    truncated = true;
    buf->truncated = true;
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
      truncated = true;
      buf->truncated = true;
    }
  }

  // Return error if truncation occurred
  return truncated ? GTEXT_CSV_E_WRITE : GTEXT_CSV_OK;
}

GTEXT_API GTEXT_CSV_Status gtext_csv_sink_buffer(GTEXT_CSV_Sink * sink) {
  if (!sink) {
    return GTEXT_CSV_E_INVALID;
  }

  GTEXT_CSV_Buffer_Sink * buf =
      (GTEXT_CSV_Buffer_Sink *)malloc( // allocator-exempt: it owns its buffer
          sizeof(GTEXT_CSV_Buffer_Sink));
  if (!buf) {
    return GTEXT_CSV_E_OOM;
  }

  buf->data = NULL;
  buf->size = 0;
  buf->used = 0;

  sink->write = buffer_write_fn;
  sink->user = buf;

  return GTEXT_CSV_OK;
}

GTEXT_API const char * gtext_csv_sink_buffer_data(const GTEXT_CSV_Sink * sink) {
  if (!sink || sink->write != buffer_write_fn) {
    return NULL;
  }

  GTEXT_CSV_Buffer_Sink * buf = (GTEXT_CSV_Buffer_Sink *)sink->user;
  if (!buf) {
    return NULL;
  }

  return buf->data ? buf->data : "";
}

GTEXT_API size_t gtext_csv_sink_buffer_size(const GTEXT_CSV_Sink * sink) {
  if (!sink || sink->write != buffer_write_fn) {
    return 0;
  }

  GTEXT_CSV_Buffer_Sink * buf = (GTEXT_CSV_Buffer_Sink *)sink->user;
  if (!buf) {
    return 0;
  }

  return buf->used;
}

GTEXT_API void gtext_csv_sink_buffer_free(GTEXT_CSV_Sink * sink) {
  if (!sink || sink->write != buffer_write_fn) {
    return;
  }

  GTEXT_CSV_Buffer_Sink * buf = (GTEXT_CSV_Buffer_Sink *)sink->user;
  if (buf) {
    free(buf->data); // allocator-exempt
    free(buf);       // allocator-exempt
    sink->user = NULL;
    sink->write = NULL;
  }
}

GTEXT_API GTEXT_CSV_Status gtext_csv_sink_fixed_buffer(
    GTEXT_CSV_Sink * sink, char * buffer, size_t size) {
  if (!sink || !buffer || size == 0) {
    return GTEXT_CSV_E_INVALID;
  }

  GTEXT_CSV_Fixed_Buffer_Sink * buf =
      (GTEXT_CSV_Fixed_Buffer_Sink *)malloc( // allocator-exempt
          sizeof(GTEXT_CSV_Fixed_Buffer_Sink));
  if (!buf) {
    return GTEXT_CSV_E_OOM;
  }

  buf->data = buffer;
  buf->size = size;
  buf->used = 0;
  buf->truncated = false;

  // Initialize buffer with null terminator
  buffer[0] = '\0';

  sink->write = fixed_buffer_write_fn;
  sink->user = buf;

  return GTEXT_CSV_OK;
}

GTEXT_API size_t gtext_csv_sink_fixed_buffer_used(const GTEXT_CSV_Sink * sink) {
  if (!sink || sink->write != fixed_buffer_write_fn) {
    return 0;
  }

  GTEXT_CSV_Fixed_Buffer_Sink * buf = (GTEXT_CSV_Fixed_Buffer_Sink *)sink->user;
  if (!buf) {
    return 0;
  }

  return buf->used;
}

GTEXT_API bool gtext_csv_sink_fixed_buffer_truncated(
    const GTEXT_CSV_Sink * sink) {
  if (!sink || sink->write != fixed_buffer_write_fn) {
    return false;
  }

  GTEXT_CSV_Fixed_Buffer_Sink * buf = (GTEXT_CSV_Fixed_Buffer_Sink *)sink->user;
  if (!buf) {
    return false;
  }

  return buf->truncated;
}

GTEXT_API void gtext_csv_sink_fixed_buffer_free(GTEXT_CSV_Sink * sink) {
  if (!sink || sink->write != fixed_buffer_write_fn) {
    return;
  }

  GTEXT_CSV_Fixed_Buffer_Sink * buf = (GTEXT_CSV_Fixed_Buffer_Sink *)sink->user;
  if (buf) {
    free(buf); // allocator-exempt
    sink->user = NULL;
    sink->write = NULL;
  }
}

// ============================================================================
// Field Escaping and Quoting Logic
// ============================================================================

/*
 * Does this field's text spell a number?
 *
 * GTEXT_CSV_QUOTE_NONNUMERIC's question, and it is deliberately about the bytes
 * rather than about a type: this module infers no types, so where Python asks
 * whether the value is an int or a float, there is no value here to ask about -
 * only text. The grammar is stated in the header and is checked against the
 * whole field:
 *
 *   [+-]? ( digits ( '.' digits? )? | '.' digits ) ( [eE] [+-]? digits )?
 *
 * Narrower than C's strtod on purpose. Hex floats, `inf` and `nan` are
 * spellings a spreadsheet does not write and a reader would not agree about,
 * and leaving them quoted costs nothing. `+1`, `007`, `1.` and `.5` are
 * accepted, because spreadsheets do write those.
 */
static bool csv_field_is_number(const char * field_data, size_t field_len) {
  if (!field_data || field_len == 0) {
    return false;
  }
  size_t i = 0;
  if (field_data[i] == '+' || field_data[i] == '-') {
    i++;
  }

  size_t int_digits = 0;
  while (i < field_len && field_data[i] >= '0' && field_data[i] <= '9') {
    i++;
    int_digits++;
  }

  size_t frac_digits = 0;
  if (i < field_len && field_data[i] == '.') {
    i++;
    while (i < field_len && field_data[i] >= '0' && field_data[i] <= '9') {
      i++;
      frac_digits++;
    }
    // "." alone, and "+." , are not numbers; one side or the other needs a
    // digit.
    if (int_digits == 0 && frac_digits == 0) {
      return false;
    }
  }
  else if (int_digits == 0) {
    return false;
  }

  if (i < field_len && (field_data[i] == 'e' || field_data[i] == 'E')) {
    i++;
    if (i < field_len && (field_data[i] == '+' || field_data[i] == '-')) {
      i++;
    }
    size_t exp_digits = 0;
    while (i < field_len && field_data[i] >= '0' && field_data[i] <= '9') {
      i++;
      exp_digits++;
    }
    if (exp_digits == 0) {
      return false; // "1e" and "1e+" are not numbers
    }
  }

  // Trailing anything at all disqualifies it: "1 ", "1abc", "1,000".
  return i == field_len;
}

static bool csv_field_needs_quoting(const char * field_data, size_t field_len,
    const GTEXT_CSV_Write_Options * opts) {
  if (!opts) {
    return false;
  }

  // quote_all_fields: always quote.  Checked before `quoting` so that code
  // written before the policy enum existed keeps behaving as it did.
  if (opts->quote_all_fields || opts->quoting == GTEXT_CSV_QUOTE_ALL) {
    return true;
  }

  // GTEXT_CSV_QUOTE_NONE consults nothing else.  What it cannot carry is
  // refused in csv_write_field rather than quoted here.
  if (opts->quoting == GTEXT_CSV_QUOTE_NONE) {
    return false;
  }

  // quote_empty_fields: quote if empty
  if (opts->quote_empty_fields && field_len == 0) {
    return true;
  }

  // Non-numeric text, when that is the policy.  A numeric field falls through
  // to the scan below rather than returning false, so a dialect whose
  // delimiter is one of `+-.eE0-9` still gets its quotes: this policy can only
  // ever add them.
  if (opts->quoting == GTEXT_CSV_QUOTE_NONNUMERIC
      && !csv_field_is_number(field_data, field_len)) {
    return true;
  }

  // quote_if_needed: check for special characters
  if (opts->quote_if_needed && field_data && field_len > 0) {
    const char delimiter = opts->dialect.delimiter;
    const char quote = opts->dialect.quote;

    for (size_t i = 0; i < field_len; i++) {
      char c = field_data[i];

      // Check for delimiter
      if (c == delimiter) {
        return true;
      }

      // Check for quote character
      if (c == quote) {
        return true;
      }

      // Check for newline (LF, CR, or CRLF)
      if (c == '\n' || c == '\r') {
        return true;
      }

      // Check for leading/trailing spaces if policy requires
      // (This is typically handled by the parser, but we check here
      // for completeness - RFC 4180 says spaces are significant)
    }
  }

  return false;
}

static size_t csv_field_escaped_length(const char * field_data,
    size_t field_len, GTEXT_CSV_Escape_Mode escape_mode, char quote_char) {
  if (!field_data || field_len == 0) {
    return field_len;
  }

  size_t escaped_len = field_len;

  switch (escape_mode) {
  case GTEXT_CSV_ESCAPE_DOUBLED_QUOTE: {
    // Each quote character becomes two quotes
    for (size_t i = 0; i < field_len; i++) {
      if (field_data[i] == quote_char) {
        // Check for overflow before incrementing
        if (escaped_len == SIZE_MAX) {
          return SIZE_MAX; // Overflow - return max value
        }
        escaped_len++; // One extra character per quote
      }
    }
    break;
  }

  case GTEXT_CSV_ESCAPE_BACKSLASH: {
    // Each quote becomes \" and each backslash becomes double backslash
    for (size_t i = 0; i < field_len; i++) {
      if (field_data[i] == quote_char || field_data[i] == '\\') {
        // Check for overflow before incrementing
        if (escaped_len == SIZE_MAX) {
          return SIZE_MAX; // Overflow - return max value
        }
        escaped_len++; // One extra character per quote or backslash
      }
    }
    break;
  }

  case GTEXT_CSV_ESCAPE_NONE: {
    // No escaping - length unchanged
    break;
  }

  default:
    // Unknown escape mode - treat as no escaping
    break;
  }

  return escaped_len;
}

static GTEXT_CSV_Status csv_field_escape(const char * field_data,
    size_t field_len, char * output_buffer, size_t output_buffer_size,
    GTEXT_CSV_Escape_Mode escape_mode, char quote_char, size_t * output_len) {
  if (!field_data || !output_buffer || !output_len) {
    return GTEXT_CSV_E_INVALID;
  }

  if (field_len == 0) {
    *output_len = 0;
    return GTEXT_CSV_OK;
  }

  size_t out_idx = 0;

  switch (escape_mode) {
  case GTEXT_CSV_ESCAPE_DOUBLED_QUOTE: {
    // Double each quote character
    for (size_t i = 0; i < field_len; i++) {
      if (out_idx + 1 > output_buffer_size) {
        return GTEXT_CSV_E_INVALID;
      }
      if (field_data[i] == quote_char) {
        // Write two quotes
        output_buffer[out_idx++] = quote_char;
        if (out_idx + 1 > output_buffer_size) {
          return GTEXT_CSV_E_INVALID;
        }
        output_buffer[out_idx++] = quote_char;
      }
      else {
        output_buffer[out_idx++] = field_data[i];
      }
    }
    break;
  }

  case GTEXT_CSV_ESCAPE_BACKSLASH: {
    // Escape quotes with backslash and backslashes with double backslash
    for (size_t i = 0; i < field_len; i++) {
      if (field_data[i] == quote_char) {
        // Write backslash followed by quote
        if (out_idx + 2 > output_buffer_size) {
          return GTEXT_CSV_E_INVALID;
        }
        output_buffer[out_idx++] = '\\';
        output_buffer[out_idx++] = quote_char;
      }
      else if (field_data[i] == '\\') {
        // Write double backslash
        if (out_idx + 2 > output_buffer_size) {
          return GTEXT_CSV_E_INVALID;
        }
        output_buffer[out_idx++] = '\\';
        output_buffer[out_idx++] = '\\';
      }
      else {
        if (out_idx + 1 > output_buffer_size) {
          return GTEXT_CSV_E_INVALID;
        }
        output_buffer[out_idx++] = field_data[i];
      }
    }
    break;
  }

  case GTEXT_CSV_ESCAPE_NONE: {
    // No escaping - copy as-is
    if (field_len > output_buffer_size) {
      return GTEXT_CSV_E_INVALID;
    }
    memcpy(output_buffer, field_data, field_len);
    out_idx = field_len;
    break;
  }

  default:
    // Unknown escape mode - copy as-is
    if (field_len > output_buffer_size) {
      return GTEXT_CSV_E_INVALID;
    }
    memcpy(output_buffer, field_data, field_len);
    out_idx = field_len;
    break;
  }

  *output_len = out_idx;
  return GTEXT_CSV_OK;
}

/*
 * Can this field go out without quotes and still mean the same thing?
 *
 * Quoting is what carries the delimiter, CR and LF through a CSV field, and no
 * escape mode is an alternative: measured, not assumed - with
 * GTEXT_CSV_ESCAPE_BACKSLASH the parser reads `a\,b` as the two fields `a\`
 * and `b`, exactly as the doubled-quote dialect does, because both escape
 * modes concern the quote character alone. So a caller who turns quoting off
 * and hands over a field holding one of those three has asked for a document
 * that cannot exist.
 *
 * It used to get one. Every case below returned GTEXT_CSV_OK and wrote bytes
 * that said something else: a field holding a comma came back as two fields, a
 * field holding a newline came back as two records, and a field holding a quote
 * came back as a document this library's own parser refuses.
 *
 * The quote character is the one conditional case. With always_escape_quotes
 * set, an unquoted field's quote is written as an escape - `""` or `\"` - and
 * neither spelling means a quote outside a quoted field, so that is refused
 * too. Cleared, the quote goes out verbatim, which is what the option is
 * documented to do and is readable by a parser with allow_unquoted_quotes on.
 * That one is the caller's call to make, so it is left to them.
 */
static bool csv_field_survives_unquoted(const char * field_data,
    size_t field_len, const GTEXT_CSV_Write_Options * opts) {
  if (!field_data || field_len == 0) {
    return true;
  }
  const char delimiter = opts->dialect.delimiter;
  const char quote = opts->dialect.quote;
  for (size_t i = 0; i < field_len; i++) {
    const char c = field_data[i];
    if (c == delimiter || c == '\n' || c == '\r') {
      return false;
    }
    if (c == quote && opts->always_escape_quotes
        && opts->dialect.escape != GTEXT_CSV_ESCAPE_NONE) {
      return false;
    }
  }
  return true;
}

GTEXT_INTERNAL_API GTEXT_CSV_Status csv_write_field(const GTEXT_CSV_Sink * sink,
    const char * field_data, size_t field_len,
    const GTEXT_CSV_Write_Options * opts) {
  if (!sink || !sink->write || !opts) {
    return GTEXT_CSV_E_INVALID;
  }

  const char quote_char = opts->dialect.quote;
  const GTEXT_CSV_Escape_Mode escape_mode = opts->dialect.escape;
  bool needs_quoting = csv_field_needs_quoting(field_data, field_len, opts);

  // Refuse before writing anything, rather than emitting a field that will
  // read back as something else.  Under the default options this is
  // unreachable: quote_if_needed makes every one of these characters force
  // quotes, so needs_quoting is already true.
  if (!needs_quoting
      && !csv_field_survives_unquoted(field_data, field_len, opts)) {
    return GTEXT_CSV_E_UNQUOTABLE_FIELD;
  }

  // If field doesn't need quoting and has no quotes to escape, write directly
  if (!needs_quoting && field_len > 0 && field_data) {
    // Check if field contains quotes that need escaping
    bool has_quotes = false;
    for (size_t i = 0; i < field_len; i++) {
      if (field_data[i] == quote_char) {
        has_quotes = true;
        break;
      }
    }

    // If no quotes and no special chars, write directly
    if (!has_quotes) {
      return sink->write(sink->user, field_data, field_len);
    }
  }

  // Field needs quoting or escaping - handle it
  if (needs_quoting) {
    // Write opening quote
    GTEXT_CSV_Status status = sink->write(sink->user, &quote_char, 1);
    if (status != GTEXT_CSV_OK) {
      return status;
    }

    // Escape and write field content
    if (field_len > 0 && field_data) {
      // Calculate escaped length
      size_t escaped_len = csv_field_escaped_length(
          field_data, field_len, escape_mode, quote_char);

      // Check for overflow in escaped length calculation
      if (escaped_len == SIZE_MAX || escaped_len < field_len) {
        // Overflow detected or invalid calculation - return error
        return GTEXT_CSV_E_LIMIT;
      }

      // Allocate temporary buffer for escaped field
      // For efficiency, use stack buffer for small fields
      // and heap buffer for large fields
      const size_t STACK_BUFFER_SIZE = 256;
      char * escape_buffer = NULL;
      bool use_stack = (escaped_len < STACK_BUFFER_SIZE);
      char stack_buffer[STACK_BUFFER_SIZE];

      if (use_stack) {
        escape_buffer = stack_buffer;
      }
      else {
        // Check for reasonable allocation size (prevent excessive memory
        // allocation) In practice, escaped_len should be reasonable, but be
        // defensive
        if (escaped_len > SIZE_MAX / 2) {
          // Avoid allocating more than half of addressable space
          return GTEXT_CSV_E_LIMIT;
        }
        escape_buffer =
            (char *)gtext_allocator_malloc(opts->allocator, escaped_len);
        if (!escape_buffer) {
          return GTEXT_CSV_E_OOM;
        }
      }

      size_t actual_escaped_len = 0;
      status = csv_field_escape(field_data, field_len, escape_buffer,
          escaped_len, escape_mode, quote_char, &actual_escaped_len);

      if (status == GTEXT_CSV_OK) {
        status = sink->write(sink->user, escape_buffer, actual_escaped_len);
      }

      if (!use_stack) {
        gtext_allocator_free(opts->allocator, escape_buffer);
      }

      if (status != GTEXT_CSV_OK) {
        return status;
      }
    }

    // Write closing quote
    return sink->write(sink->user, &quote_char, 1);
  }
  else {
    // The field is not being quoted, but it may still contain the quote
    // character.  RFC 4180 gives a quote inside an unquoted field no special
    // meaning, so escaping it is a choice rather than a requirement - which is
    // what always_escape_quotes selects.  It defaults to true, which is what
    // this code did unconditionally while the option was read by nothing.
    //
    // A quote inside a *quoted* field is not affected: leaving that one
    // unescaped would terminate the field early and produce malformed output,
    // so the quoted path above always escapes.
    if (opts->always_escape_quotes && escape_mode != GTEXT_CSV_ESCAPE_NONE &&
        field_len > 0 && field_data) {
      size_t escaped_len = csv_field_escaped_length(
          field_data, field_len, escape_mode, quote_char);

      // Check for overflow in escaped length calculation
      if (escaped_len == SIZE_MAX || escaped_len < field_len) {
        // Overflow detected or invalid calculation - return error
        return GTEXT_CSV_E_LIMIT;
      }

      if (escaped_len > field_len) {
        // Need to escape - use temporary buffer
        const size_t STACK_BUFFER_SIZE = 256;
        char * escape_buffer = NULL;
        bool use_stack = (escaped_len < STACK_BUFFER_SIZE);
        char stack_buffer[STACK_BUFFER_SIZE];

        if (use_stack) {
          escape_buffer = stack_buffer;
        }
        else {
          // Check for reasonable allocation size (prevent excessive memory
          // allocation)
          if (escaped_len > SIZE_MAX / 2) {
            // Avoid allocating more than half of addressable space
            return GTEXT_CSV_E_LIMIT;
          }
          escape_buffer =
              (char *)gtext_allocator_malloc(opts->allocator, escaped_len);
          if (!escape_buffer) {
            return GTEXT_CSV_E_OOM;
          }
        }

        size_t actual_escaped_len = 0;
        GTEXT_CSV_Status status =
            csv_field_escape(field_data, field_len, escape_buffer, escaped_len,
                escape_mode, quote_char, &actual_escaped_len);

        if (status == GTEXT_CSV_OK) {
          status = sink->write(sink->user, escape_buffer, actual_escaped_len);
        }

        if (!use_stack) {
          gtext_allocator_free(opts->allocator, escape_buffer);
        }

        return status;
      }
      else {
        // No escaping needed - write directly
        return sink->write(sink->user, field_data, field_len);
      }
    }
    else {
      // No escaping needed - write directly
      return sink->write(sink->user, field_data, field_len);
    }
  }
}

// ============================================================================
// Streaming Writer Implementation
// ============================================================================

GTEXT_API GTEXT_CSV_Writer * gtext_csv_writer_new(
    const GTEXT_CSV_Sink * sink, const GTEXT_CSV_Write_Options * opts) {
  if (!sink || !sink->write || !opts) {
    return NULL;
  }

  /* Read from @p opts and not from writer->opts, because the handle is
     allocated before writer->opts exists.  Reading it back from the copy would
     be an allocator assigned after the allocation it is meant to serve.  The
     copy is what gtext_csv_writer_free() reads, which is sound because
     writer->opts is set here and never cleared. */
  GTEXT_CSV_Writer * writer = (GTEXT_CSV_Writer *)gtext_allocator_malloc(
      opts->allocator, sizeof(GTEXT_CSV_Writer));
  if (!writer) {
    return NULL;
  }

  writer->sink = *sink; // Copy sink structure
  writer->opts = *opts; // Copy options
  writer->state = CSV_WRITER_STATE_INITIAL;
  writer->has_fields_in_record = false;
  writer->last_error = GTEXT_CSV_OK;
  writer->pending_empty_fields = 0;
  writer->pending_newline = false;

  return writer;
}

/** The record separator, or the terminator at finish(). */
static GTEXT_CSV_Status csv_writer_write_newline(GTEXT_CSV_Writer * writer) {
  const char * newline = writer->opts.newline ? writer->opts.newline : "\n";
  return writer->sink.write(writer->sink.user, newline, strlen(newline));
}

/** One field, with its preceding delimiter when it is not the first. */
static GTEXT_CSV_Status csv_writer_emit_field(
    GTEXT_CSV_Writer * writer, const char * bytes, size_t len) {
  if (writer->has_fields_in_record) {
    char delimiter = writer->opts.dialect.delimiter;
    GTEXT_CSV_Status status =
        writer->sink.write(writer->sink.user, &delimiter, 1);
    if (status != GTEXT_CSV_OK) {
      return status;
    }
  }
  GTEXT_CSV_Status status =
      csv_write_field(&writer->sink, bytes, len, &writer->opts);
  if (status != GTEXT_CSV_OK) {
    return status;
  }
  writer->has_fields_in_record = true;
  return GTEXT_CSV_OK;
}

/** Write the empty fields that were held back, now that one follows them. */
static GTEXT_CSV_Status csv_writer_flush_pending_empties(
    GTEXT_CSV_Writer * writer) {
  while (writer->pending_empty_fields > 0) {
    /* Written through the same path as any other field, so that
       quote_empty_fields and the quoting policy reach them. */
    GTEXT_CSV_Status status = csv_writer_emit_field(writer, "", 0);
    if (status != GTEXT_CSV_OK) {
      return status;
    }
    writer->pending_empty_fields--;
  }
  return GTEXT_CSV_OK;
}

GTEXT_API GTEXT_CSV_Status gtext_csv_writer_record_begin(
    GTEXT_CSV_Writer * writer) {
  if (!writer) {
    return GTEXT_CSV_E_INVALID;
  }

  // Check if already in a record
  if (writer->state == CSV_WRITER_STATE_IN_RECORD) {
    writer->last_error = GTEXT_CSV_E_INVALID;
    return GTEXT_CSV_E_INVALID;
  }

  // Check if already finished
  if (writer->state == CSV_WRITER_STATE_FINISHED) {
    writer->last_error = GTEXT_CSV_E_INVALID;
    return GTEXT_CSV_E_INVALID;
  }

  /* The previous record's separator, held back because at its record_end()
     this writer could not know whether another record would follow. */
  if (writer->pending_newline) {
    GTEXT_CSV_Status status = csv_writer_write_newline(writer);
    if (status != GTEXT_CSV_OK) {
      writer->last_error = status;
      return status;
    }
    writer->pending_newline = false;
  }

  writer->state = CSV_WRITER_STATE_IN_RECORD;
  writer->has_fields_in_record = false;
  writer->pending_empty_fields = 0;
  writer->last_error = GTEXT_CSV_OK;

  return GTEXT_CSV_OK;
}

GTEXT_API GTEXT_CSV_Status gtext_csv_writer_field(
    GTEXT_CSV_Writer * writer, const void * bytes, size_t len) {
  if (!writer) {
    return GTEXT_CSV_E_INVALID;
  }

  // Check if not in a record
  if (writer->state != CSV_WRITER_STATE_IN_RECORD) {
    writer->last_error = GTEXT_CSV_E_INVALID;
    return GTEXT_CSV_E_INVALID;
  }

  /* An empty field might be a trailing one, and that is not knowable yet, so
     it is counted rather than written.  See pending_empty_fields. */
  if (writer->opts.trim_trailing_empty_fields && len == 0) {
    writer->pending_empty_fields++;
    return GTEXT_CSV_OK;
  }

  /* A field has arrived, so any empty ones before it were not trailing. */
  GTEXT_CSV_Status status = csv_writer_flush_pending_empties(writer);
  if (status != GTEXT_CSV_OK) {
    writer->last_error = status;
    return status;
  }

  status = csv_writer_emit_field(writer, (const char *)bytes, len);
  if (status != GTEXT_CSV_OK) {
    writer->last_error = status;
    return status;
  }
  return GTEXT_CSV_OK;
}

GTEXT_API GTEXT_CSV_Status gtext_csv_writer_record_end(
    GTEXT_CSV_Writer * writer) {
  if (!writer) {
    return GTEXT_CSV_E_INVALID;
  }

  // Check if not in a record
  if (writer->state != CSV_WRITER_STATE_IN_RECORD) {
    writer->last_error = GTEXT_CSV_E_INVALID;
    return GTEXT_CSV_E_INVALID;
  }

  /* The empty fields held back were trailing after all, which is what
     trim_trailing_empty_fields asks to drop.  A record of nothing but empty
     fields therefore writes no field at all, which is what the table writer
     does for the same row. */
  writer->pending_empty_fields = 0;

  /* The newline is deferred rather than written: whether this record is the
     last - and so whether trailing_newline governs its terminator - is only
     known at finish().  record_begin() emits it if another record follows. */
  writer->pending_newline = true;

  writer->state = CSV_WRITER_STATE_INITIAL;
  writer->has_fields_in_record = false;
  return GTEXT_CSV_OK;
}

GTEXT_API GTEXT_CSV_Status gtext_csv_writer_finish(GTEXT_CSV_Writer * writer) {
  if (!writer) {
    return GTEXT_CSV_OK;
  }

  // If a record is open, close it first
  if (writer->state == CSV_WRITER_STATE_IN_RECORD) {
    GTEXT_CSV_Status status = gtext_csv_writer_record_end(writer);
    if (status != GTEXT_CSV_OK) {
      return status;
    }
  }

  /* The last record's terminator, which only this function can decide: every
     record but the last is followed by a separator, and trailing_newline says
     whether the last one is too.  This used to be skipped entirely - "handled
     by the caller if needed" - so a streaming write always ended in a newline
     and ignored the option the table writer honours. */
  if (writer->pending_newline) {
    /* Not after a failure.  A record whose field was refused - an unquotable
       field under GTEXT_CSV_QUOTE_NONE, say - was never written, and the table
       writer returns the error having emitted nothing.  Writing the terminator
       here would leave a caller who ignores the status with a file holding one
       stray newline and no data. */
    if (writer->opts.trailing_newline
        && writer->last_error == GTEXT_CSV_OK) {
      GTEXT_CSV_Status status = csv_writer_write_newline(writer);
      if (status != GTEXT_CSV_OK) {
        writer->last_error = status;
        return status;
      }
    }
    writer->pending_newline = false;
  }

  writer->state = CSV_WRITER_STATE_FINISHED;
  return GTEXT_CSV_OK;
}

GTEXT_API void gtext_csv_writer_free(GTEXT_CSV_Writer * writer) {
  if (writer) {
    // Sink is not owned by writer, so we don't free it
    gtext_allocator_free(writer->opts.allocator, writer);
  }
}

// ============================================================================
// Table Serialization Implementation
// ============================================================================

static size_t csv_find_last_non_empty_field(const csv_table_row * row) {
  if (!row || !row->fields || row->field_count == 0) {
    return SIZE_MAX;
  }

  // Iterate backwards through fields
  for (size_t i = row->field_count; i > 0; i--) {
    size_t idx = i - 1; // Convert to 0-based index
    const csv_table_field * field = &row->fields[idx];

    // Field is non-empty if length > 0
    // Note: field->data may be NULL for empty fields, but we check length
    // which is the authoritative indicator
    if (field->length > 0) {
      return idx;
    }
  }

  // All fields are empty
  return SIZE_MAX;
}

GTEXT_API GTEXT_CSV_Status gtext_csv_write_table(const GTEXT_CSV_Sink * sink,
    const GTEXT_CSV_Write_Options * opts, const GTEXT_CSV_Table * table) {
  if (!sink || !sink->write || !table) {
    return GTEXT_CSV_E_INVALID;
  }

  // Get default options if not provided
  GTEXT_CSV_Write_Options default_opts = gtext_csv_write_options_default();
  if (!opts) {
    opts = &default_opts;
  }

  // Cast to internal structure to access fields
  // The structure definition is in csv_internal.h and matches csv_table.c
  const struct GTEXT_CSV_Table * table_internal =
      (const struct GTEXT_CSV_Table *)table;

  // Defensive check: verify rows array is allocated
  if (!table_internal->rows) {
    return GTEXT_CSV_E_INVALID;
  }

  // Handle empty table.  A trailing newline terminates the last record, and a
  // table with no records has none to terminate, so nothing is written either
  // way.  This used to emit a lone newline when trailing_newline was set,
  // which was the option's only observable effect anywhere.  Both forms read
  // back as zero rows, so nothing is lost by dropping it, and "no records"
  // producing no bytes is the easier rule to state.
  if (table_internal->row_count == 0) {
    return GTEXT_CSV_OK;
  }

  // Defensive check: verify row_count doesn't exceed capacity (sanity check)
  if (table_internal->row_count > table_internal->row_capacity) {
    return GTEXT_CSV_E_INVALID;
  }

  /*
   * Refuse an unwritable table before the first byte reaches the sink.
   *
   * csv_write_field refuses a field that cannot survive unquoted, which is
   * correct but late: by the time the third field of the second record is
   * rejected, two records and two fields are already gone into a file the
   * caller now has to clean up. The whole table is in hand here, so the answer
   * is knowable in advance, and a caller who honours the status gets either a
   * complete document or an untouched sink.
   *
   * This is not available to the streaming writer, which is handed one field at
   * a time and has nothing to look ahead at. There the refusal stays per-field
   * and the sink keeps whatever was already written - stated on
   * gtext_csv_stream_write_field rather than left to be found.
   */
  for (size_t r = 0; r < table_internal->row_count; r++) {
    const csv_table_row * row = &table_internal->rows[r];
    if (!row->fields) {
      continue;
    }
    for (size_t f = 0; f < row->field_count; f++) {
      const csv_table_field * field = &row->fields[f];
      if (csv_field_needs_quoting(field->data, field->length, opts)) {
        continue;
      }
      if (!csv_field_survives_unquoted(field->data, field->length, opts)) {
        return GTEXT_CSV_E_UNQUOTABLE_FIELD;
      }
    }
  }

  // Determine start row (skip header if present)
  size_t start_row = 0;

  // Write header row if present
  if (table_internal->has_header) {
    // Bounds check: ensure row 0 is within allocated capacity
    if (table_internal->row_capacity == 0) {
      return GTEXT_CSV_E_INVALID;
    }

    const csv_table_row * header_row = &table_internal->rows[0];

    // Defensive check: verify fields array is allocated
    if (!header_row->fields && header_row->field_count > 0) {
      return GTEXT_CSV_E_INVALID;
    }

    // Determine how many fields to write (trim trailing empty if requested)
    size_t fields_to_write = header_row->field_count;
    if (opts->trim_trailing_empty_fields) {
      size_t last_non_empty = csv_find_last_non_empty_field(header_row);
      if (last_non_empty != SIZE_MAX) {
        fields_to_write = last_non_empty + 1;
      } else {
        fields_to_write = 0; // All fields empty
      }
    }

    // Write fields in header row
    for (size_t col = 0; col < fields_to_write; col++) {
      const csv_table_field * field = &header_row->fields[col];

      // Defensive check: if field has length > 0, data must not be NULL
      if (field->length > 0 && !field->data) {
        return GTEXT_CSV_E_INVALID;
      }

      // Insert delimiter before field if not first field
      if (col > 0) {
        char delimiter = opts->dialect.delimiter;
        GTEXT_CSV_Status status = sink->write(sink->user, &delimiter, 1);
        if (status != GTEXT_CSV_OK) {
          return status;
        }
      }

      // Write field with proper quoting and escaping
      GTEXT_CSV_Status status =
          csv_write_field(sink, field->data, field->length, opts);
      if (status != GTEXT_CSV_OK) {
        return status;
      }
    }

    // Write newline after header row
    const char * newline = opts->newline ? opts->newline : "\n";
    size_t newline_len = strlen(newline);
    GTEXT_CSV_Status status = sink->write(sink->user, newline, newline_len);
    if (status != GTEXT_CSV_OK) {
      return status;
    }

    // Data rows start at index 1
    start_row = 1;
  }

  // Write all data rows
  // Bounds check: ensure start_row is valid
  if (start_row > table_internal->row_count) {
    return GTEXT_CSV_E_INVALID;
  }

  for (size_t row = start_row; row < table_internal->row_count; row++) {
    // Bounds check: ensure row index is within allocated array
    if (row >= table_internal->row_capacity) {
      return GTEXT_CSV_E_INVALID;
    }

    const csv_table_row * table_row = &table_internal->rows[row];

    // Defensive check: verify fields array is allocated
    if (!table_row->fields && table_row->field_count > 0) {
      return GTEXT_CSV_E_INVALID;
    }

    // Determine how many fields to write (trim trailing empty if requested)
    size_t fields_to_write = table_row->field_count;
    if (opts->trim_trailing_empty_fields) {
      size_t last_non_empty = csv_find_last_non_empty_field(table_row);
      if (last_non_empty != SIZE_MAX) {
        fields_to_write = last_non_empty + 1;
      } else {
        fields_to_write = 0; // All fields empty
      }
    }

    // Write fields in row
    for (size_t col = 0; col < fields_to_write; col++) {
      const csv_table_field * field = &table_row->fields[col];

      // Defensive check: if field has length > 0, data must not be NULL
      if (field->length > 0 && !field->data) {
        return GTEXT_CSV_E_INVALID;
      }

      // Insert delimiter before field if not first field
      if (col > 0) {
        char delimiter = opts->dialect.delimiter;
        GTEXT_CSV_Status status = sink->write(sink->user, &delimiter, 1);
        if (status != GTEXT_CSV_OK) {
          return status;
        }
      }

      // Write field with proper quoting and escaping
      GTEXT_CSV_Status status =
          csv_write_field(sink, field->data, field->length, opts);
      if (status != GTEXT_CSV_OK) {
        return status;
      }
    }

    // A newline separates records, so every row but the last one gets one.
    // Whether the last one does is what trailing_newline decides.
    //
    // This comment used to read "except possibly last row if trailing_newline
    // is false" and then wrote the newline unconditionally, so the option
    // changed nothing for any non-empty table - its only observable effect was
    // to make an empty table emit a lone newline.  It now does what its name
    // and its documentation say.
    const bool is_last_row = (row + 1 == table_internal->row_count);
    if (!is_last_row || opts->trailing_newline) {
      const char * newline = opts->newline ? opts->newline : "\n";
      size_t newline_len = strlen(newline);
      GTEXT_CSV_Status status = sink->write(sink->user, newline, newline_len);
      if (status != GTEXT_CSV_OK) {
        return status;
      }
    }
  }

  return GTEXT_CSV_OK;
}
