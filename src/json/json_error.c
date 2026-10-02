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
 * Enhanced error reporting utilities for JSON module.
 */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/macros.h>
#include "json_internal.h"

#include <ghoti.io/text/json/json_core.h>
// Default context window sizes
#define JSON_ERROR_CONTEXT_BEFORE 20
#define JSON_ERROR_CONTEXT_AFTER 20

// Make constants available to parser
#define JSON_ERROR_CONTEXT_BEFORE_DEF 20
#define JSON_ERROR_CONTEXT_AFTER_DEF 20

GTEXT_API void gtext_json_error_free(GTEXT_JSON_Error * err) {
  if (err && err->context_snippet) {
    free(err->context_snippet); // allocator-exempt: error memory, see the option's docs
    err->context_snippet = NULL;
    err->context_snippet_len = 0;
    err->caret_offset = 0;
  }
}

GTEXT_JSON_Status json_error_generate_context_snippet(const char * input,
    size_t input_len, size_t error_offset, size_t context_before,
    size_t context_after, char ** snippet_out, size_t * snippet_len_out,
    size_t * caret_offset_out) {
  if (!input || !snippet_out || !snippet_len_out || !caret_offset_out) {
    return GTEXT_JSON_E_INVALID;
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
    return GTEXT_JSON_OK;
  }

  // Validate bounds before allocation and copy
  // snippet_start + snippet_len = snippet_end <= input_len, so bounds are safe
  if (snippet_start > input_len || snippet_end > input_len ||
      snippet_start > snippet_end) {
    // Should not happen with correct logic, but defensive check
    *snippet_out = NULL;
    *snippet_len_out = 0;
    *caret_offset_out = 0;
    return GTEXT_JSON_E_INVALID;
  }

  // Check for overflow in allocation size
  if (snippet_len > SIZE_MAX - 1) {
    *snippet_out = NULL;
    *snippet_len_out = 0;
    *caret_offset_out = 0;
    return GTEXT_JSON_E_OOM;
  }

  // Allocate snippet buffer
  char * snippet = (char *)malloc(snippet_len + 1); // allocator-exempt: error memory, see the option's docs
  if (!snippet) {
    return GTEXT_JSON_E_OOM;
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

  return GTEXT_JSON_OK;
}

const char * json_token_type_description(int token_type) {
  switch (token_type) {
  case JSON_TOKEN_EOF:
    return "end of input";
  case JSON_TOKEN_ERROR:
    return "error";
  case JSON_TOKEN_LBRACE:
    return "opening brace '{'";
  case JSON_TOKEN_RBRACE:
    return "closing brace '}'";
  case JSON_TOKEN_LBRACKET:
    return "opening bracket '['";
  case JSON_TOKEN_RBRACKET:
    return "closing bracket ']'";
  case JSON_TOKEN_COLON:
    return "colon ':'";
  case JSON_TOKEN_COMMA:
    return "comma ','";
  case JSON_TOKEN_NULL:
    return "null";
  case JSON_TOKEN_TRUE:
    return "true";
  case JSON_TOKEN_FALSE:
    return "false";
  case JSON_TOKEN_STRING:
    return "string";
  case JSON_TOKEN_NUMBER:
    return "number";
  case JSON_TOKEN_IDENT:
    return "unquoted name";
  case JSON_TOKEN_NAN:
    return "NaN";
  case JSON_TOKEN_INFINITY:
    return "Infinity";
  case JSON_TOKEN_NEG_INFINITY:
    return "-Infinity";
  default:
    return "unknown token";
  }
}
