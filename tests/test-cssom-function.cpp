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

// CSS Functions and Mixins 1 defines how a mixin is applied, and nothing here applies one yet: the rules are recognized and inactive. The body of a mixin is not a rule of the sheet's cascade
// ("div" below is not styled by the rule inside the mixin), an @apply changes no style, and a mixin with an @apply or @contents inside it does not break the sheet around it.
TEST(CssomFunction, AMixinAndAnApplyChangeNoStyleYet) {
  Cs m(page("@mixin --m(--c <color>: red) { div { color: red } & { color: blue } @contents; } #a { color: green; @apply --m(blue) { color: pink } } div { margin-top: 3px } #b { @apply --m; }", "<div id=a>x</div><div id=b>y</div>"));
  EXPECT_EQ(m.value("#a", "color"), "rgb(0, 128, 0)");
  EXPECT_EQ(m.value("#a", "margin-top"), "3px") << "the rule inside the mixin is no rule of the sheet, the one after it is";
  EXPECT_EQ(m.value("#b", "color"), "canvastext");
  EXPECT_EQ(m.value("#b", "margin-top"), "3px");
}


}  // namespace
