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

/*
 * The canonical form of a CSS tree: what "the same tree" means for the round
 * trip of CSS Syntax section 9 ("parsing the stylesheet must produce the same
 * data structures as parsing, serializing, and parsing again, except for
 * consecutive whitespace-tokens, which may be collapsed into a single
 * token").
 *
 * Two trees are the same when their canonical forms are equal. The form
 * holds every field of every node except the ones that describe the text
 * rather than the structure: positions (spans), identities, a custom
 * property's original text (it is the source's spelling), and whether the
 * end of the input closed a token, a block or a function (a serialization
 * writes the closing character). A run of whitespace tokens is one.
 *
 * Shared by the conformance runner (C), the tests and the fuzz harness (C++),
 * so it is written in the common subset of the two. Test code: it allocates
 * with malloc and recurses (as deep as the tree, which its parse bounded).
 */

#ifndef GHOTI_IO_GTEXT_TOOLS_CONFORMANCE_CSS_TREE_CANON_H
#define GHOTI_IO_GTEXT_TOOLS_CONFORMANCE_CSS_TREE_CANON_H

#include <ghoti.io/text/css.h>
#include <ghoti.io/text/macros.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char * data;
  size_t len;
  size_t cap;
} css_canon_buf;

static void css_canon_put(css_canon_buf * b, const char * s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + n + 1) {
      cap *= 2;
    }
    char * grown = (char *)realloc(b->data, cap);
    if (!grown) {
      abort();
    }
    b->data = grown;
    b->cap = cap;
  }
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
}

static void css_canon_puts(css_canon_buf * b, const char * s) {
  css_canon_put(b, s, strlen(s));
}

static void css_canon_str(css_canon_buf * b, const GTEXT_CSS_String * s) {
  char tmp[32];
  snprintf(tmp, sizeof tmp, "%zu:", s->length);
  css_canon_puts(b, tmp);
  if (s->length) {
    css_canon_put(b, s->data, s->length);
  }
}

static void css_canon_values(css_canon_buf * b, const GTEXT_CSS_Component_Value * v, size_t n);

static void css_canon_value(css_canon_buf * b, const GTEXT_CSS_Component_Value * v) {
  char tmp[96];
  snprintf(tmp, sizeof tmp, "(%d %d %u ", (int)v->kind, (int)v->type,
      (unsigned)(v->flags & ~(unsigned)GTEXT_CSS_FLAG_UNCLOSED));
  css_canon_puts(b, tmp);
  css_canon_str(b, &v->value);
  css_canon_puts(b, " ");
  css_canon_str(b, &v->unit);
  css_canon_puts(b, " ");
  css_canon_str(b, &v->repr);
  snprintf(tmp, sizeof tmp, " %.17g %u %u %u", v->number, (unsigned)v->delim,
      (unsigned)v->range_start, (unsigned)v->range_end);
  css_canon_puts(b, tmp);
  if (v->kind != GTEXT_CSS_VALUE_TOKEN) {
    css_canon_puts(b, " [");
    css_canon_values(b, v->children, v->child_count);
    css_canon_puts(b, "]");
  }
  css_canon_puts(b, ")");
}

static void css_canon_values(css_canon_buf * b, const GTEXT_CSS_Component_Value * v, size_t n) {
  int prev_ws = 0;
  for (size_t i = 0; i < n; i++) {
    const int ws = v[i].kind == GTEXT_CSS_VALUE_TOKEN && v[i].type == GTEXT_CSS_TOKEN_WHITESPACE;
    if (ws && prev_ws) {
      continue;
    }
    prev_ws = ws;
    css_canon_value(b, &v[i]);
  }
}

static void css_canon_decls(css_canon_buf * b, const GTEXT_CSS_Declaration * d, size_t n) {
  for (size_t i = 0; i < n; i++) {
    css_canon_puts(b, "(decl ");
    css_canon_str(b, &d[i].name);
    css_canon_puts(b, d[i].important ? " !important [" : " [");
    css_canon_values(b, d[i].value, d[i].value_count);
    css_canon_puts(b, "] ");
    css_canon_puts(b, d[i].original_text.data ? "custom" : "-");
    css_canon_puts(b, ")");
  }
}

static void css_canon_rules(css_canon_buf * b, const GTEXT_CSS_Rule * r, size_t n);

static void css_canon_rule(css_canon_buf * b, const GTEXT_CSS_Rule * r) {
  char tmp[64];
  snprintf(tmp, sizeof tmp, "(rule %d %d ", (int)r->kind, r->has_block ? 1 : 0);
  css_canon_puts(b, tmp);
  css_canon_str(b, &r->name);
  css_canon_puts(b, " [");
  css_canon_values(b, r->prelude, r->prelude_count);
  css_canon_puts(b, "] {");
  css_canon_decls(b, r->declarations, r->declaration_count);
  css_canon_puts(b, "} {");
  css_canon_rules(b, r->rules, r->rule_count);
  css_canon_puts(b, "} {");
  for (size_t i = 0; i < r->content_count; i++) {
    if (r->contents[i].kind == GTEXT_CSS_ITEM_RULE) {
      css_canon_rule(b, r->contents[i].rule);
    }
    else {
      css_canon_puts(b, "(decls ");
      css_canon_decls(b, r->contents[i].declarations, r->contents[i].declaration_count);
      css_canon_puts(b, ")");
    }
  }
  css_canon_puts(b, "})");
}

static void css_canon_rules(css_canon_buf * b, const GTEXT_CSS_Rule * r, size_t n) {
  for (size_t i = 0; i < n; i++) {
    css_canon_rule(b, &r[i]);
  }
}

/* The canonical form of a whole tree; free() the result. */
static char * css_tree_canon(const GTEXT_CSS_Tree * t) {
  css_canon_buf b = {NULL, 0, 0};
  char tmp[64];
  snprintf(tmp, sizeof tmp, "(tree %d %d ", (int)t->kind, (int)t->syntax_error);
  css_canon_puts(&b, tmp);
  css_canon_rules(&b, t->rules, t->rule_count);
  css_canon_puts(&b, " | ");
  css_canon_decls(&b, t->declarations, t->declaration_count);
  css_canon_puts(&b, " | ");
  for (size_t i = 0; i < t->item_count; i++) {
    if (t->items[i].kind == GTEXT_CSS_ITEM_RULE) {
      css_canon_rule(&b, t->items[i].rule);
    }
    else {
      css_canon_puts(&b, "(decls ");
      css_canon_decls(&b, t->items[i].declarations, t->items[i].declaration_count);
      css_canon_puts(&b, ")");
    }
  }
  css_canon_puts(&b, " | ");
  css_canon_values(&b, t->values, t->value_count);
  css_canon_puts(&b, " | ");
  for (size_t i = 0; i < t->list_count; i++) {
    css_canon_puts(&b, "<");
    css_canon_values(&b, t->lists[i].values, t->lists[i].count);
    css_canon_puts(&b, ">");
  }
  css_canon_puts(&b, ")");
  return b.data;
}

/* The parse function of each tree kind, to read a serialization back. */
typedef GTEXT_CSS_Status (*css_canon_parse_fn)(const char *, size_t, const GTEXT_CSS_Parse_Options *,
    GTEXT_CSS_Tree **, GTEXT_CSS_Result *);

static css_canon_parse_fn css_canon_parser_for(GTEXT_CSS_Tree_Kind kind) {
  switch (kind) {
  case GTEXT_CSS_TREE_STYLESHEET: return gtext_css_parse_stylesheet;
  case GTEXT_CSS_TREE_STYLESHEET_CONTENTS: return gtext_css_parse_stylesheet_contents;
  case GTEXT_CSS_TREE_DECLARATION_LIST: return gtext_css_parse_declaration_list;
  case GTEXT_CSS_TREE_BLOCK_CONTENTS: return gtext_css_parse_block_contents;
  case GTEXT_CSS_TREE_RULE: return gtext_css_parse_rule;
  case GTEXT_CSS_TREE_DECLARATION: return gtext_css_parse_declaration;
  case GTEXT_CSS_TREE_COMPONENT_VALUE: return gtext_css_parse_component_value;
  case GTEXT_CSS_TREE_COMPONENT_VALUE_LIST: return gtext_css_parse_component_values;
  default: return gtext_css_parse_comma_list;
  }
}

/*
 * Serialize a tree, read the text back with the same entry point and the
 * same options, and compare. Returns NULL when the round trip holds, or a
 * static description of how it failed (and, through `detail`, the text, the
 * two canonical forms; free() it). A tree that holds a syntax error has
 * nothing to write and holds trivially.
 */
static const char * css_round_trip(const GTEXT_CSS_Tree * tree, const GTEXT_CSS_Parse_Options * options,
    char ** detail) {
  if (detail) {
    *detail = NULL;
  }
  if (tree->syntax_error != GTEXT_CSS_SYNTAX_NONE) {
    return NULL;
  }
  char * text = NULL;
  size_t len = 0;
  if (gtext_css_serialize_tree_buffer(tree, NULL, &text, &len) != GTEXT_CSS_OK) {
    return "the serializer failed";
  }
  GTEXT_CSS_Parse_Options o = options ? *options : gtext_css_parse_options_default();
  GTEXT_CSS_Tree * again = NULL;
  const GTEXT_CSS_Status st = css_canon_parser_for(tree->kind)(text, len, &o, &again, NULL);
  const char * verdict = NULL;
  char * a = css_tree_canon(tree);
  char * b = NULL;
  if (st != GTEXT_CSS_OK) {
    verdict = "the serialization does not parse";
  }
  else {
    b = css_tree_canon(again);
    if (strcmp(a, b) != 0) {
      verdict = "the serialization parses to a different tree";
    }
    gtext_css_tree_free(again);
  }
  if (verdict && detail) {
    css_canon_buf d = {NULL, 0, 0};
    css_canon_puts(&d, "text: ");
    css_canon_put(&d, text, len);
    css_canon_puts(&d, "\nbefore: ");
    css_canon_puts(&d, a);
    css_canon_puts(&d, "\nafter:  ");
    css_canon_puts(&d, b ? b : "(none)");
    *detail = d.data;
  }
  free(a);
  free(b);
  gtext_css_serialize_buffer_free(NULL, text);
  return verdict;
}

#endif /* GHOTI_IO_GTEXT_TOOLS_CONFORMANCE_CSS_TREE_CANON_H */
