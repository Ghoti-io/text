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
 * Cascade layers (css_layer.h): the registry of layers, their order and the reading of a sheet's `@layer` and `@import ... layer()` rules.
 *
 * The layers of an origin are a tree whose root is the unlayered styles. A node is found by (parent, name) in a table keyed by a seeded hash, so
 * that n sibling layers cost n probes and no choice of names makes it worse. The order is the post-order of the tree, children in order of
 * first declaration, computed once at freeze without recursion.
 */

#include <ghoti.io/text/css/css_layer.h>

#include <stdlib.h>
#include <string.h>

#include "css_internal.h"

#define DEFAULT_MAX_LAYERS ((size_t)65536)
#define LIMIT_MAX_LAYERS ((size_t)1 << 24)
#define DEFAULT_MAX_DEPTH ((size_t)256)
#define LIMIT_MAX_DEPTH ((size_t)65536)
#define DEFAULT_RULE_DEPTH ((size_t)4096)
#define LIMIT_RULE_DEPTH ((size_t)1048576)

/// The nodes 1 to 3 are the roots of the three origins; a layer's public id is its node index.
#define ROOTS 3u
#define FIRST_LAYER (ROOTS + 1u)

typedef struct {
  uint32_t parent;
  uint32_t first_child, last_child, next_sibling;
  uint32_t name_off, name_len;
  uint32_t depth;
  uint32_t rank;
  uint8_t origin;
  uint8_t anonymous;
} lnode;

struct GTEXT_CSS_Layer_Registry {
  GTEXT_Allocator alloc;
  GTEXT_CSS_Layer_Options options;
  lnode * nodes;
  size_t count, cap;          ///< Nodes, index 0 unused, the roots included.
  char * names;
  size_t names_len, names_cap;
  uint32_t * table;           ///< Open addressing; 0 is empty.
  size_t table_cap, table_used;
  uint64_t seed;
  bool frozen;
  size_t origin_layers[ROOTS];
  size_t work;
};

const char * gtext_css_origin_name(GTEXT_CSS_Origin origin) {
  switch (origin) {
  case GTEXT_CSS_ORIGIN_USER_AGENT: return "user-agent";
  case GTEXT_CSS_ORIGIN_USER: return "user";
  case GTEXT_CSS_ORIGIN_AUTHOR: return "author";
  case GTEXT_CSS_ORIGIN_ANIMATION: return "animation";
  case GTEXT_CSS_ORIGIN_TRANSITION: return "transition";
  default: return NULL;
  }
}

/// The registry's work counter (probes, nodes numbered), for the quadratic suite; hidden, like css_registry_work().
size_t css_layer_work(const GTEXT_CSS_Layer_Registry * r) {
  return r ? r->work : 0;
}

GTEXT_CSS_Layer_Options gtext_css_layer_options_default(void) {
  GTEXT_CSS_Layer_Options o;
  memset(&o, 0, sizeof o);
  o.max_layers = DEFAULT_MAX_LAYERS;
  o.max_depth = DEFAULT_MAX_DEPTH;
  return o;
}

static void result_clear(GTEXT_CSS_Layer_Result * r) {
  if (r) {
    memset(r, 0, sizeof *r);
    r->message = gtext_css_status_string(GTEXT_CSS_OK);
  }
}

static GTEXT_CSS_Status result_done(GTEXT_CSS_Layer_Result * r, GTEXT_CSS_Status status) {
  if (r) {
    r->status = status;
    r->message = gtext_css_status_string(status);
  }
  return status;
}

static void result_name(GTEXT_CSS_Layer_Result * r, const char * s, size_t n) {
  if (!r) {
    return;
  }
  if (n > sizeof r->name - 1) {
    n = sizeof r->name - 1;
  }
  if (n) {
    memcpy(r->name, s, n);
  }
  r->name[n] = '\0';
}

static GTEXT_CSS_Status fail(GTEXT_CSS_Layer_Result * r, GTEXT_CSS_Status status, const char * name, size_t name_len, const char * detail) {
  if (r) {
    result_name(r, name, name_len);
    r->detail = detail;
  }
  return result_done(r, status);
}

/* ---- the hash ------------------------------------------------------------------------------------------------- */

static uint64_t mix64(uint64_t x) {
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ULL;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBULL;
  x ^= x >> 31;
  return x;
}

/// A keyed hash of (parent, name): FNV-1a over the bytes with the seed and the parent folded in, then avalanched.
static uint64_t key_hash(uint64_t seed, uint32_t parent, const char * name, size_t n) {
  uint64_t h = 0xCBF29CE484222325ULL ^ mix64(seed ^ ((uint64_t)parent * 0x9E3779B97F4A7C15ULL));
  for (size_t i = 0; i < n; i++) {
    h ^= (unsigned char)name[i];
    h *= 0x100000001B3ULL;
  }
  return mix64(h ^ seed);
}

/* ---- the registry --------------------------------------------------------------------------------------------- */

GTEXT_CSS_Status gtext_css_layer_registry_new(const GTEXT_CSS_Layer_Options * options, GTEXT_CSS_Layer_Registry ** out) {
  if (!out) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Layer_Options o = options ? *options : gtext_css_layer_options_default();
  if (o.max_layers == 0) {
    o.max_layers = DEFAULT_MAX_LAYERS;
  }
  if (o.max_layers > LIMIT_MAX_LAYERS) {
    o.max_layers = LIMIT_MAX_LAYERS;
  }
  if (o.max_depth == 0) {
    o.max_depth = DEFAULT_MAX_DEPTH;
  }
  if (o.max_depth > LIMIT_MAX_DEPTH) {
    o.max_depth = LIMIT_MAX_DEPTH;
  }
  const GTEXT_Allocator * ap = o.allocator ? o.allocator : gtext_allocator_default();
  GTEXT_Allocator a = *ap;
  GTEXT_CSS_Layer_Registry * r = gtext_allocator_calloc(&a, 1, sizeof *r);
  if (!r) {
    return GTEXT_CSS_E_OOM;
  }
  r->alloc = a;
  r->options = o;
  r->options.allocator = NULL;
  r->cap = 16;
  r->nodes = gtext_allocator_calloc(&r->alloc, r->cap, sizeof *r->nodes);
  if (!r->nodes) {
    gtext_allocator_free(&a, r);
    return GTEXT_CSS_E_OOM;
  }
  r->count = FIRST_LAYER;
  for (uint32_t i = 1; i <= ROOTS; i++) {
    r->nodes[i].origin = (uint8_t)(i - 1);
    r->nodes[i].anonymous = 1;
  }
  uint64_t seed = o.hash_seed;
  if (seed == 0) {
    /* no clock and no process state are read: the addresses of this allocation and of a local (randomized by the system) are the seed */
    seed = (uint64_t)(uintptr_t)r * 0x9E3779B97F4A7C15ULL;
    seed ^= ((uint64_t)(uintptr_t)&o) * 0xC2B2AE3D27D4EB4FULL;
    seed ^= seed >> 29;
    if (seed == 0) {
      seed = 0x5851F42D4C957F2DULL;
    }
  }
  r->seed = seed;
  *out = r;
  return GTEXT_CSS_OK;
}

void gtext_css_layer_registry_free(GTEXT_CSS_Layer_Registry * r) {
  if (!r) {
    return;
  }
  GTEXT_Allocator a = r->alloc;
  gtext_allocator_free(&a, r->nodes);
  gtext_allocator_free(&a, r->names);
  gtext_allocator_free(&a, r->table);
  gtext_allocator_free(&a, r);
}

static bool origin_has_layers(GTEXT_CSS_Origin o) {
  return (unsigned)o < ROOTS;
}

static bool is_wide_keyword(const char * s, size_t n) {
  return gtext_css_wide_keyword_lookup(s, n) != 0;
}

/// The node a (parent, name) is, or 0.
static uint32_t table_find(const GTEXT_CSS_Layer_Registry * r, uint32_t parent, const char * name, size_t n, size_t * probes) {
  if (!r->table_cap) {
    return 0;
  }
  size_t i = (size_t)(key_hash(r->seed, parent, name, n) & (r->table_cap - 1));
  for (;;) {
    const uint32_t id = r->table[i];
    if (probes) {
      (*probes)++;
    }
    if (!id) {
      return 0;
    }
    const lnode * nd = &r->nodes[id];
    if (nd->parent == parent && nd->name_len == n && memcmp(r->names + nd->name_off, name, n) == 0) {
      return id;
    }
    i = (i + 1) & (r->table_cap - 1);
  }
}

static GTEXT_CSS_Status table_insert(GTEXT_CSS_Layer_Registry * r, uint32_t id) {
  if ((r->table_used + 1) * 2 > r->table_cap) {
    size_t cap = r->table_cap ? r->table_cap * 2 : 32;
    uint32_t * t = gtext_allocator_calloc(&r->alloc, cap, sizeof *t);
    if (!t) {
      return GTEXT_CSS_E_OOM;
    }
    for (size_t k = 0; k < r->table_cap; k++) {
      const uint32_t old = r->table[k];
      if (old) {
        const lnode * nd = &r->nodes[old];
        size_t i = (size_t)(key_hash(r->seed, nd->parent, r->names + nd->name_off, nd->name_len) & (cap - 1));
        while (t[i]) {
          i = (i + 1) & (cap - 1);
        }
        t[i] = old;
      }
    }
    gtext_allocator_free(&r->alloc, r->table);
    r->table = t;
    r->table_cap = cap;
  }
  const lnode * nd = &r->nodes[id];
  size_t i = (size_t)(key_hash(r->seed, nd->parent, r->names + nd->name_off, nd->name_len) & (r->table_cap - 1));
  while (r->table[i]) {
    i = (i + 1) & (r->table_cap - 1);
  }
  r->table[i] = id;
  r->table_used++;
  return GTEXT_CSS_OK;
}

/// A new child of `parent` (a node index, a root included) named `name` (NULL: anonymous).
static GTEXT_CSS_Status node_new(GTEXT_CSS_Layer_Registry * r, uint32_t parent, const char * name, size_t n, uint32_t * out,
    GTEXT_CSS_Layer_Result * result) {
  if (r->frozen) {
    return fail(result, GTEXT_CSS_E_INVALID, name, n, "the registry is frozen: a new layer cannot be declared");
  }
  if (r->count - FIRST_LAYER >= r->options.max_layers) {
    return fail(result, GTEXT_CSS_E_LIMIT_LAYERS, name, n, "more layers than max_layers");
  }
  const size_t depth = (size_t)r->nodes[parent].depth + 1;
  if (depth > r->options.max_depth) {
    return fail(result, GTEXT_CSS_E_LIMIT_LAYER_DEPTH, name, n, "layers nested deeper than max_depth");
  }
  if (n > 0xFFFFFFFFu / 2 || r->names_len > 0xFFFFFFFFu / 2) {
    return fail(result, GTEXT_CSS_E_LIMIT_LAYERS, name, n, "the layer names are longer than the registry can hold");
  }
  if (r->count == r->cap) {
    size_t cap = r->cap * 2;
    lnode * g = gtext_allocator_realloc(&r->alloc, r->nodes, cap * sizeof *g);
    if (!g) {
      return result_done(result, GTEXT_CSS_E_OOM);
    }
    memset(g + r->cap, 0, (cap - r->cap) * sizeof *g);
    r->nodes = g;
    r->cap = cap;
  }
  if (name && r->names_len + n + 1 > r->names_cap) {
    size_t cap = r->names_cap ? r->names_cap * 2 : 256;
    while (cap < r->names_len + n + 1) {
      cap *= 2;
    }
    char * g = gtext_allocator_realloc(&r->alloc, r->names, cap);
    if (!g) {
      return result_done(result, GTEXT_CSS_E_OOM);
    }
    r->names = g;
    r->names_cap = cap;
  }
  const uint32_t id = (uint32_t)r->count;
  lnode * nd = &r->nodes[id];
  memset(nd, 0, sizeof *nd);
  nd->parent = parent;
  nd->depth = (uint32_t)depth;
  nd->origin = r->nodes[parent].origin;
  if (name) {
    nd->name_off = (uint32_t)r->names_len;
    nd->name_len = (uint32_t)n;
    memcpy(r->names + r->names_len, name, n);
    r->names[r->names_len + n] = '\0';
    r->names_len += n + 1;
    const GTEXT_CSS_Status st = table_insert(r, id);
    if (st != GTEXT_CSS_OK) {
      r->names_len = nd->name_off;
      return result_done(result, st);
    }
  }
  else {
    nd->anonymous = 1;
  }
  /* appended last among its parent's children: the order of first declaration */
  lnode * p = &r->nodes[parent];
  if (p->last_child) {
    r->nodes[p->last_child].next_sibling = id;
  }
  else {
    p->first_child = id;
  }
  p->last_child = id;
  r->count++;
  *out = id;
  return GTEXT_CSS_OK;
}

/// The node of a parent argument: the origin's root for none, else the layer, which must be of the origin.
static GTEXT_CSS_Status parent_node(const GTEXT_CSS_Layer_Registry * r, GTEXT_CSS_Origin origin, GTEXT_CSS_Layer_Id parent, uint32_t * out,
    GTEXT_CSS_Layer_Result * result) {
  if (!origin_has_layers(origin)) {
    return fail(result, GTEXT_CSS_E_INVALID, NULL, 0, "this origin has no cascade layers");
  }
  if (parent == GTEXT_CSS_LAYER_NONE) {
    *out = (uint32_t)origin + 1;
    return GTEXT_CSS_OK;
  }
  if (parent < FIRST_LAYER || parent >= r->count || r->nodes[parent].origin != (uint8_t)origin) {
    return fail(result, GTEXT_CSS_E_INVALID, NULL, 0, "the parent is not a layer of this registry and origin");
  }
  *out = parent;
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status segment_in(GTEXT_CSS_Layer_Registry * r, uint32_t parent, const char * name, size_t n, uint32_t * out,
    GTEXT_CSS_Layer_Result * result) {
  if (n == 0) {
    return fail(result, GTEXT_CSS_E_INVALID, "", 0, "a layer name segment is empty");
  }
  if (is_wide_keyword(name, n)) {
    return fail(result, GTEXT_CSS_E_INVALID, name, n, "a CSS-wide keyword is reserved and cannot be a layer name");
  }
  size_t probes = 0;
  const uint32_t found = table_find(r, parent, name, n, &probes);
  r->work += probes + 1;
  if (found) {
    *out = found;
    return GTEXT_CSS_OK;
  }
  return node_new(r, parent, name, n, out, result);
}

/* ---- paths ---------------------------------------------------------------------------------------------------- */

/// The end of the segment of a path that begins at `pos`: an unescaped `.`, or the end. A `\.` is a dot of the name and a `\\` a backslash of it.
static size_t seg_end(const char * path, size_t length, size_t pos) {
  while (pos < length && path[pos] != '.') {
    if (path[pos] == '\\' && pos + 1 < length && (path[pos + 1] == '.' || path[pos + 1] == '\\')) {
      pos++;
    }
    pos++;
  }
  return pos;
}

/// The bytes of the segment path[from, to): the text itself when it has no escape, else a decoded copy in `buf` (if it fits) or in `*heap`. NULL when memory ran out.
static const char * seg_decode(const GTEXT_Allocator * alloc, const char * path, size_t from, size_t to, char * buf, size_t cap, char ** heap, size_t * n) {
  *heap = NULL;
  bool escaped = false;
  for (size_t i = from; i + 1 < to && !escaped; i++) {
    escaped = path[i] == '\\' && (path[i + 1] == '.' || path[i + 1] == '\\');
  }
  if (!escaped) {
    *n = to - from;
    return path + from;
  }
  char * out = buf;
  if (to - from > cap) {
    GTEXT_Allocator a = *alloc;
    out = gtext_allocator_malloc(&a, to - from);
    if (!out) {
      return NULL;
    }
    *heap = out;
  }
  size_t k = 0;
  for (size_t i = from; i < to; i++) {
    if (path[i] == '\\' && i + 1 < to && (path[i + 1] == '.' || path[i + 1] == '\\')) {
      i++;
    }
    out[k++] = path[i];
  }
  *n = k;
  return out;
}

GTEXT_CSS_Status gtext_css_layer_declare_segment(GTEXT_CSS_Layer_Registry * r, GTEXT_CSS_Origin origin, GTEXT_CSS_Layer_Id parent,
    const char * name, size_t length, GTEXT_CSS_Layer_Id * out, GTEXT_CSS_Layer_Result * result) {
  result_clear(result);
  if (!r || (!name && length)) {
    return result_done(result, GTEXT_CSS_E_INVALID);
  }
  uint32_t p;
  GTEXT_CSS_Status st = parent_node(r, origin, parent, &p, result);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  uint32_t id = 0;
  st = segment_in(r, p, name ? name : "", length, &id, result);
  if (st == GTEXT_CSS_OK && out) {
    *out = id;
  }
  if (result) {
    result->work = r->work;
  }
  return result_done(result, st);
}

GTEXT_CSS_Status gtext_css_layer_declare(GTEXT_CSS_Layer_Registry * r, GTEXT_CSS_Origin origin, GTEXT_CSS_Layer_Id parent, const char * path,
    size_t length, GTEXT_CSS_Layer_Id * out, GTEXT_CSS_Layer_Result * result) {
  result_clear(result);
  if (!r || (!path && length)) {
    return result_done(result, GTEXT_CSS_E_INVALID);
  }
  uint32_t cur;
  GTEXT_CSS_Status st = parent_node(r, origin, parent, &cur, result);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  size_t pos = 0;
  for (;;) {
    const size_t end = seg_end(path, length, pos);
    char sbuf[256], * heap = NULL;
    size_t sn = 0;
    const char * seg = seg_decode(&r->alloc, path, pos, end, sbuf, sizeof sbuf, &heap, &sn);
    if (!seg) {
      return result_done(result, GTEXT_CSS_E_OOM);
    }
    uint32_t id = 0;
    st = segment_in(r, cur, seg, sn, &id, result);
    if (heap) {
      GTEXT_Allocator a = r->alloc;
      gtext_allocator_free(&a, heap);
    }
    if (st != GTEXT_CSS_OK) {
      if (result) {
        result->work = r->work;
      }
      return st;
    }
    cur = id;
    if (end >= length) {
      break;
    }
    pos = end + 1;
  }
  if (out) {
    *out = cur;
  }
  if (result) {
    result->work = r->work;
  }
  return result_done(result, GTEXT_CSS_OK);
}

GTEXT_CSS_Status gtext_css_layer_declare_anonymous(GTEXT_CSS_Layer_Registry * r, GTEXT_CSS_Origin origin, GTEXT_CSS_Layer_Id parent,
    GTEXT_CSS_Layer_Id * out, GTEXT_CSS_Layer_Result * result) {
  result_clear(result);
  if (!r) {
    return result_done(result, GTEXT_CSS_E_INVALID);
  }
  uint32_t p, id = 0;
  GTEXT_CSS_Status st = parent_node(r, origin, parent, &p, result);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  st = node_new(r, p, NULL, 0, &id, result);
  if (st == GTEXT_CSS_OK && out) {
    *out = id;
  }
  if (result) {
    result->work = r->work;
  }
  return result_done(result, st);
}

GTEXT_CSS_Layer_Id gtext_css_layer_find(const GTEXT_CSS_Layer_Registry * r, GTEXT_CSS_Origin origin, GTEXT_CSS_Layer_Id parent, const char * path,
    size_t length) {
  if (!r || (!path && length) || length == 0) {
    return GTEXT_CSS_LAYER_NONE;
  }
  uint32_t cur;
  if (parent_node(r, origin, parent, &cur, NULL) != GTEXT_CSS_OK) {
    return GTEXT_CSS_LAYER_NONE;
  }
  size_t pos = 0;
  for (;;) {
    const size_t end = seg_end(path, length, pos);
    if (end == pos) {
      return GTEXT_CSS_LAYER_NONE;
    }
    char sbuf[256], * heap = NULL;
    size_t sn = 0;
    const char * seg = seg_decode(&r->alloc, path, pos, end, sbuf, sizeof sbuf, &heap, &sn);
    if (!seg) {
      return GTEXT_CSS_LAYER_NONE;
    }
    cur = table_find(r, cur, seg, sn, NULL);
    if (heap) {
      GTEXT_Allocator a = r->alloc;
      gtext_allocator_free(&a, heap);
    }
    if (!cur) {
      return GTEXT_CSS_LAYER_NONE;
    }
    if (end >= length) {
      return cur;
    }
    pos = end + 1;
  }
}

GTEXT_CSS_Status gtext_css_layer_freeze(GTEXT_CSS_Layer_Registry * r) {
  if (!r) {
    return GTEXT_CSS_E_INVALID;
  }
  if (r->frozen) {
    return GTEXT_CSS_OK;
  }
  uint32_t * cursor = gtext_allocator_calloc(&r->alloc, r->count, sizeof *cursor);
  if (!cursor) {
    return GTEXT_CSS_E_OOM;
  }
  for (uint32_t i = 1; i < r->count; i++) {
    cursor[i] = r->nodes[i].first_child;
  }
  for (uint32_t root = 1; root <= ROOTS; root++) {
    /* post-order, without recursion: descend by the cursors, number a node when its children are done, climb by the parent */
    uint32_t counter = 0, cur = root;
    for (;;) {
      r->work++;
      if (cursor[cur]) {
        const uint32_t child = cursor[cur];
        cursor[cur] = r->nodes[child].next_sibling;
        cur = child;
        continue;
      }
      r->nodes[cur].rank = counter++;
      if (cur == root) {
        break;
      }
      cur = r->nodes[cur].parent;
    }
    r->origin_layers[root - 1] = (size_t)counter - 1;
  }
  gtext_allocator_free(&r->alloc, cursor);
  r->frozen = true;
  return GTEXT_CSS_OK;
}

bool gtext_css_layer_is_frozen(const GTEXT_CSS_Layer_Registry * r) {
  return r && r->frozen;
}

size_t gtext_css_layer_count(const GTEXT_CSS_Layer_Registry * r, GTEXT_CSS_Origin origin) {
  if (!r || !origin_has_layers(origin)) {
    return 0;
  }
  if (r->frozen) {
    return r->origin_layers[origin];
  }
  size_t n = 0;
  for (size_t i = FIRST_LAYER; i < r->count; i++) {
    n += r->nodes[i].origin == (uint8_t)origin;
  }
  return n;
}

bool gtext_css_layer_info(const GTEXT_CSS_Layer_Registry * r, GTEXT_CSS_Layer_Id layer, GTEXT_CSS_Layer_Info * out) {
  if (!out) {
    return false;
  }
  memset(out, 0, sizeof *out);
  if (!r || layer < FIRST_LAYER || layer >= r->count) {
    return false;
  }
  const lnode * nd = &r->nodes[layer];
  out->id = layer;
  out->origin = (GTEXT_CSS_Origin)nd->origin;
  out->parent = nd->parent >= FIRST_LAYER ? nd->parent : GTEXT_CSS_LAYER_NONE;
  out->anonymous = nd->anonymous != 0;
  out->segment = nd->anonymous ? NULL : r->names + nd->name_off;
  out->segment_length = nd->name_len;
  out->depth = nd->depth;
  out->rank = r->frozen ? nd->rank : 0;
  return true;
}

GTEXT_CSS_Status gtext_css_layer_rank(const GTEXT_CSS_Layer_Registry * r, GTEXT_CSS_Origin origin, GTEXT_CSS_Layer_Id layer, size_t * rank) {
  if (!r || !rank || !r->frozen || !origin_has_layers(origin)) {
    return GTEXT_CSS_E_INVALID;
  }
  if (layer == GTEXT_CSS_LAYER_NONE) {
    *rank = r->nodes[(uint32_t)origin + 1].rank;
    return GTEXT_CSS_OK;
  }
  if (layer < FIRST_LAYER || layer >= r->count || r->nodes[layer].origin != (uint8_t)origin) {
    return GTEXT_CSS_E_INVALID;
  }
  *rank = r->nodes[layer].rank;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_layer_compare(const GTEXT_CSS_Layer_Registry * r, GTEXT_CSS_Origin origin, GTEXT_CSS_Layer_Id a, GTEXT_CSS_Layer_Id b,
    int * order) {
  size_t ra, rb;
  if (!order) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Status st = gtext_css_layer_rank(r, origin, a, &ra);
  if (st == GTEXT_CSS_OK) {
    st = gtext_css_layer_rank(r, origin, b, &rb);
  }
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  *order = ra < rb ? -1 : ra > rb ? 1 : 0;
  return GTEXT_CSS_OK;
}

/// The length of a segment as it is written in a path (a dot or a backslash of the name is escaped).
static size_t seg_written(const char * name, size_t n) {
  size_t k = n;
  for (size_t i = 0; i < n; i++) {
    k += name[i] == '.' || name[i] == '\\';
  }
  return k;
}

size_t gtext_css_layer_path(const GTEXT_CSS_Layer_Registry * r, GTEXT_CSS_Layer_Id layer, char * buffer, size_t capacity) {
  if (!r || layer < FIRST_LAYER || layer >= r->count) {
    if (buffer && capacity) {
      buffer[0] = '\0';
    }
    return 0;
  }
  static const char anon[] = "(anonymous)";
  size_t total = 0;
  for (uint32_t id = layer; id >= FIRST_LAYER; id = r->nodes[id].parent) {
    total += (r->nodes[id].anonymous ? sizeof anon - 1 : seg_written(r->names + r->nodes[id].name_off, r->nodes[id].name_len)) + (id == layer ? 0 : 1);
  }
  if (buffer && capacity) {
    /* written from the end: each segment, and the dot in front of it when it has a parent layer */
    size_t end = total;
    for (uint32_t id = layer; id >= FIRST_LAYER; id = r->nodes[id].parent) {
      const char * seg = r->nodes[id].anonymous ? anon : r->names + r->nodes[id].name_off;
      const size_t raw = r->nodes[id].anonymous ? sizeof anon - 1 : r->nodes[id].name_len;
      const size_t len = r->nodes[id].anonymous ? raw : seg_written(seg, raw);
      end -= len;
      size_t at = end;
      for (size_t k = 0; k < raw; k++) {
        if (!r->nodes[id].anonymous && (seg[k] == '.' || seg[k] == '\\')) {
          if (at + 1 < capacity) {
            buffer[at] = '\\';
          }
          at++;
        }
        if (at + 1 < capacity) {
          buffer[at] = seg[k];
        }
        at++;
      }
      if (r->nodes[id].parent >= FIRST_LAYER) {
        end--;
        if (end + 1 < capacity) {
          buffer[end] = '.';
        }
      }
    }
    buffer[total < capacity ? total : capacity - 1] = '\0';
  }
  return total;
}

/* ---- reading a sheet ------------------------------------------------------------------------------------------ */

static bool ci_equal(const GTEXT_CSS_String * s, const char * word) {
  const size_t n = strlen(word);
  if (s->length != n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    char a = s->data[i];
    if (a >= 'A' && a <= 'Z') {
      a = (char)(a - 'A' + 'a');
    }
    if (a != word[i]) {
      return false;
    }
  }
  return true;
}

static bool is_ws(const GTEXT_CSS_Component_Value * v) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_WHITESPACE;
}

/// Read a `<layer-name>` from `v[0..n)`: idents separated by `.` delims with no white space between, and white space around. Returns the number of segments
/// (0: not a layer name) and their component values through `segs` (at most `max`).
static size_t read_name(const GTEXT_CSS_Component_Value * v, size_t n, const GTEXT_CSS_Component_Value ** segs, size_t max) {
  size_t i = 0;
  while (i < n && is_ws(&v[i])) {
    i++;
  }
  while (n > i && is_ws(&v[n - 1])) {
    n--;
  }
  size_t count = 0;
  bool expect_ident = true;
  for (; i < n; i++) {
    if (expect_ident) {
      if (v[i].kind != GTEXT_CSS_VALUE_TOKEN || v[i].type != GTEXT_CSS_TOKEN_IDENT || count == max) {
        return 0;
      }
      segs[count++] = &v[i];
      expect_ident = false;
    }
    else {
      if (v[i].kind != GTEXT_CSS_VALUE_TOKEN || v[i].type != GTEXT_CSS_TOKEN_DELIM || v[i].delim != '.') {
        return 0;
      }
      expect_ident = true;
    }
  }
  return expect_ident ? 0 : count;
}

#define MAX_SEGS 4096

typedef struct {
  const GTEXT_CSS_Rule * rules;            ///< A list of rules (a qualified rule's children, a tree's), or NULL.
  size_t rule_count;
  const GTEXT_CSS_Block_Item * items;      ///< An at-rule's contents, or NULL.
  size_t item_count;
  size_t pos;
  uint32_t layer;                          ///< The node new layers nest in.
  bool unresolved;                         ///< Inside a conditional rule the caller cannot evaluate.
  bool in_style;                           ///< Inside the block of a style rule (a nested group rule): a layer statement is not allowed here.
} frame;

typedef struct {
  GTEXT_CSS_Layer_Registry * reg;
  GTEXT_CSS_Origin origin;
  const GTEXT_CSS_Layer_Collect_Options * opt;
  GTEXT_CSS_Layer_Result * result;
  frame * stack;
  size_t sp, cap;
  size_t max_depth;
  bool imports_open;                       ///< No rule that closes the start of the sheet to @import has been seen at the top level.
  bool seen_import_or_namespace;           ///< An @import or @namespace rule has been seen at the top level.
  const GTEXT_CSS_Component_Value * segs[MAX_SEGS];
} walker;

static GTEXT_CSS_Status w_fail(walker * w, GTEXT_CSS_Status st, const GTEXT_CSS_Rule * rule, const char * name, const char * detail) {
  if (w->result) {
    w->result->span = rule->span;
    w->result->detail = detail;
    if (name) {
      result_name(w->result, name, strlen(name));
    }
  }
  return result_done(w->result, st);
}

static GTEXT_CSS_Status push(walker * w, const frame * f, const GTEXT_CSS_Rule * rule) {
  if (w->sp >= w->max_depth) {
    return w_fail(w, GTEXT_CSS_E_LIMIT_DEPTH, rule, "", "rules nested deeper than max_rule_depth");
  }
  if (w->sp == w->cap) {
    size_t cap = w->cap ? w->cap * 2 : 16;
    frame * g = gtext_allocator_realloc(&w->reg->alloc, w->stack, cap * sizeof *g);
    if (!g) {
      return result_done(w->result, GTEXT_CSS_E_OOM);
    }
    w->stack = g;
    w->cap = cap;
  }
  w->stack[w->sp++] = *f;
  return GTEXT_CSS_OK;
}

/// Declare the segments of a name under `parent` (a node index; roots allowed); `*out` is the last.
static GTEXT_CSS_Status declare_segs(walker * w, const GTEXT_CSS_Rule * rule, uint32_t parent, size_t n, uint32_t * out) {
  GTEXT_CSS_Layer_Registry * r = w->reg;
  uint32_t cur = parent;
  for (size_t i = 0; i < n; i++) {
    const GTEXT_CSS_String * s = &w->segs[i]->value;
    GTEXT_CSS_Layer_Result lr;
    result_clear(&lr);
    uint32_t id = 0;
    GTEXT_CSS_Status st = segment_in(r, cur, s->data ? s->data : "", s->length, &id, &lr);
    if (st != GTEXT_CSS_OK) {
      GTEXT_CSS_Layer_Result * res = w->result;
      if (res) {
        memcpy(res->name, lr.name, sizeof res->name);
        res->detail = lr.detail;
        res->span = rule->span;
      }
      return result_done(res, st);
    }
    cur = id;
  }
  *out = cur;
  return GTEXT_CSS_OK;
}

static bool conditional_name(const GTEXT_CSS_String * name) {
  return ci_equal(name, "media") || ci_equal(name, "supports") || ci_equal(name, "container") || ci_equal(name, "document") ||
      ci_equal(name, "-moz-document");
}

static bool grouping_name(const GTEXT_CSS_String * name) {
  return ci_equal(name, "scope") || ci_equal(name, "starting-style");
}

/// Whether every segment of the segs is a name and none a CSS-wide keyword (a rule that fails this is invalid and skipped).
static bool names_valid(walker * w, size_t n) {
  for (size_t i = 0; i < n; i++) {
    const GTEXT_CSS_String * s = &w->segs[i]->value;
    if (!s->data || s->length == 0 || is_wide_keyword(s->data, s->length)) {
      return false;
    }
  }
  return true;
}

static GTEXT_CSS_Status unresolved_layer(walker * w, const GTEXT_CSS_Rule * rule) {
  return w_fail(w, GTEXT_CSS_E_UNSUPPORTED, rule, "active_rule", "a layer is declared inside a conditional rule and no active callback was given");
}

/// A node for an anonymous layer, with the walker's result filled on failure.
static GTEXT_CSS_Status anonymous_in(walker * w, const GTEXT_CSS_Rule * rule, uint32_t parent, uint32_t * out) {
  GTEXT_CSS_Layer_Result lr;
  result_clear(&lr);
  const GTEXT_CSS_Status st = node_new(w->reg, parent, NULL, 0, out, &lr);
  if (st != GTEXT_CSS_OK && w->result) {
    memcpy(w->result->name, lr.name, sizeof w->result->name);
    w->result->detail = lr.detail;
    w->result->span = rule->span;
    return result_done(w->result, st);
  }
  return st;
}

static GTEXT_CSS_Status handle_layer(walker * w, const GTEXT_CSS_Rule * rule, frame * cur) {
  const GTEXT_CSS_Component_Value * p = rule->prelude;
  const size_t n = rule->prelude_count;
  if (!rule->has_block) {
    /* a statement, `@layer a, b.c;`: one invalid name makes the rule invalid and it declares nothing, so the list is checked first. A statement has no
       block, so it is not a nested group rule (CSS Nesting 3.3: "these rules are capable of being nested group rules: all the conditional group rules,
       @layer, @scope" -- the block form): inside a style rule it is not valid and declares nothing. */
    if (cur->in_style) {
      return GTEXT_CSS_OK;
    }
    for (size_t pass = 0; pass < 2; pass++) {
      size_t i = 0;
      for (;;) {
        size_t j = i;
        while (j < n && !(p[j].kind == GTEXT_CSS_VALUE_TOKEN && p[j].type == GTEXT_CSS_TOKEN_COMMA)) {
          j++;
        }
        const size_t cnt = read_name(j > i ? p + i : p, j - i, w->segs, MAX_SEGS); // p is NULL for a prelude of nothing
        if (cnt == 0 || !names_valid(w, cnt)) {
          return GTEXT_CSS_OK;
        }
        if (pass == 1) {
          uint32_t id;
          const GTEXT_CSS_Status st = declare_segs(w, rule, cur->layer, cnt, &id);
          if (st != GTEXT_CSS_OK) {
            return st;
          }
        }
        if (j >= n) {
          break;
        }
        i = j + 1;
      }
      if (pass == 0 && cur->unresolved) {
        return unresolved_layer(w, rule);
      }
    }
    /* Cascade 5, 6.4.4.2: "Any @layer rule that comes after an @import or @namespace rule will cause any subsequent @import or @namespace rules to be ignored." */
    if (w->sp == 1 && w->seen_import_or_namespace) {
      w->imports_open = false;
    }
    return GTEXT_CSS_OK;
  }
  /* a block, `@layer name? { ... }` */
  uint32_t layer = cur->layer;
  size_t i = 0;
  while (i < n && is_ws(&p[i])) {
    i++;
  }
  size_t cnt = 0;
  if (i < n) {
    cnt = read_name(p, n, w->segs, MAX_SEGS);
    if (cnt == 0 || !names_valid(w, cnt)) {
      return GTEXT_CSS_OK; /* invalid: the rule and its contents are skipped */
    }
  }
  if (cur->unresolved) {
    return unresolved_layer(w, rule);
  }
  if (w->sp == 1) {
    w->imports_open = false; /* a valid @layer block is a valid at-rule that no @import may follow */
  }
  if (i == n) {
    const GTEXT_CSS_Status st = anonymous_in(w, rule, cur->layer, &layer);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
  }
  else {
    const GTEXT_CSS_Status st = declare_segs(w, rule, cur->layer, cnt, &layer);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
  }
  if (w->opt && w->opt->on_layer) {
    w->opt->on_layer(w->opt->user, rule, (GTEXT_CSS_Layer_Id)layer);
  }
  frame f;
  memset(&f, 0, sizeof f);
  f.items = rule->contents;
  f.item_count = rule->content_count;
  f.layer = layer;
  f.unresolved = cur->unresolved;
  f.in_style = cur->in_style;
  return push(w, &f, rule);
}

/// `@import <url> layer` / `layer(name)` with conditions: the layer the sheet is read into.
///
/// Cascade 5, 6.2: "Any @import rules must precede all other valid at-rules and style rules in a style sheet (ignoring @charset, @supports-condition, and @layer
/// statement rules) and must not have any other valid at-rules or style rules between it and previous @import rules, or else the @import rule is invalid." An
/// import is read at the top of the sheet only, and needs a `<url>` or `<string>`. "The layer is added to the layer order even if the import fails to load the
/// stylesheet, but is subject to any import conditions (just as if declared by an @layer rule wrapped in @media and/or @supports blocks)".
static GTEXT_CSS_Status handle_import(walker * w, const GTEXT_CSS_Rule * rule, frame * cur) {
  if (w->sp != 1 || !w->imports_open) {
    return GTEXT_CSS_OK;
  }
  (void)cur;
  const GTEXT_CSS_Component_Value * p = rule->prelude;
  const size_t n = rule->prelude_count;
  size_t i = 0;
  while (i < n && is_ws(&p[i])) {
    i++;
  }
  const bool has_url = i < n && ((p[i].kind == GTEXT_CSS_VALUE_TOKEN && (p[i].type == GTEXT_CSS_TOKEN_URL || p[i].type == GTEXT_CSS_TOKEN_STRING)) ||
      (p[i].kind == GTEXT_CSS_VALUE_FUNCTION && ci_equal(&p[i].value, "url")));
  if (!has_url) {
    return GTEXT_CSS_OK; /* an @import with no <url> or <string> is invalid: ignored, and it closes nothing */
  }
  i++;
  while (i < n && is_ws(&p[i])) {
    i++;
  }
  bool keyword = false, function = false;
  size_t cnt = 0;
  if (i < n) {
    keyword = p[i].kind == GTEXT_CSS_VALUE_TOKEN && p[i].type == GTEXT_CSS_TOKEN_IDENT && ci_equal(&p[i].value, "layer");
    function = p[i].kind == GTEXT_CSS_VALUE_FUNCTION && ci_equal(&p[i].value, "layer");
    if (function) {
      cnt = read_name(p[i].children, p[i].child_count, w->segs, MAX_SEGS);
      if (cnt == 0 || !names_valid(w, cnt)) {
        return GTEXT_CSS_OK; /* an invalid layer name makes the import invalid */
      }
    }
    if (keyword || function) {
      i++;
    }
  }
  bool conditions = false;
  for (; i < n; i++) {
    conditions = conditions || !is_ws(&p[i]);
  }
  /* a valid import: the rules after it may still be imports */
  w->seen_import_or_namespace = true;
  if (!keyword && !function) {
    return GTEXT_CSS_OK;
  }
  if (conditions) {
    if (w->opt && w->opt->active) {
      if (!w->opt->active(w->opt->user, rule)) {
        return GTEXT_CSS_OK;
      }
    }
    else {
      return unresolved_layer(w, rule);
    }
  }
  uint32_t layer;
  const GTEXT_CSS_Status st = keyword ? anonymous_in(w, rule, cur->layer, &layer) : declare_segs(w, rule, cur->layer, cnt, &layer);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (w->opt && w->opt->on_layer) {
    w->opt->on_layer(w->opt->user, rule, (GTEXT_CSS_Layer_Id)layer);
  }
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status visit(walker * w, const GTEXT_CSS_Rule * rule) {
  frame * cur = &w->stack[w->sp - 1];
  w->reg->work++;
  const bool top = w->sp == 1;
  if (rule->kind == GTEXT_CSS_RULE_QUALIFIED) {
    if (top) {
      w->imports_open = false;
    }
    if (rule->rule_count) {
      frame f;
      memset(&f, 0, sizeof f);
      f.rules = rule->rules;
      f.rule_count = rule->rule_count;
      f.layer = cur->layer;
      f.unresolved = cur->unresolved;
      f.in_style = true;
      return push(w, &f, rule);
    }
    return GTEXT_CSS_OK;
  }
  if (rule->kind != GTEXT_CSS_RULE_AT) {
    if (top) {
      w->imports_open = false;
    }
    return GTEXT_CSS_OK;
  }
  if (ci_equal(&rule->name, "layer")) {
    return handle_layer(w, rule, cur);
  }
  if (ci_equal(&rule->name, "import")) {
    return handle_import(w, rule, cur);
  }
  if (top && !ci_equal(&rule->name, "charset")) {
    /* any other rule: no @import may follow it (and an @namespace is an @import's last follower) */
    w->imports_open = false;
    if (ci_equal(&rule->name, "namespace")) {
      w->seen_import_or_namespace = true;
    }
  }
  bool unresolved = cur->unresolved;
  if (conditional_name(&rule->name)) {
    if (w->opt && w->opt->active) {
      if (!w->opt->active(w->opt->user, rule)) {
        return GTEXT_CSS_OK;
      }
    }
    else {
      unresolved = true;
    }
  }
  else if (!grouping_name(&rule->name)) {
    return GTEXT_CSS_OK;
  }
  if (!rule->has_block || rule->content_count == 0) {
    return GTEXT_CSS_OK;
  }
  frame f;
  memset(&f, 0, sizeof f);
  f.items = rule->contents;
  f.item_count = rule->content_count;
  f.layer = cur->layer;
  f.unresolved = unresolved;
  f.in_style = cur->in_style;
  return push(w, &f, rule);
}

GTEXT_CSS_Status gtext_css_cascade_collect_layers(GTEXT_CSS_Layer_Registry * registry, GTEXT_CSS_Origin origin, GTEXT_CSS_Layer_Id base,
    const GTEXT_CSS_Tree * tree, const GTEXT_CSS_Layer_Collect_Options * options, GTEXT_CSS_Layer_Result * result) {
  result_clear(result);
  if (!registry || !tree) {
    return result_done(result, GTEXT_CSS_E_INVALID);
  }
  uint32_t root;
  GTEXT_CSS_Status st = parent_node(registry, origin, base, &root, result);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (registry->frozen) {
    return fail(result, GTEXT_CSS_E_INVALID, NULL, 0, "the registry is frozen");
  }
  walker * w = gtext_allocator_calloc(&registry->alloc, 1, sizeof *w);
  if (!w) {
    return result_done(result, GTEXT_CSS_E_OOM);
  }
  w->reg = registry;
  w->origin = origin;
  w->opt = options;
  w->result = result;
  w->imports_open = true;
  w->max_depth = options && options->max_rule_depth ? options->max_rule_depth : DEFAULT_RULE_DEPTH;
  if (w->max_depth > LIMIT_RULE_DEPTH) {
    w->max_depth = LIMIT_RULE_DEPTH;
  }
  frame f;
  memset(&f, 0, sizeof f);
  if (tree->kind == GTEXT_CSS_TREE_BLOCK_CONTENTS) {
    f.items = tree->items;
    f.item_count = tree->item_count;
  }
  else {
    f.rules = tree->rules;
    f.rule_count = tree->rule_count;
  }
  f.layer = root;
  GTEXT_CSS_Status status = push(w, &f, &(GTEXT_CSS_Rule){0});
  while (status == GTEXT_CSS_OK && w->sp > 0) {
    frame * top = &w->stack[w->sp - 1];
    const GTEXT_CSS_Rule * next = NULL;
    if (top->rules) {
      if (top->pos < top->rule_count) {
        next = &top->rules[top->pos++];
      }
    }
    else if (top->items) {
      while (top->pos < top->item_count) {
        const GTEXT_CSS_Block_Item * it = &top->items[top->pos++];
        if (it->kind == GTEXT_CSS_ITEM_RULE) {
          next = it->rule;
          break;
        }
      }
    }
    if (!next) {
      w->sp--;
      continue;
    }
    status = visit(w, next);
  }
  if (result) {
    result->work = registry->work;
  }
  gtext_allocator_free(&registry->alloc, w->stack);
  gtext_allocator_free(&registry->alloc, w);
  if (status == GTEXT_CSS_OK) {
    return result_done(result, GTEXT_CSS_OK);
  }
  return status;
}
