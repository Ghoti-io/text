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
 * @file toml_dom.c
 * @brief Building and reading a TOML document, and releasing it.
 */

#include "toml_internal.h"
#include <string.h>

GTEXT_TOML_Value * toml_value_new(
    const GTEXT_Allocator * alloc, GTEXT_TOML_Type type) {
  GTEXT_TOML_Value * value =
      gtext_allocator_calloc(alloc, 1, sizeof(GTEXT_TOML_Value));
  if (!value) return NULL;
  value->type = type;
  value->alloc = alloc;
  return value;
}

bool toml_array_push(const GTEXT_Allocator * alloc, GTEXT_TOML_Value * array,
    GTEXT_TOML_Value * item) {
  if (array->as.array.count == array->as.array.capacity) {
    size_t want = array->as.array.capacity ? array->as.array.capacity * 2 : 4;
    if (want > SIZE_MAX / sizeof(GTEXT_TOML_Value *)) return false;
    GTEXT_TOML_Value ** grown = gtext_allocator_realloc(alloc,
        array->as.array.items, want * sizeof(GTEXT_TOML_Value *));
    if (!grown) return false;
    array->as.array.items = grown;
    array->as.array.capacity = want;
  }
  array->as.array.items[array->as.array.count++] = item;
  item->parent = array;
  return true;
}

GTEXT_TOML_Value * toml_table_find(
    const GTEXT_TOML_Value * table, const char * key, size_t key_len) {
  /* Linear, as the JSON module's objects are. Keys are compared by bytes and
   * not by string functions: a TOML key may contain a NUL, because U+0000 has
   * a valid escape and a quoted key may use it. */
  for (size_t i = 0; i < table->as.table.count; ++i) {
    if (table->as.table.pairs[i].len == key_len
        && (key_len == 0
            || memcmp(table->as.table.pairs[i].key, key, key_len) == 0)) {
      return table->as.table.pairs[i].value;
    }
  }
  return NULL;
}

bool toml_table_insert(const GTEXT_Allocator * alloc, GTEXT_TOML_Value * table,
    const char * key, size_t key_len, GTEXT_TOML_Value * value) {
  if (table->as.table.count == table->as.table.capacity) {
    size_t want = table->as.table.capacity ? table->as.table.capacity * 2 : 4;
    if (want > SIZE_MAX / sizeof(toml_pair)) return false;
    toml_pair * grown = gtext_allocator_realloc(
        alloc, table->as.table.pairs, want * sizeof(toml_pair));
    if (!grown) return false;
    table->as.table.pairs = grown;
    table->as.table.capacity = want;
  }
  char * copy = gtext_allocator_malloc(alloc, key_len + 1);
  if (!copy) return false;
  if (key_len) memcpy(copy, key, key_len);
  copy[key_len] = '\0';
  table->as.table.pairs[table->as.table.count].key = copy;
  table->as.table.pairs[table->as.table.count].len = key_len;
  table->as.table.pairs[table->as.table.count].value = value;
  table->as.table.count++;
  value->parent = table;
  return true;
}

/** Push a child onto the teardown worklist, growing it. */
static bool free_push(const GTEXT_Allocator * alloc,
    GTEXT_TOML_Value *** stack, size_t * count, size_t * capacity,
    GTEXT_TOML_Value * child) {
  if (*count == *capacity) {
    size_t want = *capacity ? *capacity * 2 : 16;
    if (want > SIZE_MAX / sizeof(GTEXT_TOML_Value *)) return false;
    GTEXT_TOML_Value ** grown = gtext_allocator_realloc(
        alloc, *stack, want * sizeof(GTEXT_TOML_Value *));
    if (!grown) return false;
    *stack = grown;
    *capacity = want;
  }
  (*stack)[(*count)++] = child;
  return true;
}

/**
 * Release a tree without recursing.
 *
 * The C stack is not used to walk the document, so a tree nested as deeply as
 * the parse allowed can always be freed - and with max_depth at 0 that is as
 * deep as memory allows. `text`'s three YAML DOM walks were moved off the
 * stack for exactly this reason, taking the contract from "never crashes
 * unless you asked for no limit" to "never crashes"; a new module that starts
 * with a recursive free puts the same crash back in a new place.
 *
 * Children are pushed and the parent's own storage released immediately, so
 * the worklist holds only nodes not yet visited.
 */
void gtext_toml_free(GTEXT_TOML_Value * root) {
  if (!root) return;
  /* The root's allocator is the document's. Freeing a tree built with a
   * caller-supplied allocator through the default one would corrupt the heap,
   * silently and in the caller's process rather than here. */
  const GTEXT_Allocator * alloc = root->alloc;

  GTEXT_TOML_Value ** stack = NULL;
  size_t count = 0;
  size_t capacity = 0;

  GTEXT_TOML_Value * current = root;
  for (;;) {
    switch (current->type) {
      case GTEXT_TOML_STRING:
        gtext_allocator_free(alloc, current->as.string.data);
        break;
      case GTEXT_TOML_ARRAY:
        for (size_t i = 0; i < current->as.array.count; ++i) {
          if (!free_push(alloc, &stack, &count, &capacity,
                  current->as.array.items[i])) {
            /* An allocation failure inside a teardown is the one failure this
             * library has nowhere to report. The remaining children leak
             * rather than the process dying. */
            break;
          }
        }
        gtext_allocator_free(alloc, current->as.array.items);
        break;
      case GTEXT_TOML_TABLE:
        for (size_t i = 0; i < current->as.table.count; ++i) {
          gtext_allocator_free(alloc, current->as.table.pairs[i].key);
          if (!free_push(alloc, &stack, &count, &capacity,
                  current->as.table.pairs[i].value)) {
            break;
          }
        }
        gtext_allocator_free(alloc, current->as.table.pairs);
        break;
      default:
        break;
    }
    if (current->comments) {
      gtext_allocator_free(alloc, current->comments->leading);
      gtext_allocator_free(alloc, current->comments->trailing_inline);
      gtext_allocator_free(alloc, current->comments->trailing);
      gtext_allocator_free(alloc, current->comments);
    }
    gtext_allocator_free(alloc, current);
    if (count == 0) break;
    current = stack[--count];
  }
  gtext_allocator_free(alloc, stack);
}

GTEXT_TOML_Type gtext_toml_value_type(const GTEXT_TOML_Value * value) {
  return value ? value->type : GTEXT_TOML_TABLE;
}

const char * gtext_toml_value_string(
    const GTEXT_TOML_Value * value, size_t * len) {
  if (!value || value->type != GTEXT_TOML_STRING) return NULL;
  if (len) *len = value->as.string.len;
  return value->as.string.data;
}

bool gtext_toml_value_integer(const GTEXT_TOML_Value * value, int64_t * out) {
  if (!value || !out || value->type != GTEXT_TOML_INTEGER) return false;
  *out = value->as.integer;
  return true;
}

bool gtext_toml_value_float(const GTEXT_TOML_Value * value, double * out) {
  if (!value || !out || value->type != GTEXT_TOML_FLOAT) return false;
  *out = value->as.floating;
  return true;
}

bool gtext_toml_value_boolean(const GTEXT_TOML_Value * value, bool * out) {
  if (!value || !out || value->type != GTEXT_TOML_BOOLEAN) return false;
  *out = value->as.boolean;
  return true;
}

bool gtext_toml_value_datetime(
    const GTEXT_TOML_Value * value, GCHRON_TomlValue * out) {
  if (!value || !out || value->type != GTEXT_TOML_DATETIME) return false;
  *out = value->as.datetime;
  return true;
}

size_t gtext_toml_array_size(const GTEXT_TOML_Value * value) {
  if (!value || value->type != GTEXT_TOML_ARRAY) return 0;
  return value->as.array.count;
}

const GTEXT_TOML_Value * gtext_toml_array_get(
    const GTEXT_TOML_Value * value, size_t index) {
  if (!value || value->type != GTEXT_TOML_ARRAY) return NULL;
  if (index >= value->as.array.count) return NULL;
  return value->as.array.items[index];
}

size_t gtext_toml_table_size(const GTEXT_TOML_Value * value) {
  if (!value || value->type != GTEXT_TOML_TABLE) return 0;
  return value->as.table.count;
}

const char * gtext_toml_table_key_at(
    const GTEXT_TOML_Value * value, size_t index, size_t * len) {
  if (!value || value->type != GTEXT_TOML_TABLE) return NULL;
  if (index >= value->as.table.count) return NULL;
  if (len) *len = value->as.table.pairs[index].len;
  return value->as.table.pairs[index].key;
}

const GTEXT_TOML_Value * gtext_toml_table_value_at(
    const GTEXT_TOML_Value * value, size_t index) {
  if (!value || value->type != GTEXT_TOML_TABLE) return NULL;
  if (index >= value->as.table.count) return NULL;
  return value->as.table.pairs[index].value;
}

const GTEXT_TOML_Value * gtext_toml_table_get(
    const GTEXT_TOML_Value * value, const char * key, size_t key_len) {
  if (!value || value->type != GTEXT_TOML_TABLE || !key) return NULL;
  return toml_table_find(value, key, key_len);
}

/*--------------------------------------------------------------------------*
 * Building a document
 *
 * The parser does not go through these: it inserts nodes it has just made,
 * into containers it owns, and so cannot produce either of the shapes the
 * checks here refuse. Putting the checks in the public entry points rather
 * than in toml_table_insert() keeps the ancestor walk off the parse path,
 * where it would turn a deep document into quadratic work for a case that
 * cannot arise.
 *--------------------------------------------------------------------------*/

GTEXT_TOML_Value * gtext_toml_new_table(const GTEXT_Allocator * alloc) {
  GTEXT_TOML_Value * value = toml_value_new(alloc, GTEXT_TOML_TABLE);
  if (value) value->origin = TOML_TABLE_HEADER;
  return value;
}

GTEXT_TOML_Value * gtext_toml_new_array(const GTEXT_Allocator * alloc) {
  GTEXT_TOML_Value * value = toml_value_new(alloc, GTEXT_TOML_ARRAY);
  if (value) value->origin = TOML_ARRAY_STATIC;
  return value;
}

GTEXT_TOML_Value * gtext_toml_new_string(
    const GTEXT_Allocator * alloc, const char * bytes, size_t len) {
  if (!bytes && len) return NULL;
  GTEXT_TOML_Value * value = toml_value_new(alloc, GTEXT_TOML_STRING);
  if (!value) return NULL;
  /* One byte more than asked for, and NUL there: the accessor promises a
   * terminated buffer for convenience even though `len` is what counts. An
   * empty string still gets storage, so that the accessor never answers NULL
   * for a string that exists. */
  char * copy = gtext_allocator_malloc(alloc, len + 1);
  if (!copy) {
    gtext_allocator_free(alloc, value);
    return NULL;
  }
  if (len) memcpy(copy, bytes, len);
  copy[len] = '\0';
  value->as.string.data = copy;
  value->as.string.len = len;
  return value;
}

GTEXT_TOML_Value * gtext_toml_new_integer(
    const GTEXT_Allocator * alloc, int64_t number) {
  GTEXT_TOML_Value * value = toml_value_new(alloc, GTEXT_TOML_INTEGER);
  if (value) value->as.integer = number;
  return value;
}

GTEXT_TOML_Value * gtext_toml_new_float(
    const GTEXT_Allocator * alloc, double number) {
  GTEXT_TOML_Value * value = toml_value_new(alloc, GTEXT_TOML_FLOAT);
  if (value) value->as.floating = number;
  return value;
}

GTEXT_TOML_Value * gtext_toml_new_boolean(
    const GTEXT_Allocator * alloc, bool boolean) {
  GTEXT_TOML_Value * value = toml_value_new(alloc, GTEXT_TOML_BOOLEAN);
  if (value) value->as.boolean = boolean;
  return value;
}

GTEXT_TOML_Value * gtext_toml_new_datetime(
    const GTEXT_Allocator * alloc, const GCHRON_TomlValue * datetime) {
  if (!datetime) return NULL;
  GTEXT_TOML_Value * value = toml_value_new(alloc, GTEXT_TOML_DATETIME);
  if (value) value->as.datetime = *datetime;
  return value;
}

/**
 * The three ways a caller can make a tree that cannot be written or freed.
 *
 * `value` already stored is a double free waiting for teardown; `value` being
 * the container, or an ancestor of it, is a cycle. The walk upwards is O(depth)
 * and is the only one of the three that costs anything; it is cheap because a
 * node that is not yet stored anywhere can only be an ancestor of `container`
 * by being the root of the tree `container` is in.
 */
static GTEXT_TOML_Status toml_check_link(
    const GTEXT_TOML_Value * container, const GTEXT_TOML_Value * value) {
  if (value->parent) return GTEXT_TOML_E_STATE;
  for (const GTEXT_TOML_Value * up = container; up; up = up->parent) {
    if (up == value) return GTEXT_TOML_E_STATE;
  }
  /* One tree, one allocator: gtext_toml_free() reads the root's and frees
   * every node through it, so a subtree from a different allocator would be
   * released through the wrong one - the failure `make check-allocators`
   * exists to prevent, arriving from the caller's side instead.
   *
   * NULL and gtext_allocator_default() are the same allocator spelled two
   * ways, and a node keeps whichever spelling it was made with, so the
   * comparison has to be of what they resolve to. Comparing the stored
   * pointers would refuse a perfectly good pair of nodes for having been
   * constructed by two callers who each read the documentation. */
  const GTEXT_Allocator * a =
      container->alloc ? container->alloc : gtext_allocator_default();
  const GTEXT_Allocator * b =
      value->alloc ? value->alloc : gtext_allocator_default();
  if (a != b) return GTEXT_TOML_E_INVALID;
  return GTEXT_TOML_OK;
}

GTEXT_TOML_Status gtext_toml_table_set(GTEXT_TOML_Value * table,
    const char * key, size_t key_len, GTEXT_TOML_Value * value) {
  if (!table || !value || (!key && key_len)) return GTEXT_TOML_E_INVALID;
  if (table->type != GTEXT_TOML_TABLE) return GTEXT_TOML_E_INVALID;
  GTEXT_TOML_Status bad = toml_check_link(table, value);
  if (bad != GTEXT_TOML_OK) return bad;
  if (toml_table_find(table, key ? key : "", key_len)) {
    return GTEXT_TOML_E_DUPKEY;
  }
  if (!toml_table_insert(table->alloc, table, key ? key : "", key_len, value)) {
    return GTEXT_TOML_E_OOM;
  }
  return GTEXT_TOML_OK;
}

GTEXT_TOML_Status gtext_toml_array_append(
    GTEXT_TOML_Value * array, GTEXT_TOML_Value * value) {
  if (!array || !value) return GTEXT_TOML_E_INVALID;
  if (array->type != GTEXT_TOML_ARRAY) return GTEXT_TOML_E_INVALID;
  GTEXT_TOML_Status bad = toml_check_link(array, value);
  if (bad != GTEXT_TOML_OK) return bad;
  if (!toml_array_push(array->alloc, array, value)) return GTEXT_TOML_E_OOM;
  return GTEXT_TOML_OK;
}

GTEXT_TOML_Status gtext_toml_value_set_inline(
    GTEXT_TOML_Value * value, bool inline_style) {
  if (!value) return GTEXT_TOML_E_INVALID;
  if (value->type == GTEXT_TOML_TABLE) {
    /* Not IMPLICIT or DOTTED on the way back: both of those are how a table
     * came to exist during a parse, and neither is a spelling a writer can
     * choose. HEADER is the non-inline spelling. */
    value->origin =
        inline_style ? (unsigned char) TOML_TABLE_INLINE
                     : (unsigned char) TOML_TABLE_HEADER;
    return GTEXT_TOML_OK;
  }
  if (value->type == GTEXT_TOML_ARRAY) {
    value->origin =
        inline_style ? (unsigned char) TOML_ARRAY_STATIC
                     : (unsigned char) TOML_ARRAY_OF_TABLES;
    return GTEXT_TOML_OK;
  }
  return GTEXT_TOML_E_STATE;
}

/*==========================================================================*
 * Comments
 *
 * Three slots behind one pointer, so that a node with no comment on it costs
 * one pointer and not three strings' worth of NULLs.
 *==========================================================================*/

/**
 * The slot a comment kind lives in, allocating the block on first use.
 *
 * @return The slot, or NULL if the block could not be allocated.
 */
char ** toml_comment_slot(GTEXT_TOML_Value * value, toml_comment_kind which) {
  if (!value->comments) {
    value->comments =
        gtext_allocator_calloc(value->alloc, 1, sizeof(toml_comments));
    if (!value->comments) return NULL;
  }
  switch (which) {
    case TOML_COMMENT_LEADING: return &value->comments->leading;
    case TOML_COMMENT_INLINE: return &value->comments->trailing_inline;
    default: return &value->comments->trailing;
  }
}

bool toml_comment_store(const GTEXT_Allocator * alloc, char ** slot,
    const char * text, size_t len) {
  char * copy = NULL;
  if (text) {
    copy = gtext_allocator_malloc(alloc, len + 1);
    if (!copy) return false;
    if (len) memcpy(copy, text, len);
    copy[len] = '\0';
  }
  gtext_allocator_free(alloc, *slot);
  *slot = copy;
  return true;
}

/** Read a slot without allocating anything, for the accessors. */
static const char * comment_of(
    const GTEXT_TOML_Value * value, toml_comment_kind which) {
  if (!value || !value->comments) return NULL;
  switch (which) {
    case TOML_COMMENT_LEADING: return value->comments->leading;
    case TOML_COMMENT_INLINE: return value->comments->trailing_inline;
    default: return value->comments->trailing;
  }
}

const char * gtext_toml_value_leading_comment(const GTEXT_TOML_Value * value) {
  return comment_of(value, TOML_COMMENT_LEADING);
}

const char * gtext_toml_value_inline_comment(const GTEXT_TOML_Value * value) {
  return comment_of(value, TOML_COMMENT_INLINE);
}

const char * gtext_toml_value_trailing_comment(const GTEXT_TOML_Value * value) {
  return comment_of(value, TOML_COMMENT_TRAILING);
}

/** The three setters, which differ only in which slot they write. */
static GTEXT_TOML_Status set_comment(
    GTEXT_TOML_Value * value, toml_comment_kind which, const char * comment) {
  if (!value) return GTEXT_TOML_E_INVALID;
  /* Removing a comment from a node that has none must not allocate the block
   * to store a NULL in it: `set(NULL)` on an untouched node is how a caller
   * clears what may or may not be there, and it has no reason to fail. */
  if (!comment && !value->comments) return GTEXT_TOML_OK;
  char ** slot = toml_comment_slot(value, which);
  if (!slot) return GTEXT_TOML_E_OOM;
  if (!toml_comment_store(
          value->alloc, slot, comment, comment ? strlen(comment) : 0)) {
    return GTEXT_TOML_E_OOM;
  }
  return GTEXT_TOML_OK;
}

GTEXT_TOML_Status gtext_toml_value_set_leading_comment(
    GTEXT_TOML_Value * value, const char * comment) {
  return set_comment(value, TOML_COMMENT_LEADING, comment);
}

GTEXT_TOML_Status gtext_toml_value_set_inline_comment(
    GTEXT_TOML_Value * value, const char * comment) {
  return set_comment(value, TOML_COMMENT_INLINE, comment);
}

GTEXT_TOML_Status gtext_toml_value_set_trailing_comment(
    GTEXT_TOML_Value * value, const char * comment) {
  return set_comment(value, TOML_COMMENT_TRAILING, comment);
}
