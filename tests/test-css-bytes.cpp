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
 * A stylesheet's bytes (CSS Syntax 3.2): which encoding they are read as and
 * why, and that every position the parse reports is in the bytes whatever the
 * encoding.
 *
 * The positions are checked against a table made by something else: for each
 * document, a script decoded the bytes a byte at a time with the language's own
 * codec and wrote down, for every character, the offset of its first byte and the
 * offset after its last. A tree parsed from the bytes must have, for every span
 * and every parse error, the offsets that table gives for the same characters of
 * the same text parsed as text.
 */

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>
#include <ghoti.io/text/encoding/encoding_decode.h>

#include "css_bytes_test_support.h"

using namespace css_test;

namespace {

// ---- which encoding, and why --------------------------------------------------------------------------------

struct Decision {
  const char * name;
  std::string bytes;
  const char * protocol;
  GTEXT_Encoding environment;
  GTEXT_Encoding encoding;
  GTEXT_CSS_Encoding_Source source;
  size_t bom;
};

std::string charset(const std::string & label) {
  return "@charset \"" + label + "\";";
}

std::vector<Decision> decisions() {
  const auto BOM = GTEXT_CSS_ENCODING_SOURCE_BOM;
  const auto PRO = GTEXT_CSS_ENCODING_SOURCE_PROTOCOL;
  const auto CHS = GTEXT_CSS_ENCODING_SOURCE_CHARSET_RULE;
  const auto ENV = GTEXT_CSS_ENCODING_SOURCE_ENVIRONMENT;
  const auto DEF = GTEXT_CSS_ENCODING_SOURCE_DEFAULT;
  const GTEXT_Encoding NONE = GTEXT_ENCODING_NONE;
  return {
      {"nothing", "", nullptr, NONE, GTEXT_ENCODING_UTF_8, DEF, 0},
      {"plain", "a{}", nullptr, NONE, GTEXT_ENCODING_UTF_8, DEF, 0},
      {"charset", charset("windows-1251") + "a{}", nullptr, NONE, GTEXT_ENCODING_WINDOWS_1251, CHS, 0},
      {"charset, any case of the label", charset("WINDOWS-1251") + "a{}", nullptr, NONE, GTEXT_ENCODING_WINDOWS_1251, CHS, 0},
      {"charset, a label with white space around it", charset("  windows-1251\t") + "a{}", nullptr, NONE,
          GTEXT_ENCODING_WINDOWS_1251, CHS, 0},
      {"charset, an alias", charset("latin1") + "a{}", nullptr, NONE, GTEXT_ENCODING_WINDOWS_1252, CHS, 0},
      {"charset, utf-8", charset("utf-8") + "a{}", nullptr, NONE, GTEXT_ENCODING_UTF_8, CHS, 0},
      {"charset says utf-16le, is utf-8", charset("utf-16le") + "a{}", nullptr, NONE, GTEXT_ENCODING_UTF_8, CHS, 0},
      {"charset says utf-16be, is utf-8", charset("UTF-16BE") + "a{}", nullptr, NONE, GTEXT_ENCODING_UTF_8, CHS, 0},
      {"charset says utf-16 (a label of utf-16le), is utf-8", charset("utf-16") + "a{}", nullptr, NONE, GTEXT_ENCODING_UTF_8, CHS, 0},
      {"charset says replacement", charset("hz-gb-2312") + "a{}", nullptr, NONE, GTEXT_ENCODING_REPLACEMENT, CHS, 0},
      {"an unknown label in the charset: the environment", charset("kamoulox") + "a{}", nullptr, GTEXT_ENCODING_ISO_8859_2,
          GTEXT_ENCODING_ISO_8859_2, ENV, 0},
      {"an unknown label in the charset: utf-8", charset("kamoulox") + "a{}", nullptr, NONE, GTEXT_ENCODING_UTF_8, DEF, 0},
      {"an empty label in the charset: utf-8", charset("") + "a{}", nullptr, NONE, GTEXT_ENCODING_UTF_8, DEF, 0},
      {"charset is not at the start", " " + charset("windows-1251"), nullptr, NONE, GTEXT_ENCODING_UTF_8, DEF, 0},
      {"charset after a comment", "/**/" + charset("windows-1251"), nullptr, NONE, GTEXT_ENCODING_UTF_8, DEF, 0},
      {"@Charset", "@Charset \"windows-1251\";", nullptr, NONE, GTEXT_ENCODING_UTF_8, DEF, 0},
      {"two spaces", "@charset  \"windows-1251\";", nullptr, NONE, GTEXT_ENCODING_UTF_8, DEF, 0},
      {"no space", "@charset\"windows-1251\";", nullptr, NONE, GTEXT_ENCODING_UTF_8, DEF, 0},
      {"single quotes", "@charset 'windows-1251';", nullptr, NONE, GTEXT_ENCODING_UTF_8, DEF, 0},
      {"a space before the semicolon", "@charset \"windows-1251\" ;", nullptr, NONE, GTEXT_ENCODING_UTF_8, DEF, 0},
      {"no semicolon", "@charset \"windows-1251\"", nullptr, NONE, GTEXT_ENCODING_UTF_8, DEF, 0},
      {"no semicolon, then a brace", "@charset \"windows-1251\"}", nullptr, NONE, GTEXT_ENCODING_UTF_8, DEF, 0},
      {"a NUL in the label", std::string("@charset \"windows\0-1251\";", 25), nullptr, NONE, GTEXT_ENCODING_UTF_8, DEF, 0},
      {"a byte above 7F in the label", "@charset \"windows-1251\xc3\";", nullptr, NONE, GTEXT_ENCODING_UTF_8, DEF, 0},
      {"utf-16 bytes of a charset (not ASCII)", std::string("@\0c\0h\0a\0r\0s\0e\0t\0 \0\"\0U\0T\0F\0-\0001\0006\0L\0E\0\"\0;\0", 40), nullptr, NONE,
          GTEXT_ENCODING_UTF_8, DEF, 0},
      {"the label ends the kilobyte", charset("windows-1251" + std::string(1012 - 12, ' ')), nullptr, NONE, GTEXT_ENCODING_WINDOWS_1251, CHS, 0},
      {"the label ends one byte later", charset("windows-1251" + std::string(1013 - 12, ' ')), nullptr, NONE, GTEXT_ENCODING_UTF_8, DEF, 0},
      {"protocol", "a{}", "windows-1251", NONE, GTEXT_ENCODING_WINDOWS_1251, PRO, 0},
      {"protocol, spaced and in any case", "a{}", " Iso-8859-2\n", NONE, GTEXT_ENCODING_ISO_8859_2, PRO, 0},
      {"protocol over charset", charset("iso-8859-5") + "a{}", " Iso-8859-2", NONE, GTEXT_ENCODING_ISO_8859_2, PRO, 0},
      {"protocol over environment", "a{}", "iso-8859-2", GTEXT_ENCODING_ISO_8859_5, GTEXT_ENCODING_ISO_8859_2, PRO, 0},
      {"an unknown protocol: charset", charset("iso-8859-5") + "a{}", "kamoulox", NONE, GTEXT_ENCODING_ISO_8859_5, CHS, 0},
      {"an empty protocol: charset", charset("iso-8859-5") + "a{}", "", NONE, GTEXT_ENCODING_ISO_8859_5, CHS, 0},
      {"an unknown protocol: the environment", "a{}", "kamoulox", GTEXT_ENCODING_ISO_8859_5, GTEXT_ENCODING_ISO_8859_5, ENV, 0},
      {"a protocol of utf-16 is utf-16 (no bom to say otherwise)", "a{}", "utf-16le", NONE, GTEXT_ENCODING_UTF_16LE, PRO, 0},
      {"a protocol of utf-16be", "a{}", "UTF-16BE", NONE, GTEXT_ENCODING_UTF_16BE, PRO, 0},
      {"charset over environment", charset("iso-8859-5") + "a{}", nullptr, GTEXT_ENCODING_ISO_8859_2, GTEXT_ENCODING_ISO_8859_5, CHS, 0},
      {"environment", "a{}", nullptr, GTEXT_ENCODING_ISO_8859_2, GTEXT_ENCODING_ISO_8859_2, ENV, 0},
      {"environment of utf-16", "a{}", nullptr, GTEXT_ENCODING_UTF_16LE, GTEXT_ENCODING_UTF_16LE, ENV, 0},
      {"utf-8 bom", "\xef\xbb\xbf" "a{}", nullptr, NONE, GTEXT_ENCODING_UTF_8, BOM, 3},
      {"utf-8 bom over everything", "\xef\xbb\xbf" + charset("iso-8859-5"), "iso-8859-2", GTEXT_ENCODING_KOI8_R, GTEXT_ENCODING_UTF_8, BOM, 3},
      {"utf-16be bom", std::string("\xfe\xff\0a", 4), "iso-8859-2", GTEXT_ENCODING_KOI8_R, GTEXT_ENCODING_UTF_16BE, BOM, 2},
      {"utf-16le bom", std::string("\xff\xfe" "a\0", 4), "utf-8", NONE, GTEXT_ENCODING_UTF_16LE, BOM, 2},
      {"utf-16le bom and nothing else", "\xff\xfe", nullptr, NONE, GTEXT_ENCODING_UTF_16LE, BOM, 2},
      {"a bom, a space, and then a charset", "\xef\xbb\xbf @charset \"iso-8859-5\";", nullptr, NONE, GTEXT_ENCODING_UTF_8, BOM, 3},
      {"two thirds of a bom", "\xef\xbb", nullptr, NONE, GTEXT_ENCODING_UTF_8, DEF, 0},
      {"half of a utf-16 bom", "\xff", nullptr, NONE, GTEXT_ENCODING_UTF_8, DEF, 0},
      {"fe alone", "\xfe" "a", nullptr, NONE, GTEXT_ENCODING_UTF_8, DEF, 0},
      {"ff fe is not utf-8 bytes", std::string("\xff\xfe\x00\x00", 4), nullptr, NONE, GTEXT_ENCODING_UTF_16LE, BOM, 2},
  };
}

TEST(CssDecode, TheEncodingAndTheReasonForIt) {
  for (const Decision & d : decisions()) {
    GTEXT_CSS_Bytes_Options o = gtext_css_bytes_options_default();
    o.protocol_label = d.protocol;
    o.environment_encoding = d.environment;
    GTEXT_CSS_Encoding_Decision got;
    std::memset(&got, 0x55, sizeof got);
    ASSERT_EQ(gtext_css_determine_encoding(d.bytes.data(), d.bytes.size(), &o, &got), GTEXT_CSS_OK) << d.name;
    EXPECT_EQ(got.encoding, d.encoding) << d.name;
    EXPECT_EQ(got.source, d.source) << d.name;
    EXPECT_EQ(got.bom_length, d.bom) << d.name;
    // The parse says the same, and the answer does not depend on what follows the first kilobyte.
    const Outcome whole = whole_bytes(d.bytes, &o);
    EXPECT_EQ(whole.status, GTEXT_CSS_OK) << d.name;
    EXPECT_EQ(whole.encoding, d.encoding) << d.name;
    EXPECT_EQ(whole.source, d.source) << d.name;
    EXPECT_EQ(whole.bom_length, d.bom) << d.name;
    EXPECT_EQ(whole.input_bytes, d.bytes.size()) << d.name;
    if (d.bytes.size() >= 1024) {
      GTEXT_CSS_Encoding_Decision window;
      ASSERT_EQ(gtext_css_determine_encoding(d.bytes.data(), 1024, &o, &window), GTEXT_CSS_OK);
      EXPECT_EQ(window.encoding, d.encoding) << d.name << ": only the first kilobyte decides";
      GTEXT_CSS_Encoding_Decision longer;
      const std::string more = d.bytes + std::string(5000, 'z');
      ASSERT_EQ(gtext_css_determine_encoding(more.data(), more.size(), &o, &longer), GTEXT_CSS_OK);
      EXPECT_EQ(longer.encoding, d.encoding) << d.name << ": nothing after it does";
    }
  }
}

TEST(CssDecode, ArgumentsAreChecked) {
  GTEXT_CSS_Encoding_Decision d;
  EXPECT_EQ(gtext_css_determine_encoding("a", 1, nullptr, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_determine_encoding(nullptr, 3, nullptr, &d), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_determine_encoding(nullptr, 0, nullptr, &d), GTEXT_CSS_OK);
  EXPECT_EQ(d.encoding, GTEXT_ENCODING_UTF_8);
  GTEXT_CSS_Bytes_Options o = gtext_css_bytes_options_default();
  o.environment_encoding = static_cast<GTEXT_Encoding>(9999);
  EXPECT_EQ(gtext_css_determine_encoding("a", 1, &o, &d), GTEXT_CSS_OK);
  EXPECT_EQ(d.source, GTEXT_CSS_ENCODING_SOURCE_DEFAULT) << "an environment encoding that is not one is none";
  GTEXT_CSS_Tree * t = nullptr;
  GTEXT_CSS_Result r{};
  EXPECT_EQ(gtext_css_parse_stylesheet_bytes("a", 1, nullptr, nullptr, nullptr, &r), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(r.status, GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_parse_stylesheet_bytes(nullptr, 2, nullptr, nullptr, &t, &r), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(t, nullptr);
  EXPECT_EQ(gtext_css_parse_stylesheet_bytes(nullptr, 0, nullptr, nullptr, &t, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(t->rule_count, 0u);
  gtext_css_tree_free(t);
  EXPECT_STREQ(gtext_css_encoding_source_name(GTEXT_CSS_ENCODING_SOURCE_BOM), "bom");
  EXPECT_STREQ(gtext_css_encoding_source_name(GTEXT_CSS_ENCODING_SOURCE_COUNT), "unknown");
  EXPECT_STREQ(gtext_css_encoding_source_name(GTEXT_CSS_ENCODING_SOURCE_NONE), "none");
}

TEST(CssDecode, ATextParseNamesNoEncoding) {
  const Outcome t = whole_text("a{b:c}");
  EXPECT_EQ(t.encoding, GTEXT_ENCODING_NONE);
  EXPECT_EQ(t.source, GTEXT_CSS_ENCODING_SOURCE_NONE);
  EXPECT_EQ(t.bom_length, 0u);
  const Outcome b = whole_bytes("a{b:c}");
  EXPECT_EQ(b.encoding, GTEXT_ENCODING_UTF_8);
  EXPECT_EQ(b.source, GTEXT_CSS_ENCODING_SOURCE_DEFAULT);
}

// ---- the text that is read ----------------------------------------------------------------------------------

std::string ident_values(const std::string & bytes, const GTEXT_CSS_Bytes_Options * o) {
  Parsed p;
  p.status = gtext_css_parse_stylesheet_bytes(bytes.data(), bytes.size(), o, nullptr, &p.tree, &p.result);
  if (p.status != GTEXT_CSS_OK || p.tree->rule_count == 0) {
    return "?";
  }
  return text_of(p.tree->rules[0].prelude, p.tree->rules[0].prelude_count);
}

TEST(CssDecode, TheBytesAreReadInTheEncodingThatWasChosen) {
  GTEXT_CSS_Bytes_Options o = gtext_css_bytes_options_default();
  // U+00E9 in windows-1252 and in UTF-8; U+0449 in ISO-8859-5; "a" in UTF-16.
  EXPECT_EQ(ident_values("@\xc3\xa9", &o), "") << "(an at-rule with nothing after it)";
  o.protocol_label = "iso-8859-5";
  EXPECT_EQ(ident_values("\xf9{}", &o), "ident:\xd1\x99"); // U+0459
  o.protocol_label = "utf-16le";
  EXPECT_EQ(ident_values(std::string("a\0b\0{\0}\0", 8), &o), "ident:ab");
  EXPECT_EQ(ident_values(std::string("\xff\xfe" "a\0{\0}\0", 8), &o), "ident:a") << "the mark wins over the label that agrees too";
  o.protocol_label = "utf-16be";
  EXPECT_EQ(ident_values(std::string("\xff\xfe" "a\0{\0}\0", 8), &o), "ident:a") << "and over one that does not";
  o.protocol_label = "utf-8";
  EXPECT_EQ(ident_values("\xef\xbb\xbf" "a{}", &o), "ident:a") << "and a UTF-8 mark is not a character";
  o.protocol_label = nullptr;
  EXPECT_EQ(ident_values("\xef\xbb\xbf\xef\xbb\xbf" "a{}", &o), "ident:\xef\xbb\xbf" "a") << "(only the first is a mark)";
  // An encoding the bytes are not: the replacement character, and no error of the parse for it.
  o.protocol_label = "windows-1252";
  EXPECT_EQ(ident_values("\x81{}", &o), "delim:\xc2\x81") << "(U+0081 is not an ident code point)";
  o.protocol_label = "shift_jis";
  EXPECT_EQ(ident_values("a\xff{}", &o), "ident:a\xef\xbf\xbd");
  o.protocol_label = "hz-gb-2312";
  EXPECT_EQ(ident_values("a{}", &o), "?") << "the replacement decoder gives one U+FFFD, which is an ident";
}

TEST(CssDecode, AnIllFormedUtf8SequenceIsAParseErrorButAnotherEncodingsIsNot) {
  Parsed p;
  const std::string bad = "a\xc3{b:c}";
  p.status = gtext_css_parse_stylesheet_bytes(bad.data(), bad.size(), nullptr, nullptr, &p.tree, &p.result);
  ASSERT_EQ(p.status, GTEXT_CSS_OK);
  EXPECT_EQ(p.error_names(), (std::vector<std::string>{"invalid-utf-8"}));
  EXPECT_EQ(p.errors()[0].byte_offset, 1u);
  GTEXT_CSS_Bytes_Options o = gtext_css_bytes_options_default();
  o.protocol_label = "shift_jis";
  Parsed q;
  const std::string sj = "a\x81 {b:c}";
  q.status = gtext_css_parse_stylesheet_bytes(sj.data(), sj.size(), &o, nullptr, &q.tree, &q.result);
  ASSERT_EQ(q.status, GTEXT_CSS_OK);
  EXPECT_EQ(q.errors().size(), 0u);
}

// ---- positions in the bytes ----------------------------------------------------------------------------------

struct MapDoc {
  const char * name;
  std::string bytes;
  const char * label;
  GTEXT_Encoding encoding;
  GTEXT_CSS_Encoding_Source source;
  std::vector<size_t> starts;
  std::vector<size_t> ends;
};

const std::vector<MapDoc> & map_documents() {
  static const std::vector<MapDoc> docs = {
      {"cp1251", CSS_BYTES("@charset \x22windows-1251\x22; .\xef\xf0\xe8\xe2\xe5\xf2{\xf6\xe2\xe5\xf2:\xea\xf0\xe0\xf1\xed\xfb\xe9} /* \xea\xee\xec\xec\xe5\xed\xf2\xe0\xf0\xe8\xe9 */ a::before{content:\x22\xb8\x22} x{y:\x22\xe6\x0a} z{w:url(\xe1\xe1 ) q}\x0a"), "", GTEXT_ENCODING_WINDOWS_1251, GTEXT_CSS_ENCODING_SOURCE_CHARSET_RULE,
       {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112},
       {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113}},
      {"utf-16le bom", CSS_BYTES("\xff\xfe" "a\x00{\x00" "b\x00:\x00\xe9\x00}\x00.\x00\xfc\x00{\x00" "c\x00:\x00\x22\x00=\xd8\x00\xde\x22\x00}\x00 \x00x\x00{\x00y\x00:\x00\x22\x00" "6\x04\x0a\x00}\x00 \x00/\x00*\x00 \x00z\x00 \x00*\x00/\x00 \x00@\x00m\x00 \x00\xfc\x00{\x00q\x00:\x00r\x00}\x00"), "", GTEXT_ENCODING_UTF_16LE, GTEXT_CSS_ENCODING_SOURCE_BOM,
       {2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 30, 32, 34, 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58, 60, 62, 64, 66, 68, 70, 72, 74, 76, 78, 80, 82, 84, 86},
       {4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 30, 32, 34, 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58, 60, 62, 64, 66, 68, 70, 72, 74, 76, 78, 80, 82, 84, 86, 88}},
      {"utf-16be protocol", CSS_BYTES("\x00" "a\x00{\x00" "b\x00:\x00\xe9\x00}\x00.\x00\xfc\x00{\x00" "c\x00:\x00\x22\xd8=\xde\x00\x00\x22\x00}\x00 \x00x\x00{\x00y\x00:\x00\x22\x04" "6\x00\x0a\x00}\x00 \x00/\x00*\x00 \x00z\x00 \x00*\x00/\x00 \x00@\x00m\x00 \x00\xfc\x00{\x00q\x00:\x00r\x00}"), "utf-16be", GTEXT_ENCODING_UTF_16BE, GTEXT_CSS_ENCODING_SOURCE_PROTOCOL,
       {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 28, 30, 32, 34, 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58, 60, 62, 64, 66, 68, 70, 72, 74, 76, 78, 80, 82, 84},
       {2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 28, 30, 32, 34, 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58, 60, 62, 64, 66, 68, 70, 72, 74, 76, 78, 80, 82, 84, 86}},
      {"shift_jis", CSS_BYTES(".\x93\xfa\x96{{x:y} a{b:\x22\xb1\xb2\x22} /*\x94\x5c*/ x{y:\x22\x95\x5c\x0a}"), "shift_jis", GTEXT_ENCODING_SHIFT_JIS, GTEXT_CSS_ENCODING_SOURCE_PROTOCOL,
       {0, 1, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 25, 26, 27, 28, 29, 30, 31, 32, 33, 35, 36},
       {1, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 25, 26, 27, 28, 29, 30, 31, 32, 33, 35, 36, 37}},
      {"gb18030", CSS_BYTES(".\xd6\xd0\xce\xc4{b:c} /*\x95" "2\x82" "6*/ x{y:\x22\x95" "2\x82" "6z\x0a}"), "gb18030", GTEXT_ENCODING_GB18030, GTEXT_CSS_ENCODING_SOURCE_PROTOCOL,
       {0, 1, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 17, 18, 19, 20, 21, 22, 23, 24, 25, 29, 30, 31},
       {1, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 17, 18, 19, 20, 21, 22, 23, 24, 25, 29, 30, 31, 32}},
      {"euc-kr", CSS_BYTES(".\xc7\xd1\xb1\xb9{x:y} /*\xc7\xd1*/ q{r:\x22\xc7\xd1\x0a}"), "euc-kr", GTEXT_ENCODING_EUC_KR, GTEXT_CSS_ENCODING_SOURCE_PROTOCOL,
       {0, 1, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16, 17, 18, 19, 20, 21, 22, 23, 25, 26},
       {1, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16, 17, 18, 19, 20, 21, 22, 23, 25, 26, 27}},
      {"big5", CSS_BYTES(".\xa4\xa4\xa4\xe5{x:y} /*\xa4\xa4*/ q{r:\x22\xa4\xa4\x0a}"), "big5", GTEXT_ENCODING_BIG5, GTEXT_CSS_ENCODING_SOURCE_PROTOCOL,
       {0, 1, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16, 17, 18, 19, 20, 21, 22, 23, 25, 26},
       {1, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16, 17, 18, 19, 20, 21, 22, 23, 25, 26, 27}},
      {"iso-2022-jp", CSS_BYTES("a{b:c}/*\x1b$BF|K\x5c" "8l\x1b(B*/.\x1b$BF|K\x5c\x1b(B{d:e} x{y:\x22\x1b$BF|\x1b(B\x0a}"), "iso-2022-jp", GTEXT_ENCODING_ISO_2022_JP, GTEXT_CSS_ENCODING_SOURCE_PROTOCOL,
       {0, 1, 2, 3, 4, 5, 6, 7, 11, 13, 15, 20, 21, 22, 26, 28, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 47, 52, 53},
       {1, 2, 3, 4, 5, 6, 7, 8, 13, 15, 17, 21, 22, 23, 28, 30, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 49, 53, 54}},
      {"iso-2022-jp trailing shift", CSS_BYTES("a{b:c}/*\x1b$BF|\x1b(B*/ @m \x1b$BK\x5c\x1b(B"), "iso-2022-jp", GTEXT_ENCODING_ISO_2022_JP, GTEXT_CSS_ENCODING_SOURCE_PROTOCOL,
       {0, 1, 2, 3, 4, 5, 6, 7, 11, 16, 17, 18, 19, 20, 21, 25},
       {1, 2, 3, 4, 5, 6, 7, 8, 13, 17, 18, 19, 20, 21, 22, 27}},
  };
  return docs;
}

size_t code_points_before(const std::string & text, size_t offset) {
  size_t n = 0;
  for (size_t i = 0; i < offset && i < text.size(); i++) {
    if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
      n++;
    }
  }
  return n;
}

TEST(CssBytes, EverySpanAndEveryErrorIsInTheBytes) {
  for (const MapDoc & doc : map_documents()) {
    SCOPED_TRACE(doc.name);
    GTEXT_CSS_Bytes_Options o = gtext_css_bytes_options_default();
    o.protocol_label = *doc.label ? doc.label : nullptr;
    // The text the bytes decode to (the Encoding module's own decoder; what is tested here is the positions).
    char * text_buf = nullptr;
    size_t text_len = 0;
    GTEXT_Decoder_Options dopts = gtext_decoder_options_default();
    dopts.bom_sniffing = true;
    ASSERT_EQ(gtext_encoding_decode_buffer(doc.encoding, &dopts, doc.bytes.data(), doc.bytes.size(), &text_buf, &text_len, nullptr),
        GTEXT_ENCODING_OK);
    const std::string text(text_buf, text_len);
    gtext_encoding_buffer_free(text_buf);
    ASSERT_EQ(code_points_before(text, text.size()), doc.starts.size()) << "the table has one entry per character";

    Parsed text_tree, byte_tree;
    parse_into(text_tree, gtext_css_parse_stylesheet, text);
    byte_tree.status = gtext_css_parse_stylesheet_bytes(doc.bytes.data(), doc.bytes.size(), &o, nullptr, &byte_tree.tree,
        &byte_tree.result);
    ASSERT_EQ(text_tree.status, GTEXT_CSS_OK);
    ASSERT_EQ(byte_tree.status, GTEXT_CSS_OK);
    EXPECT_EQ(byte_tree.result.encoding, doc.encoding);
    EXPECT_EQ(byte_tree.result.encoding_source, doc.source);
    EXPECT_EQ(canon(text_tree.tree), canon(byte_tree.tree)) << "the same tree as the text's";

    auto start_of = [&](size_t d) { return d >= text.size() ? doc.bytes.size() : doc.starts[code_points_before(text, d)]; };
    auto end_of = [&](size_t d) {
      if (d >= text.size()) {
        return doc.bytes.size();
      }
      return d == 0 ? start_of(0) : doc.ends[code_points_before(text, d) - 1];
    };
    const std::vector<size_t> want = collect_spans(text_tree.tree);
    const std::vector<size_t> got = collect_spans(byte_tree.tree);
    ASSERT_EQ(want.size(), got.size());
    ASSERT_GT(want.size(), 20u) << "the document has a good many spans";
    for (size_t i = 0; i < want.size(); i += 2) {
      const size_t s = start_of(want[i]);
      size_t e = want[i + 1] == want[i] ? s : end_of(want[i + 1]);
      if (e < s) {
        e = s;
      }
      EXPECT_EQ(got[i], s) << "span " << i / 2 << " starts at text offset " << want[i];
      EXPECT_EQ(got[i + 1], e) << "span " << i / 2 << " ends at text offset " << want[i + 1];
    }

    // The parse errors, and their lines and columns, which count characters.
    const auto want_errors = text_tree.errors();
    const auto got_errors = byte_tree.errors();
    ASSERT_EQ(want_errors.size(), got_errors.size());
    if (std::string(doc.name) != "iso-2022-jp trailing shift") {
      EXPECT_GE(want_errors.size(), 1u) << "(the documents have a bad string)";
    }
    for (size_t i = 0; i < want_errors.size(); i++) {
      EXPECT_EQ(got_errors[i].code, want_errors[i].code);
      EXPECT_EQ(got_errors[i].byte_offset, start_of(want_errors[i].byte_offset)) << "error " << i;
      EXPECT_EQ(got_errors[i].line, want_errors[i].line);
      EXPECT_EQ(got_errors[i].column, want_errors[i].column);
    }
  }
}

TEST(CssBytes, ATailOfShiftSequencesIsInTheLastSpansEndButNotInAnyTokenBeforeIt) {
  // "a{b:c}/*<ESC $ B>日<ESC ( B>*/ @m <ESC $ B>本<ESC ( B>": the last token ends at the end of the bytes.
  const MapDoc * doc = nullptr;
  for (const MapDoc & d : map_documents()) {
    if (std::string(d.name) == "iso-2022-jp trailing shift") {
      doc = &d;
    }
  }
  ASSERT_NE(doc, nullptr);
  GTEXT_CSS_Bytes_Options o = gtext_css_bytes_options_default();
  o.protocol_label = "iso-2022-jp";
  Parsed p;
  p.status = gtext_css_parse_stylesheet_bytes(doc->bytes.data(), doc->bytes.size(), &o, nullptr, &p.tree, &p.result);
  ASSERT_EQ(p.status, GTEXT_CSS_OK);
  ASSERT_EQ(p.tree->rule_count, 2u);
  EXPECT_EQ(p.tree->rules[0].span.offset, 0u);
  EXPECT_EQ(p.tree->rules[0].span.end, 6u);
  // The second rule is an at-rule whose prelude ends with the character, and the end of the input ends it.
  EXPECT_EQ(p.tree->rules[1].span.end, doc->bytes.size());
}

TEST(CssBytes, AByteOrderMarkIsCountedInEveryOffsetAndNoTokenCoversIt) {
  const std::string css = "a{b:c} d{e:\"f\n}";
  const Outcome plain = whole_bytes(css);
  const Outcome marked = whole_bytes("\xef\xbb\xbf" + css);
  EXPECT_EQ(marked.bom_length, 3u);
  EXPECT_EQ(plain.errors.size(), 1u);
  ASSERT_EQ(marked.errors.size(), 1u);
  Parsed p, q;
  p.status = gtext_css_parse_stylesheet_bytes(css.data(), css.size(), nullptr, nullptr, &p.tree, &p.result);
  const std::string m = "\xef\xbb\xbf" + css;
  q.status = gtext_css_parse_stylesheet_bytes(m.data(), m.size(), nullptr, nullptr, &q.tree, &q.result);
  ASSERT_EQ(q.status, GTEXT_CSS_OK);
  ASSERT_EQ(q.tree->rule_count, p.tree->rule_count);
  for (size_t i = 0; i < p.tree->rule_count; i++) {
    EXPECT_EQ(q.tree->rules[i].span.offset, p.tree->rules[i].span.offset + 3);
    EXPECT_EQ(q.tree->rules[i].span.end, p.tree->rules[i].span.end + 3);
    EXPECT_EQ(q.tree->rules[i].span.line, p.tree->rules[i].span.line);
    EXPECT_EQ(q.tree->rules[i].span.column, p.tree->rules[i].span.column) << "the mark is not a character of the text";
  }
  EXPECT_EQ(q.errors()[0].byte_offset, p.errors()[0].byte_offset + 3);
  EXPECT_EQ(q.errors()[0].column, p.errors()[0].column);
  // And an input of nothing else.
  const Outcome only = whole_bytes("\xef\xbb\xbf");
  EXPECT_EQ(only.status, GTEXT_CSS_OK);
  EXPECT_EQ(only.bom_length, 3u);
  const Outcome only16 = whole_bytes(CSS_BYTES("\xff\xfe"));
  EXPECT_EQ(only16.status, GTEXT_CSS_OK);
  EXPECT_EQ(only16.encoding, GTEXT_ENCODING_UTF_16LE);
}

TEST(CssBytes, ALimitReachedInAnotherEncodingIsAtTheBytesTheLimitIsAt) {
  // The third token limit is hit at "d" of "a{b:c} d{...": its place in the bytes of a UTF-16 stylesheet is twice its place in the text.
  const std::string text = "a{} d{} e{} f{}";
  std::string utf16 = CSS_BYTES("\xff\xfe");
  for (char c : text) {
    utf16 += c;
    utf16 += '\0';
  }
  GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
  o.max_rules = 2;
  const Outcome as_text = whole_text(text, gtext_css_parse_stylesheet, &o);
  const Outcome as_bytes = whole_bytes(utf16, nullptr, &o);
  ASSERT_EQ(as_text.status, GTEXT_CSS_E_LIMIT_RULES);
  ASSERT_EQ(as_bytes.status, GTEXT_CSS_E_LIMIT_RULES);
  EXPECT_EQ(as_text.failure.offset, 8u);
  EXPECT_EQ(as_bytes.failure.offset, 2u + 2u * 8u);
  EXPECT_EQ(as_bytes.failure.line, as_text.failure.line);
  EXPECT_EQ(as_bytes.failure.column, as_text.failure.column);
  EXPECT_EQ(as_bytes.encoding, GTEXT_ENCODING_UTF_16LE) << "a failed parse still says how it read";
  EXPECT_EQ(as_bytes.input_bytes, utf16.size());
}

// ---- the validity seam, and @charset ---------------------------------------------------------------------------

bool refuse_charset(void *, const GTEXT_CSS_Parent *, const GTEXT_CSS_Rule * r) {
  return !(r->kind == GTEXT_CSS_RULE_AT && std::string(r->name.data, r->name.length) == "charset");
}

TEST(CssBytes, ACharsetRuleIsTheGenericAtRuleAndTheLayerThatKnowsRulesDropsIt) {
  // CSS Syntax 8.3: "there is no actual at-rule named @charset. When a stylesheet is actually parsed, any
  // occurrences of an @charset rule must be treated as an unrecognized rule, and thus dropped as invalid when
  // the stylesheet is grammar-checked." The syntax layer reads the generic at-rule; the grammar check is the
  // validity callback's.
  const std::string css = "@charset \"windows-1251\"; a{b:c} @charset \"utf-8\";";
  GTEXT_CSS_Bytes_Options bo = gtext_css_bytes_options_default();
  Parsed kept;
  kept.status = gtext_css_parse_stylesheet_bytes(css.data(), css.size(), &bo, nullptr, &kept.tree, &kept.result);
  ASSERT_EQ(kept.status, GTEXT_CSS_OK);
  ASSERT_EQ(kept.tree->rule_count, 3u);
  EXPECT_EQ(str(kept.tree->rules[0].name), "charset");
  EXPECT_EQ(kept.result.encoding, GTEXT_ENCODING_WINDOWS_1251) << "the declaration decided the encoding, rule or not";
  GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
  o.validity.rule = refuse_charset;
  Parsed dropped;
  dropped.status = gtext_css_parse_stylesheet_bytes(css.data(), css.size(), &bo, &o, &dropped.tree, &dropped.result);
  ASSERT_EQ(dropped.status, GTEXT_CSS_OK);
  ASSERT_EQ(dropped.tree->rule_count, 1u);
  EXPECT_EQ(dropped.tree->rules[0].declaration_count, 1u);
  EXPECT_EQ(dropped.result.encoding, GTEXT_ENCODING_WINDOWS_1251);
  EXPECT_EQ(dropped.error_names(), (std::vector<std::string>{"invalid-rule", "invalid-rule"}));
  EXPECT_EQ(dropped.errors()[0].byte_offset, 0u);
  EXPECT_EQ(dropped.errors()[1].byte_offset, 32u);
}

struct SeenOffsets {
  std::vector<size_t> offsets;
};

bool record(void * user, const GTEXT_CSS_Parent *, const GTEXT_CSS_Rule * r) {
  static_cast<SeenOffsets *>(user)->offsets.push_back(r->span.offset);
  return true;
}

TEST(CssBytes, AValidityCallbackSeesTheDecodedTextsOffsetsAndTheTreeTheBytes) {
  // Documented: the callbacks run during the parse, over the decoded text. For UTF-8 with no mark that is the bytes.
  const std::string css = "a{} b{}";
  SeenOffsets plain, marked;
  GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
  o.validity.rule = record;
  o.validity.user = &plain;
  EXPECT_EQ(whole_bytes(css, nullptr, &o).status, GTEXT_CSS_OK);
  o.validity.user = &marked;
  Parsed p;
  const std::string m = "\xef\xbb\xbf" + css;
  p.status = gtext_css_parse_stylesheet_bytes(m.data(), m.size(), nullptr, &o, &p.tree, &p.result);
  ASSERT_EQ(p.status, GTEXT_CSS_OK);
  EXPECT_EQ(plain.offsets, (std::vector<size_t>{0, 4}));
  EXPECT_EQ(marked.offsets, (std::vector<size_t>{0, 4})) << "the callback saw the text's offsets";
  EXPECT_EQ(p.tree->rules[0].span.offset, 3u) << "and the tree has the bytes'";
  EXPECT_EQ(p.tree->rules[1].span.offset, 7u);
}

// ---- the hostile inputs of a decoder ------------------------------------------------------------------------------

TEST(CssBytes, EveryByteOfEveryEncodingParsesAndGivesTheSameAsAStream) {
  // All 256 byte values, alone and in pairs and triples, in every encoding the module reads: no crash, no
  // failure (CSS has no syntax error), and the stream agrees.
  std::string all;
  for (int c = 0; c < 256; c++) {
    all += static_cast<char>(c);
  }
  std::string text = "a{b:\"";
  for (size_t i = 0; i < 100; i++) {
    text += static_cast<char>(0x81 + (i * 7) % 126);
    text += static_cast<char>(0x30 + (i * 13) % 0x4f);
  }
  text += "\"}";
  for (size_t e = 1; e < GTEXT_ENCODING_COUNT; e++) {
    GTEXT_CSS_Bytes_Options o = gtext_css_bytes_options_default();
    o.environment_encoding = static_cast<GTEXT_Encoding>(e);
    for (const std::string & in : {all, text, std::string("\x1b$B\x30\x21\x1b(B a{} \x1b$"), std::string("\xff\xfe\x00\xd8", 4)}) {
      if (in[0] == '\xff' && in.size() == 4) {
        // a byte order mark overrides the environment; fine, the same check
      }
      const Outcome whole = whole_bytes(in, &o);
      ASSERT_EQ(whole.status, GTEXT_CSS_OK) << gtext_encoding_name(static_cast<GTEXT_Encoding>(e));
      const Cut bytewise{{1}, true, false, "bytes"};
      expect_same_outcome(whole, through_stream(GTEXT_CSS_TREE_STYLESHEET, true, &o, nullptr, in, bytewise),
          std::string("encoding ") + gtext_encoding_name(static_cast<GTEXT_Encoding>(e)));
    }
  }
}

} // namespace
