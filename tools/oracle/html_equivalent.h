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
 * html_equivalent: the comparison between the tree an XML parser reads from an
 * `xhtml`-mode document and the tree an HTML parser reads from the same bytes.
 * The rules are the ones in notes/text/XML-HTML-PLAN.md ("The Phase 3 exit item"),
 * eight of them, and this is the one place they are implemented: the unit tests
 * (tests/test-xhtml-writer.cpp) include this header, and so does the harness
 * that scores the writer against html5lib and parse5 (html_ours.c). A rule
 * changed here changes both, and cannot be weakened in one of them.
 *
 *   1. Compared from the document node down, children in order.
 *   2. Locations, prefixes and the order of attributes are not compared.
 *   3. The known representation difference: attributes in the xmlns namespace
 *      (declarations), and attributes named `xmlns` or `xmlns:*` in no namespace
 *      (how an HTML parser keeps them on an HTML element), are dropped from every
 *      element.
 *   4. Empty text nodes are dropped; adjacent text and CDATA are one text node.
 *   5. Elements are equal when namespace and local name are equal and the
 *      attribute sets, unordered, (namespace, local name, value) are equal.
 *   6. Comments are equal by data; doctypes by name, public and system id (an
 *      absent identifier and an empty one are the same here).
 *   7. Processing instructions compare unequal to everything.
 *   8. Anything else is a difference.
 *
 * The header is valid C17 and C++20 and defines only static functions, so that
 * either can include it. When two trees differ, htmleq_compare() also says where
 * (a path of element names with child indexes), what, and the names of the elements
 * in the subtrees where they first differ. Its result is 1, 0, or HTMLEQ_OOM and
 * HTMLEQ_TOO_DEEP, which are not an answer; the comparison recurses, so it stops at
 * HTMLEQ_MAX_DEPTH levels.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GTEXT_TOOLS_ORACLE_HTML_EQUIVALENT_H
#define GHOTI_IO_GTEXT_TOOLS_ORACLE_HTML_EQUIVALENT_H

#include <ghoti.io/text/markup.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HTMLEQ_NS_XMLNS "http://www.w3.org/2000/xmlns/"

/* Where two trees first differ, and how. Both are always NUL terminated. */
typedef struct {
  char path[256];
  char why[512];
  /* The names of the elements in the subtrees where the two trees first differ (both
     sides, once each, separated by spaces): what a caller needs to say *why*, from the
     place and not from the whole document. */
  char ctx[512];
} HtmlEq_Diff;

/* Results of htmleq_compare: 1 equivalent, 0 not, and these two, which are not "not". */
#define HTMLEQ_OOM (-1)
#define HTMLEQ_TOO_DEEP (-2)
#define HTMLEQ_MAX_DEPTH 2000

typedef struct {
  const GTEXT_Markup_Node * node; /* NULL for a merged text run */
  char * text;
  size_t length;
} HtmlEq_Item;

typedef struct {
  char * bytes; /* "ns|local=value", for sorting and comparing */
  size_t length;
} HtmlEq_Attr;

static inline const char * htmleq_str(const char * s) {
  return s ? s : "";
}

static inline int htmleq_attr_order(const void * a, const void * b) {
  const HtmlEq_Attr * x = (const HtmlEq_Attr *) a;
  const HtmlEq_Attr * y = (const HtmlEq_Attr *) b;
  const size_t n = x->length < y->length ? x->length : y->length;
  const int c = memcmp(x->bytes, y->bytes, n);
  if (c != 0) {
    return c;
  }
  return x->length < y->length ? -1 : x->length > y->length ? 1 : 0;
}

/* The element's attribute set under rules 2 and 3. Returns the count, or -1 on
 * allocation failure. *out is a malloc'd array the caller frees with
 * htmleq_free_attrs. */
static inline long htmleq_attrs(const GTEXT_Markup_Node * e, HtmlEq_Attr ** out) {
  const size_t n = gtext_markup_element_attribute_count(e);
  HtmlEq_Attr * list = (HtmlEq_Attr *) calloc(n ? n : 1, sizeof(HtmlEq_Attr));
  if (!list) {
    return -1;
  }
  size_t used = 0;
  for (size_t i = 0; i < n; i++) {
    GTEXT_Markup_Attribute a;
    gtext_markup_element_attribute_at(e, i, &a);
    if (a.ns && strcmp(a.ns, HTMLEQ_NS_XMLNS) == 0) {
      continue;
    }
    if (!a.ns && (strcmp(a.local, "xmlns") == 0 || strncmp(a.local, "xmlns:", 6) == 0)) {
      continue;
    }
    /* Length-prefixed fields, "<n>:<ns><n>:<local><value>": a separator character would
       let two different (namespace, name, value) triples make one key. */
    const size_t nl = a.ns ? strlen(a.ns) : 0;
    const size_t ll = strlen(a.local);
    char * s = (char *) malloc(nl + ll + a.value_length + 48);
    if (!s) {
      for (size_t j = 0; j < used; j++) {
        free(list[j].bytes);
      }
      free(list);
      return -1;
    }
    size_t at = (size_t) sprintf(s, "%zu:", nl);
    if (nl) {
      memcpy(s + at, a.ns, nl);
      at += nl;
    }
    at += (size_t) sprintf(s + at, "%zu:", ll);
    memcpy(s + at, a.local, ll);
    at += ll;
    if (a.value_length) {
      memcpy(s + at, a.value, a.value_length);
    }
    at += a.value_length;
    s[at] = '\0';
    list[used].bytes = s;
    list[used].length = at;
    used++;
  }
  qsort(list, used, sizeof(HtmlEq_Attr), htmleq_attr_order);
  *out = list;
  return (long) used;
}

static inline void htmleq_free_attrs(HtmlEq_Attr * list, long n) {
  for (long i = 0; i < n; i++) {
    free(list[i].bytes);
  }
  free(list);
}

/* The children of `parent` under rule 4. Returns the count or -1. */
static inline long htmleq_children(const GTEXT_Markup_Node * parent, HtmlEq_Item ** out) {
  size_t count = 0;
  for (const GTEXT_Markup_Node * c = gtext_markup_node_first_child(parent); c;
       c = gtext_markup_node_next_sibling(c)) {
    count++;
  }
  HtmlEq_Item * list = (HtmlEq_Item *) calloc(count ? count : 1, sizeof(HtmlEq_Item));
  if (!list) {
    return -1;
  }
  size_t used = 0;
  for (const GTEXT_Markup_Node * c = gtext_markup_node_first_child(parent); c;
       c = gtext_markup_node_next_sibling(c)) {
    const GTEXT_Markup_Kind k = gtext_markup_node_kind(c);
    if (k == GTEXT_MARKUP_KIND_TEXT || k == GTEXT_MARKUP_KIND_CDATA) {
      size_t len = 0;
      const char * d = gtext_markup_node_data(c, &len);
      if (len == 0) {
        continue;
      }
      if (used > 0 && list[used - 1].node == NULL) {
        HtmlEq_Item * last = &list[used - 1];
        char * grown = (char *) realloc(last->text, last->length + len);
        if (!grown) {
          goto fail;
        }
        memcpy(grown + last->length, d, len);
        last->text = grown;
        last->length += len;
      }
      else {
        char * copy = (char *) malloc(len);
        if (!copy) {
          goto fail;
        }
        memcpy(copy, d, len);
        list[used].node = NULL;
        list[used].text = copy;
        list[used].length = len;
        used++;
      }
    }
    else {
      list[used].node = c;
      list[used].text = NULL;
      list[used].length = 0;
      used++;
    }
  }
  *out = list;
  return (long) used;
fail:
  for (size_t j = 0; j < used; j++) {
    free(list[j].text);
  }
  free(list);
  return -1;
}

static inline void htmleq_free_children(HtmlEq_Item * list, long n) {
  for (long i = 0; i < n; i++) {
    free(list[i].text);
  }
  free(list);
}

static inline void htmleq_add_names(char * buf, size_t cap, const GTEXT_Markup_Node * root) {
  if (!root) {
    return;
  }
  for (const GTEXT_Markup_Node * n = root; n; n = gtext_markup_node_next_in_order(root, n, false)) {
    if (gtext_markup_node_kind(n) != GTEXT_MARKUP_KIND_ELEMENT) {
      continue;
    }
    const char * name = htmleq_str(gtext_markup_element_local_name(n));
    const size_t len = strlen(name);
    const size_t used = strlen(buf);
    /* once each: look for " name " in " buf " */
    int seen = 0;
    for (const char * p = buf; (p = strstr(p, name)) != NULL; p++) {
      if ((p == buf || p[-1] == ' ') && (p[len] == ' ' || p[len] == '\0')) {
        seen = 1;
        break;
      }
    }
    if (!seen && used + len + 2 < cap) {
      if (used) {
        buf[used] = ' ';
        memcpy(buf + used + 1, name, len + 1);
      }
      else {
        memcpy(buf, name, len + 1);
      }
    }
  }
}

static inline void htmleq_note(HtmlEq_Diff * d, const char * path, const char * what, const char * a,
    size_t alen, const char * b, size_t blen, const GTEXT_Markup_Node * na, const GTEXT_Markup_Node * nb) {
  if (!d) {
    return;
  }
  snprintf(d->path, sizeof(d->path), "%.250s", path);
  snprintf(d->why, sizeof(d->why), "%s: ours %.*s, theirs %.*s", what, (int) (alen > 120 ? 120 : alen), a,
      (int) (blen > 120 ? 120 : blen), b);
  d->ctx[0] = '\0';
  htmleq_add_names(d->ctx, sizeof(d->ctx), na);
  htmleq_add_names(d->ctx, sizeof(d->ctx), nb);
}

static inline const char * htmleq_kind_name(GTEXT_Markup_Kind k) {
  switch (k) {
    case GTEXT_MARKUP_KIND_ELEMENT: return "element";
    case GTEXT_MARKUP_KIND_TEXT: return "text";
    case GTEXT_MARKUP_KIND_CDATA: return "cdata";
    case GTEXT_MARKUP_KIND_COMMENT: return "comment";
    case GTEXT_MARKUP_KIND_DOCTYPE: return "doctype";
    case GTEXT_MARKUP_KIND_DOCUMENT: return "document";
    case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION: return "processing-instruction";
    default: return "other";
  }
}

/* 1 when equivalent, 0 when not (and `diff`, if given, says where and how), -1 when memory ran out. `path`
 * is the path of `a` so far; the caller starts with "". */
static inline int htmleq_cmp(const GTEXT_Markup_Node * a, const GTEXT_Markup_Node * b, const char * path,
    HtmlEq_Diff * diff, int depth) {
  if (depth > HTMLEQ_MAX_DEPTH) {
    return HTMLEQ_TOO_DEEP;
  }
  const GTEXT_Markup_Kind ka = gtext_markup_node_kind(a);
  const GTEXT_Markup_Kind kb = gtext_markup_node_kind(b);
  if (ka != kb) {
    const char * x = htmleq_kind_name(ka);
    const char * y = htmleq_kind_name(kb);
    htmleq_note(diff, path, "node kind", x, strlen(x), y, strlen(y), a, b);
    return 0;
  }
  char here[256];
  switch (ka) {
    case GTEXT_MARKUP_KIND_ELEMENT: {
      const char * na = htmleq_str(gtext_markup_element_namespace(a));
      const char * nb = htmleq_str(gtext_markup_element_namespace(b));
      const char * la = htmleq_str(gtext_markup_element_local_name(a));
      const char * lb = htmleq_str(gtext_markup_element_local_name(b));
      if (strcmp(na, nb) != 0) {
        htmleq_note(diff, path, "element namespace", na, strlen(na), nb, strlen(nb), a, b);
        return 0;
      }
      if (strcmp(la, lb) != 0) {
        htmleq_note(diff, path, "element name", la, strlen(la), lb, strlen(lb), a, b);
        return 0;
      }
      HtmlEq_Attr * aa = NULL;
      HtmlEq_Attr * ab = NULL;
      const long cnta = htmleq_attrs(a, &aa);
      if (cnta < 0) {
        return -1;
      }
      const long cntb = htmleq_attrs(b, &ab);
      if (cntb < 0) {
        htmleq_free_attrs(aa, cnta);
        return -1;
      }
      int same = cnta == cntb;
      long first = 0;
      if (same) {
        for (; first < cnta; first++) {
          if (htmleq_attr_order(&aa[first], &ab[first]) != 0) {
            same = 0;
            break;
          }
        }
      }
      else {
        first = 0;
        while (first < cnta && first < cntb && htmleq_attr_order(&aa[first], &ab[first]) == 0) {
          first++;
        }
      }
      if (!same) {
        htmleq_note(diff, path, "attributes", first < cnta ? aa[first].bytes : "(none)",
            first < cnta ? aa[first].length : 6, first < cntb ? ab[first].bytes : "(none)",
            first < cntb ? ab[first].length : 6, a, b);
      }
      htmleq_free_attrs(aa, cnta);
      htmleq_free_attrs(ab, cntb);
      if (!same) {
        return 0;
      }
      snprintf(here, sizeof(here), "%.200s/%.40s", path, la);
      break;
    }
    case GTEXT_MARKUP_KIND_COMMENT: {
      size_t lena = 0, lenb = 0;
      const char * da = gtext_markup_node_data(a, &lena);
      const char * db = gtext_markup_node_data(b, &lenb);
      if (lena != lenb || (lena && memcmp(da, db, lena) != 0)) {
        htmleq_note(diff, path, "comment", da ? da : "", lena, db ? db : "", lenb, gtext_markup_node_parent(a), gtext_markup_node_parent(b));
        return 0;
      }
      return 1;
    }
    case GTEXT_MARKUP_KIND_DOCTYPE: {
      const char * parts_a[3] = {htmleq_str(gtext_markup_doctype_name(a)),
          htmleq_str(gtext_markup_doctype_public_id(a)), htmleq_str(gtext_markup_doctype_system_id(a))};
      const char * parts_b[3] = {htmleq_str(gtext_markup_doctype_name(b)),
          htmleq_str(gtext_markup_doctype_public_id(b)), htmleq_str(gtext_markup_doctype_system_id(b))};
      for (int i = 0; i < 3; i++) {
        if (strcmp(parts_a[i], parts_b[i]) != 0) {
          htmleq_note(diff, path, i == 0 ? "doctype name" : i == 1 ? "doctype public id" : "doctype system id",
              parts_a[i], strlen(parts_a[i]), parts_b[i], strlen(parts_b[i]), gtext_markup_node_parent(a),
              gtext_markup_node_parent(b));
          return 0;
        }
      }
      return 1;
    }
    case GTEXT_MARKUP_KIND_DOCUMENT:
      snprintf(here, sizeof(here), "%.250s", path);
      break;
    default:
      htmleq_note(diff, path, "unexpected node", "", 0, "", 0, NULL, NULL);
      return 0;
  }
  HtmlEq_Item * ca = NULL;
  HtmlEq_Item * cb = NULL;
  const long na = htmleq_children(a, &ca);
  if (na < 0) {
    return -1;
  }
  const long nb = htmleq_children(b, &cb);
  if (nb < 0) {
    htmleq_free_children(ca, na);
    return -1;
  }
  int result = 1;
  if (na != nb) {
    char x[32], y[32];
    snprintf(x, sizeof(x), "%ld children", na);
    snprintf(y, sizeof(y), "%ld children", nb);
    htmleq_note(diff, here, "child count", x, strlen(x), y, strlen(y), a, b);
    result = 0;
  }
  for (long i = 0; result == 1 && i < na; i++) {
    char sub[256];
    snprintf(sub, sizeof(sub), "%.200s[%ld]", here, i);
    if ((ca[i].node == NULL) != (cb[i].node == NULL)) {
      const char * x = ca[i].node ? htmleq_kind_name(gtext_markup_node_kind(ca[i].node)) : "text";
      const char * y = cb[i].node ? htmleq_kind_name(gtext_markup_node_kind(cb[i].node)) : "text";
      htmleq_note(diff, sub, "child kind", x, strlen(x), y, strlen(y), a, b);
      result = 0;
    }
    else if (ca[i].node == NULL) {
      if (ca[i].length != cb[i].length || memcmp(ca[i].text, cb[i].text, ca[i].length) != 0) {
        htmleq_note(diff, sub, "text", ca[i].text, ca[i].length, cb[i].text, cb[i].length, a, b);
        result = 0;
      }
    }
    else {
      result = htmleq_cmp(ca[i].node, cb[i].node, sub, diff, depth + 1);
    }
  }
  htmleq_free_children(ca, na);
  htmleq_free_children(cb, nb);
  return result;
}

static inline int htmleq_compare(const GTEXT_Markup_Node * a, const GTEXT_Markup_Node * b, const char * path,
    HtmlEq_Diff * diff) {
  return htmleq_cmp(a, b, path, diff, 0);
}

/* The yes-or-no form. Running out of memory, or a tree deeper than HTMLEQ_MAX_DEPTH, is not "the trees
 * differ", and this form has nowhere to say so: it stops the program. */
static inline bool html_equivalent(const GTEXT_Markup_Node * a, const GTEXT_Markup_Node * b) {
  const int r = htmleq_compare(a, b, "", NULL);
  if (r < 0) {
    fprintf(stderr, "html_equivalent: %s\n", r == HTMLEQ_OOM ? "out of memory" : "tree too deep to compare");
    abort();
  }
  return r == 1;
}

#endif
