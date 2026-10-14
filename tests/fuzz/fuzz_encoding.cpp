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
 * libFuzzer harness for the Encoding module (src/encoding/): the decoders and encoders of every encoding in the
 * standard, with their options, chunking, limits, a refusing sink and a failing allocator.
 *
 * The input is four option bytes and then the bytes to decode and to encode (the same bytes are read as the encoding's
 * bytes by the decoder and as UTF-8 by the encoder). o0 picks the encoding: 1 to 40 are the standard's in table order,
 * the rest are values that are no encoding. o1: bit 0 the decoder's fatal mode, bit 1 `bom_sniffing`, bit 2 `bom_removal`,
 * bits 4 to 7 how finely the input is cut. o2: bits 0 and 1 the encoder's error mode (3 is no mode), bits 4 to 7 how finely
 * it is cut. o3: bits 0 to 2 the output limit (none, the length, one less, half, one, three), bits 3 to 5 which call of the
 * sink is refused, bit 6 the allocation-failure sweep, bit 7 the misuse of every entry point.
 *
 * The multi-byte encodings (GBK, gb18030, Big5, EUC-JP, ISO-2022-JP, Shift_JIS, EUC-KR) are held to the model of tests/encoding_cjk_model.h, the
 * standard's handlers written again over the pinned indexes.json (which the harness reads, from the library's directory): the decoder's output, its error
 * and the offset of the error, the points at which an output limit may cut (a Big5 pair of code points is not cut), and the encoder's output in all three modes,
 * with the code point and the offset of a failure and the places a limit may cut (the escape sequences of ISO-2022-JP are not cut).
 * That includes `bom_sniffing`: a mark makes the decoder the UTF one it names and the UTF reference decodes what follows it; no mark leaves the model of the
 * encoding asked for, over the whole input. And, for ISO-2022-JP in fatal mode, the state across a failure: the input is cut (at every place of a short one, at
 * eight of a long one), and when the first piece fails with a fatal error in a call that does not flush the second, decoded by the same decoder, must equal the
 * model begun in the mode the failure left (tests/encoding_cjk_model.h, ModelDecoded::iso_mode) - a half-read escape, a leading byte or the output flag that
 * survived would show. The fatal status of the encoder names the code point that was handled (for ISO-2022-JP the control, not U+FFFD), and the HTML mode's
 * reference is the one the standard returns.
 *
 * The properties, for the decoder:
 *
 *     the answer - status, output, offset, count, encoding, mark - is the same for the whole input, for every
 *     byte on its own, and for random cuts;
 *     the output is well-formed UTF-8 (a validator that is not the library's), no longer than three times the input
 *     and three bytes (so the work is linear), and the sink's chunks are never above four kilobytes;
 *     for UTF-8, UTF-16LE and UTF-16BE the output is that of the reference decoders in tests/encoding_model.h (written
 *     as table lookups and over whole code units, not as the standard's state machines), mark handling included;
 *     for the single-byte encodings, `replacement` and `x-user-defined` it is the byte-by-byte decode;
 *     fatal mode fails exactly when replacement mode replaced something, its output before the failure is a prefix of
 *     replacement mode's, and its offset is within the stream;
 *     what the decoder read without error and without mark handling encodes back to the same bytes (UTF-8, the
 *     single-byte encodings, x-user-defined);
 *     the output limit passes the length and stops one byte short at the last whole code point;
 *     a sink that refuses the n-th call gets exactly the chunks before it and is not called again, and the decoder
 *     works after it;
 *     an allocation that fails - the first to the last of a call - is E_OOM or the normal answer, and never a leak,
 *     a double free or a free of a block the allocator did not make; and the streaming calls allocate nothing.
 *
 * For the encoder: the same cut and limit and sink and allocator properties, and the output is that of a model that
 * encodes each code point of the reference UTF-8 decoder on its own (a reference for `?` and for `&#N;`, the offset of
 * the first code point that cannot be written, and the order in which a malformed sequence and an unencodable code point
 * are met); the fatal output is a prefix of the other two; the HTML and replacement modes never fail but for the limit;
 * and what is written decodes back to the repaired text.
 *
 * Build with: make fuzz-encoding      Run: make fuzz-run-encoding
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <ghoti.io/text/encoding.h>

#include "../encoding_cjk_model.h"
#include "../encoding_model.h"
#include "../markup_test_support.h"

namespace {

using namespace enctest;

[[noreturn]] void fail(const char * what, const std::string & detail, const std::string & input) {
  std::fprintf(stderr, "fuzz_encoding: %s\n%s\ninput (%zu bytes): ", what, detail.c_str(), input.size());
  for (unsigned char c : input) {
    if (c >= 0x20 && c < 0x7f && c != '\\') {
      std::fputc(c, stderr);
    }
    else {
      std::fprintf(stderr, "\\x%02x", c);
    }
  }
  std::fputc('\n', stderr);
  std::fflush(stderr);
  std::abort();
}

// How many inputs reached each half (the seed replay requires most of them to).
size_t g_decoded_inputs = 0;
size_t g_encoded_inputs = 0;
size_t g_refused_values = 0;

struct Ctx {
  std::string input; // the whole input, for the report
  void check(bool ok, const char * what, const std::string & detail = std::string()) const {
    if (!ok) {
      fail(what, detail, input);
    }
  }
};

bool well_formed_utf8(const std::string & s) {
  return ref_utf8(s).replacements == 0;
}

// The cut points of a random plan: finer for higher `fineness`.
std::vector<size_t> random_cuts(Rng & rng, size_t n, unsigned fineness) {
  std::vector<size_t> cuts;
  const size_t denominator = 1 + (fineness == 0 ? 40 : 16 / fineness);
  for (size_t p = 0; p <= n; p++) {
    if (rng.below(denominator) == 0) {
      cuts.push_back(p);
    }
  }
  return cuts;
}

std::vector<size_t> every_byte(size_t n) {
  std::vector<size_t> cuts;
  for (size_t i = 1; i < n; i++) {
    cuts.push_back(i);
  }
  return cuts;
}

// The prefix of `full` that ends on a code point boundary at or below `limit` (the encoders' units are whole
// characters, which for the encoders' outputs the caller checks otherwise).
std::string boundary_prefix(const std::string & full, size_t limit) {
  size_t best = 0;
  for (size_t i = 0; i <= full.size() && i <= limit; i++) {
    if (i == full.size() || ((unsigned char)full[i] & 0xC0) != 0x80) {
      best = i;
    }
  }
  return full.substr(0, best);
}

size_t pick_limit(unsigned sel, size_t full) {
  switch (sel & 7) {
  case 1: return full;
  case 2: return full > 1 ? full - 1 : 0;
  case 3: return full / 2;
  case 4: return 1;
  case 5: return 3;
  default: return 0; // none
  }
}

GTEXT_Encoding pick_encoding(unsigned o0) {
  const unsigned k = o0 % 44;
  if (k <= 40) {
    return GTEXT_Encoding(k); // 0 is NONE
  }
  if (k == 41) {
    return GTEXT_ENCODING_COUNT;
  }
  // Values a C++ enum can hold (0 to 63 for this one) that are no encoding; an int a C caller might pass beyond them is the
  // tests' (tests/test-encoding-decode.cpp), since loading one here is undefined behaviour that clang's enum check reports.
  return k == 42 ? GTEXT_Encoding(63) : GTEXT_Encoding(50);
}

bool valid_encoding(GTEXT_Encoding e) {
  return int(e) > 0 && int(e) < int(GTEXT_ENCODING_COUNT);
}

// What a decoder has written after the first `k` bytes of `in` (the stream is over if that is all of it), with no limit.
std::string decoded_prefix(GTEXT_Encoding e, GTEXT_Decoder_Options o, const std::string & in, size_t k) {
  o.max_output_bytes = SIZE_MAX;
  o.error_mode = GTEXT_DECODER_REPLACEMENT;
  GTEXT_Decoder * d = nullptr;
  if (gtext_decoder_new(e, &o, &d) != GTEXT_ENCODING_OK) {
    return std::string();
  }
  Collect c;
  GTEXT_Encoding_Sink sink = sink_of(c);
  gtext_decoder_decode(d, in.data(), k, k == in.size(), &sink, nullptr); // (the end of the input is the end of the stream)
  gtext_decoder_free(d);
  return c.bytes;
}

std::string encoded_prefix(GTEXT_Encoding e, GTEXT_Encoder_Options o, const std::string & in, size_t k) {
  o.max_output_bytes = SIZE_MAX;
  GTEXT_Encoder * x = nullptr;
  if (gtext_encoder_new(e, &o, &x) != GTEXT_ENCODING_OK) {
    return std::string();
  }
  Collect c;
  GTEXT_Encoding_Sink sink = sink_of(c);
  gtext_encoder_encode(x, in.data(), k, k == in.size(), &sink, nullptr); // (the end of the input is the end of the stream)
  gtext_encoder_free(x);
  return c.bytes;
}

// ---- the decoder -------------------------------------------------------------------------------------------------

Out decode_with(GTEXT_Encoding e, const GTEXT_Decoder_Options & o, const std::string & in, const std::vector<size_t> & cuts,
    size_t * calls = nullptr, size_t * largest = nullptr) {
  Out out;
  GTEXT_Decoder * d = nullptr;
  out.status = gtext_decoder_new(e, &o, &d);
  if (out.status != GTEXT_ENCODING_OK) {
    return out;
  }
  Collect c;
  GTEXT_Encoding_Sink sink = sink_of(c);
  std::vector<size_t> points = cuts;
  points.push_back(in.size());
  size_t start = 0;
  for (size_t k = 0; k < points.size(); k++) {
    const size_t at = points[k] < start ? start : points[k];
    GTEXT_Decoder_Result r;
    const GTEXT_Encoding_Status s = gtext_decoder_decode(d, in.data() + start, at - start, k + 1 == points.size(), &sink, &r);
    out.replacements += r.replacements;
    out.encoding = r.encoding;
    out.bom_found = out.bom_found || r.bom_found;
    if (s != GTEXT_ENCODING_OK) {
      out.status = s;
      out.error_offset = r.error_offset;
      break;
    }
    start = at;
  }
  out.bytes = c.bytes;
  if (calls) {
    *calls = c.calls;
  }
  if (largest) {
    *largest = c.largest;
  }
  gtext_decoder_free(d);
  return out;
}

// ISO-2022-JP's decoder after a fatal error in a call that did not flush: the next call decodes as the model does from the mode the failure left, with
// nothing else of the old state (tests/encoding_cjk_model.h: ModelDecoded::iso_mode). The input is cut at a random place; when the first piece fails, the
// second is decoded by the same decoder, and it must equal the model begun in that mode (a stale escape, leading byte or output flag shows).
void check_iso_resume_at(const Ctx & ctx, const GTEXT_Decoder_Options & o, const std::string & in, size_t cut) {
  const std::string a = in.substr(0, cut), b = in.substr(cut);
  if (o.bom_sniffing) {
    // A first byte that could begin a mark is held until the sniff decides, so the first call may not fail yet; and a second stream that begins with a mark
    // is decoded as the UTF the mark names, which is not the mode the failure left. Both are the sniff's (checked above), not this property's.
    auto may_begin_mark = [](const std::string & x) { return !x.empty() && ((unsigned char)x[0] == 0xEF || (unsigned char)x[0] == 0xFE || (unsigned char)x[0] == 0xFF); };
    if (may_begin_mark(a) || may_begin_mark(b)) {
      return;
    }
  }
  const ModelDecoded fa = model_decode(GTEXT_ENCODING_ISO_2022_JP, reinterpret_cast<const unsigned char *>(a.data()), a.size(), true);
  // (a failure of the first piece, as it would be without the end of the queue: the model is run over the piece alone, with the end of the queue, so only
  // a piece whose failure is before its last byte is certain to be a failure of the unfinished call)
  if (!fa.failed || fa.error_offset >= a.size()) {
    return;
  }
  GTEXT_Decoder * d = nullptr;
  ctx.check(gtext_decoder_new(GTEXT_ENCODING_ISO_2022_JP, &o, &d) == GTEXT_ENCODING_OK, "decoder: new failed");
  Collect c1;
  GTEXT_Encoding_Sink s1 = sink_of(c1);
  GTEXT_Decoder_Result r1;
  ctx.check(gtext_decoder_decode(d, a.data(), a.size(), false, &s1, &r1) == GTEXT_ENCODING_E_MALFORMED, "decoder: the first piece did not fail as the model's did", show(a) + " cut " + std::to_string(cut) + " model offset " + std::to_string(fa.error_offset) + " of " + std::to_string(a.size()));
  Collect c2;
  GTEXT_Encoding_Sink s2 = sink_of(c2);
  GTEXT_Decoder_Result r2;
  const GTEXT_Encoding_Status st = gtext_decoder_decode(d, b.data(), b.size(), true, &s2, &r2);
  gtext_decoder_free(d);
  const ModelDecoded fb = model_decode(GTEXT_ENCODING_ISO_2022_JP, reinterpret_cast<const unsigned char *>(b.data()), b.size(), true, fa.iso_mode);
  ctx.check((st == GTEXT_ENCODING_E_MALFORMED) == fb.failed, "decoder: after a failure, the next call's verdict is not the model's from the mode it left");
  ctx.check(c2.bytes == utf8_of_cps(fb.cps), "decoder: after a failure, the next call's output is not the model's from the mode it left", show(c2.bytes) + " vs " + show(utf8_of_cps(fb.cps)));
  if (fb.failed) {
    ctx.check(r2.error_offset == fb.error_offset, "decoder: after a failure, the next call's offset is not the model's from the mode it left");
  }
}

// ... at every cut of a short input, and at a few random ones of a long one.
void check_iso_resume(const Ctx & ctx, const GTEXT_Decoder_Options & o, const std::string & in, Rng & rng) {
  if (in.size() <= 48) {
    for (size_t cut = 0; cut <= in.size(); cut++) {
      check_iso_resume_at(ctx, o, in, cut);
    }
  } else {
    for (int k = 0; k < 8; k++) {
      check_iso_resume_at(ctx, o, in, rng.next() % (in.size() + 1));
    }
  }
}

void check_decoder(const Ctx & ctx, GTEXT_Encoding e, unsigned o1, unsigned o3, const std::string & in, Rng & rng) {
  GTEXT_Decoder_Options o = gtext_decoder_options_default();
  o.error_mode = (o1 & 1) ? GTEXT_DECODER_FATAL : GTEXT_DECODER_REPLACEMENT;
  o.bom_sniffing = (o1 & 2) != 0;
  o.bom_removal = (o1 & 4) != 0;

  size_t calls = 0, largest = 0;
  const Out whole = decode_with(e, o, in, {}, &calls, &largest);
  ctx.check(whole.status == GTEXT_ENCODING_OK || whole.status == GTEXT_ENCODING_E_MALFORMED, "decoder: an unexpected status",
      gtext_encoding_status_string(whole.status));
  ctx.check(whole.status == GTEXT_ENCODING_E_MALFORMED ? (o1 & 1) != 0 : true, "decoder: malformed in replacement mode");
  ctx.check(largest <= 4096, "decoder: a chunk above four kilobytes");
  ctx.check(whole.bytes.size() <= 3 * in.size() + 3, "decoder: output more than three times the input");
  ctx.check(well_formed_utf8(whole.bytes), "decoder: the output is not well-formed UTF-8", show(whole.bytes));
  ctx.check(whole.error_offset <= in.size(), "decoder: the offset is beyond the stream");

  // The cut.
  const Out bytewise = decode_with(e, o, in, every_byte(in.size()));
  ctx.check(bytewise == whole, "decoder: byte-at-a-time differs from whole", show(bytewise) + " vs " + show(whole));
  const Out randomly = decode_with(e, o, in, random_cuts(rng, in.size(), (o1 >> 4) & 15));
  ctx.check(randomly == whole, "decoder: random cuts differ from whole", show(randomly) + " vs " + show(whole));

  // Replacement mode, for comparisons.
  GTEXT_Decoder_Options orep = o;
  orep.error_mode = GTEXT_DECODER_REPLACEMENT;
  const Out rep = (o1 & 1) ? decode_with(e, orep, in, {}) : whole;
  ctx.check(rep.status == GTEXT_ENCODING_OK, "decoder: replacement mode failed");
  if (o1 & 1) {
    ctx.check((whole.status == GTEXT_ENCODING_E_MALFORMED) == (rep.replacements > 0), "decoder: fatal and replacement disagree",
        std::to_string(rep.replacements));
    ctx.check(rep.bytes.compare(0, whole.bytes.size(), whole.bytes) == 0, "decoder: fatal output is not a prefix of replacement's");
    if (whole.status == GTEXT_ENCODING_OK) {
      ctx.check(whole.bytes == rep.bytes, "decoder: fatal succeeded with other output");
    }
  }

  // The models.
  if (e == GTEXT_ENCODING_UTF_8 || e == GTEXT_ENCODING_UTF_16LE || e == GTEXT_ENCODING_UTF_16BE) {
    GTEXT_Encoding active = e;
    std::string body = in;
    bool mark = false;
    if (o.bom_sniffing) {
      if (in.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        active = GTEXT_ENCODING_UTF_8;
        body = in.substr(3);
        mark = true;
      } else if (in.compare(0, 2, "\xFE\xFF") == 0) {
        active = GTEXT_ENCODING_UTF_16BE;
        body = in.substr(2);
        mark = true;
      } else if (in.compare(0, 2, "\xFF\xFE") == 0) {
        active = GTEXT_ENCODING_UTF_16LE;
        body = in.substr(2);
        mark = true;
      }
    }
    RefResult ref = active == GTEXT_ENCODING_UTF_8 ? ref_utf8(body) : ref_utf16(body, active == GTEXT_ENCODING_UTF_16BE);
    if (!o.bom_sniffing && o.bom_removal && !ref.cps.empty() && ref.cps[0] == 0xFEFF) {
      ref.cps.erase(ref.cps.begin());
    }
    ctx.check(rep.bytes == utf8_of_cps(ref.cps), "decoder: not the reference's output", show(rep.bytes) + " vs " + show(utf8_of_cps(ref.cps)));
    ctx.check(rep.replacements == ref.replacements, "decoder: not the reference's count");
    ctx.check(rep.bom_found == mark, "decoder: the mark");
    ctx.check(rep.encoding == active, "decoder: the encoding");
  } else if (is_multi_byte(e)) {
    // The standard's handlers, written again (tests/encoding_cjk_model.h): the output, the count, and in fatal mode the error and its offset. With
    // `bom_sniffing` a mark makes the decoder a UTF one (the UTF reference decodes what follows it) and none leaves the encoding asked for, over the whole input.
    size_t skip = 0;
    GTEXT_Encoding chosen = e;
    if (o.bom_sniffing) {
      if (in.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        chosen = GTEXT_ENCODING_UTF_8;
        skip = 3;
      } else if (in.compare(0, 2, "\xFE\xFF") == 0) {
        chosen = GTEXT_ENCODING_UTF_16BE;
        skip = 2;
      } else if (in.compare(0, 2, "\xFF\xFE") == 0) {
        chosen = GTEXT_ENCODING_UTF_16LE;
        skip = 2;
      }
    }
    if (skip != 0) {
      const RefResult ref = chosen == GTEXT_ENCODING_UTF_8 ? ref_utf8(in.substr(skip)) : ref_utf16(in.substr(skip), chosen == GTEXT_ENCODING_UTF_16BE);
      ctx.check(rep.bytes == utf8_of_cps(ref.cps) && rep.replacements == ref.replacements, "decoder: after a mark, not the reference's output",
          show(rep.bytes) + " vs " + show(utf8_of_cps(ref.cps)));
    } else {
      const ModelDecoded m = model_decode(e, reinterpret_cast<const unsigned char *>(in.data()), in.size(), false);
      ctx.check(rep.bytes == utf8_of_cps(m.cps) && rep.replacements == m.replacements, "decoder: not the model's output", show(rep.bytes) + " vs " + show(utf8_of_cps(m.cps)));
      if (o1 & 1) {
        const ModelDecoded mf = model_decode(e, reinterpret_cast<const unsigned char *>(in.data()), in.size(), true);
        ctx.check((whole.status == GTEXT_ENCODING_E_MALFORMED) == mf.failed, "decoder: fatal mode's verdict is not the model's");
        ctx.check(whole.bytes == utf8_of_cps(mf.cps), "decoder: fatal mode's output before the failure is not the model's", show(whole.bytes) + " vs " + show(utf8_of_cps(mf.cps)));
        if (mf.failed) {
          ctx.check(whole.error_offset == mf.error_offset, "decoder: fatal mode's offset is not the model's", std::to_string(whole.error_offset) + " vs " + std::to_string(mf.error_offset));
        }
      }
    }
    if (e == GTEXT_ENCODING_ISO_2022_JP && (o1 & 1)) {
      check_iso_resume(ctx, o, in, rng);
    }
  } else if (gtext_encoding_is_supported(e) && !o.bom_sniffing) {
    // One byte at a time as separate streams is the model of the encodings without state.
    std::string expect;
    size_t expected_replacements = 0;
    if (e == GTEXT_ENCODING_REPLACEMENT) {
      if (!in.empty()) {
        expect = utf8_of(0xFFFD);
        expected_replacements = 1;
      }
    } else {
      for (size_t i = 0; i < in.size(); i++) {
        const Out one = decode_with(e, orep, in.substr(i, 1), {});
        expect += one.bytes;
        expected_replacements += one.replacements;
      }
    }
    ctx.check(rep.bytes == expect, "decoder: not the byte-by-byte decode", show(rep.bytes) + " vs " + show(expect));
    ctx.check(rep.replacements == expected_replacements, "decoder: not the byte-by-byte count");
  }

  // Sniffing is the same as decoding what follows the mark in the encoding the mark chose, or the whole input in the encoding asked for
  // when there is none - for every encoding, the legacy ones too, which no model above covers.
  if (o.bom_sniffing) {
    GTEXT_Encoding chosen = e;
    size_t skip = 0;
    if (in.compare(0, 3, "\xEF\xBB\xBF") == 0) {
      chosen = GTEXT_ENCODING_UTF_8;
      skip = 3;
    } else if (in.compare(0, 2, "\xFE\xFF") == 0) {
      chosen = GTEXT_ENCODING_UTF_16BE;
      skip = 2;
    } else if (in.compare(0, 2, "\xFF\xFE") == 0) {
      chosen = GTEXT_ENCODING_UTF_16LE;
      skip = 2;
    }
    GTEXT_Decoder_Options plain = orep;
    plain.bom_sniffing = false;
    plain.bom_removal = false;
    const Out expected = decode_with(chosen, plain, in.substr(skip), {});
    ctx.check(rep.bytes == expected.bytes && rep.replacements == expected.replacements, "decoder: sniffing is not decoding what follows the mark",
        show(rep.bytes) + " vs " + show(expected.bytes));
    ctx.check(rep.bom_found == (skip != 0) && rep.encoding == chosen, "decoder: the mark or the encoding reported");
    GTEXT_Decoder_Options plain_fatal = plain;
    plain_fatal.error_mode = GTEXT_DECODER_FATAL;
    const Out expected_fatal = decode_with(chosen, plain_fatal, in.substr(skip), {});
    GTEXT_Decoder_Options sniff_fatal = o;
    sniff_fatal.error_mode = GTEXT_DECODER_FATAL;
    const Out got_fatal = decode_with(e, sniff_fatal, in, {});
    ctx.check(got_fatal.status == expected_fatal.status, "decoder: fatal mode after a mark differs from fatal mode without it");
    if (got_fatal.status == GTEXT_ENCODING_E_MALFORMED) {
      ctx.check(got_fatal.error_offset == skip + expected_fatal.error_offset, "decoder: the offset of an error after a mark does not count the mark",
          std::to_string(got_fatal.error_offset) + " vs " + std::to_string(skip + expected_fatal.error_offset));
    }
  }

  // What was read without error encodes back.
  if (!o.bom_sniffing && !o.bom_removal && rep.replacements == 0
      && (e == GTEXT_ENCODING_UTF_8 || e == GTEXT_ENCODING_X_USER_DEFINED || is_single_byte(e))) {
    GTEXT_Encoder_Options eo = encoder_options(GTEXT_ENCODER_FATAL);
    const Out back = encode_cut(e, eo, rep.bytes, {});
    ctx.check(back.status == GTEXT_ENCODING_OK && back.bytes == in, "decoder: what was decoded does not encode back", show(back));
  }

  // The output limit.
  const size_t limit = pick_limit(o3, rep.bytes.size());
  if (limit != 0 || (o3 & 7) == 2) {
    GTEXT_Decoder_Options ol = orep;
    ol.max_output_bytes = limit == 0 ? 1 : limit;
    const size_t eff = ol.max_output_bytes;
    const Out l = decode_with(e, ol, in, random_cuts(rng, in.size(), (o1 >> 4) & 15));
    if (eff < rep.bytes.size()) {
      // input_bytes of a whole-input call is the byte at which the limit was found: the stream up to the one before it fits, up to it does not.
      GTEXT_Decoder * d = nullptr;
      ctx.check(gtext_decoder_new(e, &ol, &d) == GTEXT_ENCODING_OK, "decoder: new failed");
      Collect c;
      GTEXT_Encoding_Sink sink = sink_of(c);
      GTEXT_Decoder_Result r;
      ctx.check(gtext_decoder_decode(d, in.data(), in.size(), true, &sink, &r) == GTEXT_ENCODING_E_LIMIT_OUTPUT, "decoder: the limit was not met");
      gtext_decoder_free(d);
      ctx.check(r.input_bytes >= 1 && r.input_bytes <= in.size(), "decoder: input_bytes out of range at a limit", std::to_string(r.input_bytes));
      ctx.check(decoded_prefix(e, orep, in, r.input_bytes - 1).size() <= eff, "decoder: input_bytes is past the byte at which the limit failed");
      ctx.check(decoded_prefix(e, orep, in, r.input_bytes).size() > eff, "decoder: input_bytes is short of the byte at which the limit failed",
          std::to_string(r.input_bytes));
    }
    if (eff >= rep.bytes.size()) {
      ctx.check(l.status == GTEXT_ENCODING_OK && l.bytes == rep.bytes, "decoder: a limit that fits changed the answer");
    } else {
      ctx.check(l.status == GTEXT_ENCODING_E_LIMIT_OUTPUT, "decoder: a limit below the output was not E_LIMIT_OUTPUT", show(l));
      std::string expected_prefix = boundary_prefix(rep.bytes, eff);
      if (is_multi_byte(rep.encoding)) {
        // (the encoding that decoded: with `bom_sniffing` a mark makes it a UTF one, and none leaves it the one asked for, over the whole input)
        // Two code points that one pair of bytes decodes to (Big5's four sequences) are written together or not at all.
        const ModelDecoded m = model_decode(e, reinterpret_cast<const unsigned char *>(in.data()), in.size(), false);
        size_t best = 0;
        for (uint32_t upto : m.steps) {
          const size_t end = utf8_of_cps(std::vector<uint32_t>(m.cps.begin(), m.cps.begin() + upto)).size();
          if (end <= eff) {
            best = end;
          }
        }
        expected_prefix = rep.bytes.substr(0, best);
      }
      ctx.check(l.bytes == expected_prefix, "decoder: the prefix before the limit is not the longest whole one", show(l.bytes) + " vs " + show(expected_prefix));
    }
  }

  // A sink that refuses.
  if (calls > 0 && whole.status == GTEXT_ENCODING_OK) {
    const size_t refuse = ((o3 >> 3) & 7) % (calls + 1);
    if (refuse < calls) {
      GTEXT_Decoder * d = nullptr;
      ctx.check(gtext_decoder_new(e, &o, &d) == GTEXT_ENCODING_OK, "decoder: new failed");
      Collect bad;
      bad.refuse_after = refuse;
      GTEXT_Encoding_Sink sink = sink_of(bad);
      GTEXT_Decoder_Result r;
      const GTEXT_Encoding_Status s = gtext_decoder_decode(d, in.data(), in.size(), true, &sink, &r);
      ctx.check(s == GTEXT_ENCODING_E_WRITE, "decoder: a refusing sink is not E_WRITE", gtext_encoding_status_string(s));
      ctx.check(bad.calls == refuse, "decoder: the sink was asked again after refusing");
      ctx.check(whole.bytes.compare(0, bad.bytes.size(), bad.bytes) == 0, "decoder: the sink's chunks are not a prefix");
      ctx.check(r.input_bytes >= 1 && r.input_bytes <= in.size(), "decoder: input_bytes out of range at a refusal");
      ctx.check(bad.bytes.size() <= decoded_prefix(e, orep, in, r.input_bytes - 1).size(), "decoder: input_bytes is before output that was delivered");
      ctx.check(decoded_prefix(e, orep, in, r.input_bytes).size() >= bad.bytes.size(), "decoder: output delivered is not of the bytes taken");
      Collect after;
      GTEXT_Encoding_Sink s2 = sink_of(after);
      ctx.check(gtext_decoder_decode(d, in.data(), in.size(), true, &s2, nullptr) == whole.status, "decoder: not usable after a refusal");
      ctx.check(after.bytes == whole.bytes, "decoder: another answer after a refusal");
      gtext_decoder_free(d);
    }
  }

  // The allocator.
  if (o3 & 0x40) {
    FaultAllocator fa;
    GTEXT_Decoder_Options oa = o;
    oa.allocator = &fa.alloc;
    for (size_t n = 1; n < 100; n++) {
      fa.arm(n);
      char * out = nullptr;
      size_t len = 0;
      GTEXT_Decoder_Result r;
      const GTEXT_Encoding_Status s = gtext_encoding_decode_buffer(e, &oa, in.data(), in.size(), &out, &len, &r);
      const bool failed = fa.failed;
      fa.disarm();
      if (s == GTEXT_ENCODING_OK) {
        ctx.check(std::string(out, len) == whole.bytes && whole.status == GTEXT_ENCODING_OK, "decoder: the buffer form differs");
        ctx.check(out[len] == '\0', "decoder: the buffer is not terminated");
        gtext_encoding_buffer_free(out);
      } else if (s == GTEXT_ENCODING_E_OOM) {
        ctx.check(failed && out == nullptr, "decoder: E_OOM without a failure, or a buffer after it");
      } else {
        ctx.check(s == whole.status && out == nullptr, "decoder: the buffer form fails differently");
      }
      ctx.check(fa.live.empty(), "decoder: a leak after a failed allocation", std::to_string(n));
      ctx.check(fa.bad_frees == 0, "decoder: a free of a block the allocator did not make");
      if (!failed) {
        break;
      }
    }
    // The stream allocates nothing once the decoder exists.
    GTEXT_Decoder * d = nullptr;
    fa.arm(1000000);
    if (gtext_decoder_new(e, &oa, &d) == GTEXT_ENCODING_OK) {
      const size_t base = fa.calls;
      Collect c;
      GTEXT_Encoding_Sink sink = sink_of(c);
      gtext_decoder_decode(d, in.data(), in.size(), true, &sink, nullptr);
      ctx.check(fa.calls == base, "decoder: a decode allocated");
      gtext_decoder_free(d);
    }
    fa.disarm();
    ctx.check(fa.live.empty(), "decoder: a leak");
  }
}

// ---- the encoder -------------------------------------------------------------------------------------------------

Out encode_with(GTEXT_Encoding e, const GTEXT_Encoder_Options & o, const std::string & in, const std::vector<size_t> & cuts,
    size_t * calls = nullptr, size_t * largest = nullptr) {
  Out out;
  GTEXT_Encoder * enc = nullptr;
  out.status = gtext_encoder_new(e, &o, &enc);
  if (out.status != GTEXT_ENCODING_OK) {
    return out;
  }
  Collect c;
  GTEXT_Encoding_Sink sink = sink_of(c);
  std::vector<size_t> points = cuts;
  points.push_back(in.size());
  size_t start = 0;
  for (size_t k = 0; k < points.size(); k++) {
    const size_t at = points[k] < start ? start : points[k];
    GTEXT_Encoder_Result r;
    const GTEXT_Encoding_Status s = gtext_encoder_encode(enc, in.data() + start, at - start, k + 1 == points.size(), &sink, &r);
    out.replacements += r.replacements;
    if (s != GTEXT_ENCODING_OK) {
      out.status = s;
      out.error_offset = r.error_offset;
      out.error_code_point = r.error_code_point;
      break;
    }
    start = at;
  }
  out.bytes = c.bytes;
  if (calls) {
    *calls = c.calls;
  }
  if (largest) {
    *largest = c.largest;
  }
  gtext_encoder_free(enc);
  return out;
}

// What the encoder writes for one code point, from the encoder itself, one code point to a stream: "" if it cannot.
bool encode_one(GTEXT_Encoding e, uint32_t cp, std::string * bytes) {
  const Out o = encode_cut(e, encoder_options(GTEXT_ENCODER_FATAL), utf8_of(cp), {});
  if (o.status == GTEXT_ENCODING_OK) {
    *bytes = o.bytes;
    return true;
  }
  return false;
}

void check_encoder(const Ctx & ctx, GTEXT_Encoding e, unsigned o2, unsigned o3, const std::string & in, Rng & rng) {
  const unsigned mode_bits = o2 & 3;
  GTEXT_Encoder_Options o = gtext_encoder_options_default();
  if (mode_bits == 3) {
    o.error_mode = GTEXT_Encoder_Error_Mode(3);
    GTEXT_Encoder * enc = nullptr;
    ctx.check(gtext_encoder_new(e, &o, &enc) == GTEXT_ENCODING_E_OPTIONS && !enc, "encoder: a mode that is none is not E_OPTIONS");
    return;
  }
  o.error_mode = GTEXT_Encoder_Error_Mode(mode_bits); // FATAL, HTML, REPLACEMENT
  const unsigned fineness = (o2 >> 4) & 15;

  size_t calls = 0, largest = 0;
  const Out whole = encode_with(e, o, in, {}, &calls, &largest);
  ctx.check(largest <= 4096, "encoder: a chunk above four kilobytes");
  ctx.check(whole.error_offset <= in.size(), "encoder: the offset is beyond the stream");
  const Out bytewise = encode_with(e, o, in, every_byte(in.size()));
  ctx.check(bytewise == whole, "encoder: byte-at-a-time differs from whole", show(bytewise) + " vs " + show(whole));
  const Out randomly = encode_with(e, o, in, random_cuts(rng, in.size(), fineness));
  ctx.check(randomly == whole, "encoder: random cuts differ from whole", show(randomly) + " vs " + show(whole));

  // The model: each code point of the reference decoder on its own; for the multi-byte encodings the standard's encoders written again (their escape sequences of
  // ISO-2022-JP make the code points not independent).
  const RefResult ref = ref_utf8(in);
  std::string html_expected, repl_expected;
  std::vector<size_t> ends_html, ends_repl; // where each code point's output ends: the limit never cuts inside one
  size_t first_bad = SIZE_MAX, first_unencodable = SIZE_MAX;
  const bool multi = is_multi_byte(e);
  ModelEncodeResult m_html, m_repl, m_fatal;
  std::set<size_t> m_errors;
  if (multi) {
    m_html = model_encode_ex(e, ref.cps, ModelMode::Html);
    m_repl = model_encode_ex(e, ref.cps, ModelMode::Replacement);
    m_fatal = model_encode_ex(e, ref.cps, ModelMode::Fatal);
    html_expected = m_html.bytes;
    repl_expected = m_repl.bytes;
    ends_html = m_html.atoms;
    ends_repl = m_repl.atoms;
    m_errors.insert(m_html.errors.begin(), m_html.errors.end());
    first_unencodable = m_fatal.failed ? m_fatal.error_index : SIZE_MAX;
    for (size_t k = 0; k < ref.cps.size(); k++) {
      if (ref.bad[k]) {
        first_bad = k;
        break;
      }
    }
  }
  for (size_t k = 0; !multi && k < ref.cps.size(); k++) {
    std::string b;
    if (ref.bad[k] && first_bad == SIZE_MAX) {
      first_bad = k;
    }
    if (encode_one(e, ref.cps[k], &b)) {
      html_expected += b;
      repl_expected += b;
    } else {
      html_expected += "&#" + std::to_string(ref.cps[k]) + ";";
      repl_expected += "?";
      if (first_unencodable == SIZE_MAX) {
        first_unencodable = k;
      }
    }
    ends_html.push_back(html_expected.size());
    ends_repl.push_back(repl_expected.size());
  }
  ctx.check(html_expected.size() <= 10 * in.size() + 10, "encoder: output more than ten times the input");

  if (o.error_mode == GTEXT_ENCODER_FATAL) {
    const size_t stop = std::min(first_bad, first_unencodable);
    if (stop == SIZE_MAX) {
      ctx.check(whole.status == GTEXT_ENCODING_OK && whole.bytes == html_expected, "encoder: fatal mode differs from the model",
          show(whole) + " vs " + show(html_expected));
    } else {
      const bool malformed = stop == first_bad;
      ctx.check(whole.status == (malformed ? GTEXT_ENCODING_E_MALFORMED : GTEXT_ENCODING_E_UNENCODABLE), "encoder: fatal mode's status",
          show(whole));
      std::string prefix;
      if (multi) {
        // The stream is over at the error: no closing escape. At an unencodable code point the model stops where the library does.
        prefix = malformed ? model_encode_ex(e, std::vector<uint32_t>(ref.cps.begin(), ref.cps.begin() + stop), ModelMode::Fatal, false).bytes : m_fatal.bytes;
      } else {
        for (size_t k = 0; k < stop; k++) {
          std::string b;
          encode_one(e, ref.cps[k], &b);
          prefix += b;
        }
      }
      ctx.check(whole.bytes == prefix, "encoder: fatal mode's output before the failure", show(whole) + " vs " + show(prefix));
      if (!malformed) {
        ctx.check(whole.error_code_point == (multi ? m_fatal.error_cp : ref.cps[stop]), "encoder: the code point of the failure");
        ctx.check(whole.error_offset == ref.starts[stop], "encoder: the offset of the failure",
            std::to_string(whole.error_offset) + " vs " + std::to_string(ref.starts[stop]));
      }
    }
  } else {
    ctx.check(whole.status == GTEXT_ENCODING_OK, "encoder: HTML and replacement modes failed", show(whole));
    ctx.check(whole.bytes == (o.error_mode == GTEXT_ENCODER_HTML ? html_expected : repl_expected), "encoder: not the model's output",
        show(whole) + " vs " + show(o.error_mode == GTEXT_ENCODER_HTML ? html_expected : repl_expected));
    // A replaced malformed sequence that is also unencodable is one replacement, not two.
    size_t expect_count = 0;
    for (size_t k = 0; k < ref.cps.size(); k++) {
      std::string b;
      if (multi ? (ref.bad[k] || m_errors.count(k)) : (ref.bad[k] || !encode_one(e, ref.cps[k], &b))) {
        expect_count++;
      }
    }
    ctx.check(whole.replacements == expect_count, "encoder: the number of replacements", std::to_string(whole.replacements) + " vs " + std::to_string(expect_count));
    // The fatal run's output is a prefix of this one.
    GTEXT_Encoder_Options of = o;
    of.error_mode = GTEXT_ENCODER_FATAL;
    const Out f = encode_with(e, of, in, {});
    ctx.check(whole.bytes.compare(0, f.bytes.size(), f.bytes) == 0, "encoder: fatal output is not a prefix");
    if (f.status == GTEXT_ENCODING_OK) {
      ctx.check(f.bytes == whole.bytes, "encoder: fatal succeeded with other output");
    }
  }

  // What a multi-byte encoder writes decodes, without an error, as the standard's decoder reads it (the model: not always as the text, which the standard folds
  // in places).
  if (multi && whole.status == GTEXT_ENCODING_OK && o.error_mode != GTEXT_ENCODER_FATAL) {
    const Out back = decode_whole(e, whole.bytes);
    const ModelDecoded m = model_decode(e, reinterpret_cast<const unsigned char *>(whole.bytes.data()), whole.bytes.size(), false);
    ctx.check(back.status == GTEXT_ENCODING_OK && back.bytes == utf8_of_cps(m.cps), "encoder: what was written is not read as the model reads it", show(back));
    ctx.check(m.replacements == 0, "encoder: what was written has an error in it", show(whole.bytes));
  }

  // What is written decodes back to the repaired text, where the encoding is one whose decoder is faithful.
  if (whole.status == GTEXT_ENCODING_OK && o.error_mode == GTEXT_ENCODER_FATAL
      && (e == GTEXT_ENCODING_UTF_8 || e == GTEXT_ENCODING_X_USER_DEFINED || is_single_byte(e))) {
    const Out back = decode_whole(e, whole.bytes);
    ctx.check(back.status == GTEXT_ENCODING_OK && back.replacements == 0 && back.bytes == utf8_of_cps(ref.cps),
        "encoder: what was written does not decode back", show(back));
  }

  // The limit.
  const size_t eff = pick_limit(o3, whole.bytes.size());
  if (eff != 0 && whole.status == GTEXT_ENCODING_OK) {
    GTEXT_Encoder_Options ol = o;
    ol.max_output_bytes = eff;
    const Out l = encode_with(e, ol, in, random_cuts(rng, in.size(), fineness));
    if (eff >= whole.bytes.size()) {
      ctx.check(l.status == GTEXT_ENCODING_OK && l.bytes == whole.bytes, "encoder: a limit that fits changed the answer");
    } else {
      ctx.check(l.status == GTEXT_ENCODING_E_LIMIT_OUTPUT, "encoder: a limit below the output was not E_LIMIT_OUTPUT", show(l));
      ctx.check(l.bytes.size() <= eff && whole.bytes.compare(0, l.bytes.size(), l.bytes) == 0, "encoder: the prefix before the limit");
      // ... and it is the longest prefix that ends between characters and references: the limit is never inside one, and nothing that fit is left out.
      const std::vector<size_t> & ends = o.error_mode == GTEXT_ENCODER_REPLACEMENT ? ends_repl : ends_html;
      size_t best = 0;
      for (size_t end : ends) {
        if (end <= eff) {
          best = end;
        }
      }
      ctx.check(l.bytes == whole.bytes.substr(0, best), "encoder: the prefix before the limit is not the longest whole one",
          show(l.bytes) + " vs " + show(whole.bytes.substr(0, best)));
      // input_bytes: the byte at which the limit was found.
      GTEXT_Encoder * x = nullptr;
      ctx.check(gtext_encoder_new(e, &ol, &x) == GTEXT_ENCODING_OK, "encoder: new failed");
      Collect c;
      GTEXT_Encoding_Sink sink = sink_of(c);
      GTEXT_Encoder_Result r;
      ctx.check(gtext_encoder_encode(x, in.data(), in.size(), true, &sink, &r) == GTEXT_ENCODING_E_LIMIT_OUTPUT, "encoder: the limit was not met");
      gtext_encoder_free(x);
      ctx.check(r.input_bytes >= 1 && r.input_bytes <= in.size(), "encoder: input_bytes out of range at a limit");
      ctx.check(encoded_prefix(e, o, in, r.input_bytes - 1).size() <= eff, "encoder: input_bytes is past the byte at which the limit failed");
      ctx.check(encoded_prefix(e, o, in, r.input_bytes).size() > eff, "encoder: input_bytes is short of the byte at which the limit failed",
          std::to_string(r.input_bytes));
    }
  }

  // A sink that refuses.
  if (calls > 0 && whole.status == GTEXT_ENCODING_OK) {
    const size_t refuse = ((o3 >> 3) & 7) % (calls + 1);
    if (refuse < calls) {
      GTEXT_Encoder * enc = nullptr;
      ctx.check(gtext_encoder_new(e, &o, &enc) == GTEXT_ENCODING_OK, "encoder: new failed");
      Collect bad;
      bad.refuse_after = refuse;
      GTEXT_Encoding_Sink sink = sink_of(bad);
      const GTEXT_Encoding_Status s = gtext_encoder_encode(enc, in.data(), in.size(), true, &sink, nullptr);
      ctx.check(s == GTEXT_ENCODING_E_WRITE, "encoder: a refusing sink is not E_WRITE", gtext_encoding_status_string(s));
      ctx.check(bad.calls == refuse, "encoder: the sink was asked again after refusing");
      {
        GTEXT_Encoder * y = nullptr;
        ctx.check(gtext_encoder_new(e, &o, &y) == GTEXT_ENCODING_OK, "encoder: new failed");
        Collect again;
        again.refuse_after = refuse;
        GTEXT_Encoding_Sink s5 = sink_of(again);
        GTEXT_Encoder_Result rr;
        gtext_encoder_encode(y, in.data(), in.size(), true, &s5, &rr);
        gtext_encoder_free(y);
        ctx.check(rr.input_bytes >= 1 && rr.input_bytes <= in.size(), "encoder: input_bytes out of range at a refusal");
        ctx.check(again.bytes.size() <= encoded_prefix(e, o, in, rr.input_bytes).size(), "encoder: output delivered is not of the bytes taken");
      }
      ctx.check(whole.bytes.compare(0, bad.bytes.size(), bad.bytes) == 0, "encoder: the sink's chunks are not a prefix");
      Collect after;
      GTEXT_Encoding_Sink s2 = sink_of(after);
      ctx.check(gtext_encoder_encode(enc, in.data(), in.size(), true, &s2, nullptr) == GTEXT_ENCODING_OK && after.bytes == whole.bytes,
          "encoder: not usable after a refusal");
      gtext_encoder_free(enc);
    }
  }

  // The allocator.
  if (o3 & 0x40) {
    FaultAllocator fa;
    GTEXT_Encoder_Options oa = o;
    oa.allocator = &fa.alloc;
    for (size_t n = 1; n < 100; n++) {
      fa.arm(n);
      char * out = nullptr;
      size_t len = 0;
      GTEXT_Encoder_Result r;
      const GTEXT_Encoding_Status s = gtext_encoding_encode_buffer(e, &oa, in.data(), in.size(), &out, &len, &r);
      const bool failed = fa.failed;
      fa.disarm();
      if (s == GTEXT_ENCODING_OK) {
        ctx.check(std::string(out, len) == whole.bytes && whole.status == GTEXT_ENCODING_OK, "encoder: the buffer form differs");
        gtext_encoding_buffer_free(out);
      } else if (s == GTEXT_ENCODING_E_OOM) {
        ctx.check(failed && out == nullptr, "encoder: E_OOM without a failure, or a buffer after it");
      } else {
        ctx.check(s == whole.status && out == nullptr, "encoder: the buffer form fails differently");
      }
      ctx.check(fa.live.empty(), "encoder: a leak after a failed allocation", std::to_string(n));
      ctx.check(fa.bad_frees == 0, "encoder: a free of a block the allocator did not make");
      if (!failed) {
        break;
      }
    }
    // The stream allocates nothing once the encoder exists.
    GTEXT_Encoder * x = nullptr;
    fa.arm(1000000);
    if (gtext_encoder_new(e, &oa, &x) == GTEXT_ENCODING_OK) {
      const size_t base = fa.calls;
      Collect c;
      GTEXT_Encoding_Sink sink = sink_of(c);
      gtext_encoder_encode(x, in.data(), in.size(), true, &sink, nullptr);
      ctx.check(fa.calls == base, "encoder: an encode allocated");
      gtext_encoder_free(x);
    }
    fa.disarm();
    ctx.check(fa.live.empty(), "encoder: a leak");
  }
}

// ---- misuse -------------------------------------------------------------------------------------------------------

void check_misuse(const Ctx & ctx, GTEXT_Encoding e, const std::string & in) {
  Collect c;
  GTEXT_Encoding_Sink sink = sink_of(c);
  GTEXT_Encoding_Sink nowrite = {nullptr, nullptr};
  GTEXT_Decoder * d = nullptr;
  GTEXT_Encoder * enc = nullptr;
  ctx.check(gtext_decoder_new(e, nullptr, nullptr) == GTEXT_ENCODING_E_INVALID, "misuse: new with no output");
  ctx.check(gtext_encoder_new(e, nullptr, nullptr) == GTEXT_ENCODING_E_INVALID, "misuse: new with no output");
  ctx.check(gtext_decoder_decode(nullptr, in.data(), in.size(), true, &sink, nullptr) == GTEXT_ENCODING_E_INVALID, "misuse: no decoder");
  ctx.check(gtext_encoder_encode(nullptr, in.data(), in.size(), true, &sink, nullptr) == GTEXT_ENCODING_E_INVALID, "misuse: no encoder");
  char * out = nullptr;
  size_t len = 0;
  ctx.check(gtext_encoding_decode_buffer(e, nullptr, in.data(), in.size(), nullptr, &len, nullptr) == GTEXT_ENCODING_E_INVALID, "misuse: no out");
  ctx.check(gtext_encoding_encode_buffer(e, nullptr, in.data(), in.size(), nullptr, &len, nullptr) == GTEXT_ENCODING_E_INVALID, "misuse: no out");
  if (valid_encoding(e)) {
    if (gtext_decoder_new(e, nullptr, &d) == GTEXT_ENCODING_OK) {
      ctx.check(gtext_decoder_decode(d, in.data(), in.size(), true, nullptr, nullptr) == GTEXT_ENCODING_E_INVALID, "misuse: no sink");
      ctx.check(gtext_decoder_decode(d, in.data(), in.size(), true, &nowrite, nullptr) == GTEXT_ENCODING_E_INVALID, "misuse: no write");
      ctx.check(gtext_decoder_decode(d, nullptr, 1, true, &sink, nullptr) == GTEXT_ENCODING_E_INVALID, "misuse: no data");
      // No bytes and no pointer is a legal call (a flush); clang's pointer-overflow check sees arithmetic on the null pointer.
      ctx.check(gtext_decoder_decode(d, nullptr, 0, false, &sink, nullptr) == GTEXT_ENCODING_OK, "misuse: an empty piece");
      ctx.check(gtext_decoder_decode(d, nullptr, 0, true, &sink, nullptr) == GTEXT_ENCODING_OK, "misuse: an empty flush");
      gtext_decoder_free(d);
    }
    if (gtext_encoder_new(e, nullptr, &enc) == GTEXT_ENCODING_OK) {
      ctx.check(gtext_encoder_encode(enc, in.data(), in.size(), true, nullptr, nullptr) == GTEXT_ENCODING_E_INVALID, "misuse: no sink");
      ctx.check(gtext_encoder_encode(enc, nullptr, 1, true, &sink, nullptr) == GTEXT_ENCODING_E_INVALID, "misuse: no data");
      ctx.check(gtext_encoder_encode(enc, nullptr, 0, true, &sink, nullptr) == GTEXT_ENCODING_OK, "misuse: an empty flush");
      gtext_encoder_free(enc);
    }
  } else {
    ctx.check(gtext_decoder_new(e, nullptr, &d) == GTEXT_ENCODING_E_INVALID && !d, "misuse: a value that is no encoding (decoder)");
    ctx.check(gtext_encoder_new(e, nullptr, &enc) == GTEXT_ENCODING_E_INVALID && !enc, "misuse: a value that is no encoding (encoder)");
  }
  (void)out;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 4) {
    return 0;
  }
  const unsigned o0 = data[0], o1 = data[1], o2 = data[2], o3 = data[3];
  Ctx ctx;
  ctx.input.assign(reinterpret_cast<const char *>(data), size);
  std::string in(reinterpret_cast<const char *>(data) + 4, size - 4 > 65536 ? 65536 : size - 4);
  const GTEXT_Encoding e = pick_encoding(o0);
  Rng rng(uint64_t(o0) * 1000003u + size * 31u + (size > 4 ? data[4] : 0));

  if (!valid_encoding(e)) {
    g_refused_values++;
    check_misuse(ctx, e, in);
    return 0;
  }
  GTEXT_Decoder * d = nullptr;
  const GTEXT_Encoding_Status ds = gtext_decoder_new(e, nullptr, &d);
  if (ds == GTEXT_ENCODING_OK) {
    gtext_decoder_free(d);
    ctx.check(gtext_encoding_is_supported(e), "a decoder for an encoding that is not supported");
    g_decoded_inputs++;
    check_decoder(ctx, e, o1, o3, in, rng);
  } else {
    ctx.check(false, "no decoder for an encoding of the standard", gtext_encoding_status_string(ds)); // (every one of the 40 has one)
  }
  GTEXT_Encoder * enc = nullptr;
  const GTEXT_Encoding_Status es = gtext_encoder_new(e, nullptr, &enc);
  if (es == GTEXT_ENCODING_OK) {
    gtext_encoder_free(enc);
    g_encoded_inputs++;
    check_encoder(ctx, e, o2, o3, in, rng);
  } else {
    const bool no_encoder = e == GTEXT_ENCODING_REPLACEMENT || e == GTEXT_ENCODING_UTF_16BE || e == GTEXT_ENCODING_UTF_16LE;
    ctx.check(no_encoder && es == GTEXT_ENCODING_E_NO_ENCODER, "an encoder with the wrong status", gtext_encoding_status_string(es));
  }
  if (o3 & 0x80) {
    check_misuse(ctx, e, in);
  }
  return 0;
}
