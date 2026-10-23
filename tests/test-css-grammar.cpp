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
 * The Value Definition Syntax engine (css_grammar.h), one requirement at a
 * time: what each construct of the notation means, the matrix of the spec, the
 * errors of linking, and the limits at their bounds.
 *
 * The normative text is CSS Values and Units Level 4 (Editor's Draft), section 2,
 * "Value Definition Syntax": quoted where a test depends on a sentence.
 */

#include <string>

#include <gtest/gtest.h>

#include "css_values_test_support.h"

using namespace gv;

namespace {

Defs & standard_defs() {
  static Defs defs;
  if (defs.types.empty()) {
    defs.types["length-percentage"] = "<length> | <percentage>";
    defs.types["position"] = "[ left | center | right ] || [ top | center | bottom ]";
  }
  return defs;
}

TEST(CssGrammarMatrix, MultiplierOverALengthPercentageWithARangeAndAKeyword) {
  Defs & d = standard_defs();
  EXPECT_EQ(run("[ <length-percentage [0,∞]> | auto ]{1,4}", "10px auto 5%", &d), "list/space(dim:10px kw:auto pct:5)");
}

TEST(CssGrammarMatrix, AnyOrderIsStoredInGrammarOrderWithTheAbsentOmitted) {
  Grammar g;
  compile_into(g, "a || b || c");
  ASSERT_TRUE(g.ok());
  Matched m;
  match_into(m, g, "c a");
  ASSERT_EQ(m.status, GTEXT_CSS_OK);
  EXPECT_EQ(m.dump(), "list/space(kw:a kw:c)");
  EXPECT_EQ(m.text(), "a c");
}

TEST(CssGrammarMatrix, ANumberOutOfItsRangeDoesNotMatch) {
  EXPECT_EQ(run("<number [0,∞]>", "-1"), "no match");
  EXPECT_EQ(run("<number [0,∞]>", "0"), "num:0");
  EXPECT_EQ(run("<number [0,∞]>", "2.5"), "num:2.5");
}

TEST(CssGrammarMatrix, LeftRecursionIsRefusedAtLinkNamingTheType) {
  Defs d;
  d.types["x"] = "<x> | a";
  Grammar g;
  compile_into(g, "<x>", &d);
  EXPECT_EQ(g.status, GTEXT_CSS_E_LEFT_RECURSION);
  EXPECT_STREQ(g.result.name, "<x>");
}

TEST(CssGrammarMatrix, AnUnknownTypeIsRefusedAtLinkNamingIt) {
  Grammar g;
  compile_into(g, "<nosuch>");
  EXPECT_EQ(g.status, GTEXT_CSS_E_REFERENCE);
  EXPECT_STREQ(g.result.name, "<nosuch>");
  Defs d;
  Grammar g2;
  compile_into(g2, "a | <'nosuch'>", &d);
  EXPECT_EQ(g2.status, GTEXT_CSS_E_REFERENCE);
  EXPECT_STREQ(g2.result.name, "<'nosuch'>");
}

TEST(CssGrammarMatrix, AValueWithVarIsPendingSubstitution) {
  EXPECT_EQ(run("<length>", "var(--w)"), "pending:var(--w)");
  EXPECT_EQ(run("<length>", "calc(var(--w) + 1px)"), "pending:calc(var(--w) + 1px)");
  EXPECT_EQ(run("<length>", "env(safe-area-inset-top)"), "pending:env(safe-area-inset-top)");
  EXPECT_EQ(run("auto", "attr(x)"), "pending:attr(x)");
  EXPECT_EQ(run("auto", "VAR(--x)"), "pending:VAR(--x)");
}

TEST(CssGrammarMatrix, UnitsAndTheUnitlessZero) {
  EXPECT_EQ(run("<length>", "10foo"), "no match");
  EXPECT_EQ(run("<length>", "0"), "dim:0px");
  EXPECT_EQ(run("<length>", "0 "), "dim:0px");
  EXPECT_EQ(run("<angle>", "0"), "no match");
  EXPECT_EQ(run("<time>", "0"), "no match");
  EXPECT_EQ(run("<percentage>", "0"), "no match");
  EXPECT_EQ(run("<length>", "5"), "no match");
}


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

TEST(CssGrammarNotation, JuxtapositionNeedsEveryComponentInOrder) {
  check_rows({
      {"a b c", "a b c", "list/space(kw:a kw:b kw:c)"},
      {"a b c", "a c b", "no match"},
      {"a b c", "a b", "no match"},
      {"a b c", "a b c d", "no match"},
      {"a b c", "A  /* x */ B\n C", "list/space(kw:a kw:b kw:c)"},
  });
}

TEST(CssGrammarNotation, AlternationTriesAlternativesInWrittenOrder) {
  check_rows({
      {"a | b", "b", "kw:b"},
      {"<custom-ident> | auto", "auto", "ci:auto"},
      {"auto | <custom-ident>", "auto", "kw:auto"},
      {"auto | <custom-ident>", "other", "ci:other"},
      {"<number> | <integer>", "3", "num:3"},
      {"<integer> | <number>", "3", "int:3"},
      {"<integer> | <number>", "3.5", "num:3.5"},
  });
}

TEST(CssGrammarNotation, PrecedenceIsJuxtapositionThenAndThenOrThenBar) {
  check_rows({
      // "a b | c" is "[a b] | c"
      {"a b | c", "c", "kw:c"},
      {"a b | c", "a b", "list/space(kw:a kw:b)"},
      {"a b | c", "a", "no match"},
      // "a | b && c" is "a | [b && c]"
      {"a | b && c", "c b", "list/space(kw:b kw:c)"},
      {"a | b && c", "a", "kw:a"},
      {"a | b && c", "b", "no match"},
      // "a && b || c" is "[a && b] || c"
      {"a && b || c", "c b a", "list/space(list/space(kw:a kw:b) kw:c)"},
      {"a && b || c", "c", "list/space(kw:c)"},
      {"a && b || c", "b", "no match"},
      // "a || b | c" is "[a || b] | c"
      {"a || b | c", "c", "kw:c"},
      {"a || b | c", "b a", "list/space(kw:a kw:b)"},
      // "a b || c d": juxtaposition binds tighter than "||"
      {"a b || c d", "c d a b", "list/space(list/space(kw:a kw:b) list/space(kw:c kw:d))"},
      {"a b || c d", "a c d", "no match"},
  });
}

TEST(CssGrammarNotation, AndAndNeedsAllInAnyOrderAndOrOrNeedsOneOrMore) {
  check_rows({
      {"a && b && c", "c a b", "list/space(kw:a kw:b kw:c)"},
      {"a && b && c", "a b", "no match"},
      {"a && b && c", "a b c a", "no match"},
      {"a || b || c", "c", "list/space(kw:c)"},
      {"a || b || c", "b c", "list/space(kw:b kw:c)"},
      {"a || b || c", "", "no match"},
      {"a || b || c", "a a", "no match"},
      {"a? || b?", "", "<none>"},
      {"a? && b", "b", "list/space(kw:b)"},
  });
}

TEST(CssGrammarNotation, TheMultipliersCountRepetitions) {
  check_rows({
      {"a?", "", "<none>"},
      {"a?", "a", "kw:a"},
      {"a?", "a a", "no match"},
      {"a*", "", "<none>"},
      {"a*", "a a a", "list/space(kw:a kw:a kw:a)"},
      {"a+", "", "no match"},
      {"a+", "a", "list/space(kw:a)"},
      {"a{2}", "a a", "list/space(kw:a kw:a)"},
      {"a{2}", "a", "no match"},
      {"a{2}", "a a a", "no match"},
      {"a{2,3}", "a", "no match"},
      {"a{2,3}", "a a a", "list/space(kw:a kw:a kw:a)"},
      {"a{2,3}", "a a a a", "no match"},
      {"a{2,}", "a a a a a", "list/space(kw:a kw:a kw:a kw:a kw:a)"},
      {"a{2,}", "a", "no match"},
      {"a#", "a", "list/comma(kw:a)"},
      {"a#", "a, a, a", "list/comma(kw:a kw:a kw:a)"},
      {"a#", "a a", "no match"},
      {"a#", "a,", "no match"},
      {"a#", ", a", "no match"},
      {"a#{2,3}", "a", "no match"},
      {"a#{2,3}", "a,a", "list/comma(kw:a kw:a)"},
      {"a#{2,3}", "a,a,a,a", "no match"},
      {"a#{2}", "a,a", "list/comma(kw:a kw:a)"},
      {"a#?", "", "<none>"},
      {"a#?", "a,a", "list/comma(kw:a kw:a)"},
      {"a+#", "a a, a", "list/comma(list/space(kw:a kw:a) list/space(kw:a))"},
      {"[a b]+", "a b a b", "list/space(list/space(kw:a kw:b) list/space(kw:a kw:b))"},
      {"[a | b]*", "a b b a", "list/space(kw:a kw:b kw:b kw:a)"},
      {"[a? b?]{2}", "", "<none>"},
      {"[a? b?]{2}", "a b", "list/space(list/space(kw:a kw:b))"},
  });
}

TEST(CssGrammarNotation, AGroupWithABangMustProduceSomething) {
  check_rows({
      {"[ a? b? ]!", "", "no match"},
      {"[ a? b? ]!", "b", "list/space(kw:b)"},
      {"[ a? b? ]", "", "<none>"},
      {"[ a? || b? ]!", "", "no match"},
      {"[ a? || b? ]!", "b a", "list/space(kw:a kw:b)"},
  });
}

TEST(CssGrammarNotation, LiteralCharactersAreTokensAndSeparateListItems) {
  check_rows({
      {"a , b", "a, b", "list/comma(kw:a kw:b)"},
      {"a , b", "a b", "no match"},
      {"a / b", "a/b", "list/slash(kw:a kw:b)"},
      {"a / b", "a / b", "list/slash(kw:a kw:b)"},
      {"a / b", "a b", "no match"},
      {"a / b", "a , b", "no match"},
      {"a / b", "a /", "no match"},
      {"a / b , c", "a/b, c", "list/comma(list/slash(kw:a kw:b) kw:c)"},
      {"a b / c", "a b / c", "list/slash(list/space(kw:a kw:b) kw:c)"},
      {"a : b", "a : b", "list/space(kw:a tok:: kw:b)"},
      {"a ; b", "a ; b", "list/space(kw:a tok:; kw:b)"},
      {"'/' a", "/ a", "list/space(tok:/ kw:a)"},
      {"a '=' b", "a = b", "list/space(kw:a tok:= kw:b)"},
      // a separator that may be empty on a side stays a token, so the text reads back as the same
      {"a /", "a /", "list/space(kw:a tok:/)"},
      {"a / b?", "a /", "list/space(kw:a tok:/)"},
      {"a? / b", "/ b", "list/space(tok:/ kw:b)"},
  });
}

TEST(CssGrammarNotation, KeywordsMatchWithoutRegardToCaseAndKeepTheGrammarsSpelling) {
  EXPECT_EQ(run("auto | NONE", "AUTO"), "kw:auto");
  EXPECT_EQ(run("auto | NONE", "none"), "kw:NONE");
  EXPECT_EQ(run_text("sRGB", "SRGB"), "sRGB");
}

TEST(CssGrammarNotation, FunctionNotationMatchesTheNameAndItsArguments) {
  check_rows({
      {"foo( a | b )", "foo(b)", "fn:foo(kw:b)"},
      {"foo( a | b )", "FOO( b )", "fn:foo(kw:b)"},
      {"foo( a | b )", "foo()", "no match"},
      {"foo( a | b )", "bar(a)", "no match"},
      {"foo( a | b )", "xfoo(a)", "no match"},
      {"foo( a | b )", "foo(a b)", "no match"},
      {"foo()", "foo()", "fn:foo()"},
      {"foo()", "foo( )", "fn:foo()"},
      {"foo()", "foo(a)", "no match"},
      {"foo( <number>#{3} )", "foo(1, 2, 3)", "fn:foo(list/comma(num:1 num:2 num:3))"},
      {"foo( a? )", "foo()", "fn:foo()"},
      {"foo( bar( a ) )", "foo(bar(a))", "fn:foo(fn:bar(kw:a))"},
      {"foo( a ) b", "foo(a) b", "list/space(fn:foo(kw:a) kw:b)"},
      {"foo( a )", "foo", "no match"},
      {"foo( a )", "(a)", "no match"},
  });
}

TEST(CssGrammarNotation, BlockNotationMatchesABracketedBlock) {
  Grammar g;
  compile_into(g, "{ <declaration-list> }");
  ASSERT_TRUE(g.ok());
  Matched m;
  match_declaration(m, g, "x: { a: b; c: d }");
  ASSERT_EQ(m.status, GTEXT_CSS_OK);
  EXPECT_EQ(m.dump(), "block:{(unparsed:a: b; c: d)");
  Matched e;
  match_declaration(e, g, "x: { }");
  EXPECT_EQ(e.dump(), "block:{(unparsed:)");
  Matched n;
  match_declaration(n, g, "x: ( a )");
  EXPECT_EQ(n.dump(), "no match");
  EXPECT_EQ(run("( <number> )", "(3)"), "block:((num:3)");
  EXPECT_EQ(run("( <number> )", "{3}"), "no match");
  EXPECT_EQ(run("( <number> )", "(3 4)"), "no match");
  EXPECT_EQ(run("( )", "()"), "block:(()");
}

TEST(CssGrammarNotation, BacktrackingFindsTheParseThatConsumesAllTheInput) {
  check_rows({
      {"a* a", "a a a", "list/space(list/space(kw:a kw:a) kw:a)"},
      {"a? a", "a", "list/space(kw:a)"},
      {"[a | a b] c", "a b c", "list/space(list/space(kw:a kw:b) kw:c)"},
      {"[a | a b] c", "a c", "list/space(kw:a kw:c)"},
      {"a* b? a*", "a a b a", "list/space(list/space(kw:a kw:a) kw:b list/space(kw:a))"},
      {"[a b?]+ c", "a b a c", "list/space(list/space(list/space(kw:a kw:b) list/space(kw:a)) kw:c)"},
      {"<length>? <length>? <length>", "1px 2px", "list/space(dim:1px dim:2px)"},
  });
}

TEST(CssGrammarNotation, TheEarlierItemsOfAJuxtapositionTakeAsMuchAsTheyCan) {
  // both ways of splitting "a a" between two stars are parses; the first star keeps both
  EXPECT_EQ(run("a* a*", "a a"), "list/space(list/space(kw:a kw:a))");
  EXPECT_EQ(run("a* a? a?", "a a a"), "list/space(list/space(kw:a kw:a kw:a))");
  EXPECT_EQ(run("a? a* a?", "a a"), "list/space(kw:a list/space(kw:a))");
}

TEST(CssGrammarNotation, TheSameInputGivesTheSameAnswerEveryTime) {
  Grammar g;
  compile_into(g, "[ a | a b | b ]* c?");
  ASSERT_TRUE(g.ok());
  std::string first;
  for (int i = 0; i < 5; i++) {
    Matched m;
    match_into(m, g, "a b a b b a c");
    ASSERT_TRUE(m.matched);
    if (i == 0) {
      first = m.dump();
    }
    EXPECT_EQ(m.dump(), first);
  }
}

TEST(CssGrammarNotation, ANestedMultiplierKeepsTheShapeItIsWrittenIn) {
  // "[ x* ]+" is a repetition of a repetition: a list of lists, as written (and still linear: the quadratic suite)
  check_rows({
      {"[ a* ]+", "a a", "list/space(list/space(kw:a kw:a))"},
      {"[ a+ ]*", "a a", "list/space(list/space(kw:a kw:a))"},
      {"[ a? ]*", "a a", "list/space(kw:a kw:a)"},
      {"[ a? ]*", "", "<none>"},
      {"[ a* ]?", "a a", "list/space(kw:a kw:a)"},
      {"[ a+ ]{2,}", "a a", "list/space(list/space(kw:a) list/space(kw:a))"},
      {"[ a? ]?", "a", "kw:a"},
      {"[ a+ ]#", "a a, a", "list/comma(list/space(kw:a kw:a) list/space(kw:a))"},
  });
  Grammar g;
  compile_into(g, "[ a* ]+");
  ASSERT_TRUE(g.ok());
  EXPECT_EQ(gtext_css_compiled_node_count(g.c), 3u);  // the keyword and the two multipliers
  Matched m;
  match_into(m, g, "a a");
  size_t len = 0;
  const char * t = gtext_css_compiled_slot_text(g.c, m.root()->slot, &len);
  EXPECT_EQ(std::string(t, len), "[ a* ]+");
  t = gtext_css_compiled_slot_text(g.c, m.root()->children[0].slot, &len);
  EXPECT_EQ(std::string(t, len), "a*");
}

TEST(CssGrammarTypes, EachNativeTypeMatchesWhatTheDraftSays) {
  check_rows({
      {"<integer>", "3", "int:3"},
      {"<integer>", "-3", "int:-3"},
      {"<integer>", "3.0", "no match"},
      {"<integer>", "3px", "no match"},
      {"<number>", "3", "num:3"},
      {"<number>", "-.5e1", "num:-5"},
      {"<number>", "3%", "no match"},
      {"<percentage>", "50%", "pct:50"},
      {"<percentage>", "50", "no match"},
      {"<dimension>", "3foo", "dim:3foo"},
      {"<dimension>", "3px", "dim:3px"},
      {"<dimension>", "3", "no match"},
      {"<length>", "3PX", "dim:3px"},
      {"<length>", "1Q", "dim:1Q"},
      {"<length>", "1q", "dim:1Q"},
      {"<length>", "2.54cm", "dim:2.54cm"},
      {"<length>", "3deg", "no match"},
      {"<length>", "3", "no match"},
      {"<angle>", "90deg", "dim:90deg"},
      {"<angle>", "0.25turn", "dim:0.25turn"},
      {"<angle>", "1px", "no match"},
      {"<time>", "1s", "dim:1s"},
      {"<time>", "100ms", "dim:100ms"},
      {"<frequency>", "10kHz", "dim:10kHz"},
      {"<frequency>", "10khz", "dim:10kHz"},
      {"<resolution>", "2x", "dim:2x"},
      {"<resolution>", "96dpi", "dim:96dpi"},
      {"<flex>", "1fr", "dim:1fr"},
      {"<ratio>", "16 / 9", "ratio:16/9"},
      {"<ratio>", "16/9", "ratio:16/9"},
      {"<ratio>", "1.5", "ratio:1.5/1"},
      {"<ratio>", "0 / 1", "ratio:0/1"},
      {"<ratio>", "-1 / 1", "no match"},
      {"<ratio>", "1 / -1", "no match"},
      {"<ratio>", "1 / 2 / 3", "no match"},
      {"<ratio>", "1 /", "no match"},
      {"<zero>", "0", "zero"},
      {"<zero>", "0.0", "zero"},
      {"<zero>", "1", "no match"},
      {"<string>", "'x'", "str:x"},
      {"<string>", "x", "no match"},
      {"<ident>", "x", "kw:x"},
      {"<ident>", "'x'", "no match"},
      {"<custom-ident>", "foo", "ci:foo"},
      {"<custom-ident>", "Foo", "ci:Foo"},
      {"<custom-ident>", "initial", "no match"},
      {"<custom-ident>", "INHERIT", "no match"},
      {"<custom-ident>", "unset", "no match"},
      {"<custom-ident>", "revert", "no match"},
      {"<custom-ident>", "revert-layer", "no match"},
      {"<custom-ident>", "default", "no match"},
      {"<dashed-ident>", "--foo", "di:--foo"},
      {"<dashed-ident>", "foo", "no match"},
      {"<dashed-ident>", "-foo", "no match"},
  });
}

TEST(CssGrammarTypes, AUrlIsAUrlTokenOrUrlWithAStringAndModifiers) {
  check_rows({
      {"<url>", "url(a.png)", "url:a.png"},
      {"<url>", "url( a.png )", "url:a.png"},
      {"<url>", "url('a b.png')", "url:a b.png"},
      {"<url>", "url(\"a\" foo)", "url:a(tok:foo)"},
      {"<url>", "url(\"a\" foo bar(1))", "url:a(tok:foo tok:bar(1))"},
      {"<url>", "URL('a')", "url:a"},
      {"<url>", "url()", "url:"},
      {"<url>", "url(a b)", "no match"},
      {"<url>", "url(1)", "url:1"},
      {"<url>", "url(\"1\" 2)", "no match"},
      {"<url>", "image('a')", "no match"},
  });
}

TEST(CssGrammarTypes, AnyValueAndDeclarationValueTakeOneOrMoreComponentValues) {
  check_rows({
      {"<any-value>", "a b (c [d]) e(f)", "unparsed:a b (c [d]) e(f)"},
      {"<any-value>", "", "no match"},
      {"<any-value>", "  ", "no match"},
      {"<any-value>", "a ; b", "unparsed:a ; b"},
      {"<any-value>", "a ! b", "unparsed:a ! b"},
      {"<any-value>", "a ) b", "no match"},
      {"<any-value>", "a ] b", "no match"},
      {"<any-value>", "a } b", "no match"},
      {"<any-value>", "a 'b\nc", "no match"},
      {"<any-value>", "a url(b c)", "no match"},
      {"<any-value>", "a (b ; c)", "unparsed:a (b ; c)"},
      {"<declaration-value>", "a ; b", "no match"},
      {"<declaration-value>", "a ! b", "no match"},
      {"<declaration-value>", "a (b ; c) d", "unparsed:a (b ; c) d"},
      {"<declaration-value>", "a b", "unparsed:a b"},
      {"a <any-value>", "a b c", "list/space(kw:a unparsed:b c)"},
      {"<any-value> a", "b c a", "list/space(unparsed:b c kw:a)"},
      {"<any-value> a <any-value>", "x a y a z", "list/space(unparsed:x a y kw:a unparsed:z)"},
  });
}

TEST(CssGrammarTypes, AnUnicodeRangeIsTheTokenOfAUnicodeRangeValue) {
  Grammar g;
  compile_into(g, "<urange>#");
  ASSERT_TRUE(g.ok());
  Matched m;
  match_declaration(m, g, "unicode-range: U+26, u+4??, U+0-7F");
  ASSERT_EQ(m.status, GTEXT_CSS_OK);
  EXPECT_EQ(m.dump(), "list/comma(ur:26-26 ur:400-4FF ur:0-7F)");
  EXPECT_EQ(m.text(), "U+26, U+400-4FF, U+0-7F");
  Matched n;
  match_declaration(n, g, "x: U+26, foo");
  EXPECT_EQ(n.dump(), "no match");
}

TEST(CssGrammarTypes, AnPlusBIsReadFromAsManyComponentValuesAsItTakes) {
  check_rows({
      {"<an+b>", "2n+1", "unparsed:2n+1"},
      {"<an+b>", "odd", "unparsed:2n+1"},
      {"<an+b>", "even", "unparsed:2n"},
      {"<an+b>", "-n + 3", "unparsed:-n+3"},
      {"<an+b>", "+ n", "no match"},
      {"<an+b>", "5", "unparsed:5"},
      {"<an+b>", "n- 2", "unparsed:n-2"},
      {"<an+b>", "foo", "no match"},
      {"<an+b> of a", "2n+1 of a", "list/space(unparsed:2n+1 kw:of kw:a)"},
      {"<an+b> of a", "n of a", "list/space(unparsed:n kw:of kw:a)"},
  });
}

TEST(CssGrammarTypes, TheTokenTypesMatchTokensAndWhatTheyAreInsideOf) {
  check_rows({
      {"<ident-token>", "foo", "tok:foo"},
      {"<function-token>", "foo(1)", "tok:foo(1)"},
      {"<at-keyword-token>", "@foo", "tok:@foo"},
      {"<hash-token>", "#abc", "tok:#abc"},
      {"<string-token>", "'x'", "tok:\"x\""},
      {"<bad-string-token>", "'x\n", "tok:\"\n"},
      {"<url-token>", "url(x)", "tok:url(x)"},
      {"<delim-token>", "+", "tok:+"},
      {"<number-token>", "3", "tok:3"},
      {"<percentage-token>", "3%", "tok:3%"},
      {"<dimension-token>", "3px", "tok:3px"},
      {"<colon-token>", ":", "tok::"},
      {"<semicolon-token>", ";", "tok:;"},
      {"<comma-token>", ",", "tok:,"},
      {"<(-token>", "(a)", "tok:(a)"},
      {"<[-token>", "[a]", "tok:[a]"},
      {"<{-token>", "{a}", "tok:{a}"},
      {"<)-token>", ")", "tok:)"},
      {"<]-token>", "]", "tok:]"},
      {"<}-token>", "}", "tok:}"},
      {"<CDO-token>", "<!--", "tok:<!--"},
      {"<CDC-token>", "-->", "tok:-->"},
      {"<ident-token>", "3", "no match"},
      {"<delim-token>", "foo", "no match"},
  });
}

TEST(CssGrammarTypes, AWhitespaceTokenIsAWhereNotAWhat) {
  check_rows({
      {"a <whitespace-token> b", "a b", "list/space(kw:a tok:  kw:b)"},
      {"a <whitespace-token> b", "a/**/b", "no match"},
  });
}

TEST(CssGrammarTypes, ARangeIsInclusiveAndCountsUnits) {
  check_rows({
      {"<number [0,1]>", "0", "num:0"},
      {"<number [0,1]>", "1", "num:1"},
      {"<number [0,1]>", "1.0000001", "no match"},
      {"<number [0,1]>", "-0.0000001", "no match"},
      {"<number [-1,∞]>", "-1", "num:-1"},
      {"<number [-1,∞]>", "-2", "no match"},
      {"<number [-∞,0]>", "5", "no match"},
      {"<number [-∞,0]>", "-5", "num:-5"},
      {"<number [0,infinity]>", "1e300", "num:1e+300"},
      {"<integer [1,∞]>", "1", "int:1"},
      {"<integer [1,∞]>", "0", "no match"},
      {"<percentage [0%,100%]>", "100%", "pct:100"},
      {"<percentage [0,100]>", "100.5%", "no match"},
      {"<length [0,∞]>", "-1px", "no match"},
      {"<length [0,∞]>", "0px", "dim:0px"},
      {"<length [0in,1in]>", "96px", "dim:96px"},
      {"<length [0in,1in]>", "2.54cm", "dim:2.54cm"},
      {"<length [0in,1in]>", "96.0001px", "no match"},
      {"<length [0in,1in]>", "1em", "dim:1em"},
      {"<length [0px,∞]>", "-1em", "no match"},
      {"<length [0px,∞]>", "0em", "dim:0em"},
      {"<length [1px,∞]>", "0em", "no match"},
      {"<length [-10px,-1px]>", "1em", "no match"},
      {"<length [-10px,-1px]>", "-1vw", "dim:-1vw"},
      {"<length [1px,∞]>", "0", "no match"},
      {"<length [0px,∞]>", "0", "dim:0px"},
      {"<angle [-90deg,90deg]>", "0.25turn", "dim:0.25turn"},
      {"<angle [-90deg,90deg]>", "1.6rad", "no match"},
      {"<time [0s,∞]>", "-1ms", "no match"},
      {"<time [0s,∞]>", "1ms", "dim:1ms"},
  });
}

TEST(CssGrammarTypes, ARangeOnACompositeTypeAppliesToEveryNumericTypeInIt) {
  Defs d;
  d.types["length-percentage"] = "<length> | <percentage>";
  d.types["both"] = "<length-percentage> | auto";
  check_rows({
      {"<length-percentage [0,∞]>", "-1px", "no match"},
      {"<length-percentage [0,∞]>", "-1%", "no match"},
      {"<length-percentage [0,∞]>", "1%", "pct:1"},
      {"<length-percentage [0,∞]>", "1px", "dim:1px"},
      {"<both [0,10]>", "11%", "no match"},
      {"<both [0,10]>", "auto", "kw:auto"},
      {"<length-percentage>", "-1px", "dim:-1px"},
  }, &d);
}

TEST(CssGrammarTypes, ARangeAlreadyThereIsNarrowedByTheOneThatIsAdded) {
  Defs d;
  d.types["small"] = "<number [0,5]>";
  check_rows({
      {"<small [2,9]>", "1", "no match"},
      {"<small [2,9]>", "3", "num:3"},
      {"<small [2,9]>", "6", "no match"},
      {"<small>", "0", "num:0"},
  }, &d);
}

TEST(CssGrammarTypes, AMathFunctionMatchesANumericTypeByNameAndRecordsTheRange) {
  Grammar g;
  compile_into(g, "<length [0px,10px]>");
  ASSERT_TRUE(g.ok());
  Matched m;
  match_into(m, g, "calc(100px - 3px)");
  ASSERT_TRUE(m.matched);
  const GTEXT_CSS_Value * v = m.root();
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->kind, GTEXT_CSS_VAL_FUNCTION);
  EXPECT_NE(v->flags & GTEXT_CSS_VFLAG_MATH, 0u);
  EXPECT_EQ(v->range_lo, 0.0);
  EXPECT_EQ(v->range_hi, 10.0);
  EXPECT_EQ(m.text(), "calc(97px)");
  EXPECT_NE(v->math, nullptr);
  // every math function is checked against the type it is asked to stand for (the math layer's type system, Values 4, 10.9)
  for (const char * fn : {"min(1, 2)", "max(1)", "clamp(1, 2, 3)", "round(1.4)", "mod(1, 1)", "rem(1, 1)", "sin(1)", "cos(1)",
           "tan(1)", "sin(1deg)", "pow(1, 2)", "sqrt(1)", "hypot(1)", "log(1)", "exp(1)", "abs(1)", "sign(1px)"}) {
    EXPECT_NE(run("<number>", fn), "no match") << fn;
  }
  for (const char * fn : {"asin(1)", "acos(1)", "atan(1)", "atan2(1px, 1px)", "min(1deg, 2deg)"}) {
    EXPECT_NE(run("<angle>", fn), "no match") << fn;
    EXPECT_EQ(run("<number>", fn), "no match") << fn;
  }
  EXPECT_EQ(run("<length>", "calc(1px + 2s)"), "no match");
  EXPECT_EQ(run("<number>", "sin(1px)"), "no match");
  EXPECT_EQ(run("<number>", "foo(1)"), "no match");
  EXPECT_EQ(run("<string>", "calc(1)"), "no match");
  EXPECT_EQ(run("<ident>", "calc(1)"), "no match");
  EXPECT_NE(run("<integer>", "calc(1 + 1)"), "no match");
  EXPECT_NE(run("<percentage>", "calc(1% + 1%)"), "no match");
  EXPECT_NE(run("<angle>", "calc(1deg)"), "no match");
  EXPECT_EQ(run("<angle>", "calc(1px)"), "no match");
  EXPECT_EQ(run("<dimension>", "calc(1px)"), "no match");
}

TEST(CssGrammarTypes, ABareNumberIsALengthOnlyUnderTheQuirksOption) {
  Grammar g;
  compile_into(g, "<length>");
  ASSERT_TRUE(g.ok());
  Matched plain;
  match_into(plain, g, "5");
  EXPECT_EQ(plain.dump(), "no match");
  GTEXT_CSS_Match_Options o = gtext_css_match_options_default();
  o.unitless_length = true;
  Matched quirk;
  match_into(quirk, g, "5", &o);
  ASSERT_TRUE(quirk.matched);
  EXPECT_EQ(quirk.dump(), "dim:5px");
  EXPECT_NE(quirk.root()->flags & GTEXT_CSS_VFLAG_UNITLESS_LENGTH, 0u);
  Matched zero;
  match_into(zero, g, "0", &o);
  EXPECT_EQ(zero.dump(), "dim:0px");
  EXPECT_NE(zero.root()->flags & GTEXT_CSS_VFLAG_ZERO_LENGTH, 0u);
  EXPECT_EQ(zero.root()->flags & GTEXT_CSS_VFLAG_UNITLESS_LENGTH, 0u);
  Matched angle;
  Grammar a;
  compile_into(a, "<angle>");
  match_into(angle, a, "5", &o);
  EXPECT_EQ(angle.dump(), "no match");
}

TEST(CssGrammarTypes, EmptyInputMatchesOnlyWhatCanBeEmpty) {
  check_rows({
      {"a?", "", "<none>"},
      {"a*", "", "<none>"},
      {"a", "", "no match"},
      {"a+", "", "no match"},
      {"a? b?", "", "<none>"},
      {"[a | b?]", "", "<none>"},
      {"<declaration-list>", "", "unparsed:"},
  });
}

TEST(CssGrammarTypes, ComponentValueIdsAndSpansComeFromTheInput) {
  Grammar g;
  compile_into(g, "a <length> b?");
  ASSERT_TRUE(g.ok());
  Matched m;
  match_into(m, g, "  a   12px  ");
  ASSERT_TRUE(m.matched);
  const GTEXT_CSS_Value * l = m.root();
  ASSERT_EQ(l->kind, GTEXT_CSS_VAL_LIST);
  ASSERT_EQ(l->child_count, 2u);
  EXPECT_EQ(l->children[0].span.offset, 2u);
  EXPECT_EQ(l->children[1].span.offset, 6u);
  EXPECT_EQ(l->children[1].span.end, 10u);
  EXPECT_EQ(l->span.offset, 2u);
  EXPECT_EQ(l->span.end, 10u);
  EXPECT_NE(l->children[0].component, 0u);
  EXPECT_NE(l->children[0].component, l->children[1].component);
  EXPECT_EQ(l->component, 0u);
}

TEST(CssGrammarTypes, EachValueNamesTheGrammarSlotThatMatchedIt) {
  Grammar g;
  compile_into(g, "[ <length-percentage [0,∞]> | auto ]{1,2}", &standard_defs());
  ASSERT_TRUE(g.ok());
  Matched m;
  match_into(m, g, "5% auto");
  ASSERT_TRUE(m.matched);
  const GTEXT_CSS_Value * l = m.root();
  ASSERT_EQ(l->child_count, 2u);
  size_t len = 0;
  const char * t = gtext_css_compiled_slot_text(g.c, l->children[1].slot, &len);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(std::string(t, len), "auto");
  t = gtext_css_compiled_slot_text(g.c, l->children[0].slot, &len);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(std::string(t, len), "<length-percentage [0,∞]>"); // a native type since C1b
  t = gtext_css_compiled_slot_text(g.c, l->slot, &len);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(std::string(t, len), "[ <length-percentage [0,∞]> | auto ]{1,2}");
  EXPECT_EQ(gtext_css_compiled_slot_text(g.c, (uint32_t)gtext_css_compiled_node_count(g.c), &len), nullptr);
}

TEST(CssGrammarTypes, EachKeywordTheGrammarNamesHasAnAtom) {
  Grammar g;
  compile_into(g, "auto | Normal | none");
  ASSERT_TRUE(g.ok());
  const GTEXT_CSS_Atom_Table * t = gtext_css_compiled_atoms(g.c);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(gtext_css_atom_count(t), 3u);
  const GTEXT_CSS_Atom normal = gtext_css_atom_lookup(t, "NORMAL", 6);
  ASSERT_NE(normal, GTEXT_CSS_ATOM_NONE);
  Matched m;
  match_into(m, g, "normal");
  ASSERT_TRUE(m.matched);
  EXPECT_EQ(m.root()->atom, normal);
  size_t len = 0;
  EXPECT_STREQ(gtext_css_atom_name(t, normal, &len), "Normal");
  EXPECT_EQ(len, 6u);
  EXPECT_EQ(gtext_css_atom_lookup(t, "other", 5), GTEXT_CSS_ATOM_NONE);
}

TEST(CssGrammarResolve, EachDistinctReferenceIsAskedForOnce) {
  Defs d;
  d.types["a"] = "<b> <b> <b>";
  d.types["b"] = "x | y";
  d.props["w"] = "<a>";
  Grammar g;
  compile_into(g, "<a> <'w'> <a> <b>", &d);
  ASSERT_TRUE(g.ok());
  EXPECT_EQ(d.asked, (std::vector<std::string>{"a", "'w", "b"}));
}

TEST(CssGrammarResolve, ADistinctRangeIsADistinctReference) {
  Defs d;
  d.types["t"] = "<number>";
  Grammar g;
  compile_into(g, "<t> <t [0,1]> <t [0,1]> <t [2,3]>", &d);
  ASSERT_TRUE(g.ok());
  EXPECT_EQ(d.asked.size(), 3u);
}

TEST(CssGrammarResolve, ANativeTypeIsNeverAskedFor) {
  Defs d;
  Grammar g;
  compile_into(g, "<length> <number [0,1]> <ident>", &d);
  ASSERT_TRUE(g.ok());
  EXPECT_TRUE(d.asked.empty());
  // a type of the resolver that has a native name is not consulted either
  d.types["length"] = "never";
  Grammar h;
  compile_into(h, "<length>", &d);
  ASSERT_TRUE(h.ok());
  EXPECT_TRUE(d.asked.empty());
  Matched m;
  match_into(m, h, "5px");
  EXPECT_EQ(m.dump(), "dim:5px");
}

TEST(CssGrammarResolve, TypesAndPropertiesAreSeparateNamespaces) {
  Defs d;
  d.types["w"] = "type";
  d.props["w"] = "property";
  EXPECT_EQ(run("<w>", "type", &d), "kw:type");
  EXPECT_EQ(run("<'w'>", "property", &d), "kw:property");
  EXPECT_EQ(run("<w>", "property", &d), "no match");
}

TEST(CssGrammarResolve, AFunctionTypeHasParenthesesInItsName) {
  Defs d;
  d.types["rgb()"] = "rgb( <number>#{3} )";
  EXPECT_EQ(run("<rgb()> | none", "rgb(1, 2, 3)", &d), "fn:rgb(list/comma(num:1 num:2 num:3))");
  EXPECT_EQ(d.asked, (std::vector<std::string>{"rgb()"}));
}

TEST(CssGrammarResolve, RightRecursionIsFineAndMatchesDeepInput) {
  Defs d;
  d.types["x"] = "a <x>?";
  Grammar g;
  compile_into(g, "<x>", &d);
  ASSERT_TRUE(g.ok());
  std::string in;
  for (int i = 0; i < 3000; i++) {
    in += "a ";
  }
  Matched m;
  match_into(m, g, in);
  EXPECT_TRUE(m.matched);
  Matched n;
  match_into(n, g, in + "b");
  EXPECT_EQ(n.status, GTEXT_CSS_OK);
  EXPECT_FALSE(n.matched);
}

TEST(CssGrammarResolve, RecursionThroughAFunctionConsumesInputSoItIsNotLeft) {
  Defs d;
  d.types["x"] = "f( <x> ) | a";
  EXPECT_EQ(run("<x>", "f(f(a))", &d), "fn:f(fn:f(kw:a))");
  EXPECT_EQ(run("<x>", "f(f())", &d), "no match");
}

TEST(CssGrammarLink, LeftRecursionIsRefusedInEveryShapeItTakes) {
  struct Case {
    const char * name;
    std::map<std::string, std::string> types;
    const char * root;
    const char * named;
  };
  const std::vector<Case> cases = {
      {"direct", {{"x", "<x> | a"}}, "<x>", "<x>"},
      {"a prefix that can be empty", {{"x", "a? <x> | b"}}, "<x>", "<x>"},
      {"through a second type", {{"x", "<y> a"}, {"y", "<x> | b"}}, "<x>", "<x>"},
      {"through a group", {{"x", "[ <x> a ]? b?"}}, "<x>", "<x>"},
      {"through a multiplier", {{"x", "<x>* a"}}, "<x>", "<x>"},
      {"through && and ||", {{"x", "a && <x>"}}, "<x>", "<x>"},
      {"through a chain of aliases", {{"x", "<y>"}, {"y", "<x>"}}, "<x>", ""},
      {"from a root that is not the cycle", {{"x", "a | <y>"}, {"y", "b <y2>"}, {"y2", "<y2>"}}, "<x>", "<y2>"},
      {"a type of its own before the cycle", {{"x", "a <y>"}, {"y", "<z>"}, {"z", "<y> | b"}}, "<x>", "<y>"},
  };
  for (const Case & c : cases) {
    Defs d;
    d.types = c.types;
    Grammar g;
    compile_into(g, c.root, &d);
    EXPECT_EQ(g.status, GTEXT_CSS_E_LEFT_RECURSION) << c.name;
    if (*c.named) {
      EXPECT_STREQ(g.result.name, c.named) << c.name;
    }
    EXPECT_EQ(g.c, nullptr) << c.name;
  }
}

TEST(CssGrammarLink, RecursionAfterSomethingThatConsumesIsNotLeftRecursion) {
  Defs d;
  d.types["x"] = "a <x> | b";
  d.types["y"] = "[ a | b ] <y>?";
  d.types["z"] = "a+ <z>?";
  EXPECT_EQ(run("<x>", "a a b", &d), "list/space(kw:a list/space(kw:a kw:b))");
  EXPECT_EQ(run("<y>", "a b a", &d), "list/space(kw:a list/space(kw:b list/space(kw:a)))");
  EXPECT_NE(run("<z>", "a a", &d), "no match");
}

TEST(CssGrammarLink, AnUnresolvedReferenceNamesItAndNothingIsLeaked) {
  Defs d;
  d.types["a"] = "<b>";
  Grammar g;
  compile_into(g, "x <a>", &d);
  EXPECT_EQ(g.status, GTEXT_CSS_E_REFERENCE);
  EXPECT_STREQ(g.result.name, "<b>");
  EXPECT_EQ(g.c, nullptr);
  Grammar h;
  compile_into(h, "<a>");  // no resolver at all
  EXPECT_EQ(h.status, GTEXT_CSS_E_REFERENCE);
  EXPECT_STREQ(h.result.name, "<a>");
}

TEST(CssGrammarLink, ARangeOnATypeThatIsNotNumericIsRefused) {
  Grammar g;
  compile_into(g, "<string [0,1]>");
  EXPECT_EQ(g.status, GTEXT_CSS_E_GRAMMAR);
  EXPECT_STREQ(g.result.name, "<string>");
}

TEST(CssGrammarLink, TheCompiledGrammarDoesNotDependOnTheDefinitionOrTheResolver) {
  GTEXT_CSS_Compiled * c = nullptr;
  {
    Defs d;
    d.types["t"] = "x | y";
    GTEXT_CSS_Definition * def = nullptr;
    const std::string text = "<t> z";
    ASSERT_EQ(gtext_css_definition_parse(text.data(), text.size(), nullptr, &def, nullptr), GTEXT_CSS_OK);
    EXPECT_EQ(gtext_css_definition_node_count(def), 3u);
    ASSERT_EQ(gtext_css_definition_link(def, Defs::resolve, &d, nullptr, &c, nullptr), GTEXT_CSS_OK);
    gtext_css_definition_free(def);
  }
  Grammar g;
  Matched m;
  gv::match_values(m, [&]() -> const Grammar & { g.c = c; g.status = GTEXT_CSS_OK; return g; }(), nullptr, 0);
  EXPECT_EQ(m.status, GTEXT_CSS_OK);
  EXPECT_FALSE(m.matched);
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_component_values, "y z");
  Matched m2;
  match_values(m2, g, p.tree->values, p.tree->value_count);
  EXPECT_EQ(m2.dump(), "list/space(kw:y kw:z)");
}

TEST(CssGrammarSyntax, ATextThatIsNotADefinitionIsRefusedWhereItGoesWrong) {
  struct Bad {
    const char * text;
    size_t offset;
  };
  const std::vector<Bad> bad = {
      {"", 0}, {"   ", 0}, {"a |", 3}, {"| a", 1}, {"a || ", 5},
      {"a && ", 5}, {"a &", 2}, {"a ] b", 2}, {"[ a", 3}, {"[ a ] ]", 6},
      {"( a", 3}, {"foo( a", 6}, {"foo( a ]", 7}, {"[ ]", 2}, {"<>", 1},
      {"<a", 2}, {"<'a>", 3}, {"<a [0,1>", 7}, {"<a [0 1]>", 6}, {"<a [1,0]>", 7},
      {"<a [0,1]", 8}, {"<number [0,1foo]>", 11}, {"<number [0,em]>", 11}, {"<number [0,1em]>", 11}, {"a{", 2},
      {"a{2", 3}, {"a{2,1}", 5}, {"a{0}", 3}, {"a{70000}", 6}, {"a{1,70000}", 8},
      {"a{,2}", 3}, {"a!", 1}, {"a ?", 2}, {"%", 0}, {"a | | b", 5},
      {"1", 0}, {"'ab'", 1}, {"''", 1}, {"'a", 1}, {"{ a", 3},
      {"}", 0}, {")", 0}, {"a#{", 3},
  };
  for (const Bad & b : bad) {
    GTEXT_CSS_Definition * d = nullptr;
    GTEXT_CSS_Grammar_Result r{};
    const GTEXT_CSS_Status st = gtext_css_definition_parse(b.text, strlen(b.text), nullptr, &d, &r);
    EXPECT_EQ(st, GTEXT_CSS_E_GRAMMAR) << "text: " << b.text;
    EXPECT_EQ(d, nullptr) << b.text;
    EXPECT_EQ(r.status, GTEXT_CSS_E_GRAMMAR) << b.text;
    EXPECT_STREQ(r.message, gtext_css_status_string(GTEXT_CSS_E_GRAMMAR));
    EXPECT_EQ(r.offset, b.offset) << "text: " << b.text;
  }
}

TEST(CssGrammarSyntax, WhitespaceBetweenTheNotationsIsFreeAndTheMultipliersAreAdjacent) {
  check_rows({
      {"  [  a  |  b  ]  {  1 , 2  } ", "", "compile:not a Value Definition Syntax definition "},
      {"[a|b]{1,2}", "a b", "list/space(kw:a kw:b)"},
      {"[ a | b ]{1,2}", "a", "list/space(kw:a)"},
      {"a\tb\n|\rc", "c", "kw:c"},
  });
}

TEST(CssGrammarLimits, TheDefinitionTextIsLimitedInBytes) {
  GTEXT_CSS_Grammar_Options o = gtext_css_grammar_options_default();
  o.max_definition_bytes = 11;
  const std::string at_bound = "a | b | c d";  // 11 bytes
  GTEXT_CSS_Definition * d = nullptr;
  GTEXT_CSS_Grammar_Result r{};
  ASSERT_EQ(at_bound.size(), 11u);
  EXPECT_EQ(gtext_css_definition_parse(at_bound.data(), at_bound.size(), &o, &d, &r), GTEXT_CSS_OK);
  gtext_css_definition_free(d);
  d = nullptr;
  const std::string past = at_bound + " ";
  EXPECT_EQ(gtext_css_definition_parse(past.data(), past.size(), &o, &d, &r), GTEXT_CSS_E_LIMIT_DEFINITION);
  EXPECT_EQ(d, nullptr);
  EXPECT_EQ(r.offset, 11u);
  GTEXT_CSS_Grammar_Options def = gtext_css_grammar_options_default();
  EXPECT_EQ(def.max_definition_bytes, 65536u);
  const std::string big(65537, 'a');
  EXPECT_EQ(gtext_css_definition_parse(big.data(), big.size(), nullptr, &d, &r), GTEXT_CSS_E_LIMIT_DEFINITION);
}

TEST(CssGrammarLimits, GroupsAreLimitedInDepth) {
  GTEXT_CSS_Grammar_Options o = gtext_css_grammar_options_default();
  o.max_definition_depth = 5;
  auto nest = [](int n) { return std::string(n, '[') + "a" + std::string(n, ']'); };
  GTEXT_CSS_Definition * d = nullptr;
  GTEXT_CSS_Grammar_Result r{};
  std::string t = nest(5);
  EXPECT_EQ(gtext_css_definition_parse(t.data(), t.size(), &o, &d, &r), GTEXT_CSS_OK);
  gtext_css_definition_free(d);
  d = nullptr;
  t = nest(6);
  EXPECT_EQ(gtext_css_definition_parse(t.data(), t.size(), &o, &d, &r), GTEXT_CSS_E_LIMIT_DEFINITION_DEPTH);
  EXPECT_EQ(r.offset, 5u);
  // functions, blocks and parentheses nest the same way
  t = "f(f(f(f(f(a)))))";
  EXPECT_EQ(gtext_css_definition_parse(t.data(), t.size(), &o, &d, &r), GTEXT_CSS_OK);
  gtext_css_definition_free(d);
  d = nullptr;
  t = "f(f(f(f(f(f(a))))))";
  EXPECT_EQ(gtext_css_definition_parse(t.data(), t.size(), &o, &d, &r), GTEXT_CSS_E_LIMIT_DEFINITION_DEPTH);
  // the default is deep enough to be a real bound and shallow enough to need no recursion: 256
  GTEXT_CSS_Grammar_Options def = gtext_css_grammar_options_default();
  EXPECT_EQ(def.max_definition_depth, 256u);
  t = nest(256);
  EXPECT_EQ(gtext_css_definition_parse(t.data(), t.size(), nullptr, &d, &r), GTEXT_CSS_OK);
  gtext_css_definition_free(d);
  d = nullptr;
  t = nest(257);
  EXPECT_EQ(gtext_css_definition_parse(t.data(), t.size(), nullptr, &d, &r), GTEXT_CSS_E_LIMIT_DEFINITION_DEPTH);
}

TEST(CssGrammarLimits, NodesAreLimitedInDefinitionsAndInTheLinkedWhole) {
  GTEXT_CSS_Grammar_Options o = gtext_css_grammar_options_default();
  GTEXT_CSS_Definition * d = nullptr;
  GTEXT_CSS_Grammar_Result r{};
  const std::string t = "a b c d";  // four keywords and the juxtaposition
  ASSERT_EQ(gtext_css_definition_parse(t.data(), t.size(), nullptr, &d, &r), GTEXT_CSS_OK);
  EXPECT_EQ(r.nodes, 5u);
  gtext_css_definition_free(d);
  o.max_definition_nodes = 5;
  d = nullptr;
  EXPECT_EQ(gtext_css_definition_parse(t.data(), t.size(), &o, &d, &r), GTEXT_CSS_OK);
  gtext_css_definition_free(d);
  o.max_definition_nodes = 4;
  d = nullptr;
  EXPECT_EQ(gtext_css_definition_parse(t.data(), t.size(), &o, &d, &r), GTEXT_CSS_E_LIMIT_DEFINITION_NODES);
  EXPECT_EQ(d, nullptr);
  // the linked whole counts every definition it holds: "<t> <t [0,1]>" is 3 nodes and two copies of "<number>"
  Defs defs;
  defs.types["t"] = "<number>";
  GTEXT_CSS_Grammar_Options big = gtext_css_grammar_options_default();
  Grammar g;
  compile_into(g, "<t> <t [0,1]>", &defs, &big);
  ASSERT_TRUE(g.ok());
  const size_t whole = g.result.nodes;
  EXPECT_EQ(whole, gtext_css_compiled_node_count(g.c));
  EXPECT_GT(whole, 3u);
  big.max_definition_nodes = whole;
  Grammar g2;
  compile_into(g2, "<t> <t [0,1]>", &defs, &big);
  EXPECT_TRUE(g2.ok());
  big.max_definition_nodes = whole - 1;
  Grammar g3;
  compile_into(g3, "<t> <t [0,1]>", &defs, &big);
  // the text itself has three nodes; the cut falls on the linked whole
  EXPECT_EQ(g3.status, GTEXT_CSS_E_LIMIT_DEFINITION_NODES);
}

TEST(CssGrammarLimits, ABarGroupMayBeLongButAnAndOrOrGroupHasAtMostSixtyFourMembers) {
  auto join = [](int n, const char * op) {
    std::string s = "k0";
    for (int i = 1; i < n; i++) {
      s += std::string(" ") + op + " k" + std::to_string(i);
    }
    return s;
  };
  for (const char * op : {"&&", "||"}) {
    GTEXT_CSS_Definition * d = nullptr;
    GTEXT_CSS_Grammar_Result r{};
    std::string t = join(64, op);
    EXPECT_EQ(gtext_css_definition_parse(t.data(), t.size(), nullptr, &d, &r), GTEXT_CSS_OK) << op;
    gtext_css_definition_free(d);
    d = nullptr;
    t = join(65, op);
    EXPECT_EQ(gtext_css_definition_parse(t.data(), t.size(), nullptr, &d, &r), GTEXT_CSS_E_LIMIT_GROUP) << op;
    EXPECT_EQ(d, nullptr);
  }
  GTEXT_CSS_Definition * d = nullptr;
  const std::string t = join(300, "|");
  EXPECT_EQ(gtext_css_definition_parse(t.data(), t.size(), nullptr, &d, nullptr), GTEXT_CSS_OK);
  gtext_css_definition_free(d);
  // and a group of exactly 64 members works, in any order
  Grammar g;
  compile_into(g, join(64, "&&"));
  ASSERT_TRUE(g.ok());
  std::string in;
  for (int i = 63; i >= 0; i--) {
    in += "k" + std::to_string(i) + " ";
  }
  Matched m;
  match_into(m, g, in);
  EXPECT_TRUE(m.matched);
  Matched n;
  match_into(n, g, in.substr(0, in.size() - 4));
  EXPECT_FALSE(n.matched);
}

TEST(CssGrammarLimits, MatchWorkIsBoundedAndTheBoundIsExact) {
  Grammar g;
  compile_into(g, "[ a | b ]* c");
  ASSERT_TRUE(g.ok());
  std::string in;
  for (int i = 0; i < 50; i++) {
    in += (i % 3 ? "a " : "b ");
  }
  in += "c";
  Matched probe;
  match_into(probe, g, in);
  ASSERT_TRUE(probe.matched);
  const size_t work = probe.result.work;
  ASSERT_GT(work, 100u);
  GTEXT_CSS_Match_Options o = gtext_css_match_options_default();
  o.max_match_work = work;
  Matched at;
  match_into(at, g, in, &o);
  EXPECT_EQ(at.status, GTEXT_CSS_OK);
  EXPECT_TRUE(at.matched);
  EXPECT_EQ(at.result.work, work);
  o.max_match_work = work - 1;
  Matched past;
  match_into(past, g, in, &o);
  EXPECT_EQ(past.status, GTEXT_CSS_E_LIMIT_WORK);
  EXPECT_FALSE(past.matched);
  EXPECT_EQ(past.set, nullptr);
  EXPECT_EQ(past.result.status, GTEXT_CSS_E_LIMIT_WORK);
}

TEST(CssGrammarLimits, MemoEntriesAreBoundedAndTheBoundIsExact) {
  Grammar g;
  compile_into(g, "[ a | b ]* c");
  ASSERT_TRUE(g.ok());
  std::string in;
  for (int i = 0; i < 50; i++) {
    in += (i % 3 ? "a " : "b ");
  }
  in += "c";
  Matched probe;
  match_into(probe, g, in);
  ASSERT_TRUE(probe.matched);
  const size_t memo = probe.result.memo_entries;
  ASSERT_GT(memo, 4u);
  GTEXT_CSS_Match_Options o = gtext_css_match_options_default();
  o.max_memo_entries = memo;
  Matched at;
  match_into(at, g, in, &o);
  EXPECT_EQ(at.status, GTEXT_CSS_OK);
  EXPECT_TRUE(at.matched);
  o.max_memo_entries = memo - 1;
  Matched past;
  match_into(past, g, in, &o);
  EXPECT_EQ(past.status, GTEXT_CSS_E_LIMIT_WORK);
  EXPECT_FALSE(past.matched);
}

TEST(CssGrammarLimits, AnExhaustedBoundIsNeverNoMatch) {
  Grammar g;
  compile_into(g, "[ [ a? b? ]* ]+");
  ASSERT_TRUE(g.ok());
  std::string in;
  for (int i = 0; i < 20000; i++) {
    in += "a ";
  }
  in += "c";  // does not match: the whole input must be consumed
  GTEXT_CSS_Match_Options o = gtext_css_match_options_default();
  o.max_match_work = 1000;
  Matched m;
  match_into(m, g, in, &o);
  EXPECT_EQ(m.status, GTEXT_CSS_E_LIMIT_WORK);
  EXPECT_FALSE(m.matched);
  o.max_match_work = 0;
  Matched n;
  match_into(n, g, in, &o);
  EXPECT_EQ(n.status, GTEXT_CSS_OK);
  EXPECT_FALSE(n.matched);
}

TEST(CssGrammarWork, ARepetitionOfARepetitionIsLinearInTheInput) {
  Grammar g;
  compile_into(g, "[ [ a? b? ]* ]+");
  ASSERT_TRUE(g.ok());
  size_t last = 0;
  for (int n : {5000, 20000}) {
    std::string in;
    for (int i = 0; i < n; i++) {
      in += "a ";
    }
    Matched m;
    match_into(m, g, in);
    ASSERT_EQ(m.status, GTEXT_CSS_OK) << n;
    EXPECT_TRUE(m.matched) << n;
    if (last) {
      EXPECT_LE(m.result.work, last * 5) << "4x the input must cost about 4x the work";
    }
    last = m.result.work;
  }
}

TEST(CssGrammarHook, TheGrammarHookOfSyntaxCanBeFilledByACompiledGrammar) {
  Grammar g;
  compile_into(g, "<length> | auto");
  ASSERT_TRUE(g.ok());
  GTEXT_CSS_Matcher mt;
  ASSERT_EQ(gtext_css_matcher_init(&mt, g.c, nullptr), GTEXT_CSS_OK);
  const GTEXT_CSS_Grammar hook = gtext_css_matcher_grammar(&mt);
  bool matched = false;
  GTEXT_CSS_Tree * tree = nullptr;
  ASSERT_EQ(gtext_css_parse_grammar("12px", 4, &hook, nullptr, &matched, &tree, nullptr), GTEXT_CSS_OK);
  gtext_css_tree_free(tree);
  EXPECT_TRUE(matched);
  EXPECT_EQ(dump(gtext_css_value_set_root(gtext_css_matcher_value_set(&mt, 0))), "dim:12px");
  tree = nullptr;
  ASSERT_EQ(gtext_css_parse_grammar("12", 2, &hook, nullptr, &matched, &tree, nullptr), GTEXT_CSS_OK);
  gtext_css_tree_free(tree);
  EXPECT_FALSE(matched);
  EXPECT_EQ(mt.status, GTEXT_CSS_OK);
  size_t groups = 0;
  tree = nullptr;
  size_t count = 0;
  ASSERT_EQ(gtext_css_parse_grammar_list("1px, auto, 2, 3em", 17, &hook, nullptr, &count, &tree, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(count, 3u);
  EXPECT_EQ(dump(gtext_css_value_set_root(gtext_css_matcher_value_set(&mt, 0))), "dim:1px");
  EXPECT_EQ(dump(gtext_css_value_set_root(gtext_css_matcher_value_set(&mt, 1))), "kw:auto");
  EXPECT_EQ(gtext_css_matcher_value_set(&mt, 2), nullptr);
  EXPECT_EQ(dump(gtext_css_value_set_root(gtext_css_matcher_value_set(&mt, 3))), "dim:3em");
  EXPECT_EQ(gtext_css_matcher_value_set(&mt, 100), nullptr);
  (void)groups;
  gtext_css_tree_free(tree);
  // a limit reached inside the hook answers false and leaves the status where the caller can read it
  GTEXT_CSS_Match_Options o = gtext_css_match_options_default();
  o.max_match_work = 1;
  GTEXT_CSS_Matcher low;
  ASSERT_EQ(gtext_css_matcher_init(&low, g.c, &o), GTEXT_CSS_OK);
  const GTEXT_CSS_Grammar lowhook = gtext_css_matcher_grammar(&low);
  tree = nullptr;
  ASSERT_EQ(gtext_css_parse_grammar("12px", 4, &lowhook, nullptr, &matched, &tree, nullptr), GTEXT_CSS_OK);
  gtext_css_tree_free(tree);
  EXPECT_FALSE(matched);
  EXPECT_EQ(low.status, GTEXT_CSS_E_LIMIT_WORK);
  gtext_css_matcher_free(&low);
  gtext_css_matcher_free(&mt);
  EXPECT_EQ(gtext_css_matcher_init(nullptr, g.c, nullptr), GTEXT_CSS_E_INVALID);
}

TEST(CssGrammarApi, BadArgumentsAreRefusedAndWriteNothing) {
  GTEXT_CSS_Definition * d = reinterpret_cast<GTEXT_CSS_Definition *>(0x1);
  GTEXT_CSS_Grammar_Result r{};
  EXPECT_EQ(gtext_css_definition_parse("a", 1, nullptr, nullptr, &r), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_definition_parse(nullptr, 1, nullptr, &d, &r), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(d, reinterpret_cast<GTEXT_CSS_Definition *>(0x1));
  EXPECT_EQ(r.status, GTEXT_CSS_E_INVALID);
  GTEXT_CSS_Compiled * c = reinterpret_cast<GTEXT_CSS_Compiled *>(0x1);
  EXPECT_EQ(gtext_css_definition_link(nullptr, nullptr, nullptr, nullptr, &c, &r), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(c, reinterpret_cast<GTEXT_CSS_Compiled *>(0x1));
  Grammar g;
  compile_into(g, "a");
  bool matched = true;
  EXPECT_EQ(gtext_css_match(nullptr, nullptr, 0, nullptr, &matched, nullptr, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_match(g.c, nullptr, 0, nullptr, nullptr, nullptr, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_match(g.c, nullptr, 3, nullptr, &matched, nullptr, nullptr), GTEXT_CSS_E_INVALID);
  gtext_css_definition_free(nullptr);
  gtext_css_compiled_free(nullptr);
  gtext_css_value_set_free(nullptr);
  EXPECT_EQ(gtext_css_compiled_node_count(nullptr), 0u);
  EXPECT_EQ(gtext_css_definition_node_count(nullptr), 0u);
  EXPECT_EQ(gtext_css_compiled_atoms(nullptr), nullptr);
  size_t len;
  EXPECT_EQ(gtext_css_compiled_slot_text(nullptr, 0, &len), nullptr);
  EXPECT_EQ(gtext_css_compiled_slot_text(g.c, 0, nullptr), nullptr);
  // the typed result is optional
  Matched m;
  css_test::parse_into(m.parsed, gtext_css_parse_component_values, "a");
  EXPECT_EQ(gtext_css_match(g.c, m.parsed.tree->values, m.parsed.tree->value_count, nullptr, &matched, nullptr, nullptr), GTEXT_CSS_OK);
  EXPECT_TRUE(matched);
}

} // namespace
