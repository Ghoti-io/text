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
 * The generation of a Markup document (markup_tree.h, gtext_markup_document_generation()): every change of the document advances it and nothing that only reads does, so that a
 * consumer which derives something from the tree can skip its walk when the value is the one it saw. Each mutator of the tree, of an attribute, of a node's data, of a
 * template's content, of a doctype's internal subset and of the document mode is asked in turn; each read, a failed change and a node's recorded location are asked not to advance it.
 */

#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include <ghoti.io/text/html.h>
#include <ghoti.io/text/markup.h>

namespace {

struct Fixture {
  GTEXT_Markup_Document * doc = nullptr;
  Fixture() {
    EXPECT_EQ(gtext_markup_document_new(nullptr, &doc), GTEXT_MARKUP_OK);
  }
  Fixture(const Fixture &) = delete;
  Fixture & operator=(const Fixture &) = delete;
  ~Fixture() {
    gtext_markup_document_free(doc);
  }
  uint64_t gen() const {
    return gtext_markup_document_generation(doc);
  }
  GTEXT_Markup_Node * element(const char * name) {
    GTEXT_Markup_Node * e = nullptr;
    EXPECT_EQ(gtext_markup_create_element(doc, nullptr, nullptr, name, &e), GTEXT_MARKUP_OK);
    return e;
  }
  GTEXT_Markup_Node * text(const char * data) {
    GTEXT_Markup_Node * t = nullptr;
    EXPECT_EQ(gtext_markup_create_text(doc, data, std::strlen(data), &t), GTEXT_MARKUP_OK);
    return t;
  }
};

TEST(MarkupGeneration, ANullDocumentIsZeroAndANewOneStartsWhereItIs) {
  EXPECT_EQ(gtext_markup_document_generation(nullptr), 0u);
  Fixture f;
  const uint64_t g = f.gen();
  EXPECT_EQ(f.gen(), g) << "reading it changes nothing";
}

TEST(MarkupGeneration, MakingNodesDoesNotChangeTheTreeButLinkingThemDoes) {
  Fixture f;
  const uint64_t g0 = f.gen();
  GTEXT_Markup_Node * html = f.element("html");
  GTEXT_Markup_Node * t = f.text("x");
  EXPECT_EQ(f.gen(), g0) << "a detached node is not part of the tree";
  ASSERT_EQ(gtext_markup_append_child(gtext_markup_document_root(f.doc), html), GTEXT_MARKUP_OK);
  const uint64_t g1 = f.gen();
  EXPECT_GT(g1, g0);
  ASSERT_EQ(gtext_markup_append_child(html, t), GTEXT_MARKUP_OK);
  const uint64_t g2 = f.gen();
  EXPECT_GT(g2, g1);
  GTEXT_Markup_Node * u = f.text("y");
  ASSERT_EQ(gtext_markup_insert_before(html, u, t), GTEXT_MARKUP_OK);
  const uint64_t g3 = f.gen();
  EXPECT_GT(g3, g2);
  GTEXT_Markup_Node * w = f.text("z");
  ASSERT_EQ(gtext_markup_replace(u, w), GTEXT_MARKUP_OK);
  const uint64_t g4 = f.gen();
  EXPECT_GT(g4, g3);
  ASSERT_EQ(gtext_markup_remove(w), GTEXT_MARKUP_OK);
  const uint64_t g5 = f.gen();
  EXPECT_GT(g5, g4);
  EXPECT_EQ(gtext_markup_remove(w), GTEXT_MARKUP_OK) << "already detached";
  EXPECT_EQ(f.gen(), g5) << "removing a node that is out of the tree is no change";
  ASSERT_EQ(gtext_markup_node_free(w), GTEXT_MARKUP_OK);
  EXPECT_EQ(f.gen(), g5) << "freeing a node that is out of the tree is no change";
}

TEST(MarkupGeneration, AFragmentsChildrenMovingInAreAChange) {
  Fixture f;
  GTEXT_Markup_Node * html = f.element("html");
  ASSERT_EQ(gtext_markup_append_child(gtext_markup_document_root(f.doc), html), GTEXT_MARKUP_OK);
  GTEXT_Markup_Node * frag = nullptr;
  ASSERT_EQ(gtext_markup_create_fragment(f.doc, &frag), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(frag, f.text("a")), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(frag, f.text("b")), GTEXT_MARKUP_OK);
  const uint64_t before = f.gen();
  ASSERT_EQ(gtext_markup_append_child(html, frag), GTEXT_MARKUP_OK);
  EXPECT_GT(f.gen(), before);
}

TEST(MarkupGeneration, ARefusedChangeChangesNothing) {
  Fixture f;
  GTEXT_Markup_Node * html = f.element("html");
  GTEXT_Markup_Node * body = f.element("body");
  ASSERT_EQ(gtext_markup_append_child(gtext_markup_document_root(f.doc), html), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(html, body), GTEXT_MARKUP_OK);
  const uint64_t before = f.gen();
  EXPECT_NE(gtext_markup_append_child(body, html), GTEXT_MARKUP_OK) << "an ancestor into its descendant";
  EXPECT_NE(gtext_markup_append_child(body, gtext_markup_document_root(f.doc)), GTEXT_MARKUP_OK);
  EXPECT_NE(gtext_markup_node_free(body), GTEXT_MARKUP_OK) << "a node in the tree is not freed";
  EXPECT_NE(gtext_markup_element_set_attribute(body, nullptr, nullptr, "", "v", 1), GTEXT_MARKUP_OK);
  EXPECT_EQ(f.gen(), before);
}

TEST(MarkupGeneration, AttributesAndDataAreChanges) {
  Fixture f;
  GTEXT_Markup_Node * e = f.element("div");
  GTEXT_Markup_Node * t = f.text("one");
  ASSERT_EQ(gtext_markup_append_child(gtext_markup_document_root(f.doc), e), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(e, t), GTEXT_MARKUP_OK);
  uint64_t g = f.gen();
  ASSERT_EQ(gtext_markup_element_set_attribute(e, nullptr, nullptr, "class", "a", 1), GTEXT_MARKUP_OK);
  EXPECT_GT(f.gen(), g) << "a new attribute";
  g = f.gen();
  ASSERT_EQ(gtext_markup_element_set_attribute(e, nullptr, nullptr, "class", "b", 1), GTEXT_MARKUP_OK);
  EXPECT_GT(f.gen(), g) << "a new value of an attribute";
  g = f.gen();
  EXPECT_FALSE(gtext_markup_element_remove_attribute(e, nullptr, "id"));
  EXPECT_EQ(f.gen(), g) << "removing an attribute that is not there";
  EXPECT_TRUE(gtext_markup_element_remove_attribute(e, nullptr, "class"));
  EXPECT_GT(f.gen(), g);
  g = f.gen();
  ASSERT_EQ(gtext_markup_node_set_data(t, "two", 3), GTEXT_MARKUP_OK);
  EXPECT_GT(f.gen(), g);
}

TEST(MarkupGeneration, ReadsAndLocationsDoNotChangeIt) {
  Fixture f;
  GTEXT_Markup_Node * e = f.element("div");
  ASSERT_EQ(gtext_markup_append_child(gtext_markup_document_root(f.doc), e), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_element_set_attribute(e, nullptr, nullptr, "style", "color:red", 9), GTEXT_MARKUP_OK);
  const uint64_t g = f.gen();
  GTEXT_Markup_Attribute a;
  EXPECT_TRUE(gtext_markup_element_get_attribute(e, nullptr, "style", &a));
  EXPECT_EQ(gtext_markup_element_attribute_count(e), 1u);
  EXPECT_EQ(gtext_markup_node_child_count(gtext_markup_document_root(f.doc)), 1u);
  char * text = nullptr;
  size_t n = 0;
  ASSERT_EQ(gtext_markup_node_text_content(e, nullptr, &text, &n), GTEXT_MARKUP_OK);
  gtext_markup_string_free(nullptr, text);
  GTEXT_Markup_Location loc;
  std::memset(&loc, 0, sizeof loc);
  loc.line = 3;
  gtext_markup_node_set_location(e, &loc);
  EXPECT_EQ(gtext_markup_element_set_attribute_location(e, 0, &loc), GTEXT_MARKUP_OK);
  EXPECT_EQ(f.gen(), g) << "where a node came from is not what it is";
}

TEST(MarkupGeneration, ATemplatesContentTheModeAndADoctypeAreChanges) {
  Fixture f;
  GTEXT_Markup_Node * tmpl = f.element("template");
  ASSERT_EQ(gtext_markup_append_child(gtext_markup_document_root(f.doc), tmpl), GTEXT_MARKUP_OK);
  uint64_t g = f.gen();
  GTEXT_Markup_Node * content = nullptr;
  ASSERT_EQ(gtext_markup_element_ensure_template_content(tmpl, &content), GTEXT_MARKUP_OK);
  EXPECT_GT(f.gen(), g) << "the content is made";
  g = f.gen();
  ASSERT_EQ(gtext_markup_element_ensure_template_content(tmpl, &content), GTEXT_MARKUP_OK);
  EXPECT_EQ(f.gen(), g) << "and asking again is not a change";
  ASSERT_EQ(gtext_markup_append_child(content, f.element("style")), GTEXT_MARKUP_OK);
  EXPECT_GT(f.gen(), g) << "a node into the content";
  g = f.gen();
  const GTEXT_Markup_Mode mode = gtext_markup_document_mode(f.doc);
  gtext_markup_document_set_mode(f.doc, mode);
  EXPECT_EQ(f.gen(), g) << "the mode it has";
  gtext_markup_document_set_mode(f.doc, mode == GTEXT_MARKUP_MODE_QUIRKS ? GTEXT_MARKUP_MODE_NO_QUIRKS : GTEXT_MARKUP_MODE_QUIRKS);
  EXPECT_GT(f.gen(), g) << "another mode (quirks change what a selector matches)";
  g = f.gen();
  GTEXT_Markup_Node * dt = nullptr;
  ASSERT_EQ(gtext_markup_create_doctype(f.doc, "root", nullptr, nullptr, &dt), GTEXT_MARKUP_OK);
  EXPECT_EQ(f.gen(), g);
  ASSERT_EQ(gtext_markup_doctype_insert_syntax(dt, 0, GTEXT_MARKUP_DTD_ELEMENT, "<!ELEMENT root ANY>", 19, nullptr), GTEXT_MARKUP_OK);
  EXPECT_GT(f.gen(), g) << "the internal subset";
  g = f.gen();
  EXPECT_NE(gtext_markup_doctype_insert_syntax(dt, 0, GTEXT_MARKUP_DTD_ELEMENT, "<!ELEMENT", 9, nullptr), GTEXT_MARKUP_OK);
  EXPECT_EQ(f.gen(), g) << "a refused edit";
}

TEST(MarkupGeneration, AParsedDocumentIsAtRestAndAnEditAdvancesIt) {
  GTEXT_Markup_Document * doc = nullptr;
  const char html[] = "<html><head><style>a{color:red}</style></head><body><p class=x>t</p></body></html>";
  ASSERT_EQ(gtext_html_parse(html, sizeof html - 1, nullptr, &doc, nullptr), GTEXT_HTML_OK);
  const uint64_t g = gtext_markup_document_generation(doc);
  EXPECT_EQ(gtext_markup_document_generation(doc), g);
  GTEXT_Markup_Node * style = gtext_markup_node_first_child(gtext_markup_node_first_child(gtext_markup_document_element(doc)));
  ASSERT_NE(style, nullptr);
  GTEXT_Markup_Node * text = gtext_markup_node_first_child(style);
  ASSERT_NE(text, nullptr);
  ASSERT_EQ(gtext_markup_node_set_data(text, "b{}", 3), GTEXT_MARKUP_OK);
  EXPECT_GT(gtext_markup_document_generation(doc), g);
  gtext_markup_document_free(doc);
}

} // namespace
