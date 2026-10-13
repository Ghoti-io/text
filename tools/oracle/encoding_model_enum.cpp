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
 * The C++ model of the multi-byte decoders (tests/encoding_cjk_model.h) as an enumerator, with the protocol of `encoding-ours enum` and `enumx`
 * (tools/oracle/encoding_ours.c), so that the differential (tools/oracle/encoding_mb.py) can compare this library with the model on every sequence of
 * every population, the stratified ones included, as byte streams and not one at a time. The model is the second reading of the standard, written
 * apart from the library and from tools/oracle/encoding_model.py (see tools/encoding/STANDARD and the manual's "Measured" for what is independent).
 *
 *   encoding-model enum <label hex> <n> <flags> <start> <count>
 *   encoding-model enumx <label hex> <flags> <spec>
 *
 * A record is one length byte and the UTF-8 of the decode, or 255 for an error in fatal mode (flag `f`). Run from the library's directory (the model reads
 * third_party/encoding/indexes.json).
 */

#include "../../tests/encoding_cjk_model.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace enctest;

namespace {

std::string unhex(const char * s) {
  std::string out;
  for (; s[0] && s[1]; s += 2) {
    char buf[3] = {s[0], s[1], 0};
    out += char(std::strtol(buf, nullptr, 16));
  }
  return out;
}

void record(std::string & out, GTEXT_Encoding enc, const unsigned char * in, size_t len, bool fatal) {
  const ModelDecoded m = model_decode(enc, in, len, fatal);
  if (fatal && m.failed) {
    out += char(255);
    return;
  }
  const std::string u = utf8_of_cps(m.cps);
  out += char(u.size());
  out += u;
}

bool parse_spec(const char * text, std::vector<std::vector<unsigned char>> & sets) {
  const char * p = text;
  for (;;) {
    if (sets.size() >= 8) {
      return false;
    }
    bool seen[256] = {false};
    for (;;) {
      char * end = nullptr;
      const long lo = std::strtol(p, &end, 16);
      if (end == p || lo < 0 || lo > 255) {
        return false;
      }
      long hi = lo;
      p = end;
      if (*p == '-') {
        hi = std::strtol(p + 1, &end, 16);
        if (end == p + 1 || hi < lo || hi > 255) {
          return false;
        }
        p = end;
      }
      for (long b = lo; b <= hi; b++) {
        seen[b] = true;
      }
      if (*p == ',') {
        p++;
        continue;
      }
      break;
    }
    std::vector<unsigned char> set;
    for (int b = 0; b < 256; b++) {
      if (seen[b]) {
        set.push_back((unsigned char)b);
      }
    }
    sets.push_back(set);
    if (*p == ':') {
      p++;
      continue;
    }
    return *p == 0;
  }
}

} // namespace

int main(int argc, char ** argv) {
  if (argc >= 2 && std::string(argv[1]) == "enum" && argc == 7) {
    const std::string label = unhex(argv[2]);
    const GTEXT_Encoding enc = gtext_encoding_for_label(label.data(), label.size());
    const int n = std::atoi(argv[3]);
    const bool fatal = std::strchr(argv[4], 'f') != nullptr;
    const uint64_t start = std::strtoull(argv[5], nullptr, 10), count = std::strtoull(argv[6], nullptr, 10);
    if (enc == GTEXT_ENCODING_NONE || n < 1 || n > 8) {
      std::fprintf(stderr, "encoding-model enum: no such encoding\n");
      return 2;
    }
    std::string out;
    for (uint64_t v = start; v < start + count; v++) {
      unsigned char in[8];
      for (int k = 0; k < n; k++) {
        in[k] = (unsigned char)(v >> (8 * (n - 1 - k)));
      }
      record(out, enc, in, size_t(n), fatal);
      if (out.size() > (1u << 20)) {
        std::fwrite(out.data(), 1, out.size(), stdout);
        out.clear();
      }
    }
    std::fwrite(out.data(), 1, out.size(), stdout);
    return 0;
  }
  if (argc == 5 && std::string(argv[1]) == "enumx") {
    const std::string label = unhex(argv[2]);
    const GTEXT_Encoding enc = gtext_encoding_for_label(label.data(), label.size());
    const bool fatal = std::strchr(argv[3], 'f') != nullptr;
    std::vector<std::vector<unsigned char>> sets;
    if (enc == GTEXT_ENCODING_NONE || !parse_spec(argv[4], sets)) {
      std::fprintf(stderr, "encoding-model enumx: no such encoding, or a bad spec\n");
      return 2;
    }
    std::vector<size_t> idx(sets.size(), 0);
    std::string out;
    for (;;) {
      unsigned char in[8];
      for (size_t k = 0; k < sets.size(); k++) {
        in[k] = sets[k][idx[k]];
      }
      record(out, enc, in, sets.size(), fatal);
      if (out.size() > (1u << 20)) {
        std::fwrite(out.data(), 1, out.size(), stdout);
        out.clear();
      }
      size_t k = sets.size();
      while (k > 0 && ++idx[k - 1] == sets[k - 1].size()) {
        idx[k - 1] = 0;
        k--;
      }
      if (k == 0) {
        break;
      }
    }
    std::fwrite(out.data(), 1, out.size(), stdout);
    return 0;
  }
  std::fprintf(stderr, "usage: encoding-model enum <label hex> <n> <flags> <start> <count> | enumx <label hex> <flags> <spec>\n");
  return 2;
}
