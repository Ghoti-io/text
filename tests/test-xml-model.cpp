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
 * The XML parser against a model that shares none of its code.
 *
 * A generator builds a random document twice at once: as text in every
 * spelling XML allows (references in decimal and hex, line ends in all three
 * forms, quotes of both kinds, white space inside tags, CDATA sections, a
 * document type with a hostile internal subset) and as a tree in plain C++
 * that records what the parse must produce. The parser's tree is then walked
 * against the model's: names, namespaces, attribute order and values, text
 * after normalization, node kinds and their order.
 *
 * Then every proper prefix of each document that ends before the root element
 * does is parsed too, and each must be refused: a parser that accepts half of a
 * document has lost track of what it has read.
 *
 * The generator is seeded, so a failure names its seed and reproduces.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/xml.h>

#include "../src/markup/markup_internal.h"

namespace {

using Node = GTEXT_Markup_Node;

struct MAttr {
  std::string ns;      // empty: none
  std::string prefix;  // empty: none
  std::string local;
  std::string value;
};

struct MNode {
  enum Kind { ELEMENT, TEXT, CDATA, COMMENT, PI } kind = TEXT;
  std::string ns, prefix, local;  // element
  std::vector<MAttr> attrs;
  std::vector<MNode> kids;
  std::string data;    // text, cdata, comment, pi data
  std::string target;  // pi
};

static const char * const kXmlns = "http://www.w3.org/2000/xmlns/";

class Generator {
 public:
  explicit Generator(uint32_t seed) : rng(seed) {}

  std::string out;                // the document
  std::vector<MNode> top_before;  // comments and PIs before the root
  MNode root;
  std::vector<MNode> top_after;
  bool has_doctype = false;
  std::string doctype_public, doctype_system;
  bool has_public = false, has_system = false;
  size_t root_end = 0;  // out.size() just after the root element's end tag

  void generate() {
    if (chance(60)) {
      if (chance(10)) {
        out += "\xEF\xBB\xBF";
      }
      out += "<?xml version=";
      const char q = chance(50) ? '"' : '\'';
      out += q;
      out += "1.0";
      out += q;
      if (chance(50)) {
        out += " encoding=";
        out += q;
        out += chance(50) ? "UTF-8" : "utf-8";
        out += q;
      }
      if (chance(50)) {
        out += " standalone=";
        out += q;
        out += chance(50) ? "yes" : "no";
        out += q;
      }
      out += chance(50) ? "?>" : " ?>";
    }
    misc(top_before);
    if (chance(40)) {
      doctype();
      misc(top_before);
    }
    scope.clear();
    element(root, 0);
    root_end = out.size();
    misc(top_after);
  }

 private:
  std::mt19937 rng;
  std::vector<std::pair<std::string, std::string>> scope;  // prefix -> uri; "" = default

  bool chance(int percent) { return static_cast<int>(rng() % 100) < percent; }
  size_t below(size_t n) { return rng() % n; }

  void space() {
    static const char * const forms[] = {" ", "  ", "\n", "\r\n", "\t", "\r", " \n "};
    out += forms[below(7)];
  }
  // A line end in one of its three spellings. A lone CR followed by a LF is one
  // line end and not two, so a LF is not written straight after a CR: the model
  // would otherwise count two where the specification says one.
  std::string line_end() {
    static const char * const forms[] = {"\n", "\r\n", "\r"};
    size_t pick = below(3);
    if (pick == 0 && !out.empty() && out.back() == '\r') {
      pick = 1 + below(2);
    }
    return forms[pick];
  }

  void maybe_space() {
    if (chance(30)) {
      space();
    }
  }

  void misc(std::vector<MNode> & into) {
    while (chance(45)) {
      if (chance(50)) {
        maybe_space();
      }
      if (chance(50)) {
        MNode c;
        c.kind = MNode::COMMENT;
        comment_body(c);
        into.push_back(c);
      }
      else {
        MNode p;
        p.kind = MNode::PI;
        pi_body(p);
        into.push_back(p);
      }
    }
    maybe_space();
  }

  void comment_body(MNode & c) {
    out += "<!--";
    const size_t n = below(8);
    char prev = 0;
    for (size_t i = 0; i < n; i++) {
      static const char pool[] = "ab 1<&]>x-";
      char ch = pool[below(sizeof(pool) - 1)];
      if (ch == '-' && (prev == '-' || i + 1 == n)) {
        ch = 'z';
      }
      if (i + 1 == n && ch == '-') {
        ch = 'z';
      }
      if (chance(10)) {
        out += "\r\n";
        c.data += "\n";
        prev = '\n';
        continue;
      }
      out += ch;
      c.data += ch;
      prev = ch;
    }
    out += "-->";
  }

  void pi_body(MNode & p) {
    static const char * const targets[] = {"pi", "t1", "x-y", "xmlx", "a.b", "\xC3\xA9"};
    p.target = targets[below(6)];
    out += "<?" + p.target;
    const size_t n = below(8);
    if (n == 0) {
      out += chance(70) ? "?>" : " ?>";
      return;
    }
    space();
    char prev = 0;
    for (size_t i = 0; i < n; i++) {
      static const char pool[] = "ab 1<&]>x?";
      char ch = pool[below(sizeof(pool) - 1)];
      if (i == 0 && ch == ' ') {
        ch = 'q';
      }
      if (prev == '?' && ch == '>') {
        ch = 'g';
      }
      out += ch;
      p.data += ch;
      prev = ch;
    }
    if (prev == '?') {
      out += " ";
      p.data += " ";
    }
    out += "?>";
  }

  void doctype() {
    has_doctype = true;
    out += "<!DOCTYPE";
    space();
    out += "root";
    if (chance(50)) {
      space();
      if (chance(50)) {
        has_system = true;
        doctype_system = "s.dtd";
        out += chance(50) ? "SYSTEM \"s.dtd\"" : "SYSTEM 's.dtd'";
      }
      else {
        has_public = has_system = true;
        doctype_public = "-//A B//EN";
        doctype_system = "p&<>.dtd";
        out += "PUBLIC \" -//A\r\n  B//EN \" 'p&<>.dtd'";
      }
    }
    if (chance(60)) {
      maybe_space();
      out += "[";
      static const char * const pieces[] = {
          "<!ENTITY e \"]>\">", "<!-- ]> <!ELEMENT x -->", "<?p ]> ?>", "<!ENTITY f ']'>",
          "<!ELEMENT root ANY>", "<!ATTLIST root a CDATA \">]\" b CDATA '\"'>", "%pe;",
          "<!ENTITY % pe \"<!ELEMENT y ANY>\">", "<!NOTATION n SYSTEM \"a]\">", "\n", "  ",
          "<!ENTITY g \"a\nb\">"};
      const size_t n = below(6);
      for (size_t i = 0; i < n; i++) {
        out += pieces[below(sizeof(pieces) / sizeof(pieces[0]))];
        maybe_space();
      }
      out += "]";
      maybe_space();
    }
    out += ">";
  }

  // ---- text ------------------------------------------------------------

  static std::string utf8(uint32_t cp) {
    std::string s;
    if (cp < 0x80) {
      s += static_cast<char>(cp);
    }
    else if (cp < 0x800) {
      s += static_cast<char>(0xC0 | (cp >> 6));
      s += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000) {
      s += static_cast<char>(0xE0 | (cp >> 12));
      s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      s += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else {
      s += static_cast<char>(0xF0 | (cp >> 18));
      s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      s += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return s;
  }

  std::string reference(uint32_t cp) {
    return chance(50) ? "&#" + std::to_string(cp) + ";"
                      : "&#x" + ([&] {
                          char buf[16];
                          std::snprintf(buf, sizeof(buf), chance(50) ? "%X" : "%x", cp);
                          return std::string(buf);
                        })() + ";";
  }

  // One character of character data or of an attribute value, written some way
  // XML allows, with what a conforming parser must make of it appended to
  // `expect`. `quote` is the delimiter of an attribute value, or 0 in content.
  void character(std::string & expect, char quote) {
    static const uint32_t pool[] = {'a', 'b', 'c', ' ', '1', '<', '&', '>', ']', '"', '\'',
        '\n', '\t', '\r', 0xE9, 0x4E2D, 0x1F600, 0xFFFD, 0x7F, 'x', '-', '.'};
    const uint32_t cp = pool[below(sizeof(pool) / sizeof(pool[0]))];
    const bool attribute = quote != 0;
    const std::string logical = utf8(cp);
    switch (cp) {
      case '&':
        out += chance(60) ? "&amp;" : (chance(50) ? "&#38;" : "&#x26;");
        expect += '&';
        return;
      case '<':
        out += chance(60) ? "&lt;" : "&#60;";
        expect += '<';
        return;
      case '>':
        if (!attribute && out.size() >= 2 && out.compare(out.size() - 2, 2, "]]") == 0) {
          out += "&gt;";
        }
        else {
          out += chance(50) ? ">" : (chance(50) ? "&gt;" : "&#62;");
        }
        expect += '>';
        return;
      case '"':
      case '\'':
        if (attribute && static_cast<char>(cp) == quote) {
          out += cp == '"' ? (chance(50) ? "&quot;" : "&#34;") : (chance(50) ? "&apos;" : "&#39;");
        }
        else {
          out += static_cast<char>(cp);
        }
        expect += static_cast<char>(cp);
        return;
      case '\r':
        out += reference(13);
        expect += '\r';
        return;
      case '\n':
        if (chance(25)) {
          out += reference(10);
          expect += '\n';
          return;
        }
        out += line_end();
        expect += attribute ? " " : "\n";
        return;
      case '\t':
        if (chance(25)) {
          out += reference(9);
          expect += '\t';
          return;
        }
        out += '\t';
        expect += attribute ? " " : "\t";
        return;
      default:
        break;
    }
    if (chance(8)) {
      out += reference(cp);
    }
    else {
      out += logical;
    }
    expect += logical;
  }

  void content_text(MNode & parent) {
    const size_t n = 1 + below(6);
    std::string expect;
    for (size_t i = 0; i < n; i++) {
      character(expect, 0);
    }
    append_text(parent, expect);
  }

  static void append_text(MNode & parent, const std::string & expect) {
    if (expect.empty()) {
      return;
    }
    if (!parent.kids.empty() && parent.kids.back().kind == MNode::TEXT) {
      parent.kids.back().data += expect;  // adjacent character data is one node
      return;
    }
    MNode t;
    t.kind = MNode::TEXT;
    t.data = expect;
    parent.kids.push_back(t);
  }

  // ---- elements --------------------------------------------------------

  const std::string * lookup(const std::string & prefix) const {
    for (size_t i = scope.size(); i > 0; i--) {
      if (scope[i - 1].first == prefix) {
        return &scope[i - 1].second;
      }
    }
    return nullptr;
  }

  std::string attribute_value(char quote, std::string & expect) {
    const size_t n = below(5);
    for (size_t i = 0; i < n; i++) {
      character(expect, quote);
    }
    return expect;
  }

  void element(MNode & e, int depth) {
    e.kind = MNode::ELEMENT;
    const size_t mark = scope.size();

    // What the tag will declare, then the names it can use.
    struct Decl {
      std::string prefix, uri;
    };
    std::vector<Decl> decls;
    static const char * const prefixes[] = {"", "a", "b", "c"};
    static const char * const uris[] = {"urn:1", "urn:2", "urn:3"};
    const size_t wanted = depth == 0 ? 2 + below(2) : below(3);
    for (size_t i = 0; i < wanted; i++) {
      std::string prefix = prefixes[below(4)];
      bool taken = false;
      for (const Decl & d : decls) {
        taken = taken || d.prefix == prefix;
      }
      if (taken) {
        continue;
      }
      std::string uri = uris[below(3)];
      if (prefix.empty() && chance(30)) {
        uri.clear();  // xmlns="": no default namespace
      }
      decls.push_back({prefix, uri});
    }
    for (const Decl & d : decls) {
      scope.emplace_back(d.prefix, d.uri);
    }

    // The element's name: any bound prefix, or none.
    std::vector<std::string> usable;
    for (const char * candidate : prefixes) {
      if (*candidate) {
        const std::string * uri = lookup(candidate);
        if (uri && !uri->empty()) {
          usable.push_back(candidate);
        }
      }
    }
    static const char * const locals[] = {"e", "item", "\xC3\xA9l\xC3\xA9ment", "x1", "a-b", "_z"};
    e.local = locals[below(6)];
    if (!usable.empty() && chance(50)) {
      e.prefix = usable[below(usable.size())];
      e.ns = *lookup(e.prefix);
    }
    else {
      const std::string * uri = lookup("");
      e.ns = uri ? *uri : "";
    }
    const std::string qname = e.prefix.empty() ? e.local : e.prefix + ":" + e.local;

    // Attributes, in the order they will be written: declarations and others
    // shuffled together.
    struct Pending {
      MAttr model;
      std::string text;  // as written, after the leading white space
    };
    std::vector<Pending> pending;
    for (const Decl & d : decls) {
      Pending p;
      p.model.ns = kXmlns;
      p.model.prefix = d.prefix.empty() ? "" : "xmlns";
      p.model.local = d.prefix.empty() ? "xmlns" : d.prefix;
      p.model.value = d.uri;
      const char q = chance(50) ? '"' : '\'';
      p.text = (d.prefix.empty() ? "xmlns" : "xmlns:" + d.prefix) + eq() + q + d.uri + q;
      pending.push_back(p);
    }
    std::set<std::pair<std::string, std::string>> used;
    const size_t attr_count = below(5);
    for (size_t i = 0; i < attr_count; i++) {
      Pending p;
      static const char * const plain[] = {"x", "y", "z", "w", "k-1"};
      static const char * const qualified[] = {"m", "n"};
      std::string prefix;
      std::string local;
      std::string ns;
      if (!usable.empty() && chance(50)) {
        prefix = usable[below(usable.size())];
        local = qualified[below(2)];
        ns = *lookup(prefix);
      }
      else {
        local = plain[below(5)];
      }
      if (!used.insert({ns, local}).second) {
        continue;
      }
      // Two prefixes of one namespace and the same local name would be the same
      // attribute: the set above is keyed by namespace, not by prefix.
      p.model.ns = ns;
      p.model.prefix = prefix;
      p.model.local = local;
      const char q = chance(50) ? '"' : '\'';
      std::string saved;
      saved.swap(out);
      std::string expect;
      attribute_value(q, expect);
      std::string written;
      written.swap(out);
      out.swap(saved);
      p.model.value = expect;
      p.text = (prefix.empty() ? local : prefix + ":" + local) + eq() + q + written + q;
      pending.push_back(p);
    }
    for (size_t i = pending.size(); i > 1; i--) {
      std::swap(pending[i - 1], pending[below(i)]);
    }

    out += "<" + qname;
    for (const Pending & p : pending) {
      space();
      out += p.text;
      e.attrs.push_back(p.model);
    }
    if (chance(30)) {
      maybe_space();
    }

    const bool kids_wanted = depth < 5 && chance(75);
    if (!kids_wanted && chance(60)) {
      out += "/>";
      scope.resize(mark);
      return;
    }
    out += ">";
    if (kids_wanted) {
      const size_t n = below(7);
      for (size_t i = 0; i < n; i++) {
        child(e, depth);
      }
    }
    out += "</" + qname;
    if (chance(20)) {
      maybe_space();
    }
    out += ">";
    scope.resize(mark);
  }

  std::string eq() {
    std::string s;
    if (chance(20)) {
      s += chance(50) ? " " : "\n";
    }
    s += "=";
    if (chance(20)) {
      s += chance(50) ? "  " : "\t";
    }
    return s;
  }

  void child(MNode & parent, int depth) {
    switch (below(10)) {
      case 0:
      case 1:
      case 2:
        content_text(parent);
        break;
      case 3:
      case 4:
      case 5: {
        MNode e;
        element(e, depth + 1);
        parent.kids.push_back(e);
        break;
      }
      case 6: {
        MNode c;
        c.kind = MNode::CDATA;
        out += "<![CDATA[";
        const size_t n = below(6);
        for (size_t i = 0; i < n; i++) {
          static const char pool[] = "ab <&>]x\n";
          const char ch = pool[below(sizeof(pool) - 1)];
          if (ch == '>' && out.size() >= 2 && out.compare(out.size() - 2, 2, "]]") == 0) {
            out += 'g';
            c.data += 'g';
            continue;
          }
          if (ch == '\n') {
            out += line_end();
            c.data += '\n';
            continue;
          }
          out += ch;
          c.data += ch;
        }
        out += "]]>";
        parent.kids.push_back(c);
        break;
      }
      case 7: {
        MNode c;
        c.kind = MNode::COMMENT;
        comment_body(c);
        parent.kids.push_back(c);
        break;
      }
      case 8: {
        MNode p;
        p.kind = MNode::PI;
        pi_body(p);
        parent.kids.push_back(p);
        break;
      }
      default:
        content_text(parent);
        break;
    }
  }
};

// ---- the comparison -------------------------------------------------------

struct Checker {
  std::string where;
  size_t compared = 0;

  void fail(const std::string & what) { ADD_FAILURE() << where << ": " << what; }

  static std::string opt(const char * s) { return s ? s : ""; }

  void compare_children(const std::vector<MNode> & model, Node * parent) {
    Node * dom = gtext_markup_node_first_child(parent);
    for (const MNode & m : model) {
      if (!dom) {
        fail("the tree has fewer nodes than the model");
        return;
      }
      compare_node(m, dom);
      dom = gtext_markup_node_next_sibling(dom);
    }
    if (dom) {
      fail("the tree has more nodes than the model");
    }
  }

  void compare_node(const MNode & m, Node * dom) {
    compared++;
    size_t length = 0;
    switch (m.kind) {
      case MNode::ELEMENT: {
        if (gtext_markup_node_kind(dom) != GTEXT_MARKUP_KIND_ELEMENT) {
          fail("expected an element");
          return;
        }
        if (opt(gtext_markup_element_namespace(dom)) != m.ns
            || opt(gtext_markup_element_prefix(dom)) != m.prefix
            || opt(gtext_markup_element_local_name(dom)) != m.local) {
          fail("element name {" + opt(gtext_markup_element_namespace(dom)) + "}"
              + opt(gtext_markup_element_prefix(dom)) + ":" + opt(gtext_markup_element_local_name(dom))
              + " expected {" + m.ns + "}" + m.prefix + ":" + m.local);
        }
        if (gtext_markup_element_attribute_count(dom) != m.attrs.size()) {
          fail("attribute count");
          return;
        }
        for (size_t i = 0; i < m.attrs.size(); i++) {
          GTEXT_Markup_Attribute a;
          if (gtext_markup_element_attribute_at(dom, i, &a) != GTEXT_MARKUP_OK) {
            fail("attribute_at");
            return;
          }
          const MAttr & want = m.attrs[i];
          if (opt(a.ns) != want.ns || opt(a.prefix) != want.prefix || opt(a.local) != want.local
              || std::string(a.value, a.value_length) != want.value) {
            fail("attribute " + std::to_string(i) + " {" + opt(a.ns) + "}" + opt(a.prefix) + ":"
                + opt(a.local) + "=" + std::string(a.value, a.value_length) + " expected {"
                + want.ns + "}" + want.prefix + ":" + want.local + "=" + want.value);
          }
        }
        compare_children(m.kids, dom);
        break;
      }
      case MNode::TEXT:
      case MNode::CDATA:
      case MNode::COMMENT: {
        const GTEXT_Markup_Kind kind = m.kind == MNode::TEXT ? GTEXT_MARKUP_KIND_TEXT
            : m.kind == MNode::CDATA ? GTEXT_MARKUP_KIND_CDATA : GTEXT_MARKUP_KIND_COMMENT;
        if (gtext_markup_node_kind(dom) != kind) {
          fail("node kind");
          return;
        }
        const char * data = gtext_markup_node_data(dom, &length);
        if (std::string(data, length) != m.data) {
          fail("data '" + std::string(data, length) + "' expected '" + m.data + "'");
        }
        break;
      }
      case MNode::PI: {
        if (gtext_markup_node_kind(dom) != GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION) {
          fail("expected a processing instruction");
          return;
        }
        const char * data = gtext_markup_node_data(dom, &length);
        if (opt(gtext_markup_pi_target(dom)) != m.target || std::string(data, length) != m.data) {
          fail("processing instruction");
        }
        break;
      }
    }
  }
};

TEST(XmlModel, RandomDocumentsParseToTheTreeTheyWereBuiltAs) {
  size_t nodes = 0;
  size_t with_doctype = 0;
  for (uint32_t seed = 1; seed <= 4000; seed++) {
    Generator g(seed);
    g.generate();
    GTEXT_Markup_Document * doc = nullptr;
    GTEXT_XML_Error err;
    const GTEXT_XML_Status status = gtext_xml_parse(g.out.data(), g.out.size(), nullptr, &doc, &err);
    ASSERT_EQ(status, GTEXT_XML_OK) << "seed " << seed << ": " << err.message << " at "
        << err.byte_offset << "\n" << g.out;
    const char * problem = markup_check_invariants(doc);
    ASSERT_STREQ(problem ? problem : "ok", "ok") << "seed " << seed;

    // The root node's children: the document type, then misc, the root
    // element, misc. The model keeps them as two lists around the root.
    std::vector<MNode> top = g.top_before;
    top.push_back(g.root);
    for (const MNode & m : g.top_after) {
      top.push_back(m);
    }
    Checker c;
    c.where = "seed " + std::to_string(seed);
    Node * root = gtext_markup_document_root(doc);
    Node * first = gtext_markup_node_first_child(root);
    // The doctype node sits among the misc before the root: find it, check it,
    // and compare the rest around it.
    Node * doctype = gtext_markup_document_doctype(doc);
    ASSERT_EQ(doctype != nullptr, g.has_doctype) << c.where;
    if (doctype) {
      with_doctype++;
      EXPECT_STREQ(gtext_markup_doctype_name(doctype), "root");
      EXPECT_EQ(gtext_markup_doctype_public_id(doctype) != nullptr, g.has_public) << c.where;
      EXPECT_EQ(gtext_markup_doctype_system_id(doctype) != nullptr, g.has_system) << c.where;
      if (g.has_public) {
        EXPECT_STREQ(gtext_markup_doctype_public_id(doctype), g.doctype_public.c_str()) << c.where;
      }
      if (g.has_system) {
        EXPECT_STREQ(gtext_markup_doctype_system_id(doctype), g.doctype_system.c_str()) << c.where;
      }
    }
    // Compare the list without the doctype node by walking and skipping it.
    Node * dom = first;
    for (const MNode & m : top) {
      if (dom == doctype && dom) {
        dom = gtext_markup_node_next_sibling(dom);
      }
      ASSERT_NE(dom, nullptr) << c.where;
      c.compare_node(m, dom);
      dom = gtext_markup_node_next_sibling(dom);
    }
    if (dom == doctype && dom) {
      dom = gtext_markup_node_next_sibling(dom);
    }
    EXPECT_EQ(dom, nullptr) << c.where;
    nodes += c.compared;
    gtext_markup_document_free(doc);
    if (::testing::Test::HasFailure()) {
      FAIL() << "seed " << seed << "\n" << g.out;
    }
  }
  // A generator that stopped producing structure would pass everything above.
  EXPECT_GT(nodes, 25000u);
  EXPECT_GT(with_doctype, 1000u);
}

TEST(XmlModel, NoProperPrefixOfADocumentIsADocument) {
  size_t cuts = 0;
  for (uint32_t seed = 1; seed <= 300; seed++) {
    Generator g(seed);
    g.generate();
    // Everything before the end of the root element is incomplete.
    for (size_t cut = 0; cut < g.root_end; cut++) {
      GTEXT_Markup_Document * doc = nullptr;
      const GTEXT_XML_Status status = gtext_xml_parse(g.out.data(), cut, nullptr, &doc, nullptr);
      if (status == GTEXT_XML_OK) {
        gtext_markup_document_free(doc);
      }
      ASSERT_NE(status, GTEXT_XML_OK) << "seed " << seed << " cut " << cut << "\n"
          << g.out.substr(0, cut);
      cuts++;
    }
  }
  EXPECT_GT(cuts, 50000u);
}

TEST(XmlModel, TheSameDocumentParsedTwiceIsTheSameTree) {
  // A parse depends on nothing but its input: not on a previous parse, an
  // address, or a hash seed (each document gets its own).
  for (uint32_t seed = 1; seed <= 200; seed++) {
    Generator g(seed);
    g.generate();
    GTEXT_Markup_Document * a = nullptr;
    GTEXT_Markup_Document * b = nullptr;
    ASSERT_EQ(gtext_xml_parse(g.out.data(), g.out.size(), nullptr, &a, nullptr), GTEXT_XML_OK);
    ASSERT_EQ(gtext_xml_parse(g.out.data(), g.out.size(), nullptr, &b, nullptr), GTEXT_XML_OK);
    EXPECT_TRUE(gtext_markup_equal(gtext_markup_document_root(a), gtext_markup_document_root(b)))
        << "seed " << seed;
    gtext_markup_document_free(a);
    gtext_markup_document_free(b);
  }
}

}  // namespace
