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
 * The Animation state machine (animation/animation.h) against Web Animations 1, "Animation model": the current time, the pending play and pause tasks, the ready and finished promises as
 * generations and notes, the finished state, finish, cancel, reverse, the playback rate and its seamless update, seeking, the start time, the replace state and the playback events.
 * A time is always given; the tests move it by hand.
 */

#include <cmath>
#include <limits>
#include <string>

#include <gtest/gtest.h>

#include "markup_test_support.h"

#include <ghoti.io/text/animation/animation.h>

namespace {

const double kInf = std::numeric_limits<double>::infinity();
const GTEXT_Anim_Time kNone{false, 0};

GTEXT_Anim_Time T(double ms) {
  return GTEXT_Anim_Time{true, ms};
}

std::string num(const GTEXT_Anim_Time & t) {
  if (!t.resolved) {
    return "null";
  }
  char b[48];
  snprintf(b, sizeof b, "%g", t.ms);
  return b;
}

const char * ps(GTEXT_Anim_Play_State s) {
  switch (s) {
    case GTEXT_ANIM_PLAY_IDLE: return "idle";
    case GTEXT_ANIM_PLAY_RUNNING: return "running";
    case GTEXT_ANIM_PLAY_PAUSED: return "paused";
    case GTEXT_ANIM_PLAY_FINISHED: return "finished";
  }
  return "?";
}

struct A {
  GTEXT_Anim_Animation * a = nullptr;

  explicit A(GTEXT_Anim_Timeline_Kind kind = GTEXT_ANIM_TIMELINE_DOCUMENT, double duration = 1000.0, const GTEXT_Allocator * alloc = nullptr) {
    EXPECT_EQ(gtext_anim_animation_new(alloc, kind, &a), GTEXT_ANIM_OK);
    if (duration >= 0) {
      GTEXT_Anim_Effect_Timing t = gtext_anim_timing_default();
      t.duration_auto = false;
      t.duration = duration;
      EXPECT_EQ(gtext_anim_animation_set_effect(a, &t, kNone), GTEXT_ANIM_OK);
    }
  }
  ~A() {
    gtext_anim_animation_free(a);
  }
  A(const A &) = delete;
  A & operator=(const A &) = delete;

  GTEXT_Anim_Animation_State st(GTEXT_Anim_Time now) const {
    GTEXT_Anim_Animation_State s;
    EXPECT_EQ(gtext_anim_animation_state(a, now, &s), GTEXT_ANIM_OK);
    return s;
  }
  /* "playState currentTime startTime pending" */
  std::string line(GTEXT_Anim_Time now) const {
    const auto s = st(now);
    return std::string(ps(s.play_state)) + " cur=" + num(s.current_time) + " start=" + num(s.start_time) + (s.pending ? " pending" : "");
  }
  /* the notes owed, drained: "RR2" ready resolve of generation 2, "RJ" reject, "FR" finished resolve, "FJ" reject */
  std::string notes() {
    std::string out;
    for (size_t i = 0; i < gtext_anim_animation_note_count(a); i++) {
      GTEXT_Anim_Note n;
      gtext_anim_animation_note_at(a, i, &n);
      static const char * k[] = {"RR", "RJ", "FR", "FJ"};
      out += (out.empty() ? "" : " ") + std::string(k[n.kind]) + std::to_string(n.generation);
    }
    gtext_anim_animation_notes_clear(a);
    return out;
  }
  /* the events owed, drained: "finish(cur,timeline,scheduled)" */
  std::string events() {
    GTEXT_Anim_Events * ev = nullptr;
    EXPECT_EQ(gtext_anim_events_new(nullptr, &ev), GTEXT_ANIM_OK);
    EXPECT_EQ(gtext_anim_animation_take_events(a, 7, ev), GTEXT_ANIM_OK);
    std::string out;
    for (size_t i = 0; i < gtext_anim_events_count(ev); i++) {
      GTEXT_Anim_Event e;
      gtext_anim_events_at(ev, i, &e);
      const char * names[] = {"", "", "", "", "", "", "", "", "finish", "cancel", "remove"};
      EXPECT_EQ(e.animation, 7u);
      out += (out.empty() ? "" : " ") + std::string(names[e.type]) + "(" + num(e.current_time) + "," + num(e.timeline_time) + "," + num(e.scheduled_time) + ")";
    }
    gtext_anim_events_free(ev);
    return out;
  }
  void must(GTEXT_Anim_Status s, const char * what) const {
    EXPECT_EQ(s, GTEXT_ANIM_OK) << what;
  }
};

/* ---- the initial state, the current time ---- */

TEST(AnimAnimation, ANewAnimationIsIdleWithAResolvedReadyPromiseAndAPendingFinishedPromise) {
  A x;
  const auto s = x.st(T(0));
  EXPECT_EQ(s.play_state, GTEXT_ANIM_PLAY_IDLE);
  EXPECT_FALSE(s.current_time.resolved);
  EXPECT_FALSE(s.start_time.resolved);
  EXPECT_FALSE(s.pending);
  EXPECT_EQ(s.playback_rate, 1.0);
  EXPECT_EQ(s.replace_state, GTEXT_ANIM_REPLACE_ACTIVE);
  EXPECT_EQ(s.ready, GTEXT_ANIM_PROMISE_RESOLVED);
  EXPECT_EQ(s.ready_generation, 1u);
  EXPECT_EQ(s.finished, GTEXT_ANIM_PROMISE_PENDING);
  EXPECT_EQ(s.finished_generation, 1u);
  EXPECT_EQ(s.effect_end, 1000.0);
  EXPECT_EQ(x.notes(), "");
  EXPECT_EQ(x.events(), "");
}

TEST(AnimAnimation, TheCurrentTimeIsTheHoldTimeThenTheTimelineTimeLessTheStartTimeTimesTheRate) {
  A x;
  ASSERT_EQ(gtext_anim_animation_set_start_time(x.a, T(100), T(100)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.line(T(400)), "running cur=300 start=100");
  ASSERT_EQ(gtext_anim_animation_set_playback_rate(x.a, 2, T(400)), GTEXT_ANIM_OK);
  /* the rate change keeps the current time (300) and moves the start time */
  EXPECT_EQ(x.line(T(400)), "running cur=300 start=250");
  EXPECT_EQ(x.line(T(450)), "running cur=400 start=250");
}

TEST(AnimAnimation, AnInactiveTimelineHasNoCurrentTimeButAHeldOneStaysHeld) {
  A x;
  ASSERT_EQ(gtext_anim_animation_set_current_time(x.a, T(250), kNone), GTEXT_ANIM_OK);
  EXPECT_EQ(x.line(kNone), "paused cur=250 start=null");
  ASSERT_EQ(gtext_anim_animation_set_start_time(x.a, T(100), kNone), GTEXT_ANIM_OK);
  /* "If timeline time is unresolved and new start time is resolved, make the hold time unresolved": only one of the two exists with no active timeline */
  EXPECT_EQ(x.line(kNone), "running cur=null start=100") << "a resolved start time and no hold time is the running state, whatever the timeline";
}

/* ---- play: the pending task, the ready promise ---- */

TEST(AnimAnimation, PlayRewindsAnIdleAnimationAndWaitsForTheNextTickToStart) {
  A x;
  ASSERT_EQ(gtext_anim_animation_play(x.a, T(100)), GTEXT_ANIM_OK);
  auto s = x.st(T(100));
  EXPECT_EQ(s.play_state, GTEXT_ANIM_PLAY_RUNNING);
  EXPECT_TRUE(s.pending_play);
  EXPECT_EQ(s.ready, GTEXT_ANIM_PROMISE_PENDING);
  EXPECT_EQ(s.ready_generation, 2u);
  EXPECT_EQ(x.line(T(100)), "running cur=0 start=null pending");
  EXPECT_EQ(x.notes(), "");
  /* nothing moves until the task runs, whatever the time */
  EXPECT_EQ(x.line(T(900)), "running cur=0 start=null pending");
  ASSERT_EQ(gtext_anim_animation_run_pending(x.a, T(116)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.line(T(116)), "running cur=0 start=116");
  EXPECT_EQ(x.line(T(616)), "running cur=500 start=116");
  EXPECT_EQ(x.notes(), "RR2");
}

TEST(AnimAnimation, ThePendingTaskWaitsForAnActiveTimeline) {
  A x;
  ASSERT_EQ(gtext_anim_animation_play(x.a, kNone), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_run_pending(x.a, kNone), GTEXT_ANIM_OK);
  EXPECT_TRUE(x.st(kNone).pending);
  EXPECT_EQ(x.notes(), "");
  ASSERT_EQ(gtext_anim_animation_run_pending(x.a, T(50)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.line(T(50)), "running cur=0 start=50");
  EXPECT_EQ(x.notes(), "RR2");
}

TEST(AnimAnimation, AnAnimationWithNoTimelineCannotStart) {
  A x(GTEXT_ANIM_TIMELINE_NONE);
  ASSERT_EQ(gtext_anim_animation_play(x.a, T(10)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_run_pending(x.a, T(20)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.line(T(20)), "running cur=0 start=null pending");
}

TEST(AnimAnimation, PlayingTwiceBeforeItIsReadyReusesThePromise) {
  A x;
  ASSERT_EQ(gtext_anim_animation_play(x.a, T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_play(x.a, T(0)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.st(T(0)).ready_generation, 2u);
}

TEST(AnimAnimation, PauseThenPlayLeavesTheReadyPromiseOneGenerationAndRunning) {
  /* the example of "The current ready promise": the state is running when the one promise resolves */
  A x;
  ASSERT_EQ(gtext_anim_animation_play(x.a, T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_run_pending(x.a, T(16)), GTEXT_ANIM_OK);
  x.notes();
  ASSERT_EQ(gtext_anim_animation_pause(x.a, T(116)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.st(T(116)).ready_generation, 3u);
  ASSERT_EQ(gtext_anim_animation_play(x.a, T(116)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.st(T(116)).ready_generation, 3u);
  ASSERT_EQ(gtext_anim_animation_run_pending(x.a, T(132)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.notes(), "RR3");
  EXPECT_EQ(std::string(ps(x.st(T(132)).play_state)), "running");
}

TEST(AnimAnimation, PlayRefusesToRewindAReversedAnimationWithAnInfiniteEffect) {
  A x(GTEXT_ANIM_TIMELINE_DOCUMENT, 10);
  GTEXT_Anim_Effect_Timing t = gtext_anim_timing_default();
  t.duration_auto = false;
  t.duration = 10;
  t.iterations = kInf;
  ASSERT_EQ(gtext_anim_animation_set_effect(x.a, &t, kNone), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_set_playback_rate(x.a, -1, T(0)), GTEXT_ANIM_OK);
  const auto before = x.line(T(0));
  EXPECT_EQ(gtext_anim_animation_play(x.a, T(0)), GTEXT_ANIM_INVALID_STATE);
  EXPECT_EQ(x.line(T(0)), before) << "nothing changed";
}

/* ---- pause ---- */

TEST(AnimAnimation, PausePendsAndTheTaskHoldsTheCurrentTimeAtTheReadyTime) {
  A x;
  ASSERT_EQ(gtext_anim_animation_set_start_time(x.a, T(0), T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_pause(x.a, T(300)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.line(T(300)), "paused cur=300 start=0 pending");
  ASSERT_EQ(gtext_anim_animation_run_pending(x.a, T(316)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.line(T(316)), "paused cur=316 start=null");
  EXPECT_EQ(x.line(T(900)), "paused cur=316 start=null");
  EXPECT_EQ(x.notes(), "RR2");
}

TEST(AnimAnimation, PausingAnIdleAnimationSeeksToZero) {
  A x;
  ASSERT_EQ(gtext_anim_animation_pause(x.a, T(10)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_run_pending(x.a, T(26)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.line(T(26)), "paused cur=0 start=null");
}

TEST(AnimAnimation, PauseIsANoOpWhenPausedOrPausePending) {
  A x;
  ASSERT_EQ(gtext_anim_animation_pause(x.a, T(0)), GTEXT_ANIM_OK);
  const auto g = x.st(T(0)).ready_generation;
  ASSERT_EQ(gtext_anim_animation_pause(x.a, T(0)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.st(T(0)).ready_generation, g);
  ASSERT_EQ(gtext_anim_animation_run_pending(x.a, T(16)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_pause(x.a, T(32)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.st(T(32)).ready_generation, g);
}

TEST(AnimAnimation, PauseOfAReversedIdleAnimationWithAnInfiniteEffectIsAnInvalidStateError) {
  A x(GTEXT_ANIM_TIMELINE_DOCUMENT, 10);
  GTEXT_Anim_Effect_Timing t = gtext_anim_timing_default();
  t.duration_auto = false;
  t.duration = 10;
  t.iterations = kInf;
  ASSERT_EQ(gtext_anim_animation_set_effect(x.a, &t, kNone), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_set_playback_rate(x.a, -1, T(0)), GTEXT_ANIM_OK);
  EXPECT_EQ(gtext_anim_animation_pause(x.a, T(0)), GTEXT_ANIM_INVALID_STATE);
  EXPECT_EQ(x.line(T(0)), "idle cur=null start=null");
}

/* ---- the finished state, finish notification, the finished promise ---- */

TEST(AnimAnimation, AnAnimationThatReachesItsEndIsFinishedAndTheFinishNotificationWaitsForAMicrotask) {
  A x;
  ASSERT_EQ(gtext_anim_animation_play(x.a, T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_run_pending(x.a, T(0)), GTEXT_ANIM_OK);
  x.notes();
  ASSERT_EQ(gtext_anim_animation_update_finished(x.a, T(500)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.line(T(500)), "running cur=500 start=0");
  ASSERT_EQ(gtext_anim_animation_update_finished(x.a, T(1200)), GTEXT_ANIM_OK);
  /* the hold time is the greater of the previous current time (500) and the end (1000) */
  EXPECT_EQ(x.line(T(1200)), "finished cur=1000 start=0");
  EXPECT_EQ(x.st(T(1200)).finished, GTEXT_ANIM_PROMISE_PENDING);
  EXPECT_EQ(x.notes(), "");
  EXPECT_EQ(x.events(), "");
  ASSERT_EQ(gtext_anim_animation_checkpoint(x.a, T(1200)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.notes(), "FR1");
  EXPECT_EQ(x.st(T(1200)).finished, GTEXT_ANIM_PROMISE_RESOLVED);
  /* the finish event: the animation's current time, the timeline's time, the end of the effect on the timeline's clock */
  EXPECT_EQ(x.events(), "finish(1000,1200,1000)");
  ASSERT_EQ(gtext_anim_animation_checkpoint(x.a, T(1300)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.events(), "") << "once";
}

TEST(AnimAnimation, AnimationThatLeavesTheFinishedStateBeforeTheMicrotaskNeverNotifies) {
  /* the example of "Updating the finished state": playbackRate = 2; currentTime = 2000; then updateTiming({iterations: 2}) */
  A x(GTEXT_ANIM_TIMELINE_DOCUMENT, 2000);
  ASSERT_EQ(gtext_anim_animation_set_start_time(x.a, T(0), T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_set_playback_rate(x.a, 2, T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_set_current_time(x.a, T(2000), T(0)), GTEXT_ANIM_OK);
  EXPECT_EQ(std::string(ps(x.st(T(0)).play_state)), "finished");
  GTEXT_Anim_Optional_Timing o;
  memset(&o, 0, sizeof o);
  o.present = GTEXT_ANIM_TIMING_ITERATIONS;
  o.values = gtext_anim_timing_default();
  o.values.iterations = 2;
  ASSERT_EQ(gtext_anim_animation_update_effect_timing(x.a, &o, T(0)), GTEXT_ANIM_OK);
  EXPECT_EQ(std::string(ps(x.st(T(0)).play_state)), "running");
  ASSERT_EQ(gtext_anim_animation_checkpoint(x.a, T(0)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.notes(), "");
  EXPECT_EQ(x.events(), "");
  EXPECT_EQ(x.st(T(0)).finished, GTEXT_ANIM_PROMISE_PENDING);
}

TEST(AnimAnimation, FinishNotifiesAtOnceAndTheFinishedPromiseIsReplacedWhenItLeavesTheFinishedState) {
  A x;
  ASSERT_EQ(gtext_anim_animation_finish(x.a, T(40)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.line(T(40)), "finished cur=1000 start=-960");
  EXPECT_EQ(x.notes(), "FR1");
  EXPECT_EQ(x.events(), "finish(1000,40,40)");
  /* the example: the notification was synchronous, so seeking back afterwards does not undo it, and the finished promise is a new one */
  ASSERT_EQ(gtext_anim_animation_set_current_time(x.a, T(0), T(40)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.st(T(40)).finished_generation, 2u);
  EXPECT_EQ(x.st(T(40)).finished, GTEXT_ANIM_PROMISE_PENDING);
}

TEST(AnimAnimation, TheFinishEventOfAnAnimationWithNoActiveTimelineHasNoScheduledTime) {
  /* "If animation is associated with an inactive timeline, return an unresolved time value" */
  A x;
  ASSERT_EQ(gtext_anim_animation_set_start_time(x.a, T(0), T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_set_current_time(x.a, T(5000), T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_checkpoint(x.a, kNone), GTEXT_ANIM_OK) << "the microtask runs when the timeline has become inactive";
  EXPECT_EQ(x.events(), "finish(5000,null,null)");
}

TEST(AnimAnimation, PlayingOrPausingAnimationMakesItIgnoreTheCssPlayStateFromThenOn) {
  A x;
  EXPECT_FALSE(x.st(T(0)).play_state_override);
  ASSERT_EQ(gtext_anim_animation_play_from(x.a, T(0)), GTEXT_ANIM_OK);
  EXPECT_FALSE(x.st(T(0)).play_state_override) << "what the style does is not script's";
  ASSERT_EQ(gtext_anim_animation_css_pause(x.a, T(0)), GTEXT_ANIM_OK);
  EXPECT_FALSE(x.st(T(0)).play_state_override) << "nor is the pause of animation-play-state: paused";
  ASSERT_EQ(gtext_anim_animation_pause(x.a, T(0)), GTEXT_ANIM_OK);
  EXPECT_TRUE(x.st(T(0)).play_state_override);
  A y;
  ASSERT_EQ(gtext_anim_animation_play(y.a, T(0)), GTEXT_ANIM_OK);
  EXPECT_TRUE(y.st(T(0)).play_state_override);
  A z;
  ASSERT_EQ(gtext_anim_animation_set_start_time(z.a, T(0), T(0)), GTEXT_ANIM_OK);
  EXPECT_FALSE(z.st(T(0)).play_state_override) << "a start time that does not move the animation to or from paused";
  ASSERT_EQ(gtext_anim_animation_set_start_time(z.a, kNone, T(10)), GTEXT_ANIM_OK);
  EXPECT_TRUE(z.st(T(10)).play_state_override) << "running to paused";
}

TEST(AnimAnimation, FinishWithAnInfiniteEffectOrAZeroRateIsAnInvalidStateError) {
  A x(GTEXT_ANIM_TIMELINE_DOCUMENT, 10);
  GTEXT_Anim_Effect_Timing t = gtext_anim_timing_default();
  t.duration_auto = false;
  t.duration = 10;
  t.iterations = kInf;
  ASSERT_EQ(gtext_anim_animation_set_effect(x.a, &t, kNone), GTEXT_ANIM_OK);
  EXPECT_EQ(gtext_anim_animation_finish(x.a, T(0)), GTEXT_ANIM_INVALID_STATE);
  ASSERT_EQ(gtext_anim_animation_set_playback_rate(x.a, 0, T(0)), GTEXT_ANIM_OK);
  EXPECT_EQ(gtext_anim_animation_finish(x.a, T(0)), GTEXT_ANIM_INVALID_STATE);
  /* a negative rate finishes at zero even for an infinite effect */
  ASSERT_EQ(gtext_anim_animation_set_playback_rate(x.a, -1, T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_finish(x.a, T(5)), GTEXT_ANIM_OK);
  EXPECT_EQ(std::string(ps(x.st(T(5)).play_state)), "finished");
  EXPECT_EQ(num(x.st(T(5)).current_time), "0");
}

TEST(AnimAnimation, FinishCompletesAPendingPlayOrPauseTaskAndResolvesReady) {
  A x;
  ASSERT_EQ(gtext_anim_animation_play(x.a, T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_finish(x.a, T(10)), GTEXT_ANIM_OK);
  EXPECT_FALSE(x.st(T(10)).pending);
  EXPECT_EQ(x.notes(), "RR2 FR1");
  A y;
  ASSERT_EQ(gtext_anim_animation_set_start_time(y.a, T(0), T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_pause(y.a, T(10)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_finish(y.a, T(10)), GTEXT_ANIM_OK);
  EXPECT_FALSE(y.st(T(10)).pending);
  EXPECT_EQ(y.notes(), "RR2 FR1");
  EXPECT_EQ(std::string(ps(y.st(T(10)).play_state)), "finished");
}

TEST(AnimAnimation, PlayingAFinishedAnimationRewindsItAndGivesItANewFinishedPromise) {
  A x;
  ASSERT_EQ(gtext_anim_animation_finish(x.a, T(0)), GTEXT_ANIM_OK);
  x.notes();
  x.events();
  ASSERT_EQ(gtext_anim_animation_play(x.a, T(10)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.line(T(10)), "running cur=0 start=null pending");
  EXPECT_EQ(x.st(T(10)).finished_generation, 2u);
  EXPECT_EQ(x.st(T(10)).finished, GTEXT_ANIM_PROMISE_PENDING);
}

TEST(AnimAnimation, AZeroLengthEffectIsFinishedAtOnceAndResolvesReadyBeforeFinished) {
  A x(GTEXT_ANIM_TIMELINE_DOCUMENT, 0);
  ASSERT_EQ(gtext_anim_animation_play(x.a, T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_run_pending(x.a, T(16)), GTEXT_ANIM_OK);
  /* "an animation with zero-length associated effect will resolve its current ready promise before its current finished promise" */
  ASSERT_EQ(gtext_anim_animation_checkpoint(x.a, T(16)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.notes(), "RR2 FR1");
  EXPECT_EQ(x.line(T(16)), "finished cur=0 start=16");
}

/* ---- cancel ---- */

TEST(AnimAnimation, CancelRejectsTheFinishedPromiseAndOwesACancelEvent) {
  A x;
  ASSERT_EQ(gtext_anim_animation_set_start_time(x.a, T(0), T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_cancel(x.a, T(250)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.line(T(250)), "idle cur=null start=null");
  EXPECT_EQ(x.notes(), "FJ1");
  EXPECT_EQ(x.st(T(250)).finished_generation, 2u);
  EXPECT_EQ(x.st(T(250)).finished, GTEXT_ANIM_PROMISE_PENDING);
  /* the event: no current time, the timeline's time, scheduled at it */
  EXPECT_EQ(x.events(), "cancel(null,250,250)");
}

TEST(AnimAnimation, CancelOfAnIdleAnimationDoesNothingButClear) {
  A x;
  ASSERT_EQ(gtext_anim_animation_cancel(x.a, T(0)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.notes(), "");
  EXPECT_EQ(x.events(), "");
  EXPECT_EQ(x.st(T(0)).finished_generation, 1u);
}

TEST(AnimAnimation, CancelWhileAPlayIsPendingRejectsReadyAndMakesANewResolvedOne) {
  /* "Playback was canceled": the pending play task does not run synchronously, so the promise is rejected */
  A x;
  ASSERT_EQ(gtext_anim_animation_play(x.a, T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_cancel(x.a, T(0)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.notes(), "RJ2 FJ1");
  const auto s = x.st(T(0));
  EXPECT_EQ(s.ready_generation, 3u);
  EXPECT_EQ(s.ready, GTEXT_ANIM_PROMISE_RESOLVED);
  EXPECT_FALSE(s.pending);
  EXPECT_EQ(s.play_state, GTEXT_ANIM_PLAY_IDLE);
}

TEST(AnimAnimation, CancelWithNoActiveTimelineHasNoTimelineTimeAndNoScheduledTime) {
  A x(GTEXT_ANIM_TIMELINE_NONE);
  ASSERT_EQ(gtext_anim_animation_set_current_time(x.a, T(5), kNone), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_cancel(x.a, kNone), GTEXT_ANIM_OK);
  EXPECT_EQ(x.events(), "cancel(null,null,null)");
}

/* ---- reverse ---- */

TEST(AnimAnimation, ReverseSetsAPendingRateAndSeeksToTheEnd) {
  A x;
  ASSERT_EQ(gtext_anim_animation_reverse(x.a, T(0)), GTEXT_ANIM_OK);
  auto s = x.st(T(0));
  EXPECT_TRUE(s.has_pending_rate);
  EXPECT_EQ(s.pending_rate, -1.0);
  EXPECT_EQ(s.playback_rate, 1.0);
  EXPECT_EQ(s.effective_rate, -1.0);
  EXPECT_EQ(x.line(T(0)), "running cur=1000 start=null pending");
  ASSERT_EQ(gtext_anim_animation_run_pending(x.a, T(16)), GTEXT_ANIM_OK);
  s = x.st(T(16));
  EXPECT_FALSE(s.has_pending_rate);
  EXPECT_EQ(s.playback_rate, -1.0);
  EXPECT_EQ(x.line(T(16)), "running cur=1000 start=1016");
  EXPECT_EQ(x.line(T(516)), "running cur=500 start=1016");
}

TEST(AnimAnimation, ReverseWithNoActiveTimelineIsAnInvalidStateErrorAndAFailedPlayRestoresThePendingRate) {
  A x(GTEXT_ANIM_TIMELINE_NONE);
  EXPECT_EQ(gtext_anim_animation_reverse(x.a, T(0)), GTEXT_ANIM_INVALID_STATE);
  A y;
  EXPECT_EQ(gtext_anim_animation_reverse(y.a, kNone), GTEXT_ANIM_INVALID_STATE);
  A z(GTEXT_ANIM_TIMELINE_DOCUMENT, 10);
  GTEXT_Anim_Effect_Timing t = gtext_anim_timing_default();
  t.duration_auto = false;
  t.duration = 10;
  t.iterations = kInf;
  ASSERT_EQ(gtext_anim_animation_set_effect(z.a, &t, kNone), GTEXT_ANIM_OK);
  EXPECT_EQ(gtext_anim_animation_reverse(z.a, T(0)), GTEXT_ANIM_INVALID_STATE) << "a negative effective rate and an infinite end";
  EXPECT_FALSE(z.st(T(0)).has_pending_rate) << "the original (unset) pending rate is back";
  EXPECT_EQ(z.line(T(0)), "idle cur=null start=null");
}

/* ---- seeking ---- */

TEST(AnimAnimation, SettingTheCurrentTimeToNullIsATypeErrorWhenItHasOne) {
  A x;
  ASSERT_EQ(gtext_anim_animation_set_current_time(x.a, kNone, T(0)), GTEXT_ANIM_OK) << "idle: unresolved to unresolved";
  ASSERT_EQ(gtext_anim_animation_set_current_time(x.a, T(10), T(0)), GTEXT_ANIM_OK);
  EXPECT_EQ(gtext_anim_animation_set_current_time(x.a, kNone, T(0)), GTEXT_ANIM_TYPE_ERROR);
  EXPECT_EQ(x.line(T(0)), "paused cur=10 start=null");
}

TEST(AnimAnimation, SeekingAPausePendingAnimationCompletesThePause) {
  A x;
  ASSERT_EQ(gtext_anim_animation_set_start_time(x.a, T(0), T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_pause(x.a, T(100)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_set_current_time(x.a, T(700), T(100)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.line(T(100)), "paused cur=700 start=null");
  EXPECT_EQ(x.notes(), "RR2");
}

TEST(AnimAnimation, SeekingARunningAnimationMovesItsStartTimeAndSeekingPastTheEndFinishesIt) {
  A x;
  ASSERT_EQ(gtext_anim_animation_set_start_time(x.a, T(0), T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_set_current_time(x.a, T(400), T(100)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.line(T(100)), "running cur=400 start=-300");
  ASSERT_EQ(gtext_anim_animation_set_current_time(x.a, T(5000), T(100)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.line(T(100)), "finished cur=5000 start=-4900") << "a seek past the end holds the time it was given";
}

/* ---- the start time ---- */

TEST(AnimAnimation, SettingTheStartTimeOfAPlayPendingAnimationRunsItAndCancelsThePendingTask) {
  A x;
  ASSERT_EQ(gtext_anim_animation_play(x.a, T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_set_start_time(x.a, T(-100), T(50)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.line(T(50)), "running cur=150 start=-100");
  EXPECT_EQ(x.notes(), "RR2");
}

TEST(AnimAnimation, SettingTheStartTimeToNullHoldsTheCurrentTime) {
  A x;
  ASSERT_EQ(gtext_anim_animation_set_start_time(x.a, T(0), T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_set_start_time(x.a, kNone, T(300)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.line(T(900)), "paused cur=300 start=null");
}

/* ---- playback rate ---- */

TEST(AnimAnimation, ARateOfZeroHoldsTheCurrentTimeWhileTheAnimationIsStillRunning) {
  A x;
  ASSERT_EQ(gtext_anim_animation_set_start_time(x.a, T(0), T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_set_playback_rate(x.a, 0, T(200)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.line(T(900)), "running cur=200 start=0");
  EXPECT_EQ(x.st(T(900)).hold_time.ms, 200);
}

TEST(AnimAnimation, ThePlaybackRateMustBeAFiniteNumber) {
  A x;
  EXPECT_EQ(gtext_anim_animation_set_playback_rate(x.a, kInf, T(0)), GTEXT_ANIM_TYPE_ERROR);
  EXPECT_EQ(gtext_anim_animation_set_playback_rate(x.a, std::nan(""), T(0)), GTEXT_ANIM_TYPE_ERROR);
  EXPECT_EQ(gtext_anim_animation_update_playback_rate(x.a, -kInf, T(0)), GTEXT_ANIM_TYPE_ERROR);
  EXPECT_EQ(x.st(T(0)).playback_rate, 1.0);
}

TEST(AnimAnimation, UpdatePlaybackRateOfARunningAnimationKeepsItsCurrentTimeThroughAPendingPlay) {
  A x;
  ASSERT_EQ(gtext_anim_animation_set_start_time(x.a, T(0), T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_update_playback_rate(x.a, 2, T(300)), GTEXT_ANIM_OK);
  auto s = x.st(T(300));
  EXPECT_TRUE(s.pending_play) << "it plays again, without rewinding";
  EXPECT_EQ(s.playback_rate, 1.0);
  EXPECT_EQ(s.effective_rate, 2.0);
  ASSERT_EQ(gtext_anim_animation_run_pending(x.a, T(316)), GTEXT_ANIM_OK);
  s = x.st(T(316));
  EXPECT_EQ(s.playback_rate, 2.0);
  EXPECT_FALSE(s.has_pending_rate);
  /* the current time the animation had when it was made ready (316) goes on at the new rate */
  EXPECT_EQ(num(s.current_time), "316");
  EXPECT_EQ(num(x.st(T(416)).current_time), "516");
}

TEST(AnimAnimation, UpdatePlaybackRateOfAPausedOrIdleAnimationAppliesAtOnce) {
  A x;
  ASSERT_EQ(gtext_anim_animation_update_playback_rate(x.a, 3, T(0)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.st(T(0)).playback_rate, 3.0);
  EXPECT_FALSE(x.st(T(0)).has_pending_rate);
  EXPECT_EQ(x.line(T(0)), "idle cur=null start=null");
}

TEST(AnimAnimation, UpdatePlaybackRateOfAFinishedAnimationSetsTheStartTimeAndAppliesAtOnce) {
  A x;
  ASSERT_EQ(gtext_anim_animation_finish(x.a, T(0)), GTEXT_ANIM_OK);
  x.notes();
  x.events();
  ASSERT_EQ(gtext_anim_animation_update_playback_rate(x.a, 2, T(100)), GTEXT_ANIM_OK);
  const auto s = x.st(T(100));
  EXPECT_EQ(s.playback_rate, 2.0);
  EXPECT_FALSE(s.pending);
  EXPECT_EQ(s.play_state, GTEXT_ANIM_PLAY_FINISHED);
}

TEST(AnimAnimation, UpdatePlaybackRateWhileATaskIsPendingLeavesItForTheTask) {
  A x;
  ASSERT_EQ(gtext_anim_animation_play(x.a, T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_update_playback_rate(x.a, 4, T(0)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.st(T(0)).playback_rate, 1.0);
  EXPECT_EQ(x.st(T(0)).effective_rate, 4.0);
  ASSERT_EQ(gtext_anim_animation_run_pending(x.a, T(16)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.st(T(16)).playback_rate, 4.0);
}

/* ---- effect, timeline ---- */

TEST(AnimAnimation, ChangingTheEffectTimingMovesTheEndAndTheFinishedState) {
  A x;
  ASSERT_EQ(gtext_anim_animation_set_start_time(x.a, T(0), T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_update_finished(x.a, T(1500)), GTEXT_ANIM_OK);
  EXPECT_EQ(std::string(ps(x.st(T(1500)).play_state)), "finished");
  GTEXT_Anim_Optional_Timing o;
  memset(&o, 0, sizeof o);
  o.present = GTEXT_ANIM_TIMING_DURATION;
  o.values = gtext_anim_timing_default();
  o.values.duration_auto = false;
  o.values.duration = 3000;
  ASSERT_EQ(gtext_anim_animation_update_effect_timing(x.a, &o, T(1500)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.st(T(1500)).effect_end, 3000.0);
  EXPECT_EQ(x.line(T(1500)), "running cur=1500 start=0");
  /* a negative duration is a TypeError and changes nothing */
  o.values.duration = -1;
  EXPECT_EQ(gtext_anim_animation_update_effect_timing(x.a, &o, T(1500)), GTEXT_ANIM_TYPE_ERROR);
  EXPECT_EQ(x.st(T(1500)).effect_end, 3000.0);
}

TEST(AnimAnimation, AnAnimationWithNoEffectHasAnEffectEndOfZero) {
  A x(GTEXT_ANIM_TIMELINE_DOCUMENT, -1);
  EXPECT_FALSE(x.st(T(0)).has_effect);
  EXPECT_EQ(x.st(T(0)).effect_end, 0.0);
  ASSERT_EQ(gtext_anim_animation_set_current_time(x.a, T(5), T(0)), GTEXT_ANIM_OK);
  EXPECT_EQ(std::string(ps(x.st(T(0)).play_state)), "paused") << "seeking is possible with no effect";
  EXPECT_EQ(gtext_anim_animation_update_effect_timing(x.a, nullptr, T(0)), GTEXT_ANIM_INVALID);
}

TEST(AnimAnimation, SettingAnInvalidEffectIsATypeErrorAndChangesNothing) {
  A x;
  GTEXT_Anim_Effect_Timing t = gtext_anim_timing_default();
  t.iterations = -1;
  EXPECT_EQ(gtext_anim_animation_set_effect(x.a, &t, T(0)), GTEXT_ANIM_TYPE_ERROR);
  EXPECT_EQ(x.st(T(0)).effect_end, 1000.0);
}

TEST(AnimAnimation, SettingTheTimelineKeepsAHeldTimeAndRefusesAProgressBasedOne) {
  A x(GTEXT_ANIM_TIMELINE_NONE);
  ASSERT_EQ(gtext_anim_animation_set_current_time(x.a, T(200), kNone), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_set_timeline(x.a, GTEXT_ANIM_TIMELINE_DOCUMENT, T(0)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.line(T(0)), "paused cur=200 start=null") << "the hold time stays while the start time is unresolved";
  EXPECT_EQ(gtext_anim_animation_set_timeline(x.a, GTEXT_ANIM_TIMELINE_PROGRESS, T(0)), GTEXT_ANIM_NOT_SUPPORTED);
}

TEST(AnimAnimation, AProgressBasedTimelineSetsTheStartTimeInsteadOfTheHoldTimeWhenItPlaysAndPauses) {
  A x(GTEXT_ANIM_TIMELINE_PROGRESS);
  ASSERT_EQ(gtext_anim_animation_play(x.a, T(0)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.line(T(0)), "running cur=0 start=0 pending") << "a finite timeline is played at its own time, not held";
  ASSERT_EQ(gtext_anim_animation_run_pending(x.a, T(0)), GTEXT_ANIM_OK);
  A y(GTEXT_ANIM_TIMELINE_PROGRESS);
  ASSERT_EQ(gtext_anim_animation_pause(y.a, T(0)), GTEXT_ANIM_OK);
  EXPECT_EQ(y.line(T(0)), "paused cur=0 start=0 pending");
}

/* ---- the replace state and the remove event ---- */

TEST(AnimAnimation, AFinishedAnimationThatIsReplacedIsRemovedWithAnEventAndAPersistedOneIsNot) {
  A x;
  bool removed = true;
  ASSERT_EQ(gtext_anim_animation_remove_replaced(x.a, T(0), &removed), GTEXT_ANIM_OK);
  EXPECT_FALSE(removed) << "an idle animation is not replaceable";
  GTEXT_Anim_Effect_Timing t = gtext_anim_timing_default();
  t.duration_auto = false;
  t.duration = 1000;
  t.fill = GTEXT_ANIM_FILL_FORWARDS;
  ASSERT_EQ(gtext_anim_animation_set_effect(x.a, &t, kNone), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_finish(x.a, T(10)), GTEXT_ANIM_OK);
  x.notes();
  x.events();
  ASSERT_EQ(gtext_anim_animation_remove_replaced(x.a, T(20), &removed), GTEXT_ANIM_OK);
  EXPECT_TRUE(removed);
  EXPECT_EQ(x.st(T(20)).replace_state, GTEXT_ANIM_REPLACE_REMOVED);
  EXPECT_EQ(x.events(), "remove(1000,20,20)");
  ASSERT_EQ(gtext_anim_animation_remove_replaced(x.a, T(30), &removed), GTEXT_ANIM_OK);
  EXPECT_FALSE(removed) << "once";
  A y;
  ASSERT_EQ(gtext_anim_animation_set_effect(y.a, &t, kNone), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_finish(y.a, T(10)), GTEXT_ANIM_OK);
  y.events();
  ASSERT_EQ(gtext_anim_animation_persist(y.a), GTEXT_ANIM_OK);
  EXPECT_EQ(y.st(T(10)).replace_state, GTEXT_ANIM_REPLACE_PERSISTED);
  ASSERT_EQ(gtext_anim_animation_remove_replaced(y.a, T(20), &removed), GTEXT_ANIM_OK);
  EXPECT_FALSE(removed);
  EXPECT_EQ(y.events(), "");
}

TEST(AnimAnimation, ARunningAnimationIsNotReplaceable) {
  A x;
  ASSERT_EQ(gtext_anim_animation_set_start_time(x.a, T(0), T(0)), GTEXT_ANIM_OK);
  bool removed = true;
  ASSERT_EQ(gtext_anim_animation_remove_replaced(x.a, T(10), &removed), GTEXT_ANIM_OK);
  EXPECT_FALSE(removed);
}

/* ---- the events, in the list of a frame ---- */

TEST(AnimAnimation, TheEventsOwedAreTakenInTheOrderTheyWereQueuedAndOnlyOnce) {
  A x;
  ASSERT_EQ(gtext_anim_animation_finish(x.a, T(10)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_cancel(x.a, T(20)), GTEXT_ANIM_OK);
  EXPECT_EQ(gtext_anim_animation_event_count(x.a), 2u);
  EXPECT_EQ(x.events(), "finish(1000,10,10) cancel(null,20,20)");
  EXPECT_EQ(gtext_anim_animation_event_count(x.a), 0u);
  EXPECT_EQ(x.events(), "");
}

TEST(AnimAnimation, ANoteIsMadeForEachSettlementInOrder) {
  A x;
  ASSERT_EQ(gtext_anim_animation_play(x.a, T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_run_pending(x.a, T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_update_finished(x.a, T(2000)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_checkpoint(x.a, T(2000)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_play(x.a, T(2000)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_cancel(x.a, T(2000)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.notes(), "RR2 FR1 RJ3 FJ2");
}

/* ---- inputs the first tests did not vary ---- */

TEST(AnimAnimation, PlayingAPausedAnimationResumesFromItsHeldTimeAtItsRate) {
  A x;
  ASSERT_EQ(gtext_anim_animation_set_current_time(x.a, T(300), T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_play(x.a, T(100)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.line(T(100)), "running cur=300 start=null pending") << "a paused animation is not rewound";
  ASSERT_EQ(gtext_anim_animation_run_pending(x.a, T(116)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.line(T(116)), "running cur=300 start=-184");
}

TEST(AnimAnimation, ThePauseTaskHoldsTheTimeOfAnAnimationThatRunsAtADifferentRate) {
  A x(GTEXT_ANIM_TIMELINE_DOCUMENT, 10000);
  ASSERT_EQ(gtext_anim_animation_set_start_time(x.a, T(0), T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_set_playback_rate(x.a, 2, T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_pause(x.a, T(100)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_run_pending(x.a, T(116)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.line(T(500)), "paused cur=232 start=null") << "(116 - 0) * 2";
}

TEST(AnimAnimation, CancelAppliesAPendingPlaybackRateOfAPlayThatNeverRan) {
  A x;
  ASSERT_EQ(gtext_anim_animation_reverse(x.a, T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_cancel(x.a, T(0)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.st(T(0)).playback_rate, -1.0) << "\"reset an animation's pending tasks\": apply any pending playback rate";
  EXPECT_FALSE(x.st(T(0)).has_pending_rate);
}

TEST(AnimAnimation, AnAnimationSeekedPastTheEndKeepsTheTimeItWasSeekedToAtTheNextTick) {
  A x;
  ASSERT_EQ(gtext_anim_animation_set_start_time(x.a, T(0), T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_set_current_time(x.a, T(5000), T(100)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_update_finished(x.a, T(200)), GTEXT_ANIM_OK);
  EXPECT_EQ(num(x.st(T(200)).current_time), "5000") << "the greater of the previous current time and the end";
}

TEST(AnimAnimation, AReversedAnimationSeekedBelowZeroKeepsTheTimeItWasSeekedToAtTheNextTick) {
  A x;
  ASSERT_EQ(gtext_anim_animation_set_start_time(x.a, T(0), T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_set_playback_rate(x.a, -1, T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_set_current_time(x.a, T(-50), T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_update_finished(x.a, T(100)), GTEXT_ANIM_OK);
  EXPECT_EQ(num(x.st(T(100)).current_time), "-50") << "the lesser of the previous current time and zero";
}

TEST(AnimAnimation, ARunningReversedAnimationStopsAtZero) {
  A x;
  ASSERT_EQ(gtext_anim_animation_set_start_time(x.a, T(1000), T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_set_playback_rate(x.a, -1, T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_set_current_time(x.a, T(400), T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_update_finished(x.a, T(200)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.line(T(200)), "running cur=200 start=400");
  ASSERT_EQ(gtext_anim_animation_update_finished(x.a, T(900)), GTEXT_ANIM_OK);
  EXPECT_EQ(num(x.st(T(900)).current_time), "0");
  EXPECT_EQ(std::string(ps(x.st(T(900)).play_state)), "finished");
}

TEST(AnimAnimation, AFinishedAnimationIsNotStickyWhenTheTimelineTimeGoesBackwards) {
  /* "Without this definition, a once-finished animation would remain finished even when its timeline progresses in the opposite direction": the hold time is not sticky */
  A x;
  ASSERT_EQ(gtext_anim_animation_set_start_time(x.a, T(0), T(0)), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_update_finished(x.a, T(1500)), GTEXT_ANIM_OK);
  EXPECT_EQ(std::string(ps(x.st(T(1500)).play_state)), "finished");
  ASSERT_EQ(gtext_anim_animation_update_finished(x.a, T(400)), GTEXT_ANIM_OK);
  EXPECT_EQ(x.line(T(400)), "running cur=400 start=0");
}

/* ---- arguments and memory ---- */

TEST(AnimAnimation, ANullAnimationAndABadKindAreInvalid) {
  EXPECT_EQ(gtext_anim_animation_play(nullptr, T(0)), GTEXT_ANIM_INVALID);
  EXPECT_EQ(gtext_anim_animation_persist(nullptr), GTEXT_ANIM_INVALID);
  GTEXT_Anim_Animation * a = nullptr;
  EXPECT_EQ(gtext_anim_animation_new(nullptr, static_cast<GTEXT_Anim_Timeline_Kind>(9), &a), GTEXT_ANIM_INVALID);
  EXPECT_EQ(a, nullptr);
  EXPECT_EQ(gtext_anim_animation_new(nullptr, GTEXT_ANIM_TIMELINE_DOCUMENT, nullptr), GTEXT_ANIM_INVALID);
  gtext_anim_animation_free(nullptr);
  EXPECT_EQ(gtext_anim_animation_note_count(nullptr), 0u);
  GTEXT_Anim_Animation_State s;
  EXPECT_EQ(gtext_anim_animation_state(nullptr, T(0), &s), GTEXT_ANIM_INVALID);
}

TEST(AnimAnimationOom, ACallThatCannotReserveRoomChangesNothingAndTheNextOneSucceeds) {
  FaultAllocator fa;
  A x(GTEXT_ANIM_TIMELINE_DOCUMENT, -1, &fa.alloc);
  size_t failures = 0;
  for (size_t n = 1; n < 20; n++) {
    fa.arm(n);
    const auto st = gtext_anim_animation_play(x.a, T(0));
    const bool failed = fa.failed;
    fa.disarm();
    if (st == GTEXT_ANIM_OK) {
      EXPECT_FALSE(failed);
      break;
    }
    failures++;
    EXPECT_EQ(st, GTEXT_ANIM_OOM);
    EXPECT_EQ(x.line(T(0)), "idle cur=null start=null") << "nothing changed on the failed call " << n;
  }
  EXPECT_GT(failures, 0u);
  EXPECT_EQ(x.line(T(0)), "finished cur=0 start=null pending");
  GTEXT_Anim_Effect_Timing t = gtext_anim_timing_default();
  t.duration_auto = false;
  t.duration = 1000;
  ASSERT_EQ(gtext_anim_animation_set_effect(x.a, &t, kNone), GTEXT_ANIM_OK);
  /* the events list fails whole too */
  ASSERT_EQ(gtext_anim_animation_finish(x.a, T(1)), GTEXT_ANIM_OK);
  GTEXT_Anim_Events * ev = nullptr;
  ASSERT_EQ(gtext_anim_events_new(&fa.alloc, &ev), GTEXT_ANIM_OK);
  fa.arm(1);
  EXPECT_EQ(gtext_anim_animation_take_events(x.a, 1, ev), GTEXT_ANIM_OOM);
  fa.disarm();
  EXPECT_EQ(gtext_anim_events_count(ev), 0u);
  EXPECT_EQ(gtext_anim_animation_event_count(x.a), 1u) << "the event is still owed";
  EXPECT_EQ(gtext_anim_animation_take_events(x.a, 1, ev), GTEXT_ANIM_OK);
  EXPECT_EQ(gtext_anim_events_count(ev), 1u);
  gtext_anim_events_free(ev);
}

}  // namespace
