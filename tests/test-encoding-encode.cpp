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
 * The encoders: UTF-8 (which also repairs ill-formed input as the Encoding API does), the single-byte encodings against a
 * model built from the standard's indexes for every code point of the Basic Multilingual Plane, `x-user-defined`, the three
 * error modes, fatal-mode offsets, the output limit at every value, a refusing sink, the argument checks and the
 * statuses of the encodings that have no encoder; and chunk independence for all of it.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "encoding_test_support.h"

#include <functional>
#include <set>

using namespace enctest;

namespace {

// code point -> byte of an index, the first pointer winning ("the index pointer for a code point").
std::map<uint32_t, uint8_t> encode_model(GTEXT_Encoding e, const std::map<std::string, std::vector<int>> & indexes) {
  std::map<uint32_t, uint8_t> m;
  const auto & index = indexes.at(index_name(e));
  for (int p = 0; p < 128; p++) {
    if (index[p] >= 0 && !m.count(uint32_t(index[p]))) {
      m[uint32_t(index[p])] = uint8_t(0x80 + p);
    }
  }
  return m;
}

std::string reference(uint32_t cp) {
  return "&#" + std::to_string(cp) + ";";
}

} // namespace

TEST(EncodingEncodeUtf8, WellFormedInputIsUnchangedAndIllFormedInputIsRepaired) {
  const GTEXT_Encoder_Options html = encoder_options(GTEXT_ENCODER_HTML);
  for (const std::string & s : {std::string(""), std::string("plain"), bytes_of({0xC3, 0xA9}), bytes_of({0xE2, 0x82, 0xAC}),
           bytes_of({0xF0, 0x9F, 0x90, 0xA9}), bytes_of({0xEF, 0xBF, 0xBD}), bytes_of({0xF4, 0x8F, 0xBF, 0xBF}),
           std::string("a\0b", 3)}) {
    const Out o = encode_whole(GTEXT_ENCODING_UTF_8, html, s);
    EXPECT_EQ(o.status, GTEXT_ENCODING_OK);
    EXPECT_EQ(o.bytes, s);
    EXPECT_EQ(o.replacements, 0u);
    EXPECT_EQ(encode_whole(GTEXT_ENCODING_UTF_8, encoder_options(GTEXT_ENCODER_FATAL), s).bytes, s);
  }
  // Ill-formed input is the Encoding API's: each maximal subpart is U+FFFD.
  const std::string samples[] = {bytes_of({0xE0, 0x80, 0x80}), bytes_of({'a', 0xF0, 0x9F}),
      bytes_of({0x61, 0xF1, 0x80, 0x80, 0xE1, 0x80, 0xC2, 0x62, 0x80, 0x63, 0x80, 0xBF, 0x64}),
      bytes_of({0xED, 0xA0, 0x80}), bytes_of({0xFF, 0xFE}), bytes_of({0xC3}), bytes_of({0xE2, 0x82, 'x'})};
  for (const std::string & s : samples) {
    const RefResult ref = ref_utf8(s);
    for (GTEXT_Encoder_Error_Mode mode : {GTEXT_ENCODER_HTML, GTEXT_ENCODER_REPLACEMENT}) {
      const Out o = encode_whole(GTEXT_ENCODING_UTF_8, encoder_options(mode), s);
      EXPECT_EQ(o.status, GTEXT_ENCODING_OK);
      EXPECT_EQ(o.bytes, utf8_of_cps(ref.cps)) << show(s);
      EXPECT_EQ(o.replacements, ref.replacements) << show(s);
    }
    const Out f = encode_whole(GTEXT_ENCODING_UTF_8, encoder_options(GTEXT_ENCODER_FATAL), s);
    EXPECT_EQ(f.status, GTEXT_ENCODING_E_MALFORMED) << show(s);
  }
}

TEST(EncodingEncodeUtf8, EverySequenceOfOneToThreeBytesAgainstTheReference) {
  GTEXT_Encoder * e = nullptr;
  GTEXT_Encoder * ef = nullptr;
  ASSERT_EQ(gtext_encoder_new(GTEXT_ENCODING_UTF_8, nullptr, &ef), GTEXT_ENCODING_OK); // fatal by default
  GTEXT_Encoder_Options o = encoder_options(GTEXT_ENCODER_HTML);
  ASSERT_EQ(gtext_encoder_new(GTEXT_ENCODING_UTF_8, &o, &e), GTEXT_ENCODING_OK);
  Collect c;
  GTEXT_Encoding_Sink sink = sink_of(c);
  unsigned char buf[3];
  for (int n = 1; n <= 3; n++) {
    const uint32_t total = 1u << (8 * n);
    for (uint32_t v = 0; v < total; v++) {
      for (int k = 0; k < n; k++) {
        buf[k] = (unsigned char)(v >> (8 * (n - 1 - k)));
      }
      c.bytes.clear();
      GTEXT_Encoder_Result r;
      ASSERT_EQ(gtext_encoder_encode(e, (const char *)buf, n, true, &sink, &r), GTEXT_ENCODING_OK);
      const RefResult ref = ref_utf8(std::string((const char *)buf, n));
      if (c.bytes != utf8_of_cps(ref.cps) || r.replacements != ref.replacements) {
        FAIL() << show(std::string((const char *)buf, n));
      }
      const GTEXT_Encoding_Status s = gtext_encoder_encode(ef, (const char *)buf, n, true, &sink, nullptr);
      ASSERT_EQ(s == GTEXT_ENCODING_E_MALFORMED, ref.replacements > 0) << show(std::string((const char *)buf, n));
    }
  }
  gtext_encoder_free(e);
  gtext_encoder_free(ef);
}

TEST(EncodingEncodeSingleByte, EveryCodePointOfThePlaneAgainstTheModelOfTheIndexes) {
  const auto indexes = load_single_byte_indexes();
  ASSERT_EQ(indexes.size(), 27u);
  size_t encodable = 0, refused = 0;
  for (GTEXT_Encoding e : single_byte_encodings()) {
    const auto model = encode_model(e, indexes);
    GTEXT_Encoder * fatal = nullptr;
    GTEXT_Encoder_Options oh = encoder_options(GTEXT_ENCODER_HTML);
    GTEXT_Encoder_Options of = encoder_options(GTEXT_ENCODER_FATAL);
    GTEXT_Encoder_Options orp = encoder_options(GTEXT_ENCODER_REPLACEMENT);
    ASSERT_EQ(gtext_encoder_new(e, &of, &fatal), GTEXT_ENCODING_OK);
    Collect cf;
    GTEXT_Encoding_Sink sf = sink_of(cf);
    std::string everything, expect_html;
    for (uint32_t cp = 0; cp <= 0xFFFF; cp++) {
      if (cp >= 0xD800 && cp <= 0xDFFF) {
        continue;
      }
      const std::string in = utf8_of(cp);
      everything += in;
      const bool ascii = cp < 0x80;
      const auto it = model.find(cp);
      const bool ok = ascii || it != model.end();
      if (ok) {
        expect_html += char(ascii ? cp : it->second);
      } else {
        expect_html += reference(cp);
      }
      // Fatal mode, one code point per stream, for every code point: the byte or the failure.
      cf.bytes.clear();
      GTEXT_Encoder_Result r;
      const GTEXT_Encoding_Status s = gtext_encoder_encode(fatal, in.data(), in.size(), true, &sf, &r);
      if (ok) {
        ASSERT_EQ(s, GTEXT_ENCODING_OK) << gtext_encoding_name(e) << " " << std::hex << cp;
        ASSERT_EQ(cf.bytes.size(), 1u);
        ASSERT_EQ((unsigned char)cf.bytes[0], ascii ? cp : it->second) << gtext_encoding_name(e) << " " << std::hex << cp;
        encodable++;
      } else {
        ASSERT_EQ(s, GTEXT_ENCODING_E_UNENCODABLE) << gtext_encoding_name(e) << " " << std::hex << cp;
        ASSERT_EQ(r.error_code_point, cp);
        ASSERT_EQ(r.error_offset, 0u);
        ASSERT_EQ(cf.bytes, "");
        refused++;
      }
    }
    // The three modes over the whole plane in one stream.
    const Out h = encode_whole(e, oh, everything);
    EXPECT_EQ(h.status, GTEXT_ENCODING_OK);
    EXPECT_EQ(h.bytes, expect_html) << gtext_encoding_name(e);
    std::string expect_repl;
    size_t unencodable = 0;
    for (uint32_t cp = 0; cp <= 0xFFFF; cp++) {
      if (cp >= 0xD800 && cp <= 0xDFFF) {
        continue;
      }
      if (cp < 0x80) {
        expect_repl += char(cp);
      } else if (model.count(cp)) {
        expect_repl += char(model.at(cp));
      } else {
        expect_repl += '?';
        unencodable++;
      }
    }
    const Out rp = encode_whole(e, orp, everything);
    EXPECT_EQ(rp.bytes, expect_repl) << gtext_encoding_name(e);
    EXPECT_EQ(rp.replacements, unencodable);
    EXPECT_EQ(h.replacements, unencodable);
    // The astral planes: nothing a single-byte encoding has is there.
    for (uint32_t cp = 0x10000; cp <= 0x10FFFF; cp += 251) {
      const Out a = encode_whole(e, oh, utf8_of(cp));
      ASSERT_EQ(a.bytes, reference(cp)) << std::hex << cp;
    }
    EXPECT_EQ(encode_whole(e, oh, utf8_of(0x10FFFF)).bytes, "&#1114111;");
    // Everything an encoding can encode decodes back to the code point it was.
    std::string encodable_text, encodable_bytes;
    for (const auto & [cp, b] : model) {
      encodable_text += utf8_of(cp);
      encodable_bytes += char(b);
    }
    EXPECT_EQ(encode_whole(e, of, encodable_text).bytes, encodable_bytes) << gtext_encoding_name(e);
    EXPECT_EQ(decode_whole(e, encodable_bytes).bytes, encodable_text) << gtext_encoding_name(e);
    for (int b = 0; b < 256; b++) {
      const Out d = decode_whole(e, std::string(1, char(b)));
      if (d.replacements == 0) {
        EXPECT_EQ(encode_whole(e, of, d.bytes).bytes, std::string(1, char(b))) << gtext_encoding_name(e) << " " << b;
      }
    }
    gtext_encoder_free(fatal);
  }
  EXPECT_GT(encodable, 28u * 128u);
  EXPECT_GT(refused, 28u * 60000u);
}

TEST(EncodingEncodeSingleByte, TheExamplesOfTheMatrix) {
  const GTEXT_Encoder_Options html = encoder_options(GTEXT_ENCODER_HTML);
  // U+20AC is 0x80 in windows-1252 - which is what the labels "iso-8859-1" and "latin1" name - and has no place in
  // ISO-8859-2, which is where the numeric reference of the matrix's example comes from.
  EXPECT_EQ(encode_whole(encoding_by_name("windows-1252"), html, utf8_of(0x20AC)).bytes, "\x80");
  EXPECT_EQ(encode_whole(gtext_encoding_for_label("iso-8859-1", 10), html, utf8_of(0x20AC)).bytes, "\x80");
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_ISO_8859_2, html, utf8_of(0x20AC)).bytes, "&#8364;");
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_ISO_8859_2, encoder_options(GTEXT_ENCODER_FATAL), utf8_of(0x20AC)).status,
      GTEXT_ENCODING_E_UNENCODABLE);
  // The standard's own illustration: a pile of poo in windows-1252.
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_WINDOWS_1252, html, utf8_of(0x1F4A9)).bytes, "&#128169;");
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_WINDOWS_1252, html, "a" + utf8_of(0x1F4A9) + "b").bytes, "a&#128169;b");
  // Mapped bytes are not all in the BMP's obvious place: windows-1252 has U+0152 at 0x8C, and U+0081 is 0x81.
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_WINDOWS_1252, html, utf8_of(0x152) + utf8_of(0x81)).bytes, "\x8C\x81");
  // ISO-8859-8-I writes what ISO-8859-8 does.
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_ISO_8859_8_I, html, utf8_of(0x5D0)).bytes, "\xE0");
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_ISO_8859_8, html, utf8_of(0x5D0)).bytes, "\xE0");
  // macintosh has U+F8FF; koi8-u has U+0491.
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_MACINTOSH, html, utf8_of(0xF8FF)).bytes, "\xF0");
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_KOI8_U, html, utf8_of(0x491)).bytes, "\xAD");
}

TEST(EncodingEncodeXUserDefined, TheEncoderOfSection14_5_2) {
  const GTEXT_Encoder_Options html = encoder_options(GTEXT_ENCODER_HTML);
  std::string text, bytes;
  for (int b = 0; b < 256; b++) {
    text += utf8_of(b < 0x80 ? uint32_t(b) : 0xF780u + uint32_t(b) - 0x80u);
    bytes += char(b);
  }
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_X_USER_DEFINED, encoder_options(GTEXT_ENCODER_FATAL), text).bytes, bytes);
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_X_USER_DEFINED, bytes).bytes, text);
  for (uint32_t cp : {0x80u, 0xFFu, 0x100u, 0xF77Fu, 0xF800u, 0x20ACu, 0x1F4A9u}) {
    EXPECT_EQ(encode_whole(GTEXT_ENCODING_X_USER_DEFINED, html, utf8_of(cp)).bytes, reference(cp)) << std::hex << cp;
  }
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_X_USER_DEFINED, encoder_options(GTEXT_ENCODER_REPLACEMENT), utf8_of(0xE9)).bytes, "?");
}

TEST(EncodingEncodeFatal, TheOffsetAndTheCodePointOfTheFirstFailure) {
  const GTEXT_Encoder_Options fatal = encoder_options(GTEXT_ENCODER_FATAL);
  struct Case {
    GTEXT_Encoding enc;
    std::string in;
    GTEXT_Encoding_Status status;
    size_t offset;
    uint32_t cp;
  };
  const Case cases[] = {
      {GTEXT_ENCODING_WINDOWS_1252, "abc" + utf8_of(0x20AC) + "x", GTEXT_ENCODING_OK, 0, 0},
      {GTEXT_ENCODING_ISO_8859_2, "abc" + utf8_of(0x20AC) + "x", GTEXT_ENCODING_E_UNENCODABLE, 3, 0x20AC},
      {GTEXT_ENCODING_ISO_8859_2, utf8_of(0xE9) + utf8_of(0x1F4A9), GTEXT_ENCODING_E_UNENCODABLE, 2, 0x1F4A9},
      {GTEXT_ENCODING_ISO_8859_2, "ab" + utf8_of(0x20AC) + utf8_of(0x20AC), GTEXT_ENCODING_E_UNENCODABLE, 2, 0x20AC},
      {GTEXT_ENCODING_UTF_8, "ab" + bytes_of({0xFF}) + "c", GTEXT_ENCODING_E_MALFORMED, 2, 0},
      {GTEXT_ENCODING_UTF_8, "ab" + bytes_of({0xE2, 0x82}) + "c", GTEXT_ENCODING_E_MALFORMED, 4, 0},
      {GTEXT_ENCODING_UTF_8, "ab" + bytes_of({0xE2, 0x82}), GTEXT_ENCODING_E_MALFORMED, 4, 0}, // at the end of the stream
      {GTEXT_ENCODING_WINDOWS_1252, bytes_of({0xFF}), GTEXT_ENCODING_E_MALFORMED, 0, 0},
      // Malformed input becomes U+FFFD, which a single-byte encoding may not have: in fatal mode that is the malformed error.
      {GTEXT_ENCODING_X_USER_DEFINED, "a" + bytes_of({0xF0, 0x9F}), GTEXT_ENCODING_E_MALFORMED, 3, 0},
  };
  for (const Case & c : cases) {
    const Out o = encode_whole(c.enc, fatal, c.in);
    EXPECT_EQ(o.status, c.status) << gtext_encoding_name(c.enc) << " " << show(c.in);
    EXPECT_EQ(o.error_offset, c.offset) << gtext_encoding_name(c.enc) << " " << show(c.in);
    EXPECT_EQ(o.error_code_point, c.cp) << gtext_encoding_name(c.enc) << " " << show(c.in);
    const Out bytewise = encode_cut(c.enc, fatal, c.in, [&] {
      std::vector<size_t> cuts;
      for (size_t i = 1; i < c.in.size(); i++) {
        cuts.push_back(i);
      }
      return cuts;
    }());
    EXPECT_EQ(bytewise, o) << gtext_encoding_name(c.enc) << " " << show(c.in);
  }
  // The output before the failure was delivered.
  const Out o = encode_whole(GTEXT_ENCODING_ISO_8859_2, fatal, "abc" + utf8_of(0x20AC) + "x");
  EXPECT_EQ(o.bytes, "abc");
  // After a failure the encoder is back at the start.
  GTEXT_Encoder * e = nullptr;
  ASSERT_EQ(gtext_encoder_new(GTEXT_ENCODING_ISO_8859_2, nullptr, &e), GTEXT_ENCODING_OK);
  Collect c;
  GTEXT_Encoding_Sink sink = sink_of(c);
  EXPECT_EQ(gtext_encoder_encode(e, "\xE2\x82", 2, false, &sink, nullptr), GTEXT_ENCODING_OK);
  EXPECT_EQ(gtext_encoder_encode(e, "\xAC", 1, false, &sink, nullptr), GTEXT_ENCODING_E_UNENCODABLE);
  EXPECT_EQ(gtext_encoder_encode(e, "z", 1, true, &sink, nullptr), GTEXT_ENCODING_OK);
  EXPECT_EQ(c.bytes, "z");
  gtext_encoder_free(e);
}

TEST(EncodingEncodeChunks, EveryCutOfShortInputsAndByteAtATimeFeedsOfLongOnes) {
  const int alphabet[] = {'a', 0x00, 0x7F, 0x80, 0xBF, 0xC3, 0xA9, 0xE2, 0x82, 0xAC, 0xF0, 0x9F, 0xED, 0xFF};
  const GTEXT_Encoding encodings[] = {GTEXT_ENCODING_UTF_8, GTEXT_ENCODING_WINDOWS_1252, GTEXT_ENCODING_ISO_8859_2,
      GTEXT_ENCODING_X_USER_DEFINED};
  size_t runs = 0;
  for (GTEXT_Encoding e : encodings) {
    for (GTEXT_Encoder_Error_Mode mode : {GTEXT_ENCODER_FATAL, GTEXT_ENCODER_HTML, GTEXT_ENCODER_REPLACEMENT}) {
      const GTEXT_Encoder_Options opts = encoder_options(mode);
      const size_t n = sizeof(alphabet) / sizeof(int);
      for (size_t len = 0; len <= 4; len++) {
        std::vector<size_t> idx(len, 0);
        for (;;) {
          std::string in;
          for (size_t i = 0; i < len; i++) {
            in += char(alphabet[idx[i]]);
          }
          const Out whole = encode_whole(e, opts, in);
          const size_t positions = len + 1;
          for (uint64_t mask = 0; mask < (uint64_t(1) << positions); mask++) {
            std::vector<size_t> cuts;
            for (size_t p = 0; p < positions; p++) {
              if (mask & (uint64_t(1) << p)) {
                cuts.push_back(p);
              }
            }
            const Out o = encode_cut(e, opts, in, cuts);
            runs++;
            if (!(o == whole)) {
              ADD_FAILURE() << gtext_encoding_name(e) << " " << show(in) << " mask " << mask << ": " << show(o) << " vs "
                            << show(whole);
            }
          }
          size_t k = 0;
          while (k < len && ++idx[k] == n) {
            idx[k++] = 0;
          }
          if (k == len) {
            break;
          }
        }
      }
    }
  }
  EXPECT_GT(runs, 500000u);
  Rng rng(11);
  for (GTEXT_Encoding e : encodings) {
    for (int round = 0; round < 30; round++) {
      std::string in;
      const size_t len = 1 + rng.below(round < 25 ? 400 : 30000);
      while (in.size() < len) {
        switch (rng.below(4)) {
        case 0:
          in += char(rng.below(256));
          break;
        case 1:
          in += utf8_of(uint32_t(rng.below(0x110000)));
          break;
        default:
          in += char('a' + rng.below(26));
        }
      }
      for (GTEXT_Encoder_Error_Mode mode : {GTEXT_ENCODER_HTML, GTEXT_ENCODER_REPLACEMENT}) {
        const GTEXT_Encoder_Options opts = encoder_options(mode);
        std::vector<size_t> all;
        for (size_t i = 1; i < in.size(); i++) {
          all.push_back(i);
        }
        const Out whole = encode_whole(e, opts, in);
        EXPECT_EQ(encode_cut(e, opts, in, all), whole) << gtext_encoding_name(e);
      }
    }
  }
}

TEST(EncodingEncodeLimits, TheLimitAtEveryValueGivesTheLongestWholePrefix) {
  // References and encoded characters are never cut: the prefix ends between characters.
  const std::string in = "ab" + utf8_of(0x20AC) + "c" + utf8_of(0x1F4A9) + "d" + utf8_of(0xE9);
  for (GTEXT_Encoding e : {GTEXT_ENCODING_ISO_8859_2, GTEXT_ENCODING_UTF_8}) {
    GTEXT_Encoder_Options o = encoder_options(GTEXT_ENCODER_HTML);
    const Out full = encode_whole(e, o, in);
    ASSERT_EQ(full.status, GTEXT_ENCODING_OK);
    // The units of the output: a unit is one encoded character or one reference.
    std::vector<size_t> ends;
    {
      size_t i = 0;
      while (i < full.bytes.size()) {
        if (e == GTEXT_ENCODING_ISO_8859_2 && full.bytes.compare(i, 2, "&#") == 0) {
          i = full.bytes.find(';', i) + 1;
        } else if (e == GTEXT_ENCODING_UTF_8) {
          i++;
          while (i < full.bytes.size() && (full.bytes[i] & 0xC0) == 0x80) {
            i++;
          }
        } else {
          i++;
        }
        ends.push_back(i);
      }
    }
    for (size_t limit = 1; limit <= full.bytes.size() + 1; limit++) {
      o.max_output_bytes = limit;
      const Out got = encode_whole(e, o, in);
      size_t best = 0;
      for (size_t end : ends) {
        if (end <= limit) {
          best = end;
        }
      }
      if (limit >= full.bytes.size()) {
        EXPECT_EQ(got.status, GTEXT_ENCODING_OK);
        EXPECT_EQ(got.bytes, full.bytes);
      } else {
        EXPECT_EQ(got.status, GTEXT_ENCODING_E_LIMIT_OUTPUT) << limit;
        EXPECT_EQ(got.bytes, full.bytes.substr(0, best)) << gtext_encoding_name(e) << " " << limit;
      }
    }
  }
  // ASCII runs deliver up to the limit exactly.
  GTEXT_Encoder_Options o = encoder_options(GTEXT_ENCODER_HTML);
  o.max_output_bytes = 5000;
  const Out run = encode_whole(GTEXT_ENCODING_WINDOWS_1252, o, std::string(9000, 'x'));
  EXPECT_EQ(run.status, GTEXT_ENCODING_E_LIMIT_OUTPUT);
  EXPECT_EQ(run.bytes.size(), 5000u);
  EXPECT_EQ(gtext_encoder_options_default().max_output_bytes, size_t(256) * 1024 * 1024);
  EXPECT_EQ(gtext_encoder_options_default().error_mode, GTEXT_ENCODER_FATAL);
}

TEST(EncodingEncodeSink, ARefusalIsAWriteErrorAndTheEncoderWorksAfterwards) {
  std::string in;
  for (int i = 0; i < 30000; i++) {
    in += "x" + utf8_of(0x20AC);
  }
  GTEXT_Encoder_Options o = encoder_options(GTEXT_ENCODER_HTML);
  GTEXT_Encoder * e = nullptr;
  ASSERT_EQ(gtext_encoder_new(GTEXT_ENCODING_ISO_8859_2, &o, &e), GTEXT_ENCODING_OK);
  Collect c;
  GTEXT_Encoding_Sink sink = sink_of(c);
  GTEXT_Encoder_Result r;
  ASSERT_EQ(gtext_encoder_encode(e, in.data(), in.size(), true, &sink, &r), GTEXT_ENCODING_OK);
  EXPECT_LE(c.largest, 4096u);
  EXPECT_GT(c.calls, 10u);
  EXPECT_EQ(r.output_bytes, c.bytes.size());
  EXPECT_EQ(r.replacements, 30000u);
  const size_t calls = c.calls;
  for (size_t refuse = 0; refuse < calls; refuse += (refuse < 20 ? 1 : 11)) {
    Collect bad;
    bad.refuse_after = refuse;
    GTEXT_Encoding_Sink s2 = sink_of(bad);
    EXPECT_EQ(gtext_encoder_encode(e, in.data(), in.size(), true, &s2, &r), GTEXT_ENCODING_E_WRITE) << refuse;
    EXPECT_EQ(bad.calls, refuse);
    Collect good;
    GTEXT_Encoding_Sink s3 = sink_of(good);
    ASSERT_EQ(gtext_encoder_encode(e, "ok", 2, true, &s3, &r), GTEXT_ENCODING_OK);
    ASSERT_EQ(good.bytes, "ok");
  }
  gtext_encoder_free(e);
}

TEST(EncodingEncodeArguments, EveryMisuseHasItsOwnStatus) {
  GTEXT_Encoder * e = nullptr;
  EXPECT_EQ(gtext_encoder_new(GTEXT_ENCODING_NONE, nullptr, &e), GTEXT_ENCODING_E_INVALID);
  EXPECT_EQ(gtext_encoder_new(GTEXT_ENCODING_COUNT, nullptr, &e), GTEXT_ENCODING_E_INVALID);
  EXPECT_EQ(gtext_encoder_new(GTEXT_Encoding(62), nullptr, &e), GTEXT_ENCODING_E_INVALID);
  EXPECT_EQ(gtext_encoder_new(GTEXT_ENCODING_UTF_8, nullptr, nullptr), GTEXT_ENCODING_E_INVALID);
  for (GTEXT_Encoding enc : {GTEXT_ENCODING_REPLACEMENT, GTEXT_ENCODING_UTF_16BE, GTEXT_ENCODING_UTF_16LE}) {
    EXPECT_EQ(gtext_encoder_new(enc, nullptr, &e), GTEXT_ENCODING_E_NO_ENCODER) << gtext_encoding_name(enc);
    EXPECT_EQ(e, nullptr);
    // ... and the output encoding of each is one that has one.
    EXPECT_EQ(gtext_encoder_new(gtext_encoding_output_encoding(enc), nullptr, &e), GTEXT_ENCODING_OK);
    gtext_encoder_free(e);
    e = nullptr;
  }
  for (GTEXT_Encoding enc : {GTEXT_ENCODING_GBK, GTEXT_ENCODING_GB18030, GTEXT_ENCODING_BIG5, GTEXT_ENCODING_EUC_JP,
           GTEXT_ENCODING_ISO_2022_JP, GTEXT_ENCODING_SHIFT_JIS, GTEXT_ENCODING_EUC_KR}) {
    EXPECT_EQ(gtext_encoder_new(enc, nullptr, &e), GTEXT_ENCODING_E_UNSUPPORTED) << gtext_encoding_name(enc);
    EXPECT_EQ(e, nullptr);
  }
  GTEXT_Encoder_Options bad = gtext_encoder_options_default();
  poke(bad.error_mode, 3u);
  EXPECT_EQ(gtext_encoder_new(GTEXT_ENCODING_UTF_8, &bad, &e), GTEXT_ENCODING_E_OPTIONS);
  poke(bad.error_mode, 0xFFFFFFFFu);
  EXPECT_EQ(gtext_encoder_new(GTEXT_ENCODING_UTF_8, &bad, &e), GTEXT_ENCODING_E_OPTIONS);
  GTEXT_Encoder_Options zero;
  std::memset(&zero, 0, sizeof(zero));
  ASSERT_EQ(gtext_encoder_new(GTEXT_ENCODING_UTF_8, &zero, &e), GTEXT_ENCODING_OK);
  EXPECT_EQ(gtext_encoder_encoding(e), GTEXT_ENCODING_UTF_8);
  Collect c;
  GTEXT_Encoding_Sink sink = sink_of(c);
  GTEXT_Encoder_Result r;
  EXPECT_EQ(gtext_encoder_encode(nullptr, "a", 1, true, &sink, &r), GTEXT_ENCODING_E_INVALID);
  EXPECT_EQ(gtext_encoder_encode(e, "a", 1, true, nullptr, &r), GTEXT_ENCODING_E_INVALID);
  GTEXT_Encoding_Sink nowrite = {nullptr, nullptr};
  EXPECT_EQ(gtext_encoder_encode(e, "a", 1, true, &nowrite, &r), GTEXT_ENCODING_E_INVALID);
  EXPECT_EQ(gtext_encoder_encode(e, nullptr, 2, true, &sink, &r), GTEXT_ENCODING_E_INVALID);
  EXPECT_EQ(gtext_encoder_encode(e, nullptr, 0, true, &sink, nullptr), GTEXT_ENCODING_OK);
  gtext_encoder_reset(e);
  gtext_encoder_reset(nullptr);
  gtext_encoder_free(e);
  gtext_encoder_free(nullptr);
  EXPECT_EQ(gtext_encoder_encoding(nullptr), GTEXT_ENCODING_NONE);
  // The buffer form.
  char * out = reinterpret_cast<char *>(0x1);
  size_t n = 9;
  EXPECT_EQ(gtext_encoding_encode_buffer(GTEXT_ENCODING_UTF_8, nullptr, "a", 1, nullptr, &n, &r), GTEXT_ENCODING_E_INVALID);
  EXPECT_EQ(gtext_encoding_encode_buffer(GTEXT_ENCODING_UTF_8, nullptr, nullptr, 3, &out, &n, &r), GTEXT_ENCODING_E_INVALID);
  EXPECT_EQ(gtext_encoding_encode_buffer(GTEXT_ENCODING_UTF_16LE, nullptr, "a", 1, &out, &n, &r), GTEXT_ENCODING_E_NO_ENCODER);
  EXPECT_EQ(gtext_encoding_encode_buffer(GTEXT_ENCODING_SHIFT_JIS, nullptr, "a", 1, &out, &n, &r), GTEXT_ENCODING_E_UNSUPPORTED);
  EXPECT_EQ(out, reinterpret_cast<char *>(0x1));
  EXPECT_EQ(n, 9u);
  ASSERT_EQ(gtext_encoding_encode_buffer(GTEXT_ENCODING_WINDOWS_1252, nullptr, nullptr, 0, &out, &n, nullptr), GTEXT_ENCODING_OK);
  EXPECT_EQ(n, 0u);
  EXPECT_EQ(out[0], '\0');
  gtext_encoding_buffer_free(out);
  GTEXT_Encoder_Options html = encoder_options(GTEXT_ENCODER_HTML);
  ASSERT_EQ(gtext_encoding_encode_buffer(GTEXT_ENCODING_ISO_8859_2, &html, "\xE2\x82\xAC", 3, &out, &n, &r), GTEXT_ENCODING_OK);
  EXPECT_EQ(std::string(out, n), "&#8364;");
  EXPECT_EQ(out[n], '\0');
  EXPECT_EQ(r.replacements, 1u);
  gtext_encoding_buffer_free(out);
  out = nullptr;
  EXPECT_EQ(gtext_encoding_encode_buffer(GTEXT_ENCODING_ISO_8859_2, nullptr, "\xE2\x82\xAC", 3, &out, &n, &r), GTEXT_ENCODING_E_UNENCODABLE);
  EXPECT_EQ(out, nullptr);
  EXPECT_EQ(r.error_code_point, 0x20ACu);
}
