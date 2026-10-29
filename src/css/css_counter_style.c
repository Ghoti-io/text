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
 * `@counter-style` as a typed struct (css_counter_style.h). Parses the canonical descriptor text the registry produced.
 */

#include <stdlib.h>
#include <string.h>

#include <ghoti.io/text/css/css_counter_style.h>

#include "css_atrule_internal.h"

static GTEXT_CSS_Counter_Symbol symbol_of(holder * h, const GTEXT_CSS_Component_Value * v, bool * oom, const GTEXT_Allocator * alloc) {
  GTEXT_CSS_Counter_Symbol s;
  memset(&s, 0, sizeof s);
  if (v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_STRING) {
    s.text = copy_string(h, &v->value, oom);
    s.string = true;
  }
  else if (v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_IDENT) {
    s.text = copy_string(h, &v->value, oom);
  }
  else {
    GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
    so.allocator = alloc;
    char * buf = NULL;
    size_t bl = 0;
    if (gtext_css_serialize_values_buffer(v, 1, &so, &buf, &bl) == GTEXT_CSS_OK) {
      s.text.data = css_arena_strdup(&h->arena, buf, bl);
      s.text.length = bl;
      gtext_css_serialize_buffer_free(&so, buf);
    }
    if (!s.text.data) {
      *oom = true;
    }
  }
  return s;
}

/// The number of comma separated groups, and the span [start, end) of group g.
static size_t groups_of(const GTEXT_CSS_Tree * t) {
  size_t n = 1;
  for (size_t i = 0; i < t->value_count; i++) {
    n += is_comma(&t->values[i]);
  }
  return n;
}

static void group_span(const GTEXT_CSS_Tree * t, size_t g, size_t * from, size_t * to) {
  size_t cur = 0, i = 0;
  while (cur < g && i < t->value_count) {
    if (is_comma(&t->values[i])) {
      cur++;
    }
    i++;
  }
  size_t j = i;
  while (j < t->value_count && !is_comma(&t->values[j])) {
    j++;
  }
  *from = i;
  *to = j;
}

void gtext_css_counter_style_free(GTEXT_CSS_Counter_Style * style) {
  if (style) {
    holder * h = style->priv_;
    const GTEXT_Allocator a = h->alloc;
    holder_free(h);
    gtext_allocator_free(&a, style);
  }
}

static bool read_int(const GTEXT_CSS_Component_Value * v, int64_t * out) {
  if (v && v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_NUMBER) {
    *out = clamp_i64(v->number);
    return true;
  }
  return false;
}

GTEXT_CSS_Status gtext_css_counter_style_from_atrule(const GTEXT_CSS_Atrule * rule, GTEXT_CSS_Counter_Style ** out) {
  if (!rule || !out || gtext_css_atrule_kind(rule) != GTEXT_CSS_ATRULE_COUNTER_STYLE) {
    return GTEXT_CSS_E_INVALID;
  }
  *out = NULL;
  const GTEXT_Allocator * al = &rule->alloc;
  GTEXT_CSS_Counter_Style * c = gtext_allocator_calloc(al, 1, sizeof *c);
  if (!c) {
    return GTEXT_CSS_E_OOM;
  }
  holder * h = holder_new(al);
  if (!h) {
    gtext_allocator_free(al, c);
    return GTEXT_CSS_E_OOM;
  }
  c->priv_ = h;
  c->rule = rule;
  bool oom = false;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  GTEXT_CSS_String nm;
  if (gtext_css_atrule_name_at(rule, 0, &nm, NULL)) {
    c->name = copy_string(h, &nm, &oom);
  }
  c->system = GTEXT_CSS_COUNTER_SYMBOLIC;
  c->fixed_first = 1;
  c->range_auto = true;
  c->negative_prefix.text.data = "-";
  c->negative_prefix.text.length = 1;
  c->negative_prefix.string = true;
  c->suffix.text.data = ". ";
  c->suffix.text.length = 2;
  c->suffix.string = true;
  c->pad_symbol.text.data = "";
  c->fallback.data = "decimal";
  c->fallback.length = 7;
  c->speak_as.data = "auto";
  c->speak_as.length = 4;
  GTEXT_CSS_Tree * t = NULL;
  const size_t max = rule->max_children;
  /* system */
  if ((st = css_atrule_tokens(rule, "system", &t)) == GTEXT_CSS_OK && t) {
    size_t i = 0;
    const GTEXT_CSS_Component_Value * a = next_sig(t->values, t->value_count, &i);
    static const char * const names[] = {"cyclic", "numeric", "alphabetic", "symbolic", "fixed", "additive", "extends"};
    for (int k = 0; a && k < 7; k++) {
      if (is_ident(a, names[k])) {
        c->system = (GTEXT_CSS_Counter_System)k;
      }
    }
    const GTEXT_CSS_Component_Value * b = next_sig(t->values, t->value_count, &i);
    if (c->system == GTEXT_CSS_COUNTER_FIXED) {
      read_int(b, &c->fixed_first);
    }
    else if (c->system == GTEXT_CSS_COUNTER_EXTENDS && b) {
      c->extends_name = copy_string(h, &b->value, &oom);
    }
  }
  gtext_css_tree_free(t);
  t = NULL;
  /* symbols */
  if (st == GTEXT_CSS_OK && (st = css_atrule_tokens(rule, "symbols", &t)) == GTEXT_CSS_OK && t) {
    GTEXT_CSS_Counter_Symbol * arr = css_arena_alloc(&h->arena, (t->value_count + 1) * sizeof *arr);
    size_t n = 0, i = 0;
    const GTEXT_CSS_Component_Value * v;
    if (!arr) {
      oom = true;
    }
    while (arr && (v = next_sig(t->values, t->value_count, &i)) != NULL) {
      if (n >= max) {
        st = GTEXT_CSS_E_LIMIT_DESCRIPTORS;
        break;
      }
      arr[n++] = symbol_of(h, v, &oom, al);
    }
    c->symbols = arr;
    c->symbol_count = n;
  }
  gtext_css_tree_free(t);
  t = NULL;
  /* additive-symbols */
  if (st == GTEXT_CSS_OK && (st = css_atrule_tokens(rule, "additive-symbols", &t)) == GTEXT_CSS_OK && t) {
    const size_t g = groups_of(t);
    if (g > max) {
      st = GTEXT_CSS_E_LIMIT_DESCRIPTORS;
    }
    else {
      GTEXT_CSS_Counter_Tuple * arr = css_arena_alloc(&h->arena, g * sizeof *arr);
      if (!arr) {
        oom = true;
      }
      size_t n = 0;
      for (size_t k = 0; arr && k < g; k++) {
        size_t from, to;
        group_span(t, k, &from, &to);
        size_t i = from;
        const GTEXT_CSS_Component_Value * w = next_sig(t->values, to, &i);
        const GTEXT_CSS_Component_Value * s = next_sig(t->values, to, &i);
        int64_t weight = 0;
        if (read_int(w, &weight) && s) {
          arr[n].weight = weight;
          arr[n].symbol = symbol_of(h, s, &oom, al);
          n++;
        }
      }
      c->additive = arr;
      c->additive_count = n;
    }
  }
  gtext_css_tree_free(t);
  t = NULL;
  /* negative, prefix, suffix, pad */
  if (st == GTEXT_CSS_OK && (st = css_atrule_tokens(rule, "negative", &t)) == GTEXT_CSS_OK && t) {
    size_t i = 0;
    const GTEXT_CSS_Component_Value * a = next_sig(t->values, t->value_count, &i);
    const GTEXT_CSS_Component_Value * b = next_sig(t->values, t->value_count, &i);
    c->has_negative = true;
    if (a) {
      c->negative_prefix = symbol_of(h, a, &oom, al);
    }
    if (b) {
      c->negative_suffix = symbol_of(h, b, &oom, al);
    }
  }
  gtext_css_tree_free(t);
  t = NULL;
  for (int pass = 0; pass < 2 && st == GTEXT_CSS_OK; pass++) {
    if ((st = css_atrule_tokens(rule, pass ? "suffix" : "prefix", &t)) == GTEXT_CSS_OK && t) {
      size_t i = 0;
      const GTEXT_CSS_Component_Value * a = next_sig(t->values, t->value_count, &i);
      if (a) {
        if (pass) {
          c->has_suffix = true;
          c->suffix = symbol_of(h, a, &oom, al);
        }
        else {
          c->has_prefix = true;
          c->prefix = symbol_of(h, a, &oom, al);
        }
      }
    }
    gtext_css_tree_free(t);
    t = NULL;
  }
  if (st == GTEXT_CSS_OK && (st = css_atrule_tokens(rule, "pad", &t)) == GTEXT_CSS_OK && t) {
    size_t i = 0;
    const GTEXT_CSS_Component_Value * a = next_sig(t->values, t->value_count, &i);
    const GTEXT_CSS_Component_Value * b = next_sig(t->values, t->value_count, &i);
    int64_t len = 0;
    if (read_int(a, &len) && len > 0 && b) {
      c->pad_length = len > 0xFFFFFFFF ? 0xFFFFFFFFu : (uint32_t)len;
      c->pad_symbol = symbol_of(h, b, &oom, al);
    }
  }
  gtext_css_tree_free(t);
  t = NULL;
  /* range */
  if (st == GTEXT_CSS_OK && (st = css_atrule_tokens(rule, "range", &t)) == GTEXT_CSS_OK && t) {
    const size_t g = groups_of(t);
    if (g > max) {
      st = GTEXT_CSS_E_LIMIT_DESCRIPTORS;
    }
    else {
      GTEXT_CSS_Counter_Range * arr = css_arena_alloc(&h->arena, g * sizeof *arr);
      if (!arr) {
        oom = true;
      }
      size_t n = 0;
      for (size_t k = 0; arr && k < g; k++) {
        size_t from, to;
        group_span(t, k, &from, &to);
        size_t i = from;
        const GTEXT_CSS_Component_Value * a = next_sig(t->values, to, &i);
        const GTEXT_CSS_Component_Value * b = next_sig(t->values, to, &i);
        if (a && !b && is_ident(a, "auto")) {
          continue;
        }
        if (a && b) {
          memset(&arr[n], 0, sizeof arr[n]);
          arr[n].lower_infinite = is_ident(a, "infinite");
          arr[n].upper_infinite = is_ident(b, "infinite");
          read_int(a, &arr[n].lower);
          read_int(b, &arr[n].upper);
          n++;
        }
      }
      if (n) {
        c->range_auto = false;
      }
      c->ranges = arr;
      c->range_count = n;
    }
  }
  gtext_css_tree_free(t);
  t = NULL;
  if (st == GTEXT_CSS_OK && (st = css_atrule_tokens(rule, "fallback", &t)) == GTEXT_CSS_OK && t) {
    size_t i = 0;
    const GTEXT_CSS_Component_Value * a = next_sig(t->values, t->value_count, &i);
    if (a) {
      c->fallback = copy_string(h, &a->value, &oom);
    }
  }
  gtext_css_tree_free(t);
  t = NULL;
  {
    GTEXT_CSS_Atrule_Descriptor d;
    if (gtext_css_atrule_descriptor_find(rule, "speak-as", 8, &d)) {
      c->speak_as = copy_string(h, &d.text, &oom);
    }
  }
  /* validity of the counts for the system */
  switch (c->system) {
  case GTEXT_CSS_COUNTER_CYCLIC:
  case GTEXT_CSS_COUNTER_SYMBOLIC:
  case GTEXT_CSS_COUNTER_FIXED:
    c->valid = c->symbol_count >= 1;
    break;
  case GTEXT_CSS_COUNTER_ALPHABETIC:
  case GTEXT_CSS_COUNTER_NUMERIC:
    c->valid = c->symbol_count >= 2;
    break;
  case GTEXT_CSS_COUNTER_ADDITIVE:
    c->valid = c->additive_count >= 1;
    for (size_t i = 1; c->valid && i < c->additive_count; i++) {
      if (c->additive[i].weight >= c->additive[i - 1].weight) {
        c->valid = false;
      }
    }
    break;
  case GTEXT_CSS_COUNTER_EXTENDS:
    c->valid = c->extends_name.data != NULL;
    break;
  }
  if (gtext_css_atrule_defines_nothing(rule)) {
    c->valid = false;
  }
  c->resolved_system = c->system;
  if (st == GTEXT_CSS_OK && oom) {
    st = GTEXT_CSS_E_OOM;
  }
  if (st != GTEXT_CSS_OK) {
    gtext_css_counter_style_free(c);
    return st;
  }
  *out = c;
  return GTEXT_CSS_OK;
}
