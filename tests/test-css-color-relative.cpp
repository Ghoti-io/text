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
 * Relative colors, `alpha()`, `contrast-color()` and calculations in color channels computed (src/css/css_color_compute.c): the worked examples of Color 5, 4 and 8 with the
 * numbers the draft gives, `none` and powerless components, the channel keywords of every function, the calculations that need the environment, and what stays as
 * written. The 3,300 approximate vectors of web-platform-tests are in test-css-color-vectors.cpp.
 */

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_compute_test_support.h"

using namespace cmp;

namespace {

std::string bg(const std::string & value, const std::string & parent = "", const GTEXT_CSS_Environment * env = nullptr) {
  return computed(parent, "background-color: " + value, "background-color", env);
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


/* Color 5, 4.2: "If an origin color is specified, the remaining arguments can either be specified directly, as normal, or be specified as a component keyword referring to one
   of the components of the origin color converted to the relative color processing space." The examples of 4.3 and 4.2: */
TEST(CssColorRelative, TheDraftsWorkedExamples) {
  // rgb(from indianred 255 g b): "takes the sRGB value of indianred (205 92 92) and replaces the red component with 255 to give rgb(255 92 92)"
  EXPECT_COLOR(bg("rgb(from indianred 255 g b)"), "color(srgb 1 0.360784 0.360784)", 1e-6);
  // rgb(from darkblue 16 32 b / 0.5): "rgb(16 32 139 / 0.5)"
  EXPECT_COLOR(bg("rgb(from darkblue 16 32 b / 0.5)"), "color(srgb 0.062745 0.125490 0.545098 / 0.5)", 1e-6);
  // oklab(from <54.3% -22.5% -5%> calc(1.0 - l) calc(a * 0.8) b): "oklab(0.457 -0.072 -0.02)", the percentages being numbers in the keywords
  EXPECT_COLOR(bg("oklab(from oklab(54.3% -22.5% -5%) calc(1.0 - l) calc(a * 0.8) b)"), "oklab(0.457 -0.072 -0.02)", 1e-6);
  // oklch(from <52.6% 0.115 44.6deg> l c calc(h + 90)): "oklch(0.526 0.115 134.6)", the hue being a number of degrees
  EXPECT_COLOR(bg("oklch(from oklch(52.6% 0.115 44.6deg) l c calc(h + 90))"), "oklch(0.526 0.115 134.6)", 1e-6);
  // color(from <display-p3 1 1 0> srgb r g calc(b + 0.5)): the blue of -34.63% "is not clamped. The result ... rgb(100% 100% 15.37%)"
  EXPECT_COLOR(bg("color(from color(display-p3 1 1 0) srgb r g calc(b + 0.5))"), "color(srgb 1 1 0.1537)", 1e-3);
  // "attempting to double an alpha of 0.7 in the origin color results in an alpha in the result of 1, not 1.4"
  EXPECT_COLOR(bg("oklch(from oklch(78% 0.06 75 / 0.7) l c h / calc(alpha * 2))"), "oklch(0.78 0.06 75)", 1e-6);
  // color(from color(srgb 0 0 0 / 60%) srgb alpha 0.6 0.6 / 0.9): "color(srgb 0.6 0.6 0.6 / 0.9)"; the same in rgb() has the range 0 to 255 and gives 0.6, not 153
  EXPECT_COLOR(bg("color(from color(srgb 0 0 0 / 60%) srgb alpha 0.6 0.6 / 0.9)"), "color(srgb 0.6 0.6 0.6 / 0.9)", 1e-6);
  EXPECT_COLOR(bg("rgb(from rgb(0 0 0 / 60%) alpha 153 153 / 0.9)"), "color(srgb 0.002353 0.6 0.6 / 0.9)", 1e-6);
  // the roughly-grayscale example: red becomes rgb(76.5 76.5 76.5)
  EXPECT_COLOR(bg("rgb(from red calc(r * .3 + g * .59 + b * .11) calc(r * .3 + g * .59 + b * .11) calc(r * .3 + g * .59 + b * .11))"), "color(srgb 0.3 0.3 0.3)", 1e-6);
  // 4.4 relative sRGB colors in other functions, 5.1: x y z "after conversion ... adapted to the relevant white point"; the draft's half-luminance example
  EXPECT_COLOR(bg("color(from color(display-p3 0.7 0.5 0.2) xyz-d65 calc(x/2) calc(y/2) calc(z/2))"), "color(xyz-d65 0.14 0.126 0.022)", 1e-3);
}

/* Color 5, 4.2: "If alpha is omitted, it is the origin's"? The pages say so (rgb(from rgb(20%, 40%, 60%, 80%) g b r) has an alpha of 0.8). */
TEST(CssColorRelative, AnOmittedAlphaIsTheOriginsAndNoneIsKept) {
  EXPECT_COLOR(bg("rgb(from rgb(20% 40% 60% / 0.8) g b r)"), "color(srgb 0.4 0.6 0.2 / 0.8)", 1e-6);
  EXPECT_COLOR(bg("rgb(from rgb(20% 40% 60% / none) r g b)"), "color(srgb 0.2 0.4 0.6 / none)", 1e-6);
  // "if calculations are done on missing values, none is treated as 0. This includes using channel keywords with a value of none in math expressions."
  EXPECT_COLOR(bg("rgb(from rgb(20% none 60%) r calc(g + 51) b)"), "color(srgb 0.2 0.2 0.6)", 1e-6);
  EXPECT_COLOR(bg("rgb(from rgb(20% none 60%) r g b)"), "color(srgb 0.2 none 0.6)", 1e-6);
  // Color 4, 16.2.2 and Color 5, 11: hsl() and hwb() keep their function when a channel is none
  EXPECT_EQ(bg("hsl(from rebeccapurple h s none)").rfind("hsl(270 ", 0), 0u);
  EXPECT_EQ(bg("hwb(from rebeccapurple none w b)").rfind("hwb(none ", 0), 0u);
  // "the achromatic origin color has a missing hue; the relative color also has a missing hue": oklch(0.592 0.009 17.42) carried as oklch(0.592 0.009 none), then 0.474
  EXPECT_COLOR(bg("oklch(from hsl(none 3% 50%) calc(l * 0.8) c h)"), "oklch(0.474 0.009 none)", 1e-3);
}

/* Color 5, 4.10: "The result of this function is in the color space of the origin color." and the examples with oklch(60% 0.25 315 / 0.3). */
TEST(CssColorRelative, AlphaKeepsTheOriginsSpace) {
  EXPECT_COLOR(bg("alpha(from oklch(60% 0.25 315 / 0.3) / 80%)"), "oklch(0.6 0.25 315 / 0.8)", 1e-6);
  EXPECT_COLOR(bg("alpha(from oklch(60% 0.25 315 / 0.8) / calc(alpha * 0.5))"), "oklch(0.6 0.25 315 / 0.4)", 1e-6);
  EXPECT_COLOR(bg("alpha(from red / 0.5)"), "color(srgb 1 0 0 / 0.5)", 1e-6);
  EXPECT_COLOR(bg("alpha(from color(display-p3 1 0 0) / 0.5)"), "color(display-p3 1 0 0 / 0.5)", 1e-6);
  EXPECT_COLOR(bg("alpha(from red / none)"), "color(srgb 1 0 0 / none)", 1e-6);
  EXPECT_COLOR(bg("alpha(from red / 2)"), "color(srgb 1 0 0)", 1e-6);
  EXPECT_COLOR(bg("alpha(from red / -1)"), "color(srgb 1 0 0 / 0)", 1e-6);
}

/* Color 5, 8: "contrast-color() resolves to either white or black, whichever produces maximum color contrast for text when the input color is used as a solid background.
   If both white and black produce the same contrast, it resolves to white." and "colors returned by this function should still meet the WCAG 2.1 ... AA large text". */
TEST(CssColorContrast, WhiteOrBlackWhicheverHasTheMoreContrast) {
  EXPECT_EQ(bg("contrast-color(white)"), "rgb(0, 0, 0)");
  EXPECT_EQ(bg("contrast-color(black)"), "rgb(255, 255, 255)");
  EXPECT_EQ(bg("contrast-color(yellow)"), "rgb(0, 0, 0)");
  EXPECT_EQ(bg("contrast-color(navy)"), "rgb(255, 255, 255)");
  // the alpha of the background is not part of the color that has to be read against ("a solid background")
  EXPECT_EQ(bg("contrast-color(rgb(255 255 255 / 0.2))"), "rgb(0, 0, 0)");
  EXPECT_EQ(bg("contrast-color(transparent)"), "rgb(255, 255, 255)");
  // any color space, an out-of-gamut color, a nested one and a mix
  EXPECT_EQ(bg("contrast-color(oklch(0.9 0.1 90))"), "rgb(0, 0, 0)");
  EXPECT_EQ(bg("contrast-color(color(srgb 10 10 10))"), "rgb(0, 0, 0)");
  EXPECT_EQ(bg("contrast-color(color(srgb -10 -10 -10))"), "rgb(255, 255, 255)");
  EXPECT_EQ(bg("contrast-color(contrast-color(pink))"), "rgb(255, 255, 255)");
  EXPECT_EQ(bg("contrast-color(color-mix(in srgb, white, black 80%))"), "rgb(255, 255, 255)");
  // contrast-color() is a color like any other inside another function
  EXPECT_COLOR(bg("rgb(from contrast-color(blue) r g b)"), "color(srgb 1 1 1)", 1e-6);
  EXPECT_COLOR(bg("color-mix(in srgb, contrast-color(blue) 100%, purple)"), "color(srgb 1 1 1)", 1e-6);
  // the guarantee of the draft: the color chosen has a contrast ratio of at least 3 (WCAG 2.1 AA for large text) against the background, over a sweep of the cube
  size_t checked = 0;
  for (int r = 0; r <= 255; r += 51) {
    for (int g = 0; g <= 255; g += 51) {
      for (int b = 0; b <= 255; b += 51) {
        const std::string v = bg("contrast-color(rgb(" + std::to_string(r) + " " + std::to_string(g) + " " + std::to_string(b) + "))");
        ASSERT_TRUE(v == "rgb(0, 0, 0)" || v == "rgb(255, 255, 255)") << v;
        auto lin = [](int c) {
          const double s = c / 255.0;
          return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
        };
        const double l = 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b);
        const double chosen = v == "rgb(0, 0, 0)" ? (l + 0.05) / 0.05 : 1.05 / (l + 0.05);
        const double other = v == "rgb(0, 0, 0)" ? 1.05 / (l + 0.05) : (l + 0.05) / 0.05;
        EXPECT_GE(chosen, other) << r << " " << g << " " << b;
        EXPECT_GE(chosen, 3.0) << r << " " << g << " " << b;
        checked++;
      }
    }
  }
  EXPECT_EQ(checked, 216u);
  // `currentcolor` inside contrast-color() stays the keyword in the computed value, as it does in a mix: the value is the element's color only when it is used, and an element that
  // inherits the property takes the function and resolves it against its own color (the Color page contrast-color-currentcolor-inherited.html; both pinned browsers)
  EXPECT_EQ(computed("color: white", "background-color: contrast-color(currentcolor)", "background-color"), "contrast-color(currentcolor)");
  EXPECT_EQ(computed("color: black", "background-color: contrast-color(currentcolor)", "background-color"), "contrast-color(currentcolor)");
}

/* Values 4, 10.13 and Color 4, 15.4: a calculation in a channel is resolved against the environment (a container length through the callback), NaN is zero. */
TEST(CssColorRelative, CalculationsInChannelsAreResolvedThroughTheEnvironment) {
  GTEXT_CSS_Environment env = gtext_css_environment_default();
  env.container_size = [](void *, GTEXT_CSS_Element_Handle, GTEXT_CSS_Container_Axis, double * px) {
    *px = 200.0;
    return true;
  };
  // 2cqw is 4px: sign(4px - 10px) is -1
  EXPECT_COLOR(bg("lab(calc(50 + (sign(2cqw - 10px) * 10)) 30 50 / 0.51)", "", &env), "lab(40 30 50 / 0.51)", 1e-9);
  EXPECT_COLOR(bg("color(srgb calc(0.5 + (sign(20cqw - 10px) * 0.1)) 0 0 / 0.52)", "", &env), "color(srgb 0.6 0 0 / 0.52)", 1e-9);
  EXPECT_COLOR(bg("hwb(calc(110deg + (sign(2cqw - 10px) * 10deg)) 30 50 / 51%)", "", &env), "rgba(94, 128, 77, 0.51)", 0.5);
  // without the callback the color stays as written, flagged, and is never guessed
  Style s;
  Context ctx(R());
  Block b(R(), "background-color: lab(calc(50 + sign(2cqw - 10px)) 30 50)");
  s.compute(ctx, &b);
  EXPECT_EQ(s.status, GTEXT_CSS_OK) << s.result.name;
  const GTEXT_CSS_Value * v = gtext_css_computed_root(s.s, find(R(), "background-color"));
  ASSERT_NE(v, nullptr);
  EXPECT_NE(v->flags & GTEXT_CSS_VFLAG_COLOR_UNRESOLVED, 0u);
  // Color 4, 15.4 and Properties and Values: a NaN alpha is 0
  EXPECT_COLOR(computed("", "color: color(srgb 1 1 1 / calc(NaN))", "color"), "color(srgb 1 1 1 / 0)", 1e-9);
  EXPECT_COLOR(bg("rgb(from red r g b / calc(NaN))"), "color(srgb 1 0 0 / 0)", 1e-9);
}

/* Color 5, 4.1 "Relative sRGB Colors": "Relative color syntax only applies to the modern color syntax. It cannot be used with legacy color syntax and attempting to do so is an
   error." A color the library cannot compute without the user's profile stays as written, flagged, and goes to the caller's callback. */
TEST(CssColorRelative, WhatCannotBeComputedStaysAndTheCallersColorsAreTheirs) {
  EXPECT_EQ(bg("rgb(from red, 1, 2, 3)"), "transparent");
  // a custom color space needs the profile of an @color-profile rule, which is the caller's (Color 5, 5.3): the mix stays as written
  EXPECT_EQ(bg("color-mix(in --profile, red, blue)"), "color-mix(in --profile, red, blue)");
  GTEXT_CSS_Environment env = gtext_css_environment_default();
  env.resolve_color = [](void *, const GTEXT_CSS_Color * c, GTEXT_CSS_Color_Scheme, double rgba[4]) {
    if (c->kind == GTEXT_CSS_COLOR_KEYWORD && c->keyword == GTEXT_CSS_COLOR_KW_SYSTEM) {
      rgba[0] = 0;
      rgba[1] = 128;
      rgba[2] = 0;
      rgba[3] = 1;
      return true;
    }
    return false;
  };
  // a system color is the caller's, and a mix that holds one is computed with the caller's answer
  EXPECT_COLOR(bg("color-mix(in srgb, Canvas, white)", "", &env), "color(srgb 0.5 0.75098 0.5)", 1e-4);
  EXPECT_EQ(bg("Canvas", "", &env), "rgb(0, 128, 0)");
}

} // namespace
