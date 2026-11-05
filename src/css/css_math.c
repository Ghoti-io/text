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
 * Reading a math function from component values into a calculation tree
 * (Values 4, 10.10, "parse a calculation"), resolving a tree in an environment,
 * and the public entry points.
 *
 * The syntax is read with an explicit stack of frames, one for each open
 * function and parenthesis, and the operands of every frame live on one
 * shared stack, so nesting costs a frame and not a C call and the depth is a
 * limit (GTEXT_CSS_E_LIMIT_DEPTH). Each node is simplified by css_math_simplify.c
 * the moment its operands exist, which is the draft's "eager simplification".
 */

#include <ghoti.io/math/basic.h>

#include <float.h>
#include <stdint.h>
#include <string.h>

#include "css_math_internal.h"

#define DEFAULT_MAX_DEPTH ((size_t)256)
#define DEFAULT_MAX_NODES ((size_t)65536)
#define LIMIT_MAX_DEPTH ((size_t)65536)
#define LIMIT_MAX_NODES ((size_t)1 << 24)

#define PI_VALUE 3.14159265358979323846264338327950288
#define E_VALUE 2.71828182845904523536028747135266250

GTEXT_CSS_Math_Options gtext_css_math_options_default(void) {
  GTEXT_CSS_Math_Options o;
  memset(&o, 0, sizeof o);
  o.max_depth = DEFAULT_MAX_DEPTH;
  o.max_nodes = DEFAULT_MAX_NODES;
  return o;
}

/* ---- names ----------------------------------------------------------------- */

typedef struct {
  const char * name;
  GTEXT_CSS_Math_Op op;
} fn_name;

/// op GTEXT_CSS_MATH_OP_COUNT stands for calc() itself (a group).
static const fn_name FUNCTIONS[] = {
  {"calc", GTEXT_CSS_MATH_OP_COUNT}, {"min", GTEXT_CSS_MATH_MIN}, {"max", GTEXT_CSS_MATH_MAX},
  {"clamp", GTEXT_CSS_MATH_CLAMP}, {"round", GTEXT_CSS_MATH_ROUND}, {"mod", GTEXT_CSS_MATH_MOD},
  {"rem", GTEXT_CSS_MATH_REM}, {"sin", GTEXT_CSS_MATH_SIN}, {"cos", GTEXT_CSS_MATH_COS},
  {"tan", GTEXT_CSS_MATH_TAN}, {"asin", GTEXT_CSS_MATH_ASIN}, {"acos", GTEXT_CSS_MATH_ACOS},
  {"atan", GTEXT_CSS_MATH_ATAN}, {"atan2", GTEXT_CSS_MATH_ATAN2}, {"pow", GTEXT_CSS_MATH_POW},
  {"sqrt", GTEXT_CSS_MATH_SQRT}, {"hypot", GTEXT_CSS_MATH_HYPOT}, {"log", GTEXT_CSS_MATH_LOG},
  {"exp", GTEXT_CSS_MATH_EXP}, {"abs", GTEXT_CSS_MATH_ABS}, {"sign", GTEXT_CSS_MATH_SIGN}, {"progress", GTEXT_CSS_MATH_PROGRESS}, {"random", GTEXT_CSS_MATH_RANDOM},
};

/// Numeric functions that are not math functions but may stand in a calculation as a number (Values 5, 6.1): the tree-counting
/// functions, which take no arguments and cannot be resolved until an element is known. (The served draft dropped media-progress()
/// and container-progress() "in favor of using relevant units in progress()".)
static const char * const UNRESOLVED_NUMBER_FUNCTIONS[] = {
  "sibling-index", "sibling-count",
};

/// Whether a function has no arguments at all (white space aside): `sibling-index()` is the whole grammar of the tree-counting functions.
static bool no_arguments(const GTEXT_CSS_Component_Value * fn) {
  for (size_t i = 0; i < fn->child_count; i++) {
    const GTEXT_CSS_Component_Value * c = &fn->children[i];
    if (!(c->kind == GTEXT_CSS_VALUE_TOKEN && c->type == GTEXT_CSS_TOKEN_WHITESPACE)) {
      return false;
    }
  }
  return true;
}

static char fold1(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static bool name_is(const GTEXT_CSS_String * s, const char * lower) {
  const size_t n = strlen(lower);
  if (s->length != n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    if (fold1(s->data[i]) != lower[i]) {
      return false;
    }
  }
  return true;
}

static const fn_name * find_function(const GTEXT_CSS_String * name) {
  for (size_t i = 0; i < sizeof FUNCTIONS / sizeof FUNCTIONS[0]; i++) {
    if (name_is(name, FUNCTIONS[i].name)) {
      return &FUNCTIONS[i];
    }
  }
  return NULL;
}

bool gtext_css_math_is_function(const char * name, size_t length) {
  if (!name) {
    return false;
  }
  GTEXT_CSS_String s = {name, length};
  return find_function(&s) != NULL;
}


static bool is_unresolved_number_function(const GTEXT_CSS_String * name) {
  for (size_t i = 0; i < sizeof UNRESOLVED_NUMBER_FUNCTIONS / sizeof UNRESOLVED_NUMBER_FUNCTIONS[0]; i++) {
    if (name_is(name, UNRESOLVED_NUMBER_FUNCTIONS[i])) {
      return true;
    }
  }
  return false;
}

bool css_math_accepts_function(const char * name, size_t length) {
  if (!name) {
    return false;
  }
  GTEXT_CSS_String s = {name, length};
  return find_function(&s) != NULL || is_unresolved_number_function(&s);
}

static bool strategy_of(const GTEXT_CSS_String * s, GTEXT_CSS_Math_Round * out) {
  static const struct { const char * n; GTEXT_CSS_Math_Round r; } T[] = {
    {"nearest", GTEXT_CSS_MATH_ROUND_NEAREST}, {"up", GTEXT_CSS_MATH_ROUND_UP}, {"down", GTEXT_CSS_MATH_ROUND_DOWN},
    {"to-zero", GTEXT_CSS_MATH_ROUND_TO_ZERO}, {"line-width", GTEXT_CSS_MATH_ROUND_LINE_WIDTH},
  };
  for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
    if (name_is(s, T[i].n)) {
      *out = T[i].r;
      return true;
    }
  }
  return false;
}

const char * gtext_css_math_op_name(GTEXT_CSS_Math_Op op) {
  static const char * const names[] = {
    "number", "dimension", "percent", "sum", "product", "negate", "invert", "min", "max", "clamp", "round", "mod", "rem",
    "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "pow", "sqrt", "hypot", "log", "exp", "abs", "sign", "progress", "random", "symbol",
    "unresolved",
  };
  if ((unsigned)op >= (unsigned)GTEXT_CSS_MATH_OP_COUNT) {
    return "unknown";
  }
  return names[op];
}

/* ---- the frames ------------------------------------------------------------------ */

typedef struct {
  const GTEXT_CSS_Component_Value * items;
  size_t n, i;
  GTEXT_CSS_Math_Op op;       ///< The function; OP_COUNT for calc() and parentheses.
  GTEXT_CSS_Span span;
  size_t base;                ///< Start of this frame's region of the operand stack: the arguments.
  size_t terms_base;          ///< The terms of the argument being read.
  size_t factors_base;        ///< The factors of the term being read.
  size_t nargs;
  bool expect_value;
  bool neg_pending;           ///< The term being read follows a `-`.
  bool inv_pending;           ///< The factor about to be read follows a `/`.
  bool none_arg;              ///< The argument being read is the keyword none (clamp()).
  bool strategy_pending;      ///< round(): a strategy was read; a comma must follow.
  bool strategy_written;      ///< round(): the strategy was written.
  bool no_clamp;              ///< progress(): `no-clamp` was written.
  GTEXT_CSS_Math_Round round;
  bool any_value;             ///< A value was read in the argument being read.
  const char * key;           ///< random(): the key, as written (in the arena), or NULL.
  size_t key_len;
} pframe;

typedef struct {
  pframe * v;
  size_t n, cap;
} fstack;

static bool is_ws(const GTEXT_CSS_Component_Value * c) {
  return c->kind == GTEXT_CSS_VALUE_TOKEN && c->type == GTEXT_CSS_TOKEN_WHITESPACE;
}

static bool is_delim(const GTEXT_CSS_Component_Value * c, uint32_t ch) {
  return c->kind == GTEXT_CSS_VALUE_TOKEN && c->type == GTEXT_CSS_TOKEN_DELIM && c->delim == ch;
}

static bool push_operand(mbuilder * mb, const mnode * node) {
  if (!node) {
    return false;
  }
  if (mb->nstk == mb->capstk) {
    size_t cap = mb->capstk ? mb->capstk * 2 : 64;
    const mnode ** p = gtext_allocator_realloc(&mb->alloc, mb->stk, cap * sizeof *p);
    if (!p) {
      mb_fail(mb, GTEXT_CSS_E_OOM);
      return false;
    }
    mb->stk = p;
    mb->capstk = cap;
  }
  mb->stk[mb->nstk++] = node;
  return true;
}

/// Collapse the factors of the current term into one node and make it a term of the current argument.
static bool finish_term(mbuilder * mb, pframe * F) {
  const size_t nf = mb->nstk - F->factors_base;
  const mnode * term = mk_product(mb, mb->stk + F->factors_base, nf);
  mb->nstk = F->factors_base;
  if (!term) {
    return false;
  }
  if (F->neg_pending) {
    term = mk_negate(mb, term);
    F->neg_pending = false;
    if (!term) {
      return false;
    }
  }
  if (!push_operand(mb, term)) {
    return false;
  }
  F->factors_base = mb->nstk;
  return true;
}

/// Collapse the terms of the current argument and make it an argument of the frame.
static bool finish_argument(mbuilder * mb, pframe * F) {
  if (F->none_arg) {
    F->none_arg = false;
    /* a NULL operand stands for none: push through a node-free path */
    if (mb->nstk == mb->capstk) {
      const size_t cap = mb->capstk ? mb->capstk * 2 : 64;
      const mnode ** p = gtext_allocator_realloc(&mb->alloc, mb->stk, cap * sizeof *p);
      if (!p) {
        mb_fail(mb, GTEXT_CSS_E_OOM);
        return false;
      }
      mb->stk = p;
      mb->capstk = cap;
    }
    mb->stk[mb->nstk++] = NULL;
    F->nargs++;
    F->terms_base = F->factors_base = mb->nstk;
    F->any_value = false;
    return true;
  }
  if (!finish_term(mb, F)) {
    return false;
  }
  const size_t nt = mb->nstk - F->terms_base;
  const mnode * expr = mk_sum(mb, mb->stk + F->terms_base, nt);
  mb->nstk = F->terms_base;
  if (!expr || !push_operand(mb, expr)) {
    return false;
  }
  F->nargs++;
  F->terms_base = F->factors_base = mb->nstk;
  F->any_value = false;
  return true;
}

static bool read_random_key(mbuilder * mb, pframe * F);

static bool open_frame(fstack * fs, mbuilder * mb, const GTEXT_CSS_Component_Value * cv, GTEXT_CSS_Math_Op op) {
  if (fs->n >= mb->max_depth) {
    mb_fail(mb, GTEXT_CSS_E_LIMIT_DEPTH);
    mb->where = cv->span;
    return false;
  }
  if (fs->n == fs->cap) {
    const size_t cap = fs->cap ? fs->cap * 2 : 16;
    pframe * p = gtext_allocator_realloc(&mb->alloc, fs->v, cap * sizeof *p);
    if (!p) {
      mb_fail(mb, GTEXT_CSS_E_OOM);
      return false;
    }
    fs->v = p;
    fs->cap = cap;
  }
  pframe * F = &fs->v[fs->n++];
  memset(F, 0, sizeof *F);
  F->items = cv->children;
  F->n = cv->child_count;
  F->op = op;
  F->span = cv->span;
  F->base = F->terms_base = F->factors_base = mb->nstk;
  F->expect_value = true;
  F->round = GTEXT_CSS_MATH_ROUND_NEAREST;
  if (op == GTEXT_CSS_MATH_RANDOM) {
    return read_random_key(mb, F);
  }
  return true;
}

/* Whether an identifier is a <random-ua-ident>: Values 5, 9.2, "It is a <custom-ident> that must start with the prefix ua-, or else it is invalid." */
static bool is_ua_ident(const GTEXT_CSS_Component_Value * c) {
  return c->value.length >= 3 && c->value.data[0] == 'u' && c->value.data[1] == 'a' && c->value.data[2] == '-';
}

/* The key text of one identifier or keyword, written as the serializer writes a component value. */
static GTEXT_CSS_Status key_part_text(mbuilder * mb, const GTEXT_CSS_Component_Value * c, char ** buf, size_t * len) {
  GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
  so.allocator = &mb->alloc;
  return gtext_css_serialize_values_buffer(c, 1, &so, buf, len);
}

/* Read the <random-key> of a random() whose frame `F` was just opened: Values 5, 9.2, "<random-key> = auto | <random-cache-key> | fixed <number [0,1]>",
   "<random-cache-key> = <dashed-ident> || element-scoped || [ property-scoped | property-index-scoped | <random-ua-ident> ]". The key is written in that order
   (the browsers write `element-scoped ua-width-1` whatever order it came in), and when the calculation is read for a property (the context names it) it is simplified at
   parse time, 9.4.1: "If the <random-key> is auto (or omitted), it's turned into element-scoped property-index-scoped", and "If the <random-key> contains
   property-scoped or property-index-scoped, that keyword is replaced by a <random-ua-ident>: either ua-PROPERTY with PROPERTY being the property the value was parsed
   as, or ua-PROPERTY-INDEX with PROPERTY as the previous and INDEX being the 1-indexed integer index of this random function among all random functions being used in
   the declaration." */
static bool read_random_key(mbuilder * mb, pframe * F) {
  const GTEXT_CSS_Math_Context * ctx = mb->ctx;
  const bool simplify = ctx && ctx->random_property && ctx->random_property_length && ctx->random_index;
  /* every random() takes the next index, whatever its key */
  uint32_t index = 0;
  if (simplify) {
    if (*ctx->random_index < UINT32_MAX) {
      (*ctx->random_index)++;
    }
    index = *ctx->random_index;
  }
  /* a random key comes before the first comma and is not a calculation */
  size_t comma = 0;
  while (comma < F->n && !(F->items[comma].kind == GTEXT_CSS_VALUE_TOKEN && F->items[comma].type == GTEXT_CSS_TOKEN_COMMA)) {
    comma++;
  }
  bool key = comma < F->n;
  size_t parts = 0, dashed = 0, element = 0, property = 0, fixed = 0, autos = 0, numbers = 0, fixed_at = 0, number_at = 0;
  const GTEXT_CSS_Component_Value * dashed_cv = NULL;
  const GTEXT_CSS_Component_Value * third_cv = NULL; /* property-scoped, property-index-scoped or a <random-ua-ident> */
  bool third_index = false;                          /* property-index-scoped */
  bool third_property = false;                       /* property-scoped */
  double fixed_value = 0.0;
  char * fixed_canon = NULL;
  size_t fixed_canon_len = 0;
  GTEXT_CSS_Serialize_Options fixed_so = gtext_css_serialize_options_default();
  fixed_so.allocator = &mb->alloc;
  for (size_t i = 0; key && i < comma; i++) {
    const GTEXT_CSS_Component_Value * c = &F->items[i];
    if (is_ws(c)) {
      continue;
    }
    parts++;
    if (c->kind == GTEXT_CSS_VALUE_TOKEN && c->type == GTEXT_CSS_TOKEN_IDENT) {
      if (name_is(&c->value, "fixed")) {
        fixed++;
        fixed_at = parts;
      }
      else if (name_is(&c->value, "auto")) {
        autos++;
      }
      else if (name_is(&c->value, "element-scoped")) {
        element++;
      }
      else if (name_is(&c->value, "property-scoped") || name_is(&c->value, "property-index-scoped")) {
        property++;
        third_cv = c;
        third_property = name_is(&c->value, "property-scoped");
        third_index = !third_property;
      }
      else if (c->value.length >= 2 && c->value.data[0] == '-' && c->value.data[1] == '-') {
        dashed++;
        dashed_cv = c;
      }
      else if (is_ua_ident(c)) {
        property++;
        third_cv = c;
        third_property = third_index = false;
      }
      else {
        key = false;
      }
    }
    else if (c->kind == GTEXT_CSS_VALUE_TOKEN && c->type == GTEXT_CSS_TOKEN_NUMBER) {
      numbers++;
      number_at = parts;
      fixed_value = c->number;
    }
    else if (c->kind == GTEXT_CSS_VALUE_FUNCTION && fixed == 1 && parts == 2 && gtext_css_math_is_function(c->value.data, c->value.length)) {
      /* Values 5, 9.2: `fixed <number [0,1]>`, and Values 4 lets a math function stand where a <number> is allowed (`fixed calc(0.5)`) */
      bool oom = false;
      if (css_random_key_number(c, &mb->alloc, &fixed_value, &oom, &fixed_canon, &fixed_canon_len)) {
        numbers++;
        number_at = parts;
      }
      else if (oom) {
        mb_fail(mb, GTEXT_CSS_E_OOM);
        return false;
      }
      else {
        key = false;
      }
    }
    else {
      key = false;
    }
  }
  if (key) {
    if (fixed) {
      key = fixed == 1 && numbers == 1 && parts == 2 && fixed_at == 1 && number_at == 2 && fixed_value >= 0.0 && fixed_value <= 1.0;
    }
    else if (autos) {
      key = autos == 1 && parts == 1;
    }
    else {
      key = numbers == 0 && dashed <= 1 && element <= 1 && property <= 1;
    }
  }
  if (fixed_canon && !(key && parts > 0)) {
    gtext_css_serialize_buffer_free(&fixed_so, fixed_canon);
    fixed_canon = NULL;
  }
  /* the pieces of the key, in the order of the grammar: the name, element-scoped, the third; a key that is omitted or auto is element-scoped property-index-scoped */
  const bool have_key = key && parts > 0;
  const bool defaulted = simplify && (!have_key || (autos && !fixed));
  if (!have_key && !defaulted) {
    return true; /* the first argument is no key, and nothing is simplified: the arguments start at the first item */
  }
  if (defaulted) {
    element = 1;
    third_index = true;
    third_property = false;
    third_cv = NULL;
    dashed_cv = NULL;
  }
  char * pieces[3] = {NULL, NULL, NULL};
  size_t plen[3] = {0, 0, 0};
  const GTEXT_Allocator * al = &mb->alloc;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  size_t total = 0;
  size_t npieces = 0;
  if (fixed_canon) {
    /* `fixed calc(2 / 4)` is written `fixed calc(0.5)`: the calculation as a specified value is serialized (Values 4, 10.13) */
    pieces[0] = (char *)gtext_allocator_malloc(al, 6 + fixed_canon_len + 1);
    if (!pieces[0]) {
      st = GTEXT_CSS_E_OOM;
    }
    else {
      memcpy(pieces[0], "fixed ", 6);
      memcpy(pieces[0] + 6, fixed_canon, fixed_canon_len);
      plen[0] = 6 + fixed_canon_len;
      npieces = 1;
    }
    gtext_css_serialize_buffer_free(&fixed_so, fixed_canon);
    fixed_canon = NULL;
  }
  else if (fixed) {
    /* `fixed 0.5`: the tokens as the serializer writes them */
    GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
    so.allocator = al;
    st = gtext_css_serialize_values_buffer(F->items, comma, &so, &pieces[0], &plen[0]);
    npieces = 1;
  }
  else if (autos && !defaulted) {
    /* `auto` is written as it is when nothing is simplified (a calculation read without the property it is a value of) */
    pieces[0] = (char *)gtext_allocator_malloc(al, sizeof "auto");
    if (!pieces[0]) {
      st = GTEXT_CSS_E_OOM;
    }
    else {
      memcpy(pieces[0], "auto", sizeof "auto");
      plen[0] = sizeof "auto" - 1;
      npieces = 1;
    }
  }
  else {
    if (dashed_cv && st == GTEXT_CSS_OK) {
      st = key_part_text(mb, dashed_cv, &pieces[npieces], &plen[npieces]);
      npieces++;
    }
    if (element && st == GTEXT_CSS_OK) {
      pieces[npieces] = (char *)gtext_allocator_malloc(al, sizeof "element-scoped");
      if (!pieces[npieces]) {
        st = GTEXT_CSS_E_OOM;
      }
      else {
        memcpy(pieces[npieces], "element-scoped", sizeof "element-scoped");
        plen[npieces] = sizeof "element-scoped" - 1;
        npieces++;
      }
    }
    if ((third_cv || third_index || third_property) && st == GTEXT_CSS_OK) {
      if (simplify && (third_index || third_property)) {
        /* ua-PROPERTY or ua-PROPERTY-INDEX, the property being the one the value is parsed as */
        const size_t pl = ctx->random_property_length;
        char * name = (char *)gtext_allocator_malloc(al, pl + 32);
        if (!name) {
          st = GTEXT_CSS_E_OOM;
        }
        else {
          size_t n = 0;
          memcpy(name, "ua-", 3);
          n = 3;
          for (size_t k = 0; k < pl; k++) {
            const char ch = ctx->random_property[k];
            name[n++] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + ('a' - 'A')) : ch;
          }
          if (third_index) {
            name[n++] = '-';
            char digits[16];
            size_t d = 0;
            for (uint32_t v = index; v; v /= 10) {
              digits[d++] = (char)('0' + v % 10);
            }
            while (d) {
              name[n++] = digits[--d];
            }
          }
          GTEXT_CSS_Component_Value synth;
          memset(&synth, 0, sizeof synth);
          synth.kind = GTEXT_CSS_VALUE_TOKEN;
          synth.type = GTEXT_CSS_TOKEN_IDENT;
          synth.value.data = name;
          synth.value.length = n;
          st = key_part_text(mb, &synth, &pieces[npieces], &plen[npieces]);
          gtext_allocator_free(al, name);
          npieces++;
        }
      }
      else if (third_cv && (third_property || third_index)) {
        /* nothing to simplify with: the keyword, in lower case */
        const char * kw = third_index ? "property-index-scoped" : "property-scoped";
        const size_t kl = strlen(kw);
        pieces[npieces] = (char *)gtext_allocator_malloc(al, kl + 1);
        if (!pieces[npieces]) {
          st = GTEXT_CSS_E_OOM;
        }
        else {
          memcpy(pieces[npieces], kw, kl + 1);
          plen[npieces] = kl;
          npieces++;
        }
      }
      else if (third_cv) {
        st = key_part_text(mb, third_cv, &pieces[npieces], &plen[npieces]);
        npieces++;
      }
      else {
        /* only reached when defaulted without a property: cannot happen (defaulted needs simplify) */
        st = GTEXT_CSS_E_INTERNAL;
      }
    }
  }
  char * joined = NULL;
  if (st == GTEXT_CSS_OK) {
    size_t begin[3] = {0, 0, 0}, end[3] = {0, 0, 0};
    for (size_t k = 0; k < npieces; k++) {
      size_t b = 0, e = plen[k];
      while (b < e && pieces[k][b] == ' ') {
        b++;
      }
      while (e > b && pieces[k][e - 1] == ' ') {
        e--;
      }
      begin[k] = b;
      end[k] = e;
      total += (e - b) + (k ? 1 : 0);
    }
    joined = css_arena_alloc(mb->arena, total + 1);
    if (!joined) {
      st = GTEXT_CSS_E_OOM;
    }
    else {
      size_t at = 0;
      for (size_t k = 0; k < npieces; k++) {
        if (k) {
          joined[at++] = ' ';
        }
        memcpy(joined + at, pieces[k] + begin[k], end[k] - begin[k]);
        at += end[k] - begin[k];
      }
      joined[at] = '\0';
    }
  }
  for (size_t k = 0; k < 3; k++) {
    if (pieces[k]) {
      /* the serializer's buffers and the ones made here are both the scratch allocator's */
      gtext_allocator_free(al, pieces[k]);
    }
  }
  if (st != GTEXT_CSS_OK) {
    mb_fail(mb, st);
    return false;
  }
  F->key = joined;
  F->key_len = total;
  if (have_key) {
    F->i = comma + 1;
  }
  return true;
}

bool css_random_key_number(const GTEXT_CSS_Component_Value * c, const GTEXT_Allocator * allocator, double * out, bool * oom, char ** canon, size_t * canon_length) {
  if (c->kind == GTEXT_CSS_VALUE_TOKEN) {
    if (c->type != GTEXT_CSS_TOKEN_NUMBER) {
      return false;
    }
    *out = c->number;
    return true;
  }
  if (c->kind != GTEXT_CSS_VALUE_FUNCTION || !gtext_css_math_is_function(c->value.data, c->value.length)) {
    return false;
  }
  GTEXT_CSS_Math_Options mo = gtext_css_math_options_default();
  mo.allocator = allocator;
  bool valid = false;
  GTEXT_CSS_Math * m = NULL;
  const GTEXT_CSS_Status st = gtext_css_math_parse(c, NULL, &mo, &valid, &m, NULL);
  if (st == GTEXT_CSS_E_OOM) {
    *oom = true;
    return false;
  }
  bool ok = false;
  if (st == GTEXT_CSS_OK && valid && m) {
    GTEXT_CSS_Unit unit = GTEXT_CSS_UNIT_NONE;
    bool percent = false;
    double v = 0.0;
    if (gtext_css_math_numeric(m, &v, &unit, &percent) && unit == GTEXT_CSS_UNIT_NONE && !percent && v == v) {
      *out = v;
      ok = true;
      if (canon) {
        GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
        so.allocator = allocator;
        if (gtext_css_math_serialize_buffer(m, &so, canon, canon_length) != GTEXT_CSS_OK) {
          *oom = true;
          ok = false;
        }
      }
    }
  }
  gtext_css_math_free(m);
  return ok;
}

static const GTEXT_CSS_Math_Keyword * find_keyword(const GTEXT_CSS_Math_Context * ctx, const GTEXT_CSS_String * name) {
  if (!ctx) {
    return NULL;
  }
  for (size_t i = 0; i < ctx->keyword_count; i++) {
    if (name_is(name, ctx->keywords[i].name)) {
      return &ctx->keywords[i];
    }
  }
  return NULL;
}

static double make_inf(void) {
  volatile double z = 0.0;
  return 1.0 / z;
}

static double make_nan(void) {
  volatile double z = 0.0;
  return z / z;
}

/// Serialize one component value (an unresolved function) to text in the arena.
static const mnode * unresolved_leaf(mbuilder * mb, const GTEXT_CSS_Component_Value * cv) {
  /* the tree-counting functions take no arguments (no_arguments()): their text is the name in lower case and `()`, whatever white space
     the source had inside the parentheses */
  if (cv->kind == GTEXT_CSS_VALUE_FUNCTION && no_arguments(cv) && cv->value.length < 32) {
    char text[40];
    for (size_t i = 0; i < cv->value.length; i++) {
      text[i] = fold1(cv->value.data[i]);
    }
    text[cv->value.length] = '(';
    text[cv->value.length + 1] = ')';
    GTEXT_CSS_Math_Type zero;
    memset(&zero, 0, sizeof zero);
    return mk_unresolved(mb, text, cv->value.length + 2, &zero);
  }
  GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
  so.allocator = &mb->alloc;
  char * buf = NULL;
  size_t len = 0;
  const GTEXT_CSS_Status st = gtext_css_serialize_values_buffer(cv, 1, &so, &buf, &len);
  if (st != GTEXT_CSS_OK) {
    mb_fail(mb, st);
    return NULL;
  }
  GTEXT_CSS_Math_Type zero;
  memset(&zero, 0, sizeof zero);
  const mnode * n = mk_unresolved(mb, buf, len, &zero);
  gtext_css_serialize_buffer_free(&so, buf);
  return n;
}

/* ---- anchor() and anchor-size() in a calculation --------------------------------------------------------------------- */

/// Anchor Positioning 1: "anchor() = anchor( <anchor-name>? && <anchor-side>, <length-percentage>? )" and "anchor-size() = anchor-size( [ <anchor-name> ||
/// <anchor-size> ]? , <length-percentage>? )"; both "resolve to a <length>" and are valid wherever a <length> is, so in a calculation each is a leaf of the type
/// length whose value is not known until the anchor is. Its text is written in the canonical form: the name, then the side or size, then the fallback as a
/// <length-percentage> is serialized (`0` as `0px`, a calculation as one).
static const char * const ANCHOR_SIDES[] = {"inside", "outside", "top", "left", "right", "bottom", "start", "end", "self-start", "self-end", "center"};
static const char * const ANCHOR_SIZES[] = {"width", "height", "block", "inline", "self-block", "self-inline"};
#define ANCHOR_NEST_BUDGET 64

static bool anchor_word(const GTEXT_CSS_String * s, const char * const * list, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (name_is(s, list[i])) {
      return true;
    }
  }
  return false;
}

typedef struct {
  char * d;
  size_t n, cap;
} atext;

static bool atext_put(mbuilder * mb, atext * t, const char * s, size_t n) {
  if (t->n + n + 1 > t->cap) {
    size_t cap = t->cap ? t->cap : 64;
    while (cap < t->n + n + 1) {
      cap *= 2;
    }
    char * p = gtext_allocator_realloc(&mb->alloc, t->d, cap);
    if (!p) {
      mb_fail(mb, GTEXT_CSS_E_OOM);
      return false;
    }
    t->d = p;
    t->cap = cap;
  }
  memcpy(t->d + t->n, s, n);
  t->n += n;
  t->d[t->n] = '\0';
  return true;
}

/// A value of the position slot of an anchor function (a <percentage>, or a <length-percentage> for the fallback) written as the specified value is: the token
/// as it serializes, `0` as `0px`, a calculation (whose nesting is bounded) as the math layer serializes it. Returns whether it was a value of the kind.
static bool anchor_operand(mbuilder * mb, const GTEXT_CSS_Math_Context * ctx, const GTEXT_CSS_Component_Value * v, bool percentage_only, atext * out) {
  if (v->kind == GTEXT_CSS_VALUE_TOKEN) {
    if (v->type == GTEXT_CSS_TOKEN_PERCENTAGE || (!percentage_only && v->type == GTEXT_CSS_TOKEN_DIMENSION) ||
        (!percentage_only && v->type == GTEXT_CSS_TOKEN_NUMBER && v->number == 0)) {
      if (v->type == GTEXT_CSS_TOKEN_NUMBER) {
        return atext_put(mb, out, "0px", 3);
      }
      if (v->type == GTEXT_CSS_TOKEN_DIMENSION) {
        const GTEXT_CSS_Unit u = gtext_css_unit_lookup(v->unit.data, v->unit.length);
        if (gtext_css_unit_type(u) != GTEXT_CSS_UNIT_TYPE_LENGTH) {
          mb_invalid(mb);
          return false;
        }
      }
      GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
      so.allocator = &mb->alloc;
      char * buf = NULL;
      size_t len = 0;
      const GTEXT_CSS_Status st = gtext_css_serialize_values_buffer(v, 1, &so, &buf, &len);
      if (st != GTEXT_CSS_OK) {
        mb_fail(mb, st);
        return false;
      }
      const bool ok = atext_put(mb, out, buf, len);
      gtext_css_serialize_buffer_free(&so, buf);
      return ok;
    }
    mb_invalid(mb);
    return false;
  }
  if (v->kind == GTEXT_CSS_VALUE_FUNCTION && find_function(&v->value)) {
    GTEXT_CSS_Math_Options mo = gtext_css_math_options_default();
    mo.allocator = &mb->alloc;
    /* the nesting of a calculation inside an anchor function inside a calculation is a C call, so it has a budget of its own that each level takes one from */
    const size_t budget = mb->max_depth > ANCHOR_NEST_BUDGET ? ANCHOR_NEST_BUDGET : mb->max_depth;
    if (budget <= 2) {
      mb->status = GTEXT_CSS_E_LIMIT_DEPTH;
      mb->where = v->span;
      return false;
    }
    mo.max_depth = budget - 1;
    bool valid = false;
    GTEXT_CSS_Math * m = NULL;
    GTEXT_CSS_Math_Result r;
    const GTEXT_CSS_Status st = gtext_css_math_parse(v, ctx, &mo, &valid, &m, &r);
    if (st != GTEXT_CSS_OK) {
      mb->status = st;
      mb->where = r.where;
      return false;
    }
    if (!valid) {
      mb_invalid(mb);
      return false;
    }
    const GTEXT_CSS_Math_Category cat = m->category;
    bool ok = percentage_only ? cat == GTEXT_CSS_MATH_CAT_PERCENTAGE : (cat == GTEXT_CSS_MATH_CAT_LENGTH || cat == GTEXT_CSS_MATH_CAT_PERCENTAGE);
    if (ok) {
      char * buf = NULL;
      size_t len = 0;
      const GTEXT_CSS_Status ss = gtext_css_math_serialize_buffer(m, NULL, &buf, &len);
      if (ss != GTEXT_CSS_OK) {
        mb_fail(mb, ss);
        ok = false;
      }
      else {
        ok = atext_put(mb, out, buf, len);
        gtext_css_serialize_buffer_free(NULL, buf);
      }
    }
    else {
      mb_invalid(mb);
    }
    gtext_css_math_free(m);
    return ok;
  }
  mb_invalid(mb);
  return false;
}

static const mnode * anchor_leaf(mbuilder * mb, const GTEXT_CSS_Math_Context * ctx, const GTEXT_CSS_Component_Value * fn) {
  const bool is_size = name_is(&fn->value, "anchor-size");
  const GTEXT_CSS_Component_Value * name = NULL;
  const GTEXT_CSS_Component_Value * word = NULL;       /* the side or the size */
  const GTEXT_CSS_Component_Value * position = NULL;   /* a percentage side, or a calculation of one */
  const GTEXT_CSS_Component_Value * fallback = NULL;
  bool comma = false;
  size_t before = 0;
  for (size_t i = 0; i < fn->child_count; i++) {
    const GTEXT_CSS_Component_Value * c = &fn->children[i];
    mb->work++;
    if (c->kind == GTEXT_CSS_VALUE_TOKEN && c->type == GTEXT_CSS_TOKEN_WHITESPACE) {
      continue;
    }
    if (c->kind == GTEXT_CSS_VALUE_TOKEN && c->type == GTEXT_CSS_TOKEN_COMMA) {
      if (comma) {
        mb_invalid(mb);
        return NULL;
      }
      comma = true;
      continue;
    }
    if (comma) {
      if (fallback) {
        mb_invalid(mb);
        return NULL;
      }
      fallback = c;
      continue;
    }
    before++;
    if (c->kind == GTEXT_CSS_VALUE_TOKEN && c->type == GTEXT_CSS_TOKEN_IDENT && c->value.length > 2 && c->value.data[0] == '-' && c->value.data[1] == '-') {
      if (name) {
        mb_invalid(mb);
        return NULL;
      }
      name = c;
    }
    else if (c->kind == GTEXT_CSS_VALUE_TOKEN && c->type == GTEXT_CSS_TOKEN_IDENT &&
             (is_size ? anchor_word(&c->value, ANCHOR_SIZES, sizeof ANCHOR_SIZES / sizeof *ANCHOR_SIZES)
                      : anchor_word(&c->value, ANCHOR_SIDES, sizeof ANCHOR_SIDES / sizeof *ANCHOR_SIDES))) {
      if (word || position) {
        mb_invalid(mb);
        return NULL;
      }
      word = c;
    }
    else if (!is_size && ((c->kind == GTEXT_CSS_VALUE_TOKEN && c->type == GTEXT_CSS_TOKEN_PERCENTAGE) || (c->kind == GTEXT_CSS_VALUE_FUNCTION && find_function(&c->value)))) {
      if (word || position) {
        mb_invalid(mb);
        return NULL;
      }
      position = c;
    }
    else if (is_size && !comma && before == 1 && !name && !word) {
      /* `anchor-size(10px)`: with no comma, a lone <length-percentage> is the fallback (the engine's grammar takes it so) */
      fallback = c;
      before = 0;
    }
    else {
      mb_invalid(mb);
      return NULL;
    }
  }
  if ((!is_size && !word && !position) || (comma && !fallback)) {
    mb_invalid(mb);
    return NULL;
  }
  atext t = {NULL, 0, 0};
  const char * fname = is_size ? "anchor-size(" : "anchor(";
  bool ok = atext_put(mb, &t, fname, strlen(fname));
  bool first = true;
  if (ok && name) {
    ok = atext_put(mb, &t, name->value.data, name->value.length);
    first = false;
  }
  if (ok && word) {
    ok = (first || atext_put(mb, &t, " ", 1)) && atext_put(mb, &t, word->value.data, word->value.length);
    for (size_t i = t.n - word->value.length; ok && i < t.n; i++) {
      t.d[i] = fold1(t.d[i]);
    }
    first = false;
  }
  if (ok && position) {
    ok = (first || atext_put(mb, &t, " ", 1)) && anchor_operand(mb, ctx, position, true, &t);
    first = false;
  }
  if (ok && fallback) {
    ok = (first || atext_put(mb, &t, ", ", 2)) && anchor_operand(mb, ctx, fallback, false, &t);
  }
  ok = ok && atext_put(mb, &t, ")", 1);
  const mnode * leaf = NULL;
  if (ok) {
    GTEXT_CSS_Math_Type length = gtext_css_math_category_type(GTEXT_CSS_MATH_CAT_LENGTH);
    leaf = mk_unresolved(mb, t.d, t.n, &length);
  }
  gtext_allocator_free(&mb->alloc, t.d);
  return leaf;
}

/// A value in an expression: a leaf, or the start of a nested frame (reported by *opened).
static bool read_value(fstack * fs, mbuilder * mb, const GTEXT_CSS_Math_Context * ctx, const GTEXT_CSS_Component_Value * cv,
    bool * opened, bool * has_symbols) {
  *opened = false;
  pframe * F = &fs->v[fs->n - 1];
  const mnode * leaf = NULL;
  mb->work++;
  if (cv->kind == GTEXT_CSS_VALUE_TOKEN) {
    switch (cv->type) {
    case GTEXT_CSS_TOKEN_NUMBER:
      leaf = mk_number(mb, cv->number);
      break;
    case GTEXT_CSS_TOKEN_PERCENTAGE:
      leaf = mk_percent(mb, cv->number);
      mb->saw_percent = true;
      break;
    case GTEXT_CSS_TOKEN_DIMENSION: {
      const GTEXT_CSS_Unit u = gtext_css_unit_lookup(cv->unit.data, cv->unit.length);
      /* the units of CSS Speech 1 (dB, st) are not Values units: no sentence of Values makes them part of a calculation */
      if (u == GTEXT_CSS_UNIT_NONE || gtext_css_unit_type(u) == GTEXT_CSS_UNIT_TYPE_DECIBEL || gtext_css_unit_type(u) == GTEXT_CSS_UNIT_TYPE_SEMITONES) {
        mb_invalid(mb);
        return false;
      }
      leaf = mk_dimension(mb, cv->number, u);
      break;
    }
    case GTEXT_CSS_TOKEN_IDENT: {
      const GTEXT_CSS_String * s = &cv->value;
      if (F->op == GTEXT_CSS_MATH_CLAMP && !F->any_value && name_is(s, "none")) {
        F->none_arg = true;
        F->any_value = true;
        F->expect_value = false;
        return true;
      }
      if (F->op == GTEXT_CSS_MATH_ROUND && F->nargs == 0 && !F->any_value && !F->strategy_written) {
        GTEXT_CSS_Math_Round r;
        if (strategy_of(s, &r)) {
          F->round = r;
          F->strategy_pending = true;
          F->any_value = true;
          F->expect_value = false;
          return true;
        }
      }
      if (F->op == GTEXT_CSS_MATH_PROGRESS && F->nargs == 0 && !F->any_value && !F->no_clamp && name_is(s, "no-clamp")) {
        F->no_clamp = true;
        return true;
      }
      if (name_is(s, "e")) {
        leaf = mk_number(mb, E_VALUE);
      }
      else if (name_is(s, "pi")) {
        leaf = mk_number(mb, PI_VALUE);
      }
      else if (name_is(s, "infinity")) {
        leaf = mk_number(mb, make_inf());
      }
      else if (name_is(s, "-infinity")) {
        leaf = mk_number(mb, -make_inf());
      }
      else if (name_is(s, "nan")) {
        leaf = mk_number(mb, make_nan());
      }
      else {
        const GTEXT_CSS_Math_Keyword * k = find_keyword(ctx, s);
        if (!k) {
          mb_invalid(mb);
          return false;
        }
        leaf = mk_symbol(mb, k->name, strlen(k->name), &k->type);
        *has_symbols = true;
      }
      break;
    }
    default:
      mb_invalid(mb);
      return false;
    }
  }
  else if (cv->kind == GTEXT_CSS_VALUE_BLOCK) {
    if (cv->type != GTEXT_CSS_TOKEN_OPEN_PAREN) {
      mb_invalid(mb);
      return false;
    }
    if (!open_frame(fs, mb, cv, GTEXT_CSS_MATH_OP_COUNT)) {
      return false;
    }
    *opened = true;
    return true;
  }
  else {
    const fn_name * fn = find_function(&cv->value);
    if (fn) {
      if (!open_frame(fs, mb, cv, fn->op)) {
        return false;
      }
      *opened = true;
      return true;
    }
    if (is_unresolved_number_function(&cv->value) && no_arguments(cv)) {
      leaf = unresolved_leaf(mb, cv);
      *has_symbols = true;
    }
    else if (ctx && ((name_is(&cv->value, "anchor") && (ctx->anchor_functions & GTEXT_CSS_MATH_ANCHOR)) ||
                     (name_is(&cv->value, "anchor-size") && (ctx->anchor_functions & GTEXT_CSS_MATH_ANCHOR_SIZE)))) {
      leaf = anchor_leaf(mb, ctx, cv);
      *has_symbols = true;
    }
    else {
      mb_invalid(mb);
      return false;
    }
  }
  if (!leaf) {
    return false;
  }
  if (F->inv_pending) {
    leaf = mk_invert(mb, leaf);
    F->inv_pending = false;
    if (!leaf) {
      return false;
    }
  }
  if (!push_operand(mb, leaf)) {
    return false;
  }
  F->any_value = true;
  F->expect_value = false;
  return true;
}

/// Deliver a finished frame's node to its parent as a factor.
static bool deliver(mbuilder * mb, pframe * P, const mnode * node) {
  if (!node) {
    return false;
  }
  if (P->inv_pending) {
    node = mk_invert(mb, node);
    P->inv_pending = false;
    if (!node) {
      return false;
    }
  }
  if (!push_operand(mb, node)) {
    return false;
  }
  P->any_value = true;
  P->expect_value = false;
  return true;
}

/// Finish the top frame: its function over its arguments.
static const mnode * close_frame(mbuilder * mb, pframe * F) {
  /* a frame that stops where a value is due (`1 +`, `min(1,)`, `()`), or after a strategy, is not valid */
  if (F->strategy_pending || F->expect_value) {
    mb_invalid(mb);
    return NULL;
  }
  if (!finish_argument(mb, F)) {
    return NULL;
  }
  const mnode * const * args = mb->stk + F->base;
  const mnode * r;
  if (F->op == GTEXT_CSS_MATH_OP_COUNT) {
    if (F->nargs != 1 || !args[0]) {
      mb_invalid(mb);
      return NULL;
    }
    r = args[0];
  }
  else {
    const mfn_opts opts = {F->round, F->strategy_written, F->no_clamp, F->op == GTEXT_CSS_MATH_RANDOM ? F->key : NULL, F->key_len};
    r = mk_fn(mb, F->op, args, F->nargs, &opts);
  }
  mb->nstk = F->base;
  return r;
}

/// Read the function `fn`. On OK, `*root` is the tree or NULL with mb->invalid set.
static const mnode * read_calculation(mbuilder * mb, const GTEXT_CSS_Component_Value * fn, const GTEXT_CSS_Math_Context * ctx,
    bool * has_symbols) {
  const fn_name * top = find_function(&fn->value);
  if (fn->kind == GTEXT_CSS_VALUE_FUNCTION && !top && is_unresolved_number_function(&fn->value)) {
    if (!no_arguments(fn)) {
      mb_invalid(mb);
      return NULL;
    }
    /* sibling-index() and the others stand alone as a number that is not known yet */
    *has_symbols = true;
    return unresolved_leaf(mb, fn);
  }
  if (fn->kind != GTEXT_CSS_VALUE_FUNCTION || !top) {
    mb_invalid(mb);
    return NULL;
  }
  fstack fs = {NULL, 0, 0};
  const mnode * root = NULL;
  if (!open_frame(&fs, mb, fn, top->op)) {
    goto done;
  }
  for (;;) {
    pframe * F = &fs.v[fs.n - 1];
    if (mb->status != GTEXT_CSS_OK) {
      goto done;
    }
    /* skip white space */
    bool ws_before = false;
    while (F->i < F->n && is_ws(&F->items[F->i])) {
      F->i++;
      ws_before = true;
      mb->work++;
    }
    if (F->i >= F->n) {
      const mnode * r = close_frame(mb, F);
      if (!r) {
        goto done;
      }
      fs.n--;
      if (fs.n == 0) {
        root = r;
        goto done;
      }
      if (!deliver(mb, &fs.v[fs.n - 1], r)) {
        goto done;
      }
      continue;
    }
    const GTEXT_CSS_Component_Value * cv = &F->items[F->i++];
    if (F->expect_value) {
      bool opened;
      if (!read_value(&fs, mb, ctx, cv, &opened, has_symbols)) {
        goto done;
      }
      continue;
    }
    /* an operator or a comma */
    if (F->none_arg && !(cv->kind == GTEXT_CSS_VALUE_TOKEN && cv->type == GTEXT_CSS_TOKEN_COMMA)) {
      mb_invalid(mb);
      goto done;
    }
    if (cv->kind != GTEXT_CSS_VALUE_TOKEN) {
      mb_invalid(mb);
      goto done;
    }
    if (cv->type == GTEXT_CSS_TOKEN_COMMA) {
      if (F->op == GTEXT_CSS_MATH_OP_COUNT) {
        mb_invalid(mb);
        goto done;
      }
      if (F->strategy_pending) {
        F->strategy_pending = false;
        F->strategy_written = true;
        F->any_value = false;
        F->expect_value = true;
        continue;
      }
      if (!finish_argument(mb, F)) {
        goto done;
      }
      F->expect_value = true;
      continue;
    }
    if (F->strategy_pending) {
      mb_invalid(mb);
      goto done;
    }
    if (is_delim(cv, '*') || is_delim(cv, '/')) {
      F->inv_pending = is_delim(cv, '/');
      F->expect_value = true;
      continue;
    }
    if (is_delim(cv, '+') || is_delim(cv, '-')) {
      /* white space is required on both sides of + and - */
      if (!ws_before || F->i >= F->n || !is_ws(&F->items[F->i])) {
        mb_invalid(mb);
        goto done;
      }
      if (!finish_term(mb, F)) {
        goto done;
      }
      F->neg_pending = is_delim(cv, '-');
      F->expect_value = true;
      continue;
    }
    mb_invalid(mb);
    goto done;
  }
done:
  gtext_allocator_free(&mb->alloc, fs.v);
  gtext_allocator_free(&mb->alloc, mb->stk);
  mb->stk = NULL;
  mb->nstk = mb->capstk = 0;
  return mb->status == GTEXT_CSS_OK && !mb->invalid ? root : NULL;
}

/* ---- the calculation ------------------------------------------------------------- */

static void result_fill(GTEXT_CSS_Math_Result * r, GTEXT_CSS_Status st, const mbuilder * mb) {
  if (!r) {
    return;
  }
  memset(r, 0, sizeof *r);
  r->status = st;
  r->message = gtext_css_status_string(st);
  r->where = mb->where;
  r->nodes = mb->nodes;
  r->work = mb->work;
}

GTEXT_CSS_Status css_math_build(css_arena * arena, const GTEXT_Allocator * scratch, const GTEXT_CSS_Component_Value * function,
    const GTEXT_CSS_Math_Context * ctx, size_t max_depth, size_t max_nodes, GTEXT_CSS_Math ** out, bool * valid,
    GTEXT_CSS_Math_Result * result) {
  mbuilder mb;
  memset(&mb, 0, sizeof mb);
  mb.arena = arena;
  mb.alloc = *scratch;
  mb.max_depth = max_depth ? max_depth : DEFAULT_MAX_DEPTH;
  mb.max_nodes = max_nodes ? max_nodes : DEFAULT_MAX_NODES;
  mb.status = GTEXT_CSS_OK;
  mb.ctx = ctx;
  bool has_symbols = false;
  *valid = false;
  *out = NULL;
  const mnode * root = read_calculation(&mb, function, ctx, &has_symbols);
  if (mb.status != GTEXT_CSS_OK) {
    result_fill(result, mb.status, &mb);
    return mb.status;
  }
  if (root) {
    const GTEXT_CSS_Math_Category cat = css_math_type_category(&root->type);
    if (cat != GTEXT_CSS_MATH_CAT_NONE) {
      GTEXT_CSS_Math * m = css_arena_alloc(arena, sizeof *m);
      if (!m) {
        mb.status = GTEXT_CSS_E_OOM;
        result_fill(result, mb.status, &mb);
        return mb.status;
      }
      memset(m, 0, sizeof *m);
      m->root = root;
      m->type = root->type;
      m->category = cat;
      m->has_percent = mb.saw_percent;
      m->has_symbols = has_symbols;
      m->node_count = mb.nodes;
      *out = m;
      *valid = true;
    }
  }
  result_fill(result, GTEXT_CSS_OK, &mb);
  return GTEXT_CSS_OK;
}

typedef struct {
  css_arena arena;
  GTEXT_Allocator alloc;
} math_owner;

GTEXT_CSS_Status gtext_css_math_parse(const GTEXT_CSS_Component_Value * function, const GTEXT_CSS_Math_Context * context,
    const GTEXT_CSS_Math_Options * options, bool * valid, GTEXT_CSS_Math ** out, GTEXT_CSS_Math_Result * result) {
  if (!function || !valid || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  *valid = false;
  *out = NULL;
  GTEXT_Allocator alloc = *((options && options->allocator) ? options->allocator : gtext_allocator_default());
  size_t depth = options && options->max_depth ? options->max_depth : DEFAULT_MAX_DEPTH;
  size_t nodes = options && options->max_nodes ? options->max_nodes : DEFAULT_MAX_NODES;
  if (depth > LIMIT_MAX_DEPTH) {
    depth = LIMIT_MAX_DEPTH;
  }
  if (nodes > LIMIT_MAX_NODES) {
    nodes = LIMIT_MAX_NODES;
  }
  math_owner * ow = gtext_allocator_malloc(&alloc, sizeof *ow);
  if (!ow) {
    if (result) {
      memset(result, 0, sizeof *result);
      result->status = GTEXT_CSS_E_OOM;
      result->message = gtext_css_status_string(GTEXT_CSS_E_OOM);
    }
    return GTEXT_CSS_E_OOM;
  }
  ow->alloc = alloc;
  css_arena_init(&ow->arena, &ow->alloc);
  GTEXT_CSS_Math * m = NULL;
  bool ok = false;
  const GTEXT_CSS_Status st = css_math_build(&ow->arena, &alloc, function, context, depth, nodes, &m, &ok, result);
  if (st != GTEXT_CSS_OK || !ok) {
    css_arena_free(&ow->arena);
    gtext_allocator_free(&alloc, ow);
    if (st == GTEXT_CSS_OK) {
      *valid = false;
      *out = NULL;
    }
    return st;
  }
  m->owner_ = ow;
  *valid = true;
  *out = m;
  return GTEXT_CSS_OK;
}

void gtext_css_math_free(GTEXT_CSS_Math * math) {
  if (!math || !math->owner_) {
    return;
  }
  math_owner * ow = math->owner_;
  GTEXT_Allocator alloc = ow->alloc;
  css_arena_free(&ow->arena);
  gtext_allocator_free(&alloc, ow);
}

bool gtext_css_math_numeric(const GTEXT_CSS_Math * math, double * value, GTEXT_CSS_Unit * unit, bool * percent) {
  if (!math || !math->root || !value || !css_math_is_numeric(math->root)) {
    return false;
  }
  *value = math->root->value;
  if (unit) {
    *unit = math->root->op == GTEXT_CSS_MATH_DIMENSION ? math->root->unit : GTEXT_CSS_UNIT_NONE;
  }
  if (percent) {
    *percent = math->root->op == GTEXT_CSS_MATH_PERCENT;
  }
  return true;
}

double gtext_css_math_clamp(double value, double lo, double hi) {
  if (value != value) {
    value = 0.0;
  }
  if (value < lo) {
    value = lo;
  }
  if (value > hi) {
    value = hi;
  }
  if (value == 0.0) {
    value = 0.0;
  }
  return value;
}

/* ---- resolving ------------------------------------------------------------------- */

typedef struct {
  const mnode * n;
  size_t idx;
  size_t vbase;
} rframe;

GTEXT_CSS_Status gtext_css_math_resolve(const GTEXT_CSS_Math * math, const GTEXT_CSS_Math_Env * env,
    const GTEXT_Allocator * allocator, double * value, bool * resolved) {
  if (!math || !math->root || !value || !resolved) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_Allocator alloc = *(allocator ? allocator : gtext_allocator_default());
  rframe * fr = NULL;
  double * vs = NULL;
  size_t nf = 0, capf = 0, nv = 0, capv = 0;
  bool ok = true;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  /* push root */
#define PUSHF(node) \
  do { \
    if (nf == capf) { \
      const size_t c_ = capf ? capf * 2 : 32; \
      rframe * p_ = gtext_allocator_realloc(&alloc, fr, c_ * sizeof *p_); \
      if (!p_) { st = GTEXT_CSS_E_OOM; goto out; } \
      fr = p_; capf = c_; \
    } \
    fr[nf].n = (node); fr[nf].idx = 0; fr[nf].vbase = nv; nf++; \
  } while (0)
#define PUSHV(x) \
  do { \
    if (nv == capv) { \
      const size_t c_ = capv ? capv * 2 : 32; \
      double * p_ = gtext_allocator_realloc(&alloc, vs, c_ * sizeof *p_); \
      if (!p_) { st = GTEXT_CSS_E_OOM; goto out; } \
      vs = p_; capv = c_; \
    } \
    vs[nv++] = (x); \
  } while (0)
  PUSHF(math->root);
  while (nf > 0 && ok) {
    rframe * f = &fr[nf - 1];
    const mnode * n = f->n;
    switch (n->op) {
    case GTEXT_CSS_MATH_NUMBER:
      PUSHV(n->value);
      nf--;
      continue;
    case GTEXT_CSS_MATH_DIMENSION: {
      double v = n->value;
      if (!gtext_css_unit_is_absolute(n->unit)) {
        double size;
        if (!env || !env->unit_size || !env->unit_size(env->user, n->unit, &size)) {
          ok = false;
          break;
        }
        v *= size;
      }
      PUSHV(v);
      nf--;
      continue;
    }
    case GTEXT_CSS_MATH_PERCENT:
      if (!env || !env->has_percent_basis) {
        ok = false;
        break;
      }
      PUSHV(n->value / 100.0 * env->percent_basis);
      nf--;
      continue;
    case GTEXT_CSS_MATH_SYMBOL: {
      double v;
      if (!env || !env->symbol) {
        ok = false;
        break;
      }
      /* the name is NUL-terminated in the arena, whatever its length */
      if (!env->symbol(env->user, n->text.data, &v)) {
        ok = false;
        break;
      }
      PUSHV(v);
      nf--;
      continue;
    }
    case GTEXT_CSS_MATH_UNRESOLVED: {
      /* the tree-counting functions are numbers the element tree answers (unresolved_leaf() writes their text as the lower-case name and `()`) */
      const bool index = n->text.length == 15 && memcmp(n->text.data, "sibling-index()", 15) == 0;
      const bool count = n->text.length == 15 && memcmp(n->text.data, "sibling-count()", 15) == 0;
      double v;
      if (!(index || count) || !env || !env->tree_count || !env->tree_count(env->user, index, &v)) {
        ok = false;
        break;
      }
      PUSHV(v);
      nf--;
      continue;
    }
    default:
      break;
    }
    if (!ok) {
      break;
    }
    if (f->idx < n->kid_count) {
      const mnode * k = n->kids[f->idx++];
      PUSHF(k);
      continue;
    }
    /* all operands are on the value stack from f->vbase */
    const double * a = vs + f->vbase;
    const size_t cnt = nv - f->vbase;
    double r = 0.0;
    switch (n->op) {
    case GTEXT_CSS_MATH_SUM:
      r = a[0];
      for (size_t i = 1; i < cnt; i++) {
        r += a[i];
      }
      break;
    case GTEXT_CSS_MATH_PRODUCT:
      r = a[0];
      for (size_t i = 1; i < cnt; i++) {
        r *= a[i];
      }
      break;
    case GTEXT_CSS_MATH_NEGATE:
      r = -a[0];
      break;
    case GTEXT_CSS_MATH_INVERT:
      r = 1.0 / a[0];
      break;
    default: {
      double args[64];
      double * ap = args;
      double * heap = NULL;
      if (cnt > sizeof args / sizeof args[0]) {
        heap = gtext_allocator_malloc(&alloc, cnt * sizeof *heap);
        if (!heap) {
          st = GTEXT_CSS_E_OOM;
          goto out;
        }
        ap = heap;
      }
      memcpy(ap, a, cnt * sizeof *ap);
      const bool angle_in = cnt > 0 && css_math_type_category(&n->kids[0]->type) == GTEXT_CSS_MATH_CAT_ANGLE;
      if (angle_in && (n->op == GTEXT_CSS_MATH_SIN || n->op == GTEXT_CSS_MATH_COS || n->op == GTEXT_CSS_MATH_TAN)) {
        {
          volatile double scaled = ap[0] * PI_VALUE;
          ap[0] = scaled / 180.0;
        }
      }
      if (n->op == GTEXT_CSS_MATH_ROUND && n->round == GTEXT_CSS_MATH_ROUND_LINE_WIDTH) {
        ok = false;
      }
      else if (!css_math_apply_ex(n->op, n->round, n->no_clamp, ap, cnt, &r)) {
        ok = false;
      }
      else if (n->op == GTEXT_CSS_MATH_ASIN || n->op == GTEXT_CSS_MATH_ACOS || n->op == GTEXT_CSS_MATH_ATAN ||
               n->op == GTEXT_CSS_MATH_ATAN2) {
        r = r * 180.0 / PI_VALUE;
      }
      gtext_allocator_free(&alloc, heap);
      break;
    }
    }
    if (!ok) {
      break;
    }
    nv = f->vbase;
    nf--;
    PUSHV(r);
  }
  if (ok && nv == 1) {
    *value = vs[0];
  }
  else {
    ok = false;
  }
out:
  gtext_allocator_free(&alloc, fr);
  gtext_allocator_free(&alloc, vs);
#undef PUSHF
#undef PUSHV
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  *resolved = ok;
  return GTEXT_CSS_OK;
}
