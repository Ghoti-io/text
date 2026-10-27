// SPDX-License-Identifier: LGPL-3.0-only
//
// Copyright (C) 2026 Corey Pennycuff
//
// This file is part of Ghoti.io Text.
//
// Ghoti.io Text is free software: you can redistribute it and/or modify it
// under the terms of the GNU Lesser General Public License version 3 as
// published by the Free Software Foundation.
//
// Ghoti.io Text is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
// for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

// The runner of the custom property vector gate (tools/conformance/css_variables_suite.py).
//
// It reads commands on standard input, written with \\ for a backslash, \n for a line feed and \t for a tab inside a field, and answers each:
//
//   REG TAB <flags> TAB <syntax> TAB <initial>
//       Register a custom property with this syntax (flag `n`: no syntax, which is `*`) and initial value, as CSS.registerProperty() does, and
//       freeze the registry. One line: `OK`, or `REFUSED TAB <status> TAB <detail>`.
//
//   PAGE ... END
//       The lines between are `SHEET TAB <css>` (a style sheet, in document order) and `EL TAB <id> TAB <parent> TAB <tag> TAB <element id> TAB
//       <classes> TAB <style attribute>` (the elements, parents first; the parent -1 is none). The runner collects the `@property` rules of the
//       sheets, runs the cascade of the library over the elements (the selectors it knows: a type or `*`, `#id`, `.class` and `:root`, with the
//       descendant and child combinators; `@supports` is evaluated, any other at-rule is an error) and computes each element's style from its
//       parent's. The answer is, for each element, `E TAB <id> TAB <status> TAB work=<n> peak=<n> limited=<n>` followed by `P TAB <name> TAB
//       <value>` for each property that has a computed value and `C TAB <name> TAB <text>` for each custom property, and a last line `.`.
//       A page that cannot be run is one line `ERR TAB <what>` and `.`.
//
// It reads no vector and no expectation: what is compared is the scorer's business.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ghoti.io/text/css.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char * unescape(const char * s, size_t n, size_t * out_len) {
  char * r = malloc(n + 1);
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '\\' && i + 1 < n) {
      i++;
      r[k++] = s[i] == 'n' ? '\n' : s[i] == 't' ? '\t' : s[i];
    }
    else {
      r[k++] = s[i];
    }
  }
  r[k] = '\0';
  *out_len = k;
  return r;
}

static void escape_out(const char * s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '\\') {
      fputs("\\\\", stdout);
    }
    else if (s[i] == '\n') {
      fputs("\\n", stdout);
    }
    else if (s[i] == '\t') {
      fputs("\\t", stdout);
    }
    else {
      fputc(s[i], stdout);
    }
  }
}

/* ---- fields ----------------------------------------------------------------------------------------------- */

typedef struct {
  char ** f;
  size_t n;
  char * raw;
} fields_t;

/// Split a line at TABs and unescape each field.
static fields_t split_line(char * line, size_t len) {
  fields_t r = {NULL, 0, NULL};
  size_t cap = 8;
  r.f = malloc(cap * sizeof *r.f);
  size_t start = 0;
  for (size_t i = 0; i <= len; i++) {
    if (i == len || line[i] == '\t') {
      size_t l = 0;
      if (r.n == cap) {
        cap *= 2;
        r.f = realloc(r.f, cap * sizeof *r.f);
      }
      r.f[r.n++] = unescape(line + start, i - start, &l);
      start = i + 1;
    }
  }
  return r;
}

static void free_fields(fields_t * r) {
  for (size_t i = 0; i < r->n; i++) {
    free(r->f[i]);
  }
  free(r->f);
}

/* ---- the page ---------------------------------------------------------------------------------------------- */

typedef struct {
  char * text;
  size_t length;
  GTEXT_CSS_Tree * tree;
} sheet_t;

typedef struct {
  int parent;
  char * tag;
  char * id;
  char ** classes;
  size_t nclasses;
  char * style;
} el_t;

typedef struct {
  const GTEXT_CSS_Registry * registry;
  sheet_t * sheets;
  size_t nsheets;
  el_t * els;
  size_t nels;
  GTEXT_CSS_Compute_Context * ctx;
  char error[256];
} page_t;

/* ---- @supports ------------------------------------------------------------------------------------------- */

static const char * skip_ws(const char * p, const char * end) {
  while (p < end && isspace((unsigned char)*p)) {
    p++;
  }
  return p;
}

/// The end of the parenthesized group starting at p (p[0] == '('): the index of the matching ')'; strings are stepped over.
static const char * match_paren(const char * p, const char * end) {
  int depth = 0;
  for (; p < end; p++) {
    if (*p == '"' || *p == '\'') {
      char q = *p++;
      while (p < end && *p != q) {
        if (*p == '\\') {
          p++;
        }
        p++;
      }
    }
    else if (*p == '(') {
      depth++;
    }
    else if (*p == ')') {
      depth--;
      if (depth == 0) {
        return p;
      }
    }
  }
  return NULL;
}

static int supports_cond(const page_t * pg, const char * p, const char * end, bool * known);

/// `(name: value)`, `(condition)` or `not (...)` ... : the truth of one <supports-in-parens>.
static int supports_in_parens(const page_t * pg, const char ** pp, const char * end, bool * known) {
  const char * p = skip_ws(*pp, end);
  if (p >= end) {
    *known = false;
    return 0;
  }
  if (*p != '(') {
    // selector(...) , font-tech(...) and the like: not evaluated here
    *known = false;
    return 0;
  }
  const char * close = match_paren(p, end);
  if (!close) {
    *known = false;
    return 0;
  }
  const char * in = p + 1;
  *pp = close + 1;
  // a nested condition begins with `(`, `not`
  const char * q = skip_ws(in, close);
  if (q < close && (*q == '(' || (close - q > 3 && strncmp(q, "not", 3) == 0 && isspace((unsigned char)q[3])))) {
    return supports_cond(pg, in, close, known);
  }
  const char * colon = memchr(in, ':', (size_t)(close - in));
  if (!colon) {
    *known = false;
    return 0;
  }
  const char * ns = skip_ws(in, colon);
  const char * ne = colon;
  while (ne > ns && isspace((unsigned char)ne[-1])) {
    ne--;
  }
  const char * vs = skip_ws(colon + 1, close);
  const char * ve = close;
  while (ve > vs && isspace((unsigned char)ve[-1])) {
    ve--;
  }
  // `(color: red !important)`: a declaration in a condition may carry the priority, once ("!important !important" is not a value)
  if (ve - vs >= 10 && strncasecmp(ve - 10, "!important", 10) == 0) {
    ve -= 10;
    while (ve > vs && isspace((unsigned char)ve[-1])) {
      ve--;
    }
  }
  GTEXT_CSS_Property_Value pv;
  memset(&pv, 0, sizeof pv);
  const GTEXT_CSS_Status st = gtext_css_parse_property_text(pg->registry, ns, (size_t)(ne - ns), vs, (size_t)(ve - vs), NULL, &pv);
  const bool ok = st == GTEXT_CSS_OK && (pv.outcome == GTEXT_CSS_PROP_RESULT_VALID || pv.outcome == GTEXT_CSS_PROP_RESULT_WIDE ||
      pv.outcome == GTEXT_CSS_PROP_RESULT_PENDING);
  gtext_css_property_value_free(&pv);
  return ok;
}

static int supports_cond(const page_t * pg, const char * p, const char * end, bool * known) {
  p = skip_ws(p, end);
  if (end - p > 3 && strncmp(p, "not", 3) == 0 && isspace((unsigned char)p[3])) {
    p += 3;
    const int v = supports_in_parens(pg, &p, end, known);
    return !v;
  }
  int value = supports_in_parens(pg, &p, end, known);
  for (;;) {
    p = skip_ws(p, end);
    if (p >= end) {
      return value;
    }
    bool is_and = end - p > 3 && strncmp(p, "and", 3) == 0 && isspace((unsigned char)p[3]);
    bool is_or = end - p > 2 && strncmp(p, "or", 2) == 0 && isspace((unsigned char)p[2]);
    if (!is_and && !is_or) {
      *known = false;
      return 0;
    }
    p += is_and ? 3 : 2;
    const int next = supports_in_parens(pg, &p, end, known);
    value = is_and ? (value && next) : (value || next);
  }
}

/// Whether an `@supports` rule applies. *known is false for a condition this evaluator does not take.
static bool supports_applies(const page_t * pg, const sheet_t * sh, const GTEXT_CSS_Rule * rule, bool * known) {
  *known = true;
  const char * text = sh->text + rule->prelude_span.offset;
  const size_t n = rule->prelude_span.end - rule->prelude_span.offset;
  return supports_cond(pg, text, text + n, known) != 0;
}

/* ---- selectors -------------------------------------------------------------------------------------------- */

typedef struct {
  char tag[64];       // empty: any
  char id[64];
  char cls[8][64];
  size_t ncls;
  bool root;
  char comb;          // the combinator that joins this compound to the one before: ' ' or '>'
} compound_t;

typedef struct {
  compound_t c[16];
  size_t n;
} selector_t;

static bool lower_eq(const char * a, const char * b) {
  return strcasecmp(a, b) == 0;
}

static bool parse_selector(const GTEXT_CSS_Component_Value * v, size_t n, selector_t * out) {
  memset(out, 0, sizeof *out);
  compound_t cur;
  memset(&cur, 0, sizeof cur);
  cur.comb = ' ';
  bool have = false, pending_ws = false;
  char next_comb = ' ';
  for (size_t i = 0; i < n; i++) {
    const GTEXT_CSS_Component_Value * x = &v[i];
    if (x->kind != GTEXT_CSS_VALUE_TOKEN) {
      return false;
    }
    if (x->type == GTEXT_CSS_TOKEN_WHITESPACE) {
      if (have) {
        pending_ws = true;
      }
      continue;
    }
    if (x->type == GTEXT_CSS_TOKEN_DELIM && x->delim == '>') {
      if (!have || out->n == 16) {
        return false;
      }
      out->c[out->n++] = cur;
      memset(&cur, 0, sizeof cur);
      have = false;
      pending_ws = false;
      next_comb = '>';
      cur.comb = '>';
      continue;
    }
    if (pending_ws) {
      if (out->n == 16) {
        return false;
      }
      out->c[out->n++] = cur;
      memset(&cur, 0, sizeof cur);
      cur.comb = ' ';
      have = false;
      pending_ws = false;
      next_comb = ' ';
    }
    (void)next_comb;
    have = true;
    if (x->type == GTEXT_CSS_TOKEN_IDENT && cur.tag[0] == '\0' && cur.id[0] == '\0' && cur.ncls == 0 && !cur.root) {
      snprintf(cur.tag, sizeof cur.tag, "%.*s", (int)x->value.length, x->value.data);
    }
    else if (x->type == GTEXT_CSS_TOKEN_DELIM && x->delim == '*') {
      // universal: any
    }
    else if (x->type == GTEXT_CSS_TOKEN_HASH) {
      snprintf(cur.id, sizeof cur.id, "%.*s", (int)x->value.length, x->value.data);
    }
    else if (x->type == GTEXT_CSS_TOKEN_DELIM && x->delim == '.' && i + 1 < n && v[i + 1].type == GTEXT_CSS_TOKEN_IDENT && cur.ncls < 8) {
      snprintf(cur.cls[cur.ncls++], 64, "%.*s", (int)v[i + 1].value.length, v[i + 1].value.data);
      i++;
    }
    else if (x->type == GTEXT_CSS_TOKEN_COLON && i + 1 < n && v[i + 1].type == GTEXT_CSS_TOKEN_IDENT && v[i + 1].value.length == 4 &&
        strncasecmp(v[i + 1].value.data, "root", 4) == 0) {
      cur.root = true;
      i++;
    }
    else {
      return false;
    }
  }
  if (!have || out->n == 16) {
    return false;
  }
  out->c[out->n++] = cur;
  return true;
}

static bool compound_matches(const page_t * pg, const compound_t * c, size_t e) {
  const el_t * el = &pg->els[e];
  if (c->tag[0] && !lower_eq(c->tag, el->tag)) {
    return false;
  }
  if (c->id[0] && strcmp(c->id, el->id) != 0) {
    return false;
  }
  for (size_t k = 0; k < c->ncls; k++) {
    bool found = false;
    for (size_t j = 0; j < el->nclasses; j++) {
      found = found || strcmp(c->cls[k], el->classes[j]) == 0;
    }
    if (!found) {
      return false;
    }
  }
  if (c->root && e != 0) {
    return false;
  }
  return true;
}

static bool selector_matches_at(const page_t * pg, const selector_t * s, size_t idx, size_t e) {
  if (!compound_matches(pg, &s->c[idx], e)) {
    return false;
  }
  if (idx == 0) {
    return true;
  }
  const char comb = s->c[idx].comb;
  int p = pg->els[e].parent;
  if (comb == '>') {
    return p >= 0 && selector_matches_at(pg, s, idx - 1, (size_t)p);
  }
  while (p >= 0) {
    if (selector_matches_at(pg, s, idx - 1, (size_t)p)) {
      return true;
    }
    p = pg->els[p].parent;
  }
  return false;
}

static GTEXT_CSS_Specificity specificity_of(const selector_t * s) {
  GTEXT_CSS_Specificity sp = {0, 0, 0};
  for (size_t i = 0; i < s->n; i++) {
    sp.a += s->c[i].id[0] ? 1 : 0;
    sp.b += (uint32_t)s->c[i].ncls + (s->c[i].root ? 1 : 0);
    sp.c += s->c[i].tag[0] ? 1 : 0;
  }
  return sp;
}

/* ---- the cascade over an element ------------------------------------------------------------------------ */

typedef struct {
  page_t * pg;
  size_t el;
  GTEXT_CSS_Candidates * set;
  uint32_t sheet;
  uint32_t rule;
  const sheet_t * cur;
  GTEXT_CSS_Status status;
  char why[160];
} walk_t;

static void walk_rules(walk_t * w, const GTEXT_CSS_Rule * rules, size_t count);

static void walk_items(walk_t * w, const GTEXT_CSS_Block_Item * items, size_t count) {
  for (size_t i = 0; i < count && w->status == GTEXT_CSS_OK; i++) {
    if (items[i].kind == GTEXT_CSS_ITEM_RULE) {
      walk_rules(w, items[i].rule, 1);
    }
  }
}

static void add_declarations(walk_t * w, const GTEXT_CSS_Declaration * d, size_t n, GTEXT_CSS_Specificity sp, bool attached) {
  for (size_t k = 0; k < n && w->status == GTEXT_CSS_OK; k++) {
    GTEXT_CSS_Cascade_Entry keys;
    memset(&keys, 0, sizeof keys);
    keys.origin = GTEXT_CSS_ORIGIN_AUTHOR;
    keys.important = d[k].important;
    keys.element_attached = attached;
    keys.specificity = sp;
    keys.sheet = w->sheet;
    keys.rule = w->rule;
    keys.declaration = (uint32_t)k;
    GTEXT_CSS_Property_Outcome outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
    GTEXT_CSS_Cascade_Result res;
    memset(&res, 0, sizeof res);
    const GTEXT_CSS_Status st = gtext_css_candidates_add_declaration(w->set, &keys, &d[k], NULL, &outcome, &res);
    if (st != GTEXT_CSS_OK && st != GTEXT_CSS_E_UNSUPPORTED) {
      w->status = st;
      snprintf(w->why, sizeof w->why, "adding a declaration: %s %s", gtext_css_status_string(st), res.name);
    }
  }
}

static void walk_rules(walk_t * w, const GTEXT_CSS_Rule * rules, size_t count) {
  for (size_t i = 0; i < count && w->status == GTEXT_CSS_OK; i++) {
    const GTEXT_CSS_Rule * r = &rules[i];
    w->rule++;
    if (r->kind == GTEXT_CSS_RULE_AT) {
      char name[32];
      snprintf(name, sizeof name, "%.*s", (int)r->name.length, r->name.data);
      if (strcasecmp(name, "supports") == 0) {
        bool known = true;
        const bool applies = supports_applies(w->pg, w->cur, r, &known);
        if (!known) {
          w->status = GTEXT_CSS_E_UNSUPPORTED;
          snprintf(w->why, sizeof w->why, "an @supports condition this runner does not evaluate");
        }
        else if (applies) {
          walk_items(w, r->contents, r->content_count);
        }
      }
      else if (strcasecmp(name, "property") == 0) {
        // collected apart
      }
      else {
        w->status = GTEXT_CSS_E_UNSUPPORTED;
        snprintf(w->why, sizeof w->why, "an @%s rule", name);
      }
      continue;
    }
    if (r->kind != GTEXT_CSS_RULE_QUALIFIED) {
      continue;
    }
    // the prelude is a selector list: split at the top-level commas
    size_t start = 0;
    GTEXT_CSS_Specificity best = {0, 0, 0};
    bool matched = false;
    for (size_t k = 0; k <= r->prelude_count; k++) {
      if (k == r->prelude_count || (r->prelude[k].kind == GTEXT_CSS_VALUE_TOKEN && r->prelude[k].type == GTEXT_CSS_TOKEN_COMMA)) {
        selector_t sel;
        if (!parse_selector(r->prelude + start, k - start, &sel)) {
          w->status = GTEXT_CSS_E_UNSUPPORTED;
          snprintf(w->why, sizeof w->why, "a selector this runner does not match");
          return;
        }
        if (selector_matches_at(w->pg, &sel, sel.n - 1, w->el)) {
          const GTEXT_CSS_Specificity sp = specificity_of(&sel);
          if (!matched || gtext_css_specificity_compare(sp, best) > 0) {
            best = sp;
          }
          matched = true;
        }
        start = k + 1;
      }
    }
    if (matched) {
      add_declarations(w, r->declarations, r->declaration_count, best, false);
    }
  }
}

/* ---- @property rules ----------------------------------------------------------------------------------- */

typedef struct {
  const page_t * pg;
  const sheet_t * sheet;
} active_t;

static bool rule_active(void * user, const GTEXT_CSS_Rule * rule) {
  const active_t * a = user;
  char name[32];
  snprintf(name, sizeof name, "%.*s", (int)rule->name.length, rule->name.data);
  if (strcasecmp(name, "supports") != 0) {
    return false;
  }
  bool known = true;
  return supports_applies(a->pg, a->sheet, rule, &known) && known;
}

/* ---- running a page --------------------------------------------------------------------------------- */

/// A value that is one color as `rgb(r g b / a)`: a computed color is written as a keyword where it was one, and two pages that compute the same color
/// may have written it differently. Anything else is returned as it is (false).
static bool canonical_color(const char * text, size_t n, char * out, size_t cap) {
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  GTEXT_CSS_Tree * tree = NULL;
  if (gtext_css_parse_component_values(text, n, &po, &tree, NULL) != GTEXT_CSS_OK) {
    return false;
  }
  bool done = false;
  const GTEXT_CSS_Component_Value * only = NULL;
  size_t real = 0;
  for (size_t i = 0; i < tree->value_count; i++) {
    if (!(tree->values[i].kind == GTEXT_CSS_VALUE_TOKEN && tree->values[i].type == GTEXT_CSS_TOKEN_WHITESPACE)) {
      only = &tree->values[i];
      real++;
    }
  }
  if (real == 1) {
    bool valid = false;
    GTEXT_CSS_Color * color = NULL;
    if (gtext_css_color_parse(only, NULL, &valid, &color, NULL) == GTEXT_CSS_OK && valid) {
      double rgba[4];
      if (gtext_css_color_srgb(color, rgba)) {
        snprintf(out, cap, "rgb(%g %g %g / %g)", rgba[0], rgba[1], rgba[2], rgba[3]);
        done = true;
      }
      gtext_css_color_free(color);
    }
  }
  gtext_css_tree_free(tree);
  return done;
}

static void print_value(const GTEXT_CSS_Value * root) {
  char * out = NULL;
  size_t n = 0;
  if (root && gtext_css_serialize_value_buffer(root, NULL, &out, &n) == GTEXT_CSS_OK) {
    char canon[96];
    if (canonical_color(out, n, canon, sizeof canon)) {
      escape_out(canon, strlen(canon));
    }
    else {
      escape_out(out, n);
    }
    gtext_css_serialize_buffer_free(NULL, out);
  }
}

static void print_style(const GTEXT_CSS_Registry * reg, const GTEXT_CSS_Computed_Style * s) {
  const size_t count = gtext_css_registry_property_count(reg);
  for (size_t id = 1; id <= count; id++) {
    GTEXT_CSS_Property_Info info;
    if (!gtext_css_registry_property_info(reg, (GTEXT_CSS_Property_Id)id, &info) || (info.flags & (GTEXT_CSS_PROP_SHORTHAND | GTEXT_CSS_PROP_ALIAS))) {
      continue;
    }
    const GTEXT_CSS_Value * root = gtext_css_computed_root(s, (GTEXT_CSS_Property_Id)id);
    if (!root) {
      continue;
    }
    fputs("P\t", stdout);
    escape_out(info.name, info.name_length);
    fputc('\t', stdout);
    print_value(root);
    fputc('\n', stdout);
  }
  for (size_t i = 0; i < gtext_css_computed_custom_count(s); i++) {
    GTEXT_CSS_String name;
    const GTEXT_CSS_Custom_Value * v = NULL;
    if (!gtext_css_computed_custom_at(s, i, &name, &v)) {
      continue;
    }
    char * text = NULL;
    size_t n = 0;
    if (gtext_css_custom_value_text(v, NULL, &text, &n) != GTEXT_CSS_OK) {
      continue;
    }
    fputs("C\t", stdout);
    escape_out(name.data, name.length);
    fputc('\t', stdout);
    escape_out(text, n);
    fputc('\n', stdout);
    gtext_css_serialize_buffer_free(NULL, text);
  }
}

static void fail_page(const char * what) {
  fputs("ERR\t", stdout);
  escape_out(what, strlen(what));
  fputs("\n.\n", stdout);
}

static void run_page(page_t * pg) {
  const GTEXT_CSS_Registry * reg = pg->registry;
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  GTEXT_CSS_At_Property_Set * rules = NULL;
  GTEXT_CSS_Compute_Context * ctx = NULL;
  GTEXT_CSS_Computed_Style ** styles = calloc(pg->nels ? pg->nels : 1, sizeof *styles);
  char err[256] = "";
  for (size_t i = 0; i < pg->nsheets && !err[0]; i++) {
    GTEXT_CSS_Result res;
    memset(&res, 0, sizeof res);
    const GTEXT_CSS_Status st = gtext_css_parse_stylesheet(pg->sheets[i].text, pg->sheets[i].length, &po, &pg->sheets[i].tree, &res);
    if (st != GTEXT_CSS_OK) {
      snprintf(err, sizeof err, "parsing a style sheet: %s", gtext_css_status_string(st));
    }
    gtext_css_result_free(&res);
  }
  active_t act = {pg, NULL};
  if (!err[0]) {
    GTEXT_CSS_At_Property_Options ao = gtext_css_at_property_options_default();
    ao.user = &act;
    ao.active = rule_active;
    if (gtext_css_at_property_set_new(reg, &ao, &rules) != GTEXT_CSS_OK) {
      snprintf(err, sizeof err, "making the @property set");
    }
    for (size_t i = 0; i < pg->nsheets && !err[0]; i++) {
      act.sheet = &pg->sheets[i];
      GTEXT_CSS_At_Property_Result r;
      memset(&r, 0, sizeof r);
      const GTEXT_CSS_Status st = gtext_css_at_property_collect(rules, pg->sheets[i].tree, &r);
      if (st != GTEXT_CSS_OK) {
        snprintf(err, sizeof err, "collecting @property rules: %s %s", gtext_css_status_string(st), r.detail ? r.detail : "");
      }
    }
  }
  if (!err[0]) {
    GTEXT_CSS_Compute_Options co = gtext_css_compute_options_default();
    co.property_rules = rules;
    GTEXT_CSS_Compute_Result cr;
    memset(&cr, 0, sizeof cr);
    const GTEXT_CSS_Environment env = gtext_css_environment_default();
    const GTEXT_CSS_Status st = gtext_css_compute_context_new(reg, &env, &co, &ctx, &cr);
    if (st != GTEXT_CSS_OK) {
      snprintf(err, sizeof err, "making the compute context: %s", gtext_css_status_string(st));
    }
  }
  if (err[0]) {
    fail_page(err);
    goto done;
  }
  for (size_t e = 0; e < pg->nels; e++) {
    GTEXT_CSS_Candidates * set = NULL;
    GTEXT_CSS_Status st = gtext_css_candidates_new(reg, NULL, NULL, &set);
    walk_t w;
    memset(&w, 0, sizeof w);
    w.pg = pg;
    w.el = e;
    w.set = set;
    if (st == GTEXT_CSS_OK) {
      for (size_t i = 0; i < pg->nsheets && w.status == GTEXT_CSS_OK; i++) {
        w.sheet = (uint32_t)i;
        w.rule = 0;
        w.cur = &pg->sheets[i];
        walk_rules(&w, pg->sheets[i].tree->rules, pg->sheets[i].tree->rule_count);
      }
    }
    GTEXT_CSS_Tree * attr = NULL;
    if (st == GTEXT_CSS_OK && w.status == GTEXT_CSS_OK && pg->els[e].style[0]) {
      GTEXT_CSS_Result res;
      memset(&res, 0, sizeof res);
      if (gtext_css_parse_declaration_list(pg->els[e].style, strlen(pg->els[e].style), &po, &attr, &res) == GTEXT_CSS_OK) {
        w.sheet = (uint32_t)pg->nsheets;
        w.rule = 0;
        add_declarations(&w, attr->declarations, attr->declaration_count, (GTEXT_CSS_Specificity){0, 0, 0}, true);
      }
      gtext_css_result_free(&res);
    }
    GTEXT_CSS_Cascaded * cascaded = NULL;
    GTEXT_CSS_Declaration_Block * block = NULL;
    GTEXT_CSS_Compute_Result cr;
    memset(&cr, 0, sizeof cr);
    const char * status_name = "OK";
    if (st != GTEXT_CSS_OK) {
      status_name = gtext_css_status_string(st);
    }
    else if (w.status != GTEXT_CSS_OK) {
      status_name = w.why[0] ? w.why : gtext_css_status_string(w.status);
    }
    else if ((st = gtext_css_cascade(set, NULL, &cascaded, NULL)) != GTEXT_CSS_OK) {
      status_name = gtext_css_status_string(st);
    }
    else if ((st = gtext_css_cascaded_to_block(cascaded, NULL, &block)) != GTEXT_CSS_OK) {
      status_name = gtext_css_status_string(st);
    }
    else {
      const GTEXT_CSS_Computed_Style * parent = pg->els[e].parent >= 0 ? styles[pg->els[e].parent] : NULL;
      st = gtext_css_compute_style(ctx, block, parent, NULL, &styles[e], &cr);
      if (st != GTEXT_CSS_OK) {
        status_name = gtext_css_status_string(st);
      }
    }
    printf("E\t%zu\t", e);
    escape_out(status_name, strlen(status_name));
    printf("\twork=%zu peak=%llu limited=%llu\n", cr.work, (unsigned long long)cr.substitution_peak, (unsigned long long)cr.substitution_limited);
    if (styles[e]) {
      print_style(reg, styles[e]);
    }
    gtext_css_block_free(block);
    gtext_css_cascaded_free(cascaded);
    gtext_css_candidates_free(set);
    gtext_css_tree_free(attr);
  }
  fputs(".\n", stdout);
done:
  for (size_t e = 0; e < pg->nels; e++) {
    gtext_css_computed_free(styles[e]);
  }
  free(styles);
  gtext_css_compute_context_free(ctx);
  gtext_css_at_property_set_free(rules);
  for (size_t i = 0; i < pg->nsheets; i++) {
    gtext_css_tree_free(pg->sheets[i].tree);
    pg->sheets[i].tree = NULL;
  }
}

static void free_page(page_t * pg) {
  for (size_t i = 0; i < pg->nsheets; i++) {
    free(pg->sheets[i].text);
  }
  free(pg->sheets);
  for (size_t i = 0; i < pg->nels; i++) {
    free(pg->els[i].tag);
    free(pg->els[i].id);
    for (size_t k = 0; k < pg->els[i].nclasses; k++) {
      free(pg->els[i].classes[k]);
    }
    free(pg->els[i].classes);
    free(pg->els[i].style);
  }
  free(pg->els);
  memset(pg, 0, sizeof *pg);
}

/* ---- registration ------------------------------------------------------------------------------------- */

static void run_reg(const fields_t * f) {
  if (f->n < 4) {
    puts("REFUSED\tprotocol\tREG needs flags, syntax and initial value");
    return;
  }
  GTEXT_CSS_Registry_Builder * b = NULL;
  GTEXT_CSS_Registry * r = NULL;
  GTEXT_CSS_Registry_Result res;
  memset(&res, 0, sizeof res);
  GTEXT_CSS_Status st = gtext_css_registry_builder_new(NULL, &b);
  if (st == GTEXT_CSS_OK) {
    GTEXT_CSS_Custom_Def def;
    memset(&def, 0, sizeof def);
    def.name = "--wpt";
    def.syntax = strchr(f->f[1], 'n') ? NULL : f->f[2];
    def.inherits = false;
    def.initial_value = f->f[3];
    st = gtext_css_registry_register_custom(b, &def, &res);
    if (st == GTEXT_CSS_OK) {
      st = gtext_css_registry_freeze(b, &r, &res);
      if (st == GTEXT_CSS_OK) {
        b = NULL;
      }
    }
  }
  if (st == GTEXT_CSS_OK) {
    puts("OK");
  }
  else {
    printf("REFUSED\t%s\t", gtext_css_status_string(st));
    const char * d = res.detail ? res.detail : "";
    escape_out(d, strlen(d));
    fputc('\n', stdout);
  }
  gtext_css_registry_builder_free(b);
  gtext_css_registry_free(r);
}

/* ---- main ---------------------------------------------------------------------------------------------- */

int main(void) {
  GTEXT_CSS_Registry * registry = NULL;
  if (gtext_css_registry_new(NULL, &registry) != GTEXT_CSS_OK || gtext_css_registry_prepare(registry) != GTEXT_CSS_OK) {
    fputs("the registry could not be made\n", stderr);
    return 1;
  }
  char * line = NULL;
  size_t cap = 0;
  ssize_t len;
  page_t pg;
  memset(&pg, 0, sizeof pg);
  bool in_page = false;
  while ((len = getline(&line, &cap, stdin)) >= 0) {
    if (len > 0 && line[len - 1] == '\n') {
      line[--len] = '\0';
    }
    if (!in_page && strcmp(line, "PAGE") == 0) {
      in_page = true;
      pg.registry = registry;
      continue;
    }
    if (in_page) {
      if (strcmp(line, "END") == 0) {
        run_page(&pg);
        free_page(&pg);
        in_page = false;
        fflush(stdout);
        continue;
      }
      fields_t f = split_line(line, (size_t)len);
      if (f.n >= 2 && strcmp(f.f[0], "SHEET") == 0) {
        pg.sheets = realloc(pg.sheets, (pg.nsheets + 1) * sizeof *pg.sheets);
        pg.sheets[pg.nsheets].text = strdup(f.f[1]);
        pg.sheets[pg.nsheets].length = strlen(f.f[1]);
        pg.sheets[pg.nsheets].tree = NULL;
        pg.nsheets++;
      }
      else if (f.n >= 7 && strcmp(f.f[0], "EL") == 0) {
        pg.els = realloc(pg.els, (pg.nels + 1) * sizeof *pg.els);
        el_t * e = &pg.els[pg.nels++];
        memset(e, 0, sizeof *e);
        e->parent = atoi(f.f[2]);
        e->tag = strdup(f.f[3]);
        e->id = strdup(f.f[4]);
        e->style = strdup(f.f[6]);
        char * cl = strdup(f.f[5]);
        for (char * tok = strtok(cl, " "); tok; tok = strtok(NULL, " ")) {
          e->classes = realloc(e->classes, (e->nclasses + 1) * sizeof *e->classes);
          e->classes[e->nclasses++] = strdup(tok);
        }
        free(cl);
      }
      free_fields(&f);
      continue;
    }
    fields_t f = split_line(line, (size_t)len);
    if (f.n >= 1 && strcmp(f.f[0], "REG") == 0) {
      run_reg(&f);
    }
    else {
      puts("ERR\tunknown command");
    }
    free_fields(&f);
    fflush(stdout);
  }
  free(line);
  gtext_css_registry_free(registry);
  return 0;
}
