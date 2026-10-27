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
 * Syntax strings (css_syntax_string.h).
 */

#include "css_syntax_string.h"

#include <string.h>

#include "css_internal.h"

static const char * const kTypeNames[] = {
  "length", "number", "percentage", "length-percentage", "string", "color", "image", "url", "integer", "angle", "time", "resolution",
  "transform-function", "custom-ident", "transform-list",
};

static bool is_supported_type(const char * name, size_t n) {
  for (size_t i = 0; i < sizeof kTypeNames / sizeof kTypeNames[0]; i++) {
    if (strlen(kTypeNames[i]) == n && memcmp(kTypeNames[i], name, n) == 0) {
      return true;
    }
  }
  return false;
}

void css_syntax_free(css_syntax * s) {
  if (!s) {
    return;
  }
  GTEXT_Allocator a = s->alloc;
  for (size_t i = 0; i < s->ncomps; i++) {
    gtext_allocator_free(&a, s->comps[i].name);
  }
  gtext_allocator_free(&a, s->comps);
  gtext_allocator_free(&a, s);
}

static bool is_ws_cv(const GTEXT_CSS_Component_Value * v) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_WHITESPACE;
}

static bool is_delim(const GTEXT_CSS_Component_Value * v, uint32_t cp) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_DELIM && v->delim == cp;
}

static bool is_ascii_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/// Whether an identifier can be a <custom-ident>: not a CSS-wide keyword and not `default`, compared ASCII case-insensitively (Values 4, 2.5).
static bool custom_ident_ok(const char * s, size_t n) {
  static const char * const bad[] = {"initial", "inherit", "unset", "revert", "revert-layer", "default"};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    const size_t bl = strlen(bad[i]);
    if (bl != n) {
      continue;
    }
    size_t k = 0;
    for (; k < n; k++) {
      char c = s[k];
      if (c >= 'A' && c <= 'Z') {
        c = (char)(c - 'A' + 'a');
      }
      if (c != bad[i][k]) {
        break;
      }
    }
    if (k == n) {
      return false;
    }
  }
  return true;
}

static GTEXT_CSS_Status fail_at(size_t * offset, size_t at) {
  if (offset) {
    *offset = at;
  }
  return GTEXT_CSS_E_GRAMMAR;
}

GTEXT_CSS_Status css_syntax_parse(const GTEXT_Allocator * alloc, const char * text, size_t length, css_syntax ** out, size_t * offset) {
  if (offset) {
    *offset = 0;
  }
  /* "Strip leading and trailing ASCII whitespace from string." */
  size_t b = 0, e = length;
  while (b < e && is_ascii_space(text[b])) {
    b++;
  }
  while (e > b && is_ascii_space(text[e - 1])) {
    e--;
  }
  if (b == e) {
    return fail_at(offset, 0);
  }
  GTEXT_Allocator a = *alloc;
  css_syntax * s = gtext_allocator_calloc(&a, 1, sizeof *s);
  if (!s) {
    return GTEXT_CSS_E_OOM;
  }
  s->alloc = a;
  if (e - b == 1 && text[b] == '*') {
    s->universal = true;
    *out = s;
    return GTEXT_CSS_OK;
  }
  /* comments are not white space here, and the tokenizer would skip them */
  for (size_t i = b; i + 1 < e; i++) {
    if (text[i] == '/' && text[i + 1] == '*') {
      css_syntax_free(s);
      return fail_at(offset, i);
    }
  }
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  po.allocator = alloc;
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Status st = gtext_css_parse_component_values(text + b, e - b, &po, &tree, NULL);
  if (st != GTEXT_CSS_OK) {
    css_syntax_free(s);
    return st;
  }
  const GTEXT_CSS_Component_Value * v = tree->values;
  const size_t n = tree->value_count;
  size_t i = 0;
  size_t cap = 0;
  for (;;) {
    while (i < n && is_ws_cv(&v[i])) {
      i++;
    }
    if (i >= n) {
      st = fail_at(offset, b + (n ? v[n - 1].span.end : 0));
      break;
    }
    css_syn_comp comp;
    memset(&comp, 0, sizeof comp);
    char * name = NULL;
    size_t name_len = 0;
    bool premultiplied = false;
    if (is_delim(&v[i], '<')) {
      /* a data type name: `<`, ident code points, `>` */
      if (i + 2 >= n || v[i + 1].kind != GTEXT_CSS_VALUE_TOKEN || v[i + 1].type != GTEXT_CSS_TOKEN_IDENT || !is_delim(&v[i + 2], '>') ||
          v[i + 1].span.end - v[i + 1].span.offset != v[i + 1].value.length) {
        st = fail_at(offset, b + v[i].span.offset);
        break;
      }
      if (!is_supported_type(v[i + 1].value.data, v[i + 1].value.length)) {
        st = fail_at(offset, b + v[i + 1].span.offset);
        break;
      }
      name = gtext_allocator_malloc(&a, v[i + 1].value.length + 1);
      if (!name) {
        st = GTEXT_CSS_E_OOM;
        break;
      }
      name_len = v[i + 1].value.length;
      memcpy(name, v[i + 1].value.data, name_len);
      name[name_len] = '\0';
      comp.is_type = true;
      premultiplied = name_len == 14 && memcmp(name, "transform-list", 14) == 0;
      i += 3;
    }
    else if (v[i].kind == GTEXT_CSS_VALUE_TOKEN && v[i].type == GTEXT_CSS_TOKEN_IDENT) {
      if (!custom_ident_ok(v[i].value.data, v[i].value.length)) {
        st = fail_at(offset, b + v[i].span.offset);
        break;
      }
      name = gtext_allocator_malloc(&a, v[i].value.length + 1);
      if (!name) {
        st = GTEXT_CSS_E_OOM;
        break;
      }
      name_len = v[i].value.length;
      memcpy(name, v[i].value.data, name_len);
      name[name_len] = '\0';
      i++;
    }
    else {
      st = fail_at(offset, b + v[i].span.offset);
      break;
    }
    comp.name = name;
    comp.name_len = name_len;
    if (!premultiplied && i < n && (is_delim(&v[i], '+') || is_delim(&v[i], '#'))) {
      comp.mult = v[i].delim == '+' ? '+' : '#';
      i++;
    }
    if (s->ncomps == cap) {
      const size_t nc = cap ? cap * 2 : 4;
      css_syn_comp * g = gtext_allocator_realloc(&a, s->comps, nc * sizeof *g);
      if (!g) {
        gtext_allocator_free(&a, name);
        st = GTEXT_CSS_E_OOM;
        break;
      }
      s->comps = g;
      cap = nc;
    }
    s->comps[s->ncomps++] = comp;
    /* "Consume as much whitespace as possible. EOF: return the definition. `|`: repeat. Anything else: failure." */
    while (i < n && is_ws_cv(&v[i])) {
      i++;
    }
    if (i >= n) {
      st = GTEXT_CSS_OK;
      break;
    }
    if (!is_delim(&v[i], '|')) {
      st = fail_at(offset, b + v[i].span.offset);
      break;
    }
    i++;
  }
  gtext_css_tree_free(tree);
  if (st != GTEXT_CSS_OK) {
    css_syntax_free(s);
    return st;
  }
  *out = s;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status css_syntax_component_grammar(const css_syntax * s, size_t i, char ** out, size_t * length) {
  *out = NULL;
  if (length) {
    *length = 0;
  }
  if (!s || s->universal || i >= s->ncomps) {
    return GTEXT_CSS_E_INVALID;
  }
  const css_syn_comp * c = &s->comps[i];
  if (!c->is_type) {
    return GTEXT_CSS_OK;
  }
  GTEXT_Allocator a = s->alloc;
  const bool list = c->name_len == 14 && memcmp(c->name, "transform-list", 14) == 0;
  const char * base = list ? "transform-function" : c->name;
  const size_t bl = strlen(base);
  const size_t total = bl + 2 + (list || c->mult ? 1 : 0);
  char * g = gtext_allocator_malloc(&a, total + 1);
  if (!g) {
    return GTEXT_CSS_E_OOM;
  }
  size_t k = 0;
  g[k++] = '<';
  memcpy(g + k, base, bl);
  k += bl;
  g[k++] = '>';
  if (list) {
    g[k++] = '+';
  }
  else if (c->mult) {
    g[k++] = c->mult;
  }
  g[k] = '\0';
  *out = g;
  if (length) {
    *length = k;
  }
  return GTEXT_CSS_OK;
}

/// Whether the values are the identifier of `c`, as often as its multiplier allows: once, any number of times separated by white space (`+`), or any number of
/// times separated by commas (`#`). Identifiers are compared code point by code point.
static bool idents_match(const css_syn_comp * c, const GTEXT_CSS_Component_Value * v, size_t n) {
  size_t items = 0;
  bool need_item = false; /* a comma was read and no item has followed it yet */
  for (size_t i = 0; i < n; i++) {
    if (is_ws_cv(&v[i])) {
      continue;
    }
    if (v[i].kind == GTEXT_CSS_VALUE_TOKEN && v[i].type == GTEXT_CSS_TOKEN_COMMA) {
      if (c->mult != '#' || items == 0 || need_item) {
        return false;
      }
      need_item = true;
      continue;
    }
    if (v[i].kind != GTEXT_CSS_VALUE_TOKEN || v[i].type != GTEXT_CSS_TOKEN_IDENT || v[i].value.length != c->name_len ||
        memcmp(v[i].value.data, c->name, c->name_len) != 0) {
      return false;
    }
    if (c->mult == '#' && items > 0 && !need_item) {
      return false; /* two items with no comma between */
    }
    items++;
    need_item = false;
  }
  if (items == 0 || need_item) {
    return false;
  }
  return c->mult != 0 || items == 1;
}

GTEXT_CSS_Status css_syntax_match(const css_syntax * s, GTEXT_CSS_Compiled * const * compiled, const GTEXT_CSS_Component_Value * values, size_t count,
    const GTEXT_CSS_Match_Options * options, bool * matched, css_syn_match * m) {
  *matched = false;
  m->component = 0;
  m->set = NULL;
  if (!s || s->universal) {
    return GTEXT_CSS_E_INVALID;
  }
  for (size_t i = 0; i < s->ncomps; i++) {
    const css_syn_comp * c = &s->comps[i];
    if (!c->is_type) {
      if (idents_match(c, values, count)) {
        *matched = true;
        m->component = i;
        return GTEXT_CSS_OK;
      }
      continue;
    }
    bool ok = false;
    GTEXT_CSS_Value_Set * set = NULL;
    GTEXT_CSS_Match_Result res;
    const GTEXT_CSS_Status st = gtext_css_match(compiled[i], values, count, options, &ok, &set, &res);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    if (ok) {
      *matched = true;
      m->component = i;
      m->set = set;
      return GTEXT_CSS_OK;
    }
    gtext_css_value_set_free(set);
  }
  return GTEXT_CSS_OK;
}
