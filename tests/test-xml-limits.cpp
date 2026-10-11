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

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/xml.h>

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
      GTEXT_XML_E_LIMIT_NODES, GTEXT_XML_E_LIMIT_NAMESPACES};
  for (size_t i = 0; i < seen.size(); i++) {
    for (size_t j = i + 1; j < seen.size(); j++) {
      EXPECT_NE(seen[i], seen[j]);
    }
  }
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

std::string parse_dump(const std::string & text, const xml_seams * seams, GTEXT_XML_Status * status) {
  Doc * doc = nullptr;
  GTEXT_XML_Error err;
  *status = xml_parse_with_seams(text.data(), text.size(), nullptr, seams, &doc, &err);
  if (*status != GTEXT_XML_OK) {
    return "";
  }
  const char * problem = markup_check_invariants(doc);
  EXPECT_STREQ(problem ? problem : "ok", "ok");
  std::string out = dump_to_string(gtext_markup_document_root(doc));
  gtext_markup_document_free(doc);
  return out;
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

}  // namespace
