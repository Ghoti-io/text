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
 * The values of custom properties (css_custom.h), the registrations a context computes with, and a style's access to both.
 */

#include <ghoti.io/text/css/css_custom.h>

#include <stdlib.h>
#include <string.h>

#include "css_substitute_internal.h"

/* ---- a value ----------------------------------------------------------------------------------------- */

GTEXT_CSS_Custom_Value * css_cval_new(const GTEXT_Allocator * alloc, css_rope * rope) {
  GTEXT_CSS_Custom_Value * v = gtext_allocator_calloc((GTEXT_Allocator *)alloc, 1, sizeof *v);
  if (!v) {
    return NULL;
  }
  gcu_atomic_int_init(&v->refs, 1);
  v->alloc = *alloc;
  v->rope = css_rope_retain(rope);
  return v;
}

css_rope * css_cval_rope(const GTEXT_CSS_Custom_Value * v) {
  return v ? v->rope : NULL;
}

GTEXT_CSS_Status css_rope_from_text(const GTEXT_Allocator * alloc, const char * text, size_t length, bool tainted, css_rope ** out) {
  *out = NULL;
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  po.allocator = alloc;
  po.count_errors_only = true;
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Status st = gtext_css_parse_component_values(text, length, &po, &tree, NULL);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  uint64_t tokens = 0;
  css_tokclass first = CSS_TC_SAFE, last = CSS_TC_SAFE;
  if (tree->value_count) {
    st = css_cv_count_tokens(alloc, tree->values, tree->value_count, &tokens);
    if (st == GTEXT_CSS_OK) {
      const GTEXT_CSS_Component_Value * a = &tree->values[0];
      const GTEXT_CSS_Component_Value * b = &tree->values[tree->value_count - 1];
      first = a->span.offset > 0 ? CSS_TC_SAFE : css_cv_first_class(a);
      last = b->span.end < length ? CSS_TC_SAFE : css_cv_last_class(b);
    }
  }
  gtext_css_tree_free(tree);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  *out = css_rope_leaf(alloc, text, length, first, last, tokens, tainted);
  return *out ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
}

GTEXT_CSS_Status gtext_css_custom_value_new(const GTEXT_Allocator * allocator, const char * text, size_t length, GTEXT_CSS_Custom_Value ** out) {
  if (!out || (!text && length)) {
    return GTEXT_CSS_E_INVALID;
  }
  const GTEXT_Allocator * a = allocator ? allocator : gtext_allocator_default();
  css_rope * r = NULL;
  GTEXT_CSS_Status st = css_rope_from_text(a, text ? text : "", length, false, &r);
  if (st != GTEXT_CSS_OK) {
    return st == GTEXT_CSS_E_OOM ? st : GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Custom_Value * v = css_cval_new(a, r);
  css_rope_release(r);
  if (!v) {
    return GTEXT_CSS_E_OOM;
  }
  *out = v;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Custom_Value * gtext_css_custom_value_retain(const GTEXT_CSS_Custom_Value * value) {
  GTEXT_CSS_Custom_Value * v = (GTEXT_CSS_Custom_Value *)value;
  if (v) {
    (void)gcu_atomic_int_fetch_add(&v->refs, 1);
  }
  return v;
}

void gtext_css_custom_value_free(GTEXT_CSS_Custom_Value * v) {
  if (!v || gcu_atomic_int_fetch_sub(&v->refs, 1) != 1) {
    return;
  }
  GTEXT_Allocator a = v->alloc;
  css_rope_release(v->rope);
  gtext_allocator_free(&a, v);
}

static size_t to_size(uint64_t v) {
  return v > SIZE_MAX ? SIZE_MAX : (size_t)v;
}

size_t gtext_css_custom_value_length(const GTEXT_CSS_Custom_Value * v) {
  return v ? to_size(css_rope_bytes(v->rope)) : 0;
}

size_t gtext_css_custom_value_tokens(const GTEXT_CSS_Custom_Value * v) {
  return v ? to_size(css_rope_tokens(v->rope)) : 0;
}

bool gtext_css_custom_value_tainted(const GTEXT_CSS_Custom_Value * v) {
  return v && css_rope_has_taint(v->rope);
}

static bool sink_write(void * user, const char * bytes, size_t n) {
  const GTEXT_Markup_Sink * sink = user;
  return sink->write(sink->user, bytes, n) == GTEXT_MARKUP_OK;
}

GTEXT_CSS_Status gtext_css_custom_value_write(const GTEXT_CSS_Custom_Value * v, const GTEXT_Markup_Sink * sink) {
  if (!v || !sink || !sink->write) {
    return GTEXT_CSS_E_INVALID;
  }
  return css_rope_write(&v->alloc, v->rope, sink_write, (void *)sink, NULL);
}

GTEXT_CSS_Status gtext_css_custom_value_text(const GTEXT_CSS_Custom_Value * v, const GTEXT_CSS_Serialize_Options * options, char ** out, size_t * out_length) {
  if (!v || !out || !out_length) {
    return GTEXT_CSS_E_INVALID;
  }
  const GTEXT_Allocator * a = options && options->allocator ? options->allocator : gtext_allocator_default();
  const size_t max = options && options->max_output_bytes ? options->max_output_bytes : ((size_t)256 << 20);
  return css_rope_flatten(a, v->rope, max, out, out_length, NULL, NULL, NULL);
}

/* ---- the registrations of a context --------------------------------------------------------------------- */

static int name_cmp(const char * a, size_t al, const char * b, size_t bl) {
  const size_t n = al < bl ? al : bl;
  const int c = n ? memcmp(a, b, n) : 0;
  if (c) {
    return c < 0 ? -1 : 1;
  }
  return al < bl ? -1 : al > bl ? 1 : 0;
}

static int ereg_cmp(const void * x, const void * y) {
  const css_ereg * a = x;
  const css_ereg * b = y;
  return name_cmp(a->creg->name, a->creg->name_len, b->creg->name, b->creg->name_len);
}

const css_ereg * css_ctx_find_reg(const GTEXT_CSS_Compute_Context * cc, const char * name, size_t length, size_t * index) {
  size_t lo = 0, hi = cc->nregs;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const css_ereg * e = &cc->regs[mid];
    const int c = name_cmp(name, length, e->creg->name, e->creg->name_len);
    if (c == 0) {
      if (index) {
        *index = mid;
      }
      return e;
    }
    if (c < 0) {
      hi = mid;
    }
    else {
      lo = mid + 1;
    }
  }
  return NULL;
}

void css_ctx_free_regs(GTEXT_CSS_Compute_Context * cc) {
  css_regvals_release(cc->initial_regvals);
  cc->initial_regvals = NULL;
  for (size_t i = 0; i < cc->nregs; i++) {
    gtext_css_custom_value_free(cc->regs[i].initial);
  }
  GTEXT_Allocator a = cc->alloc;
  gtext_allocator_free(&a, cc->regs);
  cc->regs = NULL;
  cc->nregs = 0;
}

GTEXT_CSS_Status css_rope_from_list(const GTEXT_Allocator * alloc, const GTEXT_CSS_Component_Value * values, size_t count, bool tainted, css_rope ** out) {
  /* identifiers separated by a space, or by a comma and a space: how a computed list is written (the white space and comments of the source are not kept) */
  GTEXT_Allocator a = *alloc;
  size_t cap = 64, len = 0;
  char * text = gtext_allocator_malloc(&a, cap);
  if (!text) {
    return GTEXT_CSS_E_OOM;
  }
  bool need_space = false;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
  so.allocator = alloc;
  for (size_t i = 0; i < count && st == GTEXT_CSS_OK; i++) {
    const GTEXT_CSS_Component_Value * v = &values[i];
    if (v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_WHITESPACE) {
      continue;
    }
    char * piece = NULL;
    size_t pl = 0;
    const char * lit = NULL;
    if (v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_COMMA) {
      lit = ",";
      pl = 1;
      need_space = false;
    }
    else {
      st = gtext_css_serialize_values_buffer(v, 1, &so, &piece, &pl);
      if (st != GTEXT_CSS_OK) {
        break;
      }
      lit = piece;
    }
    const size_t need = len + pl + 3;
    if (need > cap) {
      while (cap < need) {
        cap *= 2;
      }
      char * g = gtext_allocator_realloc(&a, text, cap);
      if (!g) {
        st = GTEXT_CSS_E_OOM;
        gtext_css_serialize_buffer_free(&so, piece);
        break;
      }
      text = g;
    }
    if (need_space) {
      text[len++] = ' ';
    }
    memcpy(text + len, lit, pl);
    len += pl;
    need_space = piece != NULL;
    if (!piece) {
      text[len++] = ' ';
    }
    gtext_css_serialize_buffer_free(&so, piece);
  }
  while (len && text[len - 1] == ' ') {
    len--;
  }
  if (st == GTEXT_CSS_OK) {
    st = css_rope_from_text(alloc, text, len, tainted, out);
  }
  gtext_allocator_free(&a, text);
  return st;
}

GTEXT_CSS_Status css_ereg_initial(css_state * st, const css_creg * creg, GTEXT_CSS_Custom_Value ** out) {
  *out = NULL;
  if (!creg->has_initial) {
    return GTEXT_CSS_OK;
  }
  const GTEXT_Allocator * alloc = &st->cc->alloc;
  css_rope * rope = NULL;
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  if (creg->syntax->universal) {
    status = css_rope_from_text(alloc, creg->initial, creg->initial_len, false, &rope);
  }
  else {
    GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
    po.allocator = alloc;
    GTEXT_CSS_Tree * tree = NULL;
    status = gtext_css_parse_component_values(creg->initial, creg->initial_len, &po, &tree, NULL);
    if (status != GTEXT_CSS_OK) {
      return status;
    }
    GTEXT_CSS_Match_Options mo = gtext_css_match_options_default();
    mo.allocator = alloc;
    bool matched = false;
    css_syn_match m;
    status = css_syntax_match(creg->syntax, creg->compiled, tree->values, tree->value_count, &mo, &matched, &m);
    if (status == GTEXT_CSS_OK && !matched) {
      status = GTEXT_CSS_E_INTERNAL; /* it matched when it was registered */
    }
    if (status == GTEXT_CSS_OK) {
      GTEXT_CSS_Value_Set * computed = NULL;
      if (m.set) {
        status = css_cv_by_type_with(st, m.set, creg->compiled ? creg->compiled[m.component] : NULL, 0, &computed);
      }
      if (status == GTEXT_CSS_OK) {
        const GTEXT_CSS_Value * root = computed ? gtext_css_value_set_root(computed) : NULL;
        if (root) {
          status = css_registered_rope(st, alloc, root, false, &rope);
        }
        else {
          status = css_rope_from_list(alloc, tree->values, tree->value_count, false, &rope);
        }
      }
      gtext_css_value_set_free(computed);
      gtext_css_value_set_free(m.set);
    }
    gtext_css_tree_free(tree);
  }
  if (status != GTEXT_CSS_OK) {
    return status;
  }
  *out = css_cval_new(alloc, rope);
  css_rope_release(rope);
  return *out ? GTEXT_CSS_OK : GTEXT_CSS_E_OOM;
}

struct css_regvals {
  GCU_Atomic_Int refs;
  GTEXT_Allocator alloc;
  size_t n;
  GTEXT_CSS_Custom_Value ** v;  ///< n values (retained), allocated with the struct
};

static css_regvals * regvals_new(const GTEXT_Allocator * alloc, size_t n);

GTEXT_CSS_Status css_ctx_make_regs(GTEXT_CSS_Compute_Context * cc, css_state * st) {
  const size_t from_registry = gtext_css_registry_custom_count(cc->registry);
  const css_creg_store * rules = css_at_property_store(cc->rules);
  const size_t from_rules = rules ? rules->count : 0;
  const size_t total = from_registry + from_rules;
  if (total == 0) {
    return GTEXT_CSS_OK;
  }
  GTEXT_Allocator a = cc->alloc;
  css_ereg * regs = gtext_allocator_calloc(&a, total, sizeof *regs);
  if (!regs) {
    return GTEXT_CSS_E_OOM;
  }
  size_t n = 0;
  for (size_t i = 0; i < from_registry; i++) {
    regs[n++].creg = css_registry_creg_at(cc->registry, i);
  }
  for (size_t i = 0; i < from_rules; i++) {
    const css_creg * c = rules->items[i];
    if (!css_registry_find_creg(cc->registry, c->name, c->name_len, NULL)) { /* the caller's registration wins */
      regs[n++].creg = c;
    }
  }
  qsort(regs, n, sizeof *regs, ereg_cmp);
  cc->regs = regs;
  cc->nregs = n;
  for (size_t i = 0; i < n; i++) {
    const GTEXT_CSS_Status status = css_ereg_initial(st, regs[i].creg, &regs[i].initial);
    if (status != GTEXT_CSS_OK) {
      css_ctx_free_regs(cc);
      return status;
    }
  }
  cc->initial_regvals = regvals_new(&cc->alloc, n);
  if (!cc->initial_regvals) {
    css_ctx_free_regs(cc);
    return GTEXT_CSS_E_OOM;
  }
  for (size_t i = 0; i < n; i++) {
    cc->initial_regvals->v[i] = gtext_css_custom_value_retain(regs[i].initial);
  }
  return GTEXT_CSS_OK;
}

static css_regvals * regvals_new(const GTEXT_Allocator * alloc, size_t n) {
  GTEXT_Allocator a = *alloc;
  css_regvals * r = gtext_allocator_calloc(&a, 1, sizeof *r + (n ? n : 1) * sizeof *r->v);
  if (!r) {
    return NULL;
  }
  gcu_atomic_int_init(&r->refs, 1);
  r->alloc = a;
  r->n = n;
  r->v = (GTEXT_CSS_Custom_Value **)(r + 1);
  return r;
}

static css_regvals * regvals_retain(css_regvals * r) {
  if (r) {
    gcu_atomic_int_fetch_add(&r->refs, 1);
  }
  return r;
}

void css_regvals_release(css_regvals * r) {
  if (!r || gcu_atomic_int_fetch_sub(&r->refs, 1) != 1) {
    return;
  }
  for (size_t i = 0; i < r->n; i++) {
    gtext_css_custom_value_free(r->v[i]);
  }
  GTEXT_Allocator a = r->alloc;
  gtext_allocator_free(&a, r);
}

const GTEXT_CSS_Custom_Value * css_style_regval(const GTEXT_CSS_Computed_Style * style, size_t index) {
  return style->regvals ? style->regvals->v[index] : NULL;
}

GTEXT_CSS_Status css_style_set_regval(GTEXT_CSS_Computed_Style * style, size_t index, const GTEXT_CSS_Custom_Value * value) {
  css_regvals * cur = style->regvals;
  if (!cur) {
    return GTEXT_CSS_E_INTERNAL;
  }
  if (cur->v[index] == value) {
    return GTEXT_CSS_OK;
  }
  if (gcu_atomic_int_load(&cur->refs) > 1) {
    /* copy on write: this style is the only one that will change it (a style under computation is not yet shared) */
    css_regvals * own = regvals_new(&style->alloc, cur->n);
    if (!own) {
      return GTEXT_CSS_E_OOM;
    }
    for (size_t i = 0; i < cur->n; i++) {
      own->v[i] = gtext_css_custom_value_retain(cur->v[i]);
    }
    css_regvals_release(cur);
    style->regvals = own;
    cur = own;
  }
  gtext_css_custom_value_free(cur->v[index]);
  cur->v[index] = gtext_css_custom_value_retain(value);
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_style_init_regvals(GTEXT_CSS_Computed_Style * style, const GTEXT_CSS_Computed_Style * parent) {
  const GTEXT_CSS_Compute_Context * cc = style->cc;
  if (!cc->nregs) {
    return GTEXT_CSS_OK;
  }
  if (parent && parent->regvals) {
    /* the parent's array, where it is what this style would start as: no registered value to change */
    bool same = true;
    for (size_t i = 0; i < cc->nregs && same; i++) {
      const css_ereg * e = &cc->regs[i];
      same = parent->regvals->v[i] == (e->creg->inherits ? parent->regvals->v[i] : e->initial);
    }
    if (same) {
      style->regvals = regvals_retain(parent->regvals);
      return GTEXT_CSS_OK;
    }
  }
  else if (cc->initial_regvals) {
    style->regvals = regvals_retain(cc->initial_regvals);
    return GTEXT_CSS_OK;
  }
  style->regvals = regvals_new(&style->alloc, cc->nregs);
  if (!style->regvals) {
    return GTEXT_CSS_E_OOM;
  }
  for (size_t i = 0; i < cc->nregs; i++) {
    const css_ereg * e = &cc->regs[i];
    const GTEXT_CSS_Custom_Value * v = (e->creg->inherits && parent && parent->regvals) ? parent->regvals->v[i] : e->initial;
    style->regvals->v[i] = gtext_css_custom_value_retain(v);
  }
  return GTEXT_CSS_OK;
}

/* ---- a style's custom properties ---------------------------------------------------------------------- */

static size_t present_regs(const GTEXT_CSS_Computed_Style * style) {
  size_t n = 0;
  if (style->regvals) {
    for (size_t i = 0; i < style->cc->nregs; i++) {
      n += style->regvals->v[i] != NULL;
    }
  }
  return n;
}

size_t gtext_css_computed_custom_count(const GTEXT_CSS_Computed_Style * style) {
  return style ? css_cmap_count(style->custom) + present_regs(style) : 0;
}

bool gtext_css_computed_custom_at(const GTEXT_CSS_Computed_Style * style, size_t index, GTEXT_CSS_String * name, const GTEXT_CSS_Custom_Value ** value) {
  if (!style) {
    return false;
  }
  size_t before = 0; /* the registered values that come before the place `index` asks for */
  size_t k = 0;      /* the registered value being looked at, among those that are present */
  if (style->regvals) {
    for (size_t i = 0; i < style->cc->nregs; i++) {
      if (!style->regvals->v[i]) {
        continue;
      }
      const css_creg * c = style->cc->regs[i].creg;
      const size_t pos = css_cmap_rank(style->custom, c->name, c->name_len) + k;
      if (pos == index) {
        if (name) {
          name->data = c->name;
          name->length = c->name_len;
        }
        if (value) {
          *value = style->regvals->v[i];
        }
        return true;
      }
      if (pos > index) {
        break;
      }
      before++;
      k++;
    }
  }
  const char * n = NULL;
  size_t length = 0;
  if (index < before || !css_cmap_at(style->custom, index - before, &n, &length, value)) {
    return false;
  }
  if (name) {
    name->data = n;
    name->length = length;
  }
  return true;
}

bool gtext_css_computed_custom_find(const GTEXT_CSS_Computed_Style * style, const char * name, size_t length, const GTEXT_CSS_Custom_Value ** value) {
  if (!style || !name) {
    return false;
  }
  size_t index = 0;
  const GTEXT_CSS_Custom_Value * v;
  if (style->cc->nregs && css_ctx_find_reg(style->cc, name, length, &index)) {
    v = css_style_regval(style, index);
  }
  else {
    v = css_cmap_find(style->custom, name, length);
  }
  if (!v) {
    return false;
  }
  if (value) {
    *value = v;
  }
  return true;
}

/* ---- the syntaxes of attr() --------------------------------------------------------------------------------- */

#define SYNTAX_CACHE_MAX 256

GTEXT_CSS_Status css_ctx_syntax(const GTEXT_CSS_Compute_Context * cc, const char * text, size_t length, const css_compiled_syntax ** out, bool * temporary) {
  GTEXT_CSS_Compute_Context * c = (GTEXT_CSS_Compute_Context *)cc;
  *temporary = false;
  GCU_MUTEX_LOCK(c->syntax_lock);
  for (size_t i = 0; i < c->nsyntax; i++) {
    if (c->syntax_cache[i]->len == length && memcmp(c->syntax_cache[i]->text, text, length) == 0) {
      *out = c->syntax_cache[i];
      GCU_MUTEX_UNLOCK(c->syntax_lock);
      return GTEXT_CSS_OK;
    }
  }
  GCU_MUTEX_UNLOCK(c->syntax_lock);
  css_compiled_syntax * made = NULL;
  const GTEXT_CSS_Status st = css_registry_compile_syntax(cc->registry, text, length, &made);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GCU_MUTEX_LOCK(c->syntax_lock);
  for (size_t i = 0; i < c->nsyntax; i++) {
    if (c->syntax_cache[i]->len == length && memcmp(c->syntax_cache[i]->text, text, length) == 0) {
      /* another thread made it first */
      *out = c->syntax_cache[i];
      GCU_MUTEX_UNLOCK(c->syntax_lock);
      css_compiled_syntax_free(made);
      return GTEXT_CSS_OK;
    }
  }
  if (c->nsyntax < SYNTAX_CACHE_MAX && length <= 4096 && c->syntax_bytes + length <= ((size_t)1 << 20)) {
    if (c->nsyntax == c->capsyntax) {
      GTEXT_Allocator a = c->alloc;
      const size_t cap = c->capsyntax ? c->capsyntax * 2 : 16;
      css_compiled_syntax ** g = gtext_allocator_realloc(&a, c->syntax_cache, cap * sizeof *g);
      if (g) {
        c->syntax_cache = g;
        c->capsyntax = cap;
      }
    }
    if (c->nsyntax < c->capsyntax) {
      c->syntax_cache[c->nsyntax++] = made;
      c->syntax_bytes += length;
      *out = made;
      GCU_MUTEX_UNLOCK(c->syntax_lock);
      return GTEXT_CSS_OK;
    }
  }
  GCU_MUTEX_UNLOCK(c->syntax_lock);
  *temporary = true;
  *out = made;
  return GTEXT_CSS_OK;
}
