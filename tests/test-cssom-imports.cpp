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
 * The import callback and the pending state (css_cssom_document.h): when the callback is asked and what it is told, what a sheet it gives does (single owner), a sheet it gives that is
 * refused, `PENDING` and `FAILED`, `gtext_cssom_import_rule_complete()` ending a pending import, `pending_imports`, the mutation guard while the callback runs, the nesting bound, and a
 * callback that fails.
 */

#include <string>

#include <gtest/gtest.h>

#include "cssom_document_test_support.h"

using namespace cod;

namespace {

size_t snapshot_imports(GTEXT_CSSOM_Sheet * s) {
  GTEXT_CSS_Sheet * snap = nullptr;
  EXPECT_EQ(gtext_cssom_sheet_snapshot(s, &snap), GTEXT_CSS_OK);
  const size_t n = snap ? gtext_css_sheet_import_count(snap) : 0;
  gtext_css_sheet_free(snap);
  return n;
}

std::string diag_kinds(GTEXT_CSSOM_Sheet * s) {
  std::string out;
  GTEXT_CSSOM_Parse_Diagnostic d;
  for (size_t i = 0; gtext_cssom_sheet_diagnostic(s, i, &d); i++) {
    out += std::to_string(d.kind) + ",";
  }
  return out;
}

TEST(CssomImports, TheCallbackIsAskedForEveryImportOfASheetMadeFromTextWithWhatTheRuleSays) {
  Importer imp;
  imp.table["a.css"] = "a{}";
  imp.table["b.css"] = "b{}";
  World w([&](GTEXT_CSSOM_Config & c) { imp.install(c); });
  GTEXT_CSSOM_Sheet_Options so = gtext_cssom_sheet_options_default();
  so.base_url = "https://example.test/dir/s.css";
  Sheet s(*w.doc, "@import url(a.css) layer(l) supports(display:grid) screen and (min-width:10px); @import 'b.css'; p{color:red}", &so);
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  ASSERT_EQ(imp.seen.size(), 2u);
  EXPECT_EQ(imp.seen[0].href, "a.css");
  EXPECT_EQ(imp.seen[0].base, "https://example.test/dir/s.css");
  EXPECT_TRUE(imp.seen[0].has_layer);
  EXPECT_EQ(imp.seen[0].layer, "l");
  EXPECT_TRUE(imp.seen[0].has_supports);
  EXPECT_EQ(imp.seen[0].supports, "display:grid");
  EXPECT_EQ(imp.seen[0].media, "screen and (min-width: 10px)");
  EXPECT_EQ(imp.seen[0].sheet, s.s);
  EXPECT_EQ(imp.seen[0].depth, 0u);
  EXPECT_EQ(imp.seen[1].href, "b.css");
  EXPECT_FALSE(imp.seen[1].has_layer);
  EXPECT_FALSE(imp.seen[1].has_supports);
  EXPECT_EQ(imp.seen[1].media, "");
  EXPECT_EQ(w.counters().import_requests, 2u);
  EXPECT_EQ(snapshot_imports(s.s), 2u);
  EXPECT_EQ(gtext_cssom_sheet_pending_imports(s.s), 0u);
}

TEST(CssomImports, ASheetTheCallbackGivesIsOwnedByTheRuleAndTheCallersReferenceIsReleased) {
  FaultAllocator fa;
  {
    Importer imp;
    imp.table["a.css"] = "a{color:red}";
    World w([&](GTEXT_CSSOM_Config & c) { imp.install(c); }, &fa.alloc);
    GTEXT_CSSOM_Sheet * child = nullptr;
    {
      Sheet s(*w.doc, "@import url(a.css);");
      Rule r = at(s.rules(), 0);
      ASSERT_EQ(gtext_cssom_import_rule_sheet(r.r, &child), GTEXT_CSS_OK);
      ASSERT_NE(child, nullptr);
      GTEXT_CSSOM_Rule * owner = nullptr;
      ASSERT_EQ(gtext_cssom_sheet_owner_rule(child, &owner), GTEXT_CSS_OK);
      EXPECT_EQ(owner, r.r) << "ownerRule";
      gtext_cssom_rule_release(owner);
    }
    // a held child keeps its importing rule, and so its sheet, reachable (css_cssom_core.h, keep-alive): the rule is its owner still
    GTEXT_CSSOM_Rule * owner = nullptr;
    ASSERT_EQ(gtext_cssom_sheet_owner_rule(child, &owner), GTEXT_CSS_OK);
    EXPECT_NE(owner, nullptr);
    gtext_cssom_rule_release(owner);
    EXPECT_EQ(gtext_cssom_sheet_rule_total(child), 1u);
    gtext_cssom_sheet_release(child);
  }
  EXPECT_TRUE(fa.live.empty()) << "nothing leaked: the callback's reference was released after the rule took its own";
  EXPECT_EQ(fa.bad_frees, 0u);
}

TEST(CssomImports, APendingImportContributesNothingUntilItIsCompletedAndThenItBumpsTheVersionAndNotifies) {
  Importer imp;
  imp.table["later.css"] = "pending";
  imp.table["never.css"] = "failed";
  World w([&](GTEXT_CSSOM_Config & c) { imp.install(c); });
  Owned host(w, "@import url(later.css); @import url(never.css); p{margin-top:1px}");
  EXPECT_EQ(gtext_cssom_sheet_pending_imports(host.s), 1u) << "the failed one is not pending";
  Rule later = at(host.rules(), 0), never = at(host.rules(), 1);
  EXPECT_TRUE(gtext_cssom_import_rule_pending(later.r));
  EXPECT_FALSE(gtext_cssom_import_rule_pending(never.r));
  EXPECT_EQ(snapshot_imports(host.s), 0u);
  EXPECT_EQ(w.value("#a", "color"), "-");
  Sheet child(*w.doc, "p{color:green}");
  const uint64_t v0 = gtext_cssom_sheet_version(host.s);
  w.doc->events.clear();
  GTEXT_CSSOM_Outcome o{};
  ASSERT_EQ(gtext_cssom_import_rule_complete(later.r, child.s, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_GT(gtext_cssom_sheet_version(host.s), v0);
  ASSERT_EQ(w.doc->events.size(), 1u);
  EXPECT_EQ(w.doc->events[0].kind, GTEXT_CSSOM_EVENT_IMPORT_COMPLETED);
  EXPECT_EQ(gtext_cssom_sheet_pending_imports(host.s), 0u);
  EXPECT_FALSE(gtext_cssom_import_rule_pending(later.r));
  EXPECT_EQ(w.value("#a", "color"), "green");
  // a failure of the load is told the same way, with no sheet
  ASSERT_EQ(gtext_cssom_import_rule_complete(never.r, nullptr, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_UNCHANGED) << "a failed import was answered already";
}

TEST(CssomImports, APendingImportThatFailsLaterIsAChangeAndStillContributesNothing) {
  Importer imp;
  imp.table["later.css"] = "pending";
  World w([&](GTEXT_CSSOM_Config & c) { imp.install(c); });
  Owned host(w, "@import url(later.css); p{margin-top:1px}");
  Rule later = at(host.rules(), 0);
  const uint64_t v0 = gtext_cssom_sheet_version(host.s);
  GTEXT_CSSOM_Outcome o{};
  ASSERT_EQ(gtext_cssom_import_rule_complete(later.r, nullptr, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_CHANGED) << "the count of pending imports fell: a promise that waits on it must hear";
  EXPECT_GT(gtext_cssom_sheet_version(host.s), v0);
  EXPECT_EQ(gtext_cssom_sheet_pending_imports(host.s), 0u);
  EXPECT_EQ(snapshot_imports(host.s), 0u);
  ASSERT_EQ(gtext_cssom_import_rule_complete(later.r, nullptr, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_UNCHANGED);
}

TEST(CssomImports, WithoutACallbackNothingIsAskedAndTheCallerGivesTheSheets) {
  World w;
  Sheet s(*w.doc, "@import url(a.css); p{}");
  EXPECT_EQ(gtext_cssom_sheet_pending_imports(s.s), 0u);
  Rule r = at(s.rules(), 0);
  EXPECT_FALSE(gtext_cssom_import_rule_pending(r.r));
  Sheet child(*w.doc, "q{}");
  ASSERT_EQ(gtext_cssom_import_rule_complete(r.r, child.s, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(snapshot_imports(s.s), 1u);
  EXPECT_EQ(w.counters().import_requests, 0u);
}

TEST(CssomImports, ASheetTheCallbackGivesThatHasAnOwnerIsRefusedWithADiagnosticAndTheImportContributesNothing) {
  Importer imp;
  World w([&](GTEXT_CSSOM_Config & c) { imp.install(c); });
  Sheet shared(*w.doc, "p{color:red}");
  imp.given = shared.s;
  Sheet s(*w.doc, "@import url(x.css); @import url(y.css); q{}");
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  EXPECT_EQ(imp.seen.size(), 2u);
  EXPECT_EQ(snapshot_imports(s.s), 1u) << "the first import took the sheet; the second would be a second owner";
  EXPECT_EQ(gtext_cssom_sheet_diagnostic_total(s.s), 1u);
  EXPECT_EQ(diag_kinds(s.s), std::to_string(GTEXT_CSSOM_PARSE_IMPORT_REFUSED) + ",");
  EXPECT_EQ(w.counters().import_refused, 1u);
  GTEXT_CSSOM_Parse_Diagnostic d;
  ASSERT_TRUE(gtext_cssom_sheet_diagnostic(s.s, 0, &d));
  EXPECT_NE(std::string(d.reference).find("one owner"), std::string::npos);
  Rule second = at(s.rules(), 1);
  GTEXT_CSSOM_Sheet * held = nullptr;
  ASSERT_EQ(gtext_cssom_import_rule_sheet(second.r, &held), GTEXT_CSS_OK);
  EXPECT_EQ(held, nullptr);
}

TEST(CssomImports, ASheetThatWouldBeItsOwnImportIsRefusedAsACycle) {
  Importer imp;
  imp.inside = [&](const GTEXT_CSSOM_Import_Request & req) { imp.given = req.sheet; };
  World w([&](GTEXT_CSSOM_Config & c) { imp.install(c); });
  Sheet s(*w.doc, "@import url(self.css); p{}");
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  imp.given = nullptr;
  EXPECT_EQ(snapshot_imports(s.s), 0u);
  EXPECT_EQ(diag_kinds(s.s), std::to_string(GTEXT_CSSOM_PARSE_IMPORT_REFUSED) + ",");
}

TEST(CssomImports, ASheetThatIsADocumentsOwnerSheetOrConstructedIsRefused) {
  Importer imp;
  World w([&](GTEXT_CSSOM_Config & c) { imp.install(c); });
  Owned listed(w, "p{}");
  Constructed built(*w.doc, "q{}");
  for (GTEXT_CSSOM_Sheet * given : {listed.s, built.s}) {
    imp.given = given;
    Sheet s(*w.doc, "@import url(x.css);");
    EXPECT_EQ(snapshot_imports(s.s), 0u);
    EXPECT_EQ(gtext_cssom_sheet_diagnostic_total(s.s), 1u);
  }
}

TEST(CssomImports, InsertRuleOfAnImportAsksTheCallbackBeforeTheRuleIsInTheSheetAndAttachesWhatComes) {
  Importer imp;
  imp.table["a.css"] = "a{color:red}";
  World w([&](GTEXT_CSSOM_Config & c) { imp.install(c); });
  Sheet s(*w.doc, "p{}");
  Sheet empty(*w.doc, "");
  w.doc->events.clear();
  bool linked = true;
  imp.inside = [&](const GTEXT_CSSOM_Import_Request & req) { linked = gtext_cssom_rule_parent_sheet(req.rule) != nullptr; };
  Ins r = insert_rule(empty.s, "@import url(a.css)", 0);
  ASSERT_EQ(r.st, GTEXT_CSS_OK);
  EXPECT_FALSE(linked) << "the rule is not in the sheet while the callback runs";
  ASSERT_EQ(imp.seen.size(), 1u);
  EXPECT_EQ(imp.seen[0].rule, r.rule.r);
  EXPECT_EQ(snapshot_imports(empty.s), 1u);
  ASSERT_EQ(w.doc->events.size(), 1u) << "one RULE_INSERTED: the sheet came with the rule";
  EXPECT_EQ(w.doc->events[0].kind, GTEXT_CSSOM_EVENT_RULE_INSERTED);
  // a position the rule cannot be at is refused before anyone is asked
  imp.seen.clear();
  EXPECT_EQ(insert_rule(s.s, "@import url(a.css)", 1).st, GTEXT_CSS_E_HIERARCHY);
  EXPECT_EQ(imp.seen.size(), 0u);
  // pending on insert
  imp.table["p.css"] = "pending";
  Ins p = insert_rule(empty.s, "@import url(p.css)", 1);
  ASSERT_EQ(p.st, GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_sheet_pending_imports(empty.s), 1u);
  EXPECT_TRUE(gtext_cssom_import_rule_pending(p.rule.r));
}

TEST(CssomImports, AFailingCallbackFailsTheInsertionAndLeavesTheSheetAsItWas) {
  Importer imp;
  FaultAllocator fa;
  {
    World w([&](GTEXT_CSSOM_Config & c) { imp.install(c); }, &fa.alloc);
    Sheet s(*w.doc, "p{}");
    imp.table["a.css"] = "a{}";
    const std::string before = dump(s.s);
    const uint64_t id = w.doc->core()->next_id;
    const uint64_t generation = gtext_cssom_document_generation(w.d());
    w.doc->events.clear();
    imp.fail = GTEXT_CSS_E_UNSUPPORTED;
    Ins r = insert_rule(s.s, "@import url(a.css)", 0);
    EXPECT_EQ(r.st, GTEXT_CSS_E_UNSUPPORTED) << "the callback's status";
    EXPECT_EQ(dump(s.s), before);
    EXPECT_EQ(w.doc->core()->next_id, id);
    EXPECT_EQ(gtext_cssom_document_generation(w.d()), generation);
    EXPECT_TRUE(w.doc->events.empty());
    // a callback that fails while a sheet is made drops the sheet
    GTEXT_CSSOM_Sheet * made = nullptr;
    EXPECT_EQ(gtext_cssom_sheet_new_parsed(w.d(), "@import url(a.css);", 19, nullptr, &made), GTEXT_CSS_E_UNSUPPORTED);
    EXPECT_EQ(made, nullptr);
  }
  EXPECT_TRUE(fa.live.empty());
}

TEST(CssomImports, AMutationFromInsideTheCallbackIsReentrantButMakingASheetIsNot) {
  Importer imp;
  GTEXT_CSS_Status mutation = GTEXT_CSS_OK, made = GTEXT_CSS_E_INTERNAL;
  World w([&](GTEXT_CSSOM_Config & c) { imp.install(c); });
  Sheet other(*w.doc, "z{}");
  imp.inside = [&](const GTEXT_CSSOM_Import_Request & req) {
    mutation = gtext_cssom_sheet_insert_rule(other.s, "y{}", 3, 0, nullptr, nullptr);
    GTEXT_CSSOM_Sheet * s = nullptr;
    made = gtext_cssom_sheet_new_parsed(gtext_cssom_sheet_document(req.sheet), "k{}", 3, nullptr, &s);
    gtext_cssom_sheet_release(s);
  };
  Sheet s(*w.doc, "@import url(a.css);");
  EXPECT_EQ(mutation, GTEXT_CSS_E_REENTRANT);
  EXPECT_EQ(made, GTEXT_CSS_OK);
  EXPECT_EQ(shape(other), "style");
}

TEST(CssomImports, ACallbackThatMakesTheImportedSheetIsAskedForItsImportsAndTheNestingIsBounded) {
  Importer imp;
  imp.table["a.css"] = "@import url(b.css); a{}";
  imp.table["b.css"] = "@import url(c.css); b{}";
  imp.table["c.css"] = "c{}";
  World w([&](GTEXT_CSSOM_Config & c) { imp.install(c); });
  {
    Sheet s(*w.doc, "@import url(a.css);");
    ASSERT_EQ(imp.seen.size(), 3u);
    EXPECT_EQ(imp.seen[0].depth, 0u);
    EXPECT_EQ(imp.seen[1].depth, 1u);
    EXPECT_EQ(imp.seen[2].depth, 2u);
    EXPECT_EQ(snapshot_imports(s.s), 1u);
  }
  // an import that asks for itself forever: the library stops asking at 64 levels, and the deepest sheet says so
  Importer loop;
  loop.table["x.css"] = "@import url(x.css); x{}";
  World v([&](GTEXT_CSSOM_Config & c) { loop.install(c); });
  Sheet top(*v.doc, "@import url(x.css);");
  ASSERT_EQ(top.status, GTEXT_CSS_OK);
  EXPECT_EQ(loop.seen.size(), 64u);
  EXPECT_EQ(v.counters().import_refused, 1u);
  EXPECT_EQ(v.counters().import_requests, 64u);
}

TEST(CssomImports, AConstructedSheetDropsItsImportsAndAsksNobody) {
  Importer imp;
  World w([&](GTEXT_CSSOM_Config & c) { imp.install(c); });
  Constructed k(*w.doc);
  const std::string text = "@import url(a.css); p{color:red}";
  ASSERT_EQ(gtext_cssom_sheet_replace_sync(k.s, text.data(), text.size(), nullptr), GTEXT_CSS_OK);
  const GTEXT_CSSOM_Rule_List * rules = nullptr;
  ASSERT_EQ(gtext_cssom_sheet_rules(k.s, &rules), GTEXT_CSS_OK);
  EXPECT_EQ(shape(rules), "style");
  EXPECT_EQ(imp.seen.size(), 0u);
  EXPECT_EQ(insert_rule(k.s, "@import url(b.css)", 0).st, GTEXT_CSS_E_SYNTAX) << "and insertRule of an import into a constructed sheet is a SyntaxError, before anyone is asked";
  EXPECT_EQ(imp.seen.size(), 0u);
}

}  // namespace
