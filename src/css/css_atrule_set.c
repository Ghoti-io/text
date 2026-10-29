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
 * The at-rules of a document's sheets in cascade order (css_atrule_set.h). Rules are collected by an iterative walk (no recursion), typed once at freeze, and found through
 * sorted indexes (O(log n) a find, O(n log n) a freeze).
 */

#include <ghoti.io/unicode/case.h>

#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/css/css_atrule_set.h>

#include "css_atrule_internal.h"

typedef struct {
  GTEXT_CSS_Atrule * rule;
  GTEXT_CSS_Atrule_Kind kind;
  GTEXT_CSS_Layer_Id layer;
  size_t rank;
  size_t order;
  size_t tree;
  void * typed;       ///< The face, counter style, keyframes, feature values, palette or color profile (owned).
  size_t pos;         ///< The place in cascade order, from freeze.
} entry;

typedef struct {
  const char * key;
  size_t key_len;
  const char * key2;  ///< A palette's family key.
  size_t key2_len;
  size_t pos;
  size_t entry;
} name_ref;

struct GTEXT_CSS_Atrule_Set {
  GTEXT_Allocator alloc;
  const GTEXT_CSS_Registry * registry;
  GTEXT_CSS_Atrule_Set_Options opt;
  GTEXT_CSS_Layer_Registry * layers;
  entry * ents;
  size_t n, cap;
  size_t * sorted;        ///< Entry indexes in cascade order.
  bool frozen;
  size_t trees;
  holder * h;             ///< The arena of the indexes and the merged objects.
  name_ref * faces; size_t nfaces;
  name_ref * features; size_t nfeatures;
  GTEXT_CSS_Feature_Values ** merged; size_t nmerged;
  name_ref * palettes; size_t npalettes;
  name_ref * profiles; size_t nprofiles;
  name_ref * counters; size_t ncounters;
  name_ref * frames; size_t nframes;
  name_ref * tries; size_t ntries;
  size_t view_transition;  ///< entry index + 1, or 0.
  size_t * pages; size_t npages;
};

GTEXT_CSS_Atrule_Set_Options gtext_css_atrule_set_options_default(void) {
  GTEXT_CSS_Atrule_Set_Options o;
  memset(&o, 0, sizeof o);
  o.origin = GTEXT_CSS_ORIGIN_AUTHOR;
  o.max_rules = 1000000;
  o.max_rule_depth = 4096;
  o.atrule = gtext_css_atrule_options_default();
  return o;
}

GTEXT_CSS_Status gtext_css_atrule_set_new(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Atrule_Set_Options * options, GTEXT_CSS_Atrule_Set ** out) {
  if (!registry || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  *out = NULL;
  GTEXT_CSS_Atrule_Set_Options o = options ? *options : gtext_css_atrule_set_options_default();
  const GTEXT_CSS_Atrule_Set_Options d = gtext_css_atrule_set_options_default();
  if (!o.max_rules) {
    o.max_rules = d.max_rules;
  }
  if (!o.max_rule_depth) {
    o.max_rule_depth = d.max_rule_depth;
  }
  if (!options || !options->atrule.max_descriptors) {
    o.atrule = d.atrule;
  }
  if (o.origin >= GTEXT_CSS_ORIGIN_ANIMATION) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_Allocator a = o.allocator ? *o.allocator : *gtext_allocator_default();
  GTEXT_CSS_Atrule_Set * s = gtext_allocator_calloc(&a, 1, sizeof *s);
  if (!s) {
    return GTEXT_CSS_E_OOM;
  }
  s->alloc = a;
  s->registry = registry;
  s->opt = o;
  s->opt.allocator = &s->alloc;
  s->opt.atrule.allocator = &s->alloc;
  GTEXT_CSS_Layer_Options lo = gtext_css_layer_options_default();
  lo.allocator = &s->alloc;
  s->h = holder_new(&s->alloc);
  if (!s->h || gtext_css_layer_registry_new(&lo, &s->layers) != GTEXT_CSS_OK) {
    holder_free(s->h);
    gtext_allocator_free(&a, s);
    return GTEXT_CSS_E_OOM;
  }
  *out = s;
  return GTEXT_CSS_OK;
}

static void free_typed(entry * e) {
  if (!e->typed) {
    return;
  }
  switch (e->kind) {
  case GTEXT_CSS_ATRULE_FONT_FACE:
    gtext_css_font_face_free(e->typed);
    break;
  case GTEXT_CSS_ATRULE_FONT_FEATURE_VALUES:
    gtext_css_feature_values_free(e->typed);
    break;
  case GTEXT_CSS_ATRULE_FONT_PALETTE_VALUES:
    gtext_css_palette_values_free(e->typed);
    break;
  case GTEXT_CSS_ATRULE_COLOR_PROFILE:
    gtext_css_color_profile_free(e->typed);
    break;
  case GTEXT_CSS_ATRULE_COUNTER_STYLE:
    gtext_css_counter_style_free(e->typed);
    break;
  case GTEXT_CSS_ATRULE_KEYFRAMES:
    gtext_css_keyframes_free(e->typed);
    break;
  default:
    break;
  }
  e->typed = NULL;
}

void gtext_css_atrule_set_free(GTEXT_CSS_Atrule_Set * s) {
  if (!s) {
    return;
  }
  const GTEXT_Allocator a = s->alloc;
  for (size_t i = 0; i < s->n; i++) {
    free_typed(&s->ents[i]);
    gtext_css_atrule_free(s->ents[i].rule);
  }
  gtext_allocator_free(&a, s->ents);
  gtext_allocator_free(&a, s->sorted);
  gtext_allocator_free(&a, s->faces);
  gtext_allocator_free(&a, s->features);
  gtext_allocator_free(&a, s->palettes);
  gtext_allocator_free(&a, s->profiles);
  gtext_allocator_free(&a, s->counters);
  gtext_allocator_free(&a, s->frames);
  gtext_allocator_free(&a, s->tries);
  gtext_allocator_free(&a, s->pages);
  gtext_allocator_free(&a, s->merged);
  gtext_css_layer_registry_free(s->layers);
  holder_free(s->h);
  gtext_allocator_free(&a, s);
}

/* ---- collecting ------------------------------------------------------------------------------------------------------------------------ */

typedef struct {
  const GTEXT_CSS_Rule * rules;
  size_t rule_count;
  const GTEXT_CSS_Block_Item * items;
  size_t item_count;
  size_t pos;
  GTEXT_CSS_Layer_Id layer;
  bool unresolved;
} frame;

static bool rname(const GTEXT_CSS_Rule * r, const char * lit) {
  return css_ascii_ieq(&r->name, lit);
}

static bool group_name(const GTEXT_CSS_Rule * r) {
  return rname(r, "media") || rname(r, "supports") || rname(r, "when") || rname(r, "else") || rname(r, "container");
}

static bool collected_kind(GTEXT_CSS_Atrule_Kind k) {
  switch (k) {
  case GTEXT_CSS_ATRULE_FONT_FACE:
  case GTEXT_CSS_ATRULE_FONT_FEATURE_VALUES:
  case GTEXT_CSS_ATRULE_FONT_PALETTE_VALUES:
  case GTEXT_CSS_ATRULE_COLOR_PROFILE:
  case GTEXT_CSS_ATRULE_COUNTER_STYLE:
  case GTEXT_CSS_ATRULE_KEYFRAMES:
  case GTEXT_CSS_ATRULE_VIEW_TRANSITION:
  case GTEXT_CSS_ATRULE_POSITION_TRY:
  case GTEXT_CSS_ATRULE_PAGE:
    return true;
  default:
    return false;
  }
}

static GTEXT_CSS_Status add_entry(GTEXT_CSS_Atrule_Set * s, GTEXT_CSS_Atrule * a, GTEXT_CSS_Layer_Id layer) {
  if (s->n >= s->opt.max_rules) {
    gtext_css_atrule_free(a);
    return GTEXT_CSS_E_LIMIT_DESCRIPTORS;
  }
  if (s->n == s->cap) {
    const size_t nc = s->cap ? s->cap * 2 : 16;
    entry * g = gtext_allocator_realloc(&s->alloc, s->ents, nc * sizeof *g);
    if (!g) {
      gtext_css_atrule_free(a);
      return GTEXT_CSS_E_OOM;
    }
    s->ents = g;
    s->cap = nc;
  }
  entry * e = &s->ents[s->n];
  memset(e, 0, sizeof *e);
  e->rule = a;
  e->kind = gtext_css_atrule_kind(a);
  e->layer = layer;
  e->order = s->n;
  e->tree = s->trees - 1;
  s->n++;
  return GTEXT_CSS_OK;
}

/// Read the names of an `@layer` prelude: each comma separated group is identifiers joined by `.` with nothing else (Cascading 5, 6.4.2). *valid false: the rule is invalid and
/// declares nothing. With `declare`, each name is declared under `parent`; `*inner` is the layer of a block (the one name, or an anonymous layer).
static GTEXT_CSS_Status layer_names(GTEXT_CSS_Atrule_Set * s, const GTEXT_CSS_Rule * r, GTEXT_CSS_Layer_Id parent, bool block, bool declare, GTEXT_CSS_Layer_Id * inner, bool * valid) {
  const GTEXT_CSS_Component_Value * v = r->prelude;
  const size_t n = r->prelude_count;
  *valid = true;
  size_t i = 0;
  while (i < n && v[i].kind == GTEXT_CSS_VALUE_TOKEN && v[i].type == GTEXT_CSS_TOKEN_WHITESPACE) {
    i++;
  }
  size_t end = n;
  while (end > i && v[end - 1].kind == GTEXT_CSS_VALUE_TOKEN && v[end - 1].type == GTEXT_CSS_TOKEN_WHITESPACE) {
    end--;
  }
  if (i == end) {
    if (!block) {
      *valid = false;
      return GTEXT_CSS_OK;
    }
    if (declare) {
      return gtext_css_layer_declare_anonymous(s->layers, s->opt.origin, parent, inner, NULL);
    }
    return GTEXT_CSS_OK;
  }
  size_t names = 0;
  while (i < end) {
    size_t j = i;
    while (j < end && !(v[j].kind == GTEXT_CSS_VALUE_TOKEN && v[j].type == GTEXT_CSS_TOKEN_COMMA)) {
      j++;
    }
    size_t a = i, b = j;
    while (a < b && v[a].kind == GTEXT_CSS_VALUE_TOKEN && v[a].type == GTEXT_CSS_TOKEN_WHITESPACE) {
      a++;
    }
    while (b > a && v[b - 1].kind == GTEXT_CSS_VALUE_TOKEN && v[b - 1].type == GTEXT_CSS_TOKEN_WHITESPACE) {
      b--;
    }
    bool want_ident = true;
    for (size_t k = a; k < b; k++) {
      const bool ident = v[k].kind == GTEXT_CSS_VALUE_TOKEN && v[k].type == GTEXT_CSS_TOKEN_IDENT;
      const bool dot = v[k].kind == GTEXT_CSS_VALUE_TOKEN && v[k].type == GTEXT_CSS_TOKEN_DELIM && v[k].value.length == 1 && v[k].value.data[0] == '.';
      if (want_ident ? !ident : !dot) {
        *valid = false;
        return GTEXT_CSS_OK;
      }
      want_ident = !want_ident;
    }
    if (a == b || want_ident) {
      *valid = false;
      return GTEXT_CSS_OK;
    }
    names++;
    if (declare) {
      /* each identifier is one segment (an escaped `.` is part of its name), declared under the one before it */
      GTEXT_CSS_Layer_Id cur = parent;
      for (size_t k = a; k < b; k++) {
        if (v[k].type != GTEXT_CSS_TOKEN_IDENT) {
          continue;
        }
        const GTEXT_CSS_Status st = gtext_css_layer_declare_segment(s->layers, s->opt.origin, cur, v[k].value.data, v[k].value.length, &cur, NULL);
        if (st == GTEXT_CSS_E_INVALID) {
          *valid = false; /* a CSS-wide keyword or an empty segment: the rule declares nothing more */
          return GTEXT_CSS_OK;
        }
        if (st != GTEXT_CSS_OK) {
          return st;
        }
      }
      if (block) {
        *inner = cur;
      }
    }
    i = j + 1;
  }
  if (block && names > 1) {
    *valid = false;
  }
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status declare_layers(GTEXT_CSS_Atrule_Set * s, const GTEXT_CSS_Rule * r, GTEXT_CSS_Layer_Id parent, bool block, GTEXT_CSS_Layer_Id * inner, bool * valid) {
  GTEXT_CSS_Status st = layer_names(s, r, parent, block, false, inner, valid);
  if (st != GTEXT_CSS_OK || !*valid) {
    return st;
  }
  return layer_names(s, r, parent, block, true, inner, valid);
}

GTEXT_CSS_Status gtext_css_atrule_set_add_tree(GTEXT_CSS_Atrule_Set * s, const GTEXT_CSS_Tree * tree) {
  if (!s || !tree || s->frozen) {
    return GTEXT_CSS_E_INVALID;
  }
  s->trees++;
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  size_t cap = 16, sp = 0;
  frame * stack = gtext_allocator_malloc(&s->alloc, cap * sizeof *stack);
  if (!stack) {
    return GTEXT_CSS_E_OOM;
  }
  memset(&stack[0], 0, sizeof stack[0]);
  if (tree->kind == GTEXT_CSS_TREE_BLOCK_CONTENTS) {
    stack[0].items = tree->items;
    stack[0].item_count = tree->item_count;
  }
  else {
    stack[0].rules = tree->rules;
    stack[0].rule_count = tree->rule_count;
  }
  sp = 1;
  while (status == GTEXT_CSS_OK && sp > 0) {
    frame * top = &stack[sp - 1];
    const GTEXT_CSS_Rule * rule = NULL;
    if (top->rules) {
      if (top->pos < top->rule_count) {
        rule = &top->rules[top->pos++];
      }
    }
    else {
      while (top->pos < top->item_count) {
        const GTEXT_CSS_Block_Item * it = &top->items[top->pos++];
        if (it->kind == GTEXT_CSS_ITEM_RULE) {
          rule = it->rule;
          break;
        }
      }
    }
    if (!rule) {
      sp--;
      continue;
    }
    if (rule->kind != GTEXT_CSS_RULE_AT) {
      continue;
    }
    const bool layer = rname(rule, "layer");
    const bool group = group_name(rule);
    if (layer || group) {
      if (layer && !rule->has_block) {
        bool ok;
        GTEXT_CSS_Layer_Id dummy;
        status = declare_layers(s, rule, top->layer, false, &dummy, &ok);
        continue;
      }
      if (!rule->has_block || rule->content_count == 0) {
        if (layer && rule->has_block) {
          bool ok;
          GTEXT_CSS_Layer_Id dummy = GTEXT_CSS_LAYER_NONE;
          status = declare_layers(s, rule, top->layer, true, &dummy, &ok);
        }
        continue;
      }
      GTEXT_CSS_Layer_Id inner = top->layer;
      bool unresolved = top->unresolved;
      if (group) {
        if (s->opt.conditions) {
          if (!gtext_css_conditions_active_rule((void *)s->opt.conditions, rule)) {
            continue;
          }
        }
        else {
          unresolved = true;
        }
      }
      else {
        bool ok;
        status = declare_layers(s, rule, top->layer, true, &inner, &ok);
        if (status != GTEXT_CSS_OK || !ok) {
          continue;
        }
      }
      if (sp >= s->opt.max_rule_depth) {
        status = GTEXT_CSS_E_LIMIT_DEPTH;
        break;
      }
      if (sp == cap) {
        frame * g = gtext_allocator_realloc(&s->alloc, stack, cap * 2 * sizeof *g);
        if (!g) {
          status = GTEXT_CSS_E_OOM;
          break;
        }
        stack = g;
        cap *= 2;
      }
      memset(&stack[sp], 0, sizeof stack[sp]);
      stack[sp].items = rule->contents;
      stack[sp].item_count = rule->content_count;
      stack[sp].layer = inner;
      stack[sp].unresolved = unresolved;
      sp++;
      continue;
    }
    /* a rule the set may collect */
    char nm[40];
    if (rule->name.length >= sizeof nm) {
      continue;
    }
    memcpy(nm, rule->name.data, rule->name.length);
    nm[rule->name.length] = 0;
    for (size_t i = 0; nm[i]; i++) {
      if (nm[i] >= 'A' && nm[i] <= 'Z') {
        nm[i] = (char)(nm[i] + 32);
      }
    }
    const void * row = css_atrule_row_find(nm, rule->name.length);
    if (!row || !collected_kind(css_atrule_row_kind(row))) {
      continue;
    }
    if (top->unresolved) {
      status = GTEXT_CSS_E_UNSUPPORTED;
      break;
    }
    GTEXT_CSS_Atrule * a = NULL;
    GTEXT_CSS_Atrule_Result res;
    status = gtext_css_atrule_parse(s->registry, rule, &s->opt.atrule, &a, &res);
    if (status != GTEXT_CSS_OK) {
      break;
    }
    if (!res.valid || !a) {
      gtext_css_atrule_free(a);
      continue;
    }
    status = add_entry(s, a, top->layer);
  }
  gtext_allocator_free(&s->alloc, stack);
  return status;
}

/* ---- freezing -------------------------------------------------------------------------------------------------------------------------- */

static const entry * g_ents;

static int cascade_cmp(const void * a, const void * b) {
  const entry * x = &g_ents[*(const size_t *)a];
  const entry * y = &g_ents[*(const size_t *)b];
  if (x->rank != y->rank) {
    return x->rank < y->rank ? -1 : 1;
  }
  return x->order < y->order ? -1 : (x->order > y->order);
}

static int name_cmp_raw(const char * a, size_t al, const char * b, size_t bl) {
  const size_t m = al < bl ? al : bl;
  const int c = m ? memcmp(a, b, m) : 0;
  if (c) {
    return c;
  }
  return al < bl ? -1 : (al > bl);
}

static int ref_cmp(const void * a, const void * b) {
  const name_ref * x = a;
  const name_ref * y = b;
  int c = name_cmp_raw(x->key, x->key_len, y->key, y->key_len);
  if (!c) {
    c = name_cmp_raw(x->key2, x->key2_len, y->key2, y->key2_len);
  }
  if (c) {
    return c;
  }
  return x->pos < y->pos ? -1 : (x->pos > y->pos);
}

/// The index of the last ref matching (key, key2), or -1.
static long ref_last(const name_ref * refs, size_t n, const char * key, size_t kl, const char * key2, size_t k2l) {
  size_t lo = 0, hi = n;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    int c = name_cmp_raw(refs[mid].key, refs[mid].key_len, key, kl);
    if (!c) {
      c = name_cmp_raw(refs[mid].key2, refs[mid].key2_len, key2, k2l);
    }
    if (c <= 0) {
      lo = mid + 1;
    }
    else {
      hi = mid;
    }
  }
  if (lo == 0) {
    return -1;
  }
  const name_ref * r = &refs[lo - 1];
  if (name_cmp_raw(r->key, r->key_len, key, kl) || name_cmp_raw(r->key2, r->key2_len, key2, k2l)) {
    return -1;
  }
  return (long)(lo - 1);
}

static bool push_ref(GTEXT_CSS_Atrule_Set * s, name_ref ** arr, size_t * n, size_t * cap, name_ref r) {
  if (*n == *cap) {
    const size_t nc = *cap ? *cap * 2 : 16;
    name_ref * g = gtext_allocator_realloc(&s->alloc, *arr, nc * sizeof *g);
    if (!g) {
      return false;
    }
    *arr = g;
    *cap = nc;
  }
  (*arr)[(*n)++] = r;
  return true;
}

/// Follow one successor edge of every style (extends in pass 0, fallback in pass 1) as a functional graph: find the cycles, flag their members, and (pass 0) resolve the system and
/// the base from the end of each path backwards, so the work is linear in the number of styles however long the chains.
static GTEXT_CSS_Status walk_counters(GTEXT_CSS_Atrule_Set * s, GTEXT_CSS_Counter_Style ** st, long * succ, int pass) {
  const size_t n = s->ncounters;
  unsigned char * state = gtext_allocator_calloc(&s->alloc, n, 1);
  size_t * path = gtext_allocator_malloc(&s->alloc, n * sizeof *path);
  size_t * where = gtext_allocator_malloc(&s->alloc, n * sizeof *where);
  if (!state || !path || !where) {
    gtext_allocator_free(&s->alloc, state);
    gtext_allocator_free(&s->alloc, path);
    gtext_allocator_free(&s->alloc, where);
    return GTEXT_CSS_E_OOM;
  }
  for (size_t i = 0; i < n; i++) {
    if (state[i]) {
      continue;
    }
    size_t plen = 0;
    size_t cur = i;
    size_t cyc_from = (size_t)-1;
    for (;;) {
      if (state[cur] == 1) {
        cyc_from = where[cur];
        break;
      }
      if (state[cur] == 2) {
        break;
      }
      state[cur] = 1;
      where[cur] = plen;
      path[plen++] = cur;
      if (succ[cur] < 0) {
        break;
      }
      cur = (size_t)succ[cur];
    }
    if (cyc_from != (size_t)-1) {
      for (size_t k = cyc_from; k < plen; k++) {
        if (pass == 0) {
          st[path[k]]->extends_cycle = true;
        }
        else {
          st[path[k]]->fallback_cycle = true;
        }
      }
    }
    for (size_t k = plen; k-- > 0;) {
      GTEXT_CSS_Counter_Style * c = st[path[k]];
      if (pass == 0) {
        if (c->system != GTEXT_CSS_COUNTER_EXTENDS) {
          c->resolved_system = c->system;
          c->base = NULL;
        }
        else if (c->extends_cycle || succ[path[k]] < 0) {
          c->resolved_system = GTEXT_CSS_COUNTER_NUMERIC;
          c->base = NULL;
        }
        else {
          const GTEXT_CSS_Counter_Style * nx = st[(size_t)succ[path[k]]];
          c->resolved_system = nx->resolved_system;
          c->base = nx->system != GTEXT_CSS_COUNTER_EXTENDS ? nx : nx->base;
        }
      }
      else if (!c->fallback_cycle && succ[path[k]] >= 0) {
        c->fallback_style = st[(size_t)succ[path[k]]];
      }
      state[path[k]] = 2;
    }
  }
  gtext_allocator_free(&s->alloc, state);
  gtext_allocator_free(&s->alloc, path);
  gtext_allocator_free(&s->alloc, where);
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status resolve_counters(GTEXT_CSS_Atrule_Set * s) {
  const size_t n = s->ncounters;
  if (!n) {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Counter_Style ** st = gtext_allocator_malloc(&s->alloc, n * sizeof *st);
  long * succ = gtext_allocator_malloc(&s->alloc, n * sizeof *succ);
  if (!st || !succ) {
    gtext_allocator_free(&s->alloc, st);
    gtext_allocator_free(&s->alloc, succ);
    return GTEXT_CSS_E_OOM;
  }
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  for (int pass = 0; pass < 2 && status == GTEXT_CSS_OK; pass++) {
    for (size_t i = 0; i < n; i++) {
      st[i] = s->ents[s->counters[i].entry].typed;
      const GTEXT_CSS_String * nm = pass == 0 ? (st[i]->system == GTEXT_CSS_COUNTER_EXTENDS ? &st[i]->extends_name : NULL) : &st[i]->fallback;
      succ[i] = nm && nm->data ? ref_last(s->counters, n, nm->data, nm->length, "", 0) : -1;
    }
    status = walk_counters(s, st, succ, pass);
  }
  gtext_allocator_free(&s->alloc, st);
  gtext_allocator_free(&s->alloc, succ);
  return status;
}

typedef struct {
  const char * name;
  size_t name_len;
  size_t order;
  GTEXT_CSS_Feature_Value v;
} fv_item;

static int fv_cmp(const void * a, const void * b) {
  const fv_item * x = a;
  const fv_item * y = b;
  const int c = name_cmp_raw(x->name, x->name_len, y->name, y->name_len);
  if (c) {
    return c;
  }
  return x->order < y->order ? -1 : (x->order > y->order);
}

/// Merge the `@font-feature-values` of one family, in cascade order: for each type a later name replaces an earlier one.
static GTEXT_CSS_Status merge_features(GTEXT_CSS_Atrule_Set * s, size_t first, size_t last) {
  GTEXT_CSS_Feature_Values * m = css_arena_alloc(&s->h->arena, sizeof *m);
  GTEXT_CSS_String * fam = css_arena_alloc(&s->h->arena, sizeof *fam);
  if (!m || !fam) {
    return GTEXT_CSS_E_OOM;
  }
  memset(m, 0, sizeof *m);
  fam->data = s->features[first].key;
  fam->length = s->features[first].key_len;
  m->families = fam;
  m->family_keys = fam;
  m->family_count = 1;
  const GTEXT_CSS_Feature_Values * lastv = NULL;
  for (size_t t = 0; t < GTEXT_CSS_FEATURE_TYPE_COUNT; t++) {
    size_t total = 0;
    for (size_t r = first; r <= last; r++) {
      const GTEXT_CSS_Feature_Values * f = s->ents[s->features[r].entry].typed;
      total += f->value_count[t];
    }
    if (!total) {
      continue;
    }
    fv_item * items = gtext_allocator_malloc(&s->alloc, total * sizeof *items);
    if (!items) {
      return GTEXT_CSS_E_OOM;
    }
    size_t n = 0;
    for (size_t r = first; r <= last; r++) {
      const GTEXT_CSS_Feature_Values * f = s->ents[s->features[r].entry].typed;
      for (size_t k = 0; k < f->value_count[t]; k++) {
        items[n].name = f->values[t][k].name.data;
        items[n].name_len = f->values[t][k].name.length;
        items[n].order = n;
        items[n].v = f->values[t][k];
        n++;
      }
    }
    qsort(items, n, sizeof *items, fv_cmp);
    GTEXT_CSS_Feature_Value * arr = css_arena_alloc(&s->h->arena, n * sizeof *arr);
    if (!arr) {
      gtext_allocator_free(&s->alloc, items);
      return GTEXT_CSS_E_OOM;
    }
    size_t w = 0;
    for (size_t k = 0; k < n; k++) {
      if (k + 1 < n && !name_cmp_raw(items[k].name, items[k].name_len, items[k + 1].name, items[k + 1].name_len)) {
        continue;
      }
      arr[w++] = items[k].v;
    }
    gtext_allocator_free(&s->alloc, items);
    m->values[t] = arr;
    m->value_count[t] = w;
  }
  for (size_t r = first; r <= last; r++) {
    const GTEXT_CSS_Feature_Values * f = s->ents[s->features[r].entry].typed;
    lastv = f;
    if (f->has_display) {
      m->has_display = true;
      m->display = f->display;
    }
  }
  m->rule = lastv ? lastv->rule : NULL;
  GTEXT_CSS_Feature_Values ** g = gtext_allocator_realloc(&s->alloc, s->merged, (s->nmerged + 1) * sizeof *g);
  if (!g) {
    return GTEXT_CSS_E_OOM;
  }
  s->merged = g;
  s->merged[s->nmerged++] = m;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_atrule_set_freeze(GTEXT_CSS_Atrule_Set * s) {
  if (!s) {
    return GTEXT_CSS_E_INVALID;
  }
  if (s->frozen) {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Status st = gtext_css_layer_freeze(s->layers);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  for (size_t i = 0; i < s->n; i++) {
    st = gtext_css_layer_rank(s->layers, s->opt.origin, s->ents[i].layer, &s->ents[i].rank);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
  }
  s->sorted = gtext_allocator_malloc(&s->alloc, (s->n ? s->n : 1) * sizeof *s->sorted);
  if (!s->sorted) {
    return GTEXT_CSS_E_OOM;
  }
  for (size_t i = 0; i < s->n; i++) {
    s->sorted[i] = i;
  }
  g_ents = s->ents;
  if (s->n) {
    if (s->n) {
    qsort(s->sorted, s->n, sizeof *s->sorted, cascade_cmp);
  }
  }
  for (size_t p = 0; p < s->n; p++) {
    s->ents[s->sorted[p]].pos = p;
  }
  size_t cf = 0, cft = 0, cp = 0, cc = 0, ccs = 0, ck = 0, ct = 0, cpg = 0;
  bool bad = false;
  for (size_t p = 0; p < s->n && !bad; p++) {
    const size_t ei = s->sorted[p];
    entry * e = &s->ents[ei];
    name_ref r;
    memset(&r, 0, sizeof r);
    r.pos = p;
    r.entry = ei;
    r.key = "";
    r.key2 = "";
    switch (e->kind) {
    case GTEXT_CSS_ATRULE_FONT_FACE: {
      GTEXT_CSS_Font_Face * f = NULL;
      st = gtext_css_font_face_from_atrule(e->rule, &f);
      if (st != GTEXT_CSS_OK) {
        bad = true;
        break;
      }
      e->typed = f;
      if (gtext_css_font_face_defined(f)) {
        r.key = f->family_key.data;
        r.key_len = f->family_key.length;
        bad = !push_ref(s, &s->faces, &s->nfaces, &cf, r);
      }
      break;
    }
    case GTEXT_CSS_ATRULE_FONT_FEATURE_VALUES: {
      GTEXT_CSS_Feature_Values * f = NULL;
      st = gtext_css_feature_values_from_atrule(e->rule, &f);
      if (st != GTEXT_CSS_OK) {
        bad = true;
        break;
      }
      e->typed = f;
      for (size_t k = 0; k < f->family_count && !bad; k++) {
        r.key = f->family_keys[k].data;
        r.key_len = f->family_keys[k].length;
        bad = !push_ref(s, &s->features, &s->nfeatures, &cft, r);
      }
      break;
    }
    case GTEXT_CSS_ATRULE_FONT_PALETTE_VALUES: {
      GTEXT_CSS_Palette_Values * f = NULL;
      st = gtext_css_palette_values_from_atrule(e->rule, &f);
      if (st != GTEXT_CSS_OK) {
        bad = true;
        break;
      }
      e->typed = f;
      for (size_t k = 0; k < f->family_count && !bad; k++) {
        r.key = f->name.data ? f->name.data : "";
        r.key_len = f->name.length;
        r.key2 = f->family_keys[k].data;
        r.key2_len = f->family_keys[k].length;
        bad = !push_ref(s, &s->palettes, &s->npalettes, &cp, r);
      }
      break;
    }
    case GTEXT_CSS_ATRULE_COLOR_PROFILE: {
      GTEXT_CSS_Color_Profile * f = NULL;
      st = gtext_css_color_profile_from_atrule(e->rule, &f);
      if (st != GTEXT_CSS_OK) {
        bad = true;
        break;
      }
      e->typed = f;
      r.key = f->name.data ? f->name.data : "";
      r.key_len = f->name.length;
      bad = !push_ref(s, &s->profiles, &s->nprofiles, &cc, r);
      break;
    }
    case GTEXT_CSS_ATRULE_COUNTER_STYLE: {
      GTEXT_CSS_Counter_Style * f = NULL;
      st = gtext_css_counter_style_from_atrule(e->rule, &f);
      if (st != GTEXT_CSS_OK) {
        bad = true;
        break;
      }
      e->typed = f;
      if (f->valid) {
        r.key = f->name.data ? f->name.data : "";
        r.key_len = f->name.length;
        bad = !push_ref(s, &s->counters, &s->ncounters, &ccs, r);
      }
      break;
    }
    case GTEXT_CSS_ATRULE_KEYFRAMES: {
      GTEXT_CSS_Keyframes * f = NULL;
      st = gtext_css_keyframes_from_atrule(e->rule, &f);
      if (st != GTEXT_CSS_OK) {
        bad = true;
        break;
      }
      e->typed = f;
      r.key = f->name.data ? f->name.data : "";
      r.key_len = f->name.length;
      bad = !push_ref(s, &s->frames, &s->nframes, &ck, r);
      break;
    }
    case GTEXT_CSS_ATRULE_POSITION_TRY: {
      GTEXT_CSS_String nm;
      if (gtext_css_atrule_name_at(e->rule, 0, &nm, NULL)) {
        r.key = nm.data;
        r.key_len = nm.length;
        bad = !push_ref(s, &s->tries, &s->ntries, &ct, r);
      }
      break;
    }
    case GTEXT_CSS_ATRULE_VIEW_TRANSITION:
      s->view_transition = ei + 1;
      break;
    case GTEXT_CSS_ATRULE_PAGE: {
      size_t * g = gtext_allocator_realloc(&s->alloc, s->pages, (s->npages + 1) * sizeof *g);
      if (!g) {
        bad = true;
        break;
      }
      s->pages = g;
      s->pages[s->npages++] = ei;
      (void)cpg;
      break;
    }
    default:
      break;
    }
    if (bad && st == GTEXT_CSS_OK) {
      st = GTEXT_CSS_E_OOM;
    }
  }
  if (bad) {
    return st;
  }
  if (s->nfaces) {
    qsort(s->faces, s->nfaces, sizeof *s->faces, ref_cmp);
  }
  if (s->nfeatures) {
    qsort(s->features, s->nfeatures, sizeof *s->features, ref_cmp);
  }
  if (s->npalettes) {
    qsort(s->palettes, s->npalettes, sizeof *s->palettes, ref_cmp);
  }
  if (s->nprofiles) {
    qsort(s->profiles, s->nprofiles, sizeof *s->profiles, ref_cmp);
  }
  if (s->ncounters) {
    qsort(s->counters, s->ncounters, sizeof *s->counters, ref_cmp);
  }
  if (s->nframes) {
    qsort(s->frames, s->nframes, sizeof *s->frames, ref_cmp);
  }
  if (s->ntries) {
    qsort(s->tries, s->ntries, sizeof *s->tries, ref_cmp);
  }
  st = resolve_counters(s);
  for (size_t i = 0; st == GTEXT_CSS_OK && i < s->nfeatures;) {
    size_t j = i;
    while (j + 1 < s->nfeatures && !name_cmp_raw(s->features[j + 1].key, s->features[j + 1].key_len, s->features[i].key, s->features[i].key_len)) {
      j++;
    }
    if (j > i) {
      st = merge_features(s, i, j);
    }
    i = j + 1;
  }
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  s->frozen = true;
  return GTEXT_CSS_OK;
}

/* ---- the finds ------------------------------------------------------------------------------------------------------------------------ */

size_t gtext_css_atrule_set_count(const GTEXT_CSS_Atrule_Set * s) {
  return s ? s->n : 0;
}

bool gtext_css_atrule_set_entry(const GTEXT_CSS_Atrule_Set * s, size_t index, GTEXT_CSS_Atrule_Entry * out) {
  if (!s || !s->frozen || !out || index >= s->n) {
    return false;
  }
  const entry * e = &s->ents[s->sorted[index]];
  out->rule = e->rule;
  out->layer = e->layer;
  out->layer_rank = e->rank;
  out->order = e->order;
  out->tree = e->tree;
  return true;
}

const GTEXT_CSS_Layer_Registry * gtext_css_atrule_set_layers(const GTEXT_CSS_Atrule_Set * s) {
  return s ? s->layers : NULL;
}

size_t gtext_css_atrule_set_family_key(const char * family, size_t length, char * buffer, size_t capacity) {
  size_t n = 0;
  const size_t cap = length * 12 + 16;
  if (!family || !buffer) {
    return 0;
  }
  char * tmp = gtext_allocator_malloc(NULL, cap);
  if (!tmp) {
    return 0;
  }
  if (guni_case_fold_utf8(family, length, GUNI_INVALID_REPLACE, false, NULL, tmp, cap, &n) != GUNI_OK) {
    n = 0;
  }
  if (n && buffer && n <= capacity) {
    memcpy(buffer, tmp, n);
  }
  else {
    n = 0;
  }
  gtext_allocator_free(NULL, tmp);
  return n;
}

size_t gtext_css_atrule_set_font_faces(const GTEXT_CSS_Atrule_Set * s, const char * key, size_t kl, const GTEXT_CSS_Font_Face ** out, size_t capacity) {
  if (!s || !s->frozen || !key) {
    return 0;
  }
  const long last = ref_last(s->faces, s->nfaces, key, kl, "", 0);
  if (last < 0) {
    return 0;
  }
  size_t first = (size_t)last;
  while (first > 0 && !name_cmp_raw(s->faces[first - 1].key, s->faces[first - 1].key_len, key, kl)) {
    first--;
  }
  const size_t total = (size_t)last - first + 1;
  for (size_t i = 0; out && i < total && i < capacity; i++) {
    out[i] = s->ents[s->faces[first + i].entry].typed;
  }
  return total;
}

const GTEXT_CSS_Feature_Values * gtext_css_atrule_set_feature_values(const GTEXT_CSS_Atrule_Set * s, const char * key, size_t kl) {
  if (!s || !s->frozen || !key) {
    return NULL;
  }
  const long last = ref_last(s->features, s->nfeatures, key, kl, "", 0);
  if (last < 0) {
    return NULL;
  }
  size_t first = (size_t)last;
  while (first > 0 && !name_cmp_raw(s->features[first - 1].key, s->features[first - 1].key_len, key, kl)) {
    first--;
  }
  if (first == (size_t)last) {
    return s->ents[s->features[last].entry].typed;
  }
  for (size_t lo = 0, hi = s->nmerged; lo < hi;) {
    const size_t mid = lo + (hi - lo) / 2;
    const GTEXT_CSS_Feature_Values * f = s->merged[mid];
    const int c = name_cmp_raw(f->family_keys[0].data, f->family_keys[0].length, key, kl);
    if (!c) {
      return f;
    }
    if (c < 0) {
      lo = mid + 1;
    }
    else {
      hi = mid;
    }
  }
  return NULL;
}

const GTEXT_CSS_Palette_Values * gtext_css_atrule_set_palette_values(const GTEXT_CSS_Atrule_Set * s, const char * name, size_t nl, const char * key, size_t kl) {
  if (!s || !s->frozen || !name || !key) {
    return NULL;
  }
  const long i = ref_last(s->palettes, s->npalettes, name, nl, key, kl);
  return i < 0 ? NULL : s->ents[s->palettes[i].entry].typed;
}

const GTEXT_CSS_Color_Profile * gtext_css_atrule_set_color_profile(const GTEXT_CSS_Atrule_Set * s, const char * name, size_t length) {
  if (!s || !s->frozen || !name) {
    return NULL;
  }
  const long i = ref_last(s->profiles, s->nprofiles, name, length, "", 0);
  return i < 0 ? NULL : s->ents[s->profiles[i].entry].typed;
}

const GTEXT_CSS_Counter_Style * gtext_css_atrule_set_counter_style(const GTEXT_CSS_Atrule_Set * s, const char * name, size_t length) {
  if (!s || !s->frozen || !name) {
    return NULL;
  }
  const long i = ref_last(s->counters, s->ncounters, name, length, "", 0);
  return i < 0 ? NULL : s->ents[s->counters[i].entry].typed;
}

const GTEXT_CSS_Keyframes * gtext_css_atrule_set_keyframes(const GTEXT_CSS_Atrule_Set * s, const char * name, size_t length) {
  if (!s || !s->frozen || !name) {
    return NULL;
  }
  const long i = ref_last(s->frames, s->nframes, name, length, "", 0);
  return i < 0 ? NULL : s->ents[s->frames[i].entry].typed;
}

const GTEXT_CSS_Atrule * gtext_css_atrule_set_view_transition(const GTEXT_CSS_Atrule_Set * s) {
  return s && s->frozen && s->view_transition ? s->ents[s->view_transition - 1].rule : NULL;
}

const GTEXT_CSS_Atrule * gtext_css_atrule_set_position_try(const GTEXT_CSS_Atrule_Set * s, const char * name, size_t length) {
  if (!s || !s->frozen || !name) {
    return NULL;
  }
  const long i = ref_last(s->tries, s->ntries, name, length, "", 0);
  return i < 0 ? NULL : s->ents[s->tries[i].entry].rule;
}

static int match_cmp(const void * a, const void * b) {
  const GTEXT_CSS_Page_Match * x = a;
  const GTEXT_CSS_Page_Match * y = b;
  if (x->layer_rank != y->layer_rank) {
    return x->layer_rank < y->layer_rank ? -1 : 1;
  }
  const int c = gtext_css_page_specificity_compare(x->specificity, y->specificity);
  if (c) {
    return c;
  }
  return x->order < y->order ? -1 : (x->order > y->order);
}

size_t gtext_css_atrule_set_page_collect(const GTEXT_CSS_Atrule_Set * s, const GTEXT_CSS_Page_Context * page, GTEXT_CSS_Page_Match * out, size_t capacity) {
  if (!s || !s->frozen || !page) {
    return 0;
  }
  size_t total = 0;
  for (size_t i = 0; i < s->npages; i++) {
    const entry * e = &s->ents[s->pages[i]];
    GTEXT_CSS_Page_Specificity sp;
    if (!gtext_css_page_matches(e->rule->sels, e->rule->nsels, page, &sp)) {
      continue;
    }
    if (out && total < capacity) {
      out[total].rule = e->rule;
      out[total].specificity = sp;
      out[total].layer = e->layer;
      out[total].layer_rank = e->rank;
      out[total].order = e->order;
    }
    total++;
  }
  if (out) {
    qsort(out, total < capacity ? total : capacity, sizeof *out, match_cmp);
  }
  return total;
}
