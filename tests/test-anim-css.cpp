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
 * The CSS Animations of an element as `CSSAnimation` objects (animation/css_animations.h), against CSS Animations 2 ("Animations", "Owning element", "Animation composite order", "The
 * 'animation-play-state' property", "Event dispatch": the table of phase changes and the elapsed times) and Web Animations 1 ("Play states", "Relevant animations", "Updating the timing of an
 * AnimationEffect").
 */

#include <string>

#include <gtest/gtest.h>

#include "css_compute_test_support.h"
#include "css_test_support.h"
#include "markup_test_support.h"

#include <ghoti.io/text/animation/css_animations.h>

using namespace cmp;

namespace {

/// The `@keyframes` rules of a style sheet text, as a frozen set.
struct Frames {
  css_test::Parsed parsed;
  GTEXT_CSS_Atrule_Set * set = nullptr;
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
};

/// The effects the declarations of an element give.
struct Effects {
  Context ctx{R()};
  Block block;
  Style style;
  GTEXT_CSS_Anim_List * list = nullptr;
  explicit Effects(const std::string & decls) : block(R(), decls) {
    style.compute(ctx, &block);
    EXPECT_EQ(gtext_css_anim_list_from_style(style.s, nullptr, &list), GTEXT_CSS_OK);
  }
  Effects(const Effects &) = delete;
  Effects & operator=(const Effects &) = delete;
  ~Effects() {
    gtext_css_anim_list_free(list);
  }
};

const char * kFrames = "@keyframes a { from { opacity: 0 } to { opacity: 1 } } @keyframes b { from { width: 0px } to { width: 10px } }";

std::string num(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%g", v);
  return buf;
}

/// One element's set, the keyframes and the events as text.
struct Rig {
  Frames frames{kFrames};
  GTEXT_Anim_Css_Set * set = nullptr;
  GTEXT_Anim_Css_Update up{};
  Rig() {
    EXPECT_EQ(gtext_anim_css_set_new(nullptr, &set), GTEXT_CSS_OK);
  }
  Rig(const Rig &) = delete;
  Rig & operator=(const Rig &) = delete;
  ~Rig() {
    gtext_anim_css_set_free(set);
  }
  GTEXT_CSS_Status update(const std::string & decls, double t) {
    Effects e(decls);
    return gtext_anim_css_set_update(set, e.list, frames.set, t, &up);
  }
  GTEXT_Anim_Css_Info info(size_t i, double t) {
    GTEXT_Anim_Css_Info out{};
    EXPECT_EQ(gtext_anim_css_set_info(set, i, t, &out), GTEXT_CSS_OK);
    return out;
  }
  /// The events of a frame: `type:name:elapsed` for each, joined by spaces; with `times`, `@scheduled` follows (`@?` for an unresolved one).
  std::string frame(double t, bool times = false, GTEXT_CSS_Status * status = nullptr) {
    GTEXT_Anim_Events * ev = nullptr;
    EXPECT_EQ(gtext_anim_events_new(nullptr, &ev), GTEXT_ANIM_OK);
    const GTEXT_CSS_Status st = gtext_anim_css_set_frame(set, t, ev);
    if (status) {
      *status = st;
    }
    static const char * const names[] = {"start", "iteration", "end", "cancel"};
    std::string out;
    for (size_t i = 0; i < gtext_anim_events_count(ev); i++) {
      GTEXT_Anim_Event e{};
      EXPECT_TRUE(gtext_anim_events_at(ev, i, &e));
      out += std::string(i ? " " : "") + names[e.type] + ":" + std::string(e.name.data, e.name.length) + ":" + num(e.elapsed_time);
      if (times) {
        out += e.scheduled_time.resolved ? "@" + num(e.scheduled_time.ms) : "@?";
      }
    }
    gtext_anim_events_free(ev);
    return out;
  }
};

/* "Changes to any of the animation properties ... cause the corresponding CSSAnimation object ... to be updated": an effect with a `@keyframes` of its name is an animation, made pending ("the resolved
   value of animation-play-state ... including when the animation is first generated"), and ready at the next frame. */
TEST(AnimCssSet, ANamedEffectWithKeyframesIsAPendingAnimationReadyAtTheNextFrame) {
  Rig r;
  ASSERT_EQ(r.update("animation: a 1s linear", 0), GTEXT_CSS_OK);
  EXPECT_EQ(r.up.started, 1u);
  ASSERT_EQ(gtext_anim_css_set_count(r.set), 1u);
  GTEXT_Anim_Css_Info i = r.info(0, 0);
  EXPECT_EQ(std::string(i.animation_name.data, i.animation_name.length), "a");
  EXPECT_EQ(i.play_state, GTEXT_ANIM_PLAY_RUNNING) << "a pending play is running";
  EXPECT_TRUE(i.pending);
  EXPECT_FALSE(i.start_time.resolved) << "startTime is null until the animation is ready";
  ASSERT_TRUE(i.current_time.resolved);
  EXPECT_EQ(i.current_time.ms, 0);
  EXPECT_TRUE(i.relevant);
  EXPECT_EQ(r.frame(0), "start:a:0");
  i = r.info(0, 500);
  EXPECT_FALSE(i.pending);
  ASSERT_TRUE(i.start_time.resolved);
  EXPECT_EQ(i.start_time.ms, 0);
  EXPECT_EQ(i.current_time.ms, 500);
  EXPECT_DOUBLE_EQ(i.computed.progress, 0.5);
  EXPECT_EQ(i.computed.phase, GTEXT_ANIM_PHASE_ACTIVE);
  EXPECT_EQ(std::string(i.easing.data, i.easing.length), "linear");
  EXPECT_EQ(i.timing.duration, 1000);
}

/* "If there is no @keyframes at-rule with <keyframes-name> matching name (or if name is none), abort this procedure. In this case no animation is generated". */
TEST(AnimCssSet, WithoutKeyframesOrWithNoneThereIsNoAnimation) {
  Rig r;
  ASSERT_EQ(r.update("animation: nokeyframes 1s", 0), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_anim_css_set_count(r.set), 0u);
  ASSERT_EQ(r.update("animation-name: none; animation-duration: 3s", 0), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_anim_css_set_count(r.set), 0u);
  ASSERT_EQ(r.update("color: red", 0), GTEXT_CSS_OK);
  EXPECT_EQ(r.up.started + r.up.updated + r.up.cancelled, 0u);
  EXPECT_EQ(gtext_anim_css_set_update(nullptr, nullptr, nullptr, 0, nullptr), GTEXT_CSS_E_INVALID);
  GTEXT_Anim_Css_Info i{};
  EXPECT_EQ(gtext_anim_css_set_info(r.set, 0, 0, &i), GTEXT_CSS_E_INVALID);
}

/* The table of "Event dispatch": "idle or before -> active: animationstart" with the interval start, "active -> after: animationend" with the interval end; a positive delay keeps the start for later. */
TEST(AnimCssSet, TheEventsAreStartAndEndAtTheirPhaseChanges) {
  Rig r;
  ASSERT_EQ(r.update("animation: a 1s linear", 0), GTEXT_CSS_OK);
  EXPECT_EQ(r.frame(0), "start:a:0");
  EXPECT_EQ(r.frame(500), "");
  EXPECT_EQ(r.frame(1000), "end:a:1");
  EXPECT_EQ(r.frame(1000), "") << "the same time asked again reports nothing";
  Rig d;
  ASSERT_EQ(d.update("animation: a 1s linear 500ms", 0), GTEXT_CSS_OK);
  EXPECT_EQ(d.frame(0), "") << "idle -> before: nothing";
  EXPECT_EQ(d.frame(499), "");
  EXPECT_EQ(d.frame(500), "start:a:0");
  EXPECT_EQ(d.frame(1500), "end:a:1");
  Rig n;
  ASSERT_EQ(n.update("animation: a 1s linear -500ms", 0), GTEXT_CSS_OK);
  EXPECT_EQ(n.frame(0), "start:a:0.5") << "the interval start of a negative delay is min(-delay, active duration)";
  EXPECT_EQ(n.frame(5000), "end:a:1");
}

/* "idle or before -> after: animationstart, animationend", the frame that skips a phase. */
TEST(AnimCssSet, AFrameThatSkipsAPhaseReportsTheEventsOfEach) {
  Rig r;
  ASSERT_EQ(r.update("animation: a 1s linear 1s", 0), GTEXT_CSS_OK);
  EXPECT_EQ(r.frame(5000), "start:a:0 end:a:1");
  Rig b;
  ASSERT_EQ(b.update("animation: a 1s linear 1s", 0), GTEXT_CSS_OK);
  EXPECT_EQ(b.frame(0), "");
  EXPECT_EQ(b.frame(5000), "start:a:0 end:a:1") << "before -> after";
}

/* "active -> active and the current iteration ... has changed since the previous animation frame: animationiteration", with "(iteration boundary - iteration start) x iteration duration"; a frame that
   crosses several boundaries reports one. */
TEST(AnimCssSet, AnIterationBoundaryIsAnIterationEventOncePerFrame) {
  Rig r;
  ASSERT_EQ(r.update("animation: a 1s linear 3", 0), GTEXT_CSS_OK);
  EXPECT_EQ(r.frame(0), "start:a:0");
  EXPECT_EQ(r.frame(1000), "iteration:a:1");
  EXPECT_EQ(r.frame(1500), "");
  EXPECT_EQ(r.frame(2500), "iteration:a:2");
  EXPECT_EQ(r.frame(3000), "end:a:3");
  Rig s;
  ASSERT_EQ(s.update("animation: a 1s linear 5", 0), GTEXT_CSS_OK);
  EXPECT_EQ(s.frame(0), "start:a:0");
  EXPECT_EQ(s.frame(3500), "iteration:a:3") << "one event for three boundaries, at the iteration the animation is in";
}

/* The scheduled event time is "when the event would ideally have been dispatched": the start time and the delay for the start, the end of the effect for the end, the boundary for an iteration. */
TEST(AnimCssSet, TheScheduledTimeIsTheIdealMomentOfTheEvent) {
  Rig r;
  ASSERT_EQ(r.update("animation: a 1s linear 500ms 2", 100), GTEXT_CSS_OK);
  EXPECT_EQ(r.frame(100, true), "");
  EXPECT_EQ(r.frame(5000, true), "start:a:0@600 end:a:2@2600") << "start time 100: the start at 100 + 500, the end at 100 + 500 + 2000; the frame's time is not the event's";
  Rig i;
  ASSERT_EQ(i.update("animation: a 1s linear 3", 0), GTEXT_CSS_OK);
  EXPECT_EQ(i.frame(0, true), "start:a:0@0");
  EXPECT_EQ(i.frame(1300, true), "iteration:a:1@1000");
}

/* "not idle and not after -> idle: animationcancel", with "the active time of the animation at the moment it was cancelled calculated using a fill mode of both": removing the name cancels. One that
   no frame saw has nothing to report; one that had finished has already said its end. */
TEST(AnimCssSet, RemovingTheNameCancelsAndReportsWhatAFrameHadSeen) {
  Rig r;
  ASSERT_EQ(r.update("animation: a 1s linear", 0), GTEXT_CSS_OK);
  EXPECT_EQ(r.frame(0), "start:a:0");
  ASSERT_EQ(r.update("color: red", 400), GTEXT_CSS_OK);
  EXPECT_EQ(r.up.cancelled, 1u);
  EXPECT_EQ(gtext_anim_css_set_count(r.set), 0u);
  EXPECT_EQ(r.frame(400, true), "cancel:a:0.4@400");
  EXPECT_EQ(r.frame(500), "");
  Rig unseen;
  ASSERT_EQ(unseen.update("animation: a 1s", 0), GTEXT_CSS_OK);
  ASSERT_EQ(unseen.update("color: red", 10), GTEXT_CSS_OK);
  EXPECT_EQ(unseen.frame(20), "") << "idle -> idle";
  Rig done;
  ASSERT_EQ(done.update("animation: a 1s linear", 0), GTEXT_CSS_OK);
  EXPECT_EQ(done.frame(2000), "start:a:0 end:a:1");
  ASSERT_EQ(done.update("color: red", 2100), GTEXT_CSS_OK);
  EXPECT_EQ(done.frame(2100), "") << "after: its end was reported";
  Rig gone;
  ASSERT_EQ(gone.update("animation: a 1s linear", 0), GTEXT_CSS_OK);
  EXPECT_EQ(gone.frame(0), "start:a:0");
  ASSERT_EQ(gtext_anim_css_set_cancel_all(gone.set, 250), GTEXT_CSS_OK);
  EXPECT_EQ(gone.frame(250), "cancel:a:0.25") << "an element that is no longer rendered terminates its animations";
}

/* "the resolved value of animation-play-state ... is newly paused: pause an animation" holds the current time and unresolves the start time; "newly running: play an animation" gives it a start time
   at the next frame that is the time less the time held. */
TEST(AnimCssSet, PausingHoldsTheCurrentTimeAndRunningAgainGivesAStartTimeLessIt) {
  Rig r;
  ASSERT_EQ(r.update("animation: a 1s linear", 0), GTEXT_CSS_OK);
  EXPECT_EQ(r.frame(0), "start:a:0");
  ASSERT_EQ(r.update("animation: a 1s linear paused", 300), GTEXT_CSS_OK);
  EXPECT_EQ(r.up.updated, 1u);
  GTEXT_Anim_Css_Info i = r.info(0, 300);
  EXPECT_TRUE(i.pending) << "a pause task";
  EXPECT_EQ(i.play_state, GTEXT_ANIM_PLAY_PAUSED);
  EXPECT_EQ(i.current_time.ms, 300);
  EXPECT_EQ(r.info(0, 800).current_time.ms, 300) << "held";
  EXPECT_EQ(r.frame(800), "");
  i = r.info(0, 900);
  EXPECT_FALSE(i.pending);
  EXPECT_FALSE(i.start_time.resolved);
  EXPECT_EQ(i.play_state, GTEXT_ANIM_PLAY_PAUSED);
  EXPECT_EQ(i.current_time.ms, 300);
  ASSERT_EQ(r.update("animation: a 1s linear running", 1000), GTEXT_CSS_OK);
  i = r.info(0, 1000);
  EXPECT_TRUE(i.pending) << "a play task";
  EXPECT_EQ(i.play_state, GTEXT_ANIM_PLAY_RUNNING);
  EXPECT_FALSE(i.start_time.resolved);
  EXPECT_EQ(i.current_time.ms, 300);
  EXPECT_EQ(r.frame(1000), "");
  i = r.info(0, 1200);
  ASSERT_TRUE(i.start_time.resolved);
  EXPECT_EQ(i.start_time.ms, 700);
  EXPECT_EQ(i.current_time.ms, 500);
  /* pausing and playing again before a frame leaves the animation as it was */
  Rig q;
  ASSERT_EQ(q.update("animation: a 1s linear", 0), GTEXT_CSS_OK);
  EXPECT_EQ(q.frame(0), "start:a:0");
  ASSERT_EQ(q.update("animation: a 1s linear paused", 100), GTEXT_CSS_OK);
  ASSERT_EQ(q.update("animation: a 1s linear", 100), GTEXT_CSS_OK);
  i = q.info(0, 400);
  EXPECT_FALSE(i.pending);
  EXPECT_EQ(i.play_state, GTEXT_ANIM_PLAY_RUNNING);
  EXPECT_EQ(i.current_time.ms, 400);
  /* a paused animation made paused: a pause task, held at 0 */
  Rig p;
  ASSERT_EQ(p.update("animation: a 1s linear paused", 0), GTEXT_CSS_OK);
  EXPECT_EQ(p.info(0, 0).play_state, GTEXT_ANIM_PLAY_PAUSED);
  EXPECT_EQ(p.frame(0), "start:a:0") << "a paused animation at its start is in its active phase";
  EXPECT_EQ(p.info(0, 5000).current_time.ms, 0);
}

/* "Within the set of CSS Animations with an owning element ... sort A and B based on their position in the computed value of the 'animation-name' property": the order of the array is the order of the list,
   and an animation that stays keeps its identity when the list is reordered. */
TEST(AnimCssSet, TheCompositeOrderIsThePositionInTheNameListAndAReorderKeepsIdentity) {
  Rig r;
  ASSERT_EQ(r.update("animation-name: a, b; animation-duration: 1s", 0), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_anim_css_set_count(r.set), 2u);
  const GTEXT_Anim_Css_Info a = r.info(0, 0), b = r.info(1, 0);
  EXPECT_EQ(std::string(a.animation_name.data, a.animation_name.length), "a");
  EXPECT_EQ(std::string(b.animation_name.data, b.animation_name.length), "b");
  EXPECT_EQ(a.order, 0u);
  EXPECT_EQ(b.order, 1u);
  EXPECT_NE(a.id, b.id);
  ASSERT_EQ(r.update("animation-name: b, a; animation-duration: 1s", 10), GTEXT_CSS_OK);
  EXPECT_EQ(r.up.started, 0u);
  EXPECT_EQ(r.up.updated, 2u);
  const GTEXT_Anim_Css_Info b2 = r.info(0, 10), a2 = r.info(1, 10);
  EXPECT_EQ(b2.id, b.id);
  EXPECT_EQ(a2.id, a.id);
  EXPECT_EQ(b2.order, 0u);
  EXPECT_EQ(a2.order, 1u);
  /* a name twice is two animations; the n-th effect of a name is the n-th animation of it */
  Rig t;
  ASSERT_EQ(t.update("animation-name: a, a; animation-duration: 1s, 2s", 0), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_anim_css_set_count(t.set), 2u);
  EXPECT_EQ(t.info(0, 0).timing.duration, 1000);
  EXPECT_EQ(t.info(1, 0).timing.duration, 2000);
  const uint64_t second = t.info(1, 0).id;
  ASSERT_EQ(t.update("animation-name: a, a; animation-duration: 1s, 3s", 5), GTEXT_CSS_OK);
  EXPECT_EQ(t.info(1, 5).id, second);
  EXPECT_EQ(t.info(1, 5).timing.duration, 3000);
  EXPECT_EQ(t.up.started, 0u);
}

/* A change of the properties updates the effect ("Changes to any of the animation properties ... cause the corresponding CSSAnimation object and its associated objects to be updated"), and the
   timing members that the API has taken over are not ("After a successful call to updateTiming() ... for each property included"). */
TEST(AnimCssSet, UpdateTimingTakesOverTheMembersItNamesAndNoOthers) {
  Rig r;
  ASSERT_EQ(r.update("animation: a 1s linear 100ms", 0), GTEXT_CSS_OK);
  EXPECT_EQ(r.frame(0), "");
  const uint64_t id = r.info(0, 0).id;
  GTEXT_Anim_Optional_Timing in{};
  in.present = GTEXT_ANIM_TIMING_DURATION;
  in.values.duration = 2000;
  ASSERT_EQ(gtext_anim_css_set_update_timing(r.set, R(), id, &in, nullptr, 0), GTEXT_ANIM_OK);
  EXPECT_EQ(r.info(0, 0).timing.duration, 2000);
  ASSERT_EQ(r.update("animation: a 3s linear 200ms", 50), GTEXT_CSS_OK);
  EXPECT_EQ(r.info(0, 50).timing.duration, 2000) << "animation-duration is no longer reflected";
  EXPECT_EQ(r.info(0, 50).timing.delay, 200) << "animation-delay still is";
  /* an easing is parsed and kept as text, and is not reflected either afterwards */
  GTEXT_Anim_Optional_Timing e{};
  e.present = GTEXT_ANIM_TIMING_EASING;
  ASSERT_EQ(gtext_anim_css_set_update_timing(r.set, R(), id, &e, "ease-in", 7), GTEXT_ANIM_OK);
  GTEXT_Anim_Css_Info i = r.info(0, 50);
  EXPECT_EQ(std::string(i.easing.data, i.easing.length), "ease-in");
  ASSERT_EQ(r.update("animation: a 3s ease-out 200ms", 60), GTEXT_CSS_OK);
  i = r.info(0, 60);
  EXPECT_EQ(std::string(i.easing.data, i.easing.length), "ease-in");
  /* the refusals change nothing */
  GTEXT_Anim_Optional_Timing bad{};
  bad.present = GTEXT_ANIM_TIMING_ITERATION_START | GTEXT_ANIM_TIMING_DELAY;
  bad.values.iteration_start = -1;
  bad.values.delay = 7;
  EXPECT_EQ(gtext_anim_css_set_update_timing(r.set, R(), id, &bad, nullptr, 0), GTEXT_ANIM_TYPE_ERROR);
  EXPECT_EQ(r.info(0, 60).timing.delay, 200);
  EXPECT_EQ(gtext_anim_css_set_update_timing(r.set, R(), id, &e, "nonsense", 8), GTEXT_ANIM_TYPE_ERROR);
  i = r.info(0, 60);
  EXPECT_EQ(std::string(i.easing.data, i.easing.length), "ease-in");
  EXPECT_EQ(gtext_anim_css_set_update_timing(r.set, R(), id + 100, &in, nullptr, 0), GTEXT_ANIM_INVALID);
  EXPECT_EQ(gtext_anim_css_set_update_timing(r.set, nullptr, id, &in, nullptr, 0), GTEXT_ANIM_INVALID);
}

/* "Relevant animations": the effect is current or in effect. A running animation, one in its delay, and one that fills forwards are relevant; one that is over with no fill is not. */
TEST(AnimCssSet, RelevanceIsCurrentOrInEffect) {
  Rig r;
  ASSERT_EQ(r.update("animation: a 1s linear", 0), GTEXT_CSS_OK);
  r.frame(0);
  EXPECT_TRUE(r.info(0, 500).relevant);
  EXPECT_FALSE(r.info(0, 1500).relevant) << "finished, fill none";
  EXPECT_EQ(r.info(0, 1500).play_state, GTEXT_ANIM_PLAY_FINISHED);
  Rig f;
  ASSERT_EQ(f.update("animation: a 1s linear forwards", 0), GTEXT_CSS_OK);
  f.frame(0);
  EXPECT_TRUE(f.info(0, 1500).relevant) << "in effect: the fill applies";
  Rig d;
  ASSERT_EQ(d.update("animation: a 1s linear 2s", 0), GTEXT_CSS_OK);
  d.frame(0);
  EXPECT_TRUE(d.info(0, 1000).relevant) << "current: before its active interval, with a playback rate above 0";
}

/* The sampler of the style gets the current time of the animation by its name and its place in the list. */
TEST(AnimCssSet, TheCurrentTimeIsFoundByNameAndPlace) {
  Rig r;
  ASSERT_EQ(r.update("animation-name: a, b; animation-duration: 1s", 0), GTEXT_CSS_OK);
  r.frame(0);
  GTEXT_CSS_String a{"a", 1}, b{"b", 1};
  double ms = -1;
  EXPECT_TRUE(gtext_anim_css_set_current_time(r.set, &a, 0, 250, &ms));
  EXPECT_EQ(ms, 250);
  EXPECT_TRUE(gtext_anim_css_set_current_time(r.set, &b, 1, 400, &ms));
  EXPECT_EQ(ms, 400);
  EXPECT_FALSE(gtext_anim_css_set_current_time(r.set, &b, 0, 400, &ms)) << "b is at place 1";
  Rig p;
  ASSERT_EQ(p.update("animation: a 1s paused", 0), GTEXT_CSS_OK);
  EXPECT_TRUE(gtext_anim_css_set_current_time(p.set, &a, 0, 900, &ms));
  EXPECT_EQ(ms, 0) << "a paused animation holds its time";
}

/* A set hands out ids that no other set of its owner does, and a set that is cleared forgets without a word. */
TEST(AnimCssSet, IdsFollowTheOwnerAndAClearedSetIsSilent) {
  Rig r;
  gtext_anim_css_set_start_ids_after(r.set, 40);
  ASSERT_EQ(r.update("animation: a 1s", 0), GTEXT_CSS_OK);
  EXPECT_EQ(r.info(0, 0).id, 41u);
  EXPECT_EQ(gtext_anim_css_set_last_id(r.set), 41u);
  const uint64_t v = gtext_anim_css_set_version(r.set);
  r.frame(0);
  gtext_anim_css_set_clear(r.set);
  EXPECT_EQ(gtext_anim_css_set_count(r.set), 0u);
  EXPECT_NE(gtext_anim_css_set_version(r.set), v);
  EXPECT_EQ(r.frame(100), "");
  EXPECT_EQ(gtext_anim_css_set_version(nullptr), 0u);
  EXPECT_EQ(gtext_anim_css_set_count(nullptr), 0u);
}

/* All or nothing: every allocation of a style change event and of a frame fails in turn, and the set and the list are as they were; the same call then does it. */
TEST(AnimCssSetOom, EveryAllocationOfAnUpdateAndAFrameFailsCleanly) {
  Frames frames(kFrames);
  Effects first("animation: a 1s linear 2");
  Effects second("animation-name: b, a; animation-duration: 2s; animation-timing-function: steps(3), ease");
  size_t failures = 0;
  for (size_t n = 1; n < 10000; n++) {
    FaultAllocator fa;
    bool done = false;
    {
      GTEXT_Anim_Css_Set * set = nullptr;
      ASSERT_EQ(gtext_anim_css_set_new(&fa.alloc, &set), GTEXT_CSS_OK);
      ASSERT_EQ(gtext_anim_css_set_update(set, first.list, frames.set, 0, nullptr), GTEXT_CSS_OK);
      fa.arm(n);
      const uint64_t v = gtext_anim_css_set_version(set);
      const GTEXT_CSS_Status st = gtext_anim_css_set_update(set, second.list, frames.set, 100, nullptr);
      if (!fa.failed) {
        EXPECT_EQ(st, GTEXT_CSS_OK) << n;
        EXPECT_EQ(gtext_anim_css_set_count(set), 2u);
        done = true;
      }
      else {
        failures++;
        EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
        EXPECT_EQ(gtext_anim_css_set_version(set), v) << n << ": a failed update changes nothing";
        EXPECT_EQ(gtext_anim_css_set_count(set), 1u) << n;
        fa.disarm();
        EXPECT_EQ(gtext_anim_css_set_update(set, second.list, frames.set, 100, nullptr), GTEXT_CSS_OK) << n;
      }
      fa.disarm();
      /* the frame, with the events list's allocator failing */
      GTEXT_Anim_Events * ev = nullptr;
      ASSERT_EQ(gtext_anim_events_new(&fa.alloc, &ev), GTEXT_ANIM_OK);
      fa.arm(1);
      EXPECT_EQ(gtext_anim_css_set_frame(set, 100, ev), GTEXT_CSS_E_OOM) << n;
      fa.disarm();
      EXPECT_EQ(gtext_anim_css_set_frame(set, 100, ev), GTEXT_CSS_OK) << n << ": the frame is made again whole";
      EXPECT_EQ(gtext_anim_events_count(ev), 2u) << n;
      gtext_anim_events_free(ev);
      gtext_anim_css_set_free(set);
    }
    EXPECT_EQ(fa.live.size(), 0u) << n << ": a block is left";
    EXPECT_EQ(fa.bad_frees, 0u) << n;
    if (done || ::testing::Test::HasFailure()) {
      break;
    }
  }
  EXPECT_GT(failures, 0u);
}

}  // namespace
