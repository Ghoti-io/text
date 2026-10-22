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
 * What the CSS byte and stream tests share: a complete dump of a tree (every
 * field of every node, spans and ids included, which the canonical form of the
 * round trip leaves out), one parse's whole outcome as comparable text, and the
 * ways of cutting an input.
 */

#ifndef GHOTI_IO_GTEXT_TESTS_CSS_BYTES_TEST_SUPPORT_H
#define GHOTI_IO_GTEXT_TESTS_CSS_BYTES_TEST_SUPPORT_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <ghoti.io/text/css.h>

#include "css_test_support.h"
#include "css_tree_dump.h"

namespace css_test {

/// A byte string from a literal, embedded NULs included.
#define CSS_BYTES(literal) std::string(literal, sizeof(literal) - 1)

/// One parse, all of it: the verdict, the tree, the errors, the encoding.
struct Outcome {
  GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;
  std::string tree;                    // dump_tree
  std::vector<std::string> errors;     // code@offset line:column
  size_t dropped = 0;
  GTEXT_CSS_Span failure{};
  GTEXT_Encoding encoding = GTEXT_ENCODING_NONE;
  GTEXT_CSS_Encoding_Source source = GTEXT_CSS_ENCODING_SOURCE_NONE;
  size_t bom_length = 0;
  size_t input_bytes = 0;
  size_t token_count = 0;
  size_t rule_count = 0;
  size_t declaration_count = 0;
  std::string serialized;
};

inline Outcome harvest(GTEXT_CSS_Status st, GTEXT_CSS_Tree * t, GTEXT_CSS_Result & r) {
  Outcome o;
  o.status = st;
  o.tree = t ? dump_tree(t) : std::string();
  if (t) {
    o.serialized = serialize(t);
  }
  for (size_t i = 0; i < gtext_css_result_error_count(&r); i++) {
    GTEXT_CSS_Error e;
    gtext_css_result_error_at(&r, i, &e);
    o.errors.push_back(std::string(e.name) + "@" + std::to_string(e.byte_offset) + " " + std::to_string(e.line) + ":" +
                       std::to_string(e.column));
  }
  o.dropped = gtext_css_result_errors_dropped(&r);
  o.failure = r.failure;
  o.encoding = r.encoding;
  o.source = r.encoding_source;
  o.bom_length = r.bom_length;
  o.input_bytes = r.input_bytes;
  o.token_count = r.token_count;
  o.rule_count = r.rule_count;
  o.declaration_count = r.declaration_count;
  if (r.message) { // (a result no call filled, for a stream that could not be made, has nothing to say)
    EXPECT_EQ(r.status, st);
  }
  return o;
}

/// The one-call parse of bytes (a stylesheet).
inline Outcome whole_bytes(const std::string & bytes, const GTEXT_CSS_Bytes_Options * b = nullptr,
    const GTEXT_CSS_Parse_Options * o = nullptr) {
  GTEXT_CSS_Tree * t = nullptr;
  GTEXT_CSS_Result r{};
  const GTEXT_CSS_Status st = gtext_css_parse_stylesheet_bytes(bytes.data(), bytes.size(), b, o, &t, &r);
  Outcome out = harvest(st, t, r);
  if (st != GTEXT_CSS_OK) {
    EXPECT_EQ(t, nullptr);
  }
  gtext_css_tree_free(t);
  gtext_css_result_free(&r);
  return out;
}

/// The one-call parse of text (a stylesheet, or `fn`).
inline Outcome whole_text(const std::string & text, ParseFn fn = gtext_css_parse_stylesheet,
    const GTEXT_CSS_Parse_Options * o = nullptr) {
  GTEXT_CSS_Tree * t = nullptr;
  GTEXT_CSS_Result r{};
  const GTEXT_CSS_Status st = fn(text.data(), text.size(), o, &t, &r);
  Outcome out = harvest(st, t, r);
  gtext_css_tree_free(t);
  gtext_css_result_free(&r);
  return out;
}

/// A way to cut an input: pieces of the given sizes, then the rest (the sizes
/// repeat if `repeat`), and empty feeds between pieces if `empties`.
struct Cut {
  std::vector<size_t> sizes;
  bool repeat = false;
  bool empties = false;
  std::string name;
};

/// A stream given the input in a cut. Stops feeding at a failure (it returns the same status every time).
inline Outcome through_stream(GTEXT_CSS_Tree_Kind kind, bool bytes_mode, const GTEXT_CSS_Bytes_Options * b,
    const GTEXT_CSS_Parse_Options * o, const std::string & data, const Cut & cut, GTEXT_CSS_Status * feed_status = nullptr) {
  GTEXT_CSS_Stream * s = nullptr;
  const GTEXT_CSS_Status ns = bytes_mode ? gtext_css_stream_new_bytes(kind, b, o, &s) : gtext_css_stream_new(kind, o, &s);
  EXPECT_EQ(ns, GTEXT_CSS_OK);
  size_t at = 0, k = 0;
  GTEXT_CSS_Status fs = GTEXT_CSS_OK;
  while (at < data.size()) {
    size_t take = data.size() - at;
    if (!cut.sizes.empty()) {
      const size_t idx = cut.repeat ? k % cut.sizes.size() : (k < cut.sizes.size() ? k : cut.sizes.size());
      take = idx < cut.sizes.size() ? cut.sizes[idx] : data.size() - at;
    }
    if (take > data.size() - at) {
      take = data.size() - at;
    }
    if (cut.empties) {
      const GTEXT_CSS_Status es = gtext_css_stream_feed(s, nullptr, 0);
      if (es != GTEXT_CSS_OK) {
        fs = es;
      }
    }
    const GTEXT_CSS_Status st = gtext_css_stream_feed(s, data.data() + at, take);
    if (st != GTEXT_CSS_OK && fs == GTEXT_CSS_OK) {
      fs = st;
    }
    at += take;
    k++;
  }
  if (feed_status) {
    *feed_status = fs;
  }
  GTEXT_CSS_Tree * t = nullptr;
  GTEXT_CSS_Result r{};
  const GTEXT_CSS_Status st = gtext_css_stream_finish(s, &t, &r);
  Outcome out = harvest(st, t, r);
  gtext_css_tree_free(t);
  gtext_css_result_free(&r);
  gtext_css_stream_free(s);
  return out;
}

/// The cuts that every input is put through: every single split point, a
/// byte at a time, the same with empty feeds between, and some fixed piece sizes.
inline std::vector<Cut> split_cuts(size_t n) {
  std::vector<Cut> cuts;
  for (size_t k = 0; k <= n; k++) {
    // A long input is split at every byte near its start, near the kilobyte that decides the encoding, near its
    // end, and at a stride between: the places a cut can matter are those, and the cost is the product.
    if (n > 1500 && !(k < 40 || (k >= 1008 && k <= 1040) || k + 40 > n || k % 41 == 0)) {
      continue;
    }
    Cut c;
    c.sizes = {k};
    c.name = "split@" + std::to_string(k);
    cuts.push_back(c);
  }
  Cut one;
  one.sizes = {1};
  one.repeat = true;
  one.name = "bytes";
  cuts.push_back(one);
  Cut one_e = one;
  one_e.empties = true;
  one_e.name = "bytes+empties";
  cuts.push_back(one_e);
  for (size_t p : {2, 3, 5, 7, 64, 1000}) {
    Cut c;
    c.sizes = {p};
    c.repeat = true;
    c.name = "pieces" + std::to_string(p);
    cuts.push_back(c);
  }
  return cuts;
}

/// A cut of random piece sizes (some of them 0).
inline Cut random_cut(uint64_t & state, size_t max_piece) {
  auto next = [&]() {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return state >> 33;
  };
  Cut c;
  const size_t count = 1 + next() % 40;
  for (size_t i = 0; i < count; i++) {
    c.sizes.push_back(next() % (max_piece + 1));
  }
  c.repeat = true;
  c.empties = next() % 3 == 0;
  c.name = "random";
  // A cut of nothing but empty pieces never finishes; make one of them real.
  c.sizes.push_back(1 + next() % max_piece);
  return c;
}

inline void expect_same_outcome(const Outcome & a, const Outcome & b, const std::string & what, bool compare_input_bytes = true) {
  EXPECT_EQ(a.status, b.status) << what;
  EXPECT_EQ(a.tree, b.tree) << what;
  EXPECT_EQ(a.serialized, b.serialized) << what;
  EXPECT_EQ(a.errors, b.errors) << what;
  EXPECT_EQ(a.dropped, b.dropped) << what;
  EXPECT_EQ(a.failure.offset, b.failure.offset) << what;
  EXPECT_EQ(a.failure.end, b.failure.end) << what;
  EXPECT_EQ(a.failure.line, b.failure.line) << what;
  EXPECT_EQ(a.failure.column, b.failure.column) << what;
  EXPECT_EQ(a.encoding, b.encoding) << what;
  EXPECT_EQ(a.source, b.source) << what;
  EXPECT_EQ(a.bom_length, b.bom_length) << what;
  if (compare_input_bytes) {
    EXPECT_EQ(a.input_bytes, b.input_bytes) << what;
  }
  EXPECT_EQ(a.token_count, b.token_count) << what;
  EXPECT_EQ(a.rule_count, b.rule_count) << what;
  EXPECT_EQ(a.declaration_count, b.declaration_count) << what;
}

} // namespace css_test

#endif // GHOTI_IO_GTEXT_TESTS_CSS_BYTES_TEST_SUPPORT_H
