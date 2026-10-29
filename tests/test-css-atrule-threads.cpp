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
 * An at-rule and a frozen set are immutable: many threads read one rule (typed views, serialization, retain and free) and one set at once. Run under TSan on the build host.
 */

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_cascade_test_support.h"
#include "css_test_support.h"

using cas::R;

TEST(CssAtruleThreads, OneRuleReadByManyThreads) {
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_stylesheet,
      "@font-face { font-family: A; src: url(a.woff2) format(woff2), local(x); unicode-range: U+25-FF, U+4??; font-weight: 200 900 }");
  ASSERT_NE(p.tree, nullptr);
  GTEXT_CSS_Atrule * a = nullptr;
  GTEXT_CSS_Atrule_Result res;
  GTEXT_CSS_Atrule_Options o = gtext_css_atrule_options_default();
  ASSERT_EQ(gtext_css_atrule_parse(R(), &p.tree->rules[0], &o, &a, &res), GTEXT_CSS_OK);
  std::string want;
  {
    char * buf = nullptr;
    size_t n = 0;
    ASSERT_EQ(gtext_css_atrule_serialize_buffer(a, nullptr, &buf, &n), GTEXT_CSS_OK);
    want.assign(buf, n);
    gtext_css_atrule_buffer_free(nullptr, buf);
  }
  std::atomic<int> bad{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([&] {
      for (int i = 0; i < 200; i++) {
        GTEXT_CSS_Atrule * mine = gtext_css_atrule_retain(a);
        GTEXT_CSS_Font_Face * f = nullptr;
        if (gtext_css_font_face_from_atrule(mine, &f) != GTEXT_CSS_OK || f->range_count != 2 || !gtext_css_font_face_defined(f)) {
          bad++;
        }
        gtext_css_font_face_free(f);
        char * buf = nullptr;
        size_t n = 0;
        if (gtext_css_atrule_serialize_buffer(mine, nullptr, &buf, &n) != GTEXT_CSS_OK || std::string(buf, n) != want) {
          bad++;
        }
        gtext_css_atrule_buffer_free(nullptr, buf);
        gtext_css_atrule_free(mine);
      }
    });
  }
  for (auto & t : threads) {
    t.join();
  }
  EXPECT_EQ(bad.load(), 0);
  gtext_css_atrule_free(a);
}

TEST(CssAtruleThreads, OneSetReadByManyThreads) {
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_stylesheet,
      "@keyframes k { from { color: red } to { color: blue } } @counter-style c { system: extends d } @counter-style d { system: cyclic; symbols: a } "
      "@font-face { font-family: Z; src: local(z) } @page :first { margin: 1px }");
  GTEXT_CSS_Atrule_Set_Options so = gtext_css_atrule_set_options_default();
  GTEXT_CSS_Atrule_Set * set = nullptr;
  ASSERT_EQ(gtext_css_atrule_set_new(R(), &so, &set), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_atrule_set_add_tree(set, p.tree), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_atrule_set_freeze(set), GTEXT_CSS_OK);
  std::atomic<int> bad{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([&] {
      for (int i = 0; i < 500; i++) {
        const GTEXT_CSS_Keyframes * k = gtext_css_atrule_set_keyframes(set, "k", 1);
        const GTEXT_CSS_Counter_Style * c = gtext_css_atrule_set_counter_style(set, "c", 1);
        const GTEXT_CSS_Font_Face * faces[2];
        char key[8];
        const size_t kn = gtext_css_atrule_set_family_key("Z", 1, key, sizeof key);
        GTEXT_CSS_Page_Context pc{};
        pc.first = true;
        GTEXT_CSS_Page_Match m[4];
        if (!k || k->frame_count != 2 || !c || c->resolved_system != GTEXT_CSS_COUNTER_CYCLIC || gtext_css_atrule_set_font_faces(set, key, kn, faces, 2) != 1 ||
            gtext_css_atrule_set_page_collect(set, &pc, m, 4) != 1) {
          bad++;
        }
      }
    });
  }
  for (auto & t : threads) {
    t.join();
  }
  EXPECT_EQ(bad.load(), 0);
  gtext_css_atrule_set_free(set);
}
