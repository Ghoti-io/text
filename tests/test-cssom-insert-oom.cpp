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
 * Allocation failure in the mutators and the writers of the rule layer: each call is swept over the n-th allocation for every n up to the last one it makes. A call that fails
 * fails with GTEXT_CSS_E_OOM and leaves the sheet, its versions, its ids, the counts of the document and its cache as they were (the dump before equals the dump after; the same
 * call made again gives the result of the unswept call, ids included); one the failure does not reach gives that result; nothing leaks once the document is released.
 */

#include <functional>
#include <string>

#include <gtest/gtest.h>

#include "cssom_test_support.h"

using namespace cot;

namespace {

struct Scenario {
  const char * name;
  const char * css;
  std::function<GTEXT_CSS_Status(Sheet &)> op;
  size_t min_failures = 3;   ///< The allocations the call is known to make at least (a plain deletion makes none).
};

GTEXT_CSS_Status run_insert(Sheet & s, const std::string & text, size_t index) {
  return insert_rule(s.s, text, index).st;
}

const Scenario kScenarios[] = {
    {"insert a style rule", "a{color:red} b{}", [](Sheet & s) { return run_insert(s, "c > d{margin:1px 2px}", 1); }},
    {"insert a grouping rule with rules inside", "a{}", [](Sheet & s) { return run_insert(s, "@media print{ b{color:red} @supports (a:b){ c{} } d{ & e{} } }", 1); }},
    {"insert a rule whose general-enclosed is kept as written", "a{}", [](Sheet & s) { return run_insert(s, "@media screen and ( a ) , print { b{} @container  foo ( a:  b ) { c{} } }", 1); }},
    {"insert a declaration block as a rule", "a{ b{} }", [](Sheet & s) { return insert_rule(path(s, {0}).r, "width:1px; color:red", 0).st; }},
    {"insert a nested rule", "a{ b{} }", [](Sheet & s) { return insert_rule(path(s, {0}).r, "> c{ color:red }", 1).st; }},
    {"insert a namespace", "@import url(a.css); @namespace x url(u);", [](Sheet & s) { return run_insert(s, "@namespace y url(v);", 2); }},
    {"insert a default namespace", "@namespace x url(u);", [](Sheet & s) { return run_insert(s, "@namespace url(d);", 1); }},
    {"insert an import", "@import url(a.css); a{}", [](Sheet & s) { return run_insert(s, "@import url(b.css) layer(x) screen;", 1); }},
    {"insert with a prefix", "@namespace x url(u); a{}", [](Sheet & s) { return run_insert(s, "x|b, *|c{}", 2); }},
    {"insert a keyframes rule", "a{}", [](Sheet & s) { return run_insert(s, "@keyframes k{ from{opacity:0} to{opacity:1} }", 0); }},
    {"insert a margin rule", "@page{ margin:1px }", [](Sheet & s) { return insert_rule(path(s, {0}).r, "@top-left{ content:\"x\" }", 0).st; }},
    {"append a keyframe", "@keyframes k{ from{opacity:0} }", [](Sheet & s) {
       GTEXT_CSSOM_Outcome o;
       return gtext_cssom_keyframes_append_rule(path(s, {0}).r, "50% {opacity:1}", 15, nullptr, &o);
     }},
    {"delete a rule", "a{ b{} } c{}", [](Sheet & s) { return delete_rule(s.s, 0); }, 0},
    {"delete a rule inside a rule", "@media print{ a{} b{} }", [](Sheet & s) { return delete_rule(path(s, {0}).r, 0); }, 0},
    {"delete a namespace", "@import url(a.css); @namespace x url(u); @namespace y url(v); @namespace url(d);", [](Sheet & s) { return delete_rule(s.s, 2); }},
    {"delete a keyframe", "@keyframes k{ from{opacity:0} 50%{opacity:.5} 50%{opacity:.6} }", [](Sheet & s) {
       GTEXT_CSSOM_Outcome o;
       return gtext_cssom_keyframes_delete_rule(path(s, {0}).r, "50%", 3, &o);
     }},
    {"add a legacy rule", "a{}", [](Sheet & s) {
       GTEXT_CSSOM_Outcome o;
       return gtext_cssom_sheet_add_rule(s.s, "b", 1, "color:red", 9, true, 0, &o);
     }},
};

TEST(CssomInsertOom, EveryMutatorIsAllOrNothing) {
  for (const Scenario & sc : kScenarios) {
    std::string want;
    GTEXT_CSS_Status want_status = GTEXT_CSS_OK;
    {
      FaultAllocator fa;
      Doc doc(nullptr, &fa.alloc);
      Sheet s(doc, sc.css);
      ASSERT_EQ(s.status, GTEXT_CSS_OK) << sc.name;
      want_status = sc.op(s);
      ASSERT_EQ(want_status, GTEXT_CSS_OK) << sc.name;
      want = dump(s.s);
    }
    size_t failures = 0;
    for (size_t n = 1; n < 100000; n++) {
      FaultAllocator fa;
      {
        Doc doc(nullptr, &fa.alloc);
        Sheet s(doc, sc.css);
        ASSERT_EQ(s.status, GTEXT_CSS_OK) << sc.name;
        const std::string before = dump(s.s);
        const uint64_t generation = gtext_cssom_document_generation(doc.d);
        const uint64_t version = gtext_cssom_sheet_version(s.s);
        GTEXT_CSSOM_Counters c0;
        ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c0), GTEXT_CSS_OK);
        const size_t events = doc.events.size();
        fa.arm(n);
        const GTEXT_CSS_Status st = sc.op(s);
        fa.disarm();
        if (!fa.failed) {
          ASSERT_EQ(st, GTEXT_CSS_OK) << sc.name << " at " << n;
          EXPECT_EQ(dump(s.s), want) << sc.name << ": the sweep ended at " << n;
          break;
        }
        failures++;
        ASSERT_EQ(st, GTEXT_CSS_E_OOM) << sc.name << ": allocation " << n << " " << gtext_css_status_string(st);
        EXPECT_EQ(dump(s.s), before) << sc.name << ": allocation " << n;
        EXPECT_EQ(gtext_cssom_document_generation(doc.d), generation) << sc.name << " " << n;
        EXPECT_EQ(gtext_cssom_sheet_version(s.s), version) << sc.name << " " << n;
        EXPECT_EQ(doc.events.size(), events) << sc.name << " " << n << ": no event for a call that failed";
        GTEXT_CSSOM_Counters c1;
        ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c1), GTEXT_CSS_OK);
        EXPECT_EQ(c1.live_objects, c0.live_objects) << sc.name << " " << n;
        EXPECT_EQ(c1.live_bytes, c0.live_bytes) << sc.name << " " << n;
        EXPECT_EQ(c1.tree_bytes, c0.tree_bytes) << sc.name << " " << n;
        // and the same call again, now that nothing fails, is the unswept call: the ids are the ones it would have had
        EXPECT_EQ(sc.op(s), GTEXT_CSS_OK) << sc.name << " " << n;
        EXPECT_EQ(dump(s.s), want) << sc.name << ": the retry after allocation " << n;
      }
      EXPECT_TRUE(fa.live.empty()) << sc.name << ": allocation " << n << " leaked";
      EXPECT_EQ(fa.bad_frees, 0u);
    }
    EXPECT_GE(failures, sc.min_failures) << sc.name;
  }
}

// cssText and the sheet's serialization allocate (the buffer, the cache entry, the parse of a selector): a failure is a status and changes no state, and the retry gives the text.
TEST(CssomInsertOom, TheWritersFailWithoutChangingTheState) {
  const char * const sheets[] = {"a, b > c{color:red; margin:1px 2px} @media print{ d{} @keyframes k{ from{opacity:0} } } @import url(a.css) screen;",
      "@namespace x url(u); x|a{ & b{ color:red } width:1px; & c{} } @page :first{ margin:1px; @top-left{ content:\"x\" } } @layer a, b;"};
  for (const char * css_text : sheets) {
    std::string want;
    std::vector<std::string> want_rules;
    {
      Doc doc;
      Sheet s(doc, css_text);
      ASSERT_EQ(s.status, GTEXT_CSS_OK);
      want = sheet_text(s.s);
      want_rules = css_list(s.rules());
    }
    size_t failures = 0;
    for (size_t n = 1; n < 100000; n++) {
      FaultAllocator fa;
      {
        Doc doc(nullptr, &fa.alloc);
        Sheet s(doc, css_text);
        ASSERT_EQ(s.status, GTEXT_CSS_OK);
        const std::string before = dump(s.s);
        GTEXT_CSSOM_Counters c0;
        gtext_cssom_document_counters(doc.d, &c0);
        fa.arm(n);
        GTEXT_CSS_Status st = GTEXT_CSS_OK;
        const std::string out = sheet_text(s.s, &st);
        fa.disarm();
        if (!fa.failed) {
          EXPECT_EQ(st, GTEXT_CSS_OK);
          EXPECT_EQ(out, want);
          break;
        }
        failures++;
        EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
        EXPECT_EQ(dump(s.s), before);
        GTEXT_CSSOM_Counters c1;
        gtext_cssom_document_counters(doc.d, &c1);
        EXPECT_EQ(c1.live_bytes, c0.live_bytes) << n;
        EXPECT_EQ(sheet_text(s.s), want) << "the retry";
        // the same for one rule's text
        for (size_t i = 0; i < want_rules.size(); i++) {
          Rule r = at(s.rules(), i);
          fa.arm(1 + (n % 7));
          char * text = nullptr;
          const GTEXT_CSS_Status rs = gtext_cssom_rule_css_text(r.r, &text, nullptr);
          fa.disarm();
          if (rs == GTEXT_CSS_OK) {
            EXPECT_EQ(std::string(text), want_rules[i]);
            gtext_cssom_buffer_free(&doc.core()->alloc, text);
          }
          else {
            EXPECT_EQ(rs, GTEXT_CSS_E_OOM);
            EXPECT_EQ(css(r.r), want_rules[i]) << "the retry";
          }
        }
      }
      EXPECT_TRUE(fa.live.empty()) << "allocation " << n << " leaked";
      EXPECT_EQ(fa.bad_frees, 0u);
    }
    EXPECT_GT(failures, 5u);
  }
}

} // namespace
