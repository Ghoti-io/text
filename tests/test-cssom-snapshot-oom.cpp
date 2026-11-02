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
 * Allocation failure in a snapshot (css_cssom_sheet.h): the call is swept over the n-th allocation for every n up to the last one it makes, over a sheet whose rules are all as they
 * were read, over one with rules that were changed (read from their text), over a sheet with imports, and over a second call that reuses the cache. A call that fails is
 * GTEXT_CSS_E_OOM and leaves the sheet, its versions, its ids, the document's counters (the generation, the live objects and bytes), its events and its cache as they were; the next call,
 * with the allocator healthy again, gives the snapshot the unswept call gives (the same dump); one the failure does not reach gives that snapshot; nothing leaks once the document is gone.
 */

#include <functional>
#include <string>

#include <gtest/gtest.h>

#include "cssom_test_support.h"

using namespace cot;

namespace {

std::string dump_of(const GTEXT_CSS_Sheet * s) {
  std::string out;
  GTEXT_Markup_Sink sink;
  sink.user = &out;
  sink.write = [](void * u, const char * d, size_t n) {
    static_cast<std::string *>(u)->append(d, n);
    return GTEXT_MARKUP_OK;
  };
  EXPECT_EQ(gtext_css_sheet_dump(s, &sink), GTEXT_CSS_OK);
  return out;
}

/// The memo of a leaf rule's `cssText` is the one allocation a read may lose and still succeed (css_cssom_rule.h: never an error); with no room for it a failed allocation is always a failed call.
const std::function<void(GTEXT_CSSOM_Config &)> kNoMemo = [](GTEXT_CSSOM_Config & c) { c.max_cached_text_bytes = 1; };

struct State {
  std::string dump;
  uint64_t version, generation, next_id;
  size_t live_objects, live_bytes, events;
  bool operator==(const State & o) const {
    return dump == o.dump && version == o.version && generation == o.generation && next_id == o.next_id && live_objects == o.live_objects && live_bytes == o.live_bytes && events == o.events;
  }
};

State state_of(Doc & doc, GTEXT_CSSOM_Sheet * s) {
  GTEXT_CSSOM_Counters c;
  EXPECT_EQ(gtext_cssom_document_counters(doc.d, &c), GTEXT_CSS_OK);
  // the bytes of the `cssText` kept for leaf rules are a memo (css_cssom_rule.h: never an error, dropped or filled as room allows): a read may fill it, and it is not the sheet's state
  return {dump(s), gtext_cssom_sheet_version(s), gtext_cssom_document_generation(doc.d), doc.core()->next_id, c.live_objects, c.live_bytes - c.cached_text_bytes, doc.events.size()};
}

struct Scenario {
  const char * name;
  const char * css;
  bool change;      ///< Change a rule of the sheet before the sweep, so that it is read from its text.
  bool imports;     ///< Give the sheet's `@import` a sheet of its own.
  bool warm;        ///< Take a snapshot first and change the sheet, so that the call under test has a cache to replace.
};

const Scenario kScenarios[] = {
    {"rules as they were read", "a{color:red} b > c{margin:1px 2px} @media print{ d{width:1px} @supports (a:b){ e{color:blue} } } @layer base, theme; @layer base{ f{color:red} }", false, false, false},
    {"a rule changed", "a{color:red} @media print{ d{width:1px} e{color:blue} } @namespace s url(urn:s); s|g{color:red} h{ color:red; i{ width:1px } }", true, false, false},
    {"an import with a sheet", "@import url(x.css) layer(l); a{color:red} @media print{ d{width:1px} }", true, true, false},
    {"a cache to replace", "a{color:red} @media print{ d{width:1px} } e{color:blue}", true, false, true},
};

TEST(CssomSnapshotOom, ASnapshotIsAllOrNothing) {
  for (const Scenario & sc : kScenarios) {
    std::string want;
    {
      FaultAllocator fa;
      Doc doc(kNoMemo, &fa.alloc);
      Sheet s(doc, sc.css);
      ASSERT_EQ(s.status, GTEXT_CSS_OK) << sc.name;
      Sheet child(doc, "k{color:green} l{margin:0}");
      if (sc.imports) {
        ASSERT_EQ(gtext_cssom_import_rule_complete(path(s, {0}).r, child.s, nullptr), GTEXT_CSS_OK);
      }
      if (sc.warm) {
        GTEXT_CSS_Sheet * w = nullptr;
        ASSERT_EQ(gtext_cssom_sheet_snapshot(s.s, &w), GTEXT_CSS_OK);
        gtext_css_sheet_free(w);
      }
      if (sc.change) {
        Decl d = style_of(path(s, {sc.imports ? 1u : 0u}).r);
        ASSERT_EQ(set(d.d, "width", "7px").o.kind, GTEXT_CSSOM_CHANGED);
      }
      GTEXT_CSS_Sheet * snap = nullptr;
      ASSERT_EQ(gtext_cssom_sheet_snapshot(s.s, &snap), GTEXT_CSS_OK) << sc.name;
      want = dump_of(snap);
      gtext_css_sheet_free(snap);
    }
    size_t failures = 0, swallowed = 0;
    for (size_t n = 1; n < 100000; n++) {
      FaultAllocator fa;
      {
        Doc doc(kNoMemo, &fa.alloc);
        Sheet s(doc, sc.css);
        ASSERT_EQ(s.status, GTEXT_CSS_OK);
        Sheet child(doc, "k{color:green} l{margin:0}");
        if (sc.imports) {
          ASSERT_EQ(gtext_cssom_import_rule_complete(path(s, {0}).r, child.s, nullptr), GTEXT_CSS_OK);
        }
        GTEXT_CSS_Sheet * warm = nullptr;
        if (sc.warm) {
          ASSERT_EQ(gtext_cssom_sheet_snapshot(s.s, &warm), GTEXT_CSS_OK);
        }
        if (sc.change) {
          Decl d = style_of(path(s, {sc.imports ? 1u : 0u}).r);
          ASSERT_EQ(set(d.d, "width", "7px").o.kind, GTEXT_CSSOM_CHANGED);
        }
        const State before = state_of(doc, s.s);
        const std::string warm_dump = warm ? dump_of(warm) : "";
        GTEXT_CSS_Sheet * snap = nullptr;
        fa.arm(n);
        const GTEXT_CSS_Status st = gtext_cssom_sheet_snapshot(s.s, &snap);
        fa.disarm();
        if (!fa.failed || st == GTEXT_CSS_OK) {
          ASSERT_EQ(st, GTEXT_CSS_OK) << sc.name;
          EXPECT_EQ(dump_of(snap), want) << sc.name << ": the sweep ended at " << n;
          gtext_css_sheet_free(snap);
          gtext_css_sheet_free(warm);
          if (fa.failed) {
            swallowed++;
            EXPECT_TRUE(state_of(doc, s.s) == before);
            continue;
          }
          break;
        }
        failures++;
        EXPECT_EQ(st, GTEXT_CSS_E_OOM) << sc.name << " at " << n << ": " << gtext_css_status_string(st);
        EXPECT_EQ(snap, nullptr);
        EXPECT_TRUE(state_of(doc, s.s) == before) << sc.name << ": the sheet changed at " << n;
        if (warm) {
          EXPECT_EQ(dump_of(warm), warm_dump) << "the cache's old snapshot is as it was";
        }
        // the next call gives what the unswept call gives
        ASSERT_EQ(gtext_cssom_sheet_snapshot(s.s, &snap), GTEXT_CSS_OK) << sc.name << " after the failure at " << n;
        EXPECT_EQ(dump_of(snap), want) << sc.name << " after the failure at " << n;
        gtext_css_sheet_free(snap);
        gtext_css_sheet_free(warm);
      }
      EXPECT_TRUE(fa.live.empty()) << sc.name << ": a leak after the failure at " << n;
      EXPECT_EQ(fa.bad_frees, 0u);
    }
    EXPECT_GT(failures, 10u) << sc.name << ": the call makes allocations";
    EXPECT_EQ(swallowed, 0u) << sc.name << ": an allocation that failed was an allocation the call needed";
  }
}

TEST(CssomSnapshotOom, ACallThatFindsTheCacheAllocatesNothing) {
  FaultAllocator fa;
  Doc doc(kNoMemo, &fa.alloc);
  Sheet s(doc, "a{color:red} @media print{ b{width:1px} }");
  GTEXT_CSS_Sheet * first = nullptr;
  ASSERT_EQ(gtext_cssom_sheet_snapshot(s.s, &first), GTEXT_CSS_OK);
  fa.arm(1);   // the very next allocation fails
  GTEXT_CSS_Sheet * second = nullptr;
  ASSERT_EQ(gtext_cssom_sheet_snapshot(s.s, &second), GTEXT_CSS_OK);
  EXPECT_FALSE(fa.failed) << "the cached call did not allocate";
  EXPECT_EQ(second, first);
  uint64_t v = 0;
  ASSERT_EQ(gtext_cssom_sheet_composite_version(s.s, &v), GTEXT_CSS_OK);
  EXPECT_FALSE(fa.failed);
  fa.disarm();
  gtext_css_sheet_free(first);
  gtext_css_sheet_free(second);
}

TEST(CssomSnapshotOom, ImportingASheetAllocatesNothing) {
  FaultAllocator fa;
  Doc doc(kNoMemo, &fa.alloc);
  Sheet host(doc, "@import url(a.css); b{color:red}");
  Sheet child(doc, "c{color:blue}");
  Rule imp = path(host, {0});
  fa.arm(1);
  ASSERT_EQ(gtext_cssom_import_rule_complete(imp.r, child.s, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_import_rule_complete(imp.r, nullptr, nullptr), GTEXT_CSS_OK);
  EXPECT_FALSE(fa.failed) << "the strong guarantee is that there is nothing to fail";
  fa.disarm();
}

} // namespace
