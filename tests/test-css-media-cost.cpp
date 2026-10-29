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
 * The cost of media queries: the work counters grow linearly with the input (n, 4n, 16n) for each shape - a chain of `not`, a long `or` list, a custom media chain and a
 * cycle, many queries, a long `and` - and never with its square.
 */

#include <cstdlib>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

namespace {

/// GTEXT_COST_SCALE multiplies the sizes of the shapes (default 1): tools/css/cost-instructions.py runs the program under callgrind at two scales and compares the instructions.
size_t cost_scale() {
  const char * s = std::getenv("GTEXT_COST_SCALE");
  const long v = s ? std::atol(s) : 1;
  return v > 0 ? static_cast<size_t>(v) : 1;
}

struct Cost {
  size_t visits = 0, defs = 0, nodes = 0;
};

Cost run(const std::string & q, const std::string * defs_text = nullptr, size_t ndefs = 0) {
  GTEXT_CSS_Condition_Defs * defs = nullptr;
  EXPECT_EQ(gtext_css_condition_defs_new(nullptr, &defs), GTEXT_CSS_OK);
  for (size_t i = 0; i < ndefs; i++) {
    EXPECT_EQ(gtext_css_condition_defs_add_custom_media_text(defs, defs_text[i].data(), defs_text[i].size(), nullptr, nullptr), GTEXT_CSS_OK);
  }
  GTEXT_CSS_Conditions_Options co = gtext_css_conditions_options_default();
  co.defs = defs;
  co.max_work = (size_t)1 << 30;
  GTEXT_CSS_Conditions * c = nullptr;
  EXPECT_EQ(gtext_css_conditions_new(nullptr, &co, &c), GTEXT_CSS_OK);
  GTEXT_CSS_Media_Options mo = gtext_css_media_options_default();
  mo.max_depth = 1u << 20;
  mo.max_nodes = 1u << 24;
  mo.max_queries = 1u << 20;
  mo.parse.max_depth = 1u << 20;
  GTEXT_CSS_Media_List * l = nullptr;
  EXPECT_EQ(gtext_css_media_list_parse(q.data(), q.size(), &mo, &l, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Condition_Call call;
  gtext_css_condition_call_init(&call);
  bool m;
  EXPECT_EQ(gtext_css_conditions_matches_list(c, l, &call, &m), GTEXT_CSS_OK);
  Cost r;
  r.visits = call.node_visits;
  r.defs = call.defs_evaluated;
  r.nodes = gtext_css_media_list_node_count(l);
  gtext_css_condition_call_free(&call);
  gtext_css_media_list_free(l);
  gtext_css_conditions_free(c);
  gtext_css_condition_defs_free(defs);
  return r;
}

template <typename F> void linear(F shape, size_t n) {
  n *= cost_scale();
  Cost a = run(shape(n)), b = run(shape(4 * n)), c = run(shape(16 * n));
  EXPECT_LE(b.visits, 5 * a.visits + 16) << "n to 4n";
  EXPECT_LE(c.visits, 5 * b.visits + 16) << "4n to 16n";
  EXPECT_LE(b.nodes, 5 * a.nodes + 16);
  EXPECT_LE(c.nodes, 5 * b.nodes + 16);
}

}  // namespace

TEST(CssMediaCost, ChainOfNot) {
  linear([](size_t n) { std::string s; for (size_t i = 0; i < n; i++) s += "(not "; s += "(color)"; for (size_t i = 0; i < n; i++) s += ")"; return s; }, 500);
}

TEST(CssMediaCost, LongOrAndAndLists) {
  linear([](size_t n) { std::string s; for (size_t i = 0; i < n; i++) s += (i ? " or (foo)" : "(foo)"); return s; }, 500);
  linear([](size_t n) { std::string s; for (size_t i = 0; i < n; i++) s += (i ? " and (color)" : "(color)"); return s; }, 500);
}

TEST(CssMediaCost, ManyQueries) {
  linear([](size_t n) { std::string s; for (size_t i = 0; i < n; i++) s += (i ? ", (foo)" : "(foo)"); return s; }, 500);
}

TEST(CssMediaCost, GeneralEnclosedNestedDeepIsParsedOnce) {
  /* `((((a b) c) c) c)`: each level fails as a condition and is read as a feature: no block is parsed twice. */
  linear([](size_t n) { std::string s; for (size_t i = 0; i < n; i++) s += "("; s += "a b"; for (size_t i = 0; i < n; i++) s += ") c"; return s; }, 250);
}

TEST(CssMediaCost, CustomMediaChainIsOnePerDefinition) {
  for (size_t n : {200u, 800u, 3200u}) {
    std::string defs[3200];
    defs[0] = "--c0 (color)";
    for (size_t i = 1; i < n; i++) defs[i] = "--c" + std::to_string(i) + " (--c" + std::to_string(i - 1) + ")";
    Cost r = run("(--c" + std::to_string(n - 1) + ")", defs, n);
    EXPECT_EQ(r.defs, n);
    EXPECT_LE(r.visits, 6 * n + 16);
  }
}

TEST(CssMediaCost, ManyUsesOfOneDefinitionEvaluateItOnce) {
  std::string defs[1] = {"--c0 (color) and (width > 1px) and (hover)"};
  for (size_t n : {100u, 400u, 1600u}) {
    std::string q;
    for (size_t i = 0; i < n; i++) q += (i ? " and (--c0)" : "(--c0)");
    Cost r = run(q, defs, 1);
    EXPECT_EQ(r.defs, 1u);
  }
}

TEST(CssMediaCost, ACycleCostsNothingAtEvaluation) {
  std::string defs[2] = {"--a (--b)", "--b (--a)"};
  Cost r = run("(--a), (--b)", defs, 2);
  EXPECT_EQ(r.defs, 0u);
}
