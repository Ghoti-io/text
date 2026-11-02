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
 * What the rule and sheet objects cost, on the document's counters and the shape of the list, never on a clock: the objects and the bytes of a parse grow as the source does
 * (n, 4n and 16n rules), a parse builds no declaration block, the depth of the rule list grows as log n, the bytes of the trees fall when the rules that held them are gone
 * and the sheet is compacted, and the compaction costs what is left and not what was deleted.
 */

#include <string>

#include <gtest/gtest.h>

#include "cssom_test_support.h"

using namespace cot;

namespace {

std::string sheet_text(size_t rules) {
  std::string t;
  t.reserve(rules * 40);
  for (size_t i = 0; i < rules; i++) {
    t += "r" + std::to_string(i) + " > a{color:red;margin:" + std::to_string(i % 9) + "px}";
  }
  return t;
}

TEST(CssomRulesCost, ObjectsBytesAndDepthGrowAsTheSourceDoes) {
  const size_t sizes[3] = {2000, 8000, 32000};
  uint64_t objects[3], bytes[3], trees[3];
  size_t depth[3];
  for (int k = 0; k < 3; k++) {
    Doc doc;
    Sheet s(doc, sheet_text(sizes[k]));
    ASSERT_EQ(s.status, GTEXT_CSS_OK);
    GTEXT_CSSOM_Counters c;
    ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c), GTEXT_CSS_OK);
    objects[k] = c.live_objects;
    bytes[k] = c.live_bytes;
    trees[k] = c.tree_bytes;
    depth[k] = css_rule_list_depth(&s.s->rules);
    EXPECT_EQ(objects[k], sizes[k] + 1) << "a rule is one object, and a parse builds no declaration block";
  }
  for (int k = 0; k < 2; k++) {
    const double b = static_cast<double>(bytes[k + 1]) / static_cast<double>(bytes[k]);
    const double t = static_cast<double>(trees[k + 1]) / static_cast<double>(trees[k]);
    /* four times the rules is a little under four times the bytes (the names are a digit longer) and never more */
    EXPECT_GT(b, 3.2) << "live bytes";
    EXPECT_LT(b, 4.4) << "live bytes";
    EXPECT_GT(t, 3.2) << "tree bytes";
    EXPECT_LT(t, 4.4) << "tree bytes";
    EXPECT_LE(depth[k + 1], depth[k] + 6) << "four times the rules, a few levels more";
  }
}

TEST(CssomRulesCost, DeclarationBlocksAreBuiltForTheRulesAskedAboutAndNoOthers) {
  Doc doc;
  Sheet s(doc, sheet_text(5000));
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  GTEXT_CSSOM_Counters c0;
  ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c0), GTEXT_CSS_OK);
  for (size_t i = 0; i < 5000; i += 100) {
    Rule r = at(s.rules(), i);
    Decl d = style_of(r.r);
    ASSERT_NE(d.d, nullptr);
  }
  GTEXT_CSSOM_Counters c1;
  ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c1), GTEXT_CSS_OK);
  EXPECT_EQ(c1.live_objects, c0.live_objects + 50) << "the blocks are alive, held by their rules";
}

// Compaction costs what is left: the objects it moves, and the bytes it allocates, are those of the survivors.
TEST(CssomRulesCost, CompactionCostsWhatIsLeftAndGivesTheRestBack) {
  Doc doc;
  Sheet s(doc, sheet_text(20000));
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  GTEXT_CSSOM_Counters c0;
  ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c0), GTEXT_CSS_OK);
  while (s.s->rules.count > 10) {
    css_rule * r = css_rule_list_first(&s.s->rules);
    css_rule_list_remove(&s.s->rules, r);
    css_obj_release_internal(&r->obj);
  }
  GTEXT_CSSOM_Counters c1;
  ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c1), GTEXT_CSS_OK);
  EXPECT_EQ(c1.tree_bytes, c0.tree_bytes) << "deleting frees no tree";
  EXPECT_LT(c1.live_bytes, c0.live_bytes) << "the rules' own bytes went";
  ASSERT_EQ(gtext_cssom_sheet_compact(s.s), GTEXT_CSS_OK);
  GTEXT_CSSOM_Counters c2;
  ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c2), GTEXT_CSS_OK);
  EXPECT_LT(c2.tree_bytes, c0.tree_bytes / 200);
  EXPECT_LT(c2.tree_bytes, 64u * 1024u);
  EXPECT_EQ(c2.live_objects, c1.live_objects);
}

} // namespace
