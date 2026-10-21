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
 * The CSS tokenizer against CSS Syntax 3.3 (preprocessing) and section 4
 * (tokenization), Editor's Draft of 20 October 2026. Each test is a rule of
 * the draft, named for what it says; the tokens are read through
 * gtext_css_parse_component_values(), whose values are the tokens (with
 * blocks and functions grouped, which these inputs avoid unless that is the
 * point). css-parsing-tests and the WPT tables score the same code against
 * vectors nobody here chose (make conformance-css-syntax); these hold the
 * rules those vectors do not reach, and the positions, which no vector has.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_test_support.h"

using namespace css_test;

namespace {

GTEXT_CSS_Parse_Options wtf8() {
  GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
  o.input_wtf8 = true;
  return o;
}

std::vector<std::string> errors_of(const std::string & css, const GTEXT_CSS_Parse_Options * o = nullptr) {
  Parsed p;
  parse_into(p, gtext_css_parse_component_values, css, o);
  return p.error_names();
}

// ---- 3.3 preprocessing -------------------------------------------------------------------------

TEST(CssPreprocessing, CarriageReturnLineFeedAndFormFeedAreEachOneNewline) {
  Parsed p;
  parse_into(p, gtext_css_parse_component_values, "a\r\nb\rc\fd\ne");
  ASSERT_EQ(p.status, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(p.tree->values, p.tree->value_count), "ident:a ws ident:b ws ident:c ws ident:d ws ident:e");
  // Each is a line of its own: lines 1 to 5, each name in column 1.
  for (size_t i = 0; i < 5; i++) {
    EXPECT_EQ(p.tree->values[2 * i].span.line, i + 1);
    EXPECT_EQ(p.tree->values[2 * i].span.column, 1u);
  }
  // The offsets are the input's bytes: CR LF is two.
  EXPECT_EQ(p.tree->values[2].span.offset, 3u);
  EXPECT_EQ(p.tree->values[4].span.offset, 5u);
}

TEST(CssPreprocessing, NullBecomesReplacementCharacter) {
  EXPECT_EQ(tokens(std::string("a\0b", 3)), "ident:a\xEF\xBF\xBD" "b");
  EXPECT_EQ(tokens(std::string("\"\0\"", 3)), "string:\xEF\xBF\xBD");
}

TEST(CssPreprocessing, ASurrogateInWtf8IsOneReplacementCharacter) {
  const GTEXT_CSS_Parse_Options o = wtf8();
  EXPECT_EQ(tokens("a\xED\xA0\x80" "b", &o), "ident:a\xEF\xBF\xBD" "b");
  EXPECT_EQ(tokens("a\xED\xBF\xBF" "b", &o), "ident:a\xEF\xBF\xBD" "b");
  EXPECT_TRUE(errors_of("a\xED\xA0\x80" "b", &o).empty());
}

TEST(CssPreprocessing, SurrogateBytesWithoutWtf8AreIllFormedUtf8) {
  // Each maximal subpart is one U+FFFD: ED is a lead byte A0 cannot follow.
  EXPECT_EQ(tokens("a\xED\xA0\x80" "b"), "ident:a\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD" "b");
  EXPECT_EQ(errors_of("a\xED\xA0\x80" "b"), (std::vector<std::string>{"invalid-utf-8", "invalid-utf-8", "invalid-utf-8"}));
}

TEST(CssPreprocessing, IllFormedUtf8IsReplacedPerMaximalSubpart) {
  // A truncated two-byte sequence, a lone continuation byte, an overlong lead, a truncated four-byte one.
  EXPECT_EQ(tokens("a\xC3" "b"), "ident:a\xEF\xBF\xBD" "b");
  EXPECT_EQ(tokens("a\x80" "b"), "ident:a\xEF\xBF\xBD" "b");
  EXPECT_EQ(tokens("a\xC0\xAF" "b"), "ident:a\xEF\xBF\xBD\xEF\xBF\xBD" "b");
  EXPECT_EQ(tokens("a\xF0\x9F\x98"), "ident:a\xEF\xBF\xBD");
  Parsed p;
  parse_into(p, gtext_css_parse_component_values, "ab\xC3");
  ASSERT_EQ(p.errors().size(), 1u);
  EXPECT_EQ(p.errors()[0].byte_offset, 2u);
  EXPECT_EQ(p.errors()[0].column, 3u);
}

// ---- 4.3.1 consume a token ---------------------------------------------------------------------

TEST(CssTokenizer, WhitespaceRunsAreOneToken) {
  EXPECT_EQ(tokens("a \t\n\r\n  b"), "ident:a ws ident:b");
  EXPECT_EQ(tokens("a /**/ b"), "ident:a ws ws ident:b") << "a comment is not white space, so it ends a run";
}

TEST(CssTokenizer, CommentsAreNotTokens) {
  EXPECT_EQ(tokens("a/* x */b/**/c"), "ident:a ident:b ident:c");
  EXPECT_EQ(tokens("/*/*/a"), "ident:a");
}

TEST(CssTokenizer, AnUnterminatedCommentIsAParseErrorAtTheEnd) {
  Parsed p;
  parse_into(p, gtext_css_parse_component_values, "a /* b");
  EXPECT_EQ(text_of(p.tree->values, p.tree->value_count), "ident:a ws");
  ASSERT_EQ(p.error_names(), (std::vector<std::string>{"eof-in-comment"}));
  EXPECT_EQ(p.errors()[0].byte_offset, 6u);
}

TEST(CssTokenizer, HashIsIdWhenItsNameWouldStartAnIdentSequence) {
  // (The space after an escape is the escape's, so `#\\31 #_` is two tokens with nothing between them.)
  EXPECT_EQ(tokens("#a #-a #--a #\\31 #_"), "hash-id:a ws hash-id:-a ws hash-id:--a ws hash-id:1 hash-id:_");
  EXPECT_EQ(tokens("#1 #-1 #-"), "hash:1 ws hash:-1 ws hash:-");
}

TEST(CssTokenizer, HashWithNoNameIsADelim) {
  EXPECT_EQ(tokens("# #\\\n"), "delim:# ws delim:# delim:\\ ws");
}

TEST(CssTokenizer, PlusMinusAndFullStopBeginANumberOnlyWhenOneFollows) {
  EXPECT_EQ(tokens("+1 -1 .1 +.1 -.1"),
      "number:+1=1/int/+ ws number:-1=-1/int/- ws number:.1=0.10000000000000001/num ws number:+.1=0.10000000000000001/num/+ ws "
      "number:-.1=-0.10000000000000001/num/-");
  EXPECT_EQ(tokens("+ - . +. -."), "delim:+ ws delim:- ws delim:. ws delim:+ delim:. ws delim:- delim:.");
}

TEST(CssTokenizer, CdcIsCheckedBeforeAnIdentSequence) {
  EXPECT_EQ(tokens("-->"), "CDC");
  EXPECT_EQ(tokens("--> --x -->x"), "CDC ws ident:--x ws CDC ident:x");
  EXPECT_EQ(tokens("<!-- <!- <"), "CDO ws delim:< delim:! delim:- ws delim:<");
}

TEST(CssTokenizer, MinusStartsAnIdentWhenTheNextTwoWouldStartOne) {
  EXPECT_EQ(tokens("-a -- -\\31  -1a"), "ident:-a ws ident:-- ws ident:-1 ws dimension:-1=-1/int/-/a");
  EXPECT_EQ(tokens("-\\31 -1a"), "ident:-1-1a") << "the escape's white space is not a token, so the name goes on";
}

TEST(CssTokenizer, AtKeywordNeedsAnIdentSequence) {
  EXPECT_EQ(tokens("@a @-a @1 @ a @\\@"), "at:a ws at:-a ws delim:@ number:1=1/int ws delim:@ ws ident:a ws at:@");
}

TEST(CssTokenizer, ABackslashThatIsNoEscapeIsADelimAndAParseError) {
  Parsed p;
  parse_into(p, gtext_css_parse_component_values, "a \\\nb");
  EXPECT_EQ(text_of(p.tree->values, p.tree->value_count), "ident:a ws delim:\\ ws ident:b");
  ASSERT_EQ(p.error_names(), (std::vector<std::string>{"invalid-escape"}));
  EXPECT_EQ(p.errors()[0].byte_offset, 2u);
  EXPECT_EQ(p.errors()[0].column, 3u);
}

TEST(CssTokenizer, PunctuationTokens) {
  EXPECT_EQ(tokens(": ; , ] ) }"), ": ws ; ws , ws ] ws ) ws }");
}

TEST(CssTokenizer, AnythingElseIsADelim) {
  EXPECT_EQ(tokens("! $ ~ ^ | * = > ? \x7F"),
      "delim:! ws delim:$ ws delim:~ ws delim:^ ws delim:| ws delim:* ws delim:= ws delim:> ws delim:? ws delim:\x7F");
}

TEST(CssTokenizer, MatchTokensOfCss21AreNotTokens) {
  EXPECT_EQ(tokens("~=|="), "delim:~ delim:= delim:| delim:=");
}

// ---- 4.2 ident code points -----------------------------------------------------------------------

TEST(CssTokenizer, TheNonAsciiIdentCodePointsAreTheDraftsList) {
  struct Case {
    const char * cp;
    bool ident;
  };
  const Case cases[] = {
      {"\xC2\x80", false}, {"\xC2\xB6", false}, {"\xC2\xB7", true}, {"\xC3\x80", true}, {"\xC3\x97", false},
      {"\xC3\x98", true}, {"\xC3\xB7", false}, {"\xC3\xB8", true}, {"\xCD\xBD", true}, {"\xCD\xBE", false},
      {"\xCD\xBF", true}, {"\xE1\xBF\xBF", true}, {"\xE2\x80\x80", false}, {"\xE2\x80\x8C", true},
      {"\xE2\x80\x8D", true}, {"\xE2\x80\x8E", false}, {"\xE2\x80\xBF", true}, {"\xE2\x81\x80", true},
      {"\xE2\x81\x81", false}, {"\xE2\x81\xB0", true}, {"\xE2\x86\x8F", true}, {"\xE2\x86\x90", false},
      {"\xE2\xB0\x80", true}, {"\xE2\xBF\xAF", true}, {"\xE2\xBF\xB0", false}, {"\xE3\x80\x80", false},
      {"\xE3\x80\x81", true}, {"\xED\x9F\xBF", true}, {"\xEF\xA4\x80", true}, {"\xEF\xB7\x8F", true},
      {"\xEF\xB7\x90", false}, {"\xEF\xB7\xAF", false}, {"\xEF\xB7\xB0", true}, {"\xEF\xBF\xBD", true},
      {"\xEF\xBF\xBE", false}, {"\xF0\x90\x80\x80", true}, {"\xF4\x8F\xBF\xBF", true},
  };
  for (const Case & c : cases) {
    const std::string s = std::string("f") + c.cp + "oo";
    if (c.ident) {
      EXPECT_EQ(tokens(s), "ident:" + s) << s;
    }
    else {
      EXPECT_EQ(tokens(s), std::string("ident:f delim:") + c.cp + " ident:oo") << s;
    }
  }
}

// ---- 4.3.7 escapes ------------------------------------------------------------------------------

TEST(CssTokenizer, AHexEscapeTakesOneToSixDigitsAndOneWhitespace) {
  EXPECT_EQ(tokens("\\41 B"), "ident:AB");
  EXPECT_EQ(tokens("\\41  B"), "ident:A ws ident:B") << "one white space is the escape's; the second is a token";
  EXPECT_EQ(tokens("\\00004142"), "ident:A42") << "six digits at most";
  EXPECT_EQ(tokens("\\41\r\nB"), "ident:AB") << "CR LF is one newline, so one white space";
}

TEST(CssTokenizer, AnEscapeOfZeroASurrogateOrPastUnicodeIsReplacementCharacter) {
  EXPECT_EQ(tokens("\\0 x"), "ident:\xEF\xBF\xBDx");
  EXPECT_EQ(tokens("\\D800 x"), "ident:\xEF\xBF\xBDx");
  EXPECT_EQ(tokens("\\DFFF x"), "ident:\xEF\xBF\xBDx");
  EXPECT_EQ(tokens("\\110000 x"), "ident:\xEF\xBF\xBDx");
  EXPECT_EQ(tokens("\\10FFFF x"), "ident:\xF4\x8F\xBF\xBFx");
}

TEST(CssTokenizer, AnyOtherEscapedCodePointIsItself) {
  EXPECT_EQ(tokens("\\. \\\" \\g"), "ident:. ws ident:\" ws ident:g");
}

TEST(CssTokenizer, AnEscapedEndOfInputIsReplacementCharacterInANameAndNothingInAString) {
  Parsed p;
  parse_into(p, gtext_css_parse_component_values, "a\\");
  EXPECT_EQ(text_of(p.tree->values, p.tree->value_count), "ident:a\xEF\xBF\xBD");
  EXPECT_EQ(p.error_names(), (std::vector<std::string>{"eof-in-escape"}));
  Parsed q;
  parse_into(q, gtext_css_parse_component_values, "\"a\\");
  EXPECT_EQ(text_of(q.tree->values, q.tree->value_count), "string:a<eof>");
  EXPECT_EQ(q.error_names(), (std::vector<std::string>{"eof-in-string"}));
}

// ---- 4.3.13 numbers -----------------------------------------------------------------------------

TEST(CssTokenizer, ANumberKeepsItsValueTypeSignAndSpelling) {
  EXPECT_EQ(tokens("12 +34 -0 1.5 1e3 1E+3 1e-3 .5e1"),
      "number:12=12/int ws number:+34=34/int/+ ws number:-0=-0/int/- ws number:1.5=1.5/num ws number:1e3=1000/num ws "
      "number:1E+3=1000/num ws number:1e-3=0.001/num ws number:.5e1=5/num");
}

TEST(CssTokenizer, ANumberIsTheCorrectlyRoundedValueOfWhatIsWritten) {
  // 0.1 + 0.2 written out: the double nearest the decimal, not a sum of doubles.
  EXPECT_EQ(tokens("0.30000000000000004 9007199254740993"),
      "number:0.30000000000000004=0.30000000000000004/num ws number:9007199254740993=9007199254740992/int");
}

TEST(CssTokenizer, AnExponentNeedsADigit) {
  EXPECT_EQ(tokens("1e"), "dimension:1=1/int/e");
  EXPECT_EQ(tokens("1e+"), "dimension:1=1/int/e delim:+");
  EXPECT_EQ(tokens("1e+x"), "dimension:1=1/int/e delim:+ ident:x");
  EXPECT_EQ(tokens("3e-2.1"), "number:3e-2=0.029999999999999999/num number:.1=0.10000000000000001/num");
}

TEST(CssTokenizer, ADecimalPointNeedsADigitAfterIt) {
  EXPECT_EQ(tokens("1."), "number:1=1/int delim:.");
  EXPECT_EQ(tokens("1.x"), "number:1=1/int delim:. ident:x");
}

TEST(CssTokenizer, APercentageHasNoTypeFlagAndAnEscapedPercentIsAUnit) {
  EXPECT_EQ(tokens("12% 1.5% 12\\%"), "percentage:12=12 ws percentage:1.5=1.5 ws dimension:12=12/int/%");
}

TEST(CssTokenizer, ADimensionsUnitIsAnIdentSequence) {
  EXPECT_EQ(tokens("12px 12-x 12--x 12\\-"), "dimension:12=12/int/px ws dimension:12=12/int/-x ws dimension:12=12/int/--x ws "
                                             "dimension:12=12/int/-");
  EXPECT_EQ(tokens("12-0"), "number:12=12/int number:-0=-0/int/-");
}

TEST(CssTokenizer, AnExponentPastTheRangeOfADoubleIsInfiniteOrZero) {
  EXPECT_EQ(tokens("1e999 -1e999 1e-999"), "number:1e999=inf/num ws number:-1e999=-inf/num/- ws number:1e-999=0/num");
}

// ---- 4.3.5 strings and 4.3.6 urls ---------------------------------------------------------------

TEST(CssTokenizer, StringsTakeEitherQuoteAndEscapes) {
  EXPECT_EQ(tokens("\"a'b\" 'a\"b' 'a\\'b' \"a\\\nb\""), "string:a'b ws string:a\"b ws string:a'b ws string:ab");
}

TEST(CssTokenizer, ANewlineInAStringIsABadStringAndTheNewlineIsReadAgain) {
  Parsed p;
  parse_into(p, gtext_css_parse_component_values, "'a\nb'");
  EXPECT_EQ(text_of(p.tree->values, p.tree->value_count), "bad-string ws ident:b string:<eof>");
  ASSERT_GE(p.errors().size(), 1u);
  EXPECT_EQ(p.error_names()[0], "newline-in-string");
  EXPECT_EQ(p.errors()[0].byte_offset, 2u);
}

TEST(CssTokenizer, AStringTheInputEndsIsAStringAndAParseError) {
  Parsed p;
  parse_into(p, gtext_css_parse_component_values, "'abc");
  EXPECT_EQ(text_of(p.tree->values, p.tree->value_count), "string:abc<eof>");
  EXPECT_EQ(p.error_names(), (std::vector<std::string>{"eof-in-string"}));
  EXPECT_EQ(p.errors()[0].byte_offset, 4u);
}

TEST(CssTokenizer, UrlFollowedByAQuoteIsAFunction) {
  EXPECT_EQ(tokens("url('a') url( \"a\") url(  'a')"),
      "f:url(string:a) ws f:url(ws string:a) ws f:url(ws string:a)");
  EXPECT_EQ(tokens("URL(a) uRl( a )"), "url:a ws url:a");
  EXPECT_EQ(tokens("\\75rl(a)"), "url:a") << "the check is on the name, not on how it is spelled";
}

TEST(CssTokenizer, AnUnquotedUrlStopsAtWhatItCannotHold) {
  Parsed p;
  parse_into(p, gtext_css_parse_component_values, "url(a b) url(a\"b) url(a(b) url(a\\\nb) url(a\\)b) url(\x01)");
  EXPECT_EQ(text_of(p.tree->values, p.tree->value_count), "bad-url ws bad-url ws bad-url ws bad-url ws url:a)b ws bad-url");
  EXPECT_EQ(p.error_names(), (std::vector<std::string>{"whitespace-in-url", "invalid-character-in-url", "invalid-character-in-url",
                                 "invalid-escape-in-url", "invalid-character-in-url"}));
}

TEST(CssTokenizer, TheRemnantsOfABadUrlRunToAnUnescapedParenthesis) {
  EXPECT_EQ(tokens("url(a b\\)c) d"), "bad-url ws ident:d");
}

TEST(CssTokenizer, AUrlTheInputEndsIsAUrlAndAParseError) {
  Parsed p;
  parse_into(p, gtext_css_parse_component_values, "url(abc");
  EXPECT_EQ(text_of(p.tree->values, p.tree->value_count), "url:abc<eof>");
  EXPECT_EQ(p.error_names(), (std::vector<std::string>{"eof-in-url"}));
  Parsed q;
  parse_into(q, gtext_css_parse_component_values, "url(abc  ");
  EXPECT_EQ(text_of(q.tree->values, q.tree->value_count), "url:abc<eof>");
  EXPECT_EQ(q.error_names(), (std::vector<std::string>{"eof-in-url"}));
}

// ---- 4.3.14 unicode-range tokens ---------------------------------------------------------------

TEST(CssTokenizer, UnicodeRangeTokensAreReadOnlyInAUnicodeRangeDeclaration) {
  EXPECT_EQ(tokens("u+1-2"), "ident:u number:+1=1/int/+ number:-2=-2/int/-");
  Parsed p;
  parse_into(p, gtext_css_parse_declaration_list, "unicode-range: u+1-2, U+4??, U+10FFFF; other: u+1");
  ASSERT_EQ(p.tree->declaration_count, 2u);
  EXPECT_EQ(text_of(p.tree->declarations[0].value, p.tree->declarations[0].value_count),
      "urange:1-2 , ws urange:400-4FF , ws urange:10FFFF-10FFFF");
  EXPECT_EQ(text_of(p.tree->declarations[1].value, p.tree->declarations[1].value_count), "ident:u number:+1=1/int/+");
}

TEST(CssTokenizer, AUnicodeRangeTakesSixDigitsAndQuestionMarksAtMost) {
  Parsed p;
  parse_into(p, gtext_css_parse_declaration_list, "UNICODE-RANGE: u+1234567 U+12345?? u+?? u+1-1234567");
  ASSERT_EQ(p.tree->declaration_count, 1u);
  EXPECT_EQ(text_of(p.tree->declarations[0].value, p.tree->declarations[0].value_count),
      "urange:123456-123456 number:7=7/int ws urange:123450-12345F delim:? ws urange:0-FF ws urange:1-123456 number:7=7/int");
}

// ---- positions ----------------------------------------------------------------------------------

TEST(CssPositions, ColumnsCountUtf16CodeUnits) {
  Parsed p;
  parse_into(p, gtext_css_parse_component_values, "\xF0\x9F\x98\x80 a \xC3\xA9 b");
  ASSERT_EQ(p.tree->value_count, 7u);
  // U+1F600 is two code units, so `a` is in column 4; U+00E9 is one.
  EXPECT_EQ(p.tree->values[2].span.column, 4u);
  EXPECT_EQ(p.tree->values[2].span.offset, 5u);
  EXPECT_EQ(p.tree->values[6].span.column, 8u);
}

TEST(CssPositions, ASpanCoversTheTokensBytes) {
  Parsed p;
  parse_into(p, gtext_css_parse_component_values, "ab \\41 x 'q' url( z )");
  ASSERT_EQ(p.tree->value_count, 7u);
  EXPECT_EQ(p.tree->values[0].span.offset, 0u);
  EXPECT_EQ(p.tree->values[0].span.end, 2u);
  EXPECT_EQ(p.tree->values[2].span.offset, 3u);
  EXPECT_EQ(p.tree->values[2].span.end, 8u) << "an escape and its white space are the token's";
  EXPECT_EQ(p.tree->values[4].span.offset, 9u);
  EXPECT_EQ(p.tree->values[4].span.end, 12u);
  EXPECT_EQ(p.tree->values[6].span.offset, 13u);
  EXPECT_EQ(p.tree->values[6].span.end, 21u);
}

TEST(CssPositions, EveryTokenizerErrorHasTheLineAndColumnOfItsPlace) {
  Parsed p;
  parse_into(p, gtext_css_parse_component_values, "a\nb 'c\nd \\\n");
  const auto e = p.errors();
  ASSERT_EQ(e.size(), 2u);
  EXPECT_EQ(std::string(e[0].name), "newline-in-string");
  EXPECT_EQ(e[0].line, 2u);
  EXPECT_EQ(e[0].column, 5u);
  EXPECT_EQ(std::string(e[1].name), "invalid-escape");
  EXPECT_EQ(e[1].line, 3u);
  EXPECT_EQ(e[1].column, 3u);
}

// ---- names --------------------------------------------------------------------------------------

TEST(CssNames, EveryStatusHasItsOwnString) {
  std::vector<std::string> seen;
  for (int s = 0; s < GTEXT_CSS_STATUS_COUNT; s++) {
    const std::string name = gtext_css_status_string((GTEXT_CSS_Status)s);
    EXPECT_NE(name, "unknown status") << s;
    for (const auto & n : seen) {
      EXPECT_NE(n, name) << s;
    }
    seen.push_back(name);
  }
  EXPECT_EQ(std::string(gtext_css_status_string(GTEXT_CSS_STATUS_COUNT)), "unknown status");
}

TEST(CssNames, EveryTokenTypeHasTheStandardsName) {
  EXPECT_EQ(std::string(gtext_css_token_type_name(GTEXT_CSS_TOKEN_IDENT)), "ident");
  EXPECT_EQ(std::string(gtext_css_token_type_name(GTEXT_CSS_TOKEN_BAD_URL)), "bad-url");
  EXPECT_EQ(std::string(gtext_css_token_type_name(GTEXT_CSS_TOKEN_CLOSE_CURLY)), "}");
  EXPECT_EQ(std::string(gtext_css_token_type_name(GTEXT_CSS_TOKEN_TYPE_COUNT)), "unknown");
  for (int t = 0; t < GTEXT_CSS_TOKEN_TYPE_COUNT; t++) {
    EXPECT_NE(std::string(gtext_css_token_type_name((GTEXT_CSS_Token_Type)t)), "unknown") << t;
  }
}

TEST(CssNames, EveryErrorCodeHasADistinctName) {
  const int codes[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42};
  std::vector<std::string> seen;
  for (int c : codes) {
    const std::string name = gtext_css_error_name((GTEXT_CSS_Error_Code)c);
    EXPECT_NE(name, "unknown") << c;
    for (const auto & n : seen) {
      EXPECT_NE(n, name) << c;
    }
    seen.push_back(name);
  }
  EXPECT_EQ(std::string(gtext_css_error_name((GTEXT_CSS_Error_Code)10)), "unknown");
  EXPECT_EQ(std::string(gtext_css_error_name((GTEXT_CSS_Error_Code)43)), "unknown");
}

} // namespace
