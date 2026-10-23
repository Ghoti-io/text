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
 * CSS colors as typed values (CSS Color 4 and 5, Editor's Drafts served 2026-10-22): every form parses to the declared
 * space and channels, `none` is kept as missing, and the declared value is written as sections 16 (Color 4) and 11 (Color 5)
 * write it. The expectations are the drafts' own examples and sentences; the pinned web-platform-tests pages are scored
 * separately (make conformance-css-values).
 */

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_color_test_support.h"
#include "css_values_test_support.h"
#include "markup_test_support.h"

using namespace gc;
using gv::Grammar;
using gv::Matched;
using gv::Defs;
using gv::compile_into;
using gv::match_into;

namespace {

struct Row {
  const char * input;
  const char * expect;
};

void check(const std::vector<Row> & rows) {
  for (const Row & r : rows) {
    EXPECT_EQ(color(r.input), r.expect) << r.input;
  }
}

}  // namespace

// ---- the matrix of this phase -------------------------------------------------------------------------------

TEST(CssColorMatrix, AnHslColorWithAlphaIsWrittenAsLegacySrgbWithAlphaKept) {
  // "hsl(120 100% 50% / .5)": legacy-space sRGB serialization, alpha kept
  EXPECT_EQ(color("hsl(120 100% 50% / .5)"), "rgba(0, 255, 0, 0.5)");
}

TEST(CssColorMatrix, AMissingChannelIsKept) {
  Color c;
  read(c, "oklch(none 0.1 30)");
  ASSERT_TRUE(c.valid);
  EXPECT_EQ(c.color->function, GTEXT_CSS_COLOR_FN_OKLCH);
  EXPECT_EQ(c.color->space, GTEXT_CSS_SPACE_OKLCH);
  EXPECT_TRUE(c.color->has_missing);
  ASSERT_EQ(c.color->component_count, 3u);
  EXPECT_EQ(c.color->components[0].kind, GTEXT_CSS_COMP_NONE);
  EXPECT_EQ(c.color->components[1].kind, GTEXT_CSS_COMP_NUMBER);
  EXPECT_DOUBLE_EQ(c.color->components[1].value, 0.1);
  EXPECT_DOUBLE_EQ(c.color->components[2].value, 30.0);
  EXPECT_EQ(serialize(c.color), "oklch(none 0.1 30)");
}

TEST(CssColorMatrix, ARelativeColorWithASubstitutionIsPendingWithItsChannelExpressionsKept) {
  // a var() anywhere makes the whole value pending substitution (Variables 1); the channel expressions stay in its text
  Defs d;
  Grammar g;
  compile_into(g, "<color>");
  ASSERT_TRUE(g.ok());
  Matched m;
  match_into(m, g, "rgb(from var(--c) r g calc(b / 2))");
  ASSERT_TRUE(m.matched);
  EXPECT_EQ(m.root()->kind, GTEXT_CSS_VAL_PENDING);
  EXPECT_EQ(css_test::str(m.root()->text), "rgb(from var(--c) r g calc(b / 2))");
}

// ---- keywords ----------------------------------------------------------------------------------------------------

TEST(CssColorKeywords, AllHundredAndFortyEightNamesParseAndKeepTheirKeyword) {
  ASSERT_EQ(gtext_css_color_named_count(), 148u);
  std::string previous;
  for (size_t i = 0; i < gtext_css_color_named_count(); i++) {
    const char * name = nullptr;
    uint32_t rgb = 0;
    ASSERT_TRUE(gtext_css_color_named_at(i, &name, &rgb));
    EXPECT_LT(previous, std::string(name)) << "sorted for a bisection";
    previous = name;
    EXPECT_EQ(color(name), name);
    std::string upper = name;
    for (char & ch : upper) {
      ch = (char)std::toupper((unsigned char)ch);
    }
    EXPECT_EQ(color(upper), name) << "ASCII case-insensitive; the lower case keyword is kept";
    Color c;
    read(c, name);
    ASSERT_TRUE(c.valid) << name;
    EXPECT_EQ(c.color->kind, GTEXT_CSS_COLOR_KEYWORD);
    EXPECT_EQ(c.color->keyword, GTEXT_CSS_COLOR_KW_NAMED);
    double rgba[4];
    ASSERT_TRUE(gtext_css_color_srgb(c.color, rgba));
    EXPECT_EQ(rgba[0], (double)((rgb >> 16) & 0xFF));
    EXPECT_EQ(rgba[1], (double)((rgb >> 8) & 0xFF));
    EXPECT_EQ(rgba[2], (double)(rgb & 0xFF));
    EXPECT_EQ(rgba[3], 1.0);
  }
  EXPECT_FALSE(gtext_css_color_named_at(148, nullptr, nullptr));
}

TEST(CssColorKeywords, TheValuesOfTheDraftsKnownNames) {
  struct N {
    const char * name;
    uint32_t rgb;
  };
  for (const N & n : std::vector<N>{{"red", 0xFF0000}, {"rebeccapurple", 0x663399}, {"aliceblue", 0xF0F8FF}, {"goldenrod", 0xDAA520}, {"yellowgreen", 0x9ACD32},
           {"lightgoldenrodyellow", 0xFAFAD2}, {"grey", 0x808080}, {"gray", 0x808080}, {"darkslategrey", 0x2F4F4F}, {"whitesmoke", 0xF5F5F5}}) {
    Color c;
    read(c, n.name);
    ASSERT_TRUE(c.valid) << n.name;
    double rgba[4];
    ASSERT_TRUE(gtext_css_color_srgb(c.color, rgba));
    EXPECT_EQ((uint32_t)rgba[0] << 16 | (uint32_t)rgba[1] << 8 | (uint32_t)rgba[2], n.rgb) << n.name;
  }
}

TEST(CssColorKeywords, TransparentCurrentcolorAndTheSystemColors) {
  // 16.2: "the serialized declared value of transparent is the string "transparent""; 16.6: "The serialized form of currentColor is the string "currentcolor""
  check({{"transparent", "transparent"}, {"TRANSPARENT", "transparent"}, {"currentColor", "currentcolor"}, {"CURRENTCOLOR", "currentcolor"}});
  for (const char * n : {"AccentColor", "AccentColorText", "ActiveText", "ButtonBorder", "ButtonFace", "ButtonText", "Canvas", "CanvasText", "Field", "FieldText",
           "GrayText", "Highlight", "HighlightText", "LinkText", "Mark", "MarkText", "SelectedItem", "SelectedItemText", "VisitedText"}) {
    Color c;
    read(c, n);
    ASSERT_TRUE(c.valid) << n;
    EXPECT_EQ(c.color->keyword, GTEXT_CSS_COLOR_KW_SYSTEM) << n;
    std::string lower = n;
    for (char & ch : lower) {
      ch = (char)std::tolower((unsigned char)ch);
    }
    EXPECT_EQ(serialize(c.color), lower) << "the ASCII lowercase keyword is retained";
  }
  for (const char * n : {"ActiveBorder", "ActiveCaption", "AppWorkspace", "Background", "ButtonHighlight", "ButtonShadow", "CaptionText", "InactiveBorder",
           "InactiveCaption", "InactiveCaptionText", "InfoBackground", "InfoText", "Menu", "MenuText", "Scrollbar", "ThreeDDarkShadow", "ThreeDFace",
           "ThreeDHighlight", "ThreeDLightShadow", "ThreeDShadow", "Window", "WindowFrame", "WindowText"}) {
    Color c;
    read(c, n);
    ASSERT_TRUE(c.valid) << n;
    EXPECT_EQ(c.color->keyword, GTEXT_CSS_COLOR_KW_DEPRECATED) << n;
  }
  check({{"notacolor", "invalid"}, {"none", "invalid"}, {"from", "invalid"}, {"inherit", "invalid"}, {"Canvasx", "invalid"}, {"", "invalid"}});
}

// ---- hex colors ------------------------------------------------------------------------------------------------------

TEST(CssColorHex, ThreeFourSixAndEightDigits) {
  check({{"#f00", "rgb(255, 0, 0)"}, {"#F00", "rgb(255, 0, 0)"}, {"#ff0000", "rgb(255, 0, 0)"}, {"#123", "rgb(17, 34, 51)"}, {"#abcdef", "rgb(171, 205, 239)"},
      {"#f008", "rgba(255, 0, 0, 0.533)"}, {"#ff000080", "rgba(255, 0, 0, 0.5)"}, {"#ff0000ff", "rgb(255, 0, 0)"}, {"#00000000", "rgba(0, 0, 0, 0)"},
      {"#12", "invalid"}, {"#12345", "invalid"}, {"#1234567", "invalid"}, {"#123456789", "invalid"}, {"#ggg", "invalid"}, {"#", "invalid"}});
}

TEST(CssColorHex, AnEightBitAlphaIsWrittenByTheDraftsAlgorithm) {
  // 16.1.1: "If there exists an integer between 0 and 100 inclusive that, when multiplied with 2.55 and rounded to the closest integer
  // (rounding up if two values are equally close), equals alpha, let rounded be that integer divided by 100. Otherwise, let rounded be alpha
  // divided by 0.255 and rounded to the closest integer (rounding up if two values are equally close), divided by 1000."
  // 237: 93 * 2.55 rounds to 237, so "0.93". 236: no integer, 236 / 0.255 = 925.49..., rounded 925, so "0.925" (the draft's example text says
  // "0.92549", which is not what its algorithm gives; the algorithm is followed). 127: 127 / 0.255 = 498.04 -> "0.498".
  check({{"#ff0000ed", "rgba(255, 0, 0, 0.93)"}, {"#ff0000ec", "rgba(255, 0, 0, 0.925)"}, {"#ff000000", "rgba(255, 0, 0, 0)"}, {"#ff00007f", "rgba(255, 0, 0, 0.498)"}});
  // the draft's other example: a magenta with alpha 0xed
  EXPECT_EQ(color("#ff00ffed"), "rgba(255, 0, 255, 0.93)");
}

// ---- rgb() -----------------------------------------------------------------------------------------------------------------

TEST(CssColorRgb, LegacyAndModernSyntaxesBothWriteTheLegacySerialization) {
  check({{"rgb(10 20 30)", "rgb(10, 20, 30)"}, {"rgb(10, 20, 30)", "rgb(10, 20, 30)"}, {"rgba(10, 20, 30, .5)", "rgba(10, 20, 30, 0.5)"},
      {"rgb(10 20 30 / 50%)", "rgba(10, 20, 30, 0.5)"}, {"RGB(10 20 30)", "rgb(10, 20, 30)"}, {"rgba(10 20 30)", "rgb(10, 20, 30)"},
      {"rgb(29 164 192 / 95%)", "rgba(29, 164, 192, 0.95)"}, {"rgb(10% 20% 30%)", "rgb(25.5, 51, 76.5)"}, {"rgb(0, 0, 0, 1)", "rgb(0, 0, 0)"},
      {"rgb(0 0 0 / 1)", "rgb(0, 0, 0)"}, {"rgb(0 0 0 / 0)", "rgba(0, 0, 0, 0)"}});
}

TEST(CssColorRgb, ValuesOutsideTheRangesAreClampedAtParsedValueTime) {
  // 4.1: "Values outside these ranges are not invalid, but are clamped to the ranges defined here at parsed-value time."
  check({{"rgb(-2 3 4)", "rgb(0, 3, 4)"}, {"rgb(257 30 40)", "rgb(255, 30, 40)"}, {"rgb(250% 20% 40%)", "rgb(255, 51, 102)"}, {"rgb(500, 0, 0)", "rgb(255, 0, 0)"},
      {"rgb(0 0 0 / -1)", "rgba(0, 0, 0, 0)"}, {"rgb(0 0 0 / 2)", "rgb(0, 0, 0)"}, {"rgb(0 0 0 / 200%)", "rgb(0, 0, 0)"}});
}

TEST(CssColorRgb, TheModernSyntaxMixesNumbersAndPercentagesTheLegacyOneDoesNot) {
  check({{"rgb(250% 51 40%)", "rgb(255, 51, 102)"}, {"rgb(255 20% 102)", "rgb(255, 51, 102)"}, {"rgb(10%, 20, 30)", "invalid"}, {"rgb(10, 20%, 30)", "invalid"},
      {"rgb(10, 20, 30%)", "invalid"}, {"rgb(10, 20, 30, none)", "invalid"}, {"rgb(none, 20, 30)", "invalid"}, {"rgb(1, 2 3)", "invalid"}, {"rgb(1 2, 3)", "invalid"},
      {"rgb(1, 2, 3, 4, 5)", "invalid"}, {"rgb(1 2 3 4)", "invalid"}, {"rgb(1 2)", "invalid"}, {"rgb(1 2 3 / )", "invalid"}, {"rgb(1px 2 3)", "invalid"}});
}

TEST(CssColorRgb, ARgbColorWithAMissingChannelIsWrittenAsColorSrgb) {
  // 16.2.2: "rgb(none 0 0) is serialized as color(srgb none 0 0) because rgb() (in its serialized legacy comma form) cannot represent none."
  check({{"rgb(none 0 0)", "color(srgb none 0 0)"}, {"rgb(none none none)", "color(srgb none none none)"}, {"rgb(none none none / none)", "color(srgb none none none / none)"},
      {"rgb(20% none none)", "color(srgb 0.2 none none)"}, {"rgb(0 0 0 / none)", "color(srgb 0 0 0 / none)"}, {"rgba(none 0 0 / .5)", "color(srgb none 0 0 / 0.5)"}});
}

TEST(CssColorRgb, ACalcThatIsANumberIsResolvedAndClamped) {
  // 16.2.1 / Color 4 "when calc() is simplified down to a single value, the color values are clamped to [0.0, 255.0]": rgb(calc(100 * 4) 127 calc(20 - 35)) is rgb(255 127 0)
  check({{"rgb(calc(100 * 4) 127 calc(20 - 35))", "rgb(255, 127, 0)"}, {"rgb(calc(infinity) 0 0)", "rgb(255, 0, 0)"}, {"rgb(calc(-infinity) 0 0)", "rgb(0, 0, 0)"},
      {"rgb(calc(NaN) 0 0)", "rgb(0, 0, 0)"}, {"rgb(0 0 0 / calc(infinity))", "rgb(0, 0, 0)"}, {"rgb(0 0 0 / calc(NaN))", "rgba(0, 0, 0, 0)"},
      {"rgb(calc(50%) 0 0)", "rgb(127.5, 0, 0)"}});
}

TEST(CssColorRgb, AChannelThatNeedsTheEnvironmentIsWrittenAsTheFunction) {
  check({{"rgb(calc(50% + (sign(1em - 10px) * 10%)), 0%, 0%, 50%)", "rgb(calc(50% + (10% * sign(1em - 10px))) 0 0 / 0.5)"},
      {"rgb(calc(50 + (sign(1em - 10px) * 10)) 0 0 / 0.5)", "rgb(calc(50 + (10 * sign(1em - 10px))) 0 0 / 0.5)"}});
  Color c;
  read(c, "rgb(calc(1em / 1px) 0 0)");
  ASSERT_TRUE(c.valid);
  EXPECT_TRUE(c.color->has_unresolved);
  double rgba[4];
  EXPECT_FALSE(gtext_css_color_srgb(c.color, rgba));
}

// ---- hsl() and hwb() ------------------------------------------------------------------------------------------------------

TEST(CssColorHsl, ConversionToSrgbIsRoundedToEightBits) {
  check({{"hsl(0 100% 50%)", "rgb(255, 0, 0)"}, {"hsl(120 100% 50%)", "rgb(0, 255, 0)"}, {"hsl(240 100% 50%)", "rgb(0, 0, 255)"}, {"hsl(0 0% 100%)", "rgb(255, 255, 255)"},
      {"hsl(0 0% 0%)", "rgb(0, 0, 0)"}, {"hsl(120 30% 50%)", "rgb(89, 166, 89)"}, {"hsl(120deg 20% 40%)", "rgb(82, 122, 82)"}, {"hsla(120, 100%, 50%, .5)", "rgba(0, 255, 0, 0.5)"},
      {"hsl(120, 100%, 50%)", "rgb(0, 255, 0)"}, {"hsl(0.5turn 100% 50%)", "rgb(0, 255, 255)"}, {"hsl(3.14159rad 100% 50%)", "rgb(0, 255, 255)"},
      {"hsl(200grad 100% 50%)", "rgb(0, 255, 255)"}, {"hsl(0 0% 50%)", "rgb(128, 128, 128)"}, {"hsl(30 100% 50%)", "rgb(255, 128, 0)"}});
}

TEST(CssColorHsl, HuesAreNormalizedAndSaturationBelowZeroIsZero) {
  check({{"hsl(480 100% 50%)", "rgb(0, 255, 0)"}, {"hsl(-120 100% 50%)", "rgb(0, 0, 255)"}, {"hsl(720 100% 50%)", "rgb(255, 0, 0)"}, {"hsl(0 -50% 40%)", "rgb(102, 102, 102)"},
      {"hsl(30 -50 60)", "rgb(153, 153, 153)"}, {"hsl(calc(infinity) 100% 50%)", "rgb(255, 0, 0)"}, {"hsl(calc(NaN) 100% 50%)", "rgb(255, 0, 0)"},
      {"hsl(120 30 50)", "rgb(89, 166, 89)"}, {"hsl(120 30% 50)", "rgb(89, 166, 89)"}});
}

TEST(CssColorHsl, TheLegacySyntaxNeedsPercentagesAndTheModernOneKeepsNoneInHsl) {
  check({{"hsl(120, 30, 50)", "invalid"}, {"hsl(120, 30%, 50)", "invalid"}, {"hsl(120, none, 50%)", "invalid"}, {"hsl(none 100% 50%)", "hsl(none 100% 50%)"},
      {"hsl(120 none 50)", "hsl(120 none 50%)"}, {"hsl(120 30 none / 0.5)", "hsl(120 30% none / 0.5)"}, {"hsl(none none none / none)", "hsl(none none none / none)"},
      {"hsla(none none none)", "hsl(none none none)"}, {"hsl(120 30% 50% / none)", "hsl(120 30% 50% / none)"}, {"hsl(480 none 50%)", "hsl(120 none 50%)"}});
}

TEST(CssColorHwb, ConversionFollowsTheDraft) {
  // hwb(150 20% 10%) is the same color as rgb(20% 90% 55%) (Color 4, 7): 51 229.5 140.25
  check({{"hwb(150 20% 10%)", "rgb(51, 230, 140)"}, {"hwb(0 0% 0%)", "rgb(255, 0, 0)"}, {"hwb(120 0% 0%)", "rgb(0, 255, 0)"}, {"hwb(120 30% 50%)", "rgb(77, 128, 77)"},
      {"hwb(120 30% 50% / 0.5)", "rgba(77, 128, 77, 0.5)"},
      // "hwb(45 40% 80%) ... achromatic color whose R, G and B components are 40 / 40 + 80 = 0.33"
      {"hwb(45 40% 80%)", "rgb(85, 85, 85)"}, {"hwb(0 100% 50% / 0)", "rgba(170, 170, 170, 0)"},
      // 127.5 is rounded up
      {"hwb(320deg 30% 40%)", "rgb(153, 77, 128)"}, {"hwb(none none none)", "hwb(none none none)"}, {"hwb(120 30 none)", "hwb(120 30% none)"}});
}

// ---- lab, lch, oklab, oklch ----------------------------------------------------------------------------------------------------

TEST(CssColorLab, PercentagesAreNumbersByTheReferenceRanges) {
  // 9.3 / 16.3: lab(56.200% 0.000 66.88%) is lab(56.2 0 83.6): the a and b of lab() are 125 at 100%
  check({{"lab(56.200% 0.000 66.88%)", "lab(56.2 0 83.6)"}, {"lab(50% 50% -20%)", "lab(50 62.5 -25)"}, {"lab(29% 39 20)", "lab(29 39 20)"},
      {"lch(37% 105.0 305.00)", "lch(37 105 305)"}, {"lch(50% 50% 20)", "lch(50 75 20)"}, {"lch(56.2% 83.6 357.4 /93%)", "lch(56.2 83.6 357.4 / 0.93)"},
      {"oklab(54.0% -0.10 -0.02)", "oklab(0.54 -0.1 -0.02)"}, {"oklab(54% -25% -5%)", "oklab(0.54 -0.1 -0.02)"}, {"oklch(56.43% 0.0900 123.40)", "oklch(0.5643 0.09 123.4)"},
      {"oklch(53.85% 0.1725 320.67 / 70%)", "oklch(0.5385 0.1725 320.67 / 0.7)"}});
}

TEST(CssColorLab, LightnessChromaAndAlphaAreClampedHuesAreNormalized) {
  check({{"lab(400 0 10/50%)", "lab(100 0 10 / 0.5)"}, {"lab(-40 0 0)", "lab(0 0 0)"}, {"lab(0 0 0 / -10%)", "lab(0 0 0 / 0)"}, {"lab(0 0 0 / 110%)", "lab(0 0 0)"},
      {"lab(50 -200 200)", "lab(50 -200 200)"}, {"lch(20 -20 0)", "lch(20 0 0)"}, {"lch(10 20 380deg)", "lch(10 20 20)"}, {"lch(10 20 -340deg)", "lch(10 20 20)"},
      {"lch(10 20 740)", "lch(10 20 20)"}, {"oklab(4 0 0.1/50%)", "oklab(1 0 0.1 / 0.5)"}, {"oklab(-0.4 0 0)", "oklab(0 0 0)"}, {"oklch(-1 -1 0)", "oklch(0 0 0)"},
      {"lab(0 0 0 / 1)", "lab(0 0 0)"}});
}

TEST(CssColorLab, MathFunctionsAreKeptInTheirSimplifiedForm) {
  // the specified value keeps calc(); "per CSS Values 4 section 10.12 Range Checking, <alpha-value>s specified using calc() are not clamped when the specified form is serialized"
  check({{"lab(calc(50 * 3) calc(0.5 - 1) calc(1.5) / calc(-0.5 + 1))", "lab(calc(150) calc(-0.5) calc(1.5) / calc(0.5))"},
      {"lab(calc(50%) 50% 0.5)", "lab(calc(50%) 62.5 0.5)"}, {"lab(none 20 calc(0.5))", "lab(none 20 calc(0.5))"}, {"lab(calc(infinity) 0 0)", "lab(calc(infinity) 0 0)"},
      {"lab(calc(0 / 0) 0 0)", "lab(calc(NaN) 0 0)"}, {"lch(calc(50 * 3) calc(0.5 - 1) calc(20deg * 2) / calc(-0.5 + 1))", "lch(calc(150) calc(-0.5) calc(40deg) / calc(0.5))"},
      {"lab(0 0 0 / calc(2 * 60%))", "lab(0 0 0 / calc(120%))"}});
}

TEST(CssColorLab, TheArgumentsOfEachFunctionAreTheDraftsAndNoOthers) {
  check({{"lab(1 2)", "invalid"}, {"lab(1 2 3 4)", "invalid"}, {"lab(1, 2, 3)", "invalid"}, {"lab(1 2 3deg)", "lab(1 2 3deg)" == std::string() ? "" : "invalid"},
      {"lch(1 2 3 / 4 5)", "invalid"}, {"oklch(1 2 red)", "invalid"}, {"lab(1px 2 3)", "invalid"}});
}

// ---- color() ------------------------------------------------------------------------------------------------------------------------

TEST(CssColorFunction, EveryPredefinedSpaceKeepsItsNameAndXyzIsXyzD65) {
  for (const char * sp : {"srgb", "srgb-linear", "display-p3", "display-p3-linear", "a98-rgb", "prophoto-rgb", "rec2020", "xyz-d50", "xyz-d65"}) {
    EXPECT_EQ(color(std::string("color(") + sp + " 0% 0.5 100%)"), std::string("color(") + sp + " 0 0.5 1)") << sp;
    EXPECT_EQ(color(std::string("COLOR(") + sp + " 0.1 0.2 0.3 / 50%)"), std::string("color(") + sp + " 0.1 0.2 0.3 / 0.5)") << sp;
  }
  EXPECT_EQ(color("color(xyz 0.1 0.2 0.3)"), "color(xyz-d65 0.1 0.2 0.3)");
  EXPECT_EQ(color("color(dIsPlAy-P3  0.964  0.763  0.787)"), "color(display-p3 0.964 0.763 0.787)");
  EXPECT_EQ(color("color(rec2020 0.400 0.660 0.340)"), "color(rec2020 0.4 0.66 0.34)");
  EXPECT_EQ(color("color(prophoto-rgb 0.2804 0.40283 0.42259/85%)"), "color(prophoto-rgb 0.2804 0.40283 0.42259 / 0.85)");
}

TEST(CssColorFunction, ValuesAreNotClampedAndAlphaIs) {
  check({{"color(srgb 400% 0 10/50%)", "color(srgb 4 0 10 / 0.5)"}, {"color(srgb -200 -200 -200 / -200)", "color(srgb -200 -200 -200 / 0)"},
      {"color(srgb 200 200 200 / 200)", "color(srgb 200 200 200)"}, {"color(srgb none none none / 0.5)", "color(srgb none none none / 0.5)"},
      {"color(srgb 0 0 0 / none)", "color(srgb 0 0 0 / none)"}, {"color(srgb calc(50%) 50% 0.5)", "color(srgb calc(50%) 0.5 0.5)"}});
}

TEST(CssColorFunction, ACustomColorSpaceTakesAnyNumberOfComponents) {
  // Color 5, 11.4: color(--swop5c 0% 70.0% 20.00% .0%) is "color(--swop5c 0 0.7 0.2 0)"
  check({{"color(--swop5c  0% 70.0% 20.00% .0%)", "color(--swop5c 0 0.7 0.2 0)"}, {"color(--x 1)", "color(--x 1)"}, {"color(--x)", "invalid"},
      {"color(--x 1 2 3 4 5 6 7 / .5)", "color(--x 1 2 3 4 5 6 7 / 0.5)"}, {"color(srgb 1 2)", "invalid"}, {"color(srgb 1 2 3 4)", "invalid"}, {"color(foo 1 2 3)", "invalid"},
      {"color(lab 1 2 3)", "invalid"}, {"color(srgb)", "invalid"}, {"color()", "invalid"}});
  Color c;
  read(c, "color(--swop5c 0 0.7 0.2 0)");
  ASSERT_TRUE(c.valid);
  EXPECT_EQ(c.color->space, GTEXT_CSS_SPACE_CUSTOM);
  EXPECT_EQ(css_test::str(c.color->name), "--swop5c");
  EXPECT_EQ(c.color->component_count, 4u);
}

// ---- color-mix(), relative colors, and the rest of Color 5 ---------------------------------------------------------

TEST(CssColorMix, TheDraftsExamplesOfTheSerializationOfTheDeclaredValue) {
  // Color 5, 11.1
  check({{"color-mix(in oklab, teal 50%, peru 50%)", "color-mix(teal, peru)"}, {"color-mix(in oklab, teal 70%, peru 70%)", "color-mix(teal 70%, peru 70%)"},
      {"color-mix(in oklch longer hue, red, green, blue)", "color-mix(in oklch longer hue, red, green, blue)"},
      {"color-mix(red 50%, green, blue)", "color-mix(red 50%, green 25%, blue 25%)"}, {"color-mix(in oklab, teal, peru 40%)", "color-mix(teal 60%, peru 40%)"},
      {"color-mix(in lch, peru 40%, palegoldenrod)", "color-mix(in lch, peru 40%, palegoldenrod 60%)"}});
}

TEST(CssColorMix, SpacesHueMethodsAndCalcPercentages) {
  check({{"color-mix(in srgb, red, blue)", "color-mix(in srgb, red, blue)"}, {"color-mix(red, blue)", "color-mix(red, blue)"},
      {"color-mix(in srgb, 70% red, 50% blue)", "color-mix(in srgb, red 70%, blue 50%)"}, {"color-mix(in hsl, red 50%, blue)", "color-mix(in hsl, red, blue)"},
      {"color-mix(in hsl shorter hue, red, blue)", "color-mix(in hsl, red, blue)"}, {"color-mix(in lch decreasing hue, red, hsl(120, 100%, 50%))", "color-mix(in lch decreasing hue, red, rgb(0, 255, 0))"},
      {"color-mix(in hsl, red calc(20%), blue)", "color-mix(in hsl, red calc(20%), blue)"}, {"color-mix(in hsl, currentcolor, blue)", "color-mix(in hsl, currentcolor, blue)"},
      {"color-mix(in xyz, red, blue)", "color-mix(in xyz-d65, red, blue)"}, {"color-mix(in --custom, red, blue)", "color-mix(in --custom, red, blue)"},
      {"color-mix(in srgb, red 0%, blue 100%)", "color-mix(in srgb, red 0%, blue 100%)"},
      // invalid: negative and over-100 percentages, a hue method on a rectangular space, nothing to mix, a missing comma
      {"color-mix(in srgb, red -10%, blue)", "invalid"}, {"color-mix(in srgb, red 110%, blue)", "invalid"}, {"color-mix(in srgb longer hue, red, blue)", "invalid"},
      {"color-mix(in srgb)", "invalid"}, {"color-mix(in srgb, )", "invalid"}, {"color-mix(in srgb red, blue)", "invalid"}, {"color-mix(in banana, red, blue)", "invalid"},
      {"color-mix(in srgb, red 10% 20%, blue)", "invalid"}, {"color-mix(in srgb, 10% 20%, blue)", "invalid"}, {"color-mix(in hsl hue, red, blue)", "invalid"}});
}

TEST(CssColorRelative, TheDraftsExamplesOfTheSerializationOfTheDeclaredValue) {
  // Color 5, 11.3
  check({{"OkLcH(from peru  l    c  h)", "oklch(from peru l c h)"}, {"rgb(from red calc(r / 2) g calc(30%))", "rgb(from red calc(0.5 * r) g calc(30%))"},
      {"hsl(from hsl(0deg 10% 50%) h s l)", "hsl(from rgb(140, 115, 115) h s l)"}, {"hsl(from hsl(none 10% 50%) h s l)", "hsl(from hsl(none 10% 50%) h s l)"},
      {"hsl(from rebeccapurple none none none / none)", "hsl(from rebeccapurple none none none / none)"}});
}

TEST(CssColorRelative, ChannelKeywordsAreTheFunctionsOwn) {
  check({{"rgb(from rebeccapurple r g b)", "rgb(from rebeccapurple r g b)"}, {"rgb(from rebeccapurple b alpha r / g)", "rgb(from rebeccapurple b alpha r / g)"},
      {"rgb(from rebeccapurple r calc(g * 2) 10)", "rgb(from rebeccapurple r calc(2 * g) 10)"}, {"rgb(from rebeccapurple r 20% 10)", "rgb(from rebeccapurple r 20% 10)"},
      {"hsl(from rebeccapurple h s l)", "hsl(from rebeccapurple h s l)"}, {"hwb(from rebeccapurple h w b)", "hwb(from rebeccapurple h w b)"},
      {"lab(from rebeccapurple l a b)", "lab(from rebeccapurple l a b)"}, {"lch(from rebeccapurple l c h)", "lch(from rebeccapurple l c h)"},
      {"oklab(from rebeccapurple l a b)", "oklab(from rebeccapurple l a b)"}, {"oklch(from rebeccapurple l c h)", "oklch(from rebeccapurple l c h)"},
      {"color(from rebeccapurple srgb r g b)", "color(from rebeccapurple srgb r g b)"}, {"color(from rebeccapurple xyz x y z)", "color(from rebeccapurple xyz-d65 x y z)"},
      {"rgb(from rebeccapurple h s l)", "invalid"}, {"hsl(from rebeccapurple r g b)", "invalid"}, {"color(from rebeccapurple xyz r g b)", "invalid"},
      {"rgb(from rebeccapurple r g)", "invalid"}, {"rgb(from r g b)", "invalid"}, {"rgb(from rebeccapurple, r, g, b)", "invalid"},
      {"rgb(from rebeccapurple r g b / alpha)", "rgb(from rebeccapurple r g b / alpha)"}, {"rgb(from rebeccapurple r g b / 0.5)", "rgb(from rebeccapurple r g b / 0.5)"},
      {"rgb(from rebeccapurple 0 0 0 / 1)", "rgb(from rebeccapurple 0 0 0 / 1)"}, {"rgb(from rebeccapurple r g b / 500)", "rgb(from rebeccapurple r g b / 500)"},
      {"hsl(from rebeccapurple 25deg s l)", "hsl(from rebeccapurple 25deg s l)"}, {"rgb(from rgb(from red r g b) r g b)", "rgb(from rgb(from red r g b) r g b)"}});
}

TEST(CssColorRelative, ARelativeColorKeepsItsOriginAndItsChannelKeywordsAsSymbols) {
  Color c;
  read(c, "rgb(from rebeccapurple r calc(g * 2) 10 / alpha)");
  ASSERT_TRUE(c.valid);
  ASSERT_NE(c.color->origin, nullptr);
  EXPECT_EQ(c.color->origin->keyword, GTEXT_CSS_COLOR_KW_NAMED);
  EXPECT_EQ(c.color->components[0].kind, GTEXT_CSS_COMP_SYMBOL);
  EXPECT_EQ(css_test::str(c.color->components[0].text), "r");
  EXPECT_EQ(c.color->components[1].kind, GTEXT_CSS_COMP_MATH);
  EXPECT_TRUE(c.color->components[1].math->has_symbols);
  EXPECT_EQ(c.color->components[2].kind, GTEXT_CSS_COMP_NUMBER);
  EXPECT_EQ(c.color->alpha.kind, GTEXT_CSS_COMP_SYMBOL);
  EXPECT_TRUE(c.color->has_unresolved);
}

TEST(CssColorOthers, LightDarkContrastColorAlphaAndDeviceCmyk) {
  check({{"light-dark(red, #00f)", "light-dark(red, rgb(0, 0, 255))"}, {"light-dark(red)", "invalid"}, {"light-dark(red, blue, green)", "invalid"},
      {"light-dark(light-dark(red, blue), green)", "light-dark(light-dark(red, blue), green)"}, {"contrast-color(red)", "contrast-color(red)"}, {"contrast-color(red, blue)", "invalid"},
      {"contrast-color(color(srgb calc(0.5) calc(1 + 1 / 1) 1 / .5))", "contrast-color(color(srgb calc(0.5) calc(2) 1 / 0.5))"},
      {"alpha(from green / 0.5)", "alpha(from green / 0.5)"}, {"alpha(from green / alpha)", "alpha(from green / alpha)"}, {"alpha(from green / none)", "alpha(from green / none)"},
      {"alpha(from green / sibling-index())", "alpha(from green / sibling-index())"}, {"alpha(from green)", "invalid"}, {"alpha(green / 0.5)", "invalid"},
      // Color 5, 11.5: device-cmyk(0 81% 81% 30%) is device-cmyk(0 0.81 0.81 0.3)
      {"device-cmyk(0 81% 81% 30%)", "device-cmyk(0 0.81 0.81 0.3)"}, {"device-cmyk(0, 0.5, 0.5, 0.2)", "device-cmyk(0 0.5 0.5 0.2)"}, {"device-cmyk(0 0 0 0 / .5)", "device-cmyk(0 0 0 0 / 0.5)"},
      {"device-cmyk(0 0 0)", "invalid"}, {"device-cmyk(0, 0, 0, 0, 0)", "invalid"}, {"device-cmyk(0 none 0 0)", "device-cmyk(0 none 0 0)"}});
}

TEST(CssColorOthers, ColorLayersIsNotInTheServedLevelFive) {
  EXPECT_EQ(color("color-layers(red, blue)"), "invalid");
}

// ---- sRGB access and the shape of the value -----------------------------------------------------------------------------

TEST(CssColorValue, SrgbIsAvailableOnlyForAColorThatIsOne) {
  double rgba[4];
  Color a;
  read(a, "hsl(120 100% 50% / .5)");
  ASSERT_TRUE(gtext_css_color_srgb(a.color, rgba));
  EXPECT_EQ(rgba[0], 0.0);
  EXPECT_EQ(rgba[1], 255.0);
  EXPECT_EQ(rgba[3], 0.5);
  Color b;
  read(b, "lab(50 0 0)");
  EXPECT_FALSE(gtext_css_color_srgb(b.color, rgba));
  Color c;
  read(c, "currentcolor");
  EXPECT_FALSE(gtext_css_color_srgb(c.color, rgba));
  Color d;
  read(d, "rgb(none 0 0)");
  EXPECT_FALSE(gtext_css_color_srgb(d.color, rgba));
  Color e;
  read(e, "transparent");
  ASSERT_TRUE(gtext_css_color_srgb(e.color, rgba));
  EXPECT_EQ(rgba[3], 0.0);
  EXPECT_FALSE(gtext_css_color_srgb(nullptr, rgba));
}

TEST(CssColorValue, TheNamesOfTheSpaces) {
  EXPECT_STREQ(gtext_css_color_space_name(GTEXT_CSS_SPACE_XYZ_D65), "xyz-d65");
  EXPECT_STREQ(gtext_css_color_space_name(GTEXT_CSS_SPACE_SRGB), "srgb");
  EXPECT_STREQ(gtext_css_color_space_name(GTEXT_CSS_SPACE_DISPLAY_P3_LINEAR), "display-p3-linear");
  EXPECT_EQ(gtext_css_color_space_name(GTEXT_CSS_SPACE_CUSTOM), nullptr);
}

TEST(CssColorValue, ColorsAreOneComponentValue) {
  check({{"red blue", "invalid"}, {"red, blue", "invalid"}, {"rgb(1 2 3) red", "invalid"}});
}

// ---- limits ---------------------------------------------------------------------------------------------------------------------------------

namespace {
std::string nest_light_dark(int n) {
  std::string s;
  for (int i = 0; i < n; i++) {
    s += "light-dark(";
  }
  s += "red";
  for (int i = 0; i < n; i++) {
    s += ", blue)";
  }
  return s;
}
}  // namespace

TEST(CssColorLimits, NestingPastTheLimitIsItsOwnStatus) {
  GTEXT_CSS_Color_Options o = gtext_css_color_options_default();
  o.max_nesting = 6;
  Color at, over;
  read(at, nest_light_dark(5), &o);  // the top one and five inside it, and red: six frames
  EXPECT_EQ(at.status, GTEXT_CSS_OK);
  EXPECT_TRUE(at.valid);
  read(over, nest_light_dark(6), &o);  // seven frames
  EXPECT_EQ(over.status, GTEXT_CSS_E_LIMIT_COLOR_NESTING);
  EXPECT_FALSE(over.valid);
  EXPECT_EQ(over.result.status, GTEXT_CSS_E_LIMIT_COLOR_NESTING);
  EXPECT_EQ(over.result.where.line, 1u);
  EXPECT_GT(over.result.where.offset, 0u);
}

TEST(CssColorLimits, NestingIsNotARecursion) {
  GTEXT_CSS_Color_Options o = gtext_css_color_options_default();
  o.max_nesting = 4096;
  const int n = 2000;
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  po.max_depth = 100000;
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_component_values, nest_light_dark(n), &po);
  ASSERT_EQ(p.status, GTEXT_CSS_OK);
  GTEXT_CSS_Color * c = nullptr;
  bool valid = false;
  ASSERT_EQ(gtext_css_color_parse(&p.tree->values[0], &o, &valid, &c, nullptr), GTEXT_CSS_OK);
  ASSERT_TRUE(valid);
  char * buf = nullptr;
  size_t len = 0;
  ASSERT_EQ(gtext_css_color_serialize_buffer(c, nullptr, &buf, &len), GTEXT_CSS_OK);
  EXPECT_EQ(len, nest_light_dark(n).size());  // (the serialization is the input, with its spaces as the draft writes them)
  gtext_css_serialize_buffer_free(nullptr, buf);
  gtext_css_color_free(c);
}

TEST(CssColorLimits, TheOutputLimitIsHonored) {
  Color c;
  read(c, "color-mix(in srgb, red, blue)");
  ASSERT_TRUE(c.valid);
  GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
  char * buf = nullptr;
  size_t len = 0;
  so.max_output_bytes = 10;
  EXPECT_EQ(gtext_css_color_serialize_buffer(c.color, &so, &buf, &len), GTEXT_CSS_E_LIMIT_OUTPUT);
  EXPECT_EQ(buf, nullptr);
  so.max_output_bytes = 29;
  EXPECT_EQ(gtext_css_color_serialize_buffer(c.color, &so, &buf, &len), GTEXT_CSS_OK);
  EXPECT_EQ(std::string(buf, len), "color-mix(in srgb, red, blue)");
  gtext_css_serialize_buffer_free(&so, buf);
}

TEST(CssColorLimits, AnAllocationThatFailsIsOutOfMemoryAndLeaksNothing) {
  for (const char * text : {"color-mix(in hsl, rgb(from red r calc(g * 2) b), light-dark(hsl(120 30% 50%), #fff) 30%)", "lab(calc(1 + 2) 3 4 / 50%)", "red"}) {
    css_test::Parsed p;
    css_test::parse_into(p, gtext_css_parse_component_values, text);
    ASSERT_EQ(p.status, GTEXT_CSS_OK);
    std::string expect = color(text);
    bool done = false;
    for (size_t n = 1; n < 600 && !done; n++) {
      FaultAllocator fa;
      fa.arm(n);
      GTEXT_CSS_Color_Options o = gtext_css_color_options_default();
      o.allocator = &fa.alloc;
      GTEXT_CSS_Color * c = nullptr;
      bool valid = false;
      const GTEXT_CSS_Status st = gtext_css_color_parse(&p.tree->values[0], &o, &valid, &c, nullptr);
      if (st == GTEXT_CSS_OK) {
        ASSERT_TRUE(valid);
        GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
        so.allocator = &fa.alloc;
        for (size_t k = 1; k < 600; k++) {
          fa.arm(k);
          char * buf = nullptr;
          size_t len = 0;
          const GTEXT_CSS_Status ss = gtext_css_color_serialize_buffer(c, &so, &buf, &len);
          if (ss == GTEXT_CSS_OK) {
            EXPECT_EQ(std::string(buf, len), expect);
            fa.disarm();
            gtext_css_serialize_buffer_free(&so, buf);
            break;
          }
          EXPECT_EQ(ss, GTEXT_CSS_E_OOM);
          EXPECT_EQ(buf, nullptr);
        }
        fa.disarm();
        gtext_css_color_free(c);
        EXPECT_TRUE(fa.live.empty());
        done = true;
        break;
      }
      EXPECT_EQ(st, GTEXT_CSS_E_OOM) << text << " " << n;
      EXPECT_EQ(c, nullptr);
      fa.disarm();
      EXPECT_TRUE(fa.live.empty()) << "leak at failure " << n << " of " << text;
    }
    EXPECT_TRUE(done) << text;
  }
}
