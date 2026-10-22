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
 * The incremental CSS parser: the same tree, errors and verdict however the
 * input is cut.
 *
 * Every document below is put through a stream in every way of cutting it that
 * can matter: split once at every byte, a byte at a time, pieces of fixed
 * sizes, random pieces with empty feeds between them. Each is compared field
 * for field (spans, identities and flags of every node, the serialization, the
 * parse errors with their positions, the failure, the encoding that was read)
 * with the one-call parse of the whole. The documents have what a cut can land
 * inside: escapes, comments, strings, urls, multi-byte characters in UTF-8 and
 * in six other encodings, byte order marks, the kilobyte that decides the
 * encoding, and input that is ill-formed in each encoding.
 */

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_bytes_test_support.h"
#include "markup_test_support.h"

extern "C" size_t gtext_css_stream_kept_bytes(const GTEXT_CSS_Stream * stream, size_t * allocated);
extern "C" GTEXT_CSS_Status gtext_css_stream_work(GTEXT_CSS_Tree_Kind kind, const void * data, size_t length, size_t chunk,
    const GTEXT_CSS_Bytes_Options * bytes_options, const GTEXT_CSS_Parse_Options * options, size_t * work);

using namespace css_test;

namespace {

// ---- the documents -----------------------------------------------------------------------------------------

std::vector<std::pair<std::string, std::string>> text_documents() {
  std::vector<std::pair<std::string, std::string>> d;
  d.push_back({"rules",
      "@import url(x.css) layer(a); @media (min-width: 1px) { a { b: c } d: e; }\n"
      "x, y > z { color: red !important; --custom: { a ] b } c; unicode-range: U+0-7F, u+4??;"
      " & .n { m: n } after: rule; font: 12px/1.5 \"q\" }\n"
      "--x:hover { a {} } p { q: url(bad url) 'bad\n; r: s } t } u { v: f(g(h[i{j}])) 1px; }\n"});
  d.push_back({"escapes", "a{b:url(  x\\)y  );c:\"s\\\n t\";d:\\41 bc;e:\\0;f:\\110000 ;g:\\}h} /* c \\*/ */ @m \\@x{y{z:\\"});
  d.push_back({"newlines", std::string("a{\r\nb:c\r\n}\fd{e:f}\r") + std::string("\0 g{h:i}", 8) + "\n\r\n"});
  d.push_back({"unclosed", "a{b:'x\\"});
  d.push_back({"unclosed comment", "a{b:c} /* open"});
  d.push_back({"unclosed url", "a{b:url(abc"});
  d.push_back({"unclosed everything", "@m x{ y( z[ \"s"});
  d.push_back({"multibyte",
      "\xc3\xa9{b:\"\xe6\x97\xa5\xe6\x9c\xac\";c:url(\xc3\xbc);\xe6\x97\xa5:\xf0\x9f\x98\x80}/*\xf0\x9f\x98\x80*/"
      " .\xe2\x82\xac{d:\\e9 \xc3\xa9}"});
  d.push_back({"nesting", "a{b:c; &:hover{d:e} f:g; .h{ i:j } k:l; @media x{m:n; o{p:q}}}"});
  d.push_back({"numbers", "a{b:+1.5e-3px -.5 1e400 1e-400 0.1% 12E+2 9007199254740993 --c:1.0}"});
  d.push_back({"ill-formed utf-8", "a\xc3{b:\xe2\x82}\xed\xa0\x80 c{d:e\xf0\x9f}\x80\x80 f{g:h}"});
  d.push_back({"bom-like", "\xef\xbb\xbf" "a{b:c}"});
  d.push_back({"empty", ""});
  d.push_back({"one byte", "a"});
  d.push_back({"only space", " \n\t "});
  // Past the kilobyte that decides the encoding.
  std::string big = "a{b:c}\n";
  while (big.size() < 2500) {
    big += ".k" + std::to_string(big.size()) + " > p { color: rgb(1, 2, 3); content: \"\xc3\xa9\xe6\x97\xa5\"; }\n";
  }
  d.push_back({"past the window", big});
  return d;
}

struct ByteDoc {
  std::string name;
  std::string bytes;
  std::string protocol;      // empty for none
  GTEXT_Encoding environment = GTEXT_ENCODING_NONE;
  GTEXT_Encoding expect = GTEXT_ENCODING_NONE;
  GTEXT_CSS_Encoding_Source expect_source = GTEXT_CSS_ENCODING_SOURCE_NONE;
  GTEXT_CSS_Bytes_Options options() const {
    GTEXT_CSS_Bytes_Options o = gtext_css_bytes_options_default();
    o.protocol_label = protocol.empty() ? nullptr : protocol.c_str();
    o.environment_encoding = environment;
    return o;
  }
};

// A document padded with spaces so that `@charset "windows-1251"` + padding + `";` ends at byte `end` (exclusive).
std::string charset_ending_at(size_t end) {
  const std::string head = "@charset \"windows-1251";
  const std::string tail = "\";";
  std::string pad;
  const size_t have = head.size() + tail.size();
  if (end > have) {
    pad = std::string(end - have, ' ');
  }
  // The padding goes inside the label (trailing white space is ignored by the label match).
  return head + pad + tail;
}

std::vector<ByteDoc> byte_documents() {
  std::vector<ByteDoc> d;
  auto add = [&](const char * name, std::string bytes, GTEXT_Encoding expect, GTEXT_CSS_Encoding_Source source,
                 std::string protocol = "", GTEXT_Encoding env = GTEXT_ENCODING_NONE) {
    ByteDoc b;
    b.name = name;
    b.bytes = std::move(bytes);
    b.protocol = std::move(protocol);
    b.environment = env;
    b.expect = expect;
    b.expect_source = source;
    d.push_back(std::move(b));
  };
  add("utf-8 default", "a{b:\xc3\xa9}/*\xe6\x97\xa5*/ .\xf0\x9f\x98\x80{}", GTEXT_ENCODING_UTF_8, GTEXT_CSS_ENCODING_SOURCE_DEFAULT);
  add("utf-8 bom", CSS_BYTES("\xef\xbb\xbf" "a{b:\xc3\xa9}.\xc3\xbc{c:\"\xf0\x9f\x98\x80\"}"), GTEXT_ENCODING_UTF_8,
      GTEXT_CSS_ENCODING_SOURCE_BOM);
  add("utf-8 bom beats label", CSS_BYTES("\xef\xbb\xbf" "@charset \"iso-8859-1\"; a{b:\xc3\xa9}"), GTEXT_ENCODING_UTF_8,
      GTEXT_CSS_ENCODING_SOURCE_BOM, "iso-8859-2");
  add("cp1251 charset",
      CSS_BYTES("@charset \x22windows-1251\x22; .\xef\xf0\xe8\xe2\xe5\xf2{\xf6\xe2\xe5\xf2:\xea\xf0\xe0\xf1\xed\xfb\xe9} /* \xea\xee\xec\xec\xe5\xed\xf2\xe0\xf0\xe8\xe9 */ a::before{content:\x22\xb8\x22}\x0a"),
      GTEXT_ENCODING_WINDOWS_1251, GTEXT_CSS_ENCODING_SOURCE_CHARSET_RULE);
  add("cp1251 protocol", CSS_BYTES(".\xef\xf0\xe8\xe2\xe5\xf2{\xf6\xe2\xe5\xf2:\xea\xf0\xe0\xf1\xed\xfb\xe9}"), GTEXT_ENCODING_WINDOWS_1251,
      GTEXT_CSS_ENCODING_SOURCE_PROTOCOL, " Windows-1251 ");
  add("cp1251 environment", CSS_BYTES("a{b:\"\xef\xf0\xe8\xe2\xe5\xf2\"}"), GTEXT_ENCODING_WINDOWS_1251,
      GTEXT_CSS_ENCODING_SOURCE_ENVIRONMENT, "", GTEXT_ENCODING_WINDOWS_1251);
  add("utf-16le bom",
      CSS_BYTES("\xff\xfe" "a\x00{\x00" "b\x00:\x00\xe9\x00}\x00.\x00\xfc\x00{\x00" "c\x00:\x00\x22\x00=\xd8\x00\xde\x22\x00}\x00"),
      GTEXT_ENCODING_UTF_16LE, GTEXT_CSS_ENCODING_SOURCE_BOM);
  add("utf-16be bom",
      CSS_BYTES("\xfe\xff\x00" "a\x00{\x00" "b\x00:\x00\xe9\x00}\x00.\x00\xfc\x00{\x00" "c\x00:\x00\x22\xd8=\xde\x00\x00\x22\x00}"),
      GTEXT_ENCODING_UTF_16BE, GTEXT_CSS_ENCODING_SOURCE_BOM);
  add("utf-16be protocol",
      CSS_BYTES("\x00" "a\x00{\x00" "b\x00:\x00\xe9\x00}\x00.\x00\xfc\x00{\x00" "c\x00:\x00\x22\xd8=\xde\x00\x00\x22\x00}"),
      GTEXT_ENCODING_UTF_16BE, GTEXT_CSS_ENCODING_SOURCE_PROTOCOL, "utf-16be");
  add("utf-16le lone surrogate and odd length", CSS_BYTES("\xff\xfe" "a\x00{\x00\x00\xd8}\x00" "b"), GTEXT_ENCODING_UTF_16LE,
      GTEXT_CSS_ENCODING_SOURCE_BOM);
  add("utf-16 charset is utf-8", CSS_BYTES("@charset \"utf-16le\"; a{b:\xc3\xa9}"), GTEXT_ENCODING_UTF_8,
      GTEXT_CSS_ENCODING_SOURCE_CHARSET_RULE);
  add("gb18030", CSS_BYTES(".\xd6\xd0\xce\xc4{b:c} /*\x95" "2\x82" "6*/ x{y:\x22\x95" "2\x82" "6z\x22}"), GTEXT_ENCODING_GB18030,
      GTEXT_CSS_ENCODING_SOURCE_PROTOCOL, "gb18030");
  add("gb18030 ill-formed", CSS_BYTES(".\xd6\xd0\xce{b:c} /*\x95" "2\x82*/ x{y:\xff\x22}\x81"), GTEXT_ENCODING_GB18030,
      GTEXT_CSS_ENCODING_SOURCE_ENVIRONMENT, "", GTEXT_ENCODING_GB18030);
  add("shift_jis", CSS_BYTES(".\x93\xfa\x96{{x:y} a{b:\x22\xb1\xb2\x22} /*\x94\x5c*/"), GTEXT_ENCODING_SHIFT_JIS,
      GTEXT_CSS_ENCODING_SOURCE_PROTOCOL, "shift_jis");
  add("shift_jis truncated", CSS_BYTES("a{b:c}/*\x94"), GTEXT_ENCODING_SHIFT_JIS, GTEXT_CSS_ENCODING_SOURCE_PROTOCOL, "sjis");
  add("euc-kr", CSS_BYTES(".\xc7\xd1\xb1\xb9{x:y} /*\xc7\xd1*/"), GTEXT_ENCODING_EUC_KR, GTEXT_CSS_ENCODING_SOURCE_PROTOCOL, "euc-kr");
  add("big5", CSS_BYTES(".\xa4\xa4\xa4\xe5{x:y} /*\xa4\xa4*/"), GTEXT_ENCODING_BIG5, GTEXT_CSS_ENCODING_SOURCE_PROTOCOL, "big5");
  add("iso-2022-jp",
      CSS_BYTES("a{b:c}/*\x1b$BF|K\x5c" "8l\x1b(B*/.\x1b$BF|K\x5c\x1b(B{d:e}\x0a"), GTEXT_ENCODING_ISO_2022_JP,
      GTEXT_CSS_ENCODING_SOURCE_PROTOCOL, "iso-2022-jp");
  add("iso-2022-jp ends in a shift", CSS_BYTES("a{b:c}/*\x1b$BF|\x1b(B*/\x1b$B"), GTEXT_ENCODING_ISO_2022_JP,
      GTEXT_CSS_ENCODING_SOURCE_PROTOCOL, "iso-2022-jp");
  add("replacement", "a{b:c}", GTEXT_ENCODING_REPLACEMENT, GTEXT_CSS_ENCODING_SOURCE_PROTOCOL, "hz-gb-2312");
  add("x-user-defined", CSS_BYTES("a{b:\x80\xff}"), GTEXT_ENCODING_X_USER_DEFINED, GTEXT_CSS_ENCODING_SOURCE_PROTOCOL, "x-user-defined");
  add("windows-1252 e-acute", CSS_BYTES("a{b:\xe9 \x80}"), GTEXT_ENCODING_WINDOWS_1252, GTEXT_CSS_ENCODING_SOURCE_PROTOCOL, "latin1");
  add("unknown protocol falls to charset", CSS_BYTES("@charset \"ISO-8859-5\"; @\xe9"), GTEXT_ENCODING_ISO_8859_5,
      GTEXT_CSS_ENCODING_SOURCE_CHARSET_RULE, "kamoulox");
  add("unknown charset falls to environment", CSS_BYTES("@charset \"kamoulox\"; @\xe9"), GTEXT_ENCODING_ISO_8859_2,
      GTEXT_CSS_ENCODING_SOURCE_ENVIRONMENT, "", GTEXT_ENCODING_ISO_8859_2);
  add("empty", "", GTEXT_ENCODING_UTF_8, GTEXT_CSS_ENCODING_SOURCE_DEFAULT);
  add("only a bom", CSS_BYTES("\xef\xbb\xbf"), GTEXT_ENCODING_UTF_8, GTEXT_CSS_ENCODING_SOURCE_BOM);
  add("half a bom", CSS_BYTES("\xef\xbb"), GTEXT_ENCODING_UTF_8, GTEXT_CSS_ENCODING_SOURCE_DEFAULT);
  add("utf-16le half a bom", CSS_BYTES("\xff"), GTEXT_ENCODING_UTF_8, GTEXT_CSS_ENCODING_SOURCE_DEFAULT);
  // The kilobyte: the declaration whose `";` ends at byte 1024 counts, one byte later it does not.
  add("charset ends at 1024", charset_ending_at(1024) + "\n a{b:\xef\xf0}", GTEXT_ENCODING_WINDOWS_1251,
      GTEXT_CSS_ENCODING_SOURCE_CHARSET_RULE);
  add("charset ends at 1025", charset_ending_at(1025) + "\n a{b:\xef\xf0}", GTEXT_ENCODING_UTF_8, GTEXT_CSS_ENCODING_SOURCE_DEFAULT);
  add("charset ends at 1023", charset_ending_at(1023) + "\n a{b:\xef\xf0}", GTEXT_ENCODING_WINDOWS_1251,
      GTEXT_CSS_ENCODING_SOURCE_CHARSET_RULE);
  {
    std::string big = "@charset \"windows-1251\"; ";
    while (big.size() < 3000) {
      big += ".\xef\xf0{b:\xe8 c}/*\xe2*/\n";
    }
    add("windows-1251 past the window", big, GTEXT_ENCODING_WINDOWS_1251, GTEXT_CSS_ENCODING_SOURCE_CHARSET_RULE);
  }
  {
    std::string big = CSS_BYTES("\xff\xfe");
    for (int i = 0; i < 700; i++) {
      big += std::string("a\x00{\x00" "b\x00:\x00\xe9\x00}\x00", 12);
    }
    add("utf-16le past the window", big, GTEXT_ENCODING_UTF_16LE, GTEXT_CSS_ENCODING_SOURCE_BOM);
  }
  return d;
}

// ---- cut independence ---------------------------------------------------------------------------------------

TEST(CssStreamText, TheAnswerIsTheSameHoweverTheInputIsCut) {
  size_t runs = 0;
  for (const auto & doc : text_documents()) {
    const Outcome whole = whole_text(doc.second);
    for (const Cut & cut : split_cuts(doc.second.size())) {
      const Outcome got = through_stream(GTEXT_CSS_TREE_STYLESHEET, false, nullptr, nullptr, doc.second, cut);
      expect_same_outcome(whole, got, doc.first + " / " + cut.name);
      runs++;
    }
  }
  EXPECT_GT(runs, 900u);
}

TEST(CssStreamText, EveryEntryPointIsCutIndependentToo) {
  struct E {
    GTEXT_CSS_Tree_Kind kind;
    ParseFn fn;
  };
  const E entries[] = {
      {GTEXT_CSS_TREE_STYLESHEET_CONTENTS, gtext_css_parse_stylesheet_contents},
      {GTEXT_CSS_TREE_DECLARATION_LIST, gtext_css_parse_declaration_list},
      {GTEXT_CSS_TREE_BLOCK_CONTENTS, gtext_css_parse_block_contents},
      {GTEXT_CSS_TREE_RULE, gtext_css_parse_rule},
      {GTEXT_CSS_TREE_DECLARATION, gtext_css_parse_declaration},
      {GTEXT_CSS_TREE_COMPONENT_VALUE, gtext_css_parse_component_value},
      {GTEXT_CSS_TREE_COMPONENT_VALUE_LIST, gtext_css_parse_component_values},
      {GTEXT_CSS_TREE_COMMA_LIST, gtext_css_parse_comma_list},
  };
  for (const E & e : entries) {
    for (const auto & doc : text_documents()) {
      const Outcome whole = whole_text(doc.second, e.fn);
      for (size_t at = 0; at <= doc.second.size(); at += 1 + doc.second.size() / 40) {
        Cut c;
        c.sizes = {at};
        const Outcome got = through_stream(e.kind, false, nullptr, nullptr, doc.second, c);
        expect_same_outcome(whole, got, doc.first + " / kind " + std::to_string(e.kind) + " split@" + std::to_string(at));
      }
      const Cut bytes{{1}, true, false, "bytes"};
      expect_same_outcome(whole, through_stream(e.kind, false, nullptr, nullptr, doc.second, bytes),
          doc.first + " / kind " + std::to_string(e.kind) + " bytes");
    }
  }
}

TEST(CssStreamBytes, TheAnswerIsTheSameHoweverTheInputIsCut) {
  size_t runs = 0;
  for (const ByteDoc & doc : byte_documents()) {
    const GTEXT_CSS_Bytes_Options bo = doc.options();
    const Outcome whole = whole_bytes(doc.bytes, &bo);
    ASSERT_EQ(whole.status, GTEXT_CSS_OK) << doc.name;
    EXPECT_EQ(whole.encoding, doc.expect) << doc.name;
    EXPECT_EQ(whole.source, doc.expect_source) << doc.name;
    EXPECT_EQ(whole.input_bytes, doc.bytes.size()) << doc.name;
    for (const Cut & cut : split_cuts(doc.bytes.size())) {
      const Outcome got = through_stream(GTEXT_CSS_TREE_STYLESHEET, true, &bo, nullptr, doc.bytes, cut);
      expect_same_outcome(whole, got, doc.name + " / " + cut.name);
      runs++;
    }
  }
  EXPECT_GT(runs, 4000u);
}

TEST(CssStreamBytes, ARandomCutIsTheSameAsWell) {
  uint64_t state = 987654321;
  for (const ByteDoc & doc : byte_documents()) {
    const GTEXT_CSS_Bytes_Options bo = doc.options();
    const Outcome whole = whole_bytes(doc.bytes, &bo);
    for (int round = 0; round < 25; round++) {
      const Cut c = random_cut(state, round % 2 ? 9 : 400);
      expect_same_outcome(whole, through_stream(GTEXT_CSS_TREE_STYLESHEET, true, &bo, nullptr, doc.bytes, c),
          doc.name + " / random " + std::to_string(round));
    }
  }
}

TEST(CssStreamText, ARandomCutIsTheSameAsWell) {
  uint64_t state = 4242;
  for (const auto & doc : text_documents()) {
    const Outcome whole = whole_text(doc.second);
    for (int round = 0; round < 25; round++) {
      const Cut c = random_cut(state, round % 2 ? 7 : 300);
      expect_same_outcome(whole, through_stream(GTEXT_CSS_TREE_STYLESHEET, false, nullptr, nullptr, doc.second, c),
          doc.first + " / random " + std::to_string(round));
    }
  }
}

// UTF-8 bytes with no mark are text: the bytes entry point and the text one agree on everything but the encoding.
TEST(CssStreamBytes, Utf8BytesAreTheSameAsTextExceptTheEncoding) {
  for (const auto & doc : text_documents()) {
    const Outcome text = whole_text(doc.second);
    const Outcome bytes = whole_bytes(doc.second);
    if (doc.first == "bom-like") {
      continue; // starts with a byte order mark: the bytes entry point skips it, the text entry point does not
    }
    EXPECT_EQ(text.status, bytes.status) << doc.first;
    EXPECT_EQ(text.tree, bytes.tree) << doc.first;
    EXPECT_EQ(text.errors, bytes.errors) << doc.first;
    EXPECT_EQ(text.serialized, bytes.serialized) << doc.first;
    EXPECT_EQ(bytes.encoding, GTEXT_ENCODING_UTF_8) << doc.first;
    EXPECT_EQ(bytes.source, GTEXT_CSS_ENCODING_SOURCE_DEFAULT) << doc.first;
    EXPECT_EQ(text.encoding, GTEXT_ENCODING_NONE) << doc.first;
    EXPECT_EQ(text.source, GTEXT_CSS_ENCODING_SOURCE_NONE) << doc.first;
  }
}

// ---- limits -------------------------------------------------------------------------------------------------

TEST(CssStreamLimits, TheInputLimitIsMetAsTheInputArrives) {
  const std::string css = "a{b:c} d{e:f} g{h:i} \xc3\xa9\xc3\xa9{j:k}";
  for (const size_t limit : {size_t(1), size_t(5), size_t(13), css.size() - 1, css.size()}) {
    GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
    o.max_input_bytes = limit;
    const Outcome whole = whole_text(css, gtext_css_parse_stylesheet, &o);
    for (const Cut & cut : split_cuts(css.size())) {
      GTEXT_CSS_Status fed = GTEXT_CSS_OK;
      const Outcome got = through_stream(GTEXT_CSS_TREE_STYLESHEET, false, nullptr, &o, css, cut, &fed);
      expect_same_outcome(whole, got, "limit " + std::to_string(limit) + " / " + cut.name, whole.status == GTEXT_CSS_OK);
      EXPECT_EQ(fed, whole.status == GTEXT_CSS_OK ? GTEXT_CSS_OK : GTEXT_CSS_E_LIMIT_INPUT) << cut.name;
      if (whole.status != GTEXT_CSS_OK) {
        EXPECT_EQ(got.failure.offset, limit);
        EXPECT_GT(got.failure.line, 0u) << "text has a place";
      }
    }
  }
}

TEST(CssStreamLimits, TheInputLimitOfBytesIsTheBytesNotTheText) {
  // 40 bytes of windows-1251 that are 80 bytes of UTF-8 text: the limit is on the bytes.
  const std::string bytes = "@charset \"windows-1251\"; a{b:\"" + std::string(10, '\xef') + "\"}";
  GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
  o.max_input_bytes = bytes.size();
  const Outcome ok = whole_bytes(bytes, nullptr, &o);
  EXPECT_EQ(ok.status, GTEXT_CSS_OK);
  o.max_input_bytes = bytes.size() - 1;
  const Outcome over = whole_bytes(bytes, nullptr, &o);
  EXPECT_EQ(over.status, GTEXT_CSS_E_LIMIT_INPUT);
  EXPECT_EQ(over.failure.offset, bytes.size() - 1);
  EXPECT_EQ(over.failure.line, 0u) << "nothing was decoded, so there is no line";
  EXPECT_EQ(over.failure.column, 0u);
  EXPECT_EQ(over.input_bytes, bytes.size());
  for (const Cut & cut : split_cuts(bytes.size())) {
    GTEXT_CSS_Status fed = GTEXT_CSS_OK;
    const Outcome got = through_stream(GTEXT_CSS_TREE_STYLESHEET, true, nullptr, &o, bytes, cut, &fed);
    EXPECT_EQ(got.status, GTEXT_CSS_E_LIMIT_INPUT) << cut.name;
    EXPECT_EQ(fed, GTEXT_CSS_E_LIMIT_INPUT) << cut.name;
    EXPECT_EQ(got.failure.offset, over.failure.offset) << cut.name;
    EXPECT_EQ(got.failure.line, 0u) << cut.name;
    EXPECT_GE(got.input_bytes, over.failure.offset + 1) << cut.name;
    EXPECT_LE(got.input_bytes, bytes.size()) << cut.name;
  }
}

TEST(CssStreamLimits, EveryOtherLimitIsTheSameAtEveryCut) {
  const std::string css = "a{b:c} d{e:{f(g[h])}} i{j:k} @m{n{o:p}} q{r:s}";
  struct L {
    const char * name;
    void (*set)(GTEXT_CSS_Parse_Options *);
  };
  const L limits[] = {
      {"tokens", [](GTEXT_CSS_Parse_Options * o) { o->max_tokens = 20; }},
      {"depth", [](GTEXT_CSS_Parse_Options * o) { o->max_depth = 3; }},
      {"rules", [](GTEXT_CSS_Parse_Options * o) { o->max_rules = 3; }},
      {"declarations", [](GTEXT_CSS_Parse_Options * o) { o->max_declarations = 3; }},
      {"errors", [](GTEXT_CSS_Parse_Options * o) { o->max_total_errors = 1; }},
  };
  for (const L & l : limits) {
    GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
    l.set(&o);
    const std::string input = std::string(l.name) == "errors" ? "a{b:'x\n} c{d:'y\n} e{f:'z\n}" : css;
    const Outcome whole = whole_text(input, gtext_css_parse_stylesheet, &o);
    EXPECT_NE(whole.status, GTEXT_CSS_OK) << l.name;
    for (const Cut & cut : split_cuts(input.size())) {
      expect_same_outcome(whole, through_stream(GTEXT_CSS_TREE_STYLESHEET, false, nullptr, &o, input, cut), std::string(l.name) + " / " + cut.name);
      const Outcome wb = whole_bytes(input, nullptr, &o);
      expect_same_outcome(wb, through_stream(GTEXT_CSS_TREE_STYLESHEET, true, nullptr, &o, input, cut), std::string(l.name) + " bytes / " + cut.name);
    }
  }
}

// ---- the stream's states --------------------------------------------------------------------------------------

TEST(CssStreamState, ConstructionAndTheOrderOfCalls) {
  GTEXT_CSS_Stream * s = reinterpret_cast<GTEXT_CSS_Stream *>(0x1);
  EXPECT_EQ(gtext_css_stream_new(GTEXT_CSS_TREE_STYLESHEET, nullptr, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_stream_new_bytes(GTEXT_CSS_TREE_STYLESHEET, nullptr, nullptr, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_stream_new(static_cast<GTEXT_CSS_Tree_Kind>(99), nullptr, &s), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(s, nullptr) << "the output is set to NULL on failure";
  s = nullptr;
  ASSERT_EQ(gtext_css_stream_new(GTEXT_CSS_TREE_STYLESHEET, nullptr, &s), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_stream_feed(nullptr, "a", 1), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_stream_feed(s, nullptr, 3), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_stream_feed(s, nullptr, 0), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_stream_feed(s, "a{b:c}", 6), GTEXT_CSS_OK);
  GTEXT_CSS_Tree * t = reinterpret_cast<GTEXT_CSS_Tree *>(0x1);
  EXPECT_EQ(gtext_css_stream_finish(s, nullptr, nullptr), GTEXT_CSS_E_INVALID) << "a NULL output is refused";
  GTEXT_CSS_Result r{};
  EXPECT_EQ(gtext_css_stream_finish(nullptr, &t, &r), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(r.status, GTEXT_CSS_E_INVALID);
  t = nullptr;
  // (the refused finish above did not end the stream)
  ASSERT_EQ(gtext_css_stream_finish(s, &t, &r), GTEXT_CSS_OK);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->rule_count, 1u);
  EXPECT_EQ(r.input_bytes, 6u);
  gtext_css_tree_free(t);
  gtext_css_result_free(&r);
  EXPECT_EQ(gtext_css_stream_feed(s, "x", 1), GTEXT_CSS_E_INVALID) << "feeding after finish";
  GTEXT_CSS_Tree * t2 = nullptr;
  EXPECT_EQ(gtext_css_stream_finish(s, &t2, &r), GTEXT_CSS_E_INVALID) << "finishing twice";
  EXPECT_EQ(t2, nullptr);
  gtext_css_result_free(&r);
  gtext_css_stream_free(s);
  gtext_css_stream_free(nullptr);
}

TEST(CssStreamState, AFreedStreamWithoutAFinishLeaksNothing) {
  FaultAllocator fa;
  GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
  o.allocator = &fa.alloc;
  o.location = "somewhere";
  GTEXT_CSS_Bytes_Options b = gtext_css_bytes_options_default();
  b.protocol_label = "windows-1251";
  for (int mode = 0; mode < 3; mode++) {
    GTEXT_CSS_Stream * s = nullptr;
    ASSERT_EQ(mode == 0 ? gtext_css_stream_new(GTEXT_CSS_TREE_STYLESHEET, &o, &s)
                        : gtext_css_stream_new_bytes(GTEXT_CSS_TREE_STYLESHEET, &b, &o, &s),
        GTEXT_CSS_OK);
    const std::string big(mode == 2 ? 3000 : 20, 'a');
    ASSERT_EQ(gtext_css_stream_feed(s, big.data(), big.size()), GTEXT_CSS_OK);
    gtext_css_stream_free(s);
    EXPECT_TRUE(fa.live.empty()) << mode;
    EXPECT_EQ(fa.bad_frees, 0u);
  }
}

TEST(CssStreamState, ALimitIsEveryLaterCallsAnswerToo) {
  GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
  o.max_input_bytes = 4;
  GTEXT_CSS_Stream * s = nullptr;
  ASSERT_EQ(gtext_css_stream_new(GTEXT_CSS_TREE_STYLESHEET, &o, &s), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_stream_feed(s, "a{b", 3), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_stream_feed(s, ":c}", 3), GTEXT_CSS_E_LIMIT_INPUT);
  EXPECT_EQ(gtext_css_stream_feed(s, "", 0), GTEXT_CSS_E_LIMIT_INPUT);
  EXPECT_EQ(gtext_css_stream_feed(s, "x", 1), GTEXT_CSS_E_LIMIT_INPUT);
  GTEXT_CSS_Tree * t = nullptr;
  GTEXT_CSS_Result r{};
  EXPECT_EQ(gtext_css_stream_finish(s, &t, &r), GTEXT_CSS_E_LIMIT_INPUT);
  EXPECT_EQ(t, nullptr);
  EXPECT_EQ(r.status, GTEXT_CSS_E_LIMIT_INPUT);
  EXPECT_EQ(r.failure.offset, 4u);
  gtext_css_result_free(&r);
  EXPECT_EQ(gtext_css_stream_finish(s, &t, &r), GTEXT_CSS_E_LIMIT_INPUT) << "a failed stream says so again";
  gtext_css_result_free(&r);
  gtext_css_stream_free(s);
}

TEST(CssStreamState, TheLocationAndTheOptionsAreCopied) {
  std::string loc = "https://example.test/a.css";
  GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
  o.location = loc.c_str();
  o.max_depth = 2;
  GTEXT_CSS_Bytes_Options b = gtext_css_bytes_options_default();
  std::string label = "windows-1251";
  b.protocol_label = label.c_str();
  GTEXT_CSS_Stream * s = nullptr;
  ASSERT_EQ(gtext_css_stream_new_bytes(GTEXT_CSS_TREE_STYLESHEET, &b, &o, &s), GTEXT_CSS_OK);
  loc.assign(loc.size(), 'x');
  label.assign(label.size(), 'x');
  const std::string css = CSS_BYTES("a{b:\"\xef\"}");
  ASSERT_EQ(gtext_css_stream_feed(s, css.data(), css.size()), GTEXT_CSS_OK);
  GTEXT_CSS_Tree * t = nullptr;
  GTEXT_CSS_Result r{};
  ASSERT_EQ(gtext_css_stream_finish(s, &t, &r), GTEXT_CSS_OK);
  EXPECT_STREQ(t->location, "https://example.test/a.css");
  EXPECT_EQ(r.encoding, GTEXT_ENCODING_WINDOWS_1251);
  gtext_css_tree_free(t);
  gtext_css_result_free(&r);
  gtext_css_stream_free(s);
}

// ---- what is kept, and what feeding costs -------------------------------------------------------------------

TEST(CssStreamMemory, WhatIsKeptIsBoundedAndTheNumberOfCallsCostsNothing) {
  std::string css;
  while (css.size() < 100000) {
    css += "a.b > c { d: e; f: \"g\" }\n";
  }
  for (const char * label : {"", "windows-1251", "utf-16le"}) {
    FaultAllocator fa;
    GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
    o.allocator = &fa.alloc;
    GTEXT_CSS_Bytes_Options b = gtext_css_bytes_options_default();
    b.protocol_label = *label ? label : nullptr;
    GTEXT_CSS_Stream * s = nullptr;
    ASSERT_EQ(gtext_css_stream_new_bytes(GTEXT_CSS_TREE_STYLESHEET, &b, &o, &s), GTEXT_CSS_OK);
    fa.arm(0);
    size_t most_kept = 0;
    for (size_t i = 0; i < css.size(); i++) {
      ASSERT_EQ(gtext_css_stream_feed(s, css.data() + i, 1), GTEXT_CSS_OK);
      size_t alloc = 0;
      const size_t kept = gtext_css_stream_kept_bytes(s, &alloc);
      most_kept = std::max(most_kept, kept);
      if (i % 5000 == 0) {
        EXPECT_LE(kept, 3 * (i + 1)) << "the text of one byte is at most three";
      }
    }
    // 100,000 calls, and the allocator was asked for a few dozen times: the buffers double.
    EXPECT_LT(fa.calls, 100u) << label;
    EXPECT_GE(fa.calls, 1u);
    EXPECT_LE(most_kept, 3 * css.size());
    size_t allocated = 0;
    (void)gtext_css_stream_kept_bytes(s, &allocated);
    EXPECT_LE(allocated, 8 * css.size() + 8192) << "the buffers are no more than a small multiple of what is kept: " << label;
    gtext_css_stream_free(s);
    EXPECT_TRUE(fa.live.empty());
  }
}

TEST(CssStreamMemory, FeedingAByteAtATimeCostsTheSameWorkAsOnePiece) {
  std::string css;
  while (css.size() < 60000) {
    css += "a.b > c { d: e; f: \"\xc3\xa9\" }\n";
  }
  for (const char * label : {"", "windows-1251"}) {
    GTEXT_CSS_Bytes_Options b = gtext_css_bytes_options_default();
    b.protocol_label = *label ? label : nullptr;
    size_t one = 0, bytewise = 0, pieces = 0;
    ASSERT_EQ(gtext_css_stream_work(GTEXT_CSS_TREE_STYLESHEET, css.data(), css.size(), 0, &b, nullptr, &one), GTEXT_CSS_OK);
    ASSERT_EQ(gtext_css_stream_work(GTEXT_CSS_TREE_STYLESHEET, css.data(), css.size(), 1, &b, nullptr, &bytewise), GTEXT_CSS_OK);
    ASSERT_EQ(gtext_css_stream_work(GTEXT_CSS_TREE_STYLESHEET, css.data(), css.size(), 4096, &b, nullptr, &pieces), GTEXT_CSS_OK);
    // A call costs one unit, and a buffer that grows by doubling copies about as many bytes as it holds: both are
    // in proportion to the bytes, so a byte at a time is a small multiple of one piece and not a power of it.
    EXPECT_LT(bytewise, one + 4 * css.size() + 1024) << label << ": a call costs a constant, however many";
    EXPECT_LT(pieces, one + 2 * css.size() + 1024) << label;
    EXPECT_GT(one, css.size() / 2) << "the counter counts something";
  }
}

} // namespace
