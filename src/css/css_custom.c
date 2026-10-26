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
 * The persistent map of custom properties (css_custom.h).
 */

#include "css_custom.h"

#include <ghoti.io/cutil/atomic.h>

#include <stdint.h>
#include <string.h>

#include "css_value_internal.h"

#define PATH_MAX_DEPTH 256

struct css_cmap {
  GCU_Atomic_Int refs;
  GTEXT_Allocator alloc;
  struct css_cmap * left;       ///< Owned (one reference).
  struct css_cmap * right;
  char * name;                  ///< Owned; the bytes of the name, NUL-terminated.
  size_t length;
  GTEXT_CSS_Value_Set * value;  ///< Retained; NULL for a name that is hidden.
  size_t live;                  ///< Names with a value in this subtree.
  uint32_t priority;
};

static uint32_t name_priority(const char * name, size_t length) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < length; i++) {
    h = (h ^ (unsigned char)name[i]) * 16777619u;
  }
  h ^= h >> 15;
  h *= 2246822519u;
  h ^= h >> 13;
  return h;
}

static int compare_names(const char * a, size_t alen, const char * b, size_t blen) {
  const size_t n = alen < blen ? alen : blen;
  const int c = n ? memcmp(a, b, n) : 0;
  if (c) {
    return c < 0 ? -1 : 1;
  }
  return alen < blen ? -1 : alen > blen ? 1 : 0;
}

css_cmap * css_cmap_retain(css_cmap * map) {
  if (map) {
    (void)gcu_atomic_int_fetch_add(&map->refs, 1);
  }
  return map;
}

void css_cmap_release(css_cmap * map) {
  if (!map) {
    return;
  }
  /* an explicit stack: a node whose count reaches zero hands its two children to the stack */
  css_cmap * stack[PATH_MAX_DEPTH * 2];
  size_t sp = 0;
  stack[sp++] = map;
  while (sp) {
    css_cmap * n = stack[--sp];
    if (!n || gcu_atomic_int_fetch_sub(&n->refs, 1) != 1) {
      continue;
    }
    /* the two children may not fit when the stack is nearly full: release those that do not as they come (the depth is logarithmic, so this is
       not reached with names that are not a deliberate attack on the hash) */
    css_cmap * kids[2] = {n->left, n->right};
    GTEXT_Allocator a = n->alloc;
    gtext_allocator_free(&a, n->name);
    gtext_css_value_set_free(n->value);
    gtext_allocator_free(&a, n);
    for (int k = 0; k < 2; k++) {
      if (!kids[k]) {
        continue;
      }
      if (sp < sizeof stack / sizeof *stack) {
        stack[sp++] = kids[k];
      }
      else {
        css_cmap_release(kids[k]);
      }
    }
  }
}

size_t css_cmap_count(const css_cmap * map) {
  return map ? map->live : 0;
}

const GTEXT_CSS_Value_Set * css_cmap_find(const css_cmap * map, const char * name, size_t length) {
  while (map) {
    const int c = compare_names(name, length, map->name, map->length);
    if (c == 0) {
      return map->value;
    }
    map = c < 0 ? map->left : map->right;
  }
  return NULL;
}

bool css_cmap_at(const css_cmap * map, size_t index, const char ** name, size_t * length, const GTEXT_CSS_Value_Set ** value) {
  if (!map || index >= map->live) {
    return false;
  }
  for (;;) {
    const size_t left = map->left ? map->left->live : 0;
    if (index < left) {
      map = map->left;
      continue;
    }
    index -= left;
    if (map->value) {
      if (index == 0) {
        if (name) {
          *name = map->name;
        }
        if (length) {
          *length = map->length;
        }
        if (value) {
          *value = map->value;
        }
        return true;
      }
      index--;
    }
    map = map->right;
    if (!map) {
      return false;
    }
  }
}

/// A node with the given name and value (retained), taking over the two references `left` and `right`. NULL on memory (the references are released).
static css_cmap * make(const GTEXT_Allocator * alloc, const char * name, size_t length, GTEXT_CSS_Value_Set * value, css_cmap * left, css_cmap * right) {
  css_cmap * n = gtext_allocator_calloc(alloc, 1, sizeof *n);
  char * copy = n ? gtext_allocator_malloc(alloc, length + 1) : NULL;
  if (!n || !copy) {
    gtext_allocator_free(alloc, n);
    css_cmap_release(left);
    css_cmap_release(right);
    return NULL;
  }
  memcpy(copy, name, length);
  copy[length] = '\0';
  gcu_atomic_int_init(&n->refs, 1);
  n->alloc = *alloc;
  n->left = left;
  n->right = right;
  n->name = copy;
  n->length = length;
  n->value = value ? gtext_css_value_set_retain(value) : NULL;
  n->live = (value ? 1 : 0) + (left ? left->live : 0) + (right ? right->live : 0);
  n->priority = name_priority(name, length);
  return n;
}

GTEXT_CSS_Status css_cmap_set(const GTEXT_Allocator * alloc, css_cmap * map, const char * name, size_t length, GTEXT_CSS_Value_Set * value, css_cmap ** out) {
  /* the path from the root to the place of the name: the nodes passed, and the way taken at each */
  const css_cmap * path[PATH_MAX_DEPTH];
  bool went_left[PATH_MAX_DEPTH];
  size_t depth = 0;
  const css_cmap * at = map;
  const css_cmap * found = NULL;
  while (at) {
    const int c = compare_names(name, length, at->name, at->length);
    if (c == 0) {
      found = at;
      break;
    }
    if (depth == PATH_MAX_DEPTH) {
      return GTEXT_CSS_E_LIMIT_DEPTH;
    }
    path[depth] = at;
    went_left[depth] = c < 0;
    depth++;
    at = c < 0 ? at->left : at->right;
  }
  css_cmap * current;
  if (found) {
    current = make(alloc, name, length, value, css_cmap_retain(found->left), css_cmap_retain(found->right));
  }
  else {
    current = make(alloc, name, length, value, NULL, NULL);
  }
  if (!current) {
    return GTEXT_CSS_E_OOM;
  }
  while (depth > 0) {
    depth--;
    const css_cmap * p = path[depth];
    if (went_left[depth]) {
      if (current->priority > p->priority) {
        /* rotate right: the new subtree's root takes the parent's place and the parent becomes its right child */
        css_cmap * lowered = make(alloc, p->name, p->length, p->value, css_cmap_retain(current->right), css_cmap_retain(p->right));
        css_cmap * top = lowered ? make(alloc, current->name, current->length, current->value, css_cmap_retain(current->left), lowered) : NULL;
        css_cmap_release(current);
        if (!top) {
          return GTEXT_CSS_E_OOM;
        }
        current = top;
      }
      else {
        css_cmap * up = make(alloc, p->name, p->length, p->value, current, css_cmap_retain(p->right));
        if (!up) {
          return GTEXT_CSS_E_OOM;
        }
        current = up;
      }
    }
    else if (current->priority > p->priority) {
      css_cmap * lowered = make(alloc, p->name, p->length, p->value, css_cmap_retain(p->left), css_cmap_retain(current->left));
      css_cmap * top = lowered ? make(alloc, current->name, current->length, current->value, lowered, css_cmap_retain(current->right)) : NULL;
      css_cmap_release(current);
      if (!top) {
        return GTEXT_CSS_E_OOM;
      }
      current = top;
    }
    else {
      css_cmap * up = make(alloc, p->name, p->length, p->value, css_cmap_retain(p->left), current);
      if (!up) {
        return GTEXT_CSS_E_OOM;
      }
      current = up;
    }
  }
  *out = current;
  return GTEXT_CSS_OK;
}
