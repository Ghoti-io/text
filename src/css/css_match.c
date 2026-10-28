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
 * The matcher (css_match.h): a compound selector against an element, a complex selector right to left with every step memoized, the pseudo-classes, and the context and its
 * options. `:nth-*` is css_match_nth.c, `:has()` css_match_has.c, `:lang()` css_match_lang.c, the memo css_match_memo.c and the ancestor filter css_match_bloom.c.
 */

#include <string.h>

#include "css_match_internal.h"

#define DEFAULT_MAX_MEMO ((size_t)1 << 22)
#define DEFAULT_MAX_WORK ((size_t)1 << 26)
#define DEFAULT_MAX_HAS ((size_t)1 << 20)
#define XHTML_NS "http://www.w3.org/1999/xhtml"

/* ---- the context -------------------------------------------------------------------------------------------- */

GTEXT_CSS_Selector_Match_Options gtext_css_selector_match_options_default(void) {
  GTEXT_CSS_Selector_Match_Options o;
  memset(&o, 0, sizeof o);
  o.max_memo_entries = DEFAULT_MAX_MEMO;
  o.max_work = DEFAULT_MAX_WORK;
  o.max_has_visits = DEFAULT_MAX_HAS;
  return o;
}

GTEXT_CSS_Status gtext_css_selector_match_context_new(const GTEXT_CSS_Element_Interface * iface, const GTEXT_CSS_Selector_Match_Options * options,
    GTEXT_CSS_Selector_Match_Context ** out) {
  if (!iface || !out || iface->size < offsetof(GTEXT_CSS_Element_Interface, user) + sizeof iface->user) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Selector_Match_Options o = options ? *options : gtext_css_selector_match_options_default();
  if (!o.max_memo_entries) {
    o.max_memo_entries = DEFAULT_MAX_MEMO;
  }
  if (!o.max_work) {
    o.max_work = DEFAULT_MAX_WORK;
  }
  if (!o.max_has_visits) {
    o.max_has_visits = DEFAULT_MAX_HAS;
  }
  const GTEXT_Allocator * ap = o.allocator ? o.allocator : gtext_allocator_default();
  GTEXT_Allocator a = *ap;
  struct GTEXT_CSS_Selector_Match_Context * ctx = gtext_allocator_calloc(&a, 1, sizeof *ctx);
  if (!ctx) {
    return GTEXT_CSS_E_OOM;
  }
  ctx->alloc = a;
  o.allocator = NULL;
  ctx->opt = o;
  const size_t n = iface->size < sizeof ctx->iface ? iface->size : sizeof ctx->iface;
  memcpy(&ctx->iface, iface, n);
  ctx->iface.size = n;
  for (int cb = 0; cb < (int)GTEXT_CSS_ELEMENT_CB_COUNT; cb++) {
    if (gtext_css_element_has(&ctx->iface, (GTEXT_CSS_Element_Callback)cb)) {
      ctx->avail |= 1ull << cb;
    }
  }
  ctx->scope = o.scope;
  ctx->tree_scope = o.tree_scope;
  *out = ctx;
  return GTEXT_CSS_OK;
}

void gtext_css_selector_match_context_free(GTEXT_CSS_Selector_Match_Context * ctx) {
  if (!ctx) {
    return;
  }
  GTEXT_Allocator a = ctx->alloc;
  gtext_allocator_free(&a, ctx->tab);
  gtext_allocator_free(&a, ctx->bloom);
  gtext_allocator_free(&a, ctx->path);
  gtext_allocator_free(&a, ctx->pathv);
  gtext_allocator_free(&a, ctx);
}

void gtext_css_selector_match_reset(GTEXT_CSS_Selector_Match_Context * ctx) {
  if (!ctx) {
    return;
  }
  css_memo_clear(ctx);
  css_bloom_clear(ctx);
}

void gtext_css_selector_match_set_scope(GTEXT_CSS_Selector_Match_Context * ctx, GTEXT_CSS_Element_Handle scope, const void * tree_scope) {
  if (!ctx) {
    return;
  }
  ctx->scope = scope;
  ctx->tree_scope = tree_scope;
  css_memo_clear(ctx);
}

GTEXT_CSS_Selector_Match_Counters gtext_css_selector_match_counters(const GTEXT_CSS_Selector_Match_Context * ctx) {
  GTEXT_CSS_Selector_Match_Counters c;
  memset(&c, 0, sizeof c);
  if (ctx) {
    c = ctx->ctr;
    c.memo_entries = ctx->used;
  }
  return c;
}

void gtext_css_selector_match_counters_reset(GTEXT_CSS_Selector_Match_Context * ctx) {
  if (ctx) {
    const size_t entries = ctx->used;
    memset(&ctx->ctr, 0, sizeof ctx->ctr);
    ctx->ctr.memo_entries = entries;
  }
}

/* ---- failing, counting -------------------------------------------------------------------------------------- */

bool css_match_fail(struct GTEXT_CSS_Selector_Match_Context * ctx, GTEXT_CSS_Status st) {
  if (ctx->status == GTEXT_CSS_OK) {
    ctx->status = st;
  }
  return false;
}

bool css_match_missing(struct GTEXT_CSS_Selector_Match_Context * ctx, GTEXT_CSS_Element_Callback cb) {
  if (ctx->status == GTEXT_CSS_OK) {
    ctx->status = GTEXT_CSS_E_UNSUPPORTED;
    ctx->missing = gtext_css_element_callback_name(cb);
  }
  return false;
}

bool css_match_work(struct GTEXT_CSS_Selector_Match_Context * ctx) {
  if (++ctx->ctr.work > ctx->opt.max_work) {
    return css_match_fail(ctx, GTEXT_CSS_E_LIMIT_MATCH_WORK);
  }
  return ctx->status == GTEXT_CSS_OK;
}

#define HAVE(ctx, cb) (((ctx)->avail >> (cb)) & 1u)

/* ---- reading the element -------------------------------------------------------------------------------------- */

static El e_parent(struct GTEXT_CSS_Selector_Match_Context * ctx, El e) {
  return ctx->iface.parent(ctx->iface.user, e);
}

static bool e_ns(struct GTEXT_CSS_Selector_Match_Context * ctx, El e, GTEXT_CSS_String * ns) {
  ns->data = NULL;
  ns->length = 0;
  if (!HAVE(ctx, GTEXT_CSS_ELEMENT_CB_NAMESPACE_URI)) {
    return false;
  }
  if (!ctx->iface.namespace_uri(ctx->iface.user, e, ns) || !ns->data || ns->length == 0) {
    ns->data = NULL;
    ns->length = 0;
    return false;
  }
  return true;
}

bool css_match_is_html(struct GTEXT_CSS_Selector_Match_Context * ctx, El element) {
  if (!ctx->opt.html_document) {
    return false;
  }
  if (!HAVE(ctx, GTEXT_CSS_ELEMENT_CB_NAMESPACE_URI)) {
    return true;
  }
  GTEXT_CSS_String ns;
  if (!e_ns(ctx, element, &ns)) {
    return true; // "An element whose namespace the interface does not give is taken as HTML" (GTEXT_CSS_Selector_Match_Options::html_document)
  }
  return ns.length == sizeof XHTML_NS - 1 && memcmp(ns.data, XHTML_NS, ns.length) == 0;
}

static bool s_eq(const char * a, size_t an, const char * b, size_t bn, bool ci) {
  if (an != bn) {
    return false;
  }
  if (!ci) {
    return an == 0 || memcmp(a, b, an) == 0;
  }
  return css_sel_ieq(a, an, b, bn);
}

static bool is_root(struct GTEXT_CSS_Selector_Match_Context * ctx, El e) {
  if (ctx->opt.root_given) {
    return e == ctx->opt.root;
  }
  if (e_parent(ctx, e)) {
    return false;
  }
  return !(HAVE(ctx, GTEXT_CSS_ELEMENT_CB_CONTAINING_HOST) && ctx->iface.containing_host(ctx->iface.user, e));
}

static bool is_scope(struct GTEXT_CSS_Selector_Match_Context * ctx, El e) {
  return ctx->scope ? e == ctx->scope : is_root(ctx, e);
}

El css_flat_parent(struct GTEXT_CSS_Selector_Match_Context * ctx, El e) {
  if (HAVE(ctx, GTEXT_CSS_ELEMENT_CB_ASSIGNED_SLOT)) {
    El slot = ctx->iface.assigned_slot(ctx->iface.user, e);
    if (slot) {
      return slot;
    }
  }
  El p = e_parent(ctx, e);
  if (p) {
    return p;
  }
  return HAVE(ctx, GTEXT_CSS_ELEMENT_CB_CONTAINING_HOST) ? ctx->iface.containing_host(ctx->iface.user, e) : NULL;
}

/* ---- simple selectors ------------------------------------------------------------------------------------------ */

static bool ns_ok(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Simple * s, El e) {
  const GTEXT_CSS_String * want = NULL;
  bool none = false;
  switch (s->ns_kind) {
  case GTEXT_CSS_NS_ANY: return true;
  case GTEXT_CSS_NS_NONE: none = true; break;
  case GTEXT_CSS_NS_NAMED: want = &s->ns_uri; break;
  case GTEXT_CSS_NS_UNSPECIFIED:
    if (!s->ns_from_default) {
      return true;
    }
    want = &s->ns_uri;
    break;
  }
  if (!HAVE(ctx, GTEXT_CSS_ELEMENT_CB_NAMESPACE_URI)) {
    return css_match_missing(ctx, GTEXT_CSS_ELEMENT_CB_NAMESPACE_URI);
  }
  GTEXT_CSS_String ns;
  const bool has = e_ns(ctx, e, &ns);
  if (none) {
    return !has;
  }
  return has && ns.length == want->length && memcmp(ns.data, want->data, ns.length) == 0;
}

static bool match_type(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Simple * s, El e) {
  if (!ns_ok(ctx, s, e)) {
    return false;
  }
  if (s->kind == GTEXT_CSS_SIMPLE_UNIVERSAL) {
    return true;
  }
  GTEXT_CSS_String ln;
  if (!ctx->iface.local_name(ctx->iface.user, e, &ln) || !ln.data) {
    return false;
  }
  return s_eq(s->name.data, s->name.length, ln.data, ln.length, css_match_is_html(ctx, e));
}

static bool match_id(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Simple * s, El e) {
  GTEXT_CSS_String id;
  if (!ctx->iface.id(ctx->iface.user, e, &id) || !id.data) {
    return false;
  }
  return s_eq(s->name.data, s->name.length, id.data, id.length, ctx->opt.quirks);
}

static bool match_class(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Simple * s, El e) {
  const size_t n = ctx->iface.class_count(ctx->iface.user, e);
  for (size_t i = 0; i < n; i++) {
    GTEXT_CSS_String c;
    if (ctx->iface.class_at(ctx->iface.user, e, i, &c) && c.data && s_eq(s->name.data, s->name.length, c.data, c.length, ctx->opt.quirks)) {
      return true;
    }
    if (!css_match_work(ctx)) {
      return false;
    }
  }
  return false;
}

static bool ws(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

static bool in_ci_list(const char * name, size_t n) {
  for (size_t i = 0; i < sizeof html_ci_attributes / sizeof html_ci_attributes[0]; i++) {
    if (strlen(html_ci_attributes[i]) == n && memcmp(html_ci_attributes[i], name, n) == 0) {
      return true;
    }
  }
  return false;
}

static bool match_attribute(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Simple * s, El e) {
  const bool html = css_match_is_html(ctx, e);
  GTEXT_CSS_String name = s->name;
  char lower[128];
  const bool unnamespaced = s->ns_kind != GTEXT_CSS_NS_NAMED; // `*|name` is compared as the name of an attribute without a namespace is
  if (html && unnamespaced && name.length < sizeof lower) {
    for (size_t i = 0; i < name.length; i++) {
      lower[i] = (char)css_sel_lower((unsigned char)name.data[i]);
    }
    lower[name.length] = '\0';
    name.data = lower;
  }
  GTEXT_CSS_String nsstr;
  const GTEXT_CSS_String * ns = NULL;
  if (s->ns_kind == GTEXT_CSS_NS_ANY) {
    ns = gtext_css_namespace_any();
  }
  else if (s->ns_kind == GTEXT_CSS_NS_NAMED) {
    nsstr = s->ns_uri;
    ns = &nsstr;
  }
  GTEXT_CSS_String v;
  if (!ctx->iface.attribute(ctx->iface.user, e, ns, &name, &v) || !v.data) {
    return false;
  }
  if (s->op == GTEXT_CSS_ATTR_EXISTS) {
    return true;
  }
  bool ci;
  switch (s->attr_case) {
  case GTEXT_CSS_ATTR_CASE_INSENSITIVE: ci = true; break;
  case GTEXT_CSS_ATTR_CASE_SENSITIVE: ci = false; break;
  default: ci = html && unnamespaced && in_ci_list(name.data, name.length); break;
  }
  const char * w = s->value.data;
  const size_t wn = s->value.length;
  switch (s->op) {
  case GTEXT_CSS_ATTR_EQUALS: return s_eq(v.data, v.length, w, wn, ci);
  case GTEXT_CSS_ATTR_INCLUDES: {
    if (wn == 0) {
      return false;
    }
    for (size_t i = 0; i < wn; i++) {
      if (ws(w[i])) {
        return false;
      }
    }
    size_t i = 0;
    while (i < v.length) {
      while (i < v.length && ws(v.data[i])) {
        i++;
      }
      size_t j = i;
      while (j < v.length && !ws(v.data[j])) {
        j++;
      }
      if (j > i && s_eq(v.data + i, j - i, w, wn, ci)) {
        return true;
      }
      i = j;
    }
    return false;
  }
  case GTEXT_CSS_ATTR_DASH:
    if (s_eq(v.data, v.length, w, wn, ci)) {
      return true;
    }
    return v.length > wn && v.data[wn] == '-' && s_eq(v.data, wn, w, wn, ci);
  case GTEXT_CSS_ATTR_PREFIX: return wn > 0 && v.length >= wn && s_eq(v.data, wn, w, wn, ci);
  case GTEXT_CSS_ATTR_SUFFIX: return wn > 0 && v.length >= wn && s_eq(v.data + v.length - wn, wn, w, wn, ci);
  case GTEXT_CSS_ATTR_SUBSTRING:
    if (wn == 0 || v.length < wn) {
      return false;
    }
    for (size_t i = 0; i + wn <= v.length; i++) {
      if (s_eq(v.data + i, wn, w, wn, ci)) {
        return true;
      }
      if ((i & 1023u) == 1023u && !css_match_work(ctx)) {
        return false;
      }
    }
    return false;
  default: return false;
  }
}

static bool match_nesting(struct GTEXT_CSS_Selector_Match_Context * ctx, El e) {
  const css_nest * n = ctx->nest;
  if (!n || !n->list) {
    return is_scope(ctx, e);
  }
  ctx->nest = n->parent;
  const bool r = css_match_any(ctx, n->list, e);
  ctx->nest = n;
  return r;
}

/* ---- pseudo-classes ----------------------------------------------------------------------------------------- */

static uint64_t state_of(struct GTEXT_CSS_Selector_Match_Context * ctx, El e) {
  uint64_t st = ctx->iface.state(ctx->iface.user, e);
  if (ctx->opt.visited == GTEXT_CSS_VISITED_NEVER) {
    if (st & GTEXT_CSS_STATE_VISITED) {
      st |= GTEXT_CSS_STATE_LINK;
    }
    st &= ~(uint64_t)GTEXT_CSS_STATE_VISITED;
  }
  return st;
}

/// The level of an HTML heading element (1 to 6), or 0.
static int heading_level(struct GTEXT_CSS_Selector_Match_Context * ctx, El e) {
  GTEXT_CSS_String ln;
  if (!ctx->iface.local_name(ctx->iface.user, e, &ln) || !ln.data || ln.length != 2 || css_sel_lower((unsigned char)ln.data[0]) != 'h' ||
      ln.data[1] < '1' || ln.data[1] > '6') {
    return 0;
  }
  GTEXT_CSS_String ns;
  if (HAVE(ctx, GTEXT_CSS_ELEMENT_CB_NAMESPACE_URI)) {
    if (!e_ns(ctx, e, &ns) || ns.length != sizeof XHTML_NS - 1 || memcmp(ns.data, XHTML_NS, ns.length) != 0) {
      return 0;
    }
  }
  return ln.data[1] - '0';
}

static bool eval_pseudo_class(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Simple * s, El e) {
  const sel_row * r = css_sel_row_of(s);
  if (r->status == SELS_PARSE_ONLY && (s->pseudo == GTEXT_CSS_SEL_PC_CURRENT_FN || s->pseudo == GTEXT_CSS_SEL_PC_ACTIVE_VIEW_TRANSITION_TYPE_FN)) {
    if (ctx->status == GTEXT_CSS_OK) {
      ctx->status = GTEXT_CSS_E_UNSUPPORTED;
      ctx->unsupported = r->name;
    }
    return false;
  }
  switch (s->pseudo) {
  case GTEXT_CSS_SEL_PC_IS_FN:
  case GTEXT_CSS_SEL_PC_WHERE_FN: return css_match_any(ctx, s->list, e);
  case GTEXT_CSS_SEL_PC_NOT_FN: {
    const bool m = css_match_any(ctx, s->list, e);
    return ctx->status == GTEXT_CSS_OK && !m;
  }
  case GTEXT_CSS_SEL_PC_HAS_FN: return css_match_has(ctx, s, e);
  case GTEXT_CSS_SEL_PC_ROOT: return is_root(ctx, e);
  case GTEXT_CSS_SEL_PC_SCOPE: return is_scope(ctx, e);
  case GTEXT_CSS_SEL_PC_EMPTY: return (ctx->iface.child_content(ctx->iface.user, e) & (GTEXT_CSS_CHILD_TEXT | GTEXT_CSS_CHILD_ELEMENT)) == 0;
  case GTEXT_CSS_SEL_PC_FIRST_CHILD: return ctx->iface.previous_sibling(ctx->iface.user, e) == NULL;
  case GTEXT_CSS_SEL_PC_LAST_CHILD: return ctx->iface.next_sibling(ctx->iface.user, e) == NULL;
  case GTEXT_CSS_SEL_PC_ONLY_CHILD:
    return ctx->iface.previous_sibling(ctx->iface.user, e) == NULL && ctx->iface.next_sibling(ctx->iface.user, e) == NULL;
  case GTEXT_CSS_SEL_PC_NTH_CHILD_FN:
  case GTEXT_CSS_SEL_PC_NTH_LAST_CHILD_FN:
  case GTEXT_CSS_SEL_PC_NTH_OF_TYPE_FN:
  case GTEXT_CSS_SEL_PC_NTH_LAST_OF_TYPE_FN: return css_match_nth(ctx, s, e);
  case GTEXT_CSS_SEL_PC_FIRST_OF_TYPE:
  case GTEXT_CSS_SEL_PC_LAST_OF_TYPE:
  case GTEXT_CSS_SEL_PC_ONLY_OF_TYPE: return css_match_of_type(ctx, s->pseudo, e);
  case GTEXT_CSS_SEL_PC_NTH_COL_FN:
  case GTEXT_CSS_SEL_PC_NTH_LAST_COL_FN: return false;
  case GTEXT_CSS_SEL_PC_DIR_FN: {
    const GTEXT_CSS_Direction d = ctx->iface.direction(ctx->iface.user, e);
    return (d == GTEXT_CSS_DIRECTION_RTL) == (s->strings[0].data[0] == 'r');
  }
  case GTEXT_CSS_SEL_PC_LANG_FN: return css_match_lang(ctx, s, e);
  case GTEXT_CSS_SEL_PC_HEADING: return heading_level(ctx, e) != 0;
  case GTEXT_CSS_SEL_PC_HEADING_FN: {
    const int level = heading_level(ctx, e);
    for (size_t i = 0; level && i < s->level_count; i++) {
      if (s->levels[i] == level) {
        return true;
      }
    }
    return false;
  }
  case GTEXT_CSS_SEL_PC_HOST: return ctx->tree_scope && ctx->iface.shadow_scope(ctx->iface.user, e) == ctx->tree_scope;
  case GTEXT_CSS_SEL_PC_HOST_FN:
    return ctx->tree_scope && ctx->iface.shadow_scope(ctx->iface.user, e) == ctx->tree_scope && css_match_any(ctx, s->list, e);
  case GTEXT_CSS_SEL_PC_HOST_CONTEXT_FN: {
    if (!ctx->tree_scope || ctx->iface.shadow_scope(ctx->iface.user, e) != ctx->tree_scope) {
      return false;
    }
    for (El x = e; x; x = css_flat_parent(ctx, x)) {
      if (css_match_any(ctx, s->list, x)) {
        return true;
      }
      if (ctx->status != GTEXT_CSS_OK || !css_match_work(ctx)) {
        return false;
      }
    }
    return false;
  }
  case GTEXT_CSS_SEL_PC_STATE_FN: return ctx->iface.custom_state(ctx->iface.user, e, &s->strings[0]);
  default: break;
  }
  if (r->state) {
    return (state_of(ctx, e) & r->state) != 0;
  }
  return false;
}

static bool eval_simple(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Simple * s, El e) {
  switch (s->kind) {
  case GTEXT_CSS_SIMPLE_TYPE:
  case GTEXT_CSS_SIMPLE_UNIVERSAL: return match_type(ctx, s, e);
  case GTEXT_CSS_SIMPLE_ID: return match_id(ctx, s, e);
  case GTEXT_CSS_SIMPLE_CLASS: return match_class(ctx, s, e);
  case GTEXT_CSS_SIMPLE_ATTRIBUTE: return match_attribute(ctx, s, e);
  case GTEXT_CSS_SIMPLE_NESTING: return match_nesting(ctx, e);
  case GTEXT_CSS_SIMPLE_PSEUDO_CLASS: return eval_pseudo_class(ctx, s, e);
  case GTEXT_CSS_SIMPLE_PSEUDO_ELEMENT: return true;
  }
  return false;
}

/// The quirk of the Quirks Mode Standard, 4.1: a compound selector of nothing but `:hover` and `:active` matches only what `:any-link` matches.
static bool hover_active_quirk_applies(const GTEXT_CSS_Compound * cmp, size_t limit) {
  bool any = false;
  for (size_t i = 0; i < limit; i++) {
    const GTEXT_CSS_Simple * s = &cmp->simples[i];
    if (s->kind == GTEXT_CSS_SIMPLE_UNIVERSAL && s->ns_kind != GTEXT_CSS_NS_NAMED) {
      continue;
    }
    if (s->kind != GTEXT_CSS_SIMPLE_PSEUDO_CLASS || (s->pseudo != GTEXT_CSS_SEL_PC_HOVER && s->pseudo != GTEXT_CSS_SEL_PC_ACTIVE)) {
      return false;
    }
    any = true;
  }
  return any;
}

bool css_match_compound(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Compound * cmp, El element, size_t limit) {
  ctx->ctr.compounds_tested++;
  if (!css_match_work(ctx)) {
    return false;
  }
  if (ctx->opt.quirks && HAVE(ctx, GTEXT_CSS_ELEMENT_CB_STATE) && hover_active_quirk_applies(cmp, limit)) {
    if ((state_of(ctx, element) & (GTEXT_CSS_STATE_LINK | GTEXT_CSS_STATE_VISITED)) == 0) {
      return false;
    }
  }
  for (size_t i = 0; i < limit; i++) {
    const GTEXT_CSS_Simple * s = &cmp->simples[i];
    if (s->never) {
      return false;
    }
    if (!eval_simple(ctx, s, element)) {
      return false;
    }
  }
  return ctx->status == GTEXT_CSS_OK;
}

/* ---- complex selectors -------------------------------------------------------------------------------------- */

bool css_match_any(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Selector_List * list, El element) {
  if (!list) {
    return false;
  }
  for (size_t i = 0; i < list->count; i++) {
    const GTEXT_CSS_Complex * cx = &list->complex[i];
    if (cx->never || cx->raw.data || cx->has_pseudo_element) {
      continue; // a pseudo-element in the list a `&` stands for represents nothing (CSS Nesting 1); no other list that is matched this way holds one
    }
    if (css_match_chain(ctx, list, cx, cx->compound_count - 1, element)) {
      return true;
    }
    if (ctx->status != GTEXT_CSS_OK) {
      return false;
    }
  }
  return false;
}

/// Whether a compound has `:host`, `:host()` or `:host-context()` among its simple selectors, or `&`, which may stand for a selector that has (the matching of `&` decides).
bool css_compound_names_host(const GTEXT_CSS_Compound * cmp) {
  for (size_t i = 0; i < cmp->simple_count; i++) {
    const GTEXT_CSS_Simple * s = &cmp->simples[i];
    if (s->kind == GTEXT_CSS_SIMPLE_NESTING) {
      return true;
    }
    if (s->kind == GTEXT_CSS_SIMPLE_PSEUDO_CLASS &&
        (s->pseudo == GTEXT_CSS_SEL_PC_HOST || s->pseudo == GTEXT_CSS_SEL_PC_HOST_FN || s->pseudo == GTEXT_CSS_SEL_PC_HOST_CONTEXT_FN)) {
      return true;
    }
  }
  return false;
}

/// The parent of an element for the combinators of a selector read in a shadow tree: the tree parent, and above the top of the shadow tree the host, which only a compound that
/// names `:host` may be matched against ("the shadow host is the parent of the top-level elements of its shadow tree" for `:host > *` and `:host p`, and no other compound of an
/// outer tree is reachable from inside one).
static El parent_for(struct GTEXT_CSS_Selector_Match_Context * ctx, El e, const GTEXT_CSS_Compound * target) {
  El p = e_parent(ctx, e);
  if (p || !ctx->tree_scope || !HAVE(ctx, GTEXT_CSS_ELEMENT_CB_CONTAINING_HOST) || !css_compound_names_host(target)) {
    return p;
  }
  return ctx->iface.containing_host(ctx->iface.user, e);
}

static const void * memo_aux_for(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Complex * cx) {
  if (!cx->anchor_dependent) {
    return NULL;
  }
  return ctx->scope ? (const void *)ctx->scope : (const void *)&ctx->scope;
}

static bool exists_above(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Selector_List * list, const GTEXT_CSS_Complex * cx, size_t j, El el,
    bool siblings, const void * aux);

bool css_match_chain(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Selector_List * list, const GTEXT_CSS_Complex * cx, size_t index, El element) {
  const GTEXT_CSS_Compound * cmp = &cx->compounds[index];
  const size_t limit = index == cx->compound_count - 1 ? cmp->first_pseudo_element : cmp->simple_count;
  const void * aux = memo_aux_for(ctx, cx);
  css_memo_ent m;
  if (css_memo_get(ctx, list->uid, MK_M, (uint32_t)cmp->id, element, aux, &m)) {
    return m.val == 2;
  }
  bool ok = css_match_compound(ctx, cmp, element, limit);
  if (ok && index > 0) {
    switch (cmp->combinator) {
    case GTEXT_CSS_COMBINATOR_CHILD: {
      El p = parent_for(ctx, element, &cx->compounds[index - 1]);
      ok = p && css_match_chain(ctx, list, cx, index - 1, p);
      break;
    }
    case GTEXT_CSS_COMBINATOR_DESCENDANT: ok = exists_above(ctx, list, cx, index - 1, element, false, aux); break;
    case GTEXT_CSS_COMBINATOR_NEXT_SIBLING: {
      El q = ctx->iface.previous_sibling(ctx->iface.user, element);
      ok = q && css_match_chain(ctx, list, cx, index - 1, q);
      break;
    }
    case GTEXT_CSS_COMBINATOR_SUBSEQUENT_SIBLING: ok = exists_above(ctx, list, cx, index - 1, element, true, aux); break;
    default: ok = false; break;
    }
  }
  if (ctx->status != GTEXT_CSS_OK) {
    return false;
  }
  css_memo_put(ctx, list->uid, MK_M, (uint32_t)cmp->id, element, aux, ok ? 2 : 1, 0, 0);
  return ok && ctx->status == GTEXT_CSS_OK;
}

static El step_up(struct GTEXT_CSS_Selector_Match_Context * ctx, El e, bool siblings, const GTEXT_CSS_Compound * target) {
  return siblings ? ctx->iface.previous_sibling(ctx->iface.user, e) : parent_for(ctx, e, target);
}

/// Some proper ancestor (or, with `siblings`, earlier sibling) of `el` matches the compound `j` and the chain to its left. Iterative, and the answer is kept for every element climbed
/// past, so that the next element of the same parent or run is answered from the memo.
static bool exists_above(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Selector_List * list, const GTEXT_CSS_Complex * cx, size_t j, El el,
    bool siblings, const void * aux) {
  const uint16_t kind = siblings ? MK_S : MK_D;
  const uint32_t step = (uint32_t)cx->compounds[j].id;
  css_memo_ent m;
  if (css_memo_get(ctx, list->uid, kind, step, el, aux, &m)) {
    return m.val == 2;
  }
  bool result = false;
  El stop = NULL; // the element whose own answer is not known: the first climbed past that matched
  for (El x = el;;) {
    El p = step_up(ctx, x, siblings, &cx->compounds[j]);
    if (!p) {
      result = false;
      break;
    }
    if (!css_match_work(ctx)) {
      return false;
    }
    if (css_match_chain(ctx, list, cx, j, p)) {
      result = true;
      stop = p;
      break;
    }
    if (ctx->status != GTEXT_CSS_OK) {
      return false;
    }
    if (css_memo_get(ctx, list->uid, kind, step, p, aux, &m)) {
      result = m.val == 2;
      stop = p; // its answer is kept, and what is above it is not this climb's to say
      break;
    }
    x = p;
  }
  // the second climb: every element from `el` up to the stop gets the answer
  for (El x = el; x && x != stop;) {
    if (!css_memo_put(ctx, list->uid, kind, step, x, aux, result ? 2 : 1, 0, 0)) {
      return false;
    }
    x = step_up(ctx, x, siblings, &cx->compounds[j]);
  }
  return result;
}

/* ---- a complex selector with pseudo-elements and the shadow selectors ------------------------------------------------- */

static bool same_pe(const GTEXT_CSS_Simple * a, const GTEXT_CSS_Simple * b) {
  if (a->pseudo != b->pseudo || a->anb_a != b->anb_a || a->anb_b != b->anb_b || a->string_count != b->string_count) {
    return false;
  }
  for (size_t i = 0; i < a->string_count; i++) {
    if (a->strings[i].length != b->strings[i].length || (a->strings[i].length && memcmp(a->strings[i].data, b->strings[i].data, a->strings[i].length))) {
      return false;
    }
  }
  return true;
}

bool css_match_complex_full(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Selector_List * list, const GTEXT_CSS_Complex * cx, El element,
    const GTEXT_CSS_Complex * request, El slot, El host) {
  if (cx->never || cx->raw.data || cx->relative) {
    return false;
  }
  const size_t n = cx->compound_count;
  const GTEXT_CSS_Compound * last = &cx->compounds[n - 1];
  const size_t k = last->first_pseudo_element;
  if (ctx->bloom_live && element == ctx->bloom_for && !ctx->opt.disable_bloom && !css_bloom_may_match(ctx, cx)) {
    return false;
  }
  if (k == last->simple_count) {
    return request == NULL && css_match_chain(ctx, list, cx, n - 1, element);
  }
  const GTEXT_CSS_Simple * first = &last->simples[k];
  size_t after = k;
  El origin = element;
  if (first->pseudo == GTEXT_CSS_SEL_PE_SLOTTED_FN) {
    if (!HAVE(ctx, GTEXT_CSS_ELEMENT_CB_ASSIGNED_SLOT)) {
      return css_match_missing(ctx, GTEXT_CSS_ELEMENT_CB_ASSIGNED_SLOT);
    }
    El s = slot ? slot : ctx->iface.assigned_slot(ctx->iface.user, element);
    if (!s || !css_match_any(ctx, first->list, element) || ctx->status != GTEXT_CSS_OK) {
      return false;
    }
    origin = s;
    after = k + 1;
  }
  else if (first->pseudo == GTEXT_CSS_SEL_PE_PART_FN) {
    if (!HAVE(ctx, GTEXT_CSS_ELEMENT_CB_HAS_PART)) {
      return css_match_missing(ctx, GTEXT_CSS_ELEMENT_CB_HAS_PART);
    }
    if (!HAVE(ctx, GTEXT_CSS_ELEMENT_CB_CONTAINING_HOST)) {
      return css_match_missing(ctx, GTEXT_CSS_ELEMENT_CB_CONTAINING_HOST);
    }
    bool found = false;
    for (El h = host ? host : ctx->iface.containing_host(ctx->iface.user, element); h && !found;
        h = host ? NULL : ctx->iface.containing_host(ctx->iface.user, h)) {
      bool all = true;
      for (size_t i = 0; i < first->string_count && all; i++) {
        all = ctx->iface.has_part(ctx->iface.user, element, h, &first->strings[i]);
      }
      if (all && css_match_chain(ctx, list, cx, n - 1, h)) {
        found = true;
      }
      if (ctx->status != GTEXT_CSS_OK || !css_match_work(ctx)) {
        return false;
      }
    }
    if (!found) {
      return false;
    }
    origin = element; // the pseudo-classes that follow are the part's
    after = k + 1;
  }
  else {
    if (!css_match_chain(ctx, list, cx, n - 1, element)) {
      return false;
    }
    after = k;
  }
  if (first->pseudo == GTEXT_CSS_SEL_PE_SLOTTED_FN && !css_match_chain(ctx, list, cx, n - 1, origin)) {
    return false;
  }
  // the pseudo-classes that follow the pseudo-element are tests of the element
  size_t chain = 0;
  for (size_t i = k; i < last->simple_count; i++) {
    const GTEXT_CSS_Simple * s = &last->simples[i];
    if (s->kind == GTEXT_CSS_SIMPLE_PSEUDO_ELEMENT) {
      if (i >= after) {
        chain++;
      }
      continue;
    }
    if (s->never || !eval_simple(ctx, s, element) || ctx->status != GTEXT_CSS_OK) {
      return false;
    }
  }
  if (!request) {
    return chain == 0;
  }
  const GTEXT_CSS_Compound * rc = &request->compounds[0];
  if (chain != rc->simple_count) {
    return false;
  }
  size_t ri = 0;
  for (size_t i = after; i < last->simple_count; i++) {
    const GTEXT_CSS_Simple * s = &last->simples[i];
    if (s->kind == GTEXT_CSS_SIMPLE_PSEUDO_ELEMENT && !same_pe(s, &rc->simples[ri++])) {
      return false;
    }
  }
  return true;
}

/* ---- the public match ------------------------------------------------------------------------------------------- */

static void begin_call(struct GTEXT_CSS_Selector_Match_Context * ctx) {
  ctx->status = GTEXT_CSS_OK;
  ctx->missing = NULL;
  ctx->unsupported = NULL;
  ctx->ctr.work = 0;
  ctx->has_count = 0;
}

static GTEXT_CSS_Status finish_call(struct GTEXT_CSS_Selector_Match_Context * ctx, GTEXT_CSS_Selector_Match_Result * result, bool matched, size_t index,
    GTEXT_CSS_Specificity spec) {
  const GTEXT_CSS_Status st = ctx->status;
  if (result) {
    memset(result, 0, sizeof *result);
    result->status = st;
    result->message = gtext_css_status_string(st);
    result->callback = st == GTEXT_CSS_E_UNSUPPORTED ? ctx->missing : NULL;
    result->selector = st == GTEXT_CSS_E_UNSUPPORTED ? ctx->unsupported : NULL;
    result->matched = st == GTEXT_CSS_OK && matched;
    result->complex_index = index;
    result->specificity = spec;
    result->span = ctx->fail_span;
  }
  return st;
}

/// The callbacks a list asks for that the table lacks; fails the call naming the first.
static bool callbacks_present(struct GTEXT_CSS_Selector_Match_Context * ctx, const GTEXT_CSS_Selector_List * list) {
  const uint64_t missing = list->callbacks & ~ctx->avail;
  if (!missing) {
    return true;
  }
  for (int cb = 0; cb < (int)GTEXT_CSS_ELEMENT_CB_COUNT; cb++) {
    if (missing >> cb & 1u) {
      return css_match_missing(ctx, (GTEXT_CSS_Element_Callback)cb);
    }
  }
  return false;
}

GTEXT_CSS_Status gtext_css_selector_match(const GTEXT_CSS_Selector_List * list, GTEXT_CSS_Selector_Match_Context * ctx, GTEXT_CSS_Element_Handle element,
    const GTEXT_CSS_Selector_List * pseudo, GTEXT_CSS_Selector_Match_Result * result) {
  if (!list || !ctx || !element) {
    return GTEXT_CSS_E_INVALID;
  }
  const GTEXT_CSS_Complex * request = NULL;
  if (pseudo) {
    if (pseudo->count != 1 || pseudo->complex[0].compound_count != 1 || pseudo->complex[0].compounds[0].first_pseudo_element != 0 ||
        pseudo->complex[0].compounds[0].simple_count == 0) {
      return GTEXT_CSS_E_INVALID;
    }
    request = &pseudo->complex[0];
  }
  begin_call(ctx);
  if (!callbacks_present(ctx, list)) {
    return finish_call(ctx, result, false, 0, gtext_css_specificity_make(0, 0, 0));
  }
  bool found = false;
  size_t best = 0;
  GTEXT_CSS_Specificity bs = gtext_css_specificity_make(0, 0, 0);
  for (size_t i = 0; i < list->count && ctx->status == GTEXT_CSS_OK; i++) {
    const GTEXT_CSS_Complex * cx = &list->complex[i];
    if (css_match_complex_full(ctx, list, cx, element, request, NULL, NULL)) {
      if (!found || gtext_css_specificity_compare(cx->specificity, bs) > 0) {
        best = i;
        bs = cx->specificity;
      }
      found = true;
    }
  }
  return finish_call(ctx, result, found, best, bs);
}

/* ---- walking ---------------------------------------------------------------------------------------------------- */

GTEXT_CSS_Status gtext_css_selector_match_walk(GTEXT_CSS_Selector_Match_Context * ctx, GTEXT_CSS_Element_Handle root, GTEXT_CSS_Walk_Fn visitor, void * user,
    GTEXT_CSS_Selector_Match_Result * result) {
  if (!ctx || !root || !visitor) {
    return GTEXT_CSS_E_INVALID;
  }
  begin_call(ctx);
  const GTEXT_CSS_Specificity zero = gtext_css_specificity_make(0, 0, 0);
  if (!HAVE(ctx, GTEXT_CSS_ELEMENT_CB_FIRST_CHILD)) {
    css_match_missing(ctx, GTEXT_CSS_ELEMENT_CB_FIRST_CHILD);
    return finish_call(ctx, result, false, 0, zero);
  }
  if (!HAVE(ctx, GTEXT_CSS_ELEMENT_CB_NEXT_SIBLING)) {
    css_match_missing(ctx, GTEXT_CSS_ELEMENT_CB_NEXT_SIBLING);
    return finish_call(ctx, result, false, 0, zero);
  }
  if (!HAVE(ctx, GTEXT_CSS_ELEMENT_CB_PARENT)) {
    css_match_missing(ctx, GTEXT_CSS_ELEMENT_CB_PARENT);
    return finish_call(ctx, result, false, 0, zero);
  }
  css_bloom_clear(ctx);
  size_t pushed = 0;
  if (!ctx->opt.disable_bloom) {
    for (El a = e_parent(ctx, root); a && ctx->status == GTEXT_CSS_OK; a = e_parent(ctx, a)) {
      if (!css_match_work(ctx) || !css_bloom_push(ctx, a)) {
        break;
      }
      pushed++;
    }
  }
  El e = root;
  while (ctx->status == GTEXT_CSS_OK) {
    ctx->bloom_for = e;
    ctx->bloom_live = !ctx->opt.disable_bloom;
    if (!visitor(user, e) || ctx->status != GTEXT_CSS_OK) {
      break;
    }
    ctx->bloom_live = false;
    El c = ctx->iface.first_child(ctx->iface.user, e);
    if (c) {
      if (!ctx->opt.disable_bloom && !css_bloom_push(ctx, e)) {
        break;
      }
      e = c;
      continue;
    }
    bool done = false;
    for (;;) {
      if (!css_match_work(ctx)) {
        done = true;
        break;
      }
      if (e == root) {
        done = true;
        break;
      }
      El nx = ctx->iface.next_sibling(ctx->iface.user, e);
      if (nx) {
        e = nx;
        break;
      }
      El p = e_parent(ctx, e);
      if (!p) {
        done = true;
        break;
      }
      if (!ctx->opt.disable_bloom && !css_bloom_pop(ctx, p)) {
        done = true;
        break;
      }
      e = p;
    }
    if (done) {
      break;
    }
  }
  (void)pushed;
  css_bloom_clear(ctx);
  return finish_call(ctx, result, false, 0, zero);
}
