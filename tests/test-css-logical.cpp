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
 * Logical properties (css_logical.h): the abstract-to-physical table of Writing Modes 4, 6.4, in every writing mode and direction; the 128
 * properties of the pin in their 20 groups, each understood; the map of every member in every flow; and the sharers of a slot.
 */

#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_compute_test_support.h"

using namespace cmp;

namespace {

GTEXT_CSS_Flow flow(GTEXT_CSS_Writing_Mode wm, GTEXT_CSS_Direction d = GTEXT_CSS_DIRECTION_LTR,
    GTEXT_CSS_Text_Orientation to = GTEXT_CSS_TEXT_ORIENTATION_MIXED) {
  GTEXT_CSS_Flow f;
  f.writing_mode = wm;
  f.direction = d;
  f.text_orientation = to;
  return f;
}

std::string phys(const std::string & property, const GTEXT_CSS_Flow & f) {
  GTEXT_CSS_Property_Id out = 0;
  if (gtext_css_logical_map(R(), find(R(), property), &f, &out) != GTEXT_CSS_OK) {
    return "<error>";
  }
  GTEXT_CSS_Property_Info i;
  std::memset(&i, 0, sizeof i);
  gtext_css_registry_property_info(R(), out, &i);
  return i.name;
}

TEST(CssLogical, TheTableOfWritingModesMapsEveryFlowRelativeSideToAPhysicalOne) {
  struct Row {
    GTEXT_CSS_Writing_Mode wm;
    const char * block_start;
    const char * block_end;
    const char * inline_start_ltr;
    const char * inline_start_rtl;
  };
  const Row rows[] = {
    {GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB, "top", "bottom", "left", "right"},
    {GTEXT_CSS_WRITING_MODE_VERTICAL_RL, "right", "left", "top", "bottom"},
    {GTEXT_CSS_WRITING_MODE_SIDEWAYS_RL, "right", "left", "top", "bottom"},
    {GTEXT_CSS_WRITING_MODE_VERTICAL_LR, "left", "right", "top", "bottom"},
    {GTEXT_CSS_WRITING_MODE_SIDEWAYS_LR, "left", "right", "bottom", "top"},
  };
  auto opposite = [](const std::string & s) -> std::string {
    return s == "top" ? "bottom" : s == "bottom" ? "top" : s == "left" ? "right" : "left";
  };
  for (const Row & r : rows) {
    const GTEXT_CSS_Flow ltr = flow(r.wm, GTEXT_CSS_DIRECTION_LTR);
    const GTEXT_CSS_Flow rtl = flow(r.wm, GTEXT_CSS_DIRECTION_RTL);
    const std::string mode = gtext_css_writing_mode_name(r.wm);
    EXPECT_EQ(phys("margin-block-start", ltr), std::string("margin-") + r.block_start) << mode;
    EXPECT_EQ(phys("margin-block-end", ltr), std::string("margin-") + r.block_end) << mode;
    EXPECT_EQ(phys("margin-block-start", rtl), std::string("margin-") + r.block_start) << mode << ": the block side does not depend on the direction";
    EXPECT_EQ(phys("margin-inline-start", ltr), std::string("margin-") + r.inline_start_ltr) << mode << " ltr";
    EXPECT_EQ(phys("margin-inline-end", ltr), std::string("margin-") + opposite(r.inline_start_ltr)) << mode << " ltr";
    EXPECT_EQ(phys("margin-inline-start", rtl), std::string("margin-") + r.inline_start_rtl) << mode << " rtl";
    EXPECT_EQ(phys("margin-inline-end", rtl), std::string("margin-") + opposite(r.inline_start_rtl)) << mode << " rtl";
  }
}

TEST(CssLogical, SizesAreHeightAndWidthByTheBlockAxis) {
  EXPECT_EQ(phys("block-size", flow(GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB)), "height");
  EXPECT_EQ(phys("inline-size", flow(GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB)), "width");
  for (auto wm : {GTEXT_CSS_WRITING_MODE_VERTICAL_RL, GTEXT_CSS_WRITING_MODE_VERTICAL_LR, GTEXT_CSS_WRITING_MODE_SIDEWAYS_RL, GTEXT_CSS_WRITING_MODE_SIDEWAYS_LR}) {
    EXPECT_EQ(phys("block-size", flow(wm)), "width") << gtext_css_writing_mode_name(wm);
    EXPECT_EQ(phys("inline-size", flow(wm)), "height") << gtext_css_writing_mode_name(wm);
    EXPECT_EQ(phys("max-inline-size", flow(wm)), "max-height");
    EXPECT_EQ(phys("min-block-size", flow(wm)), "min-width");
  }
  EXPECT_EQ(phys("overflow-block", flow(GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB)), "overflow-y");
  EXPECT_EQ(phys("overflow-inline", flow(GTEXT_CSS_WRITING_MODE_VERTICAL_RL)), "overflow-y");
  EXPECT_EQ(phys("contain-intrinsic-inline-size", flow(GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB)), "contain-intrinsic-width");
  EXPECT_EQ(phys("background-position-block", flow(GTEXT_CSS_WRITING_MODE_VERTICAL_LR)), "background-position-x");
  EXPECT_EQ(phys("overscroll-behavior-inline", flow(GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB)), "overscroll-behavior-x");
}

TEST(CssLogical, ACornerIsNamedByItsBlockSideAndItsInlineSide) {
  // start-start is the block-start, inline-start corner: top left in horizontal-tb ltr, top right in rtl
  EXPECT_EQ(phys("border-start-start-radius", flow(GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB)), "border-top-left-radius");
  EXPECT_EQ(phys("border-start-start-radius", flow(GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB, GTEXT_CSS_DIRECTION_RTL)), "border-top-right-radius");
  EXPECT_EQ(phys("border-end-end-radius", flow(GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB)), "border-bottom-right-radius");
  EXPECT_EQ(phys("border-start-end-radius", flow(GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB)), "border-top-right-radius");
  EXPECT_EQ(phys("border-end-start-radius", flow(GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB)), "border-bottom-left-radius");
  // vertical-rl ltr: block-start is right, inline-start is top: start-start is the top right corner
  EXPECT_EQ(phys("border-start-start-radius", flow(GTEXT_CSS_WRITING_MODE_VERTICAL_RL)), "border-top-right-radius");
  EXPECT_EQ(phys("border-end-start-radius", flow(GTEXT_CSS_WRITING_MODE_VERTICAL_RL)), "border-top-left-radius");
  // sideways-lr ltr: block-start is left, inline-start is bottom
  EXPECT_EQ(phys("border-start-start-radius", flow(GTEXT_CSS_WRITING_MODE_SIDEWAYS_LR)), "border-bottom-left-radius");
  EXPECT_EQ(phys("corner-end-end-shape", flow(GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB)), "corner-bottom-right-shape");
}

TEST(CssLogical, UprightForcesTheUsedDirectionToLtrInAVerticalModeOnly) {
  const GTEXT_CSS_Flow rtl_upright_v = flow(GTEXT_CSS_WRITING_MODE_VERTICAL_RL, GTEXT_CSS_DIRECTION_RTL, GTEXT_CSS_TEXT_ORIENTATION_UPRIGHT);
  EXPECT_EQ(gtext_css_flow_used_direction(&rtl_upright_v), GTEXT_CSS_DIRECTION_LTR);
  EXPECT_EQ(phys("margin-inline-start", rtl_upright_v), "margin-top") << "the used direction is ltr: inline-start is the top";
  const GTEXT_CSS_Flow rtl_mixed_v = flow(GTEXT_CSS_WRITING_MODE_VERTICAL_RL, GTEXT_CSS_DIRECTION_RTL, GTEXT_CSS_TEXT_ORIENTATION_MIXED);
  EXPECT_EQ(gtext_css_flow_used_direction(&rtl_mixed_v), GTEXT_CSS_DIRECTION_RTL);
  EXPECT_EQ(phys("margin-inline-start", rtl_mixed_v), "margin-bottom");
  const GTEXT_CSS_Flow rtl_upright_h = flow(GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB, GTEXT_CSS_DIRECTION_RTL, GTEXT_CSS_TEXT_ORIENTATION_UPRIGHT);
  EXPECT_EQ(gtext_css_flow_used_direction(&rtl_upright_h), GTEXT_CSS_DIRECTION_RTL) << "text-orientation has no effect in a horizontal mode";
  const GTEXT_CSS_Flow rtl_upright_s = flow(GTEXT_CSS_WRITING_MODE_SIDEWAYS_RL, GTEXT_CSS_DIRECTION_RTL, GTEXT_CSS_TEXT_ORIENTATION_UPRIGHT);
  EXPECT_EQ(gtext_css_flow_used_direction(&rtl_upright_s), GTEXT_CSS_DIRECTION_RTL) << "a sideways mode typesets horizontally";
  EXPECT_EQ(gtext_css_flow_used_direction(nullptr), GTEXT_CSS_DIRECTION_LTR);
}

TEST(CssLogical, ThePinHas128PropertiesIn20GroupsAndEveryOneIsUnderstood) {
  size_t members = 0;
  std::set<std::string> groups;
  const size_t n = gtext_css_registry_property_count(R());
  for (size_t id = 1; id <= n; id++) {
    GTEXT_CSS_Property_Info info;
    ASSERT_TRUE(gtext_css_registry_property_info(R(), static_cast<GTEXT_CSS_Property_Id>(id), &info));
    if (!info.logical_group) {
      continue;
    }
    members++;
    groups.insert(info.logical_group);
    GTEXT_CSS_Logical_Member m;
    ASSERT_EQ(gtext_css_logical_member(R(), static_cast<GTEXT_CSS_Property_Id>(id), &m), GTEXT_CSS_OK) << info.name;
    EXPECT_NE(m.kind, GTEXT_CSS_LOGICAL_NONE) << info.name;
    EXPECT_STREQ(m.group, info.logical_group);
  }
  EXPECT_EQ(members, 128u);
  EXPECT_EQ(groups.size(), 20u);
}

TEST(CssLogical, EveryMemberOfEveryGroupMapsToAPhysicalOneInEveryFlowAndTheSlotsAreCovered) {
  const GTEXT_CSS_Writing_Mode modes[] = {GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB, GTEXT_CSS_WRITING_MODE_VERTICAL_RL,
      GTEXT_CSS_WRITING_MODE_VERTICAL_LR, GTEXT_CSS_WRITING_MODE_SIDEWAYS_RL, GTEXT_CSS_WRITING_MODE_SIDEWAYS_LR};
  const size_t n = gtext_css_registry_property_count(R());
  for (auto wm : modes) {
    for (auto d : {GTEXT_CSS_DIRECTION_LTR, GTEXT_CSS_DIRECTION_RTL}) {
      for (auto to : {GTEXT_CSS_TEXT_ORIENTATION_MIXED, GTEXT_CSS_TEXT_ORIENTATION_UPRIGHT}) {
        const GTEXT_CSS_Flow f = flow(wm, d, to);
        std::map<std::string, std::set<size_t>> slots_seen;
        std::map<std::string, size_t> expected;
        for (size_t id = 1; id <= n; id++) {
          GTEXT_CSS_Property_Info info;
          gtext_css_registry_property_info(R(), static_cast<GTEXT_CSS_Property_Id>(id), &info);
          if (!info.logical_group) {
            continue;
          }
          GTEXT_CSS_Property_Id p = 0;
          ASSERT_EQ(gtext_css_logical_map(R(), static_cast<GTEXT_CSS_Property_Id>(id), &f, &p), GTEXT_CSS_OK) << info.name;
          GTEXT_CSS_Logical_Member pm;
          ASSERT_EQ(gtext_css_logical_member(R(), p, &pm), GTEXT_CSS_OK);
          EXPECT_FALSE(pm.logical) << info.name << " maps to " << p << ", which is not physical";
          GTEXT_CSS_Logical_Kind kind;
          size_t slot = 99;
          ASSERT_EQ(gtext_css_logical_slot(R(), static_cast<GTEXT_CSS_Property_Id>(id), &f, &kind, &slot), GTEXT_CSS_OK);
          size_t pslot = 98;
          ASSERT_EQ(gtext_css_logical_slot(R(), p, &f, nullptr, &pslot), GTEXT_CSS_OK);
          EXPECT_EQ(slot, pslot) << info.name;
          slots_seen[info.logical_group].insert(slot);
          expected[info.logical_group] = kind == GTEXT_CSS_LOGICAL_AXES ? 2 : 4;
        }
        for (auto & kv : slots_seen) {
          EXPECT_EQ(kv.second.size(), expected[kv.first]) << kv.first << " in " << gtext_css_writing_mode_name(wm);
        }
      }
    }
  }
}

TEST(CssLogical, ThePropertiesThatShareASlotAreThePhysicalOneAndTheFlowRelativeOnesThatMapToIt) {
  const GTEXT_CSS_Flow f = flow(GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB);
  std::set<std::string> names;
  size_t count = 0;
  ASSERT_EQ(gtext_css_logical_sharers(R(), find(R(), "margin-left"), &f, 0, &count, nullptr), GTEXT_CSS_OK);
  for (size_t i = 0; i < count; i++) {
    GTEXT_CSS_Property_Id p = 0;
    ASSERT_EQ(gtext_css_logical_sharers(R(), find(R(), "margin-left"), &f, i, nullptr, &p), GTEXT_CSS_OK);
    GTEXT_CSS_Property_Info info;
    gtext_css_registry_property_info(R(), p, &info);
    names.insert(info.name);
  }
  EXPECT_EQ(names, (std::set<std::string>{"margin-left", "margin-inline-start"}));
  const GTEXT_CSS_Flow rtl = flow(GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB, GTEXT_CSS_DIRECTION_RTL);
  ASSERT_EQ(gtext_css_logical_sharers(R(), find(R(), "margin-left"), &rtl, 0, &count, nullptr), GTEXT_CSS_OK);
  names.clear();
  for (size_t i = 0; i < count; i++) {
    GTEXT_CSS_Property_Id p = 0;
    gtext_css_logical_sharers(R(), find(R(), "margin-left"), &rtl, i, nullptr, &p);
    GTEXT_CSS_Property_Info info;
    gtext_css_registry_property_info(R(), p, &info);
    names.insert(info.name);
  }
  EXPECT_EQ(names, (std::set<std::string>{"margin-left", "margin-inline-end"}));
  // an index past the count gives none
  GTEXT_CSS_Property_Id p = 7;
  ASSERT_EQ(gtext_css_logical_sharers(R(), find(R(), "margin-left"), &rtl, 5, nullptr, &p), GTEXT_CSS_OK);
  EXPECT_EQ(p, GTEXT_CSS_PROPERTY_NONE);
}

TEST(CssLogical, APropertyInNoGroupIsRefusedAndAnAliasIsItsTarget) {
  const GTEXT_CSS_Flow f = flow(GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB);
  GTEXT_CSS_Property_Id p = 0;
  EXPECT_EQ(gtext_css_logical_map(R(), find(R(), "color"), &f, &p), GTEXT_CSS_E_INVALID);
  GTEXT_CSS_Logical_Member m;
  EXPECT_EQ(gtext_css_logical_member(R(), find(R(), "color"), &m), GTEXT_CSS_OK);
  EXPECT_EQ(m.kind, GTEXT_CSS_LOGICAL_NONE);
  EXPECT_EQ(m.group, nullptr);
  EXPECT_EQ(gtext_css_logical_map(R(), 0, &f, &p), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_logical_map(nullptr, find(R(), "margin-left"), &f, &p), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_logical_map(R(), find(R(), "margin-left"), nullptr, &p), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(phys("margin-left", f), "margin-left") << "a physical member maps to itself";
}

} // namespace
