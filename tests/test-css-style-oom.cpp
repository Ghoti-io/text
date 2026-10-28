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

/// A second allocator, which counts: the largest block it was asked for and the blocks it holds.
struct SizeAllocator {
  GTEXT_Allocator alloc;
  size_t largest = 0, live = 0, calls = 0;
  SizeAllocator() {
    alloc.ctx = this;
    alloc.malloc_fn = [](void * c, size_t n) -> void * {
      auto * self = static_cast<SizeAllocator *>(c);
      self->calls++;
      self->largest = n > self->largest ? n : self->largest;
      self->live++;
      return std::malloc(n ? n : 1);
    };
    alloc.calloc_fn = [](void * c, size_t n, size_t m) -> void * {
      auto * self = static_cast<SizeAllocator *>(c);
      self->calls++;
      self->largest = n * m > self->largest ? n * m : self->largest;
      self->live++;
      return std::calloc(n ? n : 1, m ? m : 1);
    };
    alloc.realloc_fn = [](void * c, void * p, size_t n) -> void * {
      auto * self = static_cast<SizeAllocator *>(c);
      self->calls++;
      self->largest = n > self->largest ? n : self->largest;
      if (!p) {
        self->live++;
      }
      return std::realloc(p, n ? n : 1);
    };
    alloc.free_fn = [](void * c, void * p) {
      auto * self = static_cast<SizeAllocator *>(c);
      if (p) {
        self->live--;
      }
      std::free(p);
    };
  }
};

TEST(CssStyleOom, ThePresentationAttributeIsConvertedInTheAllocatorOfTheCandidateSet) {
  /* the conversion of a transform builds its text in a buffer, which is the set's allocator's like everything else the call makes: adding the attribute asks the
     allocator for more than adding the text it is converted to does */
  const GTEXT_CSS_Presentation_Table * table = gtext_css_presentation_table_svg2();
  const GTEXT_CSS_Presentation_Row * tr = gtext_css_presentation_find(table, GTEXT_CSS_SVG_NAMESPACE, strlen(GTEXT_CSS_SVG_NAMESPACE), "rect", 4, "transform", 9);
  ASSERT_NE(tr, nullptr);
  const std::string value = "rotate(30 5 5) translate(1,2)";
  GTEXT_CSS_Presentation_Options po = gtext_css_presentation_options_default();
  char * converted = nullptr;
  size_t converted_length = 0;
  ASSERT_EQ(gtext_css_presentation_value_text(tr, value.data(), value.size(), &po, nullptr, &converted, &converted_length, nullptr), GTEXT_CSS_OK);
  const std::string text(converted, converted_length);
  gtext_allocator_free(nullptr, converted);
  size_t calls[2] = {0, 0};
  for (int which = 0; which < 2; which++) {
    SizeAllocator sa;
    GTEXT_CSS_Candidate_Options co = gtext_css_candidate_options_default();
    co.allocator = &sa.alloc;
    GTEXT_CSS_Candidates * set = nullptr;
    ASSERT_EQ(gtext_css_candidates_new(svgreg::shared(), nullptr, &co, &set), GTEXT_CSS_OK);
    const size_t before = sa.calls;
    GTEXT_CSS_Cascade_Entry keys;
    memset(&keys, 0, sizeof keys);
    keys.origin = GTEXT_CSS_ORIGIN_AUTHOR;
    if (which == 0) {
      ASSERT_EQ(gtext_css_candidates_add_presentation(set, &keys, tr, value.data(), value.size(), &po, nullptr, nullptr), GTEXT_CSS_OK);
    }
    else {
      GTEXT_CSS_Property_Outcome outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
      keys.presentational_hint = true;
      ASSERT_EQ(gtext_css_candidates_add_text(set, &keys, "transform", 9, text.data(), text.size(), nullptr, &outcome, nullptr), GTEXT_CSS_OK);
      ASSERT_EQ(outcome, GTEXT_CSS_PROP_RESULT_VALID);
    }
    calls[which] = sa.calls - before;
    gtext_css_candidates_free(set);
    EXPECT_EQ(sa.live, 0u) << which << ": nothing is left";
  }
  EXPECT_GT(calls[0], calls[1]) << "the conversion buffer is a block of the set's allocator";
}

TEST(CssStyleOom, AnAttributeAddedToACandidateSetSweptOverEveryAllocation) {
  const GTEXT_CSS_Presentation_Table * table = gtext_css_presentation_table_svg2();
  const GTEXT_CSS_Presentation_Row * tr = gtext_css_presentation_find(table, GTEXT_CSS_SVG_NAMESPACE, strlen(GTEXT_CSS_SVG_NAMESPACE), "rect", 4, "transform", 9);
  ASSERT_NE(tr, nullptr);
  FaultAllocator fa;
  size_t failures = 0;
  for (size_t n = 1; n < 10000; n++) {
    fa.arm(n);
    GTEXT_CSS_Candidate_Options co = gtext_css_candidate_options_default();
    co.allocator = &fa.alloc;
    GTEXT_CSS_Candidates * set = nullptr;
    GTEXT_CSS_Status st = gtext_css_candidates_new(svgreg::shared(), nullptr, &co, &set);
    if (st == GTEXT_CSS_OK) {
      GTEXT_CSS_Cascade_Entry keys;
      memset(&keys, 0, sizeof keys);
      keys.origin = GTEXT_CSS_ORIGIN_AUTHOR;
      GTEXT_CSS_Presentation_Options po = gtext_css_presentation_options_default();
      const char * v = "rotate(30 5 5) translate(1,2)";
      st = gtext_css_candidates_add_presentation(set, &keys, tr, v, strlen(v), &po, nullptr, nullptr);
    }
    gtext_css_candidates_free(set);
    fa.disarm();
    EXPECT_EQ(fa.bad_frees, 0u) << n;
    EXPECT_TRUE(fa.live.empty()) << n << ": a block is left";
    if (!fa.failed) {
      EXPECT_EQ(st, GTEXT_CSS_OK) << n;
      break;
    }
    failures++;
    EXPECT_TRUE(st == GTEXT_CSS_OK || st == GTEXT_CSS_E_OOM) << n << " " << gtext_css_status_string(st);
  }
  EXPECT_GT(failures, 3u);
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
