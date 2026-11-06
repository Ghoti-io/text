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
 * `getComputedStyle()` under animation (css_cssom_core.h, GTEXT_CSSOM_Layout_Interface::animation_time and ::timeline_version; css_cssom_computed.h, the origin
 * GTEXT_CSSOM_ORIGIN_ABSENT_TIMELINE): the value of a property is the value of the animation effect stack at the time the caller gives, keyed by the time as well as the tree, and a
 * property an animation targets that was not given a time says so.
 *
 * CSS Animations 1: "The computed value ... " is the underlying value without an animation; Web Animations 1, "Applying the composited result": the composited value is added to the
 * animation origin of the cascade, and the computed value of the property is that value.
 */

#include <map>
#include <string>

#include <gtest/gtest.h>

#include "cssom_computed_test_support.h"

using namespace cc;

namespace {

/// The layout hooks of animation: the time of each animation by its name, and the version of the timeline.
struct Clock {
  std::map<std::string, double> times;
  uint64_t version = 1;
  size_t asked = 0;
  bool has_time = true;
  bool has_version = true;
  GTEXT_CSS_Status fail = GTEXT_CSS_OK;
  GTEXT_CSSOM_Layout_Interface iface{};

  Clock() {
    iface.size = sizeof iface;
    iface.user = this;
    iface.animation_time = &Clock::time;
    iface.timeline_version = &Clock::ver;
  }
  void install(GTEXT_CSSOM_Config & c) {
    if (!has_time) {
      iface.animation_time = nullptr;
    }
    if (!has_version) {
      iface.timeline_version = nullptr;
    }
    c.layout = &iface;
  }
  static GTEXT_CSS_Status time(void * user, const void *, const GTEXT_CSS_Selector_List *, const GTEXT_CSS_Anim_Effect * e, double * ms, bool * known) {
    Clock * self = static_cast<Clock *>(user);
    self->asked++;
    if (self->fail != GTEXT_CSS_OK) {
      return self->fail;
    }
    auto it = self->times.find(std::string(e->name.data, e->name.length));
    *known = it != self->times.end();
    if (*known) {
      *ms = it->second;
    }
    return GTEXT_CSS_OK;
  }
  static uint64_t ver(void * user) {
    return static_cast<Clock *>(user)->version;
  }
};

std::string page(const std::string & css, const std::string & body = "<div id=t></div>") {
  return "<!DOCTYPE html><html><head><style id=s>" + css + "</style></head><body>" + body + "</body></html>";
}

const char * kGap = "@keyframes a { from { gap: 0px } to { gap: 100px } } #t { animation: a 10s linear -5s paused }";

/* Animations 1 and Web Animations 1: a paused animation with a delay of -5s over a 10s effect is half way through its keyframes at the animation's time 0. */
TEST(CssomAnimation, APausedAnimationIsSampledAtTheTimeTheCallerGives) {
  Clock clock;
  clock.times["a"] = 0.0;
  Cs m(page(kGap), [&](GTEXT_CSSOM_Config & c) { clock.install(c); });
  Decl d = m.computed("#t");
  EXPECT_EQ(get(d.d, "gap"), "50px");
  EXPECT_EQ(get(d.d, "row-gap"), "50px");
  EXPECT_EQ(get(d.d, "column-gap"), "50px");
  EXPECT_EQ(origin(d.d, "row-gap"), "computed");
}

/* With no callback, or no time for the animation, the computed value is the underlying one and the origin says that a timeline was missing; the scoring interpreter lists a test that
   reads it as needing one. */
TEST(CssomAnimation, WithoutATimeTheUnderlyingValueStandsAndTheOriginSaysSo) {
  Clock none;
  none.has_time = false;
  Cs a(page(kGap), [&](GTEXT_CSSOM_Config & c) { none.install(c); });
  Decl da = a.computed("#t");
  EXPECT_EQ(get(da.d, "row-gap"), "normal");
  EXPECT_EQ(origin(da.d, "row-gap"), "absent-timeline");
  EXPECT_EQ(origin(da.d, "gap"), "absent-timeline") << "a shorthand is as strong as its longhands";
  EXPECT_EQ(origin(da.d, "color"), "computed") << "a property no animation targets is not affected";
  Cs plain(page(kGap));
  Decl dp = plain.computed("#t");
  EXPECT_EQ(get(dp.d, "row-gap"), "normal");
  EXPECT_EQ(origin(dp.d, "row-gap"), "absent-timeline") << "a document with no layout at all is a document with no timeline";
  Clock other;
  other.times["b"] = 1.0;
  Cs unknown(page(kGap), [&](GTEXT_CSSOM_Config & c) { other.install(c); });
  Decl du = unknown.computed("#t");
  EXPECT_EQ(get(du.d, "row-gap"), "normal");
  EXPECT_EQ(origin(du.d, "row-gap"), "absent-timeline");
  // an element no animation names has nothing to report
  Cs idle(page("@keyframes a { from { gap: 0px } to { gap: 100px } }"));
  EXPECT_EQ(origin(idle.computed("#t").d, "row-gap"), "computed");
}

TEST(CssomAnimation, AnUntimedCustomPropertyIsReportedToo) {
  Clock clock;
  clock.has_time = false;
  Cs m(page("@keyframes a { from { --x: 1px; --y: 1px } to { --x: 2px } } #t { animation: a 10s linear both; --x: 5px; --z: 5px }"), [&](GTEXT_CSSOM_Config & c) { clock.install(c); });
  Decl d = m.computed("#t");
  EXPECT_EQ(get(d.d, "--x"), "5px");
  EXPECT_EQ(origin(d.d, "--x"), "absent-timeline");
  EXPECT_EQ(origin(d.d, "--z"), "computed") << "a custom property no keyframe sets is not affected";
  EXPECT_EQ(origin(d.d, "--y"), "absent") << "and one the element does not have is not there";
}

/* "Sampling is a pure function of (style, keyframes, time): cached styles are keyed by the time as well as the tree." */
TEST(CssomAnimation, TheTimeIsPartOfWhatACachedStyleIsKeyedBy) {
  Clock clock;
  clock.times["a"] = 2500.0;
  Cs m(page("@keyframes a { from { width: 0px } to { width: 100px } } #t { animation: a 10s linear both }"), [&](GTEXT_CSSOM_Config & c) { clock.install(c); });
  Decl d = m.computed("#t");
  EXPECT_EQ(get(d.d, "width"), "25px");
  const uint64_t before = m.dc().computed_computes;
  EXPECT_EQ(get(d.d, "width"), "25px");
  EXPECT_EQ(m.dc().computed_computes, before) << "the same time and tree: nothing is computed again";
  clock.times["a"] = 5000.0;
  EXPECT_EQ(get(d.d, "width"), "25px") << "the caller has not said that time moved";
  clock.version++;
  EXPECT_EQ(get(d.d, "width"), "50px");
  EXPECT_GT(m.dc().computed_computes, before);
  clock.times["a"] = 7500.0;
  clock.version++;
  Decl fresh = m.computed("#t");
  EXPECT_EQ(get(fresh.d, "width"), "75px");
  EXPECT_EQ(get(d.d, "width"), "75px") << "a live declaration follows the time too";
}

/* A time callback with no version cannot be cached across reads (the time could be anything), and is asked each time. */
TEST(CssomAnimation, ATimeWithNoVersionIsNeverCachedAcrossReads) {
  Clock clock;
  clock.has_version = false;
  clock.times["a"] = 2500.0;
  Cs m(page("@keyframes a { from { width: 0px } to { width: 100px } } #t { animation: a 10s linear both }"), [&](GTEXT_CSSOM_Config & c) { clock.install(c); });
  Decl d = m.computed("#t");
  EXPECT_EQ(get(d.d, "width"), "25px");
  clock.times["a"] = 5000.0;
  EXPECT_EQ(get(d.d, "width"), "50px");
}

TEST(CssomAnimation, ACallbackThatFailsFailsTheRead) {
  Clock clock;
  clock.times["a"] = 0.0;
  clock.fail = GTEXT_CSS_E_UNSUPPORTED;
  Cs m(page(kGap), [&](GTEXT_CSSOM_Config & c) { clock.install(c); });
  Decl d = m.computed("#t");
  size_t n = 0;
  EXPECT_EQ(gtext_cssom_decl_length(d.d, &n), GTEXT_CSS_E_UNSUPPORTED);
  GTEXT_CSSOM_Computed_Failure f;
  ASSERT_EQ(gtext_cssom_computed_failure(m.d(), &f), GTEXT_CSS_OK);
  EXPECT_STREQ(f.callback, "animation_time") << "the read says which callback it was";
  clock.fail = GTEXT_CSS_OK;
  EXPECT_EQ(get(d.d, "gap"), "50px") << "and the next read, with the callback healthy, is the healthy answer";
}

/* CSSOM: the sheet is live. Changing a keyframe, inserting a rule or removing one is seen by the next read. */
TEST(CssomAnimation, AChangeOfTheKeyframesIsSeenByTheNextRead) {
  Clock clock;
  clock.times["a"] = 5000.0;
  Cs m(page("@keyframes a { from { width: 0px } to { width: 100px } } #t { animation: a 10s linear both }"), [&](GTEXT_CSSOM_Config & c) { clock.install(c); });
  Decl d = m.computed("#t");
  EXPECT_EQ(get(d.d, "width"), "50px");
  GTEXT_CSSOM_Sheet * sheet = m.sheet(m.el("style"));
  ASSERT_NE(sheet, nullptr);
  const char * rule = "@keyframes a { from { width: 0px } to { width: 40px } }";
  GTEXT_CSSOM_Rule * inserted = nullptr;
  ASSERT_EQ(gtext_cssom_sheet_insert_rule(sheet, rule, std::strlen(rule), 2, &inserted, nullptr), GTEXT_CSS_OK);
  gtext_cssom_rule_release(inserted);
  EXPECT_EQ(get(d.d, "width"), "20px") << "the later @keyframes of the same name wins";
  ASSERT_EQ(gtext_cssom_sheet_delete_rule(sheet, 2, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(get(d.d, "width"), "50px");
  gtext_cssom_sheet_release(sheet);
}

/* Values 5, sibling-index(): "resolved for the animated element at sample time" - in a keyframe it is the element's place when the keyframe is computed, and a change of the tree is a
   change of the value. */
TEST(CssomAnimation, SiblingIndexInAKeyframeIsTheElementsAtSampleTime) {
  Clock clock;
  clock.times["--anim"] = 0.0;
  Cs m(page("@keyframes --anim { from { font-weight: calc(100 * sibling-index()) } to { font-weight: 900 } } #t { animation: --anim 1000s step-end }",
            "<div><div id=rm></div><div></div><div id=t></div></div>"),
      [&](GTEXT_CSSOM_Config & c) { clock.install(c); });
  Decl d = m.computed("#t");
  EXPECT_EQ(get(d.d, "font-weight"), "300");
  detach(m.el("#rm"));
  EXPECT_EQ(get(d.d, "font-weight"), "200");
}

/* Properties and Values 1: a registered custom property is animated by its computed type, and the animated value is the one `var()` reads, a shorthand included. */
TEST(CssomAnimation, ARegisteredCustomPropertyIsAnimatedAndReadByVar) {
  Clock clock;
  clock.times["angle-animation"] = 0.0;
  Cs m(page("@property --angle { syntax: \"<angle>\"; inherits: false; initial-value: 0deg }"
            "@keyframes angle-animation { from { --angle: 0deg } to { --angle: 180deg } }"
            "#t { animation: angle-animation 1000s linear -500s paused; background: linear-gradient(var(--angle), black, white) }"),
      [&](GTEXT_CSSOM_Config & c) { clock.install(c); });
  Decl d = m.computed("#t");
  EXPECT_EQ(get(d.d, "--angle"), "90deg");
  EXPECT_EQ(get(d.d, "background-image"), "linear-gradient(90deg, rgb(0, 0, 0), rgb(255, 255, 255))");
}

/* CSS Animations 1: "Unrelated custom properties do not conflict with each other" - a keyframe that sets another custom property later at the same offset adds to it. */
TEST(CssomAnimation, AnUnregisteredCustomPropertyFlipsAtTheMiddle) {
  Clock clock;
  clock.times["test"] = 0.0;
  Cs m(page("@keyframes test { 0% { --x: green } 100% { --x: green } 0% { --unused: yellow } } #t { animation: test 10s linear paused; background-color: var(--x, red) }"),
      [&](GTEXT_CSSOM_Config & c) { clock.install(c); });
  EXPECT_EQ(m.value("#t", "background-color"), "rgb(0, 128, 0)");
}

/* Cascade 6, revert-rule: in a keyframe it is the value of the declaration below (here the element's), also for a registered custom property, which is interpolated by its type. */
TEST(CssomAnimation, ARevertRuleKeyframeIsTheUnderlyingValue) {
  Clock clock;
  clock.times["anim"] = 0.0;
  Cs m(page("@property --x { syntax: \"<length>\"; inherits: false; initial-value: 0px }"
            "@keyframes anim { from { width: revert-rule; --x: revert-rule } to { width: 200px; --x: 200px } }"
            "#target { width: 100px; --x: 100px; animation: anim 3600s steps(2, start) }",
            "<div id=target></div>"),
      [&](GTEXT_CSSOM_Config & c) { clock.install(c); });
  Decl d = m.computed("#target");
  EXPECT_EQ(get(d.d, "width"), "150px");
  EXPECT_EQ(get(d.d, "--x"), "150px");
}

/* A keyframe that needs a callback the caller lacks fails the read naming it, and a layout test that reads such a property is a test that needs a layout, not a test that fails. */
TEST(CssomAnimation, AKeyframeThatNeedsAFontMetricNamesTheCallback) {
  Clock clock;
  clock.times["a"] = 0.0;
  Cs m(page("@keyframes a { from { width: 1ex } to { width: 9ex } } #t { animation: a 4s linear -2s paused }"), [&](GTEXT_CSSOM_Config & c) { clock.install(c); });
  Decl d = m.computed("#t");
  size_t n = 0;
  EXPECT_EQ(gtext_cssom_decl_length(d.d, &n), GTEXT_CSS_E_UNSUPPORTED);
  GTEXT_CSSOM_Computed_Failure f;
  ASSERT_EQ(gtext_cssom_computed_failure(m.d(), &f), GTEXT_CSS_OK);
  EXPECT_STREQ(f.callback, "ex");
}

/* The animation names of the style attribute are read like any other cascaded value: setting it starts the animation. */
TEST(CssomAnimation, AnInlineAnimationNameStartsTheAnimation) {
  Clock clock;
  clock.times["green"] = 0.0;
  Cs m(page("#t { color: red } @keyframes green { from, to { color: green } }", "<div id=t style=\"animation-duration: 1s\"></div>"),
      [&](GTEXT_CSSOM_Config & c) { clock.install(c); });
  EXPECT_EQ(m.value("#t", "color"), "rgb(255, 0, 0)");
  set_attr(m.el("#t"), "style", "animation-duration: 1s; animation-name: green");
  EXPECT_EQ(m.value("#t", "color"), "rgb(0, 128, 0)");
}

/* Pseudo-elements: the animation of `::marker` is sampled for the marker, and only for the properties that apply to it. */
TEST(CssomAnimation, AnAnimationOfAMarkerAnimatesWhatAppliesToIt) {
  Clock clock;
  clock.times["anim"] = 0.0;
  Cs m(page("::marker { animation: anim 2s -1s paused linear } @keyframes anim { from { font-size: 5px; display: flex } to { font-size: 25px; display: none } }",
            "<ul><li id=t>x</li></ul>"),
      [&](GTEXT_CSSOM_Config & c) { clock.install(c); });
  Decl d = m.computed("#t", "::marker");
  EXPECT_EQ(get(d.d, "font-size"), "15px");
  EXPECT_NE(get(d.d, "display"), "none") << "display does not apply to ::marker";
}

}  // namespace
