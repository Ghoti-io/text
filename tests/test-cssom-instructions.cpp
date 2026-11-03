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
 * The shapes of the CSSOM cost claims, one test each, with their sizes multiplied by GTEXT_COST_SCALE (default 1). The counters say what the library did; this program exists so that
 * tools/css/cssom-instructions.py can run each shape under callgrind at two scales and compare what the machine did (the instructions), which no counter can be wrong about: n distinct
 * sets of a declaration, n inserts into a sheet, n mutations and one ask of the index, a getComputedStyle chain of depth d and n reads, n media query lists and one environment change,
 * and the one shape that is quadratic by design until the index is updated incrementally, an edit and an ask alternating (CSS_CSSOM_KNOWN_COST holds its ceiling).
 */

#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cssom_computed_test_support.h"
#include "cssom_document_test_support.h"

namespace {

size_t scale() {
  const char * s = std::getenv("GTEXT_COST_SCALE");
  const long v = s ? std::strtol(s, nullptr, 10) : 1;
  return v > 0 ? static_cast<size_t>(v) : 1;
}

}  // namespace

TEST(CssomInstructions, DistinctSetsOfADeclaration) {
  using namespace cot;
  const size_t n = 1500 * scale();
  Doc doc;
  Decl d(doc);
  for (size_t i = 0; i < n; i++) {
    ASSERT_EQ(set(d.d, "--p" + std::to_string(i), "1").o.kind, GTEXT_CSSOM_CHANGED);
  }
  EXPECT_EQ(length(d.d), n);
}

TEST(CssomInstructions, RepeatedSetsOfOneProperty) {
  using namespace cot;
  const size_t n = 3000 * scale();
  Doc doc;
  Decl d(doc);
  for (size_t i = 0; i < n; i++) {
    ASSERT_EQ(set(d.d, "width", std::to_string(i + 1) + "px").o.kind, GTEXT_CSSOM_CHANGED);
  }
  EXPECT_EQ(length(d.d), 1u);
}

TEST(CssomInstructions, InsertsIntoASheetAtRandomPlaces) {
  using namespace cot;
  const size_t n = 3000 * scale();
  Doc doc;
  Sheet s(doc, "");
  std::mt19937_64 rng(7);
  for (size_t i = 0; i < n; i++) {
    ASSERT_EQ(insert_rule(s.s, "a{}", rng() % (i + 1)).st, GTEXT_CSS_OK);
  }
  EXPECT_EQ(gtext_cssom_sheet_rule_total(s.s), n);
}

namespace {

struct Big {
  cod::World w;
  std::vector<std::unique_ptr<cod::Owned>> owned;
  Big(size_t sheets, size_t rules) {
    for (size_t i = 0; i < sheets; i++) {
      std::string text;
      for (size_t r = 0; r < rules; r++) {
        text += "p.c" + std::to_string(i) + "_" + std::to_string(r) + "{color:red}";
      }
      owned.push_back(std::make_unique<cod::Owned>(w, text));
    }
  }
};

}  // namespace

TEST(CssomInstructions, ManyMutationsThenOneAskOfTheIndex) {
  const size_t n = 800 * scale();
  Big big(20, 10);
  big.w.index();
  const GTEXT_CSSOM_Counters c0 = big.w.counters();
  for (size_t i = 0; i < n; i++) {
    ASSERT_EQ(cot::insert_rule(big.owned[i % 20]->s, "q" + std::to_string(i) + "{width:1px}", 0).st, GTEXT_CSS_OK);
  }
  big.w.index();
  EXPECT_EQ(big.w.counters().index_builds - c0.index_builds, 1u);
}

// The shape that is quadratic by design until the index is updated incrementally: n edits, each followed by an ask. Each ask rebuilds the whole index.
TEST(CssomInstructions, AlternatingAnEditAndAnAskIsTheKnownQuadraticShape) {
  const size_t n = 60 * scale();
  Big big(4, 5);   // a small document, so that the n x n of the rebuilds and not the setup is what the instructions count
  big.w.index();
  const GTEXT_CSSOM_Counters c0 = big.w.counters();
  for (size_t i = 0; i < n; i++) {
    ASSERT_EQ(cot::insert_rule(big.owned[3]->s, "q" + std::to_string(i) + "{width:1px}", 0).st, GTEXT_CSS_OK);
    big.w.index();
  }
  EXPECT_EQ(big.w.counters().index_builds - c0.index_builds, n);
}

TEST(CssomInstructions, AChainOfDepthDAndManyReads) {
  using namespace cc;
  const int depth = static_cast<int>(40 * scale());
  std::string html = "<!DOCTYPE html><html><head><style>div{color:rgb(1,2,3)}</style></head><body>";
  for (int i = 0; i < depth; i++) {
    html += "<div id=d" + std::to_string(i) + ">";
  }
  html += "<p id=leaf>x</p>";
  for (int i = 0; i < depth; i++) {
    html += "</div>";
  }
  html += "</body></html>";
  Cs m(html);
  const uint64_t c0 = m.dc().computed_computes;
  for (int i = 0; i < 100 * static_cast<int>(scale()); i++) {
    ASSERT_EQ(m.value("#leaf", "color"), "rgb(1, 2, 3)");
  }
  EXPECT_LE(m.dc().computed_computes - c0, static_cast<uint64_t>(depth) + 4) << "d + n computes";
}

TEST(CssomInstructions, ManyMediaQueryListsAndOneEnvironmentChange) {
  using namespace cod;
  Cond a(1000), b(500);
  World w([&](GTEXT_CSSOM_Config & c) { c.conditions = a.c; });
  const size_t lists = 600 * scale(), distinct = 8;
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
  ASSERT_EQ(gtext_cssom_document_set_conditions(w.d(), b.c, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(called, 2u * (lists / distinct));
  for (auto * l : held) {
    gtext_cssom_media_query_list_release(l);
  }
}
