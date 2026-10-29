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
 * Out of memory in style sheets and the rule index: an allocation failed at each place in turn is GTEXT_CSS_E_OOM with no result, nothing leaked and nothing freed that the
 * allocator did not make; the first call that succeeds returns what a call that was never failed does.
 */

#include <string>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_cascade_test_support.h"
#include "css_synth_tree.h"
#include "css_test_support.h"
#include "markup_test_support.h"

using namespace cas;

namespace {

const char * const kSheet =
    "@namespace svg url(urn:svg); @layer base, theme; @layer theme { a { color: red } } "
    "a, .x { color: blue; & b { margin-top: 1px } > c { margin-left: 2px } margin-right: 3px } "
    "@scope (.x) to (.y) { p { color: green !important } } @media print { li { padding-top: 1px } } @layer { b { padding-left: 4px } } :host { --k: 1 }";

std::string dump(const GTEXT_CSS_Sheet * s) {
  std::string out;
  GTEXT_Markup_Sink sink;
  sink.user = &out;
  sink.write = [](void * u, const char * d, size_t n) {
    static_cast<std::string *>(u)->append(d, n);
    return GTEXT_MARKUP_OK;
  };
  EXPECT_EQ(gtext_css_sheet_dump(s, &sink), GTEXT_CSS_OK);
  return out;
}

}  // namespace

TEST(CssSheetOom, CompilingFailsCleanlyAtEveryAllocationAndThenAnswersAsIfNeverFailed) {
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_stylesheet, kSheet);
  ASSERT_NE(p.tree, nullptr);
  GTEXT_CSS_Sheet_Options base = gtext_css_sheet_options_default();
  GTEXT_CSS_Sheet * clean = nullptr;
  ASSERT_EQ(gtext_css_sheet_compile(p.tree, R(), &base, &clean, nullptr), GTEXT_CSS_OK);
  const std::string want = dump(clean);
  gtext_css_sheet_free(clean);
  bool answered = false;
  for (size_t n = 1; n < 20000 && !answered; n++) {
    FaultAllocator fa;
    GTEXT_CSS_Sheet_Options o = base;
    o.allocator = &fa.alloc;
    GTEXT_CSS_Sheet * s = nullptr;
    GTEXT_CSS_Sheet_Result r;
    fa.arm(n);
    const GTEXT_CSS_Status st = gtext_css_sheet_compile(p.tree, R(), &o, &s, &r);
    const bool failed = fa.failed;
    fa.disarm();
    if (st == GTEXT_CSS_OK) {
      EXPECT_FALSE(failed) << "at " << n << ": an allocation failed and the call did not say so";
      EXPECT_EQ(dump(s), want) << "at " << n;
      answered = true;
    }
    else {
      EXPECT_EQ(st, GTEXT_CSS_E_OOM) << "at " << n;
      EXPECT_EQ(s, nullptr);
    }
    gtext_css_sheet_free(s);
    EXPECT_TRUE(fa.live.empty()) << "at " << n << ": " << fa.live.size() << " blocks leaked";
    EXPECT_EQ(fa.bad_frees, 0u);
  }
  EXPECT_TRUE(answered);
}

TEST(CssSheetOom, TheIndexAndTheCollectionFailCleanlyToo) {
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_stylesheet, "@layer a, b; @layer b { p { color: red } } p, .x { color: blue; & i { margin-top: 1px } } @scope (.x) { p { margin-left: 2px } }");
  GTEXT_CSS_Sheet_Options base = gtext_css_sheet_options_default();
  GTEXT_CSS_Sheet * sheet = nullptr;
  ASSERT_EQ(gtext_css_sheet_compile(p.tree, R(), &base, &sheet, nullptr), GTEXT_CSS_OK);
  synth::Tree t;
  const int x = t.add(0, "div", "", {"x"});
  const int pp = t.add(x, "p", "", {"x"});
  const int ii = t.add(pp, "i");
  (void)ii;
  GTEXT_CSS_Selector_Match_Options mo = gtext_css_selector_match_options_default();
  mo.html_document = true;
  std::string want;
  bool answered = false;
  for (size_t n = 1; n < 6000 && !answered; n++) {
    FaultAllocator fa;
    GTEXT_CSS_Rule_Index_Options io = gtext_css_rule_index_options_default();
    io.allocator = &fa.alloc;
    io.html_document = true;
    GTEXT_CSS_Rule_Index * ix = nullptr;
    ASSERT_EQ(gtext_css_rule_index_new(R(), &io, &ix), GTEXT_CSS_OK);
    GTEXT_CSS_Sheet_Registration reg;
    std::memset(&reg, 0, sizeof reg);
    reg.origin = GTEXT_CSS_ORIGIN_AUTHOR;
    fa.arm(n);
    GTEXT_CSS_Status st = gtext_css_rule_index_add_sheet(ix, sheet, &reg, nullptr);
    if (st == GTEXT_CSS_OK) {
      st = gtext_css_rule_index_freeze(ix, nullptr);
    }
    std::string got;
    if (st == GTEXT_CSS_OK) {
      GTEXT_CSS_Selector_Match_Options o2 = mo;
      o2.allocator = &fa.alloc;
      GTEXT_CSS_Selector_Match_Context * ctx = nullptr;
      st = gtext_css_selector_match_context_new(&t.table, &o2, &ctx);
      if (st == GTEXT_CSS_OK) {
        for (int e = 1; e < static_cast<int>(t.e.size()) && st == GTEXT_CSS_OK; e++) {
          GTEXT_CSS_Candidate_Options co = gtext_css_candidate_options_default();
          co.allocator = &fa.alloc;
          GTEXT_CSS_Candidates * set = nullptr;
          st = gtext_css_candidates_new(R(), nullptr, &co, &set);
          if (st != GTEXT_CSS_OK) {
            break;
          }
          st = gtext_css_rules_collect(ix, ctx, synth::Tree::H(e), nullptr, set, nullptr, nullptr);
          for (size_t i = 0; st == GTEXT_CSS_OK && i < gtext_css_candidates_count(set); i++) {
            GTEXT_CSS_Cascade_Entry en;
            gtext_css_candidates_entry(set, i, &en);
            got += std::to_string(e) + ":" + std::to_string(en.sheet) + "." + std::to_string(en.rule) + "." + std::to_string(en.declaration) + "@" +
                std::to_string(en.specificity.a) + std::to_string(en.specificity.b) + std::to_string(en.specificity.c) + ";";
          }
          gtext_css_candidates_free(set);
        }
        gtext_css_selector_match_context_free(ctx);
      }
    }
    const bool failed = fa.failed;
    fa.disarm();
    if (st == GTEXT_CSS_OK) {
      EXPECT_FALSE(failed) << "at " << n;
      if (want.empty()) {
        want = got;
      }
      EXPECT_EQ(got, want) << "at " << n;
      answered = true;
    }
    else {
      EXPECT_EQ(st, GTEXT_CSS_E_OOM) << "at " << n;
    }
    gtext_css_rule_index_free(ix);
    EXPECT_TRUE(fa.live.empty()) << "at " << n << ": " << fa.live.size() << " blocks leaked";
    EXPECT_EQ(fa.bad_frees, 0u);
  }
  EXPECT_TRUE(answered);
  EXPECT_FALSE(want.empty());
  gtext_css_sheet_free(sheet);
}
