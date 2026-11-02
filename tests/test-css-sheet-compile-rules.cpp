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
 * The array form of the compile (css_sheet.h, gtext_css_sheet_compile_rules()): the top-level rules of a sheet given as (tree, rule, source id) elements, each possibly from a tree of
 * its own. Rule ids are numbered from 1 in each tree, so rules of different trees collide on them: the layer a block put its rules in, and the layers a rule declared, are kept in
 * tables by id, and a rule read after another tree's must not find the other's entries. The central test splits a sheet into one tree per rule and requires the dump of the compile to
 * equal that of the sheet compiled from its one tree; diagnostics and records say which source they are in; the tree form is the array form over one tree.
 */

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_cascade_test_support.h"
#include "css_test_support.h"
#include "markup_test_support.h"

using namespace cas;

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

/// The rules of a sheet, each parsed alone into a tree of its own.
struct Split {
  std::vector<std::unique_ptr<css_test::Parsed>> trees;
  std::vector<GTEXT_CSS_Rule_Source> sources;
  explicit Split(const std::vector<std::string> & rules) {
    for (size_t i = 0; i < rules.size(); i++) {
      trees.push_back(std::make_unique<css_test::Parsed>());
      css_test::parse_into(*trees.back(), gtext_css_parse_stylesheet, rules[i]);
      EXPECT_EQ(trees.back()->status, GTEXT_CSS_OK) << rules[i];
      EXPECT_EQ(trees.back()->tree->rule_count, 1u) << rules[i];
      GTEXT_CSS_Rule_Source s;
      s.tree = trees.back()->tree;
      s.rule = &trees.back()->tree->rules[0];
      s.source_id = 100 + i;
      sources.push_back(s);
    }
  }
};

std::string join(const std::vector<std::string> & rules) {
  std::string all;
  for (const std::string & r : rules) {
    all += r + "\n";
  }
  return all;
}

struct Sheet {
  GTEXT_CSS_Sheet * s = nullptr;
  GTEXT_CSS_Status st = GTEXT_CSS_E_INTERNAL;
  Sheet() = default;
  Sheet(const Sheet &) = delete;
  Sheet & operator=(const Sheet &) = delete;
  ~Sheet() {
    gtext_css_sheet_free(s);
  }
};

void whole(Sheet & out, const std::vector<std::string> & rules, const GTEXT_CSS_Sheet_Options * o = nullptr) {
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_stylesheet, join(rules));
  ASSERT_EQ(p.status, GTEXT_CSS_OK);
  out.st = gtext_css_sheet_compile(p.tree, R(), o, &out.s, nullptr);
}

const std::vector<std::vector<std::string>> kSheets = {
    {"a{color:red}", "b > c{margin:1px 2px}", "@media print{ d{width:1px} @supports (a:b){ e{color:blue} } }"},
    // layers: statements, blocks, nested blocks and an anonymous one: the layer tables are by rule id and every tree numbers its ids from 1
    {"@layer base, theme;", "@layer base{ a{color:red} }", "@layer theme{ a{color:blue} @layer inner{ b{margin:0} } }", "@layer{ c{padding:0} }", "d{width:1px}"},
    {"@layer a{ x{color:red} }", "@layer b{ y{color:red} }", "@layer a{ z{color:blue} }", "@layer c.d{ w{margin:1px} }"},
    // conditional layer occurrences: the events are by rule id as well, and are written to the dump
    {"@media print{ @layer c, d; }", "@media screen{ @layer e; }", "@media print{ @layer c, d; @layer e{ w{color:red} } }", "@media screen{ @layer f; @layer g, h; }"},
    {"@media print{ @layer a{ x{color:red} } }", "@media screen{ @layer b{ y{color:red} } }", "@layer c, d;", "@media print{ @layer c; @layer e{ z{color:blue} } }"},
    // nesting, a run of declarations after a nested rule, the namespace map
    {"@namespace s url(urn:s);", "s|a{ color:red; b{ width:1px } margin:2px; & c{ height:1px } }", "x{ @media (min-width:1px){ color:red; y{ margin:1px } } }"},
    {"@scope (.a) to (.b){ p{color:red} }", "@scope (.c){ q{color:blue} }", "@container (min-width:10px){ r{color:green} }", "@starting-style{ t{color:red} }"},
    // imports: with and without a layer, a media list and supports(), and one that is late (ignored)
    {"@import url(a.css) layer(l);", "@import url(b.css) screen and (min-width:1px);", "@import url(c.css) supports(display:grid);", "a{color:red}", "@import url(late.css);"},
    // rules that are dropped: the diagnostics
    {"x >{}", "a{color:red;bogus:1;width:oops}", "@layer a{ b{ color:red } }", "c{color:blue}"},
    {},
};

TEST(CssSheetCompileRules, OneTreePerRuleCompilesToTheSheetOfOneTree) {
  for (const std::vector<std::string> & rules : kSheets) {
    Sheet one, many;
    whole(one, rules);
    ASSERT_EQ(one.st, GTEXT_CSS_OK);
    Split split(rules);
    many.st = gtext_css_sheet_compile_rules(split.sources.data(), split.sources.size(), R(), nullptr, &many.s, nullptr);
    ASSERT_EQ(many.st, GTEXT_CSS_OK) << join(rules);
    EXPECT_EQ(dump_of(many.s), dump_of(one.s)) << join(rules);
    EXPECT_EQ(gtext_css_sheet_record_count(many.s), gtext_css_sheet_record_count(one.s));
    EXPECT_EQ(gtext_css_sheet_diagnostic_total(many.s), gtext_css_sheet_diagnostic_total(one.s));
  }
}

TEST(CssSheetCompileRules, ALayerAnotherTreeDeclaredIsNotFoundByARuleWithTheSameId) {
  // both trees put the layer block's node at the id 1: the table entry the first left (the layer `a`) must not be the second's answer
  const std::vector<std::string> rules = {"@layer a{ x{color:red} }", "@layer b{ y{color:blue} }", "z{color:green}"};
  Sheet one, many;
  whole(one, rules);
  Split split(rules);
  ASSERT_EQ(gtext_css_sheet_compile_rules(split.sources.data(), split.sources.size(), R(), nullptr, &many.s, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(split.sources[0].rule->id, split.sources[1].rule->id) << "the premise: the ids collide";
  EXPECT_EQ(dump_of(many.s), dump_of(one.s));
  EXPECT_EQ(gtext_css_sheet_layer_count(many.s), 2u);
  GTEXT_CSS_Sheet_Record r0, r1, r2;
  ASSERT_TRUE(gtext_css_sheet_record(many.s, 0, &r0));
  ASSERT_TRUE(gtext_css_sheet_record(many.s, 1, &r1));
  ASSERT_TRUE(gtext_css_sheet_record(many.s, 2, &r2));
  EXPECT_NE(r0.layer, 0u);
  EXPECT_NE(r1.layer, 0u);
  EXPECT_NE(r0.layer, r1.layer);
  EXPECT_EQ(r2.layer, 0u) << "an unlayered rule after layered ones in other trees";
}

TEST(CssSheetCompileRules, TheTreeFormIsTheArrayFormOverOneTree) {
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_stylesheet, "@layer a, b; @layer a{ x{color:red} } y{color:blue} @media print{ z{margin:0} }");
  ASSERT_EQ(p.status, GTEXT_CSS_OK);
  std::vector<GTEXT_CSS_Rule_Source> src;
  for (size_t i = 0; i < p.tree->rule_count; i++) {
    src.push_back({p.tree, &p.tree->rules[i], 7});
  }
  Sheet a, b;
  ASSERT_EQ(gtext_css_sheet_compile(p.tree, R(), nullptr, &a.s, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_sheet_compile_rules(src.data(), src.size(), R(), nullptr, &b.s, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(dump_of(a.s), dump_of(b.s));
  GTEXT_CSS_Sheet_Record ra, rb;
  ASSERT_TRUE(gtext_css_sheet_record(a.s, 0, &ra));
  ASSERT_TRUE(gtext_css_sheet_record(b.s, 0, &rb));
  EXPECT_EQ(ra.span.offset, rb.span.offset);
  EXPECT_EQ(ra.source_id, 0u) << "the tree form has no source";
  EXPECT_EQ(rb.source_id, 7u) << "the array form says which source the span indexes";
}

TEST(CssSheetCompileRules, RecordsAndDiagnosticsSayWhichSourceTheyAreIn) {
  const std::vector<std::string> rules = {"a{color:red}", "x >{}", "b{color:blue; bogus:1}", "@layer q{ c{width:oops} }"};
  Split split(rules);
  Sheet s;
  ASSERT_EQ(gtext_css_sheet_compile_rules(split.sources.data(), split.sources.size(), R(), nullptr, &s.s, nullptr), GTEXT_CSS_OK);
  // the records: a (source 0), b (source 2)
  ASSERT_EQ(gtext_css_sheet_record_count(s.s), 2u);
  GTEXT_CSS_Sheet_Record r;
  ASSERT_TRUE(gtext_css_sheet_record(s.s, 0, &r));
  EXPECT_EQ(r.source_index, 0u);
  EXPECT_EQ(r.source_id, 100u);
  ASSERT_TRUE(gtext_css_sheet_record(s.s, 1, &r));
  EXPECT_EQ(r.source_index, 2u);
  EXPECT_EQ(r.source_id, 102u);
  // the diagnostics: the invalid selector (source 1), the unknown property (source 2), the invalid value (source 3), each with the id of its own tree
  ASSERT_EQ(gtext_css_sheet_diagnostic_total(s.s), 3u);
  GTEXT_CSS_Sheet_Diagnostic d;
  ASSERT_TRUE(gtext_css_sheet_diagnostic(s.s, 0, &d));
  EXPECT_EQ(d.kind, GTEXT_CSS_SHEET_DIAG_INVALID_SELECTOR);
  EXPECT_EQ(d.source_index, 1u);
  EXPECT_EQ(d.source_id, 101u);
  ASSERT_TRUE(gtext_css_sheet_diagnostic(s.s, 1, &d));
  EXPECT_EQ(d.kind, GTEXT_CSS_SHEET_DIAG_UNKNOWN_PROPERTY);
  EXPECT_EQ(d.source_index, 2u);
  EXPECT_EQ(d.source_id, 102u);
  ASSERT_TRUE(gtext_css_sheet_diagnostic(s.s, 2, &d));
  EXPECT_EQ(d.kind, GTEXT_CSS_SHEET_DIAG_INVALID_VALUE);
  EXPECT_EQ(d.source_index, 3u);
  EXPECT_EQ(d.source_id, 103u);
}

TEST(CssSheetCompileRules, TheOrderOfTheArrayIsTheOrderOfAppearance) {
  const std::vector<std::string> rules = {"a{color:red}", "a{color:blue}"};
  Split fwd(rules);
  Split rev({rules[1], rules[0]});
  Sheet f, r;
  ASSERT_EQ(gtext_css_sheet_compile_rules(fwd.sources.data(), 2, R(), nullptr, &f.s, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_sheet_compile_rules(rev.sources.data(), 2, R(), nullptr, &r.s, nullptr), GTEXT_CSS_OK);
  EXPECT_NE(dump_of(f.s), dump_of(r.s)) << "the declarations come in the order given";
  EXPECT_NE(dump_of(f.s).find("color: red"), std::string::npos);
  GTEXT_CSS_Sheet_Record rf, rr;
  ASSERT_TRUE(gtext_css_sheet_record(f.s, 0, &rf));
  ASSERT_TRUE(gtext_css_sheet_record(r.s, 0, &rr));
  EXPECT_EQ(rf.source_index, 0u);
  EXPECT_EQ(rr.source_index, 0u);
  EXPECT_EQ(rf.source_id, 100u);
  EXPECT_EQ(rr.source_id, 100u);
}

TEST(CssSheetCompileRules, ArgumentsAreChecked) {
  Sheet s;
  EXPECT_EQ(gtext_css_sheet_compile_rules(nullptr, 0, R(), nullptr, &s.s, nullptr), GTEXT_CSS_OK) << "no sources is an empty sheet";
  ASSERT_NE(s.s, nullptr);
  EXPECT_EQ(gtext_css_sheet_record_count(s.s), 0u);
  GTEXT_CSS_Sheet * none = nullptr;
  GTEXT_CSS_Sheet_Result res;
  EXPECT_EQ(gtext_css_sheet_compile_rules(nullptr, 1, R(), nullptr, &none, &res), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(res.status, GTEXT_CSS_E_INVALID);
  GTEXT_CSS_Rule_Source bad = {nullptr, nullptr, 0};
  EXPECT_EQ(gtext_css_sheet_compile_rules(&bad, 1, R(), nullptr, &none, &res), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(none, nullptr);
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_stylesheet, "a{}");
  GTEXT_CSS_Rule_Source half = {p.tree, nullptr, 0};
  EXPECT_EQ(gtext_css_sheet_compile_rules(&half, 1, R(), nullptr, &none, &res), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_sheet_compile_rules(&half, 1, nullptr, nullptr, &none, &res), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_sheet_compile_rules(&half, 1, R(), nullptr, nullptr, &res), GTEXT_CSS_E_INVALID);
}

TEST(CssSheetCompileRules, ALargeTreeAmongSmallOnesSizesTheTablesForTheLargest) {
  // the first rule's tree is small and the last has the most nodes: the id tables hold the largest tree's ids
  std::string big = "@layer big{";
  for (int i = 0; i < 300; i++) {
    big += "r" + std::to_string(i) + "{color:red} ";
  }
  big += "}";
  const std::vector<std::string> rules = {"@layer a{ x{color:red} }", big, "@layer a{ y{color:blue} }"};
  Sheet one, many;
  whole(one, rules);
  Split split(rules);
  ASSERT_EQ(gtext_css_sheet_compile_rules(split.sources.data(), split.sources.size(), R(), nullptr, &many.s, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(dump_of(many.s), dump_of(one.s));
}

// A source whose trees are swapped in the array: the same rule list in a different array. Nothing is read through a pointer to a tree other than the element's own.
TEST(CssSheetCompileRules, EveryAllocationFailureIsOutOfMemoryAndLeaksNothing) {
  const std::vector<std::string> rules = {"@namespace s url(urn:s);", "@layer a, b;", "@layer a{ s|x{color:red} @media print{ y{width:1px} } }", "z{ color:blue; & w{ margin:1px } }",
      "@import url(a.css) layer(l);"};
  Split split(rules);
  std::string want;
  {
    Sheet s;
    ASSERT_EQ(gtext_css_sheet_compile_rules(split.sources.data(), split.sources.size(), R(), nullptr, &s.s, nullptr), GTEXT_CSS_OK);
    want = dump_of(s.s);
  }
  size_t failures = 0;
  for (size_t n = 1; n < 100000; n++) {
    FaultAllocator fa;
    GTEXT_CSS_Sheet_Options o = gtext_css_sheet_options_default();
    o.allocator = &fa.alloc;
    GTEXT_CSS_Sheet * out = nullptr;
    fa.arm(n);
    const GTEXT_CSS_Status st = gtext_css_sheet_compile_rules(split.sources.data(), split.sources.size(), R(), &o, &out, nullptr);
    fa.disarm();
    if (!fa.failed) {
      ASSERT_EQ(st, GTEXT_CSS_OK);
      EXPECT_EQ(dump_of(out), want) << "the sweep ended at " << n;
      gtext_css_sheet_free(out);
      EXPECT_TRUE(fa.live.empty());
      break;
    }
    failures++;
    EXPECT_EQ(st, GTEXT_CSS_E_OOM) << "at " << n;
    EXPECT_EQ(out, nullptr);
    EXPECT_TRUE(fa.live.empty()) << "a leak at " << n;
    EXPECT_EQ(fa.bad_frees, 0u);
  }
  EXPECT_GT(failures, 20u);
}

} // namespace
