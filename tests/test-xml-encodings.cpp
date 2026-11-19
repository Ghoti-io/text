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
 * bytes), declared ISO-8859-1 and US-ASCII, every other encoding of the
 * encoding module (Windows-1251, Shift_JIS, EUC-KR, GBK, ...) by its declared
 * label, the declaration's label checked against what the document is in,
 * malformed units and byte sequences, positions, and allocation failure while
 * converting.
 *
 * The strongest check is the round trip: a generated document is encoded in each
 * supported encoding, parsed, and its tree compared with the tree of the same
 * document parsed as UTF-8.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/encoding.h>
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

// The bytes of `utf8` in `encoding`, by the encoding module's encoder (which
// stops at a character the encoding does not have, so a test cannot pass by
// accident on a replaced one).
std::string encode_as(GTEXT_Encoding encoding, const std::string & utf8) {
  char * out = nullptr;
  size_t n = 0;
  GTEXT_Encoder_Options o = gtext_encoder_options_default();
  GTEXT_Encoder_Result r;
  const GTEXT_Encoding_Status st = gtext_encoding_encode_buffer(encoding, &o,
      utf8.data(), utf8.size(), &out, &n, &r);
  EXPECT_EQ(st, GTEXT_ENCODING_OK) << gtext_encoding_name(encoding) << " " << utf8;
  std::string bytes = st == GTEXT_ENCODING_OK ? std::string(out, n) : std::string();
  gtext_encoding_buffer_free(out);
  return bytes;
}

// The text of the root element (the whole of its text, descendants included).
std::string root_text(Doc * doc) {
  char * text = nullptr;
  size_t n = 0;
  EXPECT_EQ(gtext_markup_node_text_content(gtext_markup_document_element(doc), nullptr, &text, &n),
      GTEXT_MARKUP_OK);
  std::string out = text ? std::string(text, n) : std::string();
  gtext_markup_string_free(nullptr, text);
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
  // A bare UTF-16 or UTF-32 label matches either order. A label of an encoding
  // the library reads that is not the document's is a mismatch, and one that
  // names no encoding it reads is unsupported.
  EXPECT_EQ(parse(utf16("<?xml version=\"1.0\" encoding=\"UTF-16\"?><a/>", true, false)).status,
      GTEXT_XML_OK);
  EXPECT_EQ(parse(utf16("<?xml version=\"1.0\" encoding=\"windows-1252\"?><a/>", false, true)).status,
      GTEXT_XML_E_ENCODING_MISMATCH);
  EXPECT_EQ(parse(utf16("<?xml version=\"1.0\" encoding=\"x-no-such-encoding\"?><a/>", false, true)).status,
      GTEXT_XML_E_UNSUPPORTED_ENCODING);
  EXPECT_EQ(parse(utf32("<?xml version=\"1.0\" encoding=\"UTF-16\"?><a/>", false, true)).status,
      GTEXT_XML_E_ENCODING_MISMATCH);
}

TEST_F(XmlEncodings, UnknownAndEbcdicAreUnsupported) {
  // A label that names nothing the encoding module decodes, one that it makes
  // `replacement` of (which reads none of the text), and UTF-32's relatives.
  for (const char * label : {"x-no-such-encoding", "UTF-7", "UCS-4", "hz-gb-2312",
           "iso-2022-kr", "csiso2022kr", "EBCDIC-US"}) {
    Result r = parse(std::string("<?xml version=\"1.0\" encoding=\"") + label + "\"?><a/>");
    EXPECT_EQ(r.status, GTEXT_XML_E_UNSUPPORTED_ENCODING) << label;
    EXPECT_EQ(r.err.byte_offset, 30u) << label;  // at the label
    EXPECT_EQ(r.err.column, 31u) << label;
  }
  Result r = parse(std::string("\x4C\x6F\xA7\x94", 4));
  EXPECT_EQ(r.status, GTEXT_XML_E_UNSUPPORTED_ENCODING);
  EXPECT_EQ(r.err.byte_offset, 0u);
}

// ---- the encodings of the encoding module ---------------------------------

struct Sample {
  const char * label;
  GTEXT_Encoding encoding;
  const char * text;  // UTF-8
};

// Text that each of these encodes, in more than one byte where the encoding is
// a multi-byte one (a Cyrillic word is one byte a letter in the single-byte ones).
const Sample samples[] = {
    {"Windows-1251", GTEXT_ENCODING_WINDOWS_1251,
        "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xD0\xBC\xD0\xB8\xD1\x80"},  // Привет мир
    {"KOI8-R", GTEXT_ENCODING_KOI8_R,
        "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"},
    {"windows-1252", GTEXT_ENCODING_WINDOWS_1252, "caf\xC3\xA9 \xE2\x82\xAC"},  // café €
    {"ISO-8859-7", GTEXT_ENCODING_ISO_8859_7, "\xCE\x95\xCE\xBB\xCE\xBB\xCE\xAC\xCE\xB4\xCE\xB1"},
    {"Shift_JIS", GTEXT_ENCODING_SHIFT_JIS, "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E \xE3\x81\x82"},
    {"EUC-JP", GTEXT_ENCODING_EUC_JP, "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E \xEF\xBD\xB1"},
    {"ISO-2022-JP", GTEXT_ENCODING_ISO_2022_JP, "abc \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E def"},
    {"EUC-KR", GTEXT_ENCODING_EUC_KR, "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4"},
    {"GBK", GTEXT_ENCODING_GBK, "\xE4\xB8\xAD\xE6\x96\x87"},
    {"gb18030", GTEXT_ENCODING_GB18030, "\xE4\xB8\xAD\xE6\x96\x87 \xF0\x9F\x98\x80"},
    {"Big5", GTEXT_ENCODING_BIG5, "\xE4\xB8\xAD\xE6\x96\x87"},
    {"x-user-defined", GTEXT_ENCODING_X_USER_DEFINED, "abc"},
};

std::string declaration(const std::string & label) {
  return "<?xml version=\"1.0\" encoding=\"" + label + "\"?>\n";
}

TEST_F(XmlEncodings, EveryEncodingOfTheEncodingModuleIsRead) {
  for (const Sample & sample : samples) {
    const std::string utf8 = declaration(sample.label) + "<a>" + sample.text + "</a>";
    Result r = parse(encode_as(sample.encoding, utf8));
    ASSERT_EQ(r.status, GTEXT_XML_OK) << sample.label << ": " << r.err.message;
    EXPECT_EQ(root_text(r.doc), sample.text) << sample.label;
    // The same tree as the UTF-8 form, and the label as the document wrote it.
    Result same = parse(declaration("UTF-8") + "<a>" + sample.text + "</a>");
    ASSERT_EQ(same.status, GTEXT_XML_OK) << sample.label;
    EXPECT_EQ(dump(r.doc), dump(same.doc)) << sample.label;
    GTEXT_Markup_Encoding_Source source;
    EXPECT_EQ(encoding_of(r.doc, &source), sample.label);
    EXPECT_EQ(source, GTEXT_MARKUP_ENCODING_DECLARATION) << sample.label;
  }
  // The bytes are what they say: Windows-1251 puts a letter in one byte.
  const std::string cp1251 = declaration("windows-1251") + "<a>\xCF\xF0\xE8\xE2\xE5\xF2</a>";
  Result r = parse(cp1251);
  ASSERT_EQ(r.status, GTEXT_XML_OK);
  EXPECT_EQ(root_text(r.doc), "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82");
}

TEST_F(XmlEncodings, ALabelIsTheEncodingStandardsLabelInAnyCase) {
  const std::string want = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";
  for (const char * label : {"windows-1251", "WINDOWS-1251", "Windows-1251", "cp1251",
           "x-cp1251"}) {
    Result r = parse(declaration(label) + "<a>\xCF\xF0\xE8\xE2\xE5\xF2</a>");
    ASSERT_EQ(r.status, GTEXT_XML_OK) << label << ": " << r.err.message;
    EXPECT_EQ(root_text(r.doc), want) << label;
    GTEXT_Markup_Encoding_Source source;
    EXPECT_EQ(encoding_of(r.doc, &source), label);  // as written
  }
  const std::string kana = "\x82\xA0";  // Shift_JIS: あ
  for (const char * label : {"shift_jis", "SHIFT_JIS", "sjis", "ms_kanji", "csshiftjis",
           "windows-31j", "x-sjis"}) {
    Result r = parse(declaration(label) + "<a>" + kana + "</a>");
    ASSERT_EQ(r.status, GTEXT_XML_OK) << label << ": " << r.err.message;
    EXPECT_EQ(root_text(r.doc), "\xE3\x81\x82") << label;
  }
  // The shape of the declaration: single quotes, white space, a standalone.
  Result r = parse("<?xml  version = '1.0'\n\tencoding\n=\n'windows-1251' standalone=\"yes\" ?><a>\xCF</a>");
  ASSERT_EQ(r.status, GTEXT_XML_OK) << r.err.message;
  EXPECT_EQ(root_text(r.doc), "\xD0\x9F");
}

TEST_F(XmlEncodings, TheIanaNamesOfLatin1AndAsciiKeepTheirReading) {
  // XML 1.0 4.3.3 sends an IANA name to the encoding IANA registers for it. The
  // Encoding standard reads these as windows-1252, which differs in 0x80-0x9F.
  const std::string c1 = "<a>\x80\xE9</a>";
  for (const char * label : {"ISO-8859-1", "iso-8859-1", "latin1", "LATIN1", "l1",
           "ISO_8859-1", "iso-ir-100", "IBM819", "CP819", "csISOLatin1"}) {
    Result r = parse(declaration(label) + c1);
    ASSERT_EQ(r.status, GTEXT_XML_OK) << label << ": " << r.err.message;
    EXPECT_EQ(root_text(r.doc), "\xC2\x80\xC3\xA9") << label;
  }
  // windows-1252, which is not the same, is what its name says.
  Result r = parse(declaration("windows-1252") + c1);
  ASSERT_EQ(r.status, GTEXT_XML_OK);
  EXPECT_EQ(root_text(r.doc), "\xE2\x82\xAC\xC3\xA9");
  // US-ASCII under any of its IANA names has no byte above 0x7F...
  for (const char * label : {"US-ASCII", "us-ascii", "ANSI_X3.4-1968", "ANSI_X3.4-1986",
           "iso-ir-6", "ISO646-US", "us", "IBM367", "cp367", "csASCII"}) {
    EXPECT_EQ(parse(declaration(label) + "<a>abc</a>").status, GTEXT_XML_OK) << label;
    Result bad = parse(declaration(label) + "<a>\xE9</a>");
    EXPECT_EQ(bad.status, GTEXT_XML_E_INVALID_SEQUENCE) << label;
  }
  // ...and "ascii", which is the Encoding standard's and not IANA's, is what
  // that standard says (windows-1252).
  r = parse(declaration("ascii") + "<a>\xE9</a>");
  ASSERT_EQ(r.status, GTEXT_XML_OK);
  EXPECT_EQ(root_text(r.doc), "\xC3\xA9");
}

TEST_F(XmlEncodings, AByteSequenceThatIsNotLegalInTheEncodingIsFatalAtItsFirstByte) {
  const std::string head = declaration("Shift_JIS");
  const size_t prolog = head.size();  // the declaration and its line end
  // A lead byte, then '<': the lead byte is the ill-formed sequence.
  Result r = parse(head + "<a>\x82\xA0\x82<b/></a>");
  EXPECT_EQ(r.status, GTEXT_XML_E_INVALID_SEQUENCE);
  EXPECT_TRUE(r.err.has_raw_byte_offset);
  EXPECT_EQ(r.err.raw_byte_offset, prolog + 3 + 2u);
  EXPECT_EQ(r.err.byte_offset, prolog + 3 + 3u);  // in UTF-8: the kana is three bytes
  EXPECT_EQ(r.err.line, 2u);
  EXPECT_EQ(r.err.column, 5u);  // "<a>" and one character
  // The document ends inside the character.
  r = parse(head + "<a>\x82\xA0\x82");
  EXPECT_EQ(r.status, GTEXT_XML_E_INVALID_SEQUENCE);
  EXPECT_EQ(r.err.raw_byte_offset, prolog + 3 + 2u);
  // A byte that no Shift_JIS character starts with.
  r = parse(head + "<a>\xFF</a>");
  EXPECT_EQ(r.status, GTEXT_XML_E_INVALID_SEQUENCE);
  EXPECT_EQ(r.err.raw_byte_offset, prolog + 3u);
  EXPECT_EQ(r.err.column, 4u);
  // Single-byte encodings with bytes that map to nothing, and EUC-KR's 0xFF.
  r = parse(declaration("ISO-8859-3") + "<a>\xA5</a>");
  EXPECT_EQ(r.status, GTEXT_XML_E_INVALID_SEQUENCE);
  r = parse(declaration("EUC-KR") + "<a>\xFF</a>");
  EXPECT_EQ(r.status, GTEXT_XML_E_INVALID_SEQUENCE);
  r = parse(declaration("GBK") + "<a>\x81</a>");
  EXPECT_EQ(r.status, GTEXT_XML_E_INVALID_SEQUENCE);
  // ISO-2022-JP: a byte above 0x7F, and an escape sequence that is not one.
  r = parse(declaration("ISO-2022-JP") + "<a>\xE9</a>");
  EXPECT_EQ(r.status, GTEXT_XML_E_INVALID_SEQUENCE);
  // An escape sequence that is not one of ISO-2022-JP's, and one cut by the end.
  r = parse(declaration("ISO-2022-JP") + "<a>\x1b$X</a>");
  EXPECT_EQ(r.status, GTEXT_XML_E_INVALID_SEQUENCE);
  r = parse(declaration("ISO-2022-JP") + "<a>\x1b$");
  EXPECT_EQ(r.status, GTEXT_XML_E_INVALID_SEQUENCE);
  // Each multi-byte encoding with a character cut by the end of the document,
  // at the first byte of it.
  const char * cut[][2] = {{"EUC-JP", "\x8f\xb0"}, {"gb18030", "\x81\x30\x81"}, {"Big5", "\xa4"},
      {"EUC-KR", "\xb0"}, {"GBK", "\xb0"}};
  for (const auto & c : cut) {
    const std::string doc = declaration(c[0]) + "<a>" + c[1];
    r = parse(doc);
    EXPECT_EQ(r.status, GTEXT_XML_E_INVALID_SEQUENCE) << c[0];
    EXPECT_EQ(r.err.raw_byte_offset, doc.size() - std::strlen(c[1])) << c[0];
  }
  // A byte that decodes to a character XML does not have is the parser's, as for
  // UTF-8: NUL is the same in every encoding.
  r = parse(declaration("windows-1251") + std::string("<a>\0</a>", 8));
  EXPECT_EQ(r.status, GTEXT_XML_E_CHARACTER);
}

TEST_F(XmlEncodings, ALabelThatIsNotTheDocumentsEncodingIsAMismatch) {
  // The bytes are UTF-16 or UTF-8 with a mark: whatever the label says, the
  // first bytes decided, and a label of another encoding is not a reading.
  for (const char * label : {"windows-1251", "Shift_JIS", "EUC-KR", "GBK", "ISO-2022-JP",
           "x-user-defined"}) {
    const std::string text = declaration(label) + "<a/>";
    Result r = parse(utf16(text, false, true));
    EXPECT_EQ(r.status, GTEXT_XML_E_ENCODING_MISMATCH) << label;
    EXPECT_EQ(r.err.byte_offset, 30u) << label;
    r = parse(utf16(text, true, false));
    EXPECT_EQ(r.status, GTEXT_XML_E_ENCODING_MISMATCH) << label;
    r = parse(utf32(text, false, true));
    EXPECT_EQ(r.status, GTEXT_XML_E_ENCODING_MISMATCH) << label;
    r = parse("\xEF\xBB\xBF" + text);
    EXPECT_EQ(r.status, GTEXT_XML_E_ENCODING_MISMATCH) << label;
    EXPECT_EQ(r.err.byte_offset, 33u) << label;
    EXPECT_EQ(r.err.column, 31u) << label;
  }
  // A document that is UTF-8 with no mark and says UTF-16 in any of its names.
  for (const char * label : {"UTF-16LE", "UTF-16BE", "UCS-2", "unicode", "ISO-10646-UCS-2"}) {
    EXPECT_EQ(parse(declaration(label) + "<a/>").status, GTEXT_XML_E_ENCODING_MISMATCH) << label;
  }
  // Two encodings that are not the same: a document is in the one it names, so
  // a label that is a different name for the same encoding is the same.
  EXPECT_EQ(parse(declaration("x-cp1251") + "<a>\xCF</a>").status, GTEXT_XML_OK);
  // UTF-16's labels with an order: the document's own, or a mismatch.
  for (bool big : {false, true}) {
    const std::string own = big ? "UTF-16BE" : "UTF-16LE";
    const std::string other = big ? "UTF-16LE" : "UTF-16BE";
    EXPECT_EQ(parse(utf16(declaration(own) + "<a/>", big, true)).status, GTEXT_XML_OK) << own;
    EXPECT_EQ(parse(utf16(declaration(own) + "<a/>", big, false)).status, GTEXT_XML_OK) << own;
    EXPECT_EQ(parse(utf16(declaration(other) + "<a/>", big, true)).status,
        GTEXT_XML_E_ENCODING_MISMATCH) << other;
    for (const char * label : {"UTF-16", "UCS-2", "unicode", "ISO-10646-UCS-2"}) {
      EXPECT_EQ(parse(utf16(declaration(label) + "<a/>", big, true)).status, GTEXT_XML_OK)
          << label;
    }
  }
  // "utf8" is UTF-8 by the Encoding standard's label.
  EXPECT_EQ(parse(declaration("utf8") + "<a/>").status, GTEXT_XML_OK);
  EXPECT_EQ(parse("\xEF\xBB\xBF" + declaration("utf8") + "<a/>").status, GTEXT_XML_OK);
}

TEST_F(XmlEncodings, PositionsAfterAMultiByteCharacterCountCharactersAndUtf8Bytes) {
  // Two characters before <c/> on its line; each is two or three bytes of UTF-8
  // and one or two bytes of the original.
  const char * labels[] = {"Shift_JIS", "EUC-KR", "GBK", "Big5", "EUC-JP", "ISO-2022-JP",
      "windows-1251"};
  const char * words[] = {"\xE6\x97\xA5\xE6\x9C\xAC", "\xED\x95\x9C\xEA\xB5\xAD",
      "\xE4\xB8\xAD\xE6\x96\x87", "\xE4\xB8\xAD\xE6\x96\x87", "\xE6\x97\xA5\xE6\x9C\xAC",
      "\xE6\x97\xA5\xE6\x9C\xAC", "\xD0\x9F\xD1\x80"};
  const GTEXT_Encoding encodings[] = {GTEXT_ENCODING_SHIFT_JIS, GTEXT_ENCODING_EUC_KR,
      GTEXT_ENCODING_GBK, GTEXT_ENCODING_BIG5, GTEXT_ENCODING_EUC_JP,
      GTEXT_ENCODING_ISO_2022_JP, GTEXT_ENCODING_WINDOWS_1251};
  for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
    const std::string utf8 = declaration(labels[i]) + "<a>\n<b>" + words[i] + "<c/></b></a>";
    Result r = parse(encode_as(encodings[i], utf8));
    ASSERT_EQ(r.status, GTEXT_XML_OK) << labels[i] << ": " << r.err.message;
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
    EXPECT_EQ(where.line, 3u) << labels[i];
    EXPECT_EQ(where.column, 6u) << labels[i];  // "<b>" and two characters
    EXPECT_EQ(where.offset, utf8.find("<c/>")) << labels[i];  // bytes of the UTF-8 form
    // An error after them is at the same place.
    const std::string broken = declaration(labels[i]) + "<a>\n<b>" + words[i] + "</a>";
    r = parse(encode_as(encodings[i], broken));
    EXPECT_EQ(r.status, GTEXT_XML_E_MISMATCHED_TAG) << labels[i];
    EXPECT_EQ(r.err.line, 3u) << labels[i];
    EXPECT_EQ(r.err.column, 6u) << labels[i];
    EXPECT_EQ(r.err.byte_offset, broken.find("</a>")) << labels[i];
  }
}

TEST_F(XmlEncodings, TheInputLimitCountsTheOriginalBytesOfALegacyDocument) {
  GTEXT_XML_Parse_Options o = gtext_xml_parse_options_default();
  const std::string doc = declaration("windows-1251") + "<a>\xCF\xF0\xE8</a>";
  o.max_input_bytes = doc.size();
  EXPECT_EQ(parse(doc, &o).status, GTEXT_XML_OK);
  o.max_input_bytes = doc.size() - 1;
  EXPECT_EQ(parse(doc, &o).status, GTEXT_XML_E_LIMIT_INPUT);
}

TEST_F(XmlEncodings, ADeclarationThatNamesALegacyEncodingIsReadInAnAsciiCompatibleReading) {
  // Appendix F: the first bytes "<?xm" in ASCII are every encoding that keeps
  // ASCII where it is, and the declaration says which. A label further into the
  // declaration than the encoding's own name still counts.
  Result r = parse("<?xml version=\"1.0\" encoding=\"windows-1251\" standalone=\"no\"?><a>\xCF</a>");
  ASSERT_EQ(r.status, GTEXT_XML_OK);
  EXPECT_EQ(root_text(r.doc), "\xD0\x9F");
  // Only a declaration at the start of the document is one: after a comment it
  // is a processing instruction with a reserved name, whatever it says.
  r = parse("<!-- x --><?xml version=\"1.0\" encoding=\"windows-1251\"?><a/>");
  EXPECT_EQ(r.status, GTEXT_XML_E_SYNTAX);
  // A label that is not an EncName (white space around it) is not a label: the
  // document is read as UTF-8, as the encoding module's trimming would not be.
  r = parse("<?xml version=\"1.0\" encoding=\" shift_jis\"?><a>\x82</a>");
  EXPECT_EQ(r.status, GTEXT_XML_E_UTF8);
  r = parse("<?xml version=\"1.0\" encoding=\" shift_jis\"?><a/>");
  EXPECT_EQ(r.status, GTEXT_XML_E_SYNTAX);
}

TEST_F(XmlEncodings, AnXml11DocumentInALegacyEncodingGetsItsLineEnds) {
  // ISO-8859-2 byte 0x85 is U+0085, which XML 1.1 makes a line end (and XML 1.0
  // does not): the conversion keeps the line and the column of the original.
  Result r = parse("<?xml version=\"1.1\" encoding=\"ISO-8859-2\"?><a>x\x85y<b/></a>");
  ASSERT_EQ(r.status, GTEXT_XML_OK) << r.err.message;
  EXPECT_EQ(root_text(r.doc), "x\ny");
  GTEXT_Markup_Node * b = gtext_markup_node_first_child(gtext_markup_document_element(r.doc));
  while (b && gtext_markup_node_kind(b) != GTEXT_MARKUP_KIND_ELEMENT) {
    b = gtext_markup_node_next_sibling(b);
  }
  ASSERT_NE(b, nullptr);
  GTEXT_Markup_Location where;
  ASSERT_TRUE(gtext_markup_node_location(b, &where));
  EXPECT_EQ(where.line, 2u);
  EXPECT_EQ(where.column, 2u);  // after "y"
  r = parse("<?xml version=\"1.0\" encoding=\"ISO-8859-2\"?><a>x\x85y</a>");
  ASSERT_EQ(r.status, GTEXT_XML_OK);
  EXPECT_EQ(root_text(r.doc), "x\xC2\x85y");
}

// What the converter reads of the XML declaration to count line ends is the version, and only the version of a declaration at the start of the
// document, as the declaration is written (the quotes, the white space and the line ends in it) once it is complete. These pin where the profile
// is taken, so that how it is found can change and nothing else does.
TEST_F(XmlEncodings, TheProfileOfALegacyDocumentIsTheVersionOfItsDeclarationAndNothingAfterIt) {
  // 0x85 is U+0085 in ISO-8859-2: a line end in 1.1, a character in 1.0. Each document is read as the declaration says and the element b is
  // where the line ends put it.
  struct Case {
    const char * name;
    std::string text;
    unsigned line, column;  // of <b/>
  };
  const Case cases[] = {
      {"double quotes", "<?xml version=\"1.1\" encoding=\"ISO-8859-2\"?><a>x\x85y<b/></a>", 2, 2},
      {"single quotes", "<?xml version='1.1' encoding='ISO-8859-2'?><a>x\x85y<b/></a>", 2, 2},
      {"white space around the version", "<?xml\n\t version\r\n=\t'1.1' encoding='ISO-8859-2'?><a>x\x85y<b/></a>", 4, 2},
      {"a 1.0 declaration", "<?xml version=\"1.0\" encoding=\"ISO-8859-2\"?><a>x\x85y<b/></a>", 1, 50},
      {"1.1 after the declaration is nothing", "<?xml version=\"1.0\" encoding=\"ISO-8859-2\"?><a>x\x85y<b/><c d=\"1.1\"/></a>", 1, 50},
  };
  for (const Case & c : cases) {
    Result r = parse(c.text);
    if (r.status != GTEXT_XML_OK) {
      ADD_FAILURE() << c.name << ": " << r.err.message;
      continue;
    }
    GTEXT_Markup_Node * b = gtext_markup_node_first_child(gtext_markup_document_element(r.doc));
    while (b && gtext_markup_node_kind(b) != GTEXT_MARKUP_KIND_ELEMENT) {
      b = gtext_markup_node_next_sibling(b);
    }
    ASSERT_NE(b, nullptr) << c.name;
    GTEXT_Markup_Location where;
    ASSERT_TRUE(gtext_markup_node_location(b, &where)) << c.name;
    EXPECT_EQ(where.line, c.line) << c.name;
    EXPECT_EQ(where.column, c.column) << c.name;
  }
  // The error of a bad sequence is placed by the profile as well: line ends before it count in 1.1 and do not in 1.0 (ISO-8859-3 has no 0xA5).
  Result r = parse("<?xml version=\"1.1\" encoding=\"ISO-8859-3\"?>\x85\x85<a>\xA5</a>");
  EXPECT_EQ(r.status, GTEXT_XML_E_INVALID_SEQUENCE);
  EXPECT_EQ(r.err.line, 3u);
  EXPECT_EQ(r.err.column, 4u);
  r = parse("<?xml version=\"1.0\" encoding=\"ISO-8859-3\"?>\x85\x85<a>\xA5</a>");
  EXPECT_EQ(r.status, GTEXT_XML_E_INVALID_SEQUENCE);
  EXPECT_EQ(r.err.line, 1u);
  EXPECT_EQ(r.err.column, 49u);
  // A document that is not declared at its start is not read for a version at all, wherever it says one.
  r = parse("<?xml version=\"1.0\" encoding=\"ISO-8859-3\"?><a>x\x85y<?xml version=\"1.1\"?></a>");
  EXPECT_EQ(r.status, GTEXT_XML_E_SYNTAX);
  EXPECT_EQ(r.err.line, 1u) << "the version after the declaration is not the profile";
}

TEST_F(XmlEncodings, ALegacyDocumentWhoseFirstMarkupNeverEndsIsConvertedInLinearTime) {
  // The profile is asked about after every converted character and the answer is final once the text holds a `>`; a document that has none for a
  // megabyte (the declaration's last attribute value is never closed) is the one in which the question is asked every time, and in which a search of
  // the whole text for the `>` each time would take a minute.
  std::string doc = "<?xml version=\"1.0\" encoding=\"windows-1251\" a=\"";
  doc.append(1 << 20, '\xCF');
  const auto start = std::chrono::steady_clock::now();
  Result r = parse(doc);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  EXPECT_NE(r.status, GTEXT_XML_OK);
  EXPECT_LT(ms, 10000) << "the conversion of one megabyte must not grow with the text it has converted";
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

// When set, the characters other than ASCII come from here (a legacy encoding's
// own characters), and `max` does not apply to them.
const std::vector<uint32_t> * active_pool = nullptr;

// A random character that is legal in text and attribute values, at most `max`.
uint32_t random_char(Rng & rng, uint32_t max) {
  if (active_pool && rng.below(3) == 0) {
    return (*active_pool)[rng.below((uint32_t)active_pool->size())];
  }
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

// Whether `encoding` has the character and reads it back as itself (the encoder,
// in its fatal mode, says the first; ISO-2022-JP writes a half-width katakana as
// the full-width one, which is the second).
bool encodable(GTEXT_Encoding encoding, uint32_t cp) {
  std::string utf8;
  put_utf8(utf8, cp);
  char * out = nullptr;
  size_t n = 0;
  GTEXT_Encoder_Options o = gtext_encoder_options_default();
  GTEXT_Encoder_Result r;
  if (gtext_encoding_encode_buffer(encoding, &o, utf8.data(), utf8.size(), &out, &n, &r)
      != GTEXT_ENCODING_OK) {
    return false;
  }
  char * back = nullptr;
  size_t back_n = 0;
  GTEXT_Decoder_Options d = gtext_decoder_options_default();
  d.error_mode = GTEXT_DECODER_FATAL;
  GTEXT_Decoder_Result dr;
  const bool ok = gtext_encoding_decode_buffer(encoding, &d, out, n, &back, &back_n, &dr)
          == GTEXT_ENCODING_OK
      && std::string(back, back_n) == utf8;
  gtext_encoding_buffer_free(out);
  gtext_encoding_buffer_free(back);
  return ok;
}

TEST_F(XmlEncodings, EveryLegacyEncodingParsesToTheSameTreeAsUtf8) {
  // Characters of several scripts; each encoding keeps those it has.
  static const uint32_t scripts[] = {0xE9, 0xA0, 0x20AC, 0x3A9, 0x416, 0x44F, 0x5D0, 0xE01,
      0x65E5, 0x672C, 0x3042, 0x30A2, 0xFF71, 0xD55C, 0xAD6D, 0x4E2D, 0x6587, 0x2026, 0x1F600};
  Rng rng{777};
  for (const Sample & sample : samples) {
    if (sample.encoding == GTEXT_ENCODING_X_USER_DEFINED) {
      continue;
    }
    std::vector<uint32_t> pool;
    for (uint32_t cp : scripts) {
      if (encodable(sample.encoding, cp)) {
        pool.push_back(cp);
      }
    }
    ASSERT_FALSE(pool.empty()) << sample.label;
    active_pool = &pool;
    for (int round = 0; round < 60; round++) {
      const std::string body = random_document(rng, 0x7F, "");
      const std::string utf8 = declaration(sample.label) + body;
      Result b = parse(body);
      ASSERT_EQ(b.status, GTEXT_XML_OK) << body;
      Result r = parse(encode_as(sample.encoding, utf8));
      ASSERT_EQ(r.status, GTEXT_XML_OK) << sample.label << " " << round << ": " << r.err.message;
      EXPECT_EQ(dump(r.doc), dump(b.doc)) << sample.label << " " << round;
    }
    active_pool = nullptr;
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
  std::vector<std::string> inputs = {
      utf16(with_label("UTF-16"), false, true),
      utf16(with_label("UTF-16"), true, false),
      utf32(with_label("UTF-32"), false, true),
      std::string("<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><r a=\"\xE9\">caf\xE9</r>"),
  };
  // The encodings of the encoding module, each in text it has.
  for (const Sample & sample : samples) {
    const std::string t = sample.text;
    inputs.push_back(encode_as(sample.encoding, declaration(sample.label) + "<r a=\"" + t
        + "\"><b>" + t + " " + t + "</b><!--c--></r>"));
  }
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
    // The byte-order-qualified label is the document's own order, or a mismatch.
    EXPECT_EQ(parse(utf16("<?xml version=\"1.0\" encoding=\"UTF-16LE\"?><a/>", big, false)).status,
        big ? GTEXT_XML_E_ENCODING_MISMATCH : GTEXT_XML_OK) << big;
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
