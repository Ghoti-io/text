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

} // namespace
