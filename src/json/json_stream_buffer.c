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
 * Buffer management for JSON streaming parser.
 *
 * Handles token buffer management, growth, and state tracking for incomplete
 * tokens that span chunk boundaries.
 */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/macros.h>
#include "json_stream_internal.h"
// Token buffer helper functions
void json_token_buffer_init(
    json_token_buffer * tb, const GTEXT_Allocator * alloc) {
  if (!tb) {
    return;
  }
  memset(tb, 0, sizeof(*tb));
  tb->type = JSON_TOKEN_BUFFER_NONE;
  tb->start_offset = SIZE_MAX;
  tb->consumed_length = 0;
  // Before any growth, and never reassigned: json_token_buffer_grow() and
  // gtext_json_stream_free() both read this one field, so they cannot name
  // different allocators for one buffer.
  tb->alloc = alloc;
}

void json_token_buffer_clear(json_token_buffer * tb) {
  if (!tb) {
    return;
  }
  // Don't free buffer here - reuse it for next token
  tb->buffer_used = 0;
  tb->type = JSON_TOKEN_BUFFER_NONE;
  tb->is_buffered = false;
  tb->start_offset = SIZE_MAX;
  tb->consumed_length = 0;
  // Clear parsing state
  memset(&tb->parse_state, 0, sizeof(tb->parse_state));
}

GTEXT_JSON_Status json_token_buffer_grow(
    json_token_buffer * tb, size_t needed) {
  if (!tb) {
    return GTEXT_JSON_E_INVALID;
  }

  return json_buffer_grow_unified(&tb->buffer, &tb->buffer_size, needed,
      JSON_BUFFER_GROWTH_HYBRID,      // Hybrid growth strategy
      JSON_TOKEN_BUFFER_INITIAL_SIZE, // Initial size
      JSON_BUFFER_SMALL_THRESHOLD,    // Small threshold
      JSON_BUFFER_GROWTH_MULTIPLIER,  // Growth multiplier
      64,                             // Fixed increment for small buffers
      0,                              // No headroom
      tb->alloc                       // Recorded by json_token_buffer_init()
  );
}

GTEXT_JSON_Status json_token_buffer_append(
    json_token_buffer * tb, const char * data, size_t len) {
  if (!tb || !data) {
    return GTEXT_JSON_E_INVALID;
  }

  // Check for overflow before addition
  if (len > SIZE_MAX - tb->buffer_used) {
    return GTEXT_JSON_E_OOM;
  }

  // Grow buffer if needed
  if (tb->buffer_used + len > tb->buffer_size) {
    GTEXT_JSON_Status status =
        json_token_buffer_grow(tb, tb->buffer_used + len);
    if (status != GTEXT_JSON_OK) {
      return status;
    }
  }

  // Append data
  memcpy(tb->buffer + tb->buffer_used, data, len);
  tb->buffer_used += len;
  tb->is_buffered = true;
  return GTEXT_JSON_OK;
}

void json_token_buffer_set_string_state(json_token_buffer * tb, int in_escape,
    int unicode_escape_remaining, int high_surrogate_seen) {
  if (!tb) {
    return;
  }
  tb->parse_state.string_state.in_escape = in_escape;
  tb->parse_state.string_state.unicode_escape_remaining =
      unicode_escape_remaining;
  tb->parse_state.string_state.high_surrogate_seen = high_surrogate_seen;
}

const char * json_token_buffer_data(const json_token_buffer * tb) {
  if (!tb || !tb->is_buffered) {
    return NULL;
  }
  return tb->buffer;
}

size_t json_token_buffer_length(const json_token_buffer * tb) {
  if (!tb) {
    return 0;
  }
  return tb->buffer_used;
}
