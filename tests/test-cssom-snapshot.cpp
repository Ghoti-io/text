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
 * Snapshots (css_cssom_sheet.h): the compiled form of a live sheet. The central property is that the dump of a snapshot equals the dump of the compile of the sheet's own
 * serialization, after every step of every sequence of mutations; the rest is what makes a snapshot a thing the cascade can lean on: it is the same pointer while nothing changes,
 * it is never altered once handed out (across 10^5 mutations), a rule nobody changed keeps its spans while a changed one is read from its text, an import's sheet is a snapshot one level
 * down and a change of it is a change of the sheet that imports it, an import cannot make a cycle or have two owners, and the composite version moves exactly when something does.
 */

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

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

/// A snapshot that releases itself.
struct Snap {
  GTEXT_CSS_Sheet * s = nullptr;
  GTEXT_CSS_Status st = GTEXT_CSS_E_INTERNAL;
  explicit Snap(GTEXT_CSSOM_Sheet * sheet) {
    st = gtext_cssom_sheet_snapshot(sheet, &s);
  }
  Snap(const Snap &) = delete;
  Snap & operator=(const Snap &) = delete;
  ~Snap() {
    gtext_css_sheet_free(s);
  }
};

/// The compile of a text, for the reference: the sheet parsed and compiled as the cascade would have it from that text.
std::string compile_dump(const std::string & text) {
  GTEXT_CSS_Tree * tree = nullptr;
  EXPECT_EQ(gtext_css_parse_stylesheet(text.data(), text.size(), nullptr, &tree, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Sheet * s = nullptr;
  EXPECT_EQ(gtext_css_sheet_compile(tree, R(), nullptr, &s, nullptr), GTEXT_CSS_OK);
  const std::string d = dump_of(s);
  gtext_css_sheet_free(s);
  gtext_css_tree_free(tree);
  return d;
}

/// The property: the snapshot's dump equals the dump of the compile of the serialization.
::testing::AssertionResult equals_rebuild(GTEXT_CSSOM_Sheet * sheet) {
  Snap snap(sheet);
  if (snap.st != GTEXT_CSS_OK) {
    return ::testing::AssertionFailure() << "snapshot: " << gtext_css_status_string(snap.st);
  }
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  const std::string text = sheet_text(sheet, &st);
  if (st != GTEXT_CSS_OK) {
    return ::testing::AssertionFailure() << text;
  }
  const std::string want = compile_dump(text);
  const std::string got = dump_of(snap.s);
  if (want != got) {
    return ::testing::AssertionFailure() << "serialization:\n" << text << "\nsnapshot:\n" << got << "\nrebuild:\n" << want;
  }
  return ::testing::AssertionSuccess();
}

GTEXT_CSSOM_Counters counters(Doc & doc) {
  GTEXT_CSSOM_Counters c;
  EXPECT_EQ(gtext_cssom_document_counters(doc.d, &c), GTEXT_CSS_OK);
  return c;
}

/* ---- the basics ------------------------------------------------------------------------------------------------------------------------------ */

TEST(CssomSnapshot, ASheetIsCompiledAsItsSerializationIs) {
  const char * const texts[] = {
      "",
      "a{color:red} b > c{margin:1px 2px} @media print{ d{width:1px} @supports (a:b){ e{color:blue} } }",
      "@namespace s url(urn:s); s|a{ color:red; b{ width:1px } margin:2px; & c{ height:1px } }",
      "@layer base, theme; @layer base{ a{color:red} } @layer theme{ a{color:blue} @layer inner{ b{margin:0} } } @layer{ c{padding:0} } d{width:1px}",
      "@scope (.a) to (.b){ p{color:red} } @container (min-width:10px){ r{color:green} } @starting-style{ t{color:red} }",
      "@keyframes k{ from{a:b} 50%{color:red} } @font-face{font-family:q;src:url(a)} @page :first{ margin:1px } @property --x{syntax:'<length>';inherits:false;initial-value:0px}",
      "@charset \"utf-8\"; @bogus; x >{} a{color:red;bogus:1;width:oops} b{color:blue}",
      "a{ color:red; b{ color:blue } color:green; c{ & d{ width:1px } } }",
      // what the syntax layer and the object model each decide about what is inside a rule, which the snapshot must not read twice differently
      "a{ color:red; @bogus x; & b{width:1px} margin:2px }",
      "a{ b{ color:red } c; width:1px }",
      "a{ --x:{a:b}; color:red; & b{ --y: [1]; margin:0 } }",
      "a{ x >{} color:red; & > e{ width:1px } color:blue }",
      "a{ @import url(x.css); color:red; @font-face{ src:url(a) } & b{ margin:0 } }",
      "@media print{ a{ color:red; ; b{} } @bogus; c{ d{ e{ f{ color:blue } } } } }",
      "a{ color:red; @media (min-width:1px){ color:blue; } & b{ width:1px } color:green }",
      // declarations that the block writes differently from the text: a shorthand and its longhand, a property twice, an important one before a plain one, an invalid one
      "a{margin:1px; margin-top:2px; color:red}",
      "a{margin-top:2px; margin:1px 3px}",
      "a{color:red; color:blue} b{color:red !important; color:blue} c{width:oops; height:1px}",
      "a{border:1px solid red; border-color:blue; & b{ margin:0; margin-left:1px } }",
      "a{--x:  1px   2px; --y:{a:b}; --z: RED; COLOR:red; Width:1PX}",
      // what the text holds that the block writes differently, found only inside: a custom property's spacing, a name's case in a nested rule, in a nested group's own declarations
      "a{--x:  1px   2px}",
      "a{margin:1px 1px 1px 1px} b{padding:1px 2px 1px 2px} c{margin:0; width:0}",
      "a{ & b{ COLOR:red } }",
      "q{ @media (min-width:1px){ COLOR:red; r{ margin:0 } } }",
      "@media print{ a{ --x:  1px   2px; b{ --y:  q   r } } }",
      "@media print{ a{ margin:1px; margin-top:2px } } @layer l{ b{ padding:0; padding-left:1px } } @scope (.a){ c{ top:0; top:1px } }",
  };
  for (const char * text : texts) {
    Doc doc;
    Sheet s(doc, text);
    ASSERT_EQ(s.status, GTEXT_CSS_OK) << text;
    EXPECT_TRUE(equals_rebuild(s.s)) << text;
  }
}

TEST(CssomSnapshot, TheSamePointerWhileNothingChangesAndANewOneAfter) {
  Doc doc;
  Sheet s(doc, "a{color:red} b{color:blue}");
  Snap one(s.s), two(s.s);
  ASSERT_EQ(one.st, GTEXT_CSS_OK);
  EXPECT_EQ(one.s, two.s) << "n readers cost one compile";
  EXPECT_EQ(counters(doc).snapshot_compiles, 1u);
  EXPECT_EQ(counters(doc).snapshot_hits, 1u);
  const std::string before = dump_of(one.s);
  ASSERT_EQ(insert_rule(s.s, "c{color:green}", 2).st, GTEXT_CSS_OK);
  Snap three(s.s);
  EXPECT_NE(three.s, one.s);
  EXPECT_EQ(gtext_css_sheet_record_count(three.s), 3u);
  EXPECT_EQ(gtext_css_sheet_record_count(one.s), 2u) << "the first is as it was";
  EXPECT_EQ(dump_of(one.s), before);
  Snap four(s.s);
  EXPECT_EQ(four.s, three.s);
  EXPECT_EQ(counters(doc).snapshot_compiles, 2u);
}

TEST(CssomSnapshot, ReadingChangesNothing) {
  Doc doc;
  Sheet s(doc, "a{color:red} @media print{ b{color:blue} }");
  const uint64_t version = gtext_cssom_sheet_version(s.s);
  const uint64_t generation = gtext_cssom_document_generation(doc.d);
  const std::string before = dump(s.s);
  const size_t events = doc.events.size();
  uint64_t c0 = 0, c1 = 0;
  ASSERT_EQ(gtext_cssom_sheet_composite_version(s.s, &c0), GTEXT_CSS_OK);
  {
    Snap a(s.s);
    Snap b(s.s);
    ASSERT_EQ(a.st, GTEXT_CSS_OK);
  }
  ASSERT_EQ(gtext_cssom_sheet_composite_version(s.s, &c1), GTEXT_CSS_OK);
  EXPECT_EQ(c0, c1);
  EXPECT_EQ(gtext_cssom_sheet_version(s.s), version);
  EXPECT_EQ(gtext_cssom_document_generation(doc.d), generation);
  EXPECT_EQ(doc.events.size(), events);
  EXPECT_EQ(dump(s.s), before);
}

TEST(CssomSnapshot, ASnapshotSurvivesTheSheetTheDocumentAndTheLibraryObjects) {
  GTEXT_CSS_Sheet * kept = nullptr;
  std::string want;
  {
    Doc doc;
    Sheet s(doc, "a{color:red} @layer x{ b{width:1px} }");
    Snap snap(s.s);
    ASSERT_EQ(snap.st, GTEXT_CSS_OK);
    want = dump_of(snap.s);
    kept = gtext_css_sheet_retain(snap.s);
    doc.release();   // the document handle goes first; the sheet is still held
    Snap again(s.s);
    EXPECT_EQ(again.st, GTEXT_CSS_OK) << "a snapshot still works on a graph no document evaluates";
    EXPECT_EQ(again.s, snap.s);
  }
  EXPECT_EQ(dump_of(kept), want);
  gtext_css_sheet_free(kept);
}

TEST(CssomSnapshot, ANotOriginCleanAndADisabledSheetAreSnapshottedAll) {
  Doc doc;
  GTEXT_CSSOM_Sheet_Options o = gtext_cssom_sheet_options_default();
  o.origin_clean = false;
  o.disabled = true;
  Sheet s(doc, "a{color:red}", &o);
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  Snap snap(s.s);
  ASSERT_EQ(snap.st, GTEXT_CSS_OK) << "the flag guards the script's access, not the cascade's";
  EXPECT_EQ(gtext_css_sheet_record_count(snap.s), 1u);
}

TEST(CssomSnapshot, ArgumentsAreChecked) {
  Doc doc;
  Sheet s(doc, "a{}");
  GTEXT_CSS_Sheet * out = nullptr;
  EXPECT_EQ(gtext_cssom_sheet_snapshot(nullptr, &out), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_sheet_snapshot(s.s, nullptr), GTEXT_CSS_E_INVALID);
  uint64_t v = 0;
  EXPECT_EQ(gtext_cssom_sheet_composite_version(nullptr, &v), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_sheet_composite_version(s.s, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(out, nullptr);
}

TEST(CssomSnapshot, AConstructedSheetAndAnEmptyOneAreSnapshotted) {
  Doc doc;
  GTEXT_CSSOM_Sheet * c = nullptr;
  ASSERT_EQ(gtext_cssom_sheet_new_constructed(doc.d, nullptr, &c), GTEXT_CSS_OK);
  {
    Snap empty(c);
    ASSERT_EQ(empty.st, GTEXT_CSS_OK);
    EXPECT_EQ(gtext_css_sheet_record_count(empty.s), 0u);
  }
  ASSERT_EQ(gtext_cssom_sheet_replace_sync(c, "a{color:red} @media print{ b{width:1px} }", 40, nullptr), GTEXT_CSS_OK);
  EXPECT_TRUE(equals_rebuild(c));
  ASSERT_EQ(gtext_cssom_sheet_replace_sync(c, "", 0, nullptr), GTEXT_CSS_OK);
  Snap none(c);
  EXPECT_EQ(gtext_css_sheet_record_count(none.s), 0u) << "replacing is a change: the snapshot is of the new rules";
  gtext_cssom_sheet_release(c);
}

/* ---- what is read from where ------------------------------------------------------------------------------------------------------------------ */

TEST(CssomSnapshot, ARuleNobodyChangedKeepsItsSpansAndAChangedOneIsReadFromItsText) {
  Doc doc;
  const std::string text = "a{color:red}  b{color:blue}  c{color:green}";
  Sheet s(doc, text);
  Rule b = path(s, {1});
  GTEXT_CSSOM_Source_Info info;
  ASSERT_EQ(gtext_cssom_rule_source(b.r, &info), GTEXT_CSS_OK);
  {
    Snap snap(s.s);
    ASSERT_EQ(snap.st, GTEXT_CSS_OK);
    GTEXT_CSS_Sheet_Record r;
    ASSERT_TRUE(gtext_css_sheet_record(snap.s, 1, &r));
    EXPECT_EQ(r.span.offset, text.find("b{"));
    EXPECT_EQ(r.source_id, info.source_id) << "the span indexes the bytes of the parse the rule came from";
    EXPECT_NE(r.source_id, 0u);
    EXPECT_EQ(r.source_index, 1u);
    EXPECT_EQ(counters(doc).snapshot_rules_reused, 3u);
    EXPECT_EQ(counters(doc).snapshot_rules_reparsed, 0u);
  }
  Decl d = style_of(b.r);
  ASSERT_TRUE(set(d.d, "width", "1px").o.kind == GTEXT_CSSOM_CHANGED);
  Snap snap(s.s);
  ASSERT_EQ(snap.st, GTEXT_CSS_OK);
  GTEXT_CSS_Sheet_Record r0, r1, r2;
  ASSERT_TRUE(gtext_css_sheet_record(snap.s, 0, &r0));
  ASSERT_TRUE(gtext_css_sheet_record(snap.s, 1, &r1));
  ASSERT_TRUE(gtext_css_sheet_record(snap.s, 2, &r2));
  EXPECT_EQ(r0.span.offset, text.find("a{")) << "the others keep their place in their text";
  EXPECT_EQ(r2.span.offset, text.find("c{"));
  EXPECT_EQ(r1.source_id, 0u) << "the changed rule is read from its own serialization, whose bytes are not kept";
  EXPECT_EQ(r0.source_id, info.source_id);
  EXPECT_EQ(counters(doc).snapshot_rules_reused, 3u + 2u);
  EXPECT_EQ(counters(doc).snapshot_rules_reparsed, 1u);
  EXPECT_EQ(gtext_css_sheet_record_declarations(snap.s, 1, nullptr, nullptr, 0), 2u) << "and it has the change";
  EXPECT_TRUE(equals_rebuild(s.s));
}

TEST(CssomSnapshot, AChangeInsideAGroupRereadsTheTopLevelRuleItIsIn) {
  Doc doc;
  Sheet s(doc, "x{color:red} @media print{ a{color:red} b{color:blue} } y{color:red}");
  { Snap warm(s.s); }
  const GTEXT_CSSOM_Counters before = counters(doc);
  Rule a = path(s, {1, 0});
  Decl d = style_of(a.r);
  ASSERT_EQ(set(d.d, "margin", "1px").o.kind, GTEXT_CSSOM_CHANGED);
  Snap snap(s.s);
  ASSERT_EQ(snap.st, GTEXT_CSS_OK);
  const GTEXT_CSSOM_Counters after = counters(doc);
  EXPECT_EQ(after.snapshot_rules_reparsed - before.snapshot_rules_reparsed, 1u) << "the @media rule, whose child changed (its node has the old child)";
  EXPECT_EQ(after.snapshot_rules_reused - before.snapshot_rules_reused, 2u) << "x and y are read as they were";
  EXPECT_TRUE(equals_rebuild(s.s));
}

TEST(CssomSnapshot, ACompactionDoesNotLeaveAStaleNode) {
  Doc doc;
  Sheet s(doc, "p{color:red} @media print{ a{color:red} b{color:blue} c{color:green} } q{ color:red; r{width:1px} }");
  ASSERT_EQ(delete_rule(path(s, {1}).r, 1), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_sheet_compact(s.s), GTEXT_CSS_OK);
  EXPECT_TRUE(equals_rebuild(s.s)) << "the copy of a group's syntax has not its rules; the snapshot must not use it";
  ASSERT_EQ(delete_rule(s.s, 0), GTEXT_CSS_OK);
  EXPECT_TRUE(equals_rebuild(s.s));
}

TEST(CssomSnapshot, ARuleInsertedIntoASheetIsReadFromItsOwnTree) {
  Doc doc;
  Sheet s(doc, "a{color:red}");
  ASSERT_EQ(insert_rule(s.s, "@layer l{ b{color:blue} }", 0).st, GTEXT_CSS_OK);
  ASSERT_EQ(insert_rule(s.s, "@layer m{ c{color:blue} }", 1).st, GTEXT_CSS_OK);
  const GTEXT_CSSOM_Counters before = counters(doc);
  Snap snap(s.s);
  ASSERT_EQ(snap.st, GTEXT_CSS_OK);
  const GTEXT_CSSOM_Counters after = counters(doc);
  EXPECT_EQ(after.snapshot_rules_reused - before.snapshot_rules_reused, 3u) << "an inserted rule has not been changed since it was read";
  EXPECT_EQ(after.snapshot_rules_reparsed, before.snapshot_rules_reparsed);
  EXPECT_TRUE(equals_rebuild(s.s)) << "three trees, the first two with the same ids";
}

TEST(CssomSnapshot, LayerOccurrencesOfRulesFromSeparateTreesDoNotRunTogether) {
  // every inserted rule is a tree of its own and numbers its rules from 1: the occurrences a statement made in one are not another's
  Doc doc;
  Sheet s(doc, "");
  const char * const rules[] = {"@media print{ @layer c, d; }", "@media screen{ @layer e; }", "@media print{ @layer a{ x{color:red} } }", "@media screen{ @layer b{ y{color:red} } }",
      "@media print{ @layer f, g; }", "@media print{ @layer h; }"};
  size_t at_index = 0;
  for (const char * r : rules) {
    ASSERT_EQ(insert_rule(s.s, r, at_index++).st, GTEXT_CSS_OK) << r;
  }
  EXPECT_TRUE(equals_rebuild(s.s));
}

TEST(CssomSnapshot, ARulesSyntaxIsCheckedOnceAgainstWhatTheObjectModelWritesAndOnlyAMatchKeepsItsSpans) {
  Doc doc;
  // a: as written; g: a shorthand and its longhand (the block writes one shorthand); h: a property twice; i: as written; j: not as the block writes it (upper case name)
  const std::string text = "a { color: red; }\ng{margin:1px;margin-top:2px}\nh{color:red;color:blue}\ni { width: 1px; }\nj{COLOR:red}";
  Sheet s(doc, text);
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  GTEXT_CSSOM_Source_Info info;
  ASSERT_EQ(gtext_cssom_rule_source(path(s, {0}).r, &info), GTEXT_CSS_OK);
  {
    Snap snap(s.s);
    ASSERT_EQ(snap.st, GTEXT_CSS_OK);
    const GTEXT_CSSOM_Counters c = counters(doc);
    EXPECT_EQ(c.snapshot_rules_checked, 5u) << "each rule once";
    EXPECT_EQ(c.snapshot_rules_reused, 2u) << "a and i are what the object model writes";
    EXPECT_EQ(c.snapshot_rules_reparsed, 3u);
    GTEXT_CSS_Sheet_Record r;
    ASSERT_TRUE(gtext_css_sheet_record(snap.s, 0, &r));
    EXPECT_EQ(r.source_id, info.source_id);
    EXPECT_EQ(r.span.offset, 0u);
    ASSERT_TRUE(gtext_css_sheet_record(snap.s, 1, &r));
    EXPECT_EQ(r.source_id, 0u) << "g is read from its serialization";
    ASSERT_TRUE(gtext_css_sheet_record(snap.s, 3, &r));
    EXPECT_EQ(r.source_id, info.source_id);
    EXPECT_EQ(r.span.offset, text.find("i {"));
  }
  EXPECT_TRUE(equals_rebuild(s.s));
  // a change elsewhere: the others are not checked again, and what was found is kept
  const GTEXT_CSSOM_Counters before = counters(doc);
  ASSERT_EQ(insert_rule(s.s, "k{color:red}", 5).st, GTEXT_CSS_OK);
  Snap again(s.s);
  ASSERT_EQ(again.st, GTEXT_CSS_OK);
  const GTEXT_CSSOM_Counters after = counters(doc);
  EXPECT_EQ(after.snapshot_rules_checked - before.snapshot_rules_checked, 1u) << "only the new rule";
  EXPECT_EQ(after.snapshot_rules_reused - before.snapshot_rules_reused, 3u);
  EXPECT_EQ(after.snapshot_rules_reparsed - before.snapshot_rules_reparsed, 3u);
  EXPECT_TRUE(equals_rebuild(s.s)) << "g, h and j are still read from their text";
}

TEST(CssomSnapshot, ManyMutationsThenOneSnapshotIsOneCompile) {
  Doc doc;
  std::string text;
  for (int i = 0; i < 200; i++) {
    text += "r" + std::to_string(i) + "{color:red} ";
  }
  Sheet s(doc, text);
  Rule r = path(s, {100});
  Decl d = style_of(r.r);
  for (int i = 0; i < 10000; i++) {
    ASSERT_EQ(set(d.d, "width", std::to_string(i % 97 + 1) + "px").st, GTEXT_CSS_OK);
  }
  const GTEXT_CSSOM_Counters before = counters(doc);
  Snap snap(s.s);
  ASSERT_EQ(snap.st, GTEXT_CSS_OK);
  const GTEXT_CSSOM_Counters after = counters(doc);
  EXPECT_EQ(after.snapshot_compiles - before.snapshot_compiles, 1u);
  EXPECT_EQ(after.snapshot_rules_reparsed - before.snapshot_rules_reparsed, 1u);
  EXPECT_TRUE(equals_rebuild(s.s));
}

TEST(CssomSnapshot, ASnapshotHandedOutIsNeverAlteredByAHundredThousandMutations) {
  Doc doc;
  Sheet s(doc, "a{color:red} @media print{ b{color:blue} } @layer l{ c{width:1px} } d{ e{margin:0} }");
  Snap first(s.s);
  ASSERT_EQ(first.st, GTEXT_CSS_OK);
  const std::string want = dump_of(first.s);
  const size_t records = gtext_css_sheet_record_count(first.s);
  Rule a = path(s, {0});
  Decl d = style_of(a.r);
  GTEXT_CSS_Sheet * last = nullptr;
  for (int i = 0; i < 100000; i++) {
    switch (i % 5) {
    case 0: set(d.d, "width", std::to_string(i % 89 + 1) + "px"); break;
    case 1: set(d.d, "color", i % 2 ? "red" : "blue"); break;
    case 2: {
      Ins in = insert_rule(s.s, "z" + std::to_string(i % 7) + "{color:red}", gtext_cssom_rule_list_length(s.rules()));
      if (in.st == GTEXT_CSS_OK && gtext_cssom_rule_list_length(s.rules()) > 40) {
        delete_rule(s.s, 40);
      }
      break;
    }
    case 3: remove(d.d, "width"); break;
    case 4:
      if (i % 5000 == 4) {
        if (last) {
          gtext_css_sheet_free(last);
        }
        ASSERT_EQ(gtext_cssom_sheet_snapshot(s.s, &last), GTEXT_CSS_OK);
        ASSERT_EQ(dump_of(first.s), want) << "after " << i << " mutations";
      }
      break;
    }
  }
  EXPECT_EQ(dump_of(first.s), want);
  EXPECT_EQ(gtext_css_sheet_record_count(first.s), records);
  EXPECT_NE(dump_of(last), want);
  gtext_css_sheet_free(last);
  EXPECT_TRUE(equals_rebuild(s.s));
}

TEST(CssomSnapshot, ASheetOfADocumentDoesNotRecompileForAnotherSheetsChange) {
  Doc doc;
  Sheet one(doc, "a{color:red}");
  Sheet two(doc, "b{color:blue}");
  Snap a(one.s), b(two.s);
  const uint64_t compiles = counters(doc).snapshot_compiles;
  ASSERT_EQ(insert_rule(one.s, "c{color:green}", 1).st, GTEXT_CSS_OK);
  Snap b2(two.s);
  EXPECT_EQ(b2.s, b.s) << "the other sheet's snapshot is the same pointer";
  EXPECT_EQ(counters(doc).snapshot_compiles, compiles);
  Snap a2(one.s);
  EXPECT_NE(a2.s, a.s);
  EXPECT_EQ(counters(doc).snapshot_compiles, compiles + 1);
}

TEST(CssomSnapshot, ANestedDeclarationsRuleAndAnInsertedOneAreRead) {
  Doc doc;
  Sheet s(doc, "a{ color:red; b{ width:1px } margin:2px }");
  Rule a = path(s, {0});
  ASSERT_EQ(insert_rule(a.r, "padding:1px", 1).st, GTEXT_CSS_OK);
  ASSERT_EQ(insert_rule(a.r, "& c{ height:3px }", 0).st, GTEXT_CSS_OK);
  EXPECT_TRUE(equals_rebuild(s.s));
  ASSERT_EQ(delete_rule(a.r, 0), GTEXT_CSS_OK);
  EXPECT_TRUE(equals_rebuild(s.s));
}

/* ---- the property over sequences -------------------------------------------------------------------------------------------------------------- */

struct Rng {
  uint64_t x;
  explicit Rng(uint64_t seed) : x(seed * 0x9E3779B97F4A7C15ull + 1) {}
  uint64_t next() {
    x += 0x9E3779B97F4A7C15ull;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  size_t below(size_t n) {
    return n ? static_cast<size_t>(next() % n) : 0;
  }
};

const char * const kRules[] = {
    "a{color:red}", "b > c{margin:1px 2px}", "@media print{ d{width:1px} e{color:blue} }", "@supports (display:grid){ f{display:grid} }", "@layer base, theme;",
    "@layer base{ g{color:red} }", "@layer{ h{padding:0} }", "@namespace s url(urn:s);", "s|i{color:red}", "j{ color:red; k{ width:1px } margin:2px }", "@keyframes kf{ from{color:red} to{color:blue} }",
    "@font-face{font-family:q;src:url(a)}", "@page :first{ margin:1px }", "@scope (.a) to (.b){ l{color:red} }", "@container (min-width:10px){ m{color:green} }",
    "@starting-style{ n{color:red} }", "@import url(x.css);", "@import url(y.css) layer(l) screen;", "@property --p{syntax:'<length>';inherits:false;initial-value:0px}",
    "@counter-style cs{ system:cyclic; symbols:'a' 'b' }", "o:hover{ & p{ color:red } }", "q{ @media (min-width:1px){ color:blue; r{ margin:0 } } }",
    "n1{margin:1px;margin-top:2px}", "n2{color:red;color:blue;& m{width:1px;width:2px}}", "n3{border:1px solid red; border-color:blue}", "@media print{ n4{padding:0;padding-left:1px;--x:  a   b} }",
};
const char * const kProps[][2] = {{"color", "red"}, {"color", "blue"}, {"width", "3px"}, {"margin", "1px 2px 3px"}, {"display", "grid"}, {"--v", "1"}, {"padding", "0"}, {"height", "var(--v)"}};
const char * const kSelectors[] = {"a", "b c", ".x > y", "&", "z:hover", "s|w"};

/// A rule picked at random: a top-level rule, or one inside it.
Rule pick(const Sheet & s, Rng & rng, std::vector<size_t> * where = nullptr) {
  const GTEXT_CSSOM_Rule_List * list = s.rules();
  Rule cur;
  for (int depth = 0; depth < 4; depth++) {
    const size_t n = gtext_cssom_rule_list_length(list);
    if (!n) {
      break;
    }
    const size_t i = rng.below(n);
    Rule next = at(list, i);
    if (!next) {
      break;
    }
    if (where) {
      where->push_back(i);
    }
    cur = std::move(next);
    list = gtext_cssom_rule_css_rules(cur.r);
    if (!list || rng.below(3) == 0) {
      break;
    }
  }
  return cur;
}

void step(Sheet & s, Rng & rng) {
  switch (rng.below(13)) {
  case 0:
  case 1: {
    const size_t n = gtext_cssom_rule_list_length(s.rules());
    insert_rule(s.s, kRules[rng.below(sizeof kRules / sizeof *kRules)], rng.below(n + 1));
    break;
  }
  case 2: {
    const size_t n = gtext_cssom_rule_list_length(s.rules());
    if (n) {
      delete_rule(s.s, rng.below(n));
    }
    break;
  }
  case 3:
  case 4: {
    Rule r = pick(s, rng);
    GTEXT_CSS_Status st = GTEXT_CSS_OK;
    Decl d = r ? style_of(r.r, &st) : Decl();
    if (d.d) {
      const auto & p = kProps[rng.below(sizeof kProps / sizeof *kProps)];
      set(d.d, p[0], p[1], rng.below(4) == 0 ? "important" : "");
    }
    break;
  }
  case 5: {
    Rule r = pick(s, rng);
    GTEXT_CSS_Status st = GTEXT_CSS_OK;
    Decl d = r ? style_of(r.r, &st) : Decl();
    if (d.d) {
      remove(d.d, kProps[rng.below(sizeof kProps / sizeof *kProps)][0]);
    }
    break;
  }
  case 6: {
    Rule r = pick(s, rng);
    if (r && gtext_cssom_rule_kind(r.r) == GTEXT_CSSOM_RULE_STYLE) {
      const char * sel = kSelectors[rng.below(sizeof kSelectors / sizeof *kSelectors)];
      gtext_cssom_rule_set_selector_text(r.r, sel, std::strlen(sel), nullptr);
    }
    break;
  }
  case 7:
  case 8: {
    Rule r = pick(s, rng);
    if (r && gtext_cssom_rule_css_rules(r.r)) {
      const size_t n = gtext_cssom_rule_list_length(gtext_cssom_rule_css_rules(r.r));
      static const char * const inner[] = {"u{color:red}", "@media print{ v{width:1px} }", "color:blue", "& w{ margin:0 }", "@layer z{ y{width:2px} }"};
      insert_rule(r.r, inner[rng.below(sizeof inner / sizeof *inner)], rng.below(n + 1));
    }
    break;
  }
  case 9: {
    Rule r = pick(s, rng);
    if (r && gtext_cssom_rule_css_rules(r.r)) {
      const size_t n = gtext_cssom_rule_list_length(gtext_cssom_rule_css_rules(r.r));
      if (n) {
        delete_rule(r.r, rng.below(n));
      }
    }
    break;
  }
  case 10: {
    Rule r = pick(s, rng);
    if (r && gtext_cssom_rule_kind(r.r) == GTEXT_CSSOM_RULE_MEDIA) {
      GTEXT_CSSOM_Media_List * m = nullptr;
      if (gtext_cssom_rule_media(r.r, &m) == GTEXT_CSS_OK) {
        static const char * const q[] = {"print", "screen and (min-width: 10px)", "all", "(orientation: portrait)"};
        const char * t = q[rng.below(4)];
        gtext_cssom_media_set_text(m, t, std::strlen(t), nullptr);
        gtext_cssom_media_release(m);
      }
    }
    break;
  }
  case 11: {
    Rule r = pick(s, rng);
    if (r && gtext_cssom_rule_kind(r.r) == GTEXT_CSSOM_RULE_KEYFRAMES) {
      gtext_cssom_keyframes_append_rule(r.r, "50% { color: green }", 19, nullptr, nullptr);
    }
    else if (rng.below(8) == 0) {
      gtext_cssom_sheet_compact(s.s);
    }
    break;
  }
  case 12: {
    Rule r = pick(s, rng);
    GTEXT_CSS_Status st = GTEXT_CSS_OK;
    Decl d = r ? style_of(r.r, &st) : Decl();
    if (d.d) {
      static const char * const t[] = {"color:red;width:1px", "margin:0 !important", "", "--q:1;color:blue"};
      const char * x = t[rng.below(4)];
      set_text(d.d, x);
    }
    break;
  }
  }
}

TEST(CssomSnapshot, TheSnapshotEqualsTheRebuildAfterEveryStepOfEverySequence) {
  size_t steps = 0, checks = 0;
  const char * more = std::getenv("GTEXT_TEST_SNAPSHOT_SEEDS");   // a hunt: more sequences than the gate runs
  const uint64_t seeds = more ? std::strtoull(more, nullptr, 10) : 60;
  for (uint64_t seed = 1; seed <= seeds; seed++) {
    Doc doc;
    Rng rng(seed);
    std::string text;
    const size_t initial = 2 + rng.below(6);
    for (size_t i = 0; i < initial; i++) {
      const char * r = kRules[rng.below(sizeof kRules / sizeof *kRules)];
      text += std::string(r) + " ";
    }
    Sheet s(doc, text);
    ASSERT_EQ(s.status, GTEXT_CSS_OK);
    ASSERT_TRUE(equals_rebuild(s.s)) << "seed " << seed << ": " << text;
    for (int k = 0; k < 40; k++) {
      step(s, rng);
      steps++;
      const ::testing::AssertionResult ok = equals_rebuild(s.s);
      ASSERT_TRUE(ok) << "seed " << seed << " step " << k;
      checks++;
      // and the same pointer when asked again
      GTEXT_CSS_Sheet *p1 = nullptr, *p2 = nullptr;
      ASSERT_EQ(gtext_cssom_sheet_snapshot(s.s, &p1), GTEXT_CSS_OK);
      ASSERT_EQ(gtext_cssom_sheet_snapshot(s.s, &p2), GTEXT_CSS_OK);
      EXPECT_EQ(p1, p2);
      gtext_css_sheet_free(p1);
      gtext_css_sheet_free(p2);
    }
  }
  EXPECT_EQ(steps, seeds * 40u);
  EXPECT_EQ(checks, steps);
}

/* ---- imports ---------------------------------------------------------------------------------------------------------------------------------- */

TEST(CssomSnapshot, AnImportWithNoSheetContributesNothing) {
  Doc doc;
  Sheet s(doc, "@import url(a.css); b{color:red}");
  Snap snap(s.s);
  ASSERT_EQ(snap.st, GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_sheet_import_count(snap.s), 0u);
  EXPECT_EQ(gtext_css_sheet_record_count(snap.s), 1u);
  GTEXT_CSS_Sheet_Diagnostic d;
  ASSERT_GE(gtext_css_sheet_diagnostic_total(snap.s), 1u);
  ASSERT_TRUE(gtext_css_sheet_diagnostic(snap.s, 0, &d));
  EXPECT_EQ(d.kind, GTEXT_CSS_SHEET_DIAG_IMPORT_FAILED);
  EXPECT_TRUE(equals_rebuild(s.s));
}

TEST(CssomSnapshot, AnImportsSheetIsASnapshotOneLevelDownAndItsChangeIsTheImporterAChange) {
  Doc doc;
  Sheet host(doc, "@import url(a.css); b{color:red}");
  Sheet child(doc, "c{color:blue} d{width:1px}");
  Rule imp = path(host, {0});
  GTEXT_CSSOM_Outcome o;
  ASSERT_EQ(gtext_cssom_import_rule_complete(imp.r, child.s, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_CHANGED);
  ASSERT_FALSE(doc.events.empty());
  EXPECT_EQ(doc.events.back().kind, GTEXT_CSSOM_EVENT_IMPORT_COMPLETED);
  EXPECT_EQ(doc.events.back().sheet_id, gtext_cssom_sheet_id(host.s));
  EXPECT_EQ(doc.events.back().rule_id, gtext_cssom_rule_id(imp.r));

  Snap h1(host.s), c1(child.s);
  ASSERT_EQ(h1.st, GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_sheet_import_count(h1.s), 1u);
  EXPECT_EQ(gtext_css_sheet_import(h1.s, 0), c1.s) << "the imported sheet is the child's snapshot itself";
  EXPECT_EQ(gtext_css_sheet_record_count(gtext_css_sheet_import(h1.s, 0)), 2u);
  uint64_t v1 = 0, v2 = 0, hv = 0;
  ASSERT_EQ(gtext_cssom_sheet_composite_version(host.s, &v1), GTEXT_CSS_OK);
  // a change inside the child
  Decl d = style_of(path(child, {0}).r);
  ASSERT_EQ(set(d.d, "margin", "2px").o.kind, GTEXT_CSSOM_CHANGED);
  ASSERT_EQ(gtext_cssom_sheet_composite_version(host.s, &v2), GTEXT_CSS_OK);
  EXPECT_GT(v2, v1) << "the child's change is the host's";
  ASSERT_EQ(gtext_cssom_sheet_composite_version(child.s, &hv), GTEXT_CSS_OK);
  EXPECT_EQ(hv, v2);
  Snap h2(host.s), c2(child.s);
  EXPECT_NE(h2.s, h1.s);
  EXPECT_NE(c2.s, c1.s);
  EXPECT_EQ(gtext_css_sheet_import(h2.s, 0), c2.s);
  EXPECT_EQ(gtext_css_sheet_import(h1.s, 0), c1.s) << "the first snapshot of the host still has the first of the child";
  // a change of the host that is not in the child: the child's snapshot is not compiled again
  const uint64_t compiles = counters(doc).snapshot_compiles;
  ASSERT_EQ(insert_rule(host.s, "e{color:green}", 2).st, GTEXT_CSS_OK);
  Snap h3(host.s);
  EXPECT_EQ(gtext_css_sheet_import(h3.s, 0), c2.s) << "the child is the same snapshot";
  EXPECT_EQ(counters(doc).snapshot_compiles, compiles + 1);
  EXPECT_EQ(counters(doc).snapshot_hits > 0, true);
  // the dumps: the host's rules, with the import taking its place
  EXPECT_NE(dump_of(h3.s).find("import 0 records=2"), std::string::npos) << dump_of(h3.s);
}

TEST(CssomSnapshot, TheCompositeVersionMovesExactlyWhenSomethingInTheWalkDoes) {
  Doc doc;
  Sheet host(doc, "@import url(a.css); b{color:red}");
  Sheet child(doc, "c{color:blue}");
  Sheet other(doc, "z{color:red}");
  Rule imp = path(host, {0});
  uint64_t v = 0, w = 0;
  ASSERT_EQ(gtext_cssom_sheet_composite_version(host.s, &v), GTEXT_CSS_OK);
  ASSERT_EQ(insert_rule(other.s, "y{}", 0).st, GTEXT_CSS_OK);   // a sheet outside
  ASSERT_EQ(gtext_cssom_sheet_composite_version(host.s, &w), GTEXT_CSS_OK);
  EXPECT_EQ(w, v) << "a change of a sheet not in the walk";
  ASSERT_EQ(gtext_cssom_import_rule_complete(imp.r, child.s, nullptr), GTEXT_CSS_OK);
  uint64_t after_complete = 0;
  ASSERT_EQ(gtext_cssom_sheet_composite_version(host.s, &after_complete), GTEXT_CSS_OK);
  EXPECT_GT(after_complete, v);
  // swap for a sheet last changed long before: the importer is still changed
  Sheet old(doc, "q{}");
  ASSERT_EQ(insert_rule(child.s, "r{}", 0).st, GTEXT_CSS_OK);   // make the first child recent
  uint64_t recent = 0;
  ASSERT_EQ(gtext_cssom_sheet_composite_version(host.s, &recent), GTEXT_CSS_OK);
  Snap before(host.s);
  ASSERT_EQ(gtext_cssom_import_rule_complete(imp.r, old.s, nullptr), GTEXT_CSS_OK);
  uint64_t swapped = 0;
  ASSERT_EQ(gtext_cssom_sheet_composite_version(host.s, &swapped), GTEXT_CSS_OK);
  EXPECT_GT(swapped, recent) << "replacing the imported sheet by one that is older is a change";
  Snap now(host.s);
  EXPECT_NE(now.s, before.s);
  // removing it
  ASSERT_EQ(gtext_cssom_import_rule_complete(imp.r, nullptr, nullptr), GTEXT_CSS_OK);
  uint64_t removed = 0;
  ASSERT_EQ(gtext_cssom_sheet_composite_version(host.s, &removed), GTEXT_CSS_OK);
  EXPECT_GT(removed, swapped);
  Snap gone(host.s);
  EXPECT_EQ(gtext_css_sheet_import_count(gone.s), 0u);
}

TEST(CssomSnapshot, ImportsAreSingleOwnerAndRefuseACycle) {
  Doc doc;
  Sheet host(doc, "@import url(a.css); @import url(b.css); x{color:red}");
  Sheet child(doc, "@import url(c.css); c{color:blue}");
  Sheet grand(doc, "g{color:green}");
  Rule i0 = path(host, {0}), i1 = path(host, {1}), ic = path(child, {0});
  ASSERT_EQ(gtext_cssom_import_rule_complete(i0.r, child.s, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_import_rule_complete(ic.r, grand.s, nullptr), GTEXT_CSS_OK);
  const std::string before_host = dump(host.s), before_child = dump(child.s);
  const size_t events = doc.events.size();
  uint64_t cv = 0, cv2 = 0;
  ASSERT_EQ(gtext_cssom_sheet_composite_version(host.s, &cv), GTEXT_CSS_OK);
  // a second owner: another import rule of the same sheet
  GTEXT_CSSOM_Outcome o;
  EXPECT_EQ(gtext_cssom_import_rule_complete(i1.r, child.s, &o), GTEXT_CSS_E_INVALID_STATE);
  EXPECT_EQ(gtext_cssom_import_rule_complete(i1.r, grand.s, &o), GTEXT_CSS_E_INVALID_STATE) << "a sheet that is reached through an import is owned";
  // the importing rule's own sheet, and the sheet that holds it
  EXPECT_EQ(gtext_cssom_import_rule_complete(i1.r, host.s, &o), GTEXT_CSS_E_HIERARCHY) << "a sheet imports itself";
  EXPECT_EQ(gtext_cssom_import_rule_complete(ic.r, host.s, &o), GTEXT_CSS_E_HIERARCHY) << "a sheet two levels down imports the one at the top";
  Sheet loose(doc, "@import url(l.css);");
  Rule il = path(loose, {0});
  ASSERT_EQ(gtext_cssom_import_rule_complete(il.r, host.s, nullptr), GTEXT_CSS_OK) << "an unowned sheet may be imported";
  // now the loop would close through it: host -> child -> grand, loose -> host
  Sheet third(doc, "@import url(t.css);");
  Rule it = path(third, {0});
  ASSERT_EQ(gtext_cssom_import_rule_complete(i1.r, third.s, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_import_rule_complete(it.r, loose.s, &o), GTEXT_CSS_E_HIERARCHY) << "loose reaches third through host: third importing loose is a cycle";
  EXPECT_EQ(dump(child.s), before_child);
  ASSERT_EQ(gtext_cssom_sheet_composite_version(host.s, &cv2), GTEXT_CSS_OK);
  EXPECT_GE(cv2, cv);
  EXPECT_GT(doc.events.size(), events);
  // constructed, an owner node, another document, a rule that is not an import
  GTEXT_CSSOM_Sheet * constructed = nullptr;
  ASSERT_EQ(gtext_cssom_sheet_new_constructed(doc.d, nullptr, &constructed), GTEXT_CSS_OK);
  Sheet fresh(doc, "@import url(f.css);");
  Rule fi = path(fresh, {0});
  EXPECT_EQ(gtext_cssom_import_rule_complete(fi.r, constructed, nullptr), GTEXT_CSS_E_INVALID_STATE);
  int node = 0;
  GTEXT_CSSOM_Sheet_Options oo = gtext_cssom_sheet_options_default();
  oo.owner_node = &node;
  Sheet owned(doc, "o{color:red}", &oo);
  EXPECT_EQ(gtext_cssom_import_rule_complete(fi.r, owned.s, nullptr), GTEXT_CSS_E_INVALID_STATE) << "a document's sheet is owned by its element";
  Doc other;
  Sheet foreign(other, "f{color:red}");
  EXPECT_EQ(gtext_cssom_import_rule_complete(fi.r, foreign.s, nullptr), GTEXT_CSS_E_INVALID);
  Rule style = path(fresh, {});
  Sheet withstyle(doc, "a{color:red}");
  EXPECT_EQ(gtext_cssom_import_rule_complete(path(withstyle, {0}).r, grand.s, nullptr), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_EQ(gtext_cssom_import_rule_complete(nullptr, nullptr, nullptr), GTEXT_CSS_E_INVALID);
  gtext_cssom_sheet_release(constructed);
}

TEST(CssomSnapshot, GivingTheSameSheetOrNothingIsUnchanged) {
  Doc doc;
  Sheet host(doc, "@import url(a.css);");
  Sheet child(doc, "c{}");
  Rule imp = path(host, {0});
  GTEXT_CSSOM_Outcome o;
  ASSERT_EQ(gtext_cssom_import_rule_complete(imp.r, nullptr, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_UNCHANGED);
  ASSERT_EQ(gtext_cssom_import_rule_complete(imp.r, child.s, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_CHANGED);
  const uint64_t version = gtext_cssom_sheet_version(host.s);
  const size_t events = doc.events.size();
  ASSERT_EQ(gtext_cssom_import_rule_complete(imp.r, child.s, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_UNCHANGED);
  EXPECT_EQ(gtext_cssom_sheet_version(host.s), version);
  EXPECT_EQ(doc.events.size(), events);
}

TEST(CssomSnapshot, AnImportRuleHoldsItsSheetAndTheSheetKnowsItsOwner) {
  Doc doc;
  GTEXT_CSSOM_Sheet * kept = nullptr;
  GTEXT_CSSOM_Sheet * child_kept = nullptr;
  {
    Sheet host(doc, "@import url(a.css);");
    Sheet child(doc, "c{color:blue}");
    Rule imp = path(host, {0});
    ASSERT_EQ(gtext_cssom_import_rule_complete(imp.r, child.s, nullptr), GTEXT_CSS_OK);
    GTEXT_CSSOM_Rule * owner = nullptr;
    ASSERT_EQ(gtext_cssom_sheet_owner_rule(child.s, &owner), GTEXT_CSS_OK);
    EXPECT_EQ(owner, imp.r);
    gtext_cssom_rule_release(owner);
    GTEXT_CSSOM_Sheet * held = nullptr;
    ASSERT_EQ(gtext_cssom_import_rule_sheet(imp.r, &held), GTEXT_CSS_OK);
    EXPECT_EQ(held, child.s);
    gtext_cssom_sheet_release(held);
    kept = gtext_cssom_sheet_retain(host.s);
    child_kept = gtext_cssom_sheet_retain(child.s);
  }
  // the caller's references to the host and the child are gone but two are held: the child is alive, and the host (through the child's hold on its import rule)
  GTEXT_CSSOM_Rule * owner = nullptr;
  ASSERT_EQ(gtext_cssom_sheet_owner_rule(child_kept, &owner), GTEXT_CSS_OK);
  ASSERT_NE(owner, nullptr);
  EXPECT_EQ(gtext_cssom_rule_kind(owner), GTEXT_CSSOM_RULE_IMPORT);
  gtext_cssom_rule_release(owner);
  gtext_cssom_sheet_release(kept);
  // the host's last external reference is gone: the child still holds its owner rule, which holds the host
  ASSERT_EQ(gtext_cssom_sheet_owner_rule(child_kept, &owner), GTEXT_CSS_OK);
  EXPECT_NE(owner, nullptr) << "a held child keeps the import that owns it, and so the sheet that holds that";
  Snap snap(child_kept);
  EXPECT_EQ(snap.st, GTEXT_CSS_OK);
  gtext_cssom_rule_release(owner);
  gtext_cssom_sheet_release(child_kept);
  EXPECT_EQ(counters(doc).live_objects, 0u) << "everything was freed";
}

TEST(CssomSnapshot, ARemovedImportSheetIsAValidObjectNoImportHolds) {
  Doc doc;
  Sheet host(doc, "@import url(a.css); b{color:red}");
  Sheet child(doc, "c{color:blue}");
  Rule imp = path(host, {0});
  ASSERT_EQ(gtext_cssom_import_rule_complete(imp.r, child.s, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_import_rule_complete(imp.r, nullptr, nullptr), GTEXT_CSS_OK);
  GTEXT_CSSOM_Rule * owner = reinterpret_cast<GTEXT_CSSOM_Rule *>(1);
  ASSERT_EQ(gtext_cssom_sheet_owner_rule(child.s, &owner), GTEXT_CSS_OK);
  EXPECT_EQ(owner, nullptr);
  ASSERT_EQ(insert_rule(child.s, "d{}", 0).st, GTEXT_CSS_OK);
  Snap h(host.s);
  EXPECT_EQ(gtext_css_sheet_import_count(h.s), 0u);
  // and it may be given to another import
  Sheet other(doc, "@import url(o.css);");
  ASSERT_EQ(gtext_cssom_import_rule_complete(path(other, {0}).r, child.s, nullptr), GTEXT_CSS_OK);
}

TEST(CssomSnapshot, ADeletedImportKeepsItsSheetAndAReinsertedOneHasNone) {
  Doc doc;
  Sheet host(doc, "@import url(a.css); b{color:red}");
  Sheet child(doc, "c{color:blue}");
  Rule imp = path(host, {0});
  ASSERT_EQ(gtext_cssom_import_rule_complete(imp.r, child.s, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(delete_rule(host.s, 0), GTEXT_CSS_OK);
  Snap h(host.s);
  EXPECT_EQ(gtext_css_sheet_import_count(h.s), 0u) << "the import is gone from the sheet";
  GTEXT_CSSOM_Sheet * held = nullptr;
  ASSERT_EQ(gtext_cssom_import_rule_sheet(imp.r, &held), GTEXT_CSS_OK);
  EXPECT_EQ(held, child.s) << "the deleted rule still holds what it loaded";
  gtext_cssom_sheet_release(held);
  // a change of the detached import's sheet is not a change of the host (nothing holds the rule)
  uint64_t v1 = 0, v2 = 0;
  ASSERT_EQ(gtext_cssom_sheet_composite_version(host.s, &v1), GTEXT_CSS_OK);
  ASSERT_EQ(insert_rule(child.s, "d{}", 0).st, GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_sheet_composite_version(host.s, &v2), GTEXT_CSS_OK);
  EXPECT_EQ(v2, v1);
  ASSERT_EQ(insert_rule(host.s, "@import url(b.css);", 0).st, GTEXT_CSS_OK);
  GTEXT_CSSOM_Sheet * none = reinterpret_cast<GTEXT_CSSOM_Sheet *>(1);
  ASSERT_EQ(gtext_cssom_import_rule_sheet(path(host, {0}).r, &none), GTEXT_CSS_OK);
  EXPECT_EQ(none, nullptr);
}

TEST(CssomSnapshot, ImportsNestToTheDepthTheSheetAllowsAndNoFurther) {
  for (size_t depth = 1; depth <= 4; depth++) {
    Doc doc;
    // a chain of sheets: sheet 0 imports 1, 1 imports 2, ... and the last has a rule; `depth` imports below the top
    std::vector<std::unique_ptr<Sheet>> chain;
    for (size_t i = 0; i <= depth; i++) {
      GTEXT_CSSOM_Sheet_Options o = gtext_cssom_sheet_options_default();
      o.max_import_depth = depth;
      chain.push_back(std::make_unique<Sheet>(doc, i < depth ? "@import url(x.css); a{color:red}" : "z{color:blue}", &o));
      ASSERT_EQ(chain.back()->status, GTEXT_CSS_OK);
    }
    for (size_t i = 0; i < depth; i++) {
      ASSERT_EQ(gtext_cssom_import_rule_complete(path(*chain[i], {0}).r, chain[i + 1]->s, nullptr), GTEXT_CSS_OK);
    }
    Snap ok(chain[0]->s);
    EXPECT_EQ(ok.st, GTEXT_CSS_OK) << "a chain of exactly " << depth;
    uint64_t v = 0;
    EXPECT_EQ(gtext_cssom_sheet_composite_version(chain[0]->s, &v), GTEXT_CSS_OK);
    // one more
    GTEXT_CSSOM_Sheet_Options o = gtext_cssom_sheet_options_default();
    Sheet extra(doc, "e{color:red}", &o);
    Sheet& last = *chain[depth];
    ASSERT_EQ(insert_rule(last.s, "@import url(e.css);", 0).st, GTEXT_CSS_OK);
    ASSERT_EQ(gtext_cssom_import_rule_complete(path(last, {0}).r, extra.s, nullptr), GTEXT_CSS_OK);
    GTEXT_CSS_Sheet * too = nullptr;
    EXPECT_EQ(gtext_cssom_sheet_snapshot(chain[0]->s, &too), GTEXT_CSS_E_LIMIT_RULE_NESTING) << "one level more than " << depth;
    EXPECT_EQ(too, nullptr);
    EXPECT_EQ(gtext_cssom_sheet_composite_version(chain[0]->s, &v), GTEXT_CSS_E_LIMIT_RULE_NESTING);
    // the sheet whose imports are within its limit is still snapshotted
    Snap fine(chain.size() > 1 ? chain[1]->s : chain[0]->s);
    (void)fine;
  }
}

TEST(CssomSnapshot, ManyImportsInOneSheetAreWalkedOnceEach) {
  Doc doc;
  // 2,000 imports in one sheet, each with a sheet: the composite walk and the compile's callback visit each once (no recursion on the number of imports)
  std::string text;
  const size_t n = 2000;
  for (size_t i = 0; i < n; i++) {
    text += "@import url(" + std::to_string(i) + ".css);";
  }
  text += "x{color:red}";
  Sheet host(doc, text);
  ASSERT_EQ(host.status, GTEXT_CSS_OK);
  std::vector<std::unique_ptr<Sheet>> kids;
  for (size_t i = 0; i < n; i++) {
    kids.push_back(std::make_unique<Sheet>(doc, "k" + std::to_string(i) + "{color:blue}"));
    ASSERT_EQ(gtext_cssom_import_rule_complete(path(host, {i}).r, kids.back()->s, nullptr), GTEXT_CSS_OK);
  }
  Snap snap(host.s);
  ASSERT_EQ(snap.st, GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_sheet_import_count(snap.s), n);
  for (size_t i = 0; i < n; i += 397) {
    Snap kid(kids[i]->s);
    EXPECT_EQ(gtext_css_sheet_import(snap.s, i), kid.s);
  }
  EXPECT_EQ(counters(doc).snapshot_compiles, n + 1);
}

} // namespace
