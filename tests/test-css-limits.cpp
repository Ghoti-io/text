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
 * The limits of a CSS parse, each asserted by its bound: an input that
 * reaches the limit parses, and one that passes it by one fails with that
 * limit's own status, no tree, and the place it was passed. And depth that
 * no recursive parser could survive, under a limit raised to allow it.
 */

#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_test_support.h"

using namespace css_test;

namespace {

std::string repeat(const std::string & unit, size_t k) {
  std::string out;
  out.reserve(unit.size() * k);
  for (size_t i = 0; i < k; i++) {
    out += unit;
  }
  return out;
}

struct Bound {
  GTEXT_CSS_Status status;
  bool tree;
  GTEXT_CSS_Span failure;
};

Bound run(ParseFn fn, const std::string & css, const GTEXT_CSS_Parse_Options & o) {
  GTEXT_CSS_Tree * t = nullptr;
  GTEXT_CSS_Result r{};
  Bound b;
  b.status = fn(css.data(), css.size(), &o, &t, &r);
  b.tree = t != nullptr;
  b.failure = r.failure;
  EXPECT_EQ(r.status, b.status);
  gtext_css_tree_free(t);
  gtext_css_result_free(&r);
  return b;
}

void at_and_past(ParseFn fn, const std::string & at, const std::string & past, const GTEXT_CSS_Parse_Options & o,
    GTEXT_CSS_Status status, size_t offset, size_t line, size_t column) {
  const Bound ok = run(fn, at, o);
  EXPECT_EQ(ok.status, GTEXT_CSS_OK) << at;
  EXPECT_TRUE(ok.tree);
  const Bound no = run(fn, past, o);
  EXPECT_EQ(no.status, status) << past;
  EXPECT_FALSE(no.tree) << "a limit leaves no tree";
  EXPECT_EQ(no.failure.offset, offset) << past;
  EXPECT_EQ(no.failure.line, line) << past;
  EXPECT_EQ(no.failure.column, column) << past;
}

TEST(CssLimits, InputBytes) {
  GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
  o.max_input_bytes = 8;
  at_and_past(gtext_css_parse_stylesheet, "a{}\nb{}x", "a{}\nb{}xy", o, GTEXT_CSS_E_LIMIT_INPUT, 8, 2, 5);
}

TEST(CssLimits, Tokens) {
  GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
  o.max_tokens = 5;
  // a ws b ws c is five; the sixth is the second ws.
  at_and_past(gtext_css_parse_component_values, "a b c", "a b c d", o, GTEXT_CSS_E_LIMIT_TOKENS, 5, 1, 6);
}

TEST(CssLimits, DepthOfBlocksAndFunctions) {
  // The matrix's limit row: nesting deeper than the limit is E_LIMIT_DEPTH, no tree, and the
  // place of the opening that went past it.
  GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
  o.max_depth = 3;
  at_and_past(gtext_css_parse_component_values, "f([{}])", "f([{()}])", o, GTEXT_CSS_E_LIMIT_DEPTH, 4, 1, 5);
  at_and_past(gtext_css_parse_stylesheet, "a{b{c{}}}", "a{b{c{\nd{}}}}", o, GTEXT_CSS_E_LIMIT_DEPTH, 8, 2, 2);
  // Where the blocks close, the depth goes down again.
  EXPECT_EQ(run(gtext_css_parse_stylesheet, repeat("a{b{c{}}}", 50), o).status, GTEXT_CSS_OK);
  // A closing token that matches nothing does not close anything.
  EXPECT_EQ(run(gtext_css_parse_component_values, "([{ ) ] ( }", o).status, GTEXT_CSS_E_LIMIT_DEPTH);
}

TEST(CssLimits, Rules) {
  GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
  o.max_rules = 3;
  at_and_past(gtext_css_parse_stylesheet, "a{} b{} c{}", "a{} b{} c{}\n d{}", o, GTEXT_CSS_E_LIMIT_RULES, 13, 2, 2);
  // A nested declarations rule is a rule (b, the run c:d and a are three; with e, a is the fourth,
  // counted when it is complete), and so is one inside a block that is then dropped.
  at_and_past(gtext_css_parse_stylesheet, "a{b{} c:d}", "a{b{} c:d; e{}}", o, GTEXT_CSS_E_LIMIT_RULES, 0, 1, 1);
  at_and_past(gtext_css_parse_stylesheet, "--x:y{a{}b{}c{}}", "--x:y{a{}b{}c{}d{}}", o, GTEXT_CSS_E_LIMIT_RULES, 15, 1, 16);
}

TEST(CssLimits, Declarations) {
  GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
  o.max_declarations = 2;
  at_and_past(gtext_css_parse_declaration_list, "a:b; c:d", "a:b; c:d; e:f", o, GTEXT_CSS_E_LIMIT_DECLARATIONS, 10, 1, 11);
  at_and_past(gtext_css_parse_stylesheet, "x{a:b} y{c:d}", "x{a:b} y{c:d} z{e:f}", o, GTEXT_CSS_E_LIMIT_DECLARATIONS, 16, 1, 17);
}

TEST(CssLimits, ParseErrors) {
  GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
  o.max_total_errors = 3;
  at_and_past(gtext_css_parse_component_values, ") ) )", ") ) ) )", o, GTEXT_CSS_E_LIMIT_ERRORS, 6, 1, 7);
  // The kept list is another number, and passing it is no failure.
  o.max_total_errors = 0;
  o.max_errors = 1;
  EXPECT_EQ(run(gtext_css_parse_component_values, ") ) ) )", o).status, GTEXT_CSS_OK);
}

TEST(CssLimits, ZeroIsTheDefaultAndNeverUnlimited) {
  GTEXT_CSS_Parse_Options o;
  std::memset(&o, 0, sizeof o);
  // 513 nested blocks: one past the default depth.
  EXPECT_EQ(run(gtext_css_parse_component_values, repeat("(", 512), o).status, GTEXT_CSS_OK);
  EXPECT_EQ(run(gtext_css_parse_component_values, repeat("(", 513), o).status, GTEXT_CSS_E_LIMIT_DEPTH);
}

TEST(CssDepth, AHundredThousandNestedRulesParseAndSerializeWithoutRecursion) {
  GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
  o.max_depth = 200000;
  const std::string css = repeat("a{", 100000) + repeat("}", 100000);
  Parsed p;
  parse_into(p, gtext_css_parse_stylesheet, css, &o);
  ASSERT_EQ(p.status, GTEXT_CSS_OK);
  EXPECT_EQ(p.tree->node_count, 200000u) << "a rule and its prelude's ident, at every level";
  EXPECT_EQ(serialize(p.tree), css);
}

TEST(CssDepth, AHundredThousandNestedFunctionsParseAndSerializeWithoutRecursion) {
  GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
  o.max_depth = 200000;
  const std::string css = repeat("f([", 50000);
  Parsed p;
  parse_into(p, gtext_css_parse_component_values, css, &o);
  ASSERT_EQ(p.status, GTEXT_CSS_OK);
  EXPECT_EQ(serialize(p.tree), css + repeat("])", 50000));
  Parsed q;
  parse_into(q, gtext_css_parse_declaration_list, "a:" + css, &o);
  ASSERT_EQ(q.status, GTEXT_CSS_OK);
  EXPECT_EQ(q.tree->declaration_count, 1u);
}

} // namespace
