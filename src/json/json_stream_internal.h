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
 * Internal definitions for JSON streaming parser.
 *
 * This header contains internal-only definitions used by the JSON streaming
 * parser implementation. It should not be included by external code.
 */

#ifndef GHOTI_IO_GTEXT_SRC_JSON_JSON_STREAM_INTERNAL_H
#define GHOTI_IO_GTEXT_SRC_JSON_JSON_STREAM_INTERNAL_H

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/macros.h>

#include "json_internal.h"
#include <ghoti.io/text/json/json_core.h>
#include <ghoti.io/text/json/json_stream.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Streaming parser state machine states
 */
typedef enum {
  JSON_STREAM_STATE_INIT,  ///< Initial state, waiting for first value
  JSON_STREAM_STATE_VALUE, ///< Just processed a value, waiting for comma or
                           ///< closing bracket/brace
  JSON_STREAM_STATE_ARRAY, ///< Inside an array, expecting value or ]
  JSON_STREAM_STATE_OBJECT_KEY,   ///< Inside object, expecting key
  JSON_STREAM_STATE_OBJECT_VALUE, ///< Just processed key, expecting colon
  JSON_STREAM_STATE_EXPECT_VALUE, ///< Expecting a value (after colon in object,
                                  ///< or in array)
  JSON_STREAM_STATE_DONE,         ///< Parsing complete
  JSON_STREAM_STATE_ERROR         ///< Error state
} json_stream_state;

/**
 * @brief Stack entry for tracking nesting
 */
/**
 * One object name the streaming parser has already accepted at this level.
 * Owned by the stack entry. Arrays do not use it.
 */
typedef struct {
  char * bytes;
  size_t len;
} json_stream_name;

typedef struct {
  json_stream_state state; ///< State when entering this level
  int is_array;            ///< 1 if array, 0 if object
  int has_elements;        ///< 1 if container has at least one element
  /**
   * Names already accepted in this object, for the duplicate-name policy.
   * NULL for an array, and for an object when the policy does not consult
   * them (last-wins and collect deliver every member; see json_stream.c).
   */
  json_stream_name * names;
  size_t name_count;
  size_t name_cap;
} json_stream_stack_entry;

/**
 * @brief Token buffer type enumeration
 */
typedef enum {
  JSON_TOKEN_BUFFER_NONE,   ///< No active token buffer
  JSON_TOKEN_BUFFER_STRING, ///< Buffering a string token
  JSON_TOKEN_BUFFER_NUMBER  ///< Buffering a number token
} json_token_buffer_type;

/**
 * @brief Token buffer structure
 *
 * Unified structure for managing incomplete tokens (strings, numbers) that
 * span chunk boundaries. Maintains parsing state to allow resumption when
 * more input arrives.
 */
typedef struct json_token_buffer {
  json_token_buffer_type type; ///< Type of token being buffered

  // Buffer management (similar to CSV field_buffer)
  char * buffer;      ///< Allocated buffer (NULL if not buffered)
  size_t buffer_size; ///< Allocated buffer size in bytes
  size_t buffer_used; ///< Used buffer size in bytes
  bool is_buffered;   ///< Whether data is in allocated buffer

  // JSON-specific parsing state
  union {
    struct {
      int in_escape;                ///< 1 if last char was '\'
      int unicode_escape_remaining; ///< Hex digits remaining for \uXXXX (0-4)
      int high_surrogate_seen; ///< 1 if high surrogate seen, waiting for low
    } string_state;
    struct {
      int has_dot;           ///< 1 if number contains '.'
      int has_exp;           ///< 1 if number contains 'e' or 'E'
      int exp_sign_seen;     ///< 1 if exponent sign (+/-) seen
      int starts_with_minus; ///< 1 if number starts with '-'
      int starts_with_plus;  ///< 1 if number starts with '+' (JSON5)
      int is_hex;            ///< 1 if number is a JSON5 hex literal
    } number_state;
  } parse_state;

  // Position tracking
  size_t start_offset;    ///< Offset where token started in input_buffer
  size_t consumed_length; ///< Length of data consumed from input_buffer (for
                          ///< incomplete tokens)

  /**
   * The allocator @c buffer belongs to, or NULL for the default one.
   *
   * Recorded here, and set by json_token_buffer_init() as a parameter, so that
   * whatever grows the buffer and whatever releases it cannot disagree.  The
   * same shape as csv_field_buffer::alloc, and for the reason written at
   * src/csv/csv_stream_buffer.c:38 - a buffer grown through the caller's
   * allocator and freed through the C library's is a mismatch the tracking
   * allocator catches as a block it never made, and the only reliable way to
   * prevent it is to stop the two sites being able to name different ones.
   */
  const GTEXT_Allocator * alloc;
} json_token_buffer;

/**
 * @brief Initial size for token buffer allocation
 */
#define JSON_TOKEN_BUFFER_INITIAL_SIZE 64

/**
 * @brief Minimum size for token buffer
 */
#define JSON_TOKEN_BUFFER_MIN_SIZE 1

/**
 * @brief Multiplier for buffer growth
 */
#define JSON_BUFFER_GROWTH_MULTIPLIER 2

/**
 * @brief Threshold for hybrid growth strategy (1KB)
 *
 * Buffers smaller than this use exponential growth, larger buffers
 * use linear growth to avoid excessive memory usage.
 */
#define JSON_BUFFER_SMALL_THRESHOLD 1024

// Token buffer functions (in json_stream_buffer.c)

/**
 * @brief Initialize a token buffer structure
 *
 * Initializes all fields to safe defaults. The buffer must be initialized
 * before use.
 *
 * @param tb Token buffer to initialize (must not be NULL)
 * @param alloc Allocator the buffer will come from, or NULL for the default
 *   one.  A parameter, not a field the caller assigns afterwards: see
 *   json_token_buffer::alloc.
 */
void json_token_buffer_init(
    json_token_buffer * tb, const GTEXT_Allocator * alloc);

/**
 * @brief Clear a token buffer structure
 *
 * Frees any allocated buffer and resets all fields to initial state.
 * The buffer can be reused after clearing.
 *
 * @param tb Token buffer to clear (must not be NULL)
 */
void json_token_buffer_clear(json_token_buffer * tb);

/**
 * @brief Append data to token buffer
 *
 * Appends data to the token buffer, growing it if necessary.
 * The buffer must be in buffered mode (is_buffered == true).
 *
 * @param tb Token buffer to append to (must not be NULL)
 * @param data Data to append (must not be NULL)
 * @param len Length of data to append
 * @return GTEXT_JSON_OK on success, error code on failure
 */
GTEXT_JSON_Status json_token_buffer_append(
    json_token_buffer * tb, const char * data, size_t len);

/**
 * @brief Grow token buffer to accommodate needed size
 *
 * Allocates or reallocates the token buffer to at least the needed size.
 * Uses hybrid growth strategy (exponential for small buffers, linear for
 * large).
 *
 * @param tb Token buffer to grow (must not be NULL)
 * @param needed Minimum size needed in bytes
 * @return GTEXT_JSON_OK on success, error code on failure
 */
GTEXT_JSON_Status json_token_buffer_grow(json_token_buffer * tb, size_t needed);

/**
 * @brief Set parsing state for string token
 *
 * Sets the string parsing state in the token buffer.
 *
 * @param tb Token buffer to set state on (must not be NULL)
 * @param in_escape 1 if last char was '\', 0 otherwise
 * @param unicode_escape_remaining Hex digits remaining for \uXXXX (0-4)
 * @param high_surrogate_seen 1 if high surrogate seen, waiting for low, 0
 * otherwise
 */
void json_token_buffer_set_string_state(json_token_buffer * tb, int in_escape,
    int unicode_escape_remaining, int high_surrogate_seen);

/**
 * @brief Get current buffer data pointer
 *
 * Returns a pointer to the current buffer data. If the buffer is not
 * yet allocated, returns NULL.
 *
 * @param tb Token buffer (must not be NULL)
 * @return Pointer to buffer data, or NULL if not allocated
 */
const char * json_token_buffer_data(const json_token_buffer * tb);

/**
 * @brief Get current buffer length
 *
 * Returns the current length of data in the buffer.
 *
 * @param tb Token buffer (must not be NULL)
 * @return Current buffer length in bytes
 */
size_t json_token_buffer_length(const json_token_buffer * tb);

/**
 * @brief Internal streaming parser structure
 */
struct GTEXT_JSON_Stream {
  // Configuration
  GTEXT_JSON_Parse_Options opts; ///< Parse options (copied)
  GTEXT_JSON_Event_cb callback;  ///< Event callback
  void * user_data;              ///< User context for callback

  // State machine
  json_stream_state state; ///< Current parser state
  size_t depth;            ///< Current nesting depth

  // Input buffering (for incremental parsing)
  char * input_buffer;           ///< Buffered input data
  size_t input_buffer_size;      ///< Allocated size of buffer
  size_t input_buffer_used;      ///< Used portion of buffer
  size_t input_buffer_processed; ///< Processed portion of buffer
  size_t buffer_start_offset; ///< Absolute offset where buffer starts in total
                              ///< input

  // Lexer state (will be initialized when we have enough input)
  json_lexer lexer;      ///< Lexer instance
  int lexer_initialized; ///< Whether lexer is initialized

  // Stack for tracking nesting
  json_stream_stack_entry * stack; ///< Stack of nested structures
  size_t stack_capacity;           ///< Allocated stack capacity
  size_t stack_size;               ///< Current stack depth

  // Token buffer for incomplete tokens (strings, numbers) spanning chunks
  json_token_buffer token_buffer; ///< Buffer for incomplete tokens

  // Limits tracking
  size_t total_bytes_consumed; ///< Total bytes processed
  size_t container_elem_count; ///< Current container element count

  /**
   * FIRST_WINS saw a repeated name and has not yet consumed its value.
   * The key event is not emitted. The colon that follows arms `suppressing`.
   */
  int skip_member;

  /**
   * Events for the value of a repeated name under FIRST_WINS are not
   * delivered. The grammar still runs, so a repeated name inside that value
   * is still an error when the policy says so. Cleared when the value
   * completes and `stack_size` is back at `suppress_at`.
   */
  int suppressing;
  size_t suppress_at;

  /* Whether gtext_json_stream_finish() has been called. The DONE state cannot
   * answer that: it is also where a complete top-level value leaves the
   * parser, and bytes may legitimately follow one - JSON allows white space
   * after a document, and so does this parser's DOM entry point. So feeding is
   * refused after finish and allowed after DONE, and the token loop refuses
   * anything that is not white space or a comment with
   * GTEXT_JSON_E_TRAILING_GARBAGE. */
  int finished;
};

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GTEXT_SRC_JSON_JSON_STREAM_INTERNAL_H
