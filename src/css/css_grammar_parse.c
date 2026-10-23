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
 * The text of a CSS Value Definition Syntax grammar, read into a definition.
 *
 * Nothing here recurses: the groups of the text (`[ ]`, `name( )`, `{ }`,
 * `( )`) are an explicit stack of frames, and the four levels of precedence
 * (juxtaposition, `&&`, `||`, `|`) are four bases into one array of the nodes
 * made so far. A node is finished when its operator, or the end of its group,
 * is met.
 *
 * Reference: CSS Values and Units Level 4, section 2 (Component Value Types /
 * Value Definition Syntax): "Juxtaposing components means that all of them must
 * occur, in the given order. ... && ... || ... | ...  The four operators have
 * precedence: juxtaposition binds tighter than &&, which binds tighter than ||,
 * which binds tighter than |", and the multipliers `?` `*` `+` `#` `{A,B}`
 * and `!`, the range after a type's name, and function notation.
 */

#include <ghoti.io/math/decimal.h>
#include <ghoti.io/text/macros.h>

#include <string.h>

#include "css_grammar_internal.h"

/* ---- shared helpers --------------------------------------------------- */

static const struct {
  const char * name;
  gn_native type;
} kNatives[] = {
  {"integer", NT_INTEGER}, {"number", NT_NUMBER}, {"percentage", NT_PERCENTAGE},
  {"dimension", NT_DIMENSION}, {"length", NT_LENGTH}, {"angle", NT_ANGLE}, {"time", NT_TIME},
  {"frequency", NT_FREQUENCY}, {"resolution", NT_RESOLUTION}, {"flex", NT_FLEX},
  {"ratio", NT_RATIO}, {"zero", NT_ZERO}, {"string", NT_STRING}, {"url", NT_URL},
  {"ident", NT_IDENT}, {"custom-ident", NT_CUSTOM_IDENT}, {"dashed-ident", NT_DASHED_IDENT},
  {"any-value", NT_ANY_VALUE}, {"declaration-value", NT_DECL_VALUE}, {"urange", NT_URANGE},
  {"an+b", NT_ANB}, {"declaration-list", NT_DECL_LIST}, {"rule-list", NT_RULE_LIST},
  {"ident-token", NT_TOK_IDENT}, {"function-token", NT_TOK_FUNCTION},
  {"at-keyword-token", NT_TOK_AT_KEYWORD}, {"hash-token", NT_TOK_HASH},
  {"string-token", NT_TOK_STRING}, {"bad-string-token", NT_TOK_BAD_STRING},
  {"url-token", NT_TOK_URL}, {"bad-url-token", NT_TOK_BAD_URL}, {"delim-token", NT_TOK_DELIM},
  {"number-token", NT_TOK_NUMBER}, {"percentage-token", NT_TOK_PERCENTAGE},
  {"dimension-token", NT_TOK_DIMENSION}, {"whitespace-token", NT_TOK_WS},
  {"CDO-token", NT_TOK_CDO}, {"CDC-token", NT_TOK_CDC}, {"colon-token", NT_TOK_COLON},
  {"semicolon-token", NT_TOK_SEMICOLON}, {"comma-token", NT_TOK_COMMA},
  {"[-token", NT_TOK_OPEN_SQUARE}, {"]-token", NT_TOK_CLOSE_SQUARE},
  {"(-token", NT_TOK_OPEN_PAREN}, {")-token", NT_TOK_CLOSE_PAREN},
  {"{-token", NT_TOK_OPEN_CURLY}, {"}-token", NT_TOK_CLOSE_CURLY},
};

gn_native css_grammar_native_lookup(const char * name, size_t length) {
  for (size_t i = 0; i < sizeof kNatives / sizeof kNatives[0]; i++) {
    if (strlen(kNatives[i].name) == length && memcmp(kNatives[i].name, name, length) == 0) {
      return kNatives[i].type;
    }
  }
  return NT_COUNT;
}

bool css_grammar_native_numeric(gn_native type) {
  switch (type) {
  case NT_INTEGER: case NT_NUMBER: case NT_PERCENTAGE: case NT_DIMENSION: case NT_LENGTH:
  case NT_ANGLE: case NT_TIME: case NT_FREQUENCY: case NT_RESOLUTION: case NT_FLEX:
    return true;
  default:
    return false;
  }
}

bool css_grammar_native_nullable(gn_native type) {
  return type == NT_DECL_LIST || type == NT_RULE_LIST || type == NT_TOK_WS;
}

const char * css_grammar_message(GTEXT_CSS_Status status) {
  return gtext_css_status_string(status);
}

void css_grammar_result(GTEXT_CSS_Grammar_Result * result, GTEXT_CSS_Status status, size_t offset,
    const char * name, size_t name_length, size_t nodes) {
  if (!result) {
    return;
  }
  memset(result, 0, sizeof *result);
  result->status = status;
  result->message = css_grammar_message(status);
  result->offset = offset;
  result->nodes = nodes;
  if (name && name_length) {
    if (name_length > sizeof result->name - 1) {
      name_length = sizeof result->name - 1;
    }
    memcpy(result->name, name, name_length);
    result->name[name_length] = '\0';
  }
}

GTEXT_Allocator css_grammar_allocator(const GTEXT_CSS_Grammar_Options * options) {
  return *((options && options->allocator) ? options->allocator : gtext_allocator_default());
}

#define DEFAULT_MAX_DEFINITION_BYTES ((size_t)65536)
#define DEFAULT_MAX_DEFINITION_DEPTH ((size_t)256)
#define DEFAULT_MAX_DEFINITION_NODES ((size_t)65536)

GTEXT_CSS_Grammar_Options gtext_css_grammar_options_default(void) {
  GTEXT_CSS_Grammar_Options o;
  memset(&o, 0, sizeof o);
  o.max_definition_bytes = DEFAULT_MAX_DEFINITION_BYTES;
  o.max_definition_depth = DEFAULT_MAX_DEFINITION_DEPTH;
  o.max_definition_nodes = DEFAULT_MAX_DEFINITION_NODES;
  return o;
}

void css_grammar_limits(const GTEXT_CSS_Grammar_Options * options, size_t * bytes, size_t * depth, size_t * nodes) {
  if (bytes) {
    *bytes = (options && options->max_definition_bytes) ? options->max_definition_bytes : DEFAULT_MAX_DEFINITION_BYTES;
  }
  if (depth) {
    *depth = (options && options->max_definition_depth) ? options->max_definition_depth : DEFAULT_MAX_DEFINITION_DEPTH;
  }
  if (nodes) {
    *nodes = (options && options->max_definition_nodes) ? options->max_definition_nodes : DEFAULT_MAX_DEFINITION_NODES;
  }
}

bool css_grammar_grow(const GTEXT_Allocator * alloc, void ** p, size_t * cap, size_t need, size_t elem) {
  if (need <= *cap) {
    return true;
  }
  size_t ncap = *cap ? *cap : 16;
  while (ncap < need) {
    if (ncap > SIZE_MAX / 2) {
      return false;
    }
    ncap *= 2;
  }
  if (ncap > SIZE_MAX / elem) {
    return false;
  }
  void * grown = gtext_allocator_realloc(alloc, *p, ncap * elem);
  if (!grown) {
    return false;
  }
  *p = grown;
  *cap = ncap;
  return true;
}

/* ---- the definition --------------------------------------------------- */

void gtext_css_definition_free(GTEXT_CSS_Definition * d) {
  if (!d) {
    return;
  }
  GTEXT_Allocator a = d->alloc;
  gtext_allocator_free(&a, d->text);
  gtext_allocator_free(&a, d->nodes);
  gtext_allocator_free(&a, d->kids);
  gtext_allocator_free(&a, d);
}

size_t gtext_css_definition_node_count(const GTEXT_CSS_Definition * d) {
  return d ? d->nnodes : 0;
}

/* ---- the parser -------------------------------------------------------- */

typedef enum {
  F_ROOT = 0, ///< The whole text.
  F_BRACKET,  ///< `[ ... ]`
  F_FUNC,     ///< `name( ... )`
  F_CURLY,    ///< `{ ... }`
  F_PAREN     ///< `( ... )`
} frame_kind;

typedef struct {
  frame_kind kind;
  size_t b_alt, b_or, b_and, b_seq; ///< Bases into `acc` of the four levels' members.
  size_t open;                      ///< Offset of the opening character (the name, for a function).
  size_t name_off, name_len;        ///< A function's name.
} pframe;

typedef struct {
  GTEXT_CSS_Definition * d;
  const char * t;
  size_t n;
  size_t pos;
  uint32_t * acc;
  size_t acc_len, acc_cap;
  pframe * fr;
  size_t nfr, frcap;
  size_t max_depth;
  size_t max_nodes;
  GTEXT_CSS_Status status;
  size_t err;
} parser;

static void fail(parser * P, GTEXT_CSS_Status status, size_t at) {
  if (P->status == GTEXT_CSS_OK) {
    P->status = status;
    P->err = at;
  }
}

static bool ok(const parser * P) {
  return P->status == GTEXT_CSS_OK;
}

static uint32_t new_node(parser * P, const gnode * proto) {
  GTEXT_CSS_Definition * d = P->d;
  if (!ok(P)) {
    return 0;
  }
  if (d->nnodes >= P->max_nodes) {
    fail(P, GTEXT_CSS_E_LIMIT_DEFINITION_NODES, P->pos);
    return 0;
  }
  void * nodes = d->nodes;
  if (!css_grammar_grow(&d->alloc, &nodes, &d->ncap, d->nnodes + 1, sizeof(gnode))) {
    fail(P, GTEXT_CSS_E_OOM, P->pos);
    return 0;
  }
  d->nodes = nodes;
  d->nodes[d->nnodes] = *proto;
  return (uint32_t)d->nnodes++;
}

static bool add_kids(parser * P, const uint32_t * items, size_t count, uint32_t * first) {
  GTEXT_CSS_Definition * d = P->d;
  void * kids = d->kids;
  if (!css_grammar_grow(&d->alloc, &kids, &d->kcap, d->nkids + count, sizeof(uint32_t))) {
    fail(P, GTEXT_CSS_E_OOM, P->pos);
    return false;
  }
  d->kids = kids;
  *first = (uint32_t)d->nkids;
  if (count) {
    memcpy(d->kids + d->nkids, items, count * sizeof(uint32_t));
  }
  d->nkids += count;
  return true;
}

static void proto_init(gnode * g, gn_kind kind) {
  memset(g, 0, sizeof *g);
  g->kind = (uint8_t)kind;
  g->min = 1;
  g->max = 1;
  g->target = UINT32_MAX;
}

static bool push_acc(parser * P, uint32_t node) {
  void * acc = P->acc;
  if (!css_grammar_grow(&P->d->alloc, &acc, &P->acc_cap, P->acc_len + 1, sizeof(uint32_t))) {
    fail(P, GTEXT_CSS_E_OOM, P->pos);
    return false;
  }
  P->acc = acc;
  P->acc[P->acc_len++] = node;
  return true;
}

static void span_of(const parser * P, const uint32_t * items, size_t n, uint32_t * off, uint32_t * len) {
  const gnode * f = &P->d->nodes[items[0]];
  const gnode * l = &P->d->nodes[items[n - 1]];
  *off = f->src_off;
  const uint32_t end = l->src_off + l->src_len;
  *len = end >= f->src_off ? end - f->src_off : 0;
}

static bool is_literal(const parser * P, uint32_t node, uint32_t ch) {
  const gnode * g = &P->d->nodes[node];
  return g->kind == GN_LITERAL && g->lit == ch;
}

static bool any_literal(const parser * P, const uint32_t * items, size_t n, uint32_t ch) {
  for (size_t i = 0; i < n; i++) {
    if (is_literal(P, items[i], ch)) {
      return true;
    }
  }
  return false;
}

static uint8_t nonnull_of_group(const parser * P, gn_kind kind, const uint32_t * items, size_t n) {
  bool any = false, all = true;
  for (size_t i = 0; i < n; i++) {
    const bool nn = (P->d->nodes[items[i]].flags & GNF_NONNULL) != 0;
    any = any || nn;
    all = all && nn;
  }
  const bool r = (kind == GN_SEQ) ? any : all;
  return r ? GNF_NONNULL : 0;
}

/// A node of `kind` over `items` (not 1 of them).
static uint32_t make_group(parser * P, gn_kind kind, uint8_t sep, const uint32_t * items, size_t n) {
  gnode g;
  proto_init(&g, kind);
  g.sep = sep;
  g.flags = nonnull_of_group(P, kind, items, n);
  span_of(P, items, n, &g.src_off, &g.src_len);
  g.nkids = (uint32_t)n;
  if (!add_kids(P, items, n, &g.kids)) {
    return 0;
  }
  return new_node(P, &g);
}

/// A juxtaposition of `n` items with no separator handling.
static uint32_t plain_seq(parser * P, const uint32_t * items, size_t n) {
  if (n == 1) {
    return items[0];
  }
  return make_group(P, GN_SEQ, 0, items, n);
}

/// Split `items` at the literal `ch` into segments that are each non-empty and
/// known to consume something; false if it cannot be (a literal at an end, two
/// in a row, a segment that may be empty): the literals then stay literals.
static bool split_ok(const parser * P, const uint32_t * items, size_t n, uint32_t ch, size_t * segments) {
  size_t count = 1, run = 0;
  bool nonnull = false;
  for (size_t i = 0; i < n; i++) {
    if (is_literal(P, items[i], ch)) {
      if (run == 0 || !nonnull) {
        return false;
      }
      count++;
      run = 0;
      nonnull = false;
    }
    else {
      run++;
      nonnull = nonnull || (P->d->nodes[items[i]].flags & GNF_NONNULL) != 0;
    }
  }
  if (run == 0 || !nonnull || count < 2) {
    return false;
  }
  *segments = count;
  return true;
}

/// A juxtaposition whose slashes, if they can, separate its segments.
static uint32_t build_slash(parser * P, const uint32_t * items, size_t n) {
  size_t segs;
  if (n < 3 || !any_literal(P, items, n, '/') || !split_ok(P, items, n, '/', &segs)) {
    return plain_seq(P, items, n);
  }
  uint32_t * out = NULL;
  size_t cap = 0;
  void * o = NULL;
  if (!css_grammar_grow(&P->d->alloc, &o, &cap, segs, sizeof(uint32_t))) {
    fail(P, GTEXT_CSS_E_OOM, P->pos);
    return 0;
  }
  out = o;
  size_t k = 0, start = 0;
  for (size_t i = 0; i <= n && ok(P); i++) {
    if (i == n || is_literal(P, items[i], '/')) {
      out[k++] = plain_seq(P, items + start, i - start);
      start = i + 1;
    }
  }
  uint32_t r = 0;
  if (ok(P)) {
    r = make_group(P, GN_SEQ, 2, out, k);
  }
  gtext_allocator_free(&P->d->alloc, out);
  return r;
}

/// A juxtaposition, with commas and slashes that separate segments turned into list separators.
static uint32_t build_seq(parser * P, const uint32_t * items, size_t n) {
  if (n == 1) {
    return items[0];
  }
  size_t segs;
  if (n >= 3 && any_literal(P, items, n, ',') && split_ok(P, items, n, ',', &segs)) {
    void * o = NULL;
    size_t cap = 0;
    if (!css_grammar_grow(&P->d->alloc, &o, &cap, segs, sizeof(uint32_t))) {
      fail(P, GTEXT_CSS_E_OOM, P->pos);
      return 0;
    }
    uint32_t * out = o;
    size_t k = 0, start = 0;
    for (size_t i = 0; i <= n && ok(P); i++) {
      if (i == n || is_literal(P, items[i], ',')) {
        out[k++] = build_slash(P, items + start, i - start);
        start = i + 1;
      }
    }
    uint32_t r = 0;
    if (ok(P)) {
      r = make_group(P, GN_SEQ, 1, out, k);
    }
    gtext_allocator_free(&P->d->alloc, out);
    return r;
  }
  return build_slash(P, items, n);
}

/* Finish the level that ends at the operator or the end of the group. Each takes the members of its
   level (from its base to the end of `acc`), makes one node of them and leaves that node as the last
   member of the level below. */
static void finish_seq(parser * P, pframe * f) {
  const size_t n = P->acc_len - f->b_seq;
  if (n == 0) {
    fail(P, GTEXT_CSS_E_GRAMMAR, P->pos);
    return;
  }
  const uint32_t node = build_seq(P, P->acc + f->b_seq, n);
  if (!ok(P)) {
    return;
  }
  P->acc_len = f->b_seq;
  push_acc(P, node);
  f->b_seq = P->acc_len;
}

static void finish_level(parser * P, pframe * f, gn_kind kind, size_t * base, size_t above) {
  const size_t n = above - *base;
  uint32_t node;
  if (n == 1) {
    node = P->acc[*base];
  }
  else {
    if (kind != GN_ALT && n > GN_MAX_GROUP) {
      fail(P, GTEXT_CSS_E_LIMIT_GROUP, P->pos);
      return;
    }
    node = make_group(P, kind, 0, P->acc + *base, n);
  }
  if (!ok(P)) {
    return;
  }
  P->acc_len = *base;
  push_acc(P, node);
  *base = P->acc_len;
  (void)f;
}

static void finish_and(parser * P, pframe * f) {
  finish_seq(P, f);
  if (!ok(P)) {
    return;
  }
  finish_level(P, f, GN_AND, &f->b_and, f->b_seq);
  f->b_seq = P->acc_len;
}

static void finish_or(parser * P, pframe * f) {
  finish_and(P, f);
  if (!ok(P)) {
    return;
  }
  finish_level(P, f, GN_OR, &f->b_or, f->b_and);
  f->b_and = f->b_seq = P->acc_len;
}

static void finish_alt(parser * P, pframe * f) {
  finish_or(P, f);
  if (!ok(P)) {
    return;
  }
  finish_level(P, f, GN_ALT, &f->b_alt, f->b_or);
  f->b_or = f->b_and = f->b_seq = P->acc_len;
}

/* ---- lexical helpers --------------------------------------------------- */

static bool is_ident_start(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-' || c >= 0x80;
}

static bool is_ident_char(unsigned char c) {
  return is_ident_start(c) || (c >= '0' && c <= '9');
}

static bool is_digit(char c) {
  return c >= '0' && c <= '9';
}

static void skip_ws(parser * P) {
  while (P->pos < P->n && (P->t[P->pos] == ' ' || P->t[P->pos] == '\t' || P->t[P->pos] == '\n'
         || P->t[P->pos] == '\r' || P->t[P->pos] == '\f')) {
    P->pos++;
  }
}

/// A bound of a range: `∞`, `-∞`, `infinity`, or a number with an optional unit or `%`.
static bool read_bound(parser * P, double * out) {
  skip_ws(P);
  bool neg = false;
  size_t p = P->pos;
  if (p < P->n && (P->t[p] == '-' || P->t[p] == '+')) {
    neg = P->t[p] == '-';
    p++;
  }
  if (P->n - p >= 3 && memcmp(P->t + p, "\xE2\x88\x9E", 3) == 0) { /* U+221E */
    *out = neg ? -1.0 / 0.0 : 1.0 / 0.0;
    P->pos = p + 3;
    return true;
  }
  if (P->n - p >= 8 && memcmp(P->t + p, "infinity", 8) == 0) {
    *out = neg ? -1.0 / 0.0 : 1.0 / 0.0;
    P->pos = p + 8;
    return true;
  }
  double v;
  size_t used = 0;
  if (p >= P->n || !(is_digit(P->t[p]) || P->t[p] == '.')
      || gmath_decimal_parse(P->t + p, P->n - p, &v, &used, NULL) != GMATH_OK) {
    return false;
  }
  p += used;
  if (neg) {
    v = -v;
  }
  if (p < P->n && P->t[p] == '%') {
    p++;
  }
  else {
    const size_t us = p;
    while (p < P->n && ((P->t[p] >= 'a' && P->t[p] <= 'z') || (P->t[p] >= 'A' && P->t[p] <= 'Z'))) {
      p++;
    }
    if (p > us) {
      const GTEXT_CSS_Unit u = gtext_css_unit_lookup(P->t + us, p - us);
      if (u == GTEXT_CSS_UNIT_NONE || !gtext_css_unit_to_canonical(u, v, &v)) {
        return false;
      }
    }
  }
  *out = v;
  P->pos = p;
  return true;
}

/// A reference: `<name>`, `<'name'>` or `<name [lo,hi]>`, with P->pos on the `<`.
static uint32_t read_reference(parser * P) {
  const size_t start = P->pos;
  P->pos++;
  gnode g;
  proto_init(&g, GN_REF);
  g.native = GTEXT_CSS_REF_TYPE;
  if (P->pos < P->n && P->t[P->pos] == '\'') {
    g.native = GTEXT_CSS_REF_PROPERTY;
    P->pos++;
  }
  const size_t name = P->pos;
  if (g.native == GTEXT_CSS_REF_TYPE && P->pos < P->n && strchr("[](){}", P->t[P->pos])) {
    /* <[-token> and its five kin are named by a bracket */
    P->pos++;
  }
  while (P->pos < P->n && P->t[P->pos] != '>' && P->t[P->pos] != ' ' && P->t[P->pos] != '\'' && P->t[P->pos] != '[') {
    P->pos++;
  }
  g.str_off = (uint32_t)name;
  g.str_len = (uint32_t)(P->pos - name);
  if (g.str_len == 0) {
    fail(P, GTEXT_CSS_E_GRAMMAR, name);
    return 0;
  }
  if (g.native == GTEXT_CSS_REF_PROPERTY) {
    if (P->pos >= P->n || P->t[P->pos] != '\'') {
      fail(P, GTEXT_CSS_E_GRAMMAR, P->pos);
      return 0;
    }
    P->pos++;
  }
  skip_ws(P);
  if (P->pos < P->n && P->t[P->pos] == '[') {
    P->pos++;
    double lo, hi;
    if (!read_bound(P, &lo)) {
      fail(P, GTEXT_CSS_E_GRAMMAR, P->pos);
      return 0;
    }
    skip_ws(P);
    if (P->pos >= P->n || P->t[P->pos] != ',') {
      fail(P, GTEXT_CSS_E_GRAMMAR, P->pos);
      return 0;
    }
    P->pos++;
    if (!read_bound(P, &hi)) {
      fail(P, GTEXT_CSS_E_GRAMMAR, P->pos);
      return 0;
    }
    skip_ws(P);
    if (P->pos >= P->n || P->t[P->pos] != ']' || lo > hi) {
      fail(P, GTEXT_CSS_E_GRAMMAR, P->pos);
      return 0;
    }
    P->pos++;
    skip_ws(P);
    g.flags |= GNF_RANGE;
    g.lo = lo;
    g.hi = hi;
  }
  if (P->pos >= P->n || P->t[P->pos] != '>') {
    fail(P, GTEXT_CSS_E_GRAMMAR, P->pos);
    return 0;
  }
  P->pos++;
  g.flags |= GNF_NONNULL;
  g.src_off = (uint32_t)start;
  g.src_len = (uint32_t)(P->pos - start);
  return new_node(P, &g);
}

/// Wrap `child` in a multiplier, merging it with a multiplier of the same kind it already has (so that
/// `[ x* ]+` is `x*`: the same strings, and a repetition of a closure is not worked twice).
static uint32_t wrap_mult(parser * P, uint32_t child, uint32_t min, uint32_t max, bool comma, size_t term_start) {
  const gnode inner = P->d->nodes[child];
  if (!comma && inner.kind == GN_MULT && !(inner.flags & GNF_COMMA)) {
    const uint32_t a = inner.min, b = inner.max;
    const uint32_t grand = P->d->kids[inner.kids];
    if (b == GN_INF && max == GN_INF && a <= 1) {
      min = a * min;
      child = grand;
    }
    else if (a == 0 && b == 1 && max == GN_INF) {
      min = 0;
      child = grand;
    }
    else if (a <= 1 && b == GN_INF && min == 0 && max == 1) {
      min = 0;
      max = GN_INF;
      child = grand;
    }
    else if (a == 0 && b == 1 && min == 0 && max == 1) {
      return child;
    }
  }
  gnode g;
  proto_init(&g, GN_MULT);
  g.min = min;
  g.max = max;
  g.flags = (uint8_t)((comma ? GNF_COMMA : 0) | ((min > 0 && (P->d->nodes[child].flags & GNF_NONNULL)) ? GNF_NONNULL : 0));
  g.src_off = (uint32_t)term_start;
  g.src_len = (uint32_t)(P->pos - term_start);
  g.nkids = 1;
  if (!add_kids(P, &child, 1, &g.kids)) {
    return 0;
  }
  return new_node(P, &g);
}

/// Read `{A}`, `{A,}` or `{A,B}` with P->pos on the `{`.
static bool read_braces(parser * P, uint32_t * min, uint32_t * max) {
  size_t p = P->pos + 1;
  uint64_t a = 0, b = 0;
  size_t digits = 0;
  while (p < P->n && is_digit(P->t[p])) {
    a = a * 10 + (uint64_t)(P->t[p] - '0');
    if (a > GN_MAX_COUNT) {
      fail(P, GTEXT_CSS_E_GRAMMAR, p);
      return false;
    }
    p++;
    digits++;
  }
  if (digits == 0) {
    fail(P, GTEXT_CSS_E_GRAMMAR, p);
    return false;
  }
  *min = (uint32_t)a;
  if (p < P->n && P->t[p] == '}') {
    *max = (uint32_t)a;
  }
  else if (p < P->n && P->t[p] == ',') {
    p++;
    digits = 0;
    while (p < P->n && is_digit(P->t[p])) {
      b = b * 10 + (uint64_t)(P->t[p] - '0');
      if (b > GN_MAX_COUNT) {
        fail(P, GTEXT_CSS_E_GRAMMAR, p);
        return false;
      }
      p++;
      digits++;
    }
    if (p >= P->n || P->t[p] != '}') {
      fail(P, GTEXT_CSS_E_GRAMMAR, p);
      return false;
    }
    *max = digits ? (uint32_t)b : GN_INF;
  }
  else {
    fail(P, GTEXT_CSS_E_GRAMMAR, p);
    return false;
  }
  if (*max != GN_INF && (*max < *min || *max == 0)) {
    fail(P, GTEXT_CSS_E_GRAMMAR, p);
    return false;
  }
  P->pos = p + 1;
  return true;
}

/// The multipliers that follow a term, immediately; the term (a node) is wrapped in each.
static uint32_t read_multipliers(parser * P, uint32_t term, size_t term_start, bool group) {
  while (ok(P) && P->pos < P->n) {
    const char c = P->t[P->pos];
    uint32_t min, max;
    bool comma = false;
    if (c == '?') {
      min = 0;
      max = 1;
      P->pos++;
    }
    else if (c == '*') {
      min = 0;
      max = GN_INF;
      P->pos++;
    }
    else if (c == '+') {
      min = 1;
      max = GN_INF;
      P->pos++;
    }
    else if (c == '#') {
      min = 1;
      max = GN_INF;
      comma = true;
      P->pos++;
      if (P->pos + 1 < P->n && P->t[P->pos] == '{' && is_digit(P->t[P->pos + 1])) {
        if (!read_braces(P, &min, &max)) {
          return 0;
        }
      }
    }
    else if (c == '{' && P->pos + 1 < P->n && is_digit(P->t[P->pos + 1])) {
      if (!read_braces(P, &min, &max)) {
        return 0;
      }
    }
    else if (c == '!') {
      if (!group) {
        fail(P, GTEXT_CSS_E_GRAMMAR, P->pos);
        return 0;
      }
      P->pos++;
      gnode g;
      proto_init(&g, GN_REQ);
      g.flags = GNF_NONNULL;
      g.nkids = 1;
      g.src_off = (uint32_t)term_start;
      g.src_len = (uint32_t)(P->pos - term_start);
      if (!add_kids(P, &term, 1, &g.kids)) {
        return 0;
      }
      term = new_node(P, &g);
      group = false;
      continue;
    }
    else {
      break;
    }
    term = wrap_mult(P, term, min, max, comma, term_start);
    group = false;
  }
  return term;
}

static bool open_frame(parser * P, frame_kind kind, size_t open, size_t name_off, size_t name_len) {
  if (P->nfr >= P->max_depth + 1) {
    fail(P, GTEXT_CSS_E_LIMIT_DEFINITION_DEPTH, open);
    return false;
  }
  void * fr = P->fr;
  if (!css_grammar_grow(&P->d->alloc, &fr, &P->frcap, P->nfr + 1, sizeof(pframe))) {
    fail(P, GTEXT_CSS_E_OOM, open);
    return false;
  }
  P->fr = fr;
  pframe * f = &P->fr[P->nfr++];
  f->kind = kind;
  f->b_alt = f->b_or = f->b_and = f->b_seq = P->acc_len;
  f->open = open;
  f->name_off = name_off;
  f->name_len = name_len;
  return true;
}

/// The single node a frame's contents make, or 0 with `*empty` set when it has none.
static uint32_t close_contents(parser * P, pframe * f, bool * empty) {
  *empty = false;
  if (P->acc_len == f->b_alt) {
    *empty = true;
    return 0;
  }
  const size_t first = f->b_alt;
  finish_alt(P, f);
  if (!ok(P)) {
    return 0;
  }
  const uint32_t node = P->acc[first];
  P->acc_len = first;
  return node;
}

static bool parse_text(parser * P) {
  if (!open_frame(P, F_ROOT, 0, 0, 0)) {
    return false;
  }
  while (ok(P)) {
    skip_ws(P);
    if (P->pos >= P->n) {
      break;
    }
    pframe * f = &P->fr[P->nfr - 1];
    const char c = P->t[P->pos];
    const size_t at = P->pos;
    if (c == '|') {
      if (P->pos + 1 < P->n && P->t[P->pos + 1] == '|') {
        P->pos += 2;
        finish_and(P, f);
      }
      else {
        P->pos++;
        finish_or(P, f);
      }
      continue;
    }
    if (c == '&') {
      if (P->pos + 1 >= P->n || P->t[P->pos + 1] != '&') {
        fail(P, GTEXT_CSS_E_GRAMMAR, at);
        break;
      }
      P->pos += 2;
      finish_seq(P, f);
      continue;
    }
    uint32_t term = 0;
    bool group = false;
    size_t term_start = at;
    if (c == '[' || c == '(' || c == '{') {
      if (!open_frame(P, c == '[' ? F_BRACKET : c == '(' ? F_PAREN : F_CURLY, at, 0, 0)) {
        break;
      }
      P->pos++;
      continue;
    }
    if (c == ']' || c == ')' || c == '}') {
      const frame_kind want = c == ']' ? F_BRACKET : c == '}' ? F_CURLY : F_PAREN;
      if (P->nfr <= 1 || !(f->kind == want || (c == ')' && f->kind == F_FUNC))) {
        fail(P, GTEXT_CSS_E_GRAMMAR, at);
        break;
      }
      bool empty;
      const uint32_t inner = close_contents(P, f, &empty);
      if (!ok(P)) {
        break;
      }
      if (empty && f->kind == F_BRACKET) {
        fail(P, GTEXT_CSS_E_GRAMMAR, at);
        break;
      }
      P->pos++;
      const pframe done = *f;
      P->nfr--;
      if (done.kind == F_BRACKET) {
        term = inner;
        group = true;
      }
      else {
        gnode g;
        proto_init(&g, done.kind == F_FUNC ? GN_FUNC : GN_BLOCK);
        g.flags = GNF_NONNULL;
        if (done.kind == F_FUNC) {
          g.str_off = (uint32_t)done.name_off;
          g.str_len = (uint32_t)done.name_len;
        }
        else {
          g.native = done.kind == F_CURLY ? GTEXT_CSS_TOKEN_OPEN_CURLY : GTEXT_CSS_TOKEN_OPEN_PAREN;
        }
        g.src_off = (uint32_t)done.open;
        g.src_len = (uint32_t)(P->pos - done.open);
        if (!empty) {
          g.nkids = 1;
          if (!add_kids(P, &inner, 1, &g.kids)) {
            break;
          }
        }
        term = new_node(P, &g);
      }
      term_start = done.open;
      if (!ok(P)) {
        break;
      }
    }
    else if (c == '<') {
      term = read_reference(P);
    }
    else if (c == '\'') {
      /* A quoted literal: one code point. */
      size_t p = P->pos + 1;
      size_t len = 0;
      uint32_t cp = 0;
      const unsigned char b0 = p < P->n ? (unsigned char)P->t[p] : 0;
      if (b0 == 0 || b0 == '\'' || b0 == '\\') {
        fail(P, GTEXT_CSS_E_GRAMMAR, p);
        break;
      }
      len = b0 < 0x80 ? 1 : b0 >= 0xF0 ? 4 : b0 >= 0xE0 ? 3 : 2;
      if (p + len >= P->n || P->t[p + len] != '\'') {
        fail(P, GTEXT_CSS_E_GRAMMAR, p);
        break;
      }
      cp = b0;
      if (len > 1) {
        cp = b0 & (len == 2 ? 0x1Fu : len == 3 ? 0x0Fu : 0x07u);
        for (size_t k = 1; k < len; k++) {
          cp = (cp << 6) | ((unsigned char)P->t[p + k] & 0x3Fu);
        }
      }
      P->pos = p + len + 1;
      gnode g;
      proto_init(&g, GN_LITERAL);
      g.lit = cp;
      g.flags = GNF_NONNULL;
      g.src_off = (uint32_t)at;
      g.src_len = (uint32_t)(P->pos - at);
      term = new_node(P, &g);
    }
    else if (c == ',' || c == '/' || c == ':' || c == ';' || c == '=') {
      P->pos++;
      gnode g;
      proto_init(&g, GN_LITERAL);
      g.lit = (uint32_t)(unsigned char)c;
      g.flags = GNF_NONNULL;
      g.src_off = (uint32_t)at;
      g.src_len = 1;
      term = new_node(P, &g);
    }
    else if (is_ident_start((unsigned char)c) && !(c == '-' && P->pos + 1 < P->n && is_digit(P->t[P->pos + 1]))) {
      size_t p = P->pos;
      while (p < P->n && is_ident_char((unsigned char)P->t[p])) {
        p++;
      }
      if (p < P->n && P->t[p] == '(') {
        if (!open_frame(P, F_FUNC, at, at, p - at)) {
          break;
        }
        P->pos = p + 1;
        continue;
      }
      P->pos = p;
      gnode g;
      proto_init(&g, GN_KEYWORD);
      g.str_off = (uint32_t)at;
      g.str_len = (uint32_t)(p - at);
      g.flags = GNF_NONNULL;
      g.src_off = (uint32_t)at;
      g.src_len = g.str_len;
      term = new_node(P, &g);
    }
    else {
      fail(P, GTEXT_CSS_E_GRAMMAR, at);
      break;
    }
    if (!ok(P)) {
      break;
    }
    term = read_multipliers(P, term, term_start, group);
    if (!ok(P) || !push_acc(P, term)) {
      break;
    }
  }
  if (!ok(P)) {
    return false;
  }
  if (P->nfr != 1) {
    fail(P, GTEXT_CSS_E_GRAMMAR, P->n);
    return false;
  }
  bool empty;
  const uint32_t root = close_contents(P, &P->fr[0], &empty);
  if (!ok(P)) {
    return false;
  }
  if (empty) {
    fail(P, GTEXT_CSS_E_GRAMMAR, 0);
    return false;
  }
  P->d->root = root;
  return true;
}

GTEXT_CSS_Status gtext_css_definition_parse(const char * text, size_t length,
    const GTEXT_CSS_Grammar_Options * options, GTEXT_CSS_Definition ** out,
    GTEXT_CSS_Grammar_Result * result) {
  if (!out || (!text && length)) {
    css_grammar_result(result, GTEXT_CSS_E_INVALID, 0, NULL, 0, 0);
    return GTEXT_CSS_E_INVALID;
  }
  size_t max_bytes, max_depth, max_nodes;
  css_grammar_limits(options, &max_bytes, &max_depth, &max_nodes);
  if (length > max_bytes) {
    css_grammar_result(result, GTEXT_CSS_E_LIMIT_DEFINITION, max_bytes, NULL, 0, 0);
    return GTEXT_CSS_E_LIMIT_DEFINITION;
  }
  GTEXT_Allocator alloc = css_grammar_allocator(options);
  GTEXT_CSS_Definition * d = gtext_allocator_malloc(&alloc, sizeof *d);
  if (!d) {
    css_grammar_result(result, GTEXT_CSS_E_OOM, 0, NULL, 0, 0);
    return GTEXT_CSS_E_OOM;
  }
  memset(d, 0, sizeof *d);
  d->alloc = alloc;
  d->max_nodes = max_nodes;
  d->text = gtext_allocator_malloc(&alloc, length + 1);
  if (!d->text) {
    gtext_allocator_free(&alloc, d);
    css_grammar_result(result, GTEXT_CSS_E_OOM, 0, NULL, 0, 0);
    return GTEXT_CSS_E_OOM;
  }
  if (length) {
    memcpy(d->text, text, length);
  }
  d->text[length] = '\0';
  d->text_len = length;
  parser P;
  memset(&P, 0, sizeof P);
  P.d = d;
  P.t = d->text;
  P.n = length;
  P.max_depth = max_depth;
  P.max_nodes = max_nodes;
  const bool good = parse_text(&P);
  gtext_allocator_free(&alloc, P.acc);
  gtext_allocator_free(&alloc, P.fr);
  if (!good) {
    const GTEXT_CSS_Status st = P.status == GTEXT_CSS_OK ? GTEXT_CSS_E_INTERNAL : P.status;
    const size_t nodes = d->nnodes;
    gtext_css_definition_free(d);
    css_grammar_result(result, st, P.err, NULL, 0, nodes);
    return st;
  }
  css_grammar_result(result, GTEXT_CSS_OK, 0, NULL, 0, d->nnodes);
  *out = d;
  return GTEXT_CSS_OK;
}
