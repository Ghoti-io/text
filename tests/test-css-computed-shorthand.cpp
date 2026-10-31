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
 * A shorthand written from the computed values of its longhands (css_computed_shorthand.c). Backgrounds 4 (served 2026-10-30), 2.7: the computed value of
 * `background-position` is "a list, each item a pair of offsets (horizontal and vertical) from the top left origin, each offset given as a computed
 * <length-percentage> value"; CSS Values 4, 8.3.2: "Computed values are always serialized as two offsets (without keywords) because the computed value does not
 * preserve syntactic distinctions."
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_compute_test_support.h"

using namespace cmp;

namespace {

/// The shorthand's text from the computed style of a root element declared `decls`; "<status>" where the call fails.
std::string shorthand_text(const std::string & decls, const std::string & shorthand, GTEXT_CSS_Status * status = nullptr) {
  Context ctx(R());
  Block cb(R(), decls);
  Style style;
  style.compute(ctx, &cb);
  if (style.status != GTEXT_CSS_OK) {
    return std::string("<style ") + gtext_css_status_string(style.status) + ">";
  }
  char * text = nullptr;
  size_t n = 0;
  GTEXT_CSS_Compute_Result res{};
  const GTEXT_CSS_Status st = gtext_css_computed_shorthand_text(style.s, find(R(), shorthand), &text, &n, &res);
  if (status) {
    *status = st;
  }
  if (st != GTEXT_CSS_OK) {
    return std::string("<") + gtext_css_status_string(st) + ">";
  }
  std::string s(text, n);
  gtext_css_serialize_buffer_free(nullptr, text);
  return s;
}

struct Case {
  const char * specified;
  const char * computed;
};

void expect_all(const char * property, const std::vector<Case> & cases) {
  for (const Case & c : cases) {
    const std::string got = shorthand_text(std::string(property) + ": " + c.specified, property);
    EXPECT_EQ(got, c.computed) << property << ": " << c.specified;
    // a fixed point: the text read back is the same text
    EXPECT_EQ(shorthand_text(std::string(property) + ": " + got, property), got) << property << ": " << got;
  }
}

// "Computed values are always serialized as two offsets (without keywords)": the origin keyword is folded into the offset, measured from the top left.
TEST(CssComputedShorthand, BackgroundPositionIsPairsOfOffsets) {
  expect_all("background-position", {
      {"50% 6px", "50% 6px"},
      {"1px", "1px 50%"},
      {"1px center", "1px 50%"},
      {"center top 8px", "50% 8px"},
      {"left", "0% 50%"},
      {"right 9%", "100% 9%"},
      {"left 10px center", "10px 50%"},
      {"right 11% bottom", "89% 100%"},
      {"left 12px top 13px", "12px 13px"},
      {"right center", "100% 50%"},
      {"left bottom", "0% 100%"},
      {"bottom", "50% 100%"},
      {"bottom 16% left", "0% 84%"},
      {"top 17px right -18px", "calc(100% + 18px) 17px"},
      {"top left", "0% 0%"},
      {"bottom right 19%", "81% 100%"},
      {"calc(10px + 0.5em) calc(10px - 0.5em)", "18px 2px"},
      {"center", "50% 50%"},
      {"center right 7%", "93% 50%"},
  });
}

// "A list, each item a pair of offsets": one pair for each layer, in the order of the layers.
TEST(CssComputedShorthand, BackgroundPositionLayers) {
  expect_all("background-position", {
      {"12px 13px, 50% 6px", "12px 13px, 50% 6px"},
      {"12px 13px, 50% 6px, 30px -10px, -7px 8px", "12px 13px, 50% 6px, 30px -10px, -7px 8px"},
      {"center, left, bottom", "50% 50%, 0% 50%, 50% 100%"},
      {"right 10px bottom 3px, left", "calc(100% - 10px) calc(100% - 3px), 0% 50%"},
  });
}

// The longhands of `background-position` keep the origin keyword (Backgrounds 4, 2.6: "an offset given as a computed <length-percentage> value, plus an origin keyword").
TEST(CssComputedShorthand, TheLonghandsKeepTheKeyword) {
  Context ctx(R());
  Block cb(R(), "background-position: right 11% bottom");
  Style style;
  style.compute(ctx, &cb);
  EXPECT_EQ(style.value("background-position-x"), "right 11%");
  EXPECT_EQ(style.value("background-position-y"), "bottom 0%");
}

// Backgrounds 4 (served 2026-10-30): `box-shadow: none` is "no shadow", a layer with neither a blur nor a spread. The computed longhands hold `0px` for both, which a
// shadow that is written would carry back; a layer whose offset is `none` has nothing to carry, so the shorthand is `none`, and reads back as the same longhands.
TEST(CssComputedShorthand, BoxShadowNoneIsNone) {
  expect_all("box-shadow", {
      {"none", "none"},
      {"1px 2px", "1px 2px 0px 0px"},
      {"1px 2px red", "red 1px 2px 0px 0px"},
  });
}

// `background` holds `background-position`: its text has the offsets alone too.
TEST(CssComputedShorthand, BackgroundHoldsThePosition) {
  EXPECT_EQ(shorthand_text("background: url(a) right 10px bottom 3px no-repeat", "background"), "url(\"a\") calc(100% - 10px) calc(100% - 3px) no-repeat");
}

// The properties that a shorthand resets are among its longhands: `mask` resets the `mask-border-*` properties, `animation` the timeline and range ones. A text that left
// them out would be empty.
TEST(CssComputedShorthand, MaskAndAnimationWriteBecauseTheyReadTheResets) {
  expect_all("mask", {
      {"none", "none"},
      {"url(\"https://example.com/\") 1px 2px / contain", "url(\"https://example.com/\") 1px 2px / contain"},
      {"repeat-y", "repeat-y"},
      {"border-box border-box", "none"},
      {"content-box content-box", "content-box"},
      {"border-box content-box", "border-box content-box"},
      {"border-box no-clip", "no-clip"},
      {"none alpha", "alpha"},
      {"url(\"https://example.com/\") add", "url(\"https://example.com/\")"},
      {"intersect no-clip space round 1px 2px / contain view-box, stroke-box url(a) luminance",
          "1px 2px / contain space round view-box no-clip intersect, url(\"a\") stroke-box luminance"},
  });
  expect_all("animation", {
      {"1s", "1s"},
      {"cubic-bezier(0, -2, 1, 3)", "cubic-bezier(0, -2, 1, 3)"},
      {"1s -3s", "1s -3s"},
      {"4", "4"},
      {"none, none", "none, none"},
      {"anim paused both reverse 4 1s -3s cubic-bezier(0, -2, 1, 3)", "1s cubic-bezier(0, -2, 1, 3) -3s 4 reverse both paused anim"},
      {"anim paused both reverse, 4 1s -3s cubic-bezier(0, -2, 1, 3)", "reverse both paused anim, 1s cubic-bezier(0, -2, 1, 3) -3s 4"},
  });
}

// Fonts 4: `font-width` computes to a percentage, and the `font` shorthand takes the keywords only: a percentage that a keyword names is written as the keyword.
TEST(CssComputedShorthand, FontWritesTheKeywordOfAComputedWidth) {
  expect_all("font", {
      {"12px serif", "12px serif"},
      {"semi-condensed 12px serif", "semi-condensed 12px serif"},
      {"ultra-expanded bold 12px/2 serif", "700 ultra-expanded 12px/2 serif"},
  });
  // a width that no keyword names has no text in the shorthand
  EXPECT_EQ(shorthand_text("font: 12px serif; font-width: 90%", "font"), "");
}

// The empty text, not a failure, where the longhands cannot be written as one value: `all` has more longhands than a shorthand is read from.
TEST(CssComputedShorthand, NoTextIsTheEmptyString) {
  GTEXT_CSS_Status st = GTEXT_CSS_E_INTERNAL;
  EXPECT_EQ(shorthand_text("all: initial", "all", &st), "");
  EXPECT_EQ(st, GTEXT_CSS_OK);
}

// Not a shorthand, not a property, nothing: GTEXT_CSS_E_INVALID, and no buffer.
TEST(CssComputedShorthand, WhatIsNotAShorthandIsInvalid) {
  Context ctx(R());
  Block cb(R(), "color: red");
  Style style;
  style.compute(ctx, &cb);
  char * text = reinterpret_cast<char *>(1);
  size_t n = 7;
  EXPECT_EQ(gtext_css_computed_shorthand_text(style.s, find(R(), "color"), &text, &n, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_computed_shorthand_text(style.s, 0, &text, &n, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_computed_shorthand_text(nullptr, find(R(), "margin"), &text, &n, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_computed_shorthand_text(style.s, find(R(), "margin"), nullptr, &n, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_computed_shorthand_text(style.s, find(R(), "margin"), &text, nullptr, nullptr), GTEXT_CSS_E_INVALID);
}

// Every shorthand of the registry, for the style a value of it makes: the text reads back to the same longhands, and the text of that is the same text.
TEST(CssComputedShorthand, EveryShorthandIsAFixedPoint) {
  const GTEXT_CSS_Registry * reg = R();
  const size_t count = gtext_css_registry_property_count(reg);
  size_t shorthands = 0, written = 0;
  for (size_t i = 1; i <= count; i++) {
    GTEXT_CSS_Property_Info info;
    if (!gtext_css_registry_property_info(reg, (GTEXT_CSS_Property_Id)i, &info) || !(info.flags & GTEXT_CSS_PROP_SHORTHAND)) {
      continue;
    }
    const std::string name(info.name, info.name_length);
    if (name == "all") {
      continue;
    }
    shorthands++;
    // the initial value of the shorthand (an empty declaration) is the style a value of it makes
    GTEXT_CSS_Status st = GTEXT_CSS_E_INTERNAL;
    const std::string text = shorthand_text("", name, &st);
    if (st == GTEXT_CSS_E_UNSUPPORTED) {
      continue;   // a longhand that the engine does not compute, or a shorthand with no rule
    }
    ASSERT_EQ(st, GTEXT_CSS_OK) << name;
    if (text.empty()) {
      continue;
    }
    written++;
    const std::string again = shorthand_text(name + ": " + text, name, &st);
    if (name.compare(0, 6, "corner") == 0 && again.compare(0, 6, "<style") == 0) {
      continue;   // the corner properties are not computed yet
    }
    ASSERT_EQ(st, GTEXT_CSS_OK) << name;
    EXPECT_EQ(again, text) << name << ": " << text;
  }
  EXPECT_GT(shorthands, 60u);
  EXPECT_GT(written, 40u);
}

// A value of each of a spread of shorthands: the text is a fixed point, and a style computed from it holds the same longhands.
TEST(CssComputedShorthand, ValuesReadBackToTheSameLonghands) {
  const std::vector<std::pair<const char *, const char *>> cases = {
      {"margin", "1px 2px 3px 4px"}, {"padding", "1px 2px"}, {"border", "2px dashed red"}, {"border-radius", "1px 2px / 3px 4px"}, {"flex", "1 2 10px"},
      {"font", "italic bold 12px/20px serif"}, {"background", "url(a) 10px 20px / cover no-repeat red"}, {"transition", "opacity 1s ease-in 2s, color 3s"},
      {"grid-area", "a / b / c / d"}, {"text-decoration", "underline wavy red"}, {"list-style", "square inside"}, {"outline", "1px solid"},
      {"gap", "1px 2px"}, {"inset", "1px 2px"}, {"overflow", "hidden auto"}, {"place-items", "center start"}, {"columns", "3 20px"},
      {"mask", "url(a) 10px 20px / 30px 40px no-repeat"}, {"animation", "a 1s ease-in 2s 3 reverse both paused"}, {"offset", "10px 20px auto 30deg"},
  };
  for (const auto & c : cases) {
    const std::string text = shorthand_text(std::string(c.first) + ": " + c.second, c.first);
    ASSERT_FALSE(text.empty()) << c.first << ": " << c.second;
    EXPECT_EQ(shorthand_text(std::string(c.first) + ": " + text, c.first), text) << c.first << ": " << text;
    Context ctx(R());
    Block a(R(), std::string(c.first) + ": " + c.second), b(R(), std::string(c.first) + ": " + text);
    Style sa, sb;
    sa.compute(ctx, &a);
    sb.compute(ctx, &b);
    EXPECT_EQ(sa.dump(), sb.dump()) << c.first << ": " << c.second << " -> " << text;
  }
}

// Borders 4 (served 2026-10-30), 3.9.3: "Value: <'corner-shape'> || <'border-radius'>", "Canonical order: per grammar". The shape is written first, a radius that is its
// initial value is left out, and a slash belongs to the radius (`<'border-radius'>` is `<length-percentage>{1,4} [ / <length-percentage>{1,4} ]?`). The
// shorthand once wrote nothing for a radius of two values and read the slash as the page's separator of groups.
TEST(CssComputedShorthand, CornerShorthandsAreAShapeAndARadius) {
  expect_all("corner", {
      {"4px 5% round", "superellipse(1) 4px 5%"},
      {"4px 8px notch", "superellipse(-infinity) 4px 8px"},
      {"4px / 3px", "superellipse(1) 4px / 3px"},
      {"4px 2% / 3px notch", "superellipse(-infinity) 4px 2% / 3px"},
      {"1px 2px 3px 4px / 5px notch squircle", "superellipse(-infinity) superellipse(2) 1px 2px 3px 4px / 5px"},
      {"30px superellipse(0.4)", "superellipse(0.4) 30px"},
      {"0px round", "superellipse(1)"},
      {"notch bevel", "superellipse(-infinity) superellipse(0)"},
  });
  // 3.9.1 and 3.9.2: `<'corner-top-left-shape'> || <'border-top-left-radius'>` and `<'corner-top-shape'> || <'border-top-radius'>`
  expect_all("corner-top-left", {{"4px 10% notch", "superellipse(-infinity) 4px 10%"}, {"round", "superellipse(1)"}, {"scoop 3px", "superellipse(-1) 3px"}});
  expect_all("corner-top", {{"4px / 3px bevel", "superellipse(0) 4px / 3px"}, {"4px 8px notch", "superellipse(-infinity) 4px 8px"}});
  expect_all("corner-block-start", {{"4px scoop", "superellipse(-1) 4px"}, {"squircle", "superellipse(2)"}});
}

// The longhands the shorthand sets: a radius of two values with a slash is one border-radius, whichever shape stands beside it.
TEST(CssComputedShorthand, CornerSetsTheRadiusAndTheShapeLonghands) {
  Context ctx(R());
  Block cb(R(), "corner: 4px 2% / 3px notch");
  Style style;
  style.compute(ctx, &cb);
  EXPECT_EQ(style.value("border-top-left-radius"), "4px 3px");
  EXPECT_EQ(style.value("border-top-right-radius"), "2% 3px");
  EXPECT_EQ(style.value("corner-top-left-shape"), "superellipse(-infinity)");
  EXPECT_EQ(style.value("corner-bottom-right-shape"), "superellipse(-infinity)");
}

}  // namespace
