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
 * What a snapshot costs, asserted on the counters and not on a clock. Two shapes of the known cost of the CSSOM, which C6 removes and C5 does not hide:
 *
 * - **n mutations, then one ask** is O(n + S): one compile, however many mutations came before, and it reads the one changed rule from its text and the other S - 1 as they were.
 * - **alternate** (one mutation, one ask, n times) over S rules is n x S: n compiles, each over all S top-level rules, and only the one changed rule is parsed again (n parses, not n x S).
 *   The product is asserted exactly, as the ceiling `CSS_CSSOM_KNOWN_COST` of the C5e gate will only fall from.
 *
 * And the two things that keep the second from being worse: an unchanged big group is not read again when another rule changes, and a change in one sheet does not recompile another.
 */

#include <string>

#include <gtest/gtest.h>

#include "cssom_test_support.h"

using namespace cot;

namespace {

GTEXT_CSSOM_Counters counters(Doc & doc) {
  GTEXT_CSSOM_Counters c;
  EXPECT_EQ(gtext_cssom_document_counters(doc.d, &c), GTEXT_CSS_OK);
  return c;
}

std::string many(size_t n) {
  std::string text;
  for (size_t i = 0; i < n; i++) {
    text += "r" + std::to_string(i) + "{color:red}\n";
  }
  return text;
}

TEST(CssomSnapshotCost, NMutationsThenOneAskIsOneCompileOfOneChangedRule) {
  for (size_t n : {10u, 1000u, 20000u}) {
    Doc doc;
    const size_t s = 500;
    Sheet sheet(doc, many(s));
    { GTEXT_CSS_Sheet * warm = nullptr; ASSERT_EQ(gtext_cssom_sheet_snapshot(sheet.s, &warm), GTEXT_CSS_OK); gtext_css_sheet_free(warm); }
    const GTEXT_CSSOM_Counters before = counters(doc);
    Decl d = style_of(path(sheet, {s / 2}).r);
    for (size_t i = 0; i < n; i++) {
      ASSERT_EQ(set(d.d, "width", std::to_string(i % 91 + 1) + "px").st, GTEXT_CSS_OK);
    }
    GTEXT_CSS_Sheet * snap = nullptr;
    ASSERT_EQ(gtext_cssom_sheet_snapshot(sheet.s, &snap), GTEXT_CSS_OK);
    const GTEXT_CSSOM_Counters after = counters(doc);
    EXPECT_EQ(after.snapshot_compiles - before.snapshot_compiles, 1u) << n << " mutations";
    EXPECT_EQ(after.snapshot_rules_reparsed - before.snapshot_rules_reparsed, 1u);
    EXPECT_EQ(after.snapshot_rules_reused - before.snapshot_rules_reused, s - 1);
    gtext_css_sheet_free(snap);
  }
}

TEST(CssomSnapshotCost, AlternatingAnEditAndAnAskIsNTimesS) {
  const size_t s = 400, n = 50;
  Doc doc;
  Sheet sheet(doc, many(s));
  { GTEXT_CSS_Sheet * warm = nullptr; ASSERT_EQ(gtext_cssom_sheet_snapshot(sheet.s, &warm), GTEXT_CSS_OK); gtext_css_sheet_free(warm); }
  const GTEXT_CSSOM_Counters before = counters(doc);
  Decl d = style_of(path(sheet, {7}).r);
  for (size_t i = 0; i < n; i++) {
    ASSERT_EQ(set(d.d, "width", std::to_string(i + 1) + "px").st, GTEXT_CSS_OK);
    GTEXT_CSS_Sheet * snap = nullptr;
    ASSERT_EQ(gtext_cssom_sheet_snapshot(sheet.s, &snap), GTEXT_CSS_OK);
    gtext_css_sheet_free(snap);
  }
  const GTEXT_CSSOM_Counters after = counters(doc);
  EXPECT_EQ(after.snapshot_compiles - before.snapshot_compiles, n);
  EXPECT_EQ(after.snapshot_rules_reparsed - before.snapshot_rules_reparsed, n) << "the one changed rule is read from its text each time";
  EXPECT_EQ(after.snapshot_rules_reused - before.snapshot_rules_reused, n * (s - 1)) << "the n x S: each compile is over every rule of the sheet";
}

TEST(CssomSnapshotCost, AnUnchangedGroupIsNotReadAgainWhenAnotherRuleChanges) {
  Doc doc;
  std::string text = "@media print{";
  for (int i = 0; i < 5000; i++) {
    text += "g" + std::to_string(i) + "{color:red}";
  }
  text += "} z{color:blue}";
  Sheet sheet(doc, text);
  { GTEXT_CSS_Sheet * warm = nullptr; ASSERT_EQ(gtext_cssom_sheet_snapshot(sheet.s, &warm), GTEXT_CSS_OK); gtext_css_sheet_free(warm); }
  const GTEXT_CSSOM_Counters before = counters(doc);
  Decl d = style_of(path(sheet, {1}).r);
  ASSERT_EQ(set(d.d, "width", "1px").o.kind, GTEXT_CSSOM_CHANGED);
  GTEXT_CSS_Sheet * snap = nullptr;
  ASSERT_EQ(gtext_cssom_sheet_snapshot(sheet.s, &snap), GTEXT_CSS_OK);
  const GTEXT_CSSOM_Counters after = counters(doc);
  EXPECT_EQ(after.snapshot_rules_reparsed - before.snapshot_rules_reparsed, 1u) << "z, and not the group of 5,000 rules";
  EXPECT_EQ(after.snapshot_rules_reused - before.snapshot_rules_reused, 1u);
  gtext_css_sheet_free(snap);
}

TEST(CssomSnapshotCost, AChangeInOneSheetCompilesOnlyThatSheet) {
  Doc doc;
  Sheet a(doc, many(300));
  Sheet b(doc, many(300));
  GTEXT_CSS_Sheet *sa = nullptr, *sb = nullptr;
  ASSERT_EQ(gtext_cssom_sheet_snapshot(a.s, &sa), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_sheet_snapshot(b.s, &sb), GTEXT_CSS_OK);
  const GTEXT_CSSOM_Counters before = counters(doc);
  ASSERT_EQ(insert_rule(a.s, "x{color:red}", 0).st, GTEXT_CSS_OK);
  GTEXT_CSS_Sheet *sa2 = nullptr, *sb2 = nullptr;
  ASSERT_EQ(gtext_cssom_sheet_snapshot(a.s, &sa2), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_sheet_snapshot(b.s, &sb2), GTEXT_CSS_OK);
  const GTEXT_CSSOM_Counters after = counters(doc);
  EXPECT_EQ(after.snapshot_compiles - before.snapshot_compiles, 1u);
  EXPECT_EQ(after.snapshot_rules_reused - before.snapshot_rules_reused, 301u) << "the sheet that changed, and not the other";
  EXPECT_EQ(sb2, sb);
  gtext_css_sheet_free(sa);
  gtext_css_sheet_free(sb);
  gtext_css_sheet_free(sa2);
  gtext_css_sheet_free(sb2);
}

TEST(CssomSnapshotCost, ALargeSheetIsOneCompile) {
  Doc doc;
  std::string text;
  const size_t n = 50000;
  text.reserve(n * 14);
  for (size_t i = 0; i < n; i++) {
    text += "a{color:red}\n";
  }
  Sheet sheet(doc, text);
  ASSERT_EQ(sheet.status, GTEXT_CSS_OK);
  GTEXT_CSS_Sheet * snap = nullptr;
  ASSERT_EQ(gtext_cssom_sheet_snapshot(sheet.s, &snap), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_sheet_record_count(snap), n);
  gtext_css_sheet_free(snap);
}

TEST(CssomSnapshotCost, ADeepNestIsSnapshottedWithoutRecursionOnTheCStack) {
  Doc doc;
  // the document's default depth is 64: a nest of that depth, changed at the bottom, is read from its text and compiled
  std::string open, close;
  for (int i = 0; i < 30; i++) {
    open += "@media print{ ";
    close += " }";
  }
  Sheet sheet(doc, open + "a{color:red}" + close);
  ASSERT_EQ(sheet.status, GTEXT_CSS_OK);
  Rule r = path(sheet, {0});
  for (int i = 1; i < 30; i++) {
    r = at(gtext_cssom_rule_css_rules(r.r), 0);
  }
  r = at(gtext_cssom_rule_css_rules(r.r), 0);
  Decl d = style_of(r.r);
  ASSERT_EQ(set(d.d, "width", "1px").o.kind, GTEXT_CSSOM_CHANGED);
  GTEXT_CSS_Sheet * snap = nullptr;
  ASSERT_EQ(gtext_cssom_sheet_snapshot(sheet.s, &snap), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_sheet_record_count(snap), 1u);
  gtext_css_sheet_free(snap);
}

} // namespace
