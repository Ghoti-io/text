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
 * Threads: one conditions object, one parsed list and one custom media set are used by eight threads at once; every thread's answers equal a single thread's.
 */

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

namespace {
GTEXT_Markup_Status collect(void * user, const char * bytes, size_t len) {
  static_cast<std::string *>(user)->append(bytes, len);
  return GTEXT_MARKUP_OK;
}
}  // namespace

TEST(CssMediaThreads, EvaluationAndSerializationAreSafeOnSharedObjects) {
  GTEXT_CSS_Condition_Defs * defs = nullptr;
  ASSERT_EQ(gtext_css_condition_defs_new(nullptr, &defs), GTEXT_CSS_OK);
  const char * d[] = {"--a (color)", "--b (--a), (hover)", "--c (--b) and (width > 100px)"};
  for (const char * t : d) {
    ASSERT_EQ(gtext_css_condition_defs_add_custom_media_text(defs, t, std::strlen(t), nullptr, nullptr), GTEXT_CSS_OK);
  }
  GTEXT_CSS_Conditions_Options co = gtext_css_conditions_options_default();
  co.defs = defs;
  GTEXT_CSS_Conditions * c = nullptr;
  ASSERT_EQ(gtext_css_conditions_new(nullptr, &co, &c), GTEXT_CSS_OK);
  const char * qs = "(--c), (foo), not (--b), (400px <= width <= 2000px) and (hover), print";
  GTEXT_CSS_Media_List * l = nullptr;
  ASSERT_EQ(gtext_css_media_list_parse(qs, std::strlen(qs), nullptr, &l, nullptr), GTEXT_CSS_OK);
  auto run = [&](std::string * text, int * answer) {
    GTEXT_CSS_Condition_Call call;
    gtext_css_condition_call_init(&call);
    bool m = false;
    EXPECT_EQ(gtext_css_conditions_matches_list(c, l, &call, &m), GTEXT_CSS_OK);
    *answer = m;
    GTEXT_Markup_Sink sink{collect, text};
    EXPECT_EQ(gtext_css_media_list_serialize(l, &sink), GTEXT_CSS_OK);
    GTEXT_CSS_Media_List * r = gtext_css_media_list_retain(l);
    gtext_css_media_list_free(r);
    gtext_css_condition_call_free(&call);
  };
  std::string want;
  int want_answer = -1;
  run(&want, &want_answer);
  std::vector<std::thread> threads;
  std::atomic<int> bad{0};
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([&] {
      for (int i = 0; i < 200; i++) {
        std::string s;
        int a = -1;
        run(&s, &a);
        if (s != want || a != want_answer) bad++;
      }
    });
  }
  for (auto & th : threads) th.join();
  EXPECT_EQ(bad.load(), 0);
  gtext_css_media_list_free(l);
  gtext_css_conditions_free(c);
  gtext_css_condition_defs_free(defs);
}
