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
 * The XML writer.
 *
 * What is claimed, and where it is checked:
 *
 * - Every row of the specification's matrix, by hand: the round trip, escaping
 *   per context, each refusal (and that a refusal writes nothing and names the
 *   path), missing namespace declarations and the option that adds them, every
 *   encoding down to the byte, pretty printing and its counts, the sink and the
 *   allocator failing at every point, files.
 * - A model that shares no code with the library: a plain C++ tree generated
 *   at random, serialized by a second implementation of the rules, built into
 *   the library's tree and written. The two byte strings must be equal, in
 *   compact and pretty form and in every encoding, and the library's output must
 *   parse back to a tree equal to the one it was written from.
 * - Depth: a tree of 100,000 levels needs no C stack, and the limits refuse at
 *   exactly the value.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <ghoti.io/text/csv.h>
#include <ghoti.io/text/xml.h>

#include "../src/markup/markup_internal.h"
#include "markup_test_support.h"

namespace {

using Doc = GTEXT_Markup_Document;
using Node = GTEXT_Markup_Node;
using Opts = GTEXT_XML_Write_Options;

#define NS_XML "http://www.w3.org/XML/1998/namespace"
#define NS_XMLNS "http://www.w3.org/2000/xmlns/"

// ---- helpers ------------------------------------------------------------

Opts defaults() {
  return gtext_xml_write_options_default();
}

Doc * parse(const std::string & text) {
  GTEXT_XML_Parse_Options o = gtext_xml_parse_options_default();
  o.max_depth = 0x7fffffff;
  Doc * doc = nullptr;
  GTEXT_XML_Error err;
  const GTEXT_XML_Status st = gtext_xml_parse(text.data(), text.size(), &o, &doc, &err);
  EXPECT_EQ(st, GTEXT_XML_OK) << text << ": " << (err.message ? err.message : "");
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
    EXPECT_EQ(buf[len], '\0');
    gtext_xml_write_buffer_free(doc, &o, buf);
  }
  else {
    EXPECT_EQ(buf, nullptr);
  }
  return out;
}

Out write_default(const Doc * doc) {
  return write_with(doc, defaults());
}

// A document being built by hand, with no limits.
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
  Node * el(Node * parent, const char * local, const char * ns = nullptr,
      const char * prefix = nullptr) {
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
  Node * doctype(const char * name, const char * pub, const char * sys) {
    Node * n = nullptr;
    EXPECT_EQ(gtext_markup_create_doctype(doc, name, pub, sys, &n), GTEXT_MARKUP_OK);
    EXPECT_EQ(gtext_markup_append_child(root(), n), GTEXT_MARKUP_OK);
    return n;
  }
};

// A sink that collects, and can be told to refuse the n-th call.
struct Collect {
  std::string data;
  size_t calls = 0;
  size_t fail_on = 0;  // 1-based; 0 never
  GTEXT_Markup_Status fail_with = GTEXT_MARKUP_E_WRITE;
  static GTEXT_Markup_Status write(void * user, const char * b, size_t n) {
    auto * self = static_cast<Collect *>(user);
    self->calls++;
    if (self->fail_on != 0 && self->calls == self->fail_on) {
      return self->fail_with;
    }
    self->data.append(b, n);
    return GTEXT_MARKUP_OK;
  }
  GTEXT_Markup_Sink sink() {
    return GTEXT_Markup_Sink{&Collect::write, this};
  }
};

std::string le16(const std::string & ascii) {
  std::string s;
  for (char c : ascii) {
    s += c;
    s += '\0';
  }
  return s;
}

// ---- the matrix, row by row ---------------------------------------------

TEST(XmlWriter, TheDefaultsAreSpelledOutAndZeroIsTheDefault) {
  const Opts d = defaults();
  EXPECT_EQ(d.allocator, nullptr);
  EXPECT_EQ(d.xml_declaration, GTEXT_XML_DECLARATION_AUTO);
  EXPECT_EQ(d.encoding, GTEXT_XML_OUTPUT_UTF8);
  EXPECT_FALSE(d.expand_empty_elements);
  EXPECT_FALSE(d.declare_namespaces);
  EXPECT_EQ(d.indent, nullptr);
  EXPECT_EQ(d.newline, nullptr);
  EXPECT_EQ(d.max_depth, 256u);
  EXPECT_EQ(d.max_output_bytes, 256u * 1024 * 1024);

  Opts zero;
  std::memset(&zero, 0, sizeof(zero));
  Doc * doc = parse("<a><b/></a>");
  EXPECT_EQ(write_with(doc, zero).bytes, write_with(doc, d).bytes);
  EXPECT_EQ(write_with(doc, zero).bytes, "<a><b/></a>");
  // NULL options are the defaults too.
  char * buf = nullptr;
  size_t len = 0;
  ASSERT_EQ(gtext_xml_write_buffer(doc, nullptr, &buf, &len, nullptr), GTEXT_XML_OK);
  EXPECT_EQ(std::string(buf, len), "<a><b/></a>");
  gtext_xml_write_buffer_free(doc, nullptr, buf);
  gtext_markup_document_free(doc);
}

TEST(XmlWriter, ARichDocumentRoundTripsAndTheSecondWriteIsIdentical) {
  const std::string text =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<!-- before -->\n<?pi some data?>\n"
      "<!DOCTYPE root PUBLIC \"-//A B//EN\" \"p.dtd\" [<!ENTITY e \"x\">]>\n"
      "<root xmlns=\"urn:d\" xmlns:p=\"urn:p\" xml:lang=\"en\" p:attr=\"a&amp;b\" plain='q\"t'>"
      "text &lt; &amp; ]]&gt; \xC3\xA9 &#x20AC; &#13;end<![CDATA[<raw> & stuff]]>"
      "<p:child p:x=\"1\"/><!--c--><?t?>\n<empty></empty>"
      "<inner xmlns=\"\"><u/></inner><![CDATA[]]>tail</root>\n<!-- after -->";
  Doc * a = parse(text);
  ASSERT_NE(a, nullptr);
  const Out first = write_default(a);
  ASSERT_EQ(first.status, GTEXT_XML_OK) << first.r.message << " " << first.r.path;
  Doc * b = parse(first.bytes);
  ASSERT_NE(b, nullptr) << first.bytes;
  EXPECT_TRUE(gtext_markup_equal(gtext_markup_document_root(a), gtext_markup_document_root(b)))
      << first.bytes;
  GTEXT_Markup_Standalone sa, sb;
  EXPECT_STREQ(gtext_markup_document_xml_version(a, &sa), gtext_markup_document_xml_version(b, &sb));
  EXPECT_EQ(sa, sb);
  EXPECT_EQ(sa, GTEXT_MARKUP_STANDALONE_YES);
  const Out second = write_default(b);
  EXPECT_EQ(second.bytes, first.bytes);
  EXPECT_EQ(first.r.bytes_written, first.bytes.size());
  EXPECT_TRUE(first.r.doctype_without_subset);
  EXPECT_EQ(first.r.text_nodes_coalesced, 0u);
  EXPECT_EQ(first.bytes.rfind("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<!-- before -->\n<?pi some data?>\n<!DOCTYPE", 0), 0u);
  EXPECT_NE(first.bytes.find("<!DOCTYPE root PUBLIC \"-//A B//EN\" \"p.dtd\">"), std::string::npos);
  EXPECT_NE(first.bytes.find("<empty/>"), std::string::npos);
  gtext_markup_document_free(a);
  gtext_markup_document_free(b);
}

TEST(XmlWriter, EscapesTextAndAttributesForTheirContext) {
  Build b;
  Node * a = b.el(nullptr, "a");
  b.text(a, "a<b&c]]>d \"q\" 'a' \r\n\t");
  b.attr(a, "t", "x\ty\nz\r\"w\"<&>'");
  EXPECT_EQ(write_default(b.doc).bytes,
      "<a t=\"x&#9;y&#10;z&#13;&quot;w&quot;&lt;&amp;&gt;'\">"
      "a&lt;b&amp;c]]&gt;d \"q\" 'a' &#13;\n\t</a>");
  Doc * back = parse(write_default(b.doc).bytes);
  EXPECT_TRUE(gtext_markup_equal(b.root(), gtext_markup_document_root(back)));
  gtext_markup_document_free(back);
}

struct Refusal {
  const char * what;
  std::function<void(Build &)> build;
  GTEXT_XML_Status status;
  const char * path;
};

TEST(XmlWriter, RefusesWhatXmlCannotSpellWithAStatusAndAPathAndWritesNothing) {
  const std::vector<Refusal> cases = {
      {"comment with --", [](Build & b) { b.comment(b.el(nullptr, "a"), "x--y"); },
          GTEXT_XML_E_UNREPRESENTABLE, "/a/comment()"},
      {"comment ending in -", [](Build & b) { b.comment(b.el(nullptr, "a"), "x-"); },
          GTEXT_XML_E_UNREPRESENTABLE, "/a/comment()"},
      {"comment, top level", [](Build & b) { b.el(nullptr, "a"); b.comment(nullptr, "--"); },
          GTEXT_XML_E_UNREPRESENTABLE, "/comment()"},
      {"PI data with ?>", [](Build & b) { b.pi(b.el(nullptr, "a"), "t", "x?>y"); },
          GTEXT_XML_E_UNREPRESENTABLE, "/a/processing-instruction(t)"},
      {"PI data with leading space", [](Build & b) { b.pi(b.el(nullptr, "a"), "t", " x"); },
          GTEXT_XML_E_UNREPRESENTABLE, "/a/processing-instruction(t)"},
      {"PI data with leading tab", [](Build & b) { b.pi(b.el(nullptr, "a"), "t", "\tx"); },
          GTEXT_XML_E_UNREPRESENTABLE, "/a/processing-instruction(t)"},
      {"PI data with leading line feed", [](Build & b) { b.pi(b.el(nullptr, "a"), "t", "\nx"); },
          GTEXT_XML_E_UNREPRESENTABLE, "/a/processing-instruction(t)"},
      {"PI target xml", [](Build & b) { b.pi(b.el(nullptr, "a"), "xml", "x"); }, GTEXT_XML_E_NAME,
          "/a/processing-instruction(xml)"},
      {"PI target XmL", [](Build & b) { b.pi(b.el(nullptr, "a"), "XmL", ""); }, GTEXT_XML_E_NAME,
          "/a/processing-instruction(XmL)"},
      {"PI target with a colon", [](Build & b) { b.pi(b.el(nullptr, "a"), "a:b", ""); },
          GTEXT_XML_E_NAME, "/a/processing-instruction(a:b)"},
      {"CDATA with ]]>", [](Build & b) { b.cdata(b.el(nullptr, "a"), "x]]>y"); },
          GTEXT_XML_E_UNREPRESENTABLE, "/a/cdata()"},
      {"CR in CDATA", [](Build & b) { b.cdata(b.el(nullptr, "a"), "x\ry"); },
          GTEXT_XML_E_UNREPRESENTABLE, "/a/cdata()"},
      {"CR in a comment", [](Build & b) { b.comment(b.el(nullptr, "a"), "x\ry"); },
          GTEXT_XML_E_UNREPRESENTABLE, "/a/comment()"},
      {"CR in PI data", [](Build & b) { b.pi(b.el(nullptr, "a"), "t", "x\ry"); },
          GTEXT_XML_E_UNREPRESENTABLE, "/a/processing-instruction(t)"},
      {"NUL in text", [](Build & b) { b.text(b.el(nullptr, "a"), std::string("x\0y", 3)); },
          GTEXT_XML_E_CHARACTER, "/a/text()"},
      {"U+0001 in an attribute value",
          [](Build & b) { b.attr(b.el(nullptr, "a"), "x", "\x01"); }, GTEXT_XML_E_CHARACTER,
          "/a/@x"},
      {"U+FFFE in a comment", [](Build & b) { b.comment(b.el(nullptr, "a"), "\xEF\xBF\xBE"); },
          GTEXT_XML_E_CHARACTER, "/a/comment()"},
      {"U+FFFF in CDATA", [](Build & b) { b.cdata(b.el(nullptr, "a"), "\xEF\xBF\xBF"); },
          GTEXT_XML_E_CHARACTER, "/a/cdata()"},
      {"element name that is not a name", [](Build & b) { b.el(b.el(nullptr, "a"), "1b"); },
          GTEXT_XML_E_NAME, "/a/1b"},
      {"element name with a space", [](Build & b) { b.el(nullptr, "a b"); }, GTEXT_XML_E_NAME,
          "/a b"},
      {"element local name with a colon", [](Build & b) { b.el(nullptr, "a:b"); },
          GTEXT_XML_E_NAME, "/a:b"},
      {"attribute name that is not a name",
          [](Build & b) { b.attr(b.el(nullptr, "a"), "x y", "1"); }, GTEXT_XML_E_NAME,
          "/a/@x y"},
      {"second of two same-named elements",
          [](Build & b) {
            Node * a = b.el(nullptr, "a");
            b.el(a, "i");
            b.el(a, "i");
            b.el(a, "i");
            b.comment(b.el(a, "bad"), "--");
          },
          GTEXT_XML_E_UNREPRESENTABLE, "/a/bad/comment()"},
      {"index among same-named siblings",
          [](Build & b) {
            Node * a = b.el(nullptr, "a");
            b.el(a, "i");
            b.comment(b.el(a, "i"), "--");
          },
          GTEXT_XML_E_UNREPRESENTABLE, "/a/i[2]/comment()"},
      {"doctype name", [](Build & b) { b.doctype("a b", nullptr, nullptr); b.el(nullptr, "a"); },
          GTEXT_XML_E_NAME, "/doctype()"},
      {"public id with no system id",
          [](Build & b) { b.doctype("a", "pub", nullptr); b.el(nullptr, "a"); },
          GTEXT_XML_E_DOCTYPE, "/doctype()"},
      {"public id with a character it cannot hold",
          [](Build & b) { b.doctype("a", "p<q", "s"); b.el(nullptr, "a"); }, GTEXT_XML_E_DOCTYPE,
          "/doctype()"},
      {"public id read back with its white space collapsed",
          [](Build & b) { b.doctype("a", "p  q", "s"); b.el(nullptr, "a"); }, GTEXT_XML_E_DOCTYPE,
          "/doctype()"},
      {"public id with a leading space",
          [](Build & b) { b.doctype("a", " p", "s"); b.el(nullptr, "a"); }, GTEXT_XML_E_DOCTYPE,
          "/doctype()"},
      {"public id with a trailing space",
          [](Build & b) { b.doctype("a", "p ", "s"); b.el(nullptr, "a"); }, GTEXT_XML_E_DOCTYPE,
          "/doctype()"},
      {"system id with both kinds of quote",
          [](Build & b) { b.doctype("a", nullptr, "a\"b'c"); b.el(nullptr, "a"); },
          GTEXT_XML_E_DOCTYPE, "/doctype()"},
      {"system id with a carriage return",
          [](Build & b) { b.doctype("a", nullptr, "a\rb"); b.el(nullptr, "a"); },
          GTEXT_XML_E_DOCTYPE, "/doctype()"},
      {"template content",
          [](Build & b) {
            Node * t = b.el(nullptr, "template");
            Node * frag = nullptr;
            ASSERT_EQ(gtext_markup_element_ensure_template_content(t, &frag), GTEXT_MARKUP_OK);
          },
          GTEXT_XML_E_UNREPRESENTABLE, "/template"},
      {"no root element", [](Build & b) { b.comment(nullptr, "x"); }, GTEXT_XML_E_UNREPRESENTABLE,
          ""},
  };
  for (const Refusal & c : cases) {
    SCOPED_TRACE(c.what);
    Build b;
    c.build(b);
    Collect sink;
    GTEXT_XML_Write_Result r;
    const GTEXT_Markup_Sink s = sink.sink();
    const GTEXT_XML_Status st = gtext_xml_write(&s, b.doc, nullptr, &r);
    EXPECT_EQ(st, c.status) << (r.message ? r.message : "");
    EXPECT_EQ(r.code, c.status);
    EXPECT_NE(r.message, nullptr);
    EXPECT_STREQ(r.path, c.path);
    // The whole document is checked before the first byte goes out.
    EXPECT_EQ(sink.calls, 0u);
    EXPECT_EQ(sink.data, "");
    // The same through the buffer form: no buffer.
    EXPECT_EQ(write_default(b.doc).status, c.status);
  }
}

TEST(XmlWriter, ARefusalNamesTheOffendingPrefixOrName) {
  Build b;
  Node * a = b.el(nullptr, "a", "urn:u", "p");
  (void)a;
  Out out = write_default(b.doc);
  EXPECT_EQ(out.status, GTEXT_XML_E_NAMESPACE);
  EXPECT_STREQ(out.r.name, "p");
  EXPECT_STREQ(out.r.path, "/p:a");
}

// ---- namespaces ----------------------------------------------------------

TEST(XmlWriter, ARefusalForEveryWayANamespaceCanBeWrong) {
  struct C {
    const char * what;
    std::function<void(Build &)> build;
  };
  const std::vector<C> cases = {
      {"prefix with no declaration", [](Build & b) { b.el(nullptr, "a", "urn:u", "p"); }},
      {"default namespace with no declaration", [](Build & b) { b.el(nullptr, "a", "urn:u"); }},
      {"prefix bound to another namespace",
          [](Build & b) {
            Node * a = b.el(nullptr, "a", "urn:1", "p");
            b.xmlns(a, "p", "urn:1");
            b.el(a, "c", "urn:2", "p");
          }},
      {"xmlns attribute contradicting the element's namespace",
          [](Build & b) {
            Node * a = b.el(nullptr, "a", "urn:1", "p");
            b.xmlns(a, "p", "urn:2");
          }},
      {"xmlns default contradicting the element's namespace",
          [](Build & b) {
            Node * a = b.el(nullptr, "a", "urn:1");
            b.xmlns(a, nullptr, "urn:2");
          }},
      {"element with no namespace inside a default namespace",
          [](Build & b) {
            Node * a = b.el(nullptr, "a", "urn:1");
            b.xmlns(a, nullptr, "urn:1");
            b.el(a, "c");
          }},
      {"attribute prefix with no declaration",
          [](Build & b) { b.attr(b.el(nullptr, "a"), "x", "1", "urn:u", "p"); }},
      {"attribute in a namespace with no prefix",
          [](Build & b) {
            Node * a = b.el(nullptr, "a", "urn:u");
            b.xmlns(a, nullptr, "urn:u");
            b.attr(a, "x", "1", "urn:u", nullptr);
          }},
      {"prefix and no namespace on an element", [](Build & b) { b.el(nullptr, "a", nullptr, "p"); }},
      {"prefix and no namespace on an attribute",
          [](Build & b) { b.attr(b.el(nullptr, "a"), "x", "1", nullptr, "p"); }},
      {"an attribute named xmlns outside the xmlns namespace",
          [](Build & b) { b.attr(b.el(nullptr, "a"), "xmlns", "urn:u"); }},
      {"element prefix xmlns", [](Build & b) { b.el(nullptr, "a", "urn:u", "xmlns"); }},
      {"element in the xmlns namespace", [](Build & b) { b.el(nullptr, "a", NS_XMLNS, "p"); }},
      {"element in the XML namespace without the xml prefix",
          [](Build & b) { b.el(nullptr, "a", NS_XML, "p"); }},
      {"element with prefix xml in another namespace",
          [](Build & b) { b.el(nullptr, "a", "urn:u", "xml"); }},
      {"attribute in the XML namespace without the xml prefix",
          [](Build & b) { b.attr(b.el(nullptr, "a"), "lang", "en", NS_XML, "p"); }},
      {"attribute with prefix xml in another namespace",
          [](Build & b) { b.attr(b.el(nullptr, "a"), "lang", "en", "urn:u", "xml"); }},
      {"attribute prefix xmlns outside a declaration",
          [](Build & b) { b.attr(b.el(nullptr, "a"), "x", "1", "urn:u", "xmlns"); }},
      {"declaring the prefix xmlns",
          [](Build & b) { b.xmlns(b.el(nullptr, "a"), "xmlns", "urn:u"); }},
      {"declaring xml as something else",
          [](Build & b) { b.xmlns(b.el(nullptr, "a"), "xml", "urn:u"); }},
      {"binding the XML namespace to another prefix",
          [](Build & b) { b.xmlns(b.el(nullptr, "a"), "p", NS_XML); }},
      {"binding the xmlns namespace", [](Build & b) { b.xmlns(b.el(nullptr, "a"), "p", NS_XMLNS); }},
      {"default namespace the XML namespace",
          [](Build & b) { b.xmlns(b.el(nullptr, "a"), nullptr, NS_XML); }},
      {"undeclaring a prefix", [](Build & b) { b.xmlns(b.el(nullptr, "a"), "p", ""); }},
      {"declaring a prefix that is not a name",
          [](Build & b) { b.xmlns(b.el(nullptr, "a"), "1p", "urn:u"); }},
      {"a declaration-like attribute that is not one",
          [](Build & b) { b.attr(b.el(nullptr, "a"), "other", "u", NS_XMLNS, nullptr); }},
  };
  for (const C & c : cases) {
    SCOPED_TRACE(c.what);
    Build b;
    c.build(b);
    Collect sink;
    GTEXT_XML_Write_Result r;
    const GTEXT_Markup_Sink s = sink.sink();
    const GTEXT_XML_Status st = gtext_xml_write(&s, b.doc, nullptr, &r);
    EXPECT_TRUE(st == GTEXT_XML_E_NAMESPACE || st == GTEXT_XML_E_NAME) << st;
    EXPECT_NE(r.message, nullptr);
    EXPECT_EQ(sink.calls, 0u);
  }
}

TEST(XmlWriter, ReservedPrefixesNeedNoDeclarationAndTheyAreAllowedWhenRight) {
  Build b;
  Node * a = b.el(nullptr, "a");
  b.attr(a, "lang", "en", NS_XML, "xml");
  b.xmlns(a, "xml", NS_XML);
  EXPECT_EQ(write_default(b.doc).bytes, "<a xml:lang=\"en\" xmlns:xml=\"" NS_XML "\"/>");
  Build c;
  c.el(nullptr, "e", NS_XML, "xml");
  EXPECT_EQ(write_default(c.doc).bytes, "<xml:e/>");
}

TEST(XmlWriter, DeclareNamespacesAddsWhatIsMissingAndSaysSo) {
  Opts o = defaults();
  o.declare_namespaces = true;
  {
    Build b;
    Node * a = b.el(nullptr, "a", "urn:u", "p");
    b.attr(a, "x", "1", "urn:v", "q");
    b.el(a, "c", "urn:u", "p");
    b.el(a, "d", "urn:w");
    const Out out = write_with(b.doc, o);
    ASSERT_EQ(out.status, GTEXT_XML_OK) << out.r.message;
    EXPECT_EQ(out.bytes,
        "<p:a q:x=\"1\" xmlns:p=\"urn:u\" xmlns:q=\"urn:v\"><p:c/><d xmlns=\"urn:w\"/></p:a>");
    EXPECT_EQ(out.r.namespace_declarations_added, 3u);
    // And that is a document the parser reads.
    Doc * back = parse(out.bytes);
    ASSERT_NE(back, nullptr);
    EXPECT_STREQ(gtext_markup_element_namespace(gtext_markup_document_element(back)), "urn:u");
    gtext_markup_document_free(back);
    // Without the option the same tree is refused and names the prefix.
    const Out no = write_default(b.doc);
    EXPECT_EQ(no.status, GTEXT_XML_E_NAMESPACE);
    EXPECT_STREQ(no.r.name, "p");
  }
  {
    // A declaration in scope is not added again; undeclaring the default is.
    Build b;
    Node * a = b.el(nullptr, "a", "urn:u");
    b.xmlns(a, nullptr, "urn:u");
    b.el(a, "keep", "urn:u");
    b.el(a, "gone");
    const Out out = write_with(b.doc, o);
    ASSERT_EQ(out.status, GTEXT_XML_OK);
    EXPECT_EQ(out.bytes, "<a xmlns=\"urn:u\"><keep/><gone xmlns=\"\"/></a>");
    EXPECT_EQ(out.r.namespace_declarations_added, 1u);
  }
  {
    // A prefix bound outside to another namespace is rebound on the element
    // that needs it, and is in force for its descendants.
    Build b;
    Node * a = b.el(nullptr, "a", "urn:1", "p");
    b.xmlns(a, "p", "urn:1");
    Node * c = b.el(a, "c", "urn:2", "p");
    b.el(c, "d", "urn:2", "p");
    const Out out = write_with(b.doc, o);
    ASSERT_EQ(out.status, GTEXT_XML_OK) << out.r.message;
    EXPECT_EQ(out.bytes, "<p:a xmlns:p=\"urn:1\"><p:c xmlns:p=\"urn:2\"><p:d/></p:c></p:a>");
    Doc * back = parse(out.bytes);
    EXPECT_STREQ(gtext_markup_element_namespace(
                     gtext_markup_node_first_child(gtext_markup_node_first_child(
                         gtext_markup_document_element(back)))),
        "urn:2");
    gtext_markup_document_free(back);
  }
  {
    // What it will not do: contradict the element's own xmlns attribute, rebind
    // a prefix the element's own name relies on, or invent a prefix.
    Build b1;
    b1.xmlns(b1.el(nullptr, "a", "urn:1", "p"), "p", "urn:2");
    EXPECT_EQ(write_with(b1.doc, o).status, GTEXT_XML_E_NAMESPACE);
    Build b2;
    Node * outer = b2.el(nullptr, "o", "urn:1", "p");
    b2.xmlns(outer, "p", "urn:1");
    Node * inner = b2.el(outer, "i", "urn:1", "p");
    b2.attr(inner, "x", "1", "urn:2", "p");
    const Out out = write_with(b2.doc, o);
    EXPECT_EQ(out.status, GTEXT_XML_E_NAMESPACE);
    EXPECT_STREQ(out.r.name, "p");
    EXPECT_STREQ(out.r.path, "/p:o/p:i/@p:x");
    Build b3;
    b3.attr(b3.el(nullptr, "a"), "x", "1", "urn:u", nullptr);
    EXPECT_EQ(write_with(b3.doc, o).status, GTEXT_XML_E_NAMESPACE);
  }
}

// Every level declares a prefix of its own and then uses the first one: a scan
// of the declarations in scope would cost the depth each time.
void deep_scope(Build & c, size_t depth) {
  Node * root = c.el(nullptr, "e", "urn:0", "p0");
  c.xmlns(root, "p0", "urn:0");
  Node * cur = root;
  for (size_t i = 1; i < depth; i++) {
    const std::string pfx = "p" + std::to_string(i);
    Node * e = c.el(cur, "e", "urn:0", "p0");
    c.xmlns(e, pfx.c_str(), "urn:" + std::to_string(i));
    cur = e;
  }
}

TEST(XmlWriter, ADeepScopeWithManyPrefixesIsReadBackAndIsNotQuadratic) {
  {
    const size_t depth = 2000;
    Build c;
    deep_scope(c, depth);
    Opts o = defaults();
    o.max_depth = depth + 1;
    const Out out = write_with(c.doc, o);
    ASSERT_EQ(out.status, GTEXT_XML_OK) << out.r.message;
    GTEXT_XML_Parse_Options po = gtext_xml_parse_options_default();
    po.max_depth = depth + 1;
    po.max_namespaces = depth + 1;
    Doc * back = nullptr;
    GTEXT_XML_Error err;
    ASSERT_EQ(gtext_xml_parse(out.bytes.data(), out.bytes.size(), &po, &back, &err), GTEXT_XML_OK)
        << err.message;
    EXPECT_TRUE(gtext_markup_equal(c.root(), gtext_markup_document_root(back)));
    gtext_markup_document_free(back);
  }
  {
    const size_t depth = 100000;
    Build c;
    deep_scope(c, depth);
    Opts o = defaults();
    o.max_depth = depth + 1;
    const auto start = std::chrono::steady_clock::now();
    const Out out = write_with(c.doc, o);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    ASSERT_EQ(out.status, GTEXT_XML_OK) << out.r.message;
    // A scan of the scope for each name costs depth squared, about ten billion
    // comparisons here; this takes a fraction of a second.
    EXPECT_LT(seconds, 5.0);
  }
}

// ---- encodings -----------------------------------------------------------

TEST(XmlWriter, EncodesEveryOutputEncodingByteForByte) {
  Build b;
  gtext_markup_document_set_xml_declaration(b.doc, "1.0", GTEXT_MARKUP_STANDALONE_UNSPECIFIED);
  Node * a = b.el(nullptr, "a");
  b.attr(a, "t", "\xC3\xA9\xE2\x82\xAC");
  b.text(a, "x\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80<");

  Opts o = defaults();
  o.encoding = GTEXT_XML_OUTPUT_UTF8;
  EXPECT_EQ(write_with(b.doc, o).bytes,
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<a t=\"\xC3\xA9\xE2\x82\xAC\">x\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80&lt;</a>");

  o.encoding = GTEXT_XML_OUTPUT_UTF16LE;
  const Out le = write_with(b.doc, o);
  ASSERT_EQ(le.status, GTEXT_XML_OK);
  std::string want = "\xFF\xFE" + le16("<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n<a t=\"")
      + std::string("\xE9\x00\xAC\x20", 4) + le16("\">x") + std::string("\xE9\x00\xAC\x20\x3D\xD8\x00\xDE", 8)
      + le16("&lt;</a>");
  EXPECT_EQ(le.bytes, want);

  o.encoding = GTEXT_XML_OUTPUT_UTF16BE;
  const Out be = write_with(b.doc, o);
  ASSERT_EQ(be.status, GTEXT_XML_OK);
  std::string be_want = "\xFE\xFF";
  for (char c : std::string("<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n<a t=\"")) {
    be_want += '\0';
    be_want += c;
  }
  be_want += std::string("\x00\xE9\x20\xAC", 4);
  for (char c : std::string("\">x")) {
    be_want += '\0';
    be_want += c;
  }
  be_want += std::string("\x00\xE9\x20\xAC\xD8\x3D\xDE\x00", 8);
  for (char c : std::string("&lt;</a>")) {
    be_want += '\0';
    be_want += c;
  }
  EXPECT_EQ(be.bytes, be_want);

  o.encoding = GTEXT_XML_OUTPUT_ISO_8859_1;
  const Out l1 = write_with(b.doc, o);
  EXPECT_EQ(l1.bytes,
      "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>\n"
      "<a t=\"\xE9&#x20AC;\">x\xE9&#x20AC;&#x1F600;&lt;</a>");
  EXPECT_EQ(l1.r.references_written, 3u);

  o.encoding = GTEXT_XML_OUTPUT_US_ASCII;
  const Out as = write_with(b.doc, o);
  EXPECT_EQ(as.bytes,
      "<?xml version=\"1.0\" encoding=\"US-ASCII\"?>\n"
      "<a t=\"&#xE9;&#x20AC;\">x&#xE9;&#x20AC;&#x1F600;&lt;</a>");
  EXPECT_EQ(as.r.references_written, 5u);

  // Every one of them is a document the parser reads back to the same tree.
  for (const Out * out : {&le, &be, &l1, &as}) {
    Doc * back = parse(out->bytes);
    ASSERT_NE(back, nullptr);
    EXPECT_TRUE(gtext_markup_equal(b.root(), gtext_markup_document_root(back)));
    gtext_markup_document_free(back);
  }
}

TEST(XmlWriter, ARepresentationThatNeedsNoReferenceIsNotGivenOne) {
  Build b;
  b.text(b.el(nullptr, "a"), "caf\xC3\xA9 \xC2\x80\xC2\x9F \xC3\xBF");
  Opts o = defaults();
  o.encoding = GTEXT_XML_OUTPUT_ISO_8859_1;
  const Out out = write_with(b.doc, o);
  EXPECT_EQ(out.bytes, "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>\n<a>caf\xE9 \x80\x9F \xFF</a>");
  EXPECT_EQ(out.r.references_written, 0u);
  Doc * back = parse(out.bytes);
  EXPECT_TRUE(gtext_markup_equal(b.root(), gtext_markup_document_root(back)));
  gtext_markup_document_free(back);
}

TEST(XmlWriter, ACharacterTheEncodingLacksIsARefusalWhereNoReferenceCanStandIn) {
  struct C {
    const char * what;
    std::function<void(Build &)> build;
    const char * path;
  };
  const std::vector<C> cases = {
      {"element name", [](Build & b) { b.el(nullptr, "\xC3\xA9"); }, "/\xC3\xA9"},
      {"attribute name", [](Build & b) { b.attr(b.el(nullptr, "a"), "\xC3\xA9", "1"); }, "/a/@\xC3\xA9"},
      {"comment", [](Build & b) { b.comment(b.el(nullptr, "a"), "\xC3\xA9"); }, "/a/comment()"},
      {"PI data", [](Build & b) { b.pi(b.el(nullptr, "a"), "t", "\xC3\xA9"); },
          "/a/processing-instruction(t)"},
      {"PI target", [](Build & b) { b.pi(b.el(nullptr, "a"), "\xC3\xA9", ""); },
          "/a/processing-instruction(\xC3\xA9)"},
      {"CDATA", [](Build & b) { b.cdata(b.el(nullptr, "a"), "\xC3\xA9"); }, "/a/cdata()"},
      {"prefix", [](Build & b) { b.xmlns(b.el(nullptr, "a", "urn:u", "\xC3\xA9"), "\xC3\xA9", "urn:u"); },
          "/\xC3\xA9:a"},
      {"doctype identifier",
          [](Build & b) { b.doctype("a", nullptr, "\xC3\xA9.dtd"); b.el(nullptr, "a"); }, "/doctype()"},
  };
  for (const C & c : cases) {
    SCOPED_TRACE(c.what);
    Build b;
    c.build(b);
    Opts o = defaults();
    o.encoding = GTEXT_XML_OUTPUT_US_ASCII;
    Collect sink;
    GTEXT_XML_Write_Result r;
    const GTEXT_Markup_Sink s = sink.sink();
    EXPECT_EQ(gtext_xml_write(&s, b.doc, &o, &r), GTEXT_XML_E_UNREPRESENTABLE);
    EXPECT_STREQ(r.path, c.path);
    EXPECT_EQ(sink.calls, 0u);
    // UTF-8 and UTF-16 spell all of them, and Latin-1 is the one that has e-acute.
    o.encoding = GTEXT_XML_OUTPUT_UTF8;
    EXPECT_EQ(write_with(b.doc, o).status, GTEXT_XML_OK);
    o.encoding = GTEXT_XML_OUTPUT_UTF16BE;
    EXPECT_EQ(write_with(b.doc, o).status, GTEXT_XML_OK);
    o.encoding = GTEXT_XML_OUTPUT_ISO_8859_1;
    EXPECT_EQ(write_with(b.doc, o).status, GTEXT_XML_OK);
  }
  Build b;
  b.comment(b.el(nullptr, "a"), "\xE2\x82\xAC");
  Opts o = defaults();
  o.encoding = GTEXT_XML_OUTPUT_ISO_8859_1;
  EXPECT_EQ(write_with(b.doc, o).status, GTEXT_XML_E_UNREPRESENTABLE);
}

TEST(XmlWriter, TheXmlDeclarationFollowsTheDocumentAndTheEncoding) {
  Build none;
  none.el(nullptr, "a");
  Build declared;
  gtext_markup_document_set_xml_declaration(declared.doc, "1.0", GTEXT_MARKUP_STANDALONE_NO);
  declared.el(nullptr, "a");
  Opts o = defaults();
  EXPECT_EQ(write_with(none.doc, o).bytes, "<a/>");
  EXPECT_EQ(write_with(declared.doc, o).bytes, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n<a/>");

  o.xml_declaration = GTEXT_XML_DECLARATION_ALWAYS;
  EXPECT_EQ(write_with(none.doc, o).bytes, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<a/>");
  o.xml_declaration = GTEXT_XML_DECLARATION_NEVER;
  EXPECT_EQ(write_with(declared.doc, o).bytes, "<a/>");

  // AUTO declares when the encoding cannot be read without one.
  o.xml_declaration = GTEXT_XML_DECLARATION_AUTO;
  o.encoding = GTEXT_XML_OUTPUT_ISO_8859_1;
  EXPECT_EQ(write_with(none.doc, o).bytes, "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>\n<a/>");
  // US-ASCII output is UTF-8 already, and needs none; a document that has one
  // keeps it, naming the output's encoding.
  o.encoding = GTEXT_XML_OUTPUT_US_ASCII;
  EXPECT_EQ(write_with(none.doc, o).bytes, "<a/>");
  EXPECT_EQ(write_with(declared.doc, o).bytes,
      "<?xml version=\"1.0\" encoding=\"US-ASCII\" standalone=\"no\"?>\n<a/>");
  // UTF-16 has its mark.
  o.encoding = GTEXT_XML_OUTPUT_UTF16LE;
  EXPECT_EQ(write_with(none.doc, o).bytes, std::string("\xFF\xFE") + le16("<a/>"));

  // The encoding named is the output's, not the document's own label.
  Doc * latin = parse("<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><a/>");
  o.encoding = GTEXT_XML_OUTPUT_UTF8;
  EXPECT_EQ(write_with(latin, o).bytes, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<a/>");
  gtext_markup_document_free(latin);

  // A version XML 1.0 cannot be written as.
  Build odd;
  gtext_markup_document_set_xml_declaration(odd.doc, "1.1", GTEXT_MARKUP_STANDALONE_UNSPECIFIED);
  odd.el(nullptr, "a");
  const Out out = write_default(odd.doc);
  EXPECT_EQ(out.status, GTEXT_XML_E_UNSUPPORTED_VERSION);
  EXPECT_STREQ(out.r.name, "1.1");
  // Leaving the declaration out would hide it, so NEVER does not allow it.
  o.xml_declaration = GTEXT_XML_DECLARATION_NEVER;
  EXPECT_EQ(write_with(odd.doc, o).status, GTEXT_XML_E_UNSUPPORTED_VERSION);
  o.xml_declaration = GTEXT_XML_DECLARATION_ALWAYS;
  EXPECT_EQ(write_with(odd.doc, o).status, GTEXT_XML_E_UNSUPPORTED_VERSION);
}

TEST(XmlWriter, EmptyElementsAreSelfClosedOrExpanded) {
  Build b;
  Node * a = b.el(nullptr, "a");
  b.el(a, "e");
  b.text(b.el(a, "t"), "");
  Opts o = defaults();
  EXPECT_EQ(write_with(b.doc, o).bytes, "<a><e/><t/></a>");
  o.expand_empty_elements = true;
  EXPECT_EQ(write_with(b.doc, o).bytes, "<a><e></e><t></t></a>");
  Doc * doc = parse("<a><e/><f></f></a>");
  EXPECT_EQ(write_with(doc, o).bytes, "<a><e></e><f></f></a>");
  gtext_markup_document_free(doc);
}

TEST(XmlWriter, BadArgumentsAndOptionsAreRefusedAsInvalid) {
  Build b;
  b.el(nullptr, "a");
  Collect sink;
  const GTEXT_Markup_Sink s = sink.sink();
  GTEXT_XML_Write_Result r;
  EXPECT_EQ(gtext_xml_write(nullptr, b.doc, nullptr, &r), GTEXT_XML_E_INVALID);
  EXPECT_EQ(r.code, GTEXT_XML_E_INVALID);
  const GTEXT_Markup_Sink none = {nullptr, nullptr};
  EXPECT_EQ(gtext_xml_write(&none, b.doc, nullptr, &r), GTEXT_XML_E_INVALID);
  EXPECT_EQ(gtext_xml_write(&s, nullptr, nullptr, &r), GTEXT_XML_E_INVALID);
  EXPECT_EQ(gtext_xml_write(&s, nullptr, nullptr, nullptr), GTEXT_XML_E_INVALID);
  char * buf = nullptr;
  EXPECT_EQ(gtext_xml_write_buffer(nullptr, nullptr, &buf, nullptr, &r), GTEXT_XML_E_INVALID);
  EXPECT_EQ(gtext_xml_write_buffer(b.doc, nullptr, nullptr, nullptr, &r), GTEXT_XML_E_INVALID);
  gtext_xml_write_buffer_free(b.doc, nullptr, nullptr);
  gtext_xml_write_buffer_free(nullptr, nullptr, nullptr);

  auto bad = [&](auto mutate) {
    Opts o = defaults();
    mutate(o);
    EXPECT_EQ(write_with(b.doc, o).status, GTEXT_XML_E_INVALID);
  };
  bad([](Opts & o) { o.encoding = static_cast<GTEXT_XML_Output_Encoding>(9); });
  bad([](Opts & o) { o.encoding = static_cast<GTEXT_XML_Output_Encoding>(-1); });
  bad([](Opts & o) { o.xml_declaration = static_cast<GTEXT_XML_Declaration_Mode>(7); });
  bad([](Opts & o) { o.indent = "x"; });
  bad([](Opts & o) { o.indent = " \n"; });
  bad([](Opts & o) { o.newline = ""; });
  bad([](Opts & o) { o.newline = " "; });
  bad([](Opts & o) { o.newline = "\n "; });
  EXPECT_EQ(sink.calls, 0u);
}

// ---- pretty printing -----------------------------------------------------

Opts pretty(const char * indent = "  ") {
  Opts o = defaults();
  o.indent = indent;
  return o;
}

TEST(XmlWriter, IndentsOnlyWhereChildrenAreAllNotText) {
  Doc * doc = parse(
      "<a><b><c/><!--x--><?p q?></b><m>text<n><o/></n>more</m><d><e/></d><f/></a>");
  const Out out = write_with(doc, pretty());
  ASSERT_EQ(out.status, GTEXT_XML_OK);
  EXPECT_EQ(out.bytes,
      "<a>\n"
      "  <b>\n"
      "    <c/>\n"
      "    <!--x-->\n"
      "    <?p q?>\n"
      "  </b>\n"
      "  <m>text<n><o/></n>more</m>\n"
      "  <d>\n"
      "    <e/>\n"
      "  </d>\n"
      "  <f/>\n"
      "</a>\n");
  // a, b and d were laid out; m was mixed. n and o are below the mixed
  // element, and left as they were (an element with children counts once).
  EXPECT_EQ(out.r.elements_indented, 3u);
  EXPECT_EQ(out.r.elements_skipped_mixed, 2u);
  EXPECT_EQ(out.r.elements_skipped_preserve, 0u);
  gtext_markup_document_free(doc);
}

TEST(XmlWriter, NeverIndentsInOrBelowXmlSpacePreserve) {
  Doc * doc = parse(
      "<a><p xml:space=\"preserve\"><q><r/></q><s xml:space=\"default\"><t/></s></p>"
      "<u xml:space=\"default\"><v/></u></a>");
  const Out out = write_with(doc, pretty());
  EXPECT_EQ(out.bytes,
      "<a>\n"
      "  <p xml:space=\"preserve\"><q><r/></q><s xml:space=\"default\"><t/></s></p>\n"
      "  <u xml:space=\"default\">\n"
      "    <v/>\n"
      "  </u>\n"
      "</a>\n");
  EXPECT_EQ(out.r.elements_indented, 2u);          // a, u
  EXPECT_EQ(out.r.elements_skipped_preserve, 3u);  // p, q, s
  EXPECT_EQ(out.r.elements_skipped_mixed, 0u);
  gtext_markup_document_free(doc);
}

TEST(XmlWriter, TheNewlineAndIndentAreTheCallers) {
  Doc * doc = parse("<?xml version=\"1.0\"?><!--c--><a><b/></a><?p?>");
  Opts o = pretty("\t");
  o.newline = "\r\n";
  EXPECT_EQ(write_with(doc, o).bytes,
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<!--c-->\r\n<a>\r\n\t<b/>\r\n</a>\r\n<?p?>\r\n");
  o = pretty("");
  EXPECT_EQ(write_with(doc, o).bytes,
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!--c-->\n<a>\n<b/>\n</a>\n<?p?>\n");
  // Compact output still puts the top-level items on lines of their own, with
  // the newline asked for, and no line after the last.
  o = defaults();
  o.newline = "\r";
  EXPECT_EQ(write_with(doc, o).bytes,
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r<!--c-->\r<a><b/></a>\r<?p?>");
  gtext_markup_document_free(doc);
}

TEST(XmlWriter, PrettyOutputIsReadBackAsTheSameTreePlusWhiteSpaceAndWritesIdentically) {
  Doc * doc = parse("<a><b><c x=\"1\"/><d>t</d></b><!--k--></a>");
  const Out first = write_with(doc, pretty());
  Doc * back = parse(first.bytes);
  ASSERT_NE(back, nullptr);
  // The white space is text now, so the element that held only elements holds
  // text too; it is not the original tree.
  EXPECT_FALSE(gtext_markup_equal(gtext_markup_document_root(doc), gtext_markup_document_root(back)));
  // Written again the same way it is byte for byte what was read.
  EXPECT_EQ(write_with(back, pretty()).bytes, first.bytes);
  // And compact output of the read-back tree is that text without the final line.
  EXPECT_EQ(write_default(back).bytes, first.bytes.substr(0, first.bytes.size() - 1));
  // With the white space the writer added removed, it is the original again.
  std::vector<Node *> kill;
  for (Node * n = gtext_markup_node_next_in_order(gtext_markup_document_root(back), gtext_markup_document_root(back), false); n;
      n = gtext_markup_node_next_in_order(gtext_markup_document_root(back), n, false)) {
    size_t len = 0;
    const char * data = gtext_markup_node_data(n, &len);
    if (gtext_markup_node_kind(n) == GTEXT_MARKUP_KIND_TEXT && data
        && std::string(data, len).find_first_not_of(" \n") == std::string::npos) {
      kill.push_back(n);
    }
  }
  for (Node * n : kill) {
    gtext_markup_remove(n);
    gtext_markup_node_free(n);
  }
  EXPECT_TRUE(gtext_markup_equal(gtext_markup_document_root(doc), gtext_markup_document_root(back)));
  gtext_markup_document_free(doc);
  gtext_markup_document_free(back);
}

// ---- the document type ---------------------------------------------------

TEST(XmlWriter, TheDocumentTypeIsWrittenFromItsNameAndIdentifiers) {
  auto doctype = [](const char * name, const char * pub, const char * sys) {
    Build b;
    b.doctype(name, pub, sys);
    b.el(nullptr, "r");
    const Out out = write_default(b.doc);
    EXPECT_EQ(out.status, GTEXT_XML_OK) << out.r.message;
    EXPECT_TRUE(out.r.doctype_without_subset);
    return out.bytes;
  };
  EXPECT_EQ(doctype("r", nullptr, nullptr), "<!DOCTYPE r>\n<r/>");
  EXPECT_EQ(doctype("r", nullptr, "s.dtd"), "<!DOCTYPE r SYSTEM \"s.dtd\">\n<r/>");
  EXPECT_EQ(doctype("r", "-//A//EN", "s.dtd"), "<!DOCTYPE r PUBLIC \"-//A//EN\" \"s.dtd\">\n<r/>");
  EXPECT_EQ(doctype("r", "", ""), "<!DOCTYPE r PUBLIC \"\" \"\">\n<r/>");
  EXPECT_EQ(doctype("r", "a'b", "x\"y"), "<!DOCTYPE r PUBLIC \"a'b\" 'x\"y'>\n<r/>");
  EXPECT_EQ(doctype("p:r", nullptr, "a<b>&c"), "<!DOCTYPE p:r SYSTEM \"a<b>&c\">\n<r/>");
  // A document with no doctype does not say it dropped one.
  Build none;
  none.el(nullptr, "r");
  EXPECT_FALSE(write_default(none.doc).r.doctype_without_subset);

  // A parsed doctype with an internal subset is written without it, and says so.
  Doc * doc = parse("<!DOCTYPE r [<!ENTITY e \"x\">]><r/>");
  const Out out = write_default(doc);
  EXPECT_EQ(out.bytes, "<!DOCTYPE r>\n<r/>");
  EXPECT_TRUE(out.r.doctype_without_subset);
  gtext_markup_document_free(doc);
}

// ---- text nodes XML cannot tell apart ------------------------------------

TEST(XmlWriter, EmptyAndAdjacentTextNodesAreCountedBecauseAReadBackMergesThem) {
  Build b;
  Node * a = b.el(nullptr, "a");
  b.text(a, "x");
  b.text(a, "");
  b.text(a, "y");
  b.el(a, "e");
  b.text(a, "");
  b.text(a, "z");
  b.text(a, "w");
  const Out out = write_default(b.doc);
  EXPECT_EQ(out.bytes, "<a>xy<e/>zw</a>");
  EXPECT_EQ(out.r.text_nodes_coalesced, 4u);  // "", "y" (after x), "", "w"
  Doc * back = parse(out.bytes);
  EXPECT_EQ(gtext_markup_node_child_count(gtext_markup_document_element(back)), 3u);
  EXPECT_EQ(write_default(back).bytes, out.bytes);
  EXPECT_EQ(write_default(back).r.text_nodes_coalesced, 0u);
  gtext_markup_document_free(back);
  // An element with only empty text is an empty element, written and read as
  // one, so that writing what was read back is the same.
  Build c;
  c.text(c.el(nullptr, "a"), "");
  EXPECT_EQ(write_default(c.doc).bytes, "<a/>");
}

// ---- limits ----------------------------------------------------------------

std::string nest(size_t depth) {
  std::string s;
  for (size_t i = 0; i < depth; i++) {
    s += "<a>";
  }
  for (size_t i = 0; i < depth; i++) {
    s += "</a>";
  }
  return s;
}

TEST(XmlWriter, TheDepthLimitRefusesAtExactlyItsValue) {
  GTEXT_XML_Parse_Options po = gtext_xml_parse_options_default();
  po.max_depth = 1000;
  GTEXT_XML_Error err;
  Doc * doc = nullptr;
  const std::string text = nest(257);
  ASSERT_EQ(gtext_xml_parse(text.data(), text.size(), &po, &doc, &err), GTEXT_XML_OK);
  // The default is 256 levels, the root element being the first.
  Out out = write_default(doc);
  EXPECT_EQ(out.status, GTEXT_XML_E_LIMIT_DEPTH);
  Opts o = defaults();
  o.max_depth = 257;
  EXPECT_EQ(write_with(doc, o).status, GTEXT_XML_OK);
  o.max_depth = 256;
  EXPECT_EQ(write_with(doc, o).status, GTEXT_XML_E_LIMIT_DEPTH);
  o.max_depth = 1;
  EXPECT_EQ(write_with(doc, o).status, GTEXT_XML_E_LIMIT_DEPTH);
  gtext_markup_document_free(doc);
  Doc * d1 = parse("<a/>");
  o.max_depth = 1;
  EXPECT_EQ(write_with(d1, o).status, GTEXT_XML_OK);
  gtext_markup_document_free(d1);
  // A path this long keeps its end.
  EXPECT_EQ(out.r.path[0], '.');
  EXPECT_EQ(std::string(out.r.path).compare(0, 3, "..."), 0);
  EXPECT_LT(strlen(out.r.path), sizeof(out.r.path));
}

TEST(XmlWriter, TheOutputLimitRefusesBeforeAnythingIsWritten) {
  Doc * doc = parse("<a>0123456789</a>");
  const std::string want = "<a>0123456789</a>";
  Opts o = defaults();
  o.max_output_bytes = want.size();
  EXPECT_EQ(write_with(doc, o).bytes, want);
  o.max_output_bytes = want.size() - 1;
  Collect sink;
  GTEXT_XML_Write_Result r;
  const GTEXT_Markup_Sink s = sink.sink();
  EXPECT_EQ(gtext_xml_write(&s, doc, &o, &r), GTEXT_XML_E_LIMIT_OUTPUT);
  EXPECT_EQ(sink.calls, 0u);
  EXPECT_EQ(r.bytes_written, 0u);
  // UTF-16 counts its own bytes, mark included.
  o.encoding = GTEXT_XML_OUTPUT_UTF16LE;
  o.max_output_bytes = 2 + 2 * want.size();
  EXPECT_EQ(write_with(doc, o).status, GTEXT_XML_OK);
  o.max_output_bytes -= 1;
  EXPECT_EQ(write_with(doc, o).status, GTEXT_XML_E_LIMIT_OUTPUT);
  // A reference counts as the bytes it is.
  Build b;
  b.text(b.el(nullptr, "a"), "\xE2\x82\xAC");
  o = defaults();
  o.encoding = GTEXT_XML_OUTPUT_US_ASCII;
  const size_t exact = write_with(b.doc, o).bytes.size();
  o.max_output_bytes = exact;
  EXPECT_EQ(write_with(b.doc, o).status, GTEXT_XML_OK);
  o.max_output_bytes = exact - 1;
  EXPECT_EQ(write_with(b.doc, o).status, GTEXT_XML_E_LIMIT_OUTPUT);
  gtext_markup_document_free(doc);
}

TEST(XmlWriter, ATreeOfAHundredThousandLevelsNeedsNoCStackAndNoQuadraticCost) {
  const size_t depth = 100000;
  Build b;
  Node * cur = nullptr;
  for (size_t i = 0; i < depth; i++) {
    cur = b.el(cur, "a");
  }
  b.text(cur, "leaf");
  Opts o = defaults();
  o.max_depth = depth;
  const Out out = write_with(b.doc, o);
  ASSERT_EQ(out.status, GTEXT_XML_OK) << out.r.message;
  EXPECT_EQ(out.bytes.size(), depth * 3 + 4 + depth * 4);
  GTEXT_XML_Parse_Options po = gtext_xml_parse_options_default();
  po.max_depth = depth;
  Doc * back = nullptr;
  GTEXT_XML_Error err;
  ASSERT_EQ(gtext_xml_parse(out.bytes.data(), out.bytes.size(), &po, &back, &err), GTEXT_XML_OK)
      << err.message;
  EXPECT_TRUE(gtext_markup_equal(b.root(), gtext_markup_document_root(back)));
  gtext_markup_document_free(back);

  // Pretty printing a tree that deep would write depth squared spaces: refused
  // by the output limit, quickly, and nothing is written.
  Doc * deep_elements = nullptr;
  const std::string text = nest(depth);
  ASSERT_EQ(gtext_xml_parse(text.data(), text.size(), &po, &deep_elements, &err), GTEXT_XML_OK);
  Opts p = pretty();
  p.max_depth = depth;
  Collect sink;
  GTEXT_XML_Write_Result r;
  const GTEXT_Markup_Sink s = sink.sink();
  EXPECT_EQ(gtext_xml_write(&s, deep_elements, &p, &r), GTEXT_XML_E_LIMIT_OUTPUT);
  EXPECT_EQ(sink.calls, 0u);
  // A shallower tree fits, and its layout is exact.
  const std::string small = nest(300);
  Doc * s300 = nullptr;
  po.max_depth = 300;
  ASSERT_EQ(gtext_xml_parse(small.data(), small.size(), &po, &s300, &err), GTEXT_XML_OK);
  p = pretty(" ");
  p.max_depth = 300;
  const Out pr = write_with(s300, p);
  ASSERT_EQ(pr.status, GTEXT_XML_OK);
  std::string want;
  for (size_t i = 0; i < 300; i++) {
    want += (i ? "\n" : "") + std::string(i, ' ') + (i == 299 ? "<a/>" : "<a>");
  }
  for (size_t i = 299; i-- > 0;) {
    want += "\n" + std::string(i, ' ') + "</a>";
  }
  want += "\n";
  EXPECT_EQ(pr.bytes, want);
  EXPECT_EQ(pr.r.bytes_written, want.size());
  gtext_markup_document_free(s300);
  gtext_markup_document_free(deep_elements);
}

// ---- the sink and the allocator failing at every point ----------------------

// A document that makes several chunks and exercises every kind of output.
Doc * rich() {
  std::string text = "<?xml version=\"1.0\"?><!DOCTYPE r SYSTEM \"s\"><r xmlns:p=\"urn:p\">";
  for (int i = 0; i < 400; i++) {
    text += "<p:i n=\"" + std::to_string(i) + "\">v&amp;" + std::to_string(i) + "<!--c-->"
        "<![CDATA[d]]><?t x?></p:i>";
  }
  text += "</r>";
  return parse(text);
}

TEST(XmlWriter, ASinkThatFailsAtAnyCallEndsTheWriteWithWriteAndNothingLeaks) {
  Doc * doc = rich();
  ASSERT_NE(doc, nullptr);
  size_t successful_calls = 0;
  {
    Collect all;
    const GTEXT_Markup_Sink s = all.sink();
    ASSERT_EQ(gtext_xml_write(&s, doc, nullptr, nullptr), GTEXT_XML_OK);
    successful_calls = all.calls;
    EXPECT_GT(successful_calls, 3u);
  }
  FaultAllocator fa;
  Opts o = defaults();
  o.allocator = &fa.alloc;
  for (size_t k = 1; k <= successful_calls; k++) {
    Collect sink;
    sink.fail_on = k;
    const GTEXT_Markup_Sink s = sink.sink();
    GTEXT_XML_Write_Result r;
    EXPECT_EQ(gtext_xml_write(&s, doc, &o, &r), GTEXT_XML_E_WRITE) << k;
    EXPECT_EQ(r.code, GTEXT_XML_E_WRITE);
    EXPECT_STREQ(r.path, "");
    EXPECT_EQ(sink.calls, k);                 // stopped at the failure
    EXPECT_TRUE(fa.live.empty()) << k;
    EXPECT_EQ(fa.bad_frees, 0u);
  }
  // A sink that says it ran out of memory is reported as that.
  Collect oom_sink;
  oom_sink.fail_on = 2;
  oom_sink.fail_with = GTEXT_MARKUP_E_OOM;
  const GTEXT_Markup_Sink s = oom_sink.sink();
  EXPECT_EQ(gtext_xml_write(&s, doc, &o, nullptr), GTEXT_XML_E_OOM);
  gtext_markup_document_free(doc);
}

void sweep_allocator(const Doc * doc, Opts o, const char * label) {
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
      ASSERT_EQ(st, GTEXT_XML_OK);
      gtext_xml_write_buffer_free(doc, &o, buf);
      break;
    }
    failures++;
    ASSERT_EQ(st, GTEXT_XML_E_OOM) << n;
    ASSERT_EQ(r.code, GTEXT_XML_E_OOM);
    ASSERT_EQ(buf, nullptr);
    ASSERT_TRUE(fa.live.empty()) << n;
    ASSERT_EQ(fa.bad_frees, 0u);
    ASSERT_LT(n, 10000u);
  }
  EXPECT_GE(failures, 3u);
  EXPECT_TRUE(fa.live.empty());
  EXPECT_EQ(fa.bad_frees, 0u);
}

TEST(XmlWriter, TheAllocatorFailingAtEveryPointEndsTheWriteWithOomAndNothingLeaks) {
  Doc * doc = rich();
  Opts o = defaults();
  sweep_allocator(doc, o, "compact");
  o.indent = "  ";
  sweep_allocator(doc, o, "pretty");
  o.indent = nullptr;
  o.encoding = GTEXT_XML_OUTPUT_UTF16BE;
  sweep_allocator(doc, o, "utf-16");
  o.encoding = GTEXT_XML_OUTPUT_US_ASCII;
  sweep_allocator(doc, o, "ascii");
  gtext_markup_document_free(doc);

  // With declare_namespaces, whose working state is larger.
  Build b;
  Node * a = b.el(nullptr, "a", "urn:u", "p");
  for (int i = 0; i < 12; i++) {
    b.attr(a, ("x" + std::to_string(i)).c_str(), "1", ("urn:" + std::to_string(i)).c_str(),
        ("q" + std::to_string(i)).c_str());
  }
  o = defaults();
  o.declare_namespaces = true;
  sweep_allocator(b.doc, o, "declare");

  // And a refusal part way through, with the allocator failing in turn: the
  // status is the refusal or OOM, and in both cases nothing leaks.
  Build bad;
  Node * r = bad.el(nullptr, "a");
  for (int i = 0; i < 40; i++) {
    bad.el(r, "i");
  }
  bad.comment(r, "--");
  FaultAllocator fa;
  o = defaults();
  o.allocator = &fa.alloc;
  for (size_t n = 1; n < 40; n++) {
    fa.arm(n);
    const Out out = write_with(bad.doc, o);
    fa.disarm();
    EXPECT_TRUE(out.status == GTEXT_XML_E_OOM || out.status == GTEXT_XML_E_UNREPRESENTABLE);
    EXPECT_TRUE(fa.live.empty());
    EXPECT_EQ(fa.bad_frees, 0u);
  }
}

TEST(XmlWriter, TheDocumentsAllocatorIsUsedWhenTheOptionsHaveNone) {
  FaultAllocator fa;
  Build b(&fa.alloc);
  Node * a = b.el(nullptr, "a", "urn:u");
  b.xmlns(a, nullptr, "urn:u");
  b.text(a, "t");
  const size_t before = fa.live.size();
  size_t seen_failure = 0;
  for (size_t n = 1; n < 100; n++) {
    fa.arm(n);
    const Out out = write_default(b.doc);
    fa.disarm();
    if (!fa.failed) {
      EXPECT_EQ(out.status, GTEXT_XML_OK);
      break;
    }
    seen_failure++;
    EXPECT_EQ(out.status, GTEXT_XML_E_OOM);
    EXPECT_EQ(fa.live.size(), before);
  }
  EXPECT_GE(seen_failure, 2u);
  EXPECT_EQ(fa.live.size(), before);
  EXPECT_EQ(fa.bad_frees, 0u);
}

// ---- files -------------------------------------------------------------

struct TempDir {
  std::string path;
  TempDir() {
    path = "build/xml-writer-test-" + std::to_string(::getpid());
    ::mkdir("build", 0777);
    ::mkdir(path.c_str(), 0777);
  }
  ~TempDir() {
    if (DIR * d = ::opendir(path.c_str())) {
      while (dirent * e = ::readdir(d)) {
        if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, "..")) {
          ::unlink((path + "/" + e->d_name).c_str());
        }
      }
      ::closedir(d);
    }
    ::rmdir(path.c_str());
  }
  std::string file(const char * name) const {
    return path + "/" + name;
  }
  std::vector<std::string> list() const {
    std::vector<std::string> names;
    if (DIR * d = ::opendir(path.c_str())) {
      while (dirent * e = ::readdir(d)) {
        if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, "..")) {
          names.push_back(e->d_name);
        }
      }
      ::closedir(d);
    }
    return names;
  }
};

std::string slurp(const std::string & path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

TEST(XmlWriter, WriteFileThenParseFileGivesAnEqualTree) {
  TempDir dir;
  Doc * doc = rich();
  const std::string path = dir.file("out.xml");
  GTEXT_XML_Write_Result r;
  ASSERT_EQ(gtext_xml_write_file(path.c_str(), doc, nullptr, &r), GTEXT_XML_OK) << r.message;
  EXPECT_EQ(slurp(path), write_default(doc).bytes);
  EXPECT_EQ(dir.list(), std::vector<std::string>{"out.xml"});  // no temporary left over

  Doc * back = nullptr;
  GTEXT_XML_Error err;
  ASSERT_EQ(gtext_xml_parse_file(path.c_str(), nullptr, &back, &err), GTEXT_XML_OK) << err.message;
  EXPECT_EQ(err.code, GTEXT_XML_OK);
  EXPECT_TRUE(gtext_markup_equal(gtext_markup_document_root(doc), gtext_markup_document_root(back)));
  gtext_markup_document_free(back);

  // Another encoding, through the file.
  Opts o = defaults();
  o.encoding = GTEXT_XML_OUTPUT_UTF16LE;
  ASSERT_EQ(gtext_xml_write_file(path.c_str(), doc, &o, nullptr), GTEXT_XML_OK);
  ASSERT_EQ(gtext_xml_parse_file(path.c_str(), nullptr, &back, &err), GTEXT_XML_OK);
  EXPECT_TRUE(gtext_markup_equal(gtext_markup_document_root(doc), gtext_markup_document_root(back)));
  gtext_markup_document_free(back);
  gtext_markup_document_free(doc);
}

TEST(XmlWriter, AFileThatCannotBeReadIsReportedTheWayCsvAndJsonReportIt) {
  TempDir dir;
  Doc * back = reinterpret_cast<Doc *>(0x1);
  GTEXT_XML_Error err;
  EXPECT_EQ(gtext_xml_parse_file(dir.file("missing.xml").c_str(), nullptr, &back, &err),
      GTEXT_XML_E_INVALID);
  EXPECT_EQ(back, reinterpret_cast<Doc *>(0x1));  // written only on success
  EXPECT_EQ(err.code, GTEXT_XML_E_INVALID);
  EXPECT_STREQ(err.message, "No such file or directory");
  EXPECT_EQ(err.line, 1u);
  EXPECT_EQ(err.column, 1u);
  EXPECT_EQ(err.byte_offset, 0u);
  // The CSV and JSON file functions say the same thing for the same cause.
  GTEXT_CSV_Error cerr;
  memset(&cerr, 0, sizeof(cerr));
  EXPECT_EQ(gtext_csv_parse_file(dir.file("missing.csv").c_str(), nullptr, &cerr), nullptr);
  EXPECT_STREQ(cerr.message, err.message);

  EXPECT_EQ(gtext_xml_parse_file(nullptr, nullptr, &back, &err), GTEXT_XML_E_INVALID);
  EXPECT_EQ(gtext_xml_parse_file(dir.file("x").c_str(), nullptr, nullptr, &err), GTEXT_XML_E_INVALID);
  EXPECT_EQ(gtext_xml_parse_file(nullptr, nullptr, &back, nullptr), GTEXT_XML_E_INVALID);

  // A directory is not a file.
  EXPECT_EQ(gtext_xml_parse_file(dir.path.c_str(), nullptr, &back, &err), GTEXT_XML_E_INVALID);

  // A file that is not well-formed is the parser's refusal, positioned in the file.
  std::ofstream(dir.file("bad.xml")) << "<a>\n  <b></a>";
  ASSERT_EQ(gtext_xml_parse_file(dir.file("bad.xml").c_str(), nullptr, &back, &err),
      GTEXT_XML_E_MISMATCHED_TAG);
  EXPECT_EQ(err.line, 2u);
  EXPECT_EQ(err.column, 6u);

  // An empty file is not a document.
  std::ofstream(dir.file("empty.xml")) << "";
  EXPECT_NE(gtext_xml_parse_file(dir.file("empty.xml").c_str(), nullptr, &back, &err), GTEXT_XML_OK);

  // The input limit applies to the file, and is its own status.
  std::ofstream(dir.file("ok.xml")) << "<a>12345</a>";
  GTEXT_XML_Parse_Options po = gtext_xml_parse_options_default();
  po.max_input_bytes = 12;
  ASSERT_EQ(gtext_xml_parse_file(dir.file("ok.xml").c_str(), &po, &back, &err), GTEXT_XML_OK);
  gtext_markup_document_free(back);
  po.max_input_bytes = 11;
  back = nullptr;
  EXPECT_EQ(gtext_xml_parse_file(dir.file("ok.xml").c_str(), &po, &back, &err), GTEXT_XML_E_LIMIT_INPUT);
  EXPECT_EQ(back, nullptr);

  // The allocator reaches the file buffer and the tree: nothing leaks.
  FaultAllocator fa;
  po = gtext_xml_parse_options_default();
  po.allocator = &fa.alloc;
  for (size_t n = 1; n < 200; n++) {
    fa.arm(n);
    back = nullptr;
    const GTEXT_XML_Status st = gtext_xml_parse_file(dir.file("ok.xml").c_str(), &po, &back, &err);
    fa.disarm();
    if (!fa.failed) {
      ASSERT_EQ(st, GTEXT_XML_OK);
      gtext_markup_document_free(back);
      break;
    }
    EXPECT_EQ(st, GTEXT_XML_E_OOM) << n;
    EXPECT_EQ(back, nullptr);
    EXPECT_TRUE(fa.live.empty());
  }
  EXPECT_TRUE(fa.live.empty());
}

TEST(XmlWriter, AFailedFileWriteLeavesTheOldFileAndNoTemporary) {
  TempDir dir;
  const std::string path = dir.file("keep.xml");
  std::ofstream(path) << "<old/>";

  // A refused tree.
  Build bad;
  bad.comment(bad.el(nullptr, "a"), "--");
  GTEXT_XML_Write_Result r;
  EXPECT_EQ(gtext_xml_write_file(path.c_str(), bad.doc, nullptr, &r), GTEXT_XML_E_UNREPRESENTABLE);
  EXPECT_STREQ(r.path, "/a/comment()");
  EXPECT_EQ(slurp(path), "<old/>");
  EXPECT_EQ(dir.list(), std::vector<std::string>{"keep.xml"});

  // A refused tree, and no file there before: none appears.
  const std::string fresh = dir.file("fresh.xml");
  EXPECT_EQ(gtext_xml_write_file(fresh.c_str(), bad.doc, nullptr, nullptr), GTEXT_XML_E_UNREPRESENTABLE);
  EXPECT_EQ(dir.list(), std::vector<std::string>{"keep.xml"});

  // The output limit, which is a refusal too.
  Doc * doc = parse("<a>0123456789</a>");
  Opts o = defaults();
  o.max_output_bytes = 5;
  EXPECT_EQ(gtext_xml_write_file(path.c_str(), doc, &o, &r), GTEXT_XML_E_LIMIT_OUTPUT);
  EXPECT_EQ(slurp(path), "<old/>");

  // A refused tree is reported as that even when the path is hopeless: the tree
  // is checked before any file is made.
  const std::string hopeless = dir.file("no/such/dir/y.xml");
  EXPECT_EQ(gtext_xml_write_file(hopeless.c_str(), bad.doc, nullptr, &r), GTEXT_XML_E_UNREPRESENTABLE);
  EXPECT_STREQ(r.path, "/a/comment()");

  // A path in a directory that is not there, as CSV and JSON report it.
  const std::string nowhere = dir.file("no/such/dir/x.xml");
  EXPECT_NE(gtext_xml_write_file(nowhere.c_str(), doc, nullptr, &r), GTEXT_XML_OK);
  EXPECT_NE(r.message, nullptr);
  EXPECT_STREQ(r.path, "");

  // Success replaces it, atomically.
  EXPECT_EQ(gtext_xml_write_file(path.c_str(), doc, nullptr, &r), GTEXT_XML_OK);
  EXPECT_EQ(slurp(path), "<a>0123456789</a>");
  EXPECT_EQ(dir.list(), std::vector<std::string>{"keep.xml"});

  EXPECT_EQ(gtext_xml_write_file(nullptr, doc, nullptr, &r), GTEXT_XML_E_INVALID);
  EXPECT_EQ(gtext_xml_write_file(path.c_str(), nullptr, nullptr, &r), GTEXT_XML_E_INVALID);

  // The allocator failing at every point of a file write: an error, no leak,
  // and the file is the old one or the whole new one.
  FaultAllocator fa;
  o = defaults();
  o.allocator = &fa.alloc;
  for (size_t n = 1; n < 100; n++) {
    std::ofstream(path) << "<old/>";
    fa.arm(n);
    const GTEXT_XML_Status st = gtext_xml_write_file(path.c_str(), doc, &o, &r);
    fa.disarm();
    const std::string now = slurp(path);
    if (!fa.failed) {
      EXPECT_EQ(st, GTEXT_XML_OK);
      EXPECT_EQ(now, "<a>0123456789</a>");
      break;
    }
    EXPECT_EQ(st, GTEXT_XML_E_OOM) << n;
    EXPECT_EQ(now, "<old/>") << n;
    EXPECT_TRUE(fa.live.empty());
    EXPECT_EQ(dir.list(), std::vector<std::string>{"keep.xml"});
  }
  gtext_markup_document_free(doc);
}

// ---- shapes the tree does not allow, or reads back the same way ------------

TEST(XmlWriter, TheTreeWillNotHoldWhatTheWriterWouldHaveToGuardAgainst) {
  Build b;
  Node * text = nullptr;
  Node * cdata = nullptr;
  ASSERT_EQ(gtext_markup_create_text(b.doc, "x", 1, &text), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_create_cdata(b.doc, "x", 1, &cdata), GTEXT_MARKUP_OK);
  // Text and CDATA directly in the document.
  EXPECT_NE(gtext_markup_append_child(b.root(), text), GTEXT_MARKUP_OK);
  EXPECT_NE(gtext_markup_append_child(b.root(), cdata), GTEXT_MARKUP_OK);
  // A second root element; a document type after the root, or twice.
  b.el(nullptr, "a");
  Node * second = nullptr;
  ASSERT_EQ(gtext_markup_create_element(b.doc, nullptr, nullptr, "b", &second), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_append_child(b.root(), second), GTEXT_MARKUP_E_HIERARCHY);
  Node * dt = nullptr;
  ASSERT_EQ(gtext_markup_create_doctype(b.doc, "a", nullptr, nullptr, &dt), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_append_child(b.root(), dt), GTEXT_MARKUP_E_HIERARCHY);
  Build c;
  c.doctype("a", nullptr, nullptr);
  Node * dt2 = nullptr;
  ASSERT_EQ(gtext_markup_create_doctype(c.doc, "a", nullptr, nullptr, &dt2), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_append_child(c.root(), dt2), GTEXT_MARKUP_E_HIERARCHY);
  // And so the writer sees only a well-shaped document: write it.
  EXPECT_EQ(write_default(b.doc).bytes, "<a/>");
}

TEST(XmlWriter, TheTreeNormalizesEmptyNamespacesAndMergesAttributesByExpandedName) {
  // An empty namespace is none, so a prefix with one is a prefix with none.
  Build b;
  b.el(nullptr, "a", "", "p");
  const Out out = write_default(b.doc);
  EXPECT_EQ(out.status, GTEXT_XML_E_NAMESPACE);
  EXPECT_STREQ(out.r.name, "p");
  Build c;
  Node * e = c.el(nullptr, "a");
  c.attr(e, "x", "", "", "p");
  EXPECT_EQ(write_default(c.doc).status, GTEXT_XML_E_NAMESPACE);

  // Two attributes with the same namespace and local name under different
  // prefixes are one attribute: the second replaces the first.
  Build d;
  Node * a = d.el(nullptr, "a");
  d.xmlns(a, "p", "urn:u");
  d.xmlns(a, "q", "urn:u");
  d.attr(a, "x", "1", "urn:u", "p");
  d.attr(a, "x", "2", "urn:u", "q");
  EXPECT_EQ(gtext_markup_element_attribute_count(a), 3u);
  EXPECT_EQ(write_default(d.doc).bytes, "<a xmlns:p=\"urn:u\" xmlns:q=\"urn:u\" q:x=\"2\"/>");
  // The same plain name twice is one too.
  Build f;
  Node * g = f.el(nullptr, "a");
  f.attr(g, "x", "1");
  f.attr(g, "x", "2");
  EXPECT_EQ(gtext_markup_element_attribute_count(g), 1u);
  // Under the parser, that same pair is its error, so what the writer is given
  // can never be read back as a duplicate.
  GTEXT_XML_Error err;
  Doc * doc = nullptr;
  const std::string dup = "<a xmlns:p=\"urn:u\" xmlns:q=\"urn:u\" p:x=\"1\" q:x=\"2\"/>";
  EXPECT_EQ(gtext_xml_parse(dup.data(), dup.size(), nullptr, &doc, &err), GTEXT_XML_E_DUPLICATE_ATTRIBUTE);
}

TEST(XmlWriter, EmptyCdataAndCdataBesideTextAreReadBackAsTheyWereWritten) {
  Build b;
  Node * a = b.el(nullptr, "a");
  b.text(a, "t1");
  b.cdata(a, "");
  b.cdata(a, "c");
  b.text(a, "t2");
  b.cdata(a, "");
  const Out out = write_default(b.doc);
  EXPECT_EQ(out.bytes, "<a>t1<![CDATA[]]><![CDATA[c]]>t2<![CDATA[]]></a>");
  EXPECT_EQ(out.r.text_nodes_coalesced, 0u);
  Doc * back = parse(out.bytes);
  ASSERT_NE(back, nullptr);
  EXPECT_TRUE(gtext_markup_equal(b.root(), gtext_markup_document_root(back)));
  gtext_markup_document_free(back);
  // An element holding only an empty CDATA section is not an empty element.
  Build c;
  c.cdata(c.el(nullptr, "a"), "");
  EXPECT_EQ(write_default(c.doc).bytes, "<a><![CDATA[]]></a>");
}

TEST(XmlWriter, WithoutADeclarationIso88591OutputThatIsNotUtf8IsRefused) {
  Build b;
  b.text(b.el(nullptr, "a"), "caf\xC3\xA9");
  Opts o = defaults();
  o.encoding = GTEXT_XML_OUTPUT_ISO_8859_1;
  o.xml_declaration = GTEXT_XML_DECLARATION_NEVER;
  Out out = write_with(b.doc, o);
  EXPECT_EQ(out.status, GTEXT_XML_E_UNREPRESENTABLE);
  EXPECT_STREQ(out.r.path, "/a/text()");
  // Pure ASCII needs none; characters past Latin-1 become references, which are
  // ASCII.
  Build c;
  c.text(c.el(nullptr, "a"), "x\xE2\x82\xAC");
  EXPECT_EQ(write_with(c.doc, o).bytes, "<a>x&#x20AC;</a>");
  // With the declaration, the byte is fine.
  o.xml_declaration = GTEXT_XML_DECLARATION_ALWAYS;
  EXPECT_EQ(write_with(b.doc, o).status, GTEXT_XML_OK);
  // US-ASCII needs no declaration, and never has a high byte.
  o.encoding = GTEXT_XML_OUTPUT_US_ASCII;
  o.xml_declaration = GTEXT_XML_DECLARATION_NEVER;
  EXPECT_EQ(write_with(b.doc, o).bytes, "<a>caf&#xE9;</a>");
}

// ---- the result and the statuses -----------------------------------------

TEST(XmlWriter, TheResultIsClearedEachCallAndNullIsAccepted) {
  Doc * doc = parse("<a/>");
  GTEXT_XML_Write_Result r;
  std::memset(&r, 0xAB, sizeof(r));
  char * buf = nullptr;
  size_t len = 0;
  ASSERT_EQ(gtext_xml_write_buffer(doc, nullptr, &buf, &len, &r), GTEXT_XML_OK);
  EXPECT_EQ(r.code, GTEXT_XML_OK);
  EXPECT_EQ(r.message, nullptr);
  EXPECT_STREQ(r.path, "");
  EXPECT_STREQ(r.name, "");
  EXPECT_EQ(r.bytes_written, 4u);
  EXPECT_EQ(r.elements_indented, 0u);
  EXPECT_FALSE(r.doctype_without_subset);
  gtext_xml_write_buffer_free(doc, nullptr, buf);
  gtext_markup_document_free(doc);
}

TEST(XmlWriter, TheNewStatusesHaveNamesAndAreDistinct) {
  std::vector<std::string> names;
  for (int s = GTEXT_XML_OK; s <= GTEXT_XML_E_LIMIT_OUTPUT; s++) {
    const char * name = gtext_xml_status_string(static_cast<GTEXT_XML_Status>(s));
    ASSERT_NE(name, nullptr);
    EXPECT_STRNE(name, "unknown status") << s;
    for (const std::string & seen : names) {
      EXPECT_NE(seen, name);
    }
    names.push_back(name);
  }
}

// ---- a model that shares no code with the library ----------------------------

// XML_WRITER_SEEDS=20000 make test ... runs a soak: the same sweeps with that
// many seeds in place of the number written below.
uint32_t seeds(uint32_t normally) {
  const char * env = std::getenv("XML_WRITER_SEEDS");
  return env ? static_cast<uint32_t>(std::strtoul(env, nullptr, 10)) : normally;
}

struct MAttr {
  std::string ns, prefix, local, value;
};

struct MNode {
  enum Kind { ELEM, TEXT, CDATA, COMMENT, PI } kind = ELEM;
  std::string ns, prefix, local;  // element
  std::string data, target;       // text, CDATA, comment, PI
  std::vector<MAttr> attrs;
  std::vector<MNode> kids;
};

struct MDoc {
  std::vector<MNode> before, after;
  MNode root;
  bool declared = false;
};

class Generator {
 public:
  Generator(uint32_t seed, bool ascii_names) : rng(seed), ascii(ascii_names) {}

  MDoc make() {
    MDoc d;
    d.declared = chance(50);
    while (chance(30)) {
      d.before.push_back(misc());
    }
    scope.clear();
    d.root = element(0);
    while (chance(30)) {
      d.after.push_back(misc());
    }
    return d;
  }

 private:
  std::mt19937 rng;
  bool ascii;
  std::vector<std::pair<std::string, std::string>> scope;

  bool chance(int percent) {
    return static_cast<int>(rng() % 100) < percent;
  }
  size_t below(size_t n) {
    return rng() % n;
  }
  template <size_t N>
  std::string pick(const char * const (&pool)[N]) {
    return pool[below(N)];
  }

  std::string lookup(const std::string & prefix) const {
    for (size_t i = scope.size(); i > 0; i--) {
      if (scope[i - 1].first == prefix) {
        return scope[i - 1].second;
      }
    }
    return "";
  }

  std::string name() {
    static const char * const a[] = {"a", "b", "item", "_x", "a-b.c", "Z9"};
    static const char * const u[] = {"a", "b", "item", "\xC3\xA9", "\xE5\x90\x8D\xE5\x89\x8D", "x1"};
    return ascii ? pick(a) : pick(u);
  }

  std::string chars(const char * pool_chars, size_t pool_n, size_t max) {
    std::string s;
    const size_t n = below(max + 1);
    for (size_t i = 0; i < n; i++) {
      s += pool_chars[below(pool_n)];
    }
    return s;
  }

  std::string text_data(bool nonempty) {
    static const char * const pieces[] = {"a", "b", " ", "c", "<", ">", "&", "\"", "'", "\t", "\n",
        "\r", "]", "]]>", "\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9F\x98\x80", "&amp;", "x", "\r\n"};
    std::string s;
    const size_t n = below(8) + (nonempty ? 1 : 0);
    for (size_t i = 0; i < n; i++) {
      s += pick(pieces);
    }
    return s;
  }

  static void replace_all(std::string & s, const std::string & from, const std::string & to) {
    for (size_t at; (at = s.find(from)) != std::string::npos;) {
      s.replace(at, from.size(), to);
    }
  }

  std::string no_cr(std::string s) {
    replace_all(s, "\r", "_");
    return s;
  }

  MNode misc() {
    MNode n;
    if (chance(50)) {
      n.kind = MNode::COMMENT;
      n.data = no_cr(text_data(false));
      replace_all(n.data, "-", "+");
      if (chance(40)) {
        n.data = "a-b";
      }
    }
    else {
      n.kind = MNode::PI;
      static const char * const targets[] = {"pi", "t1", "x-y", "xmlx", "a.b"};
      n.target = pick(targets);
      n.data = no_cr(text_data(false));
      replace_all(n.data, "?>", "?_");
      while (!n.data.empty() && (n.data[0] == ' ' || n.data[0] == '\t' || n.data[0] == '\n')) {
        n.data[0] = 'q';
      }
    }
    return n;
  }

  MNode leaf() {
    MNode n;
    switch (below(4)) {
      case 0:
        n.kind = MNode::TEXT;
        n.data = text_data(true);
        break;
      case 1:
        n.kind = MNode::CDATA;
        n.data = no_cr(text_data(false));
        replace_all(n.data, "]]>", "]_>");
        break;
      default:
        n = misc();
        break;
    }
    return n;
  }

  MNode element(int depth) {
    MNode e;
    e.kind = MNode::ELEM;
    const size_t mark = scope.size();

    // Declarations of its own, some of them unused.
    static const char * const prefixes[] = {"", "a", "bb", "x"};
    static const char * const uris[] = {"urn:u1", "urn:u2", "http://e.example/\xC3\xA9"};
    std::vector<MAttr> attrs;
    auto declare = [&](const std::string & p, const std::string & uri) {
      for (const MAttr & a : attrs) {
        if (a.ns == NS_XMLNS && a.local == (p.empty() ? "xmlns" : p)) {
          return;
        }
      }
      MAttr a;
      a.ns = NS_XMLNS;
      a.prefix = p.empty() ? "" : "xmlns";
      a.local = p.empty() ? "xmlns" : p;
      a.value = uri;
      attrs.push_back(a);
      scope.push_back({p, uri});
    };
    while (chance(25)) {
      const std::string p = pick(prefixes);
      declare(p, p.empty() && chance(30) ? "" : pick(uris));
    }

    // Its own name, declared if the scope does not already say it. A prefix
    // this element declared above means what it declared.
    auto own = [&](const std::string & prefix, std::string & uri) {
      for (const MAttr & a : attrs) {
        if (a.ns == NS_XMLNS && a.local == (prefix.empty() ? "xmlns" : prefix)) {
          uri = a.value;
          return true;
        }
      }
      return false;
    };
    const std::string p = pick(prefixes);
    std::string u = chance(60) ? pick(uris) : "";
    if (p.empty()) {
      own(p, u);
      if (lookup("") != u) {
        declare("", u);
      }
      e.ns = u;
      e.prefix = "";
    }
    else {
      std::string use = u.empty() ? std::string(uris[0]) : u;
      own(p, use);
      if (lookup(p) != use) {
        declare(p, use);
      }
      e.ns = use;
      e.prefix = p;
    }
    e.local = name();

    // Attributes: plain, prefixed with a prefix in scope, and xml:*.
    static const char * const plains[] = {"id", "class", "d1", "n"};
    const size_t n_attrs = below(4);
    for (size_t i = 0; i < n_attrs; i++) {
      MAttr a;
      a.value = text_data(false);
      if (chance(60)) {
        a.local = pick(plains);
        if (!ascii && chance(30)) {
          a.local = "\xC3\xA9t";
        }
      }
      else if (chance(70)) {
        const std::string q = pick(prefixes);
        if (q.empty() || lookup(q).empty()) {
          continue;
        }
        a.prefix = q;
        a.ns = lookup(q);
        a.local = pick(plains);
      }
      else {
        a.prefix = "xml";
        a.ns = NS_XML;
        static const char * const x[] = {"lang", "space", "base"};
        a.local = pick(x);
        if (a.local == "space") {
          a.value = chance(50) ? "preserve" : "default";
        }
      }
      bool dup = false;
      for (const MAttr & b : attrs) {
        dup = dup || (b.ns == a.ns && b.local == a.local);
      }
      if (!dup) {
        attrs.push_back(a);
      }
    }
    // Any order: the declarations apply whatever the order they are written in.
    for (size_t i = attrs.size(); i > 1; i--) {
      std::swap(attrs[i - 1], attrs[below(i)]);
    }
    e.attrs = attrs;

    if (depth < 5) {
      const size_t n = chance(25) ? 0 : below(5);
      for (size_t i = 0; i < n; i++) {
        MNode k = chance(45) ? element(depth + 1) : leaf();
        if (k.kind == MNode::TEXT && !e.kids.empty() && e.kids.back().kind == MNode::TEXT) {
          continue;  // adjacent text is one node in XML
        }
        e.kids.push_back(k);
      }
    }
    scope.resize(mark);
    return e;
  }
};

// The model's own serializer: the specification's rules, written again.
class Serializer {
 public:
  Serializer(bool pretty_print, std::string newline_s) : pretty(pretty_print), nl(newline_s) {}

  std::string run(const MDoc & d, const std::string & declaration) {
    std::string out;
    bool any = false;
    auto item = [&](const std::string & s) {
      if (any) {
        out += nl;
      }
      out += s;
      any = true;
    };
    if (!declaration.empty()) {
      item(declaration);
    }
    for (const MNode & n : d.before) {
      item(leaf(n));
    }
    item(element(d.root, 0, false, false));
    for (const MNode & n : d.after) {
      item(leaf(n));
    }
    if (pretty) {
      out += nl;
    }
    return out;
  }

 private:
  bool pretty;
  std::string nl;

  static std::string escape(const std::string & s, bool attribute) {
    std::string out;
    for (char c : s) {
      switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '\r': out += "&#13;"; break;
        case '"': out += attribute ? "&quot;" : "\""; break;
        case '\t': out += attribute ? "&#9;" : "\t"; break;
        case '\n': out += attribute ? "&#10;" : "\n"; break;
        default: out += c;
      }
    }
    return out;
  }

  static std::string qname(const std::string & prefix, const std::string & local) {
    return prefix.empty() ? local : prefix + ":" + local;
  }

  static std::string leaf(const MNode & n) {
    switch (n.kind) {
      case MNode::TEXT: return escape(n.data, false);
      case MNode::CDATA: return "<![CDATA[" + n.data + "]]>";
      case MNode::COMMENT: return "<!--" + n.data + "-->";
      case MNode::PI: return "<?" + n.target + (n.data.empty() ? "" : " " + n.data) + "?>";
      default: return "";
    }
  }

  std::string element(const MNode & e, size_t depth, bool no_indent, bool preserve) {
    std::string s = "<" + qname(e.prefix, e.local);
    bool own_preserve = false;
    for (const MAttr & a : e.attrs) {
      s += " " + qname(a.prefix, a.local) + "=\"" + escape(a.value, true) + "\"";
      if (a.ns == NS_XML && a.local == "space" && a.value == "preserve") {
        own_preserve = true;
      }
    }
    if (e.kids.empty()) {
      return s + "/>";
    }
    s += ">";
    bool mixed = false;
    for (const MNode & k : e.kids) {
      mixed = mixed || k.kind == MNode::TEXT || k.kind == MNode::CDATA;
    }
    const bool pres = preserve || own_preserve;
    const bool no = no_indent || pres || mixed;
    const bool lay = pretty && !no;
    for (const MNode & k : e.kids) {
      if (lay) {
        s += nl + std::string((depth + 1) * 2, ' ');
      }
      s += k.kind == MNode::ELEM ? element(k, depth + 1, no, pres) : leaf(k);
    }
    if (lay) {
      s += nl + std::string(depth * 2, ' ');
    }
    return s + "</" + qname(e.prefix, e.local) + ">";
  }
};

// Build the library's tree from the model.
Node * build_node(Build & b, Node * parent, const MNode & m) {
  switch (m.kind) {
    case MNode::ELEM: {
      Node * e = b.el(parent, m.local.c_str(), m.ns.empty() ? nullptr : m.ns.c_str(),
          m.prefix.empty() ? nullptr : m.prefix.c_str());
      for (const MAttr & a : m.attrs) {
        b.attr(e, a.local.c_str(), a.value, a.ns.empty() ? nullptr : a.ns.c_str(),
            a.prefix.empty() ? nullptr : a.prefix.c_str());
      }
      for (const MNode & k : m.kids) {
        build_node(b, e, k);
      }
      return e;
    }
    case MNode::TEXT: return b.text(parent, m.data);
    case MNode::CDATA: return b.cdata(parent, m.data);
    case MNode::COMMENT: return b.comment(parent, m.data);
    case MNode::PI: return b.pi(parent, m.target, m.data);
  }
  return nullptr;
}

void build_doc(Build & b, const MDoc & d) {
  if (d.declared) {
    gtext_markup_document_set_xml_declaration(b.doc, "1.0", GTEXT_MARKUP_STANDALONE_UNSPECIFIED);
  }
  for (const MNode & n : d.before) {
    build_node(b, nullptr, n);
  }
  build_node(b, nullptr, d.root);
  for (const MNode & n : d.after) {
    build_node(b, nullptr, n);
  }
}

TEST(XmlWriter, GeneratedTreesAreWrittenAsAModelWritesThemAndReadBackEqual) {
  const std::string decl = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
  size_t elements_seen = 0;
  for (uint32_t seed = 1; seed <= seeds(600); seed++) {
    Generator gen(seed, false);
    const MDoc m = gen.make();
    Build b;
    build_doc(b, m);
    const char * problem = markup_check_invariants(b.doc);
    ASSERT_STREQ(problem ? problem : "ok", "ok");

    // Compact.
    const std::string want = Serializer(false, "\n").run(m, m.declared ? decl : "");
    const Out out = write_default(b.doc);
    ASSERT_EQ(out.status, GTEXT_XML_OK) << seed << " " << out.r.message << " " << out.r.path << "\n" << want;
    ASSERT_EQ(out.bytes, want) << seed;
    ASSERT_EQ(out.r.text_nodes_coalesced, 0u);
    elements_seen++;

    // Read back equal; write again identical.
    Doc * back = nullptr;
    GTEXT_XML_Error err;
    GTEXT_XML_Parse_Options po = gtext_xml_parse_options_default();
    ASSERT_EQ(gtext_xml_parse(out.bytes.data(), out.bytes.size(), &po, &back, &err), GTEXT_XML_OK)
        << seed << " " << err.message << "\n" << out.bytes;
    ASSERT_TRUE(gtext_markup_equal(b.root(), gtext_markup_document_root(back))) << seed << "\n" << out.bytes;
    ASSERT_EQ(write_default(back).bytes, out.bytes) << seed;

    // Pretty, and its second write.
    Opts p = pretty();
    const Out pr = write_with(b.doc, p);
    ASSERT_EQ(pr.status, GTEXT_XML_OK);
    ASSERT_EQ(pr.bytes, Serializer(true, "\n").run(m, m.declared ? decl : "")) << seed;
    Doc * pback = parse(pr.bytes);
    ASSERT_NE(pback, nullptr) << pr.bytes;
    ASSERT_EQ(write_with(pback, p).bytes, pr.bytes) << seed;
    gtext_markup_document_free(pback);
    gtext_markup_document_free(back);

    // The other encodings, each read back to the same tree. (ASCII and Latin-1
    // here run with names the encoding can spell; see the next test.)
    for (GTEXT_XML_Output_Encoding enc :
        {GTEXT_XML_OUTPUT_UTF16LE, GTEXT_XML_OUTPUT_UTF16BE}) {
      Opts o = defaults();
      o.encoding = enc;
      const Out e = write_with(b.doc, o);
      ASSERT_EQ(e.status, GTEXT_XML_OK);
      Doc * eback = parse(e.bytes);
      ASSERT_NE(eback, nullptr) << seed;
      ASSERT_TRUE(gtext_markup_equal(b.root(), gtext_markup_document_root(eback))) << seed;
      gtext_markup_document_free(eback);
    }
  }
  EXPECT_EQ(elements_seen, seeds(600));
}

TEST(XmlWriter, GeneratedTreesInTheOneByteEncodingsUseReferencesAndReadBackEqual) {
  for (uint32_t seed = 1000; seed < 1000 + seeds(300); seed++) {
    Generator gen(seed, true);
    const MDoc m = gen.make();
    Build b;
    build_doc(b, m);
    // Comments, PIs and CDATA in these trees hold non-ASCII characters, which
    // the encodings cannot spell in those places: that is the refusal, and the
    // rest read back. Latin-1 can spell e-acute and not the euro sign.
    for (GTEXT_XML_Output_Encoding enc :
        {GTEXT_XML_OUTPUT_ISO_8859_1, GTEXT_XML_OUTPUT_US_ASCII}) {
      Opts o = defaults();
      o.encoding = enc;
      const Out out = write_with(b.doc, o);
      if (out.status == GTEXT_XML_E_UNREPRESENTABLE) {
        // Is there really a character in a place that cannot take a reference?
        const Out utf8 = write_default(b.doc);
        bool found = false;
        for (unsigned char c : utf8.bytes) {
          found = found || c >= 0x80;
        }
        ASSERT_TRUE(found) << seed;
        EXPECT_NE(std::string(out.r.path), "");
        continue;
      }
      ASSERT_EQ(out.status, GTEXT_XML_OK) << seed << out.r.message;
      Doc * back = parse(out.bytes);
      ASSERT_NE(back, nullptr) << seed << "\n" << out.bytes;
      ASSERT_TRUE(gtext_markup_equal(b.root(), gtext_markup_document_root(back))) << seed;
      for (unsigned char c : out.bytes) {
        if (enc == GTEXT_XML_OUTPUT_US_ASCII) {
          ASSERT_LT(c, 0x80) << seed;
        }
      }
      gtext_markup_document_free(back);
    }
  }
}

TEST(XmlWriter, GeneratedTreesWithMissingDeclarationsAreWrittenWhenAsked) {
  // Build the generated trees and then strip every xmlns attribute; the writer
  // with declare_namespaces must put back declarations that make the tree read
  // back with the same namespaces. (Not the same attributes: it says where it
  // put them, and an unused declaration is not coming back.)
  size_t with_ns = 0;
  for (uint32_t seed = 1; seed <= seeds(300); seed++) {
    Generator gen(seed, true);
    MDoc m = gen.make();
    std::function<void(MNode &)> strip = [&](MNode & n) {
      if (n.kind != MNode::ELEM) {
        return;
      }
      std::vector<MAttr> keep;
      for (const MAttr & a : n.attrs) {
        if (a.ns != NS_XMLNS) {
          keep.push_back(a);
        }
      }
      n.attrs = keep;
      for (MNode & k : n.kids) {
        strip(k);
      }
    };
    strip(m.root);
    Build b;
    build_doc(b, m);
    Opts o = defaults();
    o.declare_namespaces = true;
    const Out out = write_with(b.doc, o);
    ASSERT_EQ(out.status, GTEXT_XML_OK) << seed << out.r.message << " " << out.r.path;
    Doc * back = parse(out.bytes);
    ASSERT_NE(back, nullptr) << out.bytes;
    // Compare the names: every element and attribute has the namespace it had.
    const Node * x = b.root();
    const Node * y = gtext_markup_document_root(back);
    while (x && y) {
      ASSERT_EQ(gtext_markup_node_kind(x), gtext_markup_node_kind(y)) << seed;
      if (gtext_markup_node_kind(x) == GTEXT_MARKUP_KIND_ELEMENT) {
        const char * nx = gtext_markup_element_namespace(x);
        const char * ny = gtext_markup_element_namespace(y);
        ASSERT_EQ(std::string(nx ? nx : ""), std::string(ny ? ny : "")) << seed << out.bytes;
        ASSERT_STREQ(gtext_markup_element_local_name(x), gtext_markup_element_local_name(y));
        with_ns += nx ? 1 : 0;
        for (size_t i = 0; i < gtext_markup_element_attribute_count(x); i++) {
          GTEXT_Markup_Attribute a;
          gtext_markup_element_attribute_at(x, i, &a);
          GTEXT_Markup_Attribute c;
          ASSERT_TRUE(gtext_markup_element_get_attribute(y, a.ns, a.local, &c)) << seed << out.bytes;
          ASSERT_EQ(std::string(a.value, a.value_length), std::string(c.value, c.value_length));
        }
      }
      x = gtext_markup_node_next_in_order(b.root(), x, false);
      y = gtext_markup_node_next_in_order(gtext_markup_document_root(back), y, false);
    }
    ASSERT_EQ(x == nullptr, y == nullptr);
    // And that output is written the same again from what was read.
    ASSERT_EQ(write_default(back).bytes, out.bytes) << seed;
    gtext_markup_document_free(back);
  }
  EXPECT_GT(with_ns, seeds(300) / 3);
}

}  // namespace
