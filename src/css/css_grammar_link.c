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
 * Linking a definition: every `<type>` and `<'property'>` it names is a native
 * type or comes from the caller's resolver, each distinct reference (with its
 * range) is copied into the compiled grammar once, and the result is checked for
 * left recursion and has its references replaced by what they stand for.
 *
 * Nothing recurses. The definitions to copy are a queue; "can this node match
 * without consuming a component value" is computed by repeated passes until
 * nothing changes; left recursion is a depth-first search with an explicit
 * stack.
 *
 * Reference: CSS Values and Units Level 4, section 2: a range after a type name
 * restricts the type ("the number is clamped or invalid outside it"), applying
 * to every numeric type of a composite definition.
 */

#include <ghoti.io/text/macros.h>

#include <string.h>

#include "css_grammar_internal.h"

/// One instance of a definition in the compiled grammar.
typedef struct {
  const GTEXT_CSS_Definition * def; ///< What was copied.
  uint32_t base;                    ///< Index of its first node in the compiled grammar.
  uint32_t text_base;               ///< Where its text starts in the compiled pool.
  bool has_range;                   ///< Copied with a range applied.
  double lo, hi;                    ///< The range.
  uint32_t key_name;                ///< Offset in the compiled text of the name it was linked as (for the table).
  uint32_t key_len;                 ///< Its length.
  uint8_t key_kind;                 ///< Reference kind.
  uint32_t root;                    ///< Its root node, in the compiled grammar.
} instance;

typedef struct {
  GTEXT_Allocator alloc;
  GTEXT_CSS_Compiled * c;
  size_t ncap, kcap, tcap;
  size_t max_nodes;
  GTEXT_CSS_Resolver resolver;
  void * user;
  instance * inst;                  ///< The instances, in the order made (also the work queue).
  size_t ninst, icap;
  size_t next;                      ///< Next instance to scan for references.
  struct { const GTEXT_CSS_Definition * def; uint32_t text_base; } * texts; ///< Definitions already in the pool.
  size_t ntexts, txcap;
  uint32_t * table;                 ///< Hash of instance numbers + 1 (0 empty), keyed on kind, name and range.
  size_t nslots;
  GTEXT_CSS_Status status;
  char err_name[96];
  size_t err_len;
} linker;

static void lfail(linker * L, GTEXT_CSS_Status st) {
  if (L->status == GTEXT_CSS_OK) {
    L->status = st;
  }
}

static void set_name(linker * L, const gnode * ref, const char * text) {
  const bool prop = ref->native == GTEXT_CSS_REF_PROPERTY;
  size_t k = 0;
  L->err_name[k++] = '<';
  if (prop) {
    L->err_name[k++] = '\'';
  }
  size_t n = ref->str_len;
  if (n > sizeof L->err_name - 5) {
    n = sizeof L->err_name - 5;
  }
  memcpy(L->err_name + k, text + ref->str_off, n);
  k += n;
  if (prop) {
    L->err_name[k++] = '\'';
  }
  L->err_name[k++] = '>';
  L->err_len = k;
}

static uint32_t hash_key(uint8_t kind, const char * name, size_t len, bool has_range, double lo, double hi) {
  uint32_t h = 2166136261u ^ kind;
  for (size_t i = 0; i < len; i++) {
    h = (h ^ (unsigned char)name[i]) * 16777619u;
  }
  if (has_range) {
    unsigned char b[sizeof lo * 2];
    memcpy(b, &lo, sizeof lo);
    memcpy(b + sizeof lo, &hi, sizeof hi);
    for (size_t i = 0; i < sizeof b; i++) {
      h = (h ^ b[i]) * 16777619u;
    }
  }
  return h;
}

static bool same_double(double a, double b) {
  return memcmp(&a, &b, sizeof a) == 0;
}

static size_t table_find(const linker * L, uint8_t kind, const char * name, size_t len, bool has_range,
    double lo, double hi, bool * found) {
  const size_t mask = L->nslots - 1;
  size_t i = hash_key(kind, name, len, has_range, lo, hi) & mask;
  while (L->table[i]) {
    const instance * in = &L->inst[L->table[i] - 1];
    if (in->key_kind == kind && in->key_len == len && in->has_range == has_range
        && (!has_range || (same_double(in->lo, lo) && same_double(in->hi, hi)))
        && memcmp(L->c->text + in->key_name, name, len) == 0) {
      *found = true;
      return i;
    }
    i = (i + 1) & mask;
  }
  *found = false;
  return i;
}

static bool table_grow(linker * L) {
  const size_t nslots = L->nslots ? L->nslots * 2 : 64;
  uint32_t * t = gtext_allocator_calloc(&L->alloc, nslots, sizeof *t);
  if (!t) {
    return false;
  }
  gtext_allocator_free(&L->alloc, L->table);
  L->table = t;
  L->nslots = nslots;
  for (size_t k = 0; k < L->ninst; k++) {
    const instance * in = &L->inst[k];
    if (in->key_len == 0) {
      continue; /* the root */
    }
    size_t i = hash_key(in->key_kind, L->c->text + in->key_name, in->key_len, in->has_range, in->lo, in->hi) & (nslots - 1);
    while (t[i]) {
      i = (i + 1) & (nslots - 1);
    }
    t[i] = (uint32_t)(k + 1);
  }
  return true;
}

/// Copy a definition into the compiled grammar (with a range applied, if any); returns its instance number.
static bool instantiate(linker * L, const GTEXT_CSS_Definition * def, bool has_range, double lo, double hi,
    uint8_t key_kind, uint32_t key_name, uint32_t key_len, size_t * out_index) {
  GTEXT_CSS_Compiled * c = L->c;
  if (def->nnodes > L->max_nodes || c->nnodes > L->max_nodes - def->nnodes) {
    lfail(L, GTEXT_CSS_E_LIMIT_DEFINITION_NODES);
    return false;
  }
  /* the text, once for each definition */
  uint32_t text_base = UINT32_MAX;
  for (size_t i = 0; i < L->ntexts; i++) {
    if (L->texts[i].def == def) {
      text_base = L->texts[i].text_base;
    }
  }
  if (text_base == UINT32_MAX) {
    void * tx = L->texts;
    if (!css_grammar_grow(&L->alloc, &tx, &L->txcap, L->ntexts + 1, sizeof L->texts[0])) {
      lfail(L, GTEXT_CSS_E_OOM);
      return false;
    }
    L->texts = tx;
    void * tp = c->text;
    if (c->text_len + def->text_len + 1 > UINT32_MAX / 2
        || !css_grammar_grow(&L->alloc, &tp, &L->tcap, c->text_len + def->text_len + 1, 1)) {
      lfail(L, GTEXT_CSS_E_OOM);
      return false;
    }
    c->text = tp;
    text_base = (uint32_t)c->text_len;
    memcpy(c->text + c->text_len, def->text, def->text_len + 1);
    c->text_len += def->text_len + 1;
    L->texts[L->ntexts].def = def;
    L->texts[L->ntexts].text_base = text_base;
    L->ntexts++;
  }
  void * nodes = c->nodes;
  void * kids = c->kids;
  if (!css_grammar_grow(&L->alloc, &nodes, &L->ncap, c->nnodes + def->nnodes, sizeof(gnode))) {
    lfail(L, GTEXT_CSS_E_OOM);
    return false;
  }
  c->nodes = nodes;
  if (!css_grammar_grow(&L->alloc, &kids, &L->kcap, c->nkids + def->nkids, sizeof(uint32_t))) {
    lfail(L, GTEXT_CSS_E_OOM);
    return false;
  }
  c->kids = kids;
  void * inst = L->inst;
  if (!css_grammar_grow(&L->alloc, &inst, &L->icap, L->ninst + 1, sizeof(instance))) {
    lfail(L, GTEXT_CSS_E_OOM);
    return false;
  }
  L->inst = inst;
  if ((L->ninst + 1) * 2 > L->nslots && !table_grow(L)) {
    lfail(L, GTEXT_CSS_E_OOM);
    return false;
  }
  const uint32_t base = (uint32_t)c->nnodes;
  const uint32_t kbase = (uint32_t)c->nkids;
  for (size_t i = 0; i < def->nkids; i++) {
    c->kids[c->nkids + i] = def->kids[i] + base;
  }
  c->nkids += def->nkids;
  for (size_t i = 0; i < def->nnodes; i++) {
    gnode g = def->nodes[i];
    g.kids += kbase;
    g.str_off += text_base;
    g.src_off += text_base;
    if (has_range && ((g.kind == GN_NATIVE && css_grammar_native_numeric((gn_native)g.native)) || g.kind == GN_REF)) {
      if (g.flags & GNF_RANGE) {
        if (lo > g.lo) {
          g.lo = lo;
        }
        if (hi < g.hi) {
          g.hi = hi;
        }
      }
      else {
        g.flags |= GNF_RANGE;
        g.lo = lo;
        g.hi = hi;
      }
    }
    c->nodes[base + i] = g;
  }
  c->nnodes += def->nnodes;
  instance * in = &L->inst[L->ninst];
  memset(in, 0, sizeof *in);
  in->def = def;
  in->base = base;
  in->text_base = text_base;
  in->has_range = has_range;
  in->lo = lo;
  in->hi = hi;
  in->key_kind = key_kind;
  in->key_name = key_name;
  in->key_len = key_len;
  in->root = base + def->root;
  *out_index = L->ninst++;
  if (key_len) {
    bool found;
    const size_t slot = table_find(L, key_kind, c->text + key_name, key_len, has_range, lo, hi, &found);
    L->table[slot] = (uint32_t)L->ninst;
  }
  return true;
}

/// Resolve the references of one instance.
static bool resolve_instance(linker * L, size_t index) {
  GTEXT_CSS_Compiled * c = L->c;
  const uint32_t base = L->inst[index].base;
  const size_t count = L->inst[index].def->nnodes;
  for (size_t k = 0; k < count && L->status == GTEXT_CSS_OK; k++) {
    gnode * r = &c->nodes[base + k];
    if (r->kind != GN_REF) {
      continue;
    }
    const gn_native nt = r->native == GTEXT_CSS_REF_TYPE ? css_grammar_native_lookup(c->text + r->str_off, r->str_len) : NT_COUNT;
    if (nt != NT_COUNT) {
      if ((r->flags & GNF_RANGE) && !css_grammar_native_numeric(nt)) {
        set_name(L, r, c->text);
        lfail(L, GTEXT_CSS_E_GRAMMAR);
        return false;
      }
      const uint8_t keep = (uint8_t)(r->flags & (GNF_RANGE | GNF_NONNULL));
      r->kind = GN_NATIVE;
      r->native = (uint8_t)nt;
      r->flags = (uint8_t)(keep & ~GNF_NONNULL);
      if (!css_grammar_native_nullable(nt)) {
        r->flags |= GNF_NONNULL;
      }
      continue;
    }
    const bool range = (r->flags & GNF_RANGE) != 0;
    const double lo = range ? r->lo : 0.0, hi = range ? r->hi : 0.0;
    bool found;
    size_t slot = table_find(L, r->native, c->text + r->str_off, r->str_len, range, lo, hi, &found);
    if (found) {
      r->target = L->inst[L->table[slot] - 1].root;
      continue;
    }
    if (!L->resolver) {
      set_name(L, r, c->text);
      lfail(L, GTEXT_CSS_E_REFERENCE);
      return false;
    }
    const GTEXT_CSS_Definition * def = L->resolver(L->user, (GTEXT_CSS_Ref_Kind)r->native, c->text + r->str_off, r->str_len);
    if (!def) {
      set_name(L, r, c->text);
      lfail(L, GTEXT_CSS_E_REFERENCE);
      return false;
    }
    size_t n;
    const uint8_t kind = r->native;
    const uint32_t name_off = r->str_off, name_len = r->str_len;
    if (!instantiate(L, def, range, lo, hi, kind, name_off, name_len, &n)) {
      return false;
    }
    r = &c->nodes[base + k]; /* the array may have moved */
    r->target = L->inst[n].root;
  }
  return L->status == GTEXT_CSS_OK;
}

/* ---- nullable and left recursion ---------------------------------------- */

static bool node_nullable(const GTEXT_CSS_Compiled * c, const uint8_t * nl, uint32_t n) {
  const gnode * g = &c->nodes[n];
  switch ((gn_kind)g->kind) {
  case GN_SEQ: {
    if (g->sep) {
      return false;
    }
    for (uint32_t i = 0; i < g->nkids; i++) {
      if (!nl[c->kids[g->kids + i]]) {
        return false;
      }
    }
    return true;
  }
  case GN_ALT: case GN_OR:
    for (uint32_t i = 0; i < g->nkids; i++) {
      if (nl[c->kids[g->kids + i]]) {
        return true;
      }
    }
    return false;
  case GN_AND:
    for (uint32_t i = 0; i < g->nkids; i++) {
      if (!nl[c->kids[g->kids + i]]) {
        return false;
      }
    }
    return true;
  case GN_MULT:
    return g->min == 0 || nl[c->kids[g->kids]];
  case GN_REF:
    return g->target != UINT32_MAX && nl[g->target];
  case GN_NATIVE:
    return css_grammar_native_nullable((gn_native)g->native);
  default:
    return false;
  }
}

/// The idx-th node that `n` can begin with at the same position, if there is one.
static bool left_edge(const GTEXT_CSS_Compiled * c, const uint8_t * nl, uint32_t n, uint32_t idx, uint32_t * out) {
  const gnode * g = &c->nodes[n];
  switch ((gn_kind)g->kind) {
  case GN_REF:
    if (idx == 0 && g->target != UINT32_MAX) {
      *out = g->target;
      return true;
    }
    return false;
  case GN_SEQ:
    if (g->sep) {
      if (idx == 0) {
        *out = c->kids[g->kids];
        return true;
      }
      return false;
    }
    for (uint32_t j = 0; j < idx; j++) {
      if (!nl[c->kids[g->kids + j]]) {
        return false;
      }
    }
    if (idx < g->nkids) {
      *out = c->kids[g->kids + idx];
      return true;
    }
    return false;
  case GN_ALT: case GN_AND: case GN_OR:
    if (idx < g->nkids) {
      *out = c->kids[g->kids + idx];
      return true;
    }
    return false;
  case GN_MULT: case GN_REQ:
    if (idx == 0) {
      *out = c->kids[g->kids];
      return true;
    }
    return false;
  default:
    return false;
  }
}

static bool find_left_recursion(linker * L, const uint8_t * nl) {
  GTEXT_CSS_Compiled * c = L->c;
  uint8_t * color = gtext_allocator_calloc(&L->alloc, c->nnodes, 1);
  struct { uint32_t node; uint32_t edge; } * st = gtext_allocator_malloc(&L->alloc, (c->nnodes + 1) * sizeof st[0]);
  if (!color || !st) {
    gtext_allocator_free(&L->alloc, color);
    gtext_allocator_free(&L->alloc, st);
    lfail(L, GTEXT_CSS_E_OOM);
    return false;
  }
  bool ok = true;
  /* from every node: a type that is only reached after something has been consumed is still left recursive
     if it can reach itself without consuming anything */
  for (uint32_t root = 0; root < c->nnodes && ok; root++) {
    if (color[root] != 0) {
      continue;
    }
    size_t top = 0;
    st[top].node = root;
    st[top].edge = 0;
    top++;
    color[root] = 1;
    while (top > 0 && ok) {
      const uint32_t n = st[top - 1].node;
      uint32_t to;
      if (!left_edge(c, nl, n, st[top - 1].edge, &to)) {
        color[n] = 2;
        top--;
        continue;
      }
      st[top - 1].edge++;
      if (color[to] == 1) {
        /* a cycle: name a reference on it, the last one if several */
        size_t from = 0;
        for (size_t i = 0; i < top; i++) {
          if (st[i].node == to) {
            from = i;
          }
        }
        const gnode * named = NULL;
        for (size_t i = from; i < top; i++) {
          if (c->nodes[st[i].node].kind == GN_REF) {
            named = &c->nodes[st[i].node];
          }
        }
        if (named) {
          set_name(L, named, c->text);
        }
        lfail(L, GTEXT_CSS_E_LEFT_RECURSION);
        ok = false;
        break;
      }
      if (color[to] == 0) {
        color[to] = 1;
        st[top].node = to;
        st[top].edge = 0;
        top++;
      }
    }
  }
  gtext_allocator_free(&L->alloc, color);
  gtext_allocator_free(&L->alloc, st);
  return ok;
}

/* ---- the link ----------------------------------------------------------- */

static void compiled_free_parts(GTEXT_CSS_Compiled * c) {
  if (!c) {
    return;
  }
  GTEXT_Allocator a = c->alloc;
  gtext_allocator_free(&a, c->text);
  gtext_allocator_free(&a, c->nodes);
  gtext_allocator_free(&a, c->kids);
  gtext_css_atom_table_free(c->atoms);
  gtext_allocator_free(&a, c);
}

void gtext_css_compiled_free(GTEXT_CSS_Compiled * c) {
  compiled_free_parts(c);
}

size_t gtext_css_compiled_node_count(const GTEXT_CSS_Compiled * c) {
  return c ? c->nnodes : 0;
}

const char * gtext_css_compiled_slot_text(const GTEXT_CSS_Compiled * c, uint32_t slot, size_t * length) {
  if (!c || !length || slot >= c->nnodes) {
    return NULL;
  }
  *length = c->nodes[slot].src_len;
  return c->text + c->nodes[slot].src_off;
}

const GTEXT_CSS_Atom_Table * gtext_css_compiled_atoms(const GTEXT_CSS_Compiled * c) {
  return c ? c->atoms : NULL;
}

GTEXT_CSS_Status gtext_css_definition_link(const GTEXT_CSS_Definition * def, GTEXT_CSS_Resolver resolver,
    void * user, const GTEXT_CSS_Grammar_Options * options, GTEXT_CSS_Compiled ** out,
    GTEXT_CSS_Grammar_Result * result) {
  if (!def || !out) {
    css_grammar_result(result, GTEXT_CSS_E_INVALID, 0, NULL, 0, 0);
    return GTEXT_CSS_E_INVALID;
  }
  size_t max_nodes;
  css_grammar_limits(options, NULL, NULL, &max_nodes);
  linker L;
  memset(&L, 0, sizeof L);
  L.alloc = css_grammar_allocator(options);
  L.max_nodes = max_nodes;
  L.resolver = resolver;
  L.user = user;
  GTEXT_CSS_Compiled * c = gtext_allocator_malloc(&L.alloc, sizeof *c);
  if (!c) {
    css_grammar_result(result, GTEXT_CSS_E_OOM, 0, NULL, 0, 0);
    return GTEXT_CSS_E_OOM;
  }
  memset(c, 0, sizeof *c);
  c->alloc = L.alloc;
  L.c = c;
  uint8_t * nl = NULL;
  size_t root_index;
  if (instantiate(&L, def, false, 0.0, 0.0, 0, 0, 0, &root_index)) {
    c->root = L.inst[root_index].root;
    while (L.next < L.ninst && resolve_instance(&L, L.next)) {
      L.next++;
    }
  }
  if (L.status == GTEXT_CSS_OK) {
    nl = gtext_allocator_calloc(&L.alloc, c->nnodes ? c->nnodes : 1, 1);
    if (!nl) {
      lfail(&L, GTEXT_CSS_E_OOM);
    }
  }
  if (L.status == GTEXT_CSS_OK) {
    bool changed = true;
    while (changed) {
      changed = false;
      for (size_t i = c->nnodes; i-- > 0;) {
        if (!nl[i] && node_nullable(c, nl, (uint32_t)i)) {
          nl[i] = 1;
          changed = true;
        }
      }
    }
    find_left_recursion(&L, nl);
  }
  if (L.status == GTEXT_CSS_OK) {
    /* replace every reference by what it stands for */
    for (size_t i = 0; i < c->nkids; i++) {
      uint32_t k = c->kids[i];
      size_t guard = 0;
      while (c->nodes[k].kind == GN_REF && guard++ <= c->nnodes) {
        k = c->nodes[k].target;
      }
      c->kids[i] = k;
    }
    size_t guard = 0;
    while (c->nodes[c->root].kind == GN_REF && guard++ <= c->nnodes) {
      c->root = c->nodes[c->root].target;
    }
    if (gtext_css_atom_table_new(&L.alloc, &c->atoms) != GTEXT_CSS_OK) {
      lfail(&L, GTEXT_CSS_E_OOM);
    }
  }
  if (L.status == GTEXT_CSS_OK) {
    for (size_t i = 0; i < c->nnodes && L.status == GTEXT_CSS_OK; i++) {
      gnode * g = &c->nodes[i];
      if (g->kind == GN_KEYWORD) {
        GTEXT_CSS_Atom a;
        if (gtext_css_atom_intern(c->atoms, c->text + g->str_off, g->str_len, &a) != GTEXT_CSS_OK) {
          lfail(&L, GTEXT_CSS_E_OOM);
        }
        g->atom = a;
      }
    }
  }
  gtext_allocator_free(&L.alloc, nl);
  gtext_allocator_free(&L.alloc, L.inst);
  gtext_allocator_free(&L.alloc, L.texts);
  gtext_allocator_free(&L.alloc, L.table);
  if (L.status != GTEXT_CSS_OK) {
    const size_t nodes = c->nnodes;
    compiled_free_parts(c);
    css_grammar_result(result, L.status, 0, L.err_name, L.err_len, nodes);
    return L.status;
  }
  css_grammar_result(result, GTEXT_CSS_OK, 0, NULL, 0, c->nnodes);
  *out = c;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_compile(const char * text, size_t length, GTEXT_CSS_Resolver resolver, void * user,
    const GTEXT_CSS_Grammar_Options * options, GTEXT_CSS_Compiled ** out, GTEXT_CSS_Grammar_Result * result) {
  GTEXT_CSS_Definition * d = NULL;
  const GTEXT_CSS_Status st = gtext_css_definition_parse(text, length, options, &d, result);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  const GTEXT_CSS_Status ls = gtext_css_definition_link(d, resolver, user, options, out, result);
  gtext_css_definition_free(d);
  return ls;
}
