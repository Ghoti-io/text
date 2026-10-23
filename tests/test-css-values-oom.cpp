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
 * Allocation failure in the Value Definition Syntax engine: parsing a
 * definition, linking it, matching with the typed value, writing the value and
 * the grammar hook, each with the n-th allocation failed for every n up to the
 * last one the call makes.
 *
 * A failed run is GTEXT_CSS_E_OOM with nothing returned, or the answer of the
 * run that failed nothing. Either way the allocator holds no block afterwards
 * and was never asked to free one it did not make.
 */

#include <string>

#include <gtest/gtest.h>

#include "css_values_test_support.h"
#include "markup_test_support.h"

using namespace gv;

namespace {

const char * kGrammar =
    "[ <length-percentage [0,∞]> | auto | <rgb()> ]{1,4} [ / <number>#{1,3} ]? [ foo( <ident>? ) || bar | <url> ]? , <string>*";
const char * kInput = "10px auto rgb(1,2,3) 5% / 1, 2 bar , 'a' 'b'";

void fill(Defs & d) {
  d.types["length-percentage"] = "<length> | <percentage>";
  d.types["rgb()"] = "rgb( <number>#{3} )";
}

TEST(CssValuesOom, ParsingADefinitionSweptOverEveryAllocation) {
  const std::string text = kGrammar;
  size_t failures = 0;
  for (size_t n = 1; n < 4000; n++) {
    FaultAllocator fa;
    GTEXT_CSS_Grammar_Options o = gtext_css_grammar_options_default();
    o.allocator = &fa.alloc;
    GTEXT_CSS_Definition * d = nullptr;
    GTEXT_CSS_Grammar_Result r{};
    fa.arm(n);
    const GTEXT_CSS_Status st = gtext_css_definition_parse(text.data(), text.size(), &o, &d, &r);
    fa.disarm();
    if (!fa.failed) {
      EXPECT_EQ(st, GTEXT_CSS_OK);
      gtext_css_definition_free(d);
      EXPECT_TRUE(fa.live.empty());
      break;
    }
    failures++;
    EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
    EXPECT_EQ(d, nullptr) << n;
    EXPECT_EQ(r.status, GTEXT_CSS_E_OOM);
    EXPECT_TRUE(fa.live.empty()) << n << ": a block is left";
    EXPECT_EQ(fa.bad_frees, 0u);
  }
  EXPECT_GT(failures, 5u);
}

TEST(CssValuesOom, LinkingSweptOverEveryAllocation) {
  size_t failures = 0;
  for (size_t n = 1; n < 4000; n++) {
    FaultAllocator fa;
    Defs d;
    fill(d);
    GTEXT_CSS_Grammar_Options o = gtext_css_grammar_options_default();
    o.allocator = &fa.alloc;
    GTEXT_CSS_Compiled * c = nullptr;
    GTEXT_CSS_Grammar_Result r{};
    fa.arm(n);
    const GTEXT_CSS_Status st = gtext_css_compile(kGrammar, strlen(kGrammar), Defs::resolve, &d, &o, &c, &r);
    fa.disarm();
    if (!fa.failed) {
      EXPECT_EQ(st, GTEXT_CSS_OK);
      gtext_css_compiled_free(c);
      EXPECT_TRUE(fa.live.empty());
      break;
    }
    failures++;
    EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n << " " << gtext_css_status_string(st);
    EXPECT_EQ(c, nullptr) << n;
    EXPECT_TRUE(fa.live.empty()) << n << ": a block is left";
    EXPECT_EQ(fa.bad_frees, 0u) << n;
  }
  EXPECT_GT(failures, 20u);
}

TEST(CssValuesOom, MatchingAndTheTypedValueSweptOverEveryAllocation) {
  Defs d;
  fill(d);
  Grammar g;
  compile_into(g, kGrammar, &d);
  ASSERT_TRUE(g.ok());
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_component_values, kInput);
  ASSERT_EQ(p.status, GTEXT_CSS_OK);
  Matched want;
  match_values(want, g, p.tree->values, p.tree->value_count);
  ASSERT_TRUE(want.matched) << want.dump();
  const std::string expected = want.dump();
  size_t failures = 0;
  for (size_t n = 1; n < 20000; n++) {
    FaultAllocator fa;
    GTEXT_CSS_Match_Options o = gtext_css_match_options_default();
    o.allocator = &fa.alloc;
    bool matched = false;
    GTEXT_CSS_Value_Set * set = nullptr;
    GTEXT_CSS_Match_Result r{};
    fa.arm(n);
    const GTEXT_CSS_Status st = gtext_css_match(g.c, p.tree->values, p.tree->value_count, &o, &matched, &set, &r);
    fa.disarm();
    if (!fa.failed) {
      EXPECT_EQ(st, GTEXT_CSS_OK);
      EXPECT_TRUE(matched);
      EXPECT_EQ(dump(gtext_css_value_set_root(set)), expected);
      gtext_css_value_set_free(set);
      EXPECT_TRUE(fa.live.empty());
      break;
    }
    failures++;
    EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
    EXPECT_EQ(set, nullptr) << n;
    EXPECT_EQ(r.status, GTEXT_CSS_E_OOM);
    EXPECT_TRUE(fa.live.empty()) << n << ": a block is left";
    EXPECT_EQ(fa.bad_frees, 0u) << n;
  }
  EXPECT_GT(failures, 50u);
}

TEST(CssValuesOom, APendingValueAndWritingAValueSweptOverEveryAllocation) {
  Grammar g;
  compile_into(g, "<length>");
  ASSERT_TRUE(g.ok());
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_component_values, "calc(var(--a) + 1px)");
  for (size_t n = 1; n < 200; n++) {
    FaultAllocator fa;
    GTEXT_CSS_Match_Options o = gtext_css_match_options_default();
    o.allocator = &fa.alloc;
    bool matched = false;
    GTEXT_CSS_Value_Set * set = nullptr;
    fa.arm(n);
    const GTEXT_CSS_Status st = gtext_css_match(g.c, p.tree->values, p.tree->value_count, &o, &matched, &set, nullptr);
    fa.disarm();
    if (st == GTEXT_CSS_OK) {
      EXPECT_TRUE(matched);
      gtext_css_value_set_free(set);
      EXPECT_TRUE(fa.live.empty());
      if (!fa.failed) {
        break;
      }
    }
    else {
      EXPECT_EQ(st, GTEXT_CSS_E_OOM);
      EXPECT_EQ(set, nullptr);
      EXPECT_TRUE(fa.live.empty()) << n;
    }
  }
}

TEST(CssValuesOom, TheGrammarHookKeepsNothingWhenAMatchFails) {
  Grammar g;
  compile_into(g, "<length> | auto");
  ASSERT_TRUE(g.ok());
  for (size_t n = 1; n < 100; n++) {
    FaultAllocator fa;
    GTEXT_CSS_Match_Options o = gtext_css_match_options_default();
    o.allocator = &fa.alloc;
    GTEXT_CSS_Matcher mt;
    ASSERT_EQ(gtext_css_matcher_init(&mt, g.c, &o), GTEXT_CSS_OK);
    const GTEXT_CSS_Grammar hook = gtext_css_matcher_grammar(&mt);
    css_test::Parsed p;
    css_test::parse_into(p, gtext_css_parse_component_values, "12px");
    fa.arm(n);
    const bool hit = gtext_css_match_grammar(p.tree->values, p.tree->value_count, &hook);
    fa.disarm();
    if (hit) {
      EXPECT_NE(gtext_css_matcher_value_set(&mt, 0), nullptr);
    }
    else if (fa.failed) {
      EXPECT_EQ(mt.status, GTEXT_CSS_E_OOM);
      EXPECT_EQ(gtext_css_matcher_value_set(&mt, 0), nullptr);
    }
    gtext_css_matcher_free(&mt);
    EXPECT_TRUE(fa.live.empty()) << n;
    if (!fa.failed) {
      break;
    }
  }
}

} // namespace
