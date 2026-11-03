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
 * Several documents on one registry (css_cssom_document.h): a document is one tree world with its own sheets, scopes,
 * conditions, caches and generation, and documents share only the registry and conditions objects, which are read-only. Interleaved mutations of two documents end where the same mutations of
 * each alone end; the same text under different conditions has equal sheets and different indexes; what one document hands another is refused where it must be and never accepted silently;
 * releasing one document leaves the other whole; and two documents on two threads are independent.
 */

#include <atomic>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "cssom_document_test_support.h"

using namespace cod;

namespace {

/// A deterministic mutation script over a world: `step` of it. Everything is a function of the step number, so two worlds that run the same steps are the same world.
struct Script {
  World & w;
  std::vector<std::unique_ptr<Owned>> owned;
  std::vector<std::unique_ptr<Constructed>> built;
  explicit Script(World & world) : w(world) {
    owned.push_back(std::make_unique<Owned>(w, "p{color:red} span{margin-top:1px} @media (min-width: 1000px){ p{padding-top:3px} }"));
    owned.push_back(std::make_unique<Owned>(w, "div{color:green} @layer a { span{color:blue} }"));
    built.push_back(std::make_unique<Constructed>(*w.doc, "p{margin-top:9px}"));
    EXPECT_EQ(gtext_cssom_scope_adopted_insert(gtext_cssom_document_scope(w.d()), built[0]->s, 0, nullptr), GTEXT_CSS_OK);
  }
  void step(unsigned i) {
    switch (i % 7) {
    case 0: insert_rule(owned[i % 2]->s, "p.k" + std::to_string(i) + "{color:blue}", 0); break;
    case 1: {
      const GTEXT_CSSOM_Rule_List * l = owned[i % 2]->rules();
      if (gtext_cssom_rule_list_length(l) > 2) delete_rule(owned[i % 2]->s, gtext_cssom_rule_list_length(l) - 1);
      break;
    }
    case 2: {
      Rule r = at(owned[0]->rules(), 0);
      if (r && gtext_cssom_rule_kind(r.r) == GTEXT_CSSOM_RULE_STYLE) {
        Decl d = style_of(r.r);
        set(d.d, "color", i % 2 ? "orange" : "purple");
      }
      break;
    }
    case 3: gtext_cssom_sheet_set_disabled(owned[1]->s, (i / 7) % 2 == 0, nullptr); break;
    case 4: insert_rule(built[0]->s, "p{padding-top:" + std::to_string(i % 5) + "px}", 0); break;
    case 5: w.index(); break;
    case 6: {
      Cond c(i % 2 ? 800 : 1400);
      gtext_cssom_document_set_conditions(w.d(), c.c, nullptr);
      break;
    }
    }
  }
  std::string summary() {
    std::string s = "g" + std::to_string(gtext_cssom_document_generation(w.d()));
    for (auto & o : owned) s += "|" + dump(o->s);
    for (auto & b : built) s += "|" + dump(b->s);
    const GTEXT_CSS_Rule_Index * x = w.index();
    s += "|records=" + std::to_string(gtext_css_rule_index_record_count(x)) + "|" + w.value("#a", "color") + "," + w.value("#a", "padding-top") + "," + w.value("#a", "margin-top") + "," +
         w.value("#s", "color");
    return s;
  }
};

}  // namespace

TEST(CssomMultidoc, InterleavedMutationsOfTwoDocumentsEndWhereEachEndsAlone) {
  std::string alone_a, alone_b;
  {
    World a;
    Script sa(a);
    for (unsigned i = 0; i < 60; i++) sa.step(i);
    alone_a = sa.summary();
  }
  {
    World b;
    Script sb(b);
    for (unsigned i = 0; i < 60; i++) sb.step(i * 3 + 1);
    alone_b = sb.summary();
  }
  ASSERT_NE(alone_a, alone_b);
  World a, b;
  Script sa(a), sb(b);
  for (unsigned i = 0; i < 60; i++) {
    sa.step(i);
    sb.step(i * 3 + 1);
  }
  EXPECT_EQ(sa.summary(), alone_a);
  EXPECT_EQ(sb.summary(), alone_b);
}

TEST(CssomMultidoc, TheSameTextUnderDifferentConditionsHasEqualSheetsAndDifferentIndexes) {
  Cond narrow(500), wide(1500);
  World a([&](GTEXT_CSSOM_Config & c) { c.conditions = narrow.c; });
  World b([&](GTEXT_CSSOM_Config & c) { c.conditions = wide.c; });
  const std::string text = "p{color:red} @media (min-width: 1000px){ p{margin-top:3px} } @media (max-width: 800px){ span{margin-top:7px} }";
  Owned sa(a, text), sb(b, text);
  EXPECT_EQ(dump(sa.s), dump(sb.s)) << "the live sheets do not depend on the environment";
  const GTEXT_CSS_Rule_Index * ia = a.index();
  const GTEXT_CSS_Rule_Index * ib = b.index();
  EXPECT_EQ(gtext_css_rule_index_record_count(ia), 2u);
  EXPECT_EQ(gtext_css_rule_index_record_count(ib), 2u);
  EXPECT_EQ(a.value("#a", "margin-top"), "-");
  EXPECT_EQ(b.value("#a", "margin-top"), "3px");
  EXPECT_EQ(a.value("#s", "margin-top"), "7px");
  EXPECT_EQ(b.value("#s", "margin-top"), "-");
  // the compiled sheets are the same text compiled twice, and equal
  GTEXT_CSS_Sheet *ca = nullptr, *cb = nullptr;
  ASSERT_EQ(gtext_cssom_sheet_snapshot(sa.s, &ca), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_sheet_snapshot(sb.s, &cb), GTEXT_CSS_OK);
  std::string da, db;
  GTEXT_Markup_Sink sink;
  std::memset(&sink, 0, sizeof sink);
  sink.write = [](void * u, const char * p, size_t n) {
    static_cast<std::string *>(u)->append(p, n);
    return GTEXT_MARKUP_OK;
  };
  sink.user = &da;
  ASSERT_EQ(gtext_css_sheet_dump(ca, &sink), GTEXT_CSS_OK);
  sink.user = &db;
  ASSERT_EQ(gtext_css_sheet_dump(cb, &sink), GTEXT_CSS_OK);
  EXPECT_EQ(da, db);
  gtext_css_sheet_free(ca);
  gtext_css_sheet_free(cb);
}

TEST(CssomMultidoc, WhatOneDocumentHandsAnotherIsRefusedWhereItMustBeAndNeverAcceptedSilently) {
  World a, b;
  Sheet sa(*a.doc, "p{}");
  Constructed ka(*a.doc, "p{color:red}");
  Compiled ua("p{}");
  GTEXT_CSSOM_Scope * scope_b = gtext_cssom_document_scope(b.d());
  EXPECT_EQ(gtext_cssom_scope_insert_sheet(scope_b, sa.s, 0, nullptr), GTEXT_CSS_E_INVALID) << "a sheet of A in B's list";
  EXPECT_EQ(gtext_cssom_scope_adopted_insert(scope_b, ka.s, 0, nullptr), GTEXT_CSS_E_NOT_ALLOWED) << "the constructor-document check";
  Sheet import_host(*b.doc, "@import url(x.css);");
  Rule imp = at(import_host.rules(), 0);
  EXPECT_EQ(gtext_cssom_import_rule_complete(imp.r, sa.s, nullptr), GTEXT_CSS_E_INVALID) << "a sheet of A as the import of a rule of B";
  GTEXT_CSSOM_Scope_Options o = gtext_cssom_scope_options_default();
  int h = 0;
  o.handle = &h;
  o.shadow = true;
  o.parent = gtext_cssom_document_scope(a.d());
  GTEXT_CSSOM_Scope * s = nullptr;
  EXPECT_EQ(gtext_cssom_scope_new(b.d(), &o, &s), GTEXT_CSS_E_INVALID) << "A's scope as the parent of B's";
  EXPECT_EQ(gtext_cssom_scope_reparent(gtext_cssom_document_scope(b.d()), a.d(), nullptr, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_document_register_sheet(b.d(), GTEXT_CSS_ORIGIN_USER, ua.s, nullptr), GTEXT_CSS_OK) << "a compiled sheet belongs to no document, and one may be registered with several";
  EXPECT_EQ(gtext_cssom_document_register_sheet(a.d(), GTEXT_CSS_ORIGIN_USER, ua.s, nullptr), GTEXT_CSS_OK);
  // none of it changed A or B
  EXPECT_EQ(gtext_cssom_scope_sheet_count(scope_b), 0u);
  EXPECT_EQ(gtext_cssom_scope_adopted_count(scope_b), 0u);
}

TEST(CssomMultidoc, ReleasingOneDocumentLeavesTheOtherWholeAndFreesWhatIsOnlyTheFirstOnes) {
  FaultAllocator fa;
  {
    World b;
    int h = 0;
    {
      World a([](GTEXT_CSSOM_Config &) {}, &fa.alloc);
      GTEXT_CSSOM_Scope_Options o = gtext_cssom_scope_options_default();
      o.handle = &h;
      o.shadow = true;
      GTEXT_CSSOM_Scope * scope = nullptr;
      ASSERT_EQ(gtext_cssom_scope_new(a.d(), &o, &scope), GTEXT_CSS_OK);
      Constructed k(*a.doc, "p{color:red}");
      ASSERT_EQ(gtext_cssom_scope_adopted_insert(scope, k.s, 0, nullptr), GTEXT_CSS_OK);
      Owned owner(a, "q{}");
      ASSERT_NE(a.index(), nullptr);
      gtext_cssom_scope_release(scope);
    }
    EXPECT_TRUE(fa.live.empty()) << "A's scopes, sheets, index and conditions went with A";
    Owned sb(b, "p{color:blue}");
    EXPECT_EQ(b.value("#a", "color"), "blue");
  }
  EXPECT_EQ(fa.bad_frees, 0u);
}

TEST(CssomMultidoc, TwoDocumentsOnTwoThreadsAreIndependentAndShareOnlyTheRegistryAndTheConditions) {
  Cond shared(1000);
  std::string alone[2];
  for (int t = 0; t < 2; t++) {
    World w([&](GTEXT_CSSOM_Config & c) { c.conditions = shared.c; });
    Script s(w);
    for (unsigned i = 0; i < 80; i++) s.step(i * (t + 1) + t);
    alone[t] = s.summary();
  }
  std::string result[2];
  std::atomic<int> started{0};
  auto run = [&](int t) {
    World w([&](GTEXT_CSSOM_Config & c) { c.conditions = shared.c; });
    Script s(w);
    started++;
    while (started.load() < 2) {
      std::this_thread::yield();
    }
    for (unsigned i = 0; i < 80; i++) s.step(i * (t + 1) + t);
    result[t] = s.summary();
  };
  std::thread t0(run, 0), t1(run, 1);
  t0.join();
  t1.join();
  EXPECT_EQ(result[0], alone[0]);
  EXPECT_EQ(result[1], alone[1]);
}
