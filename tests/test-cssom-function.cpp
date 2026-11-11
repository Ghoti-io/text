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
 * `@function` rules of a CSSOM document's style sheets, defined in the document's computed styles (css_cssom_computed.h, css_custom_function.h). The served text is CSS Functions and
 * Mixins 1 (the Editor's Draft pinned in tools/css/DRAFTS_COMMIT): "The name of a @function rule is a tree-scoped name. If more than one @function exists for a given name, then the rule
 * in the stronger cascade layer wins, and rules defined later win within the same layer.", and "Conditional group rules within @function are processed as normal".
 */

#include <string>

#include <gtest/gtest.h>

#include "cssom_computed_test_support.h"

using namespace cc;

namespace {

std::string page(const std::string & sheet, const std::string & body = "<div id=a><p id=b>x</p></div>") {
  return "<!DOCTYPE html><html><head><style id=s>" + sheet + "</style></head><body>" + body + "</body></html>";
}

const char * kDouble = "@function --double(--x) { result: calc(var(--x) * 2); }";

TEST(CssomFunction, ACallInAStyleRuleIsEvaluatedInTheDocumentsStyles) {
  Cs m(page(std::string(kDouble) + "#a { width: --double(3px); --r: --double(4px) }"));
  EXPECT_EQ(m.value("#a", "width"), "6px");
  EXPECT_EQ(m.value("#a", "--r"), "calc(4px * 2)");
}

TEST(CssomFunction, ACallSeesTheCustomPropertiesOfTheElementItIsOn) {
  Cs m(page("@function --f() { result: calc(var(--z) * 2); } #a { --z: 5px } #b { width: --f() }"));
  EXPECT_EQ(m.value("#b", "width"), "10px") << "--z is inherited from #a";
}

TEST(CssomFunction, AFunctionNoRuleDefinesIsGuaranteedInvalid) {
  Cs m(page("#a { width: --missing(3px); height: 7px }"));
  EXPECT_EQ(m.value("#a", "width"), "auto");
  EXPECT_EQ(m.value("#a", "height"), "7px");
}

TEST(CssomFunction, ARuleInALaterSheetWinsOverAnEarlierOne) {
  Cs m("<!DOCTYPE html><html><head><style>@function --f() { result: 1px; }</style><style>@function --f() { result: 2px; } #b { width: --f() }</style></head><body><p id=b>x</p></body></html>");
  EXPECT_EQ(m.value("#b", "width"), "2px");
  Cs n(page("@function --f() { result: 1px; } @function --f() { result: 2px; } #b { width: --f() }"));
  EXPECT_EQ(n.value("#b", "width"), "2px");
}

TEST(CssomFunction, TheStrongerLayerWins) {
  // "the rule in the stronger cascade layer wins, and rules defined later win within the same layer": an unlayered rule is stronger than a layered one that comes after it.
  Cs m(page("@function --f() { result: 1px; } @layer x { @function --f() { result: 2px; } } #b { width: --f() }"));
  EXPECT_EQ(m.value("#b", "width"), "1px");
  Cs n(page("@layer a, b; @layer b { @function --f() { result: 2px; } } @layer a { @function --f() { result: 1px; } } #b { width: --f() }"));
  EXPECT_EQ(n.value("#b", "width"), "2px");
}

TEST(CssomFunction, AFunctionInAnActiveMediaRuleIsDefinedAndOneInAnInactiveRuleIsNot) {
  Cs m(page("@media (min-width: 1px) { @function --a() { result: 1px; } } @media (max-width: 0px) { @function --b() { result: 2px; } } #b { width: --a(); height: --b() }"));
  EXPECT_EQ(m.value("#b", "width"), "1px");
  EXPECT_EQ(m.value("#b", "height"), "auto");
}

TEST(CssomFunction, AConditionalRuleOfTheBodyFollowsItsCondition) {
  // The draft's example: `result: 16px; @media (width > 1000px) { result: 20px; }` is 20px if the condition is true, and 16px otherwise. The document's viewport is 1280 wide.
  Cs m(page("@function --a() { result: 16px; @media (width > 1000px) { result: 20px; } } @function --b() { result: 16px; @media (width > 2000px) { result: 20px; } }"
            "@function --c() { @media (width > 1000px) { result: 20px; } result: 16px; } #b { width: --a(); height: --b(); margin-left: --c() }"));
  EXPECT_EQ(m.value("#b", "width"), "20px");
  EXPECT_EQ(m.value("#b", "height"), "16px");
  EXPECT_EQ(m.value("#b", "margin-left"), "16px") << "there is no early return";
}

TEST(CssomFunction, ALocalVariableInAConditionalRuleOfTheBody) {
  // The draft's example: `--size: 16px; @media (width > 1000px) { --size: 20px; } result: var(--size);`.
  Cs m(page("@function --f() { --size: 16px; @media (width > 1000px) { --size: 20px; } result: var(--size); } #b { width: --f() }"));
  EXPECT_EQ(m.value("#b", "width"), "20px");
}

/* An `@container` rule of a body needs a conditions object with a container interface, which the pinned pages of css-mixins/functions (function-container-*.html) give the document
   and the script gate scores; a bare document answers false for the rule, as it does for a style rule's. */

TEST(CssomFunction, ARuleInsertedIntoASheetDefinesTheFunctionAtTheNextRead) {
  Cs m(page("#b { width: --double(3px) }"));
  EXPECT_EQ(m.value("#b", "width"), "auto");
  GTEXT_CSSOM_Sheet * sh = m.sheet(m.el("#s"));
  const std::string rule = kDouble;
  ASSERT_EQ(gtext_cssom_sheet_insert_rule(sh, rule.data(), rule.size(), 0, nullptr, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(m.value("#b", "width"), "6px");
  ASSERT_EQ(gtext_cssom_sheet_delete_rule(sh, 0, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(m.value("#b", "width"), "auto") << "the rule is gone, the call is the guaranteed-invalid value";
  gtext_cssom_sheet_release(sh);
}

TEST(CssomFunction, ADeclaredComputedStyleReadBeforeReadsAgainWhenTheBodyChanges) {
  Cs m(page(std::string(kDouble) + "#b { width: --double(3px) }"));
  Decl before = m.computed("#b");
  EXPECT_EQ(get(before.d, "width"), "6px");
  GTEXT_CSSOM_Sheet * sh = m.sheet(m.el("#s"));
  const std::string rule = "@function --double(--x) { result: calc(var(--x) * 3); }";
  ASSERT_EQ(gtext_cssom_sheet_insert_rule(sh, rule.data(), rule.size(), 1, nullptr, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(get(before.d, "width"), "9px") << "a later rule of the same name wins";
  gtext_cssom_sheet_release(sh);
}

TEST(CssomFunction, ASheetOfStyleRulesAloneBuildsNoFunctionContextAgain) {
  Cs m(page("#a { color: red }"));
  EXPECT_EQ(m.value("#a", "color"), "rgb(255, 0, 0)");
  EXPECT_EQ(m.dc().property_context_rebuilds, 0u);
  GTEXT_CSSOM_Sheet * sh = m.sheet(m.el("#s"));
  ASSERT_EQ(gtext_cssom_sheet_insert_rule(sh, "p{color:green}", 14, 0, nullptr, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(m.value("#a", "color"), "rgb(255, 0, 0)");
  EXPECT_EQ(m.dc().property_context_rebuilds, 0u) << "an unrelated rule changes no function";
  gtext_cssom_sheet_release(sh);
}

// The body of a mixin is not a rule of the sheet's cascade until it is applied ("div" below is not styled by the rule inside the mixin, which is nested in what applies it), and an @apply
// substitutes it in place: here the nested `& { color: blue }` after the rule's own `color: green`, then the contents block (`color: pink`) in the place of @contents.
TEST(CssomFunction, AMixinAndAnApplyChangeTheStyleOfTheRuleThatAppliesIt) {
  Cs m(page("@mixin --m(--c <color>: red) { div { color: red } & { color: blue } @contents; } #a { color: green; @apply --m(blue) { color: pink } } div { margin-top: 3px } #b { @apply --m; }", "<div id=a>x</div><div id=b>y</div>"));
  EXPECT_EQ(m.value("#a", "color"), "rgb(255, 192, 203)");
  EXPECT_EQ(m.value("#a", "margin-top"), "3px") << "the rule inside the mixin is no rule of the sheet, the one after it is";
  EXPECT_EQ(m.value("#b", "color"), "rgb(0, 0, 255)");
  EXPECT_EQ(m.value("#b", "margin-top"), "3px");
}


// ---- the function set of a document (gtext_cssom_document_function_rules) ------------------------------------------------------------------------------------------------------

std::string text_of(const GTEXT_CSS_String & s) {
  return std::string(s.data, s.length);
}

/// "name(params) returns T layer sheet" of every function of the document's set, in the order the set holds them.
std::vector<std::string> functions_of(GTEXT_CSSOM_Document * d, bool * applied = nullptr, bool * none = nullptr) {
  const GTEXT_CSS_Function_Set * set = nullptr;
  EXPECT_EQ(gtext_cssom_document_function_rules(d, &set, applied), GTEXT_CSS_OK);
  if (none) {
    *none = set == nullptr;
  }
  std::vector<std::string> out;
  for (size_t i = 0; i < gtext_css_function_count(set); i++) {
    GTEXT_CSS_Function_Info info;
    EXPECT_TRUE(gtext_css_function_at(set, i, &info));
    std::string line = text_of(info.name) + "(";
    for (size_t k = 0; k < info.parameter_count; k++) {
      GTEXT_CSS_Function_Parameter p;
      EXPECT_TRUE(gtext_css_function_parameter(set, info.name.data, info.name.length, k, &p));
      line += (k ? ", " : "") + text_of(p.name) + (p.has_type ? " " + text_of(p.type) : "") + (p.has_default ? ": " + text_of(p.default_value) : "");
    }
    line += ")";
    if (info.has_returns) {
      line += " returns " + text_of(info.returns);
    }
    out.push_back(line + (info.has_result ? " result" : " no-result"));
  }
  return out;
}

TEST(CssomFunctionSet, TheDocumentHandsOutTheFunctionsItsSheetsDefineAndTheOneThatWinsForAName) {
  Cs m(page("@function --b(--x <length>: 4px, --y) returns <length> { result: var(--x) } @function --a() { --l: 1px; } @function --b() { result: 3px } @function --c(--bad bad bad) { result: 1px } #b { width: --b() }"));
  bool applied = false, none = true;
  EXPECT_EQ(functions_of(m.doc->d, &applied, &none), (std::vector<std::string>{"--a() no-result", "--b() result"})) << "in the order of the names; the later of two rules of a name is the function, a rule that is not valid defines none";
  EXPECT_TRUE(applied);
  EXPECT_FALSE(none);
  const GTEXT_CSS_Function_Set * set = nullptr;
  ASSERT_EQ(gtext_cssom_document_function_rules(m.doc->d, &set, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Function_Info info;
  EXPECT_FALSE(gtext_css_function_find(set, "--c", 3, &info));
  EXPECT_FALSE(gtext_css_function_find(set, "--B", 3, &info)) << "names are case-sensitive";
  ASSERT_TRUE(gtext_css_function_find(set, "--b", 3, &info));
  EXPECT_EQ(info.parameter_count, 0u);
  EXPECT_EQ(gtext_css_function_diagnostic_total(set), 1u) << "the earlier --b is superseded; --c is not a rule at all (the parse of the sheet drops it), so it raises none";
  EXPECT_EQ(m.value("#b", "width"), "3px") << "the set is what the document evaluates with";
}

TEST(CssomFunctionSet, TheParametersAreThoseOfTheRuleThatWon) {
  Cs m(page("@function --f(--x <length>: 4px, --y, --z type(<number> | auto): auto) returns <length> { result: var(--x) }"));
  EXPECT_EQ(functions_of(m.doc->d), (std::vector<std::string>{"--f(--x <length>: 4px, --y, --z <number> | auto: auto) returns <length> result"}));
}

TEST(CssomFunctionSet, ARuleOfAnInactiveConditionalRuleDefinesNothingAndNoRuleAtAllIsANullSet) {
  Cs m(page("@media (max-width: 0px) { @function --a() { result: 1px; } } @media (min-width: 1px) { @function --b() { result: 1px; } } @layer l { @function --c() { result: 1px; } } #a { color: red }"));
  EXPECT_EQ(functions_of(m.doc->d), (std::vector<std::string>{"--b() result", "--c() result"}));
  Cs none(page("#a { color: red }"));
  bool is_null = false;
  EXPECT_TRUE(functions_of(none.doc->d, nullptr, &is_null).empty());
  EXPECT_TRUE(is_null) << "a document with no @function rule has no set";
}

TEST(CssomFunctionSet, ASheetChangeIsSeenByTheNextCallAndADisabledSheetDefinesNothing) {
  Cs m(page("@function --a() { result: 1px; }"));
  EXPECT_EQ(functions_of(m.doc->d), (std::vector<std::string>{"--a() result"}));
  GTEXT_CSSOM_Sheet * sh = m.sheet(m.el("#s"));
  const std::string rule = "@function --z(--q) { result: 2px; }";
  ASSERT_EQ(gtext_cssom_sheet_insert_rule(sh, rule.data(), rule.size(), 0, nullptr, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(functions_of(m.doc->d), (std::vector<std::string>{"--a() result", "--z(--q) result"}));
  ASSERT_EQ(gtext_cssom_sheet_delete_rule(sh, 1, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(functions_of(m.doc->d), (std::vector<std::string>{"--z(--q) result"}));
  gtext_cssom_sheet_release(sh);
}

TEST(CssomFunctionSet, ACallerSuppliedComputeContextKeepsItsOwnFunctionsAndTheRulesAreReportedNotApplied) {
  GTEXT_CSS_Compute_Options co = gtext_css_compute_options_default();
  GTEXT_CSS_Compute_Context * ctx = nullptr;
  const GTEXT_CSS_Environment env = gtext_css_environment_default();
  GTEXT_CSS_Compute_Result cr;
  std::memset(&cr, 0, sizeof cr);
  ASSERT_EQ(gtext_css_compute_context_new(R(), &env, &co, &ctx, &cr), GTEXT_CSS_OK);
  Cs m(page("@function --a() { result: 1px; }"), [&](GTEXT_CSSOM_Config & c) { c.compute_context = ctx; });
  bool applied = true;
  EXPECT_EQ(functions_of(m.doc->d, &applied), (std::vector<std::string>{"--a() result"}));
  EXPECT_FALSE(applied);
  EXPECT_EQ(gtext_cssom_document_function_rules(nullptr, nullptr, nullptr), GTEXT_CSS_E_INVALID);
  const GTEXT_CSS_Function_Set * set = nullptr;
  EXPECT_EQ(gtext_cssom_document_function_rules(nullptr, &set, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_document_function_rules(m.doc->d, nullptr, nullptr), GTEXT_CSS_E_INVALID);
}

}  // namespace
