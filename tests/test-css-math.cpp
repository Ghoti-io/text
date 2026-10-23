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
 * The CSS math functions (CSS Values and Units 4, Editor's Draft of 24 August 2026, section 10): the type
 * system, the simplification and serialization of a calculation, each function with its argument ranges, the
 * signed zeros, infinities and NaN, the limits, and the published value family of the elementary functions
 * measured in units in the last place. The expectations are the draft's sentences and worked examples, and
 * the specified-value serializations that web-platform-tests' css/css-values pages expect.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_math_test_support.h"
#include "markup_test_support.h"

extern "C" {
bool css_math_apply(GTEXT_CSS_Math_Op op, GTEXT_CSS_Math_Round round, const double * a, size_t n, double * out);
double css_math_fmod_exact(double x, double y);
}

using namespace gm;

namespace {

struct Row {
  const char * input;
  const char * expect;
};

void check(const std::vector<Row> & rows) {
  for (const Row & r : rows) {
    EXPECT_EQ(calc(r.input), r.expect) << r.input;
  }
}

/// The value of a calculation that came down to one number, as a double (so -0 and the last place are seen).
bool value_of(const std::string & text, double * v, GTEXT_CSS_Unit * unit = nullptr) {
  Calc c;
  read(c, text);
  if (c.status != GTEXT_CSS_OK || !c.valid) {
    return false;
  }
  return gtext_css_math_numeric(c.math, v, unit, nullptr);
}

GTEXT_CSS_Math_Category category_of(const std::string & text) {
  Calc c;
  read(c, text);
  if (c.status != GTEXT_CSS_OK || !c.valid) {
    return GTEXT_CSS_MATH_CAT_NONE;
  }
  return c.math->category;
}

}  // namespace

// ---- the matrix of the specification of this phase ----------------------------------------------------------------

TEST(CssMathMatrix, ASumOfAbsoluteLengthsIsOneLength) {
  EXPECT_EQ(calc("calc(1px + 2px)"), "calc(3px)");
}

TEST(CssMathMatrix, ALengthAndATimeCannotBeAdded) {
  // Values 4, 10.9: "attempt to add the types of the left and right arguments. If this returns failure, the entire calculation's type is failure."
  EXPECT_EQ(calc("calc(1px + 2s)"), "invalid");
}

TEST(CssMathMatrix, APercentageAndALengthAreKeptWithThePercentHint) {
  Calc c;
  read(c, "calc(10% + 5px)");
  ASSERT_TRUE(c.valid);
  EXPECT_EQ(serialize(c.math), "calc(10% + 5px)");
  EXPECT_EQ(c.math->category, GTEXT_CSS_MATH_CAT_LENGTH);
  EXPECT_TRUE(c.math->has_percent);
  EXPECT_EQ(c.math->type.percent_hint, GTEXT_CSS_MATH_BASE_LENGTH + 1);
  EXPECT_EQ(c.math->type.exponent[GTEXT_CSS_MATH_BASE_LENGTH], 1);
  EXPECT_EQ(c.math->type.exponent[GTEXT_CSS_MATH_BASE_PERCENT], 0);
}

TEST(CssMathMatrix, InfinityAndNaNAreKeptAsTheDraftSerializesThem) {
  check({{"calc(infinity * 1px)", "calc(infinity * 1px)"}, {"calc(NaN)", "calc(NaN)"}, {"calc(-infinity)", "calc(-infinity)"},
      {"calc(infinity)", "calc(infinity)"}, {"calc(NaN * 1px)", "calc(NaN * 1px)"}, {"calc(1% * infinity)", "calc(infinity * 1%)"},
      {"calc(InFiNiTy)", "calc(infinity)"}, {"calc(nan)", "calc(NaN)"}});
}

TEST(CssMathMatrix, DivisionByZeroIsInfinity) {
  check({{"calc(1 / 0)", "calc(infinity)"}, {"calc(-1 / 0)", "calc(-infinity)"}, {"calc(0 / 0)", "calc(NaN)"}, {"calc(1px / 0)", "calc(infinity * 1px)"}});
}

TEST(CssMathMatrix, NestingPastTheLimitIsALimitWithALocation) {
  GTEXT_CSS_Math_Options o = gtext_css_math_options_default();
  o.max_depth = 8;
  auto nested = [](int n) {
    std::string s;
    for (int i = 0; i < n; i++) {
      s += "calc(";
    }
    s += "1";
    for (int i = 0; i < n; i++) {
      s += ")";
    }
    return s;
  };
  Calc at, over;
  read(at, nested(8), nullptr, &o);
  EXPECT_EQ(at.status, GTEXT_CSS_OK);
  EXPECT_TRUE(at.valid);
  read(over, nested(9), nullptr, &o);
  EXPECT_EQ(over.status, GTEXT_CSS_E_LIMIT_DEPTH);
  EXPECT_FALSE(over.valid);
  EXPECT_EQ(over.result.status, GTEXT_CSS_E_LIMIT_DEPTH);
  // the place is the function that went past the limit: the ninth calc(, which starts after the eight before it
  EXPECT_EQ(over.result.where.offset, 8u * 5u);
  EXPECT_EQ(over.result.where.line, 1u);
}

// ---- simplification and serialization (10.10, 10.13) -------------------------------------------------------

TEST(CssMathSerialize, TheDraftsExamplesOfTheSerializationOfASum) {
  // "calc(20px + 30px) would serialize as calc(50px) as a specified value"; "calc(20px + 0%) ... calc(0% + 20px)";
  // "calc(20px + 2em) would serialize as calc(2em + 20px)"
  check({{"calc(20px + 30px)", "calc(50px)"}, {"calc(20px + 0%)", "calc(0% + 20px)"}, {"calc(20px + 2em)", "calc(2em + 20px)"}});
}

TEST(CssMathSerialize, TheChildrenOfASumAreSortedNumberPercentageDimensionsByUnitThenTheRest) {
  // wpt css/css-values/calc-serialization.html
  check({{"calc(10px + 1vmin + 10%)", "calc(10% + 10px + 1vmin)"}, {"calc(10px + 1vmin)", "calc(10px + 1vmin)"},
      {"calc(10px + 1em)", "calc(1em + 10px)"}, {"calc(1vmin - 10px)", "calc(-10px + 1vmin)"}, {"calc(-10px + 1em)", "calc(1em - 10px)"},
      {"calc(-10px)", "calc(-10px)"}, {"calc(100% / 3 - 2 * 1em - 2 * 1px)", "calc(33.333333% - 2em - 2px)"}});
  // the dimensions are sorted ASCII case-insensitively: Q is converted, so the test uses two relative ones
  check({{"calc(1vh + 1ex + 1ch + 1em)", "calc(1ch + 1em + 1ex + 1vh)"}});
}

TEST(CssMathSerialize, AbsoluteUnitsAreConvertedToTheCanonicalOne) {
  check({{"calc(1in)", "calc(96px)"}, {"calc(1cm)", "calc(37.795276px)"}, {"calc(1pt)", "calc(1.333333px)"}, {"calc(1pc)", "calc(16px)"},
      {"calc(1turn)", "calc(360deg)"}, {"calc(100grad)", "calc(90deg)"}, {"calc(1ms)", "calc(0.001s)"}, {"calc(1khz)", "calc(1000Hz)"},
      {"calc(96dpi)", "calc(1dppx)"}, {"calc(1x)", "calc(1dppx)"}});
}

TEST(CssMathSerialize, TheNameOfARootFunctionIsWrittenNotCalc) {
  // wpt minmax-length-percent-serialize: "Arguments are simplified, but not reordered."
  check({{"min(20px, 10%)", "min(20px, 10%)"}, {"min(1em, 10%)", "min(1em, 10%)"}, {"max(10%, 20px)", "max(10%, 20px)"},
      {"calc(min(20px, 10%))", "min(20px, 10%)"}, {"min(1px)", "calc(1px)"}, {"min(1px, 2px)", "calc(1px)"},
      {"min(1% + 1px)", "calc(1% + 1px)"}, {"min(1px + 1%)", "calc(1% + 1px)"},
      // a percentage joins a length-percentage that already has the percent hint of a length, however many follow
      {"min(1em, 1%, 2%)", "min(1em, 1%, 2%)"}, {"max(1%, 1em, 2%, 3px)", "max(1%, 1em, 2%, 3px)"},
      {"clamp(1%, 1em, 2%)", "clamp(1%, 1em, 2%)"}});
}

TEST(CssMathSerialize, ClampWithNoneOnASideIsMinOrMax) {
  // wpt clamp-partial-serialize: the draft does not say; the pages agree on this
  check({{"clamp(none, 2px, 3em)", "min(2px, 3em)"}, {"clamp(1em, 2px, none)", "max(1em, 2px)"},
      {"clamp(1px, 2px, clamp(none, 4px, 5em))", "clamp(1px, 2px, min(4px, 5em))"},
      {"clamp(clamp(none, 2em, none), 4px, clamp(none, 6em, none))", "clamp(2em, 4px, 6em)"}, {"clamp(none, 33px, 30px)", "calc(30px)"},
      {"clamp(33px, 30px, none)", "calc(33px)"}, {"clamp(none, 30px, none)", "calc(30px)"}});
}

TEST(CssMathSerialize, ClampFavorsTheMinimum) {
  // "favoring the min calculation if it conflicts with the max"
  check({{"clamp(1px, 2px, 3px)", "calc(2px)"}, {"clamp(10px, 5px, 30px)", "calc(10px)"}, {"clamp(10px, 35px, 30px)", "calc(30px)"},
      {"clamp(30px, 100px, 20px)", "calc(30px)"}, {"clamp(-10px, 100px, -30px)", "calc(-10px)"}, {"clamp(1px, 1em, 1vh)", "clamp(1px, 1em, 1vh)"},
      {"clamp(10px , 35px /* foo */, 30px)", "calc(30px)"}});
}

TEST(CssMathSerialize, ZeroTermsAreKeptWhenTheirUnitDiffers) {
  // "Zero-valued terms cannot be simply removed from a Sum; they can only be combined with other values that have identical units."
  check({{"calc(0px + 1em)", "calc(1em + 0px)"}, {"calc(0px - clamp(1px, 1em, 1vh))", "calc(0px - clamp(1px, 1em, 1vh))"},
      {"calc(0% + 0px)", "calc(0% + 0px)"}});
}

TEST(CssMathSerialize, ANumberTimesASumIsDistributed) {
  check({{"calc(2 * (1px + 2em))", "calc(4em + 2px)"}, {"calc((1px + 2%) * 3)", "calc(6% + 3px)"}, {"calc((1px + 2%) / 2)", "calc(1% + 0.5px)"}});
}

TEST(CssMathSerialize, ANegatedSumIsASumOfNegatedTerms) {
  check({{"calc(-1 * (1em + 2px))", "calc(-1em - 2px)"}, {"calc(0px - (1em + 2px))", "calc(-1em - 2px)"}});
}

TEST(CssMathSerialize, ProductsOfNumbersAndOneDimensionFold) {
  check({{"calc(2 * 3em)", "calc(6em)"}, {"calc(1px * 2 * 3)", "calc(6px)"}, {"calc(10px / 2)", "calc(5px)"}, {"calc(10px / 2px)", "calc(5)"},
      {"calc(100% / 20%)", "calc(5)"}, {"calc(1px * 1px / 1px)", "calc(1px)"}, {"calc(2px * 3px / 1px)", "calc(6px)"}});
  // an area (or a rate) is not a type a property can take, so a calculation that is one is not valid (10.9: "If it can't match any of these, the math function is invalid.")
  check({{"calc(1px * 1px)", "invalid"}, {"calc(1 / 2px)", "invalid"}, {"calc(1em * 1em)", "invalid"}});
}

TEST(CssMathSerialize, AnUnresolvedProductKeepsItsShape) {
  // wpt: "calc(1 * min(NaN * 1px, NaN * 1em))"
  check({{"calc(1 * min(NaN * 2px, NaN * 4em))", "calc(1 * min(NaN * 1px, NaN * 1em))"},
      {"calc(1 * clamp(NaN * 2em, NaN * 4px, NaN * 8pt))", "calc(1 * clamp(NaN * 1em, NaN * 1px, NaN * 1px))"},
      {"calc(1em * 1em / 1px)", "calc(1em * 1em / 1px)"}, {"calc(2em / 3em)", "calc(2em / 3em)"}});
}

TEST(CssMathSerialize, NumbersAreCssomsShortestSixDecimalForm) {
  check({{"calc(0.1 + 0.2)", "calc(0.3)"}, {"calc(1 / 3)", "calc(0.333333)"}, {"calc(1e3)", "calc(1000)"}, {"calc(1000000000)", "calc(1000000000)"},
      {"calc(0.0000001)", "calc(0)"}});
}

TEST(CssMathSerialize, ParenthesesAndNestedCalcAreTheSame) {
  // "Parentheses and nesting additional calc() functions are equivalent"
  check({{"calc(2 + 3 * 4)", "calc(14)"}, {"calc((2 + 3) * 4)", "calc(20)"}, {"calc(calc(2 + 3) * 4)", "calc(20)"}, {"calc(((((1)))))", "calc(1)"}});
}

TEST(CssMathSerialize, SymbolsAndUnresolvedFunctionsKeepTheTreeShape) {
  GTEXT_CSS_Math_Keyword kw[] = {{"r", {{0, 0, 0, 0, 0, 0, 0}, 0}}, {"g", {{0, 0, 0, 0, 0, 0, 0}, 0}}};
  GTEXT_CSS_Math_Context ctx = {kw, 2};
  EXPECT_EQ(calc("calc(r)", &ctx), "calc(r)");
  EXPECT_EQ(calc("calc(g * 2)", &ctx), "calc(2 * g)");
  EXPECT_EQ(calc("calc(g * .5 + g * .5)", &ctx), "calc((0.5 * g) + (0.5 * g))");
  EXPECT_EQ(calc("calc(r * .5 - g * .5)", &ctx), "calc((0.5 * r) - (0.5 * g))");
  EXPECT_EQ(calc("calc(r / g)", &ctx), "calc(r / g)");
  EXPECT_EQ(calc("calc(-r)", &ctx), "invalid");
  EXPECT_EQ(calc("calc(r)"), "invalid");  // a keyword the context did not define
  EXPECT_EQ(calc("calc(sibling-index() * 2)"), "calc(2 * sibling-index())");
  EXPECT_EQ(calc("sibling-index()"), "sibling-index()");
}

// ---- the grammar ---------------------------------------------------------------------------------------------

TEST(CssMathSyntax, WhiteSpaceIsRequiredOnBothSidesOfPlusAndMinusOnly) {
  // "whitespace is required on both sides of the + and - operators. (The * and / operators can be used without white space around them.)"
  check({{"calc(1px+2px)", "invalid"}, {"calc(1px +2px)", "invalid"}, {"calc(1px+ 2px)", "invalid"}, {"calc(1px - -2px)", "calc(3px)"},
      {"calc(2*3)", "calc(6)"}, {"calc(6/3)", "calc(2)"}, {"calc(1px -2px)", "invalid"}, {"calc(1 +  2)", "calc(3)"}});
}

TEST(CssMathSyntax, TheTokensThatAreNotACalculationAreRefused) {
  check({{"calc()", "invalid"}, {"calc(1 +)", "invalid"}, {"calc(* 1)", "invalid"}, {"calc(1 * * 2)", "invalid"}, {"calc(1, 2)", "invalid"},
      {"calc(1 2)", "invalid"}, {"calc(foo)", "invalid"}, {"calc(\"a\")", "invalid"}, {"calc(#fff)", "invalid"}, {"calc(1px + )", "invalid"},
      {"calc((1)", "calc(1)"}, {"calc([1])", "invalid"}, {"calc(1 + (2)", "calc(3)"}, {"calc(+ 1)", "invalid"}, {"calc(1deg + 1foo)", "invalid"},
      {"calc(0 + 5px)", "invalid"}, {"min()", "invalid"}, {"min(1,)", "invalid"}, {"min(,1)", "invalid"}, {"foo(1)", "invalid"}});
}

TEST(CssMathSyntax, RoundTakesItsStrategyFirstAndOnlyThere) {
  check({{"round(up, 1.1px, 1px)", "calc(2px)"}, {"round(1.1px, up, 1px)", "invalid"}, {"round(nearest, 1.5, 1)", "calc(2)"},
      {"round(down 1px, 1px)", "invalid"}, {"round(1.5)", "calc(2)"}, {"round(up)", "invalid"}, {"round(1px)", "invalid"},
      {"round(to-zero, -1.5)", "calc(-1)"}});
}

TEST(CssMathSyntax, ConstantsAreNumbers) {
  check({{"calc(e)", "calc(2.718282)"}, {"calc(pi)", "calc(3.141593)"}, {"calc(PI)", "calc(3.141593)"}, {"calc(pi * 1px)", "calc(3.141593px)"},
      {"calc(e + 1)", "calc(3.718282)"}, {"calc(infinity * -1)", "calc(-infinity)"}});
}

TEST(CssMathSyntax, UnknownUnitsAndNonCalculationFunctionsAreRefused) {
  EXPECT_EQ(calc("calc(1foo)"), "invalid");
  EXPECT_EQ(calc("calc(var(--x))"), "invalid");  // (a match takes it as pending before the math layer sees it)
  EXPECT_EQ(calc("calc(rgb(1 2 3))"), "invalid");
}

TEST(CssMathSyntax, WhatAFunctionResolvesToDecidesWhereItCanStand) {
  EXPECT_EQ(category_of("calc(1)"), GTEXT_CSS_MATH_CAT_NUMBER);
  EXPECT_EQ(category_of("calc(1%)"), GTEXT_CSS_MATH_CAT_PERCENTAGE);
  EXPECT_EQ(category_of("calc(1px)"), GTEXT_CSS_MATH_CAT_LENGTH);
  EXPECT_EQ(category_of("calc(1deg)"), GTEXT_CSS_MATH_CAT_ANGLE);
  EXPECT_EQ(category_of("calc(1s)"), GTEXT_CSS_MATH_CAT_TIME);
  EXPECT_EQ(category_of("calc(1hz)"), GTEXT_CSS_MATH_CAT_FREQUENCY);
  EXPECT_EQ(category_of("calc(1dppx)"), GTEXT_CSS_MATH_CAT_RESOLUTION);
  EXPECT_EQ(category_of("calc(1fr)"), GTEXT_CSS_MATH_CAT_FLEX);
  // an area is not a type a property can take
  EXPECT_EQ(category_of("calc(1px * 1px)"), GTEXT_CSS_MATH_CAT_NONE);
  EXPECT_EQ(category_of("calc(1px / 1s)"), GTEXT_CSS_MATH_CAT_NONE);
  // "calc(5px - 5px + 10s) and calc(0 * 5px + 10s) are both invalid due to the attempt to add a length and a time"
  EXPECT_EQ(calc("calc(5px - 5px + 10s)"), "invalid");
  EXPECT_EQ(calc("calc(0 * 5px + 10s)"), "invalid");
  // "opacity: calc(.25 + 25%) is invalid" (a number and a percentage); and a number is not a length
  EXPECT_EQ(calc("calc(.25 + 25%)"), "invalid");
  EXPECT_EQ(calc("calc(1 + 1px)"), "invalid");
}

extern "C" {
bool css_math_type_add(const GTEXT_CSS_Math_Type * a, const GTEXT_CSS_Math_Type * b, GTEXT_CSS_Math_Type * out);
bool css_math_type_mul(const GTEXT_CSS_Math_Type * a, const GTEXT_CSS_Math_Type * b, GTEXT_CSS_Math_Type * out);
}

TEST(CssMathType, ATypeIsAnExponentMapWithAPercentHint) {
  const GTEXT_CSS_Math_Type length = gtext_css_math_category_type(GTEXT_CSS_MATH_CAT_LENGTH);
  const GTEXT_CSS_Math_Type time = gtext_css_math_category_type(GTEXT_CSS_MATH_CAT_TIME);
  const GTEXT_CSS_Math_Type percent = gtext_css_math_category_type(GTEXT_CSS_MATH_CAT_PERCENTAGE);
  const GTEXT_CSS_Math_Type number = gtext_css_math_category_type(GTEXT_CSS_MATH_CAT_NUMBER);
  GTEXT_CSS_Math_Type t, u;
  // multiplying adds the exponents: length * length / time is {length: 2, time: -1}, which no property takes
  ASSERT_TRUE(css_math_type_mul(&length, &length, &t));
  EXPECT_EQ(t.exponent[GTEXT_CSS_MATH_BASE_LENGTH], 2);
  GTEXT_CSS_Math_Type inv = time;
  inv.exponent[GTEXT_CSS_MATH_BASE_TIME] = -1;
  ASSERT_TRUE(css_math_type_mul(&t, &inv, &u));
  EXPECT_EQ(u.exponent[GTEXT_CSS_MATH_BASE_TIME], -1);
  EXPECT_EQ(gtext_css_math_category_type(GTEXT_CSS_MATH_CAT_NONE).exponent[0], 0);
  // adding a length and a time fails; adding a length and a number fails; a length and itself is itself
  EXPECT_FALSE(css_math_type_add(&length, &time, &t));
  EXPECT_FALSE(css_math_type_add(&length, &number, &t));
  ASSERT_TRUE(css_math_type_add(&length, &length, &t));
  EXPECT_EQ(t.exponent[GTEXT_CSS_MATH_BASE_LENGTH], 1);
  // the Typed OM's rule: a percentage against a length is a length with the percent hint
  ASSERT_TRUE(css_math_type_add(&length, &percent, &t));
  EXPECT_EQ(t.exponent[GTEXT_CSS_MATH_BASE_LENGTH], 1);
  EXPECT_EQ(t.exponent[GTEXT_CSS_MATH_BASE_PERCENT], 0);
  EXPECT_EQ(t.percent_hint, GTEXT_CSS_MATH_BASE_LENGTH + 1);
  // a percentage against a number is not a type
  EXPECT_FALSE(css_math_type_add(&number, &percent, &t));
  // two different hints cannot be made consistent: (length + percent) + (time + percent)
  GTEXT_CSS_Math_Type lp, tp;
  ASSERT_TRUE(css_math_type_add(&length, &percent, &lp));
  ASSERT_TRUE(css_math_type_add(&time, &percent, &tp));
  EXPECT_FALSE(css_math_type_add(&lp, &tp, &t));
  EXPECT_EQ(calc("calc((10% + 1px) + (5% + 1s))"), "invalid");
  Calc d;
  read(d, "calc(10% + 1px - 5%)");
  ASSERT_TRUE(d.valid);
  EXPECT_EQ(d.math->category, GTEXT_CSS_MATH_CAT_LENGTH);
}

// ---- the functions ---------------------------------------------------------------------------------------------

TEST(CssMathFunctions, ModAndRemAreTheDraftsExamples) {
  // 10.3: "mod(18px, 5px) resolves to the value 3px"; "mod(-140deg, -90deg) ... -50deg"; "mod(-18px, 5px) ... 2px"; "rem(-18px, 5px) ... -3px";
  // "mod(140deg, -90deg) ... -40deg"; "rem(140deg, -90deg) ... 50deg"
  check({{"mod(18px, 5px)", "calc(3px)"}, {"rem(18px, 5px)", "calc(3px)"}, {"mod(-140deg, -90deg)", "calc(-50deg)"}, {"rem(-140deg, -90deg)", "calc(-50deg)"},
      {"mod(-18px, 5px)", "calc(2px)"}, {"rem(-18px, 5px)", "calc(-3px)"}, {"mod(140deg, -90deg)", "calc(-40deg)"}, {"rem(140deg, -90deg)", "calc(50deg)"},
      {"mod(1, 1)", "calc(0)"}, {"rem(1, 1)", "calc(0)"}, {"mod(7, 3)", "calc(1)"}, {"mod(-7, 3)", "calc(2)"}, {"mod(7, -3)", "calc(-2)"}, {"rem(7, -3)", "calc(1)"}});
}

TEST(CssMathFunctions, ModAndRemArgumentRanges) {
  // 10.3.1: "In mod(A, B) or rem(A, B), if B is 0, the result is NaN. If A is infinite, the result is NaN."
  // "In mod(A, B) only, if B is infinite and A has opposite sign to B (including an oppositely-signed zero), the result is NaN."
  // "All other "infinite B" cases are valid, and just return A immediately."
  check({{"mod(1, 0)", "calc(NaN)"}, {"rem(1, 0)", "calc(NaN)"}, {"mod(infinity, 1)", "calc(NaN)"}, {"rem(-infinity, 1)", "calc(NaN)"},
      {"mod(1, infinity)", "calc(1)"}, {"mod(-1, infinity)", "calc(NaN)"}, {"mod(1, -infinity)", "calc(NaN)"}, {"mod(-1, -infinity)", "calc(-1)"},
      {"rem(1, infinity)", "calc(1)"}, {"rem(-1, infinity)", "calc(-1)"}, {"mod(NaN, 1)", "calc(NaN)"}, {"mod(1px, 0px)", "calc(NaN * 1px)"}});
}

TEST(CssMathFunctions, ModAndRemUseAnExactRemainder) {
  // fmod is exact: 0.3 mod 0.1 is not 0.1 - epsilon but the exact remainder of the two doubles
  EXPECT_EQ(css_math_fmod_exact(5.5, 2.25), 1.0);
  EXPECT_EQ(css_math_fmod_exact(-5.5, 2.25), -1.0);
  EXPECT_EQ(css_math_fmod_exact(0.3, 0.1), std::fmod(0.3, 0.1));
  EXPECT_EQ(css_math_fmod_exact(1e300, 7.0), std::fmod(1e300, 7.0));
  EXPECT_EQ(css_math_fmod_exact(9007199254740993.0, 3.0), std::fmod(9007199254740993.0, 3.0));
  EXPECT_EQ(css_math_fmod_exact(5e-324, 3e-324), std::fmod(5e-324, 3e-324));
  EXPECT_EQ(css_math_fmod_exact(1.7976931348623157e308, 5e-324), std::fmod(1.7976931348623157e308, 5e-324));
  EXPECT_TRUE(std::signbit(css_math_fmod_exact(-4.0, 2.0)));
  EXPECT_FALSE(std::signbit(css_math_fmod_exact(4.0, -2.0)));
  EXPECT_TRUE(std::isnan(css_math_fmod_exact(1.0, 0.0)));
  EXPECT_TRUE(std::isnan(css_math_fmod_exact(INFINITY, 1.0)));
  EXPECT_EQ(css_math_fmod_exact(3.0, INFINITY), 3.0);
}

TEST(CssMathFunctions, RoundStrategies) {
  // 10.3: nearest (a tie goes to upper), up, down, to-zero
  check({{"round(nearest, 1.4, 1)", "calc(1)"}, {"round(nearest, 1.5, 1)", "calc(2)"}, {"round(nearest, -1.5, 1)", "calc(-1)"},
      {"round(nearest, -1.6, 1)", "calc(-2)"}, {"round(up, 1.1, 1)", "calc(2)"}, {"round(up, -1.1, 1)", "calc(-1)"}, {"round(down, 1.9, 1)", "calc(1)"},
      {"round(down, -1.1, 1)", "calc(-2)"}, {"round(to-zero, 1.9, 1)", "calc(1)"}, {"round(to-zero, -1.9, 1)", "calc(-1)"},
      {"round(1.1, 1)", "calc(1)"}, {"round(1.1px, 1px)", "calc(1px)"}, {"round(up, 10px, 3px)", "calc(12px)"}, {"round(13px, 5px)", "calc(15px)"},
      {"round(up, 7px, -5px)", "calc(10px)"}, {"round(to-zero, 17px, 5px)", "calc(15px)"}, {"round(5px, 5px)", "calc(5px)"}, {"round(2, 1)", "calc(2)"},
      {"round(down, 1.1, 1)", "calc(1)"}});
  // A may be omitted B only when it is a number: B defaults to 1
  check({{"round(1.7)", "calc(2)"}, {"round(up, 1.1)", "calc(2)"}, {"round(down, 1px)", "invalid"}});
}

TEST(CssMathFunctions, RoundArgumentRanges) {
  // 10.3.1: "if B is 0, the result is NaN. If A and B are both infinite, the result is NaN. If A is infinite but B is finite, the result is the same
  // infinity. If A is finite but B is infinite, the result depends on the strategy and the sign of A"
  check({{"round(1, 0)", "calc(NaN)"}, {"round(infinity, infinity)", "calc(NaN)"}, {"round(infinity, 1)", "calc(infinity)"}, {"round(-infinity, 1)", "calc(-infinity)"},
      {"round(up, 1, infinity)", "calc(infinity)"}, {"round(down, -1, infinity)", "calc(-infinity)"}, {"round(nearest, 1, infinity)", "calc(0)"},
      {"round(to-zero, -1, infinity)", "calc(0)"}, {"round(up, -1, infinity)", "calc(0)"}, {"round(down, 1, infinity)", "calc(0)"}});
}

TEST(CssMathFunctions, RoundTiesAndLineWidth) {
  // line-width is snapped by the device, so it stays a function (Values 4, 10.3: "A is snapped as a line width")
  check({{"round(line-width, 1.5px, 1px)", "round(line-width, 1.5px, 1px)"}, {"round(line-width, 2.5px)", "round(line-width, 2.5px)"},
      {"round(line-width, 1)", "invalid"}});
}

TEST(CssMathFunctions, MinMaxCompareWhatTheyCan) {
  check({{"min(3px, 1px, 2px)", "calc(1px)"}, {"max(3px, 1px, 2px)", "calc(3px)"}, {"min(1px, 1em, 2px)", "min(1px, 1em)"}, {"max(1px, 1em, 2px)", "max(2px, 1em)"},
      {"min(1in, 1px)", "calc(1px)"}, {"max(1in, 100px)", "calc(100px)"}, {"min(10%, 20%)", "min(10%, 20%)"}, {"max(1, 2, 3)", "calc(3)"},
      {"min(NaN, 1)", "calc(NaN)"}, {"max(1, NaN)", "calc(NaN)"}, {"min(infinity, 1)", "calc(1)"}, {"max(-infinity, 1)", "calc(1)"},
      {"min(1px, 1)", "invalid"}, {"min(1px, 1s)", "invalid"}});
}

TEST(CssMathFunctions, TrigonometricFunctionsOfNumbersAndAngles) {
  // 10.4: "sin(45deg), sin(.125turn), and sin(3.14159 / 4) all represent the same value, approximately .707"
  EXPECT_EQ(calc("sin(45deg)"), "calc(0.707107)");
  EXPECT_EQ(calc("sin(.125turn)"), "calc(0.707107)");
  EXPECT_EQ(calc("sin(45deg)"), calc("sin(50grad)"));
  EXPECT_EQ(calc("sin(3.14159 / 4)"), "calc(0.707106)");
  check({{"cos(0)", "calc(1)"}, {"sin(0)", "calc(0)"}, {"tan(0)", "calc(0)"}, {"sin(30deg)", "calc(0.5)"}, {"cos(60deg)", "calc(0.5)"}, {"tan(45deg)", "calc(1)"},
      {"cos(180deg)", "calc(-1)"}, {"sin(270deg)", "calc(-1)"}, {"sin(1px)", "invalid"}, {"cos(1%)", "invalid"}, {"sin(1s)", "invalid"},
      {"sin(infinity)", "calc(NaN)"}, {"cos(-infinity)", "calc(NaN)"}, {"tan(NaN)", "calc(NaN)"}, {"sin(1)", "calc(0.841471)"}});
  EXPECT_EQ(category_of("sin(1deg)"), GTEXT_CSS_MATH_CAT_NUMBER);
}

TEST(CssMathFunctions, InverseTrigonometricFunctionsAreAngles) {
  // 10.4: asin in [-90deg, 90deg], acos in [0deg, 180deg], atan in [-90deg, 90deg], atan2 in (-180deg, 180deg]
  check({{"asin(1)", "calc(90deg)"}, {"asin(-1)", "calc(-90deg)"}, {"asin(0.5)", "calc(30deg)"}, {"acos(1)", "calc(0deg)"}, {"acos(-1)", "calc(180deg)"},
      {"acos(0.5)", "calc(60deg)"}, {"atan(1)", "calc(45deg)"}, {"atan(infinity)", "calc(90deg)"}, {"atan(-infinity)", "calc(-90deg)"},
      {"asin(2)", "calc(NaN * 1deg)"}, {"acos(-2)", "calc(NaN * 1deg)"}, {"atan(NaN)", "calc(NaN * 1deg)"}, {"asin(1px)", "invalid"}, {"atan(1deg)", "invalid"}});
  EXPECT_EQ(category_of("asin(0.5)"), GTEXT_CSS_MATH_CAT_ANGLE);
}

TEST(CssMathFunctions, Atan2TakesTheQuadrantIntoAccount) {
  // 10.4: "atan2(1, -1), corresponding to the point (-1, 1), returns 135deg, distinct from atan2(-1, 1) ... -45deg"
  check({{"atan2(1, -1)", "calc(135deg)"}, {"atan2(-1, 1)", "calc(-45deg)"}, {"atan2(1, 1)", "calc(45deg)"}, {"atan2(-1, -1)", "calc(-135deg)"},
      {"atan2(1px, 1px)", "calc(45deg)"}, {"atan2(1px, 1)", "invalid"}, {"atan2(0, -1)", "calc(180deg)"}, {"atan2(0, 1)", "calc(0deg)"},
      {"atan2(1, 0)", "calc(90deg)"}, {"atan2(-1, 0)", "calc(-90deg)"}, {"atan2(infinity, infinity)", "calc(45deg)"}, {"atan2(-infinity, -infinity)", "calc(-135deg)"},
      {"atan2(1, infinity)", "calc(0deg)"}, {"atan2(1, -infinity)", "calc(180deg)"}});
}

TEST(CssMathFunctions, ExponentialFunctions) {
  check({{"pow(2, 10)", "calc(1024)"}, {"pow(2, -1)", "calc(0.5)"}, {"sqrt(16)", "calc(4)"}, {"sqrt(2)", "calc(1.414214)"}, {"exp(0)", "calc(1)"},
      {"exp(1)", "calc(2.718282)"}, {"log(1)", "calc(0)"}, {"log(e)", "calc(1)"}, {"log(8, 2)", "calc(3)"}, {"log(100, 10)", "calc(2)"}, {"hypot(30px, 40px)", "calc(50px)"},
      {"hypot(3, 4)", "calc(5)"}, {"hypot(-5px)", "calc(5px)"}, {"hypot(1px, 1s)", "invalid"}, {"pow(1px, 2)", "invalid"}, {"sqrt(1px)", "invalid"},
      {"hypot(2em, 3em)", "hypot(2em, 3em)"}, {"hypot(1, 2, 2)", "calc(3)"}, {"log(1px)", "invalid"}, {"exp(1%)", "invalid"}, {"pow(2)", "invalid"}});
}

TEST(CssMathFunctions, PowArgumentRanges) {
  // 10.5.1: "In pow(A, B), if A is negative and finite, and B is finite, B must be an integer, or else the result is NaN." and the tables
  check({{"pow(-8, 1)", "calc(-8)"}, {"pow(-2, 3)", "calc(-8)"}, {"pow(-2, 0.5)", "calc(NaN)"}, {"pow(0, 0)", "calc(1)"}, {"pow(0, -1)", "calc(infinity)"},
      {"pow(0, 1)", "calc(0)"}, {"pow(infinity, 0)", "calc(1)"}, {"pow(infinity, 1)", "calc(infinity)"}, {"pow(infinity, -1)", "calc(0)"},
      {"pow(-infinity, 3)", "calc(-infinity)"}, {"pow(-infinity, 2)", "calc(infinity)"}, {"pow(-infinity, -3)", "calc(0)"},
      {"pow(2, infinity)", "calc(infinity)"}, {"pow(2, -infinity)", "calc(0)"}, {"pow(0.5, infinity)", "calc(0)"}, {"pow(0.5, -infinity)", "calc(infinity)"},
      {"pow(1, infinity)", "calc(NaN)"}, {"pow(-1, infinity)", "calc(NaN)"}, {"pow(1, -infinity)", "calc(NaN)"}, {"pow(NaN, 0)", "calc(NaN)"}, {"pow(1, NaN)", "calc(NaN)"}});
  // the signs of zero
  double v;
  ASSERT_TRUE(value_of("calc(pow(-0, 3))", &v));
  EXPECT_TRUE(std::signbit(v) && v == 0.0);
  ASSERT_TRUE(value_of("calc(pow(-0, 2))", &v));
  EXPECT_FALSE(std::signbit(v));
  ASSERT_TRUE(value_of("calc(pow(-0, -3))", &v));
  EXPECT_EQ(v, -INFINITY);
}

TEST(CssMathFunctions, SqrtLogExpHypotArgumentRanges) {
  check({{"sqrt(-1)", "calc(NaN)"}, {"sqrt(infinity)", "calc(infinity)"}, {"log(-1)", "calc(NaN)"}, {"log(0)", "calc(-infinity)"}, {"log(infinity)", "calc(infinity)"},
      {"log(2, 1)", "calc(NaN)"}, {"log(2, -2)", "calc(NaN)"}, {"exp(infinity)", "calc(infinity)"}, {"exp(-infinity)", "calc(0)"}, {"hypot(infinity, 1)", "calc(infinity)"},
      {"hypot(-infinity)", "calc(infinity)"}, {"hypot(NaN, infinity)", "calc(NaN)"}, {"log(8, 0.5)", "calc(-3)"}});
  double v;
  ASSERT_TRUE(value_of("calc(sqrt(-0))", &v));
  EXPECT_TRUE(std::signbit(v) && v == 0.0);
  ASSERT_TRUE(value_of("calc(log(1))", &v));
  EXPECT_FALSE(std::signbit(v));
  ASSERT_TRUE(value_of("calc(log(1, 0.5))", &v));
  EXPECT_FALSE(std::signbit(v));  // "If A is 1, the result is 0+"
}

TEST(CssMathFunctions, AbsAndSign) {
  check({{"abs(-5px)", "calc(5px)"}, {"abs(5px)", "calc(5px)"}, {"abs(-1.5)", "calc(1.5)"}, {"abs(-1em)", "abs(-1em)"}, {"abs(-10%)", "abs(-10%)"},
      {"sign(-5px)", "calc(-1)"}, {"sign(5px)", "calc(1)"}, {"sign(0px)", "calc(0)"}, {"sign(-1em)", "sign(-1em)"}, {"sign(NaN)", "calc(NaN)"},
      {"abs(NaN)", "calc(NaN)"}, {"abs(-infinity)", "calc(infinity)"}, {"sign(-infinity)", "calc(-1)"}});
  EXPECT_EQ(category_of("sign(5px)"), GTEXT_CSS_MATH_CAT_NUMBER);
  EXPECT_EQ(category_of("abs(-5px)"), GTEXT_CSS_MATH_CAT_LENGTH);
  double v;
  ASSERT_TRUE(value_of("calc(sign(-0))", &v));
  EXPECT_TRUE(std::signbit(v));
  ASSERT_TRUE(value_of("calc(abs(-0))", &v));
  EXPECT_FALSE(std::signbit(v));
}

TEST(CssMathFunctions, ProgressIsADistanceAlongARange) {
  // Values 5, 6: "(progress value - progress start value) / (progress end value - progress start value), clamped to the [0,1] range if no-clamp is not specified."
  check({{"progress(100px, 0px, 100px)", "calc(1)"}, {"progress(50px, 0px, 100px)", "calc(0.5)"}, {"progress(200px, 0px, 100px)", "calc(1)"},
      {"progress(-100px, 0px, 100px)", "calc(0)"}, {"progress(no-clamp 200px, 0px, 100px)", "calc(2)"}, {"progress(no-clamp -100px, 0px, 100px)", "calc(-1)"},
      {"progress(10em, 0px, 10em)", "progress(10em, 0px, 10em)"}, {"progress(2rad, 1rad, 1rad)", "calc(0)"}, {"progress(no-clamp 2, 1, 1)", "calc(infinity)"},
      {"progress(no-clamp 0, 1, 1)", "calc(-infinity)"}, {"progress(no-clamp 1, 1, 1)", "calc(0)"}, {"progress(1px, 2px)", "invalid"},
      {"progress(1px, 2px, 3s)", "invalid"}, {"progress(10px * 10px, 10px * 10px, 10px * 10px)", "invalid"}});
}

TEST(CssMathFunctions, RandomIsNotComputedHere) {
  check({{"random(0px, 100px)", "random(0px, 100px)"}, {"random(--foo, 0px, 100px)", "random(--foo, 0px, 100px)"},
      {"random(--foo element-scoped, 0px, 100px, 5px)", "random(--foo element-scoped, 0px, 100px, 5px)"}, {"random(fixed 0.5, 1, 2)", "random(fixed 0.5, 1, 2)"},
      {"random(fixed 1.5, 1, 2)", "invalid"}, {"random(auto element-scoped, 1, 2)", "invalid"}, {"random(--a --b, 1, 2)", "invalid"},
      {"random(1px, 2s)", "invalid"}, {"random(1)", "invalid"}});
}

// ---- signed zeros, infinities, NaN (10.9.2) -----------------------------------------------------------------------

TEST(CssMathSignedZero, MultiplicationAndDivisionFollowTheSignRules) {
  // wpt css/css-values/signed-zero.html: the sign of a zero is seen through the sign of the infinity it divides into
  struct Z {
    const char * input;
    bool negative;
  };
  for (const Z & z : std::vector<Z>{{"calc(-0)", true}, {"calc(0)", false}, {"calc(-0 * -1)", false}, {"calc(-0 * 1)", true}, {"calc(0 * -1)", true},
           {"calc(0 * 1)", false}, {"calc(-1 / -infinity)", false}, {"calc(-1 / infinity)", true}, {"calc(1 / -infinity)", true}, {"calc(1 / infinity)", false},
           {"calc(-0 + -0)", true}, {"calc(-0 + 0)", false}, {"calc(0 + -0)", false}, {"calc(0 + 0)", false}, {"calc(-0 - -0)", false},
           {"calc(-0 - 0)", true}, {"calc(0 - -0)", false}, {"calc(0 - 0)", false}, {"min(-0, 0)", true}, {"min(0, -0)", true}, {"max(-0, 0)", false},
           {"max(0, -0)", false}, {"clamp(-0, -1, -0)", true}, {"clamp(-0, -0, 0)", true}, {"clamp(-0, 0, 0)", false}, {"clamp(0, -1, -0)", false},
           {"round(nearest, -1, infinity)", true}, {"round(nearest, -0, infinity)", true}, {"round(nearest, 0, infinity)", false}, {"round(up, -1, infinity)", true},
           {"round(down, 1, infinity)", false}, {"mod(-1, -1)", true}, {"mod(-1, 1)", false}, {"mod(1, -1)", true}, {"mod(1, 1)", false}, {"rem(-1, -1)", true},
           {"rem(-1, 1)", true}, {"rem(1, -1)", false}, {"rem(1, 1)", false}, {"asin(-0)", true}, {"asin(0)", false}, {"atan(-0)", true}, {"atan(0)", false},
           {"atan2(-0, 0)", true}, {"atan2(-0, 1)", true}, {"sin(-0)", true}, {"tan(-0)", true}}) {
    double v;
    ASSERT_TRUE(value_of(z.input, &v)) << z.input;
    EXPECT_EQ(v, 0.0) << z.input;
    EXPECT_EQ(std::signbit(v), z.negative) << z.input;
  }
}

TEST(CssMathSignedZero, ASignedZeroDoesNotEscapeATopLevelCalculation) {
  // the range check censors it (gtext_css_math_clamp), and the serialization writes a plain 0
  EXPECT_EQ(calc("calc(-0)"), "calc(0)");
  EXPECT_FALSE(std::signbit(gtext_css_math_clamp(-0.0, -INFINITY, INFINITY)));
  EXPECT_EQ(gtext_css_math_clamp(NAN, -5, 5), 0.0);
  EXPECT_EQ(gtext_css_math_clamp(INFINITY, -5, 5), 5.0);
  EXPECT_EQ(gtext_css_math_clamp(-INFINITY, -5, 5), -5.0);
  EXPECT_EQ(gtext_css_math_clamp(7.0, 0, 5), 5.0);
  EXPECT_EQ(gtext_css_math_clamp(-7.0, 0, 5), 0.0);
  EXPECT_EQ(gtext_css_math_clamp(3.0, 0, 5), 3.0);
}

TEST(CssMathSignedZero, InfinitiesAndNaNFollowTheDraftsRules) {
  // 10.9.2
  check({{"calc(infinity + -infinity)", "calc(NaN)"}, {"calc(infinity - infinity)", "calc(NaN)"}, {"calc(infinity * 0)", "calc(NaN)"},
      {"calc(infinity / infinity)", "calc(NaN)"}, {"calc(0 / 0)", "calc(NaN)"}, {"calc(1 / infinity)", "calc(0)"}, {"calc(infinity + 1)", "calc(infinity)"},
      {"calc(-infinity - 1)", "calc(-infinity)"}, {"calc(infinity * -1)", "calc(-infinity)"}, {"calc(NaN + 1)", "calc(NaN)"}, {"calc(NaN * 0)", "calc(NaN)"},
      {"calc(1px * (infinity + -infinity))", "calc(NaN * 1px)"}, {"calc(1px * 1/infinity)", "calc(0px)"}, {"calc(1px * infinity * infinity)", "calc(infinity * 1px)"},
      {"calc(1px * -infinity * -infinity)", "calc(infinity * 1px)"}, {"calc(1in * NaN)", "calc(NaN * 1px)"}, {"calc(1% * NaN)", "calc(NaN * 1%)"},
      {"calc(1 * max(INFinity*3px, 0px))", "calc(infinity * 1px)"}, {"calc(1 * min(inFInity*4px, 0px))", "calc(0px)"},
      {"calc(1px * clamp(-Infinity, 0, infinity))", "calc(0px)"}, {"calc(1px * clamp(-inFinity, infinity, 10))", "calc(10px)"},
      {"calc(1px * max(NaN, min(0,10)))", "calc(NaN * 1px)"}, {"calc(1deg * infinity)", "calc(infinity * 1deg)"}, {"calc(1s * NaN)", "calc(NaN * 1s)"},
      {"calc(1dppx * -infinity)", "calc(-infinity * 1dppx)"}});
}

// ---- resolving a calculation in an environment ---------------------------------------------------------------------

namespace {
bool unit_size(void *, GTEXT_CSS_Unit unit, double * canonical) {
  switch (unit) {
  case GTEXT_CSS_UNIT_EM: *canonical = 16.0; return true;
  case GTEXT_CSS_UNIT_REM: *canonical = 10.0; return true;
  default: return false;
  }
}
bool symbol_value(void *, const char * name, double * v) {
  if (std::string(name) == "r") {
    *v = 3.0;
    return true;
  }
  return false;
}
}  // namespace

TEST(CssMathResolve, ARelativeUnitAndAPercentageAreResolvedByTheEnvironment) {
  Calc c;
  read(c, "calc(2em + 10% + 5px)");
  ASSERT_TRUE(c.valid);
  GTEXT_CSS_Math_Env env;
  std::memset(&env, 0, sizeof env);
  double v = 0;
  bool resolved = true;
  ASSERT_EQ(gtext_css_math_resolve(c.math, nullptr, nullptr, &v, &resolved), GTEXT_CSS_OK);
  EXPECT_FALSE(resolved);
  env.unit_size = unit_size;
  ASSERT_EQ(gtext_css_math_resolve(c.math, &env, nullptr, &v, &resolved), GTEXT_CSS_OK);
  EXPECT_FALSE(resolved);  // a percentage with no basis
  env.has_percent_basis = true;
  env.percent_basis = 200.0;
  ASSERT_EQ(gtext_css_math_resolve(c.math, &env, nullptr, &v, &resolved), GTEXT_CSS_OK);
  EXPECT_TRUE(resolved);
  EXPECT_EQ(v, 2 * 16.0 + 20.0 + 5.0);
  Calc d;
  read(d, "calc(1em + 1ex)");
  ASSERT_EQ(gtext_css_math_resolve(d.math, &env, nullptr, &v, &resolved), GTEXT_CSS_OK);
  EXPECT_FALSE(resolved);  // ex is not known to this environment
}

TEST(CssMathResolve, FunctionsOfRelativeValuesAreComputedOnceTheyAreKnown) {
  GTEXT_CSS_Math_Env env;
  std::memset(&env, 0, sizeof env);
  env.unit_size = unit_size;
  struct R {
    const char * input;
    double expect;
  };
  for (const R & r : std::vector<R>{{"min(1em, 100px)", 16.0}, {"max(1em, 100px)", 100.0}, {"clamp(1px, 1em, 10px)", 10.0}, {"abs(-1em)", 16.0},
           {"sign(-1em)", -1.0}, {"hypot(1em, 12px)", 20.0}, {"round(up, 1.1em, 10px)", 20.0}, {"mod(1em, 5px)", 1.0}, {"calc(1em / 1px)", 16.0},
           {"sin(0.5rad * 0)", 0.0}}) {
    Calc c;
    read(c, r.input);
    ASSERT_TRUE(c.valid) << r.input;
    double v = 0;
    bool resolved = false;
    ASSERT_EQ(gtext_css_math_resolve(c.math, &env, nullptr, &v, &resolved), GTEXT_CSS_OK) << r.input;
    EXPECT_TRUE(resolved) << r.input;
    EXPECT_DOUBLE_EQ(v, r.expect) << r.input;
  }
  Calc s;
  GTEXT_CSS_Math_Keyword kw[] = {{"r", {{0, 0, 0, 0, 0, 0, 0}, 0}}};
  GTEXT_CSS_Math_Context ctx = {kw, 1};
  read(s, "calc(r * 2)", &ctx);
  ASSERT_TRUE(s.valid);
  EXPECT_TRUE(s.math->has_symbols);
  env.symbol = symbol_value;
  double v = 0;
  bool resolved = false;
  ASSERT_EQ(gtext_css_math_resolve(s.math, &env, nullptr, &v, &resolved), GTEXT_CSS_OK);
  EXPECT_TRUE(resolved);
  EXPECT_EQ(v, 6.0);
}

// ---- limits and allocation ---------------------------------------------------------------------------------------

TEST(CssMathLimits, TheNumberOfNodesIsBounded) {
  GTEXT_CSS_Math_Options o = gtext_css_math_options_default();
  std::string wide = "min(";
  for (int i = 0; i < 100; i++) {
    wide += (i ? ", " : "") + std::to_string(i) + "em";
  }
  wide += ")";
  o.max_nodes = 1000;
  Calc ok;
  read(ok, wide, nullptr, &o);
  EXPECT_EQ(ok.status, GTEXT_CSS_OK);
  EXPECT_TRUE(ok.valid);
  const size_t nodes = ok.result.nodes;
  o.max_nodes = nodes;
  Calc at;
  read(at, wide, nullptr, &o);
  EXPECT_EQ(at.status, GTEXT_CSS_OK);
  o.max_nodes = nodes - 1;
  Calc over;
  read(over, wide, nullptr, &o);
  EXPECT_EQ(over.status, GTEXT_CSS_E_LIMIT_MATH_NODES);
}

TEST(CssMathLimits, DeepNestingIsNotARecursion) {
  // the default limit is 256; with the largest limit, a nesting of 20000 parentheses is read without a deep C stack
  GTEXT_CSS_Math_Options o = gtext_css_math_options_default();
  o.max_depth = 65536;
  const int n = 20000;
  std::string s = "calc(";
  for (int i = 0; i < n; i++) {
    s += "(";
  }
  s += "1px";
  for (int i = 0; i < n; i++) {
    s += ")";
  }
  s += ")";
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  po.max_depth = 100000;
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_component_values, s, &po);
  ASSERT_EQ(p.status, GTEXT_CSS_OK);
  GTEXT_CSS_Math * m = nullptr;
  bool valid = false;
  ASSERT_EQ(gtext_css_math_parse(&p.tree->values[0], nullptr, &o, &valid, &m, nullptr), GTEXT_CSS_OK);
  ASSERT_TRUE(valid);
  EXPECT_EQ(serialize(m), "calc(1px)");
  gtext_css_math_free(m);
  // and a long sum is not a deep one
  std::string sum = "calc(1px";
  for (int i = 0; i < 30000; i++) {
    sum += " + 1px";
  }
  sum += ")";
  EXPECT_EQ(calc(sum), "calc(30001px)");
}

TEST(CssMathLimits, AnAllocationThatFailsIsOutOfMemoryAndLeaksNothing) {
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_component_values, "calc(1px + 2em * 3 - min(4px, 5%) + clamp(1px, 2em, 3vw))");
  ASSERT_EQ(p.status, GTEXT_CSS_OK);
  bool saw_ok = false;
  for (size_t n = 1; n < 400; n++) {
    FaultAllocator fa;
    fa.arm(n);
    GTEXT_CSS_Math_Options o = gtext_css_math_options_default();
    o.allocator = &fa.alloc;
    GTEXT_CSS_Math * m = nullptr;
    bool valid = false;
    const GTEXT_CSS_Status st = gtext_css_math_parse(&p.tree->values[0], nullptr, &o, &valid, &m, nullptr);
    if (st == GTEXT_CSS_OK) {
      EXPECT_TRUE(valid);
      EXPECT_EQ(serialize(m), "calc(6em + 1px - min(4px, 5%) + clamp(1px, 2em, 3vw))");
      gtext_css_math_free(m);
      saw_ok = true;
      fa.disarm();
      EXPECT_TRUE(fa.live.empty());
      break;
    }
    EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
    EXPECT_EQ(m, nullptr);
    fa.disarm();
    EXPECT_TRUE(fa.live.empty()) << "leak at failure " << n;
  }
  EXPECT_TRUE(saw_ok);
}

// ---- the published value family of the elementary functions ------------------------------------------------------

namespace {

struct Family {
  std::string fn;
  std::vector<double> args;
  double expect;
};

std::vector<Family> load_family() {
  const char * dir = std::getenv("TEST_DATA_DIR");
  const std::string path = std::string(dir ? dir : "tests/data") + "/css-math-values.txt";
  std::ifstream in(path);
  std::vector<Family> out;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    std::istringstream is(line);
    Family f;
    is >> f.fn;
    std::vector<double> nums;
    std::string tok;
    while (is >> tok) {
      nums.push_back(std::strtod(tok.c_str(), nullptr));
    }
    f.expect = nums.back();
    nums.pop_back();
    f.args = nums;
    out.push_back(f);
  }
  return out;
}

/// Units in the last place between two doubles (the order of the representation).
int64_t ulps(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) {
    return std::isnan(a) && std::isnan(b) ? 0 : INT64_MAX;
  }
  auto key = [](double d) {
    int64_t i;
    std::memcpy(&i, &d, sizeof i);
    return i < 0 ? INT64_MIN - i : i;
  };
  const int64_t x = key(a), y = key(b);
  return x > y ? x - y : y - x;
}

bool compute(const Family & f, double * out) {
  const std::string & fn = f.fn;
  const std::vector<double> & a = f.args;
  if (fn == "sin") return css_math_apply(GTEXT_CSS_MATH_SIN, GTEXT_CSS_MATH_ROUND_NEAREST, a.data(), 1, out);
  if (fn == "cos") return css_math_apply(GTEXT_CSS_MATH_COS, GTEXT_CSS_MATH_ROUND_NEAREST, a.data(), 1, out);
  if (fn == "tan") return css_math_apply(GTEXT_CSS_MATH_TAN, GTEXT_CSS_MATH_ROUND_NEAREST, a.data(), 1, out);
  if (fn == "asin") return css_math_apply(GTEXT_CSS_MATH_ASIN, GTEXT_CSS_MATH_ROUND_NEAREST, a.data(), 1, out);
  if (fn == "acos") return css_math_apply(GTEXT_CSS_MATH_ACOS, GTEXT_CSS_MATH_ROUND_NEAREST, a.data(), 1, out);
  if (fn == "atan") return css_math_apply(GTEXT_CSS_MATH_ATAN, GTEXT_CSS_MATH_ROUND_NEAREST, a.data(), 1, out);
  if (fn == "atan2") return css_math_apply(GTEXT_CSS_MATH_ATAN2, GTEXT_CSS_MATH_ROUND_NEAREST, a.data(), 2, out);
  if (fn == "exp") return css_math_apply(GTEXT_CSS_MATH_EXP, GTEXT_CSS_MATH_ROUND_NEAREST, a.data(), 1, out);
  if (fn == "log") return css_math_apply(GTEXT_CSS_MATH_LOG, GTEXT_CSS_MATH_ROUND_NEAREST, a.data(), 1, out);
  if (fn == "log2") return css_math_apply(GTEXT_CSS_MATH_LOG, GTEXT_CSS_MATH_ROUND_NEAREST, a.data(), 2, out);
  if (fn == "sqrt") return css_math_apply(GTEXT_CSS_MATH_SQRT, GTEXT_CSS_MATH_ROUND_NEAREST, a.data(), 1, out);
  if (fn == "pow") return css_math_apply(GTEXT_CSS_MATH_POW, GTEXT_CSS_MATH_ROUND_NEAREST, a.data(), 2, out);
  if (fn == "hypot" || fn == "hypot3") return css_math_apply(GTEXT_CSS_MATH_HYPOT, GTEXT_CSS_MATH_ROUND_NEAREST, a.data(), a.size(), out);
  if (fn == "fmod") {
    *out = css_math_fmod_exact(a[0], a[1]);
    return true;
  }
  return false;
}

/// The most units in the last place the library may be from the correctly rounded value, for each function: what was measured over the
/// family (CSS_MATH_REPORT=1 prints it), and no more. The functions that are exact are held to exactly that; cos, asin, acos, atan2,
/// log with a base and hypot of three are one unit off at their worst. A change that makes one of them worse than it is fails here.
int64_t bound_of(const std::string & fn) {
  if (fn == "cos" || fn == "asin" || fn == "acos" || fn == "atan2" || fn == "log2" || fn == "hypot3") return 1;
  return 0;
}

using ComputeFn = bool (*)(const Family &, double *);

struct FamilyOutcome {
  std::map<std::string, int64_t> worst;
  std::map<std::string, size_t> count;
  std::vector<std::string> over;  ///< The values that were farther than their bound, as text.
};

/// The comparison: every value of the family, computed by `fn`, against its correctly rounded value and the bound of its function.
FamilyOutcome compare_family(const std::vector<Family> & family, ComputeFn fn) {
  FamilyOutcome out;
  for (const Family & f : family) {
    double got = 0;
    if (!fn(f, &got)) {
      out.over.push_back(f.fn + ": not computed");
      continue;
    }
    const int64_t u = ulps(got, f.expect);
    out.count[f.fn]++;
    if (u > out.worst[f.fn]) {
      out.worst[f.fn] = u;
    }
    if (u > bound_of(f.fn)) {
      out.over.push_back(f.fn + "(" + std::to_string(f.args[0]) + (f.args.size() > 1 ? ", " + std::to_string(f.args[1]) : "") + ") = " + std::to_string(got) +
          ", expected " + std::to_string(f.expect));
    }
  }
  return out;
}

/// A stand-in for the math library that is worse than any bound: every result is moved bound + 1 units in the last place.
bool degraded(const Family & f, double * out) {
  if (!compute(f, out)) {
    return false;
  }
  for (int64_t i = 0; i <= bound_of(f.fn); i++) {
    *out = std::nextafter(*out, INFINITY);
  }
  return true;
}

const char * const kFamilyFunctions[] = {"sin", "cos", "tan", "asin", "acos", "atan", "atan2", "exp", "log", "log2", "sqrt", "pow", "hypot", "hypot3", "fmod"};

}  // namespace

TEST(CssMathPublishedValues, EveryElementaryFunctionIsWithinItsBoundOfTheCorrectlyRoundedValue) {
  const std::vector<Family> family = load_family();
  ASSERT_GT(family.size(), 1000u) << "tests/data/css-math-values.txt (tools/css/make-math-values.py)";
  const FamilyOutcome r = compare_family(family, compute);
  for (const std::string & line : r.over) {
    ADD_FAILURE() << line;
  }
  for (const char * fn : kFamilyFunctions) {
    EXPECT_GE(r.count.count(fn) ? r.count.at(fn) : 0u, 7u) << fn;
    if (std::getenv("CSS_MATH_REPORT")) {
      std::printf("worst %-6s %2lld ulps over %zu values (bound %lld)\n", fn, (long long)r.worst.at(fn), r.count.at(fn), (long long)bound_of(fn));
    }
  }
}

TEST(CssMathPublishedValues, TheComparisonFailsForAStandInThatIsOneUnitPastEveryBound) {
  // the control: the whole comparison, run on a degraded stand-in for the library's computation, reports every function
  const std::vector<Family> family = load_family();
  const FamilyOutcome r = compare_family(family, degraded);
  EXPECT_FALSE(r.over.empty());
  for (const char * fn : kFamilyFunctions) {
    bool reported = false;
    for (const std::string & line : r.over) {
      reported = reported || line.compare(0, std::strlen(fn) + 1, std::string(fn) + "(") == 0;
    }
    EXPECT_TRUE(reported) << fn << " is not reported by the comparison when its results are past the bound";
  }
  // and the bounds are tight: the measured worst of the real computation is the bound for the functions that are not exact
  const FamilyOutcome real = compare_family(family, compute);
  for (const char * fn : kFamilyFunctions) {
    EXPECT_EQ(real.worst.at(fn), bound_of(fn)) << fn << ": the bound is not what was measured (tighten it, or find the regression)";
  }
}

TEST(CssMathPublishedValues, ExactValuesAndEdgeCasesOfTheDraft) {
  // values that must be exact (or exactly the draft's)
  double a[2], r;
  a[0] = 0.0;
  ASSERT_TRUE(css_math_apply(GTEXT_CSS_MATH_SIN, GTEXT_CSS_MATH_ROUND_NEAREST, a, 1, &r));
  EXPECT_EQ(r, 0.0);
  ASSERT_TRUE(css_math_apply(GTEXT_CSS_MATH_COS, GTEXT_CSS_MATH_ROUND_NEAREST, a, 1, &r));
  EXPECT_EQ(r, 1.0);
  a[0] = 1.0;
  ASSERT_TRUE(css_math_apply(GTEXT_CSS_MATH_ACOS, GTEXT_CSS_MATH_ROUND_NEAREST, a, 1, &r));
  EXPECT_EQ(r, 0.0);  // "In acos(A), if A is exactly 1, the result is 0."
  ASSERT_TRUE(css_math_apply(GTEXT_CSS_MATH_ASIN, GTEXT_CSS_MATH_ROUND_NEAREST, a, 1, &r));
  EXPECT_EQ(r, std::atan2(1.0, 0.0));
  a[0] = 4.0;
  ASSERT_TRUE(css_math_apply(GTEXT_CSS_MATH_SQRT, GTEXT_CSS_MATH_ROUND_NEAREST, a, 1, &r));
  EXPECT_EQ(r, 2.0);
  a[0] = 30.0;
  a[1] = 40.0;
  ASSERT_TRUE(css_math_apply(GTEXT_CSS_MATH_HYPOT, GTEXT_CSS_MATH_ROUND_NEAREST, a, 2, &r));
  EXPECT_EQ(r, 50.0);
  a[0] = 2.0;
  a[1] = 10.0;
  ASSERT_TRUE(css_math_apply(GTEXT_CSS_MATH_POW, GTEXT_CSS_MATH_ROUND_NEAREST, a, 2, &r));
  EXPECT_EQ(r, 1024.0);
}
