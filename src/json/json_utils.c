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
 * Shared utility functions for JSON module implementation.
 *
 * This file contains shared utility functions that are used across multiple
 * JSON implementation files. These functions help reduce code duplication
 * and ensure consistent behavior, especially for security-critical operations
 * like overflow checking, bounds checking, and position tracking.
 *
 * This follows the pattern established in csv_utils.c for the CSV module.
 */

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/macros.h>
#include "json_internal.h"

#include <ghoti.io/text/json/json_core.h>
size_t json_get_limit(size_t configured, size_t default_val) {
  return configured > 0 ? configured : default_val;
}

void json_position_update_offset(json_position * pos, size_t increment) {
  if (!pos) {
    return;
  }
  // Check for overflow in offset (size_t, so wraps around, but we check anyway)
  if (json_check_add_overflow(pos->offset, increment)) {
    pos->offset = SIZE_MAX; // Saturate at max
  }
  else {
    pos->offset += increment;
  }
}

void json_position_update_column(json_position * pos, size_t increment) {
  if (!pos) {
    return;
  }
  // Check for integer overflow in column
  if (json_check_int_overflow(pos->col, increment)) {
    pos->col = INT_MAX; // Saturate at max
  }
  else {
    pos->col += (int)increment;
  }
}

void json_position_increment_line(json_position * pos) {
  if (!pos) {
    return;
  }
  if (pos->line < INT_MAX) {
    pos->line++;
  }
  // If already at INT_MAX, don't increment (avoid overflow)
}

void json_position_advance(json_position * pos, const char * input,
    size_t input_len, size_t start_offset) {
  if (!pos || input_len == 0) {
    return;
  }

  // Update offset
  json_position_update_offset(pos, input_len);

  // Scan for newlines to update line/column
  if (input) {
    size_t end_offset = start_offset + input_len;
    for (size_t i = start_offset; i < end_offset; i++) {
      if (input[i] == '\n') {
        // Single-byte newline
        json_position_increment_line(pos);
        pos->col = 1;
      }
      else if (input[i] == '\r') {
        // Check for CRLF
        if (i + 1 < end_offset && input[i + 1] == '\n') {
          // CRLF - increment line, skip the LF in next iteration
          json_position_increment_line(pos);
          pos->col = 1;
          i++; // Skip the LF
        }
        else {
          // Standalone CR
          json_position_increment_line(pos);
          pos->col = 1;
        }
      }
      else {
        // Regular character - increment column
        json_position_update_column(pos, 1);
      }
    }
  }
  else {
    // No input buffer - just update column by input_len
    // (assumes no newlines in the bytes being advanced)
    json_position_update_column(pos, input_len);
  }
}

GTEXT_JSON_Status json_buffer_grow_unified(char ** buffer, size_t * capacity,
    size_t needed, json_buffer_growth_strategy strategy, size_t initial_size,
    size_t small_threshold, size_t growth_multiplier, size_t fixed_increment,
    size_t headroom, const GTEXT_Allocator * alloc) {
  if (!buffer || !capacity) {
    return GTEXT_JSON_E_INVALID;
  }

  // Use defaults if not specified
  if (initial_size == 0) {
    initial_size = 64; // Default initial size
  }
  if (small_threshold == 0) {
    small_threshold = 1024; // Default threshold (1KB)
  }
  if (growth_multiplier == 0) {
    growth_multiplier = 2; // Default multiplier (doubling)
  }
  if (fixed_increment == 0) {
    fixed_increment = 64; // Default fixed increment
  }

  // If already large enough, no need to grow
  if (needed <= *capacity) {
    return GTEXT_JSON_OK;
  }

  size_t new_capacity;

  if (*capacity == 0) {
    // Initial allocation - use minimum size or needed size, whichever is larger
    new_capacity = (needed < initial_size) ? initial_size : needed;
  }
  else if (strategy == JSON_BUFFER_GROWTH_HYBRID) {
    // Hybrid growth strategy
    if (*capacity < small_threshold) {
      // Small buffer: grow by fixed increment
      if (json_check_add_overflow(*capacity, fixed_increment)) {
        // Cannot add the increment without overflowing; ask for exactly what
        // is needed instead.  A `needed > SIZE_MAX` guard used to sit here,
        // which is always false for a size_t and so was two lines that no
        // test could ever execute.  The final overflow check below, and the
        // realloc itself, are what actually refuse an impossible size.
        new_capacity = needed;
      }
      else {
        new_capacity = *capacity + fixed_increment;
        if (new_capacity < needed) {
          new_capacity = needed;
        }
      }
    }
    else {
      // Large buffer: use exponential growth
      // Check for overflow before multiplication
      if (json_check_mul_overflow(*capacity, growth_multiplier)) {
        // Cannot multiply without overflowing; ask for exactly what is
        // needed.  See the note on the hybrid small-buffer branch above.
        new_capacity = needed;
      }
      else {
        new_capacity = *capacity * growth_multiplier;
        if (new_capacity < needed) {
          new_capacity = needed;
        }
      }
    }
  }
  else {
    // Simple strategy: double the size
    // Check for overflow before multiplication
    if (json_check_mul_overflow(*capacity, growth_multiplier)) {
      // Cannot double without overflow, use needed size directly
      new_capacity = needed;
    }
    else {
      new_capacity = *capacity * growth_multiplier;
      if (new_capacity < needed) {
        new_capacity = needed;
      }
    }
  }

  // Add headroom if specified (check for overflow before addition)
  if (headroom > 0) {
    if (json_check_add_overflow(new_capacity, headroom)) {
      // Cannot add headroom without overflow
      if (new_capacity < needed) {
        return GTEXT_JSON_E_OOM; // Cannot satisfy request
      }
      // Use new_capacity as-is (already >= needed)
    }
    else {
      new_capacity += headroom;
    }
  }

  // Final overflow check
  if (new_capacity < needed || new_capacity < *capacity) {
    return GTEXT_JSON_E_OOM;
  }

  // Reallocate buffer
  char * new_buffer = (char *)gtext_allocator_realloc(alloc, *buffer,
      new_capacity);
  if (!new_buffer) {
    return GTEXT_JSON_E_OOM;
  }

  *buffer = new_buffer;
  *capacity = new_capacity;
  return GTEXT_JSON_OK;
}

int json_check_add_overflow(size_t a, size_t b) {
  return a > SIZE_MAX - b;
}

int json_check_mul_overflow(size_t a, size_t b) {
  if (b == 0) {
    return 0; // No overflow if multiplying by 0
  }
  return a > SIZE_MAX / b;
}

int json_check_sub_underflow(size_t a, size_t b) {
  return a < b;
}

int json_check_int_overflow(int current, size_t increment) {
  if (increment > (size_t)INT_MAX) {
    return 1; // Increment itself is too large
  }
  return current > INT_MAX - (int)increment;
}

int json_check_null_param(const void * ptr, GTEXT_JSON_Error * err,
    GTEXT_JSON_Status error_code, const char * error_message) {
  if (ptr == NULL) {
    if (err) {
      *err = (GTEXT_JSON_Error){
          .code = error_code, .message = error_message, .line = 1, .col = 1};
    }
    return 1; // NULL detected
  }
  return 0; // Valid pointer
}

int json_check_bounds_index(size_t index, size_t size) {
  return index < size;
}

int json_check_bounds_offset(size_t offset, size_t size) {
  return offset < size;
}

int json_check_bounds_ptr(
    const void * ptr, const void * start, const void * end) {
  if (!ptr || !start || !end) {
    return 0; // Invalid pointers
  }
  // Use pointer comparison (requires same array/object)
  return ptr >= start && ptr < end;
}

void json_error_init_fields(GTEXT_JSON_Error * err, GTEXT_JSON_Status code,
    const char * message, size_t offset, int line, int col) {
  if (!err) {
    return;
  }
  *err = (GTEXT_JSON_Error){.code = code,
      .message = message,
      .offset = offset,
      .line = line,
      .col = col};
}

int json_check_string_length_overflow(size_t len) {
  return len > SIZE_MAX - 1;
}

/*
 * The text a keyword token was written with, or NULL for a token that is not
 * one of them.
 *
 * JSON5's unquoted object name is an ECMAScript IdentifierName, and that
 * includes the reserved words: `{true: 1}` is an object whose name is the four
 * letters. The lexer reads those words as keyword tokens, which is right
 * everywhere a value is expected and carries no text for the one place it is
 * not, so the name is recovered from the token type.
 *
 * **The type alone is not enough to recover a spelling, and this function used
 * to be asked to do it.** ::JSON_TOKEN_NEG_INFINITY is absent below because
 * `-Infinity` is not an identifier - a name cannot begin with a sign - and that
 * reasoning is right and was incomplete: `-Infinity` is the only signed
 * nonfinite the lexer gives a type of its own. `+Infinity` is handed
 * ::JSON_TOKEN_INFINITY and `+NaN` and `-NaN` are handed ::JSON_TOKEN_NAN, so
 * all three arrived here wearing the bare word's type and were answered with
 * the bare word's text. A fuzzer found `{+Infinity:5}`: accepted as an object
 * whose name is `Infinity`, a key the document never spelled and one byte
 * shorter than what it said.
 *
 * So callers in name position must use json_keyword_token_name() instead, which
 * asks the token whether its bytes are exactly the keyword. This function
 * remains the type-to-text table it always was, and is correct for what it does.
 */
GTEXT_INTERNAL_API const char * json_keyword_token_spelling(int token_type) {
  switch (token_type) {
  case JSON_TOKEN_NULL:
    return "null";
  case JSON_TOKEN_TRUE:
    return "true";
  case JSON_TOKEN_FALSE:
    return "false";
  case JSON_TOKEN_NAN:
    return "NaN";
  case JSON_TOKEN_INFINITY:
    return "Infinity";
  default:
    return NULL;
  }
}

/**
 * The name a keyword token spells, or NULL if it cannot be one.
 *
 * The rule an unquoted name has to satisfy is that its bytes are the identifier
 * - so a keyword token is a name only when the span it covers is exactly the
 * keyword. That is one comparison and it is the whole rule: a sign in front
 * makes the span longer than the word, whatever type the lexer chose, so this
 * refuses `+Infinity`, `+NaN` and `-NaN` for the same reason and without
 * needing to know which of them the lexer gave a distinct type to.
 *
 * Fixed here rather than by giving each signed spelling its own token type.
 * Eleven sites read ::JSON_TOKEN_INFINITY and twelve read ::JSON_TOKEN_NAN,
 * across six files, every one of them a place where the *value* is wanted and
 * where `+Infinity` and `Infinity` are the same number and should stay one
 * type. Name position is the only place the spelling matters, so the check
 * belongs in name position.
 */
GTEXT_INTERNAL_API const char * json_keyword_token_name(
    const json_token * token) {
  if (!token) return NULL;
  const char * spelling = json_keyword_token_spelling(token->type);
  if (!spelling) return NULL;
  return token->length == strlen(spelling) ? spelling : NULL;
}
