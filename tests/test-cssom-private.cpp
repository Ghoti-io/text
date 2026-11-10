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
 * Private custom properties (css_cssom_computed.h): the `@private` rule and a `@property` rule inside it, as the pinned CSS Functions and Mixins 1 defines them ("Private Custom
 * Properties: the @private rule", "Registering Private Properties"). Each test quotes the sentence it holds the library to. Neither Chromium 156.0.8078.47 nor Firefox 158.0.1
 * implements the rule in a stable channel (a style rule that holds one has no `@private` in its `cssRules` and the properties are plain custom properties), so every expectation here is the
 * served text's.
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

/// A custom property of `selector` as `--out` reads it, with the name of the property that was asked ("" is the guaranteed-invalid value, which has no text).
std::string out(const Cs & m, const char * selector, const char * property = "--out") {
  return m.value(selector, property);
}

/* ---- the draft's first example: a private property is not the custom property of the same name ------------------------------------------------------------------ */

// "every style rule is referring to the same custom property": the example without @private, as the control of the one with it.
TEST(CssomPrivate, WithoutPrivateEveryRuleSharesTheCustomProperty) {
  Cs m(page("section { --size: 2px; &, & > *, & > * > * { --out: var(--size); } } section > h1 { --size: 4px; } section > h1 > small { --size: 3px; }"));
  EXPECT_EQ(out(m, "#a"), "2px");
  EXPECT_EQ(out(m, "#b"), "4px");
  EXPECT_EQ(out(m, "#c"), "3px") << "\"As a result, the elements will have three different 'border-width's.\"";
}

// "If we changed that to use a private property in the first rule [...] Within the first section rule and its nested rules, --size is a private property, and the var(--size) refers to the
// private property. In the later, non-nested rules, --size is a normal custom property, independent of the private property from the first rule, so setting it to other values won't affect
// the first usage. [...] All three elements end up with identical 'border-width' values of 2px."
TEST(CssomPrivate, APrivatePropertyIsNotAffectedByTheCustomPropertyOfTheSameName) {
  Cs m(page("section { @private { --size: 2px; } &, & > *, & > * > * { --out: var(--size); } } section > h1 { --size: 4px; } section > h1 > small { --size: 3px; }"));
  EXPECT_EQ(out(m, "#a"), "2px");
  EXPECT_EQ(out(m, "#b"), "2px");
  EXPECT_EQ(out(m, "#c"), "2px");
  EXPECT_EQ(m.value("#b", "--size"), "4px") << "the custom property of the later rule is its own";
  EXPECT_EQ(m.value("#c", "--size"), "3px");
}

// The example as the draft writes it, with the property the draft names.
TEST(CssomPrivate, TheDraftsExampleResolvesBorderWidthTo2px) {
  Cs m(page("section { @private { --size: 2px; } &, & > *, & > * > * { border-style: solid; border-width: var(--size); } } section > h1 { --size: 4px; } section > h1 > small { --size: 3px; }"));
  EXPECT_EQ(m.value("#a", "border-top-width"), "2px");
  EXPECT_EQ(m.value("#b", "border-top-width"), "2px");
  EXPECT_EQ(m.value("#c", "border-top-width"), "2px");
}

// "they're still evaluated as normal for properties, which can interact with other properties set in other style rules": an unregistered private value is the tokens, resolved at each
// element ("Which will give three different 'border-width' values: 2px, 4px, and 3px").
TEST(CssomPrivate, AnUnregisteredPrivateValueIsResolvedAgainstEachElement) {
  Cs m(page("section { font-size: 10px; @private { --size: .2em; } &, & > *, & > * > * { border-style: solid; border-width: var(--size); } } section > h1 { font-size: 20px; } section > h1 > small { font-size: 15px; }"));
  EXPECT_EQ(m.value("#a", "border-top-width"), "2px");
  EXPECT_EQ(m.value("#b", "border-top-width"), "4px");
  EXPECT_EQ(m.value("#c", "border-top-width"), "3px");
}

// "Because private properties generally can't be registered custom properties [...] This can be resolved with a custom function": the typed argument resolves the em against the element.
TEST(CssomPrivate, ACustomFunctionCanResolveThePrivateValueAtTheRuleThatDeclaresIt) {
  Cs m(page("@function --as-length(--x <length>) { result: var(--x); } section { font-size: 10px; @private { --size: --as-length(.2em); } &, & > *, & > * > * { --out: var(--size); } }"
            " section > h1 { font-size: 20px; } section > h1 > small { font-size: 15px; }"));
  EXPECT_EQ(out(m, "#a"), "2px");
  EXPECT_EQ(out(m, "#b"), "2px") << "\"This then inherits as the px value to its descendants, and is unaffected by changes in font-size.\"";
  EXPECT_EQ(out(m, "#c"), "2px");
}

/* ---- the definition ---------------------------------------------------------------------------------------------------------------------------------------------- */

// "It is not exposed to the page via any CSSOM APIs. (For example, CSSStyleDeclaration.getPropertyValue() can't get its value, the CSSStyleDeclaration enumeration won't contain them, etc.)"
TEST(CssomPrivate, ThePrivatePropertyIsNotExposedToGetComputedStyle) {
  Cs m(page("section { @private { --size: 2px; --other: 3px; } --out: var(--size) var(--other); }"));
  EXPECT_EQ(out(m, "#a"), "2px 3px");
  EXPECT_EQ(m.value("#a", "--size"), "") << "getPropertyValue() can't get its value";
  EXPECT_EQ(m.value("#a", "--other"), "");
  Decl d = m.computed("#a");
  bool saw_out = false;
  for (const std::string & n : names(d.d)) {
    EXPECT_NE(n, "--size");
    EXPECT_NE(n, "--other");
    EXPECT_EQ(n.find("\xF4\x8F\xBF\xBD"), std::string::npos) << "the enumeration won't contain the private properties";
    saw_out = saw_out || n == "--out";
  }
  EXPECT_TRUE(saw_out) << "the custom property is there";
  EXPECT_EQ(length(d.d), names(d.d).size());
}

// The count and the enumeration agree whatever is private: a document with private properties lists exactly the custom properties that are not.
TEST(CssomPrivate, TheEnumerationOfAStyleWithPrivatePropertiesListsTheOthersInOrder) {
  Cs m(page("section { --a: 1; --z: 2; @private { --b: 3; --y: 4; @property --c <length>: 1px; } --m: var(--b) var(--y) var(--c); }"));
  Decl d = m.computed("#a");
  std::vector<std::string> customs;
  for (const std::string & n : names(d.d)) {
    if (n.rfind("--", 0) == 0) {
      customs.push_back(n);
    }
  }
  EXPECT_EQ(customs, (std::vector<std::string>{"--a", "--m", "--z"}));
  Cs plain(page("section { --a: 1; --z: 2; --m: 3; }"));
  EXPECT_EQ(length(m.computed("#a").d), length(plain.computed("#a").d)) << "the length counts no private property";
}

// "Two private properties with the same name are considered the same property if and only if they have the same nearest style rule ancestor that has their name in its private property
// names." Two rules that both have the name are two properties, on one element at the same time.
TEST(CssomPrivate, TwoRulesThatHaveTheNameHaveTwoProperties) {
  Cs m(page("section { @private { --x: 1; } --out1: var(--x); } #a { @private { --x: 2; } --out2: var(--x); }"));
  EXPECT_EQ(m.value("#a", "--out1"), "1");
  EXPECT_EQ(m.value("#a", "--out2"), "2") << "\"They can exist together on a single element at the same time, carry their own distinct values\"";
}

// ... and two style sheets are two sets of rules.
TEST(CssomPrivate, TwoSheetsThatHaveTheNameHaveTwoProperties) {
  Cs m("<!DOCTYPE html><html><head><style>section { @private { --x: 1; } --out1: var(--x); }</style><style>section { @private { --x: 2; } --out2: var(--x); }</style></head>"
       "<body><section id=a><p id=b>x</p></section></body></html>");
  EXPECT_EQ(m.value("#a", "--out1"), "1");
  EXPECT_EQ(m.value("#a", "--out2"), "2");
  EXPECT_EQ(m.value("#b", "--out1"), "1") << "an unregistered custom property inherits, whatever its name";
}

// "any custom property with that name is actually a private property" "Within the style rule (and any nested rules)": a nested rule that sets the name sets the private property of the rule that
// has it, and the rule that has its own has its own.
TEST(CssomPrivate, ANestedRuleSetsAndReadsThePropertyOfTheNearestRuleThatHasTheName) {
  Cs m(page("section { @private { --x: 1; } --out: var(--x); & > h1 { --x: 5; --out: var(--x); } & > p { @private { --x: 9; } --out: var(--x); } }"));
  EXPECT_EQ(out(m, "#a"), "1");
  EXPECT_EQ(out(m, "#b"), "5") << "the rule nested in the one that has the name sets that property";
  EXPECT_EQ(out(m, "#d"), "9") << "a nested rule that has the name in its own @private has its own property";
  EXPECT_EQ(m.value("#b", "--x"), "") << "and none of them is the custom property";
}

// "any reference to a custom property of that name (for example, with a var()) is a reference to the private property": inside a fallback, a nested function and an argument.
TEST(CssomPrivate, EveryReferenceInTheRuleIsAReferenceToThePrivateProperty) {
  Cs m(page("section { @private { --x: 1px; } --o1: var(--nosuch, var(--x)); --o2: calc(var(--x) * 2); --o3: var( --x ); --o4: var(--x, red); }"
            " @function --id(--a) { result: var(--a); } section { --o5: --id(var(--x)); }"));
  EXPECT_EQ(m.value("#a", "--o1"), "1px");
  EXPECT_EQ(m.value("#a", "--o2"), "calc(1px * 2)");
  EXPECT_EQ(m.value("#a", "--o3"), "1px");
  EXPECT_EQ(m.value("#a", "--o4"), "1px");
  EXPECT_EQ(m.value("#a", "--o5"), "") << "that second rule is another rule: its --x is the custom property, which is not set";
}

TEST(CssomPrivate, AnArgumentOfACustomFunctionIsSubstitutedInTheRuleThatHasThePrivateProperty) {
  Cs m(page("@function --id(--a) { result: var(--a); } section { @private { --x: 7px; } --out: --id(var(--x)); }"));
  EXPECT_EQ(out(m, "#a"), "7px");
}

// The `style()` of an `if()` reads the computed value of the property it names: of the private one.
TEST(CssomPrivate, AStyleQueryInAnIfNamesThePrivateProperty) {
  Cs m(page("section { @private { --x: 1; } --out: if(style(--x: 1): Y; else: N); --out2: if(style(--x: 2): Y; else: N); }"));
  EXPECT_EQ(out(m, "#a"), "Y");
  EXPECT_EQ(m.value("#a", "--out2"), "N");
}

// "(including those nested in nested group rules)": the names of the custom properties in a group rule of an @private rule are private names whether the condition of the group holds or not.
TEST(CssomPrivate, TheNamesInAGroupRuleOfAPrivateRuleAreThePrivateNamesWhateverItsCondition) {
  Cs m(page("section { @private { @media (max-width: 0px) { --x: 1; } } --x: 5; --out: var(--x, none); }"));
  EXPECT_EQ(out(m, "#a"), "5") << "the group does not apply, the name is private, and the declaration of the rule is the private property's";
  EXPECT_EQ(m.value("#a", "--x"), "") << "it is not the custom property";
}

// `inherit()` takes the parent's value of the property it names: of the private one.
TEST(CssomPrivate, TheNameInInheritIsThePrivateProperty) {
  Cs m(page("section { @private { --x: 1; } & > h1 { --out: inherit(--x, none); } }"));
  EXPECT_EQ(out(m, "#b"), "1");
  Cs plain(page("section { --x: 1; & > h1 { --out: inherit(--x, none); } }"));
  EXPECT_EQ(out(plain, "#b"), "1") << "as the custom property it is without @private";
}

// Names are case-sensitive, as every custom property name is.
TEST(CssomPrivate, ANameThatDiffersInCaseIsAnotherProperty) {
  Cs m(page("section { @private { --Size: 1px; } --out: var(--size, none); --out2: var(--Size); }"));
  EXPECT_EQ(out(m, "#a"), "none");
  EXPECT_EQ(m.value("#a", "--out2"), "1px");
}

// Declarations of the rule and of its @private rules are in one order, the order they are written in: the later one wins, as it does for the declarations of one rule.
TEST(CssomPrivate, ThePrivateRuleAndTheDeclarationsOfTheRuleAreOrderedByTheirPlace) {
  Cs m(page("section { --x: 1; @private { --x: 2; } --out: var(--x); } #d { --x: 1; } p { @private { --x: 3; } --x: 4; --out: var(--x); }"));
  EXPECT_EQ(out(m, "#a"), "2");
  EXPECT_EQ(out(m, "#d"), "4") << "the declaration after the @private rule wins";
}

// A private property inherits as any unregistered custom property: the value is the tokens, substituted where it is declared.
TEST(CssomPrivate, APrivatePropertyInheritsToTheElementsOfTheNestedRules) {
  Cs m(page("section { @private { --x: 1; } & > * { --out: var(--x); } }"));
  EXPECT_EQ(out(m, "#b"), "1");
  EXPECT_EQ(out(m, "#d"), "1");
  EXPECT_EQ(out(m, "#a"), "") << "section itself is not matched by the nested rule";
}

// A private property can be reset by the CSS-wide keywords as a custom property can.
TEST(CssomPrivate, TheWideKeywordsApplyToAPrivatePropertyInANestedRule) {
  Cs m(page("section { @private { --x: 1; } --out: var(--x); & > h1 { --x: initial; --out: var(--x, gone); } & > p { --x: inherit; --out: var(--x); } }"));
  EXPECT_EQ(out(m, "#a"), "1");
  EXPECT_EQ(out(m, "#b"), "gone") << "initial is the guaranteed-invalid value";
  EXPECT_EQ(out(m, "#d"), "1") << "inherit takes the parent's private property";
}

/* ---- what @private takes and where it is valid ------------------------------------------------------------------------------------------------------------------ */

// "A @private rule can only contain custom properties, nested group rules [...] and @property rules. Any other contents are invalid and will be ignored, but do not invalidate the @private
// rule itself."
TEST(CssomPrivate, OtherContentsOfAPrivateRuleAreIgnoredAndTheRuleStands) {
  Cs m(page("section { @private { color: red; --x: 1; & > h1 { --y: 2; } } --out: var(--x); } section > h1 { --out: var(--y, none); }"));
  EXPECT_EQ(out(m, "#a"), "1") << "the custom property in it still counts";
  EXPECT_NE(m.value("#a", "color"), "rgb(255, 0, 0)") << "a property is not a custom property";
  EXPECT_EQ(out(m, "#b"), "none") << "a rule in it is nothing";
}

// "The @private rule is only valid nested as a child of a style rule or a nested group rule. In any other context, it is invalid."
TEST(CssomPrivate, APrivateRuleAtTheTopOfASheetIsNothing) {
  Cs m(page("@private { --x: 1; } @media all { @private { --x: 2; } } section { --out: var(--x, none); }"));
  EXPECT_EQ(out(m, "#a"), "none");
}

// "the set of custom properties they contain define the style rule's private property names" and "(including those nested in nested group rules)": a group rule in the style rule that holds an
// @private rule, and a group rule in the @private rule.
TEST(CssomPrivate, ThePrivateRuleOfAGroupRuleInTheStyleRuleHasTheNamesOfTheStyleRule) {
  Cs m(page("section { @media (min-width: 1px) { @private { --x: 1; } } --out: var(--x, none); } p { @media (max-width: 0px) { @private { --y: 1; } } --y: 5; --out: var(--y, none); }"));
  EXPECT_EQ(out(m, "#a"), "1");
  EXPECT_EQ(out(m, "#d"), "5") << "the media query is false: the declaration of --y inside it is nothing, and --y is private in the rule, so the 5 is the private property";
  EXPECT_EQ(m.value("#d", "--y"), "") << "and not the custom property";
}

TEST(CssomPrivate, AGroupRuleInAPrivateRuleAppliesWhereItsConditionHolds) {
  Cs m(page("section { @private { @media (min-width: 1px) { --x: 1; } @media (max-width: 0px) { --x: 2; } @supports (display: nothing-at-all) { --y: 3; } } --out: var(--x) var(--y, none); }"));
  EXPECT_EQ(out(m, "#a"), "1 none") << "\"nested group rules (in which case it passes this same restriction to their contents)\"";
}

// The rule of a mixin is not a style rule: its @private rule is the mixin's (which this library does not evaluate).
TEST(CssomPrivate, APrivateRuleInAMixinBodyIsNotAppliedToTheRulesAround) {
  Cs m(page("@mixin --m() { @private { --x: 1; } } section { --out: var(--x, none); }"));
  EXPECT_EQ(out(m, "#a"), "none");
}

/* ---- registering private properties ------------------------------------------------------------------------------------------------------------------------------ */

// "A @property rule inside of a @private rule defines a private property name for the parent style rule, just like using the private property normally does. Its custom property registration
// only applies to that private property, not any other of the same name."
TEST(CssomPrivate, APropertyRuleInAPrivateRuleRegistersThePrivateProperty) {
  Cs m(page("section { font-size: 10px; @private { @property --n <length>: 3px; } --out: var(--n) var(--n); } section > h1 { --n: 2em; --out2: var(--n); font-size: 5px; }"));
  EXPECT_EQ(out(m, "#a"), "3px 3px") << "the registration gives the private property its initial value";
  EXPECT_EQ(m.value("#b", "--out2"), "2em") << "the custom property of the same name is not registered, so 2em is not computed";
  EXPECT_EQ(m.value("#b", "--n"), "2em");
}

TEST(CssomPrivate, TheRegistrationOfAPrivatePropertyTypesWhatIsSetAndWhatInherits) {
  Cs m(page("section { font-size: 10px; @private { @property --n { syntax: '<length>'; inherits: true; initial-value: 0px; } } & { --n: 2em; --out: var(--n); } & > h1 { font-size: 30px; --out: var(--n); } & > p { --n: red; --out: var(--n); } }"));
  EXPECT_EQ(out(m, "#a"), "20px");
  EXPECT_EQ(out(m, "#b"), "20px") << "computed where it is declared, and inherited as the length";
  EXPECT_EQ(out(m, "#d"), "0px") << "red is not a <length>: invalid at computed-value time, so the initial value";
}

TEST(CssomPrivate, ARegistrationOfAPrivatePropertyThatDoesNotInheritResetsAtTheChild) {
  Cs m(page("section { @private { @property --n { syntax: '<number>'; inherits: false; initial-value: 7; } --m: 3; } & { --n: 1; --out: var(--n); } & > h1 { --out: var(--n); } }"));
  EXPECT_EQ(out(m, "#a"), "1");
  EXPECT_EQ(out(m, "#b"), "7");
}

// "A @property rule can also be used directly inside of a @private rule": in the rule's own group rules the same holds, and the registration is that of the rule that has it.
TEST(CssomPrivate, GlobalRegistrationsDoNotApplyToAPrivateProperty) {
  Cs m(page("@property --n { syntax: '<length>'; inherits: false; initial-value: 5px; } section { @private { --n: red; } --out: var(--n); } p { --out: var(--n); }"));
  EXPECT_EQ(out(m, "#a"), "red") << "\"global property registrations do not apply to private properties\"";
  EXPECT_EQ(out(m, "#d"), "5px") << "and the registration still applies to the custom property";
}

TEST(CssomPrivate, AnUnregisteredPrivatePropertyAcceptsAnyValue) {
  Cs m(page("section { @private { --n: anything at all; @property --m <color>: blue; } --out: var(--n) | var(--m); }"));
  EXPECT_EQ(out(m, "#a"), "anything at all | rgb(0, 0, 255)") << "\"Just like normal custom properties, private properties are, by default, unregistered\"";
}

// The shorthand form of the draft's example, with the typed parameters: "--two: 2; /* ditto for --two */ --three: 3; /* this is valid! */".
TEST(CssomPrivate, TheDraftsExampleOfRegisteredAndUnregisteredPrivateProperties) {
  Cs m(page("section { @private { @property --two <color>: blue; --three: green; } --two: 2; --three: 3; --out: var(--two) var(--three); }"));
  EXPECT_EQ(out(m, "#a"), "rgb(0, 0, 255) 3") << "2 is not a <color>: the property is the initial value; 3 is valid for the unregistered --three";
}

// A rule that is not valid registers nothing; a descriptor that is not valid is ignored and the rule stands; of two rules for one name in one @private rule the later wins.
TEST(CssomPrivate, AnInvalidPropertyRuleRegistersNothingAndTheLaterOfTwoWins) {
  Cs m(page("section { @private { @property --a { syntax: '<length>'; initial-value: 1em; } @property --b <length>: 1px; @property --c { initial-value: 3px; syntax: 'nonsense<'; } @property --b <length>: 2px; }"
            " font-size: 10px; --out: var(--b) var(--c) var(--a, none); }"));
  EXPECT_EQ(out(m, "#a"), "2px 3px none") << "--c has no valid syntax: the descriptor is ignored and the rule registers the universal syntax with its initial value; --a is not computationally independent: no rule";
}

// A registration of the private property of one rule is not the one of another rule that has the name.
TEST(CssomPrivate, TheRegistrationBelongsToTheRuleThatHasIt) {
  Cs m(page("section { @private { @property --n <length>: 1px; } --out: var(--n); } #a { @private { --n: red; } --out2: var(--n); }"));
  EXPECT_EQ(out(m, "#a"), "1px");
  EXPECT_EQ(m.value("#a", "--out2"), "red");
}

/* ---- the rest of the cascade --------------------------------------------------------------------------------------------------------------------------------------- */

// "(if it doesn't override them with its own mixin parameters or private properties)", and for a function: "Issue: What about custom functions? If they refer to a custom property not defined
// in the function, should they see an outer private property?" The pinned text leaves it open ("I'm inclined to say yes"); the reading kept is the conservative one, a function sees the custom
// properties of the element and no private property.
TEST(CssomPrivate, ACustomFunctionDoesNotSeeAPrivatePropertyOfTheRuleThatCallsIt) {
  Cs m(page("@function --f() { result: var(--x, none); } section { @private { --x: 1; } --out: --f(); } p { --x: 2; --out: --f(); }"));
  EXPECT_EQ(out(m, "#a"), "none");
  EXPECT_EQ(out(m, "#d"), "2") << "it sees the custom property";
}

// The cascade orders the declarations of a private property as it does any: by specificity and place.
TEST(CssomPrivate, ThePrivateDeclarationsAreOrderedByTheSpecificityOfTheirRule) {
  Cs m(page("#a { @private { --x: 1; } --out: var(--x); & { --x: 2; } } section { --out2: var(--x, none); }"));
  EXPECT_EQ(out(m, "#a"), "2");
  EXPECT_EQ(m.value("#a", "--out2"), "none") << "another rule, another property";
}

/* ---- what the computed style shows ------------------------------------------------------------------------------------------------------------------------------- */

// The hidden names are custom properties of a computed style that its listing does not show (css_private_internal.h): registered and not, before and after the others in the order of the names.
TEST(CssPrivateStyle, TheComputedStyleListsAndFindsNoHiddenName) {
  const std::string hid = std::string("--\xF4\x8F\xBF\xBD") + "p1_0_";
  cust::World w;
  cust::Reg r1, r2, r3, r4;
  r1.name = "--aa";
  r1.syntax = "<number>";
  r1.initial = "1";
  r2.name = hid + "--reg";
  r2.syntax = "<number>";
  r2.initial = "2";
  r3.name = "--zz";
  r3.syntax = "<number>";
  r3.initial = "3";
  r4.name = "--0";
  r4.syntax = "<number>";
  r4.initial = "4";
  ASSERT_EQ(w.build({r1, r2, r3, r4}), GTEXT_CSS_OK);
  cust::Computed c;
  cust::compute(w, c, "--m: 5; --" + std::string("\xF4\x8F\xBF\xBD") + "p1_0_--x: 6; --" + std::string("\xF4\x8F\xBF\xBD") + "p1_0_--y: 7; --\x7f: 8; --n: 9");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  std::vector<std::string> names;
  const size_t count = gtext_css_computed_custom_count(c.s);
  for (size_t i = 0; i < count; i++) {
    GTEXT_CSS_String n;
    ASSERT_TRUE(gtext_css_computed_custom_at(c.s, i, &n, nullptr)) << i;
    names.emplace_back(n.data, n.length);
  }
  GTEXT_CSS_String past;
  EXPECT_FALSE(gtext_css_computed_custom_at(c.s, count, &past, nullptr)) << "no index past the others";
  EXPECT_EQ(names, (std::vector<std::string>{"--0", "--aa", "--m", "--n", "--zz"})) << "in the order of the names, and none that is private";
  EXPECT_EQ(c.custom("--m"), "5");
  EXPECT_EQ(c.custom("--" + std::string("\xF4\x8F\xBF\xBD") + "p1_0_--x"), "<gi>") << "found by no name";
  EXPECT_EQ(c.custom(hid + "--reg"), "<gi>") << "a registered one included";
}

/* ---- the sheets change ---------------------------------------------------------------------------------------------------------------------------------------- */

// A rule inserted into a sheet with private properties (and one deleted) changes what is read at the next read, the registrations of the private properties with it.
TEST(CssomPrivate, ARuleWithPrivatePropertiesInsertedIntoASheetAppliesAndOneDeletedDoesNot) {
  Cs m(page("section { --out: plain; }"));
  EXPECT_EQ(out(m, "#a"), "plain");
  GTEXT_CSSOM_Sheet * sh = m.sheet(m.el("style"));
  const std::string rule = "section { font-size: 10px; @private { @property --n <length>: 3px; --x: 4; } --out: var(--x) var(--n); }";
  ASSERT_EQ(gtext_cssom_sheet_insert_rule(sh, rule.data(), rule.size(), 1, nullptr, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(out(m, "#a"), "4 3px");
  ASSERT_EQ(gtext_cssom_sheet_delete_rule(sh, 1, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(out(m, "#a"), "plain");
  gtext_cssom_sheet_release(sh);
}

// A rule of the sheet that was changed after it was read (its selector was set) is read again from its text, `@private` rule included.
TEST(CssomPrivate, AStyleRuleThatWasChangedKeepsItsPrivateProperties) {
  Cs m(page("section { @private { --x: 5; @property --n <length>: 2px; } --out2: var(--x) var(--n); } p { --out: other }"));
  EXPECT_EQ(out(m, "#a", "--out2"), "5 2px");
  GTEXT_CSSOM_Sheet * sh = m.sheet(m.el("style"));
  const GTEXT_CSSOM_Rule_List * rules = nullptr;
  ASSERT_EQ(gtext_cssom_sheet_rules(sh, &rules), GTEXT_CSS_OK);
  GTEXT_CSSOM_Rule * r = gtext_cssom_rule_list_at(rules, 0);
  ASSERT_NE(r, nullptr);
  ASSERT_EQ(gtext_cssom_rule_set_selector_text(r, "p", 1, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(out(m, "#a", "--out2"), "") << "section is not matched by the rule any more";
  EXPECT_EQ(out(m, "#d", "--out2"), "5 2px") << "p is, with the rule's private properties, which are the rule's and not the other rule's";
  EXPECT_EQ(out(m, "#d"), "other");
  gtext_cssom_sheet_release(sh);
}

// A sheet that is disabled takes its private properties with it, and a sheet that is enabled again has them.
TEST(CssomPrivate, ADisabledSheetsPrivatePropertiesAreNotRegistered) {
  Cs m(page("section { font-size: 10px; @private { @property --n <length>: 3px; } --out: var(--n, none); }"));
  EXPECT_EQ(out(m, "#a"), "3px");
  GTEXT_CSSOM_Sheet * sh = m.sheet(m.el("style"));
  ASSERT_EQ(gtext_cssom_sheet_set_disabled(sh, true, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(out(m, "#a"), "");
  ASSERT_EQ(gtext_cssom_sheet_set_disabled(sh, false, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(out(m, "#a"), "3px");
  gtext_cssom_sheet_release(sh);
}

// A sheet with no `@private` rule is not compiled to look for registrations: the property rules read of it are the rules at its top, as before.
TEST(CssomPrivate, ASheetWithNoPrivateRuleIsNotReadForPrivateRegistrations) {
  Cs plain(page("section { --out: 1 } p { color: red }"));
  EXPECT_EQ(out(plain, "#a"), "1");
  EXPECT_EQ(plain.dc().property_collects, 0u);
}

/* ---- the standalone reading ------------------------------------------------------------------------------------------------------------------------------------ */

// gtext_css_at_property_collect_private(): the registrations of a compiled sheet's private properties go into a set that a caller counts and lists as before (the rules at the top of a sheet), and
// the compute context reads them from it.
TEST(CssomPrivate, CollectingTheRegistrationsOfACompiledSheetLeavesThePublicViewOfTheSetAsItWas) {
  GTEXT_CSS_Registry * registry = nullptr;
  ASSERT_EQ(gtext_css_registry_new(nullptr, &registry), GTEXT_CSS_OK);
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  const char * const text = "@property --top <length>: 1px; a { @private { @property --n <length>: 2px; --x: 1; } --y: var(--x) var(--n); }";
  GTEXT_CSS_Tree * tree = nullptr;
  ASSERT_EQ(gtext_css_parse_stylesheet(text, std::strlen(text), &po, &tree, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Sheet * sheet = nullptr;
  GTEXT_CSS_Sheet_Result sr;
  ASSERT_EQ(gtext_css_sheet_compile(tree, registry, nullptr, &sheet, &sr), GTEXT_CSS_OK);
  GTEXT_CSS_At_Property_Options o = gtext_css_at_property_options_default();
  GTEXT_CSS_At_Property_Set * set = nullptr;
  ASSERT_EQ(gtext_css_at_property_set_new(registry, &o, &set), GTEXT_CSS_OK);
  GTEXT_CSS_At_Property_Result r;
  ASSERT_EQ(gtext_css_at_property_collect(set, tree, &r), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_at_property_collect_private(set, sheet, &r), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_at_property_count(set), 1u) << "the set lists the rule at the top of the sheet and no private registration";
  GTEXT_CSS_Custom_Info info;
  EXPECT_TRUE(gtext_css_at_property_find(set, "--top", 5, &info));
  EXPECT_FALSE(gtext_css_at_property_find(set, "--n", 3, &info));
  EXPECT_EQ(gtext_css_at_property_collect_private(nullptr, sheet, &r), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_at_property_collect_private(set, nullptr, &r), GTEXT_CSS_E_INVALID);
  gtext_css_at_property_set_free(set);
  gtext_css_sheet_free(sheet);
  gtext_css_tree_free(tree);
  gtext_css_registry_free(registry);
}

namespace {
GTEXT_Markup_Status append_to_string(void * user, const char * bytes, size_t n) {
  static_cast<std::string *>(user)->append(bytes, n);
  return GTEXT_MARKUP_OK;
}

std::string dump_of(const GTEXT_CSS_Sheet * sheet) {
  std::string out;
  GTEXT_Markup_Sink sink{append_to_string, &out};
  EXPECT_EQ(gtext_css_sheet_dump(sheet, &sink), GTEXT_CSS_OK);
  return out;
}
}  // namespace

// Each compile of a sheet has its own number in the hidden names (two sheets must not share a private property), and a dump of a compiled sheet is for reading and for comparing: it leaves the
// number out, so two compiles of one text dump alike (the fuzz harness of the sheets compares a snapshot with the compile of its text).
TEST(CssomPrivate, TwoCompilesOfOneSheetDumpAlike) {
  GTEXT_CSS_Registry * registry = nullptr;
  ASSERT_EQ(gtext_css_registry_new(nullptr, &registry), GTEXT_CSS_OK);
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  const char * const text = "a { @private { --x: 1; --\\2e y: 2 } --z: var(--x) var(--\\2e y); b { @private { --x: 3 } --w: var(--x) } }";
  GTEXT_CSS_Tree * tree = nullptr;
  ASSERT_EQ(gtext_css_parse_stylesheet(text, std::strlen(text), &po, &tree, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Sheet * one = nullptr;
  GTEXT_CSS_Sheet * two = nullptr;
  GTEXT_CSS_Sheet_Result sr;
  ASSERT_EQ(gtext_css_sheet_compile(tree, registry, nullptr, &one, &sr), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_sheet_compile(tree, registry, nullptr, &two, &sr), GTEXT_CSS_OK);
  const std::string a = dump_of(one);
  EXPECT_EQ(a, dump_of(two));
  EXPECT_NE(a.find("--\xF4\x8F\xBF\xBDp_0_--x: 1"), std::string::npos) << a;
  EXPECT_NE(a.find("var(--\xF4\x8F\xBF\xBDp_0_--x)"), std::string::npos) << a;
  EXPECT_NE(a.find("--\xF4\x8F\xBF\xBDp_1_--x: 3"), std::string::npos) << "the nested rule that has the name has another property";
  gtext_css_sheet_free(one);
  gtext_css_sheet_free(two);
  gtext_css_tree_free(tree);
  gtext_css_registry_free(registry);
}

/* ---- allocation failure ------------------------------------------------------------------------------------------------------------------------------------------ */

std::string everything(Cs & m) {
  return m.value("#a", "--out") + "|" + m.value("#d", "--out") + "|" + m.value("#a", "--x");
}

// Every allocation of compiling a sheet with private properties (the names, the hidden names, the rewritten values and the registrations) and of making the context with them is failed in
// turn: a failed read is an error and leaves the document as it was, the next read with memory gives the healthy answer, and nothing leaks.
TEST(CssomPrivateOom, EveryAllocationOfReadingThePrivatePropertiesIsFailedInTurn) {
  const std::string html = page("section { font-size: 10px; @private { @property --n <length>: 3px; --x: 4; @media (min-width: 1px) { --m: var(--x) } } --out: var(--x) var(--n) var(--m) if(style(--x: 4): Y; else: N);"
                                " & > p { @private { --x: 6; } --out: var(--x) var(--n); } }");
  std::string want;
  {
    Cs m(html);
    want = everything(m);
    ASSERT_EQ(want, "4 3px 4 Y|6 3px|");
  }
  size_t failed = 0;
  for (size_t n = 1; n < 100000; n += n < 80 ? 1 : 29) {
    FaultAllocator fa;
    bool fired = false;
    {
      Cs m(html, nullptr, &fa.alloc);
      GTEXT_CSSOM_Decl * made = nullptr;
      EXPECT_EQ(gtext_cssom_computed_style(m.d(), m.handle("#a"), nullptr, 0, &made), GTEXT_CSS_OK);
      fa.arm(n);
      size_t len = 0;
      const GTEXT_CSS_Status st = gtext_cssom_decl_length(made, &len);
      fa.disarm();
      fired = fa.failed;
      if (st != GTEXT_CSS_OK) {
        failed++;
        EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
      }
      gtext_cssom_decl_release(made);
      EXPECT_EQ(everything(m), want) << "allocation " << n << ": with memory again, the healthy answer";
    }
    EXPECT_TRUE(fa.live.empty()) << "a leak after failing allocation " << n;
    EXPECT_EQ(fa.bad_frees, 0u);
    if (!fired) {
      break;
    }
  }
  EXPECT_GT(failed, 20u) << "the sweep reached the allocations of the reading";
}

}  // namespace
