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
 * The parser of media query lists (Media Queries 4, 3): <media-query-list>, <media-query>, <media-condition>, <media-in-parens>, <media-feature>
 * in the plain, boolean and range forms, and <general-enclosed>. It builds the flat node array of css_media_internal.h.
 *
 * No recursion: a parenthesized condition is a frame of an explicit stack. A `<media-in-parens>` that begins with `not`, a block or a function is
 * tried as a `( <media-condition> )` first; if that does not match, as a `( <media-feature> )`; and if neither does, it is `<general-enclosed>`
 * (Media Queries 4, 3: "the <general-enclosed> branch must only be chosen if the input does not match either of the preceding branches"). A frame
 * that fails is rolled back (its nodes, kids, values and calculations are dropped), and no block is parsed twice, so the cost is linear.
 */

#include <ghoti.io/text/css/css_condition.h>
#include <ghoti.io/text/css/css_container.h>
#include <ghoti.io/text/css/css_math.h>
#include <ghoti.io/text/css/css_media.h>
#include <ghoti.io/text/css/css_serialize.h>
#include <ghoti.io/text/css/css_value.h>

#include <string.h>

#include "css_media_internal.h"

/* ---- the generated table ---------------------------------------------- */

const css_mq_feature_row css_mq_feature_rows[CSS_MQF_COUNT] = {CSS_MQ_FEATURE_ROWS};
const char * const css_mq_keywords[CSS_MQ_KEYWORD_COUNT] = {CSS_MQ_KEYWORDS};
const css_mq_type_row css_mq_type_rows[] = {CSS_MQ_TYPE_ROWS};
const size_t css_mq_type_row_count = sizeof css_mq_type_rows / sizeof css_mq_type_rows[0];
static const char * const reserved_words[] = {CSS_MQ_RESERVED_WORDS};

static bool ieq(const char * a, size_t n, const char * lit) {
  size_t l = strlen(lit);
  if (n != l) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    if (css_mq_lower((unsigned char)a[i]) != (unsigned char)lit[i]) {
      return false;
    }
  }
  return true;
}

int css_mq_find_feature(const char * name, size_t length) {
  size_t lo = 0, hi = CSS_MQF_COUNT;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    const char * r = css_mq_feature_rows[mid].name;
    size_t rl = strlen(r);
    size_t n = rl < length ? rl : length;
    int c = 0;
    for (size_t i = 0; i < n && !c; i++) {
      c = (int)css_mq_lower((unsigned char)name[i]) - (int)(unsigned char)r[i];
    }
    if (!c) {
      c = length < rl ? -1 : (length > rl ? 1 : 0);
    }
    if (c == 0) {
      return (int)mid;
    }
    if (c < 0) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return -1;
}

int css_mq_find_keyword(int feature, const char * word, size_t length) {
  if (feature < 0 || feature >= CSS_MQF_COUNT) {
    return -1;
  }
  const css_mq_feature_row * r = &css_mq_feature_rows[feature];
  for (size_t i = 0; i < r->kw_count; i++) {
    if (ieq(word, length, css_mq_keywords[r->kw_first + i])) {
      return (int)i;
    }
  }
  return -1;
}

bool css_mq_is_reserved(const char * word, size_t length) {
  for (size_t i = 0; i < sizeof reserved_words / sizeof reserved_words[0]; i++) {
    if (ieq(word, length, reserved_words[i])) {
      return true;
    }
  }
  return false;
}

size_t gtext_css_media_feature_count(void) {
  return CSS_MQF_COUNT;
}

bool gtext_css_media_feature_row(size_t index, GTEXT_CSS_Media_Feature_Row * out) {
  if (index >= CSS_MQF_COUNT || !out) {
    return false;
  }
  const css_mq_feature_row * r = &css_mq_feature_rows[index];
  out->name = r->name;
  out->kind = r->kind == MQK_RANGE ? GTEXT_CSS_MEDIA_FEATURE_RANGE : GTEXT_CSS_MEDIA_FEATURE_DISCRETE;
  out->level = r->level;
  out->keyword_count = r->kw_count;
  out->implemented = r->status == MQS_IMPLEMENTED;
  return true;
}

int gtext_css_media_feature_find(const char * name, size_t length) {
  return name ? css_mq_find_feature(name, length) : -1;
}

/* ---- options and result ------------------------------------------------ */

GTEXT_CSS_Media_Options gtext_css_media_options_default(void) {
  GTEXT_CSS_Media_Options o;
  memset(&o, 0, sizeof o);
  o.max_queries = 65536;
  o.max_nodes = 1048576;
  o.max_depth = 256;
  o.max_diagnostics = 1024;
  return o;
}

struct GTEXT_CSS_Media_Result_Internal {
  GTEXT_Allocator alloc;
  GTEXT_CSS_Media_Diagnostic * d;
  size_t count, cap;
};

void gtext_css_media_result_free(GTEXT_CSS_Media_Result * result) {
  if (!result) {
    return;
  }
  if (result->internal) {
    GTEXT_Allocator a = result->internal->alloc;
    gtext_allocator_free(&a, result->internal->d);
    gtext_allocator_free(&a, result->internal);
  }
  memset(result, 0, sizeof *result);
}

size_t gtext_css_media_result_diagnostic_count(const GTEXT_CSS_Media_Result * result) {
  return result && result->internal ? result->internal->count : 0;
}

bool gtext_css_media_result_diagnostic(const GTEXT_CSS_Media_Result * result, size_t index, GTEXT_CSS_Media_Diagnostic * out) {
  if (!result || !result->internal || index >= result->internal->count || !out) {
    return false;
  }
  *out = result->internal->d[index];
  return true;
}

/* ---- parser state ------------------------------------------------------ */

typedef struct {
  const GTEXT_CSS_Component_Value * v;
  size_t n, pos;
  const GTEXT_CSS_Component_Value * owner; ///< The block this frame reads the contents of (NULL for a query's own tokens).
  uint32_t opbase;
  uint32_t nmark, kmark, vmark, mmark;
  uint8_t op;      ///< 0, MQN_AND or MQN_OR.
  bool is_not, allow_or, want_operand, mixed;
} frame;

typedef struct {
  GTEXT_CSS_Media_Options o;
  struct GTEXT_CSS_Media_List_Internal * in;
  GTEXT_CSS_Media_Result * res;
  GTEXT_Allocator alloc;
  GTEXT_CSS_Status status;
  GTEXT_CSS_Span failure;
  /* The query being parsed: set when it is invalid. */
  bool invalid;
  GTEXT_CSS_Media_Diag_Kind inv_kind;
  GTEXT_CSS_Span inv_span;
  const char * inv_ref;
  frame * frames;
  size_t nframes, framecap;
  uint32_t * ops;
  size_t nops, opcap;
  size_t replaced;
  size_t max_depth_seen;
  bool supports;   ///< Parsing a <supports-condition> and not a media query.
  bool bare_ok;    ///< ... and a declaration without parentheses is accepted.
  bool when;       ///< Parsing the <boolean-condition> of @when or @else: `media()` and `supports()` are the leaves.
  bool container;  ///< Parsing a <container-condition>: size features, `style()`, `scroll-state()` and `anchored()` are the leaves.
  uint8_t sub;     ///< Inside `style()`, `scroll-state()` or `anchored()`: MQ_SUB_*, so a parenthesized group is a feature of that function.
  bool nm_logical; ///< The last container feature name was `inline-size` or `block-size`.
} parser;

static void set_status(parser * P, GTEXT_CSS_Status st, GTEXT_CSS_Span span) {
  if (P->status == GTEXT_CSS_OK) {
    P->status = st;
    P->failure = span;
  }
}

static void diag(parser * P, GTEXT_CSS_Media_Diag_Kind kind, GTEXT_CSS_Span span, const char * ref) {
  if (!P->res) {
    return;
  }
  P->res->diagnostic_total++;
  struct GTEXT_CSS_Media_Result_Internal * ri = P->res->internal;
  if (!ri) {
    ri = gtext_allocator_calloc(&P->alloc, 1, sizeof *ri);
    if (!ri) {
      set_status(P, GTEXT_CSS_E_OOM, span);
      return;
    }
    ri->alloc = P->alloc;
    P->res->internal = ri;
  }
  if (ri->count >= P->o.max_diagnostics) {
    return;
  }
  if (ri->count == ri->cap) {
    size_t nc = ri->cap ? ri->cap * 2 : 8;
    GTEXT_CSS_Media_Diagnostic * nd = gtext_allocator_realloc(&P->alloc, ri->d, nc * sizeof *nd);
    if (!nd) {
      set_status(P, GTEXT_CSS_E_OOM, span);
      return;
    }
    ri->d = nd;
    ri->cap = nc;
  }
  ri->d[ri->count].kind = kind;
  ri->d[ri->count].span = span;
  ri->d[ri->count].reference = ref;
  ri->count++;
}

bool css_mq_grow(const GTEXT_Allocator * a, void ** array, size_t * cap, size_t count, size_t need, size_t elem) {
  if (count + need <= *cap) {
    return true;
  }
  size_t nc = *cap ? *cap : 16;
  while (nc < count + need) {
    if (nc > SIZE_MAX / 2 / elem) {
      return false;
    }
    nc *= 2;
  }
  void * p = gtext_allocator_realloc(a, *array, nc * elem);
  if (!p) {
    return false;
  }
  *array = p;
  *cap = nc;
  return true;
}

#define GROW(P, field, capf, countf, need, T) \
  css_mq_grow(&(P)->alloc, (void **)&(P)->in->field, &(P)->in->capf, (P)->in->countf, (need), sizeof(T))

static void invalid_query(parser * P, GTEXT_CSS_Media_Diag_Kind kind, GTEXT_CSS_Span span, const char * ref) {
  if (!P->invalid) {
    P->invalid = true;
    P->inv_kind = kind;
    P->inv_span = span;
    P->inv_ref = ref;
  }
}

static uint32_t new_node(parser * P, mq_node_kind kind, GTEXT_CSS_Span span) {
  struct GTEXT_CSS_Media_List_Internal * in = P->in;
  if (in->node_count >= P->o.max_nodes) {
    set_status(P, GTEXT_CSS_E_LIMIT_CONDITIONS, span);
    return MQ_NONE;
  }
  if (!GROW(P, nodes, node_cap, node_count, 1, mq_node)) {
    set_status(P, GTEXT_CSS_E_OOM, span);
    return MQ_NONE;
  }
  mq_node * n = &in->nodes[in->node_count];
  memset(n, 0, sizeof *n);
  n->kind = (uint8_t)kind;
  n->feature = -1;
  n->kid_first = MQ_NONE;
  n->v1 = n->v2 = MQ_NONE;
  n->span = span;
  return (uint32_t)in->node_count++;
}

static uint32_t new_value(parser * P, GTEXT_CSS_Span span) {
  struct GTEXT_CSS_Media_List_Internal * in = P->in;
  if (!GROW(P, values, value_cap, value_count, 1, mq_value)) {
    set_status(P, GTEXT_CSS_E_OOM, span);
    return MQ_NONE;
  }
  memset(&in->values[in->value_count], 0, sizeof(mq_value));
  return (uint32_t)in->value_count++;
}

/// Attach `count` kids (copied from `kids`) to node `n`.
static bool set_kids(parser * P, uint32_t n, const uint32_t * kids, size_t count) {
  struct GTEXT_CSS_Media_List_Internal * in = P->in;
  if (!GROW(P, kids, kid_cap, kid_count, count, uint32_t)) {
    set_status(P, GTEXT_CSS_E_OOM, in->nodes[n].span);
    return false;
  }
  in->nodes[n].kid_first = (uint32_t)in->kid_count;
  in->nodes[n].kid_count = (uint32_t)count;
  if (count) {
    memcpy(&in->kids[in->kid_count], kids, count * sizeof *kids);
  }
  in->kid_count += count;
  return true;
}

/// Drop the nodes from `from` on, letting go of what they own.
static void drop_nodes(struct GTEXT_CSS_Media_List_Internal * in, size_t from) {
  for (size_t i = from; i < in->node_count; i++) {
    if (in->nodes[i].kind == MQN_SELECTOR) {
      gtext_css_selector_list_free(in->nodes[i].sel);
      in->nodes[i].sel = NULL;
    }
  }
  in->node_count = from;
}

static void rollback(parser * P, const frame * f) {
  struct GTEXT_CSS_Media_List_Internal * in = P->in;
  drop_nodes(in, f->nmark);
  in->kid_count = f->kmark;
  in->value_count = f->vmark;
  while (in->math_count > f->mmark) {
    gtext_css_math_free(in->maths[--in->math_count]);
  }
  P->nops = f->opbase;
}

/* ---- tokens ------------------------------------------------------------ */

static bool is_ws(const GTEXT_CSS_Component_Value * v) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_WHITESPACE;
}

static bool is_ident(const GTEXT_CSS_Component_Value * v) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_IDENT;
}

static bool is_ident_lit(const GTEXT_CSS_Component_Value * v, const char * lit) {
  return is_ident(v) && ieq(v->value.data, v->value.length, lit);
}

static bool is_delim(const GTEXT_CSS_Component_Value * v, uint32_t c) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_DELIM && v->delim == c;
}

static bool is_paren_block(const GTEXT_CSS_Component_Value * v) {
  return v->kind == GTEXT_CSS_VALUE_BLOCK && v->type == GTEXT_CSS_TOKEN_OPEN_PAREN;
}

static size_t skip_ws(const GTEXT_CSS_Component_Value * v, size_t n, size_t pos) {
  while (pos < n && is_ws(&v[pos])) {
    pos++;
  }
  return pos;
}

/// The number of non-ws tokens at the end trimmed: the end of the significant part.
static size_t trim_end(const GTEXT_CSS_Component_Value * v, size_t from, size_t n) {
  while (n > from && is_ws(&v[n - 1])) {
    n--;
  }
  return n;
}

/* ---- values ------------------------------------------------------------ */

static const char * const REF_SYNTAX = "Media Queries 4, 3.2";
/// The serial numbers of lists (css_media_internal.h, `serial`): a counter of the process, zero at load.
static GCU_Atomic_Size css_mq_list_serial;

static const char * const REF_VALUE = "Media Queries 4, 3.2 (a value that does not match the feature's syntax)";
static const char * const REF_PREFIX = "Media Queries 4, 2.4.4";
static const char * const REF_RANGE = "Media Queries 4, 2.4.3";
static const char * const REF_TYPE = "Media Queries 4, 3";
static const char * const REF_MIXED = "Media Queries 4, 2.5";
static const char * const REF_UNKNOWN = "Media Queries 4, 3.2 (an unknown feature)";

typedef enum { VAL_OK = 0, VAL_BAD } val_result;


/// Read a math function as a number-valued operand (or whatever `want` says); on success the calculation is owned by the list.
static GTEXT_CSS_Math * read_math(parser * P, const GTEXT_CSS_Component_Value * t, GTEXT_CSS_Math_Category want, bool * hard) {
  bool valid = false;
  GTEXT_CSS_Math * m = NULL;
  GTEXT_CSS_Math_Options mo = gtext_css_math_options_default();
  mo.allocator = &P->alloc;
  GTEXT_CSS_Status st = gtext_css_math_parse(t, NULL, &mo, &valid, &m, NULL);
  if (st != GTEXT_CSS_OK) {
    set_status(P, st, t->span);
    *hard = true;
    return NULL;
  }
  if (!valid) {
    return NULL;
  }
  if (m->category != want || m->has_percent) {
    gtext_css_math_free(m);
    return NULL;
  }
  if (!GROW(P, maths, math_cap, math_count, 1, GTEXT_CSS_Math *)) {
    gtext_css_math_free(m);
    set_status(P, GTEXT_CSS_E_OOM, t->span);
    *hard = true;
    return NULL;
  }
  P->in->maths[P->in->math_count++] = m;
  return m;
}

static bool is_math_fn(const GTEXT_CSS_Component_Value * t) {
  return t->kind == GTEXT_CSS_VALUE_FUNCTION && gtext_css_math_is_function(t->value.data, t->value.length);
}

/// `<ratio> = <number [0,inf]> [ / <number [0,inf]> ]?`, each number a number token or a calculation.
static val_result parse_ratio(parser * P, const GTEXT_CSS_Component_Value * v, size_t n, mq_value * val) {
  size_t i = 0;
  bool hard = false;
  GTEXT_CSS_Math * parts[2] = {NULL, NULL};
  double nums[2] = {0, 1};
  for (int k = 0; k < 2; k++) {
    if (i >= n) {
      return VAL_BAD;
    }
    const GTEXT_CSS_Component_Value * t = &v[i];
    if (is_math_fn(t)) {
      parts[k] = read_math(P, t, GTEXT_CSS_MATH_CAT_NUMBER, &hard);
      if (!parts[k]) {
        return VAL_BAD;
      }
    } else if (t->kind == GTEXT_CSS_VALUE_TOKEN && t->type == GTEXT_CSS_TOKEN_NUMBER && t->number >= 0 && !mq_signbit(t->number)) {
      nums[k] = t->number;
    } else {
      return VAL_BAD;
    }
    i = skip_ws(v, n, i + 1);
    if (k == 0) {
      if (i >= n) {
        break;
      }
      if (!is_delim(&v[i], '/')) {
        return VAL_BAD;
      }
      i = skip_ws(v, n, i + 1);
    }
  }
  if (i < n) {
    return VAL_BAD;
  }
  val->kind = MQV_RATIO;
  val->a = nums[0];
  val->b = nums[1];
  val->math = parts[0];
  val->math2 = parts[1];
  return VAL_OK;
}

/// Parse the tokens [v, v+n) (no leading or trailing white space) as the value of a feature of unit `unit`.
static val_result parse_value(parser * P, int feature, const GTEXT_CSS_Component_Value * v, size_t n, uint32_t * out) {
  const css_mq_feature_row * row = &css_mq_feature_rows[feature];
  if (n == 0) {
    return VAL_BAD;
  }
  uint32_t vi = new_value(P, v[0].span);
  if (vi == MQ_NONE) {
    return VAL_BAD;
  }
  mq_value * val = &P->in->values[vi];
  *out = vi;
  const GTEXT_CSS_Component_Value * t = &v[0];
  if (row->unit == MQU_RATIO) {
    return parse_ratio(P, v, n, val);
  }
  bool is_math_fn = t->kind == GTEXT_CSS_VALUE_FUNCTION && gtext_css_math_is_function(t->value.data, t->value.length);
  /* A calculation: one function, nothing else. */
  if (is_math_fn) {
    if (n != 1) {
      return VAL_BAD;
    }
    GTEXT_CSS_Math * m = NULL;
    bool valid = false;
    GTEXT_CSS_Math_Options mo = gtext_css_math_options_default();
    mo.allocator = &P->alloc;
    GTEXT_CSS_Status st = gtext_css_math_parse(t, NULL, &mo, &valid, &m, NULL);
    if (st != GTEXT_CSS_OK) {
      set_status(P, st, t->span);
      return VAL_BAD;
    }
    if (!valid) {
      return VAL_BAD;
    }
    GTEXT_CSS_Math_Category cat = m->category;
    bool ok;
    switch (row->unit) {
    case MQU_LENGTH: ok = cat == GTEXT_CSS_MATH_CAT_LENGTH; break;
    case MQU_RESOLUTION: ok = cat == GTEXT_CSS_MATH_CAT_RESOLUTION; break;
    case MQU_INTEGER:
    case MQU_BOOLEAN:
    case MQU_NUMBER: ok = cat == GTEXT_CSS_MATH_CAT_NUMBER; break;
    default: ok = false; break;
    }
    if (!ok || m->has_percent) {
      gtext_css_math_free(m);
      return VAL_BAD;
    }
    if (!GROW(P, maths, math_cap, math_count, 1, GTEXT_CSS_Math *)) {
      gtext_css_math_free(m);
      set_status(P, GTEXT_CSS_E_OOM, t->span);
      return VAL_BAD;
    }
    P->in->maths[P->in->math_count++] = m;
    val->kind = MQV_MATH;
    val->math = m;
    val->math_cat = (uint8_t)cat;
    return VAL_OK;
  }
  if (t->kind != GTEXT_CSS_VALUE_TOKEN) {
    return VAL_BAD;
  }
  switch (row->unit) {
  case MQU_LENGTH:
    if (n != 1) {
      return VAL_BAD;
    }
    if (t->type == GTEXT_CSS_TOKEN_NUMBER && t->number == 0) {
      val->kind = MQV_NUMBER;
      val->a = 0;
      return VAL_OK;
    }
    if (t->type == GTEXT_CSS_TOKEN_DIMENSION) {
      GTEXT_CSS_Unit u = gtext_css_unit_lookup(t->unit.data, t->unit.length);
      if (u != GTEXT_CSS_UNIT_NONE && gtext_css_unit_type(u) == GTEXT_CSS_UNIT_TYPE_LENGTH) {
        val->kind = MQV_DIMENSION;
        val->unit = u;
        val->a = t->number;
        return VAL_OK;
      }
    }
    return VAL_BAD;
  case MQU_RESOLUTION:
    if (n != 1) {
      return VAL_BAD;
    }
    if (t->type == GTEXT_CSS_TOKEN_IDENT && ieq(t->value.data, t->value.length, "infinite")) {
      val->kind = MQV_INFINITE;
      return VAL_OK;
    }
    if (t->type == GTEXT_CSS_TOKEN_DIMENSION) {
      GTEXT_CSS_Unit u = gtext_css_unit_lookup(t->unit.data, t->unit.length);
      /* `<resolution [0,inf]>`: a negative resolution is not a value (both browsers refuse `(min-resolution: -1x)`; a length may be negative). */
      if (u != GTEXT_CSS_UNIT_NONE && gtext_css_unit_type(u) == GTEXT_CSS_UNIT_TYPE_RESOLUTION && !(t->number < 0)) {
        val->kind = MQV_DIMENSION;
        val->unit = u;
        val->a = t->number;
        return VAL_OK;
      }
    }
    return VAL_BAD;
  case MQU_INTEGER:
    if (n == 1 && t->type == GTEXT_CSS_TOKEN_NUMBER && (t->flags & GTEXT_CSS_FLAG_INTEGER)) {
      val->kind = MQV_NUMBER;
      val->integer = true;
      val->a = t->number;
      return VAL_OK;
    }
    return VAL_BAD;
  case MQU_BOOLEAN:
    if (n == 1 && t->type == GTEXT_CSS_TOKEN_NUMBER && (t->flags & GTEXT_CSS_FLAG_INTEGER) && (t->number == 0 || t->number == 1)) {
      val->kind = MQV_NUMBER;
      val->integer = true;
      val->a = t->number;
      return VAL_OK;
    }
    return VAL_BAD;
  case MQU_NUMBER:
    if (n == 1 && t->type == GTEXT_CSS_TOKEN_NUMBER) {
      val->kind = MQV_NUMBER;
      val->a = t->number;
      return VAL_OK;
    }
    return VAL_BAD;
  case MQU_KEYWORD: {
    if (n != 1 || t->type != GTEXT_CSS_TOKEN_IDENT) {
      return VAL_BAD;
    }
    int k = css_mq_find_keyword(feature, t->value.data, t->value.length);
    if (k < 0) {
      return VAL_BAD;
    }
    val->kind = MQV_IDENT;
    val->keyword = (uint16_t)k;
    return VAL_OK;
  }
  default:
    return VAL_BAD;
  }
}

/* ---- the enclosed node ------------------------------------------------- */

/// Whether the contents of a block or function are an `<any-value>?` (Syntax 3, 5.4.7): "any sequence of one or more tokens, so long as it does not contain
/// <bad-string-token>, <bad-url-token>, unmatched <)-token>, <]-token>, or <}-token>". (A `;` or a `!` is allowed; they are not in <declaration-value>.) The
/// walk uses its own stack: the nesting of the input costs memory, not the C stack. Returns false on a hard failure too, with the status set.
static bool any_value_ok(parser * P, const GTEXT_CSS_Component_Value * v, size_t n) {
  typedef struct {
    const GTEXT_CSS_Component_Value * v;
    size_t n;
  } span_t;
  size_t cap = 16, top = 0;
  span_t * st = gtext_allocator_malloc(&P->alloc, cap * sizeof *st);
  if (!st) {
    set_status(P, GTEXT_CSS_E_OOM, (GTEXT_CSS_Span){0});
    return false;
  }
  st[top++] = (span_t){v, n};
  bool ok = true;
  while (top > 0 && ok) {
    const span_t f = st[--top];
    for (size_t i = 0; i < f.n && ok; i++) {
      const GTEXT_CSS_Component_Value * c = &f.v[i];
      if (c->kind == GTEXT_CSS_VALUE_TOKEN) {
        switch (c->type) {
        case GTEXT_CSS_TOKEN_BAD_STRING:
        case GTEXT_CSS_TOKEN_BAD_URL:
        case GTEXT_CSS_TOKEN_CLOSE_PAREN:
        case GTEXT_CSS_TOKEN_CLOSE_SQUARE:
        case GTEXT_CSS_TOKEN_CLOSE_CURLY: ok = false; break;
        default: break;
        }
      } else if (c->child_count > 0) {
        if (top == cap) {
          span_t * g = gtext_allocator_realloc(&P->alloc, st, cap * 2 * sizeof *st);
          if (!g) {
            set_status(P, GTEXT_CSS_E_OOM, c->span);
            gtext_allocator_free(&P->alloc, st);
            return false;
          }
          st = g;
          cap *= 2;
        }
        st[top++] = (span_t){c->children, c->child_count};
      }
    }
  }
  gtext_allocator_free(&P->alloc, st);
  return ok;
}

/// A `<general-enclosed>` leaf. MQ_NONE when its contents are not an <any-value> (the condition is then invalid, as it is in browsers: `not (foo: red})` is not
/// `not <unknown>`) or on a failure.
static uint32_t make_enclosed(parser * P, const GTEXT_CSS_Component_Value * src) {
  if (!any_value_ok(P, src->children, src->child_count)) {
    if (P->status == GTEXT_CSS_OK) {
      invalid_query(P, GTEXT_CSS_MQDIAG_SYNTAX, src->span, "Conditional 3, 3 (<general-enclosed> holds an <any-value>, which has no bad string, no bad url and no unmatched closing token)");
    }
    return MQ_NONE;
  }
  uint32_t n = new_node(P, MQN_ENCLOSED, src->span);
  if (n != MQ_NONE) {
    P->in->nodes[n].src = src;
    P->in->nodes[n].flags = P->supports ? MQF_FALSE : 0;
  }
  return n;
}

/* ---- the feature --------------------------------------------------------- */

typedef enum { FEAT_OK = 0, FEAT_SOFT, FEAT_INVALID } feat_result;

typedef struct {
  size_t start, end; ///< Token range of a group (trimmed of white space).
} grp;

/// Name of a feature token: a single ident; fills row (or -1), prefix and whether it is dashed.
static bool name_of(const GTEXT_CSS_Component_Value * t, int * row, mq_prefix * prefix, bool * known_via_prefix) {
  *row = -1;
  *prefix = MQP_NONE;
  *known_via_prefix = false;
  if (!is_ident(t)) {
    return false;
  }
  const char * s = t->value.data;
  size_t l = t->value.length;
  int r = css_mq_find_feature(s, l);
  if (r >= 0) {
    *row = r;
    return true;
  }
  if (l > 4 && s[3] == '-' && ((css_mq_lower((unsigned char)s[0]) == 'm' && css_mq_lower((unsigned char)s[1]) == 'i' && css_mq_lower((unsigned char)s[2]) == 'n') ||
                               (css_mq_lower((unsigned char)s[0]) == 'm' && css_mq_lower((unsigned char)s[1]) == 'a' && css_mq_lower((unsigned char)s[2]) == 'x'))) {
    r = css_mq_find_feature(s + 4, l - 4);
    /* `min-` and `max-` go before a standard feature name: the vendor features have their own spellings (`-webkit-min-device-pixel-ratio`, below), and
       `min--webkit-device-pixel-ratio` is no feature (Compatibility Standard, 4.3). */
    if (r >= 0 && css_mq_feature_rows[r].name[0] != '-') {
      *row = r;
      *prefix = css_mq_lower((unsigned char)s[1]) == 'i' ? MQP_MIN : MQP_MAX;
      *known_via_prefix = true;
      return true;
    }
  }
  /* The legacy spelling of the prefixed forms of the one non-standard range feature. */
  if (ieq(s, l, "-webkit-min-device-pixel-ratio") || ieq(s, l, "-webkit-max-device-pixel-ratio")) {
    r = css_mq_find_feature("-webkit-device-pixel-ratio", 26);
    if (r >= 0) {
      *row = r;
      *prefix = css_mq_lower((unsigned char)s[9]) == 'i' ? MQP_MIN : MQP_MAX;
      *known_via_prefix = true;
    }
  }
  return true;
}

static bool feature_name(parser * P, const GTEXT_CSS_Component_Value * t, int * row, mq_prefix * prefix, bool * via);

/// Parse the contents [v, v+n) of a parenthesized block as `<media-feature>`; on FEAT_OK `*out` is the FEATURE or CUSTOM node.
static feat_result parse_feature_core(parser * P, const GTEXT_CSS_Component_Value * block, uint32_t * out) {
  if (P->supports) {
    return FEAT_SOFT; /* a supports condition has no media features: a declaration is read before this */
  }
  const GTEXT_CSS_Component_Value * v = block->children;
  size_t n = block->child_count;
  size_t s = skip_ws(v, n, 0);
  size_t e = trim_end(v, s, n);
  if (s >= e) {
    return FEAT_SOFT; /* `()` is <general-enclosed> */
  }
  /* Split at the comparison operators. */
  grp groups[3];
  mq_op ops[2];
  size_t ng = 0, nop = 0;
  size_t gstart = s;
  for (size_t i = s; i < e; i++) {
    const GTEXT_CSS_Component_Value * t = &v[i];
    if (t->kind == GTEXT_CSS_VALUE_TOKEN && t->type == GTEXT_CSS_TOKEN_DELIM && (t->delim == '<' || t->delim == '>' || t->delim == '=')) {
      mq_op op;
      size_t w = i + 1;
      if (t->delim == '=') {
        op = MQOP_EQ;
      } else {
        bool le = i + 1 < e && is_delim(&v[i + 1], '=');
        op = t->delim == '<' ? (le ? MQOP_LE : MQOP_LT) : (le ? MQOP_GE : MQOP_GT);
        if (le) {
          w = i + 2;
        }
      }
      if (nop == 2 || ng >= 3) {
        return FEAT_SOFT;
      }
      size_t ge = trim_end(v, gstart, i);
      groups[ng].start = skip_ws(v, ge, gstart);
      groups[ng].end = ge;
      ng++;
      ops[nop++] = op;
      i = w - 1;
      gstart = w;
    }
  }
  if (ng >= 3) {
    return FEAT_SOFT;
  }
  {
    size_t ge = trim_end(v, gstart, e);
    groups[ng].start = skip_ws(v, ge, gstart);
    groups[ng].end = ge;
    ng++;
  }
  if (nop == 0) {
    /* Plain: `name : value`, or boolean: `name`. */
    size_t i = groups[0].start;
    if (!is_ident(&v[i])) {
      return FEAT_SOFT;
    }
    int row;
    mq_prefix prefix;
    bool via;
    feature_name(P, &v[i], &row, &prefix, &via);
    size_t after = skip_ws(v, e, i + 1);
    bool boolean = after >= e;
    if (!boolean && v[after].type != GTEXT_CSS_TOKEN_COLON) {
      return FEAT_SOFT;
    }
    if (!boolean && v[after].kind != GTEXT_CSS_VALUE_TOKEN) {
      return FEAT_SOFT;
    }
    if (row < 0) {
      /* An unknown name. `(--name)` is a reference to a @custom-media. */
      if (boolean && !P->container && v[i].value.length > 2 && v[i].value.data[0] == '-' && v[i].value.data[1] == '-') {
        uint32_t nd = new_node(P, MQN_CUSTOM, block->span);
        if (nd == MQ_NONE) {
          return FEAT_INVALID;
        }
        char * txt = css_arena_strdup(&P->in->arena, v[i].value.data, v[i].value.length);
        if (!txt) {
          set_status(P, GTEXT_CSS_E_OOM, block->span);
          return FEAT_INVALID;
        }
        P->in->nodes[nd].text = txt;
        P->in->nodes[nd].text_length = v[i].value.length;
        *out = nd;
        return FEAT_OK;
      }
      return FEAT_SOFT;
    }
    const css_mq_feature_row * fr = &css_mq_feature_rows[row];
    if (prefix != MQP_NONE && fr->kind != MQK_RANGE) {
      invalid_query(P, GTEXT_CSS_MQDIAG_FEATURE, v[i].span, REF_PREFIX);
      return FEAT_INVALID;
    }
    if (boolean && prefix != MQP_NONE) {
      invalid_query(P, GTEXT_CSS_MQDIAG_FEATURE, v[i].span, REF_PREFIX);
      return FEAT_INVALID;
    }
    uint32_t nd = new_node(P, MQN_FEATURE, block->span);
    if (nd == MQ_NONE) {
      return FEAT_INVALID;
    }
    mq_node * node = &P->in->nodes[nd];
    node->feature = (int16_t)row;
    node->prefix = (uint8_t)prefix;
    node->flags = P->nm_logical ? MQF_LOGICAL : 0;
    if (boolean) {
      node->form = MQFORM_BOOLEAN;
      *out = nd;
      return FEAT_OK;
    }
    node->form = MQFORM_PLAIN;
    size_t vs = skip_ws(v, e, after + 1);
    uint32_t vi;
    if (vs >= e || parse_value(P, row, &v[vs], e - vs, &vi) != VAL_OK) {
      if (P->status != GTEXT_CSS_OK) {
        return FEAT_INVALID;
      }
      invalid_query(P, GTEXT_CSS_MQDIAG_VALUE, vs < e ? v[vs].span : block->span, REF_VALUE);
      return FEAT_INVALID;
    }
    P->in->nodes[nd].v1 = vi;
    *out = nd;
    return FEAT_OK;
  }
  /* Range forms. */
  for (size_t g = 0; g < ng; g++) {
    if (groups[g].start >= groups[g].end) {
      return FEAT_SOFT;
    }
  }
  int name_g = -1; /* the group that is the name */
  int row = -1;
  mq_prefix prefix = MQP_NONE;
  if (ng == 2) {
    for (int g = 0; g < 2; g++) {
      grp q = groups[g];
      if (q.end - q.start == 1 && is_ident(&v[q.start])) {
        int r;
        mq_prefix px;
        bool via;
        feature_name(P, &v[q.start], &r, &px, &via);
        if (r >= 0) {
          name_g = g;
          row = r;
          prefix = px;
          break;
        }
      }
    }
    if (name_g < 0) {
      return FEAT_SOFT;
    }
  } else {
    grp q = groups[1];
    if (q.end - q.start != 1 || !is_ident(&v[q.start])) {
      return FEAT_SOFT;
    }
    bool via;
    feature_name(P, &v[q.start], &row, &prefix, &via);
    if (row < 0) {
      return FEAT_SOFT;
    }
    name_g = 1;
  }
  const css_mq_feature_row * fr = &css_mq_feature_rows[row];
  GTEXT_CSS_Span here = v[groups[name_g].start].span;
  if (prefix != MQP_NONE || fr->kind != MQK_RANGE) {
    invalid_query(P, GTEXT_CSS_MQDIAG_FEATURE, here, prefix != MQP_NONE ? REF_PREFIX : REF_RANGE);
    return FEAT_INVALID;
  }
  if (ng == 3) {
    bool lt = (ops[0] == MQOP_LT || ops[0] == MQOP_LE) && (ops[1] == MQOP_LT || ops[1] == MQOP_LE);
    bool gt = (ops[0] == MQOP_GT || ops[0] == MQOP_GE) && (ops[1] == MQOP_GT || ops[1] == MQOP_GE);
    if (!lt && !gt) {
      invalid_query(P, GTEXT_CSS_MQDIAG_FEATURE, here, REF_RANGE);
      return FEAT_INVALID;
    }
  }
  uint32_t nd = new_node(P, MQN_FEATURE, block->span);
  if (nd == MQ_NONE) {
    return FEAT_INVALID;
  }
  P->in->nodes[nd].feature = (int16_t)row;
  P->in->nodes[nd].flags = P->nm_logical ? MQF_LOGICAL : 0;
  P->in->nodes[nd].form = MQFORM_RANGE;
  P->in->nodes[nd].op1 = (uint8_t)ops[0];
  P->in->nodes[nd].op2 = ng == 3 ? (uint8_t)ops[1] : (uint8_t)MQOP_NONE;
  P->in->nodes[nd].pos = ng == 3 ? MQPOS_MIDDLE : (name_g == 0 ? MQPOS_LEFT : MQPOS_RIGHT);
  uint32_t vals[2] = {MQ_NONE, MQ_NONE};
  size_t nv = 0;
  for (size_t g = 0; g < ng; g++) {
    if ((int)g == name_g) {
      continue;
    }
    uint32_t vi;
    if (parse_value(P, row, &v[groups[g].start], groups[g].end - groups[g].start, &vi) != VAL_OK) {
      if (P->status != GTEXT_CSS_OK) {
        return FEAT_INVALID;
      }
      invalid_query(P, GTEXT_CSS_MQDIAG_VALUE, v[groups[g].start].span, REF_VALUE);
      return FEAT_INVALID;
    }
    vals[nv++] = vi;
  }
  /* A single `=` is equality in the range form; a double range may not use it. */
  if (ng == 3 && (ops[0] == MQOP_EQ || ops[1] == MQOP_EQ)) {
    invalid_query(P, GTEXT_CSS_MQDIAG_FEATURE, here, REF_RANGE);
    return FEAT_INVALID;
  }
  P->in->nodes[nd].v1 = vals[0];
  P->in->nodes[nd].v2 = vals[1];
  *out = nd;
  return FEAT_OK;
}

/// The name of a feature token in the current mode. A container condition has the size features (`width`, `height`, `aspect-ratio`, `orientation`, and the logical
/// `inline-size` and `block-size`, which are the width and height rows with MQF_LOGICAL) and no others: any other name is unknown there.
static bool feature_name(parser * P, const GTEXT_CSS_Component_Value * t, int * row, mq_prefix * prefix, bool * via) {
  P->nm_logical = false;
  const bool ident = name_of(t, row, prefix, via);
  if (!P->container || !ident) {
    return ident;
  }
  const char * s = t->value.data;
  const size_t l = t->value.length;
  const char * base = s;
  size_t bl = l;
  mq_prefix px = MQP_NONE;
  if (l > 4 && s[3] == '-' && ((css_mq_lower((unsigned char)s[0]) == 'm' && css_mq_lower((unsigned char)s[1]) == 'i' && css_mq_lower((unsigned char)s[2]) == 'n') ||
                               (css_mq_lower((unsigned char)s[0]) == 'm' && css_mq_lower((unsigned char)s[1]) == 'a' && css_mq_lower((unsigned char)s[2]) == 'x'))) {
    px = css_mq_lower((unsigned char)s[1]) == 'i' ? MQP_MIN : MQP_MAX;
    base = s + 4;
    bl = l - 4;
  }
  /* the features a container condition has are the rows of the generated table (tools/css/atrules-supplement.tsv, `container-feature`); any other name is unknown there */
  static const css_cq_feature_row cq_features[] = {CSS_CQ_FEATURE_ROWS};
  for (size_t i = 0; i < sizeof cq_features / sizeof cq_features[0]; i++) {
    if (ieq(base, bl, cq_features[i].name)) {
      *row = cq_features[i].row;
      *prefix = px;
      *via = px != MQP_NONE;
      P->nm_logical = cq_features[i].logical != 0;
      return true;
    }
  }
  *row = -1;
  *prefix = MQP_NONE;
  *via = false;
  return true;
}

static feat_result parse_sub_feature(parser * P, const GTEXT_CSS_Component_Value * v, size_t n, GTEXT_CSS_Span span, uint32_t * out);

/// Forget the nodes, kids, values and calculations made since the marks.
static void truncate_to(parser * P, size_t nmark, size_t kmark, size_t vmark, size_t mmark) {
  struct GTEXT_CSS_Media_List_Internal * in = P->in;
  drop_nodes(in, nmark);
  in->kid_count = kmark;
  in->value_count = vmark;
  while (in->math_count > mmark) {
    gtext_css_math_free(in->maths[--in->math_count]);
  }
}

/// Parse the contents of a parenthesized block as a feature of the current mode. FEAT_INVALID only for a failure of the parse itself: an invalid
/// container feature is unknown (general-enclosed), a media feature that is invalid replaces its query.
static feat_result parse_feature(parser * P, const GTEXT_CSS_Component_Value * block, uint32_t * out) {
  if (P->sub) {
    return parse_sub_feature(P, block->children, block->child_count, block->span, out);
  }
  if (P->when) {
    return FEAT_SOFT;
  }
  struct GTEXT_CSS_Media_List_Internal * in = P->in;
  const size_t nmark = in->node_count, kmark = in->kid_count, vmark = in->value_count, mmark = in->math_count;
  feat_result fr = parse_feature_core(P, block, out);
  /* A known media feature written wrongly (a bad value, a prefix on a discrete feature, a mixed range) is one unknown leaf, as it is in both
     browsers (Media Queries 4, 3.2 and the three-valued logic of 3.4: `(color: foo) or (color)` is true), and is written back as it was written.
     Only an error of the query's own grammar still replaces the query by `not all`. */
  if (fr == FEAT_INVALID && P->status == GTEXT_CSS_OK) {
    P->invalid = false;
    truncate_to(P, nmark, kmark, vmark, mmark);
    return FEAT_SOFT;
  }
  return fr;
}

/* ---- @supports leaves ----------------------------------------------------- */

static bool is_colon(const GTEXT_CSS_Component_Value * v) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_COLON;
}

/// Whether tokens [v, v+n) read `ident :`.
static bool declaration_shaped(const GTEXT_CSS_Component_Value * v, size_t n) {
  size_t s = skip_ws(v, n, 0);
  if (s >= n || !is_ident(&v[s]) || is_ident_lit(&v[s], "not")) {
    return false;
  }
  size_t c = skip_ws(v, n, s + 1);
  return c < n && is_colon(&v[c]);
}

/// A copy of `len` bytes in the list's arena, or NULL.
static char * arena_copy(parser * P, const char * s, size_t len) {
  char * c = css_arena_strdup(&P->in->arena, s, len);
  if (!c) {
    set_status(P, GTEXT_CSS_E_OOM, (GTEXT_CSS_Span){0});
  }
  return c;
}

/// Serialize [v, v+n) (trimmed of white space) into the arena; *len receives the length.
static char * serialize_trimmed(parser * P, const GTEXT_CSS_Component_Value * v, size_t n, size_t * len) {
  size_t s = skip_ws(v, n, 0);
  size_t e = trim_end(v, s, n);
  GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
  so.allocator = &P->alloc;
  char * buf = NULL;
  size_t bl = 0;
  GTEXT_CSS_Status st = gtext_css_serialize_values_buffer(v + s, e - s, &so, &buf, &bl);
  if (st != GTEXT_CSS_OK) {
    set_status(P, st, (GTEXT_CSS_Span){0});
    return NULL;
  }
  char * c = arena_copy(P, buf, bl);
  gtext_css_serialize_buffer_free(&so, buf);
  *len = bl;
  return c;
}

/// `(property: value [!important])`.
static uint32_t make_decl(parser * P, const GTEXT_CSS_Component_Value * v, size_t n, GTEXT_CSS_Span span) {
  /* A declaration that is not even an <any-value> (an unmatched `}`, a bad string or url) is no <supports-decl> and no <general-enclosed> either: the
     condition is invalid. */
  if (!any_value_ok(P, v, n)) {
    if (P->status == GTEXT_CSS_OK) {
      invalid_query(P, GTEXT_CSS_MQDIAG_SYNTAX, span, "Conditional 3, 3 (a declaration is a <general-enclosed> when it is not valid, and that holds an <any-value>)");
    }
    return MQ_NONE;
  }
  size_t s = skip_ws(v, n, 0);
  size_t e = trim_end(v, s, n);
  size_t c = skip_ws(v, e, s + 1);
  size_t vs = skip_ws(v, e, c + 1);
  bool important = false;
  /* A trailing `! important` is accepted and ignored (Conditional 3: the declaration is parsed as the cascade parses it). */
  size_t ve = e;
  if (ve > vs && is_ident_lit(&v[ve - 1], "important")) {
    size_t b = ve - 1;
    while (b > vs && is_ws(&v[b - 1])) {
      b--;
    }
    if (b > vs && is_delim(&v[b - 1], '!')) {
      important = true;
      ve = trim_end(v, vs, b - 1);
    }
  }
  uint32_t nd = new_node(P, MQN_DECL, span);
  if (nd == MQ_NONE) {
    return MQ_NONE;
  }
  char * name = arena_copy(P, v[s].value.data, v[s].value.length);
  size_t vl = 0;
  char * val = serialize_trimmed(P, v + vs, ve > vs ? ve - vs : 0, &vl);
  if (!name || !val) {
    return MQ_NONE;
  }
  mq_node * node = &P->in->nodes[nd];
  node->text = name;
  node->text_length = v[s].value.length;
  node->value = val;
  node->value_length = vl;
  node->flags = important ? MQF_IMPORTANT : 0;
  return nd;
}

static const char * const tech_names[] = {"features-opentype", "features-aat", "features-graphite", "color-colrv0", "color-colrv1", "color-svg", "color-sbix", "color-cbdt",
    "variations", "palettes", "incremental"};
static const char * const format_names[] = {"collection", "embedded-opentype", "opentype", "svg", "truetype", "woff", "woff2"};

static int lookup_name(const char * const * names, size_t count, const GTEXT_CSS_String * w) {
  for (size_t i = 0; i < count; i++) {
    if (ieq(w->data, w->length, names[i])) {
      return (int)i;
    }
  }
  return -1;
}

/// A function of a supports condition. FEAT_SOFT: <general-enclosed>.
static feat_result parse_supports_function(parser * P, const GTEXT_CSS_Component_Value * fn, uint32_t * out) {
  static const GTEXT_CSS_Component_Value no_values[1];
  const GTEXT_CSS_Component_Value * k = fn->children ? fn->children : no_values;
  size_t kn = fn->child_count;
  size_t s = skip_ws(k, kn, 0);
  size_t e = trim_end(k, s, kn);
  const char * nm = fn->value.data;
  size_t nl = fn->value.length;
  mq_node_kind kind;
  uint32_t bits = 0;
  GTEXT_CSS_Selector_List * sel = NULL;
  const char * text = NULL;
  size_t text_len = 0;
  if (ieq(nm, nl, "selector")) {
    kind = MQN_SELECTOR;
    GTEXT_CSS_Selector_Options so = gtext_css_selector_options_default();
    so.allocator = &P->alloc;
    GTEXT_CSS_Selector_Result sr;
    GTEXT_CSS_Status st = gtext_css_selector_parse_values(k + s, e - s, &so, &sel, &sr);
    gtext_css_selector_result_free(&sr);
    if (st != GTEXT_CSS_OK) {
      set_status(P, st, fn->span);
      return FEAT_INVALID;
    }
    if (sel && (sel->count != 1 || sel->complex[0].relative)) {
      gtext_css_selector_list_free(sel);
      sel = NULL;
    }
  } else if (ieq(nm, nl, "font-tech") || ieq(nm, nl, "font-format")) {
    bool tech = ieq(nm, nl, "font-tech");
    kind = tech ? MQN_FONT_TECH : MQN_FONT_FORMAT;
    if (e - s != 1) {
      return FEAT_SOFT;
    }
    const GTEXT_CSS_Component_Value * a = &k[s];
    if (a->kind != GTEXT_CSS_VALUE_TOKEN) {
      return FEAT_SOFT;
    }
    if (a->type == GTEXT_CSS_TOKEN_IDENT) {
      int i = tech ? lookup_name(tech_names, 11, &a->value) : lookup_name(format_names, 7, &a->value);
      if (i < 0) {
        return FEAT_SOFT; /* font-tech(bogus): <general-enclosed> */
      }
      bits = (uint32_t)1 << i;
    } else if (!tech && a->type == GTEXT_CSS_TOKEN_STRING) {
      /* A string is a format by its MIME type; this library knows none, so it is a valid feature that is false (web-platform-tests: font-format('opentype') is false). */
      bits = 0;
    } else {
      return FEAT_SOFT;
    }
  } else if (ieq(nm, nl, "at-rule")) {
    kind = MQN_AT_RULE;
    if (e - s != 1 || k[s].kind != GTEXT_CSS_VALUE_TOKEN || k[s].type != GTEXT_CSS_TOKEN_AT_KEYWORD) {
      return FEAT_SOFT;
    }
    char * c = arena_copy(P, k[s].value.data, k[s].value.length);
    if (!c) {
      return FEAT_INVALID;
    }
    for (size_t i = 0; i < k[s].value.length; i++) {
      c[i] = (char)css_mq_lower((unsigned char)c[i]);
    }
    text = c;
    text_len = k[s].value.length;
  } else if (ieq(nm, nl, "named-feature") || ieq(nm, nl, "env")) {
    kind = ieq(nm, nl, "env") ? MQN_ENV : MQN_NAMED;
    if (e - s != 1 || !is_ident(&k[s])) {
      return FEAT_SOFT;
    }
    text = arena_copy(P, k[s].value.data, k[s].value.length);
    if (!text) {
      return FEAT_INVALID;
    }
    text_len = k[s].value.length;
  } else {
    return FEAT_SOFT;
  }
  uint32_t nd = new_node(P, kind, fn->span);
  if (nd == MQ_NONE) {
    gtext_css_selector_list_free(sel);
    return FEAT_INVALID;
  }
  mq_node * node = &P->in->nodes[nd];
  node->sel = sel;
  node->bits = bits;
  node->text = text;
  node->text_length = text_len;
  /* The serialization of a function leaf: name(arguments). */
  size_t al = 0;
  char * args = NULL;
  if (kind == MQN_SELECTOR && sel) {
    char * sb = NULL;
    size_t sl = 0;
    GTEXT_CSS_Status st = gtext_css_selector_serialize_buffer(sel, &P->alloc, &sb, &sl);
    if (st != GTEXT_CSS_OK) {
      set_status(P, st, fn->span);
      return FEAT_INVALID;
    }
    args = arena_copy(P, sb, sl);
    gtext_css_selector_buffer_free(&P->alloc, sb);
    al = sl;
  } else if (kind == MQN_AT_RULE) {
    args = arena_copy(P, "@", 1);
    char * full = args ? css_arena_alloc(&P->in->arena, text_len + 2) : NULL;
    if (!full) {
      set_status(P, GTEXT_CSS_E_OOM, fn->span);
      return FEAT_INVALID;
    }
    full[0] = '@';
    memcpy(full + 1, text, text_len);
    full[text_len + 1] = 0;
    args = full;
    al = text_len + 1;
  } else {
    args = serialize_trimmed(P, k + s, e - s, &al);
  }
  if (!args) {
    return FEAT_INVALID;
  }
  char * full = css_arena_alloc(&P->in->arena, nl + al + 3);
  if (!full) {
    set_status(P, GTEXT_CSS_E_OOM, fn->span);
    return FEAT_INVALID;
  }
  for (size_t i = 0; i < nl; i++) {
    full[i] = (char)css_mq_lower((unsigned char)nm[i]);
  }
  full[nl] = '(';
  memcpy(full + nl + 1, args, al);
  full[nl + 1 + al] = ')';
  full[nl + al + 2] = 0;
  node->value = full; /* the serialization */
  node->value_length = nl + al + 2;
  node->flags = MQF_SPEC_TEXT;
  *out = nd;
  return FEAT_OK;
}


/* ---- container conditions and @when ---------------------------------------- */

static bool parse_condition(parser * P, const GTEXT_CSS_Component_Value * v, size_t n, bool allow_or, uint32_t * out);

const char * const css_cq_scroll_names[CSS_CQ_SCROLL_FEATURES] = {"stuck", "snapped", "scrollable", "scrolled"};
static const char * const kw_stuck[] = {"none", "top", "right", "bottom", "left", "block-start", "inline-start", "block-end", "inline-end"};
static const char * const kw_snapped[] = {"none", "x", "y", "block", "inline", "both"};
static const char * const kw_scroll[] = {"none", "top", "right", "bottom", "left", "block-start", "inline-start", "block-end", "inline-end", "x", "y", "block", "inline"};
const char * const * const css_cq_scroll_keywords[CSS_CQ_SCROLL_FEATURES] = {kw_stuck, kw_snapped, kw_scroll, kw_scroll};
const size_t css_cq_scroll_keyword_count[CSS_CQ_SCROLL_FEATURES] = {9, 6, 13, 13};

static char * lower_copy(parser * P, const char * s, size_t len) {
  char * c = arena_copy(P, s, len);
  if (c) {
    for (size_t i = 0; i < len; i++) {
      c[i] = (char)css_mq_lower((unsigned char)c[i]);
    }
  }
  return c;
}

/// Split [s, e) at the comparison operators (`<`, `<=`, `>`, `>=`, `=`): the groups between them and the operators. false: more than two operators.
static bool split_comparisons(const GTEXT_CSS_Component_Value * v, size_t s, size_t e, grp * groups, mq_op * ops, size_t * ngroups, size_t * nops) {
  size_t ng = 0, nop = 0, gstart = s;
  for (size_t i = s; i < e; i++) {
    const GTEXT_CSS_Component_Value * t = &v[i];
    if (t->kind == GTEXT_CSS_VALUE_TOKEN && t->type == GTEXT_CSS_TOKEN_DELIM && (t->delim == '<' || t->delim == '>' || t->delim == '=')) {
      mq_op op;
      size_t w = i + 1;
      if (t->delim == '=') {
        op = MQOP_EQ;
      } else {
        const bool le = i + 1 < e && is_delim(&v[i + 1], '=');
        op = t->delim == '<' ? (le ? MQOP_LE : MQOP_LT) : (le ? MQOP_GE : MQOP_GT);
        if (le) {
          w = i + 2;
        }
      }
      if (nop == 2 || ng >= 3) {
        return false;
      }
      const size_t ge = trim_end(v, gstart, i);
      groups[ng].start = skip_ws(v, ge, gstart);
      groups[ng].end = ge;
      ng++;
      ops[nop++] = op;
      i = w - 1;
      gstart = w;
    }
  }
  const size_t ge = trim_end(v, gstart, e);
  groups[ng].start = skip_ws(v, ge, gstart);
  groups[ng].end = ge;
  ng++;
  *ngroups = ng;
  *nops = nop;
  return true;
}

/// A feature of `style()`, `scroll-state()` or `anchored()`, from the contents [v, v+n) of its parentheses (or of the function, when it is bare).
/// FEAT_SOFT: not a feature of that function (general-enclosed).
static feat_result parse_sub_feature(parser * P, const GTEXT_CSS_Component_Value * v, size_t n, GTEXT_CSS_Span span, uint32_t * out) {
  const size_t s = skip_ws(v, n, 0);
  const size_t e = trim_end(v, s, n);
  if (s >= e) {
    return FEAT_SOFT;
  }
  const uint8_t sub = P->sub;
  grp groups[3];
  mq_op ops[2];
  size_t ng = 0, nop = 0;
  if (!split_comparisons(v, s, e, groups, ops, &ng, &nop)) {
    return FEAT_SOFT;
  }
  for (size_t g = 0; g < ng; g++) {
    if (groups[g].start >= groups[g].end) {
      return FEAT_SOFT;
    }
  }
  if (nop == 0) {
    if (!is_ident(&v[s])) {
      return FEAT_SOFT;
    }
    const size_t after = skip_ws(v, e, s + 1);
    const bool boolean = after >= e;
    if (!boolean && !(v[after].kind == GTEXT_CSS_VALUE_TOKEN && v[after].type == GTEXT_CSS_TOKEN_COLON)) {
      return FEAT_SOFT;
    }
    const size_t vs = boolean ? e : skip_ws(v, e, after + 1);
    const bool custom = v[s].value.length > 2 && v[s].value.data[0] == '-' && v[s].value.data[1] == '-';
    int feat = -1;
    uint32_t kw = 0;
    if (sub == MQ_SUB_SCROLL) {
      for (int i = 0; i < CSS_CQ_SCROLL_FEATURES; i++) {
        if (ieq(v[s].value.data, v[s].value.length, css_cq_scroll_names[i])) {
          feat = i;
        }
      }
      if (feat < 0) {
        return FEAT_SOFT;
      }
      if (!boolean) {
        if (e - vs != 1 || !is_ident(&v[vs])) {
          return FEAT_SOFT;
        }
        int k = -1;
        for (size_t i = 0; i < css_cq_scroll_keyword_count[feat]; i++) {
          if (ieq(v[vs].value.data, v[vs].value.length, css_cq_scroll_keywords[feat][i])) {
            k = (int)i;
          }
        }
        if (k < 0) {
          return FEAT_SOFT;
        }
        kw = (uint32_t)k;
      }
    }
    else if (sub == MQ_SUB_ANCHORED) {
      if (!ieq(v[s].value.data, v[s].value.length, "fallback") || (!boolean && vs >= e)) {
        return FEAT_SOFT;
      }
    }
    else if (!boolean && vs >= e && !custom) {
      return FEAT_SOFT;
    }
    const uint32_t nd = new_node(P, MQN_SUBFEATURE, span);
    if (nd == MQ_NONE) {
      return FEAT_INVALID;
    }
    char * name = custom ? arena_copy(P, v[s].value.data, v[s].value.length) : lower_copy(P, v[s].value.data, v[s].value.length);
    char * val = NULL;
    size_t vl = 0;
    if (!boolean && vs < e) {
      val = sub == MQ_SUB_SCROLL ? lower_copy(P, v[vs].value.data, v[vs].value.length) : serialize_trimmed(P, v + vs, e - vs, &vl);
      if (sub == MQ_SUB_SCROLL) {
        vl = v[vs].value.length;
      }
    }
    else if (!boolean) {
      val = arena_copy(P, "", 0); /* `(--x:)`: a custom property with the empty value */
    }
    if (!name || (!boolean && !val)) {
      return FEAT_INVALID;
    }
    mq_node * node = &P->in->nodes[nd];
    node->type_id = sub;
    node->form = boolean ? MQFORM_BOOLEAN : MQFORM_PLAIN;
    node->text = name;
    node->text_length = v[s].value.length;
    node->value = val;
    node->value_length = vl;
    node->feature = (int16_t)feat;
    node->sub_kw = kw;
    *out = nd;
    return FEAT_OK;
  }
  /* A range: only the values of a style query can be compared. */
  if (sub != MQ_SUB_STYLE) {
    return FEAT_SOFT;
  }
  if (ng == 3) {
    const bool lt = (ops[0] == MQOP_LT || ops[0] == MQOP_LE) && (ops[1] == MQOP_LT || ops[1] == MQOP_LE);
    const bool gt = (ops[0] == MQOP_GT || ops[0] == MQOP_GE) && (ops[1] == MQOP_GT || ops[1] == MQOP_GE);
    if (!lt && !gt) {
      return FEAT_SOFT;
    }
  }
  const uint32_t nd = new_node(P, MQN_SUBFEATURE, span);
  if (nd == MQ_NONE) {
    return FEAT_INVALID;
  }
  const char * txt[3] = {NULL, NULL, NULL};
  size_t len[3] = {0, 0, 0};
  for (size_t g = 0; g < ng; g++) {
    txt[g] = serialize_trimmed(P, v + groups[g].start, groups[g].end - groups[g].start, &len[g]);
    if (!txt[g]) {
      return FEAT_INVALID;
    }
  }
  mq_node * node = &P->in->nodes[nd];
  node->type_id = sub;
  node->form = MQFORM_RANGE;
  node->op1 = (uint8_t)ops[0];
  node->op2 = ng == 3 ? (uint8_t)ops[1] : (uint8_t)MQOP_NONE;
  node->text = txt[0];
  node->text_length = len[0];
  if (ng == 3) {
    node->name = txt[1];
    node->name_length = len[1];
    node->value = txt[2];
    node->value_length = len[2];
  }
  else {
    node->value = txt[1];
    node->value_length = len[1];
  }
  node->feature = -1;
  *out = nd;
  return FEAT_OK;
}

/// A wrapper node of one kid.
static uint32_t make_wrapper(parser * P, mq_node_kind kind, uint32_t kid, GTEXT_CSS_Span span, uint16_t flags, uint8_t type_id) {
  const uint32_t nd = new_node(P, kind, span);
  if (nd == MQ_NONE) {
    return MQ_NONE;
  }
  P->in->nodes[nd].flags = flags;
  P->in->nodes[nd].type_id = type_id;
  return set_kids(P, nd, &kid, 1) ? nd : MQ_NONE;
}

/// `style()`, `scroll-state()` and `anchored()` of a container condition. FEAT_SOFT: <general-enclosed>.
static feat_result parse_container_function(parser * P, const GTEXT_CSS_Component_Value * fn, uint32_t * out) {
  uint8_t sub;
  if (ieq(fn->value.data, fn->value.length, "style")) {
    sub = MQ_SUB_STYLE;
  }
  else if (ieq(fn->value.data, fn->value.length, "scroll-state")) {
    sub = MQ_SUB_SCROLL;
  }
  else if (ieq(fn->value.data, fn->value.length, "anchored")) {
    sub = MQ_SUB_ANCHORED;
  }
  else {
    return FEAT_SOFT;
  }
  static const GTEXT_CSS_Component_Value no_values[1];
  const GTEXT_CSS_Component_Value * k = fn->children ? fn->children : no_values;
  const size_t s = skip_ws(k, fn->child_count, 0);
  const size_t e = trim_end(k, s, fn->child_count);
  if (s >= e) {
    return FEAT_SOFT;
  }
  struct GTEXT_CSS_Media_List_Internal * in = P->in;
  const size_t nmark = in->node_count, kmark = in->kid_count, vmark = in->value_count, mmark = in->math_count;
  const size_t nx = skip_ws(k, e, s + 1);
  const bool cond_first = is_paren_block(&k[s]) || (is_ident_lit(&k[s], "not") && nx < e);
  uint32_t cond = MQ_NONE;
  bool ok;
  P->sub = sub;
  if (cond_first) {
    ok = parse_condition(P, k + s, e - s, true, &cond);
  }
  else {
    const feat_result fr = parse_sub_feature(P, k + s, e - s, fn->span, &cond);
    ok = fr == FEAT_OK;
  }
  P->sub = MQ_SUB_NONE;
  P->invalid = false;
  if (P->status != GTEXT_CSS_OK) {
    return FEAT_INVALID;
  }
  if (!ok) {
    truncate_to(P, nmark, kmark, vmark, mmark);
    return FEAT_SOFT;
  }
  const uint32_t nd = make_wrapper(P, MQN_CQ_FN, cond, fn->span, cond_first ? 0 : MQF_BARE, sub);
  if (nd == MQ_NONE) {
    return FEAT_INVALID;
  }
  *out = nd;
  return FEAT_OK;
}

/// `media()` and `supports()` of an `@when` condition, and the supports functions as leaves. FEAT_SOFT: <general-enclosed>.
static feat_result parse_when_function(parser * P, const GTEXT_CSS_Component_Value * fn, uint32_t * out) {
  const bool is_media = ieq(fn->value.data, fn->value.length, "media");
  const bool is_supports = ieq(fn->value.data, fn->value.length, "supports");
  if (!is_media && !is_supports) {
    return parse_supports_function(P, fn, out);
  }
  static const GTEXT_CSS_Component_Value no_values[1];
  const GTEXT_CSS_Component_Value * k = fn->children ? fn->children : no_values;
  const size_t s = skip_ws(k, fn->child_count, 0);
  const size_t e = trim_end(k, s, fn->child_count);
  if (s >= e) {
    return FEAT_SOFT;
  }
  struct GTEXT_CSS_Media_List_Internal * in = P->in;
  const size_t nmark = in->node_count, kmark = in->kid_count, vmark = in->value_count, mmark = in->math_count;
  uint32_t inner = MQ_NONE;
  bool bare = false;
  P->when = false;
  if (is_media) {
    const feat_result fr = parse_feature_core(P, fn, &inner);
    P->when = true;
    if (P->status != GTEXT_CSS_OK) {
      return FEAT_INVALID;
    }
    if (fr != FEAT_OK) {
      const bool invalid = fr == FEAT_INVALID;
      P->invalid = false;
      truncate_to(P, nmark, kmark, vmark, mmark);
      if (invalid) {
        /* a media feature that is known and written wrongly is false, as it is in a media query (where it replaces the query) */
        const uint32_t en = new_node(P, MQN_ENCLOSED, fn->span);
        if (en == MQ_NONE) {
          return FEAT_INVALID;
        }
        in->nodes[en].src = fn;
        in->nodes[en].flags = MQF_FALSE;
        *out = en;
        return FEAT_OK;
      }
      return FEAT_SOFT;
    }
    const uint32_t nd = make_wrapper(P, MQN_TEST_MEDIA, inner, fn->span, 0, 0);
    if (nd == MQ_NONE) {
      return FEAT_INVALID;
    }
    *out = nd;
    return FEAT_OK;
  }
  P->supports = true;
  bool ok;
  if (declaration_shaped(k + s, e - s)) {
    bare = true;
    inner = make_decl(P, k + s, e - s, k[s].span);
    ok = inner != MQ_NONE;
  }
  else {
    ok = parse_condition(P, k + s, e - s, true, &inner);
  }
  P->supports = false;
  P->when = true;
  P->invalid = false;
  if (P->status != GTEXT_CSS_OK) {
    return FEAT_INVALID;
  }
  if (!ok) {
    truncate_to(P, nmark, kmark, vmark, mmark);
    return FEAT_SOFT;
  }
  const uint32_t nd = make_wrapper(P, MQN_TEST_SUPPORTS, inner, fn->span, bare ? MQF_BARE : 0, 0);
  if (nd == MQ_NONE) {
    return FEAT_INVALID;
  }
  *out = nd;
  return FEAT_OK;
}

/* ---- the condition ------------------------------------------------------- */

static bool push_frame(parser * P, const GTEXT_CSS_Component_Value * v, size_t n, const GTEXT_CSS_Component_Value * owner, bool allow_or) {
  if (P->nframes >= P->o.max_depth) {
    set_status(P, GTEXT_CSS_E_LIMIT_CONDITION_DEPTH, n ? v[0].span : (GTEXT_CSS_Span){0});
    return false;
  }
  if (!css_mq_grow(&P->alloc, (void **)&P->frames, &P->framecap, P->nframes, 1, sizeof(frame))) {
    set_status(P, GTEXT_CSS_E_OOM, (GTEXT_CSS_Span){0});
    return false;
  }
  frame * f = &P->frames[P->nframes++];
  memset(f, 0, sizeof *f);
  f->v = v;
  f->n = n;
  f->owner = owner;
  f->opbase = (uint32_t)P->nops;
  f->nmark = (uint32_t)P->in->node_count;
  f->kmark = (uint32_t)P->in->kid_count;
  f->vmark = (uint32_t)P->in->value_count;
  f->mmark = (uint32_t)P->in->math_count;
  f->allow_or = allow_or;
  f->want_operand = true;
  return true;
}

static bool push_op(parser * P, uint32_t node) {
  if (!css_mq_grow(&P->alloc, (void **)&P->ops, &P->opcap, P->nops, 1, sizeof(uint32_t))) {
    set_status(P, GTEXT_CSS_E_OOM, (GTEXT_CSS_Span){0});
    return false;
  }
  P->ops[P->nops++] = node;
  return true;
}

/// Parse tokens [v, v+n) as a <media-condition> (or <media-condition-without-or>). Returns true with *out set; false when the tokens do not match it
/// (P->invalid / P->status say if it is worse than that). `top` frames are not rolled back by the caller's fallback.
static bool parse_condition(parser * P, const GTEXT_CSS_Component_Value * v, size_t n, bool allow_or, uint32_t * out) {
  size_t base = P->nframes;
  if (!push_frame(P, v, n, NULL, allow_or)) {
    return false;
  }
  bool result = false;
  uint32_t ret = MQ_NONE;
  bool have_ret = false;
  for (;;) {
    if (P->status != GTEXT_CSS_OK || P->invalid) {
      break;
    }
    frame * f = &P->frames[P->nframes - 1];
    bool soft = false;
    if (have_ret) {
      /* A child frame finished: its node is the content of a parenthesized group. */
      have_ret = false;
      uint32_t pn = new_node(P, MQN_PAREN, f->v[f->pos - 1].span);
      if (pn == MQ_NONE) {
        break;
      }
      if (!set_kids(P, pn, &ret, 1) || !push_op(P, pn)) {
        break;
      }
      f->want_operand = false;
      continue;
    }
    if (f->want_operand) {
      f->pos = skip_ws(f->v, f->n, f->pos);
      if (f->pos >= f->n) {
        soft = true;
      } else {
        const GTEXT_CSS_Component_Value * t = &f->v[f->pos];
        if (is_ident_lit(t, "not") && !f->is_not && P->nops == f->opbase && !f->op) {
          f->is_not = true;
          f->pos++;
          continue;
        } else if (is_paren_block(t)) {
          f->pos++;
          if (P->supports && declaration_shaped(t->children, t->child_count)) {
            uint32_t dn = make_decl(P, t->children, t->child_count, t->span);
            if (dn == MQ_NONE || !push_op(P, dn)) {
              break;
            }
            f->want_operand = false;
            continue;
          }
          /* Decide the branch by the first token of the contents. */
          size_t cs = skip_ws(t->children, t->child_count, 0);
          bool cond_first = cs < t->child_count && (is_ident_lit(&t->children[cs], "not") || t->children[cs].kind != GTEXT_CSS_VALUE_TOKEN);
          if (cond_first && t->children[cs].kind == GTEXT_CSS_VALUE_BLOCK && t->children[cs].type != GTEXT_CSS_TOKEN_OPEN_PAREN) {
            cond_first = false;
          }
          if (cond_first) {
            if (!push_frame(P, t->children, t->child_count, t, true)) {
              break;
            }
            continue;
          }
          uint32_t fn;
          feat_result fr = parse_feature(P, t, &fn);
          if (fr == FEAT_INVALID) {
            break;
          }
          if (fr == FEAT_SOFT) {
            diag(P, GTEXT_CSS_MQDIAG_UNKNOWN, t->span, REF_UNKNOWN);
            fn = make_enclosed(P, t);
            if (fn == MQ_NONE) {
              break;
            }
          }
          if (!push_op(P, fn)) {
            break;
          }
          f->want_operand = false;
          continue;
        } else if (t->kind == GTEXT_CSS_VALUE_FUNCTION) {
          f->pos++;
          uint32_t fn = MQ_NONE;
          feat_result fr = FEAT_SOFT;
          if (P->supports) {
            fr = parse_supports_function(P, t, &fn);
          }
          else if (P->when) {
            fr = parse_when_function(P, t, &fn);
          }
          else if (P->container && !P->sub) {
            fr = parse_container_function(P, t, &fn);
          }
          f = &P->frames[P->nframes - 1]; /* a nested parse may have moved the frames */
          if (fr == FEAT_INVALID) {
            break;
          }
          if (fr == FEAT_SOFT) {
            fn = make_enclosed(P, t);
          }
          if (fn == MQ_NONE || !push_op(P, fn)) {
            break;
          }
          f->want_operand = false;
          continue;
        } else {
          soft = true;
        }
      }
    } else {
      f->pos = skip_ws(f->v, f->n, f->pos);
      if (f->pos >= f->n) {
        /* The end of the frame: build its node. */
        size_t k = P->nops - f->opbase;
        uint32_t node;
        if (f->is_not) {
          node = new_node(P, MQN_NOT, f->v[0].span);
          if (node == MQ_NONE || !set_kids(P, node, &P->ops[f->opbase], 1)) {
            break;
          }
        } else if (k == 1) {
          node = P->ops[f->opbase];
        } else {
          node = new_node(P, f->op == MQN_OR ? MQN_OR : MQN_AND, f->v[0].span);
          if (node == MQ_NONE || !set_kids(P, node, &P->ops[f->opbase], k)) {
            break;
          }
        }
        P->nops = f->opbase;
        P->nframes--;
        if (P->nframes == base) {
          *out = node;
          result = true;
          break;
        }
        ret = node;
        have_ret = true;
        continue;
      }
      const GTEXT_CSS_Component_Value * t = &f->v[f->pos];
      uint8_t op = 0;
      if (is_ident_lit(t, "and")) {
        op = MQN_AND;
      } else if (is_ident_lit(t, "or")) {
        op = MQN_OR;
      }
      if (!op || f->is_not) {
        soft = true;
      } else if (f->op && f->op != op) {
        f->mixed = true;
        soft = true;
      } else if (op == MQN_OR && !f->allow_or) {
        soft = true;
      } else {
        f->op = op;
        f->pos++;
        f->want_operand = true;
        continue;
      }
    }
    if (soft) {
      /* The frame does not match <media-condition>. */
      frame done = *f;
      rollback(P, &done);
      P->nframes--;
      if (P->nframes == base) {
        if (done.mixed) {
          invalid_query(P, GTEXT_CSS_MQDIAG_MIXED, done.pos < done.n ? done.v[done.pos].span : (done.n ? done.v[0].span : (GTEXT_CSS_Span){0}), REF_MIXED);
        } else {
          invalid_query(P, GTEXT_CSS_MQDIAG_SYNTAX, done.pos < done.n ? done.v[done.pos].span : (done.n ? done.v[0].span : (GTEXT_CSS_Span){0}), REF_SYNTAX);
        }
        break;
      }
      /* A parenthesized group: try it as a feature, then as <general-enclosed>. */
      const GTEXT_CSS_Component_Value * owner = done.owner;
      frame * pf = &P->frames[P->nframes - 1];
      uint32_t fn;
      feat_result fr = parse_feature(P, owner, &fn);
      if (fr == FEAT_INVALID) {
        break;
      }
      if (fr == FEAT_SOFT) {
        diag(P, GTEXT_CSS_MQDIAG_UNKNOWN, owner->span, REF_UNKNOWN);
        fn = make_enclosed(P, owner);
        if (fn == MQ_NONE) {
          break;
        }
      }
      if (!push_op(P, fn)) {
        break;
      }
      pf->want_operand = false;
      continue;
    }
  }
  if (!result) {
    /* Unwind whatever frames are left (a hard failure or an invalid query). */
    if (P->nframes > base) {
      frame * f = &P->frames[base];
      rollback(P, f);
      P->nframes = base;
    }
  }
  return result;
}

/* ---- the query ---------------------------------------------------------- */

static mq_type_id type_of(const char * s, size_t n) {
  for (size_t i = 0; i < css_mq_type_row_count; i++) {
    if (ieq(s, n, css_mq_type_rows[i].name)) {
      if (ieq(s, n, "all")) {
        return MQT_ID_ALL;
      }
      if (ieq(s, n, "screen")) {
        return MQT_ID_SCREEN;
      }
      if (ieq(s, n, "print")) {
        return MQT_ID_PRINT;
      }
      return css_mq_type_rows[i].cls == MQT_DEPRECATED ? MQT_ID_DEPRECATED : MQT_ID_UNKNOWN;
    }
  }
  return MQT_ID_UNKNOWN;
}

/// Parse one <media-query> over [v, v+n); always makes a QUERY node (an invalid query is `not all`, flagged). Returns MQ_NONE only on a hard failure.
static uint32_t parse_query(parser * P, const GTEXT_CSS_Component_Value * v, size_t n, GTEXT_CSS_Span where) {
  struct GTEXT_CSS_Media_List_Internal * in = P->in;
  size_t nmark = in->node_count, kmark = in->kid_count, vmark = in->value_count, mmark = in->math_count;
  P->invalid = false;
  uint32_t q = new_node(P, MQN_QUERY, where);
  if (q == MQ_NONE) {
    return MQ_NONE;
  }
  size_t s = skip_ws(v, n, 0);
  size_t e = trim_end(v, s, n);
  uint32_t cond = MQ_NONE;
  uint8_t flags = 0;
  mq_type_id tid = MQT_ID_ALL;
  const char * type_text = NULL;
  size_t type_len = 0;
  if (s >= e) {
    invalid_query(P, GTEXT_CSS_MQDIAG_EMPTY, where, REF_SYNTAX);
  } else {
    const GTEXT_CSS_Component_Value * t = &v[s];
    size_t pos = s;
    bool has_type = false;
    if (is_ident(t)) {
      size_t nx = skip_ws(v, e, s + 1);
      bool not_cond = is_ident_lit(t, "not") && nx < e && v[nx].kind != GTEXT_CSS_VALUE_TOKEN;
      if (!not_cond) {
        if (is_ident_lit(t, "not")) {
          flags |= MQF_NEGATED;
          pos = nx;
        } else if (is_ident_lit(t, "only")) {
          flags |= MQF_ONLY;
          pos = nx;
        }
        if (pos >= e || !is_ident(&v[pos])) {
          invalid_query(P, GTEXT_CSS_MQDIAG_SYNTAX, pos < e ? v[pos].span : t->span, REF_SYNTAX);
        } else if (css_mq_is_reserved(v[pos].value.data, v[pos].value.length)) {
          invalid_query(P, GTEXT_CSS_MQDIAG_TYPE, v[pos].span, REF_TYPE);
        } else {
          has_type = true;
          const GTEXT_CSS_Component_Value * ty = &v[pos];
          tid = type_of(ty->value.data, ty->value.length);
          /* The type is kept lower case and decoded; the serializer escapes it. */
          char * txt = css_arena_strdup(&in->arena, ty->value.data, ty->value.length);
          if (!txt) {
            set_status(P, GTEXT_CSS_E_OOM, where);
            return MQ_NONE;
          }
          for (size_t i = 0; i < ty->value.length; i++) {
            txt[i] = (char)css_mq_lower((unsigned char)txt[i]);
          }
          type_text = txt;
          type_len = ty->value.length;
          flags |= MQF_HAS_TYPE;
          pos = skip_ws(v, e, pos + 1);
          if (pos < e) {
            if (!is_ident_lit(&v[pos], "and")) {
              invalid_query(P, GTEXT_CSS_MQDIAG_SYNTAX, v[pos].span, REF_SYNTAX);
            } else {
              size_t cs = skip_ws(v, e, pos + 1);
              if (cs >= e) {
                invalid_query(P, GTEXT_CSS_MQDIAG_SYNTAX, v[pos].span, REF_SYNTAX);
              } else if (!parse_condition(P, &v[cs], e - cs, false, &cond) && !P->invalid && P->status == GTEXT_CSS_OK) {
                invalid_query(P, GTEXT_CSS_MQDIAG_SYNTAX, v[cs].span, REF_SYNTAX);
              }
            }
          }
        }
      }
      if (not_cond) {
        has_type = false;
        if (!parse_condition(P, &v[s], e - s, true, &cond) && !P->invalid && P->status == GTEXT_CSS_OK) {
          invalid_query(P, GTEXT_CSS_MQDIAG_SYNTAX, t->span, REF_SYNTAX);
        }
      }
    } else if (t->kind == GTEXT_CSS_VALUE_BLOCK || t->kind == GTEXT_CSS_VALUE_FUNCTION) {
      if (!parse_condition(P, &v[s], e - s, true, &cond) && !P->invalid && P->status == GTEXT_CSS_OK) {
        invalid_query(P, GTEXT_CSS_MQDIAG_SYNTAX, t->span, REF_SYNTAX);
      }
    } else {
      invalid_query(P, GTEXT_CSS_MQDIAG_SYNTAX, t->span, REF_SYNTAX);
    }
    (void)has_type;
  }
  if (P->status != GTEXT_CSS_OK) {
    return MQ_NONE;
  }
  if (P->invalid) {
    /* Replaced by `not all`: drop whatever was built for it. */
    drop_nodes(in, nmark + 1);
    in->kid_count = kmark;
    in->value_count = vmark;
    while (in->math_count > mmark) {
      gtext_css_math_free(in->maths[--in->math_count]);
    }
    P->nops = 0;
    diag(P, P->inv_kind, P->inv_span, P->inv_ref);
    if (P->status != GTEXT_CSS_OK) {
      return MQ_NONE;
    }
    in->nodes[q].flags = MQF_NEGATED | MQF_HAS_TYPE | MQF_REPLACED;
    in->nodes[q].type_id = MQT_ID_ALL;
    in->nodes[q].text = "all";
    in->nodes[q].text_length = 3;
    P->replaced++;
    return q;
  }
  mq_node * qn = &in->nodes[q];
  qn->flags = flags;
  qn->type_id = (uint8_t)tid;
  qn->text = type_text;
  qn->text_length = type_len;
  if (cond != MQ_NONE && !set_kids(P, q, &cond, 1)) {
    return MQ_NONE;
  }
  return q;
}


/// One `<container-condition>`: `[ <container-name>? <container-query>? ]!`, as a QUERY node whose `text` is the name (NULL: none), whose kid is the query (none: the
/// name alone), and whose `bits` say which kinds of feature it asks of its container. *valid false: the grammar does not match, and the whole rule is invalid.
static uint32_t parse_container_root(parser * P, const GTEXT_CSS_Component_Value * v, size_t n, GTEXT_CSS_Span where, bool * valid) {
  struct GTEXT_CSS_Media_List_Internal * in = P->in;
  *valid = true;
  P->invalid = false;
  const uint32_t q = new_node(P, MQN_QUERY, where);
  if (q == MQ_NONE) {
    return MQ_NONE;
  }
  const size_t s = skip_ws(v, n, 0);
  const size_t e = trim_end(v, s, n);
  uint32_t cond = MQ_NONE;
  const char * name = NULL;
  size_t name_len = 0;
  if (s >= e) {
    *valid = false;
  }
  else {
    size_t pos = s;
    const GTEXT_CSS_Component_Value * t = &v[s];
    if (is_ident(t) && !is_ident_lit(t, "not")) {
      static const char * const reserved[] = {"and", "or", "none", "initial", "inherit", "unset", "revert", "revert-layer", "default"};
      for (size_t i = 0; i < sizeof reserved / sizeof *reserved; i++) {
        if (is_ident_lit(t, reserved[i])) {
          *valid = false;
        }
      }
      if (*valid) {
        char * c = arena_copy(P, t->value.data, t->value.length);
        if (!c) {
          return MQ_NONE;
        }
        name = c;
        name_len = t->value.length;
        pos = skip_ws(v, e, s + 1);
      }
    }
    if (*valid && pos < e) {
      if (!parse_condition(P, v + pos, e - pos, true, &cond)) {
        *valid = false;
      }
    }
  }
  if (P->status != GTEXT_CSS_OK) {
    return MQ_NONE;
  }
  P->invalid = false;
  uint32_t bits = 0;
  if (*valid) {
    for (size_t i = (size_t)q + 1; i < in->node_count; i++) {
      const mq_node * nd = &in->nodes[i];
      if (nd->kind == MQN_FEATURE) {
        bits |= MQC_SIZE;
      }
      else if (nd->kind == MQN_CQ_FN) {
        bits |= nd->type_id == MQ_SUB_STYLE ? MQC_STYLE : (nd->type_id == MQ_SUB_SCROLL ? MQC_SCROLL : MQC_ANCHORED);
      }
      else if (nd->kind == MQN_ENCLOSED) {
        bits |= MQC_UNKNOWN;
      }
    }
  }
  mq_node * qn = &in->nodes[q];
  qn->text = name;
  qn->text_length = name_len;
  qn->bits = bits;
  if (*valid && cond != MQ_NONE && !set_kids(P, q, &cond, 1)) {
    return MQ_NONE;
  }
  return q;
}

/// One <supports-condition> (or a bare declaration): a QUERY node with no type whose kid is the condition.
static uint32_t parse_supports_root(parser * P, const GTEXT_CSS_Component_Value * v, size_t n, GTEXT_CSS_Span where) {
  struct GTEXT_CSS_Media_List_Internal * in = P->in;
  size_t nmark = in->node_count, kmark = in->kid_count, vmark = in->value_count, mmark = in->math_count;
  P->invalid = false;
  uint32_t q = new_node(P, MQN_QUERY, where);
  if (q == MQ_NONE) {
    return MQ_NONE;
  }
  size_t s = skip_ws(v, n, 0);
  size_t e = trim_end(v, s, n);
  uint32_t cond = MQ_NONE;
  if (s >= e) {
    invalid_query(P, GTEXT_CSS_MQDIAG_EMPTY, where, REF_SYNTAX);
  } else if (P->bare_ok && declaration_shaped(v + s, e - s)) {
    cond = make_decl(P, v + s, e - s, v[s].span);
  } else if (!parse_condition(P, v + s, e - s, true, &cond) && !P->invalid && P->status == GTEXT_CSS_OK) {
    invalid_query(P, GTEXT_CSS_MQDIAG_SYNTAX, v[s].span, REF_SYNTAX);
  }
  if (P->status != GTEXT_CSS_OK) {
    return MQ_NONE;
  }
  if (P->invalid) {
    drop_nodes(in, nmark + 1);
    in->kid_count = kmark;
    in->value_count = vmark;
    while (in->math_count > mmark) {
      gtext_css_math_free(in->maths[--in->math_count]);
    }
    P->nops = 0;
    diag(P, P->inv_kind, P->inv_span, P->inv_ref);
    in->nodes[q].flags = MQF_REPLACED | MQF_NEGATED | MQF_HAS_TYPE;
    in->nodes[q].text = "all";
    in->nodes[q].text_length = 3;
    P->replaced++;
    return P->status == GTEXT_CSS_OK ? q : MQ_NONE;
  }
  if (cond != MQ_NONE && !set_kids(P, q, &cond, 1)) {
    return MQ_NONE;
  }
  return q;
}

/* ---- finishing a list --------------------------------------------------- */

/// Fill the text of every ENCLOSED node and the summary of the list.
static GTEXT_CSS_Status finish(parser * P) {
  struct GTEXT_CSS_Media_List_Internal * in = P->in;
  GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
  so.allocator = &P->alloc;
  for (size_t i = 0; i < in->node_count; i++) {
    mq_node * nd = &in->nodes[i];
    if (nd->kind == MQN_FEATURE && nd->feature >= 0) {
      in->dependencies |= 1u << css_mq_feature_rows[nd->feature].dep;
      if (nd->feature < 64) {
        in->features_lo |= (uint64_t)1 << nd->feature;
      } else {
        in->features_hi |= (uint64_t)1 << (nd->feature - 64);
      }
    }
    if (nd->kind != MQN_ENCLOSED) {
      continue;
    }
    const GTEXT_CSS_Component_Value * src = nd->src;
    nd->src = NULL;
    static const GTEXT_CSS_Component_Value no_values[1];
    const GTEXT_CSS_Component_Value * kids = src->children ? src->children : no_values; /* never NULL plus an offset */
    size_t kn = src->child_count;
    size_t ks = skip_ws(kids, kn, 0);
    size_t ke = trim_end(kids, ks, kn);
    char * inner = NULL;
    size_t inner_len = 0;
    GTEXT_CSS_Status st = gtext_css_serialize_values_buffer(kids + ks, ke - ks, &so, &inner, &inner_len);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    /* `name(`inner`)` for a function, `(`inner`)` for a block. */
    size_t pre = src->kind == GTEXT_CSS_VALUE_FUNCTION ? src->value.length : 0;
    char * txt = css_arena_alloc(&in->arena, pre + inner_len + 3);
    if (!txt) {
      gtext_css_serialize_buffer_free(&so, inner);
      return GTEXT_CSS_E_OOM;
    }
    if (pre) {
      GTEXT_CSS_Serialize_Options so2 = so;
      char * nm = NULL;
      size_t nml = 0;
      GTEXT_CSS_Component_Value fnname = *src;
      fnname.kind = GTEXT_CSS_VALUE_TOKEN;
      fnname.type = GTEXT_CSS_TOKEN_IDENT;
      st = gtext_css_serialize_values_buffer(&fnname, 1, &so2, &nm, &nml);
      if (st != GTEXT_CSS_OK) {
        gtext_css_serialize_buffer_free(&so, inner);
        return st;
      }
      txt = css_arena_alloc(&in->arena, nml + inner_len + 3);
      if (!txt) {
        gtext_css_serialize_buffer_free(&so, nm);
        gtext_css_serialize_buffer_free(&so, inner);
        return GTEXT_CSS_E_OOM;
      }
      memcpy(txt, nm, nml);
      txt[nml] = '(';
      memcpy(txt + nml + 1, inner, inner_len);
      txt[nml + 1 + inner_len] = ')';
      txt[nml + 2 + inner_len] = 0;
      nd->text = txt;
      nd->text_length = nml + inner_len + 2;
      gtext_css_serialize_buffer_free(&so, nm);
    } else {
      txt[0] = '(';
      memcpy(txt + 1, inner, inner_len);
      txt[1 + inner_len] = ')';
      txt[2 + inner_len] = 0;
      nd->text = txt;
      nd->text_length = inner_len + 2;
    }
    gtext_css_serialize_buffer_free(&so, inner);
    /* A block shaped like a feature -- `(name)` or `(name: value)` -- keeps its parts for an extension callback. */
    if (src->kind == GTEXT_CSS_VALUE_BLOCK && ks < ke && is_ident(&kids[ks])) {
      size_t after = skip_ws(kids, ke, ks + 1);
      bool plain = after < ke && kids[after].kind == GTEXT_CSS_VALUE_TOKEN && kids[after].type == GTEXT_CSS_TOKEN_COLON;
      if (after >= ke || plain) {
        char * nm = css_arena_strdup(&in->arena, kids[ks].value.data, kids[ks].value.length);
        if (!nm) {
          return GTEXT_CSS_E_OOM;
        }
        for (size_t k = 0; k < kids[ks].value.length; k++) {
          nm[k] = (char)css_mq_lower((unsigned char)nm[k]);
        }
        nd->name = nm;
        nd->name_length = kids[ks].value.length;
        if (plain) {
          size_t vs = skip_ws(kids, ke, after + 1);
          char * vt = NULL;
          size_t vl = 0;
          st = gtext_css_serialize_values_buffer(kids + vs, ke - vs, &so, &vt, &vl);
          if (st != GTEXT_CSS_OK) {
            return st;
          }
          char * copy = css_arena_strdup(&in->arena, vt, vl);
          gtext_css_serialize_buffer_free(&so, vt);
          if (!copy) {
            return GTEXT_CSS_E_OOM;
          }
          nd->value = copy;
          nd->value_length = vl;
        }
      }
    }
  }
  return GTEXT_CSS_OK;
}

static void release(struct GTEXT_CSS_Media_List_Internal * in) {
  GTEXT_Allocator a = in->alloc;
  for (size_t i = 0; i < in->math_count; i++) {
    gtext_css_math_free(in->maths[i]);
  }
  for (size_t i = 0; i < in->node_count; i++) {
    if (in->nodes[i].kind == MQN_SELECTOR) {
      gtext_css_selector_list_free(in->nodes[i].sel);
    }
  }
  gtext_allocator_free(&a, in->maths);
  gtext_allocator_free(&a, in->nodes);
  gtext_allocator_free(&a, in->kids);
  gtext_allocator_free(&a, in->values);
  gtext_allocator_free(&a, in->roots);
  css_arena_free(&in->arena);
  /* `in` is the first member of the list: the block that holds it. */
  gtext_allocator_free(&a, in);
}

GTEXT_CSS_Media_List * gtext_css_media_list_retain(const GTEXT_CSS_Media_List * list) {
  if (list) {
    (void)gcu_atomic_int_fetch_add((GCU_Atomic_Int *)&list->in.refs, 1);
  }
  return (GTEXT_CSS_Media_List *)list;
}

void gtext_css_media_list_free(const GTEXT_CSS_Media_List * list) {
  if (!list) {
    return;
  }
  struct GTEXT_CSS_Media_List_Internal * in = (struct GTEXT_CSS_Media_List_Internal *)&list->in;
  if (gcu_atomic_int_fetch_sub(&in->refs, 1) != 1) {
    return;
  }
  release(in);
}

size_t gtext_css_media_list_query_count(const GTEXT_CSS_Media_List * list) {
  return list ? list->in.root_count : 0;
}

bool gtext_css_media_list_query_replaced(const GTEXT_CSS_Media_List * list, size_t index) {
  if (!list || index >= list->in.root_count) {
    return false;
  }
  return (list->in.nodes[list->in.roots[index]].flags & MQF_REPLACED) != 0;
}

size_t gtext_css_media_list_node_count(const GTEXT_CSS_Media_List * list) {
  return list ? list->in.node_count : 0;
}

/* ---- entry points -------------------------------------------------------- */

typedef enum { PI_MEDIA = 0, PI_SUPPORTS, PI_WHEN, PI_CONTAINER } parse_mode;

static GTEXT_CSS_Status parse_impl(const GTEXT_CSS_Component_Value * values, size_t count, const GTEXT_CSS_Media_Options * options, bool single,
    GTEXT_CSS_Media_List ** out, GTEXT_CSS_Media_Result * result, parse_mode mode, bool bare_ok) {
  const bool supports = mode == PI_SUPPORTS || mode == PI_WHEN; /* one condition, no comma list: the root of a supports condition and of a boolean condition are read alike */
  GTEXT_CSS_Media_Options o = options ? *options : gtext_css_media_options_default();
  if (!o.max_queries) {
    o.max_queries = 65536;
  }
  if (!o.max_nodes) {
    o.max_nodes = 1048576;
  }
  if (!o.max_depth) {
    o.max_depth = 256;
  }
  if (o.max_depth > ((size_t)1 << 24)) {
    o.max_depth = (size_t)1 << 24;
  }
  if (!o.max_diagnostics) {
    o.max_diagnostics = 1024;
  }
  const GTEXT_Allocator * al = o.allocator ? o.allocator : gtext_allocator_default();
  struct GTEXT_CSS_Media_List_Internal * in = gtext_allocator_calloc(al, 1, sizeof(GTEXT_CSS_Media_List));
  if (!in) {
    if (result) {
      result->status = GTEXT_CSS_E_OOM;
      result->message = gtext_css_status_string(GTEXT_CSS_E_OOM);
    }
    return GTEXT_CSS_E_OOM;
  }
  in->alloc = *al;
  gcu_atomic_int_init(&in->refs, 1);
  in->serial = gcu_atomic_size_fetch_add(&css_mq_list_serial, 1) + 1;
  css_arena_init(&in->arena, al);
  parser P;
  memset(&P, 0, sizeof P);
  P.o = o;
  P.in = in;
  P.res = result;
  P.alloc = *al;
  P.supports = mode == PI_SUPPORTS;
  P.bare_ok = bare_ok;
  P.when = mode == PI_WHEN;
  P.container = mode == PI_CONTAINER;
  bool container_valid = true;
  GTEXT_CSS_Span span0;
  memset(&span0, 0, sizeof span0);
  size_t start = 0;
  bool done = false;
  while (!done && P.status == GTEXT_CSS_OK) {
    size_t end = start;
    while (end < count && !(values[end].kind == GTEXT_CSS_VALUE_TOKEN && values[end].type == GTEXT_CSS_TOKEN_COMMA)) {
      end++;
    }
    if (supports) {
      end = count;
    }
    if (!single && !supports && skip_ws(values, count, 0) >= count) {
      break; /* the empty list: no tokens, or only white space */
    }
    if (in->root_count >= o.max_queries) {
      set_status(&P, GTEXT_CSS_E_LIMIT_CONDITIONS, end < count ? values[end].span : span0);
      break;
    }
    GTEXT_CSS_Span where = start < count ? values[start].span : (count ? values[count - 1].span : span0);
    uint32_t q;
    if (mode == PI_CONTAINER) {
      bool ok = true;
      q = parse_container_root(&P, values + start, end - start, where, &ok);
      container_valid = container_valid && ok;
    }
    else if (supports) {
      q = parse_supports_root(&P, values, count, where);
      done = true;
    } else if (single && end < count) {
      q = parse_query(&P, NULL, 0, where);
      if (q != MQ_NONE) {
        done = true;
      }
    } else {
      q = parse_query(&P, values + start, end - start, where);
    }
    if (q == MQ_NONE) {
      break;
    }
    if (!css_mq_grow(&P.alloc, (void **)&in->roots, &in->root_cap, in->root_count, 1, sizeof(uint32_t))) {
      set_status(&P, GTEXT_CSS_E_OOM, where);
      break;
    }
    in->roots[in->root_count++] = q;
    if (end >= count) {
      done = true;
    } else {
      start = end + 1;
    }
  }
  if (mode == PI_CONTAINER && P.status == GTEXT_CSS_OK && (!container_valid || in->root_count == 0)) {
    /* One <container-condition> that does not match the grammar makes the whole rule invalid: a list of one query that is `not all`. */
    drop_nodes(in, 0);
    in->kid_count = 0;
    in->value_count = 0;
    while (in->math_count > 0) {
      gtext_css_math_free(in->maths[--in->math_count]);
    }
    in->root_count = 0;
    const uint32_t q = new_node(&P, MQN_QUERY, count ? values[0].span : span0);
    if (q != MQ_NONE && css_mq_grow(&P.alloc, (void **)&in->roots, &in->root_cap, 0, 1, sizeof(uint32_t))) {
      in->nodes[q].flags = MQF_NEGATED | MQF_HAS_TYPE | MQF_REPLACED;
      in->nodes[q].type_id = MQT_ID_ALL;
      in->nodes[q].text = "all";
      in->nodes[q].text_length = 3;
      in->roots[in->root_count++] = q;
      P.replaced = 1;
      diag(&P, GTEXT_CSS_MQDIAG_SYNTAX, count ? values[0].span : span0, "Conditional 5, 5.4: a container condition that does not match the grammar makes the rule invalid");
    }
    else if (P.status == GTEXT_CSS_OK) {
      set_status(&P, GTEXT_CSS_E_OOM, span0);
    }
  }
  GTEXT_CSS_Status st = P.status;
  if (st == GTEXT_CSS_OK) {
    st = finish(&P);
  }
  gtext_allocator_free(&P.alloc, P.frames);
  gtext_allocator_free(&P.alloc, P.ops);
  if (result) {
    result->status = st;
    result->message = gtext_css_status_string(st);
    result->failure = P.failure;
    result->replaced = P.replaced;
  }
  if (st != GTEXT_CSS_OK) {
    release(in);
    return st;
  }
  *out = (GTEXT_CSS_Media_List *)in;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_media_list_parse_values(const GTEXT_CSS_Component_Value * values, size_t count, const GTEXT_CSS_Media_Options * options,
    GTEXT_CSS_Media_List ** out, GTEXT_CSS_Media_Result * result) {
  if (result) {
    memset(result, 0, sizeof *result);
    result->message = gtext_css_status_string(GTEXT_CSS_OK);
  }
  if (!out || (count && !values)) {
    return GTEXT_CSS_E_INVALID;
  }
  return parse_impl(values, count, options, false, out, result, PI_MEDIA, false);
}

GTEXT_CSS_Status gtext_css_media_query_parse_values(const GTEXT_CSS_Component_Value * values, size_t count, const GTEXT_CSS_Media_Options * options,
    GTEXT_CSS_Media_List ** out, GTEXT_CSS_Media_Result * result) {
  if (result) {
    memset(result, 0, sizeof *result);
    result->message = gtext_css_status_string(GTEXT_CSS_OK);
  }
  if (!out || (count && !values)) {
    return GTEXT_CSS_E_INVALID;
  }
  return parse_impl(values, count, options, true, out, result, PI_MEDIA, false);
}

GTEXT_CSS_Status gtext_css_media_list_parse(const char * text, size_t length, const GTEXT_CSS_Media_Options * options, GTEXT_CSS_Media_List ** out,
    GTEXT_CSS_Media_Result * result) {
  if (result) {
    memset(result, 0, sizeof *result);
    result->message = gtext_css_status_string(GTEXT_CSS_OK);
  }
  if (!out || (length && !text)) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Media_Options o = options ? *options : gtext_css_media_options_default();
  GTEXT_CSS_Parse_Options po = o.parse;
  if (!po.allocator) {
    po.allocator = o.allocator;
  }
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Result pr;
  memset(&pr, 0, sizeof pr);
  GTEXT_CSS_Status st = gtext_css_parse_component_values(text ? text : "", length, &po, &tree, &pr);
  if (st != GTEXT_CSS_OK) {
    if (result) {
      result->status = st;
      result->message = gtext_css_status_string(st);
      result->failure = pr.failure;
    }
    gtext_css_result_free(&pr);
    return st;
  }
  gtext_css_result_free(&pr);
  st = parse_impl(tree->values, tree->value_count, &o, false, out, result, PI_MEDIA, false);
  gtext_css_tree_free(tree);
  return st;
}

GTEXT_CSS_Status gtext_css_supports_parse_values(const GTEXT_CSS_Component_Value * values, size_t count, bool bare_declaration, const GTEXT_CSS_Media_Options * options,
    GTEXT_CSS_Supports_Condition ** out, GTEXT_CSS_Media_Result * result) {
  if (result) {
    memset(result, 0, sizeof *result);
    result->message = gtext_css_status_string(GTEXT_CSS_OK);
  }
  if (!out || (count && !values)) {
    return GTEXT_CSS_E_INVALID;
  }
  return parse_impl(values, count, options, true, out, result, PI_SUPPORTS, bare_declaration);
}

GTEXT_CSS_Status gtext_css_supports_parse(const char * text, size_t length, bool bare_declaration, const GTEXT_CSS_Media_Options * options,
    GTEXT_CSS_Supports_Condition ** out, GTEXT_CSS_Media_Result * result) {
  if (result) {
    memset(result, 0, sizeof *result);
    result->message = gtext_css_status_string(GTEXT_CSS_OK);
  }
  if (!out || (length && !text)) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Media_Options o = options ? *options : gtext_css_media_options_default();
  GTEXT_CSS_Parse_Options po = o.parse;
  if (!po.allocator) {
    po.allocator = o.allocator;
  }
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Result pr;
  memset(&pr, 0, sizeof pr);
  GTEXT_CSS_Status st = gtext_css_parse_component_values(text ? text : "", length, &po, &tree, &pr);
  if (st != GTEXT_CSS_OK) {
    if (result) {
      result->status = st;
      result->message = gtext_css_status_string(st);
      result->failure = pr.failure;
    }
    gtext_css_result_free(&pr);
    return st;
  }
  gtext_css_result_free(&pr);
  st = parse_impl(tree->values, tree->value_count, &o, true, out, result, PI_SUPPORTS, bare_declaration);
  gtext_css_tree_free(tree);
  return st;
}

GTEXT_CSS_Status gtext_css_when_parse_values(const GTEXT_CSS_Component_Value * values, size_t count, const GTEXT_CSS_Media_Options * options,
    GTEXT_CSS_Boolean_Condition ** out, GTEXT_CSS_Media_Result * result) {
  if (result) {
    memset(result, 0, sizeof *result);
    result->message = gtext_css_status_string(GTEXT_CSS_OK);
  }
  if (!out || (count && !values)) {
    return GTEXT_CSS_E_INVALID;
  }
  return parse_impl(values, count, options, true, out, result, PI_WHEN, false);
}

GTEXT_CSS_Status gtext_css_container_parse_values(const GTEXT_CSS_Component_Value * values, size_t count, const GTEXT_CSS_Media_Options * options,
    GTEXT_CSS_Container_Query ** out, GTEXT_CSS_Media_Result * result) {
  if (result) {
    memset(result, 0, sizeof *result);
    result->message = gtext_css_status_string(GTEXT_CSS_OK);
  }
  if (!out || (count && !values)) {
    return GTEXT_CSS_E_INVALID;
  }
  return parse_impl(values, count, options, false, out, result, PI_CONTAINER, false);
}

static GTEXT_CSS_Status parse_text(const char * text, size_t length, const GTEXT_CSS_Media_Options * options, parse_mode mode, GTEXT_CSS_Media_List ** out,
    GTEXT_CSS_Media_Result * result) {
  if (result) {
    memset(result, 0, sizeof *result);
    result->message = gtext_css_status_string(GTEXT_CSS_OK);
  }
  if (!out || (length && !text)) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Media_Options o = options ? *options : gtext_css_media_options_default();
  GTEXT_CSS_Parse_Options po = o.parse;
  if (!po.allocator) {
    po.allocator = o.allocator;
  }
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Result pr;
  memset(&pr, 0, sizeof pr);
  GTEXT_CSS_Status st = gtext_css_parse_component_values(text ? text : "", length, &po, &tree, &pr);
  if (st != GTEXT_CSS_OK) {
    if (result) {
      result->status = st;
      result->message = gtext_css_status_string(st);
      result->failure = pr.failure;
    }
    gtext_css_result_free(&pr);
    return st;
  }
  gtext_css_result_free(&pr);
  st = parse_impl(tree->values, tree->value_count, &o, mode != PI_CONTAINER, out, result, mode, false);
  gtext_css_tree_free(tree);
  return st;
}

GTEXT_CSS_Status gtext_css_when_parse(const char * text, size_t length, const GTEXT_CSS_Media_Options * options, GTEXT_CSS_Boolean_Condition ** out,
    GTEXT_CSS_Media_Result * result) {
  return parse_text(text, length, options, PI_WHEN, out, result);
}

GTEXT_CSS_Status gtext_css_container_parse(const char * text, size_t length, const GTEXT_CSS_Media_Options * options, GTEXT_CSS_Container_Query ** out,
    GTEXT_CSS_Media_Result * result) {
  return parse_text(text, length, options, PI_CONTAINER, out, result);
}
