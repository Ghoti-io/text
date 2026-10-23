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
 * The typed value model's plumbing: the keyword atoms, the value set that owns
 * a match's values, the names of the kinds, and the scan for arbitrary
 * substitution functions.
 *
 * Reference: CSS Variables Level 1 and CSS Values 5 (`var()`, `env()`, `attr()`
 * are the arbitrary substitution functions; a declaration that contains one is
 * valid at parse time and checked after substitution).
 */

#include <ghoti.io/text/macros.h>

#include <stdint.h>
#include <string.h>

#include "css_grammar_internal.h"
#include "css_value_internal.h"

/* ---- atoms ------------------------------------------------------------ */

typedef struct {
  uint32_t off; ///< In the pool.
  uint32_t len; ///< Without the NUL.
} atom_entry;

struct GTEXT_CSS_Atom_Table {
  GTEXT_Allocator alloc;  ///< Everything is allocated through this.
  char * pool;            ///< The spellings, each NUL-terminated.
  size_t pool_len;        ///< In use.
  size_t pool_cap;        ///< Allocated.
  atom_entry * entries;   ///< Atom n is entries[n - 1].
  size_t count;           ///< Atoms.
  size_t cap;             ///< Allocated.
  uint32_t * slots;       ///< Open-addressed hash of atom numbers (0 empty).
  size_t nslots;          ///< A power of two.
};

static char fold(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static uint32_t hash_fold(const char * s, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) {
    h ^= (unsigned char)fold(s[i]);
    h *= 16777619u;
  }
  return h;
}

static bool equal_fold(const char * a, const char * b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (fold(a[i]) != fold(b[i])) {
      return false;
    }
  }
  return true;
}

GTEXT_CSS_Status gtext_css_atom_table_new(const GTEXT_Allocator * allocator, GTEXT_CSS_Atom_Table ** out) {
  if (!out) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_Allocator a = *(allocator ? allocator : gtext_allocator_default());
  GTEXT_CSS_Atom_Table * t = gtext_allocator_malloc(&a, sizeof *t);
  if (!t) {
    return GTEXT_CSS_E_OOM;
  }
  memset(t, 0, sizeof *t);
  t->alloc = a;
  *out = t;
  return GTEXT_CSS_OK;
}

void gtext_css_atom_table_free(GTEXT_CSS_Atom_Table * t) {
  if (!t) {
    return;
  }
  GTEXT_Allocator a = t->alloc;
  gtext_allocator_free(&a, t->pool);
  gtext_allocator_free(&a, t->entries);
  gtext_allocator_free(&a, t->slots);
  gtext_allocator_free(&a, t);
}

static size_t find_slot(const GTEXT_CSS_Atom_Table * t, const char * s, size_t n, uint32_t h) {
  const size_t mask = t->nslots - 1;
  size_t i = h & mask;
  while (t->slots[i]) {
    const atom_entry * e = &t->entries[t->slots[i] - 1];
    if (e->len == n && equal_fold(t->pool + e->off, s, n)) {
      return i;
    }
    i = (i + 1) & mask;
  }
  return i;
}

static bool rehash(GTEXT_CSS_Atom_Table * t, size_t nslots) {
  uint32_t * slots = gtext_allocator_calloc(&t->alloc, nslots, sizeof *slots);
  if (!slots) {
    return false;
  }
  gtext_allocator_free(&t->alloc, t->slots);
  t->slots = slots;
  t->nslots = nslots;
  for (size_t k = 0; k < t->count; k++) {
    const atom_entry * e = &t->entries[k];
    const uint32_t h = hash_fold(t->pool + e->off, e->len);
    size_t i = h & (nslots - 1);
    while (t->slots[i]) {
      i = (i + 1) & (nslots - 1);
    }
    t->slots[i] = (uint32_t)(k + 1);
  }
  return true;
}

GTEXT_CSS_Status gtext_css_atom_intern(GTEXT_CSS_Atom_Table * t, const char * text, size_t length,
    GTEXT_CSS_Atom * out) {
  if (!t || !out || !text || length == 0 || length > UINT32_MAX / 2) {
    return GTEXT_CSS_E_INVALID;
  }
  if (t->nslots) {
    const size_t i = find_slot(t, text, length, hash_fold(text, length));
    if (t->slots[i]) {
      *out = t->slots[i];
      return GTEXT_CSS_OK;
    }
  }
  if (t->count + 1 > UINT32_MAX / 2) {
    return GTEXT_CSS_E_OOM;
  }
  if ((t->count + 1) * 2 > t->nslots && !rehash(t, t->nslots ? t->nslots * 2 : 16)) {
    return GTEXT_CSS_E_OOM;
  }
  if (t->pool_len + length + 1 > t->pool_cap) {
    size_t cap = t->pool_cap ? t->pool_cap : 256;
    while (cap < t->pool_len + length + 1) {
      cap *= 2;
    }
    char * grown = gtext_allocator_realloc(&t->alloc, t->pool, cap);
    if (!grown) {
      return GTEXT_CSS_E_OOM;
    }
    t->pool = grown;
    t->pool_cap = cap;
  }
  if (t->count == t->cap) {
    const size_t cap = t->cap ? t->cap * 2 : 16;
    atom_entry * grown = gtext_allocator_realloc(&t->alloc, t->entries, cap * sizeof *grown);
    if (!grown) {
      return GTEXT_CSS_E_OOM;
    }
    t->entries = grown;
    t->cap = cap;
  }
  memcpy(t->pool + t->pool_len, text, length);
  t->pool[t->pool_len + length] = '\0';
  t->entries[t->count].off = (uint32_t)t->pool_len;
  t->entries[t->count].len = (uint32_t)length;
  t->pool_len += length + 1;
  t->count++;
  const size_t i = find_slot(t, text, length, hash_fold(text, length));
  t->slots[i] = (uint32_t)t->count;
  *out = (GTEXT_CSS_Atom)t->count;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Atom gtext_css_atom_lookup(const GTEXT_CSS_Atom_Table * t, const char * text, size_t length) {
  if (!t || !t->nslots || !text || length == 0) {
    return GTEXT_CSS_ATOM_NONE;
  }
  return t->slots[find_slot(t, text, length, hash_fold(text, length))];
}

const char * gtext_css_atom_name(const GTEXT_CSS_Atom_Table * t, GTEXT_CSS_Atom atom, size_t * length) {
  if (!t || atom == 0 || atom > t->count) {
    return NULL;
  }
  const atom_entry * e = &t->entries[atom - 1];
  if (length) {
    *length = e->len;
  }
  return t->pool + e->off;
}

size_t gtext_css_atom_count(const GTEXT_CSS_Atom_Table * t) {
  return t ? t->count : 0;
}

/* ---- value sets ------------------------------------------------------- */

const GTEXT_CSS_Value * gtext_css_value_set_root(const GTEXT_CSS_Value_Set * set) {
  return set ? set->root : NULL;
}

size_t gtext_css_value_set_count(const GTEXT_CSS_Value_Set * set) {
  return set ? set->count : 0;
}

void gtext_css_value_set_free(GTEXT_CSS_Value_Set * set) {
  if (!set) {
    return;
  }
  GTEXT_Allocator a = set->alloc;
  css_arena_free(&set->arena);
  gtext_allocator_free(&a, set);
}

static const char * const kind_names[GTEXT_CSS_VAL_KIND_COUNT] = {
  [GTEXT_CSS_VAL_KEYWORD] = "keyword",
  [GTEXT_CSS_VAL_CUSTOM_IDENT] = "custom-ident",
  [GTEXT_CSS_VAL_DASHED_IDENT] = "dashed-ident",
  [GTEXT_CSS_VAL_STRING] = "string",
  [GTEXT_CSS_VAL_URL] = "url",
  [GTEXT_CSS_VAL_NUMBER] = "number",
  [GTEXT_CSS_VAL_INTEGER] = "integer",
  [GTEXT_CSS_VAL_PERCENTAGE] = "percentage",
  [GTEXT_CSS_VAL_DIMENSION] = "dimension",
  [GTEXT_CSS_VAL_RATIO] = "ratio",
  [GTEXT_CSS_VAL_ZERO] = "zero",
  [GTEXT_CSS_VAL_LIST] = "list",
  [GTEXT_CSS_VAL_FUNCTION] = "function",
  [GTEXT_CSS_VAL_UNICODE_RANGE] = "unicode-range",
  [GTEXT_CSS_VAL_PENDING] = "pending-substitution",
  [GTEXT_CSS_VAL_UNPARSED] = "unparsed",
  [GTEXT_CSS_VAL_TOKEN] = "token",
  [GTEXT_CSS_VAL_BLOCK] = "block",
};

const char * gtext_css_val_kind_name(GTEXT_CSS_Val_Kind kind) {
  if ((unsigned)kind >= (unsigned)GTEXT_CSS_VAL_KIND_COUNT) {
    return "unknown";
  }
  return kind_names[kind];
}

/* ---- arbitrary substitution ------------------------------------------- */

bool css_is_substitution_name(const GTEXT_CSS_String * name) {
  return css_ascii_ieq(name, "var") || css_ascii_ieq(name, "env") || css_ascii_ieq(name, "attr");
}

typedef struct {
  const GTEXT_CSS_Component_Value * values;
  size_t count;
  size_t index;
} scan_frame;

GTEXT_CSS_Status gtext_css_has_arbitrary_substitution(const GTEXT_CSS_Component_Value * values, size_t count,
    const GTEXT_Allocator * allocator, bool * found) {
  if (!found || (!values && count)) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_Allocator a = *(allocator ? allocator : gtext_allocator_default());
  scan_frame * stack = NULL;
  size_t len = 0, cap = 0;
  bool hit = false;
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  if (count) {
    cap = 16;
    stack = gtext_allocator_malloc(&a, cap * sizeof *stack);
    if (!stack) {
      return GTEXT_CSS_E_OOM;
    }
    stack[len++] = (scan_frame){values, count, 0};
  }
  while (len > 0 && !hit) {
    scan_frame * f = &stack[len - 1];
    if (f->index >= f->count) {
      len--;
      continue;
    }
    const GTEXT_CSS_Component_Value * v = &f->values[f->index++];
    if (v->kind == GTEXT_CSS_VALUE_FUNCTION && css_is_substitution_name(&v->value)) {
      hit = true;
      break;
    }
    if (v->kind != GTEXT_CSS_VALUE_TOKEN && v->child_count) {
      if (len == cap) {
        if (cap > SIZE_MAX / 2 / sizeof *stack) {
          status = GTEXT_CSS_E_OOM;
          break;
        }
        scan_frame * grown = gtext_allocator_realloc(&a, stack, cap * 2 * sizeof *stack);
        if (!grown) {
          status = GTEXT_CSS_E_OOM;
          break;
        }
        stack = grown;
        cap *= 2;
      }
      stack[len++] = (scan_frame){v->children, v->child_count, 0};
    }
  }
  gtext_allocator_free(&a, stack);
  if (status != GTEXT_CSS_OK) {
    return status;
  }
  *found = hit;
  return GTEXT_CSS_OK;
}
