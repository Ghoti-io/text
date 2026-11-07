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
 * The computed value of the basic shapes (css_shape_compute.c). Shapes 1 (served 2026-10-30), 3.2: "All <basic-shape-rect> functions compute to the equivalent
 * inset() function."; 3.3: "Omitting optional components means that default values do not show up in the serialization." and "<position> serialization rules
 * mean that keywords compute to percentages, and serialize in horizontal-vertical order."
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_compute_test_support.h"

using namespace cmp;

namespace {

struct Case {
  const char * property;
  const char * specified;
  const char * computed;
};

void expect_all(const std::vector<Case> & cases) {
  for (const Case & c : cases) {
    const std::string got = root_computed(std::string(c.property) + ": " + c.specified, c.property);
    EXPECT_EQ(got, c.computed) << c.property << ": " << c.specified;
    // the computed text is a fixed point: read back and computed again it is the same
    EXPECT_EQ(root_computed(std::string(c.property) + ": " + got, c.property), got) << c.property << ": " << got;
  }
}

// "Given rect(t r b l), the equivalent function is inset(t calc(100% - r) calc(100% - b) l)."; "An auto value makes the edge of the box coincide with the
// corresponding edge of the reference box."
TEST(CssShapeCompute, RectIsTheEquivalentInset) {
  expect_all({
      {"clip-path", "rect(0 0 0 0)", "inset(0px 100% 100% 0px)"},
      {"clip-path", "rect(10px auto 20px 30px)", "inset(10px 0% calc(100% - 20px) 30px)"},
      {"clip-path", "rect(auto auto auto auto)", "inset(0%)"},
      {"clip-path", "rect(10% 20% 15% 12%)", "inset(10% 80% 85% 12%)"},
      {"clip-path", "rect(-10% -20% -15% -12%)", "inset(-10% 120% 115% -12%)"},
      {"clip-path", "rect(0px 1% auto 3% round 0 1px)", "inset(0px 99% 0% 3% round 0px 1px)"},
      {"shape-outside", "rect(0px calc(100% - 20px) 2% 3em)", "inset(0px calc(0% + 20px) 98% 48px)"},
      {"clip-path", "rect(10% 95% 97% 12%)", "inset(10% 5% 3% 12%)"},
  });
}

// "Given xywh(x y w h), the equivalent function is inset(y calc(100% - x - w) calc(100% - y - h) x)."
TEST(CssShapeCompute, XywhIsTheEquivalentInset) {
  expect_all({
      {"clip-path", "xywh(0px 1px 2% 3em)", "inset(1px 98% calc(100% - 49px) 0px)"},
      {"clip-path", "xywh(10px 20px 30px 25px round 0 1px)", "inset(20px calc(100% - 40px) calc(100% - 45px) 10px round 0px 1px)"},
      {"clip-path", "xywh(0px 1% 2px 3% round 0px 1px 2em)", "inset(1% calc(100% - 2px) 96% 0px round 0px 1px 32px)"},
      {"offset-path", "xywh(0 1% 2px 3em)", "inset(1% calc(100% - 2px) calc(99% - 48px) 0px)"},
  });
}

// Four equal offsets of an inset() are written as one, as a specified inset() is ("the shortest form").
TEST(CssShapeCompute, EqualInsetOffsetsAreOne) {
  expect_all({
      {"clip-path", "rect(10% 90% 90% 10%)", "inset(10%)"},
      {"clip-path", "rect(10% 90% 80% 20%)", "inset(10% 10% 20% 20%)"},
      {"clip-path", "rect(10px 80% 20px 20%)", "inset(10px 20% calc(100% - 20px))"},
  });
}

// Shapes 1, 3.3: "<position> serialization rules mean that keywords compute to percentages, and serialize in horizontal-vertical order."
TEST(CssShapeCompute, PositionsAreHorizontalThenVerticalPercentages) {
  expect_all({
      {"shape-outside", "circle(at top 0% right 10%)", "circle(at 90% 0%)"},
      {"shape-outside", "circle(20px at center)", "circle(20px at 50% 50%)"},
      {"shape-outside", "circle(farthest-corner at center top)", "circle(farthest-corner at 50% 0%)"},
      {"shape-outside", "circle(4% at top right)", "circle(4% at 100% 0%)"},
      {"shape-outside", "ellipse(20px 40px at center)", "ellipse(20px 40px at 50% 50%)"},
      {"shape-outside", "ellipse(4% 20% at top right)", "ellipse(4% 20% at 100% 0%)"},
      {"shape-outside", "circle(at left 10px bottom 20px)", "circle(at 10px calc(100% - 20px))"},
      {"shape-outside", "circle(at 20px)", "circle(at 20px 50%)"},
      {"shape-outside", "circle(at bottom)", "circle(at 50% 100%)"},
      {"offset-path", "circle(at right 10px top 50%)", "circle(at calc(100% - 10px) 50%)"},
      {"clip-path", "circle(at 75% 25%)", "circle(at 75% 25%)"},
  });
}

// Images 4, 3.2.2: "<radial-size> = <radial-extent>{1,2} | <length-percentage [0,∞]>{1,2}"; Shapes 1: "If omitted it defaults to closest-side." and "Omitting optional
// components means that default values do not show up in the serialization."
TEST(CssShapeCompute, TheDefaultRadiusIsNotWritten) {
  expect_all({
      {"shape-outside", "circle(closest-side at center)", "circle(at 50% 50%)"},
      {"shape-outside", "circle(closest-side)", "circle()"},
      {"shape-outside", "ellipse(closest-side closest-side at 10% 20%)", "ellipse(at 10% 20%)"},
      {"shape-outside", "ellipse(closest-corner farthest-corner at 20px 50px)", "ellipse(closest-corner farthest-corner at 20px 50px)"},
      {"shape-outside", "circle(farthest-side at center)", "circle(farthest-side at 50% 50%)"},
  });
}

// "Two <length-percentage> values are invalid." for circle(); an ellipse takes one or two of <radial-size>. A declaration that does not parse is dropped, so the
// property has its initial value.
TEST(CssShapeCompute, RadiusGrammar) {
  EXPECT_EQ(root_computed("shape-outside: circle(4% 20%)", "shape-outside"), "none");
  EXPECT_EQ(root_computed("shape-outside: circle(closest-side closest-side)", "shape-outside"), "none");
  EXPECT_EQ(root_computed("shape-outside: ellipse(4% 20%)", "shape-outside"), "ellipse(4% 20%)");
  EXPECT_EQ(root_computed("shape-outside: circle(-10px)", "shape-outside"), "none");
}

// Images 4 writes <radial-size> as an extent pair or a length-percentage pair and no mixed pair, and the Shapes page basic-shape-circle-ellipse-serialization.html
// reads `ellipse(farthest-side 10px at center)` as valid; both pinned browsers (Chromium 156.0.8078.47, Firefox 158.0.1) accept the mixed pair and write it back
// as given. The ellipse takes each of its two radii as an extent or a length-percentage; a circle, which takes one component, still refuses the pair.
TEST(CssShapeCompute, AnEllipseRadiusPairMayMixAnExtentAndALengthPercentage) {
  expect_all({
      {"shape-outside", "ellipse(farthest-side 10px at center)", "ellipse(farthest-side 10px at 50% 50%)"},
      {"shape-outside", "ellipse(10% closest-corner at 50% 50%)", "ellipse(10% closest-corner at 50% 50%)"},
      {"shape-outside", "ellipse(closest-side 10%)", "ellipse(closest-side 10%)"},
  });
  EXPECT_EQ(root_computed("shape-outside: ellipse(closest-side 10px 5px)", "shape-outside"), "none");
  EXPECT_EQ(root_computed("shape-outside: ellipse(closest-side -1px)", "shape-outside"), "none");
  EXPECT_EQ(root_computed("shape-outside: circle(closest-side 10px)", "shape-outside"), "none");
}

// Motion 1, ray(): "<angle> values are interpreted as bearing angles"; the computed angle is in degrees, the default size and the position as percentages.
TEST(CssShapeCompute, RayIsDegreesThenSizeContainAndPosition) {
  expect_all({
      {"offset-path", "ray(0rad closest-side)", "ray(0deg)"},
      {"offset-path", "ray(0.25turn closest-corner contain)", "ray(90deg closest-corner contain)"},
      {"offset-path", "ray(200grad farthest-side)", "ray(180deg farthest-side)"},
      {"offset-path", "ray(calc(180deg - 45deg) farthest-side)", "ray(135deg farthest-side)"},
      {"offset-path", "ray(0deg at center center sides)", "ray(0deg sides at 50% 50%)"},
      {"offset-path", "ray(0deg at center center contain)", "ray(0deg contain at 50% 50%)"},
      {"offset-path", "ray(0deg sides at center center)", "ray(0deg sides at 50% 50%)"},
      {"offset-path", "ray(0deg at 100px 100px)", "ray(0deg at 100px 100px)"},
  });
}

// Lengths are absolute in every shape ("computed as specified" with the lengths of the values made absolute).
TEST(CssShapeCompute, LengthsAreAbsoluteEvenWhereThePropertyIsAsSpecified) {
  expect_all({
      {"offset-path", "inset(0px 1px 2% 3em)", "inset(0px 1px 2% 48px)"},
      {"offset-path", "polygon(round 1em, 1px 2px, 3em 4em)", "polygon(round 16px, 1px 2px, 48px 64px)"},
      {"offset-path", "polygon(round calc(1em + 2px), 1px 2px, 3px 4px)", "polygon(round 18px, 1px 2px, 3px 4px)"},
      {"offset-path", "inset(0px round 0px 1px 2% 3em)", "inset(0px round 0px 1px 2% 48px)"},
  });
}

// shape(): positions are percentages, an arc's defaults are not written.
TEST(CssShapeCompute, ShapeFunction) {
  expect_all({
      {"shape-outside", "shape(from 20px 40px, smooth to center 20%)", "shape(from 20px 40px, smooth to 50% 20%)"},
      {"offset-path", "shape(from 10px 10px, arc to 50px 3pt of 10px small rotate 0deg)", "shape(from 10px 10px, arc to 50px 4px of 10px)"},
      {"offset-path", "shape(from 10px 10px, arc to 50px 3pt of 10px 10px)", "shape(from 10px 10px, arc to 50px 4px of 10px 10px)"},
      {"offset-path", "shape(from 10% 1rem, arc to 50px 3pt of 20% cw large rotate 25deg)", "shape(from 10% 16px, arc to 50px 4px of 20% cw large rotate 25deg)"},
      {"offset-path", "shape(from 10px 10px, curve to 50px 20px with 10rem 1px / 20% 1em)", "shape(from 10px 10px, curve to 50px 20px with 160px 1px / 20% 16px)"},
      {"offset-path", "shape(from 10px 10px, hline by 10px, vline to 5rem)", "shape(from 10px 10px, hline by 10px, vline to 80px)"},
      {"offset-path", "shape(from 10px 10px, hline to right, vline to center)", "shape(from 10px 10px, hline to 100%, vline to 50%)"},
      {"offset-path", "shape(from left top, line to right bottom)", "shape(from 0% 0%, line to 100% 100%)"},
  });
}

// Shapes 1, 3.3: a reference box that is the default of the property is not written when a shape is with it (margin-box for shape-outside, border-box for the
// others); border-shape: "The first (outer) shape defaults to the border box, and the second (inner) shape defaults to the padding box." (Borders 4, 7.3)
TEST(CssShapeCompute, TheDefaultReferenceBoxIsNotWritten) {
  expect_all({
      {"shape-outside", "circle() margin-box", "circle()"},
      {"shape-outside", "polygon(evenodd, -10px -20px, -30px -40px, -50px -60px) margin-box", "polygon(evenodd, -10px -20px, -30px -40px, -50px -60px)"},
      {"shape-outside", "circle() border-box", "circle() border-box"},
      {"shape-outside", "margin-box", "margin-box"},
      {"offset-path", "inset(10% 20% 30% 40%) border-box", "inset(10% 20% 30% 40%)"},
      {"offset-path", "border-box", "border-box"},
      {"offset-path", "fill-box ellipse(at 50% 60%)", "ellipse(at 50% 60%) fill-box"},
      {"border-shape", "circle() border-box polygon(10px 10px, 100px 10px, 10px 100px) border-box", "circle() polygon(10px 10px, 100px 10px, 10px 100px) border-box"},
      {"border-shape", "circle() padding-box polygon(10px 10px, 100px 10px, 10px 100px) padding-box", "circle() padding-box polygon(10px 10px, 100px 10px, 10px 100px)"},
      {"border-shape", "circle() border-box circle() padding-box", "circle() circle()"},
      {"border-shape", "circle() content-box", "circle() content-box"},
  });
}

// What is not a shape, or not a value, is left alone.
TEST(CssShapeCompute, OtherValuesAreLeftAlone) {
  expect_all({
      {"clip-path", "none", "none"},
      {"clip-path", "url(#a)", "url(\"#a\")"},
      {"offset-path", "path(\"M 10 10 h 80 v 80 h -80 Z\")", "path(\"M 10 10 h 80 v 80 h -80 Z\")"},
      {"shape-outside", "polygon(1px 2px, 3px 4px, 5px 6px)", "polygon(1px 2px, 3px 4px, 5px 6px)"},
  });
}

// An absurdly long value is linear work and no recursion: a polygon of many points computes.
TEST(CssShapeCompute, ALongPolygonComputes) {
  std::string v = "polygon(";
  for (int i = 0; i < 2000; i++) {
    v += (i ? ", " : "") + std::to_string(i) + "px " + std::to_string(i + 1) + "em";
  }
  v += ")";
  const std::string got = root_computed("shape-outside: " + v, "shape-outside");
  EXPECT_EQ(got.compare(0, 8, "polygon("), 0);
  EXPECT_NE(got.find("1999px 32000px"), std::string::npos);
}

// Images 4 (served 2026-10-30), 3.2.2: "<radial-size> = <radial-extent>{1,2} | <length-percentage [0,∞]>{1,2}"; the pinned pages write a gradient without its defaults: the
// default size (farthest-corner), a centered position, `from 0deg`, a shape that the lengths of the size imply; positions as percentages; a stop with two positions as two stops.
TEST(CssImageCompute, GradientsDropTheirDefaults) {
  expect_all({
      {"background-image", "radial-gradient(farthest-corner, red, blue)", "radial-gradient(red, blue)"},
      {"background-image", "radial-gradient(farthest-corner at center, red, blue)", "radial-gradient(red, blue)"},
      {"background-image", "radial-gradient(farthest-corner at 10px 10px, red, blue)", "radial-gradient(at 10px 10px, red, blue)"},
      {"background-image", "radial-gradient(circle at center, red, blue)", "radial-gradient(circle, red, blue)"},
      {"background-image", "radial-gradient(circle 10px at 20px 30px, red, blue)", "radial-gradient(10px at 20px 30px, red, blue)"},
      {"background-image", "radial-gradient(ellipse 10px 20px, red, blue)", "radial-gradient(10px 20px, red, blue)"},
      {"background-image", "radial-gradient(closest-side, red, blue)", "radial-gradient(closest-side, red, blue)"},
      {"background-image", "conic-gradient(from 0deg at 50% 50%, red, gold)", "conic-gradient(red, gold)"},
      {"background-image", "conic-gradient(from 45deg at center, red, blue)", "conic-gradient(from 45deg, red, blue)"},
      {"background-image", "conic-gradient(from 0deg at 10px 10px, red, blue)", "conic-gradient(at 10px 10px, red, blue)"},
      {"background-image", "repeating-conic-gradient(from 0deg at center, red, gold)", "repeating-conic-gradient(red, gold)"},
      {"mask-image", "radial-gradient(at center, red, blue)", "radial-gradient(red, blue)"},
      {"list-style-image", "linear-gradient(to top right, red, blue)", "linear-gradient(to right top, red, blue)"},
  });
}

TEST(CssImageCompute, GradientPositionsArePercentages) {
  expect_all({
      {"background-image", "radial-gradient(at left, red, blue)", "radial-gradient(at 0% 50%, red, blue)"},
      {"background-image", "radial-gradient(at 10%, red, blue)", "radial-gradient(at 10% 50%, red, blue)"},
      {"background-image", "radial-gradient(at bottom 10% right 20%, red, blue)", "radial-gradient(at 80% 90%, red, blue)"},
      {"background-image", "conic-gradient(at right 10px top 20px, red, blue)", "conic-gradient(at calc(100% - 10px) 20px, red, blue)"},
      {"background-image", "conic-gradient(at 70px, red, blue)", "conic-gradient(at 70px 50%, red, blue)"},
      {"background-image", "radial-gradient(at 40px top, red, blue)", "radial-gradient(at 40px 0%, red, blue)"},
  });
}

TEST(CssImageCompute, AStopWithTwoPositionsIsTwoStops) {
  expect_all({
      {"background-image", "linear-gradient(black 0% 0.5em, white)", "linear-gradient(black 0%, black 8px, white)"},
      {"background-image", "radial-gradient(black 0% 0.5em, 25%, green 30% 60%, white)",
          "radial-gradient(black 0%, black 8px, 25%, green 30%, green 60%, white)"},
  });
}

TEST(CssImageCompute, ListsAndOtherImagesAreKept) {
  expect_all({
      {"background-image", "url(a.png), radial-gradient(at center, red, blue), none", "url(\"a.png\"), radial-gradient(red, blue), none"},
      {"background-image", "linear-gradient(red, blue)", "linear-gradient(red, blue)"},
  });
}

// Filter Effects 1 (served 2026-10-30), 6.2: "Omitted values are included and compute to their defaults."; 6.1: "the default value for omitted values for grayscale(), sepia()
// and invert() is 1", blur() "Default value when omitted is 0px.", "Values of amount over 100% are allowed but UAs must clamp the values to 1", "Negative values are not allowed."
TEST(CssFilterCompute, OmittedArgumentsAreTheirDefaults) {
  expect_all({
      {"filter", "blur()", "blur(0px)"},
      {"filter", "hue-rotate()", "hue-rotate(0deg)"},
      {"filter", "grayscale() invert() sepia() opacity()", "grayscale(1) invert(1) sepia(1) opacity(1)"},
      {"filter", "brightness() contrast() saturate()", "brightness(1) contrast(1) saturate(1)"},
      {"backdrop-filter", "contrast()", "contrast(1)"},
      {"filter", "drop-shadow(1px 2px)", "drop-shadow(currentcolor 1px 2px 0px)"},
      {"filter", "drop-shadow(1px 2px 3px red)", "drop-shadow(red 1px 2px 3px)"},
  });
}

TEST(CssFilterCompute, PercentagesAreNumbersAndAmountsAreClamped) {
  expect_all({
      {"filter", "grayscale(50%)", "grayscale(0.5)"},
      {"filter", "brightness(300%)", "brightness(3)"},
      {"filter", "saturate(calc(300%))", "saturate(3)"},
      {"filter", "invert(calc(300%))", "invert(1)"},
      {"filter", "opacity(2)", "opacity(1)"},
      {"filter", "contrast(calc(-10))", "contrast(0)"},
      {"filter", "sepia(calc(-10%))", "sepia(0)"},
      {"filter", "blur(calc(100px))", "blur(100px)"},
      {"filter", "hue-rotate(calc(90deg))", "hue-rotate(90deg)"},
      {"filter", "blur(2px) url(#a) invert(25%)", "blur(2px) url(\"#a\") invert(0.25)"},
  });
}

} // namespace
