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
 * The cost of the document layer, asserted on the document's counters and not on a clock (css_cssom_document.h). Two shapes of the index: *n mutations and then one ask* is one build, whatever n
 * is; *n times one mutation and one ask* is n builds of S records each, n x S, which is the known quadratic cost of C5 (C6's incremental update removes it), and the compile it makes is of
 * the one sheet that was edited, not of the document. A hit looks at the listed sheets and builds nothing. An environment change evaluates a query once however many lists ask it.
 */

#include <string>

#include <gtest/gtest.h>

#include "cssom_document_test_support.h"

using namespace cod;

namespace {

/// A world whose document scope lists `sheets` sheets of `rules` style rules each.
struct Big {
  World w;
  std::vector<std::unique_ptr<Owned>> owned;
  Big(size_t sheets, size_t rules) {
    for (size_t i = 0; i < sheets; i++) {
      std::string text;
      for (size_t r = 0; r < rules; r++) {
        text += "p.c" + std::to_string(i) + "_" + std::to_string(r) + "{color:red}";
      }
      owned.push_back(std::make_unique<Owned>(w, text));
    }
  }
};

TEST(CssomDocumentCost, ManyMutationsAndOneAskAreOneBuildWhateverTheNumberIs) {
  for (size_t n : {10u, 100u, 1000u}) {
    Big big(20, 10);
    big.w.index();
    const GTEXT_CSSOM_Counters c0 = big.w.counters();
    for (size_t i = 0; i < n; i++) {
      ASSERT_EQ(insert_rule(big.owned[i % 20]->s, "q" + std::to_string(i) + "{width:1px}", 0).st, GTEXT_CSS_OK);
    }
    ASSERT_EQ(big.w.counters().index_builds, c0.index_builds) << "no build before the ask";
    ASSERT_EQ(big.w.counters().snapshot_compiles, c0.snapshot_compiles) << "and no compile";
    const GTEXT_CSS_Rule_Index * x = big.w.index();
    const GTEXT_CSSOM_Counters c1 = big.w.counters();
    EXPECT_EQ(c1.index_builds, c0.index_builds + 1) << n;
    EXPECT_EQ(c1.snapshot_compiles, c0.snapshot_compiles + std::min<size_t>(n, 20)) << "each edited sheet is compiled once, however often it was edited: " << n;
    EXPECT_EQ(c1.index_records_rebuilt - c0.index_records_rebuilt, 200u + n) << "O(n + S): the records of the document, once";
    EXPECT_EQ(gtext_css_rule_index_record_count(x), 200u + n);
  }
}

TEST(CssomDocumentCost, AlternatingAnEditAndAnAskIsNTimesTheDocumentAndCompilesOnlyTheEditedSheet) {
  const size_t S = 20, R = 10;
  for (size_t n : {10u, 40u}) {
    Big big(S, R);
    big.w.index();
    const GTEXT_CSSOM_Counters c0 = big.w.counters();
    for (size_t i = 0; i < n; i++) {
      ASSERT_EQ(insert_rule(big.owned[3]->s, "q" + std::to_string(i) + "{width:1px}", 0).st, GTEXT_CSS_OK);
      big.w.index();
    }
    const GTEXT_CSSOM_Counters c1 = big.w.counters();
    EXPECT_EQ(c1.index_builds - c0.index_builds, n) << "one build for each edit";
    EXPECT_EQ(c1.snapshot_compiles - c0.snapshot_compiles, n) << "of the sheet that was edited: the other " << S - 1 << " answer from their caches";
    uint64_t want = 0;
    for (size_t i = 1; i <= n; i++) {
      want += S * R + i;
    }
    EXPECT_EQ(c1.index_records_rebuilt - c0.index_records_rebuilt, want) << "n x S: the known quadratic shape of C5, which C6's incremental update removes";
  }
}

TEST(CssomDocumentCost, AnAskThatFindsTheIndexLooksAtTheListedSheetsAndBuildsNothing) {
  Big big(30, 2);
  big.w.index();
  const GTEXT_CSSOM_Counters c0 = big.w.counters();
  for (int i = 0; i < 50; i++) {
    big.w.index();
  }
  const GTEXT_CSSOM_Counters c1 = big.w.counters();
  EXPECT_EQ(c1.index_hits - c0.index_hits, 50u);
  EXPECT_EQ(c1.index_builds, c0.index_builds);
  EXPECT_EQ(c1.snapshot_compiles, c0.snapshot_compiles);
  EXPECT_EQ(c1.index_key_sheets - c0.index_key_sheets, 50u * 30u) << "the sheets that are listed, once each, for each ask";
}

TEST(CssomDocumentCost, AnEnvironmentChangeEvaluatesEachDistinctQueryOnceHoweverManyListsAskIt) {
  Cond a(1000), b(500);
  World w([&](GTEXT_CSSOM_Config & c) { c.conditions = a.c; });
  const size_t lists = 2000, distinct = 8;
  std::vector<GTEXT_CSSOM_Media_Query_List *> held;
  size_t called = 0;
  for (size_t i = 0; i < lists; i++) {
    const std::string q = "(min-width: " + std::to_string(100 + (i % distinct) * 200) + "px)";
    GTEXT_CSSOM_Media_Query_List * l = nullptr;
    ASSERT_EQ(gtext_cssom_match_media(w.d(), q.data(), q.size(), &l), GTEXT_CSS_OK);
    ASSERT_EQ(gtext_cssom_media_query_list_add_listener(l, [](void * u, GTEXT_CSSOM_Media_Query_List *, bool) {
                ++*static_cast<size_t *>(u);
                return GTEXT_CSS_OK;
              }, &called), GTEXT_CSS_OK);
    held.push_back(l);
  }
  const uint64_t e0 = w.counters().mql_evaluations;
  ASSERT_EQ(gtext_cssom_document_set_conditions(w.d(), b.c, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(w.counters().mql_evaluations - e0, distinct) << "one evaluation for each distinct text, not for each list";
  // 100, 300, 500, 700, ...: at 1000 the first five are true, at 500 the first three (100, 300, 500) are: two changed, 2000 / 8 lists each
  EXPECT_EQ(called, 2u * (lists / distinct));
  for (auto * l : held) {
    gtext_cssom_media_query_list_release(l);
  }
}

TEST(CssomDocumentCost, TheIndexOfManySheetsIsBuiltInTimeLinearInTheirRecords) {
  Big big(2000, 1);
  const GTEXT_CSS_Rule_Index * x = big.w.index();
  EXPECT_EQ(gtext_css_rule_index_record_count(x), 2000u);
  EXPECT_EQ(gtext_css_rule_index_sheet_count(x), 2000u);
  const GTEXT_CSSOM_Counters c = big.w.counters();
  EXPECT_EQ(c.snapshot_compiles, 2000u);
  EXPECT_EQ(c.index_sheets_added, 2000u);
  EXPECT_EQ(c.index_records_rebuilt, 2000u);
}

}  // namespace
