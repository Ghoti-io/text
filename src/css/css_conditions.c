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
 * The `@custom-media` definitions and the conditions object: creation, sealing (the winning definition of each name and the cycles), the entry points
 * that evaluate a media query list, the dependencies, and the bridge to `if()`.
 */

#include <ghoti.io/text/css/css_condition.h>
#include <ghoti.io/text/css/css_media.h>
#include <ghoti.io/text/css/css_parse.h>

#include <stdlib.h>
#include <string.h>

#include "css_condition_internal.h"

/* ---- definitions -------------------------------------------------------- */

GTEXT_CSS_Status gtext_css_condition_defs_new(const GTEXT_Allocator * allocator, GTEXT_CSS_Condition_Defs ** out) {
  if (!out) {
    return GTEXT_CSS_E_INVALID;
  }
  const GTEXT_Allocator * al = allocator ? allocator : gtext_allocator_default();
  struct GTEXT_CSS_Condition_Defs * d = gtext_allocator_calloc(al, 1, sizeof *d);
  if (!d) {
    return GTEXT_CSS_E_OOM;
  }
  d->alloc = *al;
  gcu_atomic_int_init(&d->refs, 1);
  *out = d;
  return GTEXT_CSS_OK;
}

static void defs_release(struct GTEXT_CSS_Condition_Defs * d) {
  GTEXT_Allocator a = d->alloc;
  for (size_t i = 0; i < d->count; i++) {
    gtext_allocator_free(&a, d->items[i].name);
    gtext_css_media_list_free(d->items[i].list);
  }
  gtext_allocator_free(&a, d->items);
  gtext_allocator_free(&a, d->winner);
  gtext_allocator_free(&a, d->cyclic);
  gtext_allocator_free(&a, d->cycles);
  gtext_allocator_free(&a, d);
}

static void defs_unref(struct GTEXT_CSS_Condition_Defs * d) {
  if (d && gcu_atomic_int_fetch_sub(&d->refs, 1) == 1) {
    defs_release(d);
  }
}

void gtext_css_condition_defs_free(GTEXT_CSS_Condition_Defs * defs) {
  defs_unref(defs);
}

size_t gtext_css_condition_defs_count(const GTEXT_CSS_Condition_Defs * defs) {
  return defs ? defs->count : 0;
}

static GTEXT_CSS_Status defs_push(struct GTEXT_CSS_Condition_Defs * d, const char * name, size_t nlen, uint8_t kind, GTEXT_CSS_Media_List * list) {
  if (!css_mq_grow(&d->alloc, (void **)&d->items, &d->cap, d->count, 1, sizeof(css_def))) {
    return GTEXT_CSS_E_OOM;
  }
  char * copy = gtext_allocator_malloc(&d->alloc, nlen + 1);
  if (!copy) {
    return GTEXT_CSS_E_OOM;
  }
  memcpy(copy, name, nlen);
  copy[nlen] = 0;
  css_def * it = &d->items[d->count];
  it->name = copy;
  it->name_length = nlen;
  it->kind = kind;
  it->list = list;
  it->order = (uint32_t)d->count;
  d->count++;
  return GTEXT_CSS_OK;
}

static bool ident_is(const GTEXT_CSS_String * w, const char * lit) {
  size_t n = strlen(lit);
  if (w->length != n) {
    return false;
  }
  for (size_t k = 0; k < n; k++) {
    if (css_mq_lower((unsigned char)w->data[k]) != (unsigned char)lit[k]) {
      return false;
    }
  }
  return true;
}

GTEXT_CSS_Status gtext_css_condition_defs_add_custom_media(GTEXT_CSS_Condition_Defs * defs, const GTEXT_CSS_Component_Value * values, size_t count,
    const GTEXT_CSS_Media_Options * options, bool * added) {
  if (added) {
    *added = false;
  }
  if (!defs || (count && !values)) {
    return GTEXT_CSS_E_INVALID;
  }
  if (defs->sealed) {
    return GTEXT_CSS_E_INVALID;
  }
  size_t i = 0;
  while (i < count && values[i].kind == GTEXT_CSS_VALUE_TOKEN && values[i].type == GTEXT_CSS_TOKEN_WHITESPACE) {
    i++;
  }
  if (i >= count || values[i].kind != GTEXT_CSS_VALUE_TOKEN || values[i].type != GTEXT_CSS_TOKEN_IDENT || values[i].value.length < 3 ||
      values[i].value.data[0] != '-' || values[i].value.data[1] != '-') {
    return GTEXT_CSS_OK;
  }
  const GTEXT_CSS_Component_Value * name = &values[i++];
  while (i < count && values[i].kind == GTEXT_CSS_VALUE_TOKEN && values[i].type == GTEXT_CSS_TOKEN_WHITESPACE) {
    i++;
  }
  size_t end = count;
  while (end > i && values[end - 1].kind == GTEXT_CSS_VALUE_TOKEN && values[end - 1].type == GTEXT_CSS_TOKEN_WHITESPACE) {
    end--;
  }
  if (i >= end) {
    return GTEXT_CSS_OK;
  }
  if (end - i == 1 && values[i].kind == GTEXT_CSS_VALUE_TOKEN && values[i].type == GTEXT_CSS_TOKEN_IDENT) {
    const GTEXT_CSS_String * w = &values[i].value;
    bool is_true = ident_is(w, "true");
    bool is_false = ident_is(w, "false");
    if (is_true || is_false) {
      GTEXT_CSS_Status st = defs_push(defs, name->value.data, name->value.length, is_true ? CSS_DEF_TRUE : CSS_DEF_FALSE, NULL);
      if (st == GTEXT_CSS_OK && added) {
        *added = true;
      }
      return st;
    }
  }
  GTEXT_CSS_Media_Options mo = options ? *options : gtext_css_media_options_default();
  if (!mo.allocator) {
    mo.allocator = &defs->alloc;
  }
  GTEXT_CSS_Media_List * list = NULL;
  GTEXT_CSS_Media_Result res;
  GTEXT_CSS_Status st = gtext_css_media_list_parse_values(values + i, end - i, &mo, &list, &res);
  gtext_css_media_result_free(&res);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  st = defs_push(defs, name->value.data, name->value.length, CSS_DEF_LIST, list);
  if (st != GTEXT_CSS_OK) {
    gtext_css_media_list_free(list);
    return st;
  }
  if (added) {
    *added = true;
  }
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_condition_defs_add_custom_media_text(GTEXT_CSS_Condition_Defs * defs, const char * text, size_t length,
    const GTEXT_CSS_Media_Options * options, bool * added) {
  if (added) {
    *added = false;
  }
  if (!defs || (length && !text)) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Parse_Options po;
  memset(&po, 0, sizeof po);
  if (options) {
    po = options->parse;
  }
  if (!po.allocator) {
    po.allocator = &defs->alloc;
  }
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Result pr;
  memset(&pr, 0, sizeof pr);
  GTEXT_CSS_Status st = gtext_css_parse_component_values(text ? text : "", length, &po, &tree, &pr);
  gtext_css_result_free(&pr);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  st = gtext_css_condition_defs_add_custom_media(defs, tree->values, tree->value_count, options, added);
  gtext_css_tree_free(tree);
  return st;
}

/* ---- sealing -------------------------------------------------------------- */

static int name_cmp(const char * a, size_t an, const char * b, size_t bn) {
  size_t n = an < bn ? an : bn;
  int c = n ? memcmp(a, b, n) : 0;
  if (c) {
    return c;
  }
  return an < bn ? -1 : (an > bn ? 1 : 0);
}

static const struct GTEXT_CSS_Condition_Defs * sort_ctx;

static int winner_cmp(const void * x, const void * y) {
  const css_def * a = &sort_ctx->items[*(const uint32_t *)x];
  const css_def * b = &sort_ctx->items[*(const uint32_t *)y];
  int c = name_cmp(a->name, a->name_length, b->name, b->name_length);
  if (c) {
    return c;
  }
  return a->order < b->order ? -1 : (a->order > b->order ? 1 : 0);
}

int css_defs_find(const struct GTEXT_CSS_Condition_Defs * d, const char * name, size_t length) {
  size_t lo = 0, hi = d->winner_count;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    const css_def * it = &d->items[d->winner[mid]];
    int c = name_cmp(name, length, it->name, it->name_length);
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

bool css_defs_is_cyclic(const struct GTEXT_CSS_Condition_Defs * d, size_t w) {
  return d->cyclic[w] != 0;
}

int css_defs_kind(const struct GTEXT_CSS_Condition_Defs * d, size_t w) {
  return d->items[d->winner[w]].kind;
}

const struct GTEXT_CSS_Media_List_Internal * css_defs_list(const struct GTEXT_CSS_Condition_Defs * d, size_t w) {
  return &d->items[d->winner[w]].list->in;
}

/// Mark the winners that lie on a cycle of references (Tarjan's algorithm, with its own stacks).
static GTEXT_CSS_Status find_cycles(struct GTEXT_CSS_Condition_Defs * d) {
  size_t m = d->winner_count;
  GTEXT_Allocator * a = &d->alloc;
  d->cyclic = gtext_allocator_calloc(a, m ? m : 1, 1);
  if (!d->cyclic) {
    return GTEXT_CSS_E_OOM;
  }
  if (!m) {
    return GTEXT_CSS_OK;
  }
  size_t * start = gtext_allocator_calloc(a, m + 1, sizeof(size_t));
  if (!start) {
    return GTEXT_CSS_E_OOM;
  }
  uint32_t * edges = NULL;
  size_t ecap = 0, ecount = 0;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  for (size_t w = 0; w < m && st == GTEXT_CSS_OK; w++) {
    start[w] = ecount;
    const css_def * it = &d->items[d->winner[w]];
    if (it->kind != CSS_DEF_LIST) {
      continue;
    }
    const struct GTEXT_CSS_Media_List_Internal * in = &it->list->in;
    for (size_t k = 0; k < in->node_count; k++) {
      if (in->nodes[k].kind != MQN_CUSTOM) {
        continue;
      }
      int t = css_defs_find(d, in->nodes[k].text, in->nodes[k].text_length);
      if (t < 0) {
        continue;
      }
      if (!css_mq_grow(a, (void **)&edges, &ecap, ecount, 1, sizeof(uint32_t))) {
        st = GTEXT_CSS_E_OOM;
        break;
      }
      edges[ecount++] = (uint32_t)t;
    }
  }
  start[m] = ecount;
  int32_t * index = NULL;
  int32_t * low = NULL;
  uint32_t * stk = NULL;
  uint32_t * cs = NULL;
  size_t * cpos = NULL;
  uint8_t * on = NULL;
  if (st == GTEXT_CSS_OK) {
    index = gtext_allocator_malloc(a, m * sizeof(int32_t));
    low = gtext_allocator_malloc(a, m * sizeof(int32_t));
    stk = gtext_allocator_malloc(a, m * sizeof(uint32_t));
    cs = gtext_allocator_malloc(a, m * sizeof(uint32_t));
    cpos = gtext_allocator_malloc(a, m * sizeof(size_t));
    on = gtext_allocator_calloc(a, m, 1);
    if (!index || !low || !stk || !cs || !cpos || !on) {
      st = GTEXT_CSS_E_OOM;
    }
  }
  if (st == GTEXT_CSS_OK) {
    for (size_t i = 0; i < m; i++) {
      index[i] = -1;
    }
    int32_t counter = 0;
    size_t sp = 0, csp = 0;
    for (size_t root = 0; root < m; root++) {
      if (index[root] >= 0) {
        continue;
      }
      cs[csp] = (uint32_t)root;
      cpos[csp] = start[root];
      csp++;
      index[root] = low[root] = counter++;
      stk[sp++] = (uint32_t)root;
      on[root] = 1;
      while (csp) {
        uint32_t v = cs[csp - 1];
        size_t * p = &cpos[csp - 1];
        if (*p < start[v + 1]) {
          uint32_t w = edges[(*p)++];
          if (w == v) {
            d->cyclic[v] = 1;
          }
          if (index[w] < 0) {
            index[w] = low[w] = counter++;
            stk[sp++] = w;
            on[w] = 1;
            cs[csp] = w;
            cpos[csp] = start[w];
            csp++;
          } else if (on[w] && index[w] < low[v]) {
            low[v] = index[w];
          }
        } else {
          if (low[v] == index[v]) {
            size_t members = 0;
            size_t base = sp;
            while (base > 0 && stk[base - 1] != v) {
              base--;
            }
            base--;
            members = sp - base;
            for (size_t k = base; k < sp; k++) {
              on[stk[k]] = 0;
              if (members > 1) {
                d->cyclic[stk[k]] = 1;
              }
            }
            sp = base;
          }
          csp--;
          if (csp) {
            uint32_t parent = cs[csp - 1];
            if (low[v] < low[parent]) {
              low[parent] = low[v];
            }
          }
        }
      }
    }
  }
  gtext_allocator_free(a, start);
  gtext_allocator_free(a, edges);
  gtext_allocator_free(a, index);
  gtext_allocator_free(a, low);
  gtext_allocator_free(a, stk);
  gtext_allocator_free(a, cs);
  gtext_allocator_free(a, cpos);
  gtext_allocator_free(a, on);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  for (size_t w = 0; w < m; w++) {
    d->cycle_count += d->cyclic[w] ? 1 : 0;
  }
  if (d->cycle_count) {
    d->cycles = gtext_allocator_malloc(a, d->cycle_count * sizeof(uint32_t));
    if (!d->cycles) {
      return GTEXT_CSS_E_OOM;
    }
    /* In the order of the definitions: scan the items and keep the winners that are cyclic. */
    size_t k = 0;
    for (size_t w = 0; w < m; w++) {
      if (d->cyclic[w]) {
        d->cycles[k++] = (uint32_t)w;
      }
    }
    sort_ctx = d;
    /* order by the definition's order, not by name */
    for (size_t x = 1; x < k; x++) {
      uint32_t v = d->cycles[x];
      size_t y = x;
      while (y > 0 && d->items[d->winner[d->cycles[y - 1]]].order > d->items[d->winner[v]].order) {
        d->cycles[y] = d->cycles[y - 1];
        y--;
      }
      d->cycles[y] = v;
    }
  }
  return GTEXT_CSS_OK;
}

static GTEXT_CSS_Status seal(struct GTEXT_CSS_Condition_Defs * d) {
  if (d->sealed) {
    return GTEXT_CSS_OK;
  }
  uint32_t * all = gtext_allocator_malloc(&d->alloc, (d->count ? d->count : 1) * sizeof(uint32_t));
  if (!all) {
    return GTEXT_CSS_E_OOM;
  }
  for (size_t i = 0; i < d->count; i++) {
    all[i] = (uint32_t)i;
  }
  sort_ctx = d;
  qsort(all, d->count, sizeof(uint32_t), winner_cmp);
  /* The last of each name wins ("the truth value is based on the last one alone"). */
  size_t w = 0;
  for (size_t i = 0; i < d->count; i++) {
    if (i + 1 < d->count) {
      const css_def * a = &d->items[all[i]];
      const css_def * b = &d->items[all[i + 1]];
      if (name_cmp(a->name, a->name_length, b->name, b->name_length) == 0) {
        continue;
      }
    }
    all[w++] = all[i];
  }
  d->winner = all;
  d->winner_count = w;
  GTEXT_CSS_Status st = find_cycles(d);
  if (st != GTEXT_CSS_OK) {
    gtext_allocator_free(&d->alloc, d->winner);
    gtext_allocator_free(&d->alloc, d->cyclic);
    d->winner = NULL;
    d->cyclic = NULL;
    d->winner_count = 0;
    d->cycle_count = 0;
    return st;
  }
  d->sealed = true;
  return GTEXT_CSS_OK;
}

/* ---- the conditions object ------------------------------------------------ */

GTEXT_CSS_Conditions_Options gtext_css_conditions_options_default(void) {
  GTEXT_CSS_Conditions_Options o;
  memset(&o, 0, sizeof o);
  o.custom_media = true;
  o.max_work = (size_t)1 << 24;
  return o;
}

GTEXT_CSS_Status gtext_css_conditions_new(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Conditions_Options * options, GTEXT_CSS_Conditions ** out) {
  if (!out) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Conditions_Options o = options ? *options : gtext_css_conditions_options_default();
  if (!o.max_work) {
    o.max_work = (size_t)1 << 24;
  }
  GTEXT_CSS_Media_Environment env;
  if (o.media) {
    if (gtext_css_media_environment_check(o.media, NULL) != GTEXT_CSS_OK) {
      return GTEXT_CSS_E_INVALID;
    }
    /* A caller built before a field was added: its struct is shorter, the rest is zero (unsupported). */
    memset(&env, 0, sizeof env);
    size_t n = o.media->size < sizeof env ? o.media->size : sizeof env;
    memcpy(&env, o.media, n);
    env.size = sizeof env;
  } else {
    env = gtext_css_media_environment_default(NULL);
  }
  const GTEXT_Allocator * al = o.allocator ? o.allocator : gtext_allocator_default();
  struct GTEXT_CSS_Condition_Defs * defs = o.defs;
  if (!defs) {
    if (gtext_css_condition_defs_new(al, &defs) != GTEXT_CSS_OK) {
      return GTEXT_CSS_E_OOM;
    }
  } else {
    (void)gcu_atomic_int_fetch_add(&defs->refs, 1);
  }
  GTEXT_CSS_Status st = seal(defs);
  if (st != GTEXT_CSS_OK) {
    defs_unref(defs);
    return st;
  }
  struct GTEXT_CSS_Conditions * c = gtext_allocator_calloc(al, 1, sizeof *c);
  if (!c) {
    defs_unref(defs);
    return GTEXT_CSS_E_OOM;
  }
  c->alloc = *al;
  gcu_atomic_int_init(&c->refs, 1);
  c->registry = registry;
  c->env = env;
  c->custom_media = o.custom_media;
  c->max_work = o.max_work;
  if (o.policy) {
    c->policy = *o.policy;
  }
  c->iface = o.element_interface;
  c->defs = defs;
  *out = c;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Conditions * gtext_css_conditions_retain(const GTEXT_CSS_Conditions * conditions) {
  if (conditions) {
    (void)gcu_atomic_int_fetch_add((GCU_Atomic_Int *)&conditions->refs, 1);
  }
  return (GTEXT_CSS_Conditions *)conditions;
}

void gtext_css_conditions_free(const GTEXT_CSS_Conditions * conditions) {
  if (!conditions) {
    return;
  }
  struct GTEXT_CSS_Conditions * c = (struct GTEXT_CSS_Conditions *)conditions;
  if (gcu_atomic_int_fetch_sub(&c->refs, 1) != 1) {
    return;
  }
  GTEXT_Allocator a = c->alloc;
  defs_unref(c->defs);
  gtext_allocator_free(&a, c);
}

const GTEXT_CSS_Media_Environment * gtext_css_conditions_media_environment(const GTEXT_CSS_Conditions * conditions) {
  return conditions ? &conditions->env : NULL;
}

size_t gtext_css_conditions_cycle_count(const GTEXT_CSS_Conditions * conditions) {
  return conditions ? conditions->defs->cycle_count : 0;
}

const char * gtext_css_conditions_cycle_name(const GTEXT_CSS_Conditions * conditions, size_t index) {
  if (!conditions || index >= conditions->defs->cycle_count) {
    return NULL;
  }
  const struct GTEXT_CSS_Condition_Defs * d = conditions->defs;
  return d->items[d->winner[d->cycles[index]]].name;
}

GTEXT_CSS_Status css_conditions_prepare_call(const GTEXT_CSS_Conditions * c, GTEXT_CSS_Condition_Call * call) {
  struct GTEXT_CSS_Condition_Call_Internal * ci = call->internal;
  if (!ci) {
    ci = gtext_allocator_calloc(&c->alloc, 1, sizeof(struct GTEXT_CSS_Condition_Call_Internal));
    if (!ci) {
      return GTEXT_CSS_E_OOM;
    }
    ci->alloc = c->alloc;
    call->internal = ci;
  }
  size_t need = c->defs->winner_count;
  if (ci->owner != c->defs || ci->memo_n != need) {
    gtext_allocator_free(&ci->alloc, ci->memo);
    ci->memo = gtext_allocator_calloc(&ci->alloc, need ? need : 1, 1);
    if (!ci->memo) {
      ci->memo_n = 0;
      ci->owner = NULL;
      return GTEXT_CSS_E_OOM;
    }
    ci->memo_n = need;
    ci->owner = c->defs;
  }
  call->work = 0;
  call->detail = NULL;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_conditions_evaluate_query(const GTEXT_CSS_Conditions * c, const GTEXT_CSS_Media_List * list, size_t index,
    GTEXT_CSS_Condition_Call * call, GTEXT_CSS_Tristate * result) {
  if (!c || !list || !result || index >= list->in.root_count) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Condition_Call local;
  if (!call) {
    gtext_css_condition_call_init(&local);
    call = &local;
  }
  GTEXT_CSS_Status st = css_conditions_prepare_call(c, call);
  if (st == GTEXT_CSS_OK) {
    st = css_conditions_eval_query(c, call, &list->in, list->in.roots[index], result);
  }
  if (call == &local) {
    gtext_css_condition_call_free(&local);
  }
  return st;
}

GTEXT_CSS_Status gtext_css_conditions_matches_list(const GTEXT_CSS_Conditions * c, const GTEXT_CSS_Media_List * list, GTEXT_CSS_Condition_Call * call, bool * matches) {
  if (!c || !list || !matches) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Condition_Call local;
  if (!call) {
    gtext_css_condition_call_init(&local);
    call = &local;
  }
  GTEXT_CSS_Status st = css_conditions_prepare_call(c, call);
  bool any = list->in.root_count == 0; /* the empty list is true */
  size_t work = 0;
  for (size_t i = 0; st == GTEXT_CSS_OK && i < list->in.root_count && !any; i++) {
    GTEXT_CSS_Tristate t;
    st = css_conditions_eval_query(c, call, &list->in, list->in.roots[i], &t);
    work += call->work;
    if (st == GTEXT_CSS_OK && t == GTEXT_CSS_TRI_TRUE) {
      any = true;
    }
  }
  if (st == GTEXT_CSS_OK) {
    *matches = any;
    call->work = work;
  }
  if (call == &local) {
    gtext_css_condition_call_free(&local);
  }
  return st;
}

GTEXT_CSS_Status gtext_css_conditions_matches_media(const GTEXT_CSS_Conditions * c, const char * text, size_t length, GTEXT_CSS_Condition_Call * call, bool * matches) {
  if (!c || !matches || (length && !text)) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Media_Options o = gtext_css_media_options_default();
  o.allocator = &c->alloc;
  GTEXT_CSS_Media_List * list = NULL;
  GTEXT_CSS_Status st = gtext_css_media_list_parse(text, length, &o, &list, NULL);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  st = gtext_css_conditions_matches_list(c, list, call, matches);
  gtext_css_media_list_free(list);
  return st;
}

void gtext_css_media_list_dependencies(const GTEXT_CSS_Media_List * list, GTEXT_CSS_Condition_Dependencies * out) {
  if (!out) {
    return;
  }
  memset(out, 0, sizeof *out);
  if (!list) {
    return;
  }
  const struct GTEXT_CSS_Media_List_Internal * in = &list->in;
  out->environment = in->dependencies;
  out->features[0] = in->features_lo;
  out->features[1] = in->features_hi;
  for (size_t i = 0; i < in->node_count; i++) {
    if (in->nodes[i].kind == MQN_CUSTOM) {
      out->custom_media = true;
    } else if (in->nodes[i].kind == MQN_ENCLOSED && in->nodes[i].name) {
      out->unknown_features = true;
      out->environment |= 1u << MQD_EXTENSION;
    }
  }
}

GTEXT_CSS_Tristate gtext_css_conditions_condition(const GTEXT_CSS_Conditions * c, GTEXT_CSS_Condition_Kind kind, const GTEXT_CSS_String * text) {
  if (!c || !text || (kind != GTEXT_CSS_CONDITION_MEDIA && kind != GTEXT_CSS_CONDITION_SUPPORTS)) {
    return GTEXT_CSS_TRI_UNKNOWN;
  }
  bool m = false;
  GTEXT_CSS_Status st = kind == GTEXT_CSS_CONDITION_MEDIA ? gtext_css_conditions_matches_media(c, text->data, text->length, NULL, &m)
                                                          : gtext_css_conditions_supports_text(c, text->data, text->length, &m);
  if (st != GTEXT_CSS_OK) {
    return GTEXT_CSS_TRI_UNKNOWN;
  }
  return m ? GTEXT_CSS_TRI_TRUE : GTEXT_CSS_TRI_FALSE;
}

GTEXT_CSS_Status gtext_css_conditions_evaluate_supports(const GTEXT_CSS_Conditions * c, const GTEXT_CSS_Supports_Condition * cond, GTEXT_CSS_Condition_Call * call,
    GTEXT_CSS_Tristate * result) {
  return gtext_css_conditions_evaluate_query(c, cond, 0, call, result);
}

GTEXT_CSS_Status gtext_css_conditions_supports_text(const GTEXT_CSS_Conditions * c, const char * text, size_t length, bool * result) {
  if (!c || !result || (length && !text)) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Media_Options o = gtext_css_media_options_default();
  o.allocator = &c->alloc;
  GTEXT_CSS_Supports_Condition * cond = NULL;
  GTEXT_CSS_Status st = gtext_css_supports_parse(text, length, true, &o, &cond, NULL);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSS_Tristate t = GTEXT_CSS_TRI_UNKNOWN;
  st = gtext_css_conditions_evaluate_supports(c, cond, NULL, &t);
  gtext_css_media_list_free(cond);
  if (st == GTEXT_CSS_OK) {
    *result = t == GTEXT_CSS_TRI_TRUE;
  }
  return st;
}

static GTEXT_CSS_Tristate hook(void * user, GTEXT_CSS_Condition_Kind kind, const GTEXT_CSS_String * text, GTEXT_CSS_Element_Handle element) {
  (void)element;
  return gtext_css_conditions_condition(user, kind, text);
}

void gtext_css_conditions_environment_hook(const GTEXT_CSS_Conditions * conditions, GTEXT_CSS_Environment * env) {
  if (!env) {
    return;
  }
  env->condition = hook;
  env->user = (void *)conditions;
}
