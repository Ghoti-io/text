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
 * Allocation failure in the rule and sheet objects: the parse of a sheet, the first ask for a rule's declaration block, and a compaction, each swept over the n-th allocation
 * for every n up to the last one the call makes. A parse that fails fails with GTEXT_CSS_E_OOM and leaves nothing (no object, no byte counted, no allocation held once the
 * document is released); one that is not reached by the failure gives the sheet the unswept parse gives, byte for byte in its dump; a block that could not be built leaves the
 * rule as it was, and the next ask builds it.
 */

#include <string>

#include <gtest/gtest.h>

#include "cssom_test_support.h"

using namespace cot;

namespace {

const char * const kTexts[] = {
    "a{color:red} b > c{margin:1px 2px} @media print{ d{width:1px} @supports (a:b){ e{} } }",
    "@import url(a.css); @namespace s url(urn:s); s|a{ b{} color:red; c{} width:1px } @keyframes k{ from{a:b} 50%{} } @page :first{ margin:1px; @top-left{ content:\"x\" } }",
    "@import url(a.css) screen and ( a ), print; @media screen and ( a ) , (b/**/c) { x{} @container  foo ( a:  b ) { y{} } @media ( q ) { z{} } }",
    "@charset \"utf-8\"; @bogus; x >{} @layer a, b; @layer c{ z{} } @font-face{font-family:q;src:url(a)} @scope (a){ b{} }",
};

TEST(CssomRulesOom, TheParseOfASheetIsAllOrNothing) {
  for (const char * text : kTexts) {
    std::string want;
    {
      FaultAllocator fa;
      Doc doc(nullptr, &fa.alloc);
      Sheet s(doc, text);
      ASSERT_EQ(s.status, GTEXT_CSS_OK) << text;
      want = dump(s.s);
    }
    size_t failures = 0;
    for (size_t n = 1; n < 100000; n++) {
      FaultAllocator fa;
      {
        Doc doc(nullptr, &fa.alloc);
        fa.arm(n);
        Sheet s(doc, text);
        fa.disarm();
        if (!fa.failed) {
          ASSERT_EQ(s.status, GTEXT_CSS_OK);
          EXPECT_EQ(dump(s.s), want) << text << ": the sweep ended at " << n;
          break;
        }
        failures++;
        ASSERT_EQ(s.status, GTEXT_CSS_E_OOM) << text << ": allocation " << n << " " << gtext_css_status_string(s.status);
        EXPECT_EQ(s.s, nullptr);
        GTEXT_CSSOM_Counters c;
        ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c), GTEXT_CSS_OK);
        EXPECT_EQ(c.live_objects, 0u) << n;
        EXPECT_EQ(c.live_bytes, 0u) << n;
        EXPECT_EQ(c.tree_bytes, 0u) << n;
      }
      EXPECT_TRUE(fa.live.empty()) << text << ": allocation " << n << " leaked";
      EXPECT_EQ(fa.bad_frees, 0u);
    }
    EXPECT_GT(failures, 20u) << text;
  }
}

TEST(CssomRulesOom, TheFirstAskForABlockLeavesTheRuleAsItWasWhenItFails) {
  size_t failures = 0;
  for (size_t n = 1; n < 1000; n++) {
    FaultAllocator fa;
    {
      Doc doc(nullptr, &fa.alloc);
      Sheet s(doc, "a{color:red; margin:1px; voice-family:male; font:caption} b{}");
      ASSERT_EQ(s.status, GTEXT_CSS_OK);
      Rule a = at(s.rules(), 0);
      const std::string before = dump(a.r);
      GTEXT_CSSOM_Counters c0;
      ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c0), GTEXT_CSS_OK);
      fa.arm(n);
      GTEXT_CSS_Status st = GTEXT_CSS_OK;
      Decl d = style_of(a.r, &st);
      fa.disarm();
      if (!fa.failed) {
        ASSERT_EQ(st, GTEXT_CSS_OK);
        EXPECT_EQ(text_of(d.d), "color: red; margin: 1px; voice-family: male; font: caption;");
        break;
      }
      failures++;
      EXPECT_EQ(st, GTEXT_CSS_E_OOM);
      EXPECT_EQ(d.d, nullptr);
      EXPECT_EQ(dump(a.r), before) << "the rule is as it was";
      GTEXT_CSSOM_Counters c1;
      ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c1), GTEXT_CSS_OK);
      EXPECT_EQ(c1.live_objects, c0.live_objects);
      EXPECT_EQ(c1.live_bytes, c0.live_bytes);
      Decl again = style_of(a.r, &st);
      EXPECT_EQ(st, GTEXT_CSS_OK) << "and the next ask builds it";
      EXPECT_EQ(text_of(again.d), "color: red; margin: 1px; voice-family: male; font: caption;");
    }
    EXPECT_TRUE(fa.live.empty()) << n;
    EXPECT_EQ(fa.bad_frees, 0u);
  }
  EXPECT_GT(failures, 3u);
}

} // namespace
