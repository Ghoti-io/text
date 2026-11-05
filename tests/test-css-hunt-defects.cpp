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
    const bool have = gtext_css_block_get_property_value(block, gtext_css_registry_resolve_alias(R(), id), &got, &length, nullptr) == GTEXT_CSS_OK && length > 0;
    if (have) {
      result = "valid:" + std::string(got, length);
    }
    else {
      result = P(property, value);
    }
    gtext_css_serialize_buffer_free(nullptr, got); /* an empty text is a buffer too */
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

/* The same sentence of Images 3 and 4 (the example under it writes `Linear-Gradient( to bottom, red 0%,yellow,black 100px)` as `linear-gradient(red, yellow, black 100px)`) and the same
   two browsers (Chromium 156, Firefox 158, which agree on every line here) for the gradient line and the interpolation of a linear gradient, and for the `ellipse` that a lone extent
   implies: `to bottom` (and a half turn in any unit) is the default of the grammar, `in srgb` the default space, `shorter hue` the default hue method. */
TEST(CssHuntGradients, TheDefaultsOfALinearGradientAndOfAnEllipseAreNotWritten) {
  EXPECT_EQ(B("background-image", "linear-gradient(to bottom, blue, green)"), "valid:linear-gradient(blue, green)");
  EXPECT_EQ(B("background-image", "linear-gradient(180deg, blue, green)"), "valid:linear-gradient(blue, green)");
  EXPECT_EQ(B("background-image", "linear-gradient(200grad, blue, green)"), "valid:linear-gradient(blue, green)");
  EXPECT_EQ(B("background-image", "linear-gradient(0.5turn, blue, green)"), "valid:linear-gradient(blue, green)");
  EXPECT_EQ(B("background-image", "linear-gradient(to bottom, blue 0%, green 100%)"), "valid:linear-gradient(blue 0%, green 100%)");
  EXPECT_EQ(B("background-image", "repeating-linear-gradient(to bottom, blue, green 10px)"), "valid:repeating-linear-gradient(blue, green 10px)");
  EXPECT_EQ(B("background-image", "image-set(linear-gradient(to bottom, blue, green) 1x)"), "valid:image-set(linear-gradient(blue, green) 1x)");
  EXPECT_EQ(B("background-image", "linear-gradient(in srgb to bottom, blue, green)"), "valid:linear-gradient(blue, green)");
  EXPECT_EQ(B("background-image", "linear-gradient(to bottom in oklab, blue, green)"), "valid:linear-gradient(in oklab, blue, green)");
  EXPECT_EQ(B("background-image", "linear-gradient(in oklab 180deg, blue, green)"), "valid:linear-gradient(in oklab, blue, green)");
  EXPECT_EQ(B("background-image", "linear-gradient(in oklch shorter hue, blue, green)"), "valid:linear-gradient(in oklch, blue, green)");
  EXPECT_EQ(B("background-image", "linear-gradient(in oklch longer hue to bottom, blue, green)"), "valid:linear-gradient(in oklch longer hue, blue, green)");
  EXPECT_EQ(B("background-image", "radial-gradient(in srgb circle, blue, green)"), "valid:radial-gradient(circle, blue, green)");
  EXPECT_EQ(B("background-image", "conic-gradient(from 0deg in srgb, blue, green)"), "valid:conic-gradient(blue, green)");
  EXPECT_EQ(B("background-image", "radial-gradient(ellipse farthest-corner at center, blue, green)"), "valid:radial-gradient(blue, green)");
  EXPECT_EQ(B("background-image", "radial-gradient(ellipse farthest-corner, blue, green)"), "valid:radial-gradient(blue, green)");
  EXPECT_EQ(B("background-image", "radial-gradient(ellipse closest-side, blue, green)"), "valid:radial-gradient(closest-side, blue, green)");
  EXPECT_EQ(B("background-image", "repeating-radial-gradient(ellipse farthest-corner at center, blue, green 10px)"), "valid:repeating-radial-gradient(blue, green 10px)");
  // and what is not a default is written
  EXPECT_EQ(B("background-image", "linear-gradient(to top, blue, green)"), "valid:linear-gradient(to top, blue, green)");
  EXPECT_EQ(B("background-image", "linear-gradient(0deg, blue, green)"), "valid:linear-gradient(0deg, blue, green)");
  EXPECT_EQ(B("background-image", "linear-gradient(-180deg, blue, green)"), "valid:linear-gradient(-180deg, blue, green)");
  EXPECT_EQ(B("background-image", "linear-gradient(in srgb-linear, blue, green)"), "valid:linear-gradient(in srgb-linear, blue, green)");
  EXPECT_EQ(B("background-image", "radial-gradient(circle farthest-corner, blue, green)"), "valid:radial-gradient(circle, blue, green)");
  EXPECT_EQ(B("background-image", "radial-gradient(circle closest-side, blue, green)"), "valid:radial-gradient(circle closest-side, blue, green)");
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

/* Shapes 1 (served 2026-11-01): the <shape-box> of shape-outside is margin-box when none is given, so the shortest form leaves it out; Backgrounds 3: the second radius of a
   corner "if omitted, is equal to the first"; Text 4: "UAs must also support the alternate keyword distribute which must compute to inter-character, thus having the exact
   same meaning and behavior"; Values 4: a zero length is the length 0px whatever its unit, and a shorthand leaves out what is its longhand's initial value. Both pinned
   browsers write each of these as shown. */
TEST(CssHuntEquivalents, TheValueThatIsTheDefaultOrTheSameIsNotWritten) {
  EXPECT_EQ(P("shape-outside", "circle() margin-box"), "valid:circle()");
  EXPECT_EQ(P("shape-outside", "margin-box polygon(evenodd, 0px 0px, 1px 1px, 2px 0px)"), "valid:polygon(evenodd, 0px 0px, 1px 1px, 2px 0px)");
  EXPECT_EQ(P("shape-outside", "circle() content-box"), "valid:circle() content-box");
  EXPECT_EQ(P("border-start-end-radius", "1dvh 1dvh"), "valid:1dvh");
  EXPECT_EQ(P("border-top-left-radius", "1px 1px"), "valid:1px");
  EXPECT_EQ(P("border-top-left-radius", "1px 2px"), "valid:1px 2px");
  EXPECT_EQ(P("text-justify", "distribute"), "valid:inter-character");
  EXPECT_EQ(P("text-justify", "inter-word"), "valid:inter-word");
  EXPECT_EQ(B("offset", "auto none 0pt"), "valid:auto");
}

/* Anchor Positioning 1 (served 2026-11-01), position-area: "If only a single keyword is given, it behaves as if the second keyword is span-all if the given keyword is
   unambigous about its axis; otherwise, it behaves as if the given keyword was repeated. (For example, top is equivalent to top span-all": the span-all of such a keyword is
   not written (both pinned browsers write position-try-fallbacks: self-x-start span-all as self-x-start). */
TEST(CssHuntPositionArea, ASpanAllBesideAKeywordThatNamesItsAxisIsNotWritten) {
  EXPECT_EQ(P("position-try-fallbacks", "span-all self-x-start, left span-self-y-end"), "valid:self-x-start, left span-self-y-end");
  EXPECT_EQ(P("position-area", "top span-all"), "valid:top");
  EXPECT_EQ(P("position-area", "span-all top"), "valid:top");
  EXPECT_EQ(P("position-area", "start span-all"), "valid:start span-all");
  EXPECT_EQ(P("position-area", "center center"), "valid:center");
}

/* Values 4, 10.13 (served 2026-11-01): a calculation is written so that it reads back as the same calculation. A term whose magnitude is below what a number is written with
   (six decimals) is written as the zero it is: `calc(6em - 5e-70px)` was written `calc(6em - 0px)`, which reads back as `6em + 0px` (the simplification negates 0px as
   0 - value, a positive zero), so a second pass wrote another text (found by the css-values fuzz run). */
TEST(CssHuntMath, ATermThatRoundsToZeroIsWrittenAsAPositiveZero) {
  const std::string once = P("width", "calc(6em - 5e-70px)");
  EXPECT_EQ(once, "valid:calc(6em + 0px)");
  EXPECT_EQ(P("width", once.substr(6)), once);
  EXPECT_EQ(P("width", "min(1px, 2em * 3 - 5px / 999999999999999999999999999999999999999999999999999999999999999999999)"), "valid:min(1px, 6em + 0px)");
  EXPECT_EQ(P("width", "calc(6em - 0.1px)"), "valid:calc(6em - 0.1px)");
}

/* Values 5 (served 2026-11-01), 9.4.1 "Simplification of <random-key>": "At parse time, certain transformations are performed on the <random-key> of a random function:
   If the <random-key> is auto (or omitted), it's turned into element-scoped property-index-scoped." and "If the <random-key> contains property-scoped or
   property-index-scoped, that keyword is replaced by a <random-ua-ident>: either ua-PROPERTY with PROPERTY being the property the value was parsed as, or
   ua-PROPERTY-INDEX with PROPERTY as the previous and INDEX being the 1-indexed integer index of this random function among all random functions being used in the
   declaration." Chromium writes the same (`random(element-scoped ua-width-1, 0px, 100px)`). */
TEST(CssHuntRandom, TheKeyIsSimplifiedAtParseTime) {
  EXPECT_EQ(P("width", "random(0px, 100px)"), "valid:random(element-scoped ua-width-1, 0px, 100px)");
  EXPECT_EQ(P("height", "random(auto, 0px, 100px)"), "valid:random(element-scoped ua-height-1, 0px, 100px)");
  EXPECT_EQ(P("font-size", "random(property-scoped, 0px, 100px)"), "valid:random(ua-font-size, 0px, 100px)");
  EXPECT_EQ(P("font-size", "random(--foo property-scoped, 0px, 100px)"), "valid:random(--foo ua-font-size, 0px, 100px)");
  EXPECT_EQ(P("width", "random(property-index-scoped, 0px, 100px)"), "valid:random(ua-width-1, 0px, 100px)");
  EXPECT_EQ(P("width", "RANDOM(PROPERTY-INDEX-SCOPED, 0px, 100px)"), "valid:random(ua-width-1, 0px, 100px)");
  EXPECT_EQ(P("rotate", "random(25deg, 1turn)"), "valid:random(element-scoped ua-rotate-1, 25deg, 360deg)");
  EXPECT_EQ(P("transition-delay", "random(25ms, 50s)"), "valid:random(element-scoped ua-transition-delay-1, 0.025s, 50s)");
  // the key is written in the order of the grammar, whichever order it came in
  EXPECT_EQ(P("height", "random(--foo property-scoped element-scoped, 0px, 100px)"), "valid:random(--foo element-scoped ua-height, 0px, 100px)");
  // a key that names its own cache is not touched, nor is a fixed one
  EXPECT_EQ(P("width", "random(--foo, 0px, 100px)"), "valid:random(--foo, 0px, 100px)");
  EXPECT_EQ(P("width", "random(fixed 0.5, 0px, 100px)"), "valid:random(fixed 0.5, 0px, 100px)");
  // every random() of the declaration takes the next index, in the order the functions begin
  EXPECT_EQ(P("width", "calc(random(1px, 2px) + random(property-index-scoped, 3px, 4px) + random(--k, 5px, 6px) + random(7px, 8px))"),
            "valid:calc(random(element-scoped ua-width-1, 1px, 2px) + random(ua-width-2, 3px, 4px) + random(--k, 5px, 6px) + random(element-scoped ua-width-4, 7px, 8px))");
}

/* Values 5 (served 2026-11-01), 9.2: "<random-cache-key> = <dashed-ident> || element-scoped || [ property-scoped | property-index-scoped | <random-ua-ident> ]" and
   "It is a <custom-ident> that must start with the prefix ua-, or else it is invalid." A key the user agent wrote is a key. */
TEST(CssHuntRandom, ARandomUaIdentIsAKey) {
  EXPECT_EQ(P("width", "random(element-scoped ua-width-1, 0px, 100px)"), "valid:random(element-scoped ua-width-1, 0px, 100px)");
  EXPECT_EQ(P("width", "random(ua-x element-scoped, 10px, 20%)"), "valid:random(element-scoped ua-x, 10px, 20%)");
  EXPECT_EQ(P("width", "random(--foo ua-width, 0px, 100px, 3px)"), "valid:random(--foo ua-width, 0px, 100px, 3px)");
  EXPECT_EQ(P("width", "random(element-scoped width-1, 0px, 100px)"), "invalid");
  EXPECT_EQ(P("width", "random(ua-a ua-b, 0px, 100px)"), "invalid");
  EXPECT_EQ(P("width", "random(property-scoped ua-width, 0px, 100px)"), "invalid");
  // and what it wrote reads back as the same text
  const std::string once = P("width", "random(0px, 100px)");
  EXPECT_EQ(P("width", once.substr(6)), once);
}

/* Grid 2: a line name is an identifier, and an identifier may end in an escaped space (CSS Syntax 3, "consume an escaped code point"): `x\ ` followed by white space and
   `y` is the name "x " and the name "y", and the text that is written has to keep them two (it was written `x\ y`, which reads as the one name "x y"). */
TEST(CssHuntGridTemplate, ANameThatEndsInAnEscapedSpaceKeepsItsSeparator) {
  const std::string once = B("grid-template", "none / [x\\ \n y] 1px");
  EXPECT_EQ(once, "valid:none / [x\\  y] 1px");
  EXPECT_EQ(B("grid-template", once.substr(6)), once);
  EXPECT_EQ(B("grid-template", "none / [a\\ \t\t b] 1px [c]"), "valid:none / [a\\  b] 1px [c]");
  // a single space after the backslash is part of the one name
  EXPECT_EQ(B("grid-template", "none / [x\\ y] 1px"), "valid:none / [x\\ y] 1px");
  // the white space that ends a hex escape is part of the escape, not a separator
  EXPECT_EQ(B("grid-template", "none / [\\41  b] 1px"), "valid:none / [A b] 1px");
}

/* Images 4 (served 2026-11-01), 2.4: "Each <string> inside image-set() represents a <url>", and an option without a resolution "behaves as 1x"; both pinned browsers write
   the url() and the default resolution. */
TEST(CssHuntImageSet, AStringIsAUrlAndTheDefaultResolutionIsWritten) {
  EXPECT_EQ(P("background-image", "image-set(\"a\" type(\"b\"), url(c) 2x, linear-gradient(red, blue))"),
            "valid:image-set(url(\"a\") 1x type(\"b\"), url(\"c\") 2x, linear-gradient(red, blue) 1x)");
  EXPECT_EQ(P("list-style-image", "image-set('a b')"), "valid:image-set(url(\"a b\") 1x)");
  EXPECT_EQ(P("cursor", "image-set(\"a\", url(b) 2x, url(c) type(\"x\")) 1 2, auto"), "valid:image-set(url(\"a\") 1x, url(\"b\") 2x, url(\"c\") 1x type(\"x\")) 1 2, auto");
  EXPECT_EQ(P("mask-image", "light-dark(image-set(\"a\"), none)"), "valid:light-dark(image-set(url(\"a\") 1x), none)");
  EXPECT_EQ(P("background-image", "image-set(url(a) 1.5dppx)"), "valid:image-set(url(\"a\") 1.5dppx)");
}

/* Backgrounds-style shorthands leave out the initial value; the initial `0px` of vertical-align's baseline-shift is a zero in a unit that is a fixed multiple of the pixel
   once it is read (`0pt`), and a zero in a relative unit is a value of its own: both pinned browsers write `vertical-align: 0cap` and `0vmin` as written and `0pt` as
   `baseline`. The same for the length of overflow-clip-margin. */
TEST(CssHuntZero, AZeroInARelativeUnitIsNotTheInitialValue) {
  EXPECT_EQ(B("vertical-align", "0cap"), "valid:0cap");
  EXPECT_EQ(B("vertical-align", "0vmin"), "valid:0vmin");
  EXPECT_EQ(B("vertical-align", "0pt"), "valid:baseline");
  EXPECT_EQ(B("vertical-align", "0"), "valid:baseline");
  EXPECT_EQ(B("overflow-clip-margin", "border-box 0mm"), "valid:border-box");
  EXPECT_EQ(B("overflow-clip-margin", "border-box 0em"), "valid:border-box 0em");
}

/* Anchor Positioning 1 (served 2026-11-01): "It is only allowed in the inset properties", "These are only usable in the inset properties ... or the logical inset properties that
   refer to the horizontal axis": the logical inset longhands take anchor() and anchor-size() as `top` does, and `inset-inline` with two equal values writes one. */
TEST(CssHuntInset, TheLogicalInsetLonghandsTakeAnchorFunctions) {
  EXPECT_EQ(P("inset-inline-start", "anchor-size()"), "valid:anchor-size()");
  EXPECT_EQ(P("inset-block-end", "anchor(left)"), "valid:anchor(left)");
  EXPECT_EQ(B("inset-inline", "anchor-size() anchor-size()"), "valid:anchor-size()");
  EXPECT_EQ(P("inset-inline-end", "anchor(foo)"), "invalid");
}

/* Fonts 4 (served 2026-11-01), font-style: the computed value is "the keyword specified, plus angle in degrees if specified"; the text does not say that an oblique angle of
   zero is `normal`. The pinned browsers write `oblique 0deg` as `normal` both and disagree on the other units (Chromium `normal`, Firefox as written): only degrees are
   `normal`, the rest is kept as written. */
TEST(CssHuntFontStyle, OnlyAZeroAngleInDegreesIsNormal) {
  EXPECT_EQ(P("font-style", "oblique 0deg"), "valid:normal");
  EXPECT_EQ(P("font-style", "oblique 0rad"), "valid:oblique 0rad");
  EXPECT_EQ(P("font-style", "oblique 0turn"), "valid:oblique 0turn");
  EXPECT_EQ(P("font-style", "oblique 10deg"), "valid:oblique 10deg");
}

/* Color 5 (served 2026-11-01), 11.2 "Serializing Origin Colors": for rgb() "followed by a space separated list of the non-alpha components as specified ... followed by ' / '
   and the alpha component as specified"; a missing component stays none (the color() form of Color 4, 16.2.2, is that of a color that is not an origin). */
TEST(CssHuntColor, AnOriginWithAMissingComponentKeepsNone) {
  EXPECT_EQ(P("color", "alpha(from rgb(0 none 0) / alpha)"), "valid:alpha(from rgb(0 none 0) / alpha)");
  EXPECT_EQ(P("color", "alpha(from rgb(none none none) / alpha)"), "valid:alpha(from rgb(none none none) / alpha)");
  EXPECT_EQ(P("color", "rgb(none 2 3)"), "valid:color(srgb none 0.007843 0.011765)");
}

/* Values 5 (served 2026-11-01): "the shorthand property must serialize to that original (arbitrary substitution function-containing) value"; the text of a value that holds a
   substitution function is the text it was written as, trimmed (both pinned browsers keep the white space and the quotes inside it). */
TEST(CssHuntPending, AValueWithASubstitutionFunctionKeepsItsText) {
  EXPECT_EQ(P("width", "calc(var(--x)   +   1px)"), "pending:calc(var(--x)   +   1px)");
  EXPECT_EQ(P("content", "  attr(x)  'b'  "), "pending:attr(x)  'b'");
  EXPECT_EQ(P("width", "var(--x)"), "pending:var(--x)");
}

/* Values 4 (served 2026-11-01), 10.9: "If, in the context in which the math function containing this calculation is placed, <percentage>s are resolved relative to another type
   of value (such as in width, where <percentage> is resolved against a <length>), and that other type is not <number>, the type is determined as the other type, but with a
   percent hint set to that other type." A percentage of a color channel is resolved against a number, so `0% - 0px` is a type error there, in sign() as well. */
TEST(CssHuntColor, ASumOfAPercentageAndALengthIsNotAChannelEvenInSign) {
  EXPECT_EQ(P("color", "rgb(sign(0% - 0px), 0, 0)"), "invalid");
  EXPECT_EQ(P("color", "rgb(calc(0% - 0px), 0, 0)"), "invalid");
  EXPECT_EQ(P("color", "rgb(sign(1% - 1%), 0, 0)"), "valid:rgb(sign(0%) 0 0)");
  EXPECT_EQ(P("width", "calc(sign(1% - 1px) * 1px)"), "valid:calc(1px * sign(1% - 1px))");
}

/* Images 4 (served 2026-11-01), 3.5.3: "A color stop with two positions is equivalent to specifying two color stops with the same color"; both pinned browsers write the two
   stops, whatever the gradient. */
TEST(CssHuntGradients, AColorStopWithTwoPositionsIsWrittenAsTwoStops) {
  EXPECT_EQ(P("background-image", "conic-gradient(red 10% 20%, blue)"), "valid:conic-gradient(red 10%, red 20%, blue)");
  EXPECT_EQ(P("background-image", "linear-gradient(to right, red 10% 20%, blue)"), "valid:linear-gradient(to right, red 10%, red 20%, blue)");
  EXPECT_EQ(P("background-image", "radial-gradient(at 10px 20px, red 1px 2px, blue)"), "valid:radial-gradient(at 10px 20px, red 1px, red 2px, blue)");
  // a configuration with two numbers is not a stop
  EXPECT_EQ(P("background-image", "radial-gradient(ellipse 10px 20px, red, blue)"), "valid:radial-gradient(10px 20px, red, blue)");
  EXPECT_EQ(P("background-image", "radial-gradient(at 10px 20px, red, blue)"), "valid:radial-gradient(at 10px 20px, red, blue)");
}

/* Values 4 (served 2026-11-01), 10.13, serialization of a Sum: its children are written one after the other; an infinite term is `infinity * 1px`, and both pinned browsers
   write `calc(-infinity * 1% + infinity * 1px)` with no parentheses around the terms. */
TEST(CssHuntMath, AnInfiniteTermOfASumIsWrittenWithoutParentheses) {
  EXPECT_EQ(P("width", "calc(infinity * 1px - infinity * 1%)"), "valid:calc(-infinity * 1% + infinity * 1px)");
  EXPECT_EQ(P("width", "calc(1px + infinity * 1%)"), "valid:calc(infinity * 1% + 1px)");
  EXPECT_EQ(P("width", "calc(2 * (infinity * 1px))"), "valid:calc(infinity * 1px)");
}

/* The font shorthand writes the line height after the size with a space on each side (web-platform-tests' font-valid.html: `size + ' / ' + lineHeight`; both pinned browsers
   write `x-large / 3ic`), and leaves out a line height of `normal`. */
TEST(CssHuntFont, TheLineHeightFollowsTheSizeAfterASpacedSlash) {
  EXPECT_EQ(B("font", "12px/1.5 serif"), "valid:12px / 1.5 serif");
  EXPECT_EQ(B("font", "italic bold x-large/3ic serif"), "valid:italic bold x-large / 3ic serif");
  EXPECT_EQ(B("font", "12px/normal serif"), "valid:12px serif");
}

/* Fonts 4: `font-weight` is `<number [1,1000]>`, a math function is one, and the `font` shorthand takes it where it takes a number (web-platform-tests' css-fonts/variations/font-shorthand.html:
   `calc(900.7 - 200.1 * 2) calc(12px + 12px) Arial` is weight 500.5; both pinned browsers write it so). The weight was dropped from the shorthand's longhands (the computed weight was 400) before, and a specified value keeps its math function; an `oblique` angle may be
   a math function as well. */
TEST(CssHuntFont, AMathFunctionIsTheWeightOfTheShorthandAndMayBeTheObliqueAngle) {
  EXPECT_EQ(B("font", "calc(500.5) 12px serif"), "valid:calc(500.5) 12px serif");
  EXPECT_EQ(B("font", "calc(900.7 - 200.1 * 2) calc(12px + 12px) serif"), "valid:calc(500.5) calc(24px) serif");
  EXPECT_EQ(B("font", "oblique calc(200 + 300) 12px serif"), "valid:oblique calc(500) 12px serif");
  EXPECT_EQ(B("font", "oblique 30deg calc(200 + 300) 12px serif"), "valid:oblique 30deg calc(500) 12px serif");
  EXPECT_EQ(B("font", "calc(200 + 300) oblique calc(10deg) 12px serif"), "valid:oblique calc(10deg) calc(500) 12px serif");
}

} // namespace
