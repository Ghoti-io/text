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
 * The edges of the Value Definition Syntax engine that a review of the first
 * build found: names with white space in them, ranges and their units, quoted
 * literals and square brackets, what an opaque value may contain, the types a
 * math function stands for, the arbitrary substitution functions, the matcher's
 * allocator, the text of an opaque value, the choice of separators once it is
 * known what can be empty, spans, and the work that is charged.
 *
 * Normative text, quoted where a test depends on it (the drafts as served on
 * 2026-10-22):
 *
 * - Values 4, 5.1: "within the angle brackets, after the identifying keyword,
 *   indicating a closed range between (and including) min and max"; "Values of
 *   -infinity or infinity must be written without units, even if the value type
 *   uses units"; "Values of 0 can be written without units". It says nothing of a
 *   range on a composite type.
 * - Values 4, 2.1: "Other delimiters must be written enclosed in single quotes".
 * - Variables 1, 2.1: <declaration-value> "matches any sequence of one or more
 *   tokens, so long as the sequence does not contain <bad-string-token>,
 *   <bad-url-token>, unmatched <)-token>, <]-token>, or <}-token>, or top-level
 *   <semicolon-token> tokens or <delim-token> tokens with a value of "!"".
 * - Values 4, 10: a math function "can be used anywhere a value of that type is
 *   allowed" for <length>, <frequency>, <angle>, <time>, <flex>, <resolution>,
 *   <percentage>, <number>, <integer>; 4, 5.? : "<ratio> = <number [0,infinity]> [ /
 *   <number [0,infinity]> ]?" and "<ratio> is always serialized with both components".
 * - Values 5: "The if() function is an arbitrary substitution function",
 *   "inherit() is an arbitrary substitution function", "attr() is an arbitrary
 *   substitution function, similar to var()", "The random-item() function is an
 *   arbitrary substitution function, like var()".
 */

#include <cmath>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "css_values_test_support.h"
#include "markup_test_support.h"

using namespace gv;

namespace {

struct Row {
  const char * grammar;
  const char * input;
  const char * expect;
};

void check_rows(const std::vector<Row> & rows, Defs * defs = nullptr) {
  for (const Row & r : rows) {
    EXPECT_EQ(run(r.grammar, r.input, defs), r.expect) << "grammar: " << r.grammar << "  input: " << r.input;
  }
}

GTEXT_CSS_Status parse_status(const std::string & text, GTEXT_CSS_Grammar_Result * r = nullptr) {
  GTEXT_CSS_Definition * d = nullptr;
  const GTEXT_CSS_Status st = gtext_css_definition_parse(text.data(), text.size(), nullptr, &d, r);
  gtext_css_definition_free(d);
  return st;
}

TEST(CssGrammarEdges, AReferenceNameEndsAtAnyWhiteSpaceTheNotationSkipsElsewhere) {
  for (const char * ws : {" ", "\t", "\n", "\r", "\f", "\r\n"}) {
    EXPECT_EQ(run(std::string("<length") + ws + "[0,1]>", "1px"), "dim:1px") << "white space before a range";
    EXPECT_EQ(run(std::string("<length [0,1]") + ws + ">", "1px"), "dim:1px");
    EXPECT_EQ(run(std::string("<length") + ws + ">", "1px"), "dim:1px") << "white space before the bracket";
    EXPECT_EQ(run(std::string("<number [0,1") + ws + "]>", "2"), "no match");
  }
  Defs d;
  d.types["t"] = "x";
  d.props["p"] = "y";
  EXPECT_EQ(run("<t\t>", "x", &d), "kw:x");
  EXPECT_EQ(run("<'p'\n>", "y", &d), "kw:y");
  EXPECT_EQ(d.asked, (std::vector<std::string>{"t", "'p"}));  // (not "t\t")
}

TEST(CssGrammarEdges, ABoundIsInTheUnitOfTheTypeItRestricts) {
  GTEXT_CSS_Grammar_Result r{};
  for (const char * bad : {"<number [0px,10px]>", "<length [0deg,∞]>", "<integer [0%,50%]>", "<percentage [0px,1px]>",
           "<angle [0px,1px]>", "<time [0s,1px]>", "<length [0px,1deg]>", "<number [0%,1]>", "<flex [0,1fr1]>"}) {
    EXPECT_EQ(parse_status(bad, &r), GTEXT_CSS_E_GRAMMAR) << bad;
  }
  for (const char * good : {"<number [0,10]>", "<length [0px,∞]>", "<length [0,1in]>", "<percentage [0%,50%]>", "<percentage [0,50]>",
           "<dimension [0px,1in]>", "<angle [-90deg,90deg]>", "<angle [\xE2\x88\x92\xE2\x88\x9E,0]>", "<flex [0fr,∞]>",
           "<resolution [1dppx,2x]>", "<time [0s,1000ms]>", "<frequency [20Hz,20kHz]>"}) {
    EXPECT_EQ(parse_status(good), GTEXT_CSS_OK) << good;
  }
  // the minus sign U+2212 and an infinity
  EXPECT_EQ(run("<number [\xE2\x88\x92\xE2\x88\x9E,0]>", "-5"), "num:-5");
  EXPECT_EQ(run("<number [\xE2\x88\x92\xE2\x88\x9E,0]>", "5"), "no match");
}

TEST(CssGrammarEdges, ARangeThatTheTypesAroundItNarrowToNothingIsRefusedWhenLinked) {
  Defs d;
  d.types["small"] = "<number [0,5]>";
  d.types["wrap"] = "<small>";
  Grammar g;
  compile_into(g, "<small [6,9]>", &d);
  EXPECT_EQ(g.status, GTEXT_CSS_E_GRAMMAR);
  EXPECT_STREQ(g.result.name, "<number>");
  Grammar h;
  compile_into(h, "<wrap [5,9]>", &d);  // [5,5] is a range
  EXPECT_EQ(h.status, GTEXT_CSS_OK);
  Matched m;
  match_into(m, h, "5");
  EXPECT_EQ(m.dump(), "num:5");
  Matched n;
  match_into(n, h, "4");
  EXPECT_EQ(n.dump(), "no match");
  Grammar k;
  compile_into(k, "<wrap [5.5,9]>", &d);
  EXPECT_EQ(k.status, GTEXT_CSS_E_GRAMMAR);
}

TEST(CssGrammarEdges, ARangeOnACompositeTypeRestrictsTheNumericTerminalsAndLeavesTheOthersAlone) {
  Defs d;
  d.types["mixed"] = "<ident> | <string> | <zero> | <length> | auto";
  d.types["tokens"] = "<ident-token> | <number>";
  d.types["lp"] = "<length> | <percentage>";
  d.types["deep"] = "<mixed> | <tokens>";
  check_rows({
      {"<mixed [0,∞]>", "foo", "kw:foo"},
      {"<mixed [0,∞]>", "'s'", "str:s"},
      {"<mixed [0,∞]>", "0", "zero"},
      {"<mixed [0,∞]>", "auto", "kw:auto"},
      {"<mixed [0,∞]>", "-1px", "no match"},
      {"<mixed [0,∞]>", "1px", "dim:1px"},
      {"<tokens [0,1]>", "x", "tok:x"},
      {"<tokens [0,1]>", "2", "no match"},
      {"<deep [0,∞]>", "foo", "kw:foo"},
      {"<deep [0,∞]>", "-1", "no match"},
      // bounds in percent restrict the percentage and leave the length alone
      {"<lp [0%,50%]>", "60%", "no match"},
      {"<lp [0%,50%]>", "40%", "pct:40"},
      {"<lp [0%,50%]>", "-99px", "dim:-99px"},
      {"<lp [0px,5px]>", "99%", "pct:99"},
      {"<lp [0px,5px]>", "6px", "no match"},
  }, &d);
}

TEST(CssGrammarEdges, TheLimitsOfAGrammarAreHeldBelowWhatAn32BitOffsetCanHold) {
  struct L {
    size_t bytes, depth, nodes;
  };
  GTEXT_CSS_Grammar_Options o{};
  o.max_definition_bytes = SIZE_MAX;
  o.max_definition_depth = SIZE_MAX;
  o.max_definition_nodes = SIZE_MAX;
  size_t b = 0, de = 0, n = 0;
  css_grammar_limits_for_test(&o, &b, &de, &n);
  EXPECT_EQ(b, (size_t)1 << 30);
  EXPECT_EQ(de, (size_t)1 << 20);
  EXPECT_EQ(n, (size_t)1 << 28);
  if (sizeof(size_t) > 4) {
    // (a size_t of 32 bits cannot hold a value past 2^32: nothing to truncate there)
    const size_t past = static_cast<size_t>(uint64_t{1} << 32);
    o.max_definition_bytes = past + 5;
    css_grammar_limits_for_test(&o, &b, &de, &n);
    EXPECT_EQ(b, (size_t)1 << 30);  // not 5
    o.max_definition_nodes = past + 3;
    css_grammar_limits_for_test(&o, &b, &de, &n);
    EXPECT_EQ(n, (size_t)1 << 28);  // not 3
  }
  o.max_definition_bytes = 100;
  css_grammar_limits_for_test(&o, &b, &de, &n);
  EXPECT_EQ(b, 100u);
  // and a grammar is read and linked and matched under such options
  GTEXT_CSS_Grammar_Options big{};
  // (a byte count past 2^32 on a host that can hold one; on a 32-bit size_t, the largest there is)
  big.max_definition_bytes = sizeof(size_t) > 4 ? static_cast<size_t>((uint64_t{1} << 32) + 5) : SIZE_MAX;
  big.max_definition_nodes = SIZE_MAX;
  big.max_definition_depth = SIZE_MAX;
  Grammar g;
  compile_into(g, "[ a | b ]+", nullptr, &big);
  ASSERT_TRUE(g.ok());
  GTEXT_CSS_Match_Options mo = gtext_css_match_options_default();
  mo.max_memo_entries = sizeof(size_t) > 4 ? static_cast<size_t>(uint64_t{1} << 40) : SIZE_MAX;
  mo.max_match_work = SIZE_MAX;
  Matched m;
  match_into(m, g, "a b a", &mo);
  EXPECT_TRUE(m.matched);
}

TEST(CssGrammarEdges, TheTextsOfTheDefinitionsTogetherAreHeldToTheBytesLimit) {
  Defs d;
  d.types["t"] = std::string(30, 'a');   // 31 with its NUL
  d.types["u"] = std::string(31, 'b');   // 32 with its NUL
  GTEXT_CSS_Grammar_Options o = gtext_css_grammar_options_default();
  o.max_definition_bytes = 64;
  Grammar fits;
  compile_into(fits, "<t> <u>", &d, &o);  // the root: 7 and a NUL; 31; 32 = 71: one too many
  EXPECT_EQ(fits.status, GTEXT_CSS_E_LIMIT_DEFINITION);
  Grammar ok;
  o.max_definition_bytes = 71;
  compile_into(ok, "<t> <u>", &d, &o);
  EXPECT_EQ(ok.status, GTEXT_CSS_OK);
  o.max_definition_bytes = 70;
  Grammar past;
  compile_into(past, "<t> <u>", &d, &o);
  EXPECT_EQ(past.status, GTEXT_CSS_E_LIMIT_DEFINITION);
}

TEST(CssGrammarEdges, ANodeLimitNeverLeavesAGrammarThatIsOnlyPartOfTheText) {
  const std::string text = "[ a | b c ]{1,2} && foo( <number>#{2} )? , x";
  size_t needed = 0;
  for (size_t limit = 1; limit < 40; limit++) {
    GTEXT_CSS_Grammar_Options o = gtext_css_grammar_options_default();
    o.max_definition_nodes = limit;
    GTEXT_CSS_Definition * d = nullptr;
    GTEXT_CSS_Grammar_Result r{};
    const GTEXT_CSS_Status st = gtext_css_definition_parse(text.data(), text.size(), &o, &d, &r);
    if (st == GTEXT_CSS_OK) {
      if (!needed) {
        needed = limit;
      }
      gtext_css_definition_free(d);
    }
    else {
      EXPECT_EQ(st, GTEXT_CSS_E_LIMIT_DEFINITION_NODES) << limit;
      EXPECT_EQ(d, nullptr);
      EXPECT_EQ(needed, 0u) << "a limit above one that suffices must suffice";
    }
  }
  EXPECT_GT(needed, 5u);
}

TEST(CssGrammarEdges, AQuotedLiteralIsOneWellFormedCodePoint) {
  GTEXT_CSS_Grammar_Result r{};
  for (const char * bad : {"'\x80'", "'\xBF'", "'\xC0\x80'", "'\xC1\xBF'", "'\xE0\x80\x80'", "'\xE0\x9F\xBF'", "'\xED\xA0\x80'",
           "'\xF0\x80\x80\x80'", "'\xF4\x90\x80\x80'", "'\xF5\x80\x80\x80'", "'\xE2\x82'", "'\xE2\x28\xA1'", "'\xFF'", "'''", "'\\'",
           "''", "'ab'", "'a", "'\xF0\x9F'"}) {
    EXPECT_EQ(parse_status(bad, &r), GTEXT_CSS_E_GRAMMAR) << bad;
  }
  // (U+2022 and U+00D7 are delimiters in CSS; U+20AC and every code point above U+FFFF are identifier code points, so a
  // literal of those compiles and never matches a delim token)
  EXPECT_EQ(run("a '\xE2\x80\xA2' b", "a \xE2\x80\xA2 b"), "list/space(kw:a tok:\xE2\x80\xA2 kw:b)");
  EXPECT_EQ(run("'\xC3\x97'", "\xC3\x97"), "tok:\xC3\x97");
  EXPECT_EQ(run("'\xE2\x82\xAC'", "\xE2\x82\xAC"), "no match");
  EXPECT_EQ(parse_status("'\xF0\x9F\x98\x80'"), GTEXT_CSS_OK);
  EXPECT_EQ(run("'+' <number>", "+ 5"), "list/space(tok:+ num:5)");
}

TEST(CssGrammarEdges, AQuotedSquareBracketPairIsASquareBracketBlockOfTheInput) {
  Defs d;
  d.types["line-names"] = "'[' <custom-ident>* ']'";
  check_rows({
      {"'[' <custom-ident>* ']'", "[a b]", "block:[(list/space(ci:a ci:b))"},
      {"'[' <custom-ident>* ']'", "[a]", "block:[(list/space(ci:a))"},
      {"'[' <custom-ident>* ']'", "[]", "block:[()"},
      {"'[' <custom-ident>* ']'", "[ a ]", "block:[(list/space(ci:a))"},
      {"'[' <custom-ident>* ']'", "[1]", "no match"},
      {"'[' <custom-ident>* ']'", "(a)", "no match"},
      {"'[' <custom-ident>* ']'", "a", "no match"},
      {"'[' <custom-ident>* ']'", "a]", "no match"},
      {"'[' <custom-ident>* ']'", "[a] [b]", "no match"},
      {"'[' ']'", "[]", "block:[()"},
      // a block that the input leaves open is closed by its end, as the syntax layer reads it
      {"'[' <custom-ident>* ']'", "[a b", "block:[(list/space(ci:a ci:b))"},
      // the unquoted ones still group
      {"[ a | b ] '[' c ']'", "b [c]", "list/space(kw:b block:[(kw:c))"},
      {"'[' [ a | b ]* ']'", "[a b]", "block:[(list/space(kw:a kw:b))"},
      // grid: a track list with names
      {"<line-names>? <length> <line-names>?", "[a] 1px [b c]", "list/space(block:[(list/space(ci:a)) dim:1px block:[(list/space(ci:b ci:c)))"},
  }, &d);
  Grammar g;
  compile_into(g, "'[' <custom-ident>* ']'");
  Matched m;
  match_into(m, g, "[a b]");
  EXPECT_EQ(m.text(), "[a b]");
  EXPECT_EQ(m.root()->kind, GTEXT_CSS_VAL_BLOCK);
  EXPECT_EQ(css_test::str(m.root()->text), "[");
  // the pair has to be balanced in the grammar
  GTEXT_CSS_Grammar_Result r{};
  for (const char * bad : {"'['", "']'", "'[' a", "a ']'", "'[' a ]", "[ a ']'", "'[' [ a ']' ]", "( a ']'"}) {
    EXPECT_EQ(parse_status(bad, &r), GTEXT_CSS_E_GRAMMAR) << bad;
  }
}

TEST(CssGrammarEdges, AnyValueDoesNotContainABadTokenAtAnyDepth) {
  check_rows({
      {"<any-value>", "a (b 'x\ny) c", "no match"},
      {"<any-value>", "a [ ( url(b c) ) ] d", "no match"},
      {"<any-value>", "f(g(h('x\n)))", "no match"},
      {"<any-value>", "a [ ) ]", "no match"},
      {"<any-value>", "a ( ] )", "no match"},
      {"<any-value>", "a { } } b", "no match"},
      {"<any-value>", "a (b) [c] {d} f(e)", "unparsed:a (b) [c] {d} f(e)"},
      {"<any-value>", "a (b ; c) !", "unparsed:a (b ; c) !"},
      {"<declaration-value>", "a (b 'x\ny) c", "no match"},
      {"<declaration-value>", "f(g(url(b c)))", "no match"},
      {"<declaration-value>", "a (b ; c ! d) e", "unparsed:a (b ; c ! d) e"},
      {"<declaration-value>", "a (b ; c) ;", "no match"},
      {"<declaration-value>", "a (b ; c) !", "no match"},
      {"<any-value>", "a f(b) c", "unparsed:a f(b) c"},
      {"x <any-value>", "x (y) 'z'", "list/space(kw:x unparsed:(y) \"z\")"},
  });
}

TEST(CssGrammarEdges, ARatioTakesMathFunctionsForItsNumbersAndIsWrittenWithBoth) {
  check_rows({
      {"<ratio>", "calc(16)/calc(9)", "ratio(math:calc(16) math:calc(9))"},
      {"<ratio>", "calc(16) / 9", "ratio(math:calc(16) num:9)"},
      {"<ratio>", "16 / calc(9)", "ratio(num:16 math:calc(9))"},
      {"<ratio>", "calc(16)", "ratio(math:calc(16) num:1)"},
      {"<ratio>", "calc(1px) / 9", "no match"},
      {"<ratio>", "16 / 9", "ratio:16/9"},
      {"<ratio>", "1.5", "ratio:1.5/1"},
      {"<ratio>", "foo(1) / 9", "no match"},
      {"<ratio>", "calc(1) / -1", "no match"},
      {"<ratio>", "1 / 2 / 3", "no match"},
  });
  check_rows({});
  EXPECT_EQ(run_text("<ratio>", "calc(16)/calc(9)"), "calc(16) / calc(9)");
  EXPECT_EQ(run_text("<ratio>", "calc(16)"), "calc(16) / 1");
  EXPECT_EQ(run_text("<ratio>", "2"), "2 / 1");
  EXPECT_EQ(run_text("<ratio>", "0.5/0"), "0.5 / 0");
  EXPECT_EQ(run_text("auto || <ratio>", "16/9 auto"), "auto 16 / 9");
  Grammar g;
  compile_into(g, "<ratio>");
  Matched m;
  match_into(m, g, "CALC(16) / 9");
  ASSERT_TRUE(m.matched);
  EXPECT_NE(m.root()->flags & GTEXT_CSS_VFLAG_MATH, 0u);
  EXPECT_EQ(m.root()->child_count, 2u);
  EXPECT_EQ(css_test::str(m.root()->children[0].text), "calc(16)");
  EXPECT_NE(m.root()->children[0].math, nullptr);
  EXPECT_EQ(m.root()->interp, GTEXT_CSS_INTERP_NUMBER);
}

TEST(CssGrammarEdges, AMathFunctionStandsForEveryNumericTypeOfTheDraftAndNoOther) {
  // (Values 4, 10) each type takes the math functions whose type is its own (10.9), and no other
  struct Case { const char * type; const char * ok; const char * bad; };
  for (const Case & c : std::vector<Case>{{"<length>", "calc(1px)", "calc(1s)"}, {"<frequency>", "calc(1hz)", "calc(1px)"},
           {"<angle>", "calc(1deg)", "calc(1)"}, {"<time>", "calc(1s)", "calc(1px)"}, {"<flex>", "calc(1fr)", "calc(1px)"},
           {"<resolution>", "calc(1dppx)", "calc(1px)"}, {"<percentage>", "calc(1%)", "calc(1px)"},
           {"<number>", "calc(1)", "calc(1px)"}, {"<integer>", "min(1, 2)", "calc(1%)"}}) {
    EXPECT_EQ(run(c.type, c.ok).substr(0, 5), "math:") << c.type;
    EXPECT_EQ(run(c.type, c.bad), "no match") << c.type;
  }
  for (const char * type : {"<dimension>", "<zero>", "<string>", "<ident>", "<url>", "<custom-ident>", "<urange>"}) {
    EXPECT_EQ(run(type, "calc(1)"), "no match") << type;
  }
  // a range is recorded on the function, for the math layer to clamp to
  Grammar g;
  compile_into(g, "<flex [0fr,∞]>");
  Matched m;
  match_into(m, g, "calc(1fr)");
  ASSERT_TRUE(m.matched);
  EXPECT_EQ(m.root()->range_lo, 0.0);
  EXPECT_TRUE(std::isinf(m.root()->range_hi));
  EXPECT_EQ(m.root()->interp, GTEXT_CSS_INTERP_DISCRETE);
}

TEST(CssGrammarEdges, EveryArbitrarySubstitutionFunctionOfTheDraftsMakesAValuePending) {
  for (const char * f : {"var(--x)", "VAR(--x, 1px)", "env(safe-area-inset-top)", "attr(data-x px)", "if(media(print): 1px; else: 2px)",
           "inherit(width)", "ident(a b)", "random-item(--k, 1px, 2px)", "calc(1px + var(--x))", "a(b(c(if(x: y))))"}) {
    EXPECT_EQ(run("<length>", f).substr(0, 8), "pending:") << f;
    EXPECT_EQ(run("<ident> <ident>", std::string("a ") + f).substr(0, 8), "pending:") << f;
  }
  // a name that is not one of them (a custom function, a function that merely contains the word) is just a function
  for (const char * f : {"--foo(1)", "ifx(1)", "variable(1)", "random(1)", "xinherit(1)", "calc(1px)"}) {
    EXPECT_NE(run("<length>", f).substr(0, 8), "pending:") << f;
  }
}

TEST(CssGrammarEdges, TheMatcherCopiesItsAllocatorAndAcceptsZeroedOptions) {
  Grammar g;
  compile_into(g, "<length> | auto");
  ASSERT_TRUE(g.ok());
  GTEXT_CSS_Matcher mt;
  {
    GTEXT_CSS_Match_Options zeroed;
    std::memset(&zeroed, 0, sizeof zeroed);  // no allocator, no limits: every field means its default
    ASSERT_EQ(gtext_css_matcher_init(&mt, g.c, &zeroed), GTEXT_CSS_OK);
  }
  GTEXT_CSS_Grammar hook = gtext_css_matcher_grammar(&mt);
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_component_values, "12px");
  EXPECT_TRUE(gtext_css_match_grammar(p.tree->values, p.tree->value_count, &hook));
  EXPECT_EQ(mt.status, GTEXT_CSS_OK);
  EXPECT_EQ(dump(gtext_css_value_set_root(gtext_css_matcher_value_set(&mt, 0))), "dim:12px");
  gtext_css_matcher_free(&mt);
  // the caller's allocator struct may go away after init: the matcher holds a copy
  GTEXT_Allocator mine = *gtext_allocator_default();
  GTEXT_CSS_Match_Options o = gtext_css_match_options_default();
  o.allocator = &mine;
  GTEXT_CSS_Matcher m2;
  ASSERT_EQ(gtext_css_matcher_init(&m2, g.c, &o), GTEXT_CSS_OK);
  std::memset(&mine, 0xAB, sizeof mine);
  hook = gtext_css_matcher_grammar(&m2);
  EXPECT_TRUE(gtext_css_match_grammar(p.tree->values, p.tree->value_count, &hook));
  gtext_css_matcher_free(&m2);
}

TEST(CssGrammarEdges, TheTextOfAnOpaqueValueIsBoundedAndTheRealStatusComesBack) {
  Grammar g;
  compile_into(g, "<any-value>");
  ASSERT_TRUE(g.ok());
  const std::string input = "abcdef ghijkl";  // "abcdef ghijkl" is 13 bytes
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_component_values, input);
  for (size_t cap = 1; cap <= 14; cap++) {
    GTEXT_CSS_Match_Options o = gtext_css_match_options_default();
    o.max_text_bytes = cap;
    bool matched = false;
    GTEXT_CSS_Value_Set * set = nullptr;
    GTEXT_CSS_Match_Result r{};
    const GTEXT_CSS_Status st = gtext_css_match(g.c, p.tree->values, p.tree->value_count, &o, &matched, &set, &r);
    if (cap >= 13) {
      EXPECT_EQ(st, GTEXT_CSS_OK) << cap;
      EXPECT_TRUE(matched);
      gtext_css_value_set_free(set);
    }
    else {
      EXPECT_EQ(st, GTEXT_CSS_E_LIMIT_OUTPUT) << cap;
      EXPECT_EQ(r.status, GTEXT_CSS_E_LIMIT_OUTPUT);
      EXPECT_EQ(set, nullptr);
    }
  }
  // a pending value and a math function's arguments are opaque text as well
  Grammar l;
  compile_into(l, "<length>");
  css_test::Parsed pend, mathf;
  css_test::parse_into(pend, gtext_css_parse_component_values, "var(--aaaaaaaaaa)");
  css_test::parse_into(mathf, gtext_css_parse_component_values, "calc(1px + 2px + 3px)");
  GTEXT_CSS_Match_Options o = gtext_css_match_options_default();
  o.max_text_bytes = 5;
  bool matched = false;
  GTEXT_CSS_Value_Set * set = nullptr;
  EXPECT_EQ(gtext_css_match(l.c, pend.tree->values, pend.tree->value_count, &o, &matched, &set, nullptr), GTEXT_CSS_E_LIMIT_OUTPUT);
  EXPECT_EQ(gtext_css_match(l.c, mathf.tree->values, mathf.tree->value_count, &o, &matched, &set, nullptr), GTEXT_CSS_E_LIMIT_OUTPUT);
  EXPECT_EQ(set, nullptr);
}

TEST(CssGrammarEdges, ACommaOrSlashSeparatesSegmentsOnlyWhenEverySegmentConsumesSomething) {
  Defs d;
  d.types["maybe"] = "a?";
  d.types["maybe-via"] = "<maybe>";
  d.types["sure"] = "a";
  check_rows({
      // <maybe> can be empty, so a comma next to it stays an item: a value must write the comma back
      {"<maybe> , x", ", x", "list/space(tok:, kw:x)"},
      {"<maybe> , x", "a, x", "list/space(kw:a tok:, kw:x)"},
      {"<maybe-via> , x", ", x", "list/space(tok:, kw:x)"},
      {"x , <maybe>", "x ,", "list/space(kw:x tok:,)"},
      {"x / <maybe>", "x /", "list/space(kw:x tok:/)"},
      // <sure> cannot: the same shapes are lists with a separator
      {"<sure> , x", "a, x", "list/comma(kw:a kw:x)"},
      {"x , <sure>", "x, a", "list/comma(kw:x kw:a)"},
      {"<sure> / x", "a/x", "list/slash(kw:a kw:x)"},
      // a group needs only one member that consumes something to be a segment, and || needs all of them not to
      {"[ b && c? ] , x", "b, x", "list/comma(list/space(kw:b) kw:x)"},
      {"[ b && c? ] , x", "c b, x", "list/comma(list/space(kw:b kw:c) kw:x)"},
      {"[ b? && c? ] , x", ", x", "list/space(tok:, kw:x)"},
      {"[ b? || c? ] , x", ", x", "list/space(tok:, kw:x)"},
      {"[ b || c? ] , x", "b, x", "list/space(list/space(kw:b) tok:, kw:x)"},  // (c? alone is a match of nothing)
      {"[ b || c ] , x", "b, x", "list/comma(list/space(kw:b) kw:x)"},
      {"<maybe> / x , y", "/ x, y", "list/comma(list/space(tok:/ kw:x) kw:y)"},
  }, &d);
  // and what is written reads back as the same text
  for (const char * in : {", x", "a, x", "x ,"}) {
    const std::string first = run_text("<maybe> , x | x ,", in, &d);
    if (first != "no match") {
      EXPECT_EQ(run_text("<maybe> , x | x ,", first, &d), first) << in;
    }
  }
}

TEST(CssGrammarEdges, AListSpanStartsAtTheFirstChildThatHasOne) {
  Grammar g;
  compile_into(g, "<whitespace-token> a <whitespace-token>");
  ASSERT_TRUE(g.ok());
  Matched m;
  match_into(m, g, "  a ");
  ASSERT_TRUE(m.matched);
  const GTEXT_CSS_Value * l = m.root();
  ASSERT_EQ(l->kind, GTEXT_CSS_VAL_LIST);
  ASSERT_GE(l->child_count, 2u);
  EXPECT_EQ(l->children[0].kind, GTEXT_CSS_VAL_TOKEN);
  EXPECT_EQ(l->children[0].span.line, 0u);  // a white-space token has no span
  EXPECT_EQ(l->span.offset, 2u);
  EXPECT_EQ(l->span.end, 3u);
  EXPECT_EQ(l->span.line, 1u);
  EXPECT_EQ(l->span.column, 3u);
  // a run that is empty has none either
  Grammar b;
  compile_into(b, "{ <declaration-list> }");
  Matched k;
  match_declaration(k, b, "x:  { }");
  ASSERT_TRUE(k.matched);
  EXPECT_EQ(k.root()->kind, GTEXT_CSS_VAL_BLOCK);
  EXPECT_EQ(k.root()->child_count, 1u);
  EXPECT_EQ(k.root()->children[0].span.line, 0u);
  EXPECT_NE(k.root()->span.line, 0u);
}

TEST(CssGrammarEdges, LinkingAChainOfTypesThatCanMatchNothingTakesWorkProportionalToItsLength) {
  size_t last = 0;
  for (int n : {500, 2000, 8000}) {
    Defs d;
    for (int i = 0; i < n; i++) {
      d.types["t" + std::to_string(i)] = i + 1 < n ? "<t" + std::to_string(i + 1) + ">? a?" : "a?";
    }
    GTEXT_CSS_Grammar_Options o = gtext_css_grammar_options_default();
    o.max_definition_nodes = 1 << 20;
    o.max_definition_bytes = 1 << 26;
    d.options = o;
    Grammar g;
    compile_into(g, "<t0>", &d, &o);
    ASSERT_TRUE(g.ok()) << n << " " << gtext_css_status_string(g.status);
    EXPECT_GT(g.result.work, (size_t)n);
    if (last) {
      EXPECT_LE(g.result.work, last * 5) << "4x the chain must cost about 4x the work";
    }
    last = g.result.work;
  }
}

TEST(CssGrammarEdges, TheWorkOfAnAndOfWideIntervalsIsChargedForTheLooksItTakes) {
  Grammar g;
  compile_into(g, "<any-value> && <any-value>");
  ASSERT_TRUE(g.ok());
  std::string in;
  for (int i = 0; i < 400; i++) {
    in += "a ";
  }
  Matched m;
  match_into(m, g, in);
  ASSERT_TRUE(m.matched);
  // every state of the first member looks at every position after it: about n*n/2 looks, all charged
  EXPECT_GT(m.result.work, (size_t)400 * 400 / 4);
  GTEXT_CSS_Match_Options o = gtext_css_match_options_default();
  o.max_match_work = 400 * 400 / 8;
  Matched capped;
  match_into(capped, g, in, &o);
  EXPECT_EQ(capped.status, GTEXT_CSS_E_LIMIT_WORK);
  // the extraction of a repetition over a wide interval is charged as well
  Grammar h;
  compile_into(h, "[ <any-value> ]#");
  ASSERT_TRUE(h.ok());
  Matched w;
  match_into(w, h, in);
  ASSERT_TRUE(w.matched);
  GTEXT_CSS_Match_Options small = gtext_css_match_options_default();
  small.max_match_work = w.result.work - 1;
  Matched cut;
  match_into(cut, h, in, &small);
  EXPECT_EQ(cut.status, GTEXT_CSS_E_LIMIT_WORK);
}

TEST(CssGrammarEdges, MemoryOfTheMatchIsBoundedByAMultipleOfTheMemoEntries) {
  // The sets of positions and the arrays of the typed value are not counted as memo entries; the real bound is shown here
  // by the bytes the allocator was asked for at its peak, over the memo entries and the input's items, on inputs built to make
  // them large. Measured: at most about 410 bytes for each item or entry (4000-token inputs), and 28 KB with nothing in it;
  // the bound asserted is 512 bytes each and 64 KiB.
  struct Shape {
    const char * grammar;
    std::string input;
  };
  std::string a, ab;
  for (int i = 0; i < 4000; i++) {
    a += "a ";
    ab += "a b ";
  }
  const std::vector<Shape> shapes = {{"<any-value> && <any-value>", a}, {"[ a | a a ]+ b", a + "b"}, {"[ a* b? ]* c", ab + "c"},
      {"[ a | b ]#", "a, b, a, b, a, b, a, b"}, {"a* a* a* a*", a}};
  for (const Shape & s : shapes) {
    CountingAllocator ca;
    Grammar g;
    compile_into(g, s.grammar);
    ASSERT_TRUE(g.ok());
    css_test::Parsed p;
    css_test::parse_into(p, gtext_css_parse_component_values, s.input);
    GTEXT_CSS_Match_Options o = gtext_css_match_options_default();
    o.allocator = &ca.alloc;
    bool matched = false;
    GTEXT_CSS_Value_Set * set = nullptr;
    GTEXT_CSS_Match_Result r{};
    ca.reset();
    ASSERT_EQ(gtext_css_match(g.c, p.tree->values, p.tree->value_count, &o, &matched, &set, &r), GTEXT_CSS_OK) << s.grammar;
    const size_t items = p.tree->value_count + 1;
    const size_t bound = 65536 + 512 * (r.memo_entries + items);
    EXPECT_LE(ca.peak, bound) << s.grammar << ": peak " << ca.peak << " bytes, memo entries " << r.memo_entries << ", items " << items;
    gtext_css_value_set_free(set);
  }
}

extern "C" const void * css_ptr_at(const void * base, size_t index, size_t elem);

TEST(CssGrammarEdges, APoolThatDoesNotExistIsNotOffsetFromAndAMatchOfEmptyAnswersReadsNone) {
  // The memo of a match whose answers all have no intervals has no pool, and "pool + 0" of a null pointer is undefined (the
  // fuzzer's UBSan finding); the one place that offsets a pool does not offset a null one.
  EXPECT_EQ(css_ptr_at(nullptr, 0, 8), nullptr);
  EXPECT_EQ(css_ptr_at(nullptr, 5, 8), nullptr);
  const unsigned char bytes[64] = {};
  EXPECT_EQ(css_ptr_at(bytes, 0, 8), static_cast<const void *>(bytes));
  EXPECT_EQ(css_ptr_at(bytes, 3, 8), static_cast<const void *>(bytes + 24));
  // and such matches work: every answer of the memo has no interval, in a grammar with groups, repetitions and an &&
  for (const char * g : {"a? b?", "[ a? b? ]*", "[ a? && b? ]#?", "[ [ a? ]* ]+", "x? y? z?"}) {
    for (const char * in : {"", "a", "a b", "x"}) {
      Grammar gr;
      compile_into(gr, g);
      ASSERT_TRUE(gr.ok()) << g;
      Matched m;
      match_into(m, gr, in);
      EXPECT_EQ(m.status, GTEXT_CSS_OK) << g << " | " << in;
    }
  }
}

TEST(CssGrammarEdges, ARangeBroughtByACompositeDoesNotNarrowARangeAlreadyOnATerminalItDoesNotFit) {
  Defs d;
  d.types["t"] = "<length [0px,∞]> | <percentage>";
  check_rows({
      {"<t [0%,50%]>", "99px", "dim:99px"},
      {"<t [0%,50%]>", "-1px", "no match"},
      {"<t [0%,50%]>", "40%", "pct:40"},
      {"<t [0%,50%]>", "60%", "no match"},
  }, &d);
}

TEST(CssGrammarEdges, ARangeBroughtByACompositeIsDroppedWhereItDoesNotApplyToATerminalWithNoRangeOfItsOwn) {
  Defs d;
  d.types["t"] = "<length> | <percentage>";
  check_rows({
      {"<t [0%,50%]>", "99px", "dim:99px"},
      {"<t [0%,50%]>", "40%", "pct:40"},
      {"<t [0%,50%]>", "60%", "no match"},
  }, &d);
}

TEST(CssGrammarEdges, AMatchThatReachesItsWorkBoundAtAnyPointLeavesNothingBehind) {
  struct Case {
    const char * grammar;
    const char * input;
  };
  for (const Case & c : std::vector<Case>{{"[ a? b? ]!", ""}, {"[ a? b? ]!", "a b"}, {"[ a || b ]# c?", "a b, b a c"}, {"[ a* b? ]+ c", "a a b a c"},
           {"<any-value> && <any-value>", "x y z"}, {"foo( [ a | b ]+ ) bar( c? )", "foo(a b) bar()"}, {"a '/' b , [ c ]!", "a / b , c"}}) {
    Grammar g;
    compile_into(g, c.grammar);
    ASSERT_TRUE(g.ok()) << c.grammar;
    css_test::Parsed p;
    css_test::parse_into(p, gtext_css_parse_component_values, c.input);
    Matched probe;
    match_values(probe, g, p.tree->values, p.tree->value_count);
    ASSERT_EQ(probe.status, GTEXT_CSS_OK);
    for (size_t bound = 1; bound <= probe.result.work + 1; bound++) {
      FaultAllocator fa;
      GTEXT_CSS_Match_Options o = gtext_css_match_options_default();
      o.allocator = &fa.alloc;
      o.max_match_work = bound;
      bool matched = false;
      GTEXT_CSS_Value_Set * set = nullptr;
      const GTEXT_CSS_Status st = gtext_css_match(g.c, p.tree->values, p.tree->value_count, &o, &matched, &set, nullptr);
      EXPECT_TRUE(st == GTEXT_CSS_OK || st == GTEXT_CSS_E_LIMIT_WORK) << c.grammar << " " << bound;
      gtext_css_value_set_free(set);
      EXPECT_TRUE(fa.live.empty()) << c.grammar << " | " << c.input << ": a block is left at a work bound of " << bound;
      EXPECT_EQ(fa.bad_frees, 0u);
    }
    for (size_t memo = 1; memo <= probe.result.memo_entries + 1; memo++) {
      FaultAllocator fa;
      GTEXT_CSS_Match_Options o = gtext_css_match_options_default();
      o.allocator = &fa.alloc;
      o.max_memo_entries = memo;
      bool matched = false;
      GTEXT_CSS_Value_Set * set = nullptr;
      const GTEXT_CSS_Status st = gtext_css_match(g.c, p.tree->values, p.tree->value_count, &o, &matched, &set, nullptr);
      EXPECT_TRUE(st == GTEXT_CSS_OK || st == GTEXT_CSS_E_LIMIT_WORK) << c.grammar << " " << memo;
      gtext_css_value_set_free(set);
      EXPECT_TRUE(fa.live.empty()) << c.grammar << ": a block is left at a memo bound of " << memo;
    }
  }
}

} // namespace
