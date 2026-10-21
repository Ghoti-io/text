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
 * Allocation failure in the CSS parser: every entry point, with the n-th
 * allocation failed for every n up to the last one the call makes.
 *
 * A failed run is GTEXT_CSS_E_OOM with no tree, or the answer of the run that
 * failed nothing (the same tree and the same errors). Either way the
 * allocator holds no block afterwards and was never asked to free one it did
 * not make. The input uses every allocation site: tokens past the first
 * growth of their array, a string past its first buffer, the bracket match
 * table, the value and rule stacks, a re-tokenized unicode-range value, a
 * custom property's original text, a location, a dropped block, and the
 * error list.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_test_support.h"
#include "markup_test_support.h"

using namespace css_test;

namespace {

std::string rich() {
  std::string s = "@import url(x.css) layer(a); @media (min-width: 1px) { a { b: c } d: e; }\n";
  s += "x, y > z { color: red !important; --custom: { a ] b } c; unicode-range: U+0-7F, u+4??;";
  s += " & .n { m: n } after: rule; font: 12px/1.5 \"" + std::string(200, 'q') + "\" }\n";
  s += "--x:hover { a {} } p { q: url(bad url) 'bad\n; r: s } t } u {";
  for (int i = 0; i < 40; i++) {
    s += " v" + std::to_string(i) + ": f(g(h[i{j}])) " + std::to_string(i) + "px;";
  }
  return s;
}

struct Entry {
  const char * name;
  ParseFn fn;
};

const Entry kEntries[] = {
    {"stylesheet", gtext_css_parse_stylesheet},
    {"stylesheet_contents", gtext_css_parse_stylesheet_contents},
    {"declaration_list", gtext_css_parse_declaration_list},
    {"block_contents", gtext_css_parse_block_contents},
    {"rule", gtext_css_parse_rule},
    {"declaration", gtext_css_parse_declaration},
    {"component_value", gtext_css_parse_component_value},
    {"component_values", gtext_css_parse_component_values},
    {"comma_list", gtext_css_parse_comma_list},
};

struct Outcome {
  GTEXT_CSS_Status status;
  std::string tree;
  std::vector<std::string> errors;
  size_t dropped = 0;
};

Outcome run(ParseFn fn, const std::string & css, const GTEXT_CSS_Parse_Options * o, bool with_result) {
  Outcome out;
  GTEXT_CSS_Tree * t = nullptr;
  GTEXT_CSS_Result r{};
  out.status = fn(css.data(), css.size(), o, &t, with_result ? &r : nullptr);
  if (t) {
    out.tree = canon(t) + "\n" + serialize(t);
  }
  for (size_t i = 0; i < gtext_css_result_error_count(&r); i++) {
    GTEXT_CSS_Error e;
    gtext_css_result_error_at(&r, i, &e);
    out.errors.push_back(std::string(e.name) + "@" + std::to_string(e.byte_offset));
  }
  out.dropped = gtext_css_result_errors_dropped(&r);
  if (out.status != GTEXT_CSS_OK) {
    EXPECT_EQ(t, nullptr) << "a failure leaves no tree";
  }
  gtext_css_tree_free(t);
  gtext_css_result_free(&r);
  return out;
}

void sweep(const Entry & e, const std::string & css, bool with_result) {
  GTEXT_CSS_Parse_Options base = gtext_css_parse_options_default();
  base.location = "a-location";
  const Outcome want = run(e.fn, css, &base, with_result);
  ASSERT_EQ(want.status, GTEXT_CSS_OK) << e.name;
  bool reached_the_end = false;
  size_t failures = 0;
  for (size_t n = 1; n <= 20000 && !reached_the_end; n++) {
    FaultAllocator fa;
    GTEXT_CSS_Parse_Options o = base;
    o.allocator = &fa.alloc;
    fa.arm(n);
    const Outcome got = run(e.fn, css, &o, with_result);
    fa.disarm();
    EXPECT_TRUE(fa.live.empty()) << e.name << " " << n << ": a block is left";
    EXPECT_EQ(fa.bad_frees, 0u) << e.name << " " << n;
    if (!fa.failed) {
      reached_the_end = true;
      EXPECT_EQ(got.status, GTEXT_CSS_OK);
      EXPECT_EQ(got.tree, want.tree);
      continue;
    }
    if (got.status == GTEXT_CSS_E_OOM) {
      failures++;
      continue;
    }
    ASSERT_EQ(got.status, GTEXT_CSS_OK) << e.name << " " << n;
    EXPECT_EQ(got.tree, want.tree) << e.name << " " << n;
    EXPECT_EQ(got.errors, want.errors) << e.name << " " << n;
    EXPECT_EQ(got.dropped, want.dropped) << e.name << " " << n;
  }
  EXPECT_TRUE(reached_the_end) << e.name << ": the sweep did not reach the last allocation";
  EXPECT_GT(failures, 10u) << e.name << ": the allocations failed should mostly be refused";
}

TEST(CssOom, EveryAllocationOfEveryEntryPointCanFailAndNothingLeaks) {
  const std::string css = rich();
  for (const Entry & e : kEntries) {
    SCOPED_TRACE(e.name);
    sweep(e, css, true);
  }
}

TEST(CssOom, WithNoResultTheSameHolds) {
  const std::string css = rich();
  for (const Entry & e : kEntries) {
    SCOPED_TRACE(e.name);
    sweep(e, css, false);
  }
}

bool any(void *, size_t, const GTEXT_CSS_Component_Value *, size_t n) {
  return n > 0;
}

TEST(CssOom, TheGrammarEntryPointsToo) {
  const std::string css = "a, b(c), d e";
  GTEXT_CSS_Grammar g = {any, nullptr};
  bool done = false;
  for (size_t n = 1; n < 500 && !done; n++) {
    FaultAllocator fa;
    GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
    o.allocator = &fa.alloc;
    fa.arm(n);
    GTEXT_CSS_Tree * t = nullptr;
    bool matched = false;
    size_t m = 0;
    GTEXT_CSS_Status st = gtext_css_parse_grammar(css.data(), css.size(), &g, &o, &matched, &t, nullptr);
    gtext_css_tree_free(st == GTEXT_CSS_OK ? t : nullptr);
    t = nullptr;
    GTEXT_CSS_Status st2 = gtext_css_parse_grammar_list(css.data(), css.size(), &g, &o, &m, &t, nullptr);
    gtext_css_tree_free(st2 == GTEXT_CSS_OK ? t : nullptr);
    fa.disarm();
    EXPECT_TRUE(st == GTEXT_CSS_OK || st == GTEXT_CSS_E_OOM);
    EXPECT_TRUE(st2 == GTEXT_CSS_OK || st2 == GTEXT_CSS_E_OOM);
    if (st2 == GTEXT_CSS_OK) {
      EXPECT_EQ(m, 3u);
    }
    EXPECT_TRUE(fa.live.empty()) << n;
    done = !fa.failed;
  }
  EXPECT_TRUE(done);
}

} // namespace
