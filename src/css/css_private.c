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
 * Private custom properties as the sheet compiler reads them (css_private_internal.h). Nothing here recurses: the walks keep their own stacks.
 */

#include <ghoti.io/cutil/atomic.h>
#include <ghoti.io/text/css/css_parse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "css_atrule_internal.h"
#include "css_private_internal.h"

bool css_priv_hidden(const char * name, size_t length) {
  return length >= 2u + CSS_PRIV_MARK_LEN && name[0] == '-' && name[1] == '-' && memcmp(name + 2, CSS_PRIV_MARK, CSS_PRIV_MARK_LEN) == 0;
}

uint64_t css_priv_next_epoch(void) {
  static GCU_Atomic_Size counter;
  return (uint64_t)gcu_atomic_size_fetch_add(&counter, 1) + 1u;
}

bool css_priv_group_name(const GTEXT_CSS_String * n) {
  return css_ascii_ieq(n, "media") || css_ascii_ieq(n, "supports") || css_ascii_ieq(n, "container") || css_ascii_ieq(n, "starting-style") || css_ascii_ieq(n, "document") ||
      css_ascii_ieq(n, "-moz-document") || css_ascii_ieq(n, "when") || css_ascii_ieq(n, "else");
}

void css_priv_regvec_free(const GTEXT_Allocator * alloc, css_priv_regvec * v) {
  gtext_allocator_free(alloc, v->v);
  v->v = NULL;
  v->n = v->cap = 0;
}

/* ---- the names of a style rule --------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  const char * data;
  size_t length;
} name_ref;

typedef struct {
  name_ref * v;
  size_t n, cap;
} name_vec;

static bool grow(const GTEXT_Allocator * a, void ** p, size_t * cap, size_t need, size_t elem) {
  if (need <= *cap) {
    return true;
  }
  size_t c = *cap ? *cap * 2 : 8;
  while (c < need) {
    c *= 2;
  }
  void * q = gtext_allocator_realloc(a, *p, c * elem);
  if (!q) {
    return false;
  }
  *p = q;
  *cap = c;
  return true;
}

static int name_cmp(const void * pa, const void * pb) {
  const name_ref * a = pa;
  const name_ref * b = pb;
  if (a->length != b->length) {
    return a->length < b->length ? -1 : 1;
  }
  return a->length ? memcmp(a->data, b->data, a->length) : 0;
}

static bool is_custom(const GTEXT_CSS_String * n) {
  return n->length >= 2 && n->data[0] == '-' && n->data[1] == '-';
}

/// What the walk of the rules inside a style rule is reading.
enum {
  M_BODY = 0,      ///< The rules of the style rule: an `@private` rule or a conditional group rule that may hold one.
  M_GROUP = 1,     ///< A conditional group rule of the style rule.
  M_PRIVATE = 2,   ///< The contents of an `@private` rule: custom properties, `@property` rules, conditional group rules.
  M_PRIV_GROUP = 3 ///< A conditional group rule inside an `@private` rule: the custom properties it holds count, an `@property` in it is not read (its condition is the cascade's to answer).
};

typedef struct {
  const GTEXT_CSS_Block_Item * items;   ///< The contents of an at-rule, or
  const GTEXT_CSS_Rule * rules;         ///< the rules of the style rule.
  size_t n, pos;
  int mode;
} frame;

static GTEXT_CSS_Status add_def(css_arena * arena, const GTEXT_CSS_Registry * registry, const GTEXT_Allocator * alloc, const GTEXT_CSS_Rule * rule, const char * key, size_t key_len,
    name_vec * names, css_priv_regvec * regs, size_t first_reg) {
  css_property_def d;
  bool valid = false;
  const char * why = NULL;
  GTEXT_CSS_Status st = css_at_property_rule_read(registry, alloc, rule, &d, &valid, &why);
  if (st != GTEXT_CSS_OK || !valid) {
    return st;
  }
  for (size_t i = 0; i < d.nnames && st == GTEXT_CSS_OK; i++) {
    const GTEXT_CSS_String * nm = &d.names[i];
    if (!grow(alloc, (void **)&names->v, &names->cap, names->n + 1, sizeof *names->v)) {
      st = GTEXT_CSS_E_OOM;
      break;
    }
    names->v[names->n++] = (name_ref){nm->data, nm->length};
    css_priv_reg r;
    memset(&r, 0, sizeof r);
    r.name = css_arena_alloc(arena, key_len + nm->length + 1);
    r.syntax = d.has_syntax ? css_arena_strdup(arena, d.syntax, d.syntax_len) : NULL;
    r.initial = d.has_initial ? css_arena_strdup(arena, d.initial, d.initial_len) : NULL;
    if (!r.name || (d.has_syntax && !r.syntax) || (d.has_initial && !r.initial)) {
      st = GTEXT_CSS_E_OOM;
      break;
    }
    memcpy(r.name, key, key_len);
    memcpy(r.name + key_len, nm->data, nm->length);
    r.name[key_len + nm->length] = '\0';
    r.name_len = key_len + nm->length;
    r.has_syntax = d.has_syntax;
    r.syntax_len = d.syntax_len;
    r.inherits = d.has_inherits ? d.inherits : true;
    r.has_initial = d.has_initial;
    r.initial_len = d.initial_len;
    /* "If multiple valid @property rules represent a registration for the same <custom-property-name>, the last one in stylesheet order wins." */
    size_t at = regs->n;
    for (size_t k = first_reg; k < regs->n; k++) {
      if (regs->v[k].name_len == r.name_len && memcmp(regs->v[k].name, r.name, r.name_len) == 0) {
        at = k;
        break;
      }
    }
    if (at == regs->n) {
      if (!grow(alloc, (void **)&regs->v, &regs->cap, regs->n + 1, sizeof *regs->v)) {
        st = GTEXT_CSS_E_OOM;
        break;
      }
      regs->n++;
    }
    regs->v[at] = r;
  }
  css_property_def_free(alloc, &d);
  return st;
}

GTEXT_CSS_Status css_priv_scope_make(css_arena * arena, const GTEXT_Allocator * alloc, const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Rule * style, const css_priv_scope * parent,
    uint64_t * epoch, uint64_t serial, css_priv_regvec * regs, const css_priv_scope ** out) {
  *out = NULL;
  /* the rule's private names are only read when it holds an at-rule that can lead to one: the common rule has none */
  bool any = false;
  for (size_t i = 0; i < style->rule_count && !any; i++) {
    const GTEXT_CSS_Rule * r = &style->rules[i];
    any = r->kind == GTEXT_CSS_RULE_AT && (css_ascii_ieq(&r->name, "private") || css_priv_group_name(&r->name));
  }
  if (!any) {
    return GTEXT_CSS_OK;
  }
  if (!*epoch) {
    *epoch = css_priv_next_epoch();
  }
  char keybuf[96];
  const int kn = snprintf(keybuf, sizeof keybuf, "--" CSS_PRIV_MARK "p%llu_%llu_", (unsigned long long)*epoch, (unsigned long long)serial);
  if (kn <= 0 || (size_t)kn >= sizeof keybuf) {
    return GTEXT_CSS_E_INTERNAL;
  }
  name_vec names;
  memset(&names, 0, sizeof names);
  frame * stack = NULL;
  size_t sp = 0, scap = 0;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  const size_t first_reg = regs->n;
  if (!grow(alloc, (void **)&stack, &scap, 1, sizeof *stack)) {
    return GTEXT_CSS_E_OOM;
  }
  stack[sp++] = (frame){NULL, style->rules, style->rule_count, 0, M_BODY};
  while (sp && st == GTEXT_CSS_OK) {
    frame * f = &stack[sp - 1];
    if (f->pos >= f->n) {
      sp--;
      continue;
    }
    const GTEXT_CSS_Rule * r = NULL;
    if (f->items) {
      const GTEXT_CSS_Block_Item * it = &f->items[f->pos++];
      if (it->kind == GTEXT_CSS_ITEM_DECLARATIONS) {
        if (f->mode == M_PRIVATE || f->mode == M_PRIV_GROUP) {
          for (size_t i = 0; i < it->declaration_count; i++) {
            if (is_custom(&it->declarations[i].name)) {
              if (!grow(alloc, (void **)&names.v, &names.cap, names.n + 1, sizeof *names.v)) {
                st = GTEXT_CSS_E_OOM;
                break;
              }
              names.v[names.n++] = (name_ref){it->declarations[i].name.data, it->declarations[i].name.length};
            }
          }
        }
        continue;
      }
      r = it->rule;
    }
    else {
      r = &f->rules[f->pos++];
    }
    if (!r || r->kind != GTEXT_CSS_RULE_AT) {
      continue;
    }
    const int mode = f->mode;
    int next = -1;
    if ((mode == M_BODY || mode == M_GROUP) && css_ascii_ieq(&r->name, "private")) {
      next = mode == M_BODY ? M_PRIVATE : M_PRIV_GROUP;
    }
    else if (css_priv_group_name(&r->name)) {
      next = mode == M_BODY || mode == M_GROUP ? M_GROUP : M_PRIV_GROUP;
    }
    else if (mode == M_PRIVATE && css_ascii_ieq(&r->name, "property")) {
      st = add_def(arena, registry, alloc, r, keybuf, (size_t)kn, &names, regs, first_reg);
      continue;
    }
    if (next < 0 || !r->has_block || r->content_count == 0) {
      continue;
    }
    if (!grow(alloc, (void **)&stack, &scap, sp + 1, sizeof *stack)) {
      st = GTEXT_CSS_E_OOM;
      break;
    }
    stack[sp++] = (frame){r->contents, NULL, r->content_count, 0, next};
  }
  gtext_allocator_free(alloc, stack);
  if (st != GTEXT_CSS_OK || names.n == 0) {
    gtext_allocator_free(alloc, names.v);
    return st;
  }
  qsort(names.v, names.n, sizeof *names.v, name_cmp);
  size_t m = 0;
  for (size_t i = 0; i < names.n; i++) {
    if (m == 0 || name_cmp(&names.v[m - 1], &names.v[i]) != 0) {
      names.v[m++] = names.v[i];
    }
  }
  css_priv_scope * sc = css_arena_alloc(arena, sizeof *sc);
  GTEXT_CSS_String * kept = css_arena_alloc(arena, m * sizeof *kept);
  char * key = css_arena_strdup(arena, keybuf, (size_t)kn);
  if (!sc || !kept || !key) {
    gtext_allocator_free(alloc, names.v);
    return GTEXT_CSS_E_OOM;
  }
  for (size_t i = 0; i < m; i++) {
    char * copy = css_arena_strdup(arena, names.v[i].data, names.v[i].length);
    if (!copy) {
      gtext_allocator_free(alloc, names.v);
      return GTEXT_CSS_E_OOM;
    }
    kept[i].data = copy;
    kept[i].length = names.v[i].length;
  }
  gtext_allocator_free(alloc, names.v);
  sc->parent = parent;
  sc->names = kept;
  sc->nnames = m;
  sc->key = key;
  sc->key_len = (size_t)kn;
  *out = sc;
  return GTEXT_CSS_OK;
}

const css_priv_scope * css_priv_find(const css_priv_scope * chain, const char * name, size_t length) {
  for (const css_priv_scope * s = chain; s; s = s->parent) {
    size_t lo = 0, hi = s->nnames;
    while (lo < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      const GTEXT_CSS_String * n = &s->names[mid];
      int c;
      if (n->length != length) {
        c = n->length < length ? -1 : 1;
      }
      else {
        c = length ? memcmp(n->data, name, length) : 0;
      }
      if (c == 0) {
        return s;
      }
      if (c < 0) {
        lo = mid + 1;
      }
      else {
        hi = mid;
      }
    }
  }
  return NULL;
}

char * css_priv_name(css_arena * arena, const css_priv_scope * scope, const char * name, size_t length, size_t * out_length) {
  char * o = css_arena_alloc(arena, scope->key_len + length + 1);
  if (!o) {
    return NULL;
  }
  memcpy(o, scope->key, scope->key_len);
  if (length) {
    memcpy(o + scope->key_len, name, length);
  }
  o[scope->key_len + length] = '\0';
  *out_length = scope->key_len + length;
  return o;
}

GTEXT_CSS_Status css_priv_canonical(const GTEXT_Allocator * alloc, const char * text, size_t length, char ** out, size_t * out_length) {
  char * o = gtext_allocator_malloc(alloc, length + 1);
  if (!o) {
    return GTEXT_CSS_E_OOM;
  }
  size_t n = 0;
  for (size_t i = 0; i < length;) {
    /* `--`, the code point, `p`, digits, `_`: the digits are the sheet's */
    if (length - i > 2u + CSS_PRIV_MARK_LEN + 1 && text[i] == '-' && text[i + 1] == '-' && memcmp(text + i + 2, CSS_PRIV_MARK, CSS_PRIV_MARK_LEN) == 0 && text[i + 2 + CSS_PRIV_MARK_LEN] == 'p') {
      size_t k = i + 3u + CSS_PRIV_MARK_LEN;
      size_t d = k;
      while (d < length && text[d] >= '0' && text[d] <= '9') {
        d++;
      }
      if (d > k && d < length && text[d] == '_') {
        memcpy(o + n, text + i, k - i);
        n += k - i;
        i = d;
        continue;
      }
    }
    o[n++] = text[i++];
  }
  o[n] = '\0';
  *out = o;
  *out_length = n;
  return GTEXT_CSS_OK;
}

/* ---- the references in a value ------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  char * data;
  size_t len, cap;
} bytes;

static bool put(const GTEXT_Allocator * a, bytes * b, const char * s, size_t n) {
  if (!grow(a, (void **)&b->data, &b->cap, b->len + n + 1, 1)) {
    return false;
  }
  if (n) {
    memcpy(b->data + b->len, s, n);
  }
  b->len += n;
  return true;
}

/// The hidden name of a scope's property, written as an identifier: what is not an identifier character is escaped.
static bool put_ident(const GTEXT_Allocator * a, bytes * b, const css_priv_scope * scope, const GTEXT_CSS_String * name) {
  for (int part = 0; part < 2; part++) {
    const char * s = part ? name->data : scope->key;
    const size_t n = part ? name->length : scope->key_len;
    for (size_t i = 0; i < n; i++) {
      const unsigned char c = (unsigned char)s[i];
      if (c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_') {
        if (!put(a, b, s + i, 1)) {
          return false;
        }
      }
      else {
        char esc[16];
        const int k = snprintf(esc, sizeof esc, "\\%x ", (unsigned)c);
        if (k <= 0 || !put(a, b, esc, (size_t)k)) {
          return false;
        }
      }
    }
  }
  return true;
}

typedef struct {
  size_t offset, end;
  const css_priv_scope * scope;
  GTEXT_CSS_String name;
} edit;

typedef struct {
  edit * v;
  size_t n, cap;
} editvec;

typedef struct {
  const GTEXT_CSS_Component_Value * v;
  size_t n, pos;
  bool style;   ///< Inside a `style()` query: a custom property name is read at its place in a comparison.
} vframe;

static bool is_ws(const GTEXT_CSS_Component_Value * v) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_WHITESPACE;
}

static bool is_dashed_ident(const GTEXT_CSS_Component_Value * v) {
  return v->kind == GTEXT_CSS_VALUE_TOKEN && v->type == GTEXT_CSS_TOKEN_IDENT && v->value.length >= 2 && v->value.data[0] == '-' && v->value.data[1] == '-';
}

static GTEXT_CSS_Status note(const GTEXT_Allocator * a, editvec * e, const css_priv_scope * chain, const GTEXT_CSS_Component_Value * v) {
  const css_priv_scope * sc = css_priv_find(chain, v->value.data, v->value.length);
  if (!sc) {
    return GTEXT_CSS_OK;
  }
  if (!grow(a, (void **)&e->v, &e->cap, e->n + 1, sizeof *e->v)) {
    return GTEXT_CSS_E_OOM;
  }
  e->v[e->n++] = (edit){v->span.offset, v->span.end, sc, v->value};
  return GTEXT_CSS_OK;
}

static int edit_cmp(const void * pa, const void * pb) {
  const edit * a = pa;
  const edit * b = pb;
  return a->offset < b->offset ? -1 : a->offset > b->offset;
}

static bool may_refer(const char * text, size_t length) {
  /* a private property is referred to by var(), inherit() or a style() query: a text with none of them has nothing to write */
  for (size_t i = 0; i + 1 < length; i++) {
    if (text[i] == '-' && text[i + 1] == '-') {
      return true;
    }
  }
  return false;
}

GTEXT_CSS_Status css_priv_rewrite_text(const GTEXT_Allocator * alloc, const css_priv_scope * chain, const char * text, size_t length, char ** out, size_t * out_length) {
  *out = NULL;
  *out_length = 0;
  if (!chain || !text || !may_refer(text, length)) {
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  po.allocator = alloc;
  po.count_errors_only = true;
  GTEXT_CSS_Tree * tree = NULL;
  GTEXT_CSS_Status st = gtext_css_parse_component_values(text, length, &po, &tree, NULL);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  editvec edits;
  memset(&edits, 0, sizeof edits);
  vframe * stack = NULL;
  size_t sp = 0, scap = 0;
  if (!grow(alloc, (void **)&stack, &scap, 1, sizeof *stack)) {
    st = GTEXT_CSS_E_OOM;
  }
  else {
    stack[sp++] = (vframe){tree->values, tree->value_count, 0, false};
  }
  while (st == GTEXT_CSS_OK && sp) {
    vframe * f = &stack[sp - 1];
    if (f->pos >= f->n) {
      sp--;
      continue;
    }
    const size_t i = f->pos++;
    const GTEXT_CSS_Component_Value * v = &f->v[i];
    if (f->style && is_dashed_ident(v)) {
      /* a `style()` query names a property before the colon of a comparison, or alone, at the start of a condition or after `and`, `or` or `not` */
      size_t p = i;
      while (p > 0 && is_ws(&f->v[p - 1])) {
        p--;
      }
      bool before = p == 0;
      if (!before && f->v[p - 1].kind == GTEXT_CSS_VALUE_TOKEN && f->v[p - 1].type == GTEXT_CSS_TOKEN_IDENT) {
        before = css_ascii_ieq(&f->v[p - 1].value, "and") || css_ascii_ieq(&f->v[p - 1].value, "or") || css_ascii_ieq(&f->v[p - 1].value, "not");
      }
      size_t q = i + 1;
      while (q < f->n && is_ws(&f->v[q])) {
        q++;
      }
      const bool after = q >= f->n || (f->v[q].kind == GTEXT_CSS_VALUE_TOKEN && f->v[q].type == GTEXT_CSS_TOKEN_COLON);
      if (before && after) {
        st = note(alloc, &edits, chain, v);
      }
      continue;
    }
    if (v->kind == GTEXT_CSS_VALUE_TOKEN || !v->child_count) {
      continue;
    }
    bool style = f->style && v->kind == GTEXT_CSS_VALUE_BLOCK;
    if (v->kind == GTEXT_CSS_VALUE_FUNCTION) {
      if (css_ascii_ieq(&v->value, "var") || css_ascii_ieq(&v->value, "inherit")) {
        for (size_t k = 0; k < v->child_count; k++) {
          if (is_ws(&v->children[k])) {
            continue;
          }
          if (is_dashed_ident(&v->children[k])) {
            st = note(alloc, &edits, chain, &v->children[k]);
          }
          break;
        }
      }
      else {
        style = css_ascii_ieq(&v->value, "style");
      }
    }
    if (st != GTEXT_CSS_OK) {
      break;
    }
    if (!grow(alloc, (void **)&stack, &scap, sp + 1, sizeof *stack)) {
      st = GTEXT_CSS_E_OOM;
      break;
    }
    f = &stack[sp - 1];   /* the stack may have moved */
    stack[sp++] = (vframe){v->children, v->child_count, 0, style};
  }
  gtext_allocator_free(alloc, stack);
  if (st == GTEXT_CSS_OK && edits.n) {
    qsort(edits.v, edits.n, sizeof *edits.v, edit_cmp);
    bytes b;
    memset(&b, 0, sizeof b);
    size_t at = 0;
    for (size_t k = 0; k < edits.n && st == GTEXT_CSS_OK; k++) {
      const edit * e = &edits.v[k];
      if (e->offset < at || e->end > length || !put(alloc, &b, text + at, e->offset - at) || !put_ident(alloc, &b, e->scope, &e->name)) {
        st = e->offset < at || e->end > length ? GTEXT_CSS_E_INTERNAL : GTEXT_CSS_E_OOM;
        break;
      }
      at = e->end;
    }
    if (st == GTEXT_CSS_OK && !put(alloc, &b, text + at, length - at)) {
      st = GTEXT_CSS_E_OOM;
    }
    if (st == GTEXT_CSS_OK) {
      b.data[b.len] = '\0';
      *out = b.data;
      *out_length = b.len;
    }
    else {
      gtext_allocator_free(alloc, b.data);
    }
  }
  gtext_allocator_free(alloc, edits.v);
  gtext_css_tree_free(tree);
  return st;
}
