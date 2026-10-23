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
 * Allocation failure in the CSS parser: every entry point, with the n-th
 * allocation failed for every n up to the last one the call makes.
 *
 * A failed run is GTEXT_CSS_E_OOM with no tree, or the answer of the run that
 * failed nothing (the same tree and the same errors). Either way the
 * allocator holds no block afterwards and was never asked to free one it did
 * not make. The input uses every allocation site: tokens past the first
 * growth of their array, a string past its first buffer, the bracket match
 * table, the value and rule stacks, a re-tokenized unicode-range value, a
 * custom property's original text, a location, a dropped block, and the
 * error list.
 */

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_bytes_test_support.h"
#include "css_test_support.h"
#include "markup_test_support.h"

using namespace css_test;

namespace {

std::string rich() {
  std::string s = "@import url(x.css) layer(a); @media (min-width: 1px) { a { b: c } d: e; }\n";
  s += "x, y > z { color: red !important; --custom: { a ] b } c; unicode-range: U+0-7F, u+4??;";
  s += " & .n { m: n } after: rule; font: 12px/1.5 \"" + std::string(200, 'q') + "\" }\n";
  s += "--x:hover { a {} } p { q: url(bad url) 'bad\n; r: s } t } u {";
  for (int i = 0; i < 40; i++) {
    s += " v" + std::to_string(i) + ": f(g(h[i{j}])) " + std::to_string(i) + "px;";
  }
  return s;
}

struct Entry {
  const char * name;
  ParseFn fn;
};

const Entry kEntries[] = {
    {"stylesheet", gtext_css_parse_stylesheet},
    {"stylesheet_contents", gtext_css_parse_stylesheet_contents},
    {"declaration_list", gtext_css_parse_declaration_list},
    {"block_contents", gtext_css_parse_block_contents},
    {"rule", gtext_css_parse_rule},
    {"declaration", gtext_css_parse_declaration},
    {"component_value", gtext_css_parse_component_value},
    {"component_values", gtext_css_parse_component_values},
    {"comma_list", gtext_css_parse_comma_list},
};

struct Outcome {
  GTEXT_CSS_Status status;
  std::string tree;
  std::vector<std::string> errors;
  size_t dropped = 0;
};

Outcome run(ParseFn fn, const std::string & css, const GTEXT_CSS_Parse_Options * o, bool with_result) {
  Outcome out;
  GTEXT_CSS_Tree * t = nullptr;
  GTEXT_CSS_Result r{};
  out.status = fn(css.data(), css.size(), o, &t, with_result ? &r : nullptr);
  if (t) {
    out.tree = canon(t) + "\n" + serialize(t);
  }
  for (size_t i = 0; i < gtext_css_result_error_count(&r); i++) {
    GTEXT_CSS_Error e;
    gtext_css_result_error_at(&r, i, &e);
    out.errors.push_back(std::string(e.name) + "@" + std::to_string(e.byte_offset));
  }
  out.dropped = gtext_css_result_errors_dropped(&r);
  if (out.status != GTEXT_CSS_OK) {
    EXPECT_EQ(t, nullptr) << "a failure leaves no tree";
  }
  gtext_css_tree_free(t);
  gtext_css_result_free(&r);
  return out;
}

void sweep(const Entry & e, const std::string & css, bool with_result) {
  GTEXT_CSS_Parse_Options base = gtext_css_parse_options_default();
  base.location = "a-location";
  const Outcome want = run(e.fn, css, &base, with_result);
  ASSERT_EQ(want.status, GTEXT_CSS_OK) << e.name;
  bool reached_the_end = false;
  size_t failures = 0;
  for (size_t n = 1; n <= 20000 && !reached_the_end; n++) {
    FaultAllocator fa;
    GTEXT_CSS_Parse_Options o = base;
    o.allocator = &fa.alloc;
    fa.arm(n);
    const Outcome got = run(e.fn, css, &o, with_result);
    fa.disarm();
    EXPECT_TRUE(fa.live.empty()) << e.name << " " << n << ": a block is left";
    EXPECT_EQ(fa.bad_frees, 0u) << e.name << " " << n;
    if (!fa.failed) {
      reached_the_end = true;
      EXPECT_EQ(got.status, GTEXT_CSS_OK);
      EXPECT_EQ(got.tree, want.tree);
      continue;
    }
    if (got.status == GTEXT_CSS_E_OOM) {
      failures++;
      continue;
    }
    ASSERT_EQ(got.status, GTEXT_CSS_OK) << e.name << " " << n;
    EXPECT_EQ(got.tree, want.tree) << e.name << " " << n;
    EXPECT_EQ(got.errors, want.errors) << e.name << " " << n;
    EXPECT_EQ(got.dropped, want.dropped) << e.name << " " << n;
  }
  EXPECT_TRUE(reached_the_end) << e.name << ": the sweep did not reach the last allocation";
  EXPECT_GT(failures, 10u) << e.name << ": the allocations failed should mostly be refused";
}

TEST(CssOom, EveryAllocationOfEveryEntryPointCanFailAndNothingLeaks) {
  const std::string css = rich();
  for (const Entry & e : kEntries) {
    SCOPED_TRACE(e.name);
    sweep(e, css, true);
  }
}

TEST(CssOom, WithNoResultTheSameHolds) {
  const std::string css = rich();
  for (const Entry & e : kEntries) {
    SCOPED_TRACE(e.name);
    sweep(e, css, false);
  }
}

bool any(void *, size_t, const GTEXT_CSS_Component_Value *, size_t n) {
  return n > 0;
}

TEST(CssOom, TheGrammarEntryPointsToo) {
  const std::string css = "a, b(c), d e";
  GTEXT_CSS_Grammar g = {any, nullptr};
  bool done = false;
  for (size_t n = 1; n < 500 && !done; n++) {
    FaultAllocator fa;
    GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
    o.allocator = &fa.alloc;
    fa.arm(n);
    GTEXT_CSS_Tree * t = nullptr;
    bool matched = false;
    size_t m = 0;
    GTEXT_CSS_Status st = gtext_css_parse_grammar(css.data(), css.size(), &g, &o, &matched, &t, nullptr);
    gtext_css_tree_free(st == GTEXT_CSS_OK ? t : nullptr);
    t = nullptr;
    GTEXT_CSS_Status st2 = gtext_css_parse_grammar_list(css.data(), css.size(), &g, &o, &m, &t, nullptr);
    gtext_css_tree_free(st2 == GTEXT_CSS_OK ? t : nullptr);
    fa.disarm();
    EXPECT_TRUE(st == GTEXT_CSS_OK || st == GTEXT_CSS_E_OOM);
    EXPECT_TRUE(st2 == GTEXT_CSS_OK || st2 == GTEXT_CSS_E_OOM);
    if (st2 == GTEXT_CSS_OK) {
      EXPECT_EQ(m, 3u);
    }
    EXPECT_TRUE(fa.live.empty()) << n;
    done = !fa.failed;
  }
  EXPECT_TRUE(done);
}

// A number is converted by the math library, which allocates nothing and reads no locale. This was a test of the
// conversion this library had before, which respelled a long number in a locale whose decimal separator is not "."
// through the parse's allocator: its allocation failing had to fail the parse and not make the number 0. It stays,
// because the property it holds is still the one that matters (a parse that succeeds under any failed allocation
// has the right number), now in a locale that would have changed the old conversion's answer.
bool use_a_comma_locale() {
  // (the POSIX names, and on Windows the CRT's, which has no UTF-8 locales: tests/test-locale-numbers.cpp)
  const char * names[] = {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "fr_FR.utf8"
#ifdef _WIN32
      , "German_Germany", "French_France"
#endif
  };
  auto comma = [&]() {
    for (const char * n : names) {
      if (setlocale(LC_NUMERIC, n) && strcmp(localeconv()->decimal_point, ",") == 0) {
        return true;
      }
    }
    return false;
  };
  if (comma()) {
    return true;
  }
#ifdef _WIN32
  // nothing to generate with: no localedef, no LOCPATH, no mkdtemp
  return false;
#else
  // A host that has no such locale makes one (LOCPATH may already name a directory with it: suite/tools/xarch/text.sh makes one natively
  // for an emulated target, which cannot run localedef itself).
  char tmpl[] = "/tmp/gtext-css-locale-XXXXXX";
  const char * dir = mkdtemp(tmpl);
  if (!dir) {
    return false;
  }
  setenv("LOCPATH", dir, 1);
  const std::string cmd = std::string("env -u LD_PRELOAD -u LD_LIBRARY_PATH localedef -i de_DE -f UTF-8 ") + dir
      + "/de_DE.UTF-8 >/dev/null 2>&1";
  return system(cmd.c_str()) == 0 && comma();
#endif
}

TEST(CssOom, ANumbersConversionNeedsNoMemoryAndNoLocale) {
  const std::string saved = setlocale(LC_NUMERIC, nullptr) ? setlocale(LC_NUMERIC, nullptr) : "C";
  ASSERT_TRUE(use_a_comma_locale()) << "no comma-decimal locale could be made; a pass here would measure nothing";
  // 150 digits after the point: the old conversion respelled a number this long in the heap.
  const std::string css = "a{width:0." + std::string(150, '9') + "px}";
  size_t refused = 0;
  bool done = false;
  for (size_t n = 1; n < 400 && !done; n++) {
    FaultAllocator fa;
    GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
    o.allocator = &fa.alloc;
    fa.arm(n);
    GTEXT_CSS_Tree * t = nullptr;
    const GTEXT_CSS_Status st = gtext_css_parse_stylesheet(css.data(), css.size(), &o, &t, nullptr);
    fa.disarm();
    if (st == GTEXT_CSS_E_OOM) {
      EXPECT_EQ(t, nullptr);
      refused++;
    }
    else {
      ASSERT_EQ(st, GTEXT_CSS_OK) << n;
      ASSERT_NE(t, nullptr);
      // 0.999...9 with 150 nines is 1 in a double; a conversion that failed would have made it 0.
      const GTEXT_CSS_Component_Value & v = t->rules[0].declarations[0].value[0];
      EXPECT_EQ(v.type, GTEXT_CSS_TOKEN_DIMENSION);
      EXPECT_DOUBLE_EQ(v.number, 1.0) << "allocation " << n << " failed and the parse went on with the number " << v.number;
    }
    gtext_css_tree_free(t);
    EXPECT_TRUE(fa.live.empty()) << n;
    done = !fa.failed;
  }
  EXPECT_TRUE(done);
  EXPECT_GT(refused, 5u);
  setlocale(LC_NUMERIC, saved.c_str());
}

// ---- the byte entry point and the stream -----------------------------------------------------------------------

struct ByteCase {
  const char * name;
  std::string bytes;
  const char * label;
};

std::vector<ByteCase> byte_cases() {
  std::string longer = rich();
  return {
      {"utf-8", longer, nullptr},
      {"utf-8 with a mark", "\xef\xbb\xbf" + longer, nullptr},
      {"windows-1251", CSS_BYTES("@charset \"windows-1251\"; .\xef\xf0{b:\"\xe8\"} x{y:\"z\n} p{q:url(bad url)}") + longer, nullptr},
      {"utf-16le", CSS_BYTES("\xff\xfe" "a\x00{\x00" "b\x00:\x00\xe9\x00}\x00 \x00x\x00{\x00y\x00:\x00\x22\x00z\x00\x0a\x00}\x00"), nullptr},
      {"iso-2022-jp", CSS_BYTES("a{b:c}/*\x1b$BF|K\x5c" "8l\x1b(B*/.\x1b$BF|K\x5c\x1b(B{d:e}\x0a") + longer, "iso-2022-jp"},
      {"gb18030", CSS_BYTES(".\xd6\xd0\xce\xc4{b:c} /*\x95" "2\x82" "6*/ x{y:\x22\x95" "2\x82" "6z\n}") + longer, "gb18030"},
  };
}

// How a run is made: the one-call function, a stream fed whole, a stream fed a byte at a time, or a stream of text.
enum Mode { WHOLE, STREAM_WHOLE, STREAM_BYTES, STREAM_TEXT };

css_test::Outcome run_bytes(Mode mode, const ByteCase & c, const GTEXT_CSS_Parse_Options * o) {
  GTEXT_CSS_Bytes_Options b = gtext_css_bytes_options_default();
  b.protocol_label = c.label;
  GTEXT_CSS_Tree * t = nullptr;
  GTEXT_CSS_Result r{};
  GTEXT_CSS_Status st;
  if (mode == WHOLE) {
    st = gtext_css_parse_stylesheet_bytes(c.bytes.data(), c.bytes.size(), &b, o, &t, &r);
  }
  else {
    GTEXT_CSS_Stream * s = nullptr;
    st = mode == STREAM_TEXT ? gtext_css_stream_new(GTEXT_CSS_TREE_STYLESHEET, o, &s)
                             : gtext_css_stream_new_bytes(GTEXT_CSS_TREE_STYLESHEET, &b, o, &s);
    if (st == GTEXT_CSS_OK) {
      const size_t step = mode == STREAM_BYTES ? 1 : c.bytes.size();
      for (size_t at = 0; at < c.bytes.size() && st == GTEXT_CSS_OK; at += step) {
        st = gtext_css_stream_feed(s, c.bytes.data() + at, std::min(step, c.bytes.size() - at));
      }
      const GTEXT_CSS_Status fin = gtext_css_stream_finish(s, &t, &r);
      if (st == GTEXT_CSS_OK) {
        st = fin;
      }
      gtext_css_stream_free(s);
    }
  }
  css_test::Outcome out = harvest(st, st == GTEXT_CSS_OK ? t : nullptr, r);
  out.status = st;
  EXPECT_TRUE(st == GTEXT_CSS_OK || t == nullptr) << "a failure leaves no tree";
  gtext_css_tree_free(t);
  gtext_css_result_free(&r);
  return out;
}

TEST(CssOomBytes, EveryAllocationOfTheByteEntryPointsAndTheStreamCanFailAndNothingLeaks) {
  for (const ByteCase & c : byte_cases()) {
    for (const Mode mode : {WHOLE, STREAM_WHOLE, STREAM_BYTES, STREAM_TEXT}) {
      SCOPED_TRACE(std::string(c.name) + " / mode " + std::to_string(mode));
      GTEXT_CSS_Parse_Options base = gtext_css_parse_options_default();
      base.location = "a-location";
      const css_test::Outcome want = run_bytes(mode, c, &base);
      ASSERT_EQ(want.status, GTEXT_CSS_OK);
      bool reached_the_end = false;
      size_t refused = 0;
      for (size_t n = 1; n <= 40000 && !reached_the_end; n++) {
        FaultAllocator fa;
        GTEXT_CSS_Parse_Options o = base;
        o.allocator = &fa.alloc;
        fa.arm(n);
        const css_test::Outcome got = run_bytes(mode, c, &o);
        fa.disarm();
        ASSERT_TRUE(fa.live.empty()) << n << ": a block is left";
        ASSERT_EQ(fa.bad_frees, 0u) << n;
        if (!fa.failed) {
          reached_the_end = true;
          expect_same_outcome(want, got, "no failure");
          continue;
        }
        if (got.status == GTEXT_CSS_E_OOM) {
          refused++;
          // A failed allocation has no place, in any mode: not the input limit's byte, not a byte order mark's length.
          EXPECT_EQ(got.failure.offset, 0u) << n;
          EXPECT_EQ(got.failure.end, 0u) << n;
          EXPECT_EQ(got.failure.line, 0u) << n;
          EXPECT_EQ(got.failure.column, 0u) << n;
          // Once the encoding is decided (the first allocations of a run are the stream's own struct, copies and first
          // buffer, before it) the result says how the bytes were read, whatever the mode, as the one-call function's does.
          if (n > 5) {
            EXPECT_EQ(got.encoding, want.encoding) << "allocation " << n;
            EXPECT_EQ(got.source, want.source) << "allocation " << n;
            EXPECT_EQ(got.bom_length, want.bom_length) << "allocation " << n;
          }
          continue;
        }
        // The allocation that failed was one the run could do without: the answer is the whole's.
        expect_same_outcome(want, got, "allocation " + std::to_string(n) + " failed and the run went on");
      }
      EXPECT_TRUE(reached_the_end) << "the sweep did not reach the last allocation";
      EXPECT_GT(refused, 5u);
    }
  }
}

TEST(CssOomBytes, ATextStreamPastItsLimitSaysMemoryRatherThanAPositionFromLessText) {
  // After the limit a text stream keeps three more bytes to place the failure as the one-call parse does. If keeping them runs out
  // of memory the failure is the memory, not a line and column read from a shorter text.
  const std::string text = "a{b:\n\xc3\xa9\xc3\xa9\xc3\xa9 c} d{e:f}\n";
  for (const size_t max : {size_t{1}, size_t{4}, size_t{9}}) {
    GTEXT_CSS_Parse_Options base = gtext_css_parse_options_default();
    base.max_input_bytes = max;
    const css_test::Outcome want = css_test::whole_text(text, gtext_css_parse_stylesheet, &base);
    ASSERT_EQ(want.status, GTEXT_CSS_E_LIMIT_INPUT);
    size_t memory = 0, placed = 0;
    for (const size_t step : {size_t{1}, size_t{2}, text.size()}) {
      for (size_t n = 1; n < 60; n++) {
        FaultAllocator fa;
        GTEXT_CSS_Parse_Options o = base;
        o.allocator = &fa.alloc;
        fa.arm(n);
        GTEXT_CSS_Stream * s = nullptr;
        GTEXT_CSS_Status st = gtext_css_stream_new(GTEXT_CSS_TREE_STYLESHEET, &o, &s);
        if (st == GTEXT_CSS_OK) {
          for (size_t at = 0; at < text.size(); at += step) {
            (void)gtext_css_stream_feed(s, text.data() + at, std::min(step, text.size() - at));
          }
          GTEXT_CSS_Tree * t = nullptr;
          GTEXT_CSS_Result r{};
          st = gtext_css_stream_finish(s, &t, &r);
          EXPECT_EQ(t, nullptr);
          if (st == GTEXT_CSS_E_LIMIT_INPUT) {
            placed++;
            EXPECT_EQ(r.failure.offset, want.failure.offset) << "max " << max << " allocation " << n;
            EXPECT_EQ(r.failure.line, want.failure.line) << "max " << max << " allocation " << n << ": a position from the whole text";
            EXPECT_EQ(r.failure.column, want.failure.column) << "max " << max << " allocation " << n;
          }
          else {
            EXPECT_EQ(st, GTEXT_CSS_E_OOM) << "max " << max << " allocation " << n;
            memory++;
            EXPECT_EQ(r.failure.line, 0u);
          }
          gtext_css_result_free(&r);
          gtext_css_stream_free(s);
        }
        fa.disarm();
        EXPECT_TRUE(fa.live.empty()) << n;
        if (!fa.failed) {
          break;
        }
      }
    }
    EXPECT_GT(memory, 0u);
    EXPECT_GT(placed, 0u);
  }
}

TEST(CssOomBytes, ARefusedByteParseSaysItAndHoldsNothing) {
  // The first allocation of every path failed: OOM, no tree, a result that says so, and nothing left.
  for (const ByteCase & c : byte_cases()) {
    for (const Mode mode : {WHOLE, STREAM_BYTES}) {
      FaultAllocator fa;
      GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
      o.allocator = &fa.alloc;
      fa.arm(1);
      const css_test::Outcome got = run_bytes(mode, c, &o);
      fa.disarm();
      EXPECT_EQ(got.status, GTEXT_CSS_E_OOM) << c.name;
      EXPECT_TRUE(fa.live.empty()) << c.name;
    }
  }
}

} // namespace
