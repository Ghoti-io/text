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
 * A compiled sheet and a frozen index are shared between threads: eight threads collect, cascade and retain from one sheet and one index over one Markup document, each with an
 * adapter and a match context of its own, and every thread's answer is the single-threaded answer. It runs in the ThreadSanitizer build with the other tests.
 */

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>
#include <ghoti.io/text/html.h>
#include <ghoti.io/text/markup.h>
#include <ghoti.io/text/markup/markup_query.h>

#include "css_cascade_test_support.h"
#include "css_test_support.h"

using namespace cas;

namespace {

std::string answers(const GTEXT_Markup_Document * doc, const GTEXT_CSS_Rule_Index * index) {
  GTEXT_CSS_Markup_Adapter * adapter = nullptr;
  EXPECT_EQ(gtext_css_markup_adapter_new(doc, nullptr, &adapter), GTEXT_CSS_OK);
  GTEXT_CSS_Selector_Match_Options mo = gtext_css_markup_match_options(adapter);
  GTEXT_CSS_Selector_Match_Context * ctx = nullptr;
  EXPECT_EQ(gtext_css_selector_match_context_new(gtext_css_markup_interface(adapter), &mo, &ctx), GTEXT_CSS_OK);
  GTEXT_Markup_Node ** all = nullptr;
  size_t n = 0;
  EXPECT_EQ(gtext_markup_query_selector_all(gtext_markup_document_root(doc), "*", 1, nullptr, &all, &n, nullptr), GTEXT_MARKUP_OK);
  std::string out;
  for (size_t i = 0; i < n; i++) {
    Set set(nullptr);
    EXPECT_EQ(gtext_css_rules_collect(index, ctx, gtext_css_markup_handle(all[i]), nullptr, set.s, nullptr), GTEXT_CSS_OK);
    Out o(set);
    out += o.value("color") + "/" + o.value("margin-top") + "/" + o.value("background-color") + ";";
  }
  gtext_markup_query_free(nullptr, all);
  gtext_css_selector_match_context_free(ctx);
  gtext_css_markup_adapter_free(adapter);
  return out;
}

}  // namespace

TEST(CssSheetThreads, OneSheetAndOneIndexServeEightThreadsAndEveryAnswerIsTheSingleThreadedOne) {
  std::string html = "<!DOCTYPE html><div class=card id=c>";
  for (int i = 0; i < 40; i++) {
    html += "<section class=s" + std::to_string(i % 5) + "><p class=lead>x</p><p>y</p><span id=i" + std::to_string(i) + ">z</span></section>";
  }
  html += "</div>";
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_HTML_Result hr;
  std::memset(&hr, 0, sizeof hr);
  ASSERT_EQ(gtext_html_parse(html.data(), html.size(), nullptr, &doc, &hr), GTEXT_HTML_OK);
  gtext_html_result_free(&hr);
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_stylesheet,
      "p { color: red } .card > section > .lead { color: green } section:nth-child(odd) p { margin-top: 3px } @layer a, b { } @layer b { span { background-color: blue } } "
      "@scope (.s1) { p { margin-top: 9px } } .s2 { & p { color: orange; &:first-child { margin-top: 1px } } } span:not(#i3) { margin-top: 5px }");
  GTEXT_CSS_Sheet_Options so = gtext_css_sheet_options_default();
  so.active = [](void *, const GTEXT_CSS_Rule *) { return true; };
  GTEXT_CSS_Sheet * sheet = nullptr;
  ASSERT_EQ(gtext_css_sheet_compile(p.tree, R(), &so, &sheet, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Rule_Index_Options io = gtext_css_rule_index_options_default();
  io.html_document = true;
  GTEXT_CSS_Rule_Index * index = nullptr;
  ASSERT_EQ(gtext_css_rule_index_new(R(), &io, &index), GTEXT_CSS_OK);
  GTEXT_CSS_Sheet_Registration reg;
  std::memset(&reg, 0, sizeof reg);
  reg.origin = GTEXT_CSS_ORIGIN_AUTHOR;
  ASSERT_EQ(gtext_css_rule_index_add_sheet(index, sheet, &reg, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_rule_index_freeze(index, nullptr), GTEXT_CSS_OK);
  const std::string want = answers(doc, index);
  ASSERT_NE(want.find("green"), std::string::npos);
  std::atomic<int> bad{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([&] {
      for (int round = 0; round < 3; round++) {
        GTEXT_CSS_Sheet * extra = gtext_css_sheet_retain(sheet);
        if (answers(doc, index) != want) {
          bad++;
        }
        gtext_css_sheet_free(extra);
      }
    });
  }
  for (auto & t : threads) {
    t.join();
  }
  EXPECT_EQ(bad.load(), 0);
  gtext_css_rule_index_free(index);
  gtext_css_sheet_free(sheet);
  gtext_markup_document_free(doc);
}
