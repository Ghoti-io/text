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
 * The XHTML-compatible mode of the XML writer (GTEXT_XML_Write_Options::xhtml).
 *
 * - Every row of the specification's matrix, and each refusal and hazard kind
 *   by name, path and count; strict mode; the option validation.
 * - Without the option, output is exactly what it was (literal expectations on
 *   XHTML-shaped trees, which the new rules would change if they leaked).
 * - The round trip the XML side can check now: XML-parse, write(xhtml),
 *   XML-parse gives an equal tree, over hand-written XHTML, every
 *   well-formed xmlconf document when it is fetched, and a few thousand random
 *   trees (accepted ones round-trip; refused ones are refused with
 *   GTEXT_XML_E_XHTML or an XML status, never written).
 * - The comparison function the HTML parser half of the exit gate will use
 *   (html_equivalent below) is specified in notes/text/XML-HTML-PLAN.md and
 *   tested here against the XML side: it holds between a tree and its
 *   round-tripped self, and fails for every difference except the declaration.
 * - The allocator and the sink failing at every point.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>

#include <gtest/gtest.h>

#include <ghoti.io/text/xml.h>

#include "markup_test_support.h"

namespace {

using Doc = GTEXT_Markup_Document;
using Node = GTEXT_Markup_Node;
using Opts = GTEXT_XML_Write_Options;

#define XHTML "http://www.w3.org/1999/xhtml"
#define SVG "http://www.w3.org/2000/svg"
#define MATHML "http://www.w3.org/1998/Math/MathML"
#define NS_XMLNS "http://www.w3.org/2000/xmlns/"

Opts xopts() {
  Opts o = gtext_xml_write_options_default();
  o.xhtml = true;
  return o;
}

Doc * parse(const std::string & text, bool expect_ok = true) {
  GTEXT_XML_Parse_Options o = gtext_xml_parse_options_default();
  Doc * doc = nullptr;
  GTEXT_XML_Error err;
  const GTEXT_XML_Status st = gtext_xml_parse(text.data(), text.size(), &o, &doc, &err);
  if (expect_ok) {
    EXPECT_EQ(st, GTEXT_XML_OK) << text << ": " << (err.message ? err.message : "");
  }
  return st == GTEXT_XML_OK ? doc : nullptr;
}

struct Out {
  GTEXT_XML_Status status;
  std::string bytes;
  GTEXT_XML_Write_Result r;
};

Out write_with(const Doc * doc, const Opts & o) {
  Out out;
  char * buf = nullptr;
  size_t len = 0;
  out.status = gtext_xml_write_buffer(doc, &o, &buf, &len, &out.r);
  if (out.status == GTEXT_XML_OK) {
    out.bytes.assign(buf, len);
    gtext_xml_write_buffer_free(doc, &o, buf);
  }
  else {
    EXPECT_EQ(buf, nullptr);
  }
  return out;
}

Out write_x(const Doc * doc) {
  return write_with(doc, xopts());
}

struct Build {
  Doc * doc = nullptr;
  Build() {
    GTEXT_Markup_Options o = gtext_markup_options_default();
    o.max_depth = 0;
    o.max_attributes = 0;
    EXPECT_EQ(gtext_markup_document_new(&o, &doc), GTEXT_MARKUP_OK);
  }
  explicit Build(const GTEXT_Allocator * a) {
    GTEXT_Markup_Options o = gtext_markup_options_default();
    o.max_depth = 0;
    o.allocator = a;
    EXPECT_EQ(gtext_markup_document_new(&o, &doc), GTEXT_MARKUP_OK);
  }
  ~Build() {
    gtext_markup_document_free(doc);
  }
  Build(const Build &) = delete;
  Build & operator=(const Build &) = delete;

  Node * root() {
    return gtext_markup_document_root(doc);
  }
  // An element in the XHTML namespace unless `ns` says otherwise.
  Node * el(Node * parent, const char * local, const char * ns = XHTML, const char * prefix = nullptr) {
    Node * n = nullptr;
    EXPECT_EQ(gtext_markup_create_element(doc, ns, prefix, local, &n), GTEXT_MARKUP_OK);
    EXPECT_EQ(gtext_markup_append_child(parent ? parent : root(), n), GTEXT_MARKUP_OK);
    return n;
  }
  void attr(Node * e, const char * local, const std::string & value, const char * ns = nullptr,
      const char * prefix = nullptr) {
    EXPECT_EQ(gtext_markup_element_set_attribute(e, ns, prefix, local, value.data(), value.size()),
        GTEXT_MARKUP_OK);
  }
  void xmlns(Node * e, const char * prefix, const std::string & uri) {
    if (prefix) {
      attr(e, prefix, uri, NS_XMLNS, "xmlns");
    }
    else {
      attr(e, "xmlns", uri, NS_XMLNS, nullptr);
    }
  }
  Node * text(Node * parent, const std::string & s) {
    Node * n = nullptr;
    EXPECT_EQ(gtext_markup_create_text(doc, s.data(), s.size(), &n), GTEXT_MARKUP_OK);
    EXPECT_EQ(gtext_markup_append_child(parent, n), GTEXT_MARKUP_OK);
    return n;
  }
  Node * cdata(Node * parent, const std::string & s) {
    Node * n = nullptr;
    EXPECT_EQ(gtext_markup_create_cdata(doc, s.data(), s.size(), &n), GTEXT_MARKUP_OK);
    EXPECT_EQ(gtext_markup_append_child(parent, n), GTEXT_MARKUP_OK);
    return n;
  }
  Node * comment(Node * parent, const std::string & s) {
    Node * n = nullptr;
    EXPECT_EQ(gtext_markup_create_comment(doc, s.data(), s.size(), &n), GTEXT_MARKUP_OK);
    EXPECT_EQ(gtext_markup_append_child(parent ? parent : root(), n), GTEXT_MARKUP_OK);
    return n;
  }
  Node * pi(Node * parent, const std::string & target, const std::string & data) {
    Node * n = nullptr;
    EXPECT_EQ(gtext_markup_create_processing_instruction(
                  doc, target.c_str(), data.data(), data.size(), &n),
        GTEXT_MARKUP_OK);
    EXPECT_EQ(gtext_markup_append_child(parent ? parent : root(), n), GTEXT_MARKUP_OK);
    return n;
  }
};

// An html root with the XHTML namespace declared, for trees that should not
// depend on the declaration being added.
// With a head, so that adding a body makes the standard shape of a document.
Node * html_root(Build & b, bool with_head = false) {
  Node * html = b.el(nullptr, "html");
  b.xmlns(html, nullptr, XHTML);
  if (with_head) {
    b.el(html, "head");
  }
  return html;
}

struct Collect {
  std::string data;
  size_t calls = 0;
  size_t fail_on = 0;
  static GTEXT_Markup_Status write(void * user, const char * b, size_t n) {
    auto * self = static_cast<Collect *>(user);
    self->calls++;
    if (self->fail_on != 0 && self->calls == self->fail_on) {
      return GTEXT_MARKUP_E_WRITE;
    }
    self->data.append(b, n);
    return GTEXT_MARKUP_OK;
  }
  GTEXT_Markup_Sink sink() {
    return GTEXT_Markup_Sink{&Collect::write, this};
  }
};

// ---- html_equivalent: the comparison the HTML parser half will use ---------
//
// Two documents are equivalent when, after normalization, they are equal:
//   * locations, prefixes and the order of attributes are not compared;
//   * attributes in the xmlns namespace (declarations) are dropped from
//     elements, and so are attributes named `xmlns` or `xmlns:*` in no
//     namespace: that is the known representation difference (the XML parser
//     records declarations in the xmlns namespace; an HTML parser keeps them as
//     plain attributes);
//   * empty text nodes are dropped, adjacent text and CDATA are one text node
//     (an HTML parser has no CDATA node; XML cannot tell the others apart);
//   * element equality is namespace and local name, then the attribute set by
//     (namespace, local name, value); comments by data; doctypes by name and
//     both identifiers; processing instructions never occur (the mode refuses
//     them) and compare as unequal to everything.
struct Norm {
  const Node * node;     // nullptr for a merged text
  std::string text;
};

std::vector<Norm> normalized_children(const Node * parent) {
  std::vector<Norm> out;
  for (const Node * c = gtext_markup_node_first_child(parent); c; c = gtext_markup_node_next_sibling(c)) {
    const GTEXT_Markup_Kind k = gtext_markup_node_kind(c);
    if (k == GTEXT_MARKUP_KIND_TEXT || k == GTEXT_MARKUP_KIND_CDATA) {
      size_t len = 0;
      const char * d = gtext_markup_node_data(c, &len);
      if (len == 0) {
        continue;
      }
      if (!out.empty() && out.back().node == nullptr) {
        out.back().text.append(d, len);
      }
      else {
        out.push_back({nullptr, std::string(d, len)});
      }
    }
    else {
      out.push_back({c, ""});
    }
  }
  return out;
}

std::vector<std::string> attribute_set(const Node * e) {
  std::vector<std::string> out;
  const size_t n = gtext_markup_element_attribute_count(e);
  for (size_t i = 0; i < n; i++) {
    GTEXT_Markup_Attribute a;
    gtext_markup_element_attribute_at(e, i, &a);
    if (a.ns && std::strcmp(a.ns, NS_XMLNS) == 0) {
      continue;
    }
    // An HTML parser keeps `xmlns` and `xmlns:*` as plain attributes: the same
    // declarations, in no namespace.
    if (!a.ns && (std::strcmp(a.local, "xmlns") == 0 || std::strncmp(a.local, "xmlns:", 6) == 0)) {
      continue;
    }
    out.push_back(std::string(a.ns ? a.ns : "") + "|" + a.local + "=" + std::string(a.value, a.value_length));
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string str(const char * s) {
  return s ? s : "";
}

bool html_equivalent(const Node * a, const Node * b) {
  const GTEXT_Markup_Kind ka = gtext_markup_node_kind(a);
  if (ka != gtext_markup_node_kind(b)) {
    return false;
  }
  switch (ka) {
    case GTEXT_MARKUP_KIND_ELEMENT:
      if (str(gtext_markup_element_namespace(a)) != str(gtext_markup_element_namespace(b))
          || str(gtext_markup_element_local_name(a)) != str(gtext_markup_element_local_name(b))
          || attribute_set(a) != attribute_set(b)) {
        return false;
      }
      break;
    case GTEXT_MARKUP_KIND_COMMENT: {
      size_t la = 0, lb = 0;
      const char * da = gtext_markup_node_data(a, &la);
      const char * db = gtext_markup_node_data(b, &lb);
      return std::string(da, la) == std::string(db, lb);
    }
    case GTEXT_MARKUP_KIND_DOCTYPE:
      return str(gtext_markup_doctype_name(a)) == str(gtext_markup_doctype_name(b))
          && str(gtext_markup_doctype_public_id(a)) == str(gtext_markup_doctype_public_id(b))
          && str(gtext_markup_doctype_system_id(a)) == str(gtext_markup_doctype_system_id(b));
    case GTEXT_MARKUP_KIND_DOCUMENT:
      break;
    default:
      return false;  // processing instructions, and anything unexpected
  }
  const std::vector<Norm> ca = normalized_children(a);
  const std::vector<Norm> cb = normalized_children(b);
  if (ca.size() != cb.size()) {
    return false;
  }
  for (size_t i = 0; i < ca.size(); i++) {
    if ((ca[i].node == nullptr) != (cb[i].node == nullptr)) {
      return false;
    }
    if (ca[i].node == nullptr ? ca[i].text != cb[i].text : !html_equivalent(ca[i].node, cb[i].node)) {
      return false;
    }
  }
  return true;
}

bool trees_equal(const Doc * a, const Doc * b) {
  return gtext_markup_equal(gtext_markup_document_root(a), gtext_markup_document_root(b));
}

// ---- the matrix ---------------------------------------------------------

TEST(XhtmlWriter, DefaultsKeepTheModeOff) {
  const Opts d = gtext_xml_write_options_default();
  EXPECT_FALSE(d.xhtml);
  EXPECT_FALSE(d.xhtml_strict);
  Opts zero;
  std::memset(&zero, 0, sizeof(zero));
  EXPECT_FALSE(zero.xhtml);
}

TEST(XhtmlWriter, VoidElementsAreSelfClosedWithASpace) {
  Build b;
  Node * html = html_root(b, true);
  Node * body = b.el(html, "body");
  b.el(body, "br");
  b.el(body, "img");
  b.attr(b.el(body, "input"), "disabled", "disabled");
  b.el(body, "hr");
  const Out out = write_x(b.doc);
  ASSERT_EQ(out.status, GTEXT_XML_OK) << out.r.message;
  EXPECT_EQ(out.bytes,
      "<html xmlns=\"" XHTML "\"><head></head><body><br /><img /><input disabled=\"disabled\" /><hr /></body></html>");
  EXPECT_EQ(out.r.xhtml_hazards, 0u);
  EXPECT_EQ(out.r.xhtml_hazard_kind, GTEXT_XML_XHTML_HAZARD_NONE);
  EXPECT_STREQ(out.r.xhtml_hazard_path, "");
  // An empty text node does not make a void element non-empty.
  Build e;
  Node * h2 = html_root(e);
  Node * br = e.el(h2, "br");
  e.text(br, "");
  EXPECT_EQ(write_x(e.doc).bytes, "<html xmlns=\"" XHTML "\"><br /></html>");
}

TEST(XhtmlWriter, AVoidElementWithChildrenIsRefusedWithItsPath) {
  for (int what = 0; what < 3; what++) {
    Build b;
    Node * html = html_root(b);
    Node * br = b.el(b.el(html, "body"), "br");
    if (what == 0) {
      b.text(br, "x");
    }
    else if (what == 1) {
      b.comment(br, "c");
    }
    else {
      b.el(br, "span");
    }
    const Out out = write_x(b.doc);
    EXPECT_EQ(out.status, GTEXT_XML_E_XHTML) << what;
    EXPECT_STREQ(out.r.path, "/html/body/br");
    EXPECT_NE(std::string(out.r.message).find("void"), std::string::npos);
    EXPECT_EQ(out.r.code, GTEXT_XML_E_XHTML);
  }
}

TEST(XhtmlWriter, EveryEmptyNonVoidElementIsExpanded) {
  Build b;
  Node * html = html_root(b);
  Node * body = b.el(html, "body");
  for (const char * n : {"script", "style", "p", "div", "textarea", "title", "a", "td", "span"}) {
    b.el(body, n);
  }
  const Out out = write_x(b.doc);
  ASSERT_EQ(out.status, GTEXT_XML_OK);
  EXPECT_EQ(out.bytes,
      "<html xmlns=\"" XHTML "\"><body><script></script><style></style><p></p><div></div>"
      "<textarea></textarea><title></title><a></a><td></td><span></span></body></html>");
  EXPECT_EQ(out.bytes.find("/>"), std::string::npos);
  // Elements in another namespace follow the old rule, and expand_empty_elements
  // still applies to them.
  Build s;
  Node * svg = s.el(nullptr, "svg", SVG);
  s.xmlns(svg, nullptr, SVG);
  s.el(svg, "br", SVG);
  EXPECT_EQ(write_x(s.doc).bytes, "<svg xmlns=\"" SVG "\"><br/></svg>");
  Opts o = xopts();
  o.expand_empty_elements = true;
  EXPECT_EQ(write_with(s.doc, o).bytes, "<svg xmlns=\"" SVG "\"><br></br></svg>");
}

TEST(XhtmlWriter, ScriptAndStyleTextIsWrittenRawAndNeverAsCdata) {
  Build b;
  Node * html = html_root(b);
  Node * script = b.el(html, "script");
  b.text(script, "if (a > b && c) { x = \"1\" > 'y'; }");
  Node * style = b.el(html, "style");
  b.text(style, "a > b { color: red }\n");
  const Out out = write_x(b.doc);
  ASSERT_EQ(out.status, GTEXT_XML_E_XHTML);  // && has an &
  EXPECT_STREQ(out.r.path, "/html/script/text()");

  Build c;
  Node * h = html_root(c);
  c.text(c.el(h, "script"), "if (a > b) { x = \"1\" > 'y'; }");
  c.text(c.el(h, "style"), "a > b { color: red }\n");
  const Out ok = write_x(c.doc);
  ASSERT_EQ(ok.status, GTEXT_XML_OK) << ok.r.message << ok.r.path;
  EXPECT_EQ(ok.bytes,
      "<html xmlns=\"" XHTML "\"><script>if (a > b) { x = \"1\" > 'y'; }</script>"
      "<style>a > b { color: red }\n</style></html>");
  EXPECT_EQ(ok.bytes.find("CDATA"), std::string::npos);
  Doc * back = parse(ok.bytes);
  EXPECT_TRUE(trees_equal(c.doc, back));
  gtext_markup_document_free(back);
}

TEST(XhtmlWriter, ScriptAndStyleTextHtmlAndXmlWouldReadDifferentlyIsRefused) {
  for (const char * bad : {"a < b", "a &amp b", "x ]]> y", "a\rb", "</script>", "<!--", "&"}) {
    for (const char * name : {"script", "style"}) {
      Build b;
      Node * html = html_root(b);
      b.text(b.el(html, name), bad);
      const Out out = write_x(b.doc);
      EXPECT_EQ(out.status, GTEXT_XML_E_XHTML) << name << " " << bad;
      EXPECT_EQ(out.r.path, std::string("/html/") + name + "/text()");
      EXPECT_EQ(out.bytes, "");
    }
  }
  // The same text elsewhere is escaped as usual (and a script in another
  // namespace is not a script).
  Build b;
  Node * html = html_root(b);
  b.text(b.el(html, "p"), "a < b & c ]]> d");
  EXPECT_EQ(write_x(b.doc).bytes,
      "<html xmlns=\"" XHTML "\"><p>a &lt; b &amp; c ]]&gt; d</p></html>");
  Build s;
  Node * svg = s.el(nullptr, "svg", SVG);
  s.xmlns(svg, nullptr, SVG);
  s.text(s.el(svg, "script", SVG), "a < b");
  EXPECT_EQ(write_x(s.doc).bytes, "<svg xmlns=\"" SVG "\"><script>a &lt; b</script></svg>");
}

TEST(XhtmlWriter, ARawTextElementUnderAnEncodingThatLacksACharacterIsRefusedNotReferenced) {
  Build b;
  Node * html = html_root(b);
  b.text(b.el(html, "script"), "x = '\xE2\x82\xAC'");
  Opts o = xopts();
  o.encoding = GTEXT_XML_OUTPUT_US_ASCII;
  EXPECT_EQ(write_with(b.doc, o).status, GTEXT_XML_E_UNREPRESENTABLE);
  Build t;
  Node * h = html_root(t);
  t.text(t.el(h, "p"), "x = '\xE2\x82\xAC'");
  EXPECT_EQ(write_with(t.doc, o).status, GTEXT_XML_OK);  // a reference, as ever
}

TEST(XhtmlWriter, ALeadingLineFeedInPreTextareaListingIsRefused) {
  for (const char * name : {"pre", "textarea", "listing"}) {
    for (const char * bad : {"\nx", "\n", "\n\nx"}) {
      Build b;
      Node * html = html_root(b);
      b.text(b.el(b.el(html, "body"), name), bad);
      const Out out = write_x(b.doc);
      EXPECT_EQ(out.status, GTEXT_XML_E_XHTML) << name;
      EXPECT_EQ(out.r.path, std::string("/html/body/") + name + "/text()");
    }
    // After an empty text node it is still first; with something before it, not.
    Build e;
    Node * h = html_root(e);
    Node * pre = e.el(h, name);
    e.text(pre, "");
    e.text(pre, "\nx");
    EXPECT_EQ(write_x(e.doc).status, GTEXT_XML_E_XHTML);
    for (const char * fine : {"x\n", "x\n\ny", " \nx", "\r\nx", "a"}) {
      Build g;
      Node * h2 = html_root(g);
      g.text(g.el(h2, name), fine);
      EXPECT_EQ(write_x(g.doc).status, GTEXT_XML_OK) << name << " " << fine;
    }
    Build el;
    Node * h3 = html_root(el, true);
    Node * p3 = el.el(el.el(h3, "body"), name);
    el.el(p3, "b");
    el.text(p3, "\nx");
    // An element first: the line feed is not the first thing (it is not a
    // hazard either: only the text rule applies).
    const Out after = write_x(el.doc);
    EXPECT_EQ(after.status, GTEXT_XML_OK) << name;
    EXPECT_EQ(after.r.xhtml_hazards, std::string(name) == "textarea" ? 1u : 0u) << name;
  }
  // Other elements keep their leading line feeds.
  Build b;
  Node * html = html_root(b);
  b.text(b.el(html, "p"), "\nx");
  EXPECT_EQ(write_x(b.doc).bytes, "<html xmlns=\"" XHTML "\"><p>\nx</p></html>");
}

TEST(XhtmlWriter, TheXhtmlNamespaceIsDeclaredOnTheRootWhenMissing) {
  Build b;
  Node * html = b.el(nullptr, "html");
  b.el(html, "body");
  const Out out = write_x(b.doc);
  ASSERT_EQ(out.status, GTEXT_XML_OK) << out.r.message;
  EXPECT_EQ(out.bytes, "<html xmlns=\"" XHTML "\"><body></body></html>");
  EXPECT_EQ(out.r.namespace_declarations_added, 1u);
  // Without the mode the same tree is refused, as before.
  Opts plain = gtext_xml_write_options_default();
  EXPECT_EQ(write_with(b.doc, plain).status, GTEXT_XML_E_NAMESPACE);
  // Already declared: nothing is added.
  Build d;
  html_root(d);
  const Out declared = write_x(d.doc);
  EXPECT_EQ(declared.r.namespace_declarations_added, 0u);
  EXPECT_EQ(declared.bytes, "<html xmlns=\"" XHTML "\"></html>");
  // The re-written output declares it once, and parses to the same namespace.
  Doc * back = parse(out.bytes);
  ASSERT_NE(back, nullptr);
  EXPECT_STREQ(gtext_markup_element_namespace(gtext_markup_document_element(back)), XHTML);
  EXPECT_TRUE(html_equivalent(gtext_markup_document_root(b.doc), gtext_markup_document_root(back)));
  EXPECT_FALSE(trees_equal(b.doc, back));  // the xmlns attribute is the difference
  gtext_markup_document_free(back);
  // A root that is not XHTML does not get one, and an XHTML child under it
  // without a declaration is refused (only the root is repaired).
  Build s;
  Node * svg = s.el(nullptr, "svg", SVG);
  s.xmlns(svg, nullptr, SVG);
  s.el(svg, "p");
  EXPECT_EQ(write_x(s.doc).status, GTEXT_XML_E_NAMESPACE);
  // A root that declares another default namespace for itself contradicts.
  Build c;
  Node * r = c.el(nullptr, "html");
  c.xmlns(r, nullptr, "urn:other");
  EXPECT_EQ(write_x(c.doc).status, GTEXT_XML_E_NAMESPACE);
}

TEST(XhtmlWriter, AnElementInAnotherNamespaceIsNotVoidAndIsWrittenAsXml) {
  Build c;
  Node * h = html_root(c);
  Node * s = c.el(h, "svg", SVG);
  c.xmlns(s, nullptr, SVG);
  c.el(s, "br", SVG);
  c.el(s, "img", SVG);
  const Out ok = write_x(c.doc);
  ASSERT_EQ(ok.status, GTEXT_XML_OK) << ok.r.message << ok.r.path;
  EXPECT_EQ(ok.bytes, "<html xmlns=\"" XHTML "\"><svg xmlns=\"" SVG "\"><br/><img/></svg></html>");
  // (Written as XML, and not void; that HTML would end the svg at br is a hazard.)
  EXPECT_EQ(ok.r.xhtml_hazard_kind, GTEXT_XML_XHTML_HAZARD_DOCUMENT_STRUCTURE);
  EXPECT_GE(ok.r.xhtml_hazards, 3u);
}

TEST(XhtmlWriter, AnElementInNoNamespaceUnderAnXhtmlRootNeedsAReset) {
  // The default namespace is the XHTML one; an element in no namespace cannot be
  // written under it without an xmlns="" this writer does not invent.
  Build b;
  Node * html = html_root(b);
  b.el(html, "p", nullptr);
  const Out out = write_x(b.doc);
  EXPECT_EQ(out.status, GTEXT_XML_E_NAMESPACE);
  EXPECT_EQ(out.bytes, "");
  // With the reset in the tree it is written, and is hazardous (HTML would make
  // it an XHTML-namespace element).
  Build c;
  Node * h = html_root(c);
  Node * p = c.el(h, "p", nullptr);
  c.xmlns(p, nullptr, "");
  const Out ok = write_x(c.doc);
  ASSERT_EQ(ok.status, GTEXT_XML_OK) << ok.r.message << ok.r.path;
  EXPECT_NE(ok.r.xhtml_hazards, 0u);
}

TEST(XhtmlWriter, APrefixedXhtmlElementIsRefused) {
  Build b;
  Node * r = b.el(nullptr, "html", XHTML, "h");
  b.xmlns(r, "h", XHTML);
  const Out out = write_x(b.doc);
  EXPECT_EQ(out.status, GTEXT_XML_E_XHTML);
  EXPECT_STREQ(out.r.path, "/h:html");
  // Without the mode it is ordinary XML.
  EXPECT_EQ(write_with(b.doc, gtext_xml_write_options_default()).status, GTEXT_XML_OK);
}

TEST(XhtmlWriter, CommentsHtmlWouldEndEarlyAreRefused) {
  for (const char * bad : {">", ">x", "->", "->x", "-> y"}) {
    Build b;
    Node * html = html_root(b);
    b.comment(html, bad);
    const Out out = write_x(b.doc);
    EXPECT_EQ(out.status, GTEXT_XML_E_XHTML) << bad;
    EXPECT_STREQ(out.r.path, "/html/comment()");
  }
  for (const char * bad : {"x--y", "x-", "<!-", "a<!-"}) {  // XML already refuses these
    Build b;
    Node * html = html_root(b);
    b.comment(html, bad);
    const Out out = write_x(b.doc);
    EXPECT_EQ(out.status, GTEXT_XML_E_UNREPRESENTABLE) << bad;
  }
  for (const char * fine : {"", " ", "x", "a>b", "a->b", "- >", "<!", "<", " > "}) {
    Build b;
    Node * html = html_root(b);
    b.comment(html, fine);
    const Out out = write_x(b.doc);
    ASSERT_EQ(out.status, GTEXT_XML_OK) << "[" << fine << "]";
    Doc * back = parse(out.bytes);
    EXPECT_TRUE(trees_equal(b.doc, back));
    gtext_markup_document_free(back);
  }
  // At the top level too.
  Build t;
  html_root(t);
  t.comment(nullptr, ">");
  EXPECT_EQ(write_x(t.doc).status, GTEXT_XML_E_XHTML);
}

TEST(XhtmlWriter, ProcessingInstructionsAreRefusedEverywhere) {
  Build b;
  Node * html = html_root(b);
  b.pi(html, "php", "echo 1");
  Out out = write_x(b.doc);
  EXPECT_EQ(out.status, GTEXT_XML_E_XHTML);
  EXPECT_STREQ(out.r.path, "/html/processing-instruction(php)");
  Build t;
  html_root(t);
  t.pi(nullptr, "xml-stylesheet", "href=\"a.css\"");
  out = write_x(t.doc);
  EXPECT_EQ(out.status, GTEXT_XML_E_XHTML);
  EXPECT_STREQ(out.r.path, "/processing-instruction(xml-stylesheet)");
  // Ordinary XML still has them.
  EXPECT_EQ(write_with(t.doc, gtext_xml_write_options_default()).status, GTEXT_XML_OK);
}

TEST(XhtmlWriter, ACdataSectionIsRefusedOutsideForeignContent) {
  Build b;
  Node * html = html_root(b);
  b.cdata(b.el(html, "p"), "x");
  Out out = write_x(b.doc);
  EXPECT_EQ(out.status, GTEXT_XML_E_XHTML);
  EXPECT_STREQ(out.r.path, "/html/p/cdata()");
  Build s;
  Node * h = html_root(s);
  Node * svg = s.el(h, "svg", SVG);
  s.xmlns(svg, nullptr, SVG);
  s.cdata(s.el(svg, "text", SVG), "a < b");
  Node * math = s.el(h, "math", MATHML);
  s.xmlns(math, nullptr, MATHML);
  s.cdata(s.el(math, "mtext", MATHML), "x");  // an HTML integration point
  out = write_x(s.doc);
  EXPECT_EQ(out.status, GTEXT_XML_E_XHTML);
  EXPECT_STREQ(out.r.path, "/html/math/mtext/cdata()");
  Build f;
  Node * h2 = html_root(f);
  Node * svg2 = f.el(h2, "svg", SVG);
  f.xmlns(svg2, nullptr, SVG);
  f.cdata(f.el(svg2, "text", SVG), "a < b");
  Node * math2 = f.el(h2, "math", MATHML);
  f.xmlns(math2, nullptr, MATHML);
  f.cdata(f.el(math2, "mi", MATHML), "x");  // an integration point too
  EXPECT_EQ(write_x(f.doc).status, GTEXT_XML_E_XHTML);
  Build g;
  Node * h3 = html_root(g);
  Node * svg3 = g.el(h3, "svg", SVG);
  g.xmlns(svg3, nullptr, SVG);
  g.cdata(g.el(svg3, "text", SVG), "a < b");
  const Out ok = write_x(g.doc);
  ASSERT_EQ(ok.status, GTEXT_XML_OK);
  EXPECT_NE(ok.bytes.find("<![CDATA[a < b]]>"), std::string::npos);
}

// ---- hazards ------------------------------------------------------------

struct HazardCase {
  const char * what;
  std::function<void(Build &, Node *)> build;
  GTEXT_XML_Xhtml_Hazard kind;
  size_t count;
  const char * path;
};

TEST(XhtmlWriter, HazardsAreCountedWithTheFirstOnesKindAndPathAndStrictRefusesThem) {
  const std::vector<HazardCase> cases = {
      {"table > tr", [](Build & b, Node * body) { b.el(b.el(body, "table"), "tr"); },
          GTEXT_XML_XHTML_HAZARD_TABLE_IMPLIED_PART, 1, "/html/body/table/tr"},
      {"table > td", [](Build & b, Node * body) { b.el(b.el(body, "table"), "td"); },
          GTEXT_XML_XHTML_HAZARD_TABLE_IMPLIED_PART, 1, "/html/body/table/td"},
      {"table > tbody > td", [](Build & b, Node * body) { b.el(b.el(b.el(body, "table"), "tbody"), "td"); },
          GTEXT_XML_XHTML_HAZARD_TABLE_IMPLIED_PART, 1, "/html/body/table/tbody/td"},
      {"table > thead, tr", [](Build & b, Node * body) {
         Node * t = b.el(body, "table");
         b.el(t, "thead");
         b.el(t, "tr");
       }, GTEXT_XML_XHTML_HAZARD_TABLE_IMPLIED_PART, 1, "/html/body/table/tr"},
      {"table > text", [](Build & b, Node * body) { b.text(b.el(body, "table"), "x"); },
          GTEXT_XML_XHTML_HAZARD_TABLE_FOSTER_PARENT, 1, "/html/body/table/text()"},
      {"table > div", [](Build & b, Node * body) { b.el(b.el(body, "table"), "div"); },
          GTEXT_XML_XHTML_HAZARD_TABLE_FOSTER_PARENT, 1, "/html/body/table/div"},
      {"tr > div", [](Build & b, Node * body) { b.el(b.el(b.el(b.el(body, "table"), "tbody"), "tr"), "div"); },
          GTEXT_XML_XHTML_HAZARD_TABLE_FOSTER_PARENT, 1, "/html/body/table/tbody/tr/div"},
      {"p > div", [](Build & b, Node * body) { b.el(b.el(body, "p"), "div"); },
          GTEXT_XML_XHTML_HAZARD_P_BLOCK, 1, "/html/body/p/div"},
      {"p > span > ul", [](Build & b, Node * body) { b.el(b.el(b.el(body, "p"), "span"), "ul"); },
          GTEXT_XML_XHTML_HAZARD_P_BLOCK, 1, "/html/body/p/span/ul"},
      {"p > p", [](Build & b, Node * body) { b.el(b.el(body, "p"), "p"); },
          GTEXT_XML_XHTML_HAZARD_P_BLOCK, 1, "/html/body/p/p"},
      {"a > a", [](Build & b, Node * body) { b.el(b.el(body, "a"), "a"); },
          GTEXT_XML_XHTML_HAZARD_NESTED_A, 1, "/html/body/a/a"},
      {"a > b > a", [](Build & b, Node * body) { b.el(b.el(b.el(body, "a"), "b"), "a"); },
          GTEXT_XML_XHTML_HAZARD_NESTED_A, 1, "/html/body/a/b/a"},
      {"form > form", [](Build & b, Node * body) { b.el(b.el(b.el(body, "form"), "div"), "form"); },
          GTEXT_XML_XHTML_HAZARD_NESTED_FORM, 1, "/html/body/form/div/form"},
      {"li > li", [](Build & b, Node * body) { b.el(b.el(b.el(body, "ul"), "li"), "li"); },
          GTEXT_XML_XHTML_HAZARD_IMPLIED_END, 1, "/html/body/ul/li/li"},
      {"dd > dt", [](Build & b, Node * body) { b.el(b.el(b.el(body, "dl"), "dd"), "dt"); },
          GTEXT_XML_XHTML_HAZARD_IMPLIED_END, 1, "/html/body/dl/dd/dt"},
      {"option > option", [](Build & b, Node * body) { b.el(b.el(b.el(body, "select"), "option"), "option"); },
          GTEXT_XML_XHTML_HAZARD_IMPLIED_END, 1, "/html/body/select/option/option"},
      {"option > optgroup", [](Build & b, Node * body) { b.el(b.el(b.el(body, "select"), "option"), "optgroup"); },
          GTEXT_XML_XHTML_HAZARD_IMPLIED_END, 1, "/html/body/select/option/optgroup"},
      {"h1 > h2", [](Build & b, Node * body) { b.el(b.el(body, "h1"), "h2"); },
          GTEXT_XML_XHTML_HAZARD_IMPLIED_END, 1, "/html/body/h1/h2"},
      {"td > td", [](Build & b, Node * body) {
         b.el(b.el(b.el(b.el(b.el(body, "table"), "tbody"), "tr"), "td"), "td");
       }, GTEXT_XML_XHTML_HAZARD_IMPLIED_END, 1, "/html/body/table/tbody/tr/td/td"},
      {"tr > tr", [](Build & b, Node * body) { b.el(b.el(b.el(b.el(body, "table"), "tbody"), "tr"), "tr"); },
          GTEXT_XML_XHTML_HAZARD_IMPLIED_END, 1, "/html/body/table/tbody/tr/tr"},
      {"script > element", [](Build & b, Node * body) { b.el(b.el(body, "script"), "b"); },
          GTEXT_XML_XHTML_HAZARD_RAW_TEXT_CONTENT, 1, "/html/body/script/b"},
      {"title > comment", [](Build & b, Node * body) { b.comment(b.el(body, "title"), "c"); },
          GTEXT_XML_XHTML_HAZARD_RAW_TEXT_CONTENT, 1, "/html/body/title/comment()"},
      {"textarea > element", [](Build & b, Node * body) { b.el(b.el(body, "textarea"), "i"); },
          GTEXT_XML_XHTML_HAZARD_RAW_TEXT_CONTENT, 1, "/html/body/textarea/i"},
      {"xmp with <", [](Build & b, Node * body) { b.text(b.el(body, "xmp"), "a<b"); },
          GTEXT_XML_XHTML_HAZARD_RAW_TEXT_CONTENT, 1, "/html/body/xmp/text()"},
      {"noembed with &", [](Build & b, Node * body) { b.text(b.el(body, "noembed"), "a&b"); },
          GTEXT_XML_XHTML_HAZARD_RAW_TEXT_CONTENT, 1, "/html/body/noembed/text()"},
      {"two: the first is reported", [](Build & b, Node * body) {
         b.el(b.el(body, "a"), "a");
         b.el(b.el(body, "p"), "div");
       }, GTEXT_XML_XHTML_HAZARD_NESTED_A, 2, "/html/body/a/a"},
  };
  for (const HazardCase & c : cases) {
    SCOPED_TRACE(c.what);
    Build b;
    Node * html = html_root(b, true);
    Node * body = b.el(html, "body");
    c.build(b, body);
    const Out out = write_x(b.doc);
    ASSERT_EQ(out.status, GTEXT_XML_OK) << out.r.message << " " << out.r.path;
    EXPECT_EQ(out.r.xhtml_hazards, c.count);
    EXPECT_EQ(out.r.xhtml_hazard_kind, c.kind);
    EXPECT_STREQ(out.r.xhtml_hazard_path, c.path);
    // It is written, and the XML side round-trips it.
    Doc * back = parse(out.bytes);
    ASSERT_NE(back, nullptr);
    EXPECT_TRUE(trees_equal(b.doc, back)) << out.bytes;
    gtext_markup_document_free(back);

    Opts strict = xopts();
    strict.xhtml_strict = true;
    const Out refused = write_with(b.doc, strict);
    EXPECT_EQ(refused.status, GTEXT_XML_E_XHTML);
    EXPECT_EQ(refused.r.xhtml_hazards, 1u);
    EXPECT_EQ(refused.r.xhtml_hazard_kind, c.kind);
    EXPECT_EQ(refused.bytes, "");
    EXPECT_STRNE(refused.r.message, "");
    // The refusal names the node that is the first hazard.
    EXPECT_STREQ(refused.r.path, c.path);
  }
}

TEST(XhtmlWriter, EveryBlockLevelNameInsideAPIsAHazardAndNoInlineNameIs) {
  const char * const block[] = {"address", "article", "aside", "blockquote", "center", "details",
      "dialog", "dir", "div", "dd", "dl", "dt", "fieldset", "figcaption", "figure", "footer", "form",
      "h1", "h2", "h3", "h4", "h5", "h6", "header", "hgroup", "hr", "li", "listing", "main", "menu",
      "nav", "ol", "p", "plaintext", "pre", "search", "section", "summary", "table", "ul", "xmp"};
  const char * const inline_names[] = {"span", "a", "b", "i", "em", "strong", "code", "img", "br",
      "input", "script", "style", "caption", "td", "tr", "tbody", "option", "select", "custom-el", "svgx"};
  for (const char * n : block) {
    Build b;
    Node * html = html_root(b, true);
    b.el(b.el(b.el(html, "body"), "p"), n);
    const Out out = write_x(b.doc);
    ASSERT_EQ(out.status, GTEXT_XML_OK) << n;
    EXPECT_EQ(out.r.xhtml_hazard_kind, GTEXT_XML_XHTML_HAZARD_P_BLOCK) << n;
  }
  for (const char * n : inline_names) {
    Build b;
    Node * html = html_root(b, true);
    b.el(b.el(b.el(html, "body"), "p"), n);
    const Out out = write_x(b.doc);
    ASSERT_EQ(out.status, GTEXT_XML_OK) << n;
    EXPECT_NE(out.r.xhtml_hazard_kind, GTEXT_XML_XHTML_HAZARD_P_BLOCK) << n;
  }
}

TEST(XhtmlWriter, WellFormedHtmlStructureHasNoHazards) {
  Build b;
  Node * html = html_root(b, true);
  Node * head = gtext_markup_node_first_child(html);
  b.text(b.el(head, "title"), "T & T");
  b.text(b.el(head, "style"), "p > a { }");
  b.text(b.el(head, "script"), "var a = 1 > 0;");
  b.el(head, "meta");
  b.el(head, "link");
  Node * body = b.el(html, "body");
  Node * table = b.el(body, "table");
  b.text(table, "\n  ");
  Node * thead = b.el(table, "thead");
  b.el(b.el(thead, "tr"), "th");
  Node * tbody = b.el(table, "tbody");
  Node * tr = b.el(tbody, "tr");
  b.text(b.el(tr, "td"), "cell");
  b.comment(tbody, "note");
  b.el(table, "tfoot");
  Node * ul = b.el(body, "ul");
  b.el(b.el(ul, "li"), "ul");  // li > ul > li is fine
  b.el(b.el(b.el(b.el(ul, "li"), "ul"), "li"), "span");
  Node * p = b.el(body, "p");
  b.text(p, "text ");
  b.text(b.el(p, "a"), "link");
  b.el(p, "br");
  b.el(body, "div");
  Node * dl = b.el(body, "dl");
  b.el(dl, "dt");
  b.el(dl, "dd");
  Node * sel = b.el(body, "select");
  b.el(sel, "option");
  b.el(b.el(sel, "optgroup"), "option");
  Node * form = b.el(body, "form");
  b.el(form, "input");
  b.el(b.el(body, "pre"), "code");
  Opts strict = xopts();
  strict.xhtml_strict = true;
  const Out out = write_with(b.doc, strict);
  ASSERT_EQ(out.status, GTEXT_XML_OK) << out.r.message << " " << out.r.path;
  EXPECT_EQ(out.r.xhtml_hazards, 0u);
  Doc * back = parse(out.bytes);
  ASSERT_NE(back, nullptr);
  EXPECT_TRUE(trees_equal(b.doc, back));
  gtext_markup_document_free(back);
}

TEST(XhtmlWriter, StrictWithoutXhtmlIsAnInvalidOption) {
  Build b;
  html_root(b);
  Opts o = gtext_xml_write_options_default();
  o.xhtml_strict = true;
  const Out out = write_with(b.doc, o);
  EXPECT_EQ(out.status, GTEXT_XML_E_INVALID);
  EXPECT_STREQ(out.r.message, "xhtml_strict needs xhtml");
}

TEST(XhtmlWriter, PrettyPrintingLeavesWhiteSensitiveElementsAlone) {
  Build b;
  Node * html = html_root(b);
  Node * body = b.el(html, "body");
  Node * pre = b.el(body, "pre");
  b.el(pre, "code");
  b.el(pre, "b");
  Node * ta = b.el(body, "textarea");
  b.comment(ta, "c");  // a hazard, but must not be indented either
  b.el(b.el(body, "div"), "p");
  Opts o = xopts();
  o.indent = "  ";
  const Out out = write_with(b.doc, o);
  ASSERT_EQ(out.status, GTEXT_XML_OK) << out.r.message;
  EXPECT_NE(out.bytes.find("<pre><code></code><b></b></pre>"), std::string::npos) << out.bytes;
  EXPECT_NE(out.bytes.find("<textarea><!--c--></textarea>"), std::string::npos) << out.bytes;
  EXPECT_NE(out.bytes.find("<div>\n"), std::string::npos) << out.bytes;
  EXPECT_EQ(out.r.elements_skipped_preserve, 2u);
}

// ---- without the option, nothing changes ---------------------------------

TEST(XhtmlWriter, WithoutTheOptionXhtmlShapedTreesAreWrittenAsPlainXml) {
  Build b;
  Node * html = html_root(b);
  Node * body = b.el(html, "body");
  b.el(body, "br");
  b.el(body, "script");
  b.text(b.el(body, "style"), "a > b");
  b.text(b.el(body, "pre"), "\nx");
  b.pi(body, "p", "d");
  b.cdata(b.el(body, "p"), "c");
  b.comment(body, ">");
  b.el(b.el(b.el(body, "table"), "tr"), "td");
  const Opts d = gtext_xml_write_options_default();
  const Out out = write_with(b.doc, d);
  ASSERT_EQ(out.status, GTEXT_XML_OK);
  EXPECT_EQ(out.bytes,
      "<html xmlns=\"" XHTML "\"><body><br/><script/><style>a &gt; b</style><pre>\nx</pre>"
      "<?p d?><p><![CDATA[c]]></p><!-->--><table><tr><td/></tr></table></body></html>");
  EXPECT_EQ(out.r.xhtml_hazards, 0u);
  EXPECT_EQ(out.r.xhtml_hazard_kind, GTEXT_XML_XHTML_HAZARD_NONE);
  EXPECT_STREQ(out.r.xhtml_hazard_path, "");
  Opts zero;
  std::memset(&zero, 0, sizeof(zero));
  EXPECT_EQ(write_with(b.doc, zero).bytes, out.bytes);
  Opts off = d;
  off.xhtml = false;
  off.xhtml_strict = false;
  EXPECT_EQ(write_with(b.doc, off).bytes, out.bytes);
}

// ---- the XML-side round trip ---------------------------------------------

const char * const kXhtmlDocs[] = {
    "<html xmlns=\"" XHTML "\"><head><title>t &amp; u</title></head><body><p>a<br/>b</p></body></html>",
    "<html xmlns=\"" XHTML "\"><body><img src=\"a.png\" alt=\"x &lt; y\"/><hr/><input type=\"text\" disabled=\"disabled\"/></body></html>",
    "<html xmlns=\"" XHTML "\"><head><style>a > b { c: d }</style><script>if (1 > 0) f();</script></head><body/></html>",
    "<html xmlns=\"" XHTML "\"><body><pre>line 1\nline 2\n</pre><textarea>x\n</textarea></body></html>",
    "<!DOCTYPE html>\n<html xmlns=\"" XHTML "\"><body><!-- note --><div><span>&#x20AC; \xC3\xA9</span></div></body></html>",
    "<html xmlns=\"" XHTML "\"><body><svg xmlns=\"" SVG "\" viewBox=\"0 0 1 1\"><path d=\"M0 0\"/><text><![CDATA[a < b]]></text></svg>"
    "<math xmlns=\"" MATHML "\"><mi>x</mi></math></body></html>",
    "<html xmlns=\"" XHTML "\" xml:lang=\"en\" lang=\"en\"><body><table><tbody><tr><td>1</td><td>2</td></tr></tbody></table>"
    "<ul><li>a</li><li>b<ul><li>c</li></ul></li></ul><dl><dt>t</dt><dd>d</dd></dl></body></html>",
    "<html xmlns=\"" XHTML "\"><body><form action=\"/x\"><select><option selected=\"selected\">a</option><option>b</option></select>"
    "<button disabled=\"disabled\">go</button></form></body></html>",
    "<div xmlns=\"" XHTML "\"/>",
};

TEST(XhtmlWriter, ParsedXhtmlRoundTripsThroughTheXmlParser) {
  for (const char * text : kXhtmlDocs) {
    SCOPED_TRACE(text);
    Doc * a = parse(text);
    ASSERT_NE(a, nullptr);
    const Out out = write_x(a);
    ASSERT_EQ(out.status, GTEXT_XML_OK) << out.r.message << " " << out.r.path;
    Doc * b = parse(out.bytes);
    ASSERT_NE(b, nullptr) << out.bytes;
    EXPECT_TRUE(trees_equal(a, b)) << out.bytes;
    EXPECT_TRUE(html_equivalent(gtext_markup_document_root(a), gtext_markup_document_root(b)));
    // Writing the reread tree gives the same bytes.
    EXPECT_EQ(write_x(b).bytes, out.bytes);
    gtext_markup_document_free(a);
    gtext_markup_document_free(b);
  }
}

TEST(XhtmlWriter, EveryEncodingAndDeclarationModeRoundTripsAnAcceptedDocument) {
  Doc * a = parse(kXhtmlDocs[4]);
  ASSERT_NE(a, nullptr);
  for (GTEXT_XML_Output_Encoding enc : {GTEXT_XML_OUTPUT_UTF8, GTEXT_XML_OUTPUT_UTF16LE,
           GTEXT_XML_OUTPUT_UTF16BE, GTEXT_XML_OUTPUT_ISO_8859_1, GTEXT_XML_OUTPUT_US_ASCII}) {
    for (GTEXT_XML_Declaration_Mode m : {GTEXT_XML_DECLARATION_AUTO, GTEXT_XML_DECLARATION_ALWAYS}) {
      Opts o = xopts();
      o.encoding = enc;
      o.xml_declaration = m;
      const Out out = write_with(a, o);
      ASSERT_EQ(out.status, GTEXT_XML_OK) << enc << " " << m;
      Doc * b = nullptr;
      GTEXT_XML_Error err;
      ASSERT_EQ(gtext_xml_parse(out.bytes.data(), out.bytes.size(), nullptr, &b, &err), GTEXT_XML_OK)
          << enc << " " << (err.message ? err.message : "");
      EXPECT_TRUE(trees_equal(a, b)) << enc;
      gtext_markup_document_free(b);
    }
  }
  gtext_markup_document_free(a);
}

void collect_xml_files(const std::string & dir, std::vector<std::string> & out) {
  DIR * d = opendir(dir.c_str());
  if (!d) {
    return;
  }
  std::vector<std::string> names;
  while (dirent * e = readdir(d)) {
    if (e->d_name[0] != '.') {
      names.push_back(e->d_name);
    }
  }
  closedir(d);
  std::sort(names.begin(), names.end());
  for (const std::string & n : names) {
    const std::string path = dir + "/" + n;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      collect_xml_files(path, out);
    }
    else if (n.size() > 4 && n.compare(n.size() - 4, 4, ".xml") == 0) {
      out.push_back(path);
    }
  }
}

TEST(XhtmlWriter, EveryWellFormedXmlconfDocumentThatTheModeAcceptsRoundTrips) {
  std::vector<std::string> files;
  collect_xml_files("third_party/xml/xmlconf", files);
  if (files.empty()) {
    FAIL() << "third_party/xml/xmlconf is not fetched: run tools/xml/fetch.sh";
  }
  size_t parsed = 0, accepted = 0, refused = 0, other = 0, hazardous = 0;
  for (const std::string & path : files) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    Doc * a = nullptr;
    GTEXT_XML_Error err;
    if (gtext_xml_parse(text.data(), text.size(), nullptr, &a, &err) != GTEXT_XML_OK) {
      gtext_xml_error_free(&err);
      continue;
    }
    parsed++;
    const Out out = write_x(a);
    if (out.status == GTEXT_XML_OK) {
      accepted++;
      Doc * b = nullptr;
      GTEXT_XML_Error err2;
      ASSERT_EQ(gtext_xml_parse(out.bytes.data(), out.bytes.size(), nullptr, &b, &err2), GTEXT_XML_OK)
          << path << "\n" << out.bytes;
      EXPECT_TRUE(trees_equal(a, b)) << path;
      gtext_markup_document_free(b);
      // Strict accepts exactly the documents without hazards. (None of these is
      // HTML, so nearly all have the hazard of a root that is not html.)
      Opts strict = xopts();
      strict.xhtml_strict = true;
      EXPECT_EQ(write_with(a, strict).status == GTEXT_XML_OK, out.r.xhtml_hazards == 0) << path;
      if (out.r.xhtml_hazards != 0) {
        hazardous++;
      }
    }
    else if (out.status == GTEXT_XML_E_XHTML) {
      refused++;
      EXPECT_EQ(out.bytes, "");
      EXPECT_STRNE(out.r.message, "");
    }
    else {
      // The plain writer's own refusals: whatever it says about this document.
      other++;
      Opts plain = gtext_xml_write_options_default();
      EXPECT_NE(write_with(a, plain).status, GTEXT_XML_OK) << path;
    }
    gtext_markup_document_free(a);
  }
  // The suite is thousands of documents, nearly all well formed here or not
  // (the rest are the not-well-formed ones the parser refuses); the mode must
  // accept the great majority of what parses, and every other outcome is a
  // refusal it explained. A drop in these ratios means the mode got stricter.
  EXPECT_GT(parsed, 1000u);
  EXPECT_EQ(parsed, accepted + refused + other);
  EXPECT_GE(accepted * 10, parsed * 9);
  EXPECT_GT(refused, 0u);  // processing instructions are common in the suite
  EXPECT_GT(hazardous, accepted / 2);
  std::fprintf(stderr, "xmlconf: %zu parsed, %zu accepted, %zu refused by xhtml, %zu other\n", parsed,
      accepted, refused, other);
}

// ---- generated trees -----------------------------------------------------

struct Gen {
  std::mt19937_64 rng;
  explicit Gen(uint64_t seed) : rng(seed) {}
  size_t pick(size_t n) {
    return static_cast<size_t>(rng() % n);
  }
  std::string text() {
    static const char * const pool[] = {"x", "hello", "a < b", "a & b", "]]>", "\n", "\nx", "x\n", " ",
        "\t", "&amp;", "\xC3\xA9", "\xE2\x82\xAC", ">", "'q\"", "a > b", "--", "\r"};
    std::string s;
    for (size_t i = 0, n = 1 + pick(2); i < n; i++) {
      s += pool[pick(sizeof(pool) / sizeof(pool[0]))];
    }
    return s;
  }
  void fill(Build & b, Node * parent, int depth) {
    static const char * const names[] = {"div", "p", "span", "br", "hr", "img", "pre", "script", "style",
        "textarea", "title", "table", "tbody", "tr", "td", "th", "a", "form", "li", "ul", "dt", "dd",
        "option", "select", "b", "h1", "h2", "input", "listing", "xmp", "custom-el"};
    const size_t kids = depth > 4 ? 0 : pick(4);
    bool last_text = false;
    for (size_t i = 0; i < kids; i++) {
      const size_t what = pick(10);
      if (what < 6 && depth < 5) {
        Node * e = b.el(parent, names[pick(sizeof(names) / sizeof(names[0]))]);
        if (pick(4) == 0) {
          b.attr(e, "class", text());
        }
        if (pick(8) == 0) {
          b.attr(e, "disabled", "disabled");
        }
        fill(b, e, depth + 1);
        last_text = false;
      }
      else if (what < 8) {
        if (!last_text) {
          b.text(parent, text());
          last_text = true;
        }
      }
      else if (what == 8) {
        static const char * const comments[] = {"c", ">", "->", "x-", "a>b", "", " n "};
        b.comment(parent, comments[pick(7)]);
        last_text = false;
      }
      else if (pick(2) == 0) {
        b.cdata(parent, "d");
        last_text = false;
      }
      else {
        b.pi(parent, "t", "d");
        last_text = false;
      }
    }
  }
};

TEST(XhtmlWriter, RandomTreesAreAcceptedAndRoundTripOrRefusedAndNeverWritten) {
  size_t accepted = 0, refused_x = 0, refused_other = 0, with_hazards = 0;
  for (uint64_t seed = 1; seed <= 4000; seed++) {
    Gen g(seed);
    Build b;
    Node * html = html_root(b, true);
    g.fill(b, b.el(html, "body"), 0);
    const Out out = write_x(b.doc);
    if (out.status == GTEXT_XML_OK) {
      accepted++;
      Doc * back = parse(out.bytes);
      ASSERT_NE(back, nullptr) << seed << "\n" << out.bytes;
      ASSERT_TRUE(trees_equal(b.doc, back)) << seed << "\n" << out.bytes;
      ASSERT_TRUE(html_equivalent(gtext_markup_document_root(b.doc), gtext_markup_document_root(back)));
      // No raw-text marker leaks: no CDATA, no <br/>, no <x/> for an HTML element.
      ASSERT_EQ(out.bytes.find("<br/>"), std::string::npos) << out.bytes;
      ASSERT_EQ(out.bytes.find("<script/>"), std::string::npos);
      gtext_markup_document_free(back);
      Opts strict = xopts();
      strict.xhtml_strict = true;
      const Out s = write_with(b.doc, strict);
      if (out.r.xhtml_hazards == 0) {
        ASSERT_EQ(s.status, GTEXT_XML_OK) << seed;
        ASSERT_EQ(s.bytes, out.bytes);
      }
      else {
        with_hazards++;
        ASSERT_EQ(s.status, GTEXT_XML_E_XHTML) << seed;
        ASSERT_EQ(s.r.xhtml_hazard_kind, out.r.xhtml_hazard_kind);
        ASSERT_STREQ(s.r.xhtml_hazard_path, out.r.xhtml_hazard_path);
        ASSERT_NE(out.r.xhtml_hazard_kind, GTEXT_XML_XHTML_HAZARD_NONE);
        ASSERT_STRNE(out.r.xhtml_hazard_path, "");
      }
    }
    else {
      ASSERT_EQ(out.bytes, "");
      ASSERT_NE(std::string(out.r.message), "");
      if (out.status == GTEXT_XML_E_XHTML) {
        refused_x++;
        ASSERT_STRNE(out.r.path, "") << seed;
      }
      else {
        refused_other++;
      }
    }
  }
  EXPECT_GT(accepted, 100u);
  EXPECT_GT(refused_x, 100u);
  EXPECT_GT(with_hazards, 20u);
  std::fprintf(stderr, "generated: %zu accepted (%zu with hazards), %zu xhtml refusals, %zu other refusals\n",
      accepted, with_hazards, refused_x, refused_other);
}

// ---- the comparison function, against the XML side -------------------------

TEST(XhtmlWriter, HtmlEquivalentIgnoresOnlyTheKnownRepresentationDifferences) {
  Doc * base = parse("<html xmlns=\"" XHTML "\"><body a=\"1\" b=\"2\"><p>x<!--c--></p></body></html>");
  ASSERT_NE(base, nullptr);
  const Node * br = gtext_markup_document_root(base);
  // Itself.
  EXPECT_TRUE(html_equivalent(br, br));
  // Attribute order, a prefix choice, extra declarations, split text.
  Doc * same = parse("<h:html xmlns:h=\"" XHTML "\" xmlns:z=\"urn:z\"><h:body b=\"2\" a=\"1\"><h:p>x<!--c--></h:p></h:body></h:html>");
  EXPECT_TRUE(html_equivalent(br, gtext_markup_document_root(same)));
  EXPECT_FALSE(trees_equal(base, same));
  Doc * split = parse("<html xmlns=\"" XHTML "\"><body a=\"1\" b=\"2\"><p>x<![CDATA[]]><!--c--></p></body></html>");
  EXPECT_TRUE(html_equivalent(br, gtext_markup_document_root(split)));
  Doc * merged = parse("<html xmlns=\"" XHTML "\"><body a=\"1\" b=\"2\"><p>x<![CDATA[y]]><!--c--></p></body></html>");
  EXPECT_FALSE(html_equivalent(br, gtext_markup_document_root(merged)));  // text differs
  // The HTML side's spelling of the declarations: plain attributes.
  {
    Build plain;
    Node * html = plain.el(nullptr, "html");
    plain.attr(html, "xmlns", XHTML);
    Node * body = plain.el(html, "body");
    plain.attr(body, "a", "1");
    plain.attr(body, "b", "2");
    Node * p = plain.el(body, "p");
    plain.text(p, "x");
    plain.comment(p, "c");
    EXPECT_TRUE(html_equivalent(br, gtext_markup_document_root(plain.doc)));
  }
  // Everything else is a difference.
  for (const char * other : {
           "<html xmlns=\"" XHTML "\"><body a=\"1\" b=\"3\"><p>x<!--c--></p></body></html>",
           "<html xmlns=\"" XHTML "\"><body a=\"1\"><p>x<!--c--></p></body></html>",
           "<html xmlns=\"" XHTML "\"><body a=\"1\" b=\"2\"><p>x<!--d--></p></body></html>",
           "<html xmlns=\"" XHTML "\"><body a=\"1\" b=\"2\"><p>x</p></body></html>",
           "<html xmlns=\"" XHTML "\"><body a=\"1\" b=\"2\"><div>x<!--c--></div></body></html>",
           "<html xmlns=\"urn:other\"><body a=\"1\" b=\"2\"><p>x<!--c--></p></body></html>",
           "<html xmlns=\"" XHTML "\"><body a=\"1\" b=\"2\"><p>x<!--c--><?pi?></p></body></html>",
           "<html xmlns=\"" XHTML "\"><body a=\"1\" b=\"2\"><p>x<!--c--></p></body><br/></html>",
       }) {
    Doc * d = parse(other);
    ASSERT_NE(d, nullptr) << other;
    EXPECT_FALSE(html_equivalent(br, gtext_markup_document_root(d))) << other;
    gtext_markup_document_free(d);
  }
  gtext_markup_document_free(base);
  gtext_markup_document_free(same);
  gtext_markup_document_free(split);
  gtext_markup_document_free(merged);
}


// ---- the hazard list beyond the first one -------------------------------------

Node * std_body(Build & b) {
  return b.el(html_root(b, true), "body");
}

struct Expect {
  const char * what;
  std::function<void(Build &, Node *)> build;  // given the body
  GTEXT_XML_Xhtml_Hazard kind;                  // NONE: no hazard at all
  const char * path;
};

void check_hazard_cases(const std::vector<Expect> & cases) {
  for (const Expect & c : cases) {
    SCOPED_TRACE(c.what);
    Build b;
    Node * body = std_body(b);
    c.build(b, body);
    const Out out = write_x(b.doc);
    ASSERT_EQ(out.status, GTEXT_XML_OK) << out.r.message << " " << out.r.path;
    EXPECT_EQ(out.r.xhtml_hazard_kind, c.kind);
    EXPECT_STREQ(out.r.xhtml_hazard_path, c.path);
    EXPECT_EQ(out.r.xhtml_hazards != 0, c.kind != GTEXT_XML_XHTML_HAZARD_NONE);
    Opts strict = xopts();
    strict.xhtml_strict = true;
    const Out s = write_with(b.doc, strict);
    EXPECT_EQ(s.status, c.kind == GTEXT_XML_XHTML_HAZARD_NONE ? GTEXT_XML_OK : GTEXT_XML_E_XHTML);
    if (out.status == GTEXT_XML_OK) {
      Doc * back = parse(out.bytes);
      ASSERT_NE(back, nullptr);
      EXPECT_TRUE(trees_equal(b.doc, back)) << out.bytes;  // the XML side still holds
      gtext_markup_document_free(back);
    }
  }
}

TEST(XhtmlWriter, ElementNamesHtmlWouldFoldOrNotStartAreRefused) {
  struct Case {
    const char * what;
    std::function<void(Build &, Node *)> build;
    const char * path;
  };
  const std::vector<Case> cases = {
      {"BR", [](Build & b, Node * body) { b.el(body, "BR"); }, "/html/body/BR"},
      {"Br with children", [](Build & b, Node * body) { b.text(b.el(body, "Br"), "x"); }, "/html/body/Br"},
      {"DIV", [](Build & b, Node * body) { b.el(body, "DIV"); }, "/html/body/DIV"},
      {"mixed case", [](Build & b, Node * body) { b.el(body, "myElement"); }, "/html/body/myElement"},
      {"attribute upper", [](Build & b, Node * body) { b.attr(b.el(body, "p"), "CLASS", "x"); }, "/html/body/p"},
      {"attribute mixed", [](Build & b, Node * body) { b.attr(b.el(body, "p"), "onClick", "x"); }, "/html/body/p"},
      {"underscore start", [](Build & b, Node * body) { b.el(body, "_x"); }, "/html/body/_x"},
      {"non-ascii start", [](Build & b, Node * body) { b.el(body, "\xC3\xA9t"); }, "/html/body/\xC3\xA9t"},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.what);
    Build b;
    Node * body = std_body(b);
    c.build(b, body);
    const Out out = write_x(b.doc);
    EXPECT_EQ(out.status, GTEXT_XML_E_XHTML);
    EXPECT_STREQ(out.r.path, c.path);
    EXPECT_EQ(out.bytes, "");
  }
  // The same names are fine in plain XML, and in another namespace.
  Build b;
  Node * body = std_body(b);
  b.el(body, "BR");
  EXPECT_EQ(write_with(b.doc, gtext_xml_write_options_default()).status, GTEXT_XML_OK);
  Build s;
  Node * svg = s.el(nullptr, "svg", SVG);
  s.xmlns(svg, nullptr, SVG);
  s.el(svg, "foreignObject", SVG);
  s.attr(svg, "viewBox", "0 0 1 1");
  EXPECT_EQ(write_x(s.doc).status, GTEXT_XML_OK);
  // Digits, hyphens and lower case after the first letter are fine.
  Build ok;
  Node * ob = std_body(ok);
  ok.el(ob, "h1");
  ok.el(ob, "my-element2");
  EXPECT_EQ(write_x(ok.doc).status, GTEXT_XML_OK);
}

TEST(XhtmlWriter, DocumentStructureHtmlWouldChangeIsAHazard) {
  const GTEXT_XML_Xhtml_Hazard S = GTEXT_XML_XHTML_HAZARD_DOCUMENT_STRUCTURE;
  struct Case {
    const char * what;
    std::function<void(Build &)> build;
    GTEXT_XML_Xhtml_Hazard kind;
    const char * path;
  };
  const std::vector<Case> cases = {
      {"complete", [](Build & b) { b.el(html_root(b, true), "body"); }, GTEXT_XML_XHTML_HAZARD_NONE, ""},
      {"comments are fine", [](Build & b) {
         Node * h = html_root(b, true);
         b.comment(h, "c");
         b.el(h, "body");
         b.comment(h, "d");
       }, GTEXT_XML_XHTML_HAZARD_NONE, ""},
      {"head content", [](Build & b) {
         Node * h = html_root(b);
         Node * head = b.el(h, "head");
         b.el(head, "meta");
         b.text(b.el(head, "title"), "t");
         b.el(head, "link");
         b.el(head, "style");
         b.el(head, "script");
         b.text(head, "  ");
         b.comment(head, "c");
         b.el(h, "body");
       }, GTEXT_XML_XHTML_HAZARD_NONE, ""},
      {"root is a div", [](Build & b) { b.xmlns(b.el(nullptr, "div"), nullptr, XHTML); }, S, "/div"},
      {"root is not html, no namespace", [](Build & b) { b.el(nullptr, "doc", nullptr); }, S, "/doc"},
      {"no head", [](Build & b) { b.el(html_root(b), "body"); }, S, "/html/body"},
      {"no body", [](Build & b) { html_root(b, true); }, S, "/html"},
      {"empty html", [](Build & b) { html_root(b); }, S, "/html"},
      {"body before head", [](Build & b) {
         Node * h = html_root(b);
         b.el(h, "body");
         b.el(h, "head");
       }, S, "/html/body"},
      {"two bodies", [](Build & b) {
         Node * h = html_root(b, true);
         b.el(h, "body");
         b.el(h, "body");
       }, S, "/html/body[2]"},
      {"text in html", [](Build & b) {
         Node * h = html_root(b, true);
         b.text(h, "x");
         b.el(h, "body");
       }, S, "/html/text()"},
      {"whitespace in html", [](Build & b) {
         Node * h = html_root(b, true);
         b.text(h, " ");
         b.el(h, "body");
       }, S, "/html/text()"},
      {"element in html", [](Build & b) {
         Node * h = html_root(b, true);
         b.el(h, "body");
         b.el(h, "p");
       }, S, "/html/p"},
      {"nested html", [](Build & b) { b.el(std_body(b), "html"); }, S, "/html/body/html"},
      {"head in body", [](Build & b) { b.el(std_body(b), "head"); }, S, "/html/body/head"},
      {"body in body", [](Build & b) { b.el(std_body(b), "body"); }, S, "/html/body/body"},
      {"div closes the head", [](Build & b) {
         Node * h = html_root(b);
         b.el(b.el(h, "head"), "div");
         b.el(h, "body");
       }, S, "/html/head/div"},
      {"text closes the head", [](Build & b) {
         Node * h = html_root(b);
         b.text(b.el(h, "head"), "x");
         b.el(h, "body");
       }, S, "/html/head/text()"},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.what);
    Build b;
    c.build(b);
    const Out out = write_x(b.doc);
    ASSERT_EQ(out.status, GTEXT_XML_OK) << out.r.message << " " << out.r.path;
    EXPECT_EQ(out.r.xhtml_hazard_kind, c.kind);
    EXPECT_STREQ(out.r.xhtml_hazard_path, c.path);
    Opts strict = xopts();
    strict.xhtml_strict = true;
    EXPECT_EQ(write_with(b.doc, strict).status,
        c.kind == GTEXT_XML_XHTML_HAZARD_NONE ? GTEXT_XML_OK : GTEXT_XML_E_XHTML);
  }
}

TEST(XhtmlWriter, ImpliedEndsAreFoundThroughTheAncestorChain) {
  const GTEXT_XML_Xhtml_Hazard E = GTEXT_XML_XHTML_HAZARD_IMPLIED_END;
  const GTEXT_XML_Xhtml_Hazard N = GTEXT_XML_XHTML_HAZARD_NONE;
  check_hazard_cases({
      {"li > li", [](Build & b, Node * body) { b.el(b.el(b.el(body, "ul"), "li"), "li"); }, E, "/html/body/ul/li/li"},
      {"li > div > li", [](Build & b, Node * body) { b.el(b.el(b.el(b.el(body, "ul"), "li"), "div"), "li"); }, E,
          "/html/body/ul/li/div/li"},
      {"li > span > li", [](Build & b, Node * body) { b.el(b.el(b.el(b.el(body, "ul"), "li"), "span"), "li"); }, E,
          "/html/body/ul/li/span/li"},
      {"li > p > span > li", [](Build & b, Node * body) {
         b.el(b.el(b.el(b.el(b.el(body, "ul"), "li"), "span"), "b"), "li");
       }, E, "/html/body/ul/li/span/b/li"},
      {"li > ul > li", [](Build & b, Node * body) { b.el(b.el(b.el(b.el(body, "ul"), "li"), "ul"), "li"); }, N, ""},
      {"li > form > li", [](Build & b, Node * body) { b.el(b.el(b.el(b.el(body, "ul"), "li"), "form"), "li"); }, N, ""},
      {"li > table > ... > li", [](Build & b, Node * body) {
         b.el(b.el(b.el(b.el(b.el(b.el(b.el(body, "ul"), "li"), "table"), "tbody"), "tr"), "td"), "li");
       }, N, ""},
      {"dd > div > dt", [](Build & b, Node * body) { b.el(b.el(b.el(b.el(body, "dl"), "dd"), "div"), "dt"); }, E,
          "/html/body/dl/dd/div/dt"},
      {"dt > span > dd", [](Build & b, Node * body) { b.el(b.el(b.el(b.el(body, "dl"), "dt"), "span"), "dd"); }, E,
          "/html/body/dl/dt/span/dd"},
      {"dd > dl > dt", [](Build & b, Node * body) { b.el(b.el(b.el(b.el(body, "dl"), "dd"), "dl"), "dt"); }, N, ""},
      {"option > option", [](Build & b, Node * body) { b.el(b.el(b.el(body, "select"), "option"), "option"); }, E,
          "/html/body/select/option/option"},
      {"p > span > p", [](Build & b, Node * body) { b.el(b.el(b.el(body, "p"), "span"), "p"); },
          GTEXT_XML_XHTML_HAZARD_P_BLOCK, "/html/body/p/span/p"},
      {"sibling lis", [](Build & b, Node * body) {
         Node * ul = b.el(body, "ul");
         b.el(ul, "li");
         b.el(ul, "li");
       }, N, ""},
  });
}

TEST(XhtmlWriter, ElementsHtmlRewritesOrIgnoresAreHazards) {
  const GTEXT_XML_Xhtml_Hazard X = GTEXT_XML_XHTML_HAZARD_SPECIAL_ELEMENT;
  const GTEXT_XML_Xhtml_Hazard N = GTEXT_XML_XHTML_HAZARD_NONE;
  check_hazard_cases({
      {"image", [](Build & b, Node * body) { b.el(body, "image"); }, X, "/html/body/image"},
      {"isindex", [](Build & b, Node * body) { b.el(body, "isindex"); }, X, "/html/body/isindex"},
      {"frameset", [](Build & b, Node * body) { b.el(body, "frameset"); }, X, "/html/body/frameset"},
      {"svg in the XHTML namespace", [](Build & b, Node * body) { b.el(body, "svg"); }, X, "/html/body/svg"},
      {"math in the XHTML namespace", [](Build & b, Node * body) { b.el(body, "math"); }, X, "/html/body/math"},
      {"template with children", [](Build & b, Node * body) { b.el(b.el(body, "template"), "div"); }, X,
          "/html/body/template"},
      {"empty template", [](Build & b, Node * body) { b.el(body, "template"); }, N, ""},
      {"button > button", [](Build & b, Node * body) { b.el(b.el(body, "button"), "button"); }, X,
          "/html/body/button/button"},
      {"button > span > button", [](Build & b, Node * body) {
         b.el(b.el(b.el(body, "button"), "span"), "button");
       }, X, "/html/body/button/span/button"},
      {"sibling buttons", [](Build & b, Node * body) {
         b.el(body, "button");
         b.el(body, "button");
       }, N, ""},
      {"colgroup > div", [](Build & b, Node * body) { b.el(b.el(b.el(body, "table"), "colgroup"), "div"); }, X,
          "/html/body/table/colgroup/div"},
      {"colgroup > col", [](Build & b, Node * body) { b.el(b.el(b.el(body, "table"), "colgroup"), "col"); }, N, ""},
      {"select > div", [](Build & b, Node * body) { b.el(b.el(body, "select"), "div"); }, X, "/html/body/select/div"},
      {"select > option, optgroup, hr", [](Build & b, Node * body) {
         Node * s = b.el(body, "select");
         b.el(s, "option");
         b.el(b.el(s, "optgroup"), "option");
         b.el(s, "hr");
       }, N, ""},
      {"td in a div", [](Build & b, Node * body) { b.el(b.el(body, "div"), "td"); }, X, "/html/body/div/td"},
      {"th in a div", [](Build & b, Node * body) { b.el(b.el(body, "div"), "th"); }, X, "/html/body/div/th"},
      {"tr in a div", [](Build & b, Node * body) { b.el(b.el(body, "div"), "tr"); }, X, "/html/body/div/tr"},
      {"tr in the body", [](Build & b, Node * body) { b.el(body, "tr"); }, X, "/html/body/tr"},
      {"caption in a div", [](Build & b, Node * body) { b.el(b.el(body, "div"), "caption"); }, X,
          "/html/body/div/caption"},
      {"caption in a tr", [](Build & b, Node * body) {
         b.el(b.el(b.el(b.el(body, "table"), "tbody"), "tr"), "caption");
       }, GTEXT_XML_XHTML_HAZARD_TABLE_FOSTER_PARENT, "/html/body/table/tbody/tr/caption"},
      {"col in a div", [](Build & b, Node * body) { b.el(b.el(body, "div"), "col"); }, X, "/html/body/div/col"},
      {"colgroup in a div", [](Build & b, Node * body) { b.el(b.el(body, "div"), "colgroup"); }, X,
          "/html/body/div/colgroup"},
      {"tbody in a div", [](Build & b, Node * body) { b.el(b.el(body, "div"), "tbody"); }, X, "/html/body/div/tbody"},
      {"col directly in table", [](Build & b, Node * body) { b.el(b.el(body, "table"), "col"); },
          GTEXT_XML_XHTML_HAZARD_TABLE_IMPLIED_PART, "/html/body/table/col"},
      {"caption in table", [](Build & b, Node * body) { b.el(b.el(body, "table"), "caption"); }, N, ""},
      {"tr in table", [](Build & b, Node * body) { b.el(b.el(body, "table"), "tr"); },
          GTEXT_XML_XHTML_HAZARD_TABLE_IMPLIED_PART, "/html/body/table/tr"},
      {"a whole table", [](Build & b, Node * body) {
         Node * t = b.el(body, "table");
         b.el(t, "caption");
         b.el(b.el(t, "colgroup"), "col");
         b.el(b.el(b.el(t, "thead"), "tr"), "th");
         b.el(b.el(b.el(t, "tbody"), "tr"), "td");
         b.el(t, "tfoot");
       }, N, ""},
      {"td in a tr in a template", [](Build & b, Node * body) { b.el(b.el(body, "template"), "tr"); }, X,
          "/html/body/template"},
  });
}

TEST(XhtmlWriter, ForeignContentHtmlWouldBuildDifferentlyIsAHazard) {
  const GTEXT_XML_Xhtml_Hazard F = GTEXT_XML_XHTML_HAZARD_FOREIGN_CONTENT;
  const GTEXT_XML_Xhtml_Hazard N = GTEXT_XML_XHTML_HAZARD_NONE;
  auto svg = [](Build & b, Node * parent) {
    Node * e = b.el(parent, "svg", SVG);
    b.xmlns(e, nullptr, SVG);
    return e;
  };
  auto math = [](Build & b, Node * parent) {
    Node * e = b.el(parent, "math", MATHML);
    b.xmlns(e, nullptr, MATHML);
    return e;
  };
  check_hazard_cases({
      {"svg > g > rect", [&](Build & b, Node * body) { b.el(b.el(svg(b, body), "g", SVG), "rect", SVG); }, N, ""},
      {"math > mi", [&](Build & b, Node * body) { b.text(b.el(math(b, body), "mi", MATHML), "x"); }, N, ""},
      {"svg > div", [&](Build & b, Node * body) { b.el(svg(b, body), "div", SVG); }, F, "/html/body/svg/div"},
      {"svg > p", [&](Build & b, Node * body) { b.el(svg(b, body), "p", SVG); }, F, "/html/body/svg/p"},
      {"svg > g > span", [&](Build & b, Node * body) { b.el(b.el(svg(b, body), "g", SVG), "span", SVG); }, F,
          "/html/body/svg/g/span"},
      {"svg > table", [&](Build & b, Node * body) { b.el(svg(b, body), "table", SVG); }, F, "/html/body/svg/table"},
      {"svg > font with color", [&](Build & b, Node * body) {
         b.attr(b.el(svg(b, body), "font", SVG), "color", "red");
       }, F, "/html/body/svg/font"},
      {"svg > font", [&](Build & b, Node * body) { b.el(svg(b, body), "font", SVG); }, N, ""},
      {"math > mi > b (breakout)", [&](Build & b, Node * body) { b.el(math(b, body), "b", MATHML); }, F,
          "/html/body/math/b"},
      {"math inside svg", [&](Build & b, Node * body) { math(b, svg(b, body)); }, F, "/html/body/svg/math"},
      {"svg inside math", [&](Build & b, Node * body) { svg(b, math(b, body)); }, F, "/html/body/math/svg"},
      {"rect with no svg", [](Build & b, Node * body) { b.xmlns(b.el(body, "rect", SVG), nullptr, SVG); }, F,
          "/html/body/rect"},
      {"mi with no math", [](Build & b, Node * body) { b.xmlns(b.el(body, "mi", MATHML), nullptr, MATHML); }, F,
          "/html/body/mi"},
      {"prefixed svg", [](Build & b, Node * body) {
         Node * e = b.el(body, "svg", SVG, "s");
         b.xmlns(e, "s", SVG);
       }, F, "/html/body/s:svg"},
      {"html inside svg", [&](Build & b, Node * body) {
         Node * g = b.el(svg(b, body), "g", SVG);
         b.xmlns(b.el(g, "p"), nullptr, XHTML);
       }, F, "/html/body/svg/g/p"},
      {"html inside foreignObject", [&](Build & b, Node * body) {
         Node * fo = b.el(svg(b, body), "foreignObject", SVG);
         b.xmlns(b.el(fo, "div"), nullptr, XHTML);
       }, N, ""},
      {"svg inside foreignObject", [&](Build & b, Node * body) { svg(b, b.el(svg(b, body), "foreignObject", SVG)); },
          N, ""},
      {"rect inside foreignObject", [&](Build & b, Node * body) {
         b.el(b.el(svg(b, body), "foreignObject", SVG), "rect", SVG);
       }, F, "/html/body/svg/foreignObject/rect"},
      {"html inside mi", [&](Build & b, Node * body) {
         b.xmlns(b.el(b.el(math(b, body), "mi", MATHML), "span"), nullptr, XHTML);
       }, N, ""},
      {"an element in no known namespace", [](Build & b, Node * body) {
         b.xmlns(b.el(body, "x", "urn:x"), nullptr, "urn:x");
       }, F, "/html/body/x"},
  });
}

TEST(XhtmlWriter, ACdataSectionNeedsRealForeignContentAbove) {
  auto build = [](Build & b, Node * parent, const char * ns, const char * name, bool xmlns) {
    Node * e = b.el(parent, name, ns);
    if (xmlns) {
      b.xmlns(e, nullptr, ns);
    }
    return e;
  };
  // A legitimate one.
  Build ok;
  Node * ob = std_body(ok);
  ok.cdata(ok.el(build(ok, ob, SVG, "svg", true), "text", SVG), "a<b");
  EXPECT_EQ(write_x(ok.doc).status, GTEXT_XML_OK);
  // Directly in svg.
  Build d;
  Node * db = std_body(d);
  d.cdata(build(d, db, SVG, "svg", true), "x");
  EXPECT_EQ(write_x(d.doc).status, GTEXT_XML_OK);
  // An SVG-named element that is not reachable from an svg element.
  Build lone;
  Node * lb = std_body(lone);
  lone.cdata(build(lone, lb, SVG, "text", true), "x");
  Out out = write_x(lone.doc);
  EXPECT_EQ(out.status, GTEXT_XML_E_XHTML);
  EXPECT_STREQ(out.r.path, "/html/body/text/cdata()");
  // Under a foreign element whose parent is HTML-namespaced but is not svg/math.
  Build under_div;
  Node * ub = std_body(under_div);
  under_div.cdata(build(under_div, under_div.el(ub, "div"), SVG, "text", true), "x");
  EXPECT_EQ(write_x(under_div.doc).status, GTEXT_XML_E_XHTML);
  // In an integration point.
  Build fo;
  Node * fb = std_body(fo);
  fo.cdata(fo.el(build(fo, fb, SVG, "svg", true), "foreignObject", SVG), "x");
  EXPECT_EQ(write_x(fo.doc).status, GTEXT_XML_E_XHTML);
  // Under a foreign element that is itself a hazard (breakout name): not foreign.
  Build bo;
  Node * bb = std_body(bo);
  bo.cdata(bo.el(build(bo, bb, SVG, "svg", true), "div", SVG), "x");
  EXPECT_EQ(write_x(bo.doc).status, GTEXT_XML_E_XHTML);
  // In an HTML element.
  Build h;
  h.cdata(std_body(h), "x");
  EXPECT_EQ(write_x(h.doc).status, GTEXT_XML_E_XHTML);
}

TEST(XhtmlWriter, TextInRawishElementsIsAHazardWhenItNeedsAnEscape) {
  const GTEXT_XML_Xhtml_Hazard R = GTEXT_XML_XHTML_HAZARD_RAW_TEXT_CONTENT;
  const GTEXT_XML_Xhtml_Hazard N = GTEXT_XML_XHTML_HAZARD_NONE;
  std::vector<Expect> cases;
  for (const char * name : {"iframe", "noembed", "noframes", "noscript", "plaintext", "xmp"}) {
    for (const char * bad : {"a<b", "a&b", "a>b", "a\rb"}) {
      cases.push_back({name, nullptr, R, nullptr});
      static std::vector<std::string> keep;
      keep.push_back(std::string("/html/body/") + name + "/text()");
      const std::string nm = name;
      const std::string text = bad;
      cases.back().build = [nm, text](Build & b, Node * body) { b.text(b.el(body, nm.c_str()), text); };
      cases.back().path = keep.back().c_str();
    }
    const std::string nm = name;
    cases.push_back({name, [nm](Build & b, Node * body) { b.text(b.el(body, nm.c_str()), "plain text"); }, N, ""});
  }
  check_hazard_cases(cases);
}

TEST(XhtmlWriter, NamespacedAttributesAndRedeclarationsOnXhtmlElementsAreHazards) {
  const GTEXT_XML_Xhtml_Hazard A = GTEXT_XML_XHTML_HAZARD_NAMESPACED_ATTRIBUTE;
  const GTEXT_XML_Xhtml_Hazard N = GTEXT_XML_XHTML_HAZARD_NONE;
  check_hazard_cases({
      {"xml:lang", [](Build & b, Node * body) { b.attr(b.el(body, "p"), "lang", "en", "http://www.w3.org/XML/1998/namespace", "xml"); }, A,
          "/html/body/p"},
      {"xlink:href", [](Build & b, Node * body) {
         Node * a = b.el(body, "a");
         b.xmlns(a, "x", "http://www.w3.org/1999/xlink");
         b.attr(a, "href", "u", "http://www.w3.org/1999/xlink", "x");
       }, A, "/html/body/a"},
      {"non-root xmlns:prefix", [](Build & b, Node * body) { b.xmlns(b.el(body, "div"), "q", "urn:q"); }, A,
          "/html/body/div"},
      {"non-root default redeclared to another namespace", [](Build & b, Node * body) {
         Node * d = b.el(body, "div", SVG);
         b.xmlns(d, nullptr, SVG);
       }, GTEXT_XML_XHTML_HAZARD_FOREIGN_CONTENT, "/html/body/div"},
      {"non-root default redeclared to XHTML", [](Build & b, Node * body) { b.xmlns(b.el(body, "div"), nullptr, XHTML); }, N,
          ""},
      {"plain attributes", [](Build & b, Node * body) { b.attr(b.el(body, "div"), "class", "x"); }, N, ""},
  });
}

TEST(XhtmlWriter, ARootPrefixDeclarationIsTheKnownRepresentationDifference) {
  Build b;
  Node * html = html_root(b, true);
  b.xmlns(html, "q", "urn:q");
  b.el(html, "body");
  const Out out = write_x(b.doc);
  ASSERT_EQ(out.status, GTEXT_XML_OK);
  EXPECT_EQ(out.r.xhtml_hazards, 0u);
}

TEST(XhtmlWriter, ADocumentTypeOtherThanHtmlIsAHazard) {
  struct Case {
    const char * name;
    const char * pub;
    const char * sys;
    bool hazard;
  };
  const Case cases[] = {
      {"html", nullptr, nullptr, false},
      {"HTML", nullptr, nullptr, true},
      {"html", "-//W3C//DTD XHTML 1.0 Strict//EN", "http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd", true},
      {"html", nullptr, "about:legacy-compat", true},
      {"svg", nullptr, nullptr, true},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(std::string(c.name) + (c.pub ? " pub" : "") + (c.sys ? " sys" : ""));
    Build b;
    Node * dt = nullptr;
    ASSERT_EQ(gtext_markup_create_doctype(b.doc, c.name, c.pub, c.sys, &dt), GTEXT_MARKUP_OK);
    ASSERT_EQ(gtext_markup_append_child(b.root(), dt), GTEXT_MARKUP_OK);
    b.el(html_root(b, true), "body");
    const Out out = write_x(b.doc);
    ASSERT_EQ(out.status, GTEXT_XML_OK) << out.r.message;
    EXPECT_EQ(out.r.xhtml_hazards, c.hazard ? 1u : 0u);
    if (c.hazard) {
      EXPECT_EQ(out.r.xhtml_hazard_kind, GTEXT_XML_XHTML_HAZARD_DOCTYPE);
      EXPECT_STREQ(out.r.xhtml_hazard_path, "/doctype()");
    }
  }
}

TEST(XhtmlWriter, TheHazardPathIsTruncatedAt255BytesKeepingItsEnd) {
  Build b;
  Node * at = std_body(b);
  for (int i = 0; i < 80; i++) {
    at = b.el(at, "section");
  }
  b.el(b.el(at, "ul"), "image");
  const Out out = write_x(b.doc);
  ASSERT_EQ(out.status, GTEXT_XML_OK);
  ASSERT_EQ(out.r.xhtml_hazard_kind, GTEXT_XML_XHTML_HAZARD_SPECIAL_ELEMENT);
  const std::string path = out.r.xhtml_hazard_path;
  EXPECT_LE(path.size(), 255u);
  EXPECT_EQ(path.rfind("...", 0), 0u);
  EXPECT_EQ(path.substr(path.size() - 9), "/ul/image");
}

// The vectors html5lib ships for its own XHTML serialization (the "xhtml" member)
// are independent of this implementation: void elements are `<img />`.
TEST(XhtmlWriter, HtmlFiveLibsXhtmlVectorsAgreeOnVoidElementsAndEmptyOnes) {
  std::ifstream in("third_party/html/html5lib-tests/serializer/core.test");
  if (!in) {
    FAIL() << "third_party/html/html5lib-tests is not fetched: run tools/html/fetch.sh";
  }
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string vectors = ss.str();
  EXPECT_NE(vectors.find("\"xhtml\":    [\"<img />\"]"), std::string::npos);
  Build b;
  Node * body = std_body(b);
  b.el(body, "img");
  const Out out = write_x(b.doc);
  ASSERT_EQ(out.status, GTEXT_XML_OK);
  EXPECT_NE(out.bytes.find("<img />"), std::string::npos);
  Build e;
  Node * h = html_root(e);
  e.el(h, "head");
  EXPECT_NE(write_x(e.doc).bytes.find("<head></head>"), std::string::npos);
}

// ---- failure at every point ------------------------------------------------

Doc * rich_xhtml() {
  return parse(
      "<!DOCTYPE html>\n<html xmlns=\"" XHTML "\"><head><title>t</title><style>a > b {}</style>"
      "<script>if (1 > 0) {}</script></head><body><table><tbody><tr><td>1</td></tr></tbody></table>"
      "<p>a<br/>b<img src=\"x\"/></p><pre>x\ny</pre><svg xmlns=\"" SVG "\"><text><![CDATA[a<b]]></text></svg>"
      "<!--c--></body></html>");
}

void sweep_oom(const Doc * doc, Opts o, GTEXT_XML_Status expected_ok, const char * label) {
  SCOPED_TRACE(label);
  FaultAllocator fa;
  o.allocator = &fa.alloc;
  size_t failures = 0;
  for (size_t n = 1;; n++) {
    fa.arm(n);
    char * buf = nullptr;
    size_t len = 0;
    GTEXT_XML_Write_Result r;
    const GTEXT_XML_Status st = gtext_xml_write_buffer(doc, &o, &buf, &len, &r);
    fa.disarm();
    if (!fa.failed) {
      ASSERT_EQ(st, expected_ok);
      if (st == GTEXT_XML_OK) {
        gtext_xml_write_buffer_free(doc, &o, buf);
      }
      break;
    }
    failures++;
    ASSERT_EQ(st, GTEXT_XML_E_OOM) << n;
    ASSERT_EQ(buf, nullptr);
    ASSERT_TRUE(fa.live.empty()) << n;
    ASSERT_EQ(fa.bad_frees, 0u);
    ASSERT_LT(n, 10000u);
  }
  EXPECT_GE(failures, 2u);
  EXPECT_TRUE(fa.live.empty());
  EXPECT_EQ(fa.bad_frees, 0u);
}

TEST(XhtmlWriter, TheAllocatorFailingAtEveryPointEndsTheWriteWithOomAndNothingLeaks) {
  Doc * doc = rich_xhtml();
  ASSERT_NE(doc, nullptr);
  Opts o = xopts();
  sweep_oom(doc, o, GTEXT_XML_OK, "xhtml");
  o.indent = "  ";
  sweep_oom(doc, o, GTEXT_XML_OK, "pretty");
  o.indent = nullptr;
  o.encoding = GTEXT_XML_OUTPUT_UTF16LE;
  sweep_oom(doc, o, GTEXT_XML_OK, "utf-16");
  o.encoding = GTEXT_XML_OUTPUT_UTF8;
  o.xhtml_strict = true;
  sweep_oom(doc, o, GTEXT_XML_OK, "strict");
  gtext_markup_document_free(doc);

  // The namespace added to the root costs allocations of its own.
  Build b;
  Node * html = b.el(nullptr, "html");
  b.el(b.el(html, "body"), "br");
  sweep_oom(b.doc, xopts(), GTEXT_XML_OK, "declared on the root");

  // A refusal at the end, with the allocator failing in turn: OOM or the refusal.
  Build bad;
  Node * r = html_root(bad);
  for (int i = 0; i < 30; i++) {
    bad.el(r, "i");
  }
  bad.text(bad.el(r, "script"), "<");
  FaultAllocator fa;
  Opts ro = xopts();
  ro.allocator = &fa.alloc;
  for (size_t n = 1; n < 40; n++) {
    fa.arm(n);
    const Out out = write_with(bad.doc, ro);
    fa.disarm();
    EXPECT_TRUE(out.status == GTEXT_XML_E_OOM || out.status == GTEXT_XML_E_XHTML);
    EXPECT_TRUE(fa.live.empty());
    EXPECT_EQ(fa.bad_frees, 0u);
  }
}

TEST(XhtmlWriter, ASinkThatFailsAtAnyCallEndsTheWriteWithWriteAndNothingLeaks) {
  // Larger than one buffer, so there are several calls to fail.
  Build b;
  Node * html = html_root(b);
  Node * body = b.el(html, "body");
  for (int i = 0; i < 400; i++) {
    Node * p = b.el(body, "p");
    b.text(p, "paragraph " + std::to_string(i) + " with some text in it");
    b.el(p, "br");
    b.el(p, "img");
  }
  b.text(b.el(html, "script"), "if (a > b) {}");
  size_t calls = 0;
  {
    Collect all;
    const GTEXT_Markup_Sink s = all.sink();
    Opts o = xopts();
    ASSERT_EQ(gtext_xml_write(&s, b.doc, &o, nullptr), GTEXT_XML_OK);
    calls = all.calls;
    EXPECT_GT(calls, 3u);
  }
  FaultAllocator fa;
  Opts o = xopts();
  o.allocator = &fa.alloc;
  for (size_t k = 1; k <= calls; k++) {
    Collect sink;
    sink.fail_on = k;
    const GTEXT_Markup_Sink s = sink.sink();
    GTEXT_XML_Write_Result r;
    EXPECT_EQ(gtext_xml_write(&s, b.doc, &o, &r), GTEXT_XML_E_WRITE) << k;
    EXPECT_EQ(sink.calls, k);
    EXPECT_TRUE(fa.live.empty()) << k;
    EXPECT_EQ(fa.bad_frees, 0u);
  }
}

TEST(XhtmlWriter, ARefusalWritesNothingToTheSink) {
  Build b;
  Node * html = html_root(b);
  Node * body = b.el(html, "body");
  for (int i = 0; i < 400; i++) {
    b.text(b.el(body, "p"), "paragraph " + std::to_string(i));
  }
  b.text(b.el(body, "script"), "a < b");  // the very last thing
  Collect sink;
  const GTEXT_Markup_Sink s = sink.sink();
  GTEXT_XML_Write_Result r;
  Opts o = xopts();
  EXPECT_EQ(gtext_xml_write(&s, b.doc, &o, &r), GTEXT_XML_E_XHTML);
  EXPECT_EQ(sink.calls, 0u);
  EXPECT_STREQ(r.path, "/html/body/script/text()");
  EXPECT_STREQ(gtext_xml_status_string(GTEXT_XML_E_XHTML), "not the same tree for an HTML parser");
}

TEST(XhtmlWriter, AVeryDeepTreeNeedsNoStack) {
  Build b;
  Node * html = html_root(b, true);
  Node * at = b.el(html, "body");
  for (int i = 0; i < 100000; i++) {
    at = b.el(at, i % 2 ? "span" : "div");
  }
  Opts o = xopts();
  o.max_depth = 200000;
  const Out out = write_with(b.doc, o);
  ASSERT_EQ(out.status, GTEXT_XML_OK);
  EXPECT_EQ(out.r.xhtml_hazards, 0u);
  EXPECT_NE(out.bytes.find("<span></span>"), std::string::npos);
}

}  // namespace
