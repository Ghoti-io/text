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
 * The cost of style sheets and the rule index, measured in the counters and not in time. The index tests the rules of the keys an element has, so the work to style an element
 * does not grow with the number of rules that are for other elements; compiling a nested rule costs the size of its own selector and not the product with its parents'; and the
 * search for the scoping root of an `@scope` is a walk up the ancestors.
 */

#include <string>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_cascade_test_support.h"
#include "css_synth_tree.h"
#include "css_test_support.h"

using namespace cas;

namespace {

struct Built {
  GTEXT_CSS_Sheet * s = nullptr;
  GTEXT_CSS_Rule_Index * x = nullptr;
  ~Built() {
    gtext_css_rule_index_free(x);
    gtext_css_sheet_free(s);
  }
};

void build(Built & b, const std::string & css, bool shared_scope = false) {
  (void)shared_scope;
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_stylesheet, css);
  ASSERT_NE(p.tree, nullptr);
  ASSERT_EQ(gtext_css_sheet_compile(p.tree, R(), nullptr, &b.s, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Rule_Index_Options io = gtext_css_rule_index_options_default();
  io.html_document = true;
  ASSERT_EQ(gtext_css_rule_index_new(R(), &io, &b.x), GTEXT_CSS_OK);
  GTEXT_CSS_Sheet_Registration reg;
  std::memset(&reg, 0, sizeof reg);
  reg.origin = GTEXT_CSS_ORIGIN_AUTHOR;
  ASSERT_EQ(gtext_css_rule_index_add_sheet(b.x, b.s, &reg, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_rule_index_freeze(b.x, nullptr), GTEXT_CSS_OK);
}

size_t considered(synth::Tree & t, GTEXT_CSS_Rule_Index * x, int el, GTEXT_CSS_Selector_Match_Counters * out = nullptr) {
  GTEXT_CSS_Selector_Match_Options mo = gtext_css_selector_match_options_default();
  mo.html_document = true;
  GTEXT_CSS_Selector_Match_Context * ctx = nullptr;
  EXPECT_EQ(gtext_css_selector_match_context_new(&t.table, &mo, &ctx), GTEXT_CSS_OK);
  Set set(nullptr);
  GTEXT_CSS_Rule_Index_Result r;
  EXPECT_EQ(gtext_css_rules_collect(x, ctx, synth::Tree::H(el), nullptr, set.s, &r), GTEXT_CSS_OK);
  if (out) {
    *out = gtext_css_selector_match_counters(ctx);
  }
  gtext_css_selector_match_context_free(ctx);
  return r.work;
}

}  // namespace

TEST(CssSheetCost, TheWorkToStyleAnElementDoesNotGrowWithTheRulesThatAreForOthers) {
  size_t last = 0;
  for (size_t rules : {100u, 1000u, 8000u}) {
    std::string css;
    for (size_t i = 0; i < rules; i++) {
      css += ".c" + std::to_string(i) + " { color: red } #i" + std::to_string(i) + " { color: red } e" + std::to_string(i) + " { color: red }\n";
    }
    css += ".hit { color: blue } * { margin-top: 1px }";
    Built b;
    build(b, css);
    synth::Tree t;
    const int e = t.add(0, "div", "i5", {"hit", "c7"});
    const size_t w = considered(t, b.x, e);
    EXPECT_LE(w, 6u) << rules << " rules: only .hit, .c7, #i5 and the rule with no key are tested";
    if (last) {
      EXPECT_EQ(w, last) << "the same work for ten times the rules";
    }
    last = w;
  }
}

TEST(CssSheetCost, StylingEveryElementOfATreeIsLinearInTheElements) {
  std::string css;
  for (int i = 0; i < 400; i++) {
    css += ".c" + std::to_string(i) + " > .d" + std::to_string(i) + " { color: red }\n";
  }
  css += "div div { margin-top: 1px } .x ~ .x { margin-left: 1px }";
  Built b;
  build(b, css);
  size_t totals[2];
  int k = 0;
  for (int n : {400, 1600}) {
    synth::Tree t;
    int parent = 0;
    for (int i = 0; i < n; i++) {
      parent = t.add(i % 8 == 0 ? 0 : parent, "div", "", {i % 2 ? "x" : "d" + std::to_string(i % 400)});
    }
    GTEXT_CSS_Selector_Match_Options mo = gtext_css_selector_match_options_default();
    mo.html_document = true;
    GTEXT_CSS_Selector_Match_Context * ctx = nullptr;
    ASSERT_EQ(gtext_css_selector_match_context_new(&t.table, &mo, &ctx), GTEXT_CSS_OK);
    for (int e = 1; e <= n; e++) {
      Set set(nullptr);
      ASSERT_EQ(gtext_css_rules_collect(b.x, ctx, synth::Tree::H(e), nullptr, set.s, nullptr), GTEXT_CSS_OK);
    }
    totals[k++] = gtext_css_selector_match_counters(ctx).compounds_tested;
    gtext_css_selector_match_context_free(ctx);
  }
  EXPECT_LE(totals[1], totals[0] * 4 + 4000) << "four times the elements: " << totals[0] << " then " << totals[1] << " compound tests";
}

TEST(CssSheetCost, ANestedRuleCostsItsOwnSelectorsAndNotTheProductWithItsParents) {
  /* a,b,c { d,e,f { g,h,i { ... } } } is nine selectors of three compounds read as 3 + 3 + 3, not 27. */
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_stylesheet, "a, b, c { d, e, f { g, h, i { j, k, l { color: red } } } }");
  GTEXT_CSS_Sheet * s = nullptr;
  ASSERT_EQ(gtext_css_sheet_compile(p.tree, R(), nullptr, &s, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_sheet_record_count(s), 3u) << "only the rules that have declarations make records, one for each selector of the rule's own list";
  gtext_css_sheet_free(s);
  std::string deep;
  for (int i = 0; i < 40; i++) {
    deep += "a, b, c, d, e { ";
  }
  deep += "color: red";
  for (int i = 0; i < 40; i++) {
    deep += " }";
  }
  GTEXT_CSS_Sheet_Options o = gtext_css_sheet_options_default();
  o.max_rule_depth = 64;
  css_test::Parsed p2;
  css_test::parse_into(p2, gtext_css_parse_stylesheet, deep);
  ASSERT_EQ(gtext_css_sheet_compile(p2.tree, R(), &o, &s, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_sheet_record_count(s), 5u) << "forty levels of five selectors is five records, not 5^40";
  gtext_css_sheet_free(s);
}

TEST(CssSheetCost, TheSearchForAScopingRootIsAWalkUpTheAncestors) {
  Built b;
  build(b, "@scope (.a) { p.never { color: red } }");
  for (int depth : {200, 400, 800}) {
    synth::Tree t;
    int parent = 0;
    for (int i = 0; i < depth; i++) {
      parent = t.add(parent, "div", "", {"a"});
    }
    const int pe = t.add(parent, "p");
    GTEXT_CSS_Selector_Match_Counters c;
    considered(t, b.x, pe, &c);
    EXPECT_LE(c.compounds_tested, static_cast<size_t>(depth) * 8 + 16) << "depth " << depth;
  }
}

TEST(CssSheetCost, AScopeWithALimitIsBoundedByTheWorkLimitAndNeverHangs) {
  Built b;
  build(b, "@scope (.a) to (.z) { p { color: red } }");
  synth::Tree t;
  int parent = 0;
  for (int i = 0; i < 300; i++) {
    parent = t.add(parent, "div", "", {"a"});
  }
  const int pe = t.add(parent, "p");
  GTEXT_CSS_Selector_Match_Options mo = gtext_css_selector_match_options_default();
  mo.max_work = 500;
  GTEXT_CSS_Selector_Match_Context * ctx = nullptr;
  ASSERT_EQ(gtext_css_selector_match_context_new(&t.table, &mo, &ctx), GTEXT_CSS_OK);
  Set set(nullptr);
  const GTEXT_CSS_Status st = gtext_css_rules_collect(b.x, ctx, synth::Tree::H(pe), nullptr, set.s, nullptr);
  EXPECT_TRUE(st == GTEXT_CSS_OK || st == GTEXT_CSS_E_LIMIT_MATCH_WORK) << gtext_css_status_string(st);
  gtext_css_selector_match_context_free(ctx);
}
