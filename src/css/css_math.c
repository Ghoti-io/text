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
    /* a random key (auto, fixed <number>, a dashed identifier, element-scoped ...) comes before the first comma and is not a calculation */
    size_t comma = 0;
    while (comma < F->n && !(F->items[comma].kind == GTEXT_CSS_VALUE_TOKEN && F->items[comma].type == GTEXT_CSS_TOKEN_COMMA)) {
      comma++;
    }
    bool key = comma < F->n;
    size_t parts = 0, dashed = 0, element = 0, property = 0, fixed = 0, autos = 0, numbers = 0;
    double fixed_value = 0.0;
    for (size_t i = 0; key && i < comma; i++) {
      const GTEXT_CSS_Component_Value * c = &F->items[i];
      if (is_ws(c)) {
        continue;
      }
      parts++;
      if (c->kind == GTEXT_CSS_VALUE_TOKEN && c->type == GTEXT_CSS_TOKEN_IDENT) {
        if (name_is(&c->value, "fixed")) {
          fixed++;
        }
        else if (name_is(&c->value, "auto")) {
          autos++;
        }
        else if (name_is(&c->value, "element-scoped")) {
          element++;
        }
        else if (name_is(&c->value, "property-scoped") || name_is(&c->value, "property-index-scoped")) {
          property++;
        }
        else if (c->value.length >= 2 && c->value.data[0] == '-' && c->value.data[1] == '-') {
          dashed++;
        }
        else {
          key = false;
        }
      }
      else if (c->kind == GTEXT_CSS_VALUE_TOKEN && c->type == GTEXT_CSS_TOKEN_NUMBER) {
        numbers++;
        fixed_value = c->number;
      }
      else {
        key = false;
      }
    }
    /* <random-key> = auto | <random-cache-key> | fixed <number [0,1]>; <random-cache-key> = <dashed-ident> || element-scoped || [ property-scoped | property-index-scoped ] */
    if (key) {
      if (fixed) {
        key = fixed == 1 && numbers == 1 && parts == 2 && fixed_value >= 0.0 && fixed_value <= 1.0;
      }
      else if (autos) {
        key = autos == 1 && parts == 1;
      }
      else {
        key = numbers == 0 && dashed <= 1 && element <= 1 && property <= 1;
      }
    }
    if (key && parts > 0) {
      GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
      so.allocator = &mb->alloc;
      char * buf = NULL;
      size_t len = 0;
      const GTEXT_CSS_Status st = gtext_css_serialize_values_buffer(F->items, comma, &so, &buf, &len);
      if (st != GTEXT_CSS_OK) {
        mb_fail(mb, st);
        return false;
      }
      size_t b = 0, e = len;
      while (b < e && buf[b] == ' ') {
        b++;
      }
      while (e > b && buf[e - 1] == ' ') {
        e--;
      }
      F->key = css_arena_strdup(mb->arena, buf + b, e - b);
      F->key_len = e - b;
      gtext_css_serialize_buffer_free(&so, buf);
      if (!F->key) {
        mb_fail(mb, GTEXT_CSS_E_OOM);
        return false;
      }
      F->i = comma + 1;
    }
  }
  return true;
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
      if (u == GTEXT_CSS_UNIT_NONE) {
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
    case GTEXT_CSS_MATH_UNRESOLVED:
      ok = false;
      break;
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
