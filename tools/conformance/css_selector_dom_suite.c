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

// The runner of the selector queries gate (tools/conformance/css_selector_dom_suite.py).
//
// It builds the contexts the DOM tests of web-platform-tests' dom/nodes run their selectors in -- the fixture `ParentNode-querySelector-All-content.html` parsed by this library's HTML
// parser, the special elements that `ParentNode-querySelector-All.js` adds with script (setupSpecialElements), the in-document element `#root` with a cloned copy of itself added
// to the body, a detached clone and a fragment holding a clone -- and answers requests on standard input, one a line, the fields separated by TAB and written with \\ for a backslash,
// \n for a line feed and \t for a tab:
//
//   Q TAB <context> TAB <selector>      querySelectorAll() on the context (document, element, detached or fragment), and querySelector(), which must be the first of the others
//   M TAB <id> TAB <selector>           matches() on the first element of the document that has the id
//
// The answers: `OK TAB <ids>` (the `id` attributes of the elements, in the order returned, separated by a space; an element with none is `?`), `SYNTAX`, `TRUE`, `FALSE`, `NOELEM`,
// `INCONSISTENT` (the first match is not the first of all), or `ERR TAB <status>`.
//
// The page loads the fixture with the fragment `#target`, so `:target` matches the element with that id; nothing else is dynamic.
#include <ghoti.io/text/css.h>
#include <ghoti.io/text/html.h>
#include <ghoti.io/text/markup.h>
#include <ghoti.io/text/markup/markup_query.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GTEXT_Markup_Document * g_doc;      // the document tests, with no copy of the root in the body
static GTEXT_Markup_Document * g_doc2;     // the in-document element tests: the copy is in the body
static GTEXT_Markup_Node * g_element;      // #root of g_doc2
static GTEXT_Markup_Node * g_detached;
static GTEXT_Markup_Node * g_fragment;

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

static const GTEXT_Markup_Node * find_id(const GTEXT_Markup_Node * root, const char * id) {
  for (const GTEXT_Markup_Node * n = root; n; n = gtext_markup_node_next_in_order(root, n, false)) {
    GTEXT_Markup_Attribute a;
    if (gtext_markup_node_kind(n) == GTEXT_MARKUP_KIND_ELEMENT && gtext_markup_element_get_attribute(n, NULL, "id", &a) && strcmp(a.value, id) == 0) {
      return n;
    }
  }
  return NULL;
}

static GTEXT_Markup_Node * must_create(GTEXT_Markup_Document * doc, const char * ns, const char * local, const char * id) {
  GTEXT_Markup_Node * n = NULL;
  if (gtext_markup_create_element(doc, ns, NULL, local, &n) != GTEXT_MARKUP_OK) {
    fprintf(stderr, "css_selector_dom_suite: cannot create an element\n");
    exit(2);
  }
  if (id) {
    gtext_markup_element_set_attribute(n, NULL, NULL, "id", id, strlen(id));
  }
  return n;
}

#define HTML_NS "http://www.w3.org/1999/xhtml"

// ParentNode-querySelector-All.js, setupSpecialElements()
static void setup_special(GTEXT_Markup_Document * doc, GTEXT_Markup_Node * parent) {
  gtext_markup_append_child(parent, must_create(doc, HTML_NS, "null", NULL));
  gtext_markup_append_child(parent, must_create(doc, HTML_NS, "undefined", NULL));
  GTEXT_Markup_Node * any_ns = must_create(doc, HTML_NS, "div", "any-namespace");
  GTEXT_Markup_Node * no_ns = must_create(doc, HTML_NS, "div", "no-namespace");
  const char * const namespaces[4] = {HTML_NS, HTML_NS, NULL, "http://www.example.org/ns"};
  const char * const prefixes[2] = {"any-namespace-div", "no-namespace-div"};
  GTEXT_Markup_Node * holders[2] = {any_ns, no_ns};
  for (int h = 0; h < 2; h++) {
    for (int i = 0; i < 4; i++) {
      char id[64];
      snprintf(id, sizeof id, "%s%d", prefixes[h], i + 1);
      gtext_markup_append_child(holders[h], must_create(doc, namespaces[i], "div", id));
    }
    gtext_markup_append_child(parent, holders[h]);
  }
  GTEXT_Markup_Node * span = (GTEXT_Markup_Node *)find_id(gtext_markup_document_root(doc), "attr-presence-i1");
  if (span) {
    gtext_markup_element_set_attribute(span, "http://www.example.org/ns", NULL, "title", "", 0);
  }
}

static void annotate_clone(GTEXT_Markup_Node * root) {
  for (GTEXT_Markup_Node * n = root; n; n = gtext_markup_node_next_in_order(root, n, false)) {
    if (gtext_markup_node_kind(n) == GTEXT_MARKUP_KIND_ELEMENT) {
      gtext_markup_element_set_attribute(n, NULL, NULL, "data-clone", "", 0);
    }
  }
}

static GTEXT_Markup_Document * parse_fixture(const char * data, size_t len) {
  GTEXT_Markup_Document * doc = NULL;
  GTEXT_HTML_Result r;
  memset(&r, 0, sizeof r);
  if (gtext_html_parse(data, len, NULL, &doc, &r) != GTEXT_HTML_OK) {
    fprintf(stderr, "css_selector_dom_suite: the fixture does not parse\n");
    exit(2);
  }
  gtext_html_result_free(&r);
  return doc;
}

static uint64_t target_state(void * user, const GTEXT_Markup_Node * e) {
  (void)user;
  // the element the URL's fragment refers to, in the document the page loaded: an element of a detached tree or a fragment is not in it
  GTEXT_Markup_Attribute a;
  if (!gtext_markup_element_get_attribute(e, NULL, "id", &a) || strcmp(a.value, "target") != 0) {
    return 0u;
  }
  const GTEXT_Markup_Node * top = e;
  while (gtext_markup_node_parent(top)) {
    top = gtext_markup_node_parent(top);
  }
  return gtext_markup_node_kind(top) == GTEXT_MARKUP_KIND_DOCUMENT ? GTEXT_CSS_STATE_TARGET : 0u;
}

static GTEXT_Markup_Query_Options options(void) {
  GTEXT_Markup_Query_Options o = gtext_markup_query_options_default();
  o.state = target_state;
  return o;
}

static void print_ids(GTEXT_Markup_Node ** nodes, size_t n) {
  fputs("OK\t", stdout);
  for (size_t i = 0; i < n; i++) {
    GTEXT_Markup_Attribute a;
    if (i) {
      fputc(' ', stdout);
    }
    if (gtext_markup_element_get_attribute(nodes[i], NULL, "id", &a)) {
      escape_out(a.value, a.value_length);
    }
    else {
      fputc('?', stdout);
    }
  }
  fputc('\n', stdout);
}

int main(int argc, char ** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: css_selector_dom_suite <ParentNode-querySelector-All-content.html>\n");
    return 2;
  }
  FILE * f = fopen(argv[1], "rb");
  if (!f) {
    fprintf(stderr, "css_selector_dom_suite: cannot open %s\n", argv[1]);
    return 2;
  }
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  char * data = malloc((size_t)size + 1);
  if (fread(data, 1, (size_t)size, f) != (size_t)size) {
    return 2;
  }
  fclose(f);
  g_doc = parse_fixture(data, (size_t)size);
  g_doc2 = parse_fixture(data, (size_t)size);
  free(data);
  GTEXT_Markup_Node * root = (GTEXT_Markup_Node *)find_id(gtext_markup_document_root(g_doc), "root");
  g_element = (GTEXT_Markup_Node *)find_id(gtext_markup_document_root(g_doc2), "root");
  if (!root || !g_element) {
    fprintf(stderr, "css_selector_dom_suite: the fixture has no #root\n");
    return 2;
  }
  setup_special(g_doc, root);
  setup_special(g_doc2, g_element);
  GTEXT_Markup_Node * out_of_scope = NULL;
  gtext_markup_clone(g_doc2, g_element, true, &out_of_scope);
  annotate_clone(out_of_scope);
  GTEXT_Markup_Node * body2 = (GTEXT_Markup_Node *)find_id(gtext_markup_document_root(g_doc2), "body");
  gtext_markup_append_child(body2, out_of_scope);
  gtext_markup_clone(g_doc, root, true, &g_detached);
  gtext_markup_create_fragment(g_doc, &g_fragment);
  GTEXT_Markup_Node * fc = NULL;
  gtext_markup_clone(g_doc, root, true, &fc);
  gtext_markup_append_child(g_fragment, fc);

  const GTEXT_Markup_Query_Options o = options();
  char * line = NULL;
  size_t cap = 0;
  ssize_t got;
  while ((got = getline(&line, &cap, stdin)) >= 0) {
    size_t n = (size_t)got;
    if (n && line[n - 1] == '\n') {
      n--;
    }
    char * tab1 = memchr(line, '\t', n);
    char * tab2 = tab1 ? memchr(tab1 + 1, '\t', n - (size_t)(tab1 + 1 - line)) : NULL;
    if (!tab1 || !tab2) {
      puts("ERR\tbad request");
      continue;
    }
    size_t slen;
    char * sel = unescape(tab2 + 1, n - (size_t)(tab2 + 1 - line), &slen);
    *tab2 = '\0';
    const char * arg = tab1 + 1;
    if (line[0] == 'Q') {
      const GTEXT_Markup_Node * scope = strcmp(arg, "document") == 0 ? gtext_markup_document_root(g_doc)
          : strcmp(arg, "element") == 0 ? g_element : strcmp(arg, "detached") == 0 ? g_detached : g_fragment;
      GTEXT_Markup_Node ** nodes = NULL;
      size_t count = 0;
      GTEXT_Markup_Node * first = NULL;
      GTEXT_Markup_Status st = gtext_markup_query_selector_all(scope, sel, slen, &o, &nodes, &count, NULL);
      if (st == GTEXT_MARKUP_E_SELECTOR) {
        puts("SYNTAX");
      }
      else if (st != GTEXT_MARKUP_OK) {
        printf("ERR\t%s\n", gtext_markup_status_string(st));
      }
      else {
        st = gtext_markup_query_selector(scope, sel, slen, &o, &first, NULL);
        if (st != GTEXT_MARKUP_OK || first != (count ? nodes[0] : NULL)) {
          puts("INCONSISTENT");
        }
        else {
          print_ids(nodes, count);
        }
        gtext_markup_query_free(&o, nodes);
      }
    }
    else {
      const GTEXT_Markup_Node * el = find_id(gtext_markup_document_root(g_doc), arg);
      if (!el) {
        puts("NOELEM");
      }
      else {
        bool m = false;
        const GTEXT_Markup_Status st = gtext_markup_matches(el, sel, slen, &o, &m, NULL);
        if (st == GTEXT_MARKUP_E_SELECTOR) {
          puts("SYNTAX");
        }
        else if (st != GTEXT_MARKUP_OK) {
          printf("ERR\t%s\n", gtext_markup_status_string(st));
        }
        else {
          puts(m ? "TRUE" : "FALSE");
        }
      }
    }
    free(sel);
  }
  free(line);
  gtext_markup_document_free(g_doc);
  gtext_markup_document_free(g_doc2);
  return 0;
}
