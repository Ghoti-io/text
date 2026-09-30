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
 * Recursive descent parser for JSON.
 *
 * Implements a recursive descent parser that builds a DOM tree from JSON input.
 * Enforces strict JSON grammar, depth limits, and size limits.
 */

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/macros.h>
#include "json_internal.h"

#include <ghoti.io/text/json/json_core.h>
#include <ghoti.io/text/json/json_dom.h>
// Context window sizes for error snippets
#define JSON_ERROR_CONTEXT_BEFORE 20
#define JSON_ERROR_CONTEXT_AFTER 20


// Forward declaration
static GTEXT_JSON_Status json_parser_set_error_with_tokens(json_parser * parser,
    GTEXT_JSON_Status code, const char * message, json_position pos,
    const char * expected_token, const char * actual_token);

// Helper to set error and return status
static GTEXT_JSON_Status json_parser_set_error(json_parser * parser,
    GTEXT_JSON_Status code, const char * message, json_position pos) {
  return json_parser_set_error_with_tokens(
      parser, code, message, pos, NULL, NULL);
}

// Helper to set error with expected/actual token information
static GTEXT_JSON_Status json_parser_set_error_with_tokens(json_parser * parser,
    GTEXT_JSON_Status code, const char * message, json_position pos,
    const char * expected_token, const char * actual_token) {
  if (parser->error_out) {
    // The error snippet is C-library memory by design, because
    // gtext_json_error_free() receives only the error and cannot learn which
    // allocator produced it.  See GTEXT_JSON_Parse_Options::allocator.
    if (parser->error_out->context_snippet) {
      free(parser->error_out->context_snippet); // allocator-exempt
      parser->error_out->context_snippet = NULL;
    }

    parser->error_out->code = code;
    parser->error_out->message = message;
    parser->error_out->offset = pos.offset;
    parser->error_out->line = pos.line;
    parser->error_out->col = pos.col;
    parser->error_out->expected_token = expected_token;
    parser->error_out->actual_token = actual_token;
    parser->error_out->context_snippet = NULL;
    parser->error_out->context_snippet_len = 0;
    parser->error_out->caret_offset = 0;

    // Generate context snippet if we have input buffer access
    if (parser->lexer.input && parser->lexer.input_len > 0) {
      char * snippet = NULL;
      size_t snippet_len = 0;
      size_t caret_offset = 0;
      GTEXT_JSON_Status snippet_status =
          json_error_generate_context_snippet(parser->lexer.input,
              parser->lexer.input_len, pos.offset, JSON_ERROR_CONTEXT_BEFORE,
              JSON_ERROR_CONTEXT_AFTER, &snippet, &snippet_len, &caret_offset);
      if (snippet_status == GTEXT_JSON_OK && snippet) {
        parser->error_out->context_snippet = snippet;
        parser->error_out->context_snippet_len = snippet_len;
        parser->error_out->caret_offset = caret_offset;
      }
    }
  }
  return code;
}

// Check depth limit
static GTEXT_JSON_Status json_parser_check_depth(json_parser * parser) {
  size_t max_depth = json_get_limit(
      parser->opts ? parser->opts->max_depth : 0, JSON_DEFAULT_MAX_DEPTH);
  if (parser->depth >= max_depth) {
    return json_parser_set_error(parser, GTEXT_JSON_E_DEPTH,
        "Maximum nesting depth exceeded", parser->lexer.pos);
  }
  return GTEXT_JSON_OK;
}

// Check string size limit
static GTEXT_JSON_Status json_parser_check_string_size(
    json_parser * parser, size_t string_len) {
  size_t max_string =
      json_get_limit(parser->opts ? parser->opts->max_string_bytes : 0,
          JSON_DEFAULT_MAX_STRING_BYTES);
  if (string_len > max_string) {
    return json_parser_set_error(parser, GTEXT_JSON_E_LIMIT,
        "Maximum string size exceeded", parser->lexer.pos);
  }
  return GTEXT_JSON_OK;
}

// Check container element limit
static GTEXT_JSON_Status json_parser_check_container_elems(
    json_parser * parser, size_t current_count) {
  size_t max_elems =
      json_get_limit(parser->opts ? parser->opts->max_container_elems : 0,
          JSON_DEFAULT_MAX_CONTAINER_ELEMS);
  if (current_count >= max_elems) {
    return json_parser_set_error(parser, GTEXT_JSON_E_LIMIT,
        "Maximum container element count exceeded", parser->lexer.pos);
  }
  return GTEXT_JSON_OK;
}

// Forward declarations
static GTEXT_JSON_Status json_parse_value(
    json_parser * parser, GTEXT_JSON_Value ** out, json_context * ctx);
static GTEXT_JSON_Status json_parse_object(
    json_parser * parser, GTEXT_JSON_Value ** out, json_context * ctx);
static GTEXT_JSON_Status json_parse_array(
    json_parser * parser, GTEXT_JSON_Value ** out, json_context * ctx);

// Helper to parse an array element from a token (inline parsing to avoid double
// token consumption) The token is consumed/cleaned up by this function
static GTEXT_JSON_Status json_parse_array_element(json_parser * parser,
    json_token * token, json_context * ctx, GTEXT_JSON_Value ** out) {
  GTEXT_JSON_Value * element = NULL;
  GTEXT_JSON_Status result = GTEXT_JSON_OK;

  switch (token->type) {
  case JSON_TOKEN_NULL:
    element = json_value_new_with_existing_context(GTEXT_JSON_NULL, ctx);
    if (!element) {
      result = GTEXT_JSON_E_OOM;
    }
    json_token_cleanup(token);
    break;

  case JSON_TOKEN_TRUE:
    element = json_value_new_with_existing_context(GTEXT_JSON_BOOL, ctx);
    if (!element) {
      result = GTEXT_JSON_E_OOM;
    }
    else {
      element->as.boolean = 1;
    }
    json_token_cleanup(token);
    break;

  case JSON_TOKEN_FALSE:
    element = json_value_new_with_existing_context(GTEXT_JSON_BOOL, ctx);
    if (!element) {
      result = GTEXT_JSON_E_OOM;
    }
    else {
      element->as.boolean = 0;
    }
    json_token_cleanup(token);
    break;

  case JSON_TOKEN_STRING: {
    element = json_value_new_with_existing_context(GTEXT_JSON_STRING, ctx);
    if (!element) {
      result = GTEXT_JSON_E_OOM;
      json_token_cleanup(token);
      break;
    }

    // Check if we can use in-situ mode
    // Conditions: in-situ mode enabled, context has input buffer, no escape
    // sequences
    /*
     * in_situ_mode points the DOM at the caller's bytes, which is only sound
     * when the decoded string *is* those bytes. The length test below is the
     * proxy for that, and normalisation breaks it: canonical ordering reorders
     * combining marks without changing how many bytes they take, so a
     * normalised string can be the same length as the input and different from
     * it. In-situ would then hand back the un-normalised original and the
     * option would appear to do nothing. Normalising always copies, so the two
     * are mutually exclusive and normalisation wins.
     */
    int use_in_situ = 0;
    size_t original_start = 0;
    size_t original_len = 0;
    if (parser->opts && parser->opts->in_situ_mode &&
        !parser->opts->normalize_unicode && ctx->input_buffer &&
        ctx->input_buffer_len > 0 &&
        token->data.string.value_len == token->data.string.original_len) {
      // Verify the original string position is within bounds
      // Check for integer overflow and bounds
      original_start = token->data.string.original_start;
      original_len = token->data.string.original_len;
      if (original_start < ctx->input_buffer_len &&
          original_len <= ctx->input_buffer_len - original_start) {
        use_in_situ = 1;
      }
    }

    if (use_in_situ) {
      // Use in-situ mode: reference input buffer directly
      // Safe: original_start and original_len were validated above
      element->as.string.data = (char *)(ctx->input_buffer + original_start);
      element->as.string.len = original_len;
      element->as.string.is_in_situ = 1;
    }
    else {
      // Copy string data
      size_t str_len = token->data.string.value_len;
      if (str_len > SIZE_MAX - 1) {
        result = GTEXT_JSON_E_LIMIT;
        gtext_json_free(element);
        json_token_cleanup(token);
        break;
      }
      char * str_data =
          (char *)json_arena_alloc_for_context(ctx, str_len + 1, 1);
      if (!str_data) {
        result = GTEXT_JSON_E_OOM;
        gtext_json_free(element);
        json_token_cleanup(token);
        break;
      }
      memcpy(str_data, token->data.string.value, str_len);
      str_data[str_len] = '\0';

      element->as.string.data = str_data;
      element->as.string.len = str_len;
      element->as.string.is_in_situ = 0;
    }

    json_token_cleanup(token);
    break;
  }

  case JSON_TOKEN_NUMBER: {
    json_number * num = &token->data.number;
    element = json_value_new_with_existing_context(GTEXT_JSON_NUMBER, ctx);
    if (!element) {
      result = GTEXT_JSON_E_OOM;
      json_token_cleanup(token);
      break;
    }

    // Check if we can use in-situ mode for lexeme
    // Conditions: in-situ mode enabled, context has input buffer, lexeme exists
    int use_in_situ = 0;
    size_t number_offset = 0;
    if (parser->opts && parser->opts->in_situ_mode && ctx->input_buffer &&
        ctx->input_buffer_len > 0 && num->lexeme && num->lexeme_len > 0) {
      // Verify the number position is within bounds
      // Check for integer overflow and bounds using shared helpers
      number_offset = token->pos.offset;
      if (json_check_bounds_offset(number_offset, ctx->input_buffer_len) &&
          !json_check_add_overflow(number_offset, token->length) &&
          number_offset + token->length <= ctx->input_buffer_len) {
        use_in_situ = 1;
      }
    }

    if (use_in_situ && num->lexeme_len > 0) {
      // Use in-situ mode: reference input buffer directly
      // Safe: number_offset and number_len were validated above
      element->as.number.lexeme = (char *)(ctx->input_buffer + number_offset);
      element->as.number.lexeme_len = num->lexeme_len;
      element->as.number.is_in_situ = 1;
    }
    else if (num->lexeme && num->lexeme_len > 0) {
      // Copy lexeme if available
      size_t lexeme_len = num->lexeme_len;
      if (lexeme_len > SIZE_MAX - 1) {
        result = GTEXT_JSON_E_LIMIT;
        gtext_json_free(element);
        json_token_cleanup(token);
        break;
      }
      char * lexeme =
          (char *)json_arena_alloc_for_context(ctx, lexeme_len + 1, 1);
      if (!lexeme) {
        result = GTEXT_JSON_E_OOM;
        gtext_json_free(element);
        json_token_cleanup(token);
        break;
      }
      memcpy(lexeme, num->lexeme, lexeme_len);
      lexeme[lexeme_len] = '\0';
      element->as.number.lexeme = lexeme;
      element->as.number.lexeme_len = lexeme_len;
      element->as.number.is_in_situ = 0;
    }
    else {
      element->as.number.lexeme = NULL;
      element->as.number.lexeme_len = 0;
      element->as.number.is_in_situ = 0;
    }

    // Copy numeric representations
    if (num->flags & JSON_NUMBER_HAS_I64) {
      element->as.number.i64 = num->i64;
      element->as.number.has_i64 = 1;
    }
    if (num->flags & JSON_NUMBER_HAS_U64) {
      element->as.number.u64 = num->u64;
      element->as.number.has_u64 = 1;
    }
    if (num->flags & JSON_NUMBER_HAS_DOUBLE) {
      element->as.number.dbl = num->dbl;
      element->as.number.has_dbl = 1;
    }

    json_token_cleanup(token);
    break;
  }

  case JSON_TOKEN_LBRACKET:
    // Nested array - call json_parse_array recursively
    json_token_cleanup(token);
    result = json_parse_array(parser, &element, ctx);
    break;

  case JSON_TOKEN_LBRACE:
    // Nested object - call json_parse_object recursively
    json_token_cleanup(token);
    result = json_parse_object(parser, &element, ctx);
    break;

  case JSON_TOKEN_NAN:
  case JSON_TOKEN_INFINITY:
  case JSON_TOKEN_NEG_INFINITY: {
    // Nonfinite numbers - create as number with special lexeme
    if (!parser->opts || !parser->opts->allow_nonfinite_numbers) {
      result = json_parser_set_error(parser, GTEXT_JSON_E_NONFINITE,
          "Nonfinite numbers not allowed", token->pos);
      json_token_cleanup(token);
      break;
    }

    // Determine lexeme and double value based on token type
    const char * lexeme;
    size_t lexeme_len;
    double dbl_val;

    if (token->type == JSON_TOKEN_NAN) {
      lexeme = "NaN";
      lexeme_len = 3;
      dbl_val = NAN;
    }
    else if (token->type == JSON_TOKEN_INFINITY) {
      lexeme = "Infinity";
      lexeme_len = 8;
      dbl_val = INFINITY;
    }
    else { // JSON_TOKEN_NEG_INFINITY
      lexeme = "-Infinity";
      lexeme_len = 9;
      dbl_val = -INFINITY;
    }

    // Create value with existing context
    element = json_value_new_with_existing_context(GTEXT_JSON_NUMBER, ctx);
    if (!element) {
      result = GTEXT_JSON_E_OOM;
      json_token_cleanup(token);
      break;
    }

    // Check if we can use in-situ mode for lexeme
    // Conditions: in-situ mode enabled, context has input buffer
    int use_in_situ = 0;
    size_t number_offset = 0;
    if (parser->opts && parser->opts->in_situ_mode && ctx->input_buffer &&
        ctx->input_buffer_len > 0) {
      // Verify the number position is within bounds
      // Check for integer overflow and bounds using shared helpers
      number_offset = token->pos.offset;
      if (json_check_bounds_offset(number_offset, ctx->input_buffer_len) &&
          !json_check_add_overflow(number_offset, token->length) &&
          number_offset + token->length <= ctx->input_buffer_len) {
        use_in_situ = 1;
      }
    }

    if (use_in_situ && lexeme_len > 0) {
      // Use in-situ mode: reference input buffer directly
      // Safe: number_offset and number_len were validated above
      element->as.number.lexeme = (char *)(ctx->input_buffer + number_offset);
      element->as.number.lexeme_len = lexeme_len;
      element->as.number.is_in_situ = 1;
    }
    else if (lexeme_len > 0) {
      // Copy lexeme to arena
      // Check for overflow in lexeme_len + 1
      if (lexeme_len > SIZE_MAX - 1) {
        result = GTEXT_JSON_E_LIMIT;
        gtext_json_free(element);
        json_token_cleanup(token);
        break;
      }
      char * lexeme_copy =
          (char *)json_arena_alloc_for_context(ctx, lexeme_len + 1, 1);
      if (!lexeme_copy) {
        result = GTEXT_JSON_E_OOM;
        gtext_json_free(element);
        json_token_cleanup(token);
        break;
      }
      memcpy(lexeme_copy, lexeme, lexeme_len);
      lexeme_copy[lexeme_len] = '\0';
      element->as.number.lexeme = lexeme_copy;
      element->as.number.lexeme_len = lexeme_len;
      element->as.number.is_in_situ = 0;
    }
    else {
      element->as.number.lexeme = NULL;
      element->as.number.lexeme_len = 0;
      element->as.number.is_in_situ = 0;
    }

    // Set double representation
    element->as.number.dbl = dbl_val;
    element->as.number.has_dbl = 1;
    element->as.number.has_i64 = 0;
    element->as.number.has_u64 = 0;

    json_token_cleanup(token);
    break;
  }

  default:
    // Invalid token for array element
    result = json_parser_set_error(parser, GTEXT_JSON_E_BAD_TOKEN,
        "Unexpected token in array", token->pos);
    json_token_cleanup(token);
    break;
  }

  if (result != GTEXT_JSON_OK) {
    *out = NULL;
    return result;
  }

  if (!element) {
    *out = NULL;
    return GTEXT_JSON_E_OOM;
  }

  *out = element;
  return GTEXT_JSON_OK;
}

// Helper to find an existing key in an object
// Returns the index of the key if found, or SIZE_MAX if not found
static size_t json_object_find_key(
    const GTEXT_JSON_Value * object, const char * key, size_t key_len) {
  if (!object || object->type != GTEXT_JSON_OBJECT || !key) {
    return SIZE_MAX;
  }

  // Defensive check: pairs might be NULL if object is empty
  if (!object->as.object.pairs || object->as.object.count == 0) {
    return SIZE_MAX;
  }

  for (size_t i = 0; i < object->as.object.count; ++i) {
    // Defensive bounds check before array access
    if (!json_check_bounds_index(i, object->as.object.count)) {
      break;
    }
    if (object->as.object.pairs[i].key_len == key_len) {
      if (key_len == 0 ||
          memcmp(object->as.object.pairs[i].key, key, key_len) == 0) {
        return i;
      }
    }
  }

  return SIZE_MAX;
}

// Parse a JSON array
static GTEXT_JSON_Status json_parse_array(
    json_parser * parser, GTEXT_JSON_Value ** out, json_context * ctx) {
  // Check depth limit
  GTEXT_JSON_Status status = json_parser_check_depth(parser);
  if (status != GTEXT_JSON_OK) {
    return status;
  }

  // Check for overflow before incrementing depth
  if (json_check_add_overflow(parser->depth, 1)) {
    return json_parser_set_error(parser, GTEXT_JSON_E_DEPTH,
        "Maximum nesting depth exceeded (overflow)", parser->lexer.pos);
  }
  parser->depth++;
  GTEXT_JSON_Status result = GTEXT_JSON_OK;

  // Track if this is a root array (has its own context)
  int is_root_array = (ctx == NULL);

  // Create array value
  GTEXT_JSON_Value * array;
  if (ctx) {
    // Use existing context
    array = json_value_new_with_existing_context(GTEXT_JSON_ARRAY, ctx);
    if (!array) {
      parser->depth--;
      return json_parser_set_error(parser, GTEXT_JSON_E_OOM,
          "Failed to allocate array", parser->lexer.pos);
    }
    // Initialize empty array
    array->as.array.elems = NULL;
    array->as.array.count = 0;
    array->as.array.capacity = 0;
  }
  else {
    // Root array - create with new context
    array = gtext_json_new_array();
    if (!array) {
      parser->depth--;
      return json_parser_set_error(parser, GTEXT_JSON_E_OOM,
          "Failed to allocate array", parser->lexer.pos);
    }
    ctx = array->ctx; // Use the context from the created array
  }

  // Get opening bracket token (already consumed by caller)
  json_token token;
  int first = 1;

  while (1) {
    if (first) {
      // Get next token to check if array is empty
      status = json_lexer_next(&parser->lexer, &token);
      if (status != GTEXT_JSON_OK) {
        result = status;
        json_token_cleanup(&token);
        break;
      }

      // Check for closing bracket (empty array)
      if (token.type == JSON_TOKEN_RBRACKET) {
        json_token_cleanup(&token);
        break;
      }

      // Parse first element inline to avoid double token consumption
      // (json_parse_value would call json_lexer_next again, skipping this
      // token)

      // Check container element limit
      status = json_parser_check_container_elems(parser, array->as.array.count);
      if (status != GTEXT_JSON_OK) {
        result = status;
        json_token_cleanup(&token);
        break;
      }

      // Parse value based on token type (inline to avoid double token
      // consumption)
      GTEXT_JSON_Value * element = NULL;
      result = json_parse_array_element(parser, &token, array->ctx, &element);
      if (result != GTEXT_JSON_OK) {
        break;
      }

      // Add element to array
      status = json_array_add_element(array, element);
      if (status != GTEXT_JSON_OK) {
        result = status;
        break;
      }

      first = 0;
      continue;
    }
    else {
      // Subsequent elements: We need to get comma, then value
      // Get next token (should be comma or closing bracket)
      status = json_lexer_next(&parser->lexer, &token);
      if (status != GTEXT_JSON_OK) {
        result = status;
        json_token_cleanup(&token);
        break;
      }

      // Check for closing bracket
      if (token.type == JSON_TOKEN_RBRACKET) {
        json_token_cleanup(&token);
        break;
      }

      // Should be comma between elements
      if (token.type != JSON_TOKEN_COMMA) {
        result = json_parser_set_error_with_tokens(parser,
            GTEXT_JSON_E_BAD_TOKEN, "Expected comma between array elements",
            token.pos, json_token_type_description(JSON_TOKEN_COMMA),
            json_token_type_description(token.type));
        json_token_cleanup(&token);
        break;
      }
      json_token_cleanup(&token);

      // Get the value token after the comma
      status = json_lexer_next(&parser->lexer, &token);
      if (status != GTEXT_JSON_OK) {
        result = status;
        json_token_cleanup(&token);
        break;
      }

      // Check if this is a trailing comma (comma followed by closing bracket)
      if (token.type == JSON_TOKEN_RBRACKET) {
        if (!parser->opts || !parser->opts->allow_trailing_commas) {
          result = json_parser_set_error(parser, GTEXT_JSON_E_BAD_TOKEN,
              "Trailing comma not allowed", token.pos);
          json_token_cleanup(&token);
          break;
        }
        // Trailing comma allowed - consume closing bracket and exit
        json_token_cleanup(&token);
        break;
      }

      // Check container element limit
      status = json_parser_check_container_elems(parser, array->as.array.count);
      if (status != GTEXT_JSON_OK) {
        result = status;
        json_token_cleanup(&token);
        break;
      }

      // Parse value using the token we already have (inline to avoid double
      // consumption)
      GTEXT_JSON_Value * element = NULL;
      result = json_parse_array_element(parser, &token, array->ctx, &element);
      if (result != GTEXT_JSON_OK) {
        break;
      }

      // Add element to array
      status = json_array_add_element(array, element);
      if (status != GTEXT_JSON_OK) {
        result = status;
        break;
      }
    }
  }

  parser->depth--;

  if (result != GTEXT_JSON_OK) {
    // Only free array if it has its own context (root case).
    // If ctx was provided, array is in that context and will be freed
    // when the parent object/value is freed.
    if (is_root_array) {
      // Root array - has its own context, free it
      gtext_json_free(array);
    }
    // Otherwise, array is in parent's context, don't free it here
    return result;
  }

  *out = array;
  return GTEXT_JSON_OK;
}

// Parse a JSON object
static GTEXT_JSON_Status json_parse_object(
    json_parser * parser, GTEXT_JSON_Value ** out, json_context * ctx) {
  // Check depth limit
  GTEXT_JSON_Status status = json_parser_check_depth(parser);
  if (status != GTEXT_JSON_OK) {
    return status;
  }

  // Check for overflow before incrementing depth
  if (json_check_add_overflow(parser->depth, 1)) {
    return json_parser_set_error(parser, GTEXT_JSON_E_DEPTH,
        "Maximum nesting depth exceeded (overflow)", parser->lexer.pos);
  }
  parser->depth++;
  GTEXT_JSON_Status result = GTEXT_JSON_OK;

  // Track if this is a root object (has its own context)
  int is_root_object = (ctx == NULL);

  // Create object value
  GTEXT_JSON_Value * object;
  if (ctx) {
    // Use existing context
    object = json_value_new_with_existing_context(GTEXT_JSON_OBJECT, ctx);
    if (!object) {
      parser->depth--;
      return json_parser_set_error(parser, GTEXT_JSON_E_OOM,
          "Failed to allocate object", parser->lexer.pos);
    }
    // Initialize empty object
    object->as.object.pairs = NULL;
    object->as.object.count = 0;
    object->as.object.capacity = 0;
  }
  else {
    // Root object - create with new context
    object = gtext_json_new_object();
    if (!object) {
      parser->depth--;
      return json_parser_set_error(parser, GTEXT_JSON_E_OOM,
          "Failed to allocate object", parser->lexer.pos);
    }
    ctx = object->ctx; // Use the context from the created object
  }

  // Get opening brace token (already consumed by caller)
  json_token token;
  int first = 1;

  while (1) {
    // Get next token
    status = json_lexer_next(&parser->lexer, &token);
    if (status != GTEXT_JSON_OK) {
      result = status;
      json_token_cleanup(&token);
      break;
    }

    // Check for closing brace
    if (token.type == JSON_TOKEN_RBRACE) {
      json_token_cleanup(&token);
      break;
    }

    // Check for comma between pairs (if not first element)
    if (!first) {
      if (token.type != JSON_TOKEN_COMMA) {
        result = json_parser_set_error(parser, GTEXT_JSON_E_BAD_TOKEN,
            "Expected comma between object pairs", token.pos);
        json_token_cleanup(&token);
        break;
      }
      // Consume comma and check next token
      json_token_cleanup(&token);
      status = json_lexer_next(&parser->lexer, &token);
      if (status != GTEXT_JSON_OK) {
        result = status;
        json_token_cleanup(&token);
        break;
      }
      // Check if this is a trailing comma (comma followed by closing brace)
      if (token.type == JSON_TOKEN_RBRACE) {
        if (!parser->opts || !parser->opts->allow_trailing_commas) {
          result = json_parser_set_error(parser, GTEXT_JSON_E_BAD_TOKEN,
              "Trailing comma not allowed", token.pos);
          json_token_cleanup(&token);
          break;
        }
        // Trailing comma allowed - consume closing brace and exit
        json_token_cleanup(&token);
        break;
      }
      // Not a trailing comma - continue with key parsing (token is already the
      // key)
    }

    /* The name. A string always; an unquoted IdentifierName where JSON5 was
     * asked for, and then also one of the words the lexer reads as a keyword
     * everywhere else - `{true: 1}` is an object whose name is "true",
     * because IdentifierName includes the reserved words. Those tokens carry
     * no text of their own, so the spelling comes from the type. */
    const char * keyword_name = NULL;
    if (token.type != JSON_TOKEN_STRING && token.type != JSON_TOKEN_IDENT &&
        parser->opts && parser->opts->allow_unquoted_keys) {
      keyword_name = json_keyword_token_name(&token);
    }
    if (token.type != JSON_TOKEN_STRING && token.type != JSON_TOKEN_IDENT &&
        !keyword_name) {
      result = json_parser_set_error(parser, GTEXT_JSON_E_BAD_TOKEN,
          "Object key must be a string", token.pos);
      json_token_cleanup(&token);
      break;
    }

    // Check string size limit
    status = json_parser_check_string_size(
        parser, keyword_name ? strlen(keyword_name) : token.data.string.value_len);
    if (status != GTEXT_JSON_OK) {
      result = status;
      json_token_cleanup(&token);
      break;
    }

    // Store key and length (must save before cleanup since
    // token.data.string.value is freed)
    const char * key =
        keyword_name ? keyword_name : token.data.string.value;
    size_t key_len =
        keyword_name ? strlen(keyword_name) : token.data.string.value_len;
    json_position key_pos = token.pos; // Save position for error reporting

    // Allocate temporary copy of key to avoid use-after-free
    // (we'll use it after json_token_cleanup frees the original)
    char * key_copy = NULL;
    if (key_len > 0) {
      // Check for overflow
      if (key_len > SIZE_MAX - 1) {
        result = GTEXT_JSON_E_LIMIT;
        json_token_cleanup(&token);
        break;
      }
      key_copy = (char *)gtext_allocator_malloc(
          parser->opts ? parser->opts->allocator : NULL, key_len + 1);
      if (!key_copy) {
        result = GTEXT_JSON_E_OOM;
        json_token_cleanup(&token);
        break;
      }
      memcpy(key_copy, key, key_len);
      key_copy[key_len] = '\0';
    }

    // Get colon (now safe to cleanup token)
    json_token_cleanup(&token);
    status = json_lexer_next(&parser->lexer, &token);
    if (status != GTEXT_JSON_OK) {
      result = status;
      if (key_copy) {
        gtext_allocator_free(
            parser->opts ? parser->opts->allocator : NULL, key_copy);
      }
      break;
    }

    if (token.type != JSON_TOKEN_COLON) {
      result = json_parser_set_error_with_tokens(parser, GTEXT_JSON_E_BAD_TOKEN,
          "Expected colon after object key", token.pos,
          json_token_type_description(JSON_TOKEN_COLON),
          json_token_type_description(token.type));
      json_token_cleanup(&token);
      if (key_copy) {
        gtext_allocator_free(
            parser->opts ? parser->opts->allocator : NULL, key_copy);
      }
      break;
    }

    json_token_cleanup(&token);

    // Check container element limit
    status = json_parser_check_container_elems(parser, object->as.object.count);
    if (status != GTEXT_JSON_OK) {
      result = status;
      if (key_copy) {
        gtext_allocator_free(
            parser->opts ? parser->opts->allocator : NULL, key_copy);
      }
      break;
    }

    // Parse value (use object's context)
    GTEXT_JSON_Value * value = NULL;
    status = json_parse_value(parser, &value, object->ctx);
    if (status != GTEXT_JSON_OK) {
      result = status;
      if (key_copy) {
        gtext_allocator_free(
            parser->opts ? parser->opts->allocator : NULL, key_copy);
      }
      break;
    }

    // Handle duplicate key policies
    GTEXT_JSON_Dupkey_Mode dupkey_mode =
        parser->opts ? parser->opts->dupkeys : GTEXT_JSON_DUPKEY_ERROR;
    size_t existing_idx =
        json_object_find_key(object, key_copy ? key_copy : "", key_len);

    if (existing_idx != SIZE_MAX) {
      int handled_duplicate = 0; // Flag to track if we handled a duplicate
      // Duplicate key found - handle according to policy
      int should_break = 0; // Flag to break from while loop

      switch (dupkey_mode) {
      case GTEXT_JSON_DUPKEY_ERROR: {
        // Fail parse on duplicate key
        // Note: Don't free value here - it's part of object's arena.
        // Freeing object at the end will free everything including value.
        result = json_parser_set_error(
            parser, GTEXT_JSON_E_DUPKEY, "Duplicate key in object", key_pos);
        if (key_copy) {
          gtext_allocator_free(
            parser->opts ? parser->opts->allocator : NULL, key_copy);
        }
        should_break = 1;
        break;
      }

      case GTEXT_JSON_DUPKEY_FIRST_WINS: {
        // Keep first occurrence, discard new value
        // Note: Don't free value here - it's part of object's arena.
        // It will be freed when the object is freed. Just don't add it to the
        // object. Free temporary key copy (not needed)
        if (key_copy) {
          gtext_allocator_free(
            parser->opts ? parser->opts->allocator : NULL, key_copy);
        }
        // Continue to next pair (don't break)
        status = GTEXT_JSON_OK;
        handled_duplicate = 1;
        break;
      }

      case GTEXT_JSON_DUPKEY_LAST_WINS: {
        // Replace existing value with new one
        // Note: Don't free the old value - it's part of object's arena.
        // It will be freed when the object is freed. Just overwrite the
        // pointer. Set the new value Defensive bounds check before array access
        if (json_check_bounds_index(existing_idx, object->as.object.count)) {
          object->as.object.pairs[existing_idx].value = value;
        }
        else {
          // Bounds check failed - should not happen, but be defensive
          if (key_copy) {
            gtext_allocator_free(
            parser->opts ? parser->opts->allocator : NULL, key_copy);
          }
          return json_parser_set_error(parser, GTEXT_JSON_E_INVALID,
              "Internal error: array index out of bounds", parser->lexer.pos);
        }
        // Free temporary key copy (key already in arena)
        if (key_copy) {
          gtext_allocator_free(
            parser->opts ? parser->opts->allocator : NULL, key_copy);
        }
        // Continue to next pair (don't break)
        status = GTEXT_JSON_OK;
        handled_duplicate = 1;
        break;
      }

      case GTEXT_JSON_DUPKEY_COLLECT: {
        // Convert to array on collision
        // Defensive bounds check before array access
        if (!json_check_bounds_index(existing_idx, object->as.object.count)) {
          if (key_copy) {
            gtext_allocator_free(
            parser->opts ? parser->opts->allocator : NULL, key_copy);
          }
          return json_parser_set_error(parser, GTEXT_JSON_E_INVALID,
              "Internal error: array index out of bounds", parser->lexer.pos);
        }
        GTEXT_JSON_Value * existing_value =
            object->as.object.pairs[existing_idx].value;

        // Check if existing value is already an array
        if (existing_value->type == GTEXT_JSON_ARRAY) {
          // Append new value to existing array
          status = json_array_add_element(existing_value, value);
          if (status != GTEXT_JSON_OK) {
            // Note: Don't free value here - it's part of object's arena.
            // Freeing object at the end will free everything including value.
            result = status;
            if (key_copy) {
              gtext_allocator_free(
            parser->opts ? parser->opts->allocator : NULL, key_copy);
            }
            should_break = 1;
            break;
          }
          // Free temporary key copy (not needed)
          if (key_copy) {
            gtext_allocator_free(
            parser->opts ? parser->opts->allocator : NULL, key_copy);
          }
          // Continue to next pair (don't break)
          status = GTEXT_JSON_OK;
        }
        else {
          // Convert single value to array [old_value, new_value]
          GTEXT_JSON_Value * array = json_value_new_with_existing_context(
              GTEXT_JSON_ARRAY, object->ctx);
          if (!array) {
            // Note: Don't free value here - it's part of object's arena.
            result = GTEXT_JSON_E_OOM;
            if (key_copy) {
              gtext_allocator_free(
            parser->opts ? parser->opts->allocator : NULL, key_copy);
            }
            should_break = 1;
            break;
          }

          // Add old value to array
          status = json_array_add_element(array, existing_value);
          if (status != GTEXT_JSON_OK) {
            // Note: Don't free array or value here - they're part of object's
            // arena. Freeing object at the end will free everything.
            result = status;
            if (key_copy) {
              gtext_allocator_free(
            parser->opts ? parser->opts->allocator : NULL, key_copy);
            }
            should_break = 1;
            break;
          }

          // Add new value to array
          status = json_array_add_element(array, value);
          if (status != GTEXT_JSON_OK) {
            // Note: Don't free array or value here - they're part of object's
            // arena. Freeing object at the end will free everything.
            result = status;
            if (key_copy) {
              gtext_allocator_free(
            parser->opts ? parser->opts->allocator : NULL, key_copy);
            }
            should_break = 1;
            break;
          }

          // Replace old value with array
          // Defensive bounds check before array access (already checked above,
          // but be safe)
          if (json_check_bounds_index(existing_idx, object->as.object.count)) {
            object->as.object.pairs[existing_idx].value = array;
          }
          else {
            // Should not happen, but be defensive
            if (key_copy) {
              gtext_allocator_free(
            parser->opts ? parser->opts->allocator : NULL, key_copy);
            }
            return json_parser_set_error(parser, GTEXT_JSON_E_INVALID,
                "Internal error: array index out of bounds", parser->lexer.pos);
          }
          // Free temporary key copy (key already in arena)
          if (key_copy) {
            gtext_allocator_free(
            parser->opts ? parser->opts->allocator : NULL, key_copy);
          }
          // Continue to next pair (don't break)
          status = GTEXT_JSON_OK;
        }
        handled_duplicate = 1;
        break;
      }

      default: {
        // Unknown mode - treat as error
        // Note: Don't free value here - it's part of object's arena.
        result = GTEXT_JSON_E_INVALID;
        if (key_copy) {
          gtext_allocator_free(
            parser->opts ? parser->opts->allocator : NULL, key_copy);
        }
        should_break = 1;
        break;
      }
      }

      if (should_break) {
        break;
      }
      // If we handled a duplicate and didn't break, continue to next iteration
      // Skip the else block which would try to add the pair again
      if (handled_duplicate) {
        first = 0;
        continue; // Continue to next iteration of while loop
      }
    }
    else {
      // No duplicate - add pair normally
      // json_object_add_pair copies the key into the arena
      status = json_object_add_pair(
          object, key_copy ? key_copy : "", key_len, value);
      if (status != GTEXT_JSON_OK) {
        gtext_json_free(value);
        result = status;
        if (key_copy) {
          gtext_allocator_free(
            parser->opts ? parser->opts->allocator : NULL, key_copy);
        }
        break;
      }

      // Free temporary key copy (json_object_add_pair copied it to arena)
      if (key_copy) {
        gtext_allocator_free(
            parser->opts ? parser->opts->allocator : NULL, key_copy);
      }
    }

    // After handling a pair (duplicate or not), continue to next iteration
    // This will consume the next token (comma or closing brace) in the next
    // loop iteration

    first = 0;
  }

  parser->depth--;

  if (result != GTEXT_JSON_OK) {
    // Only free the object if it has its own context (root case). A nested
    // object lives in the parent's arena, and gtext_json_free() frees the
    // whole arena - so freeing it here destroyed the parent as well, and the
    // parent's own error path then read from freed memory. Five bytes of
    // truncated input ({"":{) were enough to reach it. The array parser
    // already guarded against exactly this.
    if (is_root_object) {
      gtext_json_free(object);
    }
    return result;
  }

  *out = object;
  return GTEXT_JSON_OK;
}

/**
 * @brief Say what a status means, for the paths that do not say it themselves.
 *
 * Most failures describe themselves on the way out. Some return a status and
 * nothing else, and the caller then had a NULL document with err.code still
 * GTEXT_JSON_OK - a failure that reads as success to anyone who checks the
 * code rather than the pointer. A leading-zero number and a string holding
 * invalid UTF-8 both arrived that way.
 */
static const char *json_status_message(GTEXT_JSON_Status status) {
  switch (status) {
    case GTEXT_JSON_E_INVALID:    return "Invalid JSON";
    case GTEXT_JSON_E_OOM:        return "Out of memory";
    case GTEXT_JSON_E_LIMIT:      return "Resource limit exceeded";
    case GTEXT_JSON_E_DEPTH:      return "Maximum nesting depth exceeded";
    case GTEXT_JSON_E_INCOMPLETE: return "Incomplete JSON input";
    case GTEXT_JSON_E_BAD_TOKEN:  return "Invalid token";
    case GTEXT_JSON_E_BAD_NUMBER: return "Invalid number";
    case GTEXT_JSON_E_BAD_ESCAPE: return "Invalid escape sequence";
    case GTEXT_JSON_E_BAD_UNICODE:
      return "Invalid Unicode: a string is not well-formed UTF-8";
    case GTEXT_JSON_E_TRAILING_GARBAGE:
      return "Trailing content after the JSON value";
    case GTEXT_JSON_E_DUPKEY:     return "Duplicate key in object";
    case GTEXT_JSON_E_NONFINITE:  return "Nonfinite numbers not allowed";
    case GTEXT_JSON_E_STATE:      return "Invalid state for operation";
    default:                      return "Parse error";
  }
}

// Parse a JSON value (recursive entry point)
static GTEXT_JSON_Status json_parse_value(
    json_parser * parser, GTEXT_JSON_Value ** out, json_context * ctx) {
  json_token token;
  memset(&token, 0, sizeof(token));
  GTEXT_JSON_Status status = json_lexer_next(&parser->lexer, &token);
  if (status != GTEXT_JSON_OK) {
    // Lexer returned an error - set error structure if available
    if (parser->error_out) {
      parser->error_out->code = status;
      // Set appropriate error message based on error code
      parser->error_out->message = json_status_message(status);
      // Use token position if available, otherwise use lexer position
      if (token.type != 0) { // Token was initialized
        parser->error_out->offset = token.pos.offset;
        parser->error_out->line = token.pos.line;
        parser->error_out->col = token.pos.col;
      }
      else {
        parser->error_out->offset = parser->lexer.pos.offset;
        parser->error_out->line = parser->lexer.pos.line;
        parser->error_out->col = parser->lexer.pos.col;
      }
    }
    json_token_cleanup(&token);
    return status;
  }

  GTEXT_JSON_Status result = GTEXT_JSON_OK;
  GTEXT_JSON_Value * value = NULL;

  // If ctx is NULL, this is the root value - create it using gtext_json_new_*
  // to establish context Otherwise, use existing context
  int is_root = (ctx == NULL);

  switch (token.type) {
  case JSON_TOKEN_NULL:
    if (is_root) {
      value = gtext_json_new_null();
    }
    else {
      value = json_value_new_with_existing_context(GTEXT_JSON_NULL, ctx);
    }
    if (!value) {
      result = GTEXT_JSON_E_OOM;
    }
    break;

  case JSON_TOKEN_TRUE:
    if (is_root) {
      value = gtext_json_new_bool(true);
    }
    else {
      value = json_value_new_with_existing_context(GTEXT_JSON_BOOL, ctx);
      if (value) {
        value->as.boolean = 1;
      }
    }
    if (!value) {
      result = GTEXT_JSON_E_OOM;
    }
    break;

  case JSON_TOKEN_FALSE:
    if (is_root) {
      value = gtext_json_new_bool(false);
    }
    else {
      value = json_value_new_with_existing_context(GTEXT_JSON_BOOL, ctx);
      if (value) {
        value->as.boolean = 0;
      }
    }
    if (!value) {
      result = GTEXT_JSON_E_OOM;
    }
    break;

  case JSON_TOKEN_STRING: {
    // Check string size limit
    status = json_parser_check_string_size(parser, token.data.string.value_len);
    if (status != GTEXT_JSON_OK) {
      result = status;
      json_token_cleanup(&token);
      break;
    }

    // Create string value
    if (is_root) {
      // For root, create context first, then check in-situ mode
      value = gtext_json_new_string(
          token.data.string.value, token.data.string.value_len);
      if (!value) {
        result = GTEXT_JSON_E_OOM;
        json_token_cleanup(&token);
        break;
      }
      // After creating root value, input buffer will be set in
      // gtext_json_parse() So we can't use in-situ mode for root strings at
      // creation time They will be copied, which is fine for root values
      value->as.string.is_in_situ = 0;
    }
    else {
      value = json_value_new_with_existing_context(GTEXT_JSON_STRING, ctx);
      if (!value) {
        result = GTEXT_JSON_E_OOM;
        json_token_cleanup(&token);
        break;
      }

      // Check if we can use in-situ mode
      // Conditions: in-situ mode enabled, context has input buffer, no escape
      // sequences
      int use_in_situ = 0;
      size_t original_start = 0;
      size_t original_len = 0;
      // Same exclusion as the other in-situ string site: a normalised string
      // can match the input's length without matching its bytes.
      if (parser->opts && parser->opts->in_situ_mode &&
          !parser->opts->normalize_unicode && ctx->input_buffer &&
          ctx->input_buffer_len > 0 &&
          token.data.string.value_len == token.data.string.original_len) {
        // Verify the original string position is within bounds
        // Check for integer overflow and bounds
        original_start = token.data.string.original_start;
        original_len = token.data.string.original_len;
        if (original_start < ctx->input_buffer_len &&
            original_len <= ctx->input_buffer_len - original_start) {
          use_in_situ = 1;
        }
      }

      if (use_in_situ) {
        // Use in-situ mode: reference input buffer directly
        // Safe: original_start and original_len were validated above
        value->as.string.data = (char *)(ctx->input_buffer + original_start);
        value->as.string.len = original_len;
        value->as.string.is_in_situ = 1;
      }
      else {
        // Allocate string data in arena
        // Check for overflow in value_len + 1
        if (token.data.string.value_len > SIZE_MAX - 1) {
          result = GTEXT_JSON_E_LIMIT;
          json_token_cleanup(&token);
          break;
        }
        char * str_data = (char *)json_arena_alloc_for_context(
            ctx, token.data.string.value_len + 1, 1);
        if (!str_data) {
          result = GTEXT_JSON_E_OOM;
          json_token_cleanup(&token);
          break;
        }

        memcpy(str_data, token.data.string.value, token.data.string.value_len);
        str_data[token.data.string.value_len] = '\0';

        value->as.string.data = str_data;
        value->as.string.len = token.data.string.value_len;
        value->as.string.is_in_situ = 0;
      }
    }

    // Note: We've copied the string to the arena. The lexer-allocated string
    // will be freed by json_token_cleanup, so we don't need to free it
    // manually. Just let json_token_cleanup handle the cleanup.
    json_token_cleanup(&token);
    break;
  }

  case JSON_TOKEN_NUMBER: {
    json_number * num = &token.data.number;

    // For root, create with gtext_json_new_number_from_lexeme then update
    // representations For children, create with existing context
    if (is_root) {
      value = gtext_json_new_number_from_lexeme(
          num->lexeme ? num->lexeme : "", num->lexeme_len);
      if (!value) {
        result = GTEXT_JSON_E_OOM;
        json_token_cleanup(&token);
        break;
      }
      // After creating root value, input buffer will be set in
      // gtext_json_parse() So we can't use in-situ mode for root numbers at
      // creation time They will be copied, which is fine for root values
      value->as.number.is_in_situ = 0;
    }
    else {
      value = json_value_new_with_existing_context(GTEXT_JSON_NUMBER, ctx);
      if (!value) {
        result = GTEXT_JSON_E_OOM;
        json_token_cleanup(&token);
        break;
      }

      // Check if we can use in-situ mode for lexeme
      // Conditions: in-situ mode enabled, context has input buffer, lexeme
      // exists
      int use_in_situ = 0;
      size_t number_offset = 0;
      if (parser->opts && parser->opts->in_situ_mode && ctx->input_buffer &&
          ctx->input_buffer_len > 0 && num->lexeme && num->lexeme_len > 0) {
        // Verify the number position is within bounds
        // Check for integer overflow and bounds using shared helpers
        number_offset = token.pos.offset;
        if (json_check_bounds_offset(number_offset, ctx->input_buffer_len) &&
            !json_check_add_overflow(number_offset, token.length) &&
            number_offset + token.length <= ctx->input_buffer_len) {
          use_in_situ = 1;
        }
      }

      if (use_in_situ && num->lexeme_len > 0) {
        // Use in-situ mode: reference input buffer directly
        // Safe: number_offset and number_len were validated above
        value->as.number.lexeme = (char *)(ctx->input_buffer + number_offset);
        value->as.number.lexeme_len = num->lexeme_len;
        value->as.number.is_in_situ = 1;
      }
      else if (num->lexeme && num->lexeme_len > 0) {
        // Copy lexeme to arena
        // Check for overflow in lexeme_len + 1
        if (num->lexeme_len > SIZE_MAX - 1) {
          result = GTEXT_JSON_E_LIMIT;
          json_token_cleanup(&token);
          break;
        }
        char * lexeme =
            (char *)json_arena_alloc_for_context(ctx, num->lexeme_len + 1, 1);
        if (!lexeme) {
          result = GTEXT_JSON_E_OOM;
          json_token_cleanup(&token);
          break;
        }
        memcpy(lexeme, num->lexeme, num->lexeme_len);
        lexeme[num->lexeme_len] = '\0';
        value->as.number.lexeme = lexeme;
        value->as.number.lexeme_len = num->lexeme_len;
        value->as.number.is_in_situ = 0;
      }
      else {
        value->as.number.lexeme = NULL;
        value->as.number.lexeme_len = 0;
        value->as.number.is_in_situ = 0;
      }
    }

    // Copy number representations (for both root and children)
    if (num->flags & JSON_NUMBER_HAS_I64) {
      value->as.number.i64 = num->i64;
      value->as.number.has_i64 = 1;
    }
    else {
      value->as.number.has_i64 = 0;
    }
    if (num->flags & JSON_NUMBER_HAS_U64) {
      value->as.number.u64 = num->u64;
      value->as.number.has_u64 = 1;
    }
    else {
      value->as.number.has_u64 = 0;
    }
    if (num->flags & JSON_NUMBER_HAS_DOUBLE) {
      value->as.number.dbl = num->dbl;
      value->as.number.has_dbl = 1;
    }
    else {
      value->as.number.has_dbl = 0;
    }

    // Clean up temporary number structure
    json_number_destroy(num);
    json_token_cleanup(&token);
    break;
  }

  case JSON_TOKEN_LBRACKET:
    json_token_cleanup(&token);
    result = json_parse_array(parser, &value, is_root ? NULL : ctx);
    break;

  case JSON_TOKEN_LBRACE:
    json_token_cleanup(&token);
    result = json_parse_object(parser, &value, is_root ? NULL : ctx);
    break;

  case JSON_TOKEN_NAN:
  case JSON_TOKEN_INFINITY:
  case JSON_TOKEN_NEG_INFINITY: {
    // Nonfinite numbers - create as number with special lexeme
    if (!parser->opts || !parser->opts->allow_nonfinite_numbers) {
      result = json_parser_set_error(parser, GTEXT_JSON_E_NONFINITE,
          "Nonfinite numbers not allowed", token.pos);
      json_token_cleanup(&token);
      break;
    }

    // Determine lexeme and double value based on token type
    const char * lexeme;
    size_t lexeme_len;
    double dbl_val;

    if (token.type == JSON_TOKEN_NAN) {
      lexeme = "NaN";
      lexeme_len = 3;
      dbl_val = NAN;
    }
    else if (token.type == JSON_TOKEN_INFINITY) {
      lexeme = "Infinity";
      lexeme_len = 8;
      dbl_val = INFINITY;
    }
    else { // JSON_TOKEN_NEG_INFINITY
      lexeme = "-Infinity";
      lexeme_len = 9;
      dbl_val = -INFINITY;
    }

    // Create value (similar to regular number handling)
    if (is_root) {
      value = gtext_json_new_number_from_lexeme(lexeme, lexeme_len);
      if (!value) {
        result = GTEXT_JSON_E_OOM;
        json_token_cleanup(&token);
        break;
      }
      // After creating root value, input buffer will be set in
      // gtext_json_parse() So we can't use in-situ mode for root numbers at
      // creation time They will be copied, which is fine for root values
      value->as.number.is_in_situ = 0;
    }
    else {
      value = json_value_new_with_existing_context(GTEXT_JSON_NUMBER, ctx);
      if (!value) {
        result = GTEXT_JSON_E_OOM;
        json_token_cleanup(&token);
        break;
      }

      // Check if we can use in-situ mode for lexeme
      // Conditions: in-situ mode enabled, context has input buffer
      int use_in_situ = 0;
      size_t number_offset = 0;
      if (parser->opts && parser->opts->in_situ_mode && ctx->input_buffer &&
          ctx->input_buffer_len > 0) {
        // Verify the number position is within bounds
        // Check for integer overflow and bounds using shared helpers
        number_offset = token.pos.offset;
        if (json_check_bounds_offset(number_offset, ctx->input_buffer_len) &&
            !json_check_add_overflow(number_offset, token.length) &&
            number_offset + token.length <= ctx->input_buffer_len) {
          use_in_situ = 1;
        }
      }

      if (use_in_situ && lexeme_len > 0) {
        // Use in-situ mode: reference input buffer directly
        // Safe: number_offset and number_len were validated above
        value->as.number.lexeme = (char *)(ctx->input_buffer + number_offset);
        value->as.number.lexeme_len = lexeme_len;
        value->as.number.is_in_situ = 1;
      }
      else if (lexeme_len > 0) {
        // Copy lexeme to arena
        // Check for overflow in lexeme_len + 1
        if (lexeme_len > SIZE_MAX - 1) {
          result = GTEXT_JSON_E_LIMIT;
          json_token_cleanup(&token);
          break;
        }
        char * lexeme_copy =
            (char *)json_arena_alloc_for_context(ctx, lexeme_len + 1, 1);
        if (!lexeme_copy) {
          result = GTEXT_JSON_E_OOM;
          gtext_json_free(value);
          json_token_cleanup(&token);
          break;
        }
        memcpy(lexeme_copy, lexeme, lexeme_len);
        lexeme_copy[lexeme_len] = '\0';
        value->as.number.lexeme = lexeme_copy;
        value->as.number.lexeme_len = lexeme_len;
        value->as.number.is_in_situ = 0;
      }
      else {
        value->as.number.lexeme = NULL;
        value->as.number.lexeme_len = 0;
        value->as.number.is_in_situ = 0;
      }
    }

    // Set double representation
    value->as.number.dbl = dbl_val;
    value->as.number.has_dbl = 1;
    value->as.number.has_i64 = 0;
    value->as.number.has_u64 = 0;

    json_token_cleanup(&token);
    break;
  }

  default:
    // This should not happen for valid JSON input
    // Common causes: lexer error, incomplete input, or internal bug
    result = json_parser_set_error(
        parser, GTEXT_JSON_E_BAD_TOKEN, "Unexpected token", token.pos);
    json_token_cleanup(&token);
    break;
  }

  if (result != GTEXT_JSON_OK) {
    // Only free value if it has its own context (root case).
    // If ctx was provided, value is in that context and will be freed
    // when the parent object/array is freed.
    if (is_root && value) {
      // Root value - has its own context, free it
      gtext_json_free(value);
    }
    // Otherwise, value is in parent's context, don't free it here
    return result;
  }

  *out = value;
  return GTEXT_JSON_OK;
}

// Internal helper function for parsing a single JSON value
static GTEXT_JSON_Value * json_parse_internal(const char * bytes, size_t len,
    const GTEXT_JSON_Parse_Options * opt, GTEXT_JSON_Error * err,
    int allow_multiple, size_t * bytes_consumed) {
  // Defensive NULL pointer checks
  if (!bytes && len > 0) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Input bytes must not be NULL",
          .line = 1,
          .col = 1};
    }
    if (bytes_consumed) {
      *bytes_consumed = 0;
    }
    return NULL;
  }

  // normalize_unicode is implemented now - the lexer normalises every string
  // it decodes.  What is refused is asking for NFC without asking for UTF-8
  // validation, because normalising bytes that have not been established as
  // text is not a defined operation: the normaliser would refuse the string
  // the parser had just agreed to accept, and the two answers would come from
  // options that look independent.  validate_utf8 is on by default, so this
  // only reaches a caller who turned it off deliberately.
  if (opt && opt->normalize_unicode && !opt->validate_utf8) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "normalize_unicode requires validate_utf8",
          .line = 1,
          .col = 1};
    }
    if (bytes_consumed) {
      *bytes_consumed = 0;
    }
    return NULL;
  }

  // Input size validation: check for reasonable input size before processing
  if (len > SIZE_MAX / 2) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Input size is too large",
          .line = 1,
          .col = 1};
    }
    if (bytes_consumed) {
      *bytes_consumed = 0;
    }
    return NULL;
  }

  // max_total_bytes was enforced for file reads (json_file_io.c) and for the
  // streaming parser (json_stream.c) but not here, so an in-memory parse
  // ignored it entirely: a 600-byte document parsed with max_total_bytes set
  // to 50.  The comment in gtext_json_parse() asserted that this function did
  // the check, which is presumably why nobody looked.
  //
  // The whole input is already in hand, so the check is on len rather than on
  // bytes consumed so far; the streaming parser necessarily does it the other
  // way, and both use the same limit and message.
  {
    size_t max_total =
        json_get_limit(opt ? opt->max_total_bytes : 0,
            JSON_DEFAULT_MAX_TOTAL_BYTES);
    if (len > max_total) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_LIMIT,
            .message = "Maximum total input size exceeded",
            .line = 1,
            .col = 1};
      }
      if (bytes_consumed) {
        *bytes_consumed = 0;
      }
      return NULL;
    }
  }

  /*
   * "NULL for defaults", which every entry point's documentation promises, has
   * to be made true here rather than assumed downstream.
   *
   * It was not. `opt` was passed on as NULL, and json_parse_number() reads its
   * options as `if (opts && opts->preserve_number_lexeme)` and
   * `if (opts && opts->parse_int64)` - so with no options every number came back
   * with no preserved lexeme, no int64 and no double. The value parsed, reported
   * type NUMBER, and held nothing: gtext_json_get_i64() and _get_double() both
   * answered GTEXT_JSON_E_INVALID, and gtext_json_write_value() could not write
   * it, so any parsed document containing a number failed to serialize. Passing
   * gtext_json_parse_options_default() explicitly worked, which is why the tests
   * never saw it - they all pass options.
   */
  GTEXT_JSON_Parse_Options effective_opts;
  if (!opt) {
    effective_opts = gtext_json_parse_options_default();
    opt = &effective_opts;
  }

  // Initialize parser state
  json_parser parser = {0};
  parser.opts = opt;
  parser.depth = 0;
  parser.total_bytes_consumed = 0;
  parser.error_out = err;

  // Initialize lexer
  GTEXT_JSON_Status status =
      json_lexer_init(&parser.lexer, bytes, len, opt, 0, /* at start */ 1);
  if (status != GTEXT_JSON_OK) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = status,
          .message = "Failed to initialize lexer",
          .line = 1,
          .col = 1};
    }
    if (bytes_consumed) {
      *bytes_consumed = 0;
    }
    return NULL;
  }

  // The root context is always created here rather than being left to
  // json_parse_value(), which would fall back to gtext_json_new_*() and with
  // it the default allocator.  That fallback is why a caller-supplied
  // allocator used to see the parser's transient buffers but not the arena
  // holding the DOM - everything the parse actually returns.
  json_context * root_ctx = json_context_new(opt ? opt->allocator : NULL);
  if (!root_ctx) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
          .message = "Failed to allocate context",
          .line = 1,
          .col = 1};
    }
    if (bytes_consumed) {
      *bytes_consumed = 0;
    }
    return NULL;
  }
  if (opt && opt->in_situ_mode) {
    json_context_set_input_buffer(root_ctx, bytes, len);
  }

  GTEXT_JSON_Value * root = NULL;
  status = json_parse_value(&parser, &root, root_ctx);

  if (status != GTEXT_JSON_OK) {
    if (root) {
      gtext_json_free(root);
    }
    else if (root_ctx) {
      json_context_free(root_ctx);
    }
    if (bytes_consumed) {
      *bytes_consumed = 0;
    }
    /* Whoever refused the input is expected to have described it, and most
       paths do. The ones that do not left the caller holding a NULL document
       and an error struct that still said GTEXT_JSON_OK - see
       json_status_message(). */
    if (err && err->code == GTEXT_JSON_OK) {
      err->code = status;
      err->message = json_status_message(status);
    }
    return NULL;
  }

  // If root_ctx was created, it's now owned by root->ctx
  // If root was created with gtext_json_new_ *, we need to set input buffer on
  // its context
  if (root && root->ctx && opt && opt->in_situ_mode && !root_ctx) {
    json_context_set_input_buffer(root->ctx, bytes, len);
  }

  // Check for trailing content
  json_token token;
  status = json_lexer_next(&parser.lexer, &token);

  if (status != GTEXT_JSON_OK) {
    // Error from lexer when checking for trailing content
    // This means there's invalid input after the successfully parsed value
    // Note: When json_lexer_next returns an error, token.pos may not be set
    // (it's initialized to 0), so we use parser.lexer.pos which is always valid
    if (allow_multiple) {
      // Multiple values allowed - return the value and bytes_consumed pointing
      // to error
      if (bytes_consumed) {
        // Use lexer position (always valid) as the error occurred at current
        // lexer position Check for bounds to prevent overflow
        *bytes_consumed =
            (parser.lexer.pos.offset <= len) ? parser.lexer.pos.offset : len;
      }
      json_token_cleanup(&token);
      return root;
    }
    else {
      // Single value mode - treat trailing invalid content as error
      gtext_json_free(root);
      json_token_cleanup(&token);
      if (err) {
        json_position pos = parser.lexer.pos;
        // Clamp offset to input length
        if (pos.offset > len) {
          pos.offset = len;
        }
        json_parser_set_error(&parser, GTEXT_JSON_E_TRAILING_GARBAGE,
            "Trailing garbage after valid JSON", pos);
      }
      if (bytes_consumed) {
        *bytes_consumed = 0;
      }
      return NULL;
    }
  }

  if (token.type != JSON_TOKEN_EOF) {
    // There's trailing content (valid token)
    if (allow_multiple) {
      // Multiple top-level values allowed - return bytes consumed (position of
      // next token)
      if (bytes_consumed) {
        // token.pos.offset is the start of the next token, which is correct
        // Check bounds to prevent overflow
        *bytes_consumed = (token.pos.offset <= len) ? token.pos.offset : len;
      }
      json_token_cleanup(&token);
      return root;
    }
    else {
      // Trailing garbage not allowed - error
      gtext_json_free(root);
      json_token_cleanup(&token);
      if (err) {
        json_position pos = token.pos;
        // Clamp offset to input length
        if (pos.offset > len) {
          pos.offset = len;
        }
        json_parser_set_error(&parser, GTEXT_JSON_E_TRAILING_GARBAGE,
            "Trailing garbage after valid JSON", pos);
      }
      if (bytes_consumed) {
        *bytes_consumed = 0;
      }
      return NULL;
    }
  }

  // EOF reached - bytes consumed is the lexer position (end of input)
  if (bytes_consumed) {
    // Check bounds to prevent overflow
    *bytes_consumed =
        (parser.lexer.pos.offset <= len) ? parser.lexer.pos.offset : len;
  }
  json_token_cleanup(&token);

  return root;
}

GTEXT_API GTEXT_JSON_Value * gtext_json_parse(const char * bytes, size_t len,
    const GTEXT_JSON_Parse_Options * opt, GTEXT_JSON_Error * err) {
  // Input validation: check for NULL bytes when len > 0
  if (!bytes && len > 0) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Input bytes must not be NULL when length is non-zero",
          .line = 1,
          .col = 1};
    }
    return NULL;
  }

  // Check for obviously invalid values here; max_total_bytes is enforced in
  // json_parse_internal.
  if (len > SIZE_MAX / 2) {
    // Input size is suspiciously large (more than half of SIZE_MAX)
    // This could indicate an overflow or invalid input
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Input size is too large",
          .line = 1,
          .col = 1};
    }
    return NULL;
  }

  // Always treat trailing content as error (single value only)
  return json_parse_internal(bytes, len, opt, err, 0, NULL);
}

GTEXT_API GTEXT_JSON_Value * gtext_json_parse_multiple(const char * bytes,
    size_t len, const GTEXT_JSON_Parse_Options * opt, GTEXT_JSON_Error * err,
    size_t * bytes_consumed) {
  // Input validation: bytes_consumed is required
  if (!bytes_consumed) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "bytes_consumed must not be NULL",
          .line = 1,
          .col = 1};
    }
    return NULL;
  }

  // Input validation: check for NULL bytes when len > 0
  if (!bytes && len > 0) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Input bytes must not be NULL when length is non-zero",
          .line = 1,
          .col = 1};
    }
    *bytes_consumed = 0;
    return NULL;
  }

  // Input validation: check for reasonable input size
  if (len > SIZE_MAX / 2) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Input size is too large",
          .line = 1,
          .col = 1};
    }
    *bytes_consumed = 0;
    return NULL;
  }
  // Allow multiple values and return bytes consumed
  return json_parse_internal(bytes, len, opt, err, 1, bytes_consumed);
}
