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
 * JSON Patch (RFC 6902) implementation.
 */

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/macros.h>
#include "json_internal.h"

#include <ghoti.io/text/json/json_core.h>
#include <ghoti.io/text/json/json_dom.h>
#include <ghoti.io/text/json/json_patch.h>
#include <ghoti.io/text/json/json_pointer.h>
// Helper function to deep clone a JSON value into the same context
// This is needed for the copy operation and schema validation
// Made non-static so it can be shared with json_schema.c
/* Everything about a clone except its children: the node, its scalar payload,
   and - for a container - the child storage, sized for the source and left
   empty.  The children are filled in by json_value_clone(), which does it
   without recursing. */
static GTEXT_JSON_Value * json_value_clone_shallow(
    const GTEXT_JSON_Value * src, json_context * ctx) {
  GTEXT_JSON_Value * dst = json_value_new_with_existing_context(src->type, ctx);
  if (!dst) {
    return NULL;
  }

  switch (src->type) {
  case GTEXT_JSON_NULL:
    // Nothing to copy
    break;

  case GTEXT_JSON_BOOL:
    dst->as.boolean = src->as.boolean;
    break;

  case GTEXT_JSON_STRING: {
    // Allocate and copy string data
    // Check for integer overflow in len + 1
    if (src->as.string.len > SIZE_MAX - 1) {
      return NULL; // Overflow
    }
    char * str_data =
        (char *)json_arena_alloc_for_context(ctx, src->as.string.len + 1, 1);
    if (!str_data) {
      return NULL;
    }
    memcpy(str_data, src->as.string.data, src->as.string.len);
    str_data[src->as.string.len] = '\0';
    dst->as.string.data = str_data;
    dst->as.string.len = src->as.string.len;
    break;
  }

  case GTEXT_JSON_NUMBER: {
    // Allocate and copy lexeme
    // Check for integer overflow in lexeme_len + 1
    if (src->as.number.lexeme_len > SIZE_MAX - 1) {
      return NULL; // Overflow
    }
    char * lexeme = (char *)json_arena_alloc_for_context(
        ctx, src->as.number.lexeme_len + 1, 1);
    if (!lexeme) {
      return NULL;
    }
    memcpy(lexeme, src->as.number.lexeme, src->as.number.lexeme_len);
    lexeme[src->as.number.lexeme_len] = '\0';
    dst->as.number.lexeme = lexeme;
    dst->as.number.lexeme_len = src->as.number.lexeme_len;
    dst->as.number.i64 = src->as.number.i64;
    dst->as.number.u64 = src->as.number.u64;
    dst->as.number.dbl = src->as.number.dbl;
    dst->as.number.has_i64 = src->as.number.has_i64;
    dst->as.number.has_u64 = src->as.number.has_u64;
    dst->as.number.has_dbl = src->as.number.has_dbl;
    break;
  }

  case GTEXT_JSON_ARRAY: {
    // Initialize array
    dst->as.array.count = 0;
    dst->as.array.capacity = src->as.array.count;
    if (dst->as.array.capacity > 0) {
      // Check for integer overflow in multiplication
      size_t elem_size = sizeof(GTEXT_JSON_Value *);
      if (dst->as.array.capacity > SIZE_MAX / elem_size) {
        return NULL; // Overflow
      }
      dst->as.array.elems = (GTEXT_JSON_Value **)json_arena_alloc_for_context(
          ctx, dst->as.array.capacity * elem_size, sizeof(GTEXT_JSON_Value *));
      if (!dst->as.array.elems) {
        return NULL;
      }
    }
    else {
      dst->as.array.elems = NULL;
    }
    break;
  }

  case GTEXT_JSON_OBJECT: {
    // Initialize object
    dst->as.object.count = 0;
    dst->as.object.capacity = src->as.object.count;
    if (dst->as.object.capacity > 0) {
      // Allocate pairs array - cast through void * to avoid anonymous struct
      // type mismatch
      size_t pair_size = sizeof(*(src->as.object.pairs));
      // Check for integer overflow in multiplication
      if (dst->as.object.capacity > SIZE_MAX / pair_size) {
        return NULL; // Overflow
      }
      void * new_pairs_ptr = json_arena_alloc_for_context(
          ctx, dst->as.object.capacity * pair_size, sizeof(void *));
      if (!new_pairs_ptr) {
        return NULL;
      }
      // Assign through void * cast (types match structurally)
      dst->as.object.pairs = (void *)new_pairs_ptr;
    }
    else {
      dst->as.object.pairs = NULL;
    }
    break;
  }
  }

  return dst;
}

/* Where a clone is inside one container: which child of @src is next, and the
   @dst that is being filled. */
typedef struct {
  const GTEXT_JSON_Value * src;
  GTEXT_JSON_Value * dst;
  size_t i;
} json_clone_frame;

/* Deep clone @p src into @p ctx, with the walk's stack on the heap.
 *
 * This is the deep copy everything in this library uses - gtext_json_clone(),
 * gtext_json_object_merge(), every patch operation that stores a value - and it
 * was a recursion.  A value built through the DOM API has no depth limit to
 * bound it: max_depth is a *parser* option, and json_core.h is careful to say
 * so.  Twenty thousand levels on a 512 KB stack took the process down, and it
 * is reached through the most ordinary call in the file.
 *
 * Cycles are not guarded against and do not need to be: a value cannot contain
 * itself, because json_check_no_cycle() refuses the insertion that would do it.
 *
 * The destination's `count` grows as children land, exactly as the recursion
 * left it, so a clone that fails part way leaves a shorter container rather
 * than one with holes in it - and its caller frees the whole context, or is
 * returning NULL into one it owns. */
GTEXT_JSON_Value * json_value_clone(
    const GTEXT_JSON_Value * src, json_context * ctx) {
  if (!src || !ctx) {
    return NULL;
  }

  GTEXT_JSON_Value * root = json_value_clone_shallow(src, ctx);
  if (!root) {
    return NULL;
  }
  if (src->type != GTEXT_JSON_ARRAY && src->type != GTEXT_JSON_OBJECT) {
    return root;
  }

  json_clone_frame inline_frames[64];
  json_clone_frame * frames = inline_frames;
  size_t count = 0;
  size_t capacity = sizeof(inline_frames) / sizeof(inline_frames[0]);
  GTEXT_JSON_Value * answer = root;

  #define JSON_CLONE_PUSH(s, d)                                                \
    do {                                                                       \
      if (count == capacity) {                                                 \
        size_t new_capacity = capacity * 2;                                    \
        json_clone_frame * grown;                                              \
        if (frames == inline_frames) {                                         \
          grown = (json_clone_frame *)gtext_allocator_malloc(                  \
              NULL, new_capacity * sizeof(*grown));                            \
          if (grown) memcpy(grown, frames, count * sizeof(*grown));            \
        }                                                                      \
        else {                                                                 \
          grown = (json_clone_frame *)gtext_allocator_realloc(                 \
              NULL, frames, new_capacity * sizeof(*grown));                    \
        }                                                                      \
        if (!grown) { answer = NULL; goto done; }                              \
        frames = grown;                                                        \
        capacity = new_capacity;                                               \
      }                                                                        \
      frames[count].src = (s);                                                 \
      frames[count].dst = (d);                                                 \
      frames[count].i = 0;                                                     \
      count++;                                                                 \
    } while (0)

  JSON_CLONE_PUSH(src, root);

  while (count > 0) {
    json_clone_frame * f = &frames[count - 1];
    const bool is_object = (f->src->type == GTEXT_JSON_OBJECT);
    const size_t children =
        is_object ? f->src->as.object.count : f->src->as.array.count;

    if (f->i >= children) {
      count--;
      continue;
    }

    const size_t i = f->i;
    const GTEXT_JSON_Value * child_src = is_object
        ? f->src->as.object.pairs[i].value
        : f->src->as.array.elems[i];
    GTEXT_JSON_Value * parent_dst = f->dst;

    if (is_object) {
      // Allocate and copy key
      // Check for integer overflow in key_len + 1
      const size_t key_len = f->src->as.object.pairs[i].key_len;
      if (key_len > SIZE_MAX - 1) {
        answer = NULL; // Overflow
        goto done;
      }
      char * key = (char *)json_arena_alloc_for_context(ctx, key_len + 1, 1);
      if (!key) {
        answer = NULL;
        goto done;
      }
      memcpy(key, f->src->as.object.pairs[i].key, key_len);
      key[key_len] = '\0';
      parent_dst->as.object.pairs[i].key = key;
      parent_dst->as.object.pairs[i].key_len = key_len;
    }

    GTEXT_JSON_Value * child_dst = NULL;
    if (child_src) {
      child_dst = json_value_clone_shallow(child_src, ctx);
      if (!child_dst) {
        answer = NULL;
        goto done;
      }
    }
    else {
      /* The recursion returned NULL for a NULL child and its caller treated
         that as a failure, so a container holding a NULL child could not be
         cloned at all.  Kept, rather than quietly changed: it is a separate
         question from this one. */
      answer = NULL;
      goto done;
    }

    if (is_object) {
      parent_dst->as.object.pairs[i].value = child_dst;
      parent_dst->as.object.count++;
    }
    else {
      parent_dst->as.array.elems[i] = child_dst;
      parent_dst->as.array.count++;
    }

    /* Advanced before the push, which can move the frame array. */
    f->i++;

    if (child_src->type == GTEXT_JSON_ARRAY
        || child_src->type == GTEXT_JSON_OBJECT) {
      JSON_CLONE_PUSH(child_src, child_dst);
    }
  }

done:
  #undef JSON_CLONE_PUSH
  if (frames != inline_frames) {
    gtext_allocator_free(NULL, frames);
  }
  return answer;
}

/* Deep equality for the test operation and for schema validation.
 *
 * This was a second copy of the predicate gtext_json_equal() already
 * implements, written out again in this file: the same type switch, the same
 * i64-then-u64-then-double-then-lexeme ladder for numbers, the same
 * unordered key match for objects.  Two copies of one decision drift, and
 * these had: the one in json_dom.c checks for NULL string data and NULL child
 * storage before reading them and this one did not.
 *
 * It was also a recursion, like everything else that walked a value in this
 * file, and so had no bound for a value built through the DOM API.  Both
 * problems have one answer, which is to ask the function that already knows.
 *
 * GTEXT_JSON_EQUAL_NUMERIC is what this always did: compare numbers by value
 * where both sides have a representation, and fall back to the lexeme. */
int json_value_equal(const GTEXT_JSON_Value * a, const GTEXT_JSON_Value * b) {
  return gtext_json_equal(a, b, GTEXT_JSON_EQUAL_NUMERIC) ? 1 : 0;
}

// Parse a string field from an operation object
static GTEXT_JSON_Status json_patch_get_string_field(
    const GTEXT_JSON_Value * op, const char * field_name, const char ** out_str,
    size_t * out_len) {
  if (!op || op->type != GTEXT_JSON_OBJECT) {
    return GTEXT_JSON_E_INVALID;
  }

  const GTEXT_JSON_Value * field_val =
      gtext_json_object_get(op, field_name, strlen(field_name));
  if (!field_val) {
    return GTEXT_JSON_E_INVALID;
  }

  if (field_val->type != GTEXT_JSON_STRING) {
    return GTEXT_JSON_E_INVALID;
  }

  *out_str = field_val->as.string.data;
  *out_len = field_val->as.string.len;
  return GTEXT_JSON_OK;
}

// Parse the "op" field from an operation object
static GTEXT_JSON_Status json_patch_get_op(const GTEXT_JSON_Value * op,
    const char ** out_op_str, size_t * out_op_len) {
  return json_patch_get_string_field(op, "op", out_op_str, out_op_len);
}

// Parse the "path" field from an operation object
static GTEXT_JSON_Status json_patch_get_path(const GTEXT_JSON_Value * op,
    const char ** out_path_str, size_t * out_path_len) {
  return json_patch_get_string_field(op, "path", out_path_str, out_path_len);
}

// Parse the "from" field from an operation object
static GTEXT_JSON_Status json_patch_get_from(const GTEXT_JSON_Value * op,
    const char ** out_from_str, size_t * out_from_len) {
  return json_patch_get_string_field(op, "from", out_from_str, out_from_len);
}

// Parse the "value" field from an operation object
static const GTEXT_JSON_Value * json_patch_get_value(
    const GTEXT_JSON_Value * op) {
  if (!op || op->type != GTEXT_JSON_OBJECT) {
    return NULL;
  }

  const GTEXT_JSON_Value * value_val = gtext_json_object_get(op, "value", 5);
  return value_val;
}

// Check if "from" is a proper prefix of "path" (for move operation validation)
static int json_pointer_is_prefix(
    const char * from, size_t from_len, const char * path, size_t path_len) {
  if (from_len >= path_len) {
    return 0; // from cannot be a prefix if it's longer or equal
  }

  // Check if path starts with from
  if (memcmp(from, path, from_len) != 0) {
    return 0;
  }

  // Check that the next character in path is '/' (proper prefix)
  if (path[from_len] == '/') {
    return 1;
  }

  return 0;
}

// Helper to find parent and last token of a JSON pointer
// Returns the parent value and the last token (key or index)
static GTEXT_JSON_Status json_patch_find_parent_and_token(
    GTEXT_JSON_Value * root, const char * path, size_t path_len,
    GTEXT_JSON_Value ** out_parent, const char ** out_token,
    size_t * out_token_len, int * out_is_array_index) {
  if (!root || !path || path_len == 0) {
    return GTEXT_JSON_E_INVALID;
  }

  /*
   * No options structure, and none needed: a patch is applied to a tree, and
   * every GTEXT_JSON_Value carries the context that made it.  json_context
   * records the allocator the parse was given and documents it as never NULL,
   * so the caller's allocator is already in reach of the argument - which is
   * why JSON Patch needed no API change to stop using the C library here.
   */
  const GTEXT_Allocator * alloc = root->ctx ? root->ctx->alloc : NULL;

  // Empty path means root itself
  if (path_len == 1 && path[0] == '/') {
    *out_parent = NULL; // No parent (root is the target)
    *out_token = NULL;
    *out_token_len = 0;
    *out_is_array_index = 0;
    return GTEXT_JSON_OK;
  }

  // Find the last '/' in the path
  const char * last_slash = NULL;
  size_t last_slash_pos = 0;
  for (size_t i = path_len; i > 0; i--) {
    // Check for unescaped '/'
    if (path[i - 1] == '/') {
      last_slash = &path[i - 1];
      last_slash_pos = i - 1;
      break;
    }
  }

  if (!last_slash) {
    // No '/' found, path is just a single token
    // This means we're operating on root with a single key/index
    if (path[0] != '/') {
      return GTEXT_JSON_E_INVALID;
    }

    *out_parent = root;
    *out_token = path + 1; // Skip leading '/'
    *out_token_len = path_len - 1;

    // Check if it's an array index
    int is_idx = 0;
    if (*out_token_len > 0) {
      // Try parsing as index
      char * token_buf =
          (char *)gtext_allocator_malloc(alloc, *out_token_len + 1);
      if (!token_buf) {
        return GTEXT_JSON_E_OOM;
      }
      memcpy(token_buf, *out_token, *out_token_len);
      token_buf[*out_token_len] = '\0';

      // Decode escape sequences
      size_t decoded_len;
      char * decoded =
          (char *)gtext_allocator_malloc(alloc, *out_token_len + 1);
      if (!decoded) {
        gtext_allocator_free(alloc, token_buf);
        return GTEXT_JSON_E_OOM;
      }
      // Simple decode: ~0 -> ~, ~1 -> /
      // Note: out_pos can never exceed *out_token_len because each input char
      // produces at most one output char (escape sequences consume 2 input,
      // produce 1 output)
      size_t out_pos = 0;
      for (size_t i = 0; i < *out_token_len; i++) {
        // Defensive bounds check (should never trigger, but prevents overflow)
        if (out_pos >= *out_token_len) {
          gtext_allocator_free(alloc, decoded);
          gtext_allocator_free(alloc, token_buf);
          return GTEXT_JSON_E_INVALID;
        }
        if (token_buf[i] == '~' && i + 1 < *out_token_len) {
          if (token_buf[i + 1] == '0') {
            decoded[out_pos++] = '~';
            i++;
          }
          else if (token_buf[i + 1] == '1') {
            decoded[out_pos++] = '/';
            i++;
          }
          else {
            decoded[out_pos++] = token_buf[i];
          }
        }
        else {
          decoded[out_pos++] = token_buf[i];
        }
      }
      decoded_len = out_pos;
      decoded[decoded_len] = '\0'; // Null-terminate for strtoull

      // Check if it's a valid array index
      if (decoded_len > 0 && isdigit((unsigned char)decoded[0])) {
        // Check for leading zeros
        if (decoded_len == 1 || (decoded[0] != '0')) {
          // Try to parse as index
          char * endptr;
          unsigned long long parsed = strtoull(decoded, &endptr, 10);
          if (*endptr == '\0' && parsed <= SIZE_MAX) {
            is_idx = 1;
          }
        }
      }

      gtext_allocator_free(alloc, decoded);
      gtext_allocator_free(alloc, token_buf);
    }

    *out_is_array_index = is_idx;
    return GTEXT_JSON_OK;
  }

  // Extract parent path (everything before last '/')
  size_t parent_path_len = last_slash_pos;
  char * parent_path =
      (char *)gtext_allocator_malloc(alloc, parent_path_len + 1);
  if (!parent_path) {
    return GTEXT_JSON_E_OOM;
  }
  memcpy(parent_path, path, parent_path_len);
  parent_path[parent_path_len] = '\0';

  // Get parent value
  GTEXT_JSON_Value * parent =
      gtext_json_pointer_get_mut(root, parent_path, parent_path_len);
  gtext_allocator_free(alloc, parent_path);

  if (!parent) {
    return GTEXT_JSON_E_INVALID;
  }

  *out_parent = parent;
  *out_token = last_slash + 1; // After the '/'
  *out_token_len = path_len - (last_slash_pos + 1);

  // Check if it's an array index
  int is_idx = 0;
  if (*out_token_len > 0) {
    // Try parsing as index
    char * token_buf =
          (char *)gtext_allocator_malloc(alloc, *out_token_len + 1);
    if (!token_buf) {
      return GTEXT_JSON_E_OOM;
    }
    memcpy(token_buf, *out_token, *out_token_len);
    token_buf[*out_token_len] = '\0';

    // Decode escape sequences
    size_t decoded_len;
    char * decoded =
          (char *)gtext_allocator_malloc(alloc, *out_token_len + 1);
    if (!decoded) {
      gtext_allocator_free(alloc, token_buf);
      return GTEXT_JSON_E_OOM;
    }
    // Simple decode: ~0 -> ~, ~1 -> /
    // Note: out_pos can never exceed *out_token_len because each input char
    // produces at most one output char (escape sequences consume 2 input,
    // produce 1 output)
    size_t out_pos = 0;
    for (size_t i = 0; i < *out_token_len; i++) {
      // Defensive bounds check (should never trigger, but prevents overflow)
      if (out_pos >= *out_token_len) {
        gtext_allocator_free(alloc, decoded);
        gtext_allocator_free(alloc, token_buf);
        return GTEXT_JSON_E_INVALID;
      }
      if (token_buf[i] == '~' && i + 1 < *out_token_len) {
        if (token_buf[i + 1] == '0') {
          decoded[out_pos++] = '~';
          i++;
        }
        else if (token_buf[i + 1] == '1') {
          decoded[out_pos++] = '/';
          i++;
        }
        else {
          decoded[out_pos++] = token_buf[i];
        }
      }
      else {
        decoded[out_pos++] = token_buf[i];
      }
    }
    decoded_len = out_pos;
    decoded[decoded_len] = '\0'; // Null-terminate for strtoull

    // Check if it's a valid array index
    if (decoded_len > 0 && isdigit((unsigned char)decoded[0])) {
      // Check for leading zeros
      if (decoded_len == 1 || (decoded[0] != '0')) {
        // Try to parse as index
        char * endptr;
        unsigned long long parsed = strtoull(decoded, &endptr, 10);
        if (*endptr == '\0' && parsed <= SIZE_MAX) {
          is_idx = 1;
        }
      }
    }

    gtext_allocator_free(alloc, decoded);
    gtext_allocator_free(alloc, token_buf);
  }

  *out_is_array_index = is_idx;
  return GTEXT_JSON_OK;
}

// Implement add operation
static GTEXT_JSON_Status json_patch_add(GTEXT_JSON_Value * root,
    const char * path, size_t path_len, const GTEXT_JSON_Value * value,
    GTEXT_JSON_Error * err) {
  const GTEXT_Allocator * alloc = root && root->ctx ? root->ctx->alloc : NULL;
  if (!root || !path || !value) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Invalid arguments for add operation"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  // Empty path means replace root
  if (path_len == 0 || (path_len == 1 && path[0] == '/')) {
    // Cannot add at root (would need to replace entire tree)
    if (err) {
      *err = (GTEXT_JSON_Error){
          .code = GTEXT_JSON_E_INVALID, .message = "Cannot add at root path"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  // Find parent and token
  GTEXT_JSON_Value * parent = NULL;
  const char * token = NULL;
  size_t token_len = 0;
  int is_array_index = 0;
  GTEXT_JSON_Status status = json_patch_find_parent_and_token(
      root, path, path_len, &parent, &token, &token_len, &is_array_index);
  if (status != GTEXT_JSON_OK) {
    if (err) {
      *err = (GTEXT_JSON_Error){
          .code = status, .message = "Invalid path for add operation"};
    }
    return status;
  }

  if (!parent) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Parent not found for add operation"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  // Clone the value into the parent's context
  json_context * ctx = parent->ctx;
  GTEXT_JSON_Value * cloned_value = json_value_clone(value, ctx);
  if (!cloned_value) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
          .message = "Out of memory cloning value for add operation"};
    }
    return GTEXT_JSON_E_OOM;
  }

  // Check if parent is an array - if so, treat token as array index (including
  // "-")
  if (parent->type == GTEXT_JSON_ARRAY) {
    // Add to array

    // Parse index
    char * token_buf = (char *)gtext_allocator_malloc(alloc, token_len + 1);
    if (!token_buf) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
            .message = "Out of memory parsing array index"};
      }
      return GTEXT_JSON_E_OOM;
    }
    memcpy(token_buf, token, token_len);
    token_buf[token_len] = '\0';

    // Decode escape sequences
    size_t decoded_len;
    char * decoded = (char *)gtext_allocator_malloc(alloc, token_len + 1);
    if (!decoded) {
      gtext_allocator_free(alloc, token_buf);
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
            .message = "Out of memory decoding array index"};
      }
      return GTEXT_JSON_E_OOM;
    }
    size_t out_pos = 0;
    for (size_t i = 0; i < token_len; i++) {
      if (token_buf[i] == '~' && i + 1 < token_len) {
        if (token_buf[i + 1] == '0') {
          decoded[out_pos++] = '~';
          i++;
        }
        else if (token_buf[i + 1] == '1') {
          decoded[out_pos++] = '/';
          i++;
        }
        else {
          decoded[out_pos++] = token_buf[i];
        }
      }
      else {
        decoded[out_pos++] = token_buf[i];
      }
    }
    decoded_len = out_pos;
    decoded[decoded_len] = '\0';

    size_t idx;
    if (strcmp(decoded, "-") == 0) {
      // Append to end
      idx = parent->as.array.count;
    }
    else {
      char * endptr;
      unsigned long long parsed = strtoull(decoded, &endptr, 10);
      if (*endptr != '\0' || parsed > SIZE_MAX) {
        gtext_allocator_free(alloc, decoded);
        gtext_allocator_free(alloc, token_buf);
        if (err) {
          *err = (GTEXT_JSON_Error){
              .code = GTEXT_JSON_E_INVALID, .message = "Invalid array index"};
        }
        return GTEXT_JSON_E_INVALID;
      }
      idx = (size_t)parsed;
      if (idx > parent->as.array.count) {
        gtext_allocator_free(alloc, decoded);
        gtext_allocator_free(alloc, token_buf);
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "Array index out of bounds"};
        }
        return GTEXT_JSON_E_INVALID;
      }
    }

    gtext_allocator_free(alloc, decoded);
    gtext_allocator_free(alloc, token_buf);

    // Insert at index
    status = gtext_json_array_insert(parent, idx, cloned_value);
    if (status != GTEXT_JSON_OK) {
      if (err) {
        *err = (GTEXT_JSON_Error){
            .code = status, .message = "Failed to insert into array"};
      }
      return status;
    }
  }
  else {
    // Add to object (or replace existing key)
    if (parent->type != GTEXT_JSON_OBJECT) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
            .message = "Path points to object key but parent is not an object"};
      }
      return GTEXT_JSON_E_INVALID;
    }

    // Decode token (handle escape sequences)
    char * decoded_key =
      (char *)gtext_allocator_malloc(alloc, token_len + 1);
    if (!decoded_key) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
            .message = "Out of memory decoding object key"};
      }
      return GTEXT_JSON_E_OOM;
    }

    size_t out_pos = 0;
    for (size_t i = 0; i < token_len; i++) {
      // Defensive bounds check (should never trigger, but prevents overflow)
      if (out_pos >= token_len) {
        gtext_allocator_free(alloc, decoded_key);
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "Buffer overflow in key decoding"};
        }
        return GTEXT_JSON_E_INVALID;
      }
      if (token[i] == '~' && i + 1 < token_len) {
        if (token[i + 1] == '0') {
          decoded_key[out_pos++] = '~';
          i++;
        }
        else if (token[i + 1] == '1') {
          decoded_key[out_pos++] = '/';
          i++;
        }
        else {
          decoded_key[out_pos++] = token[i];
        }
      }
      else {
        decoded_key[out_pos++] = token[i];
      }
    }
    size_t decoded_key_len = out_pos;

    // Put key-value pair (replaces if exists)
    status = gtext_json_object_put(
        parent, decoded_key, decoded_key_len, cloned_value);
    gtext_allocator_free(alloc, decoded_key);

    if (status != GTEXT_JSON_OK) {
      if (err) {
        *err = (GTEXT_JSON_Error){
            .code = status, .message = "Failed to add to object"};
      }
      return status;
    }
  }

  return GTEXT_JSON_OK;
}

// Implement remove operation
static GTEXT_JSON_Status json_patch_remove(GTEXT_JSON_Value * root,
    const char * path, size_t path_len, GTEXT_JSON_Error * err) {
  const GTEXT_Allocator * alloc = root && root->ctx ? root->ctx->alloc : NULL;
  if (!root || !path) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Invalid arguments for remove operation"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  // Empty path means remove root (not allowed)
  if (path_len == 0 || (path_len == 1 && path[0] == '/')) {
    if (err) {
      *err = (GTEXT_JSON_Error){
          .code = GTEXT_JSON_E_INVALID, .message = "Cannot remove root"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  // Find parent and token
  GTEXT_JSON_Value * parent = NULL;
  const char * token = NULL;
  size_t token_len = 0;
  int is_array_index = 0;
  GTEXT_JSON_Status status = json_patch_find_parent_and_token(
      root, path, path_len, &parent, &token, &token_len, &is_array_index);
  if (status != GTEXT_JSON_OK) {
    if (err) {
      *err = (GTEXT_JSON_Error){
          .code = status, .message = "Invalid path for remove operation"};
    }
    return status;
  }

  if (!parent) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Parent not found for remove operation"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  // Check if parent is an array - if so, treat token as array index
  if (parent->type == GTEXT_JSON_ARRAY) {
    // Remove from array

    // Parse index
    char * token_buf = (char *)gtext_allocator_malloc(alloc, token_len + 1);
    if (!token_buf) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
            .message = "Out of memory parsing array index"};
      }
      return GTEXT_JSON_E_OOM;
    }
    memcpy(token_buf, token, token_len);
    token_buf[token_len] = '\0';

    // Decode escape sequences
    size_t decoded_len;
    char * decoded = (char *)gtext_allocator_malloc(alloc, token_len + 1);
    if (!decoded) {
      gtext_allocator_free(alloc, token_buf);
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
            .message = "Out of memory decoding array index"};
      }
      return GTEXT_JSON_E_OOM;
    }
    size_t out_pos = 0;
    for (size_t i = 0; i < token_len; i++) {
      // Defensive bounds check (should never trigger, but prevents overflow)
      if (out_pos >= token_len) {
        gtext_allocator_free(alloc, decoded);
        gtext_allocator_free(alloc, token_buf);
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "Buffer overflow in token decoding"};
        }
        return GTEXT_JSON_E_INVALID;
      }
      if (token_buf[i] == '~' && i + 1 < token_len) {
        if (token_buf[i + 1] == '0') {
          decoded[out_pos++] = '~';
          i++;
        }
        else if (token_buf[i + 1] == '1') {
          decoded[out_pos++] = '/';
          i++;
        }
        else {
          decoded[out_pos++] = token_buf[i];
        }
      }
      else {
        decoded[out_pos++] = token_buf[i];
      }
    }
    decoded_len = out_pos;
    decoded[decoded_len] = '\0';

    char * endptr;
    unsigned long long parsed = strtoull(decoded, &endptr, 10);
    if (*endptr != '\0' || parsed > SIZE_MAX) {
      gtext_allocator_free(alloc, decoded);
      gtext_allocator_free(alloc, token_buf);
      if (err) {
        *err = (GTEXT_JSON_Error){
            .code = GTEXT_JSON_E_INVALID, .message = "Invalid array index"};
      }
      return GTEXT_JSON_E_INVALID;
    }
    size_t idx = (size_t)parsed;

    gtext_allocator_free(alloc, decoded);
    gtext_allocator_free(alloc, token_buf);

    if (idx >= parent->as.array.count) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
            .message = "Array index out of bounds"};
      }
      return GTEXT_JSON_E_INVALID;
    }

    status = gtext_json_array_remove(parent, idx);
    if (status != GTEXT_JSON_OK) {
      if (err) {
        *err = (GTEXT_JSON_Error){
            .code = status, .message = "Failed to remove from array"};
      }
      return status;
    }
  }
  else {
    // Remove from object
    if (parent->type != GTEXT_JSON_OBJECT) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
            .message = "Path points to object key but parent is not an object"};
      }
      return GTEXT_JSON_E_INVALID;
    }

    // Decode token (handle escape sequences)
    char * decoded_key =
      (char *)gtext_allocator_malloc(alloc, token_len + 1);
    if (!decoded_key) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
            .message = "Out of memory decoding object key"};
      }
      return GTEXT_JSON_E_OOM;
    }

    size_t out_pos = 0;
    for (size_t i = 0; i < token_len; i++) {
      // Defensive bounds check (should never trigger, but prevents overflow)
      if (out_pos >= token_len) {
        gtext_allocator_free(alloc, decoded_key);
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "Buffer overflow in key decoding"};
        }
        return GTEXT_JSON_E_INVALID;
      }
      if (token[i] == '~' && i + 1 < token_len) {
        if (token[i + 1] == '0') {
          decoded_key[out_pos++] = '~';
          i++;
        }
        else if (token[i + 1] == '1') {
          decoded_key[out_pos++] = '/';
          i++;
        }
        else {
          decoded_key[out_pos++] = token[i];
        }
      }
      else {
        decoded_key[out_pos++] = token[i];
      }
    }
    size_t decoded_key_len = out_pos;

    status = gtext_json_object_remove(parent, decoded_key, decoded_key_len);
    gtext_allocator_free(alloc, decoded_key);

    if (status != GTEXT_JSON_OK) {
      if (err) {
        *err = (GTEXT_JSON_Error){
            .code = status, .message = "Failed to remove from object"};
      }
      return status;
    }
  }

  return GTEXT_JSON_OK;
}

// Implement replace operation
static GTEXT_JSON_Status json_patch_replace(GTEXT_JSON_Value * root,
    const char * path, size_t path_len, const GTEXT_JSON_Value * value,
    GTEXT_JSON_Error * err) {
  // Replace is semantically equivalent to remove + add
  // But we need to ensure the target exists first
  const GTEXT_JSON_Value * target =
      gtext_json_pointer_get(root, path, path_len);
  if (!target) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Target path does not exist for replace operation"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  // Remove then add
  GTEXT_JSON_Status status = json_patch_remove(root, path, path_len, err);
  if (status != GTEXT_JSON_OK) {
    return status;
  }

  return json_patch_add(root, path, path_len, value, err);
}

// Implement move operation
static GTEXT_JSON_Status json_patch_move(GTEXT_JSON_Value * root,
    const char * from, size_t from_len, const char * path, size_t path_len,
    GTEXT_JSON_Error * err) {
  if (!root || !from || !path) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Invalid arguments for move operation"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  // Check that "from" is not a proper prefix of "path"
  if (json_pointer_is_prefix(from, from_len, path, path_len)) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Cannot move a location into one of its own descendants"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  // Get value from "from" location
  const GTEXT_JSON_Value * from_value =
      gtext_json_pointer_get(root, from, from_len);
  if (!from_value) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Source path does not exist for move operation"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  // Clone the value (it will be added to the target's context)
  // First, find the target parent to get its context
  GTEXT_JSON_Value * target_parent = NULL;
  const char * token = NULL;
  size_t token_len = 0;
  int is_array_index = 0;
  GTEXT_JSON_Status status = json_patch_find_parent_and_token(root, path,
      path_len, &target_parent, &token, &token_len, &is_array_index);
  if (status != GTEXT_JSON_OK || !target_parent) {
    if (err) {
      *err = (GTEXT_JSON_Error){
          .code = status != GTEXT_JSON_OK ? status : GTEXT_JSON_E_INVALID,
          .message = "Invalid target path for move operation"};
    }
    return status != GTEXT_JSON_OK ? status : GTEXT_JSON_E_INVALID;
  }

  json_context * target_ctx = target_parent->ctx;
  GTEXT_JSON_Value * cloned_value = json_value_clone(from_value, target_ctx);
  if (!cloned_value) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
          .message = "Out of memory cloning value for move operation"};
    }
    return GTEXT_JSON_E_OOM;
  }

  // RFC 6902 section 4.4: move is "functionally identical to a 'remove'
  // operation on the 'from' location, followed immediately by an 'add'
  // operation at the target location with the value that was just removed".
  //
  // This used to add first and remove second.  The order is unobservable when
  // the two locations are in different containers, which is what every
  // existing test did, and decides the answer when they are in the same
  // array, because the removal shifts the indices the add is measured
  // against.  Appendix A.7 is exactly that case: moving /foo/1 to /foo/3 in
  // ["all","grass","cows","eat"] must give ["all","cows","eat","grass"], and
  // adding first gave ["all","cows","grass","eat"].
  //
  // The value is cloned above, before either step, so the removal freeing the
  // source does not matter.  target_ctx is the document's arena and stays
  // valid across the removal even though target_parent may not; json_patch_add
  // locates the parent again for itself.
  status = json_patch_remove(root, from, from_len, err);
  if (status != GTEXT_JSON_OK) {
    return status;
  }

  return json_patch_add(root, path, path_len, cloned_value, err);
}

// Implement copy operation
static GTEXT_JSON_Status json_patch_copy(GTEXT_JSON_Value * root,
    const char * from, size_t from_len, const char * path, size_t path_len,
    GTEXT_JSON_Error * err) {
  if (!root || !from || !path) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Invalid arguments for copy operation"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  // Get value from "from" location
  const GTEXT_JSON_Value * from_value =
      gtext_json_pointer_get(root, from, from_len);
  if (!from_value) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Source path does not exist for copy operation"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  // Add to target location (add will clone the value)
  return json_patch_add(root, path, path_len, from_value, err);
}

// Implement test operation
static GTEXT_JSON_Status json_patch_test(GTEXT_JSON_Value * root,
    const char * path, size_t path_len, const GTEXT_JSON_Value * expected_value,
    GTEXT_JSON_Error * err) {
  if (!root || !path || !expected_value) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Invalid arguments for test operation"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  // Get value at path
  const GTEXT_JSON_Value * actual_value =
      gtext_json_pointer_get(root, path, path_len);
  if (!actual_value) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Path does not exist for test operation"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  // Compare values
  if (!json_value_equal(actual_value, expected_value)) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Test operation failed: values are not equal"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  return GTEXT_JSON_OK;
}

// Helper function to deep copy content from source to destination
// This preserves the destination's context but replaces its content
// Used for atomic patch application: apply to clone, then copy back to original
// Note: This function allows type changes because merge patch can change the
// target's type (e.g., object -> null, string -> object)
static GTEXT_JSON_Status json_value_copy_content(
    GTEXT_JSON_Value * dst, const GTEXT_JSON_Value * src) {
  if (!dst || !src) {
    return GTEXT_JSON_E_INVALID;
  }

  // If types differ, we need to change the type first
  // The old content will remain in memory but won't be accessible
  // since we're changing the type. It will be freed when the context is freed.
  if (dst->type != src->type) {
    dst->type = src->type;
    // Initialize the union to a safe state based on new type
    // This prevents accessing old type's data
    switch (src->type) {
    case GTEXT_JSON_NULL:
      // Nothing to initialize
      break;
    case GTEXT_JSON_BOOL:
      dst->as.boolean = 0;
      break;
    case GTEXT_JSON_STRING:
      dst->as.string.data = NULL;
      dst->as.string.len = 0;
      break;
    case GTEXT_JSON_NUMBER:
      dst->as.number.lexeme = NULL;
      dst->as.number.lexeme_len = 0;
      dst->as.number.i64 = 0;
      dst->as.number.u64 = 0;
      dst->as.number.dbl = 0.0;
      dst->as.number.has_i64 = 0;
      dst->as.number.has_u64 = 0;
      dst->as.number.has_dbl = 0;
      break;
    case GTEXT_JSON_ARRAY:
      dst->as.array.elems = NULL;
      dst->as.array.count = 0;
      dst->as.array.capacity = 0;
      break;
    case GTEXT_JSON_OBJECT:
      dst->as.object.pairs = NULL;
      dst->as.object.count = 0;
      dst->as.object.capacity = 0;
      break;
    }
  }

  json_context * dst_ctx = dst->ctx;
  if (!dst_ctx) {
    return GTEXT_JSON_E_INVALID;
  }

  switch (src->type) {
  case GTEXT_JSON_NULL:
    // Nothing to copy
    break;

  case GTEXT_JSON_BOOL:
    dst->as.boolean = src->as.boolean;
    break;

  case GTEXT_JSON_STRING: {
    // Free old string data (if any) - it's in the arena, will be freed with
    // context Allocate and copy new string data Check for integer overflow in
    // len + 1
    if (src->as.string.len > SIZE_MAX - 1) {
      return GTEXT_JSON_E_OOM; // Overflow
    }
    char * str_data = (char *)json_arena_alloc_for_context(
        dst_ctx, src->as.string.len + 1, 1);
    if (!str_data) {
      return GTEXT_JSON_E_OOM;
    }
    memcpy(str_data, src->as.string.data, src->as.string.len);
    str_data[src->as.string.len] = '\0';
    dst->as.string.data = str_data;
    dst->as.string.len = src->as.string.len;
    break;
  }

  case GTEXT_JSON_NUMBER: {
    // Allocate and copy lexeme
    // Check for integer overflow in lexeme_len + 1
    if (src->as.number.lexeme_len > SIZE_MAX - 1) {
      return GTEXT_JSON_E_OOM; // Overflow
    }
    char * lexeme = (char *)json_arena_alloc_for_context(
        dst_ctx, src->as.number.lexeme_len + 1, 1);
    if (!lexeme) {
      return GTEXT_JSON_E_OOM;
    }
    memcpy(lexeme, src->as.number.lexeme, src->as.number.lexeme_len);
    lexeme[src->as.number.lexeme_len] = '\0';
    dst->as.number.lexeme = lexeme;
    dst->as.number.lexeme_len = src->as.number.lexeme_len;
    dst->as.number.i64 = src->as.number.i64;
    dst->as.number.u64 = src->as.number.u64;
    dst->as.number.dbl = src->as.number.dbl;
    dst->as.number.has_i64 = src->as.number.has_i64;
    dst->as.number.has_u64 = src->as.number.has_u64;
    dst->as.number.has_dbl = src->as.number.has_dbl;
    break;
  }

  case GTEXT_JSON_ARRAY: {
    // Free old array elements (they're in the arena, will be freed with
    // context) Allocate new array
    dst->as.array.count = 0;
    dst->as.array.capacity = src->as.array.count;
    if (dst->as.array.capacity > 0) {
      // Check for integer overflow in multiplication
      size_t elem_size = sizeof(GTEXT_JSON_Value *);
      if (dst->as.array.capacity > SIZE_MAX / elem_size) {
        return GTEXT_JSON_E_OOM; // Overflow
      }
      dst->as.array.elems =
          (GTEXT_JSON_Value **)json_arena_alloc_for_context(dst_ctx,
              dst->as.array.capacity * elem_size, sizeof(GTEXT_JSON_Value *));
      if (!dst->as.array.elems) {
        return GTEXT_JSON_E_OOM;
      }
    }
    else {
      dst->as.array.elems = NULL;
    }

    // Clone each element
    for (size_t i = 0; i < src->as.array.count; i++) {
      GTEXT_JSON_Value * cloned_elem =
          json_value_clone(src->as.array.elems[i], dst_ctx);
      if (!cloned_elem) {
        return GTEXT_JSON_E_OOM;
      }
      dst->as.array.elems[i] = cloned_elem;
      dst->as.array.count++;
    }
    break;
  }

  case GTEXT_JSON_OBJECT: {
    // Free old object pairs (they're in the arena, will be freed with context)
    // Allocate new pairs array
    dst->as.object.count = 0;
    dst->as.object.capacity = src->as.object.count;
    if (dst->as.object.capacity > 0) {
      size_t pair_size = sizeof(*(src->as.object.pairs));
      // Check for integer overflow in multiplication
      if (dst->as.object.capacity > SIZE_MAX / pair_size) {
        return GTEXT_JSON_E_OOM; // Overflow
      }
      void * new_pairs_ptr = json_arena_alloc_for_context(
          dst_ctx, dst->as.object.capacity * pair_size, sizeof(void *));
      if (!new_pairs_ptr) {
        return GTEXT_JSON_E_OOM;
      }
      dst->as.object.pairs = (void *)new_pairs_ptr;
    }
    else {
      dst->as.object.pairs = NULL;
    }

    // Clone each key-value pair
    for (size_t i = 0; i < src->as.object.count; i++) {
      // Allocate and copy key
      // Check for integer overflow in key_len + 1
      if (src->as.object.pairs[i].key_len > SIZE_MAX - 1) {
        return GTEXT_JSON_E_OOM; // Overflow
      }
      char * key = (char *)json_arena_alloc_for_context(
          dst_ctx, src->as.object.pairs[i].key_len + 1, 1);
      if (!key) {
        return GTEXT_JSON_E_OOM;
      }
      memcpy(key, src->as.object.pairs[i].key, src->as.object.pairs[i].key_len);
      key[src->as.object.pairs[i].key_len] = '\0';

      // Clone value
      GTEXT_JSON_Value * cloned_val =
          json_value_clone(src->as.object.pairs[i].value, dst_ctx);
      if (!cloned_val) {
        return GTEXT_JSON_E_OOM;
      }

      dst->as.object.pairs[i].key = key;
      dst->as.object.pairs[i].key_len = src->as.object.pairs[i].key_len;
      dst->as.object.pairs[i].value = cloned_val;
      dst->as.object.count++;
    }
    break;
  }
  }

  return GTEXT_JSON_OK;
}

// Main patch apply function
// Implements true atomicity: applies all operations to a clone,
// then replaces the original only if all operations succeed
GTEXT_API GTEXT_JSON_Status gtext_json_patch_apply(GTEXT_JSON_Value * root,
    const GTEXT_JSON_Value * patch_array, GTEXT_JSON_Error * err) {
  if (!root || !patch_array) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message =
              "Invalid arguments: root and patch_array must not be NULL"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  if (patch_array->type != GTEXT_JSON_ARRAY) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Patch must be a JSON array"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  /*
   * For atomicity: clone the root, apply operations to the clone, then copy
   * the clone's content back to the original only if all succeed.
   *
   * The clone's context takes the root's allocator.  Passing NULL here put a
   * deep copy of the *entire document* in the C library's heap on every call,
   * which is the largest single allocation a patch makes and the one a caller
   * with an arena would most want inside it.  It also decided what the
   * operations below allocate through, since they read the allocator from the
   * tree they are given and that tree is this clone.
   */
  json_context * clone_ctx =
      json_context_new(root->ctx ? root->ctx->alloc : NULL);
  if (!clone_ctx) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
          .message = "Out of memory creating clone for atomic patch"};
    }
    return GTEXT_JSON_E_OOM;
  }

  GTEXT_JSON_Value * clone = json_value_clone(root, clone_ctx);
  if (!clone) {
    json_context_free(clone_ctx);
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
          .message = "Out of memory cloning root for atomic patch"};
    }
    return GTEXT_JSON_E_OOM;
  }

  // Process each operation in order on the clone
  for (size_t i = 0; i < patch_array->as.array.count; i++) {
    const GTEXT_JSON_Value * op = patch_array->as.array.elems[i];
    if (!op || op->type != GTEXT_JSON_OBJECT) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
            .message = "Patch operation must be an object"};
      }
      gtext_json_free(clone);
      return GTEXT_JSON_E_INVALID;
    }

    // Get operation type
    const char * op_str = NULL;
    size_t op_len = 0;
    GTEXT_JSON_Status status = json_patch_get_op(op, &op_str, &op_len);
    if (status != GTEXT_JSON_OK) {
      if (err) {
        *err = (GTEXT_JSON_Error){
            .code = status, .message = "Patch operation missing 'op' field"};
      }
      gtext_json_free(clone);
      return status;
    }

    // Get path
    const char * path_str = NULL;
    size_t path_len = 0;
    status = json_patch_get_path(op, &path_str, &path_len);
    if (status != GTEXT_JSON_OK) {
      if (err) {
        *err = (GTEXT_JSON_Error){
            .code = status, .message = "Patch operation missing 'path' field"};
      }
      gtext_json_free(clone);
      return status;
    }

    // Execute operation based on type
    if (op_len == 3 && memcmp(op_str, "add", 3) == 0) {
      const GTEXT_JSON_Value * value = json_patch_get_value(op);
      if (!value) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "Add operation missing 'value' field"};
        }
        gtext_json_free(clone);
        return GTEXT_JSON_E_INVALID;
      }
      status = json_patch_add(clone, path_str, path_len, value, err);
      if (status != GTEXT_JSON_OK) {
        // Operation failed - free clone and return error (atomicity)
        gtext_json_free(clone);
        return status;
      }
    }
    else if (op_len == 6 && memcmp(op_str, "remove", 6) == 0) {
      status = json_patch_remove(clone, path_str, path_len, err);
      if (status != GTEXT_JSON_OK) {
        // Operation failed - free clone and return error (atomicity)
        gtext_json_free(clone);
        return status;
      }
    }
    else if (op_len == 7 && memcmp(op_str, "replace", 7) == 0) {
      const GTEXT_JSON_Value * value = json_patch_get_value(op);
      if (!value) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "Replace operation missing 'value' field"};
        }
        gtext_json_free(clone);
        return GTEXT_JSON_E_INVALID;
      }
      status = json_patch_replace(clone, path_str, path_len, value, err);
      if (status != GTEXT_JSON_OK) {
        // Operation failed - free clone and return error (atomicity)
        gtext_json_free(clone);
        return status;
      }
    }
    else if (op_len == 4 && memcmp(op_str, "move", 4) == 0) {
      const char * from_str = NULL;
      size_t from_len = 0;
      status = json_patch_get_from(op, &from_str, &from_len);
      if (status != GTEXT_JSON_OK) {
        if (err) {
          err->code = status;
          err->message = "Move operation missing 'from' field";
          err->offset = 0;
          err->line = 0;
          err->col = 0;
        }
        gtext_json_free(clone);
        return status;
      }
      status =
          json_patch_move(clone, from_str, from_len, path_str, path_len, err);
      if (status != GTEXT_JSON_OK) {
        // Operation failed - free clone and return error (atomicity)
        gtext_json_free(clone);
        return status;
      }
    }
    else if (op_len == 4 && memcmp(op_str, "copy", 4) == 0) {
      const char * from_str = NULL;
      size_t from_len = 0;
      status = json_patch_get_from(op, &from_str, &from_len);
      if (status != GTEXT_JSON_OK) {
        if (err) {
          err->code = status;
          err->message = "Copy operation missing 'from' field";
          err->offset = 0;
          err->line = 0;
          err->col = 0;
        }
        gtext_json_free(clone);
        return status;
      }
      status =
          json_patch_copy(clone, from_str, from_len, path_str, path_len, err);
      if (status != GTEXT_JSON_OK) {
        // Operation failed - free clone and return error (atomicity)
        gtext_json_free(clone);
        return status;
      }
    }
    else if (op_len == 4 && memcmp(op_str, "test", 4) == 0) {
      const GTEXT_JSON_Value * value = json_patch_get_value(op);
      if (!value) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
              .message = "Test operation missing 'value' field"};
        }
        gtext_json_free(clone);
        return GTEXT_JSON_E_INVALID;
      }
      status = json_patch_test(clone, path_str, path_len, value, err);
      if (status != GTEXT_JSON_OK) {
        // Operation failed - free clone and return error (atomicity)
        gtext_json_free(clone);
        return status;
      }
    }
    else {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
            .message = "Unknown patch operation type"};
      }
      gtext_json_free(clone);
      return GTEXT_JSON_E_INVALID;
    }
  }

  // All operations succeeded - copy clone's content back to original
  // This preserves the original's context but replaces its content
  /* As in gtext_json_merge_patch(): the original's grafted subtrees are about
     to become unreachable, and nothing else frees them. */
  json_free_foreign_contexts(root);
  GTEXT_JSON_Status status = json_value_copy_content(root, clone);
  if (status != GTEXT_JSON_OK) {
    // Copy failed - free clone and return error
    gtext_json_free(clone);
    if (err) {
      err->code = status;
      err->message = "Failed to copy patch results back to original";
      err->offset = 0;
      err->line = 0;
      err->col = 0;
    }
    return status;
  }

  // Free the clone (all its content has been copied to original)
  gtext_json_free(clone);

  return GTEXT_JSON_OK;
}

// Recursive helper function for JSON Merge Patch
// Implements the MergePatch(Target, Patch) algorithm from RFC 7386
/* Pairs of (target, patch) still to be merged.
 *
 * json_merge_patch_one() below used to call itself for a member whose target
 * and patch are both objects, which made RFC 7386's algorithm a recursion over
 * the patch - and a patch built through the DOM API has no depth limit, since
 * max_depth is a *parser* option.  Twenty thousand nested objects on a 512 KB
 * stack took the process down.
 *
 * The order the pairs come off does not matter: each one is a distinct target
 * node and merge-patch is per-member, so no member's result depends on when
 * another was done. */
typedef struct {
  GTEXT_JSON_Value * target;
  const GTEXT_JSON_Value * patch;
} json_mp_pair;

typedef struct {
  json_mp_pair * items;
  size_t count;
  size_t capacity;
  json_mp_pair inline_items[32];
} json_mp_work;

static void json_mp_work_init(json_mp_work * w) {
  w->items = w->inline_items;
  w->count = 0;
  w->capacity = sizeof(w->inline_items) / sizeof(w->inline_items[0]);
}

static void json_mp_work_free(json_mp_work * w) {
  if (w->items != w->inline_items) {
    gtext_allocator_free(NULL, w->items);
  }
}

static bool json_mp_work_push(
    json_mp_work * w, GTEXT_JSON_Value * target, const GTEXT_JSON_Value * patch) {
  if (w->count == w->capacity) {
    size_t new_capacity = w->capacity * 2;
    json_mp_pair * grown;
    if (w->items == w->inline_items) {
      grown = (json_mp_pair *)gtext_allocator_malloc(
          NULL, new_capacity * sizeof(*grown));
      if (grown) {
        memcpy(grown, w->items, w->count * sizeof(*grown));
      }
    }
    else {
      grown = (json_mp_pair *)gtext_allocator_realloc(
          NULL, w->items, new_capacity * sizeof(*grown));
    }
    if (!grown) {
      return false;
    }
    w->items = grown;
    w->capacity = new_capacity;
  }
  w->items[w->count].target = target;
  w->items[w->count].patch = patch;
  w->count++;
  return true;
}

/* One level of RFC 7386's MergePatch: everything but the members whose target
   and patch are both objects, which go on @p work for the driver below. */
static GTEXT_JSON_Status json_merge_patch_one(GTEXT_JSON_Value * target,
    const GTEXT_JSON_Value * patch, GTEXT_JSON_Error * err,
    json_mp_work * work) {
  if (!target || !patch) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Invalid arguments: target and patch must not be NULL"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  // If patch is not an object, replace target entirely
  if (patch->type != GTEXT_JSON_OBJECT) {
    // Clone patch content directly into target
    json_context * target_ctx = target->ctx;
    if (!target_ctx) {
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
            .message = "Target value has no context"};
      }
      return GTEXT_JSON_E_INVALID;
    }

    // Change target's type first
    target->type = patch->type;

    // Clone content based on type (similar to json_value_clone but into
    // existing target)
    switch (patch->type) {
    case GTEXT_JSON_NULL:
      // Nothing to copy
      break;

    case GTEXT_JSON_BOOL:
      target->as.boolean = patch->as.boolean;
      break;

    case GTEXT_JSON_STRING: {
      // Allocate and copy string data
      // Check for integer overflow in len + 1
      if (patch->as.string.len > SIZE_MAX - 1) {
        if (err) {
          *err = (GTEXT_JSON_Error){
              .code = GTEXT_JSON_E_OOM, .message = "String length overflow"};
        }
        return GTEXT_JSON_E_OOM;
      }
      char * str_data = (char *)json_arena_alloc_for_context(
          target_ctx, patch->as.string.len + 1, 1);
      if (!str_data) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
              .message = "Out of memory cloning string"};
        }
        return GTEXT_JSON_E_OOM;
      }
      memcpy(str_data, patch->as.string.data, patch->as.string.len);
      str_data[patch->as.string.len] = '\0';
      target->as.string.data = str_data;
      target->as.string.len = patch->as.string.len;
      break;
    }

    case GTEXT_JSON_NUMBER: {
      // Allocate and copy lexeme
      // Check for integer overflow in lexeme_len + 1
      if (patch->as.number.lexeme_len > SIZE_MAX - 1) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
              .message = "Number lexeme length overflow"};
        }
        return GTEXT_JSON_E_OOM;
      }
      char * lexeme = (char *)json_arena_alloc_for_context(
          target_ctx, patch->as.number.lexeme_len + 1, 1);
      if (!lexeme) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
              .message = "Out of memory cloning number lexeme"};
        }
        return GTEXT_JSON_E_OOM;
      }
      memcpy(lexeme, patch->as.number.lexeme, patch->as.number.lexeme_len);
      lexeme[patch->as.number.lexeme_len] = '\0';
      target->as.number.lexeme = lexeme;
      target->as.number.lexeme_len = patch->as.number.lexeme_len;
      target->as.number.i64 = patch->as.number.i64;
      target->as.number.u64 = patch->as.number.u64;
      target->as.number.dbl = patch->as.number.dbl;
      target->as.number.has_i64 = patch->as.number.has_i64;
      target->as.number.has_u64 = patch->as.number.has_u64;
      target->as.number.has_dbl = patch->as.number.has_dbl;
      break;
    }

    case GTEXT_JSON_ARRAY: {
      // Initialize array
      target->as.array.count = 0;
      target->as.array.capacity = patch->as.array.count;
      if (target->as.array.capacity > 0) {
        size_t elem_size = sizeof(GTEXT_JSON_Value *);
        if (target->as.array.capacity > SIZE_MAX / elem_size) {
          if (err) {
            *err = (GTEXT_JSON_Error){
                .code = GTEXT_JSON_E_OOM, .message = "Array capacity overflow"};
          }
          return GTEXT_JSON_E_OOM;
        }
        target->as.array.elems =
            (GTEXT_JSON_Value **)json_arena_alloc_for_context(target_ctx,
                target->as.array.capacity * elem_size,
                sizeof(GTEXT_JSON_Value *));
        if (!target->as.array.elems) {
          if (err) {
            *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                .message = "Out of memory cloning array"};
          }
          return GTEXT_JSON_E_OOM;
        }
      }
      else {
        target->as.array.elems = NULL;
      }

      // Clone each element
      for (size_t i = 0; i < patch->as.array.count; i++) {
        GTEXT_JSON_Value * cloned_elem =
            json_value_clone(patch->as.array.elems[i], target_ctx);
        if (!cloned_elem) {
          if (err) {
            *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                .message = "Out of memory cloning array element"};
          }
          return GTEXT_JSON_E_OOM;
        }
        target->as.array.elems[i] = cloned_elem;
        target->as.array.count++;
      }
      break;
    }

    case GTEXT_JSON_OBJECT: {
      // This case shouldn't happen (we check patch->type != GTEXT_JSON_OBJECT
      // above) But handle it defensively
      if (err) {
        *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
            .message = "Internal error: object patch in non-object branch"};
      }
      return GTEXT_JSON_E_INVALID;
    }
    }

    return GTEXT_JSON_OK;
  }

  // Patch is an object - merge recursively
  // If target is not an object, convert it to an empty object first
  // According to RFC 7386: "if Target is not an Object, Target = {}"
  if (target->type != GTEXT_JSON_OBJECT) {
    // Convert target to empty object
    // The old content will remain in memory but won't be accessible
    // since we're changing the type. It will be freed when the context is
    // freed.
    target->type = GTEXT_JSON_OBJECT;
    target->as.object.count = 0;
    target->as.object.capacity = 0;
    target->as.object.pairs = NULL;
  }

  // Now target is guaranteed to be an object
  // Verify target is actually an object (defensive check)
  if (target->type != GTEXT_JSON_OBJECT) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Internal error: target type mismatch in merge"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  // Process each member in the patch
  // Defensive check: pairs should not be NULL when count > 0
  if (patch->as.object.count > 0 && !patch->as.object.pairs) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Invalid patch object: count > 0 but pairs is NULL"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  for (size_t i = 0; i < patch->as.object.count; i++) {
    const char * key = patch->as.object.pairs[i].key;
    size_t key_len = patch->as.object.pairs[i].key_len;
    const GTEXT_JSON_Value * patch_value = patch->as.object.pairs[i].value;

    if (!patch_value) {
      // Skip null values (shouldn't happen, but be defensive)
      continue;
    }

    // If patch value is null, remove the key from target
    if (patch_value->type == GTEXT_JSON_NULL) {
      // Remove key from target (if it exists)
      // Note: gtext_json_object_remove returns error if key doesn't exist,
      // but according to RFC 7386, removal is idempotent, so we ignore the
      // error We don't check the return value because removal is idempotent per
      // RFC 7386 Only attempt removal if target is actually an object
      // (defensive check)
      if (target->type == GTEXT_JSON_OBJECT) {
        (void)gtext_json_object_remove(target, key, key_len);
      }
      // Continue to next key (removal is idempotent)
      continue;
    }

    // Patch value is non-null - recursively merge
    // Find the key in target's pairs array to get mutable access
    // We need to re-find the key after each operation because the pairs array
    // might be reallocated during recursive operations
    GTEXT_JSON_Value * target_value_mut = NULL;

    // Search for the key - we need to do this each time because the pairs array
    // might have been reallocated in a previous iteration
    // Defensive check: pairs should not be NULL when count > 0
    if (target->as.object.count > 0 && target->as.object.pairs) {
      for (size_t j = 0; j < target->as.object.count; j++) {
        // Defensive check: key should not be NULL when key_len > 0
        if (target->as.object.pairs[j].key_len == key_len) {
          if (key_len == 0 ||
              (target->as.object.pairs[j].key && key &&
                  memcmp(target->as.object.pairs[j].key, key, key_len) == 0)) {
            target_value_mut = target->as.object.pairs[j].value;
            break;
          }
        }
      }
    }

    if (target_value_mut) {
      // Key exists - recursively merge patch_value into target_value
      // Important: target_value_mut points to pairs[j].value
      // Even if the pairs array shifts during recursive operations, this
      // pointer should remain valid as long as we don't free the value itself
      if (!json_mp_work_push(work, target_value_mut, patch_value)) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
              .message = "Out of memory merging patch"};
        }
        return GTEXT_JSON_E_OOM;
      }
    }
    else {
      // Key doesn't exist.
      json_context * target_ctx = target->ctx;
      GTEXT_JSON_Value * cloned_value = NULL;

      if (patch_value->type == GTEXT_JSON_OBJECT) {
        // RFC 7386 section 2 defines the absent case as MergePatch with a
        // target that "is not an Object", which the algorithm replaces with
        // an empty Object before walking the patch's members.  Walking them
        // is what discards the nulls: a null member removes a name from the
        // target, and an empty target has no such name, so the member
        // contributes nothing rather than being stored.
        //
        // Cloning the patch subtree verbatim, which is what this did, keeps
        // those nulls.  The last row of the specification's test table is
        // this case: {} merged with {"a":{"bb":{"ccc":null}}} is
        // {"a":{"bb":{}}}, not the patch itself.
        cloned_value =
            json_value_new_with_existing_context(GTEXT_JSON_OBJECT, target_ctx);
        if (cloned_value
            && !json_mp_work_push(work, cloned_value, patch_value)) {
          if (err) {
            *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
                .message = "Out of memory merging patch"};
          }
          return GTEXT_JSON_E_OOM;
        }
      }
      else {
        cloned_value = json_value_clone(patch_value, target_ctx);
      }

      if (!cloned_value) {
        if (err) {
          *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
              .message = "Out of memory cloning patch value"};
        }
        return GTEXT_JSON_E_OOM;
      }

      // Add to target
      GTEXT_JSON_Status status =
          gtext_json_object_put(target, key, key_len, cloned_value);
      if (status != GTEXT_JSON_OK) {
        gtext_json_free(cloned_value);
        if (err) {
          err->code = status;
          err->message = "Failed to add key to target object";
          err->offset = 0;
          err->line = 0;
          err->col = 0;
        }
        return status;
      }
    }
  }

  return GTEXT_JSON_OK;
}

/* The driver: one level at a time, with the pairs still to do on the heap. */
static GTEXT_JSON_Status json_merge_patch_recursive(GTEXT_JSON_Value * target,
    const GTEXT_JSON_Value * patch, GTEXT_JSON_Error * err) {
  json_mp_work work;
  json_mp_work_init(&work);
  if (!json_mp_work_push(&work, target, patch)) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
          .message = "Out of memory merging patch"};
    }
    json_mp_work_free(&work);
    return GTEXT_JSON_E_OOM;
  }

  GTEXT_JSON_Status status = GTEXT_JSON_OK;
  while (work.count > 0) {
    json_mp_pair pair = work.items[--work.count];
    status = json_merge_patch_one(pair.target, pair.patch, err, &work);
    if (status != GTEXT_JSON_OK) {
      break;
    }
  }
  json_mp_work_free(&work);
  return status;
}

// Main JSON Merge Patch function
// Implements true atomicity: applies merge to a clone,
// then replaces the original only if all operations succeed
GTEXT_API GTEXT_JSON_Status gtext_json_merge_patch(GTEXT_JSON_Value * target,
    const GTEXT_JSON_Value * patch, GTEXT_JSON_Error * err) {
  if (!target || !patch) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_INVALID,
          .message = "Invalid arguments: target and patch must not be NULL"};
    }
    return GTEXT_JSON_E_INVALID;
  }

  // For atomicity: clone the target, apply merge to the clone, then copy the
  // clone's content back to the original only if all succeed.  The allocator
  // comes from the target, for the reason given in gtext_json_patch_apply().
  json_context * clone_ctx =
      json_context_new(target->ctx ? target->ctx->alloc : NULL);
  if (!clone_ctx) {
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
          .message = "Out of memory creating clone for atomic merge patch"};
    }
    return GTEXT_JSON_E_OOM;
  }

  GTEXT_JSON_Value * clone = json_value_clone(target, clone_ctx);
  if (!clone) {
    json_context_free(clone_ctx);
    if (err) {
      *err = (GTEXT_JSON_Error){.code = GTEXT_JSON_E_OOM,
          .message = "Out of memory cloning target for atomic merge patch"};
    }
    return GTEXT_JSON_E_OOM;
  }

  // Apply merge to clone
  GTEXT_JSON_Status status = json_merge_patch_recursive(clone, patch, err);
  if (status != GTEXT_JSON_OK) {
    // Merge failed - free clone and return error (atomicity)
    gtext_json_free(clone);
    return status;
  }

  /* All operations succeeded - copy the clone's content back to the original.
     The original's context is preserved and its content replaced, so whatever
     the original was holding from *other* arenas has to be released first:
     nothing else ever will, and json_value_copy_content() overwrites the
     pointers that were the only way to reach it.  A target assembled by storing
     separately created values - which is every DOM-built target - leaked all of
     it on every merge, which a test of an 8000-deep object found at 1.3 GB and
     a two-level one shows just as well. */
  json_free_foreign_contexts(target);
  status = json_value_copy_content(target, clone);
  if (status != GTEXT_JSON_OK) {
    // Copy failed - free clone and return error
    gtext_json_free(clone);
    if (err) {
      err->code = status;
      err->message = "Failed to copy merge patch results back to original";
      err->offset = 0;
      err->line = 0;
      err->col = 0;
    }
    return status;
  }

  // Free the clone (all its content has been copied to original)
  gtext_json_free(clone);

  return GTEXT_JSON_OK;
}
