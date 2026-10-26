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
 * Allocation failure in the computed-value layer: a declaration block built from text (the shorthand expansion), written back as cssText (the
 * shorthand re-serialization), a compute context made, and a style computed (a child of a parent, with relative units, math, a logical
 * property and `all`), each with the n-th allocation failed for every n up to the last one the call makes (the context, which makes tens of
 * thousands, with a stride that still reaches every kind of allocation of it).
 *
 * A failed run is GTEXT_CSS_E_OOM with nothing returned, or the answer of the run that failed nothing. Either way the allocator holds no block
 * afterwards and was never asked to free one it did not make.
 */

#include <string>

#include <gtest/gtest.h>

#include "css_compute_test_support.h"
#include "markup_test_support.h"

using namespace cmp;

namespace {

/// The cssText a block of `declarations` makes with the n-th allocation failed (n = 0: none), with the allocator the sweep checks. `failed` says whether
/// the allocation was reached. The cssText is the empty string with status OOM when the failure was seen.
GTEXT_CSS_Status build_and_write(const std::string & declarations, FaultAllocator & fa, std::string * css) {
  GTEXT_CSS_Declaration_Options o = gtext_css_declaration_options_default();
  o.allocator = &fa.alloc;
  GTEXT_CSS_Declaration_Block * b = nullptr;
  GTEXT_CSS_Status st = gtext_css_block_new(R(), &o, &b);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSS_Property_Options po;
  std::memset(&po, 0, sizeof po);
  po.allocator = &fa.alloc;
  size_t pos = 0;
  while (st == GTEXT_CSS_OK && pos < declarations.size()) {
    size_t semi = declarations.find(';', pos);
    if (semi == std::string::npos) {
      semi = declarations.size();
    }
    const std::string decl = declarations.substr(pos, semi - pos);
    pos = semi + 1;
    const size_t colon = decl.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    std::string name = decl.substr(0, colon), value = decl.substr(colon + 1);
    while (!name.empty() && name.front() == ' ') {
      name.erase(name.begin());
    }
    while (!value.empty() && value.front() == ' ') {
      value.erase(value.begin());
    }
    GTEXT_CSS_Property_Outcome outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
    st = gtext_css_block_add_text(b, name.data(), name.size(), value.data(), value.size(), false, &po, &outcome, nullptr);
    if (st == GTEXT_CSS_OK && outcome != GTEXT_CSS_PROP_RESULT_VALID && outcome != GTEXT_CSS_PROP_RESULT_WIDE && outcome != GTEXT_CSS_PROP_RESULT_PENDING) {
      st = GTEXT_CSS_E_INVALID;
    }
  }
  if (st == GTEXT_CSS_OK) {
    char * out = nullptr;
    size_t n = 0;
    st = gtext_css_block_serialize_buffer(b, &out, &n);
    if (st == GTEXT_CSS_OK) {
      css->assign(out, n);
      gtext_css_serialize_buffer_free(nullptr, out);
    }
  }
  gtext_css_block_free(b);
  return st;
}

const char * const kShorthands[] = {
    "margin: 1px 2px 3px; border: 2px solid red; background: url(a) 1px 2px / 3px 4px no-repeat, red",
    "font: italic bold 12px/1.5 serif; border-radius: 1px 2px / 3px; flex: 1 1 10px; list-style: inside square",
    "grid-template: [a] \"x y\" 1fr [b] \"z z\" / auto 1fr; offset: 5px none 10px reverse / left top; animation: a 1s, b 2s ease-in",
    "grid: auto-flow dense 1fr / 100px; white-space: pre; text-spacing: none; position-try: flip-x flip-y, --a; mask: no-clip",
    "margin: var(--x); all: inherit; place-content: baseline",
};

TEST(CssComputeOom, ABlockOfShorthandsBuiltAndWrittenSweptOverEveryAllocation) {
  for (const char * decls : kShorthands) {
    std::string want;
    {
      FaultAllocator fa;
      ASSERT_EQ(build_and_write(decls, fa, &want), GTEXT_CSS_OK) << decls;
      EXPECT_TRUE(fa.live.empty());
    }
    ASSERT_FALSE(want.empty()) << decls;
    size_t failures = 0;
    for (size_t n = 1; n < 100000; n++) {
      FaultAllocator fa;
      std::string got;
      fa.arm(n);
      const GTEXT_CSS_Status st = build_and_write(decls, fa, &got);
      fa.disarm();
      if (!fa.failed) {
        EXPECT_EQ(st, GTEXT_CSS_OK) << decls << " " << n;
        EXPECT_EQ(got, want) << decls << " " << n;
        EXPECT_TRUE(fa.live.empty()) << decls;
        break;
      }
      failures++;
      EXPECT_EQ(st, GTEXT_CSS_E_OOM) << decls << ": allocation " << n << " " << gtext_css_status_string(st);
      EXPECT_TRUE(got.empty()) << n;
      EXPECT_TRUE(fa.live.empty()) << decls << " " << n << ": a block is left";
      EXPECT_EQ(fa.bad_frees, 0u) << decls << " " << n;
    }
    EXPECT_GT(failures, 20u) << decls;
  }
}

TEST(CssComputeOom, ExpandingAShorthandKeepsNothingWhenAnAllocationFails) {
  GTEXT_CSS_Property_Value pv;
  ASSERT_EQ(gtext_css_parse_property_text(R(), "background", 10, "url(a) 1px 2px / 3px 4px, red", 29, nullptr, &pv), GTEXT_CSS_OK);
  ASSERT_EQ(pv.outcome, GTEXT_CSS_PROP_RESULT_VALID);
  size_t failures = 0;
  for (size_t n = 1; n < 100000; n++) {
    FaultAllocator fa;
    GTEXT_CSS_Longhand_List * list = nullptr;
    GTEXT_CSS_Shorthand_Result result{};
    fa.arm(n);
    const GTEXT_CSS_Status st = gtext_css_shorthand_expand(R(), &pv, &fa.alloc, &list, &result);
    fa.disarm();
    if (!fa.failed) {
      EXPECT_EQ(st, GTEXT_CSS_OK);
      EXPECT_GT(gtext_css_longhand_list_count(list), 8u);
      gtext_css_longhand_list_free(list);
      EXPECT_TRUE(fa.live.empty());
      break;
    }
    failures++;
    EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n << " " << gtext_css_status_string(st);
    EXPECT_EQ(list, nullptr) << n;
    EXPECT_TRUE(fa.live.empty()) << n << ": a block is left";
    EXPECT_EQ(fa.bad_frees, 0u) << n;
  }
  gtext_css_property_value_free(&pv);
  EXPECT_GT(failures, 20u);
}

/// Make a context with the n-th allocation failed; `*reached` says whether it was.
GTEXT_CSS_Status make_context(FaultAllocator & fa, GTEXT_CSS_Compute_Context ** c) {
  GTEXT_CSS_Compute_Options o = gtext_css_compute_options_default();
  o.allocator = &fa.alloc;
  GTEXT_CSS_Compute_Result r{};
  return gtext_css_compute_context_new(R(), nullptr, &o, c, &r);
}

TEST(CssComputeOom, MakingAContextSweptOverTheAllocationsWithAStride) {
  // the context parses and computes the initial value of every property of the pin: tens of thousands of allocations, and the cost of one run is
  // milliseconds, so every 211th allocation is failed (a prime, so that no kind of allocation is always skipped), and the first and last 64
  size_t total = 0;
  {
    FaultAllocator fa;
    GTEXT_CSS_Compute_Context * c = nullptr;
    fa.arm(0);
    ASSERT_EQ(make_context(fa, &c), GTEXT_CSS_OK);
    total = fa.calls;
    gtext_css_compute_context_free(c);
    EXPECT_TRUE(fa.live.empty());
  }
  ASSERT_GT(total, 1000u);
  size_t failures = 0;
  for (size_t n = 1; n <= total + 1; n += (n < 64 || n + 64 > total) ? 1 : 211) {
    FaultAllocator fa;
    GTEXT_CSS_Compute_Context * c = nullptr;
    fa.arm(n);
    const GTEXT_CSS_Status st = make_context(fa, &c);
    fa.disarm();
    if (!fa.failed) {
      EXPECT_EQ(st, GTEXT_CSS_OK) << n;
      gtext_css_compute_context_free(c);
      EXPECT_TRUE(fa.live.empty());
      continue;
    }
    failures++;
    EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n << " " << gtext_css_status_string(st);
    EXPECT_EQ(c, nullptr) << n;
    EXPECT_TRUE(fa.live.empty()) << n << ": a block is left";
    EXPECT_EQ(fa.bad_frees, 0u) << n;
  }
  EXPECT_GT(failures, 30u);
}

TEST(CssComputeOom, ComputingAStyleSweptOverEveryAllocation) {
  Block pb(R(), "font-size: 20px; color: red; writing-mode: vertical-rl; --a: 1");
  Block cb(R(), "width: 2em; margin-inline-start: calc(1em + 3px); height: calc(50% - 10px); border-style: none; border-width: 5px; "
                "font-weight: bolder; color: light-dark(red, blue); background-color: color-mix(in srgb, red, blue)");
  std::string want;
  {
    FaultAllocator fa;
    GTEXT_CSS_Compute_Context * ctx = nullptr;
    ASSERT_EQ(make_context(fa, &ctx), GTEXT_CSS_OK);
    GTEXT_CSS_Computed_Style *ps = nullptr, *cs = nullptr;
    GTEXT_CSS_Compute_Result r{};
    ASSERT_EQ(gtext_css_compute_style(ctx, pb.b, nullptr, nullptr, &ps, &r), GTEXT_CSS_OK) << r.name;
    ASSERT_EQ(gtext_css_compute_style(ctx, cb.b, ps, nullptr, &cs, &r), GTEXT_CSS_OK) << r.name;
    Style tmp;
    tmp.s = cs;
    want = tmp.dump();
    tmp.s = nullptr;
    gtext_css_computed_free(cs);
    gtext_css_computed_free(ps);
    gtext_css_compute_context_free(ctx);
    EXPECT_TRUE(fa.live.empty());
  }
  ASSERT_FALSE(want.empty());

  // the context and the parent are made first, and the sweep fails the allocations of the child alone
  size_t failures = 0;
  for (size_t n = 1; n < 100000; n++) {
    FaultAllocator fa;
    GTEXT_CSS_Compute_Context * ctx = nullptr;
    ASSERT_EQ(make_context(fa, &ctx), GTEXT_CSS_OK);
    GTEXT_CSS_Computed_Style *ps = nullptr, *cs = nullptr;
    GTEXT_CSS_Compute_Result r{};
    ASSERT_EQ(gtext_css_compute_style(ctx, pb.b, nullptr, nullptr, &ps, &r), GTEXT_CSS_OK);
    fa.arm(n);
    const GTEXT_CSS_Status st = gtext_css_compute_style(ctx, cb.b, ps, nullptr, &cs, &r);
    fa.disarm();
    const bool failed = fa.failed;
    if (!failed) {
      EXPECT_EQ(st, GTEXT_CSS_OK) << r.name;
      Style tmp;
      tmp.s = cs;
      EXPECT_EQ(tmp.dump(), want) << n;
    }
    else {
      failures++;
      EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n << " " << gtext_css_status_string(st) << " " << r.name;
      EXPECT_EQ(cs, nullptr) << n;
    }
    gtext_css_computed_free(ps);
    gtext_css_compute_context_free(ctx);
    EXPECT_TRUE(fa.live.empty()) << n << ": a block is left";
    EXPECT_EQ(fa.bad_frees, 0u) << n;
    if (!failed) {
      break;
    }
  }
  EXPECT_GT(failures, 20u);
}

} // namespace
