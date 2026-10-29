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

static void rollback(parser * P, const frame * f) {
  struct GTEXT_CSS_Media_List_Internal * in = P->in;
  in->node_count = f->nmark;
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
      if (u != GTEXT_CSS_UNIT_NONE && gtext_css_unit_type(u) == GTEXT_CSS_UNIT_TYPE_RESOLUTION) {
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

static uint32_t make_enclosed(parser * P, const GTEXT_CSS_Component_Value * src) {
  uint32_t n = new_node(P, MQN_ENCLOSED, src->span);
  if (n != MQ_NONE) {
    P->in->nodes[n].src = src;
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
    if (r >= 0) {
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

/// Parse the contents [v, v+n) of a parenthesized block as `<media-feature>`; on FEAT_OK `*out` is the FEATURE or CUSTOM node.
static feat_result parse_feature(parser * P, const GTEXT_CSS_Component_Value * block, uint32_t * out) {
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
    name_of(&v[i], &row, &prefix, &via);
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
      if (boolean && v[i].value.length > 2 && v[i].value.data[0] == '-' && v[i].value.data[1] == '-') {
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
        name_of(&v[q.start], &r, &px, &via);
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
    name_of(&v[q.start], &row, &prefix, &via);
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
          uint32_t fn = make_enclosed(P, t);
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
    in->node_count = nmark + 1;
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
    const GTEXT_CSS_Component_Value * kids = src->children;
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

static GTEXT_CSS_Status parse_impl(const GTEXT_CSS_Component_Value * values, size_t count, const GTEXT_CSS_Media_Options * options, bool single,
    GTEXT_CSS_Media_List ** out, GTEXT_CSS_Media_Result * result) {
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
  css_arena_init(&in->arena, al);
  parser P;
  memset(&P, 0, sizeof P);
  P.o = o;
  P.in = in;
  P.res = result;
  P.alloc = *al;
  GTEXT_CSS_Span span0;
  memset(&span0, 0, sizeof span0);
  size_t start = 0;
  bool done = false;
  while (!done && P.status == GTEXT_CSS_OK) {
    size_t end = start;
    while (end < count && !(values[end].kind == GTEXT_CSS_VALUE_TOKEN && values[end].type == GTEXT_CSS_TOKEN_COMMA)) {
      end++;
    }
    if (!single && skip_ws(values, count, 0) >= count) {
      break; /* the empty list: no tokens, or only white space */
    }
    if (in->root_count >= o.max_queries) {
      set_status(&P, GTEXT_CSS_E_LIMIT_CONDITIONS, end < count ? values[end].span : span0);
      break;
    }
    GTEXT_CSS_Span where = start < count ? values[start].span : (count ? values[count - 1].span : span0);
    uint32_t q;
    if (single && end < count) {
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
  return parse_impl(values, count, options, false, out, result);
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
  return parse_impl(values, count, options, true, out, result);
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
  st = parse_impl(tree->values, tree->value_count, &o, false, out, result);
  gtext_css_tree_free(tree);
  return st;
}
