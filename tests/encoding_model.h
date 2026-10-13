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
 * What the Encoding tests and the fuzz harness share, with no test framework: sinks, whole and chunked decoding and
 * encoding, and two reference decoders written differently from the library's - one for UTF-8 by the maximal-subpart
 * definition of the Unicode standard's Table 3-7, one for UTF-16 over whole code units - which the exhaustive
 * tests compare it with.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GTEXT_TESTS_ENCODING_MODEL_H
#define GHOTI_IO_GTEXT_TESTS_ENCODING_MODEL_H

#include <ghoti.io/text/encoding.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>


namespace enctest {

// ---- sinks ---------------------------------------------------------------------------------------------------

struct Collect {
  std::string bytes;
  size_t calls = 0;
  size_t largest = 0;
  size_t refuse_after = SIZE_MAX; // refuse the call with this index (0-based)
};

inline bool collect_write(void * user, const char * bytes, size_t len) {
  auto * c = static_cast<Collect *>(user);
  if (c->calls >= c->refuse_after) {
    return false;
  }
  c->calls++;
  if (len > c->largest) {
    c->largest = len;
  }
  c->bytes.append(bytes, len);
  return true;
}

inline GTEXT_Encoding_Sink sink_of(Collect & c) {
  GTEXT_Encoding_Sink s;
  s.write = collect_write;
  s.user = &c;
  return s;
}

// ---- running a codec -----------------------------------------------------------------------------------------

struct Out {
  GTEXT_Encoding_Status status = GTEXT_ENCODING_OK;
  std::string bytes;
  size_t error_offset = 0;
  size_t replacements = 0;
  uint32_t error_code_point = 0;
  GTEXT_Encoding encoding = GTEXT_ENCODING_NONE;
  bool bom_found = false;
  bool operator==(const Out & o) const {
    return status == o.status && bytes == o.bytes && error_offset == o.error_offset && replacements == o.replacements
        && error_code_point == o.error_code_point && encoding == o.encoding && bom_found == o.bom_found;
  }
};

inline std::string show(const std::string & s) {
  std::string r;
  char b[8];
  for (unsigned char c : s) {
    std::snprintf(b, sizeof(b), "%02X ", c);
    r += b;
  }
  return r;
}

inline std::string show(const Out & o) {
  return std::string("status ") + gtext_encoding_status_string(o.status) + ", bytes [" + show(o.bytes) + "], offset "
      + std::to_string(o.error_offset) + ", replacements " + std::to_string(o.replacements);
}

// Decode `in` in pieces cut at `cuts` (offsets, ascending), the last with flush. Statuses other than OK stop at once.
inline Out decode_cut(GTEXT_Encoding enc, const GTEXT_Decoder_Options & opts, const std::string & in,
    const std::vector<size_t> & cuts) {
  Out out;
  GTEXT_Decoder * d = nullptr;
  out.status = gtext_decoder_new(enc, &opts, &d);
  if (out.status != GTEXT_ENCODING_OK) {
    return out;
  }
  Collect c;
  GTEXT_Encoding_Sink sink = sink_of(c);
  size_t start = 0;
  std::vector<size_t> points = cuts;
  points.push_back(in.size());
  for (size_t k = 0; k < points.size(); k++) {
    const bool last = k + 1 == points.size();
    GTEXT_Decoder_Result r;
    const GTEXT_Encoding_Status s = gtext_decoder_decode(d, in.data() + start, points[k] - start, last, &sink, &r);
    out.replacements += r.replacements;
    out.encoding = r.encoding;
    out.bom_found = out.bom_found || r.bom_found;
    if (s != GTEXT_ENCODING_OK) {
      out.status = s;
      out.error_offset = r.error_offset;
      break;
    }
    start = points[k];
  }
  out.bytes = c.bytes;
  gtext_decoder_free(d);
  return out;
}

inline Out decode_whole(GTEXT_Encoding enc, const GTEXT_Decoder_Options & opts, const std::string & in) {
  return decode_cut(enc, opts, in, {});
}

inline Out decode_whole(GTEXT_Encoding enc, const std::string & in) {
  GTEXT_Decoder_Options o = gtext_decoder_options_default();
  return decode_whole(enc, o, in);
}

inline Out decode_bytewise(GTEXT_Encoding enc, const GTEXT_Decoder_Options & opts, const std::string & in) {
  std::vector<size_t> cuts;
  for (size_t i = 1; i < in.size(); i++) {
    cuts.push_back(i);
  }
  return decode_cut(enc, opts, in, cuts);
}

inline Out encode_cut(GTEXT_Encoding enc, const GTEXT_Encoder_Options & opts, const std::string & in,
    const std::vector<size_t> & cuts) {
  Out out;
  GTEXT_Encoder * e = nullptr;
  out.status = gtext_encoder_new(enc, &opts, &e);
  if (out.status != GTEXT_ENCODING_OK) {
    return out;
  }
  Collect c;
  GTEXT_Encoding_Sink sink = sink_of(c);
  size_t start = 0;
  std::vector<size_t> points = cuts;
  points.push_back(in.size());
  for (size_t k = 0; k < points.size(); k++) {
    const bool last = k + 1 == points.size();
    GTEXT_Encoder_Result r;
    const GTEXT_Encoding_Status s = gtext_encoder_encode(e, in.data() + start, points[k] - start, last, &sink, &r);
    out.replacements += r.replacements;
    if (s != GTEXT_ENCODING_OK) {
      out.status = s;
      out.error_offset = r.error_offset;
      out.error_code_point = r.error_code_point;
      break;
    }
    start = points[k];
  }
  out.bytes = c.bytes;
  gtext_encoder_free(e);
  return out;
}

inline Out encode_whole(GTEXT_Encoding enc, const GTEXT_Encoder_Options & opts, const std::string & in) {
  return encode_cut(enc, opts, in, {});
}

inline GTEXT_Encoder_Options encoder_options(GTEXT_Encoder_Error_Mode mode) {
  GTEXT_Encoder_Options o = gtext_encoder_options_default();
  o.error_mode = mode;
  return o;
}

inline GTEXT_Decoder_Options decoder_options(GTEXT_Decoder_Error_Mode mode) {
  GTEXT_Decoder_Options o = gtext_decoder_options_default();
  o.error_mode = mode;
  return o;
}

// Put a bit pattern that is no value of the enum into a field of an options struct, which the library (C) reads and C++ never does:
// converting or loading such a value in C++ is undefined behaviour, a C caller passing one is the case the checks are for.
template <class T>
inline void poke(T & field, unsigned raw) {
  static_assert(sizeof(T) == sizeof(unsigned), "an enum of the size of an int");
  std::memcpy(static_cast<void *>(&field), &raw, sizeof raw);
}

inline std::string utf8_of(uint32_t cp) {
  std::string s;
  if (cp < 0x80) {
    s += char(cp);
  } else if (cp < 0x800) {
    s += char(0xC0 | (cp >> 6));
    s += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    s += char(0xE0 | (cp >> 12));
    s += char(0x80 | ((cp >> 6) & 0x3F));
    s += char(0x80 | (cp & 0x3F));
  } else {
    s += char(0xF0 | (cp >> 18));
    s += char(0x80 | ((cp >> 12) & 0x3F));
    s += char(0x80 | ((cp >> 6) & 0x3F));
    s += char(0x80 | (cp & 0x3F));
  }
  return s;
}

inline std::string bytes_of(std::initializer_list<int> v) {
  std::string s;
  for (int b : v) {
    s += char(b);
  }
  return s;
}

// The enumerator of a name in encodings.json ("windows-1252").
inline GTEXT_Encoding encoding_by_name(const std::string & name) {
  for (int e = 1; e < GTEXT_ENCODING_COUNT; e++) {
    if (name == gtext_encoding_name(GTEXT_Encoding(e))) {
      return GTEXT_Encoding(e);
    }
  }
  return GTEXT_ENCODING_NONE;
}

inline bool is_single_byte(GTEXT_Encoding e) {
  if (e == GTEXT_ENCODING_UTF_8 || e == GTEXT_ENCODING_REPLACEMENT || e == GTEXT_ENCODING_UTF_16BE
      || e == GTEXT_ENCODING_UTF_16LE || e == GTEXT_ENCODING_X_USER_DEFINED) {
    return false;
  }
  return gtext_encoding_is_supported(e);
}

// The name of the index a single-byte encoding uses (ISO-8859-8-I uses ISO-8859-8's).
inline std::string index_name(GTEXT_Encoding e) {
  std::string n = gtext_encoding_name(e);
  for (auto & c : n) {
    c = char(std::tolower((unsigned char)c));
  }
  if (n == "iso-8859-8-i") {
    n = "iso-8859-8";
  }
  return n;
}

inline std::vector<GTEXT_Encoding> single_byte_encodings() {
  std::vector<GTEXT_Encoding> v;
  for (int e = 1; e < GTEXT_ENCODING_COUNT; e++) {
    if (is_single_byte(GTEXT_Encoding(e))) {
      v.push_back(GTEXT_Encoding(e));
    }
  }
  return v;
}

// ---- reference decoders --------------------------------------------------------------------------------------

// UTF-8 with U+FFFD for each maximal subpart of an ill-formed subsequence (Unicode 16, section 3.9.6, Table 3-7).
// Written by byte-range tables, not by the standard's boundary state machine that the library follows.
struct RefResult {
  std::vector<uint32_t> cps;
  size_t replacements = 0;
  std::vector<size_t> starts; ///< UTF-8 only: the offset of the first byte of each code point (or of its subpart).
  std::vector<bool> bad;      ///< UTF-8 only: the code point is a U+FFFD that stands for ill-formed input.
};

inline RefResult ref_utf8(const std::string & s) {
  struct Form {
    int n;
    int lo[4];
    int hi[4];
  };
  static const Form forms[] = {
      {1, {0x00}, {0x7F}},
      {2, {0xC2, 0x80}, {0xDF, 0xBF}},
      {3, {0xE0, 0xA0, 0x80}, {0xE0, 0xBF, 0xBF}},
      {3, {0xE1, 0x80, 0x80}, {0xEC, 0xBF, 0xBF}},
      {3, {0xED, 0x80, 0x80}, {0xED, 0x9F, 0xBF}},
      {3, {0xEE, 0x80, 0x80}, {0xEF, 0xBF, 0xBF}},
      {4, {0xF0, 0x90, 0x80, 0x80}, {0xF0, 0xBF, 0xBF, 0xBF}},
      {4, {0xF1, 0x80, 0x80, 0x80}, {0xF3, 0xBF, 0xBF, 0xBF}},
      {4, {0xF4, 0x80, 0x80, 0x80}, {0xF4, 0x8F, 0xBF, 0xBF}},
  };
  RefResult r;
  size_t i = 0;
  const auto at = [&](size_t k) { return int((unsigned char)s[k]); };
  while (i < s.size()) {
    size_t best = 0; // the longest prefix of any well-formed form
    const Form * whole = nullptr;
    for (const Form & f : forms) {
      size_t k = 0;
      while (k < size_t(f.n) && i + k < s.size() && at(i + k) >= f.lo[k] && at(i + k) <= f.hi[k]) {
        k++;
      }
      if (k == size_t(f.n)) {
        whole = &f;
        best = k;
        break;
      }
      if (k > best) {
        best = k;
      }
    }
    if (whole) {
      uint32_t cp = 0;
      if (whole->n == 1) {
        cp = uint32_t(at(i));
      } else if (whole->n == 2) {
        cp = (uint32_t(at(i) & 0x1F) << 6) | uint32_t(at(i + 1) & 0x3F);
      } else if (whole->n == 3) {
        cp = (uint32_t(at(i) & 0x0F) << 12) | (uint32_t(at(i + 1) & 0x3F) << 6) | uint32_t(at(i + 2) & 0x3F);
      } else {
        cp = (uint32_t(at(i) & 0x07) << 18) | (uint32_t(at(i + 1) & 0x3F) << 12) | (uint32_t(at(i + 2) & 0x3F) << 6)
            | uint32_t(at(i + 3) & 0x3F);
      }
      r.cps.push_back(cp);
      r.starts.push_back(i);
      r.bad.push_back(false);
      i += whole->n;
    } else {
      r.cps.push_back(0xFFFD);
      r.starts.push_back(i);
      r.bad.push_back(true);
      r.replacements++;
      i += best > 0 ? best : 1;
    }
  }
  return r;
}

// UTF-16 over whole code units, by the standard's description of what its decoder does with them: a lone or
// unpaired surrogate is U+FFFD and the unit after an unpaired leading surrogate is read again, and an odd byte at the
// end, or an odd byte and a leading surrogate before it, is one U+FFFD in all.
inline RefResult ref_utf16(const std::string & s, bool big_endian) {
  RefResult r;
  std::vector<uint32_t> units;
  for (size_t i = 0; i + 1 < s.size(); i += 2) {
    const uint32_t a = (unsigned char)s[i];
    const uint32_t b = (unsigned char)s[i + 1];
    units.push_back(big_endian ? (a << 8 | b) : (b << 8 | a));
  }
  const bool odd = s.size() % 2 == 1;
  size_t i = 0;
  while (i < units.size()) {
    const uint32_t u = units[i];
    if (u >= 0xD800 && u <= 0xDBFF) {
      if (i + 1 < units.size()) {
        const uint32_t v = units[i + 1];
        if (v >= 0xDC00 && v <= 0xDFFF) {
          r.cps.push_back(0x10000 + ((u - 0xD800) << 10) + (v - 0xDC00));
          i += 2;
          continue;
        }
        r.cps.push_back(0xFFFD);
        r.replacements++;
        i += 1;
        continue;
      }
      // a leading surrogate at the end of the units: one error covers it and a byte after it
      r.cps.push_back(0xFFFD);
      r.replacements++;
      i += 1;
      continue;
    }
    if (u >= 0xDC00 && u <= 0xDFFF) {
      r.cps.push_back(0xFFFD);
      r.replacements++;
    } else {
      r.cps.push_back(u);
    }
    i++;
  }
  if (odd) {
    // A leading surrogate that is the last unit already got its U+FFFD above, and the standard's single error at the
    // end of the stream covers it and the byte after it; otherwise the odd byte is an error of its own.
    const bool pending_lead = !units.empty() && units.back() >= 0xD800 && units.back() <= 0xDBFF;
    if (!pending_lead) {
      r.cps.push_back(0xFFFD);
      r.replacements++;
    }
  }
  return r;
}

inline std::string utf8_of_cps(const std::vector<uint32_t> & cps) {
  std::string s;
  for (uint32_t cp : cps) {
    s += utf8_of(cp);
  }
  return s;
}

// A small deterministic generator (the tests must be repeatable).
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
  uint64_t next() {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return s * 0x2545F4914F6CDD1Dull;
  }
  size_t below(size_t n) {
    return size_t(next() % n);
  }
};

} // namespace enctest

#endif // GHOTI_IO_GTEXT_TESTS_ENCODING_MODEL_H
