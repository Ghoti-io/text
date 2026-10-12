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
 * The statuses, the default options, the result and the error list of the public HTML API.
 */

#include <ghoti.io/text/macros.h>

#include <stdint.h>
#include <string.h>

#include "html_api_internal.h"

/* The public error codes are the internal ones. A code added to either side must be added to the
   other, and these say so at compile time (the names are held equal by a test). */
_Static_assert((int)GTEXT_HTML_ERR_COUNT == 53, "codes 0 to 52 are the tokenizer's list (53 of them; number 41 is raised by the tree builder)");
_Static_assert((int)GTEXT_HTML_ERROR_INVALID_UTF_8_IN_INPUT_STREAM == (int)GTEXT_HTML_ERR_INVALID_UTF8,
    "the last tokenizer code");
_Static_assert(GTEXT_HTML_TREE_ERROR_BASE == 64, "the tree builder's codes begin at 64");
_Static_assert((int)GTEXT_HTML_TREE_ERR_COUNT == 16, "the tree builder has sixteen codes");
_Static_assert((int)GTEXT_HTML_ERROR_XMLNS_ATTRIBUTE_MISMATCH
        == GTEXT_HTML_TREE_ERROR_BASE + (int)GTEXT_HTML_TREE_ERR_XMLNS_MISMATCH,
    "the last tree builder code");
_Static_assert((int)GTEXT_HTML_ERROR_UNEXPECTED_NULL_CHARACTER_IN_CONTENT
        == GTEXT_HTML_TREE_ERROR_BASE + (int)GTEXT_HTML_TREE_ERR_UNEXPECTED_NULL_CHARACTER,
    "the tree builder's null character code");

const char * gtext_html_status_string(GTEXT_HTML_Status status) {
  switch (status) {
    case GTEXT_HTML_OK:
      return "ok";
    case GTEXT_HTML_E_INVALID:
      return "invalid argument";
    case GTEXT_HTML_E_OOM:
      return "out of memory";
    case GTEXT_HTML_E_STATE:
      return "wrong state for this call";
    case GTEXT_HTML_E_UNSUPPORTED_ENCODING:
      return "unsupported encoding";
    case GTEXT_HTML_E_INTERNAL:
      return "internal error";
    case GTEXT_HTML_E_LIMIT_INPUT:
      return "input too long";
    case GTEXT_HTML_E_LIMIT_DEPTH:
      return "too many elements open at once";
    case GTEXT_HTML_E_LIMIT_NODES:
      return "too many nodes";
    case GTEXT_HTML_E_LIMIT_NAME:
      return "name too long";
    case GTEXT_HTML_E_LIMIT_VALUE:
      return "attribute value too long";
    case GTEXT_HTML_E_LIMIT_COMMENT:
      return "comment too long";
    case GTEXT_HTML_E_LIMIT_TEXT:
      return "run of text too long";
    case GTEXT_HTML_E_LIMIT_ATTRIBUTES:
      return "too many attributes on a tag";
  }
  return "unknown status";
}

GTEXT_HTML_Parse_Options gtext_html_parse_options_default(void) {
  GTEXT_HTML_Parse_Options opts;
  memset(&opts, 0, sizeof(opts));
  opts.max_input_bytes = GTEXT_HTML_TOKENIZER_DEFAULT_MAX_INPUT;
  opts.max_open_elements = GTEXT_HTML_TREE_DEFAULT_MAX_OPEN_ELEMENTS;
  opts.max_nodes = GTEXT_HTML_TREE_DEFAULT_MAX_NODES;
  opts.max_name_bytes = GTEXT_HTML_TOKENIZER_DEFAULT_MAX_NAME;
  opts.max_attribute_bytes = GTEXT_HTML_TOKENIZER_DEFAULT_MAX_VALUE;
  opts.max_comment_bytes = GTEXT_HTML_TOKENIZER_DEFAULT_MAX_COMMENT;
  opts.max_text_bytes = GTEXT_HTML_TOKENIZER_DEFAULT_MAX_TEXT;
  opts.max_attributes = GTEXT_HTML_TOKENIZER_DEFAULT_MAX_ATTRIBUTES;
  opts.max_errors = GTEXT_HTML_TOKENIZER_DEFAULT_MAX_ERRORS;
  return opts;
}

/* ---- the error codes ---------------------------------------------------- */

const char * gtext_html_error_name(GTEXT_HTML_Error_Code code) {
  /* The tree builder's name for a U+0000 it does not want is the tokenizer's name for another
     U+0000; the public names are all different, so this one is spelled out. */
  if ((int)code == (int)GTEXT_HTML_ERROR_UNEXPECTED_NULL_CHARACTER_IN_CONTENT) {
    return "unexpected-null-character-in-content";
  }
  return gtext_html_tree_error_name((int)code);
}

/* ---- the result --------------------------------------------------------- */

/// Zero a result (what every parse function does first).
void html_result_clear(GTEXT_HTML_Result * result) {
  if (result) {
    memset(result, 0, sizeof(*result));
  }
}

/// Fill a result for a failure that has nothing to report but its status and message.
void html_result_set_failure(
    GTEXT_HTML_Result * result, GTEXT_HTML_Status status, const char * message) {
  if (result) {
    html_result_clear(result);
    result->status = status;
    result->message = message ? message : gtext_html_status_string(status);
  }
}

/// Copy the kept parse errors of a tree builder into a result; false if the allocator failed.
bool html_result_set_errors(GTEXT_HTML_Result * result, const GTEXT_Allocator * allocator,
    const GTEXT_HTML_Tree * tree) {
  if (!result) {
    return true;
  }
  const size_t count = gtext_html_tree_error_count(tree);
  const size_t dropped = gtext_html_tree_errors_dropped(tree);
  if (count > (SIZE_MAX - sizeof(struct GTEXT_HTML_Result_Internal)) / sizeof(GTEXT_HTML_Error)) {
    return false;
  }
  struct GTEXT_HTML_Result_Internal * internal = gtext_allocator_malloc(
      allocator, sizeof(*internal) + count * sizeof(GTEXT_HTML_Error));
  if (!internal) {
    return false;
  }
  internal->allocator = *allocator;
  internal->count = count;
  internal->dropped = dropped;
  for (size_t i = 0; i < count; i++) {
    const GTEXT_HTML_TreeError * e = gtext_html_tree_error_at(tree, i);
    GTEXT_HTML_Error * out = &internal->errors[i];
    out->code = (GTEXT_HTML_Error_Code)e->code;
    out->name = gtext_html_error_name(out->code);
    out->line = e->line;
    out->column = e->column;
    out->byte_offset = e->offset;
  }
  result->internal = internal;
  return true;
}

void gtext_html_result_free(GTEXT_HTML_Result * result) {
  if (!result) {
    return;
  }
  struct GTEXT_HTML_Result_Internal * internal = result->internal;
  if (internal) {
    const GTEXT_Allocator allocator = internal->allocator;
    gtext_allocator_free(&allocator, internal);
  }
  memset(result, 0, sizeof(*result));
}

size_t gtext_html_result_error_count(const GTEXT_HTML_Result * result) {
  return result && result->internal ? result->internal->count : 0;
}

size_t gtext_html_result_errors_dropped(const GTEXT_HTML_Result * result) {
  return result && result->internal ? result->internal->dropped : 0;
}

bool gtext_html_result_error_at(
    const GTEXT_HTML_Result * result, size_t index, GTEXT_HTML_Error * out) {
  if (!result || !out || !result->internal || index >= result->internal->count) {
    return false;
  }
  *out = result->internal->errors[index];
  return true;
}
