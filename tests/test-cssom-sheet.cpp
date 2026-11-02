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
 * The sheet object (css_cssom_sheet.h): its attributes, `disabled` and the origin-clean flag, the parse's limits and the status each one gives, nests and sheets of the size the
 * specification names (a hundred thousand deep, a million rules) without recursion, the compaction that gives the memory of a mostly deleted sheet back, and the canonical
 * dump and its hash.
 */

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cssom_test_support.h"

using namespace cot;

namespace {

/* ---- attributes ------------------------------------------------------------------------------------------------------------------------------- */

TEST(CssomSheet, AttributesAreWhatItWasMadeWith) {
  Doc doc;
  int owner = 0;
  GTEXT_CSSOM_Sheet_Options o = gtext_cssom_sheet_options_default();
  o.href = "https://example.test/a.css";
  o.title = "Main";
  o.base_url = "https://example.test/";
  o.owner_node = &owner;
  o.alternate = true;
  Sheet s(doc, "a{}", &o);
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  EXPECT_STREQ(gtext_cssom_sheet_type(s.s), "text/css");
  char * out = nullptr;
  size_t n = 0;
  ASSERT_EQ(gtext_cssom_sheet_href(s.s, &out, &n), GTEXT_CSS_OK);
  EXPECT_EQ(take(doc.core(), out), "https://example.test/a.css");
  ASSERT_EQ(gtext_cssom_sheet_title(s.s, &out, &n), GTEXT_CSS_OK);
  EXPECT_EQ(take(doc.core(), out), "Main");
  ASSERT_EQ(gtext_cssom_sheet_base_url(s.s, &out, &n), GTEXT_CSS_OK);
  EXPECT_EQ(take(doc.core(), out), "https://example.test/");
  EXPECT_EQ(gtext_cssom_sheet_owner_node(s.s), &owner);
  EXPECT_TRUE(gtext_cssom_sheet_alternate(s.s));
  EXPECT_FALSE(gtext_cssom_sheet_disabled(s.s));
  EXPECT_TRUE(gtext_cssom_sheet_origin_clean(s.s));
  EXPECT_FALSE(gtext_cssom_sheet_constructed(s.s));
  EXPECT_EQ(gtext_cssom_sheet_document(s.s), doc.d);
  // a sheet made with no options has none of them
  Sheet bare(doc, "b{}");
  ASSERT_EQ(gtext_cssom_sheet_href(bare.s, &out, &n), GTEXT_CSS_OK);
  EXPECT_EQ(out, nullptr);
  EXPECT_EQ(n, 0u);
  EXPECT_EQ(gtext_cssom_sheet_owner_node(bare.s), nullptr);
  EXPECT_NE(gtext_cssom_sheet_id(s.s), gtext_cssom_sheet_id(bare.s));
  EXPECT_LT(gtext_cssom_sheet_id(s.s), gtext_cssom_sheet_id(bare.s)) << "ids are monotonic";
}

TEST(CssomSheet, ArgumentsAreChecked) {
  Doc doc;
  GTEXT_CSSOM_Sheet * s = nullptr;
  EXPECT_EQ(gtext_cssom_sheet_new_parsed(nullptr, "", 0, nullptr, &s), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_sheet_new_parsed(doc.d, nullptr, 3, nullptr, &s), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_sheet_new_parsed(doc.d, "", 0, nullptr, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_sheet_new_parsed(doc.d, nullptr, 0, nullptr, &s), GTEXT_CSS_OK) << "an empty sheet";
  EXPECT_EQ(gtext_cssom_rule_list_length(nullptr), 0u);
  const GTEXT_CSSOM_Rule_List * list = nullptr;
  EXPECT_EQ(gtext_cssom_sheet_rules(nullptr, &list), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_sheet_rules(s, &list), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_rule_list_length(list), 0u);
  gtext_cssom_sheet_release(s);
  gtext_cssom_sheet_release(nullptr);
  gtext_cssom_rule_release(nullptr);
  EXPECT_EQ(gtext_cssom_sheet_version(nullptr), 0u);
  EXPECT_EQ(gtext_cssom_sheet_compact(nullptr), GTEXT_CSS_E_INVALID);
}

// CSSOM, `cssRules`: "If the origin-clean flag is unset, throw a SecurityError exception."
TEST(CssomSheet, ReadingTheRulesOfASheetThatIsNotOriginCleanIsASecurityError) {
  Doc doc;
  GTEXT_CSSOM_Sheet_Options o = gtext_cssom_sheet_options_default();
  o.origin_clean = false;
  Sheet s(doc, "a{}", &o);
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  const GTEXT_CSSOM_Rule_List * list = reinterpret_cast<const GTEXT_CSSOM_Rule_List *>(1);
  EXPECT_EQ(gtext_cssom_sheet_rules(s.s, &list), GTEXT_CSS_E_SECURITY);
  EXPECT_EQ(list, reinterpret_cast<const GTEXT_CSSOM_Rule_List *>(1)) << "written only on success";
  EXPECT_STREQ(gtext_cssom_exception_name(GTEXT_CSS_E_SECURITY), "SecurityError");
  const uint64_t v = gtext_cssom_sheet_version(s.s);
  gtext_cssom_sheet_set_origin_clean(s.s, true);
  EXPECT_EQ(gtext_cssom_sheet_rules(s.s, &list), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_sheet_version(s.s), v) << "the flag is the caller's fact, not a mutation";
}

// "setting [disabled] bumps the version and notifies"; setting the value it has is nothing.
TEST(CssomSheet, DisabledBumpsTheVersionAndSendsOneEventOnlyWhenItChanges) {
  Doc doc;
  Sheet s(doc, "a{}");
  const uint64_t v = gtext_cssom_sheet_version(s.s), gen = gtext_cssom_document_generation(doc.d);
  GTEXT_CSSOM_Outcome o;
  EXPECT_EQ(gtext_cssom_sheet_set_disabled(s.s, false, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_UNCHANGED);
  EXPECT_EQ(gtext_cssom_sheet_version(s.s), v);
  EXPECT_TRUE(doc.events.empty());
  EXPECT_EQ(gtext_cssom_sheet_set_disabled(s.s, true, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_TRUE(gtext_cssom_sheet_disabled(s.s));
  EXPECT_EQ(gtext_cssom_sheet_version(s.s), v + 1);
  EXPECT_EQ(gtext_cssom_document_generation(doc.d), gen + 1);
  ASSERT_EQ(doc.events.size(), 1u);
  EXPECT_EQ(doc.events[0].kind, GTEXT_CSSOM_EVENT_SHEET_DISABLED_CHANGED);
  EXPECT_EQ(gtext_cssom_sheet_set_disabled(nullptr, true, &o), GTEXT_CSS_E_INVALID);
}

// A mutation from inside a notification is refused (the reentrancy flag): the event of the first completes.
TEST(CssomSheet, DisablingFromInsideANotificationIsRefused) {
  Doc doc;
  Sheet s(doc, "a{}");
  GTEXT_CSS_Status inner = GTEXT_CSS_OK;
  doc.inside = [&]() {
    inner = gtext_cssom_sheet_set_disabled(s.s, false, nullptr);
    return GTEXT_CSS_OK;
  };
  EXPECT_EQ(gtext_cssom_sheet_set_disabled(s.s, true, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(inner, GTEXT_CSS_E_REENTRANT);
  EXPECT_TRUE(gtext_cssom_sheet_disabled(s.s));
}

/* ---- limits ------------------------------------------------------------------------------------------------------------------------------------ */

std::string nest(size_t depth) {
  std::string t;
  for (size_t i = 0; i < depth; i++) {
    t += "a{";
  }
  for (size_t i = 0; i < depth; i++) {
    t += "}";
  }
  return t;
}

// "rule nesting (_LIMIT_RULE_NESTING, default 64, at most 4,096)": 64 deep is fine, 65 is the limit's status, and the limit is the document's.
TEST(CssomSheetLimits, RuleNestingIsBoundedByTheDocument) {
  {
    Doc doc;
    Sheet fits(doc, nest(64));
    EXPECT_EQ(fits.status, GTEXT_CSS_OK);
    Sheet over(doc, nest(65));
    EXPECT_EQ(over.status, GTEXT_CSS_E_LIMIT_RULE_NESTING);
    EXPECT_EQ(over.s, nullptr);
    GTEXT_CSSOM_Counters c;
    ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c), GTEXT_CSS_OK);
    EXPECT_EQ(c.live_objects, 64u + 1u) << "the failed parse left nothing";
  }
  {
    Doc doc([](GTEXT_CSSOM_Config & c) { c.max_rule_nesting = 200; });
    Sheet s(doc, nest(200));
    EXPECT_EQ(s.status, GTEXT_CSS_OK);
    Sheet over(doc, nest(201));
    EXPECT_EQ(over.status, GTEXT_CSS_E_LIMIT_RULE_NESTING);
  }
  {
    Doc doc([](GTEXT_CSSOM_Config & c) { c.max_rule_nesting = 1000000; });
    Sheet s(doc, nest(4096));
    EXPECT_EQ(s.status, GTEXT_CSS_OK) << "the limit is capped at 4,096, not lowered";
    Sheet over(doc, nest(4097));
    EXPECT_EQ(over.status, GTEXT_CSS_E_LIMIT_RULE_NESTING);
  }
}

// "a 100,000-deep nest ... no recursion in the C stack": a status, not a crash, and nothing left. (Depth is the parser's limit here, which is a status of its own.)
TEST(CssomSheetLimits, AHundredThousandDeepNestIsAStatusAndNotACrash) {
  Doc doc;
  Sheet deep(doc, nest(100000));
  EXPECT_NE(deep.status, GTEXT_CSS_OK);
  EXPECT_TRUE(deep.status == GTEXT_CSS_E_LIMIT_DEPTH || deep.status == GTEXT_CSS_E_LIMIT_RULE_NESTING) << gtext_css_status_string(deep.status);
  GTEXT_CSSOM_Counters c;
  ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c), GTEXT_CSS_OK);
  EXPECT_EQ(c.live_objects, 0u);
  EXPECT_EQ(c.live_bytes, 0u);
  // and the deepest nest the limit allows is freed without recursion
  Doc doc2([](GTEXT_CSSOM_Config & c2) { c2.max_rule_nesting = 4096; });
  {
    Sheet deepest(doc2, nest(4096));
    ASSERT_EQ(deepest.status, GTEXT_CSS_OK);
  }
  ASSERT_EQ(gtext_cssom_document_counters(doc2.d, &c), GTEXT_CSS_OK);
  EXPECT_EQ(c.live_objects, 0u);
}

TEST(CssomSheetLimits, RulesPerSheetInputAndWorkAreBounded) {
  {
    Doc doc([](GTEXT_CSSOM_Config & c) { c.max_rules = 10; });
    Sheet fits(doc, "a{} b{} c{} d{} e{} f{} g{} h{} i{} j{}");
    EXPECT_EQ(fits.status, GTEXT_CSS_OK);
    Sheet over(doc, "a{} b{} c{} d{} e{} f{} g{} h{} i{} j{} k{}");
    EXPECT_EQ(over.status, GTEXT_CSS_E_LIMIT_RULES);
    Sheet nested(doc, "a{ b{} c{} d{} e{} f{} g{} h{} i{} j{} k{} }");
    EXPECT_EQ(nested.status, GTEXT_CSS_E_LIMIT_RULES) << "the nested rules count";
  }
  {
    // the rules a parse makes are not all the rules the sheet holds: a run of declarations in a grouping rule inside a style rule is a rule of its own
    Doc doc([](GTEXT_CSSOM_Config & c) { c.max_rules = 7; });
    Sheet fits(doc, "a{ @media print{ x:1; & b{} y:2; & c{} z:3 } }");
    EXPECT_EQ(fits.status, GTEXT_CSS_OK);
    EXPECT_EQ(gtext_cssom_sheet_rule_total(fits.s), 7u);
    Sheet over(doc, "a{ @media print{ x:1; & b{} y:2; & c{} z:3; & d{} } }");
    EXPECT_EQ(over.status, GTEXT_CSS_E_LIMIT_RULES) << "eight rules, five of them made by the parse";
  }
  {
    Doc doc([](GTEXT_CSSOM_Config & c) { c.max_input_bytes = 8; });
    Sheet fits(doc, "a{}b{}c{}");
    EXPECT_EQ(fits.status, GTEXT_CSS_E_LIMIT_INPUT);
    Sheet ok(doc, "a{}b{}");
    EXPECT_EQ(ok.status, GTEXT_CSS_OK);
  }
  {
    Doc doc([](GTEXT_CSSOM_Config & c) { c.max_work = 3; });
    Sheet over(doc, "a{ b{} c{} d{} }");
    EXPECT_EQ(over.status, GTEXT_CSS_E_LIMIT_WORK);
    Sheet ok(doc, "a{ b{} c{} }");
    EXPECT_EQ(ok.status, GTEXT_CSS_OK);
  }
  {
    Doc doc([](GTEXT_CSSOM_Config & c) { c.max_live_objects = 4; });
    Sheet over(doc, "a{} b{} c{} d{}");
    EXPECT_EQ(over.status, GTEXT_CSS_E_LIMIT_CSSOM) << "the sheet and three rules are four objects";
    GTEXT_CSSOM_Counters c;
    ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c), GTEXT_CSS_OK);
    EXPECT_EQ(c.live_objects, 0u);
    Sheet ok(doc, "a{} b{} c{}");
    EXPECT_EQ(ok.status, GTEXT_CSS_OK);
  }
  {
    Doc doc([](GTEXT_CSSOM_Config & c) { c.max_live_bytes = 4096; });
    std::string big;
    for (int i = 0; i < 200; i++) {
      big += "a" + std::to_string(i) + "{color:red}";
    }
    Sheet over(doc, big);
    EXPECT_EQ(over.status, GTEXT_CSS_E_LIMIT_CSSOM) << "bytes of the objects and of the trees count";
    GTEXT_CSSOM_Counters c;
    ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c), GTEXT_CSS_OK);
    EXPECT_EQ(c.live_bytes, 0u);
    EXPECT_EQ(c.tree_bytes, 0u);
  }
}

// A sheet of a million rules is held without a call that recurses, in O(source), and is freed whole.
TEST(CssomSheetLimits, AMillionRulesAreParsedHeldAndFreed) {
  Doc doc([](GTEXT_CSSOM_Config & c) { c.max_rules = 1100000; });
  std::string text;
  text.reserve(10u << 20);
  for (int i = 0; i < 1000000; i++) {
    text += "a{}";
  }
  {
    Sheet s(doc, text);
    ASSERT_EQ(s.status, GTEXT_CSS_OK);
    EXPECT_EQ(gtext_cssom_rule_list_length(s.rules()), 1000000u);
    EXPECT_EQ(gtext_cssom_sheet_rule_total(s.s), 1000000u);
    EXPECT_LE(css_rule_list_depth(&s.s->rules), 100u);
    Rule last = at(s.rules(), 999999);
    size_t i = 0;
    ASSERT_TRUE(gtext_cssom_rule_list_index_of(s.rules(), last.r, &i));
    EXPECT_EQ(i, 999999u);
  }
  GTEXT_CSSOM_Counters c;
  ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c), GTEXT_CSS_OK);
  EXPECT_EQ(c.live_objects, 0u);
  EXPECT_EQ(c.live_bytes, 0u);
}

/* ---- compaction -------------------------------------------------------------------------------------------------------------------------------- */

// "Tree arena: delete 999,999 of 10^6 rules, then compact: tree bytes fall; ids, identity and versions unchanged." (Deleting is the list's removal here.)
TEST(CssomSheetCompact, AMostlyDeletedSheetGivesItsMemoryBack) {
  Doc doc;
  std::string text;
  for (int i = 0; i < 20000; i++) {
    text += "r" + std::to_string(i) + " > a, b{color:red; margin:1px 2px} @media screen{ c{width:" + std::to_string(i) + "px} }\n";
  }
  Sheet s(doc, text);
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  GTEXT_CSSOM_Counters before;
  ASSERT_EQ(gtext_cssom_document_counters(doc.d, &before), GTEXT_CSS_OK);
  EXPECT_GT(before.tree_bytes, 1000000u);
  Rule keep = at(s.rules(), 39999);   // the media rule of the last pair
  Rule keep_child = at(gtext_cssom_rule_css_rules(keep.r), 0);
  Rule keep_style = at(s.rules(), 39998);
  const uint64_t keep_id = gtext_cssom_rule_id(keep.r), child_id = gtext_cssom_rule_id(keep_child.r), sheet_id = gtext_cssom_sheet_id(s.s);
  // delete everything but the last pair, from the front
  while (s.s->rules.count > 2) {
    css_rule * r = css_rule_list_first(&s.s->rules);
    css_rule_list_remove(&s.s->rules, r);
    css_obj_release_internal(&r->obj);
  }
  GTEXT_CSSOM_Counters mid;
  ASSERT_EQ(gtext_cssom_document_counters(doc.d, &mid), GTEXT_CSS_OK);
  EXPECT_EQ(mid.tree_bytes, before.tree_bytes) << "the tree lives while any rule of it is attached";
  const std::string dump_before = dump(s.s);
  const uint64_t sheet_version = gtext_cssom_sheet_version(s.s), rule_version = gtext_cssom_rule_version(keep.r);
  const size_t nrules = s.s->rules.count;
  ASSERT_EQ(gtext_cssom_sheet_compact(s.s), GTEXT_CSS_OK);
  GTEXT_CSSOM_Counters after;
  ASSERT_EQ(gtext_cssom_document_counters(doc.d, &after), GTEXT_CSS_OK);
  EXPECT_LT(after.tree_bytes, before.tree_bytes / 50) << "from " << before.tree_bytes << " to " << after.tree_bytes;
  EXPECT_EQ(gtext_cssom_rule_id(keep.r), keep_id);
  EXPECT_EQ(gtext_cssom_rule_id(keep_child.r), child_id);
  EXPECT_EQ(gtext_cssom_sheet_id(s.s), sheet_id);
  EXPECT_EQ(gtext_cssom_sheet_version(s.s), sheet_version);
  EXPECT_EQ(gtext_cssom_rule_version(keep.r), rule_version);
  EXPECT_EQ(s.s->rules.count, nrules);
  Rule again = at(s.rules(), 1);
  EXPECT_EQ(again.r, keep.r) << "identity: the objects are the same objects";
  // the rules read as they did, from the copy of the tree
  Decl style = style_of(keep_child.r);
  EXPECT_EQ(text_of(style.d), "width: 19999px;");
  Decl s2 = style_of(keep_style.r);
  EXPECT_EQ(text_of(s2.d), "color: red; margin: 1px 2px;");
  GTEXT_CSSOM_Source_Info info;
  ASSERT_EQ(gtext_cssom_rule_source(keep.r, &info), GTEXT_CSS_OK);
  EXPECT_EQ(info.kind, GTEXT_CSSOM_SOURCE_MODIFIED) << "the span no longer says where the rule is";
  char * out = nullptr;
  EXPECT_EQ(gtext_cssom_rule_source_text(keep.r, &out, nullptr), GTEXT_CSS_E_INVALID_STATE);
  EXPECT_EQ(gtext_cssom_sheet_source_text(s.s, &out, nullptr), GTEXT_CSS_E_INVALID_STATE);
  // the dump differs in the source kind and id only
  EXPECT_NE(dump(s.s), dump_before);
  EXPECT_EQ(dump(s.s).find("parsed"), std::string::npos);
  // compacting again changes nothing observable
  const uint64_t tb = after.tree_bytes;
  ASSERT_EQ(gtext_cssom_sheet_compact(s.s), GTEXT_CSS_OK);
  GTEXT_CSSOM_Counters twice;
  ASSERT_EQ(gtext_cssom_document_counters(doc.d, &twice), GTEXT_CSS_OK);
  EXPECT_LE(twice.tree_bytes, tb + 64);
}

// Every rule of a sheet survives a compaction: the shape, the text of every declaration block, the nested-declarations runs and the keyframes.
TEST(CssomSheetCompact, CopiesEveryKindOfNodeFaithfully) {
  Doc doc;
  const std::string text =
      "@namespace s url(urn:s); a{ color:red; & b{ width:1px } height:2px; @media print{ margin:1px; & c{} top:0 } }"
      " @media screen{ d{ e{} f:g } } @keyframes k{ from{ opacity:0 } 50%{ opacity:.5 } to{ opacity:1 } }"
      " @page :first{ margin:1px; @top-left{ content:\"x\" } } @supports (a:b){ @layer l{ x{ y:z } } } @font-face{font-family:q;src:url(a.woff2)}";
  Sheet a(doc, text);
  Sheet b(doc, text);
  ASSERT_EQ(a.status, GTEXT_CSS_OK);
  ASSERT_EQ(shape(a), shape(b));
  // read every block of `b` before, of `a` after, and compare
  std::vector<std::string> before;
  std::function<void(const GTEXT_CSSOM_Rule_List *, std::vector<std::string> &)> blocks = [&](const GTEXT_CSSOM_Rule_List * list, std::vector<std::string> & out) {
    for (GTEXT_CSSOM_Rule * r = gtext_cssom_rule_list_first(list); r;) {
      GTEXT_CSS_Status st = GTEXT_CSS_OK;
      Decl d = style_of(r, &st);
      out.push_back(std::string(gtext_cssom_rule_kind_name(gtext_cssom_rule_kind(r))) + ":" + (d.d ? text_of(d.d) : "-"));
      if (const GTEXT_CSSOM_Rule_List * inner = gtext_cssom_rule_css_rules(r)) {
        blocks(inner, out);
      }
      GTEXT_CSSOM_Rule * next = gtext_cssom_rule_list_next(list, r);
      gtext_cssom_rule_release(r);
      r = next;
    }
  };
  blocks(b.rules(), before);
  ASSERT_EQ(gtext_cssom_sheet_compact(a.s), GTEXT_CSS_OK);
  std::vector<std::string> after;
  blocks(a.rules(), after);
  EXPECT_EQ(before, after);
  EXPECT_EQ(shape(a), shape(b));
  EXPECT_GT(before.size(), 12u);
}

// An allocation failure at any point of a compaction leaves the sheet as it was.
TEST(CssomSheetCompact, IsAllOrNothingUnderAllocationFailure) {
  size_t failures = 0;
  for (size_t n = 1; n < 500; n++) {
    FaultAllocator fa;
    {
      Doc doc(nullptr, &fa.alloc);
      Sheet s(doc, "a{ b{} } @media print{ c{ color:red } } d{}");
      ASSERT_EQ(s.status, GTEXT_CSS_OK);
      const std::string before = dump(s.s);
      GTEXT_CSSOM_Counters c0;
      ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c0), GTEXT_CSS_OK);
      fa.arm(n);
      const GTEXT_CSS_Status st = gtext_cssom_sheet_compact(s.s);
      fa.disarm();
      if (!fa.failed) {
        EXPECT_EQ(st, GTEXT_CSS_OK);
        break;
      }
      failures++;
      EXPECT_EQ(st, GTEXT_CSS_E_OOM);
      EXPECT_EQ(dump(s.s), before);
      GTEXT_CSSOM_Counters c1;
      ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c1), GTEXT_CSS_OK);
      EXPECT_EQ(c1.live_bytes, c0.live_bytes);
      EXPECT_EQ(c1.tree_bytes, c0.tree_bytes);
      EXPECT_EQ(c1.live_objects, c0.live_objects);
    }
    EXPECT_TRUE(fa.live.empty()) << n;
    EXPECT_EQ(fa.bad_frees, 0u);
  }
  EXPECT_GT(failures, 0u);
}

/* ---- the dump ------------------------------------------------------------------------------------------------------------------------------------ */

// The dump is plain text that depends on the calls and nothing else, and its hash is the SHA-256 of it.
TEST(CssomSheetDump, IsDeterministicAndItsHashIsOfTheText) {
  const std::string text = "@import url(a.css); a{color:red; & b{} width:1px} @media print{ c{} } @layer x; bogus >{}";
  std::string dumps[2];
  unsigned char hashes[2][GTEXT_CSSOM_HASH_LENGTH];
  for (int run = 0; run < 2; run++) {
    Doc doc;
    GTEXT_CSSOM_Sheet_Options o = gtext_cssom_sheet_options_default();
    o.href = "h\"ref\n";
    Sheet s(doc, text, &o);
    ASSERT_EQ(s.status, GTEXT_CSS_OK);
    dumps[run] = dump(s.s);
    ASSERT_EQ(gtext_cssom_sheet_hash(s.s, hashes[run]), GTEXT_CSS_OK);
  }
  EXPECT_EQ(dumps[0], dumps[1]);
  EXPECT_EQ(std::memcmp(hashes[0], hashes[1], GTEXT_CSSOM_HASH_LENGTH), 0);
  EXPECT_EQ(dumps[0].rfind("cssom-sheet 1\nsheet ", 0), 0u);
  EXPECT_NE(dumps[0].find("href \"h\\x22ref\\x0a\""), std::string::npos) << dumps[0];
  EXPECT_NE(dumps[0].find("diag invalid-selector 1:"), std::string::npos);
  EXPECT_NE(dumps[0].find("rule "), std::string::npos);
  // the rule's dump covers the rule and what is under it
  Doc doc;
  Sheet s(doc, text);
  Rule a = at(s.rules(), 1);
  const std::string rd = dump(a.r);
  EXPECT_EQ(rd.rfind("cssom-rule 1\nrule ", 0), 0u);
  EXPECT_NE(rd.find(" style lazy"), std::string::npos);
  EXPECT_EQ(std::count(rd.begin(), rd.end(), '\n'), 1 + 1 + 2) << "the header, the rule, its two children";
  // building the block shows in the dump, and does not change the other rules'
  Decl d = style_of(a.r);
  EXPECT_NE(dump(a.r).find(" style version 0"), std::string::npos);
  unsigned char h1[GTEXT_CSSOM_HASH_LENGTH];
  ASSERT_EQ(gtext_cssom_rule_hash(a.r, h1), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_rule_hash(nullptr, h1), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_sheet_dump(nullptr, nullptr), GTEXT_CSS_E_INVALID);
}

} // namespace
