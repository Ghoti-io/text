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
 * libFuzzer harness for the public HTML API (src/html/html_api.c, html_core.c over the tree builder).
 *
 * The tree builder has its own harness (fuzz_html_tree.cpp), which holds the builder to the properties
 * that make a parse one parse. This one holds the layer over it, through the API a program uses
 * (include/ghoti.io/text/html.h), to the properties of a public API:
 *
 * The input is five option bytes and then bytes for the parser. o0: bit 0 the scripting flag, bit 1 says
 * the bytes are a fragment and bits 2 to 7 choose its context element from a table (the same table as
 * fuzz_html_tree.cpp's). o1: the size of the pieces (low four bits) and a tiny open-element limit (high
 * four). o2: the seed of the random cut. o3: the failure sweep's mode (bit 0 cuts, bit 1 a byte at a
 * time, bit 2 the fragment/document alternative) and the single cut (bits 3 to 7). o4: bit 0 count-only,
 * bit 1 a byte order mark in front, bits 2 and 3 the error cap, bit 4 a UTF-8 transport label, bit 5 no
 * result struct in the failure sweep, bits 6 and 7 the world: 0 is the UTF-8 world described below, and
 * anything else (for a document) is the **encoding world**, in which three more bytes follow the five.
 *
 * **The encoding world** reads the bytes in any encoding of a table of twenty, announced as the options
 * choose (a certain or a tentative transport label, a default, or nothing), with `ignore_meta` or not, with
 * a byte order mark in front or not and with a `<meta>` in front of the bytes, inside the first 1024 or
 * after them (so that the parser reads it only as it parses, and restarts): e0 is the encoding (bits 0 to
 * 4), how it is announced (5 and 6) and ignore_meta (7); e1 is the encoding the `<meta>` names (low four
 * bits, 0 for none), the padding before it (4 and 5) and the mark (6 and 7); e2 is the form of the `<meta>`
 * (charset or http-equiv), its case, and a second, later one. The properties, besides every one of the
 * UTF-8 world's that does not name UTF-8 (the cuts, the limits, the failure sweep, the incremental
 * parser's states):
 *     the encoding is a name of the Encoding Standard, the document records it and the source the result
 *     gives; the confidence and the flags make sense (a restart is source DECLARATION, certain, and
 *     the declared encoding is not the one the bytes began as); a byte order mark has its length;
 *     the decision is the sniff function's (gtext_html_sniff_encoding()) when the parse did not restart;
 *     **the oracle**: the Encoding module decodes the input as the encoding the result names, with the
 *     decoder's source map, and the tree builder reads that UTF-8 itself; the tree, the mode and
 *     the errors are the same (the errors for ill-formed bytes are the decoder's replacements), and
 *     every offset is the oracle's taken back through the map;
 *     a restart is what a parse that began in the declared encoding gives;
 *     UTF-8 read through the decoder and the source map is UTF-8 read by the tokenizer.
 *
 *     the answer - status, tree (in the tree-construction dump), document mode, every error (code,
 *     name, line, column, offset), the dropped count, the encoding and its source, the input and node
 *     counts - is the same whole (gtext_html_parse), by the incremental parser whole, a byte at a time,
 *     in pieces of a size the options choose, in random pieces, and cut once; for a fragment the same
 *     through gtext_html_parse_fragment() and the incremental fragment parser;
 *     the answer is the tree builder's own, called directly, with a leading byte order mark skipped
 *     (the layer adds nothing and loses nothing);
 *     the result agrees with the call: its status is the return value, a failure has no document or
 *     fragment, no node count and no mode, a success has a document that passes the invariant checker
 *     and the tree is a valid HTML tree (fuzz_html_tree.cpp's checker), the result's mode and node count
 *     are the document's, a document records "UTF-8" with the source the result gives, every error's
 *     name is the name of its code (and not "unknown"), positions are 1-based, offsets are inside the
 *     input, and the kept errors are at most the cap;
 *     the error cap keeps the first errors, in order, and kept plus dropped is the same number whatever
 *     the cap; count-only keeps none and counts all;
 *     a byte order mark in front of a document changes nothing but the encoding's source and the
 *     offsets, which are three more, and is text in a fragment; a UTF-8 transport label is the source CALLER;
 *     a limit that is tiny (open elements 1 to 8, nodes, input) is its own status, with no document,
 *     the boundary is exact for the input (the size passes, one byte less is refused, however the input
 *     is cut), and when a run under a limit succeeds it is the roomy run's;
 *     the incremental parser is sticky: after a failure every call returns the failure; after a success
 *     a second finish and a feed are state errors and the document stays the first caller's; the
 *     document and the result are freed in either order;
 *     a fragment can be cloned into a document of the caller's, equal to the original;
 *     an allocation that fails - the first to the last of a call, through the entry point the options
 *     choose - is OOM with no document, or the answer the call would have given, and never a leak or a
 *     free of a block the allocator did not make.
 *
 * The seeds (tests/fuzz/corpus/html) are made by tools/html/make-api-fuzz-seeds.py from the tree
 * builder's seeds and a list of its own.
 *
 * Build with: make fuzz-html      Run: make fuzz-run-html
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ghoti.io/text/encoding.h>

#include "../../include/ghoti.io/text/markup/markup_html.h"
#include "../../src/html/html_tree_internal.h"
#include "../../src/markup/markup_internal.h"
#include "../../tools/conformance/html_tree_dump.h"
#include "../html_api_test_support.h"
#include "../markup_test_support.h"

extern "C" GTEXT_HTML_Status html_parser_force_transcode(GTEXT_HTML_Parser * parser);

namespace {

using namespace html_api_test;

[[noreturn]] void fail(const char * what, const std::string & detail, const std::string & input) {
  std::fprintf(stderr, "fuzz_html: %s\n%s\ninput (%zu bytes): ", what, detail.c_str(), input.size());
  for (unsigned char c : input) {
    if (c >= 0x20 && c < 0x7f && c != '\\') {
      std::fputc(c, stderr);
    }
    else {
      std::fprintf(stderr, "\\x%02x", c);
    }
  }
  std::fputc('\n', stderr);
  std::fflush(stderr);
  std::abort();
}

// The contexts the fuzzer parses fragments in (index = the first option byte >> 2).
struct FuzzCtx {
  GTEXT_HTML_Namespace ns;
  const char * name;
  const char * encoding; // an `encoding` attribute of the context element, or null
  bool form;
  GTEXT_Markup_Mode quirks;
};
const FuzzCtx kContexts[64] = {
    {GTEXT_HTML_NS_HTML, "div", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "td", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "th", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "tr", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "tbody", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "thead", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "tfoot", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "caption", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "colgroup", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "table", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "template", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "select", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "body", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "html", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "head", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "frameset", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "title", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "textarea", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "style", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "script", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "plaintext", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "noscript", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "xmp", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "iframe", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "form", nullptr, true, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "div", nullptr, true, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "div", nullptr, false, GTEXT_MARKUP_MODE_QUIRKS},
    {GTEXT_HTML_NS_HTML, "p", nullptr, false, GTEXT_MARKUP_MODE_LIMITED_QUIRKS},
    {GTEXT_HTML_NS_HTML, "span", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "option", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "svg", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "math", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_SVG, "svg", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_SVG, "g", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_SVG, "path", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_SVG, "foreignObject", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_SVG, "desc", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_SVG, "title", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_SVG, "td", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_SVG, "select", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_SVG, "template", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_SVG, "foreignobject", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_MATHML, "math", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_MATHML, "mi", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_MATHML, "mo", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_MATHML, "mn", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_MATHML, "ms", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_MATHML, "mtext", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_MATHML, "annotation-xml", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_MATHML, "annotation-xml", "text/html", false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_MATHML, "annotation-xml", "Application/XHTML+xml", false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_MATHML, "annotation-xml", "text/plain", false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_MATHML, "mrow", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_MATHML, "mglyph", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_MATHML, "table", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_MATHML, "td", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_MATHML, "select", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_SVG, "script", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_SVG, "text", nullptr, false, GTEXT_MARKUP_MODE_QUIRKS},
    {GTEXT_HTML_NS_MATHML, "mfrac", nullptr, true, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "td", nullptr, true, GTEXT_MARKUP_MODE_QUIRKS},
    {GTEXT_HTML_NS_HTML, "tr", nullptr, false, GTEXT_MARKUP_MODE_QUIRKS},
    {GTEXT_HTML_NS_HTML, "dd", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
    {GTEXT_HTML_NS_HTML, "nobr", nullptr, false, GTEXT_MARKUP_MODE_NO_QUIRKS},
};

// The properties of a tree, checked with the public accessors and an explicit stack.
std::string check_tree(const GTEXT_Markup_Document * doc, const GTEXT_Markup_Node * fragment) {
  const char * invariants = markup_check_invariants(doc);
  if (invariants) {
    return std::string("the invariant checker: ") + invariants;
  }
  const GTEXT_Markup_Mode mode = gtext_markup_document_mode(doc);
  if (mode != GTEXT_MARKUP_MODE_NO_QUIRKS && mode != GTEXT_MARKUP_MODE_LIMITED_QUIRKS
      && mode != GTEXT_MARKUP_MODE_QUIRKS) {
    return "the document mode is not one of the three";
  }
  const GTEXT_Markup_Node * root = gtext_markup_document_root(doc);
  const GTEXT_Markup_Node * html = gtext_markup_document_element(doc);
  if (!html || !gtext_markup_element_is(html, GTEXT_MARKUP_NS_HTML, "html")) {
    return "the document has no html element";
  }
  size_t elements = 0;
  if (fragment) {
    // The root element's children were moved into the fragment.
    if (gtext_markup_node_first_child(html)) {
      return "a fragment's root html element still has children";
    }
    if (gtext_markup_node_kind(fragment) != GTEXT_MARKUP_KIND_FRAGMENT || gtext_markup_node_parent(fragment)) {
      return "the fragment is not a detached document fragment";
    }
    elements = 2; // no head and body to ask for
  }
  for (const GTEXT_Markup_Node * c = gtext_markup_node_first_child(html); c; c = gtext_markup_node_next_sibling(c)) {
    if (gtext_markup_node_kind(c) != GTEXT_MARKUP_KIND_ELEMENT) {
      continue;
    }
    if (elements == 0 && !gtext_markup_element_is(c, GTEXT_MARKUP_NS_HTML, "head")) {
      return "the first element in html is not head";
    }
    if (elements == 1 && !gtext_markup_element_is(c, GTEXT_MARKUP_NS_HTML, "body")
        && !gtext_markup_element_is(c, GTEXT_MARKUP_NS_HTML, "frameset")) {
      return "the second element in html is not body or frameset";
    }
    if (elements >= 2 && !gtext_markup_element_is(c, GTEXT_MARKUP_NS_HTML, "noframes")) {
      return "html has an element after the second that is not a noframes";
    }
    elements++;
  }
  if (elements < 2) {
    return "html does not have a head and a body (or frameset)";
  }
  // Every node, template contents included, with an explicit stack: the public walk does not enter a
  // template's content.
  std::vector<const GTEXT_Markup_Node *> todo;
  todo.push_back(root);
  if (fragment) {
    todo.push_back(fragment);
  }
  while (!todo.empty()) {
    const GTEXT_Markup_Node * n = todo.back();
    todo.pop_back();
    for (const GTEXT_Markup_Node * c = gtext_markup_node_first_child(n); c; c = gtext_markup_node_next_sibling(c)) {
      todo.push_back(c);
    }
    switch (gtext_markup_node_kind(n)) {
      case GTEXT_MARKUP_KIND_ELEMENT: {
        const char * ns = gtext_markup_element_namespace(n);
        if (!ns) {
          return "an element in no namespace";
        }
        const bool is_html = std::strcmp(ns, GTEXT_MARKUP_NS_HTML) == 0;
        const bool is_svg = std::strcmp(ns, GTEXT_MARKUP_NS_SVG) == 0;
        const bool is_math = std::strcmp(ns, GTEXT_MARKUP_NS_MATHML) == 0;
        if (!is_html && !is_svg && !is_math) {
          return "an element outside the HTML, SVG and MathML namespaces";
        }
        const char * name = gtext_markup_element_local_name(n);
        if (is_html) {
          for (const char * p = name; *p; p++) {
            if (*p >= 'A' && *p <= 'Z') {
              return "an HTML element name with an upper case letter";
            }
          }
        }
        else if (!gtext_markup_html_foreign_name_survives(ns, false, name)) {
          return "a foreign element name that an HTML parser could not have made (not in the standard's table)";
        }
        for (size_t i = 0; i < gtext_markup_element_attribute_count(n); i++) {
          GTEXT_Markup_Attribute a;
          if (gtext_markup_element_attribute_at(n, i, &a) != GTEXT_MARKUP_OK) {
            return "an attribute that cannot be read";
          }
          if (a.ns) {
            if (is_html) {
              return "an attribute in a namespace on an HTML element";
            }
            if (std::strcmp(a.ns, GTEXT_MARKUP_NS_XLINK) != 0 && std::strcmp(a.ns, GTEXT_MARKUP_NS_XML) != 0
                && std::strcmp(a.ns, GTEXT_MARKUP_NS_XMLNS) != 0) {
              return "an attribute in a namespace the adjustment table does not make";
            }
          }
          else if (!is_html && !gtext_markup_html_foreign_name_survives(ns, true, a.local)) {
            return "a foreign attribute name that an HTML parser could not have made";
          }
        }
        const GTEXT_Markup_Node * content = gtext_markup_element_template_content(n);
        if (is_html && std::strcmp(name, "template") == 0) {
          if (!content) {
            return "a template without a content fragment";
          }
          if (gtext_markup_node_first_child(n)) {
            return "a template element with children of its own";
          }
          if (gtext_markup_node_kind(content) != GTEXT_MARKUP_KIND_FRAGMENT) {
            return "a template content that is not a document fragment";
          }
          todo.push_back(content);
        }
        else if (content) {
          return "a template content on an element that is not a template";
        }
        const GTEXT_Markup_Node * parent = gtext_markup_node_parent(n);
        const GTEXT_Markup_Kind pk = parent ? gtext_markup_node_kind(parent) : GTEXT_MARKUP_KIND_NONE;
        const bool in_content = pk == GTEXT_MARKUP_KIND_FRAGMENT;
        auto parent_is = [&](std::initializer_list<const char *> names) {
          if (in_content) {
            return true;
          }
          for (const char * want : names) {
            if (parent && pk == GTEXT_MARKUP_KIND_ELEMENT && gtext_markup_element_is(parent, GTEXT_MARKUP_NS_HTML, want)) {
              return true;
            }
          }
          return false;
        };
        if (!is_html) {
          break; // the shapes below are HTML's
        }
        if (std::strcmp(name, "tr") == 0 && !parent_is({"tbody", "thead", "tfoot"})) {
          return "a tr that is not in a table section";
        }
        if ((std::strcmp(name, "td") == 0 || std::strcmp(name, "th") == 0) && !parent_is({"tr"})) {
          return "a td or th that is not in a row";
        }
        if ((std::strcmp(name, "caption") == 0 || std::strcmp(name, "colgroup") == 0 || std::strcmp(name, "tbody") == 0
                || std::strcmp(name, "thead") == 0 || std::strcmp(name, "tfoot") == 0)
            && !parent_is({"table"})) {
          return "a caption, colgroup or table section that is not in a table";
        }
        if (std::strcmp(name, "col") == 0) {
          if (!parent_is({"colgroup"})) {
            return "a col that is not in a colgroup";
          }
          if (gtext_markup_node_first_child(n)) {
            return "a col with children";
          }
        }
        break;
      }
      case GTEXT_MARKUP_KIND_CDATA:
        return "a CDATA section";
      case GTEXT_MARKUP_KIND_TEXT: {
        size_t len = 0;
        const char * data = gtext_markup_node_data(n, &len);
        if (len == 0) {
          return "an empty text node";
        }
        if (std::memchr(data, 0, len)) {
          return "a text node with a NUL (the tree builder drops it or makes a U+FFFD of it)";
        }
        const GTEXT_Markup_Node * next = gtext_markup_node_next_sibling(n);
        if (next && gtext_markup_node_kind(next) == GTEXT_MARKUP_KIND_TEXT) {
          return "two adjacent text nodes";
        }
        const GTEXT_Markup_Node * parent = gtext_markup_node_parent(n);
        if (gtext_markup_node_kind(parent) == GTEXT_MARKUP_KIND_DOCUMENT) {
          return "text directly in the document";
        }
        if (gtext_markup_node_kind(parent) == GTEXT_MARKUP_KIND_ELEMENT
            && gtext_markup_element_is(parent, GTEXT_MARKUP_NS_HTML, gtext_markup_element_local_name(parent))) {
          const char * pn = gtext_markup_element_local_name(parent);
          if (std::strcmp(pn, "table") == 0 || std::strcmp(pn, "tbody") == 0 || std::strcmp(pn, "thead") == 0
              || std::strcmp(pn, "tfoot") == 0 || std::strcmp(pn, "tr") == 0) {
            for (size_t i = 0; i < len; i++) {
              const char c = data[i];
              if (c != ' ' && c != '\t' && c != '\n' && c != '\f' && c != '\r') {
                return "text that is not white space directly in a table part";
              }
            }
          }
        }
        break;
      }
      default:
        break;
    }
  }
  return "";
}


// The encodings of the encoding world: a label for each, in the order e0 and e1 choose them (modulo the count).
struct EncChoice {
  const char * label;
};
const EncChoice kEncodings[20] = {{"utf-8"}, {"utf-16le"}, {"utf-16be"}, {"windows-1252"}, {"iso-8859-2"}, {"koi8-r"}, {"windows-1251"},
    {"shift_jis"}, {"euc-jp"}, {"iso-2022-jp"}, {"gbk"}, {"gb18030"}, {"big5"}, {"euc-kr"}, {"x-user-defined"}, {"hz-gb-2312"},
    {"iso-8859-8-i"}, {"windows-874"}, {"macintosh"}, {"ibm866"}};

// The encoding (the library's enum) that the standard's name `name` is, or NONE.
GTEXT_Encoding encoding_named(const std::string & name) {
  for (int e = (int)GTEXT_ENCODING_NONE + 1; e < (int)GTEXT_ENCODING_COUNT; e++) {
    if (name == gtext_encoding_name((GTEXT_Encoding)e)) {
      return (GTEXT_Encoding)e;
    }
  }
  return GTEXT_ENCODING_NONE;
}

bool is_failure_status(GTEXT_HTML_Status s) {
  return s == GTEXT_HTML_E_OOM || (s >= GTEXT_HTML_E_LIMIT_INPUT && s <= GTEXT_HTML_E_LIMIT_ATTRIBUTES);
}

GTEXT_HTML_Fragment_Context make_context(const FuzzCtx & c, GTEXT_HTML_Context_Attribute * attr) {
  GTEXT_HTML_Fragment_Context fc = ctx(c.ns, c.name);
  fc.form_ancestor = c.form;
  fc.quirks = c.quirks;
  if (c.encoding) {
    attr->name = "encoding";
    attr->value = c.encoding;
    fc.attributes = attr;
    fc.attribute_count = 1;
  }
  return fc;
}

// What the tree builder says for the same bytes, called directly: the reference the layer must not
// differ from. A document's leading UTF-8 byte order mark is skipped; a fragment's is text.
struct Reference {
  bool ok = false;
  std::string dump;
  GTEXT_Markup_Mode mode = GTEXT_MARKUP_MODE_NO_QUIRKS;
  std::vector<Err> errors;
  size_t dropped = 0;
};

Reference reference(const std::string & input, const FuzzCtx * c, bool scripting, size_t max_open, bool skip_bom = true) {
  Reference r;
  GTEXT_HTML_TreeOptions o = {};
  o.scripting = scripting;
  o.max_open_elements = max_open;
  o.max_errors = 1u << 20;
  GTEXT_HTML_Tree * tree = nullptr;
  if (c) {
    GTEXT_HTML_FragmentContext fc;
    std::memset(&fc, 0, sizeof(fc));
    fc.ns = c->ns;
    fc.local_name = c->name;
    fc.local_name_length = std::strlen(c->name);
    GTEXT_HTML_Attribute enc;
    std::memset(&enc, 0, sizeof(enc));
    if (c->encoding) {
      enc.name = "encoding";
      enc.name_length = 8;
      enc.value = c->encoding;
      enc.value_length = std::strlen(c->encoding);
      fc.attributes = &enc;
      fc.attribute_count = 1;
    }
    fc.form_ancestor = c->form;
    fc.quirks = c->quirks;
    fc.scripting = scripting;
    GTEXT_HTML_TreeStatus st;
    tree = gtext_html_tree_new_fragment(&o, &fc, &st);
  }
  else {
    tree = gtext_html_tree_new(&o);
  }
  if (!tree) {
    return r;
  }
  size_t skip = 0;
  if (skip_bom && !c && input.size() >= 3 && input.compare(0, 3, "\xEF\xBB\xBF") == 0) {
    skip = 3;
    if (gtext_html_tree_skip_input(tree, 3) != GTEXT_HTML_TREE_OK) {
      fail("the builder refused to skip the byte order mark", "", input);
    }
  }
  GTEXT_HTML_TreeStatus s = gtext_html_tree_feed(tree, input.data() + skip, input.size() - skip);
  if (s == GTEXT_HTML_TREE_OK) {
    s = gtext_html_tree_finish(tree);
  }
  if (s == GTEXT_HTML_TREE_OK) {
    GTEXT_Markup_Document * doc = nullptr;
    GTEXT_Markup_Node * frag = nullptr;
    if (c) {
      frag = gtext_html_tree_take_fragment(tree, &doc);
    }
    else {
      doc = gtext_html_tree_take_document(tree);
    }
    if (doc) {
      r.ok = true;
      r.mode = gtext_markup_document_mode(doc);
      r.dump = frag ? dump_fragment(frag) : dump_doc(doc);
      gtext_markup_document_free(doc);
    }
    for (size_t i = 0; i < gtext_html_tree_error_count(tree); i++) {
      const GTEXT_HTML_TreeError * e = gtext_html_tree_error_at(tree, i);
      r.errors.push_back({e->code, "", e->line, e->column, e->offset});
    }
    r.dropped = gtext_html_tree_errors_dropped(tree);
  }
  gtext_html_tree_free(tree);
  return r;
}

struct Config {
  const FuzzCtx * ctx = nullptr;
  bool scripting = false;
  size_t max_open = 0;
  size_t max_nodes = 0;
  size_t max_input = 0;
  size_t max_errors = 1u << 20;
  bool count_only = false;
  const char * transport = nullptr;
  bool tentative = false;           // the transport label is only the caller's guess
  const char * fallback = nullptr;  // default_encoding
  bool ignore_meta = false;
  bool enc_world = false;           // the encoding is not UTF-8 by construction (see the file's comment)
  FaultAllocator * fa = nullptr;
  size_t arm = 0;
  bool want_result = true;
};

Outcome exec(const std::string & input, const Config & cfg, const Call & call) {
  GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
  o.scripting = cfg.scripting;
  o.max_open_elements = cfg.max_open;
  o.max_nodes = cfg.max_nodes;
  o.max_input_bytes = cfg.max_input;
  o.max_errors = cfg.max_errors;
  o.count_errors_only = cfg.count_only;
  o.transport_encoding = cfg.transport;
  o.transport_encoding_certainty = cfg.tentative ? GTEXT_HTML_TENTATIVE : GTEXT_HTML_CERTAIN;
  o.default_encoding = cfg.fallback;
  o.ignore_meta = cfg.ignore_meta;
  if (cfg.fa) {
    o.allocator = &cfg.fa->alloc;
    cfg.fa->arm(cfg.arm);
  }
  GTEXT_HTML_Context_Attribute attr;
  GTEXT_HTML_Fragment_Context fc;
  if (cfg.ctx) {
    fc = make_context(*cfg.ctx, &attr);
  }
  Call c = call;
  c.fragment = cfg.ctx != nullptr;
  Outcome out = run_call(c, input, &o, cfg.ctx ? &fc : nullptr, cfg.want_result);
  if (cfg.fa) {
    cfg.fa->disarm();
  }
  return out;
}

// The properties of one outcome, whatever the input.
void check_outcome(const Outcome & o, const Config & cfg, const std::string & input, size_t input_bytes) {
  if (o.status != GTEXT_HTML_OK && !is_failure_status(o.status)) {
    fail("an unexpected status", std::to_string((int)o.status), input);
  }
  if (cfg.want_result && o.result_status != o.status) {
    fail("the result's status is not the call's", std::to_string((int)o.result_status), input);
  }
  if (cfg.want_result && o.message.empty()) {
    fail("a result without a message", "", input);
  }
  if (o.status != GTEXT_HTML_OK) {
    if (o.doc || o.fragment) {
      fail("a failed call has a document", "", input);
    }
    if (cfg.want_result && (o.node_count != 0 || o.mode != GTEXT_MARKUP_MODE_NO_QUIRKS)) {
      fail("a failed call reports a tree", "", input);
    }
    return;
  }
  if (!o.doc) {
    fail("a successful call has no document", "", input);
  }
  const std::string problem = check_tree(o.doc, o.fragment);
  if (!problem.empty()) {
    fail("the tree is not a valid HTML tree", problem, input);
  }
  if (!cfg.want_result) {
    return;
  }
  if (o.mode != gtext_markup_document_mode(o.doc)) {
    fail("the result's mode is not the document's", "", input);
  }
  if (o.node_count != gtext_markup_document_node_count(o.doc)) {
    fail("the result's node count is not the document's", "", input);
  }
  if (!cfg.enc_world && o.encoding != "UTF-8") {
    fail("the encoding is not UTF-8", o.encoding, input);
  }
  if (cfg.enc_world && !o.fragment && encoding_named(o.encoding) == GTEXT_ENCODING_NONE) {
    fail("the encoding is not a name of the standard", o.encoding, input);
  }
  if (!o.fragment) {
    GTEXT_Markup_Encoding_Source source = GTEXT_MARKUP_ENCODING_UNKNOWN;
    const char * label = gtext_markup_document_encoding(o.doc, &source);
    if (!label || o.encoding != label || source != o.source) {
      fail("the document does not record the encoding the result gives", "", input);
    }
    if (o.confidence != GTEXT_HTML_CERTAIN && o.confidence != GTEXT_HTML_TENTATIVE) {
      fail("a document's confidence is neither certain nor tentative", std::to_string((int)o.confidence), input);
    }
    if (o.reparsed && (o.source != GTEXT_MARKUP_ENCODING_DECLARATION || o.confidence != GTEXT_HTML_CERTAIN || o.bom_length != 0)) {
      fail("a restart that is not a certain declaration", "", input);
    }
    if (o.source == GTEXT_MARKUP_ENCODING_BOM && (o.bom_length < 2 || o.bom_length > 3 || o.confidence != GTEXT_HTML_CERTAIN)) {
      fail("a byte order mark without its length, or not certain", "", input);
    }
    if (o.source != GTEXT_MARKUP_ENCODING_BOM && o.bom_length != 0) {
      fail("a byte order mark length without a byte order mark", "", input);
    }
    if (o.source == GTEXT_MARKUP_ENCODING_CALLER && o.confidence != GTEXT_HTML_CERTAIN) {
      fail("a transport label that is not certain is the source CALLER", "", input);
    }
  }
  else if (o.confidence != GTEXT_HTML_IRRELEVANT) {
    fail("a fragment's confidence is not irrelevant", "", input);
  }
  if (o.source != GTEXT_MARKUP_ENCODING_BOM && o.source != GTEXT_MARKUP_ENCODING_CALLER
      && o.source != GTEXT_MARKUP_ENCODING_DEFAULT && o.source != GTEXT_MARKUP_ENCODING_DECLARATION
      && o.source != GTEXT_MARKUP_ENCODING_CALLER_DEFAULT) {
    fail("an encoding source that cannot be", std::to_string((int)o.source), input);
  }
  if (o.input_bytes != input_bytes) {
    fail("the input count is not the input", std::to_string(o.input_bytes), input);
  }
  if (cfg.count_only && !o.errors.empty()) {
    fail("count-only kept an error", "", input);
  }
  if (o.errors.size() > cfg.max_errors) {
    fail("more errors kept than the cap", "", input);
  }
  for (const Err & e : o.errors) {
    const bool valid = (e.code >= 0 && e.code <= 52) || (e.code >= 64 && e.code <= 79);
    if (!valid || e.name != gtext_html_error_name((GTEXT_HTML_Error_Code)e.code) || e.name == "unknown") {
      fail("an error code and its name disagree", e.name, input);
    }
    if (e.line < 1 || e.column < 1 || e.offset > input_bytes) {
      fail("an error position out of range", e.name, input);
    }
  }
}

// "" if the two outcomes are the same answer, else the first thing that differs.
std::string differ(const Outcome & a, const Outcome & b) {
  if (a.status != b.status) {
    return "status " + std::to_string((int)a.status) + " vs " + std::to_string((int)b.status);
  }
  if (a.result_status != b.result_status) {
    return "result status";
  }
  if (a.tree != b.tree) {
    return "tree";
  }
  if (a.errors.size() != b.errors.size() || a.dropped != b.dropped) {
    return "error count " + std::to_string(a.errors.size()) + "+" + std::to_string(a.dropped) + " vs "
        + std::to_string(b.errors.size()) + "+" + std::to_string(b.dropped);
  }
  for (size_t i = 0; i < a.errors.size(); i++) {
    if (!(a.errors[i] == b.errors[i])) {
      return "error " + std::to_string(i) + " (" + a.errors[i].name + ")";
    }
  }
  if (a.mode != b.mode) {
    return "mode";
  }
  if (a.encoding != b.encoding || a.source != b.source || a.confidence != b.confidence || a.reparsed != b.reparsed
      || a.bom_length != b.bom_length || a.transport_ignored != b.transport_ignored || a.default_ignored != b.default_ignored) {
    return "encoding";
  }
  if (a.status == GTEXT_HTML_OK && a.input_bytes != b.input_bytes) { // (a failed parse counts what it got to)
    return "input bytes " + std::to_string(a.input_bytes) + " vs " + std::to_string(b.input_bytes);
  }
  if (a.node_count != b.node_count) {
    return "node count";
  }
  return "";
}

bool same(const Outcome & a, const Outcome & b) {
  return differ(a, b).empty();
}

struct MapSink {
  std::string utf8;
  std::vector<size_t> offsets;
  std::vector<unsigned char> replaced;
  std::vector<unsigned char> widths;
};

bool map_write(void * user, const char * bytes, size_t len, const GTEXT_Encoding_Source_Map * map) {
  auto * m = static_cast<MapSink *>(user);
  m->utf8.append(bytes, len);
  m->offsets.insert(m->offsets.end(), map->offsets, map->offsets + len);
  m->replaced.insert(m->replaced.end(), map->replaced, map->replaced + len);
  m->widths.insert(m->widths.end(), map->widths, map->widths + len);
  return true;
}

// The Encoding module's decode of the input as `enc`, byte order mark sniffed as the standard's "decode" does, with the decoder's map.
bool decode_with_map(GTEXT_Encoding enc, const std::string & input, MapSink * out) {
  GTEXT_Decoder_Options o = gtext_decoder_options_default();
  o.track_offsets = true;
  o.bom_sniffing = true;
  o.max_output_bytes = SIZE_MAX;
  GTEXT_Decoder * d = nullptr;
  if (gtext_decoder_new(enc, &o, &d) != GTEXT_ENCODING_OK) {
    return false;
  }
  GTEXT_Encoding_Mapped_Sink sink = {map_write, out};
  const bool ok = gtext_decoder_decode_mapped(d, input.data(), input.size(), true, &sink, nullptr) == GTEXT_ENCODING_OK;
  gtext_decoder_free(d);
  return ok;
}

// The oracle: what the tree builder says for the Encoding module's decode of the input, with every offset taken back through the map.
void check_against_the_oracle(const std::string & input, const Outcome & whole, const Config & cfg) {
  const GTEXT_Encoding enc = encoding_named(whole.encoding);
  if (enc == GTEXT_ENCODING_NONE) {
    fail("the encoding the result names is not an encoding", whole.encoding, input);
  }
  MapSink m;
  if (!decode_with_map(enc, input, &m)) {
    fail("the oracle's decoder failed", whole.encoding, input);
  }
  // (no byte order mark is skipped here: the decoder took it, and a U+FEFF left in the text is a character)
  Reference ref = reference(m.utf8, nullptr, cfg.scripting, cfg.max_open, false);
  if (!ref.ok || ref.dump != whole.tree || ref.mode != whole.mode) {
    fail("the parse is not the tree builder's on the decoder's output", whole.encoding, input);
  }
  size_t replaced = 0;
  for (size_t i = 0; i < m.utf8.size(); i++) {
    const unsigned char b = (unsigned char)m.utf8[i];
    if ((b & 0xC0) != 0x80 && m.replaced[i]) {
      replaced++;
    }
  }
  std::vector<Err> mine;
  size_t invalid = 0;
  for (const Err & e : whole.errors) {
    if (e.name == "invalid-utf-8-in-input-stream") {
      invalid++;
    }
    else {
      mine.push_back(e);
    }
  }
  if (invalid != replaced) {
    fail("the errors for ill-formed bytes are not the decoder's replacements", std::to_string(invalid) + " vs " + std::to_string(replaced), input);
  }
  if (mine.size() != ref.errors.size() || whole.dropped != 0) {
    fail("the errors are not the tree builder's on the decoder's output", std::to_string(mine.size()) + " vs " + std::to_string(ref.errors.size()), input);
  }
  for (size_t i = 0; i < mine.size(); i++) {
    // An error at the place after the character just read is at the end of that character in the original; any other is at the start of
    // the character its position names (a shift sequence can lie between the two).
    const bool after = ref.errors[i].code == GTEXT_HTML_ERR_NULL_CHARACTER_REFERENCE
        || ref.errors[i].code == GTEXT_HTML_ERR_CHARACTER_REFERENCE_OUTSIDE_UNICODE_RANGE
        || ref.errors[i].code == GTEXT_HTML_ERR_SURROGATE_CHARACTER_REFERENCE
        || ref.errors[i].code == GTEXT_HTML_ERR_NONCHARACTER_CHARACTER_REFERENCE
        || ref.errors[i].code == GTEXT_HTML_ERR_CONTROL_CHARACTER_REFERENCE
        || ref.errors[i].code == GTEXT_HTML_ERR_INCORRECTLY_OPENED_COMMENT;
    const size_t at = ref.errors[i].offset;
    const size_t want_after = at == 0 ? 0 : (at <= m.offsets.size() ? m.offsets[at - 1] + m.widths[at - 1] : input.size());
    const size_t want_start = at < m.offsets.size() ? m.offsets[at] : input.size();
    // (a missing semicolon is at the place after the name of a named reference and at the character that ended a numeric one: either)
    const bool either = ref.errors[i].code == GTEXT_HTML_ERR_MISSING_SEMICOLON_AFTER_CHARACTER_REFERENCE;
    const size_t want = after ? want_after : want_start;
    if (mine[i].code != ref.errors[i].code || mine[i].line != ref.errors[i].line || mine[i].column != ref.errors[i].column
        || (either ? (mine[i].offset != want_after && mine[i].offset != want_start) : mine[i].offset != want)) {
      fail("an error is not the oracle's (code, line, column, or the offset taken back through the map)", mine[i].name, input);
    }
  }
}

// The decision is the sniff function's when the parse did not restart, and is not the sniffed encoding when it did.
void check_the_decision(const std::string & input, const Outcome & whole, const Config & cfg) {
  GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
  o.transport_encoding = cfg.transport;
  o.transport_encoding_certainty = cfg.tentative ? GTEXT_HTML_TENTATIVE : GTEXT_HTML_CERTAIN;
  o.default_encoding = cfg.fallback;
  o.ignore_meta = cfg.ignore_meta;
  GTEXT_HTML_Sniff_Result r;
  if (gtext_html_sniff_encoding(input.data(), input.size(), &o, &r) != GTEXT_HTML_OK) {
    fail("the sniff function failed", "", input);
  }
  const std::string sniffed = r.encoding_name ? r.encoding_name : "";
  if (!whole.reparsed) {
    if (whole.encoding != sniffed || whole.source != r.source || whole.bom_length != r.bom_length
        || whole.transport_ignored != r.transport_encoding_ignored || whole.default_ignored != r.default_encoding_ignored) {
      fail("the parser's decision is not the sniff function's", whole.encoding + " vs " + sniffed, input);
    }
    if (r.confidence == GTEXT_HTML_CERTAIN && whole.confidence != GTEXT_HTML_CERTAIN) {
      fail("a certain decision became tentative", "", input);
    }
  }
  else if (whole.encoding == sniffed || r.confidence == GTEXT_HTML_CERTAIN) {
    fail("a restart to the encoding it began as, or from a certain one", whole.encoding, input);
  }
}

// A restart is what a parse that began in the declared encoding gives.
void check_the_restart(const std::string & input, const Outcome & whole, const Config & cfg) {
  Config direct = cfg;
  direct.transport = nullptr;
  std::string label = whole.encoding;
  direct.transport = label.c_str();
  direct.tentative = false;
  direct.fallback = nullptr;
  Outcome o;
  {
    // (the name of an encoding is not always a label of it: `replacement` is the one that is not)
    if (whole.encoding == "replacement") {
      return;
    }
    o = exec(input, direct, {"parse", false, false, {}});
  }
  if (!o.ok() || o.tree != whole.tree || o.errors != whole.errors || o.reparsed) {
    fail("a restart is not what a parse that began in the declared encoding gives", whole.encoding, input);
  }
}

// UTF-8 through the decoder and the source map is UTF-8 through the tokenizer.
void check_the_transcoded_route(const std::string & input, const Outcome & whole, const Config & cfg) {
  GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
  o.scripting = cfg.scripting;
  o.max_open_elements = cfg.max_open;
  o.max_errors = cfg.max_errors;
  o.transport_encoding = cfg.transport;
  o.transport_encoding_certainty = cfg.tentative ? GTEXT_HTML_TENTATIVE : GTEXT_HTML_CERTAIN;
  o.default_encoding = cfg.fallback;
  o.ignore_meta = cfg.ignore_meta;
  for (int how = 0; how < 3; how++) {
    GTEXT_HTML_Parser * p = nullptr;
    if (gtext_html_parser_new(&o, &p) != GTEXT_HTML_OK) {
      fail("a parser could not be made", "", input);
    }
    html_parser_force_transcode(p);
    GTEXT_HTML_Status st = GTEXT_HTML_OK;
    const size_t step = how == 0 ? input.size() : how == 1 ? 1 : 7;
    for (size_t at = 0; at < input.size() && st == GTEXT_HTML_OK; at += step) {
      st = gtext_html_parser_feed(p, input.data() + at, std::min(step, input.size() - at));
    }
    GTEXT_Markup_Document * doc = nullptr;
    GTEXT_HTML_Result r;
    const GTEXT_HTML_Status fin = gtext_html_parser_finish(p, &doc, &r);
    if (st != GTEXT_HTML_OK || fin != GTEXT_HTML_OK || !doc) {
      fail("the decoder's route failed where the other did not", std::to_string((int)fin), input);
    }
    Outcome got;
    got.doc = doc;
    got.tree = dump_doc(doc);
    harvest(got, r);
    gtext_html_parser_free(p);
    if (got.tree != whole.tree || got.errors != whole.errors || got.encoding != whole.encoding || got.reparsed != whole.reparsed
        || got.source != whole.source) {
      fail("UTF-8 through the decoder is not UTF-8 through the tokenizer", "feed " + std::to_string(how), input);
    }
  }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 5 || size > 4096) {
    return 0;
  }
  const uint8_t o0 = data[0], o1 = data[1], o2 = data[2], o3 = data[3], o4 = data[4];
  const bool fragment = (o0 & 2) != 0;
  // The encoding world: for a document, when o4's top bits say so; three more option bytes follow.
  const bool enc_world = !fragment && (o4 >> 6) != 0 && size >= 8;
  const uint8_t e0 = enc_world ? data[5] : 0, e1 = enc_world ? data[6] : 0, e2 = enc_world ? data[7] : 0;
  std::string body((const char *)data + (enc_world ? 8 : 5), size - (enc_world ? 8 : 5));
  const FuzzCtx * context = fragment ? &kContexts[(o0 >> 2) & 63] : nullptr;
  const std::string bom = "\xEF\xBB\xBF";
  if (!enc_world && !fragment && body.size() >= 2
      && (((unsigned char)body[0] == 0xFF && (unsigned char)body[1] == 0xFE) || ((unsigned char)body[0] == 0xFE && (unsigned char)body[1] == 0xFF))) {
    body[0] = 'x'; // (a UTF-16 byte order mark in front would make the UTF-8 world something else)
  }
  // A byte order mark in front of a document, when o4 says so (the UTF-8 world).
  const bool add_bom = !fragment && !enc_world && (o4 & 2) != 0;
  std::string input = add_bom ? bom + body : body;
  Config roomy;
  roomy.ctx = context;
  roomy.scripting = (o0 & 1) != 0;
  roomy.max_open = 64; // fixed, as the builder's fuzzer does
  roomy.transport = (o4 & 16) ? (const char *)"UTF-8" : nullptr;
  roomy.enc_world = enc_world;
  if (!fragment && !enc_world) {
    roomy.ignore_meta = true; // (a `<meta>` among the bytes would make the parse another encoding: that is the other world's)
  }
  if (enc_world) {
    // The encoding the bytes are in, how it is announced, and what is put in front of them.
    const char * label = kEncodings[(e0 & 31) % 20].label;
    roomy.transport = nullptr;
    switch ((e0 >> 5) & 3) {
      case 0: roomy.transport = label; break;
      case 1: roomy.transport = label; roomy.tentative = true; break;
      case 2: roomy.fallback = label; break;
      default: break;
    }
    roomy.ignore_meta = (e0 & 128) != 0;
    std::string front;
    switch (e1 >> 6) {
      case 1: front += bom; break;
      case 2: front += std::string("\xFF\xFE", 2); break;
      case 3: front += std::string("\xFE\xFF", 2); break;
      default: break;
    }
    static const size_t pads[4] = {0, 980, 1000, 1100};
    const size_t pad = pads[(e1 >> 4) & 3];
    if (pad >= 8) {
      front += "<!--" + std::string(pad - 7, ' ') + "-->";
    }
    const unsigned meta_idx = e1 & 15;
    if (meta_idx != 0) {
      const char * named = kEncodings[meta_idx % 20].label;
      std::string tag = (e2 & 2) ? "<META " : "<meta ";
      if (e2 & 1) {
        tag += std::string("http-equiv=Content-Type content=\"text/html; charset=") + named + "\">";
      }
      else {
        tag += std::string("charset=") + named + ">";
      }
      front += tag;
      if (e2 & 4) {
        front += std::string("<meta charset=") + kEncodings[(meta_idx + 3) % 20].label + ">";
      }
    }
    input = front + body;
  }
  const bool has_bom = !fragment && input.size() >= 3 && input.compare(0, 3, bom) == 0;

  const Call whole_call = {"parse", fragment, false, {}};
  const Outcome whole = exec(input, roomy, whole_call);
  check_outcome(whole, roomy, input, input.size());
  if (whole.status == GTEXT_HTML_E_OOM) {
    fail("OOM with the default allocator", "", input);
  }
  if (whole.ok() && !enc_world) {
    const GTEXT_Markup_Encoding_Source want =
        has_bom ? GTEXT_MARKUP_ENCODING_BOM : (roomy.transport ? GTEXT_MARKUP_ENCODING_CALLER : GTEXT_MARKUP_ENCODING_DEFAULT);
    if (whole.source != want) {
      fail("the encoding's source is wrong", std::to_string((int)whole.source), input);
    }
  }

  // The encoding world: the decision, the oracle, the restart, and the other route for UTF-8.
  if (whole.ok() && enc_world) {
    check_the_decision(input, whole, roomy);
    check_against_the_oracle(input, whole, roomy);
    if (whole.reparsed) {
      check_the_restart(input, whole, roomy);
    }
    if (whole.encoding == "UTF-8") {
      check_the_transcoded_route(input, whole, roomy);
    }
  }

  // The tree builder's own answer, with the mark skipped.
  if (whole.ok() && !enc_world) {
    const Reference ref = reference(input, context, roomy.scripting, roomy.max_open);
    if (!ref.ok || ref.dump != whole.tree || ref.mode != whole.mode || ref.dropped != whole.dropped
        || ref.errors.size() != whole.errors.size()) {
      fail("the layer's answer is not the tree builder's", ref.ok ? "" : "the builder failed", input);
    }
    for (size_t i = 0; i < ref.errors.size(); i++) {
      if (ref.errors[i].code != whole.errors[i].code || ref.errors[i].line != whole.errors[i].line
          || ref.errors[i].column != whole.errors[i].column || ref.errors[i].offset != whole.errors[i].offset) {
        fail("an error differs from the tree builder's", whole.errors[i].name, input);
      }
    }
  }

  // The same again (state that outlives a call), and however it is cut.
  {
    const Outcome again = exec(input, roomy, whole_call);
    if (!same(whole, again)) {
      fail("the same input gave two answers", "", input);
    }
  }
  const size_t piece = 1 + (o1 & 15);
  const Cut cuts[] = {{Cut::WHOLE, 0}, {Cut::BYTES, 0}, {Cut::PIECES, piece}, {Cut::RANDOM, (size_t)o2 + 1},
      {Cut::SPLIT, input.empty() ? 0 : (size_t)(o3 >> 3) % (input.size() + 1)}};
  for (const Cut & cut : cuts) {
    const Call call = {"parser", fragment, true, cut};
    const Outcome got = exec(input, roomy, call);
    check_outcome(got, roomy, input, input.size());
    if (!same(whole, got)) {
      fail("the answer depends on how the input is cut",
          "cut " + std::to_string(cut.kind) + " at " + std::to_string(cut.at) + ": " + differ(whole, got), input);
    }
  }

  // A byte order mark in front of a document changes the source and the offsets and nothing else.
  if (!fragment && !enc_world && !has_bom && whole.ok()) {
    const Outcome marked = exec(bom + input, roomy, whole_call);
    if (!marked.ok() || marked.tree != whole.tree || marked.source != GTEXT_MARKUP_ENCODING_BOM
        || marked.errors.size() != whole.errors.size() || marked.dropped != whole.dropped
        || marked.input_bytes != whole.input_bytes + 3) {
      fail("a byte order mark changed the parse", "", input);
    }
    for (size_t i = 0; i < whole.errors.size(); i++) {
      if (marked.errors[i].code != whole.errors[i].code || marked.errors[i].line != whole.errors[i].line
          || marked.errors[i].column != whole.errors[i].column || marked.errors[i].offset != whole.errors[i].offset + 3) {
        fail("a byte order mark moved an error by other than its three bytes", "", input);
      }
    }
  }
  if (fragment && whole.ok()) {
    // U+FEFF in front of a fragment is text: the same bytes with and without the mark differ by that text.
    const Outcome marked = exec(bom + input, roomy, whole_call);
    if (!marked.ok() || marked.source == GTEXT_MARKUP_ENCODING_BOM) {
      fail("a fragment's U+FEFF was taken for a byte order mark", "", input);
    }
  }

  // The error cap and the count-only mode.
  if (whole.ok()) {
    static const size_t caps[4] = {1024, 1, 10, 1000};
    Config capped = roomy;
    capped.max_errors = caps[(o4 >> 2) & 3];
    const Outcome c = exec(input, capped, whole_call);
    check_outcome(c, capped, input, input.size());
    if (!c.ok() || c.tree != whole.tree) {
      fail("the error cap changed the tree", "", input);
    }
    const size_t total = whole.errors.size() + whole.dropped;
    if (c.errors.size() != std::min(capped.max_errors, total) || c.errors.size() + c.dropped != total) {
      fail("kept plus dropped is not every error", std::to_string(total), input);
    }
    for (size_t i = 0; i < c.errors.size(); i++) {
      if (!(c.errors[i] == whole.errors[i])) {
        fail("the cap did not keep the first errors", "", input);
      }
    }
    Config count = roomy;
    count.count_only = true;
    const Outcome k = exec(input, count, whole_call);
    check_outcome(k, count, input, input.size());
    if (!k.ok() || !k.errors.empty() || k.dropped != total || k.tree != whole.tree) {
      fail("count-only is not the whole count and no errors", "", input);
    }
  }

  // The limits: the input's boundary is exact, however the input is cut; the others are their own
  // status, and a run that succeeds under one is the roomy run's.
  if (whole.ok() && input.size() >= 2) { // (a limit of 0 is the default, so one byte has no limit under it; a
                                         // failure of another kind that comes first stands)
    Config exact = roomy;
    exact.max_input = input.size();
    Config under = roomy;
    under.max_input = input.size() - 1;
    for (const Cut & cut : {Cut{Cut::WHOLE, 0}, Cut{Cut::PIECES, piece}, Cut{Cut::BYTES, 0}}) {
      const Call call = {"parser", fragment, true, cut};
      const Outcome at = exec(input, exact, call);
      if (!same(whole, at)) {
        fail("an input exactly at its limit is not the roomy run's", "", input);
      }
      const Outcome below = exec(input, under, call);
      if (below.status != GTEXT_HTML_E_LIMIT_INPUT || below.doc) {
        fail("an input one byte over its limit was not refused", std::to_string((int)below.status), input);
      }
    }
    const Outcome below = exec(input, under, whole_call);
    if (below.status != GTEXT_HTML_E_LIMIT_INPUT || below.doc) {
      fail("an input one byte over its limit was not refused (whole)", "", input);
    }
  }
  {
    Config tiny = roomy;
    tiny.max_open = 1 + (o1 >> 4) % 8;
    const Outcome small = exec(input, tiny, whole_call);
    check_outcome(small, tiny, input, input.size());
    if (small.ok()) {
      if (!whole.ok() || small.tree != whole.tree) {
        fail("a run under a tiny open-element limit succeeded with another answer", "", input);
      }
    }
    else if (small.status != GTEXT_HTML_E_LIMIT_DEPTH && small.status != whole.status) {
      fail("an unexpected status under a tiny open-element limit", std::to_string((int)small.status), input);
    }
    Config few = roomy;
    few.max_nodes = 3 + o2 % 20;
    const Outcome nodes = exec(input, few, whole_call);
    check_outcome(nodes, few, input, input.size());
    if (nodes.ok() && (!whole.ok() || nodes.tree != whole.tree)) {
      fail("a run under a node limit succeeded with another answer", "", input);
    }
    if (!nodes.ok() && nodes.status != GTEXT_HTML_E_LIMIT_NODES && nodes.status != whole.status) {
      fail("an unexpected status under a node limit", std::to_string((int)nodes.status), input);
    }
  }

  // The incremental parser's states.
  if (whole.ok()) {
    GTEXT_HTML_Context_Attribute attr;
    GTEXT_HTML_Fragment_Context fc;
    if (context) {
      fc = make_context(*context, &attr);
    }
    GTEXT_HTML_Parse_Options po = gtext_html_parse_options_default();
    po.scripting = roomy.scripting;
    po.max_open_elements = roomy.max_open;
    // The same encoding options as the whole parse it is compared with: without them the bytes are another document (a `<meta>` or a
    // sniffed label reads them in another encoding, which changes the depth and the tree), which the first soak of Phase 3k found.
    po.transport_encoding = roomy.transport;
    po.transport_encoding_certainty = roomy.tentative ? GTEXT_HTML_TENTATIVE : GTEXT_HTML_CERTAIN;
    po.default_encoding = roomy.fallback;
    po.ignore_meta = roomy.ignore_meta;
    GTEXT_HTML_Parser * p = nullptr;
    if ((context ? gtext_html_parser_new_fragment(&fc, &po, &p) : gtext_html_parser_new(&po, &p)) != GTEXT_HTML_OK) {
      fail("a parser could not be made", "", input);
    }
    const GTEXT_HTML_Status fed = gtext_html_parser_feed(p, input.data(), input.size());
    if (fed != GTEXT_HTML_OK) {
      fail("a feed failed", std::string(gtext_html_status_string(fed)) + (context ? ", in a fragment context" : ", as a document"), input);
    }
    GTEXT_Markup_Document * doc = nullptr;
    GTEXT_Markup_Node * frag = nullptr;
    GTEXT_HTML_Result r;
    const GTEXT_HTML_Status fin = context ? gtext_html_parser_finish_fragment(p, &doc, &frag, &r) : gtext_html_parser_finish(p, &doc, &r);
    if (fin != GTEXT_HTML_OK || !doc) {
      fail("the finish failed", "", input);
    }
    // After a success: state errors, and the document stays ours.
    GTEXT_Markup_Document * other = nullptr;
    GTEXT_Markup_Node * other_frag = nullptr;
    GTEXT_HTML_Result r2;
    const GTEXT_HTML_Status again = context ? gtext_html_parser_finish_fragment(p, &other, &other_frag, &r2)
                                            : gtext_html_parser_finish(p, &other, &r2);
    if (again != GTEXT_HTML_E_STATE || other || r2.status != GTEXT_HTML_E_STATE) {
      fail("a second finish is not a state error", std::to_string((int)again), input);
    }
    gtext_html_result_free(&r2);
    if (gtext_html_parser_feed(p, "x", 1) != GTEXT_HTML_E_STATE) {
      fail("a feed after the finish is not a state error", "", input);
    }
    // Free in the order o1 says: the result before the document, or after.
    if (o1 & 1) {
      gtext_html_result_free(&r);
      gtext_html_parser_free(p);
      const char * inv = markup_check_invariants(doc);
      if (inv) {
        fail("the document was damaged by freeing the result and the parser", inv, input);
      }
    }
    else {
      gtext_html_parser_free(p);
      const char * inv = markup_check_invariants(doc);
      if (inv) {
        fail("the document was damaged by freeing the parser", inv, input);
      }
      gtext_html_result_free(&r);
    }
    // A fragment can be cloned into a document of the caller's, equal to the original.
    if (frag) {
      Outcome target = run_call({"parse", false, false, {}}, "<!DOCTYPE html><p>", nullptr, nullptr);
      GTEXT_Markup_Node * copy = nullptr;
      if (!target.ok() || gtext_markup_clone(target.doc, frag, true, &copy) != GTEXT_MARKUP_OK || !gtext_markup_equal(frag, copy)) {
        fail("a fragment cloned into another document is not equal", "", input);
      }
      // Its children go in as the body's last.
      GTEXT_Markup_Node * body_el = nullptr;
      const GTEXT_Markup_Node * root = gtext_markup_document_root(target.doc);
      for (GTEXT_Markup_Node * n = gtext_markup_node_next_in_order(root, root, false); n;
           n = gtext_markup_node_next_in_order(root, n, false)) {
        if (gtext_markup_node_kind(n) == GTEXT_MARKUP_KIND_ELEMENT && gtext_markup_element_is(n, GTEXT_MARKUP_NS_HTML, "body")) {
          body_el = n;
        }
      }
      if (!body_el || gtext_markup_append_child(body_el, copy) != GTEXT_MARKUP_OK) {
        // Only a limit can refuse it; the target is a default document, so it should not.
        fail("a cloned fragment could not be inserted", "", input);
      }
      const char * inv = markup_check_invariants(target.doc);
      if (inv) {
        fail("the target document is damaged by the inserted fragment", inv, input);
      }
    }
    gtext_markup_document_free(doc);
  }
  else if (is_failure_status(whole.status)) {
    // A failed parser repeats its failure to every call, whatever the call's arguments and whatever its kind.
    GTEXT_HTML_Context_Attribute attr;
    GTEXT_HTML_Fragment_Context fc;
    if (context) {
      fc = make_context(*context, &attr);
    }
    GTEXT_HTML_Parse_Options po = gtext_html_parse_options_default();
    po.max_open_elements = roomy.max_open;
    po.scripting = roomy.scripting;
    GTEXT_HTML_Parser * p = nullptr;
    if ((context ? gtext_html_parser_new_fragment(&fc, &po, &p) : gtext_html_parser_new(&po, &p)) == GTEXT_HTML_OK) {
      const GTEXT_HTML_Status fed = gtext_html_parser_feed(p, input.data(), input.size());
      GTEXT_Markup_Document * doc = nullptr;
      GTEXT_Markup_Node * frag = nullptr;
      GTEXT_HTML_Result r;
      const GTEXT_HTML_Status fin = context ? gtext_html_parser_finish_fragment(p, &doc, &frag, &r)
                                            : gtext_html_parser_finish(p, &doc, &r);
      if (fin != whole.status || doc || (fed != GTEXT_HTML_OK && fed != fin)) {
        fail("a failed parser does not repeat its failure", std::to_string((int)fin), input);
      }
      gtext_html_result_free(&r);
      if (gtext_html_parser_feed(p, "x", 1) != fin || gtext_html_parser_feed(p, nullptr, 3) != fin) {
        fail("a feed after a failure is not the failure", "", input);
      }
      GTEXT_Markup_Document * other = nullptr;
      GTEXT_Markup_Node * other_frag = nullptr;
      GTEXT_HTML_Result r2;
      // The wrong kind of finish and a NULL output are complaints about the call, and the failure comes first.
      const GTEXT_HTML_Status wrong = context ? gtext_html_parser_finish(p, &other, &r2)
                                              : gtext_html_parser_finish_fragment(p, &other, &other_frag, &r2);
      if (wrong != fin || other) {
        fail("the wrong kind of finish after a failure is not the failure", std::to_string((int)wrong), input);
      }
      gtext_html_result_free(&r2);
      GTEXT_HTML_Result r3;
      if ((context ? gtext_html_parser_finish_fragment(p, nullptr, &other_frag, &r3) : gtext_html_parser_finish(p, nullptr, &r3)) != fin) {
        fail("a NULL output after a failure is not the failure", "", input);
      }
      gtext_html_result_free(&r3);
      gtext_html_parser_free(p);
    }
  }

  // An allocation that fails, anywhere: OOM with no document, or the normal answer, and no leak. The
  // sweep goes on until a run completes with no failure injected. The entry point is one the options
  // choose, so that over a corpus every one is swept.
  if (whole.ok()) {
    Cut cut;
    bool use_parser = (o3 & 1) != 0;
    cut.kind = (o3 & 2) ? Cut::BYTES : (use_parser ? Cut::PIECES : Cut::WHOLE);
    cut.at = piece;
    const Call sweep_call = {"sweep", fragment, use_parser, cut};
    bool reached_the_end = false;
    // The number of allocations the call makes, measured on a run that fails none: the sweep goes one past it, so
    // that a dense input has the room it needs and a call that allocates without end is a failure, not a timeout.
    size_t allocations = 0;
    {
      FaultAllocator counting;
      Config c = roomy;
      c.fa = &counting;
      c.arm = 0;
      c.want_result = (o4 & 32) == 0;
      Outcome r = exec(input, c, sweep_call);
      allocations = counting.calls;
      if (!r.ok()) {
        fail("the counting run failed", "", input);
      }
    }
    for (size_t n = 1; n <= allocations + 2; n++) {
      FaultAllocator fa;
      Config c = roomy;
      c.fa = &fa;
      c.arm = n;
      c.want_result = (o4 & 32) == 0;
      {
        Outcome r = exec(input, c, sweep_call);
        if (!fa.failed) {
          if (!r.ok() || r.tree != whole.tree || (c.want_result && r.errors != whole.errors)) {
            fail("the run past the last allocation is not the normal one", std::to_string(n), input);
          }
          reached_the_end = true;
        }
        else if (r.ok()) {
          if (r.tree != whole.tree) {
            fail("an absorbed allocation failure changed the tree", std::to_string(n), input);
          }
          if (c.want_result && (r.errors != whole.errors || r.dropped != whole.dropped || r.node_count != whole.node_count)) {
            fail("a success after a failed allocation has an incomplete result", std::to_string(n), input);
          }
        }
        else {
          if (r.status != GTEXT_HTML_E_OOM) {
            fail("an allocation failure that is not OOM", std::to_string(n) + " status " + std::to_string((int)r.status), input);
          }
          if (r.doc || r.fragment) {
            fail("a failed allocation left a document", std::to_string(n), input);
          }
        }
      }
      if (fa.bad_frees != 0) {
        fail("a free of a block the allocator did not make", std::to_string(n), input);
      }
      if (!fa.live.empty()) {
        fail("a leak after a failed allocation", std::to_string(n), input);
      }
      if (reached_the_end) {
        break;
      }
    }
    if (!reached_the_end) {
      fail("the allocation sweep did not reach the end of the call", std::to_string(allocations) + " allocations counted", input);
    }
  }
  return 0;
}
