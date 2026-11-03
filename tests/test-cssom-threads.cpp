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
 * The documented concurrency model (css_cssom_document.h): one writer thread mutates the live objects and, between mutations, asks for the index; any number of reader threads hold a retained
 * index and compute against it while the writer goes on. The writer applies 10^4 mutations (rules inserted and deleted, declarations edited, sheets switched off and on, adopted sheets, a
 * resize), rebuilds the index after each, and publishes it with the answer it computed itself, alone, at that index; eight readers take whichever index is published, compute the same
 * question on their own match context and compare. A reader never sees an index that is being built, and an index is never altered after it was handed out. It runs in the ThreadSanitizer
 * arm with the other tests.
 */

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "cssom_document_test_support.h"

using namespace cod;

namespace {

/// The cascaded color, margin and padding of every `p`, `span` and `div` of the page through an index, on a match context of the caller's own.
std::string answers(const Page & page, const GTEXT_CSS_Rule_Index * index) {
  GTEXT_CSS_Selector_Match_Context * ctx = nullptr;
  EXPECT_EQ(gtext_css_selector_match_context_new(gtext_css_markup_interface(page.adapter), &page.mo, &ctx), GTEXT_CSS_OK);
  std::string out;
  for (GTEXT_Markup_Node * el : page.all("*")) {
    cas::Set set(nullptr);
    EXPECT_EQ(gtext_css_rules_collect(index, ctx, gtext_css_markup_handle(el), nullptr, set.s, nullptr, nullptr), GTEXT_CSS_OK);
    cas::Out o(set);
    out += o.value("color") + "/" + o.value("margin-top") + "/" + o.value("padding-top") + ";";
  }
  gtext_css_selector_match_context_free(ctx);
  return out;
}

struct Slot {
  std::mutex m;
  GTEXT_CSS_Rule_Index * index = nullptr;   // an owner held by the slot
  std::string want;
  uint64_t number = 0;
  bool stop = false;
};

TEST(CssomThreads, OneWriterAndEightReadersEveryResultIsTheSingleThreadedOneAtTheSameIndex) {
  std::string html = "<!DOCTYPE html><div id=d class=card>";
  for (int i = 0; i < 6; i++) {
    html += "<p class=k" + std::to_string(i % 3) + ">x</p><span>y</span>";
  }
  html += "</div>";
  World w(nullptr, nullptr, html);
  // the page is read by every thread: each takes its own adapter (the adapter keeps lookups of its last question), the document it reads is not written
  Slot slot;
  std::atomic<uint64_t> checked{0};
  std::atomic<int> bad{0};
  std::vector<std::thread> readers;
  for (int t = 0; t < 8; t++) {
    readers.emplace_back([&] {
      Page mine(html);
      uint64_t last = 0;
      for (;;) {
        GTEXT_CSS_Rule_Index * held = nullptr;
        std::string want;
        uint64_t number = 0;
        bool stop = false;
        {
          std::lock_guard<std::mutex> g(slot.m);
          stop = slot.stop;
          if (slot.index) {
            held = gtext_css_rule_index_retain(slot.index);
            want = slot.want;
            number = slot.number;
          }
        }
        if (held) {
          if (number != last) {
            if (answers(mine, held) != want) {
              bad++;
            }
            last = number;
            checked++;
          }
          gtext_css_rule_index_free(held);
        }
        if (stop) {
          break;
        }
        std::this_thread::yield();
      }
    });
  }
  // the writer
  GTEXT_CSSOM_Scope * scope = gtext_cssom_document_scope(w.d());
  Owned a(w, "p{color:red} span{margin-top:1px} @media (min-width: 1000px){ p{padding-top:3px} } .k1{margin-top:2px}");
  Owned b(w, "div{color:green} p.k2{color:blue}");
  Constructed k1(*w.doc, "p{margin-top:9px}"), k2(*w.doc, "span{padding-top:4px}");
  Cond wide(1400), narrow(800);
  uint64_t seed = 12345;
  auto next = [&]() {
    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<unsigned>(seed >> 33);
  };
  for (unsigned i = 0; i < 10000; i++) {
    const unsigned r = next();
    switch (r % 8) {
    case 0: insert_rule(a.s, "p.k" + std::to_string(r % 3) + "{color:" + (r % 2 ? "orange" : "purple") + "}", 0); break;
    case 1: {
      const size_t n = gtext_cssom_rule_list_length(a.rules());
      if (n > 4) delete_rule(a.s, r % n);
      break;
    }
    case 2: {
      Rule rule = at(b.rules(), 1);
      if (rule && gtext_cssom_rule_kind(rule.r) == GTEXT_CSSOM_RULE_STYLE) {
        Decl d = style_of(rule.r);
        set(d.d, "color", r % 2 ? "teal" : "navy");
      }
      break;
    }
    case 3: gtext_cssom_sheet_set_disabled(b.s, (r >> 4) % 2 == 0, nullptr); break;
    case 4: {
      GTEXT_CSSOM_Sheet * both[] = {k1.s, k2.s};
      GTEXT_CSSOM_Sheet * one[] = {k2.s};
      if ((r >> 3) % 2) gtext_cssom_scope_adopted_set(scope, both, 2, nullptr);
      else gtext_cssom_scope_adopted_set(scope, one, 1, nullptr);
      break;
    }
    case 5: gtext_cssom_document_set_conditions(w.d(), (r >> 5) % 2 ? wide.c : narrow.c, nullptr); break;
    case 6: insert_rule(k1.s, "p{padding-top:" + std::to_string(r % 4) + "px}", 0); break;
    case 7: {
      const GTEXT_CSSOM_Rule_List * list = nullptr;
      EXPECT_EQ(gtext_cssom_sheet_rules(k1.s, &list), GTEXT_CSS_OK);
      if (gtext_cssom_rule_list_length(list) > 1) delete_rule(k1.s, 0);
      break;
    }
    }
    GTEXT_CSS_Rule_Index * x = nullptr;
    ASSERT_EQ(gtext_cssom_document_index_retained(w.d(), &x), GTEXT_CSS_OK);
    const std::string want = answers(w.page, x);
    {
      std::lock_guard<std::mutex> g(slot.m);
      gtext_css_rule_index_free(slot.index);
      slot.index = x;
      slot.want = want;
      slot.number = i + 1;
    }
  }
  {
    std::lock_guard<std::mutex> g(slot.m);
    slot.stop = true;
  }
  for (auto & t : readers) {
    t.join();
  }
  gtext_css_rule_index_free(slot.index);
  EXPECT_EQ(bad.load(), 0);
  const char * slow_env = std::getenv("GTEXT_TEST_SLOWDOWN");   // under Valgrind the threads take turns and the readers get fewer reads in the writer's time
  const double slow = slow_env && std::atof(slow_env) >= 1.0 && std::atof(slow_env) <= 1000.0 ? std::atof(slow_env) : 1.0;
  EXPECT_GT(static_cast<double>(checked.load()), 100.0 / slow) << "the readers did work while the writer wrote";
}

}  // namespace
