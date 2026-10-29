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
 * Out of memory in conditional style sheets and container queries: an allocation failed at each place in turn is GTEXT_CSS_E_OOM, with no result, nothing leaked and nothing partial,
 * for the compile of a sheet that holds every kind of condition node, for the index built over it (the evaluation of its nodes, the layer occurrences) and for the collection of an
 * element's rules through container queries; the first call that succeeds returns what one that never failed does.
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
    "@layer a, b; @media (min-width: 600px) { @layer c { p { color: red } } } @supports (display: grid) { p { margin-top: 1px } } "
    "@when media(width > 1px) { p { margin-left: 2px } } @else { p { margin-left: 3px } } "
    "@container (width > 1px) { @layer d { p { margin-right: 4px } } } @container style(--a: 1) and style(--b) { p { color: blue } } "
    ".x { @media (min-width: 1px) { color: green; & p { padding-top: 5px } } @starting-style { padding-left: 1px } } @import url(lib.css) layer(e) supports(display: grid) (min-width: 1px) scope((.x))";

}  // namespace

TEST(CssConditionsOom, CompilingTheIndexingAndTheCollectionFailCleanly) {
  css_test::Parsed lib;
  css_test::parse_into(lib, gtext_css_parse_stylesheet, "p { background-color: red }");
  GTEXT_CSS_Sheet * libsheet = nullptr;
  ASSERT_EQ(gtext_css_sheet_compile(lib.tree, R(), nullptr, &libsheet, nullptr), GTEXT_CSS_OK);
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_stylesheet, kSheet);
  ASSERT_NE(p.tree, nullptr);
  synth::Tree t;
  const int x = t.add(0, "div", "", {"x"});
  const int pp = t.add(x, "p");
  (void)pp;
  GTEXT_CSS_Selector_Match_Options mo = gtext_css_selector_match_options_default();
  mo.html_document = true;
  std::string want;
  bool answered = false;
  for (size_t n = 1; n < 40000 && !answered; n++) {
    FaultAllocator fa;
    GTEXT_CSS_Sheet_Options so = gtext_css_sheet_options_default();
    so.allocator = &fa.alloc;
    so.import = [](void * u, const GTEXT_CSS_Import_Request *) { return gtext_css_sheet_retain(static_cast<GTEXT_CSS_Sheet *>(u)); };
    so.import_user = libsheet;
    GTEXT_CSS_Conditions_Options co = gtext_css_conditions_options_default();
    co.allocator = &fa.alloc;
    co.element_interface = &t.table;
    GTEXT_CSS_Container_Interface ci;
    std::memset(&ci, 0, sizeof ci);
    ci.size = sizeof ci;
    ci.computed_style = [](void *, GTEXT_CSS_Element_Handle) -> const GTEXT_CSS_Computed_Style * { return nullptr; };
    co.container = &ci;
    GTEXT_CSS_Conditions * cond = nullptr;
    ASSERT_EQ(gtext_css_conditions_new(R(), &co, &cond), GTEXT_CSS_OK);
    GTEXT_CSS_Rule_Index_Options io = gtext_css_rule_index_options_default();
    io.allocator = &fa.alloc;
    io.html_document = true;
    io.conditions = cond;
    GTEXT_CSS_Rule_Index * ix = nullptr;
    ASSERT_EQ(gtext_css_rule_index_new(R(), &io, &ix), GTEXT_CSS_OK);
    GTEXT_CSS_Sheet * sheet = nullptr;
    fa.arm(n);
    GTEXT_CSS_Status st = gtext_css_sheet_compile(p.tree, R(), &so, &sheet, nullptr);
    std::string got;
    if (st == GTEXT_CSS_OK) {
      GTEXT_CSS_Sheet_Registration reg;
      std::memset(&reg, 0, sizeof reg);
      reg.origin = GTEXT_CSS_ORIGIN_AUTHOR;
      st = gtext_css_rule_index_add_sheet(ix, sheet, &reg, nullptr);
      if (st == GTEXT_CSS_OK) {
        st = gtext_css_rule_index_freeze(ix, nullptr);
      }
    }
    if (st == GTEXT_CSS_OK) {
      GTEXT_CSS_Selector_Match_Options o2 = mo;
      o2.allocator = &fa.alloc;
      GTEXT_CSS_Selector_Match_Context * ctx = nullptr;
      st = gtext_css_selector_match_context_new(&t.table, &o2, &ctx);
      for (int e = 1; st == GTEXT_CSS_OK && e < static_cast<int>(t.e.size()); e++) {
        GTEXT_CSS_Candidate_Options cao = gtext_css_candidate_options_default();
        cao.allocator = &fa.alloc;
        GTEXT_CSS_Candidates * set = nullptr;
        st = gtext_css_candidates_new(R(), nullptr, &cao, &set);
        if (st != GTEXT_CSS_OK) {
          break;
        }
        GTEXT_CSS_Rules_Collect_Options ro;
        std::memset(&ro, 0, sizeof ro);
        ro.starting_style = (e & 1) != 0;
        st = gtext_css_rules_collect(ix, ctx, synth::Tree::H(e), nullptr, set, &ro, nullptr);
        for (size_t i = 0; st == GTEXT_CSS_OK && i < gtext_css_candidates_count(set); i++) {
          GTEXT_CSS_Cascade_Entry en;
          gtext_css_candidates_entry(set, i, &en);
          got += std::to_string(e) + ":" + std::to_string(en.sheet) + "." + std::to_string(en.rule) + "." + std::to_string(en.declaration) + ";";
        }
        gtext_css_candidates_free(set);
      }
      gtext_css_selector_match_context_free(ctx);
    }
    const bool failed = fa.failed;
    fa.disarm();
    if (st == GTEXT_CSS_OK) {
      EXPECT_FALSE(failed) << "at " << n << ": an allocation failed and the call did not say so";
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
    gtext_css_sheet_free(sheet);
    gtext_css_conditions_free(cond);
    EXPECT_TRUE(fa.live.empty()) << "at " << n << ": " << fa.live.size() << " blocks leaked";
    EXPECT_EQ(fa.bad_frees, 0u);
  }
  EXPECT_TRUE(answered);
  EXPECT_FALSE(want.empty());
  gtext_css_sheet_free(libsheet);
}
