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
 * The selector parser (css_selector.h): component values to the compiled form. One recursive-descent reader over the values of a prelude, with the five lists of
 * the grammar as modes and the rows of the generated tables (css_selector_tables.h) saying what each pseudo-class and pseudo-element is, what it takes as an
 * argument and what may follow it. The recursion is bounded by `max_depth` (at most MAX_DEPTH_LIMIT), so the stack use is.
 */

#include <string.h>

#include "css_selector_internal.h"

#define DEFAULT_MAX_COMPONENTS ((size_t)1 << 20)
#define DEFAULT_MAX_LIST ((size_t)1 << 16)
#define DEFAULT_MAX_COMPOUNDS ((size_t)1024)
#define DEFAULT_MAX_DEPTH ((size_t)64)
#define MAX_DEPTH_LIMIT ((size_t)256)
#define DEFAULT_MAX_DIAGNOSTICS ((size_t)1024)
#define MAX_DIAGNOSTICS_LIMIT ((size_t)1 << 20)

/// Process-unique serials of parses. Zero is a valid initial value of an atomic.
static GCU_Atomic_Size uid_counter;

typedef GTEXT_CSS_Component_Value V;

/// A scratch vector of fixed-size elements. Indices, not pointers, are kept across pushes.
typedef struct {
  unsigned char * data;
  size_t len;
  size_t cap;
} vec;

/// What the context of a selector says about it (see the file's description in css_selector.h).
typedef struct {
  unsigned no_pe : 1;         ///< A pseudo-element is not allowed.
  unsigned in_has : 1;        ///< Inside `:has()`: `:has()` is not.
  unsigned compound_only : 1; ///< Logical arguments are single compounds (inside `:host()` and `::slotted()`).
  unsigned compound_list : 1; ///< A comma separated list of compounds (`::cue()`).
  unsigned restrict_kind : 2; ///< After a pseudo-element: 0 none, 1 no tree-asking pseudo-class, 2 user action pseudo-classes only.
  unsigned restrict_forgive : 1; ///< A violation of the restriction is accepted and the selector can never match.
  unsigned restrict_extra : 1;   ///< The restriction has extra pseudo-classes (the row of the pseudo-element names them).
  uint16_t restrict_row;      ///< The pseudo-element row whose `pclass_extra` applies.
  size_t depth;               ///< Functional pseudo-classes around.
} sctx;

typedef struct {
  const GTEXT_CSS_Selector_Options * o;
  struct GTEXT_CSS_Selector_List_Internal * in;
  css_arena * arena;
  GTEXT_Allocator alloc;
  GTEXT_CSS_Status status;
  GTEXT_CSS_Span fail;
  size_t nodes;
  size_t max_components;
  size_t max_list;
  size_t max_compounds;
  size_t max_depth;
  GTEXT_CSS_Id next_id;
  size_t uid;
  const GTEXT_CSS_Namespace_Map * ns;
  GTEXT_CSS_Selector_Diagnostic * diag;
  size_t ndiag;
  size_t capdiag;
  size_t total_diag;
  size_t keep_diag;
  vec simples;
  vec compounds;
  vec complexes;
  vec strings;
  vec flags;
  vec levels;
  vec attrs;
} P;

/* ---- scratch ------------------------------------------------------------------------------------------------ */

static bool vec_reserve(P * p, vec * v, size_t esz, size_t more) {
  if (v->len + more <= v->cap) {
    return true;
  }
  size_t cap = v->cap ? v->cap : 16;
  while (cap < v->len + more) {
    if (cap > ((size_t)-1) / 2 / esz) {
      p->status = GTEXT_CSS_E_OOM;
      return false;
    }
    cap *= 2;
  }
  void * d = gtext_allocator_realloc(&p->alloc, v->data, cap * esz);
  if (!d) {
    p->status = GTEXT_CSS_E_OOM;
    return false;
  }
  v->data = d;
  v->cap = cap;
  return true;
}

#define VEC_AT(v, T, i) (((T *)(v).data)[i])


static bool vec_push(P * p, vec * v, size_t esz, const void * item) {
  if (!vec_reserve(p, v, esz, 1)) {
    return false;
  }
  memcpy(v->data + v->len * esz, item, esz);
  v->len++;
  return true;
}

static void vec_free(P * p, vec * v) {
  gtext_allocator_free(&p->alloc, v->data);
  v->data = NULL;
  v->len = v->cap = 0;
}

/// Copy `n` elements from `base` of a scratch vector to the arena; the vector is cut back to `base`.
static void * take(P * p, vec * v, size_t esz, size_t base) {
  const size_t n = v->len - base;
  void * out = NULL;
  if (n) {
    out = css_arena_alloc(p->arena, n * esz);
    if (!out) {
      p->status = GTEXT_CSS_E_OOM;
    }
    else {
      memcpy(out, v->data + base * esz, n * esz);
    }
  }
  v->len = base;
  return out;
}

static GTEXT_CSS_String sdup(P * p, const char * s, size_t n) {
  GTEXT_CSS_String r = {NULL, 0};
  char * d = css_arena_strdup(p->arena, s ? s : "", n);
  if (!d) {
    p->status = GTEXT_CSS_E_OOM;
    return r;
  }
  r.data = d;
  r.length = n;
  return r;
}

static GTEXT_CSS_String sdup_lower(P * p, const char * s, size_t n) {
  GTEXT_CSS_String r = sdup(p, s, n);
  if (r.data) {
    char * m = (char *)r.data;
    for (size_t i = 0; i < n; i++) {
      m[i] = (char)css_sel_lower((unsigned char)m[i]);
    }
  }
  return r;
}

/* ---- diagnostics and limits ---------------------------------------------------------------------------------- */

static GTEXT_CSS_Span span_of(const V * v, size_t lo, size_t hi) {
  GTEXT_CSS_Span s;
  memset(&s, 0, sizeof s);
  if (hi > lo) {
    s = v[lo].span;
    s.end = v[hi - 1].span.end;
  }
  return s;
}

static void diag(P * p, GTEXT_CSS_Selector_Diag_Kind kind, GTEXT_CSS_Span span, const char * reference) {
  p->total_diag++;
  if (p->ndiag >= p->keep_diag) {
    return;
  }
  if (p->ndiag == p->capdiag) {
    size_t cap = p->capdiag ? p->capdiag * 2 : 8;
    if (cap > p->keep_diag) {
      cap = p->keep_diag;
    }
    GTEXT_CSS_Selector_Diagnostic * d = gtext_allocator_realloc(&p->alloc, p->diag, cap * sizeof *d);
    if (!d) {
      p->status = GTEXT_CSS_E_OOM;
      return;
    }
    p->diag = d;
    p->capdiag = cap;
  }
  p->diag[p->ndiag].kind = kind;
  p->diag[p->ndiag].span = span;
  p->diag[p->ndiag].reference = reference;
  p->ndiag++;
}

static bool limit(P * p, GTEXT_CSS_Status st, GTEXT_CSS_Span at) {
  if (p->status == GTEXT_CSS_OK) {
    p->status = st;
    p->fail = at;
  }
  return false;
}

/// Count a node; false (and the parse failed) past the limit.
static bool node(P * p, GTEXT_CSS_Span at, GTEXT_CSS_Id * id) {
  if (p->status != GTEXT_CSS_OK) {
    return false;
  }
  if (p->nodes >= p->max_components) {
    return limit(p, GTEXT_CSS_E_LIMIT_SELECTOR_COMPONENTS, at);
  }
  p->nodes++;
  *id = ++p->next_id;
  return true;
}

/* ---- the values ------------------------------------------------------------------------------------------------ */

static bool is_ws(const V * v) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_WHITESPACE;
}

static bool is_tok(const V * v, GTEXT_CSS_Token_Type t) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == t;
}

static bool is_delim(const V * v, uint32_t cp) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_DELIM && v->delim == cp;
}

static bool is_ident(const V * v) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_IDENT;
}

static bool is_function(const V * v) {
  return v->kind == GTEXT_CSS_VALUE_FUNCTION;
}

static bool is_block(const V * v, GTEXT_CSS_Token_Type open) {
  return v->kind == GTEXT_CSS_VALUE_BLOCK && v->type == open;
}

static bool is_comma(const V * v) {
  return is_tok(v, GTEXT_CSS_TOKEN_COMMA);
}

static size_t skip_ws(const V * v, size_t pos, size_t hi) {
  while (pos < hi && is_ws(&v[pos])) {
    pos++;
  }
  return pos;
}

/// Narrow [lo, hi) past the white space at both ends.
static void trim(const V * v, size_t * lo, size_t * hi) {
  while (*lo < *hi && is_ws(&v[*lo])) {
    (*lo)++;
  }
  while (*hi > *lo && is_ws(&v[*hi - 1])) {
    (*hi)--;
  }
}

/* ---- references: where in the drafts a refusal is decided -------------------------------------------------- */

#define REF_GRAMMAR "Selectors 4, 18 (grammar): the selector does not match the grammar"
#define REF_UNKNOWN "Selectors 4, 3.1: an unknown pseudo-class or pseudo-element makes the selector invalid"
#define REF_FORGIVING "Selectors 4, 18.2: a forgiving selector list drops the entries that are not valid selectors"
#define REF_PE_LAST "Selectors 4, 3.6.1: a pseudo-element ends its complex selector (or is followed by the pseudo-elements and pseudo-classes it allows)"
#define REF_NOT_IN "Selectors 4, 4.3 and 4.4: :not(), :is() and :has() take no pseudo-element, and :has() does not nest"
#define REF_PREFIX "Selectors 4, 6.3: a namespace prefix must be declared"
#define REF_ARG "Selectors 4, 3 and 18: the argument is not what the pseudo-class takes"
#define REF_AFTER_PART "CSS Shadow Parts 1, 3: ::part() takes pseudo-classes after it that do not ask the tree"
#define REF_EMPTY "Selectors 4, 3.1: a selector list needs a selector"

/* ---- the table ---------------------------------------------------------------------------------------------- */

/// The row of a pseudo-class (`element` false) or pseudo-element, by lower-case name, parentheses and the number of colons written; -1 for none.
static int find_row(bool element, bool functional, const char * name, size_t len, int colons) {
  for (int i = 0; i < GTEXT_CSS_SEL_ROW_COUNT; i++) {
    const sel_row * r = &sel_rows[i];
    if (r->kind != (element ? SELK_ELEMENT : SELK_CLASS) || (bool)r->functional != functional) {
      continue;
    }
    if (element) {
      if (r->colons != colons) {
        continue;
      }
    }
    else if (r->colons != 1) {
      continue;
    }
    if (strlen(r->base) == len && memcmp(r->base, name, len) == 0) {
      return i;
    }
  }
  return -1;
}

int gtext_css_selector_find_row(const char * name) {
  if (!name) {
    return -1;
  }
  for (int i = 0; i < GTEXT_CSS_SEL_ROW_COUNT; i++) {
    if (strcmp(sel_rows[i].name, name) == 0) {
      return i;
    }
  }
  return -1;
}

size_t gtext_css_selector_row_count(void) {
  return (size_t)GTEXT_CSS_SEL_ROW_COUNT;
}

bool gtext_css_selector_row(size_t index, GTEXT_CSS_Selector_Row * out) {
  if (index >= (size_t)GTEXT_CSS_SEL_ROW_COUNT || !out) {
    return false;
  }
  const sel_row * r = &sel_rows[index];
  out->name = r->name;
  out->base = r->base;
  out->note = r->note;
  out->state = r->state;
  out->callbacks = r->callbacks;
  out->dependencies = r->dep;
  out->pseudo_element = r->kind == SELK_ELEMENT;
  out->implemented = r->status == SELS_IMPLEMENTED;
  out->parse_only = r->status == SELS_PARSE_ONLY;
  return true;
}

/* ---- forward declarations --------------------------------------------------------------------------------- */

static bool parse_list(P * p, const V * v, size_t lo, size_t hi, GTEXT_CSS_Selector_Mode mode, const sctx * c, GTEXT_CSS_Span fallback,
    GTEXT_CSS_Selector_List ** out);

/* ---- type, universal, namespace ---------------------------------------------------------------------------- */

static const GTEXT_CSS_Namespace * ns_find(P * p, const char * prefix, size_t n) {
  if (!p->ns) {
    return NULL;
  }
  for (size_t i = 0; i < p->ns->count; i++) {
    const GTEXT_CSS_Namespace * e = &p->ns->entries[i];
    if (e->prefix && strlen(e->prefix) == n && memcmp(e->prefix, prefix, n) == 0) {
      return e;
    }
  }
  return NULL;
}

/// Read `[prefix] '|' name` / `name` for a type or attribute selector at v[*pos]: fills the namespace fields and `name` (empty for `*`), sets `*star`
/// for a universal name. false: not this construct (nothing consumed), or invalid (a diagnostic).
typedef enum { QN_NO = 0, QN_YES, QN_BAD } qn_result;

static qn_result qualified_name(P * p, const V * v, size_t * pos, size_t hi, bool is_attr, GTEXT_CSS_Simple * s, bool * star) {
  size_t i = *pos;
  if (i >= hi) {
    return QN_NO;
  }
  enum { PFX_NONE, PFX_ANY, PFX_EMPTY, PFX_NAME } pfx = PFX_NONE;
  const V * prefix_v = NULL;
  size_t j = i;
  // an optional prefix: ident | '*' | nothing, then '|'
  if ((is_ident(&v[j]) || is_delim(&v[j], '*')) && j + 1 < hi && is_delim(&v[j + 1], '|') && j + 2 < hi &&
      (is_ident(&v[j + 2]) || is_delim(&v[j + 2], '*'))) {
    pfx = is_delim(&v[j], '*') ? PFX_ANY : PFX_NAME;
    if (pfx == PFX_NAME) {
      prefix_v = &v[j];
    }
    j += 2;
  }
  else if (is_delim(&v[j], '|') && j + 1 < hi && (is_ident(&v[j + 1]) || is_delim(&v[j + 1], '*'))) {
    pfx = PFX_EMPTY;
    j += 1;
  }
  if (j >= hi || !(is_ident(&v[j]) || is_delim(&v[j], '*'))) {
    return QN_NO;
  }
  if (is_attr && is_delim(&v[j], '*')) {
    // `[*]` and `[ns|*]` are not attribute names
    return QN_NO;
  }
  const V * name_v = &v[j];
  *star = is_delim(name_v, '*');
  s->name = *star ? (GTEXT_CSS_String){"", 0} : sdup(p, name_v->value.data, name_v->value.length);
  s->ns_kind = GTEXT_CSS_NS_UNSPECIFIED;
  s->ns_prefix = (GTEXT_CSS_String){NULL, 0};
  s->ns_uri = (GTEXT_CSS_String){NULL, 0};
  s->ns_from_default = false;
  if (pfx == PFX_ANY) {
    s->ns_kind = GTEXT_CSS_NS_ANY;
  }
  else if (pfx == PFX_EMPTY) {
    s->ns_kind = GTEXT_CSS_NS_NONE;
  }
  else if (pfx == PFX_NAME) {
    const GTEXT_CSS_Namespace * e = ns_find(p, prefix_v->value.data, prefix_v->value.length);
    if (!e) {
      diag(p, GTEXT_CSS_SELDIAG_PREFIX, prefix_v->span, REF_PREFIX);
      return QN_BAD;
    }
    s->ns_kind = GTEXT_CSS_NS_NAMED;
    s->ns_prefix = sdup(p, prefix_v->value.data, prefix_v->value.length);
    s->ns_uri = sdup(p, e->uri ? e->uri : "", e->uri ? strlen(e->uri) : 0);
  }
  if (is_attr && pfx == PFX_EMPTY) {
    // an attribute with no prefix has no namespace, which is what `|` says
    s->ns_kind = GTEXT_CSS_NS_UNSPECIFIED;
  }
  if (!is_attr) {
    if (pfx == PFX_NONE && p->ns && p->ns->default_uri) {
      s->ns_uri = sdup(p, p->ns->default_uri, strlen(p->ns->default_uri));
      s->ns_from_default = true;
    }
    else if (pfx == PFX_ANY && !(p->ns && p->ns->default_uri)) {
      // `*|` with no default namespace is what no prefix is: it is not kept
      s->ns_kind = GTEXT_CSS_NS_UNSPECIFIED;
    }
  }
  *pos = j + 1;
  return QN_YES;
}

/* ---- attribute selectors ----------------------------------------------------------------------------------- */

static bool parse_attribute(P * p, const V * blk, GTEXT_CSS_Simple * s) {
  const V * v = blk->children;
  size_t hi = blk->child_count;
  size_t pos = skip_ws(v, 0, hi);
  bool star = false;
  qn_result q = qualified_name(p, v, &pos, hi, true, s, &star);
  if (q != QN_YES) {
    if (q == QN_NO) {
      diag(p, GTEXT_CSS_SELDIAG_SYNTAX, blk->span, REF_GRAMMAR);
    }
    return false;
  }
  s->kind = GTEXT_CSS_SIMPLE_ATTRIBUTE;
  s->op = GTEXT_CSS_ATTR_EXISTS;
  pos = skip_ws(v, pos, hi);
  if (pos == hi) {
    return true;
  }
  // the operator: '=' or one of ~ | ^ $ * followed by '='
  if (is_delim(&v[pos], '=')) {
    s->op = GTEXT_CSS_ATTR_EQUALS;
    pos++;
  }
  else if (pos + 1 < hi && is_delim(&v[pos + 1], '=')) {
    switch (v[pos].kind == GTEXT_CSS_VALUE_TOKEN && v[pos].type == GTEXT_CSS_TOKEN_DELIM ? v[pos].delim : 0) {
    case '~': s->op = GTEXT_CSS_ATTR_INCLUDES; break;
    case '|': s->op = GTEXT_CSS_ATTR_DASH; break;
    case '^': s->op = GTEXT_CSS_ATTR_PREFIX; break;
    case '$': s->op = GTEXT_CSS_ATTR_SUFFIX; break;
    case '*': s->op = GTEXT_CSS_ATTR_SUBSTRING; break;
    default:
      diag(p, GTEXT_CSS_SELDIAG_SYNTAX, blk->span, REF_GRAMMAR);
      return false;
    }
    pos += 2;
  }
  else {
    diag(p, GTEXT_CSS_SELDIAG_SYNTAX, blk->span, REF_GRAMMAR);
    return false;
  }
  pos = skip_ws(v, pos, hi);
  if (pos >= hi || !(is_ident(&v[pos]) || is_tok(&v[pos], GTEXT_CSS_TOKEN_STRING))) {
    diag(p, GTEXT_CSS_SELDIAG_SYNTAX, blk->span, REF_GRAMMAR);
    return false;
  }
  s->value = sdup(p, v[pos].value.data, v[pos].value.length);
  pos++;
  pos = skip_ws(v, pos, hi);
  if (pos < hi) {
    if (is_ident(&v[pos]) && (css_sel_ieq(v[pos].value.data, v[pos].value.length, "i", 1) || css_sel_ieq(v[pos].value.data, v[pos].value.length, "s", 1))) {
      s->attr_case = css_sel_lower((unsigned char)v[pos].value.data[0]) == 'i' ? GTEXT_CSS_ATTR_CASE_INSENSITIVE : GTEXT_CSS_ATTR_CASE_SENSITIVE;
      pos++;
      pos = skip_ws(v, pos, hi);
    }
  }
  if (pos != hi) {
    diag(p, GTEXT_CSS_SELDIAG_SYNTAX, blk->span, REF_GRAMMAR);
    return false;
  }
  return true;
}

/* ---- arguments of pseudo-classes and pseudo-elements ------------------------------------------------------- */

static bool arg_fail(P * p, GTEXT_CSS_Span at) {
  diag(p, GTEXT_CSS_SELDIAG_ARGUMENT, at, REF_ARG);
  return false;
}

static bool push_string(P * p, const char * s, size_t n, bool quoted) {
  GTEXT_CSS_String str = sdup(p, s, n);
  if (p->status != GTEXT_CSS_OK) {
    return false;
  }
  const bool q = quoted;
  return vec_push(p, &p->strings, sizeof str, &str) && vec_push(p, &p->flags, sizeof q, &q);
}

/// The single ident or string between [lo, hi) after trimming; NULL if it is not exactly one token.
static const V * sole_token(const V * v, size_t lo, size_t hi) {
  trim(v, &lo, &hi);
  return hi - lo == 1 ? &v[lo] : NULL;
}

static bool enum_has(const char * list, const char * s, size_t n, size_t * which_len, const char ** which) {
  const char * q = list;
  while (*q) {
    const char * e = strchr(q, ',');
    size_t ln = e ? (size_t)(e - q) : strlen(q);
    if (css_sel_ieq(q, ln, s, n)) {
      *which = q;
      *which_len = ln;
      return true;
    }
    q += ln + (e ? 1 : 0);
  }
  return false;
}

/// Store the pushed strings from `sbase` (and flags) in the simple.
static void seal_strings(P * p, size_t sbase, GTEXT_CSS_Simple * s) {
  const size_t n = p->strings.len - sbase;
  s->string_count = n;
  if (n) {
    s->strings = take(p, &p->strings, sizeof(GTEXT_CSS_String), sbase);
    s->string_quoted = take(p, &p->flags, sizeof(bool), sbase);
  }
}

/// Read the argument of the functional pseudo `s->pseudo`, whose arguments are in `fn->children`. `c` is the context of the pseudo.
static bool parse_argument(P * p, const V * fn, const sel_row * row, GTEXT_CSS_Simple * s, const sctx * c) {
  const V * v = fn->children;
  const size_t n = fn->child_count;
  const size_t sbase = p->strings.len;
  sctx nested = *c;
  nested.depth = c->depth + 1;
  nested.restrict_kind = c->restrict_kind;
  switch ((sel_arg)row->arg) {
  case SELARG_FORGIVING: {
    nested.no_pe = 1;
    nested.compound_list = 0;
    GTEXT_CSS_Selector_List * l = NULL;
    // an :is() after a pseudo-element: what it holds is checked against the restriction, and accepted if it breaks it
    if (c->restrict_kind) {
      nested.restrict_forgive = 1;
    }
    if (!parse_list(p, v, 0, n, GTEXT_CSS_SELECTOR_FORGIVING_LIST, &nested, fn->span, &l)) {
      return false;
    }
    s->list = l;
    return true;
  }
  case SELARG_REAL: {
    nested.no_pe = 1;
    nested.compound_list = 0;
    nested.restrict_forgive = 0;
    GTEXT_CSS_Selector_List * l = NULL;
    if (!parse_list(p, v, 0, n, GTEXT_CSS_SELECTOR_REAL_LIST, &nested, fn->span, &l)) {
      return false;
    }
    s->list = l;
    return true;
  }
  case SELARG_RELATIVE: {
    nested.no_pe = 1;
    nested.in_has = 1;
    nested.compound_only = 0;
    nested.compound_list = 0;
    nested.restrict_kind = 0;
    GTEXT_CSS_Selector_List * l = NULL;
    if (!parse_list(p, v, 0, n, GTEXT_CSS_SELECTOR_RELATIVE_LIST, &nested, fn->span, &l)) {
      return false;
    }
    s->list = l;
    return true;
  }
  case SELARG_COMPOUND:
  case SELARG_COMPOUND_LIST: {
    nested.no_pe = 1;
    nested.compound_only = 1;
    nested.compound_list = row->arg == SELARG_COMPOUND_LIST;
    nested.restrict_kind = 0;
    GTEXT_CSS_Selector_List * l = NULL;
    if (!parse_list(p, v, 0, n, GTEXT_CSS_SELECTOR_COMPOUND, &nested, fn->span, &l)) {
      return false;
    }
    s->list = l;
    return true;
  }
  case SELARG_ANB: {
    int64_t a, b;
    if (!gtext_css_anb_parse(v, n, &a, &b)) {
      return arg_fail(p, fn->span);
    }
    s->anb_a = a;
    s->anb_b = b;
    return true;
  }
  case SELARG_ANB_OF: {
    size_t split = n;
    for (size_t i = 1; i < n; i++) {
      if (is_ident(&v[i]) && css_sel_ieq(v[i].value.data, v[i].value.length, "of", 2)) {
        // white space before it is not required: `2n+1/**/of .x` has a comment there, and a digit followed by letters would have been one dimension token
        split = i;
        break;
      }
    }
    int64_t a, b;
    if (!gtext_css_anb_parse(v, split, &a, &b)) {
      return arg_fail(p, fn->span);
    }
    s->anb_a = a;
    s->anb_b = b;
    if (split < n) {
      nested.no_pe = 1;
      nested.compound_list = 0;
      nested.restrict_kind = 0;
      GTEXT_CSS_Selector_List * l = NULL;
      if (!parse_list(p, v, split + 1, n, GTEXT_CSS_SELECTOR_REAL_LIST, &nested, fn->span, &l)) {
        return false;
      }
      s->list = l;
      s->has_of = true;
    }
    return true;
  }
  case SELARG_LANG: {
    size_t lo = 0;
    bool any = false;
    while (lo <= n) {
      size_t hi = lo;
      while (hi < n && !is_comma(&v[hi])) {
        hi++;
      }
      const V * t = sole_token(v, lo, hi);
      if (!t || !(is_ident(t) || is_tok(t, GTEXT_CSS_TOKEN_STRING))) {
        return arg_fail(p, fn->span);
      }
      if (!push_string(p, t->value.data, t->value.length, t->type == GTEXT_CSS_TOKEN_STRING)) {
        return false;
      }
      any = true;
      if (hi >= n) {
        break;
      }
      lo = hi + 1;
    }
    if (!any) {
      return arg_fail(p, fn->span);
    }
    seal_strings(p, sbase, s);
    return p->status == GTEXT_CSS_OK;
  }
  case SELARG_DIR: {
    const V * t = sole_token(v, 0, n);
    // any identifier is valid (Selectors 4 grammar: `:dir( <ident> )`); one that is not a direction matches nothing, as in the browsers
    if (!t || !is_ident(t)) {
      return arg_fail(p, fn->span);
    }
    GTEXT_CSS_String d = sdup_lower(p, t->value.data, t->value.length);
    if (!push_string(p, d.data, d.length, false)) {
      return false;
    }
    seal_strings(p, sbase, s);
    return p->status == GTEXT_CSS_OK;
  }
  case SELARG_LEVELS: {
    size_t lo = 0;
    const size_t lbase = p->levels.len;
    for (;;) {
      size_t hi = lo;
      while (hi < n && !is_comma(&v[hi])) {
        hi++;
      }
      const V * t = sole_token(v, lo, hi);
      if (!t || !is_tok(t, GTEXT_CSS_TOKEN_NUMBER) || !(t->flags & GTEXT_CSS_FLAG_INTEGER)) {
        p->levels.len = lbase;
        return arg_fail(p, fn->span);
      }
      double d = t->number;
      int64_t iv = d >= 9.2e18 ? INT64_MAX : d <= -9.2e18 ? -INT64_MAX : (int64_t)d;
      if (!vec_push(p, &p->levels, sizeof iv, &iv)) {
        return false;
      }
      if (hi >= n) {
        break;
      }
      lo = hi + 1;
    }
    s->level_count = p->levels.len - lbase;
    s->levels = take(p, &p->levels, sizeof(int64_t), lbase);
    return p->status == GTEXT_CSS_OK;
  }
  case SELARG_STATE: {
    const V * t = sole_token(v, 0, n);
    if (!t || !is_ident(t)) {
      return arg_fail(p, fn->span);
    }
    if (!push_string(p, t->value.data, t->value.length, false)) {
      return false;
    }
    seal_strings(p, sbase, s);
    return p->status == GTEXT_CSS_OK;
  }
  case SELARG_IDENTS_COMMA:
  case SELARG_PART_IDENTS: {
    const bool comma = row->arg == SELARG_IDENTS_COMMA;
    size_t lo = 0;
    if (comma) {
      for (;;) {
        size_t hi = lo;
        while (hi < n && !is_comma(&v[hi])) {
          hi++;
        }
        const V * t = sole_token(v, lo, hi);
        if (!t || !is_ident(t)) {
          return arg_fail(p, fn->span);
        }
        if (!push_string(p, t->value.data, t->value.length, false)) {
          return false;
        }
        if (hi >= n) {
          break;
        }
        lo = hi + 1;
      }
    }
    else {
      size_t i = skip_ws(v, 0, n);
      if (i == n) {
        return arg_fail(p, fn->span);
      }
      while (i < n) {
        if (!is_ident(&v[i])) {
          return arg_fail(p, fn->span);
        }
        if (!push_string(p, v[i].value.data, v[i].value.length, false)) {
          return false;
        }
        i++;
        const size_t j = skip_ws(v, i, n);
        if (j == i && j < n) {
          return arg_fail(p, fn->span);
        }
        i = j;
      }
    }
    seal_strings(p, sbase, s);
    return p->status == GTEXT_CSS_OK;
  }
  case SELARG_NAME_OR_STAR: {
    const V * t = sole_token(v, 0, n);
    if (!t) {
      return arg_fail(p, fn->span);
    }
    if (is_delim(t, '*')) {
      s->anb_a = 1;
      if (!push_string(p, "*", 1, false)) {
        return false;
      }
    }
    else if (is_ident(t)) {
      if (!push_string(p, t->value.data, t->value.length, false)) {
        return false;
      }
    }
    else {
      return arg_fail(p, fn->span);
    }
    seal_strings(p, sbase, s);
    return p->status == GTEXT_CSS_OK;
  }
  case SELARG_PT_NAME: {
    size_t i = skip_ws(v, 0, n);
    size_t hi = n;
    while (hi > i && is_ws(&v[hi - 1])) {
      hi--;
    }
    if (i >= hi) {
      return arg_fail(p, fn->span);
    }
    if (is_delim(&v[i], '*')) {
      s->anb_a = 1;
      if (!push_string(p, "*", 1, false)) {
        return false;
      }
    }
    else if (is_ident(&v[i])) {
      if (!push_string(p, v[i].value.data, v[i].value.length, false)) {
        return false;
      }
    }
    else {
      return arg_fail(p, fn->span);
    }
    i++;
    while (i < hi) {
      if (!is_delim(&v[i], '.') || i + 1 >= hi || !is_ident(&v[i + 1])) {
        return arg_fail(p, fn->span);
      }
      if (!push_string(p, v[i + 1].value.data, v[i + 1].value.length, false)) {
        return false;
      }
      i += 2;
    }
    seal_strings(p, sbase, s);
    return p->status == GTEXT_CSS_OK;
  }
  case SELARG_ENUM: {
    const V * t = sole_token(v, 0, n);
    if (!t) {
      return arg_fail(p, fn->span);
    }
    const char * which = NULL;
    size_t wl = 0;
    if (is_delim(t, '*')) {
      if (!enum_has(row->enum_values, "*", 1, &wl, &which)) {
        return arg_fail(p, fn->span);
      }
      s->anb_a = 1;
    }
    else if (!is_ident(t) || !enum_has(row->enum_values, t->value.data, t->value.length, &wl, &which)) {
      return arg_fail(p, fn->span);
    }
    if (!push_string(p, which, wl, false)) {
      return false;
    }
    seal_strings(p, sbase, s);
    return p->status == GTEXT_CSS_OK;
  }
  default:
    return arg_fail(p, fn->span);
  }
}

/* ---- pseudo-classes and pseudo-elements ------------------------------------------------------------------- */

/// What a compound holds so far, for the rules about pseudo-elements.
typedef struct {
  int last_pe;             ///< The row of the last pseudo-element, or -1.
  bool saw_pe;
} cstate;

static bool row_is_logical(const sel_row * r) {
  return r->spec == SELSPEC_MAX && r->functional && r->kind == SELK_CLASS && (r->arg == SELARG_FORGIVING || r->arg == SELARG_REAL);
}

/// Parse the pseudo-class or pseudo-element at v[*pos] (a colon). `st` carries the pseudo-element chain of the compound.
static bool parse_pseudo(P * p, const V * v, size_t * pos, size_t hi, const sctx * c, cstate * st, GTEXT_CSS_Simple * s) {
  const size_t start = *pos;
  size_t i = start + 1;
  int colons = 1;
  if (i < hi && is_tok(&v[i], GTEXT_CSS_TOKEN_COLON)) {
    colons = 2;
    i++;
  }
  if (i >= hi) {
    diag(p, GTEXT_CSS_SELDIAG_SYNTAX, v[start].span, REF_GRAMMAR);
    return false;
  }
  const V * nv = &v[i];
  bool functional;
  if (is_ident(nv)) {
    functional = false;
  }
  else if (is_function(nv)) {
    functional = true;
  }
  else {
    diag(p, GTEXT_CSS_SELDIAG_SYNTAX, nv->span, REF_GRAMMAR);
    return false;
  }
  GTEXT_CSS_Span here = v[start].span;
  here.end = nv->span.end;
  char lower[64];
  const GTEXT_CSS_String * nm = &nv->value;
  int row = -1;
  if (nm->length < sizeof lower) {
    for (size_t k = 0; k < nm->length; k++) {
      lower[k] = (char)css_sel_lower((unsigned char)nm->data[k]);
    }
    lower[nm->length] = '\0';
    if (colons == 1) {
      row = find_row(false, functional, lower, nm->length, 1);
      if (row < 0) {
        row = find_row(true, functional, lower, nm->length, 1);
      }
    }
    else {
      row = find_row(true, functional, lower, nm->length, 2);
    }
  }
  if (row < 0 || sel_rows[row].status == SELS_UNSUPPORTED) {
    diag(p, GTEXT_CSS_SELDIAG_UNKNOWN_SELECTOR, here, REF_UNKNOWN);
    return false;
  }
  const sel_row * r = &sel_rows[row];
  const bool is_pe = r->kind == SELK_ELEMENT;
  s->span = here;
  s->functional = functional;
  s->kind = is_pe ? GTEXT_CSS_SIMPLE_PSEUDO_ELEMENT : GTEXT_CSS_SIMPLE_PSEUDO_CLASS;
  s->pseudo = r->alias_row;
  const sel_row * canon = &sel_rows[r->alias_row];
  s->name = sdup(p, canon->base, strlen(canon->base));
  s->legacy = is_pe && r->colons == 1;
  if (is_pe) {
    if (c->no_pe) {
      diag(p, GTEXT_CSS_SELDIAG_NOT_ALLOWED, here, REF_NOT_IN);
      return false;
    }
    if (st->saw_pe) {
      const sel_row * prev = &sel_rows[st->last_pe];
      if (s->legacy || !(prev->follow & (1ull << canon->pe_index))) {
        diag(p, GTEXT_CSS_SELDIAG_NOT_ALLOWED, here, REF_PE_LAST);
        return false;
      }
    }
    st->saw_pe = true;
    st->last_pe = (int)r->alias_row;
  }
  else {
    // a pseudo-class after a pseudo-element
    if (r->kind == SELK_CLASS && c->no_pe == 0 && st->saw_pe) {
      const sel_row * pe = &sel_rows[st->last_pe];
      bool ok;
      switch (pe->pclass) {
      case PCLASS_NONE: ok = canon->arg == SELARG_FORGIVING; break;
      case PCLASS_USER: ok = (canon->flags & SELF_UA) || row_is_logical(canon); break;
      case PCLASS_NONTREE: ok = !(canon->flags & SELF_TREE); break;
      default: ok = false; break;
      }
      if (!ok) {
        for (int k = 0; k < 3 && pe->pclass_extra[k] != 0xffff; k++) {
          if (pe->pclass_extra[k] == r->alias_row) {
            ok = true;
          }
        }
      }
      if (!ok) {
        diag(p, GTEXT_CSS_SELDIAG_NOT_ALLOWED, here, REF_AFTER_PART);
        return false;
      }
    }
    if (canon->arg == SELARG_RELATIVE && c->in_has) {
      diag(p, GTEXT_CSS_SELDIAG_NOT_ALLOWED, here, REF_NOT_IN);
      return false;
    }
  }
  if (functional) {
    sctx cc = *c;
    if (!is_pe && st->saw_pe) {
      const sel_row * pe = &sel_rows[st->last_pe];
      cc.restrict_kind = pe->pclass == PCLASS_USER ? 2 : pe->pclass == PCLASS_NONTREE ? 1 : 3;
      cc.restrict_extra = 0;
    }
    else if (is_pe) {
      cc.restrict_kind = 0;
    }
    if (!parse_argument(p, nv, canon, s, &cc)) {
      return false;
    }
  }
  else if (canon->arg != SELARG_NONE && canon->arg != SELARG_TYPE) {
    // a name that needs an argument, written without one
    if (canon->functional) {
      diag(p, GTEXT_CSS_SELDIAG_ARGUMENT, here, REF_ARG);
      return false;
    }
  }
  *pos = i + 1;
  return true;
}

/* ---- compounds and complex selectors ----------------------------------------------------------------------- */

static bool restricted_violation(const sctx * c, const GTEXT_CSS_Simple * s) {
  if (!c->restrict_kind) {
    return false;
  }
  if (s->kind != GTEXT_CSS_SIMPLE_PSEUDO_CLASS || c->restrict_kind == 3) {
    return true;
  }
  const sel_row * r = css_sel_row_of(s);
  if (c->restrict_kind == 1) {
    return (r->flags & SELF_TREE) != 0;
  }
  return !((r->flags & SELF_UA) || row_is_logical(r));
}

/// Read one compound at v[*pos]. false: invalid (a diagnostic) or failed. `restricted_out` is set when a simple broke the restriction and was accepted.
static bool parse_compound(P * p, const V * v, size_t * pos, size_t hi, const sctx * c, GTEXT_CSS_Compound * cmp, bool * never_out) {
  const size_t sbase = p->simples.len;
  const size_t start = *pos;
  cstate st = {-1, false};
  bool first = true;
  GTEXT_CSS_Span cs = v[start].span;
  size_t i = *pos;
  for (;;) {
    if (i >= hi) {
      break;
    }
    GTEXT_CSS_Simple s;
    memset(&s, 0, sizeof s);
    s.span = v[i].span;
    bool matched = true;
    if (first) {
      bool star = false;
      size_t j = i;
      qn_result q = qualified_name(p, v, &j, hi, false, &s, &star);
      if (q == QN_BAD) {
        return false;
      }
      if (q == QN_YES) {
        s.kind = star ? GTEXT_CSS_SIMPLE_UNIVERSAL : GTEXT_CSS_SIMPLE_TYPE;
        s.span.end = v[j - 1].span.end;
        i = j;
        if (!node(p, s.span, &s.id) || !vec_push(p, &p->simples, sizeof s, &s)) {
          return false;
        }
        if (restricted_violation(c, &s)) {
          if (!c->restrict_forgive) {
            diag(p, GTEXT_CSS_SELDIAG_NOT_ALLOWED, s.span, REF_AFTER_PART);
            return false;
          }
          VEC_AT(p->simples, GTEXT_CSS_Simple, p->simples.len - 1).never = true;
          *never_out = true;
        }
        first = false;
        continue;
      }
      first = false;
    }
    const V * t = &v[i];
    if (st.saw_pe && !(is_tok(t, GTEXT_CSS_TOKEN_COLON))) {
      matched = false;
    }
    else if (is_tok(t, GTEXT_CSS_TOKEN_HASH) && (t->flags & GTEXT_CSS_FLAG_HASH_ID)) {
      s.kind = GTEXT_CSS_SIMPLE_ID;
      s.name = sdup(p, t->value.data, t->value.length);
      i++;
    }
    else if (is_delim(t, '.') && i + 1 < hi && is_ident(&v[i + 1])) {
      s.kind = GTEXT_CSS_SIMPLE_CLASS;
      s.name = sdup(p, v[i + 1].value.data, v[i + 1].value.length);
      s.span.end = v[i + 1].span.end;
      i += 2;
    }
    else if (is_block(t, GTEXT_CSS_TOKEN_OPEN_SQUARE)) {
      if (!parse_attribute(p, t, &s)) {
        return false;
      }
      i++;
    }
    else if (is_delim(t, '&')) {
      s.kind = GTEXT_CSS_SIMPLE_NESTING;
      i++;
    }
    else if (is_tok(t, GTEXT_CSS_TOKEN_COLON)) {
      size_t j = i;
      if (!parse_pseudo(p, v, &j, hi, c, &st, &s)) {
        return false;
      }
      i = j;
      s.span.end = v[j - 1].span.end;
    }
    else {
      matched = false;
    }
    if (!matched) {
      break;
    }
    if (p->status != GTEXT_CSS_OK) {
      return false;
    }
    if (!node(p, s.span, &s.id) || !vec_push(p, &p->simples, sizeof s, &s)) {
      return false;
    }
    {
      GTEXT_CSS_Simple * back = &VEC_AT(p->simples, GTEXT_CSS_Simple, p->simples.len - 1);
      if (restricted_violation(c, back)) {
        if (!c->restrict_forgive) {
          diag(p, GTEXT_CSS_SELDIAG_NOT_ALLOWED, back->span, REF_AFTER_PART);
          return false;
        }
        back->never = true;
        *never_out = true;
      }
    }
  }
  if (p->simples.len == sbase) {
    diag(p, GTEXT_CSS_SELDIAG_SYNTAX, v[start].span, REF_GRAMMAR);
    return false;
  }
  if (!node(p, cs, &cmp->id)) {
    return false;
  }
  cmp->span = cs;
  cmp->span.end = v[i - 1].span.end;
  cmp->simple_count = p->simples.len - sbase;
  cmp->first_pseudo_element = cmp->simple_count;
  for (size_t k = 0; k < cmp->simple_count; k++) {
    if (VEC_AT(p->simples, GTEXT_CSS_Simple, sbase + k).kind == GTEXT_CSS_SIMPLE_PSEUDO_ELEMENT) {
      cmp->first_pseudo_element = k;
      break;
    }
  }
  cmp->simples = take(p, &p->simples, sizeof(GTEXT_CSS_Simple), sbase);
  *pos = i;
  return p->status == GTEXT_CSS_OK;
}

static bool combinator_at(const V * v, size_t pos, size_t hi, GTEXT_CSS_Combinator * comb, size_t * width) {
  if (pos >= hi) {
    return false;
  }
  if (is_delim(&v[pos], '>')) {
    *comb = GTEXT_CSS_COMBINATOR_CHILD;
    *width = 1;
    return true;
  }
  if (is_delim(&v[pos], '+')) {
    *comb = GTEXT_CSS_COMBINATOR_NEXT_SIBLING;
    *width = 1;
    return true;
  }
  if (is_delim(&v[pos], '~')) {
    *comb = GTEXT_CSS_COMBINATOR_SUBSEQUENT_SIBLING;
    *width = 1;
    return true;
  }
  if (is_delim(&v[pos], '|') && pos + 1 < hi && is_delim(&v[pos + 1], '|')) {
    *comb = GTEXT_CSS_COMBINATOR_COLUMN;
    *width = 2;
    return true;
  }
  return false;
}

static bool parse_complex(P * p, const V * v, size_t lo, size_t hi, bool relative, const sctx * c, GTEXT_CSS_Complex * out) {
  size_t pos = skip_ws(v, lo, hi);
  const size_t cbase = p->compounds.len;
  GTEXT_CSS_Combinator comb = GTEXT_CSS_COMBINATOR_NONE;
  bool never = false;
  if (relative) {
    size_t w;
    GTEXT_CSS_Combinator lead;
    if (combinator_at(v, pos, hi, &lead, &w) && lead != GTEXT_CSS_COMBINATOR_COLUMN) {
      comb = lead;
      pos += w;
      pos = skip_ws(v, pos, hi);
    }
    else {
      comb = GTEXT_CSS_COMBINATOR_DESCENDANT;
    }
  }
  if (pos >= hi) {
    diag(p, GTEXT_CSS_SELDIAG_EMPTY, span_of(v, lo, hi), REF_EMPTY);
    p->compounds.len = cbase;
    return false;
  }
  bool has_pe = false;
  for (;;) {
    GTEXT_CSS_Compound cmp;
    memset(&cmp, 0, sizeof cmp);
    if (has_pe) {
      // a pseudo-element must be the last: here is a combinator and a compound after it
      diag(p, GTEXT_CSS_SELDIAG_NOT_ALLOWED, v[pos].span, REF_PE_LAST);
      p->compounds.len = cbase;
      return false;
    }
    cmp.combinator = comb;
    if (!parse_compound(p, v, &pos, hi, c, &cmp, &never)) {
      p->compounds.len = cbase;
      return false;
    }
    has_pe = cmp.first_pseudo_element < cmp.simple_count;
    if (c->restrict_kind && comb != GTEXT_CSS_COMBINATOR_NONE && !(relative && p->compounds.len == cbase)) {
      if (!c->restrict_forgive) {
        diag(p, GTEXT_CSS_SELDIAG_NOT_ALLOWED, cmp.span, REF_AFTER_PART);
        p->compounds.len = cbase;
        return false;
      }
      never = true;
    }
    if (p->compounds.len - cbase >= p->max_compounds) {
      p->compounds.len = cbase;
      return limit(p, GTEXT_CSS_E_LIMIT_SELECTOR_COMPONENTS, cmp.span);
    }
    if (!vec_push(p, &p->compounds, sizeof cmp, &cmp)) {
      return false;
    }
    // what follows: the end, a combinator, white space then a compound
    size_t after = skip_ws(v, pos, hi);
    if (after >= hi) {
      break;
    }
    size_t w;
    GTEXT_CSS_Combinator next;
    if (combinator_at(v, after, hi, &next, &w)) {
      pos = skip_ws(v, after + w, hi);
      if (pos >= hi) {
        diag(p, GTEXT_CSS_SELDIAG_SYNTAX, v[after].span, REF_GRAMMAR);
        p->compounds.len = cbase;
        return false;
      }
      comb = next;
    }
    else if (after > pos) {
      comb = GTEXT_CSS_COMBINATOR_DESCENDANT;
      pos = after;
    }
    else {
      diag(p, GTEXT_CSS_SELDIAG_SYNTAX, v[pos].span, REF_GRAMMAR);
      p->compounds.len = cbase;
      return false;
    }
  }
  GTEXT_CSS_Complex cx;
  memset(&cx, 0, sizeof cx);
  cx.span = span_of(v, lo, hi);
  {
    size_t a = lo, b = hi;
    trim(v, &a, &b);
    cx.span = span_of(v, a, b);
  }
  if (c->compound_only && p->compounds.len - cbase > 1) {
    diag(p, GTEXT_CSS_SELDIAG_NOT_ALLOWED, cx.span, REF_ARG);
    p->compounds.len = cbase;
    return false;
  }
  if (!node(p, cx.span, &cx.id)) {
    p->compounds.len = cbase;
    return false;
  }
  cx.compound_count = p->compounds.len - cbase;
  cx.relative = relative;
  cx.never = never;
  cx.has_pseudo_element = has_pe;
  cx.compounds = take(p, &p->compounds, sizeof(GTEXT_CSS_Compound), cbase);
  if (p->status != GTEXT_CSS_OK) {
    return false;
  }
  *out = cx;
  return true;
}

/* ---- lists ------------------------------------------------------------------------------------------------ */

static void finish_complex(GTEXT_CSS_Complex * cx);

static bool parse_list(P * p, const V * v, size_t lo, size_t hi, GTEXT_CSS_Selector_Mode mode, const sctx * c, GTEXT_CSS_Span fallback,
    GTEXT_CSS_Selector_List ** out) {
  if (p->status != GTEXT_CSS_OK) {
    return false;
  }
  if (c->depth > p->max_depth) {
    GTEXT_CSS_Span at = lo < hi ? v[lo].span : fallback;
    return limit(p, GTEXT_CSS_E_LIMIT_SELECTOR_DEPTH, at);
  }
  const size_t xbase = p->complexes.len;
  const bool forgiving = mode == GTEXT_CSS_SELECTOR_FORGIVING_LIST;
  const bool relative = mode == GTEXT_CSS_SELECTOR_RELATIVE_LIST;
  bool had_empty = false;
  size_t a = lo;
  size_t pieces = 0;
  for (;;) {
    size_t b = a;
    while (b < hi && !is_comma(&v[b])) {
      b++;
    }
    size_t ta = a, tb = b;
    trim(v, &ta, &tb);
    pieces++;
    if (mode == GTEXT_CSS_SELECTOR_COMPOUND && pieces > 1 && !c->compound_list) {
      diag(p, GTEXT_CSS_SELDIAG_SYNTAX, v[b > a ? b - 1 : a - 1].span, REF_ARG); // the last token of the second selector, or the comma that begins an empty one (v[b] may be past the end)
      p->complexes.len = xbase;
      return false;
    }
    if (ta == tb) {
      if (!forgiving) {
        diag(p, GTEXT_CSS_SELDIAG_EMPTY, lo < hi ? span_of(v, lo, hi) : fallback, REF_EMPTY);
        p->complexes.len = xbase;
        return false;
      }
      had_empty = true;
    }
    else {
      const size_t dsave = p->ndiag;
      const size_t tsave = p->total_diag;
      const size_t ssave = p->simples.len, csave = p->compounds.len, strsave = p->strings.len, fsave = p->flags.len, lsave = p->levels.len, asave = p->attrs.len;
      GTEXT_CSS_Complex cx;
      memset(&cx, 0, sizeof cx);
      const bool ok = parse_complex(p, v, ta, tb, relative, c, &cx);
      if (p->status != GTEXT_CSS_OK) {
        p->complexes.len = xbase;
        return false;
      }
      if (!ok) {
        // an entry that is not a selector leaves nothing of what it had read: a forgiving list goes on with the vectors as they were (a simple left behind would be the next compound's)
        p->simples.len = ssave;
        p->compounds.len = csave;
        p->strings.len = strsave;
        p->flags.len = fsave;
        p->levels.len = lsave;
        p->attrs.len = asave;
      }
      if (ok) {
        if (p->complexes.len - xbase >= p->max_list) {
          p->complexes.len = xbase;
          return limit(p, GTEXT_CSS_E_LIMIT_SELECTOR_LIST, v[ta].span);
        }
        finish_complex(&cx);
        if (!vec_push(p, &p->complexes, sizeof cx, &cx)) {
          p->complexes.len = xbase;
          return false;
        }
      }
      else if (forgiving) {
        // the entry is not a selector: it stays as written, never matched, with the diagnostic of why replaced by the one that it was
        p->ndiag = dsave;
        p->total_diag = tsave;
        diag(p, GTEXT_CSS_SELDIAG_DROPPED, span_of(v, ta, tb), REF_FORGIVING);
        GTEXT_CSS_Complex rawc;
        memset(&rawc, 0, sizeof rawc);
        rawc.span = span_of(v, ta, tb);
        rawc.never = true;
        if (!node(p, rawc.span, &rawc.id)) {
          p->complexes.len = xbase;
          return false;
        }
        char * rbuf = NULL;
        size_t rlen = 0;
        const GTEXT_CSS_Serialize_Options so = {&p->alloc, GTEXT_CSS_NUMBERS_SOURCE, 0};
        if (gtext_css_serialize_values_buffer(v + ta, tb - ta, &so, &rbuf, &rlen) != GTEXT_CSS_OK) {
          p->status = GTEXT_CSS_E_OOM;
          p->complexes.len = xbase;
          return false;
        }
        rawc.raw = sdup(p, rbuf, rlen);
        gtext_css_serialize_buffer_free(&so, rbuf);
        if (p->status != GTEXT_CSS_OK || !vec_push(p, &p->complexes, sizeof rawc, &rawc)) {
          p->complexes.len = xbase;
          return false;
        }
      }
      else {
        p->complexes.len = xbase;
        return false;
      }
    }
    if (b >= hi) {
      break;
    }
    a = b + 1;
  }
  if (!forgiving && p->complexes.len == xbase) {
    diag(p, GTEXT_CSS_SELDIAG_EMPTY, fallback, REF_EMPTY);
    return false;
  }
  GTEXT_CSS_Selector_List * l = css_arena_alloc(p->arena, sizeof *l);
  if (!l) {
    p->status = GTEXT_CSS_E_OOM;
    return false;
  }
  memset(l, 0, sizeof *l);
  GTEXT_CSS_Span ls = lo < hi ? span_of(v, lo, hi) : fallback;
  if (!node(p, ls, &l->id)) {
    p->complexes.len = xbase;
    return false;
  }
  l->span = ls;
  l->mode = mode;
  l->had_empty_entry = had_empty;
  l->count = p->complexes.len - xbase;
  l->complex = take(p, &p->complexes, sizeof(GTEXT_CSS_Complex), xbase);
  if (p->status != GTEXT_CSS_OK) {
    return false;
  }
  l->uid = p->uid;
  // the summaries of the list, from its complex selectors (each one's from its parts)
  for (size_t k = 0; k < l->count; k++) {
    const GTEXT_CSS_Complex * cx = &l->complex[k];
    if (cx->raw.data) {
      continue;
    }
    if (gtext_css_specificity_compare(cx->specificity, l->max_specificity) > 0) {
      l->max_specificity = cx->specificity;
    }
    l->has_nesting |= cx->has_nesting;
    l->dependencies |= cx->dependencies;
  }
  *out = l;
  return true;
}

/* ---- the summaries of a complex selector -------------------------------------------------------------------- */

static void merge_list(GTEXT_CSS_Complex * cx, const GTEXT_CSS_Selector_List * l) {
  if (!l) {
    return;
  }
  cx->dependencies |= l->dependencies;
  cx->has_nesting |= l->has_nesting;
  for (size_t k = 0; k < l->count; k++) {
    cx->anchor_dependent |= l->complex[k].anchor_dependent;
  }
}

static void finish_complex(GTEXT_CSS_Complex * cx) {
  GTEXT_CSS_Specificity sp = gtext_css_specificity_make(0, 0, 0);
  uint32_t dep = 0;
  for (size_t i = 0; i < cx->compound_count; i++) {
    const GTEXT_CSS_Compound * cmp = &cx->compounds[i];
    switch (cmp->combinator) {
    case GTEXT_CSS_COMBINATOR_DESCENDANT:
    case GTEXT_CSS_COMBINATOR_CHILD: dep |= GTEXT_CSS_DEP_TREE; break;
    case GTEXT_CSS_COMBINATOR_NEXT_SIBLING:
    case GTEXT_CSS_COMBINATOR_SUBSEQUENT_SIBLING: dep |= GTEXT_CSS_DEP_SIBLING; break;
    case GTEXT_CSS_COMBINATOR_COLUMN: dep |= GTEXT_CSS_DEP_COLUMN; break;
    default: break;
    }
    for (size_t k = 0; k < cmp->simple_count; k++) {
      const GTEXT_CSS_Simple * s = &cmp->simples[k];
      sp = gtext_css_specificity_add(sp, css_selector_simple_specificity(s, gtext_css_specificity_make(0, 1, 0)));
      switch (s->kind) {
      case GTEXT_CSS_SIMPLE_TYPE: dep |= GTEXT_CSS_DEP_NAME; break;
      case GTEXT_CSS_SIMPLE_UNIVERSAL: break;
      case GTEXT_CSS_SIMPLE_ID: dep |= GTEXT_CSS_DEP_ID; break;
      case GTEXT_CSS_SIMPLE_CLASS: dep |= GTEXT_CSS_DEP_CLASS; break;
      case GTEXT_CSS_SIMPLE_ATTRIBUTE: dep |= GTEXT_CSS_DEP_ATTR; break;
      case GTEXT_CSS_SIMPLE_NESTING:
        dep |= GTEXT_CSS_DEP_NESTING;
        cx->has_nesting = true;
        cx->anchor_dependent = true;
        break;
      case GTEXT_CSS_SIMPLE_PSEUDO_CLASS:
      case GTEXT_CSS_SIMPLE_PSEUDO_ELEMENT: {
        const sel_row * r = css_sel_row_of(s);
        dep |= r->dep;
        if (s->kind == GTEXT_CSS_SIMPLE_PSEUDO_ELEMENT) {
          dep |= GTEXT_CSS_DEP_PSEUDO;
        }
        if (r->spec == SELSPEC_CLASS && s->pseudo == GTEXT_CSS_SEL_PC_SCOPE) {
          cx->anchor_dependent = true;
        }
        break;
      }
      }
      if (s->never) {
        cx->never = true;
      }
      merge_list(cx, s->list);
    }
  }
  cx->specificity = sp;
  cx->dependencies = dep | cx->dependencies;
  const GTEXT_CSS_Compound * last = &cx->compounds[cx->compound_count - 1];
  cx->has_pseudo_element = last->first_pseudo_element < last->simple_count;
}

/* ---- the root list's summaries (requires, dependencies) --------------------------------------------------- */

static void walk_list(P * p, const GTEXT_CSS_Selector_List * l, GTEXT_CSS_Selector_List * root) {
  for (size_t i = 0; i < l->count && p->status == GTEXT_CSS_OK; i++) {
    const GTEXT_CSS_Complex * cx = &l->complex[i];
    for (size_t j = 0; j < cx->compound_count; j++) {
      const GTEXT_CSS_Compound * cmp = &cx->compounds[j];
      switch (cmp->combinator) {
      case GTEXT_CSS_COMBINATOR_DESCENDANT:
      case GTEXT_CSS_COMBINATOR_CHILD: root->callbacks |= 1ull << GTEXT_CSS_ELEMENT_CB_PARENT; break;
      case GTEXT_CSS_COMBINATOR_NEXT_SIBLING:
      case GTEXT_CSS_COMBINATOR_SUBSEQUENT_SIBLING: root->callbacks |= 1ull << GTEXT_CSS_ELEMENT_CB_PREVIOUS_SIBLING; break;
      case GTEXT_CSS_COMBINATOR_COLUMN: root->unmatchable = true; break;
      default: break;
      }
      for (size_t k = 0; k < cmp->simple_count; k++) {
        const GTEXT_CSS_Simple * s = &cmp->simples[k];
        switch (s->kind) {
        case GTEXT_CSS_SIMPLE_TYPE:
          root->callbacks |= 1ull << GTEXT_CSS_ELEMENT_CB_LOCAL_NAME;
          if (s->ns_kind == GTEXT_CSS_NS_NAMED || s->ns_kind == GTEXT_CSS_NS_NONE || s->ns_uri.data) {
            root->callbacks |= 1ull << GTEXT_CSS_ELEMENT_CB_NAMESPACE_URI;
          }
          break;
        case GTEXT_CSS_SIMPLE_UNIVERSAL:
          if (s->ns_kind == GTEXT_CSS_NS_NAMED || s->ns_kind == GTEXT_CSS_NS_NONE || s->ns_uri.data) {
            root->callbacks |= 1ull << GTEXT_CSS_ELEMENT_CB_NAMESPACE_URI;
          }
          break;
        case GTEXT_CSS_SIMPLE_ID: root->callbacks |= 1ull << GTEXT_CSS_ELEMENT_CB_ID; break;
        case GTEXT_CSS_SIMPLE_CLASS:
          root->callbacks |= (1ull << GTEXT_CSS_ELEMENT_CB_CLASS_COUNT) | (1ull << GTEXT_CSS_ELEMENT_CB_CLASS_AT);
          break;
        case GTEXT_CSS_SIMPLE_ATTRIBUTE: {
          root->callbacks |= 1ull << GTEXT_CSS_ELEMENT_CB_ATTRIBUTE;
          bool have = false;
          for (size_t a = 0; a < p->attrs.len; a++) {
            const GTEXT_CSS_String * e = &VEC_AT(p->attrs, GTEXT_CSS_String, a);
            if (e->length == s->name.length && memcmp(e->data, s->name.data, e->length) == 0) {
              have = true;
              break;
            }
          }
          if (!have) {
            vec_push(p, &p->attrs, sizeof s->name, &s->name);
          }
          break;
        }
        case GTEXT_CSS_SIMPLE_NESTING: break;
        case GTEXT_CSS_SIMPLE_PSEUDO_CLASS:
        case GTEXT_CSS_SIMPLE_PSEUDO_ELEMENT: {
          const sel_row * r = css_sel_row_of(s);
          root->callbacks |= r->callbacks;
          root->state |= r->state;
          if (r->status == SELS_PARSE_ONLY) {
            root->unmatchable = true;
          }
          break;
        }
        }
        if (s->list) {
          walk_list(p, s->list, root);
        }
      }
    }
  }
}

/* ---- entry points ------------------------------------------------------------------------------------------ */

GTEXT_CSS_Selector_Options gtext_css_selector_options_default(void) {
  GTEXT_CSS_Selector_Options o;
  memset(&o, 0, sizeof o);
  o.mode = GTEXT_CSS_SELECTOR_COMPLEX_LIST;
  o.max_components = DEFAULT_MAX_COMPONENTS;
  o.max_list = DEFAULT_MAX_LIST;
  o.max_compounds = DEFAULT_MAX_COMPOUNDS;
  o.max_depth = DEFAULT_MAX_DEPTH;
  o.max_diagnostics = DEFAULT_MAX_DIAGNOSTICS;
  o.parse = gtext_css_parse_options_default();
  return o;
}

struct GTEXT_CSS_Selector_Result_Internal {
  GTEXT_Allocator alloc;
  size_t count;
  GTEXT_CSS_Selector_Diagnostic d[];
};

void gtext_css_selector_result_free(GTEXT_CSS_Selector_Result * result) {
  if (!result) {
    return;
  }
  if (result->internal) {
    GTEXT_Allocator a = result->internal->alloc;
    gtext_allocator_free(&a, result->internal);
  }
  memset(result, 0, sizeof *result);
}

size_t gtext_css_selector_result_diagnostic_count(const GTEXT_CSS_Selector_Result * result) {
  return result && result->internal ? result->internal->count : 0;
}

bool gtext_css_selector_result_diagnostic(const GTEXT_CSS_Selector_Result * result, size_t index, GTEXT_CSS_Selector_Diagnostic * out) {
  if (!result || !result->internal || !out || index >= result->internal->count) {
    return false;
  }
  *out = result->internal->d[index];
  return true;
}

GTEXT_CSS_Selector_List * gtext_css_selector_list_retain(const GTEXT_CSS_Selector_List * list) {
  if (list && list->internal) {
    (void)gcu_atomic_int_fetch_add(&list->internal->refs, 1);
  }
  return (GTEXT_CSS_Selector_List *)list;
}

static void internal_release(struct GTEXT_CSS_Selector_List_Internal * in) {
  GTEXT_Allocator a = in->alloc;
  css_arena_free(&in->arena);
  gtext_allocator_free(&a, in);
}

void gtext_css_selector_list_free(const GTEXT_CSS_Selector_List * list) {
  if (!list || !list->internal) {
    return;
  }
  struct GTEXT_CSS_Selector_List_Internal * in = list->internal;
  if (gcu_atomic_int_fetch_sub(&in->refs, 1) != 1) {
    return;
  }
  internal_release(in);
}

static GTEXT_CSS_Status parse_impl(const V * values, size_t count, const GTEXT_CSS_Selector_Options * options, GTEXT_CSS_Selector_List ** out,
    GTEXT_CSS_Selector_Result * result) {
  if (result) {
    memset(result, 0, sizeof *result);
    result->message = gtext_css_status_string(GTEXT_CSS_OK);
  }
  if (!out || (count && !values)) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Selector_Options o = options ? *options : gtext_css_selector_options_default();
  if (!o.max_components) {
    o.max_components = DEFAULT_MAX_COMPONENTS;
  }
  if (!o.max_list) {
    o.max_list = DEFAULT_MAX_LIST;
  }
  if (!o.max_compounds) {
    o.max_compounds = DEFAULT_MAX_COMPOUNDS;
  }
  if (!o.max_depth) {
    o.max_depth = DEFAULT_MAX_DEPTH;
  }
  if (o.max_depth > MAX_DEPTH_LIMIT) {
    o.max_depth = MAX_DEPTH_LIMIT;
  }
  if (!o.max_diagnostics) {
    o.max_diagnostics = DEFAULT_MAX_DIAGNOSTICS;
  }
  if (o.max_diagnostics > MAX_DIAGNOSTICS_LIMIT) {
    o.max_diagnostics = MAX_DIAGNOSTICS_LIMIT;
  }
  if ((unsigned)o.mode > (unsigned)GTEXT_CSS_SELECTOR_REAL_LIST) {
    return GTEXT_CSS_E_INVALID;
  }
  const GTEXT_Allocator * ra = o.allocator ? o.allocator : gtext_allocator_default();
  P p;
  memset(&p, 0, sizeof p);
  p.o = &o;
  p.alloc = *ra;
  p.max_components = o.max_components;
  p.max_list = o.max_list;
  p.max_compounds = o.max_compounds;
  p.max_depth = o.max_depth;
  p.keep_diag = o.max_diagnostics;
  p.ns = o.namespaces;
  p.uid = gcu_atomic_size_fetch_add(&uid_counter, 1);
  struct GTEXT_CSS_Selector_List_Internal * in = gtext_allocator_calloc(&p.alloc, 1, sizeof *in);
  if (!in) {
    if (result) {
      result->status = GTEXT_CSS_E_OOM;
      result->message = gtext_css_status_string(GTEXT_CSS_E_OOM);
    }
    return GTEXT_CSS_E_OOM;
  }
  gcu_atomic_int_init(&in->refs, 1);
  in->alloc = p.alloc;
  css_arena_init(&in->arena, &p.alloc);
  p.arena = &in->arena;
  p.in = in;
  sctx c;
  memset(&c, 0, sizeof c);
  c.no_pe = o.mode == GTEXT_CSS_SELECTOR_REAL_LIST;
  c.compound_only = o.mode == GTEXT_CSS_SELECTOR_COMPOUND;
  GTEXT_CSS_Span fallback;
  memset(&fallback, 0, sizeof fallback);
  fallback.line = 1;
  fallback.column = 1;
  GTEXT_CSS_Selector_List * list = NULL;
  if (!parse_list(&p, values, 0, count, o.mode, &c, fallback, &list)) {
    list = NULL;
  }
  if (list && p.status == GTEXT_CSS_OK) {
    list->internal = in;
    walk_list(&p, list, list);
    list->node_count = p.nodes;
    if (p.status == GTEXT_CSS_OK) {
      in->dep.kinds = list->dependencies;
      in->dep.state = list->state;
      in->dep.attribute_count = p.attrs.len;
      if (p.attrs.len) {
        GTEXT_CSS_String * a = css_arena_alloc(&in->arena, p.attrs.len * sizeof *a);
        if (!a) {
          p.status = GTEXT_CSS_E_OOM;
        }
        else {
          memcpy(a, p.attrs.data, p.attrs.len * sizeof *a);
          in->dep.attributes = a;
        }
      }
      in->req.callbacks = list->callbacks;
      in->req.state = list->state;
      in->req.unmatchable = list->unmatchable;
    }
  }
  vec_free(&p, &p.simples);
  vec_free(&p, &p.compounds);
  vec_free(&p, &p.complexes);
  vec_free(&p, &p.strings);
  vec_free(&p, &p.flags);
  vec_free(&p, &p.levels);
  vec_free(&p, &p.attrs);
  GTEXT_CSS_Status st = p.status;
  if (st == GTEXT_CSS_OK && result) {
    result->valid = list != NULL;
  }
  if (result) {
    result->status = st;
    result->message = gtext_css_status_string(st);
    result->failure = p.fail;
    result->diagnostic_total = p.total_diag;
    if (p.ndiag && st == GTEXT_CSS_OK) {
      struct GTEXT_CSS_Selector_Result_Internal * ri =
          gtext_allocator_malloc(ra, sizeof *ri + p.ndiag * sizeof(GTEXT_CSS_Selector_Diagnostic));
      if (!ri) {
        st = GTEXT_CSS_E_OOM;
        result->status = st;
        result->message = gtext_css_status_string(st);
        result->valid = false;
      }
      else {
        ri->alloc = *ra;
        ri->count = p.ndiag;
        memcpy(ri->d, p.diag, p.ndiag * sizeof(GTEXT_CSS_Selector_Diagnostic));
        result->internal = ri;
      }
    }
  }
  gtext_allocator_free(&p.alloc, p.diag);
  if (st != GTEXT_CSS_OK || !list) {
    internal_release(in);
    if (st == GTEXT_CSS_OK) {
      *out = NULL;
    }
    return st;
  }
  *out = list;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_selector_parse_values(const GTEXT_CSS_Component_Value * values, size_t count, const GTEXT_CSS_Selector_Options * options,
    GTEXT_CSS_Selector_List ** out, GTEXT_CSS_Selector_Result * result) {
  return parse_impl(values, count, options, out, result);
}

GTEXT_CSS_Status gtext_css_selector_parse(const char * text, size_t length, const GTEXT_CSS_Selector_Options * options, GTEXT_CSS_Selector_List ** out,
    GTEXT_CSS_Selector_Result * result) {
  if (result) {
    memset(result, 0, sizeof *result);
    result->message = gtext_css_status_string(GTEXT_CSS_OK);
  }
  if (!out || (length && !text)) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Selector_Options o = options ? *options : gtext_css_selector_options_default();
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
  st = parse_impl(tree->values, tree->value_count, &o, out, result);
  gtext_css_tree_free(tree);
  return st;
}
