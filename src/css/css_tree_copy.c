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
 * A copy of the parts of a syntax tree into another tree (cssom_rule_internal.h): what a compaction of a CSSOM sheet needs, to give the memory of a tree back while some of its
 * rules live on. A node is copied with everything under it (its strings, its component values, its declarations, its rules), the copy has ids of its own, and the spans keep the
 * numbers they had. Nothing recurses: the nodes still to be filled are on explicit stacks.
 */

#include <string.h>

#include "css_internal.h"
#include "cssom_rule_internal.h"

GTEXT_CSS_Status css_tree_new_empty(const GTEXT_Allocator * alloc, GTEXT_CSS_Tree ** out) {
  css_arena arena;
  css_arena_init(&arena, alloc);
  GTEXT_CSS_Tree * tree = css_arena_alloc(&arena, sizeof *tree);
  struct GTEXT_CSS_Tree_Internal * internal = tree ? css_arena_alloc(&arena, sizeof *internal) : NULL;
  if (!tree || !internal) {
    css_arena_free(&arena);
    return GTEXT_CSS_E_OOM;
  }
  memset(tree, 0, sizeof *tree);
  memset(internal, 0, sizeof *internal);
  internal->arena = arena;   /* the tree and its private part are in the arena, which is copied to its place: only the chunks it points at are the memory */
  gcu_atomic_int_init(&internal->refs, 1);
  tree->internal = internal;
  tree->kind = GTEXT_CSS_TREE_STYLESHEET;
  *out = tree;
  return GTEXT_CSS_OK;
}

typedef struct {
  const GTEXT_CSS_Rule * src;
  GTEXT_CSS_Rule * dst;
} rule_pair;

typedef struct {
  const GTEXT_CSS_Component_Value * src;
  GTEXT_CSS_Component_Value * dst;
  size_t n;
} value_pair;

typedef struct {
  GTEXT_CSS_Tree * tree;
  css_arena * arena;
  GTEXT_Allocator temp;     // where the stacks come from
  rule_pair * rules;
  size_t nrules, caprules;
  value_pair * values;
  size_t nvalues, capvalues;
  GTEXT_CSS_Status st;
} copier;

static void cp_fail(copier * c, GTEXT_CSS_Status st) {
  if (c->st == GTEXT_CSS_OK) {
    c->st = st;
  }
}

static GTEXT_CSS_Id next_id(copier * c) {
  return ++c->tree->node_count;
}

static void cp_string(copier * c, GTEXT_CSS_String * dst, const GTEXT_CSS_String * src) {
  if (!src->data) {
    dst->data = NULL;
    dst->length = 0;
    return;
  }
  char * s = css_arena_strdup(c->arena, src->data, src->length);
  if (!s) {
    cp_fail(c, GTEXT_CSS_E_OOM);
    dst->data = NULL;
    dst->length = 0;
    return;
  }
  dst->data = s;
  dst->length = src->length;
}

static void push_values(copier * c, const GTEXT_CSS_Component_Value * src, GTEXT_CSS_Component_Value * dst, size_t n) {
  if (c->nvalues == c->capvalues) {
    const size_t cap = c->capvalues ? c->capvalues * 2 : 16;
    value_pair * grown = gtext_allocator_realloc(&c->temp, c->values, cap * sizeof *grown);
    if (!grown) {
      cp_fail(c, GTEXT_CSS_E_OOM);
      return;
    }
    c->values = grown;
    c->capvalues = cap;
  }
  c->values[c->nvalues++] = (value_pair){src, dst, n};
}

static void push_rule(copier * c, const GTEXT_CSS_Rule * src, GTEXT_CSS_Rule * dst) {
  if (c->nrules == c->caprules) {
    const size_t cap = c->caprules ? c->caprules * 2 : 16;
    rule_pair * grown = gtext_allocator_realloc(&c->temp, c->rules, cap * sizeof *grown);
    if (!grown) {
      cp_fail(c, GTEXT_CSS_E_OOM);
      return;
    }
    c->rules = grown;
    c->caprules = cap;
  }
  c->rules[c->nrules++] = (rule_pair){src, dst};
}

/// A new array of `n` component values with the contents of `src`; the children are filled later, from the stack.
static GTEXT_CSS_Component_Value * cp_values(copier * c, const GTEXT_CSS_Component_Value * src, size_t n) {
  if (!src || n == 0) {
    return NULL;
  }
  GTEXT_CSS_Component_Value * dst = css_arena_alloc(c->arena, n * sizeof *dst);
  if (!dst) {
    cp_fail(c, GTEXT_CSS_E_OOM);
    return NULL;
  }
  push_values(c, src, dst, n);
  return dst;
}

/// Fill one array that cp_values() made: copy each value, its strings, and give it an id; its children get their arrays, to be filled in their turn.
static void fill_values(copier * c, const GTEXT_CSS_Component_Value * src, GTEXT_CSS_Component_Value * dst, size_t n) {
  for (size_t i = 0; i < n; i++) {
    dst[i] = src[i];
    dst[i].id = next_id(c);
    cp_string(c, &dst[i].value, &src[i].value);
    cp_string(c, &dst[i].unit, &src[i].unit);
    cp_string(c, &dst[i].repr, &src[i].repr);
    dst[i].children = src[i].child_count ? cp_values(c, src[i].children, src[i].child_count) : NULL;
    if (!dst[i].children) {
      dst[i].child_count = 0;
    }
  }
}

static void drain_values(copier * c) {
  while (c->nvalues && c->st == GTEXT_CSS_OK) {
    const value_pair p = c->values[--c->nvalues];
    fill_values(c, p.src, p.dst, p.n);
  }
}

static GTEXT_CSS_Declaration * cp_declarations(copier * c, const GTEXT_CSS_Declaration * src, size_t n) {
  if (!src || n == 0) {
    return NULL;
  }
  GTEXT_CSS_Declaration * dst = css_arena_alloc(c->arena, n * sizeof *dst);
  if (!dst) {
    cp_fail(c, GTEXT_CSS_E_OOM);
    return NULL;
  }
  for (size_t i = 0; i < n; i++) {
    dst[i] = src[i];
    dst[i].id = next_id(c);
    cp_string(c, &dst[i].name, &src[i].name);
    cp_string(c, &dst[i].original_text, &src[i].original_text);
    dst[i].value = cp_values(c, src[i].value, src[i].value_count);
    if (!dst[i].value) {
      dst[i].value_count = 0;
    }
  }
  return dst;
}

/// Fill one rule from its original: the node itself, and the arrays under it; the rules under it are pushed for their turn.
static void fill_rule(copier * c, const GTEXT_CSS_Rule * src, GTEXT_CSS_Rule * dst) {
  *dst = *src;
  dst->id = next_id(c);
  cp_string(c, &dst->name, &src->name);
  dst->prelude = cp_values(c, src->prelude, src->prelude_count);
  if (!dst->prelude) {
    dst->prelude_count = 0;
  }
  dst->declarations = cp_declarations(c, src->declarations, src->declaration_count);
  if (!dst->declarations) {
    dst->declaration_count = 0;
  }
  dst->rules = NULL;
  dst->rule_count = 0;
  if (src->rule_count) {
    GTEXT_CSS_Rule * kids = css_arena_alloc(c->arena, src->rule_count * sizeof *kids);
    if (!kids) {
      cp_fail(c, GTEXT_CSS_E_OOM);
      return;
    }
    dst->rules = kids;
    dst->rule_count = src->rule_count;
    for (size_t i = 0; i < src->rule_count; i++) {
      push_rule(c, &src->rules[i], &kids[i]);
    }
  }
  dst->contents = NULL;
  dst->content_count = 0;
  if (src->content_count) {
    size_t nrules = 0;
    for (size_t i = 0; i < src->content_count; i++) {
      nrules += src->contents[i].kind == GTEXT_CSS_ITEM_RULE;
    }
    GTEXT_CSS_Rule * kids = nrules ? css_arena_alloc(c->arena, nrules * sizeof *kids) : NULL;
    GTEXT_CSS_Block_Item * items = css_arena_alloc(c->arena, src->content_count * sizeof *items);
    if ((nrules && !kids) || !items) {
      cp_fail(c, GTEXT_CSS_E_OOM);
      return;
    }
    size_t r = 0;
    for (size_t i = 0; i < src->content_count; i++) {
      items[i] = src->contents[i];
      if (src->contents[i].kind == GTEXT_CSS_ITEM_RULE) {
        items[i].rule = &kids[r];
        push_rule(c, src->contents[i].rule, &kids[r]);
        r++;
      }
      else {
        items[i].declarations = cp_declarations(c, src->contents[i].declarations, src->contents[i].declaration_count);
        if (!items[i].declarations) {
          items[i].declaration_count = 0;
        }
      }
    }
    dst->contents = items;
    dst->content_count = src->content_count;
  }
}

static GTEXT_CSS_Status copier_run(copier * c) {
  while (c->st == GTEXT_CSS_OK && (c->nrules || c->nvalues)) {
    if (c->nvalues) {
      drain_values(c);
      continue;
    }
    const rule_pair p = c->rules[--c->nrules];
    fill_rule(c, p.src, p.dst);
  }
  const GTEXT_CSS_Status st = c->st;
  gtext_allocator_free(&c->temp, c->rules);
  gtext_allocator_free(&c->temp, c->values);
  return st;
}

GTEXT_CSS_Status css_tree_copy_rule(GTEXT_CSS_Tree * dst, const GTEXT_CSS_Rule * src, const GTEXT_CSS_Rule ** out) {
  copier c;
  memset(&c, 0, sizeof c);
  c.tree = dst;
  c.arena = &dst->internal->arena;
  c.temp = c.arena->alloc;
  GTEXT_CSS_Rule * root = css_arena_alloc(c.arena, sizeof *root);
  if (!root) {
    return GTEXT_CSS_E_OOM;
  }
  push_rule(&c, src, root);
  const GTEXT_CSS_Status st = copier_run(&c);
  if (st == GTEXT_CSS_OK) {
    *out = root;
  }
  return st;
}

GTEXT_CSS_Status css_tree_copy_run(GTEXT_CSS_Tree * dst, const GTEXT_CSS_Declaration * src, size_t count, const GTEXT_CSS_Declaration ** out) {
  copier c;
  memset(&c, 0, sizeof c);
  c.tree = dst;
  c.arena = &dst->internal->arena;
  c.temp = c.arena->alloc;
  GTEXT_CSS_Declaration * run = cp_declarations(&c, src, count);
  const GTEXT_CSS_Status st = copier_run(&c);
  if (st == GTEXT_CSS_OK) {
    *out = run;
  }
  return st;
}
