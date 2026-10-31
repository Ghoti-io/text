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
 * The last named defects of the computed-value completion pass (the property vectors that were left open when the exception lists were
 * closed): one test for each, quoting the sentence of the served drafts that decides it.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_compute_test_support.h"

using namespace cmp;

namespace {

std::string P(const std::string & property, const std::string & value) {
  return reg::parse(R(), property, value);
}

/* CSS Sizing 4, 7: "<calc-size()> = calc-size( <calc-size-basis>, <calc-sum> )" and "Within the calc-size calculation, the size is a <calc-keyword> which is a <length>,
   and resolves at used value time to the size specified above." A basis keyword is "any sizing keywords allowed in the context where the function is used. For
   example, in width, it matches auto, min-content, stretch, etc."; "Note: The none keyword is not usable within calc-size().". */
TEST(CssCalcSize, TheSizeKeywordIsALengthInTheCalculation) {
  EXPECT_EQ(P("width", "calc-size(auto, size)"), "valid:calc-size(auto, size)");
  EXPECT_EQ(P("height", "calc-size(max-content, size)"), "valid:calc-size(max-content, size)");
  EXPECT_EQ(P("width", "calc-size(fit-content, size)"), "valid:calc-size(fit-content, size)");
  EXPECT_EQ(P("width", "calc-size(10px, sign(size) * size)"), "valid:calc-size(10px, sign(size) * size)");
  // a calculation, not the text of one: the product of two sizes is an area
  EXPECT_EQ(P("width", "calc-size(auto, size * size)"), "invalid");
  // `size` outside calc-size() is nothing
  EXPECT_EQ(P("width", "size"), "invalid");
  EXPECT_EQ(P("width", "calc(50px * sign(size))"), "invalid");
}

TEST(CssCalcSize, TheBasisKeywordsAreTheOnesOfTheProperty) {
  EXPECT_EQ(P("min-width", "calc-size(auto, size)"), "valid:calc-size(auto, size)");
  EXPECT_EQ(P("max-width", "calc-size(auto, size)"), "invalid");      // max-width has no auto
  EXPECT_EQ(P("max-width", "calc-size(max-content, size / 2)"), "valid:calc-size(max-content, 0.5 * size)");
  EXPECT_EQ(P("width", "calc-size(none, size)"), "invalid");
  EXPECT_EQ(P("max-width", "calc-size(none, size)"), "invalid");
  EXPECT_EQ(P("flex-basis", "calc-size(content, size)"), "valid:calc-size(content, size)");
  EXPECT_EQ(P("width", "calc-size(content, size)"), "invalid");
  EXPECT_EQ(P("flex-basis", "calc-size(auto, size)"), "valid:calc-size(auto, size)");
  // "Also allows any related sizing functions, like the fit-content() function in width/etc."
  EXPECT_EQ(P("width", "calc-size(fit-content(20px), size)"), "valid:calc-size(fit-content(20px), size)");
  EXPECT_EQ(P("width", "calc-size(fit-content(2in), size + 1px)"), "valid:calc-size(fit-content(192px), 1px + size)");
  EXPECT_EQ(P("width", "calc-size(fit-content(), size)"), "invalid");
  EXPECT_EQ(P("width", "calc-size(fit-content(20), size)"), "invalid");
  EXPECT_EQ(P("width", "calc-size(circle(20px), size)"), "invalid");
  EXPECT_EQ(root_computed("width: calc-size(fit-content(2em), size)", "width"), "calc-size(fit-content(32px), size)");
}

/* "calc-size(any, ...) ... The calc-size calculation cannot use the size keyword; it is a syntax error for size to appear in the calc-size calculation." and
   "The size keyword is allowed, in this case, even if the inner calc-size() uses any." */
TEST(CssCalcSize, AnyHasNoSizeButANestedAnyGivesItToTheOuter) {
  EXPECT_EQ(P("width", "calc-size(any, 25em)"), "valid:calc-size(any, 25em)");
  EXPECT_EQ(P("width", "calc-size(any, size)"), "invalid");
  EXPECT_EQ(P("width", "calc-size(any, fit-content)"), "invalid");
  EXPECT_EQ(P("width", "calc-size(calc-size(any, 30px), size)"), "valid:calc-size(calc-size(any, 30px), size)");
  EXPECT_EQ(P("width", "calc-size(calc-size(min-content, size), size)"), "valid:calc-size(calc-size(min-content, size), size)");
  EXPECT_EQ(P("width", "calc-size(any, calc-size(any, 10px))"), "invalid");
  EXPECT_EQ(P("width", "calc-size(any, calc-size(10px, sign(size) * size))"), "invalid");
}

/* "However, a <calc-size()> is specifically not a <length>; any place that wants to accept calc-size() must explicitly include it in its grammar." */
TEST(CssCalcSize, ACalcSizeIsNotALength) {
  EXPECT_EQ(P("width", "calc(calc-size(auto, size))"), "invalid");
  EXPECT_EQ(P("width", "calc(abs(calc-size(auto, size)))"), "invalid");
  EXPECT_EQ(P("width", "min(calc-size(auto, 0px), calc-size(auto, size))"), "invalid");
  EXPECT_EQ(P("height", "calc(12% + calc-size(any, 31%))"), "invalid");
}

/* "For either argument, if a <calc-sum> is given, its type must match <length-percentage>, and it must resolve to a <length>." */
TEST(CssCalcSize, ACalculationOfAnotherTypeIsRefused) {
  EXPECT_EQ(P("width", "calc-size(0px, 0px)"), "valid:calc-size(0px, 0px)");
  EXPECT_EQ(P("width", "calc-size(0, 0px)"), "invalid");
  EXPECT_EQ(P("width", "calc-size(0px, 0)"), "invalid");
  EXPECT_EQ(P("width", "calc-size(0, 0)"), "invalid");
  EXPECT_EQ(P("width", "calc-size(30px)"), "invalid");
  EXPECT_EQ(P("width", "calc-size(any)"), "invalid");
  EXPECT_EQ(P("width", "calc-size(size)"), "invalid");
  EXPECT_EQ(P("width", "calc-size(calc-size(any, 30px))"), "invalid");
}

/* Values 4, 10.13: a Sum is written "(" first ", " ... ")" with its terms sorted (a percentage before the dimensions), a Product inside it in parentheses, and "If a
   result of this serialization starts with a "(" ... and ends with a ")" remove those characters": calc-size()'s calculation is written as the inside of a calc(). */
TEST(CssCalcSize, TheCalculationIsWrittenAsTheInsideOfACalc) {
  EXPECT_EQ(P("height", "calc-size(min-content, size * 2)"), "valid:calc-size(min-content, 2 * size)");
  EXPECT_EQ(P("max-height", "calc-size(fit-content, 30px + size / 2)"), "valid:calc-size(fit-content, 30px + (0.5 * size))");
  EXPECT_EQ(P("width", "calc-size(fit-content, 50% + size / 2)"), "valid:calc-size(fit-content, 50% + (0.5 * size))");
  EXPECT_EQ(P("width", "calc-size(any, 50px + 30%)"), "valid:calc-size(any, 30% + 50px)");
  EXPECT_EQ(P("width", "calc-size(calc-size(2in, 30px), 25em)"), "valid:calc-size(calc-size(192px, 30px), 25em)");
  EXPECT_EQ(P("width", "calc-size(auto, calc(10px + 5px))"), "valid:calc-size(auto, 15px)");
  EXPECT_EQ(P("width", "calc-size(auto, min(size, 100px))"), "valid:calc-size(auto, min(size, 100px))");
}

/* Sizing 4, 7.1: "Before used value time, calc-size() functions are not resolved or simplified" -- the lengths in them are computed as the lengths of a calc() are, and
   `size` stays. */
TEST(CssCalcSize, TheLengthsInItAreComputedAndSizeStays) {
  EXPECT_EQ(root_computed("width: calc-size(auto, size + 2em)", "width"), "calc-size(auto, 32px + size)");
  EXPECT_EQ(root_computed("width: calc-size(1in, 2em)", "width"), "calc-size(96px, 32px)");
  EXPECT_EQ(root_computed("width: calc-size(auto, 25%)", "width"), "calc-size(auto, 25%)");
}

/* Transitions 1, 2.3: "Note that order is important within the items in this property: the first value that can be parsed as a time is assigned to the transition-duration, and
   the second value that can be parsed as a time is assigned to transition-delay", and transition-duration is "<time [0,∞]>#". Animations 1 says the same of animation. */
TEST(CssTransitionTimes, TheFirstTimeIsTheDurationAndIsNotNegative) {
  EXPECT_EQ(P("transition", "-1s -2s"), "invalid");
  EXPECT_EQ(P("transition", "-2s 1s"), "invalid");
  EXPECT_EQ(P("transition", "width -1s -2s"), "invalid");
  EXPECT_EQ(P("transition", "width ease -1s 2s"), "invalid");
  EXPECT_EQ(P("transition", "width 1s, color -1s -1s"), "invalid");
  EXPECT_EQ(P("transition", "calc(-1s) 2s"), "invalid");
  EXPECT_EQ(P("animation", "-1s -2s"), "invalid");
  EXPECT_EQ(P("animation", "-2s 1s"), "invalid");
  EXPECT_EQ(P("-webkit-transition", "-1s -2s"), "invalid");
  // one time is the duration and the second is the delay, which may be negative
  EXPECT_EQ(P("transition", "1s -2s"), "valid:1s -2s");
  EXPECT_EQ(P("transition", "width 0s -2s"), "valid:width 0s -2s");
  EXPECT_EQ(P("transition", "-1s"), "invalid");     // a lone time is the duration
  EXPECT_EQ(P("animation", "-1s"), "invalid");
  EXPECT_EQ(P("transition", "2s 1s"), "valid:2s 1s");
  EXPECT_EQ(P("animation", "1s -2s"), "valid:1s -2s");
  // a time inside another function is not the first time of the value
  EXPECT_EQ(P("transition", "steps(2) 1s -2s"), "valid:1s steps(2) -2s");
}

/* Values 4, 10.9.2: "NaN does not escape a top-level calculation; it's censored into a zero value", and Easing 2, 2.3: "If the <step-position> is jump-none, the <integer> must be
   at least 2, or the function is invalid. Otherwise, the <integer> must be at least 1": zero is not. A finite value below the least is clamped when computed, as Values 4,
   10.12 and the computed value of calc(-10) in steps() say. */
TEST(CssEasingSteps, ACalculationOfNaNIsZeroAndZeroIsNotAStepCount) {
  EXPECT_EQ(P("animation-timing-function", "steps(calc(0/0), jump-none)"), "invalid");
  EXPECT_EQ(P("animation-timing-function", "steps(calc(0/0))"), "invalid");
  EXPECT_EQ(P("animation-timing-function", "steps(calc(NaN), jump-both)"), "invalid");
  EXPECT_EQ(P("animation-timing-function", "steps(calc(1), jump-none)"), "valid:steps(calc(1), jump-none)");
  EXPECT_EQ(P("animation-timing-function", "steps(calc(-10), start)"), "valid:steps(calc(-10), start)");
  EXPECT_EQ(P("animation-timing-function", "steps(calc(1/0), jump-none)"), "valid:steps(calc(infinity), jump-none)");
  EXPECT_EQ(P("transition-timing-function", "ease, steps(calc(0/0), start)"), "invalid");
  // a slot that holds zero holds a NaN, and the censored zero of a slot that does not is clamped when computed (flex-line-count is <integer [1,∞]>)
  EXPECT_EQ(P("flex-line-count", "calc(NaN)"), "valid:calc(NaN)");
  EXPECT_EQ(P("opacity", "calc(0/0)"), "valid:calc(NaN)");
  EXPECT_EQ(P("z-index", "calc(0/0)"), "valid:calc(NaN)");
}

/* Anchor Positioning 1, 5.1: <anchor-size()> is a "New value" of "block-size, inline-size, width, height, min-block-size, min-inline-size, min-width, min-height, max-block-size,
   max-inline-size, max-width, max-height, top, left, right, bottom, margin-top, margin-left, margin-right, margin-bottom" and of no other property: flex-basis (content | <'width'>) is
   not one, and the flex shorthand that takes it is not either. */
TEST(CssAnchorSize, FlexBasisIsNotASizingPropertyOfAnchorPositioning) {
  EXPECT_EQ(P("flex-basis", "anchor-size(--a width)"), "invalid");
  EXPECT_EQ(P("flex-basis", "anchor-size(--a width, 10px)"), "invalid");
  EXPECT_EQ(P("flex-basis", "calc(anchor-size(--a width) + 1px)"), "invalid");
  EXPECT_EQ(P("flex", "1 1 anchor-size(--a width)"), "invalid");
  EXPECT_EQ(P("width", "anchor-size(--a width)"), "valid:anchor-size(--a width)");
  EXPECT_EQ(P("min-height", "calc(anchor-size(--a height) + 1px)"), "valid:calc(1px + anchor-size(--a height))");
  // the rest of width's values are flex-basis's
  EXPECT_EQ(P("flex-basis", "content"), "valid:content");
  EXPECT_EQ(P("flex-basis", "auto"), "valid:auto");
  EXPECT_EQ(P("flex-basis", "fit-content(20px)"), "valid:fit-content(20px)");
  EXPECT_EQ(P("flex-basis", "calc-size(any, 50px)"), "valid:calc-size(any, 50px)");
}

/* Link Parameters 1, 2.1: "<param-spec> = color | accent-color | [ <dashed-ident> <css-type>? ]"; Mixins 1, 2.1: "<css-type> = <syntax-component> | <type()>", "<type()> = type( <syntax> )",
   and "To serialize a CSS type ... If the <css-type> consists of a single <syntax-component>, return the corresponding syntax string. Otherwise ... "type(" ... ")"". */
TEST(CssParam, ATypeIsAComponentOrATypeFunctionAndASingleComponentIsWrittenBare) {
  EXPECT_EQ(P("link-parameters", "param(--a <color>, orange)"), "valid:param(--a <color>, orange)");
  EXPECT_EQ(P("link-parameters", "param(--a type(<color>), orange)"), "valid:param(--a <color>, orange)");
  EXPECT_EQ(P("link-parameters", "param(--a type( <length>+ ), 1px)"), "valid:param(--a <length>+, 1px)");
  EXPECT_EQ(P("link-parameters", "param(--a <color>#, red)"), "valid:param(--a <color>#, red)");
  EXPECT_EQ(P("link-parameters", "param(--a type(<color> | <length>), orange)"), "valid:param(--a type(<color> | <length>), orange)");
  EXPECT_EQ(P("link-parameters", "param(--a type(*), orange)"), "valid:param(--a type(*), orange)");
  EXPECT_EQ(P("link-parameters", "param(--a auto, orange)"), "valid:param(--a auto, orange)");
  EXPECT_EQ(P("link-parameters", "param(--a <transform-list>, none)"), "valid:param(--a <transform-list>, none)");
  EXPECT_EQ(P("link-parameters", "param(color, blue)"), "valid:param(color, blue)");
  EXPECT_EQ(P("link-parameters", "param(accent-color, blue)"), "valid:param(accent-color, blue)");
  EXPECT_EQ(P("link-parameters", "param(--a <notatype>, orange)"), "invalid");
  EXPECT_EQ(P("link-parameters", "param(--a <color> <length>, orange)"), "invalid");
  EXPECT_EQ(P("link-parameters", "param(--a type(<color> <length>), orange)"), "invalid");
  EXPECT_EQ(P("link-parameters", "param(--a type(), orange)"), "invalid");
  EXPECT_EQ(P("link-parameters", "param(--a type(<color> |), orange)"), "invalid");
  EXPECT_EQ(P("link-parameters", "param(<color>, orange)"), "invalid");
  EXPECT_EQ(P("link-parameters", "param(--a <transform-list>+, none)"), "invalid");
  EXPECT_EQ(P("link-parameters", "param(color <color>, blue)"), "invalid");
}

TEST(CssParam, TheSameSpecIsReadInAUrlModifier) {
  EXPECT_EQ(P("background-image", "url(\"icon.svg\" param(--c <color>, green))"), "valid:url(\"icon.svg\" param(--c <color>, green))");
  EXPECT_EQ(P("background-image", "url(\"icon.svg\" param(--c type(<color>), green))"), "valid:url(\"icon.svg\" param(--c <color>, green))");
  EXPECT_EQ(P("background-image", "url(\"icon.svg\" param(--c type(<color> | <length>), green))"), "valid:url(\"icon.svg\" param(--c type(<color> | <length>), green))");
  EXPECT_EQ(P("background-image", "url(\"icon.svg\" param(--c <notatype>, green))"), "invalid");
  EXPECT_EQ(P("background-image", "url(\"icon.svg\" param(--c type(<nope>), green))"), "invalid");
  EXPECT_EQ(P("background-image", "url(\"icon.svg\" param(<color>, green))"), "invalid");
  // (Values 4, 4.5.3: an unknown modifier stays valid)
  EXPECT_EQ(P("background-image", "url(\"icon.svg\" unknown(<notatype>))"), "valid:url(\"icon.svg\" unknown(<notatype>))");
}

} // namespace
