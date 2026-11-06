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
 * The animation effects of an element (css_animation.h): the `animation-*` longhands of a computed style read as a list of effects.
 *
 * CSS Animations 1: "These list-valued properties, which are all longhands of the 'animation' shorthand, form a coordinating list property group with 'animation-name' as the
 * coordinating list base property and each item in the coordinated value list defining the properties of a single animation effect." and "If multiple animations are attempting
 * to modify the same property, then the animation closest to the end of the list of names wins."
 */

#include <string>

#include <gtest/gtest.h>

#include "css_compute_test_support.h"
#include "markup_test_support.h"

using namespace cmp;

namespace {

struct Effects {
  Context ctx{R()};
  Block block;
  Style style;
  GTEXT_CSS_Anim_List * list = nullptr;
  GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;
  explicit Effects(const std::string & decls) : block(R(), decls) {
    style.compute(ctx, &block);
    status = gtext_css_anim_list_from_style(style.s, nullptr, &list);
  }
  Effects(const Effects &) = delete;
  Effects & operator=(const Effects &) = delete;
  ~Effects() {
    gtext_css_anim_list_free(list);
  }
  size_t count() const {
    return gtext_css_anim_list_count(list);
  }
  GTEXT_CSS_Anim_Effect at(size_t i) const {
    GTEXT_CSS_Anim_Effect e;
    std::memset(&e, 0, sizeof e);
    EXPECT_TRUE(gtext_css_anim_list_at(list, i, &e)) << i;
    return e;
  }
};

std::string name_of(const GTEXT_CSS_Anim_Effect & e) {
  return std::string(e.name.data, e.name.length);
}

/* Animations 1, animation-name: "none ... there will be no animation"; the initial value of every other longhand is the one of its table. */
TEST(CssAnimationEffects, AnElementThatNamesNoAnimationHasNoEffects) {
  Effects e("color: red");
  ASSERT_EQ(e.status, GTEXT_CSS_OK);
  EXPECT_EQ(e.count(), 0u);
  Effects none("animation-name: none; animation-duration: 3s");
  ASSERT_EQ(none.status, GTEXT_CSS_OK);
  EXPECT_EQ(none.count(), 0u);
}

TEST(CssAnimationEffects, TheShorthandSetsEveryFieldOfOneEffect) {
  Effects e("animation: a 10s linear -5s 2.5 alternate-reverse both paused");
  ASSERT_EQ(e.status, GTEXT_CSS_OK);
  ASSERT_EQ(e.count(), 1u);
  const GTEXT_CSS_Anim_Effect x = e.at(0);
  EXPECT_EQ(x.index, 0u);
  EXPECT_EQ(name_of(x), "a");
  EXPECT_FALSE(x.name_is_string);
  EXPECT_FALSE(x.duration_auto);
  EXPECT_DOUBLE_EQ(x.duration_ms, 10000.0);
  ASSERT_NE(x.easing, nullptr);
  EXPECT_EQ(x.easing->kind, GTEXT_CSS_EASING_LINEAR);
  EXPECT_DOUBLE_EQ(x.delay_start_ms, -5000.0);
  EXPECT_DOUBLE_EQ(x.delay_end_ms, 0.0);
  EXPECT_FALSE(x.infinite);
  EXPECT_DOUBLE_EQ(x.iteration_count, 2.5);
  EXPECT_EQ(x.direction, GTEXT_CSS_ANIM_DIRECTION_ALTERNATE_REVERSE);
  EXPECT_EQ(x.fill, GTEXT_CSS_ANIM_FILL_BOTH);
  EXPECT_EQ(x.composite, GTEXT_CSS_ANIM_COMPOSITE_REPLACE);
  EXPECT_TRUE(x.paused);
  EXPECT_TRUE(x.timeline_auto);
}

TEST(CssAnimationEffects, TheInitialValuesAreTheOnesOfTheLonghands) {
  Effects e("animation-name: a");
  ASSERT_EQ(e.status, GTEXT_CSS_OK);
  ASSERT_EQ(e.count(), 1u);
  const GTEXT_CSS_Anim_Effect x = e.at(0);
  EXPECT_TRUE(x.duration_auto) << "animation-duration: auto is the initial value";
  EXPECT_DOUBLE_EQ(x.duration_ms, 0.0);
  ASSERT_NE(x.easing, nullptr);
  EXPECT_EQ(x.easing->keyword, GTEXT_CSS_EASING_KW_EASE);
  EXPECT_DOUBLE_EQ(x.delay_start_ms, 0.0);
  EXPECT_DOUBLE_EQ(x.delay_end_ms, 0.0);
  EXPECT_FALSE(x.infinite);
  EXPECT_DOUBLE_EQ(x.iteration_count, 1.0);
  EXPECT_EQ(x.direction, GTEXT_CSS_ANIM_DIRECTION_NORMAL);
  EXPECT_EQ(x.fill, GTEXT_CSS_ANIM_FILL_NONE);
  EXPECT_EQ(x.composite, GTEXT_CSS_ANIM_COMPOSITE_REPLACE);
  EXPECT_FALSE(x.paused);
  EXPECT_TRUE(x.timeline_auto);
}

/* Values 4, Linked Properties: a coordinating list property that has too few values is repeated, one that has too many is truncated. */
TEST(CssAnimationEffects, AShorterListIsRepeatedAndALongerOneIsTruncated) {
  Effects e("animation-name: a, b, c; animation-duration: 1s, 2s; animation-iteration-count: infinite");
  ASSERT_EQ(e.status, GTEXT_CSS_OK);
  ASSERT_EQ(e.count(), 3u);
  EXPECT_DOUBLE_EQ(e.at(0).duration_ms, 1000.0);
  EXPECT_DOUBLE_EQ(e.at(1).duration_ms, 2000.0);
  EXPECT_DOUBLE_EQ(e.at(2).duration_ms, 1000.0) << "repeated";
  for (size_t i = 0; i < 3; i++) {
    EXPECT_TRUE(e.at(i).infinite);
    EXPECT_DOUBLE_EQ(e.at(i).iteration_count, 0.0);
  }
  Effects t("animation-name: a; animation-duration: 1s, 2s, 3s");
  ASSERT_EQ(t.status, GTEXT_CSS_OK);
  ASSERT_EQ(t.count(), 1u);
  EXPECT_DOUBLE_EQ(t.at(0).duration_ms, 1000.0) << "truncated";
}

TEST(CssAnimationEffects, ANoneKeepsItsPlaceInTheListAndIsNotAnEffect) {
  Effects e("animation-name: a, none, b; animation-duration: 1s, 2s, 3s; animation-fill-mode: forwards, none, backwards");
  ASSERT_EQ(e.status, GTEXT_CSS_OK);
  ASSERT_EQ(e.count(), 2u);
  EXPECT_EQ(e.at(0).index, 0u);
  EXPECT_EQ(name_of(e.at(0)), "a");
  EXPECT_DOUBLE_EQ(e.at(0).duration_ms, 1000.0);
  EXPECT_EQ(e.at(0).fill, GTEXT_CSS_ANIM_FILL_FORWARDS);
  EXPECT_EQ(e.at(1).index, 2u);
  EXPECT_EQ(name_of(e.at(1)), "b");
  EXPECT_DOUBLE_EQ(e.at(1).duration_ms, 3000.0) << "the third item of each list belongs to b, not the second";
  EXPECT_EQ(e.at(1).fill, GTEXT_CSS_ANIM_FILL_BACKWARDS);
}

/* Animations 1: "<keyframes-name> = <custom-ident> | <string>" */
TEST(CssAnimationEffects, AStringIsAName) {
  Effects e("animation-name: \"b c\", d");
  ASSERT_EQ(e.status, GTEXT_CSS_OK);
  ASSERT_EQ(e.count(), 2u);
  EXPECT_EQ(name_of(e.at(0)), "b c");
  EXPECT_TRUE(e.at(0).name_is_string);
  EXPECT_FALSE(e.at(1).name_is_string);
}

/* Animations 2: animation-composition "list, each item a keyword as specified"; animation-delay-start and animation-delay-end are the two halves of animation-delay. */
TEST(CssAnimationEffects, TheCompositionAndBothDelaysAreRead) {
  Effects e("animation-name: a, b, c; animation-composition: add, accumulate, replace; animation-delay-start: 100ms, -2s; animation-delay-end: 1s");
  ASSERT_EQ(e.status, GTEXT_CSS_OK);
  ASSERT_EQ(e.count(), 3u);
  EXPECT_EQ(e.at(0).composite, GTEXT_CSS_ANIM_COMPOSITE_ADD);
  EXPECT_EQ(e.at(1).composite, GTEXT_CSS_ANIM_COMPOSITE_ACCUMULATE);
  EXPECT_EQ(e.at(2).composite, GTEXT_CSS_ANIM_COMPOSITE_REPLACE);
  EXPECT_DOUBLE_EQ(e.at(0).delay_start_ms, 100.0);
  EXPECT_DOUBLE_EQ(e.at(1).delay_start_ms, -2000.0);
  EXPECT_DOUBLE_EQ(e.at(2).delay_start_ms, 100.0);
  for (size_t i = 0; i < 3; i++) {
    EXPECT_DOUBLE_EQ(e.at(i).delay_end_ms, 1000.0);
  }
}

TEST(CssAnimationEffects, EachTimingFunctionIsTypedAndOneOfTheList) {
  Effects e("animation-name: a, b, c; animation-timing-function: steps(3, jump-none), cubic-bezier(0.1, 0.2, 0.3, 0.4), linear(0, 1)");
  ASSERT_EQ(e.status, GTEXT_CSS_OK);
  ASSERT_EQ(e.count(), 3u);
  ASSERT_NE(e.at(0).easing, nullptr);
  EXPECT_EQ(e.at(0).easing->kind, GTEXT_CSS_EASING_STEPS);
  EXPECT_EQ(e.at(0).easing->steps, 3);
  EXPECT_EQ(e.at(0).easing->position, GTEXT_CSS_STEP_JUMP_NONE);
  EXPECT_EQ(e.at(1).easing->kind, GTEXT_CSS_EASING_CUBIC_BEZIER);
  EXPECT_DOUBLE_EQ(e.at(1).easing->x2, 0.3);
  EXPECT_EQ(e.at(2).easing->kind, GTEXT_CSS_EASING_LINEAR_POINTS);
}

/* Animations 1, animation-timeline (Animations 2): only the document timeline is sampled; a named, scroll or view timeline is not. */
TEST(CssAnimationEffects, OnlyTheAutoTimelineIsTheDocumentTimeline) {
  Effects e("animation-name: a, b, c; animation-timeline: auto, none, scroll()");
  ASSERT_EQ(e.status, GTEXT_CSS_OK);
  ASSERT_EQ(e.count(), 3u);
  EXPECT_TRUE(e.at(0).timeline_auto);
  EXPECT_FALSE(e.at(1).timeline_auto);
  EXPECT_FALSE(e.at(2).timeline_auto);
}

TEST(CssAnimationEffects, AListWithNoneOfTheKnownLonghandsIsRefusedNotGuessed) {
  GTEXT_CSS_Anim_List * list = nullptr;
  EXPECT_EQ(gtext_css_anim_list_from_style(nullptr, nullptr, &list), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(list, nullptr);
  Effects e("color: red");
  EXPECT_EQ(gtext_css_anim_list_from_style(e.style.s, nullptr, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_FALSE(gtext_css_anim_list_at(e.list, 0, nullptr));
  GTEXT_CSS_Anim_Effect x;
  EXPECT_FALSE(gtext_css_anim_list_at(e.list, 0, &x)) << "past the end";
  EXPECT_EQ(gtext_css_anim_list_count(nullptr), 0u);
  gtext_css_anim_list_free(nullptr);
}

/* A value that is a calculation is computed before it is read (the computed layer), so a delay of calc() is a number of seconds. */
TEST(CssAnimationEffects, AComputedCalculationIsAnOrdinaryTime) {
  Effects e("animation-name: a; animation-duration: calc(1s + 500ms); animation-delay-start: calc(-1 * 2s)");
  ASSERT_EQ(e.status, GTEXT_CSS_OK);
  ASSERT_EQ(e.count(), 1u);
  EXPECT_DOUBLE_EQ(e.at(0).duration_ms, 1500.0);
  EXPECT_DOUBLE_EQ(e.at(0).delay_start_ms, -2000.0);
}

/* Every allocation of the list goes through the allocator that was given, and a failure leaves nothing behind. */
TEST(CssAnimationEffectsOom, EveryAllocationOfAListIsSweptAndNothingIsLeft) {
  Context ctx(R());
  Block b(R(), "animation: a 1s, \"b c\" 2s steps(3), d 3s linear(0, 1) infinite; animation-composition: add");
  Style s;
  s.compute(ctx, &b);
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  size_t failures = 0;
  for (size_t n = 1; n < 200; n++) {
    FaultAllocator fa;
    GTEXT_CSS_Anim_List * list = nullptr;
    fa.arm(n);
    const GTEXT_CSS_Status st = gtext_css_anim_list_from_style(s.s, &fa.alloc, &list);
    fa.disarm();
    if (!fa.failed) {
      EXPECT_EQ(st, GTEXT_CSS_OK);
      EXPECT_EQ(gtext_css_anim_list_count(list), 3u);
      gtext_css_anim_list_free(list);
      EXPECT_TRUE(fa.live.empty());
      EXPECT_EQ(fa.bad_frees, 0u);
      break;
    }
    failures++;
    EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
    EXPECT_EQ(list, nullptr) << n;
    EXPECT_TRUE(fa.live.empty()) << n << ": a block is left";
    EXPECT_EQ(fa.bad_frees, 0u) << n;
  }
  EXPECT_GT(failures, 3u) << "the sweep reached the allocations";
}

}  // namespace
