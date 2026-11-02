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
 * The rule list (cssom_rule_internal.h): an order-statistics treap whose nodes are the rules themselves, with the priority taken from a hash of the rule's id. Every operation
 * is iterative: the rotations are done bottom-up through the parent pointers, so a list of a million rules never recurses on the C stack.
 */

#include <string.h>

#include "cssom_rule_internal.h"

/// A hash of the id with fixed constants: the same on every host, and nothing like an address.
static uint32_t priority_of(uint64_t id) {
  uint64_t x = id + 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  x ^= x >> 31;
  return (uint32_t)(x >> 32);
}

static size_t size_of(const css_rule * n) {
  return n ? n->size : 0;
}

static void update(css_rule * n) {
  n->size = 1 + size_of(n->left) + size_of(n->right);
}

/// Rotate `x` above its parent, keeping the order and the sizes.
static void rotate_up(struct GTEXT_CSSOM_Rule_List * list, css_rule * x) {
  css_rule * p = x->up;
  css_rule * g = p->up;
  if (p->left == x) {
    p->left = x->right;
    if (x->right) {
      x->right->up = p;
    }
    x->right = p;
  }
  else {
    p->right = x->left;
    if (x->left) {
      x->left->up = p;
    }
    x->left = p;
  }
  p->up = x;
  x->up = g;
  if (!g) {
    list->root = x;
  }
  else if (g->left == p) {
    g->left = x;
  }
  else {
    g->right = x;
  }
  update(p);
  update(x);
}

void css_rule_list_insert(struct GTEXT_CSSOM_Rule_List * list, size_t index, css_rule * rule) {
  rule->left = rule->right = rule->up = NULL;
  rule->size = 1;
  rule->prio = priority_of(rule->obj.id);
  rule->list = list;
  if (index > list->count) {
    index = list->count;
  }
  if (!list->root) {
    list->root = rule;
  }
  else {
    css_rule * n = list->root;
    size_t at = index;
    for (;;) {
      const size_t before = size_of(n->left);
      if (at <= before) {
        if (!n->left) {
          n->left = rule;
          break;
        }
        n = n->left;
      }
      else {
        at -= before + 1;
        if (!n->right) {
          n->right = rule;
          break;
        }
        n = n->right;
      }
    }
    rule->up = n;
    for (css_rule * a = n; a; a = a->up) {
      a->size++;
    }
    while (rule->up && rule->prio > rule->up->prio) {
      rotate_up(list, rule);
    }
  }
  list->count++;
  css_obj_set_parent(&rule->obj, list->owner);
}

void css_rule_list_remove(struct GTEXT_CSSOM_Rule_List * list, css_rule * rule) {
  /* rotate it down to a leaf, always lifting the child with the higher priority, then cut it off */
  while (rule->left || rule->right) {
    css_rule * child;
    if (!rule->left) {
      child = rule->right;
    }
    else if (!rule->right) {
      child = rule->left;
    }
    else {
      child = rule->left->prio > rule->right->prio ? rule->left : rule->right;
    }
    rotate_up(list, child);
  }
  css_rule * p = rule->up;
  if (!p) {
    list->root = NULL;
  }
  else {
    if (p->left == rule) {
      p->left = NULL;
    }
    else {
      p->right = NULL;
    }
    for (css_rule * a = p; a; a = a->up) {
      a->size--;
    }
  }
  rule->up = rule->left = rule->right = NULL;
  rule->size = 1;
  rule->list = NULL;
  list->count--;
  css_obj_set_parent(&rule->obj, NULL);
}

css_rule * css_rule_list_at(const struct GTEXT_CSSOM_Rule_List * list, size_t index) {
  css_rule * n = list->root;
  while (n) {
    const size_t before = size_of(n->left);
    if (index < before) {
      n = n->left;
    }
    else if (index == before) {
      return n;
    }
    else {
      index -= before + 1;
      n = n->right;
    }
  }
  return NULL;
}

css_rule * css_rule_list_first(const struct GTEXT_CSSOM_Rule_List * list) {
  css_rule * n = list->root;
  while (n && n->left) {
    n = n->left;
  }
  return n;
}

css_rule * css_rule_list_next(const css_rule * rule) {
  if (rule->right) {
    css_rule * n = rule->right;
    while (n->left) {
      n = n->left;
    }
    return n;
  }
  const css_rule * x = rule;
  while (x->up && x->up->right == x) {
    x = x->up;
  }
  return x->up;
}

size_t css_rule_list_index_of(const css_rule * rule) {
  size_t index = size_of(rule->left);
  for (const css_rule * x = rule; x->up; x = x->up) {
    if (x->up->right == x) {
      index += size_of(x->up->left) + 1;
    }
  }
  return index;
}

size_t css_rule_list_depth(const struct GTEXT_CSSOM_Rule_List * list) {
  /* a walk over the whole treap by the parent pointers, counting the depth of the node it is at */
  size_t best = 0, depth = 0;
  const css_rule * n = list->root;
  const css_rule * prev = NULL;
  while (n) {
    const css_rule * up = n->up;
    const css_rule * next;
    if (prev == up) {   /* arrived from above */
      depth++;
      if (depth > best) {
        best = depth;
      }
      next = n->left ? n->left : (n->right ? n->right : up);
    }
    else if (prev == n->left) {
      next = n->right ? n->right : up;
    }
    else {
      next = up;
    }
    if (next == up) {
      depth--;
    }
    prev = n;
    n = next;
  }
  return best;
}

void css_rule_list_release_all(struct GTEXT_CSSOM_Rule_List * list) {
  /* release in order, from the leftmost: a node is released after its predecessors, and no pointer is read from a node after its release is asked (a released rule is only
     queued to be freed, but the walk reads `up` and `right` before it asks) */
  css_rule * n = css_rule_list_first(list);
  while (n) {
    css_rule * next = css_rule_list_next(n);
    n->list = NULL;
    css_obj_release_internal(&n->obj);
    n = next;
  }
  list->root = NULL;
  list->count = 0;
}
