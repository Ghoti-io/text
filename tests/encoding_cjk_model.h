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
 * A second transcription of the multi-byte decoders and encoders of the Encoding Standard (sections 10 to 13), for the tests: written as
 * the standard writes its handlers (one byte or end-of-queue in; "continue", "finished", an error or code points out; "restore" puts bytes
 * back in front of the queue), over the pinned indexes.json read by this library's JSON parser, with the index operations in the
 * standard's own words ("the first pointer", "excluding all entries whose pointer is ...") rather than the library's generated
 * two-level tables. It shares no code with src/encoding/encoding_cjk.c, and tools/oracle/encoding_model.py is a third transcription in
 * another language; **all three were written from the same text by one author, handler for handler**, so their agreement shows that the transcriptions agree and
 * not that three readings were made. What is independent of them is Node's ICU, CPython's codecs and the standard's own tests; and
 * tools/encoding/check-transcription.py compares the numbers of the standard's text with those of the library's handlers.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GTEXT_TESTS_ENCODING_CJK_MODEL_H
#define GHOTI_IO_GTEXT_TESTS_ENCODING_CJK_MODEL_H

#include "encoding_model.h"

#include <ghoti.io/text/json.h>

#include <map>
#include <unordered_map>

namespace enctest {

// ---- the indexes ---------------------------------------------------------------------------------------------

struct CjkIndexes {
  std::vector<int32_t> big5, euc_kr, gb18030, jis0208, jis0212, katakana; // -1 for null
  std::vector<std::pair<uint32_t, uint32_t>> ranges;                       // (pointer, code point)
  // "the index pointer for a code point": first pointers, and the filtered ones.
  std::unordered_map<uint32_t, uint32_t> gb18030_first, euc_kr_first, jis0208_first, shift_jis_first, big5_pointer;
};

// The pinned indexes.json, read with this library's JSON parser (nothing generated reads it). A harness that cannot find it stops: it would otherwise check nothing.
inline GTEXT_JSON_Value * cjk_load_json(const char * path) {
  GTEXT_JSON_Error err;
  std::memset(&err, 0, sizeof(err));
  GTEXT_JSON_Value * v = gtext_json_parse_file(path, nullptr, &err);
  if (!v) {
    std::fprintf(stderr, "%s is not here or does not parse (run tools/encoding/fetch.sh from the library's directory)\n", path);
    std::abort();
  }
  return v;
}

inline std::vector<int32_t> load_index(GTEXT_JSON_Value * root, const char * name) {
  std::vector<int32_t> out;
  const GTEXT_JSON_Value * arr = gtext_json_object_get(root, name, std::strlen(name));
  for (size_t i = 0; i < gtext_json_array_size(arr); i++) {
    const GTEXT_JSON_Value * v = gtext_json_array_get(arr, i);
    int64_t x = -1;
    if (gtext_json_typeof(v) != GTEXT_JSON_NULL) {
      gtext_json_get_i64(v, &x);
    }
    out.push_back(int32_t(x));
  }
  return out;
}

inline void first_pointers(const std::vector<int32_t> & index, std::unordered_map<uint32_t, uint32_t> & out,
    uint32_t skip_from = 0, uint32_t skip_lo = 1, uint32_t skip_hi = 0, bool last_for_six = false) {
  for (uint32_t p = skip_from; p < index.size(); p++) {
    if (index[p] < 0 || (p >= skip_lo && p <= skip_hi)) {
      continue;
    }
    const uint32_t cp = uint32_t(index[p]);
    const bool six = cp == 0x2550 || cp == 0x255E || cp == 0x2561 || cp == 0x256A || cp == 0x5341 || cp == 0x5345;
    if (last_for_six && six) {
      out[cp] = p; // "return the last pointer corresponding to codePoint in index"
    } else if (!out.count(cp)) {
      out[cp] = p; // the first
    }
  }
}

inline const CjkIndexes & cjk_indexes() {
  static const CjkIndexes idx = [] {
    CjkIndexes r;
    GTEXT_JSON_Value * root = cjk_load_json("third_party/encoding/indexes.json");
    r.big5 = load_index(root, "big5");
    r.euc_kr = load_index(root, "euc-kr");
    r.gb18030 = load_index(root, "gb18030");
    r.jis0208 = load_index(root, "jis0208");
    r.jis0212 = load_index(root, "jis0212");
    r.katakana = load_index(root, "iso-2022-jp-katakana");
    const GTEXT_JSON_Value * rs = gtext_json_object_get(root, "gb18030-ranges", 14);
    for (size_t i = 0; i < gtext_json_array_size(rs); i++) {
      const GTEXT_JSON_Value * row = gtext_json_array_get(rs, i);
      int64_t a = 0, b = 0;
      gtext_json_get_i64(gtext_json_array_get(row, 0), &a);
      gtext_json_get_i64(gtext_json_array_get(row, 1), &b);
      r.ranges.emplace_back(uint32_t(a), uint32_t(b));
    }
    gtext_json_free(root);
    first_pointers(r.gb18030, r.gb18030_first);
    first_pointers(r.euc_kr, r.euc_kr_first);
    first_pointers(r.jis0208, r.jis0208_first);
    first_pointers(r.jis0208, r.shift_jis_first, 0, 8272, 8835);                // "excluding all entries whose pointer is in the range 8272 to 8835"
    first_pointers(r.big5, r.big5_pointer, (0xA1 - 0x81) * 157, 1, 0, true);   // "excluding all entries whose pointer is less than (0xA1 - 0x81) x 157"
    return r;
  }();
  return idx;
}

inline int32_t index_cp(const std::vector<int32_t> & index, int64_t pointer) {
  if (pointer < 0 || pointer >= int64_t(index.size())) {
    return -1;
  }
  return index[size_t(pointer)];
}

// ---- gb18030 ranges --------------------------------------------------------------------------------------------

inline int64_t ranges_code_point(uint32_t pointer) {
  const auto & rs = cjk_indexes().ranges;
  if ((pointer > 39419 && pointer < 189000) || pointer > 1237575) {
    return -1;
  }
  if (pointer == 7457) {
    return 0xE7C7;
  }
  size_t row = 0;
  for (size_t i = 0; i < rs.size(); i++) {
    if (rs[i].first <= pointer) {
      row = i;
    }
  }
  return int64_t(rs[row].second) + int64_t(pointer) - int64_t(rs[row].first);
}

inline uint32_t ranges_pointer(uint32_t cp) {
  const auto & rs = cjk_indexes().ranges;
  if (cp == 0xE7C7) {
    return 7457;
  }
  size_t row = 0;
  for (size_t i = 0; i < rs.size(); i++) {
    if (rs[i].second <= cp) {
      row = i;
    }
  }
  return rs[row].first + cp - rs[row].second;
}

// ---- decoders --------------------------------------------------------------------------------------------------

enum class Step { Finished, Continue, Error, Items };

// A decoder instance as the standard has it: all the encodings' state in one struct, only the fields of the encoding in use being touched.
struct ModelDecoder {
  GTEXT_Encoding enc;
  int first = 0, second = 0, third = 0; // gb18030
  int leading = 0;                      // Big5, EUC-JP, EUC-KR, Shift_JIS, ISO-2022-JP leading
  bool jis0212 = false;
  int state = 0, output_state = 0;      // ISO-2022-JP: 0 ASCII, 1 Roman, 2 katakana, 3 lead, 4 trail, 5 escape start, 6 escape
  bool output = false;
  // results of a step
  uint32_t cps[2];
  int n = 0;
  int restore[4] = {0, 0, 0, 0};
  int restore_n = 0;
  void set_restore(std::initializer_list<int> v) {
    restore_n = 0;
    for (int b : v) {
      restore[restore_n++] = b;
    }
  }

  explicit ModelDecoder(GTEXT_Encoding e) : enc(e) {}

  Step items1(uint32_t a) {
    cps[0] = a;
    n = 1;
    return Step::Items;
  }
  Step items2(uint32_t a, uint32_t b) {
    cps[0] = a;
    cps[1] = b;
    n = 2;
    return Step::Items;
  }

  Step handle(int byte) { // byte < 0: end-of-queue
    restore_n = 0;
    n = 0;
    const CjkIndexes & I = cjk_indexes();
    switch (enc) {
    case GTEXT_ENCODING_GBK:
    case GTEXT_ENCODING_GB18030: {
      if (byte < 0 && first == 0 && second == 0 && third == 0) {
        return Step::Finished;
      }
      if (byte < 0) {
        first = second = third = 0;
        return Step::Error;
      }
      if (third != 0) {
        if (byte < 0x30 || byte > 0x39) {
          set_restore({second, third, byte});
          first = second = third = 0;
          return Step::Error;
        }
        const int64_t cp = ranges_code_point(uint32_t((first - 0x81) * (10 * 126 * 10) + (second - 0x30) * (10 * 126) + (third - 0x81) * 10 + byte - 0x30));
        first = second = third = 0;
        if (cp < 0) {
          return Step::Error;
        }
        return items1(uint32_t(cp));
      }
      if (second != 0) {
        if (byte >= 0x81 && byte <= 0xFE) {
          third = byte;
          return Step::Continue;
        }
        set_restore({second, byte});
        first = second = 0;
        return Step::Error;
      }
      if (first != 0) {
        if (byte >= 0x30 && byte <= 0x39) {
          second = byte;
          return Step::Continue;
        }
        const int lead = first;
        first = 0;
        int64_t pointer = -1;
        const int offset = byte < 0x7F ? 0x40 : 0x41;
        if ((byte >= 0x40 && byte <= 0x7E) || (byte >= 0x80 && byte <= 0xFE)) {
          pointer = int64_t(lead - 0x81) * 190 + (byte - offset);
        }
        const int32_t cp = index_cp(I.gb18030, pointer);
        if (cp >= 0) {
          return items1(uint32_t(cp));
        }
        if (byte <= 0x7F) {
          set_restore({byte});
        }
        return Step::Error;
      }
      if (byte <= 0x7F) {
        return items1(uint32_t(byte));
      }
      if (byte == 0x80) {
        return items1(0x20AC);
      }
      if (byte >= 0x81 && byte <= 0xFE) {
        first = byte;
        return Step::Continue;
      }
      return Step::Error;
    }
    case GTEXT_ENCODING_BIG5: {
      if (byte < 0 && leading != 0) {
        leading = 0;
        return Step::Error;
      }
      if (byte < 0) {
        return Step::Finished;
      }
      if (leading != 0) {
        const int lead = leading;
        leading = 0;
        int64_t pointer = -1;
        const int offset = byte < 0x7F ? 0x40 : 0x62;
        if ((byte >= 0x40 && byte <= 0x7E) || (byte >= 0xA1 && byte <= 0xFE)) {
          pointer = int64_t(lead - 0x81) * 157 + (byte - offset);
        }
        if (pointer == 1133) return items2(0x00CA, 0x0304);
        if (pointer == 1135) return items2(0x00CA, 0x030C);
        if (pointer == 1164) return items2(0x00EA, 0x0304);
        if (pointer == 1166) return items2(0x00EA, 0x030C);
        const int32_t cp = index_cp(I.big5, pointer);
        if (cp >= 0) {
          return items1(uint32_t(cp));
        }
        if (byte <= 0x7F) {
          set_restore({byte});
        }
        return Step::Error;
      }
      if (byte <= 0x7F) {
        return items1(uint32_t(byte));
      }
      if (byte >= 0x81 && byte <= 0xFE) {
        leading = byte;
        return Step::Continue;
      }
      return Step::Error;
    }
    case GTEXT_ENCODING_EUC_JP: {
      if (byte < 0 && leading != 0) {
        leading = 0;
        return Step::Error;
      }
      if (byte < 0) {
        return Step::Finished;
      }
      if (leading == 0x8E && byte >= 0xA1 && byte <= 0xDF) {
        leading = 0;
        return items1(uint32_t(0xFF61 - 0xA1 + byte));
      }
      if (leading == 0x8F && byte >= 0xA1 && byte <= 0xFE) {
        jis0212 = true;
        leading = byte;
        return Step::Continue;
      }
      if (leading != 0) {
        const int lead = leading;
        leading = 0;
        int32_t cp = -1;
        if (lead >= 0xA1 && lead <= 0xFE && byte >= 0xA1 && byte <= 0xFE) {
          cp = index_cp(jis0212 ? I.jis0212 : I.jis0208, int64_t(lead - 0xA1) * 94 + byte - 0xA1);
        }
        jis0212 = false;
        if (cp >= 0) {
          return items1(uint32_t(cp));
        }
        if (byte <= 0x7F) {
          set_restore({byte});
        }
        return Step::Error;
      }
      if (byte <= 0x7F) {
        return items1(uint32_t(byte));
      }
      if (byte == 0x8E || byte == 0x8F || (byte >= 0xA1 && byte <= 0xFE)) {
        leading = byte;
        return Step::Continue;
      }
      return Step::Error;
    }
    case GTEXT_ENCODING_ISO_2022_JP: {
      switch (state) {
      case 0: // ASCII
        if (byte == 0x1B) { state = 5; return Step::Continue; }
        if (byte >= 0 && byte <= 0x7F && byte != 0x0E && byte != 0x0F) { output = false; return items1(uint32_t(byte)); }
        if (byte < 0) return Step::Finished;
        output = false;
        return Step::Error;
      case 1: // Roman
        if (byte == 0x1B) { state = 5; return Step::Continue; }
        if (byte == 0x5C) { output = false; return items1(0xA5); }
        if (byte == 0x7E) { output = false; return items1(0x203E); }
        if (byte >= 0 && byte <= 0x7F && byte != 0x0E && byte != 0x0F) { output = false; return items1(uint32_t(byte)); }
        if (byte < 0) return Step::Finished;
        output = false;
        return Step::Error;
      case 2: // katakana
        if (byte == 0x1B) { state = 5; return Step::Continue; }
        if (byte >= 0x21 && byte <= 0x5F) { output = false; return items1(uint32_t(0xFF61 - 0x21 + byte)); }
        if (byte < 0) return Step::Finished;
        output = false;
        return Step::Error;
      case 3: // lead byte
        if (byte == 0x1B) { state = 5; return Step::Continue; }
        if (byte >= 0x21 && byte <= 0x7E) { output = false; leading = byte; state = 4; return Step::Continue; }
        if (byte < 0) return Step::Finished;
        output = false;
        return Step::Error;
      case 4: { // trail byte
        if (byte == 0x1B) { state = 5; return Step::Error; }
        if (byte >= 0x21 && byte <= 0x7E) {
          state = 3;
          const int32_t cp = index_cp(I.jis0208, int64_t(leading - 0x21) * 94 + byte - 0x21);
          if (cp < 0) return Step::Error;
          return items1(uint32_t(cp));
        }
        state = 3;
        return Step::Error;
      }
      case 5: // escape start
        if (byte == 0x24 || byte == 0x28) { leading = byte; state = 6; return Step::Continue; }
        if (byte >= 0) set_restore({byte});
        output = false;
        state = output_state;
        return Step::Error;
      default: { // escape
        const int lead = leading;
        leading = 0;
        int next = -1;
        if (lead == 0x28 && byte == 0x42) next = 0;
        if (lead == 0x28 && byte == 0x4A) next = 1;
        if (lead == 0x28 && byte == 0x49) next = 2;
        if (lead == 0x24 && (byte == 0x40 || byte == 0x42)) next = 3;
        if (next >= 0) {
          state = output_state = next;
          const bool was = output;
          output = true;
          return was ? Step::Error : Step::Continue;
        }
        if (byte < 0) { set_restore({lead}); } else { set_restore({lead, byte}); }
        output = false;
        state = output_state;
        return Step::Error;
      }
      }
    }
    case GTEXT_ENCODING_SHIFT_JIS: {
      if (byte < 0 && leading != 0) {
        leading = 0;
        return Step::Error;
      }
      if (byte < 0) {
        return Step::Finished;
      }
      if (leading != 0) {
        const int lead = leading;
        leading = 0;
        int64_t pointer = -1;
        const int offset = byte < 0x7F ? 0x40 : 0x41;
        const int lead_offset = lead < 0xA0 ? 0x81 : 0xC1;
        if ((byte >= 0x40 && byte <= 0x7E) || (byte >= 0x80 && byte <= 0xFC)) {
          pointer = int64_t(lead - lead_offset) * 188 + byte - offset;
        }
        if (pointer >= 8836 && pointer <= 10715) {
          return items1(uint32_t(0xE000 - 8836 + pointer));
        }
        const int32_t cp = index_cp(I.jis0208, pointer);
        if (cp >= 0) {
          return items1(uint32_t(cp));
        }
        if (byte <= 0x7F) {
          set_restore({byte});
        }
        return Step::Error;
      }
      if (byte <= 0x80) {
        return items1(uint32_t(byte));
      }
      if (byte >= 0xA1 && byte <= 0xDF) {
        return items1(uint32_t(0xFF61 - 0xA1 + byte));
      }
      if ((byte >= 0x81 && byte <= 0x9F) || (byte >= 0xE0 && byte <= 0xFC)) {
        leading = byte;
        return Step::Continue;
      }
      return Step::Error;
    }
    case GTEXT_ENCODING_EUC_KR: {
      if (byte < 0 && leading != 0) {
        leading = 0;
        return Step::Error;
      }
      if (byte < 0) {
        return Step::Finished;
      }
      if (leading != 0) {
        const int lead = leading;
        leading = 0;
        int64_t pointer = -1;
        if (byte >= 0x41 && byte <= 0xFE) {
          pointer = int64_t(lead - 0x81) * 190 + (byte - 0x41);
        }
        const int32_t cp = index_cp(I.euc_kr, pointer);
        if (cp >= 0) {
          return items1(uint32_t(cp));
        }
        if (byte <= 0x7F) {
          set_restore({byte});
        }
        return Step::Error;
      }
      if (byte <= 0x7F) {
        return items1(uint32_t(byte));
      }
      if (byte >= 0x81 && byte <= 0xFE) {
        leading = byte;
        return Step::Continue;
      }
      return Step::Error;
    }
    default:
      return Step::Error;
    }
  }
};

struct ModelDecoded {
  std::vector<uint32_t> cps;
  bool failed = false;       // fatal mode: an error was returned
  size_t error_offset = 0;   // ... at this byte of the input (the length for end-of-queue)
  size_t replacements = 0;
  std::vector<uint32_t> steps; // after each step that wrote, how many code points were written so far: the boundaries of what is written together
  int iso_mode = 0;          // ISO-2022-JP, at the end (after a fatal error: the mode the next stream begins in): the decoder output state, 0 ASCII, 1 Roman, 2 katakana, 3 JIS X 0208
};

// "Process a queue" with the handler, the input and the end-of-queue, in "replacement" or "fatal" mode.
// `iso_start`: ISO-2022-JP's mode at the start (the decoder state and the decoder output state; the rest of its state is initial).
inline void model_decode_into(ModelDecoded & r, GTEXT_Encoding enc, const unsigned char * in, size_t len, bool fatal, int iso_start = 0) {
  r.cps.clear();
  r.steps.clear();
  r.failed = false;
  r.error_offset = 0;
  r.replacements = 0;
  ModelDecoder d(enc);
  d.state = d.output_state = iso_start;
  struct Mode {
    ModelDecoded & r;
    const ModelDecoder & d;
    ~Mode() { r.iso_mode = d.output_state; }
  } mode{r, d};
  thread_local std::vector<std::pair<int, size_t>> pending; // restored bytes, the next to read last
  pending.clear();
  size_t pos = 0;
  for (;;) {
    int byte;
    size_t where;
    if (!pending.empty()) {
      byte = pending.back().first;
      where = pending.back().second;
      pending.pop_back();
    } else if (pos < len) {
      byte = in[pos];
      where = pos++;
    } else {
      byte = -1;
      where = pos;
    }
    const Step s = d.handle(byte);
    if (s == Step::Finished) {
      return;
    }
    if (s == Step::Continue) {
      continue;
    }
    if (s == Step::Items) {
      for (int k = 0; k < d.n; k++) {
        r.cps.push_back(d.cps[k]);
      }
      r.steps.push_back(uint32_t(r.cps.size()));
      continue;
    }
    if (fatal) {
      r.failed = true;
      r.error_offset = where;
      return;
    }
    r.cps.push_back(0xFFFD);
    r.steps.push_back(uint32_t(r.cps.size()));
    r.replacements++;
    for (int k = d.restore_n; k > 0; k--) {
      pending.emplace_back(d.restore[k - 1], where);
    }
  }
}

inline ModelDecoded model_decode(GTEXT_Encoding enc, const unsigned char * in, size_t len, bool fatal, int iso_start = 0) {
  ModelDecoded r;
  model_decode_into(r, enc, in, len, fatal, iso_start);
  return r;
}

// ---- encoders --------------------------------------------------------------------------------------------------------

struct ModelEncodeResult {
  std::string bytes;
  bool failed = false;
  size_t error_index = 0;        // fatal mode: the index of the code point
  uint32_t error_cp = 0;
  std::vector<size_t> atoms;     // the length of the output after each character, reference or escape sequence: where an output limit may cut
  std::vector<size_t> errors;    // the indexes of the code points the encoder refused (written as a reference, or as ? in replacement mode)
};

inline bool model_gb18030_cp(uint32_t cp, bool gbk, std::string & out) {
  static const std::map<uint32_t, std::pair<int, int>> side = {{0xE78D, {0xA6, 0xD9}}, {0xE78E, {0xA6, 0xDA}}, {0xE78F, {0xA6, 0xDB}},
      {0xE790, {0xA6, 0xDC}}, {0xE791, {0xA6, 0xDD}}, {0xE792, {0xA6, 0xDE}}, {0xE793, {0xA6, 0xDF}}, {0xE794, {0xA6, 0xEC}},
      {0xE795, {0xA6, 0xED}}, {0xE796, {0xA6, 0xF3}}, {0xE81E, {0xFE, 0x59}}, {0xE826, {0xFE, 0x61}}, {0xE82B, {0xFE, 0x66}},
      {0xE82C, {0xFE, 0x67}}, {0xE832, {0xFE, 0x6D}}, {0xE843, {0xFE, 0x7E}}, {0xE854, {0xFE, 0x90}}, {0xE864, {0xFE, 0xA0}}};
  const CjkIndexes & I = cjk_indexes();
  if (cp <= 0x7F) {
    out += char(cp);
    return true;
  }
  if (cp == 0xE5E5) {
    return false;
  }
  if (gbk && cp == 0x20AC) {
    out += char(0x80);
    return true;
  }
  auto s = side.find(cp);
  if (s != side.end()) {
    out += char(s->second.first);
    out += char(s->second.second);
    return true;
  }
  auto f = I.gb18030_first.find(cp);
  if (f != I.gb18030_first.end()) {
    const uint32_t pointer = f->second;
    const uint32_t trailing = pointer % 190;
    out += char(pointer / 190 + 0x81);
    out += char(trailing + (trailing < 0x3F ? 0x40 : 0x41));
    return true;
  }
  if (gbk) {
    return false;
  }
  uint32_t p = ranges_pointer(cp);
  const uint32_t b1 = p / (10 * 126 * 10);
  p %= 10 * 126 * 10;
  const uint32_t b2 = p / (10 * 126);
  p %= 10 * 126;
  out += char(b1 + 0x81);
  out += char(b2 + 0x30);
  out += char(p / 10 + 0x81);
  out += char(p % 10 + 0x30);
  return true;
}

// The encoders that keep no state; false for "error with codePoint". (ISO-2022-JP has its own below.)
inline bool model_stateless_cp(GTEXT_Encoding enc, uint32_t cp, std::string & out) {
  const CjkIndexes & I = cjk_indexes();
  switch (enc) {
  case GTEXT_ENCODING_GBK:
    return model_gb18030_cp(cp, true, out);
  case GTEXT_ENCODING_GB18030:
    return model_gb18030_cp(cp, false, out);
  case GTEXT_ENCODING_BIG5: {
    if (cp <= 0x7F) {
      out += char(cp);
      return true;
    }
    auto f = I.big5_pointer.find(cp);
    if (f == I.big5_pointer.end()) {
      return false;
    }
    const uint32_t t = f->second % 157;
    out += char(f->second / 157 + 0x81);
    out += char(t + (t < 0x3F ? 0x40 : 0x62));
    return true;
  }
  case GTEXT_ENCODING_EUC_JP: {
    if (cp <= 0x7F) {
      out += char(cp);
      return true;
    }
    if (cp == 0xA5) { out += char(0x5C); return true; }
    if (cp == 0x203E) { out += char(0x7E); return true; }
    if (cp >= 0xFF61 && cp <= 0xFF9F) { out += char(0x8E); out += char(cp - 0xFF61 + 0xA1); return true; }
    if (cp == 0x2212) cp = 0xFF0D;
    auto f = I.jis0208_first.find(cp);
    if (f == I.jis0208_first.end()) return false;
    out += char(f->second / 94 + 0xA1);
    out += char(f->second % 94 + 0xA1);
    return true;
  }
  case GTEXT_ENCODING_SHIFT_JIS: {
    if (cp <= 0x80) {
      out += char(cp);
      return true;
    }
    if (cp == 0xA5) { out += char(0x5C); return true; }
    if (cp == 0x203E) { out += char(0x7E); return true; }
    if (cp >= 0xFF61 && cp <= 0xFF9F) { out += char(cp - 0xFF61 + 0xA1); return true; }
    if (cp == 0x2212) cp = 0xFF0D;
    auto f = I.shift_jis_first.find(cp);
    if (f == I.shift_jis_first.end()) return false;
    const uint32_t lead = f->second / 188;
    const uint32_t t = f->second % 188;
    out += char(lead + (lead < 0x1F ? 0x81 : 0xC1));
    out += char(t + (t < 0x3F ? 0x40 : 0x41));
    return true;
  }
  case GTEXT_ENCODING_EUC_KR: {
    if (cp <= 0x7F) {
      out += char(cp);
      return true;
    }
    auto f = I.euc_kr_first.find(cp);
    if (f == I.euc_kr_first.end()) return false;
    out += char(f->second / 190 + 0x81);
    out += char(f->second % 190 + 0x41);
    return true;
  }
  default:
    return false;
  }
}

enum class ModelMode { Html, Fatal, Replacement };

// "Process a queue" for an encoder on a list of code points, then end-of-queue (if `flush`: without it, ISO-2022-JP's closing escape is not written, as when
// the stream ends in an error); mode Html writes &#N; for an error, Replacement writes ? (this library's addition), Fatal stops.
inline ModelEncodeResult model_encode_ex(GTEXT_Encoding enc, const std::vector<uint32_t> & cps, ModelMode mode, bool flush = true) {
  ModelEncodeResult r;
  const CjkIndexes & I = cjk_indexes();
  auto atom = [&] { r.atoms.push_back(r.bytes.size()); };
  // `cp`: the code point handled (what the fatal status carries); `ref`: the code point "error with codePoint" names, which the html mode writes as a
  // reference (ISO-2022-JP: U+FFFD for U+000E, U+000F and U+001B, "rather than codePoint to prevent attacks").
  auto fail = [&](size_t index, uint32_t cp, uint32_t ref = 0xFFFFFFFFu) {
    if (ref == 0xFFFFFFFFu) {
      ref = cp;
    }
    if (mode == ModelMode::Fatal) {
      r.failed = true;
      r.error_index = index;
      r.error_cp = cp;
      return false;
    }
    r.errors.push_back(index);
    r.bytes += mode == ModelMode::Html ? "&#" + std::to_string(ref) + ";" : std::string("?");
    atom();
    return true;
  };
  if (enc != GTEXT_ENCODING_ISO_2022_JP) {
    for (size_t i = 0; i < cps.size(); i++) {
      if (model_stateless_cp(enc, cps[i], r.bytes)) {
        atom();
      } else if (!fail(i, cps[i])) {
        return r;
      }
    }
    return r;
  }
  enum { ASCII, ROMAN, JIS0208 } state = ASCII;
  std::vector<std::pair<size_t, uint32_t>> queue; // the next to read last
  for (size_t i = cps.size(); i > 0; i--) {
    queue.emplace_back(i - 1, cps[i - 1]);
  }
  while (!queue.empty()) {
    auto [index, cp] = queue.back();
    queue.pop_back();
    if ((state == ASCII || state == ROMAN) && (cp == 0x0E || cp == 0x0F || cp == 0x1B)) {
      if (!fail(index, cp, 0xFFFD)) return r;
      continue;
    }
    if (state == ASCII && cp <= 0x7F) {
      r.bytes += char(cp);
      atom();
      continue;
    }
    if (state == ROMAN && ((cp <= 0x7F && cp != 0x5C && cp != 0x7E) || cp == 0xA5 || cp == 0x203E)) {
      r.bytes += cp <= 0x7F ? char(cp) : (cp == 0xA5 ? char(0x5C) : char(0x7E));
      atom();
      continue;
    }
    if (cp <= 0x7F && state != ASCII) {
      queue.emplace_back(index, cp);
      state = ASCII;
      r.bytes += "\x1b(B";
      atom();
      continue;
    }
    if ((cp == 0xA5 || cp == 0x203E) && state != ROMAN) {
      queue.emplace_back(index, cp);
      state = ROMAN;
      r.bytes += "\x1b(J";
      atom();
      continue;
    }
    if (cp == 0x2212) cp = 0xFF0D;
    if (cp >= 0xFF61 && cp <= 0xFF9F) cp = uint32_t(I.katakana[cp - 0xFF61]);
    auto f = I.jis0208_first.find(cp);
    if (f == I.jis0208_first.end()) {
      if (state == JIS0208) {
        queue.emplace_back(index, cp);
        state = ASCII;
        r.bytes += "\x1b(B";
        atom();
        continue;
      }
      if (!fail(index, cp)) return r;
      continue;
    }
    if (state != JIS0208) {
      queue.emplace_back(index, cp);
      state = JIS0208;
      r.bytes += "\x1b$B";
      atom();
      continue;
    }
    r.bytes += char(f->second / 94 + 0x21);
    r.bytes += char(f->second % 94 + 0x21);
    atom();
  }
  if (state != ASCII && flush) {
    r.bytes += "\x1b(B";
    atom();
  }
  return r;
}

inline ModelEncodeResult model_encode(GTEXT_Encoding enc, const std::vector<uint32_t> & cps, bool html) {
  return model_encode_ex(enc, cps, html ? ModelMode::Html : ModelMode::Fatal);
}

inline std::vector<uint32_t> cps_of_utf8(const std::string & s) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = (unsigned char)s[i];
    uint32_t cp;
    size_t n;
    if (c < 0x80) { cp = c; n = 1; }
    else if (c < 0xE0) { cp = c & 0x1F; n = 2; }
    else if (c < 0xF0) { cp = c & 0x0F; n = 3; }
    else { cp = c & 0x07; n = 4; }
    for (size_t k = 1; k < n && i + k < s.size(); k++) {
      cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
    }
    out.push_back(cp);
    i += n;
  }
  return out;
}

} // namespace enctest

#endif // GHOTI_IO_GTEXT_TESTS_ENCODING_CJK_MODEL_H
