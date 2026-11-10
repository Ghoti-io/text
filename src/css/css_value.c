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
#include "css_readings.h"

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
  size_t max_atoms;       ///< Most atoms (GTEXT_CSS_E_LIMIT_ATOMS beyond).
  size_t max_bytes;       ///< Most bytes of spelling, NULs included.
};

/// The defaults of an atom table's limits.
#define ATOMS_DEFAULT_MAX ((size_t)1 << 24)
#define ATOMS_DEFAULT_BYTES ((size_t)1 << 30)

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
  t->max_atoms = ATOMS_DEFAULT_MAX;
  t->max_bytes = ATOMS_DEFAULT_BYTES;
  *out = t;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_atom_table_limits(GTEXT_CSS_Atom_Table * t, size_t max_atoms, size_t max_bytes) {
  if (!t) {
    return GTEXT_CSS_E_INVALID;
  }
  /* an atom is a uint32_t and an offset into the pool is one too: the limits stay inside both */
  t->max_atoms = max_atoms ? (max_atoms < ATOMS_DEFAULT_MAX ? max_atoms : ATOMS_DEFAULT_MAX) : ATOMS_DEFAULT_MAX;
  t->max_bytes = max_bytes ? (max_bytes < ATOMS_DEFAULT_BYTES ? max_bytes : ATOMS_DEFAULT_BYTES) : ATOMS_DEFAULT_BYTES;
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
  if (!t || !out || !text || length == 0) {
    return GTEXT_CSS_E_INVALID;
  }
  if (t->nslots) {
    const size_t i = find_slot(t, text, length, hash_fold(text, length));
    if (t->slots[i]) {
      *out = t->slots[i];
      return GTEXT_CSS_OK;
    }
  }
  if (t->count >= t->max_atoms || length >= t->max_bytes || t->pool_len > t->max_bytes - length - 1) {
    return GTEXT_CSS_E_LIMIT_ATOMS;
  }
  if ((t->count + 1) * 2 > t->nslots && !rehash(t, t->nslots ? t->nslots * 2 : 16)) {
    return GTEXT_CSS_E_OOM;
  }
  if (t->pool_len + length + 1 > t->pool_cap) {
    size_t cap = t->pool_cap ? t->pool_cap : 256;
    while (cap < t->pool_len + length + 1) {
      if (cap > SIZE_MAX / 2) {
        return GTEXT_CSS_E_OOM;
      }
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

GTEXT_CSS_Value_Set * css_vset_new(const GTEXT_Allocator * alloc) {
  GTEXT_CSS_Value_Set * set = gtext_allocator_malloc(alloc, sizeof *set);
  if (!set) {
    return NULL;
  }
  memset(set, 0, sizeof *set);
  set->alloc = *alloc;
  css_arena_init(&set->arena, &set->alloc);
  gcu_atomic_int_init(&set->refs, 1);
  return set;
}

bool css_vset_depend(GTEXT_CSS_Value_Set * set, const GTEXT_CSS_Value_Set * other) {
  if (!set || !other || set == other) {
    return true;
  }
  for (size_t i = set->ndeps; i-- > 0;) {
    if (set->deps[i] == other) {
      return true;
    }
  }
  if (set->ndeps == set->depcap) {
    const size_t ncap = set->depcap ? set->depcap * 2 : 4;
    if (ncap > SIZE_MAX / sizeof *set->deps) {
      return false;
    }
    GTEXT_CSS_Value_Set ** grown = gtext_allocator_realloc(&set->alloc, set->deps, ncap * sizeof *grown);
    if (!grown) {
      return false;
    }
    set->deps = grown;
    set->depcap = ncap;
  }
  set->deps[set->ndeps++] = gtext_css_value_set_retain(other);
  return true;
}

GTEXT_CSS_Value_Set * gtext_css_value_set_retain(const GTEXT_CSS_Value_Set * set) {
  if (!set) {
    return NULL;
  }
  /* the count is the only thing that changes in a set after it is made, and it changes atomically */
  GTEXT_CSS_Value_Set * mutable_set = (GTEXT_CSS_Value_Set *)(uintptr_t)set;
  (void)gcu_atomic_int_fetch_add(&mutable_set->refs, 1);
  return mutable_set;
}

void gtext_css_value_set_free(GTEXT_CSS_Value_Set * set) {
  /* Iterative: a set may keep a chain of sets alive (each computed value that points into the one before), and a deep chain must not
     recurse on release. */
  GTEXT_CSS_Value_Set * pending = NULL;  /* a list of sets to release, linked through `root` (a set being released has no use for it) */
  if (!set) {
    return;
  }
  if (gcu_atomic_int_fetch_sub(&set->refs, 1) != 1) {
    return;
  }
  pending = set;
  set->root = NULL;
  while (pending) {
    GTEXT_CSS_Value_Set * cur = pending;
    pending = (GTEXT_CSS_Value_Set *)(uintptr_t)cur->root;
    for (size_t i = 0; i < cur->ndeps; i++) {
      GTEXT_CSS_Value_Set * d = cur->deps[i];
      if (gcu_atomic_int_fetch_sub(&d->refs, 1) == 1) {
        d->root = (const GTEXT_CSS_Value *)(uintptr_t)pending;
        pending = d;
      }
    }
    GTEXT_Allocator a = cur->alloc;
    if (cur->deps) {
      gtext_allocator_free(&a, cur->deps);
    }
    css_arena_free(&cur->arena);
    gtext_allocator_free(&a, cur);
  }
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
  [GTEXT_CSS_VAL_COLOR] = "color",
};

const char * gtext_css_val_kind_name(GTEXT_CSS_Val_Kind kind) {
  if ((unsigned)kind >= (unsigned)GTEXT_CSS_VAL_KIND_COUNT) {
    return "unknown";
  }
  return kind_names[kind];
}

/// Whether an identifier is one an unquoted family name may not hold: a generic family keyword or a CSS-wide keyword (Fonts 4, 2.1: "Font family names that happen to be the
/// same as a keyword value (inherit, serif, sans-serif, monospace, fantasy, and cursive) must be quoted to prevent confusion with the keywords with the same names").
bool css_is_reserved_family_word(const GTEXT_CSS_String * w) {
  static const char * const words[] = {"serif", "sans-serif", "cursive", "fantasy", "monospace", "system-ui", "ui-serif", "ui-sans-serif", "ui-monospace", "ui-rounded", "emoji",
      "math", "fangsong", "initial", "inherit", "unset", "revert", "revert-layer", "revert-rule", "default"};
  for (size_t i = 0; i < sizeof words / sizeof *words; i++) {
    if (css_ascii_ieq(w, words[i])) {
      return true;
    }
  }
  return false;
}

/* ---- arbitrary substitution ------------------------------------------- */

/* The arbitrary substitution functions, as the served drafts name them (Values 5: "The if() function is an arbitrary
   substitution function", "inherit() is an arbitrary substitution function", "attr() is an arbitrary substitution
   function, similar to var()", "The random-item() function is an arbitrary substitution function, like var()", and
   ident(); Variables 1: "The var() function is an arbitrary substitution function"; Environment Variables: env()). cycle() is a <whole-value> that needs the
   element's inherited value ("compare the property's inherited value with each Cn"), so it is resolved when a style is computed as the others are.
   The draft's list is not in one place; a function with a dashed name (a custom function) is not in it either. */
bool css_is_substitution_name(const GTEXT_CSS_String * name) {
  /* Mixins 1 (served 2026-10-31), 2: "<dashed-function>s are arbitrary substitution functions, like var(). Their presence in a property's value causes it to be
     assumed valid at parse time, and only evaluated and parsed at computed-value time", and "A <dashed-function> is a functional notation whose function name
     starts with two dashes". */
  if (name->length > 2 && name->data[0] == '-' && name->data[1] == '-') {
    return true;
  }
  static const char * const names[] = {"var", "env", "attr", "if", "inherit", "ident", "random-item", "cycle"};
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
    if (css_ascii_ieq(name, names[i])) {
      return true;
    }
  }
  return false;
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

static bool sub_name_is(const GTEXT_CSS_Component_Value * v, const char * lower) {
  return css_ascii_ieq(&v->value, lower);
}

/**
 * The argument grammar of a dashed function (Mixins 1, 2.1, as pinned 2026-11-07): "<dashed-function> = --*( [ <declaration-value>? ]#? ,? )", "That is, a comma-separated
 * list of anything (including an empty value), optionally with a final trailing comma", so any number of comma-separated arguments, each of which may be empty, with the {}
 * wrapper of Values 5 as a whole argument that holds something (a {} block beside other tokens, or an empty one, is no argument), and no top-level `;` or `!`. (The earlier
 * pin had "--*( <declaration-value>#? )", which refused an empty argument and a trailing comma.) A final empty argument is dropped when the call is evaluated, so the syntax
 * accepts every comma-separated list.
 */
static bool dashed_function_syntax(const GTEXT_CSS_Component_Value * fn) {
  bool has_content = false, has_block = false, has_other = false, block_empty = true;
  for (size_t k = 0; k <= fn->child_count; k++) {
    const GTEXT_CSS_Component_Value * c = k < fn->child_count ? &fn->children[k] : NULL;
    if (!c || (c->kind == GTEXT_CSS_VALUE_TOKEN && c->type == GTEXT_CSS_TOKEN_COMMA)) {
      if (has_content && has_block && (has_other || block_empty)) {
        return false;
      }
      has_content = has_block = has_other = false;
      block_empty = true;
      continue;
    }
    if (c->kind == GTEXT_CSS_VALUE_TOKEN) {
      if (c->type == GTEXT_CSS_TOKEN_WHITESPACE) {
        continue;
      }
      if (c->type == GTEXT_CSS_TOKEN_SEMICOLON || (c->type == GTEXT_CSS_TOKEN_DELIM && c->delim == '!')) {
        return false;
      }
    }
    has_content = true;
    if (c->kind == GTEXT_CSS_VALUE_BLOCK && c->type == GTEXT_CSS_TOKEN_OPEN_CURLY) {
      has_block = true;
      for (size_t j = 0; j < c->child_count; j++) {
        const GTEXT_CSS_Component_Value * b = &c->children[j];
        if (b->kind == GTEXT_CSS_VALUE_TOKEN && (b->type == GTEXT_CSS_TOKEN_SEMICOLON || (b->type == GTEXT_CSS_TOKEN_DELIM && b->delim == '!'))) {
          return false;  /* what is inside the {} is a <declaration-value> as well */
        }
        if (!(b->kind == GTEXT_CSS_VALUE_TOKEN && b->type == GTEXT_CSS_TOKEN_WHITESPACE)) {
          block_empty = false;
        }
      }
    }
    else {
      has_other = true;
    }
  }
  return true;
}

/**
 * The argument grammar of random-item(), which is an arbitrary substitution function (Values 5, 10.3): "random-item( <random-key> , [ <declaration-value>? ]# )", where
 * "wrapping the values in {} allows their commas to be distinguished from the function's argument-separating commas", and the wrapper is "only allowed for some
 * function arguments--those defined as free-form productions": a {} block is a whole argument, with nothing but white space beside it. So the key before the first
 * comma has to hold something, at least one item has to follow, and no item holds a top-level `;` or `!`.
 */
static bool random_item_syntax(const GTEXT_CSS_Component_Value * fn) {
  size_t group = 0;
  bool has_content = false, has_block = false, has_other = false;
  for (size_t k = 0; k <= fn->child_count; k++) {
    const GTEXT_CSS_Component_Value * c = k < fn->child_count ? &fn->children[k] : NULL;
    if (!c || (c->kind == GTEXT_CSS_VALUE_TOKEN && c->type == GTEXT_CSS_TOKEN_COMMA)) {
      if (group == 0 && !has_content) {
        return false;  /* the key is empty */
      }
      if (has_block && has_other) {
        return false;  /* a {} block is the whole argument */
      }
      group++;
      has_content = has_block = has_other = false;
      continue;
    }
    if (c->kind == GTEXT_CSS_VALUE_TOKEN) {
      if (c->type == GTEXT_CSS_TOKEN_WHITESPACE) {
        continue;
      }
      if (c->type == GTEXT_CSS_TOKEN_SEMICOLON || (c->type == GTEXT_CSS_TOKEN_DELIM && c->delim == '!')) {
        return false;
      }
    }
    has_content = true;
    if (c->kind == GTEXT_CSS_VALUE_BLOCK && c->type == GTEXT_CSS_TOKEN_OPEN_CURLY) {
      has_block = true;
    }
    else {
      has_other = true;
    }
  }
  return group >= 2;  /* the key and at least one item (an item may be empty) */
}

#if !GTEXT_CSS_READING_ENV_UNKNOWN_NAME_VALID
/// Whether the first argument of an `env()` reads `<custom-ident> <integer [0,∞]>*` (css_readings.h, GTEXT_CSS_READING_ENV_UNKNOWN_NAME_VALID): a name, then indexes.
static bool env_first_argument_is_a_name(const GTEXT_CSS_Component_Value * fn) {
  bool named = false;
  for (size_t k = 0; k < fn->child_count; k++) {
    const GTEXT_CSS_Component_Value * c = &fn->children[k];
    if (c->kind == GTEXT_CSS_VALUE_TOKEN && c->type == GTEXT_CSS_TOKEN_COMMA) {
      break;
    }
    if (c->kind == GTEXT_CSS_VALUE_TOKEN && c->type == GTEXT_CSS_TOKEN_WHITESPACE) {
      continue;
    }
    if (!named) {
      if (c->kind != GTEXT_CSS_VALUE_TOKEN || c->type != GTEXT_CSS_TOKEN_IDENT || gtext_css_wide_keyword_lookup(c->value.data, c->value.length) != 0) {
        return false;
      }
      named = true;
    }
    else if (c->kind != GTEXT_CSS_VALUE_TOKEN || c->type != GTEXT_CSS_TOKEN_NUMBER || !(c->flags & GTEXT_CSS_FLAG_INTEGER) || c->number < 0) {
      return false;
    }
  }
  return named;
}
#endif

/**
 * Whether every `var()`, `env()` and `attr()` in the values is syntactically valid. Values 5, on the arbitrary substitution functions:
 * "If a property value contains one or more arbitrary substitution functions, and all of those functions are themselves syntactically
 * valid according to their argument grammars, the entire value's grammar must be assumed to be valid at parse time". The argument
 * grammar of the three is `var( <declaration-value> , <declaration-value>? )` (Variables 1, 3; `<env-args>` and `<attr-args>` the
 * same): the first argument, up to the first top-level comma, has to hold something, so `var()` and `var(, x)` are not valid.
 * What the first argument names (a `--` name, an environment variable) is checked when the value is substituted, not now.
 * The second argument starts with a `{` block or not: when it does, it is that block and nothing else (Values 5, "Commas in Function Arguments"), so `var(--a, {x} y)` is not valid.
 * Walks with its own stack, so nesting costs memory and not stack.
 */
GTEXT_CSS_Status css_substitution_syntax(const GTEXT_CSS_Component_Value * values, size_t count, const GTEXT_Allocator * a, bool * valid) {
  *valid = true;
  /* "<declaration-value> ... does not contain <bad-string-token>, <bad-url-token>, unmatched <)-token>, <]-token>, or <}-token>, or top-level <semicolon-token>
     tokens or <delim-token> tokens with a value of "!"": the whole value is one, and so is each argument of a function that takes one (the fallback of var(),
     `var(--a,!)`, is not: the declaration holding it is invalid at parse time, and the one before it in the cascade stands). */
  for (size_t i = 0; i < count; i++) {
    const GTEXT_CSS_Component_Value * v = &values[i];
    if (v->kind == GTEXT_CSS_VALUE_TOKEN && (v->type == GTEXT_CSS_TOKEN_SEMICOLON || (v->type == GTEXT_CSS_TOKEN_DELIM && v->delim == '!'))) {
      *valid = false;
      return GTEXT_CSS_OK;
    }
  }
  struct frame {
    const GTEXT_CSS_Component_Value * v;
    size_t n;
  };
  size_t cap = 16, top = 0;
  struct frame * st = gtext_allocator_malloc(a, cap * sizeof *st);
  if (!st) {
    return GTEXT_CSS_E_OOM;
  }
  st[top++] = (struct frame){values, count};
  while (top > 0 && *valid) {
    struct frame f = st[--top];
    for (size_t i = 0; i < f.n && *valid; i++) {
      const GTEXT_CSS_Component_Value * v = &f.v[i];
      if (v->kind == GTEXT_CSS_VALUE_TOKEN && (v->type == GTEXT_CSS_TOKEN_BAD_STRING || v->type == GTEXT_CSS_TOKEN_BAD_URL || v->type == GTEXT_CSS_TOKEN_CLOSE_PAREN ||
                                              v->type == GTEXT_CSS_TOKEN_CLOSE_SQUARE || v->type == GTEXT_CSS_TOKEN_CLOSE_CURLY)) {
        /* a closing bracket that is a token of its own closes nothing: it is unmatched */
        *valid = false;
        break;
      }
      if (v->kind == GTEXT_CSS_VALUE_FUNCTION && (sub_name_is(v, "var") || sub_name_is(v, "env") || sub_name_is(v, "attr") || sub_name_is(v, "inherit"))) {
        bool first = false, comma = false, started = false, braced = false, first_block = false, first_empty = false;
        size_t first_count = 0;
        for (size_t k = 0; k < v->child_count; k++) {
          const GTEXT_CSS_Component_Value * c = &v->children[k];
          if (c->kind == GTEXT_CSS_VALUE_TOKEN) {
            if (c->type == GTEXT_CSS_TOKEN_SEMICOLON || (c->type == GTEXT_CSS_TOKEN_DELIM && c->delim == '!')) {
              *valid = false; /* a top-level `;` or `!` in an argument */
              break;
            }
            if (c->type == GTEXT_CSS_TOKEN_COMMA && !comma) {
              comma = true;
              continue;
            }
          }
          if (!comma && !(c->kind == GTEXT_CSS_VALUE_TOKEN && c->type == GTEXT_CSS_TOKEN_WHITESPACE)) {
            first = true;
            first_count++;
            if (c->kind == GTEXT_CSS_VALUE_BLOCK && c->type == GTEXT_CSS_TOKEN_OPEN_CURLY) {
              first_block = true;
              first_empty = true;
              for (size_t j = 0; j < c->child_count; j++) {
                if (!(c->children[j].kind == GTEXT_CSS_VALUE_TOKEN && c->children[j].type == GTEXT_CSS_TOKEN_WHITESPACE)) {
                  first_empty = false;
                }
              }
            }
          }
          if (comma && !(c->kind == GTEXT_CSS_VALUE_TOKEN && c->type == GTEXT_CSS_TOKEN_WHITESPACE)) {
            /* Values 5, "Commas in Function Arguments": a free-form production that starts with a "{" token "matches just the {} block that the "{" token opens", so the fallback, which is
               one, is the block alone: anything after it (`var(--a, {x} y)`, `var(--a, {x}, y)`) is not matched by the function's grammar, and the declaration is invalid at parse time. */
            if (braced) {
              *valid = false;
              break;
            }
            if (!started) {
              braced = c->kind == GTEXT_CSS_VALUE_BLOCK && c->type == GTEXT_CSS_TOKEN_OPEN_CURLY;
            }
            started = true;
          }
        }
        /* The first argument is a <declaration-value>, a free-form production that "does not match any top-level commas or {} blocks" unless it starts with a "{" token, and then "matches just the {} block":
           `var(--x {--y})` and `env({--x} --y)` are not valid, and `var({})` has nothing in its block (<declaration-value> is one or more tokens). What the argument names is not the syntax check's:
           env() reads "<custom-ident> <integer [0,∞]>*" when it is substituted (css-env-1: the name that "is a recognized environment variable name", otherwise the fallback), so `env(10px)` and
           `env(test 0.1, green)` are valid here and take their fallback there. */
        if (!first || (first_block && (first_count != 1 || first_empty))) {
          *valid = false;
        }
#if !GTEXT_CSS_READING_ENV_UNKNOWN_NAME_VALID
        if (sub_name_is(v, "env") && !env_first_argument_is_a_name(v)) {
          *valid = false;
        }
#endif
      }
      if (v->kind == GTEXT_CSS_VALUE_FUNCTION && v->value.length > 2 && v->value.data[0] == '-' && v->value.data[1] == '-' && !dashed_function_syntax(v)) {
        *valid = false;
        break;
      }
      if (v->kind == GTEXT_CSS_VALUE_FUNCTION && sub_name_is(v, "random-item") && !random_item_syntax(v)) {
        *valid = false;
        break;
      }
      if (v->child_count > 0) {
        if (top == cap) {
          cap *= 2;
          struct frame * g = gtext_allocator_realloc(a, st, cap * sizeof *st);
          if (!g) {
            gtext_allocator_free(a, st);
            return GTEXT_CSS_E_OOM;
          }
          st = g;
        }
        st[top++] = (struct frame){v->children, v->child_count};
      }
    }
  }
  gtext_allocator_free(a, st);
  return GTEXT_CSS_OK;
}
