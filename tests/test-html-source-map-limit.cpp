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
 * The limit of the offset-exact route on a build with a 32-bit `size_t` (Phase 3j noted it; Phase 3k exercises it).
 *
 * A document that is not UTF-8 reaches the tree builder through the decoder's source map, one `size_t` per byte of
 * the UTF-8: the offset in the original in the low bits, the width of the character in four bits and the replaced
 * flag at the top. With a 32-bit `size_t` that leaves 27 bits for the offset, so an input is accepted up to
 * 2^27 bytes (128 MiB) and a byte beyond that makes the parse fail with GTEXT_HTML_E_LIMIT_INPUT: a failure with a
 * name, not an offset that wrapped. The UTF-8 route carries no map and has no such limit (its limit is
 * `max_input_bytes`).
 *
 * With a 64-bit `size_t` the limit is 2^59 bytes and cannot be reached, so the two tests that cross it skip (the
 * third states the arithmetic on both). `suite/tools/xarch/text.sh i686` runs them for real, under qemu-i386, and
 * requires that they ran.
 *
 * The input is a series of 1 KiB paragraphs (`<p>` and 1021 letters), so that the document stays a small number of
 * nodes: the text of the whole is 128 MiB and the tree about as much again.
 */

#include <cstdint>
#include <cstdio>
#include <string>

#include <gtest/gtest.h>

#include <ghoti.io/text/html.h>

#include "../src/html/html_tokenizer_internal.h"

namespace {

constexpr size_t kLimit = (size_t)1 << 27;  // the largest input the 32-bit mapped route takes
constexpr size_t kUnit = 1024;              // "<p>" + 1021 letters
constexpr size_t kPiece = 1 << 20;          // what is fed at a time (a whole number of units)

std::string unit() {
  std::string u = "<p>";
  u.append(kUnit - u.size(), 'a');
  return u;
}

std::string piece() {
  std::string p;
  const std::string u = unit();
  for (size_t i = 0; i < kPiece / kUnit; i++) {
    p += u;
  }
  return p;
}

GTEXT_HTML_Parse_Options options(const char * label) {
  GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
  o.max_input_bytes = SIZE_MAX;
  o.max_text_bytes = SIZE_MAX;
  o.max_nodes = SIZE_MAX;
  o.transport_encoding = label;
  o.transport_encoding_certainty = GTEXT_HTML_CERTAIN;
  return o;
}

}  // namespace

TEST(HtmlSourceMapLimit, TheMapHoldsTheOffsetInAllButTheTopFiveBits) {
  EXPECT_EQ(GTEXT_HTML_SOURCE_START_MASK, (((size_t)1) << (sizeof(size_t) * 8 - 5)) - 1);
  EXPECT_EQ(GTEXT_HTML_SOURCE_WIDTH_SHIFT, sizeof(size_t) * 8 - 5);
  EXPECT_EQ(GTEXT_HTML_SOURCE_REPLACED, ((size_t)1) << (sizeof(size_t) * 8 - 1));
  if (sizeof(size_t) == 4) {
    EXPECT_EQ((size_t)GTEXT_HTML_SOURCE_START_MASK + 1, kLimit);  // 128 MiB
  }
  else {
    EXPECT_GT((uint64_t)GTEXT_HTML_SOURCE_START_MASK, (uint64_t)1 << 50);
  }
}

TEST(HtmlSourceMapLimit, ABytePastTheLimitOfTheMappedRouteIsALimitFailure) {
  if (sizeof(size_t) != 4) {
    GTEST_SKIP() << "64-bit size_t: the limit is 2^59 bytes and cannot be reached";
  }
  GTEXT_HTML_Parser * p = nullptr;
  const GTEXT_HTML_Parse_Options o = options("windows-1252");
  ASSERT_EQ(gtext_html_parser_new(&o, &p), GTEXT_HTML_OK);
  const std::string chunk = piece();
  size_t fed = 0;
  while (fed < kLimit) {
    ASSERT_EQ(gtext_html_parser_feed(p, chunk.data(), chunk.size()), GTEXT_HTML_OK) << "at " << fed;
    fed += chunk.size();
  }
  ASSERT_EQ(fed, kLimit);  // every offset up to 2^27 - 1 was taken
  EXPECT_EQ(gtext_html_parser_feed(p, "a", 1), GTEXT_HTML_E_LIMIT_INPUT);
  EXPECT_EQ(gtext_html_parser_feed(p, "a", 1), GTEXT_HTML_E_LIMIT_INPUT) << "the failure is sticky";
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_HTML_Result r;
  EXPECT_EQ(gtext_html_parser_finish(p, &doc, &r), GTEXT_HTML_E_LIMIT_INPUT);
  EXPECT_EQ(doc, nullptr);
  gtext_html_result_free(&r);
  gtext_html_parser_free(p);
}

TEST(HtmlSourceMapLimit, TheUtf8RouteCarriesNoMapAndGoesPastIt) {
  if (sizeof(size_t) != 4) {
    GTEST_SKIP() << "64-bit size_t: 128 MiB is nowhere near a limit of the map, and the 32-bit test is the point";
  }
  GTEXT_HTML_Parser * p = nullptr;
  const GTEXT_HTML_Parse_Options o = options("utf-8");
  ASSERT_EQ(gtext_html_parser_new(&o, &p), GTEXT_HTML_OK);
  const std::string chunk = piece();
  size_t fed = 0;
  while (fed < kLimit + kPiece) {
    ASSERT_EQ(gtext_html_parser_feed(p, chunk.data(), chunk.size()), GTEXT_HTML_OK) << "at " << fed;
    fed += chunk.size();
  }
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_HTML_Result r;
  ASSERT_EQ(gtext_html_parser_finish(p, &doc, &r), GTEXT_HTML_OK);
  ASSERT_NE(doc, nullptr);
  gtext_markup_document_free(doc);
  gtext_html_result_free(&r);
  gtext_html_parser_free(p);
}
