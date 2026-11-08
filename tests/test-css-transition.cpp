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
 * CSS Transitions 1 and 2 (css_transition.h): starting, reversing, retargeting, cancelling and completing transitions on a style change event, and the current value of a running
 * transition in the style (GTEXT_CSS_Style_Options::transitions). Each test quotes the sentence of the pinned drafts it checks.
 */

#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_compute_test_support.h"
#include "css_test_support.h"
#include "markup_test_support.h"

using namespace cmp;

namespace {

Context & shared() {
  static Context ctx(R());
  return ctx;
}

/// The `@keyframes` rules of a style sheet text, as a frozen set, and a clock that gives every animation one time.
struct Frames {
  css_test::Parsed parsed;
  GTEXT_CSS_Atrule_Set * set = nullptr;
  double now = 0;
  explicit Frames(const std::string & css) {
    GTEXT_CSS_Atrule_Set_Options o = gtext_css_atrule_set_options_default();
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
  static GTEXT_CSS_Status clock(void * user, GTEXT_CSS_Element_Handle, const GTEXT_CSS_Selector_List *, const GTEXT_CSS_Anim_Effect *, double * ms, bool * known) {
    *ms = static_cast<Frames *>(user)->now;
    *known = true;
    return GTEXT_CSS_OK;
  }
};

/// A style from author declarations, with optional transitions.
GTEXT_CSS_Computed_Style * make_style(Context & ctx, const std::string & decls, const GTEXT_CSS_Transition_Set * set = nullptr, double time_ms = 0, const GTEXT_CSS_Computed_Style * parent = nullptr,
    GTEXT_CSS_Status * status = nullptr, Frames * frames = nullptr) {
  GTEXT_CSS_Style_Transitions tr{};
  tr.size = sizeof tr;
  tr.set = set;
  tr.time_ms = time_ms;
  GTEXT_CSS_Style_Animation an{};
  an.size = sizeof an;
  GTEXT_CSS_Style_Options o{};
  o.transitions = set ? &tr : nullptr;
  if (frames) {
    an.keyframes = frames->set;
    an.user = frames;
    an.current_time = &Frames::clock;
    o.animation = &an;
  }
  GTEXT_CSS_Style_Entry e;
  std::memset(&e, 0, sizeof e);
  e.keys.origin = GTEXT_CSS_ORIGIN_AUTHOR;
  e.text = {decls.data(), decls.size()};
  GTEXT_CSS_Computed_Style * s = nullptr;
  const GTEXT_CSS_Status st = gtext_css_style_compute(ctx.c, nullptr, parent, &e, 1, &o, &s, nullptr);
  if (status) {
    *status = st;
  }
  return s;
}

std::string val(const GTEXT_CSS_Computed_Style * style, const std::string & property) {
  const GTEXT_CSS_Value * root = gtext_css_computed_root(style, find(R(), property));
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

struct Tx {
  Context & ctx = shared();
  GTEXT_CSS_Transition_Set * set = nullptr;
  GTEXT_CSS_Transition_Update up{};
  Tx() {
    EXPECT_EQ(gtext_css_transition_set_new(nullptr, &set), GTEXT_CSS_OK);
  }
  Tx(const Tx &) = delete;
  Tx & operator=(const Tx &) = delete;
  ~Tx() {
    gtext_css_transition_set_free(set);
  }
  /// One style change event: the before-change and after-change declarations at a time.
  GTEXT_CSS_Status event(const std::string & before, const std::string & after, double t) {
    GTEXT_CSS_Computed_Style * b = before.empty() ? nullptr : make_style(ctx, before);
    GTEXT_CSS_Computed_Style * a = make_style(ctx, after);
    const GTEXT_CSS_Status st = gtext_css_transitions_update(ctx.c, set, b, a, t, &up);
    gtext_css_computed_free(a);
    gtext_css_computed_free(b);
    return st;
  }
  /// The style the caller shows at a time.
  std::string shown(const std::string & decls, const std::string & property, double t) {
    GTEXT_CSS_Status status = GTEXT_CSS_OK;
    GTEXT_CSS_Computed_Style * s = make_style(ctx, decls, set, t, nullptr, &status);
    const std::string v = s ? val(s, property) : "<no style: status " + std::to_string(static_cast<int>(status)) + ">";
    gtext_css_computed_free(s);
    return v;
  }
  size_t running() const {
    return gtext_css_transition_set_running_count(set);
  }
  GTEXT_CSS_Transition_Info info(size_t i = 0) const {
    GTEXT_CSS_Transition_Info t{};
    EXPECT_TRUE(gtext_css_transition_set_running_at(set, i, &t));
    return t;
  }
};

const std::string kOpacity = "transition: opacity 1s linear; ";

/* "then implementations must ... start a transition whose: start time is the time of the style change event plus the matching transition delay, end time is the start time plus the
   matching transition duration, start value is the value of the transitioning property in the before-change style, end value ... in the after-change style". */
TEST(CssTransition, AChangedPropertyWithAMatchingTransitionStartsOne) {
  Tx t;
  ASSERT_EQ(t.event("opacity: 0", kOpacity + "opacity: 1", 1000), GTEXT_CSS_OK);
  EXPECT_EQ(t.up.started, 1u);
  ASSERT_EQ(t.running(), 1u);
  const GTEXT_CSS_Transition_Info i = t.info();
  EXPECT_EQ(i.start_time_ms, 1000);
  EXPECT_EQ(i.end_time_ms, 2000);
  EXPECT_EQ(std::string(i.start_value.data, i.start_value.length), "0");
  EXPECT_EQ(std::string(i.end_value.data, i.end_value.length), "1");
  EXPECT_EQ(i.reversing_shortening_factor, 1.0);
  EXPECT_EQ(std::string(i.reversing_adjusted_start_value.data, i.reversing_adjusted_start_value.length), "0");
}

/* "If the current time is at or before the start time of the transition (that is, during the delay phase of the transition), the current value is a specified style that will compute to
   the start value of the transition. If the current time is after the start time ... the result of interpolating the property using ... p: the output of the timing function for input
   (current time - start time) / (end time - start time)". */
TEST(CssTransition, TheCurrentValueIsTheStartValueInTheDelayAndTheInterpolationAfterIt) {
  Tx t;
  const std::string after = "transition: opacity 1s linear 500ms; opacity: 1";
  ASSERT_EQ(t.event("opacity: 0", after, 1000), GTEXT_CSS_OK);
  EXPECT_EQ(t.info().start_time_ms, 1500);
  EXPECT_EQ(t.shown(after, "opacity", 1000), "0");
  EXPECT_EQ(t.shown(after, "opacity", 1500), "0");
  EXPECT_EQ(t.shown(after, "opacity", 1750), "0.25");
  EXPECT_EQ(t.shown(after, "opacity", 2000), "0.5");
  EXPECT_EQ(t.shown(after, "opacity", 2499), "0.999");
  EXPECT_EQ(t.shown(after, "opacity", 2500), "1") << "at the end time the transition no longer applies, and the underlying value shows";
}

/* "Define the combined duration of the transition as the sum of max(matching transition duration, 0s) and the matching transition delay": a combined duration that is not greater than 0s
   starts nothing, and a negative delay starts a transition that is part way through. */
TEST(CssTransition, TheCombinedDurationDecidesAndANegativeDelayStartsPartWayThrough) {
  Tx zero;
  ASSERT_EQ(zero.event("opacity: 0", "transition: opacity 0s; opacity: 1", 0), GTEXT_CSS_OK);
  EXPECT_EQ(zero.running(), 0u);
  Tx negative;
  ASSERT_EQ(negative.event("opacity: 0", "transition: opacity 1s -2s; opacity: 1", 0), GTEXT_CSS_OK) << "-2s + 1s is not greater than 0s";
  EXPECT_EQ(negative.running(), 0u);
  Tx half;
  const std::string after = "transition: opacity 2s linear -1s; opacity: 1";
  ASSERT_EQ(half.event("opacity: 0", after, 0), GTEXT_CSS_OK);
  ASSERT_EQ(half.running(), 1u);
  EXPECT_EQ(half.info().start_time_ms, -1000);
  EXPECT_EQ(half.shown(after, "opacity", 0), "0.5");
}

/* The transition does not apply at or after its end time: a style that no longer says what it ended on shows its own value. */
TEST(CssTransition, ATransitionPastItsEndTimeIsNotInTheCascade) {
  Tx t;
  const std::string a = "transition: opacity 1s linear; opacity: 1";
  ASSERT_EQ(t.event("opacity: 0", a, 0), GTEXT_CSS_OK);
  EXPECT_EQ(t.shown("transition: opacity 1s linear; opacity: 0.2", "opacity", 999), "0.999");
  EXPECT_EQ(t.shown("transition: opacity 1s linear; opacity: 0.2", "opacity", 1000), "0.2");
  EXPECT_EQ(t.shown("transition: opacity 1s linear; opacity: 0.2", "opacity", 5000), "0.2");
}

/* The reversing shortening factor is "the absolute value, clamped to the range [0, 1]": a timing function that overshoots makes the sum larger than 1, and the factor stays 1. */
TEST(CssTransition, TheShorteningFactorOfAnOvershootingTimingFunctionIsClamped) {
  Tx t;
  const std::string out = "transition: opacity 1s cubic-bezier(0, 2, 1, 2); opacity: 1";
  const std::string back = "transition: opacity 1s cubic-bezier(0, 2, 1, 2); opacity: 0";
  ASSERT_EQ(t.event("opacity: 0", out, 0), GTEXT_CSS_OK);
  ASSERT_EQ(t.event(out, back, 500), GTEXT_CSS_OK);
  ASSERT_EQ(t.running(), 1u);
  EXPECT_EQ(t.info().reversing_shortening_factor, 1.0) << "the output of the timing function at half way is 1.625";
  EXPECT_EQ(t.info().end_time_ms, 1500);
}

/* ... "the absolute value": a timing function that undershoots makes the sum negative at half way (-0.625), and the factor is its absolute value. */
TEST(CssTransition, TheShorteningFactorIsTheAbsoluteValueOfTheSum) {
  Tx t;
  const std::string out = "transition: opacity 1s cubic-bezier(0, -1, 1, -1); opacity: 1";
  const std::string back = "transition: opacity 1s cubic-bezier(0, -1, 1, -1); opacity: 0";
  ASSERT_EQ(t.event("opacity: 0", out, 0), GTEXT_CSS_OK);
  ASSERT_EQ(t.event(out, back, 500), GTEXT_CSS_OK);
  ASSERT_EQ(t.running(), 1u);
  EXPECT_EQ(t.info().reversing_shortening_factor, 0.625);
  EXPECT_EQ(t.info().end_time_ms, 500 + 625);
}

/* "start time is the time of the style change event plus: if the matching transition delay is nonnegative, the matching transition delay, or if the matching transition delay is negative,
   the product of the new transition's reversing shortening factor and the matching transition delay". */
TEST(CssTransition, AReversalScalesANegativeDelayByTheShorteningFactor) {
  Tx t;
  const std::string out = "transition: opacity 1s linear -200ms; opacity: 1";
  const std::string back = "transition: opacity 1s linear -200ms; opacity: 0";
  ASSERT_EQ(t.event("opacity: 0", out, 0), GTEXT_CSS_OK);
  EXPECT_EQ(t.info().start_time_ms, -200);
  ASSERT_EQ(t.event(out, back, 250), GTEXT_CSS_OK);
  ASSERT_EQ(t.running(), 1u);
  const GTEXT_CSS_Transition_Info i = t.info();
  EXPECT_EQ(i.reversing_shortening_factor, 0.45) << "(250 - -200) / 1000";
  EXPECT_NEAR(i.start_time_ms, 250 + 0.45 * -200, 1e-9);
  EXPECT_NEAR(i.end_time_ms, i.start_time_ms + 450, 1e-9);
}

/* "If there is such a value": no matching transition-property value, no transition; the before-change style absent ("If an element is not in the document during that style change event
   or was not in the document during the previous style change event, then transitions are not started"); the same value, no change. */
TEST(CssTransition, NothingStartsWithoutAMatchABeforeStyleOrAChange) {
  Tx a;
  ASSERT_EQ(a.event("opacity: 0", "transition: color 1s; opacity: 1", 0), GTEXT_CSS_OK);
  EXPECT_EQ(a.running(), 0u);
  Tx b;
  ASSERT_EQ(b.event("", kOpacity + "opacity: 1", 0), GTEXT_CSS_OK);
  EXPECT_EQ(b.running(), 0u);
  Tx c;
  ASSERT_EQ(c.event("opacity: 1", kOpacity + "opacity: 1", 0), GTEXT_CSS_OK);
  EXPECT_EQ(c.running(), 0u);
  Tx n;
  ASSERT_EQ(n.event("opacity: 0", "transition: none; opacity: 1", 0), GTEXT_CSS_OK);
  EXPECT_EQ(n.running(), 0u);
}

/* "the last value in the transition-property in the element's after-change style that matches the property": `all` matches everything, a shorthand name its longhands, and the last item
   decides the duration (the lists are matched up from the first value). */
TEST(CssTransition, TheLastMatchingValueSuppliesTheDurationAndAShorthandMatchesItsLonghands) {
  Tx t;
  ASSERT_EQ(t.event("opacity: 0; margin-top: 0px; margin-left: 0px", "transition-property: opacity, all, margin; transition-duration: 1s, 2s, 4s; opacity: 1; margin-top: 10px; margin-left: 10px", 0), GTEXT_CSS_OK);
  ASSERT_EQ(t.running(), 3u);
  for (size_t i = 0; i < t.running(); i++) {
    const GTEXT_CSS_Transition_Info info = t.info(i);
    const bool margin = info.property != find(R(), "opacity");
    EXPECT_EQ(info.end_time_ms, margin ? 4000 : 2000) << i;
  }
}

/* "If one of the other properties doesn't have enough comma-separated values to match the number of values of 'transition-property', the user agent must calculate its used value by
   repeating the list of values until there are enough." */
TEST(CssTransition, ShortListsAreRepeated) {
  Tx t;
  ASSERT_EQ(t.event("opacity: 0; margin-top: 0px; margin-left: 0px; margin-right: 0px", "transition-property: opacity, margin-top, margin-left; transition-duration: 1s, 3s; opacity: 1; margin-top: 10px; margin-left: 10px; margin-right: 10px", 0), GTEXT_CSS_OK);
  ASSERT_EQ(t.running(), 3u);
  std::vector<double> ends;
  for (size_t i = 0; i < 3; i++) {
    ends.push_back(t.info(i).end_time_ms);
  }
  EXPECT_EQ(ends, (std::vector<double>{1000, 3000, 1000}));
}

/* "Running transitions complete at a time that is equal to or after their end time ... move all transitions that complete at that time from the set of running transitions to the set of
   completed transitions" and "the element does not have a completed transition for the property or the end value of the completed transition is different from the after-change style": a
   completed transition stops the same change from starting another. */
TEST(CssTransition, ACompletedTransitionIsKeptSoThatTheSameValueDoesNotRestartIt) {
  Tx t;
  const std::string after = kOpacity + "opacity: 1";
  ASSERT_EQ(t.event("opacity: 0", after, 0), GTEXT_CSS_OK);
  ASSERT_EQ(t.event(after, after, 1000), GTEXT_CSS_OK);
  EXPECT_EQ(t.up.completed, 1u);
  EXPECT_EQ(t.running(), 0u);
  ASSERT_EQ(gtext_css_transition_set_completed_count(t.set), 1u);
  GTEXT_CSS_Property_Id p = 0;
  GTEXT_CSS_String end{};
  ASSERT_TRUE(gtext_css_transition_set_completed_at(t.set, 0, &p, &end));
  EXPECT_EQ(p, find(R(), "opacity"));
  EXPECT_EQ(std::string(end.data, end.length), "1");
  /* a before-change style that differs from the after-change one and a completed transition with the same end value: no new transition */
  ASSERT_EQ(t.event("opacity: 0", after, 1100), GTEXT_CSS_OK);
  EXPECT_EQ(t.running(), 0u);
  /* a different value forgets the completed transition and starts one */
  ASSERT_EQ(t.event(after, kOpacity + "opacity: 0.5", 1200), GTEXT_CSS_OK);
  EXPECT_EQ(t.running(), 1u);
  EXPECT_EQ(gtext_css_transition_set_completed_count(t.set), 0u);
}

/* "If the element has a running transition or completed transition for the property, and there is not a matching transition-property value, then implementations must cancel the
   running transition or remove the completed transition". */
TEST(CssTransition, ARunningTransitionIsCancelledWhenNothingMatchesAnyMore) {
  Tx t;
  const std::string after = kOpacity + "opacity: 1";
  ASSERT_EQ(t.event("opacity: 0", after, 0), GTEXT_CSS_OK);
  ASSERT_EQ(t.running(), 1u);
  ASSERT_EQ(t.event(after, "transition: none; opacity: 1", 500), GTEXT_CSS_OK);
  EXPECT_EQ(t.up.cancelled, 1u);
  EXPECT_EQ(t.running(), 0u);
  EXPECT_EQ(t.shown("transition: none; opacity: 1", "opacity", 500), "1");
}

/* "Otherwise, implementations must cancel the running transition and start a new transition whose: start time is the time of the style change event plus the matching transition delay,
   end time is the start time plus the matching transition duration, start value is the current value of the property in the running transition, end value is the value of the property in
   the after-change style, reversing-adjusted start value is the same as the start value, and reversing shortening factor is 1." */
TEST(CssTransition, ANewTargetCancelsTheRunningTransitionAndStartsFromItsCurrentValue) {
  Tx t;
  const std::string a = kOpacity + "opacity: 1";
  ASSERT_EQ(t.event("opacity: 0", a, 0), GTEXT_CSS_OK);
  const std::string b = kOpacity + "opacity: 0.8";
  ASSERT_EQ(t.event(a, b, 500), GTEXT_CSS_OK);
  EXPECT_EQ(t.up.cancelled, 1u);
  EXPECT_EQ(t.up.started, 1u);
  ASSERT_EQ(t.running(), 1u);
  const GTEXT_CSS_Transition_Info i = t.info();
  EXPECT_EQ(std::string(i.start_value.data, i.start_value.length), "0.5");
  EXPECT_EQ(std::string(i.end_value.data, i.end_value.length), "0.8");
  EXPECT_EQ(i.start_time_ms, 500);
  EXPECT_EQ(i.end_time_ms, 1500);
  EXPECT_EQ(i.reversing_shortening_factor, 1.0);
  EXPECT_EQ(std::string(i.reversing_adjusted_start_value.data, i.reversing_adjusted_start_value.length), "0.5");
  EXPECT_EQ(t.shown(b, "opacity", 1000), "0.65");
}

/* "Otherwise, if the reversing-adjusted start value of the running transition is the same as the value of the property in the after-change style ... cancel the running transition and
   start a new transition whose: reversing-adjusted start value is the end value of the running transition, reversing shortening factor is the absolute value, clamped to the range [0, 1],
   of the sum of: the output of the timing function of the old transition at the time of the style change event, times the reversing shortening factor of the old transition [and] 1 minus the
   reversing shortening factor of the old transition ... end time is the start time plus the product of the matching transition duration and the new transition's reversing shortening
   factor, start value is the current value of the property in the running transition". */
TEST(CssTransition, ReversingAQuarterWayShortensTheNewTransitionToAQuarter) {
  Tx t;
  const std::string out = kOpacity + "opacity: 1";
  const std::string back = kOpacity + "opacity: 0";
  ASSERT_EQ(t.event("opacity: 0", out, 0), GTEXT_CSS_OK);
  ASSERT_EQ(t.event(out, back, 250), GTEXT_CSS_OK);
  ASSERT_EQ(t.running(), 1u);
  GTEXT_CSS_Transition_Info i = t.info();
  EXPECT_EQ(i.reversing_shortening_factor, 0.25);
  EXPECT_EQ(i.start_time_ms, 250);
  EXPECT_EQ(i.end_time_ms, 500);
  EXPECT_EQ(std::string(i.start_value.data, i.start_value.length), "0.25");
  EXPECT_EQ(std::string(i.end_value.data, i.end_value.length), "0");
  EXPECT_EQ(std::string(i.reversing_adjusted_start_value.data, i.reversing_adjusted_start_value.length), "1");
  EXPECT_EQ(t.shown(back, "opacity", 375), "0.125");
  /* and reversing again, half way: shortening factor |0.5 * 0.25 + (1 - 0.25)| = 0.875 */
  ASSERT_EQ(t.event(back, out, 375), GTEXT_CSS_OK);
  i = t.info();
  EXPECT_EQ(i.reversing_shortening_factor, 0.875);
  EXPECT_EQ(i.end_time_ms, 375 + 875);
}

/* "if the combined duration is less than or equal to 0s ... then implementations must cancel the running transition": the running transition is cancelled, and none starts. */
TEST(CssTransition, ARunningTransitionIsCancelledWhenTheNewDurationIsZero) {
  Tx t;
  ASSERT_EQ(t.event("opacity: 0", kOpacity + "opacity: 1", 0), GTEXT_CSS_OK);
  ASSERT_EQ(t.event(kOpacity + "opacity: 1", "transition: opacity 0s; opacity: 0.2", 100), GTEXT_CSS_OK);
  EXPECT_EQ(t.running(), 0u);
  EXPECT_EQ(t.up.cancelled, 1u);
  EXPECT_EQ(t.up.started, 0u);
}

/* "(CSS Cascade) Transition declarations; Important user agent declarations; ..." the current value is above an important author declaration, and is the value of the element that its
   descendants inherit. */
TEST(CssTransition, TheCurrentValueBeatsAnImportantDeclarationAndIsInherited) {
  Tx t;
  const std::string a = "transition: font-size 1s linear; font-size: 30px !important";
  ASSERT_EQ(t.event("font-size: 10px", a, 0), GTEXT_CSS_OK);
  ASSERT_EQ(t.running(), 1u);
  GTEXT_CSS_Computed_Style * parent = make_style(t.ctx, a, t.set, 500);
  ASSERT_NE(parent, nullptr);
  EXPECT_EQ(val(parent, "font-size"), "20px");
  GTEXT_CSS_Computed_Style * child = make_style(t.ctx, "", nullptr, 0, parent);
  ASSERT_NE(child, nullptr);
  EXPECT_EQ(val(child, "font-size"), "20px");
  gtext_css_computed_free(child);
  gtext_css_computed_free(parent);
}

/* "the property values are transitionable if they have an animation type that is neither not animatable nor discrete": visibility is discrete, so no transition starts; with
   transition-behavior: allow-discrete (Transitions 2) one does and the value changes half way. */
TEST(CssTransition, ADiscretePropertyTransitionsOnlyWithAllowDiscrete) {
  Tx plain;
  ASSERT_EQ(plain.event("visibility: hidden", "transition: visibility 1s linear; visibility: visible", 0), GTEXT_CSS_OK);
  EXPECT_EQ(plain.running(), 0u);
  Tx t;
  const std::string a = "transition: visibility 1s linear; transition-behavior: allow-discrete; visibility: visible";
  ASSERT_EQ(t.event("visibility: hidden", a, 0), GTEXT_CSS_OK);
  ASSERT_EQ(t.running(), 1u);
  EXPECT_TRUE(t.info().allow_discrete);
  EXPECT_EQ(t.shown(a, "visibility", 100), "hidden");
  EXPECT_EQ(t.shown(a, "visibility", 600), "visible");
}

/* "Note that above rules mean that transitions do not start when the computed value of a property changes as a result of declarative animation (as opposed to scripted animation). This
   happens because the before-change style includes up-to-date style for declarative animations." The before-change style here was sampled at an earlier time than the after-change style, so the
   animated value moved between them: the property an animation applies starts no transition. */
TEST(CssTransition, APropertyAnAnimationAppliesDoesNotStartATransition) {
  Frames frames("@keyframes a { from { opacity: 0 } to { opacity: 0.5 } }");
  Tx t;
  const std::string decls = "animation: a 10s linear both; transition: opacity 1s linear; opacity: 1";
  frames.now = 0;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  GTEXT_CSS_Computed_Style * before = make_style(t.ctx, decls, nullptr, 0, nullptr, &st, &frames);
  frames.now = 5000;
  GTEXT_CSS_Computed_Style * after = make_style(t.ctx, decls, nullptr, 0, nullptr, &st, &frames);
  ASSERT_NE(before, nullptr);
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(val(before, "opacity"), "0");
  EXPECT_EQ(val(after, "opacity"), "0.25");
  EXPECT_TRUE(gtext_css_computed_animation_applied(after, find(R(), "opacity")));
  EXPECT_FALSE(gtext_css_computed_animation_applied(after, find(R(), "width")));
  ASSERT_EQ(gtext_css_transitions_update(t.ctx.c, t.set, before, after, 5000, &t.up), GTEXT_CSS_OK);
  EXPECT_EQ(t.running(), 0u);
  gtext_css_computed_free(before);
  gtext_css_computed_free(after);
}

/* "Implementations must add this value to the cascade if and only if that property is not currently undergoing a CSS Animation ([CSS3-ANIMATIONS]) on the same element." */
TEST(CssTransition, ARunningTransitionIsNotInTheCascadeWhileAnAnimationAppliesTheProperty) {
  Frames frames("@keyframes a { from { opacity: 0.8 } to { opacity: 0.8 } }");
  Tx t;
  const std::string plain = "transition: opacity 1s linear; opacity: 1";
  ASSERT_EQ(t.event("opacity: 0", plain, 0), GTEXT_CSS_OK);
  ASSERT_EQ(t.running(), 1u);
  EXPECT_EQ(t.shown(plain, "opacity", 500), "0.5");
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  GTEXT_CSS_Computed_Style * animated = make_style(t.ctx, plain + "; animation: a 10s linear both", t.set, 500, nullptr, &st, &frames);
  ASSERT_NE(animated, nullptr);
  EXPECT_EQ(val(animated, "opacity"), "0.8") << "the animation applies the property: the transition is still running, and is not in the cascade";
  gtext_css_computed_free(animated);
}

/* "p: the output of the timing function for input (current time - start time) / (end time - start time)": a step function is not the identity. */
TEST(CssTransition, TheTimingFunctionShapesTheProgress) {
  Tx t;
  const std::string a = "transition: opacity 1s steps(4, jump-end); opacity: 1";
  ASSERT_EQ(t.event("opacity: 0", a, 0), GTEXT_CSS_OK);
  EXPECT_EQ(t.shown(a, "opacity", 300), "0.25");
  EXPECT_EQ(t.shown(a, "opacity", 600), "0.5");
  EXPECT_EQ(t.shown(a, "opacity", 999), "0.75");
}

/* CSS Values 4, "Interpolation": the result is clamped to the allowed range. A timing function that overshoots (here -60 after the 60% mark) takes a radius below zero, and the page
   basic-shape-interpolation.html expects the radius clamped to zero: `circle(0px)`, `ellipse(0px 0px)` and `inset(10%)` (a zero corner radius is not written). */
TEST(CssTransition, ARadiusThatANegativeEasingTakesBelowZeroIsClampedToZero) {
  struct Case {
    const char * from;
    const char * to;
    const char * want;
  };
  for (const Case & c : {Case{"circle(25px)", "circle(26px)", "circle(0px)"}, Case{"ellipse(25px 25px)", "ellipse(26px 26px)", "ellipse(0px 0px)"},
                          Case{"inset(10% round 25px)", "inset(10% round 26px)", "inset(10%)"}}) {
    Tx t;
    const std::string after = std::string("transition: all 100s cubic-bezier(0, 0, 1, -60) -61s; shape-outside: ") + c.to;
    ASSERT_EQ(t.event(std::string("shape-outside: ") + c.from, after, 0), GTEXT_CSS_OK) << c.from;
    ASSERT_EQ(t.running(), 1u) << c.from;
    EXPECT_EQ(t.shown(after, "shape-outside", 0), c.want) << c.from;
  }
}

/* The offsets of inset() and the position of circle() after `at` may be negative: only the radii are clamped. */
TEST(CssTransition, OnlyTheRadiiOfAShapeAreClamped) {
  Tx t;
  const std::string after = "transition: all 100s linear; shape-outside: circle(10px at -20px 5px)";
  ASSERT_EQ(t.event("shape-outside: circle(10px at 20px 5px)", after, 0), GTEXT_CSS_OK);
  EXPECT_EQ(t.shown(after, "shape-outside", 50000), "circle(10px at 0px 5px)");
  EXPECT_EQ(t.shown(after, "shape-outside", 75000), "circle(10px at -10px 5px)");
}

/* The same for the offsets that precede `round` in inset(), rect() and xywh() (the last two compute to inset(), the left offset of xywh(-20px ...) being the one that moves): they are lengths that may be negative, and only the radii after `round` are clamped. */
TEST(CssTransition, TheOffsetsBeforeRoundOfAnInsetAreNotClamped) {
  struct Case {
    const char * from;
    const char * to;
    const char * want;
  };
  for (const Case & c : {Case{"inset(10px)", "inset(-20px)", "inset(-5px)"}, Case{"inset(10px round 8px)", "inset(-20px round 8px)", "inset(-5px round 8px)"},
                          Case{"xywh(10px 10px 50px 50px)", "xywh(-20px 10px 50px 50px)", "inset(10px calc(100% - 45px) calc(100% - 60px) -5px)"}}) {
    Tx t;
    const std::string after = std::string("transition: all 100s linear; shape-outside: ") + c.to;
    ASSERT_EQ(t.event(std::string("shape-outside: ") + c.from, after, 0), GTEXT_CSS_OK) << c.from;
    EXPECT_EQ(t.shown(after, "shape-outside", 50000), c.want) << c.from;
  }
}

/* A flow-relative name in `transition-property` stands for the physical property it shares its value with, and the flow-relative property is not transitioned on its own (CSS Logical 1). */
TEST(CssTransition, AFlowRelativePropertyTransitionsAsItsPhysicalOne) {
  Tx t;
  const std::string a = "transition: margin-inline-start 1s linear; margin-left: 10px";
  ASSERT_EQ(t.event("margin-left: 0px", a, 0), GTEXT_CSS_OK);
  ASSERT_EQ(t.running(), 1u);
  EXPECT_EQ(t.info().property, find(R(), "margin-left"));
  EXPECT_EQ(t.shown(a, "margin-left", 500), "5px");
}

/* Completion at the event time: a running transition whose end time has passed completes before the other rules are applied. */
TEST(CssTransition, ATransitionCompletesAtTheFirstEventAtOrAfterItsEndTime) {
  Tx t;
  const std::string a = kOpacity + "opacity: 1";
  ASSERT_EQ(t.event("opacity: 0", a, 0), GTEXT_CSS_OK);
  ASSERT_EQ(t.event(a, a, 999), GTEXT_CSS_OK);
  EXPECT_EQ(t.running(), 1u);
  ASSERT_EQ(t.event(a, a, 1000), GTEXT_CSS_OK);
  EXPECT_EQ(t.running(), 0u);
  EXPECT_EQ(t.up.completed, 1u);
}

TEST(CssTransition, TheVersionChangesWithTheSetAndOnlyWithIt) {
  Tx t;
  const uint64_t v0 = gtext_css_transition_set_version(t.set);
  const std::string a = kOpacity + "opacity: 1";
  ASSERT_EQ(t.event(a, a, 0), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_transition_set_version(t.set), v0) << "nothing happened";
  ASSERT_EQ(t.event("opacity: 0", a, 0), GTEXT_CSS_OK);
  const uint64_t v1 = gtext_css_transition_set_version(t.set);
  EXPECT_NE(v1, v0);
  gtext_css_transition_set_clear(t.set);
  EXPECT_EQ(t.running(), 0u);
  EXPECT_NE(gtext_css_transition_set_version(t.set), v1);
}

TEST(CssTransition, BadArgumentsAreRefused) {
  Tx t;
  EXPECT_EQ(gtext_css_transitions_update(nullptr, t.set, nullptr, nullptr, 0, nullptr), GTEXT_CSS_E_INVALID);
  GTEXT_CSS_Computed_Style * a = make_style(t.ctx, "opacity: 1");
  EXPECT_EQ(gtext_css_transitions_update(t.ctx.c, nullptr, nullptr, a, 0, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_transitions_update(t.ctx.c, t.set, nullptr, nullptr, 0, nullptr), GTEXT_CSS_E_INVALID);
  gtext_css_computed_free(a);
  GTEXT_CSS_Transition_Set * s = nullptr;
  EXPECT_EQ(gtext_css_transition_set_new(nullptr, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_transition_set_new(nullptr, &s), GTEXT_CSS_OK);
  gtext_css_transition_set_free(s);
  gtext_css_transition_set_free(nullptr);
  EXPECT_EQ(gtext_css_transition_set_running_count(nullptr), 0u);
  GTEXT_CSS_Transition_Info info{};
  EXPECT_FALSE(gtext_css_transition_set_running_at(nullptr, 0, &info));
  EXPECT_FALSE(gtext_css_transition_set_running_at(t.set, 0, &info));
}

/* ---- allocation failure ---------------------------------------------------------------------------------------------------------------------------- */

/* An update that fails leaves the set as it was ("all or nothing"): every allocation of a start, a reversal and a retarget fails in turn. */
TEST(CssTransitionOom, EveryAllocationOfAnUpdateFailsCleanlyAndLeavesTheSetAsItWas) {
  FaultAllocator fa;
  GTEXT_CSS_Compute_Options co = gtext_css_compute_options_default();
  co.allocator = &fa.alloc;
  Context ctx(R(), nullptr, &co);
  ASSERT_EQ(ctx.status, GTEXT_CSS_OK);
  const std::string out = kOpacity + "opacity: 1";
  const std::string back = kOpacity + "opacity: 0";
  GTEXT_CSS_Computed_Style * zero = make_style(ctx, "opacity: 0");
  GTEXT_CSS_Computed_Style * one = make_style(ctx, out);
  GTEXT_CSS_Computed_Style * zero_t = make_style(ctx, back);
  ASSERT_NE(zero, nullptr);
  ASSERT_NE(one, nullptr);
  ASSERT_NE(zero_t, nullptr);
  struct Step {
    const GTEXT_CSS_Computed_Style * before;
    const GTEXT_CSS_Computed_Style * after;
    double t;
  };
  const Step steps[] = {{zero, one, 0}, {one, zero_t, 250}, {zero_t, one, 300}, {one, one, 5000}};
  GTEXT_CSS_Transition_Set * set = nullptr;
  ASSERT_EQ(gtext_css_transition_set_new(&fa.alloc, &set), GTEXT_CSS_OK);
  for (const Step & st : steps) {
    const uint64_t version = gtext_css_transition_set_version(set);
    const size_t running = gtext_css_transition_set_running_count(set);
    const size_t baseline = fa.live.size();
    bool done = false;
    for (size_t n = 1; n < 100000 && !done; n++) {
      fa.arm(n);
      const GTEXT_CSS_Status status = gtext_css_transitions_update(ctx.c, set, st.before, st.after, st.t, nullptr);
      fa.disarm();
      if (!fa.failed) {
        EXPECT_EQ(status, GTEXT_CSS_OK);
        done = true;
        break;
      }
      EXPECT_EQ(status, GTEXT_CSS_E_OOM) << n;
      EXPECT_EQ(gtext_css_transition_set_version(set), version) << n << ": a failed update changed the set";
      EXPECT_EQ(gtext_css_transition_set_running_count(set), running) << n;
      EXPECT_EQ(fa.live.size(), baseline) << n << ": a block is left";
      EXPECT_EQ(fa.bad_frees, 0u) << n;
    }
    EXPECT_TRUE(done);
  }
  gtext_css_transition_set_free(set);
  gtext_css_computed_free(zero);
  gtext_css_computed_free(one);
  gtext_css_computed_free(zero_t);
}

/* The style computed with a set, every allocation failed in turn. */
TEST(CssTransitionOom, TheStyleOfATransitioningElementSweptOverEveryAllocation) {
  FaultAllocator fa;
  GTEXT_CSS_Compute_Options co = gtext_css_compute_options_default();
  co.allocator = &fa.alloc;
  Context ctx(R(), nullptr, &co);
  ASSERT_EQ(ctx.status, GTEXT_CSS_OK);
  GTEXT_CSS_Transition_Set * set = nullptr;
  ASSERT_EQ(gtext_css_transition_set_new(&fa.alloc, &set), GTEXT_CSS_OK);
  const std::string a = "transition: opacity 1s linear, width 1s linear; opacity: 1; width: 100px";
  GTEXT_CSS_Computed_Style * before = make_style(ctx, "opacity: 0; width: 0px");
  GTEXT_CSS_Computed_Style * after = make_style(ctx, a);
  ASSERT_EQ(gtext_css_transitions_update(ctx.c, set, before, after, 0, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_transition_set_running_count(set), 2u);
  gtext_css_computed_free(before);
  gtext_css_computed_free(after);
  {
    GTEXT_CSS_Computed_Style * warm = make_style(ctx, a, set, 500);
    ASSERT_NE(warm, nullptr);
    gtext_css_computed_free(warm);
  }
  const size_t baseline = fa.live.size();
  size_t failures = 0;
  for (size_t n = 1; n < 100000; n++) {
    GTEXT_CSS_Status status = GTEXT_CSS_OK;
    fa.arm(n);
    GTEXT_CSS_Computed_Style * s = make_style(ctx, a, set, 500, nullptr, &status);
    fa.disarm();
    if (!fa.failed) {
      ASSERT_NE(s, nullptr);
      EXPECT_EQ(val(s, "opacity"), "0.5");
      EXPECT_EQ(val(s, "width"), "50px");
      gtext_css_computed_free(s);
      EXPECT_EQ(fa.live.size(), baseline);
      break;
    }
    failures++;
    EXPECT_EQ(status, GTEXT_CSS_E_OOM) << n;
    EXPECT_EQ(s, nullptr) << n;
    EXPECT_EQ(fa.live.size(), baseline) << n << ": a block is left";
    EXPECT_EQ(fa.bad_frees, 0u) << n;
  }
  EXPECT_GT(failures, 20u);
  gtext_css_transition_set_free(set);
}

} // namespace
