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
 * The order of checks (css_cssom_sheet.h, css_cssom_rule.h): one list for each entry point, the first error wins, and a test for every two adjacent entries of every list: a
 * state in which both conditions hold, and the status of the earlier. An order swap is seen by the pair it swapped. The lists are the Editors' Draft of CSSOM as read, with the
 * answers of this library where the draft and the browsers differ (the parse before the index: both browsers look at the index first).
 *
 * (e) `replace` and `replaceSync` are the constructed sheets', and (f) `setProperty` is the declaration's (test-cssom-decl.cpp, test-cssom-sheet.cpp): not here.
 */

#include <functional>
#include <string>

#include <gtest/gtest.h>

#include "cssom_test_support.h"

using namespace cot;

namespace {

struct World {
  Doc doc;
  Sheet s;
  explicit World(const std::string & css) : s(doc, css) {
    EXPECT_EQ(s.status, GTEXT_CSS_OK);
  }
};

/* ---- (a) CSSStyleSheet.insertRule ------------------------------------------------------------------------------------------------------------------ */

// (REENTRANT, SECURITY): a call from an observer is refused before anything of the sheet is looked at.
TEST(CssomOrderInsertRule, ReentrantBeforeSecurity) {
  World w("a{}");
  gtext_cssom_sheet_set_origin_clean(w.s.s, false);
  gtext_cssom_sheet_set_origin_clean(w.s.s, true);
  GTEXT_CSS_Status inner = GTEXT_CSS_OK;
  w.doc.inside = [&]() {
    gtext_cssom_sheet_set_origin_clean(w.s.s, false);
    inner = insert_rule(w.s.s, "b{}", 0).st;
    gtext_cssom_sheet_set_origin_clean(w.s.s, true);
    return GTEXT_CSS_OK;
  };
  ASSERT_EQ(insert_rule(w.s.s, "c{}", 1).st, GTEXT_CSS_OK);
  EXPECT_EQ(inner, GTEXT_CSS_E_REENTRANT);
}

// (SECURITY, NOT_ALLOWED): "If the origin-clean flag is unset, throw a SecurityError exception. If the disallow modification flag is set, throw a NotAllowedError".
TEST(CssomOrderInsertRule, SecurityBeforeNotAllowed) {
  World w("a{}");
  gtext_cssom_sheet_set_origin_clean(w.s.s, false);
  w.s.s->disallow_modification = true;
  EXPECT_EQ(insert_rule(w.s.s, "b{}", 0).st, GTEXT_CSS_E_SECURITY);
}

// (NOT_ALLOWED, SYNTAX): the flag is looked at before the text.
TEST(CssomOrderInsertRule, NotAllowedBeforeTheParse) {
  World w("a{}");
  w.s.s->disallow_modification = true;
  EXPECT_EQ(insert_rule(w.s.s, "a{} b{}", 0).st, GTEXT_CSS_E_NOT_ALLOWED);
}

// (SYNTAX, INDEX_SIZE): "Let parsed rule be the return value of invoking parse a rule with rule. If parsed rule is a syntax error, throw a SyntaxError" comes before "insert a CSS
// rule", whose first step is the index: a bad index with unparseable text is a SyntaxError (Chromium 156 and Firefox 158 answer IndexSizeError; the draft is followed).
TEST(CssomOrderInsertRule, TheParseBeforeTheIndex) {
  World w("a{}");
  EXPECT_EQ(insert_rule(w.s.s, "a{} b{}", 9).st, GTEXT_CSS_E_SYNTAX);
  EXPECT_EQ(insert_rule(w.s.s, "", 9).st, GTEXT_CSS_E_SYNTAX);
  EXPECT_EQ(insert_rule(w.s.s, "b{}", 9).st, GTEXT_CSS_E_INDEX_SIZE) << "the control: the same index with a rule";
}

// (the import into a constructed sheet, INDEX_SIZE): the SyntaxError is before "insert a CSS rule".
TEST(CssomOrderInsertRule, TheConstructedImportBeforeTheIndex) {
  World w("a{}");
  w.s.s->constructed = true;
  EXPECT_EQ(insert_rule(w.s.s, "@import url(a.css);", 9).st, GTEXT_CSS_E_SYNTAX);
  EXPECT_EQ(insert_rule(w.s.s, "b{}", 9).st, GTEXT_CSS_E_INDEX_SIZE);
}

// (INDEX_SIZE, HIERARCHY): an @import after a style rule is a HierarchyRequestError; with an index past the end it is IndexSizeError.
TEST(CssomOrderInsertRule, TheIndexBeforeTheConstraints) {
  World w("a{}");
  EXPECT_EQ(insert_rule(w.s.s, "@import url(a.css);", 9).st, GTEXT_CSS_E_INDEX_SIZE);
  EXPECT_EQ(insert_rule(w.s.s, "@import url(a.css);", 1).st, GTEXT_CSS_E_HIERARCHY) << "the control";
}

// (HIERARCHY, INVALID_STATE): an @namespace after a style rule violates the constraint and the list holds a style rule: the constraint is the first.
TEST(CssomOrderInsertRule, TheConstraintsBeforeTheNamespaceState) {
  World w("a{}");
  EXPECT_EQ(insert_rule(w.s.s, "@namespace x url(u);", 1).st, GTEXT_CSS_E_HIERARCHY);
  EXPECT_EQ(insert_rule(w.s.s, "@namespace x url(u);", 0).st, GTEXT_CSS_E_INVALID_STATE) << "the control: the constraint holds and the list does not";
}

/* ---- (b) a rule's insertRule, the nested flag --------------------------------------------------------------------------------------------------------- */

// (REENTRANT, INDEX_SIZE)
TEST(CssomOrderRuleInsertRule, ReentrantBeforeTheIndex) {
  World w("@media print{ a{} }");
  Rule media = at(w.s.rules(), 0);
  GTEXT_CSS_Status inner = GTEXT_CSS_OK;
  w.doc.inside = [&]() {
    inner = insert_rule(media.r, "b{}", 99).st;
    return GTEXT_CSS_OK;
  };
  ASSERT_EQ(insert_rule(w.s.s, "c{}", 1).st, GTEXT_CSS_OK);
  EXPECT_EQ(inner, GTEXT_CSS_E_REENTRANT);
}

// (INDEX_SIZE, the parse): "If index is greater than length, then throw an IndexSizeError" is the first step, before the text is parsed: a bad index with unparseable text is an
// IndexSizeError.
TEST(CssomOrderRuleInsertRule, TheIndexBeforeTheParse) {
  World w("a{ b{} }");
  Rule a = at(w.s.rules(), 0);
  EXPECT_EQ(insert_rule(a.r, "x y z{", 9).st, GTEXT_CSS_E_INDEX_SIZE);
  EXPECT_EQ(insert_rule(a.r, "", 9).st, GTEXT_CSS_E_INDEX_SIZE);
  EXPECT_EQ(insert_rule(a.r, "x y z{", 1).st, GTEXT_CSS_OK) << "the control: a selector list that is a rule at the right index (`x y z` is a valid selector)";
}

// (the parse, the constraints): a text of two rules is not one rule, whatever the first would be.
TEST(CssomOrderRuleInsertRule, TheParseBeforeTheConstraints) {
  World w("@media print{ a{} }");
  Rule media = at(w.s.rules(), 0);
  EXPECT_EQ(insert_rule(media.r, "@import url(a.css); b{}", 0).st, GTEXT_CSS_E_SYNTAX);
  EXPECT_EQ(insert_rule(media.r, "@import url(a.css);", 0).st, GTEXT_CSS_E_HIERARCHY) << "the control";
}

/* ---- (c) CSSStyleSheet.deleteRule --------------------------------------------------------------------------------------------------------------------- */

// (REENTRANT, SECURITY)
TEST(CssomOrderDeleteRule, ReentrantBeforeSecurity) {
  World w("a{} b{}");
  GTEXT_CSS_Status inner = GTEXT_CSS_OK;
  w.doc.inside = [&]() {
    gtext_cssom_sheet_set_origin_clean(w.s.s, false);
    inner = delete_rule(w.s.s, 0);
    gtext_cssom_sheet_set_origin_clean(w.s.s, true);
    return GTEXT_CSS_OK;
  };
  ASSERT_EQ(insert_rule(w.s.s, "c{}", 2).st, GTEXT_CSS_OK);
  EXPECT_EQ(inner, GTEXT_CSS_E_REENTRANT);
}

// (SECURITY, NOT_ALLOWED)
TEST(CssomOrderDeleteRule, SecurityBeforeNotAllowed) {
  World w("a{}");
  gtext_cssom_sheet_set_origin_clean(w.s.s, false);
  w.s.s->disallow_modification = true;
  EXPECT_EQ(delete_rule(w.s.s, 0), GTEXT_CSS_E_SECURITY);
}

// (NOT_ALLOWED, INDEX_SIZE)
TEST(CssomOrderDeleteRule, NotAllowedBeforeTheIndex) {
  World w("a{}");
  w.s.s->disallow_modification = true;
  EXPECT_EQ(delete_rule(w.s.s, 9), GTEXT_CSS_E_NOT_ALLOWED);
}

// (INDEX_SIZE, INVALID_STATE): an index past the end names no @namespace rule.
TEST(CssomOrderDeleteRule, TheIndexBeforeTheNamespaceState) {
  World w("@namespace x url(u); a{}");
  EXPECT_EQ(delete_rule(w.s.s, 9), GTEXT_CSS_E_INDEX_SIZE);
  EXPECT_EQ(delete_rule(w.s.s, 0), GTEXT_CSS_E_INVALID_STATE) << "the control";
}

/* ---- (d) a rule's deleteRule ---------------------------------------------------------------------------------------------------------------------------- */

// (REENTRANT, INDEX_SIZE): "remove a CSS rule" alone.
TEST(CssomOrderRuleDeleteRule, ReentrantBeforeTheIndex) {
  World w("@media print{ a{} }");
  Rule media = at(w.s.rules(), 0);
  GTEXT_CSS_Status inner = GTEXT_CSS_OK;
  w.doc.inside = [&]() {
    inner = delete_rule(media.r, 99);
    return GTEXT_CSS_OK;
  };
  ASSERT_EQ(insert_rule(w.s.s, "c{}", 1).st, GTEXT_CSS_OK);
  EXPECT_EQ(inner, GTEXT_CSS_E_REENTRANT);
  EXPECT_EQ(delete_rule(media.r, 99), GTEXT_CSS_E_INDEX_SIZE);
}

} // namespace
