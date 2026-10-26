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
 * without consuming a component value" is a worklist over the nodes that depend
 * on a node when it turns out to be able to; left recursion is a depth-first
 * search with an explicit stack; the commas and slashes that can separate the
 * items of a list are decided after that, when it is known which items can be
 * empty.
 *
 * Ranges. Values 4, 5.1: "within the angle brackets, after the identifying
 * keyword, indicating a closed range between (and including) min and max";
 * "Values of -infinity or infinity must be written without units, even if the
 * value type uses units"; "Values of 0 can be written without units, even if the
 * value type doesn't allow 'unitless zeroes'". The draft does not say what a
 * range on a composite type such as <length-percentage [0,infinity]> means. Here
 * it restricts every numeric terminal of the definition that its bounds can
 * apply to (the unit of a bound must be that terminal's kind of unit); a
 * terminal of another kind, and a type that is not numeric, are left as they
 * are. A range written on a native type that its bounds do not fit
 * (<number [0px,10px]>) is no range, and so is one that the ranges of the
 * types around it narrow to nothing.
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
  uint8_t rclass;                   ///< What the range's bounds were written in (RC_*).
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
  size_t max_bytes;                 ///< The bound on the texts of all the definitions together.
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
  size_t work;                      ///< Units of work (GTEXT_CSS_Grammar_Result::work).
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

static uint32_t hash_key(uint8_t kind, const char * name, size_t len, bool has_range, double lo, double hi, uint8_t rclass) {
  uint32_t h = (2166136261u ^ kind) * 16777619u ^ rclass;
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
    double lo, double hi, uint8_t rclass, bool * found) {
  const size_t mask = L->nslots - 1;
  size_t i = hash_key(kind, name, len, has_range, lo, hi, rclass) & mask;
  while (L->table[i]) {
    const instance * in = &L->inst[L->table[i] - 1];
    if (in->key_kind == kind && in->key_len == len && in->has_range == has_range
        && (!has_range || (same_double(in->lo, lo) && same_double(in->hi, hi) && in->rclass == rclass))
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
    size_t i = hash_key(in->key_kind, L->c->text + in->key_name, in->key_len, in->has_range, in->lo, in->hi, in->rclass) & (nslots - 1);
    while (t[i]) {
      i = (i + 1) & (nslots - 1);
    }
    t[i] = (uint32_t)(k + 1);
  }
  return true;
}

/// Copy a definition into the compiled grammar (with a range applied, if any); returns its instance number.
static bool instantiate(linker * L, const GTEXT_CSS_Definition * def, bool has_range, double lo, double hi, uint8_t rclass,
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
    if (def->text_len + 1 > L->max_bytes || c->text_len > L->max_bytes - def->text_len - 1) { /* (a NUL follows each text) */
      lfail(L, GTEXT_CSS_E_LIMIT_DEFINITION); /* the texts of the definitions together are more than an offset can hold */
      return false;
    }
    if (!css_grammar_grow(&L->alloc, &tp, &L->tcap, c->text_len + def->text_len + 1, 1)) {
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
    if (has_range && g.kind == GN_REF) {
      /* the range of the type around it reaches every reference in the definition that it can restrict (the name is in the
         compiled text: g.str_off has been moved there with the rest of the node) */
      const gn_native nt = g.native == GTEXT_CSS_REF_TYPE ? css_grammar_native_lookup(c->text + g.str_off, g.str_len) : NT_COUNT;
      const bool applies = g.native == GTEXT_CSS_REF_PROPERTY ? true : nt == NT_COUNT ? true
          : css_grammar_native_numeric(nt) && css_grammar_range_fits(nt, rclass);
      if (applies) {
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
          g.rclass = rclass;
        }
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
  in->rclass = rclass;
  in->key_kind = key_kind;
  in->key_name = key_name;
  in->key_len = key_len;
  in->root = base + def->root;
  if (key_len == 10 && key_kind == GTEXT_CSS_REF_TYPE && memcmp(c->text + key_name, "try-tactic", 10) == 0 && c->nodes[in->root].kind == GN_OR) {
    /* Anchor Positioning 1: the tactics of a try fallback are applied in the order they are written, so the value keeps that order */
    c->nodes[in->root].flags |= GNF_SOURCE_ORDER;
  }
  *out_index = L->ninst++;
  if (key_len) {
    bool found;
    const size_t slot = table_find(L, key_kind, c->text + key_name, key_len, has_range, lo, hi, rclass, &found);
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
    if ((r->flags & GNF_RANGE) && r->lo > r->hi) {
      set_name(L, r, c->text); /* the ranges around it left nothing */
      lfail(L, GTEXT_CSS_E_GRAMMAR);
      return false;
    }
    if (nt != NT_COUNT) {
      /* A range on a native type that cannot take one (`<string [0,1]>`) is a grammar error. A range that bounds a numeric type is
         one it fits: the parser refuses the other, and a range brought by an enclosing type is brought only where it fits. */
      if ((r->flags & GNF_RANGE) && (!css_grammar_native_numeric(nt) || !css_grammar_range_fits(nt, r->rclass))) {
        set_name(L, r, c->text);
        lfail(L, GTEXT_CSS_E_GRAMMAR);
        return false;
      }
      const uint8_t flags = (uint8_t)(r->flags & GNF_RANGE);
      r->kind = GN_NATIVE;
      r->native = (uint8_t)nt;
      r->flags = flags;
      continue;
    }
    const bool range = (r->flags & GNF_RANGE) != 0;
    const double lo = range ? r->lo : 0.0, hi = range ? r->hi : 0.0;
    const uint8_t rclass = range ? r->rclass : (uint8_t)RC_NONE;
    bool found;
    size_t slot = table_find(L, r->native, c->text + r->str_off, r->str_len, range, lo, hi, rclass, &found);
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
    if (!instantiate(L, def, range, lo, hi, rclass, kind, name_off, name_len, &n)) {
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
    /* A juxtaposition with commas between segments that can all be empty may be empty: the commas are omitted with the segments
     * (Values 4, 2; see omit_commas()). Decided on the literals as written, so that the answer is the same before and after the
     * rewrite. A comma at an end or two together are literal items, as they are matched. */
    bool commas = false, whole = !(g->flags & GNF_OMITTED) && g->nkids >= 3;
    for (uint32_t i = 0; whole && i < g->nkids; i++) {
      const bool lit = c->nodes[c->kids[g->kids + i]].kind == GN_LITERAL && c->nodes[c->kids[g->kids + i]].lit == ',';
      const bool prev = i > 0 && c->nodes[c->kids[g->kids + i - 1]].kind == GN_LITERAL && c->nodes[c->kids[g->kids + i - 1]].lit == ',';
      if (lit) {
        commas = true;
        whole = i > 0 && i + 1 < g->nkids && !prev;
      }
    }
    for (uint32_t i = 0; i < g->nkids; i++) {
      const gnode * k = &c->nodes[c->kids[g->kids + i]];
      if (commas && whole && k->kind == GN_LITERAL && k->lit == ',') {
        continue;
      }
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
      L->work += 1;
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

/* ---- which nodes can match nothing -------------------------------------- */

/// The nodes whose nullability depends on node `x` (every node that has it as a kid, and every reference to it) are
/// listed in `list` from `begin[x]` to `begin[x + 1]`. A node is looked at once to start with, and again only when a node
/// it depends on has turned out to be able to match nothing, which happens at most once for each, so the work is the
/// number of edges and the nodes (charged to the result's work).
static bool nullable_worklist(linker * L, uint8_t * nl) {
  GTEXT_CSS_Compiled * c = L->c;
  const size_t n = c->nnodes;
  size_t * begin = gtext_allocator_calloc(&L->alloc, n + 2, sizeof *begin);
  size_t * cursor = gtext_allocator_malloc(&L->alloc, (n + 1) * sizeof *cursor);
  uint32_t * stack = gtext_allocator_malloc(&L->alloc, (n + 1) * sizeof *stack);
  uint8_t * queued = gtext_allocator_calloc(&L->alloc, n + 1, 1);
  uint32_t * list = NULL;
  if (!begin || !cursor || !stack || !queued) {
    lfail(L, GTEXT_CSS_E_OOM);
  }
  for (int pass = 0; pass < 2 && L->status == GTEXT_CSS_OK; pass++) {
    for (size_t i = 0; i < n; i++) {
      const gnode * g = &c->nodes[i];
      if (g->kind == GN_REF) {
        if (g->target != UINT32_MAX) {
          if (pass == 0) {
            begin[g->target + 1]++;
          }
          else {
            list[cursor[g->target]++] = (uint32_t)i;
          }
        }
        continue;
      }
      if (g->kind != GN_SEQ && g->kind != GN_ALT && g->kind != GN_AND && g->kind != GN_OR && g->kind != GN_MULT) {
        continue;
      }
      for (uint32_t k = 0; k < g->nkids; k++) {
        const uint32_t kid = c->kids[g->kids + k];
        if (pass == 0) {
          begin[kid + 1]++;
        }
        else {
          list[cursor[kid]++] = (uint32_t)i;
        }
      }
    }
    if (pass == 0) {
      for (size_t i = 1; i <= n; i++) {
        begin[i] += begin[i - 1];
      }
      list = gtext_allocator_malloc(&L->alloc, (begin[n] + 1) * sizeof *list);
      if (!list) {
        lfail(L, GTEXT_CSS_E_OOM);
        break;
      }
      memcpy(cursor, begin, n * sizeof *cursor);
    }
  }
  if (L->status == GTEXT_CSS_OK) {
    size_t top = 0;
    for (size_t i = n; i-- > 0;) { /* the kids before the nodes that hold them: most nodes settle in one look */
      stack[top++] = (uint32_t)i;
      queued[i] = 1;
    }
    while (top > 0) {
      const uint32_t x = stack[--top];
      queued[x] = 0;
      L->work += 1;
      if (nl[x] || !node_nullable(c, nl, x)) {
        continue;
      }
      nl[x] = 1;
      for (size_t k = begin[x]; k < begin[x + 1]; k++) {
        const uint32_t d = list[k];
        L->work += 1;
        if (!nl[d] && !queued[d]) {
          queued[d] = 1;
          stack[top++] = d;
        }
      }
    }
  }
  gtext_allocator_free(&L->alloc, begin);
  gtext_allocator_free(&L->alloc, cursor);
  gtext_allocator_free(&L->alloc, stack);
  gtext_allocator_free(&L->alloc, queued);
  gtext_allocator_free(&L->alloc, list);
  return L->status == GTEXT_CSS_OK;
}

/* ---- commas and slashes between the items of a list ------------------------ */

static bool is_sep_literal(const GTEXT_CSS_Compiled * c, uint32_t node, uint32_t ch) {
  const gnode * g = &c->nodes[node];
  return g->kind == GN_LITERAL && g->lit == ch;
}

static uint32_t add_node(linker * L, const gnode * proto) {
  GTEXT_CSS_Compiled * c = L->c;
  if (c->nnodes >= L->max_nodes) {
    lfail(L, GTEXT_CSS_E_LIMIT_DEFINITION_NODES);
    return GN_NONE;
  }
  void * nodes = c->nodes;
  if (!css_grammar_grow(&L->alloc, &nodes, &L->ncap, c->nnodes + 1, sizeof(gnode))) {
    lfail(L, GTEXT_CSS_E_OOM);
    return GN_NONE;
  }
  c->nodes = nodes;
  c->nodes[c->nnodes] = *proto;
  return (uint32_t)c->nnodes++;
}

static bool add_kid_run(linker * L, const uint32_t * items, size_t n, uint32_t * first) {
  GTEXT_CSS_Compiled * c = L->c;
  void * kids = c->kids;
  if (!css_grammar_grow(&L->alloc, &kids, &L->kcap, c->nkids + n, sizeof(uint32_t))) {
    lfail(L, GTEXT_CSS_E_OOM);
    return false;
  }
  c->kids = kids;
  *first = (uint32_t)c->nkids;
  memcpy(c->kids + c->nkids, items, n * sizeof(uint32_t));
  c->nkids += n;
  return true;
}

/* ---- omissible commas ---------------------------------------------------------- */

/// Most segments of a juxtaposition that the omission of commas is worked out for (the number of nodes it makes is quadratic in it).
#define OMIT_MAX_SEGMENTS 16

static uint32_t make_inner(linker * L, uint8_t kind, const uint32_t * kids, size_t n, uint32_t min, uint32_t max, uint32_t src_from,
    uint32_t src_to, uint8_t * nl, uint8_t nullable) {
  GTEXT_CSS_Compiled * c = L->c;
  uint32_t first = 0;
  if (!add_kid_run(L, kids, n, &first)) {
    return GN_NONE;
  }
  gnode g;
  memset(&g, 0, sizeof g);
  g.kind = kind;
  g.min = min;
  g.max = max;
  g.target = UINT32_MAX;
  g.kids = first;
  g.nkids = (uint32_t)n;
  g.flags = GNF_OMITTED;
  const gnode * f = &c->nodes[src_from];
  const gnode * l = &c->nodes[src_to];
  g.src_off = f->src_off;
  g.src_len = l->src_off + l->src_len >= f->src_off ? l->src_off + l->src_len - f->src_off : 0;
  const uint32_t made = add_node(L, &g);
  if (made != GN_NONE) {
    nl[made] = nullable;
  }
  return made;
}

/// Values 4, 2: "Commas specified in the grammar are implicitly omissible in some circumstances, when used to separate optional
/// terms in the grammar. Within a top-level list in a property or other CSS value, or a function's argument list, a comma specified
/// in the grammar must be omitted if: all items preceding the comma have been omitted; all items following the comma have been
/// omitted; multiple commas would be adjacent (ignoring white space/comments), due to the items between the commas being omitted."
/// A juxtaposition `S0 , S1 , S2 ...` in which some segment can be empty is therefore a sequence of the segments that are present,
/// with a comma between each two of them and none otherwise; a segment is present when it matches something. It is written out as
/// nodes: the first present segment (an alternative for each segment that can be the first), then for each later segment either `, S`
/// or, when it can be empty, `[ , !S ]?`.
static bool omit_commas(linker * L, uint32_t node, size_t m, const size_t * begin, const size_t * count, const bool * good, uint8_t * nl) {
  GTEXT_CSS_Compiled * c = L->c;
  const gnode g = c->nodes[node];
  uint32_t * orig = gtext_allocator_malloc(&L->alloc, g.nkids * sizeof *orig);
  if (!orig) {
    lfail(L, GTEXT_CSS_E_OOM);
    return false;
  }
  memcpy(orig, c->kids + g.kids, g.nkids * sizeof *orig);
  uint32_t seg[OMIT_MAX_SEGMENTS], req[OMIT_MAX_SEGMENTS], alt[OMIT_MAX_SEGMENTS];
  /* what follows a segment that is present: `, S` (two items, flat) when S cannot be empty, else the one item `[ , !S ]?` */
  uint32_t tail[OMIT_MAX_SEGMENTS][2];
  size_t tail_n[OMIT_MAX_SEGMENTS];
  uint32_t lit = GN_NONE;
  for (uint32_t i = 0; i < g.nkids && lit == GN_NONE; i++) {
    if (is_sep_literal(c, orig[i], ',')) {
      lit = orig[i];
    }
  }
  if (lit == GN_NONE) {
    gtext_allocator_free(&L->alloc, orig);
    return true; /* no comma to work out */
  }
  bool ok = true;
  for (size_t i = 0; i < m && ok; i++) {
    if (count[i] == 1) {
      seg[i] = orig[begin[i]];
    }
    else {
      /* Not nullable as far as `nl` is concerned: a segment is used only as the first item of an alternative (through `req`, which is
       * never nullable) or after a comma, so nothing reads this flag; a segment that can be empty is made optional by `good`. */
      seg[i] = make_inner(L, GN_SEQ, orig + begin[i], count[i], 1, 1, orig[begin[i]], orig[begin[i] + count[i] - 1], nl, 0);
      ok = seg[i] != GN_NONE;
    }
    if (!ok) {
      break;
    }
    if (good[i]) {
      req[i] = seg[i];
    }
    else {
      req[i] = make_inner(L, GN_REQ, &seg[i], 1, 1, 1, seg[i], seg[i], nl, 0);
      ok = req[i] != GN_NONE;
    }
  }
  for (size_t j = 1; j < m && ok; j++) {
    if (good[j]) {
      tail[j][0] = lit;
      tail[j][1] = seg[j];
      tail_n[j] = 2;
    }
    else {
      const uint32_t two[2] = {lit, req[j]};
      uint32_t p = make_inner(L, GN_SEQ, two, 2, 1, 1, lit, seg[j], nl, 0);
      ok = p != GN_NONE;
      if (ok) {
        p = make_inner(L, GN_MULT, &p, 1, 0, 1, lit, seg[j], nl, 1);
        ok = p != GN_NONE;
      }
      tail[j][0] = p;
      tail_n[j] = 1;
    }
  }
  /* the first present segment: the first segment if it cannot be empty, else an alternative for each segment that can be first */
  size_t last_first = 0;
  while (ok && last_first + 1 < m && !good[last_first]) {
    last_first++;
  }
  uint32_t items[2 * OMIT_MAX_SEGMENTS + 2];
  size_t nalt = 0;
  for (size_t k = 0; k <= last_first && ok; k++) {
    size_t n = 0;
    items[n++] = req[k];
    for (size_t j = k + 1; j < m; j++) {
      for (size_t t = 0; t < tail_n[j]; t++) {
        items[n++] = tail[j][t];
      }
    }
    alt[nalt] = n == 1 ? items[0] : make_inner(L, GN_SEQ, items, n, 1, 1, seg[k], seg[m - 1], nl, 0);
    ok = alt[nalt] != GN_NONE;
    nalt++;
  }
  if (ok) {
    /* the juxtaposition becomes the node it is now equal to; its index is what its parents hold */
    const bool nullable_all = last_first + 1 == m && !good[last_first];
    uint32_t first;
    uint32_t body = GN_NONE;
    if (nalt == 1) {
      body = alt[0];  /* the first segment cannot be empty: the segments in order */
    }
    else {
      body = make_inner(L, GN_ALT, alt, nalt, 1, 1, seg[0], seg[m - 1], nl, 0); /* read by nothing: the node the parents hold carries the flag */
      ok = body != GN_NONE;
    }
    if (ok && nullable_all) {
      /* every segment can be empty: the whole may be empty */
      if (add_kid_run(L, &body, 1, &first)) {
        gnode * w = &c->nodes[node];
        w->kind = GN_MULT;
        w->min = 0;
        w->max = 1;
        w->kids = first;
        w->nkids = 1;
        w->sep = 0;
      }
      else {
        ok = false;
      }
    }
    else if (ok) {
      /* copy the body node's own shape into the node the parents hold */
      const gnode b = c->nodes[body];
      gnode * w = &c->nodes[node];
      w->kind = b.kind;
      w->kids = b.kids;
      w->nkids = b.nkids;
      w->min = b.min;
      w->max = b.max;
      w->sep = 0;
      w->flags |= GNF_OMITTED;
    }
  }
  gtext_allocator_free(&L->alloc, orig);
  return ok && L->status == GTEXT_CSS_OK;
}

/// A juxtaposition whose literal `ch` items can only separate non-empty segments is a list of those segments with `ch`
/// as its separator: the segments are the items between the literals, and each has to consume something (it has an item
/// that cannot match nothing), or a separator could be missing from the text the value is written as. Otherwise the
/// literals stay items. `nl` says which nodes can match nothing; new nodes are never empty.
static bool split_one(linker * L, uint32_t node, uint32_t ch, uint8_t sep, uint8_t * nl) {
  GTEXT_CSS_Compiled * c = L->c;
  const gnode g = c->nodes[node];
  const uint32_t * kids = c->kids + g.kids;
  size_t segments = 1, run = 0;
  bool good = false, any = false, all_good = true;
  size_t sbegin[OMIT_MAX_SEGMENTS], scount[OMIT_MAX_SEGMENTS];
  bool sgood[OMIT_MAX_SEGMENTS];
  bool room = true; /* the segments fit the arrays above */
  size_t begin_run = 0;
  for (uint32_t i = 0; i < g.nkids; i++) {
    if (is_sep_literal(c, kids[i], ch)) {
      any = true;
      if (run == 0) {
        return true; /* an empty segment: not a list */
      }
      if (segments <= OMIT_MAX_SEGMENTS) {
        sbegin[segments - 1] = begin_run;
        scount[segments - 1] = run;
        sgood[segments - 1] = good;
      }
      else {
        room = false;
      }
      all_good = all_good && good;
      segments++;
      run = 0;
      good = false;
      begin_run = i + 1;
    }
    else {
      run++;
      good = good || !nl[kids[i]];
    }
  }
  if (!any || run == 0) {
    return true;
  }
  if (segments <= OMIT_MAX_SEGMENTS) {
    sbegin[segments - 1] = begin_run;
    scount[segments - 1] = run;
    sgood[segments - 1] = good;
  }
  else {
    room = false;
  }
  all_good = all_good && good;
  if (!all_good) {
    /* a segment can be empty: the commas around it are omissible, and are not separators of a list */
    return ch == ',' && room ? omit_commas(L, node, segments, sbegin, scount, sgood, nl) : true;
  }
  uint32_t * seg = gtext_allocator_malloc(&L->alloc, segments * sizeof *seg);
  if (!seg) {
    lfail(L, GTEXT_CSS_E_OOM);
    return false;
  }
  size_t k = 0, begin = 0;
  uint32_t * items = c->kids + g.kids; /* (the kid array may move while nodes are added: indexes are used below) */
  (void)items;
  for (uint32_t i = 0; i <= g.nkids && L->status == GTEXT_CSS_OK; i++) {
    if (i == g.nkids || is_sep_literal(c, c->kids[g.kids + i], ch)) {
      const size_t count = i - begin;
      if (count == 1) {
        seg[k++] = c->kids[g.kids + begin];
      }
      else {
        gnode sn;
        memset(&sn, 0, sizeof sn);
        sn.kind = GN_SEQ;
        sn.min = sn.max = 1;
        sn.target = UINT32_MAX;
        sn.nkids = (uint32_t)count;
        const gnode * f = &c->nodes[c->kids[g.kids + begin]];
        const gnode * l = &c->nodes[c->kids[g.kids + i - 1]];
        sn.src_off = f->src_off;
        sn.src_len = l->src_off + l->src_len - f->src_off;
        uint32_t first;
        uint32_t tmp[64];
        uint32_t * run_items = count <= 64 ? tmp : gtext_allocator_malloc(&L->alloc, count * sizeof *run_items);
        if (!run_items) {
          lfail(L, GTEXT_CSS_E_OOM);
          break;
        }
        memcpy(run_items, c->kids + g.kids + begin, count * sizeof *run_items);
        const bool added = add_kid_run(L, run_items, count, &first);
        if (run_items != tmp) {
          gtext_allocator_free(&L->alloc, run_items);
        }
        if (!added) {
          break;
        }
        sn.kids = first;
        const uint32_t made = add_node(L, &sn);
        if (made == GN_NONE) {
          break;
        }
        nl[made] = 0;
        seg[k++] = made;
      }
      begin = i + 1;
    }
  }
  if (L->status == GTEXT_CSS_OK) {
    uint32_t first;
    if (add_kid_run(L, seg, k, &first)) {
      gnode * w = &c->nodes[node];
      w->kids = first;
      w->nkids = (uint32_t)k;
      w->sep = sep;
    }
  }
  gtext_allocator_free(&L->alloc, seg);
  return L->status == GTEXT_CSS_OK;
}

static bool split_sequences(linker * L, uint8_t * nl) {
  GTEXT_CSS_Compiled * c = L->c;
  for (size_t i = 0; i < c->nnodes && L->status == GTEXT_CSS_OK; i++) {
    const gnode g = c->nodes[i];
    if (g.kind != GN_SEQ || g.sep != 0 || g.nkids < 3 || (g.flags & GNF_OMITTED)) {
      continue;
    }
    L->work += g.nkids;
    if (!split_one(L, (uint32_t)i, ',', 1, nl)) {
      return false;
    }
    if (c->nodes[i].sep == 0 && !(c->nodes[i].flags & GNF_OMITTED) && !split_one(L, (uint32_t)i, '/', 2, nl)) {
      return false;
    }
  }
  return L->status == GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_definition_link(const GTEXT_CSS_Definition * def, GTEXT_CSS_Resolver resolver,
    void * user, const GTEXT_CSS_Grammar_Options * options, GTEXT_CSS_Compiled ** out,
    GTEXT_CSS_Grammar_Result * result) {
  if (!def || !out) {
    css_grammar_result(result, GTEXT_CSS_E_INVALID, 0, NULL, 0, 0, 0);
    return GTEXT_CSS_E_INVALID;
  }
  size_t max_nodes, max_bytes;
  css_grammar_limits(options, &max_bytes, NULL, &max_nodes);
  linker L;
  memset(&L, 0, sizeof L);
  L.max_bytes = max_bytes;
  L.alloc = css_grammar_allocator(options);
  L.max_nodes = max_nodes;
  L.resolver = resolver;
  L.user = user;
  GTEXT_CSS_Compiled * c = gtext_allocator_malloc(&L.alloc, sizeof *c);
  if (!c) {
    css_grammar_result(result, GTEXT_CSS_E_OOM, 0, NULL, 0, 0, 0);
    return GTEXT_CSS_E_OOM;
  }
  memset(c, 0, sizeof *c);
  c->alloc = L.alloc;
  L.c = c;
  uint8_t * nl = NULL;
  size_t root_index;
  if (instantiate(&L, def, false, 0.0, 0.0, RC_NONE, 0, 0, 0, &root_index)) {
    c->root = L.inst[root_index].root;
    while (L.next < L.ninst && resolve_instance(&L, L.next)) {
      L.next++;
    }
  }
  if (L.status == GTEXT_CSS_OK) {
    /* room for the nodes that splitting a juxtaposition into segments, or working out its omissible commas, can add: at most six for each kid */
    nl = gtext_allocator_calloc(&L.alloc, c->nnodes + 6 * c->nkids + 8, 1);
    if (!nl) {
      lfail(&L, GTEXT_CSS_E_OOM);
    }
  }
  if (L.status == GTEXT_CSS_OK && nullable_worklist(&L, nl)) {
    /* the juxtapositions are split (and their omissible commas worked out) first: a left recursion that only the rewritten
     * form shows (`y = a? , [<y>]?`) is found in it. The nullability the rewrite relies on is the one decided above. */
    if (split_sequences(&L, nl)) {
      find_left_recursion(&L, nl);
    }
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
        const GTEXT_CSS_Status ast = gtext_css_atom_intern(c->atoms, c->text + g->str_off, g->str_len, &a);
        if (ast != GTEXT_CSS_OK) {
          lfail(&L, ast);
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
    css_grammar_result(result, L.status, 0, L.err_name, L.err_len, nodes, L.work);
    return L.status;
  }
  css_grammar_result(result, GTEXT_CSS_OK, 0, NULL, 0, c->nnodes, L.work);
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
