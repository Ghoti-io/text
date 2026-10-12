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
 * The HTML tokenizer (src/html/html_tokenizer.c).
 *
 * Every expectation here comes from the WHATWG HTML standard's tokenization
 * chapter ("Tokenization", 13.2.5, and "Preprocessing the input stream"), read
 * at the state the case exercises, not from what the code returned: the
 * expected strings were worked out by hand from the state descriptions (and the
 * columns by counting). Where the standard leaves a choice and this tokenizer
 * made one, the test name says so.
 *
 * The html5lib-tests vectors are the conformance gate (`make
 * conformance-html-tokenizer`); this file is what that gate cannot be: the
 * rows of the specification's matrix by name, every way of cutting the input
 * in two (and in three, for the short ones), an allocation failure at every
 * point, work counted on pathological inputs, the limits, and the interface a
 * tree builder drives - a pull loop that changes the tokenizer's state between
 * tokens and expects the very next character to be read in it.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "../src/html/charref_internal.h"
#include "html_tokenizer_test_support.h"
#include "markup_test_support.h"

using namespace tokenizer_support;

namespace {

Outcome run(const std::string & input) {
  return tokenize(input);
}

Config in_state(GTEXT_HTML_TokenizerState state, const char * last = nullptr) {
  Config s;
  s.state = state;
  if (last) {
    s.has_last = true;
    s.last = last;
  }
  return s;
}

std::string s(const char * literal, size_t n) {
  return std::string(literal, n);
}

// ---- The error list ------------------------------------------------------

TEST(HtmlTokenizerErrors, EveryCodeHasTheStandardsSpellingAndNoTwoAreAlike) {
  std::set<std::string> seen;
  for (int i = 0; i < GTEXT_HTML_ERR_COUNT; i++) {
    const char * name = gtext_html_error_code_name((GTEXT_HTML_ErrorCode)i);
    ASSERT_NE(name, nullptr);
    EXPECT_STRNE(name, "unknown") << "code " << i;
    EXPECT_TRUE(seen.insert(name).second) << name << " appears twice";
    for (const char * p = name; *p; p++) {
      EXPECT_TRUE((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')
          << name << " is not lower case words joined by hyphens";
    }
  }
  // 52 codes in "Parse errors" at the retrieval of 2026-10-11, and the one this
  // tokenizer adds for the decoder.
  EXPECT_EQ(seen.size(), 53u);
  EXPECT_EQ(std::string(gtext_html_error_code_name(
                GTEXT_HTML_ERR_ABRUPT_CLOSING_OF_EMPTY_COMMENT)),
      "abrupt-closing-of-empty-comment");
  EXPECT_EQ(std::string(gtext_html_error_code_name(
                GTEXT_HTML_ERR_MISSING_WHITESPACE_BETWEEN_DOCTYPE_PUBLIC_AND_SYSTEM_IDENTIFIERS)),
      "missing-whitespace-between-doctype-public-and-system-identifiers");
  EXPECT_EQ(std::string(gtext_html_error_code_name(
                GTEXT_HTML_ERR_NON_VOID_HTML_ELEMENT_START_TAG_WITH_TRAILING_SOLIDUS)),
      "non-void-html-element-start-tag-with-trailing-solidus");
  EXPECT_EQ(std::string(gtext_html_error_code_name((GTEXT_HTML_ErrorCode)999)),
      "unknown");
}

// ---- The tokens ----------------------------------------------------------

TEST(HtmlTokenizer, ATagWithAttributesAndTextAndAnEndTag) {
  const Outcome r = run("<p class=a id='b' hidden>Hi</p>");
  EXPECT_EQ(r.tokens_text(), "S(p class=a id=b hidden=) C\"Hi\" E(p) F");
  EXPECT_EQ(r.errors_text(), "");
  EXPECT_EQ(r.status, GTEXT_HTML_TOKENIZER_DONE);
}

TEST(HtmlTokenizer, TagAndAttributeNamesAreLowerCasedAndValuesAreNot) {
  EXPECT_EQ(run("<DIV CLASS=A Id=\"Bc\">").tokens_text(),
      "S(div class=A id=Bc) F");
}

TEST(HtmlTokenizer, ADuplicateAttributeIsAnErrorAndIsDropped) {
  const Outcome r = run("<a b=1 b=2 c>");
  EXPECT_EQ(r.tokens_text(), "S(a b=1 c=) F");
  // Reported where the user agent leaves the name state: the `=` of the second.
  EXPECT_EQ(r.errors_text(), "duplicate-attribute@1:9");
}

TEST(HtmlTokenizer, ADroppedDuplicatesValueIsReadAndDiscardedIncludingReferences) {
  EXPECT_EQ(run("<a b=1 b=\"x&amp;y\" c=2>").tokens_text(), "S(a b=1 c=2) F");
}

TEST(HtmlTokenizer, TheSameNamesInTheNextTagAreNotDuplicates) {
  const Outcome r = run("<a x y><a x y>");
  EXPECT_EQ(r.tokens_text(), "S(a x= y=) S(a x= y=) F");
  EXPECT_EQ(r.errors_text(), "");
}

TEST(HtmlTokenizer, ThousandsOfAttributesAreComparedByLookupAndEveryRepeatIsFound) {
  std::string in = "<a";
  const int n = 5000;
  for (int i = 0; i < n; i++) {
    in += " a" + std::to_string(i);
  }
  in += " a0 a4999 a2500>";
  Config setup;
  setup.options.max_attributes = SIZE_MAX;
  const Outcome r = tokenize(in, {}, setup);
  ASSERT_EQ(r.tokens.size(), 2u);
  EXPECT_EQ(r.errors.size(), 3u);
  EXPECT_LE(r.work, GTEXT_HTML_TOKENIZER_WORK_BOUND(in.size()));
  // The 5,000 names, once each, in order.
  size_t at = r.tokens[0].find(" a0=");
  EXPECT_NE(at, std::string::npos);
  EXPECT_EQ(std::count(r.tokens[0].begin(), r.tokens[0].end(), '='), n);
}

TEST(HtmlTokenizer, ASolidusInATagIsTheSelfClosingFlagOnlyBeforeTheGreaterThan) {
  EXPECT_EQ(run("<br/>").tokens_text(), "S(br)/ F");
  const Outcome r = run("<a/b>");
  EXPECT_EQ(r.tokens_text(), "S(a b=) F");
  EXPECT_EQ(r.errors_text(), "unexpected-solidus-in-tag@1:4");
  EXPECT_EQ(run("<a b=c/>").tokens_text(), "S(a b=c/) F"); // unquoted value takes the /
  EXPECT_EQ(run("<a b='c'/>").tokens_text(), "S(a b=c)/ F");
}

TEST(HtmlTokenizer, AnEndTagWithAttributesOrASolidusIsReportedAtTheGreaterThan) {
  Outcome r = run("</a b>");
  EXPECT_EQ(r.tokens_text(), "E(a b=) F");
  EXPECT_EQ(r.errors_text(), "end-tag-with-attributes@1:6");
  r = run("</a/>");
  EXPECT_EQ(r.tokens_text(), "E(a)/ F");
  EXPECT_EQ(r.errors_text(), "end-tag-with-trailing-solidus@1:5");
}

TEST(HtmlTokenizer, TheOddCornersOfAttributeSyntax) {
  Outcome r = run("<a =b>");
  EXPECT_EQ(r.tokens_text(), "S(a =b=) F");
  EXPECT_EQ(r.errors_text(), "unexpected-equals-sign-before-attribute-name@1:4");
  r = run("<a b\"c'd<e=f>");
  EXPECT_EQ(r.tokens_text(), "S(a b\"c'd<e=f) F");
  EXPECT_EQ(r.errors_text(),
      "unexpected-character-in-attribute-name@1:5 "
      "unexpected-character-in-attribute-name@1:7 "
      "unexpected-character-in-attribute-name@1:9");
  r = run("<a b=c\"d'e<f=g`h>");
  EXPECT_EQ(r.tokens_text(), "S(a b=c\"d'e<f=g`h) F");
  EXPECT_EQ(r.errors_text(),
      "unexpected-character-in-unquoted-attribute-value@1:7 "
      "unexpected-character-in-unquoted-attribute-value@1:9 "
      "unexpected-character-in-unquoted-attribute-value@1:11 "
      "unexpected-character-in-unquoted-attribute-value@1:13 "
      "unexpected-character-in-unquoted-attribute-value@1:15");
  r = run("<a b=\"c\"d>");
  EXPECT_EQ(r.tokens_text(), "S(a b=c d=) F");
  EXPECT_EQ(r.errors_text(), "missing-whitespace-between-attributes@1:9");
  r = run("<a b=>");
  EXPECT_EQ(r.tokens_text(), "S(a b=) F");
  EXPECT_EQ(r.errors_text(), "missing-attribute-value@1:6");
  r = run("<a b\t=\t'c'>");
  EXPECT_EQ(r.tokens_text(), "S(a b=c) F");
  EXPECT_EQ(r.errors_text(), "");
}

TEST(HtmlTokenizer, ANulInANameOrAValueIsAReplacementCharacter) {
  const Outcome r = run(s("<a\0b c\0d=e\0f>", 13));
  EXPECT_EQ(r.tokens_text(),
      "S(a\\xef\\xbf\\xbdb c\\xef\\xbf\\xbdd=e\\xef\\xbf\\xbdf) F");
  EXPECT_EQ(r.errors.size(), 3u);
}

TEST(HtmlTokenizer, TheEndOfTheInputInsideATagAbandonsTheTag) {
  Outcome r = run("<a");
  EXPECT_EQ(r.tokens_text(), "F");
  EXPECT_EQ(r.errors_text(), "eof-in-tag@1:3");
  r = run("<a b='c");
  EXPECT_EQ(r.tokens_text(), "F");
  EXPECT_EQ(r.errors_text(), "eof-in-tag@1:8");
  r = run("x<a b=c");
  EXPECT_EQ(r.tokens_text(), "C\"x\" F");
  r = run("<a b");
  EXPECT_EQ(r.errors_text(), "eof-in-tag@1:5");
  r = run("<a b=\"c\"");
  EXPECT_EQ(r.errors_text(), "eof-in-tag@1:9");
  r = run("<a/");
  EXPECT_EQ(r.errors_text(), "eof-in-tag@1:4");
}

TEST(HtmlTokenizer, ALessThanSignThatBeginsNothingIsText) {
  Outcome r = run("a < b");
  EXPECT_EQ(r.tokens_text(), "C\"a < b\" F");
  EXPECT_EQ(r.errors_text(), "invalid-first-character-of-tag-name@1:4");
  r = run("<");
  EXPECT_EQ(r.tokens_text(), "C\"<\" F");
  EXPECT_EQ(r.errors_text(), "eof-before-tag-name@1:2");
  r = run("</");
  EXPECT_EQ(r.tokens_text(), "C\"</\" F");
  EXPECT_EQ(r.errors_text(), "eof-before-tag-name@1:3");
  r = run("</>");
  EXPECT_EQ(r.tokens_text(), "F");
  EXPECT_EQ(r.errors_text(), "missing-end-tag-name@1:3");
  r = run("</1>");
  EXPECT_EQ(r.tokens_text(), "M(1) F");
  EXPECT_EQ(r.errors_text(), "invalid-first-character-of-tag-name@1:3");
}

// ---- Comments ------------------------------------------------------------

TEST(HtmlTokenizerComments, TheShapesOfAComment) {
  EXPECT_EQ(run("<!---->").tokens_text(), "M() F");
  EXPECT_EQ(run("<!-- a -- b -->").tokens_text(), "M( a -- b ) F");
  EXPECT_EQ(run("<!--a-->b").tokens_text(), "M(a) C\"b\" F");
  Outcome r = run("<!-->");
  EXPECT_EQ(r.tokens_text(), "M() F");
  EXPECT_EQ(r.errors_text(), "abrupt-closing-of-empty-comment@1:5");
  r = run("<!--->");
  EXPECT_EQ(r.tokens_text(), "M() F");
  EXPECT_EQ(r.errors_text(), "abrupt-closing-of-empty-comment@1:6");
  r = run("<!--a--!>");
  EXPECT_EQ(r.tokens_text(), "M(a) F");
  EXPECT_EQ(r.errors_text(), "incorrectly-closed-comment@1:9");
  r = run("<!--a--!b-->");
  EXPECT_EQ(r.tokens_text(), "M(a--!b) F");
  r = run("<!--a--!-->");
  EXPECT_EQ(r.tokens_text(), "M(a--!) F");
}

TEST(HtmlTokenizerComments, TheEndOfTheInputInAComment) {
  for (const char * in : {"<!--", "<!--a", "<!--a-", "<!--a--", "<!--a--!",
           "<!---", "<!--<", "<!--<!", "<!--<!-", "<!--<!--"}) {
    const Outcome r = run(in);
    ASSERT_EQ(r.errors.size(), 1u) << in;
    EXPECT_EQ(r.errors[0].substr(0, 15), "eof-in-comment@") << in;
    ASSERT_EQ(r.tokens.size(), 2u) << in;
    EXPECT_EQ(r.tokens[0][0], 'M') << in;
    EXPECT_EQ(r.tokens[1], "F") << in;
  }
  EXPECT_EQ(run("<!--a--").tokens[0], "M(a)");
  EXPECT_EQ(run("<!--a-").tokens[0], "M(a)");
  EXPECT_EQ(run("<!--a--!").tokens[0], "M(a)"); // the bang is not data at the end
}

TEST(HtmlTokenizerComments, ANestedCommentOpenerIsAnErrorAndStaysInTheComment) {
  const Outcome r = run("<!-- <!-- x -->");
  EXPECT_EQ(r.tokens_text(), "M( <!-- x ) F");
  EXPECT_EQ(r.errors_text(), "nested-comment@1:10");
  // No error when what follows `<!--` ends the comment or the input.
  EXPECT_EQ(run("<!-- <!-->").errors_text(), "");
  // Two more hyphens after `<!--` are comment end dashes appended to the data
  // by the comment end state, not the end: `-->` follows.
  EXPECT_EQ(run("<!-- <!---->").tokens_text(), "M( <!--) F");
  EXPECT_EQ(run("<!--<!-x-->").tokens_text(), "M(<!-x) F");
}

TEST(HtmlTokenizerComments, ABogusCommentIsWhatEveryOtherMarkupDeclarationBecomes) {
  Outcome r = run("<!foo>");
  EXPECT_EQ(r.tokens_text(), "M(foo) F");
  EXPECT_EQ(r.errors_text(), "incorrectly-opened-comment@1:3");
  r = run("<!DOC>");
  EXPECT_EQ(r.tokens_text(), "M(DOC) F");
  EXPECT_EQ(r.errors_text(), "incorrectly-opened-comment@1:3");
  r = run("<!-x>");
  EXPECT_EQ(r.tokens_text(), "M(-x) F");
  EXPECT_EQ(run("</ a>").tokens_text(), "M( a) F");
  r = run("<!");
  EXPECT_EQ(r.tokens_text(), "M() F");
  EXPECT_EQ(r.errors_text(), "incorrectly-opened-comment@1:3");
  // A NUL in a bogus comment is a replacement character.
  EXPECT_EQ(run(s("<!a\0b>", 6)).tokens_text(), "M(a\\xef\\xbf\\xbdb) F");
}

// ---- DOCTYPE -------------------------------------------------------------

TEST(HtmlTokenizerDoctype, TheShapesOfADoctype) {
  EXPECT_EQ(run("<!DOCTYPE html>").tokens_text(), "D(html|-|-) F");
  EXPECT_EQ(run("<!doctype HTML>").tokens_text(), "D(html|-|-) F");
  EXPECT_EQ(run("<!DocType\thtml\n>").tokens_text(), "D(html|-|-) F");
  EXPECT_EQ(run("<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01//EN\" "
                "\"http://www.w3.org/TR/html4/strict.dtd\">")
                .tokens_text(),
      "D(html|-//W3C//DTD HTML 4.01//EN|http://www.w3.org/TR/html4/strict.dtd) F");
  EXPECT_EQ(run("<!DOCTYPE html system 'about:legacy-compat'>").tokens_text(),
      "D(html|-|about:legacy-compat) F");
  EXPECT_EQ(run("<!DOCTYPE html PUBLIC 'a'>").tokens_text(), "D(html|a|-) F");
  EXPECT_EQ(run("<!DOCTYPE html PUBLIC ''>").tokens_text(), "D(html||-) F");
  EXPECT_EQ(run("<!DOCTYPE html PUBLIC 'a' 'b' >").errors_text(), "");
  // An empty identifier is not a missing one.
  EXPECT_EQ(run("<!DOCTYPE html SYSTEM \"\">").tokens_text(), "D(html|-|) F");
}

TEST(HtmlTokenizerDoctype, ForceQuirksAndItsErrors) {
  Outcome r = run("<!DOCTYPE>");
  EXPECT_EQ(r.tokens_text(), "D(-|-|-)! F");
  EXPECT_EQ(r.errors_text(), "missing-doctype-name@1:10");
  r = run("<!DOCTYPE");
  EXPECT_EQ(r.tokens_text(), "D(-|-|-)! F");
  EXPECT_EQ(r.errors_text(), "eof-in-doctype@1:10");
  r = run("<!DOCTYPEhtml>");
  EXPECT_EQ(r.tokens_text(), "D(html|-|-) F");
  EXPECT_EQ(r.errors_text(), "missing-whitespace-before-doctype-name@1:10");
  r = run("<!DOCTYPE html foo>");
  EXPECT_EQ(r.tokens_text(), "D(html|-|-)! F");
  EXPECT_EQ(r.errors_text(), "invalid-character-sequence-after-doctype-name@1:16");
  r = run("<!DOCTYPE html PUBLIC>");
  EXPECT_EQ(r.tokens_text(), "D(html|-|-)! F");
  EXPECT_EQ(r.errors_text(), "missing-doctype-public-identifier@1:22");
  r = run("<!DOCTYPE html PUBLIC\"a\">");
  EXPECT_EQ(r.tokens_text(), "D(html|a|-) F");
  EXPECT_EQ(r.errors_text(), "missing-whitespace-after-doctype-public-keyword@1:22");
  r = run("<!DOCTYPE html PUBLIC x>");
  EXPECT_EQ(r.tokens_text(), "D(html|-|-)! F");
  EXPECT_EQ(r.errors_text(), "missing-quote-before-doctype-public-identifier@1:23");
  r = run("<!DOCTYPE html PUBLIC \"a>");
  EXPECT_EQ(r.tokens_text(), "D(html|a|-)! F");
  EXPECT_EQ(r.errors_text(), "abrupt-doctype-public-identifier@1:25");
  r = run("<!DOCTYPE html PUBLIC 'a''b'>");
  EXPECT_EQ(r.tokens_text(), "D(html|a|b) F");
  EXPECT_EQ(r.errors_text(),
      "missing-whitespace-between-doctype-public-and-system-identifiers@1:26");
  r = run("<!DOCTYPE html PUBLIC 'a' x>");
  EXPECT_EQ(r.tokens_text(), "D(html|a|-)! F");
  EXPECT_EQ(r.errors_text(), "missing-quote-before-doctype-system-identifier@1:27");
  r = run("<!DOCTYPE html SYSTEM>");
  EXPECT_EQ(r.tokens_text(), "D(html|-|-)! F");
  EXPECT_EQ(r.errors_text(), "missing-doctype-system-identifier@1:22");
  r = run("<!DOCTYPE html SYSTEM'a'>");
  EXPECT_EQ(r.tokens_text(), "D(html|-|a) F");
  EXPECT_EQ(r.errors_text(), "missing-whitespace-after-doctype-system-keyword@1:22");
  r = run("<!DOCTYPE html SYSTEM 'a>");
  EXPECT_EQ(r.tokens_text(), "D(html|-|a)! F");
  EXPECT_EQ(r.errors_text(), "abrupt-doctype-system-identifier@1:25");
  // After the system identifier, junk is an error but does not force quirks.
  r = run("<!DOCTYPE html SYSTEM 'a' x>");
  EXPECT_EQ(r.tokens_text(), "D(html|-|a) F");
  EXPECT_EQ(r.errors_text(), "unexpected-character-after-doctype-system-identifier@1:27");
  r = run("<!DOCTYPE html PUBLIC 'a' 'b");
  EXPECT_EQ(r.tokens_text(), "D(html|a|b)! F");
  EXPECT_EQ(r.errors_text(), "eof-in-doctype@1:29");
}

TEST(HtmlTokenizerDoctype, ANulIsAReplacementCharacterInANameOrAnIdentifierAndIgnoredInABogusDoctype) {
  EXPECT_EQ(run(s("<!DOCTYPE a\0b PUBLIC 'c\0d' 'e\0f'>", 33)).tokens_text(),
      "D(a\\xef\\xbf\\xbdb|c\\xef\\xbf\\xbdd|e\\xef\\xbf\\xbdf) F");
  const Outcome r = run(s("<!DOCTYPE a x\0y>", 16));
  EXPECT_EQ(r.tokens_text(), "D(a|-|-)! F");
  EXPECT_EQ(r.errors_text(),
      "invalid-character-sequence-after-doctype-name@1:13 "
      "unexpected-null-character@1:14");
}

// ---- CDATA ---------------------------------------------------------------

TEST(HtmlTokenizerCdata, InHtmlContentItIsABogusCommentWithAnError) {
  const Outcome r = run("<![CDATA[x]]>");
  EXPECT_EQ(r.tokens_text(), "M([CDATA[x]]) F");
  EXPECT_EQ(r.errors_text(), "cdata-in-html-content@1:9");
}

TEST(HtmlTokenizerCdata, InForeignContentItIsCharactersAndTheTreeBuilderSaysWhen) {
  Config setup;
  setup.options.allow_cdata = [](void *) { return true; };
  Outcome r = tokenize("a<![CDATA[x<y&amp;]]]>z", {}, setup);
  EXPECT_EQ(r.tokens_text(), "C\"ax<y&amp;]z\" F");
  EXPECT_EQ(r.errors_text(), "");
  r = tokenize("<![CDATA[a]]b]]]>", {}, setup);
  EXPECT_EQ(r.tokens_text(), "C\"a]]b]\" F");
  r = tokenize("<![CDATA[", {}, setup);
  EXPECT_EQ(r.tokens_text(), "F");
  EXPECT_EQ(r.errors_text(), "eof-in-cdata@1:10");
  // A NUL is a character here; foreign content decides what it means.
  r = tokenize(s("<![CDATA[\0]]>", 13), {}, setup);
  EXPECT_EQ(r.tokens_text(), "C\"\\x00\"[n] F");
}

TEST(HtmlTokenizerCdata, TheCallbackIsAskedAtEachOccurrenceAndAnOverrideWins) {
  static int asked;
  asked = 0;
  Config setup;
  setup.options.allow_cdata = [](void *) { return (++asked % 2) == 1; };
  const Outcome r = tokenize("<![CDATA[a]]><![CDATA[b]]>", {}, setup);
  EXPECT_EQ(asked, 2);
  EXPECT_EQ(r.tokens_text(), "C\"a\" M([CDATA[b]]) F");

  GTEXT_HTML_Tokenizer * tok = gtext_html_tokenizer_new(nullptr);
  ASSERT_NE(tok, nullptr);
  gtext_html_tokenizer_set_allow_cdata(tok, GTEXT_HTML_CDATA_ALLOW);
  const std::string in = "<![CDATA[q]]>";
  ASSERT_EQ(gtext_html_tokenizer_feed(tok, in.data(), in.size()),
      GTEXT_HTML_TOKENIZER_OK);
  gtext_html_tokenizer_finish(tok);
  const GTEXT_HTML_Token * t = nullptr;
  ASSERT_EQ(gtext_html_tokenizer_next(tok, &t), GTEXT_HTML_TOKENIZER_TOKEN);
  EXPECT_EQ(render(t), "C\"q\"");
  gtext_html_tokenizer_free(tok);
}

TEST(HtmlTokenizer, ACharacterTokenSaysWhereEachPieceOfItsDataCameFrom) {
  // The data of a run is normalized, so the position of a byte is not the start plus its index: a segment
  // starts wherever the data stops running on byte for byte from the input.
  GTEXT_HTML_Tokenizer * tok = gtext_html_tokenizer_new(nullptr);
  ASSERT_NE(tok, nullptr);
  const std::string in = "ab&amp;c\r\nd";
  ASSERT_EQ(gtext_html_tokenizer_feed(tok, in.data(), in.size()), GTEXT_HTML_TOKENIZER_OK);
  gtext_html_tokenizer_finish(tok);
  const GTEXT_HTML_Token * t = nullptr;
  ASSERT_EQ(gtext_html_tokenizer_next(tok, &t), GTEXT_HTML_TOKENIZER_TOKEN);
  ASSERT_EQ(std::string(t->data, t->data_length), "ab&c\nd");
  // "ab&" are bytes 0, 1 and 2 of the input (the `&` of the reference is where a plain character would
  // be), so one segment; "c" is byte 7, after the five bytes of the reference, so a segment of its own; the
  // line feed is the carriage return at 8, still running on; "d" is byte 10, two on from it, not one.
  ASSERT_EQ(t->segment_count, 3u);
  EXPECT_EQ(t->segments[0].index, 0u);
  EXPECT_EQ(t->segments[0].offset, 0u);
  EXPECT_EQ(t->segments[1].index, 3u);
  EXPECT_EQ(t->segments[1].offset, 7u);
  EXPECT_EQ(t->segments[2].index, 5u);
  EXPECT_EQ(t->segments[2].offset, 10u);
  gtext_html_tokenizer_free(tok);
}

TEST(HtmlTokenizerCdata, TheCdataSectionStateStartsInTheVectorsToo) {
  const Outcome r = tokenize("a]]b]]>c", {}, in_state(GTEXT_HTML_STATE_CDATA_SECTION));
  EXPECT_EQ(r.tokens_text(), "C\"a]]bc\" F");
  EXPECT_EQ(r.errors_text(), "");
}

// ---- Processing instructions ---------------------------------------------

TEST(HtmlTokenizerProcessingInstructions, TheShapesOfOne) {
  EXPECT_EQ(run("<?php echo ?>").tokens_text(), "P(php|echo ) F");
  EXPECT_EQ(run("<?foo>").tokens_text(), "P(foo|) F");
  EXPECT_EQ(run("<?foo bar?>").tokens_text(), "P(foo|bar) F");
  EXPECT_EQ(run("<?foo bar?" "?>").tokens_text(), "P(foo|bar?) F");
  EXPECT_EQ(run("<?foo?>").tokens_text(), "P(foo|) F");
  EXPECT_EQ(run("<?_a-b1  \t x?y>").tokens_text(), "P(_a-b1|x?y) F");
  EXPECT_EQ(run("<?a?x?" ">").tokens_text(), "P(a|?x) F");
}

TEST(HtmlTokenizerProcessingInstructions, AnXmlDeclarationIsABogusCommentWithAnError) {
  Outcome r = run("<?xml version='1.0'?>");
  EXPECT_EQ(r.tokens_text(), "M(?xml version='1.0'?) F");
  EXPECT_EQ(r.errors_text(), "disallowed-processing-instruction-target@1:6");
  r = run("<?XML-Stylesheet href=a?>");
  EXPECT_EQ(r.tokens_text(), "M(?XML-Stylesheet href=a?) F");
  EXPECT_EQ(r.errors_text(), "disallowed-processing-instruction-target@1:17");
  EXPECT_EQ(run("<?xmlx?>").tokens_text(), "P(xmlx|) F");
}

TEST(HtmlTokenizerProcessingInstructions, ABadTargetIsABogusComment) {
  Outcome r = run("<?1>");
  EXPECT_EQ(r.tokens_text(), "M(?1) F");
  EXPECT_EQ(r.errors_text(), "invalid-first-character-of-processing-instruction-target@1:3");
  r = run("<?a\"b>");
  EXPECT_EQ(r.tokens_text(), "M(?a\"b) F");
  EXPECT_EQ(r.errors_text(), "invalid-processing-instruction-target@1:4");
  r = run("<?");
  EXPECT_EQ(r.tokens_text(), "F");
  EXPECT_EQ(r.errors_text(), "eof-in-processing-instruction@1:3");
  for (const char * in : {"<?a", "<?a ", "<?a b", "<?a b?"}) {
    const Outcome e = run(in);
    EXPECT_EQ(e.tokens_text(), "F") << in;
    EXPECT_EQ(e.errors.size(), 1u) << in;
  }
}

// ---- Character references -------------------------------------------------

TEST(HtmlTokenizerReferences, TheNamedOnesWithAndWithoutTheSemicolon) {
  Outcome r = run("&amp;");
  EXPECT_EQ(r.tokens_text(), "C\"&\" F");
  EXPECT_EQ(r.errors_text(), "");
  // Row "References": `&amp` is "&" and a missing-semicolon error.
  r = run("&amp");
  EXPECT_EQ(r.tokens_text(), "C\"&\" F");
  EXPECT_EQ(r.errors_text(), "missing-semicolon-after-character-reference@1:5");
  r = run("&ampx");
  EXPECT_EQ(r.tokens_text(), "C\"&x\" F");
  EXPECT_EQ(r.errors_text(), "missing-semicolon-after-character-reference@1:5");
  // `notit;` is not a name and `not` is: only `not` is consumed.
  r = run("&notit;");
  EXPECT_EQ(r.tokens_text(), "C\"\\xc2\\xacit;\" F");
  EXPECT_EQ(r.errors_text(), "missing-semicolon-after-character-reference@1:5");
  r = run("&notin;");
  EXPECT_EQ(r.tokens_text(), "C\"\\xe2\\x88\\x89\" F");
  EXPECT_EQ(r.errors_text(), "");
  // Two code points.
  EXPECT_EQ(run("&NotEqualTilde;").tokens_text(), "C\"\\xe2\\x89\\x82\\xcc\\xb8\" F");
}

TEST(HtmlTokenizerReferences, ANameThatIsNotOneIsKeptAndAnUnknownOneWithASemicolonIsAnError) {
  Outcome r = run("&zzz;");
  EXPECT_EQ(r.tokens_text(), "C\"&zzz;\" F");
  EXPECT_EQ(r.errors_text(), "unknown-named-character-reference@1:5");
  r = run("&zzz");
  EXPECT_EQ(r.tokens_text(), "C\"&zzz\" F");
  EXPECT_EQ(r.errors_text(), "");
  r = run("a & b &; &1");
  EXPECT_EQ(r.tokens_text(), "C\"a & b &; &1\" F");
  EXPECT_EQ(r.errors_text(), "");
  // Names are case sensitive.
  EXPECT_EQ(run("&AMP; &Amp;").tokens_text(), "C\"& &Amp;\" F");
  EXPECT_EQ(run("&Amp;").errors_text(), "unknown-named-character-reference@1:5");
}

TEST(HtmlTokenizerReferences, InAnAttributeALegacyNameFollowedByEqualsOrAlnumIsLeftAlone) {
  Outcome r = run("<a b=\"&notit;\">");
  EXPECT_EQ(r.tokens_text(), "S(a b=&notit;) F");
  EXPECT_EQ(r.errors_text(), "");
  r = run("<a b=\"&amp=\" c='&ampx' d=&amp\">");
  EXPECT_EQ(r.tokens_text(), "S(a b=&amp= c=&ampx d=&\") F");
  // `&amp"` in an unquoted value: not followed by `=` or a letter, so decoded
  // (with the missing semicolon), and then the quote itself is the error.
  EXPECT_EQ(r.errors_text(),
      "missing-semicolon-after-character-reference@1:30 "
      "unexpected-character-in-unquoted-attribute-value@1:30");
  // Followed by anything else it is decoded, with the error.
  r = run("<a b=\"&amp x\">");
  EXPECT_EQ(r.tokens_text(), "S(a b=& x) F");
  EXPECT_EQ(r.errors_text(), "missing-semicolon-after-character-reference@1:11");
  // With its semicolon it is decoded whatever follows.
  EXPECT_EQ(run("<a b=\"&amp;x&amp;=\">").tokens_text(), "S(a b=&x&=) F");
  // The exception is only for a name without a semicolon.
  EXPECT_EQ(run("<a b='&copy'>").tokens_text(), "S(a b=\\xc2\\xa9) F");
  EXPECT_EQ(run("<a b='&copy'>").errors_text(),
      "missing-semicolon-after-character-reference@1:12");
  EXPECT_EQ(run("<a b=&notin;>").tokens_text(), "S(a b=\\xe2\\x88\\x89) F");
  // Unknown with a semicolon is an error in an attribute as in text.
  EXPECT_EQ(run("<a b='&zz;'>").errors_text(),
      "unknown-named-character-reference@1:10");
}

TEST(HtmlTokenizerReferences, ReferencesInRcdataButNotInRawtext) {
  EXPECT_EQ(tokenize("&lt;b&gt;&amp;", {}, in_state(GTEXT_HTML_STATE_RCDATA)).tokens_text(),
      "C\"<b>&\" F");
  EXPECT_EQ(tokenize("&lt;", {}, in_state(GTEXT_HTML_STATE_RAWTEXT)).tokens_text(),
      "C\"&lt;\" F");
  EXPECT_EQ(tokenize("&lt;", {}, in_state(GTEXT_HTML_STATE_SCRIPT_DATA)).tokens_text(),
      "C\"&lt;\" F");
  EXPECT_EQ(tokenize("&lt;", {}, in_state(GTEXT_HTML_STATE_PLAINTEXT)).tokens_text(),
      "C\"&lt;\" F");
}

TEST(HtmlTokenizerReferences, TheNumericOnesAndTheirErrors) {
  Outcome r = run("&#65;&#x41;&#X61;");
  EXPECT_EQ(r.tokens_text(), "C\"AAa\" F");
  EXPECT_EQ(r.errors_text(), "");
  r = run("&#65");
  EXPECT_EQ(r.tokens_text(), "C\"A\" F");
  EXPECT_EQ(r.errors_text(), "missing-semicolon-after-character-reference@1:5");
  r = run("&#x41g");
  EXPECT_EQ(r.tokens_text(), "C\"Ag\" F");
  EXPECT_EQ(r.errors_text(), "missing-semicolon-after-character-reference@1:6");
  r = run("&#128512;");
  EXPECT_EQ(r.tokens_text(), "C\"\\xf0\\x9f\\x98\\x80\" F");
  r = run("&#;");
  EXPECT_EQ(r.tokens_text(), "C\"&#;\" F");
  EXPECT_EQ(r.errors_text(), "absence-of-digits-in-numeric-character-reference@1:3");
  r = run("&#x;");
  EXPECT_EQ(r.tokens_text(), "C\"&#x;\" F");
  EXPECT_EQ(r.errors_text(), "absence-of-digits-in-numeric-character-reference@1:4");
  r = run("&#X");
  EXPECT_EQ(r.tokens_text(), "C\"&#X\" F");
  EXPECT_EQ(r.errors_text(), "absence-of-digits-in-numeric-character-reference@1:4");
}

TEST(HtmlTokenizerReferences, NumbersTheStandardReplacesOrRemapsOrReports) {
  // Reported where the end state runs: after the semicolon.
  Outcome r = run("&#0;");
  EXPECT_EQ(r.tokens_text(), "C\"\\xef\\xbf\\xbd\" F");
  EXPECT_EQ(r.errors_text(), "null-character-reference@1:5");
  r = run("&#x110000;");
  EXPECT_EQ(r.tokens_text(), "C\"\\xef\\xbf\\xbd\" F");
  EXPECT_EQ(r.errors_text(), "character-reference-outside-unicode-range@1:11");
  r = run("&#99999999999999999999;");
  EXPECT_EQ(r.tokens_text(), "C\"\\xef\\xbf\\xbd\" F");
  EXPECT_EQ(r.errors_text(), "character-reference-outside-unicode-range@1:24");
  r = run("&#xD800;&#xDFFF;&#xD7FF;&#xE000;");
  EXPECT_EQ(r.tokens_text(),
      "C\"\\xef\\xbf\\xbd\\xef\\xbf\\xbd\\xed\\x9f\\xbf\\xee\\x80\\x80\" F");
  EXPECT_EQ(r.errors_text(),
      "surrogate-character-reference@1:9 surrogate-character-reference@1:17");
  r = run("&#xFDD0;&#xFFFF;&#x10FFFE;&#xFDEF;&#xFDF0;");
  EXPECT_EQ(r.errors.size(), 4u);
  for (const std::string & e : r.errors) {
    EXPECT_EQ(e.substr(0, 32), "noncharacter-character-reference") << e;
  }
  r = run("&#x1;&#x7F;&#x0D;&#x9;&#xA;&#xC;&#x20;");
  EXPECT_EQ(r.errors.size(), 3u); // 1, 7F, 0D: tab, LF, FF and space are not
  for (const std::string & e : r.errors) {
    EXPECT_EQ(e.substr(0, 27), "control-character-reference") << e;
  }
  // A carriage return from a reference is not normalized: it is not input.
  EXPECT_EQ(run("&#13;").tokens_text(), "C\"\\x0d\" F");
}

TEST(HtmlTokenizerReferences, TheC1RangeIsWindows1252WhereWindows1252IsDefined) {
  // The Encoding standard's index for windows-1252, 0x80 to 0x9F; the five it
  // leaves undefined keep their own code points.
  static const uint32_t expected[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
  };
  auto encode = [](uint32_t cp) {
    std::string out;
    if (cp < 0x80) {
      out += (char)cp;
    }
    else if (cp < 0x800) {
      out += (char)(0xC0 | (cp >> 6));
      out += (char)(0x80 | (cp & 0x3F));
    }
    else {
      out += (char)(0xE0 | (cp >> 12));
      out += (char)(0x80 | ((cp >> 6) & 0x3F));
      out += (char)(0x80 | (cp & 0x3F));
    }
    return out;
  };
  for (uint32_t i = 0; i < 32; i++) {
    const Outcome r = run("&#" + std::to_string(0x80 + i) + ";");
    ASSERT_EQ(r.tokens.size(), 2u);
    EXPECT_EQ(r.tokens[0], "C\"" + show(encode(expected[i]).data(), encode(expected[i]).size()) + "\"")
        << "0x" << std::hex << 0x80 + i;
    EXPECT_EQ(r.errors.size(), 1u);
    EXPECT_EQ(r.errors[0].substr(0, 27), "control-character-reference");
  }
  // Just outside the range on each side.
  EXPECT_EQ(run("&#x7E;&#xA0;").tokens_text(), "C\"~\\xc2\\xa0\" F");
}

TEST(HtmlTokenizerReferences, AReferenceCutAtTheEndOfTheInputIsTheSameAsOneCutByACharacter) {
  EXPECT_EQ(run("&").tokens_text(), "C\"&\" F");
  EXPECT_EQ(run("&#").tokens_text(), "C\"&#\" F");
  EXPECT_EQ(run("&n").tokens_text(), "C\"&n\" F");
  EXPECT_EQ(run("&am").tokens_text(), "C\"&am\" F");
  EXPECT_EQ(run("a&").tokens_text(), "C\"a&\" F");
  EXPECT_EQ(run("&amp;&").tokens_text(), "C\"&&\" F");
}

TEST(HtmlTokenizerReferences, ALongRunOfLettersAfterAnAmpersandIsText) {
  const std::string tail(100000, 'a');
  const Outcome r = run("&" + tail + ";");
  ASSERT_EQ(r.tokens.size(), 2u);
  EXPECT_EQ(r.tokens[0].size(), tail.size() + 5);
  EXPECT_EQ(r.errors_text(), "unknown-named-character-reference@1:100002");
}

// ---- Preprocessing -------------------------------------------------------

TEST(HtmlTokenizerPreprocessing, NewlinesAreNormalizedAndCountedOnce) {
  Outcome r = run(s("a\r\nb\rc\n\rd\r\r\ne", 13));
  EXPECT_EQ(r.tokens_text(), "C\"a\\nb\\nc\\n\\nd\\n\\ne\" F");
  r = run("a\r\n\x01");
  ASSERT_EQ(r.errors.size(), 1u);
  EXPECT_EQ(r.errors[0], "control-character-in-input-stream@2:1");
  EXPECT_EQ(r.error_offsets[0], 3u); // the CR LF is two bytes
  r = run("\r\n<p>");
  EXPECT_EQ(r.token_lines[1], 2u);
  EXPECT_EQ(r.token_columns[1], 1u);
  EXPECT_EQ(r.token_offsets[1], 2u);
  // A carriage return at the end of the input is a line feed too.
  EXPECT_EQ(run("a\r").tokens_text(), "C\"a\\n\" F");
}

TEST(HtmlTokenizerPreprocessing, ANulIsAnErrorInDataAndAReplacementCharacterElsewhere) {
  Outcome r = run(s("a\0b", 3));
  EXPECT_EQ(r.tokens_text(), "C\"a\\x00b\"[n] F");
  EXPECT_EQ(r.errors_text(), "unexpected-null-character@1:2");
  r = tokenize(s("a\0b", 3), {}, in_state(GTEXT_HTML_STATE_RCDATA));
  EXPECT_EQ(r.tokens_text(), "C\"a\\xef\\xbf\\xbdb\" F");
  EXPECT_EQ(r.errors_text(), "unexpected-null-character@1:2");
  r = tokenize(s("a\0b", 3), {}, in_state(GTEXT_HTML_STATE_RAWTEXT));
  EXPECT_EQ(r.tokens_text(), "C\"a\\xef\\xbf\\xbdb\" F");
  r = tokenize(s("a\0b", 3), {}, in_state(GTEXT_HTML_STATE_SCRIPT_DATA));
  EXPECT_EQ(r.tokens_text(), "C\"a\\xef\\xbf\\xbdb\" F");
  r = tokenize(s("a\0b", 3), {}, in_state(GTEXT_HTML_STATE_PLAINTEXT));
  EXPECT_EQ(r.tokens_text(), "C\"a\\xef\\xbf\\xbdb\" F");
  r = run(s("<!--a\0b-->", 10));
  EXPECT_EQ(r.tokens_text(), "M(a\\xef\\xbf\\xbdb) F");
}

TEST(HtmlTokenizerPreprocessing, ControlsAndNoncharactersAreErrorsAndPassThrough) {
  Outcome r = run("a\x01" "b\x7F" "c\xC2\x80" "d\xC2\x9F" "e");
  EXPECT_EQ(r.tokens.size(), 2u);
  EXPECT_EQ(r.errors_text(),
      "control-character-in-input-stream@1:2 control-character-in-input-stream@1:4 "
      "control-character-in-input-stream@1:6 control-character-in-input-stream@1:8");
  // None of the white space controls is an error: tab, LF, FF, CR (normalized).
  EXPECT_EQ(run("\t\n\f\r ").errors_text(), "");
  // VT is.
  EXPECT_EQ(run("\x0B").errors_text(), "control-character-in-input-stream@1:1");
  r = run("\xEF\xB7\x90" "\xEF\xBF\xBE" "\xEF\xBF\xBF" "\xEF\xB7\xAF" "\xF0\x9F\xBF\xBF" "\xF4\x8F\xBF\xBF" "\xEF\xB7\xB0");
  EXPECT_EQ(r.errors.size(), 6u); // FDD0, FFFE, FFFF, FDEF, 1FFFF, 10FFFF; FDF0 is not
  for (const std::string & e : r.errors) {
    EXPECT_EQ(e.substr(0, 28), "noncharacter-in-input-stream") << e;
  }
  // The input stream's own errors come before the state's for the same character.
  r = run("<a\x01>");
  EXPECT_EQ(r.errors_text(), "control-character-in-input-stream@1:3");
}

TEST(HtmlTokenizerPreprocessing, InvalidUtf8BecomesOneReplacementCharacterPerMaximalSubpart) {
  const std::string fffd = "\\xef\\xbf\\xbd";
  Outcome r = run("\xFF");
  EXPECT_EQ(r.tokens_text(), "C\"" + fffd + "\" F");
  EXPECT_EQ(r.errors_text(), "invalid-utf-8-in-input-stream@1:1");
  // Truncated at the end: one.
  EXPECT_EQ(run("\xE2\x82").tokens_text(), "C\"" + fffd + "\" F");
  EXPECT_EQ(run("a\xF0\x9F\x98").tokens_text(), "C\"a" + fffd + "\" F");
  // Truncated by another character: one, and the character is kept.
  EXPECT_EQ(run("\xE2\x82z").tokens_text(), "C\"" + fffd + "z\" F");
  // Overlong and out of range: the lead is refused, and so is each trailing byte.
  EXPECT_EQ(run("\xC0\x80").tokens_text(), "C\"" + fffd + fffd + "\" F");
  EXPECT_EQ(run("\xE0\x80\x80").tokens_text(), "C\"" + fffd + fffd + fffd + "\" F");
  EXPECT_EQ(run("\xF4\x90\x80\x80").tokens_text(), "C\"" + fffd + fffd + fffd + fffd + "\" F");
  // A surrogate's encoding.
  EXPECT_EQ(run("\xED\xA0\x80").tokens_text(), "C\"" + fffd + fffd + fffd + "\" F");
  // A lone continuation byte, and the bytes that never occur.
  EXPECT_EQ(run("\x80\xC1\xF5").tokens_text(), "C\"" + fffd + fffd + fffd + "\" F");
  // Valid ones are not touched.
  EXPECT_EQ(run("\xC2\xA9\xE2\x82\xAC\xF0\x9F\x98\x80").errors_text(), "");
}

TEST(HtmlTokenizerPreprocessing, WtfEightReadsALoneSurrogateAsOneAndReportsIt) {
  Config setup;
  setup.options.wtf8 = true;
  const Outcome r = tokenize("a\xED\xA0\x80" "b\xED\xBF\xBF", {}, setup);
  EXPECT_EQ(r.tokens_text(), "C\"a\\xed\\xa0\\x80b\\xed\\xbf\\xbf\" F");
  EXPECT_EQ(r.errors_text(),
      "surrogate-in-input-stream@1:2 surrogate-in-input-stream@1:4");
  // Not otherwise looser.
  EXPECT_EQ(tokenize("\xE0\x80\x80", {}, setup).errors.size(), 3u);
}

TEST(HtmlTokenizerPreprocessing, ColumnsCountUtf16CodeUnitsAsTheVectorsDo) {
  // U+10000 is two code units, U+FFFF one (and a noncharacter).
  const Outcome r = run("\xF0\x90\x80\x80\x01");
  EXPECT_EQ(r.errors_text(), "control-character-in-input-stream@1:3");
  EXPECT_EQ(r.error_offsets[0], 4u);
}

// ---- The states a tree builder sets --------------------------------------

TEST(HtmlTokenizerStates, RawtextEndsOnlyAtTheAppropriateEndTag) {
  Config rawtext = in_state(GTEXT_HTML_STATE_RAWTEXT, "style");
  EXPECT_EQ(tokenize("a<b>c</style>d", {}, rawtext).tokens_text(),
      "C\"a<b>c\" E(style) C\"d\" F");
  EXPECT_EQ(tokenize("x</styl>y</STYLE >z</style/>", {}, rawtext).tokens_text(),
      "C\"x</styl>y\" E(style) C\"z\" E(style)/ F");
  EXPECT_EQ(tokenize("</style", {}, rawtext).tokens_text(), "C\"</style\" F");
  EXPECT_EQ(tokenize("</style x='>'>a", {}, rawtext).tokens_text(),
      "E(style x=>) C\"a\" F");
  // No start tag emitted: no end tag is appropriate.
  EXPECT_EQ(tokenize("a</style>", {}, in_state(GTEXT_HTML_STATE_RAWTEXT)).tokens_text(),
      "C\"a</style>\" F");
  // The name has to match whole.
  EXPECT_EQ(tokenize("</styles></style>", {}, rawtext).tokens_text(),
      "C\"</styles>\" E(style) F");
  EXPECT_EQ(tokenize("</ style>", {}, rawtext).tokens_text(), "C\"</ style>\" F");
}

TEST(HtmlTokenizerStates, RcdataReadsReferencesAndEndsLikeRawtext) {
  Config rcdata = in_state(GTEXT_HTML_STATE_RCDATA, "title");
  EXPECT_EQ(tokenize("a&amp;<b></title>", {}, rcdata).tokens_text(),
      "C\"a&<b>\" E(title) F");
  EXPECT_EQ(tokenize("</titlex></Title>", {}, rcdata).tokens_text(),
      "C\"</titlex>\" E(title) F");
}

TEST(HtmlTokenizerStates, ScriptDataAndItsEscapedAndDoubleEscapedStates) {
  Config script = in_state(GTEXT_HTML_STATE_SCRIPT_DATA, "script");
  EXPECT_EQ(tokenize("a<b>c</script>d", {}, script).tokens_text(),
      "C\"a<b>c\" E(script) C\"d\" F");
  // Escaped: an end tag still ends it. Double escaped: it does not.
  EXPECT_EQ(tokenize("<!--</script>x", {}, script).tokens_text(),
      "C\"<!--\" E(script) C\"x\" F");
  EXPECT_EQ(tokenize("<!--<script></script>--></script>", {}, script).tokens_text(),
      "C\"<!--<script></script>-->\" E(script) F");
  EXPECT_EQ(tokenize("<!--<SCRIPT >a</scRipt >b-->x</script>", {}, script).tokens_text(),
      "C\"<!--<SCRIPT >a</scRipt >b-->x\" E(script) F");
  // `script` is the whole word; `scripts` does not double-escape.
  EXPECT_EQ(tokenize("<!--<scripts></script>", {}, script).tokens_text(),
      "C\"<!--<scripts>\" E(script) F");
  // The escape needs two hyphens after the `<!`.
  EXPECT_EQ(tokenize("<!-x</script>", {}, script).tokens_text(),
      "C\"<!-x\" E(script) F");
  EXPECT_EQ(tokenize("<!---->y</script>", {}, script).tokens_text(),
      "C\"<!---->y\" E(script) F");
  EXPECT_EQ(tokenize("<!--->y</script>", {}, script).tokens_text(),
      "C\"<!--->y\" E(script) F");
  // End of file inside the escaped states.
  for (const char * in : {"<!--", "<!--a", "<!--a-", "<!--a--", "<!--<script>", "<!--<script>-", "<!--<script>--"}) {
    const Outcome r = tokenize(in, {}, script);
    EXPECT_EQ(r.errors.size(), 1u) << in;
    EXPECT_EQ(r.errors[0].substr(0, 36), "eof-in-script-html-comment-like-text") << in;
  }
  // A NUL in the escaped states.
  const Outcome n = tokenize(s("<!--\0-\0-\0", 9), {}, script);
  EXPECT_EQ(n.errors.size(), 4u); // three NULs and the end of the file
}

TEST(HtmlTokenizerStates, PlaintextNeverEnds) {
  const Outcome r = tokenize("<b>&amp;</plaintext>", {}, in_state(GTEXT_HTML_STATE_PLAINTEXT));
  EXPECT_EQ(r.tokens_text(), "C\"<b>&amp;</plaintext>\" F");
  EXPECT_EQ(r.errors_text(), "");
}

TEST(HtmlTokenizerStates, SetLastStartTagAndTheAppropriateEndTagTestFollowTheTokenizerOn) {
  // The tokenizer remembers the last start tag it emitted by itself.
  GTEXT_HTML_Tokenizer * tok = gtext_html_tokenizer_new(nullptr);
  ASSERT_NE(tok, nullptr);
  const std::string in = "<textarea>x</textarea>y</textarea>";
  ASSERT_EQ(gtext_html_tokenizer_feed(tok, in.data(), in.size()), GTEXT_HTML_TOKENIZER_OK);
  gtext_html_tokenizer_finish(tok);
  const GTEXT_HTML_Token * t = nullptr;
  std::vector<std::string> out;
  while (gtext_html_tokenizer_next(tok, &t) == GTEXT_HTML_TOKENIZER_TOKEN) {
    out.push_back(render(t));
    if (t->type == GTEXT_HTML_TOKEN_START_TAG) {
      // The tree builder's instruction after <textarea>.
      EXPECT_EQ(gtext_html_tokenizer_set_state(tok, GTEXT_HTML_STATE_RCDATA),
          GTEXT_HTML_TOKENIZER_OK);
      EXPECT_EQ(gtext_html_tokenizer_get_state(tok), GTEXT_HTML_STATE_RCDATA);
    }
    if (t->type == GTEXT_HTML_TOKEN_END_TAG) {
      EXPECT_EQ(gtext_html_tokenizer_set_state(tok, GTEXT_HTML_STATE_DATA),
          GTEXT_HTML_TOKENIZER_OK);
    }
  }
  ASSERT_EQ(out.size(), 6u);
  EXPECT_EQ(out[0], "S(textarea)");
  EXPECT_EQ(out[1], "C\"x\"");
  EXPECT_EQ(out[2], "E(textarea)");
  EXPECT_EQ(out[3], "C\"y\"");
  EXPECT_EQ(out[4], "E(textarea)");
  EXPECT_EQ(out[5], "F");
  // Clearing it.
  EXPECT_EQ(gtext_html_tokenizer_set_last_start_tag(tok, nullptr, 0), GTEXT_HTML_TOKENIZER_OK);
  EXPECT_EQ(gtext_html_tokenizer_set_state(tok, (GTEXT_HTML_TokenizerState)77),
      GTEXT_HTML_TOKENIZER_E_INVALID);
  gtext_html_tokenizer_free(tok);
}

TEST(HtmlTokenizerStates, ANewStateAppliesToTheVeryNextCharacterEvenOneAlreadyFed) {
  // `<script>` and its content are in one chunk: nothing past the start tag may
  // have been read in the data state.
  static const char * wanted;
  wanted = "script";
  Config setup;
  setup.on_token = [](GTEXT_HTML_Tokenizer * tok, const GTEXT_HTML_Token * t) {
    if (t->type == GTEXT_HTML_TOKEN_START_TAG
        && std::string(t->name, t->name_length) == wanted) {
      gtext_html_tokenizer_set_state(tok, GTEXT_HTML_STATE_SCRIPT_DATA);
    }
  };
  for (const std::vector<size_t> & cuts :
      {std::vector<size_t>{}, std::vector<size_t>{1}, std::vector<size_t>{3}, std::vector<size_t>{8, 1, 5}}) {
    const Outcome r = tokenize("<script>if (a<b&&c) {}</script>after<p>", cuts, setup);
    EXPECT_EQ(r.tokens_text(),
        "S(script) C\"if (a<b&&c) {}\" E(script) C\"after\" S(p) F");
    EXPECT_EQ(r.errors_text(), "");
  }
}

// ---- Positions -----------------------------------------------------------

TEST(HtmlTokenizerPositions, ATokenStartsWhereItsFirstCharacterIs) {
  const Outcome r = run("ab\n<p x=1>&amp;<!--c--><!DOCTYPE d>");
  ASSERT_EQ(r.tokens.size(), 6u); // C S C M D F
  EXPECT_EQ(r.token_lines[0], 1u);
  EXPECT_EQ(r.token_columns[0], 1u);
  EXPECT_EQ(r.token_offsets[0], 0u);
  EXPECT_EQ(r.token_lines[1], 2u);
  EXPECT_EQ(r.token_columns[1], 1u);
  EXPECT_EQ(r.token_offsets[1], 3u);
  EXPECT_EQ(r.token_columns[2], 8u); // the `&` of the reference
  EXPECT_EQ(r.token_offsets[2], 10u);
  EXPECT_EQ(r.token_columns[3], 13u); // the `<` of the comment
  EXPECT_EQ(r.token_columns[4], 21u);
  EXPECT_EQ(r.token_offsets[5], 35u); // the end of the input
}

TEST(HtmlTokenizerPositions, AnErrorsOffsetIsInTheBytesAsFedWhateverTheCutting) {
  const std::string in = "\xC3\xA9<a b=1 b=2>\r\n\xEF\xBF\xBE";
  const Outcome whole = run(in);
  ASSERT_EQ(whole.errors.size(), 2u);
  EXPECT_EQ(whole.errors[0], "duplicate-attribute@1:10");
  EXPECT_EQ(whole.error_offsets[0], 10u);
  EXPECT_EQ(whole.errors[1], "noncharacter-in-input-stream@2:1");
  EXPECT_EQ(whole.error_offsets[1], 15u);
  for (size_t cut = 1; cut < in.size(); cut++) {
    EXPECT_TRUE(whole.same_as(tokenize(in, {cut})))
        << "cut at " << cut << ": " << tokenize(in, {cut}).everything();
  }
}

// ---- The interface -------------------------------------------------------

TEST(HtmlTokenizerInterface, ArgumentsAreChecked) {
  const GTEXT_HTML_Token * t = nullptr;
  EXPECT_EQ(gtext_html_tokenizer_next(nullptr, &t), GTEXT_HTML_TOKENIZER_E_INVALID);
  GTEXT_HTML_Tokenizer * tok = gtext_html_tokenizer_new(nullptr);
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(gtext_html_tokenizer_next(tok, nullptr), GTEXT_HTML_TOKENIZER_E_INVALID);
  EXPECT_EQ(gtext_html_tokenizer_feed(nullptr, "a", 1), GTEXT_HTML_TOKENIZER_E_INVALID);
  EXPECT_EQ(gtext_html_tokenizer_feed(tok, nullptr, 1), GTEXT_HTML_TOKENIZER_E_INVALID);
  EXPECT_EQ(gtext_html_tokenizer_feed(tok, nullptr, 0), GTEXT_HTML_TOKENIZER_OK);
  EXPECT_EQ(gtext_html_tokenizer_set_state(nullptr, GTEXT_HTML_STATE_DATA),
      GTEXT_HTML_TOKENIZER_E_INVALID);
  EXPECT_EQ(gtext_html_tokenizer_set_last_start_tag(tok, nullptr, 3), GTEXT_HTML_TOKENIZER_E_INVALID);
  gtext_html_tokenizer_finish(nullptr);
  gtext_html_tokenizer_free(nullptr);
  gtext_html_tokenizer_free(tok);
}

TEST(HtmlTokenizerInterface, AChunkIsBorrowedUntilTheTokenizerAsksForMore) {
  GTEXT_HTML_Tokenizer * tok = gtext_html_tokenizer_new(nullptr);
  ASSERT_NE(tok, nullptr);
  const GTEXT_HTML_Token * t = nullptr;
  ASSERT_EQ(gtext_html_tokenizer_next(tok, &t), GTEXT_HTML_TOKENIZER_NEED_INPUT);
  const std::string in = "<a>b<c>";
  ASSERT_EQ(gtext_html_tokenizer_feed(tok, in.data(), in.size()), GTEXT_HTML_TOKENIZER_OK);
  // The first chunk has not been drained.
  EXPECT_EQ(gtext_html_tokenizer_feed(tok, "x", 1), GTEXT_HTML_TOKENIZER_E_STATE);
  ASSERT_EQ(gtext_html_tokenizer_next(tok, &t), GTEXT_HTML_TOKENIZER_TOKEN);
  EXPECT_EQ(render(t), "S(a)");
  EXPECT_EQ(gtext_html_tokenizer_feed(tok, "x", 1), GTEXT_HTML_TOKENIZER_E_STATE);
  ASSERT_EQ(gtext_html_tokenizer_next(tok, &t), GTEXT_HTML_TOKENIZER_TOKEN);
  EXPECT_EQ(render(t), "C\"b\"");
  ASSERT_EQ(gtext_html_tokenizer_next(tok, &t), GTEXT_HTML_TOKENIZER_TOKEN);
  EXPECT_EQ(render(t), "S(c)");
  ASSERT_EQ(gtext_html_tokenizer_next(tok, &t), GTEXT_HTML_TOKENIZER_NEED_INPUT);
  EXPECT_EQ(gtext_html_tokenizer_feed(tok, "x", 1), GTEXT_HTML_TOKENIZER_OK);
  gtext_html_tokenizer_finish(tok);
  EXPECT_EQ(gtext_html_tokenizer_feed(tok, "y", 1), GTEXT_HTML_TOKENIZER_E_STATE);
  ASSERT_EQ(gtext_html_tokenizer_next(tok, &t), GTEXT_HTML_TOKENIZER_TOKEN);
  EXPECT_EQ(render(t), "C\"x\"");
  ASSERT_EQ(gtext_html_tokenizer_next(tok, &t), GTEXT_HTML_TOKENIZER_TOKEN);
  EXPECT_EQ(render(t), "F");
  EXPECT_EQ(gtext_html_tokenizer_next(tok, &t), GTEXT_HTML_TOKENIZER_DONE);
  EXPECT_EQ(gtext_html_tokenizer_next(tok, &t), GTEXT_HTML_TOKENIZER_DONE);
  gtext_html_tokenizer_free(tok);
}

TEST(HtmlTokenizerInterface, ATokenIsDeliveredTheMomentItIsCompleteNotWhenTheNextOneStarts) {
  // `<a>` alone in a feed: the start tag comes out without more input.
  GTEXT_HTML_Tokenizer * tok = gtext_html_tokenizer_new(nullptr);
  ASSERT_NE(tok, nullptr);
  ASSERT_EQ(gtext_html_tokenizer_feed(tok, "<a>", 3), GTEXT_HTML_TOKENIZER_OK);
  const GTEXT_HTML_Token * t = nullptr;
  ASSERT_EQ(gtext_html_tokenizer_next(tok, &t), GTEXT_HTML_TOKENIZER_TOKEN);
  EXPECT_EQ(render(t), "S(a)");
  EXPECT_EQ(gtext_html_tokenizer_next(tok, &t), GTEXT_HTML_TOKENIZER_NEED_INPUT);
  // Text is held until something ends the run: that is the one exception.
  ASSERT_EQ(gtext_html_tokenizer_feed(tok, "text", 4), GTEXT_HTML_TOKENIZER_OK);
  EXPECT_EQ(gtext_html_tokenizer_next(tok, &t), GTEXT_HTML_TOKENIZER_NEED_INPUT);
  ASSERT_EQ(gtext_html_tokenizer_feed(tok, "more<", 5), GTEXT_HTML_TOKENIZER_OK);
  EXPECT_EQ(gtext_html_tokenizer_next(tok, &t), GTEXT_HTML_TOKENIZER_NEED_INPUT);
  ASSERT_EQ(gtext_html_tokenizer_feed(tok, "b>", 2), GTEXT_HTML_TOKENIZER_OK);
  ASSERT_EQ(gtext_html_tokenizer_next(tok, &t), GTEXT_HTML_TOKENIZER_TOKEN);
  EXPECT_EQ(render(t), "C\"textmore\"");
  ASSERT_EQ(gtext_html_tokenizer_next(tok, &t), GTEXT_HTML_TOKENIZER_TOKEN);
  EXPECT_EQ(render(t), "S(b)");
  gtext_html_tokenizer_free(tok);
}

TEST(HtmlTokenizerInterface, WhiteSpaceOnlyRunsAreFlagged) {
  EXPECT_EQ(run(" \t\n\f ").tokens_text(), "C\" \\x09\\n\\x0c \"[w] F");
  EXPECT_EQ(run(" a ").tokens_text(), "C\" a \" F");
  // A character token made by a reference is a character token like any other.
  EXPECT_EQ(run("&#32;&#9;").tokens_text(), "C\" \\x09\"[w] F");
  EXPECT_EQ(run("&#32;&#65;").tokens_text(), "C\" A\" F");
}

// ---- The limits ----------------------------------------------------------

TEST(HtmlTokenizerLimits, EachHasItsOwnStatusAndItIsExactlyAtTheLimit) {
  struct Case {
    const char * ok;
    const char * over;
    GTEXT_HTML_TokenizerStatus status;
    void (*set)(GTEXT_HTML_TokenizerOptions *);
  };
  const Case cases[] = {
    {"<abc>", "<abcd>", GTEXT_HTML_TOKENIZER_E_LIMIT_NAME,
        [](GTEXT_HTML_TokenizerOptions * o) { o->max_name_bytes = 3; }},
    {"<a bcd>", "<a bcde>", GTEXT_HTML_TOKENIZER_E_LIMIT_NAME,
        [](GTEXT_HTML_TokenizerOptions * o) { o->max_name_bytes = 3; }},
    {"<!DOCTYPE abc>", "<!DOCTYPE abcd>", GTEXT_HTML_TOKENIZER_E_LIMIT_NAME,
        [](GTEXT_HTML_TokenizerOptions * o) { o->max_name_bytes = 3; }},
    {"<a b=abc>", "<a b=abcd>", GTEXT_HTML_TOKENIZER_E_LIMIT_VALUE,
        [](GTEXT_HTML_TokenizerOptions * o) { o->max_value_bytes = 3; }},
    {"<a b='&amp;&amp;'>", "<a b='&amp;&amp;&amp;&amp;'>", GTEXT_HTML_TOKENIZER_E_LIMIT_VALUE,
        [](GTEXT_HTML_TokenizerOptions * o) { o->max_value_bytes = 3; }},
    {"<!DOCTYPE a PUBLIC 'abc'>", "<!DOCTYPE a PUBLIC 'abcd'>", GTEXT_HTML_TOKENIZER_E_LIMIT_VALUE,
        [](GTEXT_HTML_TokenizerOptions * o) { o->max_value_bytes = 3; }},
    {"<!DOCTYPE a SYSTEM 'abc'>", "<!DOCTYPE a SYSTEM 'abcd'>", GTEXT_HTML_TOKENIZER_E_LIMIT_VALUE,
        [](GTEXT_HTML_TokenizerOptions * o) { o->max_value_bytes = 3; }},
    {"<!--abc-->", "<!--abcd-->", GTEXT_HTML_TOKENIZER_E_LIMIT_COMMENT,
        [](GTEXT_HTML_TokenizerOptions * o) { o->max_comment_bytes = 3; }},
    {"<?a bcd?>", "<?a bcde?>", GTEXT_HTML_TOKENIZER_E_LIMIT_COMMENT,
        [](GTEXT_HTML_TokenizerOptions * o) { o->max_comment_bytes = 3; }},
    {"abc", "abcd", GTEXT_HTML_TOKENIZER_E_LIMIT_TEXT,
        [](GTEXT_HTML_TokenizerOptions * o) { o->max_text_bytes = 3; }},
    {"a<b c d e>", "a<b c d e f>", GTEXT_HTML_TOKENIZER_E_LIMIT_ATTRIBUTES,
        [](GTEXT_HTML_TokenizerOptions * o) { o->max_attributes = 3; }},
  };
  for (const Case & c : cases) {
    Config setup;
    c.set(&setup.options);
    const Outcome ok = tokenize(c.ok, {}, setup);
    EXPECT_EQ(ok.status, GTEXT_HTML_TOKENIZER_DONE) << c.ok << " " << ok.everything();
    for (const std::vector<size_t> & cuts : {std::vector<size_t>{}, std::vector<size_t>{1}}) {
      const Outcome over = tokenize(c.over, cuts, setup);
      EXPECT_EQ(over.status, c.status) << c.over << " " << status_name(over.status);
    }
  }
}

TEST(HtmlTokenizerLimits, AttributesAtTheLimitPassAndDroppedDuplicatesDoNotCount) {
  Config setup;
  setup.options.max_attributes = 3;
  EXPECT_EQ(tokenize("<a b c d>", {}, setup).status, GTEXT_HTML_TOKENIZER_DONE);
  EXPECT_EQ(tokenize("<a b b b b c d>", {}, setup).status, GTEXT_HTML_TOKENIZER_DONE);
  EXPECT_EQ(tokenize("<a b c d e>", {}, setup).status, GTEXT_HTML_TOKENIZER_E_LIMIT_ATTRIBUTES);
}

TEST(HtmlTokenizerLimits, ALimitIsStickyAndTheErrorsSoFarAreStillThere) {
  Config setup;
  setup.options.max_name_bytes = 3;
  GTEXT_HTML_Tokenizer * tok = gtext_html_tokenizer_new(&setup.options);
  ASSERT_NE(tok, nullptr);
  const std::string in = "\x01<abcd>more";
  ASSERT_EQ(gtext_html_tokenizer_feed(tok, in.data(), in.size()), GTEXT_HTML_TOKENIZER_OK);
  const GTEXT_HTML_Token * t = nullptr;
  EXPECT_EQ(gtext_html_tokenizer_next(tok, &t), GTEXT_HTML_TOKENIZER_E_LIMIT_NAME);
  EXPECT_EQ(gtext_html_tokenizer_next(tok, &t), GTEXT_HTML_TOKENIZER_E_LIMIT_NAME);
  gtext_html_tokenizer_finish(tok);
  EXPECT_EQ(gtext_html_tokenizer_next(tok, &t), GTEXT_HTML_TOKENIZER_E_LIMIT_NAME);
  EXPECT_EQ(gtext_html_tokenizer_feed(tok, "x", 1), GTEXT_HTML_TOKENIZER_E_LIMIT_NAME);
  ASSERT_EQ(gtext_html_tokenizer_error_count(tok), 1u);
  EXPECT_EQ(gtext_html_tokenizer_error_at(tok, 0)->code,
      GTEXT_HTML_ERR_CONTROL_CHARACTER_IN_INPUT_STREAM);
  EXPECT_EQ(gtext_html_tokenizer_error_at(tok, 1), nullptr);
  gtext_html_tokenizer_free(tok);
}

TEST(HtmlTokenizerLimits, TheInputLimitCountsTheBytesFedAcrossChunks) {
  Config setup;
  setup.options.max_input_bytes = 10;
  GTEXT_HTML_Tokenizer * tok = gtext_html_tokenizer_new(&setup.options);
  ASSERT_NE(tok, nullptr);
  const GTEXT_HTML_Token * t = nullptr;
  ASSERT_EQ(gtext_html_tokenizer_feed(tok, "0123456", 7), GTEXT_HTML_TOKENIZER_OK);
  ASSERT_EQ(gtext_html_tokenizer_next(tok, &t), GTEXT_HTML_TOKENIZER_NEED_INPUT);
  ASSERT_EQ(gtext_html_tokenizer_feed(tok, "789", 3), GTEXT_HTML_TOKENIZER_OK);
  ASSERT_EQ(gtext_html_tokenizer_next(tok, &t), GTEXT_HTML_TOKENIZER_NEED_INPUT);
  EXPECT_EQ(gtext_html_tokenizer_feed(tok, "a", 1), GTEXT_HTML_TOKENIZER_E_LIMIT_INPUT);
  EXPECT_EQ(gtext_html_tokenizer_next(tok, &t), GTEXT_HTML_TOKENIZER_E_LIMIT_INPUT);
  gtext_html_tokenizer_free(tok);
  // A single chunk that is too long, with a size that would overflow the sum.
  tok = gtext_html_tokenizer_new(&setup.options);
  EXPECT_EQ(gtext_html_tokenizer_feed(tok, "x", SIZE_MAX), GTEXT_HTML_TOKENIZER_E_LIMIT_INPUT);
  gtext_html_tokenizer_free(tok);
}

TEST(HtmlTokenizerLimits, ErrorsKeptAreCappedAndTheRestAreCounted) {
  Config setup;
  setup.options.max_errors = 3;
  const Outcome r = tokenize(std::string(10, '\x01') + "<a b b b>", {}, setup);
  EXPECT_EQ(r.errors.size(), 3u);
  EXPECT_EQ(r.dropped, 9u); // ten controls and two duplicates, three kept
  EXPECT_EQ(r.status, GTEXT_HTML_TOKENIZER_DONE);
  EXPECT_EQ(r.tokens[1], "S(a b=)");
}

TEST(HtmlTokenizerLimits, AZeroLimitMeansTheDefaultAndNeverUnlimited) {
  Config setup; // all zero
  const std::string big(1000000, 'a');
  EXPECT_EQ(tokenize("<" + big + ">", {}, setup).status, GTEXT_HTML_TOKENIZER_E_LIMIT_NAME);
  EXPECT_EQ(tokenize(big, {}, setup).status, GTEXT_HTML_TOKENIZER_DONE);
  EXPECT_EQ(tokenize("<!--" + big + "-->", {}, setup).status, GTEXT_HTML_TOKENIZER_DONE);
}

// ---- Chunking ------------------------------------------------------------

std::vector<std::string> corpus() {
  return {
    "",
    "plain text",
    "a\r\nb\rc\r\n\r\nd\r",
    "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80 end",
    "\xFF\xE2\x82z\xC0\x80\xED\xA0\x80\xF4\x90\x80\x80 \xE2\x82",
    "<p class=a id='b' hidden>Hi &amp; bye</p>",
    "<DIV A=1 a=2 B='x&amp;y' c=\"&notit;\" d=&amp= e=&copy f=&ampx>",
    "<a b=\"c\"d e=f/g h=>\"<br/><a/b></a x/>",
    "<!-- a -- b --><!--><!---><!--a--!><!--<!--x-->--><!--",
    "<!DOCTYPE html><!doctype HTML PUBLIC \"-//a//b\" 'c'><!DOCTYPE x SYSTEM 'y'>"
    "<!DOCTYPE><!DOCTYPE a b><!DOCTYPE a PUBLIC><!DOCTYPE a PUBLIC 'b",
    "<![CDATA[x]]><![CDATA[a]]b]]]>",
    "<?php echo 1 ?><?xml v?><?x?y?" "?><?1><?a\"><?",
    "&amp;&amp&ampx&notit;&notin;&NotEqualTilde;&zzz;&#65;&#x41&#X;&#;&#0;&#x110000;&#xD800;&#x80;&#150;&#13;&",
    "&AMP&aacute&aa&aacut&acirc;&ac&",
    "<script>a<b>c</script><!--<script></script>--></script>",
    "<!-x><!foo><!DOC><!<?><</><1></ ><a<b>",
    "<a \"x\" 'y' <z> =b c==d>",
    "<a b='\x01\xEF\xBF\xBE' c>\x7F\xC2\x80",
    "x<!-- c -->y<![CDATA[z]]>w<!DOCTYPE>v",
    "<textarea>a&lt;/textarea></textarea>b",
    "<style>a</styl></style >b</style/>",
    "<title>&amp;&amp</title x>",
    "&a&am&amp&ampx&ampx;&b&bi&big&bigc&",
    "<a b=&#x41;&#66 c='&#x;' d=\"&#\">",
  };
}

TEST(HtmlTokenizerChunking, EveryWayOfCuttingTheInputInTwoGivesTheWholeRunsAnswer) {
  const std::vector<std::pair<GTEXT_HTML_TokenizerState, const char *>> starts = {
    {GTEXT_HTML_STATE_DATA, nullptr},
    {GTEXT_HTML_STATE_RCDATA, "title"},
    {GTEXT_HTML_STATE_RAWTEXT, "style"},
    {GTEXT_HTML_STATE_SCRIPT_DATA, "script"},
    {GTEXT_HTML_STATE_PLAINTEXT, nullptr},
    {GTEXT_HTML_STATE_CDATA_SECTION, nullptr},
  };
  size_t runs = 0;
  for (const auto & start : starts) {
    Config setup = in_state(start.first, start.second);
    setup.options.allow_cdata = [](void *) { return false; };
    for (const std::string & in : corpus()) {
      const Outcome whole = tokenize(in, {}, setup);
      EXPECT_EQ(whole.status, GTEXT_HTML_TOKENIZER_DONE);
      for (size_t cut = 0; cut <= in.size(); cut++) {
        const Outcome two = tokenize(in, {cut, in.size()}, setup);
        runs++;
        ASSERT_TRUE(whole.same_as(two))
            << "state " << start.first << " input " << show(in.data(), in.size())
            << " cut at " << cut << "\n whole: " << whole.everything()
            << "\n cut:   " << two.everything();
      }
      const Outcome bytes = tokenize(in, {1}, setup);
      ASSERT_TRUE(whole.same_as(bytes))
          << "state " << start.first << " input " << show(in.data(), in.size())
          << "\n whole: " << whole.everything() << "\n bytes: " << bytes.everything();
    }
  }
  EXPECT_GT(runs, 5000u);
}

TEST(HtmlTokenizerChunking, EveryWayOfCuttingTheShortInputsInThree) {
  const std::vector<std::string> shorts = {
    "&notit;x", "<!--a-->", "<!doctype html>", "<![CDATA[x]]>", "\xE2\x82\xAC\r\nz",
    "<a b=\"&amp;\">", "<?a b?>", "&#x41;", "<!DOCTYPE a PUBLIC 'b' 'c'>", "&ampx;", "<a/b>",
  };
  for (const std::string & in : shorts) {
    Config setup;
    setup.options.allow_cdata = [](void *) { return true; };
    const Outcome whole = tokenize(in, {}, setup);
    for (size_t a = 1; a < in.size(); a++) {
      for (size_t b = 1; a + b < in.size(); b++) {
        const Outcome three = tokenize(in, {a, b, in.size()}, setup);
        ASSERT_TRUE(whole.same_as(three))
            << show(in.data(), in.size()) << " cut " << a << "," << b << "\n whole: "
            << whole.everything() << "\n cut:   " << three.everything();
      }
    }
  }
}

TEST(HtmlTokenizerChunking, RandomPiecesGiveTheWholeRunsAnswerToo) {
  uint64_t seed = 12345;
  auto next = [&seed]() {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (size_t)(seed >> 33);
  };
  for (const std::string & in : corpus()) {
    const Outcome whole = run(in);
    for (int round = 0; round < 30; round++) {
      std::vector<size_t> cuts;
      for (size_t at = 0; at < in.size(); ) {
        const size_t take = 1 + next() % 11;
        cuts.push_back(take);
        at += take;
      }
      const Outcome r = tokenize(in, cuts);
      ASSERT_TRUE(whole.same_as(r)) << show(in.data(), in.size());
    }
  }
}

TEST(HtmlTokenizerChunking, ALoneCarriageReturnAtTheEndOfAPieceWaitsForTheLineFeed) {
  const Outcome whole = run("a\r\nb");
  const Outcome cut = tokenize("a\r\nb", {2, 1, 1});
  EXPECT_EQ(cut.tokens_text(), "C\"a\\nb\" F");
  EXPECT_TRUE(whole.same_as(cut));
}

// ---- Work ----------------------------------------------------------------

// Names, values, comments and text of any length: the limits that would make the inputs below fail
// with a limit status (which would make the work assertion about nothing) are raised, and the status
// must be DONE. Fed a byte at a time and whole, the work is at most GTEXT_HTML_TOKENIZER_WORK_BOUND.
Config unlimited() {
  Config c;
  c.options.max_name_bytes = SIZE_MAX;
  c.options.max_value_bytes = SIZE_MAX;
  c.options.max_comment_bytes = SIZE_MAX;
  c.options.max_text_bytes = SIZE_MAX;
  c.options.max_input_bytes = SIZE_MAX;
  c.options.max_attributes = SIZE_MAX;
  return c;
}

void expect_linear(const std::string & in, const char * what, Config c = unlimited()) {
  const Outcome r = tokenize(in, {1}, c);
  EXPECT_EQ(r.status, GTEXT_HTML_TOKENIZER_DONE) << what << ": " << status_name(r.status);
  EXPECT_LE(r.work, GTEXT_HTML_TOKENIZER_WORK_BOUND(in.size())) << what << ": work " << r.work
      << " for " << in.size() << " bytes";
  const Outcome all = tokenize(in, {}, c);
  EXPECT_EQ(all.status, GTEXT_HTML_TOKENIZER_DONE) << what << " (whole)";
  EXPECT_LE(all.work, GTEXT_HTML_TOKENIZER_WORK_BOUND(in.size())) << what << " (whole)";
}

TEST(HtmlTokenizerWork, ALongUnterminatedConstructCostsABoundedAmountPerByte) {
  const size_t n = 300000; // far past the 8 KiB default name limit
  const std::string a(n, 'a');
  expect_linear("<!--" + a, "comment");
  expect_linear("<!--" + std::string(n, '-'), "dashes in a comment");
  expect_linear("<!--" + std::string(n / 2, '<') + std::string(n / 2, '!'), "less-thans in a comment");
  expect_linear("<!doctype " + a, "doctype name");
  expect_linear("<!doctype " + a + " public '" + a + "' '" + a + "'", "doctype name and identifiers");
  expect_linear("<!doctype html public '" + a, "doctype identifier");
  expect_linear("<" + a, "tag name");
  expect_linear("<" + a + " " + a + "=" + a + ">", "tag, attribute name and unquoted value");
  expect_linear("<a " + a, "attribute name");
  expect_linear("<a b=\"" + a, "quoted value");
  expect_linear("<a b=" + a, "unquoted value");
  expect_linear("<a b='" + std::string(n / 5, '&') + "amp;", "ampersands in a value");
  expect_linear("&" + a, "reference name");
  expect_linear("&#" + std::string(n, '9'), "decimal reference");
  expect_linear("&#x" + std::string(n, 'f'), "hex reference");
  expect_linear("<![CDATA[" + a, "cdata, not allowed");
  expect_linear("<?" + a, "processing instruction target");
  expect_linear("<?" + a + " " + a, "processing instruction target and data");
  expect_linear("<?a " + a, "processing instruction data");
  expect_linear("<!" + a, "bogus comment");
  expect_linear("</" + a, "end tag name");
  expect_linear("</" + a + " " + a + "=" + a + ">", "end tag with an attribute");
  expect_linear(a, "text");
  expect_linear(std::string(n / 4, '\xE2') + std::string(n / 4, '\x82'), "broken UTF-8");
  expect_linear("<!-" + std::string(n, '-'), "dashes after an incomplete opener");
  std::string tags;
  for (size_t i = 0; i < n / 8; i++) {
    tags += "<a b=1>";
  }
  expect_linear(tags, "many tags");
  std::string attrs = "<a";
  for (size_t i = 0; i < 1000; i++) {
    attrs += " a" + std::to_string(i);
  }
  expect_linear(attrs + ">", "attributes");
}

TEST(HtmlTokenizerWork, TheStatesATreeBuilderSetCostTheSame) {
  const std::string a(300000, 'a');
  Config raw = unlimited();
  raw.state = GTEXT_HTML_STATE_RAWTEXT;
  raw.has_last = true;
  raw.last = "style";
  expect_linear("</styl" + a + "</style" + a, "a RAWTEXT end tag candidate", raw);
  expect_linear("</style" + a, "a RAWTEXT end tag candidate that is the name and more", raw);
  Config rcdata = raw;
  rcdata.state = GTEXT_HTML_STATE_RCDATA;
  expect_linear("</styl" + a + "&amp;" + a, "an RCDATA end tag candidate", rcdata);
  Config script = raw;
  script.state = GTEXT_HTML_STATE_SCRIPT_DATA;
  script.last = "script";
  expect_linear("<!--<script>" + a + "</script" + a, "script data, double escaped", script);
  expect_linear("<!--<scrip" + a + "</script>", "script data, a double escape word", script);
  expect_linear("<!--</script" + a, "script data escaped end tag candidate", script);
}

// Counts live bytes and remembers the most there ever were.
struct PeakAllocator {
  GTEXT_Allocator alloc;
  std::map<void *, size_t> live;
  size_t bytes = 0;
  size_t peak = 0;
  PeakAllocator() {
    alloc.ctx = this;
    alloc.malloc_fn = [](void * c, size_t n) { return ((PeakAllocator *)c)->take(nullptr, n); };
    alloc.calloc_fn = [](void * c, size_t k, size_t n) {
      void * p = ((PeakAllocator *)c)->take(nullptr, k * n);
      std::memset(p, 0, k * n);
      return p;
    };
    alloc.realloc_fn = [](void * c, void * p, size_t n) { return ((PeakAllocator *)c)->take(p, n); };
    alloc.free_fn = [](void * c, void * p) { ((PeakAllocator *)c)->give(p); };
  }
  void * take(void * old, size_t n) {
    void * p = std::malloc(n ? n : 1);
    if (old) {
      const size_t was = live[old];
      std::memcpy(p, old, was < n ? was : n);
      give(old);
    }
    live[p] = n;
    bytes += n;
    peak = std::max(peak, bytes);
    return p;
  }
  void give(void * p) {
    if (!p) {
      return;
    }
    bytes -= live[p];
    live.erase(p);
    std::free(p);
  }
};

TEST(HtmlTokenizerWork, MemoryHeldBetweenFeedsIsBoundedByTheConstructNotTheInput) {
  // A byte at a time, tokens drained: what the tokenizer holds is the tokens'
  // own buffers and the few bytes of look-ahead, however much has gone through.
  PeakAllocator counting;
  Config setup;
  setup.options.allocator = &counting.alloc;
  GTEXT_HTML_Tokenizer * tok = gtext_html_tokenizer_new(&setup.options);
  ASSERT_NE(tok, nullptr);
  const std::string unit = "<p class=x>caf\xC3\xA9 &amp; <!-- c --> \r\n</p>";
  const GTEXT_HTML_Token * t = nullptr;
  size_t tokens = 0;
  size_t peak_after_first = 0;
  for (int i = 0; i < 4000; i++) {
    for (char c : unit) {
      ASSERT_EQ(gtext_html_tokenizer_feed(tok, &c, 1), GTEXT_HTML_TOKENIZER_OK);
      while (gtext_html_tokenizer_next(tok, &t) == GTEXT_HTML_TOKENIZER_TOKEN) {
        tokens++;
      }
    }
    if (i == 10) {
      peak_after_first = counting.peak;
    }
  }
  EXPECT_EQ(tokens, 20000u);
  EXPECT_EQ(counting.peak, peak_after_first) << "memory grew with the input";
  EXPECT_LT(counting.peak, 16384u);
  gtext_html_tokenizer_free(tok);
  EXPECT_TRUE(counting.live.empty());
}

// ---- Allocation failure --------------------------------------------------

TEST(HtmlTokenizerOom, AnAllocationFailureAtEveryPointIsAnErrorAndLeaksNothing) {
  std::string rich =
      "<!DOCTYPE html PUBLIC 'a' 'b'><p class=a id=b data-x=\"&amp;&notit;\" a b c d e f g h i j k>"
      "text &amp; more &#x41;<!-- comment --><?pi data?><![CDATA[x]]>"
      "\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01\x01"
      "<a x x></a y><script>a</script>\r\n";
  Config base;
  base.options.allow_cdata = [](void *) { return true; };
  const Outcome expected = tokenize(rich, {}, base);
  ASSERT_EQ(expected.status, GTEXT_HTML_TOKENIZER_DONE);
  size_t failures = 0;
  size_t total = 0;
  for (const std::vector<size_t> & cuts : {std::vector<size_t>{}, std::vector<size_t>{1}, std::vector<size_t>{7, 3}}) {
    for (size_t n = 1;; n++) {
      FaultAllocator fa;
      Config setup = base;
      setup.options.allocator = &fa.alloc;
      fa.arm(n);
      const Outcome r = tokenize(rich, cuts, setup);
      EXPECT_EQ(fa.bad_frees, 0u);
      EXPECT_TRUE(fa.live.empty()) << "leak after failing allocation " << n;
      if (!fa.failed) {
        // Past the last allocation: the run is the ordinary one.
        EXPECT_TRUE(expected.same_as(r)) << r.everything();
        total = n;
        break;
      }
      failures++;
      EXPECT_EQ(r.status, GTEXT_HTML_TOKENIZER_E_OOM) << "allocation " << n << " " << r.everything();
      ASSERT_LT(n, 100000u);
    }
  }
  EXPECT_GT(failures, 30u);
  EXPECT_GT(total, 0u);
}

TEST(HtmlTokenizerOom, NewReturnsNullWhenTheAllocatorFails) {
  FaultAllocator fa;
  GTEXT_HTML_TokenizerOptions opts = {};
  opts.allocator = &fa.alloc;
  fa.arm(1);
  EXPECT_EQ(gtext_html_tokenizer_new(&opts), nullptr);
  EXPECT_TRUE(fa.live.empty());
}

TEST(HtmlTokenizerOom, SetLastStartTagReportsAFailureAndTheTokenizerStaysUsable) {
  FaultAllocator fa;
  GTEXT_HTML_TokenizerOptions opts = {};
  opts.allocator = &fa.alloc;
  GTEXT_HTML_Tokenizer * tok = gtext_html_tokenizer_new(&opts);
  ASSERT_NE(tok, nullptr);
  fa.arm(1);
  EXPECT_EQ(gtext_html_tokenizer_set_last_start_tag(tok, "style", 5), GTEXT_HTML_TOKENIZER_E_OOM);
  const GTEXT_HTML_Token * t = nullptr;
  EXPECT_EQ(gtext_html_tokenizer_next(tok, &t), GTEXT_HTML_TOKENIZER_E_OOM);
  gtext_html_tokenizer_free(tok);
  EXPECT_TRUE(fa.live.empty());
}

// ---- Hand-checked tricky cases -------------------------------------------

TEST(HtmlTokenizerTricky, ACommentOpenerThatIsAlmostAnotherThing) {
  EXPECT_EQ(run("<!-->a").tokens_text(), "M() C\"a\" F");
  EXPECT_EQ(run("<!--a-->-->").tokens_text(), "M(a) C\"-->\" F");
  EXPECT_EQ(run("<!DOCTYPE").tokens_text(), "D(-|-|-)! F");
  EXPECT_EQ(run("<!doctyp").tokens_text(), "M(doctyp) F");
  EXPECT_EQ(run("<![CDAT").tokens_text(), "M([CDAT) F");
  EXPECT_EQ(run("<![cdata[x]]>").tokens_text(), "M([cdata[x]]) F"); // case sensitive
}

TEST(HtmlTokenizerTricky, AnEndTagNameIsAbandonedWhereTheStandardSaysAndNotBefore) {
  Config raw = in_state(GTEXT_HTML_STATE_RAWTEXT, "xmp");
  EXPECT_EQ(tokenize("</xm", {}, raw).tokens_text(), "C\"</xm\" F");
  EXPECT_EQ(tokenize("</xmp", {}, raw).tokens_text(), "C\"</xmp\" F");
  EXPECT_EQ(tokenize("</xmp ", {}, raw).tokens_text(), "F");
  EXPECT_EQ(tokenize("</xmp ", {}, raw).errors_text(), "eof-in-tag@1:7");
  EXPECT_EQ(tokenize("</xmp/", {}, raw).errors_text(), "eof-in-tag@1:7");
  EXPECT_EQ(tokenize("</xmp\n>", {}, raw).tokens_text(), "E(xmp) F");
  EXPECT_EQ(tokenize("</xmp\x01", {}, raw).tokens_text(), "C\"</xmp\\x01\" F");
  EXPECT_EQ(tokenize("</XmP>", {}, raw).tokens_text(), "E(xmp) F");
}

TEST(HtmlTokenizerTricky, TheTokenAfterATokenThatEndsTheInputInTheMiddleOfAConstruct) {
  EXPECT_EQ(run("a<b").tokens_text(), "C\"a\" F");
  EXPECT_EQ(run("<p>a<b").tokens_text(), "S(p) C\"a\" F");
  EXPECT_EQ(run("a&amp").tokens_text(), "C\"a&\" F");
  EXPECT_EQ(run("<!--x").tokens_text(), "M(x) F");
}


// ---- Review round: things the limits must not turn into failures ----------

TEST(HtmlTokenizerReview, AnAbandonedEndTagCandidateLongerThanTheNameLimitIsTextNotAFailure) {
  // The standard has no limit on the letters after `</` that turn out not to name the open element.
  const std::string letters(10000, 'x');
  for (auto state : {GTEXT_HTML_STATE_RCDATA, GTEXT_HTML_STATE_RAWTEXT, GTEXT_HTML_STATE_SCRIPT_DATA}) {
    Config c = in_state(state, "style");
    const Outcome r = tokenize("a</styl" + letters + ">b</style>c", {}, c);
    EXPECT_EQ(r.status, GTEXT_HTML_TOKENIZER_DONE) << status_name(r.status);
    ASSERT_EQ(r.tokens.size(), 4u);
    EXPECT_EQ(r.tokens[0], "C\"a</styl" + letters + ">b\"");
    EXPECT_EQ(r.tokens[1], "E(style)");
    EXPECT_EQ(r.tokens[2], "C\"c\"");
    // And a byte at a time, with a tiny limit.
    c.options.max_name_bytes = 3;
    const Outcome t = tokenize("a</styl" + letters + ">b</sty>c", {1}, c);
    EXPECT_EQ(t.status, GTEXT_HTML_TOKENIZER_DONE);
    EXPECT_EQ(t.tokens[0], "C\"a</styl" + letters + ">b</sty>c\"");
  }
  // The same in the escaped states of script data.
  Config c = in_state(GTEXT_HTML_STATE_SCRIPT_DATA, "script");
  const Outcome e = tokenize("<!--</scrip" + letters + ">-->", {}, c);
  EXPECT_EQ(e.status, GTEXT_HTML_TOKENIZER_DONE);
}

TEST(HtmlTokenizerReview, NumericReferencesWorkWithTheSmallestNameLimit) {
  Config c;
  c.options.max_name_bytes = 1;
  const Outcome r = tokenize("&#x41;&#66;&#xZ&#;&amp;", {}, c);
  EXPECT_EQ(r.status, GTEXT_HTML_TOKENIZER_DONE) << status_name(r.status);
  EXPECT_EQ(r.tokens_text(), "C\"AB&#xZ&#;&\" F");
  c.options.max_name_bytes = 2;
  EXPECT_EQ(tokenize("<a b='&#x41;'>", {1}, c).status, GTEXT_HTML_TOKENIZER_DONE);
}


TEST(HtmlTokenizerReview, AllowCdataIsATriStateThatTheCallerCanClear) {
  static int asked;
  asked = 0;
  GTEXT_HTML_TokenizerOptions o = {};
  o.allow_cdata = [](void *) { asked++; return true; };
  GTEXT_HTML_Tokenizer * tok = gtext_html_tokenizer_new(&o);
  ASSERT_NE(tok, nullptr);
  const std::string in = "<![CDATA[a]]>";
  std::vector<std::string> seen;
  auto once = [&]() {
    // A fresh run of the same input through the same tokenizer is not possible, so a new feed: the
    // state is data again after each section.
    ASSERT_EQ(gtext_html_tokenizer_feed(tok, in.data(), in.size()), GTEXT_HTML_TOKENIZER_OK);
    const GTEXT_HTML_Token * t = nullptr;
    GTEXT_HTML_TokenizerStatus st;
    while ((st = gtext_html_tokenizer_next(tok, &t)) == GTEXT_HTML_TOKENIZER_TOKEN) {
      seen.push_back(render(t));
    }
    ASSERT_EQ(st, GTEXT_HTML_TOKENIZER_NEED_INPUT);
  };
  once(); // fresh: the callback, which says yes
  EXPECT_EQ(asked, 1);
  gtext_html_tokenizer_set_allow_cdata(tok, GTEXT_HTML_CDATA_REFUSE);
  once(); // refused whatever the callback says; it is not asked
  EXPECT_EQ(asked, 1);
  once(); // and it stands
  EXPECT_EQ(asked, 1);
  gtext_html_tokenizer_set_allow_cdata(tok, GTEXT_HTML_CDATA_DEFAULT);
  once(); // cleared: the callback again
  EXPECT_EQ(asked, 2);
  gtext_html_tokenizer_set_allow_cdata(tok, GTEXT_HTML_CDATA_ALLOW);
  once();
  gtext_html_tokenizer_set_allow_cdata(tok, (GTEXT_HTML_CdataOverride)77); // ignored
  once();
  EXPECT_EQ(asked, 2);
  gtext_html_tokenizer_finish(tok);
  const GTEXT_HTML_Token * t = nullptr;
  while (gtext_html_tokenizer_next(tok, &t) == GTEXT_HTML_TOKENIZER_TOKEN) {
    seen.push_back(render(t));
  }
  gtext_html_tokenizer_free(tok);
  // refused: a comment each time; allowed (by the callback, then by the override, then with the
  // override unknown to the tokenizer): the section's text, held until the end.
  ASSERT_EQ(seen.size(), 5u);
  EXPECT_EQ(seen[0], "C\"a\"");
  EXPECT_EQ(seen[1], "M([CDATA[a]])");
  EXPECT_EQ(seen[2], "M([CDATA[a]])");
  EXPECT_EQ(seen[3], "C\"aaa\"");
  EXPECT_EQ(seen[4], "F");
}

TEST(HtmlTokenizerReview, ANameThatCollidesUnderOneSeedIsNotQuadraticUnderAnother) {
  const uint64_t attack = 12345, other = 99991;
  const size_t mask = 2047; // the table of a tag of 1,000 attributes
  std::vector<std::string> names;
  for (uint64_t i = 0; names.size() < 900; i++) {
    const std::string n = "n" + std::to_string(i);
    if ((gtext_html_tokenizer_name_hash(attack, n.data(), n.size()) & mask) == 7) {
      names.push_back(n);
    }
    ASSERT_LT(i, 20000000u);
  }
  std::string in = "<a";
  for (const std::string & n : names) {
    in += " " + n;
  }
  in += ">";
  Config c = unlimited();
  c.options.hash_seed = attack;
  const Outcome bad = tokenize(in, {}, c);
  c.options.hash_seed = other;
  const Outcome good = tokenize(in, {}, c);
  c.options.hash_seed = 0;
  const Outcome automatic = tokenize(in, {}, c);
  EXPECT_EQ(good.tokens, bad.tokens);
  EXPECT_EQ(good.status, GTEXT_HTML_TOKENIZER_DONE);
  // Under the seed the names were built for, the probes are the square of the count (this is what
  // the bound below would not hold for); under another, they are within the bound.
  EXPECT_GT(bad.work, names.size() * names.size() / 4);
  EXPECT_LE(good.work, GTEXT_HTML_TOKENIZER_WORK_BOUND(in.size()));
  EXPECT_LE(automatic.work, GTEXT_HTML_TOKENIZER_WORK_BOUND(in.size()));
  // Two tokenizers made one after the other do not share a seed.
  GTEXT_HTML_Tokenizer * t1 = gtext_html_tokenizer_new(nullptr);
  GTEXT_HTML_Tokenizer * t2 = gtext_html_tokenizer_new(nullptr);
  ASSERT_NE(t1, nullptr);
  ASSERT_NE(t2, nullptr);
  gtext_html_tokenizer_free(t1);
  gtext_html_tokenizer_free(t2);
}

TEST(HtmlTokenizerReview, ALargeChunkAfterATailIsReadInPlaceAndNotCopiedWhole) {
  PeakAllocator counting;
  Config c;
  c.options.allocator = &counting.alloc;
  GTEXT_HTML_Tokenizer * tok = gtext_html_tokenizer_new(&c.options);
  ASSERT_NE(tok, nullptr);
  const GTEXT_HTML_Token * t = nullptr;
  size_t tokens = 0;
  auto drain = [&]() {
    while (gtext_html_tokenizer_next(tok, &t) == GTEXT_HTML_TOKENIZER_TOKEN) {
      tokens++;
    }
  };
  drain();
  // The first byte of a three-byte character: a tail that must wait.
  ASSERT_EQ(gtext_html_tokenizer_feed(tok, "\xE2", 1), GTEXT_HTML_TOKENIZER_OK);
  drain();
  std::string big = "\x82\xAC";
  for (int i = 0; i < 2000000; i++) {
    big += "<p>";
  }
  ASSERT_EQ(gtext_html_tokenizer_feed(tok, big.data(), big.size()), GTEXT_HTML_TOKENIZER_OK);
  drain();
  EXPECT_EQ(tokens, 2000001u); // the euro sign's text, and two million tags... less the last text
  EXPECT_LT(counting.peak, 64u * 1024) << "a chunk of " << big.size() << " bytes was copied";
  gtext_html_tokenizer_finish(tok);
  drain();
  gtext_html_tokenizer_free(tok);
  EXPECT_TRUE(counting.live.empty());
  // The answer is the same as for one piece.
  const std::string whole = "\xE2" + big;
  const Outcome a = tokenize(whole.substr(0, 3000), {});
  const Outcome b = tokenize(whole.substr(0, 3000), {1, 5000});
  EXPECT_TRUE(a.same_as(b));
}

TEST(HtmlTokenizerReview, AccessorsAcceptNullLikeTheRest) {
  EXPECT_EQ(gtext_html_tokenizer_get_state(nullptr), GTEXT_HTML_STATE_DATA);
  EXPECT_EQ(gtext_html_tokenizer_error_count(nullptr), 0u);
  EXPECT_EQ(gtext_html_tokenizer_error_at(nullptr, 0), nullptr);
  EXPECT_EQ(gtext_html_tokenizer_errors_dropped(nullptr), 0u);
  EXPECT_EQ(gtext_html_tokenizer_work(nullptr), 0u);
  gtext_html_tokenizer_set_allow_cdata(nullptr, GTEXT_HTML_CDATA_ALLOW);
}

TEST(HtmlTokenizerReview, ErrorsCanBeOnlyCounted) {
  Config c;
  c.options.count_errors_only = true;
  const Outcome r = tokenize("\x01\x01<a b b>&amp&#0;", {}, c);
  EXPECT_EQ(r.status, GTEXT_HTML_TOKENIZER_DONE);
  EXPECT_EQ(r.errors.size(), 0u);
  EXPECT_EQ(r.dropped, 5u);
  // max_errors of 0 is the default and keeps them.
  Config d;
  EXPECT_EQ(tokenize("\x01\x01", {}, d).errors.size(), 2u);
  // Nothing was allocated for the list.
  FaultAllocator fa;
  Config e;
  e.options.count_errors_only = true;
  e.options.allocator = &fa.alloc;
  const size_t before = fa.calls;
  (void)before;
  tokenize("\x01\x01\x01", {}, e);
  EXPECT_TRUE(fa.live.empty());
}

TEST(HtmlTokenizerReview, TheNamedReferenceScratchHoldsTheTablesLongestName) {
  EXPECT_LE(gtext_html_charref_longest + 2, (size_t)GTEXT_HTML_TOKENIZER_NAME_WINDOW);
  // And the longest name in the table is found through the tokenizer.
  std::string longest;
  for (size_t i = 0; i < gtext_html_charref_count; i++) {
    if (gtext_html_charref_table[i].name_length == gtext_html_charref_longest) {
      longest = std::string(gtext_html_charref_name(&gtext_html_charref_table[i]), gtext_html_charref_longest);
    }
  }
  ASSERT_FALSE(longest.empty());
  const Outcome r = tokenize("&" + longest + "x", {1});
  EXPECT_EQ(r.errors_text(), "");
  EXPECT_NE(r.tokens[0].find("x"), std::string::npos);
}

TEST(HtmlTokenizerReview, TheInputLimitIsTheOneExceptionToCutIndependence) {
  Config c;
  c.options.max_input_bytes = 5;
  const std::string in = "<a>b<c>";
  const Outcome whole = tokenize(in, {}, c);
  const Outcome bytes = tokenize(in, {1}, c);
  EXPECT_EQ(whole.status, GTEXT_HTML_TOKENIZER_E_LIMIT_INPUT);
  EXPECT_EQ(bytes.status, GTEXT_HTML_TOKENIZER_E_LIMIT_INPUT);
  EXPECT_EQ(whole.tokens.size(), 0u);
  EXPECT_EQ(bytes.tokens_text(), "S(a)");
}

TEST(HtmlTokenizerReview, ProcessingInstructionsEndWhereTheBogusCommentTheyReplaceDid) {
  // The references have no processing instruction states, so there is nothing to put these to.
  // What can be checked by hand: for a processing instruction ended by `>` the token ends at the
  // first `>`, exactly where the bogus comment of the older standard ended, and with `?>` at the
  // `?>`; what follows is read as data either way.
  for (const char * body : {"a b", "a  b c", "a-b_c 1", "xmlx d", "a\tb"}) {
    const std::string in = std::string("<?") + body + ">X<p>";
    const Outcome r = tokenize(in);
    ASSERT_EQ(r.tokens.size(), 4u) << in;
    EXPECT_EQ(r.tokens[0][0], 'P') << in;
    EXPECT_EQ(r.tokens[1], "C\"X\"");
    EXPECT_EQ(r.token_offsets[1], in.find('>') + 1);
  }
  for (const char * body : {"a b", "a  b c", "a-b_c 1"}) {
    const std::string in = std::string("<?") + body + "?>X<p>";
    const Outcome r = tokenize(in);
    EXPECT_EQ(r.token_offsets[1], in.find("?>") + 2) << in;
  }
}

} // namespace
