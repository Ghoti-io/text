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
 * Out of memory in media queries and the conditions object: an allocation failed at each place in turn is GTEXT_CSS_E_OOM with no result, nothing leaked and nothing freed
 * that the allocator did not make; the first call that succeeds returns what a call that was never failed does.
 */

#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "markup_test_support.h"

namespace {

GTEXT_Markup_Status collect(void * user, const char * bytes, size_t len) {
  static_cast<std::string *>(user)->append(bytes, len);
  return GTEXT_MARKUP_OK;
}

const char * const kQueries =
    "not screen and (min-width: calc(100px + 5em)) and (aspect-ratio: calc(16) / 9), (400px <= width <= 700px) or (foo: bar) or foo(bar) or (--x), "
    "only print and (resolution: 2x), ((color) and (not (hover))), (a b c), unknown";

}  // namespace

TEST(CssMediaOom, ParsingFailsCleanlyAtEveryAllocationAndThenAnswersAsIfNeverFailed) {
  GTEXT_CSS_Media_List * clean = nullptr;
  ASSERT_EQ(gtext_css_media_list_parse(kQueries, std::strlen(kQueries), nullptr, &clean, nullptr), GTEXT_CSS_OK);
  std::string want;
  GTEXT_Markup_Sink sink{collect, &want};
  ASSERT_EQ(gtext_css_media_list_serialize(clean, &sink), GTEXT_CSS_OK);
  gtext_css_media_list_free(clean);
  bool answered = false;
  for (size_t n = 1; n < 5000 && !answered; n++) {
    FaultAllocator fa;
    GTEXT_CSS_Media_Options o = gtext_css_media_options_default();
    o.allocator = &fa.alloc;
    GTEXT_CSS_Media_List * l = nullptr;
    GTEXT_CSS_Media_Result r;
    fa.arm(n);
    const GTEXT_CSS_Status st = gtext_css_media_list_parse(kQueries, std::strlen(kQueries), &o, &l, &r);
    const bool failed = fa.failed;
    fa.disarm();
    if (st == GTEXT_CSS_OK) {
      EXPECT_FALSE(failed) << "at " << n << ": an allocation failed and the call did not say so";
      std::string got;
      GTEXT_Markup_Sink s2{collect, &got};
      EXPECT_EQ(gtext_css_media_list_serialize(l, &s2), GTEXT_CSS_OK);
      EXPECT_EQ(got, want) << "at " << n;
      answered = true;
    } else {
      EXPECT_EQ(st, GTEXT_CSS_E_OOM) << "at " << n;
      EXPECT_EQ(l, nullptr);
    }
    gtext_css_media_list_free(l);
    gtext_css_media_result_free(&r);
    EXPECT_TRUE(fa.live.empty()) << "at " << n << ": " << fa.live.size() << " blocks leaked";
    EXPECT_EQ(fa.bad_frees, 0u);
  }
  EXPECT_TRUE(answered);
}

TEST(CssMediaOom, SerializingToABufferFailsCleanly) {
  GTEXT_CSS_Media_List * l = nullptr;
  ASSERT_EQ(gtext_css_media_list_parse(kQueries, std::strlen(kQueries), nullptr, &l, nullptr), GTEXT_CSS_OK);
  bool answered = false;
  for (size_t n = 1; n < 500 && !answered; n++) {
    FaultAllocator fa;
    char * out = nullptr;
    size_t len = 0;
    fa.arm(n);
    const GTEXT_CSS_Status st = gtext_css_media_list_serialize_buffer(l, &fa.alloc, &out, &len);
    fa.disarm();
    if (st == GTEXT_CSS_OK) {
      EXPECT_GT(len, 0u);
      answered = true;
    } else {
      EXPECT_EQ(st, GTEXT_CSS_E_OOM) << "at " << n;
      EXPECT_EQ(out, nullptr);
    }
    gtext_css_media_buffer_free(&fa.alloc, out);
    EXPECT_TRUE(fa.live.empty()) << "at " << n;
    EXPECT_EQ(fa.bad_frees, 0u);
  }
  EXPECT_TRUE(answered);
  gtext_css_media_list_free(l);
}

TEST(CssMediaOom, DefinitionsTheConditionsObjectAndAnEvaluationFailCleanly) {
  bool answered = false;
  for (size_t n = 1; n < 5000 && !answered; n++) {
    FaultAllocator fa;
    GTEXT_CSS_Condition_Defs * defs = nullptr;
    GTEXT_CSS_Conditions * c = nullptr;
    bool ok = true, failed_status = false;
    fa.arm(n);
    GTEXT_CSS_Status st = gtext_css_condition_defs_new(&fa.alloc, &defs);
    const char * texts[] = {"--a (color)", "--b (--a), (hover)", "--c (--d)", "--d (--c)", "--e false"};
    for (size_t i = 0; st == GTEXT_CSS_OK && i < 5; i++) {
      bool added = false;
      st = gtext_css_condition_defs_add_custom_media_text(defs, texts[i], std::strlen(texts[i]), nullptr, &added);
    }
    if (st == GTEXT_CSS_OK) {
      GTEXT_CSS_Conditions_Options co = gtext_css_conditions_options_default();
      co.allocator = &fa.alloc;
      co.defs = defs;
      st = gtext_css_conditions_new(nullptr, &co, &c);
    }
    bool m = false;
    if (st == GTEXT_CSS_OK) {
      GTEXT_CSS_Condition_Call call;
      gtext_css_condition_call_init(&call);
      const char * q = "(--b) and (width > calc(1px)), (--c)";
      st = gtext_css_conditions_matches_media(c, q, std::strlen(q), &call, &m);
      gtext_css_condition_call_free(&call);
    }
    const bool failed = fa.failed;
    fa.disarm();
    if (st == GTEXT_CSS_OK) {
      EXPECT_FALSE(failed) << "at " << n << ": an allocation failed and the call did not say so";
      EXPECT_TRUE(m);
      answered = true;
    } else {
      failed_status = st == GTEXT_CSS_E_OOM;
      EXPECT_TRUE(failed_status) << "at " << n << ": " << gtext_css_status_string(st);
      (void)ok;
    }
    gtext_css_conditions_free(c);
    gtext_css_condition_defs_free(defs);
    EXPECT_TRUE(fa.live.empty()) << "at " << n << ": " << fa.live.size() << " blocks leaked";
    EXPECT_EQ(fa.bad_frees, 0u);
  }
  EXPECT_TRUE(answered);
}
