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
 * Allocation failure in the setters of a rule and in the edits of a typed rule's block: each call is swept over the n-th allocation for every n up to the last one it makes. A call that fails
 * fails with GTEXT_CSS_E_OOM and leaves the sheet, its versions, its ids, the counts of the document and its cache as they were (the dump before equals the dump after; the same
 * call made again gives the result of the unswept call, ids included); one the failure does not reach gives that result; nothing leaks once the document is released.
 */

#include <functional>
#include <string>

#include <gtest/gtest.h>

#include "cssom_test_support.h"

using namespace cot;

namespace {

bool needs_block(const char * name) {
  const std::string n = name;
  return n.find("a page") == 0 || n.find("a keyframe property") == 0 || n.find("a font-face") == 0 || n.find("the cssText") == 0 || n.find("the removal") == 0;
}

struct Scenario {
  const char * name;
  const char * css;
  std::function<GTEXT_CSS_Status(Sheet &)> op;
  size_t min_failures = 3;   ///< The allocations the call is known to make at least.
};

GTEXT_CSS_Status sel(Sheet & s, std::initializer_list<size_t> p, const char * text) {
  GTEXT_CSSOM_Outcome o;
  return gtext_cssom_rule_set_selector_text(path(s, p).r, text, std::strlen(text), &o);
}

GTEXT_CSS_Status attr_set(Sheet & s, std::initializer_list<size_t> p, const char * name, const char * value) {
  GTEXT_CSSOM_Outcome o;
  return gtext_cssom_rule_set_attribute(path(s, p).r, name, std::strlen(name), value, std::strlen(value), &o);
}

GTEXT_CSS_Status prop(Sheet & s, std::initializer_list<size_t> p, const char * name, const char * value) {
  Rule r = path(s, p);
  Decl d = style_of(r.r);
  if (!d.d) {
    return GTEXT_CSS_E_OOM;
  }
  return set(d.d, name, value).st;
}

const Scenario kScenarios[] = {
    {"selectorText of a style rule", "a, b { color: red; & c {} }", [](Sheet & s) { return sel(s, {0}, "d > e, f"); }},
    {"selectorText of a nested rule", "a { b { color: red } }", [](Sheet & s) { return sel(s, {0, 0}, "> c"); }},
    {"selectorText of a page rule", "@page { size: a3 }", [](Sheet & s) { return sel(s, {0}, ":first"); }},
    {"keyText", "@keyframes k { from { color: red } }", [](Sheet & s) {
       GTEXT_CSSOM_Outcome o;
       return gtext_cssom_rule_set_key_text(path(s, {0, 0}).r, "10%, 90%", 8, &o);
     }},
    {"a counter style descriptor", "@counter-style c { system: cyclic; symbols: a b }", [](Sheet & s) { return attr_set(s, {0}, "symbols", "x y z"); }},
    {"a keyframes name", "@keyframes k { from { color: red } }", [](Sheet & s) { return attr_set(s, {0}, "name", "x y"); }},
    {"a page size", "@page { size: a3 }", [](Sheet & s) { return prop(s, {0}, "size", "portrait"); }},
    {"a page margin", "@page { size: a3 }", [](Sheet & s) { return prop(s, {0}, "margin", "1px 2px"); }},
    {"a keyframe property", "@keyframes k { from { color: red } }", [](Sheet & s) { return prop(s, {0, 0}, "margin-top", "1px"); }},
    {"a font-face descriptor", "@font-face { font-family: A }", [](Sheet & s) { return prop(s, {0}, "font-display", "swap"); }},
    {"the cssText of a typed block", "@page { size: a3 }", [](Sheet & s) {
       Rule r = path(s, {0});
       Decl d = style_of(r.r);
       return d.d ? set_text(d.d, "margin-top: 1px; size: portrait").st : GTEXT_CSS_E_OOM;
     }},
    {"the removal from a typed block", "@page { size: a3; margin: 1px }", [](Sheet & s) {
       Rule r = path(s, {0});
       Decl d = style_of(r.r);
       return d.d ? remove(d.d, "size").st : GTEXT_CSS_E_OOM;
     }},
};

TEST(CssomSettersOom, EveryMutatorIsAllOrNothing) {
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
        if (needs_block(sc.name)) {
          const bool in_keyframes = std::string(sc.css).find("@keyframes") != std::string::npos;
          Rule r = in_keyframes ? path(s, {0, 0}) : path(s, {0});
          Decl d = style_of(r.r);
        }
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

}  // namespace
