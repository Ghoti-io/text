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
 * The computed-style call from several threads: a compute context and a parent style are shared (they are immutable, and a style is reference counted with
 * atomic counts); every thread computes the styles of the same elements, retains and releases the shared parent and the styles it makes, and reads their
 * dumps and hashes, which must be the ones a single thread made first. Run under ThreadSanitizer (`make test-tsan`) a missing synchronization is a report; run
 * plainly, a count that is not atomic or a cache that is shared unsafely is a wrong dump or a crash.
 */

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "css_markup_tree.h"
#include "css_svg_registry.h"
#include "markup_test_support.h"

namespace {

std::string dump_text(const GTEXT_CSS_Computed_Style * s) {
  std::string out;
  GTEXT_Markup_Sink sink;
  memset(&sink, 0, sizeof sink);
  sink.user = &out;
  sink.write = [](void * u, const char * b, size_t n) -> GTEXT_Markup_Status {
    static_cast<std::string *>(u)->append(b, n);
    return GTEXT_MARKUP_OK;
  };
  EXPECT_EQ(gtext_css_computed_dump(s, &sink), GTEXT_CSS_OK);
  return out;
}

std::string hash_text(const GTEXT_CSS_Computed_Style * s) {
  unsigned char h[GTEXT_CSS_COMPUTED_HASH_LENGTH];
  EXPECT_EQ(gtext_css_computed_hash(s, h), GTEXT_CSS_OK);
  return std::string(reinterpret_cast<char *>(h), sizeof h);
}

TEST(CssStyleThreads, ComputeRetainAndFreeFromManyThreadsOnASharedContextAndParent) {
  const std::string doc = std::string("<svg xmlns=\"") + GTEXT_CSS_SVG_NAMESPACE + "\" id=\"p\" fill=\"orange\" font-size=\"10\" style=\"--a: 3px; --list: 1 2 3\">"
      "<rect id=\"a\" fill=\"red\" stroke=\"blue\" stroke-width=\"2\" transform=\"rotate(30 5 5)\"/>"
      "<rect id=\"b\" style=\"stroke-width: var(--a); --b: var(--list) 4; fill: green\" opacity=\"0.5\"/>"
      "<path id=\"c\" d=\"M0 0 L10 10\" stroke-dasharray=\"1,2 3\" font-size=\"2em\" style=\"width: calc(1em + 2px); display: contents\"/>"
      "<text id=\"d\" letter-spacing=\"0\" word-spacing=\"normal\" paint-order=\"markers stroke\" style=\"color-scheme: dark; color: light-dark(red, blue)\">x</text></svg>";
  cmt::Tree tree;
  ASSERT_TRUE(tree.parse(doc, false));
  tree.build();
  GTEXT_CSS_Compute_Options co = gtext_css_compute_options_default();
  co.elements = &tree.table;
  GTEXT_CSS_Compute_Context * ctx = nullptr;
  ASSERT_EQ(gtext_css_compute_context_new(svgreg::shared(), nullptr, &co, &ctx, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Computed_Style * parent = nullptr;
  ASSERT_EQ(gtext_css_style_compute(ctx, tree.find("p"), nullptr, nullptr, 0, nullptr, &parent, nullptr), GTEXT_CSS_OK);

  const char * ids[] = {"a", "b", "c", "d"};
  std::string want_dump[4], want_hash[4];
  for (size_t i = 0; i < 4; i++) {
    GTEXT_CSS_Computed_Style * s = nullptr;
    ASSERT_EQ(gtext_css_style_compute(ctx, tree.find(ids[i]), parent, nullptr, 0, nullptr, &s, nullptr), GTEXT_CSS_OK) << ids[i];
    want_dump[i] = dump_text(s);
    want_hash[i] = hash_text(s);
    gtext_css_computed_free(s);
  }
  const std::string parent_dump = dump_text(parent);
  const std::string parent_hash = hash_text(parent);

  const size_t threads = 8, rounds = 150;
  std::atomic<size_t> wrong{0}, failed{0};
  std::atomic<bool> go{false};
  std::vector<std::thread> pool;
  for (size_t t = 0; t < threads; t++) {
    pool.emplace_back([&, t]() {
      while (!go.load()) {
      }
      for (size_t r = 0; r < rounds; r++) {
        const size_t i = (t + r) % 4;
        /* the parent is retained and released by every thread while the others compute from it */
        GTEXT_CSS_Computed_Style * held = gtext_css_computed_retain(parent);
        GTEXT_CSS_Computed_Style * s = nullptr;
        GTEXT_CSS_Compute_Result res;
        const GTEXT_CSS_Status st = gtext_css_style_compute(ctx, tree.find(ids[i]), parent, nullptr, 0, nullptr, &s, &res);
        if (st != GTEXT_CSS_OK) {
          if (failed++ == 0) {
            ADD_FAILURE() << ids[i] << ": " << gtext_css_status_string(st) << " " << res.name << " " << (res.detail ? res.detail : "");
          }
          gtext_css_computed_free(held);
          continue;
        }
        GTEXT_CSS_Computed_Style * again = gtext_css_computed_retain(s);
        if (dump_text(s) != want_dump[i] || hash_text(again) != want_hash[i]) {
          wrong++;
        }
        if (dump_text(held) != parent_dump || hash_text(held) != parent_hash) {
          wrong++;
        }
        gtext_css_computed_free(again);
        gtext_css_computed_free(held);
        gtext_css_computed_free(s);
      }
    });
  }
  go = true;
  for (std::thread & th : pool) {
    th.join();
  }
  EXPECT_EQ(failed.load(), 0u);
  EXPECT_EQ(wrong.load(), 0u);
  EXPECT_EQ(dump_text(parent), parent_dump);
  gtext_css_computed_free(parent);
  gtext_css_compute_context_free(ctx);
}

} // namespace
