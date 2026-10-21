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
 * The CSS serializer: CSS Syntax 9 (the round trip, the table of token pairs
 * that need a comment, the U+005C delim, the escaping of units) and 9.1
 * (An+B), spelled by CSSOM's "Common Serializing Idioms" (identifiers and
 * strings, CSSOM Editor's Draft of 13 October 2026), and the sink, limit and
 * buffer contract of css_serialize.h.
 */

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_test_support.h"
#include "markup_test_support.h"

using namespace css_test;

namespace {

std::string ident(const std::string & s) {
  std::string out;
  GTEXT_Markup_Sink sink = {string_sink, &out};
  EXPECT_EQ(gtext_css_serialize_identifier(s.data(), s.size(), &sink), GTEXT_CSS_OK);
  return out;
}

std::string quoted(const std::string & s) {
  std::string out;
  GTEXT_Markup_Sink sink = {string_sink, &out};
  EXPECT_EQ(gtext_css_serialize_string(s.data(), s.size(), &sink), GTEXT_CSS_OK);
  return out;
}

std::string values_text(const std::string & css, GTEXT_CSS_Number_Style numbers = GTEXT_CSS_NUMBERS_SOURCE) {
  Parsed p;
  parse_into(p, gtext_css_parse_component_values, css);
  GTEXT_CSS_Serialize_Options o = gtext_css_serialize_options_default();
  o.numbers = numbers;
  return serialize(p.tree, &o);
}

// ---- CSSOM's idioms -----------------------------------------------------------------------------

TEST(CssSerializeIdentifier, FollowsCssomsRules) {
  EXPECT_EQ(ident("abc-_Z9"), "abc-_Z9");
  EXPECT_EQ(ident("0a"), "\\30 a") << "a first digit is escaped as a code point";
  EXPECT_EQ(ident("-0"), "-\\30 ") << "a digit second after a hyphen is escaped";
  EXPECT_EQ(ident("-"), "\\-") << "a lone hyphen is an escaped character";
  EXPECT_EQ(ident("--"), "--");
  EXPECT_EQ(ident("a b.c"), "a\\ b\\.c");
  EXPECT_EQ(ident("a\x01\x1f\x7f"), "a\\1 \\1f \\7f ");
  EXPECT_EQ(ident(std::string("a\0b", 3)), "a\xEF\xBF\xBD" "b") << "U+0000 is U+FFFD";
  EXPECT_EQ(ident("\xC3\xA9\xF0\x9F\x98\x80"), "\xC3\xA9\xF0\x9F\x98\x80") << "non-ASCII ident code points are themselves";
  EXPECT_EQ(ident("\xC2\x80"), "\\\xC2\x80") << "U+0080 is not an ident code point, so it is escaped";
  EXPECT_EQ(ident("\xFF"), "\xEF\xBF\xBD") << "ill-formed UTF-8 is U+FFFD";
}

TEST(CssSerializeString, FollowsCssomsRules) {
  EXPECT_EQ(quoted("a'b"), "\"a'b\"");
  EXPECT_EQ(quoted("a\"b\\c"), "\"a\\\"b\\\\c\"");
  EXPECT_EQ(quoted("a\nb\x7f"), "\"a\\a b\\7f \"");
  EXPECT_EQ(quoted(""), "\"\"");
}

TEST(CssSerializeIdioms, RefuseAnUnusableSink) {
  GTEXT_Markup_Sink none = {nullptr, nullptr};
  EXPECT_EQ(gtext_css_serialize_identifier("a", 1, &none), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_serialize_string("a", 1, nullptr), GTEXT_CSS_E_INVALID);
  std::string out;
  GTEXT_Markup_Sink sink = {string_sink, &out};
  EXPECT_EQ(gtext_css_serialize_identifier(nullptr, 1, &sink), GTEXT_CSS_E_INVALID);
}

// ---- section 9: tokens that would run together --------------------------------------------------

struct Tok {
  const char * name;
  const char * text;
};

// The table's rows and columns, each as text that is that one token.
const Tok kRows[] = {{"ident", "foo"}, {"at-keyword", "@foo"}, {"hash", "#foo"}, {"dimension", "123foo"}, {"#", "#"},
    {"-", "-"}, {"number", "123"}, {"@", "@"}, {".", "."}, {"+", "+"}, {"/", "/"}};
const Tok kColumns[] = {{"ident", "bar"}, {"function", "bar()"}, {"url", "url(bar)"}, {"bad url", "url(a b)"}, {"-", "-"},
    {"number", "123"}, {"percentage", "123%"}, {"dimension", "123em"}, {"CDC", "-->"}, {"(", "()"}, {"*", "*"}, {"%", "%"}};

// The table of the draft, served 2026-10-21: x where a comment is needed.
const char * const kTable[] = {
    // ident fn  url bad  -  num  %  dim CDC  (   *   %
    "xxxxxxxxxx..", // ident
    "xxxxxxxxx...", // at-keyword
    "xxxxxxxxx...", // hash
    "xxxxxxxxx...", // dimension
    "xxxxxxxxx...", // #
    "xxxxxxxxx...", // -
    "xxxx.xxxx..x", // number
    "xxxxx...x...", // @
    ".....xxx....", // .
    ".....xxx....", // +
    "..........x.", // /
};

TEST(CssSerializeTokens, APairOfTokensGetsACommentExactlyWhereTheTableSays) {
  for (size_t r = 0; r < sizeof kRows / sizeof kRows[0]; r++) {
    for (size_t c = 0; c < sizeof kColumns / sizeof kColumns[0]; c++) {
      const std::string in = std::string(kRows[r].text) + "/**/" + kColumns[c].text;
      Parsed p;
      parse_into(p, gtext_css_parse_component_values, in);
      ASSERT_EQ(p.tree->value_count, 2u) << in << " is not two tokens: " << text_of(p.tree->values, p.tree->value_count);
      const std::string out = serialize(p.tree);
      const bool comment = out.find("/**/") != std::string::npos;
      EXPECT_EQ(comment, kTable[r][c] == 'x') << kRows[r].name << " then " << kColumns[c].name << ": " << out;
      EXPECT_EQ(round_trip(p.tree), "") << in;
    }
  }
}

TEST(CssSerializeTokens, PairsTheTableDoesNotListAreSeparatedToo) {
  // `<` `!` `--` would be a CDO token; `u` `+` and a unicode-range token followed by what it takes
  // would read again as one unicode-range token in a unicode-range value.
  EXPECT_EQ(values_text("</**/!/**/--"), "</**/!--");
  Parsed p;
  parse_into(p, gtext_css_parse_declaration_list, "unicode-range: u/**/+1, U+1/**/2, U+1/**/?, U+1/**/-2, U+1/**/a");
  const std::string out = serialize(p.tree);
  EXPECT_EQ(out, "unicode-range:u/**/+1, U+1/**/2, U+1/**/?, U+1/**/-2, U+1/**/a;");
  EXPECT_EQ(round_trip(p.tree), "");
}

TEST(CssSerializeTokens, ABackslashDelimIsABackslashAndANewline) {
  EXPECT_EQ(values_text("a \\\n b"), "a \\\n b");
  Parsed p;
  parse_into(p, gtext_css_parse_declaration_list, "a: b \\\n");
  EXPECT_EQ(serialize(p.tree), "a:b \\\n;") << "the newline is written even where the white space after it was trimmed";
  EXPECT_EQ(round_trip(p.tree), "");
}

TEST(CssSerializeTokens, ABadStringIsAQuoteAndANewline) {
  Parsed p;
  parse_into(p, gtext_css_parse_declaration_list, "a: 'x\n; b: c");
  EXPECT_EQ(serialize(p.tree), "a:\"\n;b:c;");
  EXPECT_EQ(round_trip(p.tree), "");
}

TEST(CssSerializeTokens, AUrlIsWrittenUnquotedAndABadUrlStaysBad) {
  // CSSOM's serialize a URL would write url("..."), which reads back as a function.
  EXPECT_EQ(values_text("url(a\\)b\\ c\\\"\\'\\(\\\\d)"), "url(a\\)b\\20 c\\\"\\'\\(\\\\d)");
  EXPECT_EQ(values_text("url(a b)"), "url(()");
  // An escape can put a CR or an FF in a url, which written raw would be read back as a newline: white
  // space, and a bad url (a find of the fuzzer's).
  Parsed cr;
  parse_into(cr, gtext_css_parse_component_values, "url(a\\d b\\c c\\9 d)");
  EXPECT_EQ(serialize(cr.tree), "url(a\\d b\\c c\\9 d)");
  EXPECT_EQ(round_trip(cr.tree), "");
  EXPECT_EQ(values_text("url('a')"), "url(\"a\")") << "a function named url with a string in it";
}

TEST(CssSerializeTokens, AnUnrestrictedHashKeepsItsLeadingDigitAndAnIdHashIsAnIdentifier) {
  EXPECT_EQ(values_text("#0a #-0a #\\30 a #a\\ b"), "#0a #-0a #\\30 a #a\\ b");
  Parsed p;
  parse_into(p, gtext_css_parse_component_values, "#0a #\\30 a");
  EXPECT_EQ(round_trip(p.tree), "") << "the type flags survive";
}

TEST(CssSerializeTokens, ADimensionsUnitIsEscapedWhereItWouldReadAsAnExponent) {
  EXPECT_EQ(values_text("1\\65 5 1\\65-5 1e-x 1em 1\\-"), "1\\65 5 1\\65 -5 1e-x 1em 1\\-");
  Parsed p;
  parse_into(p, gtext_css_parse_component_values, "1\\65 5 1\\65-5 1E\\2b 5");
  EXPECT_EQ(round_trip(p.tree), "");
}

TEST(CssSerializeTokens, AFunctionKeepsTheCaseOfItsName) {
  EXPECT_EQ(values_text("RGB(1)"), "RGB(1)");
}

// ---- numbers ------------------------------------------------------------------------------------

TEST(CssSerializeNumbers, SourceNumbersAreWrittenAsTheyCame) {
  EXPECT_EQ(values_text("+1.50 .5e1 -0 1E3%"), "+1.50 .5e1 -0 1E3%");
}

TEST(CssSerializeNumbers, CssomNumbersAreBaseTenWithAtMostSixDecimals) {
  EXPECT_EQ(values_text("1.0 .1 +5 1e3 0.1234567 -0.0000001 -1.5 1e21 12.5%", GTEXT_CSS_NUMBERS_CSSOM),
      "1 0.1 5 1000 0.123457 0 -1.5 1000000000000000000000 12.5%");
  EXPECT_EQ(values_text("1.0px 2e1em", GTEXT_CSS_NUMBERS_CSSOM), "1px 20em");
}

// ---- rules and declarations ---------------------------------------------------------------------

TEST(CssSerializeRules, AStylesheetIsWrittenCompactlyWithEveryDeclarationEnded) {
  Parsed p;
  parse_into(p, gtext_css_parse_stylesheet, "a , b { c : d ; e { f : g } h : i !IMPORTANT } @m x { y {} z: w } @i 'u' ;");
  EXPECT_EQ(serialize(p.tree), "a , b {c:d;e {f:g;}h:i !important;}@m x {y {}z:w;}@i \"u\" ;");
}

TEST(CssSerializeRules, ANestedRuleThatWouldReadAsADeclarationGetsAFillerThatIsDropped) {
  // `b:` with a block is a rule here only because `d` came after it; written last in its block it
  // would read back as the declaration `b:{}`.
  Parsed p;
  parse_into(p, gtext_css_parse_stylesheet, "a{b:{c} d}");
  EXPECT_EQ(serialize(p.tree), "a{b:{}!}");
  EXPECT_EQ(round_trip(p.tree), "");
  Parsed q;
  parse_into(q, gtext_css_parse_stylesheet, "a{b:{c} d; e:f}");
  EXPECT_EQ(serialize(q.tree), "a{b:{}e:f;}") << "followed by something, it needs nothing";
  EXPECT_EQ(round_trip(q.tree), "");
}

TEST(CssSerializeRules, AUnicodeRangeDeclarationIsNotMarkedImportantTwice) {
  Parsed p;
  parse_into(p, gtext_css_parse_declaration_list, "unicode-range: U+0-7F !important; a: b !important");
  EXPECT_EQ(serialize(p.tree), "unicode-range:U+0-7F !important;a:b !important;");
  EXPECT_EQ(round_trip(p.tree), "");
}

TEST(CssSerializeRules, ACommaListWithAnEmptyLastGroupEndsWithItsComma) {
  for (const char * in : {"a,,", ",", "a, b", ",a", "", "a,"}) {
    Parsed p;
    parse_into(p, gtext_css_parse_comma_list, in);
    EXPECT_EQ(round_trip(p.tree), "") << in;
  }
  Parsed p;
  parse_into(p, gtext_css_parse_comma_list, "a,,");
  EXPECT_EQ(serialize(p.tree), "a,,");
}

TEST(CssSerializeRules, OneRuleOneDeclarationAndValuesOnTheirOwn) {
  Parsed p;
  parse_into(p, gtext_css_parse_stylesheet, "a{b:c !important; d:{e}}");
  std::string out;
  GTEXT_Markup_Sink sink = {string_sink, &out};
  ASSERT_EQ(gtext_css_serialize_declaration(&p.tree->rules[0].declarations[0], nullptr, &sink), GTEXT_CSS_OK);
  EXPECT_EQ(out, "b:c !important");
  out.clear();
  ASSERT_EQ(gtext_css_serialize_rule(&p.tree->rules[0], false, nullptr, &sink), GTEXT_CSS_OK);
  EXPECT_EQ(out, "a{b:c !important;d:{e};}");
  out.clear();
  const GTEXT_CSS_Declaration & d = p.tree->rules[0].declarations[1];
  ASSERT_EQ(gtext_css_serialize_values(d.value, d.value_count, nullptr, &sink), GTEXT_CSS_OK);
  EXPECT_EQ(out, "{e}");
  out.clear();
  EXPECT_EQ(gtext_css_serialize_values(nullptr, 0, nullptr, &sink), GTEXT_CSS_OK);
  EXPECT_EQ(out, "");
  Parsed e;
  parse_into(e, gtext_css_parse_rule, "a b");
  EXPECT_EQ(serialize(e.tree), "") << "a syntax error writes nothing";
}

// ---- the round trip (section 9) -----------------------------------------------------------------

TEST(CssRoundTrip, ParseSerializeParseIsTheSameTreeForEveryEntryPoint) {
  struct Case {
    ParseFn fn;
    const char * css;
  };
  const Case cases[] = {
      {gtext_css_parse_stylesheet, "a{b:c;d{e:f}g:h} @media (x) { i { j: k } } --x:hover{} l{--m: {n} o; p: {q} !important}"},
      {gtext_css_parse_stylesheet, "a{b:{c} d} e{f:g{} h} i{j:k{}} l{m:{} !important} n{o:{} ! }"},
      {gtext_css_parse_stylesheet, "a{ <!-- b{} --> } c{\\\n} d{'e\n} f{url(g h) url(i} j{"},
      {gtext_css_parse_stylesheet, "@a } b {} c { @d } e { @f; g:h }"},
      {gtext_css_parse_stylesheet_contents, "a<!---->{}"},
      {gtext_css_parse_declaration_list, "a: b } c; @d { e } ; f: g ! important ; --h: ; unicode-range: u+1?, U+1-2"},
      {gtext_css_parse_block_contents, "a:b; c{} d:e; @f; g{h:{}}"},
      {gtext_css_parse_rule, "  @a b { c; d: e }  "},
      {gtext_css_parse_declaration, " --x: a {b} c "},
      {gtext_css_parse_component_value, " f( [a] {b} (c) ) "},
      {gtext_css_parse_component_values, "a(b [c {d ) e"},
      {gtext_css_parse_component_values, "#0 #a 1e3 1. .5% -- --> <!-- u+1 \\- \\30 x"},
      {gtext_css_parse_comma_list, "a, b(c, d), , e,"},
  };
  for (const Case & c : cases) {
    Parsed p;
    parse_into(p, c.fn, c.css);
    ASSERT_EQ(p.status, GTEXT_CSS_OK) << c.css;
    EXPECT_EQ(round_trip(p.tree), "") << c.css;
  }
}

TEST(CssRoundTrip, TheRoundTripIsAFixedPoint) {
  Parsed p;
  parse_into(p, gtext_css_parse_stylesheet, "a/**/b{c:d e/**/f;--g: h /* i */ j}");
  const std::string once = serialize(p.tree);
  Parsed q;
  parse_into(q, gtext_css_parse_stylesheet, once);
  EXPECT_EQ(serialize(q.tree), once);
}

// ---- sinks, limits and buffers ------------------------------------------------------------------

struct Refusing {
  size_t left;
  std::string got;
};

GTEXT_Markup_Status refusing_sink(void * user, const char * bytes, size_t len) {
  auto * r = static_cast<Refusing *>(user);
  if (len > r->left) {
    return GTEXT_MARKUP_E_WRITE;
  }
  r->left -= len;
  r->got.append(bytes, len);
  return GTEXT_MARKUP_OK;
}

TEST(CssSerializeSink, ASinkThatRefusesStopsTheWriteWithEWrite) {
  Parsed p;
  parse_into(p, gtext_css_parse_stylesheet, "a{b:c}");
  Refusing r{3, ""};
  GTEXT_Markup_Sink sink = {refusing_sink, &r};
  EXPECT_EQ(gtext_css_serialize_tree(p.tree, nullptr, &sink), GTEXT_CSS_E_WRITE);
  EXPECT_EQ(r.got, "a{b");
}

TEST(CssSerializeSink, TheOutputLimitWritesUpToItAndNoFurther) {
  Parsed p;
  parse_into(p, gtext_css_parse_stylesheet, "abc{def:ghi}");
  GTEXT_CSS_Serialize_Options o = gtext_css_serialize_options_default();
  o.max_output_bytes = 13;
  std::string out;
  GTEXT_Markup_Sink sink = {string_sink, &out};
  EXPECT_EQ(gtext_css_serialize_tree(p.tree, &o, &sink), GTEXT_CSS_OK) << "exactly the limit";
  EXPECT_EQ(out.size(), 13u);
  out.clear();
  o.max_output_bytes = 12;
  EXPECT_EQ(gtext_css_serialize_tree(p.tree, &o, &sink), GTEXT_CSS_E_LIMIT_OUTPUT);
  EXPECT_EQ(out, "abc{def:ghi;");
}

TEST(CssSerializeSink, UnusableArgumentsAreInvalid) {
  Parsed p;
  parse_into(p, gtext_css_parse_stylesheet, "a{}");
  std::string out;
  GTEXT_Markup_Sink sink = {string_sink, &out};
  GTEXT_Markup_Sink none = {nullptr, nullptr};
  EXPECT_EQ(gtext_css_serialize_tree(nullptr, nullptr, &sink), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_serialize_tree(p.tree, nullptr, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_serialize_tree(p.tree, nullptr, &none), GTEXT_CSS_E_INVALID);
  GTEXT_CSS_Serialize_Options o = gtext_css_serialize_options_default();
  o.numbers = (GTEXT_CSS_Number_Style)7;
  EXPECT_EQ(gtext_css_serialize_tree(p.tree, &o, &sink), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_serialize_rule(nullptr, false, nullptr, &sink), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_serialize_declaration(nullptr, nullptr, &sink), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_serialize_values(nullptr, 1, nullptr, &sink), GTEXT_CSS_E_INVALID);
  char * buf = nullptr;
  size_t len = 0;
  EXPECT_EQ(gtext_css_serialize_tree_buffer(p.tree, nullptr, nullptr, &len), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_serialize_values_buffer(nullptr, 0, nullptr, &buf, nullptr), GTEXT_CSS_E_INVALID);
}

TEST(CssSerializeBuffer, IsTerminatedAndFreedWithItsOptionsAllocator) {
  FaultAllocator fa;
  GTEXT_CSS_Serialize_Options o = gtext_css_serialize_options_default();
  o.allocator = &fa.alloc;
  Parsed p;
  parse_into(p, gtext_css_parse_stylesheet, "a{b:c}");
  char * buf = nullptr;
  size_t len = 0;
  ASSERT_EQ(gtext_css_serialize_tree_buffer(p.tree, &o, &buf, &len), GTEXT_CSS_OK);
  EXPECT_EQ(std::string(buf), "a{b:c;}");
  EXPECT_EQ(len, 7u);
  EXPECT_EQ(fa.live.size(), 1u);
  gtext_css_serialize_buffer_free(&o, buf);
  EXPECT_TRUE(fa.live.empty());
  Parsed e;
  parse_into(e, gtext_css_parse_stylesheet, "");
  ASSERT_EQ(gtext_css_serialize_tree_buffer(e.tree, &o, &buf, &len), GTEXT_CSS_OK);
  EXPECT_EQ(len, 0u);
  EXPECT_STREQ(buf, "");
  gtext_css_serialize_buffer_free(&o, buf);
  gtext_css_serialize_buffer_free(&o, nullptr);
  EXPECT_TRUE(fa.live.empty());
}

TEST(CssSerializeBuffer, EveryAllocationCanFailAndNothingLeaks) {
  Parsed p;
  parse_into(p, gtext_css_parse_stylesheet, "a{b:c; d{e:f}} g{h:i}");
  const std::string want = serialize(p.tree);
  bool done = false;
  for (size_t n = 1; n < 200 && !done; n++) {
    FaultAllocator fa;
    GTEXT_CSS_Serialize_Options o = gtext_css_serialize_options_default();
    o.allocator = &fa.alloc;
    fa.arm(n);
    char * buf = nullptr;
    size_t len = 0;
    const GTEXT_CSS_Status st = gtext_css_serialize_tree_buffer(p.tree, &o, &buf, &len);
    fa.disarm();
    if (st == GTEXT_CSS_OK) {
      EXPECT_EQ(std::string(buf, len), want) << n;
      gtext_css_serialize_buffer_free(&o, buf);
      done = !fa.failed;
    }
    else {
      EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
      EXPECT_EQ(buf, nullptr) << n;
    }
    EXPECT_TRUE(fa.live.empty()) << n;
    EXPECT_EQ(fa.bad_frees, 0u) << n;
  }
  EXPECT_TRUE(done);
}

// ---- 9.1 An+B -----------------------------------------------------------------------------------

TEST(CssSerializeAnB, FollowsSection91) {
  struct Case {
    int64_t a, b;
    const char * want;
  };
  const Case cases[] = {{0, 0, "0"}, {0, -3, "-3"}, {1, 0, "n"}, {-1, 0, "-n"}, {2, 1, "2n+1"}, {-2, -1, "-2n-1"},
      {1, 5, "n+5"}, {-1, -5, "-n-5"}, {INT64_MIN, INT64_MIN, "-9223372036854775808n-9223372036854775808"}};
  for (const Case & c : cases) {
    char buf[64];
    const size_t n = gtext_css_anb_serialize(c.a, c.b, buf, sizeof buf);
    EXPECT_EQ(std::string(buf, n), c.want);
  }
  char small[3];
  EXPECT_EQ(gtext_css_anb_serialize(2, 1, small, sizeof small), 0u) << "a buffer too small writes nothing";
  EXPECT_EQ(gtext_css_anb_serialize(2, 1, nullptr, 10), 0u);
}

} // namespace
