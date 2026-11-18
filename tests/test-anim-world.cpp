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
#include <string>

#include <gtest/gtest.h>

#include "cssom_computed_test_support.h"

#include <ghoti.io/text/animation/world.h>

using namespace cc;

namespace {

/// A page, a tracker over its document, the layout hooks that give the tracker's sets and the CSS animations' time, and a world over both.
struct Wd {
  GTEXT_CSSOM_Layout_Interface iface{};
  GTEXT_CSSOM_Transition_Tracker * tracker = nullptr;
  GTEXT_Anim_World * world = nullptr;
  Cs m;
  explicit Wd(const std::string & html, const GTEXT_Allocator * world_alloc = nullptr)
      : m(html, [this](GTEXT_CSSOM_Config & c) {
          iface.size = sizeof iface;
          iface.user = this;
          iface.transitions = &Wd::sets;
          iface.transition_version = &Wd::ver;
          iface.animation_time = &Wd::time;
          iface.timeline_version = &Wd::ver;
          c.layout = &iface;
        }) {
    EXPECT_EQ(gtext_cssom_transition_tracker_new(m.doc->d, nullptr, &tracker), GTEXT_CSS_OK);
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
    static const char * const names[] = {"animationstart", "animationiteration", "animationend", "animationcancel", "transitionrun", "transitionstart", "transitionend", "transitioncancel"};
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
