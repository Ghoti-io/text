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
 * `color-mix()` computed (src/css/css_color_compute.c): the worked examples of Color 4, 13 and Color 5, 3 and 11.1 with the numbers the draft gives, the form of the result
 * by the mixing space, the percentages, the four hue methods, `none`, and the cost of a deep nesting. Every expected string is the draft's.
 */

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_color_test_support.h"
#include "css_compute_test_support.h"
#include "../src/css/css_color_compute.h"

using namespace cmp;

namespace {

std::string bg(const std::string & value, const std::string & parent = "") {
  return computed(parent, "background-color: " + value, "background-color");
}

/// The numbers of a color text and the text without them, compared within eps (the pages' way: color-testcommon.js).
bool near_color(const std::string & got, const std::string & want, double eps) {
  auto split = [](const std::string & s, std::vector<double> & nums, std::string & text) {
    std::string tok;
    for (char c : s) {
      if (!((c >= '0' && c <= '9') || c == '.')) {
        text.push_back(c);
      }
      if (c == ' ' || c == '(' || c == ',') {
        if (!tok.empty() && std::strtod(tok.c_str(), nullptr) == std::strtod(tok.c_str(), nullptr)) {
          char * end = nullptr;
          const double v = std::strtod(tok.c_str(), &end);
          if (end != tok.c_str()) {
            nums.push_back(v);
          }
        }
        tok.clear();
      }
      else {
        tok.push_back(c);
      }
    }
    char * end = nullptr;
    const double v = std::strtod(tok.c_str(), &end);
    if (!tok.empty() && end != tok.c_str()) {
      nums.push_back(v);
    }
  };
  std::vector<double> a, b;
  std::string ta, tb;
  split(got, a, ta);
  split(want, b, tb);
  if (ta != tb || a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); i++) {
    if (!(std::fabs(a[i] - b[i]) <= eps)) {
      return false;
    }
  }
  return true;
}

#define EXPECT_COLOR(got, want, eps) EXPECT_TRUE(near_color((got), (want), (eps))) << "got " << (got) << ", want " << (want)

/* Color 5, 11.1: "The result of this color mixture color-mix(in lch, peru 40%, palegoldenrod) is serialized as the string "lch(79.7256 40.448 84.771)" while the result of
   color-mix(in srgb, peru 40%, palegoldenrod) is serialized as the string "color(srgb 0.8816 0.7545 0.4988)"." */
TEST(CssColorMix, TheDraftsTwoSerializedExamples) {
  EXPECT_COLOR(bg("color-mix(in lch, peru 40%, palegoldenrod)"), "lch(79.7256 40.448 84.771)", 0.01);
  EXPECT_COLOR(bg("color-mix(in srgb, peru 40%, palegoldenrod)"), "color(srgb 0.8816 0.7545 0.4988)", 0.0001);
}

/* Color 5, 3.3: "the mixed lightness is 47.9855 * 0.65 + 52.1496 * 0.35 = 49.4429 ... the mixed result is lch(49.4429% 40.4830 162.5452)" for teal 65% and olive. */
TEST(CssColorMix, TealAndOliveInLch) {
  EXPECT_COLOR(bg("color-mix(in lch, teal 65%, olive)"), "lch(49.4429 40.4830 162.5452)", 0.01);
}

/* Color 5, 3.3: "color-mix(in oklab, teal, olive, blue)": three colors, each one third; the draft's mixed result is oklab(52.53% -0.0550 -0.0720). */
TEST(CssColorMix, ThreeColorsAreMixedInOrderWithEqualShares) {
  EXPECT_COLOR(bg("color-mix(in oklab, teal, olive, blue)"), "oklab(0.5253 -0.0550 -0.0720)", 0.0005);
}

/* Color 5, 3.2 "Percentage Normalization": "These syntactic forms are thus all equivalent: purple 50%, plum 50% / purple 50%, plum / purple, plum 50% / purple, plum / plum,
   purple / purple 80%, plum 80%. All produce a 50-50 mix ... However, this form is not the same, as the alpha is less than one: color-mix(in lch, purple 30%, plum 30%)
   produces lch(51.51% 52.21 325.8 / 0.6)." */
TEST(CssColorMix, PercentagesAreNormalizedAndTheLeftoverIsTheAlpha) {
  const std::string half = bg("color-mix(in lch, purple 50%, plum 50%)");
  EXPECT_COLOR(half, "lch(51.51 52.21 325.8)", 0.05);
  for (const char * form : {"color-mix(in lch, purple 50%, plum)", "color-mix(in lch, purple, plum 50%)", "color-mix(in lch, purple, plum)",
           "color-mix(in lch, plum, purple)", "color-mix(in lch, purple 80%, plum 80%)"}) {
    EXPECT_COLOR(bg(form), "lch(51.51 52.21 325.8)", 0.05) << form;
  }
  EXPECT_COLOR(bg("color-mix(in lch, purple 30%, plum 30%)"), "lch(51.51 52.21 325.8 / 0.6)", 0.05);
  // Values 5, 6.1: both percentages zero: the shares are equal and the alpha is zero
  EXPECT_COLOR(bg("color-mix(in srgb, red 0%, blue 0%)"), "color(srgb 0.5 0 0.5 / 0)", 1e-6);
  // Color 5, 11.1: "color-mix(red 50%, green, blue) ... color-mix(red 50%, green 25%, blue 25%)": what is left is shared by the colors without a percentage
  EXPECT_COLOR(bg("color-mix(in srgb, red 50%, green, blue)"), "color(srgb 0.5 0.125490 0.25)", 1e-6);
  EXPECT_COLOR(bg("color-mix(in srgb, red 50%, green 25%, blue 25%)"), "color(srgb 0.5 0.125490 0.25)", 1e-6);
  // a percentage of 100 on one color alone is that color
  EXPECT_COLOR(bg("color-mix(in srgb, red 100%, blue)"), "color(srgb 1 0 0)", 1e-6);
  // a percentage that is a calculation is resolved before it is used
  EXPECT_COLOR(bg("color-mix(in srgb, red calc(30% + 20%), blue)"), "color(srgb 0.5 0 0.5)", 1e-6);
}

/* Color 5, 11.1, the table of the form of a result by the mixing space. */
TEST(CssColorMix, TheFormOfAResultIsTheMixingSpacesOwn) {
  const struct {
    const char * space;
    const char * prefix;
  } rows[] = {
    {"srgb", "color(srgb "}, {"srgb-linear", "color(srgb-linear "}, {"display-p3", "color(display-p3 "}, {"a98-rgb", "color(a98-rgb "},
    {"prophoto-rgb", "color(prophoto-rgb "}, {"rec2020", "color(rec2020 "}, {"hsl", "color(srgb "}, {"hwb", "color(srgb "}, {"xyz", "color(xyz-d65 "},
    {"xyz-d50", "color(xyz-d50 "}, {"xyz-d65", "color(xyz-d65 "}, {"lab", "lab("}, {"lch", "lch("}, {"oklab", "oklab("}, {"oklch", "oklch("},
  };
  for (const auto & r : rows) {
    const std::string got = bg(std::string("color-mix(in ") + r.space + ", red, blue)");
    EXPECT_EQ(got.rfind(r.prefix, 0), 0u) << r.space << ": " << got;
  }
  // "if the result of mixing in the hsl or hwb color space has at least one missing color component ... the form used is the modern hsl(h s l / a) or hwb(h w b / a) syntax"
  EXPECT_EQ(bg("color-mix(in hsl, hsl(none 50% 50%), hsl(none 30% 40%))").rfind("hsl(none ", 0), 0u);
  EXPECT_EQ(bg("color-mix(in hwb, hwb(none 20% 30%), hwb(none 10% 40%))").rfind("hwb(none ", 0), 0u);
  // the default space is oklab, and an out-of-gamut channel is kept
  EXPECT_EQ(bg("color-mix(red, blue)").rfind("oklab(", 0), 0u);
  EXPECT_COLOR(bg("color-mix(in srgb, color(srgb 2 0 0), color(srgb 0 0 0))"), "color(srgb 1 0 0)", 1e-6);
}

/* Color 4, 13.4: "To interpolate, in the sRGB color space, the two sRGB colors rgb(24% 12% 98% / 0.4) and rgb(62% 26% 64% / 0.6) they would first be converted to premultiplied form ...
   The midpoint ... is rgb(46.8% 20.4% 77.6% / 0.5) when premultiplication is undone." */
TEST(CssColorMix, ColorsAreMixedPremultiplied) {
  EXPECT_COLOR(bg("color-mix(in srgb, rgb(24% 12% 98% / 0.4), rgb(62% 26% 64% / 0.6))"), "color(srgb 0.468 0.204 0.776 / 0.5)", 1e-6);
  // "in the Lab color space ... lab(58.873% 51.552 7.108 / 0.5)" for rgb(76% 62% 03% / 0.4) and color(display-p3 0.84 0.19 0.72 / 0.6)
  EXPECT_COLOR(bg("color-mix(in lab, rgb(76% 62% 3% / 0.4), color(display-p3 0.84 0.19 0.72 / 0.6))"), "lab(58.873 51.552 7.108 / 0.5)", 0.02);
  // "in the chroma-preserving LCH color space ... lch(58.873% 81.126 31.82 / 0.5)": the hue is not premultiplied
  EXPECT_COLOR(bg("color-mix(in lch, rgb(76% 62% 3% / 0.4), color(display-p3 0.84 0.19 0.72 / 0.6))"), "lch(58.873 81.126 31.82 / 0.5)", 0.01);
}

/* Color 4, 13.5: the four hue methods; the draft's red oklch(0.6 0.24 30) and yellow oklch(0.8 0.15 90) are at 60 degrees on the shorter arc and 240 on the longer; the brown
   oklch(0.5 0.1 30) and turquoise oklch(0.7 0.1 190) are at 110 increasing and 290 decreasing. */
TEST(CssColorMix, TheFourHueMethods) {
  EXPECT_COLOR(bg("color-mix(in oklch, oklch(0.6 0.24 30), oklch(0.8 0.15 90))"), "oklch(0.7 0.195 60)", 1e-6);
  EXPECT_COLOR(bg("color-mix(in oklch shorter hue, oklch(0.6 0.24 30), oklch(0.8 0.15 90))"), "oklch(0.7 0.195 60)", 1e-6);
  EXPECT_COLOR(bg("color-mix(in oklch longer hue, oklch(0.6 0.24 30), oklch(0.8 0.15 90))"), "oklch(0.7 0.195 240)", 1e-6);
  EXPECT_COLOR(bg("color-mix(in oklch increasing hue, oklch(0.5 0.1 30), oklch(0.7 0.1 190))"), "oklch(0.6 0.1 110)", 1e-6);
  EXPECT_COLOR(bg("color-mix(in oklch decreasing hue, oklch(0.5 0.1 30), oklch(0.7 0.1 190))"), "oklch(0.6 0.1 290)", 1e-6);
  // the second color's hue past the first: increasing continues, it does not flip
  EXPECT_COLOR(bg("color-mix(in oklch increasing hue, oklch(0.5 0.1 30), oklch(0.7 0.1 230))"), "oklch(0.6 0.1 130)", 1e-6);
  EXPECT_COLOR(bg("color-mix(in oklch decreasing hue, oklch(0.5 0.1 30), oklch(0.7 0.1 230))"), "oklch(0.6 0.1 310)", 1e-6);
  // a hue method on a space without a hue is not a color
  EXPECT_EQ(bg("color-mix(in srgb longer hue, red, blue)"), "transparent") << "the declaration is refused and the initial value stays";
}

/* Color 4, 13.3: "If a color with a carried forward missing component is interpolated with another color which is not missing that component, the missing component is treated as
   having the other color's component value." and "If both colors are missing a given component, the interpolated color will also be missing that component." */
TEST(CssColorMix, AMissingComponentTakesTheOtherColorsValue) {
  EXPECT_COLOR(bg("color-mix(in oklch, oklch(0.783 0.108 326.5), oklch(0.392 0.4 none))"), "oklch(0.5875 0.254 326.5)", 1e-4);
  EXPECT_COLOR(bg("color-mix(in oklch, oklch(0.392 0.4 none), oklch(0.783 0.108 326.5))"), "oklch(0.5875 0.254 326.5)", 1e-4);
  EXPECT_COLOR(bg("color-mix(in oklch, oklch(0.2 0.1 none), oklch(0.4 0.3 none))"), "oklch(0.3 0.2 none)", 1e-6);
  EXPECT_COLOR(bg("color-mix(in srgb, rgb(0 255 0 / 0.5), rgb(0 255 0 / none))"), "color(srgb 0 1 0 / 0.5)", 1e-6);
  EXPECT_COLOR(bg("color-mix(in srgb, oklch(0 none none), white)"), "color(srgb 0.5 0.5 0.5)", 1e-6);
  // "the premultiplied OkLCh values [0.3915, 0.054, 326] and [0.196, 0.2, 0]" for the missing alpha of the second color
  EXPECT_COLOR(bg("color-mix(in oklch, oklch(0.783 0.108 326.5 / 0.5), oklch(0.392 0.4 0 / none))"), "oklch(0.5875 0.254 343.25 / 0.5)", 1e-3);
}

/* Color 5, 10.1: "Otherwise (if currentColor was used in the function), the computed value is the color-mix() function with each <color> parameter resolved ... thus preserving
   inheritance into child elements." The `color` property itself takes the inherited color for currentcolor. */
TEST(CssColorMix, CurrentcolorKeepsTheMixAsWrittenAndTheColorPropertyResolvesIt) {
  const std::string kept = bg("color-mix(in srgb, currentcolor, red)", "color: blue");
  EXPECT_EQ(kept.rfind("color-mix(in srgb, currentcolor", 0), 0u) << kept;
  EXPECT_EQ(computed("color: blue", "color: color-mix(in srgb, currentcolor, red)", "color"), "color(srgb 0.5 0 0.5)");
}

/* Color 5, 3.3 mixes colors pair by pair: a nesting at the parser's default limit (32 deep) is computed with no recursion in the C stack, and the work is linear. */
TEST(CssColorMix, ADeepNestingIsComputed) {
  std::string v = "red";
  for (int i = 0; i < 30; i++) {
    v = "color-mix(in srgb, " + v + ", blue)";
  }
  const std::string got = bg(v);
  EXPECT_EQ(got.rfind("color(srgb ", 0), 0u) << got;
  // each level halves the distance to blue: red's channel is 2^-30 of the way, not zero
  EXPECT_COLOR(got, "color(srgb 0 0 1)", 1e-6);
}

/* D6: the work of a mix chain is linear in its length and a nesting to the depth the parser allows is a loop, not a recursion. */
TEST(CssColorMix, TheWorkOfANestedMixIsLinearAndTheDepthIsNotTheStacks) {
  size_t work[3] = {0, 0, 0};
  const size_t depth[3] = {25, 50, 100};
  for (int k = 0; k < 3; k++) {
    std::string v = "red";
    for (size_t i = 0; i < depth[k]; i++) {
      v = "color-mix(in oklab, " + v + ", blue)";
    }
    GTEXT_CSS_Color_Options o = gtext_css_color_options_default();
    o.max_nesting = 4096;
    gc::Color c;
    gc::read(c, v, &o);
    ASSERT_EQ(c.status, GTEXT_CSS_OK);
    ASSERT_TRUE(c.valid) << depth[k];
    css_color_ctx ctx{};
    ctx.alloc = gtext_allocator_default();
    css_cc_value out;
    bool done = false;
    ASSERT_EQ(css_color_compute(&ctx, c.color, &out, &done), GTEXT_CSS_OK);
    ASSERT_TRUE(done);
    work[k] = ctx.work;
  }
  // twice the depth is twice the work, give or take a constant
  EXPECT_LE(work[1], work[0] * 2 + 8);
  EXPECT_LE(work[2], work[1] * 2 + 8);
  EXPECT_GE(work[2], work[1] * 3 / 2);
}

} // namespace
