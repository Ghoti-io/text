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
 * Container queries (css_container.h): the container info of a computed style, the query container of an element (a walk up the flat-tree ancestors, memoized
 * in the caller's call context so that a tree of n elements costs O(n) steps for each distinct request), and the evaluation of the size, scroll-state and
 * anchored features against the container. The style features are css_container_style.c.
 */

#include <ghoti.io/text/css/css_condition.h>
#include <ghoti.io/text/css/css_container.h>
#include <ghoti.io/text/css/css_logical.h>
#include <ghoti.io/text/css/css_value.h>

#include <string.h>

#include "css_container_internal.h"

/* ---- the info of a computed style ----------------------------------------------------------------------------- */

static bool kw_is(const GTEXT_CSS_String * s, const char * lit) {
  const size_t n = strlen(lit);
  if (s->length != n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    unsigned char a = (unsigned char)s->data[i];
    if (a >= 'A' && a <= 'Z') {
      a = (unsigned char)(a + 32);
    }
    if (a != (unsigned char)lit[i]) {
      return false;
    }
  }
  return true;
}

GTEXT_CSS_Status gtext_css_container_info(const GTEXT_CSS_Computed_Style * style, GTEXT_CSS_Container_Info * info) {
  if (!style || !info) {
    return GTEXT_CSS_E_INVALID;
  }
  memset(info, 0, sizeof *info);
  info->style = style;
  const GTEXT_CSS_Value_Set * set = NULL;
  GTEXT_CSS_Status st = gtext_css_computed_get_by_name(style, "container-type", 14, &set, NULL);
  if (st != GTEXT_CSS_OK) {
    return st == GTEXT_CSS_E_INVALID ? GTEXT_CSS_E_UNSUPPORTED : st;
  }
  const GTEXT_CSS_Value * root = gtext_css_value_set_root(set);
  if (root) {
    const GTEXT_CSS_Value * items = root->kind == GTEXT_CSS_VAL_LIST ? root->children : root;
    const size_t n = root->kind == GTEXT_CSS_VAL_LIST ? root->child_count : 1;
    for (size_t i = 0; i < n; i++) {
      if (items[i].kind != GTEXT_CSS_VAL_KEYWORD) {
        continue;
      }
      if (kw_is(&items[i].text, "size")) {
        info->types |= GTEXT_CSS_CONTAINER_TYPE_SIZE;
      }
      else if (kw_is(&items[i].text, "inline-size")) {
        info->types |= GTEXT_CSS_CONTAINER_TYPE_INLINE_SIZE;
      }
      else if (kw_is(&items[i].text, "scroll-state")) {
        info->types |= GTEXT_CSS_CONTAINER_TYPE_SCROLL_STATE;
      }
      else if (kw_is(&items[i].text, "anchored")) {
        info->types |= GTEXT_CSS_CONTAINER_TYPE_ANCHORED;
      }
    }
  }
  set = NULL;
  st = gtext_css_computed_get_by_name(style, "container-name", 14, &set, NULL);
  if (st != GTEXT_CSS_OK) {
    return st == GTEXT_CSS_E_INVALID ? GTEXT_CSS_E_UNSUPPORTED : st;
  }
  root = gtext_css_value_set_root(set);
  if (root) {
    if (root->kind == GTEXT_CSS_VAL_LIST) {
      info->name_count = root->child_count;
    }
    else if (root->kind == GTEXT_CSS_VAL_CUSTOM_IDENT) {
      info->name_count = 1;
    }
  }
  return GTEXT_CSS_OK;
}

bool gtext_css_container_info_name(const GTEXT_CSS_Container_Info * info, size_t index, GTEXT_CSS_String * name) {
  if (!info || !info->style || index >= info->name_count) {
    return false;
  }
  const GTEXT_CSS_Value_Set * set = NULL;
  if (gtext_css_computed_get_by_name(info->style, "container-name", 14, &set, NULL) != GTEXT_CSS_OK) {
    return false;
  }
  const GTEXT_CSS_Value * root = gtext_css_value_set_root(set);
  if (!root) {
    return false;
  }
  const GTEXT_CSS_Value * v = root->kind == GTEXT_CSS_VAL_LIST ? &root->children[index] : root;
  if (name) {
    *name = v->text;
  }
  return true;
}

/* ---- the parsed list ---------------------------------------------------------------------------------------------- */

size_t gtext_css_container_query_count(const GTEXT_CSS_Container_Query * q) {
  return q ? q->in.root_count : 0;
}

const char * gtext_css_container_query_name(const GTEXT_CSS_Container_Query * q, size_t index, size_t * length) {
  if (!q || index >= q->in.root_count) {
    return NULL;
  }
  const mq_node * n = &q->in.nodes[q->in.roots[index]];
  if (length) {
    *length = n->text_length;
  }
  return n->text;
}

uint32_t gtext_css_container_query_kinds(const GTEXT_CSS_Container_Query * q, size_t index) {
  if (!q || index >= q->in.root_count) {
    return 0;
  }
  return q->in.nodes[q->in.roots[index]].bits;
}

/* ---- the call's tables -------------------------------------------------------------------------------------------- */

static size_t phash(const void * a, uint32_t b) {
  uint64_t h = (uint64_t)(uintptr_t)a * 0x9e3779b97f4a7c15ull ^ ((uint64_t)b + 1) * 0xc2b2ae3d27d4eb4full;
  h ^= h >> 31;
  h *= 0xff51afd7ed558ccdull;
  h ^= h >> 29;
  return (size_t)h;
}

static void free_request_names(struct GTEXT_CSS_Condition_Call_Internal * ci) {
  for (size_t i = 0; i < ci->nreq; i++) {
    gtext_allocator_free(&ci->alloc, ci->reqs[i].name);
    ci->reqs[i].name = NULL;
  }
}

void css_cq_call_free(struct GTEXT_CSS_Condition_Call_Internal * ci) {
  if (!ci) {
    return;
  }
  free_request_names(ci);
  gtext_allocator_free(&ci->alloc, ci->reqs);
  gtext_allocator_free(&ci->alloc, ci->cache);
  gtext_allocator_free(&ci->alloc, ci->results);
  gtext_allocator_free(&ci->alloc, (void *)ci->path);
  css_cq_style_free(ci);
  ci->reqs = NULL;
  ci->cache = NULL;
  ci->results = NULL;
  ci->path = NULL;
  ci->nreq = ci->capreq = ci->cache_used = ci->cache_cap = ci->results_used = ci->results_cap = ci->path_cap = 0;
}

void gtext_css_condition_call_reset(GTEXT_CSS_Condition_Call * call) {
  if (!call || !call->internal) {
    return;
  }
  struct GTEXT_CSS_Condition_Call_Internal * ci = call->internal;
  if (ci->cache) {
    memset(ci->cache, 0, ci->cache_cap * sizeof *ci->cache);
  }
  ci->cache_used = 0;
  if (ci->results) {
    memset(ci->results, 0, ci->results_cap * sizeof *ci->results);
  }
  ci->results_used = 0;
  free_request_names(ci);
  ci->nreq = 0;
  if (ci->memo) {
    memset(ci->memo, 0, ci->memo_n);
  }
  css_cq_style_reset(ci);
}

static GTEXT_CSS_Status cache_grow(struct GTEXT_CSS_Condition_Call_Internal * ci) {
  const size_t ncap = ci->cache_cap ? ci->cache_cap * 2 : 256;
  cq_cache_entry * n = gtext_allocator_calloc(&ci->alloc, ncap, sizeof *n);
  if (!n) {
    return GTEXT_CSS_E_OOM;
  }
  for (size_t i = 0; i < ci->cache_cap; i++) {
    const cq_cache_entry * e = &ci->cache[i];
    if (e->request) {
      size_t j = phash(e->element, e->request) & (ncap - 1);
      while (n[j].request) {
        j = (j + 1) & (ncap - 1);
      }
      n[j] = *e;
    }
  }
  gtext_allocator_free(&ci->alloc, ci->cache);
  ci->cache = n;
  ci->cache_cap = ncap;
  return GTEXT_CSS_OK;
}

static const cq_cache_entry * cache_get(const struct GTEXT_CSS_Condition_Call_Internal * ci, const void * element, uint32_t request) {
  if (!ci->cache_cap) {
    return NULL;
  }
  for (size_t j = phash(element, request) & (ci->cache_cap - 1);; j = (j + 1) & (ci->cache_cap - 1)) {
    const cq_cache_entry * e = &ci->cache[j];
    if (!e->request) {
      return NULL;
    }
    if (e->element == element && e->request == request) {
      return e;
    }
  }
}

static GTEXT_CSS_Status cache_put(struct GTEXT_CSS_Condition_Call_Internal * ci, const void * element, uint32_t request, const void * container) {
  if ((ci->cache_used + 1) * 2 > ci->cache_cap) {
    const GTEXT_CSS_Status st = cache_grow(ci);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
  }
  size_t j = phash(element, request) & (ci->cache_cap - 1);
  while (ci->cache[j].request) {
    if (ci->cache[j].element == element && ci->cache[j].request == request) {
      ci->cache[j].container = container;
      return GTEXT_CSS_OK;
    }
    j = (j + 1) & (ci->cache_cap - 1);
  }
  ci->cache[j] = (cq_cache_entry){element, request, container};
  ci->cache_used++;
  return GTEXT_CSS_OK;
}

static size_t rhash(const void * container, size_t serial, uint32_t root) {
  return phash(container, (uint32_t)((uint64_t)serial * 0x9e3779b1u) ^ (uint32_t)((uint64_t)serial >> 32) ^ (root * 0x85ebca6bu));
}

static GTEXT_CSS_Status results_grow(struct GTEXT_CSS_Condition_Call_Internal * ci) {
  const size_t ncap = ci->results_cap ? ci->results_cap * 2 : 256;
  cq_result_entry * n = gtext_allocator_calloc(&ci->alloc, ncap, sizeof *n);
  if (!n) {
    return GTEXT_CSS_E_OOM;
  }
  for (size_t i = 0; i < ci->results_cap; i++) {
    const cq_result_entry * e = &ci->results[i];
    if (e->serial) {
      size_t j = rhash(e->container, e->serial, e->root) & (ncap - 1);
      while (n[j].serial) {
        j = (j + 1) & (ncap - 1);
      }
      n[j] = *e;
    }
  }
  gtext_allocator_free(&ci->alloc, ci->results);
  ci->results = n;
  ci->results_cap = ncap;
  return GTEXT_CSS_OK;
}

static bool results_get(const struct GTEXT_CSS_Condition_Call_Internal * ci, const void * container, size_t serial, uint32_t root, uint8_t * result) {
  if (!ci->results_cap) {
    return false;
  }
  for (size_t j = rhash(container, serial, root) & (ci->results_cap - 1);; j = (j + 1) & (ci->results_cap - 1)) {
    const cq_result_entry * e = &ci->results[j];
    if (!e->serial) {
      return false;
    }
    if (e->container == container && e->serial == serial && e->root == root) {
      *result = e->result;
      return true;
    }
  }
}

static GTEXT_CSS_Status results_put(struct GTEXT_CSS_Condition_Call_Internal * ci, const void * container, size_t serial, uint32_t root, uint8_t result) {
  if ((ci->results_used + 1) * 2 > ci->results_cap) {
    const GTEXT_CSS_Status st = results_grow(ci);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
  }
  size_t j = rhash(container, serial, root) & (ci->results_cap - 1);
  while (ci->results[j].serial) {
    j = (j + 1) & (ci->results_cap - 1);
  }
  ci->results[j] = (cq_result_entry){container, serial, root, result};
  ci->results_used++;
  return GTEXT_CSS_OK;
}

/// The id (plus one) of a request, made if it is new.
static GTEXT_CSS_Status request_id(const GTEXT_CSS_Conditions * c, GTEXT_CSS_Condition_Call * call, const char * name, size_t name_len, uint32_t mask, uint32_t * out) {
  struct GTEXT_CSS_Condition_Call_Internal * ci = call->internal;
  for (size_t i = 0; i < ci->nreq; i++) {
    const cq_request * r = &ci->reqs[i];
    if (r->mask == mask && r->name_len == name_len && (name_len == 0 || memcmp(r->name, name, name_len) == 0)) {
      *out = (uint32_t)i + 1;
      return GTEXT_CSS_OK;
    }
  }
  if (ci->nreq >= c->max_requests) {
    return GTEXT_CSS_E_LIMIT_CONTAINER;
  }
  if (ci->nreq == ci->capreq) {
    const size_t nc = ci->capreq ? ci->capreq * 2 : 16;
    cq_request * n = gtext_allocator_realloc(&ci->alloc, ci->reqs, nc * sizeof *n);
    if (!n) {
      return GTEXT_CSS_E_OOM;
    }
    ci->reqs = n;
    ci->capreq = nc;
  }
  char * own = NULL;
  if (name_len) {
    own = gtext_allocator_malloc(&ci->alloc, name_len);
    if (!own) {
      return GTEXT_CSS_E_OOM;
    }
    memcpy(own, name, name_len);
  }
  ci->reqs[ci->nreq++] = (cq_request){own, name_len, mask};
  call->container_requests = ci->nreq;
  *out = (uint32_t)ci->nreq;
  return GTEXT_CSS_OK;
}

/* ---- the query container ---------------------------------------------------------------------------------------- */

static const GTEXT_CSS_Computed_Style * style_of(const GTEXT_CSS_Conditions * c, GTEXT_CSS_Element_Handle e) {
  return c->container.computed_style ? c->container.computed_style(c->container.user, e) : NULL;
}

/// Whether an element is a query container for a request. A style that cannot be read is not a container.
static GTEXT_CSS_Status qualifies(const GTEXT_CSS_Conditions * c, GTEXT_CSS_Element_Handle e, const cq_request * r, bool * out) {
  *out = false;
  const GTEXT_CSS_Computed_Style * style = style_of(c, e);
  if (!style) {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Container_Info info;
  const GTEXT_CSS_Status st = gtext_css_container_info(style, &info);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (r->mask & MQC_UNKNOWN) {
    return GTEXT_CSS_OK;
  }
  if ((r->mask & MQC_SIZE) && !(info.types & (GTEXT_CSS_CONTAINER_TYPE_SIZE | GTEXT_CSS_CONTAINER_TYPE_INLINE_SIZE))) {
    return GTEXT_CSS_OK;
  }
  if ((r->mask & MQC_SIZE) && !(info.types & GTEXT_CSS_CONTAINER_TYPE_SIZE)) {
    /* an inline-size container supports the inline axis only, and the container is selected for the whole condition */
    const GTEXT_CSS_Flow flow = gtext_css_computed_flow(style);
    const bool inline_horizontal = gtext_css_flow_axis(&flow, true) == GTEXT_CSS_AXIS_HORIZONTAL;
    if ((r->mask & (MQC_AX_B | MQC_AX_BOTH)) || ((r->mask & MQC_AX_W) && !inline_horizontal) || ((r->mask & MQC_AX_H) && inline_horizontal)) {
      return GTEXT_CSS_OK;
    }
  }
  if ((r->mask & MQC_SCROLL) && !(info.types & GTEXT_CSS_CONTAINER_TYPE_SCROLL_STATE)) {
    return GTEXT_CSS_OK;
  }
  if ((r->mask & MQC_ANCHORED) && !(info.types & GTEXT_CSS_CONTAINER_TYPE_ANCHORED)) {
    return GTEXT_CSS_OK;
  }
  if (r->name) {
    bool found = false;
    for (size_t i = 0; i < info.name_count && !found; i++) {
      GTEXT_CSS_String nm;
      if (gtext_css_container_info_name(&info, i, &nm) && nm.length == r->name_len && memcmp(nm.data, r->name, r->name_len) == 0) {
        found = true;
      }
    }
    if (!found) {
      return GTEXT_CSS_OK;
    }
  }
  *out = true;
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status parent_of(const GTEXT_CSS_Element_Interface * iface, GTEXT_CSS_Element_Handle e, GTEXT_CSS_Element_Handle * out) {
  return gtext_css_element_get_inheritance_parent(iface, e, out, NULL);
}

/// The nearest inclusive ancestor of `start` that is a query container for the request (NULL: none). Memoized: every element on the way is given the answer.
GTEXT_CSS_Status css_cq_resolve(const GTEXT_CSS_Conditions * c, GTEXT_CSS_Condition_Call * call, const GTEXT_CSS_Element_Interface * iface, GTEXT_CSS_Element_Handle start,
    bool from_element, const char * name, size_t name_len, uint32_t mask, GTEXT_CSS_Element_Handle * out) {
  struct GTEXT_CSS_Condition_Call_Internal * ci = call->internal;
  *out = NULL;
  uint32_t req = 0;
  GTEXT_CSS_Status st = request_id(c, call, name_len ? name : NULL, name_len, mask, &req);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!iface) {
    call->detail = "element_interface";
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  if ((mask & MQC_STYLE) && !c->container.computed_style) {
    call->detail = "container.computed_style";
    return GTEXT_CSS_E_UNSUPPORTED;
  }
  GTEXT_CSS_Element_Handle e = start;
  if (!from_element) {
    st = parent_of(iface, start, &e);
    if (st != GTEXT_CSS_OK) {
      call->detail = "parent";
      return st;
    }
  }
  const cq_request * r = &ci->reqs[req - 1];
  size_t np = 0;
  const void * result = NULL;
  const GTEXT_CSS_Element_Handle first = e;
  while (e) {
    const cq_cache_entry * hit = cache_get(ci, e, req);
    if (hit) {
      call->container_hits++;
      result = hit->container;
      break;
    }
    call->container_steps++;
    bool q = false;
    st = qualifies(c, e, r, &q);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    if (q) {
      result = e;
      st = cache_put(ci, e, req, e);
      if (st != GTEXT_CSS_OK) {
        return st;
      }
      break;
    }
    if (np == ci->path_cap) {
      const size_t nc = ci->path_cap ? ci->path_cap * 2 : 64;
      const void ** n = gtext_allocator_realloc(&ci->alloc, (void *)ci->path, nc * sizeof *n);
      if (!n) {
        return GTEXT_CSS_E_OOM;
      }
      ci->path = n;
      ci->path_cap = nc;
    }
    ci->path[np++] = e;
    GTEXT_CSS_Element_Handle up = NULL;
    st = parent_of(iface, e, &up);
    if (st != GTEXT_CSS_OK) {
      call->detail = "parent";
      return st;
    }
    e = up;
  }
  for (size_t i = 0; i < np; i++) {
    st = cache_put(ci, ci->path[i], req, result);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
  }
  (void)first;
  *out = result;
  return GTEXT_CSS_OK;
}

/* ---- the evaluation context --------------------------------------------------------------------------------------- */

static double font_px(const GTEXT_CSS_Computed_Style * style, double fallback) {
  const GTEXT_CSS_Value_Set * set = NULL;
  if (gtext_css_computed_get_by_name(style, "font-size", 9, &set, NULL) != GTEXT_CSS_OK) {
    return fallback;
  }
  const GTEXT_CSS_Value * v = gtext_css_value_set_root(set);
  if (v && v->kind == GTEXT_CSS_VAL_DIMENSION && v->number > 0) {
    return v->number; /* a computed length is in px */
  }
  return fallback;
}

static void cq_init(css_cq_ctx * cq, const GTEXT_CSS_Conditions * c, GTEXT_CSS_Condition_Call * call, GTEXT_CSS_Element_Handle container, const GTEXT_CSS_Element_Interface * iface) {
  memset(cq, 0, sizeof *cq);
  cq->c = c;
  cq->call = call;
  cq->iface = iface;
  cq->container = container;
  cq->style = style_of(c, container);
  GTEXT_CSS_Container_Info info;
  memset(&info, 0, sizeof info);
  if (cq->style && gtext_css_container_info(cq->style, &info) == GTEXT_CSS_OK) {
    cq->types = info.types;
  }
  if (cq->style) {
    cq->flow = gtext_css_computed_flow(cq->style);
  }
  cq->inline_horizontal = gtext_css_flow_axis(&cq->flow, true) == GTEXT_CSS_AXIS_HORIZONTAL;
}

/// A `cq*` unit in a feature value is relative to the container's own query container (its ancestors).
static bool cq_size_cb(void * user, GTEXT_CSS_Container_Axis axis, double * px) {
  css_cq_ctx * cq = user;
  return gtext_css_container_environment_size(cq->c, cq->call, cq->container, axis, px);
}

GTEXT_CSS_Tristate css_cq_eval_size(css_cq_ctx * cq, const struct GTEXT_CSS_Media_List_Internal * in, const mq_node * n, GTEXT_CSS_Status * status, const char ** detail) {
  const int row = n->feature;
  const bool logical = (n->flags & MQF_LOGICAL) != 0;
  /* the axes the feature asks about: a physical axis (width, height), or the container's inline or block axis; aspect-ratio and orientation need both */
  const bool both = row == CSS_MQF_ASPECT_RATIO || row == CSS_MQF_ORIENTATION;
  bool horizontal = row == CSS_MQF_WIDTH;
  if (logical) {
    horizontal = (row == CSS_MQF_WIDTH) == cq->inline_horizontal; /* WIDTH is inline-size, HEIGHT block-size */
  }
  bool contained = (cq->types & GTEXT_CSS_CONTAINER_TYPE_SIZE) != 0;
  if (!contained && !both && (cq->types & GTEXT_CSS_CONTAINER_TYPE_INLINE_SIZE)) {
    contained = horizontal == cq->inline_horizontal; /* only the inline axis is contained */
  }
  if (!contained) {
    return GTEXT_CSS_TRI_UNKNOWN; /* "the query container does not support container size queries on the relevant axes": unknown */
  }
  if (!cq->c->container.content_box) {
    *status = GTEXT_CSS_E_UNSUPPORTED;
    *detail = "container.size";
    return GTEXT_CSS_TRI_UNKNOWN;
  }
  if (!cq->size_loaded) {
    cq->size_loaded = true;
    cq->size_ok = cq->c->container.content_box(cq->c->container.user, cq->container, &cq->size);
  }
  if (!cq->size_ok) {
    return GTEXT_CSS_TRI_UNKNOWN; /* no principal box */
  }
  mq_container_view view;
  memset(&view, 0, sizeof view);
  view.cq_size = cq_size_cb;
  view.cq_user = cq;
  view.width = cq->size.width;
  view.height = cq->size.height;
  view.inline_horizontal = cq->inline_horizontal;
  view.font_size = cq->style ? font_px(cq->style, cq->c->env.default_font_size) : cq->c->env.default_font_size;
  return css_mq_eval_feature_view(&cq->c->env, &view, in, n, status, detail);
}

/* ---- scroll-state and anchored ------------------------------------------------------------------------------------ */

/// The physical edges a scroll-state keyword names, by the container's flow. Returns false for `none`.
static bool keyword_edges(const GTEXT_CSS_Flow * flow, const char * kw, uint32_t * edges, bool * axis) {
  *axis = false;
  if (strcmp(kw, "none") == 0) {
    return false;
  }
  if (strcmp(kw, "top") == 0) {
    *edges = GTEXT_CSS_EDGE_TOP;
  }
  else if (strcmp(kw, "right") == 0) {
    *edges = GTEXT_CSS_EDGE_RIGHT;
  }
  else if (strcmp(kw, "bottom") == 0) {
    *edges = GTEXT_CSS_EDGE_BOTTOM;
  }
  else if (strcmp(kw, "left") == 0) {
    *edges = GTEXT_CSS_EDGE_LEFT;
  }
  else if (strcmp(kw, "block-start") == 0) {
    *edges = 1u << gtext_css_flow_block_side(flow, false);
  }
  else if (strcmp(kw, "block-end") == 0) {
    *edges = 1u << gtext_css_flow_block_side(flow, true);
  }
  else if (strcmp(kw, "inline-start") == 0) {
    *edges = 1u << gtext_css_flow_inline_side(flow, false);
  }
  else if (strcmp(kw, "inline-end") == 0) {
    *edges = 1u << gtext_css_flow_inline_side(flow, true);
  }
  else if (strcmp(kw, "x") == 0) {
    *edges = GTEXT_CSS_EDGE_LEFT | GTEXT_CSS_EDGE_RIGHT;
    *axis = true;
  }
  else if (strcmp(kw, "y") == 0) {
    *edges = GTEXT_CSS_EDGE_TOP | GTEXT_CSS_EDGE_BOTTOM;
    *axis = true;
  }
  else if (strcmp(kw, "block") == 0) {
    *edges = (1u << gtext_css_flow_block_side(flow, false)) | (1u << gtext_css_flow_block_side(flow, true));
    *axis = true;
  }
  else if (strcmp(kw, "inline") == 0) {
    *edges = (1u << gtext_css_flow_inline_side(flow, false)) | (1u << gtext_css_flow_inline_side(flow, true));
    *axis = true;
  }
  else {
    *edges = 0;
  }
  return true;
}

static GTEXT_CSS_Tristate eval_scroll(css_cq_ctx * cq, const mq_node * n, GTEXT_CSS_Status * status, const char ** detail) {
  if (!cq->c->container.scroll_state) {
    *status = GTEXT_CSS_E_UNSUPPORTED;
    *detail = "container.scroll_state";
    return GTEXT_CSS_TRI_UNKNOWN;
  }
  if (!cq->scroll_loaded) {
    cq->scroll_loaded = true;
    memset(&cq->scroll, 0, sizeof cq->scroll);
    cq->scroll_ok = cq->c->container.scroll_state(cq->c->container.user, cq->container, &cq->scroll);
  }
  if (!cq->scroll_ok) {
    return GTEXT_CSS_TRI_UNKNOWN;
  }
  const GTEXT_CSS_Container_Scroll_State * st = &cq->scroll;
  const int feature = n->feature;
  const bool boolean = n->form == MQFORM_BOOLEAN;
  const char * kw = boolean ? "none" : css_cq_scroll_keywords[feature][n->sub_kw];
  if (feature == 1) {
    /* snapped: none is "not a snap target"; x and y the axes; block and inline by the container's own flow (the scroll container's is not known here) */
    uint32_t want = 0;
    if (strcmp(kw, "x") == 0) {
      want = GTEXT_CSS_SNAP_AXIS_X;
    }
    else if (strcmp(kw, "y") == 0) {
      want = GTEXT_CSS_SNAP_AXIS_Y;
    }
    else if (strcmp(kw, "both") == 0) {
      want = GTEXT_CSS_SNAP_AXIS_X | GTEXT_CSS_SNAP_AXIS_Y;
    }
    else if (strcmp(kw, "block") == 0 || strcmp(kw, "inline") == 0) {
      want = gtext_css_flow_axis(&cq->flow, strcmp(kw, "inline") == 0) == GTEXT_CSS_AXIS_HORIZONTAL ? GTEXT_CSS_SNAP_AXIS_X : GTEXT_CSS_SNAP_AXIS_Y;
    }
    if (boolean) {
      return st->snap_target ? GTEXT_CSS_TRI_TRUE : GTEXT_CSS_TRI_FALSE;
    }
    if (strcmp(kw, "none") == 0) {
      return st->snap_target ? GTEXT_CSS_TRI_FALSE : GTEXT_CSS_TRI_TRUE;
    }
    return st->snap_target && (st->snapped & want) == want ? GTEXT_CSS_TRI_TRUE : GTEXT_CSS_TRI_FALSE;
  }
  const bool capable = feature == 0 ? st->sticky : st->scroll_container;
  const uint32_t mask = feature == 0 ? st->stuck : (feature == 2 ? st->scrollable : st->scrolled);
  if (boolean) {
    return capable && mask ? GTEXT_CSS_TRI_TRUE : GTEXT_CSS_TRI_FALSE; /* "true for any value but none" */
  }
  uint32_t edges = 0;
  bool axis = false;
  if (!keyword_edges(&cq->flow, kw, &edges, &axis)) {
    return capable && mask == 0 ? GTEXT_CSS_TRI_TRUE : GTEXT_CSS_TRI_FALSE; /* "None of the values match if the container is not [sticky / a scroll container]" */
  }
  (void)axis;
  return capable && (mask & edges) ? GTEXT_CSS_TRI_TRUE : GTEXT_CSS_TRI_FALSE;
}

static GTEXT_CSS_Tristate eval_anchored(css_cq_ctx * cq, const mq_node * n, GTEXT_CSS_Status * status, const char ** detail) {
  if (!cq->c->container.anchored_fallback) {
    *status = GTEXT_CSS_E_UNSUPPORTED;
    *detail = "container.anchored_fallback";
    return GTEXT_CSS_TRI_UNKNOWN;
  }
  const bool boolean = n->form == MQFORM_BOOLEAN;
  GTEXT_CSS_String v = {boolean ? "none" : n->value, boolean ? 4 : n->value_length};
  bool matches = false;
  if (!cq->c->container.anchored_fallback(cq->c->container.user, cq->container, &v, &matches)) {
    return GTEXT_CSS_TRI_UNKNOWN;
  }
  return (boolean ? !matches : matches) ? GTEXT_CSS_TRI_TRUE : GTEXT_CSS_TRI_FALSE;
}

GTEXT_CSS_Tristate css_cq_eval_sub(css_cq_ctx * cq, const struct GTEXT_CSS_Media_List_Internal * in, const mq_node * n, GTEXT_CSS_Status * status, const char ** detail) {
  switch (n->type_id) {
  case MQ_SUB_SCROLL: return eval_scroll(cq, n, status, detail);
  case MQ_SUB_ANCHORED: return eval_anchored(cq, n, status, detail);
  case MQ_SUB_STYLE: return css_cq_eval_style(cq, in, n, status, detail);
  default: return GTEXT_CSS_TRI_UNKNOWN;
  }
}

/* ---- a root against an element -------------------------------------------------------------------------------------- */

/// A container condition (one root of a list) for an element: the three-valued answer.
GTEXT_CSS_Status css_cq_eval_root(const GTEXT_CSS_Conditions * c, GTEXT_CSS_Condition_Call * call, const GTEXT_CSS_Element_Interface * iface,
    const struct GTEXT_CSS_Media_List_Internal * list, uint32_t root, GTEXT_CSS_Element_Handle element, bool from_element, GTEXT_CSS_Tristate * out) {
  const mq_node * q = &list->nodes[root];
  *out = GTEXT_CSS_TRI_FALSE;
  if ((q->flags & MQF_REPLACED) || (q->bits & MQC_UNKNOWN)) {
    return GTEXT_CSS_OK; /* an invalid rule; a feature that is unknown or unsupported selects no container */
  }
  GTEXT_CSS_Element_Handle container = NULL;
  GTEXT_CSS_Status st = css_cq_resolve(c, call, iface, element, from_element, q->text, q->text_length, q->bits, &container);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!container) {
    *out = GTEXT_CSS_TRI_UNKNOWN; /* "If no ancestor is an eligible query container, then the container query is unknown for that element." */
    return GTEXT_CSS_OK;
  }
  struct GTEXT_CSS_Condition_Call_Internal * ci = call->internal;
  uint8_t memo;
  if (results_get(ci, container, list->serial, root, &memo)) {
    *out = (GTEXT_CSS_Tristate)memo;
    return GTEXT_CSS_OK;
  }
  css_cq_ctx cq;
  cq_init(&cq, c, call, container, iface);
  GTEXT_CSS_Tristate t = GTEXT_CSS_TRI_UNKNOWN;
  call->container_evaluations++;
  st = css_conditions_eval_query_cq(c, call, list, root, &cq, &t);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  st = results_put(ci, container, list->serial, root, (uint8_t)t);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  *out = t;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_conditions_evaluate_container(const GTEXT_CSS_Conditions * c, const GTEXT_CSS_Container_Query * query, GTEXT_CSS_Element_Handle element,
    bool from_element, GTEXT_CSS_Condition_Call * call, bool * matches) {
  if (!c || !query || !element || !matches) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Condition_Call local;
  if (!call) {
    gtext_css_condition_call_init(&local);
    call = &local;
  }
  GTEXT_CSS_Status st = css_conditions_prepare_call(c, call);
  bool any = false;
  for (size_t i = 0; st == GTEXT_CSS_OK && i < query->in.root_count && !any; i++) {
    GTEXT_CSS_Tristate t = GTEXT_CSS_TRI_FALSE;
    st = css_cq_eval_root(c, call, c->iface, &query->in, query->in.roots[i], element, from_element, &t);
    any = t == GTEXT_CSS_TRI_TRUE;
  }
  if (st == GTEXT_CSS_OK) {
    *matches = any;
  }
  if (call == &local) {
    gtext_css_condition_call_free(&local);
  }
  return st;
}

bool gtext_css_container_environment_size(const GTEXT_CSS_Conditions * c, GTEXT_CSS_Condition_Call * call, GTEXT_CSS_Element_Handle element, GTEXT_CSS_Container_Axis axis,
    double * px) {
  if (!c || !element || !px || !c->container.content_box || !c->iface) {
    return false;
  }
  GTEXT_CSS_Condition_Call local;
  if (!call) {
    gtext_css_condition_call_init(&local);
    call = &local;
  }
  bool ok = false;
  if (css_conditions_prepare_call(c, call) == GTEXT_CSS_OK) {
    /* the nearest ancestor that accepts container size queries on the axis: a `size` container for any, an `inline-size` one for its inline axis */
    GTEXT_CSS_Element_Handle cur = element;
    for (size_t guard = 0; guard < ((size_t)1 << 24); guard++) {
      GTEXT_CSS_Element_Handle a = NULL;
      if (css_cq_resolve(c, call, c->iface, cur, false, NULL, 0, MQC_SIZE, &a) != GTEXT_CSS_OK || !a) {
        break;
      }
      css_cq_ctx cq;
      cq_init(&cq, c, call, a, c->iface);
      const bool inline_axis = axis == GTEXT_CSS_CONTAINER_INLINE || axis == GTEXT_CSS_CONTAINER_BLOCK;
      const bool want_inline = axis == GTEXT_CSS_CONTAINER_INLINE;
      const bool horizontal = inline_axis ? (want_inline == cq.inline_horizontal) : axis == GTEXT_CSS_CONTAINER_WIDTH;
      const bool contained = (cq.types & GTEXT_CSS_CONTAINER_TYPE_SIZE) || horizontal == cq.inline_horizontal;
      if (!contained) {
        cur = a; /* an inline-size container does not accept the other axis: look further up */
        continue;
      }
      GTEXT_CSS_Container_Metrics m;
      if (c->container.content_box(c->container.user, a, &m)) {
        *px = horizontal ? m.width : m.height;
        ok = true;
      }
      break;
    }
  }
  if (call == &local) {
    gtext_css_condition_call_free(&local);
  }
  return ok;
}
