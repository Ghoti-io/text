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
 * JSON Pointer (RFC 6901) implementation.
 */

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/macros.h>
#include "json_internal.h"

#include <ghoti.io/text/json/json_core.h>
#include <ghoti.io/text/json/json_dom.h>
#include <ghoti.io/text/json/json_pointer.h>
// Decode a JSON Pointer reference token
// Decodes escape sequences: ~0 -> ~, ~1 -> /
// The output buffer must be large enough (worst case: same size as input)
static GTEXT_JSON_Status json_pointer_decode_token(const char * input,
    size_t input_len, char * output, size_t output_capacity,
    size_t * output_len) {
  size_t out_pos = 0;
  size_t in_pos = 0;

  while (in_pos < input_len) {
    if (out_pos >= output_capacity) {
      return GTEXT_JSON_E_INVALID;
    }

    if (input[in_pos] == '~') {
      // Check for escape sequence
      if (in_pos + 1 >= input_len) {
        // Incomplete escape sequence
        return GTEXT_JSON_E_INVALID;
      }

      char escape_char = input[in_pos + 1];
      if (escape_char == '0') {
        output[out_pos++] = '~';
        in_pos += 2;
      }
      else if (escape_char == '1') {
        output[out_pos++] = '/';
        in_pos += 2;
      }
      else {
        // Invalid escape sequence
        return GTEXT_JSON_E_INVALID;
      }
    }
    else {
      output[out_pos++] = input[in_pos++];
    }
  }

  *output_len = out_pos;
  return GTEXT_JSON_OK;
}

// Check if a string represents a valid array index
// Validates non-negative integer format, rejects leading zeros (except "0")
static int json_pointer_parse_index(
    const char * str, size_t len, size_t * out_idx) {
  if (len == 0) {
    return 0;
  }

  // Check for leading zero (only "0" is allowed)
  if (len > 1 && str[0] == '0') {
    return 0;
  }

  // Parse as unsigned integer
  size_t idx = 0;
  for (size_t i = 0; i < len; ++i) {
    if (!isdigit((unsigned char)str[i])) {
      return 0;
    }

    size_t digit = (size_t)(str[i] - '0');
    // Check for overflow (SIZE_MAX / 10)
    if (idx > SIZE_MAX / 10) {
      return 0;
    }
    idx *= 10;
    if (idx > SIZE_MAX - digit) {
      return 0;
    }
    idx += digit;
  }

  *out_idx = idx;
  return 1;
}

/* Internal function that performs the actual pointer evaluation.
   Handles both const and non-const versions.

   @p alloc is where the one transient allocation here comes from - the decoded
   token, which lives for one loop iteration and never escapes. It is a
   parameter rather than read from options because neither public entry point
   has any: see gtext_json_pointer_get_with_allocator(). json_patch.c needs it
   threaded this way too, since every pointer walk a patch performs happens
   inside a call the caller gave an allocator to. */
static GTEXT_JSON_Value * json_pointer_evaluate(GTEXT_JSON_Value * root,
    const char * ptr, size_t len, const GTEXT_Allocator * alloc) {
  if (!root || !ptr) {
    return NULL;
  }

  // RFC 6901 section 3:  json-pointer = *( "/" reference-token )
  //
  // The token count is therefore the count of '/' characters, and a token may
  // be empty - "/" is a pointer to the member named "", not a pointer to the
  // root.  This loop is shaped after that rule: consume a '/', then take
  // everything up to the next unescaped '/' as one token, empty or not.
  //
  // It used to start after the leading '/' and run `while (pos < len)`, so
  // "/" processed no tokens at all and returned the root, and the branch for
  // an empty token skipped it with the comment "refers to empty string key"
  // without ever performing that lookup.  Both are section 5's fourth
  // example, which evaluates "/" against a document with a "" member.
  GTEXT_JSON_Value * current = root;
  size_t pos = 0;

  while (pos < len) {
    if (ptr[pos] != '/') {
      // A non-empty pointer must begin with '/', and every token after the
      // first is reached by consuming one.
      return NULL;
    }
    pos++;

    // Find the end of this reference token.  '~1' is an escaped '/' and does
    // not separate tokens.
    size_t token_start = pos;
    size_t token_end = pos;

    while (token_end < len) {
      if (ptr[token_end] == '~') {
        if (token_end + 1 < len
            && (ptr[token_end + 1] == '0' || ptr[token_end + 1] == '1')) {
          if (token_end > SIZE_MAX - 2) {
            return NULL; // Overflow
          }
          token_end += 2;
          continue;
        }
        // Not a valid escape; json_pointer_decode_token rejects it below.
        if (token_end == SIZE_MAX) {
          return NULL; // Overflow
        }
        token_end++;
      }
      else if (ptr[token_end] == '/') {
        break; // Unescaped '/' separates tokens.
      }
      else {
        if (token_end == SIZE_MAX) {
          return NULL; // Overflow
        }
        token_end++;
      }
    }

    size_t token_len = token_end - token_start;

    // Verify bounds defensively.
    if (token_len > len || token_start > len - token_len) {
      return NULL;
    }

    // Decode the token.  An empty token decodes to the empty string and is
    // looked up like any other member name.
    if (token_len > SIZE_MAX - 1) {
      return NULL; // Token too large
    }
    char * decoded = (char *)gtext_allocator_malloc(alloc, token_len + 1);
    if (!decoded) {
      return NULL;
    }

    size_t decoded_len = 0;
    GTEXT_JSON_Status status = json_pointer_decode_token(
        ptr + token_start, token_len, decoded, token_len + 1, &decoded_len);

    if (status != GTEXT_JSON_OK) {
      gtext_allocator_free(alloc, decoded);
      return NULL;
    }

    decoded[decoded_len] = '\0';

    size_t array_idx = 0;
    int is_array_index =
        json_pointer_parse_index(decoded, decoded_len, &array_idx);

    if (is_array_index) {
      if (current->type != GTEXT_JSON_ARRAY) {
        gtext_allocator_free(alloc, decoded);
        return NULL;
      }
      if (array_idx >= current->as.array.count) {
        gtext_allocator_free(alloc, decoded);
        return NULL;
      }
      current = current->as.array.elems[array_idx];
    }
    else {
      if (current->type != GTEXT_JSON_OBJECT) {
        gtext_allocator_free(alloc, decoded);
        return NULL;
      }

      const GTEXT_JSON_Value * found = gtext_json_object_get(
          (const GTEXT_JSON_Value *)current, decoded, decoded_len);

      if (!found) {
        gtext_allocator_free(alloc, decoded);
        return NULL;
      }

      // Cast away const: the caller owns this tree, and the const entry point
      // re-applies const to the result.
      current = (GTEXT_JSON_Value *)found;
    }

    gtext_allocator_free(alloc, decoded);
    pos = token_end;
  }

  return current;
}

GTEXT_API const GTEXT_JSON_Value * gtext_json_pointer_get(
    const GTEXT_JSON_Value * root, const char * ptr, size_t len) {
  // Cast away const for internal evaluation
  // This is safe because we're only reading
  return json_pointer_evaluate((GTEXT_JSON_Value *)root, ptr, len, NULL);
}

GTEXT_API GTEXT_JSON_Value * gtext_json_pointer_get_mut(
    GTEXT_JSON_Value * root, const char * ptr, size_t len) {
  return json_pointer_evaluate(root, ptr, len, NULL);
}

GTEXT_API const GTEXT_JSON_Value * gtext_json_pointer_get_with_allocator(
    const GTEXT_JSON_Value * root, const char * ptr, size_t len,
    const GTEXT_Allocator * alloc) {
  return json_pointer_evaluate((GTEXT_JSON_Value *)root, ptr, len, alloc);
}

GTEXT_API GTEXT_JSON_Value * gtext_json_pointer_get_mut_with_allocator(
    GTEXT_JSON_Value * root, const char * ptr, size_t len,
    const GTEXT_Allocator * alloc) {
  return json_pointer_evaluate(root, ptr, len, alloc);
}
