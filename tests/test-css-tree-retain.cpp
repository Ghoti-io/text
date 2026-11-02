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
 * The reference count of the syntax tree (css_core.h): a tree has one owner when a parse returns it, gtext_css_tree_retain() adds one atomically, gtext_css_tree_free() releases
 * one, and the memory goes with the last. A CSSOM rule holds the tree it was read from, so a tree is shared by the rules of one parse and outlives the sheet's text.
 */

#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "markup_test_support.h"

namespace {

GTEXT_CSS_Tree * parse(const GTEXT_Allocator * alloc, const char * text) {
  GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
  o.allocator = alloc;
  GTEXT_CSS_Tree * tree = nullptr;
  EXPECT_EQ(gtext_css_parse_stylesheet(text, std::strlen(text), &o, &tree, nullptr), GTEXT_CSS_OK);
  return tree;
}

TEST(CssTreeRetain, TheMemoryGoesWithTheLastOwner) {
  FaultAllocator fa;
  GTEXT_CSS_Tree * tree = parse(&fa.alloc, "a{color:red} @media print{b{margin:1px}}");
  ASSERT_NE(tree, nullptr);
  EXPECT_EQ(tree->rule_count, 2u);
  EXPECT_GT(gtext_css_tree_bytes(tree), 0u);
  GTEXT_CSS_Tree * second = gtext_css_tree_retain(tree);
  EXPECT_EQ(second, tree) << "the same tree";
  GTEXT_CSS_Tree * third = gtext_css_tree_retain(tree);
  const size_t bytes = gtext_css_tree_bytes(tree);
  gtext_css_tree_free(tree);   // one owner of three
  EXPECT_FALSE(fa.live.empty());
  EXPECT_EQ(second->rule_count, 2u) << "still readable";
  EXPECT_EQ(gtext_css_tree_bytes(second), bytes);
  gtext_css_tree_free(second);
  EXPECT_FALSE(fa.live.empty());
  EXPECT_EQ(third->rules[0].declaration_count, 1u);
  gtext_css_tree_free(third);   // the last
  EXPECT_TRUE(fa.live.empty()) << "nothing is left";
  EXPECT_EQ(fa.bad_frees, 0u);
}

TEST(CssTreeRetain, NullIsIgnoredAndEveryKindOfTreeCanBeShared) {
  EXPECT_EQ(gtext_css_tree_retain(nullptr), nullptr);
  EXPECT_EQ(gtext_css_tree_bytes(nullptr), 0u);
  gtext_css_tree_free(nullptr);
  FaultAllocator fa;
  GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
  o.allocator = &fa.alloc;
  GTEXT_CSS_Tree * trees[4] = {nullptr, nullptr, nullptr, nullptr};
  ASSERT_EQ(gtext_css_parse_declaration_list("a:b; c:d", 8, &o, &trees[0], nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_parse_rule("a{}", 3, &o, &trees[1], nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_parse_component_values("1px 2px", 7, &o, &trees[2], nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_parse_stylesheet("", 0, &o, &trees[3], nullptr), GTEXT_CSS_OK);
  for (GTEXT_CSS_Tree * t : trees) {
    ASSERT_NE(t, nullptr);
    GTEXT_CSS_Tree * again = gtext_css_tree_retain(t);
    gtext_css_tree_free(t);
    EXPECT_EQ(again->kind, t->kind);
    gtext_css_tree_free(again);
  }
  EXPECT_TRUE(fa.live.empty());
}

// The count is atomic: threads that retain and release a shared tree leave it with the owners it had, and the last release frees it once.
TEST(CssTreeRetain, TheCountIsAtomic) {
  FaultAllocator fa;
  GTEXT_CSS_Tree * tree = parse(&fa.alloc, "a{color:red}");
  ASSERT_NE(tree, nullptr);
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([tree]() {
      for (int i = 0; i < 20000; i++) {
        GTEXT_CSS_Tree * mine = gtext_css_tree_retain(tree);
        if (mine->rule_count != 1u) {
          std::abort();
        }
        gtext_css_tree_free(mine);
      }
    });
  }
  for (std::thread & t : threads) {
    t.join();
  }
  EXPECT_FALSE(fa.live.empty());
  EXPECT_EQ(tree->rule_count, 1u);
  gtext_css_tree_free(tree);
  EXPECT_TRUE(fa.live.empty());
}

} // namespace
