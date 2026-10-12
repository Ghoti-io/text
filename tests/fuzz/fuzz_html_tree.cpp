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
 * libFuzzer harness for the HTML tree builder (src/html/html_tree*.c).
 *
 * The input is four option bytes and then bytes for the parser. Bit 1 of the first option byte says the
 * bytes are a fragment, and its high six bits choose the context element of the HTML fragment parsing
 * algorithm from a table (kContexts: HTML elements of every mode and tokenizer state, SVG and MathML
 * elements including each integration point, an annotation-xml with and without the encoding that makes
 * it one, a form, a document in quirks mode); otherwise the bytes are a document. The same bytes are
 * parsed whole, a byte at a time, in pieces of a size the options choose and in
 * pieces of random sizes, and the properties are the ones that make those one parse:
 *
 *     the status, the tree (in the tree-construction dump, a fragment's too), the document mode and
 *     every parse error (code, line, column, offset) are the same however the bytes
 *     were cut;
 *     the tree's invariant checker holds on every tree the parser returns, and the
 *     tree is a valid HTML tree: a document has one `html` element with a `head`
 *     and then a `body` or a `frameset` and nothing else among its elements but
 *     `noframes` after a frameset (a fragment's document has an empty `html` and its
 *     fragment holds the result), every
 *     element is in the HTML, SVG or MathML namespace, an HTML one with a lower-case name and
 *     a foreign one with a name an HTML parser reads back as written (the standard's table for
 *     SVG), attributes in no namespace except on foreign elements, where the XLink, XML and
 *     XMLNS namespaces are allowed, no CDATA section, no empty text node and no two adjacent
 *     ones, and the document mode is set to one of the three; and, since stage 3d, the shapes
 *     the table modes and the template modes make: a template element has a content
 *     fragment and no children of its own and nothing else has a content; a tr is
 *     in a tbody, thead, tfoot or a template's content; a td or th in a tr or a
 *     template's content; a caption, colgroup, tbody, thead or tfoot in a table or a
 *     template's content; a col in a colgroup or a template's content, and empty;
 *     the text directly in a table, tbody, thead, tfoot or tr is white space (the
 *     rest is foster parented); the walk visits the template contents too;
 *     a parse that is not OK is one of the distinct failure statuses, has no tree,
 *     and says the same on every call after it (it is sticky);
 *     a limit that is tiny (open elements 1 to 8, nodes) is its own status and when
 *     the parse still succeeds the tree is the roomy run's;
 *     an allocation that fails - the first to the last of a run - is OOM
 *     with no tree, or the answer the run would have given, and never a leak or a
 *     free of a block the allocator did not make;
 *     the work done stays within GTEXT_HTML_TREE_WORK_BOUND_AT(n, 64) at a fixed open-element limit.
 *
 * The seeds (tests/fuzz/corpus/html-tree) are the inputs of the tree-construction
 * vectors, made by tools/html/make-tree-fuzz-seeds.py.
 *
 * Build with: make fuzz-html-tree      Run: make fuzz-run-html-tree
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../include/ghoti.io/text/markup/markup_html.h"
#include "../../src/html/html_tree_internal.h"
#include "../../src/markup/markup_internal.h"
#include "../../tools/conformance/html_tree_dump.h"
#include "../markup_test_support.h"

namespace {

[[noreturn]] void fail(const char * what, const std::string & detail, const std::string & input) {
  std::fprintf(stderr, "fuzz_html_tree: %s\n%s\ninput (%zu bytes): ", what, detail.c_str(), input.size());
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

struct Result {
  GTEXT_HTML_TreeStatus status = GTEXT_HTML_TREE_OK;
  bool has_tree = false;
  std::string dump;
  int mode = 0;
  std::vector<GTEXT_HTML_TreeError> errors;
  size_t dropped = 0;
  size_t work = 0;
  std::string problem; // a property of the tree that failed
};

bool same(const Result & a, const Result & b) {
  if (a.status != b.status || a.has_tree != b.has_tree || a.dump != b.dump
      || a.mode != b.mode || a.dropped != b.dropped || a.errors.size() != b.errors.size()) {
    return false;
  }
  for (size_t i = 0; i < a.errors.size(); i++) {
    if (a.errors[i].code != b.errors[i].code || a.errors[i].line != b.errors[i].line
        || a.errors[i].column != b.errors[i].column || a.errors[i].offset != b.errors[i].offset) {
      return false;
    }
  }
  return true;
}

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

struct Config {
  const FuzzCtx * ctx = nullptr; // a fragment in this context, or a document
  bool scripting = false;
  size_t max_open = 0;
  size_t max_nodes = 0;
  size_t arm = 0; // fail this allocation, 0 never
  FaultAllocator * fa = nullptr;
};

// cuts: piece sizes in turn (the rest in one); one_byte: a byte at a time; seed != 0: random pieces.
Result run(const std::string & input, const Config & cfg, const std::vector<size_t> & cuts, bool one_byte,
    uint64_t seed, bool check) {
  Result r;
  GTEXT_HTML_TreeOptions o = {};
  o.scripting = cfg.scripting;
  o.max_open_elements = cfg.max_open;
  o.max_nodes = cfg.max_nodes;
  o.max_errors = 4096;
  o.hash_seed = 99;
  if (cfg.fa) {
    o.allocator = &cfg.fa->alloc;
    cfg.fa->arm(cfg.arm);
  }
  GTEXT_HTML_Tree * tree = nullptr;
  if (cfg.ctx) {
    GTEXT_HTML_FragmentContext fc;
    std::memset(&fc, 0, sizeof(fc));
    fc.ns = cfg.ctx->ns;
    fc.local_name = cfg.ctx->name;
    fc.local_name_length = std::strlen(cfg.ctx->name);
    GTEXT_HTML_Attribute enc;
    std::memset(&enc, 0, sizeof(enc));
    if (cfg.ctx->encoding) {
      enc.name = "encoding";
      enc.name_length = 8;
      enc.value = cfg.ctx->encoding;
      enc.value_length = std::strlen(cfg.ctx->encoding);
      fc.attributes = &enc;
      fc.attribute_count = 1;
    }
    fc.form_ancestor = cfg.ctx->form;
    fc.quirks = cfg.ctx->quirks;
    fc.scripting = cfg.scripting;
    GTEXT_HTML_TreeStatus st = GTEXT_HTML_TREE_OK;
    tree = gtext_html_tree_new_fragment(&o, &fc, &st);
    if (!tree) {
      if (st != GTEXT_HTML_TREE_E_OOM && !GTEXT_HTML_TREE_IS_LIMIT(st)) {
        fail("a fragment builder could not be made, and not for memory or a limit", std::to_string((int)st), input);
      }
      r.status = st;
      return r;
    }
  }
  else {
    tree = gtext_html_tree_new(&o);
  }
  if (!tree) {
    r.status = GTEXT_HTML_TREE_E_OOM;
    return r;
  }
  GTEXT_HTML_TreeStatus s = GTEXT_HTML_TREE_OK;
  size_t at = 0;
  size_t step = 0;
  uint64_t rng = seed * 6364136223846793005ULL + 1442695040888963407ULL;
  while (at < input.size() && s == GTEXT_HTML_TREE_OK) {
    size_t n = input.size() - at;
    if (one_byte) {
      n = 1;
    }
    else if (seed) {
      rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
      n = 1 + (rng >> 33) % 9;
    }
    else if (step < cuts.size()) {
      n = cuts[step++];
    }
    n = n < input.size() - at ? n : input.size() - at;
    std::vector<char> piece(input.begin() + at, input.begin() + at + n);
    s = gtext_html_tree_feed(tree, piece.data(), n);
    at += n;
  }
  if (s == GTEXT_HTML_TREE_OK) {
    s = gtext_html_tree_finish(tree);
  }
  r.status = s;
  for (size_t i = 0; i < gtext_html_tree_error_count(tree); i++) {
    r.errors.push_back(*gtext_html_tree_error_at(tree, i));
  }
  r.dropped = gtext_html_tree_errors_dropped(tree);
  r.work = gtext_html_tree_work(tree);
  if (s != GTEXT_HTML_TREE_OK) {
    // Sticky: the same status on every later call, and no document.
    if (gtext_html_tree_feed(tree, "x", 1) != s && s != GTEXT_HTML_TREE_E_STATE) {
      fail("a failure is not sticky (feed)", "", input);
    }
    if (gtext_html_tree_finish(tree) != s && s != GTEXT_HTML_TREE_E_STATE) {
      fail("a failure is not sticky (finish)", "", input);
    }
    if (gtext_html_tree_status(tree) != s) {
      fail("the status accessor disagrees with the call that failed", "", input);
    }
    if (gtext_html_tree_take_document(tree) != nullptr) {
      fail("a failed parse gave a document", "", input);
    }
    GTEXT_Markup_Document * none = nullptr;
    if (gtext_html_tree_take_fragment(tree, &none) != nullptr || none != nullptr) {
      fail("a failed parse gave a fragment", "", input);
    }
  }
  else {
    GTEXT_Markup_Document * doc = nullptr;
    GTEXT_Markup_Node * frag = nullptr;
    if (cfg.ctx) {
      if (gtext_html_tree_take_document(tree) != nullptr) {
        fail("a fragment builder gave a document of its own", "", input);
      }
      frag = gtext_html_tree_take_fragment(tree, &doc);
      if (!frag || !doc) {
        fail("a successful fragment parse gave no fragment", "", input);
      }
      GTEXT_Markup_Document * again = nullptr;
      if (gtext_html_tree_take_fragment(tree, &again) != nullptr || again != nullptr) {
        fail("a fragment was given twice", "", input);
      }
    }
    else {
      doc = gtext_html_tree_take_document(tree);
      if (!doc) {
        fail("a successful parse gave no document", "", input);
      }
    }
    r.has_tree = true;
    if (check) {
      r.problem = check_tree(doc, frag);
    }
    r.mode = (int)gtext_markup_document_mode(doc);
    size_t n = 0;
    char * d = frag ? htd_dump_children(frag, &n) : htd_dump(doc, &n);
    r.dump = d ? std::string(d, n) : "";
    std::free(d);
    gtext_markup_document_free(doc);
  }
  gtext_html_tree_free(tree);
  return r;
}

bool is_failure_status(GTEXT_HTML_TreeStatus s) {
  return s == GTEXT_HTML_TREE_E_OOM || GTEXT_HTML_TREE_IS_LIMIT(s);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 4 || size > 4096) {
    return 0;
  }
  const uint8_t o0 = data[0], o1 = data[1], o2 = data[2], o3 = data[3];
  const std::string input((const char *)data + 4, size - 4);

  Config roomy;
  roomy.scripting = (o0 & 1) != 0;
  if ((o0 & 2) != 0) {
    roomy.ctx = &kContexts[(o0 >> 2) & 63];
  }
  roomy.max_open = 64; // a fixed limit, so that the work bound below is a bound
  Result whole = run(input, roomy, {}, false, 0, true);
  if (whole.status != GTEXT_HTML_TREE_OK && !is_failure_status(whole.status)) {
    fail("an unexpected status", std::to_string((int)whole.status), input);
  }
  if (whole.status == GTEXT_HTML_TREE_E_OOM) {
    fail("OOM with the default allocator", "", input);
  }
  if (!whole.problem.empty()) {
    fail("the tree is not a valid HTML tree", whole.problem, input);
  }
  if (whole.status != GTEXT_HTML_TREE_OK && whole.has_tree) {
    fail("a failed parse has a tree", "", input);
  }
  if (whole.work > GTEXT_HTML_TREE_WORK_BOUND_AT(input.size(), roomy.max_open)) {
    fail("the work is not linear", "work " + std::to_string(whole.work) + " for " + std::to_string(input.size()) + " bytes",
        input);
  }

  // The same parse again, to catch state that outlives a parser.
  Result again = run(input, roomy, {}, false, 0, false);
  if (!same(whole, again)) {
    fail("the same input gave two answers", "", input);
  }

  // However it is cut.
  const Result bytes = run(input, roomy, {}, true, 0, false);
  if (!same(whole, bytes)) {
    fail("the answer depends on feeding a byte at a time", "", input);
  }
  const size_t piece = 1 + (o1 % 13);
  std::vector<size_t> cuts(input.size() / piece + 2, piece);
  const Result pieces = run(input, roomy, cuts, false, 0, false);
  if (!same(whole, pieces)) {
    fail("the answer depends on the size of the pieces", std::to_string(piece), input);
  }
  const Result random = run(input, roomy, {}, false, (uint64_t)o2 + 1, false);
  if (!same(whole, random)) {
    fail("the answer depends on random pieces", "", input);
  }
  const Result split = run(input, roomy, {input.size() ? (size_t)(o3 % (input.size() + 1)) : 0}, false, 0, false);
  if (!same(whole, split)) {
    fail("the answer depends on one cut", std::to_string(o3 % (input.size() + 1)), input);
  }

  // Tiny limits: their own status, and a successful run is the roomy run's.
  Config tiny = roomy;
  tiny.max_open = 1 + (o1 >> 4) % 8;
  const Result small = run(input, tiny, {}, false, 0, true);
  if (small.status == GTEXT_HTML_TREE_OK) {
    if (whole.status != GTEXT_HTML_TREE_OK || small.dump != whole.dump) {
      fail("a run under a tiny open-element limit succeeded with another answer", std::to_string(tiny.max_open), input);
    }
  }
  else if (small.status == GTEXT_HTML_TREE_E_LIMIT_DEPTH) {
    if (small.has_tree) {
      fail("the depth limit left a tree", "", input);
    }
  }
  else if (small.status != whole.status
      && !GTEXT_HTML_TREE_IS_LIMIT(small.status)) {
    fail("an unexpected status under a tiny limit", std::to_string((int)small.status), input);
  }
  if (!small.problem.empty()) {
    fail("the tree under a tiny limit is not valid", small.problem, input);
  }
  Config few = roomy;
  few.max_nodes = 3 + o2 % 20;
  const Result nodes = run(input, few, {}, false, 0, true);
  if (nodes.status == GTEXT_HTML_TREE_OK && (whole.status != GTEXT_HTML_TREE_OK || nodes.dump != whole.dump)) {
    fail("a run under a node limit succeeded with another answer", "", input);
  }
  if (nodes.status == GTEXT_HTML_TREE_E_LIMIT_NODES && nodes.has_tree) {
    fail("the node limit left a tree", "", input);
  }

  // An allocation that fails, anywhere: OOM with no tree, or the normal answer, and no leak. The sweep goes
  // on until a run completes with no failure injected: the last allocation of the parse has been reached.
  // (A generous cap stops a run that never completes, which would itself be a defect.)
  bool reached_the_end = false;
  for (size_t n = 1; n <= 20000; n++) {
    FaultAllocator fa;
    Config c = roomy;
    c.fa = &fa;
    c.arm = n;
    const Result r = run(input, c, o3 & 1 ? std::vector<size_t>{} : cuts, o3 & 2 ? true : false, 0, false);
    if (fa.bad_frees != 0) {
      fail("a free of a block the allocator did not make", std::to_string(n), input);
    }
    if (!fa.live.empty()) {
      fail("a leak after a failed allocation", std::to_string(n), input);
    }
    if (!fa.failed) {
      if (!same(whole, r)) {
        fail("the run past the last allocation is not the normal one", std::to_string(n), input);
      }
      reached_the_end = true;
      break;
    }
    if (r.status == GTEXT_HTML_TREE_OK) {
      if (r.dump != whole.dump) {
        fail("an absorbed allocation failure changed the tree", std::to_string(n), input);
      }
    }
    else if (r.status != GTEXT_HTML_TREE_E_OOM && r.status != whole.status) {
      fail("an allocation failure that is not OOM", std::to_string(n) + " status " + std::to_string((int)r.status), input);
    }
    if (r.status != GTEXT_HTML_TREE_OK && r.has_tree) {
      fail("a failed allocation left a tree", std::to_string(n), input);
    }
  }
  if (!reached_the_end) {
    fail("the allocation sweep did not reach the end of the parse", "20000 allocations", input);
  }
  return 0;
}
