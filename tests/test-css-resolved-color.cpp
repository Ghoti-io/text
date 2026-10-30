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
 * The resolved value of a color that holds `currentcolor` (tools/oracle/css_resolved.h). CSSOM, "Resolved Values": for a color property the resolved value is
 * the used value, in which `currentcolor` is the element's color. Color 5, 10.1 and 10.2 keep the keyword in the *computed* value of a mix or a relative color,
 * so the computed layer (the style) still holds the function; the layer that is asked for the resolved value computes the mix. Both pinned browsers write the
 * mixed color for `background-color: color-mix(in srgb, currentcolor, blue)` under `color: rgb(0, 255, 0)` (recorded in css_browser_answers.jsonl, cases `cc-bg.2`, `cc-bg.3`).
 */

#include <string>

#include <gtest/gtest.h>

#include "css_compute_test_support.h"
#include "../tools/oracle/css_resolved.h"

using namespace cmp;

namespace {

/// What a style declared `parent_decls` then `decls` resolves `property` to.
std::string resolved_of(const std::string & parent_decls, const std::string & decls, const std::string & property) {
  Context ctx(R());
  Block pb(R(), parent_decls);
  Style parent;
  parent.compute(ctx, &pb);
  Block cb(R(), decls);
  Style style;
  style.compute(ctx, &cb, &parent);
  if (style.status != GTEXT_CSS_OK) {
    return std::string("<style ") + gtext_css_status_string(style.status) + ">";
  }
  std::string out;
  GTEXT_CSS_Compute_Result res{};
  const GTEXT_CSS_Status st = resolved_value::read(R(), style.s, find(R(), property), &out, &res);
  return st == GTEXT_CSS_OK ? out : std::string("<") + gtext_css_status_string(st) + ">";
}

} // namespace

// The computed value keeps the function (Color 5, 10.1); the resolved value is the mix.
TEST(CssResolvedColor, CurrentcolorInAMixIsTheElementsColor) {
  const std::string parent = "color: rgb(0, 255, 0)";
  EXPECT_EQ(resolved_of(parent, "background-color: color-mix(in srgb, currentcolor, blue)", "background-color"), "color(srgb 0 0.5 0.5)");
  EXPECT_EQ(resolved_of(parent, "background-color: color-mix(in srgb, blue, currentcolor 25%)", "background-color").rfind("color(srgb", 0), 0u);
  EXPECT_EQ(resolved_of(parent, "border-top-color: color-mix(in srgb, currentcolor 50%, rgb(255, 0, 0))", "border-top-color"), "color(srgb 0.5 0.5 0)");
}

TEST(CssResolvedColor, CurrentcolorInARelativeColorIsTheElementsColor) {
  const std::string parent = "color: rgb(0, 255, 0)";
  EXPECT_EQ(resolved_of(parent, "background-color: rgb(from currentcolor r g b / 0.5)", "background-color"), "color(srgb 0 1 0 / 0.5)");
}

TEST(CssResolvedColor, CurrentcolorAloneAndInAnImageIsTheElementsColor) {
  const std::string parent = "color: rgb(0, 255, 0)";
  EXPECT_EQ(resolved_of(parent, "background-color: currentcolor", "background-color"), "rgb(0, 255, 0)");
  EXPECT_EQ(resolved_of(parent, "background-image: linear-gradient(red, currentcolor)", "background-image"), "linear-gradient(rgb(255, 0, 0), rgb(0, 255, 0))");
  EXPECT_EQ(resolved_of(parent, "background-image: linear-gradient(red, color-mix(in srgb, currentcolor, blue))", "background-image"),
      "linear-gradient(rgb(255, 0, 0), color(srgb 0 0.5 0.5))");
}

// The `color` property's own `currentcolor` is the inherited color (Color 4, 15.5) and is not changed here.
TEST(CssResolvedColor, TheColorPropertyItself) {
  EXPECT_EQ(resolved_of("color: rgb(0, 255, 0)", "color: color-mix(in srgb, currentcolor, blue)", "color"), "color(srgb 0 0.5 0.5)");
}
