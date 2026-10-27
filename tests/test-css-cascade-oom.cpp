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
 * Allocation failure in the cascade: a layer registry built (by hand and from a parsed sheet), a candidate set filled (a shorthand's expansion, `all`, custom
 * properties, dropped declarations with their diagnostics), cascaded (with the losers kept: the rolling-back walk and its sorts), written as a declaration
 * block and as the dump, each with the n-th allocation failed for every n up to the last one the run makes.
 *
 * A failed run is GTEXT_CSS_E_OOM with nothing returned, or the answer of the run that failed nothing. Either way the allocator holds no block
 * afterwards and was never asked to free one it did not make.
 */

#include <string>

#include <gtest/gtest.h>

#include "css_cascade_test_support.h"
#include "css_test_support.h"
#include "markup_test_support.h"

using namespace cas;

namespace {

/// One element's whole cascade under `fa`: the answer is the dump of the winners and the text of the block, and the losers.
GTEXT_CSS_Status whole(FaultAllocator & fa, const std::string & sheet, std::string * answer) {
  GTEXT_CSS_Layer_Options lo = gtext_css_layer_options_default();
  lo.allocator = &fa.alloc;
  GTEXT_CSS_Layer_Registry * layers = nullptr;
  GTEXT_CSS_Status st = gtext_css_layer_registry_new(&lo, &layers);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSS_Candidates * set = nullptr;
  GTEXT_CSS_Cascaded * c = nullptr;
  GTEXT_CSS_Declaration_Block * block = nullptr;
  css_test::Parsed parsed;
  css_test::parse_into(parsed, gtext_css_parse_stylesheet, sheet);
  if (parsed.status != GTEXT_CSS_OK) {
    gtext_css_layer_registry_free(layers);
    return parsed.status;
  }
  st = gtext_css_cascade_collect_layers(layers, GTEXT_CSS_ORIGIN_AUTHOR, 0, parsed.tree, nullptr, nullptr);
  if (st == GTEXT_CSS_OK) {
    st = gtext_css_layer_freeze(layers);
  }
  if (st == GTEXT_CSS_OK) {
    GTEXT_CSS_Candidate_Options co = gtext_css_candidate_options_default();
    co.allocator = &fa.alloc;
    st = gtext_css_candidates_new(R(), layers, &co, &set);
  }
  /* the declarations of the style rules of the sheet, in order, each rule in the layer of the block it is in: the sheet here is flat, so a rule's layer
     is named by the first word of its selector (`a.b { ... }` is in layer a.b, `x` is unlayered) */
  uint32_t order = 0;
  for (size_t r = 0; st == GTEXT_CSS_OK && r < parsed.tree->rule_count; r++) {
    const GTEXT_CSS_Rule & rule = parsed.tree->rules[r];
    if (rule.kind != GTEXT_CSS_RULE_QUALIFIED) {
      continue;
    }
    std::string sel = css_test::text_of(rule.prelude, rule.prelude_count);
    GTEXT_CSS_Layer_Id layer = 0;
    K k;
    if (sel.rfind("ident:x", 0) != 0) {
      std::string name;
      for (size_t i = 0; i < rule.prelude_count; i++) {
        if (rule.prelude[i].type == GTEXT_CSS_TOKEN_IDENT) {
          name += css_test::str(rule.prelude[i].value);
        }
        else if (rule.prelude[i].type == GTEXT_CSS_TOKEN_DELIM) {
          name += ".";
        }
      }
      layer = gtext_css_layer_find(layers, GTEXT_CSS_ORIGIN_AUTHOR, 0, name.data(), name.size());
    }
    k.layer(layer).order(0, r, 0).id(rule.id);
    k.e.specificity.b = (uint32_t)(r % 3);
    for (size_t d = 0; st == GTEXT_CSS_OK && d < rule.declaration_count; d++) {
      k.e.declaration = (uint32_t)d;
      GTEXT_CSS_Property_Outcome outcome;
      st = gtext_css_candidates_add_declaration(set, &k.e, &rule.declarations[d], nullptr, &outcome, nullptr);
      order++;
    }
  }
  if (st == GTEXT_CSS_OK) {
    GTEXT_CSS_Cascade_Options o{};
    o.allocator = &fa.alloc;
    o.keep_losers = true;
    st = gtext_css_cascade(set, &o, &c, nullptr);
  }
  if (st == GTEXT_CSS_OK) {
    GTEXT_CSS_Declaration_Options bo = gtext_css_declaration_options_default();
    bo.allocator = &fa.alloc;
    st = gtext_css_cascaded_to_block(c, &bo, &block);
  }
  if (st == GTEXT_CSS_OK) {
    std::string text;
    GTEXT_Markup_Sink sink;
    sink.user = &text;
    sink.write = [](void * u, const char * b, size_t n) -> GTEXT_Markup_Status {
      static_cast<std::string *>(u)->append(b, n);
      return GTEXT_MARKUP_OK;
    };
    st = gtext_css_cascaded_dump(c, &sink);
    if (st == GTEXT_CSS_OK) {
      char * css = nullptr;
      size_t n = 0;
      st = gtext_css_block_serialize_buffer(block, &css, &n);
      if (st == GTEXT_CSS_OK) {
        text += std::string(css, n) + "\n";
        gtext_css_serialize_buffer_free(nullptr, css);
      }
    }
    for (size_t i = 0; i < gtext_css_cascaded_count(c); i++) {
      text += std::to_string(gtext_css_cascaded_loser_count(c, i)) + ",";
    }
    uint64_t h = 0;
    if (st == GTEXT_CSS_OK) {
      st = gtext_css_cascaded_hash(c, &h);
    }
    if (st == GTEXT_CSS_OK) {
      *answer = text + std::to_string(h);
    }
  }
  gtext_css_block_free(block);
  gtext_css_cascaded_free(c);
  gtext_css_candidates_free(set);
  gtext_css_layer_registry_free(layers);
  return st;
}

const char * const kSheets[] = {
    // layers, a shorthand, a rolling keyword and a logical pair
    "@layer a, b.c;\n"
    "x { margin: 1px 2px; color: red; margin-inline-start: 5px }\n"
    "a { color: revert-layer; width: 10px; margin-left: revert-layer }\n"
    "b.c { color: blue !important; width: 20px }\n"
    "x { width: revert-layer; padding: 3px; --y: 1; --x: 2; --y: 3 }\n",
    // all, a custom property and dropped declarations
    "@layer q;\n"
    "x { all: initial; direction: rtl; margin-inline-start: 1px; nonsense: 1; color: not(a color) }\n"
    "q { all: revert-layer; margin-left: 2px; --a: 1 }\n"
    "x { font: italic 12px serif; --a: 2; height: var(--h) }\n",
    // a long chain
    "@layer l1, l2, l3, l4;\n"
    "l1 { width: 1px; height: 1px }\nl2 { width: revert-layer; height: 2px }\nl3 { width: revert-layer; height: revert-layer }\nl4 { width: revert-layer; "
    "height: revert-layer; color: revert }\nx { border: 1px solid red; border-top: revert-rule }\n",
};

TEST(CssCascadeOom, TheWholeCascadeOfAnElementSweptOverEveryAllocation) {
  for (const char * sheet : kSheets) {
    std::string want;
    {
      FaultAllocator fa;
      ASSERT_EQ(whole(fa, sheet, &want), GTEXT_CSS_OK) << sheet;
      EXPECT_TRUE(fa.live.empty());
      EXPECT_EQ(fa.bad_frees, 0u);
    }
    ASSERT_FALSE(want.empty()) << sheet;
    size_t failures = 0;
    for (size_t n = 1; n < 200000; n++) {
      FaultAllocator fa;
      std::string got;
      fa.arm(n);
      const GTEXT_CSS_Status st = whole(fa, sheet, &got);
      fa.disarm();
      if (!fa.failed) {
        EXPECT_EQ(st, GTEXT_CSS_OK) << sheet << " " << n;
        EXPECT_EQ(got, want) << sheet << " " << n;
        EXPECT_TRUE(fa.live.empty()) << sheet;
        break;
      }
      failures++;
      ASSERT_EQ(st, GTEXT_CSS_E_OOM) << sheet << ": allocation " << n << " " << gtext_css_status_string(st);
      ASSERT_TRUE(got.empty()) << n;
      ASSERT_TRUE(fa.live.empty()) << sheet << " " << n << ": a block is left";
      ASSERT_EQ(fa.bad_frees, 0u) << sheet << " " << n;
    }
    EXPECT_GT(failures, 60u) << sheet;
  }
}

TEST(CssCascadeOom, ReadingTheLayersOfASheetSweptOverEveryAllocation) {
  const std::string sheet =
      "@layer a, b.c, d;\n@layer { @layer e {} @layer e {} }\n@import url(x.css) layer(f.g);\n@import url(y.css) layer;\n@layer b.c { x {color: red} }\n"
      "@media print { @layer h {} }\n";
  css_test::Parsed parsed;
  css_test::parse_into(parsed, gtext_css_parse_stylesheet, sheet);
  ASSERT_EQ(parsed.status, GTEXT_CSS_OK);
  auto run = [&](FaultAllocator & fa, std::string * order) {
    GTEXT_CSS_Layer_Options lo = gtext_css_layer_options_default();
    lo.allocator = &fa.alloc;
    GTEXT_CSS_Layer_Registry * l = nullptr;
    GTEXT_CSS_Status st = gtext_css_layer_registry_new(&lo, &l);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    GTEXT_CSS_Layer_Collect_Options co;
    std::memset(&co, 0, sizeof co);
    co.active = [](void *, const GTEXT_CSS_Rule *) { return true; };
    st = gtext_css_cascade_collect_layers(l, GTEXT_CSS_ORIGIN_AUTHOR, 0, parsed.tree, &co, nullptr);
    if (st == GTEXT_CSS_OK) {
      st = gtext_css_layer_freeze(l);
    }
    if (st == GTEXT_CSS_OK) {
      order->clear();
      for (GTEXT_CSS_Layer_Id id = 1; id < 64; id++) {
        GTEXT_CSS_Layer_Info info;
        if (gtext_css_layer_info(l, id, &info)) {
          char buf[128];
          gtext_css_layer_path(l, id, buf, sizeof buf);
          *order += std::string(buf) + "=" + std::to_string(info.rank) + ";";
        }
      }
    }
    gtext_css_layer_registry_free(l);
    return st;
  };
  std::string want;
  {
    FaultAllocator fa;
    ASSERT_EQ(run(fa, &want), GTEXT_CSS_OK);
    EXPECT_TRUE(fa.live.empty());
  }
  size_t failures = 0;
  for (size_t n = 1; n < 100000; n++) {
    FaultAllocator fa;
    std::string got;
    fa.arm(n);
    const GTEXT_CSS_Status st = run(fa, &got);
    fa.disarm();
    if (!fa.failed) {
      EXPECT_EQ(st, GTEXT_CSS_OK);
      EXPECT_EQ(got, want);
      break;
    }
    failures++;
    ASSERT_EQ(st, GTEXT_CSS_E_OOM) << n;
    ASSERT_TRUE(fa.live.empty()) << n;
    ASSERT_EQ(fa.bad_frees, 0u);
  }
  EXPECT_GT(failures, 5u);
}

TEST(CssCascadeOom, AddingToASetKeepsNothingWhenAnAllocationFails) {
  for (const char * decl : {"margin: 1px 2px 3px 4px", "font: italic 12px serif", "all: initial", "--x: abc", "nonsense: 1", "color: not-a-color(", "margin: var(--m)"}) {
    const std::string text = decl;
    const size_t colon = text.find(':');
    const std::string name = text.substr(0, colon), value = text.substr(colon + 2);
    size_t want_count = 0;
    {
      FaultAllocator fa;
      GTEXT_CSS_Candidate_Options o = gtext_css_candidate_options_default();
      o.allocator = &fa.alloc;
      GTEXT_CSS_Candidates * s = nullptr;
      ASSERT_EQ(gtext_css_candidates_new(R(), nullptr, &o, &s), GTEXT_CSS_OK);
      K k;
      GTEXT_CSS_Property_Outcome outcome;
      ASSERT_EQ(gtext_css_candidates_add_text(s, &k.e, name.data(), name.size(), value.data(), value.size(), nullptr, &outcome, nullptr), GTEXT_CSS_OK);
      want_count = gtext_css_candidates_count(s);
      gtext_css_candidates_free(s);
      EXPECT_TRUE(fa.live.empty());
    }
    size_t failures = 0;
    for (size_t n = 1; n < 100000; n++) {
      FaultAllocator fa;
      GTEXT_CSS_Candidate_Options o = gtext_css_candidate_options_default();
      o.allocator = &fa.alloc;
      GTEXT_CSS_Candidates * s = nullptr;
      GTEXT_CSS_Status st = gtext_css_candidates_new(R(), nullptr, &o, &s);
      K k;
      GTEXT_CSS_Property_Outcome outcome;
      fa.arm(n);
      if (st == GTEXT_CSS_OK) {
        st = gtext_css_candidates_add_text(s, &k.e, name.data(), name.size(), value.data(), value.size(), nullptr, &outcome, nullptr);
      }
      fa.disarm();
      if (!fa.failed) {
        EXPECT_EQ(st, GTEXT_CSS_OK) << decl;
        EXPECT_EQ(gtext_css_candidates_count(s), want_count) << decl;
        gtext_css_candidates_free(s);
        EXPECT_TRUE(fa.live.empty());
        break;
      }
      failures++;
      ASSERT_EQ(st, GTEXT_CSS_E_OOM) << decl << " " << n << " " << gtext_css_status_string(st);
      EXPECT_EQ(gtext_css_candidates_count(s), 0u) << decl << " " << n << ": the set is as it was";
      gtext_css_candidates_free(s);
      ASSERT_TRUE(fa.live.empty()) << decl << " " << n;
      ASSERT_EQ(fa.bad_frees, 0u);
    }
    EXPECT_GT(failures, 3u) << decl;
  }
}

} // namespace
