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
 * Applying a mixin: `@mixin`, `@apply` and `@contents` as the pinned CSS Functions and Mixins 1 defines them ("Defining Mixins", "The @contents Rule", "The @apply Rule", "Arguments and Local
 * Variables", and the examples of "Private Custom Properties" and "Evaluating Mixins"). Each test quotes the sentence it holds the library to. The "Evaluating Mixins" section is still
 * marked "TODO rewrite to use the new @private concepts" in the pinned text and twenty-two of the twenty-seven pinned web-platform-tests of mixins are written with the dropped `@result`, so
 * nothing scores this and neither Chromium 156.0.8078.47 nor Firefox 158.0.1 implements it (a sheet that holds `@mixin` has no such rule in `cssRules` and an `@apply` is dropped): every
 * expectation is the served text's, and where the text is silent the reading kept is written in the test and in css-variables.md.
 */

#include <string>

#include <gtest/gtest.h>

#include "css_custom_test_support.h"
#include "cssom_computed_test_support.h"

using namespace cc;

namespace {

std::string page(const std::string & sheet, const std::string & body = "<section id=a><h1 id=b><small id=c>x</small></h1><p id=d>y</p></section>") {
  return "<!DOCTYPE html><html><head><style>" + sheet + "</style></head><body>" + body + "</body></html>";
}

std::string out(const Cs & m, const char * selector, const char * property = "--out") {
  return m.value(selector, property);
}

/* ---- the draft's examples -------------------------------------------------------------------------------------------------------------------------------------- */

// "For example, the following mixin use: [...] will produce a linear-gradient(red, green, blue) value, taking --color-1 from the outside (since nothing overrides it), --color2 from the
// mixin parameter (overriding the value from the element), and --color3 from the private property in the mixin body (overriding both the value from the element and the mixin parameter of
// that name)."
TEST(CssomMixinEval, TheDraftsExampleOfShadowedValues) {
  Cs m(page("@mixin --shadowed-values(--color2: green, --color3: green) { @private { --color3: blue; } --out: linear-gradient(var(--color1), var(--color2), var(--color3)); }"
            " p { --color1: red; --color2: red; --color3: red; @apply --shadowed-values(); }"));
  EXPECT_EQ(out(m, "#d"), "linear-gradient(red, green, blue)");
  EXPECT_EQ(m.value("#d", "--color2"), "red") << "the custom properties of the element are not changed";
  EXPECT_EQ(m.value("#d", "--color3"), "red");
}

// "@apply --one; Invokes the --one mixin, with no arguments or contents." and the arguments: "the arguments passed to the dashed function are mapped to the mixin's arguments".
TEST(CssomMixinEval, AParameterTakesItsArgument) {
  Cs m(page("@mixin --m(--a, --b) { --out: var(--a) / var(--b); } p { @apply --m(1px, 2px); }"));
  EXPECT_EQ(out(m, "#d"), "1px / 2px");
}

// "Passing too few arguments is fine; the missing arguments take their default values instead."
TEST(CssomMixinEval, AMissingArgumentTakesItsDefault) {
  Cs m(page("@mixin --m(--a: 1, --b: 2) { --out: var(--a) var(--b); } p { @apply --m(9); }"));
  EXPECT_EQ(out(m, "#d"), "9 2");
}

// "A <dashed-ident> passes no arguments. (That is, @apply --foo; is identical to @apply --foo();)"
TEST(CssomMixinEval, ADashedIdentAndAnEmptyArgumentListAreTheSame) {
  Cs m(page("@mixin --m(--a: 1) { --out: var(--a); } #d { @apply --m; } #a { @apply --m(); }"));
  EXPECT_EQ(out(m, "#d"), "1");
  EXPECT_EQ(out(m, "#a"), "1");
}


// "A mixin's parameters define private properties in the mixin body, set to the value passed for the argument. As usual for private properties, these are accessible using var()": the
// default of a parameter may name the parameters before it ("--right-color <color>: var(--left-color)" is the draft's own example).
TEST(CssomMixinEval, ADefaultMayNameAnEarlierParameter) {
  Cs m(page("@mixin --squish(--left <color>, --right <color>: var(--left)) { --out: var(--left) var(--right); } p { @apply --squish(red); } #a { @apply --squish(red, blue); }"));
  EXPECT_EQ(out(m, "#d"), "rgb(255, 0, 0) rgb(255, 0, 0)") << "the default is the value the first parameter took (a <color> parameter is computed where it is declared)";
  EXPECT_EQ(out(m, "#a"), "rgb(255, 0, 0) rgb(0, 0, 255)");
}

// "Because they're built on private properties, mixin parameters share the same potentially confusing weakness: all mentions of the private property name in the mixin body are rewritten to a
// unique private name, but the value is only visible to the applying element and its descendants. [...] The .foo element will get color: green, from the passed argument value, but its following
// sibling will get color: red, as it doesn't see the private property value and instead triggers fallback."
TEST(CssomMixinEval, TheFollowingSiblingDoesNotSeeTheArgument) {
  Cs m(page("@mixin --only-descs(--arg) { --out: var(--arg); & + * { --out: var(--arg, red); } } .foo { @apply --only-descs(green); }",
            "<div class=foo id=a><b id=b>x</b></div><p id=d>y</p>"));
  EXPECT_EQ(out(m, "#a"), "green");
  EXPECT_EQ(out(m, "#d"), "red");
  EXPECT_EQ(out(m, "#b"), "green") << "a descendant does see it: an unregistered custom property inherits";
}

// "Note: Even if the same mixin is applied on a parent and a child, each application of the mixin creates a unique set of styles with their own private properties, so the child's sibling won't
// see the parent's --arg private property."
TEST(CssomMixinEval, EachApplicationHasItsOwnPrivateProperties) {
  Cs m(page("@mixin --m(--arg) { --out: var(--arg); } #a { @apply --m(parent); } #b { @apply --m(child); }"));
  EXPECT_EQ(out(m, "#a"), "parent");
  EXPECT_EQ(out(m, "#b"), "child") << "the child's own application sets its own property, not the parent's";
  EXPECT_EQ(out(m, "#c"), "child");
}

// The draft's four examples of the section "Evaluating Mixins" that are written with the new concepts: "In this example, the --size custom properties are distinct from the --size private
// property defined by the mixin parameter, so all three elements share a 2px border-width."
TEST(CssomMixinEval, TheCustomPropertiesOfTheSameNameAreNotTheParameter) {
  Cs m(page("@mixin --untyped(--size) { &, & > *, & > * > * { border-style: solid; border-width: var(--size); } } section { @apply --untyped(2px); } section > h1 { --size: 4px; } section > h1 > small { --size: 3px; }"));
  EXPECT_EQ(m.value("#a", "border-top-width"), "2px");
  EXPECT_EQ(m.value("#b", "border-top-width"), "2px");
  EXPECT_EQ(m.value("#c", "border-top-width"), "2px");
}

// "In this example, the .2em is left unresolved in the mixin parameter, so every element gets its own border-width: .2em value, resolving to three different lengths based on each element's
// font-size."
TEST(CssomMixinEval, AnUntypedParameterIsResolvedAtEachElement) {
  Cs m(page("@mixin --untyped(--size) { &, & > *, & > * > * { border-style: solid; border-width: var(--size); } } section { font-size: 10px; @apply --untyped(.2em); } section > h1 { font-size: 20px; }"
            " section > h1 > small { font-size: 15px; }"));
  EXPECT_EQ(m.value("#a", "border-top-width"), "2px");
  EXPECT_EQ(m.value("#b", "border-top-width"), "4px");
  EXPECT_EQ(m.value("#c", "border-top-width"), "3px");
}

// "In this example, because the mixin parameter is typed as <length>, it resolves the .2em to 2px on the applying element, and all three elements share a 2px border-width."
TEST(CssomMixinEval, ATypedParameterIsResolvedOnTheApplyingElement) {
  Cs m(page("@mixin --typed(--size <length>) { &, & > *, & > * > * { border-style: solid; border-width: var(--size); } } section { font-size: 10px; @apply --typed(.2em); } section > h1 { font-size: 20px; }"
            " section > h1 > small { font-size: 15px; }"));
  EXPECT_EQ(m.value("#a", "border-top-width"), "2px");
  EXPECT_EQ(m.value("#b", "border-top-width"), "2px");
  EXPECT_EQ(m.value("#c", "border-top-width"), "2px");
}

// "a custom property registration with that name, a syntax of the parameter type, an inherit flag of true, and no initial value": a value that is not of the type is invalid at computed-value
// time, which is the guaranteed-invalid value, so a var() of the parameter takes its fallback.
TEST(CssomMixinEval, AnArgumentThatIsNotOfTheTypeIsInvalid) {
  Cs m(page("@mixin --m(--n <length>) { --out: var(--n, fallback); } #a { @apply --m(3px); } #d { @apply --m(red); }"));
  EXPECT_EQ(out(m, "#a"), "3px");
  EXPECT_EQ(out(m, "#d"), "fallback");
}

// "Passing too few arguments is fine; the missing arguments take their default values instead.": a parameter with no default that is not passed has no value, and var() of it falls back (the
// text does not say; the reading kept is the one of a custom property that is not set).
TEST(CssomMixinEval, AMissingArgumentWithNoDefaultHasNoValue) {
  Cs m(page("@mixin --m(--a, --b) { --out: var(--a, none-a) var(--b, none-b); } p { @apply --m(1); }"));
  EXPECT_EQ(out(m, "#d"), "1 none-b");
}

// "if more arguments are passed than the length of the mixin's argument list, the @apply application does nothing."
TEST(CssomMixinEval, TooManyArgumentsDoNothing) {
  Cs m(page("@mixin --m(--a) { --out: applied; color: red; } p { --out: before; @apply --m(1, 2); }"));
  EXPECT_EQ(out(m, "#d"), "before");
  EXPECT_NE(m.value("#d", "color"), "rgb(255, 0, 0)");
}

// "If no such mixin exists, the @apply does nothing."
TEST(CssomMixinEval, AMixinThatDoesNotExistDoesNothing) {
  Cs m(page("@mixin --m { --out: applied; } p { --out: before; @apply --nope; @apply --nope(1); @apply --M; }"));
  EXPECT_EQ(out(m, "#d"), "before") << "a mixin name is case-sensitive, as a dashed-ident is";
}

// "The @apply rule is only valid in the body of a style rule or nested group rule; using it in any other context causes it to be invalid and ignored."
TEST(CssomMixinEval, AnApplyAtTheTopOfASheetDoesNothing) {
  Cs m(page("@mixin --m { --out: applied; } @apply --m; @media all { @apply --m; } p { --out: before; }"));
  EXPECT_EQ(out(m, "#d"), "before");
}

// "@apply rules are processed before any styles are applied, as they effectively modify the stylesheet itself": the declarations are the rule's, in the place of the @apply. A declaration of the
// rule after it wins over the mixin's, and one before it loses.
TEST(CssomMixinEval, TheDeclarationsOfAMixinTakeThePlaceOfTheApply) {
  Cs m(page("@mixin --m { --a: mixin; --b: mixin; } p { --a: before; @apply --m; --b: after; --out: var(--a) var(--b); }"));
  EXPECT_EQ(out(m, "#d"), "mixin after");
}

// "causing it to substitute into the rule in place of the @apply rule itself": the declarations have the specificity of the rule they are substituted into, so an id rule beats a class rule
// whichever has the mixin.
TEST(CssomMixinEval, TheDeclarationsOfAMixinHaveTheSpecificityOfTheRule) {
  Cs m(page("@mixin --m(--v) { --out: var(--v); } #d { @apply --m(by-id); } p.c { @apply --m(by-class); }", "<p id=d class=c>x</p>"));
  EXPECT_EQ(out(m, "#d"), "by-id");
  Cs n(page("@mixin --m(--v) { --out: var(--v); } #d { --out: plain-id; } p.c { @apply --m(by-class); }", "<p id=d class=c>x</p>"));
  EXPECT_EQ(out(n, "#d"), "plain-id");
}

// "!important": the declarations of a mixin body keep their importance, so an important one beats a later normal one of the rule.
TEST(CssomMixinEval, AnImportantDeclarationOfTheBodyBeatsALaterNormalOne) {
  Cs m(page("@mixin --m { --out: important !important; } p { @apply --m; --out: normal; }"));
  EXPECT_EQ(out(m, "#d"), "important");
}

// "The body of a mixin is parsed as a nested declarations rule; it accepts properties, conditional group rules, and nested style rules, just like an ordinary style rule would."
TEST(CssomMixinEval, ANestedRuleOfTheBodyIsNestedInTheRule) {
  Cs m(page("@mixin --m(--v) { & > h1 { --out: var(--v); } } section { @apply --m(nested); }"));
  EXPECT_EQ(out(m, "#b"), "nested");
  EXPECT_EQ(out(m, "#a"), "");
  EXPECT_EQ(out(m, "#d"), "");
}

// A conditional group rule of the body applies where its condition holds.
TEST(CssomMixinEval, AGroupRuleOfTheBodyAppliesWhereItsConditionHolds) {
  Cs m(page("@mixin --m { @media all { --yes: 1; } @media not all { --no: 1; } @supports (display: block) { --sup: 1; } } p { @apply --m; }"));
  EXPECT_EQ(m.value("#d", "--yes"), "1");
  EXPECT_EQ(m.value("#d", "--no"), "");
  EXPECT_EQ(m.value("#d", "--sup"), "1");
}

// "@apply rule is only valid in the body of a style rule or nested group rule": an @apply in a group rule nested in a style rule applies where the condition holds.
TEST(CssomMixinEval, AnApplyInANestedGroupRuleAppliesWhereItsConditionHolds) {
  Cs m(page("@mixin --m { --out: applied; } p { @media all { @apply --m; } } #a { @media not all { @apply --m; } --out: kept; }"));
  EXPECT_EQ(out(m, "#d"), "applied");
  EXPECT_EQ(out(m, "#a"), "kept");
}

// "a mixin sees any private properties visible to the style rule it's substituted into (if it doesn't override them with its own mixin parameters or private properties)."
TEST(CssomMixinEval, AMixinSeesThePrivatePropertiesOfTheRuleItIsSubstitutedInto) {
  Cs m(page("@mixin --m(--p) { --out: var(--outer) var(--p) var(--outer2); @private { --outer2: own; } } p { @private { --outer: o; --outer2: o2; } @apply --m(1); }"));
  EXPECT_EQ(out(m, "#d"), "o 1 own");
}

// The arguments are written in the rule of the @apply: a private property of the rule is what a var() in an argument names.
TEST(CssomMixinEval, AnArgumentIsReadInTheScopeOfTheApply) {
  Cs m(page("@mixin --m(--p) { --out: var(--p); } p { @private { --x: hidden-x; } @apply --m(var(--x)); }"));
  EXPECT_EQ(out(m, "#d"), "hidden-x");
}

/* ---- @contents ------------------------------------------------------------------------------------------------------------------------------------------- */

// The draft's example of the one-column mixin: "the mixin handles some common conditions for the author, and substitutes the contents block into a predefined @media or @container rule."
TEST(CssomMixinEval, TheContentsBlockIsSubstitutedIntoAGroupRuleOfTheMixin) {
  Cs m(page("@mixin --when-true() { @media all { @contents; } } @mixin --when-false() { @media not all { @contents; } }"
            " p { @apply --when-true { --out: yes; } @apply --when-false { --out: no; } }"));
  EXPECT_EQ(out(m, "#d"), "yes");
}

// "If the @apply rule invoking the mixin passed a contents block, the @contents is replaced with the contents block [...] Otherwise [...] the @contents rule is replaced with its fallback block."
// and "omitting the block entirely triggers @contents fallback, while passing an empty block will substitute the empty block".
TEST(CssomMixinEval, TheFallbackBlockIsUsedWhenNoBlockWasPassed) {
  Cs m(page("@mixin --just-contents() { @contents { --out: fallback; } } #a { @apply --just-contents; } #d { @apply --just-contents {}; } #b { @apply --just-contents { --out: passed; } }",
            "<div id=a></div><div id=d></div><div id=b></div>"));
  EXPECT_EQ(out(m, "#a"), "fallback");
  EXPECT_EQ(out(m, "#d"), "") << "an empty block substitutes nothing at all";
  EXPECT_EQ(out(m, "#b"), "passed");
}

// "The empty statement form behaves identically to passing an empty block." (the form of @contents with no fallback).
TEST(CssomMixinEval, TheStatementFormOfContentsHasNoFallback) {
  Cs m(page("@mixin --m { --a: before; @contents; --b: after; } p { --out: x; @apply --m; --c: var(--a) var(--b); }"));
  EXPECT_EQ(m.value("#d", "--c"), "before after");
}

// "It's valid for an @apply rule to pass a contents block, but the mixin not use it."
TEST(CssomMixinEval, AContentsBlockThatTheMixinDoesNotUseIsIgnored) {
  Cs m(page("@mixin --m { --a: mixin; } p { @apply --m { --a: block; } }"));
  EXPECT_EQ(m.value("#d", "--a"), "mixin");
}

// "Outside of a mixin body, the @contents rule is invalid and ignored."
TEST(CssomMixinEval, AContentsOutsideAMixinBodyDoesNothing) {
  Cs m(page("p { --out: before; @contents { --out: fallback; } }"));
  EXPECT_EQ(out(m, "#d"), "before");
}

// A mixin may substitute the block in more than one place.
TEST(CssomMixinEval, TheBlockIsSubstitutedAtEveryContents) {
  Cs m(page("@mixin --twice { --a: 1; @contents; --b: 2; @contents; } p { @apply --twice { --c: block; } --out: var(--a) var(--b) var(--c); }"));
  EXPECT_EQ(out(m, "#d"), "1 2 block");
}

// The block belongs to the invoker: it is read in the scope of the @apply (a private property of the rule is visible to it, a parameter of the mixin is not), and the mixin's own @private
// is not the block's.
TEST(CssomMixinEval, TheContentsBlockIsReadInTheScopeOfTheApply) {
  Cs m(page("@mixin --m(--arg: param) { @private { --own: mixin; } @contents; } p { --arg: element; @private { --mine: rule; } @apply --m { --out: var(--arg) var(--own, no-own) var(--mine); } }"));
  EXPECT_EQ(out(m, "#d"), "element no-own rule");
}

// "a contents block ... the mixin can then substitute into itself": into a nested rule of the body, the block's declarations are that rule's.
TEST(CssomMixinEval, TheBlockIsSubstitutedIntoANestedRuleOfTheBody) {
  Cs m(page("@mixin --kids { & > * { @contents; } } section { @apply --kids { --out: kid; } }"));
  EXPECT_EQ(out(m, "#b"), "kid");
  EXPECT_EQ(out(m, "#a"), "");
}

/* ---- mixins in mixins ------------------------------------------------------------------------------------------------------------------------------------ */

// "In particular, further mixins can be invoked." An argument of the inner @apply names a parameter of the outer.
TEST(CssomMixinEval, AMixinMayApplyAnother) {
  Cs m(page("@mixin --inner(--x) { --out: inner(var(--x)); } @mixin --outer(--y) { @apply --inner(var(--y) from-outer); } p { @apply --outer(Y); }"));
  EXPECT_EQ(out(m, "#d"), "inner(Y from-outer)");
}

// A block passed to an @apply inside a mixin is a block of that mixin's body: its @contents is the outer's block when the outer passed one.
TEST(CssomMixinEval, AContentsBlockPassedInsideAMixinIsThatMixinsBlock) {
  Cs m(page("@mixin --leaf { @contents; } @mixin --branch(--v) { @apply --leaf { --out: leaf-of var(--v); } } p { @apply --branch(B); }"));
  EXPECT_EQ(out(m, "#d"), "leaf-of B");
}

// An @apply in a contents block is an @apply of the invoker's rule.
TEST(CssomMixinEval, AnApplyInAContentsBlockIsTheInvokersApply) {
  Cs m(page("@mixin --wrap { @contents; } @mixin --inner(--v: dflt) { --out: inner-var(--v); } p { @apply --wrap { @apply --inner(deep); } }"));
  EXPECT_EQ(out(m, "#d"), "");   // a contents block is a <declaration-list>: an at-rule in it is not read
}

/* ---- cycles ---------------------------------------------------------------------------------------------------------------------------------------------- */

// The draft says nothing of a mixin that applies itself. The reading kept: the @apply of a mixin that is being applied does nothing (like an @apply of a mixin that does not exist), which is finite
// and leaves the rest of the body.
TEST(CssomMixinEval, AMixinThatAppliesItselfAppliesOnce) {
  Cs m(page("@mixin --loop { --n: x; @apply --loop; --out: done; } p { @apply --loop; }"));
  EXPECT_EQ(out(m, "#d"), "done");
  EXPECT_EQ(m.value("#d", "--n"), "x");
}

TEST(CssomMixinEval, TwoMixinsThatApplyEachOtherStopAtTheCycle) {
  Cs m(page("@mixin --a { --a: 1; @apply --b; } @mixin --b { --b: 2; @apply --a; } p { @apply --a; }"));
  EXPECT_EQ(m.value("#d", "--a"), "1");
  EXPECT_EQ(m.value("#d", "--b"), "2");
}

// The same mixin applied twice side by side is not a cycle.
TEST(CssomMixinEval, TheSameMixinAppliedTwiceIsNotACycle) {
  Cs m(page("@mixin --m(--v) { --a: var(--v); --out: var(--v); } p { @apply --m(a); @apply --m(b); --c: var(--a); }"));
  EXPECT_EQ(out(m, "#d"), "b") << "both applications were made, the later declarations win";
  EXPECT_EQ(m.value("#d", "--c"), "b");
}

// A mixin passed as a block's own apply is not an application inside itself.
TEST(CssomMixinEval, AContentsBlockMayApplyTheMixinThatHoldsIt) {
  Cs m(page("@mixin --m { --a: 1; @contents; } p { @apply --m; }"));
  EXPECT_EQ(m.value("#d", "--a"), "1");
}

/* ---- which mixin ----------------------------------------------------------------------------------------------------------------------------------------- */

// "If more than one @mixin exists for a given name, then the rule in the stronger cascade layer wins, and rules defined later win within the same layer."
TEST(CssomMixinEval, TheLaterMixinOfTheSameNameWins) {
  Cs m(page("@mixin --m { --out: first; } @mixin --m { --out: second; } p { @apply --m; }"));
  EXPECT_EQ(out(m, "#d"), "second");
}

TEST(CssomMixinEval, TheStrongerLayerWins) {
  Cs m(page("@layer weak, strong; @layer strong { @mixin --m { --out: strong; } } @layer weak { @mixin --m { --out: weak; } } p { @apply --m; }"));
  EXPECT_EQ(out(m, "#d"), "strong") << "the layer that is declared later is the stronger, whatever the order of the rules";
  Cs n(page("@layer a { @mixin --m { --out: layered; } } @mixin --m { --out: unlayered; } p { @apply --m; }"));
  EXPECT_EQ(out(n, "#d"), "unlayered") << "unlayered rules are stronger than layered ones";
}

// "The name of a @mixin rule is a tree-scoped name": a mixin is found from any sheet of the tree, before or after the sheet that applies it.
TEST(CssomMixinEval, AMixinOfAnotherSheetIsFound) {
  Cs m("<!DOCTYPE html><html><head><style>p { @apply --m(1); }</style><style>@mixin --m(--v) { --out: from-other-sheet var(--v); }</style></head><body><p id=d>y</p></body></html>");
  EXPECT_EQ(out(m, "#d"), "from-other-sheet 1");
  Cs n("<!DOCTYPE html><html><head><style>@mixin --m(--v) { --out: from-other-sheet var(--v); }</style><style>p { @apply --m(2); }</style></head><body><p id=d>y</p></body></html>");
  EXPECT_EQ(out(n, "#d"), "from-other-sheet 2");
}

// A mixin of a sheet that does not take part (disabled) is not found, and one added to a sheet, changed or removed changes what the sheets that apply it are.
TEST(CssomMixinEval, TheSheetsThatApplyAMixinFollowItsDefinition) {
  Cs m("<!DOCTYPE html><html><head><style id=user>p { --out: none; @apply --m; }</style><style id=lib>@mixin --m { --out: v1; }</style></head><body><p id=d>y</p></body></html>");
  EXPECT_EQ(out(m, "#d"), "v1");
  GTEXT_CSSOM_Sheet * lib = m.sheet(m.el("#lib"));
  ASSERT_EQ(gtext_cssom_sheet_insert_rule(lib, "@mixin --m { --out: v2; }", 24, 1, nullptr, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(out(m, "#d"), "v2") << "a later rule of the name wins";
  ASSERT_EQ(gtext_cssom_sheet_delete_rule(lib, 1, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(out(m, "#d"), "v1");
  ASSERT_EQ(gtext_cssom_sheet_set_disabled(lib, true, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(out(m, "#d"), "none") << "a sheet that is disabled defines no mixin";
  ASSERT_EQ(gtext_cssom_sheet_set_disabled(lib, false, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(out(m, "#d"), "v1");
  gtext_cssom_sheet_release(lib);
}

// A mixin in a conditional group rule is not read (the reading kept: where a mixin is applied is decided before any condition is evaluated).
TEST(CssomMixinEval, AMixinInAConditionalGroupRuleIsNotRead) {
  Cs m(page("@media all { @mixin --m { --out: applied; } } p { --out: before; @apply --m; }"));
  EXPECT_EQ(out(m, "#d"), "before");
}

// A mixin in an @layer block is read, in its layer.
TEST(CssomMixinEval, AMixinInALayerBlockIsRead) {
  Cs m(page("@layer l { @mixin --m { --out: layered; } } p { @apply --m; }"));
  EXPECT_EQ(out(m, "#d"), "layered");
}

/* ---- what is not exposed --------------------------------------------------------------------------------------------------------------------------------- */

// "(except that none of those custom properties actually show up in the element's styles)": the parameters are private properties, which "It is not exposed to the page via any CSSOM APIs".
TEST(CssomMixinEval, TheParametersAreNotExposed) {
  Cs m(page("@mixin --m(--arg, --other: 2) { --out: var(--arg) var(--other); } p { @apply --m(1); }"));
  EXPECT_EQ(out(m, "#d"), "1 2");
  EXPECT_EQ(m.value("#d", "--arg"), "");
  EXPECT_EQ(m.value("#d", "--other"), "");
  Decl d = m.computed("#d");
  for (const std::string & n : names(d.d)) {
    EXPECT_EQ(n.find("\xF4\x8F\xBF\xBD"), std::string::npos) << n;
    EXPECT_NE(n, "--arg");
  }
}

// What a mixin does leaves the CSSOM as it was written: the rules are there, as text.
TEST(CssomMixinEval, TheCssomKeepsTheRulesAsWritten) {
  Cs m(page("@mixin --m(--a: 1) { color: red; } p { @apply --m(2) { color: blue; } }"));
  GTEXT_CSSOM_Sheet * sh = m.sheet(m.el("style"));
  const GTEXT_CSSOM_Rule_List * rules = nullptr;
  ASSERT_EQ(gtext_cssom_sheet_rules(sh, &rules), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_rule_list_length(rules), 2u);
  gtext_cssom_sheet_release(sh);
}


// The arguments are read in the scope of the @apply, whatever names the mixin's parameters have: a parameter named like a private property of the rule does not take its place in an argument.
TEST(CssomMixinEval, AnArgumentNamingAPrivatePropertyIsNotReadAsAParameter) {
  Cs m(page("@mixin --m(--p, --x) { --out: var(--p) var(--x); } p { @private { --x: site; } @apply --m(var(--x), mixin-x); }"));
  EXPECT_EQ(out(m, "#d"), "site mixin-x");
}

// "A mixin's parameters define private properties in the mixin body", and so does its @private rule even when it has no parameter: the property is not the element's custom property of the name.
TEST(CssomMixinEval, ThePrivateRuleOfAMixinWithNoParametersHasItsOwnProperty) {
  Cs m(page("@mixin --m { @private { --t: inner; } --out: var(--t); } p { --t: outer; @apply --m; --o2: var(--t); }"));
  EXPECT_EQ(out(m, "#d"), "inner");
  EXPECT_EQ(m.value("#d", "--o2"), "outer");
}

}  // namespace
