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
 * A tree in the html5lib-tests / web-platform-tests tree-construction dump
 * format (the format of the `#document` section of the .dat vectors), written
 * from a GTEXT_Markup_Document:
 *
 *   | <!DOCTYPE html>                       "public" "system" only if either is non-empty
 *   | <html>                                children two spaces deeper
 *   |   <head>
 *   |   <body>
 *   |     class="x"                         attributes, sorted by name (UTF-16 code units), before the children
 *   |     "text"                            text, with its line feeds as they are
 *   |     <!-- comment -->
 *   |     <?target data?>                   processing instruction
 *
 * Elements in the SVG and MathML namespaces are written `<svg name>` and `<math name>`; an
 * attribute in the XLink, XML or XMLNS namespace is written with a prefix word (`xlink href`).
 * A template's content is written as a `content` line under the element, with the content's children
 * below it, before the element's own children.
 *
 * One definition, used by the conformance runner, the unit tests (C++) and the fuzz harness. It
 * walks the tree with an explicit stack of positions and never recurses.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GTEXT_TOOLS_CONFORMANCE_HTML_TREE_DUMP_H
#define GHOTI_IO_GTEXT_TOOLS_CONFORMANCE_HTML_TREE_DUMP_H

#include <ghoti.io/text/markup.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char * data;
  size_t length;
  size_t capacity;
  int failed;
} HtdBuf;

static void htd_put(HtdBuf * b, const char * s, size_t n) {
  if (b->failed || n == 0) {
    return;
  }
  if (b->length + n + 1 > b->capacity) {
    size_t cap = b->capacity ? b->capacity : 256;
    while (cap < b->length + n + 1) {
      cap *= 2;
    }
    char * grown = (char *)realloc(b->data, cap);
    if (!grown) {
      b->failed = 1;
      return;
    }
    b->data = grown;
    b->capacity = cap;
  }
  memcpy(b->data + b->length, s, n);
  b->length += n;
  b->data[b->length] = '\0';
}

static void htd_puts(HtdBuf * b, const char * s) {
  htd_put(b, s, strlen(s));
}

static void htd_line_start(HtdBuf * b, size_t depth) {
  htd_put(b, "| ", 2);
  for (size_t i = 0; i < depth; i++) {
    htd_put(b, "  ", 2);
  }
}

/* UTF-16 code unit order of two UTF-8 strings (the dump sorts attribute names that way). */
static int htd_cmp16(const char * a, const char * b) {
  const unsigned char * p = (const unsigned char *)a;
  const unsigned char * q = (const unsigned char *)b;
  for (;;) {
    uint32_t x = 0, y = 0;
    unsigned lx = 1, ly = 1;
    if (*p >= 0xF0) { x = ((uint32_t)(*p & 7) << 18) | ((uint32_t)(p[1] & 63) << 12) | ((uint32_t)(p[2] & 63) << 6) | (p[3] & 63); lx = 4; }
    else if (*p >= 0xE0) { x = ((uint32_t)(*p & 15) << 12) | ((uint32_t)(p[1] & 63) << 6) | (p[2] & 63); lx = 3; }
    else if (*p >= 0xC0) { x = ((uint32_t)(*p & 31) << 6) | (p[1] & 63); lx = 2; }
    else { x = *p; }
    if (*q >= 0xF0) { y = ((uint32_t)(*q & 7) << 18) | ((uint32_t)(q[1] & 63) << 12) | ((uint32_t)(q[2] & 63) << 6) | (q[3] & 63); ly = 4; }
    else if (*q >= 0xE0) { y = ((uint32_t)(*q & 15) << 12) | ((uint32_t)(q[1] & 63) << 6) | (q[2] & 63); ly = 3; }
    else if (*q >= 0xC0) { y = ((uint32_t)(*q & 31) << 6) | (q[1] & 63); ly = 2; }
    else { y = *q; }
    if (x != y) {
      /* A supplementary-plane character is a surrogate pair: its first unit is in D800..DBFF. */
      const uint32_t ux = x >= 0x10000 ? 0xD800 + ((x - 0x10000) >> 10) : x;
      const uint32_t uy = y >= 0x10000 ? 0xD800 + ((y - 0x10000) >> 10) : y;
      return ux < uy ? -1 : ux > uy ? 1 : (x < y ? -1 : 1);
    }
    if (x == 0) {
      return 0;
    }
    p += lx;
    q += ly;
  }
}

static const char * htd_ns_word(const char * ns) {
  if (!ns) {
    return "";
  }
  if (strcmp(ns, "http://www.w3.org/2000/svg") == 0) {
    return "svg ";
  }
  if (strcmp(ns, "http://www.w3.org/1998/Math/MathML") == 0) {
    return "math ";
  }
  return "";
}

static const char * htd_attr_ns_word(const char * ns) {
  if (!ns) {
    return "";
  }
  if (strcmp(ns, "http://www.w3.org/1999/xlink") == 0) {
    return "xlink ";
  }
  if (strcmp(ns, "http://www.w3.org/XML/1998/namespace") == 0) {
    return "xml ";
  }
  if (strcmp(ns, "http://www.w3.org/2000/xmlns/") == 0) {
    return "xmlns ";
  }
  return "";
}

typedef struct {
  char * name; /* word + local name, owned */
  const char * value;
  size_t value_length;
} HtdAttr;

static int htd_attr_cmp(const void * a, const void * b) {
  return htd_cmp16(((const HtdAttr *)a)->name, ((const HtdAttr *)b)->name);
}

static void htd_element(HtdBuf * b, const GTEXT_Markup_Node * el, size_t depth) {
  htd_line_start(b, depth);
  htd_put(b, "<", 1);
  htd_puts(b, htd_ns_word(gtext_markup_element_namespace(el)));
  htd_puts(b, gtext_markup_element_local_name(el));
  htd_put(b, ">\n", 2);
  const size_t n = gtext_markup_element_attribute_count(el);
  if (n == 0) {
    return;
  }
  HtdAttr * attrs = (HtdAttr *)calloc(n, sizeof(HtdAttr));
  if (!attrs) {
    b->failed = 1;
    return;
  }
  size_t made = 0;
  for (size_t i = 0; i < n; i++) {
    GTEXT_Markup_Attribute a;
    if (gtext_markup_element_attribute_at(el, i, &a) != GTEXT_MARKUP_OK) {
      continue;
    }
    const char * word = htd_attr_ns_word(a.ns);
    const size_t wl = strlen(word);
    const size_t ll = strlen(a.local);
    char * name = (char *)malloc(wl + ll + 1);
    if (!name) {
      b->failed = 1;
      break;
    }
    memcpy(name, word, wl);
    memcpy(name + wl, a.local, ll + 1);
    attrs[made].name = name;
    attrs[made].value = a.value;
    attrs[made].value_length = a.value_length;
    made++;
  }
  qsort(attrs, made, sizeof(HtdAttr), htd_attr_cmp);
  for (size_t i = 0; i < made; i++) {
    htd_line_start(b, depth + 1);
    htd_puts(b, attrs[i].name);
    htd_put(b, "=\"", 2);
    htd_put(b, attrs[i].value, attrs[i].value_length);
    htd_put(b, "\"\n", 2);
    free(attrs[i].name);
  }
  free(attrs);
}

static void htd_leaf(HtdBuf * b, const GTEXT_Markup_Node * n, size_t depth) {
  switch (gtext_markup_node_kind(n)) {
    case GTEXT_MARKUP_KIND_DOCTYPE: {
      const char * pub = gtext_markup_doctype_public_id(n);
      const char * sys = gtext_markup_doctype_system_id(n);
      htd_line_start(b, depth);
      htd_puts(b, "<!DOCTYPE ");
      htd_puts(b, gtext_markup_doctype_name(n));
      if ((pub && *pub) || (sys && *sys)) {
        htd_puts(b, " \"");
        htd_puts(b, pub ? pub : "");
        htd_puts(b, "\" \"");
        htd_puts(b, sys ? sys : "");
        htd_puts(b, "\"");
      }
      htd_puts(b, ">\n");
      break;
    }
    case GTEXT_MARKUP_KIND_TEXT:
    case GTEXT_MARKUP_KIND_CDATA: {
      htd_line_start(b, depth);
      size_t len = 0;
      const char * data = gtext_markup_node_data(n, &len);
      htd_put(b, "\"", 1);
      htd_put(b, data, len);
      htd_put(b, "\"\n", 2);
      break;
    }
    case GTEXT_MARKUP_KIND_COMMENT:
      htd_line_start(b, depth);
      {
        size_t len = 0;
        const char * data = gtext_markup_node_data(n, &len);
        htd_puts(b, "<!-- ");
        htd_put(b, data, len);
        htd_puts(b, " -->\n");
      }
      break;
    case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION:
      htd_line_start(b, depth);
      htd_puts(b, "<?");
      htd_puts(b, gtext_markup_pi_target(n));
      htd_puts(b, " ");
      {
        size_t len = 0;
        const char * data = gtext_markup_node_data(n, &len);
        htd_put(b, data, len);
      }
      htd_puts(b, "?>\n");
      break;
    default:
      break;
  }
}

typedef struct {
  const GTEXT_Markup_Node * next; /* the next sibling to write at this level */
  size_t depth;
} HtdFrame;

/* The dump of the document's children, in a NUL-terminated block the caller frees with free().
   NULL if memory ran out. The last line ends in a line feed. The walk keeps its place in an
   array of frames, so it needs neither recursion nor parent links: a template's content (which
   has no parent as far as the accessors say) is written under a `content` line, before the
   element's own children. */
static char * htd_dump_children(const GTEXT_Markup_Node * root, size_t * length_out);

static char * htd_dump(const GTEXT_Markup_Document * doc, size_t * length_out) {
  return htd_dump_children(gtext_markup_document_root(doc), length_out);
}

/* The same for the children of any container: a document's root, or the document fragment the
   fragment parsing algorithm returns (whose dump has the same shape: the vectors' `#document` section). */
static char * htd_dump_children(const GTEXT_Markup_Node * root, size_t * length_out) {
  HtdBuf b;
  memset(&b, 0, sizeof(b));
  HtdFrame * frames = NULL;
  size_t nframes = 0;
  size_t cap = 0;
  int ok = 1;
#define HTD_PUSH(first, d) \
  do { \
    if ((first) != NULL) { \
      if (nframes == cap) { \
        size_t ncap = cap ? cap * 2 : 64; \
        HtdFrame * grown = (HtdFrame *)realloc(frames, ncap * sizeof(HtdFrame)); \
        if (!grown) { \
          ok = 0; \
          break; \
        } \
        frames = grown; \
        cap = ncap; \
      } \
      frames[nframes].next = (first); \
      frames[nframes].depth = (d); \
      nframes++; \
    } \
  } while (0)
  HTD_PUSH(gtext_markup_node_first_child(root), 0);
  while (ok && nframes > 0 && !b.failed) {
    HtdFrame * f = &frames[nframes - 1];
    const GTEXT_Markup_Node * n = f->next;
    if (!n) {
      nframes--;
      continue;
    }
    const size_t depth = f->depth;
    f->next = gtext_markup_node_next_sibling(n);
    if (gtext_markup_node_kind(n) == GTEXT_MARKUP_KIND_ELEMENT) {
      htd_element(&b, n, depth);
      HTD_PUSH(gtext_markup_node_first_child(n), depth + 1);
      const GTEXT_Markup_Node * content = gtext_markup_element_template_content(n);
      if (content) {
        htd_line_start(&b, depth + 1);
        htd_puts(&b, "content\n");
        HTD_PUSH(gtext_markup_node_first_child(content), depth + 2);
      }
    }
    else {
      htd_leaf(&b, n, depth);
    }
  }
#undef HTD_PUSH
  free(frames);
  if (!ok || b.failed) {
    free(b.data);
    return NULL;
  }
  if (!b.data) {
    b.data = (char *)malloc(1);
    if (b.data) {
      b.data[0] = '\0';
    }
  }
  if (length_out) {
    *length_out = b.length;
  }
  return b.data;
}

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GTEXT_TOOLS_CONFORMANCE_HTML_TREE_DUMP_H */
