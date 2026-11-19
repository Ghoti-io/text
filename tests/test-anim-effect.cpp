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
 * Keyframe effects (animation/effect.h) and the animations script makes (animation/world.h), against Web Animations 1 ("Processing a keyframes argument", "Calculating computed keyframes", "The effect
 * value of a keyframe effect", "Effect composition", "The effect stack", "Removing replaced animations", "The KeyframeEffect interface", `Element.animate()`) and Web Animations 2 ("Effect
 * accumulation").
 */

#include <cmath>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cssom_computed_test_support.h"

#include <ghoti.io/text/animation/animation.h>
#include <ghoti.io/text/animation/effect.h>
#include <ghoti.io/text/animation/world.h>

using namespace cc;

namespace {

struct Arg;

/// A page, a tracker over its document, the layout hooks, and a world over both (as test-anim-world.cpp has them).
struct Ew {
  GTEXT_CSSOM_Layout_Interface iface{};
  GTEXT_CSSOM_Transition_Tracker * tracker = nullptr;
  GTEXT_Anim_World * world = nullptr;
  Cs m;
  explicit Ew(const std::string & html, const GTEXT_Allocator * world_alloc = nullptr)
      : m(html, [this](GTEXT_CSSOM_Config & c) {
          iface.size = sizeof iface;
          iface.user = this;
          iface.transitions = &Ew::sets;
          iface.transition_version = &Ew::ver;
          iface.animation_time = &Ew::time;
          iface.timeline_version = &Ew::ver;
          c.layout = &iface;
        }) {
    EXPECT_EQ(gtext_cssom_transition_tracker_new(m.doc->d, nullptr, &tracker), GTEXT_CSS_OK);
    EXPECT_EQ(gtext_anim_world_new(m.doc->d, tracker, world_alloc, &world), GTEXT_ANIM_OK);
  }
  Ew(const Ew &) = delete;
  Ew & operator=(const Ew &) = delete;
  ~Ew() {
    gtext_anim_world_free(world);
    gtext_cssom_transition_tracker_free(tracker);
  }
  static GTEXT_CSS_Status sets(void * user, const void * element, const GTEXT_CSS_Selector_List * pseudo, const GTEXT_CSS_Transition_Set ** set, double * ms) {
    Ew * self = static_cast<Ew *>(user);
    return gtext_cssom_transition_tracker_lookup(self->tracker, element, pseudo, set, ms);
  }
  static GTEXT_CSS_Status time(void * user, const void * element, const GTEXT_CSS_Selector_List * pseudo, const GTEXT_CSS_Anim_Effect * effect, double * ms, bool * known) {
    Ew * self = static_cast<Ew *>(user);
    return gtext_cssom_transition_tracker_animation_time(self->tracker, element, pseudo, effect, ms, known);
  }
  static uint64_t ver(void * user) {
    return gtext_cssom_transition_tracker_version(static_cast<Ew *>(user)->tracker);
  }
  void at(double ms) {
    gtext_cssom_transition_tracker_set_time(tracker, ms);
  }
  /// One frame: the pending tasks of the animations script made run, at the tracker's time.
  void frame() {
    GTEXT_Anim_Events * ev = nullptr;
    ASSERT_EQ(gtext_anim_world_update(world, &ev), GTEXT_ANIM_OK);
    gtext_anim_events_free(ev);
  }
  void flush(const char * selector, const char * pseudo = nullptr) {
    ASSERT_EQ(gtext_cssom_transition_tracker_flush(tracker, m.handle(selector), pseudo, pseudo ? std::strlen(pseudo) : 0), GTEXT_CSS_OK);
  }
  /// `getAnimations()` as text: `S` a script animation, `A` a CSS animation, `T` a transition, with its id for scripts.
  std::string list(const char * selector, bool subtree = false, const char * pseudo = nullptr) {
    GTEXT_Anim_List * l = nullptr;
    EXPECT_EQ(gtext_anim_world_get_animations(world, selector ? m.handle(selector) : nullptr, pseudo, pseudo ? std::strlen(pseudo) : 0, subtree, &l), GTEXT_ANIM_OK);
    std::string out;
    for (size_t i = 0; i < gtext_anim_list_count(l); i++) {
      GTEXT_Anim_Info a{};
      EXPECT_TRUE(gtext_anim_list_at(l, i, &a));
      out += std::string(i ? " " : "") + (a.kind == GTEXT_ANIM_KIND_CSS_TRANSITION ? "T:" : a.kind == GTEXT_ANIM_KIND_CSS_ANIMATION ? "A:" : "S:");
      out += a.kind == GTEXT_ANIM_KIND_SCRIPT ? std::to_string(a.id - ((uint64_t)1 << 40)) : std::string(a.name.data, a.name.length);
      if (a.pseudo_element.length) {
        out += "(" + std::string(a.pseudo_element.data, a.pseudo_element.length) + ")";
      }
    }
    gtext_anim_list_free(l);
    return out;
  }
  /// A script animation on `selector` with the keyframes and options given.
  GTEXT_Anim_Animation * animate(const char * selector, Arg & a, double ms, uint64_t * id = nullptr, const GTEXT_Anim_Effect_Options * options = nullptr);
  const void * h(const char * selector) const {
    return m.handle(selector);
  }
  std::string v(const char * selector, const char * property) {
    return m.value(selector, property);
  }
};

/// The text the `keyframes` argument is built from, kept alive with the structs that point at it.
struct Arg {
  std::deque<std::string> text;
  std::vector<GTEXT_Anim_Keyframe_Like> frames;
  std::vector<std::vector<GTEXT_Anim_Keyframe_Property_Input>> props;
  GTEXT_Anim_Keyframes_Argument arg{};
  GTEXT_Anim_Property_Indexed idx{};
  std::vector<GTEXT_Anim_Property_List_Input> lists;
  std::vector<std::vector<GTEXT_Anim_Text>> values;
  std::vector<GTEXT_Anim_Offset> offsets;
  std::vector<GTEXT_Anim_Text> easings;
  std::vector<GTEXT_Anim_Keyframe_Composite> composites;

  GTEXT_Anim_Text t(const std::string & s) {
    text.push_back(s);
    return GTEXT_Anim_Text{text.back().data(), text.back().size()};
  }
  /// The array form: one keyframe-like object, as `{ name: value, ... }`, with an offset (NAN for null), an easing ("" for none) and a composite.
  Arg & frame(std::vector<std::pair<std::string, std::string>> pairs, double offset = NAN, const std::string & easing = "", GTEXT_Anim_Keyframe_Composite composite = GTEXT_ANIM_KF_COMPOSITE_AUTO) {
    GTEXT_Anim_Keyframe_Like f{};
    f.offset.is_null = offset != offset;
    f.offset.value = f.offset.is_null ? 0 : offset;
    if (!easing.empty()) {
      f.easing = t(easing);
    }
    f.composite = composite;
    props.emplace_back();
    for (auto & p : pairs) {
      props.back().push_back({t(p.first), t(p.second)});
    }
    frames.push_back(f);
    arg.kind = GTEXT_ANIM_KEYFRAMES_ARRAY;
    return *this;
  }
  /// The object form: `{ name: [values] }`.
  Arg & list(const std::string & name, std::vector<std::string> v) {
    values.emplace_back();
    for (auto & s : v) {
      values.back().push_back(t(s));
    }
    lists.push_back({t(name), nullptr, v.size()});
    arg.kind = GTEXT_ANIM_KEYFRAMES_INDEXED;
    return *this;
  }
  Arg & offset(std::vector<double> o) {
    for (double d : o) {
      offsets.push_back({d != d, d != d ? 0 : d});
    }
    arg.kind = GTEXT_ANIM_KEYFRAMES_INDEXED;
    return *this;
  }
  Arg & easing(std::vector<std::string> e) {
    for (auto & s : e) {
      easings.push_back(t(s));
    }
    arg.kind = GTEXT_ANIM_KEYFRAMES_INDEXED;
    return *this;
  }
  Arg & composite(std::vector<GTEXT_Anim_Keyframe_Composite> c) {
    composites = c;
    arg.kind = GTEXT_ANIM_KEYFRAMES_INDEXED;
    return *this;
  }
  const GTEXT_Anim_Keyframes_Argument * get() {
    if (arg.kind == GTEXT_ANIM_KEYFRAMES_ARRAY) {
      for (size_t i = 0; i < frames.size(); i++) {
        frames[i].properties = props[i].data();
        frames[i].property_count = props[i].size();
      }
      arg.array = frames.data();
      arg.array_count = frames.size();
    }
    else if (arg.kind == GTEXT_ANIM_KEYFRAMES_INDEXED) {
      for (size_t i = 0; i < lists.size(); i++) {
        lists[i].values = values[i].data();
      }
      idx.offsets = offsets.data();
      idx.offset_count = offsets.size();
      idx.easings = easings.data();
      idx.easing_count = easings.size();
      idx.composites = composites.data();
      idx.composite_count = composites.size();
      idx.properties = lists.data();
      idx.property_count = lists.size();
      arg.indexed = &idx;
    }
    return &arg;
  }
};

GTEXT_Anim_Effect_Options duration(double ms);

GTEXT_Anim_Animation * Ew::animate(const char * selector, Arg & a, double ms, uint64_t * id, const GTEXT_Anim_Effect_Options * options) {
  GTEXT_Anim_Effect_Options o = options ? *options : duration(ms);
  GTEXT_Anim_Animation * an = nullptr;
  uint64_t local = 0;
  EXPECT_EQ(gtext_anim_world_animate(world, h(selector), a.get(), &o, nullptr, id ? id : &local, &an), GTEXT_ANIM_OK);
  return an;
}

std::string frames_text(GTEXT_Anim_Keyframes * k) {
  std::string out;
  for (size_t i = 0; i < gtext_anim_keyframes_count(k); i++) {
    GTEXT_Anim_Keyframe_Info f{};
    EXPECT_TRUE(gtext_anim_keyframes_at(k, i, &f));
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s%s/%g %.*s", i ? " | " : "", f.offset_null ? "null" : std::to_string(f.offset).c_str(), f.computed_offset, (int)f.easing.length, f.easing.data);
    out += buf;
    if (f.composite != GTEXT_ANIM_KF_COMPOSITE_AUTO) {
      out += std::string(" c") + std::to_string((int)f.composite);
    }
    for (size_t j = 0; j < f.property_count; j++) {
      GTEXT_CSS_String n, idl, val;
      EXPECT_TRUE(gtext_anim_keyframes_property_at(k, i, j, &n, &idl, &val));
      out += " " + std::string(idl.data, idl.length) + "=" + std::string(val.data, val.length);
    }
  }
  return out;
}

std::string processed(Ew & w, Arg & a, GTEXT_Anim_Status * status = nullptr) {
  GTEXT_Anim_Keyframes * k = nullptr;
  const GTEXT_Anim_Status st = gtext_anim_keyframes_process(w.world, a.get(), &k);
  if (status) {
    *status = st;
  }
  else {
    EXPECT_EQ(st, GTEXT_ANIM_OK);
  }
  const std::string out = k ? frames_text(k) : "";
  gtext_anim_keyframes_free(k);
  return out;
}

GTEXT_Anim_Effect_Options duration(double ms) {
  GTEXT_Anim_Effect_Options o{};
  o.timing.present = GTEXT_ANIM_TIMING_DURATION;
  o.timing.values = gtext_anim_timing_default();
  o.timing.values.duration = ms;
  o.timing.values.duration_auto = false;
  return o;
}

const char * PAGE = "<style>#a { opacity: 0.25 } #b { opacity: 1 }</style><div id=a></div><div id=b></div>";

}  // namespace

/* "The first array-form is the canonical form, and is the form returned by getKeyframes()": the offsets nobody gave are computed (the first 0, the last 1, the others evenly), the easing defaults to linear. */
TEST(AnimEffectKeyframes, TheArrayFormKeepsItsOffsetsAndComputesTheMissingOnes) {
  Ew w(PAGE);
  Arg a;
  a.frame({{"opacity", "0"}}).frame({{"opacity", "0.5"}}).frame({{"opacity", "1"}}, 0.9);
  EXPECT_EQ(processed(w, a), "null/0 linear opacity=0 | null/0.45 linear opacity=0.5 | 0.900000/0.9 linear opacity=1");
}

/* "The following three expressions are all equivalent": one keyframe of one value has offset 1 (the last keyframe, with nothing before it). */
TEST(AnimEffectKeyframes, OneKeyframeIsAtOne) {
  Ew w(PAGE);
  Arg a;
  a.frame({{"opacity", "0.5"}});
  EXPECT_EQ(processed(w, a), "null/1 linear opacity=0.5");
  Arg b;
  b.list("opacity", {"0.5"});
  EXPECT_EQ(processed(w, b), "null/1 linear opacity=0.5");
}

/* The object form: each value is a keyframe, "the list of values will be repeated as needed" for the easing and composite, the offsets are assigned in order until either list ends. */
TEST(AnimEffectKeyframes, TheObjectFormMakesOneKeyframePerValueAndSpreadsTheOtherMembers) {
  Ew w(PAGE);
  Arg a;
  a.list("opacity", {"0", "0.5", "1", "0.2", "0.9"}).offset({NAN, 0.5}).easing({"ease-in", "ease-out"}).composite({GTEXT_ANIM_KF_COMPOSITE_ADD});
  EXPECT_EQ(processed(w, a),
      "null/0 ease-in c2 opacity=0 | 0.500000/0.5 ease-out c2 opacity=0.5 | null/0.666667 ease-in c2 opacity=1 | null/0.833333 ease-out c2 opacity=0.2 | null/1 ease-in c2 opacity=0.9");
}

/* "Merge adjacent keyframes ... when they have equal computed keyframe offsets": a property with two values and one with three: the keyframes at 0 and at 1 carry both, the middle one only the longer's. */
TEST(AnimEffectKeyframes, TheValuesOfTwoPropertiesAtOneOffsetAreOneKeyframe) {
  Ew w(PAGE);
  Arg a;
  a.list("opacity", {"0", "0.5", "1"}).list("left", {"0px", "100px"});
  EXPECT_EQ(processed(w, a), "null/0 linear left=0px opacity=0 | null/0.5 linear opacity=0.5 | null/1 linear left=100px opacity=1")
      << "the properties of a keyframe are sorted by the code points of the names they were given under";
}

/* "Sort animation properties in ascending order by the Unicode codepoints", and the properties that are not animatable are not properties of the keyframe: `animationName` is not animatable; `foo` is no property. */
TEST(AnimEffectKeyframes, OnlyAnimatablePropertiesAreKept) {
  Ew w(PAGE);
  Arg a;
  a.frame({{"zIndex", "3"}, {"animationName", "x"}, {"foo", "1"}, {"marginLeft", "4px"}, {"--custom", "v"}, {"cssFloat", "left"}});
  EXPECT_EQ(processed(w, a), "null/1 linear --custom=v cssFloat=left marginLeft=4px zIndex=3");
}

/* "If the property value is invalid according to the syntax for the property, discard the property-value pair", and a value is serialized as CSSOM serializes it. */
TEST(AnimEffectKeyframes, AnInvalidValueIsDiscardedAndAValueIsSerialized) {
  Ew w(PAGE);
  Arg a;
  a.frame({{"opacity", "not a number"}, {"left", "10PX"}, {"color", "rgb(255,0,0)"}, {"top", ""}}, 0);
  EXPECT_EQ(processed(w, a), "0.000000/0 linear color=rgb(255, 0, 0) left=10px");
}

/* "If processed keyframes is not loosely sorted by offset, throw a TypeError", an offset out of 0..1, an easing that does not parse: after the other members were read. */
TEST(AnimEffectKeyframes, TheTypeErrorsOfTheArgument) {
  Ew w(PAGE);
  GTEXT_Anim_Status st;
  Arg unsorted;
  unsorted.frame({{"opacity", "0"}}, 0.6).frame({{"opacity", "1"}}, 0.4);
  processed(w, unsorted, &st);
  EXPECT_EQ(st, GTEXT_ANIM_TYPE_ERROR);
  Arg out_of_range;
  out_of_range.frame({{"opacity", "0"}}, -0.1);
  processed(w, out_of_range, &st);
  EXPECT_EQ(st, GTEXT_ANIM_TYPE_ERROR);
  Arg over;
  over.frame({{"opacity", "0"}}, 1.5);
  processed(w, over, &st);
  EXPECT_EQ(st, GTEXT_ANIM_TYPE_ERROR);
  Arg infinite;
  infinite.frame({{"opacity", "0"}}, INFINITY);
  processed(w, infinite, &st);
  EXPECT_EQ(st, GTEXT_ANIM_TYPE_ERROR);
  Arg easing;
  easing.frame({{"opacity", "0"}}, NAN, "ease-sideways");
  processed(w, easing, &st);
  EXPECT_EQ(st, GTEXT_ANIM_TYPE_ERROR);
  Arg unused;
  unused.list("opacity", {"0", "1"}).easing({"linear", "ease", "not-an-easing"});
  processed(w, unused, &st);
  EXPECT_EQ(st, GTEXT_ANIM_TYPE_ERROR) << "an unused easing is parsed too";
  Arg equal;
  equal.frame({{"opacity", "0"}}, 0.5).frame({{"opacity", "1"}}, 0.5);
  EXPECT_EQ(processed(w, equal), "0.500000/0.5 linear opacity=0 | 0.500000/0.5 linear opacity=1") << "equal offsets are permitted";
}

/* "null: an empty sequence of keyframes", and a null element of the sequence is an empty keyframe. */
TEST(AnimEffectKeyframes, NullAndNullElements) {
  Ew w(PAGE);
  Arg none;
  EXPECT_EQ(processed(w, none), "");
  Arg a;
  a.frame({{"opacity", "0"}});
  a.frames.push_back(GTEXT_Anim_Keyframe_Like{});
  a.frames.back().null_entry = true;
  a.props.emplace_back();
  EXPECT_EQ(processed(w, a), "null/0 linear opacity=0 | null/1 linear");
}

/* animate(): "Construct a new KeyframeEffect ... a new Animation ... play an animation": the effect value is the interpolation at the animation's current time, the underlying value outside the active phase. */
TEST(AnimEffectAnimate, AnimateInterpolatesAtTheCurrentTime) {
  Ew w(PAGE);
  w.at(0);
  Arg a;
  a.list("opacity", {"0", "1"});
  GTEXT_Anim_Effect_Options o = duration(1000);
  GTEXT_Anim_Animation * an = nullptr;
  uint64_t id = 0;
  ASSERT_EQ(gtext_anim_world_animate(w.world, w.h("#a"), a.get(), &o, nullptr, &id, &an), GTEXT_ANIM_OK);
  EXPECT_EQ(w.v("#a", "opacity"), "0") << "pending, its current time is 0";
  w.frame();
  w.at(250);
  EXPECT_EQ(w.v("#a", "opacity"), "0.25");
  w.at(750);
  EXPECT_EQ(w.v("#a", "opacity"), "0.75");
  w.at(1000);
  w.frame();
  EXPECT_EQ(w.v("#a", "opacity"), "0.25") << "after the end, with no fill: the underlying value";
  EXPECT_EQ(w.v("#b", "opacity"), "1");
}

/* "The animation effects of an animation whose replace state is removed are not included in the effect stacks", and `fill: forwards` keeps the effect after the end. */
TEST(AnimEffectAnimate, FillForwardsKeepsTheLastValue) {
  Ew w(PAGE);
  w.at(0);
  Arg a;
  a.list("opacity", {"0", "0.5"});
  GTEXT_Anim_Effect_Options o = duration(1000);
  o.timing.present |= GTEXT_ANIM_TIMING_FILL;
  o.timing.values.fill = GTEXT_ANIM_FILL_FORWARDS;
  GTEXT_Anim_Animation * an = nullptr;
  uint64_t id = 0;
  ASSERT_EQ(gtext_anim_world_animate(w.world, w.h("#a"), a.get(), &o, nullptr, &id, &an), GTEXT_ANIM_OK);
  w.frame();
  w.at(5000);
  EXPECT_EQ(w.v("#a", "opacity"), "0.5");
}

/* "Effect composition": replace gives the effect value; add adds it to the underlying value, so the second of two animations on one property is over the first (the effect stack, in composite order). */
TEST(AnimEffectStack, TheLaterAnimationIsOverTheEarlierAndAddAddsToTheUnderlyingValue) {
  Ew w(PAGE);
  w.at(0);
  Arg a;
  a.list("opacity", {"0.5", "0.5"});
  Arg b;
  b.list("opacity", {"0.25", "0.25"});
  GTEXT_Anim_Effect_Options o = duration(1000);
  o.has_composite = true;
  o.composite = GTEXT_ANIM_COMPOSITE_ADD;
  w.animate("#a", a, 1000);
  w.frame();
  w.at(10);
  EXPECT_EQ(w.v("#a", "opacity"), "0.5");
  w.animate("#a", b, 1000, nullptr, &o);
  w.frame();
  w.at(20);
  EXPECT_EQ(w.v("#a", "opacity"), "0.75") << "0.25 added to what the first animation gives (0.5)";
  EXPECT_EQ(w.list("#a"), "S:0 S:1") << "in the order the animations were made";
}

/* "accumulate": the same for numbers; and a keyframe's own composite operation is over the effect's. */
TEST(AnimEffectStack, AKeyframesCompositeOperationIsOverTheEffects) {
  Ew w(PAGE);
  w.at(0);
  Arg a;
  a.frame({{"opacity", "0.5"}}, 0, "", GTEXT_ANIM_KF_COMPOSITE_ADD).frame({{"opacity", "0.5"}}, 1, "", GTEXT_ANIM_KF_COMPOSITE_REPLACE);
  w.animate("#a", a, 1000);
  w.frame();
  w.at(0);
  EXPECT_EQ(w.v("#a", "opacity"), "0.75") << "the first keyframe adds 0.5 to the underlying 0.25";
  w.at(500);
  EXPECT_EQ(w.v("#a", "opacity"), "0.625") << "halfway between 0.75 and 0.5";
  w.at(900);
  EXPECT_EQ(w.v("#a", "opacity"), "0.525");
}

/* Web Animations 2, "Effect accumulation": with an iteration composite operation of accumulate each iteration is accumulated onto the final value of the one before. */
TEST(AnimEffectStack, IterationCompositeAccumulateAddsTheFinalValueOfEachPreviousIteration) {
  Ew w("<div id=a style='opacity:0'></div><div id=b style='margin-left: 0px; z-index: 0'></div>");
  w.at(0);
  Arg a;
  a.list("zIndex", {"0", "10"});
  GTEXT_Anim_Effect_Options o = duration(1000);
  o.timing.present |= GTEXT_ANIM_TIMING_ITERATIONS;
  o.timing.values.iterations = 4;
  o.has_iteration_composite = true;
  o.iteration_composite = GTEXT_ANIM_ITERATION_ACCUMULATE;
  w.animate("#b", a, 1000, nullptr, &o);
  w.frame();
  w.at(500);
  EXPECT_EQ(w.v("#b", "z-index"), "5");
  w.at(1500);
  EXPECT_EQ(w.v("#b", "z-index"), "15") << "10 from the first iteration, 5 of the second";
  w.at(3500);
  EXPECT_EQ(w.v("#b", "z-index"), "35");
}

/* "The effect value ... If iteration progress is less than zero and there is more than one keyframe with a computed offset of 0, add the first keyframe": a negative easing overshoot picks the first of the keyframes at 0. */
TEST(AnimEffectStack, OverlappingKeyframesTakeTheLastAtThePointOfOverlap) {
  Ew w(PAGE);
  w.at(0);
  Arg a;
  a.frame({{"opacity", "0"}}, 0).frame({{"opacity", "1"}}, 0.5).frame({{"opacity", "0.25"}}, 0.5).frame({{"opacity", "0.75"}}, 1);
  w.animate("#a", a, 1000);
  w.frame();
  w.at(499);
  EXPECT_EQ(w.v("#a", "opacity").substr(0, 4), "0.99");
  w.at(500);
  EXPECT_EQ(w.v("#a", "opacity"), "0.25") << "at the point of overlap the value jumps to the last keyframe";
  w.at(750);
  EXPECT_EQ(w.v("#a", "opacity"), "0.5");
}

/* "If iteration progress < 0 and there is more than one keyframe ... with a computed keyframe offset of 0, add the first keyframe"; "If iteration progress >= 1 and there is more than one with an offset of 1, add the last
   keyframe": an easing that overshoots (`linear()` here, from -0.5 to 1.5) takes the progress out of 0..1, and the value is that of the first or the last of the overlapping keyframes, not an extrapolation. */
TEST(AnimEffectStack, AnOvershootingProgressTakesTheFirstOrTheLastOfTheKeyframesAtTheEnd) {
  Ew w(PAGE);
  w.at(0);
  Arg a;
  a.frame({{"opacity", "0.2"}}, 0).frame({{"opacity", "0.4"}}, 0).frame({{"opacity", "0.6"}}, 1).frame({{"opacity", "0.8"}}, 1);
  GTEXT_Anim_Effect_Options o = duration(1000);
  o.timing.present |= GTEXT_ANIM_TIMING_EASING | GTEXT_ANIM_TIMING_FILL;
  o.timing.values.fill = GTEXT_ANIM_FILL_BOTH;
  o.easing = a.t("linear(-0.5, 1.5)");
  w.animate("#a", a, 1000, nullptr, &o);
  w.frame();
  w.at(0);
  EXPECT_EQ(w.v("#a", "opacity"), "0.2") << "progress -0.5: the first of the keyframes at 0";
  w.at(1000);
  w.frame();
  GTEXT_Anim_List * dl = nullptr;
  gtext_anim_world_get_animations(w.world, nullptr, nullptr, 0, false, &dl);
  GTEXT_Anim_Info di{};
  gtext_anim_list_at(dl, 0, &di);
  EXPECT_EQ(w.v("#a", "opacity"), "0.8") << "progress 1.5: the last of the keyframes at 1";
}

/* "Keyframe-specific easing function ... applied to the period between the keyframe and the next", and the effect's easing is over the whole iteration. */
TEST(AnimEffectStack, TheKeyframeEasingAppliesBetweenKeyframesAndTheEffectEasingOverTheIteration) {
  Ew w(PAGE);
  w.at(0);
  Arg a;
  a.frame({{"opacity", "0"}}, 0, "steps(2, end)").frame({{"opacity", "1"}}, 1);
  w.animate("#a", a, 1000);
  w.frame();
  w.at(400);
  EXPECT_EQ(w.v("#a", "opacity"), "0");
  w.at(600);
  EXPECT_EQ(w.v("#a", "opacity"), "0.5");
  Ew x(PAGE);
  x.at(0);
  Arg b;
  b.list("opacity", {"0", "1"});
  GTEXT_Anim_Effect_Options o = duration(1000);
  o.timing.present |= GTEXT_ANIM_TIMING_EASING;
  o.easing = b.t("steps(2, end)");
  x.animate("#a", b, 1000, nullptr, &o);
  x.frame();
  x.at(400);
  EXPECT_EQ(x.v("#a", "opacity"), "0");
  x.at(600);
  EXPECT_EQ(x.v("#a", "opacity"), "0.5");
}

/* "Animation classes": a script animation is above CSS animations in composite order, in getAnimations() and in the value. */
TEST(AnimEffectStack, AScriptAnimationIsOverACssAnimation) {
  Ew w("<style>@keyframes k { from { opacity: 0 } to { opacity: 0 } } #a { animation: k 1s linear }</style><div id=a></div>");
  w.at(0);
  w.flush("#a");
  Arg a;
  a.list("opacity", {"1", "1"});
  w.animate("#a", a, 1000);
  w.frame();
  w.at(100);
  EXPECT_EQ(w.list("#a"), "A:k S:0");
  EXPECT_EQ(w.v("#a", "opacity"), "1");
  EXPECT_EQ(w.list(nullptr), "A:k S:0");
}

/* "The relevant animations for a subtree", pseudo-element targets, and an animation with no target. */
TEST(AnimEffectTarget, GetAnimationsFindsScriptAnimationsByTarget) {
  Ew w("<style>#a::before { content: 'x' }</style><div id=a><p id=p></p></div><div id=b></div>");
  w.at(0);
  Arg a;
  a.list("opacity", {"0", "1"});
  w.animate("#a", a, 1000);
  w.animate("#p", a, 1000);
  GTEXT_Anim_Effect_Options o = duration(1000);
  o.has_pseudo = true;
  o.pseudo = a.t(":before");
  w.animate("#a", a, 1000, nullptr, &o);
  EXPECT_EQ(w.list("#a"), "S:0");
  EXPECT_EQ(w.list("#a", true), "S:0 S:1 S:2(::before)") << "the subtree: the element, its descendants and the pseudo-elements";
  EXPECT_EQ(w.list("#a", false, "::before"), "S:2(::before)");
  EXPECT_EQ(w.list("#b"), "");
  EXPECT_EQ(w.list(nullptr), "S:0 S:1 S:2(::before)");
}

/* "pseudo-element parsing": the legacy single-colon forms are the two-colon ones, and an invalid selector is a SyntaxError that leaves the effect as it was. */
TEST(AnimEffectTarget, ThePseudoElementOfAnEffect) {
  Ew w(PAGE);
  Arg a;
  a.list("opacity", {"0", "1"});
  GTEXT_Anim_Effect * e = nullptr;
  ASSERT_EQ(gtext_anim_world_effect_new(w.world, w.h("#a"), a.get(), nullptr, &e), GTEXT_ANIM_OK);
  const void * el = nullptr;
  GTEXT_CSS_String ps;
  ASSERT_EQ(gtext_anim_effect_get_target(e, &el, &ps), GTEXT_ANIM_OK);
  EXPECT_EQ(el, w.h("#a"));
  EXPECT_EQ(ps.length, 0u);
  ASSERT_EQ(gtext_anim_effect_set_pseudo(e, ":after", 6), GTEXT_ANIM_OK);
  gtext_anim_effect_get_target(e, &el, &ps);
  EXPECT_EQ(std::string(ps.data, ps.length), "::after");
  EXPECT_EQ(gtext_anim_effect_set_pseudo(e, "::nonsense(", 11), GTEXT_ANIM_SYNTAX_ERROR);
  EXPECT_EQ(gtext_anim_effect_set_pseudo(e, "after", 5), GTEXT_ANIM_SYNTAX_ERROR);
  EXPECT_EQ(gtext_anim_effect_set_pseudo(e, "::before ", 9), GTEXT_ANIM_SYNTAX_ERROR);
  gtext_anim_effect_get_target(e, &el, &ps);
  EXPECT_EQ(std::string(ps.data, ps.length), "::after") << "a refused value leaves it as it was";
  ASSERT_EQ(gtext_anim_effect_set_pseudo(e, nullptr, 0), GTEXT_ANIM_OK);
  gtext_anim_effect_get_target(e, &el, &ps);
  EXPECT_EQ(ps.length, 0u);
  GTEXT_Anim_Effect_Options o{};
  o.has_pseudo = true;
  o.pseudo = a.t("::??");
  GTEXT_Anim_Effect * bad = nullptr;
  EXPECT_EQ(gtext_anim_world_effect_new(w.world, w.h("#a"), a.get(), &o, &bad), GTEXT_ANIM_SYNTAX_ERROR);
}

/* "Removing replaced animations": a finished animation whose every property is overridden by a finished one above it is removed, with a remove event, and is no longer in getAnimations() or the stack; persist() keeps it. */
TEST(AnimEffectReplace, AFinishedAnimationThatIsFullyOverriddenIsRemoved) {
  Ew w(PAGE);
  w.at(0);
  Arg a;
  a.list("opacity", {"0.5", "0.5"});
  Arg b;
  b.list("opacity", {"0.75", "0.75"});
  GTEXT_Anim_Effect_Options o = duration(100);
  o.timing.present |= GTEXT_ANIM_TIMING_FILL;
  o.timing.values.fill = GTEXT_ANIM_FILL_FORWARDS;
  GTEXT_Anim_Animation * first = w.animate("#a", a, 100, nullptr, &o);
  GTEXT_Anim_Animation * second = w.animate("#a", b, 100, nullptr, &o);
  GTEXT_Anim_Animation * third = w.animate("#a", b, 100, nullptr, &o);
  ASSERT_EQ(gtext_anim_animation_persist(third), GTEXT_ANIM_OK);
  w.frame();
  w.at(200);
  GTEXT_Anim_Events * ev = nullptr;
  ASSERT_EQ(gtext_anim_world_update(w.world, &ev), GTEXT_ANIM_OK);
  std::string types;
  for (size_t i = 0; i < gtext_anim_events_count(ev); i++) {
    GTEXT_Anim_Event e{};
    gtext_anim_events_at(ev, i, &e);
    types += e.type == GTEXT_ANIM_EVENT_REMOVE ? "remove " : e.type == GTEXT_ANIM_EVENT_FINISH ? "finish " : "? ";
  }
  gtext_anim_events_free(ev);
  EXPECT_EQ(types, "remove remove ") << "the finish notification steps run at a microtask checkpoint";
  ASSERT_EQ(gtext_anim_world_checkpoint(w.world), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_world_update(w.world, &ev), GTEXT_ANIM_OK);
  types.clear();
  for (size_t i = 0; i < gtext_anim_events_count(ev); i++) {
    GTEXT_Anim_Event e{};
    gtext_anim_events_at(ev, i, &e);
    types += e.type == GTEXT_ANIM_EVENT_FINISH ? "finish " : "? ";
  }
  gtext_anim_events_free(ev);
  EXPECT_EQ(types, "finish finish finish ");
  GTEXT_Anim_Animation_State s;
  ASSERT_EQ(gtext_anim_animation_state(first, {true, 200}, &s), GTEXT_ANIM_OK);
  EXPECT_EQ(s.replace_state, GTEXT_ANIM_REPLACE_REMOVED);
  ASSERT_EQ(gtext_anim_animation_state(second, {true, 200}, &s), GTEXT_ANIM_OK);
  EXPECT_EQ(s.replace_state, GTEXT_ANIM_REPLACE_REMOVED);
  ASSERT_EQ(gtext_anim_animation_state(third, {true, 200}, &s), GTEXT_ANIM_OK);
  EXPECT_EQ(s.replace_state, GTEXT_ANIM_REPLACE_PERSISTED);
  EXPECT_EQ(w.list("#a"), "S:2");
  EXPECT_EQ(w.v("#a", "opacity"), "0.75");
}

/* "The animation effects of an animation whose replace state is removed are not included in the effect stacks": the animation above it adds to the value underneath, so the removed one is seen not to contribute: the page's own
   0.25, and 0.25 added, not 0.5 (the removed animation's) and 0.25 added. */
TEST(AnimEffectReplace, ARemovedAnimationIsNotInTheStack) {
  Ew w(PAGE);
  w.at(0);
  Arg a;
  a.list("opacity", {"0.5", "0.5"});
  Arg b;
  b.list("opacity", {"0.25", "0.25"});
  GTEXT_Anim_Effect_Options o = duration(100);
  o.timing.present |= GTEXT_ANIM_TIMING_FILL;
  o.timing.values.fill = GTEXT_ANIM_FILL_FORWARDS;
  GTEXT_Anim_Effect_Options added = o;
  added.has_composite = true;
  added.composite = GTEXT_ANIM_COMPOSITE_ADD;
  w.animate("#a", a, 100, nullptr, &o);
  w.animate("#a", b, 100, nullptr, &added);
  w.frame();
  w.at(10);
  EXPECT_EQ(w.v("#a", "opacity"), "0.75") << "both are running: 0.5, and 0.25 added";
  w.at(200);
  w.frame();
  EXPECT_EQ(w.list("#a"), "S:1") << "the first is replaced: the second covers its property, and is finished";
  EXPECT_EQ(w.v("#a", "opacity"), "0.5") << "0.25 of the page, and the 0.25 of the second added; the removed one is not underneath";
}

/* "persist()" and a property no later animation covers keep the animation. */
TEST(AnimEffectReplace, AnAnimationOfAnotherPropertyIsNotReplaced) {
  Ew w(PAGE);
  w.at(0);
  Arg a;
  a.list("opacity", {"0.5", "0.5"}).list("zIndex", {"1", "1"});
  Arg b;
  b.list("opacity", {"0.75", "0.75"});
  GTEXT_Anim_Effect_Options o = duration(100);
  o.timing.present |= GTEXT_ANIM_TIMING_FILL;
  o.timing.values.fill = GTEXT_ANIM_FILL_FORWARDS;
  w.animate("#a", a, 100, nullptr, &o);
  w.animate("#a", b, 100, nullptr, &o);
  w.frame();
  w.at(200);
  w.frame();
  EXPECT_EQ(w.list("#a"), "S:0 S:1");
}

/* The members of KeyframeEffect: target, composite, iterationComposite, the timing and its easing, getComputedTiming() at the animation's time, and the effect that leaves an animation. */
TEST(AnimEffectObject, TheMembersOfAKeyframeEffect) {
  Ew w(PAGE);
  w.at(0);
  Arg a;
  a.list("opacity", {"0", "1"});
  GTEXT_Anim_Effect_Options o = duration(1000);
  o.has_composite = true;
  o.composite = GTEXT_ANIM_COMPOSITE_ACCUMULATE;
  GTEXT_Anim_Effect * e = nullptr;
  ASSERT_EQ(gtext_anim_world_effect_new(w.world, w.h("#a"), a.get(), &o, &e), GTEXT_ANIM_OK);
  EXPECT_EQ(gtext_anim_effect_get_composite(e), GTEXT_ANIM_COMPOSITE_ACCUMULATE);
  EXPECT_EQ(gtext_anim_effect_get_iteration_composite(e), GTEXT_ANIM_ITERATION_REPLACE);
  EXPECT_EQ(gtext_anim_effect_animation(e), nullptr);
  GTEXT_Anim_Effect_Timing t;
  GTEXT_CSS_String easing;
  ASSERT_EQ(gtext_anim_effect_get_timing(e, &t, &easing), GTEXT_ANIM_OK);
  EXPECT_EQ(t.duration, 1000);
  EXPECT_EQ(std::string(easing.data, easing.length), "linear");
  GTEXT_Anim_Computed_Timing c;
  ASSERT_EQ(gtext_anim_effect_get_computed_timing(e, {true, 0}, &c), GTEXT_ANIM_OK);
  EXPECT_FALSE(c.local_time.resolved) << "no animation: no local time";
  EXPECT_EQ(c.phase, GTEXT_ANIM_PHASE_IDLE);
  /* an animation holds it */
  uint64_t id = 0;
  GTEXT_Anim_Animation * an = nullptr;
  ASSERT_EQ(gtext_anim_world_animation_new(w.world, e, GTEXT_ANIM_TIMELINE_DOCUMENT, &id, &an), GTEXT_ANIM_OK);
  EXPECT_EQ(gtext_anim_effect_animation(e), an);
  EXPECT_EQ(gtext_anim_animation_effect(an), e);
  ASSERT_EQ(gtext_anim_animation_play(an, {true, 0}), GTEXT_ANIM_OK);
  w.frame();
  ASSERT_EQ(gtext_anim_effect_get_computed_timing(e, {true, 250}, &c), GTEXT_ANIM_OK);
  EXPECT_EQ(c.local_time.ms, 250);
  EXPECT_DOUBLE_EQ(c.progress, 0.25);
  /* updateTiming goes through the animation */
  GTEXT_Anim_Optional_Timing in{};
  in.present = GTEXT_ANIM_TIMING_DURATION | GTEXT_ANIM_TIMING_EASING;
  in.values = gtext_anim_timing_default();
  in.values.duration = 500;
  in.values.duration_auto = false;
  GTEXT_Anim_Text ease = a.t("ease-in");
  ASSERT_EQ(gtext_anim_effect_update_timing(e, &in, &ease, {true, 250}), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_effect_get_timing(e, &t, &easing), GTEXT_ANIM_OK);
  EXPECT_EQ(t.duration, 500);
  EXPECT_EQ(std::string(easing.data, easing.length), "ease-in");
  GTEXT_Anim_Animation_State s;
  ASSERT_EQ(gtext_anim_animation_state(an, {true, 250}, &s), GTEXT_ANIM_OK);
  EXPECT_EQ(s.effect_end, 500);
  GTEXT_Anim_Text bad = a.t("nonsense");
  EXPECT_EQ(gtext_anim_effect_update_timing(e, &in, &bad, {true, 250}), GTEXT_ANIM_TYPE_ERROR);
  ASSERT_EQ(gtext_anim_effect_get_timing(e, &t, &easing), GTEXT_ANIM_OK);
  EXPECT_EQ(std::string(easing.data, easing.length), "ease-in") << "all or nothing";
  /* "If new effect is ... the associated effect of another previous animation, set the associated effect of previous animation to null" */
  GTEXT_Anim_Animation * other = nullptr;
  ASSERT_EQ(gtext_anim_world_animation_new(w.world, nullptr, GTEXT_ANIM_TIMELINE_DOCUMENT, &id, &other), GTEXT_ANIM_OK);
  ASSERT_EQ(gtext_anim_animation_set_effect_object(other, e, {true, 250}), GTEXT_ANIM_OK);
  EXPECT_EQ(gtext_anim_animation_effect(an), nullptr);
  EXPECT_EQ(gtext_anim_effect_animation(e), other);
  ASSERT_EQ(gtext_anim_animation_state(an, {true, 250}, &s), GTEXT_ANIM_OK);
  EXPECT_FALSE(s.has_effect);
  /* copy */
  GTEXT_Anim_Effect * copy = nullptr;
  ASSERT_EQ(gtext_anim_world_effect_copy(w.world, e, &copy), GTEXT_ANIM_OK);
  EXPECT_EQ(gtext_anim_effect_get_composite(copy), GTEXT_ANIM_COMPOSITE_ACCUMULATE);
  ASSERT_EQ(gtext_anim_effect_get_timing(copy, &t, &easing), GTEXT_ANIM_OK);
  EXPECT_EQ(t.duration, 500);
  EXPECT_EQ(std::string(easing.data, easing.length), "ease-in");
  GTEXT_Anim_Keyframes * k = nullptr;
  ASSERT_EQ(gtext_anim_effect_get_keyframes(copy, &k), GTEXT_ANIM_OK);
  EXPECT_EQ(frames_text(k), "null/0 linear opacity=0 | null/1 linear opacity=1");
  gtext_anim_keyframes_free(k);
}

/* setKeyframes(): "If that procedure throws an exception, this effect's keyframes are not modified", and the effect value follows the new keyframes at once. */
TEST(AnimEffectObject, SetKeyframesReplacesTheKeyframesOrLeavesThem) {
  Ew w(PAGE);
  w.at(0);
  Arg a;
  a.list("opacity", {"0", "1"});
  GTEXT_Anim_Effect * e = nullptr;
  GTEXT_Anim_Animation * an = w.animate("#a", a, 1000);
  e = gtext_anim_animation_effect(an);
  ASSERT_NE(e, nullptr);
  w.frame();
  w.at(500);
  EXPECT_EQ(w.v("#a", "opacity"), "0.5");
  Arg b;
  b.list("opacity", {"1", "0"});
  ASSERT_EQ(gtext_anim_effect_set_keyframes(e, b.get()), GTEXT_ANIM_OK);
  EXPECT_EQ(w.v("#a", "opacity"), "0.5");
  w.at(250);
  EXPECT_EQ(w.v("#a", "opacity"), "0.75");
  Arg bad;
  bad.frame({{"opacity", "1"}}, 2);
  EXPECT_EQ(gtext_anim_effect_set_keyframes(e, bad.get()), GTEXT_ANIM_TYPE_ERROR);
  EXPECT_EQ(w.v("#a", "opacity"), "0.75");
  /* target = null: the effect applies to nothing */
  ASSERT_EQ(gtext_anim_effect_set_target(e, nullptr), GTEXT_ANIM_OK);
  EXPECT_EQ(w.v("#a", "opacity"), "0.25");
  ASSERT_EQ(gtext_anim_effect_set_target(e, w.h("#b")), GTEXT_ANIM_OK);
  EXPECT_EQ(w.v("#b", "opacity"), "0.75");
  EXPECT_EQ(w.v("#a", "opacity"), "0.25");
}

/* An animation with a null effect, finished tasks and events: pausing, seeking and cancel move the value. */
TEST(AnimEffectAnimate, PausingSeekingAndCancelling) {
  Ew w(PAGE);
  w.at(0);
  Arg a;
  a.list("opacity", {"0", "1"});
  GTEXT_Anim_Animation * an = w.animate("#a", a, 1000);
  w.frame();
  w.at(500);
  ASSERT_EQ(gtext_anim_animation_pause(an, {true, 500}), GTEXT_ANIM_OK);
  gtext_anim_world_touch(w.world);
  w.frame();
  w.at(900);
  EXPECT_EQ(w.v("#a", "opacity"), "0.5") << "paused at 500";
  ASSERT_EQ(gtext_anim_animation_set_current_time(an, {true, 750}, {true, 900}), GTEXT_ANIM_OK);
  gtext_anim_world_touch(w.world);
  EXPECT_EQ(w.v("#a", "opacity"), "0.75");
  ASSERT_EQ(gtext_anim_animation_cancel(an, {true, 900}), GTEXT_ANIM_OK);
  gtext_anim_world_touch(w.world);
  EXPECT_EQ(w.v("#a", "opacity"), "0.25") << "idle: no effect";
  EXPECT_EQ(w.list("#a"), "");
}

/* An allocation that fails is an OOM, not a crash and not a leak: for each allocation of an operation in turn, make that one fail; the call reports GTEXT_ANIM_OOM (or the style read GTEXT_CSS_E_OOM), what it made is released (the
   world's allocator holds nothing after the world is freed), and the same call again goes through. */
namespace {

template <typename Setup, typename Fn>
size_t oom_sweep_with(Setup setup, Fn fn, size_t limit = 600) {
  size_t failures = 0;
  for (size_t n = 1; n < limit; n++) {
    FaultAllocator fa;
    {
      Ew w(PAGE, &fa.alloc);
      w.at(0);
      setup(w);
      fa.arm(n);
      const int st = fn(w, true);
      const bool failed = fa.failed;
      fa.disarm();
      if (!failed) {
        EXPECT_EQ(st, 0) << n << ": no allocation failed, the call went through";
        break;
      }
      failures++;
      EXPECT_TRUE(st == (int)GTEXT_ANIM_OOM || st == (int)GTEXT_CSS_E_OOM) << n << ": " << st;
      EXPECT_EQ(fn(w, false), 0) << n << ": the same call again goes through";
    }
    EXPECT_TRUE(fa.live.empty()) << n << ": " << fa.live.size() << " left";
    EXPECT_EQ(fa.bad_frees, 0u);
  }
  return failures;
}

template <typename Fn>
size_t oom_sweep(Fn fn, size_t limit = 600) {
  return oom_sweep_with([](Ew &) {}, fn, limit);
}

}  // namespace

TEST(AnimEffectOom, ProcessingAKeyframesArgument) {
  Arg a;
  a.frame({{"marginLeft", "1px"}, {"opacity", "0"}}, 0, "ease-in", GTEXT_ANIM_KF_COMPOSITE_ADD).frame({{"opacity", "1"}}, NAN, "steps(3)").frame({{"opacity", "0.5"}}, 1);
  Arg b;
  b.list("opacity", {"0", "0.5", "1"}).list("left", {"0px", "100px"}).offset({NAN, 0.25}).easing({"ease-in", "ease-out"}).composite({GTEXT_ANIM_KF_COMPOSITE_ACCUMULATE});
  EXPECT_GT(oom_sweep([&](Ew & w, bool) {
    GTEXT_Anim_Keyframes * k = nullptr;
    const GTEXT_Anim_Status st = gtext_anim_keyframes_process(w.world, a.get(), &k);
    EXPECT_EQ(k != nullptr, st == GTEXT_ANIM_OK);
    gtext_anim_keyframes_free(k);
    return (int)st;
  }), 20u);
  EXPECT_GT(oom_sweep([&](Ew & w, bool) {
    GTEXT_Anim_Keyframes * k = nullptr;
    const GTEXT_Anim_Status st = gtext_anim_keyframes_process(w.world, b.get(), &k);
    EXPECT_EQ(k != nullptr, st == GTEXT_ANIM_OK);
    gtext_anim_keyframes_free(k);
    return (int)st;
  }), 20u);
}

TEST(AnimEffectOom, MakingAnEffectAndAnimatingAndReadingTheStyle) {
  Arg a;
  a.list("opacity", {"0", "1"});
  GTEXT_Anim_Effect_Options o = duration(1000);
  o.has_pseudo = true;
  o.pseudo = a.t(":before");
  o.timing.present |= GTEXT_ANIM_TIMING_EASING;
  o.easing = a.t("ease-in-out");
  EXPECT_GT(oom_sweep([&](Ew & w, bool) {
    GTEXT_Anim_Effect * e = nullptr;
    return (int)gtext_anim_world_effect_new(w.world, w.h("#a"), a.get(), &o, &e);
  }), 20u);
  EXPECT_GT(oom_sweep([&](Ew & w, bool) {
    GTEXT_Anim_Effect_Options p = duration(1000);
    GTEXT_Anim_Animation * an = nullptr;
    uint64_t id = 0;
    const GTEXT_Anim_Status st = gtext_anim_world_animate(w.world, w.h("#a"), a.get(), &p, nullptr, &id, &an);
    EXPECT_EQ(an != nullptr, st == GTEXT_ANIM_OK);
    return (int)st;
  }), 15u);
  /* the style read of an animated element, with the world's allocator failing in the sampler's hook */
  EXPECT_GT(oom_sweep_with([&](Ew & w) {
    GTEXT_Anim_Effect_Options p = duration(1000);
    GTEXT_Anim_Animation * an = nullptr;
    uint64_t id = 0;
    ASSERT_EQ(gtext_anim_world_animate(w.world, w.h("#a"), a.get(), &p, nullptr, &id, &an), GTEXT_ANIM_OK);
    w.frame();
    w.at(500);
  }, [&](Ew & w, bool) {
    GTEXT_CSSOM_Decl * d = nullptr;
    GTEXT_CSS_Status st = gtext_cssom_computed_style(w.m.doc->d, w.h("#a"), nullptr, 0, &d);
    if (st == GTEXT_CSS_OK) {
      char * text = nullptr;
      size_t n = 0;
      st = gtext_cssom_decl_get_property_value(d, "opacity", 7, &text, &n);   /* the style is made at the first read */
      if (st == GTEXT_CSS_OK) {
        EXPECT_EQ(std::string(text, n), "0.5");
        gtext_css_serialize_buffer_free(nullptr, text);
      }
    }
    gtext_cssom_decl_release(d);
    return (int)st;
  }, 200), 0u);
}

TEST(AnimEffectOom, TheEffectMethods) {
  Arg a;
  a.list("opacity", {"0", "1"});
  Arg b;
  b.list("opacity", {"1", "0.5"}).list("left", {"1px", "2px"});
  GTEXT_Anim_Effect * e = nullptr;
  EXPECT_GT(oom_sweep_with([&](Ew & w) {
    GTEXT_Anim_Effect_Options p = duration(1000);
    ASSERT_EQ(gtext_anim_world_effect_new(w.world, w.h("#a"), a.get(), &p, &e), GTEXT_ANIM_OK);
  }, [&](Ew & w, bool armed) {
    GTEXT_Anim_Status st = gtext_anim_effect_set_keyframes(e, b.get());
    if (st != GTEXT_ANIM_OK) {
      GTEXT_Anim_Keyframes * k = nullptr;
      EXPECT_EQ(gtext_anim_effect_get_keyframes(e, &k), armed ? GTEXT_ANIM_OK : GTEXT_ANIM_OK);
      EXPECT_EQ(frames_text(k), "null/0 linear opacity=0 | null/1 linear opacity=1") << "the keyframes are as they were";
      gtext_anim_keyframes_free(k);
      return (int)st;
    }
    GTEXT_Anim_Effect * copy = nullptr;
    return (int)gtext_anim_world_effect_copy(w.world, e, &copy);
  }), 10u);
}

namespace {

std::string committed(GTEXT_Anim_World * world, const GTEXT_Anim_Animation * a, GTEXT_Anim_Status * status = nullptr) {
  GTEXT_Anim_Style_Values * v = nullptr;
  const GTEXT_Anim_Status st = gtext_anim_world_commit_styles(world, a, &v);
  if (status) {
    *status = st;
  }
  else {
    EXPECT_EQ(st, GTEXT_ANIM_OK);
  }
  std::string out;
  for (size_t i = 0; i < gtext_anim_style_values_count(v); i++) {
    GTEXT_CSS_String n, val;
    EXPECT_TRUE(gtext_anim_style_values_at(v, i, &n, &val));
    out += std::string(i ? "; " : "") + std::string(n.data, n.length) + ": " + std::string(val.data, val.length);
  }
  gtext_anim_style_values_free(v);
  return out;
}

}  // namespace

/* "commit computed styles ... the endpoint-inclusive active interval flag": an animation that has just finished, with no fill, commits its last value; the declarations are the physical longhands. */
TEST(AnimEffectCommit, TheEndpointIsInclusiveAndShorthandsAreLonghands) {
  Ew w(PAGE);
  w.at(0);
  Arg a;
  a.list("opacity", {"0", "1"}).list("margin", {"0px", "10px"});
  GTEXT_Anim_Animation * an = w.animate("#a", a, 1000);
  w.frame();
  w.at(500);
  EXPECT_EQ(committed(w.world, an), "margin-bottom: 5px; margin-left: 5px; margin-right: 5px; margin-top: 5px; opacity: 0.5");
  w.at(1000);
  w.frame();
  EXPECT_EQ(w.v("#a", "opacity"), "0.25") << "after the end, with no fill, nothing applies";
  EXPECT_EQ(committed(w.world, an), "margin-bottom: 10px; margin-left: 10px; margin-right: 10px; margin-top: 10px; opacity: 1");
  EXPECT_EQ(gtext_anim_style_values_element(nullptr), nullptr);
}

/* "Remove from partialEffectStack any animation effects whose animation has a higher composite order than animation": the committed value of the lower animation leaves the higher out; of the higher it includes both. */
TEST(AnimEffectCommit, TheEffectsAboveTheAnimationAreLeftOut) {
  Ew w(PAGE);
  w.at(0);
  Arg a;
  a.list("opacity", {"0.2", "0.2"});
  Arg b;
  b.list("opacity", {"0.8", "0.8"});
  GTEXT_Anim_Effect_Options o = duration(1000);
  GTEXT_Anim_Animation * lower = w.animate("#a", a, 1000, nullptr, &o);
  GTEXT_Anim_Animation * higher = w.animate("#a", b, 1000, nullptr, &o);
  w.frame();
  w.at(10);
  EXPECT_EQ(w.v("#a", "opacity"), "0.8");
  EXPECT_EQ(committed(w.world, lower), "opacity: 0.2");
  EXPECT_EQ(committed(w.world, higher), "opacity: 0.8");
  EXPECT_EQ(w.v("#a", "opacity"), "0.8") << "a commit leaves the document as it was";
}

/* The two exceptions: a pseudo-element is no element with a style attribute; an element that is not rendered has no style to commit. And an animation with no effect has no targets. */
TEST(AnimEffectCommit, TheExceptions) {
  Ew w("<style>#n { display: none } #a::before { content: 'x' }</style><div id=a></div><div id=n></div>");
  w.at(0);
  Arg a;
  a.list("opacity", {"0", "1"});
  GTEXT_Anim_Effect_Options o = duration(1000);
  o.has_pseudo = true;
  o.pseudo = a.t("::before");
  GTEXT_Anim_Status st;
  GTEXT_Anim_Animation * pseudo = w.animate("#a", a, 1000, nullptr, &o);
  committed(w.world, pseudo, &st);
  EXPECT_EQ(st, GTEXT_ANIM_NO_MODIFICATION_ALLOWED);
  GTEXT_Anim_Animation * hidden = w.animate("#n", a, 1000);
  committed(w.world, hidden, &st);
  EXPECT_EQ(st, GTEXT_ANIM_INVALID_STATE);
  GTEXT_Anim_Animation * none = nullptr;
  uint64_t id = 0;
  ASSERT_EQ(gtext_anim_world_animation_new(w.world, nullptr, GTEXT_ANIM_TIMELINE_DOCUMENT, &id, &none), GTEXT_ANIM_OK);
  EXPECT_EQ(committed(w.world, none), "");
}

/* "Since the procedure to commit computed styles includes the effect values for the animation even if it is removed": the animation that was replaced still commits. */
TEST(AnimEffectCommit, AReplacedAnimationStillCommits) {
  Ew w(PAGE);
  w.at(0);
  Arg a;
  a.list("opacity", {"0.5", "0.5"});
  Arg b;
  b.list("opacity", {"0.75", "0.75"});
  GTEXT_Anim_Effect_Options o = duration(100);
  o.timing.present |= GTEXT_ANIM_TIMING_FILL;
  o.timing.values.fill = GTEXT_ANIM_FILL_FORWARDS;
  GTEXT_Anim_Animation * first = w.animate("#a", a, 100, nullptr, &o);
  w.animate("#a", b, 100, nullptr, &o);
  w.frame();
  w.at(200);
  w.frame();
  GTEXT_Anim_Animation_State s;
  ASSERT_EQ(gtext_anim_animation_state(first, {true, 200}, &s), GTEXT_ANIM_OK);
  ASSERT_EQ(s.replace_state, GTEXT_ANIM_REPLACE_REMOVED);
  EXPECT_EQ(committed(w.world, first), "opacity: 0.5");
}
