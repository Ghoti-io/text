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
 * Allocation failure in the public HTML API: every entry point, with the n-th allocation failed for
 * every n up to the last one the call makes.
 *
 * What each failed run must be: GTEXT_HTML_E_OOM with no document and no fragment, or - when the
 * allocation that failed was one the call can do without - success with exactly the tree and the
 * errors of the run that failed nothing. And in both cases nothing is left behind: after the document
 * and the result are freed the allocator holds no block, and it has never been asked to free one it
 * did not make (FaultAllocator in markup_test_support.h knows which blocks are live).
 *
 * The sweep ends when a run completes with no failure injected, so the last allocation of the call
 * has been failed too. A document that is rich enough to use every allocation site the layer has
 * (the builder's, the tokenizer's, the tree's, the error list, the result, the document's encoding)
 * is the input.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/html.h>

#include "html_api_test_support.h"
#include "markup_test_support.h"

using namespace html_api_test;

namespace {

const char * const kRich =
    "\xEF\xBB\xBF<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01//EN\" \"u\"><html lang=en><head><title>t&amp;</title>"
    "<script>x<y</script></head><body class=\"a b\" id=x><p>one<b>two<i>three</p><p>four</b></i>"
    "<table><tr><td>x<td>y</table><template><tr><td>t</template><svg viewBox=\"0 0 1 1\"><foreignObject><p>f"
    "</foreignObject><path d=M0/></svg><math><mi>x</mi></math><!-- c --><?pi x?><textarea>\n&lt;</textarea>"
    "\xc3\xa9 \xF0\x9F\x98\x80 &#x41; &notin; </x></y></z>";

struct Mode {
  const char * name;
  Call call;
  bool want_result;
};

const Mode kModes[] = {
    {"parse", {"parse", false, false, {}}, true},
    {"parse-no-result", {"parse", false, false, {}}, false},
    {"fragment", {"fragment", true, false, {}}, true},
    {"fragment-no-result", {"fragment", true, false, {}}, false},
    {"parser-whole", {"parser", false, true, {Cut::WHOLE, 0}}, true},
    {"parser-bytes", {"parser", false, true, {Cut::BYTES, 0}}, true},
    {"parser-split", {"parser", false, true, {Cut::SPLIT, 2}}, true},
    {"parser-fragment-whole", {"parser", true, true, {Cut::WHOLE, 0}}, true},
    {"parser-fragment-bytes", {"parser", true, true, {Cut::BYTES, 0}}, true},
};

class HtmlApiOom : public ::testing::TestWithParam<Mode> {};

std::string mode_name(const ::testing::TestParamInfo<Mode> & info) {
  std::string n = info.param.name;
  for (char & c : n) {
    if (c == '-') {
      c = '_';
    }
  }
  return n;
}

INSTANTIATE_TEST_SUITE_P(EntryPoints, HtmlApiOom, ::testing::ValuesIn(kModes), mode_name);

TEST_P(HtmlApiOom, EveryAllocationCanFailAndNothingLeaks) {
  const Mode & mode = GetParam();
  const std::string input = kRich;
  GTEXT_HTML_Fragment_Context td = ctx(GTEXT_HTML_NS_HTML, "td");
  GTEXT_HTML_Context_Attribute attr = {"encoding", "text/html"};
  GTEXT_HTML_Fragment_Context annotation = ctx(GTEXT_HTML_NS_MATHML, "annotation-xml");
  annotation.attributes = &attr;
  annotation.attribute_count = 1;
  // The attribute list of the context is an allocation of the call too.
  for (const GTEXT_HTML_Fragment_Context * context : {&td, &annotation}) {
    if (!mode.call.fragment && context == &annotation) {
      continue;
    }
    // The run that fails nothing, for the comparison.
    Outcome baseline = run_call(mode.call, input, nullptr, context, mode.want_result);
    ASSERT_TRUE(baseline.ok()) << mode.name;
    size_t last = 0;
    bool reached_the_end = false;
    size_t oom_runs = 0;
    for (size_t n = 1; n <= 5000; n++) {
      FaultAllocator fa;
      GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
      o.allocator = &fa.alloc;
      {
        fa.arm(n);
        Outcome got = run_call(mode.call, input, &o, context, mode.want_result);
        fa.disarm();
        if (!fa.failed) {
          // Past the last allocation: the normal answer.
          ASSERT_TRUE(got.ok()) << mode.name << " " << n;
          EXPECT_EQ(got.tree, baseline.tree) << mode.name << " " << n;
          if (mode.want_result) {
            EXPECT_EQ(got.errors, baseline.errors);
          }
          reached_the_end = true;
          last = n;
        }
        else if (got.ok()) {
          // An allocation the call could do without: the same answer.
          EXPECT_EQ(got.tree, baseline.tree) << mode.name << " " << n;
          if (mode.want_result) {
            // The result is part of the answer: a success is complete, errors, counts and encoding included.
            EXPECT_EQ(got.errors, baseline.errors) << mode.name << " " << n;
            EXPECT_EQ(got.dropped, baseline.dropped) << mode.name << " " << n;
            EXPECT_EQ(got.node_count, baseline.node_count) << mode.name << " " << n;
            EXPECT_EQ(got.encoding, baseline.encoding) << mode.name << " " << n;
            EXPECT_EQ(got.source, baseline.source) << mode.name << " " << n;
            EXPECT_EQ(got.mode, baseline.mode) << mode.name << " " << n;
          }
        }
        else {
          oom_runs++;
          EXPECT_EQ(got.status, GTEXT_HTML_E_OOM) << mode.name << " " << n << ": " << gtext_html_status_string(got.status);
          EXPECT_EQ(got.doc, nullptr) << mode.name << " " << n;
          EXPECT_EQ(got.fragment, nullptr);
          if (mode.want_result) {
            EXPECT_EQ(got.result_status, GTEXT_HTML_E_OOM);
          }
        }
      } // the document and the result are freed here
      EXPECT_EQ(fa.bad_frees, 0u) << mode.name << " " << n << ": a free of a block the allocator did not make";
      EXPECT_TRUE(fa.live.empty()) << mode.name << " " << n << ": " << fa.live.size() << " blocks leaked";
      if (reached_the_end || ::testing::Test::HasFailure()) {
        break;
      }
    }
    EXPECT_TRUE(reached_the_end) << mode.name << ": the sweep did not reach the end of the call";
    EXPECT_GT(oom_runs, 20u) << mode.name << ": the sweep should have failed many allocations (last " << last << ")";
  }
}

TEST_P(HtmlApiOom, TheAllocatorSeesEveryAllocationOfTheCall) {
  const Mode & mode = GetParam();
  FaultAllocator fa;
  GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
  o.allocator = &fa.alloc;
  const GTEXT_HTML_Fragment_Context td = ctx(GTEXT_HTML_NS_HTML, "td");
  fa.arm(0);
  {
    Outcome got = run_call(mode.call, kRich, &o, &td, mode.want_result);
    ASSERT_TRUE(got.ok());
    EXPECT_GT(fa.calls, 50u);
    EXPECT_FALSE(fa.live.empty()) << "the document lives in the caller's allocator";
  }
  EXPECT_TRUE(fa.live.empty());
  EXPECT_EQ(fa.bad_frees, 0u);
}

TEST(HtmlApiOomParser, FeedingAfterAFailureAndFreeingAreSafe) {
  // The allocation that fails is in the middle of a feed; the parser then repeats OOM and frees clean.
  const std::string input = kRich;
  for (size_t n = 1; n < 400; n++) {
    FaultAllocator fa;
    GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
    o.allocator = &fa.alloc;
    GTEXT_HTML_Parser * p = nullptr;
    fa.arm(n);
    const GTEXT_HTML_Status made = gtext_html_parser_new(&o, &p);
    if (made != GTEXT_HTML_OK) {
      EXPECT_EQ(made, GTEXT_HTML_E_OOM);
      EXPECT_EQ(p, nullptr);
      EXPECT_TRUE(fa.live.empty());
      continue;
    }
    GTEXT_HTML_Status s = GTEXT_HTML_OK;
    for (size_t at = 0; at < input.size() && s == GTEXT_HTML_OK; at += 5) {
      s = gtext_html_parser_feed(p, input.data() + at, std::min<size_t>(5, input.size() - at));
    }
    if (s != GTEXT_HTML_OK) {
      EXPECT_EQ(s, GTEXT_HTML_E_OOM);
      EXPECT_EQ(gtext_html_parser_feed(p, "x", 1), GTEXT_HTML_E_OOM);
      GTEXT_Markup_Document * doc = nullptr;
      GTEXT_HTML_Result r;
      fa.disarm();
      EXPECT_EQ(gtext_html_parser_finish(p, &doc, &r), GTEXT_HTML_E_OOM);
      EXPECT_EQ(doc, nullptr);
      gtext_html_result_free(&r);
    }
    gtext_html_parser_free(p);
    EXPECT_TRUE(fa.live.empty()) << n;
    EXPECT_EQ(fa.bad_frees, 0u);
    if (!fa.failed) {
      break;
    }
  }
}

} // namespace
