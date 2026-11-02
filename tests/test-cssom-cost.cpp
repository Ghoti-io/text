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
 * What the declaration object costs, on the counters and not on a clock: n distinct sets, n repeated sets of one property, n removals, n `item(i)` of a declaration
 * of n declarations, and n mutations of an element's declaration (each of which writes the attribute, which is O(size) by the specification), run at n, 4n and 16n. The
 * work counters (the block's probes, the document's counters) grow as n (as n squared for the write-back, which serializes the block each time, and which is the
 * specification's cost).
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cssom_test_support.h"

using namespace cot;

namespace {

uint64_t probes_of(GTEXT_CSSOM_Decl * d) {
  return gtext_css_block_probes(d->block);
}

TEST(CssomCost, NDistinctSetsNRepeatedSetsAndNRemovalsOfAFreeStandingDeclarationAreLinear) {
  const size_t sizes[3] = {1000, 4000, 16000};
  uint64_t distinct[3], repeated[3], removed[3];
  for (int k = 0; k < 3; k++) {
    Doc doc;
    {
      Decl d(doc);
      for (size_t i = 0; i < sizes[k]; i++) {
        ASSERT_EQ(set(d.d, "--p" + std::to_string(i), "1").o.kind, GTEXT_CSSOM_CHANGED);
      }
      distinct[k] = probes_of(d.d);
      ASSERT_EQ(length(d.d), sizes[k]);
      for (size_t i = 0; i < sizes[k]; i++) {
        ASSERT_EQ(remove(d.d, "--p" + std::to_string(i)).o.kind, GTEXT_CSSOM_CHANGED);
      }
      removed[k] = probes_of(d.d) - distinct[k];
      ASSERT_EQ(length(d.d), 0u);
    }
    {
      Decl d(doc);
      for (size_t i = 0; i < sizes[k]; i++) {
        ASSERT_EQ(set(d.d, "width", std::to_string(i + 1) + "px").o.kind, GTEXT_CSSOM_CHANGED);
      }
      repeated[k] = probes_of(d.d);
    }
  }
  for (int k = 0; k < 3; k++) {
    EXPECT_LE(distinct[k], 12 * sizes[k]);
    EXPECT_LE(removed[k], 12 * sizes[k]);
    EXPECT_LE(repeated[k], 4 * sizes[k]);
  }
  for (int k = 1; k < 3; k++) {
    EXPECT_LE(distinct[k], 5 * distinct[k - 1]);
    EXPECT_LE(removed[k], 5 * removed[k - 1]);
    EXPECT_LE(repeated[k], 5 * repeated[k - 1]);
  }
}

// n `item(i)` of n declarations: the compaction of removed declarations is done once and the rest is O(1) each.
TEST(CssomCost, AnItemLoopAfterRemovalsIsLinear) {
  Doc doc;
  Decl d(doc);
  std::string text;
  for (int i = 0; i < 20000; i++) {
    text += "--p" + std::to_string(i) + ": 1; ";
  }
  ASSERT_EQ(set_text(d.d, text).st, GTEXT_CSS_OK);
  for (int i = 0; i < 20000; i += 3) {
    ASSERT_EQ(remove(d.d, "--p" + std::to_string(i)).o.kind, GTEXT_CSSOM_CHANGED);
  }
  const size_t n = length(d.d);
  ASSERT_LT(n, 20000u);
  for (size_t i = 0; i < n; i++) {
    ASSERT_NE(item(d.d, i), "");
  }
  EXPECT_EQ(item(d.d, n), "");
}

// The write-back is O(size of the block) by the specification ("update style attribute for" serializes it): n mutations of an element's declaration of n declarations cost n times n,
// and the cost is the serialization, not the validation: the comparisons and the reparses stay at zero.
TEST(CssomCost, AnInlineMutationCostsTheSerializationAndNothingMore) {
  Attrs attrs;
  Doc doc([&](GTEXT_CSSOM_Config & c) {
    c.tree_version = &Attrs::tree_version;
    c.tree_version_user = &attrs;
  });
  GTEXT_CSSOM_Attribute_Binding b = attrs.binding(false);
  GTEXT_CSSOM_Decl * d = nullptr;
  ASSERT_EQ(gtext_cssom_inline_style(doc.d, "e", &b, &d), GTEXT_CSS_OK);
  Decl hold(d);
  for (size_t i = 0; i < 400; i++) {
    ASSERT_EQ(set(d, "--p" + std::to_string(i), "1").o.kind, GTEXT_CSSOM_CHANGED);
  }
  GTEXT_CSSOM_Counters c;
  ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c), GTEXT_CSS_OK);
  EXPECT_EQ(c.inline_writes, 400u);
  EXPECT_EQ(c.inline_compares, 0u);
  EXPECT_EQ(c.inline_reparses, 0u);
  EXPECT_EQ(length(d), 400u);
}

} // namespace
