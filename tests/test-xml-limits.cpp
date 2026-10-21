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
 * The XML parser's limits (each at the limit, one past it, and far past it),
 * allocation failure at every point of a parse, and the seams the DTD phase will
 * attach to.
 *
 * A limit test that only checks the refusal could pass against a parser whose
 * limit is off by one in the permissive direction, so every limit is checked on
 * both sides, and the refusals are distinct statuses (the matrix in the plan
 * asks for that, and a caller can act on the difference).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <chrono>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/xml.h>
#include <ghoti.io/text/xml/xml_stream.h>

#include "../src/markup/markup_internal.h"
#include "../src/xml/xml_internal.h"
#include "markup_test_support.h"

namespace {

using Doc = GTEXT_Markup_Document;

GTEXT_XML_Status parse_with(const std::string & text, const GTEXT_XML_Parse_Options & opts,
    GTEXT_XML_Error * err = nullptr) {
  Doc * doc = nullptr;
  GTEXT_XML_Error local;
  const GTEXT_XML_Status status =
      gtext_xml_parse(text.data(), text.size(), &opts, &doc, err ? err : &local);
  if (status == GTEXT_XML_OK) {
    const char * problem = markup_check_invariants(doc);
    EXPECT_STREQ(problem ? problem : "ok", "ok") << text;
    gtext_markup_document_free(doc);
  }
  else {
    EXPECT_EQ(doc, nullptr);
  }
  return status;
}

TEST(XmlLimits, TheDefaultsAreSpelledOutAndZeroMeansTheDefault) {
  const GTEXT_XML_Parse_Options d = gtext_xml_parse_options_default();
  EXPECT_EQ(d.allocator, nullptr);
  EXPECT_EQ(d.max_input_bytes, 64u * 1024 * 1024);
  EXPECT_EQ(d.max_depth, 256u);
  EXPECT_EQ(d.max_attributes, 1024u);
  EXPECT_EQ(d.max_attribute_bytes, 16u * 1024 * 1024);
  EXPECT_EQ(d.max_name_bytes, 8192u);
  EXPECT_EQ(d.max_nodes, 10000000u);
  EXPECT_EQ(d.max_namespaces, 256u);
  EXPECT_EQ(d.max_external_source_bytes, 16u * 1024 * 1024);
  EXPECT_EQ(d.max_external_bytes, 64u * 1024 * 1024);
  EXPECT_EQ(d.max_expanded_bytes, 64u * 1024 * 1024);
  EXPECT_EQ(d.max_amplification, 100u);
  EXPECT_EQ(d.amplification_threshold, 8u * 1024 * 1024);
  EXPECT_EQ(d.max_entity_depth, 32u);
  EXPECT_EQ(d.max_references, 1000000u);
  EXPECT_EQ(d.max_declarations, 100000u);
  EXPECT_EQ(d.max_total_work, 256u * 1024 * 1024);

  // A zeroed struct is the defaults: no field's zero means "unlimited".
  GTEXT_XML_Parse_Options zero;
  std::memset(&zero, 0, sizeof(zero));
  std::string deep256, deep257;
  for (int i = 0; i < 256; i++) {
    deep256 += "<a>";
  }
  deep257 = deep256 + "<a>";
  std::string close256, close257;
  for (int i = 0; i < 256; i++) {
    close256 += "</a>";
  }
  close257 = close256 + "</a>";
  EXPECT_EQ(parse_with(deep256 + close256, zero), GTEXT_XML_OK);
  EXPECT_EQ(parse_with(deep257 + close257, zero), GTEXT_XML_E_LIMIT_DEPTH);
  EXPECT_EQ(parse_with(deep257 + close257, d), GTEXT_XML_E_LIMIT_DEPTH);
  GTEXT_XML_Parse_Options big = d;
  big.max_depth = SIZE_MAX;
  EXPECT_EQ(parse_with(deep257 + close257, big), GTEXT_XML_OK);
}

TEST(XmlLimits, Input) {
  GTEXT_XML_Parse_Options o = gtext_xml_parse_options_default();
  const std::string text = "<a>12345</a>";
  o.max_input_bytes = text.size();
  EXPECT_EQ(parse_with(text, o), GTEXT_XML_OK);
  o.max_input_bytes = text.size() - 1;
  GTEXT_XML_Error err;
  EXPECT_EQ(parse_with(text, o, &err), GTEXT_XML_E_LIMIT_INPUT);
  EXPECT_EQ(err.byte_offset, 0u);
  EXPECT_EQ(err.line, 1u);
  EXPECT_EQ(err.column, 1u);
  o.max_input_bytes = 1;
  EXPECT_EQ(parse_with(text, o), GTEXT_XML_E_LIMIT_INPUT);
  // The check is on the length, before anything is read: garbage that is too
  // long is a limit and not a syntax error.
  EXPECT_EQ(parse_with(std::string(100, '\xFF'), o), GTEXT_XML_E_LIMIT_INPUT);
}

TEST(XmlLimits, Depth) {
  GTEXT_XML_Parse_Options o = gtext_xml_parse_options_default();
  o.max_depth = 3;
  EXPECT_EQ(parse_with("<a><b><c/></b></a>", o), GTEXT_XML_OK);
  EXPECT_EQ(parse_with("<a><b><c>text<!--c--><?p?></c></b></a>", o), GTEXT_XML_OK);
  GTEXT_XML_Error err;
  // One past, as an empty element and as one with content; the position is the
  // start tag that went over.
  EXPECT_EQ(parse_with("<a><b><c><d/></c></b></a>", o, &err), GTEXT_XML_E_LIMIT_DEPTH);
  EXPECT_EQ(err.byte_offset, 9u);
  EXPECT_EQ(parse_with("<a><b><c><d></d></c></b></a>", o), GTEXT_XML_E_LIMIT_DEPTH);
  // Siblings are not depth.
  EXPECT_EQ(parse_with("<a><b/><b/><b/><b/><b/></a>", o), GTEXT_XML_OK);
  o.max_depth = 1;
  EXPECT_EQ(parse_with("<a/>", o), GTEXT_XML_OK);
  EXPECT_EQ(parse_with("<a>t</a>", o), GTEXT_XML_OK);
  EXPECT_EQ(parse_with("<a><b/></a>", o), GTEXT_XML_E_LIMIT_DEPTH);

  // The limit travels into the tree. The document's own limit counts the
  // character data inside the deepest element, which the parse allows, so it is
  // one more than the elements': a tree the parser built is always valid under
  // its own limit, and an edit that would go further is refused.
  o.max_depth = 2;
  Doc * doc = nullptr;
  ASSERT_EQ(gtext_xml_parse("<a><b>x</b></a>", 15, &o, &doc, nullptr), GTEXT_XML_OK);
  GTEXT_Markup_Node * b = gtext_markup_node_first_child(gtext_markup_document_element(doc));
  GTEXT_Markup_Node * c = nullptr;
  GTEXT_Markup_Node * d = nullptr;
  ASSERT_EQ(gtext_markup_create_element(doc, nullptr, nullptr, "c", &c), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_create_element(doc, nullptr, nullptr, "d", &d), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_append_child(b, c), GTEXT_MARKUP_OK);
  EXPECT_EQ(gtext_markup_append_child(c, d), GTEXT_MARKUP_E_LIMIT);
  gtext_markup_document_free(doc);
}

TEST(XmlLimits, Attributes) {
  GTEXT_XML_Parse_Options o = gtext_xml_parse_options_default();
  o.max_attributes = 3;
  EXPECT_EQ(parse_with("<a x=\"1\" y=\"2\" z=\"3\"/>", o), GTEXT_XML_OK);
  GTEXT_XML_Error err;
  EXPECT_EQ(parse_with("<a x=\"1\" y=\"2\" z=\"3\" w=\"4\"/>", o, &err), GTEXT_XML_E_LIMIT_ATTRIBUTES);
  EXPECT_EQ(err.byte_offset, 21u);  // The fourth attribute's name.
  // Namespace declarations are attributes, and count.
  EXPECT_EQ(parse_with("<a xmlns=\"u\" xmlns:p=\"v\" x=\"1\"/>", o), GTEXT_XML_OK);
  EXPECT_EQ(parse_with("<a xmlns=\"u\" xmlns:p=\"v\" xmlns:q=\"w\" x=\"1\"/>", o),
      GTEXT_XML_E_LIMIT_ATTRIBUTES);
  // Per element, not per document.
  EXPECT_EQ(parse_with("<r><a x=\"1\" y=\"2\" z=\"3\"/><a x=\"1\" y=\"2\" z=\"3\"/></r>", o), GTEXT_XML_OK);
  // A duplicate over the limit is over the limit.
  EXPECT_EQ(parse_with("<a x=\"1\" y=\"2\" z=\"3\" x=\"4\"/>", o), GTEXT_XML_E_LIMIT_ATTRIBUTES);
  // And far past, without ever holding them all.
  std::string many = "<a";
  for (int i = 0; i < 100000; i++) {
    many += " a" + std::to_string(i) + "=\"\"";
  }
  EXPECT_EQ(parse_with(many + "/>", gtext_xml_parse_options_default()), GTEXT_XML_E_LIMIT_ATTRIBUTES);
}

TEST(XmlLimits, AttributeValueBytes) {
  GTEXT_XML_Parse_Options o = gtext_xml_parse_options_default();
  o.max_attribute_bytes = 4;
  EXPECT_EQ(parse_with("<a x=\"abcd\"/>", o), GTEXT_XML_OK);
  GTEXT_XML_Error err;
  EXPECT_EQ(parse_with("<a x=\"abcde\"/>", o, &err), GTEXT_XML_E_LIMIT_VALUE);
  EXPECT_EQ(err.byte_offset, 3u);  // The attribute.
  // Counted after expansion, in bytes.
  EXPECT_EQ(parse_with("<a x=\"&amp;&lt;&gt;&#65;\"/>", o), GTEXT_XML_OK);
  EXPECT_EQ(parse_with("<a x=\"&amp;&lt;&gt;&#65;B\"/>", o), GTEXT_XML_E_LIMIT_VALUE);
  EXPECT_EQ(parse_with("<a x=\"\xC3\xA9\xC3\xA9\"/>", o), GTEXT_XML_OK);
  EXPECT_EQ(parse_with("<a x=\"\xC3\xA9\xC3\xA9z\"/>", o), GTEXT_XML_E_LIMIT_VALUE);
  EXPECT_EQ(parse_with("<a x=\"a\n\n\n\nb\"/>", o), GTEXT_XML_E_LIMIT_VALUE);
  // Each attribute has its own allowance.
  EXPECT_EQ(parse_with("<a x=\"abcd\" y=\"abcd\" z=\"abcd\"/>", o), GTEXT_XML_OK);
  // Far past: refused, and the refusal is the limit's.
  o.max_attribute_bytes = 1000;
  EXPECT_EQ(parse_with("<a x=\"" + std::string(5000000, 'x') + "\"/>", o), GTEXT_XML_E_LIMIT_VALUE);
}

TEST(XmlLimits, NameBytes) {
  GTEXT_XML_Parse_Options o = gtext_xml_parse_options_default();
  o.max_name_bytes = 3;
  EXPECT_EQ(parse_with("<abc/>", o), GTEXT_XML_OK);
  GTEXT_XML_Error err;
  EXPECT_EQ(parse_with("<abcd/>", o, &err), GTEXT_XML_E_LIMIT_NAME);
  EXPECT_EQ(err.byte_offset, 1u);
  EXPECT_EQ(parse_with("<a abc=\"\"/>", o), GTEXT_XML_OK);
  EXPECT_EQ(parse_with("<a abcd=\"\"/>", o), GTEXT_XML_E_LIMIT_NAME);
  EXPECT_EQ(parse_with("<a><?abc?></a>", o), GTEXT_XML_OK);
  EXPECT_EQ(parse_with("<a><?abcd?></a>", o), GTEXT_XML_E_LIMIT_NAME);
  EXPECT_EQ(parse_with("<!DOCTYPE abc><a/>", o), GTEXT_XML_OK);
  EXPECT_EQ(parse_with("<!DOCTYPE abcd><a/>", o), GTEXT_XML_E_LIMIT_NAME);
  EXPECT_EQ(parse_with("<a></abcd></a>", o), GTEXT_XML_E_LIMIT_NAME);
  // A prefix and its local name are one name, as written.
  EXPECT_EQ(parse_with("<a xmlns:p=\"u\"><p:a/></a>", o), GTEXT_XML_E_LIMIT_NAME);
  o.max_name_bytes = 4;
  EXPECT_EQ(parse_with("<a xmlns:p=\"u\"><p:a/></a>", o), GTEXT_XML_E_LIMIT_NAME);  // xmlns:p is 7
  o.max_name_bytes = 7;
  EXPECT_EQ(parse_with("<a xmlns:p=\"u\"><p:a/></a>", o), GTEXT_XML_OK);
  // Bytes, not characters.
  o.max_name_bytes = 4;
  EXPECT_EQ(parse_with("<\xC3\xA9\xC3\xA9/>", o), GTEXT_XML_OK);
  EXPECT_EQ(parse_with("<\xC3\xA9\xC3\xA9z/>", o), GTEXT_XML_E_LIMIT_NAME);
  // A reference's name; and far past, without reading the whole thing.
  o.max_name_bytes = 3;
  EXPECT_EQ(parse_with("<a>&abcd;</a>", o), GTEXT_XML_E_LIMIT_NAME);
  EXPECT_EQ(parse_with("<" + std::string(5000000, 'x') + "/>", gtext_xml_parse_options_default()),
      GTEXT_XML_E_LIMIT_NAME);
}

TEST(XmlLimits, Nodes) {
  // The document node is a node: the document below has five.
  const std::string text = "<!--c--><a><b/>t<![CDATA[x]]><?p?></a>";
  // document, comment, a, b, text, cdata, pi = 7
  GTEXT_XML_Parse_Options o = gtext_xml_parse_options_default();
  o.max_nodes = 7;
  EXPECT_EQ(parse_with(text, o), GTEXT_XML_OK);
  o.max_nodes = 6;
  GTEXT_XML_Error err;
  EXPECT_EQ(parse_with(text, o, &err), GTEXT_XML_E_LIMIT_NODES);
  o.max_nodes = 1;
  EXPECT_EQ(parse_with("<a/>", o), GTEXT_XML_E_LIMIT_NODES);
  o.max_nodes = 2;
  EXPECT_EQ(parse_with("<a/>", o), GTEXT_XML_OK);
  EXPECT_EQ(parse_with("<!DOCTYPE a><a/>", o), GTEXT_XML_E_LIMIT_NODES);
  o.max_nodes = 3;
  EXPECT_EQ(parse_with("<!DOCTYPE a><a/>", o), GTEXT_XML_OK);
  // Text is one node however many references it holds.
  o.max_nodes = 3;
  EXPECT_EQ(parse_with("<a>&amp;&lt;x&#65;</a>", o), GTEXT_XML_OK);
  // A flat document far past the limit.
  std::string flat = "<a>";
  for (int i = 0; i < 100000; i++) {
    flat += "<b/>";
  }
  o.max_nodes = 50000;
  EXPECT_EQ(parse_with(flat + "</a>", o), GTEXT_XML_E_LIMIT_NODES);
  o.max_nodes = 100002;
  EXPECT_EQ(parse_with(flat + "</a>", o), GTEXT_XML_OK);
}

TEST(XmlLimits, NamespacesInScope) {
  GTEXT_XML_Parse_Options o = gtext_xml_parse_options_default();
  o.max_namespaces = 2;
  EXPECT_EQ(parse_with("<a xmlns:p=\"1\" xmlns:q=\"2\"/>", o), GTEXT_XML_OK);
  GTEXT_XML_Error err;
  EXPECT_EQ(parse_with("<a xmlns:p=\"1\" xmlns:q=\"2\" xmlns:r=\"3\"/>", o, &err),
      GTEXT_XML_E_LIMIT_NAMESPACES);
  EXPECT_EQ(err.byte_offset, 27u);  // The third declaration.
  // In scope at once: a nested element adds to its parent's.
  EXPECT_EQ(parse_with("<a xmlns:p=\"1\"><b xmlns:q=\"2\"/></a>", o), GTEXT_XML_OK);
  EXPECT_EQ(parse_with("<a xmlns:p=\"1\"><b xmlns:q=\"2\"><c xmlns:r=\"3\"/></b></a>", o),
      GTEXT_XML_E_LIMIT_NAMESPACES);
  // A redeclaration of a prefix is another declaration in scope.
  EXPECT_EQ(parse_with("<a xmlns:p=\"1\"><b xmlns:p=\"2\"><c xmlns:p=\"3\"/></b></a>", o),
      GTEXT_XML_E_LIMIT_NAMESPACES);
  // Siblings are not in scope together, and the default namespace counts.
  EXPECT_EQ(parse_with("<r><a xmlns:p=\"1\" xmlns:q=\"2\"/><b xmlns:p=\"1\" xmlns=\"2\"/></r>", o),
      GTEXT_XML_OK);
  EXPECT_EQ(parse_with("<r xmlns=\"d\"><a xmlns:p=\"1\" xmlns:q=\"2\"/></r>", o),
      GTEXT_XML_E_LIMIT_NAMESPACES);
  // The implicit xml prefix is not a declaration.
  EXPECT_EQ(parse_with("<a xml:lang=\"en\" xmlns:p=\"1\" xmlns:q=\"2\"/>", o), GTEXT_XML_OK);
  // Far past: a chain of nested declarations.
  GTEXT_XML_Parse_Options d = gtext_xml_parse_options_default();
  d.max_depth = 1000;
  std::string chain;
  for (int i = 0; i < 1000; i++) {
    chain += "<a xmlns:p" + std::to_string(i) + "=\"u\">";
  }
  EXPECT_EQ(parse_with(chain, d), GTEXT_XML_E_LIMIT_NAMESPACES);
}

TEST(XmlLimits, EveryLimitIsItsOwnStatus) {
  std::vector<GTEXT_XML_Status> seen = {GTEXT_XML_E_LIMIT_INPUT, GTEXT_XML_E_LIMIT_DEPTH,
      GTEXT_XML_E_LIMIT_ATTRIBUTES, GTEXT_XML_E_LIMIT_VALUE, GTEXT_XML_E_LIMIT_NAME,
      GTEXT_XML_E_LIMIT_NODES, GTEXT_XML_E_LIMIT_NAMESPACES,
      GTEXT_XML_E_LIMIT_EXTERNAL_SOURCE, GTEXT_XML_E_LIMIT_EXTERNAL_TOTAL,
      GTEXT_XML_E_LIMIT_EXPANSION, GTEXT_XML_E_LIMIT_ENTITY_DEPTH,
      GTEXT_XML_E_LIMIT_REFERENCES, GTEXT_XML_E_LIMIT_DECLARATIONS,
      GTEXT_XML_E_LIMIT_WORK};
  for (size_t i = 0; i < seen.size(); i++) {
    for (size_t j = i + 1; j < seen.size(); j++) {
      EXPECT_NE(seen[i], seen[j]);
    }
  }
  EXPECT_STREQ(gtext_xml_status_string(GTEXT_XML_E_LIMIT_REFERENCES),
      "too many XML references");
}

TEST(XmlLimits, ReferenceAndDeclarationBudgetsAreChargedAtUse) {
  GTEXT_XML_Parse_Options o = gtext_xml_parse_options_default();
  o.max_references = 1;
  EXPECT_EQ(parse_with("<a>&amp;</a>", o), GTEXT_XML_OK);
  EXPECT_EQ(parse_with("<a>&amp;&lt;</a>", o), GTEXT_XML_E_LIMIT_REFERENCES);

  o.dtd_mode = GTEXT_XML_DTD_PROCESS;
  EXPECT_EQ(parse_with("<!DOCTYPE a [<!ENTITY e '&amp;'>]><a/>", o),
      GTEXT_XML_OK);
  EXPECT_EQ(parse_with("<!DOCTYPE a [<!ENTITY e '&amp;&#65;'>]><a/>", o),
      GTEXT_XML_E_LIMIT_REFERENCES);
  EXPECT_EQ(parse_with("<!DOCTYPE a [<!ENTITY % p ''><!ENTITY % q ''>"
      "<!ENTITY e '%p;%q;'>]><a/>", o),
      GTEXT_XML_E_LIMIT_REFERENCES);
  // A DTD reference is charged when parsed and again when a default value is
  // applied to an element.
  o.max_references = 2;
  EXPECT_EQ(parse_with("<!DOCTYPE a [<!ATTLIST a x CDATA '&amp;'>]><a/>", o),
      GTEXT_XML_OK);
  EXPECT_EQ(parse_with("<!DOCTYPE a [<!ATTLIST a x CDATA '&amp;&#65;'>]><a/>", o),
      GTEXT_XML_E_LIMIT_REFERENCES);

  const std::string one = "<!DOCTYPE a [<!ELEMENT a EMPTY>]><a/>";
  o.max_declarations = 1;
  EXPECT_EQ(parse_with(one, o), GTEXT_XML_OK);
  const std::string two =
      "<!DOCTYPE a [<!ELEMENT a EMPTY><!ATTLIST a id ID #IMPLIED>]><a/>";
  EXPECT_EQ(parse_with(two, o), GTEXT_XML_E_LIMIT_DECLARATIONS);
}

TEST(XmlLimits, FiniteDefaultsAdmitDenseDeclarationAndRepeatedReferenceWork) {
  GTEXT_XML_Parse_Options options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_PROCESS;
  std::string input = "<!DOCTYPE r [";
  for (size_t i = 0; i < options.max_declarations; i++) {
    input += "<!ELEMENT r EMPTY>";
  }
  input += "]><r>";
  for (size_t i = 0; i < 100000; i++) {
    input += "&amp;";
  }
  input += "</r>";
  EXPECT_EQ(parse_with(input, options), GTEXT_XML_OK);
}

TEST(XmlLimits, WorkFailureInDoctypeReleasesItsTemporaryBuffers) {
  const std::string input = "<!DOCTYPE r SYSTEM 'system-id'><r/>";
  const auto failing_parse_allocations = [&](size_t work_limit) {
    FaultAllocator allocator;
    GTEXT_XML_Parse_Options options = gtext_xml_parse_options_default();
    options.allocator = &allocator.alloc;
    options.max_total_work = work_limit;
    GTEXT_Markup_Document * doc = nullptr;
    EXPECT_EQ(gtext_xml_parse(input.data(), input.size(), &options, &doc, nullptr),
        GTEXT_XML_E_LIMIT_WORK);
    EXPECT_EQ(doc, nullptr);
    EXPECT_TRUE(allocator.live.empty());
    EXPECT_EQ(allocator.bad_frees, 0u);
    return allocator.calls;
  };
  const size_t before_name_copy =
      failing_parse_allocations(input.size() + 14);
  const size_t after_name_copy =
      failing_parse_allocations(input.size() + 15);
  // After the complete-input validation pass, the doctype name has been copied
  // into its temporary buffer before whitespace scanning exhausts the budget.
  EXPECT_GT(after_name_copy, before_name_copy);
}

TEST(XmlLimits, SourceAndAggregateExternalByteBudgetsAreCheckedBeforePush) {
  static const unsigned char root[] = "document";
  static const unsigned char five[] = "12345";
  static const unsigned char one[] = "x";
  xml_ctx p{};
  p.allocator = gtext_allocator_default();
  p.max_source_depth = 4;
  p.max_entity_depth = 4;
  p.max_external_source_bytes = 5;
  p.max_external_bytes = 5;
  p.max_expanded_bytes = 32;
  p.in = root;
  p.length = sizeof(root) - 1;
  ASSERT_EQ(xml_source_init(&p, root), GTEXT_XML_OK);
  ASSERT_EQ(xml_source_push(&p, XML_SOURCE_EXTERNAL_ENTITY, five, nullptr, 0,
      five, 5), GTEXT_XML_OK);
  EXPECT_EQ(p.external_bytes, 5u);
  ASSERT_EQ(xml_source_pop(&p), GTEXT_XML_OK);
  std::memset(&p.err, 0, sizeof(p.err));
  EXPECT_EQ(xml_source_push(&p, XML_SOURCE_EXTERNAL_ENTITY, one, nullptr, 0,
      one, 1),
      GTEXT_XML_E_LIMIT_EXTERNAL_TOTAL);
  EXPECT_EQ(p.source_count, 1u);
  xml_source_free(&p);

  p = {};
  p.allocator = gtext_allocator_default();
  p.max_source_depth = 4;
  p.max_entity_depth = 4;
  p.max_external_source_bytes = 4;
  p.max_external_bytes = 32;
  p.max_expanded_bytes = 32;
  p.in = root;
  p.length = sizeof(root) - 1;
  ASSERT_EQ(xml_source_init(&p, root), GTEXT_XML_OK);
  EXPECT_EQ(xml_source_push(&p, XML_SOURCE_EXTERNAL_ENTITY, five, nullptr, 0,
      five, 5),
      GTEXT_XML_E_LIMIT_EXTERNAL_SOURCE);
  EXPECT_EQ(p.source_count, 1u);
  xml_source_free(&p);

  p = {};
  p.allocator = gtext_allocator_default();
  p.max_source_depth = 4;
  p.max_entity_depth = 4;
  p.max_external_source_bytes = 5;
  p.max_external_bytes = 5;
  p.in = root;
  p.length = sizeof(root) - 1;
  ASSERT_EQ(xml_source_init(&p, root), GTEXT_XML_OK);
  ASSERT_EQ(xml_source_push(&p, XML_SOURCE_PARAMETER_ENTITY, one, nullptr, 0,
      nullptr, 0),
      GTEXT_XML_OK);
  EXPECT_EQ(xml_source_charge_external(&p, 5), GTEXT_XML_OK);
  EXPECT_EQ(p.expanded_bytes, 5u);
  EXPECT_EQ(xml_source_charge_external(&p, 1), GTEXT_XML_E_LIMIT_EXTERNAL_SOURCE);
  EXPECT_EQ(p.external_bytes, 6u);
  EXPECT_EQ(p.sources[p.source_count - 1].bytes_charged, 6u);
  EXPECT_EQ(xml_source_pop(&p), GTEXT_XML_OK);
  xml_source_free(&p);

  p = {};
  p.allocator = gtext_allocator_default();
  p.max_source_depth = 4;
  p.max_entity_depth = 4;
  p.max_external_source_bytes = 10;
  p.max_external_bytes = 10;
  p.max_expanded_bytes = 5;
  p.in = root;
  p.length = sizeof(root) - 1;
  ASSERT_EQ(xml_source_init(&p, root), GTEXT_XML_OK);
  ASSERT_EQ(xml_source_push(&p, XML_SOURCE_PARAMETER_ENTITY, one, nullptr, 0,
      nullptr, 0),
      GTEXT_XML_OK);
  ASSERT_EQ(xml_source_charge_external(&p, 5), GTEXT_XML_OK);
  EXPECT_EQ(xml_source_charge_external(&p, 1), GTEXT_XML_E_LIMIT_EXPANSION);
  EXPECT_EQ(p.external_bytes, 6u);
  EXPECT_EQ(p.expanded_bytes, 6u);
  EXPECT_EQ(p.sources[p.source_count - 1].bytes_charged, 6u);
  xml_source_free(&p);
}

TEST(XmlLimits, TotalWorkUsesCheckedArithmeticAndRejectsBeforeMoreParsing) {
  GTEXT_XML_Parse_Options o = gtext_xml_parse_options_default();
  const std::string input = "<root>content</root>";
  size_t low = 1;
  size_t high = 1024;
  while (low < high) {
    const size_t mid = low + (high - low) / 2;
    o.max_total_work = mid;
    if (parse_with(input, o) == GTEXT_XML_OK) high = mid;
    else low = mid + 1;
  }
  ASSERT_GT(low, 1u);
  o.max_total_work = low;
  EXPECT_EQ(parse_with(input, o), GTEXT_XML_OK);
  o.max_total_work = low - 1;
  EXPECT_EQ(parse_with(input, o), GTEXT_XML_E_LIMIT_WORK);

  xml_ctx p{};
  p.allocator = gtext_allocator_default();
  p.max_total_work = 10;
  EXPECT_EQ(xml_charge_work(&p, 10), GTEXT_XML_OK);
  EXPECT_EQ(xml_charge_work(&p, 1), GTEXT_XML_E_LIMIT_WORK);
  p.err = {};
  p.total_work = SIZE_MAX - 1;
  EXPECT_EQ(xml_charge_work(&p, 2), GTEXT_XML_E_LIMIT_WORK);
}

TEST(XmlLimits, EveryLedgerCounterRejectsOverflowWithoutChangingCounters) {
  xml_ctx p{};
  p.allocator = gtext_allocator_default();
  p.max_expanded_bytes = SIZE_MAX;
  p.expanded_bytes = SIZE_MAX - 1;
  EXPECT_EQ(xml_charge(&p, &p.expanded_bytes, SIZE_MAX, 2,
      GTEXT_XML_E_LIMIT_EXPANSION, "expansion"), GTEXT_XML_E_LIMIT_EXPANSION);
  EXPECT_EQ(p.expanded_bytes, SIZE_MAX - 1);

  p.err = {};
  p.references = SIZE_MAX - 1;
  EXPECT_EQ(xml_charge(&p, &p.references, SIZE_MAX, 2,
      GTEXT_XML_E_LIMIT_REFERENCES, "XML references"), GTEXT_XML_E_LIMIT_REFERENCES);
  EXPECT_EQ(p.references, SIZE_MAX - 1);

  p.err = {};
  p.declarations = SIZE_MAX - 1;
  EXPECT_EQ(xml_charge(&p, &p.declarations, SIZE_MAX, 2,
      GTEXT_XML_E_LIMIT_DECLARATIONS, "declarations"),
      GTEXT_XML_E_LIMIT_DECLARATIONS);
  EXPECT_EQ(p.declarations, SIZE_MAX - 1);

  unsigned char root[] = "<r/>";
  unsigned char external[] = "x";
  p = {};
  p.allocator = gtext_allocator_default();
  p.max_source_depth = 4;
  p.max_entity_depth = 4;
  p.max_external_source_bytes = SIZE_MAX;
  p.max_external_bytes = SIZE_MAX;
  p.max_expanded_bytes = SIZE_MAX;
  p.in = root;
  p.length = sizeof(root) - 1;
  ASSERT_EQ(xml_source_init(&p, root), GTEXT_XML_OK);
  ASSERT_EQ(xml_source_push(&p, XML_SOURCE_PARAMETER_ENTITY, external,
      nullptr, 0, external, 1), GTEXT_XML_OK);
  p.sources[p.source_count - 1].bytes_charged = SIZE_MAX - 1;
  p.expanded_bytes = 0;
  EXPECT_EQ(xml_source_charge_external(&p, 2),
      GTEXT_XML_E_LIMIT_EXTERNAL_SOURCE);
  EXPECT_EQ(p.sources[p.source_count - 1].bytes_charged, SIZE_MAX);
  EXPECT_EQ(p.external_bytes, 3u);
  EXPECT_EQ(p.expanded_bytes, 2u);

  p.err = {};
  p.sources[p.source_count - 1].bytes_charged = 0;
  p.external_bytes = SIZE_MAX - 1;
  p.expanded_bytes = 0;
  EXPECT_EQ(xml_source_charge_external(&p, 2),
      GTEXT_XML_E_LIMIT_EXTERNAL_TOTAL);
  EXPECT_EQ(p.sources[p.source_count - 1].bytes_charged, 2u);
  EXPECT_EQ(p.external_bytes, SIZE_MAX);
  EXPECT_EQ(p.expanded_bytes, 2u);
  xml_source_free(&p);
}

// ---- allocation failure --------------------------------------------------

// Parse with the n-th allocation failing, for n = 1, 2, ... until the parse
// gets all the way through without the failure firing. At every n that did
// fire, the answer is out-of-memory (or the document's own error, if it was
// found before the failing allocation), there is no tree, and the allocator
// holds nothing.
struct Sweep {
  size_t failures = 0;
  size_t successes = 0;
  size_t reached_error = 0;
  size_t swallowed = 0;  // A failed allocation the tree absorbs by design.
};

Sweep sweep(const std::string & text, const GTEXT_XML_Parse_Options & base,
    GTEXT_XML_Status expected_when_not_oom) {
  Sweep result;
  FaultAllocator fa;
  GTEXT_XML_Parse_Options o = base;
  o.allocator = &fa.alloc;
  for (size_t n = 1; n < 200000; n++) {
    fa.arm(n);
    Doc * doc = nullptr;
    GTEXT_XML_Error err;
    const GTEXT_XML_Status status =
        gtext_xml_parse(text.data(), text.size(), &o, &doc, &err);
    if (fa.failed && status == GTEXT_XML_OK) {
      // The markup tree grows its name table on a best-effort basis: a table
      // that cannot grow is slower and not wrong (markup_support.c), so one
      // failed allocation there is absorbed and the parse succeeds. That is the
      // only absorbed failure there is, and it is counted so that a second
      // kind would be noticed.
      result.swallowed++;
      const char * problem = markup_check_invariants(doc);
      EXPECT_STREQ(problem ? problem : "ok", "ok") << "n=" << n;
      gtext_markup_document_free(doc);
      EXPECT_TRUE(fa.live.empty()) << "n=" << n;
      continue;
    }
    if (fa.failed) {
      result.failures++;
      if (status == GTEXT_XML_E_OOM) {
        EXPECT_EQ(doc, nullptr) << "n=" << n;
        EXPECT_EQ(err.code, GTEXT_XML_E_OOM);
      }
      else {
        // The failing allocation was made on a path the parse did not need
        // to finish (it was already going to refuse the document).
        EXPECT_EQ(status, expected_when_not_oom) << "n=" << n;
        result.reached_error++;
      }
      if (doc) {
        gtext_markup_document_free(doc);
      }
      EXPECT_TRUE(fa.live.empty()) << "n=" << n << ": " << fa.live.size() << " blocks leaked";
      EXPECT_EQ(fa.bad_frees, 0u) << "n=" << n;
      continue;
    }
    fa.disarm();
    // The failure did not fire: the parse got through every allocation.
    EXPECT_EQ(status, expected_when_not_oom) << "n=" << n;
    if (status == GTEXT_XML_OK) {
      const char * problem = markup_check_invariants(doc);
      EXPECT_STREQ(problem ? problem : "ok", "ok");
      EXPECT_FALSE(fa.live.empty());
      gtext_markup_document_free(doc);
      result.successes++;
    }
    EXPECT_TRUE(fa.live.empty()) << "n=" << n;
    EXPECT_EQ(fa.bad_frees, 0u);
    return result;
  }
  ADD_FAILURE() << "the parse never finished";
  return result;
}

std::string rich_document() {
  std::string attrs;
  for (int i = 0; i < 40; i++) {
    attrs += " k" + std::to_string(i) + "=\"v&amp;" + std::to_string(i) + "\"";
  }
  std::string decls;
  for (int i = 0; i < 12; i++) {
    decls += " xmlns:n" + std::to_string(i) + "=\"urn:n" + std::to_string(i) + "\"";
  }
  std::string deep;
  std::string close;
  for (int i = 0; i < 30; i++) {
    deep += "<d" + std::to_string(i) + ">";
    close = "</d" + std::to_string(i) + ">" + close;
  }
  return "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
         "<!DOCTYPE r PUBLIC \"-//P  Q//EN\" \"r.dtd\" [<!ENTITY e \"v\"><!-- c -->]>\n"
         "<!--before--><?pi data?>\n"
         "<r xmlns=\"urn:d\"" + decls + attrs + " n3:x=\"1\" n4:y=\"2\">"
         "text &lt;&#65; <![CDATA[cd]]> more<!--in--><?p q?>"
         "<n1:e n2:z=\"3\"><f/></n1:e>" + deep + "leaf" + close + "</r>\n<!--after-->\n";
}

TEST(XmlOom, EveryAllocationOfARichDocumentFailsInTurn) {
  const Sweep s = sweep(rich_document(), gtext_xml_parse_options_default(), GTEXT_XML_OK);
  EXPECT_EQ(s.successes, 1u);
  // Dozens of nodes, atoms and arrays: a sweep that failed a handful of
  // allocations would be a sweep of something else.
  EXPECT_GT(s.failures, 150u);
  EXPECT_EQ(s.reached_error, 0u);
  // The name table's growth: it doubles from 64, and the document has fewer
  // than 200 distinct names.
  EXPECT_LE(s.swallowed, 2u);
}

TEST(XmlOom, EveryAllocationOfAnXml11DocumentFailsInTurn) {
  const Sweep s = sweep(
      "<?xml version=\"1.1\"?><r a='&#x1;'>&#x85;<child/></r>",
      gtext_xml_parse_options_default(), GTEXT_XML_OK);
  EXPECT_EQ(s.successes, 1u);
  EXPECT_GT(s.failures, 10u);
  EXPECT_EQ(s.reached_error, 0u);
}

TEST(XmlOom, ZeroBudgetIsOutOfMemory) {
  FaultAllocator fa;
  GTEXT_XML_Parse_Options o = gtext_xml_parse_options_default();
  o.allocator = &fa.alloc;
  fa.arm(1);
  Doc * doc = nullptr;
  GTEXT_XML_Error err;
  EXPECT_EQ(gtext_xml_parse("<a/>", 4, &o, &doc, &err), GTEXT_XML_E_OOM);
  EXPECT_EQ(doc, nullptr);
  EXPECT_EQ(err.code, GTEXT_XML_E_OOM);
  EXPECT_TRUE(fa.live.empty());
}

TEST(XmlOom, EveryAllocationOfARefusedDocumentFailsInTurn) {
  // Documents that are refused, at various depths of the parse: whatever the
  // failing allocation, there is no tree and nothing is left behind, and the
  // answer is either the document's own refusal or out-of-memory.
  struct Bad {
    std::string text;
    GTEXT_XML_Status status;
  };
  const Bad bad[] = {
      {"<a xmlns:p=\"u\" p:x=\"1\"><b></a>", GTEXT_XML_E_MISMATCHED_TAG},
      {"<a x=\"1\" y=\"2\" x=\"3\"/>", GTEXT_XML_E_DUPLICATE_ATTRIBUTE},
      {"<!DOCTYPE a PUBLIC \"p\" \"s\" [<!ELEMENT a ANY>]><a><!--c-->&nbsp;</a>", GTEXT_XML_E_ENTITY},
      {"<a><b/><c>text</c><![CDATA[x]]><q:z/></a>", GTEXT_XML_E_NAMESPACE},
  };
  for (const Bad & b : bad) {
    const Sweep s = sweep(b.text, gtext_xml_parse_options_default(), b.status);
    EXPECT_GT(s.failures, 3u) << b.text;
    EXPECT_EQ(s.successes, 0u);
  }
}

TEST(XmlOom, TheLimitsAreRefusalsNotLeaks) {
  GTEXT_XML_Parse_Options o = gtext_xml_parse_options_default();
  o.max_nodes = 5;
  const Sweep s = sweep("<a><b/><c/><d/><e/><f/></a>", o, GTEXT_XML_E_LIMIT_NODES);
  EXPECT_GT(s.failures, 3u);
}

// ---- the seams -----------------------------------------------------------

struct SeamLog {
  std::vector<std::string> resolved;
  std::vector<std::string> typed;
};

int resolve_stub(void * user, const char * name, size_t length, const char ** text, size_t * out) {
  static_cast<SeamLog *>(user)->resolved.emplace_back(name, length);
  if (length == 1 && name[0] == 'e') {
    *text = "XYZ";
    *out = 3;
    return 1;
  }
  return 0;
}

int type_stub(void * user, const char * element, size_t element_length, const char * attribute,
    size_t attribute_length) {
  static_cast<SeamLog *>(user)->typed.push_back(
      std::string(element, element_length) + "/" + std::string(attribute, attribute_length));
  return attribute_length == 1 && attribute[0] == 't';
}

const xml_default_attribute kDefaults[] = {{"x", "dx"}, {"d", "dv"}, {"p:z", "pz"}};

size_t defaults_stub(void *, const char * element, size_t element_length,
    const xml_default_attribute ** list) {
  if (element_length == 1 && element[0] == 'a') {
    *list = kDefaults;
    return 3;
  }
  *list = nullptr;
  return 0;
}

int parsed_source_stub(void *, const char * name, size_t length,
    const void ** identity, const void ** bytes, size_t * source_length) {
  static const char replacement[] = "<b>&n;</b><c/>";
  static const char nested[] = "deep";
  if (length == 1 && name[0] == 'e') {
    *identity = replacement;
    *bytes = replacement;
    *source_length = sizeof(replacement) - 1;
    return 1;
  }
  if (length == 1 && name[0] == 'n') {
    *identity = nested;
    *bytes = nested;
    *source_length = sizeof(nested) - 1;
    return 1;
  }
  return 0;
}

int predefined_source_stub(void * user, const char *, size_t,
    const void ** identity, const void ** bytes, size_t * source_length) {
  ++*static_cast<size_t *>(user);
  static const char replacement[] = "<b/>";
  *identity = replacement;
  *bytes = replacement;
  *source_length = sizeof(replacement) - 1;
  return 1;
}

struct DepthEntitySources {
  char identities[64]{};
  char replacements[65][8]{};
};

void init_depth_entity_sources(DepthEntitySources * sources) {
  for (size_t i = 0; i < 64; ++i) {
    if (i == 63) {
      std::strcpy(sources->replacements[i], "terminal");
    }
    else {
      std::snprintf(sources->replacements[i], sizeof(sources->replacements[i]),
          "&e%zu;", i + 1);
    }
  }
}

int open_depth_entity_source(void * user, const char * name, size_t length,
    const void ** identity, const void ** bytes, size_t * source_length) {
  if (length < 2 || name[0] != 'e') return 0;
  size_t index = 0;
  for (size_t i = 1; i < length; ++i) {
    if (name[i] < '0' || name[i] > '9') return 0;
    const size_t digit = static_cast<size_t>(name[i] - '0');
    if (index > (63 - digit) / 10) return 0;
    index = index * 10 + digit;
  }
  if (index >= 64) return 0;
  auto * sources = static_cast<DepthEntitySources *>(user);
  *identity = &sources->identities[index];
  *bytes = sources->replacements[index];
  *source_length = std::strlen(sources->replacements[index]);
  return 1;
}

std::string parse_dump(const std::string & text, const xml_seams * seams,
    GTEXT_XML_Status * status, const GTEXT_XML_Parse_Options * options = nullptr) {
  Doc * doc = nullptr;
  GTEXT_XML_Error err;
  *status = xml_parse_with_seams(text.data(), text.size(), options, seams, &doc, &err);
  if (*status != GTEXT_XML_OK) {
    return "";
  }
  const char * problem = markup_check_invariants(doc);
  EXPECT_STREQ(problem ? problem : "ok", "ok");
  std::string out = dump_to_string(gtext_markup_document_root(doc));
  gtext_markup_document_free(doc);
  return out;
}

TEST(XmlLimits, EachEntityUsePaysExpansionAndDepthBudgets) {
  SeamLog log;
  xml_seams seams{};
  seams.resolve_entity = resolve_stub;
  seams.user = &log;
  GTEXT_XML_Parse_Options o = gtext_xml_parse_options_default();
  o.max_expanded_bytes = 6;
  Doc * doc = nullptr;
  const std::string repeated = "<a>&e;&e;</a>";
  EXPECT_EQ(xml_parse_with_seams(repeated.data(), repeated.size(), &o, &seams,
                &doc, nullptr), GTEXT_XML_OK);
  gtext_markup_document_free(doc);
  doc = nullptr;
  o.max_expanded_bytes = 5;
  EXPECT_EQ(xml_parse_with_seams(repeated.data(), repeated.size(), &o, &seams,
                &doc, nullptr), GTEXT_XML_E_LIMIT_EXPANSION);
  EXPECT_EQ(doc, nullptr);

  DepthEntitySources sources;
  init_depth_entity_sources(&sources);
  xml_seams nested{};
  nested.open_entity_source = open_depth_entity_source;
  nested.user = &sources;
  o = gtext_xml_parse_options_default();
  o.dtd_mode = GTEXT_XML_DTD_PROCESS;
  o.max_entity_depth = 1;
  const std::string entity = "<a>&e0;</a>";
  EXPECT_EQ(xml_parse_with_seams(entity.data(), entity.size(), &o, &nested,
                &doc, nullptr), GTEXT_XML_E_LIMIT_ENTITY_DEPTH);
  EXPECT_EQ(doc, nullptr);

  std::strcpy(sources.replacements[31], "terminal");
  o.max_entity_depth = 32;
  EXPECT_EQ(xml_parse_with_seams(entity.data(), entity.size(), &o, &nested,
                &doc, nullptr), GTEXT_XML_OK);
  gtext_markup_document_free(doc);
  doc = nullptr;
  o.max_entity_depth = 31;
  EXPECT_EQ(xml_parse_with_seams(entity.data(), entity.size(), &o, &nested,
                &doc, nullptr), GTEXT_XML_E_LIMIT_ENTITY_DEPTH);
  EXPECT_EQ(doc, nullptr);
}

TEST(XmlSeams, NullSeamsChangeNothing) {
  const std::string rich = rich_document();
  const char * docs[] = {"<a x=\"1\">t&amp;</a>", "<a xmlns:p=\"u\"><p:b/></a>", rich.c_str()};
  xml_seams none;
  std::memset(&none, 0, sizeof(none));
  for (const char * text : docs) {
    GTEXT_XML_Status a, b, c;
    const std::string pub = parse_dump(text, nullptr, &a);
    const std::string zeroed = parse_dump(text, &none, &b);
    Doc * doc = nullptr;
    c = gtext_xml_parse(text, std::strlen(text), nullptr, &doc, nullptr);
    ASSERT_EQ(c, GTEXT_XML_OK);
    const std::string direct = dump_to_string(gtext_markup_document_root(doc));
    gtext_markup_document_free(doc);
    EXPECT_EQ(a, GTEXT_XML_OK);
    EXPECT_EQ(b, GTEXT_XML_OK);
    EXPECT_EQ(pub, direct);
    EXPECT_EQ(zeroed, direct);
  }
}

TEST(XmlSeams, AnEntityResolverIsAskedOnlyAboutWhatIsNotPredefined) {
  SeamLog log;
  xml_seams seams;
  std::memset(&seams, 0, sizeof(seams));
  seams.resolve_entity = resolve_stub;
  seams.user = &log;
  GTEXT_XML_Status status;
  EXPECT_EQ(parse_dump("<a x=\"&e;&amp;\">&amp;&e;&lt;&#65;</a>", &seams, &status),
      "#document\n  <a>\n    @x=\"XYZ&\"\n    \"&XYZ<A\"\n");
  EXPECT_EQ(status, GTEXT_XML_OK);
  ASSERT_EQ(log.resolved.size(), 2u);  // Never asked about amp, lt or a number.
  EXPECT_EQ(log.resolved[0], "e");
  // One it does not know is the same error as with no resolver.
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_XML_Error err;
  const std::string text = "<a>&nope;</a>";
  EXPECT_EQ(xml_parse_with_seams(text.data(), text.size(), nullptr, &seams, &doc, &err),
      GTEXT_XML_E_ENTITY);
  EXPECT_EQ(err.byte_offset, 3u);
  EXPECT_EQ(parse_dump("<a>&e;</a>", nullptr, &status), "");
  EXPECT_EQ(status, GTEXT_XML_E_ENTITY);
}

TEST(XmlSeams, AttributeTypesNormalizeFurther) {
  SeamLog log;
  xml_seams seams;
  std::memset(&seams, 0, sizeof(seams));
  seams.attribute_type = type_stub;
  seams.user = &log;
  GTEXT_XML_Status status;
  EXPECT_EQ(parse_dump("<a t=\"  x   y\t z  \" u=\"  x   y  \" v=\"   \" t2=\"  q \"/>", &seams, &status),
      "#document\n  <a>\n    @t=\"x y z\"\n    @u=\"  x   y  \"\n    @v=\"   \"\n    @t2=\"  q \"\n");
  EXPECT_EQ(status, GTEXT_XML_OK);
  ASSERT_EQ(log.typed.size(), 4u);
  EXPECT_EQ(log.typed[0], "a/t");
  EXPECT_EQ(parse_dump("<a t=\"  \"/>", &seams, &status), "#document\n  <a>\n    @t=\"\"\n");
  EXPECT_EQ(parse_dump("<a t=\"&#32;x&#32;&#32;y&#32;\"/>", &seams, &status),
      "#document\n  <a>\n    @t=\"x y\"\n");
}

TEST(XmlSeams, DefaultAttributesAreAddedWhenAbsent) {
  xml_seams seams;
  std::memset(&seams, 0, sizeof(seams));
  seams.default_attributes = defaults_stub;
  GTEXT_XML_Status status;
  // x is given, so its default is not used; d is added after the written ones;
  // p:z resolves through the scope like a written attribute.
  EXPECT_EQ(parse_dump("<a xmlns:p=\"u\" x=\"1\"/>", &seams, &status),
      "#document\n  <a>\n"
      "    @{http://www.w3.org/2000/xmlns/}xmlns:p=\"u\"\n"
      "    @x=\"1\"\n    @d=\"dv\"\n    @{u}p:z=\"pz\"\n");
  EXPECT_EQ(status, GTEXT_XML_OK);
  // A default that needs a prefix nobody declared is the same namespace error.
  EXPECT_EQ(parse_dump("<a/>", &seams, &status), "");
  EXPECT_EQ(status, GTEXT_XML_E_NAMESPACE);
  // An element with no defaults is untouched.
  EXPECT_EQ(parse_dump("<b/>", &seams, &status), "#document\n  <b>\n");
  // Defaults are attributes, and count against the limit.
  GTEXT_XML_Parse_Options o = gtext_xml_parse_options_default();
  o.max_attributes = 3;
  Doc * doc = nullptr;
  const std::string text = "<a xmlns:p=\"u\" x=\"1\"/>";
  EXPECT_EQ(xml_parse_with_seams(text.data(), text.size(), &o, &seams, &doc, nullptr),
      GTEXT_XML_E_LIMIT_ATTRIBUTES);
}

TEST(XmlSourceStack, PushPopRestoresTheParentCursorAndGrammarContext) {
  xml_ctx p{};
  const GTEXT_Allocator * allocator = gtext_allocator_default();
  p.allocator = allocator;
  p.max_source_depth = 3;
  static const unsigned char parent[] = "<root>";
  static const unsigned char child[] = "text";
  p.in = parent;
  p.length = sizeof(parent) - 1;
  p.pos = 2;
  p.line = 4;
  p.column = 3;
  p.cursor_previous = 'o';
  p.version = XML_VERSION_10;
  p.frame_count = 1;
  ASSERT_EQ(xml_source_init(&p, parent), GTEXT_XML_OK);
  ASSERT_EQ(xml_source_push(&p, XML_SOURCE_INTERNAL_ENTITY, child, nullptr, 0,
                child, sizeof(child) - 1), GTEXT_XML_OK);
  EXPECT_EQ(p.in, child);
  EXPECT_EQ(p.pos, 0u);
  EXPECT_EQ(p.line, 1u);
  EXPECT_EQ(p.column, 1u);
  EXPECT_EQ(p.source_count, 2u);

  p.pos = 4;
  p.version = XML_VERSION_11;
  ASSERT_EQ(xml_source_pop(&p), GTEXT_XML_OK);
  EXPECT_EQ(p.in, parent);
  EXPECT_EQ(p.length, sizeof(parent) - 1);
  EXPECT_EQ(p.pos, 2u);
  EXPECT_EQ(p.line, 4u);
  EXPECT_EQ(p.column, 3u);
  EXPECT_EQ(p.cursor_previous, static_cast<uint32_t>('o'));
  EXPECT_EQ(p.version, XML_VERSION_10);
  EXPECT_EQ(p.source_count, 1u);
  xml_source_free(&p);
}

TEST(XmlSourceStack, RejectsPhysicalNestingAndTheBound) {
  xml_ctx p{};
  p.allocator = gtext_allocator_default();
  p.max_source_depth = 2;
  static const unsigned char root[] = "document";
  static const unsigned char entity[] = "replacement";
  static const unsigned char deep[] = "deep";
  p.in = root;
  p.length = sizeof(root) - 1;
  ASSERT_EQ(xml_source_init(&p, root), GTEXT_XML_OK);
  ASSERT_EQ(xml_source_push(&p, XML_SOURCE_INTERNAL_ENTITY, entity, nullptr, 0,
                entity, sizeof(entity) - 1), GTEXT_XML_OK);
  p.frame_count = 1;
  EXPECT_EQ(xml_source_pop(&p), GTEXT_XML_E_MISMATCHED_TAG);
  p.frame_count = 0;
  ASSERT_EQ(xml_source_pop(&p), GTEXT_XML_OK);
  // The parser preserves its first error; clear it here because the second
  // half isolates the independent depth-limit outcome.
  std::memset(&p.err, 0, sizeof(p.err));
  ASSERT_EQ(xml_source_push(&p, XML_SOURCE_INTERNAL_ENTITY, entity, nullptr, 0,
                entity, sizeof(entity) - 1), GTEXT_XML_OK);
  EXPECT_EQ(xml_source_push(&p, XML_SOURCE_INTERNAL_ENTITY, deep, nullptr, 0,
                deep, sizeof(deep) - 1), GTEXT_XML_E_LIMIT_DEPTH);
  xml_source_free(&p);
}

TEST(XmlSourceStack, ExternalSubsetDoesNotConsumeEntityDepth) {
  xml_ctx p{};
  p.allocator = gtext_allocator_default();
  p.max_source_depth = 4;
  p.max_entity_depth = 1;
  static const unsigned char root[] = "document";
  static const unsigned char subset[] = "<!ELEMENT r EMPTY>";
  static const unsigned char entity[] = "replacement";
  p.in = root;
  p.length = sizeof(root) - 1;
  ASSERT_EQ(xml_source_init(&p, root), GTEXT_XML_OK);
  ASSERT_EQ(xml_source_push(&p, XML_SOURCE_EXTERNAL_SUBSET, subset, nullptr, 0,
      subset, sizeof(subset) - 1), GTEXT_XML_OK);
  EXPECT_EQ(p.entity_depth, 0u);
  ASSERT_EQ(xml_source_push(&p, XML_SOURCE_INTERNAL_ENTITY, entity, nullptr, 0,
      entity, sizeof(entity) - 1), GTEXT_XML_OK);
  EXPECT_EQ(p.entity_depth, 1u);
  EXPECT_EQ(xml_source_pop(&p), GTEXT_XML_OK);
  EXPECT_EQ(p.entity_depth, 0u);
  EXPECT_EQ(xml_source_pop(&p), GTEXT_XML_OK);
  xml_source_free(&p);
}

TEST(XmlSourceStack, RejectsMissingIdentityAndInvalidBytes) {
  xml_ctx p{};
  p.allocator = gtext_allocator_default();
  p.max_source_depth = 2;
  static const unsigned char root[] = "document";
  static const unsigned char entity[] = "replacement";
  p.in = root;
  p.length = sizeof(root) - 1;
  ASSERT_EQ(xml_source_init(&p, root), GTEXT_XML_OK);
  EXPECT_EQ(xml_source_push(&p, XML_SOURCE_INTERNAL_ENTITY, nullptr,
                nullptr, 0, entity, sizeof(entity) - 1), GTEXT_XML_E_INVALID);
  EXPECT_EQ(xml_source_push(&p, XML_SOURCE_INTERNAL_ENTITY, entity,
                nullptr, 0, nullptr, 1), GTEXT_XML_E_INVALID);
  EXPECT_EQ(xml_source_push(&p, XML_SOURCE_INTERNAL_ENTITY, entity,
                nullptr, 0, nullptr, 0), GTEXT_XML_OK);
  EXPECT_NE(p.in, nullptr);
  EXPECT_EQ(xml_source_pop(&p), GTEXT_XML_OK);
  ASSERT_EQ(xml_source_push(&p, XML_SOURCE_PARAMETER_ENTITY, entity,
                nullptr, 0, entity, sizeof(entity) - 1), GTEXT_XML_OK);
  EXPECT_EQ(xml_source_pop(&p), GTEXT_XML_OK);
  ASSERT_EQ(xml_source_push(&p, XML_SOURCE_EXTERNAL_SUBSET, entity,
                nullptr, 0, entity, sizeof(entity) - 1), GTEXT_XML_OK);
  EXPECT_EQ(xml_source_pop(&p), GTEXT_XML_OK);
  xml_source_free(&p);
}

TEST(XmlSourceStack, DomParsesNestedMarkupAndResumesTheParent) {
  xml_seams seams{};
  seams.open_entity_source = parsed_source_stub;
  GTEXT_XML_Parse_Options options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_PROCESS;
  GTEXT_XML_Status status = GTEXT_XML_OK;
  const std::string tree = parse_dump("<!DOCTYPE a><a>left&e;right</a>",
      &seams, &status, &options);
  EXPECT_EQ(status, GTEXT_XML_OK);
  EXPECT_EQ(tree,
      "#document\n  <!DOCTYPE \"a\" - ->\n  <a>\n    \"left\"\n    <b>\n      \"deep\"\n    <c>\n    \"right\"\n");
}

TEST(XmlSourceStack, CompletedEntityIdentityCanBeEnteredAgain) {
  xml_seams seams{};
  seams.open_entity_source = parsed_source_stub;
  GTEXT_XML_Parse_Options options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_PROCESS;
  GTEXT_XML_Status status = GTEXT_XML_OK;
  const std::string tree = parse_dump("<a>&e;&e;</a>", &seams, &status, &options);
  EXPECT_EQ(status, GTEXT_XML_OK);
  EXPECT_EQ(tree,
      "#document\n  <a>\n    <b>\n      \"deep\"\n    <c>\n"
      "    <b>\n      \"deep\"\n    <c>\n");
}

TEST(XmlSourceStack, DomEnforcesTheDefaultNestedSourceDepth) {
  DepthEntitySources sources;
  init_depth_entity_sources(&sources);
  xml_seams seams{};
  seams.open_entity_source = open_depth_entity_source;
  seams.user = &sources;
  GTEXT_XML_Parse_Options options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_PROCESS;
  Doc * doc = nullptr;
  const std::string input = "<a>&e0;</a>";
  EXPECT_EQ(xml_parse_with_seams(input.data(), input.size(), &options, &seams,
                &doc, nullptr), GTEXT_XML_E_LIMIT_ENTITY_DEPTH);
  EXPECT_EQ(doc, nullptr);
}

TEST(XmlSourceStack, ParsedSourceSeamCannotReplacePredefinedEntities) {
  size_t calls = 0;
  xml_seams seams{};
  seams.open_entity_source = predefined_source_stub;
  seams.user = &calls;
  GTEXT_XML_Parse_Options options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_PROCESS;
  GTEXT_XML_Status status = GTEXT_XML_OK;
  EXPECT_EQ(parse_dump("<a>&lt;</a>", &seams, &status, &options),
      "#document\n  <a>\n    \"<\"\n");
  EXPECT_EQ(status, GTEXT_XML_OK);
  EXPECT_EQ(calls, 0u);

  EXPECT_EQ(parse_dump("<a>&e;</a>", &seams, &status), "");
  EXPECT_EQ(status, GTEXT_XML_E_ENTITY);
  EXPECT_EQ(calls, 0u);
}

TEST(XmlSourceStack, DomRejectsEntityBoundaryCrossingsAndCycles) {
  static const char closes_parent[] = "</a>";
  static const char opens_child[] = "<b>";
  struct BadSource { const char * bytes; };
  auto bad_source = [](void * user, const char * name, size_t length,
                        const void ** identity, const void ** bytes,
                        size_t * source_length) -> int {
    if (length != 1 || name[0] != 'e') return 0;
    const auto * source = static_cast<BadSource *>(user);
    *identity = source->bytes;
    *bytes = source->bytes;
    *source_length = std::strlen(source->bytes);
    return 1;
  };
  for (const char * source : {closes_parent, opens_child}) {
    BadSource user{source};
    xml_seams seams{};
    seams.open_entity_source = bad_source;
    seams.user = &user;
    GTEXT_XML_Parse_Options options = gtext_xml_parse_options_default();
    options.dtd_mode = GTEXT_XML_DTD_PROCESS;
    Doc * doc = nullptr;
    const std::string input = "<!DOCTYPE a><a>&e;</a>";
    GTEXT_XML_Error err;
    const GTEXT_XML_Status status = xml_parse_with_seams(input.data(), input.size(),
        &options, &seams, &doc, &err);
    EXPECT_EQ(status, GTEXT_XML_E_MISMATCHED_TAG);
    EXPECT_EQ(doc, nullptr);
  }

  static const char recursive[] = "<b>&e;</b>";
  BadSource user{recursive};
  xml_seams seams{};
  seams.open_entity_source = bad_source;
  seams.user = &user;
  GTEXT_XML_Parse_Options options = gtext_xml_parse_options_default();
  options.dtd_mode = GTEXT_XML_DTD_PROCESS;
  Doc * doc = nullptr;
  const std::string input = "<!DOCTYPE a><a>&e;</a>";
  EXPECT_EQ(xml_parse_with_seams(input.data(), input.size(), &options,
                &seams, &doc, nullptr), GTEXT_XML_E_ENTITY);
  EXPECT_EQ(doc, nullptr);
}

// ---- entity amplification -------------------------------------------------------------------------

std::string billion_laughs(int k) {
  std::string out = "<!DOCTYPE lolz [<!ENTITY lol0 \"lol\">";
  for (int level = 1; level <= 9; ++level) {
    out += "<!ENTITY lol" + std::to_string(level) + " \"";
    for (int j = 0; j < 10; ++j) out += "&lol" + std::to_string(level - 1) + ";";
    out += "\">";
  }
  return out + "]><lolz>&lol" + std::to_string(k) + ";</lolz>";
}

GTEXT_XML_Parse_Options processing() {
  GTEXT_XML_Parse_Options o = gtext_xml_parse_options_default();
  o.dtd_mode = GTEXT_XML_DTD_PROCESS;
  o.dtd_external_policy = GTEXT_XML_DTD_EXTERNAL_SKIP;
  return o;
}

GTEXT_XML_Status counted(const std::string & text, const GTEXT_XML_Parse_Options & o, size_t * work) {
  Doc * doc = nullptr;
  GTEXT_XML_Error err;
  const GTEXT_XML_Status st = xml_parse_counted(text.data(), text.size(), &o, &doc, &err, work);
  gtext_markup_document_free(doc);
  return st;
}

// A billion laughs (3 x 10^9 bytes from under a kilobyte) under the default limits is refused once the amplified size
// passes the threshold: a few megabytes of linear work. It used to run until the million-reference limit, recording a
// source for every expansion in a table it searched each time, which took minutes. The bound is on the parser's work
// counter; the wall clock is only a sanity bound, a hundred times what it takes.
TEST(XmlLimits, BillionLaughsUnderTheDefaultsIsRefusedByTheAmplificationBound) {
  const std::string bomb = billion_laughs(9);
  const auto start = std::chrono::steady_clock::now();
  size_t work = 0;
  EXPECT_EQ(counted(bomb, processing(), &work), GTEXT_XML_E_LIMIT_AMPLIFICATION);
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  // 8 MiB amplified at 3 bytes and 64 a reference is about 125,000 references; the work is a few units each.
  EXPECT_LT(work, (size_t)8 * 1024 * 1024) << "work " << work;
  EXPECT_LT(seconds, 10.0);

  // Every depth from 6 up is refused after the same work: what is refused is the expansion, not the document.
  size_t work6 = 0;
  EXPECT_EQ(counted(billion_laughs(6), processing(), &work6), GTEXT_XML_E_LIMIT_AMPLIFICATION);
  EXPECT_LE((double)work, 1.05 * (double)work6) << work << " " << work6;
  // Depth 5 (300 KB) is below the threshold and expands.
  EXPECT_EQ(counted(billion_laughs(5), processing(), nullptr), GTEXT_XML_OK);

  // The event parser applies the same bound, the input being what was fed.
  GTEXT_XML_Parse_Options o = processing();
  GTEXT_XML_Stream * stream = gtext_xml_stream_new(&o, [](const GTEXT_XML_Event *, void *) { return GTEXT_XML_OK; }, nullptr);
  ASSERT_NE(stream, nullptr);
  GTEXT_XML_Error err;
  GTEXT_XML_Status st = gtext_xml_stream_feed(stream, bomb.data(), bomb.size(), &err);
  if (st == GTEXT_XML_OK) st = gtext_xml_stream_finish(stream, &err);
  EXPECT_EQ(st, GTEXT_XML_E_LIMIT_AMPLIFICATION);
  EXPECT_LT(xml_stream_work(stream), (size_t)8 * 1024 * 1024);
  gtext_xml_stream_free(stream);
}

// With the bound turned off, the expansion limit stops the bomb, and the work it takes to get there is linear in the
// limit: the accounting does not walk anything that grows with what was expanded.
TEST(XmlLimits, BillionLaughsWithoutTheBoundStopsAtTheExpansionLimitAfterLinearWork) {
  GTEXT_XML_Parse_Options o = processing();
  o.max_amplification = SIZE_MAX;
  size_t at[2] = {0, 0};
  const size_t limit[2] = {(size_t)1 << 20, (size_t)4 << 20};
  for (int i = 0; i < 2; ++i) {
    o.max_expanded_bytes = limit[i];
    EXPECT_EQ(counted(billion_laughs(9), o, &at[i]), GTEXT_XML_E_LIMIT_EXPANSION);
  }
  EXPECT_GT(at[0], limit[0] / 2);
  EXPECT_LE((double)at[1] / (double)at[0], 4.5) << at[0] << " " << at[1];
  o.max_expanded_bytes = SIZE_MAX;
  o.max_references = 100000;
  size_t refs = 0;
  EXPECT_EQ(counted(billion_laughs(9), o, &refs), GTEXT_XML_E_LIMIT_REFERENCES);
  EXPECT_LT(refs, (size_t)100000 * 64);
}

// One large entity referenced many times (the "quadratic blowup"): each reference is cheap to write and expensive to
// expand. Under the defaults it is refused once past the threshold, after work that does not grow with the references
// that follow.
TEST(XmlLimits, OneLargeEntityReferencedManyTimesIsRefusedAfterBoundedWork) {
  auto blowup = [](size_t references, bool attribute) {
    std::string out = "<!DOCTYPE r [<!ENTITY a \"" + std::string(10000, 'a') + "\">]>";
    std::string refs;
    for (size_t i = 0; i < references; ++i) refs += "&a;";
    return out + (attribute ? "<r v=\"" + refs + "\"/>" : "<r>" + refs + "</r>");
  };
  for (bool attribute : {false, true}) {
    size_t small = 0, large = 0;
    EXPECT_EQ(counted(blowup(2000, attribute), processing(), &small), GTEXT_XML_E_LIMIT_AMPLIFICATION);
    EXPECT_EQ(counted(blowup(20000, attribute), processing(), &large), GTEXT_XML_E_LIMIT_AMPLIFICATION);
    EXPECT_LE((double)large, 1.05 * (double)small) << attribute << ": " << small << " " << large;
    // 100 references (1 MB) are below the threshold.
    EXPECT_EQ(counted(blowup(100, attribute), processing(), nullptr), GTEXT_XML_OK) << attribute;
  }
}

// The amplified size of a document of three references to a ten-byte entity is 3 x 10 replacement bytes plus 3 x 64 for
// the references: 222. Each limit at its bound passes and one past it fails, in both parsers.
TEST(XmlLimits, AmplificationThresholdAndFactorAtTheirBounds) {
  const std::string text = "<!DOCTYPE r [<!ENTITY a \"0123456789\">]><r>&a;&a;&a;</r>";
  const size_t amplified = 3 * 10 + 3 * 64;
  auto stream_status = [&](const GTEXT_XML_Parse_Options & o) {
    GTEXT_XML_Stream * s = gtext_xml_stream_new(&o, [](const GTEXT_XML_Event *, void *) { return GTEXT_XML_OK; }, nullptr);
    GTEXT_XML_Error err;
    GTEXT_XML_Status st = gtext_xml_stream_feed(s, text.data(), text.size(), &err);
    if (st == GTEXT_XML_OK) st = gtext_xml_stream_finish(s, &err);
    gtext_xml_stream_free(s);
    return st;
  };
  GTEXT_XML_Parse_Options o = processing();
  o.max_amplification = 1; // every byte past the threshold is out of proportion
  o.amplification_threshold = amplified;
  EXPECT_EQ(parse_with(text, o), GTEXT_XML_OK);
  EXPECT_EQ(stream_status(o), GTEXT_XML_OK);
  o.amplification_threshold = amplified - 1;
  GTEXT_XML_Error err;
  EXPECT_EQ(parse_with(text, o, &err), GTEXT_XML_E_LIMIT_AMPLIFICATION);
  EXPECT_EQ(err.byte_offset, text.rfind("&a;")) << "the reference that went over";
  EXPECT_EQ(stream_status(o), GTEXT_XML_E_LIMIT_AMPLIFICATION);

  o.amplification_threshold = 1;
  const size_t factor = (amplified + text.size() - 1) / text.size(); // the least that admits it
  ASSERT_GT(factor, 1u);
  o.max_amplification = factor;
  EXPECT_EQ(parse_with(text, o), GTEXT_XML_OK);
  EXPECT_EQ(stream_status(o), GTEXT_XML_OK);
  o.max_amplification = factor - 1;
  EXPECT_EQ(parse_with(text, o), GTEXT_XML_E_LIMIT_AMPLIFICATION);
  EXPECT_EQ(stream_status(o), GTEXT_XML_E_LIMIT_AMPLIFICATION);

  // Zero is the default, and the default does not reach a document this small.
  o.max_amplification = 0;
  o.amplification_threshold = 0;
  EXPECT_EQ(parse_with(text, o), GTEXT_XML_OK);
  EXPECT_STREQ(gtext_xml_status_string(GTEXT_XML_E_LIMIT_AMPLIFICATION), "entity amplification too large");
}

}  // namespace
