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
 * gtext_xml_parse(): what it builds, and where and why it refuses.
 *
 * The expectations for a refusal are written from the specifications, not read
 * back from the parser: each row says which production was violated, and the
 * position is where that production's text begins in the input as counted by
 * hand (the offsets are in the comments where they are not obvious).
 *
 * Every tree a test builds is also held to markup_check_invariants(), so a parse
 * that returns a broken structure fails here whatever it looks like.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/xml.h>

#include "../src/markup/markup_internal.h"
#include "markup_test_support.h"

namespace {

using Doc = GTEXT_Markup_Document;
using Node = GTEXT_Markup_Node;

struct Result {
  GTEXT_XML_Status status = GTEXT_XML_OK;
  Doc * doc = nullptr;
  GTEXT_XML_Error err{};
};

class XmlParse : public ::testing::Test {
 protected:
  std::vector<Doc *> docs;

  Result parse(const std::string & text, const GTEXT_XML_Parse_Options * opts = nullptr) {
    Result r;
    r.status = gtext_xml_parse(text.data(), text.size(), opts, &r.doc, &r.err);
    if (r.status == GTEXT_XML_OK) {
      EXPECT_NE(r.doc, nullptr);
      docs.push_back(r.doc);
      const char * problem = markup_check_invariants(r.doc);
      EXPECT_STREQ(problem ? problem : "ok", "ok") << text;
      EXPECT_EQ(r.err.code, GTEXT_XML_OK);
    }
    else {
      EXPECT_EQ(r.doc, nullptr) << "a failed parse must not write its output";
      EXPECT_EQ(r.err.code, r.status);
      EXPECT_NE(r.err.message, nullptr);
    }
    return r;
  }

  // The document as the library's own dump writes it, minus the first line.
  std::string tree(const std::string & text) {
    Result r = parse(text);
    EXPECT_EQ(r.status, GTEXT_XML_OK) << text << ": " << r.err.message;
    return r.doc ? dump_to_string(gtext_markup_document_root(r.doc)) : "";
  }

  void expect_error(const std::string & text, GTEXT_XML_Status status,
      size_t offset, size_t line, size_t column) {
    Result r = parse(text);
    EXPECT_EQ(r.status, status) << text << " -> " << gtext_xml_status_string(r.status)
                                << ": " << (r.err.message ? r.err.message : "");
    EXPECT_EQ(r.err.byte_offset, offset) << text;
    EXPECT_EQ(r.err.line, line) << text;
    EXPECT_EQ(r.err.column, column) << text;
  }

  void TearDown() override {
    for (Doc * d : docs) {
      gtext_markup_document_free(d);
    }
  }
};

TEST_F(XmlParse, TheMatrixsWellFormedRow) {
  // <a xmlns="u"><b x="1"/></a>: b is in u, its attribute x in no namespace.
  Result r = parse("<a xmlns=\"u\"><b x=\"1\"/></a>");
  ASSERT_EQ(r.status, GTEXT_XML_OK);
  Node * a = gtext_markup_document_element(r.doc);
  ASSERT_NE(a, nullptr);
  EXPECT_TRUE(gtext_markup_element_is(a, "u", "a"));
  Node * b = gtext_markup_node_first_child(a);
  ASSERT_NE(b, nullptr);
  EXPECT_TRUE(gtext_markup_element_is(b, "u", "b"));
  GTEXT_Markup_Attribute x;
  ASSERT_TRUE(gtext_markup_element_get_attribute(b, nullptr, "x", &x));
  EXPECT_EQ(x.ns, nullptr);
  EXPECT_EQ(x.prefix, nullptr);
  EXPECT_STREQ(x.value, "1");
  EXPECT_EQ(gtext_markup_node_next_sibling(b), nullptr);
}

TEST_F(XmlParse, TheMatrixsNotWellFormedRows) {
  // Mismatched tag: the position is the end tag's.
  expect_error("<a><b></a>", GTEXT_XML_E_MISMATCHED_TAG, 6, 1, 7);
  // "]]>" in text: the position is the "]".
  expect_error("<a>]]></a>", GTEXT_XML_E_SYNTAX, 3, 1, 4);
  expect_error("<a>x ]]> y</a>", GTEXT_XML_E_SYNTAX, 5, 1, 6);
  // "--" in a comment: the position is the first "-" of the pair.
  expect_error("<a><!-- a -- b --></a>", GTEXT_XML_E_SYNTAX, 10, 1, 11);
  expect_error("<a><!-- a ---></a>", GTEXT_XML_E_SYNTAX, 10, 1, 11);
  // A duplicate attribute: the second one's name.
  expect_error("<a x=\"1\" x=\"2\"/>", GTEXT_XML_E_DUPLICATE_ATTRIBUTE, 9, 1, 10);
}

TEST_F(XmlParse, TheMatrixsNamespaceRows) {
  expect_error("<p:a/>", GTEXT_XML_E_NAMESPACE, 1, 1, 2);
  expect_error("<a xmlns:xml=\"x\"/>", GTEXT_XML_E_NAMESPACE, 3, 1, 4);
}

TEST_F(XmlParse, TheMatrixsEntityRows) {
  EXPECT_EQ(tree("<a>&amp;&#x41;&#65;&lt;&gt;&apos;&quot;</a>"),
      "#document\n  <a>\n    \"&AA<>'\\\"\"\n");
  // The position of the "&", not of the name or the semicolon.
  expect_error("<a>&nbsp;</a>", GTEXT_XML_E_ENTITY, 3, 1, 4);
  expect_error("<a x=\"&nbsp;\"/>", GTEXT_XML_E_ENTITY, 6, 1, 7);
}

TEST_F(XmlParse, TheMatrixsDoctypeRows) {
  // A ']' inside a quoted literal and inside a comment, in the internal subset,
  // does not end it.
  const std::string doc =
      "<!DOCTYPE a [\n"
      "<!ENTITY e \"]>\">\n"
      "<!-- ]> -->\n"
      "<!ENTITY f ']'>\n"
      "<?pi ]> ?>\n"
      "]><a/>";
  EXPECT_EQ(tree(doc), "#document\n  <!DOCTYPE \"a\" - ->\n  <a>\n");
  // Unterminated: end of input inside the subset, in a literal, a comment.
  expect_error("<!DOCTYPE a [ <!ENTITY e \"x\">", GTEXT_XML_E_UNEXPECTED_EOF, 29, 1, 30);
  expect_error("<!DOCTYPE a [ <!ENTITY e \"x]>", GTEXT_XML_E_UNEXPECTED_EOF, 29, 1, 30);
  expect_error("<!DOCTYPE a [ <!-- ]> ", GTEXT_XML_E_UNEXPECTED_EOF, 14, 1, 15);
}

TEST_F(XmlParse, TheMatrixsLimitRows) {
  GTEXT_XML_Parse_Options o = gtext_xml_parse_options_default();
  o.max_depth = 2;
  EXPECT_EQ(parse("<a><b/></a>", &o).status, GTEXT_XML_OK);
  EXPECT_EQ(parse("<a><b><c/></b></a>", &o).status, GTEXT_XML_E_LIMIT_DEPTH);
}

TEST_F(XmlParse, TheTreeOfEveryKindOfNode) {
  const std::string text =
      "<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"yes\"?>\n"
      "<!DOCTYPE a PUBLIC \"-//X  Y//EN\" \"x.dtd\" [<!ENTITY e \"v\">]>\n"
      "<!-- c --><a xmlns=\"u\" xmlns:p=\"v\" p:q=\"1\" r=\"a&#10;b\tc&amp;\">"
      "<b/>t<![CDATA[<x>]]>t2&lt;<?pi data ?></a>\n<!--end-->";
  EXPECT_EQ(tree(text),
      "#document\n"
      "  <!DOCTYPE \"a\" \"-//X Y//EN\" \"x.dtd\">\n"
      "  <!--\" c \"-->\n"
      "  <{u}a>\n"
      "    @{http://www.w3.org/2000/xmlns/}xmlns=\"u\"\n"
      "    @{http://www.w3.org/2000/xmlns/}xmlns:p=\"v\"\n"
      "    @{v}p:q=\"1\"\n"
      "    @r=\"a\\nb c&\"\n"
      "    <{u}b>\n"
      "    \"t\"\n"
      "    <![CDATA[\"<x>\"]]>\n"
      "    \"t2<\"\n"
      "    <?pi \"data \"?>\n"
      "  <!--\"end\"-->\n");
  Result r = parse(text);
  GTEXT_Markup_Encoding_Source source;
  EXPECT_STREQ(gtext_markup_document_encoding(r.doc, &source), "utf-8");
  EXPECT_EQ(source, GTEXT_MARKUP_ENCODING_DECLARATION);
  GTEXT_Markup_Standalone standalone;
  EXPECT_STREQ(gtext_markup_document_xml_version(r.doc, &standalone), "1.0");
  EXPECT_EQ(standalone, GTEXT_MARKUP_STANDALONE_YES);
  Node * doctype = gtext_markup_document_doctype(r.doc);
  ASSERT_NE(doctype, nullptr);
  EXPECT_STREQ(gtext_markup_doctype_name(doctype), "a");
  EXPECT_STREQ(gtext_markup_doctype_public_id(doctype), "-//X Y//EN");
  EXPECT_STREQ(gtext_markup_doctype_system_id(doctype), "x.dtd");
}

TEST_F(XmlParse, ADocumentWithoutADeclarationHasNoVersion) {
  Result r = parse("<a/>");
  ASSERT_EQ(r.status, GTEXT_XML_OK);
  GTEXT_Markup_Standalone standalone;
  EXPECT_EQ(gtext_markup_document_xml_version(r.doc, &standalone), nullptr);
  EXPECT_EQ(standalone, GTEXT_MARKUP_STANDALONE_UNSPECIFIED);
  GTEXT_Markup_Encoding_Source source;
  EXPECT_STREQ(gtext_markup_document_encoding(r.doc, &source), "UTF-8");
  EXPECT_EQ(source, GTEXT_MARKUP_ENCODING_DEFAULT);
  EXPECT_EQ(gtext_markup_document_doctype(r.doc), nullptr);
}

TEST_F(XmlParse, StandaloneNoAndPseudoAttributeQuoting) {
  Result r = parse("<?xml version='1.0' encoding='UTF-8' standalone='no' ?><a/>");
  ASSERT_EQ(r.status, GTEXT_XML_OK);
  GTEXT_Markup_Standalone standalone;
  gtext_markup_document_xml_version(r.doc, &standalone);
  EXPECT_EQ(standalone, GTEXT_MARKUP_STANDALONE_NO);
  EXPECT_EQ(parse("<?xml version=\"1.0\"?><a/>").status, GTEXT_XML_OK);
  EXPECT_EQ(parse("<?xml version = \"1.0\" ?><a/>").status, GTEXT_XML_OK);
  EXPECT_EQ(parse("<?xml version=\"1.0\" standalone=\"yes\"?><a/>").status, GTEXT_XML_OK);
}

TEST_F(XmlParse, TheXmlDeclarationIsStrict) {
  // Wrong order, missing parts, bad values, not first, no whitespace.
  EXPECT_EQ(parse("<?xml encoding=\"UTF-8\" version=\"1.0\"?><a/>").status, GTEXT_XML_E_SYNTAX);
  EXPECT_EQ(parse("<?xml?><a/>").status, GTEXT_XML_E_SYNTAX);
  EXPECT_EQ(parse("<?xml version=\"1.0\" standalone=\"maybe\"?><a/>").status, GTEXT_XML_E_SYNTAX);
  EXPECT_EQ(parse("<?xml version=\"1.0\" standalone=\"yes\" encoding=\"UTF-8\"?><a/>").status,
      GTEXT_XML_E_SYNTAX);
  EXPECT_EQ(parse("<?xml version=\"1.0\"encoding=\"UTF-8\"?><a/>").status, GTEXT_XML_E_SYNTAX);
  EXPECT_EQ(parse("<?xml version=1.0?><a/>").status, GTEXT_XML_E_SYNTAX);
  EXPECT_EQ(parse("<?xml version=\"1.0\"><a/>").status, GTEXT_XML_E_SYNTAX);
  EXPECT_EQ(parse("<?xml version=\"x.y\"?><a/>").status, GTEXT_XML_E_SYNTAX);
  EXPECT_EQ(parse("<?xml version=\"2.0\"?><a/>").status, GTEXT_XML_E_SYNTAX);
  EXPECT_EQ(parse("<?xml version=\"1.\"?><a/>").status, GTEXT_XML_E_SYNTAX);
  // Only first: after white space, or after a comment, it is a reserved target.
  EXPECT_EQ(parse(" <?xml version=\"1.0\"?><a/>").status, GTEXT_XML_E_SYNTAX);
  EXPECT_EQ(parse("<!----><?xml version=\"1.0\"?><a/>").status, GTEXT_XML_E_SYNTAX);
  EXPECT_EQ(parse("<a/><?XmL x?>").status, GTEXT_XML_E_SYNTAX);
  // A target that merely starts with "xml" is an ordinary one.
  EXPECT_EQ(parse("<?xml-stylesheet href=\"s.css\"?><a/>").status, GTEXT_XML_OK);
  EXPECT_EQ(parse("<?xmlx y?><a/>").status, GTEXT_XML_OK);
}

TEST_F(XmlParse, VersionsAndEncodingsAreStatementsAboutTheParser) {
  Result r = parse("<?xml version=\"1.1\"?><a/>");
  ASSERT_EQ(r.status, GTEXT_XML_OK);
  GTEXT_Markup_Standalone standalone;
  EXPECT_STREQ(gtext_markup_document_xml_version(r.doc, &standalone), "1.1");
  EXPECT_EQ(parse("<?xml version=\"1.2\"?><a/>").status, GTEXT_XML_E_UNSUPPORTED_VERSION);
  r = parse("<?xml version=\"1.0\" encoding=\"windows-1252\"?><a/>");
  EXPECT_EQ(r.status, GTEXT_XML_E_UNSUPPORTED_ENCODING);
  EXPECT_EQ(r.err.byte_offset, 30u);  // The value: <?xml version="1.0" encoding="
  // A UTF-8 document labeled UTF-16 is a mismatch the specification makes fatal.
  r = parse("<?xml version=\"1.0\" encoding=\"UTF-16\"?><a/>");
  EXPECT_EQ(r.status, GTEXT_XML_E_ENCODING_MISMATCH);
  EXPECT_EQ(r.err.byte_offset, 30u);
  EXPECT_EQ(parse("<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><a/>").status, GTEXT_XML_OK);
  EXPECT_EQ(parse("<?xml version=\"1.0\" encoding=\"US-ASCII\"?><a/>").status, GTEXT_XML_OK);
  EXPECT_EQ(parse("<?xml version=\"1.0\" encoding=\"UTF-8\"?><a/>").status, GTEXT_XML_OK);
  EXPECT_EQ(parse("<?xml version=\"1.0\" encoding=\"Utf-8\"?><a/>").status, GTEXT_XML_OK);
  // An encoding name that is not one is a syntax error, not a statement.
  EXPECT_EQ(parse("<?xml version=\"1.0\" encoding=\"8bit\"?><a/>").status, GTEXT_XML_E_SYNTAX);
  EXPECT_EQ(parse("<?xml version=\"1.0\" encoding=\"\"?><a/>").status, GTEXT_XML_E_SYNTAX);
}

TEST_F(XmlParse, Xml11CharacterAndReferenceRules) {
  EXPECT_EQ(parse("<a>\x01</a>").status, GTEXT_XML_E_CHARACTER);
  EXPECT_EQ(parse("<a>&#x1;</a>").status, GTEXT_XML_E_CHARACTER);
  Result r = parse("<?xml version=\"1.1\"?><a>&#x1;&#x7F;&#x85;&#x9F;</a>");
  ASSERT_EQ(r.status, GTEXT_XML_OK);
  GTEXT_Markup_Standalone standalone;
  EXPECT_STREQ(gtext_markup_document_xml_version(r.doc, &standalone), "1.1");
  EXPECT_EQ(parse("<?xml version=\"1.1\"?><a>\xC2\x85</a>").status, GTEXT_XML_OK);
  for (const std::string & literal : {
           std::string("\x01"), std::string("\x7F"),
           std::string("\xC2\x80"), std::string("\xC2\x86")}) {
    const std::string input = "<?xml version=\"1.1\"?><a>" + literal + "</a>";
    Result bad = parse(input);
    EXPECT_EQ(bad.status, GTEXT_XML_E_CHARACTER);
    EXPECT_EQ(bad.doc, nullptr);
  }
  for (const std::string & body : {
           std::string("<a v='\x7F'/>"), std::string("<a><!--\x7F--></a>"),
           std::string("<a><?p \x7F?></a>"),
           std::string("<a><![CDATA[\x7F]]></a>")}) {
    const std::string input = "<?xml version=\"1.1\"?>" + body;
    EXPECT_EQ(parse(input).status, GTEXT_XML_E_CHARACTER);
  }
  EXPECT_EQ(parse("<?xml version=\"1.1\"?><a>&#0;</a>").status,
      GTEXT_XML_E_CHARACTER);
  expect_error("<?xml version=\"1.1\"?><a>\x01</a>",
      GTEXT_XML_E_CHARACTER, 24, 1, 25);
  std::string nul = "<?xml version=\"1.1\"?><a></a>";
  nul.insert(24, 1, '\0');
  EXPECT_EQ(parse(nul).status, GTEXT_XML_E_CHARACTER);
  // XML 1.0 Fifth Edition and XML 1.1 Second Edition share the name ranges.
  EXPECT_EQ(parse("<?xml version=\"1.0\"?><\xC3\x80/>").status, GTEXT_XML_OK);
  EXPECT_EQ(parse("<?xml version=\"1.1\"?><\xC3\x80/>").status, GTEXT_XML_OK);
  EXPECT_EQ(parse("<?xml version=\"1.0\"?><\xCC\x80/>").status, GTEXT_XML_E_SYNTAX);
  EXPECT_EQ(parse("<?xml version=\"1.1\"?><\xCC\x80/>").status, GTEXT_XML_E_SYNTAX);
}

TEST_F(XmlParse, ByteOrderMarksAndWideEncodings) {
  // A UTF-8 mark is skipped and counted in the offsets, not in the columns.
  Result r = parse("\xEF\xBB\xBF<a/>");
  ASSERT_EQ(r.status, GTEXT_XML_OK);
  GTEXT_Markup_Encoding_Source source;
  EXPECT_STREQ(gtext_markup_document_encoding(r.doc, &source), "UTF-8");
  EXPECT_EQ(source, GTEXT_MARKUP_ENCODING_BOM);
  GTEXT_Markup_Location where;
  ASSERT_TRUE(gtext_markup_node_location(gtext_markup_document_element(r.doc), &where));
  EXPECT_EQ(where.offset, 3u);
  EXPECT_EQ(where.line, 1u);
  EXPECT_EQ(where.column, 1u);
  EXPECT_EQ(parse("\xEF\xBB\xBF<?xml version=\"1.0\"?><a/>").status, GTEXT_XML_OK);
  // A mark outranks a declaration's label as the encoding's source.
  for (const char * text : {"\xEF\xBB\xBF<?xml version=\"1.0\"?><a/>",
           "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\"?><a/>"}) {
    Result b = parse(text);
    ASSERT_EQ(b.status, GTEXT_XML_OK);
    GTEXT_Markup_Encoding_Source from;
    EXPECT_STREQ(gtext_markup_document_encoding(b.doc, &from), "UTF-8");
    EXPECT_EQ(from, GTEXT_MARKUP_ENCODING_BOM);
  }
  // A mark and no document is still not a document.
  EXPECT_EQ(parse("\xEF\xBB\xBF").status, GTEXT_XML_E_UNEXPECTED_EOF);
  // Two marks: the second is a character that is not white space.
  EXPECT_EQ(parse("\xEF\xBB\xBF\xEF\xBB\xBF<a/>").status, GTEXT_XML_E_SYNTAX);

  // EBCDIC is recognised and declined; UTF-16 and UTF-32 are read
  // (test-xml-encodings.cpp).
  Result e = parse(std::string("\x4C\x6F\xA7\x94", 4));
  EXPECT_EQ(e.status, GTEXT_XML_E_UNSUPPORTED_ENCODING);
  EXPECT_EQ(e.err.byte_offset, 0u);
}

TEST_F(XmlParse, EmptyAndMissingRoots) {
  expect_error("", GTEXT_XML_E_UNEXPECTED_EOF, 0, 1, 1);
  expect_error("  \n ", GTEXT_XML_E_UNEXPECTED_EOF, 4, 2, 2);
  expect_error("<!-- c -->", GTEXT_XML_E_UNEXPECTED_EOF, 10, 1, 11);
  EXPECT_EQ(parse("<!-- c --").status, GTEXT_XML_E_UNEXPECTED_EOF);
  EXPECT_EQ(parse("<a/><!-- c --").status, GTEXT_XML_E_UNEXPECTED_EOF);
  EXPECT_EQ(parse("<a/><!-- c -- ").status, GTEXT_XML_E_SYNTAX);
  expect_error("text", GTEXT_XML_E_SYNTAX, 0, 1, 1);
  expect_error("</a>", GTEXT_XML_E_SYNTAX, 0, 1, 1);
  expect_error("<a>", GTEXT_XML_E_UNEXPECTED_EOF, 3, 1, 4);
  expect_error("<a", GTEXT_XML_E_UNEXPECTED_EOF, 2, 1, 3);
  expect_error("<a/><b/>", GTEXT_XML_E_SYNTAX, 4, 1, 5);
  expect_error("<a/>text", GTEXT_XML_E_SYNTAX, 4, 1, 5);
  expect_error("<a/>&amp;", GTEXT_XML_E_SYNTAX, 4, 1, 5);
  expect_error("<a/></a>", GTEXT_XML_E_SYNTAX, 4, 1, 5);
  EXPECT_EQ(parse("<a/>  \n<!--x--><?p q?> \n").status, GTEXT_XML_OK);
}

TEST_F(XmlParse, PositionsCountLinesAndCharactersTheXmlWay) {
  // The same document with each line ending the specification names.
  expect_error("<a>\n  <b/>\n  </c></a>", GTEXT_XML_E_MISMATCHED_TAG, 13, 3, 3);
  expect_error("<a>\r\n  <b/>\r\n  </c></a>", GTEXT_XML_E_MISMATCHED_TAG, 15, 3, 3);
  expect_error("<a>\r  <b/>\r  </c></a>", GTEXT_XML_E_MISMATCHED_TAG, 13, 3, 3);
  // Mixed endings, and a LF after a CR is one break, not two.
  expect_error("<a>\r\n\n\r</c></a>", GTEXT_XML_E_MISMATCHED_TAG, 7, 4, 1);
  // Columns count characters: the e-acute is two bytes and one column.
  expect_error("<a>\xC3\xA9<c></b>", GTEXT_XML_E_MISMATCHED_TAG, 8, 1, 8);
  expect_error("<a>\xF0\x9F\x98\x80<c></b>", GTEXT_XML_E_MISMATCHED_TAG, 10, 1, 8);
}

TEST_F(XmlParse, NodesCarryTheirLocations) {
  Result r = parse("<a x=\"1\"\n   y=\"2\">\n text\n <!--c--><?p q?><![CDATA[z]]><b/>\r\n</a>");
  ASSERT_EQ(r.status, GTEXT_XML_OK);
  Node * a = gtext_markup_document_element(r.doc);
  GTEXT_Markup_Location where;
  ASSERT_TRUE(gtext_markup_node_location(a, &where));
  EXPECT_EQ(where.offset, 0u);
  EXPECT_EQ(where.line, 1u);
  EXPECT_EQ(where.column, 1u);
  ASSERT_TRUE(gtext_markup_element_attribute_location(a, 0, &where));
  EXPECT_EQ(where.offset, 3u);
  EXPECT_EQ(where.column, 4u);
  ASSERT_TRUE(gtext_markup_element_attribute_location(a, 1, &where));
  EXPECT_EQ(where.line, 2u);
  EXPECT_EQ(where.column, 4u);
  // The text starts where its first character is: line 2 after the tag's '>'.
  Node * text = gtext_markup_node_first_child(a);
  ASSERT_EQ(gtext_markup_node_kind(text), GTEXT_MARKUP_KIND_TEXT);
  ASSERT_TRUE(gtext_markup_node_location(text, &where));
  EXPECT_EQ(where.line, 2u);
  EXPECT_EQ(where.column, 10u);
  Node * comment = gtext_markup_node_next_sibling(text);
  ASSERT_EQ(gtext_markup_node_kind(comment), GTEXT_MARKUP_KIND_COMMENT);
  ASSERT_TRUE(gtext_markup_node_location(comment, &where));
  EXPECT_EQ(where.line, 4u);
  EXPECT_EQ(where.column, 2u);
  Node * pi = gtext_markup_node_next_sibling(comment);
  ASSERT_EQ(gtext_markup_node_kind(pi), GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION);
  ASSERT_TRUE(gtext_markup_node_location(pi, &where));
  EXPECT_EQ(where.column, 10u);
  Node * cdata = gtext_markup_node_next_sibling(pi);
  ASSERT_EQ(gtext_markup_node_kind(cdata), GTEXT_MARKUP_KIND_CDATA);
  ASSERT_TRUE(gtext_markup_node_location(cdata, &where));
  EXPECT_EQ(where.column, 17u);
  Node * b = gtext_markup_node_next_sibling(cdata);
  ASSERT_TRUE(gtext_markup_node_location(b, &where));
  EXPECT_EQ(where.column, 30u);
}

TEST_F(XmlParse, LineEndsBecomeLineFeedsInEveryKindOfText) {
  Result r = parse("<a>x\r\ny\rz\n\r\nw<!--c\r\nd--><?p q\rr?><![CDATA[e\r\nf]]></a>");
  ASSERT_EQ(r.status, GTEXT_XML_OK);
  Node * a = gtext_markup_document_element(r.doc);
  size_t n = 0;
  Node * text = gtext_markup_node_first_child(a);
  EXPECT_STREQ(gtext_markup_node_data(text, &n), "x\ny\nz\n\nw");
  Node * comment = gtext_markup_node_next_sibling(text);
  EXPECT_STREQ(gtext_markup_node_data(comment, &n), "c\nd");
  Node * pi = gtext_markup_node_next_sibling(comment);
  EXPECT_STREQ(gtext_markup_node_data(pi, &n), "q\nr");
  Node * cdata = gtext_markup_node_next_sibling(pi);
  EXPECT_STREQ(gtext_markup_node_data(cdata, &n), "e\nf");
}

TEST_F(XmlParse, Xml11NormalizesEveryLiteralLineEndButNotReferences) {
  const std::string doc =
      "<?xml version='1.1'?><a v='q\r\nr\r\xC2\x85s\xC2\x85t\xE2\x80\xA8u&#x85;&#x2028;'>"
      "first\r\nsecond\r\xC2\x85third\xC2\x85"
      "fourth\xE2\x80\xA8"
      "fifth"
      "&#xD;&#x85;&#x2028;</a>";
  Result r = parse(doc);
  ASSERT_EQ(r.status, GTEXT_XML_OK) << r.err.message;
  EXPECT_STREQ(gtext_markup_document_xml_version(r.doc, nullptr), "1.1");
  Node * root = gtext_markup_document_element(r.doc);
  GTEXT_Markup_Attribute value;
  ASSERT_TRUE(gtext_markup_element_get_attribute(root, nullptr, "v", &value));
  EXPECT_EQ(std::string(value.value, value.value_length),
      "q r s t u\xC2\x85\xE2\x80\xA8");
  Node * text = gtext_markup_node_first_child(root);
  size_t length = 0;
  ASSERT_EQ(gtext_markup_node_kind(text), GTEXT_MARKUP_KIND_TEXT);
  const char * data = gtext_markup_node_data(text, &length);
  EXPECT_EQ(std::string(data, length),
      "first\nsecond\nthird\nfourth\nfifth\r\xC2\x85\xE2\x80\xA8");

  Result nodes = parse("<?xml version='1.1'?><a><!--c\xC2\x85" "d\xE2\x80\xA8" "e-->"
      "<?p x\xC2\x85y\xE2\x80\xA8z?><![CDATA[m\xC2\x85n\xE2\x80\xA8o]]></a>");
  ASSERT_EQ(nodes.status, GTEXT_XML_OK) << nodes.err.message;
  Node * child = gtext_markup_node_first_child(
      gtext_markup_document_element(nodes.doc));
  ASSERT_EQ(gtext_markup_node_kind(child), GTEXT_MARKUP_KIND_COMMENT);
  EXPECT_STREQ(gtext_markup_node_data(child, &length), "c\nd\ne");
  child = gtext_markup_node_next_sibling(child);
  ASSERT_EQ(gtext_markup_node_kind(child), GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION);
  EXPECT_STREQ(gtext_markup_node_data(child, &length), "x\ny\nz");
  child = gtext_markup_node_next_sibling(child);
  ASSERT_EQ(gtext_markup_node_kind(child), GTEXT_MARKUP_KIND_CDATA);
  EXPECT_STREQ(gtext_markup_node_data(child, &length), "m\nn\no");

  Result doctype = parse("<?xml version='1.1'?><!DOCTYPE a SYSTEM 'x\r\xC2\x85y\xE2\x80\xA8z'><a/>");
  ASSERT_EQ(doctype.status, GTEXT_XML_OK) << doctype.err.message;
  EXPECT_STREQ(gtext_markup_doctype_system_id(
      gtext_markup_document_doctype(doctype.doc)), "x\ny\nz");

  Result location = parse("<?xml version='1.1'?><a>x\r\xC2\x85<b></c></a>");
  EXPECT_EQ(location.status, GTEXT_XML_E_MISMATCHED_TAG);
  EXPECT_EQ(location.err.byte_offset, 31u);
  EXPECT_EQ(location.err.line, 2u);
  EXPECT_EQ(location.err.column, 4u);

  Result declaration_space = parse("<?xml\xC2\x85version='1.1'?><a\xE2\x80\xA8x='1'/>");
  ASSERT_EQ(declaration_space.status, GTEXT_XML_OK) << declaration_space.err.message;
  EXPECT_STREQ(gtext_markup_document_xml_version(declaration_space.doc, nullptr), "1.1");
}

TEST_F(XmlParse, Xml10DoesNotTreatNelOrLineSeparatorAsLineEnds) {
  Result r = parse("<a>x\xC2\x85y\xE2\x80\xA8z</a>");
  ASSERT_EQ(r.status, GTEXT_XML_OK);
  Node * text = gtext_markup_node_first_child(gtext_markup_document_element(r.doc));
  size_t length = 0;
  const char * data = gtext_markup_node_data(text, &length);
  EXPECT_EQ(std::string(data, length),
      "x\xC2\x85y\xE2\x80\xA8z");
}

TEST_F(XmlParse, AttributeValuesAreNormalizedAsCdata) {
  // Literal tab, line feed and carriage return (with or without its line
  // feed) become one space each; a reference to any of them stays itself;
  // other white space is kept.
  Result r = parse("<a v=\"a\tb\nc\rd\r\ne&#9;f&#10;g&#13;h&#x20;i  j\"/>");
  ASSERT_EQ(r.status, GTEXT_XML_OK);
  GTEXT_Markup_Attribute v;
  ASSERT_TRUE(gtext_markup_element_get_attribute(
      gtext_markup_document_element(r.doc), nullptr, "v", &v));
  EXPECT_EQ(std::string(v.value, v.value_length), "a b c d e\tf\ng\rh i  j");
  // Quotes: either kind inside the other.
  r = parse("<a v='say \"hi\"' w=\"it's\"/>");
  ASSERT_EQ(r.status, GTEXT_XML_OK);
  ASSERT_TRUE(gtext_markup_element_get_attribute(
      gtext_markup_document_element(r.doc), nullptr, "v", &v));
  EXPECT_STREQ(v.value, "say \"hi\"");
  // '<' is not allowed; '>' is.
  expect_error("<a v=\"<\"/>", GTEXT_XML_E_SYNTAX, 6, 1, 7);
  EXPECT_EQ(parse("<a v=\">\"/>").status, GTEXT_XML_OK);
  EXPECT_EQ(parse("<a v=\"\"/>").status, GTEXT_XML_OK);
}

TEST_F(XmlParse, CharacterDataIsOneNodeUntilMarkup) {
  EXPECT_EQ(tree("<a>x&amp;y&#67;z</a>"), "#document\n  <a>\n    \"x&yCz\"\n");
  EXPECT_EQ(tree("<a>x<!--c-->y</a>"),
      "#document\n  <a>\n    \"x\"\n    <!--\"c\"-->\n    \"y\"\n");
  EXPECT_EQ(tree("<a>&lt;</a>"), "#document\n  <a>\n    \"<\"\n");
  EXPECT_EQ(tree("<a>&#xD;</a>"), "#document\n  <a>\n    \"\\r\"\n");
  EXPECT_EQ(tree("<a><![CDATA[]]></a>"), "#document\n  <a>\n    <![CDATA[\"\"]]>\n");
  EXPECT_EQ(tree("<a></a>"), "#document\n  <a>\n");
  EXPECT_EQ(tree("<a> </a>"), "#document\n  <a>\n    \" \"\n");
  // CDATA is literal: markup and references inside are text.
  EXPECT_EQ(tree("<a><![CDATA[<b>&amp;]]]></a>"),
      "#document\n  <a>\n    <![CDATA[\"<b>&amp;]\"]]>\n");
  EXPECT_EQ(tree("<a><![CDATA[a]]><![CDATA[b]]></a>"),
      "#document\n  <a>\n    <![CDATA[\"a\"]]>\n    <![CDATA[\"b\"]]>\n");
}

TEST_F(XmlParse, WhiteSpaceOutsideTheRootIsNotKept) {
  EXPECT_EQ(tree("  \n<!--a-->\n <a/>\n\t<?p q?> \r\n"),
      "#document\n  <!--\"a\"-->\n  <a>\n  <?p \"q\"?>\n");
}

TEST_F(XmlParse, MarkupAndCharactersThatAreNotAllowed) {
  // Control characters, the non-characters, bad UTF-8: each with its own status
  // and the position of the offending character.
  expect_error(std::string("<a>x\x01y</a>"), GTEXT_XML_E_CHARACTER, 4, 1, 5);
  expect_error(std::string("<a>\x0B</a>"), GTEXT_XML_E_CHARACTER, 3, 1, 4);
  expect_error(std::string("<a>\xEF\xBF\xBE</a>"), GTEXT_XML_E_CHARACTER, 3, 1, 4);
  expect_error(std::string("<a>\xEF\xBF\xBF</a>"), GTEXT_XML_E_CHARACTER, 3, 1, 4);
  expect_error(std::string("<a>\xFF</a>"), GTEXT_XML_E_UTF8, 3, 1, 4);
  expect_error(std::string("<a>\xC0\x80</a>"), GTEXT_XML_E_UTF8, 3, 1, 4);      // overlong NUL
  expect_error(std::string("<a>\xED\xA0\x80</a>"), GTEXT_XML_E_UTF8, 3, 1, 4);  // a surrogate
  expect_error(std::string("<a>\xF4\x90\x80\x80</a>"), GTEXT_XML_E_UTF8, 3, 1, 4);
  expect_error(std::string("<a>\xE0\x81\xA0</a>"), GTEXT_XML_E_UTF8, 3, 1, 4);      // overlong '`'
  expect_error(std::string("<a>\xF0\x80\x81\xA0</a>"), GTEXT_XML_E_UTF8, 3, 1, 4);  // overlong '`'
  expect_error(std::string("<a>\xE0\x9F\xBF</a>"), GTEXT_XML_E_UTF8, 3, 1, 4);      // overlong U+07FF
  expect_error(std::string("<a>\xE2\x82</a>"), GTEXT_XML_E_UTF8, 3, 1, 4);      // truncated
  expect_error(std::string("<a>\xC3"), GTEXT_XML_E_UTF8, 3, 1, 4);
  expect_error(std::string("<a x=\"\x01\"/>"), GTEXT_XML_E_CHARACTER, 6, 1, 7);
  expect_error(std::string("<a><!--\x02--></a>"), GTEXT_XML_E_CHARACTER, 7, 1, 8);
  expect_error(std::string("<a><![CDATA[\x03]]></a>"), GTEXT_XML_E_CHARACTER, 12, 1, 13);
  expect_error(std::string("<a><?p \x04?></a>"), GTEXT_XML_E_CHARACTER, 7, 1, 8);
  expect_error(std::string("<a\xFF/>"), GTEXT_XML_E_UTF8, 2, 1, 3);
  expect_error(std::string("<a/>\x00", 5), GTEXT_XML_E_CHARACTER, 4, 1, 5);
  // The valid extremes are fine.
  EXPECT_EQ(parse("<a>\xEF\xBF\xBD\xF4\x8F\xBF\xBF\xEE\x80\x80\xED\x9F\xBF</a>").status, GTEXT_XML_OK);
  EXPECT_EQ(parse("<a>\t\n\r \x7F</a>").status, GTEXT_XML_OK);
}

TEST_F(XmlParse, CharacterReferences) {
  EXPECT_EQ(tree("<a>&#x1F600;&#128512;&#x10FFFF;&#xE000;&#xFFFD;&#x7F;</a>"),
      "#document\n  <a>\n    \"\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF4\x8F\xBF\xBF\xEE\x80\x80\xEF\xBF\xBD\\x7F\"\n");
  // The '&' is where every one of these is reported.
  expect_error("<a>&#0;</a>", GTEXT_XML_E_CHARACTER, 3, 1, 4);
  expect_error("<a>&#1;</a>", GTEXT_XML_E_CHARACTER, 3, 1, 4);
  expect_error("<a>&#xB;</a>", GTEXT_XML_E_CHARACTER, 3, 1, 4);
  expect_error("<a>&#xD800;</a>", GTEXT_XML_E_CHARACTER, 3, 1, 4);
  expect_error("<a>&#xDFFF;</a>", GTEXT_XML_E_CHARACTER, 3, 1, 4);
  expect_error("<a>&#xFFFE;</a>", GTEXT_XML_E_CHARACTER, 3, 1, 4);
  expect_error("<a>&#xFFFF;</a>", GTEXT_XML_E_CHARACTER, 3, 1, 4);
  expect_error("<a>&#x110000;</a>", GTEXT_XML_E_CHARACTER, 3, 1, 4);
  // Far past 32 and 64 bits: no wrap back into range.
  expect_error("<a>&#4294967361;</a>", GTEXT_XML_E_CHARACTER, 3, 1, 4);
  expect_error("<a>&#x100000041;</a>", GTEXT_XML_E_CHARACTER, 3, 1, 4);
  expect_error("<a>&#99999999999999999999999999;</a>", GTEXT_XML_E_CHARACTER, 3, 1, 4);
  expect_error("<a>&#xFFFFFFFFFFFFFFFFFFFFFF41;</a>", GTEXT_XML_E_CHARACTER, 3, 1, 4);
  // Malformed.
  expect_error("<a>&#;</a>", GTEXT_XML_E_SYNTAX, 3, 1, 4);
  expect_error("<a>&#x;</a>", GTEXT_XML_E_SYNTAX, 3, 1, 4);
  expect_error("<a>&#65</a>", GTEXT_XML_E_SYNTAX, 3, 1, 4);
  expect_error("<a>&#6a;</a>", GTEXT_XML_E_SYNTAX, 3, 1, 4);
  expect_error("<a>&#X41;</a>", GTEXT_XML_E_SYNTAX, 3, 1, 4);
  expect_error("<a>& amp;</a>", GTEXT_XML_E_SYNTAX, 3, 1, 4);
  expect_error("<a>&amp </a>", GTEXT_XML_E_SYNTAX, 3, 1, 4);
  expect_error("<a>&amp ;</a>", GTEXT_XML_E_SYNTAX, 3, 1, 4);
  expect_error("<a>&;</a>", GTEXT_XML_E_SYNTAX, 3, 1, 4);
  expect_error("<a>&.x;</a>", GTEXT_XML_E_SYNTAX, 3, 1, 4);
  expect_error("<a>&", GTEXT_XML_E_UNEXPECTED_EOF, 3, 1, 4);
  expect_error("<a>&amp", GTEXT_XML_E_UNEXPECTED_EOF, 3, 1, 4);
  expect_error("<a>&#", GTEXT_XML_E_UNEXPECTED_EOF, 3, 1, 4);
  // Case matters for the predefined five.
  expect_error("<a>&AMP;</a>", GTEXT_XML_E_ENTITY, 3, 1, 4);
  expect_error("<a>&Lt;</a>", GTEXT_XML_E_ENTITY, 3, 1, 4);
  // A declared entity is still refused: declarations are not processed.
  expect_error("<!DOCTYPE a [<!ENTITY e \"x\">]><a>&e;</a>", GTEXT_XML_E_ENTITY, 33, 1, 34);
  // A reference is not character data for the ']]>' rule.
  EXPECT_EQ(tree("<a>]]&gt;</a>"), "#document\n  <a>\n    \"]]>\"\n");
  EXPECT_EQ(tree("<a>]&#93;&gt;</a>"), "#document\n  <a>\n    \"]]>\"\n");
  EXPECT_EQ(tree("<a>]]</a>"), "#document\n  <a>\n    \"]]\"\n");
  EXPECT_EQ(tree("<a>]></a>"), "#document\n  <a>\n    \"]>\"\n");
  EXPECT_EQ(tree("<a>></a>"), "#document\n  <a>\n    \">\"\n");
}

TEST_F(XmlParse, CommentsProcessingInstructionsAndCdata) {
  EXPECT_EQ(tree("<a><!----></a>"), "#document\n  <a>\n    <!--\"\"-->\n");
  EXPECT_EQ(tree("<a><!-- a - b --></a>"), "#document\n  <a>\n    <!--\" a - b \"-->\n");
  expect_error("<a><!--->", GTEXT_XML_E_UNEXPECTED_EOF, 3, 1, 4);
  expect_error("<a><!-- x", GTEXT_XML_E_UNEXPECTED_EOF, 3, 1, 4);
  expect_error("<a><!- x --></a>", GTEXT_XML_E_SYNTAX, 3, 1, 4);
  expect_error("<a><!-- a --- b --></a>", GTEXT_XML_E_SYNTAX, 10, 1, 11);

  EXPECT_EQ(tree("<a><?t?></a>"), "#document\n  <a>\n    <?t \"\"?>\n");
  EXPECT_EQ(tree("<a><?t  d ? > ?></a>"), "#document\n  <a>\n    <?t \"d ? > \"?>\n");
  expect_error("<a><?t", GTEXT_XML_E_UNEXPECTED_EOF, 6, 1, 7);
  expect_error("<a><?t d", GTEXT_XML_E_UNEXPECTED_EOF, 3, 1, 4);
  expect_error("<a><?td?></a><?", GTEXT_XML_E_UNEXPECTED_EOF, 15, 1, 16);
  expect_error("<a><?t\x01?></a>", GTEXT_XML_E_CHARACTER, 6, 1, 7);
  expect_error("<a><? t?></a>", GTEXT_XML_E_SYNTAX, 5, 1, 6);
  expect_error("<a><?xml x?></a>", GTEXT_XML_E_SYNTAX, 5, 1, 6);
  expect_error("<a><?XML x?></a>", GTEXT_XML_E_SYNTAX, 5, 1, 6);
  expect_error("<a><?a:b x?></a>", GTEXT_XML_E_NAMESPACE, 5, 1, 6);

  EXPECT_EQ(tree("<a><![CDATA[a]b]]c]]></a>"),
      "#document\n  <a>\n    <![CDATA[\"a]b]]c\"]]>\n");
  expect_error("<a><![CDATA[x", GTEXT_XML_E_UNEXPECTED_EOF, 3, 1, 4);
  expect_error("<a><![CDATA[x]]", GTEXT_XML_E_UNEXPECTED_EOF, 3, 1, 4);
  expect_error("<a><![cdata[x]]></a>", GTEXT_XML_E_SYNTAX, 3, 1, 4);
  expect_error("<a><![CDATA x]]></a>", GTEXT_XML_E_SYNTAX, 3, 1, 4);
  expect_error("<![CDATA[x]]><a/>", GTEXT_XML_E_SYNTAX, 0, 1, 1);
  expect_error("<a/><![CDATA[x]]>", GTEXT_XML_E_SYNTAX, 4, 1, 5);
  expect_error("<a><!DOCTYPE b></a>", GTEXT_XML_E_SYNTAX, 3, 1, 4);
  expect_error("<a><!x></a>", GTEXT_XML_E_SYNTAX, 3, 1, 4);
}

TEST_F(XmlParse, StartAndEndTags) {
  EXPECT_EQ(tree("<a b = \"1\"\n\tc='2'  ></a  >"),
      "#document\n  <a>\n    @b=\"1\"\n    @c=\"2\"\n");
  EXPECT_EQ(tree("<a/>"), "#document\n  <a>\n");
  expect_error("<a / >", GTEXT_XML_E_SYNTAX, 3, 1, 4);
  expect_error("<a b=\"1\"c=\"2\"/>", GTEXT_XML_E_SYNTAX, 8, 1, 9);
  expect_error("<a b=1/>", GTEXT_XML_E_SYNTAX, 5, 1, 6);
  expect_error("<a b/>", GTEXT_XML_E_SYNTAX, 4, 1, 5);
  expect_error("<a b=\"1/>", GTEXT_XML_E_UNEXPECTED_EOF, 9, 1, 10);
  expect_error("<a b='1\"/>", GTEXT_XML_E_UNEXPECTED_EOF, 10, 1, 11);
  expect_error("<1a/>", GTEXT_XML_E_SYNTAX, 1, 1, 2);
  expect_error("< a/>", GTEXT_XML_E_SYNTAX, 1, 1, 2);
  expect_error("<a 1=\"x\"/>", GTEXT_XML_E_SYNTAX, 3, 1, 4);
  expect_error("<a></ a>", GTEXT_XML_E_SYNTAX, 5, 1, 6);
  expect_error("<a></a b>", GTEXT_XML_E_SYNTAX, 7, 1, 8);
  expect_error("<a></A>", GTEXT_XML_E_MISMATCHED_TAG, 3, 1, 4);
  expect_error("<a></ab>", GTEXT_XML_E_MISMATCHED_TAG, 3, 1, 4);
  expect_error("<ab></a>", GTEXT_XML_E_MISMATCHED_TAG, 4, 1, 5);
  expect_error("<a><b></a></b>", GTEXT_XML_E_MISMATCHED_TAG, 6, 1, 7);
  expect_error("<a></a", GTEXT_XML_E_UNEXPECTED_EOF, 6, 1, 7);
  // Names are exact, namespaces aside: the prefix is part of the end tag.
  EXPECT_EQ(parse("<p:a xmlns:p=\"u\"></p:a>").status, GTEXT_XML_OK);
  EXPECT_EQ(parse("<p:a xmlns:p=\"u\" xmlns:q=\"u\"></q:a>").status, GTEXT_XML_E_MISMATCHED_TAG);
}

TEST_F(XmlParse, NamesFollowTheFifthEdition) {
  // Characters the fifth edition added to NameStartChar and NameChar.
  EXPECT_EQ(parse("<\xE4\xB8\xAD x\xC2\xB7=\"1\"/>").status, GTEXT_XML_OK);
  EXPECT_EQ(parse("<a\xCC\x80/>").status, GTEXT_XML_OK);
  EXPECT_EQ(parse("<\xF0\x90\x80\x80/>").status, GTEXT_XML_OK);  // U+10000
  EXPECT_EQ(parse("<_a.b-c1/>").status, GTEXT_XML_OK);
  // Not at the start.
  expect_error("<\xCC\x80" "a/>", GTEXT_XML_E_SYNTAX, 1, 1, 2);
  expect_error("<-a/>", GTEXT_XML_E_SYNTAX, 1, 1, 2);
  expect_error("<.a/>", GTEXT_XML_E_SYNTAX, 1, 1, 2);
  expect_error("<\xC2\xB7" "a/>", GTEXT_XML_E_SYNTAX, 1, 1, 2);
  // U+0300..036F are name characters, not name start characters.
  expect_error("<\xCD\xAF/>", GTEXT_XML_E_SYNTAX, 1, 1, 2);
  // U+D7 (multiplication sign) is excluded; U+FFFD is the last in the BMP.
  expect_error("<a\xC3\x97/>", GTEXT_XML_E_SYNTAX, 2, 1, 3);
  EXPECT_EQ(parse("<a\xEF\xBF\xBD/>").status, GTEXT_XML_OK);
}

TEST_F(XmlParse, NamespaceDeclarationsAndScope) {
  // The default namespace applies to elements and not to attributes; it can be
  // undeclared; a prefix can be redeclared in a nested element.
  EXPECT_EQ(tree("<a xmlns=\"u\" x=\"1\"><b xmlns=\"\"><c/></b><d/></a>"),
      "#document\n"
      "  <{u}a>\n"
      "    @{http://www.w3.org/2000/xmlns/}xmlns=\"u\"\n"
      "    @x=\"1\"\n"
      "    <b>\n"
      "      @{http://www.w3.org/2000/xmlns/}xmlns=\"\"\n"
      "      <c>\n"
      "    <{u}d>\n");
  EXPECT_EQ(tree("<p:a xmlns:p=\"1\"><p:b xmlns:p=\"2\" p:x=\"y\"/><p:c/></p:a>"),
      "#document\n"
      "  <{1}p:a>\n"
      "    @{http://www.w3.org/2000/xmlns/}xmlns:p=\"1\"\n"
      "    <{2}p:b>\n"
      "      @{http://www.w3.org/2000/xmlns/}xmlns:p=\"2\"\n"
      "      @{2}p:x=\"y\"\n"
      "    <{1}p:c>\n");
  // A declaration applies to the element it is on, wherever in the tag.
  EXPECT_EQ(tree("<p:a p:x=\"1\" xmlns:p=\"u\"/>"),
      "#document\n  <{u}p:a>\n    @{u}p:x=\"1\"\n"
      "    @{http://www.w3.org/2000/xmlns/}xmlns:p=\"u\"\n");
  // Out of scope after the element closes.
  expect_error("<r><a xmlns:p=\"u\"/><p:b/></r>", GTEXT_XML_E_NAMESPACE, 20, 1, 21);
  // xml is always bound; xmlns attributes live in the xmlns namespace.
  EXPECT_EQ(tree("<a xml:lang=\"en\" xml:space=\"preserve\"/>"),
      "#document\n  <a>\n    @{http://www.w3.org/XML/1998/namespace}xml:lang=\"en\"\n"
      "    @{http://www.w3.org/XML/1998/namespace}xml:space=\"preserve\"\n");
  EXPECT_EQ(tree("<xml:a/>"), "#document\n  <{http://www.w3.org/XML/1998/namespace}xml:a>\n");
  // The prefix xml may be declared, to its own namespace only.
  EXPECT_EQ(parse("<a xmlns:xml=\"http://www.w3.org/XML/1998/namespace\"/>").status, GTEXT_XML_OK);
  // The same URI under two prefixes, and a name that means the same thing in two.
  EXPECT_EQ(parse("<a xmlns:p=\"u\" xmlns:q=\"u\"><p:b q:x=\"1\" p:y=\"2\"/></a>").status,
      GTEXT_XML_OK);
  // Attribute order is source order, declarations included.
  Result r = parse("<a z=\"1\" xmlns:p=\"u\" p:y=\"2\" b=\"3\" xmlns=\"d\"/>");
  Node * a = gtext_markup_document_element(r.doc);
  ASSERT_EQ(gtext_markup_element_attribute_count(a), 5u);
  const char * order[] = {"z", "p", "y", "b", "xmlns"};
  for (size_t i = 0; i < 5; i++) {
    GTEXT_Markup_Attribute at;
    ASSERT_EQ(gtext_markup_element_attribute_at(a, i, &at), GTEXT_MARKUP_OK);
    EXPECT_STREQ(at.local, order[i]) << i;
  }

  // XML 1.1 prefix undeclaration shadows the outer binding through its
  // element, permits a descendant rebind, and restores the ancestor on exit.
  EXPECT_EQ(tree("<?xml version='1.1'?><p:a xmlns:p='u'><b xmlns:p=''>"
                 "<c xmlns:p='v'><p:d/></c></b><p:f/></p:a>"),
      "#document\n"
      "  <{u}p:a>\n"
      "    @{http://www.w3.org/2000/xmlns/}xmlns:p=\"u\"\n"
      "    <b>\n"
      "      @{http://www.w3.org/2000/xmlns/}xmlns:p=\"\"\n"
      "      <c>\n"
      "        @{http://www.w3.org/2000/xmlns/}xmlns:p=\"v\"\n"
      "        <{v}p:d>\n"
      "    <{u}p:f>\n");
}

TEST_F(XmlParse, NamespaceErrors) {
  // Unbound prefixes, on an element and on an attribute.
  expect_error("<a p:x=\"1\"/>", GTEXT_XML_E_NAMESPACE, 3, 1, 4);
  expect_error("<a><p:b/></a>", GTEXT_XML_E_NAMESPACE, 4, 1, 5);
  expect_error("<a><b></b></p:a>", GTEXT_XML_E_MISMATCHED_TAG, 10, 1, 11);
  // Reserved prefixes and names.
  expect_error("<a xmlns:xmlns=\"u\"/>", GTEXT_XML_E_NAMESPACE, 3, 1, 4);
  expect_error("<a xmlns:xmlns=\"http://www.w3.org/2000/xmlns/\"/>", GTEXT_XML_E_NAMESPACE, 3, 1, 4);
  expect_error("<a xmlns=\"http://www.w3.org/2000/xmlns/\"/>", GTEXT_XML_E_NAMESPACE, 3, 1, 4);
  expect_error("<a xmlns:p=\"http://www.w3.org/2000/xmlns/\"/>", GTEXT_XML_E_NAMESPACE, 3, 1, 4);
  expect_error("<a xmlns:p=\"http://www.w3.org/XML/1998/namespace\"/>", GTEXT_XML_E_NAMESPACE, 3, 1, 4);
  expect_error("<a xmlns=\"http://www.w3.org/XML/1998/namespace\"/>", GTEXT_XML_E_NAMESPACE, 3, 1, 4);
  expect_error("<a xmlns:xml=\"u\"/>", GTEXT_XML_E_NAMESPACE, 3, 1, 4);
  expect_error("<xmlns:a xmlns:xmlns2=\"u\"/>", GTEXT_XML_E_NAMESPACE, 1, 1, 2);
  expect_error("<a xmlns:p=\"\"/>", GTEXT_XML_E_NAMESPACE, 3, 1, 4);
  EXPECT_EQ(parse("<?xml version='1.1'?><p:a xmlns:p='u'><b xmlns:p=''><p:c/>"
                  "</b></p:a>").status, GTEXT_XML_E_NAMESPACE);
  EXPECT_EQ(parse("<?xml version='1.1'?><a xmlns:p='u'><b xmlns:p='' p:x='1'/>"
                  "</a>").status, GTEXT_XML_E_NAMESPACE);
  // The XML 1.1 undeclaration support does not relax reserved bindings or
  // duplicate-expanded-name checks.
  EXPECT_EQ(parse("<?xml version='1.1'?><a xmlns:xml='u'/>").status,
      GTEXT_XML_E_NAMESPACE);
  EXPECT_EQ(parse("<?xml version='1.1'?><a xmlns:p='u' xmlns:q='u' "
                  "p:x='1' q:x='2'/>").status,
      GTEXT_XML_E_DUPLICATE_ATTRIBUTE);
  // Not a QName: two colons, a colon at either end, a bad local part.
  expect_error("<a:b:c/>", GTEXT_XML_E_NAMESPACE, 1, 1, 2);
  expect_error("<:a/>", GTEXT_XML_E_NAMESPACE, 1, 1, 2);
  expect_error("<a:/>", GTEXT_XML_E_NAMESPACE, 1, 1, 2);
  expect_error("<a:1/>", GTEXT_XML_E_NAMESPACE, 1, 1, 2);
  expect_error("<a xmlns:p=\"u\" p:q:r=\"1\"/>", GTEXT_XML_E_NAMESPACE, 15, 1, 16);
  expect_error("<a :x=\"1\"/>", GTEXT_XML_E_NAMESPACE, 3, 1, 4);
  expect_error("<a x:=\"1\"/>", GTEXT_XML_E_NAMESPACE, 3, 1, 4);
  // Two attributes that are the same expanded name under different prefixes;
  // the same local name in different namespaces is fine.
  expect_error("<a xmlns:p=\"u\" xmlns:q=\"u\" p:x=\"1\" q:x=\"2\"/>", GTEXT_XML_E_DUPLICATE_ATTRIBUTE, 35, 1, 36);
  EXPECT_EQ(parse("<a xmlns:p=\"u\" xmlns:q=\"v\" p:x=\"1\" q:x=\"2\" x=\"3\"/>").status, GTEXT_XML_OK);
  // A declaration twice is a duplicate attribute, like any other name.
  expect_error("<a xmlns:p=\"u\" xmlns:p=\"v\"/>", GTEXT_XML_E_DUPLICATE_ATTRIBUTE, 15, 1, 16);
  expect_error("<a xmlns=\"u\" xmlns=\"v\"/>", GTEXT_XML_E_DUPLICATE_ATTRIBUTE, 13, 1, 14);
  // The default namespace is not a prefix: an unprefixed attribute named
  // xmlns is the declaration, and p:xmlns is an ordinary attribute.
  EXPECT_EQ(parse("<a xmlns:p=\"u\" p:xmlns=\"1\"/>").status, GTEXT_XML_OK);
}

TEST_F(XmlParse, DoctypeIdentifiersAndForms) {
  Result r = parse("<!DOCTYPE a SYSTEM 'x.dtd'><a/>");
  ASSERT_EQ(r.status, GTEXT_XML_OK);
  Node * d = gtext_markup_document_doctype(r.doc);
  EXPECT_STREQ(gtext_markup_doctype_system_id(d), "x.dtd");
  EXPECT_EQ(gtext_markup_doctype_public_id(d), nullptr);
  r = parse("<!DOCTYPE a PUBLIC \"\" \"\"><a/>");
  ASSERT_EQ(r.status, GTEXT_XML_OK);
  d = gtext_markup_document_doctype(r.doc);
  EXPECT_STREQ(gtext_markup_doctype_public_id(d), "");
  EXPECT_STREQ(gtext_markup_doctype_system_id(d), "");
  // The public identifier is normalized: runs of white space become one space.
  // (A tab is not a PubidChar, so it is refused: see below.)
  r = parse("<!DOCTYPE a PUBLIC \"  -//A \n B//  \" 'u'><a/>");
  ASSERT_EQ(r.status, GTEXT_XML_OK);
  EXPECT_STREQ(gtext_markup_doctype_public_id(gtext_markup_document_doctype(r.doc)), "-//A B//");
  // A system literal may hold what a public one cannot.
  EXPECT_EQ(parse("<!DOCTYPE a SYSTEM \"a b<c>&d[e]'\"><a/>").status, GTEXT_XML_OK);
  EXPECT_EQ(parse("<!DOCTYPE a PUBLIC \"-'()+,./:=?;!*#@$_%\" 'u'><a/>").status, GTEXT_XML_OK);
  EXPECT_EQ(parse("<!DOCTYPE a PUBLIC '-()' 'u'><a/>").status, GTEXT_XML_OK);
  // Public identifier characters.
  expect_error("<!DOCTYPE a PUBLIC \"a\tb\" 'u'><a/>", GTEXT_XML_E_DOCTYPE, 21, 1, 22);
  expect_error("<!DOCTYPE a PUBLIC \"a<b\" 'u'><a/>", GTEXT_XML_E_DOCTYPE, 21, 1, 22);
  expect_error("<!DOCTYPE a PUBLIC \"a[b\" 'u'><a/>", GTEXT_XML_E_DOCTYPE, 21, 1, 22);
  expect_error("<!DOCTYPE a PUBLIC 'a\\'b' 'u'><a/>", GTEXT_XML_E_DOCTYPE, 21, 1, 22);
  expect_error("<!DOCTYPE a PUBLIC 'a'b' 'u'><a/>", GTEXT_XML_E_DOCTYPE, 22, 1, 23);
  // Structure.
  expect_error("<!DOCTYPE a PUBLIC 'p'><a/>", GTEXT_XML_E_DOCTYPE, 22, 1, 23);
  expect_error("<!DOCTYPE a PUBLIC><a/>", GTEXT_XML_E_DOCTYPE, 18, 1, 19);
  expect_error("<!DOCTYPE a SYSTEM><a/>", GTEXT_XML_E_DOCTYPE, 18, 1, 19);
  expect_error("<!DOCTYPE a SYSTEM x><a/>", GTEXT_XML_E_DOCTYPE, 19, 1, 20);
  expect_error("<!DOCTYPE a SYSTEM 'x'y><a/>", GTEXT_XML_E_DOCTYPE, 22, 1, 23);
  expect_error("<!DOCTYPEa><a/>", GTEXT_XML_E_DOCTYPE, 9, 1, 10);
  expect_error("<!DOCTYPE><a/>", GTEXT_XML_E_DOCTYPE, 9, 1, 10);
  expect_error("<!DOCTYPE a", GTEXT_XML_E_UNEXPECTED_EOF, 11, 1, 12);
  expect_error("<!DOCTYPE a [", GTEXT_XML_E_UNEXPECTED_EOF, 13, 1, 14);
  expect_error("<!DOCTYPE a ]><a/>", GTEXT_XML_E_DOCTYPE, 12, 1, 13);
  expect_error("<!DOCTYPE a []", GTEXT_XML_E_UNEXPECTED_EOF, 14, 1, 15);
  expect_error("<!DOCTYPE a [] x><a/>", GTEXT_XML_E_DOCTYPE, 15, 1, 16);
  expect_error("<!DOCTYPE a><!DOCTYPE b><a/>", GTEXT_XML_E_DOCTYPE, 12, 1, 13);
  expect_error("<a/><!DOCTYPE a>", GTEXT_XML_E_DOCTYPE, 4, 1, 5);
  expect_error("<a><!DOCTYPE a></a>", GTEXT_XML_E_SYNTAX, 3, 1, 4);
  expect_error("<!doctype a><a/>", GTEXT_XML_E_SYNTAX, 0, 1, 1);
  EXPECT_EQ(parse("<!DOCTYPE a [] ><a/>").status, GTEXT_XML_OK);
  EXPECT_EQ(parse("<!DOCTYPE  a\n\tSYSTEM\n'x'\n[\n]\n>\n<a/>").status, GTEXT_XML_OK);
  EXPECT_EQ(parse("<?xml version=\"1.0\"?><!--c--><!DOCTYPE a><?p?><a/>").status, GTEXT_XML_OK);
  // A document type with a colon in its name is fine in XML; so is nothing else
  // special about it.
  EXPECT_EQ(parse("<!DOCTYPE p:a><p:a xmlns:p=\"u\"/>").status, GTEXT_XML_OK);
}

TEST_F(XmlParse, TheInternalSubsetIsSkippedByItsRealEnd) {
  struct Case {
    const char * subset;
    bool ok;
  };
  const Case cases[] = {
      {"", true},
      {" \n ", true},
      {"<!ELEMENT a ANY>", true},
      {"<!ELEMENT a (b|c)*>\n<!ATTLIST a x CDATA \"]\" y CDATA '>'>", true},
      {"<!ENTITY e \"]]>\">", true},
      {"<!ENTITY e '\"]'>", true},
      {"<!ENTITY % pe \"<!ELEMENT b ANY>\">", true},
      {"<!ENTITY % pe SYSTEM \"p.ent\">%pe;", true},
      {"%pe;", true},
      {" %pe; %other; ", true},
      {"<!NOTATION n SYSTEM \"x\">", true},
      {"<!-- ] -->", true},
      {"<!-- <!ELEMENT --> -->", false},                   // "--" inside a comment
      {"<!--a-->", true},
      {"<?p ]?>", true},
      {"<?p <!ENTITY?>", true},
      {"<!ENTITY e \"a\nb\">", true},
      {"<!ENTITY e \"\xC3\xA9\">", true},
      // Not terminated, or not what may be here.
      {"<!ELEMENT a ANY", false},
      {"<!ELEMENT a ANY<!ELEMENT b ANY>", false},
      {"<!ELEMENT a ANY ]", false},
      {"<!ELEMENT a \"x>", false},
      {"<!ELEMENTa ANY>", false},
      {"<!FOO a>", false},
      {"<![INCLUDE[ ]]>", false},
      {"<a/>", false},
      {"text", false},
      {"&amp;", false},
      {"%", false},
      {"%pe", false},
      {"% pe;", false},
      {"<!ENTITY e %pe;>", false},          // a PE reference inside a declaration
      {"<!ATTLIST a x CDATA %d;>", false},
      {"<!ENTITY % e %pe;>", false},
      {"<!ENTITY e %pe; %x;>", false},        // two references, not a literal between them
      {"<?", false},
      {"<!", false},
      {"<!-", false},
      {"<!--", false},
      {"<!ENTITY e \"\x01\">", false},
      {"<!ELEMENT a\xFF>", false},
  };
  for (const Case & c : cases) {
    const std::string text = std::string("<!DOCTYPE a [") + c.subset + "]><a/>";
    Result r = parse(text);
    EXPECT_EQ(r.status == GTEXT_XML_OK, c.ok) << text << " -> "
        << gtext_xml_status_string(r.status) << " " << (r.err.message ? r.err.message : "");
  }
  // Many declarations: linear, and the tree is just the doctype and the root.
  std::string many = "<!DOCTYPE a [";
  for (int i = 0; i < 20000; i++) {
    many += "<!ENTITY e" + std::to_string(i) + " \"]]>\">";
  }
  many += "]><a/>";
  EXPECT_EQ(tree(many), "#document\n  <!DOCTYPE \"a\" - ->\n  <a>\n");
}

TEST_F(XmlParse, ExternalIdentifiersAreRecordedAndNeverOpened) {
  // A name that would be a path or a URL is only text in the tree.
  Result r = parse("<!DOCTYPE a SYSTEM \"/etc/passwd\" []><a/>");
  ASSERT_EQ(r.status, GTEXT_XML_OK);
  EXPECT_STREQ(
      gtext_markup_doctype_system_id(gtext_markup_document_doctype(r.doc)), "/etc/passwd");
  expect_error("<!DOCTYPE a SYSTEM \"/etc/passwd\" [<!ENTITY x SYSTEM \"file:///etc/passwd\">]><a>&x;</a>",
      GTEXT_XML_E_ENTITY, 78, 1, 79);
}

TEST_F(XmlParse, ArgumentsAndStatuses) {
  Doc * doc = nullptr;
  GTEXT_XML_Error err;
  EXPECT_EQ(gtext_xml_parse("<a/>", 4, nullptr, nullptr, &err), GTEXT_XML_E_INVALID);
  EXPECT_EQ(err.code, GTEXT_XML_E_INVALID);
  EXPECT_EQ(gtext_xml_parse(nullptr, 4, nullptr, &doc, &err), GTEXT_XML_E_INVALID);
  EXPECT_EQ(doc, nullptr);
  EXPECT_EQ(gtext_xml_parse(nullptr, 0, nullptr, &doc, &err), GTEXT_XML_E_UNEXPECTED_EOF);
  // No error struct is fine, on success and on failure.
  EXPECT_EQ(gtext_xml_parse("<a/>", 4, nullptr, &doc, nullptr), GTEXT_XML_OK);
  ASSERT_NE(doc, nullptr);
  gtext_markup_document_free(doc);
  doc = nullptr;
  EXPECT_EQ(gtext_xml_parse("<a>", 3, nullptr, &doc, nullptr), GTEXT_XML_E_UNEXPECTED_EOF);
  EXPECT_EQ(doc, nullptr);
  // Embedded NUL: the length is the length.
  EXPECT_EQ(gtext_xml_parse("<a/>\0<b/>", 9, nullptr, &doc, &err), GTEXT_XML_E_CHARACTER);
  // A cleared error after a success.
  GTEXT_XML_Error stale;
  stale.code = GTEXT_XML_E_SYNTAX;
  EXPECT_EQ(gtext_xml_parse("<a/>", 4, nullptr, &doc, &stale), GTEXT_XML_OK);
  EXPECT_EQ(stale.code, GTEXT_XML_OK);
  gtext_markup_document_free(doc);
  gtext_xml_error_free(&stale);
  EXPECT_EQ(stale.code, GTEXT_XML_OK);
  EXPECT_EQ(stale.message, nullptr);
  gtext_xml_error_free(nullptr);

  // Every status has a name, and no two share one.
  std::vector<std::string> names;
  for (int s = GTEXT_XML_OK; s <= GTEXT_XML_E_XHTML; s++) {
    const char * name = gtext_xml_status_string(static_cast<GTEXT_XML_Status>(s));
    ASSERT_NE(name, nullptr);
    EXPECT_STRNE(name, "unknown status") << s;
    for (const std::string & seen : names) {
      EXPECT_NE(seen, name);
    }
    names.push_back(name);
  }
  EXPECT_STREQ(gtext_xml_status_string(static_cast<GTEXT_XML_Status>(9999)), "unknown status");
}

TEST_F(XmlParse, ADeepDocumentNeedsNoCStack) {
  // Far deeper than a recursive parser, a recursive free or a recursive walk
  // could survive on a default stack. The limit is lifted to say so.
  GTEXT_XML_Parse_Options o = gtext_xml_parse_options_default();
  o.max_depth = SIZE_MAX;
  const size_t depth = 300000;
  std::string text;
  for (size_t i = 0; i < depth; i++) {
    text += "<a>";
  }
  for (size_t i = 0; i < depth; i++) {
    text += "</a>";
  }
  Doc * doc = nullptr;
  ASSERT_EQ(gtext_xml_parse(text.data(), text.size(), &o, &doc, nullptr), GTEXT_XML_OK);
  EXPECT_EQ(gtext_markup_document_node_count(doc), depth + 1);
  gtext_markup_document_free(doc);
  // And refused at the default limit, rather than slowly.
  EXPECT_EQ(gtext_xml_parse(text.data(), text.size(), nullptr, &doc, nullptr),
      GTEXT_XML_E_LIMIT_DEPTH);
}

TEST_F(XmlParse, ManyAttributesWithUniqueAndDuplicateNames) {
  GTEXT_XML_Parse_Options o = gtext_xml_parse_options_default();
  o.max_attributes = 6000;
  std::string unique = "<a";
  for (int i = 0; i < 5000; i++) {
    unique += " n" + std::to_string(i) + "=\"" + std::to_string(i) + "\"";
  }
  Result r = parse(unique + "/>", &o);
  ASSERT_EQ(r.status, GTEXT_XML_OK);
  EXPECT_EQ(gtext_markup_element_attribute_count(gtext_markup_document_element(r.doc)), 5000u);
  // A duplicate at the very end is found, and reported where it is.
  std::string dup = unique + " n17=\"x\"/>";
  r = parse(dup, &o);
  EXPECT_EQ(r.status, GTEXT_XML_E_DUPLICATE_ATTRIBUTE);
  EXPECT_EQ(r.err.byte_offset, unique.size() + 1);
  // Prefixed attributes across many prefixes bound to few namespaces.
  std::string ns = "<a xmlns:p0=\"u\" xmlns:p1=\"u\" xmlns:p2=\"v\"";
  for (int i = 0; i < 1000; i++) {
    ns += " p" + std::to_string(i % 3) + ":n" + std::to_string(i) + "=\"x\"";
  }
  EXPECT_EQ(parse(ns + "/>", &o).status, GTEXT_XML_OK);
  // The same local name through two prefixes of one namespace, far apart.
  EXPECT_EQ(parse(ns + " p1:n0=\"x\"/>", &o).status, GTEXT_XML_E_DUPLICATE_ATTRIBUTE);
}

}  // namespace
