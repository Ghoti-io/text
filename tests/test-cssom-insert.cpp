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
 * insertRule, deleteRule, appendRule, findRule and the legacy addRule and removeRule (css_cssom_sheet.h, css_cssom_rule.h): one test for each row of the error table of the
 * specification and each constraint of the order of a sheet's top, the nested flag and the declarations it turns into rules, the namespaces a selector is read in, the detach of
 * a deleted rule, identity, versions, notifications, counts and limits. The quoted sentences are the Editors' Draft of CSSOM as read, and CSS Animations 1 for the keyframes.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cssom_test_support.h"

using namespace cot;

namespace {

/// A sheet that is not constructed (a <style> element's), as a string of CSS.
struct Fixture {
  Doc doc;
  Sheet s;
  explicit Fixture(const std::string & css, const std::function<void(GTEXT_CSSOM_Config &)> & tweak = nullptr) : doc(tweak), s(doc, css) {
    EXPECT_EQ(s.status, GTEXT_CSS_OK);
  }
  std::string text() const {
    return joined(s.rules());
  }
};

/* ---- insert -------------------------------------------------------------------------------------------------------------------------------------- */

// "Insert a CSS rule ... Insert new rule into list at the zero-indexed position index. Return index."
TEST(CssomInsert, AnInsertedRuleIsAtTheIndexAndIsTheObjectTheListHolds) {
  Fixture f("a{color:red}");
  Ins r = insert_rule(f.s.s, "b{color:blue}", 1);
  ASSERT_EQ(r.st, GTEXT_CSS_OK);
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_CHANGED);
  ASSERT_TRUE(r.rule);
  EXPECT_EQ(f.text(), "a { color: red; } | b { color: blue; }");
  Rule again = at(f.s.rules(), 1);
  EXPECT_EQ(again.r, r.rule.r) << "the same object every time";
  EXPECT_EQ(gtext_cssom_rule_version(r.rule.r), 0u);
  Ins front = insert_rule(f.s.s, "c{}", 0);
  ASSERT_EQ(front.st, GTEXT_CSS_OK);
  EXPECT_EQ(f.text(), "c { } | a { color: red; } | b { color: blue; }");
  size_t index = 99;
  EXPECT_TRUE(gtext_cssom_rule_list_index_of(f.s.rules(), r.rule.r, &index));
  EXPECT_EQ(index, 2u) << "and the same pointer at its new index";
  GTEXT_CSSOM_Sheet * parent = gtext_cssom_rule_parent_sheet(r.rule.r);
  EXPECT_EQ(parent, f.s.s);
  gtext_cssom_sheet_release(parent);
}

TEST(CssomInsert, TheSheetsVersionAndGenerationMoveOnceAndOneEventIsSent) {
  Fixture f("a{}");
  const uint64_t v0 = gtext_cssom_sheet_version(f.s.s);
  const uint64_t g0 = gtext_cssom_document_generation(f.doc.d);
  Ins r = insert_rule(f.s.s, "b{}", 1);
  ASSERT_EQ(r.st, GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_sheet_version(f.s.s), v0 + 1);
  EXPECT_EQ(gtext_cssom_document_generation(f.doc.d), g0 + 1);
  EXPECT_EQ(r.o.generation, g0 + 1);
  ASSERT_EQ(f.doc.events.size(), 1u);
  EXPECT_EQ(f.doc.events[0].kind, GTEXT_CSSOM_EVENT_RULE_INSERTED);
  EXPECT_EQ(f.doc.events[0].sheet_id, gtext_cssom_sheet_id(f.s.s));
  EXPECT_EQ(f.doc.events[0].rule_id, gtext_cssom_rule_id(r.rule.r));
  EXPECT_EQ(f.doc.events[0].rule, static_cast<const void *>(r.rule.r));
}

// "If index is greater than length, then throw an IndexSizeError exception." The index may be the length: the rule is appended.
TEST(CssomInsert, AtTheEndIsAppendAndPastTheEndIsIndexSizeError) {
  Fixture f("a{}");
  EXPECT_EQ(insert_rule(f.s.s, "b{}", 1).st, GTEXT_CSS_OK);
  const std::string before = dump(f.s.s);
  const uint64_t v = gtext_cssom_sheet_version(f.s.s);
  Ins bad = insert_rule(f.s.s, "c{}", 3);
  EXPECT_EQ(bad.st, GTEXT_CSS_E_INDEX_SIZE);
  EXPECT_FALSE(bad.rule);
  EXPECT_EQ(dump(f.s.s), before);
  EXPECT_EQ(gtext_cssom_sheet_version(f.s.s), v);
  EXPECT_EQ(f.doc.events.size(), 1u) << "a failed call sends nothing";
}

// "Let parsed rule be the return value of invoking parse a rule with rule. If parsed rule is a syntax error, throw a SyntaxError DOMException."
TEST(CssomInsert, TextThatIsNotExactlyOneValidRuleIsSyntaxError) {
  Fixture f("a{}");
  const std::string before = dump(f.s.s);
  const char * const bad[] = {"", "   ", "a{} b{}", "b{} trailing", "@bogus x;", "@media (min-width:1px)", "@media print", "x >{}", "b{color:red} }", "{}", "@import;", ";", "@import 3;",
      "@import \"x\" {}", "@layer;", "@layer a b;", "@layer a, b {}", "@layer a..b;", "@layer \"x\";"};
  for (const char * t : bad) {
    Ins r = insert_rule(f.s.s, t, 0);
    EXPECT_EQ(r.st, GTEXT_CSS_E_SYNTAX) << '"' << t << '"';
    EXPECT_EQ(dump(f.s.s), before) << t;
  }
  // a rule closed by the end of the input is a rule: "parse a rule" closes it
  Ins closed = insert_rule(f.s.s, "b{color:red", 1);
  EXPECT_EQ(closed.st, GTEXT_CSS_OK);
  EXPECT_EQ(f.text(), "a { } | b { color: red; }");
  EXPECT_EQ(insert_rule(f.s.s, "  c { } ", 2).st, GTEXT_CSS_OK) << "white space around the rule is not another rule";
}

// A rule whose own grammar refuses it is invalid, and "If the whole rule is dropped, return a syntax error."
TEST(CssomInsert, AnAtRuleThatItsOwnGrammarRefusesIsSyntaxError) {
  Fixture f("");
  const char * const bad[] = {"@page foo bar{}", "@supports{}", "@namespace;", "@namespace x url(u) {}", "@container (min-width:1px){} x",
      "@scope (a) to {}", "@media print;"};
  for (const char * t : bad) {
    EXPECT_EQ(insert_rule(f.s.s, t, 0).st, GTEXT_CSS_E_SYNTAX) << t;
  }
  EXPECT_EQ(gtext_cssom_rule_list_length(f.s.rules()), 0u);
}

// Properties and Values API: all of the descriptors of `@property` are optional, a prelude may list names, and the statement form is a rule too, so each is inserted (and a prelude that
// is not one is a SyntaxError).
TEST(CssomInsert, APropertyRuleOfAnyOfItsFormsIsInsertedAndOneThatIsNotOneIsSyntaxError) {
  Fixture f("");
  for (const char * t : {"@property --p{}", "@property --p{syntax:'<length>'}", "@property --p <length>: 1px;", "@property --p;", "@property --a, --b{inherits:false}"}) {
    EXPECT_EQ(insert_rule(f.s.s, t, 0).st, GTEXT_CSS_OK) << t;
  }
  EXPECT_EQ(gtext_cssom_rule_list_length(f.s.rules()), 5u);
  EXPECT_EQ(f.text(),
      "@property --a, --b { syntax: \"*\"; inherits: false; } | @property --p { syntax: \"*\"; inherits: true; } | @property --p { syntax: \"<length>\"; inherits: true; initial-value: 1px; } | "
      "@property --p { syntax: \"<length>\"; inherits: true; } | @property --p { syntax: \"*\"; inherits: true; }");
  for (const char * t : {"@property --p, {}", "@property --p <foo>;", "@property p{}", "@property --p <length> | auto;", "@property --a, --b <length>;"}) {
    EXPECT_EQ(insert_rule(f.s.s, t, 0).st, GTEXT_CSS_E_SYNTAX) << t;
  }
  EXPECT_EQ(gtext_cssom_rule_list_length(f.s.rules()), 5u);
}

// The CSSWG's resolution of w3c/csswg-drafts#14227: a rule of several names is "a single object with a `names` array and not expand[ed] into multiple objects". insertRule makes one rule (index
// 0 is the rule, the list grows by one), its `name` is empty, its text keeps every name, and deleteRule removes it whole.
TEST(CssomInsert, APropertyRuleOfSeveralNamesIsInsertedAndDeletedAsOneRule) {
  Fixture f("a{}");
  Ins r = insert_rule(f.s.s, "@property --x, --y, --z { syntax: '<length>'; inherits: false }", 0);
  ASSERT_EQ(r.st, GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_rule_list_length(f.s.rules()), 2u) << "one rule, not three";
  GTEXT_CSSOM_String_List l{};
  ASSERT_EQ(gtext_cssom_property_get_names(r.rule.r, &l), GTEXT_CSS_OK);
  ASSERT_EQ(l.count, 3u);
  EXPECT_EQ(std::string(l.items[2].text, l.items[2].length), "--z");
  gtext_cssom_buffer_free(&reinterpret_cast<css_obj *>(r.rule.r)->core->alloc, static_cast<char *>(l.block));
  char * name = nullptr;
  size_t n = 0;
  ASSERT_EQ(gtext_cssom_rule_get_attribute(r.rule.r, "name", 4, &name, &n, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(n, 0u);
  gtext_cssom_buffer_free(&reinterpret_cast<css_obj *>(r.rule.r)->core->alloc, name);
  EXPECT_EQ(f.text(), "@property --x, --y, --z { syntax: \"<length>\"; inherits: false; } | a { }");
  ASSERT_EQ(delete_rule(f.s.s, 0), GTEXT_CSS_OK);
  EXPECT_EQ(f.text(), "a { }");
}

TEST(CssomInsert, ARuleWithRulesInsideIsInsertedWithAllOfThem) {
  Fixture f("a{}");
  Ins r = insert_rule(f.s.s, "@media print{ b{color:red} @supports (a:b){ c{} } }", 1);
  ASSERT_EQ(r.st, GTEXT_CSS_OK);
  EXPECT_EQ(shape(f.s), "style,media[style,supports[style]]");
  EXPECT_EQ(gtext_cssom_sheet_rule_total(f.s.s), 5u);
  Rule media = at(f.s.rules(), 1);
  EXPECT_EQ(media.r, r.rule.r);
  Rule inner = path(f.s, {1, 1, 0});
  ASSERT_TRUE(inner);
  Rule parent = Rule(gtext_cssom_rule_parent_rule(inner.r));
  EXPECT_EQ(gtext_cssom_rule_kind(parent.r), GTEXT_CSSOM_RULE_SUPPORTS);
  EXPECT_EQ(gtext_cssom_sheet_rule_total(f.s.s), 5u);
  // the source of an inserted rule says it is not a position in the sheet's text
  GTEXT_CSSOM_Source_Info src;
  ASSERT_EQ(gtext_cssom_rule_source(inner.r, &src), GTEXT_CSS_OK);
  EXPECT_EQ(src.kind, GTEXT_CSSOM_SOURCE_INSERTED);
  char * text = nullptr;
  EXPECT_EQ(gtext_cssom_rule_source_text(inner.r, &text, nullptr), GTEXT_CSS_E_INVALID_STATE);
  EXPECT_EQ(gtext_cssom_rule_source_text(at(f.s.rules(), 0).r, &text, nullptr), GTEXT_CSS_OK);
  gtext_cssom_buffer_free(&f.doc.core()->alloc, text);
}

// The ids are monotonic and never reused: an inserted rule, and the ones inside it, get the next ones.
TEST(CssomInsert, IdsAreTheNextOnesAndAFailedInsertionDoesNotSpendThem) {
  Fixture f("a{}");
  Ins first = insert_rule(f.s.s, "b{}", 1);
  const uint64_t id = gtext_cssom_rule_id(first.rule.r);
  EXPECT_EQ(insert_rule(f.s.s, "x >{}", 0).st, GTEXT_CSS_E_SYNTAX);
  EXPECT_EQ(insert_rule(f.s.s, "@media print{ c{} d{} }", 9).st, GTEXT_CSS_E_INDEX_SIZE);
  Ins second = insert_rule(f.s.s, "c{}", 2);
  EXPECT_EQ(gtext_cssom_rule_id(second.rule.r), id + 2) << "the id after the inserted rule's own source";
  Ins media = insert_rule(f.s.s, "@media print{ c{} d{} }", 3);
  ASSERT_EQ(media.st, GTEXT_CSS_OK);
  Rule inner = path(f.s, {3, 1});
  EXPECT_GT(gtext_cssom_rule_id(inner.r), gtext_cssom_rule_id(media.rule.r));
}

/* ---- order of a sheet's top ------------------------------------------------------------------------------------------------------------------ */

// CSS Cascade 5: "Any @import rules must precede all other valid at-rules and style rules in a style sheet (ignoring @charset and @layer statements)". "If new rule cannot be
// inserted into list at the zero-indexed position index due to constraints specified by CSS, then throw a HierarchyRequestError exception."
TEST(CssomInsert, AnImportMustPrecedeTheOtherRules) {
  Fixture f("a{} b{}");
  EXPECT_EQ(insert_rule(f.s.s, "@import url(x.css);", 1).st, GTEXT_CSS_E_HIERARCHY);
  EXPECT_EQ(insert_rule(f.s.s, "@import url(x.css);", 2).st, GTEXT_CSS_E_HIERARCHY);
  EXPECT_EQ(insert_rule(f.s.s, "@import url(x.css);", 0).st, GTEXT_CSS_OK);
  EXPECT_EQ(insert_rule(f.s.s, "@import url(y.css);", 1).st, GTEXT_CSS_OK) << "after another import";
  EXPECT_EQ(f.text(), "@import url(\"x.css\"); | @import url(\"y.css\"); | a { } | b { }");
  EXPECT_EQ(insert_rule(f.s.s, "@import url(z.css);", 3).st, GTEXT_CSS_E_HIERARCHY);
}

TEST(CssomInsert, ALayerStatementDoesNotStandInTheWayOfAnImport) {
  Fixture f("@layer a; b{}");
  EXPECT_EQ(insert_rule(f.s.s, "@import url(x.css);", 1).st, GTEXT_CSS_OK) << "after a layer statement";
  EXPECT_EQ(insert_rule(f.s.s, "@import url(y.css);", 3).st, GTEXT_CSS_E_HIERARCHY) << "not after a style rule";
  EXPECT_EQ(insert_rule(f.s.s, "@layer c;", 0).st, GTEXT_CSS_OK) << "a layer statement goes anywhere";
  EXPECT_EQ(insert_rule(f.s.s, "@layer d;", 4).st, GTEXT_CSS_OK);
  EXPECT_EQ(insert_rule(f.s.s, "@layer e { }", 0).st, GTEXT_CSS_E_HIERARCHY) << "a layer block is a rule an import must precede";
  EXPECT_EQ(insert_rule(f.s.s, "@layer e { }", 4).st, GTEXT_CSS_OK) << "after the import";
}

TEST(CssomInsert, AStyleRuleOrAnyOtherRuleCannotBeInsertedAheadOfAnImportOrANamespace) {
  Fixture f("@import url(a.css); @namespace x url(u);");
  EXPECT_EQ(insert_rule(f.s.s, "a{}", 0).st, GTEXT_CSS_E_HIERARCHY);
  EXPECT_EQ(insert_rule(f.s.s, "@media print{}", 1).st, GTEXT_CSS_E_HIERARCHY);
  EXPECT_EQ(insert_rule(f.s.s, "@font-face{font-family:x;src:url(a)}", 2).st, GTEXT_CSS_OK) << "after both";
  EXPECT_EQ(insert_rule(f.s.s, "a{}", 3).st, GTEXT_CSS_OK);
  EXPECT_EQ(insert_rule(f.s.s, "@layer s;", 0).st, GTEXT_CSS_OK);
}

// CSS Namespaces 3: an @namespace rule "follows all @charset and @import rules and precedes all other at-rules and style rules".
TEST(CssomInsert, ANamespaceFollowsTheImportsAndPrecedesTheOthers) {
  Fixture f("@import url(a.css);");
  EXPECT_EQ(insert_rule(f.s.s, "@namespace x url(u);", 0).st, GTEXT_CSS_E_HIERARCHY) << "before an import";
  EXPECT_EQ(insert_rule(f.s.s, "@namespace x url(u);", 1).st, GTEXT_CSS_OK);
  EXPECT_EQ(insert_rule(f.s.s, "@namespace y url(v);", 1).st, GTEXT_CSS_OK) << "among namespaces";
  EXPECT_EQ(insert_rule(f.s.s, "@namespace url(w);", 3).st, GTEXT_CSS_OK) << "the default one";
  EXPECT_EQ(f.text(), "@import url(\"a.css\"); | @namespace y url(\"v\"); | @namespace x url(\"u\"); | @namespace url(\"w\");");
}

// "If new rule is an @namespace at-rule, and list contains anything other than @import at-rules, and @namespace at-rules, throw an InvalidStateError exception."
TEST(CssomInsert, ANamespaceIntoAListWithAStyleRuleIsInvalidState) {
  Fixture f("a{}");
  EXPECT_EQ(insert_rule(f.s.s, "@namespace x url(u);", 0).st, GTEXT_CSS_E_INVALID_STATE) << "it would be first, but the list holds a style rule";
  EXPECT_EQ(insert_rule(f.s.s, "@namespace x url(u);", 1).st, GTEXT_CSS_E_HIERARCHY) << "after the style rule the constraint comes first";
  Fixture g("@layer a;");
  EXPECT_EQ(insert_rule(g.s.s, "@namespace x url(u);", 0).st, GTEXT_CSS_E_INVALID_STATE) << "a layer statement is something other than an import or a namespace";
  Fixture h("@import url(a.css); @namespace x url(u);");
  EXPECT_EQ(insert_rule(h.s.s, "@namespace y url(v);", 2).st, GTEXT_CSS_OK);
}

// "If parsed rule is an @import rule, and the constructed flag is set, throw a SyntaxError DOMException."
TEST(CssomInsert, AnImportIntoAConstructedSheetIsSyntaxError) {
  Fixture f("");
  f.s.s->constructed = true;
  const std::string before = dump(f.s.s);
  EXPECT_EQ(insert_rule(f.s.s, "@import url(a.css);", 0).st, GTEXT_CSS_E_SYNTAX);
  EXPECT_EQ(dump(f.s.s), before);
  EXPECT_EQ(insert_rule(f.s.s, "a{}", 0).st, GTEXT_CSS_OK);
}

TEST(CssomInsert, ACharsetIsNotARule) {
  Fixture f("");
  EXPECT_EQ(insert_rule(f.s.s, "@charset \"utf-8\";", 0).st, GTEXT_CSS_E_SYNTAX);
}

// "If the disallow modification flag is set, throw a NotAllowedError DOMException." and "If the origin-clean flag is unset, throw a SecurityError exception."
TEST(CssomInsert, ASheetThatMayNotBeModifiedOrReadRefusesTheCalls) {
  Fixture f("a{}");
  f.s.s->disallow_modification = true;
  EXPECT_EQ(insert_rule(f.s.s, "b{}", 0).st, GTEXT_CSS_E_NOT_ALLOWED);
  EXPECT_EQ(delete_rule(f.s.s, 0), GTEXT_CSS_E_NOT_ALLOWED);
  f.s.s->disallow_modification = false;
  gtext_cssom_sheet_set_origin_clean(f.s.s, false);
  EXPECT_EQ(insert_rule(f.s.s, "b{}", 0).st, GTEXT_CSS_E_SECURITY);
  EXPECT_EQ(delete_rule(f.s.s, 0), GTEXT_CSS_E_SECURITY);
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  sheet_text(f.s.s, &st);
  EXPECT_EQ(st, GTEXT_CSS_E_SECURITY);
  gtext_cssom_sheet_set_origin_clean(f.s.s, true);
  EXPECT_EQ(gtext_cssom_rule_list_length(f.s.rules()), 1u);
  EXPECT_STREQ(gtext_cssom_exception_name(GTEXT_CSS_E_SECURITY), "SecurityError");
}

/* ---- the rules inside a rule, the nested flag --------------------------------------------------------------------------------------------- */

// "If index is greater than length, then throw an IndexSizeError exception." comes before the text is looked at in a rule's list.
TEST(CssomInsertNested, TheIndexIsLookedAtBeforeTheText) {
  Fixture f("@media print{ a{} }");
  Rule media = at(f.s.rules(), 0);
  EXPECT_EQ(insert_rule(media.r, "a{} b{}", 5).st, GTEXT_CSS_E_INDEX_SIZE);
  EXPECT_EQ(insert_rule(media.r, "a{} b{}", 1).st, GTEXT_CSS_E_SYNTAX);
  EXPECT_EQ(insert_rule(media.r, "b{}", 1).st, GTEXT_CSS_OK);
  EXPECT_EQ(shape(f.s), "media[style,style]");
}

// "If new rule is a syntax error, and nested is set, perform the following substeps: Set declarations to the results of performing parse a CSS declaration block, on argument rule.
// If declarations is empty, throw a SyntaxError exception. Otherwise, set new rule to a new nested declarations rule with declarations as it contents."
TEST(CssomInsertNested, ADeclarationBlockIntoAStyleRuleIsANestedDeclarationsRule) {
  Fixture f("a{ b{} }");
  Rule a = at(f.s.rules(), 0);
  Ins r = insert_rule(a.r, "width:1px; color:red", 0);
  ASSERT_EQ(r.st, GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_rule_kind(r.rule.r), GTEXT_CSSOM_RULE_NESTED_DECLARATIONS);
  EXPECT_EQ(css(r.rule.r), "width: 1px; color: red;");
  EXPECT_EQ(shape(f.s), "style[nested-declarations,style]");
  Decl style = style_of(r.rule.r);
  EXPECT_EQ(text_of(style.d), "width: 1px; color: red;");
  EXPECT_EQ(insert_rule(a.r, "xwidth:1px", 0).st, GTEXT_CSS_E_SYNTAX) << "nothing valid";
  EXPECT_EQ(insert_rule(a.r, "color:nonsense", 0).st, GTEXT_CSS_E_SYNTAX) << "a block whose declarations are all invalid is empty";
  EXPECT_EQ(insert_rule(a.r, "", 0).st, GTEXT_CSS_E_SYNTAX);
  EXPECT_EQ(insert_rule(a.r, ";", 0).st, GTEXT_CSS_E_SYNTAX);
  EXPECT_EQ(insert_rule(a.r, "--x:1", 0).st, GTEXT_CSS_OK);
  EXPECT_EQ(insert_rule(a.r, "color:red !important", 0).st, GTEXT_CSS_OK);
  EXPECT_EQ(css(at(gtext_cssom_rule_css_rules(a.r), 0).r), "color: red !important;");
  EXPECT_EQ(insert_rule(a.r, "c{} d:e", 0).st, GTEXT_CSS_E_SYNTAX) << "a rule and a declaration are not one rule, and not declarations only";
}

// A declaration block is a rule only inside a style rule: the pinned web-platform-tests page css-nesting/nested-declarations-cssom.html expects a SyntaxError for it in a top-level
// @media and in a sheet, as both browsers do (the prose of the draft says only "with the nested flag set").
TEST(CssomInsertNested, ADeclarationBlockOutsideAStyleRuleIsSyntaxError) {
  Fixture f("@media print{ a{} } b{}");
  Rule media = at(f.s.rules(), 0);
  EXPECT_EQ(insert_rule(media.r, "width:1px", 0).st, GTEXT_CSS_E_SYNTAX);
  EXPECT_EQ(insert_rule(f.s.s, "width:1px", 0).st, GTEXT_CSS_E_SYNTAX);
  Fixture g("x{ @media print{ a{} } }");
  Rule inner = path(g.s, {0, 0});
  EXPECT_EQ(gtext_cssom_rule_kind(inner.r), GTEXT_CSSOM_RULE_MEDIA);
  Ins r = insert_rule(inner.r, "width:1px", 0);
  EXPECT_EQ(r.st, GTEXT_CSS_OK) << "the same grouping rule inside a style rule has the nested flag set";
  EXPECT_EQ(gtext_cssom_rule_kind(r.rule.r), GTEXT_CSSOM_RULE_NESTED_DECLARATIONS);
}

// CSS Nesting: a relative selector (a leading combinator, an implied "&") is valid inside a style rule and nowhere else; "&" is written.
TEST(CssomInsertNested, ARelativeSelectorIsValidOnlyInsideAStyleRule) {
  Fixture f("a{ b{} } @media print{ c{} }");
  Rule a = at(f.s.rules(), 0);
  Rule media = at(f.s.rules(), 1);
  EXPECT_EQ(insert_rule(a.r, "> d{}", 0).st, GTEXT_CSS_OK);
  EXPECT_EQ(insert_rule(a.r, "e{}", 0).st, GTEXT_CSS_OK);
  EXPECT_EQ(joined(gtext_cssom_rule_css_rules(a.r)), "& e { } | & > d { } | & b { }");
  EXPECT_EQ(insert_rule(media.r, "> d{}", 0).st, GTEXT_CSS_E_SYNTAX);
  EXPECT_EQ(insert_rule(f.s.s, "> d{}", 0).st, GTEXT_CSS_E_SYNTAX);
  EXPECT_EQ(insert_rule(media.r, "& d{}", 0).st, GTEXT_CSS_OK) << "& is a selector at the top";
  EXPECT_EQ(insert_rule(f.s.s, "&b{}", 0).st, GTEXT_CSS_E_SYNTAX) << "a type selector may not follow &";
}

TEST(CssomInsertNested, ARuleInAGroupInsideAStyleRuleIsReadRelative) {
  Fixture f("x{ @media print{ c{} } }");
  Rule media = path(f.s, {0, 0});
  EXPECT_EQ(insert_rule(media.r, "> d{}", 0).st, GTEXT_CSS_OK);
  EXPECT_EQ(insert_rule(media.r, "e{}", 0).st, GTEXT_CSS_OK);
  EXPECT_EQ(joined(gtext_cssom_rule_css_rules(media.r)), "& e { } | & > d { } | & c { }");
  Rule deep = path(f.s, {0, 0, 1});
  EXPECT_TRUE(deep);
  Fixture g("x{ @media print{ @supports (a:b){ c{} } } }");
  Rule sup = path(g.s, {0, 0, 0});
  EXPECT_EQ(insert_rule(sup.r, "> d{}", 0).st, GTEXT_CSS_OK) << "at any depth under the style rule";
}

// An at-rule that CSS allows at the top of a sheet and not here is a HierarchyRequestError ("constraints specified by CSS"): Chromium 156 and Firefox 158 both answer so; an at-rule
// that is not valid anywhere is a SyntaxError.
TEST(CssomInsertNested, AnAtRuleThePlaceDoesNotAllowIsHierarchyRequestError) {
  Fixture f("@media print{ a{} } b{ c{} }");
  Rule media = at(f.s.rules(), 0);
  Rule b = at(f.s.rules(), 1);
  EXPECT_EQ(insert_rule(media.r, "@import url(a.css);", 0).st, GTEXT_CSS_E_HIERARCHY);
  EXPECT_EQ(insert_rule(media.r, "@namespace x url(u);", 0).st, GTEXT_CSS_E_HIERARCHY);
  EXPECT_EQ(insert_rule(b.r, "@font-face{font-family:x;src:url(a)}", 0).st, GTEXT_CSS_E_HIERARCHY);
  EXPECT_EQ(insert_rule(b.r, "@import url(a.css);", 0).st, GTEXT_CSS_E_HIERARCHY);
  EXPECT_EQ(insert_rule(media.r, "@top-left{}", 0).st, GTEXT_CSS_E_SYNTAX) << "a margin rule is not a rule of a sheet";
  EXPECT_EQ(insert_rule(media.r, "@bogus x;", 0).st, GTEXT_CSS_E_SYNTAX);
  EXPECT_EQ(insert_rule(media.r, "@charset \"x\";", 0).st, GTEXT_CSS_E_SYNTAX);
  EXPECT_EQ(insert_rule(media.r, "@font-face{font-family:x;src:url(a)}", 0).st, GTEXT_CSS_OK);
  EXPECT_EQ(insert_rule(media.r, "@keyframes k{}", 0).st, GTEXT_CSS_OK);
  EXPECT_EQ(insert_rule(media.r, "@layer x;", 0).st, GTEXT_CSS_OK);
  EXPECT_EQ(insert_rule(b.r, "@media print{ d{} }", 0).st, GTEXT_CSS_OK);
  EXPECT_EQ(insert_rule(b.r, "@layer y;", 0).st, GTEXT_CSS_E_SYNTAX) << "an @layer statement is not a rule of a style rule's block (both browsers)";
  EXPECT_EQ(insert_rule(b.r, "@layer y{ d{} }", 0).st, GTEXT_CSS_OK) << "a block is";
}

// CSS Cascade 6: the declarations directly in an `@scope` are the scope root's, and CSSOM has them as a nested-declarations rule (both browsers write them in the rule's text).
TEST(CssomInsertNested, ADeclarationBlockIntoAScopeIsANestedDeclarationsRule) {
  Fixture f("@scope (.s){ b{} width:1px }");
  EXPECT_EQ(shape(f.s), "scope[style,nested-declarations]");
  Rule scope = at(f.s.rules(), 0);
  Ins r = insert_rule(scope.r, "color:red", 1);
  ASSERT_EQ(r.st, GTEXT_CSS_OK);
  EXPECT_EQ(css(scope.r), "@scope (.s) {\n  b { }\n  color: red;\n  width: 1px;\n}");
  Fixture g("@scope (.s){ @media print{ b{} } }");
  EXPECT_EQ(insert_rule(path(g.s, {0, 0}).r, "color:red", 0).st, GTEXT_CSS_E_SYNTAX) << "not in a grouping rule inside it";
}

TEST(CssomInsertNested, ARuleIntoAPageIsAMarginRule) {
  Fixture f("@page{ margin:1px }");
  Rule page = at(f.s.rules(), 0);
  Ins r = insert_rule(page.r, "@top-left{ content:\"x\" }", 0);
  ASSERT_EQ(r.st, GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_rule_kind(r.rule.r), GTEXT_CSSOM_RULE_PAGE_MARGIN);
  EXPECT_EQ(css(page.r), "@page {\n  margin: 1px;\n  @top-left { content: \"x\"; }\n}");
  EXPECT_EQ(insert_rule(page.r, "a{}", 0).st, GTEXT_CSS_E_HIERARCHY);
  EXPECT_EQ(insert_rule(page.r, "@top-left{}", 5).st, GTEXT_CSS_E_INDEX_SIZE);
  EXPECT_EQ(delete_rule(page.r, 0), GTEXT_CSS_OK);
  EXPECT_EQ(css(page.r), "@page { margin: 1px; }");
}

TEST(CssomInsertNested, OnlyAKindWithAListHasInsertRule) {
  Fixture f("@font-face{font-family:x;src:url(a)} @keyframes k{} @layer a;");
  for (size_t i = 0; i < 3; i++) {
    Rule r = at(f.s.rules(), i);
    EXPECT_EQ(insert_rule(r.r, "a{}", 0).st, GTEXT_CSS_E_UNSUPPORTED) << i;
    EXPECT_EQ(delete_rule(r.r, 0), GTEXT_CSS_E_UNSUPPORTED) << i;
  }
  EXPECT_EQ(gtext_cssom_rule_insert_rule(nullptr, "a{}", 3, 0, nullptr, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_sheet_insert_rule(nullptr, "a{}", 3, 0, nullptr, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_sheet_insert_rule(f.s.s, nullptr, 3, 0, nullptr, nullptr), GTEXT_CSS_E_INVALID);
}

// A detached rule is a valid object that no sheet holds: inserting into it changes nothing a document can see.
TEST(CssomInsertNested, InsertingIntoADetachedRuleChangesNoSheetAndSendsNothing) {
  Fixture f("x{ y{} }");
  Rule x = at(f.s.rules(), 0);
  ASSERT_EQ(delete_rule(f.s.s, 0), GTEXT_CSS_OK);
  f.doc.events.clear();
  const uint64_t g = gtext_cssom_document_generation(f.doc.d);
  const uint64_t v = gtext_cssom_sheet_version(f.s.s);
  Ins r = insert_rule(x.r, "z{}", 0);
  ASSERT_EQ(r.st, GTEXT_CSS_OK);
  EXPECT_EQ(css(x.r), "x {\n  & z { }\n  & y { }\n}");
  EXPECT_EQ(gtext_cssom_document_generation(f.doc.d), g);
  EXPECT_EQ(gtext_cssom_sheet_version(f.s.s), v);
  EXPECT_TRUE(f.doc.events.empty());
  EXPECT_TRUE(gtext_cssom_rule_detached(r.rule.r));
  EXPECT_EQ(gtext_cssom_sheet_rule_total(f.s.s), 0u);
}

/* ---- namespaces ---------------------------------------------------------------------------------------------------------------------------------- */

// Selectors 4 and Namespaces 3: a prefix the style sheet has not declared makes the selector invalid; the prefixes are the sheet's @namespace rules.
TEST(CssomInsertNamespaces, APrefixMustBeDeclaredByTheSheet) {
  Fixture f("@namespace x url(u); a{}");
  EXPECT_EQ(insert_rule(f.s.s, "x|b{}", 2).st, GTEXT_CSS_OK);
  EXPECT_EQ(insert_rule(f.s.s, "y|b{}", 2).st, GTEXT_CSS_E_SYNTAX);
  EXPECT_EQ(insert_rule(f.s.s, "*|b{}", 2).st, GTEXT_CSS_OK);
  EXPECT_EQ(insert_rule(f.s.s, "|b{}", 2).st, GTEXT_CSS_OK);
  EXPECT_EQ(joined(f.s.rules()), "@namespace x url(\"u\"); | a { } | |b { } | b { } | x|b { }") << "*| is written as nothing when there is no default namespace, as the browsers do";
}

TEST(CssomInsertNamespaces, AnInsertedNamespaceIsDeclaredForTheRulesInsertedAfterIt) {
  Fixture f("");
  EXPECT_EQ(insert_rule(f.s.s, "x|b{}", 0).st, GTEXT_CSS_E_SYNTAX);
  ASSERT_EQ(insert_rule(f.s.s, "@namespace x url(u);", 0).st, GTEXT_CSS_OK);
  EXPECT_EQ(insert_rule(f.s.s, "x|b{}", 1).st, GTEXT_CSS_OK);
  // the default namespace: a type selector is then in it, and *| says any
  Fixture g("");
  ASSERT_EQ(insert_rule(g.s.s, "@namespace url(d);", 0).st, GTEXT_CSS_OK);
  ASSERT_EQ(insert_rule(g.s.s, "a{}", 1).st, GTEXT_CSS_OK);
  ASSERT_EQ(insert_rule(g.s.s, "*|b{}", 2).st, GTEXT_CSS_OK);
  EXPECT_EQ(joined(g.s.rules()), "@namespace url(\"d\"); | a { } | *|b { }");
}

// A deleted namespace is no longer declared; a style rule read in it keeps its text after it has left the sheet.
TEST(CssomInsertNamespaces, ADeletedNamespaceIsNotDeclaredAndARuleKeepsItsText) {
  Fixture g("@namespace x url(u); @namespace y url(v);");
  Ins keep = insert_rule(g.s.s, "x|a{}", 2);
  ASSERT_EQ(keep.st, GTEXT_CSS_OK);
  EXPECT_EQ(delete_rule(g.s.s, 0), GTEXT_CSS_E_INVALID_STATE) << "a style rule follows";
  ASSERT_EQ(delete_rule(g.s.s, 2), GTEXT_CSS_OK);
  EXPECT_EQ(css(keep.rule.r), "x|a { }") << "the rule is read in the namespaces it was made in";
  ASSERT_EQ(delete_rule(g.s.s, 0), GTEXT_CSS_OK);
  EXPECT_EQ(insert_rule(g.s.s, "x|b{}", 1).st, GTEXT_CSS_E_SYNTAX) << "x is no longer declared";
  EXPECT_EQ(insert_rule(g.s.s, "y|b{}", 1).st, GTEXT_CSS_OK);
  EXPECT_EQ(css(keep.rule.r), "x|a { }");
}

TEST(CssomInsertNamespaces, ALaterNamespaceOfAPrefixReplacesTheEarlier) {
  Fixture f("");
  ASSERT_EQ(insert_rule(f.s.s, "@namespace x url(u);", 0).st, GTEXT_CSS_OK);
  ASSERT_EQ(insert_rule(f.s.s, "@namespace x url(v);", 1).st, GTEXT_CSS_OK);
  ASSERT_EQ(delete_rule(f.s.s, 1), GTEXT_CSS_OK);
  EXPECT_EQ(insert_rule(f.s.s, "x|a{}", 1).st, GTEXT_CSS_OK) << "x is bound again by the rule that is left";
  EXPECT_EQ(gtext_cssom_rule_list_length(f.s.rules()), 2u);
}

/* ---- delete ---------------------------------------------------------------------------------------------------------------------------------------- */

// "Remove rule old rule from list at the zero-indexed position index. Set old rule's parent CSS rule and parent CSS style sheet to null."
TEST(CssomDelete, ARuleIsDetachedAndStillReadable) {
  Fixture f("a{color:red} b{}");
  Rule a = at(f.s.rules(), 0);
  const uint64_t id = gtext_cssom_rule_id(a.r);
  const uint64_t v = gtext_cssom_sheet_version(f.s.s);
  ASSERT_EQ(delete_rule(f.s.s, 0), GTEXT_CSS_OK);
  EXPECT_EQ(f.text(), "b { }");
  EXPECT_EQ(gtext_cssom_sheet_version(f.s.s), v + 1);
  EXPECT_TRUE(gtext_cssom_rule_detached(a.r));
  EXPECT_EQ(gtext_cssom_rule_parent_sheet(a.r), nullptr);
  EXPECT_EQ(gtext_cssom_rule_parent_rule(a.r), nullptr);
  EXPECT_EQ(gtext_cssom_rule_id(a.r), id);
  EXPECT_EQ(css(a.r), "a { color: red; }");
  ASSERT_EQ(f.doc.events.size(), 1u);
  EXPECT_EQ(f.doc.events[0].kind, GTEXT_CSSOM_EVENT_RULE_REMOVED);
  EXPECT_EQ(f.doc.events[0].rule_id, id);
  EXPECT_EQ(f.doc.events[0].sheet_id, gtext_cssom_sheet_id(f.s.s));
}

TEST(CssomDelete, EveryRuleUnderTheDeletedOneIsDetachedWithIt) {
  Fixture f("@media print{ a{ b{} } } c{}");
  Rule media = at(f.s.rules(), 0);
  Rule a = path(f.s, {0, 0});
  Rule b = path(f.s, {0, 0, 0});
  ASSERT_EQ(delete_rule(f.s.s, 0), GTEXT_CSS_OK);
  for (GTEXT_CSSOM_Rule * r : {media.r, a.r, b.r}) {
    EXPECT_TRUE(gtext_cssom_rule_detached(r));
    EXPECT_EQ(gtext_cssom_rule_parent_sheet(r), nullptr);
  }
  Rule pa = Rule(gtext_cssom_rule_parent_rule(a.r));
  EXPECT_EQ(pa.r, media.r) << "the parent rule is the group the rule is still in";
  Rule pm = Rule(gtext_cssom_rule_parent_rule(media.r));
  EXPECT_FALSE(pm);
  EXPECT_EQ(gtext_cssom_sheet_rule_total(f.s.s), 1u);
  EXPECT_EQ(css(b.r), "& b { }") << "readable";
  // and the declarations of a detached rule change the rule and nothing else
  Decl sd = style_of(a.r);
  const uint64_t g = gtext_cssom_document_generation(f.doc.d);
  const uint64_t v = gtext_cssom_sheet_version(f.s.s);
  f.doc.events.clear();
  Result res = set(sd.d, "color", "red");
  EXPECT_EQ(res.st, GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_document_generation(f.doc.d), g);
  EXPECT_EQ(gtext_cssom_sheet_version(f.s.s), v);
  EXPECT_TRUE(f.doc.events.empty());
}

TEST(CssomDelete, TheIndexMustBeLessThanTheNumberOfRules) {
  Fixture f("a{}");
  const std::string before = dump(f.s.s);
  EXPECT_EQ(delete_rule(f.s.s, 1), GTEXT_CSS_E_INDEX_SIZE);
  EXPECT_EQ(delete_rule(f.s.s, SIZE_MAX), GTEXT_CSS_E_INDEX_SIZE);
  EXPECT_EQ(dump(f.s.s), before);
  EXPECT_EQ(delete_rule(f.s.s, 0), GTEXT_CSS_OK);
  EXPECT_EQ(delete_rule(f.s.s, 0), GTEXT_CSS_E_INDEX_SIZE);
  Fixture g("@media print{ a{} }");
  Rule media = at(g.s.rules(), 0);
  EXPECT_EQ(delete_rule(media.r, 1), GTEXT_CSS_E_INDEX_SIZE);
  EXPECT_EQ(delete_rule(media.r, 0), GTEXT_CSS_OK);
  EXPECT_EQ(shape(g.s), "media");
}

// "If old rule is an @namespace at-rule, and list contains anything other than @import at-rules, and @namespace at-rules, throw an InvalidStateError exception."
TEST(CssomDelete, ANamespaceCannotBeDeletedFromAListWithOtherRules) {
  Fixture f("@import url(a.css); @namespace x url(u); a{}");
  EXPECT_EQ(delete_rule(f.s.s, 1), GTEXT_CSS_E_INVALID_STATE);
  EXPECT_EQ(delete_rule(f.s.s, 0), GTEXT_CSS_OK) << "an import can";
  EXPECT_EQ(delete_rule(f.s.s, 1), GTEXT_CSS_OK) << "the style rule";
  EXPECT_EQ(delete_rule(f.s.s, 0), GTEXT_CSS_OK) << "and then the namespace";
  Fixture g("@namespace x url(u); @layer a;");
  EXPECT_EQ(delete_rule(g.s.s, 0), GTEXT_CSS_E_INVALID_STATE);
}

TEST(CssomDelete, TheCountsOfTheSheetAndOfTheRulesAboveFollow) {
  Fixture f("@media print{ a{ b{} c{} } d{} }");
  Rule a = path(f.s, {0, 0});
  Rule media = path(f.s, {0});
  EXPECT_EQ(media.r->descendants, 4u);
  EXPECT_EQ(gtext_cssom_sheet_rule_total(f.s.s), 5u);
  ASSERT_EQ(delete_rule(media.r, 0), GTEXT_CSS_OK);
  EXPECT_EQ(media.r->descendants, 1u);
  EXPECT_EQ(gtext_cssom_sheet_rule_total(f.s.s), 2u);
  EXPECT_EQ(a.r->descendants, 2u) << "the deleted rule keeps its own";
  ASSERT_EQ(insert_rule(media.r, "@supports (a:b){ e{} f{} }", 0).st, GTEXT_CSS_OK);
  EXPECT_EQ(media.r->descendants, 4u);
  EXPECT_EQ(gtext_cssom_sheet_rule_total(f.s.s), 5u);
}

TEST(CssomDelete, ADeletedRuleThatIsHeldKeepsNothingAliveAndAFreedOneIsGone) {
  Fixture f("a{} b{}");
  GTEXT_CSSOM_Counters c0;
  gtext_cssom_document_counters(f.doc.d, &c0);
  {
    Rule held = at(f.s.rules(), 0);
    ASSERT_EQ(delete_rule(f.s.s, 0), GTEXT_CSS_OK);
    GTEXT_CSSOM_Counters c1;
    gtext_cssom_document_counters(f.doc.d, &c1);
    EXPECT_EQ(c1.live_objects, c0.live_objects) << "held, so alive";
  }
  GTEXT_CSSOM_Counters c2;
  gtext_cssom_document_counters(f.doc.d, &c2);
  EXPECT_EQ(c2.live_objects, c0.live_objects - 1) << "released, so freed";
}

/* ---- keyframes ----------------------------------------------------------------------------------------------------------------------------------- */

// CSS Animations 1, appendRule: "A valid rule is always appended e.g. even if its key(s) already exists."
TEST(CssomKeyframes, AppendRuleAppendsAValidKeyframeAndIgnoresTheRest) {
  Fixture f("@keyframes k{ from{opacity:0} }");
  Rule k = at(f.s.rules(), 0);
  GTEXT_CSSOM_Outcome o;
  GTEXT_CSSOM_Rule * made = nullptr;
  ASSERT_EQ(gtext_cssom_keyframes_append_rule(k.r, "50% {opacity:1}", 15, &made, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_CHANGED);
  Rule fifty(made);
  EXPECT_EQ(css(fifty.r), "50% { opacity: 1; }");
  const char * const ignored[] = {"bogus", "", "from{} to{}", "@media print{}", "10%", "200% {opacity:1}", "a{opacity:1}"};
  for (const char * t : ignored) {
    GTEXT_CSSOM_Rule * none = reinterpret_cast<GTEXT_CSSOM_Rule *>(1);
    ASSERT_EQ(gtext_cssom_keyframes_append_rule(k.r, t, strlen(t), &none, &o), GTEXT_CSS_OK) << t;
    EXPECT_EQ(o.kind, GTEXT_CSSOM_IGNORED_INVALID_SELECTOR) << t;
    EXPECT_EQ(none, nullptr) << t;
  }
  EXPECT_EQ(gtext_cssom_rule_list_length(gtext_cssom_rule_css_rules(k.r)), 2u);
  ASSERT_EQ(gtext_cssom_keyframes_append_rule(k.r, "from {}", 7, nullptr, &o), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_rule_list_length(gtext_cssom_rule_css_rules(k.r)), 3u) << "a key that is there already is appended";
  EXPECT_EQ(joined(gtext_cssom_rule_css_rules(k.r)), "0% { opacity: 0; } | 50% { opacity: 1; } | 0% { }");
  // the declarations a keyframe does not take are dropped
  const std::string with_animation = "60% {animation-name:x; color:bogus; width:1px}";
  ASSERT_EQ(gtext_cssom_keyframes_append_rule(k.r, with_animation.data(), with_animation.size(), nullptr, &o), GTEXT_CSS_OK);
  EXPECT_EQ(css(at(gtext_cssom_rule_css_rules(k.r), 3).r), "60% { width: 1px; }");
  EXPECT_EQ(gtext_cssom_keyframes_append_rule(nullptr, "from{}", 6, nullptr, &o), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_keyframes_append_rule(k.r, nullptr, 1, nullptr, &o), GTEXT_CSS_E_INVALID);
}

// "The deleteRule method deletes the last declared CSSKeyframeRule matching the specified keyframe selector. If no matching rule exists, the method does nothing." "The match is
// not sensitive to white space around the values in the list." "from and to resolve to 0% and 100%".
TEST(CssomKeyframes, FindRuleAndDeleteRuleMatchTheLastKeyframeOfTheSelectorList) {
  Fixture f("@keyframes k{ from{opacity:0} 10%{opacity:.1} 10%{opacity:.2} 25%,75%{opacity:.5} to{opacity:1} }");
  Rule k = at(f.s.rules(), 0);
  auto find = [&](const std::string & sel) {
    GTEXT_CSSOM_Rule * r = nullptr;
    EXPECT_EQ(gtext_cssom_keyframes_find_rule(k.r, sel.data(), sel.size(), &r), GTEXT_CSS_OK);
    return Rule(r);
  };
  EXPECT_EQ(css(find("10%").r), "10% { opacity: 0.2; }") << "the last one";
  EXPECT_EQ(css(find("  25% ,  75%").r), "25%, 75% { opacity: 0.5; }");
  EXPECT_FALSE(find("75%"));
  EXPECT_FALSE(find("75%, 25%")) << "the number and order of the values must match";
  EXPECT_EQ(css(find("0%").r), "0% { opacity: 0; }");
  EXPECT_EQ(css(find("from").r), "0% { opacity: 0; }");
  EXPECT_EQ(css(find("TO").r), "100% { opacity: 1; }");
  EXPECT_EQ(css(find("100%").r), "100% { opacity: 1; }");
  EXPECT_FALSE(find("bogus"));
  EXPECT_FALSE(find(""));
  GTEXT_CSSOM_Outcome o;
  // deleting "10%" deletes the last declared, then finds the earlier one
  Rule last = find("10%");
  ASSERT_EQ(gtext_cssom_keyframes_delete_rule(k.r, "10%", 3, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_TRUE(gtext_cssom_rule_detached(last.r)) << "the removed keyframe reads parentRule and parentStyleSheet null";
  Rule rest = find("10%");
  EXPECT_EQ(css(rest.r), "10% { opacity: 0.1; }");
  const std::string before = dump(f.s.s);
  ASSERT_EQ(gtext_cssom_keyframes_delete_rule(k.r, "nope", 4, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_UNCHANGED);
  EXPECT_EQ(dump(f.s.s), before);
  ASSERT_EQ(gtext_cssom_keyframes_delete_rule(k.r, "to", 2, &o), GTEXT_CSS_OK);
  EXPECT_EQ(joined(gtext_cssom_rule_css_rules(k.r)), "0% { opacity: 0; } | 10% { opacity: 0.1; } | 25%, 75% { opacity: 0.5; }");
}

TEST(CssomKeyframes, TheKeyframesCallsAreForAKeyframesRule) {
  Fixture f("@media print{}");
  Rule m = at(f.s.rules(), 0);
  GTEXT_CSSOM_Outcome o;
  GTEXT_CSSOM_Rule * r = nullptr;
  EXPECT_EQ(gtext_cssom_keyframes_append_rule(m.r, "from{}", 6, nullptr, &o), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_EQ(gtext_cssom_keyframes_delete_rule(m.r, "from", 4, &o), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_EQ(gtext_cssom_keyframes_find_rule(m.r, "from", 4, &r), GTEXT_CSS_E_UNSUPPORTED);
}

TEST(CssomKeyframes, AKeyframesRuleSendsTheNotificationsOfAnyRule) {
  Fixture f("@keyframes k{}");
  Rule k = at(f.s.rules(), 0);
  GTEXT_CSSOM_Outcome o;
  f.doc.events.clear();
  ASSERT_EQ(gtext_cssom_keyframes_append_rule(k.r, "from{}", 6, nullptr, &o), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_keyframes_delete_rule(k.r, "from", 4, &o), GTEXT_CSS_OK);
  ASSERT_EQ(f.doc.events.size(), 2u);
  EXPECT_EQ(f.doc.events[0].kind, GTEXT_CSSOM_EVENT_RULE_INSERTED);
  EXPECT_EQ(f.doc.events[1].kind, GTEXT_CSSOM_EVENT_RULE_REMOVED);
}

/* ---- the legacy members -------------------------------------------------------------------------------------------------------------------- */

// "The addRule(selector, block, optionalIndex) method ... Let rule be an empty string. Append selector to rule. Append " { " to rule. If block is not empty, append block, followed
// by a space, to rule. Append "}" to rule. Let index be optionalIndex if provided, or the number of CSS rules in the stylesheet otherwise. Call insertRule()."
TEST(CssomLegacy, AddRuleIsInsertRuleOfTheSelectorAndTheBlock) {
  Fixture f("x{}");
  GTEXT_CSSOM_Outcome o;
  ASSERT_EQ(gtext_cssom_sheet_add_rule(f.s.s, "a", 1, "color:red", 9, false, 0, &o), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_sheet_add_rule(f.s.s, "b", 1, "", 0, true, 0, &o), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_sheet_add_rule(f.s.s, "c", 1, "width:1px; color:red", 20, true, 1, &o), GTEXT_CSS_OK);
  EXPECT_EQ(f.text(), "b { } | c { width: 1px; color: red; } | x { } | a { color: red; }");
  EXPECT_EQ(gtext_cssom_sheet_add_rule(f.s.s, "d", 1, "", 0, true, 9, &o), GTEXT_CSS_E_INDEX_SIZE);
  EXPECT_EQ(gtext_cssom_sheet_add_rule(f.s.s, "x >", 3, "", 0, false, 0, &o), GTEXT_CSS_E_SYNTAX);
  EXPECT_EQ(gtext_cssom_sheet_add_rule(f.s.s, nullptr, 1, "", 0, false, 0, &o), GTEXT_CSS_E_INVALID);
  // a block that closes the rule and opens another is what the text says, as in the browsers
  ASSERT_EQ(gtext_cssom_sheet_add_rule(f.s.s, "e{", 2, "color:red", 9, false, 0, &o), GTEXT_CSS_OK);
}

TEST(CssomLegacy, RemoveRuleIsDeleteRule) {
  Fixture f("a{} b{}");
  GTEXT_CSSOM_Outcome o;
  ASSERT_EQ(gtext_cssom_sheet_remove_rule(f.s.s, 0, &o), GTEXT_CSS_OK);
  EXPECT_EQ(f.text(), "b { }");
  EXPECT_EQ(gtext_cssom_sheet_remove_rule(f.s.s, 1, &o), GTEXT_CSS_E_INDEX_SIZE);
}

/* ---- compaction after edits --------------------------------------------------------------------------------------------------------------- */

// A rule's own syntax is what a compaction copies: a rule that was inserted or deleted after the parse has not the children its node has.
TEST(CssomInsertCompact, ACompactionAfterEditsKeepsTheText) {
  Doc doc;
  Sheet s(doc, "@namespace x url(u); x|a{ & b{} color:red; & c{} } @media print{ d{} e{} } @keyframes k{ from{opacity:0} 50%{opacity:.5} } @page{ margin:1px; @top-left{ content:\"x\" } } @font-feature-values F{ @swash{ s:1 } }");
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  Rule a = path(s, {1});
  Rule media = path(s, {2});
  Rule kf = path(s, {3});
  Rule page = path(s, {4});
  ASSERT_EQ(insert_rule(a.r, "width:1px", 1).st, GTEXT_CSS_OK);
  ASSERT_EQ(insert_rule(a.r, "> z{ color:blue }", 0).st, GTEXT_CSS_OK);
  ASSERT_EQ(delete_rule(a.r, 2), GTEXT_CSS_OK);
  ASSERT_EQ(insert_rule(media.r, "@supports (a:b){ f{} }", 1).st, GTEXT_CSS_OK);
  ASSERT_EQ(delete_rule(media.r, 0), GTEXT_CSS_OK);
  GTEXT_CSSOM_Outcome o;
  ASSERT_EQ(gtext_cssom_keyframes_append_rule(kf.r, "75% {opacity:.7}", 16, nullptr, &o), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_keyframes_delete_rule(kf.r, "from", 4, &o), GTEXT_CSS_OK);
  ASSERT_EQ(insert_rule(page.r, "@top-right{ content:\"y\" }", 1).st, GTEXT_CSS_OK);
  ASSERT_EQ(delete_rule(page.r, 0), GTEXT_CSS_OK);
  const std::string before = sheet_text(s.s);
  const uint64_t version = gtext_cssom_sheet_version(s.s);
  const std::vector<uint64_t> ids = {gtext_cssom_rule_id(a.r), gtext_cssom_rule_id(media.r)};
  ASSERT_EQ(gtext_cssom_sheet_compact(s.s), GTEXT_CSS_OK);
  EXPECT_EQ(sheet_text(s.s), before);
  EXPECT_EQ(gtext_cssom_sheet_version(s.s), version);
  EXPECT_EQ(gtext_cssom_rule_id(a.r), ids[0]);
  EXPECT_EQ(gtext_cssom_rule_id(media.r), ids[1]);
  // and the sheet can be edited again, and compacted again
  ASSERT_EQ(insert_rule(a.r, "w{}", 0).st, GTEXT_CSS_OK);
  ASSERT_EQ(delete_rule(s.s, 0), GTEXT_CSS_E_INVALID_STATE);
  EXPECT_EQ(gtext_cssom_sheet_compact(s.s), GTEXT_CSS_OK);
  EXPECT_NE(sheet_text(s.s), before);
}

/* ---- reentrancy, limits, exhaustion --------------------------------------------------------------------------------------------------------- */

// "a mutation from inside a notification callback: E_REENTRANT (a binding must queue)": the first mutation completes.
TEST(CssomInsertLimits, AMutationFromAnObserverIsRefusedAndTheFirstCompletes) {
  Fixture f("a{}");
  GTEXT_CSS_Status inner = GTEXT_CSS_OK, inner_delete = GTEXT_CSS_OK;
  f.doc.inside = [&]() {
    inner = insert_rule(f.s.s, "x{}", 0).st;
    inner_delete = delete_rule(f.s.s, 0);
    return GTEXT_CSS_OK;
  };
  Ins r = insert_rule(f.s.s, "b{}", 1);
  ASSERT_EQ(r.st, GTEXT_CSS_OK);
  EXPECT_EQ(inner, GTEXT_CSS_E_REENTRANT);
  EXPECT_EQ(inner_delete, GTEXT_CSS_E_REENTRANT);
  EXPECT_EQ(f.text(), "a { } | b { }");
}

TEST(CssomInsertLimits, TheRuleLimitIsExactAndAFailedInsertionChangesNothing) {
  Fixture f("a{} b{}", [](GTEXT_CSSOM_Config & c) { c.max_rules = 4; });
  ASSERT_EQ(insert_rule(f.s.s, "c{}", 2).st, GTEXT_CSS_OK);
  ASSERT_EQ(insert_rule(f.s.s, "d{}", 3).st, GTEXT_CSS_OK);
  const std::string before = dump(f.s.s);
  Ins over = insert_rule(f.s.s, "e{}", 4);
  EXPECT_EQ(over.st, GTEXT_CSS_E_LIMIT_RULES);
  EXPECT_EQ(dump(f.s.s), before);
  EXPECT_EQ(insert_rule(f.s.s, "@media print{ e{} }", 0).st, GTEXT_CSS_E_LIMIT_RULES) << "the rules inside count";
  ASSERT_EQ(delete_rule(f.s.s, 0), GTEXT_CSS_OK);
  EXPECT_EQ(insert_rule(f.s.s, "e{}", 0).st, GTEXT_CSS_OK) << "a deletion gives the room back";
}

TEST(CssomInsertLimits, TheNestingLimitCountsTheDepthOfThePlace) {
  Fixture f("a{ b{} }", [](GTEXT_CSSOM_Config & c) { c.max_rule_nesting = 3; });
  Rule a = at(f.s.rules(), 0);
  Rule b = path(f.s, {0, 0});
  EXPECT_EQ(insert_rule(b.r, "c{}", 0).st, GTEXT_CSS_OK) << "depth 3";
  Rule c = path(f.s, {0, 0, 0});
  EXPECT_EQ(insert_rule(c.r, "d{}", 0).st, GTEXT_CSS_E_LIMIT_RULE_NESTING) << "depth 4";
  EXPECT_EQ(insert_rule(a.r, "@media print{ d{ e{} } }", 0).st, GTEXT_CSS_E_LIMIT_RULE_NESTING);
  EXPECT_EQ(insert_rule(a.r, "@media print{ d{} }", 0).st, GTEXT_CSS_OK);
}

TEST(CssomInsertLimits, TheInputLimitAndTheWorkLimitAreStatuses) {
  Fixture f("a{}", [](GTEXT_CSSOM_Config & c) {
    c.max_input_bytes = 16;
    c.max_work = 8;
  });
  EXPECT_EQ(insert_rule(f.s.s, std::string(17, ' ') + "b{}", 0).st, GTEXT_CSS_E_LIMIT_INPUT);
  EXPECT_EQ(insert_rule(f.s.s, "@media print{a{}b{}c{}d{}e{}f{}g{}h{}}", 0).st, GTEXT_CSS_E_LIMIT_INPUT);
  Fixture g("a{}", [](GTEXT_CSSOM_Config & c) { c.max_work = 3; });
  EXPECT_EQ(insert_rule(g.s.s, "@media print{a{}b{}c{}d{}}", 0).st, GTEXT_CSS_E_LIMIT_WORK);
  EXPECT_EQ(gtext_cssom_rule_list_length(g.s.rules()), 1u);
}

// "The counters generation, rule_id, sheet_id and source_id ... a mutation at 2^64 - 1 refuses with GTEXT_CSS_E_LIMIT_CSSOM". The refusal is atomic.
TEST(CssomInsertLimits, TheGenerationCounterAtItsEndRefusesWholeAndAtomically) {
  Fixture f("a{}", [](GTEXT_CSSOM_Config & c) { c.first_generation = UINT64_MAX - 2; });
  ASSERT_EQ(insert_rule(f.s.s, "b{}", 1).st, GTEXT_CSS_OK);
  ASSERT_EQ(insert_rule(f.s.s, "c{}", 2).st, GTEXT_CSS_OK);
  const std::string before = dump(f.s.s);
  EXPECT_EQ(gtext_cssom_document_generation(f.doc.d), UINT64_MAX);
  EXPECT_EQ(insert_rule(f.s.s, "d{}", 3).st, GTEXT_CSS_E_LIMIT_CSSOM);
  EXPECT_EQ(delete_rule(f.s.s, 0), GTEXT_CSS_E_LIMIT_CSSOM);
  EXPECT_EQ(dump(f.s.s), before);
  EXPECT_EQ(gtext_cssom_document_generation(f.doc.d), UINT64_MAX);
}

TEST(CssomInsertLimits, TheLiveObjectLimitIsAStatusAndNothingIsLeft) {
  Fixture f("a{}", [](GTEXT_CSSOM_Config & c) { c.max_live_objects = 4; });
  GTEXT_CSSOM_Counters c0;
  gtext_cssom_document_counters(f.doc.d, &c0);
  ASSERT_GE(c0.live_objects, 2u);
  Ins r;
  size_t i = 0;
  for (; i < 10; i++) {
    r = insert_rule(f.s.s, "x{}", 0);
    if (r.st != GTEXT_CSS_OK) {
      break;
    }
  }
  EXPECT_EQ(r.st, GTEXT_CSS_E_LIMIT_CSSOM);
  EXPECT_LT(i, 10u);
  const std::string before = dump(f.s.s);
  GTEXT_CSSOM_Counters c1;
  gtext_cssom_document_counters(f.doc.d, &c1);
  EXPECT_EQ(insert_rule(f.s.s, "x{}", 0).st, GTEXT_CSS_E_LIMIT_CSSOM);
  GTEXT_CSSOM_Counters c2;
  gtext_cssom_document_counters(f.doc.d, &c2);
  EXPECT_EQ(c2.live_objects, c1.live_objects);
  EXPECT_EQ(c2.live_bytes, c1.live_bytes);
  EXPECT_EQ(dump(f.s.s), before);
}

TEST(CssomInsertLimits, ARuleHeldAfterItsSheetIsReleasedStillTakesCalls) {
  Rule held;
  {
    Doc doc;
    Sheet s(doc, "@media print{ a{} }");
    held = at(s.rules(), 0);
    doc.release();
  }
  Ins r = insert_rule(held.r, "b{}", 0);
  EXPECT_EQ(r.st, GTEXT_CSS_OK) << "the core is alive while an object is held";
  EXPECT_EQ(css(held.r), "@media print {\n  b { }\n  a { }\n}");
}

} // namespace
