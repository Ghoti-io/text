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
 * Out of memory in the matcher and the Markup queries: an allocation failed at each place in turn is GTEXT_MARKUP_E_OOM (or the selector layer's), with no result, nothing leaked
 * and nothing freed that the allocator did not make; the first call that succeeds returns the answer of a call that was never failed. The same for the parser and the serializer
 * of selectors.
 */

#include <string>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>
#include <ghoti.io/text/markup/markup_query.h>

#include "css_match_test_support.h"
#include "markup_test_support.h"

namespace {

const char * const kHtml = "<!DOCTYPE html><div id=\"r\" class=\"a\"><ul><li class=\"x\" id=\"l1\">1</li><li id=\"l2\">2</li><li class=\"x\" id=\"l3\">3</li></ul><p id=\"p\">t<b>b</b></p></div>";

}  // namespace

TEST(CssMatchOom, EveryQueryFailsCleanlyAtEveryAllocationAndThenAnswersAsIfNeverFailed) {
  cmatch::Doc d;
  ASSERT_TRUE(d.html(kHtml));
  const char * const selectors[] = {"li", ".x ~ li", "div li:nth-child(odd)", "ul:has(> li.x + li)", ":is(ul, ol) > li:nth-child(2n+1 of .x)", "p b", "li:not(.x):last-child",
      "div:has(li ~ li ~ li)", ":lang(en)"};
  for (const char * sel : selectors) {
    const std::string want = cmatch::all(d.root(), sel);
    ASSERT_EQ(want.find('!'), std::string::npos) << sel;
    bool answered = false;
    for (size_t n = 1; n < 4000 && !answered; n++) {
      FaultAllocator fa;
      GTEXT_Markup_Query_Options o = gtext_markup_query_options_default();
      o.allocator = &fa.alloc;
      GTEXT_Markup_Node ** out = nullptr;
      size_t count = 0;
      GTEXT_Markup_Query_Result r;
      std::memset(&r, 0, sizeof r);
      fa.arm(n);
      const GTEXT_Markup_Status st = gtext_markup_query_selector_all(d.root(), sel, std::strlen(sel), &o, &out, &count, &r);
      const bool failed = fa.failed;
      fa.disarm();
      if (st == GTEXT_MARKUP_OK) {
        EXPECT_FALSE(failed) << sel << " at " << n << ": an allocation failed and the call did not say so";
        std::string got;
        for (size_t i = 0; i < count; i++) {
          got += (i ? " " : "") + cmatch::id_of(out[i]);
        }
        EXPECT_EQ(got, want) << sel << " at " << n;
        answered = true;
      }
      else {
        EXPECT_EQ(st, GTEXT_MARKUP_E_OOM) << sel << " at " << n;
        EXPECT_EQ(out, nullptr);
      }
      gtext_markup_query_free(&o, out);
      gtext_markup_query_result_free(&r);
      EXPECT_TRUE(fa.live.empty()) << sel << " at " << n << ": " << fa.live.size() << " blocks leaked";
      EXPECT_EQ(fa.bad_frees, 0u);
    }
    EXPECT_TRUE(answered) << sel << ": a call with no failure in its first 4000 allocations";
  }
}

TEST(CssMatchOom, TheParserAndTheSerializerFailCleanlyToo) {
  const char * const selectors[] = {"a > b + c ~ d", ":is(.a, ::bogus):not(:has(> b))", "::part(a b):hover", ":nth-child(2n+1 of li.x, p)", "[a|=b i], svg|x"};
  for (const char * sel : selectors) {
    bool done = false;
    for (size_t n = 1; n < 2000 && !done; n++) {
      FaultAllocator fa;
      GTEXT_CSS_Selector_Options o = gtext_css_selector_options_default();
      o.allocator = &fa.alloc;
      const GTEXT_CSS_Namespace ns[] = {{"svg", "urn:svg"}};
      GTEXT_CSS_Namespace_Map map{ns, 1, nullptr};
      o.namespaces = &map;
      GTEXT_CSS_Selector_List * l = nullptr;
      GTEXT_CSS_Selector_Result r;
      std::memset(&r, 0, sizeof r);
      fa.arm(n);
      const GTEXT_CSS_Status st = gtext_css_selector_parse(sel, std::strlen(sel), &o, &l, &r);
      fa.disarm();
      if (st == GTEXT_CSS_OK) {
        ASSERT_NE(l, nullptr) << sel;
        char * buf = nullptr;
        size_t len = 0;
        for (size_t m = 1; m < 200; m++) {
          fa.arm(m);
          const GTEXT_CSS_Status ss = gtext_css_selector_serialize_buffer(l, &fa.alloc, &buf, &len);
          const bool f = fa.failed;
          fa.disarm();
          if (ss == GTEXT_CSS_OK) {
            EXPECT_FALSE(f);
            gtext_css_selector_buffer_free(&fa.alloc, buf);
            break;
          }
          EXPECT_EQ(ss, GTEXT_CSS_E_OOM);
        }
        gtext_css_selector_list_free(l);
        done = true;
      }
      else {
        EXPECT_EQ(st, GTEXT_CSS_E_OOM) << sel << " at " << n;
        EXPECT_EQ(l, nullptr);
      }
      gtext_css_selector_result_free(&r);
      EXPECT_TRUE(fa.live.empty()) << sel << " at " << n;
      EXPECT_EQ(fa.bad_frees, 0u);
    }
    EXPECT_TRUE(done) << sel;
  }
}
