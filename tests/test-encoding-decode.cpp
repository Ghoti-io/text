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
 * The decoders: UTF-8 and UTF-16 against reference decoders written another way, over every sequence of one to three
 * bytes; every byte of every single-byte encoding against the standard's indexes; `replacement` and `x-user-defined`;
 * the byte order mark options; fatal mode with its offsets; the output limit at every value; a sink that refuses; the
 * argument checks; and **chunk independence** - the same output however the input is cut, for every way of cutting
 * short inputs and for byte-at-a-time feeds of long ones.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "encoding_test_support.h"

#include <functional>
#include <set>

using namespace enctest;

namespace {

// A decode of `in` as one call with flush, reusing a decoder and a sink, for the exhaustive loops.
struct Fast {
  GTEXT_Decoder * d = nullptr;
  Collect c;
  GTEXT_Encoding_Sink sink = sink_of(c);
  explicit Fast(GTEXT_Encoding e, GTEXT_Decoder_Error_Mode mode = GTEXT_DECODER_REPLACEMENT) {
    GTEXT_Decoder_Options o = decoder_options(mode);
    EXPECT_EQ(gtext_decoder_new(e, &o, &d), GTEXT_ENCODING_OK);
  }
  ~Fast() {
    gtext_decoder_free(d);
  }
  GTEXT_Encoding_Status run(const unsigned char * p, size_t n, GTEXT_Decoder_Result * r) {
    c.bytes.clear();
    return gtext_decoder_decode(d, p, n, true, &sink, r);
  }
};

std::string cps_utf8(std::initializer_list<uint32_t> cps) {
  std::string s;
  for (uint32_t cp : cps) {
    s += utf8_of(cp);
  }
  return s;
}

} // namespace

// ---- UTF-8 ----------------------------------------------------------------------------------------------------

TEST(EncodingDecodeUtf8, KnownSequences) {
  struct Case {
    std::string in;
    std::string out;
  };
  const Case cases[] = {
      {"", ""},
      {"abc", "abc"},
      {bytes_of({0xC3, 0xA9}), bytes_of({0xC3, 0xA9})},
      {bytes_of({0xE2, 0x82, 0xAC}), bytes_of({0xE2, 0x82, 0xAC})},
      {bytes_of({0xF0, 0x9F, 0x92, 0xA9}), bytes_of({0xF0, 0x9F, 0x92, 0xA9})},
      {bytes_of({0x00}), bytes_of({0x00})},
      // The matrix: E0 80 80 is three errors (E0 wants A0..BF), a truncated tail is one.
      {bytes_of({0xE0, 0x80, 0x80}), cps_utf8({0xFFFD, 0xFFFD, 0xFFFD})},
      {bytes_of({'a', 0xF0, 0x9F}), "a" + cps_utf8({0xFFFD})},
      {bytes_of({0xE2, 0x82}), cps_utf8({0xFFFD})},
      {bytes_of({0xE2}), cps_utf8({0xFFFD})},
      // Unicode's own example of the maximal subpart rule (Table 3-8 and the text of 3.9.6).
      {bytes_of({0x61, 0xF1, 0x80, 0x80, 0xE1, 0x80, 0xC2, 0x62, 0x80, 0x63, 0x80, 0xBF, 0x64}),
          "a" + cps_utf8({0xFFFD, 0xFFFD, 0xFFFD}) + "b" + cps_utf8({0xFFFD}) + "c" + cps_utf8({0xFFFD, 0xFFFD}) + "d"},
      {bytes_of({0xC0, 0xAF, 0xE0, 0x80, 0xBF, 0xF0, 0x81, 0x82, 0x41}),
          cps_utf8({0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0x41})},
      // Surrogates, and above U+10FFFF, and the bytes that never occur.
      {bytes_of({0xED, 0xA0, 0x80}), cps_utf8({0xFFFD, 0xFFFD, 0xFFFD})},
      {bytes_of({0xED, 0xBF, 0xBF}), cps_utf8({0xFFFD, 0xFFFD, 0xFFFD})},
      {bytes_of({0xED, 0x9F, 0xBF}), bytes_of({0xED, 0x9F, 0xBF})},
      {bytes_of({0xF4, 0x90, 0x80, 0x80}), cps_utf8({0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD})},
      {bytes_of({0xF4, 0x8F, 0xBF, 0xBF}), bytes_of({0xF4, 0x8F, 0xBF, 0xBF})},
      {bytes_of({0xF5, 0x80, 0x80, 0x80}), cps_utf8({0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD})},
      {bytes_of({0xFF}), cps_utf8({0xFFFD})},
      {bytes_of({0xFE, 0xFF}), cps_utf8({0xFFFD, 0xFFFD})},
      {bytes_of({0xC1, 0xBF}), cps_utf8({0xFFFD, 0xFFFD})},
      {bytes_of({0x80}), cps_utf8({0xFFFD})},
      {bytes_of({0xBF, 0x80}), cps_utf8({0xFFFD, 0xFFFD})},
      // An ASCII byte is never swallowed by an error.
      {bytes_of({0xE2, 0x82, 0x41}), cps_utf8({0xFFFD}) + "A"},
      {bytes_of({0xF0, 0x9F, 0x41}), cps_utf8({0xFFFD}) + "A"},
      {bytes_of({0xC3, 0x00}), cps_utf8({0xFFFD}) + std::string(1, '\0')},
      // WPT: the ones from api-surrogates-utf8 and textdecoder-fatal.
      {bytes_of({0xFC, 0x80, 0x80, 0x80, 0x80, 0x80}), cps_utf8({0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD})},
      {bytes_of({0xF8, 0x80, 0x80, 0x80, 0x80}), cps_utf8({0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD})},
      {bytes_of({0x00, 0xFE, 0xFF}), std::string(1, '\0') + cps_utf8({0xFFFD, 0xFFFD})},
      {bytes_of({0xF0, 0x90, 0x80}), cps_utf8({0xFFFD})},
      {bytes_of({0xF0, 0x80, 0x80}), cps_utf8({0xFFFD, 0xFFFD, 0xFFFD})},
  };
  for (const Case & c : cases) {
    const Out o = decode_whole(GTEXT_ENCODING_UTF_8, c.in);
    EXPECT_EQ(o.status, GTEXT_ENCODING_OK);
    EXPECT_EQ(o.bytes, c.out) << show(c.in);
    EXPECT_EQ(decode_bytewise(GTEXT_ENCODING_UTF_8, gtext_decoder_options_default(), c.in).bytes, c.out) << show(c.in);
  }
}

TEST(EncodingDecodeUtf8, EverySequenceOfOneToThreeBytesAgainstTheReference) {
  Fast f(GTEXT_ENCODING_UTF_8);
  Fast fatal(GTEXT_ENCODING_UTF_8, GTEXT_DECODER_FATAL);
  size_t count = 0;
  size_t ill_formed = 0;
  unsigned char buf[3];
  for (int n = 1; n <= 3; n++) {
    const uint32_t total = 1u << (8 * n);
    for (uint32_t v = 0; v < total; v++) {
      for (int k = 0; k < n; k++) {
        buf[k] = (unsigned char)(v >> (8 * (n - 1 - k)));
      }
      GTEXT_Decoder_Result r;
      ASSERT_EQ(f.run(buf, n, &r), GTEXT_ENCODING_OK);
      const RefResult ref = ref_utf8(std::string((const char *)buf, n));
      if (f.c.bytes != utf8_of_cps(ref.cps) || r.replacements != ref.replacements) {
        FAIL() << "input " << show(std::string((const char *)buf, n)) << "got [" << show(f.c.bytes) << "] with "
               << r.replacements << " replacements, the reference [" << show(utf8_of_cps(ref.cps)) << "] with "
               << ref.replacements;
      }
      // Fatal mode fails exactly when replacement mode replaced something.
      GTEXT_Decoder_Result fr;
      const GTEXT_Encoding_Status s = fatal.run(buf, n, &fr);
      if ((s == GTEXT_ENCODING_E_MALFORMED) != (ref.replacements > 0) || (s != GTEXT_ENCODING_OK && s != GTEXT_ENCODING_E_MALFORMED)) {
        FAIL() << "fatal mode on " << show(std::string((const char *)buf, n));
      }
      if (s == GTEXT_ENCODING_E_MALFORMED) {
        ill_formed++;
        ASSERT_LE(fr.error_offset, size_t(n));
      }
      count++;
    }
  }
  EXPECT_EQ(count, 256u + 65536u + 16777216u);
  EXPECT_GT(ill_formed, 14000000u);
}

TEST(EncodingDecodeUtf8, EveryScalarValueRoundTripsAndNothingElseIsAccepted) {
  Fast f(GTEXT_ENCODING_UTF_8, GTEXT_DECODER_FATAL);
  size_t scalars = 0;
  for (uint32_t cp = 0; cp <= 0x10FFFF; cp++) {
    const std::string in = utf8_of(cp);
    GTEXT_Decoder_Result r;
    const GTEXT_Encoding_Status s = f.run((const unsigned char *)in.data(), in.size(), &r);
    if (cp >= 0xD800 && cp <= 0xDFFF) {
      ASSERT_EQ(s, GTEXT_ENCODING_E_MALFORMED) << std::hex << cp; // an encoded surrogate
      continue;
    }
    ASSERT_EQ(s, GTEXT_ENCODING_OK) << std::hex << cp;
    ASSERT_EQ(f.c.bytes, in) << std::hex << cp;
    scalars++;
  }
  EXPECT_EQ(scalars, 0x110000u - 0x800u);
  // The first code points that are not: one past the last, and 5- and 6-byte forms.
  for (const std::string & in : {bytes_of({0xF4, 0x90, 0x80, 0x80}), bytes_of({0xF8, 0x88, 0x80, 0x80, 0x80}),
           bytes_of({0xFC, 0x84, 0x80, 0x80, 0x80, 0x80})}) {
    GTEXT_Decoder_Result r;
    EXPECT_EQ(f.run((const unsigned char *)in.data(), in.size(), &r), GTEXT_ENCODING_E_MALFORMED);
  }
}

// ---- UTF-16 ---------------------------------------------------------------------------------------------------

TEST(EncodingDecodeUtf16, KnownSequences) {
  // LE: a pair, split across the two bytes of a unit and between units.
  const std::string pair = bytes_of({0x3D, 0xD8, 0x29, 0xDC});
  for (size_t cut = 0; cut <= pair.size(); cut++) {
    EXPECT_EQ(decode_cut(GTEXT_ENCODING_UTF_16LE, gtext_decoder_options_default(), pair, cut ? std::vector<size_t>{cut} : std::vector<size_t>{}).bytes,
        cps_utf8({0x1F429}));
  }
  // BE: the same pair.
  const std::string pair_be = bytes_of({0xD8, 0x3D, 0xDC, 0x29});
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_UTF_16BE, pair_be).bytes, cps_utf8({0x1F429}));
  EXPECT_EQ(decode_bytewise(GTEXT_ENCODING_UTF_16BE, gtext_decoder_options_default(), pair_be).bytes, cps_utf8({0x1F429}));
  // A lone leading surrogate, at the end and followed by a unit; a lone trailing one.
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_UTF_16LE, bytes_of({0x3D, 0xD8})).bytes, cps_utf8({0xFFFD}));
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_UTF_16LE, bytes_of({0x3D, 0xD8, 0x41, 0x00})).bytes, cps_utf8({0xFFFD}) + "A");
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_UTF_16LE, bytes_of({0x29, 0xDC, 0x41, 0x00})).bytes, cps_utf8({0xFFFD}) + "A");
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_UTF_16LE, bytes_of({0x3D, 0xD8, 0x3D, 0xD8, 0x29, 0xDC})).bytes,
      cps_utf8({0xFFFD, 0x1F429}));
  // A truncated unit: WPT's "truncated code unit".
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_UTF_16LE, bytes_of({0x00})).bytes, cps_utf8({0xFFFD}));
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_UTF_16LE, bytes_of({0x41, 0x00, 0x42})).bytes, "A" + cps_utf8({0xFFFD}));
  // One error at the end covers a leading surrogate and the byte after it.
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_UTF_16LE, bytes_of({0x3D, 0xD8, 0x42})).bytes, cps_utf8({0xFFFD}));
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_UTF_16BE, bytes_of({0xD8, 0x3D, 0x42})).bytes, cps_utf8({0xFFFD}));
  // WPT's textdecoder-utf16-surrogates cases.
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_UTF_16LE, bytes_of({0x61, 0x00, 0x62, 0x00, 0x00, 0xD8, 0x77, 0x00, 0x78, 0x00})).bytes,
      "ab" + cps_utf8({0xFFFD}) + "wx");
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_UTF_16LE, bytes_of({0x00, 0xD8, 0x00, 0xD8})).bytes, cps_utf8({0xFFFD, 0xFFFD}));
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_UTF_16LE, bytes_of({0xFF, 0xDF, 0x00, 0xD8})).bytes, cps_utf8({0xFFFD, 0xFFFD}));
  // The U+FFFE and U+FFFF code units are ordinary code points.
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_UTF_16LE, bytes_of({0xFE, 0xFF, 0xFF, 0xFF})).bytes, cps_utf8({0xFFFE, 0xFFFF}));
}

TEST(EncodingDecodeUtf16, EverySequenceOfOneToThreeBytesAgainstTheReference) {
  for (int big = 0; big < 2; big++) {
    Fast f(big ? GTEXT_ENCODING_UTF_16BE : GTEXT_ENCODING_UTF_16LE);
    Fast fatal(big ? GTEXT_ENCODING_UTF_16BE : GTEXT_ENCODING_UTF_16LE, GTEXT_DECODER_FATAL);
    unsigned char buf[3];
    for (int n = 1; n <= 3; n++) {
      const uint32_t total = 1u << (8 * n);
      for (uint32_t v = 0; v < total; v++) {
        for (int k = 0; k < n; k++) {
          buf[k] = (unsigned char)(v >> (8 * (n - 1 - k)));
        }
        GTEXT_Decoder_Result r;
        ASSERT_EQ(f.run(buf, n, &r), GTEXT_ENCODING_OK);
        const RefResult ref = ref_utf16(std::string((const char *)buf, n), big);
        if (f.c.bytes != utf8_of_cps(ref.cps) || r.replacements != ref.replacements) {
          FAIL() << (big ? "BE " : "LE ") << show(std::string((const char *)buf, n)) << "got [" << show(f.c.bytes)
                 << "] the reference [" << show(utf8_of_cps(ref.cps)) << "]";
        }
        GTEXT_Decoder_Result fr;
        const GTEXT_Encoding_Status s = fatal.run(buf, n, &fr);
        ASSERT_EQ(s == GTEXT_ENCODING_E_MALFORMED, ref.replacements > 0) << show(std::string((const char *)buf, n));
      }
    }
  }
}

TEST(EncodingDecodeUtf16, EveryValidUnitAndPair) {
  Fast le(GTEXT_ENCODING_UTF_16LE, GTEXT_DECODER_FATAL);
  Fast be(GTEXT_ENCODING_UTF_16BE, GTEXT_DECODER_FATAL);
  size_t n = 0;
  for (uint32_t cp = 0; cp <= 0x10FFFF; cp++) {
    if (cp >= 0xD800 && cp <= 0xDFFF) {
      continue;
    }
    unsigned char l[4], b[4];
    size_t len;
    if (cp < 0x10000) {
      l[0] = cp & 0xFF; l[1] = cp >> 8; b[0] = l[1]; b[1] = l[0]; len = 2;
    } else {
      const uint32_t v = cp - 0x10000;
      const uint32_t hi = 0xD800 + (v >> 10), lo = 0xDC00 + (v & 0x3FF);
      l[0] = hi & 0xFF; l[1] = hi >> 8; l[2] = lo & 0xFF; l[3] = lo >> 8;
      b[0] = l[1]; b[1] = l[0]; b[2] = l[3]; b[3] = l[2]; len = 4;
    }
    GTEXT_Decoder_Result r;
    ASSERT_EQ(le.run(l, len, &r), GTEXT_ENCODING_OK);
    ASSERT_EQ(le.c.bytes, utf8_of(cp)) << std::hex << cp;
    ASSERT_EQ(be.run(b, len, &r), GTEXT_ENCODING_OK);
    ASSERT_EQ(be.c.bytes, utf8_of(cp)) << std::hex << cp;
    n++;
  }
  EXPECT_EQ(n, 0x110000u - 0x800u);
}

// ---- the single-byte encodings, replacement and x-user-defined -----------------------------------------

TEST(EncodingDecodeSingleByte, EveryByteOfEveryEncodingAgainstTheStandardsIndexes) {
  const auto indexes = load_single_byte_indexes();
  ASSERT_EQ(indexes.size(), 27u);
  const auto encodings = single_byte_encodings();
  ASSERT_EQ(encodings.size(), 28u);
  size_t bytes_checked = 0, unmapped = 0;
  for (GTEXT_Encoding e : encodings) {
    ASSERT_TRUE(indexes.count(index_name(e))) << gtext_encoding_name(e);
    const auto & index = indexes.at(index_name(e));
    std::string all;
    std::string want;
    for (int b = 0; b < 256; b++) {
      all += char(b);
      const int cp = b < 0x80 ? b : index[b - 0x80];
      want += utf8_of(cp < 0 ? 0xFFFD : uint32_t(cp));
      // One byte at a time, in both error modes.
      const Out r = decode_whole(e, std::string(1, char(b)));
      EXPECT_EQ(r.bytes, utf8_of(cp < 0 ? 0xFFFD : uint32_t(cp))) << gtext_encoding_name(e) << " " << b;
      EXPECT_EQ(r.replacements, cp < 0 ? 1u : 0u);
      const Out f = decode_whole(e, decoder_options(GTEXT_DECODER_FATAL), std::string(1, char(b)));
      EXPECT_EQ(f.status, cp < 0 ? GTEXT_ENCODING_E_MALFORMED : GTEXT_ENCODING_OK) << gtext_encoding_name(e) << " " << b;
      if (cp < 0) {
        EXPECT_EQ(f.error_offset, 0u);
        unmapped++;
      }
      bytes_checked++;
    }
    EXPECT_EQ(decode_whole(e, all).bytes, want) << gtext_encoding_name(e);
    EXPECT_EQ(decode_bytewise(e, gtext_decoder_options_default(), all).bytes, want) << gtext_encoding_name(e);
    // ASCII is ASCII in all of them, and an error in the middle does not disturb the rest.
    EXPECT_EQ(decode_whole(e, "plain text 123").bytes, "plain text 123");
  }
  EXPECT_EQ(bytes_checked, 28u * 256u);
  // Bytes the indexes leave without a code point: 8859-3 7, 8859-6 45, 8859-7 3, 8859-8 36 (and 8859-8-I the same 36),
  // windows-874 8, 1253 3, 1255 10, 1257 2.
  EXPECT_EQ(unmapped, 7u + 45u + 3u + 36u + 36u + 8u + 3u + 10u + 2u);
}

TEST(EncodingDecodeSingleByte, TheOtherNonUnicodeEncodings) {
  // replacement: one U+FFFD for the whole stream, none for an empty one.
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_REPLACEMENT, "").bytes, "");
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_REPLACEMENT, "a").bytes, cps_utf8({0xFFFD}));
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_REPLACEMENT, std::string(100000, 'x')).bytes, cps_utf8({0xFFFD}));
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_REPLACEMENT, std::string(100000, 'x')).replacements, 1u);
  EXPECT_EQ(decode_bytewise(GTEXT_ENCODING_REPLACEMENT, gtext_decoder_options_default(), std::string(1000, 'x')).bytes,
      cps_utf8({0xFFFD}));
  // ... even when the first chunk is empty, and across flushes (each stream gets its own).
  EXPECT_EQ(decode_cut(GTEXT_ENCODING_REPLACEMENT, gtext_decoder_options_default(), "abc", {0, 1}).bytes, cps_utf8({0xFFFD}));
  {
    GTEXT_Decoder * d = nullptr;
    ASSERT_EQ(gtext_decoder_new(GTEXT_ENCODING_REPLACEMENT, nullptr, &d), GTEXT_ENCODING_OK);
    Collect c;
    GTEXT_Encoding_Sink sink = sink_of(c);
    EXPECT_EQ(gtext_decoder_decode(d, "ab", 2, true, &sink, nullptr), GTEXT_ENCODING_OK);
    EXPECT_EQ(gtext_decoder_decode(d, "cd", 2, true, &sink, nullptr), GTEXT_ENCODING_OK);
    EXPECT_EQ(c.bytes, cps_utf8({0xFFFD, 0xFFFD}));
    gtext_decoder_free(d);
  }
  // Fatal mode: the first byte is the error.
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_REPLACEMENT, decoder_options(GTEXT_DECODER_FATAL), "a").status,
      GTEXT_ENCODING_E_MALFORMED);
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_REPLACEMENT, decoder_options(GTEXT_DECODER_FATAL), "").status, GTEXT_ENCODING_OK);
  // x-user-defined: 0x80..0xFF are U+F780..U+F7FF.
  std::string all, want;
  for (int b = 0; b < 256; b++) {
    all += char(b);
    want += utf8_of(b < 0x80 ? uint32_t(b) : 0xF780u + uint32_t(b) - 0x80u);
  }
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_X_USER_DEFINED, all).bytes, want);
  EXPECT_EQ(decode_bytewise(GTEXT_ENCODING_X_USER_DEFINED, gtext_decoder_options_default(), all).bytes, want);
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_X_USER_DEFINED, decoder_options(GTEXT_DECODER_FATAL), all).status, GTEXT_ENCODING_OK);
}

// ---- byte order marks -----------------------------------------------------------------------------------------------

TEST(EncodingDecodeBom, TheThreeVariantsOfTheStandard) {
  const std::string bom8 = bytes_of({0xEF, 0xBB, 0xBF});
  const std::string text = "a";
  GTEXT_Decoder_Options none = gtext_decoder_options_default(); // "UTF-8 decode without BOM"
  GTEXT_Decoder_Options removal = none; // "UTF-8 decode"
  removal.bom_removal = true;
  GTEXT_Decoder_Options sniff = none; // "decode"
  sniff.bom_sniffing = true;
  GTEXT_Decoder_Options both = sniff;
  both.bom_removal = true;

  EXPECT_EQ(decode_whole(GTEXT_ENCODING_UTF_8, none, bom8 + text).bytes, bom8 + text);
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_UTF_8, removal, bom8 + text).bytes, text);
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_UTF_8, sniff, bom8 + text).bytes, text);
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_UTF_8, both, bom8 + text).bytes, text);
  // Only one mark is removed, by every variant that removes one.
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_UTF_8, removal, bom8 + bom8 + text).bytes, bom8 + text);
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_UTF_8, sniff, bom8 + bom8 + text).bytes, bom8 + text);
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_UTF_8, both, bom8 + bom8 + text).bytes, bom8 + text);
  // A mark that is not first is text.
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_UTF_8, removal, text + bom8).bytes, text + bom8);
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_UTF_8, sniff, text + bom8).bytes, text + bom8);
  // Removal looks at the first thing written: after an error it is too late.
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_UTF_8, removal, std::string(1, char(0xFF)) + bom8).bytes,
      cps_utf8({0xFFFD, 0xFEFF}));
  // Removal is for the three Unicode encodings only.
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_WINDOWS_1252, removal, bom8).bytes, cps_utf8({0xEF, 0xBB, 0xBF}));
  // Sniffing chooses the encoding whatever was asked for, and says so.
  {
    const Out o = decode_whole(GTEXT_ENCODING_WINDOWS_1252, sniff, bom8 + "\xC3\xA9");
    EXPECT_EQ(o.bytes, cps_utf8({0xE9}));
    EXPECT_EQ(o.encoding, GTEXT_ENCODING_UTF_8);
    EXPECT_TRUE(o.bom_found);
  }
  {
    const Out o = decode_whole(GTEXT_ENCODING_UTF_8, sniff, bytes_of({0xFF, 0xFE, 0x41, 0x00, 0x3D, 0xD8, 0x29, 0xDC}));
    EXPECT_EQ(o.bytes, "A" + cps_utf8({0x1F429}));
    EXPECT_EQ(o.encoding, GTEXT_ENCODING_UTF_16LE);
    EXPECT_TRUE(o.bom_found);
  }
  {
    const Out o = decode_whole(GTEXT_ENCODING_WINDOWS_1252, sniff, bytes_of({0xFE, 0xFF, 0x00, 0x41}));
    EXPECT_EQ(o.bytes, "A");
    EXPECT_EQ(o.encoding, GTEXT_ENCODING_UTF_16BE);
  }
  // No mark: the asked-for encoding, and the bytes that looked like the start of one are ordinary input.
  for (const std::string & in : {bytes_of({0xEF}), bytes_of({0xEF, 0xBB}), bytes_of({0xEF, 0xBB, 0x41}), bytes_of({0xEF, 0x41}),
           bytes_of({0xFE}), bytes_of({0xFE, 0x41}), bytes_of({0xFF}), bytes_of({0xFF, 0x41}), bytes_of({0xFE, 0xFE}),
           bytes_of({0xEF, 0xBB, 0xBE}), bytes_of({0xFF, 0xFF, 0xFE})}) {
    for (GTEXT_Encoding e : {GTEXT_ENCODING_WINDOWS_1252, GTEXT_ENCODING_UTF_8, GTEXT_ENCODING_UTF_16LE,
             GTEXT_ENCODING_X_USER_DEFINED, GTEXT_ENCODING_KOI8_R}) {
      const Out with = decode_whole(e, sniff, in);
      const Out without = decode_whole(e, none, in);
      EXPECT_EQ(with.bytes, without.bytes) << show(in) << gtext_encoding_name(e);
      EXPECT_EQ(with.replacements, without.replacements);
      EXPECT_FALSE(with.bom_found);
      EXPECT_EQ(with.encoding, e);
    }
  }
  // The same however the input is cut (the held bytes of a possible mark, at every position).
  for (const std::string & in : {bom8 + text, bytes_of({0xFF, 0xFE, 0x41, 0x00}), bytes_of({0xFE, 0xFF, 0x00, 0x41}),
           bytes_of({0xEF, 0xBB}), bytes_of({0xEF, 0xBB, 0x41}), bytes_of({0xFE, 0x41, 0x42}), bytes_of({0xFF})}) {
    const Out whole = decode_whole(GTEXT_ENCODING_WINDOWS_1252, sniff, in);
    EXPECT_EQ(decode_bytewise(GTEXT_ENCODING_WINDOWS_1252, sniff, in), whole) << show(in);
    for (size_t cut = 0; cut <= in.size(); cut++) {
      EXPECT_EQ(decode_cut(GTEXT_ENCODING_WINDOWS_1252, sniff, in, {cut}).bytes, whole.bytes) << show(in) << cut;
    }
  }
}

TEST(EncodingDecodeBom, RemovalForTheUtf16EncodingsAndTheApiOptions) {
  GTEXT_Decoder_Options removal = gtext_decoder_options_default();
  removal.bom_removal = true;
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_UTF_16LE, removal, bytes_of({0xFF, 0xFE, 0x41, 0x00})).bytes, "A");
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_UTF_16BE, removal, bytes_of({0xFE, 0xFF, 0x00, 0x41})).bytes, "A");
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_UTF_16LE, removal, bytes_of({0xFF, 0xFE, 0xFF, 0xFE, 0x41, 0x00})).bytes,
      cps_utf8({0xFEFF}) + "A");
  // A BE mark read as LE is U+FFFE, which is not a mark.
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_UTF_16LE, removal, bytes_of({0xFE, 0xFF})).bytes, cps_utf8({0xFFFE}));
  // The decoder's stream is over at a flush, and the next stream may begin with a mark of its own.
  GTEXT_Decoder * d = nullptr;
  ASSERT_EQ(gtext_decoder_new(GTEXT_ENCODING_UTF_8, &removal, &d), GTEXT_ENCODING_OK);
  Collect c;
  GTEXT_Encoding_Sink sink = sink_of(c);
  const std::string bom8 = bytes_of({0xEF, 0xBB, 0xBF});
  EXPECT_EQ(gtext_decoder_decode(d, bom8.data(), 2, false, &sink, nullptr), GTEXT_ENCODING_OK); // half a mark held
  EXPECT_EQ(gtext_decoder_decode(d, bom8.data() + 2, 1, false, &sink, nullptr), GTEXT_ENCODING_OK);
  EXPECT_EQ(gtext_decoder_decode(d, "x", 1, true, &sink, nullptr), GTEXT_ENCODING_OK);
  EXPECT_EQ(c.bytes, "x");
  c.bytes.clear();
  EXPECT_EQ(gtext_decoder_decode(d, bom8.data(), 3, true, &sink, nullptr), GTEXT_ENCODING_OK);
  EXPECT_EQ(c.bytes, "");
  gtext_decoder_free(d);
}

// ---- chunk independence ------------------------------------------------------------------------------------------

namespace {

void for_all_cuts(size_t n, const std::function<void(const std::vector<size_t> &)> & f) {
  // Every subset of the n - 1 interior positions (and the empty chunks that cuts at the ends would make are covered by
  // allowing a cut at 0 and at n as well).
  const size_t positions = n + 1;
  for (uint64_t mask = 0; mask < (uint64_t(1) << positions); mask++) {
    std::vector<size_t> cuts;
    for (size_t p = 0; p < positions; p++) {
      if (mask & (uint64_t(1) << p)) {
        cuts.push_back(p);
      }
    }
    f(cuts);
  }
}

} // namespace

TEST(EncodingDecodeChunks, EveryCutOfEveryShortInputGivesTheWholeInputsAnswer) {
  const int utf8_alphabet[] = {0x00, 0x41, 0x7F, 0x80, 0x8F, 0x90, 0xA0, 0xBF, 0xC1, 0xC2, 0xDF, 0xE0, 0xE1, 0xED, 0xEF, 0xF0,
      0xF1, 0xF4, 0xF5, 0xFF};
  const int utf16_alphabet[] = {0x00, 0x41, 0xD7, 0xD8, 0xDB, 0xDC, 0xDF, 0xE0, 0xFF, 0x3D, 0x29, 0xFE};
  const int single_alphabet[] = {0x00, 0x41, 0x7F, 0x80, 0x81, 0xA5, 0xFF};
  struct Row {
    GTEXT_Encoding enc;
    const int * alphabet;
    size_t size;
    size_t length;
  };
  const Row rows[] = {
      {GTEXT_ENCODING_UTF_8, utf8_alphabet, sizeof(utf8_alphabet) / sizeof(int), 4},
      {GTEXT_ENCODING_UTF_16LE, utf16_alphabet, sizeof(utf16_alphabet) / sizeof(int), 4},
      {GTEXT_ENCODING_UTF_16BE, utf16_alphabet, sizeof(utf16_alphabet) / sizeof(int), 4},
      {GTEXT_ENCODING_ISO_8859_3, single_alphabet, sizeof(single_alphabet) / sizeof(int), 3},
      {GTEXT_ENCODING_REPLACEMENT, single_alphabet, sizeof(single_alphabet) / sizeof(int), 3},
      {GTEXT_ENCODING_X_USER_DEFINED, single_alphabet, sizeof(single_alphabet) / sizeof(int), 3},
  };
  size_t decodes = 0;
  for (const Row & row : rows) {
    for (GTEXT_Decoder_Error_Mode mode : {GTEXT_DECODER_REPLACEMENT, GTEXT_DECODER_FATAL}) {
      GTEXT_Decoder_Options opts = decoder_options(mode);
      opts.bom_sniffing = false;
      for (size_t len = 0; len <= row.length; len++) {
        std::vector<size_t> idx(len, 0);
        for (;;) {
          std::string in;
          for (size_t i = 0; i < len; i++) {
            in += char(row.alphabet[idx[i]]);
          }
          const Out whole = decode_whole(row.enc, opts, in);
          for_all_cuts(len, [&](const std::vector<size_t> & cuts) {
            const Out o = decode_cut(row.enc, opts, in, cuts);
            decodes++;
            if (!(o == whole)) {
              ADD_FAILURE() << gtext_encoding_name(row.enc) << " " << show(in) << " cut at " << cuts.size()
                            << " points: " << show(o) << " vs whole " << show(whole);
            }
          });
          size_t k = 0;
          while (k < len && ++idx[k] == row.size) {
            idx[k++] = 0;
          }
          if (k == len) {
            break;
          }
        }
      }
    }
  }
  EXPECT_GT(decodes, 1000000u);
}

TEST(EncodingDecodeChunks, ByteAtATimeFeedsOfLongInputsAndRandomCuts) {
  Rng rng(7);
  const GTEXT_Encoding encodings[] = {GTEXT_ENCODING_UTF_8, GTEXT_ENCODING_UTF_16LE, GTEXT_ENCODING_UTF_16BE,
      GTEXT_ENCODING_WINDOWS_1252, GTEXT_ENCODING_ISO_8859_7, GTEXT_ENCODING_REPLACEMENT, GTEXT_ENCODING_X_USER_DEFINED,
      GTEXT_ENCODING_KOI8_U};
  size_t total = 0;
  for (GTEXT_Encoding e : encodings) {
    for (int round = 0; round < 40; round++) {
      // A mix of well-formed text and noise, so that errors of every kind fall everywhere.
      std::string in;
      const size_t len = 1 + rng.below(round < 30 ? 300 : 20000);
      while (in.size() < len) {
        switch (rng.below(5)) {
        case 0:
          in += char(rng.below(256));
          break;
        case 1:
          in += utf8_of(uint32_t(rng.below(0x110000)));
          break;
        case 2:
          in += bytes_of({int(0xD8 + rng.below(8)), int(rng.below(256)), int(0xDC + rng.below(4)), int(rng.below(256))});
          break;
        default:
          in += char('a' + rng.below(26));
        }
      }
      for (bool sniff : {false, true}) {
        GTEXT_Decoder_Options opts = gtext_decoder_options_default();
        opts.bom_sniffing = sniff;
        opts.bom_removal = !sniff && (round & 1);
        const Out whole = decode_whole(e, opts, in);
        EXPECT_EQ(decode_bytewise(e, opts, in), whole) << gtext_encoding_name(e);
        std::vector<size_t> cuts;
        for (size_t p = 0; p < in.size(); p++) {
          if (rng.below(7) == 0) {
            cuts.push_back(p);
          }
        }
        EXPECT_EQ(decode_cut(e, opts, in, cuts), whole) << gtext_encoding_name(e);
        total += in.size();
      }
    }
  }
  EXPECT_GT(total, 1000000u);
}

// ---- fatal mode, offsets, results ----------------------------------------------------------------------------------

TEST(EncodingDecodeFatal, TheOffsetOfTheByteAtWhichTheErrorWasFound) {
  GTEXT_Decoder_Options fatal = decoder_options(GTEXT_DECODER_FATAL);
  struct Case {
    GTEXT_Encoding enc;
    std::string in;
    size_t offset;
  };
  const Case cases[] = {
      {GTEXT_ENCODING_UTF_8, bytes_of({'a', 'b', 0xFF, 'c'}), 2},
      {GTEXT_ENCODING_UTF_8, bytes_of({'a', 0xE2, 0x82, 'c'}), 3},        // found at the byte that is not a continuation
      {GTEXT_ENCODING_UTF_8, bytes_of({'a', 'b', 0xE2, 0x82}), 4},        // found at the end of the stream
      {GTEXT_ENCODING_UTF_8, bytes_of({0xE0, 0x80, 0x80}), 1},
      {GTEXT_ENCODING_UTF_8, bytes_of({0xED, 0xA0, 0x80}), 1},
      {GTEXT_ENCODING_UTF_8, bytes_of({0xC0}), 0},
      {GTEXT_ENCODING_UTF_16LE, bytes_of({0x41, 0x00, 0x00}), 3},         // an odd byte at the end
      {GTEXT_ENCODING_UTF_16LE, bytes_of({0x00, 0xDC}), 1},               // a lone trailing surrogate, found at its second byte
      {GTEXT_ENCODING_UTF_16LE, bytes_of({0x00, 0xD8, 0x41, 0x00}), 3},   // a leading one, found at the unit after it
      {GTEXT_ENCODING_UTF_16BE, bytes_of({0xD8, 0x00}), 2},               // a leading one at the end
      {GTEXT_ENCODING_ISO_8859_3, bytes_of({'a', 'b', 0xA5}), 2},
      {GTEXT_ENCODING_REPLACEMENT, bytes_of({'a', 'b'}), 0},
  };
  for (const Case & c : cases) {
    const Out whole = decode_whole(c.enc, fatal, c.in);
    EXPECT_EQ(whole.status, GTEXT_ENCODING_E_MALFORMED) << show(c.in);
    EXPECT_EQ(whole.error_offset, c.offset) << gtext_encoding_name(c.enc) << " " << show(c.in);
    EXPECT_EQ(decode_bytewise(c.enc, fatal, c.in).error_offset, c.offset) << gtext_encoding_name(c.enc) << " " << show(c.in);
  }
  // The output before the error was delivered; the error ends the call; the decoder is usable again.
  {
    GTEXT_Decoder * d = nullptr;
    ASSERT_EQ(gtext_decoder_new(GTEXT_ENCODING_UTF_8, &fatal, &d), GTEXT_ENCODING_OK);
    Collect c;
    GTEXT_Encoding_Sink sink = sink_of(c);
    GTEXT_Decoder_Result r;
    const std::string bad = bytes_of({'o', 'k', 0xFF, 'n', 'o'});
    EXPECT_EQ(gtext_decoder_decode(d, bad.data(), bad.size(), true, &sink, &r), GTEXT_ENCODING_E_MALFORMED);
    EXPECT_EQ(c.bytes, "ok");
    EXPECT_EQ(r.output_bytes, 2u);
    EXPECT_EQ(r.status, GTEXT_ENCODING_E_MALFORMED);
    EXPECT_STREQ(r.message, "malformed input");
    EXPECT_EQ(r.input_bytes, 3u); // the faulty byte counts as taken
    EXPECT_EQ(gtext_decoder_decode(d, "fine", 4, true, &sink, &r), GTEXT_ENCODING_OK);
    EXPECT_EQ(c.bytes, "okfine");
    EXPECT_EQ(r.error_offset, 0u);
    // WPT: "Error seen with fatal does not prevent future decodes", also when the error ends a streamed piece.
    const std::string heart = bytes_of({0xE2, 0x99, 0xA5});
    c.bytes.clear();
    EXPECT_EQ(gtext_decoder_decode(d, heart.data(), 3, false, &sink, nullptr), GTEXT_ENCODING_OK);
    EXPECT_EQ(gtext_decoder_decode(d, heart.data(), 2, true, &sink, nullptr), GTEXT_ENCODING_E_MALFORMED);
    EXPECT_EQ(gtext_decoder_decode(d, heart.data(), 3, true, &sink, nullptr), GTEXT_ENCODING_OK);
    EXPECT_EQ(c.bytes, heart + heart);
    gtext_decoder_free(d);
  }
  // A failure part-way through a stream starts the next stream afresh, whatever was held.
  {
    GTEXT_Decoder * d = nullptr;
    ASSERT_EQ(gtext_decoder_new(GTEXT_ENCODING_UTF_8, &fatal, &d), GTEXT_ENCODING_OK);
    Collect c;
    GTEXT_Encoding_Sink sink = sink_of(c);
    EXPECT_EQ(gtext_decoder_decode(d, "\xE2\x82", 2, false, &sink, nullptr), GTEXT_ENCODING_OK);
    EXPECT_EQ(gtext_decoder_decode(d, "\xFF", 1, false, &sink, nullptr), GTEXT_ENCODING_E_MALFORMED);
    EXPECT_EQ(gtext_decoder_decode(d, "\xAC", 1, true, &sink, nullptr), GTEXT_ENCODING_E_MALFORMED); // not the rest of the old one
    gtext_decoder_free(d);
  }
}

TEST(EncodingDecodeFatal, ReplacementsAreCountedAndFlushStartsOver) {
  GTEXT_Decoder * d = nullptr;
  ASSERT_EQ(gtext_decoder_new(GTEXT_ENCODING_UTF_8, nullptr, &d), GTEXT_ENCODING_OK);
  EXPECT_EQ(gtext_decoder_encoding(d), GTEXT_ENCODING_UTF_8);
  Collect c;
  GTEXT_Encoding_Sink sink = sink_of(c);
  GTEXT_Decoder_Result r;
  EXPECT_EQ(gtext_decoder_decode(d, "\xE2\x82", 2, false, &sink, &r), GTEXT_ENCODING_OK);
  EXPECT_EQ(r.replacements, 0u);
  EXPECT_EQ(r.output_bytes, 0u);
  EXPECT_EQ(r.input_bytes, 2u);
  EXPECT_EQ(gtext_decoder_decode(d, "\xFF\xFF", 2, true, &sink, &r), GTEXT_ENCODING_OK);
  EXPECT_EQ(r.replacements, 3u);
  EXPECT_EQ(c.bytes, cps_utf8({0xFFFD, 0xFFFD, 0xFFFD}));
  // The truncated sequence of a flushed stream is gone: the next stream is not its continuation.
  c.bytes.clear();
  EXPECT_EQ(gtext_decoder_decode(d, "\xE2\x82", 2, true, &sink, &r), GTEXT_ENCODING_OK);
  EXPECT_EQ(gtext_decoder_decode(d, "\xAC", 1, true, &sink, &r), GTEXT_ENCODING_OK);
  EXPECT_EQ(c.bytes, cps_utf8({0xFFFD, 0xFFFD}));
  // reset() abandons a stream in progress.
  c.bytes.clear();
  EXPECT_EQ(gtext_decoder_decode(d, "\xE2\x82", 2, false, &sink, &r), GTEXT_ENCODING_OK);
  gtext_decoder_reset(d);
  EXPECT_EQ(gtext_decoder_decode(d, "\xAC", 1, true, &sink, &r), GTEXT_ENCODING_OK);
  EXPECT_EQ(c.bytes, cps_utf8({0xFFFD}));
  gtext_decoder_reset(nullptr);
  gtext_decoder_free(d);
  gtext_decoder_free(nullptr);
  EXPECT_EQ(gtext_decoder_encoding(nullptr), GTEXT_ENCODING_NONE);
}

// ---- limits and the sink --------------------------------------------------------------------------------------------

namespace {

// The longest prefix of `full` (UTF-8) that ends on a code point boundary and has at most `limit` bytes.
std::string boundary_prefix(const std::string & full, size_t limit) {
  size_t best = 0;
  for (size_t i = 0; i <= full.size() && i <= limit; i++) {
    if (i == full.size() || ((unsigned char)full[i] & 0xC0) != 0x80) {
      best = i;
    }
  }
  return full.substr(0, best);
}

} // namespace

TEST(EncodingDecodeLimits, TheLimitAtEveryValueGivesTheLongestWholePrefix) {
  // ASCII, 2-, 3- and 4-byte forms, replacements, all written in one stream.
  std::string in = "ab";
  in += utf8_of(0xE9);
  in += "c";
  in += utf8_of(0x20AC);
  in += bytes_of({0xFF});
  in += utf8_of(0x1F429);
  in += "d";
  in += bytes_of({0xE2, 0x82});
  const Out full = decode_whole(GTEXT_ENCODING_UTF_8, in);
  ASSERT_EQ(full.status, GTEXT_ENCODING_OK);
  for (size_t limit = 1; limit <= full.bytes.size() + 2; limit++) {
    GTEXT_Decoder_Options o = gtext_decoder_options_default();
    o.max_output_bytes = limit;
    const Out got = decode_whole(GTEXT_ENCODING_UTF_8, o, in);
    if (limit >= full.bytes.size()) {
      EXPECT_EQ(got.status, GTEXT_ENCODING_OK) << limit;
      EXPECT_EQ(got.bytes, full.bytes);
    } else {
      EXPECT_EQ(got.status, GTEXT_ENCODING_E_LIMIT_OUTPUT) << limit;
      EXPECT_EQ(got.bytes, boundary_prefix(full.bytes, limit)) << limit;
      EXPECT_LE(got.bytes.size(), limit);
    }
    // ... and the same fed a byte at a time.
    EXPECT_EQ(decode_bytewise(GTEXT_ENCODING_UTF_8, o, in).bytes, got.bytes) << limit;
    EXPECT_EQ(decode_bytewise(GTEXT_ENCODING_UTF_8, o, in).status, got.status) << limit;
  }
  // The same through a long run of ASCII (the fast path), where a run is delivered up to the limit exactly.
  const std::string ascii(10000, 'x');
  for (size_t limit : {size_t(1), size_t(4095), size_t(4096), size_t(4097), size_t(9999), size_t(10000), size_t(10001)}) {
    GTEXT_Decoder_Options o = gtext_decoder_options_default();
    o.max_output_bytes = limit;
    const Out got = decode_whole(GTEXT_ENCODING_UTF_8, o, ascii);
    EXPECT_EQ(got.status, limit >= ascii.size() ? GTEXT_ENCODING_OK : GTEXT_ENCODING_E_LIMIT_OUTPUT) << limit;
    EXPECT_EQ(got.bytes.size(), std::min(limit, ascii.size())) << limit;
  }
  // The limit counts a stream, over all its calls, and a flush starts a new count.
  {
    GTEXT_Decoder_Options o = gtext_decoder_options_default();
    o.max_output_bytes = 10;
    GTEXT_Decoder * d = nullptr;
    ASSERT_EQ(gtext_decoder_new(GTEXT_ENCODING_UTF_8, &o, &d), GTEXT_ENCODING_OK);
    Collect c;
    GTEXT_Encoding_Sink sink = sink_of(c);
    EXPECT_EQ(gtext_decoder_decode(d, "abcdef", 6, false, &sink, nullptr), GTEXT_ENCODING_OK);
    EXPECT_EQ(gtext_decoder_decode(d, "ghij", 4, false, &sink, nullptr), GTEXT_ENCODING_OK);
    GTEXT_Decoder_Result r;
    EXPECT_EQ(gtext_decoder_decode(d, "k", 1, false, &sink, &r), GTEXT_ENCODING_E_LIMIT_OUTPUT);
    EXPECT_EQ(c.bytes, "abcdefghij");
    EXPECT_STREQ(r.message, "output limit exceeded");
    // A failure starts a new stream; so does a flush.
    EXPECT_EQ(gtext_decoder_decode(d, "0123456789", 10, true, &sink, nullptr), GTEXT_ENCODING_OK);
    EXPECT_EQ(gtext_decoder_decode(d, "0123456789", 10, true, &sink, nullptr), GTEXT_ENCODING_OK);
    gtext_decoder_free(d);
  }
  // The default is 256 MiB (and 0 selects it); a limit of 1 is a limit of 1.
  EXPECT_EQ(gtext_decoder_options_default().max_output_bytes, size_t(256) * 1024 * 1024);
}

TEST(EncodingDecodeSink, AChunkIsNeverMoreThanAFewKilobytesAndARefusalIsAWriteError) {
  std::string in;
  for (int i = 0; i < 20000; i++) {
    in += utf8_of(0x20AC);
  }
  GTEXT_Decoder * d = nullptr;
  ASSERT_EQ(gtext_decoder_new(GTEXT_ENCODING_UTF_8, nullptr, &d), GTEXT_ENCODING_OK);
  Collect c;
  GTEXT_Encoding_Sink sink = sink_of(c);
  GTEXT_Decoder_Result r;
  ASSERT_EQ(gtext_decoder_decode(d, in.data(), in.size(), true, &sink, &r), GTEXT_ENCODING_OK);
  EXPECT_EQ(c.bytes, in);
  EXPECT_LE(c.largest, 4096u);
  EXPECT_GT(c.calls, 10u);
  EXPECT_EQ(r.output_bytes, in.size());
  // Every sink call that can be refused, is.
  const size_t calls = c.calls;
  for (size_t refuse = 0; refuse < calls; refuse += (refuse < 20 ? 1 : 7)) {
    Collect bad;
    bad.refuse_after = refuse;
    GTEXT_Encoding_Sink s2 = sink_of(bad);
    EXPECT_EQ(gtext_decoder_decode(d, in.data(), in.size(), true, &s2, &r), GTEXT_ENCODING_E_WRITE) << refuse;
    EXPECT_EQ(bad.calls, refuse); // the sink is not called again after it refused
    EXPECT_EQ(r.output_bytes, bad.bytes.size());
    EXPECT_STREQ(r.message, "the sink refused output");
    // ... and the decoder works afterwards.
    Collect good;
    GTEXT_Encoding_Sink s3 = sink_of(good);
    ASSERT_EQ(gtext_decoder_decode(d, "ok", 2, true, &s3, &r), GTEXT_ENCODING_OK);
    ASSERT_EQ(good.bytes, "ok");
  }
  // The final flush of the buffer is a sink call too.
  Collect last;
  last.refuse_after = 0;
  GTEXT_Encoding_Sink s4 = sink_of(last);
  EXPECT_EQ(gtext_decoder_decode(d, "tiny", 4, true, &s4, &r), GTEXT_ENCODING_E_WRITE);
  gtext_decoder_free(d);
}

// ---- arguments ---------------------------------------------------------------------------------------------------------

TEST(EncodingDecodeArguments, EveryMisuseHasItsOwnStatus) {
  GTEXT_Decoder * d = nullptr;
  EXPECT_EQ(gtext_decoder_new(GTEXT_ENCODING_NONE, nullptr, &d), GTEXT_ENCODING_E_INVALID);
  EXPECT_EQ(gtext_decoder_new(GTEXT_ENCODING_COUNT, nullptr, &d), GTEXT_ENCODING_E_INVALID);
  EXPECT_EQ(gtext_decoder_new(GTEXT_Encoding(62), nullptr, &d), GTEXT_ENCODING_E_INVALID);
  EXPECT_EQ(gtext_decoder_new(GTEXT_Encoding(63), nullptr, &d), GTEXT_ENCODING_E_INVALID);
  EXPECT_EQ(gtext_decoder_new(GTEXT_ENCODING_UTF_8, nullptr, nullptr), GTEXT_ENCODING_E_INVALID);
  EXPECT_EQ(d, nullptr);
  // The seven multi-byte encodings, which the first release of the module answered E_UNSUPPORTED, have decoders: no encoding does not.
  for (int e = 1; e < GTEXT_ENCODING_COUNT; e++) {
    ASSERT_EQ(gtext_decoder_new(GTEXT_Encoding(e), nullptr, &d), GTEXT_ENCODING_OK) << gtext_encoding_name(GTEXT_Encoding(e));
    gtext_decoder_free(d);
    d = nullptr;
  }
  GTEXT_Decoder_Options bad = gtext_decoder_options_default();
  poke(bad.error_mode, 2u);
  EXPECT_EQ(gtext_decoder_new(GTEXT_ENCODING_UTF_8, &bad, &d), GTEXT_ENCODING_E_OPTIONS);
  poke(bad.error_mode, 0xFFFFFFFFu);
  EXPECT_EQ(gtext_decoder_new(GTEXT_ENCODING_UTF_8, &bad, &d), GTEXT_ENCODING_E_OPTIONS);
  // Zeroed options are the defaults.
  GTEXT_Decoder_Options zero;
  std::memset(&zero, 0, sizeof(zero));
  ASSERT_EQ(gtext_decoder_new(GTEXT_ENCODING_UTF_8, &zero, &d), GTEXT_ENCODING_OK);
  Collect c;
  GTEXT_Encoding_Sink sink = sink_of(c);
  GTEXT_Decoder_Result r;
  std::memset(&r, 0xAA, sizeof(r));
  EXPECT_EQ(gtext_decoder_decode(nullptr, "a", 1, true, &sink, &r), GTEXT_ENCODING_E_INVALID);
  EXPECT_EQ(r.status, GTEXT_ENCODING_E_INVALID);
  EXPECT_NE(r.message, nullptr);
  EXPECT_EQ(gtext_decoder_decode(d, "a", 1, true, nullptr, &r), GTEXT_ENCODING_E_INVALID);
  GTEXT_Encoding_Sink nowrite = {nullptr, nullptr};
  EXPECT_EQ(gtext_decoder_decode(d, "a", 1, true, &nowrite, &r), GTEXT_ENCODING_E_INVALID);
  EXPECT_EQ(gtext_decoder_decode(d, nullptr, 1, true, &sink, &r), GTEXT_ENCODING_E_INVALID);
  EXPECT_EQ(gtext_decoder_decode(d, nullptr, 0, true, &sink, nullptr), GTEXT_ENCODING_OK); // no bytes, no pointer
  EXPECT_EQ(gtext_decoder_decode(d, "a", 1, true, &sink, nullptr), GTEXT_ENCODING_OK);       // no result wanted
  EXPECT_EQ(c.bytes, "a");
  gtext_decoder_free(d);
  // The buffer form.
  char * out = reinterpret_cast<char *>(0x1);
  size_t n = 77;
  EXPECT_EQ(gtext_encoding_decode_buffer(GTEXT_ENCODING_UTF_8, nullptr, "a", 1, nullptr, &n, &r), GTEXT_ENCODING_E_INVALID);
  EXPECT_EQ(gtext_encoding_decode_buffer(GTEXT_ENCODING_UTF_8, nullptr, nullptr, 3, &out, &n, &r), GTEXT_ENCODING_E_INVALID);
  EXPECT_EQ(gtext_encoding_decode_buffer(GTEXT_ENCODING_NONE, nullptr, "a", 1, &out, &n, &r), GTEXT_ENCODING_E_INVALID);
  GTEXT_Decoder_Options fatal_opts = decoder_options(GTEXT_DECODER_FATAL);
  EXPECT_EQ(gtext_encoding_decode_buffer(GTEXT_ENCODING_BIG5, &fatal_opts, "\x81", 1, &out, &n, &r), GTEXT_ENCODING_E_MALFORMED);
  EXPECT_EQ(out, reinterpret_cast<char *>(0x1)); // untouched on failure
  EXPECT_EQ(n, 77u);
  EXPECT_EQ(gtext_encoding_decode_buffer(GTEXT_ENCODING_UTF_8, nullptr, nullptr, 0, &out, &n, nullptr), GTEXT_ENCODING_OK);
  ASSERT_NE(out, nullptr); // an empty result is still a string
  EXPECT_EQ(n, 0u);
  EXPECT_EQ(out[0], '\0');
  gtext_encoding_buffer_free(out);
  gtext_encoding_buffer_free(nullptr);
  // Embedded NULs are kept; the buffer is NUL-terminated beyond its length.
  const std::string with_nul("a\0b\xC3\xA9", 5);
  ASSERT_EQ(gtext_encoding_decode_buffer(GTEXT_ENCODING_UTF_8, nullptr, with_nul.data(), with_nul.size(), &out, &n, &r), GTEXT_ENCODING_OK);
  EXPECT_EQ(std::string(out, n), with_nul);
  EXPECT_EQ(out[n], '\0');
  EXPECT_EQ(r.output_bytes, n);
  gtext_encoding_buffer_free(out);
  // A failure leaves no buffer.
  out = nullptr;
  EXPECT_EQ(gtext_encoding_decode_buffer(GTEXT_ENCODING_UTF_8, &bad, "a", 1, &out, &n, &r), GTEXT_ENCODING_E_OPTIONS);
  GTEXT_Decoder_Options fatal = decoder_options(GTEXT_DECODER_FATAL);
  EXPECT_EQ(gtext_encoding_decode_buffer(GTEXT_ENCODING_UTF_8, &fatal, "a\xFF", 2, &out, &n, &r), GTEXT_ENCODING_E_MALFORMED);
  EXPECT_EQ(out, nullptr);
  EXPECT_EQ(r.error_offset, 1u);
  GTEXT_Decoder_Options tiny = gtext_decoder_options_default();
  tiny.max_output_bytes = 3;
  EXPECT_EQ(gtext_encoding_decode_buffer(GTEXT_ENCODING_UTF_8, &tiny, "abcd", 4, &out, &n, &r), GTEXT_ENCODING_E_LIMIT_OUTPUT);
  EXPECT_EQ(out, nullptr);
}

