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
 * The HTML parser's encodings (phase 3j): the standard's encoding sniffing algorithm, the decoding of every encoding, positions in
 * the original bytes, chunking, and "change the encoding".
 *
 *  - Sniffing: the byte order marks, a transport label, the `<meta>` prescan (each attribute shape, the 1023/1024/1025-byte
 *    boundary, comments before the meta, the XML declaration and the UTF-16 one), the defaults, `ignore_meta`.
 *  - **Families.** The same documents in UTF-8, UTF-16LE/BE with a mark, windows-1252, ISO-8859-2, Shift_JIS, EUC-JP, ISO-2022-JP, GBK,
 *    Big5 and EUC-KR (made with this library's encoders): whole, at every single cut, one byte at a time, and by a restart; the tree, the
 *    errors, the locations and the decision are the same in each way, and equal to the oracle: the document decoded to UTF-8 by the
 *    Encoding module and parsed by the tokenizer's own UTF-8 reading, its offsets taken back to the original bytes by the decoder's map.
 *  - The route a UTF-8 document takes (the tokenizer reads it itself) against the route every other encoding takes (the decoder and a
 *    source map), forced for UTF-8 too.
 *  - Change the encoding: a late `<meta>` restarts the parse and the result is that of a parse that began in the declared encoding.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/text/encoding.h>
#include <ghoti.io/text/html.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "html_api_test_support.h"

extern "C" GTEXT_HTML_Status html_parser_force_transcode(GTEXT_HTML_Parser * parser);

using namespace html_api_test;

namespace {

// ---- helpers --------------------------------------------------------------------------------------------

std::string encode_as(GTEXT_Encoding enc, const std::string & utf8, GTEXT_Encoder_Error_Mode mode = GTEXT_ENCODER_FATAL) {
  GTEXT_Encoder_Options o = gtext_encoder_options_default();
  o.error_mode = mode;
  char * out = nullptr;
  size_t n = 0;
  const GTEXT_Encoding_Status st = gtext_encoding_encode_buffer(enc, &o, utf8.data(), utf8.size(), &out, &n, nullptr);
  EXPECT_EQ(st, GTEXT_ENCODING_OK) << gtext_encoding_name(enc) << " " << utf8;
  std::string s;
  if (st == GTEXT_ENCODING_OK) {
    s.assign(out, n);
    gtext_encoding_buffer_free(out);
  }
  return s;
}

std::string utf16(const std::string & utf8, bool be, bool bom) {
  std::string out;
  if (bom) {
    out += be ? std::string("\xFE\xFF", 2) : std::string("\xFF\xFE", 2);
  }
  size_t i = 0;
  auto put = [&](unsigned unit) {
    if (be) {
      out += char(unit >> 8);
      out += char(unit & 0xFF);
    } else {
      out += char(unit & 0xFF);
      out += char(unit >> 8);
    }
  };
  while (i < utf8.size()) {
    const unsigned char b = (unsigned char)utf8[i];
    uint32_t cp;
    size_t n;
    if (b < 0x80) {
      cp = b;
      n = 1;
    } else if (b < 0xE0) {
      cp = b & 0x1F;
      n = 2;
    } else if (b < 0xF0) {
      cp = b & 0x0F;
      n = 3;
    } else {
      cp = b & 0x07;
      n = 4;
    }
    for (size_t k = 1; k < n; k++) {
      cp = (cp << 6) | ((unsigned char)utf8[i + k] & 0x3F);
    }
    i += n;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      put(0xD800 + (cp >> 10));
      put(0xDC00 + (cp & 0x3FF));
    } else {
      put(cp);
    }
  }
  return out;
}

GTEXT_HTML_Parse_Options options_with(const char * transport = nullptr, const char * fallback = nullptr) {
  GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
  o.transport_encoding = transport;
  o.default_encoding = fallback;
  return o;
}

const Call k_whole = {"whole", false, false, {}};

Outcome parse(const std::string & bytes, const GTEXT_HTML_Parse_Options * o = nullptr) {
  return run_call(k_whole, bytes, o, nullptr);
}

Outcome parse_cut(const std::string & bytes, const GTEXT_HTML_Parse_Options * o, Cut cut) {
  const Call c = {"parser", false, true, cut};
  return run_call(c, bytes, o, nullptr);
}

std::string sniff_name(const std::string & bytes, const GTEXT_HTML_Parse_Options * o = nullptr, GTEXT_HTML_Sniff_Result * out = nullptr) {
  GTEXT_HTML_Sniff_Result r;
  EXPECT_EQ(gtext_html_sniff_encoding(bytes.data(), bytes.size(), o, &r), GTEXT_HTML_OK);
  if (out) {
    *out = r;
  }
  return r.encoding_name ? r.encoding_name : "";
}

} // namespace

// ---- sniffing -------------------------------------------------------------------------------------------

TEST(HtmlSniff, ByteOrderMarks) {
  GTEXT_HTML_Sniff_Result r;
  EXPECT_EQ(sniff_name(std::string("\xEF\xBB\xBF<p>"), nullptr, &r), "UTF-8");
  EXPECT_EQ(r.source, GTEXT_MARKUP_ENCODING_BOM);
  EXPECT_EQ(r.confidence, GTEXT_HTML_CERTAIN);
  EXPECT_EQ(r.bom_length, 3u);
  EXPECT_EQ(sniff_name(std::string("\xFF\xFE<\0p\0", 6), nullptr, &r), "UTF-16LE");
  EXPECT_EQ(r.bom_length, 2u);
  EXPECT_EQ(sniff_name(std::string("\xFE\xFF\0<\0p", 6), nullptr, &r), "UTF-16BE");
  EXPECT_EQ(r.source, GTEXT_MARKUP_ENCODING_BOM);
  // the mark beats a transport label, a meta and a default; not a prefix of one
  GTEXT_HTML_Parse_Options o = options_with("windows-1252", "shift_jis");
  EXPECT_EQ(sniff_name(std::string("\xEF\xBB\xBF<meta charset=koi8-r>"), &o, &r), "UTF-8");
  EXPECT_EQ(r.source, GTEXT_MARKUP_ENCODING_BOM);
  EXPECT_EQ(sniff_name(std::string("\xEF\xBB<p>"), &o, &r), "windows-1252");
  EXPECT_EQ(r.bom_length, 0u);
  EXPECT_EQ(sniff_name(std::string("\xFF"), nullptr, &r), "UTF-8");
  EXPECT_EQ(r.source, GTEXT_MARKUP_ENCODING_DEFAULT);
}

TEST(HtmlSniff, TransportLabelAndDefaults) {
  GTEXT_HTML_Sniff_Result r;
  GTEXT_HTML_Parse_Options o = options_with("iso-8859-2");
  EXPECT_EQ(sniff_name("<meta charset=utf-8>", &o, &r), "ISO-8859-2");
  EXPECT_EQ(r.source, GTEXT_MARKUP_ENCODING_CALLER);
  EXPECT_EQ(r.confidence, GTEXT_HTML_CERTAIN);
  // a tentative label loses to the prescan and beats the default
  o.transport_encoding_certainty = GTEXT_HTML_TENTATIVE;
  o.default_encoding = "koi8-r";
  EXPECT_EQ(sniff_name("<meta charset=utf-8>", &o, &r), "UTF-8");
  EXPECT_EQ(r.source, GTEXT_MARKUP_ENCODING_DECLARATION);
  EXPECT_EQ(r.confidence, GTEXT_HTML_TENTATIVE);
  EXPECT_EQ(sniff_name("<p>", &o, &r), "ISO-8859-2");
  EXPECT_EQ(r.source, GTEXT_MARKUP_ENCODING_CALLER_DEFAULT);
  // unknown labels are ignored and flagged
  o = options_with("no-such-thing", "nor-this");
  EXPECT_EQ(sniff_name("<p>", &o, &r), "UTF-8");
  EXPECT_EQ(r.source, GTEXT_MARKUP_ENCODING_DEFAULT);
  EXPECT_TRUE(r.transport_encoding_ignored);
  EXPECT_TRUE(r.default_encoding_ignored);
  // the default, when nothing else says
  o = options_with(nullptr, "windows-1250");
  EXPECT_EQ(sniff_name("<p>", &o, &r), "windows-1250");
  EXPECT_EQ(r.source, GTEXT_MARKUP_ENCODING_CALLER_DEFAULT);
  EXPECT_EQ(r.confidence, GTEXT_HTML_TENTATIVE);
  // ignore_meta: the document's declaration is not looked for
  o = options_with();
  o.ignore_meta = true;
  EXPECT_EQ(sniff_name("<meta charset=koi8-r>", &o, &r), "UTF-8");
  // arguments
  EXPECT_EQ(gtext_html_sniff_encoding("x", 1, nullptr, nullptr), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(gtext_html_sniff_encoding(nullptr, 1, nullptr, &r), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(r.status, GTEXT_HTML_E_INVALID);
  EXPECT_EQ(gtext_html_sniff_encoding(nullptr, 0, nullptr, &r), GTEXT_HTML_OK);
  EXPECT_STREQ(r.encoding_name, "UTF-8");
  o = options_with();
  o.transport_encoding_certainty = static_cast<GTEXT_HTML_Encoding_Certainty>(5);
  EXPECT_EQ(gtext_html_sniff_encoding("x", 1, &o, &r), GTEXT_HTML_E_INVALID);
}

TEST(HtmlSniff, MetaCharsetShapes) {
  struct Row {
    const char * html;
    const char * want; // "" for no declaration (the default)
  };
  const Row rows[] = {
      {"<meta charset=\"iso8859-2\">", "ISO-8859-2"},
      {"<meta charset='iso8859-2'>", "ISO-8859-2"},
      {"<meta charset=iso8859-2>", "ISO-8859-2"},
      {"<META CHARSET=ISO8859-2>", "ISO-8859-2"},
      {"<meta   charset  =  iso8859-2 >", "ISO-8859-2"},
      {"<meta\tcharset=iso8859-2>", "ISO-8859-2"},
      {"<meta/charset=iso8859-2>", "ISO-8859-2"},
      {"<meta foo=bar charset=iso8859-2>", "ISO-8859-2"},
      {"<meta charset=iso8859-2 foo=bar>", "ISO-8859-2"},
      {"<meta http-equiv=Content-Type content=\"text/html; charset=iso8859-2\">", "ISO-8859-2"},
      {"<meta content=\"text/html; charset=iso8859-2\" http-equiv=content-type>", "ISO-8859-2"},
      {"<meta http-equiv=content-type content='text/html;charset=iso8859-2'>", "ISO-8859-2"},
      {"<meta http-equiv=content-type content=text/html;charset=iso8859-2>", "ISO-8859-2"},
      {"<meta http-equiv=content-type content=\"charset = 'iso8859-2'\">", "ISO-8859-2"},
      {"<meta http-equiv=refresh content=\"0; charset=iso8859-2\">", ""},        // no pragma
      {"<meta content=\"charset=iso8859-2\">", ""},                              // content without http-equiv
      {"<meta http-equiv=content-type content=\"text/html\">", ""},              // no charset in it
      {"<meta charset=nonsense>", ""},                                           // failure: next byte
      {"<meta charset=nonsense charset=iso8859-2>", ""},                         // the duplicate is ignored
      {"<meta charset=iso8859-2 charset=koi8-r>", "ISO-8859-2"},
      {"<meta charset=nonsense http-equiv=content-type content=\"charset=iso8859-2\">", ""}, // charset is not null
      {"<meta http-equiv=content-type content=\"charset=iso8859-2\" charset=koi8-r>", "KOI8-R"}, // charset attribute wins
      {"<meta charset=utf-16>", "UTF-8"},                                        // a declared UTF-16 means UTF-8
      {"<meta charset=utf-16le>", "UTF-8"},
      {"<meta charset=unicodefffe>", "UTF-8"},
      {"<meta charset=x-user-defined>", "windows-1252"},                         // ... and x-user-defined means windows-1252
      {"<meta charset=latin1>", "windows-1252"},
      {"<meta charset=\" iso8859-2 \">", "ISO-8859-2"},                          // white space around a label is trimmed
      {"<!-- <meta charset=koi8-r> --><meta charset=iso8859-2>", "ISO-8859-2"},  // a comment hides one
      {"<!--><meta charset=koi8-r>", "KOI8-R"},                                  // `<!-->` is a whole comment
      {"<!---><meta charset=koi8-r>", "KOI8-R"},
      {"<!----><meta charset=koi8-r>", "KOI8-R"},
      {"<!-- ><meta charset=koi8-r> -->", ""},
      {"<?php <meta charset=koi8-r> ?><meta charset=iso8859-2>", "ISO-8859-2"},  // `<?` runs to the first `>`
      {"<!doctype <meta charset=koi8-r>", "ISO-8859-2" /* replaced below */},
      {"<metafoo charset=koi8-r>", ""},                                          // not a meta
      {"<meta>", ""},
      {"<meta charset>", ""},
      {"<meta charset=>", ""},
      {"</meta charset=koi8-r>", ""},                                            // an end tag's attributes are read, and a meta rule does not apply
      {"<div class=\"<meta charset=koi8-r>\"><meta charset=iso8859-2>", "ISO-8859-2"}, // quoted `>` hides
      {"<p title='x'><meta charset=koi8-r>", "KOI8-R"},
      {"<a<meta charset=koi8-r>", "KOI8-R"},                                     // `<a` reads to a delimiter; `<meta` is a new tag after the `>`? no: see below
  };
  GTEXT_HTML_Sniff_Result r;
  for (const Row & row : rows) {
    std::string want = row.want;
    std::string html = row.html;
    if (html == "<!doctype <meta charset=koi8-r>") {
      want = "";
    }
    if (html == "<a<meta charset=koi8-r>") {
      want = ""; // the tag `<a<meta` has the name `a<meta`; its attribute is the charset, but that is not a meta element
    }
    const std::string got = sniff_name(html, nullptr, &r);
    EXPECT_EQ(got, want.empty() ? "UTF-8" : want) << html;
    EXPECT_EQ(r.source, want.empty() ? GTEXT_MARKUP_ENCODING_DEFAULT : GTEXT_MARKUP_ENCODING_DECLARATION) << html;
    EXPECT_EQ(r.confidence, GTEXT_HTML_TENTATIVE) << html;
  }
}

TEST(HtmlSniff, TheFirstThousandAndTwentyFourBytes) {
  const std::string meta = "<meta charset=iso8859-2>";
  for (size_t at : {size_t(0), size_t(900), size_t(1000), size_t(1024) - meta.size() - 1, size_t(1024) - meta.size(),
           size_t(1024) - meta.size() + 1, size_t(1100)}) {
    // the tag ends at `at + meta.size()`: it is seen if all of it, and the byte after, are within the first 1024 bytes
    const std::string doc = std::string(at, ' ') + meta + std::string(2000, ' ');
    const size_t end = at + meta.size(); // one past the `>`
    // The attribute reader needs the byte after the value: the `>` is inside the 1024 when end <= 1024.
    const bool seen = end <= 1024;
    EXPECT_EQ(sniff_name(doc), seen ? "ISO-8859-2" : "UTF-8") << at;
  }
  // a quoted value is complete at its closing quote, but the standard then advances past it: no byte after it, no answer
  const std::string quoted = "<meta charset=\"iso8859-2\"";
  for (size_t fill : {size_t(1024) - quoted.size() - 1, size_t(1024) - quoted.size()}) {
    const std::string doc = std::string(fill, ' ') + quoted + ">" + std::string(10, ' ');
    const bool seen = fill + quoted.size() < 1024;
    EXPECT_EQ(sniff_name(doc), seen ? "ISO-8859-2" : "UTF-8") << fill;
  }
  // a shorter input is all there is
  EXPECT_EQ(sniff_name("<meta charset=iso8859-2>"), "ISO-8859-2");
  EXPECT_EQ(sniff_name("<meta charset=iso8859-2"), "UTF-8") << "no byte after the value: out of bytes";
  EXPECT_EQ(sniff_name("<meta charset=\"iso8859-2\""), "UTF-8");
}

TEST(HtmlSniff, XmlDeclarationAndUtf16Prescan) {
  GTEXT_HTML_Sniff_Result r;
  // when the prescan runs out of bytes without an answer, an XML declaration is read
  EXPECT_EQ(sniff_name("<?xml version=\"1.0\" encoding=\"iso-8859-2\"?><html>", nullptr, &r), "ISO-8859-2");
  EXPECT_EQ(r.source, GTEXT_MARKUP_ENCODING_DECLARATION);
  EXPECT_EQ(sniff_name("<?xml encoding='koi8-r'?>"), "KOI8-R");
  EXPECT_EQ(sniff_name("<?xml version=\"1.0\"  encoding  =  \"koi8-r\" ?>"), "KOI8-R");
  EXPECT_EQ(sniff_name("<?xml version=\"1.0\" encoding=\"utf-16\"?>"), "UTF-8");
  EXPECT_EQ(sniff_name("<?xml version=\"1.0\" encoding=\"nonsense\"?>"), "UTF-8");
  EXPECT_EQ(sniff_name("<?xml version=\"1.0\" encoding=\"koi8-r \"?>"), "UTF-8") << "a space inside fails";
  EXPECT_EQ(sniff_name("<?xml version=\"1.0\"?><html encoding=\"koi8-r\">"), "UTF-8") << "`encoding` is after the `>`";
  EXPECT_EQ(sniff_name(" <?xml encoding=\"koi8-r\"?>"), "UTF-8") << "it must be the first thing";
  EXPECT_EQ(sniff_name("<?xml encoding=\"koi8-r\"?>"), "KOI8-R");
  EXPECT_EQ(sniff_name("<?xml encoding=koi8-r?>"), "UTF-8");
  EXPECT_EQ(sniff_name("<?xml encoding=\"koi8-r?>"), "UTF-8");
  // a meta beats it: the prescan returns first
  EXPECT_EQ(sniff_name("<?xml encoding=\"koi8-r\"?><meta charset=iso8859-2>"), "ISO-8859-2");
  // UTF-16 `<?x` without a mark
  EXPECT_EQ(sniff_name(std::string("<\0?\0x\0m\0l\0", 10), nullptr, &r), "UTF-16LE");
  EXPECT_EQ(r.source, GTEXT_MARKUP_ENCODING_DECLARATION);
  EXPECT_EQ(r.confidence, GTEXT_HTML_TENTATIVE);
  EXPECT_EQ(sniff_name(std::string("\0<\0?\0x\0m\0l", 10)), "UTF-16BE");
  EXPECT_EQ(sniff_name(std::string("<\0?\0y\0", 6)), "UTF-8");
  // ignore_meta skips all of it
  GTEXT_HTML_Parse_Options o = options_with();
  o.ignore_meta = true;
  EXPECT_EQ(sniff_name("<?xml encoding=\"koi8-r\"?>", &o), "UTF-8");
  EXPECT_EQ(sniff_name(std::string("<\0?\0x\0", 6), &o), "UTF-8");
}

// ---- parsing in an encoding ---------------------------------------------------------------------------------

TEST(HtmlEncodingParse, ADeclaredEncodingIsUsedAndRecorded) {
  // 0xE9 is e-acute in windows-1252 and in ISO-8859-2, and U+0451 in KOI8-R.
  const std::string doc = std::string("<!DOCTYPE html><meta charset=koi8-r><p>") + "\xA3" + "</p>";
  const Outcome o = parse(doc);
  ASSERT_TRUE(o.ok());
  EXPECT_EQ(o.encoding, "KOI8-R");
  EXPECT_EQ(o.source, GTEXT_MARKUP_ENCODING_DECLARATION);
  EXPECT_EQ(o.confidence, GTEXT_HTML_CERTAIN) << "the parser met the meta and it agreed";
  EXPECT_FALSE(o.reparsed);
  EXPECT_NE(o.tree.find("\xD1\x91"), std::string::npos) << o.tree; // U+0451
  GTEXT_Markup_Encoding_Source source;
  EXPECT_STREQ(gtext_markup_document_encoding(o.doc, &source), "KOI8-R");
  EXPECT_EQ(source, GTEXT_MARKUP_ENCODING_DECLARATION);
}

TEST(HtmlEncodingParse, ATransportLabelBeatsTheDocument) {
  const std::string doc = std::string("<meta charset=utf-8><p>") + "\xE9" + "</p>";
  const GTEXT_HTML_Parse_Options o = options_with("iso-8859-2");
  const Outcome got = parse(doc, &o);
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got.encoding, "ISO-8859-2");
  EXPECT_EQ(got.source, GTEXT_MARKUP_ENCODING_CALLER);
  EXPECT_EQ(got.confidence, GTEXT_HTML_CERTAIN);
  EXPECT_FALSE(got.reparsed);
  EXPECT_NE(got.tree.find("\xC3\xA9"), std::string::npos) << got.tree; // U+00E9 in ISO-8859-2
}

TEST(HtmlEncodingParse, ALateMetaRestartsTheParse) {
  // 1100 bytes of comment first, so that the prescan does not see the meta; the head is still open when it comes.
  const std::string pad = "<!--" + std::string(1100, 'x') + "-->";
  const std::string doc = pad + "<meta charset=shift_jis><title>" + "\x83\x41" + "</title><p>" + "\x82\xA0" + "</p>";
  const Outcome got = parse(doc);
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got.encoding, "Shift_JIS");
  EXPECT_EQ(got.source, GTEXT_MARKUP_ENCODING_DECLARATION);
  EXPECT_TRUE(got.reparsed);
  EXPECT_EQ(got.confidence, GTEXT_HTML_CERTAIN);
  EXPECT_NE(got.tree.find("\xE3\x82\xA2"), std::string::npos) << got.tree; // the katakana A
  // equal to a parse that began in Shift_JIS
  const GTEXT_HTML_Parse_Options t = options_with("shift_jis");
  const Outcome direct = parse(doc, &t);
  ASSERT_TRUE(direct.ok());
  EXPECT_EQ(got.tree, direct.tree);
  EXPECT_EQ(got.errors, direct.errors);
  EXPECT_FALSE(direct.reparsed);
  // ... and in any chunking
  for (const Cut & cut : {Cut{Cut::WHOLE, 0}, Cut{Cut::BYTES, 0}, Cut{Cut::SPLIT, 1}, Cut{Cut::SPLIT, 1024}, Cut{Cut::SPLIT, 1100},
           Cut{Cut::SPLIT, 1130}, Cut{Cut::PIECES, 7}, Cut{Cut::PIECES, 1000}}) {
    const Outcome c = parse_cut(doc, nullptr, cut);
    ASSERT_TRUE(c.ok()) << cut.kind << " " << cut.at;
    EXPECT_EQ(c.tree, got.tree) << cut.kind << " " << cut.at;
    EXPECT_EQ(c.errors, got.errors) << cut.kind << " " << cut.at;
    EXPECT_EQ(c.encoding, "Shift_JIS");
    EXPECT_TRUE(c.reparsed);
    EXPECT_EQ(c.input_bytes, doc.size());
  }
  // a second late meta after the restart is an ordinary element
  const std::string two = pad + "<meta charset=shift_jis><meta charset=koi8-r><p>x";
  const Outcome second = parse(two);
  ASSERT_TRUE(second.ok());
  EXPECT_EQ(second.encoding, "Shift_JIS");
  EXPECT_NE(second.tree.find("charset=\"koi8-r\""), std::string::npos) << second.tree;
}

TEST(HtmlEncodingParse, AMetaThatAgreesOrComesTooLateChangesNothing) {
  const std::string pad = "<!--" + std::string(1100, 'x') + "-->";
  // the same as the default: confirmed, no restart
  const Outcome same = parse(pad + "<meta charset=utf-8><p>x");
  ASSERT_TRUE(same.ok());
  EXPECT_EQ(same.encoding, "UTF-8");
  EXPECT_FALSE(same.reparsed);
  EXPECT_EQ(same.confidence, GTEXT_HTML_CERTAIN);
  EXPECT_EQ(same.source, GTEXT_MARKUP_ENCODING_DEFAULT);
  // after the head: an element, nothing more
  const Outcome body = parse(pad + "<p>x<meta charset=koi8-r>");
  ASSERT_TRUE(body.ok());
  EXPECT_EQ(body.encoding, "UTF-8");
  EXPECT_FALSE(body.reparsed);
  EXPECT_EQ(body.confidence, GTEXT_HTML_TENTATIVE);
  // UTF-16 in the meta means UTF-8
  const Outcome sixteen = parse(pad + "<meta charset=utf-16le><p>x");
  EXPECT_EQ(sixteen.encoding, "UTF-8");
  EXPECT_FALSE(sixteen.reparsed);
  // ignore_meta: neither the prescan nor the restart
  GTEXT_HTML_Parse_Options ig = options_with();
  ig.ignore_meta = true;
  const Outcome ignored = parse("<meta charset=koi8-r>" + pad + "<meta charset=iso8859-2><p>x", &ig);
  EXPECT_EQ(ignored.encoding, "UTF-8");
  EXPECT_FALSE(ignored.reparsed);
  // a certain label: nothing overrides it
  const GTEXT_HTML_Parse_Options cert = options_with("windows-1252");
  const Outcome label = parse(pad + "<meta charset=koi8-r><p>x", &cert);
  EXPECT_EQ(label.encoding, "windows-1252");
  EXPECT_FALSE(label.reparsed);
  EXPECT_EQ(label.confidence, GTEXT_HTML_CERTAIN);
  // a tentative one can be overridden
  GTEXT_HTML_Parse_Options tent = options_with("windows-1252");
  tent.transport_encoding_certainty = GTEXT_HTML_TENTATIVE;
  const Outcome overridden = parse(pad + "<meta charset=koi8-r><p>x", &tent);
  EXPECT_EQ(overridden.encoding, "KOI8-R");
  EXPECT_TRUE(overridden.reparsed);
}

TEST(HtmlEncodingParse, UnknownBytesAreReplacedAndReported) {
  // 0x81 is undefined in windows-1253? use ISO-8859-3, which has holes: 0xA5 is one.
  const GTEXT_HTML_Parse_Options o = options_with("iso-8859-3");
  const Outcome got = parse(std::string("<p>a") + "\xA5" + "b", &o);
  ASSERT_TRUE(got.ok());
  EXPECT_NE(got.tree.find("a\xEF\xBF\xBD" "b"), std::string::npos) << got.tree;
  ASSERT_FALSE(got.errors.empty());
  bool found = false;
  for (const Err & e : got.errors) {
    if (e.name == "invalid-utf-8-in-input-stream") {
      found = true;
      EXPECT_EQ(e.offset, 4u);
      EXPECT_EQ(e.column, 5u);
    }
  }
  EXPECT_TRUE(found);
}

TEST(HtmlEncodingParse, Utf16WithAndWithoutAMark) {
  const std::string text = "<!DOCTYPE html><title>t</title><p>\xE3\x81\x82 \xF0\x9F\x98\x80</p>";
  const Outcome reference = parse(text);
  ASSERT_TRUE(reference.ok());
  for (int be = 0; be < 2; be++) {
    const std::string with_mark = utf16(text, be, true);
    Outcome got = parse(with_mark);
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.tree, reference.tree);
    EXPECT_EQ(got.encoding, be ? "UTF-16BE" : "UTF-16LE");
    EXPECT_EQ(got.source, GTEXT_MARKUP_ENCODING_BOM);
    EXPECT_EQ(got.confidence, GTEXT_HTML_CERTAIN);
    EXPECT_EQ(got.bom_length, 2u);
    EXPECT_EQ(got.input_bytes, with_mark.size());
    const std::string bare = utf16(text, be, false);
    const GTEXT_HTML_Parse_Options o = options_with(be ? "utf-16be" : "utf-16le");
    got = parse(bare, &o);
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.tree, reference.tree);
    EXPECT_EQ(got.source, GTEXT_MARKUP_ENCODING_CALLER);
  }
}

TEST(HtmlEncodingParse, TheResultOfAFragmentIsCharacters) {
  const GTEXT_HTML_Fragment_Context div = ctx(GTEXT_HTML_NS_HTML, "div");
  GTEXT_HTML_Parse_Options o = options_with("shift_jis", "nonsense");
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_Markup_Node * frag = nullptr;
  GTEXT_HTML_Result r;
  std::memset(&r, 0, sizeof(r));
  ASSERT_EQ(gtext_html_parse_fragment(&div, "\xE3\x81\x82", 3, &o, &doc, &frag, &r), GTEXT_HTML_OK);
  EXPECT_STREQ(r.encoding, "UTF-8") << "the characters are already decoded";
  EXPECT_EQ(r.encoding_confidence, GTEXT_HTML_IRRELEVANT);
  EXPECT_EQ(r.encoding_source, GTEXT_MARKUP_ENCODING_CALLER);
  EXPECT_TRUE(r.default_encoding_ignored);
  EXPECT_FALSE(r.transport_encoding_ignored);
  EXPECT_EQ(dump_fragment(frag), "| \"\xE3\x81\x82\"\n");
  gtext_html_result_free(&r);
  gtext_markup_document_free(doc);
}

// ---- the families -----------------------------------------------------------------------------------------

namespace {

struct Family {
  const char * name;        // for messages
  GTEXT_Encoding encoding;
  const char * label;       // a transport label that names it
  const char * payload;     // UTF-8 text that the encoding can write
  bool bom;                 // UTF-16: with a mark
  bool be;
};

const Family kFamilies[] = {
    {"utf-8", GTEXT_ENCODING_UTF_8, "utf-8", "\xC3\xA9\xE6\x97\xA5\xE6\x9C\xAC\xF0\x9F\x98\x80", false, false},
    {"utf-16le", GTEXT_ENCODING_UTF_16LE, "utf-16le", "\xC3\xA9\xE6\x97\xA5\xE6\x9C\xAC\xF0\x9F\x98\x80", true, false},
    {"utf-16be", GTEXT_ENCODING_UTF_16BE, "utf-16be", "\xC3\xA9\xE6\x97\xA5\xE6\x9C\xAC\xF0\x9F\x98\x80", true, true},
    {"windows-1252", GTEXT_ENCODING_WINDOWS_1252, "windows-1252", "\xC3\xA9\xE2\x82\xAC\xC3\xBC", false, false},
    {"iso-8859-2", GTEXT_ENCODING_ISO_8859_2, "iso-8859-2", "\xC4\x9B\xC5\xA1\xC4\x8D\xC5\x99", false, false},
    {"shift_jis", GTEXT_ENCODING_SHIFT_JIS, "shift_jis", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\x82\xEF\xBD\xB1", false, false},
    {"euc-jp", GTEXT_ENCODING_EUC_JP, "euc-jp", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\x82\xEF\xBD\xB1", false, false},
    {"iso-2022-jp", GTEXT_ENCODING_ISO_2022_JP, "iso-2022-jp", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\x82", false, false},
    {"gbk", GTEXT_ENCODING_GBK, "gbk", "\xE4\xB8\xAD\xE6\x96\x87\xE6\xB1\x89\xE5\xAD\x97", false, false},
    {"big5", GTEXT_ENCODING_BIG5, "big5", "\xE4\xB8\xAD\xE6\x96\x87\xE6\xBC\xA2\xE5\xAD\x97", false, false},
    {"euc-kr", GTEXT_ENCODING_EUC_KR, "euc-kr", "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4", false, false},
};

/// The documents. `@` stands for the family's payload. They reach every kind of position: text in each tokenizer state, attributes,
/// comments, a doctype, raw text, foster parented text, character references, a carriage return, a NUL, an abandoned end tag.
const char * const kTemplates[] = {
    "<!DOCTYPE html><html><head><title>@</title></head><body><p class=\"@\">@ text @</p></body></html>",
    "<p>@<b>@</b>@<i>@",
    "<!-- @ --><p>@",
    "<!DOCTYPE @><p>x",
    "<table>@<tr><td>@</td></tr>@</table>",
    "<table> @ <b>x</b></table>",
    "<script>var s = \"@\"; if (a < b) {}</script><p>@",
    "<script>@</scr</script>@</scrip x>@",
    "<style>@ { color: red }</style><textarea>@&amp;@</textarea><p>@",
    "<title>@</tit>@&lt;/title>@</title>@",
    "<p>@&amp;@&lt;@&#x41;@&#65;@&eacute;@&notit;@&bogus;@&amp @",
    // the errors raised at the place after a character just read: a reference's end (where a shift sequence or a wide character can lie between
    // it and the next character, which a reader that has not got the next yet cannot know), a missing semicolon, a comment opened wrongly
    "<p>@&#0;@&#x110000;@&#xD800;@&#xFDD0;@&#x80;@&#13;@&#0;",
    "<p>@&amp@&notit@&#65@&lt<!x>@<!-@>@<!DOCTYPE>@<!>@<!",
    "<a title=\"@&amp;@\" id=@ x='@'>@</a>@<a title=@>",
    "<p>\r\n@\r@\n@\r\n\r\n@\r",
    "<p>@\x0b@</p>@\x01",
    "<@-tag @-attr=@>@</@-tag>",
    "<svg><title>@</title><foreignObject>@<p>@</p></foreignObject><desc>@</desc></svg><math><mi>@</mi></math>",
    "<select><option>@<option>@</select><pre>\n@\n\n@</pre><listing>\n@</listing>",
    "<noscript><p>@</noscript><p>@<noframes>@</noframes>",
    "<![CDATA[@]]><svg><![CDATA[@]]>@]]></svg>",
    "<?php @ ?><p>@<?xml @?>",
    "<p>@</ @><p>@</p @>@<p>@<</p>@<!@>@<!--@--!>@<!-->@",
    "<template><td>@</td><tr>@</template>@",
    "<ruby>@<rt>@</ruby><frameset><frame>@</frameset>@",
    "@@@@",
    "",
    "<p",
    "<p a='@",
    "<!--@",
    "<script>@<!--@<script>@</script>@-->@</script>@",
    "<plaintext>@</plaintext>@",
};

std::string with_payload(const char * tmpl, const std::string & payload) {
  std::string out;
  for (const char * p = tmpl; *p; p++) {
    if (*p == '@') {
      out += payload;
    } else {
      out += *p;
    }
  }
  return out;
}

std::string template_text(size_t i) {
  if (i == 13) {
    return std::string("<p>@\0@</p>@\0", 12);
  }
  return kTemplates[i];
}

/// A document in the family's bytes (a mark for the UTF-16 ones, which a file of that encoding has).
std::string family_bytes(const Family & f, const std::string & utf8) {
  if (f.encoding == GTEXT_ENCODING_UTF_16LE || f.encoding == GTEXT_ENCODING_UTF_16BE) {
    return utf16(utf8, f.be, f.bom);
  }
  return encode_as(f.encoding, utf8);
}

struct Located {
  std::string what;
  size_t offset, line, column;
  bool operator==(const Located & o) const {
    return what == o.what && offset == o.offset && line == o.line && column == o.column;
  }
};

void walk(const GTEXT_Markup_Node * n, std::vector<Located> & out) {
  for (const GTEXT_Markup_Node * c = n; c; c = gtext_markup_node_next_sibling(c)) {
    Located l;
    l.what = std::to_string((int)gtext_markup_node_kind(c));
    if (gtext_markup_node_kind(c) == GTEXT_MARKUP_KIND_ELEMENT) {
      l.what += std::string(" ") + gtext_markup_element_local_name(c);
    }
    GTEXT_Markup_Location loc;
    std::memset(&loc, 0, sizeof(loc));
    const bool has = gtext_markup_node_location(c, &loc);
    l.what += has ? " L" : " -";
    l.offset = loc.offset;
    l.line = loc.line;
    l.column = loc.column;
    out.push_back(l);
    walk(gtext_markup_node_first_child(c), out);
    if (gtext_markup_node_kind(c) == GTEXT_MARKUP_KIND_ELEMENT) {
      const GTEXT_Markup_Node * content = gtext_markup_element_template_content(c);
      if (content) {
        walk(gtext_markup_node_first_child(content), out);
      }
    }
  }
}

std::vector<Located> locations(const GTEXT_Markup_Document * doc) {
  std::vector<Located> out;
  walk(gtext_markup_document_root(doc), out);
  return out;
}

struct MapSink {
  std::string utf8;
  std::vector<size_t> offsets;
  std::vector<unsigned char> widths;
};

bool map_write(void * user, const char * bytes, size_t len, const GTEXT_Encoding_Source_Map * map) {
  auto * m = static_cast<MapSink *>(user);
  m->utf8.append(bytes, len);
  m->offsets.insert(m->offsets.end(), map->offsets, map->offsets + len);
  m->widths.insert(m->widths.end(), map->widths, map->widths + len);
  return true;
}

/// The oracle's first half: the Encoding module's decode of the bytes, with where each byte of the UTF-8 came from.
MapSink decode_with_map(GTEXT_Encoding enc, const std::string & bytes) {
  MapSink m;
  GTEXT_Decoder_Options o = gtext_decoder_options_default();
  o.track_offsets = true;
  o.bom_sniffing = true;
  GTEXT_Decoder * d = nullptr;
  EXPECT_EQ(gtext_decoder_new(enc, &o, &d), GTEXT_ENCODING_OK);
  GTEXT_Encoding_Mapped_Sink sink = {map_write, &m};
  EXPECT_EQ(gtext_decoder_decode_mapped(d, bytes.data(), bytes.size(), true, &sink, nullptr), GTEXT_ENCODING_OK);
  gtext_decoder_free(d);
  return m;
}

// An error that the tokenizer reports at the place after the character it has read (a numeric character reference's end, say) is at the end of
// that character in the original; any other is at the start of the character the position names. (Where a shift sequence lies between two
// characters the two are not the same place.)
bool after_kind(const std::string & name) {
  return name == "null-character-reference" || name == "character-reference-outside-unicode-range" || name == "surrogate-character-reference"
      || name == "noncharacter-character-reference" || name == "control-character-reference" || name == "incorrectly-opened-comment";
}

// (a missing semicolon is at the place after the name of a named reference and at the character that ended a numeric one: either)
bool either_kind(const std::string & name) {
  return name == "missing-semicolon-after-character-reference";
}

size_t translate(const MapSink & m, size_t u, size_t total, bool after = false) {
  if (after) {
    return u == 0 ? 0 : (u <= m.offsets.size() ? m.offsets[u - 1] + m.widths[u - 1] : total);
  }
  return u < m.offsets.size() ? m.offsets[u] : total;
}

} // namespace

TEST(HtmlEncodingFamilies, EveryWayOfParsingGivesTheOraclesAnswer) {
  for (const Family & f : kFamilies) {
    for (size_t t = 0; t < sizeof(kTemplates) / sizeof(kTemplates[0]); t++) {
      const std::string utf8 = with_payload(template_text(t).c_str(), f.payload);
      const std::string bytes = family_bytes(f, utf8);
      const std::string where = std::string(f.name) + " template " + std::to_string(t);
      GTEXT_HTML_Parse_Options o = options_with(f.bom ? nullptr : f.label);
      if (f.bom) {
        o.transport_encoding = nullptr; // the mark decides
      }
      // the oracle: decode, parse the UTF-8 with the tokenizer's own reading, take the offsets back
      const MapSink m = decode_with_map(f.encoding, bytes);
      ASSERT_EQ(m.utf8, utf8) << where << ": the oracle's own decode";
      const GTEXT_HTML_Parse_Options plain = options_with("utf-8");
      const Outcome expect = parse(m.utf8, &plain);
      ASSERT_TRUE(expect.ok()) << where;
      const Outcome got = parse(bytes, &o);
      ASSERT_TRUE(got.ok()) << where;
      ASSERT_EQ(got.tree, expect.tree) << where;
      ASSERT_EQ(got.errors.size(), expect.errors.size()) << where;
      for (size_t i = 0; i < got.errors.size(); i++) {
        EXPECT_EQ(got.errors[i].name, expect.errors[i].name) << where << " error " << i;
        EXPECT_EQ(got.errors[i].line, expect.errors[i].line) << where << " error " << i;
        EXPECT_EQ(got.errors[i].column, expect.errors[i].column) << where << " error " << i;
        if (either_kind(expect.errors[i].name)) {
          EXPECT_TRUE(got.errors[i].offset == translate(m, expect.errors[i].offset, bytes.size(), true)
                      || got.errors[i].offset == translate(m, expect.errors[i].offset, bytes.size(), false))
              << where << " error " << i << " " << got.errors[i].name << " at " << got.errors[i].offset;
        }
        else {
          EXPECT_EQ(got.errors[i].offset, translate(m, expect.errors[i].offset, bytes.size(), after_kind(expect.errors[i].name))) << where << " error " << i << " " << got.errors[i].name;
        }
      }
      const std::vector<Located> want = locations(expect.doc);
      const std::vector<Located> have = locations(got.doc);
      ASSERT_EQ(have.size(), want.size()) << where;
      for (size_t i = 0; i < want.size(); i++) {
        EXPECT_EQ(have[i].what, want[i].what) << where;
        EXPECT_EQ(have[i].line, want[i].line) << where << " " << want[i].what;
        EXPECT_EQ(have[i].column, want[i].column) << where << " " << want[i].what;
        const bool located = want[i].what.size() > 1 && want[i].what.compare(want[i].what.size() - 2, 2, " L") == 0;
        EXPECT_EQ(have[i].offset, located ? translate(m, want[i].offset, bytes.size()) : 0u)
            << where << " node " << want[i].what << " at " << want[i].offset;
      }
      EXPECT_EQ(got.encoding, gtext_encoding_name(f.encoding)) << where;
      EXPECT_EQ(got.input_bytes, bytes.size());
      EXPECT_EQ(got.source, f.bom ? GTEXT_MARKUP_ENCODING_BOM : GTEXT_MARKUP_ENCODING_CALLER) << where;
      // every way of cutting the input, and a parser fed one byte at a time, say the same
      for (size_t cut = 0; cut <= bytes.size(); cut++) {
        const Outcome c = parse_cut(bytes, &o, Cut{Cut::SPLIT, cut});
        ASSERT_TRUE(c.ok()) << where << " cut " << cut;
        ASSERT_EQ(c.tree, got.tree) << where << " cut " << cut;
        ASSERT_EQ(c.errors, got.errors) << where << " cut " << cut;
        ASSERT_EQ(c.encoding, got.encoding);
        ASSERT_EQ(c.input_bytes, got.input_bytes);
      }
      const Outcome each = parse_cut(bytes, &o, Cut{Cut::BYTES, 0});
      ASSERT_TRUE(each.ok());
      EXPECT_EQ(each.tree, got.tree) << where;
      EXPECT_EQ(each.errors, got.errors) << where;
    }
  }
}

// ---- a restart, in every family -------------------------------------------------------------------------

extern "C" size_t html_parser_kept_bytes(const GTEXT_HTML_Parser * parser);

TEST(HtmlEncodingFamilies, ARestartGivesWhatAParseThatBeganInTheDeclaredEncodingGives) {
  for (const Family & f : kFamilies) {
    if (f.encoding == GTEXT_ENCODING_UTF_16LE || f.encoding == GTEXT_ENCODING_UTF_16BE || f.encoding == GTEXT_ENCODING_UTF_8
        || f.encoding == GTEXT_ENCODING_ISO_2022_JP) {
      continue; // (the first three cannot declare themselves in ASCII markup; ISO-2022-JP is below)
    }
    SCOPED_TRACE(f.name);
    const std::string pad = "<!--" + std::string(1100, 'x') + "-->";
    const std::string utf8 = pad + "<meta charset=" + f.label + "><title>" + f.payload + "</title><p class=\"" + f.payload + "\">" + f.payload
        + "<table>" + f.payload + "<tr><td>" + f.payload + "</table><!--" + f.payload + "-->";
    const std::string bytes = family_bytes(f, utf8);
    const GTEXT_HTML_Parse_Options label = options_with(f.label);
    const Outcome direct = parse(bytes, &label);
    ASSERT_TRUE(direct.ok());
    EXPECT_FALSE(direct.reparsed);
    const Outcome restarted = parse(bytes);
    ASSERT_TRUE(restarted.ok());
    EXPECT_TRUE(restarted.reparsed);
    EXPECT_EQ(restarted.encoding, gtext_encoding_name(f.encoding));
    EXPECT_EQ(restarted.source, GTEXT_MARKUP_ENCODING_DECLARATION);
    EXPECT_EQ(restarted.confidence, GTEXT_HTML_CERTAIN);
    EXPECT_EQ(restarted.tree, direct.tree);
    EXPECT_EQ(restarted.errors, direct.errors);
    EXPECT_EQ(restarted.input_bytes, bytes.size());
    EXPECT_EQ(locations(restarted.doc), locations(direct.doc));
    // the restart is the same however the bytes arrive: cut at the offsets around the window and the meta, and a byte at a time
    for (size_t cut = 1090; cut < 1160 && cut < bytes.size(); cut++) {
      const Outcome c = parse_cut(bytes, nullptr, Cut{Cut::SPLIT, cut});
      ASSERT_TRUE(c.ok()) << cut;
      ASSERT_EQ(c.tree, direct.tree) << cut;
      ASSERT_EQ(c.errors, direct.errors) << cut;
      ASSERT_TRUE(c.reparsed) << cut;
    }
    for (size_t piece : {size_t(1), size_t(7), size_t(500), size_t(1023), size_t(1024), size_t(1025)}) {
      const Outcome c = parse_cut(bytes, nullptr, Cut{Cut::PIECES, piece});
      ASSERT_TRUE(c.ok()) << piece;
      EXPECT_EQ(c.tree, direct.tree) << piece;
      EXPECT_EQ(c.errors, direct.errors) << piece;
      EXPECT_TRUE(c.reparsed) << piece;
    }
    // ... and a meta inside the window is not a restart at all, in any cut
    const std::string early = "<meta charset=" + std::string(f.label) + ">" + family_bytes(f, std::string("<p>") + f.payload);
    const Outcome e = parse(early);
    ASSERT_TRUE(e.ok());
    EXPECT_FALSE(e.reparsed);
    EXPECT_EQ(e.encoding, gtext_encoding_name(f.encoding));
    EXPECT_EQ(e.confidence, GTEXT_HTML_CERTAIN);
    EXPECT_EQ(e.source, GTEXT_MARKUP_ENCODING_DECLARATION);
  }
}

TEST(HtmlEncodingFamilies, Iso2022JpDeclaresItselfAndRestarts) {
  const Family & f = kFamilies[7];
  ASSERT_EQ(f.encoding, GTEXT_ENCODING_ISO_2022_JP);
  const std::string pad = "<!--" + std::string(1100, 'x') + "-->";
  const std::string utf8 = pad + "<meta charset=iso-2022-jp><title>" + f.payload + "</title><p>" + f.payload + "</p>";
  const std::string bytes = family_bytes(f, utf8);
  const GTEXT_HTML_Parse_Options label = options_with("iso-2022-jp");
  const Outcome direct = parse(bytes, &label);
  const Outcome restarted = parse(bytes);
  ASSERT_TRUE(direct.ok() && restarted.ok());
  EXPECT_TRUE(restarted.reparsed);
  EXPECT_EQ(restarted.tree, direct.tree);
  EXPECT_EQ(restarted.errors, direct.errors);
  EXPECT_EQ(locations(restarted.doc), locations(direct.doc));
  EXPECT_EQ(parse_cut(bytes, nullptr, Cut{Cut::BYTES, 0}).tree, direct.tree);
}

// ---- positions, worked by hand -------------------------------------------------------------------------------

TEST(HtmlEncodingPositions, ShiftJisCharactersAreTwoBytesAndOneColumn) {
  // "<p>" + 日本 (93 FA 96 7B) + "<b>" + 語 (8C EA) + "x" + "</i>" -- the stray end tag is an error at a place after four kanji bytes and a tag
  const std::string doc = std::string("<!DOCTYPE html><p>") + "\x93\xFA\x96\x7B" + "<b>" + "\x8C\xEA" + "x</i>";
  const GTEXT_HTML_Parse_Options o = options_with("shift_jis");
  const Outcome got = parse(doc, &o);
  ASSERT_TRUE(got.ok());
  const Err * stray = nullptr;
  for (const Err & e : got.errors) {
    if (e.name == "unexpected-end-tag") {
      stray = &e;
    }
  }
  ASSERT_NE(stray, nullptr);
  // `<!DOCTYPE html>` is 15 bytes, `<p>` 3, 日本 4, `<b>` 3, 語 2, `x` 1: the `</i>` begins at byte 28, in column 15 + 3 + 2 + 3 + 1 + 1 + 1
  EXPECT_EQ(stray->offset, 28u);
  EXPECT_EQ(stray->line, 1u);
  EXPECT_EQ(stray->column, 1u + 15 + 3 + 2 + 3 + 1 + 1);
  // the element's locations too
  const std::vector<Located> l = locations(got.doc);
  bool saw_b = false;
  for (const Located & n : l) {
    if (n.what == "4 b L") {
      saw_b = true;
      EXPECT_EQ(n.offset, 15u + 3 + 4);
      EXPECT_EQ(n.column, 1u + 15 + 3 + 2);
    }
  }
  EXPECT_TRUE(saw_b);
}

TEST(HtmlEncodingPositions, Utf16CountsBytesAndTheMarkTakesTwo) {
  // FF FE, "<p>" (6 bytes), a surrogate pair (4 bytes, 2 columns), a lone trail surrogate (2 bytes, a replacement), "</b>"
  std::string doc = "\xFF\xFE";
  for (unsigned char c : std::string("<p>")) {
    doc += char(c);
    doc += '\0';
  }
  doc += std::string("\x3D\xD8\x00\xDE", 4);
  doc += std::string("\x00\xDC", 2);
  for (unsigned char c : std::string("</b>")) {
    doc += char(c);
    doc += '\0';
  }
  const Outcome got = parse(doc);
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got.bom_length, 2u);
  size_t invalid = 0, stray = 0;
  for (const Err & e : got.errors) {
    if (e.name == "invalid-utf-8-in-input-stream") {
      invalid++;
      EXPECT_EQ(e.offset, 2u + 6 + 4) << "the replaced unit begins after the mark, `<p>` and the pair";
      EXPECT_EQ(e.column, 1u + 3 + 2);
    }
    if (e.name == "unexpected-end-tag") {
      stray++;
      EXPECT_EQ(e.offset, 2u + 6 + 4 + 2);
      EXPECT_EQ(e.column, 1u + 3 + 2 + 1);
    }
  }
  EXPECT_EQ(invalid, 1u);
  EXPECT_EQ(stray, 1u);
  EXPECT_NE(got.tree.find("\xF0\x9F\x98\x80\xEF\xBF\xBD"), std::string::npos) << got.tree;
  // the same, a byte at a time
  const Outcome each = parse_cut(doc, nullptr, Cut{Cut::BYTES, 0});
  EXPECT_EQ(each.errors, got.errors);
  EXPECT_EQ(each.tree, got.tree);
}

TEST(HtmlEncodingPositions, ACarriageReturnAndAReferenceInAnEncodedRun) {
  // text with CR LF, a lone CR, a reference and kanji, in a table so that each character has an error at its own place: the lines and
  // columns are those of the same document in UTF-8 (the offsets are the family test's: it takes them back through the decoder's map)
  const std::string kanji = "\x93\xFA";
  const std::string doc = "<table>" + kanji + "\r\n&amp;" + kanji + "\r" + kanji + "x</table>";
  const std::string utf8 = "<table>\xE6\x97\xA5\r\n&amp;\xE6\x97\xA5\r\xE6\x97\xA5x</table>";
  const GTEXT_HTML_Parse_Options o = options_with("shift_jis");
  const GTEXT_HTML_Parse_Options u = options_with("utf-8");
  const Outcome got = parse(doc, &o);
  const Outcome want = parse(utf8, &u);
  ASSERT_TRUE(got.ok() && want.ok());
  EXPECT_EQ(got.tree, want.tree);
  ASSERT_EQ(got.errors.size(), want.errors.size());
  size_t table_chars = 0;
  for (size_t i = 0; i < got.errors.size(); i++) {
    EXPECT_EQ(got.errors[i].name, want.errors[i].name);
    EXPECT_EQ(got.errors[i].line, want.errors[i].line) << i;
    EXPECT_EQ(got.errors[i].column, want.errors[i].column) << i;
    if (got.errors[i].name == "unexpected-character") {
      table_chars++;
    }
  }
  EXPECT_EQ(table_chars, 7u) << "one for each character foster parented (the kanji, LF, &, the kanji, LF, the kanji, x)";
  // the first character's place, by hand: byte 7, column 8
  for (const Err & e : got.errors) {
    if (e.name == "unexpected-character") {
      EXPECT_EQ(e.offset, 7u);
      EXPECT_EQ(e.column, 8u);
      break;
    }
  }
}

// ---- limits, memory, arguments ----------------------------------------------------------------------------------

TEST(HtmlEncodingLimits, TheInputLimitCountsTheBytesGiven) {
  std::string doc = "\xFF\xFE";
  for (unsigned char c : std::string("<p>x</p>")) {
    doc += char(c);
    doc += '\0';
  }
  GTEXT_HTML_Parse_Options o = options_with();
  o.max_input_bytes = doc.size();
  EXPECT_TRUE(parse(doc, &o).ok());
  for (const Cut & cut : {Cut{Cut::WHOLE, 0}, Cut{Cut::BYTES, 0}, Cut{Cut::SPLIT, 1}, Cut{Cut::SPLIT, 5}}) {
    EXPECT_TRUE(parse_cut(doc, &o, cut).ok()) << cut.kind << " " << cut.at;
  }
  o.max_input_bytes = doc.size() - 1;
  EXPECT_EQ(parse(doc, &o).status, GTEXT_HTML_E_LIMIT_INPUT);
  for (const Cut & cut : {Cut{Cut::WHOLE, 0}, Cut{Cut::BYTES, 0}, Cut{Cut::SPLIT, 1}, Cut{Cut::SPLIT, 5}}) {
    EXPECT_EQ(parse_cut(doc, &o, cut).status, GTEXT_HTML_E_LIMIT_INPUT) << cut.kind << " " << cut.at;
  }
  // the decoded text is longer than the bytes: the limit is on the bytes
  const std::string sjis(100, '\x93'); // 50 pairs ("\x93\x93" is a character)
  GTEXT_HTML_Parse_Options t = options_with("shift_jis");
  t.max_input_bytes = sjis.size() + 3;
  EXPECT_TRUE(parse("<p>" + sjis, &t).ok());
  // a byte of windows-1252 that is three in UTF-8
  const std::string euro(300, '\x80');
  GTEXT_HTML_Parse_Options w = options_with("windows-1252");
  w.max_input_bytes = euro.size();
  EXPECT_TRUE(parse(euro, &w).ok());
}

TEST(HtmlEncodingLimits, TheTextLimitIsOnTheDecodedBytes) {
  const std::string euro(10, '\x80'); // 30 bytes of UTF-8
  GTEXT_HTML_Parse_Options w = options_with("windows-1252");
  w.max_text_bytes = 30;
  EXPECT_TRUE(parse("<p>" + euro, &w).ok());
  w.max_text_bytes = 29;
  EXPECT_EQ(parse("<p>" + euro, &w).status, GTEXT_HTML_E_LIMIT_TEXT);
}

TEST(HtmlEncodingMemory, WhatIsKeptForARestartIsTheHeadAndThenNothing) {
  GTEXT_HTML_Parser * p = nullptr;
  ASSERT_EQ(gtext_html_parser_new(nullptr, &p), GTEXT_HTML_OK);
  // below the window: held, and nothing is built
  EXPECT_EQ(gtext_html_parser_feed(p, "<!DOCTYPE html><title>t</title>", 31), GTEXT_HTML_OK);
  EXPECT_EQ(html_parser_kept_bytes(p), 31u);
  // the window fills: the head is not over yet (only a title), so every byte is kept
  const std::string comment(2000, ' ');
  EXPECT_EQ(gtext_html_parser_feed(p, comment.data(), comment.size()), GTEXT_HTML_OK);
  EXPECT_EQ(html_parser_kept_bytes(p), 31 + 2000u);
  // the body begins: the head is behind, and the bytes are let go
  EXPECT_EQ(gtext_html_parser_feed(p, "<p>x", 4), GTEXT_HTML_OK);
  EXPECT_EQ(html_parser_kept_bytes(p), 0u);
  const std::string text(100000, 'a');
  EXPECT_EQ(gtext_html_parser_feed(p, text.data(), text.size()), GTEXT_HTML_OK);
  EXPECT_EQ(html_parser_kept_bytes(p), 0u) << "later bytes stream";
  gtext_html_parser_free(p);

  // a certain encoding keeps nothing, not even the window
  GTEXT_HTML_Parse_Options o = options_with("utf-8");
  ASSERT_EQ(gtext_html_parser_new(&o, &p), GTEXT_HTML_OK);
  EXPECT_EQ(gtext_html_parser_feed(p, "<!DOCTYPE html>", 15), GTEXT_HTML_OK);
  EXPECT_EQ(html_parser_kept_bytes(p), 0u) << "the first three bytes decide; the tree is being built";
  gtext_html_parser_free(p);

  // ignore_meta keeps nothing either
  o = options_with();
  o.ignore_meta = true;
  ASSERT_EQ(gtext_html_parser_new(&o, &p), GTEXT_HTML_OK);
  EXPECT_EQ(gtext_html_parser_feed(p, "<!DOCTYPE html><!--", 19), GTEXT_HTML_OK);
  EXPECT_EQ(gtext_html_parser_feed(p, comment.data(), comment.size()), GTEXT_HTML_OK);
  EXPECT_EQ(html_parser_kept_bytes(p), 0u);
  gtext_html_parser_free(p);

  // a meta that agrees ends the keeping before the head does
  ASSERT_EQ(gtext_html_parser_new(nullptr, &p), GTEXT_HTML_OK);
  const std::string head = "<meta charset=utf-8>" + std::string(1100, ' ') + "<title>t</title>";
  EXPECT_EQ(gtext_html_parser_feed(p, head.data(), head.size()), GTEXT_HTML_OK);
  EXPECT_EQ(html_parser_kept_bytes(p), 0u);
  gtext_html_parser_free(p);
}

TEST(HtmlEncodingArguments, TheOptionsAreChecked) {
  GTEXT_HTML_Parser * p = nullptr;
  GTEXT_HTML_Parse_Options o = options_with();
  o.transport_encoding_certainty = static_cast<GTEXT_HTML_Encoding_Certainty>(3);
  EXPECT_EQ(gtext_html_parser_new(&o, &p), GTEXT_HTML_E_INVALID);
  GTEXT_Markup_Document * doc = nullptr;
  EXPECT_EQ(gtext_html_parse("x", 1, &o, &doc, nullptr), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(doc, nullptr);
  // the irrelevant confidence is only ever an answer
  o.transport_encoding_certainty = GTEXT_HTML_IRRELEVANT;
  EXPECT_EQ(gtext_html_parser_new(&o, &p), GTEXT_HTML_E_INVALID);
  // an empty document, every route
  for (const char * label : {"utf-8", "shift_jis", "utf-16le", "iso-2022-jp"}) {
    const GTEXT_HTML_Parse_Options l = options_with(label);
    const Outcome e = parse("", &l);
    ASSERT_TRUE(e.ok()) << label;
    EXPECT_EQ(e.input_bytes, 0u);
    EXPECT_EQ(e.tree, parse("").tree) << label;
  }
}

// ---- threads ----------------------------------------------------------------------------------------------

TEST(HtmlEncodingConcurrency, IndependentParsesInDifferentEncodingsShareNothing) {
  // Eight threads, each parsing the documents of every family (and a restarted one) many times, whole and in pieces: each answer is the
  // one a single thread got. (make test-tsan runs this under ThreadSanitizer: the decoder, the sniffer and the serializer's encoder
  // keep no state outside their objects.)
  std::vector<std::pair<std::string, GTEXT_HTML_Parse_Options>> jobs;
  static std::vector<std::string> keep; // (the options point at the labels)
  keep.reserve(64);
  for (const Family & f : kFamilies) {
    const std::string bytes = family_bytes(f, with_payload(kTemplates[0], f.payload));
    keep.push_back(bytes);
    jobs.push_back({bytes, options_with(f.bom ? nullptr : f.label)});
  }
  const std::string pad = "<!--" + std::string(1100, 'x') + "-->";
  keep.push_back(pad + "<meta charset=shift_jis><p>\x93\xFA");
  jobs.push_back({keep.back(), options_with()});
  std::vector<std::string> expected;
  for (auto & j : jobs) {
    const Outcome o = parse(j.first, &j.second);
    ASSERT_TRUE(o.ok());
    expected.push_back(o.tree);
  }
  std::vector<std::string> failures(8);
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([&, t] {
      for (int round = 0; round < 40 && failures[t].empty(); round++) {
        for (size_t i = 0; i < jobs.size(); i++) {
          const Outcome w = parse(jobs[i].first, &jobs[i].second);
          const Outcome p = parse_cut(jobs[i].first, &jobs[i].second, Cut{Cut::PIECES, size_t(3 + (round + t) % 400)});
          if (!w.ok() || !p.ok() || w.tree != expected[i] || p.tree != expected[i]) {
            failures[t] = "job " + std::to_string(i) + " round " + std::to_string(round);
            break;
          }
          GTEXT_HTML_Serialize_Options so = gtext_html_serialize_options_default();
          so.encoding = "shift_jis";
          char * buf = nullptr;
          size_t n = 0;
          if (gtext_html_serialize_buffer(gtext_markup_document_root(w.doc), GTEXT_HTML_SERIALIZE_CHILDREN, &so, &buf, &n, nullptr)
              != GTEXT_HTML_OK && true) {
            // (a character Shift_JIS lacks in a name would be the only refusal; the corpus has none in names)
            failures[t] = "serialize " + std::to_string(i);
            break;
          }
          gtext_html_serialize_buffer_free(nullptr, &so, buf);
        }
      }
    });
  }
  for (auto & th : threads) {
    th.join();
  }
  for (const std::string & f : failures) {
    EXPECT_EQ(f, "");
  }
}

// ---- review: the result of a call with no parser, the reparse limit, the test hooks, pending table text ---------

TEST(HtmlEncodingResult, ConfidenceIsUndecidedWhenThereIsNoEncoding) {
  GTEXT_HTML_Result r;
  EXPECT_EQ(gtext_html_parse("<p>", 3, nullptr, nullptr, &r), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(r.encoding, nullptr);
  EXPECT_EQ(r.encoding_confidence, GTEXT_HTML_UNDECIDED) << "zero is CERTAIN: a failure with no encoding must not read as one";
  EXPECT_EQ(r.encoding_source, GTEXT_MARKUP_ENCODING_UNKNOWN);
  EXPECT_FALSE(r.reparsed);
  EXPECT_FALSE(r.reparse_abandoned);
  gtext_html_result_free(&r);
  EXPECT_EQ(r.encoding_confidence, GTEXT_HTML_UNDECIDED) << "a freed result is cleared the same way";
  // a parse that got as far as a parser has one
  GTEXT_Markup_Document * doc = nullptr;
  ASSERT_EQ(gtext_html_parse("<p>", 3, nullptr, &doc, &r), GTEXT_HTML_OK);
  EXPECT_NE(r.encoding, nullptr);
  EXPECT_EQ(r.encoding_confidence, GTEXT_HTML_TENTATIVE);
  gtext_markup_document_free(doc);
  gtext_html_result_free(&r);
  // ... and so does a failure after it
  GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
  o.max_input_bytes = 1;
  EXPECT_EQ(gtext_html_parse("<p>", 3, &o, &doc, &r), GTEXT_HTML_E_LIMIT_INPUT);
  gtext_html_result_free(&r);
}

namespace {

// A document whose head stays open (a comment pads it) with a declaration of windows-1251 that ends at byte `end_at`, and then a title in that encoding.
std::string meta_ending_at(size_t end_at) {
  const std::string meta = "<meta charset=windows-1251>";
  const size_t pad = end_at - meta.size() - 7;
  return "<!--" + std::string(pad, ' ') + "-->" + meta + "<title>\xCF\xF0</title><p>" + std::string(100, 'x');
}

} // namespace

TEST(HtmlReparseLimit, AMetaThatEndsWithinTheLimitRestartsHoweverTheInputIsCutAndOnePastItDoesNot) {
  GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
  o.max_reparse_bytes = 2048;
  const std::string within = meta_ending_at(2048);
  const std::string past = meta_ending_at(2049);
  ASSERT_EQ(within.find("windows-1251>") + 12, 2047u) << "the meta's last byte is byte 2048";
  const Cut cuts[] = {{Cut::WHOLE, 0}, {Cut::BYTES, 0}, {Cut::PIECES, 7}, {Cut::PIECES, 1000}, {Cut::SPLIT, 2045}, {Cut::SPLIT, 2048},
      {Cut::SPLIT, 2049}, {Cut::RANDOM, 5}};
  // the whole buffer, which keeps nothing and so has nothing to give up: the answer the incremental parser must give when it has not given up
  const Outcome whole_within = parse(within, &o);
  ASSERT_TRUE(whole_within.ok());
  EXPECT_TRUE(whole_within.reparsed);
  EXPECT_FALSE(whole_within.reparse_abandoned);
  for (const Cut & cut : cuts) {
    const Outcome a = parse_cut(within, &o, cut);
    ASSERT_TRUE(a.ok()) << cut.kind << " " << cut.at;
    EXPECT_TRUE(a.reparsed) << cut.kind << " " << cut.at;
    EXPECT_FALSE(a.reparse_abandoned) << cut.kind << " " << cut.at;
    EXPECT_EQ(a.encoding, "windows-1251");
    EXPECT_EQ(a.tree, whole_within.tree) << cut.kind << " " << cut.at;
    EXPECT_EQ(a.errors, whole_within.errors) << cut.kind << " " << cut.at;
    const Outcome b = parse_cut(past, &o, cut);
    ASSERT_TRUE(b.ok()) << cut.kind << " " << cut.at;
    EXPECT_FALSE(b.reparsed) << cut.kind << " " << cut.at;
    EXPECT_TRUE(b.reparse_abandoned) << cut.kind << " " << cut.at;
    EXPECT_EQ(b.encoding, "UTF-8");
    EXPECT_EQ(b.confidence, GTEXT_HTML_TENTATIVE);
    EXPECT_NE(b.tree.find("<meta>"), std::string::npos) << "an ordinary element";
  }
  // gtext_html_parse() keeps nothing, so it has nothing to give up: it restarts for a declaration anywhere in the head
  const Outcome whole_past = parse(past, &o);
  EXPECT_TRUE(whole_past.reparsed);
  EXPECT_FALSE(whole_past.reparse_abandoned);
  // unlimited
  GTEXT_HTML_Parse_Options u = o;
  u.max_reparse_bytes = SIZE_MAX;
  const Outcome unlimited = parse_cut(past, &u, Cut{Cut::PIECES, 7});
  EXPECT_TRUE(unlimited.reparsed);
  EXPECT_FALSE(unlimited.reparse_abandoned);
  // a limit under the prescan's window is the window
  GTEXT_HTML_Parse_Options tiny = o;
  tiny.max_reparse_bytes = 1;
  const Outcome t = parse_cut(meta_ending_at(1024), &tiny, Cut{Cut::BYTES, 0});
  EXPECT_TRUE(t.reparsed || t.encoding == "windows-1251") << "the prescan finds a declaration within the window in any case";
}

TEST(HtmlReparseLimit, TheBytesAreLetGoAndTheDefaultIsFourMebibytes) {
  GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
  o.max_reparse_bytes = 4096;
  GTEXT_HTML_Parser * p = nullptr;
  ASSERT_EQ(gtext_html_parser_new(&o, &p), GTEXT_HTML_OK);
  const std::string pad = "<!--" + std::string(3000, ' ') + "-->";
  ASSERT_EQ(gtext_html_parser_feed(p, pad.data(), pad.size()), GTEXT_HTML_OK);
  EXPECT_EQ(html_parser_kept_bytes(p), pad.size());
  const std::string more(2000, ' ');
  ASSERT_EQ(gtext_html_parser_feed(p, ("<!--" + more + "-->").data(), more.size() + 7), GTEXT_HTML_OK);
  EXPECT_EQ(html_parser_kept_bytes(p), 0u) << "past the limit nothing is kept";
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_HTML_Result r;
  ASSERT_EQ(gtext_html_parser_finish(p, &doc, &r), GTEXT_HTML_OK);
  EXPECT_TRUE(r.reparse_abandoned);
  gtext_markup_document_free(doc);
  gtext_html_result_free(&r);
  gtext_html_parser_free(p);

  // the default: a declaration at 3 MiB restarts, one at 5 MiB does not
  for (size_t at : {size_t(3) << 20, size_t(5) << 20}) {
    const Outcome a = parse_cut(meta_ending_at(at), nullptr, Cut{Cut::PIECES, 65536});
    ASSERT_TRUE(a.ok());
    EXPECT_EQ(a.reparsed, at < (size_t(4) << 20)) << at;
    EXPECT_EQ(a.reparse_abandoned, at >= (size_t(4) << 20)) << at;
  }
}

TEST(HtmlTestHooks, ForceTranscodeIsRefusedOnceTheFirstByteIsFed) {
  GTEXT_HTML_Parser * p = nullptr;
  ASSERT_EQ(gtext_html_parser_new(nullptr, &p), GTEXT_HTML_OK);
  EXPECT_EQ(html_parser_force_transcode(nullptr), GTEXT_HTML_E_INVALID);
  ASSERT_EQ(gtext_html_parser_feed(p, "<p>", 3), GTEXT_HTML_OK);
  EXPECT_EQ(html_parser_force_transcode(p), GTEXT_HTML_E_STATE);
  gtext_html_parser_free(p);
  ASSERT_EQ(gtext_html_parser_new(nullptr, &p), GTEXT_HTML_OK);
  EXPECT_EQ(html_parser_force_transcode(p), GTEXT_HTML_OK);
  gtext_html_parser_free(p);
}

TEST(HtmlEncodingTable, FosteredTextBeyondTheSegmentLimitKeepsItsPlaces) {
  // 3000 pieces of text in a table (each a kanji and a reference, so the tokenizer cuts the run into tokens of at most 1024 segments and more than
  // 1024 pieces wait together), every character an error and every piece foster parented. The places are worked out here from how the document
  // was written, not taken from another parse: piece k's kanji is at byte base + k * width, its `&amp;` after it, and a column counts the
  // characters of the source (a reference is five).
  std::string utf8 = "<!DOCTYPE html><table>";
  for (int i = 0; i < 3000; i++) {
    utf8 += "\xE6\x97\xA5&amp;";
  }
  utf8 += "<tr><td>\xE6\x97\xA5</table>\xE6\x97\xA5";
  struct Case {
    GTEXT_Encoding enc;
    const char * label;
    size_t base, kanji, amp; // bytes before the first piece, and the width of a kanji and of `&amp;`
  };
  for (const Case & c : {Case{GTEXT_ENCODING_SHIFT_JIS, "shift_jis", 22, 2, 5}, Case{GTEXT_ENCODING_UTF_16LE, "utf-16le", 44, 2, 10}}) {
    const std::string sent = c.enc == GTEXT_ENCODING_UTF_16LE ? utf16(utf8, false, false) : encode_as(c.enc, utf8);
    GTEXT_HTML_Parse_Options o = options_with(c.label);
    o.max_errors = 1000000;
    const MapSink m = decode_with_map(c.enc, sent);
    ASSERT_EQ(m.utf8, utf8) << c.label;
    GTEXT_HTML_Parse_Options plain = options_with("utf-8");
    plain.max_errors = 1000000;
    const Outcome expect = parse(m.utf8, &plain);
    ASSERT_TRUE(expect.ok());
    const Cut cuts[] = {{Cut::WHOLE, 0}, {Cut::BYTES, 0}, {Cut::PIECES, 1000}, {Cut::PIECES, 7}};
    for (int k = -1; k < 4; k++) {
      const Outcome got = k < 0 ? parse(sent, &o) : parse_cut(sent, &o, cuts[k]);
      const std::string where = std::string(c.label) + " cut " + std::to_string(k);
      ASSERT_TRUE(got.ok()) << where;
      ASSERT_EQ(got.tree, expect.tree) << where;
      ASSERT_EQ(got.errors.size(), expect.errors.size()) << where;
      size_t seen = 0;
      for (const Err & e : got.errors) {
        if (e.name != "unexpected-character" || seen >= 6000) {
          continue;
        }
        const size_t piece = seen / 2;
        const bool is_amp = seen % 2 == 1;
        EXPECT_EQ(e.offset, c.base + piece * (c.kanji + c.amp) + (is_amp ? c.kanji : 0)) << where << " character " << seen;
        EXPECT_EQ(e.line, 1u) << where << " character " << seen;
        EXPECT_EQ(e.column, 23 + 6 * piece + (is_amp ? 1 : 0)) << where << " character " << seen;
        ASSERT_FALSE(::testing::Test::HasFailure()) << "(the first place that is wrong)";
        seen++;
      }
      EXPECT_EQ(seen, 6000u) << where;
    }
  }
}
