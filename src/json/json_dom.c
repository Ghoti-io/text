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
 * DOM value structure and memory management for JSON module.
 */

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/allocator.h>
#include <ghoti.io/text/macros.h>
#include "json_internal.h"
#include "../text_number_internal.h"

#include <ghoti.io/text/json/json_core.h>
#include <ghoti.io/text/json/json_dom.h>
// Arena allocator implementation
// Uses a simple linked list of blocks for efficient bulk allocation

// Default block size (64KB)
#define JSON_ARENA_DEFAULT_BLOCK_SIZE (64 * 1024)

// Create a new arena allocator
// initial_block_size: Initial block size (0 = use default)
// Returns: Pointer to arena, or NULL on failure
static json_arena * json_arena_new(
    size_t initial_block_size, const GTEXT_Allocator * alloc) {
  json_arena * arena = gtext_allocator_malloc(alloc, sizeof(json_arena));
  if (!arena) {
    return NULL;
  }

  arena->alloc = alloc;

  arena->block_size = initial_block_size > 0 ? initial_block_size
                                             : JSON_ARENA_DEFAULT_BLOCK_SIZE;
  arena->first = NULL;
  arena->current = NULL;

  return arena;
}

// Allocate memory from the arena
// arena: Arena to allocate from
// size: Size in bytes to allocate
// align: Alignment requirement (must be power of 2, not 0)
// Returns: Pointer to allocated memory, or NULL on failure
static void * json_arena_alloc(json_arena * arena, size_t size, size_t align) {
  if (!arena || size == 0) {
    return NULL;
  }

  // Validate alignment: must be power of 2 and not 0
  // Check if align is 0 or not a power of 2
  if (align == 0 || (align & (align - 1)) != 0) {
    return NULL; // Invalid alignment
  }

  // Calculate alignment mask
  size_t align_mask = align - 1;

  // If we have a current block, try to allocate from it
  if (arena->current) {
    size_t offset = arena->current->used;
    size_t aligned_offset = (offset + align_mask) & ~align_mask;

    if (aligned_offset + size <= arena->current->size) {
      // Can fit in current block
      arena->current->used = aligned_offset + size;
      return arena->current->data + aligned_offset;
    }
  }

  // Need a new block
  // Block size should be at least size + alignment overhead
  size_t block_size = arena->block_size;
  // Check for overflow in size + align
  if (size > SIZE_MAX - align) {
    return NULL; // Overflow
  }
  if (size + align > block_size) {
    block_size = size + align;
  }

  // Check for overflow in sizeof + block_size
  size_t block_alloc_size = sizeof(json_arena_block) + block_size;
  if (block_alloc_size < block_size) { // Overflow check
    return NULL;
  }
  json_arena_block * block =
      gtext_allocator_malloc(arena->alloc, block_alloc_size);
  if (!block) {
    return NULL;
  }

  block->next = NULL;
  block->size = block_size;

  // Align the data pointer
  size_t offset = 0;
  size_t aligned_offset = (offset + align_mask) & ~align_mask;
  block->used = aligned_offset + size;

  // Link into arena
  if (arena->first == NULL) {
    arena->first = block;
  }
  else {
    arena->current->next = block;
  }
  arena->current = block;

  return block->data + aligned_offset;
}

// Free all memory in the arena
// arena: Arena to free (can be NULL)
static void json_arena_free(json_arena * arena) {
  if (!arena) {
    return;
  }

  /* Captured before the arena itself is released, since the allocator
   * pointer lives inside it. */
  const GTEXT_Allocator * alloc = arena->alloc;

  json_arena_block * block = arena->first;
  while (block) {
    json_arena_block * next = block->next;
    gtext_allocator_free(alloc, block);
    block = next;
  }

  gtext_allocator_free(alloc, arena);
}


// Create a new context with an arena
// Allocates a context and arena for a new DOM tree.
// The context is allocated with malloc (not in the arena) so it can
// be accessed to free the arena.
// Returns: New context, or NULL on failure
json_context * json_context_new(const GTEXT_Allocator * alloc) {
  /* Resolved once here so that every later use - including the frees, which
   * happen after the options struct may be gone - reads a stable pointer. */
  if (!alloc) {
    alloc = gtext_allocator_default();
  }

  json_context * ctx = gtext_allocator_malloc(alloc, sizeof(json_context));
  if (!ctx) {
    return NULL;
  }

  ctx->alloc = alloc;
  ctx->arena = json_arena_new(0, alloc); // Use default block size
  if (!ctx->arena) {
    gtext_allocator_free(alloc, ctx);
    return NULL;
  }

  ctx->input_buffer = NULL;
  ctx->input_buffer_len = 0;

  return ctx;
}

// Set input buffer for in-situ mode
// This stores a reference to the input buffer in the context.
// The buffer is caller-owned and must remain valid for the lifetime of the DOM.
// ctx: Context to set input buffer on (must not be NULL)
// input_buffer: Original input buffer (caller-owned, must remain valid)
// input_buffer_len: Length of input buffer
void json_context_set_input_buffer(
    json_context * ctx, const char * input_buffer, size_t input_buffer_len) {
  if (!ctx) {
    return;
  }
  ctx->input_buffer = input_buffer;
  ctx->input_buffer_len = input_buffer_len;
}

// Free a context and its arena
// ctx: Context to free (can be NULL)
// Note: The input buffer (if set) is caller-owned and is NOT freed here.
void json_context_free(json_context * ctx) {
  if (!ctx) {
    return;
  }

  const GTEXT_Allocator * alloc = ctx->alloc;
  json_arena_free(ctx->arena);
  gtext_allocator_free(alloc, ctx);
}

// Recursively free child values that have different contexts
/* One node whose children are still being scanned, and whether its own context
   has to be freed once they have been.  @ctx_to_free is held separately because
   the node's memory *is* that context's arena, so it cannot be read after. */
typedef struct {
  GTEXT_JSON_Value * node;
  size_t i;
  json_context * ctx_to_free;
} json_free_frame;

/* The recursion this replaces, kept for one case: a stack that will not grow.
   Freeing cannot report a failure, so the alternatives are to leak the foreign
   contexts or to do what the code did before, and doing what it did before is
   the better of the two - it is only reachable once the allocator is already
   refusing, and it is what every caller survived until now. */
static void json_free_children_fallback(GTEXT_JSON_Value * v) {
  if (!v) {
    return;
  }
  json_context * parent_ctx = v->ctx;
  size_t count = 0;
  if (v->type == GTEXT_JSON_ARRAY && v->as.array.elems) {
    count = v->as.array.count;
  }
  else if (v->type == GTEXT_JSON_OBJECT && v->as.object.pairs) {
    count = v->as.object.count;
  }
  for (size_t i = 0; i < count; i++) {
    GTEXT_JSON_Value * child = (v->type == GTEXT_JSON_ARRAY)
        ? v->as.array.elems[i]
        : v->as.object.pairs[i].value;
    if (!child) {
      continue;
    }
    json_context * child_ctx =
        (child->ctx && child->ctx != parent_ctx) ? child->ctx : NULL;
    json_free_children_fallback(child);
    if (child_ctx) {
      json_context_free(child_ctx);
    }
  }
}

/* How many children @p v has for the purposes of this scan. */
static size_t json_free_child_count(const GTEXT_JSON_Value * v) {
  if (v->type == GTEXT_JSON_ARRAY && v->as.array.elems) {
    return v->as.array.count;
  }
  if (v->type == GTEXT_JSON_OBJECT && v->as.object.pairs) {
    return v->as.object.count;
  }
  return 0;
}

static GTEXT_JSON_Value * json_free_child_at(
    const GTEXT_JSON_Value * v, size_t i) {
  if (v->type == GTEXT_JSON_ARRAY) {
    return v->as.array.elems[i];
  }
  return v->as.object.pairs[i].value;
}

/* Find every descendant whose context differs from its parent's and free that
 * context, deepest first.
 *
 * This is not about reclaiming the nodes: a value's nodes live in its
 * context's arena and gtext_json_free() frees that arena in one go.  It exists
 * because a subtree grafted in from *another* arena - which is what every
 * gtext_json_array_push() of a separately created value does - owns an arena of
 * its own that nothing else will free.  So the walk visits every node, not only
 * the containers, because a scalar can have been grafted too.
 *
 * It was a recursion, and a value built through the DOM API has no depth limit
 * to bound it - max_depth is a *parser* option, and json_core.h is careful to
 * say so.  Two hundred thousand levels, assembled by the documented API with
 * nothing unusual asked for, could not be freed: the process died trying.
 *
 * Deepest first matters and is what the stack gives: a context is freed when
 * its frame pops, which is after everything below it has been scanned. */
GTEXT_INTERNAL_API void json_free_foreign_contexts(GTEXT_JSON_Value * v) {
  if (!v) {
    return;
  }

  /* Deep enough that nothing a parser could have produced ever allocates:
     json_core.h puts the recursive parser's own ceiling at about 18000 levels
     on an 8 MiB stack, and its default limit at 256. */
  json_free_frame inline_frames[64];
  json_free_frame * frames = inline_frames;
  size_t count = 0;
  size_t capacity = sizeof(inline_frames) / sizeof(inline_frames[0]);

  frames[0].node = v;
  frames[0].i = 0;
  frames[0].ctx_to_free = NULL;
  count = 1;

  while (count > 0) {
    json_free_frame * f = &frames[count - 1];
    const size_t children = json_free_child_count(f->node);

    if (f->i >= children) {
      json_context * doomed = f->ctx_to_free;
      count--;
      if (doomed) {
        json_context_free(doomed);
      }
      continue;
    }

    GTEXT_JSON_Value * child = json_free_child_at(f->node, f->i);
    json_context * parent_ctx = f->node->ctx;
    f->i++;
    if (!child) {
      continue;
    }

    if (count == capacity) {
      size_t new_capacity = capacity * 2;
      json_free_frame * grown;
      if (frames == inline_frames) {
        grown = (json_free_frame *)gtext_allocator_malloc(
            NULL, new_capacity * sizeof(*grown));
        if (grown) {
          memcpy(grown, frames, count * sizeof(*grown));
        }
      }
      else {
        grown = (json_free_frame *)gtext_allocator_realloc(
            NULL, frames, new_capacity * sizeof(*grown));
      }
      if (!grown) {
        /* See json_free_children_fallback(). */
        json_context * child_ctx =
            (child->ctx && child->ctx != parent_ctx) ? child->ctx : NULL;
        json_free_children_fallback(child);
        if (child_ctx) {
          json_context_free(child_ctx);
        }
        continue;
      }
      frames = grown;
      capacity = new_capacity;
    }

    frames[count].node = child;
    frames[count].i = 0;
    frames[count].ctx_to_free =
        (child->ctx && child->ctx != parent_ctx) ? child->ctx : NULL;
    count++;
  }

  if (frames != inline_frames) {
    gtext_allocator_free(NULL, frames);
  }
}

GTEXT_API void gtext_json_free(GTEXT_JSON_Value * v) {
  if (!v || !v->ctx) {
    return;
  }

  // First, recursively free any children that have different contexts
  json_free_foreign_contexts(v);

  // Then free this value's context, which frees the arena and all memory
  json_context * ctx = v->ctx;
  json_context_free(ctx);
}

// Alignment for GTEXT_JSON_Value (align to pointer size, typically 8 bytes)
#define JSON_VALUE_ALIGN 8

// Helper to create a value with a new context
static GTEXT_JSON_Value * json_value_new_with_context(
    GTEXT_JSON_Type type, json_context * ctx) {
  if (!ctx) {
    return NULL;
  }

  GTEXT_JSON_Value * val = (GTEXT_JSON_Value *)json_arena_alloc(
      ctx->arena, sizeof(GTEXT_JSON_Value), JSON_VALUE_ALIGN);
  if (!val) {
    return NULL;
  }

  val->type = type;
  val->ctx = ctx;
  val->contained = false;
  memset(&val->as, 0, sizeof(val->as));

  return val;
}

// Internal helper to create a value using an existing context (for parser)
// This allows all values in a parse tree to share the same context
GTEXT_JSON_Value * json_value_new_with_existing_context(
    GTEXT_JSON_Type type, json_context * ctx) {
  return json_value_new_with_context(type, ctx);
}

// Internal helper to allocate from an arena (for parser)
void * json_arena_alloc_for_context(
    json_context * ctx, size_t size, size_t align) {
  if (!ctx || !ctx->arena) {
    return NULL;
  }
  return json_arena_alloc(ctx->arena, size, align);
}

GTEXT_API GTEXT_JSON_Value * gtext_json_new_null(void) {
  json_context * ctx = json_context_new(NULL);
  if (!ctx) {
    return NULL;
  }

  GTEXT_JSON_Value * val = json_value_new_with_context(GTEXT_JSON_NULL, ctx);
  if (!val) {
    json_context_free(ctx);
    return NULL;
  }

  return val;
}

GTEXT_API GTEXT_JSON_Value * gtext_json_new_bool(bool b) {
  json_context * ctx = json_context_new(NULL);
  if (!ctx) {
    return NULL;
  }

  GTEXT_JSON_Value * val = json_value_new_with_context(GTEXT_JSON_BOOL, ctx);
  if (!val) {
    json_context_free(ctx);
    return NULL;
  }

  val->as.boolean = b ? 1 : 0;
  return val;
}

GTEXT_INTERNAL_API GTEXT_JSON_Value * json_value_new_string_on(
    const char * s, size_t len, const GTEXT_Allocator * alloc) {
  // Allow NULL pointer only if len is 0 (empty string)
  if (!s && len > 0) {
    return NULL;
  }

  json_context * ctx = json_context_new(alloc);
  if (!ctx) {
    return NULL;
  }

  GTEXT_JSON_Value * val = json_value_new_with_context(GTEXT_JSON_STRING, ctx);
  if (!val) {
    json_context_free(ctx);
    return NULL;
  }

  // Allocate string data (len + 1 for null terminator)
  if (len > SIZE_MAX - 1) {
    json_context_free(ctx);
    return NULL;
  }
  char * str_data = (char *)json_arena_alloc(ctx->arena, len + 1, 1);
  if (!str_data) {
    json_context_free(ctx);
    return NULL;
  }

  // Copy string data (safe even if s is NULL and len is 0)
  if (len > 0 && s != NULL) {
    memcpy(str_data, s, len);
  }
  str_data[len] = '\0'; // Null terminator

  val->as.string.data = str_data;
  val->as.string.len = len;
  return val;
}

GTEXT_API GTEXT_JSON_Value * gtext_json_new_string(const char * s, size_t len) {
  /*
   * The C library, because a public builder has no options and so no caller
   * allocator to inherit - which is the right answer here and the wrong one
   * inside the library, where there usually is one.  JSON Schema builds a
   * throwaway string value for every key `propertyNames` checks, and it has
   * the compiling schema's allocator in hand; it calls
   * json_value_new_string_on() with it rather than this.
   */
  return json_value_new_string_on(s, len, NULL);
}

GTEXT_API GTEXT_JSON_Value * gtext_json_new_number_from_lexeme(
    const char * s, size_t len) {
  if (!s || len == 0) {
    return NULL;
  }

  json_context * ctx = json_context_new(NULL);
  if (!ctx) {
    return NULL;
  }

  GTEXT_JSON_Value * val = json_value_new_with_context(GTEXT_JSON_NUMBER, ctx);
  if (!val) {
    json_context_free(ctx);
    return NULL;
  }

  // Allocate lexeme
  char * lexeme = (char *)json_arena_alloc(ctx->arena, len + 1, 1);
  if (!lexeme) {
    json_context_free(ctx);
    return NULL;
  }

  memcpy(lexeme, s, len);
  lexeme[len] = '\0';

  val->as.number.lexeme = lexeme;
  val->as.number.lexeme_len = len;
  val->as.number.has_i64 = 0;
  val->as.number.has_u64 = 0;
  val->as.number.has_dbl = 0;
  return val;
}

GTEXT_API GTEXT_JSON_Value * gtext_json_new_number_i64(int64_t x) {
  json_context * ctx = json_context_new(NULL);
  if (!ctx) {
    return NULL;
  }

  GTEXT_JSON_Value * val = json_value_new_with_context(GTEXT_JSON_NUMBER, ctx);
  if (!val) {
    json_context_free(ctx);
    return NULL;
  }

  // Generate lexeme from int64
  char lexeme_buf[32]; // Enough for any int64
  int snprintf_result =
      snprintf(lexeme_buf, sizeof(lexeme_buf), "%lld", (long long)x);
  if (snprintf_result < 0 || (size_t)snprintf_result >= sizeof(lexeme_buf)) {
    json_context_free(ctx);
    return NULL;
  }
  size_t lexeme_len = (size_t)snprintf_result;

  // Allocate lexeme in arena
  char * lexeme = (char *)json_arena_alloc(ctx->arena, lexeme_len + 1, 1);
  if (!lexeme) {
    json_context_free(ctx);
    return NULL;
  }
  memcpy(lexeme, lexeme_buf, lexeme_len + 1);

  val->as.number.lexeme = lexeme;
  val->as.number.lexeme_len = lexeme_len;
  val->as.number.i64 = x;
  val->as.number.has_i64 = 1;
  val->as.number.has_u64 = 0;
  val->as.number.has_dbl = 0;
  return val;
}

GTEXT_API GTEXT_JSON_Value * gtext_json_new_number_u64(uint64_t x) {
  json_context * ctx = json_context_new(NULL);
  if (!ctx) {
    return NULL;
  }

  GTEXT_JSON_Value * val = json_value_new_with_context(GTEXT_JSON_NUMBER, ctx);
  if (!val) {
    json_context_free(ctx);
    return NULL;
  }

  // Generate lexeme from uint64
  char lexeme_buf[32]; // Enough for any uint64
  int snprintf_result =
      snprintf(lexeme_buf, sizeof(lexeme_buf), "%llu", (unsigned long long)x);
  if (snprintf_result < 0 || (size_t)snprintf_result >= sizeof(lexeme_buf)) {
    json_context_free(ctx);
    return NULL;
  }
  size_t lexeme_len = (size_t)snprintf_result;

  // Allocate lexeme in arena
  char * lexeme = (char *)json_arena_alloc(ctx->arena, lexeme_len + 1, 1);
  if (!lexeme) {
    json_context_free(ctx);
    return NULL;
  }
  memcpy(lexeme, lexeme_buf, lexeme_len + 1);

  val->as.number.lexeme = lexeme;
  val->as.number.lexeme_len = lexeme_len;
  val->as.number.u64 = x;
  val->as.number.has_i64 = 0;
  val->as.number.has_u64 = 1;
  val->as.number.has_dbl = 0;
  return val;
}

GTEXT_API GTEXT_JSON_Value * gtext_json_new_number_double(double x) {
  json_context * ctx = json_context_new(NULL);
  if (!ctx) {
    return NULL;
  }

  GTEXT_JSON_Value * val = json_value_new_with_context(GTEXT_JSON_NUMBER, ctx);
  if (!val) {
    json_context_free(ctx);
    return NULL;
  }

  // Generate lexeme from double
  // Use %g for compact representation, but %f might be better for round-trip
  // For now, use %g and let the user specify precision if needed
  char lexeme_buf[64]; // Enough for any double
  /* Not snprintf: it writes LC_NUMERIC's separator, and a lexeme of "0,1"
     is not a JSON number at all. */
  int snprintf_result = gtext_number_format_double(
      lexeme_buf, sizeof(lexeme_buf), x, GTEXT_NUMBER_GENERAL, 17);
  if (snprintf_result < 0 || (size_t)snprintf_result >= sizeof(lexeme_buf)) {
    json_context_free(ctx);
    return NULL;
  }
  size_t lexeme_len = (size_t)snprintf_result;

  // Allocate lexeme in arena
  char * lexeme = (char *)json_arena_alloc(ctx->arena, lexeme_len + 1, 1);
  if (!lexeme) {
    json_context_free(ctx);
    return NULL;
  }
  memcpy(lexeme, lexeme_buf, lexeme_len + 1);

  val->as.number.lexeme = lexeme;
  val->as.number.lexeme_len = lexeme_len;
  val->as.number.dbl = x;
  val->as.number.has_i64 = 0;
  val->as.number.has_u64 = 0;
  val->as.number.has_dbl = 1;
  return val;
}

GTEXT_API GTEXT_JSON_Value * gtext_json_new_array(void) {
  json_context * ctx = json_context_new(NULL);
  if (!ctx) {
    return NULL;
  }

  GTEXT_JSON_Value * val = json_value_new_with_context(GTEXT_JSON_ARRAY, ctx);
  if (!val) {
    json_context_free(ctx);
    return NULL;
  }

  // Initialize empty array
  val->as.array.elems = NULL;
  val->as.array.count = 0;
  val->as.array.capacity = 0;
  return val;
}

GTEXT_API GTEXT_JSON_Value * gtext_json_new_object(void) {
  json_context * ctx = json_context_new(NULL);
  if (!ctx) {
    return NULL;
  }

  GTEXT_JSON_Value * val = json_value_new_with_context(GTEXT_JSON_OBJECT, ctx);
  if (!val) {
    json_context_free(ctx);
    return NULL;
  }

  // Initialize empty object
  val->as.object.pairs = NULL;
  val->as.object.count = 0;
  val->as.object.capacity = 0;
  return val;
}

// Accessor functions

GTEXT_API GTEXT_JSON_Type gtext_json_typeof(const GTEXT_JSON_Value * v) {
  if (!v) {
    return GTEXT_JSON_NULL;
  }
  return v->type;
}

GTEXT_API GTEXT_JSON_Status gtext_json_get_bool(
    const GTEXT_JSON_Value * v, bool * out) {
  if (!v || !out) {
    return GTEXT_JSON_E_INVALID;
  }
  if (v->type != GTEXT_JSON_BOOL) {
    return GTEXT_JSON_E_INVALID;
  }
  *out = v->as.boolean ? true : false;
  return GTEXT_JSON_OK;
}

GTEXT_API GTEXT_JSON_Status gtext_json_get_string(
    const GTEXT_JSON_Value * v, const char ** out, size_t * out_len) {
  if (!v || !out || !out_len) {
    return GTEXT_JSON_E_INVALID;
  }
  if (v->type != GTEXT_JSON_STRING) {
    return GTEXT_JSON_E_INVALID;
  }
  *out = v->as.string.data;
  *out_len = v->as.string.len;
  return GTEXT_JSON_OK;
}

GTEXT_API GTEXT_JSON_Status gtext_json_get_number_lexeme(
    const GTEXT_JSON_Value * v, const char ** out, size_t * out_len) {
  if (!v || !out || !out_len) {
    return GTEXT_JSON_E_INVALID;
  }
  if (v->type != GTEXT_JSON_NUMBER) {
    return GTEXT_JSON_E_INVALID;
  }
  if (!v->as.number.lexeme) {
    return GTEXT_JSON_E_INVALID;
  }
  *out = v->as.number.lexeme;
  *out_len = v->as.number.lexeme_len;
  return GTEXT_JSON_OK;
}

GTEXT_API GTEXT_JSON_Status gtext_json_get_i64(
    const GTEXT_JSON_Value * v, int64_t * out) {
  if (!v || !out) {
    return GTEXT_JSON_E_INVALID;
  }
  if (v->type != GTEXT_JSON_NUMBER) {
    return GTEXT_JSON_E_INVALID;
  }
  if (!v->as.number.has_i64) {
    return GTEXT_JSON_E_INVALID;
  }
  *out = v->as.number.i64;
  return GTEXT_JSON_OK;
}

GTEXT_API GTEXT_JSON_Status gtext_json_get_u64(
    const GTEXT_JSON_Value * v, uint64_t * out) {
  if (!v || !out) {
    return GTEXT_JSON_E_INVALID;
  }
  if (v->type != GTEXT_JSON_NUMBER) {
    return GTEXT_JSON_E_INVALID;
  }
  if (!v->as.number.has_u64) {
    return GTEXT_JSON_E_INVALID;
  }
  *out = v->as.number.u64;
  return GTEXT_JSON_OK;
}

GTEXT_API GTEXT_JSON_Status gtext_json_get_double(
    const GTEXT_JSON_Value * v, double * out) {
  if (!v || !out) {
    return GTEXT_JSON_E_INVALID;
  }
  if (v->type != GTEXT_JSON_NUMBER) {
    return GTEXT_JSON_E_INVALID;
  }
  if (!v->as.number.has_dbl) {
    return GTEXT_JSON_E_INVALID;
  }
  *out = v->as.number.dbl;
  return GTEXT_JSON_OK;
}

GTEXT_API size_t gtext_json_array_size(const GTEXT_JSON_Value * v) {
  if (!v || v->type != GTEXT_JSON_ARRAY) {
    return 0;
  }
  return v->as.array.count;
}

GTEXT_API const GTEXT_JSON_Value * gtext_json_array_get(
    const GTEXT_JSON_Value * v, size_t idx) {
  if (!v || v->type != GTEXT_JSON_ARRAY) {
    return NULL;
  }
  if (idx >= v->as.array.count) {
    return NULL;
  }
  return v->as.array.elems[idx];
}

GTEXT_API size_t gtext_json_object_size(const GTEXT_JSON_Value * v) {
  if (!v || v->type != GTEXT_JSON_OBJECT) {
    return 0;
  }
  return v->as.object.count;
}

GTEXT_API const char * gtext_json_object_key(
    const GTEXT_JSON_Value * v, size_t idx, size_t * key_len) {
  if (!v || v->type != GTEXT_JSON_OBJECT) {
    if (key_len) {
      *key_len = 0;
    }
    return NULL;
  }
  if (idx >= v->as.object.count) {
    if (key_len) {
      *key_len = 0;
    }
    return NULL;
  }
  if (key_len) {
    *key_len = v->as.object.pairs[idx].key_len;
  }
  return v->as.object.pairs[idx].key;
}

GTEXT_API const GTEXT_JSON_Value * gtext_json_object_value(
    const GTEXT_JSON_Value * v, size_t idx) {
  if (!v || v->type != GTEXT_JSON_OBJECT) {
    return NULL;
  }
  if (idx >= v->as.object.count) {
    return NULL;
  }
  return v->as.object.pairs[idx].value;
}

GTEXT_API const GTEXT_JSON_Value * gtext_json_object_get(
    const GTEXT_JSON_Value * v, const char * key, size_t key_len) {
  if (!v || !key || v->type != GTEXT_JSON_OBJECT) {
    return NULL;
  }

  // Linear search through object pairs
  for (size_t i = 0; i < v->as.object.count; ++i) {
    if (v->as.object.pairs[i].key_len == key_len) {
      if (key_len == 0 ||
          memcmp(v->as.object.pairs[i].key, key,
              v->as.object.pairs[i].key_len) == 0) {
        return v->as.object.pairs[i].value;
      }
    }
  }

  return NULL;
}

// Internal helper functions for parser

GTEXT_JSON_Status json_array_add_element(
    GTEXT_JSON_Value * array, GTEXT_JSON_Value * element) {
  if (!array || array->type != GTEXT_JSON_ARRAY || !element) {
    return GTEXT_JSON_E_INVALID;
  }

  // Grow array if needed
  if (array->as.array.count >= array->as.array.capacity) {
    size_t new_capacity =
        array->as.array.capacity == 0 ? 8 : array->as.array.capacity * 2;

    // Check for overflow
    if (new_capacity < array->as.array.capacity) {
      return GTEXT_JSON_E_LIMIT;
    }

    // Allocate new array (using arena from the array's context)
    // Check for overflow in multiplication
    if (new_capacity > SIZE_MAX / sizeof(GTEXT_JSON_Value *)) {
      return GTEXT_JSON_E_LIMIT;
    }
    GTEXT_JSON_Value ** new_elems =
        (GTEXT_JSON_Value **)json_arena_alloc_for_context(array->ctx,
            new_capacity * sizeof(GTEXT_JSON_Value *), sizeof(void *));
    if (!new_elems) {
      return GTEXT_JSON_E_OOM;
    }

    // Copy existing elements
    if (array->as.array.elems) {
      memcpy(new_elems, array->as.array.elems,
          array->as.array.count * sizeof(GTEXT_JSON_Value *));
    }

    array->as.array.elems = new_elems;
    array->as.array.capacity = new_capacity;
  }

  // Add element (check for overflow before incrementing)
  if (array->as.array.count == SIZE_MAX) {
    return GTEXT_JSON_E_LIMIT;
  }
  element->contained = true;
  array->as.array.elems[array->as.array.count++] = element;
  return GTEXT_JSON_OK;
}

GTEXT_JSON_Status json_object_add_pair(GTEXT_JSON_Value * object,
    const char * key, size_t key_len, GTEXT_JSON_Value * value) {
  if (!object || object->type != GTEXT_JSON_OBJECT || !key || !value) {
    return GTEXT_JSON_E_INVALID;
  }

  // Grow object if needed
  if (object->as.object.count >= object->as.object.capacity) {
    size_t new_capacity =
        object->as.object.capacity == 0 ? 8 : object->as.object.capacity * 2;

    // Check for overflow
    if (new_capacity < object->as.object.capacity) {
      return GTEXT_JSON_E_LIMIT;
    }

    // Allocate new pairs array (using arena from the object's context)
    // Check for overflow in multiplication
    size_t pair_size = sizeof(*(object->as.object.pairs));
    if (new_capacity > SIZE_MAX / pair_size) {
      return GTEXT_JSON_E_LIMIT;
    }
    // Cast through void * to avoid anonymous struct type mismatch issues
    void * new_pairs_ptr = json_arena_alloc_for_context(
        object->ctx, new_capacity * pair_size, sizeof(void *));
    if (!new_pairs_ptr) {
      return GTEXT_JSON_E_OOM;
    }

    // Copy existing pairs
    if (object->as.object.pairs) {
      memcpy(new_pairs_ptr, object->as.object.pairs,
          object->as.object.count * sizeof(*(object->as.object.pairs)));
    }

    // Assign through void * to avoid type checking (we know the types match)
    object->as.object.pairs = (void *)new_pairs_ptr;
    object->as.object.capacity = new_capacity;
  }

  // Allocate key string in arena
  // Check for overflow in key_len + 1
  if (key_len > SIZE_MAX - 1) {
    return GTEXT_JSON_E_LIMIT;
  }
  char * key_copy =
      (char *)json_arena_alloc_for_context(object->ctx, key_len + 1, 1);
  if (!key_copy) {
    return GTEXT_JSON_E_OOM;
  }
  memcpy(key_copy, key, key_len);
  key_copy[key_len] = '\0';

  // Add pair (check for overflow before incrementing)
  if (object->as.object.count == SIZE_MAX) {
    // Note: key_copy was allocated from arena, so it will be freed when arena
    // is freed
    return GTEXT_JSON_E_LIMIT;
  }
  size_t idx = object->as.object.count++;
  object->as.object.pairs[idx].key = key_copy;
  object->as.object.pairs[idx].key_len = key_len;
  value->contained = true;
  object->as.object.pairs[idx].value = value;
  return GTEXT_JSON_OK;
}

// Public mutation API functions

/* Would storing @p child inside @p container make the value contain itself?
 *
 * Four bytes of API misuse did this:
 *
 *     GTEXT_JSON_Value *a = gtext_json_new_array();
 *     gtext_json_array_push(a, a);          // returned GTEXT_JSON_OK
 *     gtext_json_free(a);                   // SIGSEGV
 *
 * and nothing reported anything until the process died.  A cyclic value is not
 * just unfreeable: it has no JSON to be written as, every DOM walk here is a
 * walk over containment, and there are a dozen of them - free, write, equal,
 * clone, merge-patch, JSONPath, schema compilation - so the place to stop it is
 * the door it comes in by, not each walk in turn.  The YAML half of this
 * library refuses the equivalent in apply_merge_keys() for the same reason.
 *
 * The question is reachability in the *pre-insert* value, which is acyclic by
 * induction because every door asks this: a cycle through the new edge
 * container -> child exists exactly when @p container is reachable from
 * @p child already, or is @p child.  So the walk terminates.
 *
 * The identity case is checked first and separately, because it is the cheap
 * one and the common mistake.  After that the cost is the size of @p child's
 * subtree, paid per insertion - which makes building a deep value by wrapping
 * ("push the big thing into a fresh container, repeat") quadratic in its
 * depth.  That is the price of the guarantee; a caller assembling something
 * very large top-down rather than inside-out pays nothing, because then the
 * child is a leaf.
 *
 * Returns GTEXT_JSON_OK when the insertion is safe, E_INVALID when it would
 * make a cycle, and E_OOM when the walk could not be completed - which is not
 * "safe", and is reported rather than assumed either way. */
static GTEXT_JSON_Status json_check_no_cycle(
    const GTEXT_JSON_Value * container, const GTEXT_JSON_Value * child) {
  if (!container || !child) {
    return GTEXT_JSON_OK;
  }
  if (container == child) {
    return GTEXT_JSON_E_INVALID;
  }
  /* Only a container can hold anything, so only a container can be a step on
     a path back to @p container. */
  if (child->type != GTEXT_JSON_ARRAY && child->type != GTEXT_JSON_OBJECT) {
    return GTEXT_JSON_OK;
  }
  /* And nothing can reach a value that nothing points at.  See the `contained`
     field: this is what keeps assembling a deep value inside-out linear rather
     than quadratic, and it is sound only because every door below sets the
     flag. */
  if (!container->contained) {
    return GTEXT_JSON_OK;
  }

  const GTEXT_JSON_Value ** stack = NULL;
  size_t count = 0, capacity = 0;
  GTEXT_JSON_Status status = GTEXT_JSON_OK;

  /* No allocator threaded in: this is a predicate with no document in hand
     whose scratch is transient, the same reason nodes_equal() in
     yaml_resolve.c gives for its own frame stack. */
  #define JSON_CYCLE_PUSH(n)                                                   \
    do {                                                                       \
      const GTEXT_JSON_Value * push_node = (n);                                \
      if (push_node) {                                                         \
        if (push_node == container) {                                          \
          status = GTEXT_JSON_E_INVALID;                                       \
          goto done;                                                           \
        }                                                                      \
        if (push_node->type == GTEXT_JSON_ARRAY                                \
            || push_node->type == GTEXT_JSON_OBJECT) {                         \
          if (count == capacity) {                                             \
            size_t new_capacity = capacity == 0 ? 32 : capacity * 2;           \
            const GTEXT_JSON_Value ** items =                                  \
                (const GTEXT_JSON_Value **)gtext_allocator_realloc(            \
                    NULL, (void *)stack, new_capacity * sizeof(*items));       \
            if (!items) { status = GTEXT_JSON_E_OOM; goto done; }              \
            stack = items;                                                     \
            capacity = new_capacity;                                           \
          }                                                                    \
          stack[count++] = push_node;                                          \
        }                                                                      \
      }                                                                        \
    } while (0)

  JSON_CYCLE_PUSH(child);

  while (count > 0) {
    const GTEXT_JSON_Value * node = stack[--count];
    if (node->type == GTEXT_JSON_ARRAY) {
      for (size_t i = 0; i < node->as.array.count; i++) {
        JSON_CYCLE_PUSH(node->as.array.elems[i]);
      }
    }
    else {
      for (size_t i = 0; i < node->as.object.count; i++) {
        JSON_CYCLE_PUSH(node->as.object.pairs[i].value);
      }
    }
  }

done:
  #undef JSON_CYCLE_PUSH
  gtext_allocator_free(NULL, (void *)stack);
  return status;
}

GTEXT_API GTEXT_JSON_Status gtext_json_array_push(
    GTEXT_JSON_Value * arr, GTEXT_JSON_Value * child) {
  if (!arr || arr->type != GTEXT_JSON_ARRAY || !child) {
    return GTEXT_JSON_E_INVALID;
  }
  GTEXT_JSON_Status cycle = json_check_no_cycle(arr, child);
  if (cycle != GTEXT_JSON_OK) {
    return cycle;
  }

  // Use the internal helper function which handles growing the array
  return json_array_add_element(arr, child);
}

GTEXT_API GTEXT_JSON_Status gtext_json_array_set(
    GTEXT_JSON_Value * arr, size_t idx, GTEXT_JSON_Value * child) {
  if (!arr || arr->type != GTEXT_JSON_ARRAY || !child) {
    return GTEXT_JSON_E_INVALID;
  }

  // Check bounds
  if (idx >= arr->as.array.count) {
    return GTEXT_JSON_E_INVALID;
  }

  /* Before the old value is freed below, so a refused call changes nothing. */
  GTEXT_JSON_Status cycle = json_check_no_cycle(arr, child);
  if (cycle != GTEXT_JSON_OK) {
    return cycle;
  }

  // Get the old value to be replaced
  GTEXT_JSON_Value * old_value = arr->as.array.elems[idx];

  // If the old value has a different context, free it recursively
  if (old_value && old_value->ctx && old_value->ctx != arr->ctx) {
    json_context * child_ctx = old_value->ctx;
    // Recursively free child's children first
    json_free_foreign_contexts(old_value);
    // Now free the child's context (this frees the child value structure
    // itself)
    json_context_free(child_ctx);
  }

  // Set element at index (replacing existing element)
  child->contained = true;
  arr->as.array.elems[idx] = child;

  // Note: If child has a different context, it will be freed when
  // gtext_json_free() is called on the root, via
  // json_free_foreign_contexts(). We don't free it here because the child
  // value structure itself is in that context's arena.
  return GTEXT_JSON_OK;
}

GTEXT_API GTEXT_JSON_Status gtext_json_array_insert(
    GTEXT_JSON_Value * arr, size_t idx, GTEXT_JSON_Value * child) {
  if (!arr || arr->type != GTEXT_JSON_ARRAY || !child) {
    return GTEXT_JSON_E_INVALID;
  }

  // Check bounds - allow inserting at count (same as push)
  if (idx > arr->as.array.count) {
    return GTEXT_JSON_E_INVALID;
  }

  GTEXT_JSON_Status cycle = json_check_no_cycle(arr, child);
  if (cycle != GTEXT_JSON_OK) {
    return cycle;
  }

  // If inserting at the end, use push logic
  if (idx == arr->as.array.count) {
    return json_array_add_element(arr, child);
  }

  // Need to insert in the middle - grow array if needed first
  if (arr->as.array.count >= arr->as.array.capacity) {
    size_t new_capacity =
        arr->as.array.capacity == 0 ? 8 : arr->as.array.capacity * 2;

    // Check for overflow
    if (new_capacity < arr->as.array.capacity) {
      return GTEXT_JSON_E_LIMIT;
    }

    // Allocate new array
    if (new_capacity > SIZE_MAX / sizeof(GTEXT_JSON_Value *)) {
      return GTEXT_JSON_E_LIMIT;
    }
    GTEXT_JSON_Value ** new_elems =
        (GTEXT_JSON_Value **)json_arena_alloc_for_context(arr->ctx,
            new_capacity * sizeof(GTEXT_JSON_Value *), sizeof(void *));
    if (!new_elems) {
      return GTEXT_JSON_E_OOM;
    }

    // Copy existing elements: [0..idx) to [0..idx), then [idx..count) to
    // [idx+1..count+1)
    if (arr->as.array.elems) {
      // Copy elements before insertion point
      if (idx > 0) {
        memcpy(
            new_elems, arr->as.array.elems, idx * sizeof(GTEXT_JSON_Value *));
      }
      // Copy elements after insertion point, shifted right by one
      if (idx < arr->as.array.count) {
        memcpy(new_elems + idx + 1, arr->as.array.elems + idx,
            (arr->as.array.count - idx) * sizeof(GTEXT_JSON_Value *));
      }
    }

    arr->as.array.elems = new_elems;
    arr->as.array.capacity = new_capacity;
  }
  else {
    // Have capacity - shift elements in place to make room
    // Shift elements from idx to count-1 one position to the right
    // Note: Writing to elems[count] is safe because we're in the else branch,
    // which means count < capacity, so elems[count] is within allocated bounds.
    for (size_t i = arr->as.array.count; i > idx; --i) {
      arr->as.array.elems[i] = arr->as.array.elems[i - 1];
    }
  }

  // Insert new element at idx
  child->contained = true;
  arr->as.array.elems[idx] = child;

  // Increment count (check for overflow)
  if (arr->as.array.count == SIZE_MAX) {
    return GTEXT_JSON_E_LIMIT;
  }
  arr->as.array.count++;
  return GTEXT_JSON_OK;
}

GTEXT_API GTEXT_JSON_Status gtext_json_array_remove(
    GTEXT_JSON_Value * arr, size_t idx) {
  if (!arr || arr->type != GTEXT_JSON_ARRAY) {
    return GTEXT_JSON_E_INVALID;
  }

  // Check bounds
  if (idx >= arr->as.array.count) {
    return GTEXT_JSON_E_INVALID;
  }

  // Get the value to be removed
  GTEXT_JSON_Value * removed_value = arr->as.array.elems[idx];

  // If the removed value has a different context, free it recursively
  if (removed_value && removed_value->ctx && removed_value->ctx != arr->ctx) {
    json_context * child_ctx = removed_value->ctx;
    // Recursively free child's children first
    json_free_foreign_contexts(removed_value);
    // Now free the child's context (this frees the child value structure
    // itself)
    json_context_free(child_ctx);
  }

  // Shift elements to the left to fill the gap
  for (size_t i = idx; i + 1 < arr->as.array.count; ++i) {
    arr->as.array.elems[i] = arr->as.array.elems[i + 1];
  }

  // Decrement count
  arr->as.array.count--;
  return GTEXT_JSON_OK;
}

GTEXT_API GTEXT_JSON_Status gtext_json_object_put(GTEXT_JSON_Value * obj,
    const char * key, size_t key_len, GTEXT_JSON_Value * val) {
  if (!obj || obj->type != GTEXT_JSON_OBJECT || !key || !val) {
    return GTEXT_JSON_E_INVALID;
  }

  /* Before anything is replaced, so a refused call changes nothing. */
  GTEXT_JSON_Status cycle = json_check_no_cycle(obj, val);
  if (cycle != GTEXT_JSON_OK) {
    return cycle;
  }

  // Check if key already exists - if so, replace the value
  for (size_t i = 0; i < obj->as.object.count; ++i) {
    if (obj->as.object.pairs[i].key_len == key_len) {
      if (key_len == 0 ||
          memcmp(obj->as.object.pairs[i].key, key, key_len) == 0) {
        // Key exists - replace value
        GTEXT_JSON_Value * old_value = obj->as.object.pairs[i].value;

        // If the old value has a different context, free it recursively
        if (old_value && old_value->ctx && old_value->ctx != obj->ctx) {
          json_context * child_ctx = old_value->ctx;
          // Recursively free child's children first
          json_free_foreign_contexts(old_value);
          // Now free the child's context (this frees the child value structure
          // itself)
          json_context_free(child_ctx);
        }

        val->contained = true;
        obj->as.object.pairs[i].value = val;
        return GTEXT_JSON_OK;
      }
    }
  }

  // Key doesn't exist - add new pair using internal helper
  return json_object_add_pair(obj, key, key_len, val);
}

GTEXT_API GTEXT_JSON_Status gtext_json_object_remove(
    GTEXT_JSON_Value * obj, const char * key, size_t key_len) {
  if (!obj || obj->type != GTEXT_JSON_OBJECT || !key) {
    return GTEXT_JSON_E_INVALID;
  }

  // Find the key
  size_t found_idx = SIZE_MAX;
  for (size_t i = 0; i < obj->as.object.count; ++i) {
    if (obj->as.object.pairs[i].key_len == key_len) {
      if (key_len == 0 ||
          memcmp(obj->as.object.pairs[i].key, key, key_len) == 0) {
        found_idx = i;
        break;
      }
    }
  }

  // Key not found
  if (found_idx == SIZE_MAX) {
    return GTEXT_JSON_E_INVALID;
  }

  // Get the value to be removed
  GTEXT_JSON_Value * removed_value = obj->as.object.pairs[found_idx].value;

  // If the removed value has a different context, free it recursively
  if (removed_value && removed_value->ctx && removed_value->ctx != obj->ctx) {
    json_context * child_ctx = removed_value->ctx;
    // Recursively free child's children first
    json_free_foreign_contexts(removed_value);
    // Now free the child's context (this frees the child value structure
    // itself)
    json_context_free(child_ctx);
  }

  // Shift pairs to the left to fill the gap
  for (size_t i = found_idx; i + 1 < obj->as.object.count; ++i) {
    obj->as.object.pairs[i] = obj->as.object.pairs[i + 1];
  }

  // Decrement count
  obj->as.object.count--;
  return GTEXT_JSON_OK;
}

// Helper function for deep equality comparison with configurable mode
/* Everything but the two collection arms: the answer for a pair of values whose
   types already match and which are not both containers. */
static bool json_scalar_equal(const GTEXT_JSON_Value * a,
    const GTEXT_JSON_Value * b, GTEXT_JSON_Equal_Mode mode) {
  switch (a->type) {
  case GTEXT_JSON_NULL:
    return true;

  case GTEXT_JSON_BOOL:
    return a->as.boolean == b->as.boolean;

  case GTEXT_JSON_STRING:
    if (a->as.string.len != b->as.string.len) {
      return false;
    }
    // Check for NULL pointers before memcmp
    if (!a->as.string.data || !b->as.string.data) {
      return (
          a->as.string.data == b->as.string.data); // Both NULL or both non-NULL
    }
    return memcmp(a->as.string.data, b->as.string.data, a->as.string.len) == 0;

  case GTEXT_JSON_NUMBER:
    if (mode == GTEXT_JSON_EQUAL_LEXEME) {
      // Lexeme-based comparison: must have identical lexemes
      if (a->as.number.lexeme_len != b->as.number.lexeme_len) {
        return false;
      }
      if (a->as.number.lexeme && b->as.number.lexeme) {
        return memcmp(a->as.number.lexeme, b->as.number.lexeme,
                   a->as.number.lexeme_len) == 0;
      }
      // If either doesn't have a lexeme, they're not equal in lexeme mode
      return false;
    }
    // Numeric equivalence comparison: the same representation, if both have it
    if (a->as.number.has_i64 && b->as.number.has_i64) {
      return a->as.number.i64 == b->as.number.i64;
    }
    if (a->as.number.has_u64 && b->as.number.has_u64) {
      return a->as.number.u64 == b->as.number.u64;
    }
    if (a->as.number.has_dbl && b->as.number.has_dbl) {
      double diff = fabs(a->as.number.dbl - b->as.number.dbl);
      return diff < 1e-15 || (a->as.number.dbl == b->as.number.dbl);
    }
    // Fall back to lexeme comparison if no numeric representation available
    if (a->as.number.lexeme_len != b->as.number.lexeme_len) {
      return false;
    }
    if (a->as.number.lexeme && b->as.number.lexeme) {
      return memcmp(a->as.number.lexeme, b->as.number.lexeme,
                 a->as.number.lexeme_len) == 0;
    }
    return false;

  default:
    return false;
  }
}

/* b's first pair whose key is the one of a's pair @p i, or -1.
 *
 * JSON object keys are strings, so the match is a string search and not a
 * recursive comparison - which is the whole reason this walk is a plain
 * conjunction and nodes_equal() in the YAML half is not.  First match wins, and
 * its value decides: that is what the recursion did, and an object with two
 * equal keys is not a shape either side has to agree about. */
static long json_object_match(const GTEXT_JSON_Value * a,
    const GTEXT_JSON_Value * b, size_t i) {
  const char * key = a->as.object.pairs[i].key;
  const size_t key_len = a->as.object.pairs[i].key_len;
  for (size_t j = 0; j < b->as.object.count; j++) {
    if (b->as.object.pairs[j].key_len != key_len) {
      continue;
    }
    if (key_len == 0) {
      if ((key == NULL) == (b->as.object.pairs[j].key == NULL)) {
        return (long)j;
      }
      continue;
    }
    if (key && b->as.object.pairs[j].key
        && memcmp(b->as.object.pairs[j].key, key, key_len) == 0) {
      return (long)j;
    }
  }
  return -1;
}

/* Where the comparison is inside one pair of containers. */
typedef struct {
  const GTEXT_JSON_Value * a;
  const GTEXT_JSON_Value * b;
  size_t i;
} json_eq_frame;

/* Deep equality, with the walk's stack on the heap.
 *
 * It was a recursion, and a value built through the DOM API has no depth limit
 * to bound it - max_depth is a *parser* option.  Two hundred thousand levels
 * took the process down.
 *
 * A plain conjunction, so no result has to be carried back up: the first
 * inequality is the answer and the walk stops there.  The frontier is a cursor
 * per container rather than every child at once, so the stack is as deep as the
 * values and not as wide as their widest array.
 *
 * Cycles are not guarded against, and do not need to be: a value cannot contain
 * itself, because json_check_no_cycle() refuses the insertion that would do it.
 *
 * A stack that will not grow is reported as "not equal", which is the one place
 * this function has to give a wrong answer rather than an error - it returns a
 * bool and gtext_json_equal() has nowhere to put a failure.  Said here because
 * it is the kind of thing a reader should not have to find out by reading the
 * allocation. */
static bool json_value_equal_internal(const GTEXT_JSON_Value * a,
    const GTEXT_JSON_Value * b, GTEXT_JSON_Equal_Mode mode) {
  json_eq_frame inline_frames[64];
  json_eq_frame * frames = inline_frames;
  size_t count = 0;
  size_t capacity = sizeof(inline_frames) / sizeof(inline_frames[0]);
  bool answer = true;

  #define JSON_EQ_VISIT(na, nb)                                                \
    do {                                                                       \
      const GTEXT_JSON_Value * x = (na);                                       \
      const GTEXT_JSON_Value * y = (nb);                                       \
      if (x != y) {                                                            \
        if (!x || !y || x->type != y->type) { answer = false; goto done; }      \
        if (x->type == GTEXT_JSON_ARRAY) {                                     \
          if (x->as.array.count != y->as.array.count) {                        \
            answer = false; goto done;                                         \
          }                                                                    \
          if (!x->as.array.elems || !y->as.array.elems) {                      \
            if (x->as.array.elems != y->as.array.elems) {                      \
              answer = false; goto done;                                       \
            }                                                                  \
          }                                                                    \
          else { JSON_EQ_PUSH(x, y); }                                         \
        }                                                                      \
        else if (x->type == GTEXT_JSON_OBJECT) {                               \
          if (x->as.object.count != y->as.object.count) {                      \
            answer = false; goto done;                                         \
          }                                                                    \
          if (!x->as.object.pairs || !y->as.object.pairs) {                    \
            if (x->as.object.pairs != y->as.object.pairs) {                    \
              answer = false; goto done;                                       \
            }                                                                  \
          }                                                                    \
          else { JSON_EQ_PUSH(x, y); }                                         \
        }                                                                      \
        else if (!json_scalar_equal(x, y, mode)) {                             \
          answer = false; goto done;                                           \
        }                                                                      \
      }                                                                        \
    } while (0)

  #define JSON_EQ_PUSH(na, nb)                                                 \
    do {                                                                       \
      if (count == capacity) {                                                 \
        size_t new_capacity = capacity * 2;                                    \
        json_eq_frame * grown;                                                 \
        if (frames == inline_frames) {                                         \
          grown = (json_eq_frame *)gtext_allocator_malloc(                     \
              NULL, new_capacity * sizeof(*grown));                            \
          if (grown) memcpy(grown, frames, count * sizeof(*grown));            \
        }                                                                      \
        else {                                                                 \
          grown = (json_eq_frame *)gtext_allocator_realloc(                    \
              NULL, frames, new_capacity * sizeof(*grown));                    \
        }                                                                      \
        if (!grown) { answer = false; goto done; }                             \
        frames = grown;                                                        \
        capacity = new_capacity;                                               \
      }                                                                        \
      frames[count].a = (na);                                                  \
      frames[count].b = (nb);                                                  \
      frames[count].i = 0;                                                     \
      count++;                                                                 \
    } while (0)

  JSON_EQ_VISIT(a, b);

  while (count > 0) {
    json_eq_frame * f = &frames[count - 1];

    if (f->a->type == GTEXT_JSON_ARRAY) {
      if (f->i >= f->a->as.array.count) { count--; continue; }
      const GTEXT_JSON_Value * ca = f->a->as.array.elems[f->i];
      const GTEXT_JSON_Value * cb = f->b->as.array.elems[f->i];
      f->i++;
      JSON_EQ_VISIT(ca, cb);
      continue;
    }

    if (f->i >= f->a->as.object.count) { count--; continue; }
    const long j = json_object_match(f->a, f->b, f->i);
    if (j < 0) { answer = false; goto done; }
    const GTEXT_JSON_Value * va = f->a->as.object.pairs[f->i].value;
    const GTEXT_JSON_Value * vb = f->b->as.object.pairs[(size_t)j].value;
    f->i++;
    JSON_EQ_VISIT(va, vb);
  }

done:
  #undef JSON_EQ_VISIT
  #undef JSON_EQ_PUSH
  if (frames != inline_frames) {
    gtext_allocator_free(NULL, frames);
  }
  return answer;
}

GTEXT_API bool gtext_json_equal(const GTEXT_JSON_Value * a,
    const GTEXT_JSON_Value * b, GTEXT_JSON_Equal_Mode mode) {
  return json_value_equal_internal(a, b, mode);
}

GTEXT_INTERNAL_API GTEXT_JSON_Value * json_value_clone_new_context(
    const GTEXT_JSON_Value * src, const GTEXT_Allocator * alloc) {
  if (!src) {
    return NULL;
  }

  json_context * new_ctx = json_context_new(alloc);
  if (!new_ctx) {
    return NULL;
  }

  // Use the existing clone function but with the new context
  GTEXT_JSON_Value * dst = json_value_clone(src, new_ctx);
  if (!dst) {
    json_context_free(new_ctx);
    return NULL;
  }

  return dst;
}

GTEXT_API GTEXT_JSON_Value * gtext_json_clone(const GTEXT_JSON_Value * src) {
  /*
   * The clone inherits the source's allocator rather than the C library.
   *
   * gtext_json_clone() takes no options, so for a long time this passed NULL
   * and a clone of an arena-allocated document came from the C library -
   * self-consistent, because the clone records the context that made it and
   * gtext_json_free() releases through it, but a complete bypass of an
   * allocator that was sitting in src->ctx the whole time.  json_context::alloc
   * is documented as never NULL, so there is nothing to default here.
   *
   * A caller that needs the copy on a *different* allocator from the original
   * calls json_value_clone_new_context() and names one; the compiled schema
   * does, because it outlives the document it was built from.
   */
  return json_value_clone_new_context(
      src, src && src->ctx ? src->ctx->alloc : NULL);
}

GTEXT_API GTEXT_JSON_Status gtext_json_object_merge(GTEXT_JSON_Value * target,
    const GTEXT_JSON_Value * source, GTEXT_JSON_Merge_Policy policy) {
  if (!target || target->type != GTEXT_JSON_OBJECT) {
    return GTEXT_JSON_E_INVALID;
  }
  if (!source || source->type != GTEXT_JSON_OBJECT) {
    return GTEXT_JSON_E_INVALID;
  }

  // Iterate through all pairs in source
  for (size_t i = 0; i < source->as.object.count; i++) {
    const char * key = source->as.object.pairs[i].key;
    size_t key_len = source->as.object.pairs[i].key_len;
    const GTEXT_JSON_Value * source_val = source->as.object.pairs[i].value;

    // Check if key exists in target
    int key_exists = 0;
    for (size_t j = 0; j < target->as.object.count; j++) {
      if (target->as.object.pairs[j].key_len == key_len) {
        if (key_len == 0 ||
            memcmp(target->as.object.pairs[j].key, key, key_len) == 0) {
          key_exists = 1;

          // Handle conflict based on policy
          if (policy == GTEXT_JSON_MERGE_ERROR) {
            return GTEXT_JSON_E_DUPKEY; // Conflict detected
          }

          // If both values are objects, merge recursively
          if (target->as.object.pairs[j].value != NULL &&
              target->as.object.pairs[j].value->type == GTEXT_JSON_OBJECT &&
              source_val != NULL && source_val->type == GTEXT_JSON_OBJECT) {
            GTEXT_JSON_Status status = gtext_json_object_merge(
                target->as.object.pairs[j].value, source_val, policy);
            if (status != GTEXT_JSON_OK) {
              return status;
            }
          }
          else {
            // Not both objects - replace based on policy
            if (policy == GTEXT_JSON_MERGE_LAST_WINS) {
              // Clone the source value into target's context
              GTEXT_JSON_Value * cloned_val =
                  json_value_clone(source_val, target->ctx);
              if (!cloned_val) {
                return GTEXT_JSON_E_OOM;
              }
              // Free old value if it has different context
              GTEXT_JSON_Value * old_val = target->as.object.pairs[j].value;
              if (old_val && old_val->ctx && old_val->ctx != target->ctx) {
                json_context * old_ctx = old_val->ctx;
                json_free_foreign_contexts(old_val);
                json_context_free(old_ctx);
              }
              target->as.object.pairs[j].value = cloned_val;
            }
            // For FIRST_WINS, do nothing (keep existing value)
          }
          break;
        }
      }
    }

    // Key doesn't exist in target - add it
    if (!key_exists) {
      // Clone the source value into target's context
      GTEXT_JSON_Value * cloned_val = json_value_clone(source_val, target->ctx);
      if (!cloned_val) {
        return GTEXT_JSON_E_OOM;
      }
      GTEXT_JSON_Status status =
          json_object_add_pair(target, key, key_len, cloned_val);
      if (status != GTEXT_JSON_OK) {
        // Note: If json_object_add_pair fails after cloning, the cloned value
        // is orphaned in target->ctx. Since arena allocators don't support
        // freeing individual items, the value will remain allocated until
        // target->ctx is freed. This is acceptable because:
        // 1. The failure cases are rare (OOM or integer overflow)
        // 2. The memory will be freed when the target object is freed
        // 3. The alternative (cloning only after successful allocation) would
        //    require significant refactoring and is not worth the complexity
        return status;
      }
    }
  }

  return GTEXT_JSON_OK;
}
