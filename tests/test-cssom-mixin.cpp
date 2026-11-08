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
 * `@mixin`, `@apply` and `@contents` as rules of a style sheet: what is a rule and where, and what CSSOM says of each (css_cssom_rule.h). The served text is CSS Functions and Mixins 1
 * (the Editor's Draft pinned in tools/css/DRAFTS_COMMIT), in the sections "Defining Mixins", "The @contents Rule", "The @apply Rule" and "CSSOM"; every test quotes the sentence it checks.
 *
 * **`@result` is not a rule of the draft.** Its examples and the section on defining mixins write a mixin's body as a style rule's ("The body of a mixin is parsed as a nested declarations
 * rule; it accepts properties, conditional group rules, and nested style rules, just like an ordinary style rule would"), the grammar and the IDL of the pinned data have no `@result`, the
 * CSSOM section has no interface for it, and the one section that still writes it opens "Issue: TODO rewrite to use the new @private concepts". So a `@result` in a body is an at-rule the
 * body does not take, and it is dropped as any unknown at-rule is. 22 of the 27 pinned web-platform-tests pages of the mixins use it all the same; they are evidence of an older draft.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cssom_test_support.h"
#include "markup_test_support.h"

using namespace cot;

namespace {

/// The kinds of the rules of a sheet, with the rules inside a rule between brackets.
std::string shape_of(const std::string & css) {
  Doc doc;
  Sheet s(doc, css);
  EXPECT_EQ(s.status, GTEXT_CSS_OK) << css;
  return shape(s);
}

/// The text of the first rule of a sheet.
std::string first(const std::string & css) {
  Doc doc;
  Sheet s(doc, css);
  EXPECT_EQ(s.status, GTEXT_CSS_OK) << css;
  Rule r = at(s.rules(), 0);
  return r ? ::css(r.r) : "<no rule>";
}

std::string iface(const Rule & r) {
  return gtext_cssom_rule_interface_name(r.r);
}

/* ---- the rules ---------------------------------------------------------------------------------------------------------------------------------- */

// "The @mixin rule defines a mixin, and consists of a name, a list of mixin parameters, and a mixin body." CSSOM: "The CSSMixinRule interface represents a @mixin rule."
TEST(CssomMixin, AMixinIsARuleOfItsOwnKind) {
  Doc doc;
  Sheet s(doc, "@mixin --m() { color: green }");
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_rule_list_length(s.rules()), 1u);
  Rule m = at(s.rules(), 0);
  EXPECT_STREQ(gtext_cssom_rule_kind_name(gtext_cssom_rule_kind(m.r)), "mixin");
  EXPECT_EQ(iface(m), "CSSMixinRule");
  EXPECT_EQ(gtext_cssom_rule_type(m.r), 0u);
  EXPECT_NE(gtext_cssom_rule_css_rules(m.r), nullptr) << "CSSMixinRule is a CSSGroupingRule";
}

// "While declarations may be specified directly within a @mixin rule, they are not represented as such in the CSSOM. Instead, consecutive segments of declarations appear as if wrapped in
// CSSNestedDeclarations rules."
TEST(CssomMixin, TheDeclarationsOfABodyAreNestedDeclarationsRules) {
  EXPECT_EQ(shape_of("@mixin --m() { color: green }"), "mixin[nested-declarations]");
  EXPECT_EQ(shape_of("@mixin --m() { color: green; & b { color: red } width: 1px }"), "mixin[nested-declarations,style,nested-declarations]");
  EXPECT_EQ(shape_of("@mixin --m() { }"), "mixin");
}

// "In addition to everything normally valid in a style rule" ... "it accepts properties, conditional group rules, and nested style rules, just like an ordinary style rule would. In
// particular, further mixins can be invoked."
TEST(CssomMixin, ABodyTakesWhatAStyleRuleTakes) {
  EXPECT_EQ(shape_of("@mixin --m() { @media (width) { color: red } }"), "mixin[media[nested-declarations]]");
  EXPECT_EQ(shape_of("@mixin --m() { @supports (width: 0) { & b {} } }"), "mixin[supports[style]]");
  EXPECT_EQ(shape_of("@mixin --m() { @container (width) { color: red } }"), "mixin[container[nested-declarations]]");
  EXPECT_EQ(shape_of("@mixin --m() { @starting-style { color: red } }"), "mixin[starting-style[nested-declarations]]");
  EXPECT_EQ(shape_of("@mixin --m() { @scope (.a) { b {} } }"), "mixin[scope[style]]");
  EXPECT_EQ(shape_of("@mixin --m() { @apply --other; }"), "mixin[apply]");
  EXPECT_EQ(shape_of("@mixin --m() { & > p { color: red } > b { color: red } }"), "mixin[style,style]") << "a nested selector is a relative one";
}

// The same sentence, the other way: a rule the body does not take is invalid and dropped. `@result` is none of them (see the file comment); `@font-face` is not valid inside a style rule.
TEST(CssomMixin, ARuleAStyleRuleDoesNotTakeIsNotInABody) {
  EXPECT_EQ(shape_of("@mixin --m() { @result { color: green } }"), "mixin") << "@result is no rule of the draft";
  EXPECT_EQ(shape_of("@mixin --m() { @result { div {} } }"), "mixin");
  EXPECT_EQ(shape_of("@mixin --m() { @font-face { font-family: x } }"), "mixin");
  EXPECT_EQ(shape_of("@mixin --m() { @import url(x); }"), "mixin");
}

// "@layer is not valid within a mixin body. The contents of any @layer within a mixin body are dropped when the mixin is applied." Chromium and Firefox implement none of it; the pinned
// page css/css-mixins/mixins/mixin-parsing.html asserts it for the three forms, at any depth, and that the rule around the @layer survives.
TEST(CssomMixin, ALayerIsNotValidInABody) {
  EXPECT_EQ(shape_of("@mixin --m() { @layer bar; }"), "mixin");
  EXPECT_EQ(shape_of("@mixin --m() { @layer bar {} }"), "mixin");
  EXPECT_EQ(shape_of("@mixin --m() { @layer {} }"), "mixin");
  EXPECT_EQ(shape_of("@mixin --m() { @layer bar { color: red } }"), "mixin");
}

TEST(CssomMixin, ALayerIsDroppedAtAnyDepthAndWhatIsAroundItStays) {
  EXPECT_EQ(first("@mixin --m() { div { @layer bar; color: green } }"), "@mixin --m { & div { color: green; } }");
  EXPECT_EQ(first("@mixin --m() { @media (width) { @layer bar; div { color: green } } }"), "@mixin --m { @media (width) {\n  & div { color: green; }\n} }");
  EXPECT_EQ(first("@mixin --m() { @supports (width: 0) { @layer bar {} div { color: green } } }"), "@mixin --m { @supports (width: 0) {\n  & div { color: green; }\n} }");
  EXPECT_EQ(first("@mixin --m() { @container (width) { @layer bar; div { color: green } } }"), "@mixin --m { @container (width) {\n  & div { color: green; }\n} }");
  EXPECT_EQ(first("@mixin --m() { @scope (.foo) { @layer bar; div { color: green } } }"), "@mixin --m { @scope (& .foo) {\n  div { color: green; }\n} }");
  // outside a mixin an @layer in a style rule is what it was
  EXPECT_EQ(shape_of("a { @layer bar { color: red } }"), "style[layer[nested-declarations]]");
}

// "A @mixin rule cannot be a nested group rule; it is invalid within the body of a style rule." Nor in a mixin, whose body is read as one.
TEST(CssomMixin, AMixinIsInvalidInAStyleRuleAndInAMixin) {
  EXPECT_EQ(shape_of("div { @mixin --m() {} }"), "style");
  EXPECT_EQ(shape_of("div { @media print { @mixin --m() {} } }"), "style[media]");
  EXPECT_EQ(shape_of("@mixin --a() { @mixin --b() {} }"), "mixin");
}

// The pinned page: "@mixin is valid wherever @keyframes is valid." (the places of the ledger: the top of a sheet and a group rule)
TEST(CssomMixin, AMixinIsValidWhereAKeyframesRuleIs) {
  EXPECT_EQ(shape_of("@mixin --m() {}"), "mixin");
  EXPECT_EQ(shape_of("@media (width) { @keyframes --k {} @mixin --m() {} }"), "media[keyframes,mixin]");
  EXPECT_EQ(shape_of("@container (width) { @keyframes --k {} @mixin --m() {} }"), "container[keyframes,mixin]");
  EXPECT_EQ(shape_of("@supports (width: 0) { @keyframes --k {} @mixin --m() {} }"), "supports[keyframes,mixin]");
  EXPECT_EQ(shape_of("@layer foo { @keyframes --k {} @mixin --m() {} }"), "layer[keyframes,mixin]");
}

/* ---- the prelude of a mixin ---------------------------------------------------------------------------------------------------------------------- */

// "The <function-token> or <ident-token> production must start with two dashes (U+002D HYPHEN-MINUS), similar to <dashed-ident>, or else the definition is invalid." and "If an <ident-token>
// is used, it is equivalent to a <function-token> followed by an empty parameter list."
TEST(CssomMixin, TheNameBeginsWithTwoDashesAndTheParenthesesAreOptional) {
  EXPECT_EQ(shape_of("@mixin m() {}"), "");
  EXPECT_EQ(shape_of("@mixin m {}"), "");
  EXPECT_EQ(shape_of("@mixin -m() {}"), "");
  EXPECT_EQ(shape_of("@mixin --m {}"), "mixin");
  EXPECT_EQ(shape_of("@mixin --m() {}"), "mixin");
  EXPECT_EQ(shape_of("@mixin --m(--a, --b) {}"), "mixin");
  EXPECT_EQ(shape_of("@mixin --m --n {}"), "");
  EXPECT_EQ(shape_of("@mixin --m() --n {}"), "");
  EXPECT_EQ(shape_of("@mixin {}"), "");
  EXPECT_EQ(shape_of("@mixin --m();"), "") << "a mixin has a block";
}

// "(Identical to @function, save that it lacks a return type, and that the parameter list and enclosing parentheses are optional.)" There is no `returns`.
TEST(CssomMixin, AMixinHasNoReturnType) {
  EXPECT_EQ(shape_of("@mixin --m() returns <length> {}"), "");
}

// "If the mixin parameters contain the same <custom-property-name> more than once, then the @mixin rule is invalid."
TEST(CssomMixin, AParameterNameTwiceMakesTheRuleInvalid) {
  EXPECT_EQ(shape_of("@mixin --m(--a, --a) {}"), "");
  EXPECT_EQ(shape_of("@mixin --m(--a <length>, --a <color>) {}"), "");
  EXPECT_EQ(shape_of("@mixin --m(--a, --b) {}"), "mixin");
}

// "If a mixin parameter has both a default value and a parameter type then the default value must parse successfully according to that parameter type's syntax. Otherwise, the @mixin rule is
// invalid."
TEST(CssomMixin, ADefaultMustBeAValueOfTheParametersType) {
  EXPECT_EQ(shape_of("@mixin --m(--a <length>: red) {}"), "");
  EXPECT_EQ(shape_of("@mixin --m(--a <length>: 1px) {}"), "mixin");
  EXPECT_EQ(shape_of("@mixin --m(--a: red) {}"), "mixin") << "no type, no check";
  EXPECT_EQ(shape_of("@mixin --m(--a type(<length> | <color>): red) {}"), "mixin");
  EXPECT_EQ(shape_of("@mixin --m(--a <length>: var(--x)) {}"), "mixin") << "a substitution is checked when it is substituted";
}

TEST(CssomMixin, AParameterIsANameAnOptionalTypeAndAnOptionalDefault) {
  EXPECT_EQ(shape_of("@mixin --m(a) {}"), "");
  EXPECT_EQ(shape_of("@mixin --m(--a <nonsense>) {}"), "");
  EXPECT_EQ(shape_of("@mixin --m(--a, ) {}"), "mixin") << "Mixins 1 (pinned 2026-11-07): \"@mixin [<function-token> <function-parameter>#? ,? ) | <ident-token> ]\", a single trailing comma";
  EXPECT_EQ(shape_of("@mixin --m(--a,, ) {}"), "");
  EXPECT_EQ(shape_of("@mixin --m(, ) {}"), "");
  EXPECT_EQ(shape_of("@mixin --m(--a <length>+: 1px 2px) {}"), "mixin");
}

/* ---- @contents ----------------------------------------------------------------------------------------------------------------------------------- */

// "In addition to everything normally valid in a style rule, the mixin body can contain one or more @contents rules" and "Outside of a mixin body, the @contents rule is invalid and ignored."
TEST(CssomMixin, AContentsRuleIsValidOnlyInAMixinBody) {
  EXPECT_EQ(shape_of("@mixin --m() { @contents; }"), "mixin[contents]");
  EXPECT_EQ(shape_of("@mixin --m() { @contents { color: red } }"), "mixin[contents[nested-declarations]]");
  EXPECT_EQ(shape_of("@mixin --m() { @contents; @contents; }"), "mixin[contents,contents]") << "\"one or more\"";
  EXPECT_EQ(shape_of("div { @contents; }"), "style");
  EXPECT_EQ(shape_of("div { @contents { color: red } }"), "style");
  EXPECT_EQ(shape_of("@contents;"), "");
  EXPECT_EQ(shape_of("@media print { @contents; }"), "media");
  EXPECT_EQ(shape_of("@media print { div { @contents; } }"), "media[style]");
}

// The example of the draft: `@mixin --one-column() { @media (width <= 800px) { @contents; } }`; and a style rule inside the body is in the body all the same.
TEST(CssomMixin, AContentsRuleIsValidAtAnyDepthOfTheBody) {
  EXPECT_EQ(shape_of("@mixin --one-column() { @media (width <= 800px) { @contents; } }"), "mixin[media[contents]]");
  EXPECT_EQ(shape_of("@mixin --m() { & .a { @contents { color: blue } } }"), "mixin[style[contents[nested-declarations]]]");
  EXPECT_EQ(shape_of("@mixin --m() { @supports (width: 0) { @media print { @contents } } }"), "mixin[supports[media[contents]]]") << "the end of a block ends the statement";
}

// "That is, it is either an empty statement ended immediately by a semicolon, or a fallback block treated as a nested declarations rule." CSSOM: CSSContentsBlockRule and
// CSSContentsStatementRule.
TEST(CssomMixin, AContentsRuleIsAStatementOrABlock) {
  Doc doc;
  Sheet s(doc, "@mixin --m() { @contents; @contents { color: red } @contents {} }");
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  Rule a = path(s, {0, 0});
  Rule b = path(s, {0, 1});
  Rule c = path(s, {0, 2});
  ASSERT_TRUE(a && b && c);
  EXPECT_EQ(iface(a), "CSSContentsStatementRule");
  EXPECT_EQ(iface(b), "CSSContentsBlockRule");
  EXPECT_EQ(iface(c), "CSSContentsBlockRule");
  EXPECT_FALSE(gtext_cssom_rule_has_block(a.r));
  EXPECT_TRUE(gtext_cssom_rule_has_block(b.r));
  EXPECT_EQ(gtext_cssom_rule_css_rules(a.r), nullptr) << "a statement has no rules";
  ASSERT_NE(gtext_cssom_rule_css_rules(b.r), nullptr);
  EXPECT_EQ(gtext_cssom_rule_list_length(gtext_cssom_rule_css_rules(b.r)), 1u);
  EXPECT_EQ(gtext_cssom_rule_list_length(gtext_cssom_rule_css_rules(c.r)), 0u);
}

TEST(CssomMixin, AContentsRuleTakesNoPrelude) {
  EXPECT_EQ(shape_of("@mixin --m() { @contents foo; }"), "mixin");
  EXPECT_EQ(shape_of("@mixin --m() { @contents foo { color: red } }"), "mixin");
}

/* ---- @apply -------------------------------------------------------------------------------------------------------------------------------------- */

// "The @apply rule is only valid in the body of a style rule or nested group rule; using it in any other context causes it to be invalid and ignored."
TEST(CssomMixin, AnApplyRuleIsValidInTheBodyOfAStyleRuleOrANestedGroupRule) {
  EXPECT_EQ(shape_of("div { @apply --m; }"), "style[apply]");
  EXPECT_EQ(shape_of("div { color: red; @apply --m; width: 1px }"), "style[apply,nested-declarations]");
  EXPECT_EQ(shape_of("div { @media print { @apply --m; } }"), "style[media[apply]]");
  EXPECT_EQ(shape_of("div { & b { @apply --m; } }"), "style[style[apply]]");
  EXPECT_EQ(shape_of("@apply --m;"), "");
  EXPECT_EQ(shape_of("@apply --m();"), "");
  EXPECT_EQ(shape_of("@media print { @apply --m; }"), "media");
  EXPECT_EQ(shape_of("@layer x { @apply --m; }"), "layer");
  EXPECT_EQ(shape_of("@font-face { @apply --m; }"), "font-face");
  EXPECT_EQ(shape_of("@keyframes --k { @apply --m; }"), "keyframes");
}

// The examples of the draft: `@apply --one;`, `@apply --two(blue);`, `@apply --three {color: red;}`, `@apply --four(blue) {color: red;}`.
TEST(CssomMixin, AnApplyRuleIsAStatementOrABlockAndHasArguments) {
  Doc doc;
  Sheet s(doc, ".foo { @apply --one; @apply --two(blue); @apply --three {color: red;} @apply --four(blue) {color: red;} @apply --five {} }");
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_rule_list_length(gtext_cssom_rule_css_rules(at(s.rules(), 0).r)), 5u);
  const char * want[] = {"CSSApplyStatementRule", "CSSApplyStatementRule", "CSSApplyBlockRule", "CSSApplyBlockRule", "CSSApplyBlockRule"};
  for (size_t i = 0; i < 5; i++) {
    Rule r = path(s, {0, i});
    ASSERT_TRUE(r);
    EXPECT_STREQ(gtext_cssom_rule_kind_name(gtext_cssom_rule_kind(r.r)), "apply");
    EXPECT_EQ(iface(r), want[i]) << i;
  }
  EXPECT_EQ(gtext_cssom_rule_css_rules(path(s, {0, 0}).r), nullptr);
  EXPECT_EQ(gtext_cssom_rule_list_length(gtext_cssom_rule_css_rules(path(s, {0, 2}).r)), 1u) << "the contents block is the block's rules";
  EXPECT_EQ(gtext_cssom_rule_list_length(gtext_cssom_rule_css_rules(path(s, {0, 4}).r)), 0u);
}

// `<<@apply>> = @apply [ <<dashed-ident>> | <<dashed-function>> ] [ { <<declaration-list>> } ]?;` and `<dashed-function> = --*( <declaration-value>#? )`.
TEST(CssomMixin, TheNameOfAnApplyRuleIsADashedIdentOrADashedFunction) {
  EXPECT_EQ(shape_of("div { @apply m; }"), "style");
  EXPECT_EQ(shape_of("div { @apply m(); }"), "style");
  EXPECT_EQ(shape_of("div { @apply -m; }"), "style");
  EXPECT_EQ(shape_of("div { @apply; }"), "style");
  EXPECT_EQ(shape_of("div { @apply --a --b; }"), "style");
  EXPECT_EQ(shape_of("div { @apply --a(1) --b; }"), "style");
  EXPECT_EQ(shape_of("div { @apply --a(,); }"), "style[apply]") << "Mixins 1 (pinned 2026-11-07): an argument may be empty (\"--solo(,)\" passes a single empty argument)";
  EXPECT_EQ(shape_of("div { @apply --a(1,); }"), "style[apply]") << "a final trailing comma";
  EXPECT_EQ(shape_of("div { @apply --a(); }"), "style[apply]");
  EXPECT_EQ(shape_of("div { @apply --a(1, 2 3, {4, 5}); }"), "style[apply]");
  EXPECT_EQ(shape_of("div { @apply --a(red !important); }"), "style") << "a `!` at the top level is no part of a declaration value";
  EXPECT_EQ(shape_of("div { @apply --a({red !important}); }"), "style") << "nor in the contents of a {} argument (the pinned page important.html)";
  EXPECT_EQ(shape_of("div { @apply --a(f(red !important)); }"), "style[apply]") << "a nested one is inside a function";
  EXPECT_EQ(shape_of("div { @apply --a({}); }"), "style[apply]");
  EXPECT_EQ(shape_of("@mixin --m(--x: red !important) {}"), "") << "\"!important invalidates mixin parameter defaults\"";
}

/* ---- @private ------------------------------------------------------------------------------------------------------------------------------------ */

// "The @private rule is only valid nested as a child of a style rule or a nested group rule. In any other context, it is invalid." The pinned IDL has no interface for it, so a rule that is
// valid is exposed as every recognized and inactive at-rule is.
TEST(CssomMixin, APrivateRuleIsValidOnlyInAStyleRuleOrANestedGroupRule) {
  EXPECT_EQ(shape_of("@private { --x: 1 }"), "");
  EXPECT_EQ(shape_of("@media print { @private { --x: 1 } }"), "media");
  EXPECT_EQ(shape_of("div { @private { --x: 1 } }"), "style[preserved]");
  EXPECT_EQ(shape_of("div { @media print { @private { --x: 1 } } }"), "style[media[preserved]]");
  EXPECT_EQ(shape_of("@mixin --m() { @private { --x: 1 } }"), "mixin[preserved]");
}

/* ---- the text of a rule ---------------------------------------------------------------------------------------------------------------------------- */

// CSSOM, "serialize a CSSMixinRule": "@mixin" and a SPACE, the name serialized as an identifier, "If the mixin has at least one parameter: a LEFT PARENTHESIS, serialize a function parameter on
// each of the parameters joined by a COMMA and a SPACE, a RIGHT PARENTHESIS", "{" and a SPACE, "serialize a CSS rule on each rule in cssRules, filtering out empty strings, all joined by a
// single SPACE", a SPACE and "}". The draft has no SPACE before the "{" of a mixin and has one in the algorithms of @function, @apply and the contents rule: it is written.
TEST(CssomMixin, TheTextOfAMixin) {
  EXPECT_EQ(first("@mixin --m() { color: green }"), "@mixin --m { color: green; }");
  EXPECT_EQ(first("@mixin --m { color: green }"), "@mixin --m { color: green; }");
  EXPECT_EQ(first("@mixin   --m(  )   {color:green;width:1px}"), "@mixin --m { color: green; width: 1px; }");
  EXPECT_EQ(first("@mixin --m() { }"), "@mixin --m {  }") << "an opening brace and a SPACE, no rules, a SPACE and a closing brace";
  EXPECT_EQ(first("@mixin --m() { color: red; & b { color: blue } width: 1px }"), "@mixin --m { color: red; & b { color: blue; } width: 1px; }");
  EXPECT_EQ(first("@mixin --m() { @media print { color: red } }"), "@mixin --m { @media print {\n  color: red;\n} }");
  EXPECT_EQ(first("@mixin --m() { @contents; @contents { color: red } }"), "@mixin --m { @contents; @contents { color: red; } }");
}

// "serialize a function parameter": the name, "If the function parameter has a type, and that type is not the universal syntax definition: a SPACE, the result of serialize a CSS type", "If
// the function parameter has a default value: a COLON, a SPACE, the default"; "serialize a CSS type": "If the <css-type> consists of a single <syntax-component>, return the corresponding
// syntax string. Otherwise ... type( ... )".
TEST(CssomMixin, TheParametersOfAMixinAreWrittenAsTheDraftHasThem) {
  EXPECT_EQ(first("@mixin --m3(--arg type(<length>): 1em, --other-arg) { margin-left: var(--arg) }"), "@mixin --m3(--arg <length>: 1em, --other-arg) { margin-left: var(--arg); }");
  EXPECT_EQ(first("@mixin --m(--a <length>) {}"), "@mixin --m(--a <length>) {  }");
  EXPECT_EQ(first("@mixin --m(--a type(<length> | <color>): red) {}"), "@mixin --m(--a type(<length> | <color>): red) {  }");
  EXPECT_EQ(first("@mixin --m(--a type(*)) {}"), "@mixin --m(--a) {  }") << "the universal syntax is left out";
  EXPECT_EQ(first("@mixin --m(--a <length>+: 1px 2px, --b: x) {}"), "@mixin --m(--a <length>+: 1px 2px, --b: x) {  }");
  EXPECT_EQ(first("@mixin --m(  --a   <length>  :   1px  ) {}"), "@mixin --m(--a <length>: 1px) {  }");
}

// "serialize an identifier": a name is escaped as one is.
TEST(CssomMixin, TheNameOfAMixinIsWrittenAsAnIdentifier) {
  EXPECT_EQ(first("@mixin --a\\ b() {}"), "@mixin --a\\ b {  }");
  EXPECT_EQ(first("@mixin --\\31 () {}"), "@mixin --1 {  }");
}

// "serialize a CSSApplyStatementRule": "@apply" and a SPACE, the prelude, a SEMICOLON; "serialize an @apply prelude": the name, and "If the @apply rule has at least one argument: a LEFT
// PARENTHESIS, serialize a function argument on each argument joined by a COMMA and a SPACE, a RIGHT PARENTHESIS". The text of a rule inside a style rule is on a line of its own.
TEST(CssomMixin, TheTextOfAnApplyStatement) {
  EXPECT_EQ(first("#foo { @apply --m1; }"), "#foo {\n  @apply --m1;\n}");
  EXPECT_EQ(first("#foo { @apply --m1(); }"), "#foo {\n  @apply --m1;\n}") << "a dashed-ident and a function with no arguments are one";
  EXPECT_EQ(first("#foo { @apply --m(blue, 1px  2px); }"), "#foo {\n  @apply --m(blue, 1px 2px);\n}");
  EXPECT_EQ(first("#foo { color: red; @apply --m2 { color: green; } }"), "#foo {\n  color: red;\n  @apply --m2 { color: green; }\n}");
  EXPECT_EQ(first("#foo { @apply --m5 { } }"), "#foo {\n  @apply --m5 {  }\n}");
  EXPECT_EQ(first("#foo { @apply --m(a) { color: green; & b { color: red } } }"), "#foo {\n  @apply --m(a) { color: green; & b { color: red; } }\n}");
}

// "serialize a function argument": "Serialize argument as if it had been the specified value of a custom property ... If serialized argument does not contain any top-level <comma-token>s,
// nor any top-level <{-token>s, return serialized argument. Otherwise, return a LEFT CURLY BRACKET, a SPACE, the value, a SPACE and a RIGHT CURLY BRACKET." and, of an argument written
// as `{}` ("a {} argument is its contents"), the braces are written only when the contents need them.
TEST(CssomMixin, AnArgumentIsWrittenInBracesWhenItHasACommaOrABrace) {
  EXPECT_EQ(first("#foo { @apply --m({green}); }"), "#foo {\n  @apply --m(green);\n}");
  EXPECT_EQ(first("#foo { @apply --m({a, b}); }"), "#foo {\n  @apply --m({ a, b });\n}");
  EXPECT_EQ(first("#foo { @apply --m({a {b}}); }"), "#foo {\n  @apply --m({ a {b} });\n}");
  EXPECT_EQ(first("#foo { @apply --m(f(a, b)); }"), "#foo {\n  @apply --m(f(a, b));\n}") << "a comma inside a function is not at the top level";
  EXPECT_EQ(first("#foo { @apply --m(  a   b  ); }"), "#foo {\n  @apply --m(a b);\n}");
}

// "serialize a CSSContentsStatementRule": "@contents" and a SEMICOLON; "serialize a CSSContentsBlockRule": "@contents", a SPACE, "{" and a SPACE, the rules, a SPACE and "}".
TEST(CssomMixin, TheTextOfAContentsRule) {
  Doc doc;
  Sheet s(doc, "@mixin --m() { @contents; @contents { color: red } @contents {} @contents { a {} } }");
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  EXPECT_EQ(css(path(s, {0, 0}).r), "@contents;");
  EXPECT_EQ(css(path(s, {0, 1}).r), "@contents { color: red; }");
  EXPECT_EQ(css(path(s, {0, 2}).r), "@contents {  }");
  EXPECT_EQ(css(path(s, {0, 3}).r), "@contents { & a { } }") << "a nested rule in a fallback block";
}

// The text of a sheet is a fixed point: the sheet read again from it writes the same bytes.
TEST(CssomMixin, TheTextIsAFixedPoint) {
  const char * sheets[] = {
      "@mixin --m(--a <length>: 1px, --b type(<color> | auto)) { color: var(--b); @media print { @contents { color: red } } & > p { @apply --n(1, {2, 3}) { width: var(--a) } } }",
      "div { @apply --a; @apply --b(c) { color: red; & d { @apply --e; } } }",
      "@mixin --x { @contents; }",
  };
  for (const char * css_text : sheets) {
    Doc doc;
    Sheet a(doc, css_text);
    ASSERT_EQ(a.status, GTEXT_CSS_OK);
    const std::string once = sheet_text(a.s);
    Sheet b(doc, once);
    ASSERT_EQ(b.status, GTEXT_CSS_OK);
    EXPECT_EQ(sheet_text(b.s), once) << css_text;
    EXPECT_EQ(shape(b), shape(a)) << css_text;
  }
}

/* ---- insertRule ---------------------------------------------------------------------------------------------------------------------------------- */

// CSSOM: "The CSSMixinRule interface ... CSSGroupingRule", so a rule is inserted into a mixin as into any grouping rule; the pinned page mixin-cssom.tentative.html: "@apply is not legal at top
// level" (`insertRule('@apply --m1();')` throws a SyntaxError).
TEST(CssomMixin, InsertRuleMakesAndPlacesTheNewRules) {
  Doc doc;
  Sheet s(doc, "div { color: red }");
  Ins m = insert_rule(s.s, "@mixin --m() { color: green }", 0);
  EXPECT_EQ(m.st, GTEXT_CSS_OK);
  EXPECT_EQ(shape(s), "mixin[nested-declarations],style");
  Ins bad = insert_rule(s.s, "@apply --m1();", 0);
  EXPECT_EQ(bad.st, GTEXT_CSS_E_SYNTAX) << "an @apply is not a rule of a sheet";
  Ins bad2 = insert_rule(s.s, "@contents;", 0);
  EXPECT_EQ(bad2.st, GTEXT_CSS_E_SYNTAX);
  Rule div = at(s.rules(), 1);
  Ins a = insert_rule(div.r, "@apply --m;", 0);
  EXPECT_EQ(a.st, GTEXT_CSS_OK);
  EXPECT_EQ(css(div.r), "div {\n  color: red;\n  @apply --m;\n}");
  Rule mixin = at(s.rules(), 0);
  Ins c = insert_rule(mixin.r, "@contents { color: blue }", 1);
  EXPECT_EQ(c.st, GTEXT_CSS_OK);
  EXPECT_EQ(css(mixin.r), "@mixin --m { color: green; @contents { color: blue; } }");
  Ins l = insert_rule(mixin.r, "@layer x {}", 0);
  EXPECT_EQ(l.st, GTEXT_CSS_E_SYNTAX) << "an @layer is not valid in a mixin body";
  Ins nm = insert_rule(mixin.r, "@mixin --n() {}", 0);
  EXPECT_NE(nm.st, GTEXT_CSS_OK) << "a mixin is not valid in a mixin";
  Ins c2 = insert_rule(div.r, "@contents;", 0);
  EXPECT_NE(c2.st, GTEXT_CSS_OK) << "@contents is not valid outside a mixin body";
  EXPECT_EQ(delete_rule(mixin.r, 1), GTEXT_CSS_OK);
  EXPECT_EQ(css(mixin.r), "@mixin --m { color: green; }");
}

TEST(CssomMixin, DeclarationsAreInsertedIntoABlockAsANestedDeclarationsRule) {
  Doc doc;
  Sheet s(doc, "@mixin --m() { @contents; }");
  Rule m = at(s.rules(), 0);
  Ins d = insert_rule(m.r, "color: green", 0);
  EXPECT_EQ(d.st, GTEXT_CSS_OK);
  EXPECT_EQ(css(m.r), "@mixin --m { color: green; @contents; }");
  EXPECT_EQ(shape(s), "mixin[nested-declarations,contents]");
}

// A rule's text that is changed and read again is the rule it was: an edit of a rule inside a mixin does not lose the `@contents` that is valid there.
TEST(CssomMixin, EditingARuleInAMixinKeepsWhatIsValidInIt) {
  Doc doc;
  Sheet s(doc, "@mixin --m() { & a { @contents { color: red } } }");
  Rule inner = path(s, {0, 0});
  ASSERT_TRUE(inner);
  char * out = nullptr;
  size_t n = 0;
  GTEXT_CSSOM_Outcome o{};
  ASSERT_EQ(gtext_cssom_rule_set_selector_text(inner.r, "& b", 3, &o), GTEXT_CSS_OK);
  (void)out;
  (void)n;
  EXPECT_EQ(shape(s), "mixin[style[contents[nested-declarations]]]");
  EXPECT_EQ(css(path(s, {0}).r), "@mixin --m { & b {\n  @contents { color: red; }\n} }");
}

/* ---- the members -------------------------------------------------------------------------------------------------------------------------------- */

std::string attribute(const Rule & r, const char * name) {
  char * out = nullptr;
  size_t n = 0;
  bool is_null = false;
  const GTEXT_CSS_Status st = gtext_cssom_rule_get_attribute(r.r, name, std::strlen(name), &out, &n, &is_null);
  if (st != GTEXT_CSS_OK) {
    return std::string("<") + gtext_css_status_string(st) + ">";
  }
  std::string text(out, n);
  gtext_cssom_buffer_free(&reinterpret_cast<css_obj *>(r.r)->core->alloc, out);
  return text;
}

// CSSOM: "name: The result of serialize an identifier on the mixin's name"; "contents: True if the mixin accepts a contents block, and false otherwise."
TEST(CssomMixin, TheNameAndTheContentsFlagOfAMixin) {
  Doc doc;
  Sheet s(doc, "@mixin --a\\ b() {} @mixin --c() { color: red } @mixin --d { @contents; } @mixin --e() { @media print { & p { @contents { color: red } } } } @mixin --f() { @apply --d; }");
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  EXPECT_EQ(attribute(at(s.rules(), 0), "name"), "--a\\ b");
  EXPECT_EQ(attribute(at(s.rules(), 1), "name"), "--c");
  EXPECT_EQ(attribute(at(s.rules(), 0), "contents"), "false");
  EXPECT_EQ(attribute(at(s.rules(), 1), "contents"), "false");
  EXPECT_EQ(attribute(at(s.rules(), 2), "contents"), "true");
  EXPECT_EQ(attribute(at(s.rules(), 3), "contents"), "true") << "at any depth of the body";
  EXPECT_EQ(attribute(at(s.rules(), 4), "contents"), "false") << "an @apply of a mixin that takes contents does not make this one take them";
  bool writable = true;
  EXPECT_TRUE(gtext_cssom_rule_has_attribute(at(s.rules(), 0).r, "name", 4, &writable));
  EXPECT_FALSE(writable);
  EXPECT_FALSE(gtext_cssom_rule_has_attribute(at(s.rules(), 0).r, "getParameters", 13, nullptr)) << "an operation is not an attribute";
  EXPECT_EQ(attribute(at(s.rules(), 0), "selectorText"), "<not supported>");
}

// A contents block is the rules the block holds, and edited in place it changes what the mixin accepts.
TEST(CssomMixin, TheContentsFlagFollowsTheRulesInTheMixin) {
  Doc doc;
  Sheet s(doc, "@mixin --m() { color: red; @contents; }");
  Rule m = at(s.rules(), 0);
  EXPECT_EQ(attribute(m, "contents"), "true");
  ASSERT_EQ(gtext_cssom_rule_delete_rule(m.r, 1, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(attribute(m, "contents"), "false");
}

// CSSApplyBlockRule and CSSApplyStatementRule: "name: If the @apply rule has an associated <dashed-ident>, the result of serialize an identifier on that ident. Otherwise, the result of serialize an
// identifier on the name of the associated <dashed-function>."
TEST(CssomMixin, TheNameOfAnApplyRule) {
  Doc doc;
  Sheet s(doc, "a { @apply --one; @apply --two(blue); @apply --three { color: red } @apply --f\\ g(1) { } }");
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  EXPECT_EQ(attribute(path(s, {0, 0}), "name"), "--one");
  EXPECT_EQ(attribute(path(s, {0, 1}), "name"), "--two");
  EXPECT_EQ(attribute(path(s, {0, 2}), "name"), "--three");
  EXPECT_EQ(attribute(path(s, {0, 3}), "name"), "--f\\ g");
}

// CSSMixinRule.getParameters(): the FunctionParameter of each parameter: "name", "type: represented as a syntax string, or "*" if the parameter has no type", "defaultValue: or null if the
// argument does not have a default".
TEST(CssomMixin, GetParametersIsTheParametersOfTheMixin) {
  Doc doc;
  Sheet s(doc, "@mixin --m(--a, --b <length>: 1px, --c type(<color> | auto): red, --d type(*): x y, --e <length>+) {} @mixin --n {} @mixin --o() {}");
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  Rule m = at(s.rules(), 0);
  GTEXT_CSSOM_Function_Parameters ps{};
  ASSERT_EQ(gtext_cssom_mixin_get_parameters(m.r, &ps), GTEXT_CSS_OK);
  ASSERT_EQ(ps.count, 5u);
  struct Want { const char * name, * type, * def; bool has; };
  const Want want[] = {{"--a", "*", "", false}, {"--b", "<length>", "1px", true}, {"--c", "<color> | auto", "red", true}, {"--d", "*", "x y", true}, {"--e", "<length>+", "", false}};
  for (size_t i = 0; i < 5; i++) {
    EXPECT_EQ(std::string(ps.items[i].name, ps.items[i].name_length), want[i].name) << i;
    EXPECT_EQ(std::string(ps.items[i].type, ps.items[i].type_length), want[i].type) << i;
    EXPECT_EQ(ps.items[i].has_default, want[i].has) << i;
    EXPECT_EQ(std::string(ps.items[i].default_value, ps.items[i].default_length), want[i].def) << i;
  }
  gtext_cssom_buffer_free(&reinterpret_cast<css_obj *>(m.r)->core->alloc, static_cast<char *>(ps.block));
  for (size_t k = 1; k <= 2; k++) {
    Rule none = at(s.rules(), k);
    GTEXT_CSSOM_Function_Parameters zero{};
    ASSERT_EQ(gtext_cssom_mixin_get_parameters(none.r, &zero), GTEXT_CSS_OK);
    EXPECT_EQ(zero.count, 0u);
    EXPECT_EQ(zero.items, nullptr);
    gtext_cssom_buffer_free(&reinterpret_cast<css_obj *>(none.r)->core->alloc, static_cast<char *>(zero.block));
  }
  GTEXT_CSSOM_Function_Parameters bad{};
  Doc d2;
  Sheet t(d2, "a { color: red }");
  EXPECT_EQ(gtext_cssom_mixin_get_parameters(at(t.rules(), 0).r, &bad), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_EQ(gtext_cssom_mixin_get_parameters(nullptr, &bad), GTEXT_CSS_E_INVALID);
}

// getArguments(): each argument as a string, a `{}` argument as its contents, the statement and the block form alike.
TEST(CssomMixin, GetArgumentsIsTheArgumentsOfTheApply) {
  Doc doc;
  Sheet s(doc, "a { @apply --x; @apply --y(blue, 1px  2px, {a, b}, f(c, d)); @apply --z({}) { color: red } }");
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  const std::vector<std::vector<std::string>> want = {{}, {"blue", "1px 2px", "a, b", "f(c, d)"}, {""}};
  for (size_t i = 0; i < 3; i++) {
    Rule r = path(s, {0, i});
    GTEXT_CSSOM_String_List l{};
    ASSERT_EQ(gtext_cssom_apply_get_arguments(r.r, &l), GTEXT_CSS_OK) << i;
    ASSERT_EQ(l.count, want[i].size()) << i;
    for (size_t k = 0; k < l.count; k++) {
      EXPECT_EQ(std::string(l.items[k].text, l.items[k].length), want[i][k]) << i << "," << k;
    }
    gtext_cssom_buffer_free(&reinterpret_cast<css_obj *>(r.r)->core->alloc, static_cast<char *>(l.block));
  }
  GTEXT_CSSOM_String_List l{};
  EXPECT_EQ(gtext_cssom_apply_get_arguments(path(s, {0}).r, &l), GTEXT_CSS_E_UNSUPPORTED);
}

// Mixins 1 (pinned 2026-11-07), @apply: "The @apply <dashed-function> syntax has the same whitespace-trimming behavior as custom functions, and the same treatment of a final trailing
// comma." and, for custom functions, "If the final argument is empty, it's dropped" and "--func(1, 2,,) (or --solo(,) to pass a single empty argument)".
TEST(CssomMixin, AFinalEmptyArgumentOfAnApplyIsDropped) {
  Doc doc;
  Sheet s(doc, "a { @apply --a(); @apply --b(1,); @apply --c(1, 2,,); @apply --d(,); @apply --e(,,); @apply --f( x , ); @apply --g(,1); }");
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  const std::vector<std::vector<std::string>> want = {{}, {"1"}, {"1", "2", ""}, {""}, {"", ""}, {"x"}, {"", "1"}};
  for (size_t i = 0; i < want.size(); i++) {
    Rule r = path(s, {0, i});
    GTEXT_CSSOM_String_List l{};
    ASSERT_EQ(gtext_cssom_apply_get_arguments(r.r, &l), GTEXT_CSS_OK) << i;
    ASSERT_EQ(l.count, want[i].size()) << i;
    for (size_t k = 0; k < l.count; k++) {
      EXPECT_EQ(std::string(l.items[k].text, l.items[k].length), want[i][k]) << i << "," << k;
    }
    gtext_cssom_buffer_free(&reinterpret_cast<css_obj *>(r.r)->core->alloc, static_cast<char *>(l.block));
  }
}

// "<@mixin> = @mixin [<function-token> <function-parameter>#? ,? ) | <ident-token> ]" and "Note: Like @function, a single trailing comma can be put at the end of the argument list."
TEST(CssomMixin, AMixinTakesATrailingCommaAndAnEmptyDefault) {
  EXPECT_EQ(first("@mixin --m(--a, --b,) {}"), "@mixin --m(--a, --b) {  }");
  EXPECT_EQ(first("@mixin --m(--a:) {}"), "@mixin --m(--a: ) {  }") << "an empty default is a default: the colon and the space are written";
  EXPECT_EQ(first("@mixin --m(--a,,) {}"), "<no rule>") << "a second comma is no trailing comma";
}

/* ---- allocation failure ------------------------------------------------------------------------------------------------------------------------- */

// Every allocation of the text of a mixin, an @apply and an @contents, and of the members that read their preludes, fails in turn: the call is the one it would have been or is
// out of memory, the rule is as it was, and nothing is held once the document is released.
TEST(CssomMixinOom, EveryAllocationOfTheTextAndTheMembersFailsCleanly) {
  const char * const text =
      "@mixin --m(--a <length>: 1px, --b type(<color> | auto), --c) { color: var(--a); @media print { @contents { color: red } } & > p { @apply --n(1px, {a, b}) { width: 1px } } }"
      " div { @apply --m(2px, red); @apply --k; }";
  std::string want_mixin, want_apply;
  {
    Doc doc;
    Sheet s(doc, text);
    ASSERT_EQ(s.status, GTEXT_CSS_OK);
    want_mixin = css(at(s.rules(), 0).r);
    want_apply = css(path(s, {1, 0}).r);
  }
  size_t failures = 0;
  for (size_t n = 1; n < 100000; n++) {
    FaultAllocator fa;
    bool done = false;
    {
      Doc doc(nullptr, &fa.alloc);
      Sheet s(doc, text);
      ASSERT_EQ(s.status, GTEXT_CSS_OK);
      Rule m = at(s.rules(), 0);
      Rule a = path(s, {1, 0});
      ASSERT_TRUE(m && a);
      fa.arm(n);
      char * out = nullptr;
      size_t len = 0;
      GTEXT_CSS_Status st = gtext_cssom_rule_css_text(m.r, &out, &len);
      if (st == GTEXT_CSS_OK) {
        EXPECT_EQ(take(doc.core(), out), want_mixin) << n;
      }
      else {
        EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
      }
      st = gtext_cssom_rule_css_text(a.r, &out, &len);
      if (st == GTEXT_CSS_OK) {
        EXPECT_EQ(take(doc.core(), out), want_apply) << n;
      }
      else {
        EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
      }
      GTEXT_CSSOM_Function_Parameters ps{};
      st = gtext_cssom_mixin_get_parameters(m.r, &ps);
      if (st == GTEXT_CSS_OK) {
        EXPECT_EQ(ps.count, 3u) << n;
        gtext_cssom_buffer_free(&doc.core()->alloc, static_cast<char *>(ps.block));
      }
      else {
        EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
      }
      GTEXT_CSSOM_String_List l{};
      st = gtext_cssom_apply_get_arguments(a.r, &l);
      if (st == GTEXT_CSS_OK) {
        EXPECT_EQ(l.count, 2u) << n;
        gtext_cssom_buffer_free(&doc.core()->alloc, static_cast<char *>(l.block));
      }
      else {
        EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
      }
      char * name = nullptr;
      bool is_null = false;
      st = gtext_cssom_rule_get_attribute(m.r, "contents", 8, &name, &len, &is_null);
      if (st == GTEXT_CSS_OK) {
        EXPECT_EQ(take(doc.core(), name), "true") << n;
      }
      else {
        EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
      }
      done = !fa.failed;
      fa.disarm();
      if (!done) {
        failures++;
      }
      EXPECT_EQ(css(m.r), want_mixin) << "after allocation " << n << " failed";
    }
    EXPECT_TRUE(fa.live.empty()) << "allocation " << n << " leaked";
    EXPECT_EQ(fa.bad_frees, 0u);
    if (done) {
      break;
    }
  }
  EXPECT_GT(failures, 10u);
}

} // namespace
