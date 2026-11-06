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
 * The value of a property under animation at a time (css_style.h, GTEXT_CSS_Style_Animation), against Web Animations 1 ("The effect value of a keyframe effect", "The effect stack",
 * "Effect composition", "Applying the composited result"), CSS Animations 1 (keyframes, the timing function between keyframes, the order of the names) and CSS Animations 2
 * (`animation-composition`).
 */

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_compute_test_support.h"
#include "css_test_support.h"
#include "markup_test_support.h"

using namespace cmp;

namespace {

/// The `@keyframes` rules of a style sheet text, as a frozen set.
struct Frames {
  css_test::Parsed parsed;
  GTEXT_CSS_Atrule_Set * set = nullptr;
  explicit Frames(const std::string & css, const GTEXT_Allocator * alloc = nullptr) {
    GTEXT_CSS_Atrule_Set_Options o = gtext_css_atrule_set_options_default();
    o.allocator = alloc;
    EXPECT_EQ(gtext_css_atrule_set_new(R(), &o, &set), GTEXT_CSS_OK);
    css_test::parse_into(parsed, gtext_css_parse_stylesheet, css.c_str());
    EXPECT_EQ(gtext_css_atrule_set_add_tree(set, parsed.tree), GTEXT_CSS_OK);
    EXPECT_EQ(gtext_css_atrule_set_freeze(set), GTEXT_CSS_OK);
  }
  Frames(const Frames &) = delete;
  Frames & operator=(const Frames &) = delete;
  ~Frames() {
    gtext_css_atrule_set_free(set);
  }
};

/// The times a test gives: by the name of the animation. A name it does not list has none.
struct Clock {
  std::map<std::string, double> by_name;
  std::vector<std::string> asked;
  GTEXT_CSS_Status fail = GTEXT_CSS_OK;
  static GTEXT_CSS_Status call(void * user, GTEXT_CSS_Element_Handle, const GTEXT_CSS_Selector_List *, const GTEXT_CSS_Anim_Effect * e, double * ms, bool * known) {
    Clock * self = static_cast<Clock *>(user);
    const std::string name(e->name.data, e->name.length);
    self->asked.push_back(name);
    if (self->fail != GTEXT_CSS_OK) {
      return self->fail;
    }
    auto it = self->by_name.find(name);
    *known = it != self->by_name.end();
    if (*known) {
      *ms = it->second;
    }
    return GTEXT_CSS_OK;
  }
};

/// One element: its declarations, the keyframes and the clock.
/// One context for the elements of a test that are related (a style is made for its context; its child's must be too).
Context & shared_context() {
  static Context ctx(R());
  return ctx;
}

struct Sampled {
  Context & ctx = shared_context();
  Frames frames;
  Clock clock;
  GTEXT_CSS_Style_Animation animation{};
  GTEXT_CSS_Computed_Style * style = nullptr;
  GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;
  Sampled(const std::string & keyframes, const std::string & decls, std::map<std::string, double> times, const GTEXT_CSS_Computed_Style * parent = nullptr, bool with_clock = true)
      : frames(keyframes) {
    clock.by_name = std::move(times);
    animation.size = sizeof animation;
    animation.keyframes = frames.set;
    animation.user = &clock;
    animation.current_time = with_clock ? &Clock::call : nullptr;
    GTEXT_CSS_Style_Options o{};
    o.animation = &animation;
    GTEXT_CSS_Style_Entry entry;
    std::memset(&entry, 0, sizeof entry);
    entry.keys.origin = GTEXT_CSS_ORIGIN_AUTHOR;
    entry.text = {decls.data(), decls.size()};
    status = gtext_css_style_compute(ctx.c, nullptr, parent, &entry, 1, &o, &style, nullptr);
  }
  Sampled(const Sampled &) = delete;
  Sampled & operator=(const Sampled &) = delete;
  ~Sampled() {
    gtext_css_computed_free(style);
  }
  std::string value(const std::string & property) const {
    Style tmp;
    if (!style) {
      return "<no style>";
    }
    const GTEXT_CSS_Property_Id id = find(R(), property);
    if (property.rfind("--", 0) == 0) {
      const GTEXT_CSS_Custom_Value * cv = nullptr;
      if (!gtext_css_computed_custom_find(style, property.data(), property.size(), &cv)) {
        return "<unset>";
      }
      char * out = nullptr;
      size_t n = 0;
      gtext_css_custom_value_text(cv, nullptr, &out, &n);
      std::string text(out, n);
      gtext_css_serialize_buffer_free(nullptr, out);
      return text;
    }
    const GTEXT_CSS_Value * root = gtext_css_computed_root(style, id);
    if (!root) {
      return "<none>";
    }
    char * out = nullptr;
    size_t n = 0;
    if (gtext_css_serialize_value_buffer(root, nullptr, &out, &n) != GTEXT_CSS_OK) {
      return "<error>";
    }
    std::string text(out, n);
    gtext_css_serialize_buffer_free(nullptr, out);
    return text;
  }
};

const char * kGap = "@keyframes a { from { gap: 0px } to { gap: 100px } }";

/* The I/O matrix of the spec: `animation: a 10s linear -5s paused` with `@keyframes a {from {gap: 0px} to {gap: 100px}}` is 50px at the animation's time 0 (a paused animation
   holds the current time it was created with; with a delay of -5s that is an active time of 5s, half of 10s). */
TEST(CssAnimationSample, APausedAnimationWithANegativeDelayIsHalfWayThroughItsKeyframes) {
  Sampled s(kGap, "animation: a 10s linear -5s paused", {{"a", 0.0}});
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  EXPECT_EQ(s.value("row-gap"), "50px");
  EXPECT_EQ(s.value("column-gap"), "50px");
  EXPECT_EQ(s.clock.asked, std::vector<std::string>{"a"}) << "the clock is asked once for the one animation";
}

/* Web Animations 1, "Calculating the active time": the time is the local time of the animation; the effect value is found at the iteration progress. */
TEST(CssAnimationSample, TheTimeIsTheCallersAndTheValueIsAFunctionOfIt) {
  for (double ms : {0.0, 2500.0, 5000.0, 10000.0}) {
    Sampled s(kGap, "animation: a 10s linear both", {{"a", ms}});
    ASSERT_EQ(s.status, GTEXT_CSS_OK);
    EXPECT_EQ(s.value("row-gap"), std::to_string(static_cast<int>(ms / 100)) + "px") << ms;
  }
}

/* Spec I/O matrix: "no timeline callback: the underlying value". A callback that has no time for the animation, and an options struct with no callback, are the same. */
TEST(CssAnimationSample, WithoutATimeTheUnderlyingValueStands) {
  Sampled none(kGap, "animation: a 10s linear -5s paused", {}, nullptr, false);
  ASSERT_EQ(none.status, GTEXT_CSS_OK);
  EXPECT_EQ(none.value("row-gap"), "normal");
  Sampled unknown(kGap, "animation: a 10s linear -5s paused", {{"other", 3.0}});
  ASSERT_EQ(unknown.status, GTEXT_CSS_OK);
  EXPECT_EQ(unknown.value("row-gap"), "normal");
  /* and the style says that the property was not sampled, for lack of a time */
  EXPECT_TRUE(gtext_css_computed_animation_untimed(unknown.style, find(R(), "row-gap")));
  EXPECT_TRUE(gtext_css_computed_animation_untimed(unknown.style, find(R(), "column-gap")));
  EXPECT_FALSE(gtext_css_computed_animation_untimed(unknown.style, find(R(), "width")));
  Sampled timed(kGap, "animation: a 10s linear -5s paused", {{"a", 0.0}});
  EXPECT_FALSE(gtext_css_computed_animation_untimed(timed.style, find(R(), "row-gap")));
}

TEST(CssAnimationSample, ACallbackThatFailsFailsTheStyle) {
  Clock clock;
  clock.fail = GTEXT_CSS_E_UNSUPPORTED;
  Context ctx(R());
  Frames frames(kGap);
  GTEXT_CSS_Style_Animation an{};
  an.size = sizeof an;
  an.keyframes = frames.set;
  an.user = &clock;
  an.current_time = &Clock::call;
  GTEXT_CSS_Style_Options o{};
  o.animation = &an;
  GTEXT_CSS_Style_Entry entry;
  std::memset(&entry, 0, sizeof entry);
  entry.keys.origin = GTEXT_CSS_ORIGIN_AUTHOR;
  const char * decls = "animation: a 10s";
  entry.text = {decls, std::strlen(decls)};
  GTEXT_CSS_Computed_Style * s = nullptr;
  EXPECT_EQ(gtext_css_style_compute(ctx.c, nullptr, nullptr, &entry, 1, &o, &s, nullptr), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_EQ(s, nullptr);
}

/* Without the animation options a style is the underlying one, whatever the element names. */
TEST(CssAnimationSample, AnElementIsNotSampledWhenTheCallerAsksForNoAnimation) {
  Context ctx(R());
  GTEXT_CSS_Style_Entry entry;
  std::memset(&entry, 0, sizeof entry);
  entry.keys.origin = GTEXT_CSS_ORIGIN_AUTHOR;
  const char * decls = "animation: a 10s linear -5s paused";
  entry.text = {decls, std::strlen(decls)};
  GTEXT_CSS_Style_Options o{};
  GTEXT_CSS_Computed_Style * s = nullptr;
  ASSERT_EQ(gtext_css_style_compute(ctx.c, nullptr, nullptr, &entry, 1, &o, &s, nullptr), GTEXT_CSS_OK);
  EXPECT_FALSE(gtext_css_computed_animation_untimed(s, find(R(), "row-gap")));
  gtext_css_computed_free(s);
}

/* Web Animations 1, "interpolation": "discrete: p < 0.5 gives V_start, otherwise V_end" - the spec's matrix: a discrete property flips at 50 percent of the segment. */
TEST(CssAnimationSample, ADiscretePropertyFlipsAtTheMiddleOfTheSegment) {
  const char * kf = "@keyframes a { from { float: left } to { float: right } }";
  EXPECT_EQ(Sampled(kf, "animation: a 10s linear both", {{"a", 4999.0}}).value("float"), "left");
  EXPECT_EQ(Sampled(kf, "animation: a 10s linear both", {{"a", 5000.0}}).value("float"), "right");
  EXPECT_EQ(Sampled(kf, "animation: a 10s linear both", {{"a", 0.0}}).value("float"), "left");
}

/* Animations 1: "If a 0% or from keyframe is not specified, then the user agent constructs a 0% keyframe using the computed values of the properties being animated. If a 100% or
   to keyframe is not specified, then the user agent constructs a 100% keyframe". */
TEST(CssAnimationSample, AMissingEndKeyframeIsTheUnderlyingValue) {
  EXPECT_EQ(Sampled("@keyframes a { to { width: 100px } }", "width: 20px; animation: a 10s linear both", {{"a", 5000.0}}).value("width"), "60px");
  EXPECT_EQ(Sampled("@keyframes a { from { width: 100px } }", "width: 20px; animation: a 10s linear both", {{"a", 5000.0}}).value("width"), "60px");
  EXPECT_EQ(Sampled("@keyframes a { 50% { width: 100px } }", "width: 20px; animation: a 10s linear both", {{"a", 2500.0}}).value("width"), "60px");
  EXPECT_EQ(Sampled("@keyframes a { 50% { width: 100px } }", "width: 20px; animation: a 10s linear both", {{"a", 7500.0}}).value("width"), "60px");
}

/* Animations 1: "If a property is not specified for a keyframe ... the animation of that property proceeds as if that keyframe did not exist. Conceptually, it is as if a set of
   keyframes is constructed for each property that is present in any of the keyframes, and an animation is run independently for each property." */
TEST(CssAnimationSample, EachPropertyRunsItsOwnKeyframes) {
  const char * kf = "@keyframes a { 0% { width: 0px; height: 0px } 50% { height: 100px } 100% { width: 100px; height: 100px } }";
  Sampled s(kf, "animation: a 10s linear both", {{"a", 2500.0}});
  EXPECT_EQ(s.value("width"), "25px") << "width has the keyframes at 0% and 100%";
  EXPECT_EQ(s.value("height"), "50px") << "height has them at 0%, 50% and 100%";
}

/* Animations 1, "Timing functions for keyframes": "A keyframe style rule may also declare the timing function that is to be used as the animation moves to the next keyframe";
   "A timing function specified on the to or 100% keyframe is ignored"; without one the animation's own is used between each pair. */
TEST(CssAnimationSample, TheTimingFunctionIsAppliedBetweenKeyframes) {
  const char * kf = "@keyframes a { from { width: 0px; animation-timing-function: steps(2, jump-end) } to { width: 100px; animation-timing-function: ease-in } }";
  EXPECT_EQ(Sampled(kf, "animation: a 10s linear both", {{"a", 6000.0}}).value("width"), "50px") << "the keyframe's own steps(2)";
  const char * plain = "@keyframes a { from { width: 0px } to { width: 100px } }";
  EXPECT_EQ(Sampled(plain, "animation: a 10s steps(4, jump-end) both", {{"a", 6000.0}}).value("width"), "50px") << "the animation's function";
  EXPECT_EQ(Sampled(plain, "animation: a 10s step-end both", {{"a", 0.0}}).value("width"), "0px");
  EXPECT_EQ(Sampled(plain, "animation: a 10s step-end both", {{"a", 9999.0}}).value("width"), "0px");
}

/* Animations 1, animation-fill-mode and animation-delay: nothing is applied before the delay (without a backwards fill) or after the end (without a forwards fill). */
TEST(CssAnimationSample, TheFillModeDecidesTheValuesOutsideTheActiveInterval) {
  const char * kf = "@keyframes a { from { width: 10px } to { width: 90px } }";
  EXPECT_EQ(Sampled(kf, "width: 50px; animation: a 10s linear 2s", {{"a", 1000.0}}).value("width"), "50px");
  EXPECT_EQ(Sampled(kf, "width: 50px; animation: a 10s linear 2s backwards", {{"a", 1000.0}}).value("width"), "10px");
  EXPECT_EQ(Sampled(kf, "width: 50px; animation: a 10s linear 2s", {{"a", 20000.0}}).value("width"), "50px");
  EXPECT_EQ(Sampled(kf, "width: 50px; animation: a 10s linear 2s forwards", {{"a", 20000.0}}).value("width"), "90px");
  EXPECT_EQ(Sampled(kf, "width: 50px; animation: a 10s linear 2s both", {{"a", 20000.0}}).value("width"), "90px");
}

/* Animations 1: "If multiple animations are attempting to modify the same property, then the animation closest to the end of the list of names wins." */
TEST(CssAnimationSample, TheLastNameInTheListWins) {
  const char * kf = "@keyframes a { from { width: 0px } to { width: 100px } } @keyframes b { from { width: 0px } to { width: 50px } }";
  Sampled s(kf, "animation: a 10s linear both, b 10s linear both", {{"a", 5000.0}, {"b", 2000.0}});
  EXPECT_EQ(s.value("width"), "10px");
  Sampled r(kf, "animation: b 10s linear both, a 10s linear both", {{"a", 5000.0}, {"b", 2000.0}});
  EXPECT_EQ(r.value("width"), "50px");
  // an animation with no time gives way to the one that has
  Sampled one(kf, "animation: a 10s linear both, b 10s linear both", {{"a", 5000.0}});
  EXPECT_EQ(one.value("width"), "50px");
}

/* Animations 2 and Web Animations 1, "Effect composition": add is "underlying value + effect value"; replace is the effect value. */
TEST(CssAnimationSample, TheCompositeOperationCombinesTheEffectWithTheUnderlyingValue) {
  const char * kf = "@keyframes a { from { width: 0px } to { width: 100px } }";
  EXPECT_EQ(Sampled(kf, "width: 10px; animation: a 10s linear both", {{"a", 5000.0}}).value("width"), "50px");
  EXPECT_EQ(Sampled(kf, "width: 10px; animation: a 10s linear both; animation-composition: add", {{"a", 5000.0}}).value("width"), "60px");
  EXPECT_EQ(Sampled(kf, "width: 10px; animation: a 10s linear both; animation-composition: accumulate", {{"a", 5000.0}}).value("width"), "60px");
  // a second animation adds to what the first one made, not to the underlying value
  const char * two = "@keyframes a { from { width: 0px } to { width: 100px } } @keyframes b { from { width: 0px } to { width: 10px } }";
  EXPECT_EQ(Sampled(two, "width: 10px; animation: a 10s linear both, b 10s linear both; animation-composition: replace, add", {{"a", 5000.0}, {"b", 5000.0}}).value("width"), "55px");
  // a keyframe's own composite operation is the one for the property in that keyframe
  const char * own = "@keyframes a { from { width: 0px; animation-composition: add } to { width: 100px } }";
  EXPECT_EQ(Sampled(own, "width: 10px; animation: a 10s linear both", {{"a", 0.0}}).value("width"), "10px");
}

/* Web Animations 1, "Computing property values": the values of a keyframe are computed in the context of the element; em is the element's, `inherit` the parent's. */
TEST(CssAnimationSample, TheValuesOfAKeyframeAreComputedInTheContextOfTheElement) {
  const char * kf = "@keyframes a { from { column-width: 3em } to { column-width: 5em } }";
  EXPECT_EQ(Sampled(kf, "font-size: 10px; animation: a 4s linear -2s paused", {{"a", 0.0}}).value("column-width"), "40px");
  EXPECT_EQ(Sampled(kf, "font-size: 20px; animation: a 4s linear -2s paused", {{"a", 0.0}}).value("column-width"), "80px");
  // a calculation that is negative at the end is clamped at the end, before it is interpolated
  EXPECT_EQ(Sampled("@keyframes a { from { column-width: 40px } to { column-width: calc(40px - 2em) } }", "font-size: 90px; animation: a 4s linear -2s paused", {{"a", 0.0}}).value("column-width"),
      "20px");
  Sampled parent("", "column-width: 40px", {});
  EXPECT_EQ(Sampled("@keyframes a { from { column-width: 20px } to { column-width: inherit } }", "animation: a 4s linear -2s paused", {{"a", 0.0}}, parent.style).value("column-width"),
      "30px");
}

/* What an animation does to its element it does to what depends on it: a child inherits the animated value, and a custom property that a keyframe sets is the one `var()` reads. */
TEST(CssAnimationSample, WhatDependsOnAnAnimatedValueSeesIt) {
  const char * kf = "@keyframes a { from { font-size: 10px } to { font-size: 30px } }";
  Sampled parent(kf, "animation: a 10s linear both", {{"a", 5000.0}});
  ASSERT_EQ(parent.value("font-size"), "20px");
  Sampled child("", "width: 2em", {}, parent.style);
  EXPECT_EQ(child.value("font-size"), "20px");
  EXPECT_EQ(child.value("width"), "40px");
  const char * custom = "@keyframes a { 0% { --x: 10px } 100% { --x: 30px } } @keyframes g { from, to { --y: green } 0% { --unused: yellow } }";
  EXPECT_EQ(Sampled(custom, "--x: 5px; width: var(--x); animation: a 10s step-end both", {{"a", 0.0}}).value("width"), "10px");
  EXPECT_EQ(Sampled(custom, "--x: 5px; width: var(--x); animation: a 10s step-end both", {{"a", 9999.0}}).value("width"), "10px");
  EXPECT_EQ(Sampled(custom, "--x: 5px; width: var(--x); animation: a 10s step-end both", {{"a", 10000.0}}).value("width"), "30px");
  EXPECT_EQ(Sampled(custom, "--y: red; color: var(--y, red); animation: g 10s linear paused", {{"g", 0.0}}).value("color"), "green");
}

/* Cascade 5: the animation origin is above every normal declaration, and below !important. */
TEST(CssAnimationSample, AnAnimationOverridesNormalDeclarationsAndNotImportantOnes) {
  const char * kf = "@keyframes a { from { width: 0px; height: 0px } to { width: 100px; height: 100px } }";
  Sampled s(kf, "width: 10px; height: 10px !important; animation: a 10s linear both", {{"a", 5000.0}});
  EXPECT_EQ(s.value("width"), "50px");
  EXPECT_EQ(s.value("height"), "10px");
}

/* Colors: "premultiplied" interpolation in sRGB for the legacy syntaxes. */
TEST(CssAnimationSample, AColorIsInterpolatedInSrgb) {
  const char * kf = "@keyframes a { from { color: rgb(0, 100, 200) } to { color: rgb(100, 200, 0) } }";
  EXPECT_EQ(Sampled(kf, "animation: a 2s linear -1s paused", {{"a", 0.0}}).value("color"), "rgb(50, 150, 100)");
  EXPECT_EQ(Sampled("@keyframes a { from { color: rgba(0, 0, 0, 0) } to { color: rgb(100, 200, 0) } }", "animation: a 2s linear -1s paused", {{"a", 0.0}}).value("color"),
      "rgba(100, 200, 0, 0.5)") << "premultiplied: the color of the transparent end does not pull the middle";
}

/* Color 4: "Interpolating to or from currentcolor is possible. The numerical value used for this purpose is the used value." The keyframe's currentcolor is the element's color, and
   it follows the element's color when that changes. */
TEST(CssAnimationSample, ACurrentcolorKeyframeIsTheColorOfTheElement) {
  const char * kf = "@keyframes a { from { column-rule-color: currentColor } to { column-rule-color: rgb(0, 60, 0) } }";
  EXPECT_EQ(Sampled(kf, "color: rgb(60, 0, 0); animation: a 4s linear -2s paused", {{"a", 0.0}}).value("column-rule-color"), "rgb(30, 30, 0)");
  EXPECT_EQ(Sampled(kf, "color: rgb(0, 0, 60); animation: a 4s linear -2s paused", {{"a", 0.0}}).value("column-rule-color"), "rgb(0, 30, 30)");
  // `inherit` of a property whose value is currentcolor is currentcolor of the element that inherits it
  Sampled parent("", "color: rgb(80, 0, 0)", {});
  EXPECT_EQ(Sampled("@keyframes a { from { column-rule-color: inherit } to { column-rule-color: rgb(0, 0, 80) } }", "animation: a 4s linear -2s paused", {{"a", 0.0}}, parent.style)
                .value("column-rule-color"),
      "rgb(40, 0, 40)");
}

/* Values 4: a length and a percentage interpolate as a calculation. */
TEST(CssAnimationSample, ALengthAndAPercentageInterpolateAsACalculation) {
  const char * kf = "@keyframes a { from { margin-left: 10px } to { margin-left: 50% } }";
  EXPECT_EQ(Sampled(kf, "animation: a 2s linear -1s paused", {{"a", 0.0}}).value("margin-left"), "calc(25% + 5px)");
}

/* A list of the same shape is interpolated item by item; a value of another shape is discrete. */
TEST(CssAnimationSample, AListIsInterpolatedByItemAndAMismatchIsDiscrete) {
  EXPECT_EQ(Sampled("@keyframes a { from { text-shadow: rgb(0, 200, 0) 1px 2px 3px } to { text-shadow: rgb(100, 0, 200) 3px 2px 1px } }", "animation: a 2s linear -1s paused", {{"a", 0.0}})
                .value("text-shadow"),
      "rgb(50, 100, 100) 2px 2px 2px 0px");
  EXPECT_EQ(Sampled("@keyframes a { from { text-shadow: none } to { text-shadow: 1px 1px red } }", "animation: a 2s linear -1s paused", {{"a", 0.0}}).value("text-shadow"), "red 1px 1px 0px 0px") << "a list of another length is discrete";
}

/* Only a timeline of the document is sampled here: a scroll timeline is the caller's to supply. */
TEST(CssAnimationSample, AnAnimationOfAnotherTimelineIsNotSampled) {
  Sampled s(kGap, "animation: a 10s linear -5s paused; animation-timeline: scroll()", {{"a", 0.0}});
  EXPECT_EQ(s.value("row-gap"), "normal");
  EXPECT_TRUE(s.clock.asked.empty()) << "and its time is not asked for";
}

/* A name with no @keyframes rule is no animation; a name that is `none` is none. */
TEST(CssAnimationSample, ANameWithNoKeyframesIsNoAnimation) {
  Sampled s(kGap, "animation: nothing 10s linear -5s paused", {{"nothing", 0.0}});
  EXPECT_EQ(s.value("row-gap"), "normal");
  EXPECT_TRUE(s.clock.asked.empty());
}

/* Never animated: the animation properties themselves, and a property the registry does not animate. */
TEST(CssAnimationSample, AKeyframeCannotAnimateTheAnimationProperties) {
  Sampled s("@keyframes a { from { animation-duration: 1s; width: 0px } to { animation-duration: 99s; width: 10px } }", "animation: a 10s linear both", {{"a", 5000.0}});
  EXPECT_EQ(s.value("width"), "5px");
  EXPECT_EQ(s.value("animation-duration"), "10s");
}

/* ---- allocation failure ---------------------------------------------------------------------------------------------------------------------------- */

TEST(CssAnimationSampleOom, TheStyleOfAnAnimatedElementSweptOverEveryAllocation) {
  FaultAllocator fa;
  GTEXT_CSS_Compute_Options co = gtext_css_compute_options_default();
  co.allocator = &fa.alloc;
  Context ctx(R(), nullptr, &co);
  ASSERT_EQ(ctx.status, GTEXT_CSS_OK);
  Frames frames("@keyframes a { 0% { width: 0px; color: red; --x: 1px } 50% { width: 10px } 100% { width: 100px; color: blue; --x: 9px; animation-timing-function: ease } }"
                "@keyframes b { to { height: 50% } }");
  Clock clock;
  clock.by_name = {{"a", 3000.0}, {"b", 1000.0}};
  GTEXT_CSS_Style_Animation an{};
  an.size = sizeof an;
  an.keyframes = frames.set;
  an.user = &clock;
  an.current_time = &Clock::call;
  GTEXT_CSS_Style_Options o{};
  o.animation = &an;
  GTEXT_CSS_Style_Entry entry;
  std::memset(&entry, 0, sizeof entry);
  entry.keys.origin = GTEXT_CSS_ORIGIN_AUTHOR;
  const char * decls = "animation: a 10s linear both, b 10s ease both; animation-composition: replace, add; height: 20px; width: var(--x)";
  entry.text = {decls, std::strlen(decls)};
  {
    /* one run first: what the context keeps for later runs is not a leak of a failed one */
    GTEXT_CSS_Computed_Style * warm = nullptr;
    ASSERT_EQ(gtext_css_style_compute(ctx.c, nullptr, nullptr, &entry, 1, &o, &warm, nullptr), GTEXT_CSS_OK);
    gtext_css_computed_free(warm);
  }
  const size_t baseline = fa.live.size();
  size_t failures = 0, stride = 1;
  for (size_t n = 1; n < 200000; n += stride) {
    GTEXT_CSS_Computed_Style * s = nullptr;
    fa.arm(n);
    const GTEXT_CSS_Status st = gtext_css_style_compute(ctx.c, nullptr, nullptr, &entry, 1, &o, &s, nullptr);
    fa.disarm();
    if (!fa.failed) {
      EXPECT_EQ(st, GTEXT_CSS_OK);
      ASSERT_NE(s, nullptr);
      gtext_css_computed_free(s);
      EXPECT_EQ(fa.live.size(), baseline) << "a block is left";
      break;
    }
    failures++;
    EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
    EXPECT_EQ(s, nullptr) << n;
    EXPECT_EQ(fa.live.size(), baseline) << n << ": a block is left";
    EXPECT_EQ(fa.bad_frees, 0u) << n;
    if (n > 600) {
      stride = 5;
    }
  }
  EXPECT_GT(failures, 50u) << "the sweep reached the allocations of the sampler";
}

}  // namespace
