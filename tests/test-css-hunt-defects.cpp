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
 * Defects the property hunt (css_prop_diff.py, scope full) found in this library, each by a test that quotes the sentence of the served drafts that decides it.
 */

#include <string>

#include <gtest/gtest.h>

#include "css_compute_test_support.h"

using namespace cmp;

namespace {

std::string P(const std::string & property, const std::string & value) {
  return reg::parse(R(), property, value);
}

/* A value as a browser reads it: set on a declaration block and asked for (`style.setProperty`, `getPropertyValue`), which is the shortest form of the longhands
   it made for a shorthand, and the grammar's own text for any other property. */
std::string B(const std::string & property, const std::string & value) {
  GTEXT_CSS_Declaration_Block * block = nullptr;
  if (gtext_css_block_new(R(), nullptr, &block) != GTEXT_CSS_OK) {
    return "error";
  }
  GTEXT_CSS_Property_Outcome outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
  std::string result = "invalid";
  if (gtext_css_block_add_text(block, property.data(), property.size(), value.data(), value.size(), false, nullptr, &outcome, nullptr) == GTEXT_CSS_OK &&
      outcome == GTEXT_CSS_PROP_RESULT_VALID) {
    GTEXT_CSS_Property_Id id = GTEXT_CSS_PROPERTY_NONE;
    GTEXT_CSS_Property_Info info;
    (void)info;
    id = gtext_css_registry_find_property(R(), property.data(), property.size());
    char * got = nullptr;
    size_t length = 0;
    if (gtext_css_block_get_property_value(block, gtext_css_registry_resolve_alias(R(), id), &got, &length, nullptr) == GTEXT_CSS_OK && length > 0) {
      result = "valid:" + std::string(got, length);
      gtext_css_serialize_buffer_free(nullptr, got);
    }
    else {
      result = P(property, value);
    }
  }
  gtext_css_block_free(block);
  return result;
}

/* Values 4, 6.2 (served 2026-11-01): "Note: Units are ASCII case-insensitive and serialize as lowercase, for example 1Q serializes as 1q." (and for frequency "1Hz
   serializes as 1hz"). Both pinned browsers write 1q. */
TEST(CssHuntUnits, AUnitIsWrittenInLowerCase) {
  EXPECT_EQ(P("width", "1Q"), "valid:1q");
  EXPECT_EQ(P("width", "1q"), "valid:1q");
  EXPECT_EQ(B("border-radius", "1Q"), "valid:1q");
  EXPECT_EQ(P("transition-duration", "1S"), "valid:1s");
}

/* Values 4, 5 (served 2026-11-01): "For zero lengths the unit identifier is optional", and a <length> is written with its unit: the shorthand writes 0px as its
   longhands do. */
TEST(CssHuntZero, ABareZeroRadiusIsWrittenAsALength) {
  EXPECT_EQ(B("border-radius", "0"), "valid:0px");
  EXPECT_EQ(B("border-radius", "0.0"), "valid:0px");
  EXPECT_EQ(B("border-radius", "0 0 1px 0"), "valid:0px 0px 1px");
  EXPECT_EQ(B("border-radius", "0 / 1px"), "valid:0px / 1px");
  EXPECT_EQ(P("border-top-left-radius", "0"), "valid:0px");
  EXPECT_EQ(B("border-image-outset", "0"), "valid:0");
}

/* Values 4, 2.1 (served 2026-11-01): "Keywords ... are ASCII case-insensitive"; Painting and Text: color-interpolation takes `auto | sRGB | linearRGB`, shape-rendering
   `crispEdges`, pointer-events `visiblePainted`. The drafts do not say that a keyword is written as they spell it, and both pinned browsers write it in lower case. */
TEST(CssHuntKeywords, AKeywordIsWrittenInLowerCase) {
  EXPECT_EQ(B("color-interpolation", "linearRGB"), "valid:linearrgb");
  EXPECT_EQ(B("color-interpolation", "SRGB"), "valid:srgb");
  EXPECT_EQ(B("pointer-events", "visiblePainted"), "valid:visiblepainted");
  EXPECT_EQ(B("shape-rendering", "CRISPEDGES"), "valid:crispedges");
  EXPECT_EQ(B("text-rendering", "optimizeLegibility"), "valid:optimizelegibility");
  EXPECT_EQ(B("display", "BLOCK"), "valid:block");
  // a name the author chose keeps its case
  EXPECT_EQ(B("animation-name", "Fade"), "valid:Fade");
  EXPECT_EQ(B("font-family", "Fira Sans"), "valid:Fira Sans");
}

/* Values 4, 10.12 (served 2026-11-01): "While CSS intentionally leaves numeric precision/range UA-defined, extremely large values (including, notably, ±∞) will clamp to the
   minimum/maximum value allowed." The range of an <integer> is that of a 32-bit integer in both pinned browsers (z-index 4294967296 is 2147483647). */
TEST(CssHuntIntegers, AnIntegerPastTheRangeIsClamped) {
  EXPECT_EQ(B("z-index", "4294967296"), "valid:2147483647");
  EXPECT_EQ(B("order", "2147483648"), "valid:2147483647");
  EXPECT_EQ(B("z-index", "2147483647"), "valid:2147483647");
  EXPECT_EQ(B("z-index", "-2147483648"), "valid:-2147483648");
  EXPECT_EQ(B("z-index", "-4294967296"), "valid:-2147483648");
  EXPECT_EQ(B("grid-column", "span 4294967296"), "valid:span 2147483647");
  EXPECT_EQ(B("grid-row", "4294967296 / 2"), "valid:2147483647 / 2");
  EXPECT_EQ(P("grid-template-columns", "repeat(4294967296, 1px)"), "valid:repeat(2147483647, 1px)");
  EXPECT_EQ(B("column-count", "4294967296"), "valid:2147483647");
  // a number that is not an integer keeps its value
  EXPECT_EQ(B("opacity", "4294967296"), "valid:4294967296");
}

/* Filter Effects 1, drop-shadow(): "Values are interpreted as for box-shadow but with the optional 3rd <length> value being the standard deviation instead of blur radius", and
   Backgrounds 3, box-shadow: "3rd <length [0,∞]>" with "Negative values are invalid." (box-shadow refuses it as well). Both pinned browsers refuse a negative deviation. */
TEST(CssHuntDropShadow, ANegativeStandardDeviationIsInvalid) {
  EXPECT_EQ(P("filter", "drop-shadow(red 1px 2px -3px)"), "invalid");
  EXPECT_EQ(P("filter", "drop-shadow(1px 2px -3dvh)"), "invalid");
  EXPECT_EQ(P("backdrop-filter", "drop-shadow(color-mix(in srgb, red, blue) -1dvh 0lvi -100lvi)"), "invalid");
  EXPECT_EQ(P("filter", "drop-shadow(red 1px 2px 3px)"), "valid:drop-shadow(red 1px 2px 3px)");
  EXPECT_EQ(P("filter", "drop-shadow(-1px -2px)"), "valid:drop-shadow(-1px -2px)");
  EXPECT_EQ(P("filter", "drop-shadow(1px 2px 0)"), "valid:drop-shadow(1px 2px 0px)");
  EXPECT_EQ(P("filter", "drop-shadow(1px 2px 3px 4px)"), "invalid");
}

/* Motion 1, offset: "Omitted values are set to their initial values." The text gives no form for a value whose components before the `/` are all initial; both pinned browsers
   write the first of them, offset-position, which is `normal` (a path of `none` after a non-initial rotation is written, as the page expects: `none reverse`). */
TEST(CssHuntOffset, AllInitialComponentsAreWrittenAsThePosition) {
  EXPECT_EQ(B("offset", "normal none"), "valid:normal");
  EXPECT_EQ(B("offset", "none"), "valid:normal");
  EXPECT_EQ(B("offset", "normal / 0%"), "valid:normal / 0% center");
  EXPECT_EQ(B("offset", "normal none reverse"), "valid:none reverse");
  EXPECT_EQ(B("offset", "auto"), "valid:auto");
}

/* Grid 2, 7.3 (served 2026-11-01): "A sequence of ident code points, representing a named cell token", "A sequence of one or more "." (U+002E FULL STOP), representing a null
   cell token", "A sequence of any other characters, representing a trash token" and "A trash token is a syntax error, and makes the declaration invalid". `:` is none of the
   first two; `10px` is a run of ident code points. Both pinned browsers refuse a string with a colon in it. */
TEST(CssHuntGridAreas, ATrashTokenInARowIsInvalid) {
  EXPECT_EQ(P("grid-template-areas", "\"a: 10px\""), "invalid");
  EXPECT_EQ(P("grid-template-areas", "\"a:b\""), "invalid");
  EXPECT_EQ(P("grid-template-areas", "\"a, b\""), "invalid");
  EXPECT_EQ(P("grid-template", "[a] \"a: 10px\""), "invalid");
  EXPECT_EQ(P("grid-template-areas", "\"a 10px\""), "valid:\"a 10px\"");
  EXPECT_EQ(P("grid-template-areas", "\"a . ..\" \"b-c _d \xc3\xa9\""), "valid:\"a . .\" \"b-c _d \xc3\xa9\"");
}

/* Images 4, 4: "<color-stripe> = <color> && [ <length-percentage> | <flex> ]?" with the thicknesses "<length [0,∞]>" ("Negative length values are invalid."), "<percentage [0,100]>"
   and a bare "<flex>"; Grid 2 gives the range [0,∞] to the flex of a track ("<flex [0,∞]>: A non-negative dimension with the unit fr") and defines "a flexible length or
   <flex>" itself as "a dimension with the fr unit", with no range. So a negative <flex> is a valid thickness of a stripe (stripes() has no engine to ask), and is not a valid track. */
TEST(CssHuntStripes, ANegativeFlexIsAThicknessAndALengthIsNot) {
  EXPECT_EQ(P("border-color", "stripes(red -1fr)"), "valid:stripes(red -1fr)");
  EXPECT_EQ(P("border-color", "stripes(red -1px)"), "invalid");
  EXPECT_EQ(P("border-color", "stripes(red 101%)"), "invalid");
  EXPECT_EQ(P("grid-template-columns", "-1fr"), "invalid");
}

/* Text Decoration 4, text-shadow: "Values are interpreted as for box-shadow" and "Negative spread values are invalid." (the blur is "<length [0,∞]>" in Backgrounds 3, with
   "Negative values are invalid."). The spread itself is the fourth length. */
TEST(CssHuntTextShadow, ANegativeSpreadIsInvalid) {
  EXPECT_EQ(P("text-shadow", "1px 2px 3px -4px"), "invalid");
  EXPECT_EQ(P("text-shadow", "1px 2px -3px"), "invalid");
  EXPECT_EQ(P("text-shadow", "red 1px 2px 3px 4px inset"), "valid:red 1px 2px 3px 4px inset");
  EXPECT_EQ(P("text-shadow", "-1px -2px"), "valid:-1px -2px");
  EXPECT_EQ(P("box-shadow", "1px 2px 3px -4px"), "valid:1px 2px 3px -4px");
}

/* Images 4, 8 (served 2026-11-01): "To serialize any function defined in this module, serialize it per its individual grammar, in the order its grammar is written in, omitting
   components when possible without changing the meaning". Both pinned browsers leave out of a gradient what is its default: `from 0deg`, `at center` (and `at 50% 50%`, `at 50%`),
   a lone `farthest-corner`, the `ellipse` of two sizes and the `circle` of one, and complete a one-value position (`at 70px` is `at 70px center`). */
TEST(CssHuntGradients, TheDefaultsAGradientLeavesOutAreNotWritten) {
  EXPECT_EQ(B("background-image", "conic-gradient(from 0deg, red, blue)"), "valid:conic-gradient(red, blue)");
  EXPECT_EQ(B("background-image", "conic-gradient(at center, red, blue)"), "valid:conic-gradient(red, blue)");
  EXPECT_EQ(B("background-image", "conic-gradient(from 0deg at 50% 50%, red, gold)"), "valid:conic-gradient(red, gold)");
  EXPECT_EQ(B("background-image", "conic-gradient(at 50%, red, blue)"), "valid:conic-gradient(red, blue)");
  EXPECT_EQ(B("background-image", "conic-gradient(at 70px, red, blue)"), "valid:conic-gradient(at 70px center, red, blue)");
  EXPECT_EQ(B("background-image", "conic-gradient(from 2rad at 50% 50%, red, blue)"), "valid:conic-gradient(from 2rad, red, blue)");
  EXPECT_EQ(B("background-image", "conic-gradient(from 1rad at 10% 20%, red, blue)"), "valid:conic-gradient(from 1rad at 10% 20%, red, blue)");
  EXPECT_EQ(B("background-image", "repeating-conic-gradient(from 0deg at center, red, gold)"), "valid:repeating-conic-gradient(red, gold)");
  EXPECT_EQ(B("mask-image", "radial-gradient(at center, red, blue)"), "valid:radial-gradient(red, blue)");
  EXPECT_EQ(B("mask-image", "radial-gradient(at 10%, red, blue)"), "valid:radial-gradient(at 10% center, red, blue)");
  EXPECT_EQ(B("mask-image", "radial-gradient(farthest-corner, red, blue)"), "valid:radial-gradient(red, blue)");
  EXPECT_EQ(B("mask-image", "radial-gradient(closest-side, red, blue)"), "valid:radial-gradient(closest-side, red, blue)");
  EXPECT_EQ(B("list-style-image", "radial-gradient(ellipse 10px 20px, red, blue)"), "valid:radial-gradient(10px 20px, red, blue)");
  EXPECT_EQ(B("list-style-image", "radial-gradient(circle 10px at 5px 6px, red, blue)"), "valid:radial-gradient(10px at 5px 6px, red, blue)");
  EXPECT_EQ(B("list-style-image", "radial-gradient(circle, red, blue)"), "valid:radial-gradient(circle, red, blue)");
  EXPECT_EQ(B("background-image", "linear-gradient(to right, red, blue)"), "valid:linear-gradient(to right, red, blue)");
  EXPECT_EQ(B("shape-outside", "radial-gradient(ellipse 10px 20px at 5px 6px, red, blue)"), "valid:radial-gradient(10px 20px at 5px 6px, red, blue)");
  EXPECT_EQ(B("background-image", "radial-gradient(red, blue), url(a.png)"), "valid:radial-gradient(red, blue), url(\"a.png\")");
}

/* Easing 2, 2.1.1 (served 2026-11-01): "control points originally specified with two input progress values are turned into two separate control points, and the input
   progress values are in strictly ascending order", with the examples "linear(0 20%, 0.5 10%, 1) serializes as linear(0 20%, 0.5 20%, 1)" and "linear(0, 0.25 25% 75%, 1)
   serializes as linear(0, 0.25 25%, 0.25 75%, 1)"; a control point that "originally lacked an input progress value" is written without one. */
TEST(CssHuntEasing, ALinearControlPointWithTwoPositionsIsTwoPointsInAscendingOrder) {
  EXPECT_EQ(P("animation-timing-function", "linear(0, 0.25, 1)"), "valid:linear(0, 0.25, 1)");
  EXPECT_EQ(P("animation-timing-function", "linear(0 20%, 0.5 10%, 1)"), "valid:linear(0 20%, 0.5 20%, 1)");
  EXPECT_EQ(P("animation-timing-function", "linear(0, 0.25 25% 75%, 1)"), "valid:linear(0, 0.25 25%, 0.25 75%, 1)");
  EXPECT_EQ(P("transition-timing-function", "ease, linear(1 50% 40%, 0 10%)"), "valid:ease, linear(1 50%, 1 50%, 0 50%)");
  EXPECT_EQ(P("transition-timing-function", "linear(0 0.5% 2%, 65535 -0.5%, -0.5 0%)"), "valid:linear(0 0.5%, 0 2%, 65535 2%, -0.5 2%)");
  /* a position that is not a plain percentage is left as it was written */
  EXPECT_EQ(P("animation-timing-function", "linear(0 calc(10% + 5%) 40%, 1)"), "valid:linear(0 calc(15%) 40%, 1)");
}

/* Values 5, 9.2 (served 2026-11-01): "<random-key> = auto | <random-cache-key> | fixed <number [0,1]>", and Values 4 lets a math function stand where a <number> is allowed;
   Chromium reads `fixed calc(0.5)`. A calculation that is not a number in [0,1], or is not first after `fixed`, is no key. */
TEST(CssHuntRandom, AFixedKeyTakesACalculationThatIsANumber) {
  EXPECT_EQ(P("width", "random(fixed 0.5, 0%, 100%)"), "valid:random(fixed 0.5, 0%, 100%)");
  EXPECT_EQ(P("width", "random(fixed calc(0.5), 0%, 100%)"), "valid:random(fixed calc(0.5), 0%, 100%)");
  EXPECT_EQ(P("width", "random(fixed calc(2 / 4), 0%, 100%)"), "valid:random(fixed calc(0.5), 0%, 100%)");
  EXPECT_EQ(P("width", "random(fixed calc(2), 0%, 100%)"), "invalid");
  EXPECT_EQ(P("width", "random(fixed calc(1px), 0%, 100%)"), "invalid");
  EXPECT_EQ(P("width", "random(fixed calc(50%), 0%, 100%)"), "invalid");
  EXPECT_EQ(P("width", "random(calc(0.5) fixed, 0%, 100%)"), "invalid");
}

/* Images 4, 8 (served 2026-11-01): a gradient is serialized "omitting components when possible without changing the meaning", wherever the image stands: inside an
   image-set() option, as an argument of light-dark(), as an item of `content` or of `shape-outside`. Both pinned browsers write the defaults out of all of them. */
TEST(CssHuntGradients, TheDefaultsAreLeftOutInsideImageSetLightDarkAndContent) {
  EXPECT_EQ(B("background-image", "image-set(radial-gradient(farthest-corner, red, blue) 1x)"), "valid:image-set(radial-gradient(red, blue) 1x)");
  EXPECT_EQ(B("mask-image", "light-dark(none, conic-gradient(from 0deg at 50% 50%, red, gold))"), "valid:light-dark(none, conic-gradient(red, gold))");
  EXPECT_EQ(B("mask-image", "light-dark(radial-gradient(at center, red, blue), image-set(conic-gradient(from 0deg, red, blue) 2x))"),
      "valid:light-dark(radial-gradient(red, blue), image-set(conic-gradient(red, blue) 2x))");
  EXPECT_EQ(B("content", "radial-gradient(ellipse 50% 40% at calc(2em + 10px) 30%, red, blue)"), "valid:radial-gradient(50% 40% at calc(2em + 10px) 30%, red, blue)");
  EXPECT_EQ(B("content", "conic-gradient(from 0deg, red, blue) \"a\""), "valid:conic-gradient(red, blue) \"a\"");
  EXPECT_EQ(B("content", "\"a\" counter(x)"), "valid:\"a\" counter(x)");
  EXPECT_EQ(B("shape-outside", "image-set(radial-gradient(farthest-corner, red, blue) 1x)"), "valid:image-set(radial-gradient(red, blue) 1x)");
  /* what keeps a meaning is kept */
  EXPECT_EQ(B("background-image", "image-set(radial-gradient(closest-side, red, blue) 1x)"), "valid:image-set(radial-gradient(closest-side, red, blue) 1x)");
}

/* Transitions 1, 2.3 (served 2026-11-01): "If there is more than one <single-transition> in the shorthand, and any of the transitions has none as the
   <single-transition-property>, then the declaration is invalid." Both pinned browsers refuse it. */
TEST(CssHuntTransition, NoneBesideAnotherTransitionIsInvalid) {
  EXPECT_EQ(P("transition", "none"), "valid:none");
  EXPECT_EQ(P("transition", "none 1s"), "valid:none 1s");
  EXPECT_EQ(P("transition", "none, foo"), "invalid");
  EXPECT_EQ(P("transition", "foo, none"), "invalid");
  EXPECT_EQ(P("transition", "foo 1s, none 2s ease-out"), "invalid");
  EXPECT_EQ(P("transition", "foo 1s, bar 2s"), "valid:foo 1s, bar 2s");
}

/* Values 4, 6 (served 2026-11-01): a time in milliseconds is a time, and 0ms is the value 0s that transition-duration and transition-delay start as. The shorthand leaves out a
   component that is its longhand's initial value, so it leaves out 0ms as it leaves out 0s; both pinned browsers write `transition: foo 0ms` as `foo`. A delay that is not zero
   still needs the duration before it, in the spelling the author used. */
TEST(CssHuntTransition, AZeroTimeInAnyUnitIsTheInitialValue) {
  EXPECT_EQ(B("transition", "foo 0ms"), "valid:foo");
  EXPECT_EQ(B("transition", "foo 1s 0ms"), "valid:foo 1s");
  EXPECT_EQ(B("transition", "0ms steps(1, end)"), "valid:steps(1)");
  EXPECT_EQ(B("transition", "foo, 0.0ms"), "valid:foo, all");
  EXPECT_EQ(B("transition", "foo 0ms 1s"), "valid:foo 0ms 1s");
}

/* Syntax 3 (served 2026-11-01), "consume a declaration": "remove leading and trailing <whitespace-token>s from the value": the value of a property read from text, a
   substitution function included, does not begin or end with white space. */
TEST(CssHuntWhitespace, AValueDoesNotKeepItsLeadingAndTrailingWhiteSpace) {
  EXPECT_EQ(P("top", "\t --func( a )\n"), "pending:--func( a )");
  EXPECT_EQ(P("width", "  var(--x)  "), "pending:var(--x)");
  EXPECT_EQ(P("width", " 1px "), "valid:1px");
}

} // namespace
