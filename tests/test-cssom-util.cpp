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
 * `CSS.escape()` (css_cssom_util.h): CSSOM's "serialize an identifier" with the rules the wrapper adds: the empty text, the NUL, the text that is not well-formed, the lone
 * surrogate of a WTF-8 text, and the property that an escaped identifier reads back as the text.
 */

#include <cstdint>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_test_support.h"
#include "markup_test_support.h"

namespace {

std::string escape(const std::string & text, GTEXT_CSSOM_Text_Encoding enc = GTEXT_CSSOM_TEXT_UTF8, GTEXT_CSS_Status * st_out = nullptr) {
  char * out = nullptr;
  size_t n = 0;
  const GTEXT_CSS_Status st = gtext_cssom_escape_buffer(text.data(), text.size(), enc, nullptr, &out, &n);
  if (st_out) {
    *st_out = st;
  }
  if (st != GTEXT_CSS_OK) {
    return std::string("<") + gtext_css_status_string(st) + ">";
  }
  std::string s(out, n);
  gtext_cssom_buffer_free(nullptr, out);
  return s;
}

// The matrix: "`CSS.escape`: "-", "1a", "\u0000", "a b"  \-, \31 a, U+FFFD, a\ b".
TEST(CssomEscape, TheRowsOfTheMatrix) {
  EXPECT_EQ(escape("-"), "\\-");
  EXPECT_EQ(escape("1a"), "\\31 a");
  EXPECT_EQ(escape(std::string("\0", 1)), "\xEF\xBF\xBD");
  EXPECT_EQ(escape("a b"), "a\\ b");
}

// CSSOM, "serialize an identifier": the steps, one each.
TEST(CssomEscape, EachStepOfSerializeAnIdentifier) {
  EXPECT_EQ(escape(""), "") << "the empty string is the empty string";
  EXPECT_EQ(escape(std::string("a\0b", 3)), "a\xEF\xBF\xBD" "b") << "U+0000 is U+FFFD";
  EXPECT_EQ(escape("\x01"), "\\1 ") << "[\\1-\\1f] is escaped as a code point";
  EXPECT_EQ(escape("a\x1f"), "a\\1f ");
  EXPECT_EQ(escape("\x7f"), "\\7f ") << "U+007F too";
  EXPECT_EQ(escape("0"), "\\30 ") << "the first character, a digit";
  EXPECT_EQ(escape("a0"), "a0");
  EXPECT_EQ(escape("-0"), "-\\30 ") << "the second, a digit after a leading hyphen";
  EXPECT_EQ(escape("--0"), "--0");
  EXPECT_EQ(escape("-a"), "-a");
  EXPECT_EQ(escape("--"), "--") << "a hyphen alone is escaped; two are an identifier";
  EXPECT_EQ(escape("a-"), "a-");
  EXPECT_EQ(escape("_"), "_");
  EXPECT_EQ(escape("a.b#c"), "a\\.b\\#c");
  EXPECT_EQ(escape("a\"b'c"), "a\\\"b\\'c");
  EXPECT_EQ(escape("\xC3\xA9"), "\xC3\xA9") << "a non-ASCII identifier character is itself";
  EXPECT_EQ(escape("\xF0\x9F\x98\x80"), "\xF0\x9F\x98\x80") << "a supplementary one";
  EXPECT_EQ(escape("a\\b"), "a\\\\b");
  EXPECT_EQ(escape(" "), "\\ ");
  EXPECT_EQ(escape("\n"), "\\a ");
}

TEST(CssomEscape, IllFormedTextIsInvalidAndNothingIsWritten) {
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  const char * const bad[] = {"\xC3", "\xC0\x80", "\xED\xA0\x80", "\xF5\x80\x80\x80", "a\x80", "\xE2\x82", "\xF0\x9F\x98"};
  for (const char * b : bad) {
    escape(b, GTEXT_CSSOM_TEXT_UTF8, &st);
    EXPECT_EQ(st, GTEXT_CSS_E_INVALID) << b;
  }
  std::string written;
  GTEXT_Markup_Sink sink;
  std::memset(&sink, 0, sizeof sink);
  sink.user = &written;
  sink.write = [](void * u, const char * s, size_t n) {
    static_cast<std::string *>(u)->append(s, n);
    return GTEXT_MARKUP_OK;
  };
  EXPECT_EQ(gtext_cssom_escape("ab\xC3", 3, GTEXT_CSSOM_TEXT_UTF8, &sink), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(written, "") << "an ill-formed text writes nothing, not its well-formed prefix";
  EXPECT_EQ(gtext_cssom_escape(nullptr, 1, GTEXT_CSSOM_TEXT_UTF8, &sink), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_escape("a", 1, GTEXT_CSSOM_TEXT_UTF8, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_escape("a", 1, static_cast<GTEXT_CSSOM_Text_Encoding>(9), &sink), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_escape(nullptr, 0, GTEXT_CSSOM_TEXT_UTF8, &sink), GTEXT_CSS_OK);
}

TEST(CssomEscape, AWtf8TextKeepsALoneSurrogateAsOneCodePoint) {
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  const std::string lone = "a\xED\xA0\x80" "z";   // U+D800
  EXPECT_EQ(escape(lone, GTEXT_CSSOM_TEXT_UTF8, &st), "<invalid argument>");
  EXPECT_EQ(st, GTEXT_CSS_E_INVALID);
  const std::string out = escape(lone, GTEXT_CSSOM_TEXT_WTF8, &st);
  EXPECT_EQ(st, GTEXT_CSS_OK);
  EXPECT_EQ(out, "a\\\xED\xA0\x80" "z") << "one code point, escaped with a backslash as a character that is not an identifier character is";
  // a pair of surrogates in WTF-8 is two code points; well-formed text is the same in both
  EXPECT_EQ(escape("ab\xC3\xA9", GTEXT_CSSOM_TEXT_WTF8), "ab\xC3\xA9");
}

struct FailSink {
  size_t calls = 0;
  size_t fail_at = 0;
};

TEST(CssomEscape, ASinkThatRefusesIsAWriteError) {
  FailSink fs;
  fs.fail_at = 1;
  GTEXT_Markup_Sink sink;
  std::memset(&sink, 0, sizeof sink);
  sink.user = &fs;
  sink.write = [](void * u, const char *, size_t) {
    FailSink * f = static_cast<FailSink *>(u);
    return ++f->calls >= f->fail_at ? GTEXT_MARKUP_E_OOM : GTEXT_MARKUP_OK;
  };
  EXPECT_EQ(gtext_cssom_escape("abc", 3, GTEXT_CSSOM_TEXT_UTF8, &sink), GTEXT_CSS_E_WRITE);
}

TEST(CssomEscape, AllocationFailureIsOomAndLeaksNothing) {
  const std::string input = "-1 a.b\xC3\xA9\x01" "z";
  for (size_t n = 1; n < 100; n++) {
    FaultAllocator fa;
    char * out = nullptr;
    size_t len = 0;
    fa.arm(n);
    const GTEXT_CSS_Status st = gtext_cssom_escape_buffer(input.data(), input.size(), GTEXT_CSSOM_TEXT_UTF8, &fa.alloc, &out, &len);
    fa.disarm();
    if (!fa.failed) {
      EXPECT_EQ(st, GTEXT_CSS_OK);
      gtext_cssom_buffer_free(&fa.alloc, out);
      EXPECT_TRUE(fa.live.empty());
      break;
    }
    EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
    EXPECT_EQ(out, nullptr);
    EXPECT_TRUE(fa.live.empty()) << n;
  }
  FaultAllocator fa;
  char * out = nullptr;
  ASSERT_EQ(gtext_cssom_escape_buffer("", 0, GTEXT_CSSOM_TEXT_UTF8, &fa.alloc, &out, nullptr), GTEXT_CSS_OK);
  EXPECT_STREQ(out, "") << "the empty text is a buffer holding the empty string";
  gtext_cssom_buffer_free(&fa.alloc, out);
  EXPECT_TRUE(fa.live.empty());
}

// CSSOM: the result is an identifier that reads back as the text. The population: strings over the characters that matter, tokenized again.
TEST(CssomEscape, AnEscapedIdentifierReadsBackAsTheText) {
  const char * const atoms[] = {"-", "_", "0", "9", "a", "Z", " ", ".", "#", "\\", "\"", "'", "(", ")", "\t", "\n", "\x01", "\x7f", "\xC3\xA9", "\xE2\x82\xAC",
      "\xF0\x9F\x98\x80", "\xC3\x97", "\xE2\x80\x8B", "e", "+", "/", "*", "@", "!", ":", ";", "{", "}", ",", "<", ">", "\xEF\xBF\xBD", "\xC2\xA0"};
  uint64_t state = 0x9e3779b97f4a7c15ull;
  auto next = [&]() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };
  size_t checked = 0;
  for (size_t round = 0; round < 6000; round++) {
    std::string text;
    const size_t len = 1 + next() % 6;
    for (size_t i = 0; i < len; i++) {
      text += atoms[next() % (sizeof atoms / sizeof atoms[0])];
    }
    GTEXT_CSS_Status st = GTEXT_CSS_OK;
    const std::string escaped = escape(text, GTEXT_CSSOM_TEXT_UTF8, &st);
    ASSERT_EQ(st, GTEXT_CSS_OK);
    css_test::Parsed p;
    p.status = gtext_css_parse_component_values(escaped.data(), escaped.size(), nullptr, &p.tree, &p.result);
    ASSERT_EQ(p.status, GTEXT_CSS_OK);
    ASSERT_EQ(p.tree->value_count, 1u) << "text <" << text << "> escaped <" << escaped << "> is not one value";
    const GTEXT_CSS_Component_Value & v = p.tree->values[0];
    ASSERT_EQ(v.kind, GTEXT_CSS_VALUE_TOKEN) << escaped;
    ASSERT_EQ(v.type, GTEXT_CSS_TOKEN_IDENT) << "<" << text << "> escaped <" << escaped << ">";
    EXPECT_EQ(std::string(v.value.data, v.value.length), text) << escaped;
    checked++;
  }
  EXPECT_EQ(checked, 6000u);
}

} // namespace
