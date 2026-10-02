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
 * Error handling utilities for CSV module.
 */

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/macros.h>
#include "csv_internal.h"

#include <ghoti.io/text/csv/csv_core.h>
GTEXT_API void gtext_csv_error_free(GTEXT_CSV_Error * err) {
  if (!err) {
    return;
  }
  if (err->context_snippet) {
    free(err->context_snippet); // allocator-exempt: error memory, see the option's docs
    err->context_snippet = NULL;
    err->context_snippet_len = 0;
    err->caret_offset = 0;
  }
  /* A formatted message is owned by the error.  This used to be handled by
   * pointing message at context_snippet and freeing only the snippet, which
   * left message dangling at a heap block this function had just released -
   * reachable from gtext_csv_row_append() with a mismatched field count,
   * because the struct documents message as a static string and a caller may
   * reasonably read it after freeing. */
  if (err->message_is_owned) {
    /* Cast away const: when the flag is set, the library allocated it. */
    free((void *)(uintptr_t)err->message); // allocator-exempt: error memory, see the option's docs
    err->message = NULL;
    err->message_is_owned = false;
  }
}

GTEXT_INTERNAL_API GTEXT_CSV_Status csv_error_generate_context_snippet(
    const char * input, size_t input_len, size_t error_offset,
    size_t context_before, size_t context_after, char ** snippet_out,
    size_t * snippet_len_out, size_t * caret_offset_out) {
  if (!input || !snippet_out || !snippet_len_out || !caret_offset_out) {
    return GTEXT_CSV_E_INVALID;
  }

  // Clamp error offset to input length
  if (error_offset > input_len) {
    error_offset = input_len;
  }

  // Calculate snippet boundaries with overflow protection
  size_t snippet_start = 0;
  if (error_offset > context_before) {
    // Safe subtraction: error_offset > context_before guarantees no underflow
    snippet_start = error_offset - context_before;
  }

  size_t snippet_end = input_len;
  // Check for overflow: if error_offset + context_after would overflow, use
  // input_len
  if (error_offset <= SIZE_MAX - context_after &&
      error_offset + context_after < input_len) {
    snippet_end = error_offset + context_after;
  }

  // Calculate snippet length (snippet_end >= snippet_start always)
  size_t snippet_len = snippet_end - snippet_start;
  if (snippet_len == 0) {
    // Empty input or error at end
    *snippet_out = NULL;
    *snippet_len_out = 0;
    *caret_offset_out = 0;
    return GTEXT_CSV_OK;
  }

  // Validate bounds before allocation and copy
  // snippet_start + snippet_len = snippet_end <= input_len, so bounds are safe
  if (snippet_start > input_len || snippet_end > input_len ||
      snippet_start > snippet_end) {
    // Should not happen with correct logic, but defensive check
    *snippet_out = NULL;
    *snippet_len_out = 0;
    *caret_offset_out = 0;
    return GTEXT_CSV_E_INVALID;
  }

  // Check for overflow in allocation size
  if (snippet_len > SIZE_MAX - 1) {
    *snippet_out = NULL;
    *snippet_len_out = 0;
    *caret_offset_out = 0;
    return GTEXT_CSV_E_OOM;
  }

  // Allocate snippet buffer
  char * snippet = (char *)malloc(snippet_len + 1); // allocator-exempt: error memory, see the option's docs
  if (!snippet) {
    return GTEXT_CSV_E_OOM;
  }

  // Copy snippet (bounds already validated above)
  memcpy(snippet, input + snippet_start, snippet_len);
  snippet[snippet_len] = '\0';

  // Calculate caret offset within snippet
  // error_offset is clamped to <= input_len, and snippet_start <= error_offset,
  // so this subtraction is safe and caret_offset <= snippet_len
  size_t caret_offset = error_offset - snippet_start;

  // Defensive check: caret_offset should be within snippet bounds
  if (caret_offset > snippet_len) {
    // Should not happen, but clamp to be safe
    caret_offset = snippet_len;
  }

  *snippet_out = snippet;
  *snippet_len_out = snippet_len;
  *caret_offset_out = caret_offset;

  return GTEXT_CSV_OK;
}

GTEXT_INTERNAL_API GTEXT_CSV_Status csv_error_copy(
    GTEXT_CSV_Error * dst, const GTEXT_CSV_Error * src) {
  if (!dst || !src) {
    return GTEXT_CSV_E_INVALID;
  }

  // Save source context snippet info before copying
  char * src_snippet = src->context_snippet;
  size_t src_snippet_len = src->context_snippet_len;
  size_t src_caret_offset = src->caret_offset;

  // Free existing context snippet in destination
  if (dst->context_snippet) {
    free(dst->context_snippet); // allocator-exempt: error memory, see the option's docs
    dst->context_snippet = NULL;
  }

  // Copy all fields (this will copy the context_snippet pointer, but we'll
  // replace it)
  *dst = *src;

  // Deep copy context snippet if present
  if (src_snippet && src_snippet_len > 0) {
    // Check for overflow in allocation size
    if (src_snippet_len > SIZE_MAX - 1) {
      dst->context_snippet = NULL;
      dst->context_snippet_len = 0;
      dst->caret_offset = 0;
      return GTEXT_CSV_E_OOM;
    }

    dst->context_snippet = (char *)malloc(src_snippet_len + 1); // allocator-exempt: error memory, see the option's docs
    if (!dst->context_snippet) {
      dst->context_snippet_len = 0;
      dst->caret_offset = 0;
      return GTEXT_CSV_E_OOM;
    }

    memcpy(dst->context_snippet, src_snippet, src_snippet_len);
    dst->context_snippet[src_snippet_len] = '\0';
    dst->context_snippet_len = src_snippet_len;
    dst->caret_offset = src_caret_offset;
  }
  else {
    // No snippet to copy
    dst->context_snippet = NULL;
    dst->context_snippet_len = 0;
    dst->caret_offset = 0;
  }

  return GTEXT_CSV_OK;
}
