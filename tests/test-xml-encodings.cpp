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
 * gtext_xml_parse() across encodings: UTF-16 and UTF-32 (by mark or by the first
 * bytes), declared ISO-8859-1 and US-ASCII, the declaration's label checked
 * against what the document is in, malformed units, positions, and allocation
 * failure while converting.
 *
 * The strongest check is the round trip: a generated document is encoded in each
 * supported encoding, parsed, and its tree compared with the tree of the same
 * document parsed as UTF-8.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/xml.h>

#include "../src/markup/markup_internal.h"
#include "markup_test_support.h"

namespace {

using Doc = GTEXT_Markup_Document;

struct Result {
  GTEXT_XML_Status status = GTEXT_XML_OK;
  Doc * doc = nullptr;
  GTEXT_XML_Error err{};
};

class XmlEncodings : public ::testing::Test {
 protected:
  std::vector<Doc *> docs;

  Result parse(const std::string & text, const GTEXT_XML_Parse_Options * opts = nullptr) {
    Result r;
    r.status = gtext_xml_parse(text.data(), text.size(), opts, &r.doc, &r.err);
    if (r.status == GTEXT_XML_OK) {
      docs.push_back(r.doc);
      const char * problem = markup_check_invariants(r.doc);
      EXPECT_STREQ(problem ? problem : "ok", "ok");
    }
    else {
      EXPECT_EQ(r.doc, nullptr);
      EXPECT_EQ(r.err.code, r.status);
    }
    return r;
  }

  void TearDown() override {
    for (Doc * d : docs) {
      gtext_markup_document_free(d);
    }
  }
};

// ---- encoders ------------------------------------------------------------

std::vector<uint32_t> code_points(const std::string & utf8) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i < utf8.size();) {
    const unsigned char b = (unsigned char)utf8[i];
    uint32_t cp;
    size_t n;
    if (b < 0x80) { cp = b; n = 1; }
    else if (b < 0xE0) { cp = b & 0x1F; n = 2; }
    else if (b < 0xF0) { cp = b & 0x0F; n = 3; }
    else { cp = b & 0x07; n = 4; }
    for (size_t k = 1; k < n; k++) {
      cp = (cp << 6) | ((unsigned char)utf8[i + k] & 0x3F);
    }
    out.push_back(cp);
    i += n;
  }
  return out;
}

void put16(std::string & s, uint32_t u, bool big) {
  s += (char)(big ? u >> 8 : u & 0xFF);
  s += (char)(big ? u & 0xFF : u >> 8);
}

std::string utf16(const std::string & utf8, bool big, bool bom) {
  std::string out;
  if (bom) {
    put16(out, 0xFEFF, big);
  }
  for (uint32_t cp : code_points(utf8)) {
    if (cp >= 0x10000) {
      cp -= 0x10000;
      put16(out, 0xD800 + (cp >> 10), big);
      put16(out, 0xDC00 + (cp & 0x3FF), big);
    }
    else {
      put16(out, cp, big);
    }
  }
  return out;
}

std::string utf32(const std::string & utf8, bool big, bool bom) {
  std::string out;
  auto put = [&](uint32_t u) {
    for (int k = 0; k < 4; k++) {
      out += (char)(big ? u >> (24 - 8 * k) : u >> (8 * k));
    }
  };
  if (bom) {
    put(0xFEFF);
  }
  for (uint32_t cp : code_points(utf8)) {
    put(cp);
  }
  return out;
}

std::string single_byte(const std::string & utf8) {
  std::string out;
  for (uint32_t cp : code_points(utf8)) {
    out += (char)cp;
  }
  return out;
}

std::string dump(Doc * doc) {
  return dump_to_string(gtext_markup_document_root(doc));
}

std::string encoding_of(Doc * doc, GTEXT_Markup_Encoding_Source * source) {
  const char * name = gtext_markup_document_encoding(doc, source);
  return name ? name : "";
}

// ---- the matrix ----------------------------------------------------------

TEST_F(XmlEncodings, Utf16ByMark) {
  Result r = parse(std::string("\xFF\xFE<\0a\0/\0>\0", 10));
  ASSERT_EQ(r.status, GTEXT_XML_OK);
  GTEXT_Markup_Encoding_Source source;
  EXPECT_EQ(encoding_of(r.doc, &source), "UTF-16");
  EXPECT_EQ(source, GTEXT_MARKUP_ENCODING_BOM);
  r = parse(std::string("\xFE\xFF\0<\0a\0/\0>", 10));
  ASSERT_EQ(r.status, GTEXT_XML_OK);
  EXPECT_EQ(encoding_of(r.doc, &source), "UTF-16");
  EXPECT_EQ(source, GTEXT_MARKUP_ENCODING_BOM);
  // The mark is not a character: the root starts at line 1, column 1, and the
  // offsets are those of the UTF-8 form.
  GTEXT_Markup_Location where;
  ASSERT_TRUE(gtext_markup_node_location(gtext_markup_document_element(r.doc), &where));
  EXPECT_EQ(where.offset, 0u);
  EXPECT_EQ(where.line, 1u);
  EXPECT_EQ(where.column, 1u);
}

TEST_F(XmlEncodings, Utf16WithoutMarkNeedsTheDeclaration) {
  const std::string text = "<?xml version=\"1.0\" encoding=\"UTF-16\"?><a/>";
  for (bool big : {false, true}) {
    Result r = parse(utf16(text, big, false));
    ASSERT_EQ(r.status, GTEXT_XML_OK) << big;
    GTEXT_Markup_Encoding_Source source;
    EXPECT_EQ(encoding_of(r.doc, &source), "UTF-16");
    EXPECT_EQ(source, GTEXT_MARKUP_ENCODING_DECLARATION);
    // With the mark as well, the mark is the source.
    r = parse(utf16(text, big, true));
    ASSERT_EQ(r.status, GTEXT_XML_OK);
    EXPECT_EQ(encoding_of(r.doc, &source), "UTF-16");
    EXPECT_EQ(source, GTEXT_MARKUP_ENCODING_BOM);
  }
}

TEST_F(XmlEncodings, Utf32) {
  const std::string bare = "<a>x</a>";
  const std::string declared = "<?xml version=\"1.0\" encoding=\"utf-32\"?><a>x</a>";
  const std::string expected = dump(parse(bare).doc);
  for (bool big : {false, true}) {
    for (bool bom : {false, true}) {
      for (const std::string * text : {&bare, &declared}) {
        Result r = parse(utf32(*text, big, bom));
        ASSERT_EQ(r.status, GTEXT_XML_OK) << big << bom << *text;
        GTEXT_Markup_Encoding_Source source;
        const std::string name = encoding_of(r.doc, &source);
        EXPECT_EQ(source, bom ? GTEXT_MARKUP_ENCODING_BOM
                              : (text == &declared ? GTEXT_MARKUP_ENCODING_DECLARATION
                                                   : GTEXT_MARKUP_ENCODING_DEFAULT));
        EXPECT_EQ(name, text == &declared ? "utf-32" : "UTF-32");
        EXPECT_EQ(dump(r.doc), expected);
      }
    }
  }
}

TEST_F(XmlEncodings, Latin1) {
  Result r = parse("<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><a>caf\xE9</a>");
  ASSERT_EQ(r.status, GTEXT_XML_OK);
  GTEXT_Markup_Node * a = gtext_markup_document_element(r.doc);
  ASSERT_NE(a, nullptr);
  const std::string tree = dump(r.doc);
  EXPECT_NE(tree.find("caf\xC3\xA9"), std::string::npos) << tree;
  GTEXT_Markup_Encoding_Source source;
  EXPECT_EQ(encoding_of(r.doc, &source), "ISO-8859-1");
  EXPECT_EQ(source, GTEXT_MARKUP_ENCODING_DECLARATION);
  // Case-insensitive, either quote, and every byte is a character.
  r = parse("<?xml version='1.0' encoding='iso-8859-1'?><a>\xA0\xFF\x80</a>");
  EXPECT_EQ(r.status, GTEXT_XML_OK);
}

TEST_F(XmlEncodings, Ascii) {
  Result r = parse("<?xml version=\"1.0\" encoding=\"us-ascii\"?><a>plain</a>");
  ASSERT_EQ(r.status, GTEXT_XML_OK);
  GTEXT_Markup_Encoding_Source source;
  EXPECT_EQ(encoding_of(r.doc, &source), "us-ascii");
  EXPECT_EQ(source, GTEXT_MARKUP_ENCODING_DECLARATION);
  // A byte above 0x7F is refused, at its own position (line 2, column 4).
  r = parse("<?xml version=\"1.0\" encoding=\"US-ASCII\"?>\n<a>\x80</a>");
  EXPECT_EQ(r.status, GTEXT_XML_E_INVALID_SEQUENCE);
  EXPECT_EQ(r.err.line, 2u);
  EXPECT_EQ(r.err.column, 4u);
  EXPECT_EQ(r.err.byte_offset, 45u);  // 42-byte declaration, newline, "<a>"
}

TEST_F(XmlEncodings, MismatchesAreFatal) {
  // UTF-16 or UTF-32 documents labeled as another family.
  for (const char * label : {"UTF-8", "ISO-8859-1", "US-ASCII", "UTF-32"}) {
    const std::string text =
        std::string("<?xml version=\"1.0\" encoding=\"") + label + "\"?><a/>";
    Result r = parse(utf16(text, false, true));
    EXPECT_EQ(r.status, GTEXT_XML_E_ENCODING_MISMATCH) << label;
    EXPECT_EQ(r.err.byte_offset, 30u) << label;  // the label, as for UTF-8
    EXPECT_EQ(r.err.column, 31u) << label;
    r = parse(utf16(text, true, false));
    EXPECT_EQ(r.status, GTEXT_XML_E_ENCODING_MISMATCH) << label;
  }
  // The other way: UTF-8 documents labeled UTF-16 or UTF-32, with or without a mark.
  for (const char * label : {"UTF-16", "UTF-32", "utf-16"}) {
    const std::string text =
        std::string("<?xml version=\"1.0\" encoding=\"") + label + "\"?><a/>";
    Result r = parse(text);
    EXPECT_EQ(r.status, GTEXT_XML_E_ENCODING_MISMATCH) << label;
    EXPECT_EQ(r.err.byte_offset, 30u);
    r = parse("\xEF\xBB\xBF" + text);
    EXPECT_EQ(r.status, GTEXT_XML_E_ENCODING_MISMATCH) << label;
    EXPECT_EQ(r.err.byte_offset, 33u);
    EXPECT_EQ(r.err.column, 31u);
  }
  // A mark outranks a single-byte label.
  EXPECT_EQ(parse("\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><a/>").status,
      GTEXT_XML_E_ENCODING_MISMATCH);
  // A bare UTF-16 or UTF-32 label matches either order, and a label of a
  // family the parser has no reader for is unsupported, not a mismatch.
  EXPECT_EQ(parse(utf16("<?xml version=\"1.0\" encoding=\"UTF-16\"?><a/>", true, false)).status,
      GTEXT_XML_OK);
  EXPECT_EQ(parse(utf16("<?xml version=\"1.0\" encoding=\"windows-1252\"?><a/>", false, true)).status,
      GTEXT_XML_E_UNSUPPORTED_ENCODING);
  EXPECT_EQ(parse(utf32("<?xml version=\"1.0\" encoding=\"UTF-16\"?><a/>", false, true)).status,
      GTEXT_XML_E_ENCODING_MISMATCH);
}

TEST_F(XmlEncodings, OtherAndEbcdicAreUnsupported) {
  EXPECT_EQ(parse("<?xml version=\"1.0\" encoding=\"windows-1252\"?><a/>").status,
      GTEXT_XML_E_UNSUPPORTED_ENCODING);
  EXPECT_EQ(parse("<?xml version=\"1.0\" encoding=\"Shift_JIS\"?><a/>").status,
      GTEXT_XML_E_UNSUPPORTED_ENCODING);
  Result r = parse(std::string("\x4C\x6F\xA7\x94", 4));
  EXPECT_EQ(r.status, GTEXT_XML_E_UNSUPPORTED_ENCODING);
  EXPECT_EQ(r.err.byte_offset, 0u);
}

TEST_F(XmlEncodings, BadUnitsAreRefused) {
  const std::string head = utf16("<a>", false, true);
  const std::string tail = utf16("</a>", false, false);
  struct Case {
    const char * name;
    std::string middle;
  };
  const Case cases[] = {
      {"lone high surrogate", std::string("\x00\xD8", 2)},
      {"lone low surrogate", std::string("\x00\xDC", 2)},
      {"reversed pair", std::string("\x00\xDC\x00\xD8", 4)},
      {"high followed by a plain unit", std::string("\x00\xD8\x41\x00", 4)},
  };
  for (const Case & c : cases) {
    Result r = parse(head + c.middle + tail);
    EXPECT_EQ(r.status, GTEXT_XML_E_INVALID_SEQUENCE) << c.name;
  }
  // A high surrogate at the very end, and an odd number of bytes.
  EXPECT_EQ(parse(head + std::string("\x00\xD8", 2)).status, GTEXT_XML_E_INVALID_SEQUENCE);
  EXPECT_EQ(parse(utf16("<a/>", false, true) + "x").status, GTEXT_XML_E_INVALID_SEQUENCE);
  EXPECT_EQ(parse(std::string("\xFF\xFE<", 3)).status, GTEXT_XML_E_INVALID_SEQUENCE);
  // The position is the bad unit's: line 1, column 4 (after "<a>"), 3 bytes in.
  Result r = parse(head + std::string("\x00\xD8", 2) + tail);
  EXPECT_EQ(r.err.line, 1u);
  EXPECT_EQ(r.err.column, 4u);
  EXPECT_EQ(r.err.byte_offset, 3u);

  // UTF-32: past U+10FFFF, a surrogate, a truncated unit.
  const std::string head32 = utf32("<a>", true, true);
  EXPECT_EQ(parse(head32 + std::string("\x00\x11\x00\x00", 4)).status, GTEXT_XML_E_INVALID_SEQUENCE);
  EXPECT_EQ(parse(head32 + std::string("\xFF\xFF\xFF\xFF", 4)).status, GTEXT_XML_E_INVALID_SEQUENCE);
  EXPECT_EQ(parse(head32 + std::string("\x00\x00\xD8\x00", 4)).status, GTEXT_XML_E_INVALID_SEQUENCE);
  EXPECT_EQ(parse(head32 + std::string("\x00\x00\xDF\xFF", 4)).status, GTEXT_XML_E_INVALID_SEQUENCE);
  EXPECT_EQ(parse(head32 + std::string("\x00\x00\x00", 3)).status, GTEXT_XML_E_INVALID_SEQUENCE);
  // The last scalar values convert (U+10FFFE and U+10FFFF are characters XML allows).
  for (const char * last : {"\x00\x10\xFF\xFE", "\x00\x10\xFF\xFF"}) {
    EXPECT_EQ(parse(head32 + std::string(last, 4) + utf32("</a>", true, false)).status,
        GTEXT_XML_OK);
  }
}

TEST_F(XmlEncodings, LinesColumnsAndOffsets) {
  // Characters, not bytes or units: the astral character is one column.
  const std::string text = "<a>\n<b>\xF0\x9F\x98\x80<c/></b>\r\n</a>";
  for (int e = 0; e < 4; e++) {
    std::string bytes = e == 0 ? utf16(text, false, true)
        : e == 1 ? utf16(text, true, true)
        : e == 2 ? utf32(text, false, true) : utf32(text, true, true);
    Result r = parse(bytes);
    ASSERT_EQ(r.status, GTEXT_XML_OK) << e;
    GTEXT_Markup_Node * a = gtext_markup_document_element(r.doc);
    GTEXT_Markup_Node * b = nullptr;
    for (GTEXT_Markup_Node * k = gtext_markup_node_first_child(a); k; k = gtext_markup_node_next_sibling(k)) {
      if (gtext_markup_node_kind(k) == GTEXT_MARKUP_KIND_ELEMENT) {
        b = k;
      }
    }
    ASSERT_NE(b, nullptr);
    GTEXT_Markup_Node * c = nullptr;
    for (GTEXT_Markup_Node * k = gtext_markup_node_first_child(b); k; k = gtext_markup_node_next_sibling(k)) {
      if (gtext_markup_node_kind(k) == GTEXT_MARKUP_KIND_ELEMENT) {
        c = k;
      }
    }
    ASSERT_NE(c, nullptr);
    GTEXT_Markup_Location where;
    ASSERT_TRUE(gtext_markup_node_location(c, &where));
    EXPECT_EQ(where.line, 2u) << e;
    EXPECT_EQ(where.column, 5u) << e;  // "<b>" and one character
    EXPECT_EQ(where.offset, text.find("<c/>")) << e;  // bytes of the UTF-8 form
  }
  // An error after the line end CR LF.
  Result r = parse(utf16("<a>\r\n</b>", false, true));
  EXPECT_EQ(r.status, GTEXT_XML_E_MISMATCHED_TAG);
  EXPECT_EQ(r.err.line, 2u);
  EXPECT_EQ(r.err.column, 1u);
  EXPECT_EQ(r.err.byte_offset, 5u);
}

TEST_F(XmlEncodings, TheInputLimitCountsOriginalBytes) {
  GTEXT_XML_Parse_Options o = gtext_xml_parse_options_default();
  const std::string wide = utf32("<a/>", false, true);  // 20 bytes, 4 as UTF-8
  o.max_input_bytes = 20;
  EXPECT_EQ(parse(wide, &o).status, GTEXT_XML_OK);
  o.max_input_bytes = 19;
  EXPECT_EQ(parse(wide, &o).status, GTEXT_XML_E_LIMIT_INPUT);
}

// ---- round trip ----------------------------------------------------------

struct Rng {
  uint64_t s;
  uint32_t next() {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(s >> 33);
  }
  uint32_t below(uint32_t n) { return next() % n; }
};

void put_utf8(std::string & s, uint32_t cp) {
  if (cp < 0x80) {
    s += (char)cp;
  }
  else if (cp < 0x800) {
    s += (char)(0xC0 | cp >> 6);
    s += (char)(0x80 | (cp & 0x3F));
  }
  else if (cp < 0x10000) {
    s += (char)(0xE0 | cp >> 12);
    s += (char)(0x80 | ((cp >> 6) & 0x3F));
    s += (char)(0x80 | (cp & 0x3F));
  }
  else {
    s += (char)(0xF0 | cp >> 18);
    s += (char)(0x80 | ((cp >> 12) & 0x3F));
    s += (char)(0x80 | ((cp >> 6) & 0x3F));
    s += (char)(0x80 | (cp & 0x3F));
  }
}

// A random character that is legal in text and attribute values, at most `max`.
uint32_t random_char(Rng & rng, uint32_t max) {
  static const uint32_t pool[] = {0x41, 0x7A, 0x20, 0x0A, 0x09, 0x30, 0x7E, 0xE9, 0xFF, 0xA0,
      0x100, 0x3A9, 0x7FF, 0x800, 0x65E5, 0xD7FF, 0xE000, 0xFEFF, 0xFFFD, 0x10000,
      0x1F600, 0x10FFFF};
  for (;;) {
    uint32_t cp = rng.below(3) == 0 ? pool[rng.below(sizeof(pool) / sizeof(pool[0]))]
                                    : 0x20 + rng.below(0x5F);
    if (cp == '<' || cp == '&' || cp == '"' || cp == '\'' || cp == '>' || cp == '-' || cp == '?' || cp == ']' || cp == 0x10FFFF
        || cp == 0xFEFF) {
      continue;
    }
    if (cp <= max) {
      return cp;
    }
  }
}

std::string random_text(Rng & rng, uint32_t max, size_t n) {
  std::string s;
  for (size_t i = 0; i < n; i++) {
    put_utf8(s, random_char(rng, max));
  }
  return s;
}

std::string random_document(Rng & rng, uint32_t max, const std::string & prolog) {
  std::string doc = prolog + "<r a=\"" + random_text(rng, max, rng.below(8)) + "\">";
  const uint32_t parts = 1 + rng.below(12);
  for (uint32_t i = 0; i < parts; i++) {
    switch (rng.below(5)) {
      case 0:
        doc += random_text(rng, max, rng.below(20));
        break;
      case 1:
        doc += "<e" + std::to_string(i) + " k=\"" + random_text(rng, max, rng.below(6)) + "\">"
            + random_text(rng, max, rng.below(10)) + "</e" + std::to_string(i) + ">";
        break;
      case 2:
        doc += "<!--" + random_text(rng, max, rng.below(10)) + "-->";
        break;
      case 3:
        doc += "<![CDATA[" + random_text(rng, max, rng.below(10)) + "]]>";
        break;
      default:
        doc += "&amp;&#" + std::to_string(0x41 + rng.below(20)) + ";<?pi "
            + random_text(rng, max, 1 + rng.below(5)) + "?>";
        break;
    }
  }
  return doc + "</r>\n";
}

TEST_F(XmlEncodings, EveryEncodingParsesToTheSameTreeAsUtf8) {
  Rng rng{12345};
  for (int round = 0; round < 300; round++) {
    // Wide encodings, any character, with and without a mark.
    const std::string plain = random_document(rng, 0x10FFFF, "");
    Result expected = parse(plain);
    ASSERT_EQ(expected.status, GTEXT_XML_OK) << plain;
    const std::string want = dump(expected.doc);
    for (bool big : {false, true}) {
      Result r = parse(utf16(plain, big, true));
      ASSERT_EQ(r.status, GTEXT_XML_OK) << round << " utf16 bom: " << r.err.message;
      EXPECT_EQ(dump(r.doc), want) << round;
      r = parse(utf32(plain, big, true));
      ASSERT_EQ(r.status, GTEXT_XML_OK) << round << " utf32 bom: " << r.err.message;
      EXPECT_EQ(dump(r.doc), want) << round;
      const std::string with16 = random_document(rng, 0x10FFFF,
          "<?xml version=\"1.0\" encoding=\"UTF-16\"?>");
      const std::string with32 = "<?xml version=\"1.0\" encoding=\"UTF-32\"?>"
          + with16.substr(with16.find("?>") + 2);
      const std::string base = with16.substr(with16.find("?>") + 2);
      Result b = parse(base);
      ASSERT_EQ(b.status, GTEXT_XML_OK);
      r = parse(utf16(with16, big, false));
      ASSERT_EQ(r.status, GTEXT_XML_OK) << round << " utf16: " << r.err.message;
      EXPECT_EQ(dump(r.doc), dump(b.doc)) << round;
      r = parse(utf32(with32, big, false));
      ASSERT_EQ(r.status, GTEXT_XML_OK) << round << " utf32: " << r.err.message;
      EXPECT_EQ(dump(r.doc), dump(b.doc)) << round;
    }
    // Single-byte encodings: characters up to U+FF and up to U+7F.
    for (int kind = 0; kind < 2; kind++) {
      const uint32_t max = kind == 0 ? 0xFF : 0x7F;
      const std::string label = kind == 0 ? "ISO-8859-1" : "US-ASCII";
      const std::string utf8_form = random_document(rng, max,
          "<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
      const std::string declared = "<?xml version=\"1.0\" encoding=\"" + label + "\"?>"
          + utf8_form.substr(utf8_form.find("?>") + 2);
      Result b = parse(utf8_form);
      ASSERT_EQ(b.status, GTEXT_XML_OK) << utf8_form;
      Result r = parse(single_byte(declared));
      ASSERT_EQ(r.status, GTEXT_XML_OK) << round << " " << label << ": " << r.err.message;
      EXPECT_EQ(dump(r.doc), dump(b.doc)) << round << " " << label;
      GTEXT_Markup_Encoding_Source source;
      EXPECT_EQ(encoding_of(r.doc, &source), label);
      EXPECT_EQ(source, GTEXT_MARKUP_ENCODING_DECLARATION);
    }
  }
}

// ---- allocation failure --------------------------------------------------

TEST_F(XmlEncodings, AllocationFailureWhileConverting) {
  const std::string base = "<?xml version=\"1.0\" encoding=\"ENC\"?><r a=\"\xC3\xA9\"><b>caf\xC3\xA9 "
                           "\xE6\x97\xA5</b><!--c--></r>";
  auto with_label = [&](const char * label) {
    std::string s = base;
    s.replace(s.find("ENC"), 3, label);
    return s;
  };
  const std::string inputs[] = {
      utf16(with_label("UTF-16"), false, true),
      utf16(with_label("UTF-16"), true, false),
      utf32(with_label("UTF-32"), false, true),
      std::string("<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><r a=\"\xE9\">caf\xE9</r>"),
  };
  for (const std::string & text : inputs) {
    FaultAllocator fa;
    GTEXT_XML_Parse_Options o = gtext_xml_parse_options_default();
    o.allocator = &fa.alloc;
    size_t failures = 0;
    bool finished = false;
    for (size_t n = 1; n < 100000 && !finished; n++) {
      fa.arm(n);
      Doc * doc = nullptr;
      GTEXT_XML_Error err;
      const GTEXT_XML_Status status = gtext_xml_parse(text.data(), text.size(), &o, &doc, &err);
      if (!fa.failed) {
        fa.disarm();
        ASSERT_EQ(status, GTEXT_XML_OK);
        gtext_markup_document_free(doc);
        finished = true;
      }
      else if (status == GTEXT_XML_OK) {
        gtext_markup_document_free(doc);  // The tree's best-effort name table.
      }
      else {
        failures++;
        EXPECT_EQ(status, GTEXT_XML_E_OOM) << "n=" << n;
        EXPECT_EQ(doc, nullptr);
      }
      EXPECT_TRUE(fa.live.empty()) << "n=" << n << ": " << fa.live.size() << " blocks leaked";
      EXPECT_EQ(fa.bad_frees, 0u);
    }
    EXPECT_TRUE(finished);
    EXPECT_GT(failures, 10u);
  }
}

}  // namespace

TEST_F(XmlEncodings, ReviewPins) {
  GTEXT_Markup_Encoding_Source source;
  // A mark-less UTF-16 document with no declaration, starting with a PI.
  for (bool big : {false, true}) {
    Result r = parse(utf16("<?p?><a/>", big, false));
    ASSERT_EQ(r.status, GTEXT_XML_OK) << big;
    EXPECT_EQ(encoding_of(r.doc, &source), "UTF-16");
    EXPECT_EQ(source, GTEXT_MARKUP_ENCODING_DEFAULT);
    // Starting with "<a" it is not detected (documented).
    EXPECT_NE(parse(utf16("<a/>", big, false)).status, GTEXT_XML_OK) << big;
    // A bare mark and nothing else: an empty document, not a crash.
    EXPECT_EQ(parse(utf16("", big, true)).status, GTEXT_XML_E_UNEXPECTED_EOF) << big;
    // The byte-order-qualified labels are declined on purpose.
    EXPECT_EQ(parse(utf16("<?xml version=\"1.0\" encoding=\"UTF-16LE\"?><a/>", big, false)).status,
        GTEXT_XML_E_UNSUPPORTED_ENCODING) << big;
  }
  // The raw label scan agrees with the real parser on legal declaration shapes.
  const char * shapes[] = {
      "<?xml version='1.0' encoding='ISO-8859-1'?><a>\xE9</a>",
      "<?xml  version = \"1.0\"\n\tencoding\n=\n\"iso-8859-1\" standalone=\"yes\" ?><a>\xE9</a>",
      "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><!-- \xE9 --><a/>"};
  for (const char * text : shapes) {
    EXPECT_EQ(parse(text).status, GTEXT_XML_OK) << text;
  }
  // The new statuses have messages.
  Result bad = parse("<?xml version=\"1.0\" encoding=\"US-ASCII\"?><a>\xE9</a>");
  EXPECT_EQ(bad.status, GTEXT_XML_E_INVALID_SEQUENCE);
  EXPECT_NE(std::string(gtext_xml_status_string(GTEXT_XML_E_INVALID_SEQUENCE)), "unknown status");
  EXPECT_NE(std::string(gtext_xml_status_string(GTEXT_XML_E_ENCODING_MISMATCH)), "unknown status");
}
