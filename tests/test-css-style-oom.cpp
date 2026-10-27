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
 * Allocation failure in the computed-style call: an element with presentation attributes, a `style` attribute, entries and diagnostics, with the n-th
 * allocation failed for every n up to the last one the call makes, and the dump and the hash with a failing sink.
 *
 * A failed run is GTEXT_CSS_E_OOM with no style returned, or the answer of the run that failed nothing; either way the allocator holds no block afterwards
 * and was never asked to free one it did not make. A run that failed nothing gives the answer of the first, so a failure that was swallowed and turned into a
 * different value is found.
 */

#include <string>

#include <gtest/gtest.h>

#include "css_compute_test_support.h"
#include "css_markup_tree.h"
#include "css_svg_registry.h"
#include "markup_test_support.h"

namespace {

struct Scenario {
  const char * name;
  const char * parent_attrs;
  const char * attrs;
};

const Scenario kScenarios[] = {
    {"attributes", "fill=\"orange\" font-size=\"10\"", "fill=\"red\" stroke=\"blue\" stroke-width=\"2\" opacity=\"0.5\" font-size=\"2em\""},
    {"style", "", "style=\"fill: green; stroke-width: 4px !important; margin: 1px 2px; bogus: 1; width: calc(1em + 2px)\" fill=\"red\""},
    {"custom", "style=\"--a: 3px\"", "style=\"--b: var(--a) 2px; --c: var(--b) var(--b); stroke-width: var(--a); --cycle: var(--cycle)\""},
    {"transform", "", "transform=\"rotate(30 5 5) translate(1,2)\" stroke-dasharray=\"1,2 3\" fill=\"12px\""},
    {"path", "", "d=\"M0 0 L10 10\" x=\"3\" y=\"4\" width=\"5\" height=\"6\" rx=\"1\" ry=\"2\""},
};

TEST(CssStyleOom, ComputingAStyleSweptOverEveryAllocation) {
  for (const Scenario & sc : kScenarios) {
    FaultAllocator fa;
    {
      const std::string doc = std::string("<svg xmlns=\"") + GTEXT_CSS_SVG_NAMESPACE + "\" id=\"p\" " + sc.parent_attrs + "><rect id=\"r\" " + sc.attrs +
          "/><path id=\"pa\" " + sc.attrs + "/></svg>";
      cmt::Tree tree;
      ASSERT_TRUE(tree.parse(doc, false)) << sc.name;
      tree.build();
      GTEXT_CSS_Compute_Options co = gtext_css_compute_options_default();
      co.elements = &tree.table;
      co.allocator = &fa.alloc;
      GTEXT_CSS_Compute_Context * ctx = nullptr;
      ASSERT_EQ(gtext_css_compute_context_new(svgreg::shared(), nullptr, &co, &ctx, nullptr), GTEXT_CSS_OK);
      GTEXT_CSS_Computed_Style * parent = nullptr;
      ASSERT_EQ(gtext_css_style_compute(ctx, tree.find("p"), nullptr, nullptr, 0, nullptr, &parent, nullptr), GTEXT_CSS_OK) << sc.name;
      const std::string text = "stroke-width: 1px; opacity: 0.25 !important";
      GTEXT_CSS_Style_Entry entry;
      memset(&entry, 0, sizeof entry);
      entry.keys.origin = GTEXT_CSS_ORIGIN_AUTHOR;
      entry.text = {text.data(), text.size()};
      auto run = [&](GTEXT_CSS_Computed_Style ** out) {
        return gtext_css_style_compute(ctx, tree.find(sc.name[0] == 'p' && sc.name[1] == 'a' ? "pa" : "r"), parent, &entry, 1, nullptr, out, nullptr);
      };
      std::string want;
      fa.arm(0);
      {
        GTEXT_CSS_Computed_Style * s = nullptr;
        ASSERT_EQ(run(&s), GTEXT_CSS_OK) << sc.name;
        GTEXT_Markup_Sink sink;
        memset(&sink, 0, sizeof sink);
        sink.user = &want;
        sink.write = [](void * u, const char * b, size_t n) -> GTEXT_Markup_Status {
          static_cast<std::string *>(u)->append(b, n);
          return GTEXT_MARKUP_OK;
        };
        ASSERT_EQ(gtext_css_computed_dump(s, &sink), GTEXT_CSS_OK);
        want += "diagnostics " + std::to_string(gtext_css_computed_diagnostic_total(s));
        gtext_css_computed_free(s);
      }
      ASSERT_GT(fa.calls, 10u) << sc.name;
      size_t failures = 0;
      for (size_t n = 1; n < 200000; n++) {
        std::string got;
        GTEXT_CSS_Status st;
        {
          GTEXT_CSS_Computed_Style * s = nullptr;
          fa.arm(n);
          st = run(&s);
          fa.disarm();
          if (st == GTEXT_CSS_OK) {
            GTEXT_Markup_Sink sink;
            memset(&sink, 0, sizeof sink);
            sink.user = &got;
            sink.write = [](void * u, const char * b, size_t len) -> GTEXT_Markup_Status {
              static_cast<std::string *>(u)->append(b, len);
              return GTEXT_MARKUP_OK;
            };
            EXPECT_EQ(gtext_css_computed_dump(s, &sink), GTEXT_CSS_OK);
            got += "diagnostics " + std::to_string(gtext_css_computed_diagnostic_total(s));
          }
          else {
            EXPECT_EQ(s, nullptr) << sc.name << " " << n;
          }
          gtext_css_computed_free(s);
        }
        if (!fa.failed) {
          EXPECT_EQ(st, GTEXT_CSS_OK) << sc.name << " " << n;
          EXPECT_EQ(got, want) << sc.name << " " << n;
          break;
        }
        failures++;
        if (st != GTEXT_CSS_OK) {
          EXPECT_EQ(st, GTEXT_CSS_E_OOM) << sc.name << ": allocation " << n << " " << gtext_css_status_string(st);
        }
        else {
          EXPECT_EQ(got, want) << sc.name << " " << n << ": a failure that was swallowed changed the answer";
        }
        EXPECT_EQ(fa.bad_frees, 0u) << sc.name << " " << n;
      }
      EXPECT_GT(failures, 10u) << sc.name;
      gtext_css_computed_free(parent);
      gtext_css_compute_context_free(ctx);
    }
    EXPECT_TRUE(fa.live.empty()) << sc.name << ": a block is left";
    EXPECT_EQ(fa.bad_frees, 0u) << sc.name;
  }
}

TEST(CssStyleOom, TheDumpAndTheHashSweptOverEveryAllocation) {
  FaultAllocator fa;
  {
    cmt::Tree tree;
    ASSERT_TRUE(tree.parse(std::string("<svg xmlns=\"") + GTEXT_CSS_SVG_NAMESPACE + "\" id=\"r\" style=\"--a: 1; fill: red\"/>", false));
    tree.build();
    GTEXT_CSS_Compute_Options co = gtext_css_compute_options_default();
    co.elements = &tree.table;
    co.allocator = &fa.alloc;
    GTEXT_CSS_Compute_Context * ctx = nullptr;
    ASSERT_EQ(gtext_css_compute_context_new(svgreg::shared(), nullptr, &co, &ctx, nullptr), GTEXT_CSS_OK);
    GTEXT_CSS_Computed_Style * s = nullptr;
    ASSERT_EQ(gtext_css_style_compute(ctx, tree.find("r"), nullptr, nullptr, 0, nullptr, &s, nullptr), GTEXT_CSS_OK);
    unsigned char want[32];
    ASSERT_EQ(gtext_css_computed_hash(s, want), GTEXT_CSS_OK);
    GTEXT_Markup_Sink null_sink;
    memset(&null_sink, 0, sizeof null_sink);
    null_sink.write = [](void *, const char *, size_t) -> GTEXT_Markup_Status { return GTEXT_MARKUP_OK; };
    size_t failures = 0;
    for (size_t n = 1; n < 1000; n++) {
      unsigned char got[32];
      fa.arm(n);
      const GTEXT_CSS_Status h = gtext_css_computed_hash(s, got);
      const GTEXT_CSS_Status d = gtext_css_computed_dump(s, &null_sink);
      fa.disarm();
      if (!fa.failed) {
        EXPECT_EQ(h, GTEXT_CSS_OK);
        EXPECT_EQ(d, GTEXT_CSS_OK);
        EXPECT_EQ(memcmp(got, want, 32), 0);
        break;
      }
      failures++;
      EXPECT_TRUE(h == GTEXT_CSS_E_OOM || d == GTEXT_CSS_E_OOM) << n << ": the failure is not reported (hash " << h << ", dump " << d << ")";
      if (h == GTEXT_CSS_OK) {
        EXPECT_EQ(memcmp(got, want, 32), 0) << n << ": a hash that was reported is the hash";
      }
    }
    EXPECT_GT(failures, 0u) << "the dump allocates the sorted names";
    gtext_css_computed_free(s);
    gtext_css_compute_context_free(ctx);
  }
  EXPECT_TRUE(fa.live.empty());
  EXPECT_EQ(fa.bad_frees, 0u);
}

} // namespace
