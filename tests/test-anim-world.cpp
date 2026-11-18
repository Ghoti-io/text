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
 * The animations of a document (animation/world.h) over the tracker of the CSSOM, against Web Animations 1 ("Relevant animations", "Animation events", "Update animations and send events"), CSS
 * Animations 2 and CSS Transitions 2 ("Animation composite order"): `getAnimations()` of an element, of its subtree and of a document, the sampler's time for a CSS animation, and a frame's events in
 * the order of their scheduled times.
 */

#include <cstdio>
#include <set>
#include <string>

#include <gtest/gtest.h>

#include "cssom_computed_test_support.h"

#include <ghoti.io/text/animation/animation.h>
#include <ghoti.io/text/animation/world.h>

using namespace cc;

namespace {

/// A page, a tracker over its document, the layout hooks that give the tracker's sets and the CSS animations' time, and a world over both.
struct Wd {
  GTEXT_CSSOM_Layout_Interface iface{};
  GTEXT_CSSOM_Transition_Tracker * tracker = nullptr;
  GTEXT_Anim_World * world = nullptr;
  Cs m;
  explicit Wd(const std::string & html, const GTEXT_Allocator * world_alloc = nullptr, const GTEXT_Allocator * tracker_alloc = nullptr)
      : m(html, [this](GTEXT_CSSOM_Config & c) {
          iface.size = sizeof iface;
          iface.user = this;
          iface.transitions = &Wd::sets;
          iface.transition_version = &Wd::ver;
          iface.animation_time = &Wd::time;
          iface.timeline_version = &Wd::ver;
          c.layout = &iface;
        }) {
    EXPECT_EQ(gtext_cssom_transition_tracker_new(m.doc->d, tracker_alloc, &tracker), GTEXT_CSS_OK);
    EXPECT_EQ(gtext_anim_world_new(m.doc->d, tracker, world_alloc, &world), GTEXT_ANIM_OK);
  }
  Wd(const Wd &) = delete;
  Wd & operator=(const Wd &) = delete;
  ~Wd() {
    gtext_anim_world_free(world);
    gtext_cssom_transition_tracker_free(tracker);
  }
  static GTEXT_CSS_Status sets(void * user, const void * element, const GTEXT_CSS_Selector_List * pseudo, const GTEXT_CSS_Transition_Set ** set, double * ms) {
    Wd * self = static_cast<Wd *>(user);
    if (!self->tracker) {
      *set = nullptr;
      *ms = 0;
      return GTEXT_CSS_OK;
    }
    return gtext_cssom_transition_tracker_lookup(self->tracker, element, pseudo, set, ms);
  }
  static GTEXT_CSS_Status time(void * user, const void * element, const GTEXT_CSS_Selector_List * pseudo, const GTEXT_CSS_Anim_Effect * effect, double * ms, bool * known) {
    Wd * self = static_cast<Wd *>(user);
    *known = false;
    if (!self->tracker) {
      return GTEXT_CSS_OK;
    }
    return gtext_cssom_transition_tracker_animation_time(self->tracker, element, pseudo, effect, ms, known);
  }
  static uint64_t ver(void * user) {
    Wd * self = static_cast<Wd *>(user);
    return self->tracker ? gtext_cssom_transition_tracker_version(self->tracker) : 0;
  }
  void flush(const char * selector, const char * pseudo = nullptr) {
    ASSERT_EQ(gtext_cssom_transition_tracker_flush(tracker, m.handle(selector), pseudo, pseudo ? std::strlen(pseudo) : 0), GTEXT_CSS_OK);
  }
  void at(double ms) {
    gtext_cssom_transition_tracker_set_time(tracker, ms);
  }
  /// How many transitions the tracker holds as running for an element (idle ones included: they are still in the set).
  size_t running(const char * selector) {
    const GTEXT_CSS_Transition_Set * set = nullptr;
    double ms = 0;
    EXPECT_EQ(gtext_cssom_transition_tracker_lookup(tracker, m.handle(selector), nullptr, &set, &ms), GTEXT_CSS_OK);
    return gtext_css_transition_set_running_count(set);
  }
  void set_class(const char * selector, const std::string & value) {
    GTEXT_Markup_Node * e = m.el(selector);
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(gtext_markup_element_set_attribute(e, nullptr, nullptr, "class", value.data(), value.size()), GTEXT_MARKUP_OK);
  }
  /// `getAnimations()` of an element as text: `kind:name` for each, joined by spaces (`T` a transition, `A` an animation); the document's with a null selector.
  std::string list(const char * selector, bool subtree = false, const char * pseudo = nullptr) {
    GTEXT_Anim_List * l = nullptr;
    EXPECT_EQ(gtext_anim_world_get_animations(world, selector ? m.handle(selector) : nullptr, pseudo, pseudo ? std::strlen(pseudo) : 0, subtree, &l), GTEXT_ANIM_OK);
    std::string out;
    for (size_t i = 0; i < gtext_anim_list_count(l); i++) {
      GTEXT_Anim_Info a{};
      EXPECT_TRUE(gtext_anim_list_at(l, i, &a));
      out += std::string(i ? " " : "") + (a.kind == GTEXT_ANIM_KIND_CSS_TRANSITION ? "T:" : "A:") + std::string(a.name.data, a.name.length);
      if (a.pseudo_element.length) {
        out += "(" + std::string(a.pseudo_element.data, a.pseudo_element.length) + ")";
      }
    }
    gtext_anim_list_free(l);
    return out;
  }
  GTEXT_Anim_Info first(const char * selector) {
    GTEXT_Anim_List * l = nullptr;
    EXPECT_EQ(gtext_anim_world_get_animations(world, m.handle(selector), nullptr, 0, false, &l), GTEXT_ANIM_OK);
    GTEXT_Anim_Info a{};
    EXPECT_TRUE(gtext_anim_list_at(l, 0, &a));
    gtext_anim_list_free(l);
    return a;   /* the strings are the tracker's: valid until it is updated */
  }
  /// One frame of the world as text: `type:name:elapsed@scheduled` for each event, joined by spaces, with the element's id when `ids`.
  std::string frame(bool ids = false) {
    GTEXT_Anim_Events * ev = nullptr;
    EXPECT_EQ(gtext_anim_world_update(world, &ev), GTEXT_ANIM_OK);
    return events_text(ev, ids);
  }
  std::string events_text(GTEXT_Anim_Events * ev, bool ids) {
    static const char * const names[] = {"animationstart", "animationiteration", "animationend", "animationcancel", "transitionrun", "transitionstart", "transitionend", "transitioncancel", "finish", "cancel", "remove"};
    std::string out;
    for (size_t i = 0; i < gtext_anim_events_count(ev); i++) {
      GTEXT_Anim_Event e{};
      EXPECT_TRUE(gtext_anim_events_at(ev, i, &e));
      char buf[64];
      std::snprintf(buf, sizeof buf, ":%g@%g", e.elapsed_time, e.scheduled_time.ms);
      std::string who;
      if (ids) {
        for (const char * id : {"#a", "#b", "#c", "#t", "#p"}) {
          if (m.handle(id) == e.element) {
            who = std::string(id) + " ";
          }
        }
      }
      out += std::string(i ? " " : "") + who + names[e.type] + ":" + std::string(e.name.data, e.name.length) + buf;
      if (e.pseudo_element.length) {
        out += "(" + std::string(e.pseudo_element.data, e.pseudo_element.length) + ")";
      }
    }
    gtext_anim_events_free(ev);
    return out;
  }
};

std::string page(const std::string & css, const std::string & body = "<div id=t></div>") {
  return "<!DOCTYPE html><html><head><style id=s>" + css + "</style></head><body>" + body + "</body></html>";
}

const char * kKeys = "@keyframes a { from { opacity: 0 } to { opacity: 1 } } @keyframes b { from { color: red } to { color: blue } } ";

/* "Element.getAnimations()": the relevant animations whose target is the element. A style change event makes a CSS animation, which is pending until the frame, and then has a start time and a current time;
   the style of the element at the time is the animation's value at its current time (the tracker is the layout's `animation_time`). */
TEST(AnimWorld, AnElementsCssAnimationIsMadeByTheStyleChangeEventAndReadyAtTheFrame) {
  Wd w(page(std::string(kKeys) + "#t { animation: a 1s linear }"));
  w.at(0);
  EXPECT_EQ(w.list("#t"), "") << "no style change event yet";
  w.flush("#t");
  EXPECT_EQ(w.list("#t"), "A:a");
  GTEXT_Anim_Info a = w.first("#t");
  EXPECT_EQ(a.kind, GTEXT_ANIM_KIND_CSS_ANIMATION);
  EXPECT_TRUE(a.pending);
  EXPECT_FALSE(a.start_time.resolved);
  EXPECT_EQ(a.current_time.ms, 0);
  EXPECT_EQ(a.play_state, GTEXT_ANIM_PLAY_RUNNING);
  EXPECT_EQ(w.m.value("#t", "opacity"), "0") << "at its current time 0";
  EXPECT_EQ(w.frame(), "animationstart:a:0@0");
  a = w.first("#t");
  EXPECT_FALSE(a.pending);
  EXPECT_TRUE(a.start_time.resolved);
  EXPECT_EQ(a.start_time.ms, 0);
  w.at(500);
  EXPECT_EQ(w.first("#t").current_time.ms, 500);
  EXPECT_EQ(w.m.value("#t", "opacity"), "0.5") << "the sampler's time is the animation's current time";
  w.at(1000);
  EXPECT_EQ(w.frame(), "animationend:a:1@1000");
  EXPECT_EQ(w.list("#t"), "") << "finished with no fill: not relevant";
  EXPECT_EQ(w.m.value("#t", "opacity"), "1") << "and it applies nothing";
}

/* "Animation composite order": "CSS Animations with an owning element have a later composite order than CSS Transitions", and the animations of one element are in the order of the animation-name list. */
TEST(AnimWorld, TheTransitionsOfAnElementComeBeforeItsAnimationsAndTheAnimationsFollowTheNameList) {
  Wd w(page(std::string(kKeys) + "#t { width: 10px; animation: b 5s, a 5s; transition: width 1s linear } #t.on { width: 20px }"));
  w.at(0);
  w.flush("#t");
  EXPECT_EQ(w.list("#t"), "A:b A:a");
  w.set_class("#t", "on");
  w.flush("#t");
  EXPECT_EQ(w.list("#t"), "T:width A:b A:a");
  GTEXT_Anim_Info t = w.first("#t");
  EXPECT_EQ(t.kind, GTEXT_ANIM_KIND_CSS_TRANSITION);
  EXPECT_EQ(t.timing.fill, GTEXT_ANIM_FILL_BACKWARDS) << "the transition effect has fill backwards";
  EXPECT_EQ(t.timing.duration, 1000);
  EXPECT_EQ(t.computed.phase, GTEXT_ANIM_PHASE_ACTIVE);
  EXPECT_EQ(std::string(t.start_value.data, t.start_value.length), "10px");
  EXPECT_EQ(std::string(t.end_value.data, t.end_value.length), "20px");
}

/* "Document.getAnimations()": every relevant animation in the document, transitions first, each class in the tree order of the owning elements: not the order the elements were seen in. */
TEST(AnimWorld, ADocumentsAnimationsAreInTreeOrderWhateverTheOrderOfTheStyleChangeEvents) {
  Wd w(page(std::string(kKeys) + "div { width: 10px; transition: width 1s linear } #a, #c { animation: a 5s } #b.on { width: 20px } #a.on { width: 30px }",
      "<div id=a></div><div id=b></div><div id=c></div>"));
  w.at(0);
  w.flush("#c");
  w.flush("#b");
  w.flush("#a");
  w.set_class("#b", "on");
  w.set_class("#a", "on");
  w.flush("#b");
  w.flush("#a");
  EXPECT_EQ(w.list(nullptr), "T:width T:width A:a A:a");
  GTEXT_Anim_List * l = nullptr;
  ASSERT_EQ(gtext_anim_world_get_animations(w.world, nullptr, nullptr, 0, false, &l), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_list_count(l), 4u);
  GTEXT_Anim_Info x[4];
  for (size_t i = 0; i < 4; i++) {
    ASSERT_TRUE(gtext_anim_list_at(l, i, &x[i]));
  }
  EXPECT_EQ(x[0].element, w.m.handle("#a")) << "the transitions: a before b, though b was seen first";
  EXPECT_EQ(x[1].element, w.m.handle("#b"));
  EXPECT_EQ(x[2].element, w.m.handle("#a")) << "the animations: a before c";
  EXPECT_EQ(x[3].element, w.m.handle("#c"));
  EXPECT_FALSE(gtext_anim_list_at(l, 4, &x[0]));
  gtext_anim_list_free(l);
}

/* "getAnimations({subtree: true})": the element's, its pseudo-elements' and its descendants'; without it, only the element's own. A pseudo-element is asked for by name. */
TEST(AnimWorld, SubtreeAddsTheDescendantsAndThePseudoElements) {
  Wd w(page(std::string(kKeys) + "#p { animation: a 5s } #t { animation: b 5s } #p::before { content: ''; animation: b 5s } #p::after { content: ''; animation: a 5s }",
      "<div id=p><div id=t></div></div>"));
  w.at(0);
  w.flush("#t");
  w.flush("#p", "::before");
  w.flush("#p", "::after");
  EXPECT_EQ(w.list("#p"), "A:a");
  EXPECT_EQ(w.list("#p", true), "A:a A:b(::before) A:a(::after) A:b") << "the element, ::before, ::after, then the element's children";
  EXPECT_EQ(w.list("#t"), "A:b");
  EXPECT_EQ(w.list("#p", false, "::before"), "A:b(::before)");
  EXPECT_EQ(w.list("#p", false, ":before"), "A:b(::before)") << "the legacy spelling is the pseudo-element";
  EXPECT_EQ(w.list("#p", true, "::before"), "A:b(::before)") << "a pseudo-element has no descendants";
  EXPECT_EQ(w.list(nullptr), "A:a A:b(::before) A:a(::after) A:b");
}

/* "Relevant animations": a finished animation with a fill is in effect and stays; an animation in its delay is current. A transition that has ended is not (its fill is backwards). */
TEST(AnimWorld, RelevanceFollowsTheFillAndThePhase) {
  Wd w(page(std::string(kKeys) + "#t { animation: a 1s linear forwards, b 1s linear 5s }"));
  w.at(0);
  w.flush("#t");
  w.frame();
  w.at(3000);
  EXPECT_EQ(w.list("#t"), "A:a A:b") << "a fills forwards; b is in its delay";
  w.at(7000);
  w.frame();
  EXPECT_EQ(w.list("#t"), "A:a");
}

/* "Update animations and send events": the transitions' events and the animations' in one list, sorted by the scheduled event time; transitions before animations between equal times. */
TEST(AnimWorld, AFrameIsOneListOfTheTransitionsAndTheAnimationsEventsInTheOrderOfTheirTimes) {
  Wd w(page(std::string(kKeys) + "#t { width: 10px; animation: a 1s linear; transition: width 100ms linear } #t.on { width: 20px }"));
  w.at(0);
  w.flush("#t");
  w.set_class("#t", "on");
  w.flush("#t");
  EXPECT_EQ(w.list("#t"), "T:width A:a");
  w.at(5000);
  EXPECT_EQ(w.frame(), "transitionrun:width:0@0 transitionstart:width:0@0 animationstart:a:0@0 transitionend:width:0.1@100 animationend:a:1@1000");
  EXPECT_EQ(w.frame(), "") << "the same time again";
}

/* "Sort ... by their scheduled event time ... Within events with equal scheduled event times, sort by their composite order": an event with no scheduled time (an animation with no start time) comes before every
   one that has one, and equal times keep the order they were appended in. */
TEST(AnimEventList, SortsByScheduledTimeUnresolvedFirstAndStably) {
  GTEXT_Anim_Events * ev = nullptr;
  ASSERT_EQ(gtext_anim_events_new(nullptr, &ev), GTEXT_ANIM_OK);
  struct In {
    uint64_t id;
    bool resolved;
    double ms;
  };
  const In in[] = {{1, true, 50}, {2, true, 10}, {3, false, 0}, {4, true, 10}, {5, true, 50}, {6, false, 0}, {7, true, 0}};
  for (const In & i : in) {
    GTEXT_Anim_Event e{};
    e.type = GTEXT_ANIM_EVENT_ANIMATIONSTART;
    e.animation = i.id;
    e.scheduled_time = GTEXT_Anim_Time{i.resolved, i.ms};
    ASSERT_EQ(gtext_anim_events_append(ev, &e), GTEXT_ANIM_OK);
  }
  gtext_anim_events_sort(ev);
  std::string order;
  for (size_t k = 0; k < gtext_anim_events_count(ev); k++) {
    GTEXT_Anim_Event e{};
    ASSERT_TRUE(gtext_anim_events_at(ev, k, &e));
    order += std::to_string(e.animation) + " ";
  }
  EXPECT_EQ(order, "3 6 7 2 4 1 5 ");
  gtext_anim_events_free(ev);
}

/* "terminate running animations with this owning element": a class that removes the animation cancels it, with the active time of the moment, and the event is the element's. */
TEST(AnimWorld, RemovingAnAnimationCancelsItAndReportsItAtTheElement) {
  Wd w(page(std::string(kKeys) + "#t { animation: a 1s linear } #t.off { animation: none }", "<div id=t></div>"));
  w.at(0);
  w.flush("#t");
  EXPECT_EQ(w.frame(true), "#t animationstart:a:0@0");
  w.at(400);
  w.set_class("#t", "off");
  w.flush("#t");
  EXPECT_EQ(w.list("#t"), "");
  EXPECT_EQ(w.frame(true), "#t animationcancel:a:0.4@400");
  EXPECT_EQ(w.frame(true), "");
  /* an element that is forgotten (it left the document) cancels what it had, and the next frame says so once */
  Wd f(page(std::string(kKeys) + "#t { animation: a 1s linear }"));
  f.at(0);
  f.flush("#t");
  EXPECT_EQ(f.frame(true), "#t animationstart:a:0@0");
  f.at(250);
  size_t gone = 0;
  ASSERT_EQ(gtext_cssom_transition_tracker_forget(f.tracker, f.m.handle("#t"), &gone), GTEXT_CSS_OK);
  EXPECT_EQ(gone, 1u);
  EXPECT_EQ(f.frame(true), "#t animationcancel:a:0.25@250") << "the element is gone from the tracker, and its cancellation is still the element's event";
  EXPECT_EQ(f.frame(), "");
}

/* An element that is not rendered ("display: none") has no animations ("terminate running animations with this owning element"); when it is rendered again its animations start anew. */
TEST(AnimWorld, DisplayNoneTerminatesTheAnimationsAndTheyStartAgain) {
  Wd w(page(std::string(kKeys) + "#t { animation: a 1s linear } #t.hide { display: none }"));
  w.at(0);
  w.flush("#t");
  EXPECT_EQ(w.frame(), "animationstart:a:0@0");
  w.at(300);
  w.set_class("#t", "hide");
  w.flush("#t");
  EXPECT_EQ(w.list("#t"), "");
  EXPECT_EQ(w.frame(), "animationcancel:a:0.3@300");
  w.at(500);
  w.set_class("#t", "");
  w.flush("#t");
  EXPECT_EQ(w.list("#t"), "A:a");
  EXPECT_EQ(w.first("#t").current_time.ms, 0);
  EXPECT_EQ(w.frame(), "animationstart:a:0@500");
}

/* A page that holds an Animation object reads it after it has left `getAnimations()`: `find` gives an animation that is finished and no longer relevant, and says it is gone once it was cancelled. */
TEST(AnimWorld, FindReadsAnAnimationThatHasLeftTheListAndSaysWhenItIsGone) {
  Wd w(page(std::string(kKeys) + "#t { animation: a 1s linear } #t.off { animation: none }"));
  w.at(0);
  w.flush("#t");
  w.frame();
  const GTEXT_Anim_Info a = w.first("#t");
  const uint64_t id = a.id;
  w.at(2000);
  EXPECT_EQ(w.list("#t"), "") << "finished with no fill: not relevant";
  bool found = false;
  GTEXT_Anim_Info info{};
  ASSERT_EQ(gtext_anim_world_find(w.world, GTEXT_ANIM_KIND_CSS_ANIMATION, id, &found, &info), GTEXT_ANIM_OK);
  ASSERT_TRUE(found);
  EXPECT_EQ(info.play_state, GTEXT_ANIM_PLAY_FINISHED);
  EXPECT_EQ(info.current_time.ms, 2000);
  EXPECT_EQ(std::string(info.name.data, info.name.length), "a");
  ASSERT_EQ(gtext_anim_world_find(w.world, GTEXT_ANIM_KIND_CSS_TRANSITION, id, &found, &info), GTEXT_ANIM_OK);
  EXPECT_FALSE(found) << "the id is of the other kind";
  w.set_class("#t", "off");
  w.flush("#t");
  ASSERT_EQ(gtext_anim_world_find(w.world, GTEXT_ANIM_KIND_CSS_ANIMATION, id, &found, &info), GTEXT_ANIM_OK);
  EXPECT_FALSE(found) << "cancelled: a caller reads it as idle";
  EXPECT_EQ(gtext_anim_world_find(nullptr, GTEXT_ANIM_KIND_CSS_ANIMATION, id, &found, &info), GTEXT_ANIM_INVALID);
}

/* The sampler asks for the time of an animation the tracker has not made: it is not known, and the layout answers as it would without one. */
TEST(AnimWorld, ASamplerTimeIsKnownOnlyForAnAnimationTheTrackerMade) {
  Wd w(page(std::string(kKeys) + "#t { animation: a 1s linear }"));
  GTEXT_CSS_Anim_Effect e{};
  e.name = GTEXT_CSS_String{"a", 1};
  double ms = -1;
  bool known = true;
  w.at(40);
  ASSERT_EQ(gtext_cssom_transition_tracker_animation_time(w.tracker, w.m.handle("#t"), nullptr, &e, &ms, &known), GTEXT_CSS_OK);
  EXPECT_FALSE(known);
  w.flush("#t");
  ASSERT_EQ(gtext_cssom_transition_tracker_animation_time(w.tracker, w.m.handle("#t"), nullptr, &e, &ms, &known), GTEXT_CSS_OK);
  EXPECT_TRUE(known);
  EXPECT_EQ(ms, 0) << "pending: the time is held at 0 until the frame";
  w.at(140);
  w.frame();
  w.at(240);
  ASSERT_EQ(gtext_cssom_transition_tracker_animation_time(w.tracker, w.m.handle("#t"), nullptr, &e, &ms, &known), GTEXT_CSS_OK);
  EXPECT_TRUE(known);
  EXPECT_EQ(ms, 200) << "the start time is the style change event's, at 40";
  e.index = 3;
  ASSERT_EQ(gtext_cssom_transition_tracker_animation_time(w.tracker, w.m.handle("#t"), nullptr, &e, &ms, &known), GTEXT_CSS_OK);
  EXPECT_FALSE(known) << "another place in the list";
  EXPECT_EQ(gtext_cssom_transition_tracker_animation_time(nullptr, nullptr, nullptr, &e, &ms, &known), GTEXT_CSS_E_INVALID);
}

/* The timing of a transition is the general model's (timing.h) over the transition's effect: delay, duration, fill backwards, one iteration, the matching easing. Before its delay the effect is in effect (the
   fill) with the progress of the start, and after its end it is not relevant. */
TEST(AnimWorld, TheTimingOfATransitionIsTheGeneralModelOverItsEffect) {
  Wd w(page("#t { width: 10px; transition: width 1s ease-in 400ms } #t.on { width: 20px }"));
  w.at(0);
  w.flush("#t");
  w.set_class("#t", "on");
  w.flush("#t");
  GTEXT_CSS_Easing * ease = nullptr;
  ASSERT_EQ(gtext_anim_easing_parse(cmp::R(), "ease-in", 7, nullptr, &ease), GTEXT_ANIM_OK);
  w.frame();   /* ready: the current time is the time since the style change event */
  struct Case {
    double ms;
    GTEXT_Anim_Phase phase;
    double input;   // the directed progress
  };
  for (const Case & c : {Case{0, GTEXT_ANIM_PHASE_BEFORE, 0}, Case{200, GTEXT_ANIM_PHASE_BEFORE, 0}, Case{400, GTEXT_ANIM_PHASE_ACTIVE, 0}, Case{900, GTEXT_ANIM_PHASE_ACTIVE, 0.5},
           Case{1399, GTEXT_ANIM_PHASE_ACTIVE, 0.999}}) {
    w.at(c.ms);
    const GTEXT_Anim_Info a = w.first("#t");
    EXPECT_EQ(a.computed.phase, c.phase) << c.ms;
    EXPECT_TRUE(a.computed.in_effect) << c.ms;
    EXPECT_EQ(a.computed.end_time, 1400) << c.ms;
    EXPECT_EQ(a.computed.active_duration, 1000) << c.ms;
    EXPECT_DOUBLE_EQ(a.computed.progress, gtext_css_easing_output(ease, c.input, c.phase == GTEXT_ANIM_PHASE_BEFORE)) << c.ms;
    EXPECT_EQ(a.computed.current_iteration, 0) << c.ms << " (in the before phase with a fill backwards the iteration is the first)";
    EXPECT_EQ(std::string(a.easing.data, a.easing.length), "ease-in");
    EXPECT_EQ(a.play_state, GTEXT_ANIM_PLAY_RUNNING);
  }
  gtext_css_easing_free(ease);
  w.at(1400);
  EXPECT_EQ(w.list("#t"), "") << "after its end the effect is neither current nor in effect";
}

/* The pending phase of a transition (Transitions 2, "Event dispatch"; Web Animations 1, "Waiting for the associated effect"): a frame at which the transitions are not ready reports the run, the next
   frame the start. The snapshot says pending until a ready frame has seen it, and a CSS animation made at the same event is not pending-phased (its events are the table's, as before). */
TEST(AnimWorld, AFrameThatIsNotReadyLeavesANewTransitionPendingAndReportsTheRunOnly) {
  Wd w(page(std::string(kKeys) + "#t { width: 10px; transition: width 100ms linear } #t.on { width: 20px }"));
  w.at(0);
  w.flush("#t");
  w.set_class("#t", "on");
  w.flush("#t");
  gtext_cssom_transition_tracker_set_frame_pending(w.tracker, true);
  w.at(0);
  EXPECT_EQ(w.frame(), "transitionrun:width:0@0");
  EXPECT_TRUE(w.first("#t").pending);
  w.at(16);
  EXPECT_EQ(w.frame(), "") << "a second frame that is not ready adds nothing";
  gtext_cssom_transition_tracker_set_frame_pending(w.tracker, false);
  w.at(32);
  EXPECT_EQ(w.frame(), "transitionstart:width:0@0");
  EXPECT_FALSE(w.first("#t").pending);
  w.at(100);
  EXPECT_EQ(w.frame(), "transitionend:width:0.1@100");
}

/* ---- an Animation holds the timing state of a CSS animation script has touched ---------------------------------------------------------------------- */

/* whether a frame's text has an event of exactly this type (not a longer one that ends in it: `animationcancel` is not `cancel`) */
bool has_event(const std::string & frame, const std::string & type) {
  return (" " + frame).find(" " + type + ":") != std::string::npos;
}

struct Ctl {
  Wd & w;
  GTEXT_Anim_Animation * a = nullptr;
  explicit Ctl(Wd & wd, const char * selector = "#t", GTEXT_Anim_Kind kind = GTEXT_ANIM_KIND_CSS_ANIMATION) : w(wd) {
    EXPECT_EQ(gtext_anim_world_control(w.world, kind, w.first(selector).id, &a), GTEXT_ANIM_OK);
  }
  GTEXT_Anim_Time now() const {
    return GTEXT_Anim_Time{true, gtext_anim_world_time(w.world)};
  }
  GTEXT_Anim_Animation_State st() const {
    GTEXT_Anim_Animation_State s;
    EXPECT_EQ(gtext_anim_animation_state(a, now(), &s), GTEXT_ANIM_OK);
    return s;
  }
  std::string notes() const {
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
};

TEST(AnimWorldControl, TheSameAnimationIsGivenForTheSameCssAnimationAndItsStateIsTheCssOnes) {
  Wd w(page(std::string(kKeys) + "#t { animation: a 1s linear }"));
  w.at(0);
  w.flush("#t");
  Ctl c(w);
  GTEXT_Anim_Animation * again = nullptr;
  ASSERT_EQ(gtext_anim_world_control(w.world, GTEXT_ANIM_KIND_CSS_ANIMATION, w.first("#t").id, &again), GTEXT_ANIM_OK);
  EXPECT_EQ(again, c.a);
  EXPECT_EQ(gtext_anim_world_control_count(w.world), 1u);
  auto s = c.st();
  EXPECT_TRUE(s.pending_play) << "the CSS animation is pending until the first frame";
  EXPECT_EQ(s.play_state, GTEXT_ANIM_PLAY_RUNNING);
  EXPECT_EQ(s.ready, GTEXT_ANIM_PROMISE_PENDING);
  EXPECT_TRUE(w.first("#t").pending);
  w.at(16);
  EXPECT_EQ(w.frame(), "animationstart:a:0@0");
  s = c.st();
  EXPECT_FALSE(s.pending);
  EXPECT_EQ(c.notes(), "RR2");
  EXPECT_EQ(s.ready, GTEXT_ANIM_PROMISE_RESOLVED);
  EXPECT_EQ(w.m.value("#t", "opacity"), "0.016") << "ready at the time of the style change event (0), as before the Animation held it: 16 ms of it have gone at the first frame";
}

TEST(AnimWorldControl, PauseHoldsTheStyleAndThePlayStateAndPlayResumesIt) {
  Wd w(page(std::string(kKeys) + "#t { animation: a 1s linear }"));
  w.at(0);
  w.flush("#t");
  Ctl c(w);
  w.at(0);
  w.frame();
  w.at(500);
  EXPECT_EQ(w.m.value("#t", "opacity"), "0.5");
  ASSERT_EQ(gtext_anim_animation_pause(c.a, c.now()), GTEXT_ANIM_OK);
  w.at(516);
  w.frame();
  EXPECT_EQ(c.st().play_state, GTEXT_ANIM_PLAY_PAUSED);
  w.at(900);
  EXPECT_EQ(w.m.value("#t", "opacity"), "0.516") << "held at the time the pause task ran";
  EXPECT_EQ(w.first("#t").play_state, GTEXT_ANIM_PLAY_PAUSED);
  ASSERT_EQ(gtext_anim_animation_play(c.a, c.now()), GTEXT_ANIM_OK);
  w.at(916);
  w.frame();
  w.at(1016);
  EXPECT_EQ(w.m.value("#t", "opacity"), "0.616");
}

TEST(AnimWorldControl, FinishEndsTheAnimationAtOnceWithItsEventsAndAFinishEvent) {
  Wd w(page(std::string(kKeys) + "#t { animation: a 1s linear }"));
  w.at(0);
  w.flush("#t");
  Ctl c(w);
  w.at(0);
  w.frame();
  w.at(100);
  ASSERT_EQ(gtext_anim_animation_finish(c.a, c.now()), GTEXT_ANIM_OK);
  EXPECT_EQ(c.notes(), "RR2 FR1");
  EXPECT_EQ(c.st().play_state, GTEXT_ANIM_PLAY_FINISHED);
  w.at(116);
  const std::string f = w.frame();
  EXPECT_NE(f.find("animationend:a:1@"), std::string::npos) << f;
  EXPECT_TRUE(has_event(f, "finish")) << f;
}

TEST(AnimWorldControl, CancelByScriptIsAnAnimationCancelAndACancelEventAndTheAnimationNoLongerApplies) {
  Wd w(page(std::string(kKeys) + "#t { animation: a 1s linear }"));
  w.at(0);
  w.flush("#t");
  Ctl c(w);
  w.at(0);
  w.frame();
  w.at(300);
  EXPECT_EQ(w.frame(), "");
  ASSERT_EQ(gtext_anim_animation_cancel(c.a, c.now()), GTEXT_ANIM_OK);
  EXPECT_EQ(c.notes(), "RR2 FJ1");
  EXPECT_EQ(c.st().play_state, GTEXT_ANIM_PLAY_IDLE);
  EXPECT_EQ(w.m.value("#t", "opacity"), "1") << "no effect: the property has its own value";
  w.at(316);
  const std::string f = w.frame();
  EXPECT_NE(f.find("animationcancel:a:0.3@"), std::string::npos) << f;
  EXPECT_TRUE(has_event(f, "cancel")) << f;
  EXPECT_EQ(w.list("#t"), "") << "an idle animation is not relevant";
}

TEST(AnimWorldControl, ReverseRunsTheAnimationBackwardsFromItsEnd) {
  Wd w(page(std::string(kKeys) + "#t { animation: a 1s linear }"));
  w.at(0);
  w.flush("#t");
  Ctl c(w);
  w.at(0);
  w.frame();
  ASSERT_EQ(gtext_anim_animation_finish(c.a, c.now()), GTEXT_ANIM_OK);
  w.at(16);
  w.frame();
  ASSERT_EQ(gtext_anim_animation_reverse(c.a, c.now()), GTEXT_ANIM_OK);
  w.at(32);
  w.frame();
  EXPECT_EQ(c.st().playback_rate, -1.0);
  w.at(532);
  EXPECT_EQ(w.m.value("#t", "opacity"), "0.5");
}

TEST(AnimWorldControl, AStyleChangeThatRemovesTheNameCancelsTheAnimationAndSettlesItsPromises) {
  Wd w(page(std::string(kKeys) + "#t { animation: a 1s linear } #t.off { animation: none }"));
  w.at(0);
  w.flush("#t");
  Ctl c(w);
  w.at(0);
  w.frame();
  w.at(300);
  w.set_class("#t", "off");
  w.flush("#t");
  EXPECT_EQ(c.notes(), "RR2 FJ1");
  EXPECT_EQ(c.st().play_state, GTEXT_ANIM_PLAY_IDLE);
  const std::string f = w.frame();
  EXPECT_NE(f.find("animationcancel:a:0.3@"), std::string::npos) << f << " (the elapsed time is the active time of the animation script had run, read before it was cancelled)";
  EXPECT_TRUE(has_event(f, "cancel")) << f;
}

TEST(AnimWorldControl, AStyleChangeOfThePlayStateOfAnAnimationScriptHasTouchedPausesItAndPlaysItAgain) {
  Wd w(page(std::string(kKeys) + "#t { animation: a 1s linear } #t.p { animation-play-state: paused }"));
  w.at(0);
  w.flush("#t");
  Ctl c(w);
  w.at(0);
  w.frame();
  w.at(300);
  w.set_class("#t", "p");
  w.flush("#t");
  EXPECT_EQ(c.st().play_state, GTEXT_ANIM_PLAY_PAUSED) << "a pause task is pending: paused whatever the start time";
  w.at(316);
  w.frame();
  w.at(800);
  EXPECT_EQ(w.m.value("#t", "opacity"), "0.3") << "held at the time of the style change event that paused it, as for a record that holds its own state";
  w.set_class("#t", "");
  w.flush("#t");
  w.at(816);
  w.frame();
  w.at(916);
  EXPECT_EQ(w.m.value("#t", "opacity"), "0.416") << "it runs again from the time it was held at";
}

TEST(AnimWorldControl, AScriptPauseIsNotUndoneByALaterChangeOfThePlayStateInTheStyle) {
  /* "After a successful call to play() or pause() on a CSSAnimation, any subsequent change to the animation-play-state will no longer cause the CSSAnimation to be played or paused" */
  Wd w(page(std::string(kKeys) + "#t { animation: a 1s linear } #t.p { animation-play-state: paused }"));
  w.at(0);
  w.flush("#t");
  Ctl c(w);
  w.at(0);
  w.frame();
  w.at(100);
  ASSERT_EQ(gtext_anim_animation_pause(c.a, c.now()), GTEXT_ANIM_OK);
  w.at(116);
  w.frame();
  w.set_class("#t", "p");
  w.flush("#t");
  w.set_class("#t", "");
  w.flush("#t");
  w.at(200);
  w.frame();
  EXPECT_EQ(c.st().play_state, GTEXT_ANIM_PLAY_PAUSED) << "the style said paused and then running again; the script's pause stands";
  w.at(900);
  EXPECT_EQ(w.m.value("#t", "opacity"), "0.116");
}

TEST(AnimWorldControl, ATaskScriptMakesIsReadyAtTheFrameEvenWhenAnEarlierStyleTaskWasFinishedByScript) {
  Wd w(page(std::string(kKeys) + "#t { animation: a 1s linear } #t.p { animation-play-state: paused }"));
  w.at(0);
  w.flush("#t");
  Ctl c(w);
  w.at(0);
  w.frame();
  w.at(300);
  w.set_class("#t", "p");
  w.flush("#t");   /* a pause task, ready at the time of this style change event (300) */
  ASSERT_EQ(gtext_anim_animation_finish(c.a, c.now()), GTEXT_ANIM_OK) << "script finishes it before the task ran";
  ASSERT_EQ(gtext_anim_animation_play(c.a, c.now()), GTEXT_ANIM_OK) << "and plays it again: a new task, script's";
  w.at(316);
  w.frame();
  w.at(416);
  EXPECT_EQ(w.m.value("#t", "opacity"), "0.1") << "ready at the frame (316), so 100 ms of it have gone at 416; ready at the style change event (300) would be 116";
}

TEST(AnimWorldControl, TheScheduledTimeOfAnEventFollowsThePlaybackRate) {
  /* "animation time to timeline time": time x (1 / playback rate) + start time */
  Wd w(page(std::string(kKeys) + "#t { animation: a 1s linear }"));
  w.at(0);
  w.flush("#t");
  Ctl c(w);
  w.at(0);
  w.frame();
  ASSERT_EQ(gtext_anim_animation_set_playback_rate(c.a, 2, c.now()), GTEXT_ANIM_OK);
  w.at(500);
  EXPECT_EQ(w.frame(), "animationend:a:1@500");
}

/* ---- an Animation holds the timing state of a CSS transition script has touched ------------------------------------------------------------------- */

/* A page with a transition of the width from 10px to 20px, over 100 ms (a delay when given), started by the class `on`. */
std::string tpage(const char * transition = "width 100ms linear", const char * extra = "") {
  return page(std::string("#t { width: 10px; transition: ") + transition + " } #t.on { width: 20px } #t.back { width: 10px } #t.off { transition-property: none } #t.auto { width: auto } " + extra);
}

/// Start the transition of `#t` at time 0 (the style change event) and make an Animation of it.
void start(Wd & w) {
  w.at(0);
  w.flush("#t");
  w.set_class("#t", "on");
  w.flush("#t");
}

struct TCtl : Ctl {
  explicit TCtl(Wd & wd, const char * selector = "#t") : Ctl(wd, selector, GTEXT_ANIM_KIND_CSS_TRANSITION) {}
};

TEST(AnimWorldTransitionControl, TheSameAnimationIsGivenForTheSameTransitionAndAnIdIsKeptApartByItsKind) {
  /* one style change event makes both, and each set of the element numbers its animations from the same place: the transition and the animation have the same id */
  Wd w(page("@keyframes a { from { opacity: 0 } to { opacity: 1 } } #t { width: 10px; transition: width 100ms linear } #t.on { width: 20px; animation: a 1s linear }"));
  start(w);
  GTEXT_Anim_Info t = w.first("#t");
  ASSERT_EQ(t.kind, GTEXT_ANIM_KIND_CSS_TRANSITION);
  GTEXT_Anim_Animation *a1 = nullptr, *a2 = nullptr, *an = nullptr;
  ASSERT_EQ(gtext_anim_world_control(w.world, GTEXT_ANIM_KIND_CSS_TRANSITION, t.id, &a1), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_world_control(w.world, GTEXT_ANIM_KIND_CSS_TRANSITION, t.id, &a2), GTEXT_ANIM_OK);
  EXPECT_EQ(a1, a2);
  EXPECT_EQ(gtext_anim_world_control_count(w.world), 1u);
  /* the CSS animation of the same element has an id of its own kind, whatever it is: it is another Animation */
  GTEXT_Anim_List * l = nullptr;
  ASSERT_EQ(gtext_anim_world_get_animations(w.world, w.m.handle("#t"), nullptr, 0, false, &l), GTEXT_ANIM_OK);
  GTEXT_Anim_Info ai{};
  ASSERT_EQ(gtext_anim_list_count(l), 2u);
  ASSERT_TRUE(gtext_anim_list_at(l, 1, &ai));
  gtext_anim_list_free(l);
  ASSERT_EQ(ai.kind, GTEXT_ANIM_KIND_CSS_ANIMATION);
  ASSERT_EQ(ai.id, t.id) << "the ids of the two kinds coincide, which is why the world keys an Animation by the kind as well";
  ASSERT_EQ(gtext_anim_world_control(w.world, GTEXT_ANIM_KIND_CSS_ANIMATION, ai.id, &an), GTEXT_ANIM_OK);
  EXPECT_NE(an, a1);
  EXPECT_EQ(gtext_anim_world_control_count(w.world), 2u);
  GTEXT_Anim_Kind kind = GTEXT_ANIM_KIND_CSS_ANIMATION;
  uint64_t id = 0;
  GTEXT_Anim_Animation * at = nullptr;
  ASSERT_TRUE(gtext_anim_world_control_at(w.world, 0, &kind, &id, &at));
  EXPECT_EQ(kind, GTEXT_ANIM_KIND_CSS_TRANSITION);
  EXPECT_EQ(id, t.id);
  EXPECT_EQ(at, a1);
  GTEXT_Anim_Animation * none = nullptr;
  EXPECT_EQ(gtext_anim_world_control(w.world, GTEXT_ANIM_KIND_CSS_TRANSITION, 12345, &none), GTEXT_ANIM_INVALID);
  EXPECT_EQ(gtext_anim_world_control(w.world, static_cast<GTEXT_Anim_Kind>(99), t.id, &none), GTEXT_ANIM_INVALID);
}

TEST(AnimWorldTransitionControl, TwoElementsThatEachHaveATransitionHaveTransitionsOfDifferentIds) {
  /* the ids a tracker gives are for one tracker, not for one element: an Animation is found by its id */
  Wd w(page("div { width: 10px; transition: width 100ms linear } div.on { width: 20px }", "<div id=a></div><div id=b></div><div id=c></div>"));
  w.at(0);
  for (const char * id : {"#a", "#b", "#c"}) {
    w.flush(id);
  }
  for (const char * id : {"#a", "#b"}) {
    w.set_class(id, "on");
    w.flush(id);
  }
  w.set_class("#a", "");
  w.flush("#a");
  w.set_class("#a", "on");
  w.flush("#a");
  w.set_class("#c", "on");
  w.flush("#c");
  GTEXT_Anim_List * l = nullptr;
  ASSERT_EQ(gtext_anim_world_get_animations(w.world, nullptr, nullptr, 0, false, &l), GTEXT_ANIM_OK);
  std::set<uint64_t> ids;
  for (size_t i = 0; i < gtext_anim_list_count(l); i++) {
    GTEXT_Anim_Info a{};
    ASSERT_TRUE(gtext_anim_list_at(l, i, &a));
    ids.insert(a.id);
  }
  EXPECT_EQ(ids.size(), gtext_anim_list_count(l));
  EXPECT_EQ(ids.size(), 3u);
  gtext_anim_list_free(l);
}

TEST(AnimWorldTransitionControl, ANewTransitionIsPendingUntilTheFrameAndThenStartsAtTheStyleChangeEvent) {
  Wd w(page("#t { width: 10px; transition: width 100ms linear } #t.on { width: 20px }"));
  start(w);
  TCtl c(w);
  auto s = c.st();
  EXPECT_TRUE(s.pending_play) << "a transition is pending until the first frame (Web Animations 1, \"Playing an animation\"), with its current time held at 0";
  EXPECT_EQ(s.play_state, GTEXT_ANIM_PLAY_RUNNING);
  EXPECT_EQ(s.current_time.ms, 0);
  EXPECT_FALSE(s.start_time.resolved);
  EXPECT_EQ(s.ready, GTEXT_ANIM_PROMISE_PENDING);
  EXPECT_TRUE(w.first("#t").pending);
  w.at(16);
  EXPECT_EQ(w.frame(), "transitionrun:width:0@0 transitionstart:width:0@0");
  s = c.st();
  EXPECT_FALSE(s.pending);
  EXPECT_EQ(s.start_time.ms, 0) << "ready at the time of the style change event, as for a record that holds its own state";
  EXPECT_EQ(c.notes(), "RR2");
  EXPECT_EQ(w.m.value("#t", "width"), "11.6px");
}

TEST(AnimWorldTransitionControl, PauseHoldsTheValueAndPlayResumesIt) {
  Wd w(page("#t { width: 10px; transition: width 100ms linear } #t.on { width: 20px }"));
  start(w);
  TCtl c(w);
  w.at(0);
  w.frame();
  w.at(50);
  EXPECT_EQ(w.m.value("#t", "width"), "15px");
  ASSERT_EQ(gtext_anim_animation_pause(c.a, c.now()), GTEXT_ANIM_OK);
  gtext_anim_world_touch(w.world);
  w.at(66);
  w.frame();
  EXPECT_EQ(c.st().play_state, GTEXT_ANIM_PLAY_PAUSED);
  EXPECT_EQ(w.first("#t").play_state, GTEXT_ANIM_PLAY_PAUSED);
  w.at(90);
  EXPECT_EQ(w.m.value("#t", "width"), "16.6px") << "held at the time the pause task ran";
  ASSERT_EQ(gtext_anim_animation_play(c.a, c.now()), GTEXT_ANIM_OK);
  gtext_anim_world_touch(w.world);
  w.at(106);
  w.frame();
  w.at(126);
  EXPECT_EQ(w.m.value("#t", "width"), "18.6px") << "it runs again from where it was held (66 ms of 100, and 20 ms since)";
}

TEST(AnimWorldTransitionControl, FinishEndsTheTransitionAtOnceWithItsEventsAndAFinishEventAndItCompletesAtTheNextStyleChangeEvent) {
  Wd w(page("#t { width: 10px; transition: width 100ms linear } #t.on { width: 20px }"));
  start(w);
  TCtl c(w);
  w.at(0);
  w.frame();
  c.notes();
  w.at(30);
  EXPECT_EQ(w.m.value("#t", "width"), "13px");
  ASSERT_EQ(gtext_anim_animation_finish(c.a, c.now()), GTEXT_ANIM_OK);
  gtext_anim_world_touch(w.world);
  EXPECT_EQ(c.notes(), "FR1");
  EXPECT_EQ(c.st().play_state, GTEXT_ANIM_PLAY_FINISHED);
  EXPECT_EQ(w.m.value("#t", "width"), "20px");
  w.at(46);
  const std::string f = w.frame();
  EXPECT_NE(f.find("transitionend:width:0.1@"), std::string::npos) << f;
  EXPECT_TRUE(has_event(f, "finish")) << f;
  EXPECT_EQ(w.list("#t"), "") << "finished with no forwards fill: not relevant";
  /* "Running transitions complete at a time that is equal to or after their end time, but prior to the first style change event whose time is equal to or after their end time": at the next event */
  w.set_class("#t", "on x");
  w.flush("#t");
  EXPECT_EQ(w.running("#t"), 0u) << "it completed: moved to the completed transitions";
  EXPECT_EQ(c.st().play_state, GTEXT_ANIM_PLAY_FINISHED) << "its Animation keeps its state: the effect is gone from the element";
  EXPECT_EQ(w.frame(), "");
}

TEST(AnimWorldTransitionControl, CancelByScriptLeavesTheTransitionInTheSetWithNoEffectAndNoStyleChangeBringsItBack) {
  Wd w(page("#t { width: 10px; transition: width 100ms linear } #t.on { width: 20px } #t.slow { transition-duration: 200ms }"));
  start(w);
  TCtl c(w);
  w.at(0);
  w.frame();
  w.at(30);
  w.frame();
  c.notes();
  ASSERT_EQ(gtext_anim_animation_cancel(c.a, c.now()), GTEXT_ANIM_OK);
  gtext_anim_world_touch(w.world);
  EXPECT_EQ(c.notes(), "FJ1");
  EXPECT_EQ(c.st().play_state, GTEXT_ANIM_PLAY_IDLE);
  EXPECT_EQ(w.m.value("#t", "width"), "20px") << "no effect: the property has the value the style says";
  EXPECT_EQ(w.list("#t"), "") << "an idle animation is not relevant";
  w.at(46);
  const std::string f = w.frame();
  EXPECT_NE(f.find("transitioncancel:width:0.03@"), std::string::npos) << f << " (the active time the last frame saw)";
  EXPECT_TRUE(has_event(f, "cancel")) << f;
  /* "Note that calling the cancel() method on the CSSTransition object representing a running transition does not cause the transition to be disassociated from its owning element": a change of the
     transition properties does not start another transition or make it live again */
  w.set_class("#t", "on slow");
  w.flush("#t");
  EXPECT_EQ(w.m.value("#t", "width"), "20px");
  EXPECT_EQ(c.st().play_state, GTEXT_ANIM_PLAY_IDLE);
  EXPECT_EQ(w.running("#t"), 1u) << "still in the set of running transitions";
}

TEST(AnimWorldTransitionControl, ACancelledTransitionCanBePlayedAgainAndAnimatesFromItsStartValueAtOnce) {
  /* CSSTransition-canceling: "After canceling a transition, it can still be re-used" */
  Wd w(tpage());
  start(w);
  TCtl c(w);
  w.at(0);
  w.frame();
  w.at(40);
  ASSERT_EQ(gtext_anim_animation_cancel(c.a, c.now()), GTEXT_ANIM_OK);
  gtext_anim_world_touch(w.world);
  EXPECT_EQ(w.m.value("#t", "width"), "20px");
  ASSERT_EQ(gtext_anim_animation_play(c.a, c.now()), GTEXT_ANIM_OK);
  gtext_anim_world_touch(w.world);
  EXPECT_EQ(w.m.value("#t", "width"), "10px") << "play() of an idle animation: current time 0 at once, a pending task";
  EXPECT_TRUE(c.st().pending_play);
  w.at(56);
  /* the table compares the phase of one frame with the phase of the one before: no frame saw the transition idle, so it was active and is active, and nothing is dispatched but the playback event */
  EXPECT_EQ(w.frame(), "cancel::0@40");
  EXPECT_EQ(c.st().play_state, GTEXT_ANIM_PLAY_RUNNING);
  EXPECT_FALSE(c.st().pending);
  w.at(106);
  EXPECT_EQ(w.m.value("#t", "width"), "15px") << "ready at the frame (56): 50 ms of it have gone";
}

TEST(AnimWorldTransitionControl, ATransitionAFrameSawIdleRunsAgainWhenItIsPlayedAgain) {
  /* "idle -> pending or before: transitionrun", and after it "pending or before -> active: transitionstart" */
  Wd w(tpage());
  start(w);
  TCtl c(w);
  w.at(0);
  w.frame();
  w.at(40);
  ASSERT_EQ(gtext_anim_animation_cancel(c.a, c.now()), GTEXT_ANIM_OK);
  gtext_anim_world_touch(w.world);
  EXPECT_EQ(w.frame(), "transitioncancel:width:0@40 cancel::0@40");
  ASSERT_EQ(gtext_anim_animation_play(c.a, c.now()), GTEXT_ANIM_OK);
  gtext_anim_world_touch(w.world);
  w.at(56);
  EXPECT_EQ(w.frame(), "transitionrun:width:0@56 transitionstart:width:0@56") << "ready at this frame, with no delay: idle to active";
}

TEST(AnimWorldTransitionControl, ACancelledFinishedTransitionCanBePlayedAgain) {
  Wd w(tpage());
  start(w);
  TCtl c(w);
  w.at(0);
  w.frame();
  w.at(20);
  ASSERT_EQ(gtext_anim_animation_finish(c.a, c.now()), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_cancel(c.a, c.now()), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_play(c.a, c.now()), GTEXT_ANIM_OK);
  gtext_anim_world_touch(w.world);
  EXPECT_EQ(w.m.value("#t", "width"), "10px");
  w.at(36);
  w.frame();
  EXPECT_EQ(c.st().play_state, GTEXT_ANIM_PLAY_RUNNING);
}

TEST(AnimWorldTransitionControl, ReplayingAFinishedTransitionRewindsPlayback) {
  /* CSSTransition-finished: "Restarting a finished transition rewinds playback" */
  Wd w(tpage());
  start(w);
  TCtl c(w);
  ASSERT_EQ(gtext_anim_animation_finish(c.a, c.now()), GTEXT_ANIM_OK);
  EXPECT_EQ(c.notes(), "RR2 FR1") << "finish() of an animation that is pending resolves its ready promise";
  ASSERT_EQ(gtext_anim_animation_play(c.a, c.now()), GTEXT_ANIM_OK);
  gtext_anim_world_touch(w.world);
  EXPECT_EQ(w.m.value("#t", "width"), "10px");
  EXPECT_EQ(c.st().play_state, GTEXT_ANIM_PLAY_RUNNING);
  /* the task play() made is script's, ready at the frame (16), and not the style change event's (0) that the first task, which finish() cancelled before any frame ran it, was */
  w.at(16);
  w.frame();
  w.at(66);
  EXPECT_EQ(w.m.value("#t", "width"), "15px");
}

TEST(AnimWorldTransitionControl, ATransitionTheStyleReversesIsCancelledAndItsPromisesSettle) {
  /* "Reversing a running transition cancels the original transition" */
  Wd w(tpage());
  start(w);
  TCtl c(w);
  w.at(0);
  w.frame();
  w.at(30);
  w.frame();
  c.notes();
  w.set_class("#t", "back");
  w.flush("#t");
  EXPECT_EQ(c.notes(), "FJ1");
  EXPECT_EQ(c.st().play_state, GTEXT_ANIM_PLAY_IDLE);
  const std::string f = w.frame();
  EXPECT_NE(f.find("transitioncancel:width:0.03@"), std::string::npos) << f;
  EXPECT_TRUE(has_event(f, "cancel")) << f;
  EXPECT_EQ(w.list("#t"), "T:width") << "the reversed transition is a transition of its own";
}

TEST(AnimWorldTransitionControl, ATransitionCancelledByTheStyleBeforeItIsReadyRejectsItsReadyPromise) {
  /* CSSTransition-ready: "ready promise is rejected when a transition is canceled by updating transition-property" */
  Wd w(tpage());
  start(w);
  TCtl c(w);
  EXPECT_TRUE(c.st().pending_play);
  w.set_class("#t", "on off");
  w.flush("#t");
  EXPECT_EQ(c.notes(), "RJ2 FJ1") << "the pending ready promise is rejected with an AbortError, and the finished promise too";
  EXPECT_EQ(c.st().play_state, GTEXT_ANIM_PLAY_IDLE);
  EXPECT_EQ(w.frame(), "cancel::0@0") << "no frame saw the transition: \"idle -> idle\" owes no transitioncancel, and the animation's own cancel event is owed";
}

TEST(AnimWorldTransitionControl, ATransitionWhoseTargetCannotBeInterpolatedAnyMoreIsCancelled) {
  Wd w(tpage());
  start(w);
  TCtl c(w);
  w.at(0);
  w.frame();
  w.at(10);
  c.notes();
  w.set_class("#t", "on auto");
  w.flush("#t");
  EXPECT_EQ(c.st().play_state, GTEXT_ANIM_PLAY_IDLE);
  EXPECT_EQ(w.list("#t"), "");
}

TEST(AnimWorldTransitionControl, CurrentTimeSeeksATransitionAndASeekPastTheEndFinishesIt) {
  /* CSSTransition-currentTime: "currentTime can be used to seek a CSS transition", "Skipping forwards through transition" */
  Wd w(tpage("width 100ms linear 100ms"));
  start(w);
  TCtl c(w);
  w.at(0);
  w.frame();
  EXPECT_EQ(w.first("#t").current_time.ms, 0);
  ASSERT_EQ(gtext_anim_animation_set_current_time(c.a, {true, 150}, c.now()), GTEXT_ANIM_OK);
  gtext_anim_world_touch(w.world);
  EXPECT_EQ(w.first("#t").current_time.ms, 150);
  EXPECT_EQ(w.m.value("#t", "width"), "15px");
  ASSERT_EQ(gtext_anim_animation_set_current_time(c.a, {true, 100}, c.now()), GTEXT_ANIM_OK);
  gtext_anim_world_touch(w.world);
  EXPECT_EQ(w.m.value("#t", "width"), "10px");
  ASSERT_EQ(gtext_anim_animation_set_current_time(c.a, {true, 200}, c.now()), GTEXT_ANIM_OK);
  gtext_anim_world_touch(w.world);
  EXPECT_EQ(c.st().play_state, GTEXT_ANIM_PLAY_FINISHED);
  const std::string f = w.frame();
  EXPECT_NE(f.find("transitionend:width:0.1@"), std::string::npos) << f;
  EXPECT_EQ(w.m.value("#t", "width"), "20px");
  /* a null current time is a TypeError and changes nothing */
  EXPECT_EQ(gtext_anim_animation_set_current_time(c.a, {false, 0}, c.now()), GTEXT_ANIM_TYPE_ERROR);
}

TEST(AnimWorldTransitionControl, TheTransitionAStyleReversesStartsFromTheCurrentPositionOfASeekedTransition) {
  /* CSSTransition-currentTime: "Transition reversing behavior respects currentTime and uses the transition's current position" */
  Wd w(tpage("width 100ms ease-in"));
  start(w);
  TCtl c(w);
  w.at(0);
  w.frame();
  ASSERT_EQ(gtext_anim_animation_set_current_time(c.a, {true, 50}, c.now()), GTEXT_ANIM_OK);
  gtext_anim_world_touch(w.world);
  const double portion = w.first("#t").computed.progress;
  EXPECT_GT(portion, 0.0);
  EXPECT_LT(portion, 0.5);
  w.set_class("#t", "back");
  w.flush("#t");
  ASSERT_EQ(w.list("#t"), "T:width") << "the reversed transition is produced: its start and end values are not both 10px";
  const GTEXT_Anim_Info r = w.first("#t");
  EXPECT_NEAR(r.computed.active_duration, 100 * portion, 1.0) << "the reversing shortening factor is the progress of the seeked transition";
  EXPECT_EQ(std::string(r.start_value.data, r.start_value.length).substr(0, 4), "13.1") << "from the value at the seeked position (ease-in at 0.5 is about 0.316): " << std::string(r.start_value.data, r.start_value.length);
}

TEST(AnimWorldTransitionControl, TheStartTimeSeeksATransitionAndMovesItsPhaseThroughTheEvents) {
  /* CSSTransition-startTime: "The start time can be set to seek a transition", "Seeking a transition using start time dispatches transition events" */
  Wd w(tpage("width 100ms linear 100ms"));
  start(w);
  TCtl c(w);
  w.at(0);
  EXPECT_EQ(w.frame(), "transitionrun:width:0@0");
  w.at(16);
  const double now = 16;
  ASSERT_EQ(gtext_anim_animation_set_start_time(c.a, {true, now - 100}, c.now()), GTEXT_ANIM_OK);
  gtext_anim_world_touch(w.world);
  EXPECT_EQ(w.m.value("#t", "width"), "10px");
  ASSERT_EQ(gtext_anim_animation_set_start_time(c.a, {true, now - 150}, c.now()), GTEXT_ANIM_OK);
  gtext_anim_world_touch(w.world);
  EXPECT_EQ(w.m.value("#t", "width"), "15px");
  const std::string f = w.frame();
  EXPECT_NE(f.find("transitionstart:width:0@"), std::string::npos) << f;
  ASSERT_EQ(gtext_anim_animation_set_start_time(c.a, {true, now - 200}, c.now()), GTEXT_ANIM_OK);
  gtext_anim_world_touch(w.world);
  const std::string g = w.frame();
  EXPECT_NE(g.find("transitionend:width:0.1@"), std::string::npos) << g;
}

TEST(AnimWorldTransitionControl, AReversedTransitionRunsBackwardsAndAFasterRateEndsItSooner) {
  Wd w(tpage());
  start(w);
  TCtl c(w);
  w.at(0);
  w.frame();
  w.at(60);
  ASSERT_EQ(gtext_anim_animation_reverse(c.a, c.now()), GTEXT_ANIM_OK);
  gtext_anim_world_touch(w.world);
  EXPECT_EQ(c.st().effective_rate, -1.0);
  w.at(76);
  w.frame();
  w.at(96);
  EXPECT_EQ(w.m.value("#t", "width"), "15.6px") << "the task is ready at the frame (76), when the transition had run 76 ms, and from there it goes back for 20 ms";
}

TEST(AnimWorldTransitionControl, ThePlaybackRateScalesTheTransitionAndTheScheduledTimeOfItsEnd) {
  Wd w(tpage());
  start(w);
  TCtl c(w);
  w.at(0);
  w.frame();
  ASSERT_EQ(gtext_anim_animation_set_playback_rate(c.a, 2, c.now()), GTEXT_ANIM_OK);
  gtext_anim_world_touch(w.world);
  w.at(25);
  EXPECT_EQ(w.m.value("#t", "width"), "15px");
  w.at(50);
  const std::string f = w.frame();
  EXPECT_NE(f.find("transitionend:width:0.1@50"), std::string::npos) << f << " (animation time to timeline time: time x (1 / playback rate) + start time)";
}

TEST(AnimWorldTransitionControl, ADisplayNoneElementCancelsTheAnimationOfItsTransition) {
  Wd w(page("#t { width: 10px; transition: width 100ms linear } #t.on { width: 20px } #t.none { display: none }"));
  start(w);
  TCtl c(w);
  w.at(0);
  w.frame();
  w.at(10);
  c.notes();
  w.set_class("#t", "on none");
  w.flush("#t");
  EXPECT_EQ(c.st().play_state, GTEXT_ANIM_PLAY_IDLE);
  EXPECT_EQ(c.notes(), "FJ1");
  const std::string f = w.frame();
  EXPECT_NE(f.find("transitioncancel:width:0.01@10"), std::string::npos) << f << " (the active time at the moment: 10 ms)";
  EXPECT_TRUE(has_event(f, "cancel")) << f;
}

TEST(AnimWorldTransitionControl, ATransitionHeldBeforeAReadyFrameIsReadyAtTheTimeOfTheStyleChangeEventAndOneScriptStartsAtTheFrame) {
  Wd w(tpage());
  start(w);
  gtext_cssom_transition_tracker_set_frame_pending(w.tracker, true);
  TCtl c(w);
  w.at(8);
  EXPECT_EQ(w.frame(), "transitionrun:width:0@0");
  EXPECT_TRUE(c.st().pending_play) << "a frame that is not ready does not run the task";
  gtext_cssom_transition_tracker_set_frame_pending(w.tracker, false);
  w.at(24);
  EXPECT_EQ(w.frame(), "transitionstart:width:0@0");
  EXPECT_FALSE(c.st().pending);
  EXPECT_EQ(c.st().start_time.ms, 0) << "ready at the time of the style change event";
}

TEST(AnimWorldTransitionControl, ATransitionAWorldLetsGoOfIsWhereItsAnimationWasAndKeepsRunning) {
  Wd w(tpage());
  start(w);
  {
    TCtl c(w);
    w.at(0);
    w.frame();
    w.at(20);
    ASSERT_EQ(gtext_anim_animation_pause(c.a, c.now()), GTEXT_ANIM_OK);
    w.at(36);
    w.frame();
    EXPECT_EQ(c.st().play_state, GTEXT_ANIM_PLAY_PAUSED);
    w.at(76);
  }
  gtext_anim_world_free(w.world);
  w.world = nullptr;
  EXPECT_EQ(w.m.value("#t", "width"), "13.6px") << "held at 36 ms of 100 when the world let go of it (76)";
  w.at(96);
  EXPECT_EQ(w.m.value("#t", "width"), "15.6px") << "a record that holds its own timing runs on at the rate of 1 from there";
  GTEXT_CSSOM_Transition_Events * ev = nullptr;
  ASSERT_EQ(gtext_cssom_transition_tracker_events(w.tracker, &ev), GTEXT_CSS_OK);
  gtext_cssom_transition_events_free(ev);
}


/* All or nothing: a style change event that cancels the transition an Animation holds either does it whole (the transition is replaced, the Animation is cancelled and its promises settle) or not at all (the set is as it was and
   the Animation runs on), whichever allocation fails: the tracker's (the working copy of the set, the room of the Animations to cancel) or the Animation's (the room for what a cancel makes). */
/// The id of the first running transition of `#t` (0 for none).
uint64_t running_id(Wd & w) {
  const GTEXT_CSS_Transition_Set * set = nullptr;
  double ms = 0;
  EXPECT_EQ(gtext_cssom_transition_tracker_lookup(w.tracker, w.m.handle("#t"), nullptr, &set, &ms), GTEXT_CSS_OK);
  GTEXT_CSS_Transition_Info info{};
  return gtext_css_transition_set_running_at(set, 0, &info) ? info.id : 0;
}

void style_cancel_sweep(bool fail_tracker, bool drained) {
  size_t failures = 0;
  /* The number of pause and play cycles the host leaves undrained before a `cancel()`: the room an Animation has for notes is a power of two, and some number of notes leaves it with less than a
     cancel needs. The Animation is idle then (the transition is still in the set, as the page of CSSTransition-canceling needs), and a style change event that replaces the transition asks it to cancel. */
  for (size_t variant = 0; variant < (drained ? 1u : 24u); variant++) {
    for (size_t n = 1; n < 400; n++) {
      FaultAllocator fa;
      Wd w(tpage(), fail_tracker ? nullptr : &fa.alloc, fail_tracker ? &fa.alloc : nullptr);
      start(w);
      TCtl c(w);
      double t = 0;
      w.at(t);
      w.frame();
      for (size_t i = 0; !drained && i < variant / 2; i++) {
        ASSERT_EQ(gtext_anim_animation_pause(c.a, c.now()), GTEXT_ANIM_OK);
        w.at(t += 1);
        w.frame();
        ASSERT_EQ(gtext_anim_animation_play(c.a, c.now()), GTEXT_ANIM_OK);
        w.at(t += 1);
        w.frame();
      }
      if (!drained && variant % 2) {
        ASSERT_EQ(gtext_anim_animation_pause(c.a, c.now()), GTEXT_ANIM_OK);   /* one note more or less */
        w.at(t += 1);
        w.frame();
      }
      ASSERT_EQ(gtext_anim_animation_cancel(c.a, c.now()), GTEXT_ANIM_OK);   /* the last call of the host adds notes and nothing makes room after it */
      if (drained) {
        c.notes();
      }
      const size_t before = gtext_anim_animation_note_count(c.a);
      const uint64_t was = running_id(w);
      ASSERT_NE(was, 0u);
      w.set_class("#t", "back");
      fa.arm(n);
      const GTEXT_CSS_Status st = gtext_cssom_transition_tracker_flush(w.tracker, w.m.handle("#t"), nullptr, 0);
      const bool failed = fa.failed;
      fa.disarm();
      if (st == GTEXT_CSS_OK) {
        EXPECT_NE(running_id(w), was) << n << ": the transition was replaced";
        if (!failed) {
          break;
        }
        continue;
      }
      failures++;
      EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
      EXPECT_EQ(running_id(w), was) << variant << "/" << n << ": a failed event leaves the set as it was";
      EXPECT_EQ(gtext_anim_animation_note_count(c.a), before) << n;
      EXPECT_EQ(c.st().play_state, GTEXT_ANIM_PLAY_IDLE) << n;
      ASSERT_EQ(gtext_cssom_transition_tracker_flush(w.tracker, w.m.handle("#t"), nullptr, 0), GTEXT_CSS_OK) << n << ": the same event again goes through";
      EXPECT_NE(running_id(w), was) << n;
    }
  }
  EXPECT_GT(failures, 0u);
}

TEST(AnimWorldTransitionOom, AStyleChangeEventThatCancelsAHeldTransitionIsWholeWhenTheTrackerCannotAllocate) {
  style_cancel_sweep(true, true);
}

TEST(AnimWorldTransitionOom, AStyleChangeEventThatCancelsAHeldTransitionIsWholeWhenTheAnimationCannotMakeRoom) {
  style_cancel_sweep(false, false);
}

TEST(AnimWorldTransitionOom, AFrameThatCannotBeMadeWhileATransitionIsHeldIsReportedWholeByTheNextCall) {
  size_t failures = 0;
  for (size_t n = 1; n < 400; n++) {
    FaultAllocator fa;
    Wd w(tpage(), &fa.alloc);
    start(w);
    TCtl c(w);
    w.at(16);
    fa.arm(n);
    GTEXT_Anim_Events * ev = nullptr;
    const GTEXT_Anim_Status st = gtext_anim_world_update(w.world, &ev);
    const bool failed = fa.failed;
    fa.disarm();
    if (st == GTEXT_ANIM_OK) {
      EXPECT_EQ(w.events_text(ev, false), "transitionrun:width:0@0 transitionstart:width:0@0") << n;
      if (!failed) {
        break;
      }
      continue;
    }
    failures++;
    EXPECT_EQ(st, GTEXT_ANIM_OOM) << n;
    EXPECT_EQ(w.frame(), "transitionrun:width:0@0 transitionstart:width:0@0") << n << ": the next frame reports it whole (the events of the transitions wait in the world)";
    EXPECT_FALSE(c.st().pending) << n;
  }
  EXPECT_GT(failures, 0u);
}

TEST(AnimWorldTransitionControl, ADisplayNoneCancelWithAHeldTransitionIsWholeOrNothing) {
  /* gtext_css_transition_set_cancel_all(): the element is no longer being rendered */
  size_t failures = 0;
  for (size_t n = 1; n < 400; n++) {
    FaultAllocator fa;
    Wd w(page("#t { width: 10px; transition: width 100ms linear } #t.on { width: 20px } #t.none { display: none }"), nullptr, &fa.alloc);
    start(w);
    TCtl c(w);
    w.at(0);
    w.frame();
    c.notes();
    w.set_class("#t", "on none");
    fa.arm(n);
    const GTEXT_CSS_Status st = gtext_cssom_transition_tracker_flush(w.tracker, w.m.handle("#t"), nullptr, 0);
    const bool failed = fa.failed;
    fa.disarm();
    if (st == GTEXT_CSS_OK) {
      EXPECT_EQ(c.st().play_state, GTEXT_ANIM_PLAY_IDLE) << n;
      if (!failed) {
        break;
      }
      continue;
    }
    failures++;
    EXPECT_EQ(c.st().play_state, GTEXT_ANIM_PLAY_RUNNING) << n;
    ASSERT_EQ(gtext_cssom_transition_tracker_flush(w.tracker, w.m.handle("#t"), nullptr, 0), GTEXT_CSS_OK) << n;
    EXPECT_EQ(c.st().play_state, GTEXT_ANIM_PLAY_IDLE) << n;
  }
  EXPECT_GT(failures, 0u);
}

/* All or nothing: a frame whose list cannot be made reports its events at the next call, whole. */
TEST(AnimWorldOom, AFrameThatCannotBeMadeIsMadeWholeByTheNextCall) {
  FaultAllocator fa;
  Wd w(page(std::string(kKeys) + "#t { width: 10px; animation: a 1s linear; transition: width 100ms linear } #t.on { width: 20px }"), &fa.alloc);
  w.at(0);
  w.flush("#t");
  w.set_class("#t", "on");
  w.flush("#t");
  w.at(5000);
  size_t failures = 0;
  for (size_t n = 1; n < 1000; n++) {
    fa.arm(n);
    GTEXT_Anim_Events * ev = nullptr;
    const GTEXT_Anim_Status st = gtext_anim_world_update(w.world, &ev);
    const bool failed = fa.failed;
    fa.disarm();
    if (st == GTEXT_ANIM_OK) {
      /* a failure of the merge sort's scratch is not a failure of the frame: the sort falls back to insertion, in the same order */
      EXPECT_EQ(w.events_text(ev, false), "transitionrun:width:0@0 transitionstart:width:0@0 animationstart:a:0@0 transitionend:width:0.1@100 animationend:a:1@1000") << n << (failed ? " (the sort's scratch failed)" : "");
      break;
    }
    failures++;
    EXPECT_EQ(st, GTEXT_ANIM_OOM) << n;
    EXPECT_EQ(ev, nullptr);
  }
  EXPECT_GT(failures, 0u);
  EXPECT_EQ(w.frame(), "");
}

}  // namespace
