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
 * The contract of the Encoding module's calls beyond what the codecs compute: which byte of the input a failure is reported at, what an argument
 * error does to a stream, the offsets of an error after a byte order mark, the work done (counted, not timed), the label key, and which encodings have an
 * encoder.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "encoding_test_support.h"

#include <functional>

using namespace enctest;

namespace {

struct Outcome {
  GTEXT_Encoding_Status status = GTEXT_ENCODING_OK;
  size_t input_bytes = 0;
  size_t output_bytes = 0;
  std::string bytes;
};

Outcome decode_once(GTEXT_Encoding e, GTEXT_Decoder_Options o, const std::string & in, size_t refuse_after = SIZE_MAX) {
  GTEXT_Decoder * d = nullptr;
  EXPECT_EQ(gtext_decoder_new(e, &o, &d), GTEXT_ENCODING_OK);
  Collect c;
  c.refuse_after = refuse_after;
  GTEXT_Encoding_Sink sink = sink_of(c);
  GTEXT_Decoder_Result r;
  Outcome run;
  run.status = gtext_decoder_decode(d, in.data(), in.size(), true, &sink, &r);
  run.input_bytes = r.input_bytes;
  run.output_bytes = r.output_bytes;
  run.bytes = c.bytes;
  gtext_decoder_free(d);
  return run;
}

Outcome encode_once(GTEXT_Encoding e, GTEXT_Encoder_Options o, const std::string & in, size_t refuse_after = SIZE_MAX) {
  GTEXT_Encoder * x = nullptr;
  EXPECT_EQ(gtext_encoder_new(e, &o, &x), GTEXT_ENCODING_OK);
  Collect c;
  c.refuse_after = refuse_after;
  GTEXT_Encoding_Sink sink = sink_of(c);
  GTEXT_Encoder_Result r;
  Outcome run;
  run.status = gtext_encoder_encode(x, in.data(), in.size(), true, &sink, &r);
  run.input_bytes = r.input_bytes;
  run.output_bytes = r.output_bytes;
  run.bytes = c.bytes;
  gtext_encoder_free(x);
  return run;
}

GTEXT_Decoder_Options limited(size_t n) {
  GTEXT_Decoder_Options o = gtext_decoder_options_default();
  o.max_output_bytes = n;
  return o;
}

} // namespace

TEST(EncodingContractInputBytes, TheDecoderReportsTheByteAtWhichTheLimitOrTheSinkFailed) {
  // "Up to and including the byte at which it was found": the first byte that cannot be written, counted from 1.
  Outcome r = decode_once(GTEXT_ENCODING_UTF_8, limited(5000), std::string(10000, 'x'));
  EXPECT_EQ(r.status, GTEXT_ENCODING_E_LIMIT_OUTPUT);
  EXPECT_EQ(r.input_bytes, 5001u); // an ASCII run: the byte after the 5000 that fit, not the end of the run
  EXPECT_EQ(r.bytes.size(), 5000u);
  r = decode_once(GTEXT_ENCODING_WINDOWS_1252, limited(1), "xyz");
  EXPECT_EQ(r.input_bytes, 2u);
  r = decode_once(GTEXT_ENCODING_UTF_8, limited(3), "ab" + utf8_of(0xE9));
  EXPECT_EQ(r.status, GTEXT_ENCODING_E_LIMIT_OUTPUT);
  EXPECT_EQ(r.input_bytes, 4u); // the second byte of the letter completes it
  r = decode_once(GTEXT_ENCODING_UTF_8, limited(5), utf8_of(0xE9) + std::string(8, 'x'));
  EXPECT_EQ(r.input_bytes, 6u); // two bytes for the letter, three that fit, the sixth is the one
  r = decode_once(GTEXT_ENCODING_UTF_8, limited(2), "ab");
  EXPECT_EQ(r.status, GTEXT_ENCODING_OK);
  EXPECT_EQ(r.input_bytes, 2u);

  // Split in chunks: the count is of the call's input.
  GTEXT_Decoder_Options o = limited(7);
  GTEXT_Decoder * d = nullptr;
  ASSERT_EQ(gtext_decoder_new(GTEXT_ENCODING_UTF_8, &o, &d), GTEXT_ENCODING_OK);
  Collect c;
  GTEXT_Encoding_Sink sink = sink_of(c);
  GTEXT_Decoder_Result res;
  ASSERT_EQ(gtext_decoder_decode(d, "xxxx", 4, false, &sink, &res), GTEXT_ENCODING_OK);
  EXPECT_EQ(res.input_bytes, 4u);
  ASSERT_EQ(gtext_decoder_decode(d, "yyyyyy", 6, false, &sink, &res), GTEXT_ENCODING_E_LIMIT_OUTPUT);
  EXPECT_EQ(res.input_bytes, 4u); // three fit
  gtext_decoder_free(d);

  // The sink: refused at the first full chunk of an ASCII run, of letters, and at the last flush.
  r = decode_once(GTEXT_ENCODING_UTF_8, gtext_decoder_options_default(), std::string(10000, 'x'), 0);
  EXPECT_EQ(r.status, GTEXT_ENCODING_E_WRITE);
  EXPECT_EQ(r.input_bytes, 4097u);
  std::string letters;
  for (int i = 0; i < 3000; i++) {
    letters += utf8_of(0xE9);
  }
  r = decode_once(GTEXT_ENCODING_UTF_8, gtext_decoder_options_default(), letters, 0);
  EXPECT_EQ(r.status, GTEXT_ENCODING_E_WRITE);
  EXPECT_EQ(r.input_bytes, 4098u); // 2048 letters fill the chunk; the second byte of the next one needs the sink
  r = decode_once(GTEXT_ENCODING_UTF_8, gtext_decoder_options_default(), "abc", 0);
  EXPECT_EQ(r.status, GTEXT_ENCODING_E_WRITE);
  EXPECT_EQ(r.input_bytes, 3u); // all of it was taken; the last flush is what failed
}

TEST(EncodingContractInputBytes, TheEncoderReportsTheByteAtWhichTheLimitOrTheSinkFailed) {
  GTEXT_Encoder_Options o = gtext_encoder_options_default();
  o.error_mode = GTEXT_ENCODER_HTML;
  o.max_output_bytes = 5000;
  Outcome r = encode_once(GTEXT_ENCODING_WINDOWS_1252, o, std::string(10000, 'x'));
  EXPECT_EQ(r.status, GTEXT_ENCODING_E_LIMIT_OUTPUT);
  EXPECT_EQ(r.input_bytes, 5001u);
  o.max_output_bytes = 1;
  r = encode_once(GTEXT_ENCODING_WINDOWS_1252, o, "a" + utf8_of(0xE9));
  EXPECT_EQ(r.input_bytes, 3u); // the second byte of the letter completes it
  o.max_output_bytes = 3;
  r = encode_once(GTEXT_ENCODING_ISO_8859_2, o, "x" + utf8_of(0x20AC));
  EXPECT_EQ(r.status, GTEXT_ENCODING_E_LIMIT_OUTPUT);
  EXPECT_EQ(r.input_bytes, 4u); // the reference would be seven bytes
  o.max_output_bytes = 0;
  r = encode_once(GTEXT_ENCODING_UTF_8, o, std::string(10000, 'x'), 0);
  EXPECT_EQ(r.status, GTEXT_ENCODING_E_WRITE);
  EXPECT_EQ(r.input_bytes, 4097u);
  r = encode_once(GTEXT_ENCODING_UTF_8, o, "abc", 0);
  EXPECT_EQ(r.input_bytes, 3u);
}

TEST(EncodingContractArguments, AnArgumentErrorDoesNotTouchTheStream) {
  GTEXT_Decoder * d = nullptr;
  ASSERT_EQ(gtext_decoder_new(GTEXT_ENCODING_UTF_8, nullptr, &d), GTEXT_ENCODING_OK);
  Collect c;
  GTEXT_Encoding_Sink sink = sink_of(c);
  ASSERT_EQ(gtext_decoder_decode(d, "\xE2\x82", 2, false, &sink, nullptr), GTEXT_ENCODING_OK);
  EXPECT_EQ(gtext_decoder_decode(d, "x", 1, true, nullptr, nullptr), GTEXT_ENCODING_E_INVALID);
  GTEXT_Encoding_Sink nowrite = {nullptr, nullptr};
  EXPECT_EQ(gtext_decoder_decode(d, "x", 1, true, &nowrite, nullptr), GTEXT_ENCODING_E_INVALID);
  EXPECT_EQ(gtext_decoder_decode(d, nullptr, 3, true, &sink, nullptr), GTEXT_ENCODING_E_INVALID);
  ASSERT_EQ(gtext_decoder_decode(d, "\xAC", 1, true, &sink, nullptr), GTEXT_ENCODING_OK);
  EXPECT_EQ(c.bytes, utf8_of(0x20AC)); // the held bytes survived the three mistakes
  gtext_decoder_free(d);

  GTEXT_Encoder * e = nullptr;
  ASSERT_EQ(gtext_encoder_new(GTEXT_ENCODING_WINDOWS_1252, nullptr, &e), GTEXT_ENCODING_OK);
  Collect c2;
  GTEXT_Encoding_Sink sink2 = sink_of(c2);
  ASSERT_EQ(gtext_encoder_encode(e, "\xE2\x82", 2, false, &sink2, nullptr), GTEXT_ENCODING_OK);
  EXPECT_EQ(gtext_encoder_encode(e, "x", 1, true, nullptr, nullptr), GTEXT_ENCODING_E_INVALID);
  EXPECT_EQ(gtext_encoder_encode(e, nullptr, 3, true, &sink2, nullptr), GTEXT_ENCODING_E_INVALID);
  ASSERT_EQ(gtext_encoder_encode(e, "\xAC", 1, true, &sink2, nullptr), GTEXT_ENCODING_OK);
  EXPECT_EQ(c2.bytes, "\x80");
  gtext_encoder_free(e);
}

TEST(EncodingContractOffsets, AFatalErrorAfterASniffedMarkCountsTheMarkInItsOffset) {
  GTEXT_Decoder_Options fatal = decoder_options(GTEXT_DECODER_FATAL);
  fatal.bom_sniffing = true;
  struct Case {
    std::string in;
    size_t offset;
    size_t input_bytes_whole;
  };
  const Case cases[] = {
      {bytes_of({0xEF, 0xBB, 0xBF, 'a', 0xFF}), 4, 5},
      {bytes_of({0xFF, 0xFE, 0x41, 0x00, 0x00}), 5, 5}, // the odd byte is met at the end of the stream
      {bytes_of({0xFE, 0xFF, 0x00, 0x41, 0xD8}), 5, 5},
      {bytes_of({0xEF, 0xBB, 0xBF, 0xE2, 0x82}), 5, 5},
      {bytes_of({0xFE, 0xFF, 0xDC, 0x00}), 3, 4},       // a trailing surrogate, found at its second byte
      {bytes_of({0xFF, 0xFE, 0x00, 0xDC}), 3, 4},
  };
  for (const Case & c : cases) {
    const Out whole = decode_whole(GTEXT_ENCODING_WINDOWS_1252, fatal, c.in);
    EXPECT_EQ(whole.status, GTEXT_ENCODING_E_MALFORMED) << show(c.in);
    EXPECT_EQ(whole.error_offset, c.offset) << show(c.in);
    const size_t n = c.in.size();
    for (uint32_t mask = 0; mask < (1u << (n + 1)); mask++) {
      std::vector<size_t> cuts;
      for (size_t p = 0; p <= n; p++) {
        if (mask & (1u << p)) {
          cuts.push_back(p);
        }
      }
      const Out o = decode_cut(GTEXT_ENCODING_WINDOWS_1252, fatal, c.in, cuts);
      EXPECT_EQ(o.error_offset, c.offset) << show(c.in) << " mask " << mask;
      EXPECT_EQ(o.status, GTEXT_ENCODING_E_MALFORMED);
    }
    GTEXT_Decoder * d = nullptr;
    ASSERT_EQ(gtext_decoder_new(GTEXT_ENCODING_WINDOWS_1252, &fatal, &d), GTEXT_ENCODING_OK);
    Collect col;
    GTEXT_Encoding_Sink sink = sink_of(col);
    GTEXT_Decoder_Result r;
    EXPECT_EQ(gtext_decoder_decode(d, c.in.data(), n, true, &sink, &r), GTEXT_ENCODING_E_MALFORMED);
    EXPECT_EQ(r.input_bytes, c.input_bytes_whole) << show(c.in);
    gtext_decoder_free(d);
  }
}

TEST(EncodingContractWork, TheStepsAreCountedAndProportionalToTheInput) {
  struct Case {
    GTEXT_Encoding enc;
    std::string unit;
  };
  const Case cases[] = {
      {GTEXT_ENCODING_UTF_8, bytes_of({0xF0, 0x9F, 0x92, 0xF0, 0x9F, 0xE2, 0x82, 0xE2})},
      {GTEXT_ENCODING_UTF_8, bytes_of({0xE0, 0xE0, 0xE0, 0x80})},
      {GTEXT_ENCODING_UTF_16LE, bytes_of({0x00, 0xD8, 0x00, 0xD8, 0x00, 0xD8, 0x41, 0x00})},
      {GTEXT_ENCODING_UTF_16BE, bytes_of({0xD8, 0x00, 0xD8, 0x00, 0xD8, 0x00, 0x00, 0x41})},
      {GTEXT_ENCODING_WINDOWS_1252, bytes_of({0x80, 0x41, 0xFF, 0x00})},
      {GTEXT_ENCODING_ISO_8859_6, bytes_of({0xA1, 0xA2, 0xA3})},
      {GTEXT_ENCODING_REPLACEMENT, bytes_of({0xA1, 0xA2, 0xA3})},
  };
  for (const Case & c : cases) {
    size_t steps[2] = {0, 0};
    size_t sizes[2] = {0, 0};
    for (int k = 0; k < 2; k++) {
      std::string in;
      for (size_t i = 0; i < (k ? 80000u : 10000u); i++) {
        in += c.unit;
      }
      char * out = nullptr;
      size_t n = 0;
      GTEXT_Decoder_Result r;
      ASSERT_EQ(gtext_encoding_decode_buffer(c.enc, nullptr, in.data(), in.size(), &out, &n, &r), GTEXT_ENCODING_OK);
      gtext_encoding_buffer_free(out);
      steps[k] = r.steps;
      sizes[k] = in.size();
      EXPECT_LE(r.steps, 4 * in.size() + 4) << gtext_encoding_name(c.enc);
    }
    // Eight times the input is eight times the work, to within the constant of the end of the stream: counted, so no clock decides it.
    const size_t expect = steps[0] * 8;
    EXPECT_LE(steps[1], expect + 16) << gtext_encoding_name(c.enc);
    EXPECT_GE(steps[1] + 16, expect) << gtext_encoding_name(c.enc);
    EXPECT_EQ(sizes[1], sizes[0] * 8);
  }
  // The encoders: ill-formed input, each byte an error of its own, and references.
  for (GTEXT_Encoder_Error_Mode mode : {GTEXT_ENCODER_HTML, GTEXT_ENCODER_REPLACEMENT}) {
    GTEXT_Encoder_Options o = gtext_encoder_options_default();
    o.error_mode = mode;
    size_t steps[2] = {0, 0};
    for (int k = 0; k < 2; k++) {
      std::string in;
      for (size_t i = 0; i < (k ? 80000u : 10000u); i++) {
        in += bytes_of({0xE2, 0x82, 0xAC, 0xF0, 0x9F, 0xE2, 0x82, 0x41, 0xFF});
      }
      char * out = nullptr;
      size_t n = 0;
      GTEXT_Encoder_Result r;
      ASSERT_EQ(gtext_encoding_encode_buffer(GTEXT_ENCODING_ISO_8859_2, &o, in.data(), in.size(), &out, &n, &r), GTEXT_ENCODING_OK);
      gtext_encoding_buffer_free(out);
      steps[k] = r.steps;
      EXPECT_LE(r.steps, 3 * in.size() + 3);
    }
    EXPECT_LE(steps[1], steps[0] * 8 + 16);
    EXPECT_GE(steps[1] + 16, steps[0] * 8);
  }
}

TEST(EncodingContractLabels, TheLongestLabelFitsTheKey) {
  size_t longest = 0;
  for (size_t i = 0; i < gtext_encoding_label_count(); i++) {
    size_t len = 0;
    ASSERT_TRUE(gtext_encoding_label_at(i, nullptr, &len, nullptr));
    longest = std::max(longest, len);
  }
  EXPECT_LE(longest, 64u);
  EXPECT_GE(longest, 17u); // csisolatincyrillic: a key shorter than the longest label must lose it
  for (size_t n : {size_t(63), size_t(64), size_t(65), size_t(128)}) {
    EXPECT_EQ(gtext_encoding_for_label(std::string(n, 'a').data(), n), GTEXT_ENCODING_NONE) << n;
  }
  for (size_t i = 0; i < gtext_encoding_label_count(); i++) {
    const char * l = nullptr;
    size_t len = 0;
    GTEXT_Encoding e = GTEXT_ENCODING_NONE;
    ASSERT_TRUE(gtext_encoding_label_at(i, &l, &len, &e));
    // the label itself, upper-cased, with the most white space around it that keeps the whole under any plausible buffer
    std::string s(30, ' ');
    for (size_t k = 0; k < len; k++) {
      s += char(std::toupper((unsigned char)l[k]));
    }
    s += std::string(30, '\t');
    EXPECT_EQ(gtext_encoding_for_label(s.data(), s.size()), e) << std::string(l, len);
  }
}

TEST(EncodingContractEncoders, WhichEncodingsHaveOne) {
  size_t with = 0;
  for (int e = 1; e < GTEXT_ENCODING_COUNT; e++) {
    GTEXT_Encoder * x = nullptr;
    const GTEXT_Encoding_Status s = gtext_encoder_new(GTEXT_Encoding(e), nullptr, &x);
    EXPECT_EQ(gtext_encoding_has_encoder(GTEXT_Encoding(e)), s == GTEXT_ENCODING_OK) << gtext_encoding_name(GTEXT_Encoding(e));
    with += s == GTEXT_ENCODING_OK;
    gtext_encoder_free(x);
  }
  EXPECT_EQ(with, 30u); // UTF-8, the 28 single-byte encodings, x-user-defined
  EXPECT_FALSE(gtext_encoding_has_encoder(GTEXT_ENCODING_UTF_16LE));
  EXPECT_FALSE(gtext_encoding_has_encoder(GTEXT_ENCODING_REPLACEMENT));
  EXPECT_FALSE(gtext_encoding_has_encoder(GTEXT_ENCODING_NONE));
  EXPECT_FALSE(gtext_encoding_has_encoder(GTEXT_Encoding(63)));
}

TEST(EncodingContractNull, AnEmptyFlushNeedsNoBuffer) {
  for (bool sniff : {false, true}) {
    GTEXT_Decoder_Options o = gtext_decoder_options_default();
    o.bom_sniffing = sniff;
    GTEXT_Decoder * d = nullptr;
    ASSERT_EQ(gtext_decoder_new(GTEXT_ENCODING_UTF_8, &o, &d), GTEXT_ENCODING_OK);
    Collect c;
    GTEXT_Encoding_Sink sink = sink_of(c);
    EXPECT_EQ(gtext_decoder_decode(d, nullptr, 0, false, &sink, nullptr), GTEXT_ENCODING_OK);
    EXPECT_EQ(gtext_decoder_decode(d, "\xEF", 1, false, &sink, nullptr), GTEXT_ENCODING_OK);
    EXPECT_EQ(gtext_decoder_decode(d, nullptr, 0, true, &sink, nullptr), GTEXT_ENCODING_OK);
    EXPECT_EQ(c.bytes, utf8_of(0xFFFD));
    gtext_decoder_free(d);
  }
  GTEXT_Encoder * e = nullptr;
  ASSERT_EQ(gtext_encoder_new(GTEXT_ENCODING_UTF_8, nullptr, &e), GTEXT_ENCODING_OK);
  Collect c;
  GTEXT_Encoding_Sink sink = sink_of(c);
  EXPECT_EQ(gtext_encoder_encode(e, nullptr, 0, true, &sink, nullptr), GTEXT_ENCODING_OK);
  gtext_encoder_free(e);
}
