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
 * What insertRule, deleteRule and the writers cost, asserted on counters and on the shape of the rule list and not on a clock: n inserts and deletes at any places of a sheet
 * of n rules make n rules and keep the list's treap logarithmic; the order constraints are decided from counts and never by a walk (a sheet of imports and a hundred thousand
 * calls); a chain of rules nested to the limit takes an insertion in O(depth); the text of a leaf is read from the cache after its first writing.
 */

#include <algorithm>
#include <random>
#include <string>

#include <gtest/gtest.h>

#include "cssom_test_support.h"

using namespace cot;

namespace {

size_t log2_ceil(size_t n) {
  size_t k = 0;
  while ((size_t(1) << k) < n) {
    k++;
  }
  return k;
}

// Inserting n rules at random places of a sheet makes n rules (the counter of the work), and the treap stays within a few times log n deep.
TEST(CssomInsertCost, NInsertsMakeNRulesAndTheListStaysLogarithmic) {
  for (size_t n : {1000u, 4000u, 16000u}) {
    Doc doc;
    Sheet s(doc, "");
    std::mt19937_64 rng(n);
    for (size_t i = 0; i < n; i++) {
      Ins r = insert_rule(s.s, "a{}", rng() % (i + 1));
      ASSERT_EQ(r.st, GTEXT_CSS_OK);
    }
    GTEXT_CSSOM_Counters c;
    gtext_cssom_document_counters(doc.d, &c);
    EXPECT_EQ(c.rule_inserts, n);
    EXPECT_EQ(gtext_cssom_sheet_rule_total(s.s), n);
    EXPECT_LE(css_rule_list_depth(&s.s->rules), 4 * log2_ceil(n) + 8) << n;
    // deleting them at random places leaves nothing
    for (size_t i = n; i > 0; i--) {
      ASSERT_EQ(delete_rule(s.s, rng() % i), GTEXT_CSS_OK);
    }
    EXPECT_EQ(gtext_cssom_rule_list_length(s.rules()), 0u);
    EXPECT_EQ(gtext_cssom_sheet_rule_total(s.s), 0u);
  }
}

// The order constraints of a sheet's top are counts kept in the list: a hundred thousand imports, and an insertion in the middle of them, a refusal of a style rule among them,
// and a namespace at the end, each decided without a walk. The test is that the whole run finishes in a time a walk (10^5 x 10^5) could not.
TEST(CssomInsertCost, TheOrderConstraintsAreDecidedFromCounts) {
  Doc doc;
  Sheet s(doc, "");
  const size_t n = 100000;
  for (size_t i = 0; i < n; i++) {
    ASSERT_EQ(insert_rule(s.s, "@import url(a.css);", i).st, GTEXT_CSS_OK);
  }
  std::mt19937_64 rng(7);
  for (size_t i = 0; i < n; i++) {
    const size_t at_index = rng() % (n + 1);
    EXPECT_EQ(insert_rule(s.s, "a{}", at_index).st, at_index == n ? GTEXT_CSS_OK : GTEXT_CSS_E_HIERARCHY) << at_index;
    if (at_index == n) {
      ASSERT_EQ(delete_rule(s.s, n), GTEXT_CSS_OK);
    }
  }
  ASSERT_EQ(insert_rule(s.s, "@namespace x url(u);", n).st, GTEXT_CSS_OK);
  EXPECT_EQ(insert_rule(s.s, "@namespace x url(u);", 5).st, GTEXT_CSS_E_HIERARCHY);
}

// An insertion into a rule nested to the limit costs the depth: the counts of the rules above are updated, one step each.
TEST(CssomInsertCost, AnInsertionIntoADeepRuleIsOrderOfTheDepth) {
  Doc doc([](GTEXT_CSSOM_Config & c) { c.max_rule_nesting = 2048; });
  std::string text;
  for (int i = 0; i < 2000; i++) {
    text += "a{";
  }
  for (int i = 0; i < 2000; i++) {
    text += "}";
  }
  Sheet s(doc, text);
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  Rule top = at(s.rules(), 0);
  EXPECT_EQ(top.r->descendants, 1999u);
  Rule cur = Rule(gtext_cssom_rule_retain(top.r));
  for (int i = 0; i < 1999; i++) {
    Rule next = at(gtext_cssom_rule_css_rules(cur.r), 0);
    cur = std::move(next);
  }
  for (int i = 0; i < 40; i++) {
    ASSERT_EQ(insert_rule(cur.r, "b{}", 0).st, GTEXT_CSS_OK);
  }
  EXPECT_EQ(top.r->descendants, 2039u);
  EXPECT_EQ(gtext_cssom_sheet_rule_total(s.s), 2040u);
  EXPECT_EQ(insert_rule(cur.r, "c{}", 0).st, GTEXT_CSS_OK) << "2001 deep";
  Rule b = at(gtext_cssom_rule_css_rules(cur.r), 0);
  EXPECT_EQ(insert_rule(b.r, "c{}", 0).st, GTEXT_CSS_OK);
}

// The text of a leaf is written once and read from the cache after: n reads of the same rule write it once.
TEST(CssomInsertCost, ALeafIsWrittenOnceForManyReads) {
  Doc doc;
  Sheet s(doc, "a{color:red; margin:1px 2px 3px 4px; background:url(a) no-repeat}");
  Rule a = at(s.rules(), 0);
  for (int i = 0; i < 10000; i++) {
    ASSERT_NE(css(a.r), "");
  }
  GTEXT_CSSOM_Counters c;
  gtext_cssom_document_counters(doc.d, &c);
  EXPECT_EQ(c.cache_fills, 1u);
  EXPECT_EQ(c.cache_hits, 9999u);
}

// A namespace deletion rebuilds the environment from the namespace rules only, found by the counts: deleting each of a few thousand is bounded by their number squared.
TEST(CssomInsertCost, DeletingNamespacesIsBoundedByTheirNumber) {
  Doc doc;
  Sheet s(doc, "");
  const size_t n = 2000;
  for (size_t i = 0; i < n; i++) {
    ASSERT_EQ(insert_rule(s.s, "@namespace p" + std::to_string(i) + " url(u" + std::to_string(i) + ");", i).st, GTEXT_CSS_OK);
  }
  for (size_t i = 0; i < n; i++) {
    ASSERT_EQ(delete_rule(s.s, 0), GTEXT_CSS_OK);
  }
  EXPECT_EQ(gtext_cssom_sheet_rule_total(s.s), 0u);
}

} // namespace
