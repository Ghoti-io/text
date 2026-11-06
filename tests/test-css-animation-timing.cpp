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
 * The timing model of an effect and the easing functions (css_animation.h), against Web Animations 1 ("Timing model": phases, active time, overall progress, simple iteration
 * progress, current iteration index, directed progress) and CSS Easing Functions 1 and 2 (the output of a cubic Bezier, a step function and `linear()`).
 */

#include <string>

#include <gtest/gtest.h>

#include "css_compute_test_support.h"

using namespace cmp;

namespace {

GTEXT_CSS_Anim_Effect effect(double duration_ms, double delay_ms = 0, double count = 1, GTEXT_CSS_Anim_Fill fill = GTEXT_CSS_ANIM_FILL_NONE,
    GTEXT_CSS_Anim_Direction dir = GTEXT_CSS_ANIM_DIRECTION_NORMAL, double end_delay_ms = 0) {
  GTEXT_CSS_Anim_Effect e;
  std::memset(&e, 0, sizeof e);
  e.duration_ms = duration_ms;
  e.delay_start_ms = delay_ms;
  e.delay_end_ms = end_delay_ms;
  e.iteration_count = count;
  e.fill = fill;
  e.direction = dir;
  e.timeline_auto = true;
  return e;
}

GTEXT_CSS_Anim_Timing at(const GTEXT_CSS_Anim_Effect & e, double local) {
  GTEXT_CSS_Anim_Timing t;
  gtext_css_anim_timing(&e, local, &t);
  return t;
}

/* Web Animations 1: "An animation effect is in the before phase if the animation effect's local time is not unresolved and ... the local time is less than the before-active
   boundary time" and "in the after phase if ... the local time is greater than the active-after boundary time, or the animation direction is 'forwards', the endpoint-inclusive
   active interval flag is false, and the local time is equal to the active-after boundary time". */
TEST(CssAnimationTiming, TheThreePhasesAndTheirBoundaries) {
  const GTEXT_CSS_Anim_Effect e = effect(2000, 1000, 1, GTEXT_CSS_ANIM_FILL_BOTH);
  EXPECT_EQ(at(e, 500).phase, GTEXT_CSS_ANIM_PHASE_BEFORE);
  EXPECT_EQ(at(e, 999.5).phase, GTEXT_CSS_ANIM_PHASE_BEFORE);
  EXPECT_EQ(at(e, 1000).phase, GTEXT_CSS_ANIM_PHASE_ACTIVE) << "the start of the active interval is in it";
  EXPECT_EQ(at(e, 2999).phase, GTEXT_CSS_ANIM_PHASE_ACTIVE);
  EXPECT_EQ(at(e, 3000).phase, GTEXT_CSS_ANIM_PHASE_AFTER) << "the end is not";
  EXPECT_EQ(at(e, 9000).phase, GTEXT_CSS_ANIM_PHASE_AFTER);
}

/* "Calculating the active time": the before phase is "max(local time - start delay, 0)" with a backwards or both fill mode and unresolved otherwise; the after phase is
   "max(min(local time - start delay, active duration), 0)" with a forwards or both fill mode. */
TEST(CssAnimationTiming, TheFillModeDecidesWhetherAnEffectOutsideItsActiveIntervalApplies) {
  struct Case {
    GTEXT_CSS_Anim_Fill fill;
    bool before, after;
  };
  for (const Case & c : {Case{GTEXT_CSS_ANIM_FILL_NONE, false, false}, Case{GTEXT_CSS_ANIM_FILL_FORWARDS, false, true}, Case{GTEXT_CSS_ANIM_FILL_BACKWARDS, true, false},
           Case{GTEXT_CSS_ANIM_FILL_BOTH, true, true}}) {
    const GTEXT_CSS_Anim_Effect e = effect(2000, 1000, 1, c.fill);
    EXPECT_EQ(at(e, 500).in_effect, c.before) << c.fill;
    EXPECT_EQ(at(e, 1500).in_effect, true) << c.fill;
    EXPECT_EQ(at(e, 4000).in_effect, c.after) << c.fill;
  }
  const GTEXT_CSS_Anim_Effect both = effect(2000, 1000, 1, GTEXT_CSS_ANIM_FILL_BOTH);
  EXPECT_DOUBLE_EQ(at(both, 200).active_time_ms, 0.0);
  EXPECT_DOUBLE_EQ(at(both, 200).progress, 0.0);
  EXPECT_DOUBLE_EQ(at(both, 9000).active_time_ms, 2000.0);
  EXPECT_DOUBLE_EQ(at(both, 9000).progress, 1.0) << "an active interval that ends at the end of an iteration fills by holding the endpoint of the final iteration";
}

/* Animations 1, animation-delay: "A negative delay is valid. ... it means that the animation executes immediately, but is automatically progressed by the absolute value of the delay". */
TEST(CssAnimationTiming, ANegativeDelayStartsTheAnimationPartWayThrough) {
  const GTEXT_CSS_Anim_Effect e = effect(10000, -5000);
  const GTEXT_CSS_Anim_Timing t = at(e, 0);
  EXPECT_EQ(t.phase, GTEXT_CSS_ANIM_PHASE_ACTIVE);
  EXPECT_TRUE(t.in_effect);
  EXPECT_DOUBLE_EQ(t.active_time_ms, 5000.0);
  EXPECT_DOUBLE_EQ(t.progress, 0.5);
  EXPECT_DOUBLE_EQ(at(e, 2500).progress, 0.75);
}

/* "If the iteration count is not an integer, the animation will end partway through its last cycle." Overall progress is active time over iteration duration; the simple
   iteration progress is its fractional part; the current iteration index is its floor. */
TEST(CssAnimationTiming, AFractionalCountEndsPartWayThroughTheLastIteration) {
  const GTEXT_CSS_Anim_Effect e = effect(1000, 0, 2.5, GTEXT_CSS_ANIM_FILL_FORWARDS);
  const GTEXT_CSS_Anim_Timing mid = at(e, 1250);
  EXPECT_DOUBLE_EQ(mid.overall_progress, 1.25);
  EXPECT_DOUBLE_EQ(mid.simple_progress, 0.25);
  EXPECT_DOUBLE_EQ(mid.iteration_index, 1.0);
  const GTEXT_CSS_Anim_Timing end = at(e, 5000);
  EXPECT_EQ(end.phase, GTEXT_CSS_ANIM_PHASE_AFTER);
  EXPECT_DOUBLE_EQ(end.overall_progress, 2.5);
  EXPECT_DOUBLE_EQ(end.progress, 0.5);
  EXPECT_DOUBLE_EQ(end.iteration_index, 2.0);
}

/* "when an animation's active interval ends precisely at the end of an iteration, it fills by holding the endpoint of the final iteration rather than the start of the next":
   simple iteration progress 1.0 and the index floor(overall progress) - 1. */
TEST(CssAnimationTiming, AnEndOnAnIterationBoundaryHoldsTheEndpointOfTheFinalIteration) {
  const GTEXT_CSS_Anim_Effect e = effect(1000, 0, 3, GTEXT_CSS_ANIM_FILL_FORWARDS);
  const GTEXT_CSS_Anim_Timing t = at(e, 7000);
  EXPECT_DOUBLE_EQ(t.simple_progress, 1.0);
  EXPECT_DOUBLE_EQ(t.iteration_index, 2.0);
  // in the middle of the animation, the start of an iteration is the start of the next one
  const GTEXT_CSS_Anim_Timing b = at(e, 2000);
  EXPECT_DOUBLE_EQ(b.simple_progress, 0.0);
  EXPECT_DOUBLE_EQ(b.iteration_index, 2.0);
}

/* Directed progress: normal and reverse; alternate plays "odd counts normal"; alternate-reverse the opposite ("iterations start counting from 1" in Animations 1, index from 0 here). */
TEST(CssAnimationTiming, TheDirectionTurnsTheProgressAround) {
  auto progress = [](GTEXT_CSS_Anim_Direction d, double local) { return at(effect(1000, 0, 4, GTEXT_CSS_ANIM_FILL_NONE, d), local).progress; };
  EXPECT_DOUBLE_EQ(progress(GTEXT_CSS_ANIM_DIRECTION_NORMAL, 250), 0.25);
  EXPECT_DOUBLE_EQ(progress(GTEXT_CSS_ANIM_DIRECTION_REVERSE, 250), 0.75);
  EXPECT_DOUBLE_EQ(progress(GTEXT_CSS_ANIM_DIRECTION_ALTERNATE, 250), 0.25);
  EXPECT_DOUBLE_EQ(progress(GTEXT_CSS_ANIM_DIRECTION_ALTERNATE, 1250), 0.75);
  EXPECT_DOUBLE_EQ(progress(GTEXT_CSS_ANIM_DIRECTION_ALTERNATE, 2250), 0.25);
  EXPECT_DOUBLE_EQ(progress(GTEXT_CSS_ANIM_DIRECTION_ALTERNATE_REVERSE, 250), 0.75);
  EXPECT_DOUBLE_EQ(progress(GTEXT_CSS_ANIM_DIRECTION_ALTERNATE_REVERSE, 1250), 0.25);
  EXPECT_TRUE(at(effect(1000, 0, 4, GTEXT_CSS_ANIM_FILL_NONE, GTEXT_CSS_ANIM_DIRECTION_ALTERNATE), 250).forwards);
  EXPECT_FALSE(at(effect(1000, 0, 4, GTEXT_CSS_ANIM_FILL_NONE, GTEXT_CSS_ANIM_DIRECTION_ALTERNATE), 1250).forwards);
}

/* "Calculating the transformed progress": the before flag is set in the before phase when going forwards, and in the after phase when going backwards. */
TEST(CssAnimationTiming, TheBeforeFlag) {
  EXPECT_TRUE(at(effect(1000, 500, 1, GTEXT_CSS_ANIM_FILL_BOTH), 100).before_flag);
  EXPECT_FALSE(at(effect(1000, 500, 1, GTEXT_CSS_ANIM_FILL_BOTH), 600).before_flag);
  EXPECT_FALSE(at(effect(1000, 0, 1, GTEXT_CSS_ANIM_FILL_BOTH), 5000).before_flag);
  EXPECT_TRUE(at(effect(1000, 0, 1, GTEXT_CSS_ANIM_FILL_BOTH, GTEXT_CSS_ANIM_DIRECTION_REVERSE), 5000).before_flag);
  EXPECT_FALSE(at(effect(1000, 500, 1, GTEXT_CSS_ANIM_FILL_BOTH, GTEXT_CSS_ANIM_DIRECTION_REVERSE), 100).before_flag);
}

/* "If the iteration duration is zero, if the animation effect is in the before phase, let overall progress be zero; otherwise, let it be equal to the iteration count."
   Animations 1: a duration of 0s "occurs instantaneously for any valid value of 'animation-iteration-count', including infinite". */
TEST(CssAnimationTiming, AZeroDurationIsInstantaneous) {
  GTEXT_CSS_Anim_Effect e = effect(0, 1000, 2, GTEXT_CSS_ANIM_FILL_BOTH);
  const GTEXT_CSS_Anim_Timing before = at(e, 500);
  EXPECT_TRUE(before.in_effect);
  EXPECT_DOUBLE_EQ(before.progress, 0.0);
  const GTEXT_CSS_Anim_Timing after = at(e, 1000);
  EXPECT_EQ(after.phase, GTEXT_CSS_ANIM_PHASE_AFTER);
  EXPECT_TRUE(after.in_effect);
  EXPECT_DOUBLE_EQ(after.overall_progress, 2.0);
  EXPECT_DOUBLE_EQ(after.progress, 1.0);
  e = effect(0, 0, 0, GTEXT_CSS_ANIM_FILL_FORWARDS);
  e.infinite = true;
  const GTEXT_CSS_Anim_Timing inf = at(e, 10);
  EXPECT_TRUE(inf.in_effect);
  EXPECT_TRUE(inf.overall_infinite);
  EXPECT_DOUBLE_EQ(inf.progress, 1.0);
  e.fill = GTEXT_CSS_ANIM_FILL_NONE;
  EXPECT_FALSE(at(e, 10).in_effect) << "an instantaneous animation with no fill has no visible effect";
}

TEST(CssAnimationTiming, AnInfiniteCountNeverEnds) {
  GTEXT_CSS_Anim_Effect e = effect(1000, 0, 0, GTEXT_CSS_ANIM_FILL_NONE, GTEXT_CSS_ANIM_DIRECTION_ALTERNATE);
  e.infinite = true;
  const GTEXT_CSS_Anim_Timing t = at(e, 1e9 + 250);
  EXPECT_EQ(t.phase, GTEXT_CSS_ANIM_PHASE_ACTIVE);
  EXPECT_TRUE(t.in_effect);
  EXPECT_NEAR(t.simple_progress, 0.25, 1e-3);
  EXPECT_FALSE(t.iteration_infinite);
}

/* "If either the iteration duration or iteration count are zero, the active duration is zero": an iteration count of 0 plays no iteration. */
TEST(CssAnimationTiming, AZeroCountPlaysNoIteration) {
  const GTEXT_CSS_Anim_Effect e = effect(1000, 0, 0, GTEXT_CSS_ANIM_FILL_FORWARDS);
  const GTEXT_CSS_Anim_Timing t = at(e, 10);
  EXPECT_EQ(t.phase, GTEXT_CSS_ANIM_PHASE_AFTER);
  EXPECT_DOUBLE_EQ(t.progress, 0.0) << "the simple iteration progress is not held at 1 when no iteration was played";
}

/* "The end time of an animation effect is max(start delay + active duration + end delay, 0)", and "active-after boundary time" is that without the end delay: the end delay is
   in the after phase. */
TEST(CssAnimationTiming, TheEndDelayIsInTheAfterPhase) {
  const GTEXT_CSS_Anim_Effect e = effect(1000, 0, 1, GTEXT_CSS_ANIM_FILL_FORWARDS, GTEXT_CSS_ANIM_DIRECTION_NORMAL, 2000);
  EXPECT_EQ(at(e, 999).phase, GTEXT_CSS_ANIM_PHASE_ACTIVE);
  EXPECT_EQ(at(e, 1500).phase, GTEXT_CSS_ANIM_PHASE_AFTER);
  EXPECT_DOUBLE_EQ(at(e, 1500).progress, 1.0);
  // a negative end delay brings the end time before the end of the active interval
  const GTEXT_CSS_Anim_Effect n = effect(1000, 0, 1, GTEXT_CSS_ANIM_FILL_FORWARDS, GTEXT_CSS_ANIM_DIRECTION_NORMAL, -400);
  EXPECT_EQ(at(n, 700).phase, GTEXT_CSS_ANIM_PHASE_AFTER);
}

/* ---- easing -------------------------------------------------------------------------------------------------------------------------------------- */

struct Ease {
  Context ctx{R()};
  Block block;
  Style style;
  GTEXT_CSS_Anim_List * list = nullptr;
  explicit Ease(const std::string & timing) : block(R(), "animation-name: a; animation-timing-function: " + timing) {
    style.compute(ctx, &block);
    EXPECT_EQ(gtext_css_anim_list_from_style(style.s, nullptr, &list), GTEXT_CSS_OK);
  }
  Ease(const Ease &) = delete;
  Ease & operator=(const Ease &) = delete;
  ~Ease() {
    gtext_css_anim_list_free(list);
  }
  double operator()(double x, bool before = false) const {
    GTEXT_CSS_Anim_Effect e;
    EXPECT_TRUE(gtext_css_anim_list_at(list, 0, &e));
    return gtext_css_easing_output(e.easing, x, before);
  }
};

/* Easing 1: the keyword `ease` is cubic-bezier(0.25, 0.1, 0.25, 1.0); `linear` is the identity. */
TEST(CssEasingOutput, TheCubicBezierKeywordsAndTheLinearKeyword) {
  EXPECT_DOUBLE_EQ(Ease("linear")(0.3), 0.3);
  const Ease ease("ease");
  EXPECT_DOUBLE_EQ(ease(0.0), 0.0);
  EXPECT_DOUBLE_EQ(ease(1.0), 1.0);
  EXPECT_NEAR(ease(0.5), 0.8024033, 1e-6);
  EXPECT_NEAR(Ease("ease-in")(0.5), 0.3153, 1e-3);
  EXPECT_NEAR(Ease("ease-out")(0.5), 0.6847, 1e-3);
  EXPECT_NEAR(Ease("ease-in-out")(0.5), 0.5, 1e-9);
  EXPECT_NEAR(Ease("cubic-bezier(0.1, 0.7, 1.0, 0.1)")(0.3), 0.3691563, 1e-6) << "bisection of the curve, computed apart";
}

/* Easing 1: "For input progress values outside the range [0, 1], the curve is extended infinitely using tangent of the curve at the closest endpoint": below 0 the line through P1
   and P0 when x1 > 0, else the line through P2 and P0 when x2 > 0, else 0; above 1 the line through P2 and P3 when x2 < 1, else the line through P1 and P3 when x1 < 1, else 1. */
TEST(CssEasingOutput, ACubicBezierIsExtendedAlongItsTangents) {
  const Ease c("cubic-bezier(0.5, 1, 0.5, 2)");
  EXPECT_NEAR(c(-0.5), -0.5 * (1.0 / 0.5), 1e-9) << "P1 and P0";
  EXPECT_NEAR(c(1.5), 1.0 + 0.5 * ((1.0 - 2.0) / (1.0 - 0.5)), 1e-9) << "P2 and P3";
  const Ease x1zero("cubic-bezier(0, 0, 0.58, 1)");
  EXPECT_NEAR(x1zero(-0.5), -0.5 * (1.0 / 0.58), 1e-9) << "x1 is 0, so P2 and P0";
  EXPECT_DOUBLE_EQ(x1zero(2.0), 1.0) << "x2 < 1 and y2 is 1: the tangent is flat";
  EXPECT_DOUBLE_EQ(Ease("cubic-bezier(0, 5, 0, 5)")(-1.0), 0.0);
  EXPECT_DOUBLE_EQ(Ease("cubic-bezier(1, 5, 1, 5)")(3.0), 1.0);
}

/* Easing 1, step functions: current step = floor(input x steps); jump-start and jump-both add one; "If both the before flag is set and input x steps is integral" decrement; a step
   below zero is zero for an input of at least 0; the step is at most the number of jumps for an input of at most 1; the output is the step over the jumps. */
TEST(CssEasingOutput, TheStepFunctions) {
  const Ease end("steps(4, jump-end)");
  EXPECT_DOUBLE_EQ(end(0.0), 0.0);
  EXPECT_DOUBLE_EQ(end(0.3), 0.25);
  EXPECT_DOUBLE_EQ(end(0.99), 0.75);
  EXPECT_DOUBLE_EQ(end(1.0), 1.0);
  const Ease start("steps(4, jump-start)");
  EXPECT_DOUBLE_EQ(start(0.0), 0.25);
  EXPECT_DOUBLE_EQ(start(0.3), 0.5);
  EXPECT_DOUBLE_EQ(start(1.0), 1.0) << "capped at the number of jumps";
  const Ease none("steps(3, jump-none)");
  EXPECT_DOUBLE_EQ(none(0.0), 0.0);
  EXPECT_DOUBLE_EQ(none(0.5), 0.5);
  EXPECT_DOUBLE_EQ(none(1.0), 1.0);
  const Ease both("steps(2, jump-both)");
  EXPECT_DOUBLE_EQ(both(0.0), 1.0 / 3.0);
  EXPECT_DOUBLE_EQ(both(0.5), 2.0 / 3.0);
  EXPECT_DOUBLE_EQ(both(1.0), 1.0);
  EXPECT_DOUBLE_EQ(Ease("step-start")(0.2), 1.0);
  EXPECT_DOUBLE_EQ(Ease("step-end")(0.9), 0.0);
}

TEST(CssEasingOutput, TheBeforeFlagLowersAStepAtAnIntegralInput) {
  const Ease start("steps(2, jump-start)");
  EXPECT_DOUBLE_EQ(start(0.0, false), 0.5);
  EXPECT_DOUBLE_EQ(start(0.0, true), 0.0) << "during a backwards fill the first step is not yet taken";
  EXPECT_DOUBLE_EQ(start(0.5, true), 0.5);
  EXPECT_DOUBLE_EQ(start(0.25, true), 0.5) << "not integral: unchanged";
  const Ease end("steps(2, jump-end)");
  EXPECT_DOUBLE_EQ(end(0.5, false), 0.5);
  EXPECT_DOUBLE_EQ(end(0.5, true), 0.0);
}

/* Easing 2, "calculate linear easing output progress": a single point; the last point at an input that matches (the first, with the before flag); the line through the nearest two
   points; "If inputProgress is smaller than any input progress value in points, let A and B be the first two items", the last two for a larger one. */
TEST(CssEasingOutput, TheLinearFunctionOfPoints) {
  const Ease l("linear(0, 0.5 20%, 1)");
  EXPECT_DOUBLE_EQ(l(0.0), 0.0);
  EXPECT_DOUBLE_EQ(l(0.2), 0.5);
  EXPECT_DOUBLE_EQ(l(0.1), 0.25);
  EXPECT_DOUBLE_EQ(l(0.6), 0.75);
  EXPECT_DOUBLE_EQ(l(1.0), 1.0);
  EXPECT_DOUBLE_EQ(l(-1.0), -2.5) << "the first two points";
  EXPECT_DOUBLE_EQ(l(2.0), 1.0 + 1.0 * (0.5 / 0.8)) << "the last two points";
  const Ease jump("linear(0, 0.2 50%, 0.8 50%, 1)");
  EXPECT_DOUBLE_EQ(jump(0.5, false), 0.8) << "the last point at that input";
  EXPECT_DOUBLE_EQ(Ease("linear(0 0%, 1 0%)")(0.0, true), 0.0) << "the before flag takes the first point";
}

}  // namespace
