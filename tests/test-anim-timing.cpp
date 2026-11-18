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
 * The timing of an animation effect as a value (animation/timing.h), against Web Animations 1: "Animation effect phases and states" (with a playback rate and the endpoint-inclusive flag),
 * "Calculating the active time", "Calculating the overall progress" (with an iteration start), "Calculating the simple iteration progress", "Calculating the current iteration index",
 * "Calculating the directed progress", "Calculating the transformed progress", "Updating the timing of an AnimationEffect" and the dictionaries EffectTiming and ComputedEffectTiming; and
 * the general model held to the timing model of the effects of CSS Animations (css_animation.h) on every input that one takes.
 */

#include <cmath>
#include <cstring>
#include <limits>
#include <string>

#include <gtest/gtest.h>

#include "css_compute_test_support.h"

#include <ghoti.io/text/animation/timing.h>

using namespace cmp;

namespace {

const double kInf = std::numeric_limits<double>::infinity();

GTEXT_Anim_Time T(double ms) {
  return GTEXT_Anim_Time{true, ms};
}

const GTEXT_Anim_Time kUnresolved{false, 0};

GTEXT_Anim_Effect_Timing timing(double duration, double delay = 0, double iterations = 1, GTEXT_Anim_Fill fill = GTEXT_ANIM_FILL_NONE,
    GTEXT_Anim_Direction dir = GTEXT_ANIM_DIRECTION_NORMAL, double end_delay = 0, double iteration_start = 0) {
  GTEXT_Anim_Effect_Timing t = gtext_anim_timing_default();
  t.duration_auto = false;
  t.duration = duration;
  t.delay = delay;
  t.iterations = iterations;
  t.fill = fill;
  t.direction = dir;
  t.end_delay = end_delay;
  t.iteration_start = iteration_start;
  return t;
}

GTEXT_Anim_Computed_Timing at(const GTEXT_Anim_Effect_Timing & t, GTEXT_Anim_Time local, double rate = 1.0, bool inclusive = false) {
  GTEXT_Anim_Computed_Timing out;
  EXPECT_EQ(gtext_anim_computed_timing(&t, local, rate, inclusive, &out), GTEXT_ANIM_OK);
  return out;
}

/* "The defaults of EffectTiming": delay 0, endDelay 0, fill "auto", iterationStart 0, iterations 1, duration "auto", direction "normal", easing "linear". */
TEST(AnimTiming, TheDefaultsAreThoseOfEffectTiming) {
  const GTEXT_Anim_Effect_Timing t = gtext_anim_timing_default();
  EXPECT_EQ(t.delay, 0);
  EXPECT_EQ(t.end_delay, 0);
  EXPECT_EQ(t.fill, GTEXT_ANIM_FILL_AUTO);
  EXPECT_EQ(t.iteration_start, 0);
  EXPECT_EQ(t.iterations, 1);
  EXPECT_TRUE(t.duration_auto);
  EXPECT_EQ(t.direction, GTEXT_ANIM_DIRECTION_NORMAL);
  EXPECT_EQ(t.easing, nullptr);
  EXPECT_EQ(gtext_anim_timing_validate(&t), GTEXT_ANIM_OK);
}

/* "ComputedEffectTiming": fill auto is "none" and the string auto is the number 0 in what getComputedTiming() returns; the end time is max(start delay + active duration + end delay, 0). */
TEST(AnimTiming, ComputedTimingExpandsAutoAndFillsTheEndTimes) {
  const GTEXT_Anim_Computed_Timing c = at(gtext_anim_timing_default(), T(0));
  EXPECT_EQ(c.timing.fill, GTEXT_ANIM_FILL_NONE);
  EXPECT_FALSE(c.timing.duration_auto);
  EXPECT_EQ(c.timing.duration, 0);
  EXPECT_EQ(c.active_duration, 0);
  EXPECT_EQ(c.end_time, 0);
  const GTEXT_Anim_Computed_Timing d = at(timing(2000, 500, 3, GTEXT_ANIM_FILL_NONE, GTEXT_ANIM_DIRECTION_NORMAL, 250), T(0));
  EXPECT_EQ(d.active_duration, 6000);
  EXPECT_EQ(d.end_time, 6750);
  EXPECT_EQ(at(timing(1000, -5000, 1, GTEXT_ANIM_FILL_NONE, GTEXT_ANIM_DIRECTION_NORMAL, 0), T(0)).end_time, 0) << "the end time is not negative";
}

/* "If the local time is unresolved, the effect is in none of the phases": the active time, the overall progress and every progress after it are unresolved (null). */
TEST(AnimTiming, AnUnresolvedLocalTimeIsInNoPhase) {
  const GTEXT_Anim_Computed_Timing c = at(timing(1000, 0, 1, GTEXT_ANIM_FILL_BOTH), kUnresolved);
  EXPECT_EQ(c.phase, GTEXT_ANIM_PHASE_IDLE);
  EXPECT_FALSE(c.local_time.resolved);
  EXPECT_FALSE(c.in_effect);
  EXPECT_FALSE(c.has_progress);
  EXPECT_FALSE(c.has_current_iteration);
  EXPECT_EQ(c.end_time, 1000) << "the end time is a property of the timing and does not need a time";
}

/* "An animation effect is in the before phase if ... the local time is less than the before-active boundary time, or the animation direction is backwards, the endpoint-inclusive flag
   is false and the local time is equal to it", and "in the after phase if ... greater than the active-after boundary time, or forwards, the flag false and equal". */
TEST(AnimTiming, TheBoundariesBelongToTheSideTheDirectionSays) {
  const GTEXT_Anim_Effect_Timing t = timing(2000, 1000, 1, GTEXT_ANIM_FILL_BOTH);
  EXPECT_EQ(at(t, T(1000)).phase, GTEXT_ANIM_PHASE_ACTIVE) << "forwards: the start of the active interval is in it";
  EXPECT_EQ(at(t, T(3000)).phase, GTEXT_ANIM_PHASE_AFTER) << "forwards: the end is not";
  EXPECT_EQ(at(t, T(1000), -1.0).phase, GTEXT_ANIM_PHASE_BEFORE) << "backwards: the start is not";
  EXPECT_EQ(at(t, T(3000), -1.0).phase, GTEXT_ANIM_PHASE_ACTIVE) << "backwards: the end is";
  EXPECT_EQ(at(t, T(1000), -1.0, true).phase, GTEXT_ANIM_PHASE_ACTIVE) << "inclusive: both ends are in the active interval";
  EXPECT_EQ(at(t, T(3000), 1.0, true).phase, GTEXT_ANIM_PHASE_ACTIVE);
  EXPECT_EQ(at(t, T(999.5)).phase, GTEXT_ANIM_PHASE_BEFORE);
  EXPECT_EQ(at(t, T(3000.5), -1.0).phase, GTEXT_ANIM_PHASE_AFTER);
  EXPECT_EQ(at(t, T(2000), 0.0).phase, GTEXT_ANIM_PHASE_ACTIVE) << "a playback rate of zero is not negative: forwards";
}

/* A negative end delay shortens the end time below the active interval: the after phase starts at the end time ("or after the end time if that comes first"). */
TEST(AnimTiming, ANegativeEndDelayBringsTheAfterPhaseForward) {
  const GTEXT_Anim_Effect_Timing t = timing(1000, 0, 1, GTEXT_ANIM_FILL_FORWARDS, GTEXT_ANIM_DIRECTION_NORMAL, -400);
  EXPECT_EQ(at(t, T(599)).phase, GTEXT_ANIM_PHASE_ACTIVE);
  EXPECT_EQ(at(t, T(600)).phase, GTEXT_ANIM_PHASE_AFTER);
  const GTEXT_Anim_Computed_Timing c = at(t, T(900));
  EXPECT_EQ(c.phase, GTEXT_ANIM_PHASE_AFTER);
  EXPECT_EQ(c.active_time, 900) << "the active time of the after phase is min(local - delay, active duration): the end delay does not cut it";
  /* and a negative start delay is in effect before the local time reaches zero, so a time before the interval is in the before phase though the interval began */
  const GTEXT_Anim_Effect_Timing n = timing(1000, -300, 1, GTEXT_ANIM_FILL_BOTH);
  EXPECT_EQ(at(n, T(0)).phase, GTEXT_ANIM_PHASE_ACTIVE);
  EXPECT_EQ(at(n, T(0)).active_time, 300);
}

/* "Calculating the active time": the before phase is max(local time - start delay, 0) with a backwards or both fill and unresolved otherwise; the after phase is
   max(min(local time - start delay, active duration), 0) with a forwards or both fill. */
TEST(AnimTiming, TheFillModeDecidesWhetherAnEffectOutsideItsIntervalApplies) {
  struct Case {
    GTEXT_Anim_Fill fill;
    bool before, after;
  };
  for (const Case & c : {Case{GTEXT_ANIM_FILL_AUTO, false, false}, Case{GTEXT_ANIM_FILL_NONE, false, false}, Case{GTEXT_ANIM_FILL_FORWARDS, false, true},
           Case{GTEXT_ANIM_FILL_BACKWARDS, true, false}, Case{GTEXT_ANIM_FILL_BOTH, true, true}}) {
    const GTEXT_Anim_Effect_Timing t = timing(2000, 1000, 1, c.fill);
    const GTEXT_Anim_Computed_Timing b = at(t, T(400));
    EXPECT_EQ(b.in_effect, c.before) << "fill " << c.fill << " before";
    if (c.before) {
      EXPECT_EQ(b.active_time, 0);
    }
    const GTEXT_Anim_Computed_Timing a = at(t, T(9000));
    EXPECT_EQ(a.in_effect, c.after) << "fill " << c.fill << " after";
    if (c.after) {
      EXPECT_EQ(a.active_time, 2000) << "held at the active duration";
    }
  }
}

/* "Calculating the overall progress ... + iteration start", "the simple iteration progress is overall progress % 1.0" and "the current iteration index is floor(overall progress)". */
TEST(AnimTiming, TheIterationStartMovesTheProgressAndTheIterationIndex) {
  const GTEXT_Anim_Effect_Timing t = timing(1000, 0, 2, GTEXT_ANIM_FILL_BOTH, GTEXT_ANIM_DIRECTION_NORMAL, 0, 0.5);
  const GTEXT_Anim_Computed_Timing c = at(t, T(250));
  EXPECT_DOUBLE_EQ(c.overall_progress, 0.75);
  EXPECT_DOUBLE_EQ(c.simple_progress, 0.75);
  EXPECT_EQ(c.current_iteration, 0);
  const GTEXT_Anim_Computed_Timing d = at(t, T(750));
  EXPECT_DOUBLE_EQ(d.overall_progress, 1.25);
  EXPECT_DOUBLE_EQ(d.simple_progress, 0.25);
  EXPECT_EQ(d.current_iteration, 1);
  /* the effect "will still repeat twice" and ends halfway through its iteration interval: the end of the active interval is at overall progress 2.5 */
  const GTEXT_Anim_Computed_Timing e = at(t, T(5000));
  EXPECT_DOUBLE_EQ(e.overall_progress, 2.5);
  EXPECT_DOUBLE_EQ(e.simple_progress, 0.5);
  EXPECT_EQ(e.current_iteration, 2);
  /* an iteration start of 1.2 begins 20% of the way through the second iteration */
  const GTEXT_Anim_Computed_Timing f = at(timing(1000, 0, 1, GTEXT_ANIM_FILL_BOTH, GTEXT_ANIM_DIRECTION_NORMAL, 0, 1.2), T(0));
  EXPECT_NEAR(f.simple_progress, 0.2, 1e-12);
  EXPECT_EQ(f.current_iteration, 1);
}

/* "If the simple iteration progress is zero, in the active or after phase, the active time equals the active duration and the iteration count is not zero, it is 1.0": an interval that
   ends exactly at the end of an iteration holds the end of the last iteration; the index is floor(overall progress) - 1. */
TEST(AnimTiming, AnIntervalEndingOnAnIterationBoundaryHoldsTheEndOfTheLastIteration) {
  const GTEXT_Anim_Effect_Timing t = timing(1000, 0, 3, GTEXT_ANIM_FILL_FORWARDS);
  const GTEXT_Anim_Computed_Timing c = at(t, T(3000));
  EXPECT_EQ(c.phase, GTEXT_ANIM_PHASE_AFTER);
  EXPECT_EQ(c.simple_progress, 1.0);
  EXPECT_EQ(c.current_iteration, 2);
  EXPECT_EQ(c.progress, 1.0);
  /* in the middle the boundary is the start of the next iteration */
  const GTEXT_Anim_Computed_Timing m = at(t, T(2000));
  EXPECT_EQ(m.simple_progress, 0.0);
  EXPECT_EQ(m.current_iteration, 2);
  /* and an effect of no iterations never played one: it does not hold 1.0 */
  const GTEXT_Anim_Computed_Timing z = at(timing(1000, 0, 0, GTEXT_ANIM_FILL_FORWARDS), T(10));
  EXPECT_EQ(z.simple_progress, 0.0);
  EXPECT_EQ(z.active_duration, 0);
}

/* "If the iteration duration is zero, ... overall progress is zero in the before phase and the iteration count otherwise." */
TEST(AnimTiming, AZeroDurationEffectIsItsIterationCountAfterItsStart) {
  const GTEXT_Anim_Effect_Timing t = timing(0, 100, 4, GTEXT_ANIM_FILL_BOTH);
  const GTEXT_Anim_Computed_Timing b = at(t, T(50));
  EXPECT_EQ(b.phase, GTEXT_ANIM_PHASE_BEFORE);
  EXPECT_EQ(b.overall_progress, 0);
  const GTEXT_Anim_Computed_Timing a = at(t, T(100));
  EXPECT_EQ(a.phase, GTEXT_ANIM_PHASE_AFTER);
  EXPECT_EQ(a.overall_progress, 4);
  EXPECT_EQ(a.current_iteration, 3);
  EXPECT_EQ(a.simple_progress, 1.0);
  /* infinite repeats of nothing finish at once, and "for a zero-duration animation that repeats infinite times, the value will be positive Infinity" */
  const GTEXT_Anim_Computed_Timing i = at(timing(0, 0, kInf, GTEXT_ANIM_FILL_FORWARDS), T(0));
  EXPECT_EQ(i.phase, GTEXT_ANIM_PHASE_AFTER);
  EXPECT_EQ(i.current_iteration, kInf);
  EXPECT_EQ(i.active_duration, 0) << "infinity times zero is zero by the definition of the active duration";
}

/* An infinite count of a non-zero duration has an infinite active duration, and end time: the effect never leaves its active phase. */
TEST(AnimTiming, AnInfiniteCountNeverEnds) {
  const GTEXT_Anim_Effect_Timing t = timing(500, 100, kInf, GTEXT_ANIM_FILL_FORWARDS);
  const GTEXT_Anim_Computed_Timing c = at(t, T(1e9));
  EXPECT_EQ(c.active_duration, kInf);
  EXPECT_EQ(c.end_time, kInf);
  EXPECT_EQ(c.phase, GTEXT_ANIM_PHASE_ACTIVE);
  EXPECT_EQ(c.current_iteration, std::floor((1e9 - 100) / 500));
  /* an infinite duration is one iteration that never completes */
  const GTEXT_Anim_Computed_Timing d = at(timing(kInf, 0, 1), T(12345));
  EXPECT_EQ(d.phase, GTEXT_ANIM_PHASE_ACTIVE);
  EXPECT_EQ(d.simple_progress, 0.0);
  EXPECT_EQ(d.end_time, kInf);
}

/* "Calculating the directed progress": reverse flips every iteration; alternate flips the odd ones; alternate-reverse the even ones; an infinite index is forwards. */
TEST(AnimTiming, TheDirectionFlipsTheIterationsItSays) {
  struct Case {
    GTEXT_Anim_Direction dir;
    bool first, second;
  };
  for (const Case & c : {Case{GTEXT_ANIM_DIRECTION_NORMAL, true, true}, Case{GTEXT_ANIM_DIRECTION_REVERSE, false, false}, Case{GTEXT_ANIM_DIRECTION_ALTERNATE, true, false},
           Case{GTEXT_ANIM_DIRECTION_ALTERNATE_REVERSE, false, true}}) {
    const GTEXT_Anim_Effect_Timing t = timing(1000, 0, 4, GTEXT_ANIM_FILL_BOTH, c.dir);
    const GTEXT_Anim_Computed_Timing a = at(t, T(250));
    const GTEXT_Anim_Computed_Timing b = at(t, T(1250));
    EXPECT_EQ(a.forwards, c.first) << c.dir;
    EXPECT_EQ(b.forwards, c.second) << c.dir;
    EXPECT_DOUBLE_EQ(a.directed_progress, c.first ? 0.25 : 0.75);
    EXPECT_DOUBLE_EQ(b.directed_progress, c.second ? 0.25 : 0.75);
  }
  const GTEXT_Anim_Computed_Timing inf = at(timing(0, 0, kInf, GTEXT_ANIM_FILL_FORWARDS, GTEXT_ANIM_DIRECTION_ALTERNATE_REVERSE), T(0));
  EXPECT_TRUE(inf.forwards) << "an infinite iteration index is forwards";
}

/* "Calculating the transformed progress": the effect easing function is evaluated at the directed progress, with the before flag set in the before phase going forwards and in the after
   phase going backwards. */
TEST(AnimTiming, TheEasingIsAppliedToTheDirectedProgressWithItsBeforeFlag) {
  GTEXT_CSS_Easing * steps = nullptr;
  ASSERT_EQ(gtext_anim_easing_parse(R(), "steps(2, jump-start)", 20, nullptr, &steps), GTEXT_ANIM_OK);
  GTEXT_Anim_Effect_Timing t = timing(1000, 0, 1, GTEXT_ANIM_FILL_BOTH);
  t.easing = steps;
  const GTEXT_Anim_Computed_Timing c = at(t, T(250));
  EXPECT_DOUBLE_EQ(c.progress, 0.5) << "steps(2, jump-start) at 0.25 is 0.5";
  EXPECT_FALSE(c.before_flag);
  const GTEXT_Anim_Computed_Timing b = at(timing(1000, 100, 1, GTEXT_ANIM_FILL_BACKWARDS), T(0));
  EXPECT_TRUE(b.before_flag);
  EXPECT_TRUE(b.has_progress);
  GTEXT_Anim_Effect_Timing r = timing(1000, 0, 1, GTEXT_ANIM_FILL_BOTH, GTEXT_ANIM_DIRECTION_REVERSE);
  EXPECT_TRUE(at(r, T(5000)).before_flag) << "after phase, going backwards";
  EXPECT_FALSE(at(r, T(-5)).before_flag) << "before phase, going backwards";
  gtext_css_easing_free(steps);
}

/* "Updating the timing of an AnimationEffect": iterationStart < 0, iterations < 0 or NaN, duration < 0 or NaN (and a string other than auto, which the binding refuses) are TypeErrors. */
TEST(AnimTiming, ValidateRefusesTheTypeErrorsOfTheDictionary) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  GTEXT_Anim_Effect_Timing t = gtext_anim_timing_default();
  t.iteration_start = -0.5;
  EXPECT_EQ(gtext_anim_timing_validate(&t), GTEXT_ANIM_TYPE_ERROR);
  t = gtext_anim_timing_default();
  t.iterations = -1;
  EXPECT_EQ(gtext_anim_timing_validate(&t), GTEXT_ANIM_TYPE_ERROR);
  t.iterations = nan;
  EXPECT_EQ(gtext_anim_timing_validate(&t), GTEXT_ANIM_TYPE_ERROR);
  t.iterations = kInf;
  EXPECT_EQ(gtext_anim_timing_validate(&t), GTEXT_ANIM_OK) << "Infinity is a valid iteration count";
  t.duration_auto = false;
  t.duration = -1;
  EXPECT_EQ(gtext_anim_timing_validate(&t), GTEXT_ANIM_TYPE_ERROR);
  t.duration = nan;
  EXPECT_EQ(gtext_anim_timing_validate(&t), GTEXT_ANIM_TYPE_ERROR);
  t.duration = kInf;
  EXPECT_EQ(gtext_anim_timing_validate(&t), GTEXT_ANIM_OK) << "Infinity is a valid duration";
  t.duration = 0;
  t.delay = kInf;
  EXPECT_EQ(gtext_anim_timing_validate(&t), GTEXT_ANIM_TYPE_ERROR) << "delay is a double, not an unrestricted double";
  t.delay = 0;
  t.end_delay = nan;
  EXPECT_EQ(gtext_anim_timing_validate(&t), GTEXT_ANIM_TYPE_ERROR);
  EXPECT_EQ(gtext_anim_timing_validate(nullptr), GTEXT_ANIM_INVALID);
}

/* "update the timing properties ... Assign each member that exists": members that do not exist keep their value; a failure assigns nothing. */
TEST(AnimTiming, UpdateAssignsTheMembersThatExistAndNothingOnAFailure) {
  GTEXT_Anim_Effect_Timing t = timing(1000, 10, 2, GTEXT_ANIM_FILL_FORWARDS);
  GTEXT_Anim_Optional_Timing in{};
  in.present = GTEXT_ANIM_TIMING_DELAY | GTEXT_ANIM_TIMING_DURATION | GTEXT_ANIM_TIMING_DIRECTION;
  in.values.delay = -20;
  in.values.duration_auto = true;
  in.values.direction = GTEXT_ANIM_DIRECTION_ALTERNATE;
  ASSERT_EQ(gtext_anim_timing_update(&t, &in), GTEXT_ANIM_OK);
  EXPECT_EQ(t.delay, -20);
  EXPECT_TRUE(t.duration_auto);
  EXPECT_EQ(t.duration, 0);
  EXPECT_EQ(t.direction, GTEXT_ANIM_DIRECTION_ALTERNATE);
  EXPECT_EQ(t.iterations, 2) << "a member that does not exist is unchanged";
  EXPECT_EQ(t.fill, GTEXT_ANIM_FILL_FORWARDS);
  /* a good member beside a bad one: nothing is assigned */
  const GTEXT_Anim_Effect_Timing before = t;
  GTEXT_Anim_Optional_Timing bad{};
  bad.present = GTEXT_ANIM_TIMING_DELAY | GTEXT_ANIM_TIMING_ITERATION_START;
  bad.values.delay = 500;
  bad.values.iteration_start = -1;
  EXPECT_EQ(gtext_anim_timing_update(&t, &bad), GTEXT_ANIM_TYPE_ERROR);
  EXPECT_EQ(std::memcmp(&t, &before, sizeof t), 0);
  /* an empty dictionary changes nothing */
  GTEXT_Anim_Optional_Timing none{};
  EXPECT_EQ(gtext_anim_timing_update(&t, &none), GTEXT_ANIM_OK);
  EXPECT_EQ(std::memcmp(&t, &before, sizeof t), 0);
  /* a number for a duration that was auto */
  GTEXT_Anim_Optional_Timing d{};
  d.present = GTEXT_ANIM_TIMING_DURATION;
  d.values.duration = 750;
  ASSERT_EQ(gtext_anim_timing_update(&t, &d), GTEXT_ANIM_OK);
  EXPECT_FALSE(t.duration_auto);
  EXPECT_EQ(t.duration, 750);
  EXPECT_EQ(gtext_anim_timing_update(nullptr, &d), GTEXT_ANIM_INVALID);
}

/* "cannot be parsed using the <easing-function> production" is a TypeError: a keyword of another property, a list, a steps(0), a cubic-bezier with an x outside 0 to 1. */
TEST(AnimTiming, EasingParseTakesOneEasingFunction) {
  struct Case {
    const char * text;
    bool ok;
  };
  for (const Case & c : {Case{"linear", true}, Case{"ease-in-out", true}, Case{"cubic-bezier(0.1, 0.7, 1, 0.1)", true}, Case{"steps(4, end)", true}, Case{"linear(0, 0.25 75%, 1)", true},
           Case{"  ease  ", true}, Case{"ease, linear", false}, Case{"steps(0)", false}, Case{"cubic-bezier(2, 0, 1, 1)", false}, Case{"inherit", false}, Case{"foo", false},
           Case{"", false}, Case{"1s", false}}) {
    GTEXT_CSS_Easing * e = nullptr;
    const GTEXT_Anim_Status st = gtext_anim_easing_parse(R(), c.text, std::strlen(c.text), nullptr, &e);
    EXPECT_EQ(st == GTEXT_ANIM_OK, c.ok) << "'" << c.text << "' status " << st;
    EXPECT_EQ(e != nullptr, c.ok) << c.text;
    if (!c.ok) {
      EXPECT_EQ(st, GTEXT_ANIM_TYPE_ERROR) << c.text;
    }
    gtext_css_easing_free(e);
  }
  GTEXT_CSS_Easing * e = nullptr;
  EXPECT_EQ(gtext_anim_easing_parse(nullptr, "linear", 6, nullptr, &e), GTEXT_ANIM_INVALID);
}

/* The model of the effects of CSS Animations (a playback rate of 1, an iteration start of 0, no end delay but the one the animation-delay-end longhand names) and the general one are the
   same arithmetic: they agree on the phase, the active time, the overall progress, the iteration index and the progress for every input the older takes. A grid, not a sample. */
TEST(AnimTiming, TheGeneralModelAgreesWithTheModelOfCssAnimationsEverywhere) {
  const double durations[] = {0, 1, 250, 1000};
  const double delays[] = {-1500, -250, 0, 250, 1500};
  const double end_delays[] = {-250, 0, 400};
  const double counts[] = {0, 0.5, 1, 2.5, 4, kInf};
  const GTEXT_CSS_Anim_Fill css_fills[] = {GTEXT_CSS_ANIM_FILL_NONE, GTEXT_CSS_ANIM_FILL_FORWARDS, GTEXT_CSS_ANIM_FILL_BACKWARDS, GTEXT_CSS_ANIM_FILL_BOTH};
  const GTEXT_Anim_Fill fills[] = {GTEXT_ANIM_FILL_NONE, GTEXT_ANIM_FILL_FORWARDS, GTEXT_ANIM_FILL_BACKWARDS, GTEXT_ANIM_FILL_BOTH};
  const GTEXT_CSS_Anim_Direction css_dirs[] = {GTEXT_CSS_ANIM_DIRECTION_NORMAL, GTEXT_CSS_ANIM_DIRECTION_REVERSE, GTEXT_CSS_ANIM_DIRECTION_ALTERNATE,
      GTEXT_CSS_ANIM_DIRECTION_ALTERNATE_REVERSE};
  const GTEXT_Anim_Direction dirs[] = {GTEXT_ANIM_DIRECTION_NORMAL, GTEXT_ANIM_DIRECTION_REVERSE, GTEXT_ANIM_DIRECTION_ALTERNATE, GTEXT_ANIM_DIRECTION_ALTERNATE_REVERSE};
  const double locals[] = {-2000, -250, -1, 0, 1, 125, 250, 249.5, 500, 999, 1000, 1250, 2500, 3999, 4000, 4250, 6000, 100000};
  size_t compared = 0;
  for (double dur : durations) {
    for (double delay : delays) {
      for (double end : end_delays) {
        for (double count : counts) {
          for (size_t f = 0; f < 4; f++) {
            for (size_t d = 0; d < 4; d++) {
              GTEXT_CSS_Anim_Effect old;
              std::memset(&old, 0, sizeof old);
              old.duration_ms = dur;
              old.delay_start_ms = delay;
              old.delay_end_ms = end;
              old.infinite = count == kInf;
              old.iteration_count = old.infinite ? 0 : count;
              old.fill = css_fills[f];
              old.direction = css_dirs[d];
              GTEXT_Anim_Effect_Timing t = timing(dur, delay, count, fills[f], dirs[d], end);
              for (double local : locals) {
                GTEXT_CSS_Anim_Timing o;
                gtext_css_anim_timing(&old, local, &o);
                const GTEXT_Anim_Computed_Timing n = at(t, T(local));
                const std::string where = "dur " + std::to_string(dur) + " delay " + std::to_string(delay) + " end " + std::to_string(end) + " count " + std::to_string(count) + " fill " +
                    std::to_string(f) + " dir " + std::to_string(d) + " at " + std::to_string(local);
                ASSERT_EQ(static_cast<int>(o.phase), static_cast<int>(n.phase)) << where;
                ASSERT_EQ(o.in_effect, n.in_effect) << where;
                if (!o.in_effect) {
                  continue;
                }
                ASSERT_EQ(o.active_time_ms, n.active_time) << where;
                if (o.overall_infinite) {
                  ASSERT_EQ(n.overall_progress, kInf) << where;
                }
                else {
                  ASSERT_EQ(o.overall_progress, n.overall_progress) << where;
                }
                ASSERT_EQ(o.simple_progress, n.simple_progress) << where;
                if (o.iteration_infinite) {
                  ASSERT_EQ(n.current_iteration, kInf) << where;
                }
                else if (!o.overall_infinite) {
                  ASSERT_EQ(o.iteration_index, n.current_iteration) << where;
                }
                ASSERT_EQ(o.forwards, n.forwards) << where;
                ASSERT_EQ(o.progress, n.directed_progress) << where;
                ASSERT_EQ(o.before_flag, n.before_flag) << where;
                compared++;
              }
            }
          }
        }
      }
    }
  }
  EXPECT_GT(compared, 20000u) << "the grid reached the inputs it was made for";
}

}  // namespace
