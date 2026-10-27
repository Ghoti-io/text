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
 * The persistent map of custom properties (css_cmap.h).
 */

#include "css_cmap.h"

#include <ghoti.io/cutil/atomic.h>

#include <stdint.h>
#include <string.h>


#define PATH_MAX_DEPTH 256

struct css_cmap {
  GCU_Atomic_Int refs;
  GTEXT_Allocator alloc;
  struct css_cmap * left;       ///< Owned (one reference).
  struct css_cmap * right;
  char * name;                  ///< Owned; the bytes of the name, NUL-terminated.
  size_t length;
  GTEXT_CSS_Custom_Value * value;  ///< Retained; NULL for a name that is hidden.
  size_t live;                  ///< Names with a value in this subtree.
  uint32_t priority;
};

/// The priority of a name: a keyed hash (the key is the context's seed, which an author of a stylesheet does not know), so that no choice of names makes the
/// tree deep. A fixed hash would let a few hundred names be chosen to chain, and the depth is bounded.
static uint32_t name_priority(const char * name, size_t length, uint64_t seed) {
  uint64_t h = 0xcbf29ce484222325ULL ^ seed;
  for (size_t i = 0; i < length; i++) {
    h = (h ^ (unsigned char)name[i]) * 0x100000001b3ULL;
  }
  /* splitmix64 finalizer, with the key again: a bijection, so equal names (and only those, up to collisions) share a priority */
  h += seed + 0x9E3779B97F4A7C15ULL;
  h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ULL;
  h = (h ^ (h >> 27)) * 0x94D049BB133111EBULL;
  h ^= h >> 31;
  return (uint32_t)(h >> 32);
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
    gtext_css_custom_value_free(n->value);
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

uint64_t css_cmap_shape(const css_cmap * map, size_t * height) {
  const css_cmap * stack[PATH_MAX_DEPTH * 2];
  size_t depth_of[PATH_MAX_DEPTH * 2];
  size_t sp = 0, tall = 0;
  uint64_t h = 1469598103934665603ULL;
  if (map) {
    stack[sp] = map;
    depth_of[sp++] = 1;
  }
  while (sp) {
    const css_cmap * n = stack[--sp];
    const size_t d = depth_of[sp];
    if (d > tall) {
      tall = d;
    }
    for (size_t i = 0; i < n->length; i++) {
      h = (h ^ (unsigned char)n->name[i]) * 1099511628211ULL;
    }
    h = (h ^ (uint64_t)d) * 1099511628211ULL;
    h = (h ^ (uint64_t)n->priority) * 1099511628211ULL;
    h = (h ^ (n->value ? 1u : 2u)) * 1099511628211ULL;
    for (int k = 0; k < 2; k++) {
      const css_cmap * kid = k ? n->right : n->left;
      if (kid) {
        if (sp == sizeof stack / sizeof *stack) {
          if (height) {
            *height = (size_t)-1;
          }
          return 0;
        }
        stack[sp] = kid;
        depth_of[sp++] = d + 1;
      }
    }
  }
  if (height) {
    *height = tall;
  }
  return h;
}

size_t css_cmap_count(const css_cmap * map) {
  return map ? map->live : 0;
}

const GTEXT_CSS_Custom_Value * css_cmap_find(const css_cmap * map, const char * name, size_t length) {
  while (map) {
    const int c = compare_names(name, length, map->name, map->length);
    if (c == 0) {
      return map->value;
    }
    map = c < 0 ? map->left : map->right;
  }
  return NULL;
}

size_t css_cmap_rank(const css_cmap * map, const char * name, size_t length) {
  size_t rank = 0;
  while (map) {
    if (compare_names(name, length, map->name, map->length) <= 0) {
      map = map->left;
    }
    else {
      rank += (map->left ? map->left->live : 0) + (map->value ? 1 : 0);
      map = map->right;
    }
  }
  return rank;
}

bool css_cmap_at(const css_cmap * map, size_t index, const char ** name, size_t * length, const GTEXT_CSS_Custom_Value ** value) {
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
static css_cmap * make(const GTEXT_Allocator * alloc, const char * name, size_t length, GTEXT_CSS_Custom_Value * value, css_cmap * left, css_cmap * right,
    uint32_t priority) {
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
  n->value = value ? gtext_css_custom_value_retain(value) : NULL;
  n->live = (value ? 1 : 0) + (left ? left->live : 0) + (right ? right->live : 0);
  n->priority = priority;
  return n;
}

GTEXT_CSS_Status css_cmap_set(const GTEXT_Allocator * alloc, css_cmap * map, const char * name, size_t length, GTEXT_CSS_Custom_Value * value, uint64_t seed,
    css_cmap ** out, size_t * work) {
  /* the path from the root to the place of the name: the nodes passed, and the way taken at each */
  const css_cmap * path[PATH_MAX_DEPTH];
  bool went_left[PATH_MAX_DEPTH];
  size_t depth = 0;
  size_t steps = 1; /* the nodes looked at, and below the nodes made */
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
    steps++;
    at = c < 0 ? at->left : at->right;
  }
  css_cmap * current;
  if (found) {
    current = make(alloc, name, length, value, css_cmap_retain(found->left), css_cmap_retain(found->right), found->priority);
  }
  else {
    current = make(alloc, name, length, value, NULL, NULL, name_priority(name, length, seed));
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
        css_cmap * lowered = make(alloc, p->name, p->length, p->value, css_cmap_retain(current->right), css_cmap_retain(p->right), p->priority);
        css_cmap * top = lowered ? make(alloc, current->name, current->length, current->value, css_cmap_retain(current->left), lowered, current->priority) : NULL;
        css_cmap_release(current);
        if (!top) {
          return GTEXT_CSS_E_OOM;
        }
        current = top;
      }
      else {
        css_cmap * up = make(alloc, p->name, p->length, p->value, current, css_cmap_retain(p->right), p->priority);
        if (!up) {
          return GTEXT_CSS_E_OOM;
        }
        current = up;
      }
    }
    else if (current->priority > p->priority) {
      css_cmap * lowered = make(alloc, p->name, p->length, p->value, css_cmap_retain(p->left), css_cmap_retain(current->left), p->priority);
      css_cmap * top = lowered ? make(alloc, current->name, current->length, current->value, lowered, css_cmap_retain(current->right), current->priority) : NULL;
      css_cmap_release(current);
      if (!top) {
        return GTEXT_CSS_E_OOM;
      }
      current = top;
    }
    else {
      css_cmap * up = make(alloc, p->name, p->length, p->value, css_cmap_retain(p->left), current, p->priority);
      if (!up) {
        return GTEXT_CSS_E_OOM;
      }
      current = up;
    }
  }
  *out = current;
  if (work) {
    *work += 2 * steps; /* the path read, and the path made again */
  }
  return GTEXT_CSS_OK;
}
