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
 * The multi-byte encodings (GBK, gb18030, Big5, EUC-JP, ISO-2022-JP, Shift_JIS, EUC-KR): the decoders and encoders against the model of
 * tests/encoding_cjk_model.h, which reads the pinned indexes.json and writes the standard's handlers a second time.
 *
 *   decode   every sequence of one, two and three bytes, in both error modes, with the offset of a fatal error; all 1,587,600 four-byte
 *            forms of gb18030; every sequence of four and five bytes over the bytes that matter for the encoding; ISO-2022-JP's escape
 *            sequences, exhaustively over a small alphabet; known sequences from the standard's text.
 *   encode   every code point (U+0000 to U+10FFFF but the surrogates) through every encoder in HTML mode (the bytes) and fatal mode (the
 *            status, the code point, the offset); ISO-2022-JP's three states over every pair and triple of the characters that move it.
 *   round    every code point an encoding writes decodes to itself, except the code points the standard folds (listed here, and counted).
 *   chunks   every cut, and every pair of cuts, of every short input, decode and encode; byte at a time; random long inputs.
 *   limits   the output limit at every value cuts only between characters, references, escape sequences and the two code points of one Big5 sequence.
 *   memory   the n-th allocation failing, and no allocation in a stream.
 *
 * **Under a sanitizer the sweeps are reduced**, because the exhaustive run is what the plain build is for and a sanitizer's job is the memory behaviour of the code it
 * sees: the three-byte sequences are every seventh (every thirty-first under ThreadSanitizer; a stride coprime to 256 visits every last byte), the code points above the
 * Basic Multilingual Plane every seventh (thirty-first), the alphabet sweeps of ISO-2022-JP and the other multi-byte decoders one byte shorter (a length of 3 for the
 * 4-byte one, 4 for the 5-byte, 5 for the 6-byte), the random streams a tenth of the rounds (3,000 for 30,000; 40 for 400), and two exact counts that depend on the whole sweep (the
 * gb18030 ranges' 39,420 + 1,048,576 characters, and the number of code points each encoder writes) are asserted only in the full run.
 * **GTEXT_TEST_FULL=1 in the environment turns every one of these reductions off**, for the same sanitizer: `GTEXT_TEST_FULL=1 tools/memcap -m 8G --
 * make test-asan PREFIX=...` (hours under TSan: run it as a job of its own, on tools/evo). Everything else is the same.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "encoding_test_support.h"
#include "encoding_cjk_model.h"
#include "markup_test_support.h"

#include <functional>
#include <set>

using namespace enctest;

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define GTEXT_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define GTEXT_TEST_SANITIZED 1
#endif
#endif
#ifndef GTEXT_TEST_SANITIZED
#define GTEXT_TEST_SANITIZED 0
#endif
// How much a sanitizer slows the sweeps: ThreadSanitizer about three times more than AddressSanitizer.
#if defined(__SANITIZE_THREAD__)
#define GTEXT_TEST_STRIDE 31
#elif GTEXT_TEST_SANITIZED
#define GTEXT_TEST_STRIDE 7
#else
#define GTEXT_TEST_STRIDE 1
#endif

namespace {

// Is this a sanitized build that takes the reduced sweeps? GTEXT_TEST_FULL=1 in the environment says no (see the header of this file).
inline bool reduced() {
  static const bool r = GTEXT_TEST_SANITIZED && !std::getenv("GTEXT_TEST_FULL");
  return r;
}
inline uint32_t sweep_stride() {
  return reduced() ? GTEXT_TEST_STRIDE : 1;
}

// The next code point of a sweep of all of them: under a sanitizer, every code point of the Basic Multilingual Plane (where the tables are dense and the special cases
// are) and every seventh above it.
inline uint32_t next_code_point(uint32_t cp) {
  cp++;
  if (cp >= 0xD800 && cp <= 0xDFFF) {
    cp = 0xE000;
  }
  if (reduced() && cp > 0x10000) {
    cp += GTEXT_TEST_STRIDE - 1;
  }
  return cp;
}

// A decoder reused across calls, for the exhaustive loops.
struct FastDec {
  GTEXT_Decoder * d = nullptr;
  Collect c;
  GTEXT_Encoding_Sink sink = sink_of(c);
  explicit FastDec(GTEXT_Encoding e, GTEXT_Decoder_Error_Mode mode = GTEXT_DECODER_REPLACEMENT) {
    GTEXT_Decoder_Options o = decoder_options(mode);
    EXPECT_EQ(gtext_decoder_new(e, &o, &d), GTEXT_ENCODING_OK);
  }
  ~FastDec() {
    gtext_decoder_free(d);
  }
  GTEXT_Encoding_Status run(const unsigned char * p, size_t n, GTEXT_Decoder_Result * r) {
    c.bytes.clear();
    return gtext_decoder_decode(d, p, n, true, &sink, r);
  }
};

struct FastEnc {
  GTEXT_Encoder * e = nullptr;
  Collect c;
  GTEXT_Encoding_Sink sink = sink_of(c);
  explicit FastEnc(GTEXT_Encoding enc, GTEXT_Encoder_Error_Mode mode) {
    GTEXT_Encoder_Options o = encoder_options(mode);
    EXPECT_EQ(gtext_encoder_new(enc, &o, &e), GTEXT_ENCODING_OK);
  }
  ~FastEnc() {
    gtext_encoder_free(e);
  }
  GTEXT_Encoding_Status run(const std::string & in, GTEXT_Encoder_Result * r) {
    c.bytes.clear();
    return gtext_encoder_encode(e, in.data(), in.size(), true, &sink, r);
  }
};

std::string name_of(GTEXT_Encoding e) {
  return gtext_encoding_name(e);
}

std::string hex(const std::string & s) {
  return show(s);
}

// Compare the library with the model for one input, both modes. Returns false (after reporting) on the first difference.
bool decode_agrees(FastDec & rep, FastDec & fat, GTEXT_Encoding enc, const unsigned char * in, size_t n, ModelDecoded & m_rep,
    ModelDecoded & m_fat) {
  model_decode_into(m_rep, enc, in, n, false);
  GTEXT_Decoder_Result r;
  if (rep.run(in, n, &r) != GTEXT_ENCODING_OK || rep.c.bytes != utf8_of_cps(m_rep.cps) || r.replacements != m_rep.replacements) {
    ADD_FAILURE() << name_of(enc) << " replacement mode, input " << hex(std::string((const char *)in, n)) << ": got [" << hex(rep.c.bytes)
                  << "] with " << r.replacements << " replacements, the model [" << hex(utf8_of_cps(m_rep.cps)) << "] with "
                  << m_rep.replacements;
    return false;
  }
  model_decode_into(m_fat, enc, in, n, true);
  GTEXT_Decoder_Result fr;
  const GTEXT_Encoding_Status s = fat.run(in, n, &fr);
  const bool failed = s == GTEXT_ENCODING_E_MALFORMED;
  if ((s != GTEXT_ENCODING_OK && !failed) || failed != m_fat.failed || fat.c.bytes != utf8_of_cps(m_fat.cps)
      || (failed && fr.error_offset != m_fat.error_offset)) {
    ADD_FAILURE() << name_of(enc) << " fatal mode, input " << hex(std::string((const char *)in, n)) << ": status "
                  << gtext_encoding_status_string(s) << " offset " << fr.error_offset << " output [" << hex(fat.c.bytes) << "], the model: "
                  << (m_fat.failed ? "error" : "ok") << " at " << m_fat.error_offset << " output [" << hex(utf8_of_cps(m_fat.cps)) << "]";
    return false;
  }
  return true;
}

// Every sequence of one to three bytes.
void exhaustive_decode(GTEXT_Encoding enc) {
  FastDec rep(enc);
  FastDec fat(enc, GTEXT_DECODER_FATAL);
  ModelDecoded m_rep, m_fat;
  size_t count = 0;
  size_t errors = 0;
  unsigned char buf[3];
  for (int n = 1; n <= 3; n++) {
    const uint32_t total = 1u << (8 * n);
    const uint32_t stride = n == 3 ? sweep_stride() : 1;
    for (uint32_t v = 0; v < total; v += stride) {
      for (int k = 0; k < n; k++) {
        buf[k] = (unsigned char)(v >> (8 * (n - 1 - k)));
      }
      if (!decode_agrees(rep, fat, enc, buf, size_t(n), m_rep, m_fat)) {
        return;
      }
      count++;
      errors += m_rep.replacements > 0;
    }
  }
  const size_t expected = 256u + 65536u + (16777216u + sweep_stride() - 1) / sweep_stride();
  EXPECT_EQ(count, expected) << name_of(enc);
  EXPECT_GT(errors, 100u) << name_of(enc);
}

// The bytes that decide something in some multi-byte decoder: the ends of every range the decoders of sections 10 to 13 test, and one inside.
const unsigned char kInteresting[] = {0x00, 0x0A, 0x0E, 0x0F, 0x1B, 0x20, 0x21, 0x24, 0x28, 0x2F, 0x30, 0x39, 0x3A, 0x40, 0x41, 0x42, 0x49, 0x4A, 0x5C, 0x5F, 0x60,
    0x7E, 0x7F, 0x80, 0x81, 0x8E, 0x8F, 0x9F, 0xA0, 0xA1, 0xDF, 0xE0, 0xFC, 0xFD, 0xFE, 0xFF};

void alphabet_decode(GTEXT_Encoding enc, size_t length, std::vector<unsigned char> alphabet) {
  FastDec rep(enc);
  FastDec fat(enc, GTEXT_DECODER_FATAL);
  ModelDecoded m_rep, m_fat;
  size_t total = 1;
  for (size_t i = 0; i < length; i++) {
    total *= alphabet.size();
  }
  std::vector<unsigned char> buf(length);
  for (size_t v = 0; v < total; v++) {
    size_t x = v;
    for (size_t k = length; k > 0; k--) {
      buf[k - 1] = alphabet[x % alphabet.size()];
      x /= alphabet.size();
    }
    if (!decode_agrees(rep, fat, enc, buf.data(), length, m_rep, m_fat)) {
      return;
    }
  }
}

std::string cp_utf8(std::initializer_list<uint32_t> cps) {
  std::string s;
  for (uint32_t cp : cps) {
    s += utf8_of(cp);
  }
  return s;
}

// What decoding `bytes` gives, as UTF-8, with the library (one call, flush).
std::string dec(GTEXT_Encoding e, const std::string & bytes, GTEXT_Decoder_Error_Mode mode = GTEXT_DECODER_REPLACEMENT) {
  GTEXT_Decoder_Options o = decoder_options(mode);
  return decode_whole(e, o, bytes).bytes;
}

} // namespace

// ---- known sequences ------------------------------------------------------------------------------------------------------

TEST(EncodingCjkDecode, TheMatrixOfTheSpecAndTheStandardsOwnExamples) {
  // Shift_JIS: katakana A, half-width katakana I, ideographic space, an invalid trail byte (the space is not swallowed).
  EXPECT_EQ(dec(GTEXT_ENCODING_SHIFT_JIS, bytes_of({0x83, 0x41})), cp_utf8({0x30A2}));
  EXPECT_EQ(dec(GTEXT_ENCODING_SHIFT_JIS, bytes_of({0xB1})), cp_utf8({0xFF71}));
  EXPECT_EQ(dec(GTEXT_ENCODING_SHIFT_JIS, bytes_of({0x81, 0x40})), cp_utf8({0x3000}));
  EXPECT_EQ(dec(GTEXT_ENCODING_SHIFT_JIS, bytes_of({0x81, 0x20})), cp_utf8({0xFFFD, 0x20}));
  EXPECT_EQ(decode_whole(GTEXT_ENCODING_SHIFT_JIS, decoder_options(GTEXT_DECODER_FATAL), bytes_of({0x81, 0x20})).status,
      GTEXT_ENCODING_E_MALFORMED);
  // ... and 0x80 is U+0080 (the standard's text), 0xA0 and 0xFD to 0xFF are errors, the EUDC pointers 8836 to 10715 are the private use area.
  EXPECT_EQ(dec(GTEXT_ENCODING_SHIFT_JIS, bytes_of({0x80})), cp_utf8({0x80}));
  EXPECT_EQ(dec(GTEXT_ENCODING_SHIFT_JIS, bytes_of({0xA0, 0xFD, 0xFE, 0xFF})), cp_utf8({0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD}));
  EXPECT_EQ(dec(GTEXT_ENCODING_SHIFT_JIS, bytes_of({0xF0, 0x40})), cp_utf8({0xE000})); // pointer (0xF0 - 0xC1) x 188 + 0 = 8836
  EXPECT_EQ(dec(GTEXT_ENCODING_SHIFT_JIS, bytes_of({0xF9, 0xFC})), cp_utf8({0xE757}));  // pointer 10715
  // EUC-JP: hiragana A, half-width katakana I, a JIS X 0212 character, and a truncated sequence.
  EXPECT_EQ(dec(GTEXT_ENCODING_EUC_JP, bytes_of({0xA4, 0xA2})), cp_utf8({0x3042}));
  EXPECT_EQ(dec(GTEXT_ENCODING_EUC_JP, bytes_of({0x8E, 0xB1})), cp_utf8({0xFF71}));
  EXPECT_EQ(dec(GTEXT_ENCODING_EUC_JP, bytes_of({0x8F, 0xB0, 0xA1})), cp_utf8({0x4E02}));
  EXPECT_EQ(dec(GTEXT_ENCODING_EUC_JP, bytes_of({0xA4})), cp_utf8({0xFFFD}));
  EXPECT_EQ(dec(GTEXT_ENCODING_EUC_JP, bytes_of({0x8F, 0xB0})), cp_utf8({0xFFFD}));
  EXPECT_EQ(dec(GTEXT_ENCODING_EUC_JP, bytes_of({0x8E, 0x41})), cp_utf8({0xFFFD, 0x41}));
  // ISO-2022-JP: hiragana A between escape sequences, and the errors of the escape states.
  EXPECT_EQ(dec(GTEXT_ENCODING_ISO_2022_JP, bytes_of({0x1B, 0x24, 0x42, 0x24, 0x22, 0x1B, 0x28, 0x42})), cp_utf8({0x3042}));
  EXPECT_EQ(dec(GTEXT_ENCODING_ISO_2022_JP, bytes_of({0x1B, 0x24, 0x42, 0x24, 0x22, 0x1B, 0x28, 0x42, 0x41})), cp_utf8({0x3042, 0x41}));
  EXPECT_EQ(dec(GTEXT_ENCODING_ISO_2022_JP, bytes_of({0x1B, 0x28, 0x4A, 0x5C, 0x7E, 0x1B, 0x28, 0x42, 0x5C})),
      cp_utf8({0xA5, 0x203E, 0x5C}));
  EXPECT_EQ(dec(GTEXT_ENCODING_ISO_2022_JP, bytes_of({0x1B, 0x28, 0x49, 0x31, 0x5F, 0x60})), cp_utf8({0xFF71, 0xFF9F, 0xFFFD}));
  EXPECT_EQ(dec(GTEXT_ENCODING_ISO_2022_JP, bytes_of({0x1B, 0x41})), cp_utf8({0xFFFD, 0x41}));  // escape start, restore, error
  EXPECT_EQ(dec(GTEXT_ENCODING_ISO_2022_JP, bytes_of({0x1B, 0x24, 0x41})), cp_utf8({0xFFFD, 0x24, 0x41}));
  EXPECT_EQ(dec(GTEXT_ENCODING_ISO_2022_JP, bytes_of({0x0E, 0x0F, 0x41})), cp_utf8({0xFFFD, 0xFFFD, 0x41}));
  EXPECT_EQ(dec(GTEXT_ENCODING_ISO_2022_JP, bytes_of({0x1B, 0x28, 0x42, 0x1B, 0x28, 0x42})), cp_utf8({0xFFFD})); // two escapes with nothing between: an error
  EXPECT_EQ(dec(GTEXT_ENCODING_ISO_2022_JP, bytes_of({0x1B, 0x24, 0x42, 0x24})), cp_utf8({0xFFFD}));              // lead byte alone at the end
  EXPECT_EQ(dec(GTEXT_ENCODING_ISO_2022_JP, bytes_of({0x1B, 0x24})), cp_utf8({0xFFFD, 0x24})); // escape at the end of the stream: restore the 0x24
  EXPECT_EQ(dec(GTEXT_ENCODING_ISO_2022_JP, bytes_of({0x80, 0xFF})), cp_utf8({0xFFFD, 0xFFFD}));
  // An escape in the trail byte state: "Set ISO-2022-JP decoder state to escape start and return error" (the lead byte is lost, the escape is read).
  EXPECT_EQ(dec(GTEXT_ENCODING_ISO_2022_JP, bytes_of({0x1B, 0x24, 0x42, 0x24, 0x1B, 0x28, 0x42, 0x41})), cp_utf8({0xFFFD, 0x41}));
  // GBK and gb18030: the four-byte form, ideographic space, euro sign for 0x80, and 0xFF.
  for (GTEXT_Encoding e : {GTEXT_ENCODING_GBK, GTEXT_ENCODING_GB18030}) {
    EXPECT_EQ(dec(e, bytes_of({0x81, 0x30, 0x81, 0x30})), cp_utf8({0x80})) << name_of(e);
    EXPECT_EQ(dec(e, bytes_of({0xA1, 0xA1})), cp_utf8({0x3000})) << name_of(e);
    EXPECT_EQ(dec(e, bytes_of({0xA3, 0xA0})), cp_utf8({0x3000})) << name_of(e); // "0xA3 0xA0 which maps to U+3000" for compatibility
    EXPECT_EQ(dec(e, bytes_of({0x80})), cp_utf8({0x20AC})) << name_of(e);       // the standard: "If byte is 0x80, then return code point U+20AC"
    EXPECT_EQ(dec(e, bytes_of({0xFF})), cp_utf8({0xFFFD})) << name_of(e);
    // A four-byte form broken at its last byte: error, and « second, third, byte » restored: the digit, then the lead byte with the byte after it (a pointer of the two-byte index).
    EXPECT_EQ(dec(e, bytes_of({0x81, 0x30, 0x81, 0x41})), cp_utf8({0xFFFD, 0x30}) + utf8_of(uint32_t(cjk_indexes().gb18030[1]))) << name_of(e);
    EXPECT_EQ(dec(e, bytes_of({0x84, 0x31, 0xA4, 0x39})), cp_utf8({0xFFFF})) << name_of(e);       // pointer 39419, the last of the first ranges
    EXPECT_EQ(dec(e, bytes_of({0x84, 0x31, 0xA5, 0x30})), cp_utf8({0xFFFD})) << name_of(e);       // pointer 39420: "greater than 39419 and less than 189000" is null
    EXPECT_EQ(dec(e, bytes_of({0x90, 0x30, 0x81, 0x30})), cp_utf8({0x10000})) << name_of(e);      // pointer 189000
    EXPECT_EQ(dec(e, bytes_of({0xE3, 0x32, 0x9A, 0x35})), cp_utf8({0x10FFFF})) << name_of(e);     // pointer 1237575, the last
    EXPECT_EQ(dec(e, bytes_of({0xE3, 0x32, 0x9A, 0x36})), cp_utf8({0xFFFD})) << name_of(e);       // 1237576: "greater than 1237575" is null
  }
  EXPECT_EQ(dec(GTEXT_ENCODING_GB18030, bytes_of({0x81, 0x35, 0xF4, 0x37})), cp_utf8({0xE7C7})); // pointer 7457
  // Big5: the four two-code-point sequences, and the pointers around them.
  EXPECT_EQ(dec(GTEXT_ENCODING_BIG5, bytes_of({0x88, 0x62})), cp_utf8({0xCA, 0x304}));
  EXPECT_EQ(dec(GTEXT_ENCODING_BIG5, bytes_of({0x88, 0x64})), cp_utf8({0xCA, 0x30C}));
  EXPECT_EQ(dec(GTEXT_ENCODING_BIG5, bytes_of({0x88, 0xA3})), cp_utf8({0xEA, 0x304}));
  EXPECT_EQ(dec(GTEXT_ENCODING_BIG5, bytes_of({0x88, 0xA5})), cp_utf8({0xEA, 0x30C}));
  EXPECT_EQ(dec(GTEXT_ENCODING_BIG5, bytes_of({0xA4, 0x40})), cp_utf8({0x4E00}));
  EXPECT_EQ(dec(GTEXT_ENCODING_BIG5, bytes_of({0x81, 0x20})), cp_utf8({0xFFFD, 0x20}));
  EXPECT_EQ(dec(GTEXT_ENCODING_BIG5, bytes_of({0x81})), cp_utf8({0xFFFD}));
  EXPECT_EQ(dec(GTEXT_ENCODING_BIG5, bytes_of({0x80, 0xFF})), cp_utf8({0xFFFD, 0xFFFD}));
  // The decoder reads the Hong Kong extensions below pointer 5024 that the encoder "avoids returning literally": 0x8740 is pointer 6 x 157 = 942 (a
  // plane 2 character in the index), and the decoder reads it.
  EXPECT_EQ(dec(GTEXT_ENCODING_BIG5, bytes_of({0x87, 0x40})), utf8_of(uint32_t(cjk_indexes().big5[942])));
  // EUC-KR: Hangul syllable GA, and the region of the index "C9 FE".
  EXPECT_EQ(dec(GTEXT_ENCODING_EUC_KR, bytes_of({0xB0, 0xA1})), cp_utf8({0xAC00}));
  EXPECT_EQ(dec(GTEXT_ENCODING_EUC_KR, bytes_of({0x81, 0x41})), cp_utf8({0xAC02}));
  // 0xC9 0xFE is in the user-defined area of KS X 1001: the index has no code point for it, and the decoder says error (the standard decides, "per index").
  EXPECT_EQ(cjk_indexes().euc_kr[(0xC9 - 0x81) * 190 + (0xFE - 0x41)], -1);
  EXPECT_EQ(dec(GTEXT_ENCODING_EUC_KR, bytes_of({0xC9, 0xFE})), cp_utf8({0xFFFD}));
  EXPECT_EQ(dec(GTEXT_ENCODING_EUC_KR, bytes_of({0xC9, 0xA1})), cp_utf8({0xFFFD}));
  EXPECT_EQ(dec(GTEXT_ENCODING_EUC_KR, bytes_of({0x81, 0x20})), cp_utf8({0xFFFD, 0x20}));
  EXPECT_EQ(dec(GTEXT_ENCODING_EUC_KR, bytes_of({0x80})), cp_utf8({0xFFFD}));
}

// ---- exhaustive decodes ------------------------------------------------------------------------------------------------------

#define GTEXT_CJK_EXHAUSTIVE(Name, Enc)                                                                              \
  TEST(EncodingCjkDecodeExhaustive, Name) {                                                                          \
    exhaustive_decode(Enc);                                                                                           \
  }
GTEXT_CJK_EXHAUSTIVE(Gbk, GTEXT_ENCODING_GBK)
GTEXT_CJK_EXHAUSTIVE(Gb18030, GTEXT_ENCODING_GB18030)
GTEXT_CJK_EXHAUSTIVE(Big5, GTEXT_ENCODING_BIG5)
GTEXT_CJK_EXHAUSTIVE(EucJp, GTEXT_ENCODING_EUC_JP)
GTEXT_CJK_EXHAUSTIVE(Iso2022Jp, GTEXT_ENCODING_ISO_2022_JP)
GTEXT_CJK_EXHAUSTIVE(ShiftJis, GTEXT_ENCODING_SHIFT_JIS)
GTEXT_CJK_EXHAUSTIVE(EucKr, GTEXT_ENCODING_EUC_KR)

TEST(EncodingCjkDecodeExhaustive, EveryFourByteFormOfGb18030) {
  // 126 x 10 x 126 x 10 sequences of the form lead digit lead digit: every pointer of the ranges index, valid or not.
  for (GTEXT_Encoding enc : {GTEXT_ENCODING_GB18030, GTEXT_ENCODING_GBK}) {
    FastDec rep(enc);
    FastDec fat(enc, GTEXT_DECODER_FATAL);
    ModelDecoded m_rep, m_fat;
    size_t count = 0, valid = 0, seen = 0;
    unsigned char buf[4];
    for (int a = 0x81; a <= 0xFE; a++) {
      for (int b = 0x30; b <= 0x39; b++) {
        for (int c = 0x81; c <= 0xFE; c++) {
          for (int d = 0x30; d <= 0x39; d++) {
            buf[0] = (unsigned char)a;
            buf[1] = (unsigned char)b;
            buf[2] = (unsigned char)c;
            buf[3] = (unsigned char)d;
            if (reduced() && (seen++ % GTEXT_TEST_STRIDE) != 0) {
              continue; // (under a sanitizer, every seventh or thirty-first)
            }
            if (!decode_agrees(rep, fat, enc, buf, 4, m_rep, m_fat)) {
              return;
            }
            count++;
            valid += m_rep.replacements == 0;
          }
        }
      }
    }
    if (!reduced()) {
      EXPECT_EQ(count, 126u * 10u * 126u * 10u);
      // The pointers of the ranges index: 39420 in the BMP ranges (but the surrogates' gap does not exist: the index skips none), and 1,048,576 above it.
      EXPECT_EQ(valid, 39420u + 1048576u);
    }
  }
}

TEST(EncodingCjkDecodeExhaustive, EverySequenceOfFourBytesOverTheBytesThatMatter) {
  const std::vector<unsigned char> alphabet(std::begin(kInteresting), std::end(kInteresting));
  const size_t length = reduced() ? 3 : 4;
  for (GTEXT_Encoding enc : multi_byte_encodings()) {
    alphabet_decode(enc, length, alphabet);
    if (::testing::Test::HasFailure()) {
      return;
    }
  }
}

TEST(EncodingCjkDecodeExhaustive, EverySequenceOfFiveBytesOverTheBytesThatDecideTheRestoreRules) {
  // The restore rules re-read up to three bytes: five bytes cover the cases where a restored byte is itself the start of the next sequence.
  const std::vector<unsigned char> alphabet = {0x1B, 0x24, 0x28, 0x30, 0x39, 0x41, 0x81, 0xA1, 0xFE, 0x8E, 0x8F};
  const size_t length = reduced() ? 4 : 5;
  for (GTEXT_Encoding enc : multi_byte_encodings()) {
    alphabet_decode(enc, length, alphabet);
    if (::testing::Test::HasFailure()) {
      return;
    }
  }
}

TEST(EncodingCjkDecodeExhaustive, Iso2022JpEscapeSequencesAndTheTwoByteCharactersBetweenThem) {
  // ESC, the two introducers, every final byte of the four sequences, a byte of each kind of state, a control, a high byte: twelve bytes, six of them in a row.
  const std::vector<unsigned char> alphabet = {0x1B, 0x24, 0x28, 0x40, 0x42, 0x49, 0x4A, 0x21, 0x5C, 0x7E, 0x0E, 0x80};
  alphabet_decode(GTEXT_ENCODING_ISO_2022_JP, reduced() ? 5 : 6, alphabet);
}

TEST(EncodingCjkDecodeRandom, LongerStreamsOfEveryEncodingAgainstTheModel) {
  Rng rng(0xC11);
  for (GTEXT_Encoding enc : multi_byte_encodings()) {
    FastDec rep(enc);
    FastDec fat(enc, GTEXT_DECODER_FATAL);
    ModelDecoded m_rep, m_fat;
    for (int round = 0; round < (reduced() ? 3000 : 30000); round++) {
      std::string in;
      const size_t len = rng.below(60);
      while (in.size() < len) {
        switch (rng.below(5)) {
        case 0: in += char(rng.below(256)); break;
        case 1: in += char(kInteresting[rng.below(sizeof kInteresting)]); break;
        case 2: in += char(0x20 + rng.below(0x5F)); break;
        case 3: in += char(0x81 + rng.below(0x7E)); in += char(0x40 + rng.below(0xBF)); break;
        default: { // a real character
          std::string one;
          const std::vector<uint32_t> cps = {uint32_t(0x3042 + rng.below(80)), uint32_t(0x4E00 + rng.below(5000)), uint32_t(0xAC00 + rng.below(2000)),
              uint32_t(0xFF61 + rng.below(63)), uint32_t(0x80 + rng.below(0x700))};
          const ModelEncodeResult e = model_encode(enc, {cps[rng.below(cps.size())]}, true);
          in += e.bytes;
        }
        }
      }
      if (!decode_agrees(rep, fat, enc, (const unsigned char *)in.data(), in.size(), m_rep, m_fat)) {
        return;
      }
    }
  }
}

// ---- encoders ----------------------------------------------------------------------------------------------------------------------

// Every code point through one encoder, alone, in HTML mode (the bytes) and in fatal mode (the status, the code point and the offset).
void exhaustive_encode(GTEXT_Encoding enc, size_t * encodable_out) {
  FastEnc html(enc, GTEXT_ENCODER_HTML);
  FastEnc fatal(enc, GTEXT_ENCODER_FATAL);
  size_t encodable = 0;
  for (uint32_t cp = 0; cp <= 0x10FFFF; cp = next_code_point(cp)) {
    const std::string in = utf8_of(cp);
    const ModelEncodeResult mh = model_encode(enc, {cp}, true);
    GTEXT_Encoder_Result r;
    if (html.run(in, &r) != GTEXT_ENCODING_OK || html.c.bytes != mh.bytes) {
      ADD_FAILURE() << name_of(enc) << " U+" << std::hex << cp << " (HTML): got [" << hex(html.c.bytes) << "], the model [" << hex(mh.bytes) << "]";
      return;
    }
    const ModelEncodeResult mf = model_encode(enc, {cp}, false);
    const GTEXT_Encoding_Status s = fatal.run(in, &r);
    if (mf.failed) {
      if (s != GTEXT_ENCODING_E_UNENCODABLE || r.error_code_point != mf.error_cp || r.error_offset != 0 || fatal.c.bytes != mf.bytes) {
        ADD_FAILURE() << name_of(enc) << " U+" << std::hex << cp << " (fatal): status " << gtext_encoding_status_string(s) << " code point "
                      << r.error_code_point << ", the model: error with U+" << mf.error_cp;
        return;
      }
    } else {
      encodable++;
      if (s != GTEXT_ENCODING_OK || fatal.c.bytes != mf.bytes) {
        ADD_FAILURE() << name_of(enc) << " U+" << std::hex << cp << " (fatal): status " << gtext_encoding_status_string(s);
        return;
      }
    }
  }
  *encodable_out = encodable;
}

TEST(EncodingCjkEncodeExhaustive, EveryCodePointThroughEveryEncoder) {
  // How many code points each encoder writes (not as a reference): the count is derived from the standard's data and pinned here as a tripwire.
  std::map<GTEXT_Encoding, size_t> counts;
  for (GTEXT_Encoding enc : multi_byte_encodings()) {
    size_t n = 0;
    exhaustive_encode(enc, &n);
    if (::testing::Test::HasFailure()) {
      return;
    }
    counts[enc] = n;
  }
  // gb18030 writes every scalar value but U+E5E5 (1,112,064 - 1). The others write what their index holds, ASCII included, once per code point
  // (a code point on two pointers counts once): 128 + the two-byte index (+ the half-width katakana, U+00A5 and U+203E, U+2212 for the Japanese ones,
  // U+0080 for Shift_JIS, the side table and U+20AC for GBK). The counts come from the standard's data through the model above and are a tripwire for a
  // changed index: a changed pin changes them on purpose, and this is the line to read.
  if (!reduced()) {
    EXPECT_EQ(counts[GTEXT_ENCODING_GB18030], 1112064u - 1u);
    EXPECT_EQ(counts[GTEXT_ENCODING_GBK], 24085u);
    EXPECT_EQ(counts[GTEXT_ENCODING_BIG5], 14781u);
    EXPECT_EQ(counts[GTEXT_ENCODING_EUC_JP], 7520u);
    EXPECT_EQ(counts[GTEXT_ENCODING_SHIFT_JIS], 7521u);
    EXPECT_EQ(counts[GTEXT_ENCODING_ISO_2022_JP], 7517u);
    EXPECT_EQ(counts[GTEXT_ENCODING_EUC_KR], 17176u);
  }
  for (const auto & [enc, n] : counts) {
    RecordProperty(name_of(enc).c_str(), int(n));
  }
}

// Which code points an encoding writes but does not read back as the same code point: the standard's folds, listed and counted.
TEST(EncodingCjkRoundTrip, EveryCodePointAnEncodingWritesDecodesToItselfExceptTheFoldedOnes) {
  for (GTEXT_Encoding enc : multi_byte_encodings()) {
    FastEnc fatal(enc, GTEXT_ENCODER_FATAL);
    std::map<uint32_t, uint32_t> folded; // encodable, decodes to something else (the first code point of it)
    size_t same = 0;
    for (uint32_t cp = 0; cp <= 0x10FFFF; cp = next_code_point(cp)) {
      GTEXT_Encoder_Result r;
      if (fatal.run(utf8_of(cp), &r) != GTEXT_ENCODING_OK) {
        continue;
      }
      const std::string back = decode_whole(enc, fatal.c.bytes).bytes;
      if (back == utf8_of(cp)) {
        same++;
      } else {
        folded[cp] = cps_of_utf8(back).empty() ? 0 : cps_of_utf8(back)[0];
      }
    }
    std::map<uint32_t, uint32_t> want;
    switch (enc) {
    case GTEXT_ENCODING_EUC_JP:
    case GTEXT_ENCODING_SHIFT_JIS:
      // U+00A5 and U+203E are written as the ASCII 0x5C and 0x7E; U+2212 is written as U+FF0D (the pointer of the full-width hyphen-minus).
      want = {{0xA5, 0x5C}, {0x203E, 0x7E}, {0x2212, 0xFF0D}};
      break;
    case GTEXT_ENCODING_ISO_2022_JP:
      // U+2212 as U+FF0D; every half-width katakana as its full-width form (and U+00A5 and U+203E go through the Roman state and come back).
      want = {{0x2212, 0xFF0D}};
      for (uint32_t cp = 0xFF61; cp <= 0xFF9F; cp++) {
        want[cp] = uint32_t(cjk_indexes().katakana[cp - 0xFF61]);
      }
      break;
    case GTEXT_ENCODING_GBK:
    case GTEXT_ENCODING_GB18030:
      // The eighteen code points of the encoder's side table (GB18030-2005 compatibility) decode as the code points the 2022 edition assigned.
      for (const auto & [cp, bytes] : std::vector<std::pair<uint32_t, std::string>>{{0xE78D, bytes_of({0xA6, 0xD9})}, {0xE78E, bytes_of({0xA6, 0xDA})},
               {0xE78F, bytes_of({0xA6, 0xDB})}, {0xE790, bytes_of({0xA6, 0xDC})}, {0xE791, bytes_of({0xA6, 0xDD})}, {0xE792, bytes_of({0xA6, 0xDE})},
               {0xE793, bytes_of({0xA6, 0xDF})}, {0xE794, bytes_of({0xA6, 0xEC})}, {0xE795, bytes_of({0xA6, 0xED})}, {0xE796, bytes_of({0xA6, 0xF3})},
               {0xE81E, bytes_of({0xFE, 0x59})}, {0xE826, bytes_of({0xFE, 0x61})}, {0xE82B, bytes_of({0xFE, 0x66})}, {0xE82C, bytes_of({0xFE, 0x67})},
               {0xE832, bytes_of({0xFE, 0x6D})}, {0xE843, bytes_of({0xFE, 0x7E})}, {0xE854, bytes_of({0xFE, 0x90})}, {0xE864, bytes_of({0xFE, 0xA0})}}) {
        want[cp] = cps_of_utf8(dec(enc, bytes))[0];
      }
      break;
    default:
      break;
    }
    EXPECT_EQ(folded, want) << name_of(enc);
    EXPECT_GT(same, 7000u) << name_of(enc);
  }
}

// The stateful encoder: every pair and triple of the characters that move ISO-2022-JP between its states, cut anywhere.
TEST(EncodingCjkEncode, Iso2022JpStatesOverEveryPairAndTripleOfTheCharactersThatMoveThem) {
  const std::vector<uint32_t> alphabet = {0x41, 0x5C, 0x7E, 0x0A, 0x0E, 0x0F, 0x1B, 0xA5, 0x203E, 0x2212, 0xFF0D, 0xFF61, 0xFF71, 0xFF9F, 0x3042, 0x4E9C, 0x00E9,
      0x20AC, 0x1F4A9, 0xFFFD};
  for (size_t length = 1; length <= 4; length++) {
    size_t total = 1;
    for (size_t i = 0; i < length; i++) {
      total *= alphabet.size();
    }
    FastEnc html(GTEXT_ENCODING_ISO_2022_JP, GTEXT_ENCODER_HTML);
    FastEnc fatal(GTEXT_ENCODING_ISO_2022_JP, GTEXT_ENCODER_FATAL);
    for (size_t v = 0; v < total; v++) {
      std::vector<uint32_t> cps(length);
      size_t x = v;
      std::string in;
      for (size_t k = length; k > 0; k--) {
        cps[k - 1] = alphabet[x % alphabet.size()];
        x /= alphabet.size();
      }
      for (uint32_t cp : cps) {
        in += utf8_of(cp);
      }
      const ModelEncodeResult mh = model_encode(GTEXT_ENCODING_ISO_2022_JP, cps, true);
      GTEXT_Encoder_Result r;
      if (html.run(in, &r) != GTEXT_ENCODING_OK || html.c.bytes != mh.bytes) {
        FAIL() << "HTML mode, input " << hex(in) << ": got [" << hex(html.c.bytes) << "], the model [" << hex(mh.bytes) << "]";
      }
      const ModelEncodeResult mf = model_encode(GTEXT_ENCODING_ISO_2022_JP, cps, false);
      const GTEXT_Encoding_Status s = fatal.run(in, &r);
      if (mf.failed) {
        size_t offset = 0;
        for (size_t k = 0; k < mf.error_index; k++) {
          offset += utf8_of(cps[k]).size();
        }
        // (the fatal status names the control itself; only the html mode's reference is U+FFFD's: "return error with U+FFFD")
        if (s != GTEXT_ENCODING_E_UNENCODABLE || r.error_code_point != mf.error_cp || r.error_offset != offset || fatal.c.bytes != mf.bytes) {
          FAIL() << "fatal mode, input " << hex(in) << ": " << gtext_encoding_status_string(s) << " code point " << r.error_code_point << " at "
                 << r.error_offset << " output [" << hex(fatal.c.bytes) << "], the model: U+" << std::hex << mf.error_cp << " at " << offset << " output ["
                 << hex(mf.bytes) << "]";
        }
      } else if (s != GTEXT_ENCODING_OK || fatal.c.bytes != mf.bytes) {
        FAIL() << "fatal mode, input " << hex(in) << ": status " << gtext_encoding_status_string(s);
      }
    }
  }
}

// The served encoder (12.2.2) step by step, written out by hand (not by the model): what is written before an unencodable character or a control from each of the
// three encoder states, and what the fatal status and the html reference say. ESC ( B is `1B 28 42`, ESC ( J `1B 28 4A`, ESC $ B `1B 24 42`.
TEST(EncodingCjkEncode, Iso2022JpTheSequenceAroundAnErrorFromEachStateAndWhatTheErrorNames) {
  const std::string to_ascii = bytes_of({0x1B, 0x28, 0x42}), to_roman = bytes_of({0x1B, 0x28, 0x4A}), to_jis = bytes_of({0x1B, 0x24, 0x42});
  struct Case {
    const char * name;
    std::vector<uint32_t> in;
    std::string html;               // the html mode's whole output
    bool fatal_fails;               // the fatal mode stops ...
    std::string fatal_bytes;        // ... having written these bytes ...
    uint32_t fatal_cp;              // ... naming this code point ...
    size_t fatal_offset;            // ... at this byte of the UTF-8 input
  };
  const std::vector<Case> cases = {
    // ASCII state: no escape is written, a reference is text already.
    {"ASCII, then an unencodable character", {'A', 0x20AC, 'B'}, "A&#8364;B", true, "A", 0x20AC, 1},
    // Roman state: an unencodable non-ASCII character is "Return error with codePoint" at once, with no escape before it (only the jis0208 state has the
    // "restore ... 0x1B 0x28 0x42" step); the reference is written in the Roman state, where it is ASCII text too (digits, & # ;), and the escape back to
    // ASCII is the flush's, or comes before an ASCII character Roman does not have (U+005C, U+007E).
    {"Roman, then an unencodable character", {0xA5, 0x20AC, 'B'}, to_roman + "\x5c&#8364;B" + to_ascii, true, to_roman + "\x5c", 0x20AC, 2},
    // jis0208 state: "If pointer is null: if the encoder state is jis0208, then restore codePoint to ioQueue, set the encoder state to ASCII, and return ESC ( B".
    {"jis0208, then an unencodable character", {0x3042, 0x20AC, 'B'}, to_jis + "\x24\x22" + to_ascii + "&#8364;B", true, to_jis + "\x24\x22" + to_ascii, 0x20AC, 3},
    // After a half-width katakana, which is written as the full-width one (state jis0208).
    {"half-width katakana, then an unencodable character", {0xFF71, 0x20AC}, to_jis + "\x25\x22" + to_ascii + "&#8364;", true, to_jis + "\x25\x22" + to_ascii, 0x20AC, 3},
    {"U+2212 (set to U+FF0D), then an unencodable character", {0x2212, 0x20AC}, to_jis + "\x21\x5d" + to_ascii + "&#8364;", true, to_jis + "\x21\x5d" + to_ascii, 0x20AC, 3},
    // The controls: error "with U+FFFD" in the ASCII and Roman states.
    {"U+000E in ASCII", {'A', 0x0E}, "A&#65533;", true, "A", 0x0E, 1},
    {"U+000F in ASCII", {0x0F}, "&#65533;", true, "", 0x0F, 0},
    {"U+001B in ASCII", {0x1B, 'A'}, "&#65533;A", true, "", 0x1B, 0},
    {"U+001B in Roman", {0xA5, 0x1B}, to_roman + "\x5c&#65533;" + to_ascii, true, to_roman + "\x5c", 0x1B, 2},
    {"U+000E in Roman", {0x203E, 0x0E}, to_roman + "\x7e&#65533;" + to_ascii, true, to_roman + "\x7e", 0x0E, 3},
    // From jis0208 a control is an ASCII code point: the escape to ASCII comes first, and then the ASCII state refuses it.
    {"U+001B in jis0208", {0x3042, 0x1B}, to_jis + "\x24\x22" + to_ascii + "&#65533;", true, to_jis + "\x24\x22" + to_ascii, 0x1B, 3},
    {"U+000F after a half-width katakana", {0xFF71, 0x0F}, to_jis + "\x25\x22" + to_ascii + "&#65533;", true, to_jis + "\x25\x22" + to_ascii, 0x0F, 3},
    // The same two code points again after an error: the state is what the error left.
    {"Roman, an error, then a character only ASCII has", {0xA5, 0x20AC, '~'}, to_roman + "\x5c&#8364;" + to_ascii + "~", true, to_roman + "\x5c", 0x20AC, 2},
    {"jis0208, an error, then jis0208 again", {0x3042, 0x20AC, 0x3042}, to_jis + "\x24\x22" + to_ascii + "&#8364;" + to_jis + "\x24\x22" + to_ascii, true,
        to_jis + "\x24\x22" + to_ascii, 0x20AC, 3},
  };
  for (const Case & c : cases) {
    std::string in;
    for (uint32_t cp : c.in) {
      in += utf8_of(cp);
    }
    const Out h = encode_whole(GTEXT_ENCODING_ISO_2022_JP, encoder_options(GTEXT_ENCODER_HTML), in);
    const std::string & want = c.html;
    EXPECT_EQ(h.bytes, want) << c.name;
    EXPECT_EQ(h.status, GTEXT_ENCODING_OK) << c.name;
    const Out f = encode_whole(GTEXT_ENCODING_ISO_2022_JP, encoder_options(GTEXT_ENCODER_FATAL), in);
    EXPECT_EQ(f.status, GTEXT_ENCODING_E_UNENCODABLE) << c.name;
    EXPECT_EQ(f.bytes, c.fatal_bytes) << c.name;
    EXPECT_EQ(f.error_code_point, c.fatal_cp) << c.name;
    EXPECT_EQ(f.error_offset, c.fatal_offset) << c.name;
    // The model says the same of both modes (the fatal status names the handled code point, the html reference the one the standard returns).
    std::vector<uint32_t> cps = c.in;
    EXPECT_EQ(model_encode(GTEXT_ENCODING_ISO_2022_JP, cps, true).bytes, want) << c.name;
    const ModelEncodeResult mf = model_encode(GTEXT_ENCODING_ISO_2022_JP, cps, false);
    EXPECT_TRUE(mf.failed) << c.name;
    EXPECT_EQ(mf.error_cp, c.fatal_cp) << c.name;
  }
  // In the Roman state an unencodable character at the very end: the reference is followed by the flush's escape.
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_ISO_2022_JP, encoder_options(GTEXT_ENCODER_HTML), cp_utf8({0xA5, 0x20AC})).bytes, to_roman + "\x5c&#8364;" + to_ascii);
  // The replacement mode (this library's addition) writes `?` for a control as for any other.
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_ISO_2022_JP, encoder_options(GTEXT_ENCODER_REPLACEMENT), std::string("a\x1b" "b")).bytes, "a?b");
}

TEST(EncodingCjkEncode, Iso2022JpTheSpecsExampleAndTheEscapeBackToAsciiBeforeAReference) {
  // The matrix: U+3042, then U+00E9 (not in JIS X 0208) in HTML mode.
  const Out out = encode_whole(GTEXT_ENCODING_ISO_2022_JP, encoder_options(GTEXT_ENCODER_HTML), cp_utf8({0x3042, 0xE9}));
  EXPECT_EQ(out.status, GTEXT_ENCODING_OK);
  EXPECT_EQ(out.bytes, bytes_of({0x1B, 0x24, 0x42, 0x24, 0x22, 0x1B, 0x28, 0x42}) + "&#233;");
  EXPECT_EQ(out.replacements, 1u);
  // Encoding U+00A5 gives 0x1B 0x28 0x4A 0x5C 0x1B 0x28 0x42: the standard's own example, and the one that cannot be concatenated.
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_ISO_2022_JP, encoder_options(GTEXT_ENCODER_HTML), cp_utf8({0xA5})).bytes,
      bytes_of({0x1B, 0x28, 0x4A, 0x5C, 0x1B, 0x28, 0x42}));
  // A reference in the Roman state is written in it (the escape back to ASCII is at the end), and a reference in the jis0208 state is preceded by the escape.
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_ISO_2022_JP, encoder_options(GTEXT_ENCODER_HTML), cp_utf8({0xA5, 0xE9, 0xA5})).bytes,
      bytes_of({0x1B, 0x28, 0x4A, 0x5C}) + "&#233;" + bytes_of({0x5C, 0x1B, 0x28, 0x42}));
  // The controls it will not write: error with U+FFFD, which the HTML mode writes as "&#65533;"; the fatal status names the control itself (see the next test).
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_ISO_2022_JP, encoder_options(GTEXT_ENCODER_HTML), std::string("a\x1b" "b")).bytes, "a&#65533;b");
  const Out fatal = encode_whole(GTEXT_ENCODING_ISO_2022_JP, encoder_options(GTEXT_ENCODER_FATAL), std::string("a\x0e"));
  EXPECT_EQ(fatal.status, GTEXT_ENCODING_E_UNENCODABLE);
  EXPECT_EQ(fatal.error_code_point, 0x0Eu);
  EXPECT_EQ(fatal.error_offset, 1u);
  // Half-width katakana become full-width; U+2212 becomes U+FF0D.
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_ISO_2022_JP, encoder_options(GTEXT_ENCODER_FATAL), cp_utf8({0xFF71})).bytes,
      encode_whole(GTEXT_ENCODING_ISO_2022_JP, encoder_options(GTEXT_ENCODER_FATAL), cp_utf8({0x30A2})).bytes);
  // The stream of nothing is nothing, and a stream ending in the jis0208 state ends with the escape (the flush), also when fed in pieces.
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_ISO_2022_JP, encoder_options(GTEXT_ENCODER_FATAL), "").bytes, "");
  GTEXT_Encoder * e = nullptr;
  GTEXT_Encoder_Options o = encoder_options(GTEXT_ENCODER_FATAL);
  ASSERT_EQ(gtext_encoder_new(GTEXT_ENCODING_ISO_2022_JP, &o, &e), GTEXT_ENCODING_OK);
  Collect c;
  GTEXT_Encoding_Sink sink = sink_of(c);
  const std::string a = cp_utf8({0x3042});
  ASSERT_EQ(gtext_encoder_encode(e, a.data(), a.size(), false, &sink, nullptr), GTEXT_ENCODING_OK);
  EXPECT_EQ(c.bytes, bytes_of({0x1B, 0x24, 0x42, 0x24, 0x22})); // not closed yet
  ASSERT_EQ(gtext_encoder_encode(e, nullptr, 0, true, &sink, nullptr), GTEXT_ENCODING_OK);
  EXPECT_EQ(c.bytes, bytes_of({0x1B, 0x24, 0x42, 0x24, 0x22, 0x1B, 0x28, 0x42}));
  // ... and the encoder starts over in ASCII after a flush.
  c.bytes.clear();
  const std::string b = "b";
  ASSERT_EQ(gtext_encoder_encode(e, b.data(), b.size(), true, &sink, nullptr), GTEXT_ENCODING_OK);
  EXPECT_EQ(c.bytes, "b");
  gtext_encoder_free(e);
}

TEST(EncodingCjkEncode, TheOtherEncodersKnownCharacters) {
  const GTEXT_Encoder_Options html = encoder_options(GTEXT_ENCODER_HTML);
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_SHIFT_JIS, html, cp_utf8({0x3042, 0xFF71, 0xA5, 0x203E, 0x80, 0x2212})).bytes,
      bytes_of({0x82, 0xA0, 0xB1, 0x5C, 0x7E, 0x80, 0x81, 0x7C}));
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_EUC_JP, html, cp_utf8({0x3042, 0xFF71, 0xA5, 0x203E, 0x80})).bytes,
      bytes_of({0xA4, 0xA2, 0x8E, 0xB1, 0x5C, 0x7E}) + "&#128;");
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_EUC_KR, html, cp_utf8({0xAC00, 0x41, 0x1F4A9})).bytes, bytes_of({0xB0, 0xA1, 0x41}) + "&#128169;");
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_BIG5, html, cp_utf8({0x4E00, 0x41, 0x1F4A9})).bytes, bytes_of({0xA4, 0x40, 0x41}) + "&#128169;");
  // Big5: the code points that occur twice: the last pointer for six of them, the first for the others ("There are other duplicate code points").
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_BIG5, html, cp_utf8({0x2550})).bytes, bytes_of({0xF9, 0xF9}));
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_BIG5, html, cp_utf8({0x5341})).bytes, bytes_of({0xA4, 0x51})); // pointers 5287 (0xA2 0xCC) and 5512: the last
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_BIG5, html, cp_utf8({0x4E8C})).bytes, bytes_of({0xA4, 0x47})); // an ordinary character
  // GBK against gb18030: the euro sign, U+E5E5 and a code point only the four-byte form has.
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_GBK, html, cp_utf8({0x20AC})).bytes, bytes_of({0x80}));
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_GB18030, html, cp_utf8({0x20AC})).bytes, bytes_of({0xA2, 0xE3}));
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_GBK, html, cp_utf8({0xE5E5})).bytes, "&#58853;");
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_GB18030, html, cp_utf8({0xE5E5})).bytes, "&#58853;");
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_GBK, html, cp_utf8({0x10000})).bytes, "&#65536;");
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_GB18030, html, cp_utf8({0x10000, 0x10FFFF, 0x80})).bytes,
      bytes_of({0x90, 0x30, 0x81, 0x30, 0xE3, 0x32, 0x9A, 0x35, 0x81, 0x30, 0x81, 0x30}));
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_GB18030, html, cp_utf8({0xE7C7})).bytes, bytes_of({0x81, 0x35, 0xF4, 0x37}));
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_GB18030, html, cp_utf8({0xE78D})).bytes, bytes_of({0xA6, 0xD9}));
  // The fatal mode of a stateless encoder: the code point and its offset.
  const Out f = encode_whole(GTEXT_ENCODING_SHIFT_JIS, encoder_options(GTEXT_ENCODER_FATAL), "ab" + cp_utf8({0xAC00}));
  EXPECT_EQ(f.status, GTEXT_ENCODING_E_UNENCODABLE);
  EXPECT_EQ(f.error_code_point, 0xAC00u);
  EXPECT_EQ(f.error_offset, 2u);
  EXPECT_EQ(f.bytes, "ab");
}

// ---- chunks ---------------------------------------------------------------------------------------------------------------------------

TEST(EncodingCjkChunks, EveryCutAndEveryPairOfCutsOfEveryShortInputGivesTheWholeInputsAnswer) {
  const std::vector<unsigned char> alphabet = {0x1B, 0x24, 0x28, 0x30, 0x39, 0x41, 0x42, 0x4A, 0x62, 0x81, 0x8E, 0x8F, 0xA1, 0xFE, 0xFF};
  const size_t length = reduced() ? 3 : 4;
  for (GTEXT_Encoding enc : multi_byte_encodings()) {
    size_t total = 1;
    for (size_t i = 0; i < length; i++) {
      total *= alphabet.size();
    }
    for (GTEXT_Decoder_Error_Mode mode : {GTEXT_DECODER_REPLACEMENT, GTEXT_DECODER_FATAL}) {
      const GTEXT_Decoder_Options o = decoder_options(mode);
      for (size_t v = 0; v < total; v++) {
        std::string in(length, '\0');
        size_t x = v;
        for (size_t k = length; k > 0; k--) {
          in[k - 1] = char(alphabet[x % alphabet.size()]);
          x /= alphabet.size();
        }
        const Out whole = decode_whole(enc, o, in);
        for (size_t a = 0; a <= length; a++) {
          for (size_t b = a; b <= length; b++) {
            if (!(decode_cut(enc, o, in, {a, b}) == whole)) {
              FAIL() << name_of(enc) << " " << hex(in) << " cut at " << a << " and " << b << ": " << show(decode_cut(enc, o, in, {a, b})) << " but whole "
                     << show(whole);
            }
          }
        }
      }
    }
  }
}

TEST(EncodingCjkChunks, EncodersCutAtEveryByteOfTheirUtf8) {
  const std::vector<uint32_t> alphabet = {0x41, 0x5C, 0xA5, 0x203E, 0x2212, 0xFF71, 0x3042, 0x4E9C, 0xE9, 0x20AC, 0x10000, 0x1F4A9, 0xE5E5, 0x0E};
  for (GTEXT_Encoding enc : multi_byte_encodings()) {
    for (GTEXT_Encoder_Error_Mode mode : {GTEXT_ENCODER_HTML, GTEXT_ENCODER_FATAL}) {
      const GTEXT_Encoder_Options o = encoder_options(mode);
      for (size_t length = 1; length <= 3; length++) {
        size_t total = 1;
        for (size_t i = 0; i < length; i++) {
          total *= alphabet.size();
        }
        for (size_t v = 0; v < total; v++) {
          std::string in;
          size_t x = v;
          for (size_t k = 0; k < length; k++) {
            in += utf8_of(alphabet[x % alphabet.size()]);
            x /= alphabet.size();
          }
          const Out whole = encode_whole(enc, o, in);
          for (size_t a = 0; a <= in.size(); a++) {
            for (size_t b = a; b <= in.size(); b += 1 + (in.size() > 6)) {
              if (!(encode_cut(enc, o, in, {a, b}) == whole)) {
                FAIL() << name_of(enc) << " " << hex(in) << " cut at " << a << " and " << b << ": " << show(encode_cut(enc, o, in, {a, b}))
                       << " but whole " << show(whole);
              }
            }
          }
          if (!(encode_cut(enc, o, in, [&] {
                  std::vector<size_t> all;
                  for (size_t i = 1; i < in.size(); i++) {
                    all.push_back(i);
                  }
                  return all;
                }()) == whole)) {
            FAIL() << name_of(enc) << " " << hex(in) << " byte at a time";
          }
        }
      }
    }
  }
}

TEST(EncodingCjkChunks, LongRandomStreamsInRandomPiecesDecodeAndEncodeAsOneCall) {
  Rng rng(0xC1C);
  for (GTEXT_Encoding enc : multi_byte_encodings()) {
    for (int round = 0; round < (reduced() ? 40 : 400); round++) {
      std::string in;
      const size_t len = rng.below(3000);
      while (in.size() < len) {
        if (rng.below(4) == 0) {
          in += char(rng.below(256));
        } else if (rng.below(2) == 0) {
          in += char(0x20 + rng.below(0x5F));
        } else {
          const uint32_t cp = rng.below(2) ? uint32_t(0x3040 + rng.below(0x1000)) : uint32_t(0xAC00 + rng.below(0x400));
          in += model_encode(enc, {cp}, true).bytes;
        }
      }
      for (GTEXT_Decoder_Error_Mode mode : {GTEXT_DECODER_REPLACEMENT, GTEXT_DECODER_FATAL}) {
        const GTEXT_Decoder_Options o = decoder_options(mode);
        std::vector<size_t> cuts;
        for (size_t p = 1; p < in.size(); p += 1 + rng.below(40)) {
          cuts.push_back(p);
        }
        const Out whole = decode_whole(enc, o, in);
        ASSERT_TRUE(decode_cut(enc, o, in, cuts) == whole) << name_of(enc);
        ASSERT_TRUE(decode_bytewise(enc, o, in) == whole) << name_of(enc);
      }
      // The same text through the encoder, in pieces that fall inside UTF-8 characters.
      const std::string text = decode_whole(enc, in).bytes;
      const GTEXT_Encoder_Options eo = encoder_options(GTEXT_ENCODER_HTML);
      std::vector<size_t> cuts;
      for (size_t p = 1; p < text.size(); p += 1 + rng.below(30)) {
        cuts.push_back(p);
      }
      ASSERT_TRUE(encode_cut(enc, eo, text, cuts) == encode_whole(enc, eo, text)) << name_of(enc);
    }
  }
}

// ---- limits ---------------------------------------------------------------------------------------------------------------------------

namespace {

// What a limit of `limit` bytes must leave in the sink: the longest prefix of `full` that ends at one of the boundaries.
std::string longest_prefix(const std::string & full, const std::vector<size_t> & boundaries, size_t limit) {
  size_t best = 0;
  for (size_t b : boundaries) {
    if (b <= limit) {
      best = b;
    }
  }
  return full.substr(0, best);
}

} // namespace

TEST(EncodingCjkLimits, TheDecodersLimitAtEveryValueCutsOnlyBetweenWhatIsWrittenTogether) {
  // Big5's two-code-point sequences are written together; a replacement character is one code point; the rest are single code points.
  const std::vector<std::pair<GTEXT_Encoding, std::string>> inputs = {
      {GTEXT_ENCODING_BIG5, bytes_of({0x41, 0x88, 0x62, 0xA4, 0x40, 0x88, 0xA5, 0xFF, 0x88, 0x64, 0x42})},
      {GTEXT_ENCODING_GB18030, bytes_of({0x41, 0x81, 0x30, 0x81, 0x30, 0xA1, 0xA1, 0x90, 0x30, 0x81, 0x30, 0x80, 0xFF})},
      {GTEXT_ENCODING_GBK, bytes_of({0x41, 0x81, 0x30, 0x81, 0x30, 0xA1, 0xA1, 0x90, 0x30, 0x81, 0x30, 0x80, 0xFF})},
      {GTEXT_ENCODING_SHIFT_JIS, bytes_of({0x41, 0x83, 0x41, 0xB1, 0x81, 0x20, 0x80, 0xF0, 0x40})},
      {GTEXT_ENCODING_EUC_JP, bytes_of({0x41, 0xA4, 0xA2, 0x8E, 0xB1, 0x8F, 0xB0, 0xA1, 0xA4})},
      {GTEXT_ENCODING_EUC_KR, bytes_of({0x41, 0xB0, 0xA1, 0x81, 0x41, 0xC9, 0xFE, 0xFF})},
      {GTEXT_ENCODING_ISO_2022_JP, bytes_of({0x41, 0x1B, 0x24, 0x42, 0x24, 0x22, 0x24, 0x24, 0x1B, 0x28, 0x4A, 0x5C, 0x1B, 0x28, 0x49, 0x31, 0x1B, 0x28, 0x42, 0x42})},
  };
  for (const auto & [enc, in] : inputs) {
    const ModelDecoded m = model_decode(enc, (const unsigned char *)in.data(), in.size(), false);
    const std::string full = utf8_of_cps(m.cps);
    std::vector<size_t> boundaries = {0};
    for (uint32_t upto : m.steps) {
      boundaries.push_back(utf8_of_cps(std::vector<uint32_t>(m.cps.begin(), m.cps.begin() + upto)).size());
    }
    for (size_t limit = 0; limit <= full.size() + 2; limit++) {
      GTEXT_Decoder_Options o = decoder_options(GTEXT_DECODER_REPLACEMENT);
      o.max_output_bytes = limit == 0 ? 1 : limit; // (0 is the default, never unlimited)
      const Out got = decode_whole(enc, o, in);
      const size_t eff = limit == 0 ? 1 : limit;
      if (eff >= full.size()) {
        EXPECT_EQ(got.status, GTEXT_ENCODING_OK) << name_of(enc) << " limit " << limit;
        EXPECT_EQ(got.bytes, full) << name_of(enc) << " limit " << limit;
      } else {
        EXPECT_EQ(got.status, GTEXT_ENCODING_E_LIMIT_OUTPUT) << name_of(enc) << " limit " << limit;
        EXPECT_EQ(got.bytes, longest_prefix(full, boundaries, eff)) << name_of(enc) << " limit " << limit;
      }
    }
  }
}

TEST(EncodingCjkLimits, TheEncodersLimitAtEveryValueCutsOnlyBetweenCharactersReferencesAndEscapeSequences) {
  const std::vector<std::vector<uint32_t>> texts = {
      {0x41, 0x3042, 0xFF71, 0xE9, 0xA5, 0x20AC, 0x4E9C, 0x42, 0x1F4A9, 0x203E, 0x0E, 0x30A2},
      {0x3042, 0x3044, 0xE9, 0x3046, 0x41, 0xA5, 0xA5, 0x5C, 0x4E9C},
      {0xAC00, 0x41, 0x10000, 0x80, 0xE5E5, 0x2550, 0xCA},
  };
  for (GTEXT_Encoding enc : multi_byte_encodings()) {
    for (const auto & cps : texts) {
      const ModelEncodeResult m = model_encode(enc, cps, true);
      std::vector<size_t> boundaries = {0};
      boundaries.insert(boundaries.end(), m.atoms.begin(), m.atoms.end());
      std::string in;
      for (uint32_t cp : cps) {
        in += utf8_of(cp);
      }
      for (size_t limit = 1; limit <= m.bytes.size() + 2; limit++) {
        GTEXT_Encoder_Options o = encoder_options(GTEXT_ENCODER_HTML);
        o.max_output_bytes = limit;
        const Out got = encode_whole(enc, o, in);
        if (limit >= m.bytes.size()) {
          EXPECT_EQ(got.status, GTEXT_ENCODING_OK) << name_of(enc) << " limit " << limit;
          EXPECT_EQ(got.bytes, m.bytes) << name_of(enc) << " limit " << limit;
        } else {
          EXPECT_EQ(got.status, GTEXT_ENCODING_E_LIMIT_OUTPUT) << name_of(enc) << " limit " << limit << " of " << hex(m.bytes);
          EXPECT_EQ(got.bytes, longest_prefix(m.bytes, boundaries, limit)) << name_of(enc) << " limit " << limit;
        }
      }
    }
  }
}

// ---- sinks and memory ---------------------------------------------------------------------------------------------------------------------

TEST(EncodingCjkSink, ARefusalIsAWriteErrorAndTheCodecWorksAfterwards) {
  for (GTEXT_Encoding enc : multi_byte_encodings()) {
    std::string in;
    for (int i = 0; i < 3000; i++) {
      in += model_encode(enc, {uint32_t(0x3042 + i % 80)}, true).bytes;
    }
    GTEXT_Decoder * d = nullptr;
    ASSERT_EQ(gtext_decoder_new(enc, nullptr, &d), GTEXT_ENCODING_OK);
    for (size_t refuse : {size_t(0), size_t(1)}) {
      Collect c;
      c.refuse_after = refuse;
      GTEXT_Encoding_Sink sink = sink_of(c);
      EXPECT_EQ(gtext_decoder_decode(d, in.data(), in.size(), true, &sink, nullptr), GTEXT_ENCODING_E_WRITE) << name_of(enc);
    }
    Collect ok;
    GTEXT_Encoding_Sink sink = sink_of(ok);
    EXPECT_EQ(gtext_decoder_decode(d, in.data(), in.size(), true, &sink, nullptr), GTEXT_ENCODING_OK) << name_of(enc);
    EXPECT_EQ(ok.bytes, decode_whole(enc, in).bytes);
    gtext_decoder_free(d);
  }
}

TEST(EncodingCjkMemory, EveryAllocationOfTheCodecsMayFailAndAStreamAllocatesNothing) {
  FaultAllocator fa;
  for (GTEXT_Encoding enc : multi_byte_encodings()) {
    for (size_t size : {size_t(10), size_t(3000), size_t(60000)}) {
      std::string in;
      for (size_t i = 0; in.size() < size; i++) {
        in += model_encode(enc, {uint32_t(0x3042 + i % 80)}, true).bytes + "a";
      }
      const Out reference = decode_whole(enc, in);
      GTEXT_Decoder_Options o = gtext_decoder_options_default();
      o.allocator = &fa.alloc;
      size_t calls = 0;
      for (size_t n = 1; n < 100; n++) {
        fa.arm(n);
        char * out = nullptr;
        size_t len = 0;
        GTEXT_Decoder_Result r;
        const GTEXT_Encoding_Status s = gtext_encoding_decode_buffer(enc, &o, in.data(), in.size(), &out, &len, &r);
        const bool failed = fa.failed;
        fa.disarm();
        if (s == GTEXT_ENCODING_OK) {
          EXPECT_FALSE(failed);
          EXPECT_EQ(std::string(out, len), reference.bytes);
          gtext_encoding_buffer_free(out);
        } else {
          EXPECT_EQ(s, GTEXT_ENCODING_E_OOM);
          EXPECT_EQ(out, nullptr);
        }
        EXPECT_TRUE(fa.live.empty()) << "leak after failing allocation " << n;
        EXPECT_EQ(fa.bad_frees, 0u);
        if (!failed) {
          calls = n - 1;
          break;
        }
      }
      EXPECT_GE(calls, 2u) << name_of(enc);
      // The encoder, the same way, from the text the decoder made.
      GTEXT_Encoder_Options eo = encoder_options(GTEXT_ENCODER_HTML);
      const Out ref_enc = encode_whole(enc, eo, reference.bytes);
      eo.allocator = &fa.alloc;
      for (size_t n = 1; n < 100; n++) {
        fa.arm(n);
        char * out = nullptr;
        size_t len = 0;
        const GTEXT_Encoding_Status s = gtext_encoding_encode_buffer(enc, &eo, reference.bytes.data(), reference.bytes.size(), &out, &len, nullptr);
        const bool failed = fa.failed;
        fa.disarm();
        if (s == GTEXT_ENCODING_OK) {
          EXPECT_EQ(std::string(out, len), ref_enc.bytes);
          gtext_encoding_buffer_free(out);
        } else {
          EXPECT_EQ(s, GTEXT_ENCODING_E_OOM);
        }
        EXPECT_TRUE(fa.live.empty());
        if (!failed) {
          break;
        }
      }
      // A stream makes no allocation of its own.
      GTEXT_Decoder * d = nullptr;
      fa.arm(100000);
      ASSERT_EQ(gtext_decoder_new(enc, &o, &d), GTEXT_ENCODING_OK);
      const size_t after_new = fa.calls;
      Collect c;
      GTEXT_Encoding_Sink sink = sink_of(c);
      ASSERT_EQ(gtext_decoder_decode(d, in.data(), in.size(), true, &sink, nullptr), GTEXT_ENCODING_OK);
      EXPECT_EQ(fa.calls, after_new) << "a decode allocated";
      gtext_decoder_free(d);
      GTEXT_Encoder * e = nullptr;
      ASSERT_EQ(gtext_encoder_new(enc, &eo, &e), GTEXT_ENCODING_OK);
      const size_t after_enc = fa.calls;
      Collect c2;
      GTEXT_Encoding_Sink sink2 = sink_of(c2);
      ASSERT_EQ(gtext_encoder_encode(e, reference.bytes.data(), reference.bytes.size(), true, &sink2, nullptr), GTEXT_ENCODING_OK);
      EXPECT_EQ(fa.calls, after_enc) << "an encode allocated";
      gtext_encoder_free(e);
      fa.disarm();
      EXPECT_TRUE(fa.live.empty());
    }
  }
}

// ---- work ----------------------------------------------------------------------------------------------------------------------------------

TEST(EncodingCjkWork, TheStepsAreAtMostFourPerByteOfTheWorstInputsOfEachDecoder) {
  // The inputs that make the restore rules re-read the most: a four-byte form broken at its last byte, an escape sequence broken at its last byte.
  const std::map<GTEXT_Encoding, std::string> worst = {
      {GTEXT_ENCODING_GB18030, bytes_of({0x81, 0x30, 0x81, 0x41})},
      {GTEXT_ENCODING_GBK, bytes_of({0x81, 0x30, 0x81, 0x41})},
      {GTEXT_ENCODING_ISO_2022_JP, bytes_of({0x1B, 0x24, 0x41})},
      {GTEXT_ENCODING_BIG5, bytes_of({0x81, 0x20})},
      {GTEXT_ENCODING_EUC_JP, bytes_of({0x8F, 0xA1, 0x20})},
      {GTEXT_ENCODING_SHIFT_JIS, bytes_of({0x81, 0x20})},
      {GTEXT_ENCODING_EUC_KR, bytes_of({0x81, 0x20})},
  };
  for (const auto & [enc, unit] : worst) {
    std::string in;
    while (in.size() < 100000) {
      in += unit;
    }
    GTEXT_Decoder * d = nullptr;
    ASSERT_EQ(gtext_decoder_new(enc, nullptr, &d), GTEXT_ENCODING_OK);
    Collect c;
    GTEXT_Encoding_Sink sink = sink_of(c);
    GTEXT_Decoder_Result r;
    ASSERT_EQ(gtext_decoder_decode(d, in.data(), in.size(), true, &sink, &r), GTEXT_ENCODING_OK);
    EXPECT_LE(r.steps, 4 * in.size()) << name_of(enc);
    gtext_decoder_free(d);
  }
}

// ---- the state of ISO-2022-JP across a failure ----------------------------------------------------------------------------------------

TEST(EncodingCjkDecode, Iso2022JpGoesOnInItsModeAfterAFatalErrorInACallThatDidNotFlush) {
  // The standard's own test (web-platform-tests, textdecoder-mistakes.any.js, "fatal stream: iso-2022-jp"): "This is the only decoder which does not clear
  // internal state before throwing in stream mode (non-EOF throws). So the internal state of this decoder can legitimately persist after an error was thrown."
  GTEXT_Decoder * d = nullptr;
  GTEXT_Decoder_Options o = decoder_options(GTEXT_DECODER_FATAL);
  ASSERT_EQ(gtext_decoder_new(GTEXT_ENCODING_ISO_2022_JP, &o, &d), GTEXT_ENCODING_OK);
  auto run = [&](const std::string & in, bool flush, std::string * out, GTEXT_Decoder_Result * r) {
    Collect c;
    GTEXT_Encoding_Sink sink = sink_of(c);
    const GTEXT_Encoding_Status s = gtext_decoder_decode(d, in.data(), in.size(), flush, &sink, r);
    *out = c.bytes;
    return s;
  };
  std::string out;
  GTEXT_Decoder_Result r;
  ASSERT_EQ(run("\x7e", true, &out, &r), GTEXT_ENCODING_OK);
  EXPECT_EQ(out, "~");
  // Switch to Roman, then an error: the call fails, and the decoder is still in the Roman state.
  EXPECT_EQ(run(bytes_of({0x1B, 0x28, 0x4A, 0xFF}), false, &out, &r), GTEXT_ENCODING_E_MALFORMED);
  EXPECT_EQ(r.error_offset, 3u);
  ASSERT_EQ(run("\x7e", true, &out, &r), GTEXT_ENCODING_OK);
  EXPECT_EQ(out, cp_utf8({0x203E})); // not "~"
  // ... and a flush starts over: the next stream is ASCII again.
  ASSERT_EQ(run("\x7e", true, &out, &r), GTEXT_ENCODING_OK);
  EXPECT_EQ(out, "~");
  // Katakana.
  ASSERT_EQ(run("B", true, &out, &r), GTEXT_ENCODING_OK);
  EXPECT_EQ(out, "B");
  EXPECT_EQ(run(bytes_of({0x1B, 0x28, 0x49, 0xFF}), false, &out, &r), GTEXT_ENCODING_E_MALFORMED);
  ASSERT_EQ(run("B", true, &out, &r), GTEXT_ENCODING_OK);
  EXPECT_EQ(out, cp_utf8({0xFF82}));
  // A failure in a call that flushes ends the stream: the next call is a new one, in ASCII.
  EXPECT_EQ(run(bytes_of({0x1B, 0x28, 0x4A, 0xFF}), true, &out, &r), GTEXT_ENCODING_E_MALFORMED);
  ASSERT_EQ(run("\x7e", true, &out, &r), GTEXT_ENCODING_OK);
  EXPECT_EQ(out, "~");
  // A failure that is not an error of the data (an output limit, a refusing sink) abandons the stream: ASCII again.
  gtext_decoder_free(d);
  GTEXT_Decoder_Options limited = decoder_options(GTEXT_DECODER_REPLACEMENT);
  limited.max_output_bytes = 4;
  ASSERT_EQ(gtext_decoder_new(GTEXT_ENCODING_ISO_2022_JP, &limited, &d), GTEXT_ENCODING_OK);
  EXPECT_EQ(run(bytes_of({0x1B, 0x28, 0x4A, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66}), false, &out, &r), GTEXT_ENCODING_E_LIMIT_OUTPUT);
  ASSERT_EQ(run("\x7e", true, &out, &r), GTEXT_ENCODING_OK);
  EXPECT_EQ(out, "~");
  gtext_decoder_free(d);
  // The other decoders do start over after a fatal error: `replacement`, whose handler would otherwise find its one error spent.
  d = nullptr;
  ASSERT_EQ(gtext_decoder_new(GTEXT_ENCODING_REPLACEMENT, &o, &d), GTEXT_ENCODING_OK);
  EXPECT_EQ(run("a", false, &out, &r), GTEXT_ENCODING_E_MALFORMED);
  EXPECT_EQ(run("a", false, &out, &r), GTEXT_ENCODING_E_MALFORMED);
  gtext_decoder_free(d);
}

// What of ISO-2022-JP's decoder survives a fatal error in a call that did not flush: the mode (the decoder output state) and nothing else. The served handler
// (12.2.1) is switched on the decoder state and carries three more variables: the leading byte, the output flag and the output state itself. Each case runs a
// first call that fails with a given state and a second call (a flush) that tells which state the decoder is in.
TEST(EncodingCjkDecode, Iso2022JpWhichStateSurvivesAFatalErrorInACallThatDidNotFlush) {
  struct Case {
    const char * name;
    std::vector<int> first;     // the bytes of the call that fails (fatal mode, no flush) ...
    size_t error_offset;        // ... at this offset ...
    std::vector<int> second;    // ... and the bytes of the call after it (a flush) ...
    std::vector<uint32_t> want; // ... which decode to these code points, the decoder having begun in the mode the failure left, with nothing else of the old state.
    int mode;                   // the model's name for the mode the failure left (0 ASCII, 1 Roman, 2 katakana, 3 JIS X 0208)
  };
  const std::vector<Case> cases = {
    // The mode states: the error is in the state, and the state is the mode.
    {"ASCII: a byte above 0x7F", {0x41, 0xFF}, 1, {0x7E, 0x5C}, {0x7E, 0x5C}, 0},
    {"ASCII: 0x0E", {0x0E}, 0, {0x7E}, {0x7E}, 0},
    {"Roman: a byte above 0x7F", {0x1B, 0x28, 0x4A, 0xFF}, 3, {0x7E, 0x5C}, {0x203E, 0xA5}, 1},
    {"Roman: 0x0F", {0x1B, 0x28, 0x4A, 0x0F}, 3, {0x5C}, {0xA5}, 1},
    {"katakana: a byte outside 0x21 to 0x5F", {0x1B, 0x28, 0x49, 0x20}, 3, {0x42}, {0xFF82}, 2},
    {"jis0208, between characters: a byte above 0x7F", {0x1B, 0x24, 0x42, 0x21, 0x21, 0xFF}, 5, {0x21, 0x21, 0x21, 0x21}, {0x3000, 0x3000}, 3},
    {"jis0208, between characters: the other escape for it (ESC $ @)", {0x1B, 0x24, 0x40, 0x80}, 3, {0x21, 0x21}, {0x3000}, 3},
    // The trailing byte state: every error from it returns to the leading byte state (the mode) before it returns, whatever the byte.
    {"jis0208, after a leading byte: a trailing byte above 0x7E", {0x1B, 0x24, 0x42, 0x21, 0xFF}, 4, {0x21, 0x21, 0x21, 0x21}, {0x3000, 0x3000}, 3},
    {"jis0208, after a leading byte: a trailing byte with no code point", {0x1B, 0x24, 0x42, 0x7E, 0x7E}, 4, {0x21, 0x21, 0x21, 0x21}, {0x3000, 0x3000}, 3},
    // ... except ESC, which the standard answers with "set the decoder state to escape start and return error": a half-read escape sequence. It must not be
    // finished by the next stream.
    {"jis0208, after a leading byte: ESC (the state left is escape start)", {0x1B, 0x24, 0x42, 0x21, 0x1B}, 4, {0x21, 0x21}, {0x3000}, 3},
    {"Roman, an escape start followed by ESC", {0x1B, 0x28, 0x4A, 0x1B, 0x1B}, 4, {0x7E}, {0x203E}, 1},
    // The escape states: an escape sequence that is not one is an error with the state returned to the output state, and the leading byte cleared.
    {"an escape start with a byte that begins no sequence", {0x1B, 0x28, 0x4A, 0x1B, 0x41}, 4, {0x7E}, {0x203E}, 1},
    {"an escape that selects nothing (ESC ( A)", {0x1B, 0x24, 0x42, 0x1B, 0x28, 0x41}, 5, {0x21, 0x21}, {0x3000}, 3},
    {"an escape that selects nothing (ESC $ A)", {0x1B, 0x28, 0x49, 0x1B, 0x24, 0x41}, 5, {0x42}, {0xFF82}, 2},
    // The output flag: two escape sequences with nothing between are an error ("Return continue, if output is false, and error otherwise"); the flag the second
    // leaves set is not the next stream's, whose first escape is not "following" anything.
    {"two escape sequences with nothing between", {0x1B, 0x28, 0x4A, 0x1B, 0x28, 0x4A}, 5, {0x1B, 0x28, 0x42, 0x41}, {0x41}, 1},
    {"two escape sequences, the second selecting jis0208", {0x1B, 0x28, 0x42, 0x1B, 0x24, 0x42}, 5, {0x1B, 0x28, 0x42, 0x41}, {0x41}, 3},
  };
  for (const Case & c : cases) {
    GTEXT_Decoder * d = nullptr;
    GTEXT_Decoder_Options o = decoder_options(GTEXT_DECODER_FATAL);
    ASSERT_EQ(gtext_decoder_new(GTEXT_ENCODING_ISO_2022_JP, &o, &d), GTEXT_ENCODING_OK);
    auto run = [&](const std::vector<int> & in, bool flush, std::string * out, GTEXT_Decoder_Result * r) {
      std::string bytes;
      for (int b : in) {
        bytes += char(b);
      }
      Collect col;
      GTEXT_Encoding_Sink sink = sink_of(col);
      const GTEXT_Encoding_Status st = gtext_decoder_decode(d, bytes.data(), bytes.size(), flush, &sink, r);
      *out = col.bytes;
      return st;
    };
    std::string out;
    GTEXT_Decoder_Result r;
    ASSERT_EQ(run(c.first, false, &out, &r), GTEXT_ENCODING_E_MALFORMED) << c.name;
    EXPECT_EQ(r.error_offset, c.error_offset) << c.name;
    // The model (the standard's handler, with its state kept by the caller) says which mode the failure left; the library must continue in that mode and in
    // no other state.
    std::string first;
    for (int b : c.first) {
      first += char(b);
    }
    const ModelDecoded mf = model_decode(GTEXT_ENCODING_ISO_2022_JP, reinterpret_cast<const unsigned char *>(first.data()), first.size(), true);
    ASSERT_TRUE(mf.failed) << c.name;
    EXPECT_EQ(mf.iso_mode, c.mode) << c.name;
    ASSERT_EQ(run(c.second, true, &out, &r), GTEXT_ENCODING_OK) << c.name;
    EXPECT_EQ(out, utf8_of_cps(c.want)) << c.name;
    // (the model begun in that mode says the same)
    std::string second;
    for (int b : c.second) {
      second += char(b);
    }
    const ModelDecoded ms = model_decode(GTEXT_ENCODING_ISO_2022_JP, reinterpret_cast<const unsigned char *>(second.data()), second.size(), true, c.mode);
    EXPECT_FALSE(ms.failed) << c.name;
    EXPECT_EQ(utf8_of_cps(ms.cps), utf8_of_cps(c.want)) << c.name;
    // The offsets and the position start over: the next failure is counted from its own call's first byte.
    ASSERT_EQ(run(c.first, false, &out, &r), GTEXT_ENCODING_E_MALFORMED) << c.name << " (again)";
    gtext_decoder_free(d);
  }
}

TEST(EncodingCjkDecode, Iso2022JpModeIsClearedByAResetAFlushAndAByteOrderMark) {
  const std::string roman_error = bytes_of({0x1B, 0x28, 0x4A, 0xFF});
  GTEXT_Decoder_Options o = decoder_options(GTEXT_DECODER_FATAL);
  auto probe = [&](GTEXT_Decoder * d) {
    Collect col;
    GTEXT_Encoding_Sink sink = sink_of(col);
    GTEXT_Decoder_Result r;
    EXPECT_EQ(gtext_decoder_decode(d, "\x7e", 1, true, &sink, &r), GTEXT_ENCODING_OK);
    return col.bytes;
  };
  auto fail = [&](GTEXT_Decoder * d, const std::string & in, bool flush) {
    Collect col;
    GTEXT_Encoding_Sink sink = sink_of(col);
    GTEXT_Decoder_Result r;
    return gtext_decoder_decode(d, in.data(), in.size(), flush, &sink, &r);
  };
  GTEXT_Decoder * d = nullptr;
  ASSERT_EQ(gtext_decoder_new(GTEXT_ENCODING_ISO_2022_JP, &o, &d), GTEXT_ENCODING_OK);
  // The mode survives ...
  EXPECT_EQ(fail(d, roman_error, false), GTEXT_ENCODING_E_MALFORMED);
  EXPECT_EQ(probe(d), cp_utf8({0x203E}));
  // ... but gtext_decoder_reset() clears all of it.
  EXPECT_EQ(fail(d, roman_error, false), GTEXT_ENCODING_E_MALFORMED);
  gtext_decoder_reset(d);
  EXPECT_EQ(probe(d), "~");
  // A successful non-flush call and then the end of the stream: the mode does not survive the flush (nothing failed).
  Collect col;
  GTEXT_Encoding_Sink sink = sink_of(col);
  const std::string roman = bytes_of({0x1B, 0x28, 0x4A});
  ASSERT_EQ(gtext_decoder_decode(d, roman.data(), roman.size(), false, &sink, nullptr), GTEXT_ENCODING_OK);
  ASSERT_EQ(gtext_decoder_decode(d, "", 0, true, &sink, nullptr), GTEXT_ENCODING_OK);
  EXPECT_EQ(probe(d), "~");
  // A fatal error in a replacement-mode decoder cannot happen; the replacement-mode decoder starts over after anything.
  gtext_decoder_free(d);
  GTEXT_Decoder_Options rep = decoder_options(GTEXT_DECODER_REPLACEMENT);
  ASSERT_EQ(gtext_decoder_new(GTEXT_ENCODING_ISO_2022_JP, &rep, &d), GTEXT_ENCODING_OK);
  ASSERT_EQ(gtext_decoder_decode(d, roman_error.data(), roman_error.size(), false, &sink, nullptr), GTEXT_ENCODING_OK);
  ASSERT_EQ(gtext_decoder_decode(d, "", 0, true, &sink, nullptr), GTEXT_ENCODING_OK);
  EXPECT_EQ(probe(d), "~");
  gtext_decoder_free(d);
  // With byte order mark sniffing, a stream that began with a mark was not decoded as ISO-2022-JP and leaves no mode of it; one that did not, does.
  GTEXT_Decoder_Options sn = decoder_options(GTEXT_DECODER_FATAL);
  sn.bom_sniffing = true;
  ASSERT_EQ(gtext_decoder_new(GTEXT_ENCODING_ISO_2022_JP, &sn, &d), GTEXT_ENCODING_OK);
  EXPECT_EQ(fail(d, roman_error, false), GTEXT_ENCODING_E_MALFORMED);
  EXPECT_EQ(probe(d), cp_utf8({0x203E}));
  EXPECT_EQ(fail(d, bytes_of({0xEF, 0xBB, 0xBF, 0xFF}), false), GTEXT_ENCODING_E_MALFORMED); // UTF-8 after a mark: an error of UTF-8's
  EXPECT_EQ(probe(d), "~");
  gtext_decoder_free(d);
}

TEST(EncodingCjkEncode, TheFormSubmissionOfThePagesOfTheSuiteWithAStatefulEncoder) {
  // web-platform-tests iso2022jp-encode-form-errors-stateful.html (a browser page; its vector is here): unencodable characters between ISO-2022-JP's modes.
  const std::string text = cp_utf8({'A', 'B', 'C', '~', 0xA4, 0x2022, 0x2605, 0x661F, 0x1F31F, 0x661F, 0x2605, 0x2022, 0xA4, '~', 'X', 'Y', 'Z'});
  const std::string want = "ABC~&#164;&#8226;" + bytes_of({0x1B, '$', 'B', '!', 'z', '@', '1', 0x1B, '(', 'B'}) + "&#127775;"
      + bytes_of({0x1B, '$', 'B', '@', '1', '!', 'z', 0x1B, '(', 'B'}) + "&#8226;&#164;~XYZ";
  EXPECT_EQ(encode_whole(GTEXT_ENCODING_ISO_2022_JP, encoder_options(GTEXT_ENCODER_HTML), text).bytes, want);
  EXPECT_EQ(model_encode(GTEXT_ENCODING_ISO_2022_JP, cps_of_utf8(text), true).bytes, want);
}

TEST(EncodingCjkWork, RunsOfAsciiAreCopiedWholeExceptByIso2022Jp) {
  // Between characters an ASCII byte is its own code point, so a decoder copies a run of them without a step of its handler (the one step is the
  // end of the stream); ISO-2022-JP, whose 0x0E, 0x0F and 0x1B are not ASCII characters, takes a step for each.
  const std::string run(5000, 'x');
  for (GTEXT_Encoding enc : multi_byte_encodings()) {
    GTEXT_Decoder * d = nullptr;
    ASSERT_EQ(gtext_decoder_new(enc, nullptr, &d), GTEXT_ENCODING_OK);
    Collect c;
    GTEXT_Encoding_Sink sink = sink_of(c);
    GTEXT_Decoder_Result r;
    ASSERT_EQ(gtext_decoder_decode(d, run.data(), run.size(), true, &sink, &r), GTEXT_ENCODING_OK);
    EXPECT_EQ(c.bytes, run);
    EXPECT_EQ(r.steps, enc == GTEXT_ENCODING_ISO_2022_JP ? run.size() + 1 : size_t(1)) << name_of(enc);
    gtext_decoder_free(d);
  }
}
