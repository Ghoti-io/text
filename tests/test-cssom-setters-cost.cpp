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
 * What the setters and `replace()` cost, asserted on the counters and on the bytes the document holds and not on a clock: a setter on one rule of a sheet of 100,000 rules costs the
 * rule and not the sheet (no rule is made, the document holds no more bytes after a thousand of them than after one, since each replaces the syntax of the rule and lets the old go);
 * replacing the rules of a large sheet leaves the document holding the new rules and the new text and nothing of the old.
 */

#include <string>

#include <gtest/gtest.h>

#include "cssom_test_support.h"

using namespace cot;

TEST(CssomSettersCost, ASetterCostsTheRuleNotTheSheet) {
  Doc doc;
  std::string text;
  const size_t n = 100000;
  for (size_t i = 0; i < n; i++) {
    text += "a" + std::to_string(i) + "{color:red}";
  }
  Sheet s(doc, text);
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  Rule r = path(s, {n / 2});
  ASSERT_EQ(gtext_cssom_rule_set_selector_text(r.r, "b0", 2, nullptr), GTEXT_CSS_OK);
  GTEXT_CSSOM_Counters c0;
  ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c0), GTEXT_CSS_OK);
  for (size_t i = 1; i <= 1000; i++) {
    const std::string sel = "b" + std::to_string(i % 10);
    ASSERT_EQ(gtext_cssom_rule_set_selector_text(r.r, sel.data(), sel.size(), nullptr), GTEXT_CSS_OK);
  }
  GTEXT_CSSOM_Counters c1;
  ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c1), GTEXT_CSS_OK);
  EXPECT_EQ(c1.rule_inserts, c0.rule_inserts) << "no rule is made";
  EXPECT_EQ(c1.live_objects, c0.live_objects);
  EXPECT_LE(c1.live_bytes, c0.live_bytes + 64) << "the syntax of the old selectors is let go: a thousand setters hold what one does";
  EXPECT_EQ(gtext_cssom_sheet_rule_total(s.s), n);
  EXPECT_EQ(gtext_cssom_sheet_version(s.s), 1001u);
}

TEST(CssomSettersCost, ReplacingALargeSheetLeavesNothingOfTheOldOne) {
  Doc doc;
  GTEXT_CSSOM_Sheet * s = nullptr;
  ASSERT_EQ(gtext_cssom_sheet_new_constructed(doc.d, nullptr, &s), GTEXT_CSS_OK);
  std::string text;
  const size_t n = 100000;
  for (size_t i = 0; i < n; i++) {
    text += "a{color:red}";
  }
  ASSERT_EQ(gtext_cssom_sheet_replace_sync(s, text.data(), text.size(), nullptr), GTEXT_CSS_OK);
  GTEXT_CSSOM_Counters one;
  ASSERT_EQ(gtext_cssom_document_counters(doc.d, &one), GTEXT_CSS_OK);
  for (int k = 0; k < 3; k++) {
    ASSERT_EQ(gtext_cssom_sheet_replace_sync(s, text.data(), text.size(), nullptr), GTEXT_CSS_OK);
  }
  GTEXT_CSSOM_Counters many;
  ASSERT_EQ(gtext_cssom_document_counters(doc.d, &many), GTEXT_CSS_OK);
  EXPECT_EQ(many.live_objects, one.live_objects);
  EXPECT_LE(many.tree_bytes, one.tree_bytes + 64);
  EXPECT_LE(many.live_bytes, one.live_bytes + 4096);
  EXPECT_EQ(gtext_cssom_sheet_rule_total(s), n);
  gtext_cssom_sheet_release(s);
}
