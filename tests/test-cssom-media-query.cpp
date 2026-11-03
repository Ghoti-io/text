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
 * `matchMedia()`, `MediaQueryList` and the environment change (css_cssom_document.h): Media Queries 4 "evaluate media queries and report changes" (every list with a listener is
 * evaluated once, a list whose answer changed calls its listeners in the order they were added, the lists in the order they were made), the cached `matches`, a change that fails
 * leaving the document as it was, and `CSS.supports()` over the document's conditions.
 */

#include <string>

#include <gtest/gtest.h>

#include "cssom_document_test_support.h"

using namespace cod;

namespace {

struct Log {
  std::vector<std::string> calls;
};

/// A listener that records `tag:matches`, then runs `extra`.
struct Tag {
  Log * log;
  std::string tag;
  std::function<GTEXT_CSS_Status(GTEXT_CSSOM_Media_Query_List *, bool)> extra;
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  Tag(Log * l, const std::string & t, std::function<GTEXT_CSS_Status(GTEXT_CSSOM_Media_Query_List *, bool)> e = nullptr) : log(l), tag(t), extra(std::move(e)) {}
};

GTEXT_CSS_Status listener(void * user, GTEXT_CSSOM_Media_Query_List * list, bool matches) {
  Tag * t = static_cast<Tag *>(user);
  t->log->calls.push_back(t->tag + ":" + (matches ? "1" : "0"));
  if (t->extra) {
    return t->extra(list, matches);
  }
  return t->status;
}

/// A media query list that releases itself.
struct Mql {
  GTEXT_CSSOM_Media_Query_List * l = nullptr;
  GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;
  Mql(GTEXT_CSSOM_Document * d, const std::string & text) {
    status = gtext_cssom_match_media(d, text.data(), text.size(), &l);
  }
  Mql(const Mql &) = delete;
  Mql & operator=(const Mql &) = delete;
  ~Mql() {
    gtext_cssom_media_query_list_release(l);
  }
  bool matches() const {
    bool m = false;
    EXPECT_EQ(gtext_cssom_media_query_list_matches(l, &m), GTEXT_CSS_OK);
    return m;
  }
  std::string media() const {
    char * out = nullptr;
    size_t n = 0;
    EXPECT_EQ(gtext_cssom_media_query_list_media(l, &out, &n), GTEXT_CSS_OK);
    std::string s(out, n);
    gtext_cssom_buffer_free(nullptr, out);
    return s;
  }
};

bool font_metric(void *, const GTEXT_CSS_Font_Query *, GTEXT_CSS_Font_Metric, double * px) {
  *px = 8;
  return true;
}

/// A conditions object that can answer `ex`, or one that cannot.
GTEXT_CSS_Conditions * conditions_with_metrics(bool with) {
  GTEXT_CSS_Media_Environment env = gtext_css_media_environment_default(nullptr);
  env.font_metric = with ? font_metric : nullptr;
  GTEXT_CSS_Conditions_Options o = gtext_css_conditions_options_default();
  o.media = &env;
  GTEXT_CSS_Conditions * c = nullptr;
  EXPECT_EQ(gtext_css_conditions_new(R(), &o, &c), GTEXT_CSS_OK);
  return c;
}

TEST(CssomMediaQuery, MatchMediaParsesAndEvaluatesForTheDocumentsConditions) {
  Cond c(1000);
  World w([&](GTEXT_CSSOM_Config & cfg) { cfg.conditions = c.c; });
  Mql wide(w.d(), "(min-width: 900px)"), narrow(w.d(), "(min-width: 1100px)"), print(w.d(), "print"), screen(w.d(), "screen"), none(w.d(), "");
  EXPECT_TRUE(wide.matches());
  EXPECT_FALSE(narrow.matches());
  EXPECT_FALSE(print.matches());
  EXPECT_TRUE(screen.matches());
  EXPECT_TRUE(none.matches()) << "the empty list matches everything";
  EXPECT_EQ(none.media(), "");
  EXPECT_EQ(wide.media(), "(min-width: 900px)") << "the serialization of the media query list";
  // Media Queries 4: a query that is not valid is `not all`
  Mql bad(w.d(), "and");
  EXPECT_EQ(bad.media(), "not all");
  EXPECT_FALSE(bad.matches());
  EXPECT_EQ(gtext_cssom_match_media(nullptr, "", 0, &bad.l), GTEXT_CSS_E_INVALID);
}

TEST(CssomMediaQuery, MatchesIsCachedPerConditionsObjectAndAskedAgainWhenTheEnvironmentChanges) {
  Cond a(1000), b(500);
  World w([&](GTEXT_CSSOM_Config & cfg) { cfg.conditions = a.c; });
  Mql m(w.d(), "(min-width: 900px)");
  const uint64_t e0 = w.counters().mql_evaluations;
  EXPECT_TRUE(m.matches());
  EXPECT_TRUE(m.matches());
  EXPECT_TRUE(m.matches());
  EXPECT_EQ(w.counters().mql_evaluations, e0 + 1);
  ASSERT_EQ(gtext_cssom_document_set_conditions(w.d(), b.c, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(w.counters().mql_evaluations, e0 + 1) << "a list with no listener is not evaluated by the change";
  EXPECT_FALSE(m.matches());
  EXPECT_FALSE(m.matches());
  EXPECT_EQ(w.counters().mql_evaluations, e0 + 2);
  ASSERT_EQ(gtext_cssom_document_set_conditions(w.d(), a.c, nullptr), GTEXT_CSS_OK);
  EXPECT_TRUE(m.matches()) << "back to the first object: asked again, the epoch is a different one";
  EXPECT_EQ(w.counters().mql_evaluations, e0 + 3);
}

TEST(CssomMediaQuery, TheEnvironmentChangeEvaluatesTheListsWithListenersOnceAndReportsTheChangedOnesInOrder) {
  Cond c1000(1000), c1200(1200), c800(800);
  World w([&](GTEXT_CSSOM_Config & cfg) { cfg.conditions = c1000.c; });
  Log log;
  Mql a(w.d(), "(min-width: 900px)"), b(w.d(), "(min-width: 1100px)"), pr(w.d(), "print"), d(w.d(), "(min-width: 900px)"), idle(w.d(), "(min-width: 1100px)");
  Tag a1{&log, "A1"}, a2{&log, "A2"}, tb{&log, "B"}, tc{&log, "C"}, td{&log, "D"};
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(d.l, listener, &td), GTEXT_CSS_OK);   // added first, but made after a
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(a.l, listener, &a1), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(a.l, listener, &a2), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(a.l, listener, &a1), GTEXT_CSS_OK) << "the same pair is added once";
  EXPECT_EQ(gtext_cssom_media_query_list_listener_count(a.l), 2u);
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(b.l, listener, &tb), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(pr.l, listener, &tc), GTEXT_CSS_OK);
  const uint64_t e0 = w.counters().mql_evaluations;
  w.doc->events.clear();
  GTEXT_CSSOM_Outcome o{};
  ASSERT_EQ(gtext_cssom_document_set_conditions(w.d(), c1200.c, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(log.calls, (std::vector<std::string>{"B:1"})) << "only b changed";
  EXPECT_EQ(w.counters().mql_evaluations, e0 + 3) << "four lists with listeners, three different texts; the idle list is not evaluated";
  ASSERT_EQ(w.doc->events.size(), 1u);
  EXPECT_EQ(w.doc->events[0].kind, GTEXT_CSSOM_EVENT_CONDITIONS_CHANGED);
  log.calls.clear();
  ASSERT_EQ(gtext_cssom_document_set_conditions(w.d(), c800.c, &o), GTEXT_CSS_OK);
  EXPECT_EQ(log.calls, (std::vector<std::string>{"A1:0", "A2:0", "B:0", "D:0"})) << "lists in the order they were made, the listeners of one in the order they were added";
  EXPECT_EQ(o.observer_status, GTEXT_CSS_OK);
  EXPECT_EQ(w.counters().mql_notifications, 5u);
  log.calls.clear();
  ASSERT_EQ(gtext_cssom_document_set_conditions(w.d(), c800.c, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_UNCHANGED);
  EXPECT_TRUE(log.calls.empty());
  EXPECT_FALSE(idle.matches()) << "and the idle one is evaluated when it is asked";
}

TEST(CssomMediaQuery, AListenerRunsAfterTheChangeIsCompleteMayReadAndMayNotMutate) {
  Cond a(1000), b(500);
  World w([&](GTEXT_CSSOM_Config & cfg) { cfg.conditions = a.c; });
  Mql m(w.d(), "(min-width: 900px)");
  Log log;
  GTEXT_CSS_Status mutation = GTEXT_CSS_OK, scope_mutation = GTEXT_CSS_OK;
  bool seen_matches = true;
  const GTEXT_CSS_Conditions * seen_conditions = nullptr;
  uint64_t generation_inside = 0;
  Sheet s(*w.doc, "p{}");
  Tag t{&log, "L", [&](GTEXT_CSSOM_Media_Query_List * l, bool) {
          mutation = gtext_cssom_sheet_set_disabled(s.s, true, nullptr);
          scope_mutation = gtext_cssom_scope_insert_sheet(gtext_cssom_document_scope(w.d()), s.s, 0, nullptr);
          EXPECT_EQ(gtext_cssom_media_query_list_matches(l, &seen_matches), GTEXT_CSS_OK);
          seen_conditions = gtext_cssom_document_conditions(w.d());
          generation_inside = gtext_cssom_document_generation(w.d());
          return GTEXT_CSS_OK;
        }};
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(m.l, listener, &t), GTEXT_CSS_OK);
  const uint64_t g0 = gtext_cssom_document_generation(w.d());
  ASSERT_EQ(gtext_cssom_document_set_conditions(w.d(), b.c, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(mutation, GTEXT_CSS_E_REENTRANT);
  EXPECT_EQ(scope_mutation, GTEXT_CSS_E_REENTRANT);
  EXPECT_FALSE(seen_matches);
  EXPECT_EQ(seen_conditions, b.c) << "the change is complete when the listener runs";
  EXPECT_EQ(generation_inside, g0 + 1);
  EXPECT_FALSE(gtext_cssom_sheet_disabled(s.s));
  // and the guard is released afterwards
  EXPECT_EQ(gtext_cssom_sheet_set_disabled(s.s, true, nullptr), GTEXT_CSS_OK);
}

TEST(CssomMediaQuery, AListenerThatFailsIsReportedAndDeliveryGoesOn) {
  Cond a(1000), b(500);
  World w([&](GTEXT_CSSOM_Config & cfg) { cfg.conditions = a.c; });
  Mql m1(w.d(), "(min-width: 900px)"), m2(w.d(), "(min-width: 800px)");
  Log log;
  Tag bad{&log, "bad"}, bad2{&log, "bad2"}, good{&log, "good"};
  bad.status = GTEXT_CSS_E_UNSUPPORTED;
  bad2.status = GTEXT_CSS_E_LIMIT_WORK;
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(m1.l, listener, &bad), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(m1.l, listener, &bad2), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(m2.l, listener, &good), GTEXT_CSS_OK);
  GTEXT_CSSOM_Outcome o{};
  ASSERT_EQ(gtext_cssom_document_set_conditions(w.d(), b.c, &o), GTEXT_CSS_OK) << "the change is never undone";
  EXPECT_EQ(o.observer_status, GTEXT_CSS_E_UNSUPPORTED) << "the first failure";
  EXPECT_EQ(log.calls, (std::vector<std::string>{"bad:0", "bad2:0", "good:0"}));
}

TEST(CssomMediaQuery, ListenersAddedOrRemovedDuringDeliveryFollowTheRule) {
  Cond a(1000), b(500), c(1000);
  World w([&](GTEXT_CSSOM_Config & cfg) { cfg.conditions = a.c; });
  Mql m(w.d(), "(min-width: 900px)");
  Log log;
  Tag late{&log, "late"}, second{&log, "second"}, third{&log, "third"};
  Tag first{&log, "first", [&](GTEXT_CSSOM_Media_Query_List * l, bool) {
              EXPECT_EQ(gtext_cssom_media_query_list_remove_listener(l, listener, &second), GTEXT_CSS_OK);
              EXPECT_EQ(gtext_cssom_media_query_list_add_listener(l, listener, &late), GTEXT_CSS_OK);
              return GTEXT_CSS_OK;
            }};
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(m.l, listener, &first), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(m.l, listener, &second), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(m.l, listener, &third), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_document_set_conditions(w.d(), b.c, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(log.calls, (std::vector<std::string>{"first:0", "third:0"})) << "the removed one is not called, the added one is not called for this event";
  EXPECT_EQ(gtext_cssom_media_query_list_listener_count(m.l), 3u);
  log.calls.clear();
  first.extra = nullptr;
  ASSERT_EQ(gtext_cssom_document_set_conditions(w.d(), c.c, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(log.calls, (std::vector<std::string>{"first:1", "third:1", "late:1"}));
}

TEST(CssomMediaQuery, AListIsReleasedFromInsideAListenerWithoutHarm) {
  Cond a(1000), b(500);
  World w([&](GTEXT_CSSOM_Config & cfg) { cfg.conditions = a.c; });
  GTEXT_CSSOM_Media_Query_List * x = nullptr;
  GTEXT_CSSOM_Media_Query_List * y = nullptr;
  ASSERT_EQ(gtext_cssom_match_media(w.d(), "(min-width: 900px)", 18, &x), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_match_media(w.d(), "(min-width: 800px)", 18, &y), GTEXT_CSS_OK);
  Log log;
  Tag tx{&log, "x", [&](GTEXT_CSSOM_Media_Query_List *, bool) {
           gtext_cssom_media_query_list_release(y);   // the other list dies while the report is under way
           y = nullptr;
           return GTEXT_CSS_OK;
         }};
  Tag ty{&log, "y"};
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(x, listener, &tx), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(y, listener, &ty), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_document_set_conditions(w.d(), b.c, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(y, nullptr) << "x's listener released y, the test's only reference";
  EXPECT_EQ(log.calls, (std::vector<std::string>{"x:0", "y:0"})) << "the report holds every list it is going to call";
  gtext_cssom_media_query_list_release(x);
}

/* The rule the observers have, for the listeners: whoever releases the document inside a call, nothing is called after it. The lists are held by their owners, so going on is
   memory-safe, and wrong. */
TEST(CssomMediaQuery, NoListenerIsCalledAfterTheListenerThatReleasedTheDocument) {
  Cond c1000(1000), c800(800);
  World w([&](GTEXT_CSSOM_Config & cfg) { cfg.conditions = c1000.c; });
  Log log;
  Mql a(w.d(), "(min-width: 900px)"), b(w.d(), "(min-width: 950px)");
  Tag first{&log, "A1", [&](GTEXT_CSSOM_Media_Query_List *, bool) {
              w.doc->release();
              return GTEXT_CSS_OK;
            }};
  Tag second{&log, "A2"}, other{&log, "B"};
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(a.l, listener, &first), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(a.l, listener, &second), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(b.l, listener, &other), GTEXT_CSS_OK);
  GTEXT_CSSOM_Outcome o{};
  ASSERT_EQ(gtext_cssom_document_set_conditions(w.d(), c800.c, &o), GTEXT_CSS_OK);
  EXPECT_EQ(log.calls, (std::vector<std::string>{"A1:0"})) << "neither the next listener of the list nor the next list is called";
}

TEST(CssomMediaQuery, NoListenerIsCalledWhenAnObserverOfTheChangeReleasedTheDocument) {
  Cond c1000(1000), c800(800);
  World w([&](GTEXT_CSSOM_Config & cfg) { cfg.conditions = c1000.c; });
  Log log;
  Mql a(w.d(), "(min-width: 900px)");
  Tag listening{&log, "A"};
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(a.l, listener, &listening), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_document_observe(w.d(), [](void * user, const GTEXT_CSSOM_Event * e) {
              if (e->kind == GTEXT_CSSOM_EVENT_CONDITIONS_CHANGED) {
                static_cast<World *>(user)->doc->release();
              }
              return GTEXT_CSS_OK;
            }, &w, nullptr), GTEXT_CSS_OK);
  GTEXT_CSSOM_Outcome o{};
  ASSERT_EQ(gtext_cssom_document_set_conditions(w.d(), c800.c, &o), GTEXT_CSS_OK);
  EXPECT_TRUE(log.calls.empty()) << "the observer ran first and released the document";
}

TEST(CssomMediaQuery, AChangeThatCannotBeEvaluatedLeavesTheDocumentAsItWas) {
  GTEXT_CSS_Conditions * with = conditions_with_metrics(true);
  GTEXT_CSS_Conditions * without = conditions_with_metrics(false);
  {
    World w([&](GTEXT_CSSOM_Config & cfg) { cfg.conditions = with; });
    Mql m(w.d(), "(min-width: 10ex)");
    EXPECT_TRUE(m.matches());
    Log log;
    Tag t{&log, "t"};
    ASSERT_EQ(gtext_cssom_media_query_list_add_listener(m.l, listener, &t), GTEXT_CSS_OK);
    const uint64_t g = gtext_cssom_document_generation(w.d());
    w.doc->events.clear();
    GTEXT_CSSOM_Outcome o{};
    EXPECT_EQ(gtext_cssom_document_set_conditions(w.d(), without, &o), GTEXT_CSS_E_UNSUPPORTED) << "no font metric to answer `ex`";
    EXPECT_EQ(gtext_cssom_document_conditions(w.d()), with);
    EXPECT_EQ(gtext_cssom_document_generation(w.d()), g);
    EXPECT_TRUE(w.doc->events.empty());
    EXPECT_TRUE(log.calls.empty());
    EXPECT_TRUE(m.matches()) << "its answer is the one it had";
  }
  gtext_css_conditions_free(with);
  gtext_css_conditions_free(without);
}

TEST(CssomMediaQuery, TheNumberOfListsIsBoundedAndTheListsOutliveTheDocument) {
  GTEXT_CSSOM_Media_Query_List * kept = nullptr;
  {
    World w([](GTEXT_CSSOM_Config & cfg) { cfg.max_media_query_lists = 2; });
    Mql a(w.d(), "print"), b(w.d(), "screen");
    EXPECT_EQ(Mql(w.d(), "all").status, GTEXT_CSS_E_LIMIT_CSSOM);
    ASSERT_EQ(gtext_cssom_match_media(w.d(), "screen", 6, &kept), GTEXT_CSS_E_LIMIT_CSSOM);
    b.~Mql();
    new (&b) Mql(w.d(), "print");   // the freed slot is reusable
    EXPECT_EQ(b.status, GTEXT_CSS_OK);
    ASSERT_EQ(gtext_cssom_match_media(w.d(), "screen", 6, &kept), GTEXT_CSS_E_LIMIT_CSSOM);
    ASSERT_EQ(gtext_cssom_match_media(w.d(), "(min-width: 1px)", 16, &kept), GTEXT_CSS_E_LIMIT_CSSOM);
  }
  World w2;
  ASSERT_EQ(gtext_cssom_match_media(w2.d(), "screen", 6, &kept), GTEXT_CSS_OK);
  int finalized = 0;
  ASSERT_EQ(gtext_cssom_object_set_user(kept, &finalized, [](void * p, GTEXT_CSSOM_Object_Kind kind) {
              EXPECT_EQ(kind, GTEXT_CSSOM_OBJECT_MEDIA_QUERY_LIST);
              ++*static_cast<int *>(p);
            }), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_object_kind(kept), GTEXT_CSSOM_OBJECT_MEDIA_QUERY_LIST);
  w2.doc->release();
  bool m = false;
  EXPECT_EQ(gtext_cssom_media_query_list_matches(kept, &m), GTEXT_CSS_OK) << "the last answer";
  EXPECT_TRUE(m);
  EXPECT_EQ(finalized, 0);
  gtext_cssom_media_query_list_release(kept);
  EXPECT_EQ(finalized, 1);
}

TEST(CssomMediaQuery, AReferenceTakenToTheConditionsSurvivesTheChangeThatReplacesThem) {
  /* the borrowed pointer is invalid after the change (the header says so); the retained one is the caller's own, and the document's default conditions are the document's alone */
  Cond c800(800);
  UnmapAllocator ua;   // what the document frees is gone from the address space, so a reference that was not taken is seen to be read
  World w(nullptr, &ua.alloc);
  const GTEXT_CSS_Conditions * borrowed = gtext_cssom_document_conditions(w.d());
  GTEXT_CSS_Conditions * kept = gtext_cssom_document_conditions_retained(w.d());
  ASSERT_NE(kept, nullptr);
  EXPECT_EQ(kept, borrowed) << "the same object";
  EXPECT_EQ(gtext_cssom_document_conditions_retained(nullptr), nullptr);
  ASSERT_EQ(gtext_cssom_document_set_conditions(w.d(), c800.c, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_document_conditions(w.d()), c800.c);
  bool supported = false;
  EXPECT_EQ(gtext_css_conditions_supports(kept, "color", "red", &supported), GTEXT_CSS_OK) << "the old object is whole: the document's let-go was not the last";
  EXPECT_TRUE(supported);
  gtext_css_conditions_free(kept);
}

TEST(CssomMediaQuery, SupportsIsAskedOfTheDocumentsConditions) {
  World w;
  bool r = false;
  ASSERT_EQ(gtext_cssom_supports(w.d(), "display", "grid", &r), GTEXT_CSS_OK);
  EXPECT_TRUE(r);
  ASSERT_EQ(gtext_cssom_supports(w.d(), "display", "nonsense", &r), GTEXT_CSS_OK);
  EXPECT_FALSE(r);
  ASSERT_EQ(gtext_cssom_supports(w.d(), "no-such-property", "1", &r), GTEXT_CSS_OK);
  EXPECT_FALSE(r);
  const std::string t1 = "(display: grid) and (color: red)", t2 = "(foo: bar)", t3 = "display: grid";
  ASSERT_EQ(gtext_cssom_supports_text(w.d(), t1.data(), t1.size(), &r), GTEXT_CSS_OK);
  EXPECT_TRUE(r);
  ASSERT_EQ(gtext_cssom_supports_text(w.d(), t2.data(), t2.size(), &r), GTEXT_CSS_OK);
  EXPECT_FALSE(r);
  ASSERT_EQ(gtext_cssom_supports_text(w.d(), t3.data(), t3.size(), &r), GTEXT_CSS_OK) << "a bare declaration is allowed";
  EXPECT_TRUE(r);
  EXPECT_EQ(gtext_cssom_supports(nullptr, "a", "b", &r), GTEXT_CSS_E_INVALID);
  // the policy is the conditions object's: a document made with one that supports nothing
  GTEXT_CSS_Support_Policy policy;
  std::memset(&policy, 0, sizeof policy);
  policy.property_supported = [](void *, GTEXT_CSS_Property_Id) { return false; };
  GTEXT_CSS_Conditions_Options o = gtext_css_conditions_options_default();
  o.policy = &policy;
  GTEXT_CSS_Conditions * c = nullptr;
  ASSERT_EQ(gtext_css_conditions_new(R(), &o, &c), GTEXT_CSS_OK);
  World narrow([&](GTEXT_CSSOM_Config & cfg) { cfg.conditions = c; });
  ASSERT_EQ(gtext_cssom_supports(narrow.d(), "display", "grid", &r), GTEXT_CSS_OK);
  EXPECT_FALSE(r);
  gtext_css_conditions_free(c);
}

}  // namespace
