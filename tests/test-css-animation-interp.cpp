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
 * The combination of two computed values by their type (src/css/css_anim_interp.c), against Web Animations 1 ("Combining effects") and CSS Values 4 ("Combining values:
 * interpolation, addition, and accumulation").
 */

#include <string>

#include <gtest/gtest.h>

#include "../src/css/css_anim_internal.h"
#include "css_compute_test_support.h"
#include "markup_test_support.h"

using namespace cmp;

namespace {

/// The computed value of a property declared `value`, kept with its style.
struct Computed {
  Context ctx{R()};
  Block block;
  Style style;
  const GTEXT_CSS_Value * root = nullptr;
  Computed(const std::string & property, const std::string & value) : block(R(), property + ": " + value) {
    style.compute(ctx, &block);
    root = gtext_css_computed_root(style.s, find(R(), property));
  }
  Computed(const Computed &) = delete;
  Computed & operator=(const Computed &) = delete;
};

std::string mix(const std::string & property, const std::string & a, const std::string & b, double p, css_blend_op op = CSS_BLEND_LERP, bool * discrete = nullptr) {
  Computed x(property, a), y(property, b);
  EXPECT_NE(x.root, nullptr) << a;
  EXPECT_NE(y.root, nullptr) << b;
  char * text = nullptr;
  size_t n = 0;
  const GTEXT_CSS_Status st = css_interp_text(nullptr, op, x.root, y.root, p, nullptr, &text, &n, discrete);
  if (st != GTEXT_CSS_OK) {
    return std::string("<") + gtext_css_status_string(st) + ">";
  }
  std::string out(text, n);
  gtext_css_serialize_buffer_free(nullptr, text);
  return out;
}

/* Web Animations 1: interpolation "returns V_start at p = 0 and V_end at p = 1" for every type; between, a length is a number of its unit. */
TEST(CssAnimationInterp, ALengthIsInterpolatedAsANumber) {
  EXPECT_EQ(mix("width", "0px", "100px", 0.0), "0px");
  EXPECT_EQ(mix("width", "0px", "100px", 0.25), "25px");
  EXPECT_EQ(mix("width", "10px", "20px", 1.0), "20px");
  EXPECT_EQ(mix("width", "10px", "30px", 1.5), "40px") << "an easing may overshoot";
  EXPECT_EQ(mix("width", "10px", "30px", -0.5), "0px");
}

/* Values 4: an integer is interpolated as a real number and "rounded to the closest integer, ties toward positive infinity". */
TEST(CssAnimationInterp, AnIntegerIsRounded) {
  EXPECT_EQ(mix("z-index", "1", "4", 0.5), "3");
  EXPECT_EQ(mix("z-index", "1", "4", 0.4), "2");
  EXPECT_EQ(mix("z-index", "-3", "-2", 0.5), "-2");
  EXPECT_EQ(mix("opacity", "0", "1", 0.25), "0.25");
}

TEST(CssAnimationInterp, APercentageAndAnAngleAreNumbers) {
  EXPECT_EQ(mix("font-stretch", "50%", "200%", 0.5), "125%");
  EXPECT_EQ(mix("font-style", "oblique 10deg", "oblique 30deg", 0.5), "oblique 20deg");
}

/* Values 4: "calc() ... a length and a percentage": the interpolation of two values of different kinds is a calculation. */
TEST(CssAnimationInterp, ALengthAndAPercentageMakeACalculation) {
  EXPECT_EQ(mix("margin-left", "10px", "50%", 0.5), "calc((10px) * 0.5 + (50%) * 0.5)");
  EXPECT_EQ(mix("margin-left", "calc(10% + 4px)", "20px", 0.5), "calc((calc(10% + 4px)) * 0.5 + (20px) * 0.5)");
}

/* Color 4, "Interpolating colors": legacy sRGB colors are interpolated premultiplied. */
TEST(CssAnimationInterp, ALegacyColorIsInterpolatedInSrgbPremultiplied) {
  EXPECT_EQ(mix("color", "rgb(0, 100, 200)", "rgb(100, 200, 0)", 0.5), "rgb(50 150 100)");
  EXPECT_EQ(mix("color", "rgba(0, 0, 0, 0)", "rgb(100, 200, 0)", 0.5), "rgb(100 200 0 / 0.5)");
  EXPECT_EQ(mix("color", "red", "blue", 0.0), "red") << "p = 0 is the first value as it is";
}

/* A pair of lists of one shape is combined item by item; a pair that differs in a keyword is discrete. */
TEST(CssAnimationInterp, AListIsCombinedByItem) {
  EXPECT_EQ(mix("text-shadow", "rgb(0, 200, 0) 1px 2px 3px", "rgb(100, 0, 200) 3px 2px 1px", 0.5), "rgb(50 100 100) 2px 2px 2px 0px");
  bool discrete = false;
  EXPECT_EQ(mix("font-variation-settings", "\"wght\" 100", "\"wdth\" 200", 0.7, CSS_BLEND_LERP, &discrete), "\"wdth\" 200");
  EXPECT_TRUE(discrete);
  discrete = true;
  EXPECT_EQ(mix("font-variation-settings", "\"wght\" 100", "\"wght\" 300", 0.5, CSS_BLEND_LERP, &discrete), "\"wght\" 200");
  EXPECT_FALSE(discrete);
}

/* Web Animations 1: "discrete ... V_start for p < 0.5, V_end for p >= 0.5". */
TEST(CssAnimationInterp, ADiscreteTypeFlipsAtTheMiddle) {
  bool discrete = false;
  EXPECT_EQ(mix("float", "left", "right", 0.499, CSS_BLEND_LERP, &discrete), "left");
  EXPECT_TRUE(discrete);
  EXPECT_EQ(mix("float", "left", "right", 0.5), "right");
  EXPECT_EQ(mix("width", "10px", "auto", 0.2), "10px") << "a length and a keyword are not of one type";
  EXPECT_EQ(mix("width", "10px", "auto", 0.8), "auto");
}

/* Web Animations 1: addition and accumulation of lengths are the sum, the operands in the order underlying value then effect value. */
TEST(CssAnimationInterp, AdditionAndAccumulationAreTheSumAndAnIncompatibleTypeIsReplaced) {
  EXPECT_EQ(mix("width", "10px", "30px", 0.0, CSS_BLEND_ADD), "40px");
  EXPECT_EQ(mix("width", "10px", "30px", 0.0, CSS_BLEND_ACCUMULATE), "40px");
  EXPECT_EQ(mix("float", "left", "right", 0.0, CSS_BLEND_ADD), "right");
  EXPECT_EQ(mix("margin-left", "10px", "30%", 0.0, CSS_BLEND_ADD), "calc((10px) * 1 + (30%) * 1)");
}

TEST(CssAnimationInterpOom, EveryAllocationOfACombinationIsSwept) {
  Computed x("text-shadow", "rgb(0, 200, 0) 1px 2px 3px, red 1px 1px"), y("text-shadow", "rgb(100, 0, 200) 3px 2px 1px, blue 2px 2px");
  size_t failures = 0;
  for (size_t n = 1; n < 400; n++) {
    FaultAllocator fa;
    char * text = nullptr;
    size_t len = 0;
    fa.arm(n);
    const GTEXT_CSS_Status st = css_interp_text(&fa.alloc, CSS_BLEND_LERP, x.root, y.root, 0.5, nullptr, &text, &len, nullptr);
    fa.disarm();
    if (!fa.failed) {
      EXPECT_EQ(st, GTEXT_CSS_OK);
      GTEXT_CSS_Serialize_Options so{};
      so.allocator = &fa.alloc;
      gtext_css_serialize_buffer_free(&so, text);
      EXPECT_TRUE(fa.live.empty());
      break;
    }
    failures++;
    EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
    EXPECT_EQ(text, nullptr) << n;
    EXPECT_TRUE(fa.live.empty()) << n << ": a block is left";
    EXPECT_EQ(fa.bad_frees, 0u) << n;
  }
  EXPECT_GE(failures, 4u);
}

}  // namespace
