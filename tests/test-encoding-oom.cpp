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
 * Memory: the allocator that refuses the n-th allocation, for every n a call makes, through every entry point that
 * allocates (the two constructors and the two buffer forms, with outputs of one chunk and of several, which grow the
 * buffer); the streaming calls themselves make no allocation at all; nothing leaks and nothing is freed through the
 * wrong allocator.
 *
 * And threads: any number of decoders and encoders, one to a thread, and the label table from all of them at once.
 * (test-asan runs this under AddressSanitizer and test-tsan under ThreadSanitizer.)
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "encoding_test_support.h"
#include "markup_test_support.h"

#include <atomic>
#include <functional>
#include <thread>

using namespace enctest;

namespace {

// Run `body` with the allocator failing the n-th allocation, for n = 1, 2, ... until a run makes no failure. Returns the
// number of allocations the failure-free run made.
size_t sweep(FaultAllocator & fa, const std::function<void(bool failed)> & body) {
  for (size_t n = 1; n < 200; n++) {
    fa.arm(n);
    body(false);
    const bool failed = fa.failed;
    fa.disarm();
    EXPECT_TRUE(fa.live.empty()) << "leak after failing allocation " << n << ": " << fa.live.size();
    EXPECT_EQ(fa.bad_frees, 0u);
    if (!failed) {
      return n - 1;
    }
  }
  ADD_FAILURE() << "a call that never stops allocating";
  return 0;
}

} // namespace

TEST(EncodingOom, EveryAllocationOfTheDecoderAndTheBufferFormMayFail) {
  FaultAllocator fa;
  for (size_t size : {size_t(0), size_t(10), size_t(3000), size_t(100000)}) {
    std::string in;
    while (in.size() < size) {
      in += "ab" + utf8_of(0x20AC);
    }
    const Out reference = decode_whole(GTEXT_ENCODING_UTF_8, in);
    GTEXT_Decoder_Options o = gtext_decoder_options_default();
    o.allocator = &fa.alloc;
    size_t failures = 0;
    const size_t calls = sweep(fa, [&](bool) {
      char * out = nullptr;
      size_t n = 0;
      GTEXT_Decoder_Result r;
      const GTEXT_Encoding_Status s = gtext_encoding_decode_buffer(GTEXT_ENCODING_UTF_8, &o, in.data(), in.size(), &out, &n, &r);
      if (s == GTEXT_ENCODING_OK) {
        EXPECT_EQ(std::string(out, n), reference.bytes);
        gtext_encoding_buffer_free(out);
      } else {
        EXPECT_EQ(s, GTEXT_ENCODING_E_OOM);
        EXPECT_EQ(r.status, GTEXT_ENCODING_E_OOM);
        EXPECT_EQ(out, nullptr);
        failures++;
      }
    });
    EXPECT_GE(calls, 2u); // the decoder, and the buffer
    EXPECT_EQ(failures, calls) << size;
    // The decoder alone is one allocation, and a stream through it makes none.
    GTEXT_Decoder * d = nullptr;
    fa.arm(1);
    EXPECT_EQ(gtext_decoder_new(GTEXT_ENCODING_UTF_8, &o, &d), GTEXT_ENCODING_E_OOM);
    EXPECT_EQ(d, nullptr);
    fa.arm(100000);
    ASSERT_EQ(gtext_decoder_new(GTEXT_ENCODING_UTF_8, &o, &d), GTEXT_ENCODING_OK);
    const size_t after_new = fa.calls;
    Collect c;
    GTEXT_Encoding_Sink sink = sink_of(c);
    ASSERT_EQ(gtext_decoder_decode(d, in.data(), in.size(), true, &sink, nullptr), GTEXT_ENCODING_OK);
    EXPECT_EQ(fa.calls, after_new) << "a decode allocated";
    gtext_decoder_free(d);
    fa.disarm();
    EXPECT_TRUE(fa.live.empty());
  }
}

TEST(EncodingOom, EveryAllocationOfTheEncoderAndTheBufferFormMayFail) {
  FaultAllocator fa;
  for (size_t size : {size_t(0), size_t(10), size_t(3000), size_t(100000)}) {
    std::string in;
    while (in.size() < size) {
      in += "ab" + utf8_of(0x20AC) + utf8_of(0x1F4A9);
    }
    GTEXT_Encoder_Options o = encoder_options(GTEXT_ENCODER_HTML);
    const Out reference = encode_whole(GTEXT_ENCODING_ISO_8859_2, o, in);
    o.allocator = &fa.alloc;
    size_t failures = 0;
    const size_t calls = sweep(fa, [&](bool) {
      char * out = nullptr;
      size_t n = 0;
      GTEXT_Encoder_Result r;
      const GTEXT_Encoding_Status s = gtext_encoding_encode_buffer(GTEXT_ENCODING_ISO_8859_2, &o, in.data(), in.size(), &out, &n, &r);
      if (s == GTEXT_ENCODING_OK) {
        EXPECT_EQ(std::string(out, n), reference.bytes);
        gtext_encoding_buffer_free(out);
      } else {
        EXPECT_EQ(s, GTEXT_ENCODING_E_OOM);
        EXPECT_EQ(out, nullptr);
        failures++;
      }
    });
    EXPECT_GE(calls, 2u);
    EXPECT_EQ(failures, calls) << size;
    GTEXT_Encoder * e = nullptr;
    fa.arm(1);
    EXPECT_EQ(gtext_encoder_new(GTEXT_ENCODING_ISO_8859_2, &o, &e), GTEXT_ENCODING_E_OOM);
    EXPECT_EQ(e, nullptr);
    fa.arm(100000);
    ASSERT_EQ(gtext_encoder_new(GTEXT_ENCODING_ISO_8859_2, &o, &e), GTEXT_ENCODING_OK);
    const size_t after_new = fa.calls;
    Collect c;
    GTEXT_Encoding_Sink sink = sink_of(c);
    ASSERT_EQ(gtext_encoder_encode(e, in.data(), in.size(), true, &sink, nullptr), GTEXT_ENCODING_OK);
    EXPECT_EQ(fa.calls, after_new) << "an encode allocated";
    gtext_encoder_free(e);
    fa.disarm();
    EXPECT_TRUE(fa.live.empty());
  }
}

TEST(EncodingOom, AFailureInTheMiddleOfTheOutputLeavesNothingBehindAndDoesNotCallTheSink) {
  // The buffer form's growth fails at a later allocation: no output buffer escapes.
  FaultAllocator fa;
  std::string in(300000, 'x');
  GTEXT_Decoder_Options o = gtext_decoder_options_default();
  o.allocator = &fa.alloc;
  fa.arm(5); // the decoder, then four doublings of the buffer
  char * out = reinterpret_cast<char *>(0x1);
  size_t n = 0;
  EXPECT_EQ(gtext_encoding_decode_buffer(GTEXT_ENCODING_UTF_8, &o, in.data(), in.size(), &out, &n, nullptr), GTEXT_ENCODING_E_OOM);
  EXPECT_EQ(out, reinterpret_cast<char *>(0x1));
  fa.disarm();
  EXPECT_TRUE(fa.live.empty());
  EXPECT_EQ(fa.bad_frees, 0u);
}

TEST(EncodingOom, AnAllocatorGivenToTheOptionsIsTheOnlyOneUsedAndTheBufferRemembersIt) {
  FaultAllocator fa;
  GTEXT_Decoder_Options o = gtext_decoder_options_default();
  o.allocator = &fa.alloc;
  char * out = nullptr;
  size_t n = 0;
  ASSERT_EQ(gtext_encoding_decode_buffer(GTEXT_ENCODING_WINDOWS_1252, &o, "caf\xE9", 4, &out, &n, nullptr), GTEXT_ENCODING_OK);
  EXPECT_EQ(std::string(out, n), "caf\xC3\xA9");
  // The decoder is gone, the buffer is not, and it is freed through the allocator that made it, which the options no longer name.
  EXPECT_EQ(fa.live.size(), 1u);
  gtext_encoding_buffer_free(out);
  EXPECT_TRUE(fa.live.empty());
  EXPECT_EQ(fa.bad_frees, 0u);
}

TEST(EncodingConcurrency, DecodersEncodersAndTheLabelTableOnManyThreads) {
  const unsigned threads = 8;
  std::atomic<unsigned> failures{0};
  std::vector<std::thread> pool;
  for (unsigned t = 0; t < threads; t++) {
    pool.emplace_back([&failures, t] {
      Rng rng(100 + t);
      const GTEXT_Encoding encodings[] = {GTEXT_ENCODING_UTF_8, GTEXT_ENCODING_UTF_16LE, GTEXT_ENCODING_WINDOWS_1250,
          GTEXT_ENCODING_KOI8_R, GTEXT_ENCODING_X_USER_DEFINED, GTEXT_ENCODING_REPLACEMENT};
      for (int round = 0; round < 60; round++) {
        const GTEXT_Encoding e = encodings[rng.below(6)];
        std::string in;
        for (size_t i = 0; i < 2000; i++) {
          in += char(rng.below(256));
        }
        const Out whole = decode_whole(e, in);
        std::vector<size_t> cuts;
        for (size_t p = 1; p < in.size(); p += 1 + rng.below(50)) {
          cuts.push_back(p);
        }
        if (!(decode_cut(e, gtext_decoder_options_default(), in, cuts) == whole)) {
          failures++;
        }
        const GTEXT_Encoding oe = gtext_encoding_output_encoding(e);
        const Out enc = encode_whole(oe, encoder_options(GTEXT_ENCODER_HTML), whole.bytes);
        if (enc.status != GTEXT_ENCODING_OK) {
          failures++;
        }
        for (const char * label : {"UTF-8", " latin1 ", "koi8-r", "x-user-defined", "ISO-8859-2", "nope"}) {
          const GTEXT_Encoding got = gtext_encoding_for_label(label, std::strlen(label));
          if ((got == GTEXT_ENCODING_NONE) != (std::strcmp(label, "nope") == 0)) {
            failures++;
          }
        }
      }
    });
  }
  for (auto & th : pool) {
    th.join();
  }
  EXPECT_EQ(failures.load(), 0u);
}
