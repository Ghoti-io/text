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
 * The HTML tree builder's foreign content and fragment parsing (src/html/html_tree_foreign.c and
 * html_tree_fragment.c), stage 3e.
 *
 * Every expectation is worked out by hand from the standard's tree construction chapter (13.2.6 and 13.2.6.5
 * for foreign content, 13.4 for fragments; read as served on 2026-10-12) and written as the tree-construction
 * dump, not taken from what the code returned. The web-platform-tests vectors are the conformance gate
 * (`make conformance-html-tree`); this file is what that gate cannot be: the matrix rows by name, the
 * adjustment tables as data checked against the vocabulary the writers use, every breakout tag and every
 * integration point, the contexts of the fragment algorithm and what each one decides, the API's argument
 * checks, every cut of every fragment vector, an allocation failure at every point, the limits, and the work
 * counted on the inputs that make a naive implementation quadratic.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace std::string_literals;

#include "../src/html/html_tree_internal.h"
#include "../src/html/html_tree_private.h"
#include "../src/markup/markup_internal.h"
#include "../tools/conformance/html_tree_dump.h"
#include "markup_test_support.h"

namespace {

// The context element of a fragment, as the tests write it.
struct Ctx {
  GTEXT_HTML_Namespace ns = GTEXT_HTML_NS_HTML;
  std::string name;
  std::vector<std::pair<std::string, std::string>> attrs;
  bool form = false;
  GTEXT_Markup_Mode quirks = GTEXT_MARKUP_MODE_NO_QUIRKS;
  bool scripting = false;
};

Ctx html_ctx(const std::string & name) {
  Ctx c;
  c.name = name;
  return c;
}
Ctx svg_ctx(const std::string & name) {
  Ctx c;
  c.ns = GTEXT_HTML_NS_SVG;
  c.name = name;
  return c;
}
Ctx math_ctx(const std::string & name) {
  Ctx c;
  c.ns = GTEXT_HTML_NS_MATHML;
  c.name = name;
  return c;
}

// The result of one parse, of a document or of a fragment.
struct Out {
  GTEXT_HTML_TreeStatus status = GTEXT_HTML_TREE_OK;
  bool has_tree = false;
  std::string dump;
  std::string invariants; // empty when they hold
  std::vector<GTEXT_HTML_TreeError> errors;
  size_t dropped = 0;
  size_t work = 0;
  std::string first_mode; // the insertion mode before the first byte (fragments)

  std::vector<std::string> error_names() const {
    std::vector<std::string> v;
    for (const auto & e : errors) {
      v.push_back(gtext_html_tree_error_name(e.code));
    }
    return v;
  }
  bool has_error(const char * name) const {
    for (const auto & n : error_names()) {
      if (n == name) {
        return true;
      }
    }
    return false;
  }
  bool same_as(const Out & o) const {
    if (status != o.status || has_tree != o.has_tree || dump != o.dump || dropped != o.dropped
        || errors.size() != o.errors.size()) {
      return false;
    }
    for (size_t i = 0; i < errors.size(); i++) {
      if (errors[i].code != o.errors[i].code || errors[i].line != o.errors[i].line
          || errors[i].column != o.errors[i].column || errors[i].offset != o.errors[i].offset) {
        return false;
      }
    }
    return true;
  }
};

// Parse `input` as a document (ctx == nullptr) or as a fragment in `ctx`; fed whole, in the pieces `cuts`
// names (the rest after them), or a byte at a time. Each piece is a heap copy freed at once, so a builder that
// kept a pointer into one is a use-after-free for the sanitizers; so is the context's attribute array.
Out run(const std::string & input, const Ctx * ctx = nullptr, GTEXT_HTML_TreeOptions o = {},
    const std::vector<size_t> & cuts = {}, bool one_byte = false) {
  Out r;
  if (o.max_errors == 0) {
    o.max_errors = 100000;
  }
  GTEXT_HTML_Tree * tree = nullptr;
  if (ctx) {
    GTEXT_HTML_FragmentContext fc;
    std::memset(&fc, 0, sizeof(fc));
    fc.ns = ctx->ns;
    char * name = static_cast<char *>(std::malloc(ctx->name.size() + 1));
    std::memcpy(name, ctx->name.c_str(), ctx->name.size() + 1);
    fc.local_name = name;
    fc.local_name_length = ctx->name.size();
    std::vector<GTEXT_HTML_Attribute> attrs(ctx->attrs.size());
    std::vector<char *> owned;
    for (size_t i = 0; i < ctx->attrs.size(); i++) {
      char * n = static_cast<char *>(std::malloc(ctx->attrs[i].first.size() + 1));
      char * v = static_cast<char *>(std::malloc(ctx->attrs[i].second.size() + 1));
      std::memcpy(n, ctx->attrs[i].first.c_str(), ctx->attrs[i].first.size() + 1);
      std::memcpy(v, ctx->attrs[i].second.c_str(), ctx->attrs[i].second.size() + 1);
      owned.push_back(n);
      owned.push_back(v);
      std::memset(&attrs[i], 0, sizeof(attrs[i]));
      attrs[i].name = n;
      attrs[i].name_length = ctx->attrs[i].first.size();
      attrs[i].value = v;
      attrs[i].value_length = ctx->attrs[i].second.size();
    }
    fc.attributes = attrs.empty() ? nullptr : attrs.data();
    fc.attribute_count = attrs.size();
    fc.form_ancestor = ctx->form;
    fc.quirks = ctx->quirks;
    fc.scripting = ctx->scripting;
    GTEXT_HTML_TreeStatus st = GTEXT_HTML_TREE_OK;
    tree = gtext_html_tree_new_fragment(&o, &fc, &st);
    // The context is by value: nothing of it may be read after the call.
    std::free(name);
    for (char * p : owned) {
      std::free(p);
    }
    if (!tree) {
      r.status = st;
      return r;
    }
    r.first_mode = gtext_html_tree_mode_name(tree);
  }
  else {
    tree = gtext_html_tree_new(&o);
    if (!tree) {
      r.status = GTEXT_HTML_TREE_E_OOM;
      return r;
    }
  }
  size_t at = 0;
  GTEXT_HTML_TreeStatus s = GTEXT_HTML_TREE_OK;
  auto feed = [&](size_t n) {
    if (s == GTEXT_HTML_TREE_OK) {
      std::vector<char> piece(input.begin() + at, input.begin() + at + n);
      s = gtext_html_tree_feed(tree, piece.data(), n);
    }
    at += n;
  };
  if (one_byte) {
    while (at < input.size()) {
      feed(1);
    }
  }
  else {
    for (size_t n : cuts) {
      n = std::min(n, input.size() - at);
      feed(n);
    }
    if (at < input.size()) {
      feed(input.size() - at);
    }
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
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_Markup_Node * frag = nullptr;
  if (ctx) {
    EXPECT_EQ(gtext_html_tree_take_document(tree), nullptr) << "a fragment builder hands over its document with the fragment";
    frag = gtext_html_tree_take_fragment(tree, &doc);
  }
  else {
    doc = gtext_html_tree_take_document(tree);
  }
  if (s == GTEXT_HTML_TREE_OK) {
    EXPECT_NE(doc, nullptr);
  }
  else {
    EXPECT_EQ(doc, nullptr);
  }
  if (doc) {
    r.has_tree = true;
    const char * bad = markup_check_invariants(doc);
    r.invariants = bad ? bad : "";
    size_t n = 0;
    char * d = frag ? htd_dump_children(frag, &n) : htd_dump(doc, &n);
    r.dump = d ? std::string(d, n) : "(dump failed)";
    std::free(d);
    gtext_markup_document_free(doc);
  }
  gtext_html_tree_free(tree);
  return r;
}

std::string tree(const std::vector<std::string> & lines) {
  std::string out;
  for (const auto & l : lines) {
    out += "| " + l + "\n";
  }
  return out;
}

// The dump of a document (invariants checked).
std::string doc_of(const std::string & input) {
  const Out r = run(input);
  EXPECT_EQ(r.status, GTEXT_HTML_TREE_OK) << input;
  EXPECT_EQ(r.invariants, "") << input;
  return r.dump;
}

// The dump of a fragment (invariants checked).
std::string frag_of(const std::string & input, const Ctx & ctx) {
  const Out r = run(input, &ctx);
  EXPECT_EQ(r.status, GTEXT_HTML_TREE_OK) << input;
  EXPECT_EQ(r.invariants, "") << input;
  return r.dump;
}

// A body with these lines in it.
std::string in_body(const std::vector<std::string> & lines) {
  std::vector<std::string> all = {"<html>", "  <head>", "  <body>"};
  for (const auto & l : lines) {
    all.push_back("    " + l);
  }
  return tree(all);
}

std::string repeat(const std::string & s, size_t n) {
  std::string out;
  out.reserve(s.size() * n);
  for (size_t i = 0; i < n; i++) {
    out += s;
  }
  return out;
}

} // namespace

// ---- The matrix of the specification --------------------------------------

TEST(HtmlTreeForeign, InlineSvgKeepsItsNamespaceAndTheCaseOfNamesTheStandardRestores) {
  const Out r = run("<svg viewBox=\"0 0 1 1\"><clipPath/></svg>");
  ASSERT_EQ(r.status, GTEXT_HTML_TREE_OK);
  EXPECT_EQ(r.invariants, "");
  EXPECT_EQ(r.dump, in_body({"<svg svg>", "  viewBox=\"0 0 1 1\"", "  <svg clipPath>"}));
  // The self-closing flag of a foreign element is acknowledged: the only error is the missing doctype.
  EXPECT_EQ(r.error_names(), std::vector<std::string>{"missing-doctype"});
  // The name's case is the standard's, not the input's, and a name not in the table is lower case.
  EXPECT_EQ(doc_of("<svg><CLIPPATH><ClipPath><clippathx>"),
      in_body({"<svg svg>", "  <svg clipPath>", "    <svg clipPath>", "      <svg clippathx>"}));
}

TEST(HtmlTreeForeign, MathmlTextAndAnnotationXmlAsAnHtmlIntegrationPoint) {
  const Out r = run("<math><mi>x</mi><annotation-xml encoding=\"text/html\"><p>");
  ASSERT_EQ(r.status, GTEXT_HTML_TREE_OK);
  EXPECT_EQ(r.dump,
      in_body({"<math math>", "  <math mi>", "    \"x\"", "  <math annotation-xml>", "    encoding=\"text/html\"",
          "    <p>"}));
  EXPECT_EQ(r.invariants, "");
}

TEST(HtmlTreeForeign, HtmlStartTagsBreakOutOfForeignContent) {
  Out r = run("<svg><p>");
  EXPECT_EQ(r.dump, in_body({"<svg svg>", "<p>"}));
  EXPECT_TRUE(r.has_error("html-in-foreign-content"));
  r = run("<svg><font color=r>");
  EXPECT_EQ(r.dump, in_body({"<svg svg>", "<font>", "  color=\"r\""}));
  EXPECT_TRUE(r.has_error("html-in-foreign-content"));
  // `font` with none of color, face, size is not one of them.
  r = run("<svg><font>");
  EXPECT_EQ(r.dump, in_body({"<svg svg>", "  <svg font>"}));
  EXPECT_FALSE(r.has_error("html-in-foreign-content"));
  for (const char * attr : {"face=x", "size=1", "color", "COLOR=x"}) {
    SCOPED_TRACE(attr);
    EXPECT_TRUE(run(std::string("<svg><font ") + attr + ">").has_error("html-in-foreign-content"));
  }
  EXPECT_FALSE(run("<svg><font id=x class=y>").has_error("html-in-foreign-content"));
}

TEST(HtmlTreeForeign, ForeignObjectHoldsHtml) {
  EXPECT_EQ(doc_of("<svg><foreignObject><div>x</div>y"),
      in_body({"<svg svg>", "  <svg foreignObject>", "    <div>", "      \"x\"", "    \"y\""}));
}

TEST(HtmlTreeForeign, NamespacedAttributesAreAdjusted) {
  const std::string in =
      "<svg xlink:href=\"#a\" xml:lang=\"x\" xml:space=\"preserve\" xmlns=\"http://www.w3.org/2000/svg\" "
      "xmlns:xlink=\"http://www.w3.org/1999/xlink\" xlink:title=t xlink:actuate=a xlink:arcrole=r xlink:role=l "
      "xlink:show=s xlink:type=y other:name=o>";
  const Out r = run(in);
  ASSERT_EQ(r.status, GTEXT_HTML_TREE_OK);
  EXPECT_EQ(r.invariants, "");
  // Sorted by the name as the dump writes it: the prefix word, then the local name.
  EXPECT_EQ(r.dump,
      in_body({"<svg svg>", "  other:name=\"o\"", "  xlink actuate=\"a\"", "  xlink arcrole=\"r\"",
          "  xlink href=\"#a\"", "  xlink role=\"l\"", "  xlink show=\"s\"", "  xlink title=\"t\"",
          "  xlink type=\"y\"", "  xml lang=\"x\"", "  xml space=\"preserve\"", "  xmlns xlink=\"http://www.w3.org/1999/xlink\"",
          "  xmlns xmlns=\"http://www.w3.org/2000/svg\""}));
  // They are attributes of the elements of foreign content only: an HTML element keeps the name as written.
  EXPECT_EQ(doc_of("<p xlink:href=a xml:lang=b xmlns=c>"), in_body({"<p>", "  xlink:href=\"a\"", "  xml:lang=\"b\"", "  xmlns=\"c\""}));
  // The attribute is in a namespace of the tree, not just spelled with a prefix.
  GTEXT_Markup_Document * doc = nullptr;
  ASSERT_EQ(gtext_html_parse(in.data(), in.size(), nullptr, &doc, nullptr), GTEXT_HTML_TREE_OK);
  const GTEXT_Markup_Node * body = gtext_markup_node_last_child(gtext_markup_node_last_child(gtext_markup_document_root(doc)));
  const GTEXT_Markup_Node * svg = gtext_markup_node_first_child(body);
  ASSERT_NE(svg, nullptr);
  bool seen = false;
  for (size_t i = 0; i < gtext_markup_element_attribute_count(svg); i++) {
    GTEXT_Markup_Attribute a;
    ASSERT_EQ(gtext_markup_element_attribute_at(svg, i, &a), GTEXT_MARKUP_OK);
    if (std::string(a.local) == "href") {
      seen = true;
      EXPECT_STREQ(a.ns, GTEXT_MARKUP_NS_XLINK);
    }
  }
  EXPECT_TRUE(seen);
  gtext_markup_document_free(doc);
}

TEST(HtmlTreeForeign, AnXmlnsThatIsNotTheElementsNamespaceIsAParseError) {
  // "If element has an xmlns attribute in the XMLNS namespace whose value is not exactly the same as the
  // element's namespace, that is a parse error. Similarly, if element has an xmlns:xlink attribute ...
  // whose value is not the XLink Namespace".
  EXPECT_FALSE(run("<svg xmlns=\"http://www.w3.org/2000/svg\">").has_error("xmlns-attribute-mismatch"));
  EXPECT_TRUE(run("<svg xmlns=\"http://example.com/\">").has_error("xmlns-attribute-mismatch"));
  EXPECT_FALSE(run("<math xmlns=\"http://www.w3.org/1998/Math/MathML\">").has_error("xmlns-attribute-mismatch"));
  EXPECT_TRUE(run("<math xmlns=\"http://www.w3.org/2000/svg\">").has_error("xmlns-attribute-mismatch"));
  EXPECT_FALSE(run("<svg xmlns:xlink=\"http://www.w3.org/1999/xlink\">").has_error("xmlns-attribute-mismatch"));
  EXPECT_TRUE(run("<svg xmlns:xlink=\"http://example.com/\">").has_error("xmlns-attribute-mismatch"));
  // Each one is an error, and the element is still made with them.
  const Out r = run("<svg xmlns=\"a\" xmlns:xlink=\"b\">");
  size_t n = 0;
  for (const auto & e : r.error_names()) {
    n += e == "xmlns-attribute-mismatch";
  }
  EXPECT_EQ(n, 2u);
  EXPECT_EQ(r.dump, in_body({"<svg svg>", "  xmlns xlink=\"b\"", "  xmlns xmlns=\"a\""}));
  // HTML elements have no such attributes.
  EXPECT_FALSE(run("<p xmlns=\"a\" xmlns:xlink=\"b\">").has_error("xmlns-attribute-mismatch"));
}

TEST(HtmlTreeForeign, CdataSectionsAreTextInForeignContentAndBogusCommentsElsewhere) {
  Out r = run("<svg><![CDATA[x<y]]>z");
  EXPECT_EQ(r.dump, in_body({"<svg svg>", "  \"x<yz\""}));
  EXPECT_FALSE(r.has_error("cdata-in-html-content"));
  r = run("<p><![CDATA[x]]>");
  EXPECT_EQ(r.dump, in_body({"<p>", "  <!-- [CDATA[x]] -->"}));
  EXPECT_TRUE(r.has_error("cdata-in-html-content"));
  // The decision is the adjusted current node's namespace, not the insertion mode's rules: a MathML text
  // integration point and an SVG foreignObject are not in the HTML namespace.
  EXPECT_EQ(doc_of("<math><mi><![CDATA[x]]>"), in_body({"<math math>", "  <math mi>", "    \"x\""}));
  EXPECT_EQ(doc_of("<svg><foreignObject><![CDATA[x]]>"),
      in_body({"<svg svg>", "  <svg foreignObject>", "    \"x\""}));
  // An HTML element inside foreign content is the HTML namespace again.
  r = run("<svg><foreignObject><p><![CDATA[x]]>");
  EXPECT_TRUE(r.has_error("cdata-in-html-content"));
  // After the foreign element is closed, CDATA is a comment again; in a fragment it is the context element
  // that decides, while the stack holds only the root.
  r = run("<svg></svg><![CDATA[x]]>");
  EXPECT_TRUE(r.has_error("cdata-in-html-content"));
  EXPECT_EQ(frag_of("<![CDATA[x]]>y", svg_ctx("g")), tree({"\"xy\""}));
  EXPECT_EQ(frag_of("<![CDATA[x]]>y", html_ctx("div")), tree({"<!-- [CDATA[x]] -->", "\"y\""}));
  EXPECT_EQ(frag_of("<![CDATA[x]]>y", math_ctx("mi")), tree({"\"xy\""}));
}

TEST(HtmlTreeForeign, ANulIsAFffdAndOnlyOtherCharactersClearFramesetOk) {
  const std::string nul("<svg>\0x", 7);
  const Out r = run(nul);
  EXPECT_EQ(r.dump, in_body({"<svg svg>", "  \"\xEF\xBF\xBDx\""}));
  ASSERT_TRUE(r.has_error("unexpected-null-character"));
  // The error is at the NUL, not at the start of the run.
  for (const auto & e : r.errors) {
    if (std::string(gtext_html_tree_error_name(e.code)) == "unexpected-null-character") {
      EXPECT_EQ(e.line, 1u);
      EXPECT_EQ(e.column, 6u);
      EXPECT_EQ(e.offset, 5u);
    }
  }
  const char * frameset = "<frameset>";
  // White space and a NUL leave "frameset-ok" alone; any other character clears it.
  EXPECT_EQ(doc_of(std::string("<svg> \t\n</svg>") + frameset), tree({"<html>", "  <head>", "  <frameset>"}));
  EXPECT_EQ(doc_of(std::string("<svg>\0</svg>", 12) + frameset), tree({"<html>", "  <head>", "  <frameset>"}));
  EXPECT_EQ(doc_of(std::string("<svg>x</svg>") + frameset), in_body({"<svg svg>", "  \"x\""}));
}

TEST(HtmlTreeForeign, CommentsProcessingInstructionsAndDoctypesInForeignContent) {
  const Out r = run("<svg><!--c--><?pi x?><!DOCTYPE html><g>");
  EXPECT_EQ(r.dump, in_body({"<svg svg>", "  <!-- c -->", "  <?pi x?>", "  <svg g>"}));
  EXPECT_TRUE(r.has_error("unexpected-doctype"));
  // The end of the file is handled by the insertion mode: elements left open are an error.
  EXPECT_TRUE(r.has_error("open-elements-at-eof"));
  EXPECT_FALSE(run("<svg></svg>").has_error("open-elements-at-eof"));
}

TEST(HtmlTreeForeign, ForeignElementsAreInTheNamespacesOfTheirParents) {
  // "Insert a foreign element for the token, with the adjusted current node's namespace": an element
  // made inside svg is SVG whatever its name, and one made inside MathML is MathML, and inside an HTML
  // integration point it is HTML again.
  EXPECT_EQ(doc_of("<svg><mi><math><title>"), in_body({"<svg svg>", "  <svg mi>", "    <svg math>", "      <svg title>"}));
  EXPECT_EQ(doc_of("<math><svg><desc>"), in_body({"<math math>", "  <math svg>", "    <math desc>"}));
  EXPECT_EQ(doc_of("<svg><desc><math><mi><svg><title><b>"),
      in_body({"<svg svg>", "  <svg desc>", "    <math math>", "      <math mi>", "        <svg svg>", "          <svg title>",
          "            <b>"}));
}

// ---- The adjustment tables --------------------------------------------------

namespace {

std::string lower_of(std::string s) {
  for (char & c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

} // namespace

TEST(HtmlTreeForeign, ForeignNameTablesAreTheStandardsAndTheVocabularys) {
  // The sizes in the standard as served on 2026-10-12: 37 SVG tag names, 58 SVG attribute names, 11
  // namespaced attributes.
  EXPECT_EQ(gtext_html_tree_test_table_size(0), 37u);
  EXPECT_EQ(gtext_html_tree_test_table_size(1), 58u);
  EXPECT_EQ(gtext_html_tree_test_table_size(2), 11u);
  EXPECT_EQ(gtext_html_tree_test_table_size(3), 0u);
  const char * svg = GTEXT_MARKUP_NS_SVG;
  const char * math = GTEXT_MARKUP_NS_MATHML;
  std::string prev;
  for (int which = 0; which < 2; which++) {
    prev.clear();
    for (size_t i = 0; i < gtext_html_tree_test_table_size(which); i++) {
      const char * from = nullptr;
      const char * to = nullptr;
      ASSERT_TRUE(gtext_html_tree_test_table_at(which, i, &from, &to));
      SCOPED_TRACE(std::string(from) + " -> " + to);
      EXPECT_LT(prev, std::string(from)) << "the tables are searched in byte order";
      prev = from;
      // The entry is the lower-cased form of its target, and not the target (or it would be a no-op).
      EXPECT_EQ(lower_of(to), std::string(from));
      EXPECT_STRNE(from, to);
      // The vocabulary the writers use agrees: the target is read back as written, the folded form is not.
      const bool attribute = which == 1;
      EXPECT_TRUE(gtext_markup_html_foreign_name_survives(svg, attribute, to));
      EXPECT_FALSE(gtext_markup_html_foreign_name_survives(svg, attribute, from));
      // The parser applies it: in SVG only.
      const std::string name = from;
      if (which == 0) {
        EXPECT_EQ(doc_of("<svg><" + name + ">"), in_body({"<svg svg>", std::string("  <svg ") + to + ">"}));
        EXPECT_EQ(doc_of("<math><" + name + ">"), in_body({"<math math>", "  <math " + name + ">"}));
        EXPECT_EQ(doc_of("<svg><" + std::string(to) + ">"), in_body({"<svg svg>", std::string("  <svg ") + to + ">"}))
            << "the input's case does not matter, the tokenizer lower-cases it";
        // An HTML integration point makes HTML elements, whose names are not adjusted.
        EXPECT_EQ(doc_of("<svg><desc><" + name + ">"),
            in_body({"<svg svg>", "  <svg desc>", "    <" + name + ">"}));
      }
      else {
        EXPECT_EQ(doc_of("<svg " + name + "=1>"), in_body({"<svg svg>", std::string("  ") + to + "=\"1\""}));
        EXPECT_EQ(doc_of("<math " + name + "=1>"), in_body({"<math math>", "  " + name + "=\"1\""}));
        EXPECT_EQ(doc_of("<p " + name + "=1>"), in_body({"<p>", "  " + name + "=\"1\""}));
        // On a child element too (the adjusted current node's namespace is SVG).
        EXPECT_EQ(doc_of("<svg><g " + name + "=1>"),
            in_body({"<svg svg>", "  <svg g>", std::string("    ") + to + "=\"1\""}));
      }
    }
  }
  EXPECT_TRUE(gtext_markup_html_foreign_name_survives(svg, false, "feDropShadow"))
      << "the 37th: the vocabulary carried 36 until this stage compared it with the standard";
  // MathML's one attribute.
  EXPECT_EQ(doc_of("<math definitionurl=x>"), in_body({"<math math>", "  definitionURL=\"x\""}));
  EXPECT_EQ(doc_of("<math><mi definitionurl=x>"), in_body({"<math math>", "  <math mi>", "    definitionURL=\"x\""}));
  EXPECT_EQ(doc_of("<svg definitionurl=x>"), in_body({"<svg svg>", "  definitionurl=\"x\""}));
  EXPECT_EQ(doc_of("<p definitionurl=x>"), in_body({"<p>", "  definitionurl=\"x\""}));
  EXPECT_TRUE(gtext_markup_html_foreign_name_survives(math, true, "definitionURL"));
  // The namespaced attributes: sorted, and each one a name the standard lists.
  const char * const expected_foreign[] = {"xlink:actuate", "xlink:arcrole", "xlink:href", "xlink:role",
      "xlink:show", "xlink:title", "xlink:type", "xml:lang", "xml:space", "xmlns", "xmlns:xlink"};
  for (size_t i = 0; i < 11; i++) {
    const char * from = nullptr;
    const char * to = nullptr;
    ASSERT_TRUE(gtext_html_tree_test_table_at(2, i, &from, &to));
    EXPECT_STREQ(from, expected_foreign[i]);
    EXPECT_FALSE(gtext_html_tree_test_table_at(2, 11, &from, &to));
    const std::string prefix = std::string(to);
    EXPECT_TRUE(prefix == "xlink" || prefix == "xml" || prefix == "xmlns");
  }
  // A near miss is not adjusted.
  EXPECT_EQ(doc_of("<svg xlink:foo=1 xml:base=2 xmlns:x=3 XLINK:HREF=4>"),
      in_body({"<svg svg>", "  xlink href=\"4\"", "  xlink:foo=\"1\"", "  xml:base=\"2\"", "  xmlns:x=\"3\""}));
}

TEST(HtmlTreeForeign, TheNineElementsThatAreSpecialAndBoundariesAreTheVocabularysToo) {
  const char * svg = GTEXT_MARKUP_NS_SVG;
  const char * math = GTEXT_MARKUP_NS_MATHML;
  const char * const svg_special[] = {"foreignobject", "desc", "title"};
  const char * const math_special[] = {"mi", "mo", "mn", "ms", "mtext", "annotation-xml"};
  for (const char * n : svg_special) {
    const uint32_t f = ht_foreign_flags(NS_SVG, n, std::strlen(n));
    EXPECT_EQ(f, uint32_t(F_SPECIAL | F_SC_GEN | F_HIP)) << n;
    // The vocabulary says it with the name's case restored.
    const char * cased = std::strcmp(n, "foreignobject") == 0 ? "foreignObject" : n;
    EXPECT_TRUE(gtext_markup_html_is_special(svg, cased));
    EXPECT_FALSE(gtext_markup_html_is_special(math, cased));
  }
  for (const char * n : math_special) {
    const uint32_t f = ht_foreign_flags(NS_MATH, n, std::strlen(n));
    const bool annotation = std::strcmp(n, "annotation-xml") == 0;
    EXPECT_EQ(f, uint32_t(F_SPECIAL | F_SC_GEN | (annotation ? F_ANNOT : F_MTIP))) << n;
    EXPECT_TRUE(gtext_markup_html_is_special(math, n));
    EXPECT_FALSE(gtext_markup_html_is_special(svg, n));
  }
  // Nothing else is special or a boundary in foreign content: not even the elements HTML has under the
  // same names, and not the other namespace's.
  for (const char * n : {"p", "td", "table", "template", "html", "button", "g", "svg", "math", "mglyph", "annotation"}) {
    EXPECT_EQ(ht_foreign_flags(NS_SVG, n, std::strlen(n)), 0u) << n;
    EXPECT_EQ(ht_foreign_flags(NS_MATH, n, std::strlen(n)), 0u) << n;
  }
  EXPECT_EQ(ht_foreign_flags(NS_SVG, "mi", 2), 0u);
  EXPECT_EQ(ht_foreign_flags(NS_MATH, "desc", 4), 0u);
  EXPECT_EQ(ht_foreign_flags(NS_HTML, "mi", 2), 0u) << "the HTML table is not asked";
  // A foreign element named like an HTML scope boundary is not one: a button inside a td in svg.
  EXPECT_EQ(doc_of("<table><tr><td><svg><td><p>"),
      in_body({"<table>", "  <tbody>", "    <tr>", "      <td>", "        <svg svg>", "          <svg td>", "        <p>"}));
}

// ---- Integration points -------------------------------------------------------

TEST(HtmlTreeForeign, MathmlTextIntegrationPointsTakeHtmlExceptMglyphAndMalignmark) {
  for (const char * mi : {"mi", "mo", "mn", "ms", "mtext"}) {
    SCOPED_TRACE(mi);
    const std::string open = std::string("<math><") + mi + ">";
    const std::string head = std::string("  <math ") + mi + ">";
    EXPECT_EQ(doc_of(open + "<b>x</b>y"), in_body({"<math math>", head, "    <b>", "      \"x\"", "    \"y\""}));
    EXPECT_EQ(doc_of(open + "<mglyph>"), in_body({"<math math>", head, "    <math mglyph>"}));
    EXPECT_EQ(doc_of(open + "<malignmark>"), in_body({"<math math>", head, "    <math malignmark>"}));
    // Anything else that is not mglyph or malignmark is an HTML start tag here, and a non-HTML name is an
    // HTML element too.
    EXPECT_EQ(doc_of(open + "<mglyphx>"), in_body({"<math math>", head, "    <mglyphx>"}));
    EXPECT_EQ(doc_of(open + "<g>"), in_body({"<math math>", head, "    <g>"}));
    // An end tag is not one of the cases: the foreign rules close the integration point itself.
    EXPECT_EQ(doc_of(open + "x</" + mi + ">y"), in_body({"<math math>", head, "    \"x\"", "  \"y\""}));
    // An svg start tag is an HTML one: an SVG element.
    EXPECT_EQ(doc_of(open + "<svg>"), in_body({"<math math>", head, "    <svg svg>"}));
  }
  // The other MathML elements are not integration points.
  EXPECT_EQ(doc_of("<math><mrow><b>x"), in_body({"<math math>", "  <math mrow>", "<b>", "  \"x\""}));
}

TEST(HtmlTreeForeign, AnnotationXmlIsAnHtmlIntegrationPointOnlyWithTheRightEncoding) {
  for (const char * enc : {"text/html", "TEXT/HTML", "Text/Html", "application/xhtml+xml", "APPLICATION/XHTML+XML"}) {
    SCOPED_TRACE(enc);
    EXPECT_EQ(doc_of(std::string("<math><annotation-xml encoding=\"") + enc + "\"><b>x</b>y"),
        in_body({"<math math>", "  <math annotation-xml>", std::string("    encoding=\"") + enc + "\"", "    <b>",
            "      \"x\"", "    \"y\""}));
  }
  for (const char * enc : {"text/plain", "", "text/html ", " text/html", "text/htm", "application/xml", "xhtml"}) {
    SCOPED_TRACE(enc);
    // Not an integration point: <b> is a breakout tag.
    const Out r = run(std::string("<math><annotation-xml encoding=\"") + enc + "\"><b>x");
    EXPECT_EQ(r.dump, in_body({"<math math>", "  <math annotation-xml>", std::string("    encoding=\"") + enc + "\"", "<b>",
        "  \"x\""}));
    EXPECT_TRUE(r.has_error("html-in-foreign-content"));
  }
  // No attribute at all; and another attribute.
  EXPECT_EQ(doc_of("<math><annotation-xml><b>x"), in_body({"<math math>", "  <math annotation-xml>", "<b>", "  \"x\""}));
  EXPECT_EQ(doc_of("<math><annotation-xml xencoding=\"text/html\"><b>"),
      in_body({"<math math>", "  <math annotation-xml>", "    xencoding=\"text/html\"", "<b>"}));
  // The token's `encoding` decides, not a later one: attributes are removed by the tokenizer when duplicated,
  // the first stays.
  EXPECT_EQ(doc_of("<math><annotation-xml encoding=text/html encoding=x><b>"),
      in_body({"<math math>", "  <math annotation-xml>", "    encoding=\"text/html\"", "    <b>"}));
  // Only an annotation-xml in MathML: the SVG namespace has no such element, and the HTML one is plain.
  EXPECT_EQ(doc_of("<svg><annotation-xml encoding=\"text/html\"><b>"),
      in_body({"<svg svg>", "  <svg annotation-xml>", "    encoding=\"text/html\"", "<b>"}));
  // Without the encoding an annotation-xml still passes an svg start tag to the HTML rules.
  EXPECT_EQ(doc_of("<math><annotation-xml><svg><g>"),
      in_body({"<math math>", "  <math annotation-xml>", "    <svg svg>", "      <svg g>"}));
  // ... and anything else in it is foreign (a MathML element).
  EXPECT_EQ(doc_of("<math><annotation-xml><g>"), in_body({"<math math>", "  <math annotation-xml>", "    <math g>"}));
  // A character inside takes the HTML rules only with the encoding, and is text both ways.
  EXPECT_EQ(doc_of("<math><annotation-xml>x"), in_body({"<math math>", "  <math annotation-xml>", "    \"x\""}));
  const Out text = run("<math><annotation-xml encoding=text/html>\0<b>x</b>"s);
  EXPECT_EQ(text.dump, in_body({"<math math>", "  <math annotation-xml>", "    encoding=\"text/html\"", "    <b>", "      \"x\""}));
  EXPECT_TRUE(text.has_error("unexpected-null-character")) << "a NUL in an integration point is the HTML rules' (ignored)";
}

TEST(HtmlTreeForeign, SvgForeignObjectDescAndTitleAreHtmlIntegrationPoints) {
  for (const char * n : {"foreignObject", "desc", "title"}) {
    SCOPED_TRACE(n);
    const std::string open = std::string("<svg><") + n + ">";
    const std::string head = std::string("  <svg ") + n + ">";
    EXPECT_EQ(doc_of(open + "<div>y</div>z"), in_body({"<svg svg>", head, "    <div>", "      \"y\"", "    \"z\""}));
    // Every start tag: an HTML element of the name, svg and math included.
    EXPECT_EQ(doc_of(open + "<g>"), in_body({"<svg svg>", head, "    <g>"}));
    EXPECT_EQ(doc_of(open + "<svg><g>"), in_body({"<svg svg>", head, "    <svg svg>", "      <svg g>"}));
    EXPECT_EQ(doc_of(open + "<math><mi>"), in_body({"<svg svg>", head, "    <math math>", "      <math mi>"}));
    // The end tag is the foreign rules': it closes the element.
    EXPECT_EQ(doc_of(open + "x</" + n + ">y"), in_body({"<svg svg>", head, "    \"x\"", "  \"y\""}));
  }
  // `title` and `desc` of SVG are not the RCDATA `title` of HTML: the tokenizer does not change state, so a
  // tag inside is a tag; the HTML `title` inside them is the RCDATA one.
  EXPECT_EQ(doc_of("<svg><title><b>x</b></title>y"),
      in_body({"<svg svg>", "  <svg title>", "    <b>", "      \"x\"", "  \"y\""}));
  EXPECT_EQ(doc_of("<svg><title><title><b>x</b></title>y"),
      in_body({"<svg svg>", "  <svg title>", "    <title>", "      \"<b>x</b>\"", "    \"y\""}));
  EXPECT_EQ(doc_of("<svg><foreignObject><textarea><b>x</b></textarea>y"),
      in_body({"<svg svg>", "  <svg foreignObject>", "    <textarea>", "      \"<b>x</b>\"", "    \"y\""}));
}

TEST(HtmlTreeForeign, TheIntegrationPointsAreBoundariesOfScope) {
  // `</p>` inside a desc: there is no p in button scope (the desc is a boundary), so the end tag makes one.
  EXPECT_EQ(doc_of("<p>a<svg><desc></p>x"),
      in_body({"<p>", "  \"a\"", "  <svg svg>", "    <svg desc>", "      <p>", "      \"x\""}));
  // The same through the MathML text integration points and annotation-xml with an encoding.
  EXPECT_EQ(doc_of("<p>a<math><mi></p>x"),
      in_body({"<p>", "  \"a\"", "  <math math>", "    <math mi>", "      <p>", "      \"x\""}));
  EXPECT_EQ(doc_of("<p>a<math><annotation-xml encoding=text/html></p>x"),
      in_body({"<p>", "  \"a\"", "  <math math>", "    <math annotation-xml>", "      encoding=\"text/html\"", "      <p>",
          "      \"x\""}));
  // A foreign element that is not a boundary is not one: <svg><g></p> breaks out first (and closes the p).
  EXPECT_EQ(doc_of("<p>a<svg><g></p>x"), in_body({"<p>", "  \"a\"", "  <svg svg>", "    <svg g>", "\"x\""}));
  // A list item in a foreignObject is not closed by an li outside it (list item scope stops there).
  EXPECT_EQ(doc_of("<ul><li>a<svg><foreignObject><li>b"),
      in_body({"<ul>", "  <li>", "    \"a\"", "    <svg svg>", "      <svg foreignObject>", "        <li>", "          \"b\""}));
}

// ---- Breakout ----------------------------------------------------------------

namespace {

const char * const kBreakouts[] = {"b", "big", "blockquote", "body", "br", "center", "code", "dd", "div", "dl", "dt",
    "em", "embed", "h1", "h2", "h3", "h4", "h5", "h6", "head", "hr", "i", "img", "li", "listing", "menu", "meta",
    "nobr", "ol", "p", "pre", "ruby", "s", "small", "span", "strong", "strike", "sub", "sup", "table", "tt", "u",
    "ul", "var"};

} // namespace

TEST(HtmlTreeForeign, EveryStartTagOfTheBreakoutListEndsForeignContent) {
  EXPECT_EQ(sizeof(kBreakouts) / sizeof(kBreakouts[0]), 44u) << "the standard's list, with font apart: 45 names";
  for (const char * name : kBreakouts) {
    SCOPED_TRACE(name);
    for (const char * ns : {"svg", "math"}) {
      SCOPED_TRACE(ns);
      const std::string in = std::string("<") + ns + "><g><" + name + ">";
      const Out r = run(in);
      ASSERT_EQ(r.status, GTEXT_HTML_TREE_OK);
      EXPECT_EQ(r.invariants, "");
      EXPECT_TRUE(r.has_error("html-in-foreign-content"));
      // The foreign elements are closed and empty: nothing foreign is below the namespace's own element.
      const std::string svg_line = std::string("    <") + ns + " " + ns + ">\n";
      EXPECT_NE(r.dump.find("| " + svg_line), std::string::npos) << r.dump;
      EXPECT_NE(r.dump.find("|       <" + std::string(ns) + " g>\n"), std::string::npos) << r.dump;
      // `body` and `head` are ignored by the body rules; the rest is made at the level of the namespace's
      // element (the body's children), not below it.
      if (std::strcmp(name, "body") != 0 && std::strcmp(name, "head") != 0) {
        EXPECT_NE(r.dump.find(std::string("|     <") + name + ">\n"), std::string::npos) << r.dump;
      }
      // Foreign content did not continue: in the body, the element after it is its sibling.
      EXPECT_EQ(r.dump.find("|         "), std::string::npos) << r.dump;
    }
  }
  // These are not on the list; the foreign rules make an element of the namespace.
  for (const char * name : {"a", "button", "select", "form", "input", "textarea", "title", "style", "script", "td",
           "tr", "th", "section", "main", "nav", "image", "rb", "rt", "rp", "rtc", "svg", "math", "caption", "col",
           "frame", "frameset", "template", "details", "summary", "object", "applet", "iframe", "noscript", "mi",
           "annotation-xml", "foreignobject", "g", "font", "address", "article", "aside", "dialog", "dir", "fieldset",
           "figure", "footer", "header", "hgroup", "main", "marquee", "optgroup", "option", "plaintext", "search",
           "tbody", "tfoot", "thead", "xmp", "area", "base", "col", "param", "source", "track", "wbr", "keygen"}) {
    SCOPED_TRACE(name);
    const Out r = run(std::string("<svg><") + name + ">");
    ASSERT_EQ(r.status, GTEXT_HTML_TREE_OK);
    EXPECT_FALSE(r.has_error("html-in-foreign-content"));
    const std::string shown = std::strcmp(name, "foreignobject") == 0 ? "foreignObject" : name;
    EXPECT_NE(r.dump.find("|       <svg " + shown + ">\n"), std::string::npos) << r.dump;
  }
}

TEST(HtmlTreeForeign, BreakoutPopsToAnIntegrationPointOrHtmlAndReprocessesThere) {
  // Down to the first HTML element: the integration point stays open and takes the tag.
  EXPECT_EQ(doc_of("<svg><foreignObject><svg><g><g><p>x"),
      in_body({"<svg svg>", "  <svg foreignObject>", "    <svg svg>", "      <svg g>", "        <svg g>", "    <p>",
          "      \"x\""}));
  EXPECT_EQ(doc_of("<math><mi><math><g><h1>x"),
      in_body({"<math math>", "  <math mi>", "    <math math>", "      <math g>", "    <h1>", "      \"x\""}));
  // (An element made inside an integration point is HTML: `<g>` there is not an SVG element.)
  EXPECT_EQ(doc_of("<svg><foreignObject><g><g><p>x"),
      in_body({"<svg svg>", "  <svg foreignObject>", "    <g>", "      <g>", "        <p>", "          \"x\""}));
  // Down to HTML: an HTML element between two foreign ones.
  EXPECT_EQ(doc_of("<div><svg><g><div><svg><g><br>"),
      in_body({"<div>", "  <svg svg>", "    <svg g>", "  <div>", "    <svg svg>", "      <svg g>", "    <br>"}));
  // The reprocessing is by the insertion mode: in a table, a breakout tag is foster parented as usual.
  EXPECT_EQ(doc_of("<table><tr><td><svg><g><p>x"),
      in_body({"<table>", "  <tbody>", "    <tr>", "      <td>", "        <svg svg>", "          <svg g>", "        <p>",
          "          \"x\""}));
  // The breakout tag that makes a table.
  EXPECT_EQ(doc_of("<svg><g><table><tr><td>x"),
      in_body({"<svg svg>", "  <svg g>", "<table>", "  <tbody>", "    <tr>", "      <td>", "        \"x\""}));
  // Breakout in the fragment case: nothing to pop, the stack is just the root.
  EXPECT_EQ(frag_of("<p>x", svg_ctx("g")), tree({"<p>", "  \"x\""}));
  EXPECT_EQ(frag_of("<g><p>x", svg_ctx("g")), tree({"<svg g>", "<p>", "  \"x\""}));
}

TEST(HtmlTreeForeign, EndTagsBrAndPBreakOutToo) {
  Out r = run("<svg></p>");
  EXPECT_EQ(r.dump, in_body({"<svg svg>", "<p>"}));
  EXPECT_TRUE(r.has_error("html-in-foreign-content"));
  r = run("<svg><g></br>");
  EXPECT_EQ(r.dump, in_body({"<svg svg>", "  <svg g>", "<br>"}));
  EXPECT_TRUE(r.has_error("html-in-foreign-content"));
  // No other end tag does: `</div>` is a stray end tag of the foreign rules (and HTML's, which ignores it).
  r = run("<svg></div>");
  EXPECT_EQ(r.dump, in_body({"<svg svg>"}));
  EXPECT_FALSE(r.has_error("html-in-foreign-content"));
  // </br> in a MathML text integration point.
  EXPECT_EQ(doc_of("<math><mi></br>x"), in_body({"<math math>", "  <math mi>", "    <br>", "    \"x\""}));
}

// ---- The end tag loop ----------------------------------------------------------

TEST(HtmlTreeForeign, AnEndTagPopsUpToTheNearestOpenElementOfThatName) {
  Out r = run("<svg><g><path></g>x");
  EXPECT_EQ(r.dump, in_body({"<svg svg>", "  <svg g>", "    <svg path>", "  \"x\""}));
  EXPECT_TRUE(r.has_error("unexpected-end-tag")) << "the current node (path) is not named g";
  // Matching is on the lower-cased name of the node, against a token whose name is lower case already.
  r = run("<svg><clipPath><foreignObject></CLIPPATH>x");
  EXPECT_EQ(r.dump, in_body({"<svg svg>", "  <svg clipPath>", "    <svg foreignObject>", "  \"x\""}));
  EXPECT_EQ(doc_of("<svg><clipPath></CLIPPATH>x"), in_body({"<svg svg>", "  <svg clipPath>", "  \"x\""}));
  EXPECT_EQ(doc_of("<svg><foreignObject>a</foreignobject>x"), in_body({"<svg svg>", "  <svg foreignObject>", "    \"a\"", "  \"x\""}));
  // The nearest one, not the first.
  EXPECT_EQ(doc_of("<svg><g><g><g></g>x"),
      in_body({"<svg svg>", "  <svg g>", "    <svg g>", "      <svg g>", "      \"x\""}));
  // A name with no open element: the HTML rules get the token, and they ignore it.
  r = run("<svg><g></path>");
  EXPECT_EQ(r.dump, in_body({"<svg svg>", "  <svg g>"}));
  EXPECT_TRUE(r.has_error("unexpected-end-tag"));
  // It does not reach past an HTML element: the foreignObject below the div is out of its reach.
  r = run("<svg><foreignObject><div><svg><g></foreignObject>x");
  EXPECT_EQ(r.dump,
      in_body({"<svg svg>", "  <svg foreignObject>", "    <div>", "      <svg svg>", "        <svg g>", "          \"x\""}));
  // An HTML end tag for an HTML element of the same name finds it by the HTML rules.
  EXPECT_EQ(doc_of("<div><svg><g></div>x"), in_body({"<div>", "  <svg svg>", "    <svg g>", "\"x\""}));
  // It does reach past integration points: they are not HTML elements.
  EXPECT_EQ(doc_of("<svg><foreignObject><math><mrow></svg>x"),
      in_body({"<svg svg>", "  <svg foreignObject>", "    <math math>", "      <math mrow>", "\"x\""}));
  EXPECT_EQ(doc_of("<svg><foreignObject><math><mi><svg><g></g></svg>x"),
      in_body({"<svg svg>", "  <svg foreignObject>", "    <math math>", "      <math mi>", "        <svg svg>",
          "          <svg g>", "        \"x\""}));
  // A name open in both foreign namespaces: the nearest one is closed first, and then the other, across
  // the namespace boundary.
  EXPECT_EQ(doc_of("<math><g><annotation-xml encoding=text/html><svg><g></g></g>x"),
      in_body({"<math math>", "  <math g>", "    <math annotation-xml>", "      encoding=\"text/html\"", "      <svg svg>",
          "        <svg g>", "  \"x\""}));
  // The end of the file with elements open is an error of the insertion mode.
  EXPECT_TRUE(run("<svg><g>").has_error("open-elements-at-eof"));
}

TEST(HtmlTreeForeign, ScriptInSvgIsPoppedAtItsEndTagOrWhenSelfClosing) {
  // A breakout inside it ends it; the `</script>` is then an HTML end tag, which finds no script.
  EXPECT_EQ(doc_of("<svg><script>a<b>c</script>y"), in_body({"<svg svg>", "  <svg script>", "    \"a\"", "<b>", "  \"cy\""}));
  // The text of an SVG script is ordinary text in foreign content (the tokenizer does not change state).
  EXPECT_EQ(doc_of("<svg><script>x</script>y"), in_body({"<svg svg>", "  <svg script>", "    \"x\"", "  \"y\""}));
  EXPECT_EQ(doc_of("<svg><script/>y"), in_body({"<svg svg>", "  <svg script>", "  \"y\""}));
  EXPECT_EQ(doc_of("<svg><g/>y"), in_body({"<svg svg>", "  <svg g>", "  \"y\""}));
  EXPECT_EQ(doc_of("<svg><g>y"), in_body({"<svg svg>", "  <svg g>", "    \"y\""}));
  EXPECT_EQ(doc_of("<math><mi/>y"), in_body({"<math math>", "  <math mi>", "  \"y\""}));
  EXPECT_EQ(doc_of("<svg/>y"), in_body({"<svg svg>", "\"y\""}));
  EXPECT_EQ(doc_of("<math/>y"), in_body({"<math math>", "\"y\""}));
  // A script in MathML, or an HTML script in a foreignObject, is not the SVG case.
  EXPECT_EQ(doc_of("<math><script>x</script>y"), in_body({"<math math>", "  <math script>", "    \"x\"", "  \"y\""}));
  const Out r = run("<svg><foreignObject><script>if (a<b) x</script>y");
  EXPECT_EQ(r.dump, in_body({"<svg svg>", "  <svg foreignObject>", "    <script>", "      \"if (a<b) x\"", "    \"y\""}));
  // A self-closing flag on an HTML element is still an error in HTML, and not in foreign content.
  EXPECT_FALSE(run("<svg><g/>").has_error("non-void-html-element-start-tag-with-trailing-solidus"));
  EXPECT_TRUE(run("<div/>").has_error("non-void-html-element-start-tag-with-trailing-solidus"));
  EXPECT_TRUE(run("<svg><foreignObject><div/>").has_error("non-void-html-element-start-tag-with-trailing-solidus"));
}

// ---- Fragments ----------------------------------------------------------------

TEST(HtmlTreeFragment, TheContextDecidesTheInsertionModeAtTheStart) {
  struct Case {
    Ctx ctx;
    const char * mode;
  };
  const Case cases[] = {
      {html_ctx("div"), "in body"},      {html_ctx("td"), "in body"},        {html_ctx("th"), "in body"},
      {html_ctx("tr"), "in row"},        {html_ctx("tbody"), "in table body"}, {html_ctx("thead"), "in table body"},
      {html_ctx("tfoot"), "in table body"}, {html_ctx("caption"), "in caption"},
      {html_ctx("colgroup"), "in column group"}, {html_ctx("table"), "in table"},
      {html_ctx("template"), "in template"}, {html_ctx("head"), "in body"},
      {html_ctx("body"), "in body"},     {html_ctx("frameset"), "in frameset"}, {html_ctx("html"), "before head"},
      {html_ctx("select"), "in body"},   {html_ctx("title"), "in body"},     {html_ctx("script"), "in body"},
      {html_ctx("nonsense"), "in body"}, {html_ctx("TD"), "in body"},        {html_ctx("TABLE"), "in table"}, {html_ctx("Tr"), "in row"},
      {svg_ctx("td"), "in body"},        {svg_ctx("table"), "in body"},      {svg_ctx("template"), "in body"},
      {svg_ctx("svg"), "in body"},       {math_ctx("math"), "in body"},      {math_ctx("tr"), "in body"},
      {svg_ctx("frameset"), "in body"},  {math_ctx("html"), "in body"},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(std::string(c.ctx.ns == GTEXT_HTML_NS_SVG ? "svg " : c.ctx.ns == GTEXT_HTML_NS_MATHML ? "math " : "") + c.ctx.name);
    const Out r = run("", &c.ctx);
    EXPECT_EQ(r.status, GTEXT_HTML_TREE_OK);
    EXPECT_EQ(r.first_mode, c.mode);
    EXPECT_EQ(r.invariants, "");
    // The end of the file makes what "before head" and the modes after it make; every other context makes nothing.
    EXPECT_EQ(r.dump, c.ctx.ns == GTEXT_HTML_NS_HTML && c.ctx.name == "html" ? tree({"<head>", "<body>"}) : "");
  }
}

TEST(HtmlTreeFragment, TheContextDecidesTheTokenizersStateAndNothingInTheHtmlNamespaceElseDoes) {
  const std::string in = "<b>x</b>";
  const std::string as_text = tree({"\"<b>x</b>\""});
  const std::string as_markup = tree({"<b>", "  \"x\""});
  for (const char * n : {"title", "textarea", "style", "xmp", "iframe", "noembed", "noframes", "script", "plaintext"}) {
    SCOPED_TRACE(n);
    EXPECT_EQ(frag_of(in, html_ctx(n)), as_text);
    // The same names in another namespace are ordinary elements.
    EXPECT_EQ(frag_of(in, svg_ctx(n)), as_markup);
    EXPECT_EQ(frag_of(in, math_ctx(n)), as_markup);
  }
  for (const char * n : {"div", "td", "select", "template", "noscript", "body", "p", "option", "svg", "math"}) {
    SCOPED_TRACE(n);
    EXPECT_EQ(frag_of(in, html_ctx(n)), as_markup);
  }
  // noscript: RAWTEXT only with scripting.
  Ctx ns = html_ctx("noscript");
  EXPECT_EQ(frag_of(in, ns), as_markup);
  ns.scripting = true;
  EXPECT_EQ(frag_of(in, ns), as_text);
  // No end tag ends them: there is no appropriate end tag in the fragment case.
  EXPECT_EQ(frag_of("a</title><b>", html_ctx("title")), tree({"\"a</title><b>\""}));
  EXPECT_EQ(frag_of("a</script>b", html_ctx("script")), tree({"\"a</script>b\""}));
  EXPECT_EQ(frag_of("a</style>b", html_ctx("style")), tree({"\"a</style>b\""}));
  EXPECT_EQ(frag_of("a&amp;b<x>", html_ctx("textarea")), tree({"\"a&b<x>\""}));
  EXPECT_EQ(frag_of("a&amp;b<x>", html_ctx("style")), tree({"\"a&amp;b<x>\""}));
  EXPECT_EQ(frag_of("<!--x--><b>", html_ctx("plaintext")), tree({"\"<!--x--><b>\""}));
}

TEST(HtmlTreeFragment, TableContextsTakeTheirPartsAndMakeNoImpliedOnesAbove) {
  EXPECT_EQ(frag_of("<td>a<td>b", html_ctx("tr")), tree({"<td>", "  \"a\"", "<td>", "  \"b\""}));
  EXPECT_EQ(frag_of("<tr><td>a", html_ctx("tbody")), tree({"<tr>", "  <td>", "    \"a\""}));
  EXPECT_EQ(frag_of("<tr><td>a", html_ctx("table")), tree({"<tbody>", "  <tr>", "    <td>", "      \"a\""}));
  EXPECT_EQ(frag_of("<col><col>", html_ctx("colgroup")), tree({"<col>", "<col>"}));
  EXPECT_EQ(frag_of("<col>", html_ctx("table")), tree({"<colgroup>", "  <col>"}));
  EXPECT_EQ(frag_of("<td>a", html_ctx("td")), tree({"\"a\""}));
  EXPECT_EQ(frag_of("<tr>a", html_ctx("td")), tree({"\"a\""}));
  // A cell start tag in a caption is ignored: there is no caption in table scope (fragment case).
  EXPECT_EQ(frag_of("<b>a<td>c", html_ctx("caption")), tree({"<b>", "  \"ac\""}));
  // Text in a table is foster parented, which in the fragment case is the root (the first element on the stack).
  EXPECT_EQ(frag_of("x<tr><td>a", html_ctx("table")), tree({"\"x\"", "<tbody>", "  <tr>", "    <td>", "      \"a\""}));
  EXPECT_EQ(frag_of("<b>x", html_ctx("tr")), tree({"<b>", "  \"x\""}));
  // A table start tag in a table body context is ignored (no tbody, thead or tfoot is in table scope: fragment
  // case); the b is foster parented, which is the root; the row clears the stack back to the body context.
  EXPECT_EQ(frag_of("<table><b>x<tr><td>y", html_ctx("tbody")),
      tree({"<b>", "  \"x\"", "<tr>", "  <td>", "    \"y\""}));
}

TEST(HtmlTreeFragment, ATemplateContextTakesTemplateContentAndIsParsingTemplateContents) {
  EXPECT_EQ(frag_of("<tr><td>x", html_ctx("template")), tree({"<tr>", "  <td>", "    \"x\""}));
  EXPECT_EQ(frag_of("<td>x", html_ctx("template")), tree({"<td>", "  \"x\""}));
  EXPECT_EQ(frag_of("<col>", html_ctx("template")), tree({"<col>"}));
  EXPECT_EQ(frag_of("<caption>c", html_ctx("template")), tree({"<caption>", "  \"c\""}));
  EXPECT_EQ(frag_of("<b>x", html_ctx("template")), tree({"<b>", "  \"x\""}));
  // The end of the file with no template on the stack stops: no errors beyond the content's.
  const Ctx tmpl = html_ctx("template");
  const Out r = run("x", &tmpl);
  EXPECT_EQ(r.dump, tree({"\"x\""}));
  EXPECT_EQ(r.error_names(), std::vector<std::string>{});
  // A template end tag is stray (none is on the stack), a form in it is inserted and no pointer is set:
  // "parsing template contents" is true for a template context.
  EXPECT_EQ(frag_of("</template>x", html_ctx("template")), tree({"\"x\""}));
  EXPECT_EQ(frag_of("<form><form>", html_ctx("template")), tree({"<form>", "  <form>"}));
  EXPECT_EQ(frag_of("<form></form>x</form>", html_ctx("template")), tree({"<form>", "\"x\""}));
  // A template inside a non-template context is as usual.
  EXPECT_EQ(frag_of("<template><tr><td>x</template>y", html_ctx("div")),
      tree({"<template>", "  content", "    <tr>", "      <td>", "        \"x\"", "\"y\""}));
}

TEST(HtmlTreeFragment, AFormAncestorMakesAFormStartTagANestedFormAndAnEndTagStray) {
  Ctx plain = html_ctx("div");
  Ctx inform = html_ctx("div");
  inform.form = true;
  // With the pointer set, a form start tag is a nested form: ignored (outside a template).
  Out r = run("<form><p>x", &inform);
  EXPECT_EQ(r.dump, tree({"<p>", "  \"x\""}));
  EXPECT_TRUE(r.has_error("nested-form"));
  r = run("<form><p>x", &plain);
  EXPECT_EQ(r.dump, tree({"<form>", "  <p>", "    \"x\""}));
  EXPECT_FALSE(r.has_error("nested-form"));
  // `</form>` finds the pointer set to an element that is not on the stack: a stray end tag, and the pointer is null after.
  r = run("</form><form>y", &inform);
  EXPECT_EQ(r.dump, tree({"<form>", "  \"y\""}));
  EXPECT_TRUE(r.has_error("unexpected-end-tag"));
  // In a table context the form start tag has its own rule, with the same pointer.
  Ctx tform = html_ctx("table");
  tform.form = true;
  EXPECT_EQ(frag_of("<form>", tform), tree({}));
  EXPECT_EQ(frag_of("<form>", html_ctx("table")), tree({"<form>"}));
  // The context element itself being the form (form = true with name "form").
  Ctx f = html_ctx("form");
  f.form = true;
  EXPECT_EQ(frag_of("<form>a", f), tree({"\"a\""}));
}

TEST(HtmlTreeFragment, TheQuirksModeIsTheContextDocumentsAndDecidesTheTableStartTag) {
  Ctx quirks = html_ctx("div");
  quirks.quirks = GTEXT_MARKUP_MODE_QUIRKS;
  Ctx limited = html_ctx("div");
  limited.quirks = GTEXT_MARKUP_MODE_LIMITED_QUIRKS;
  // A `table` start tag closes a p in button scope unless the document is in quirks mode.
  EXPECT_EQ(frag_of("<p>a<table>", html_ctx("div")), tree({"<p>", "  \"a\"", "<table>"}));
  EXPECT_EQ(frag_of("<p>a<table>", limited), tree({"<p>", "  \"a\"", "<table>"}));
  EXPECT_EQ(frag_of("<p>a<table>", quirks), tree({"<p>", "  \"a\"", "  <table>"}));
}

TEST(HtmlTreeFragment, ASelectContextIgnoresSelectAndInputStartTags) {
  EXPECT_EQ(frag_of("<option>a<select>b", html_ctx("select")), tree({"<option>", "  \"ab\""}));
  EXPECT_EQ(frag_of("<input>a", html_ctx("select")), tree({"\"a\""}));
  // Not a select context: the usual rules (a select closes an open one; here none is).
  EXPECT_EQ(frag_of("<select>a<input>b", html_ctx("div")), tree({"<select>", "  \"a\"", "<input>", "\"b\""}));
  EXPECT_EQ(frag_of("<option>a<select>b", html_ctx("optgroup")), tree({"<option>", "  \"a\"", "  <select>", "    \"b\""}));
  const Ctx sel = html_ctx("select");
  const Out r = run("<select>", &sel);
  EXPECT_TRUE(r.has_error("unexpected-start-tag"));
}

TEST(HtmlTreeFragment, HtmlAndFramesetAndBodyContexts) {
  EXPECT_EQ(frag_of("<head><title>t</title></head><body>x", html_ctx("html")),
      tree({"<head>", "  <title>", "    \"t\"", "<body>", "  \"x\""}));
  EXPECT_EQ(frag_of("x", html_ctx("html")), tree({"<head>", "<body>", "  \"x\""}));
  // The end of the file: before head and in head and after head each make what is missing.
  EXPECT_EQ(frag_of("", html_ctx("html")), tree({"<head>", "<body>"}));
  EXPECT_EQ(frag_of("<frameset><frame>", html_ctx("html")), tree({"<head>", "<frameset>", "  <frame>"}));
  EXPECT_EQ(frag_of("<frame><frameset>a b", html_ctx("frameset")), tree({"<frame>", "<frameset>", "  \" \""}));
  // The end of a root frameset is ignored (the current node is the root): a parse error.
  const Ctx fs = html_ctx("frameset");
  const Out r = run("</frameset><frame>", &fs);
  EXPECT_EQ(r.dump, tree({"<frame>"}));
  EXPECT_TRUE(r.has_error("unexpected-end-tag"));
  // In a body context no body element is on the stack (the context is not), so `</html>` and `</body>` find
  // none in scope and are ignored by "in body"; "after body" is not reached.
  EXPECT_EQ(frag_of("a</html>b", html_ctx("body")), tree({"\"ab\""}));
  EXPECT_EQ(frag_of("a</body>b", html_ctx("body")), tree({"\"ab\""}));
  // "After body" is reached from an html context: `</body>` with a body open switches to it, `</html>` there is
  // "If the parser's fragment context element is non-null, this is a parse error; ignore the token. (fragment
  // case)", and a comment goes "as the last child of the first element in the stack of open elements" (the root,
  // so the fragment's last child).
  // (The `y` that follows is in body again, which is still open: "after body" hands anything else back to "in body".)
  EXPECT_EQ(frag_of("<body>x</body></html><!--c-->y", html_ctx("html")),
      tree({"<head>", "<body>", "  \"xy\"", "<!-- c -->"}));
  const Ctx html_c = html_ctx("html");
  const Out after = run("<body>x</body></html>", &html_c);
  EXPECT_TRUE(after.has_error("unexpected-end-tag"));
  EXPECT_EQ(frag_of("<body class=x><html class=y>z", html_ctx("body")), tree({"\"z\""}));
}

TEST(HtmlTreeFragment, ForeignContextsAreTheAdjustedCurrentNodeWhileTheStackHoldsOnlyTheRoot) {
  EXPECT_EQ(frag_of("<g><path/>x", svg_ctx("svg")), tree({"<svg g>", "  <svg path>", "  \"x\""}));
  EXPECT_EQ(frag_of("<clippath>", svg_ctx("svg")), tree({"<svg clipPath>"}));
  EXPECT_EQ(frag_of("<mi>", math_ctx("math")), tree({"<math mi>"}));
  EXPECT_EQ(frag_of("<clippath>", math_ctx("math")), tree({"<math clippath>"}));
  EXPECT_EQ(frag_of("x", svg_ctx("g")), tree({"\"x\""}));
  EXPECT_EQ(frag_of("x\0y"s, svg_ctx("g")), tree({"\"x\xEF\xBF\xBDy\""}));
  EXPECT_EQ(frag_of("<!--c-->", svg_ctx("g")), tree({"<!-- c -->"}));
  // The attributes are adjusted for the context's namespace.
  EXPECT_EQ(frag_of("<g viewbox=1 xlink:href=a>", svg_ctx("svg")),
      tree({"<svg g>", "  viewBox=\"1\"", "  xlink href=\"a\""}));
  EXPECT_EQ(frag_of("<g viewbox=1 definitionurl=2>", math_ctx("math")), tree({"<math g>", "  definitionURL=\"2\"", "  viewbox=\"1\""}));
  // The context's own name decides nothing beyond its integration point and namespace: the stray end tag
  // is a parse error, as the current node (the root) is not named so, and is ignored.
  const Ctx path = svg_ctx("path");
  Out r = run("</path>x", &path);
  EXPECT_EQ(r.dump, tree({"\"x\""}));
  EXPECT_EQ(r.error_names(), std::vector<std::string>{"unexpected-end-tag"});
  r = run("</html>x", &path);
  EXPECT_EQ(r.dump, tree({"\"x\""}));
  EXPECT_TRUE(r.error_names().empty()) << "the root is named html: no mismatch";
  // Once an element is open, the adjusted current node is the current node.
  EXPECT_EQ(frag_of("<g></g><p>", svg_ctx("svg")), tree({"<svg g>", "<p>"}));
  EXPECT_EQ(frag_of("<g></g>x", svg_ctx("svg")), tree({"<svg g>", "\"x\""}));
}

TEST(HtmlTreeFragment, IntegrationPointContextsTakeHtml) {
  for (const char * n : {"foreignObject", "desc", "title"}) {
    SCOPED_TRACE(n);
    EXPECT_EQ(frag_of("<div>x</div>y", svg_ctx(n)), tree({"<div>", "  \"x\"", "\"y\""}));
    EXPECT_EQ(frag_of("<g>", svg_ctx(n)), tree({"<g>"}));
    EXPECT_EQ(frag_of("</desc>y", svg_ctx(n)), tree({"\"y\""}));
  }
  // The exact case is the standard's: an SVG element named foreignobject is not the foreignObject.
  EXPECT_EQ(frag_of("<g>", svg_ctx("foreignobject")), tree({"<svg g>"}));
  EXPECT_EQ(frag_of("<g>", svg_ctx("foreignObject")), tree({"<g>"}));
  for (const char * n : {"mi", "mo", "mn", "ms", "mtext"}) {
    SCOPED_TRACE(n);
    EXPECT_EQ(frag_of("<div>x</div>y", math_ctx(n)), tree({"<div>", "  \"x\"", "\"y\""}));
    EXPECT_EQ(frag_of("<mglyph>", math_ctx(n)), tree({"<math mglyph>"}));
    EXPECT_EQ(frag_of("<malignmark>", math_ctx(n)), tree({"<math malignmark>"}));
    EXPECT_EQ(frag_of("x", math_ctx(n)), tree({"\"x\""}));
  }
  // annotation-xml: an integration point by the encoding of the context's start tag.
  Ctx a = math_ctx("annotation-xml");
  EXPECT_EQ(frag_of("<g>", a), tree({"<math g>"}));
  EXPECT_EQ(frag_of("<svg><g>", a), tree({"<svg svg>", "  <svg g>"})) << "an svg start tag takes the HTML rules";
  a.attrs = {{"encoding", "text/html"}};
  EXPECT_EQ(frag_of("<g>", a), tree({"<g>"}));
  a.attrs = {{"encoding", "Application/XHTML+xml"}};
  EXPECT_EQ(frag_of("<g>", a), tree({"<g>"}));
  a.attrs = {{"encoding", "text/plain"}};
  EXPECT_EQ(frag_of("<g>", a), tree({"<math g>"}));
  a.attrs = {{"x", "text/html"}};
  EXPECT_EQ(frag_of("<g>", a), tree({"<math g>"}));
  a.attrs = {{"x", "y"}, {"encoding", "text/html"}};
  EXPECT_EQ(frag_of("<g>", a), tree({"<g>"}));
  // Only a MathML annotation-xml has an encoding that matters.
  Ctx h = html_ctx("annotation-xml");
  h.attrs = {{"encoding", "text/html"}};
  EXPECT_EQ(frag_of("<g>", h), tree({"<g>"})) << "an HTML element takes HTML rules anyway";
  Ctx s2 = svg_ctx("annotation-xml");
  s2.attrs = {{"encoding", "text/html"}};
  EXPECT_EQ(frag_of("<g>", s2), tree({"<svg g>"}));
}

// ---- The fragment API -----------------------------------------------------------

TEST(HtmlTreeFragment, ArgumentsAreChecked) {
  GTEXT_HTML_FragmentContext c;
  std::memset(&c, 0, sizeof(c));
  GTEXT_HTML_TreeStatus st = GTEXT_HTML_TREE_OK;
  EXPECT_EQ(gtext_html_tree_new_fragment(nullptr, nullptr, &st), nullptr);
  EXPECT_EQ(st, GTEXT_HTML_TREE_E_INVALID);
  EXPECT_EQ(gtext_html_tree_new_fragment(nullptr, &c, &st), nullptr) << "no local name";
  EXPECT_EQ(st, GTEXT_HTML_TREE_E_INVALID);
  c.local_name = "div";
  EXPECT_EQ(gtext_html_tree_new_fragment(nullptr, &c, &st), nullptr) << "an empty local name";
  EXPECT_EQ(st, GTEXT_HTML_TREE_E_INVALID);
  c.local_name_length = 3;
  c.ns = static_cast<GTEXT_HTML_Namespace>(7);
  EXPECT_EQ(gtext_html_tree_new_fragment(nullptr, &c, &st), nullptr);
  EXPECT_EQ(st, GTEXT_HTML_TREE_E_INVALID);
  c.ns = GTEXT_HTML_NS_HTML;
  c.quirks = static_cast<GTEXT_Markup_Mode>(9);
  EXPECT_EQ(gtext_html_tree_new_fragment(nullptr, &c, &st), nullptr);
  EXPECT_EQ(st, GTEXT_HTML_TREE_E_INVALID);
  c.quirks = GTEXT_MARKUP_MODE_NO_QUIRKS;
  c.attribute_count = 1;
  EXPECT_EQ(gtext_html_tree_new_fragment(nullptr, &c, &st), nullptr) << "attributes counted and not there";
  EXPECT_EQ(st, GTEXT_HTML_TREE_E_INVALID);
  c.attribute_count = 0;
  // A status pointer is optional.
  GTEXT_HTML_Tree * t = gtext_html_tree_new_fragment(nullptr, &c, nullptr);
  ASSERT_NE(t, nullptr);
  // A fragment builder gives its document with the fragment, once.
  GTEXT_Markup_Document * doc = nullptr;
  EXPECT_EQ(gtext_html_tree_take_fragment(t, &doc), nullptr) << "not finished";
  EXPECT_EQ(gtext_html_tree_take_fragment(t, nullptr), nullptr);
  EXPECT_EQ(gtext_html_tree_feed(t, "x", 1), GTEXT_HTML_TREE_OK);
  EXPECT_EQ(gtext_html_tree_finish(t), GTEXT_HTML_TREE_OK);
  EXPECT_EQ(gtext_html_tree_take_document(t), nullptr);
  GTEXT_Markup_Node * frag = gtext_html_tree_take_fragment(t, &doc);
  ASSERT_NE(frag, nullptr);
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gtext_markup_node_kind(frag), GTEXT_MARKUP_KIND_FRAGMENT);
  EXPECT_EQ(gtext_markup_node_document(frag), doc);
  EXPECT_EQ(gtext_markup_node_parent(frag), nullptr);
  ASSERT_NE(gtext_markup_node_first_child(frag), nullptr);
  EXPECT_EQ(gtext_markup_node_kind(gtext_markup_node_first_child(frag)), GTEXT_MARKUP_KIND_TEXT);
  GTEXT_Markup_Document * again = nullptr;
  EXPECT_EQ(gtext_html_tree_take_fragment(t, &again), nullptr) << "taken once";
  EXPECT_EQ(again, nullptr);
  // The root element is in the document, empty.
  const GTEXT_Markup_Node * html = gtext_markup_node_first_child(gtext_markup_document_root(doc));
  ASSERT_NE(html, nullptr);
  EXPECT_STREQ(gtext_markup_element_local_name(html), "html");
  EXPECT_EQ(gtext_markup_node_first_child(html), nullptr);
  EXPECT_EQ(markup_check_invariants(doc), nullptr);
  gtext_markup_document_free(doc);
  gtext_html_tree_free(t);
  // A document builder has no fragment to give.
  GTEXT_HTML_Tree * d = gtext_html_tree_new(nullptr);
  ASSERT_NE(d, nullptr);
  gtext_html_tree_finish(d);
  EXPECT_EQ(gtext_html_tree_take_fragment(d, &doc), nullptr);
  GTEXT_Markup_Document * taken = gtext_html_tree_take_document(d);
  EXPECT_NE(taken, nullptr);
  gtext_markup_document_free(taken);
  gtext_html_tree_free(d);
  // Whole-buffer form.
  GTEXT_Markup_Document * pd = nullptr;
  GTEXT_Markup_Node * pf = nullptr;
  GTEXT_HTML_Tree * pt = nullptr;
  c.local_name = "td";
  c.local_name_length = 2;
  EXPECT_EQ(gtext_html_parse_fragment(&c, "<b>x", 4, nullptr, &pd, &pf, &pt), GTEXT_HTML_TREE_OK);
  ASSERT_NE(pf, nullptr);
  EXPECT_EQ(gtext_markup_element_local_name(gtext_markup_node_first_child(pf)), std::string("b"));
  gtext_markup_document_free(pd);
  gtext_html_tree_free(pt);
  EXPECT_EQ(gtext_html_parse_fragment(&c, nullptr, 3, nullptr, &pd, &pf, nullptr), GTEXT_HTML_TREE_E_INVALID);
  EXPECT_EQ(gtext_html_parse_fragment(&c, nullptr, 0, nullptr, nullptr, &pf, nullptr), GTEXT_HTML_TREE_E_INVALID);
  EXPECT_EQ(gtext_html_parse_fragment(nullptr, "x", 1, nullptr, &pd, &pf, nullptr), GTEXT_HTML_TREE_E_INVALID);
}

TEST(HtmlTreeFragment, TheContextIsDescribedByValueAndNeverKept) {
  // run() frees the name and the attribute array the moment new_fragment returns: under a sanitizer a
  // builder that kept a pointer into them fails here, in the two integration-point cases that read them.
  Ctx a = math_ctx("annotation-xml");
  a.attrs = {{"encoding", "text/html"}, {"other", "value"}};
  EXPECT_EQ(frag_of("<g>", a), tree({"<g>"}));
  EXPECT_EQ(frag_of("<g>", svg_ctx(std::string(300, 'x'))), tree({"<svg g>"})) << "a long name too";
  EXPECT_EQ(frag_of("<g>x", html_ctx(std::string(300, 'x'))), tree({"<g>", "  \"x\""}));
  // The context element is a name; nothing in the tree is made for it.
  const Ctx svg = svg_ctx("svg");
  const Out r = run("<g>", &svg);
  EXPECT_EQ(r.dump, tree({"<svg g>"}));
}

TEST(HtmlTreeFragment, TheResultIsTheRootsChildrenInOrderWhateverMovedThem) {
  // Foster parenting that returns the root, adoption agency repairs and reconstruction among them.
  EXPECT_EQ(frag_of("<b>1<p>2</b>3</p>4", html_ctx("div")),
      tree({"<b>", "  \"1\"", "<p>", "  <b>", "    \"2\"", "  \"3\"", "\"4\""}));
  // The second a ends the first (removed from the stack, out of scope behind the table) and is foster
  // parented before the table, into the first a.
  EXPECT_EQ(frag_of("<a><table><a>", html_ctx("div")), tree({"<a>", "  <a>", "  <table>"}));
  // A comment before anything and after everything.
  EXPECT_EQ(frag_of("<!--a-->x<!--b-->", html_ctx("div")), tree({"<!-- a -->", "\"x\"", "<!-- b -->"}));
  // A DOCTYPE is ignored.
  const Ctx div = html_ctx("div");
  const Out r = run("<!DOCTYPE html>x", &div);
  EXPECT_EQ(r.dump, tree({"\"x\""}));
  EXPECT_TRUE(r.has_error("unexpected-doctype"));
}

// ---- Cutting the input ------------------------------------------------------------

namespace {

std::string wpt_dir() {
  std::vector<std::string> candidates;
  if (const char * env = std::getenv("GTEXT_WPT_DIR")) {
    candidates.push_back(env);
  }
  const std::string here = __FILE__;
  const size_t slash = here.rfind('/');
  if (slash != std::string::npos) {
    candidates.push_back(here.substr(0, slash) + "/../third_party/html/wpt/html/syntax/parsing/resources");
  }
  candidates.push_back("third_party/html/wpt/html/syntax/parsing/resources");
  for (const std::string & c : candidates) {
    if (DIR * d = opendir(c.c_str())) {
      closedir(d);
      return c;
    }
  }
  return candidates.back();
}

struct Vector {
  std::string data;
  bool fragment = false;
  Ctx ctx;
};

// Every vector of the pinned tree-construction files that is a fragment vector, with its context.
std::vector<Vector> fragment_vectors() {
  std::vector<Vector> out;
  const std::string dir = wpt_dir();
  DIR * d = opendir(dir.c_str());
  EXPECT_NE(d, nullptr) << dir << " is not fetched: run tools/html/fetch.sh";
  if (!d) {
    return out;
  }
  std::vector<std::string> names;
  while (dirent * e = readdir(d)) {
    const std::string n = e->d_name;
    if (n.size() > 4 && n.substr(n.size() - 4) == ".dat" && n.rfind("scripted_", 0) != 0) {
      names.push_back(n);
    }
  }
  closedir(d);
  std::sort(names.begin(), names.end());
  for (const std::string & name : names) {
    std::ifstream in(dir + "/" + name, std::ios::binary);
    std::string line;
    std::string section;
    Vector cur;
    bool have = false;
    bool first_data = true;
    bool first_ctx = true;
    auto finish = [&]() {
      if (have && cur.fragment) {
        out.push_back(cur);
      }
    };
    while (std::getline(in, line)) {
      if (line == "#data") {
        finish();
        cur = Vector();
        have = true;
        first_data = true;
        first_ctx = true;
        section = "data";
      }
      else if (line == "#errors" || line == "#new-errors" || line == "#document" || line == "#script-on"
          || line == "#script-off") {
        section = line.substr(1);
      }
      else if (line == "#document-fragment") {
        section = "document-fragment";
        cur.fragment = true;
      }
      else if (section == "data") {
        cur.data += (first_data ? "" : "\n") + line;
        first_data = false;
      }
      else if (section == "document-fragment" && first_ctx && !line.empty()) {
        first_ctx = false;
        if (line.rfind("svg ", 0) == 0) {
          cur.ctx.ns = GTEXT_HTML_NS_SVG;
          cur.ctx.name = line.substr(4);
        }
        else if (line.rfind("math ", 0) == 0) {
          cur.ctx.ns = GTEXT_HTML_NS_MATHML;
          cur.ctx.name = line.substr(5);
        }
        else {
          cur.ctx.name = line;
        }
      }
    }
    finish();
  }
  return out;
}

} // namespace

TEST(HtmlTreeFragmentChunking, EveryTwoWaySplitAndAByteAtATimeGiveTheWholeInputsAnswer) {
  const std::vector<Vector> vectors = fragment_vectors();
  constexpr size_t kMin = 200; // 206 in the pinned tree
  ASSERT_GE(vectors.size(), kMin);
  size_t cut_runs = 0;
  size_t foreign = 0;
  for (int scripting = 0; scripting < 2; scripting++) {
    GTEXT_HTML_TreeOptions o = {};
    o.scripting = scripting != 0;
    for (const Vector & v : vectors) {
      Ctx ctx = v.ctx;
      ctx.scripting = scripting != 0;
      foreign += ctx.ns != GTEXT_HTML_NS_HTML;
      const Out whole = run(v.data, &ctx, o);
      ASSERT_EQ(whole.status, GTEXT_HTML_TREE_OK) << v.data;
      ASSERT_EQ(whole.invariants, "") << v.data;
      for (size_t k = 0; k <= v.data.size(); k++) {
        const Out split = run(v.data, &ctx, o, {k});
        cut_runs++;
        if (!whole.same_as(split)) {
          FAIL() << "cut at " << k << " changed the answer for " << ::testing::PrintToString(v.data) << " in "
                 << ctx.name;
        }
      }
      const Out bytes = run(v.data, &ctx, o, {}, true);
      if (!whole.same_as(bytes)) {
        FAIL() << "a byte at a time changed the answer for " << ::testing::PrintToString(v.data);
      }
    }
  }
  EXPECT_GT(cut_runs, 5000u);
  EXPECT_GT(foreign, 100u) << "the fragment vectors hold SVG and MathML contexts";
}

TEST(HtmlTreeFragmentChunking, ForeignHeavyInputsGiveTheSameAnswerCutEveryWayInDocumentsAndFragments) {
  const std::string inputs[] = {
      "<svg viewBox=1 xlink:href=a><clipPath/><foreignObject><div>x<p>y</foreignObject></svg>z",
      "<math><mi>x<mglyph><annotation-xml encoding=TEXT/HTML><p>a</annotation-xml><mo>y</math>",
      "<svg><![CDATA[a]]b]]]>c<![CDATA[d]]><title><b>e</b></title></svg><![CDATA[f]]>",
      "<table><tr><td><svg><g><td><p>x<tr>y</table><svg><desc><table><tr><td>q",
      "<b><svg><g></b><p>x</svg><i>a</i>",
      "<svg>a\0b<g/>c</svg>"s, "<math><annotation-xml><svg><g></g></annotation-xml><b>x",
      "<svg><script>a<b>c</script>y</svg><svg><script/>z",
      "<select><svg><option></svg><math><mi><select>", "<svg><font color=r><font>x",
  };
  for (const std::string & in : inputs) {
    SCOPED_TRACE(::testing::PrintToString(in));
    const Ctx ctxs[] = {html_ctx("div"), svg_ctx("svg"), svg_ctx("foreignObject"), math_ctx("annotation-xml"),
        math_ctx("mi"), html_ctx("table"), html_ctx("template")};
    for (int which = -1; which < 7; which++) {
      const Ctx * ctx = which < 0 ? nullptr : &ctxs[which];
      const Out whole = run(in, ctx);
      ASSERT_EQ(whole.status, GTEXT_HTML_TREE_OK);
      ASSERT_EQ(whole.invariants, "");
      for (size_t a = 0; a <= in.size(); a++) {
        for (size_t b = a; b <= in.size(); b++) {
          const Out cut = run(in, ctx, {}, {a, b - a});
          ASSERT_TRUE(whole.same_as(cut)) << "cuts " << a << ", " << b << " context " << which;
        }
      }
      ASSERT_TRUE(whole.same_as(run(in, ctx, {}, {}, true)));
    }
  }
}

// ---- Out of memory -------------------------------------------------------------------

TEST(HtmlTreeForeignOom, AnAllocationFailureAtEveryPointIsOomWithNoTreeAndNoLeak) {
  struct Case {
    std::string input;
    const Ctx * ctx;
  };
  const Ctx td = html_ctx("td");
  const Ctx svg = svg_ctx("svg");
  const Ctx annotation = [] {
    Ctx c = math_ctx("annotation-xml");
    c.attrs = {{"encoding", "text/html"}};
    c.quirks = GTEXT_MARKUP_MODE_QUIRKS;
    c.form = true;
    return c;
  }();
  const Ctx tmpl = html_ctx("template");
  const Case cases[] = {
      {"<!DOCTYPE html><svg viewBox=\"0 0 1 1\" xlink:href=a xml:lang=b xmlns=\"x\" xmlns:xlink=y><clipPath/>"
       "<foreignObject><div>x<p>y</foreignObject><title>t</title><g><![CDATA[c]]></g></svg><math definitionurl=d>"
       "<mi>x<mglyph/><annotation-xml encoding=TEXT/HTML><p>a</annotation-xml>\xEF\xBF\xBD<mo>y</math>",
          nullptr},
      {"<table><tr><td><svg><g><td><p>x<tr>y</table><b><svg><g></b><svg><desc><table><tr><td>q", nullptr},
      {"<svg a=1><g b=2><path c=3/>x\0y</g><p>z<svg></g></svg>"s, nullptr},
      {"<b>1<g viewbox=1 xlink:href=a><path/>x</g><p>2<form>3<form>", &svg},
      {"<td>a<tr>b<td><svg><mi>c<b>d", &td},
      {"<form><b>1<p>2</b>3<svg><desc><form>x", &annotation},
      {"<tr><td>x<template><tr><td>y</template>z", &tmpl},
  };
  for (const Case & c : cases) {
    for (const std::vector<size_t> & cuts : {std::vector<size_t>{}, std::vector<size_t>{9}}) {
      GTEXT_HTML_TreeOptions plain = {};
      const Out expected = run(c.input, c.ctx, plain, cuts);
      ASSERT_EQ(expected.status, GTEXT_HTML_TREE_OK) << c.input;
      size_t failures = 0;
      for (size_t n = 1;; n++) {
        FaultAllocator fa;
        GTEXT_HTML_TreeOptions o = {};
        o.allocator = &fa.alloc;
        fa.arm(n);
        const Out r = run(c.input, c.ctx, o, cuts);
        EXPECT_EQ(fa.bad_frees, 0u);
        EXPECT_TRUE(fa.live.empty()) << "leak after failing allocation " << n << " in " << c.input;
        if (!fa.failed) {
          EXPECT_TRUE(expected.same_as(r)) << "allocation " << n;
          break;
        }
        failures++;
        EXPECT_EQ(r.status, GTEXT_HTML_TREE_E_OOM) << "allocation " << n;
        EXPECT_FALSE(r.has_tree);
        ASSERT_LT(n, 100000u);
      }
      EXPECT_GT(failures, 10u);
    }
  }
}

TEST(HtmlTreeForeignOom, NewFragmentReturnsNullWithOomWhenTheAllocatorFailsAnywhereInSetup) {
  const Ctx contexts[] = {html_ctx("template"), svg_ctx("foreignObject"), html_ctx(std::string(100, 'x')),
      svg_ctx(std::string(100, 'X'))};
  for (const Ctx & ctx : contexts) {
    size_t failures = 0;
    for (size_t n = 1; n < 100; n++) {
      FaultAllocator fa;
      GTEXT_HTML_TreeOptions o = {};
      o.allocator = &fa.alloc;
      GTEXT_HTML_FragmentContext fc;
      std::memset(&fc, 0, sizeof(fc));
      fc.ns = ctx.ns;
      fc.local_name = ctx.name.c_str();
      fc.local_name_length = ctx.name.size();
      fa.arm(n);
      GTEXT_HTML_TreeStatus st = GTEXT_HTML_TREE_OK;
      GTEXT_HTML_Tree * tree = gtext_html_tree_new_fragment(&o, &fc, &st);
      if (!fa.failed) {
        EXPECT_NE(tree, nullptr);
        gtext_html_tree_free(tree);
        EXPECT_TRUE(fa.live.empty());
        break;
      }
      failures++;
      EXPECT_EQ(tree, nullptr) << n;
      EXPECT_EQ(st, GTEXT_HTML_TREE_E_OOM) << n;
      EXPECT_TRUE(fa.live.empty()) << n;
      EXPECT_EQ(fa.bad_frees, 0u);
    }
    EXPECT_GT(failures, 5u);
  }
}

// ---- Limits ---------------------------------------------------------------------------

TEST(HtmlTreeForeignLimits, DeepForeignNestingIsTheDepthLimitWithNoTree) {
  for (const char * open : {"<svg>", "<math>", "<svg><g>", "<svg><foreignObject><svg>"}) {
    SCOPED_TRACE(open);
    const Out r = run(repeat(open, 600));
    EXPECT_EQ(r.status, GTEXT_HTML_TREE_E_LIMIT_DEPTH);
    EXPECT_FALSE(r.has_tree);
    // Sticky: further input says the same.
    GTEXT_HTML_Tree * t = gtext_html_tree_new(nullptr);
    const std::string in = repeat(open, 600);
    EXPECT_EQ(gtext_html_tree_feed(t, in.data(), in.size()), GTEXT_HTML_TREE_E_LIMIT_DEPTH);
    EXPECT_EQ(gtext_html_tree_feed(t, "<p>", 3), GTEXT_HTML_TREE_E_LIMIT_DEPTH);
    EXPECT_EQ(gtext_html_tree_finish(t), GTEXT_HTML_TREE_E_LIMIT_DEPTH);
    EXPECT_EQ(gtext_html_tree_take_document(t), nullptr);
    gtext_html_tree_free(t);
  }
  // 510 fit: html and body count.
  EXPECT_EQ(run(repeat("<svg>", 510)).status, GTEXT_HTML_TREE_OK);
  EXPECT_EQ(run(repeat("<svg>", 511)).status, GTEXT_HTML_TREE_E_LIMIT_DEPTH);
  // The same in a fragment: the root counts, the context does not.
  const Ctx svg = svg_ctx("svg");
  EXPECT_EQ(run(repeat("<g>", 511), &svg).status, GTEXT_HTML_TREE_OK);
  EXPECT_EQ(run(repeat("<g>", 512), &svg).status, GTEXT_HTML_TREE_E_LIMIT_DEPTH);
  // The node limit, distinctly.
  GTEXT_HTML_TreeOptions o = {};
  o.max_nodes = 50;
  EXPECT_EQ(run(repeat("<svg/>", 100), nullptr, o).status, GTEXT_HTML_TREE_E_LIMIT_NODES);
  EXPECT_EQ(run(repeat("<g/>", 100), &svg, o).status, GTEXT_HTML_TREE_E_LIMIT_NODES);
  // A limit on the open elements, and raised: foreign content is 100,000 deep when asked.
  o = {};
  o.max_open_elements = 100000;
  o.max_nodes = 10000000;
  EXPECT_EQ(run(repeat("<svg>", 5000), nullptr, o).status, GTEXT_HTML_TREE_OK);
}

// ---- Work: the inputs that make a naive implementation quadratic -------------------------

namespace {

// The tree builder's work for one parse fed whole and for the same fed a byte at a time (which must agree).
size_t work_of(const std::string & input, const Ctx * ctx, size_t max_open = 0) {
  size_t whole = 0;
  for (int byte_at_a_time = 0; byte_at_a_time < 2; byte_at_a_time++) {
    GTEXT_HTML_TreeOptions o = {};
    o.max_open_elements = max_open ? max_open : 10000000;
    o.max_nodes = 100000000;
    GTEXT_HTML_Tree * tree = nullptr;
    if (ctx) {
      GTEXT_HTML_FragmentContext fc;
      std::memset(&fc, 0, sizeof(fc));
      fc.ns = ctx->ns;
      fc.local_name = ctx->name.c_str();
      fc.local_name_length = ctx->name.size();
      fc.quirks = ctx->quirks;
      tree = gtext_html_tree_new_fragment(&o, &fc, nullptr);
    }
    else {
      tree = gtext_html_tree_new(&o);
    }
    EXPECT_NE(tree, nullptr);
    GTEXT_HTML_TreeStatus s = GTEXT_HTML_TREE_OK;
    if (byte_at_a_time) {
      for (size_t i = 0; i < input.size() && s == GTEXT_HTML_TREE_OK; i++) {
        s = gtext_html_tree_feed(tree, input.data() + i, 1);
      }
    }
    else {
      s = gtext_html_tree_feed(tree, input.data(), input.size());
    }
    if (s == GTEXT_HTML_TREE_OK) {
      s = gtext_html_tree_finish(tree);
    }
    EXPECT_EQ(s, GTEXT_HTML_TREE_OK);
    const size_t w = gtext_html_tree_work(tree);
    if (byte_at_a_time) {
      EXPECT_EQ(w, whole) << "the tree builder's work depends on how the input was cut";
    }
    whole = w;
    GTEXT_Markup_Document * doc = nullptr;
    if (ctx) {
      gtext_html_tree_take_fragment(tree, &doc);
    }
    else {
      doc = gtext_html_tree_take_document(tree);
    }
    if (doc) {
      const char * bad = markup_check_invariants(doc);
      EXPECT_EQ(std::string(bad ? bad : ""), "");
      gtext_markup_document_free(doc);
    }
    gtext_html_tree_free(tree);
  }
  return whole;
}

void expect_linear(const char * label, const std::string & unit, size_t n, const std::string & prefix = "",
    const std::string & suffix = "", const Ctx * ctx = nullptr, double limit = 6.0) {
  SCOPED_TRACE(label);
  auto make = [&](size_t k) { return prefix + repeat(unit, k) + suffix; };
  const size_t a = work_of(make(n), ctx);
  const size_t b = work_of(make(4 * n), ctx);
  ASSERT_GT(a, 0u);
  EXPECT_LT((double)b / (double)a, limit) << "work " << a << " -> " << b;
  EXPECT_LE(b, GTEXT_HTML_TREE_WORK_BOUND(make(4 * n).size())) << "work is bounded by a multiple of the input";
}

} // namespace

TEST(HtmlTreeForeignWork, DeepForeignNestingIsLinear) {
  expect_linear("svg", "<svg>", 2000);
  expect_linear("g", "<g>", 2000, "<svg>");
  expect_linear("math mrow", "<mrow>", 2000, "<math>");
  expect_linear("nested svg in annotation-xml", "<svg><foreignObject>", 1000);
  expect_linear("deep then text", "<g>x", 2000, "<svg>");
  expect_linear("deep then stray end tags", "</path>", 4000, "<svg>" + repeat("<g>", 3000));
  expect_linear("deep then ends of the open names", "</g></nothere>", 2000, "<svg>" + repeat("<g>", 2000));
  expect_linear("deep self-closing", "<g/>", 4000, "<svg>" + repeat("<g>", 3000));
  expect_linear("attributes adjusted", "<g viewbox=1 xlink:href=a attributename=b>", 2000, "<svg>");
  const Ctx svg_c = svg_ctx("svg");
  expect_linear("svg in a fragment", "<g>", 2000, "", "", &svg_c);
}

TEST(HtmlTreeForeignWork, LongRunsOfBreakoutTagsPoppingForeignElementsAreLinear) {
  // Each breakout pops what the previous tag left: the pops are the pushes, paid once.
  expect_linear("svg g p", "<svg><g><g><p>", 1500);
  expect_linear("deep then breakouts", "<p>", 3000, "<svg>" + repeat("<g>", 3000));
  expect_linear("deep, a breakout, deep again", "<g><g><g><g><g><b>", 1000, "<svg>");
  expect_linear("math", "<math><mrow><mrow><div>", 1500);
  expect_linear("end tags p and br", "<svg><g></p><svg><g></br>", 1000);
  expect_linear("breakout in a table", "<table><tr><td><svg><g><p>", 600);
  const Ctx g_c = svg_ctx("g");
  expect_linear("breakout in fragment context", "<svg><g><p>", 1500, "", "", &g_c);
  expect_linear("fonts", "<svg><font color=x><svg><font>", 1000);
}

TEST(HtmlTreeForeignWork, ManyIntegrationPointSwitchesAreLinear) {
  expect_linear("foreignObject and div", "<svg><foreignObject><div>", 800);
  expect_linear("foreignObject text svg", "<svg><foreignObject>x<svg>", 800);
  expect_linear("desc title", "<svg><desc><p>a</p><title><b>", 800);
  expect_linear("mi", "<math><mi><b>x</b><mo>y", 800);
  expect_linear("annotation-xml", "<math><annotation-xml encoding=text/html><div>", 800);
  expect_linear("annotation-xml without encoding", "<math><annotation-xml><svg><g>", 800);
  expect_linear("switches and ends", "<svg><foreignObject><math><mi><svg><g></g></svg></mi></math></foreignObject></svg>", 400);
  expect_linear("many foreignObjects in a row", "<svg><foreignObject>a</foreignObject>b</svg>", 2000);
  expect_linear("p closing across a boundary", "<p><svg><desc></p>", 1000);
  expect_linear("li across a boundary", "<li><svg><foreignObject><li>", 800);
}

TEST(HtmlTreeFragmentWork, LargeFragmentInputsAreLinear) {
  const Ctx td = html_ctx("td");
  const Ctx tbody = html_ctx("tbody");
  const Ctx tmpl = html_ctx("template");
  const Ctx svg = svg_ctx("svg");
  const Ctx fo = svg_ctx("foreignObject");
  const Ctx math = math_ctx("mi");
  expect_linear("div in td", "<div>x", 3000, "", "", &td);
  expect_linear("tr td in tbody", "<tr><td>x", 3000, "", "", &tbody);
  expect_linear("misnested b i in td", "<b><i>x</b></i>", 1500, "", "", &td);
  expect_linear("text in tbody foster parented", "x<b>y", 3000, "", "", &tbody);
  expect_linear("templates", "<template><tr><td>x</template>", 1500, "", "", &tmpl);
  expect_linear("svg g", "<g>x</g>", 3000, "", "", &svg);
  expect_linear("foreignObject div", "<div>x<p>", 2000, "", "", &fo);
  expect_linear("mi", "<b>x</b><mglyph/>", 2000, "", "", &math);
  expect_linear("nested forms", "<form><p>x", 3000, "", "", &td);
  const Ctx select = html_ctx("select");
  expect_linear("select", "<option>x<select>y", 2000, "", "", &select);
}

TEST(HtmlTreeForeignWork, ForeignElementsWithManyDistinctNamesAreLinear) {
  // Every name is a slot of its own: the hash table of names grows, and a name is found by one probe.
  std::string names;
  for (size_t i = 0; i < 4000; i++) {
    names += "<e" + std::to_string(i) + "/>";
  }
  std::string names4;
  for (size_t i = 0; i < 16000; i++) {
    names4 += "<e" + std::to_string(i) + "/>";
  }
  const size_t a = work_of("<svg>" + names, nullptr);
  const size_t b = work_of("<svg>" + names4, nullptr);
  EXPECT_LT((double)b / (double)a, 6.0);
  const size_t c = work_of("<svg>" + names, nullptr);
  EXPECT_EQ(a, c);
}

// ---- Review items: context names, the fragment entry points, the scripting flag, ordering -------------

TEST(HtmlTreeFragment, AnHtmlContextNameIsFoldedToLowerCaseAndAForeignOneIsExact) {
  // HTML names are lower case (the parser makes them so), so `SELECT` is the select the parser would have made of it.
  EXPECT_EQ(frag_of("<input>a", html_ctx("SELECT")), tree({"\"a\""}));
  EXPECT_EQ(frag_of("<tr><td>x", html_ctx("TEMPLATE")), tree({"<tr>", "  <td>", "    \"x\""}));
  const Ctx tbody_upper = html_ctx("Tbody");
  EXPECT_EQ(run("", &tbody_upper).first_mode, "in table body");
  // An SVG or MathML name is matched exactly everywhere: `foreignobject` is an unknown SVG element, neither an
  // integration point nor the HTML-named thing it spells, and so is `FOREIGNOBJECT`.
  EXPECT_EQ(frag_of("<g>", svg_ctx("foreignobject")), tree({"<svg g>"}));
  EXPECT_EQ(frag_of("<g>", svg_ctx("FOREIGNOBJECT")), tree({"<svg g>"}));
  EXPECT_EQ(frag_of("<g>", svg_ctx("foreignObject")), tree({"<g>"}));
  EXPECT_EQ(frag_of("<g>", math_ctx("MI")), tree({"<math g>"}));
}

TEST(HtmlTreeFragment, AttributePointersAreCheckedWhenTheirLengthsAreNotZero) {
  GTEXT_HTML_FragmentContext c;
  std::memset(&c, 0, sizeof(c));
  c.local_name = "annotation-xml";
  c.local_name_length = 14;
  c.ns = GTEXT_HTML_NS_MATHML;
  GTEXT_HTML_Attribute a;
  std::memset(&a, 0, sizeof(a));
  c.attributes = &a;
  c.attribute_count = 1;
  GTEXT_HTML_TreeStatus st = GTEXT_HTML_TREE_OK;
  a.name_length = 8; // a name that is not there
  EXPECT_EQ(gtext_html_tree_new_fragment(nullptr, &c, &st), nullptr);
  EXPECT_EQ(st, GTEXT_HTML_TREE_E_INVALID);
  a.name = "encoding";
  a.value_length = 4; // a value that is not there
  EXPECT_EQ(gtext_html_tree_new_fragment(nullptr, &c, &st), nullptr);
  EXPECT_EQ(st, GTEXT_HTML_TREE_E_INVALID);
  a.value = "text";
  a.value_length = 0; // an empty value needs no pointer, and an empty name is fine too (a different attribute)
  GTEXT_HTML_Tree * t = gtext_html_tree_new_fragment(nullptr, &c, &st);
  EXPECT_NE(t, nullptr);
  gtext_html_tree_free(t);
  a.name = nullptr;
  a.name_length = 0;
  a.value = nullptr;
  t = gtext_html_tree_new_fragment(nullptr, &c, &st);
  EXPECT_NE(t, nullptr);
  gtext_html_tree_free(t);
}

TEST(HtmlTreeFragment, ParseFragmentSetsItsOutputsAtEntryAndLeavesThemNullOnFailure) {
  GTEXT_HTML_FragmentContext c;
  std::memset(&c, 0, sizeof(c));
  c.local_name = "div";
  c.local_name_length = 3;
  auto garbage_doc = [] { return reinterpret_cast<GTEXT_Markup_Document *>(static_cast<uintptr_t>(8)); };
  auto garbage_node = [] { return reinterpret_cast<GTEXT_Markup_Node *>(static_cast<uintptr_t>(8)); };
  // Success with no builder asked for: the builder is freed inside, and the fragment and its document are the caller's.
  GTEXT_Markup_Document * doc = garbage_doc();
  GTEXT_Markup_Node * frag = garbage_node();
  ASSERT_EQ(gtext_html_parse_fragment(&c, "<b>x</b>y", 9, nullptr, &doc, &frag, nullptr), GTEXT_HTML_TREE_OK);
  ASSERT_NE(frag, nullptr);
  size_t n = 0;
  char * d = htd_dump_children(frag, &n);
  EXPECT_EQ(std::string(d, n), tree({"<b>", "  \"x\"", "\"y\""}));
  std::free(d);
  EXPECT_EQ(markup_check_invariants(doc), nullptr);
  gtext_markup_document_free(doc);
  // A failing parse (the depth limit): the outputs are null, not what the caller had, and the builder is the caller's to free.
  GTEXT_HTML_TreeOptions o = {};
  o.max_open_elements = 3;
  doc = garbage_doc();
  frag = garbage_node();
  GTEXT_HTML_Tree * tree_out = nullptr;
  EXPECT_EQ(gtext_html_parse_fragment(&c, "<div><div><div><div>", 20, &o, &doc, &frag, &tree_out),
      GTEXT_HTML_TREE_E_LIMIT_DEPTH);
  EXPECT_EQ(doc, nullptr);
  EXPECT_EQ(frag, nullptr);
  ASSERT_NE(tree_out, nullptr);
  EXPECT_EQ(gtext_html_tree_status(tree_out), GTEXT_HTML_TREE_E_LIMIT_DEPTH);
  gtext_html_tree_free(tree_out);
  // The same with no builder asked for: nothing leaks (a sanitizer checks), the outputs are null.
  doc = garbage_doc();
  frag = garbage_node();
  EXPECT_EQ(gtext_html_parse_fragment(&c, "<div><div><div><div>", 20, &o, &doc, &frag, nullptr), GTEXT_HTML_TREE_E_LIMIT_DEPTH);
  EXPECT_EQ(doc, nullptr);
  EXPECT_EQ(frag, nullptr);
  // A bad request: invalid, and the outputs it can reach are null.
  doc = garbage_doc();
  frag = garbage_node();
  EXPECT_EQ(gtext_html_parse_fragment(nullptr, "x", 1, nullptr, &doc, &frag, nullptr), GTEXT_HTML_TREE_E_INVALID);
  EXPECT_EQ(doc, nullptr);
  EXPECT_EQ(frag, nullptr);
  // A failing allocator at every point: OOM, null outputs, nothing left over.
  for (size_t k = 1; k < 200; k++) {
    FaultAllocator fa;
    GTEXT_HTML_TreeOptions fo = {};
    fo.allocator = &fa.alloc;
    doc = garbage_doc();
    frag = garbage_node();
    fa.arm(k);
    const GTEXT_HTML_TreeStatus s = gtext_html_parse_fragment(&c, "<b>x</b><svg><g>y", 17, &fo, &doc, &frag, nullptr);
    if (!fa.failed) {
      EXPECT_EQ(s, GTEXT_HTML_TREE_OK);
      gtext_markup_document_free(doc);
      EXPECT_TRUE(fa.live.empty());
      break;
    }
    EXPECT_EQ(s, GTEXT_HTML_TREE_E_OOM) << k;
    EXPECT_EQ(doc, nullptr) << k;
    EXPECT_EQ(frag, nullptr) << k;
    EXPECT_TRUE(fa.live.empty()) << k;
  }
}

TEST(HtmlTreeFragment, TheFormRulesAskAboutParsingTemplateContentsWhichATemplateContextIs) {
  // "In table", form: "If the form element pointer is not null, and the parser is not parsing template contents,
  // then ignore the token. Otherwise: Insert an HTML element for the token, and, if the parser is not parsing
  // template contents, set the form element pointer ... Pop that form element off the stack of open elements."
  // Outside a template the first form sets the pointer and the second is ignored; in a template context
  // ("parsing template contents ... or the parser's fragment context element is a template element") neither sets
  // it and both are inserted.
  EXPECT_EQ(frag_of("<table><form><form>", html_ctx("div")), tree({"<table>", "  <form>"}));
  EXPECT_EQ(frag_of("<table><form><form>", html_ctx("template")), tree({"<table>", "  <form>", "  <form>"}));
  // "In body", form start tag, the same sentence; and the end tag: "If the parser is not parsing template
  // contents: ... Otherwise: If the stack of open elements does not have a form element in scope ... ignore".
  EXPECT_EQ(frag_of("<form><form>", html_ctx("template")), tree({"<form>", "  <form>"}));
  EXPECT_EQ(frag_of("<form>a</form>b</form>", html_ctx("template")), tree({"<form>", "  \"a\"", "\"b\""}));
  EXPECT_EQ(frag_of("<form>a</form>b</form>", html_ctx("div")), tree({"<form>", "  \"a\"", "\"b\""}));
}

TEST(HtmlTreeFragment, AForeignContextsEndTagIsAStrayEndTagOfTheRootNotOfTheContext) {
  // 13.2.6.5 "any other end tag": "1. Initialize node to be the current node (the bottommost node of the stack).
  // 2. If node's tag name, converted to ASCII lowercase, is not the same as the tag name of the token, then this is a
  // parse error. 3. ... If node is the topmost element in the stack of open elements, then return. (fragment case)".
  // The current node of a fragment is the root html element, never the context element, so `</svg>` in an svg
  // context is a stray end tag, as the vectors say (foreign-fragment.dat: "Stray end tag svg"); `</html>` is not.
  const Ctx svg = svg_ctx("svg");
  Out r = run("</svg>X", &svg);
  EXPECT_EQ(r.dump, tree({"\"X\""}));
  EXPECT_EQ(r.error_names(), std::vector<std::string>{"unexpected-end-tag"});
  const Ctx path = svg_ctx("path");
  r = run("</path>X", &path);
  EXPECT_EQ(r.error_names(), std::vector<std::string>{"unexpected-end-tag"});
  r = run("</html>X", &path);
  EXPECT_TRUE(r.error_names().empty());
}

TEST(HtmlTreeFragment, ThereIsOneScriptingFlagAndItIsTheContexts) {
  const std::string noscript = "<noscript><b>x</b></noscript>";
  const std::string as_element = tree({"<noscript>", "  <b>", "    \"x\""});
  const std::string as_text = tree({"<noscript>", "  \"<b>x</b>\""});
  for (int option : {0, 1}) {
    for (int flag : {0, 1}) {
      SCOPED_TRACE("option " + std::to_string(option) + ", context " + std::to_string(flag));
      GTEXT_HTML_TreeOptions o = {};
      o.scripting = option != 0;
      Ctx div = html_ctx("div");
      div.scripting = flag != 0;
      // The context's flag decides, whatever the options say.
      EXPECT_EQ(run(noscript, &div, o).dump, flag ? as_text : as_element);
      Ctx ns = html_ctx("noscript");
      ns.scripting = flag != 0;
      EXPECT_EQ(run("<b>x</b>", &ns, o).dump, flag ? tree({"\"<b>x</b>\""}) : tree({"<b>", "  \"x\""}));
    }
  }
  // A document builder reads the option, as before.
  GTEXT_HTML_TreeOptions on = {};
  on.scripting = true;
  EXPECT_NE(run("<head><noscript><p>x", nullptr, on).dump, run("<head><noscript><p>x").dump);
}

TEST(HtmlTreeForeign, ASelfClosingSvgScriptIsPoppedLikeAnyOtherElementBecauseNothingIsRun) {
  // "act as described in the steps for a "script" end tag" begins with popping the current node and goes on to
  // run the script, which this parser never does: the observable rule is the pop.
  EXPECT_EQ(doc_of("<svg><script/><script/>y"), in_body({"<svg svg>", "  <svg script>", "  <svg script>", "  \"y\""}));
  EXPECT_EQ(doc_of("<svg><script/></script>y"), in_body({"<svg svg>", "  <svg script>", "  \"y\""}));
}

TEST(HtmlTreeFragment, AFramesetContextStaysInFramesetAfterAnInnerFramesetIsClosed) {
  // "If the parser's fragment context element is null and the current node is no longer a frameset element, then
  // switch to "after frameset"": the context is not null, so the mode stays "in frameset" and the frame is made.
  EXPECT_EQ(frag_of("<frameset></frameset><frame>", html_ctx("frameset")), tree({"<frameset>", "<frame>"}));
}

TEST(HtmlTreeErrorsForeign, TheForeignErrorsAreNamedAndPositioned) {
  Out r = run("<svg><p>");
  const GTEXT_HTML_TreeError * e = nullptr;
  for (const auto & x : r.errors) {
    if (std::string(gtext_html_tree_error_name(x.code)) == "html-in-foreign-content") {
      e = &x;
    }
  }
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->line, 1u);
  EXPECT_EQ(e->column, 6u);
  EXPECT_EQ(e->offset, 5u);
  r = run("\n  <svg xmlns=a>");
  e = nullptr;
  for (const auto & x : r.errors) {
    if (std::string(gtext_html_tree_error_name(x.code)) == "xmlns-attribute-mismatch") {
      e = &x;
    }
  }
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->line, 2u);
  EXPECT_EQ(e->column, 3u);
  EXPECT_EQ(e->offset, 3u);
  // An end tag p that breaks out is the same error at the end tag.
  r = run("<svg></p>");
  ASSERT_TRUE(r.has_error("html-in-foreign-content"));
}

TEST(HtmlTreeForeign, TheCdataDecisionIsMadeAfterEveryEarlierTokenWhereverTheInputIsCut) {
  // The tokenizer asks "is the adjusted current node not in the HTML namespace" when it has read `<![CDATA[`.
  // The tree builder has handled every token before that one by then (a token is delivered the moment it is
  // complete and nothing past it has been read), and characters never change the stack, so the answer does not
  // depend on the cut. Every cut of each input gives the whole input's answer, and the answer is the right one.
  struct Case {
    std::string in;
    std::string dump;
    bool cdata_error;
  };
  const Case cases[] = {
      {"<svg><![CDATA[x]]>", in_body({"<svg svg>", "  \"x\""}), false},
      {"<svg>ab<![CDATA[x]]>cd", in_body({"<svg svg>", "  \"abxcd\""}), false},
      {"<p><![CDATA[x]]>", in_body({"<p>", "  <!-- [CDATA[x]] -->"}), true},
      {"<p>ab<![CDATA[x]]>", in_body({"<p>", "  \"ab\"", "  <!-- [CDATA[x]] -->"}), true},
      {"<svg></svg><![CDATA[x]]>", in_body({"<svg svg>", "<!-- [CDATA[x]] -->"}), true},
      {"<math><mi><![CDATA[a]]><![CDATA[b]]>", in_body({"<math math>", "  <math mi>", "    \"ab\""}), false},
      {"<svg><foreignObject><p><![CDATA[x]]>", in_body({"<svg svg>", "  <svg foreignObject>", "    <p>", "      <!-- [CDATA[x]] -->"}), true},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.in);
    const Out whole = run(c.in);
    EXPECT_EQ(whole.dump, c.dump);
    EXPECT_EQ(whole.has_error("cdata-in-html-content"), c.cdata_error);
    for (size_t k = 0; k <= c.in.size(); k++) {
      EXPECT_TRUE(whole.same_as(run(c.in, nullptr, {}, {k}))) << k;
    }
    EXPECT_TRUE(whole.same_as(run(c.in, nullptr, {}, {}, true)));
  }
}
