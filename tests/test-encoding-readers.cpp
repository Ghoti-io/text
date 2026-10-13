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
 * One standard, several readers: the Encoding module against the UTF-8 and UTF-16 readers this library already had.
 *
 * The HTML parser's accepted encoding labels are a hand-kept list (src/html/html_api.c), and the XML parser (src/xml/xml_encoding.c) and the HTML
 * tokenizer each read UTF-8 and, for XML, UTF-16 in code of their own. They have not been switched to the Encoding module in this stage, and
 * must not change behaviour here; what is checked is that they agree with it, or differ for a reason that is named:
 *
 *   - the HTML label list is exactly the set of labels of the Encoding module that name UTF-8;
 *   - the HTML tokenizer's UTF-8 reading of every sequence of one and two bytes, and every 17th of three, is the Encoding module's decode (U+FFFD for what
 *     is not well-formed), through the text of a `plaintext` element;
 *   - the XML parser's UTF-8 reading of the same sequences, and its UTF-16LE and UTF-16BE reading of one code unit (all 65,536) and of two (a sample, every
 *     surrogate boundary among them): **it accepts only what is well-formed, and what it accepts is what the Encoding module decodes**; it also refuses
 *     what XML's own `Char` production forbids (the C0 controls, U+FFFE and U+FFFF) however well-formed the encoding of them is. Those are the two
 *     classes of difference, both by design: XML has no U+FFFD substitution and has its own character set. Any other difference fails the test.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/text/encoding.h>
#include <ghoti.io/text/html.h>
#include <ghoti.io/text/xml.h>

#include "encoding_model.h"

#include <gtest/gtest.h>

// The parser's own label test (src/html/html_api.c, declared in html_api_internal.h, a header C++ cannot include): internal, hidden in the shared library, linked from the archive.
extern "C" bool html_label_is_utf8(const char * label);

using namespace enctest;

namespace {

std::string text_of_root(GTEXT_Markup_Document * doc) {
  const GTEXT_Markup_Node * root = gtext_markup_document_element(doc);
  char * text = nullptr;
  size_t n = 0;
  if (!root || gtext_markup_node_text_content(root, nullptr, &text, &n) != GTEXT_MARKUP_OK) {
    return "<no text>";
  }
  std::string s(text, n);
  gtext_markup_string_free(nullptr, text);
  return s;
}

bool xml_char(uint32_t cp) {
  return cp == 0x9 || cp == 0xA || cp == 0xD || (cp >= 0x20 && cp <= 0xD7FF) || (cp >= 0xE000 && cp <= 0xFFFD) || (cp >= 0x10000 && cp <= 0x10FFFF);
}

// The code points of well-formed UTF-8.
std::vector<uint32_t> scalars(const std::string & utf8) {
  return ref_utf8(utf8).cps;
}

struct Tally {
  size_t compared = 0, agreed = 0, refused_ill_formed = 0, refused_not_xml_char = 0;
};

} // namespace

TEST(EncodingReaders, TheHtmlLabelListIsTheEncodingModulesUtf8Labels) {
  size_t utf8_labels = 0;
  for (size_t i = 0; i < gtext_encoding_label_count(); i++) {
    const char * l = nullptr;
    size_t len = 0;
    GTEXT_Encoding e = GTEXT_ENCODING_NONE;
    ASSERT_TRUE(gtext_encoding_label_at(i, &l, &len, &e));
    const std::string label(l, len);
    const bool utf8 = e == GTEXT_ENCODING_UTF_8;
    utf8_labels += utf8;
    for (const std::string & spelling : {label, std::string("  ") + label + "\t", [&] {
           std::string u = label;
           for (auto & c : u) {
             c = char(std::toupper((unsigned char)c));
           }
           return u;
         }()}) {
      EXPECT_EQ(html_label_is_utf8(spelling.c_str()), utf8) << spelling;
    }
  }
  EXPECT_EQ(utf8_labels, 6u);
  // What the parser's two label options accept is accepted by the Encoding module as UTF-8 and nothing else is.
  for (const char * l : {"utf-8", "UTF8", " Unicode-1-1-UTF-8 ", "x-unicode20utf8"}) {
    EXPECT_TRUE(html_label_is_utf8(l));
    EXPECT_EQ(gtext_encoding_for_label(l, std::strlen(l)), GTEXT_ENCODING_UTF_8);
  }
  for (const char * l : {"utf-16", "latin1", "", "utf-8x", "replacement"}) {
    EXPECT_FALSE(html_label_is_utf8(l)) << l;
  }
}

TEST(EncodingReaders, TheHtmlTokenizersUtf8IsTheEncodingModulesDecode) {
  Tally t;
  std::string in;
  for (int n = 1; n <= 3; n++) {
    const uint32_t total = 1u << (8 * n);
    const uint32_t stride = n == 3 ? 17 : 1;
    for (uint32_t v = 0; v < total; v += stride) {
      std::string seq;
      for (int k = 0; k < n; k++) {
        seq += char((v >> (8 * (n - 1 - k))) & 0xFF);
      }
      // A plaintext element holds its text as it is, but for what the input stream does to a NUL and to a carriage return.
      if (seq.find('\0') != std::string::npos || seq.find('\r') != std::string::npos) {
        continue;
      }
      const std::string doc_text = "<plaintext>" + seq;
      GTEXT_Markup_Document * doc = nullptr;
      GTEXT_HTML_Result res;
      std::memset(&res, 0, sizeof res);
      GTEXT_HTML_Parse_Options opts = gtext_html_parse_options_default();
      opts.count_errors_only = true;
      ASSERT_EQ(gtext_html_parse(doc_text.data(), doc_text.size(), &opts, &doc, &res), GTEXT_HTML_OK);
      const std::string html_text = text_of_root(doc);
      gtext_markup_document_free(doc);
      gtext_html_result_free(&res);
      const Out dec = decode_whole(GTEXT_ENCODING_UTF_8, seq);
      t.compared++;
      if (html_text == dec.bytes) {
        t.agreed++;
      } else {
        FAIL() << "HTML tokenizer and Encoding module differ on " << show(seq) << ": [" << show(html_text) << "] vs [" << show(dec.bytes) << "]";
      }
    }
  }
  RecordProperty("compared", std::to_string(t.compared));
  EXPECT_GT(t.compared, 900000u);
  EXPECT_EQ(t.agreed, t.compared);
}

TEST(EncodingReaders, TheXmlParsersUtf8IsTheEncodingModulesDecodeAndWhatXmlForbids) {
  Tally t;
  for (int n = 1; n <= 3; n++) {
    const uint32_t total = 1u << (8 * n);
    const uint32_t stride = n == 3 ? 17 : 1;
    for (uint32_t v = 0; v < total; v += stride) {
      std::string seq;
      for (int k = 0; k < n; k++) {
        seq += char((v >> (8 * (n - 1 - k))) & 0xFF);
      }
      if (seq.find_first_of("\r<>&") != std::string::npos) {
        continue; // markup, and the line end normalization: not what is being read
      }
      const std::string doc_text = "<a>" + seq + "</a>";
      GTEXT_Markup_Document * doc = nullptr;
      GTEXT_XML_Error err;
      std::memset(&err, 0, sizeof err);
      const GTEXT_XML_Status s = gtext_xml_parse(doc_text.data(), doc_text.size(), nullptr, &doc, &err);
      const Out dec = decode_whole(GTEXT_ENCODING_UTF_8, seq);
      t.compared++;
      if (s == GTEXT_XML_OK) {
        const std::string got = text_of_root(doc);
        gtext_markup_document_free(doc);
        ASSERT_EQ(dec.replacements, 0u) << "XML accepted what is not well-formed UTF-8: " << show(seq);
        ASSERT_EQ(got, dec.bytes) << show(seq);
        t.agreed++;
      } else if (dec.replacements > 0) {
        t.refused_ill_formed++;
      } else {
        bool all_char = true;
        for (uint32_t cp : scalars(seq)) {
          all_char = all_char && xml_char(cp);
        }
        ASSERT_FALSE(all_char) << "XML refused well-formed UTF-8 of XML characters: " << show(seq);
        t.refused_not_xml_char++;
      }
    }
  }
  RecordProperty("agreed", std::to_string(t.agreed));
  RecordProperty("refused_ill_formed", std::to_string(t.refused_ill_formed));
  RecordProperty("refused_not_xml_char", std::to_string(t.refused_not_xml_char));
  EXPECT_GT(t.agreed, 30000u);
  EXPECT_GT(t.refused_ill_formed, 100000u);
  EXPECT_GT(t.refused_not_xml_char, 10u);
}

TEST(EncodingReaders, TheXmlParsersUtf16IsTheEncodingModulesDecodeAndWhatXmlForbids) {
  Tally t;
  const uint32_t companions[] = {0x41, 0xD800, 0xDBFF, 0xDC00, 0xDFFF, 0x20AC};
  for (int big = 0; big < 2; big++) {
    const GTEXT_Encoding enc = big ? GTEXT_ENCODING_UTF_16BE : GTEXT_ENCODING_UTF_16LE;
    const auto units = [&](const std::vector<uint32_t> & us) {
      std::string s;
      for (uint32_t u : us) {
        s += char(big ? u >> 8 : u & 0xFF);
        s += char(big ? u & 0xFF : u >> 8);
      }
      return s;
    };
    const auto text = [&](const std::string & s) {
      std::vector<uint32_t> us;
      for (char c : s) {
        us.push_back((unsigned char)c);
      }
      return units(us);
    };
    const std::string bom = units({0xFEFF});
    std::vector<std::vector<uint32_t>> bodies;
    for (uint32_t u = 0; u <= 0xFFFF; u++) {
      bodies.push_back({u});
    }
    for (uint32_t u = 0; u <= 0xFFFF; u += 1) {
      for (uint32_t c : companions) {
        if (u % 5 == 0 || (u >= 0xD7F0 && u <= 0xE010)) {
          bodies.push_back({u, c});
        }
      }
    }
    for (const auto & body : bodies) {
      // the markup characters among them would be markup
      bool markup = false;
      for (uint32_t u : body) {
        markup = markup || u == '<' || u == '>' || u == '&' || u == '\r';
      }
      if (markup) {
        continue;
      }
      const std::string seq = units(body);
      const std::string doc_text = bom + text("<a>") + seq + text("</a>");
      GTEXT_Markup_Document * doc = nullptr;
      GTEXT_XML_Error err;
      std::memset(&err, 0, sizeof err);
      const GTEXT_XML_Status s = gtext_xml_parse(doc_text.data(), doc_text.size(), nullptr, &doc, &err);
      const Out dec = decode_whole(enc, seq);
      t.compared++;
      if (s == GTEXT_XML_OK) {
        const std::string got = text_of_root(doc);
        gtext_markup_document_free(doc);
        ASSERT_EQ(dec.replacements, 0u) << "XML accepted what is not well-formed UTF-16: " << show(seq);
        ASSERT_EQ(got, dec.bytes) << show(seq);
        t.agreed++;
      } else if (dec.replacements > 0) {
        t.refused_ill_formed++;
      } else {
        bool all_char = true;
        for (uint32_t cp : scalars(dec.bytes)) {
          all_char = all_char && xml_char(cp);
        }
        ASSERT_FALSE(all_char) << "XML refused well-formed UTF-16 of XML characters: " << show(seq);
        t.refused_not_xml_char++;
      }
    }
  }
  RecordProperty("agreed", std::to_string(t.agreed));
  EXPECT_GT(t.agreed, 100000u);
  EXPECT_GT(t.refused_ill_formed, 1000u);
  EXPECT_GT(t.refused_not_xml_char, 10u);
}
