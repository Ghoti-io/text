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
 * The reference count of the rule index (css_sheet.h): an index has one owner when it is made, gtext_css_rule_index_retain() adds one atomically, gtext_css_rule_index_free() releases one,
 * and the index, with the sheets it retains, goes with the last. This is what lets a thread compute styles against index N while the writer builds N + 1.
 */

#include <cstdlib>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_cascade_test_support.h"
#include "css_test_support.h"
#include "markup_test_support.h"

namespace {

struct Built {
  GTEXT_CSS_Rule_Index * index = nullptr;
  Built(FaultAllocator & fa, const char * css) {
    css_test::Parsed p;
    css_test::parse_into(p, gtext_css_parse_stylesheet, css);
    EXPECT_EQ(p.status, GTEXT_CSS_OK);
    GTEXT_CSS_Sheet_Options so = gtext_css_sheet_options_default();
    so.allocator = &fa.alloc;
    GTEXT_CSS_Sheet * sheet = nullptr;
    EXPECT_EQ(gtext_css_sheet_compile(p.tree, cas::R(), &so, &sheet, nullptr), GTEXT_CSS_OK);
    GTEXT_CSS_Rule_Index_Options io = gtext_css_rule_index_options_default();
    io.allocator = &fa.alloc;
    EXPECT_EQ(gtext_css_rule_index_new(cas::R(), &io, &index), GTEXT_CSS_OK);
    GTEXT_CSS_Sheet_Registration reg;
    std::memset(&reg, 0, sizeof reg);
    reg.origin = GTEXT_CSS_ORIGIN_AUTHOR;
    EXPECT_EQ(gtext_css_rule_index_add_sheet(index, sheet, &reg, nullptr), GTEXT_CSS_OK);
    gtext_css_sheet_free(sheet);   // the index retained it
    EXPECT_EQ(gtext_css_rule_index_freeze(index, nullptr), GTEXT_CSS_OK);
  }
};

TEST(CssRuleIndexRetain, TheMemoryGoesWithTheLastOwner) {
  FaultAllocator fa;
  Built b(fa, "a{color:red} .b{margin:1px} c{width:1px}");
  ASSERT_NE(b.index, nullptr);
  const size_t records = gtext_css_rule_index_record_count(b.index);
  EXPECT_GT(records, 0u);
  GTEXT_CSS_Rule_Index * second = gtext_css_rule_index_retain(b.index);
  EXPECT_EQ(second, b.index);
  GTEXT_CSS_Rule_Index * third = gtext_css_rule_index_retain(b.index);
  gtext_css_rule_index_free(b.index);   // one owner of three
  EXPECT_FALSE(fa.live.empty());
  EXPECT_EQ(gtext_css_rule_index_record_count(second), records) << "still readable";
  gtext_css_rule_index_free(second);
  EXPECT_FALSE(fa.live.empty());
  EXPECT_EQ(gtext_css_rule_index_sheet_count(third), 1u);
  gtext_css_rule_index_free(third);   // the last
  EXPECT_TRUE(fa.live.empty()) << "the index and the sheet it retained are gone";
  EXPECT_EQ(fa.bad_frees, 0u);
}

TEST(CssRuleIndexRetain, NullIsIgnored) {
  EXPECT_EQ(gtext_css_rule_index_retain(nullptr), nullptr);
  gtext_css_rule_index_free(nullptr);
}

TEST(CssRuleIndexRetain, AnIndexThatWasNeverRetainedIsFreedByTheFirstFree) {
  FaultAllocator fa;
  {
    GTEXT_CSS_Rule_Index_Options io = gtext_css_rule_index_options_default();
    io.allocator = &fa.alloc;
    GTEXT_CSS_Rule_Index * x = nullptr;
    ASSERT_EQ(gtext_css_rule_index_new(cas::R(), &io, &x), GTEXT_CSS_OK);
    gtext_css_rule_index_free(x);
  }
  EXPECT_TRUE(fa.live.empty());
}

// The count is atomic: threads that retain, read and release a frozen index leave it with the owners it had, and the last release frees it once. The writer lets go of its own
// while the readers still hold theirs, which is the case the retain exists for.
TEST(CssRuleIndexRetain, TheCountIsAtomicAndAReaderKeepsTheIndexAfterTheWriterLetsGo) {
  FaultAllocator fa;
  Built b(fa, "a{color:red} .b{margin:1px}");
  GTEXT_CSS_Rule_Index * index = b.index;
  const size_t records = gtext_css_rule_index_record_count(index);
  std::vector<GTEXT_CSS_Rule_Index *> held;
  for (int t = 0; t < 8; t++) {
    held.push_back(gtext_css_rule_index_retain(index));
  }
  gtext_css_rule_index_free(index);   // the writer's own
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([x = held[t], records]() {
      for (int i = 0; i < 20000; i++) {
        GTEXT_CSS_Rule_Index * mine = gtext_css_rule_index_retain(x);
        if (gtext_css_rule_index_record_count(mine) != records) {
          std::abort();
        }
        gtext_css_rule_index_free(mine);
      }
      gtext_css_rule_index_free(x);
    });
  }
  for (std::thread & t : threads) {
    t.join();
  }
  EXPECT_TRUE(fa.live.empty());
  EXPECT_EQ(fa.bad_frees, 0u);
}

} // namespace
