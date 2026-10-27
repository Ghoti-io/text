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
 * The persistent map of custom properties (src/css/css_custom.h): lookup, the order of the names, that a map is never changed by a map made from it,
 * that a hidden name is hidden and not removed, the sharing that makes a long chain cost what it holds, and allocation failure.
 */

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "../src/css/css_custom.h"
#include "css_compute_test_support.h"
#include "markup_test_support.h"

using namespace cmp;

namespace {

/// A value set for a text, from the property layer (any typed value will do).
GTEXT_CSS_Value_Set * value_of(const std::string & text) {
  GTEXT_CSS_Property_Value pv;
  if (gtext_css_parse_property_text(R(), "--x", 3, text.data(), text.size(), nullptr, &pv) != GTEXT_CSS_OK || pv.outcome != GTEXT_CSS_PROP_RESULT_VALID) {
    return nullptr;
  }
  GTEXT_CSS_Value_Set * set = gtext_css_value_set_retain(pv.set);
  gtext_css_property_value_free(&pv);
  return set;
}

struct Map {
  css_cmap * m = nullptr;
  Map() = default;
  explicit Map(css_cmap * made) : m(made) {}
  Map(const Map &) = delete;
  Map & operator=(const Map &) = delete;
  ~Map() {
    css_cmap_release(m);
  }
};

css_cmap * set(css_cmap * base, const std::string & name, const std::string & text, const GTEXT_Allocator * alloc = nullptr) {
  GTEXT_CSS_Value_Set * v = text.empty() ? nullptr : value_of(text);
  css_cmap * out = nullptr;
  EXPECT_EQ(css_cmap_set(alloc ? alloc : gtext_allocator_default(), base, name.data(), name.size(), v, 12345, &out, nullptr), GTEXT_CSS_OK);
  gtext_css_value_set_free(v);
  return out;
}

std::string text_of_value(const GTEXT_CSS_Value_Set * v) {
  if (!v) {
    return "<none>";
  }
  char * buf = nullptr;
  size_t n = 0;
  gtext_css_serialize_value_buffer(gtext_css_value_set_root(v), nullptr, &buf, &n);
  std::string s(buf, n);
  gtext_css_serialize_buffer_free(nullptr, buf);
  return s;
}

std::string find(css_cmap * m, const std::string & name) {
  return text_of_value(css_cmap_find(m, name.data(), name.size()));
}

std::vector<std::string> names(css_cmap * m) {
  std::vector<std::string> out;
  for (size_t i = 0; i < css_cmap_count(m); i++) {
    const char * n = nullptr;
    size_t l = 0;
    EXPECT_TRUE(css_cmap_at(m, i, &n, &l, nullptr));
    out.emplace_back(n, l);
  }
  return out;
}

TEST(CssCustomMap, TheEmptyMapHasNothingAndNullIsTheEmptyMap) {
  EXPECT_EQ(css_cmap_count(nullptr), 0u);
  EXPECT_EQ(css_cmap_find(nullptr, "--a", 3), nullptr);
  EXPECT_FALSE(css_cmap_at(nullptr, 0, nullptr, nullptr, nullptr));
  css_cmap_release(nullptr);
  EXPECT_EQ(css_cmap_retain(nullptr), nullptr);
}

TEST(CssCustomMap, NamesAreFoundAndComeInTheOrderOfTheirBytesThenTheirLength) {
  Map a(set(nullptr, "--b", "2"));
  Map b(set(a.m, "--a", "1"));
  Map c(set(b.m, "--ab", "12"));
  Map d(set(c.m, "--A", "upper"));
  EXPECT_EQ(find(d.m, "--a"), "1");
  EXPECT_EQ(find(d.m, "--ab"), "12");
  EXPECT_EQ(find(d.m, "--A"), "upper") << "case is kept";
  EXPECT_EQ(find(d.m, "--nope"), "<none>");
  EXPECT_EQ((names(d.m)), (std::vector<std::string>{"--A", "--a", "--ab", "--b"}));
  const char * n = nullptr;
  size_t l = 0;
  const GTEXT_CSS_Value_Set * v = nullptr;
  EXPECT_TRUE(css_cmap_at(d.m, 3, &n, &l, &v));
  EXPECT_EQ(std::string(n, l), "--b");
  EXPECT_EQ(text_of_value(v), "2");
  EXPECT_FALSE(css_cmap_at(d.m, 4, &n, &l, &v));
}

TEST(CssCustomMap, SettingANameDoesNotChangeTheMapItWasMadeFrom) {
  Map a(set(nullptr, "--x", "1"));
  Map b(set(a.m, "--x", "2"));
  Map c(set(b.m, "--y", "3"));
  EXPECT_EQ(find(a.m, "--x"), "1");
  EXPECT_EQ(find(b.m, "--x"), "2");
  EXPECT_EQ(find(c.m, "--x"), "2");
  EXPECT_EQ(css_cmap_count(a.m), 1u);
  EXPECT_EQ(css_cmap_count(b.m), 1u);
  EXPECT_EQ(css_cmap_count(c.m), 2u);
  EXPECT_EQ(find(a.m, "--y"), "<none>");
}

TEST(CssCustomMap, ANameSetToNothingIsHiddenAndTheMapBeforeStillHasIt) {
  Map a(set(nullptr, "--x", "1"));
  Map b(set(a.m, "--y", "2"));
  Map c(set(b.m, "--x", ""));
  EXPECT_EQ(find(c.m, "--x"), "<none>");
  EXPECT_EQ(css_cmap_count(c.m), 1u);
  EXPECT_EQ((names(c.m)), (std::vector<std::string>{"--y"}));
  EXPECT_EQ(find(b.m, "--x"), "1");
  EXPECT_EQ(css_cmap_count(b.m), 2u);
  Map d(set(c.m, "--x", "again"));
  EXPECT_EQ(find(d.m, "--x"), "again");
  EXPECT_EQ(css_cmap_count(d.m), 2u);
}

TEST(CssCustomMap, ManyNamesStayOrderedAndFindableAndTheTreeStaysShallow) {
  std::map<std::string, std::string> model;
  Map root;
  uint32_t seed = 12345;
  for (int i = 0; i < 3000; i++) {
    seed = seed * 1664525u + 1013904223u;
    const std::string name = "--n" + std::to_string(seed % 1500);
    const std::string value = std::to_string(i);
    css_cmap * next = set(root.m, name, value);
    css_cmap_release(root.m);
    root.m = next;
    model[name] = value;
  }
  EXPECT_EQ(css_cmap_count(root.m), model.size());
  std::vector<std::string> expected;
  for (const auto & kv : model) {
    expected.push_back(kv.first);
  }
  std::sort(expected.begin(), expected.end(), [](const std::string & a, const std::string & b) {
    const int c = std::memcmp(a.data(), b.data(), std::min(a.size(), b.size()));
    return c != 0 ? c < 0 : a.size() < b.size();
  });
  EXPECT_EQ(names(root.m), expected);
  for (const auto & kv : model) {
    EXPECT_EQ(find(root.m, kv.first), kv.second) << kv.first;
  }
}

TEST(CssCustomMap, TheShapeOfTheTreeDependsOnTheNamesAndNotOnTheOrderTheyWereSetIn) {
  // a treap is the one tree its names and priorities make: a rotation to the left that is wrong, or one that is missed, makes another tree for one order
  // of insertion, which the names found and their order do not show
  std::vector<std::string> all;
  for (int i = 0; i < 700; i++) {
    all.push_back("--k" + std::to_string((i * 7919) % 700));
  }
  std::vector<std::vector<std::string>> orders(4, all);
  std::sort(orders[0].begin(), orders[0].end());
  orders[1] = orders[0];
  std::reverse(orders[1].begin(), orders[1].end());
  uint32_t seed = 99;
  for (size_t i = orders[2].size(); i > 1; i--) {
    seed = seed * 1664525u + 1013904223u;
    std::swap(orders[2][i - 1], orders[2][seed % i]);
  }
  for (size_t i = 0; i < orders[3].size(); i++) {   // the middle outward
    orders[3][i] = orders[0][i % 2 ? orders[0].size() / 2 + i / 2 : orders[0].size() / 2 - i / 2 - 1];
  }
  uint64_t shape = 0;
  size_t height = 0;
  for (size_t o = 0; o < orders.size(); o++) {
    Map m;
    for (const std::string & name : orders[o]) {
      css_cmap * next = set(m.m, name, "1");
      css_cmap_release(m.m);
      m.m = next;
    }
    size_t h = 0;
    const uint64_t sh = css_cmap_shape(m.m, &h);
    EXPECT_EQ(css_cmap_count(m.m), 700u) << "order " << o;
    if (o == 0) {
      shape = sh;
      height = h;
    }
    EXPECT_EQ(sh, shape) << "order " << o;
    EXPECT_EQ(h, height) << "order " << o;
    EXPECT_LT(h, 40u) << "700 names, 2 log2 n is 20: order " << o;
    EXPECT_EQ(names(m.m).size(), 700u);
  }
}

TEST(CssCustomMap, ALongChainSharesWhatItInherits) {
  // 4000 maps, each made from the one before with one more name: a copy at every level would hold 8 million nodes; sharing holds about
  // 4000 times the nodes of one path
  FaultAllocator counting;
  Map keep;
  std::vector<css_cmap *> all;
  css_cmap * prev = nullptr;
  for (int i = 0; i < 4000; i++) {
    css_cmap * next = set(prev, "--p" + std::to_string(i), std::to_string(i), &counting.alloc);
    all.push_back(next);
    prev = next;
  }
  // blocks held: nodes and their names (and the value sets, through the default allocator, are not counted here)
  EXPECT_LT(counting.live.size(), (size_t)4000 * 2 * 40) << "the maps share their nodes";
  EXPECT_EQ(css_cmap_count(prev), 4000u);
  EXPECT_EQ(css_cmap_count(all[99]), 100u);
  for (css_cmap * m : all) {
    css_cmap_release(m);
  }
  EXPECT_TRUE(counting.live.empty()) << "every node is freed with the last map that holds it";
  EXPECT_EQ(counting.bad_frees, 0u);
}

TEST(CssCustomMap, RetainedMapsOutliveTheirOwner) {
  Map a(set(nullptr, "--x", "1"));
  css_cmap * extra = css_cmap_retain(a.m);
  css_cmap_release(a.m);
  a.m = nullptr;
  EXPECT_EQ(find(extra, "--x"), "1");
  css_cmap_release(extra);
}

TEST(CssCustomMap, AnAllocationThatFailsLeavesTheMapAsItWasAndLeaksNothing) {
  Map base(set(nullptr, "--q", "1"));
  Map more(set(base.m, "--m", "2"));
  for (size_t n = 1; n < 100; n++) {
    FaultAllocator fa;
    css_cmap * out = nullptr;
    GTEXT_CSS_Value_Set * v = value_of("v");
    fa.arm(n);
    const GTEXT_CSS_Status st = css_cmap_set(&fa.alloc, more.m, "--z", 3, v, 12345, &out, nullptr);
    fa.disarm();
    gtext_css_value_set_free(v);
    if (!fa.failed) {
      EXPECT_EQ(st, GTEXT_CSS_OK);
      EXPECT_EQ(find(out, "--z"), "v");
      css_cmap_release(out);
      break;
    }
    EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
    EXPECT_EQ(out, nullptr) << n;
    EXPECT_EQ(css_cmap_count(more.m), 2u);
    EXPECT_TRUE(fa.live.empty()) << n << ": a node is left";
    EXPECT_EQ(fa.bad_frees, 0u);
  }
}

} // namespace
