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
 * @brief Building and reading a parsed TOML document, and releasing it.
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
