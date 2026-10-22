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
 * The decoder's source map (gtext_decoder_decode_mapped): the offset, in the original bytes, of every code point it writes, and
 * the flag for a U+FFFD that stands for ill-formed input. This is what lets the HTML parser report positions in the bytes it was
 * given for a Shift_JIS or UTF-16 document, so it is held to four things:
 *
 *  1. the bytes are those of the plain decoder, in every encoding, whatever the cuts;
 *  2. the map is the same whatever the cuts (every single cut of short inputs, one byte at a time, random cuts of long ones);
 *  3. **the slices tile**: for every encoding but ISO-2022-JP (whose characters depend on the mode an earlier escape sequence set),
 *     the bytes from one code point's offset to the next one's decode, alone, to that code point - which is what "the offset of
 *     the sequence that gave it" means, checked by the decoder itself rather than by a second description of each encoding;
 *  4. hand-worked cases for the places a map can lie: ill-formed UTF-8, a surrogate pair, a lone trail surrogate, a restored byte,
 *     Big5's pair, a truncated sequence at the end, a byte order mark, an ISO-2022-JP escape sequence.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "encoding_model.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>

using namespace enctest;

namespace {

struct Mapped {
  GTEXT_Encoding_Status status = GTEXT_ENCODING_OK;
  std::string bytes;
  std::vector<size_t> offsets; // one per byte
  std::vector<unsigned char> replaced;
  std::vector<unsigned char> widths;
  size_t calls = 0;
  bool operator==(const Mapped & o) const {
    return status == o.status && bytes == o.bytes && offsets == o.offsets && replaced == o.replaced && widths == o.widths;
  }
};

bool mapped_write(void * user, const char * bytes, size_t len, const GTEXT_Encoding_Source_Map * map) {
  auto * m = static_cast<Mapped *>(user);
  m->calls++;
  m->bytes.append(bytes, len);
  m->offsets.insert(m->offsets.end(), map->offsets, map->offsets + len);
  m->replaced.insert(m->replaced.end(), map->replaced, map->replaced + len);
  m->widths.insert(m->widths.end(), map->widths, map->widths + len);
  return true;
}

Mapped decode_mapped(GTEXT_Encoding enc, const std::string & in, const std::vector<size_t> & cuts,
    bool bom_sniffing = false, size_t max_output = 0) {
  Mapped m;
  GTEXT_Decoder_Options o = gtext_decoder_options_default();
  o.track_offsets = true;
  o.bom_sniffing = bom_sniffing;
  o.max_output_bytes = max_output;
  GTEXT_Decoder * d = nullptr;
  m.status = gtext_decoder_new(enc, &o, &d);
  if (m.status != GTEXT_ENCODING_OK) {
    return m;
  }
  GTEXT_Encoding_Mapped_Sink sink = {mapped_write, &m};
  std::vector<size_t> points = cuts;
  points.push_back(in.size());
  size_t start = 0;
  for (size_t k = 0; k < points.size(); k++) {
    const bool last = k + 1 == points.size();
    const GTEXT_Encoding_Status s =
        gtext_decoder_decode_mapped(d, in.data() + start, points[k] - start, last, &sink, nullptr);
    if (s != GTEXT_ENCODING_OK) {
      m.status = s;
      break;
    }
    start = points[k];
  }
  gtext_decoder_free(d);
  return m;
}

// The same bytes through the plain interface.
std::string decode_plain(GTEXT_Encoding enc, const std::string & in, bool bom_sniffing = false) {
  GTEXT_Decoder_Options o = gtext_decoder_options_default();
  o.bom_sniffing = bom_sniffing;
  return decode_whole(enc, o, in).bytes;
}

struct MRng {
  uint64_t s;
  explicit MRng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ULL + 1) {}
  uint32_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return uint32_t(s >> 11);
  }
};

// A byte string that makes the decoders work: ASCII, the high bytes, the bytes the stateful and the escape-driven ones watch for.
std::string random_input(MRng & r, size_t n) {
  static const unsigned char special[] = {0x1B, 0x24, 0x28, 0x42, 0x4A, 0x49, 0x0E, 0x0F, 0x8E, 0x8F, 0x80, 0x81, 0xA0, 0xFE, 0xFF,
      0xC2, 0xE0, 0xED, 0xF0, 0xF4, 0xD8, 0xDC, 0x00, 0x7F, 0x21, 0x7E, 0xA1, 0x30, 0x39};
  std::string s;
  for (size_t i = 0; i < n; i++) {
    const uint32_t k = r.next() % 8;
    if (k < 2) {
      s += char('a' + r.next() % 26);
    } else if (k < 5) {
      s += char(special[r.next() % sizeof(special)]);
    } else {
      s += char(r.next() & 0xFF);
    }
  }
  return s;
}

struct Cp {
  size_t offset;
  bool replaced;
  std::string utf8;
};

// The code points of a mapped decode, one entry each (UTF-8 split by hand).
std::vector<Cp> split(const Mapped & m) {
  std::vector<Cp> out;
  size_t i = 0;
  while (i < m.bytes.size()) {
    const unsigned char b = (unsigned char)m.bytes[i];
    const size_t n = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
    out.push_back({m.offsets[i], m.replaced[i] != 0, m.bytes.substr(i, n)});
    for (size_t k = 1; k < n; k++) { // every byte of a code point shares its entry
      EXPECT_EQ(m.offsets[i + k], m.offsets[i]);
      EXPECT_EQ(m.replaced[i + k], m.replaced[i]);
    }
    i += n;
  }
  return out;
}

std::vector<GTEXT_Encoding> all_encodings() {
  std::vector<GTEXT_Encoding> v;
  for (int e = int(GTEXT_ENCODING_NONE) + 1; e < int(GTEXT_ENCODING_COUNT); e++) {
    v.push_back(GTEXT_Encoding(e));
  }
  return v;
}

} // namespace

// 1 and 2 and 3.
TEST(EncodingMapped, EveryEncodingBytesCutsAndSlices) {
  MRng r(7);
  for (GTEXT_Encoding enc : all_encodings()) {
    for (int round = 0; round < 400; round++) {
      const std::string in = random_input(r, r.next() % 30);
      const std::string plain = decode_plain(enc, in);
      const Mapped whole = decode_mapped(enc, in, {});
      ASSERT_EQ(whole.status, GTEXT_ENCODING_OK) << gtext_encoding_name(enc) << " " << show(in);
      ASSERT_EQ(whole.bytes, plain) << gtext_encoding_name(enc) << " " << show(in);
      ASSERT_EQ(whole.offsets.size(), whole.bytes.size());
      // every single cut, and every byte on its own
      for (size_t cut = 0; cut <= in.size(); cut++) {
        const Mapped two = decode_mapped(enc, in, {cut});
        ASSERT_TRUE(two == whole) << gtext_encoding_name(enc) << " cut at " << cut << " of " << show(in);
      }
      std::vector<size_t> each;
      for (size_t k = 1; k < in.size(); k++) {
        each.push_back(k);
      }
      ASSERT_TRUE(decode_mapped(enc, in, each) == whole) << gtext_encoding_name(enc) << " one byte at a time " << show(in);
      // the offsets rise, stay inside the input, and the slices tile (not for ISO-2022-JP: its characters depend on the mode)
      const std::vector<Cp> cps = split(whole);
      for (size_t k = 0; k < cps.size(); k++) {
        ASSERT_LE(cps[k].offset, in.size());
        if (k > 0) {
          ASSERT_LE(cps[k - 1].offset, cps[k].offset);
        }
      }
      if (enc == GTEXT_ENCODING_ISO_2022_JP) {
        continue;
      }
      size_t k = 0;
      size_t expect_start = 0;
      while (k < cps.size()) {
        size_t g = k + 1; // a group: the code points of one sequence (Big5's pair)
        while (g < cps.size() && cps[g].offset == cps[k].offset) {
          g++;
        }
        const size_t end = g < cps.size() ? cps[g].offset : in.size();
        ASSERT_EQ(cps[k].offset, expect_start) << gtext_encoding_name(enc) << " " << show(in) << " the slices do not tile";
        std::string group;
        for (size_t x = k; x < g; x++) {
          group += cps[x].utf8;
        }
        ASSERT_EQ(decode_plain(enc, in.substr(cps[k].offset, end - cps[k].offset)), group)
            << gtext_encoding_name(enc) << " " << show(in) << " slice " << cps[k].offset << ".." << end;
        expect_start = end;
        k = g;
      }
      if (cps.empty()) {
        ASSERT_TRUE(plain.empty());
      } else {
        ASSERT_EQ(expect_start, in.size()) << gtext_encoding_name(enc) << " " << show(in) << " the slices stop short";
      }
    }
  }
}

TEST(EncodingMapped, LongInputsAtRandomCuts) {
  MRng r(11);
  for (GTEXT_Encoding enc : all_encodings()) {
    const std::string in = random_input(r, 9000); // more than two chunks of output
    const Mapped whole = decode_mapped(enc, in, {});
    ASSERT_EQ(whole.status, GTEXT_ENCODING_OK);
    ASSERT_EQ(whole.bytes, decode_plain(enc, in));
    ASSERT_GE(whole.calls, 1u);
    for (int round = 0; round < 6; round++) {
      std::vector<size_t> cuts;
      for (size_t at = r.next() % 700; at < in.size(); at += 1 + r.next() % 1500) {
        cuts.push_back(at);
      }
      ASSERT_TRUE(decode_mapped(enc, in, cuts) == whole) << gtext_encoding_name(enc);
    }
  }
}

TEST(EncodingMapped, AsciiRunsAreLinear) {
  std::string in(10000, 'x');
  for (size_t i = 0; i < in.size(); i++) {
    in[i] = char('a' + i % 26);
  }
  for (GTEXT_Encoding enc : {GTEXT_ENCODING_UTF_8, GTEXT_ENCODING_WINDOWS_1252, GTEXT_ENCODING_SHIFT_JIS, GTEXT_ENCODING_EUC_KR}) {
    const Mapped m = decode_mapped(enc, in, {17, 4096, 5000});
    ASSERT_EQ(m.bytes, in);
    for (size_t i = 0; i < in.size(); i++) {
      ASSERT_EQ(m.offsets[i], i);
      ASSERT_EQ(m.replaced[i], 0);
    }
  }
}

TEST(EncodingMapped, Utf8IllFormed) {
  // "a", C3 A9 (e acute), a stray 80, E2 82 (truncated, then "b"), F0 9F 98 80 (U+1F600), FF.
  const std::string in = std::string("a") + "\xC3\xA9" + "\x80" + "\xE2\x82" + "b" + "\xF0\x9F\x98\x80" + "\xFF";
  const Mapped m = decode_mapped(GTEXT_ENCODING_UTF_8, in, {});
  const std::vector<Cp> cps = split(m);
  ASSERT_EQ(cps.size(), 7u);
  const struct { size_t off; bool rep; } want[] = {{0, false}, {1, false}, {3, true}, {4, true}, {6, false}, {7, false}, {11, true}};
  for (size_t i = 0; i < 7; i++) {
    EXPECT_EQ(cps[i].offset, want[i].off) << i;
    EXPECT_EQ(cps[i].replaced, want[i].rep) << i;
  }
  // an input U+FFFD is a code point, not a replacement
  const Mapped lit = decode_mapped(GTEXT_ENCODING_UTF_8, "\xEF\xBF\xBD", {});
  ASSERT_EQ(lit.bytes, "\xEF\xBF\xBD");
  EXPECT_EQ(lit.replaced[0], 0);
}

TEST(EncodingMapped, Utf16) {
  // LE: 'a', U+20AC, U+1F600 as a surrogate pair, a lone trail surrogate DC00, then one odd byte.
  const std::string le = std::string("a\0\xAC\x20\x3D\xD8\x00\xDE\x00\xDCZ", 11);
  const Mapped m = decode_mapped(GTEXT_ENCODING_UTF_16LE, le, {});
  const std::vector<Cp> cps = split(m);
  ASSERT_EQ(cps.size(), 5u);
  const size_t offs[] = {0, 2, 4, 8, 10};
  const bool reps[] = {false, false, false, true, true};
  for (size_t i = 0; i < 5; i++) {
    EXPECT_EQ(cps[i].offset, offs[i]) << i;
    EXPECT_EQ(cps[i].replaced, reps[i]) << i;
  }
  // a lead surrogate followed by an ordinary unit: the replacement is the lead's, the unit after it stays itself
  const std::string be("\xD8\x3D\x00\x41", 4);
  const std::vector<Cp> b = split(decode_mapped(GTEXT_ENCODING_UTF_16BE, be, {}));
  ASSERT_EQ(b.size(), 2u);
  EXPECT_EQ(b[0].offset, 0u);
  EXPECT_TRUE(b[0].replaced);
  EXPECT_EQ(b[1].offset, 2u);
  EXPECT_FALSE(b[1].replaced);
  EXPECT_EQ(b[1].utf8, "A");
}

TEST(EncodingMapped, ShiftJisCharactersAreTwoBytes) {
  // "a", U+3042 (82 A0), "b", half-width katakana (B1), a bad trail (81 20), "c".
  const std::string in = std::string("a") + "\x82\xA0" + "b" + "\xB1" + "\x81 " + "c";
  const std::vector<Cp> cps = split(decode_mapped(GTEXT_ENCODING_SHIFT_JIS, in, {}));
  ASSERT_EQ(cps.size(), 7u);
  const size_t offs[] = {0, 1, 3, 4, 5, 6, 7};
  for (size_t i = 0; i < cps.size(); i++) {
    EXPECT_EQ(cps[i].offset, offs[i]) << i;
  }
  EXPECT_TRUE(cps[4].replaced); // the 81 whose trail byte was a space
  EXPECT_FALSE(cps[5].replaced);
  EXPECT_EQ(cps[5].utf8, " ");  // the space was restored, not swallowed
}

TEST(EncodingMapped, Big5PairShareAnOffset) {
  // Big5 pointer 1133: 0x88 0x62 decodes to two code points (U+00CA U+0304).
  const std::string in = std::string("x") + "\x88\x62" + "y";
  const std::vector<Cp> cps = split(decode_mapped(GTEXT_ENCODING_BIG5, in, {}));
  ASSERT_EQ(cps.size(), 4u);
  EXPECT_EQ(cps[1].offset, 1u);
  EXPECT_EQ(cps[2].offset, 1u);
  EXPECT_EQ(cps[3].offset, 3u);
}

TEST(EncodingMapped, Gb18030RestoredBytes) {
  // 81 30 81 then a byte that cannot end a four-byte form: 81 is replaced, and 30, 81 and the byte are read again - 30 as
  // itself, 81 41 as a two-byte character (U+4E04) that begins where the restored 81 was.
  const std::string in = std::string("\x81\x30\x81", 3) + "A";
  const std::vector<Cp> cps = split(decode_mapped(GTEXT_ENCODING_GB18030, in, {}));
  ASSERT_EQ(cps.size(), 3u);
  EXPECT_EQ(cps[0].offset, 0u);
  EXPECT_TRUE(cps[0].replaced);
  EXPECT_EQ(cps[1].offset, 1u);
  EXPECT_EQ(cps[1].utf8, "0");
  EXPECT_EQ(cps[2].offset, 2u);
  EXPECT_EQ(cps[2].utf8, "\xE4\xB8\x84");
}

// The width of a code point is the bytes of the input it was decoded from, so that its offset plus its width is the place just after it. Where
// a shift sequence lies between two characters that place is not where the next one begins, which is why a reader needs both.
TEST(EncodingMapped, WidthsAreTheBytesOfEachCodePoint) {
  struct Case {
    GTEXT_Encoding enc;
    std::string in;
    std::vector<std::pair<size_t, size_t>> want; // (offset, width) of each code point
  };
  const std::vector<Case> cases = {
    {GTEXT_ENCODING_UTF_8, "a\xC3\xA9\xE3\x81\x82\xF0\x9F\x98\x80", {{0, 1}, {1, 2}, {3, 3}, {6, 4}}},
    {GTEXT_ENCODING_UTF_16LE, std::string("a\0", 2) + std::string("\x42\x30", 2) + std::string("\x3D\xD8\x00\xDE", 4), {{0, 2}, {2, 2}, {4, 4}}},
    {GTEXT_ENCODING_UTF_16BE, std::string("\0a", 2) + std::string("\xD8\x3D\xDE\x00", 4), {{0, 2}, {2, 4}}},
    {GTEXT_ENCODING_SHIFT_JIS, "a\x93\xFA\xB1z", {{0, 1}, {1, 2}, {3, 1}, {4, 1}}},
    {GTEXT_ENCODING_EUC_JP, "a\x8F\xB0\xA1\xA4\xA2", {{0, 1}, {1, 3}, {4, 2}}},
    {GTEXT_ENCODING_GB18030, "a\x81\x30\x81\x30\xB0\xA1", {{0, 1}, {1, 4}, {5, 2}}},
    // an ill-formed sequence is one replacement, as wide as the bytes of the maximal subpart
    {GTEXT_ENCODING_UTF_8, "a\xE3\x81z\xFF" "b", {{0, 1}, {1, 2}, {3, 1}, {4, 1}, {5, 1}}},
    // a sequence cut off by the end of the input
    {GTEXT_ENCODING_UTF_8, "ab\xF0\x9F\x98", {{0, 1}, {1, 1}, {2, 3}}},
    // escape sequences are no one's width: the kanji begins after its escape and the ASCII after the one that follows
    {GTEXT_ENCODING_ISO_2022_JP, "a\x1B$B\x46\x7C\x1B(Bz", {{0, 1}, {4, 2}, {9, 1}}},
  };
  for (const Case & c : cases) {
    for (const std::vector<size_t> & cuts : std::vector<std::vector<size_t>>{{}, {1}, {2, 3}}) {
      const Mapped m = decode_mapped(c.enc, c.in, cuts);
      ASSERT_EQ(m.status, GTEXT_ENCODING_OK);
      const std::vector<Cp> cps = split(m);
      ASSERT_EQ(cps.size(), c.want.size()) << c.in.size();
      size_t at = 0;
      for (size_t i = 0; i < cps.size(); i++) {
        EXPECT_EQ(cps[i].offset, c.want[i].first) << "case " << (int)c.enc << " code point " << i;
        EXPECT_EQ((size_t)m.widths[at], c.want[i].second) << "case " << (int)c.enc << " code point " << i;
        for (size_t k = 1; k < cps[i].utf8.size(); k++) {
          EXPECT_EQ(m.widths[at + k], m.widths[at]) << "all the bytes of a code point share its width";
        }
        at += cps[i].utf8.size();
      }
    }
  }
}

TEST(EncodingMapped, TruncatedSequenceAtTheEnd) {
  const std::string in = "ab\xE3\x81"; // a three-byte character cut after two bytes
  const std::vector<Cp> cps = split(decode_mapped(GTEXT_ENCODING_UTF_8, in, {}));
  ASSERT_EQ(cps.size(), 3u);
  EXPECT_EQ(cps[2].offset, 2u);
  EXPECT_TRUE(cps[2].replaced);
  // ... and when the cut is between the calls, not at the end of the input
  const std::vector<Cp> cut = split(decode_mapped(GTEXT_ENCODING_UTF_8, in, {3}));
  ASSERT_EQ(cut.size(), 3u);
  EXPECT_EQ(cut[2].offset, 2u);
}

TEST(EncodingMapped, ByteOrderMarkIsCountedNotMapped) {
  const std::string u8 = std::string("\xEF\xBB\xBF") + "ab";
  Mapped m = decode_mapped(GTEXT_ENCODING_WINDOWS_1252, u8, {1}, true);
  ASSERT_EQ(m.bytes, "ab");
  EXPECT_EQ(m.offsets[0], 3u);
  EXPECT_EQ(m.offsets[1], 4u);
  const std::string u16 = std::string("\xFF\xFE") + std::string("a\0", 2) + std::string("b\0", 2);
  m = decode_mapped(GTEXT_ENCODING_UTF_8, u16, {1, 2}, true);
  ASSERT_EQ(m.bytes, "ab");
  EXPECT_EQ(m.offsets[0], 2u);
  EXPECT_EQ(m.offsets[1], 4u);
  // A start that only looked like a mark: its bytes are input, with their own offsets.
  m = decode_mapped(GTEXT_ENCODING_WINDOWS_1252, std::string("\xEF\xBB") + "z", {1}, true);
  const std::vector<Cp> cps = split(m);
  ASSERT_EQ(cps.size(), 3u);
  EXPECT_EQ(cps[0].offset, 0u);
  EXPECT_EQ(cps[1].offset, 1u);
  EXPECT_EQ(cps[2].offset, 2u);
}

TEST(EncodingMapped, Iso2022JpEscapeSequencesAreGaps) {
  // "a", ESC $ B (JIS X 0208), U+3042 (24 22), ESC ( B, "b".
  const std::string in = std::string("a") + "\x1B$B" + "\x24\x22" + "\x1B(B" + "b";
  const std::vector<Cp> cps = split(decode_mapped(GTEXT_ENCODING_ISO_2022_JP, in, {}));
  ASSERT_EQ(cps.size(), 3u);
  EXPECT_EQ(cps[0].offset, 0u);
  EXPECT_EQ(cps[1].offset, 4u); // its own two bytes, not the escape sequence before them
  EXPECT_EQ(cps[1].utf8, "\xE3\x81\x82");
  EXPECT_EQ(cps[2].offset, 9u);
  // the same, cut at every place
  const Mapped whole = decode_mapped(GTEXT_ENCODING_ISO_2022_JP, in, {});
  for (size_t cut = 0; cut <= in.size(); cut++) {
    EXPECT_TRUE(decode_mapped(GTEXT_ENCODING_ISO_2022_JP, in, {cut}) == whole) << cut;
  }
  // an escape sequence that is wrong: the replacement starts at its first byte
  const std::vector<Cp> bad = split(decode_mapped(GTEXT_ENCODING_ISO_2022_JP, std::string("a") + "\x1B$B" + "\x1B$B" + "c", {}));
  ASSERT_GE(bad.size(), 2u);
  EXPECT_TRUE(bad[1].replaced);
  EXPECT_EQ(bad[1].offset, 4u);
}

TEST(EncodingMapped, ReplacementEncodingIsOneReplacement) {
  const std::vector<Cp> cps = split(decode_mapped(GTEXT_ENCODING_REPLACEMENT, "hz-gb-2312 stuff", {3}));
  ASSERT_EQ(cps.size(), 1u);
  EXPECT_EQ(cps[0].offset, 0u);
  EXPECT_TRUE(cps[0].replaced);
}

TEST(EncodingMapped, ArgumentsAndTheLimit) {
  // a decoder made without track_offsets cannot be asked for a map
  GTEXT_Decoder * d = nullptr;
  ASSERT_EQ(gtext_decoder_new(GTEXT_ENCODING_UTF_8, nullptr, &d), GTEXT_ENCODING_OK);
  Mapped m;
  GTEXT_Encoding_Mapped_Sink sink = {mapped_write, &m};
  EXPECT_EQ(gtext_decoder_decode_mapped(d, "a", 1, true, &sink, nullptr), GTEXT_ENCODING_E_INVALID);
  gtext_decoder_free(d);
  GTEXT_Decoder_Options o = gtext_decoder_options_default();
  o.track_offsets = true;
  ASSERT_EQ(gtext_decoder_new(GTEXT_ENCODING_UTF_8, &o, &d), GTEXT_ENCODING_OK);
  EXPECT_EQ(gtext_decoder_decode_mapped(d, nullptr, 1, true, &sink, nullptr), GTEXT_ENCODING_E_INVALID);
  GTEXT_Encoding_Mapped_Sink none = {nullptr, nullptr};
  EXPECT_EQ(gtext_decoder_decode_mapped(d, "a", 1, true, &none, nullptr), GTEXT_ENCODING_E_INVALID);
  EXPECT_EQ(gtext_decoder_decode_mapped(d, "a", 1, true, nullptr, nullptr), GTEXT_ENCODING_E_INVALID);
  EXPECT_EQ(gtext_decoder_decode_mapped(nullptr, "a", 1, true, &sink, nullptr), GTEXT_ENCODING_E_INVALID);
  // the plain interface of a tracking decoder still works
  Collect c;
  GTEXT_Encoding_Sink plain = sink_of(c);
  EXPECT_EQ(gtext_decoder_decode(d, "xy", 2, true, &plain, nullptr), GTEXT_ENCODING_OK);
  EXPECT_EQ(c.bytes, "xy");
  gtext_decoder_free(d);
  // a limit gives a prefix of whole code points with their entries
  const Mapped lim = decode_mapped(GTEXT_ENCODING_SHIFT_JIS, std::string("a") + "\x82\xA0" + "b", {}, false, 3);
  EXPECT_EQ(lim.status, GTEXT_ENCODING_E_LIMIT_OUTPUT);
  EXPECT_EQ(lim.bytes, "a");
  ASSERT_EQ(lim.offsets.size(), 1u);
  // a refusing sink
  struct Refuse {
    static bool no(void *, const char *, size_t, const GTEXT_Encoding_Source_Map *) { return false; }
  };
  ASSERT_EQ(gtext_decoder_new(GTEXT_ENCODING_UTF_8, &o, &d), GTEXT_ENCODING_OK);
  GTEXT_Encoding_Mapped_Sink refuse = {Refuse::no, nullptr};
  EXPECT_EQ(gtext_decoder_decode_mapped(d, "abc", 3, true, &refuse, nullptr), GTEXT_ENCODING_E_WRITE);
  gtext_decoder_free(d);
}

// gtext_encoder_set_error_mode: between calls of one stream, an encoder with a mode (ISO-2022-JP) keeps it.
namespace {

struct Bytes {
  std::string s;
};

bool bytes_write(void * user, const char * b, size_t n) {
  static_cast<Bytes *>(user)->s.append(b, n);
  return true;
}

} // namespace

// ---- a chunk is whole code points -------------------------------------------------------------------------------------------

// What a mapped sink may rely on (the CSS module's decode does): every chunk it is given starts at the first byte of a code point and
// ends at the last byte of one - never inside a sequence, whatever the encoding, the cuts, the errors, or the size of the chunk
// (GTEXT_ENCODING_MAX_CHUNK, which the long inputs below cross many times, with 2-, 3- and 4-byte characters and U+FFFD runs) - and
// the bytes of one code point share an offset and a width.
struct WholeCheck {
  size_t chunks = 0;
  size_t bytes = 0;
  size_t bad = 0;
  std::string why;
};

bool whole_sink(void * user, const char * bytes, size_t len, const GTEXT_Encoding_Source_Map * map) {
  auto * w = static_cast<WholeCheck *>(user);
  w->chunks++;
  w->bytes += len;
  size_t i = 0;
  while (i < len) {
    const unsigned char c = static_cast<unsigned char>(bytes[i]);
    const size_t n = c < 0x80 ? 1 : (c >= 0xC2 && c < 0xE0) ? 2 : (c >= 0xE0 && c < 0xF0) ? 3 : (c >= 0xF0 && c < 0xF5) ? 4 : 0;
    if (n == 0 || i + n > len) {
      w->bad++;
      w->why = "a chunk starts or ends inside a sequence at byte " + std::to_string(i) + " of " + std::to_string(len);
      return true;
    }
    for (size_t k = 1; k < n; k++) {
      if ((static_cast<unsigned char>(bytes[i + k]) & 0xC0) != 0x80 || map->offsets[i + k] != map->offsets[i]
          || map->widths[i + k] != map->widths[i] || map->replaced[i + k] != map->replaced[i]) {
        w->bad++;
        w->why = "the bytes of one code point do not share a map entry at byte " + std::to_string(i);
        return true;
      }
    }
    i += n;
  }
  return true;
}

TEST(EncodingMapped, EveryChunkIsWholeCodePointsInEveryEncoding) {
  // Inputs that put every kind of code point and every kind of error across the 4096-byte chunk boundary.
  std::vector<std::pair<std::string, std::string>> inputs;
  std::string three, four, bad8, kanji2022, mixed;
  for (int i = 0; i < 6000; i++) {
    three += "\xe3\x81\x82";
    four += "\xf0\x9f\x98\x80";
    bad8 += "\xff\xc3";
    mixed += (i % 3 == 0) ? "\xc3\xa9" : (i % 3 == 1) ? "a" : "\xe2\x82\xac";
  }
  kanji2022 = "\x1b$B";
  for (int i = 0; i < 6000; i++) {
    kanji2022 += "F|";
    if (i % 997 == 0) {
      kanji2022 += "\x1b(B" "a" "\x1b$B";
    }
  }
  inputs.push_back({"three-byte characters", three});
  inputs.push_back({"four-byte characters", four});
  inputs.push_back({"ill-formed", bad8});
  inputs.push_back({"mixed lengths", mixed});
  inputs.push_back({"iso-2022-jp", kanji2022});
  uint64_t x = 88172645463325252ULL;
  std::string noise;
  for (int i = 0; i < 40000; i++) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    noise += static_cast<char>((x >> 11) & 0xff);
    if (x % 29 == 0) {
      noise += '\x1b';
    }
  }
  inputs.push_back({"noise", noise});
  for (int e = 1; e < GTEXT_ENCODING_COUNT; e++) {
    const GTEXT_Encoding enc = static_cast<GTEXT_Encoding>(e);
    for (const auto & in : inputs) {
      for (const size_t piece : {size_t{0}, size_t{1000}, size_t{4093}}) { // 0: one call
        GTEXT_Decoder_Options o = gtext_decoder_options_default();
        o.track_offsets = true;
        o.max_output_bytes = SIZE_MAX;
        GTEXT_Decoder * d = nullptr;
        ASSERT_EQ(gtext_decoder_new(enc, &o, &d), GTEXT_ENCODING_OK);
        WholeCheck w;
        const GTEXT_Encoding_Mapped_Sink sink = {whole_sink, &w};
        const size_t step = piece ? piece : in.second.size();
        for (size_t at = 0; at < in.second.size(); at += step) {
          const size_t n = std::min(step, in.second.size() - at);
          GTEXT_Decoder_Result r;
          std::memset(&r, 0, sizeof r);
          ASSERT_EQ(gtext_decoder_decode_mapped(d, reinterpret_cast<const unsigned char *>(in.second.data()) + at, n,
                        at + n >= in.second.size(), &sink, &r),
              GTEXT_ENCODING_OK);
        }
        gtext_decoder_free(d);
        EXPECT_EQ(w.bad, 0u) << gtext_encoding_name(enc) << " / " << in.first << " / piece " << piece << ": " << w.why;
        EXPECT_GT(w.chunks, 0u);
      }
    }
  }
}

TEST(EncoderErrorMode, NullAndOutOfRangeAreIgnored) {
  gtext_encoder_set_error_mode(nullptr, GTEXT_ENCODER_HTML); // (nothing to check but that it returns)
  GTEXT_Encoder * e = nullptr;
  GTEXT_Encoder_Options o = gtext_encoder_options_default();
  o.error_mode = GTEXT_ENCODER_FATAL;
  ASSERT_EQ(gtext_encoder_new(GTEXT_ENCODING_WINDOWS_1252, &o, &e), GTEXT_ENCODING_OK);
  GTEXT_Encoder_Error_Mode bad;
  const int poked = 99;
  std::memcpy(&bad, &poked, sizeof(bad));
  gtext_encoder_set_error_mode(e, bad);
  Bytes out;
  GTEXT_Encoding_Sink sink = {bytes_write, &out};
  GTEXT_Encoder_Result r;
  EXPECT_EQ(gtext_encoder_encode(e, "a\xE6\x97\xA5", 4, true, &sink, &r), GTEXT_ENCODING_E_UNENCODABLE) << "still fatal";
  gtext_encoder_set_error_mode(e, GTEXT_ENCODER_HTML);
  gtext_encoder_set_error_mode(e, bad);
  out.s.clear();
  EXPECT_EQ(gtext_encoder_encode(e, "a\xE6\x97\xA5", 4, true, &sink, &r), GTEXT_ENCODING_OK) << "still the HTML mode";
  EXPECT_EQ(out.s, "a&#26085;");
  gtext_encoder_free(e);
}

TEST(EncoderErrorMode, ChangingItInTheMiddleOfAnIso2022JpStreamKeepsTheMode) {
  GTEXT_Encoder * e = nullptr;
  GTEXT_Encoder_Options o = gtext_encoder_options_default();
  o.error_mode = GTEXT_ENCODER_FATAL;
  ASSERT_EQ(gtext_encoder_new(GTEXT_ENCODING_ISO_2022_JP, &o, &e), GTEXT_ENCODING_OK);
  Bytes out;
  GTEXT_Encoding_Sink sink = {bytes_write, &out};
  GTEXT_Encoder_Result r;
  // kanji (the encoder enters the two-byte set) ... a euro sign in the HTML mode (back to ASCII, then the reference) ... kanji again in the
  // fatal mode (into the set again) ... the end (back to ASCII)
  ASSERT_EQ(gtext_encoder_encode(e, "\xE6\x97\xA5", 3, false, &sink, &r), GTEXT_ENCODING_OK);
  gtext_encoder_set_error_mode(e, GTEXT_ENCODER_HTML);
  ASSERT_EQ(gtext_encoder_encode(e, "\xE2\x82\xAC", 3, false, &sink, &r), GTEXT_ENCODING_OK);
  gtext_encoder_set_error_mode(e, GTEXT_ENCODER_FATAL);
  ASSERT_EQ(gtext_encoder_encode(e, "\xE6\x97\xA5", 3, false, &sink, &r), GTEXT_ENCODING_OK);
  EXPECT_EQ(gtext_encoder_encode(e, "\xE2\x82\xAC", 3, false, &sink, &r), GTEXT_ENCODING_E_UNENCODABLE) << "fatal again";
  // (a failure puts the encoder back at the start, and writes the escape back to ASCII first)
  EXPECT_EQ(out.s, "\x1B$BF|\x1B(B&#8364;\x1B$BF|\x1B(B");
  gtext_encoder_free(e);
}
