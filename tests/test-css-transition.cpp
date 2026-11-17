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
  ASSERT_TRUE(gtext_css_transition_set_completed_at(t.set, 0, &p, nullptr, &end));
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

// ---- custom properties ---------------------------------------------------------------------------------------------------------------------------------------------------------------

/// The computed value text of a custom property of a style ("<none>" for the guaranteed-invalid value).
std::string custom_val(const GTEXT_CSS_Computed_Style * style, const std::string & name) {
  const GTEXT_CSS_Custom_Value * cv = nullptr;
  if (!gtext_css_computed_custom_find(style, name.data(), name.size(), &cv) || !cv) {
    return "<none>";
  }
  char * out = nullptr;
  size_t n = 0;
  if (gtext_css_custom_value_text(cv, nullptr, &out, &n) != GTEXT_CSS_OK) {
    return "<error>";
  }
  std::string text(out, n);
  gtext_css_serialize_buffer_free(nullptr, out);
  return text;
}

std::string shown_custom(Tx & t, const std::string & decls, const std::string & name, double ms) {
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  GTEXT_CSS_Computed_Style * s = make_style(t.ctx, decls, t.set, ms, nullptr, &status);
  const std::string v = s ? custom_val(s, name) : "<no style>";
  gtext_css_computed_free(s);
  return v;
}

/* "the animation type is discrete" for "a custom property [that] is not registered" (Web Animations 1, "Custom Properties"), and "the property values are transitionable if they have an animation
   type that is neither not animatable nor discrete": an unregistered custom property transitions only with `transition-behavior: allow-discrete` (Transitions 2), when its value is the start value
   until the middle of the transition and the end value from it, as every discrete animation type does. */
TEST(CssTransition, AnUnregisteredCustomPropertyTransitionsOnlyWhenTheBehaviorAllowsDiscreteValues) {
  Tx plain;
  ASSERT_EQ(plain.event("--x: a", "transition: --x 1s linear; --x: b", 0), GTEXT_CSS_OK);
  EXPECT_EQ(plain.up.started, 0u) << "a discrete pair, and the behavior is normal";
  Tx t;
  const std::string decls = "transition: --x 1s linear; transition-behavior: allow-discrete; --x: b";
  ASSERT_EQ(t.event("--x: a", decls, 100), GTEXT_CSS_OK);
  EXPECT_EQ(t.up.started, 1u);
  ASSERT_EQ(t.running(), 1u);
  const GTEXT_CSS_Transition_Info i = t.info();
  EXPECT_EQ(i.property, 0u);
  EXPECT_EQ(std::string(i.custom_name.data, i.custom_name.length), "--x");
  EXPECT_EQ(std::string(i.start_value.data, i.start_value.length), "a");
  EXPECT_EQ(std::string(i.end_value.data, i.end_value.length), "b");
  EXPECT_EQ(shown_custom(t, decls, "--x", 100), "a");
  EXPECT_EQ(shown_custom(t, decls, "--x", 599), "a") << "below the middle of the transition";
  EXPECT_EQ(shown_custom(t, decls, "--x", 600), "b") << "from it";
  EXPECT_EQ(shown_custom(t, decls, "--x", 1100), "b");
  ASSERT_EQ(t.event(decls, decls, 1100), GTEXT_CSS_OK);
  EXPECT_EQ(t.up.completed, 1u);
  GTEXT_CSS_Property_Id p = 1;
  GTEXT_CSS_String name{}, end{};
  ASSERT_TRUE(gtext_css_transition_set_completed_at(t.set, 0, &p, &name, &end));
  EXPECT_EQ(p, 0u);
  EXPECT_EQ(std::string(name.data, name.length), "--x");
  EXPECT_EQ(std::string(end.data, end.length), "b");
}

/* A custom property nothing sets has the guaranteed-invalid value, which has no text (the empty one is `--x:;`): a transition from it is discrete as well, and to it ("the value of the property in
   the after-change style" is that value), and the current value of one that is still the guaranteed-invalid value is that value again and not an empty one. */
TEST(CssTransition, TheGuaranteedInvalidValueIsAValueOfADiscreteTransition) {
  Tx t;
  const std::string with = "transition: --x 1s step-end; transition-behavior: allow-discrete; --x: a";
  const std::string without = "transition: --x 1s step-end; transition-behavior: allow-discrete";
  ASSERT_EQ(t.event(without, with, 0), GTEXT_CSS_OK);
  ASSERT_EQ(t.up.started, 1u);
  EXPECT_EQ(std::string(t.info().start_value.data, t.info().start_value.length), "initial") << "the guaranteed-invalid value is written as the declaration that gives it";
  EXPECT_EQ(shown_custom(t, with, "--x", 500), "<none>");
  ASSERT_EQ(t.event(with, without, 2000), GTEXT_CSS_OK);
  EXPECT_EQ(t.up.started, 1u) << "back to no value: a transition to the guaranteed-invalid value";
}

/* `all` names the properties that can be animated, which are the longhands and the registered custom properties that have a type; an unregistered one is named by its own name only. */
TEST(CssTransition, AllDoesNotNameAnUnregisteredCustomProperty) {
  Tx t;
  ASSERT_EQ(t.event("--x: a", "transition: all 1s linear; transition-behavior: allow-discrete; --x: b", 0), GTEXT_CSS_OK);
  EXPECT_EQ(t.up.started, 0u);
}

/* The custom properties of a transition are told apart by name (names are case-sensitive), and a running transition of a custom property that no `transition-property` item names any more is
   cancelled like any other. */
TEST(CssTransition, CustomPropertiesAreKeyedByTheirCaseSensitiveNameAndAreCancelledWhenNoLongerNamed) {
  Tx t;
  const std::string both = "transition: --a 1s linear, --A 1s linear; transition-behavior: allow-discrete; ";
  ASSERT_EQ(t.event("--a: 1; --A: 1", both + "--a: 2; --A: 3", 0), GTEXT_CSS_OK);
  EXPECT_EQ(t.up.started, 2u);
  ASSERT_EQ(t.running(), 2u);
  EXPECT_NE(std::string(t.info(0).custom_name.data, t.info(0).custom_name.length), std::string(t.info(1).custom_name.data, t.info(1).custom_name.length));
  ASSERT_EQ(t.event(both + "--a: 2; --A: 3", "transition: --a 1s linear; transition-behavior: allow-discrete; --a: 2; --A: 3", 100), GTEXT_CSS_OK);
  EXPECT_EQ(t.up.cancelled, 1u) << "--A is not named any more";
  ASSERT_EQ(t.running(), 1u);
  EXPECT_EQ(std::string(t.info(0).custom_name.data, t.info(0).custom_name.length), "--a");
}

/* ---- events and CSSTransition ---------------------------------------------------------------------------------------------------------------------- */

const char * type_name(GTEXT_CSS_Transition_Event_Type t) {
  switch (t) {
    case GTEXT_CSS_TRANSITION_EVENT_RUN: return "run";
    case GTEXT_CSS_TRANSITION_EVENT_START: return "start";
    case GTEXT_CSS_TRANSITION_EVENT_END: return "end";
    case GTEXT_CSS_TRANSITION_EVENT_CANCEL: return "cancel";
  }
  return "?";
}

std::string num(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%g", v);
  return buf;
}

/// One frame of a set at a time, as text: `type:name:elapsed` for each event, joined by spaces.
std::string frame(GTEXT_CSS_Transition_Set * set, double t, GTEXT_CSS_Status * status = nullptr) {
  GTEXT_CSS_Transition_Events * ev = nullptr;
  EXPECT_EQ(gtext_css_transition_events_new(nullptr, &ev), GTEXT_CSS_OK);
  const GTEXT_CSS_Status st = gtext_css_transition_set_frame(set, t, ev);
  if (status) {
    *status = st;
  }
  std::string out;
  for (size_t i = 0; i < gtext_css_transition_events_count(ev); i++) {
    GTEXT_CSS_Transition_Event e{};
    EXPECT_TRUE(gtext_css_transition_events_at(ev, i, &e));
    out += std::string(i ? " " : "") + type_name(e.type) + ":" + std::string(e.property_name.data, e.property_name.length) + ":" + num(e.elapsed_time);
  }
  gtext_css_transition_events_free(ev);
  return out;
}

const std::string kFade = "transition: opacity 1s linear; ";

/* "The transitionrun event occurs when a transition is created", "The transitionstart event occurs when a transition's delay phase ends", "The transitionend event occurs at the completion of the
   transition ... The value of elapsedTime for this event is equal to the value of transition-duration" (Transitions 1); the typical sequence "Regular playback: transitionrun, transitionstart,
   transitionend" (Transitions 2). The first frame sees an active transition: idle -> active. */
TEST(CssTransitionEvents, RegularPlaybackIsRunStartEnd) {
  Tx t;
  ASSERT_EQ(t.event("opacity: 0", kFade + "opacity: 1", 0), GTEXT_CSS_OK);
  EXPECT_EQ(frame(t.set, 0), "run:opacity:0 start:opacity:0");
  EXPECT_EQ(frame(t.set, 500), "");
  EXPECT_EQ(frame(t.set, 1000), "end:opacity:1");
  EXPECT_EQ(frame(t.set, 1000), "") << "the same time again reports nothing: the phase of each transition moved on";
  ASSERT_EQ(t.event(kFade + "opacity: 1", kFade + "opacity: 1", 1000), GTEXT_CSS_OK);
  EXPECT_EQ(t.up.completed, 1u);
  EXPECT_EQ(frame(t.set, 1100), "") << "a transition that had already reported its end reports nothing when the style change event moves it to the completed ones";
}

/* "idle -> pending or before: transitionrun" and "pending or before -> active: transitionstart" with the "interval start" (0 for a positive delay); "active -> after: transitionend". */
TEST(CssTransitionEvents, APositiveDelayRunsFirstAndStartsWhenTheDelayEnds) {
  Tx t;
  ASSERT_EQ(t.event("opacity: 0", "transition: opacity 1s linear 500ms; opacity: 1", 0), GTEXT_CSS_OK);
  EXPECT_EQ(frame(t.set, 0), "run:opacity:0");
  EXPECT_EQ(frame(t.set, 499), "");
  EXPECT_EQ(frame(t.set, 500), "start:opacity:0");
  EXPECT_EQ(frame(t.set, 1499), "");
  EXPECT_EQ(frame(t.set, 1500), "end:opacity:1");
}

/* "A negative transition-delay will cause the event to fire with an elapsedTime equal to the absolute value of the delay capped to the transition-duration ... min(max(-'transition-delay', 0),
   'transition-duration')" (Transitions 1), and the same for transitionstart; the end event has the duration. */
TEST(CssTransitionEvents, ANegativeDelayGivesTheRunAndTheStartAnElapsedTimeOfTheDelayAndTheEndTheDuration) {
  Tx t;
  ASSERT_EQ(t.event("opacity: 0", "transition: opacity 2s linear -500ms; opacity: 1", 0), GTEXT_CSS_OK);
  EXPECT_EQ(frame(t.set, 0), "run:opacity:0.5 start:opacity:0.5");
  EXPECT_EQ(frame(t.set, 1500), "end:opacity:2");
}

/* The rows of the table that a frame that skips a phase gives: "idle -> after: transitionrun, transitionstart, transitionend" and "pending or before -> after: transitionstart, transitionend". */
TEST(CssTransitionEvents, AFrameThatSkipsAPhaseReportsTheEventsOfEachInOrder) {
  Tx a;
  ASSERT_EQ(a.event("opacity: 0", "transition: opacity 1s linear 1s; opacity: 1", 0), GTEXT_CSS_OK);
  EXPECT_EQ(frame(a.set, 5000), "run:opacity:0 start:opacity:0 end:opacity:1");
  Tx b;
  ASSERT_EQ(b.event("opacity: 0", "transition: opacity 1s linear 1s; opacity: 1", 0), GTEXT_CSS_OK);
  EXPECT_EQ(frame(b.set, 0), "run:opacity:0");
  EXPECT_EQ(frame(b.set, 5000), "start:opacity:0 end:opacity:1");
}

/* The frame goes backwards: "active -> before: transitionend ... interval start", "after -> active: transitionstart ... interval end", "after -> before: transitionstart, transitionend". The set's
   transitions are what a caller that has moved its own clock back sees; the table is the same. */
TEST(CssTransitionEvents, TheTableForATimeThatMovesBackwards) {
  Tx t;
  ASSERT_EQ(t.event("opacity: 0", "transition: opacity 1s linear 1s; opacity: 1", 0), GTEXT_CSS_OK);
  EXPECT_EQ(frame(t.set, 1500), "run:opacity:0 start:opacity:0");
  EXPECT_EQ(frame(t.set, 500), "end:opacity:0") << "active -> before: transitionend at the interval start";
  EXPECT_EQ(frame(t.set, 2500), "start:opacity:0 end:opacity:1") << "before -> after";
  EXPECT_EQ(frame(t.set, 1500), "start:opacity:1") << "after -> active: transitionstart at the interval end";
  EXPECT_EQ(frame(t.set, 500), "end:opacity:0") << "active -> before";
  EXPECT_EQ(frame(t.set, 2500), "start:opacity:0 end:opacity:1");
  EXPECT_EQ(frame(t.set, 500), "start:opacity:1 end:opacity:0") << "after -> before: transitionstart at the interval end, transitionend at the interval start";
}

/* "transitioncancel ... the number of seconds from the end of the transition's delay to the moment when the transition was canceled" (Transitions 1), and the table: "not idle and not after
   -> idle: transitioncancel" with "the active time of the animation at the moment it was canceled". A retarget cancels the transition (its generation is the older one: it is reported first)
   and starts another, which has its own run and start. "Interrupted playback: transitionrun, transitionstart, transitioncancel." */
TEST(CssTransitionEvents, ARetargetCancelsAtTheActiveTimeAndStartsTheNext) {
  Tx t;
  const std::string a = kFade + "opacity: 1", b = kFade + "opacity: 0.5";
  ASSERT_EQ(t.event("opacity: 0", a, 0), GTEXT_CSS_OK);
  EXPECT_EQ(frame(t.set, 0), "run:opacity:0 start:opacity:0");
  ASSERT_EQ(t.event(a, b, 400), GTEXT_CSS_OK);
  EXPECT_EQ(t.up.cancelled, 1u);
  EXPECT_EQ(t.up.started, 1u);
  EXPECT_EQ(frame(t.set, 400), "cancel:opacity:0.4 run:opacity:0 start:opacity:0");
  EXPECT_EQ(frame(t.set, 1400), "end:opacity:1");
}

/* "Interrupted playback during delay or pending phase: transitionrun, transitioncancel": a cancellation in the delay phase has an active time of 0 (a fill of both clamps it). */
TEST(CssTransitionEvents, ACancellationInTheDelayPhaseHasAnElapsedTimeOfZero) {
  Tx t;
  const std::string a = "transition: opacity 1s linear 1s; opacity: 1";
  ASSERT_EQ(t.event("opacity: 0", a, 0), GTEXT_CSS_OK);
  EXPECT_EQ(frame(t.set, 0), "run:opacity:0");
  ASSERT_EQ(t.event(a, "transition: none; opacity: 1", 300), GTEXT_CSS_OK) << "the property is no longer in transition-property";
  EXPECT_EQ(t.up.cancelled, 1u);
  EXPECT_EQ(frame(t.set, 300), "cancel:opacity:0");
}

/* "idle -> idle": a transition that no frame had seen and that is cancelled reports nothing, and "For every transitionrun event there will be a exactly one transitionend or transitioncancel". */
TEST(CssTransitionEvents, ATransitionCancelledBeforeAnyFrameReportsNothing) {
  Tx t;
  const std::string a = kFade + "opacity: 1";
  ASSERT_EQ(t.event("opacity: 0", a, 0), GTEXT_CSS_OK);
  ASSERT_EQ(t.event(a, kFade + "opacity: 0.5", 100), GTEXT_CSS_OK);
  ASSERT_EQ(t.event(kFade + "opacity: 0.5", "transition: none; opacity: 0.5", 150), GTEXT_CSS_OK);
  EXPECT_EQ(t.running(), 0u);
  EXPECT_EQ(frame(t.set, 150), "");
}

/* "When a transition completes, implementations must move all transitions that complete at that time from the set of running transitions to the set of completed transitions and then fire the
   events for those completions" (Transitions 1): a transition that completes in a style change event before any frame saw it still reports everything up to its end. */
TEST(CssTransitionEvents, ATransitionThatCompletedBeforeAnyFrameReportsRunStartAndEnd) {
  Tx t;
  const std::string a = kFade + "opacity: 1";
  ASSERT_EQ(t.event("opacity: 0", a, 0), GTEXT_CSS_OK);
  ASSERT_EQ(t.event(a, a, 2000), GTEXT_CSS_OK);
  EXPECT_EQ(t.up.completed, 1u);
  EXPECT_EQ(frame(t.set, 2000), "run:opacity:0 start:opacity:0 end:opacity:1");
  EXPECT_EQ(frame(t.set, 2000), "");
}

/* The same when a frame saw it active: only the end is owed. */
TEST(CssTransitionEvents, ATransitionThatCompletesAfterAFrameSawItActiveOwesOnlyTheEnd) {
  Tx t;
  const std::string a = kFade + "opacity: 1";
  ASSERT_EQ(t.event("opacity: 0", a, 0), GTEXT_CSS_OK);
  EXPECT_EQ(frame(t.set, 100), "run:opacity:0 start:opacity:0");
  ASSERT_EQ(t.event(a, a, 1500), GTEXT_CSS_OK);
  EXPECT_EQ(frame(t.set, 1500), "end:opacity:1");
  EXPECT_EQ(frame(t.set, 1600), "");
}

/* "Sort A and B in ascending order by the Unicode codepoints that make up the expanded transition property name", after the transition generation. The registry iterates the longhands by name and the
   custom properties after them, so the order the transitions are made in is not the codepoint order of `--z` and `opacity` ('-' sorts before 'o'); a transition of an earlier style change event
   comes before one of a later one whatever its name (`color` sorts before `opacity`). */
TEST(CssTransitionEvents, EventsAreInTheCompositeOrderOfTheTransitions) {
  Tx t;
  const std::string tr = "transition: opacity 1s linear, --z 1s linear, color 1s linear; transition-behavior: allow-discrete; ";
  ASSERT_EQ(t.event("opacity: 0; --z: a; color: red", tr + "opacity: 1; --z: b; color: red", 0), GTEXT_CSS_OK);
  ASSERT_EQ(t.running(), 2u);
  EXPECT_EQ(std::string(t.info(0).property_name.data, t.info(0).property_name.length), "opacity") << "the transitions are made longhands first: the order of the running set is not the composite order";
  ASSERT_EQ(t.event(tr + "opacity: 1; --z: b; color: red", tr + "opacity: 1; --z: b; color: blue", 20), GTEXT_CSS_OK) << "color is of the second generation";
  ASSERT_EQ(t.running(), 3u);
  EXPECT_EQ(frame(t.set, 20), "run:--z:0 start:--z:0 run:opacity:0 start:opacity:0 run:color:0 start:color:0");
}

/* A custom property's transition reports the property's name, and the event is of transition 0 of the longhand ids. */
TEST(CssTransitionEvents, ACustomPropertyEventNamesTheCustomProperty) {
  Tx t;
  const std::string tr = "transition: --x 1s linear; transition-behavior: allow-discrete; ";
  ASSERT_EQ(t.event("--x: a", tr + "--x: b", 0), GTEXT_CSS_OK);
  GTEXT_CSS_Transition_Events * ev = nullptr;
  ASSERT_EQ(gtext_css_transition_events_new(nullptr, &ev), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_transition_set_frame(t.set, 0, ev), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_transition_events_count(ev), 2u);
  GTEXT_CSS_Transition_Event e{};
  ASSERT_TRUE(gtext_css_transition_events_at(ev, 0, &e));
  EXPECT_EQ(e.property, 0u);
  EXPECT_EQ(std::string(e.property_name.data, e.property_name.length), "--x");
  EXPECT_EQ(e.transition, t.info().id);
  EXPECT_FALSE(gtext_css_transition_events_at(ev, 2, &e));
  gtext_css_transition_events_free(ev);
}

/* "implementations must also cancel any running transitions on elements that are no longer being rendered and remove transitions on them from the set of completed transitions" (Transitions 2);
   what a frame had seen is reported as cancelled at the active time of the moment, and a set that was never framed reports nothing. clear() is the same without the events. */
TEST(CssTransitionEvents, CancelAllCancelsTheRunningAndForgetsTheCompleted) {
  Tx t;
  const std::string a = "transition: opacity 1s linear, color 1s linear; opacity: 1; color: red";
  ASSERT_EQ(t.event("opacity: 0; color: blue", a, 0), GTEXT_CSS_OK);
  ASSERT_EQ(t.running(), 2u);
  EXPECT_EQ(frame(t.set, 0), "run:color:0 start:color:0 run:opacity:0 start:opacity:0");
  ASSERT_EQ(t.event(a, "transition: opacity 3s linear, color 1s linear; opacity: 1; color: red", 1000), GTEXT_CSS_OK);
  EXPECT_EQ(t.up.completed, 2u);
  EXPECT_EQ(gtext_css_transition_set_completed_count(t.set), 2u);
  ASSERT_EQ(t.event("opacity: 1; color: red", "transition: opacity 1s linear; opacity: 0", 1000), GTEXT_CSS_OK);
  ASSERT_EQ(t.running(), 1u);
  EXPECT_EQ(frame(t.set, 1100), "end:color:1 end:opacity:1 run:opacity:0 start:opacity:0") << "the two completions owed, then the new transition";
  ASSERT_EQ(gtext_css_transition_set_cancel_all(t.set, 1400), GTEXT_CSS_OK);
  EXPECT_EQ(t.running(), 0u);
  EXPECT_EQ(gtext_css_transition_set_completed_count(t.set), 0u);
  EXPECT_EQ(frame(t.set, 1400), "cancel:opacity:0.4");
  EXPECT_EQ(frame(t.set, 1400), "");
  Tx finished;
  ASSERT_EQ(finished.event("opacity: 0", kFade + "opacity: 1", 0), GTEXT_CSS_OK);
  ASSERT_EQ(finished.event(kFade + "opacity: 1", kFade + "opacity: 1", 1000), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_transition_set_completed_count(finished.set), 1u);
  ASSERT_EQ(gtext_css_transition_set_cancel_all(finished.set, 1000), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_transition_set_completed_count(finished.set), 0u) << "the completed transitions are removed";
  Tx quiet;
  ASSERT_EQ(quiet.event("opacity: 0", kFade + "opacity: 1", 0), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_transition_set_cancel_all(quiet.set, 100), GTEXT_CSS_OK);
  EXPECT_EQ(frame(quiet.set, 100), "") << "no frame had seen the transition";
  Tx cleared;
  ASSERT_EQ(cleared.event("opacity: 0", kFade + "opacity: 1", 0), GTEXT_CSS_OK);
  EXPECT_EQ(frame(cleared.set, 0), "run:opacity:0 start:opacity:0");
  gtext_css_transition_set_clear(cleared.set);
  EXPECT_EQ(frame(cleared.set, 100), "") << "clear() is the set of something that is gone: no events";
}

/* A reversal: "Reversed playback after completion: transitionrun, transitionstart, transitionend, transitionstart, transitionend" is for a seeked transition; a CSS reversal cancels the running
   transition and starts a new one, whose effect is shortened (its end event has the shortened duration, "interval end" of its own effect). */
TEST(CssTransitionEvents, AReversalCancelsTheOldAndTheShortenedNewOneEndsAtItsOwnDuration) {
  Tx t;
  const std::string out = kFade + "opacity: 1", back = kFade + "opacity: 0";
  ASSERT_EQ(t.event("opacity: 0", out, 0), GTEXT_CSS_OK);
  EXPECT_EQ(frame(t.set, 0), "run:opacity:0 start:opacity:0");
  ASSERT_EQ(t.event(out, back, 250), GTEXT_CSS_OK);
  ASSERT_EQ(t.running(), 1u);
  EXPECT_EQ(t.info().reversing_shortening_factor, 0.25);
  EXPECT_EQ(frame(t.set, 250), "cancel:opacity:0.25 run:opacity:0 start:opacity:0");
  EXPECT_EQ(frame(t.set, 500), "end:opacity:0.25");
}

/* The snapshot of a transition as the Web Animations interface reads it ("transitionProperty", `startTime` unresolved while pending, `getTiming()` and `getComputedTiming()` of the pinned CSSTransition
   pages: fill "backwards", endDelay 0, the delay and the duration, the easing). */
TEST(CssTransitionEvents, TheSnapshotOfARunningTransitionIsItsCSSTransition) {
  Tx t;
  const std::string a = "transition: opacity 2s ease-in 1s; opacity: 1";
  ASSERT_EQ(t.event("opacity: 0", a, 1000), GTEXT_CSS_OK);
  GTEXT_CSS_Transition_Animation an{};
  ASSERT_EQ(gtext_css_transition_set_animation_at(t.ctx.c, t.set, 0, 1000, &an), GTEXT_CSS_OK);
  EXPECT_EQ(std::string(an.transition_property.data, an.transition_property.length), "opacity");
  EXPECT_TRUE(an.pending);
  EXPECT_FALSE(an.start_time_resolved);
  EXPECT_EQ(an.current_time_ms, 0);
  EXPECT_EQ(an.play_state, GTEXT_CSS_TRANSITION_PLAY_RUNNING);
  EXPECT_EQ(an.delay_ms, 1000);
  EXPECT_EQ(an.end_delay_ms, 0);
  EXPECT_EQ(an.duration_ms, 2000);
  EXPECT_EQ(an.active_duration_ms, 2000);
  EXPECT_EQ(an.end_time_ms, 3000);
  EXPECT_EQ(std::string(an.easing.data, an.easing.length), "ease-in");
  EXPECT_EQ(std::string(an.start_value.data, an.start_value.length), "0");
  EXPECT_EQ(std::string(an.end_value.data, an.end_value.length), "1");
  EXPECT_EQ(an.id, t.info().id);
  EXPECT_EQ(frame(t.set, 1000), "run:opacity:0");
  ASSERT_EQ(gtext_css_transition_set_animation_at(t.ctx.c, t.set, 0, 1500, &an), GTEXT_CSS_OK);
  EXPECT_FALSE(an.pending);
  EXPECT_TRUE(an.start_time_resolved);
  EXPECT_EQ(an.start_time_ms, 1000);
  EXPECT_EQ(an.current_time_ms, 500);
  EXPECT_EQ(an.local_time_ms, 500);
  EXPECT_EQ(an.phase, GTEXT_CSS_TRANSITION_PHASE_BEFORE) << "the delay is 1000: local time 500 is before the effect";
  ASSERT_TRUE(an.has_progress) << "a fill of backwards holds the first frame in the delay phase";
  EXPECT_EQ(an.progress, 0);
  ASSERT_EQ(gtext_css_transition_set_animation_at(t.ctx.c, t.set, 0, 2000, &an), GTEXT_CSS_OK);
  EXPECT_EQ(an.phase, GTEXT_CSS_TRANSITION_PHASE_ACTIVE);
  EXPECT_EQ(an.progress, 0) << "ease-in at 0";
  ASSERT_EQ(gtext_css_transition_set_animation_at(t.ctx.c, t.set, 0, 3000, &an), GTEXT_CSS_OK);
  EXPECT_EQ(an.phase, GTEXT_CSS_TRANSITION_PHASE_ACTIVE);
  EXPECT_GT(an.progress, 0.3) << "half way through, ease-in is below half";
  EXPECT_LT(an.progress, 0.5);
  ASSERT_EQ(gtext_css_transition_set_animation_at(t.ctx.c, t.set, 0, 4000, &an), GTEXT_CSS_OK);
  EXPECT_EQ(an.phase, GTEXT_CSS_TRANSITION_PHASE_AFTER);
  EXPECT_EQ(an.play_state, GTEXT_CSS_TRANSITION_PLAY_FINISHED);
  EXPECT_FALSE(an.has_progress) << "after the effect, with no forwards fill, the progress is null";
  EXPECT_NE(gtext_css_transition_set_animation_at(t.ctx.c, t.set, 1, 4000, &an), GTEXT_CSS_OK);
}

/* A negative delay starts the effect part way through: the snapshot's delay is negative, and the transition is active from the first frame. */
TEST(CssTransitionEvents, ANegativeDelayIsTheEffectsDelayAndTheTransitionIsActiveAtOnce) {
  Tx t;
  ASSERT_EQ(t.event("opacity: 0", "transition: opacity 2s linear -500ms; opacity: 1", 100), GTEXT_CSS_OK);
  EXPECT_EQ(frame(t.set, 100), "run:opacity:0.5 start:opacity:0.5");
  GTEXT_CSS_Transition_Animation an{};
  ASSERT_EQ(gtext_css_transition_set_animation_at(t.ctx.c, t.set, 0, 100, &an), GTEXT_CSS_OK);
  EXPECT_EQ(an.delay_ms, -500);
  EXPECT_EQ(an.start_time_ms, 100);
  EXPECT_EQ(an.current_time_ms, 0);
  EXPECT_EQ(an.phase, GTEXT_CSS_TRANSITION_PHASE_ACTIVE);
  EXPECT_EQ(an.end_time_ms, 1500);
  EXPECT_NEAR(an.progress, 0.25, 1e-9);
}

/* Every allocation of a frame fails in turn: the set and the list are as they were, and the same frame then reports everything. */
TEST(CssTransitionOom, EveryAllocationOfAFrameFailsCleanlyAndTheFrameIsThenWhole) {
  FaultAllocator fa;
  GTEXT_CSS_Compute_Options co = gtext_css_compute_options_default();
  co.allocator = &fa.alloc;
  Context ctx(R(), nullptr, &co);
  ASSERT_EQ(ctx.status, GTEXT_CSS_OK);
  const std::string out = kOpacity + "opacity: 1", back = kOpacity + "opacity: 0";
  GTEXT_CSS_Computed_Style * zero = make_style(ctx, "opacity: 0");
  GTEXT_CSS_Computed_Style * one = make_style(ctx, out);
  GTEXT_CSS_Computed_Style * zero_t = make_style(ctx, back);
  ASSERT_NE(zero, nullptr);
  GTEXT_CSS_Transition_Set * set = nullptr;
  ASSERT_EQ(gtext_css_transition_set_new(&fa.alloc, &set), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_transitions_update(ctx.c, set, zero, one, 0, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Transition_Events * seen = nullptr;
  ASSERT_EQ(gtext_css_transition_events_new(&fa.alloc, &seen), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_transition_set_frame(set, 0, seen), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_transitions_update(ctx.c, set, one, zero_t, 250, nullptr), GTEXT_CSS_OK) << "a cancellation and a new transition: the frame owes a cancel and a run and a start";
  const size_t baseline = fa.live.size();
  bool done = false;
  for (size_t n = 1; n < 100000 && !done; n++) {
    GTEXT_CSS_Transition_Events * ev = nullptr;
    ASSERT_EQ(gtext_css_transition_events_new(&fa.alloc, &ev), GTEXT_CSS_OK);
    fa.arm(n);
    const GTEXT_CSS_Status st = gtext_css_transition_set_frame(set, 250, ev);
    fa.disarm();
    if (!fa.failed) {
      ASSERT_EQ(st, GTEXT_CSS_OK);
      EXPECT_EQ(gtext_css_transition_events_count(ev), 3u);
      done = true;
    }
    else {
      EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
      EXPECT_EQ(gtext_css_transition_events_count(ev), 0u) << n;
    }
    gtext_css_transition_events_free(ev);
  }
  EXPECT_TRUE(done);
  EXPECT_EQ(fa.bad_frees, 0u);
  gtext_css_transition_events_free(seen);
  gtext_css_transition_set_free(set);
  gtext_css_computed_free(zero);
  gtext_css_computed_free(one);
  gtext_css_computed_free(zero_t);
  (void)baseline;
}

} // namespace
