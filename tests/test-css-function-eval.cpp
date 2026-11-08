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
 * Evaluating a custom function (CSS Functions and Mixins 1, "Using Custom Functions" and "Evaluating Custom Functions", the Editor's Draft pinned in
 * tools/css/DRAFTS_COMMIT): a `<dashed-function>` in a property or a custom property is replaced by the `result` of the function of that name, evaluated with its
 * arguments as a second scope nested in the element's. Each test quotes the sentence of the draft it checks. The examples are the draft's own.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_custom_test_support.h"
#include "markup_test_support.h"

using namespace cust;

namespace {

/// A world whose context has the `@function` rules of `sheet`.
struct FnWorld : World {
  explicit FnWorld(const std::string & sheet, const std::vector<Reg> & regs = {}) {
    function_sheets.push_back(sheet);
    EXPECT_EQ(build(regs), GTEXT_CSS_OK);
  }
};

std::string width_of(World & w, const std::string & decls) {
  Computed c;
  compute(w, c, decls);
  EXPECT_EQ(c.status, GTEXT_CSS_OK) << decls;
  return c.value(w.registry.r, "width");
}

std::string custom_of(World & w, const std::string & decls, const std::string & name) {
  Computed c;
  compute(w, c, decls);
  EXPECT_EQ(c.status, GTEXT_CSS_OK) << decls;
  return c.custom(name);
}

TEST(CssFunctionEval, APlainCallIsReplacedByTheResult) {
  // "A <dashed-function> is a functional notation whose function name starts with two dashes" ... "the result of a custom function evaluation can be substituted
  // into the value of a property with a <dashed-function>."
  FnWorld w("@function --double(--x) { result: calc(var(--x) * 2); }");
  EXPECT_EQ(width_of(w, "width: --double(3px)"), "6px");
}

TEST(CssFunctionEval, TheNegativeExampleOfTheDraft) {
  // The first example of "Defining Custom Functions": `padding: --negative(var(--gap))` and `padding: --negative(1em)`.
  FnWorld w("@function --negative(--value) { result: calc(-1 * var(--value)); }");
  Computed c;
  compute(w, c, "--gap: 4px; margin-left: --negative(var(--gap)); margin-top: --negative(2px)");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.value(w.registry.r, "margin-left"), "-4px");
  EXPECT_EQ(c.value(w.registry.r, "margin-top"), "-2px");
}

TEST(CssFunctionEval, ACallInACustomPropertyIsReplacedToo) {
  // "A <dashed-function> can only be used where var() is allowed", and var() is allowed in a custom property.
  FnWorld w("@function --double(--x) { result: calc(var(--x) * 2); }");
  EXPECT_EQ(custom_of(w, "--r: --double(3px)", "--r"), "calc(3px * 2)");
}

TEST(CssFunctionEval, AParameterShadowsACustomPropertyAndALocalShadowsBoth) {
  // The draft's example in "Arguments & Local Variables": "But function parameters 'shadow' custom properties, and local variables 'shadow' both", `z-index: --add-a-b-c(20, 30); /* 321 */`.
  FnWorld w(
      "@function --add-a-b-c(--b, --c) { --c: 300; result: calc(var(--a) + var(--b) + var(--c)); }");
  Computed c;
  compute(w, c, "--a: 1; --b: 2; --c: 3; z-index: --add-a-b-c(20, 30)");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.value(w.registry.r, "z-index"), "321");
}

TEST(CssFunctionEval, TheCallSitesCustomPropertiesAreVisible) {
  // "Similarly, custom properties are implicitly available": `--double-z() returns <number>` with `--z: 3` gives `z-index: 6`.
  FnWorld w("@function --double-z() returns <number> { result: calc(var(--z) * 2); }");
  Computed c;
  compute(w, c, "--z: 3; z-index: --double-z()");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.value(w.registry.r, "z-index"), "6");
}

TEST(CssFunctionEval, ALocalVariableAndAParameterOfTheCallerAreVisibleToTheCallee) {
  // "A custom function can access local variables and function parameters from functions higher up in the call stack": `z-index: --outer(1); /* 3 */`.
  FnWorld w(
      "@function --outer(--outer-arg) { --outer-local: 2; result: --inner(); }\n"
      "@function --inner() returns <number> { result: calc(var(--outer-arg) + var(--outer-local)); }");
  Computed c;
  compute(w, c, "z-index: --outer(1)");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.value(w.registry.r, "z-index"), "3");
}

TEST(CssFunctionEval, LocalVariablesMayBeDeclaredAfterTheirUse) {
  // "Local variable descriptors may appear before or after they are referenced."
  FnWorld w("@function --circle-area(--r) { result: calc(3 * var(--r2)); --r2: var(--r) * var(--r); }");
  Computed c;
  compute(w, c, "z-index: --circle-area(2)");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.value(w.registry.r, "z-index"), "12");
}

TEST(CssFunctionEval, TheLaterResultWins) {
  // "If this happens, then declarations appearing later win over earlier ones." `result: 3; result: 3.14;` is 3.14.
  FnWorld w("@function --mypi() { result: 3; result: 3.14; }");
  EXPECT_EQ(custom_of(w, "--r: --mypi()", "--r"), "3.14");
}

TEST(CssFunctionEval, AFunctionWithNothingInItIsGuaranteedInvalid) {
  // No parameter and no declaration: there is no array of entries to sort (UBSan: a null pointer passed to qsort), and the function, being valid, has no result.
  FnWorld w("@function --empty() {}");
  EXPECT_EQ(custom_of(w, "--r: --empty()", "--r"), "<gi>");
  EXPECT_EQ(custom_of(w, "--r: --empty(1)", "--r"), "<gi>");
}

TEST(CssFunctionEval, AFunctionWithoutResultIsAlwaysGuaranteedInvalid) {
  // "If no result descriptor exists, the function is valid, but always returns the guaranteed-invalid value."
  FnWorld w("@function --none() { --x: 1; }");
  EXPECT_EQ(custom_of(w, "--r: --none()", "--r"), "<gi>");
  Computed c;
  compute(w, c, "--r: x --none() y");
  EXPECT_EQ(c.custom("--r"), "<gi>");
}

TEST(CssFunctionEval, ANameNoRuleDefinesIsGuaranteedInvalid) {
  // "Let function be the result of dereferencing the dashed function's name ... If no such name exists, return the guaranteed-invalid value."
  FnWorld w("@function --there() { result: 1; }");
  EXPECT_EQ(custom_of(w, "--r: --missing(1px)", "--r"), "<gi>");
  EXPECT_EQ(width_of(w, "width: --missing(1px)"), "auto");
}

TEST(CssFunctionEval, WithoutFunctionRulesACallIsGuaranteedInvalid) {
  World w;
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  EXPECT_EQ(custom_of(w, "--r: --f(1px)", "--r"), "<gi>");
  EXPECT_EQ(width_of(w, "width: --f(1px)"), "auto");
}

TEST(CssFunctionEval, ADefaultFillsAMissingArgument) {
  // "Let arg value be the value of the corresponding argument in arguments, or the guaranteed-invalid value if there is no corresponding argument. ... a value of
  // first-valid(arg value, default value)."
  FnWorld w("@function --f(--a, --b: 5px) { result: var(--a, none) var(--b); }");
  EXPECT_EQ(custom_of(w, "--r: --f(1px)", "--r"), "1px 5px");
  EXPECT_EQ(custom_of(w, "--r: --f(1px, 2px)", "--r"), "1px 2px");
  EXPECT_EQ(custom_of(w, "--r: --f()", "--r"), "none 5px");
}

TEST(CssFunctionEval, AGuaranteedInvalidArgumentSelectsTheDefault) {
  // "Note: This may leave some (or all) arguments as the guaranteed-invalid value, triggering default values (if any)."
  FnWorld w("@function --f(--a: 7px) { result: var(--a); }");
  EXPECT_EQ(custom_of(w, "--r: --f(var(--nothing))", "--r"), "7px");
  EXPECT_EQ(custom_of(w, "--r: --f(var(--nothing, 2px))", "--r"), "2px");
}

TEST(CssFunctionEval, AParameterWithNoDefaultAndNoArgumentIsGuaranteedInvalid) {
  FnWorld w("@function --f(--a) { result: var(--a); }");
  EXPECT_EQ(custom_of(w, "--r: --f()", "--r"), "<gi>");
}

TEST(CssFunctionEval, MoreArgumentsThanParametersIsGuaranteedInvalid) {
  // "If the number of items in arguments is greater than the number of function parameters in custom function, return the guaranteed-invalid value."
  FnWorld w("@function --f(--a, --b) { result: var(--a) var(--b); }");
  EXPECT_EQ(custom_of(w, "--r: --f(1, 2)", "--r"), "1 2");
  EXPECT_EQ(custom_of(w, "--r: --f(1, 2, 3)", "--r"), "<gi>");
}

TEST(CssFunctionEval, ABraceArgumentIsOneArgument) {
  // The draft's example: "A comma-containing value can be passed as a single argument by wrapping the value in curly braces": `width: --max-plus-x({ 1px, 7px, 2px }, 3px); /* 10px */`.
  FnWorld w("@function --max-plus-x(--list, --x) { result: calc(max(var(--list)) + var(--x)); }");
  EXPECT_EQ(width_of(w, "width: --max-plus-x({ 1px, 7px, 2px }, 3px)"), "10px");
}

TEST(CssFunctionEval, AFinalEmptyArgumentIsDroppedAndAnotherCommaPassesOne) {
  // Mixins 1 (pinned 2026-11-07), 2.2: "Each <declaration-value> has leading/trailing whitespace trimmed from it. If the final argument is empty, it's dropped." and "If you
  // do want to pass an empty final argument, put another comma after it, like --func(1, 2,,) (or --solo(,) to pass a single empty argument)."
  FnWorld w("@function --f(--a: 9, --b: 8, --c: 7) { result: [var(--a)][var(--b)][var(--c)]; }");
  EXPECT_EQ(custom_of(w, "--r: --f()", "--r"), "[9][8][7]");
  EXPECT_EQ(custom_of(w, "--r: --f(1,)", "--r"), "[1][8][7]");
  EXPECT_EQ(custom_of(w, "--r: --f(1, 2, )", "--r"), "[1][2][7]");
  EXPECT_EQ(custom_of(w, "--r: --f(1, 2,,)", "--r"), "[1][2][]");
  EXPECT_EQ(custom_of(w, "--r: --f(,)", "--r"), "[][8][7]");
  EXPECT_EQ(custom_of(w, "--r: --f(1,,)", "--r"), "[1][][7]");
  EXPECT_EQ(custom_of(w, "--r: --f(,,3)", "--r"), "[][][3]");
}

TEST(CssFunctionEval, AnEmptyDefaultIsAValueAndAnEmptyArgumentIsNotGuaranteedInvalid) {
  // Mixins 1 (pinned 2026-11-07), 2.1: "Whether a default value is provided or omitted is determined by the presence or absence of the colon; an empty default value (like
  // `@function --x(--arg:){}`) is perfectly valid."
  FnWorld w("@function --f(--a:) { result: [var(--a, none)]; } @function --g(--a) { result: [var(--a, none)]; }");
  EXPECT_EQ(custom_of(w, "--r: --f()", "--r"), "[]");
  EXPECT_EQ(custom_of(w, "--r: --f(5)", "--r"), "[5]");
  EXPECT_EQ(custom_of(w, "--r: --g()", "--r"), "[none]");
}

TEST(CssFunctionEval, ATrailingCommaAfterTheParametersIsAllowed) {
  // Mixins 1 (pinned 2026-11-07), 2.1: "<@function> = @function <function-token> <function-parameter>#? ,? )" and "Note: A final trailing comma is allowed after the arguments, to
  // make formatting arguments across multiple lines easier." The example of the draft: --refs(--one: calc(var(--two) + var(--three)), --two: 5, --three: calc(var(--two) + 1),)
  // called with no arguments returns 11.
  FnWorld w("@function --refs(--one: calc(var(--two) + var(--three)), --two: 5, --three: calc(var(--two) + 1),) returns <integer> { result: var(--one); }");
  EXPECT_EQ(custom_of(w, "--r: --refs()", "--r"), "11");
}

TEST(CssFunctionEval, ATypedParameterTakesTheDefaultForAnArgumentOfAnotherType) {
  // "a syntax of the parameter type": the registration of a parameter has the parameter's type, so a value that is not of it is invalid, and the next of first-valid() is used.
  FnWorld w(
      "@function --f(--w <length>: 5px) returns <length> { result: var(--w); }\n"
      "@function --g(--w <length>) returns <length> { result: var(--w); }");
  EXPECT_EQ(custom_of(w, "--r: --f(3px)", "--r"), "3px");
  EXPECT_EQ(custom_of(w, "--r: --f(2)", "--r"), "5px");
  EXPECT_EQ(custom_of(w, "--r: --g(3px)", "--r"), "3px");
  EXPECT_EQ(custom_of(w, "--r: --g(2)", "--r"), "<gi>");
}

TEST(CssFunctionEval, TheResultIsCheckedAgainstTheReturnType) {
  // "The result descriptor itself does not have a type, but its resolved value is type-checked during the substitution of a <dashed-function>": a registration named result with the return type.
  FnWorld w("@function --f(--x) returns <length> { result: var(--x); }");
  EXPECT_EQ(custom_of(w, "--r: --f(4px)", "--r"), "4px");
  EXPECT_EQ(custom_of(w, "--r: --f(4)", "--r"), "<gi>");
  EXPECT_EQ(width_of(w, "width: --f(4)"), "auto");
}

TEST(CssFunctionEval, AParameterIsComputedByItsType) {
  // A registered custom property's value is its computed value: `2em` of a <length> parameter is the pixels it computes to.
  FnWorld w("@function --f(--w <length>) { result: var(--w); }");
  Computed c;
  compute(w, c, "font-size: 10px; --r: --f(2em)");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.custom("--r"), "20px");
}

TEST(CssFunctionEval, ASelfCallIsACycle) {
  // The draft's example: "In the following, --foo() is in a cycle with itself", so the result is the guaranteed-invalid value.
  FnWorld w("@function --foo(--x) { result: --foo(10); }");
  EXPECT_EQ(custom_of(w, "--r: --foo(1)", "--r"), "<gi>");
  EXPECT_EQ(width_of(w, "width: --foo(1px)"), "auto");
}

TEST(CssFunctionEval, ALocalVariableThatCallsTheFunctionIsACycleEvenIfUnused) {
  // "Similarly, --bar() is in a cycle with itself, even though the local variable --x is never referenced by result": `--x: --bar(); result: 1;`.
  FnWorld w("@function --bar() { --x: --bar(); result: 1; }");
  EXPECT_EQ(custom_of(w, "--r: --bar()", "--r"), "<gi>");
}

TEST(CssFunctionEval, TwoFunctionsCallingEachOtherAreACycle) {
  FnWorld w(
      "@function --a() { result: --b(); }\n"
      "@function --b() { result: --a(); }\n"
      "@function --c() { result: 1; }");
  EXPECT_EQ(custom_of(w, "--r: --a()", "--r"), "<gi>");
  EXPECT_EQ(custom_of(w, "--r: --b()", "--r"), "<gi>");
  EXPECT_EQ(custom_of(w, "--r: --c()", "--r"), "1");
}

TEST(CssFunctionEval, TheSameFunctionCalledTwiceInTurnIsNotACycle) {
  FnWorld w("@function --one() { result: 1; }\n@function --two() { result: calc(--one() + --one()); }");
  EXPECT_EQ(custom_of(w, "--r: --two()", "--r"), "calc(1 + 1)");
}

TEST(CssFunctionEval, CustomPropertiesThatCallTheFunctionAreNotACycle) {
  // The draft's example: "--baz() is not in a cycle in the example below ... The custom properties --x and --y both reference --baz(), but that's fine": `width: var(--x); /* 11px */ height: var(--y); /* 12px */`.
  FnWorld w("@function --baz(--x) { --y: 10px; result: calc(var(--x) + var(--y)); }");
  Computed c;
  compute(w, c, "--x: --baz(1px); --y: --baz(2px); width: var(--x); height: var(--y)");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.value(w.registry.r, "width"), "11px");
  EXPECT_EQ(c.value(w.registry.r, "height"), "12px");
}

TEST(CssFunctionEval, AFunctionThatReadsTheCustomPropertyThatCallsItIsACycle) {
  // The call site's custom property is in progress, and the function reads it: the same cycle as `--a: var(--b); --b: var(--a)`; every participant is the guaranteed-invalid value.
  FnWorld w("@function --f() { result: var(--y, fallback); }");
  Computed c;
  compute(w, c, "--y: --f(); --z: ok");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.custom("--y"), "<gi>");
  EXPECT_EQ(c.custom("--z"), "ok");
}

TEST(CssFunctionEval, ACallSiteCustomPropertyThatIsNotInProgressIsRead) {
  FnWorld w("@function --f() { result: calc(var(--y) * 2); }");
  Computed c;
  compute(w, c, "--y: 4px; --r: --f()");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.custom("--r"), "calc(4px * 2)");
}

TEST(CssFunctionEval, ResultInheritIsTheKeyword) {
  // "On result, all CSS-wide keywords are left unresolved. Note: result: inherit, for example, will cause the <dashed-function> to evaluate to the inherit keyword, similar to var(--unknown, inherit)."
  FnWorld w("@function --f() { result: inherit; }");
  Computed p;
  compute(w, p, "--r: 5px");
  ASSERT_EQ(p.status, GTEXT_CSS_OK);
  Computed c;
  compute(w, c, "--r: --f()", &p);
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.custom("--r"), "5px");
}

TEST(CssFunctionEval, InitialOfALocalVariableIsTheParameterAndInheritIsTheCallSite) {
  // "setting the --foo local variable to initial will resolve it to the --foo parameter, while inherit will resolve it to the --foo custom property from the call site."
  FnWorld w(
      "@function --i(--foo) { --foo: initial; result: var(--foo); }\n"
      "@function --h(--foo) { --foo: inherit; result: var(--foo); }");
  EXPECT_EQ(custom_of(w, "--foo: site; --r: --i(param)", "--r"), "param");
  EXPECT_EQ(custom_of(w, "--foo: site; --r: --h(param)", "--r"), "site");
}

TEST(CssFunctionEval, AnyOtherCssWideKeywordOnACustomPropertyIsGuaranteedInvalid) {
  // "any other CSS-wide keyword: Resolves to the guaranteed-invalid value."
  FnWorld w(
      "@function --u() { --x: unset; result: var(--x, dflt); }\n"
      "@function --v() { --x: revert; result: var(--x, dflt); }\n"
      "@function --n() { --x: initial; result: var(--x, dflt); }");
  EXPECT_EQ(custom_of(w, "--r: --u()", "--r"), "dflt");
  EXPECT_EQ(custom_of(w, "--r: --v()", "--r"), "dflt");
  EXPECT_EQ(custom_of(w, "--r: --n()", "--r"), "dflt");
}

TEST(CssFunctionEval, InheritIsReinterpretedUsingTheLocalType) {
  // "inherit: Resolves like an inherit() function with the custom property name as its one and only argument. Note: This ensures that a function parameter defaulted to inherit is
  // reinterpreted using the local parameter type." The call site's value is a length or it is not: a value of another type is invalid, which is the registration's initial value, the
  // parameter's value. (A typed parameter cannot be *defaulted* to a keyword: "the default value must parse successfully according to that parameter type's syntax", css_custom_function.h.)
  FnWorld w("@function --f(--w <length>) returns <length> { --w: inherit; result: var(--w); }");
  EXPECT_EQ(custom_of(w, "--w: 6px; --r: --f(1px)", "--r"), "6px");
  EXPECT_EQ(custom_of(w, "--w: 6; --r: --f(1px)", "--r"), "1px");
  FnWorld u("@function --g(--w: inherit) { result: var(--w, none); }");
  EXPECT_EQ(custom_of(u, "--w: site; --r: --g()", "--r"), "site");
  EXPECT_EQ(custom_of(u, "--r: --g()", "--r"), "none");
}

TEST(CssFunctionEval, InheritFunctionInTheBodyReadsTheCallSite) {
  FnWorld w("@function --f(--x) { result: inherit(--x, none); }");
  EXPECT_EQ(custom_of(w, "--x: site; --r: --f(param)", "--r"), "site");
  EXPECT_EQ(custom_of(w, "--r: --f(param)", "--r"), "none");
}

TEST(CssFunctionEval, InheritReferToTheParentStackFrame) {
  // The pinned page css-mixins/functions/local-inherit-substitution.html: "inherit() refers to parent stack frame" (the element, another function call), "referring to
  // guaranteed-invalid in parent frame", "referring to cycle in parent frame" and "referring to typed value in parent frame".
  FnWorld w(
      "@function --e() { --x: FAIL2; result: inherit(--x); }\n"
      "@function --f() { --x: PASS; result: --g(); }\n"
      "@function --g() { --x: FAIL3; result: inherit(--x); }\n"
      "@function --f2() { --x: var(--noexist); result: --g2(); }\n"
      "@function --g2() { --x: FAIL3; result: inherit(--x, PASS); }\n"
      "@function --f3() { --x: var(--x); result: --g2(); }\n"
      "@function --f4(--x <length>) { result: --g4(); }\n"
      "@function --g4() { --x: 14px; result: inherit(--x); }");
  EXPECT_EQ(custom_of(w, "--x: PASS; --r: --e()", "--r"), "PASS");
  EXPECT_EQ(custom_of(w, "--x: FAIL2; --r: --f()", "--r"), "PASS");
  EXPECT_EQ(custom_of(w, "--x: FAIL2; --r: --f2()", "--r"), "PASS");
  EXPECT_EQ(custom_of(w, "--x: FAIL2; --r: --f3()", "--r"), "PASS");
  EXPECT_EQ(custom_of(w, "--x: 13px; --r: --f4(42.0px)", "--r"), "42px");
}

TEST(CssFunctionEval, AnArgumentMayBeACallAndIsEvaluatedAtTheCallSite) {
  FnWorld w("@function --inc(--n) { result: calc(var(--n) + 1); }");
  EXPECT_EQ(custom_of(w, "--r: --inc(--inc(1))", "--r"), "calc(calc(1 + 1) + 1)");
}

TEST(CssFunctionEval, AnArgumentCannotSeeTheCalleesNames) {
  // Arguments are substituted before the function is entered ("For each arg in arguments, substitute arbitrary substitution functions in arg"), in the calling context.
  FnWorld w("@function --f(--p) { --q: local; result: var(--p); }");
  EXPECT_EQ(custom_of(w, "--q: site; --r: --f(var(--q))", "--r"), "site");
}

TEST(CssFunctionEval, AttrReadsTheElementOfTheCallingContext) {
  // "all values which would normally refer to the element being styled instead refer to calling context's root element. Note: For example, attr() in a property".
  FnWorld w("@function --f() returns <length> { result: attr(data-w px); }");
  Node n;
  n.attrs = {{"data-w", "12"}};
  Computed c;
  compute(w, c, "width: --f()", nullptr, &n);
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.value(w.registry.r, "width"), "12px");
}

TEST(CssFunctionEval, AFunctionCallsItsRegisteredCallSiteProperty) {
  // The registrations of the context are not visible inside the function ("Only the custom property registrations in registrations are visible; all other custom properties are treated as unregistered"),
  // and a call site's registered value is read as the computed value text.
  FnWorld w("@function --f() { result: var(--len); }", {{"--len", "<length>", "0px", true, true, false}});
  Computed c;
  compute(w, c, "font-size: 10px; --len: 2em; --r: --f()");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.custom("--r"), "20px");
}

TEST(CssFunctionEval, TheResultIsCountedInTheBudget) {
  // Safely handling overly-long substitution (Values 5, Appendix A): what a call expands into counts as what var() expands into, so a result past the budget is the guaranteed-invalid value.
  const char * sheet = "@function --f(--x) { result: var(--x) var(--x) var(--x) var(--x); }";
  {
    World w;
    w.function_sheets.push_back(sheet);
    ASSERT_EQ(w.build(), GTEXT_CSS_OK);
    Computed c;
    compute(w, c, "--r: --f(a)");
    ASSERT_EQ(c.status, GTEXT_CSS_OK);
    EXPECT_EQ(c.custom("--r"), "a a a a");
  }
  {
    World w;
    w.function_sheets.push_back(sheet);
    w.options.substitute.max_substituted_tokens = 3;
    ASSERT_EQ(w.build(), GTEXT_CSS_OK);
    Computed c;
    compute(w, c, "--r: --f(a)");
    ASSERT_EQ(c.status, GTEXT_CSS_OK);
    EXPECT_EQ(c.custom("--r"), "<gi>");
    EXPECT_GT(c.result.substitution_limited, 0u);
  }
}

TEST(CssFunctionEval, AnImportantResultIsAnUnknownDescriptor) {
  // The F-a rule: `result` takes no !important, so the function has no result.
  FnWorld w("@function --f() { result: 1 !important; }");
  EXPECT_EQ(custom_of(w, "--r: --f()", "--r"), "<gi>");
}

TEST(CssFunctionEval, ACallInsideAnotherFunctionsBodyWithADefaultThatCallsAFunction) {
  FnWorld w(
      "@function --base() { result: 4px; }\n"
      "@function --f(--a: --base()) { result: var(--a); }");
  EXPECT_EQ(custom_of(w, "--r: --f()", "--r"), "4px");
  EXPECT_EQ(custom_of(w, "--r: --f(2px)", "--r"), "2px");
}

TEST(CssFunctionEval, ADefaultMayReadAnEarlierParameterAndTheCallSite) {
  // The argument rule gives every parameter as a custom property of the hypothetical element, so a default reads the other parameters, then the call site.
  FnWorld w("@function --f(--a, --b: var(--a), --c: var(--site)) { result: var(--a) var(--b) var(--c); }");
  EXPECT_EQ(custom_of(w, "--site: s; --r: --f(1)", "--r"), "1 1 s");
}

TEST(CssFunctionEval, ADefaultThatReadsItselfIsACycle) {
  FnWorld w("@function --f(--a: var(--a)) { result: var(--a, fb); }");
  EXPECT_EQ(custom_of(w, "--r: --f()", "--r"), "fb");
}

TEST(CssFunctionEval, TheLimitsOfTheCallDepthAndTheBody) {
  std::string sheet;
  for (int i = 0; i < 20; i++) {
    sheet += "@function --f" + std::to_string(i) + "() { result: " + (i < 19 ? "--f" + std::to_string(i + 1) + "()" : std::string("end")) + "; }\n";
  }
  {
    World w;
    w.function_sheets.push_back(sheet);
    w.options.substitute.max_call_depth = 10;
    ASSERT_EQ(w.build(), GTEXT_CSS_OK);
    Computed c;
    compute(w, c, "--r: --f0()");
    EXPECT_EQ(c.status, GTEXT_CSS_E_LIMIT_DEPTH);
  }
  {
    World w;
    w.function_sheets.push_back(sheet);
    w.options.substitute.max_call_depth = 20;
    ASSERT_EQ(w.build(), GTEXT_CSS_OK);
    Computed c;
    compute(w, c, "--r: --f0()");
    ASSERT_EQ(c.status, GTEXT_CSS_OK);
    EXPECT_EQ(c.custom("--r"), "end");
  }
  {
    World w;
    w.function_sheets.push_back("@function --b() { --a: 1; --b: 2; --c: 3; result: 4; }");
    w.options.substitute.max_function_declarations = 3;
    ASSERT_EQ(w.build(), GTEXT_CSS_OK);
    Computed c;
    compute(w, c, "--r: --b()");
    EXPECT_EQ(c.status, GTEXT_CSS_E_LIMIT_CUSTOM_PROPERTIES);
  }
  {
    World w;
    w.function_sheets.push_back("@function --b() { --a: 1; --b: 2; --c: 3; result: 4; }");
    w.options.substitute.max_function_declarations = 4;
    ASSERT_EQ(w.build(), GTEXT_CSS_OK);
    Computed c;
    compute(w, c, "--r: --b()");
    ASSERT_EQ(c.status, GTEXT_CSS_OK);
    EXPECT_EQ(c.custom("--r"), "4");
  }
}

TEST(CssFunctionEval, AChainOfAHundredThousandCallsUsesNoCStack) {
  // The frames are an explicit stack: a call inside a call, 100,000 deep, is a status when a limit says so and a value when none does, never a stack overflow.
  const int n = 100000;
  std::string sheet;
  sheet.reserve(static_cast<size_t>(n) * 48);
  for (int i = 0; i < n; i++) {
    sheet += "@function --f" + std::to_string(i) + "() { result: " + (i + 1 < n ? "--f" + std::to_string(i + 1) + "()" : std::string("end")) + "; }\n";
  }
  {
    World w;
    w.function_sheets.push_back(sheet);
    w.fn_options.max_functions = static_cast<size_t>(n) + 8;
    ASSERT_EQ(w.build(), GTEXT_CSS_OK);
    Computed c;
    compute(w, c, "--r: --f0()");
    EXPECT_EQ(c.status, GTEXT_CSS_E_LIMIT_DEPTH);   // the default depth of calls
  }
  {
    World w;
    w.function_sheets.push_back(sheet);
    w.fn_options.max_functions = static_cast<size_t>(n) + 8;
    w.options.substitute.max_call_depth = static_cast<size_t>(n) + 8;
    w.options.substitute.max_substitution_depth = static_cast<size_t>(1) << 24;
    ASSERT_EQ(w.build(), GTEXT_CSS_OK);
    Computed c;
    compute(w, c, "--r: --f0()");
    ASSERT_EQ(c.status, GTEXT_CSS_OK);
    EXPECT_EQ(c.custom("--r"), "end");
  }
}

TEST(CssFunctionEval, AnArgumentListLongerThanTheParametersIsGuaranteedInvalid) {
  FnWorld w("@function --f(--a) { result: var(--a); }");
  std::string args;
  for (int i = 0; i < 70000; i++) {
    args += i ? ",1" : "1";
  }
  EXPECT_EQ(custom_of(w, "--r: --f(" + args + ")", "--r"), "<gi>");
}

// ---- cost -------------------------------------------------------------------------------------------------

/// The work one computation counted (GTEXT_CSS_Compute_Result::work), the counter the quadratic suite asserts on and not the clock.
size_t work_of(World & w, const std::string & decls) {
  Computed c;
  compute(w, c, decls);
  EXPECT_EQ(c.status, GTEXT_CSS_OK);
  return c.result.work;
}

TEST(CssFunctionEvalCost, ManyCallsOfOneFunctionCostWhatTheyHold) {
  // A function called n times in one value: the work grows with n, four times the calls are not sixteen times the work.
  FnWorld w("@function --one(--x) { result: var(--x); }");
  auto many = [](int n) {
    std::string v = "--r: ";
    for (int i = 0; i < n; i++) {
      v += (i ? " " : "") + std::string("--one(") + std::to_string(i) + ")";
    }
    return v;
  };
  const size_t a = work_of(w, many(500)), b = work_of(w, many(2000)), c = work_of(w, many(8000));
  EXPECT_LT(b, a * 6) << a << " " << b;
  EXPECT_LT(c, b * 6) << b << " " << c;
}

TEST(CssFunctionEvalCost, AFunctionOfManyParametersCostsWhatItsParametersHold) {
  auto sheet = [](int n) {
    std::string params, body;
    for (int i = 0; i < n; i++) {
      params += (i ? ", --p" : "--p") + std::to_string(i);
      body += (i ? " " : "") + std::string("var(--p") + std::to_string(i) + ")";
    }
    return "@function --f(" + params + ") { result: " + body + "; }";
  };
  auto call = [](int n) {
    std::string args;
    for (int i = 0; i < n; i++) {
      args += (i ? "," : "") + std::to_string(i);
    }
    return "--r: --f(" + args + ")";
  };
  size_t last = 0;
  for (int n : {200, 800, 3200}) {
    World w;
    w.function_sheets.push_back(sheet(n));
    ASSERT_EQ(w.build(), GTEXT_CSS_OK);
    const size_t work = work_of(w, call(n));
    if (last) {
      EXPECT_LT(work, last * 6) << n;
    }
    last = work;
  }
}

TEST(CssFunctionEvalCost, ABodyOfManyLocalVariablesCostsWhatItHolds) {
  auto sheet = [](int n) {
    std::string body;
    for (int i = 0; i < n; i++) {
      body += "--l" + std::to_string(i) + ": " + (i ? "var(--l" + std::to_string(i - 1) + ")" : std::string("1")) + "; ";
    }
    return "@function --f() { " + body + " result: var(--l" + std::to_string(n - 1) + "); }";
  };
  size_t last = 0;
  for (int n : {200, 800, 3200}) {
    World w;
    w.function_sheets.push_back(sheet(n));
    ASSERT_EQ(w.build(), GTEXT_CSS_OK);
    EXPECT_EQ(custom_of(w, "--r: --f()", "--r"), "1");
    const size_t work = work_of(w, "--r: --f()");
    if (last) {
      EXPECT_LT(work, last * 6) << n;
    }
    last = work;
  }
}

// ---- allocation failure ------------------------------------------------------------------------------------

TEST(CssFunctionEvalOom, EveryAllocationOfACallFailsCleanly) {
  const char * sheet =
      "@function --f(--a <length>: 2px, --b: var(--site, none)) returns <length> { --l: calc(var(--a) * 2); --a: initial; result: var(--l); }\n"
      "@function --g(--x) { result: --f(var(--x)); }\n"
      "@function --cyc() { --z: --cyc(); result: 1; }";
  const char * decls = "--site: s; --r: --g(3px); --s: --cyc(); --t: --f(); width: --g(1px); height: --f(2); margin-top: --missing()";
  FaultAllocator fa;
  {
    World w;
    w.options.allocator = &fa.alloc;
    w.function_sheets.push_back(sheet);
    ASSERT_EQ(w.build(), GTEXT_CSS_OK);
    std::string want;
    fa.arm(0);
    {
      Computed c;
      compute(w, c, decls);
      ASSERT_EQ(c.status, GTEXT_CSS_OK);
      want = c.custom("--r") + "|" + c.custom("--s") + "|" + c.custom("--t") + "|" + c.value(w.registry.r, "width") + "|" + c.value(w.registry.r, "height");
    }
    EXPECT_EQ(want.substr(0, 4), "6px|");
    ASSERT_GT(fa.calls, 30u);
    size_t failures = 0;
    for (size_t n = 1; n < 100000; n++) {
      std::string got;
      GTEXT_CSS_Status st;
      {
        fa.arm(n);
        Computed c;
        compute(w, c, decls);
        fa.disarm();
        st = c.status;
        if (st == GTEXT_CSS_OK) {
          got = c.custom("--r") + "|" + c.custom("--s") + "|" + c.custom("--t") + "|" + c.value(w.registry.r, "width") + "|" + c.value(w.registry.r, "height");
        }
        else {
          EXPECT_EQ(c.s, nullptr) << n;
        }
      }
      if (!fa.failed) {
        EXPECT_EQ(st, GTEXT_CSS_OK) << n;
        EXPECT_EQ(got, want) << n;
        break;
      }
      failures++;
      if (st != GTEXT_CSS_OK) {
        EXPECT_EQ(st, GTEXT_CSS_E_OOM) << "allocation " << n << " " << gtext_css_status_string(st);
      }
      else {
        EXPECT_EQ(got, want) << n << ": a failure that was swallowed changed the answer";
      }
      EXPECT_EQ(fa.bad_frees, 0u) << n;
    }
    EXPECT_GT(failures, 30u);
  }
  EXPECT_TRUE(fa.live.empty()) << "a block is left";
  EXPECT_EQ(fa.bad_frees, 0u);
}

}  // namespace
