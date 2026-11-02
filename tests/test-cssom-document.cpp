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
 * The CSSOM document as the cascade sees it (css_cssom_document.h): the index and the tuple it is cached on, the sheets that take part and those that do not, `styleSheets`, adopted
 * sheets (the constructor-document check, the last position), tree scopes and their registrations, the user-agent and user sheets, and the environment change leaving the compiled
 * sheets as they were. Each test says what it asserts in its name; the sentences of the standards that decide a case are quoted where they do.
 */

#include <string>

#include <gtest/gtest.h>

#include "cssom_document_test_support.h"

using namespace cod;

namespace {

GTEXT_CSS_Sheet * snapshot_of(GTEXT_CSSOM_Sheet * s) {
  GTEXT_CSS_Sheet * snap = nullptr;
  EXPECT_EQ(gtext_cssom_sheet_snapshot(s, &snap), GTEXT_CSS_OK);
  gtext_css_sheet_free(snap);   // the sheet's cache holds it; the pointer is the identity
  return snap;
}

GTEXT_CSS_Status adopt(GTEXT_CSSOM_Scope * scope, GTEXT_CSSOM_Sheet * sheet, size_t index) {
  return gtext_cssom_scope_adopted_insert(scope, sheet, index, nullptr);
}

/* ---- the index --------------------------------------------------------------------------------------------------------------------------------- */

TEST(CssomDocument, ADocumentWithNothingInItHasNoObjectsAndItsOwnScopeTakesNoId) {
  Doc doc;
  const GTEXT_CSSOM_Counters c = [&] {
    GTEXT_CSSOM_Counters x;
    EXPECT_EQ(gtext_cssom_document_counters(doc.d, &x), GTEXT_CSS_OK);
    return x;
  }();
  EXPECT_EQ(c.live_objects, 0u);
  EXPECT_EQ(c.live_bytes, 0u);
  EXPECT_EQ(c.live_scopes, 1u) << "the document's scope exists and is no object of the counters";
  EXPECT_EQ(doc.core()->next_id, 1u) << "and the first thing made gets the first id";
  EXPECT_EQ(gtext_cssom_scope_id(gtext_cssom_document_scope(doc.d)), 0u);
}

TEST(CssomDocument, AnEmptyDocumentHasAnEmptyIndexThatIsTheSameObjectWhileNothingChanges) {
  World w;
  const GTEXT_CSS_Rule_Index * a = w.index();
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(gtext_css_rule_index_record_count(a), 0u);
  EXPECT_EQ(gtext_css_rule_index_sheet_count(a), 0u);
  EXPECT_EQ(w.index(), a);
  EXPECT_EQ(w.counters().index_builds, 1u);
  EXPECT_EQ(w.counters().index_hits, 1u);
  EXPECT_EQ(w.counters().live_scopes, 1u) << "the document's own scope";
}

TEST(CssomDocument, AuthorSheetsTakePartInTheOrderOfStyleSheetsAndTheLastOneWins) {
  World w;
  Owned a(w, "p{color:red; margin-top:1px}"), b(w, "p{color:blue}");
  EXPECT_EQ(w.value("#a", "color"), "blue");
  EXPECT_EQ(w.value("#a", "margin-top"), "1px");
  GTEXT_CSSOM_Scope * scope = gtext_cssom_document_scope(w.d());
  EXPECT_EQ(gtext_cssom_scope_sheet_count(scope), 2u);
  // CSSOM: `document.styleSheets` is the order of the owner nodes: moving the second one before the first makes the first win
  ASSERT_EQ(gtext_cssom_scope_remove_sheet(scope, b.s, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_scope_insert_sheet(scope, b.s, 0, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(w.value("#a", "color"), "red");
}

TEST(CssomDocument, UserAgentAndUserSheetsAreRegisteredAndComeBeforeTheAuthorsAndLoseToThem) {
  World w;
  Compiled ua("p{color:green; margin-top:1px; padding-top:7px}"), user("p{margin-top:2px; color:orange}");
  GTEXT_CSSOM_Outcome o{};
  ASSERT_EQ(gtext_cssom_document_register_sheet(w.d(), GTEXT_CSS_ORIGIN_USER, user.s, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_CHANGED);
  ASSERT_EQ(gtext_cssom_document_register_sheet(w.d(), GTEXT_CSS_ORIGIN_USER_AGENT, ua.s, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(w.value("#a", "padding-top"), "7px") << "only the user agent has it";
  EXPECT_EQ(w.value("#a", "margin-top"), "2px") << "the user's origin beats the user agent's";
  EXPECT_EQ(w.value("#a", "color"), "orange");
  Owned author(w, "p{color:red}");
  EXPECT_EQ(w.value("#a", "color"), "red") << "the author's origin beats both";
  EXPECT_EQ(gtext_cssom_document_register_sheet(w.d(), GTEXT_CSS_ORIGIN_USER, user.s, nullptr), GTEXT_CSS_E_INVALID_STATE) << "registered already";
  EXPECT_EQ(gtext_cssom_document_register_sheet(w.d(), GTEXT_CSS_ORIGIN_AUTHOR, user.s, nullptr), GTEXT_CSS_E_INVALID) << "the author's sheets are the document's own";
  ASSERT_EQ(gtext_cssom_document_unregister_sheet(w.d(), user.s, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(w.value("#a", "margin-top"), "1px");
  EXPECT_EQ(gtext_cssom_document_unregister_sheet(w.d(), user.s, nullptr), GTEXT_CSS_E_NOT_FOUND);
}

TEST(CssomDocument, ADisabledSheetDoesNotTakePartAndTakesPartAgainWhenItIsSwitchedOn) {
  World w;
  Owned a(w, "p{color:red}"), b(w, "p{color:blue}");
  EXPECT_EQ(w.value("#a", "color"), "blue");
  const GTEXT_CSS_Rule_Index * before = w.index();
  ASSERT_EQ(gtext_cssom_sheet_set_disabled(b.s, true, nullptr), GTEXT_CSS_OK);
  const GTEXT_CSS_Rule_Index * off = w.index();
  EXPECT_NE(off, before);
  EXPECT_EQ(gtext_css_rule_index_sheet_count(off), 1u);
  EXPECT_EQ(w.value("#a", "color"), "red");
  ASSERT_EQ(gtext_cssom_sheet_set_disabled(b.s, false, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(w.value("#a", "color"), "blue");
}

TEST(CssomDocument, ASheetWhoseMediaIsFalseForTheConditionsDoesNotTakePartAndTheEnvironmentChangeChangesThat) {
  Cond narrow(800);
  World w([&](GTEXT_CSSOM_Config & c) { c.conditions = narrow.c; });
  GTEXT_CSSOM_Sheet_Options so = gtext_cssom_sheet_options_default();
  so.media = "(min-width: 1000px)";
  Owned wide(w, "p{color:red}", &so);
  Owned always(w, "p{margin-top:3px}");
  EXPECT_EQ(w.value("#a", "color"), "-");
  EXPECT_EQ(w.value("#a", "margin-top"), "3px");
  const uint64_t compiles = w.counters().snapshot_compiles;
  GTEXT_CSS_Sheet * s1 = snapshot_of(wide.s);
  GTEXT_CSS_Sheet * s2 = snapshot_of(always.s);
  Cond big(1200);
  GTEXT_CSSOM_Outcome o{};
  ASSERT_EQ(gtext_cssom_document_set_conditions(w.d(), big.c, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(w.value("#a", "color"), "red");
  // "the environment change leaves sheets pointer-equal": a resize recompiles nothing
  EXPECT_EQ(snapshot_of(wide.s), s1);
  EXPECT_EQ(snapshot_of(always.s), s2);
  EXPECT_EQ(w.counters().snapshot_compiles, compiles + 1) << "the wide sheet was compiled once, when it first took part";
  EXPECT_EQ(gtext_cssom_document_conditions(w.d()), big.c);
  // and the same object again is nothing
  ASSERT_EQ(gtext_cssom_document_set_conditions(w.d(), big.c, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_UNCHANGED);
}

TEST(CssomDocument, TheMediaOfASheetIsSetAfterwardsAndTheIndexFollows) {
  World w;
  Owned a(w, "p{color:red}");
  EXPECT_EQ(w.value("#a", "color"), "red");
  GTEXT_CSSOM_Media_List * list = nullptr;
  ASSERT_EQ(gtext_cssom_sheet_media(a.s, &list), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_media_set_text(list, "print", 5, nullptr), GTEXT_CSS_OK);
  gtext_cssom_media_release(list);
  EXPECT_EQ(w.value("#a", "color"), "-") << "a screen document is not print";
}

TEST(CssomDocument, TheIndexIsCachedOnItsInputsAndOnNothingElse) {
  World w;
  Owned a(w, "p{color:red} @media print { p { margin-top:1px } }");
  Owned b(w, "span{color:blue}");
  const GTEXT_CSS_Rule_Index * x = w.index();
  const uint64_t builds = w.counters().index_builds;
  // reads change nothing
  GTEXT_CSS_Sheet * snap = nullptr;
  ASSERT_EQ(gtext_cssom_sheet_snapshot(a.s, &snap), GTEXT_CSS_OK);
  gtext_css_sheet_free(snap);
  const GTEXT_CSSOM_Rule_List * rules = a.rules();
  Rule r = at(rules, 0);
  EXPECT_FALSE(css(r.r).empty());
  EXPECT_EQ(w.index(), x);
  EXPECT_EQ(w.counters().index_builds, builds);
  // each input, once
  ASSERT_EQ(insert_rule(a.s, "i{color:green}", 0).st, GTEXT_CSS_OK);
  const GTEXT_CSS_Rule_Index * y = w.index();
  EXPECT_NE(y, x) << "a rule inserted into a sheet that takes part";
  Decl d = style_of(path(a, {1}).r);
  ASSERT_EQ(set(d.d, "color", "orange").o.kind, GTEXT_CSSOM_CHANGED);
  const GTEXT_CSS_Rule_Index * z = w.index();
  EXPECT_NE(z, y) << "a declaration of a rule of it";
  EXPECT_EQ(w.counters().index_builds, builds + 2);
  Compiled ua("div{color:red}");
  ASSERT_EQ(gtext_cssom_document_register_sheet(w.d(), GTEXT_CSS_ORIGIN_USER_AGENT, ua.s, nullptr), GTEXT_CSS_OK);
  const GTEXT_CSS_Rule_Index * u = w.index();
  EXPECT_NE(u, z) << "the registration version";
  ASSERT_EQ(gtext_cssom_scope_remove_sheet(gtext_cssom_document_scope(w.d()), b.s, nullptr), GTEXT_CSS_OK);
  EXPECT_NE(w.index(), u) << "the structure version";
  EXPECT_EQ(w.counters().index_builds, builds + 4);
}

TEST(CssomDocument, ManyMutationsAndOneAskAreOneBuild) {
  World w;
  Owned a(w, "p{color:red}");
  w.index();
  const GTEXT_CSSOM_Counters c0 = w.counters();
  for (int i = 0; i < 200; i++) {
    ASSERT_EQ(insert_rule(a.s, "q" + std::to_string(i) + "{color:blue}", 0).st, GTEXT_CSS_OK);
  }
  EXPECT_EQ(w.counters().index_builds, c0.index_builds) << "nothing is built until the next ask";
  w.index();
  w.index();
  const GTEXT_CSSOM_Counters c1 = w.counters();
  EXPECT_EQ(c1.index_builds, c0.index_builds + 1);
  EXPECT_EQ(c1.snapshot_compiles, c0.snapshot_compiles + 1) << "and one compile of the sheet";
}

TEST(CssomDocument, AChangeInAnImportedSheetChangesTheIndexOfTheDocument) {
  Importer imp;
  imp.table["child.css"] = "p{color:green}";
  World w([&](GTEXT_CSSOM_Config & c) { imp.install(c); });
  Owned host(w, "@import url(child.css); span{color:red}");
  EXPECT_EQ(w.value("#a", "color"), "green") << "the callback made the sheet and the rule holds it";
  GTEXT_CSSOM_Sheet * child = nullptr;
  Rule imp_rule = at(host.rules(), 0);
  ASSERT_EQ(gtext_cssom_import_rule_sheet(imp_rule.r, &child), GTEXT_CSS_OK);
  ASSERT_NE(child, nullptr);
  const GTEXT_CSS_Rule_Index * before = w.index();
  ASSERT_EQ(insert_rule(child, "p{color:blue}", 1).st, GTEXT_CSS_OK);
  EXPECT_NE(w.index(), before) << "the composite version of the importer moved";
  EXPECT_EQ(w.value("#a", "color"), "blue");
  gtext_cssom_sheet_release(child);
}

/* ---- styleSheets --------------------------------------------------------------------------------------------------------------------------- */

TEST(CssomDocument, StyleSheetsHoldsTheOwnerSheetsInTheOrderGivenAndRefusesWhatIsNotOne) {
  World w;
  GTEXT_CSSOM_Scope * scope = gtext_cssom_document_scope(w.d());
  Sheet a(*w.doc, "a{}"), b(*w.doc, "b{}"), c(*w.doc, "c{}");
  EXPECT_EQ(gtext_cssom_scope_insert_sheet(scope, a.s, 1, nullptr), GTEXT_CSS_E_INDEX_SIZE);
  ASSERT_EQ(gtext_cssom_scope_insert_sheet(scope, a.s, 0, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_scope_insert_sheet(scope, c.s, 1, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_scope_insert_sheet(scope, b.s, 1, nullptr), GTEXT_CSS_OK);
  GTEXT_CSSOM_Sheet * s1 = gtext_cssom_scope_sheet_at(scope, 1);
  EXPECT_EQ(s1, b.s);
  gtext_cssom_sheet_release(s1);
  EXPECT_EQ(gtext_cssom_scope_sheet_at(scope, 3), nullptr);
  EXPECT_EQ(gtext_cssom_scope_insert_sheet(scope, a.s, 0, nullptr), GTEXT_CSS_E_INVALID_STATE) << "a sheet is in one list";
  Constructed k(*w.doc);
  EXPECT_EQ(gtext_cssom_scope_insert_sheet(scope, k.s, 0, nullptr), GTEXT_CSS_E_INVALID_STATE) << "a constructed sheet is adopted, it is not an owner sheet";
  World other;
  Sheet foreign(*other.doc, "f{}");
  EXPECT_EQ(gtext_cssom_scope_insert_sheet(scope, foreign.s, 0, nullptr), GTEXT_CSS_E_INVALID) << "a sheet of another document";
  EXPECT_EQ(gtext_cssom_scope_remove_sheet(scope, foreign.s, nullptr), GTEXT_CSS_E_NOT_FOUND);
  EXPECT_EQ(gtext_cssom_scope_insert_sheet(nullptr, a.s, 0, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_scope_insert_sheet(scope, nullptr, 0, nullptr), GTEXT_CSS_E_INVALID);
}

TEST(CssomDocument, ASheetWhoseOwnerIsRemovedStaysAValidObjectWithNoOwnerNodeAndLeavesTheCascade) {
  // WPT CSSStyleSheet-modify-after-removal.html: insertRule on a sheet whose owner left works and affects nothing
  World w;
  int owner_token = 0;
  GTEXT_CSSOM_Sheet_Options so = gtext_cssom_sheet_options_default();
  so.owner_node = &owner_token;
  Sheet a(*w.doc, "p{color:red}", &so);
  GTEXT_CSSOM_Scope * scope = gtext_cssom_document_scope(w.d());
  ASSERT_EQ(gtext_cssom_scope_insert_sheet(scope, a.s, 0, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(w.value("#a", "color"), "red");
  EXPECT_EQ(gtext_cssom_sheet_owner_node(a.s), &owner_token);
  w.doc->events.clear();
  GTEXT_CSSOM_Outcome o{};
  ASSERT_EQ(gtext_cssom_scope_remove_sheet(scope, a.s, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_CHANGED);
  ASSERT_EQ(w.doc->events.size(), 1u);
  EXPECT_EQ(w.doc->events[0].kind, GTEXT_CSSOM_EVENT_SHEET_REMOVED);
  EXPECT_EQ(gtext_cssom_sheet_owner_node(a.s), nullptr);
  EXPECT_EQ(w.value("#a", "color"), "-");
  ASSERT_EQ(insert_rule(a.s, "p{margin-top:9px}", 0).st, GTEXT_CSS_OK) << "it is still a mutable object";
  EXPECT_EQ(w.value("#a", "margin-top"), "-") << "and affects nothing";
  EXPECT_EQ(shape(a), "style,style");
}

TEST(CssomDocument, ASheetHeldOnlyByItsScopeStaysAliveAndTakesPart) {
  World w;
  GTEXT_CSSOM_Scope * scope = gtext_cssom_document_scope(w.d());
  GTEXT_CSSOM_Sheet * s = nullptr;
  const std::string text = "p{color:red}";
  ASSERT_EQ(gtext_cssom_sheet_new_parsed(w.d(), text.data(), text.size(), nullptr, &s), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_scope_insert_sheet(scope, s, 0, nullptr), GTEXT_CSS_OK);
  gtext_cssom_sheet_release(s);
  EXPECT_EQ(w.counters().live_objects, 2u) << "the sheet and its rule";
  EXPECT_EQ(w.value("#a", "color"), "red");
  GTEXT_CSSOM_Sheet * again = gtext_cssom_scope_sheet_at(scope, 0);
  ASSERT_EQ(again, s);
  ASSERT_EQ(gtext_cssom_scope_remove_sheet(scope, again, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(w.counters().live_objects, 2u) << "taken out, it is held by the caller's reference";
  gtext_cssom_sheet_release(again);
  EXPECT_EQ(w.counters().live_objects, 0u);
}

TEST(CssomDocument, AnEventIsSentForEverySheetListChange) {
  World w;
  Sheet a(*w.doc, "a{}");
  GTEXT_CSSOM_Scope * scope = gtext_cssom_document_scope(w.d());
  ASSERT_EQ(gtext_cssom_scope_insert_sheet(scope, a.s, 0, nullptr), GTEXT_CSS_OK);
  Constructed k(*w.doc, "p{color:red}");
  w.doc->events.clear();
  const uint64_t g0 = gtext_cssom_document_generation(w.d());
  ASSERT_EQ(adopt(scope, k.s, 0), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_scope_adopted_remove(scope, 0, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_scope_remove_sheet(scope, a.s, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(w.doc->events.size(), 3u);
  EXPECT_EQ(w.doc->events[0].kind, GTEXT_CSSOM_EVENT_ADOPTED_CHANGED);
  EXPECT_EQ(w.doc->events[1].kind, GTEXT_CSSOM_EVENT_ADOPTED_CHANGED);
  EXPECT_EQ(w.doc->events[2].kind, GTEXT_CSSOM_EVENT_SHEET_REMOVED);
  EXPECT_EQ(gtext_cssom_document_generation(w.d()), g0 + 3);
  EXPECT_EQ(w.doc->events[2].sheet_id, gtext_cssom_sheet_id(a.s));
}

/* ---- adopted sheets ---------------------------------------------------------------------------------------------------------------------------- */

TEST(CssomDocument, AdoptedSheetsComeAfterTheOwnerSheetsAndAreOrderedByTheList) {
  World w;
  Constructed c1(*w.doc, "p{color:red}"), c2(*w.doc, "p{color:blue}");
  Owned owner(w, "p{color:green}");
  GTEXT_CSSOM_Scope * scope = gtext_cssom_document_scope(w.d());
  EXPECT_EQ(w.value("#a", "color"), "green");
  ASSERT_EQ(adopt(scope, c1.s, 0), GTEXT_CSS_OK);
  EXPECT_EQ(w.value("#a", "color"), "red") << "CSSOM: adopted style sheets are ordered after the document's own";
  ASSERT_EQ(adopt(scope, c2.s, 1), GTEXT_CSS_OK);
  EXPECT_EQ(w.value("#a", "color"), "blue");
  ASSERT_EQ(adopt(scope, c2.s, 0), GTEXT_CSS_OK);
  EXPECT_EQ(w.value("#a", "color"), "blue") << "the list is [c2, c1, c2]: c2 takes part once, at its last position, which is after c1";
}

TEST(CssomDocument, ASheetListedMoreThanOnceTakesPartOnceAtItsLastPosition) {
  // The last occurrence is the position (Constructable Stylesheets: the adopted list is applied in order and a repeated sheet applies where it last appears)
  World w;
  Constructed x(*w.doc, "p{color:red; margin-top:1px}"), y(*w.doc, "p{color:blue}");
  GTEXT_CSSOM_Scope * scope = gtext_cssom_document_scope(w.d());
  GTEXT_CSSOM_Sheet * list[] = {x.s, y.s, x.s};
  ASSERT_EQ(gtext_cssom_scope_adopted_set(scope, list, 3, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_scope_adopted_count(scope), 3u);
  EXPECT_EQ(w.value("#a", "color"), "red") << "x is last";
  EXPECT_EQ(gtext_css_rule_index_sheet_count(w.index()), 2u) << "registered once";
  GTEXT_CSSOM_Sheet * list2[] = {y.s, x.s, y.s};
  ASSERT_EQ(gtext_cssom_scope_adopted_set(scope, list2, 3, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(w.value("#a", "color"), "blue") << "y is last now";
  EXPECT_EQ(w.value("#a", "margin-top"), "1px");
}

TEST(CssomDocument, TakingAnAdoptedSheetOutOfTheListTakesItOutOfTheIndexEvenWhenNothingElseChanged) {
  World w;
  Constructed old_one(*w.doc, "p{color:red}");
  GTEXT_CSSOM_Scope * scope = gtext_cssom_document_scope(w.d());
  ASSERT_EQ(adopt(scope, old_one.s, 0), GTEXT_CSS_OK);
  Owned newest(w, "span{margin-top:1px}");
  ASSERT_EQ(insert_rule(newest.s, "div{margin-top:2px}", 0).st, GTEXT_CSS_OK) << "the sheet changed last has the greatest stamp, so a sheet leaving the set cannot be seen in it";
  EXPECT_EQ(gtext_css_rule_index_sheet_count(w.index()), 2u);
  ASSERT_EQ(gtext_cssom_scope_adopted_remove(scope, 0, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_rule_index_sheet_count(w.index()), 1u);
  GTEXT_CSSOM_Sheet * list[] = {old_one.s};
  ASSERT_EQ(gtext_cssom_scope_adopted_set(scope, list, 1, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_rule_index_sheet_count(w.index()), 2u);
  ASSERT_EQ(gtext_cssom_scope_adopted_set(scope, nullptr, 0, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_rule_index_sheet_count(w.index()), 1u);
}

TEST(CssomDocument, OnlyAConstructedSheetOfThisDocumentCanBeAdopted) {
  World a, b;
  Constructed mine(*a.doc), theirs(*b.doc);
  Sheet parsed(*a.doc, "p{}");
  GTEXT_CSSOM_Scope * scope = gtext_cssom_document_scope(a.d());
  GTEXT_CSSOM_Outcome o{};
  EXPECT_EQ(gtext_cssom_scope_adopted_insert(scope, parsed.s, 0, &o), GTEXT_CSS_E_NOT_ALLOWED) << "not constructed";
  EXPECT_EQ(gtext_cssom_scope_adopted_insert(scope, theirs.s, 0, &o), GTEXT_CSS_E_NOT_ALLOWED) << "constructed by another document";
  EXPECT_EQ(std::string(gtext_cssom_exception_name(GTEXT_CSS_E_NOT_ALLOWED)), "NotAllowedError");
  GTEXT_CSSOM_Sheet * bad[] = {mine.s, theirs.s};
  EXPECT_EQ(gtext_cssom_scope_adopted_set(scope, bad, 2, nullptr), GTEXT_CSS_E_NOT_ALLOWED);
  EXPECT_EQ(gtext_cssom_scope_adopted_count(scope), 0u) << "the list is as it was";
  EXPECT_EQ(gtext_cssom_scope_adopted_insert(scope, mine.s, 1, nullptr), GTEXT_CSS_E_INDEX_SIZE);
  ASSERT_EQ(adopt(scope, mine.s, 0), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_scope_adopted_remove(scope, 1, nullptr), GTEXT_CSS_E_INDEX_SIZE);
  GTEXT_CSSOM_Sheet * same[] = {mine.s};
  ASSERT_EQ(gtext_cssom_scope_adopted_set(scope, same, 1, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_UNCHANGED) << "a list equal to the current one";
  EXPECT_EQ(gtext_cssom_scope_adopted_set(scope, nullptr, 0, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(gtext_cssom_scope_adopted_count(scope), 0u);
}

TEST(CssomDocument, AnAdoptedSheetIsHeldByTheScopeAndTheListIsBounded) {
  World w([](GTEXT_CSSOM_Config & c) { c.max_adopted = 2; });
  GTEXT_CSSOM_Scope * scope = gtext_cssom_document_scope(w.d());
  GTEXT_CSSOM_Sheet * raw = nullptr;
  ASSERT_EQ(gtext_cssom_sheet_new_constructed(w.d(), nullptr, &raw), GTEXT_CSS_OK);
  ASSERT_EQ(adopt(scope, raw, 0), GTEXT_CSS_OK);
  gtext_cssom_sheet_release(raw);   // only the scope holds it now
  GTEXT_CSSOM_Sheet * held = gtext_cssom_scope_adopted_at(scope, 0);
  ASSERT_NE(held, nullptr);
  EXPECT_EQ(held, raw);
  ASSERT_EQ(adopt(scope, held, 1), GTEXT_CSS_OK);
  EXPECT_EQ(adopt(scope, held, 2), GTEXT_CSS_E_LIMIT_CSSOM) << "max_adopted counts a repeated sheet twice";
  gtext_cssom_sheet_release(held);
}

/* ---- tree scopes ------------------------------------------------------------------------------------------------------------------------------ */

TEST(CssomDocument, EachScopeRegistersItsSheetsWithItsContextAndTheIndexChecksTheOrder) {
  World w;
  int host_scope = 0, shadow_a = 0, shadow_b = 0;
  Scope outer(w.d(), &host_scope, true);
  ASSERT_EQ(outer.status, GTEXT_CSS_OK);
  Scope inner(w.d(), &shadow_a, true, outer.s);
  ASSERT_EQ(inner.status, GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_scope_context(gtext_cssom_document_scope(w.d())), 0u);
  EXPECT_EQ(gtext_cssom_scope_context(outer.s), 1u);
  EXPECT_EQ(gtext_cssom_scope_context(inner.s), 2u) << "assigned in the order made: a host's scope before its shadow trees";
  EXPECT_EQ(gtext_cssom_scope_parent(inner.s), outer.s);
  EXPECT_EQ(gtext_cssom_scope_parent(outer.s), nullptr);
  EXPECT_TRUE(gtext_cssom_scope_shadow(inner.s));
  EXPECT_EQ(gtext_cssom_scope_handle(inner.s), &shadow_a);
  EXPECT_EQ(gtext_cssom_scope_document(inner.s), w.d());
  Sheet doc_sheet(*w.doc, "p{color:red}"), outer_sheet(*w.doc, "p{margin-top:1px}"), inner_sheet(*w.doc, "p{padding-top:2px}");
  ASSERT_EQ(gtext_cssom_scope_insert_sheet(gtext_cssom_document_scope(w.d()), doc_sheet.s, 0, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_scope_insert_sheet(outer.s, outer_sheet.s, 0, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_scope_insert_sheet(inner.s, inner_sheet.s, 0, nullptr), GTEXT_CSS_OK);
  const GTEXT_CSS_Rule_Index * x = w.index();
  ASSERT_NE(x, nullptr);
  EXPECT_EQ(gtext_css_rule_index_sheet_count(x), 3u);
  EXPECT_NE(gtext_css_rule_index_layers(x, 0), nullptr);
  EXPECT_NE(gtext_css_rule_index_layers(x, 1), nullptr);
  EXPECT_NE(gtext_css_rule_index_layers(x, 2), nullptr);
  EXPECT_EQ(gtext_css_rule_index_layers(x, 3), nullptr);
  // C3c's check: a shadow tree's context must be greater than its host scope's. Explicit contexts that break it are the index's refusal.
  Scope bad(w.d(), &shadow_b, true, outer.s, 1);
  ASSERT_EQ(bad.status, GTEXT_CSS_OK);
  Sheet bad_sheet(*w.doc, "p{}");
  ASSERT_EQ(gtext_cssom_scope_insert_sheet(bad.s, bad_sheet.s, 0, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  EXPECT_EQ(w.index(&st), nullptr);
  EXPECT_EQ(st, GTEXT_CSS_E_INVALID) << "context order";
  ASSERT_EQ(gtext_cssom_scope_remove(bad.s, nullptr), GTEXT_CSS_OK);
  EXPECT_NE(w.index(&st), nullptr) << "and the index of the earlier state is not lost: it is built again without it";
  EXPECT_EQ(st, GTEXT_CSS_OK);
}

TEST(CssomDocument, AScopeIsRegisteredWithItsDocumentAndRefusesWhatDoesNotBelong) {
  World w, other;
  int h1 = 0, h2 = 0;
  GTEXT_CSSOM_Scope_Options o = gtext_cssom_scope_options_default();
  o.shadow = true;
  GTEXT_CSSOM_Scope * s = nullptr;
  EXPECT_EQ(gtext_cssom_scope_new(w.d(), &o, &s), GTEXT_CSS_E_INVALID) << "a shadow scope needs a handle";
  Scope foreign(other.d(), &h1, true);
  o.handle = &h2;
  o.parent = foreign.s;
  EXPECT_EQ(gtext_cssom_scope_new(w.d(), &o, &s), GTEXT_CSS_E_INVALID) << "a parent of another document";
  o.parent = nullptr;
  EXPECT_EQ(gtext_cssom_scope_new(nullptr, &o, &s), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_scope_remove(gtext_cssom_document_scope(w.d()), nullptr), GTEXT_CSS_E_INVALID) << "the document's own scope cannot be taken out";
  EXPECT_EQ(w.counters().live_scopes, 1u);
}

TEST(CssomDocument, TheNumberOfScopesIsBounded) {
  World w([](GTEXT_CSSOM_Config & c) { c.max_scopes = 3; });
  int h[4] = {0, 0, 0, 0};
  Scope a(w.d(), &h[0], true), b(w.d(), &h[1], true);
  EXPECT_EQ(a.status, GTEXT_CSS_OK);
  EXPECT_EQ(b.status, GTEXT_CSS_OK);
  Scope c(w.d(), &h[2], true);
  EXPECT_EQ(c.status, GTEXT_CSS_E_LIMIT_CSSOM);
  ASSERT_EQ(gtext_cssom_scope_remove(a.s, nullptr), GTEXT_CSS_OK);
  Scope d(w.d(), &h[3], true);
  EXPECT_EQ(d.status, GTEXT_CSS_OK);
}

TEST(CssomDocument, ARemovedScopeTakesItsSheetsOutOfTheCascadeAndStaysAnEmptyObject) {
  World w;
  int h = 0;
  Scope shadow(w.d(), &h, true);
  Constructed k(*w.doc, "p{color:red}");
  Sheet owner(*w.doc, "p{margin-top:1px}");
  ASSERT_EQ(adopt(shadow.s, k.s, 0), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_scope_insert_sheet(shadow.s, owner.s, 0, nullptr), GTEXT_CSS_OK);
  Owned newest(w, "span{margin-top:1px}");
  ASSERT_EQ(insert_rule(newest.s, "div{margin-top:2px}", 0).st, GTEXT_CSS_OK) << "the sheet changed last: what leaves the set cannot be seen in the greatest stamp";
  EXPECT_EQ(gtext_css_rule_index_sheet_count(w.index()), 3u);
  w.doc->events.clear();
  GTEXT_CSSOM_Outcome o{};
  ASSERT_EQ(gtext_cssom_scope_remove(shadow.s, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_CHANGED);
  ASSERT_EQ(w.doc->events.size(), 2u);
  EXPECT_EQ(w.doc->events[0].kind, GTEXT_CSSOM_EVENT_SHEET_REMOVED);
  EXPECT_EQ(w.doc->events[1].kind, GTEXT_CSSOM_EVENT_ADOPTED_CHANGED);
  EXPECT_EQ(gtext_css_rule_index_sheet_count(w.index()), 1u) << "only the document's own sheet is left";
  EXPECT_EQ(gtext_cssom_scope_document(shadow.s), nullptr);
  EXPECT_EQ(gtext_cssom_scope_sheet_count(shadow.s), 0u);
  EXPECT_EQ(gtext_cssom_scope_adopted_count(shadow.s), 0u);
  EXPECT_EQ(w.counters().live_scopes, 1u);
  ASSERT_EQ(gtext_cssom_scope_remove(shadow.s, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_UNCHANGED);
  // the sheets are untouched objects
  EXPECT_EQ(gtext_cssom_sheet_owner_node(owner.s), nullptr);
  EXPECT_EQ(insert_rule(owner.s, "q{}", 0).st, GTEXT_CSS_OK);
}

TEST(CssomDocument, ASheetAdoptedByTheDocumentAndByTwoShadowRootsIsOneObjectAndOneSnapshot) {
  World w;
  int h1 = 0, h2 = 0;
  Scope s1(w.d(), &h1, true), s2(w.d(), &h2, true);
  Constructed k(*w.doc, "p{color:red}");
  GTEXT_CSSOM_Scope * scopes[] = {gtext_cssom_document_scope(w.d()), s1.s, s2.s};
  for (GTEXT_CSSOM_Scope * sc : scopes) {
    ASSERT_EQ(adopt(sc, k.s, 0), GTEXT_CSS_OK);
  }
  const GTEXT_CSS_Rule_Index * x = w.index();
  EXPECT_EQ(gtext_css_rule_index_sheet_count(x), 3u) << "registered once for each scope";
  EXPECT_EQ(w.counters().snapshot_compiles, 1u) << "and compiled once";
  const uint64_t records = gtext_css_rule_index_record_count(x);
  EXPECT_EQ(records, 3u);
  ASSERT_EQ(insert_rule(k.s, "q{color:blue}", 1).st, GTEXT_CSS_OK);
  const GTEXT_CSS_Rule_Index * y = w.index();
  EXPECT_NE(y, x);
  EXPECT_EQ(gtext_css_rule_index_record_count(y), 6u) << "mutated once, all three scopes changed";
  EXPECT_EQ(w.counters().snapshot_compiles, 2u);
}

TEST(CssomDocument, AScopeMovedToAnotherDocumentKeepsItsListsAndTheOthersDocumentTakesOnlyItsOwnSheets) {
  // WPT CSSStyleSheet-template-adoption.html: a shadow root adopted by another document keeps adoptedStyleSheets, and the sheets constructed by the old document take part nowhere
  World a, b;
  int h = 0;
  Scope shadow(a.d(), &h, true);
  Constructed from_a(*a.doc, "p{color:red}");
  Constructed from_b(*b.doc, "p{margin-top:5px}");
  Sheet owner_a(*a.doc, "q{color:blue}");
  ASSERT_EQ(adopt(shadow.s, from_a.s, 0), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_scope_insert_sheet(shadow.s, owner_a.s, 0, nullptr), GTEXT_CSS_OK);
  // each document has a sheet of its own that was changed last, so that what leaves or joins a set cannot be seen in the greatest stamp
  Owned top_a(a, "span{margin-top:1px}"), top_b(b, "span{margin-top:1px}");
  ASSERT_EQ(insert_rule(top_a.s, "div{margin-top:2px}", 0).st, GTEXT_CSS_OK);
  ASSERT_EQ(insert_rule(top_b.s, "div{margin-top:2px}", 0).st, GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_rule_index_sheet_count(a.index()), 3u);
  EXPECT_EQ(gtext_css_rule_index_sheet_count(b.index()), 1u);
  a.doc->events.clear();
  b.doc->events.clear();
  GTEXT_CSSOM_Outcome o{};
  ASSERT_EQ(gtext_cssom_scope_reparent(shadow.s, b.d(), nullptr, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(gtext_cssom_scope_document(shadow.s), b.d());
  EXPECT_EQ(gtext_cssom_scope_adopted_count(shadow.s), 1u) << "the list is kept";
  ASSERT_EQ(a.doc->events.size(), 1u);
  ASSERT_EQ(b.doc->events.size(), 1u);
  EXPECT_EQ(a.doc->events[0].kind, GTEXT_CSSOM_EVENT_ADOPTED_CHANGED);
  EXPECT_EQ(b.doc->events[0].kind, GTEXT_CSSOM_EVENT_ADOPTED_CHANGED);
  EXPECT_EQ(gtext_css_rule_index_sheet_count(a.index()), 1u);
  EXPECT_EQ(gtext_css_rule_index_sheet_count(b.index()), 1u) << "A's sheets, the adopted one and the owner sheet, take part nowhere in B";
  EXPECT_EQ(gtext_cssom_scope_sheet_count(shadow.s), 1u) << "and the owner sheet stays listed for the caller to re-sync";
  // B can adopt its own sheet in the scope: the check is against the document the scope belongs to at that moment
  EXPECT_EQ(adopt(shadow.s, from_a.s, 0), GTEXT_CSS_E_NOT_ALLOWED);
  ASSERT_EQ(adopt(shadow.s, from_b.s, 1), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_rule_index_sheet_count(b.index()), 2u);
  EXPECT_EQ(gtext_cssom_scope_adopted_count(shadow.s), 2u);
  // and back: the list is as it was left, and A's sheet takes part again, B's no longer
  ASSERT_EQ(gtext_cssom_scope_reparent(shadow.s, a.d(), nullptr, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_rule_index_sheet_count(a.index()), 3u);
  EXPECT_EQ(gtext_css_rule_index_sheet_count(b.index()), 1u);
  ASSERT_EQ(gtext_cssom_scope_reparent(shadow.s, a.d(), nullptr, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_UNCHANGED);
}

TEST(CssomDocument, ReparentingRefusesWhatCannotBe) {
  World a, b;
  int h1 = 0, h2 = 0;
  Scope parent(a.d(), &h1, true), child(a.d(), &h2, true, parent.s);
  EXPECT_EQ(gtext_cssom_scope_reparent(parent.s, a.d(), child.s, nullptr), GTEXT_CSS_E_INVALID) << "a scope cannot sit under its own child";
  EXPECT_EQ(gtext_cssom_scope_reparent(child.s, b.d(), parent.s, nullptr), GTEXT_CSS_E_INVALID) << "the parent is of another document";
  EXPECT_EQ(gtext_cssom_scope_reparent(gtext_cssom_document_scope(a.d()), b.d(), nullptr, nullptr), GTEXT_CSS_E_INVALID);
  ASSERT_EQ(gtext_cssom_scope_remove(child.s, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_scope_reparent(child.s, b.d(), nullptr, nullptr), GTEXT_CSS_E_INVALID) << "a removed scope";
  ASSERT_EQ(gtext_cssom_scope_reparent(parent.s, b.d(), nullptr, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(a.counters().live_scopes, 1u);
  EXPECT_EQ(b.counters().live_scopes, 2u);
}

TEST(CssomDocument, AScopeOutlivesItsDocumentAndTheDocumentsSheetsAreReleasedWithIt) {
  GTEXT_CSSOM_Scope * scope = nullptr;
  GTEXT_CSSOM_Sheet * sheet = nullptr;
  int h = 0;
  {
    World w;
    GTEXT_CSSOM_Scope_Options o = gtext_cssom_scope_options_default();
    o.handle = &h;
    o.shadow = true;
    ASSERT_EQ(gtext_cssom_scope_new(w.d(), &o, &scope), GTEXT_CSS_OK);
    ASSERT_EQ(gtext_cssom_sheet_new_parsed(w.d(), "p{}", 3, nullptr, &sheet), GTEXT_CSS_OK);
    ASSERT_EQ(gtext_cssom_scope_insert_sheet(scope, sheet, 0, nullptr), GTEXT_CSS_OK);
    ASSERT_EQ(w.index() != nullptr, true);
    w.doc->release();
  }
  EXPECT_EQ(gtext_cssom_scope_document(scope), nullptr);
  EXPECT_EQ(gtext_cssom_scope_sheet_count(scope), 1u);
  EXPECT_EQ(gtext_cssom_sheet_rule_total(sheet), 1u);
  gtext_cssom_sheet_release(sheet);
  gtext_cssom_scope_release(scope);
}

}  // namespace
