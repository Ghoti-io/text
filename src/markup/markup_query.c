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
 * The Markup queries (markup_query.h): the adapter and a match context made for the call, the walk of css_match.h for the descendants, the selector parsed as the DOM parses
 * one.
 */

#include <string.h>

#include <ghoti.io/text/css/css_match.h>
#include <ghoti.io/text/css/css_markup.h>
#include <ghoti.io/text/markup/markup_query.h>

#define MAX_SELECTOR_BYTES ((size_t)1 << 20)

GTEXT_Markup_Query_Options gtext_markup_query_options_default(void) {
  GTEXT_Markup_Query_Options o;
  memset(&o, 0, sizeof o);
  o.selector = gtext_css_selector_options_default();
  return o;
}

void gtext_markup_query_result_free(GTEXT_Markup_Query_Result * result) {
  if (!result) {
    return;
  }
  gtext_css_selector_result_free(&result->selector);
  memset(result, 0, sizeof *result);
}

typedef struct {
  GTEXT_CSS_Markup_Adapter * adapter;
  GTEXT_CSS_Selector_Match_Context * ctx;
  GTEXT_CSS_Selector_List * owned;
  GTEXT_Markup_Query_Result * result;
} Q;

static GTEXT_Markup_Status map(GTEXT_CSS_Status st) {
  switch (st) {
  case GTEXT_CSS_OK: return GTEXT_MARKUP_OK;
  case GTEXT_CSS_E_OOM: return GTEXT_MARKUP_E_OOM;
  case GTEXT_CSS_E_INVALID:
  case GTEXT_CSS_E_UNSUPPORTED:
  case GTEXT_CSS_E_INTERNAL: return GTEXT_MARKUP_E_INVALID;
  default: return GTEXT_MARKUP_E_LIMIT;
  }
}

static GTEXT_Markup_Status done(GTEXT_Markup_Query_Result * r, GTEXT_Markup_Status st, GTEXT_CSS_Status css, const char * unsupported) {
  if (r) {
    r->status = st;
    r->message = gtext_markup_status_string(st);
    r->css_status = css;
    r->unsupported = unsupported;
  }
  return st;
}

static bool is_scope_kind(const GTEXT_Markup_Node * n) {
  const GTEXT_Markup_Kind k = gtext_markup_node_kind(n);
  return k == GTEXT_MARKUP_KIND_DOCUMENT || k == GTEXT_MARKUP_KIND_FRAGMENT || k == GTEXT_MARKUP_KIND_ELEMENT;
}

static void q_free(Q * q) {
  gtext_css_selector_match_context_free(q->ctx);
  gtext_css_markup_adapter_free(q->adapter);
  gtext_css_selector_list_free(q->owned);
}

/// Make the adapter and the context, and parse the selector if it is text. `scope_el` is the element `:scope` is, or NULL for the root.
static GTEXT_Markup_Status q_open(Q * q, const GTEXT_Markup_Node * node, const char * text, size_t length, const GTEXT_CSS_Selector_List * list,
    const GTEXT_Markup_Query_Options * options, const GTEXT_Markup_Node * scope_el, GTEXT_Markup_Query_Result * result) {
  memset(q, 0, sizeof *q);
  q->result = result;
  if (result) {
    memset(result, 0, sizeof *result);
    result->message = gtext_markup_status_string(GTEXT_MARKUP_OK);
  }
  GTEXT_Markup_Query_Options o = options ? *options : gtext_markup_query_options_default();
  if (!node || !is_scope_kind(node) || (!list && (!text && length)) || (!list && !text && !length && 0)) {
    return done(result, GTEXT_MARKUP_E_INVALID, GTEXT_CSS_E_INVALID, NULL);
  }
  if (!list) {
    if (length > MAX_SELECTOR_BYTES) {
      return done(result, GTEXT_MARKUP_E_LIMIT, GTEXT_CSS_E_LIMIT_INPUT, NULL);
    }
    GTEXT_CSS_Selector_Options so = o.selector;
    if (!so.max_components && !so.max_list && !so.max_depth && !so.max_diagnostics) {
      so = gtext_css_selector_options_default();
    }
    so.mode = GTEXT_CSS_SELECTOR_COMPLEX_LIST;
    so.namespaces = NULL;
    if (!so.allocator) {
      so.allocator = o.allocator;
    }
    GTEXT_CSS_Selector_Result sr;
    memset(&sr, 0, sizeof sr);
    GTEXT_CSS_Status st = gtext_css_selector_parse(text ? text : "", length, &so, &q->owned, &sr);
    if (st != GTEXT_CSS_OK) {
      if (result) {
        result->selector = sr;
      }
      else {
        gtext_css_selector_result_free(&sr);
      }
      return done(result, map(st), st, NULL);
    }
    if (!q->owned) {
      if (result) {
        result->selector = sr;
      }
      else {
        gtext_css_selector_result_free(&sr);
      }
      return done(result, GTEXT_MARKUP_E_SELECTOR, GTEXT_CSS_OK, NULL);
    }
    gtext_css_selector_result_free(&sr);
    list = q->owned;
  }
  q->owned = q->owned; /* the list the call uses is `list` */
  GTEXT_CSS_Markup_Options mo;
  memset(&mo, 0, sizeof mo);
  mo.allocator = o.allocator;
  mo.kind = o.kind;
  mo.state = o.state;
  mo.user = o.user;
  const GTEXT_Markup_Document * doc = gtext_markup_node_document(node);
  GTEXT_CSS_Status st = gtext_css_markup_adapter_new(doc, &mo, &q->adapter);
  if (st != GTEXT_CSS_OK) {
    q_free(q);
    return done(result, map(st), st, NULL);
  }
  GTEXT_CSS_Selector_Match_Options mopt = gtext_css_markup_match_options(q->adapter);
  mopt.allocator = o.allocator;
  mopt.scope = scope_el;
  if (o.max_memo_entries) {
    mopt.max_memo_entries = o.max_memo_entries;
  }
  if (o.max_work) {
    mopt.max_work = o.max_work;
  }
  st = gtext_css_selector_match_context_new(gtext_css_markup_interface(q->adapter), &mopt, &q->ctx);
  if (st != GTEXT_CSS_OK) {
    q_free(q);
    return done(result, map(st), st, NULL);
  }
  return GTEXT_MARKUP_OK;
}

static GTEXT_Markup_Status q_finish(Q * q, GTEXT_CSS_Status st, const GTEXT_CSS_Selector_Match_Result * mr) {
  const char * unsupported = NULL;
  if (st == GTEXT_CSS_E_UNSUPPORTED && mr) {
    unsupported = mr->callback ? mr->callback : mr->selector;
  }
  const GTEXT_Markup_Status ms = done(q->result, map(st), st, unsupported);
  q_free(q);
  return ms;
}

/// Visit the descendant elements of the node in document order; the node itself is skipped.
typedef struct {
  const GTEXT_CSS_Selector_List * list;
  GTEXT_CSS_Selector_Match_Context * ctx;
  const GTEXT_Markup_Node * skip;
  GTEXT_Markup_Query_Fn fn;
  void * user;
  GTEXT_CSS_Status status;
  GTEXT_CSS_Selector_Match_Result last;
} Each;

static bool visit(void * user, GTEXT_CSS_Element_Handle h) {
  Each * e = user;
  if (h == e->skip) {
    return true;
  }
  GTEXT_CSS_Selector_Match_Result r;
  const GTEXT_CSS_Status st = gtext_css_selector_match(e->list, e->ctx, h, NULL, &r);
  if (st != GTEXT_CSS_OK) {
    e->status = st;
    e->last = r;
    return false;
  }
  if (r.matched) {
    return e->fn(e->user, h);
  }
  return true;
}

static GTEXT_Markup_Status each_impl(const GTEXT_Markup_Node * scope, const char * text, size_t length, const GTEXT_CSS_Selector_List * list,
    const GTEXT_Markup_Query_Options * options, GTEXT_Markup_Query_Fn fn, void * user, GTEXT_Markup_Query_Result * result) {
  Q q;
  const bool element = scope && gtext_markup_node_kind(scope) == GTEXT_MARKUP_KIND_ELEMENT;
  GTEXT_Markup_Status ms = q_open(&q, scope, text, length, list, options, element ? scope : NULL, result);
  if (ms != GTEXT_MARKUP_OK) {
    return ms;
  }
  Each e;
  memset(&e, 0, sizeof e);
  e.list = list ? list : q.owned;
  e.ctx = q.ctx;
  e.skip = element ? scope : NULL;
  e.fn = fn;
  e.user = user;
  GTEXT_CSS_Selector_Match_Result mr;
  memset(&mr, 0, sizeof mr);
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  if (element) {
    st = gtext_css_selector_match_walk(q.ctx, scope, visit, &e, &mr);
  }
  else {
    for (const GTEXT_Markup_Node * c = gtext_markup_node_first_child(scope); c && st == GTEXT_CSS_OK && e.status == GTEXT_CSS_OK; c = gtext_markup_node_next_sibling(c)) {
      if (gtext_markup_node_kind(c) == GTEXT_MARKUP_KIND_ELEMENT) {
        st = gtext_css_selector_match_walk(q.ctx, c, visit, &e, &mr);
      }
    }
  }
  if (st == GTEXT_CSS_OK && e.status != GTEXT_CSS_OK) {
    st = e.status;
    mr = e.last;
  }
  return q_finish(&q, st, &mr);
}

/* ---- the first match ----------------------------------------------------------------------------------------------- */

typedef struct {
  const GTEXT_Markup_Node * found;
} First;

static bool first_fn(void * user, const GTEXT_Markup_Node * el) {
  ((First *)user)->found = el;
  return false;
}

static GTEXT_Markup_Status first_impl(const GTEXT_Markup_Node * scope, const char * text, size_t length, const GTEXT_CSS_Selector_List * list,
    const GTEXT_Markup_Query_Options * options, GTEXT_Markup_Node ** out, GTEXT_Markup_Query_Result * result) {
  if (!out) {
    return done(result, GTEXT_MARKUP_E_INVALID, GTEXT_CSS_E_INVALID, NULL);
  }
  First f = {NULL};
  const GTEXT_Markup_Status ms = each_impl(scope, text, length, list, options, first_fn, &f, result);
  if (ms == GTEXT_MARKUP_OK) {
    *out = (GTEXT_Markup_Node *)f.found;
  }
  return ms;
}

GTEXT_Markup_Status gtext_markup_query_selector(const GTEXT_Markup_Node * scope, const char * selector, size_t length, const GTEXT_Markup_Query_Options * options,
    GTEXT_Markup_Node ** out, GTEXT_Markup_Query_Result * result) {
  return first_impl(scope, selector, length, NULL, options, out, result);
}

GTEXT_Markup_Status gtext_markup_query_selector_compiled(const GTEXT_Markup_Node * scope, const GTEXT_CSS_Selector_List * list,
    const GTEXT_Markup_Query_Options * options, GTEXT_Markup_Node ** out, GTEXT_Markup_Query_Result * result) {
  if (!list) {
    return done(result, GTEXT_MARKUP_E_INVALID, GTEXT_CSS_E_INVALID, NULL);
  }
  return first_impl(scope, NULL, 0, list, options, out, result);
}

GTEXT_Markup_Status gtext_markup_query_selector_each(const GTEXT_Markup_Node * scope, const char * selector, size_t length,
    const GTEXT_Markup_Query_Options * options, GTEXT_Markup_Query_Fn fn, void * user, GTEXT_Markup_Query_Result * result) {
  if (!fn) {
    return done(result, GTEXT_MARKUP_E_INVALID, GTEXT_CSS_E_INVALID, NULL);
  }
  return each_impl(scope, selector, length, NULL, options, fn, user, result);
}

GTEXT_Markup_Status gtext_markup_query_selector_each_compiled(const GTEXT_Markup_Node * scope, const GTEXT_CSS_Selector_List * list,
    const GTEXT_Markup_Query_Options * options, GTEXT_Markup_Query_Fn fn, void * user, GTEXT_Markup_Query_Result * result) {
  if (!fn || !list) {
    return done(result, GTEXT_MARKUP_E_INVALID, GTEXT_CSS_E_INVALID, NULL);
  }
  return each_impl(scope, NULL, 0, list, options, fn, user, result);
}

/* ---- all matches ---------------------------------------------------------------------------------------------------- */

typedef struct {
  GTEXT_Allocator alloc;
  GTEXT_Markup_Node ** items;
  size_t n;
  size_t cap;
  bool oom;
} Array;

static bool all_fn(void * user, const GTEXT_Markup_Node * el) {
  Array * a = user;
  if (a->n == a->cap) {
    const size_t nc = a->cap ? a->cap * 2 : 16;
    GTEXT_Markup_Node ** ni = gtext_allocator_realloc(&a->alloc, a->items, nc * sizeof *ni);
    if (!ni) {
      a->oom = true;
      return false;
    }
    a->items = ni;
    a->cap = nc;
  }
  a->items[a->n++] = (GTEXT_Markup_Node *)el;
  return true;
}

static GTEXT_Markup_Status all_impl(const GTEXT_Markup_Node * scope, const char * text, size_t length, const GTEXT_CSS_Selector_List * list,
    const GTEXT_Markup_Query_Options * options, GTEXT_Markup_Node *** out, size_t * count, GTEXT_Markup_Query_Result * result) {
  if (!out || !count) {
    return done(result, GTEXT_MARKUP_E_INVALID, GTEXT_CSS_E_INVALID, NULL);
  }
  Array a;
  memset(&a, 0, sizeof a);
  a.alloc = *(options && options->allocator ? options->allocator : gtext_allocator_default());
  const GTEXT_Markup_Status ms = each_impl(scope, text, length, list, options, all_fn, &a, result);
  if (ms != GTEXT_MARKUP_OK || a.oom) {
    gtext_allocator_free(&a.alloc, a.items);
    return ms != GTEXT_MARKUP_OK ? ms : done(result, GTEXT_MARKUP_E_OOM, GTEXT_CSS_E_OOM, NULL);
  }
  *out = a.items;
  *count = a.n;
  return GTEXT_MARKUP_OK;
}

GTEXT_Markup_Status gtext_markup_query_selector_all(const GTEXT_Markup_Node * scope, const char * selector, size_t length,
    const GTEXT_Markup_Query_Options * options, GTEXT_Markup_Node *** out, size_t * count, GTEXT_Markup_Query_Result * result) {
  return all_impl(scope, selector, length, NULL, options, out, count, result);
}

GTEXT_Markup_Status gtext_markup_query_selector_all_compiled(const GTEXT_Markup_Node * scope, const GTEXT_CSS_Selector_List * list,
    const GTEXT_Markup_Query_Options * options, GTEXT_Markup_Node *** out, size_t * count, GTEXT_Markup_Query_Result * result) {
  if (!list) {
    return done(result, GTEXT_MARKUP_E_INVALID, GTEXT_CSS_E_INVALID, NULL);
  }
  return all_impl(scope, NULL, 0, list, options, out, count, result);
}

void gtext_markup_query_free(const GTEXT_Markup_Query_Options * options, GTEXT_Markup_Node ** nodes) {
  gtext_allocator_free(options ? options->allocator : NULL, nodes);
}

/* ---- matches and closest ------------------------------------------------------------------------------------------------ */

static GTEXT_Markup_Status closest_impl(const GTEXT_Markup_Node * element, const char * text, size_t length, const GTEXT_CSS_Selector_List * list,
    const GTEXT_Markup_Query_Options * options, bool inclusive_only_self, bool * matched, GTEXT_Markup_Node ** out, GTEXT_Markup_Query_Result * result) {
  Q q;
  if (!element || gtext_markup_node_kind(element) != GTEXT_MARKUP_KIND_ELEMENT) {
    return done(result, GTEXT_MARKUP_E_INVALID, GTEXT_CSS_E_INVALID, NULL);
  }
  GTEXT_Markup_Status ms = q_open(&q, element, text, length, list, options, element, result);
  if (ms != GTEXT_MARKUP_OK) {
    return ms;
  }
  const GTEXT_CSS_Selector_List * l = list ? list : q.owned;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  GTEXT_CSS_Selector_Match_Result mr;
  memset(&mr, 0, sizeof mr);
  const GTEXT_Markup_Node * found = NULL;
  for (const GTEXT_Markup_Node * p = element; p && gtext_markup_node_kind(p) == GTEXT_MARKUP_KIND_ELEMENT; p = gtext_markup_node_parent(p)) {
    st = gtext_css_selector_match(l, q.ctx, p, NULL, &mr);
    if (st != GTEXT_CSS_OK || mr.matched) {
      if (st == GTEXT_CSS_OK) {
        found = p;
      }
      break;
    }
    if (inclusive_only_self) {
      break;
    }
  }
  if (st == GTEXT_CSS_OK) {
    if (matched) {
      *matched = found != NULL;
    }
    if (out) {
      *out = (GTEXT_Markup_Node *)found;
    }
  }
  return q_finish(&q, st, &mr);
}

GTEXT_Markup_Status gtext_markup_matches(const GTEXT_Markup_Node * element, const char * selector, size_t length, const GTEXT_Markup_Query_Options * options,
    bool * out, GTEXT_Markup_Query_Result * result) {
  if (!out) {
    return done(result, GTEXT_MARKUP_E_INVALID, GTEXT_CSS_E_INVALID, NULL);
  }
  return closest_impl(element, selector, length, NULL, options, true, out, NULL, result);
}

GTEXT_Markup_Status gtext_markup_matches_compiled(const GTEXT_Markup_Node * element, const GTEXT_CSS_Selector_List * list,
    const GTEXT_Markup_Query_Options * options, bool * out, GTEXT_Markup_Query_Result * result) {
  if (!out || !list) {
    return done(result, GTEXT_MARKUP_E_INVALID, GTEXT_CSS_E_INVALID, NULL);
  }
  return closest_impl(element, NULL, 0, list, options, true, out, NULL, result);
}

GTEXT_Markup_Status gtext_markup_closest(const GTEXT_Markup_Node * element, const char * selector, size_t length, const GTEXT_Markup_Query_Options * options,
    GTEXT_Markup_Node ** out, GTEXT_Markup_Query_Result * result) {
  if (!out) {
    return done(result, GTEXT_MARKUP_E_INVALID, GTEXT_CSS_E_INVALID, NULL);
  }
  return closest_impl(element, selector, length, NULL, options, false, NULL, out, result);
}

GTEXT_Markup_Status gtext_markup_closest_compiled(const GTEXT_Markup_Node * element, const GTEXT_CSS_Selector_List * list,
    const GTEXT_Markup_Query_Options * options, GTEXT_Markup_Node ** out, GTEXT_Markup_Query_Result * result) {
  if (!out || !list) {
    return done(result, GTEXT_MARKUP_E_INVALID, GTEXT_CSS_E_INVALID, NULL);
  }
  return closest_impl(element, NULL, 0, list, options, false, NULL, out, result);
}
