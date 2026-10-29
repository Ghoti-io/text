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
 * Cost shapes of the at-rule layer: a long counter style chain, a long cycle, many keyframe blocks, many unicode ranges and many rules in a set are handled in time linear (or n log n)
 * in their size. The check is a ratio of two runs, 4n against n, bounded well below the quadratic 16.
 */

#include <chrono>
#include <functional>
#include <string>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_cascade_test_support.h"
#include "css_test_support.h"

using cas::R;

namespace {

double seconds(const std::function<void()> & fn) {
  double best = 1e9;
  for (int i = 0; i < 3; i++) {
    const auto t0 = std::chrono::steady_clock::now();
    fn();
    best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  }
  return best;
}

// Time `make(n)` at n and 4n; linear work grows by about 4, quadratic by 16.
void expect_linear(const char * what, size_t n, const std::function<std::function<void()>(size_t)> & make) {
  const double small = seconds(make(n));
  const double big = seconds(make(4 * n));
  EXPECT_LT(big, std::max(small, 0.002) * 10.0) << what << ": " << small << " s at " << n << ", " << big << " s at " << 4 * n;
}

void build_set(const std::string & css, size_t expect_entries) {
  css_test::Parsed p;
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  po.max_rules = 10000000;
  po.max_tokens = 100000000;
  p.status = gtext_css_parse_stylesheet(css.data(), css.size(), &po, &p.tree, &p.result);
  ASSERT_NE(p.tree, nullptr);
  GTEXT_CSS_Atrule_Set_Options so = gtext_css_atrule_set_options_default();
  GTEXT_CSS_Atrule_Set * set = nullptr;
  ASSERT_EQ(gtext_css_atrule_set_new(R(), &so, &set), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_atrule_set_add_tree(set, p.tree), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_atrule_set_freeze(set), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_atrule_set_count(set), expect_entries);
  gtext_css_atrule_set_free(set);
}

}  // namespace

TEST(CssAtruleCost, CounterStyleChain) {
  expect_linear("a chain of extends", 2000, [](size_t n) {
    std::string css;
    for (size_t i = 0; i < n; i++) {
      css += "@counter-style s" + std::to_string(i) + " { system: extends s" + std::to_string(i + 1) + " }";
    }
    css += "@counter-style s" + std::to_string(n) + " { system: cyclic; symbols: a }";
    return [css, n] { build_set(css, n + 1); };
  });
}

TEST(CssAtruleCost, CounterStyleCycle) {
  expect_linear("a cycle of extends", 2000, [](size_t n) {
    std::string css;
    for (size_t i = 0; i < n; i++) {
      css += "@counter-style s" + std::to_string(i) + " { system: extends s" + std::to_string((i + 1) % n) + " }";
    }
    return [css, n] { build_set(css, n); };
  });
}

TEST(CssAtruleCost, SameNameManyRules) {
  expect_linear("many keyframes of one name", 2000, [](size_t n) {
    std::string css;
    for (size_t i = 0; i < n; i++) {
      css += "@keyframes k { from { color: red } }";
    }
    return [css, n] { build_set(css, n); };
  });
}

TEST(CssAtruleCost, KeyframeBlocks) {
  expect_linear("keyframe blocks of m selectors", 500, [](size_t n) {
    std::string css = "@keyframes k {";
    for (size_t i = 0; i < n; i++) {
      css += " " + std::to_string(i % 100) + "%, " + std::to_string((i * 7) % 100) + "% { color: red; width: " + std::to_string(i) + "px }";
    }
    css += " }";
    return [css] { build_set(css, 1); };
  });
}

TEST(CssAtruleCost, UnicodeRanges) {
  expect_linear("unicode ranges", 2000, [](size_t n) {
    std::string css = "@font-face { font-family: A; src: local(a); unicode-range: ";
    for (size_t i = 0; i < n; i++) {
      char b[40];
      std::snprintf(b, sizeof b, "%sU+%zX-%zX", i ? ", " : "", (n - i) * 4, (n - i) * 4 + 1);
      css += b;
    }
    css += " }";
    GTEXT_CSS_Atrule_Options o = gtext_css_atrule_options_default();
    o.max_list = 10000000;
    o.max_descriptors = 10000000;
    return [css, o] {
      css_test::Parsed p;
      GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
      po.max_tokens = 100000000;
      p.status = gtext_css_parse_stylesheet(css.data(), css.size(), &po, &p.tree, &p.result);
      ASSERT_NE(p.tree, nullptr);
      GTEXT_CSS_Atrule * a = nullptr;
      GTEXT_CSS_Atrule_Result res;
      ASSERT_EQ(gtext_css_atrule_parse(R(), &p.tree->rules[0], &o, &a, &res), GTEXT_CSS_OK);
      GTEXT_CSS_Font_Face * f = nullptr;
      EXPECT_EQ(gtext_css_font_face_from_atrule(a, &f), GTEXT_CSS_OK);
      gtext_css_font_face_free(f);
      gtext_css_atrule_free(a);
    };
  });
}

TEST(CssAtruleCost, ManyDistinctRules) {
  expect_linear("many distinct rules", 2000, [](size_t n) {
    std::string css;
    for (size_t i = 0; i < n; i++) {
      css += "@font-face { font-family: F" + std::to_string(i) + "; src: local(x) } @keyframes k" + std::to_string(i) + " { to { color: red } }";
    }
    return [css, n] { build_set(css, 2 * n); };
  });
}
