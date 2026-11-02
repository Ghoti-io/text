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
 * Allocation failure and the observers: every allocation of a mutation with an observer registered, immediately and inside a batch, is failed in turn.
 *
 * A call that fails is GTEXT_CSS_E_OOM and leaves everything as it was, and no event has been sent or queued (the place in a batch is taken before the mutation changes anything). A
 * call that succeeds sends its one event (or the scenario's events, for a mutator that sends several); the event is exactly the healthy run's when `summary_complete`, and when the
 * summary could not be made it says so and says anything may have changed (GTEXT_CSSOM_DEP_ALL for a rule). Nothing is ever lost from a batch (`events_lost`), and nothing leaks.
 */

#include <gtest/gtest.h>

#include "cssom_events_test_support.h"

using namespace coe;

namespace {

struct Scn {
  const char * name;
  std::function<void(Ctx &)> setup;
  std::function<GTEXT_CSS_Status(Ctx &)> op;
  size_t events;                    ///< How many events the call sends when it succeeds.
  bool rule_event;                  ///< The first event is about a rule: a degraded summary says GTEXT_CSSOM_DEP_ALL.
};

std::string signature(const Seen & s) {
  std::string out = std::string(kind_name(s.kind)) + "|p" + path_text(s.path) + (s.has_path ? "+" : "-") + "|s" + s.selectors + "|b" + s.selectors_before + "|d" +
                    std::to_string(s.deps) + "/" + std::to_string(s.deps_before) + "|c" + (s.has_condition ? std::to_string(s.cond_environment) : "-") + "|" + join(s.props) + "|" +
                    join(s.customs) + (s.all ? "|all" : "") + "|n" + std::to_string(s.descendants) + (s.detached ? "|det" : "");
  return out;
}

const std::vector<Scn> & scenarios() {
  static const std::vector<Scn> v = {
      {"insert a nested style rule", [](Ctx & c) { c.owned("a{} @media screen{ b{} }"); c.rule_at(c.sheets[0], 1); },
          [](Ctx & c) { return gtext_cssom_rule_insert_rule(c.rules[0], "p.q > span:hover{color:red}", 26, 1, nullptr, nullptr); }, 1, true},
      {"insert a media rule", [](Ctx & c) { c.owned("a{}"); },
          [](Ctx & c) { return gtext_cssom_sheet_insert_rule(c.sheets[0], "@media (min-width: 3px){ b{} }", 30, 0, nullptr, nullptr); }, 1, true},
      {"delete a rule", [](Ctx & c) { c.owned("a{} @media screen{ b{} c.d{} }"); c.rule_at(c.sheets[0], 1); },
          [](Ctx & c) { return gtext_cssom_rule_delete_rule(c.rules[0], 1, nullptr); }, 1, true},
      {"set a selector text", [](Ctx & c) { c.owned("a.b, #c{color:red}"); c.style_of(c.rule_at(c.sheets[0], 0)); },   // the block is made the first time it is asked for, and stays
          [](Ctx & c) { return gtext_cssom_rule_set_selector_text(c.rules[0], "x > [y]", 7, nullptr); }, 1, true},
      {"set the media of a rule", [](Ctx & c) { c.owned("@media screen{a{}}"); c.media_of(c.rule_at(c.sheets[0], 0)); },
          [](Ctx & c) { return gtext_cssom_media_set_text(c.medias[0], "(color)", 7, nullptr); }, 1, true},
      {"set the media of a sheet", [](Ctx & c) { c.owned("a{}"); c.media_of(c.sheets[0]); },
          [](Ctx & c) { return gtext_cssom_media_set_text(c.medias[0], "(min-width: 9px), print", 23, nullptr); }, 1, false},
      {"change declarations", [](Ctx & c) { c.owned("a:hover{color:red}"); c.style_of(c.rule_at(c.sheets[0], 0)); },
          [](Ctx & c) { return gtext_cssom_decl_set_css_text(c.decls[0], "margin: 1px; --z: 1; --a: 2; color: blue", 39, nullptr); }, 1, true},
      {"remove a scope with sheets", [](Ctx & c) {
            GTEXT_CSSOM_Scope * s = c.scope(0);
            for (int i = 0; i < 3; i++) {
              GTEXT_CSSOM_Sheet * x = c.sheet("p{}");
              EXPECT_EQ(gtext_cssom_scope_insert_sheet(s, x, static_cast<size_t>(i), nullptr), GTEXT_CSS_OK);
            }
            GTEXT_CSSOM_Sheet * k = c.constructed("q{}");
            EXPECT_EQ(gtext_cssom_scope_adopted_insert(s, k, 0, nullptr), GTEXT_CSS_OK);
          },
          [](Ctx & c) { return gtext_cssom_scope_remove(c.scopes[0], nullptr); }, 4, false},
      {"replace a sheet", [](Ctx & c) { c.constructed("a{} b{}"); }, [](Ctx & c) { return gtext_cssom_sheet_replace_sync(c.sheets[0], "c{}", 3, nullptr); }, 1, false},
  };
  return v;
}

/// What the healthy run sends.
std::vector<std::string> healthy(const Scn & sc, bool batch) {
  Ctx c;
  sc.setup(c);
  Rec rec(c.d());
  if (batch) {
    EXPECT_EQ(gtext_cssom_document_begin_batch(c.d()), GTEXT_CSS_OK);
  }
  EXPECT_EQ(sc.op(c), GTEXT_CSS_OK) << sc.name;
  if (batch) {
    EXPECT_TRUE(rec.seen.empty());
    EXPECT_EQ(gtext_cssom_document_end_batch(c.d(), nullptr), GTEXT_CSS_OK);
  }
  EXPECT_EQ(rec.seen.size(), sc.events) << sc.name;
  std::vector<std::string> out;
  for (const Seen & s : rec.seen) {
    out.push_back(signature(s));
  }
  return out;
}

void sweep(bool batch) {
  for (const Scn & sc : scenarios()) {
    SCOPED_TRACE(std::string(sc.name) + (batch ? " in a batch" : ""));
    const std::vector<std::string> want = healthy(sc, batch);
    size_t failures = 0, ok_runs = 0;
    for (size_t n = 1; n < 4000; n++) {
      FaultAllocator fa;
      bool fired = false;
      {
        Ctx c(&fa.alloc);
        sc.setup(c);
        Rec rec(c.d());
        ASSERT_EQ(rec.status, GTEXT_CSS_OK);
        if (batch) {
          ASSERT_EQ(gtext_cssom_document_begin_batch(c.d()), GTEXT_CSS_OK);
        }
        const uint64_t gen = gtext_cssom_document_generation(c.d());
        const std::string before = c.sheets.empty() ? std::string() : dump(c.sheets[0]);
        fa.arm(n);
        const GTEXT_CSS_Status st = sc.op(c);
        fa.disarm();
        fired = fa.failed;
        if (st != GTEXT_CSS_OK) {
          failures++;
          EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
          EXPECT_EQ(gtext_cssom_document_generation(c.d()), gen) << "a failed call moves nothing (" << n << ")";
          EXPECT_TRUE(rec.seen.empty()) << "and sends nothing (" << n << ")";
          if (!c.sheets.empty() && std::string(sc.name) != "remove a scope with sheets" && std::string(sc.name) != "replace a sheet") {
            EXPECT_EQ(dump(c.sheets[0]), before) << n;
          }
          if (batch) {
            EXPECT_EQ(c.w->doc->core()->qcount, 0u) << "and queues nothing (" << n << ")";
          }
        }
        else {
          ok_runs++;
          if (batch) {
            EXPECT_TRUE(rec.seen.empty());
            ASSERT_EQ(gtext_cssom_document_end_batch(c.d(), nullptr), GTEXT_CSS_OK);
          }
          ASSERT_EQ(rec.seen.size(), sc.events) << n;
          for (size_t i = 0; i < rec.seen.size(); i++) {
            if (rec.seen[i].complete) {
              EXPECT_EQ(signature(rec.seen[i]), want[i]) << "a complete summary is the healthy one (" << n << ")";
            }
            else {
              if (sc.rule_event && i == 0) {
                // what could not be made is said to be anything: a selector list, its predecessor, the condition, the path, or a list of properties that is shorter than it should be
                const Seen & x = rec.seen[i];
                const bool marked = x.deps == GTEXT_CSSOM_DEP_ALL || x.deps_before == GTEXT_CSSOM_DEP_ALL || x.cond_environment == 0x3ffu || signature(x) != want[i];
                EXPECT_TRUE(marked) << "a summary that could not be made says what it could not make (" << n << ")";
              }
              EXPECT_EQ(std::string(kind_name(rec.seen[i].kind)), want[i].substr(0, want[i].find('|'))) << "the kind is always right";
            }
          }
          size_t here = 0;
          for (const Seen & x : rec.seen) {
            here += x.complete ? 0 : 1;
          }
          EXPECT_EQ(c.w->counters().events_degraded, here) << "every degraded event is counted";
        }
        EXPECT_EQ(c.w->counters().events_lost, 0u) << n;
      }
      EXPECT_TRUE(fa.live.empty()) << "nothing leaks (" << n << ")";
      EXPECT_EQ(fa.bad_frees, 0u);
      if (!fired) {
        break;   // the call allocated fewer times than n: the sweep is done
      }
    }
    EXPECT_GT(ok_runs, 0u);
    (void)failures;
  }
}

TEST(CssomEventsOom, EveryAllocationOfAMutationWithAnObserverIsAllOrNothing) {
  sweep(false);
}

TEST(CssomEventsOom, AndInsideABatchTheEventsPlaceIsTakenBeforeAnythingChanges) {
  sweep(true);
}

TEST(CssomEventsOom, ARegistrationThatCannotGrowTheRegistryChangesNothing) {
  FaultAllocator fa;
  {
    Doc doc(nullptr, &fa.alloc);
    auto fn = [](void *, const GTEXT_CSSOM_Event *) { return GTEXT_CSS_OK; };
    for (size_t n = 1; n < 20; n++) {
      fa.arm(n);
      GTEXT_CSSOM_Observer_Handle h = 0;
      const GTEXT_CSS_Status st = gtext_cssom_document_observe(doc.d, fn, nullptr, &h);
      fa.disarm();
      if (st == GTEXT_CSS_OK) {
        EXPECT_NE(h, 0u);
        break;
      }
      EXPECT_EQ(st, GTEXT_CSS_E_OOM);
      EXPECT_EQ(h, 0u);
      EXPECT_EQ(gtext_cssom_document_observer_count(doc.d), 0u);
    }
    EXPECT_EQ(gtext_cssom_document_observer_count(doc.d), 1u);
  }
  EXPECT_TRUE(fa.live.empty());
}

}  // namespace
