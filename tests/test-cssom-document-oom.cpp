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
 * Allocation failure in the document layer (css_cssom_document.h): every call that allocates is swept over the n-th allocation for every n up to the last one it makes. A call that
 * fails is GTEXT_CSS_E_OOM (or the status its callback gave) and leaves the objects, the lists, the generation, the ids, the live counts and the events as they were, and builds no index;
 * the call repeated with a healthy allocator ends in the state the unswept call ends in; nothing leaks once the world is gone.
 */

#include <functional>
#include <string>

#include <gtest/gtest.h>

#include "cssom_document_test_support.h"

using namespace cod;

namespace {

const std::function<void(GTEXT_CSSOM_Config &)> kNoMemo = [](GTEXT_CSSOM_Config & c) { c.max_cached_text_bytes = 1; };

/// Everything a scenario holds: the allocator under test, the callback's table, the world, and the things to let go of before the world goes.
struct Env {
  FaultAllocator & fa;
  Importer imp;
  std::unique_ptr<World> w;
  std::unique_ptr<World> other;   ///< A second document, for the scope that moves; declared after `w`, so it goes first.
  std::vector<std::function<void()>> cleanup;
  std::vector<GTEXT_CSSOM_Sheet *> sheets;
  std::vector<GTEXT_CSSOM_Scope *> scopes;
  std::vector<GTEXT_CSSOM_Media_Query_List *> lists;
  std::vector<Cond *> conds;
  std::vector<Compiled *> compiled;
  std::vector<std::string> log;
  int handles[4] = {0, 0, 0, 0};

  explicit Env(FaultAllocator & allocator) : fa(allocator) {
    w = std::make_unique<World>([this](GTEXT_CSSOM_Config & c) {
      kNoMemo(c);
      imp.install(c);
    }, &fa.alloc);
  }
  Env(const Env &) = delete;
  Env & operator=(const Env &) = delete;
  ~Env() {
    for (auto it = cleanup.rbegin(); it != cleanup.rend(); ++it) {
      (*it)();
    }
    for (auto * l : lists) gtext_cssom_media_query_list_release(l);
    for (auto * s : scopes) gtext_cssom_scope_release(s);
    for (auto * s : sheets) gtext_cssom_sheet_release(s);
    for (auto * c : conds) delete c;
    for (auto * c : compiled) delete c;
  }
  GTEXT_CSSOM_Sheet * sheet(const std::string & text) {
    GTEXT_CSSOM_Sheet * s = nullptr;
    EXPECT_EQ(gtext_cssom_sheet_new_parsed(w->d(), text.data(), text.size(), nullptr, &s), GTEXT_CSS_OK);
    sheets.push_back(s);
    return s;
  }
  GTEXT_CSSOM_Sheet * constructed(const std::string & text) {
    GTEXT_CSSOM_Sheet * s = nullptr;
    EXPECT_EQ(gtext_cssom_sheet_new_constructed(w->d(), nullptr, &s), GTEXT_CSS_OK);
    EXPECT_EQ(gtext_cssom_sheet_replace_sync(s, text.data(), text.size(), nullptr), GTEXT_CSS_OK);
    sheets.push_back(s);
    return s;
  }
  GTEXT_CSSOM_Scope * scope(int handle, bool shadow = true, GTEXT_CSSOM_Scope * parent = nullptr) {
    GTEXT_CSSOM_Scope_Options o = gtext_cssom_scope_options_default();
    o.handle = &handles[handle];
    o.shadow = shadow;
    o.parent = parent;
    GTEXT_CSSOM_Scope * s = nullptr;
    EXPECT_EQ(gtext_cssom_scope_new(w->d(), &o, &s), GTEXT_CSS_OK);
    scopes.push_back(s);
    return s;
  }
  GTEXT_CSSOM_Scope * doc_scope() {
    return gtext_cssom_document_scope(w->d());
  }
  /// What a failed call must not have changed.
  std::string state() {
    const GTEXT_CSSOM_Counters c = w->counters();
    std::string s = "g" + std::to_string(gtext_cssom_document_generation(w->d())) + " id" + std::to_string(w->doc->core()->next_id) + " o" + std::to_string(c.live_objects) + " b" +
                    std::to_string(c.live_bytes - c.cached_text_bytes) + " e" + std::to_string(w->doc->events.size()) + " ib" + std::to_string(c.index_builds) + " sc" +
                    std::to_string(c.live_scopes) + " ml" + std::to_string(c.mql_notifications) + " ir" + std::to_string(c.import_requests);
    for (auto * sc : scopes) {
      s += " [" + std::to_string(gtext_cssom_scope_sheet_count(sc)) + "/" + std::to_string(gtext_cssom_scope_adopted_count(sc)) + "/" + std::to_string(gtext_cssom_scope_context(sc)) + "/" +
           std::to_string(reinterpret_cast<uintptr_t>(gtext_cssom_scope_document(sc)) != 0) + "]";
    }
    s += " [" + std::to_string(gtext_cssom_scope_sheet_count(doc_scope())) + "/" + std::to_string(gtext_cssom_scope_adopted_count(doc_scope())) + "]";
    for (auto * sh : sheets) {
      s += " v" + std::to_string(gtext_cssom_sheet_version(sh)) + "p" + std::to_string(gtext_cssom_sheet_pending_imports(sh)) + "d" + std::to_string(gtext_cssom_sheet_diagnostic_total(sh));
    }
    return s;
  }
};

struct Scenario {
  const char * name;
  std::function<void(Env &)> setup;
  std::function<GTEXT_CSS_Status(Env &)> op;
  std::function<std::string(Env &)> result;   ///< What the call leaves that the state does not show.
  bool callback_ids = false;
  size_t min_failures = 1;     ///< The call is meant to allocate: 0 for one that is swept to show that it does not.   ///< The import callback makes sheets, which take ids and count as requests even when the call that asked fails: those two figures are not compared.
};

/// The state without a field (" id12": a space, the tag and its digits).
std::string without_field(const std::string & state, const std::string & tag) {
  const size_t at = state.find(" " + tag);
  if (at == std::string::npos) {
    return state;
  }
  size_t end = at + 1 + tag.size();
  while (end < state.size() && state[end] >= '0' && state[end] <= '9') {
    end++;
  }
  return state.substr(0, at) + state.substr(end);
}

std::string comparable(const Scenario & sc, std::string state) {
  if (sc.callback_ids) {
    state = without_field(without_field(state, "id"), "ir");
  }
  return state;
}

TEST(CssomDocumentOom, EveryCallThatAllocatesIsAllOrNothing) {
  const std::vector<Scenario> scenarios = {
      {"a scope", [](Env & e) { e.scope(0); },
          [](Env & e) {
            GTEXT_CSSOM_Scope_Options o = gtext_cssom_scope_options_default();
            o.handle = &e.handles[1];
            o.shadow = true;
            o.parent = e.scopes[0];
            GTEXT_CSSOM_Scope * s = nullptr;
            const GTEXT_CSS_Status st = gtext_cssom_scope_new(e.w->d(), &o, &s);
            if (st == GTEXT_CSS_OK) {
              e.scopes.push_back(s);
            }
            return st;
          },
          [](Env & e) { return std::to_string(e.scopes.size()) + ":" + std::to_string(e.w->counters().live_scopes); }},
      {"an owner sheet", [](Env & e) { e.sheet("p{color:red}"); e.sheet("q{}"); },
          [](Env & e) { return gtext_cssom_scope_insert_sheet(e.doc_scope(), e.sheets[1], 0, nullptr); },
          [](Env & e) { return std::to_string(gtext_cssom_scope_sheet_count(e.doc_scope())); }},
      {"an adopted sheet", [](Env & e) { e.constructed("p{}"); e.constructed("q{}"); },
          [](Env & e) { return gtext_cssom_scope_adopted_insert(e.doc_scope(), e.sheets[1], 0, nullptr); },
          [](Env & e) { return std::to_string(gtext_cssom_scope_adopted_count(e.doc_scope())); }},
      {"the adopted list set", [](Env & e) { e.constructed("p{}"); e.constructed("q{}"); },
          [](Env & e) {
            GTEXT_CSSOM_Sheet * list[] = {e.sheets[0], e.sheets[1], e.sheets[0]};
            return gtext_cssom_scope_adopted_set(e.doc_scope(), list, 3, nullptr);
          },
          [](Env & e) { return std::to_string(gtext_cssom_scope_adopted_count(e.doc_scope())); }},
      {"a registered sheet", [](Env & e) { e.compiled.push_back(new Compiled("p{color:red}")); },
          [](Env & e) { return gtext_cssom_document_register_sheet(e.w->d(), GTEXT_CSS_ORIGIN_USER, e.compiled[0]->s, nullptr); },
          [](Env &) { return std::string(); }},
      {"matchMedia", [](Env &) {},
          [](Env & e) {
            GTEXT_CSSOM_Media_Query_List * l = nullptr;
            const GTEXT_CSS_Status st = gtext_cssom_match_media(e.w->d(), "(min-width: 900px) and (max-width: 2000px), print", 49, &l);
            if (st == GTEXT_CSS_OK) {
              e.lists.push_back(l);
            }
            return st;
          },
          [](Env & e) {
            bool m = false;
            return std::string(gtext_cssom_media_query_list_matches(e.lists.empty() ? nullptr : e.lists[0], &m) == GTEXT_CSS_OK ? (m ? "1" : "0") : "?");
          }},
      {"a listener", [](Env & e) {
            GTEXT_CSSOM_Media_Query_List * l = nullptr;
            EXPECT_EQ(gtext_cssom_match_media(e.w->d(), "(min-width: 900px)", 18, &l), GTEXT_CSS_OK);
            e.lists.push_back(l);
          },
          [](Env & e) {
            return gtext_cssom_media_query_list_add_listener(e.lists[0], [](void *, GTEXT_CSSOM_Media_Query_List *, bool) { return GTEXT_CSS_OK; }, &e);
          },
          [](Env & e) { return std::to_string(gtext_cssom_media_query_list_listener_count(e.lists[0])); }},
      {"an environment change with listeners", [](Env & e) {
            for (const char * q : {"(min-width: 900px)", "(min-width: 1100px)", "print", "(min-width: 900px)"}) {
              GTEXT_CSSOM_Media_Query_List * l = nullptr;
              EXPECT_EQ(gtext_cssom_match_media(e.w->d(), q, std::strlen(q), &l), GTEXT_CSS_OK);
              EXPECT_EQ(gtext_cssom_media_query_list_add_listener(l, [](void * u, GTEXT_CSSOM_Media_Query_List *, bool m) {
                          static_cast<Env *>(u)->log.push_back(m ? "1" : "0");
                          return GTEXT_CSS_OK;
                        }, &e), GTEXT_CSS_OK);
              e.lists.push_back(l);
            }
            e.conds.push_back(new Cond(1000, &e.fa.alloc));
          },
          [](Env & e) { return gtext_cssom_document_set_conditions(e.w->d(), e.conds[0]->c, nullptr); },
          [](Env & e) {
            std::string s;
            for (const auto & x : e.log) s += x;
            return s + "/" + std::to_string(gtext_cssom_document_conditions(e.w->d()) == e.conds[0]->c);
          }},
      {"the index of a document with every kind of sheet", [](Env & e) {
            e.compiled.push_back(new Compiled("p{color:green}"));
            EXPECT_EQ(gtext_cssom_document_register_sheet(e.w->d(), GTEXT_CSS_ORIGIN_USER_AGENT, e.compiled[0]->s, nullptr), GTEXT_CSS_OK);
            GTEXT_CSSOM_Sheet * owner = e.sheet("@import url(c.css); p{color:red} @media print{ q{width:1px} }");
            EXPECT_EQ(gtext_cssom_scope_insert_sheet(e.doc_scope(), owner, 0, nullptr), GTEXT_CSS_OK);
            GTEXT_CSSOM_Sheet * k = e.constructed("p{margin-top:1px} @layer a { span { color:blue } }");
            EXPECT_EQ(gtext_cssom_scope_adopted_insert(e.doc_scope(), k, 0, nullptr), GTEXT_CSS_OK);
            GTEXT_CSSOM_Scope * shadow = e.scope(0);
            GTEXT_CSSOM_Sheet * in_shadow = e.sheet("p{padding-top:1px}");
            EXPECT_EQ(gtext_cssom_scope_insert_sheet(shadow, in_shadow, 0, nullptr), GTEXT_CSS_OK);
          },
          [](Env & e) {
            const GTEXT_CSS_Rule_Index * x = nullptr;
            return gtext_cssom_document_index(e.w->d(), &x);
          },
          [](Env & e) {
            const GTEXT_CSS_Rule_Index * x = nullptr;
            gtext_cssom_document_index(e.w->d(), &x);
            return x ? std::to_string(gtext_css_rule_index_record_count(x)) + "/" + std::to_string(gtext_css_rule_index_sheet_count(x)) : std::string("none");
          }},
      {"the index after an environment change that moves a media query", [](Env & e) {
            GTEXT_CSSOM_Sheet * s = e.sheet("@media (min-width: 1000px) { p{color:red} } q{width:1px}");
            EXPECT_EQ(gtext_cssom_scope_insert_sheet(e.doc_scope(), s, 0, nullptr), GTEXT_CSS_OK);
            const GTEXT_CSS_Rule_Index * x = nullptr;
            EXPECT_EQ(gtext_cssom_document_index(e.w->d(), &x), GTEXT_CSS_OK);
            e.conds.push_back(new Cond(500, &e.fa.alloc));
            EXPECT_EQ(gtext_cssom_document_set_conditions(e.w->d(), e.conds[0]->c, nullptr), GTEXT_CSS_OK);
          },
          [](Env & e) {
            const GTEXT_CSS_Rule_Index * x = nullptr;
            return gtext_cssom_document_index(e.w->d(), &x);
          },
          [](Env & e) {
            const GTEXT_CSS_Rule_Index * x = nullptr;
            gtext_cssom_document_index(e.w->d(), &x);
            return std::to_string(gtext_css_rule_index_record_count(x));
          }},
      {"a scope removed", [](Env & e) {
            GTEXT_CSSOM_Scope * s = e.scope(0);
            EXPECT_EQ(gtext_cssom_scope_adopted_insert(s, e.constructed("p{}"), 0, nullptr), GTEXT_CSS_OK);
            EXPECT_EQ(gtext_cssom_scope_insert_sheet(s, e.sheet("q{}"), 0, nullptr), GTEXT_CSS_OK);
          },
          [](Env & e) { return gtext_cssom_scope_remove(e.scopes[0], nullptr); },
          [](Env & e) { return std::to_string(gtext_cssom_scope_sheet_count(e.scopes[0])); }, false, 0},
      {"a scope moved to another document", [](Env & e) { e.scope(0); e.other = std::make_unique<World>(nullptr, &e.fa.alloc); },
          [](Env & e) { return gtext_cssom_scope_reparent(e.scopes[0], e.other->d(), nullptr, nullptr); },
          [](Env & e) { return std::to_string(gtext_cssom_scope_document(e.scopes[0]) == e.other->d()); }, false, 0},
      {"an @import made from text with a sheet given and one pending", [](Env & e) {
            e.imp.table["a.css"] = "a{color:red}";
            e.imp.table["b.css"] = "pending";
          },
          [](Env & e) {
            GTEXT_CSSOM_Sheet * s = nullptr;
            const std::string text = "@import url(a.css); @import url(b.css); p{}";
            const GTEXT_CSS_Status st = gtext_cssom_sheet_new_parsed(e.w->d(), text.data(), text.size(), nullptr, &s);
            if (st == GTEXT_CSS_OK) {
              e.sheets.push_back(s);
            }
            return st;
          },
          [](Env & e) { return std::to_string(e.sheets.size()) + ":" + std::to_string(gtext_cssom_sheet_pending_imports(e.sheets.empty() ? nullptr : e.sheets[0])); }, true},
      {"insertRule of an @import", [](Env & e) {
            e.imp.table["a.css"] = "a{color:red}";
            e.sheet("p{}");
            e.sheet("");
          },
          [](Env & e) { return insert_rule(e.sheets[1], "@import url(a.css)", 0).st; },
          [](Env & e) { return std::to_string(gtext_cssom_sheet_rule_total(e.sheets[1])) + ":" + std::to_string(gtext_cssom_sheet_pending_imports(e.sheets[1])); }, true},
  };
  for (const Scenario & sc : scenarios) {
    std::string want_state, want_result;
    {
      FaultAllocator fa;
      Env e(fa);
      sc.setup(e);
      ASSERT_EQ(sc.op(e), GTEXT_CSS_OK) << sc.name;
      want_state = comparable(sc, e.state());
      want_result = sc.result(e);
    }
    size_t failures = 0, swallowed = 0;
    for (size_t n = 1; n < 100000; n++) {
      FaultAllocator fa;
      {
        Env e(fa);
        sc.setup(e);
        const std::string before = comparable(sc, e.state());
        fa.arm(n);
        const GTEXT_CSS_Status st = sc.op(e);
        fa.disarm();
        if (!fa.failed) {
          ASSERT_EQ(st, GTEXT_CSS_OK) << sc.name << " at " << n;
          EXPECT_EQ(comparable(sc, e.state()), want_state) << sc.name << ": the sweep ended at " << n;
          EXPECT_EQ(sc.result(e), want_result) << sc.name;
          break;
        }
        if (st == GTEXT_CSS_OK) {
          swallowed++;
          continue;
        }
        failures++;
        EXPECT_EQ(st, GTEXT_CSS_E_OOM) << sc.name << " at " << n << ": " << gtext_css_status_string(st);
        EXPECT_EQ(comparable(sc, e.state()), before) << sc.name << ": the state changed by the failure at " << n;
        // the call repeated with a healthy allocator ends where the unswept call ends
        ASSERT_EQ(sc.op(e), GTEXT_CSS_OK) << sc.name << " after the failure at " << n;
        EXPECT_EQ(comparable(sc, e.state()), want_state) << sc.name << " after the failure at " << n;
        EXPECT_EQ(sc.result(e), want_result) << sc.name << " after the failure at " << n;
      }
      EXPECT_TRUE(fa.live.empty()) << sc.name << ": a leak after the failure at " << n;
      EXPECT_EQ(fa.bad_frees, 0u);
    }
    EXPECT_GE(failures, sc.min_failures) << sc.name << ": the call makes allocations";
    EXPECT_EQ(swallowed, 0u) << sc.name << ": an allocation that failed was an allocation the call needed";
  }
}

TEST(CssomDocumentOom, TheDocumentItselfIsAllOrNothing) {
  size_t failures = 0;
  for (size_t n = 1; n < 1000; n++) {
    FaultAllocator fa;
    GTEXT_CSSOM_Document * d = nullptr;
    GTEXT_CSSOM_Config c = gtext_cssom_config_default();
    c.registry = R();
    c.allocator = &fa.alloc;
    fa.arm(n);
    const GTEXT_CSS_Status st = gtext_cssom_document_new(&c, &d);
    fa.disarm();
    if (!fa.failed) {
      ASSERT_EQ(st, GTEXT_CSS_OK);
      gtext_cssom_document_release(d);
      EXPECT_TRUE(fa.live.empty());
      break;
    }
    failures++;
    EXPECT_EQ(st, GTEXT_CSS_E_OOM);
    EXPECT_EQ(d, nullptr);
    EXPECT_TRUE(fa.live.empty()) << "a leak after the failure at " << n;
    EXPECT_EQ(fa.bad_frees, 0u);
  }
  EXPECT_GE(failures, 3u);
}

TEST(CssomDocumentOom, CallsThatFindTheirAnswerAllocateNothing) {
  FaultAllocator fa;
  Env e(fa);
  e.sheet("p{color:red}");
  ASSERT_EQ(gtext_cssom_scope_insert_sheet(e.doc_scope(), e.sheets[0], 0, nullptr), GTEXT_CSS_OK);
  GTEXT_CSSOM_Media_Query_List * l = nullptr;
  ASSERT_EQ(gtext_cssom_match_media(e.w->d(), "(min-width: 900px)", 18, &l), GTEXT_CSS_OK);
  e.lists.push_back(l);
  const GTEXT_CSS_Rule_Index * x = nullptr;
  ASSERT_EQ(gtext_cssom_document_index(e.w->d(), &x), GTEXT_CSS_OK);
  bool m = false;
  ASSERT_EQ(gtext_cssom_media_query_list_matches(l, &m), GTEXT_CSS_OK);
  fa.arm(1);
  const GTEXT_CSS_Rule_Index * y = nullptr;
  EXPECT_EQ(gtext_cssom_document_index(e.w->d(), &y), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_media_query_list_matches(l, &m), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_document_set_conditions(e.w->d(), gtext_cssom_document_conditions(e.w->d()), nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_scope_adopted_remove(e.doc_scope(), 0, nullptr), GTEXT_CSS_E_INDEX_SIZE);
  EXPECT_FALSE(fa.failed) << "a cached index, a cached answer and a change to the same conditions allocate nothing";
  fa.disarm();
  EXPECT_EQ(y, x);
}

}  // namespace
