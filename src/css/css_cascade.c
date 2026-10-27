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
 * The cascade (css_cascade.h): the candidate set with the sort keys of its declarations, and the driver that groups the candidates by the property they
 * set (a logical group's members by the slot they share in the element's flow), decides each group (css_revert.c) and hands back the winners.
 */

#include <stdlib.h>
#include <string.h>

#include "css_cascade_internal.h"
#include "css_shorthand_internal.h"

#define DEFAULT_MAX_CANDIDATES ((size_t)1 << 20)
#define LIMIT_MAX_CANDIDATES ((size_t)1 << 28)
#define DEFAULT_MAX_DIAGNOSTICS ((size_t)1024)
#define LIMIT_MAX_DIAGNOSTICS ((size_t)1 << 20)

/* ---- specificity ------------------------------------------------------------------------------------------- */

static uint32_t saturate(uint64_t v) {
  return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

GTEXT_CSS_Specificity gtext_css_specificity_make(uint64_t a, uint64_t b, uint64_t c) {
  GTEXT_CSS_Specificity s;
  s.a = saturate(a);
  s.b = saturate(b);
  s.c = saturate(c);
  return s;
}

GTEXT_CSS_Specificity gtext_css_specificity_add(GTEXT_CSS_Specificity x, GTEXT_CSS_Specificity y) {
  return gtext_css_specificity_make((uint64_t)x.a + y.a, (uint64_t)x.b + y.b, (uint64_t)x.c + y.c);
}

int gtext_css_specificity_compare(GTEXT_CSS_Specificity x, GTEXT_CSS_Specificity y) {
  if (x.a != y.a) {
    return x.a < y.a ? -1 : 1;
  }
  if (x.b != y.b) {
    return x.b < y.b ? -1 : 1;
  }
  if (x.c != y.c) {
    return x.c < y.c ? -1 : 1;
  }
  return 0;
}

/* ---- the result record -------------------------------------------------------------------------------------- */

static void result_clear(GTEXT_CSS_Cascade_Result * r) {
  if (r) {
    memset(r, 0, sizeof *r);
    r->message = gtext_css_status_string(GTEXT_CSS_OK);
  }
}

static GTEXT_CSS_Status result_done(GTEXT_CSS_Cascade_Result * r, GTEXT_CSS_Status st) {
  if (r) {
    r->status = st;
    r->message = gtext_css_status_string(st);
  }
  return st;
}

static void result_name(GTEXT_CSS_Cascade_Result * r, const char * s, size_t n) {
  if (!r) {
    return;
  }
  if (n > sizeof r->name - 1) {
    n = sizeof r->name - 1;
  }
  if (n) {
    memcpy(r->name, s, n);
  }
  r->name[n] = '\0';
}

static GTEXT_CSS_Status fail(GTEXT_CSS_Cascade_Result * r, GTEXT_CSS_Status st, const char * name, const char * detail) {
  if (r) {
    if (name) {
      result_name(r, name, strlen(name));
    }
    r->detail = detail;
  }
  return result_done(r, st);
}

/* ---- the candidate set -------------------------------------------------------------------------------------- */

GTEXT_CSS_Candidate_Options gtext_css_candidate_options_default(void) {
  GTEXT_CSS_Candidate_Options o;
  memset(&o, 0, sizeof o);
  o.max_candidates = DEFAULT_MAX_CANDIDATES;
  o.max_diagnostics = DEFAULT_MAX_DIAGNOSTICS;
  return o;
}

GTEXT_CSS_Status gtext_css_candidates_new(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Layer_Registry * layers,
    const GTEXT_CSS_Candidate_Options * options, GTEXT_CSS_Candidates ** out) {
  if (!registry || !out || (layers && !gtext_css_layer_is_frozen(layers))) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Candidate_Options o = options ? *options : gtext_css_candidate_options_default();
  if (o.max_candidates == 0) {
    o.max_candidates = DEFAULT_MAX_CANDIDATES;
  }
  if (o.max_candidates > LIMIT_MAX_CANDIDATES) {
    o.max_candidates = LIMIT_MAX_CANDIDATES;
  }
  if (o.max_diagnostics == 0) {
    o.max_diagnostics = DEFAULT_MAX_DIAGNOSTICS;
  }
  if (o.max_diagnostics > LIMIT_MAX_DIAGNOSTICS) {
    o.max_diagnostics = LIMIT_MAX_DIAGNOSTICS;
  }
  const GTEXT_Allocator * ap = o.allocator ? o.allocator : css_registry_allocator(registry);
  GTEXT_Allocator a = *ap;
  GTEXT_CSS_Candidates * s = gtext_allocator_calloc(&a, 1, sizeof *s);
  if (!s) {
    return GTEXT_CSS_E_OOM;
  }
  s->registry = registry;
  s->layers = layers;
  s->alloc = a;
  s->options = o;
  s->options.allocator = NULL;
  s->all_id = gtext_css_registry_find_property(registry, "all", 3);
  s->custom_id = gtext_css_registry_find_property(registry, "--x", 3);
  gcu_atomic_int_init(&s->refs, 1);
  gcu_atomic_int_init(&s->sealed, 0);
  *out = s;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Candidates * gtext_css_candidates_retain(GTEXT_CSS_Candidates * set) {
  if (set) {
    (void)gcu_atomic_int_fetch_add(&set->refs, 1);
  }
  return set;
}

void gtext_css_candidates_free(GTEXT_CSS_Candidates * set) {
  if (!set || gcu_atomic_int_fetch_sub(&set->refs, 1) != 1) {
    return;
  }
  GTEXT_Allocator a = set->alloc;
  for (size_t i = 0; i < set->count; i++) {
    gtext_css_value_set_free(set->items[i].set);
  }
  gtext_allocator_free(&a, set->items);
  gtext_allocator_free(&a, set->contexts);
  gtext_allocator_free(&a, set->names);
  gtext_allocator_free(&a, set->diags);
  gtext_allocator_free(&a, set);
}

/// The set's work counter (probes of its table of contexts), for the quadratic suite; hidden, like css_registry_work().
size_t css_candidates_work(const GTEXT_CSS_Candidates * set) {
  return set ? set->work : 0;
}

size_t gtext_css_candidates_count(const GTEXT_CSS_Candidates * set) {
  return set ? set->count : 0;
}

bool gtext_css_candidates_sealed(const GTEXT_CSS_Candidates * set) {
  return set && gcu_atomic_int_load(&set->sealed) != 0;
}

static void entry_fill(const GTEXT_CSS_Candidates * s, const cand * c, size_t index, GTEXT_CSS_Property_Id property, GTEXT_CSS_Cascade_Entry * e) {
  memset(e, 0, sizeof *e);
  e->origin = (GTEXT_CSS_Origin)c->origin;
  e->important = c->important;
  e->presentational_hint = c->hint;
  e->element_attached = c->attached;
  e->layer = c->layer;
  e->context = c->context;
  e->scoped = c->scoped;
  e->scope_distance = c->scope_distance;
  e->specificity = c->specificity;
  e->sheet = c->sheet;
  e->rule = c->rule;
  e->declaration = c->declaration;
  e->rule_id = c->rule_id;
  e->declaration_id = c->declaration_id;
  e->span = c->span;
  e->property = property ? property : c->property;
  if (c->kind == CAND_CUSTOM) {
    e->custom_name.data = cand_name(s, c);
    e->custom_name.length = c->name_len;
  }
  e->outcome = (GTEXT_CSS_Property_Outcome)c->outcome;
  e->wide = (GTEXT_CSS_Wide_Keyword)c->wide;
  e->set = c->set;
  e->shorthand = c->shorthand;
  e->group = c->group;
  e->from_all = c->kind == CAND_ALL;
  e->index = index;
}

bool gtext_css_candidates_entry(const GTEXT_CSS_Candidates * set, size_t index, GTEXT_CSS_Cascade_Entry * out) {
  if (!set || !out || index >= set->count) {
    return false;
  }
  entry_fill(set, &set->items[index], index, 0, out);
  return true;
}

/// Room for `more` candidates; GTEXT_CSS_E_LIMIT_CANDIDATES at the declaration the keys describe.
static GTEXT_CSS_Status reserve(GTEXT_CSS_Candidates * s, size_t more, const GTEXT_CSS_Cascade_Entry * keys, GTEXT_CSS_Cascade_Result * r) {
  if (more > s->options.max_candidates || s->count > s->options.max_candidates - more) {
    if (r) {
      r->declaration = keys->declaration_id;
      r->span = keys->span;
    }
    return fail(r, GTEXT_CSS_E_LIMIT_CANDIDATES, NULL, "more candidates than max_candidates");
  }
  if (s->count + more <= s->cap) {
    return GTEXT_CSS_OK;
  }
  size_t cap = s->cap ? s->cap : 16;
  while (cap < s->count + more) {
    cap *= 2;
  }
  cand * g = gtext_allocator_realloc(&s->alloc, s->items, cap * sizeof *g);
  if (!g) {
    return result_done(r, GTEXT_CSS_E_OOM);
  }
  s->items = g;
  s->cap = cap;
  return GTEXT_CSS_OK;
}

/// Append `n` bytes (and a NUL) to the name buffer; the offset is in `*off`.
static GTEXT_CSS_Status name_put(GTEXT_CSS_Candidates * s, const char * name, size_t n, uint32_t * off) {
  if (n >= 0x7FFFFFFFu || s->names_len > 0x7FFFFFFFu - n - 1) {
    return GTEXT_CSS_E_LIMIT_CANDIDATES;
  }
  if (s->names_len + n + 1 > s->names_cap) {
    size_t cap = s->names_cap ? s->names_cap : 256;
    while (cap < s->names_len + n + 1) {
      cap *= 2;
    }
    char * g = gtext_allocator_realloc(&s->alloc, s->names, cap);
    if (!g) {
      return GTEXT_CSS_E_OOM;
    }
    s->names = g;
    s->names_cap = cap;
  }
  memcpy(s->names + s->names_len, name, n);
  s->names[s->names_len + n] = '\0';
  *off = (uint32_t)s->names_len;
  s->names_len += n + 1;
  return GTEXT_CSS_OK;
}

/* ---- the contexts ------------------------------------------------------------------------------------------- */

static size_t ctx_slot(uint32_t context, size_t cap) {
  uint32_t h = context * 0x9E3779B1u;
  h ^= h >> 15;
  h *= 0x85EBCA6Bu;
  h ^= h >> 13;
  return (size_t)h & (cap - 1);
}

/// The entry of a context, or NULL when there is none.
static const ctx_entry * ctx_find(const GTEXT_CSS_Candidates * s, uint32_t context) {
  if (!s->contexts) {
    return NULL;
  }
  size_t i = ctx_slot(context, s->contexts_cap);
  while (s->contexts[i].occupied) {
    if (s->contexts[i].context == context) {
      return &s->contexts[i];
    }
    i = (i + 1) & (s->contexts_cap - 1);
  }
  return NULL;
}

/// The entry of a context, made if it is new; NULL when memory ran out.
static ctx_entry * ctx_get(GTEXT_CSS_Candidates * s, uint32_t context) {
  if ((s->ncontexts + 1) * 2 > s->contexts_cap) {
    const size_t cap = s->contexts_cap ? s->contexts_cap * 2 : 8;
    ctx_entry * t = gtext_allocator_calloc(&s->alloc, cap, sizeof *t);
    if (!t) {
      return NULL;
    }
    for (size_t k = 0; k < s->contexts_cap; k++) {
      if (s->contexts[k].occupied) {
        size_t i = ctx_slot(s->contexts[k].context, cap);
        while (t[i].occupied) {
          i = (i + 1) & (cap - 1);
        }
        t[i] = s->contexts[k];
      }
    }
    gtext_allocator_free(&s->alloc, s->contexts);
    s->contexts = t;
    s->contexts_cap = cap;
  }
  size_t i = ctx_slot(context, s->contexts_cap);
  while (s->contexts[i].occupied) {
    s->work++;
    if (s->contexts[i].context == context) {
      return &s->contexts[i];
    }
    i = (i + 1) & (s->contexts_cap - 1);
  }
  s->contexts[i].occupied = 1;
  s->contexts[i].context = context;
  s->ncontexts++;
  return &s->contexts[i];
}

/// The layer registry that the layers of a context belong to.
static const GTEXT_CSS_Layer_Registry * layers_for(const GTEXT_CSS_Candidates * s, uint32_t context) {
  const ctx_entry * e = ctx_find(s, context);
  return e && e->layers ? e->layers : s->layers;
}

GTEXT_CSS_Status gtext_css_candidates_use_layers(GTEXT_CSS_Candidates * set, uint32_t context, const GTEXT_CSS_Layer_Registry * layers) {
  if (!set || !layers || !gtext_css_layer_is_frozen(layers) || gcu_atomic_int_load(&set->sealed)) {
    return GTEXT_CSS_E_INVALID;
  }
  const ctx_entry * seen = ctx_find(set, context);
  if (seen && seen->used) {
    return GTEXT_CSS_E_INVALID; /* its candidates were keyed by the ranks of another registry */
  }
  ctx_entry * e = ctx_get(set, context);
  if (!e) {
    return GTEXT_CSS_E_OOM;
  }
  e->layers = layers;
  return GTEXT_CSS_OK;
}

/// The sort key of an entry's keys, validated; `*c` is filled except for the property and the value.
static GTEXT_CSS_Status make_base(GTEXT_CSS_Candidates * s, const GTEXT_CSS_Cascade_Entry * k, cand * c, GTEXT_CSS_Cascade_Result * r) {
  memset(c, 0, sizeof *c);
  if ((unsigned)k->origin >= (unsigned)GTEXT_CSS_ORIGIN_COUNT) {
    return fail(r, GTEXT_CSS_E_INVALID, NULL, "the origin is not one");
  }
  const bool timed = k->origin == GTEXT_CSS_ORIGIN_ANIMATION || k->origin == GTEXT_CSS_ORIGIN_TRANSITION;
  if (timed && (k->important || k->presentational_hint || k->element_attached || k->layer != GTEXT_CSS_LAYER_NONE)) {
    return fail(r, GTEXT_CSS_E_INVALID, NULL, "an animation or transition declaration is not important, a hint, element-attached or layered");
  }
  if (k->presentational_hint && (k->origin != GTEXT_CSS_ORIGIN_AUTHOR || k->important || k->element_attached || k->layer != GTEXT_CSS_LAYER_NONE)) {
    return fail(r, GTEXT_CSS_E_INVALID, NULL, "a presentational hint is author origin, normal, unlayered and not element-attached");
  }
  if (k->element_attached && k->layer != GTEXT_CSS_LAYER_NONE) {
    return fail(r, GTEXT_CSS_E_INVALID, NULL, "an element-attached declaration is not in a layer");
  }
  size_t rank = 0, count = 0;
  if (!timed) {
    const GTEXT_CSS_Layer_Registry * lr = layers_for(s, k->context);
    if (k->layer != GTEXT_CSS_LAYER_NONE && !lr) {
      return fail(r, GTEXT_CSS_E_INVALID, NULL, "a layer was given and the candidate set has no layer registry for its context");
    }
    if (lr) {
      count = gtext_css_layer_count(lr, k->origin);
      if (gtext_css_layer_rank(lr, k->origin, k->layer, &rank) != GTEXT_CSS_OK) {
        return fail(r, GTEXT_CSS_E_INVALID, NULL, "the layer is not a layer of this origin in the layer registry");
      }
    }
  }
  if (!ctx_get(s, k->context)) { /* the entry is made here, where memory can be refused; push() marks it used */
    return result_done(r, GTEXT_CSS_E_OOM);
  }
  uint32_t tier;
  switch (k->origin) {
  case GTEXT_CSS_ORIGIN_USER_AGENT: tier = k->important ? TIER_IMPORTANT_UA : TIER_NORMAL_UA; break;
  case GTEXT_CSS_ORIGIN_USER: tier = k->important ? TIER_IMPORTANT_USER : TIER_NORMAL_USER; break;
  case GTEXT_CSS_ORIGIN_AUTHOR: tier = k->important ? TIER_IMPORTANT_AUTHOR : k->presentational_hint ? TIER_HINT : TIER_NORMAL_AUTHOR; break;
  case GTEXT_CSS_ORIGIN_ANIMATION: tier = TIER_ANIMATION; break;
  default: tier = TIER_TRANSITION; break;
  }
  /* a hint, an attached declaration and a timed one have no style rule: no specificity, no scope */
  const bool ruleless = k->presentational_hint || k->element_attached || timed;
  GTEXT_CSS_Specificity spec = {0, 0, 0};
  if (!ruleless) {
    spec = k->specificity;
  }
  const bool scoped = !ruleless && k->scoped;
  uint32_t distance = k->scope_distance;
  if (distance == UINT32_MAX) {
    distance = UINT32_MAX - 1;
  }
  c->k[CK_TIER] = tier;
  c->k[CK_CONTEXT] = k->important ? k->context : UINT32_MAX - k->context;
  c->k[CK_ATTACHED] = k->element_attached ? 1u : 0u;
  c->layer_normal = k->presentational_hint ? 0u : (uint32_t)rank + 1u;
  c->k[CK_LAYER] = k->presentational_hint ? 0u : (k->important ? (uint32_t)(count - rank) + 1u : (uint32_t)rank + 1u);
  c->k[CK_SPEC_A] = spec.a;
  c->k[CK_SPEC_B] = spec.b;
  c->k[CK_SPEC_C] = spec.c;
  c->k[CK_SCOPE] = scoped ? UINT32_MAX - distance : 0u;
  c->k[CK_SHEET] = k->sheet;
  c->k[CK_RULE] = k->rule;
  c->k[CK_DECLARATION] = k->declaration;
  c->context = k->context;
  c->layer = k->layer;
  c->sheet = k->sheet;
  c->rule = k->rule;
  c->declaration = k->declaration;
  c->scoped = scoped;
  c->scope_distance = scoped ? k->scope_distance : 0;
  c->specificity = spec;
  c->rule_id = k->rule_id;
  c->declaration_id = k->declaration_id;
  c->span = k->span;
  c->origin = (uint8_t)k->origin;
  c->level = k->origin == GTEXT_CSS_ORIGIN_USER_AGENT ? 0 : k->origin == GTEXT_CSS_ORIGIN_USER ? 1 : 2;
  c->important = k->important;
  c->hint = k->presentational_hint;
  c->attached = k->element_attached;
  return GTEXT_CSS_OK;
}

/// Append a candidate made from `base`; the room was reserved. A value set is retained.
static void push(GTEXT_CSS_Candidates * s, const cand * base, cand_kind kind, GTEXT_CSS_Property_Id property, GTEXT_CSS_Property_Id shorthand,
    GTEXT_CSS_Property_Outcome outcome, GTEXT_CSS_Wide_Keyword wide, const GTEXT_CSS_Value_Set * set, uint32_t name_off, uint32_t name_len,
    uint32_t group) {
  ctx_entry * ce = (ctx_entry *)(uintptr_t)ctx_find(s, base->context);  /* made by make_base() */
  if (ce) {
    ce->used = 1; /* from here the context has a candidate: its registry is no longer to be changed */
  }
  cand * c = &s->items[s->count];
  *c = *base;
  c->k[CK_POSITION] = (uint32_t)s->count;
  c->kind = (uint8_t)kind;
  c->property = property;
  c->shorthand = shorthand;
  c->outcome = (uint8_t)outcome;
  c->wide = (uint8_t)wide;
  c->set = gtext_css_value_set_retain(set);
  c->name_off = name_off;
  c->name_len = name_len;
  c->group = group;
  s->count++;
}

static GTEXT_CSS_Status check_open(GTEXT_CSS_Candidates * s, const GTEXT_CSS_Cascade_Entry * keys, GTEXT_CSS_Cascade_Result * r) {
  if (!s || !keys) {
    return result_done(r, GTEXT_CSS_E_INVALID);
  }
  if (gcu_atomic_int_load(&s->sealed)) {
    return fail(r, GTEXT_CSS_E_INVALID, NULL, "the candidate set was sealed by a cascade: no more candidates are added");
  }
  return GTEXT_CSS_OK;
}

static bool is_wide_outcome(GTEXT_CSS_Property_Outcome o) {
  return o == GTEXT_CSS_PROP_RESULT_VALID || o == GTEXT_CSS_PROP_RESULT_WIDE || o == GTEXT_CSS_PROP_RESULT_PENDING;
}

GTEXT_CSS_Status gtext_css_candidates_add(GTEXT_CSS_Candidates * set, const GTEXT_CSS_Cascade_Entry * keys, const GTEXT_CSS_Property_Value * value,
    GTEXT_CSS_Cascade_Result * result) {
  result_clear(result);
  GTEXT_CSS_Status st = check_open(set, keys, result);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!value || !is_wide_outcome(value->outcome)) {
    return fail(result, GTEXT_CSS_E_INVALID, NULL, "only a valid, wide or pending value is added as a candidate");
  }
  GTEXT_CSS_Property_Info info;
  if (!value->property || !gtext_css_registry_property_info(set->registry, value->property, &info)) {
    return result_done(result, GTEXT_CSS_E_INVALID);
  }
  if (info.flags & GTEXT_CSS_PROP_CUSTOM) {
    return fail(result, GTEXT_CSS_E_INVALID, NULL, "a custom property is added with gtext_css_candidates_add_custom()");
  }
  cand base;
  st = make_base(set, keys, &base, result);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (result) {
    result->property = value->property;
    result_name(result, info.name, info.name_length);
  }
  const uint32_t group = set->next_group;
  if (value->property == set->all_id) {
    if (value->outcome != GTEXT_CSS_PROP_RESULT_WIDE) {
      return fail(result, GTEXT_CSS_E_INVALID, "all", "`all` takes a CSS-wide keyword only");
    }
    st = reserve(set, 1, keys, result);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    push(set, &base, CAND_ALL, value->property, 0, value->outcome, value->wide, NULL, 0, 0, group);
    set->next_group++;
    return result_done(result, GTEXT_CSS_OK);
  }
  if (!(info.flags & GTEXT_CSS_PROP_SHORTHAND)) {
    st = reserve(set, 1, keys, result);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    push(set, &base, CAND_PROPERTY, value->property, 0, value->outcome, value->wide, value->set, 0, 0, group);
    set->next_group++;
    return result_done(result, GTEXT_CSS_OK);
  }
  GTEXT_CSS_Longhand_List * list = NULL;
  GTEXT_CSS_Shorthand_Result sr;
  st = gtext_css_shorthand_expand(set->registry, value, &set->alloc, &list, &sr);
  if (st != GTEXT_CSS_OK) {
    if (result) {
      result_name(result, sr.name, strlen(sr.name));
      result->detail = sr.detail;
    }
    return result_done(result, st);
  }
  const size_t n = gtext_css_longhand_list_count(list);
  st = reserve(set, n, keys, result);
  if (st != GTEXT_CSS_OK) {
    gtext_css_longhand_list_free(list);
    return st;
  }
  for (size_t i = 0; i < n; i++) {
    GTEXT_CSS_Longhand lh;
    gtext_css_longhand_list_at(list, i, &lh);
    push(set, &base, CAND_PROPERTY, lh.property, value->property, lh.outcome, lh.wide, lh.set, 0, 0, group);
  }
  set->next_group++;
  gtext_css_longhand_list_free(list);
  if (result) {
    result->work = n;
  }
  return result_done(result, GTEXT_CSS_OK);
}

GTEXT_CSS_Status gtext_css_candidates_add_custom(GTEXT_CSS_Candidates * set, const GTEXT_CSS_Cascade_Entry * keys, const char * name,
    size_t name_length, const GTEXT_CSS_Property_Value * value, GTEXT_CSS_Cascade_Result * result) {
  result_clear(result);
  GTEXT_CSS_Status st = check_open(set, keys, result);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!name || name_length < 3 || name[0] != '-' || name[1] != '-' || !value ||
      (value->outcome != GTEXT_CSS_PROP_RESULT_VALID && value->outcome != GTEXT_CSS_PROP_RESULT_WIDE)) {
    return fail(result, GTEXT_CSS_E_INVALID, NULL, "a custom property has a name beginning `--` and a valid or wide value");
  }
  result_name(result, name, name_length);
  cand base;
  st = make_base(set, keys, &base, result);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  st = reserve(set, 1, keys, result);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  uint32_t off = 0;
  st = name_put(set, name, name_length, &off);
  if (st != GTEXT_CSS_OK) {
    return fail(result, st, NULL, st == GTEXT_CSS_E_OOM ? NULL : "the custom property names are longer than the set can hold");
  }
  push(set, &base, CAND_CUSTOM, set->custom_id, 0, value->outcome, value->wide, value->set, off, (uint32_t)name_length, set->next_group++);
  return result_done(result, GTEXT_CSS_OK);
}

/* ---- diagnostics and declarations ----------------------------------------------------------------------------- */

static GTEXT_CSS_Status diagnose(GTEXT_CSS_Candidates * s, GTEXT_CSS_Diagnostic_Kind kind, const char * name, size_t n, GTEXT_CSS_Id declaration,
    const GTEXT_CSS_Span * span) {
  s->diag_total++;
  if (s->ndiags >= s->options.max_diagnostics) {
    return GTEXT_CSS_OK;
  }
  if (s->ndiags == s->diag_cap) {
    size_t cap = s->diag_cap ? s->diag_cap * 2 : 8;
    cand_diag * g = gtext_allocator_realloc(&s->alloc, s->diags, cap * sizeof *g);
    if (!g) {
      return GTEXT_CSS_E_OOM;
    }
    s->diags = g;
    s->diag_cap = cap;
  }
  uint32_t off = 0;
  GTEXT_CSS_Status st = name_put(s, name, n, &off);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  cand_diag * d = &s->diags[s->ndiags++];
  d->kind = kind;
  d->name_off = off;
  d->name_len = (uint32_t)n;
  d->declaration = declaration;
  d->span = *span;
  return GTEXT_CSS_OK;
}

size_t gtext_css_candidates_diagnostic_total(const GTEXT_CSS_Candidates * set) {
  return set ? set->diag_total : 0;
}

bool gtext_css_candidates_diagnostic(const GTEXT_CSS_Candidates * set, size_t index, GTEXT_CSS_Cascade_Diagnostic * out) {
  if (!set || !out || index >= set->ndiags) {
    return false;
  }
  const cand_diag * d = &set->diags[index];
  out->kind = d->kind;
  out->name.data = set->names + d->name_off;
  out->name.length = d->name_len;
  out->declaration = d->declaration;
  out->span = d->span;
  return true;
}

/// What a parsed declaration comes to for the cascade: add it, or drop it and say why.
static GTEXT_CSS_Status add_parsed(GTEXT_CSS_Candidates * set, const GTEXT_CSS_Cascade_Entry * keys, const char * name, size_t name_length,
    GTEXT_CSS_Property_Value * pv, GTEXT_CSS_Property_Outcome * outcome, GTEXT_CSS_Cascade_Result * result) {
  if (outcome) {
    *outcome = pv->outcome;
  }
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  if (pv->outcome == GTEXT_CSS_PROP_RESULT_UNKNOWN || pv->outcome == GTEXT_CSS_PROP_RESULT_INVALID) {
    st = diagnose(set, pv->outcome == GTEXT_CSS_PROP_RESULT_UNKNOWN ? GTEXT_CSS_DIAG_UNKNOWN_PROPERTY : GTEXT_CSS_DIAG_INVALID_VALUE, name, name_length,
        keys->declaration_id, &keys->span);
    if (result) {
      result_name(result, name, name_length);
      result->detail = pv->outcome == GTEXT_CSS_PROP_RESULT_UNKNOWN ? "unknown property: the declaration is dropped" :
                                                                       "invalid value: the declaration is dropped";
      result->declaration = keys->declaration_id;
      result->span = keys->span;
    }
    return result_done(result, st);
  }
  if (name_length >= 2 && name[0] == '-' && name[1] == '-') {
    return gtext_css_candidates_add_custom(set, keys, name, name_length, pv, result);
  }
  return gtext_css_candidates_add(set, keys, pv, result);
}

GTEXT_CSS_Status gtext_css_candidates_add_declaration(GTEXT_CSS_Candidates * set, const GTEXT_CSS_Cascade_Entry * keys,
    const GTEXT_CSS_Declaration * declaration, const GTEXT_CSS_Property_Options * options, GTEXT_CSS_Property_Outcome * outcome,
    GTEXT_CSS_Cascade_Result * result) {
  result_clear(result);
  GTEXT_CSS_Status st = check_open(set, keys, result);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!declaration || !declaration->name.data) {
    return result_done(result, GTEXT_CSS_E_INVALID);
  }
  GTEXT_CSS_Cascade_Entry k = *keys;
  k.important = declaration->important;
  k.declaration_id = declaration->id;
  k.span = declaration->span;
  GTEXT_CSS_Property_Options po;
  memset(&po, 0, sizeof po);
  if (options) {
    po = *options;
  }
  if (!po.allocator) {
    po.allocator = &set->alloc;
  }
  GTEXT_CSS_Property_Value pv;
  st = gtext_css_parse_property_declaration(set->registry, declaration, &po, &pv);
  if (st != GTEXT_CSS_OK) {
    if (result) {
      result_name(result, declaration->name.data, declaration->name.length);
      result->property = pv.property;
      result->declaration = declaration->id;
      result->span = declaration->span;
      result->detail = st == GTEXT_CSS_E_UNSUPPORTED ? "the value of this property cannot be judged: the declaration was not added" : NULL;
    }
    gtext_css_property_value_free(&pv);
    return result_done(result, st);
  }
  st = add_parsed(set, &k, declaration->name.data, declaration->name.length, &pv, outcome, result);
  gtext_css_property_value_free(&pv);
  return st;
}

GTEXT_CSS_Status gtext_css_candidates_add_text(GTEXT_CSS_Candidates * set, const GTEXT_CSS_Cascade_Entry * keys, const char * name,
    size_t name_length, const char * text, size_t text_length, const GTEXT_CSS_Property_Options * options, GTEXT_CSS_Property_Outcome * outcome,
    GTEXT_CSS_Cascade_Result * result) {
  result_clear(result);
  GTEXT_CSS_Status st = check_open(set, keys, result);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  if (!name || (!text && text_length)) {
    return result_done(result, GTEXT_CSS_E_INVALID);
  }
  GTEXT_CSS_Property_Options po;
  memset(&po, 0, sizeof po);
  if (options) {
    po = *options;
  }
  if (!po.allocator) {
    po.allocator = &set->alloc;
  }
  GTEXT_CSS_Property_Value pv;
  st = gtext_css_parse_property_text(set->registry, name, name_length, text ? text : "", text_length, &po, &pv);
  if (st != GTEXT_CSS_OK) {
    if (result) {
      result_name(result, name, name_length);
      result->property = pv.property;
      result->detail = st == GTEXT_CSS_E_UNSUPPORTED ? "the value of this property cannot be judged: the declaration was not added" : NULL;
    }
    gtext_css_property_value_free(&pv);
    return result_done(result, st);
  }
  st = add_parsed(set, keys, name, name_length, &pv, outcome, result);
  gtext_css_property_value_free(&pv);
  return st;
}

/* ---- the result ----------------------------------------------------------------------------------------------- */

typedef struct {
  uint32_t unit;       ///< The property the winner is for: the physical property of its slot. 0 for a custom property.
  uint32_t entry;      ///< The winning candidate.
  uint32_t rollbacks;  ///< The rolling declarations passed on the way.
} win;

struct GTEXT_CSS_Cascaded {
  GTEXT_Allocator alloc;
  GTEXT_CSS_Candidates * set;     ///< Retained.
  GTEXT_CSS_Flow flow;
  win * wins;
  size_t nwins, nreg;             ///< The winners, and how many of them are of registered properties (they come first).
  size_t * lstart;                ///< keep_losers: winner i's losers are litems[lstart[i] .. lstart[i+1]); NULL otherwise.
  uint32_t * litems;              ///< ... a candidate index, the top bit set for a rolled-back one.
};

void gtext_css_cascaded_free(GTEXT_CSS_Cascaded * c) {
  if (!c) {
    return;
  }
  GTEXT_Allocator a = c->alloc;
  gtext_allocator_free(&a, c->wins);
  gtext_allocator_free(&a, c->lstart);
  gtext_allocator_free(&a, c->litems);
  gtext_css_candidates_free(c->set);
  gtext_allocator_free(&a, c);
}

GTEXT_CSS_Flow gtext_css_cascaded_flow(const GTEXT_CSS_Cascaded * c) {
  GTEXT_CSS_Flow f;
  memset(&f, 0, sizeof f);
  return c ? c->flow : f;
}

size_t gtext_css_cascaded_count(const GTEXT_CSS_Cascaded * c) {
  return c ? c->nwins : 0;
}

static void win_entry(const GTEXT_CSS_Cascaded * c, const win * w, GTEXT_CSS_Cascade_Entry * out) {
  const cand * k = &c->set->items[w->entry];
  entry_fill(c->set, k, w->entry, k->kind == CAND_ALL ? w->unit : 0, out);
}

bool gtext_css_cascaded_at(const GTEXT_CSS_Cascaded * c, size_t index, GTEXT_CSS_Cascade_Entry * entry, size_t * rollbacks) {
  if (!c || !entry || index >= c->nwins) {
    return false;
  }
  win_entry(c, &c->wins[index], entry);
  if (rollbacks) {
    *rollbacks = c->wins[index].rollbacks;
  }
  return true;
}

size_t gtext_css_cascaded_loser_count(const GTEXT_CSS_Cascaded * c, size_t index) {
  if (!c || !c->lstart || index >= c->nwins) {
    return 0;
  }
  return c->lstart[index + 1] - c->lstart[index];
}

bool gtext_css_cascaded_loser(const GTEXT_CSS_Cascaded * c, size_t index, size_t loser, GTEXT_CSS_Cascade_Entry * entry, GTEXT_CSS_Loss_Reason * reason) {
  if (!c || !c->lstart || !entry || index >= c->nwins || loser >= c->lstart[index + 1] - c->lstart[index]) {
    return false;
  }
  const uint32_t v = c->litems[c->lstart[index] + loser];
  const uint32_t idx = v & 0x7FFFFFFFu;
  const cand * k = &c->set->items[idx];
  entry_fill(c->set, k, idx, k->kind == CAND_ALL ? c->wins[index].unit : 0, entry);
  if (reason) {
    *reason = (v & 0x80000000u) ? GTEXT_CSS_LOSS_ROLLED_BACK : GTEXT_CSS_LOSS_OUTRANKED;
  }
  return true;
}

static int name_cmp(const char * a, size_t an, const char * b, size_t bn) {
  const size_t n = an < bn ? an : bn;
  const int d = n ? memcmp(a, b, n) : 0;
  if (d) {
    return d < 0 ? -1 : 1;
  }
  return an < bn ? -1 : an > bn ? 1 : 0;
}

bool gtext_css_cascaded_find(const GTEXT_CSS_Cascaded * c, GTEXT_CSS_Property_Id property, const char * custom_name, size_t custom_length,
    GTEXT_CSS_Cascade_Entry * entry) {
  if (!c) {
    return false;
  }
  size_t lo, hi;
  if (custom_name) {
    lo = c->nreg;
    hi = c->nwins;
    while (lo < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      const cand * k = &c->set->items[c->wins[mid].entry];
      const int d = name_cmp(cand_name(c->set, k), k->name_len, custom_name, custom_length);
      if (d == 0) {
        if (entry) {
          win_entry(c, &c->wins[mid], entry);
        }
        return true;
      }
      if (d < 0) {
        lo = mid + 1;
      }
      else {
        hi = mid;
      }
    }
    return false;
  }
  GTEXT_CSS_Property_Info info;
  if (!property || !gtext_css_registry_property_info(c->set->registry, property, &info)) {
    return false;
  }
  GTEXT_CSS_Property_Id unit = property;
  if (info.logical_group) {
    GTEXT_CSS_Property_Id physical;
    if (gtext_css_logical_map(c->set->registry, property, &c->flow, &physical) == GTEXT_CSS_OK) {
      unit = physical;
    }
  }
  lo = 0;
  hi = c->nreg;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (c->wins[mid].unit == unit) {
      if (entry) {
        win_entry(c, &c->wins[mid], entry);
      }
      return true;
    }
    if (c->wins[mid].unit < unit) {
      lo = mid + 1;
    }
    else {
      hi = mid;
    }
  }
  return false;
}

/* ---- cascading ------------------------------------------------------------------------------------------------ */

static bool kw_is(const GTEXT_CSS_Value * v, const char * word) {
  if (!v || v->kind != GTEXT_CSS_VAL_KEYWORD || !v->text.data) {
    return false;
  }
  const size_t n = strlen(word);
  if (v->text.length != n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    char a = v->text.data[i];
    if (a >= 'A' && a <= 'Z') {
      a = (char)(a - 'A' + 'a');
    }
    if (a != word[i]) {
      return false;
    }
  }
  return true;
}

/// The value of one of the three flow properties from its winner (CASCADE_NONE: none); `which` 0 writing-mode, 1 direction, 2 text-orientation.
static int flow_value(const cascade_run * run, uint32_t winner, int which, const GTEXT_CSS_Flow * parent) {
  int inherited = which == 0 ? (int)parent->writing_mode : which == 1 ? (int)parent->direction : (int)parent->text_orientation;
  if (winner == CASCADE_NONE) {
    return inherited;
  }
  const cand * c = &run->c[winner];
  if (c->outcome == GTEXT_CSS_PROP_RESULT_WIDE) {
    return c->wide == GTEXT_CSS_WIDE_INITIAL ? 0 : inherited;
  }
  if (c->outcome != GTEXT_CSS_PROP_RESULT_VALID) {
    return inherited; /* pending: invalid at computed-value time is `unset`, and these properties inherit */
  }
  const GTEXT_CSS_Value * v = gtext_css_value_set_root(c->set);
  if (which == 0) {
    if (kw_is(v, "vertical-rl")) return GTEXT_CSS_WRITING_MODE_VERTICAL_RL;
    if (kw_is(v, "vertical-lr")) return GTEXT_CSS_WRITING_MODE_VERTICAL_LR;
    if (kw_is(v, "sideways-rl")) return GTEXT_CSS_WRITING_MODE_SIDEWAYS_RL;
    if (kw_is(v, "sideways-lr")) return GTEXT_CSS_WRITING_MODE_SIDEWAYS_LR;
    return GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB;
  }
  if (which == 1) {
    return kw_is(v, "rtl") ? GTEXT_CSS_DIRECTION_RTL : GTEXT_CSS_DIRECTION_LTR;
  }
  if (kw_is(v, "upright")) return GTEXT_CSS_TEXT_ORIENTATION_UPRIGHT;
  if (kw_is(v, "sideways")) return GTEXT_CSS_TEXT_ORIENTATION_SIDEWAYS;
  return GTEXT_CSS_TEXT_ORIENTATION_MIXED;
}

typedef struct {
  cascade_run run;
  const GTEXT_CSS_Registry * reg;
  size_t nprops;
  GTEXT_CSS_Flow flow;
  uint32_t * unit_cache;     ///< unit_cache[p - 1]: the unit of property p, 0 for not yet known.
  GTEXT_CSS_Property_Id wm, dir, to, bidi;
} state;

static uint32_t unit_of(state * st, GTEXT_CSS_Property_Id p) {
  if (p == 0 || p > st->nprops) {
    return p;
  }
  uint32_t u = st->unit_cache[p - 1];
  if (u) {
    return u;
  }
  u = p;
  GTEXT_CSS_Property_Info info;
  if (gtext_css_registry_property_info(st->reg, p, &info) && info.logical_group) {
    GTEXT_CSS_Property_Id physical;
    if (gtext_css_logical_map(st->reg, p, &st->flow, &physical) == GTEXT_CSS_OK) {
      u = physical;
    }
  }
  st->unit_cache[p - 1] = u;
  return u;
}

static bool all_applies(const state * st, uint32_t unit) {
  return unit != st->dir && unit != st->bidi;
}

/// Whether property `p` is a unit `all` can win: a longhand that is its own slot.
static bool all_unit(state * st, GTEXT_CSS_Property_Id p) {
  GTEXT_CSS_Property_Info info;
  if (!gtext_css_registry_property_info(st->reg, p, &info)) {
    return false;
  }
  if (info.flags & (GTEXT_CSS_PROP_SHORTHAND | GTEXT_CSS_PROP_ALIAS | GTEXT_CSS_PROP_CUSTOM)) {
    return false;
  }
  return unit_of(st, p) == p && all_applies(st, p);
}

typedef struct {
  const cand * c;
  const char * names;
  const win * wins;
} name_ctx;

/// Compare two winners (indexes into the winners) of custom properties by name.
static int by_custom_name(const void * ctx, uint32_t x, uint32_t y) {
  const name_ctx * n = ctx;
  const cand * a = &n->c[n->wins[x].entry];
  const cand * b = &n->c[n->wins[y].entry];
  return name_cmp(n->names + a->name_off, a->name_len, n->names + b->name_off, b->name_len);
}

static uint64_t mix64(uint64_t x) {
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ULL;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBULL;
  x ^= x >> 31;
  return x;
}

static uint64_t name_hash(uint64_t seed, const char * s, size_t n) {
  uint64_t h = 0xCBF29CE484222325ULL ^ mix64(seed);
  for (size_t i = 0; i < n; i++) {
    h ^= (unsigned char)s[i];
    h *= 0x100000001B3ULL;
  }
  return mix64(h ^ seed);
}

#define FAIL_OOM(r) result_done((r), GTEXT_CSS_E_OOM)

GTEXT_CSS_Status gtext_css_cascade(GTEXT_CSS_Candidates * set, const GTEXT_CSS_Cascade_Options * options, GTEXT_CSS_Cascaded ** out,
    GTEXT_CSS_Cascade_Result * result) {
  result_clear(result);
  if (!set || !out) {
    return result_done(result, GTEXT_CSS_E_INVALID);
  }
  GTEXT_CSS_Cascade_Options opt;
  memset(&opt, 0, sizeof opt);
  if (options) {
    opt = *options;
  }
  gcu_atomic_int_store(&set->sealed, 1);
  const GTEXT_CSS_Registry * reg = set->registry;
  GTEXT_Allocator alloc = opt.allocator ? *opt.allocator : set->alloc;
  state st;
  memset(&st, 0, sizeof st);
  st.reg = reg;
  st.nprops = gtext_css_registry_property_count(reg);
  st.run.set = set;
  st.run.c = set->items;
  st.run.n = set->count;
  st.run.alloc = alloc;
  const cand * C = set->items;
  const size_t n = set->count;
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  GTEXT_CSS_Cascaded * res = NULL;
  uint32_t *ent_unit = NULL, *ustart = NULL, *uitems = NULL, *fl = NULL, *ctable = NULL, *cwin = NULL, *cwin_tmp = NULL;
  win * wins = NULL;
  size_t nwins = 0, nreg = 0;
  size_t *lstart = NULL;
  uint32_t * litems = NULL;
  size_t ncustom = 0;

  st.unit_cache = gtext_allocator_calloc(&alloc, st.nprops + 1, sizeof *st.unit_cache);
  ent_unit = gtext_allocator_calloc(&alloc, n + 1, sizeof *ent_unit);
  st.run.all = gtext_allocator_calloc(&alloc, n + 1, sizeof *st.run.all);
  if (!st.unit_cache || !ent_unit || !st.run.all) {
    status = GTEXT_CSS_E_OOM;
    goto done;
  }
  st.wm = gtext_css_registry_find_property(reg, "writing-mode", 12);
  st.dir = gtext_css_registry_find_property(reg, "direction", 9);
  st.to = gtext_css_registry_find_property(reg, "text-orientation", 16);
  st.bidi = gtext_css_registry_find_property(reg, "unicode-bidi", 12);
  for (size_t i = 0; i < n; i++) {
    if (C[i].kind == CAND_ALL) {
      st.run.all[st.run.nall++] = (uint32_t)i;
    }
    else if (C[i].kind == CAND_CUSTOM) {
      ncustom++;
    }
  }

  /* the flow first: a logical property is mapped by it */
  GTEXT_CSS_Flow parent;
  memset(&parent, 0, sizeof parent);
  if (opt.parent_flow) {
    parent = *opt.parent_flow;
  }
  st.flow = parent;
  if (opt.flow) {
    st.flow = *opt.flow;
  }
  else if (n && st.wm && st.dir && st.to) {
    const GTEXT_CSS_Property_Id fp[3] = {st.wm, st.dir, st.to};
    size_t counts[3] = {0, 0, 0}, total = 0;
    for (size_t i = 0; i < n; i++) {
      if (C[i].kind == CAND_PROPERTY) {
        for (int f = 0; f < 3; f++) {
          if (C[i].property == fp[f]) {
            counts[f]++;
            total++;
          }
        }
      }
    }
    if (total || st.run.nall) {
      fl = gtext_allocator_calloc(&alloc, total + 1, sizeof *fl);
      if (!fl) {
        status = GTEXT_CSS_E_OOM;
        goto done;
      }
      size_t at[3] = {0, counts[0], counts[0] + counts[1]}, begin[3] = {0, counts[0], counts[0] + counts[1]};
      for (size_t i = 0; i < n; i++) {
        if (C[i].kind == CAND_PROPERTY) {
          for (int f = 0; f < 3; f++) {
            if (C[i].property == fp[f]) {
              fl[at[f]++] = (uint32_t)i;
            }
          }
        }
      }
      int v[3];
      for (int f = 0; f < 3; f++) {
        uint32_t w, rb;
        status = cascade_resolve(&st.run, fl + begin[f], counts[f], all_applies(&st, fp[f]), &w, &rb);
        if (status != GTEXT_CSS_OK) {
          goto done;
        }
        v[f] = flow_value(&st.run, w, f, &parent);
      }
      st.flow.writing_mode = (GTEXT_CSS_Writing_Mode)v[0];
      st.flow.direction = (GTEXT_CSS_Direction)v[1];
      st.flow.text_orientation = (GTEXT_CSS_Text_Orientation)v[2];
    }
  }

  /* the unit of each candidate: the physical property of its slot, or the custom property's number */
  size_t nunits = st.nprops;
  if (ncustom) {
    size_t cap = 16;
    while (cap < 2 * ncustom) {
      cap *= 2;
    }
    ctable = gtext_allocator_calloc(&alloc, cap, sizeof *ctable);
    if (!ctable) {
      status = GTEXT_CSS_E_OOM;
      goto done;
    }
    uint64_t seed = opt.hash_seed;
    if (seed == 0) {
      seed = (uint64_t)(uintptr_t)&st * 0x9E3779B97F4A7C15ULL;
      seed ^= (uint64_t)(uintptr_t)set * 0xC2B2AE3D27D4EB4FULL;
      seed ^= seed >> 29;
      if (seed == 0) {
        seed = 0x5851F42D4C957F2DULL;
      }
    }
    for (size_t i = 0; i < n; i++) {
      if (C[i].kind != CAND_CUSTOM) {
        continue;
      }
      const char * nm = cand_name(set, &C[i]);
      size_t h = (size_t)(name_hash(seed, nm, C[i].name_len) & (cap - 1));
      for (;;) {
        st.run.work++;
        const uint32_t at = ctable[h];
        if (!at) {
          ctable[h] = (uint32_t)i + 1;
          ent_unit[i] = (uint32_t)(nunits++);
          break;
        }
        const cand * o = &C[at - 1];
        if (o->name_len == C[i].name_len && memcmp(cand_name(set, o), nm, C[i].name_len) == 0) {
          ent_unit[i] = ent_unit[at - 1];
          break;
        }
        h = (h + 1) & (cap - 1);
      }
    }
  }
  for (size_t i = 0; i < n; i++) {
    st.run.work++;
    if (C[i].kind == CAND_PROPERTY) {
      ent_unit[i] = unit_of(&st, C[i].property) - 1;
    }
  }
  /* group the candidates by unit (a counting sort: stable, so each group is in the order added) */
  ustart = gtext_allocator_calloc(&alloc, nunits + 2, sizeof *ustart);
  uitems = gtext_allocator_calloc(&alloc, n + 1, sizeof *uitems);
  if (!ustart || !uitems) {
    status = GTEXT_CSS_E_OOM;
    goto done;
  }
  for (size_t i = 0; i < n; i++) {
    if (C[i].kind != CAND_ALL) {
      ustart[ent_unit[i] + 1]++;
    }
  }
  for (size_t u = 0; u < nunits; u++) {
    ustart[u + 1] += ustart[u];
  }
  {
    uint32_t * fill = gtext_allocator_malloc(&alloc, (nunits + 1) * sizeof *fill);
    if (!fill) {
      status = GTEXT_CSS_E_OOM;
      goto done;
    }
    memcpy(fill, ustart, (nunits + 1) * sizeof *fill);
    for (size_t i = 0; i < n; i++) {
      if (C[i].kind != CAND_ALL) {
        uitems[fill[ent_unit[i]]++] = (uint32_t)i;
      }
    }
    gtext_allocator_free(&alloc, fill);
  }

  /* decide each unit */
  wins = gtext_allocator_calloc(&alloc, nunits + 1, sizeof *wins);
  cwin = gtext_allocator_calloc(&alloc, ncustom + 1, sizeof *cwin);
  cwin_tmp = gtext_allocator_calloc(&alloc, ncustom + 1, sizeof *cwin_tmp);
  if (!wins || !cwin || !cwin_tmp) {
    status = GTEXT_CSS_E_OOM;
    goto done;
  }
  const bool any_all = st.run.nall > 0;
  for (size_t u = 0; u < st.nprops; u++) {
    const uint32_t own_n = ustart[u + 1] - ustart[u];
    const GTEXT_CSS_Property_Id p = (GTEXT_CSS_Property_Id)(u + 1);
    st.run.work++;
    const bool use_all = any_all && all_applies(&st, p);
    if (own_n == 0 && !(use_all && all_unit(&st, p))) {
      continue;
    }
    uint32_t w, rb;
    status = cascade_resolve(&st.run, uitems + ustart[u], own_n, use_all, &w, &rb);
    if (status != GTEXT_CSS_OK) {
      goto done;
    }
    if (w != CASCADE_NONE) {
      wins[nwins].unit = p;
      wins[nwins].entry = w;
      wins[nwins].rollbacks = rb;
      nwins++;
    }
  }
  nreg = nwins;
  size_t ncw = 0;
  for (size_t u = st.nprops; u < nunits; u++) {
    uint32_t w, rb;
    status = cascade_resolve(&st.run, uitems + ustart[u], ustart[u + 1] - ustart[u], false, &w, &rb);
    if (status != GTEXT_CSS_OK) {
      goto done;
    }
    if (w != CASCADE_NONE) {
      wins[nwins].unit = 0;
      wins[nwins].entry = w;
      wins[nwins].rollbacks = rb;
      cwin[ncw++] = (uint32_t)nwins;
      nwins++;
    }
  }
  /* the custom properties in the order of their names, so that the output does not depend on the order the candidates came in */
  if (ncw > 1) {
    win * copy = gtext_allocator_malloc(&alloc, ncw * sizeof *copy);
    if (!copy) {
      status = GTEXT_CSS_E_OOM;
      goto done;
    }
    memcpy(copy, wins + nreg, ncw * sizeof *copy);
    name_ctx nc = {C, set->names, wins};
    cascade_msort(cwin, cwin_tmp, ncw, by_custom_name, &nc, &st.run.sort_work);
    for (size_t i = 0; i < ncw; i++) {
      wins[nreg + i] = copy[cwin[i] - nreg];
    }
    gtext_allocator_free(&alloc, copy);
  }

  /* the candidates that lost, when asked for */
  if (opt.keep_losers && nwins) {
    lstart = gtext_allocator_calloc(&alloc, nwins + 1, sizeof *lstart);
    if (!lstart) {
      status = GTEXT_CSS_E_OOM;
      goto done;
    }
    size_t cap = 0;
    for (size_t i = 0; i < nwins; i++) {
      const win * w = &wins[i];
      const cand * wc = &C[w->entry];
      uint32_t unit_index = w->unit ? w->unit - 1 : 0;
      const uint32_t * own;
      size_t own_n;
      bool use_all;
      if (w->unit) {
        own = uitems + ustart[unit_index];
        own_n = ustart[unit_index + 1] - ustart[unit_index];
        use_all = any_all && all_applies(&st, w->unit);
      }
      else {
        unit_index = ent_unit[w->entry];
        own = uitems + ustart[unit_index];
        own_n = ustart[unit_index + 1] - ustart[unit_index];
        use_all = false;
      }
      (void)wc;
      uint32_t * sorted = NULL;
      size_t sn = 0;
      status = cascade_sorted(&st.run, own, own_n, use_all, &sorted, &sn);
      if (status != GTEXT_CSS_OK) {
        goto done;
      }
      if (lstart[i] + sn > cap) {
        size_t ncap = cap ? cap * 2 : 64;
        while (ncap < lstart[i] + sn) {
          ncap *= 2;
        }
        uint32_t * g = gtext_allocator_realloc(&alloc, litems, ncap * sizeof *g);
        if (!g) {
          gtext_allocator_free(&alloc, sorted);
          status = GTEXT_CSS_E_OOM;
          goto done;
        }
        litems = g;
        cap = ncap;
      }
      size_t pos = 0, outn = 0;
      bool seen = false;
      for (; pos < sn; pos++) {
        if (sorted[pos] == w->entry) {
          seen = true;
          continue;
        }
        litems[lstart[i] + outn++] = sorted[pos] | (seen ? 0u : 0x80000000u);
      }
      gtext_allocator_free(&alloc, sorted);
      lstart[i + 1] = lstart[i] + outn;
    }
  }

  /* the result owns what it needs and a reference to the candidates */
  res = gtext_allocator_calloc(&alloc, 1, sizeof *res);
  if (!res) {
    status = GTEXT_CSS_E_OOM;
    goto done;
  }
  res->alloc = alloc;
  res->set = gtext_css_candidates_retain(set);
  res->flow = st.flow;
  res->wins = wins;
  res->nwins = nwins;
  res->nreg = nreg;
  res->lstart = lstart;
  res->litems = litems;
  wins = NULL;
  lstart = NULL;
  litems = NULL;
  *out = res;
  res = NULL;

done:
  if (result) {
    result->work = st.run.work + st.run.sort_work;
  }
  gtext_allocator_free(&alloc, st.unit_cache);
  gtext_allocator_free(&alloc, ent_unit);
  gtext_allocator_free(&alloc, st.run.all);
  gtext_allocator_free(&alloc, st.run.all_sorted);
  gtext_allocator_free(&alloc, st.run.buf_a);
  gtext_allocator_free(&alloc, st.run.buf_b);
  gtext_allocator_free(&alloc, st.run.skip);
  gtext_allocator_free(&alloc, ustart);
  gtext_allocator_free(&alloc, uitems);
  gtext_allocator_free(&alloc, fl);
  gtext_allocator_free(&alloc, ctable);
  gtext_allocator_free(&alloc, cwin);
  gtext_allocator_free(&alloc, cwin_tmp);
  gtext_allocator_free(&alloc, wins);
  gtext_allocator_free(&alloc, lstart);
  gtext_allocator_free(&alloc, litems);
  if (status == GTEXT_CSS_E_OOM) {
    return FAIL_OOM(result);
  }
  return result_done(result, status);
}

/* ---- the block ------------------------------------------------------------------------------------------------ */

GTEXT_CSS_Status gtext_css_cascaded_to_block(const GTEXT_CSS_Cascaded * c, const GTEXT_CSS_Declaration_Options * options,
    GTEXT_CSS_Declaration_Block ** out) {
  if (!c || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Declaration_Block * block = NULL;
  GTEXT_CSS_Status st = gtext_css_block_new(c->set->registry, options, &block);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  for (size_t i = 0; i < c->nwins && st == GTEXT_CSS_OK; i++) {
    const win * w = &c->wins[i];
    const cand * k = &c->set->items[w->entry];
    const GTEXT_CSS_Property_Id property = w->unit ? (k->kind == CAND_ALL ? w->unit : k->property) : k->property;
    st = css_block_push(block, property, k->kind == CAND_CUSTOM ? cand_name(c->set, k) : NULL, k->kind == CAND_CUSTOM ? k->name_len : 0, k->important,
        (GTEXT_CSS_Property_Outcome)k->outcome, (GTEXT_CSS_Wide_Keyword)k->wide, k->set, k->shorthand, k->group);
  }
  if (st != GTEXT_CSS_OK) {
    gtext_css_block_free(block);
    return st;
  }
  *out = block;
  return GTEXT_CSS_OK;
}

/* ---- the dump ------------------------------------------------------------------------------------------------- */

typedef struct {
  GTEXT_Allocator alloc;
  char * data;
  size_t len, cap;
  bool failed;
} dbuf;

static void dput(dbuf * b, const char * s, size_t n) {
  if (b->failed) {
    return;
  }
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + n + 1) {
      cap *= 2;
    }
    char * g = gtext_allocator_realloc(&b->alloc, b->data, cap);
    if (!g) {
      b->failed = true;
      return;
    }
    b->data = g;
    b->cap = cap;
  }
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
}

static void dputs(dbuf * b, const char * s) {
  dput(b, s, strlen(s));
}

static void dnum(dbuf * b, unsigned long long v) {
  char t[24];
  size_t i = sizeof t;
  do {
    t[--i] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  dput(b, t + i, sizeof t - i);
}

static GTEXT_CSS_Status dump_to_buffer(const GTEXT_CSS_Cascaded * c, dbuf * b) {
  memset(b, 0, sizeof *b);
  b->alloc = c->alloc;
  const GTEXT_CSS_Candidates * s = c->set;
  dputs(b, "cascaded 1\n");
  for (size_t i = 0; i < c->nwins && !b->failed; i++) {
    const win * w = &c->wins[i];
    const cand * k = &s->items[w->entry];
    GTEXT_CSS_Property_Info info;
    if (k->kind == CAND_CUSTOM) {
      dput(b, cand_name(s, k), k->name_len);
    }
    else if (gtext_css_registry_property_info(s->registry, k->kind == CAND_ALL ? w->unit : k->property, &info)) {
      dput(b, info.name, info.name_length);
    }
    dputs(b, ": ");
    if (k->kind == CAND_CUSTOM && k->outcome == GTEXT_CSS_PROP_RESULT_VALID) {
      const GTEXT_CSS_Value * root = gtext_css_value_set_root(k->set);
      if (root && root->text.data) {
        dput(b, root->text.data, root->text.length);
      }
    }
    else {
      char * text = NULL;
      size_t tn = 0;
      const GTEXT_CSS_Status st = css_item_text(s->registry, &c->alloc, k->kind == CAND_ALL ? w->unit : k->property, (GTEXT_CSS_Property_Outcome)k->outcome,
          (GTEXT_CSS_Wide_Keyword)k->wide, k->set, &text, &tn);
      if (st != GTEXT_CSS_OK) {
        gtext_allocator_free(&b->alloc, b->data);
        return st;
      }
      dput(b, text, tn);
      GTEXT_Allocator a = c->alloc;
      gtext_allocator_free(&a, text);
    }
    if (k->important) {
      dputs(b, " !important");
    }
    dputs(b, " [");
    dputs(b, gtext_css_origin_name((GTEXT_CSS_Origin)k->origin));
    if (k->hint) {
      dputs(b, " hint");
    }
    if (k->attached) {
      dputs(b, " attached");
    }
    if (k->kind == CAND_ALL) {
      dputs(b, " all");
    }
    dputs(b, " layer=");
    if (k->layer && layers_for(s, k->context)) {
      char path[256];
      const size_t pn = gtext_css_layer_path(layers_for(s, k->context), k->layer, path, sizeof path);
      dput(b, path, pn < sizeof path ? pn : sizeof path - 1);
    }
    else {
      dputs(b, "-");
    }
    dputs(b, " ctx=");
    dnum(b, k->context);
    dputs(b, " spec=");
    dnum(b, k->specificity.a);
    dputs(b, ",");
    dnum(b, k->specificity.b);
    dputs(b, ",");
    dnum(b, k->specificity.c);
    dputs(b, " scope=");
    if (k->scoped) {
      dnum(b, k->scope_distance);
    }
    else {
      dputs(b, "-");
    }
    dputs(b, " order=");
    dnum(b, k->sheet);
    dputs(b, ".");
    dnum(b, k->rule);
    dputs(b, ".");
    dnum(b, k->declaration);
    dputs(b, " rb=");
    dnum(b, w->rollbacks);
    dputs(b, "]\n");
  }
  if (b->failed) {
    gtext_allocator_free(&b->alloc, b->data);
    return GTEXT_CSS_E_OOM;
  }
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_cascaded_dump(const GTEXT_CSS_Cascaded * c, const GTEXT_Markup_Sink * sink) {
  if (!c || !sink || !sink->write) {
    return GTEXT_CSS_E_INVALID;
  }
  dbuf b;
  GTEXT_CSS_Status st = dump_to_buffer(c, &b);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  const GTEXT_Markup_Status rc = sink->write(sink->user, b.data, b.len);
  gtext_allocator_free(&b.alloc, b.data);
  return rc != GTEXT_MARKUP_OK ? GTEXT_CSS_E_WRITE : GTEXT_CSS_OK;
}

GTEXT_CSS_Status gtext_css_cascaded_hash(const GTEXT_CSS_Cascaded * c, uint64_t * hash) {
  if (!c || !hash) {
    return GTEXT_CSS_E_INVALID;
  }
  dbuf b;
  GTEXT_CSS_Status st = dump_to_buffer(c, &b);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  uint64_t h = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < b.len; i++) {
    h ^= (unsigned char)b.data[i];
    h *= 0x100000001b3ULL;
  }
  gtext_allocator_free(&b.alloc, b.data);
  *hash = h;
  return GTEXT_CSS_OK;
}
