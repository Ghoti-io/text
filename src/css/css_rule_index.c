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
 * The rule index (css_sheet.h): the sheets of a document merged into one order of appearance and one layer order for each tree scope, and the records of each scope in buckets
 * by the id, a class or the local name of the subject, so that an element tests the records of the keys it has, and the rules with no key.
 *
 * Reading is const and takes the caches of the caller's match context, so any number of threads may collect from one frozen index, each with a context of its own.
 */

#include <stdlib.h>
#include <string.h>

#include "css_registry_internal.h"
#include "css_sheet_internal.h"

#define DEF_MAX_RECORDS ((size_t)16777216)
#define MAX_PLACE_DEPTH ((size_t)70)

typedef struct {
  char * key;
  size_t len;
  uint32_t * v;
  size_t n, cap;
} bucket;

typedef struct {
  bucket * b;
  size_t nb, capb;
  int32_t * slots;
  size_t nslots;
} bmap;

typedef struct {
  uint32_t * v;
  size_t n, cap;
} u32vec;

typedef struct {
  const void * scope;       ///< The scope handle; NULL: the document.
  uint32_t context;
  bool shadow;
  const void * parent_scope;
  GTEXT_CSS_Layer_Registry * layers;
  bool own_layers;
  bmap ids, classes, tags;
  u32vec other, host, slotted, part;
} group;

typedef struct {
  GTEXT_CSS_Sheet * sheet;
  GTEXT_CSS_Origin origin;
  uint32_t number;
  group * g;
  uint32_t * lmap;
  size_t lmap_n;
  GTEXT_CSS_Element_Handle implicit_root;
  bool owned;                 ///< The index holds an owner of the sheet (the sheets it imports are owned by it).
} placed;

typedef struct {
  uint32_t pl, rec;
} entry;

struct GTEXT_CSS_Rule_Index {
  const GTEXT_CSS_Registry * registry;
  GTEXT_Allocator alloc;
  GTEXT_CSS_Rule_Index_Options opt;
  placed ** pls;
  size_t npl, cappl;
  group ** groups;
  size_t ng, capg;
  int32_t * gslots; ///< By scope handle, built when frozen.
  size_t ngslots;
  entry * all;
  size_t nall;
  size_t total;
  uint64_t seed;
  bool frozen, failed, has_shadow;
  bool has_part; ///< some sheet, a document's too, has a rule ending in `::part()`: it reaches into shadow trees whether or not one has a sheet of its own
};

GTEXT_CSS_Rule_Index_Options gtext_css_rule_index_options_default(void) {
  GTEXT_CSS_Rule_Index_Options o;
  memset(&o, 0, sizeof o);
  o.max_records = DEF_MAX_RECORDS;
  return o;
}

/* ---- small containers ------------------------------------------------------------------------------------- */

static bool vgrow(const GTEXT_Allocator * a, void ** p, size_t * cap, size_t need, size_t elem) {
  if (need <= *cap) {
    return true;
  }
  size_t n = *cap ? *cap * 2 : 8;
  while (n < need) {
    n *= 2;
  }
  if (n > ((size_t)-1) / elem) {
    return false;
  }
  void * q = gtext_allocator_realloc(a, *p, n * elem);
  if (!q) {
    return false;
  }
  *p = q;
  *cap = n;
  return true;
}

static bool vpush(const GTEXT_Allocator * a, u32vec * v, uint32_t x) {
  if (!vgrow(a, (void **)&v->v, &v->cap, v->n + 1, sizeof *v->v)) {
    return false;
  }
  v->v[v->n++] = x;
  return true;
}

static uint64_t mix(uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdull;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ull;
  x ^= x >> 33;
  return x;
}

static uint64_t khash(uint64_t seed, const char * k, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull ^ mix(seed);
  for (size_t i = 0; i < n; i++) {
    h ^= (unsigned char)k[i];
    h *= 0x100000001b3ull;
  }
  return mix(h ^ seed);
}

static bucket * bmap_find(const bmap * m, uint64_t seed, const char * k, size_t n) {
  if (!m->nslots) {
    return NULL;
  }
  size_t i = (size_t)(khash(seed, k, n) & (m->nslots - 1));
  for (;;) {
    const int32_t s = m->slots[i];
    if (s < 0) {
      return NULL;
    }
    bucket * b = &m->b[s];
    if (b->len == n && memcmp(b->key, k, n) == 0) {
      return b;
    }
    i = (i + 1) & (m->nslots - 1);
  }
}

static bucket * bmap_get(const GTEXT_Allocator * a, bmap * m, uint64_t seed, const char * k, size_t n) {
  bucket * f = bmap_find(m, seed, k, n);
  if (f) {
    return f;
  }
  if ((m->nb + 1) * 2 > m->nslots) {
    const size_t ns = m->nslots ? m->nslots * 2 : 16;
    int32_t * sl = gtext_allocator_malloc(a, ns * sizeof *sl);
    if (!sl) {
      return NULL;
    }
    memset(sl, 0xff, ns * sizeof *sl);
    for (size_t j = 0; j < m->nb; j++) {
      size_t i = (size_t)(khash(seed, m->b[j].key, m->b[j].len) & (ns - 1));
      while (sl[i] >= 0) {
        i = (i + 1) & (ns - 1);
      }
      sl[i] = (int32_t)j;
    }
    gtext_allocator_free(a, m->slots);
    m->slots = sl;
    m->nslots = ns;
  }
  if (!vgrow(a, (void **)&m->b, &m->capb, m->nb + 1, sizeof *m->b)) {
    return NULL;
  }
  char * kc = gtext_allocator_malloc(a, n + 1);
  if (!kc) {
    return NULL;
  }
  memcpy(kc, k, n);
  kc[n] = '\0';
  bucket * b = &m->b[m->nb];
  memset(b, 0, sizeof *b);
  b->key = kc;
  b->len = n;
  size_t i = (size_t)(khash(seed, k, n) & (m->nslots - 1));
  while (m->slots[i] >= 0) {
    i = (i + 1) & (m->nslots - 1);
  }
  m->slots[i] = (int32_t)m->nb;
  m->nb++;
  return b;
}

static void bmap_free(const GTEXT_Allocator * a, bmap * m) {
  for (size_t i = 0; i < m->nb; i++) {
    gtext_allocator_free(a, m->b[i].key);
    gtext_allocator_free(a, m->b[i].v);
  }
  gtext_allocator_free(a, m->b);
  gtext_allocator_free(a, m->slots);
  memset(m, 0, sizeof *m);
}

/* ---- the index ----------------------------------------------------------------------------------------------- */

static void group_free(const GTEXT_Allocator * a, group * g) {
  if (!g) {
    return;
  }
  bmap_free(a, &g->ids);
  bmap_free(a, &g->classes);
  bmap_free(a, &g->tags);
  gtext_allocator_free(a, g->other.v);
  gtext_allocator_free(a, g->host.v);
  gtext_allocator_free(a, g->slotted.v);
  gtext_allocator_free(a, g->part.v);
  if (g->own_layers) {
    gtext_css_layer_registry_free(g->layers);
  }
  gtext_allocator_free(a, g);
}

GTEXT_CSS_Status gtext_css_rule_index_new(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Rule_Index_Options * options, GTEXT_CSS_Rule_Index ** out) {
  if (!registry || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Rule_Index_Options o = options ? *options : gtext_css_rule_index_options_default();
  const GTEXT_Allocator alloc = o.allocator ? *o.allocator : *css_registry_allocator(registry);
  GTEXT_CSS_Rule_Index * x = gtext_allocator_calloc(&alloc, 1, sizeof *x);
  if (!x) {
    return GTEXT_CSS_E_OOM;
  }
  x->registry = registry;
  x->alloc = alloc;
  x->opt = o;
  x->opt.allocator = NULL;
  if (!x->opt.max_records) {
    x->opt.max_records = DEF_MAX_RECORDS;
  }
  x->seed = mix((uint64_t)(uintptr_t)x ^ ((uint64_t)(uintptr_t)&o << 1));
  *out = x;
  return GTEXT_CSS_OK;
}

void gtext_css_rule_index_free(GTEXT_CSS_Rule_Index * x) {
  if (!x) {
    return;
  }
  const GTEXT_Allocator a = x->alloc;
  for (size_t i = 0; i < x->npl; i++) {
    placed * p = x->pls[i];
    if (!p) {
      continue;
    }
    if (p->owned) {
      gtext_css_sheet_free(p->sheet);
    }
    gtext_allocator_free(&a, p->lmap);
    gtext_allocator_free(&a, p);
  }
  for (size_t i = 0; i < x->ng; i++) {
    group_free(&a, x->groups[i]);
  }
  gtext_allocator_free(&a, x->pls);
  gtext_allocator_free(&a, x->groups);
  gtext_allocator_free(&a, x->gslots);
  gtext_allocator_free(&a, x->all);
  gtext_allocator_free(&a, x);
}

static GTEXT_CSS_Status ires(GTEXT_CSS_Rule_Index_Result * r, GTEXT_CSS_Status st, const char * name, const char * detail) {
  if (r) {
    r->status = st;
    r->message = gtext_css_status_string(st);
    if (name) {
      css_copy_name(r->name, sizeof r->name, name);
    }
    r->detail = detail;
  }
  return st;
}

static void ires_init(GTEXT_CSS_Rule_Index_Result * r) {
  if (r) {
    memset(r, 0, sizeof *r);
    r->message = gtext_css_status_string(GTEXT_CSS_OK);
  }
}

static group * find_group_scope(const GTEXT_CSS_Rule_Index * x, const void * scope) {
  if (x->gslots) {
    size_t i = (size_t)(mix((uint64_t)(uintptr_t)scope ^ x->seed) & (x->ngslots - 1));
    for (;;) {
      const int32_t s = x->gslots[i];
      if (s < 0) {
        return NULL;
      }
      if (x->groups[s]->scope == scope) {
        return x->groups[s];
      }
      i = (i + 1) & (x->ngslots - 1);
    }
  }
  for (size_t i = 0; i < x->ng; i++) {
    if (x->groups[i]->scope == scope) {
      return x->groups[i];
    }
  }
  return NULL;
}

static GTEXT_CSS_Status place(GTEXT_CSS_Rule_Index * x, GTEXT_CSS_Sheet * sheet, group * g, GTEXT_CSS_Origin origin, uint32_t base, GTEXT_CSS_Element_Handle implicit_root,
    size_t depth, GTEXT_CSS_Rule_Index_Result * res) {
  if (depth > MAX_PLACE_DEPTH) {
    return ires(res, GTEXT_CSS_E_LIMIT_RULE_NESTING, "imports", "imports are nested deeper than the index reads");
  }
  if (sheet->nrecs > x->opt.max_records || x->total > x->opt.max_records - sheet->nrecs) {
    return ires(res, GTEXT_CSS_E_LIMIT_SHEET_SIZE, "max_records", "the index would hold more records than max_records");
  }
  placed * p = gtext_allocator_calloc(&x->alloc, 1, sizeof *p);
  if (!p) {
    return ires(res, GTEXT_CSS_E_OOM, NULL, NULL);
  }
  p->sheet = sheet;
  p->origin = origin;
  p->g = g;
  p->implicit_root = implicit_root;
  p->lmap_n = gtext_css_sheet_layer_count(sheet) + 1;
  p->lmap = gtext_allocator_calloc(&x->alloc, p->lmap_n, sizeof *p->lmap);
  if (!p->lmap) {
    gtext_allocator_free(&x->alloc, p);
    return ires(res, GTEXT_CSS_E_OOM, NULL, NULL);
  }
  p->lmap[0] = base;
  x->total += sheet->nrecs;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  for (size_t i = 0; i < sheet->nevents && st == GTEXT_CSS_OK; i++) {
    const sheet_event * ev = &sheet->events[i];
    if (ev->kind == 0) {
      GTEXT_CSS_Layer_Info info;
      if (!gtext_css_layer_info(sheet->layers, sheet_layer_id(sheet, ev->node), &info) || sheet_layer_no(sheet, info.parent) >= p->lmap_n) {
        st = GTEXT_CSS_E_INTERNAL;
        break;
      }
      GTEXT_CSS_Layer_Id id = 0;
      GTEXT_CSS_Layer_Result lr;
      memset(&lr, 0, sizeof lr);
      const GTEXT_CSS_Layer_Id parent = p->lmap[sheet_layer_no(sheet, info.parent)];
      st = info.anonymous ? gtext_css_layer_declare_anonymous(g->layers, origin, parent, &id, &lr) :
                            gtext_css_layer_declare_segment(g->layers, origin, parent, info.segment, info.segment_length, &id, &lr);
      if (st != GTEXT_CSS_OK) {
        ires(res, st, lr.name, lr.detail);
        break;
      }
      p->lmap[ev->node] = id;
    }
    else {
      GTEXT_CSS_Sheet * child = sheet->imports[ev->import];
      const uint32_t cbase = ev->node ? p->lmap[ev->node] : p->lmap[0];
      st = place(x, child, g, origin, cbase, implicit_root, depth + 1, res);
    }
  }
  if (st != GTEXT_CSS_OK) {
    if (res && res->status == GTEXT_CSS_OK) {
      ires(res, st, NULL, NULL);
    }
    gtext_allocator_free(&x->alloc, p->lmap);
    gtext_allocator_free(&x->alloc, p);
    return st;
  }
  if (!vgrow(&x->alloc, (void **)&x->pls, &x->cappl, x->npl + 1, sizeof *x->pls)) {
    gtext_allocator_free(&x->alloc, p->lmap);
    gtext_allocator_free(&x->alloc, p);
    return ires(res, GTEXT_CSS_E_OOM, NULL, NULL);
  }
  p->number = (uint32_t)x->npl;
  x->pls[x->npl++] = p;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_rule_index_add_sheet(GTEXT_CSS_Rule_Index * x, GTEXT_CSS_Sheet * sheet, const GTEXT_CSS_Sheet_Registration * reg,
    GTEXT_CSS_Rule_Index_Result * res) {
  ires_init(res);
  if (!x || !sheet || !reg) {
    return ires(res, GTEXT_CSS_E_INVALID, NULL, "a NULL argument");
  }
  if (x->frozen || x->failed) {
    return ires(res, GTEXT_CSS_E_INVALID, NULL, x->frozen ? "the index is frozen" : "an earlier add failed: the index is not usable");
  }
  if (sheet->registry != x->registry) {
    return ires(res, GTEXT_CSS_E_INVALID, NULL, "the sheet was compiled with another registry");
  }
  if (reg->origin != GTEXT_CSS_ORIGIN_USER_AGENT && reg->origin != GTEXT_CSS_ORIGIN_USER && reg->origin != GTEXT_CSS_ORIGIN_AUTHOR) {
    return ires(res, GTEXT_CSS_E_INVALID, "origin", "a sheet is of the user-agent, user or author origin");
  }
  if (reg->shadow != (reg->tree_scope != NULL) || (!reg->shadow && reg->context != 0) || (reg->shadow && reg->context == 0)) {
    return ires(res, GTEXT_CSS_E_INVALID, "context", "the document is context 0 with no scope handle; a shadow tree has a handle and a context above 0");
  }
  group * g = find_group_scope(x, reg->tree_scope);
  if (!g) {
    for (size_t i = 0; i < x->ng; i++) {
      if (x->groups[i]->context == reg->context) {
        return ires(res, GTEXT_CSS_E_INVALID, "context", "two scopes have one context number");
      }
    }
    g = gtext_allocator_calloc(&x->alloc, 1, sizeof *g);
    if (!g) {
      return ires(res, GTEXT_CSS_E_OOM, NULL, NULL);
    }
    g->scope = reg->tree_scope;
    g->context = reg->context;
    g->shadow = reg->shadow;
    g->parent_scope = reg->parent_scope;
    if (reg->layers) {
      g->layers = reg->layers;
    }
    else {
      GTEXT_CSS_Layer_Options lo = gtext_css_layer_options_default();
      lo.allocator = &x->alloc;
      if (gtext_css_layer_registry_new(&lo, &g->layers) != GTEXT_CSS_OK) {
        gtext_allocator_free(&x->alloc, g);
        return ires(res, GTEXT_CSS_E_OOM, NULL, NULL);
      }
      g->own_layers = true;
    }
    if (!vgrow(&x->alloc, (void **)&x->groups, &x->capg, x->ng + 1, sizeof *x->groups)) {
      group_free(&x->alloc, g);
      return ires(res, GTEXT_CSS_E_OOM, NULL, NULL);
    }
    x->groups[x->ng++] = g;
    x->has_shadow = x->has_shadow || reg->shadow;
  }
  else if (g->context != reg->context || g->shadow != reg->shadow || g->parent_scope != reg->parent_scope ||
      (reg->layers && reg->layers != g->layers) || (!reg->layers && !g->own_layers)) {
    return ires(res, GTEXT_CSS_E_INVALID, "context", "a scope is registered again with another context, parent or layer registry");
  }
  if (gtext_css_layer_is_frozen(g->layers)) {
    return ires(res, GTEXT_CSS_E_INVALID, "layers", "the layer registry of the context is frozen");
  }
  GTEXT_CSS_Status st = place(x, sheet, g, reg->origin, 0, reg->implicit_root, 0, res);
  if (st != GTEXT_CSS_OK) {
    x->failed = true;
    return st;
  }
  // the index owns the sheet it was given (the sheets it imports are owned by it); the top sheet's placement is the last one made
  gtext_css_sheet_retain(sheet);
  x->pls[x->npl - 1]->owned = true;
  return GTEXT_CSS_OK;
}

/* ---- freezing ------------------------------------------------------------------------------------------------- */

static void lower_copy(char * dst, const char * s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    dst[i] = (char)css_sel_lower((unsigned char)s[i]);
  }
}

/// Put a record in the bucket of its subject.
static GTEXT_CSS_Status bucket_record(GTEXT_CSS_Rule_Index * x, group * g, uint32_t ei, const sheet_record * r) {
  const GTEXT_CSS_Complex * cx = &r->list->complex[r->cx];
  const GTEXT_CSS_Compound * last = &cx->compounds[cx->compound_count - 1];
  const GTEXT_Allocator * a = &x->alloc;
  if (r->flags & REC_SLOTTED) {
    return vpush(a, &g->slotted, ei) ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
  }
  if (r->flags & REC_PART) {
    x->has_part = true;
    return vpush(a, &g->part, ei) ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
  }
  if (r->flags & REC_HOST) {
    return vpush(a, &g->host, ei) ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
  }
  if (g->shadow && css_compound_names_host(last) && !vpush(a, &g->host, ei)) { // the host may be the subject without a :host beside the rest (`:is(:host)`, a nested `&`): tried there, and as usual below
    return GTEXT_CSS_E_OOM;
  }
  const GTEXT_CSS_Simple *id = NULL, *cls = NULL, *type = NULL;
  for (size_t i = 0; i < last->first_pseudo_element; i++) {
    const GTEXT_CSS_Simple * s = &last->simples[i];
    if (s->kind == GTEXT_CSS_SIMPLE_ID && !id) {
      id = s;
    }
    else if (s->kind == GTEXT_CSS_SIMPLE_CLASS && !cls) {
      cls = s;
    }
    else if (s->kind == GTEXT_CSS_SIMPLE_TYPE && !type) {
      type = s;
    }
  }
  bmap * m = NULL;
  const GTEXT_CSS_Simple * key = NULL;
  bool fold = false;
  if (id) {
    m = &g->ids;
    key = id;
    fold = x->opt.quirks;
  }
  else if (cls) {
    m = &g->classes;
    key = cls;
    fold = x->opt.quirks;
  }
  else if (type && type->name.length) {
    m = &g->tags;
    key = type;
    fold = x->opt.html_document;
  }
  if (!m) {
    return vpush(a, &g->other, ei) ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
  }
  char stack[128];
  char * k = stack;
  if (key->name.length > sizeof stack) {
    k = gtext_allocator_malloc(a, key->name.length);
    if (!k) {
      return GTEXT_CSS_E_OOM;
    }
  }
  if (fold) {
    lower_copy(k, key->name.data, key->name.length);
  }
  else {
    memcpy(k, key->name.data, key->name.length);
  }
  bucket * b = bmap_get(a, m, x->seed, k, key->name.length);
  const bool ok = b && vgrow(a, (void **)&b->v, &b->cap, b->n + 1, sizeof *b->v);
  if (ok) {
    b->v[b->n++] = ei;
  }
  if (k != stack) {
    gtext_allocator_free(a, k);
  }
  return ok ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
}

GTEXT_CSS_Status gtext_css_rule_index_freeze(GTEXT_CSS_Rule_Index * x, GTEXT_CSS_Rule_Index_Result * res) {
  ires_init(res);
  if (!x) {
    return ires(res, GTEXT_CSS_E_INVALID, NULL, "a NULL argument");
  }
  if (x->frozen) {
    return GTEXT_CSS_OK;
  }
  if (x->failed) {
    return ires(res, GTEXT_CSS_E_INVALID, NULL, "an earlier add failed: the index is not usable");
  }
  for (size_t i = 0; i < x->ng; i++) {
    const group * g = x->groups[i];
    if (!g->shadow) {
      continue;
    }
    const group * parent = find_group_scope(x, g->parent_scope);
    if (parent && g->context <= parent->context) {
      return ires(res, GTEXT_CSS_E_INVALID, "context order", "a shadow tree's context must be greater than the context of the scope that holds its host");
    }
  }
  size_t ngs = 16;
  while (ngs < x->ng * 2) {
    ngs *= 2;
  }
  x->gslots = gtext_allocator_malloc(&x->alloc, ngs * sizeof *x->gslots);
  if (!x->gslots) {
    return ires(res, GTEXT_CSS_E_OOM, NULL, NULL);
  }
  memset(x->gslots, 0xff, ngs * sizeof *x->gslots);
  x->ngslots = ngs;
  for (size_t j = 0; j < x->ng; j++) {
    size_t i = (size_t)(mix((uint64_t)(uintptr_t)x->groups[j]->scope ^ x->seed) & (ngs - 1));
    while (x->gslots[i] >= 0) {
      i = (i + 1) & (ngs - 1);
    }
    x->gslots[i] = (int32_t)j;
  }
  x->all = gtext_allocator_malloc(&x->alloc, (x->total ? x->total : 1) * sizeof *x->all);
  if (!x->all) {
    return ires(res, GTEXT_CSS_E_OOM, NULL, NULL);
  }
  for (size_t pi = 0; pi < x->npl; pi++) {
    const placed * p = x->pls[pi];
    for (size_t ri = 0; ri < p->sheet->nrecs; ri++) {
      const uint32_t ei = (uint32_t)x->nall;
      x->all[x->nall++] = (entry){(uint32_t)pi, (uint32_t)ri};
      const GTEXT_CSS_Status st = bucket_record(x, p->g, ei, &p->sheet->recs[ri]);
      if (st != GTEXT_CSS_OK) {
        return ires(res, st, NULL, NULL);
      }
    }
  }
  for (size_t i = 0; i < x->ng; i++) {
    const GTEXT_CSS_Status st = gtext_css_layer_freeze(x->groups[i]->layers);
    if (st != GTEXT_CSS_OK) {
      return ires(res, st, NULL, NULL);
    }
  }
  x->frozen = true;
  return GTEXT_CSS_OK;
}

const GTEXT_CSS_Layer_Registry * gtext_css_rule_index_layers(const GTEXT_CSS_Rule_Index * x, uint32_t context) {
  if (!x) {
    return NULL;
  }
  for (size_t i = 0; i < x->ng; i++) {
    if (x->groups[i]->context == context) {
      return x->groups[i]->layers;
    }
  }
  return NULL;
}

size_t gtext_css_rule_index_record_count(const GTEXT_CSS_Rule_Index * x) {
  return x ? x->total : 0;
}

size_t gtext_css_rule_index_sheet_count(const GTEXT_CSS_Rule_Index * x) {
  return x ? x->npl : 0;
}

/* ---- collecting ------------------------------------------------------------------------------------------------- */

typedef struct {
  uint32_t number, run;
  GTEXT_CSS_Specificity spec;
  uint32_t ei;
  uint32_t dist;
  bool scoped;
} hit;

/// Whether an element is a scoping root of a scope for the subject being collected: the answers of valid_root(), kept for the one subject, so that the nested scopes of a sheet cost
/// the depth of the tree and not its power.
typedef struct {
  const placed * pl; ///< the placement (the same sheet placed twice has its own implicit root and tree scope)
  const sheet_scope * sc;
  const void * root;
  uint8_t val; ///< 0 empty, 1 no, 2 yes
} rootmemo;

typedef struct {
  const GTEXT_CSS_Rule_Index * x;
  struct GTEXT_CSS_Selector_Match_Context * ctx;
  rootmemo * rm;
  size_t rm_cap, rm_used;
  El element;
  const GTEXT_CSS_Complex * request;
  hit * hits;
  size_t nhits, caphits;
  El orig_scope;
  const void * orig_ts;
  const css_nest * orig_nest;
  GTEXT_CSS_Status status;
} walk;

static El parent_of(struct GTEXT_CSS_Selector_Match_Context * ctx, El e) {
  return ctx->iface.parent(ctx->iface.user, e);
}

static size_t rm_hash(const placed * pl, const sheet_scope * sc, const void * r) {
  uint64_t h = (uint64_t)(uintptr_t)sc * 0x9e3779b97f4a7c15ull ^ (uint64_t)(uintptr_t)r * 0xc2b2ae3d27d4eb4full ^ (uint64_t)(uintptr_t)pl * 0xff51afd7ed558ccdull;
  h ^= h >> 29;
  return (size_t)h;
}

static uint8_t rm_get(const walk * w, const placed * pl, const sheet_scope * sc, const void * r) {
  if (!w->rm_cap) {
    return 0;
  }
  for (size_t i = rm_hash(pl, sc, r) & (w->rm_cap - 1);; i = (i + 1) & (w->rm_cap - 1)) {
    const rootmemo * e = &w->rm[i];
    if (!e->val) {
      return 0;
    }
    if (e->pl == pl && e->sc == sc && e->root == r) {
      return e->val;
    }
  }
}

static void rm_put(walk * w, const placed * pl, const sheet_scope * sc, const void * r, uint8_t val) {
  if ((w->rm_used + 1) * 2 > w->rm_cap) {
    const size_t ncap = w->rm_cap ? w->rm_cap * 2 : 64;
    rootmemo * n = gtext_allocator_calloc(&w->ctx->alloc, ncap, sizeof *n);
    if (!n) {
      css_match_fail(w->ctx, GTEXT_CSS_E_OOM);
      return;
    }
    for (size_t k = 0; k < w->rm_cap; k++) {
      if (w->rm[k].val) {
        size_t i = rm_hash(w->rm[k].pl, w->rm[k].sc, w->rm[k].root) & (ncap - 1);
        while (n[i].val) {
          i = (i + 1) & (ncap - 1);
        }
        n[i] = w->rm[k];
      }
    }
    gtext_allocator_free(&w->ctx->alloc, w->rm);
    w->rm = n;
    w->rm_cap = ncap;
  }
  size_t i = rm_hash(pl, sc, r) & (w->rm_cap - 1);
  while (w->rm[i].val) {
    i = (i + 1) & (w->rm_cap - 1);
  }
  w->rm[i] = (rootmemo){pl, sc, r, val};
  w->rm_used++;
}

static bool valid_root_compute(walk * w, const placed * pl, const sheet_scope * sc, El r, El x);

/// Whether `r` is a scoping root of `sc` for the element `x` (`r` is an inclusive ancestor of `x`): it matches the start, and nothing on the way from `x` up to `r` is a scoping
/// limit. The start of an `@scope` inside another is read with `:scope` (and so `&`) an outer root that `r` is inside, any that makes it match (an iteration up the ancestors, and
/// a recursion only as deep as the nesting of the scopes, which is bounded).
static bool valid_root(walk * w, const placed * pl, const sheet_scope * sc, El r, El x) {
  const uint8_t known = rm_get(w, pl, sc, r);
  if (known) {
    return known == 2;
  }
  const bool v = valid_root_compute(w, pl, sc, r, x);
  if (w->ctx->status == GTEXT_CSS_OK) {
    rm_put(w, pl, sc, r, v ? 2 : 1);
  }
  return v;
}

static bool valid_root_compute(walk * w, const placed * pl, const sheet_scope * sc, El r, El x) {
  struct GTEXT_CSS_Selector_Match_Context * ctx = w->ctx;
  bool started = false;
  if (!sc->start) {
    started = pl->implicit_root && r == pl->implicit_root;
  }
  else if (!sc->parent) {
    ctx->scope = w->orig_scope;
    ctx->nest = sc->outer_nest;
    started = css_match_any(ctx, sc->start, r) && ctx->status == GTEXT_CSS_OK;
  }
  else {
    for (El outer = r; outer && !started; outer = ctx->iface.parent(ctx->iface.user, outer)) {
      if (!css_match_work(ctx)) {
        return false;
      }
      if (!valid_root(w, pl, sc->parent, outer, x)) { // the subject is what the outer limit is about, not the inner root
        if (ctx->status != GTEXT_CSS_OK) {
          return false;
        }
        continue;
      }
      ctx->scope = outer;
      ctx->nest = sc->outer_nest;
      started = css_match_any(ctx, sc->start, r);
      if (ctx->status != GTEXT_CSS_OK) {
        return false;
      }
    }
  }
  if (!started) {
    return false;
  }
  if (sc->end) {
    ctx->scope = r;
    ctx->nest = NULL;
    for (El y = x; y; y = y == r ? NULL : ctx->iface.parent(ctx->iface.user, y)) { // up to and including the root: a limit that matches the root (`to (:scope)`) leaves nothing in scope
      if (!css_match_work(ctx)) {
        return false;
      }
      if (css_match_any(ctx, sc->end, y)) {
        return false;
      }
      if (ctx->status != GTEXT_CSS_OK) {
        return false;
      }
    }
  }
  return true;
}

/// Match a record against the element, in or out of an `@scope`. `slot` and `host` are for `::slotted()` and `::part()`.
static bool record_matches(walk * w, const placed * pl, const sheet_record * r, El slot, El host, uint32_t * dist, bool * scoped) {
  struct GTEXT_CSS_Selector_Match_Context * ctx = w->ctx;
  const GTEXT_CSS_Complex * cx = &r->list->complex[r->cx];
  *scoped = false;
  *dist = 0;
  if (r->list->callbacks & ~ctx->avail) {
    for (int cb = 0; cb < (int)GTEXT_CSS_ELEMENT_CB_COUNT; cb++) {
      if ((r->list->callbacks & ~ctx->avail) >> cb & 1u) {
        css_match_missing(ctx, (GTEXT_CSS_Element_Callback)cb);
        return false;
      }
    }
  }
  if (!r->scope) {
    ctx->scope = w->orig_scope;
    ctx->nest = r->nest;
    return css_match_complex_full(ctx, r->list, cx, w->element, w->request, slot, host);
  }
  *scoped = true;
  if (!CSS_HAVE(ctx, GTEXT_CSS_ELEMENT_CB_PARENT)) {
    css_match_missing(ctx, GTEXT_CSS_ELEMENT_CB_PARENT);
    return false;
  }
  size_t h = 0;
  for (El root = w->element; root; root = parent_of(ctx, root), h++) {
    if (!css_match_work(ctx)) {
      return false;
    }
    if (!valid_root(w, pl, r->scope, root, w->element)) {
      if (ctx->status != GTEXT_CSS_OK) {
        return false;
      }
      continue;
    }
    ctx->scope = root;
    ctx->nest = r->nest;
    if (css_match_complex_full(ctx, r->list, cx, w->element, w->request, slot, host)) {
      *dist = (uint32_t)h;
      return true;
    }
    if (ctx->status != GTEXT_CSS_OK) {
      return false;
    }
  }
  return false;
}

static bool add_hit(walk * w, const placed * pl, uint32_t ei, const sheet_record * r, uint32_t dist, bool scoped) {
  if (w->nhits == w->caphits) {
    if (!vgrow(&w->ctx->alloc, (void **)&w->hits, &w->caphits, w->nhits + 1, sizeof *w->hits)) {
      w->status = GTEXT_CSS_E_OOM;
      return false;
    }
  }
  hit * h = &w->hits[w->nhits++];
  h->number = pl->number;
  h->run = r->run;
  h->spec = r->spec;
  h->ei = ei;
  h->dist = dist;
  h->scoped = scoped;
  return true;
}

/// Test the records of a list (indexes into `all`), those that apply to what is asked (the element, or the pseudo-element).
static bool test_list(walk * w, const u32vec * v, El slot, El host, const void * tree_scope) {
  struct GTEXT_CSS_Selector_Match_Context * ctx = w->ctx;
  ctx->tree_scope = tree_scope;
  for (size_t i = 0; i < v->n; i++) {
    const entry * e = &w->x->all[v->v[i]];
    const placed * pl = w->x->pls[e->pl];
    const sheet_record * r = &pl->sheet->recs[e->rec];
    if (((r->flags & REC_PSEUDO) != 0) != (w->request != NULL)) {
      continue;
    }
    ctx->ctr.rules_considered++;
    uint32_t dist;
    bool scoped;
    const bool m = record_matches(w, pl, r, slot, host, &dist, &scoped);
    if (ctx->status != GTEXT_CSS_OK) {
      w->status = ctx->status;
      return false;
    }
    if (m && !add_hit(w, pl, v->v[i], r, dist, scoped)) {
      return false;
    }
  }
  return true;
}

static bool test_bucket(walk * w, const bmap * m, const char * key, size_t n, bool fold, const void * tree_scope) {
  char stack[128];
  char * k = stack;
  if (n > sizeof stack) {
    k = gtext_allocator_malloc(&w->ctx->alloc, n);
    if (!k) {
      w->status = GTEXT_CSS_E_OOM;
      return false;
    }
  }
  if (fold) {
    lower_copy(k, key, n);
  }
  else {
    memcpy(k, key, n);
  }
  const bucket * b = bmap_find(m, w->x->seed, k, n);
  if (k != stack) {
    gtext_allocator_free(&w->ctx->alloc, k);
  }
  if (!b) {
    return true;
  }
  const u32vec v = {b->v, b->n, b->cap};
  return test_list(w, &v, NULL, NULL, tree_scope);
}

/// The group an element's own rules are in: the document's, or the shadow tree's it is in.
static group * group_of(const GTEXT_CSS_Rule_Index * x, struct GTEXT_CSS_Selector_Match_Context * ctx, El e, bool * missing) {
  *missing = false;
  if (!CSS_HAVE(ctx, GTEXT_CSS_ELEMENT_CB_CONTAINING_HOST) || !ctx->iface.containing_host(ctx->iface.user, e)) {
    return find_group_scope(x, NULL);
  }
  if (!CSS_HAVE(ctx, GTEXT_CSS_ELEMENT_CB_TREE_SCOPE)) {
    *missing = true;
    return NULL;
  }
  return find_group_scope(x, ctx->iface.tree_scope(ctx->iface.user, e));
}

static bool test_group(walk * w, const group * g) {
  struct GTEXT_CSS_Selector_Match_Context * ctx = w->ctx;
  GTEXT_CSS_String s;
  if (g->ids.nb) {
    if (!CSS_HAVE(ctx, GTEXT_CSS_ELEMENT_CB_ID)) {
      css_match_missing(ctx, GTEXT_CSS_ELEMENT_CB_ID);
      w->status = ctx->status;
      return false;
    }
    if (ctx->iface.id(ctx->iface.user, w->element, &s) && s.data && s.length && !test_bucket(w, &g->ids, s.data, s.length, w->x->opt.quirks, g->scope)) {
      return false;
    }
  }
  if (g->classes.nb) {
    if (!CSS_HAVE(ctx, GTEXT_CSS_ELEMENT_CB_CLASS_COUNT) || !CSS_HAVE(ctx, GTEXT_CSS_ELEMENT_CB_CLASS_AT)) {
      css_match_missing(ctx, CSS_HAVE(ctx, GTEXT_CSS_ELEMENT_CB_CLASS_COUNT) ? GTEXT_CSS_ELEMENT_CB_CLASS_AT : GTEXT_CSS_ELEMENT_CB_CLASS_COUNT);
      w->status = ctx->status;
      return false;
    }
    const size_t nc = ctx->iface.class_count(ctx->iface.user, w->element);
    for (size_t i = 0; i < nc; i++) {
      if (ctx->iface.class_at(ctx->iface.user, w->element, i, &s) && s.data && s.length && !test_bucket(w, &g->classes, s.data, s.length, w->x->opt.quirks, g->scope)) {
        return false;
      }
    }
  }
  if (g->tags.nb) {
    if (!CSS_HAVE(ctx, GTEXT_CSS_ELEMENT_CB_LOCAL_NAME)) {
      css_match_missing(ctx, GTEXT_CSS_ELEMENT_CB_LOCAL_NAME);
      w->status = ctx->status;
      return false;
    }
    if (ctx->iface.local_name(ctx->iface.user, w->element, &s) && s.data && s.length && !test_bucket(w, &g->tags, s.data, s.length, w->x->opt.html_document, g->scope)) {
      return false;
    }
  }
  return test_list(w, &g->other, NULL, NULL, g->scope);
}

static int hit_cmp(const void * pa, const void * pb) {
  const hit * a = pa;
  const hit * b = pb;
  if (a->number != b->number) {
    return a->number < b->number ? -1 : 1;
  }
  if (a->run != b->run) {
    return a->run < b->run ? -1 : 1;
  }
  const int c = gtext_css_specificity_compare(a->spec, b->spec);
  if (c) {
    return -c; // the most specific first
  }
  if (a->scoped != b->scoped) {
    return a->scoped ? -1 : 1;
  }
  if (a->dist != b->dist) {
    return a->dist < b->dist ? -1 : 1;
  }
  return a->ei < b->ei ? -1 : a->ei > b->ei;
}

GTEXT_CSS_Status gtext_css_rules_collect(const GTEXT_CSS_Rule_Index * x, GTEXT_CSS_Selector_Match_Context * ctx, GTEXT_CSS_Element_Handle element,
    const GTEXT_CSS_Selector_List * pseudo, GTEXT_CSS_Candidates * set, GTEXT_CSS_Rule_Index_Result * res) {
  ires_init(res);
  if (!x || !ctx || !element || !set) {
    return ires(res, GTEXT_CSS_E_INVALID, NULL, "a NULL argument");
  }
  if (!x->frozen) {
    return ires(res, GTEXT_CSS_E_INVALID, NULL, "the index is not frozen");
  }
  if (gtext_css_candidates_sealed(set)) {
    return ires(res, GTEXT_CSS_E_INVALID, NULL, "the candidate set is sealed");
  }
  if (ctx->opt.html_document != x->opt.html_document || ctx->opt.quirks != x->opt.quirks) {
    // the buckets are keyed with the index's folding; a context that folds otherwise would look in the wrong bucket and find no rule, silently
    return ires(res, GTEXT_CSS_E_INVALID, "options", "html_document and quirks of the match context are not those of the index");
  }
  walk w;
  memset(&w, 0, sizeof w);
  w.x = x;
  w.ctx = ctx;
  w.element = element;
  if (pseudo) {
    if (pseudo->count != 1 || pseudo->complex[0].compound_count != 1 || pseudo->complex[0].compounds[0].first_pseudo_element != 0 ||
        pseudo->complex[0].compounds[0].simple_count == 0) {
      return ires(res, GTEXT_CSS_E_INVALID, NULL, "the pseudo-element asked for is not a list of one complex selector made of pseudo-elements");
    }
    w.request = &pseudo->complex[0];
  }
  w.orig_scope = ctx->scope;
  w.orig_ts = ctx->tree_scope;
  w.orig_nest = ctx->nest;
  ctx->status = GTEXT_CSS_OK;
  ctx->missing = NULL;
  ctx->unsupported = NULL;
  ctx->ctr.work = 0;
  const size_t considered = ctx->ctr.rules_considered;
  bool missing = false;
  group * g0 = group_of(x, ctx, element, &missing);
  if (missing) {
    css_match_missing(ctx, GTEXT_CSS_ELEMENT_CB_TREE_SCOPE);
    w.status = ctx->status;
  }
  if (w.status == GTEXT_CSS_OK && g0) {
    (void)test_group(&w, g0);
    /* `:host::part(x)` of the shadow tree's own sheet styles the parts of the host the element is in */
    if (w.status == GTEXT_CSS_OK && g0->shadow && g0->part.n && CSS_HAVE(ctx, GTEXT_CSS_ELEMENT_CB_CONTAINING_HOST)) {
      El own = ctx->iface.containing_host(ctx->iface.user, element);
      if (own) {
        (void)test_list(&w, &g0->part, NULL, own, g0->scope);
      }
    }
  }
  /* the shadow tree the element is the host of: its `:host` rules */
  if (w.status == GTEXT_CSS_OK && x->has_shadow && CSS_HAVE(ctx, GTEXT_CSS_ELEMENT_CB_SHADOW_SCOPE)) {
    const void * ss = ctx->iface.shadow_scope(ctx->iface.user, element);
    group * gs = ss ? find_group_scope(x, ss) : NULL;
    if (gs) {
      (void)test_list(&w, &gs->host, NULL, NULL, gs->scope);
    }
  }
  /* the slots the element is assigned to, and the slots those are assigned to: the `::slotted()` rules of the shadow tree of each (the flat tree's chain) */
  if (w.status == GTEXT_CSS_OK && x->has_shadow && CSS_HAVE(ctx, GTEXT_CSS_ELEMENT_CB_ASSIGNED_SLOT)) {
    for (El slot = css_match_is_shadow_slot(ctx, element) ? NULL : ctx->iface.assigned_slot(ctx->iface.user, element); slot && w.status == GTEXT_CSS_OK; slot = ctx->iface.assigned_slot(ctx->iface.user, slot)) {
      bool m2 = false;
      group * gl = group_of(x, ctx, slot, &m2);
      if (gl && gl->shadow) {
        (void)test_list(&w, &gl->slotted, slot, NULL, gl->scope);
      }
      if (!css_match_work(ctx)) {
        w.status = ctx->status;
      }
    }
  }
  /* the hosts above the element: the `::part()` rules of the scope each host is in */
  if (w.status == GTEXT_CSS_OK && x->has_part && CSS_HAVE(ctx, GTEXT_CSS_ELEMENT_CB_CONTAINING_HOST)) {
    for (El h = ctx->iface.containing_host(ctx->iface.user, element); h && w.status == GTEXT_CSS_OK; h = ctx->iface.containing_host(ctx->iface.user, h)) {
      bool m2 = false;
      group * gh = group_of(x, ctx, h, &m2);
      if (gh && gh->part.n) {
        (void)test_list(&w, &gh->part, NULL, h, gh->scope);
      }
      if (!css_match_work(ctx)) {
        w.status = ctx->status;
      }
    }
  }
  ctx->scope = w.orig_scope;
  ctx->tree_scope = w.orig_ts;
  ctx->nest = w.orig_nest;
  GTEXT_CSS_Status st = w.status;
  if (res) {
    res->work = ctx->ctr.rules_considered - considered;
  }
  if (st != GTEXT_CSS_OK) {
    gtext_allocator_free(&ctx->alloc, w.hits);
    gtext_allocator_free(&ctx->alloc, w.rm);
    return ires(res, st, ctx->missing ? ctx->missing : (ctx->unsupported ? ctx->unsupported : NULL),
        st == GTEXT_CSS_E_UNSUPPORTED ? "a callback the sheets need is absent from the element interface" : NULL);
  }
  if (w.nhits > 1) {
    qsort(w.hits, w.nhits, sizeof *w.hits, hit_cmp);
  }
  /* the layer registry of each context that contributed, before the first candidate of any (giving a context its registry again is harmless) */
  const group * last = NULL;
  for (size_t i = 0; i < w.nhits && st == GTEXT_CSS_OK; i++) {
    const group * g = x->pls[x->all[w.hits[i].ei].pl]->g;
    if (g != last) {
      st = gtext_css_candidates_use_layers(set, g->context, g->layers);
      last = g;
    }
  }
  size_t added = 0;
  for (size_t i = 0; i < w.nhits && st == GTEXT_CSS_OK; i++) {
    if (i > 0 && w.hits[i].number == w.hits[i - 1].number && w.hits[i].run == w.hits[i - 1].run) {
      continue; // the same rule by another of its complex selectors: the most specific was first
    }
    const entry * e = &x->all[w.hits[i].ei];
    const placed * pl = x->pls[e->pl];
    const sheet_record * r = &pl->sheet->recs[e->rec];
    const sheet_run * run = &pl->sheet->runs[r->run];
    for (size_t j = 0; j < run->ndecl && st == GTEXT_CSS_OK; j++) {
      const sheet_decl * d = &run->decls[j];
      GTEXT_CSS_Cascade_Entry k;
      memset(&k, 0, sizeof k);
      k.origin = pl->origin;
      k.important = d->important;
      k.layer = r->layer < pl->lmap_n ? pl->lmap[r->layer] : 0;
      k.context = pl->g->context;
      k.scoped = w.hits[i].scoped;
      k.scope_distance = w.hits[i].dist;
      k.specificity = w.hits[i].spec;
      k.sheet = pl->number;
      k.rule = r->run;
      k.declaration = (uint32_t)j;
      k.rule_id = run->rule_id;
      k.declaration_id = d->id;
      k.span = d->span;
      GTEXT_CSS_Cascade_Result cr;
      memset(&cr, 0, sizeof cr);
      st = d->custom ? gtext_css_candidates_add_custom(set, &k, d->name, d->name_len, &d->pv, &cr) : gtext_css_candidates_add(set, &k, &d->pv, &cr);
      if (st != GTEXT_CSS_OK) {
        ires(res, st, cr.name, cr.detail);
      }
      added++;
    }
  }
  gtext_allocator_free(&ctx->alloc, w.hits);
  gtext_allocator_free(&ctx->alloc, w.rm);
  (void)added;
  if (st != GTEXT_CSS_OK && res && res->status == GTEXT_CSS_OK) {
    ires(res, st, NULL, NULL);
  }
  return st;
}
