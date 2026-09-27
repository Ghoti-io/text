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
 * Streaming (incremental) JSON parser implementation.
 *
 * Implements an event-based streaming parser that accepts input in chunks
 * and emits events for each JSON value encountered.
 */

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/macros.h>
#include "json_internal.h"
#include "json_stream_internal.h"

#include <ghoti.io/text/json/json_core.h>
#include <ghoti.io/text/json/json_stream.h>
// Context window sizes for error snippets
#define JSON_ERROR_CONTEXT_BEFORE 20
#define JSON_ERROR_CONTEXT_AFTER 20

// Grow a buffer if needed (uses unified buffer growth function)
static GTEXT_JSON_Status json_stream_grow_buffer(
    char ** buffer, size_t * capacity, size_t needed) {
  return json_buffer_grow_unified(buffer, capacity, needed,
      JSON_BUFFER_GROWTH_SIMPLE, // Simple doubling strategy
      0,                         // Use default initial size
      0,                         // Not used for simple strategy
      0,                         // Use default multiplier (2)
      0,                         // Not used for simple strategy
      1024                       // Add 1024 bytes headroom
  );
}

// Grow stack if needed
static GTEXT_JSON_Status json_stream_grow_stack(GTEXT_JSON_Stream * st) {
  if (st->stack_size < st->stack_capacity) {
    return GTEXT_JSON_OK;
  }

  // Check for overflow before multiplication
  size_t new_capacity;
  if (st->stack_capacity == 0) {
    new_capacity = 16; // Initial capacity
  }
  else if (st->stack_capacity > SIZE_MAX / 2) {
    return GTEXT_JSON_E_OOM; // Cannot double without overflow
  }
  else {
    new_capacity = st->stack_capacity * 2;
    // Verify no overflow occurred
    if (new_capacity < st->stack_capacity) {
      return GTEXT_JSON_E_OOM; // Overflow
    }
  }

  // Check for overflow in size calculation
  size_t alloc_size = new_capacity * sizeof(json_stream_stack_entry);
  if (alloc_size / sizeof(json_stream_stack_entry) != new_capacity) {
    return GTEXT_JSON_E_OOM; // Multiplication overflowed
  }

  json_stream_stack_entry * new_stack =
      (json_stream_stack_entry *)realloc(st->stack, alloc_size);
  if (!new_stack) {
    return GTEXT_JSON_E_OOM;
  }

  st->stack = new_stack;
  st->stack_capacity = new_capacity;
  return GTEXT_JSON_OK;
}

// Push state onto stack
static GTEXT_JSON_Status json_stream_push(
    GTEXT_JSON_Stream * st, json_stream_state state, int is_array) {
  // Check depth limit before growing stack (more efficient)
  size_t max_depth = json_get_limit(st->opts.max_depth, JSON_DEFAULT_MAX_DEPTH);
  if (st->depth >= max_depth) {
    return GTEXT_JSON_E_DEPTH;
  }

  // Grow stack if needed
  GTEXT_JSON_Status status = json_stream_grow_stack(st);
  if (status != GTEXT_JSON_OK) {
    return status;
  }

  // No bounds check here: json_stream_grow_stack() returns GTEXT_JSON_OK only
  // with stack_size < stack_capacity.  It either returned early because that
  // was already true, or it grew - and stack_size <= stack_capacity always
  // holds, so the size that triggered the growth was exactly the capacity and
  // the new capacity (16 from zero, or double otherwise) is strictly larger.
  // The check that used to stand here could not fire, unlike the guard in
  // csv_stream_unescape_field() which rests on an invariant maintained in
  // another file and is kept for that reason.

  st->stack[st->stack_size].state = state;
  st->stack[st->stack_size].is_array = is_array;
  st->stack[st->stack_size].has_elements = 0;
  st->stack[st->stack_size].names = NULL;
  st->stack[st->stack_size].name_count = 0;
  st->stack[st->stack_size].name_cap = 0;
  st->stack_size++;
  st->depth++;

  return GTEXT_JSON_OK;
}

// Pop state from stack
static void json_stream_free_names(json_stream_stack_entry * entry) {
  if (!entry->names) {
    return;
  }
  for (size_t i = 0; i < entry->name_count; i++) {
    free(entry->names[i].bytes);
  }
  free(entry->names);
  entry->names = NULL;
  entry->name_count = 0;
  entry->name_cap = 0;
}

static void json_stream_pop(GTEXT_JSON_Stream * st) {
  if (st->stack_size > 0) {
    json_stream_free_names(&st->stack[st->stack_size - 1]);
    st->stack_size--;
    st->depth--;
  }
}

// Get current stack entry (or NULL if empty)
static json_stream_stack_entry * json_stream_top(GTEXT_JSON_Stream * st) {
  if (st->stack_size == 0) {
    return NULL;
  }
  return &st->stack[st->stack_size - 1];
}

/*
 * ERROR and FIRST_WINS have to know which names this object already has.
 * LAST_WINS and COLLECT do not: both deliver every member, because a value
 * the callback has already been handed cannot be replaced or wrapped after
 * the fact. Acceptance still agrees with the DOM parser for those two - a
 * repeated name is not an error there either.
 */
static int json_stream_tracks_names(const GTEXT_JSON_Stream * st) {
  return st->opts.dupkeys == GTEXT_JSON_DUPKEY_ERROR
      || st->opts.dupkeys == GTEXT_JSON_DUPKEY_FIRST_WINS;
}

static int json_stream_name_seen(
    const json_stream_stack_entry * entry, const char * bytes, size_t len) {
  for (size_t i = 0; i < entry->name_count; i++) {
    if (entry->names[i].len == len
        && (len == 0 || memcmp(entry->names[i].bytes, bytes, len) == 0)) {
      return 1;
    }
  }
  return 0;
}

static GTEXT_JSON_Status json_stream_name_add(json_stream_stack_entry * entry,
    const char * bytes, size_t len) {
  if (entry->name_count == entry->name_cap) {
    size_t cap = entry->name_cap == 0 ? 4 : entry->name_cap * 2;
    if (cap < entry->name_cap) {
      return GTEXT_JSON_E_OOM;
    }
    json_stream_name * grown =
        (json_stream_name *)realloc(entry->names, cap * sizeof(*grown));
    if (!grown) {
      return GTEXT_JSON_E_OOM;
    }
    entry->names = grown;
    entry->name_cap = cap;
  }
  char * copy = NULL;
  if (len > 0) {
    copy = (char *)malloc(len);
    if (!copy) {
      return GTEXT_JSON_E_OOM;
    }
    memcpy(copy, bytes, len);
  }
  entry->names[entry->name_count].bytes = copy;
  entry->names[entry->name_count].len = len;
  entry->name_count++;
  return GTEXT_JSON_OK;
}

/* The value of a repeated FIRST_WINS name has finished. */
static void json_stream_finish_suppressed(GTEXT_JSON_Stream * st) {
  if (st->suppressing && st->stack_size == st->suppress_at) {
    st->suppressing = 0;
  }
}

// Set error in stream and error structure (standardized error handling)
// Similar to csv_stream_set_error() in CSV module
static GTEXT_JSON_Status json_stream_set_error(GTEXT_JSON_Stream * st,
    GTEXT_JSON_Status code, const char * message, json_position pos,
    GTEXT_JSON_Error * err) {
  // Set stream state to error
  st->state = JSON_STREAM_STATE_ERROR;

  // Set error in provided error structure if available
  if (err) {
    // Free any existing context snippet
    // Note: err is always initialized (via memset) at public API entry points
    // (GTEXT_JSON_Stream_feed and GTEXT_JSON_Stream_finish), so context_snippet
    // will be NULL if properly initialized. However, if err is reused across
    // multiple calls without being freed by the caller, we need to free any
    // existing snippet. Since err is always initialized at entry points, we can
    // safely check and free context_snippet if it's non-NULL. This matches the
    // CSV pattern. IMPORTANT: The entry points
    // (GTEXT_JSON_Stream_feed/GTEXT_JSON_Stream_finish) free any existing
    // snippet before zeroing, so by the time we get here, context_snippet
    // should be NULL. But we check anyway for safety in case err is passed
    // directly.
    if (err->context_snippet != NULL) {
      free(err->context_snippet);
      err->context_snippet = NULL;
    }

    // Set error fields
    *err = (GTEXT_JSON_Error){.code = code,
        .message = message,
        .offset = pos.offset,
        .line = pos.line,
        .col = pos.col};

    // Generate context snippet if we have input buffer access
    const char * input_for_snippet = NULL;
    size_t input_len_for_snippet = 0;
    size_t error_offset = pos.offset;

    // Use input buffer if available
    if (st->input_buffer && st->input_buffer_used > 0) {
      input_for_snippet = st->input_buffer;
      input_len_for_snippet = st->input_buffer_used;
      // For buffered input, error offset is relative to buffer_start_offset
      // Adjust error_offset to be relative to input_buffer
      if (error_offset >= st->buffer_start_offset) {
        size_t buffer_relative_offset = error_offset - st->buffer_start_offset;
        // Clamp to buffer bounds
        if (buffer_relative_offset > input_len_for_snippet) {
          error_offset = input_len_for_snippet;
        }
        else {
          error_offset = buffer_relative_offset;
        }
      }
      else {
        // Error offset is before buffer start - use 0
        error_offset = 0;
      }
    }

    if (input_for_snippet && input_len_for_snippet > 0 &&
        error_offset <= input_len_for_snippet) {
      char * snippet = NULL;
      size_t snippet_len = 0;
      size_t caret_offset = 0;

      GTEXT_JSON_Status snippet_status =
          json_error_generate_context_snippet(input_for_snippet,
              input_len_for_snippet, error_offset, JSON_ERROR_CONTEXT_BEFORE,
              JSON_ERROR_CONTEXT_AFTER, &snippet, &snippet_len, &caret_offset);

      if (snippet_status == GTEXT_JSON_OK && snippet) {
        err->context_snippet = snippet;
        err->context_snippet_len = snippet_len;
        err->caret_offset = caret_offset;
      }
    }
  }

  return code;
}

// Forward declarations
static GTEXT_JSON_Status json_stream_handle_token(
    GTEXT_JSON_Stream * st, const json_token * token, GTEXT_JSON_Error * err);
static GTEXT_JSON_Status json_stream_handle_value_token(
    GTEXT_JSON_Stream * st, const json_token * token, GTEXT_JSON_Error * err);

// Emit an event through the callback
static GTEXT_JSON_Status json_stream_emit_event(GTEXT_JSON_Stream * st,
    GTEXT_JSON_Event_Type type, const GTEXT_JSON_Event * evt_data) {
  /* A repeated name under FIRST_WINS is parsed and not delivered. The
   * callback has not seen the member, which is what "the first one wins"
   * means for a stream that cannot retract an event. */
  if (st->suppressing) {
    return GTEXT_JSON_OK;
  }

  GTEXT_JSON_Event evt;
  memset(&evt, 0, sizeof(evt));
  evt.type = type;

  if (evt_data) {
    evt = *evt_data;
  }

  GTEXT_JSON_Error err;
  memset(&err, 0, sizeof(err));
  GTEXT_JSON_Status status = st->callback(st->user_data, &evt, &err);

  if (status != GTEXT_JSON_OK) {
    st->state = JSON_STREAM_STATE_ERROR;
    return status;
  }

  return GTEXT_JSON_OK;
}

// Compact the input buffer by shifting unprocessed data to the start
static void json_stream_compact_buffer(GTEXT_JSON_Stream * st) {
  if (st->input_buffer_processed == 0) {
    return; // Nothing to compact
  }

  if (st->input_buffer_processed >= st->input_buffer_used) {
    // All data processed, reset buffer
    st->input_buffer_used = 0;
    st->input_buffer_processed = 0;
    st->buffer_start_offset = st->total_bytes_consumed;
    return;
  }

  // Shift remaining data to start of buffer
  size_t remaining = st->input_buffer_used - st->input_buffer_processed;
  memmove(st->input_buffer, st->input_buffer + st->input_buffer_processed,
      remaining);

  // Update buffer start offset to reflect the new buffer position
  // Check for overflow (for tracking purposes - if overflow, clamp to SIZE_MAX)
  if (st->buffer_start_offset > SIZE_MAX - st->input_buffer_processed) {
    st->buffer_start_offset = SIZE_MAX;
  }
  else {
    st->buffer_start_offset += st->input_buffer_processed;
  }
  st->input_buffer_used = remaining;
  st->input_buffer_processed = 0;
}

// Process tokens from the buffered input
static GTEXT_JSON_Status json_stream_process_tokens(
    GTEXT_JSON_Stream * st, GTEXT_JSON_Error * err) {
  // Compact buffer if needed (shift unprocessed data to start)
  json_stream_compact_buffer(st);

  // If no unprocessed data, nothing to do
  // If we have an incomplete token but no input, we need to wait for more input
  // (finish() will handle completing incomplete tokens)
  if (st->input_buffer_used == 0) {
    return GTEXT_JSON_OK;
  }

  // Initialize or reinitialize lexer with current buffer
  // We reinitialize after compacting to ensure the input pointer is valid
  GTEXT_JSON_Status status = json_lexer_init(&st->lexer, st->input_buffer,
      st->input_buffer_used, &st->opts,
      1, // streaming mode
      /* The buffer is compacted, so it begins where the input does only while
       * nothing has been consumed - which is what decides whether a BOM here
       * is a leading one. */
      st->buffer_start_offset == 0);
  if (status != GTEXT_JSON_OK) {
    json_position pos = {
        .offset = st->buffer_start_offset, .line = 1, .col = 1};
    return json_stream_set_error(
        st, status, "Failed to initialize lexer", pos, err);
  }
  // Set token buffer pointer for resumption support
  st->lexer.token_buffer = &st->token_buffer;
  st->lexer_initialized = 1;

  // If resuming from incomplete token, adjust lexer position and fix state if
  // needed
  if (st->token_buffer.type != JSON_TOKEN_BUFFER_NONE) {
    // Token buffer is active - we need to resume parsing
    // The lexer should start from current_offset, which is already set
    // correctly by json_lexer_init. The parse functions will check
    // token_buffer.type and resume accordingly. When resuming a value token
    // (string or number), we should be in a state that expects a value, not in
    // VALUE state (which means we just processed a value and expect
    // comma/closing bracket). If we're in VALUE state, it means the state
    // changed between when we detected the incomplete token and now. We need to
    // fix the state to expect a value.
    /*
     * There used to be a state rewrite here: if the state was
     * JSON_STREAM_STATE_VALUE while a string or number was buffered, it was
     * reset to EXPECT_VALUE, on the reasoning that "the state changed between
     * when we detected the incomplete token and now".
     *
     * That reasoning is wrong in the case that matters. `[1 2]` fed a byte at a
     * time leaves the parser in VALUE after the space completes the first
     * number, and the buffered `2` is a genuine second value that the grammar
     * forbids - so rewriting the state to EXPECT_VALUE made the parser accept
     * it. In one feed the same input was refused with "Unexpected token after
     * value", which means the answer depended on where the caller's chunk
     * boundaries fell.
     *
     * Removing it changed no other test: the JSON binary's other 364 cases pass
     * unaltered, so the rewrite was repairing a state that was already right.
     */
  }

  // Process tokens until we can't continue (EOF or error)
  json_token token;
  while (1) {
    memset(&token, 0, sizeof(token));

    GTEXT_JSON_Status token_status = json_lexer_next(&st->lexer, &token);
    if (token_status != GTEXT_JSON_OK) {
      // Check for incomplete input (string or number spanning chunks, or
      // partial keyword)
      if (token_status == GTEXT_JSON_E_INCOMPLETE) {
        // Incomplete token - need more input
        // For strings/numbers: token buffer has the partial token data, input
        // can be marked as processed For keywords: no token buffer, partial
        // keyword stays in input buffer, don't mark as processed
        if (st->token_buffer.type != JSON_TOKEN_BUFFER_NONE) {
          // String or number - token buffer has the data
          // The lexer has advanced current_offset to the end of the incomplete
          // token Mark input as processed - the incomplete token data is in
          // token_buffer, not input_buffer If current_offset ==
          // input_buffer_used, all input is processed and buffer will be reset
          // This is correct - the next chunk will be appended to the (now
          // empty) buffer
          st->input_buffer_processed = st->lexer.current_offset;

          // Ensure we don't exceed input_buffer_used
          if (st->input_buffer_processed > st->input_buffer_used) {
            st->input_buffer_processed = st->input_buffer_used;
          }
        }
        else {
          // Keyword - partial keyword stays in input buffer, don't mark as
          // processed current_offset should not have advanced (lexer keeps
          // partial keyword in buffer) Don't update input_buffer_processed -
          // keep partial keyword in buffer
        }

        json_token_cleanup(&token);
        return GTEXT_JSON_OK;
      }

      // Error tokenizing - might be incomplete input or actual error
      // If it's a string/number parsing error, might need more input
      if (token.type == JSON_TOKEN_ERROR) {
        // Check if we're at EOF - if so, we might need more input
        if (st->lexer.current_offset >= st->lexer.input_len) {
          // At end of current buffer, wait for more input
          json_token_cleanup(&token);
          return GTEXT_JSON_OK;
        }
      }

      /* Real error. token_status, not status: `status` is the result of the
       * lexer *initialisation* above and is GTEXT_JSON_OK by now, so passing it
       * here set the stream's state to ERROR and then returned success. A
       * caller saw OK from the feed that failed, an error struct whose code
       * said OK beside a message saying "Tokenization error", and
       * GTEXT_JSON_E_STATE from the next feed - which is where the defect
       * usually got noticed, one call too late to say what was wrong. */
      json_position pos = {.offset = st->buffer_start_offset + token.pos.offset,
          .line = token.pos.line,
          .col = token.pos.col};
      json_token_cleanup(&token);
      return json_stream_set_error(
          st, token_status, "Tokenization error", pos, err);
    }

    // Update processed offset
    st->input_buffer_processed = st->lexer.current_offset;

    // Handle EOF
    if (token.type == JSON_TOKEN_EOF) {
      json_token_cleanup(&token);
      // At end of buffer, wait for more input (unless finish() was called)
      return GTEXT_JSON_OK;
    }

    // An error has already been reported; say nothing further about it.
    if (st->state == JSON_STREAM_STATE_ERROR) {
      json_token_cleanup(&token);
      return GTEXT_JSON_OK;
    }

    /*
     * A token after the document has ended is trailing garbage.
     *
     * This used to return GTEXT_JSON_OK and drop the token, so anything that
     * lexed cleanly after a complete document was silently ignored: `[1,2,3],`
     * and `{"a":1},` were accepted in a single feed, while `{"a":1}extra` was
     * refused - not by this check but because `extra` fails to lex. Which
     * trailing bytes were caught depended on whether they happened to tokenize.
     *
     * Delivered across two feeds the same input was already refused, because
     * json_stream_validate_state() rejects a feed in the DONE state. So the
     * answer also depended on where the caller's chunk boundaries fell.
     *
     * Whitespace does not reach here - the lexer skips it - and neither does
     * EOF, handled above.
     */
    if (st->state == JSON_STREAM_STATE_DONE) {
      json_position pos = {
          .offset = st->buffer_start_offset + token.pos.offset,
          .line = token.pos.line,
          .col = token.pos.col};
      json_token_cleanup(&token);
      return json_stream_set_error(st, GTEXT_JSON_E_TRAILING_GARBAGE,
          "Trailing content after the document", pos, err);
    }

    // Process token based on current state
    status = json_stream_handle_token(st, &token, err);
    json_token_cleanup(&token);

    if (status != GTEXT_JSON_OK) {
      // Error occurred - state may have been set to ERROR
      return status;
    }

    // If we're in error or done state, stop processing
    if (st->state == JSON_STREAM_STATE_ERROR ||
        st->state == JSON_STREAM_STATE_DONE) {
      return GTEXT_JSON_OK;
    }
  }
}

// Validate stream state before processing (defensive check)
static GTEXT_JSON_Status json_stream_validate_state(
    GTEXT_JSON_Stream * st, GTEXT_JSON_Error * err) {
  // Check for invalid states
  if (st->state == JSON_STREAM_STATE_ERROR) {
    // Already in error state - don't process further
    return GTEXT_JSON_E_STATE;
  }
  if (st->finished) {
    // finish() has been called; nothing more may be fed.
    return GTEXT_JSON_E_STATE;
  }
  // Validate state is one of the expected active states
  if (st->state != JSON_STREAM_STATE_INIT &&
      st->state != JSON_STREAM_STATE_VALUE &&
      st->state != JSON_STREAM_STATE_ARRAY &&
      st->state != JSON_STREAM_STATE_OBJECT_KEY &&
      st->state != JSON_STREAM_STATE_OBJECT_VALUE &&
      st->state != JSON_STREAM_STATE_EXPECT_VALUE &&
      st->state != JSON_STREAM_STATE_DONE) {
    // Invalid state - this should not happen
    json_position pos = {
        .offset = st->total_bytes_consumed, .line = 1, .col = 1};
    return json_stream_set_error(
        st, GTEXT_JSON_E_STATE, "Invalid parser state", pos, err);
  }
  return GTEXT_JSON_OK;
}

// Handle a token based on current parser state


/*
 * Close an array: the `]` half, in one place, for the same reason as
 * json_stream_close_object().
 *
 * The trailing-comma test lives here, and that is the fix as much as the
 * sharing is. There were two copies: the one reached from
 * JSON_STREAM_STATE_VALUE had the test, and the one reached from
 * JSON_STREAM_STATE_EXPECT_VALUE did not - so `[1,]` was accepted whatever
 * allow_trailing_commas said, while `{"a":1,}` was correctly refused. The
 * asymmetry was invisible because each copy read correctly on its own.
 *
 * EXPECT_VALUE is entered three ways: `[` for a first element, `,` inside an
 * array, and `:` inside an object. `has_elements` is what separates the first
 * from the second, which is why an empty array passes and a trailing comma does
 * not.
 */
static GTEXT_JSON_Status json_stream_close_array(GTEXT_JSON_Stream * st,
    const json_token * token, GTEXT_JSON_Error * err) {
  json_stream_stack_entry * array_top = json_stream_top(st);
  json_position pos = {.offset = st->buffer_start_offset + token->pos.offset,
      .line = token->pos.line,
      .col = token->pos.col};

  if (!array_top || !array_top->is_array) {
    return json_stream_set_error(
        st, GTEXT_JSON_E_BAD_TOKEN, "Unexpected ]", pos, err);
  }

  if (array_top->has_elements && st->state == JSON_STREAM_STATE_EXPECT_VALUE
      && !st->opts.allow_trailing_commas) {
    return json_stream_set_error(
        st, GTEXT_JSON_E_BAD_TOKEN, "Trailing comma not allowed", pos, err);
  }

  GTEXT_JSON_Event evt;
  evt.type = GTEXT_JSON_EVT_ARRAY_END;
  GTEXT_JSON_Status status =
      json_stream_emit_event(st, GTEXT_JSON_EVT_ARRAY_END, &evt);
  if (status != GTEXT_JSON_OK) {
    return status;
  }

  json_stream_pop(st);
  if (st->stack_size == 0) {
    st->state = JSON_STREAM_STATE_DONE;
  }
  else {
    json_stream_stack_entry * parent_top = json_stream_top(st);
    if (parent_top) {
      parent_top->has_elements = 1;
    }
    st->state = JSON_STREAM_STATE_VALUE;
  }
  json_stream_finish_suppressed(st);
  return GTEXT_JSON_OK;
}

/*
 * Close an object: the `}` half of the grammar, in one place.
 *
 * There were two copies of this and a third case that needed it and did not
 * have it. JSON_STREAM_STATE_OBJECT_KEY - the state a `{` leaves the parser in,
 * and the state a `,` inside an object returns it to - accepted a string and
 * nothing else, so every empty object was refused with "Expected object key
 * (string)": {}, { }, {"a":{}} and [{}] alike, at any chunk size, while the DOM
 * parser accepted all of them. An empty array was fine, which is why this was
 * not obvious.
 *
 * The trailing-comma test belongs here rather than at the call sites because it
 * is the reason the two cases differ at all: reaching `}` with elements already
 * emitted and a key expected means the last thing seen was a comma. In the
 * VALUE state that condition is false by construction, which is why the copy
 * there tested a state it could never be in.
 */
static GTEXT_JSON_Status json_stream_close_object(GTEXT_JSON_Stream * st,
    const json_token * token, GTEXT_JSON_Error * err) {
  json_stream_stack_entry * object_top = json_stream_top(st);
  json_position pos = {.offset = st->buffer_start_offset + token->pos.offset,
      .line = token->pos.line,
      .col = token->pos.col};

  if (!object_top || object_top->is_array) {
    return json_stream_set_error(
        st, GTEXT_JSON_E_BAD_TOKEN, "Unexpected }", pos, err);
  }

  if (object_top->has_elements && st->state == JSON_STREAM_STATE_OBJECT_KEY
      && !st->opts.allow_trailing_commas) {
    return json_stream_set_error(
        st, GTEXT_JSON_E_BAD_TOKEN, "Trailing comma not allowed", pos, err);
  }

  GTEXT_JSON_Event evt;
  evt.type = GTEXT_JSON_EVT_OBJECT_END;
  GTEXT_JSON_Status status =
      json_stream_emit_event(st, GTEXT_JSON_EVT_OBJECT_END, &evt);
  if (status != GTEXT_JSON_OK) {
    return status;
  }

  json_stream_pop(st);
  if (st->stack_size == 0) {
    st->state = JSON_STREAM_STATE_DONE;
  }
  else {
    // A nested object counts as an element of whatever holds it.
    json_stream_stack_entry * parent_top = json_stream_top(st);
    if (parent_top) {
      parent_top->has_elements = 1;
    }
    st->state = JSON_STREAM_STATE_VALUE;
  }
  json_stream_finish_suppressed(st);
  return GTEXT_JSON_OK;
}

static GTEXT_JSON_Status json_stream_handle_token(
    GTEXT_JSON_Stream * st, const json_token * token, GTEXT_JSON_Error * err) {
  // Validate state before processing token
  GTEXT_JSON_Status state_status = json_stream_validate_state(st, err);
  if (state_status != GTEXT_JSON_OK) {
    return state_status;
  }
  GTEXT_JSON_Status status;

  // Handle based on current state
  switch (st->state) {
  case JSON_STREAM_STATE_INIT:
    // Expecting first value
    return json_stream_handle_value_token(st, token, err);

  case JSON_STREAM_STATE_VALUE:
    // Just processed a value, expect comma or closing bracket/brace
    if (token->type == JSON_TOKEN_COMMA) {
      // Continue to next value
      json_stream_stack_entry * current_top = json_stream_top(st);
      if (!current_top) {
        // Comma at root level - invalid
        json_position pos = {
            .offset = st->buffer_start_offset + token->pos.offset,
            .line = token->pos.line,
            .col = token->pos.col};
        return json_stream_set_error(st, GTEXT_JSON_E_BAD_TOKEN,
            "Unexpected comma at root level", pos, err);
      }

      // Update state based on container type
      if (current_top->is_array) {
        st->state = JSON_STREAM_STATE_EXPECT_VALUE;
      }
      else {
        st->state = JSON_STREAM_STATE_OBJECT_KEY;
      }
      return GTEXT_JSON_OK;
    }
    else if (token->type == JSON_TOKEN_RBRACKET) {
      return json_stream_close_array(st, token, err);
    }
    else if (token->type == JSON_TOKEN_RBRACE) {
      return json_stream_close_object(st, token, err);
    }
    else {
      // Unexpected token
      json_position pos = {
          .offset = st->buffer_start_offset + token->pos.offset,
          .line = token->pos.line,
          .col = token->pos.col};
      return json_stream_set_error(
          st, GTEXT_JSON_E_BAD_TOKEN, "Unexpected token after value", pos, err);
    }

  case JSON_STREAM_STATE_ARRAY:
    // Expecting array element value
    return json_stream_handle_value_token(st, token, err);

  case JSON_STREAM_STATE_EXPECT_VALUE:
    // Expecting a value: after `[` for a first element, after `,` inside an
    // array, or after `:` inside an object.
    //
    // `]` may close an array here - empty when nothing has been emitted, a
    // trailing comma when something has - and json_stream_close_array() tells
    // those apart.
    if (token->type == JSON_TOKEN_RBRACKET) {
      return json_stream_close_array(st, token, err);
    }
    // `}` may not. This state is only ever reached inside an object by way of a
    // colon, so a `}` here is a member with no value: `{"a":}`. It used to be
    // handled as "end of object (empty object)", which is the one state where
    // that reading is wrong - and it was missing from the state where it is
    // right. So the streaming parser accepted {"a":} and refused {}, both
    // against the DOM parser. Falling through to the value handler gives the
    // error the token deserves.
    // Otherwise, handle as value token
    return json_stream_handle_value_token(st, token, err);

  case JSON_STREAM_STATE_OBJECT_KEY:
    // `}` here closes an object with no members - or, if members have already
    // been emitted, follows a comma and is a trailing one. Both answers are in
    // json_stream_close_object(); what this state used to do was refuse every
    // empty object.
    if (token->type == JSON_TOKEN_RBRACE) {
      return json_stream_close_object(st, token, err);
    }
    /* Expecting a name: a string always, an unquoted IdentifierName where
     * JSON5 was asked for, and then also one of the words the lexer reads as a
     * keyword everywhere else, because IdentifierName includes the reserved
     * words. A keyword token carries no text, so the spelling comes from the
     * type - the same rule the DOM parser applies, from the same function. */
    const char * keyword_name = NULL;
    if (token->type != JSON_TOKEN_STRING && token->type != JSON_TOKEN_IDENT &&
        st->opts.allow_unquoted_keys) {
      keyword_name = json_keyword_token_spelling(token->type);
    }
    if (token->type != JSON_TOKEN_STRING && token->type != JSON_TOKEN_IDENT &&
        !keyword_name) {
      json_position pos = {
          .offset = st->buffer_start_offset + token->pos.offset,
          .line = token->pos.line,
          .col = token->pos.col};
      return json_stream_set_error(
          st, GTEXT_JSON_E_BAD_TOKEN, "Expected object key (string)", pos, err);
    }

    const char * name =
        keyword_name ? keyword_name : token->data.string.value;
    size_t name_len =
        keyword_name ? strlen(keyword_name) : token->data.string.value_len;
    json_stream_stack_entry * object = json_stream_top(st);
    if (object && !object->is_array && json_stream_tracks_names(st)) {
      if (json_stream_name_seen(object, name, name_len)) {
        json_position pos = {
            .offset = st->buffer_start_offset + token->pos.offset,
            .line = token->pos.line,
            .col = token->pos.col};
        if (st->opts.dupkeys == GTEXT_JSON_DUPKEY_ERROR) {
          return json_stream_set_error(
              st, GTEXT_JSON_E_DUPKEY, "Duplicate key in object", pos, err);
        }
        /* FIRST_WINS: parse the member, do not deliver it. */
        st->skip_member = 1;
        st->state = JSON_STREAM_STATE_OBJECT_VALUE;
        return GTEXT_JSON_OK;
      }
      status = json_stream_name_add(object, name, name_len);
      if (status != GTEXT_JSON_OK) {
        json_position pos = {
            .offset = st->buffer_start_offset + token->pos.offset,
            .line = token->pos.line,
            .col = token->pos.col};
        return json_stream_set_error(
            st, status, "Out of memory", pos, err);
      }
    }

    // Emit key event
    {
      GTEXT_JSON_Event evt;
      evt.type = GTEXT_JSON_EVT_KEY;
      evt.as.str.s = name;
      evt.as.str.len = name_len;
      status = json_stream_emit_event(st, GTEXT_JSON_EVT_KEY, &evt);
      if (status != GTEXT_JSON_OK) {
        return status;
      }
    }

    st->state = JSON_STREAM_STATE_OBJECT_VALUE;
    return GTEXT_JSON_OK;

  case JSON_STREAM_STATE_OBJECT_VALUE: {
    // Expecting colon after key
    if (token->type != JSON_TOKEN_COLON) {
      json_position pos = {
          .offset = st->buffer_start_offset + token->pos.offset,
          .line = token->pos.line,
          .col = token->pos.col};
      return json_stream_set_error(st, GTEXT_JSON_E_BAD_TOKEN,
          "Expected colon after object key", pos, err);
    }

    // After colon, expect value. A repeated FIRST_WINS name is still parsed,
    // so a broken value and a repeated name inside it are still errors, and
    // none of its events are delivered.
    if (st->skip_member) {
      st->skip_member = 0;
      st->suppressing = 1;
      st->suppress_at = st->stack_size;
    }
    st->state = JSON_STREAM_STATE_EXPECT_VALUE;
    return GTEXT_JSON_OK;
  }

  default: {
    json_position pos = {.offset = st->buffer_start_offset + token->pos.offset,
        .line = token->pos.line,
        .col = token->pos.col};
    return json_stream_set_error(
        st, GTEXT_JSON_E_STATE, "Invalid parser state", pos, err);
  }
  }
}

// Handle a value token (null, bool, number, string, array begin, object begin)
static GTEXT_JSON_Status json_stream_handle_value_token(
    GTEXT_JSON_Stream * st, const json_token * token, GTEXT_JSON_Error * err) {
  GTEXT_JSON_Status status;
  GTEXT_JSON_Event evt;

  switch (token->type) {
  case JSON_TOKEN_NULL:
    evt.type = GTEXT_JSON_EVT_NULL;
    status = json_stream_emit_event(st, GTEXT_JSON_EVT_NULL, &evt);
    if (status != GTEXT_JSON_OK) {
      return status;
    }
    break;

  case JSON_TOKEN_TRUE:
  case JSON_TOKEN_FALSE:
    evt.type = GTEXT_JSON_EVT_BOOL;
    evt.as.boolean = (token->type == JSON_TOKEN_TRUE) ? 1 : 0;
    status = json_stream_emit_event(st, GTEXT_JSON_EVT_BOOL, &evt);
    if (status != GTEXT_JSON_OK) {
      return status;
    }
    break;

  case JSON_TOKEN_NUMBER:
    evt.type = GTEXT_JSON_EVT_NUMBER;
    evt.as.number.s = token->data.number.lexeme;
    evt.as.number.len = token->data.number.lexeme_len;
    status = json_stream_emit_event(st, GTEXT_JSON_EVT_NUMBER, &evt);
    if (status != GTEXT_JSON_OK) {
      return status;
    }
    break;

  case JSON_TOKEN_STRING:
    evt.type = GTEXT_JSON_EVT_STRING;
    evt.as.str.s = token->data.string.value;
    evt.as.str.len = token->data.string.value_len;
    status = json_stream_emit_event(st, GTEXT_JSON_EVT_STRING, &evt);
    if (status != GTEXT_JSON_OK) {
      return status;
    }
    break;

  case JSON_TOKEN_LBRACKET:
    // Array begin
    status = json_stream_push(st, st->state, 1);
    if (status != GTEXT_JSON_OK) {
      json_position pos = {
          .offset = st->buffer_start_offset + token->pos.offset,
          .line = token->pos.line,
          .col = token->pos.col};
      return json_stream_set_error(
          st, status, "Failed to push array onto stack", pos, err);
    }

    // Reset element count for new container
    st->container_elem_count = 0;

    evt.type = GTEXT_JSON_EVT_ARRAY_BEGIN;
    status = json_stream_emit_event(st, GTEXT_JSON_EVT_ARRAY_BEGIN, &evt);
    if (status != GTEXT_JSON_OK) {
      return status;
    }

    st->state = JSON_STREAM_STATE_EXPECT_VALUE;
    return GTEXT_JSON_OK;

  case JSON_TOKEN_LBRACE:
    // Object begin
    status = json_stream_push(st, st->state, 0);
    if (status != GTEXT_JSON_OK) {
      json_position pos = {
          .offset = st->buffer_start_offset + token->pos.offset,
          .line = token->pos.line,
          .col = token->pos.col};
      return json_stream_set_error(
          st, status, "Failed to push object onto stack", pos, err);
    }

    // Reset element count for new container
    st->container_elem_count = 0;

    evt.type = GTEXT_JSON_EVT_OBJECT_BEGIN;
    status = json_stream_emit_event(st, GTEXT_JSON_EVT_OBJECT_BEGIN, &evt);
    if (status != GTEXT_JSON_OK) {
      return status;
    }

    st->state = JSON_STREAM_STATE_OBJECT_KEY;
    return GTEXT_JSON_OK;

  case JSON_TOKEN_NAN:
  case JSON_TOKEN_INFINITY:
  case JSON_TOKEN_NEG_INFINITY:
    // Nonfinite numbers - emit as number event with lexeme
    evt.type = GTEXT_JSON_EVT_NUMBER;
    // For nonfinite numbers, we need to get the lexeme from the token
    // The lexer should have stored it in the number structure
    if (token->type == JSON_TOKEN_NAN) {
      evt.as.number.s = "NaN";
      evt.as.number.len = 3;
    }
    else if (token->type == JSON_TOKEN_INFINITY) {
      evt.as.number.s = "Infinity";
      evt.as.number.len = 8;
    }
    else {
      evt.as.number.s = "-Infinity";
      evt.as.number.len = 9;
    }
    status = json_stream_emit_event(st, GTEXT_JSON_EVT_NUMBER, &evt);
    if (status != GTEXT_JSON_OK) {
      return status;
    }
    break;

  default: {
    json_position pos = {.offset = st->buffer_start_offset + token->pos.offset,
        .line = token->pos.line,
        .col = token->pos.col};
    return json_stream_set_error(st, GTEXT_JSON_E_BAD_TOKEN,
        "Unexpected token, expected value", pos, err);
  }
  }

  // After emitting a scalar value, update state
  if (st->stack_size == 0) {
    // Root level value - we're done
    st->state = JSON_STREAM_STATE_DONE;
  }
  else {
    // Mark that container has elements
    json_stream_stack_entry * top = json_stream_top(st);
    if (top) {
      top->has_elements = 1;
    }

    /* A repeated name under FIRST_WINS is not a member, so it does not count
     * against the container limit and its events were not delivered. */
    int dropping = st->suppressing && st->stack_size == st->suppress_at;
    if (!dropping) {
      // Check container element limit
      st->container_elem_count++;
      size_t max_elems = json_get_limit(
          st->opts.max_container_elems, JSON_DEFAULT_MAX_CONTAINER_ELEMS);
      if (st->container_elem_count > max_elems) {
        json_position pos = {
            .offset = st->buffer_start_offset + token->pos.offset,
            .line = token->pos.line,
            .col = token->pos.col};
        return json_stream_set_error(st, GTEXT_JSON_E_LIMIT,
            "Maximum container element count exceeded", pos, err);
      }
    }

    // Inside container - expect comma or closing bracket/brace
    st->state = JSON_STREAM_STATE_VALUE;
    json_stream_finish_suppressed(st);
  }

  return GTEXT_JSON_OK;
}

GTEXT_API GTEXT_JSON_Stream * gtext_json_stream_new(
    const GTEXT_JSON_Parse_Options * opt, GTEXT_JSON_Event_cb cb, void * user) {
  // Input validation: callback is required
  if (!cb) {
    return NULL;
  }

  // normalize_unicode is implemented; what is refused is asking for it
  // without validate_utf8.  See json_parse_internal() for why.
  if (opt && opt->normalize_unicode && !opt->validate_utf8) {
    return NULL;
  }

  GTEXT_JSON_Stream * st =
      (GTEXT_JSON_Stream *)calloc(1, sizeof(GTEXT_JSON_Stream));
  if (!st) {
    return NULL;
  }

  // Copy parse options or use defaults
  if (opt) {
    st->opts = *opt;
    // Validate option values (defensive checks)
    // Note: Limits are validated when used via json_get_limit(), but we can
    // check for obviously invalid values here
    // Most limits are size_t, so 0 is valid (means use default)
    // We don't need to validate boolean flags or enum values as they're
    // type-safe
  }
  else {
    st->opts = gtext_json_parse_options_default();
  }

  st->callback = cb;
  st->user_data = user;
  st->state = JSON_STREAM_STATE_INIT;
  st->depth = 0;
  st->lexer_initialized = 0;
  st->buffer_start_offset = 0;

  // Initialize buffers with reasonable starting sizes
  st->input_buffer_size = 4096;
  st->input_buffer = (char *)malloc(st->input_buffer_size);
  if (!st->input_buffer) {
    free(st);
    return NULL;
  }

  // Initialize token buffer
  json_token_buffer_init(&st->token_buffer);
  // Note: buffer is allocated on-demand, not here

  // The stack is left NULL with capacity 0, from the calloc above, and the
  // first json_stream_push() allocates it.  It used to be allocated here as
  // well, which made json_stream_grow_stack()'s "capacity is zero" arm
  // unreachable - a whole allocation path that no test could take because
  // nothing ever arrived at it, and one of the lines tools/coverage.sh
  // reports as never executed.  One allocation site is both simpler and the
  // one that gets exercised; a stream whose input never nests now does not
  // allocate a stack at all.
  return st;
}

GTEXT_API void gtext_json_stream_free(GTEXT_JSON_Stream * st) {
  if (!st) {
    return;
  }

  // Free all buffers
  for (size_t i = 0; i < st->stack_size; i++) {
    json_stream_free_names(&st->stack[i]);
  }
  free(st->input_buffer);
  // Free token buffer if it was allocated
  if (st->token_buffer.buffer) {
    free(st->token_buffer.buffer);
    st->token_buffer.buffer = NULL;
  }
  free(st->stack);

  free(st);
}

GTEXT_API GTEXT_JSON_Status gtext_json_stream_feed(GTEXT_JSON_Stream * st,
    const char * bytes, size_t len, GTEXT_JSON_Error * err) {
  // Initialize error structure at entry point to ensure it's always in a known
  // state This ensures that when json_stream_set_error() is called, err is
  // always initialized. Free any existing context_snippet before zeroing to
  // prevent leaks.
  if (err) {
    gtext_json_error_free(err);
    *err = (GTEXT_JSON_Error){0};
  }
  if (!st) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Stream must not be NULL",
          .line = 1,
          .col = 1};
    }
    return GTEXT_JSON_E_INVALID;
  }

  if (!bytes && len > 0) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Input bytes must not be NULL",
          .line = 1,
          .col = 1};
    }
    return GTEXT_JSON_E_INVALID;
  }

  /* The DONE state is not closed for business: JSON allows white space after a
   * document and this parser's DOM entry point accepts it, so a caller feeding
   * a file in fixed-size blocks must be able to hand over the newline that
   * ended it. What is refused is feeding after finish(), and content rather
   * than white space - the token loop answers that with
   * GTEXT_JSON_E_TRAILING_GARBAGE, whichever feed the content arrives in.
   *
   * Refusing the whole feed here made the answer depend on where the caller's
   * chunk boundaries fell: `"a"\n` was accepted in one feed and refused in
   * two. The JSON fuzzer's differential found that the moment it was asked
   * whether the two parsers agree. */
  if (st->state == JSON_STREAM_STATE_ERROR || st->finished) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_STATE,
          .message = "Stream is in invalid state for feeding",
          .line = 1,
          .col = 1};
    }
    return GTEXT_JSON_E_STATE;
  }

  // Input validation: empty input is valid (no-op)
  if (len == 0) {
    return GTEXT_JSON_OK;
  }

  // Input validation: check for reasonable input size (prevent obvious
  // overflow) This is a defensive check - the actual limit is enforced via
  // max_total_bytes
  if (len > SIZE_MAX / 2) {
    json_position pos = {
        .offset = st->total_bytes_consumed, .line = 1, .col = 1};
    return json_stream_set_error(
        st, GTEXT_JSON_E_INVALID, "Input chunk size is too large", pos, err);
  }

  // Append to input buffer
  // Check for integer overflow before addition
  if (st->input_buffer_used > SIZE_MAX - len) {
    json_position pos = {
        .offset = st->total_bytes_consumed, .line = 1, .col = 1};
    return json_stream_set_error(
        st, GTEXT_JSON_E_OOM, "Input buffer size would overflow", pos, err);
  }

  size_t needed = st->input_buffer_used + len;
  GTEXT_JSON_Status status = json_stream_grow_buffer(
      &st->input_buffer, &st->input_buffer_size, needed);
  if (status != GTEXT_JSON_OK) {
    json_position pos = {
        .offset = st->total_bytes_consumed, .line = 1, .col = 1};
    return json_stream_set_error(
        st, status, "Failed to grow input buffer", pos, err);
  }

  // Verify bounds before memcpy (defensive check) using shared helper
  if (json_check_add_overflow(st->input_buffer_used, len) ||
      st->input_buffer_used + len > st->input_buffer_size) {
    json_position pos = {
        .offset = st->total_bytes_consumed, .line = 1, .col = 1};
    return json_stream_set_error(
        st, GTEXT_JSON_E_OOM, "Buffer size mismatch", pos, err);
  }

  memcpy(st->input_buffer + st->input_buffer_used, bytes, len);
  st->input_buffer_used += len;

  // Update buffer start offset if this is the first chunk
  if (st->buffer_start_offset == 0 && st->input_buffer_used == len) {
    st->buffer_start_offset = 0; // First chunk starts at offset 0
  }

  // Check for integer overflow before updating total_bytes_consumed using
  // shared helper
  if (json_check_add_overflow(st->total_bytes_consumed, len)) {
    json_position pos = {
        .offset = st->total_bytes_consumed, .line = 1, .col = 1};
    return json_stream_set_error(
        st, GTEXT_JSON_E_OOM, "Total bytes consumed would overflow", pos, err);
  }
  st->total_bytes_consumed += len;

  // Check total bytes limit
  size_t max_total =
      json_get_limit(st->opts.max_total_bytes, JSON_DEFAULT_MAX_TOTAL_BYTES);
  if (st->total_bytes_consumed > max_total) {
    json_position pos = {
        .offset = st->total_bytes_consumed, .line = 1, .col = 1};
    return json_stream_set_error(
        st, GTEXT_JSON_E_LIMIT, "Maximum total input size exceeded", pos, err);
  }

  // Process buffered input and emit events through callback
  GTEXT_JSON_Status process_status = json_stream_process_tokens(st, err);
  if (process_status == GTEXT_JSON_E_INCOMPLETE) {
    // This should never happen - json_stream_process_tokens should return
    // GTEXT_JSON_OK for incomplete tokens Convert to GTEXT_JSON_OK since
    // incomplete tokens are expected in streaming mode
    return GTEXT_JSON_OK;
  }
  return process_status;
}

GTEXT_API GTEXT_JSON_Status gtext_json_stream_finish(
    GTEXT_JSON_Stream * st, GTEXT_JSON_Error * err) {
  // Initialize error structure at entry point to ensure it's always in a known
  // state This ensures that when json_stream_set_error() is called, err is
  // always initialized. Free any existing context_snippet before zeroing to
  // prevent leaks.
  if (err) {
    gtext_json_error_free(err);
    *err = (GTEXT_JSON_Error){0};
  }
  if (!st) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Stream must not be NULL",
          .line = 1,
          .col = 1};
    }
    return GTEXT_JSON_E_INVALID;
  }

  if (st->state == JSON_STREAM_STATE_ERROR) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_STATE,
          .message = "Stream is in error state",
          .line = 1,
          .col = 1};
    }
    return GTEXT_JSON_E_STATE;
  }

  // Complete parsing of any remaining buffered input
  // First, compact buffer to ensure unprocessed data is at the start
  json_stream_compact_buffer(st);

  GTEXT_JSON_Status status = json_stream_process_tokens(st, err);
  if (status != GTEXT_JSON_OK) {
    return status;
  }

  // Check if we have an incomplete token in the buffer that needs to be
  // force-completed This can happen even if there's no unprocessed input (the
  // input was already marked as processed)
  if (st->token_buffer.type != JSON_TOKEN_BUFFER_NONE) {
    // We have an incomplete token in the buffer - force complete it
    // by temporarily disabling streaming mode and trying again
    int old_streaming_mode = st->lexer.streaming_mode;
    st->lexer.streaming_mode = 0; // Force complete mode

    // Reinitialize lexer with any remaining unprocessed input
    // If there's unprocessed input, it's at the start after compaction
    size_t unprocessed_len = st->input_buffer_used - st->input_buffer_processed;

    // Defensive check: ensure unprocessed_len doesn't exceed buffer size
    if (unprocessed_len > st->input_buffer_size) {
      st->lexer.streaming_mode = old_streaming_mode; // Restore
      json_position pos = {
          .offset = st->total_bytes_consumed, .line = 1, .col = 1};
      return json_stream_set_error(
          st, GTEXT_JSON_E_INVALID, "Invalid buffer state", pos, err);
    }

    // Reinitialize lexer - if there's unprocessed input, use it; otherwise use
    // empty buffer
    status = json_lexer_init(&st->lexer, st->input_buffer, unprocessed_len,
        &st->opts,
        0, // force complete mode (not streaming)
        st->buffer_start_offset == 0);
    if (status != GTEXT_JSON_OK) {
      st->lexer.streaming_mode = old_streaming_mode; // Restore
      json_position pos = {
          .offset = st->total_bytes_consumed, .line = 1, .col = 1};
      return json_stream_set_error(
          st, status, "Failed to reinitialize lexer for completion", pos, err);
    }
    // Ensure streaming_mode is set correctly
    st->lexer.streaming_mode = 0;
    // Set token buffer pointer - we'll try to complete the incomplete token
    st->lexer.token_buffer = &st->token_buffer;
    st->lexer_initialized = 1;

    // Process all remaining tokens (including the completed incomplete token)
    // After compaction, input_buffer_processed is 0, so we start from the
    // beginning
    json_token token;
    while (1) {
      memset(&token, 0, sizeof(token));
      status = json_lexer_next(&st->lexer, &token);
      if (status != GTEXT_JSON_OK) {
        // If still incomplete or error, it's a real problem
        st->lexer.streaming_mode = old_streaming_mode; // Restore
        json_position pos;
        // Check for overflow in offset calculation (for error reporting)
        if (st->buffer_start_offset > SIZE_MAX - token.pos.offset) {
          pos.offset = SIZE_MAX;
        }
        else {
          pos.offset = st->buffer_start_offset + token.pos.offset;
        }
        pos.line = token.pos.line;
        pos.col = token.pos.col;
        json_token_cleanup(&token);
        return json_stream_set_error(
            st, status, "Incomplete or invalid JSON input", pos, err);
      }

      // Handle EOF
      if (token.type == JSON_TOKEN_EOF) {
        // Update processed offset before breaking
        // Safe: lexer.current_offset is always <= lexer.input_len (which is
        // unprocessed_len)
        if (st->lexer.current_offset > unprocessed_len) {
          // Defensive check
          st->lexer.streaming_mode = old_streaming_mode; // Restore
          json_position pos = {
              .offset = st->buffer_start_offset, .line = 1, .col = 1};
          json_token_cleanup(&token);
          return json_stream_set_error(
              st, GTEXT_JSON_E_INVALID, "Lexer offset out of bounds", pos, err);
        }
        st->input_buffer_processed = st->lexer.current_offset;
        json_token_cleanup(&token);
        break;
      }

      // Adjust token position to be relative to original buffer start
      // token.pos.offset is relative to the reinitialized buffer start (which
      // is st->input_buffer) After compaction, buffer_start_offset points to
      // where the unprocessed data originally started Check for overflow in
      // offset calculation (for error reporting - not critical but avoids UB)
      json_token adjusted_token = token;
      if (json_check_add_overflow(st->buffer_start_offset, token.pos.offset)) {
        // Overflow in offset calculation - clamp to SIZE_MAX for error
        // reporting
        adjusted_token.pos.offset = SIZE_MAX;
      }
      else {
        adjusted_token.pos.offset = st->buffer_start_offset + token.pos.offset;
      }

      // Process token
      status = json_stream_handle_token(st, &adjusted_token, err);
      json_token_cleanup(&token);

      if (status != GTEXT_JSON_OK) {
        st->lexer.streaming_mode = old_streaming_mode; // Restore
        return status;
      }

      // Update processed offset after successful token processing
      // After compaction, we start from 0, so current_offset is the amount
      // processed Safe: lexer.current_offset is always <= lexer.input_len
      // (which is unprocessed_len) and unprocessed_len <=
      // st->input_buffer_used, so this assignment is safe
      if (st->lexer.current_offset > unprocessed_len) {
        // Defensive check: current_offset should not exceed what we gave the
        // lexer
        st->lexer.streaming_mode = old_streaming_mode; // Restore
        json_position pos = {
            .offset = st->buffer_start_offset, .line = 1, .col = 1};
        return json_stream_set_error(
            st, GTEXT_JSON_E_INVALID, "Lexer offset out of bounds", pos, err);
      }
      st->input_buffer_processed = st->lexer.current_offset;

      // If we're in error or done state, stop processing
      if (st->state == JSON_STREAM_STATE_ERROR ||
          st->state == JSON_STREAM_STATE_DONE) {
        break;
      }
    }

    st->lexer.streaming_mode = old_streaming_mode; // Restore
  }

  // Check if we have unprocessed input that needs to be force-completed
  // This can happen even if state is INIT (when an incomplete token was
  // encountered)
  if (st->input_buffer_used > st->input_buffer_processed) {
    // Have unprocessed input - might be incomplete token
    // Since finish() was called, we have all input, so force completion
    // by temporarily disabling streaming mode and trying again
    int old_streaming_mode = st->lexer.streaming_mode;
    st->lexer.streaming_mode = 0; // Force complete mode

    // Reinitialize lexer to start from unprocessed position
    // First, ensure we have unprocessed data to work with
    // Safe subtraction: we already checked st->input_buffer_used >
    // st->input_buffer_processed
    size_t unprocessed_len = st->input_buffer_used - st->input_buffer_processed;

    // Defensive check: ensure unprocessed_len doesn't exceed buffer size
    if (unprocessed_len > st->input_buffer_size) {
      st->lexer.streaming_mode = old_streaming_mode; // Restore
      json_position pos = {
          .offset = st->total_bytes_consumed, .line = 1, .col = 1};
      return json_stream_set_error(
          st, GTEXT_JSON_E_INVALID, "Invalid buffer state", pos, err);
    }

    if (unprocessed_len == 0) {
      // No unprocessed data, nothing to do
      st->lexer.streaming_mode = old_streaming_mode; // Restore
      // Continue to next check
    }
    else {

      // After compaction, unprocessed data is at the start of the buffer
      // and input_buffer_processed should be 0
      // So we use st->input_buffer directly (not st->input_buffer +
      // st->input_buffer_processed) Safe: unprocessed_len <=
      // st->input_buffer_size, so st->input_buffer + unprocessed_len is valid
      status = json_lexer_init(&st->lexer, st->input_buffer, unprocessed_len,
          &st->opts,
          0, // force complete mode (not streaming)
          st->buffer_start_offset == 0);
      if (status != GTEXT_JSON_OK) {
        st->lexer.streaming_mode = old_streaming_mode; // Restore
        json_position pos = {
            .offset = st->total_bytes_consumed, .line = 1, .col = 1};
        return json_stream_set_error(st, status,
            "Failed to reinitialize lexer for completion", pos, err);
      }
      // Ensure streaming_mode is set correctly (json_lexer_init should have set
      // it, but be explicit)
      st->lexer.streaming_mode = 0;
      // Set token buffer pointer - if there's an incomplete token, we'll try to
      // complete it
      st->lexer.token_buffer = &st->token_buffer;
      st->lexer_initialized = 1;

      // Process the remaining input with force-complete mode
      // After compaction, input_buffer_processed is 0, so we start from the
      // beginning
      json_token token;
      while (1) {
        memset(&token, 0, sizeof(token));
        status = json_lexer_next(&st->lexer, &token);
        if (status != GTEXT_JSON_OK) {
          // If still incomplete or error, it's a real problem
          st->lexer.streaming_mode = old_streaming_mode; // Restore
          json_position pos;
          // Check for overflow in offset calculation (for error reporting)
          if (st->buffer_start_offset > SIZE_MAX - token.pos.offset) {
            pos.offset = SIZE_MAX;
          }
          else {
            pos.offset = st->buffer_start_offset + token.pos.offset;
          }
          pos.line = token.pos.line;
          pos.col = token.pos.col;
          json_token_cleanup(&token);
          return json_stream_set_error(
              st, status, "Incomplete or invalid JSON input", pos, err);
        }

        // Handle EOF
        if (token.type == JSON_TOKEN_EOF) {
          // Update processed offset before breaking
          // Safe: lexer.current_offset is always <= lexer.input_len (which is
          // unprocessed_len)
          if (st->lexer.current_offset > unprocessed_len) {
            // Defensive check
            st->lexer.streaming_mode = old_streaming_mode; // Restore
            json_position pos = {
                .offset = st->buffer_start_offset, .line = 1, .col = 1};
            json_token_cleanup(&token);
            return json_stream_set_error(st, GTEXT_JSON_E_INVALID,
                "Lexer offset out of bounds", pos, err);
          }
          st->input_buffer_processed = st->lexer.current_offset;
          json_token_cleanup(&token);
          break;
        }

        // Adjust token position to be relative to original buffer start
        // token.pos.offset is relative to the reinitialized buffer start (which
        // is st->input_buffer) After compaction, buffer_start_offset points to
        // where the unprocessed data originally started Check for overflow in
        // offset calculation (for error reporting - not critical but avoids UB)
        json_token adjusted_token = token;
        if (st->buffer_start_offset > SIZE_MAX - token.pos.offset) {
          // Overflow in offset calculation - clamp to SIZE_MAX for error
          // reporting
          adjusted_token.pos.offset = SIZE_MAX;
        }
        else {
          adjusted_token.pos.offset =
              st->buffer_start_offset + token.pos.offset;
        }

        // Process token
        status = json_stream_handle_token(st, &adjusted_token, err);
        json_token_cleanup(&token);

        if (status != GTEXT_JSON_OK) {
          st->lexer.streaming_mode = old_streaming_mode; // Restore
          return status;
        }

        // Update processed offset after successful token processing
        // After compaction, we start from 0, so current_offset is the amount
        // processed Safe: lexer.current_offset is always <= lexer.input_len
        // (which is unprocessed_len) and unprocessed_len <=
        // st->input_buffer_used, so this assignment is safe
        if (st->lexer.current_offset > unprocessed_len) {
          // Defensive check: current_offset should not exceed what we gave the
          // lexer
          st->lexer.streaming_mode = old_streaming_mode; // Restore
          json_position pos = {
              .offset = st->buffer_start_offset, .line = 1, .col = 1};
          return json_stream_set_error(
              st, GTEXT_JSON_E_INVALID, "Lexer offset out of bounds", pos, err);
        }
        st->input_buffer_processed = st->lexer.current_offset;

        // If we're in error or done state, stop processing
        if (st->state == JSON_STREAM_STATE_ERROR ||
            st->state == JSON_STREAM_STATE_DONE) {
          break;
        }
      }
      st->lexer.streaming_mode = old_streaming_mode; // Restore
    }
  }

  // Validate that structure is complete (no unmatched brackets)
  // Check this AFTER processing all remaining tokens
  if (st->stack_size > 0) {
    json_position pos = {
        .offset = st->total_bytes_consumed, .line = 1, .col = 1};
    return json_stream_set_error(
        st, GTEXT_JSON_E_INCOMPLETE, "Incomplete JSON structure", pos, err);
  }

  /* Validate final state. INIT means no value was ever started - handling one
   * moves the state to VALUE, DONE, ARRAY or OBJECT_KEY - so a document has to
   * be refused here whether or not bytes arrived. This used to require an
   * empty buffer as well, on the reasoning that bytes having arrived meant a
   * value had, and the effect was that an input holding nothing but white
   * space, or nothing but a comment, was *accepted* with no events emitted.
   * gtext_json_parse() refuses all three; the JSON fuzzer's differential found
   * the pair. */
  if (st->state == JSON_STREAM_STATE_INIT) {
    json_position pos = {.offset = 0, .line = 1, .col = 1};
    return json_stream_set_error(
        st, GTEXT_JSON_E_INCOMPLETE, "No JSON value provided", pos, err);
  }

  st->state = JSON_STREAM_STATE_DONE;
  st->finished = 1;
  return GTEXT_JSON_OK;
}
