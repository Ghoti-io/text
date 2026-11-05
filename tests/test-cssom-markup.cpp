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
 * The Markup binding (css_cssom_markup.h): the owner sheets of `<style>` and `<link rel=stylesheet>` elements, `sync` and its generation shortcut, the removed-owner lifecycle, the
 * one-element path, and the `style` attribute of a Markup element.
 *
 * The forms of the matrix (what makes an element have a sheet) are the answers of the HTML Standard and of the two browsers, which agree on every one of them
 * (probed in Chromium 156 and Firefox 158). Where the browsers differ (a `media` or `title` attribute changed, text set to the value it has) the test says which side the library is on.
 */

#include <gtest/gtest.h>

#include "cssom_markup_test_support.h"

using namespace com;

namespace {

/* ---- what has a sheet ---------------------------------------------------------------------------------------------------------------------------------------- */

struct Form {
  const char * what;
  const char * html;        ///< The body of a page; one `<style id=t>` or `<link id=t>`.
  int rules;                ///< Rules of its sheet, or -1 for none.
};

TEST(CssomMarkup, EveryFormOfTheMatrixHasASheetOrNone) {
  const std::vector<Form> forms = {
      {"a plain style", "<style id=t>p{} q{}</style>", 2},
      {"type empty", "<style id=t type=''>p{}</style>", 1},
      {"type text/css", "<style id=t type='text/css'>p{}</style>", 1},
      {"type TEXT/CSS", "<style id=t type='TEXT/CSS'>p{}</style>", 1},
      {"type text/plain", "<style id=t type='text/plain'>p{}</style>", -1},
      {"type with parameters", "<style id=t type='text/css; charset=utf-8'>p{}</style>", -1},
      {"type with a leading space", "<style id=t type=' text/css'>p{}</style>", -1},
      {"an empty style", "<style id=t></style>", 0},
      {"a style with only a comment", "<style id=t></style>", 0},
      {"an svg style", "<svg><style id=t>p{}</style></svg>", 1},
      {"an svg style of the wrong type", "<svg><style id=t type='text/plain'>p{}</style></svg>", -1},
      {"a link with no callback", "<link id=t rel=stylesheet href=a.css>", -1},
  };
  for (const Form & f : forms) {
    SCOPED_TRACE(f.what);
    const std::string html = std::string("<!DOCTYPE html><html><head></head><body>") + f.html + "</body></html>";
    Mk m(html);
    ASSERT_EQ(m.status, GTEXT_CSS_OK);
    ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
    GTEXT_Markup_Node * t = m.el("#t");
    ASSERT_NE(t, nullptr);
    GTEXT_CSSOM_Sheet * s = m.sheet(t);
    if (f.rules < 0) {
      EXPECT_EQ(s, nullptr);
      EXPECT_EQ(m.count(), 0u);
    }
    else {
      ASSERT_NE(s, nullptr);
      EXPECT_EQ(m.count(), 1u);
      EXPECT_EQ(gtext_cssom_sheet_rule_total(s), static_cast<size_t>(f.rules));
      EXPECT_EQ(gtext_cssom_sheet_owner_node(s), t);
      EXPECT_FALSE(gtext_cssom_sheet_constructed(s));
    }
    gtext_cssom_sheet_release(s);
  }
}

TEST(CssomMarkup, AStyleInATemplatesContentAndOneNotYetInsertedHaveNoSheet) {
  Mk m("<!DOCTYPE html><html><head></head><body><template id=tp></template></body></html>");
  GTEXT_Markup_Node * tp = m.el("#tp");
  ASSERT_EQ(gtext_markup_element_ensure_template_content(tp, nullptr), GTEXT_MARKUP_OK);
  GTEXT_Markup_Node * content = gtext_markup_element_template_content(tp);
  ASSERT_NE(content, nullptr);
  GTEXT_Markup_Node * inside = make_style(m.markup(), "p{}");
  append(content, inside);
  GTEXT_Markup_Node * loose = make_style(m.markup(), "q{}");
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  EXPECT_EQ(m.count(), 0u);
  EXPECT_EQ(m.sheet(inside), nullptr);
  EXPECT_EQ(m.sheet(loose), nullptr);
  // the one-element path agrees: it is not connected
  GTEXT_CSSOM_Outcome o{};
  ASSERT_EQ(gtext_cssom_markup_style_element_changed(m.binding, inside, &o), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_markup_style_element_changed(m.binding, loose, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_UNCHANGED);
  EXPECT_EQ(m.count(), 0u);
  // inserted: it has one
  append(m.el("body"), loose);
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  EXPECT_EQ(m.count(), 1u);
}

TEST(CssomMarkup, TheTextIsTheTextAndCdataChildrenInOrderAndNothingElse) {
  Mk m("<!DOCTYPE html><html><head></head><body></body></html>");
  GTEXT_Markup_Node * st = make(m.markup(), "style");
  GTEXT_Markup_Node * a = nullptr, * b = nullptr, * c = nullptr;
  ASSERT_EQ(gtext_markup_create_text(m.markup(), "p{co", 4, &a), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_create_comment(m.markup(), "x", 1, &c), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_create_text(m.markup(), "lor:red} q{}", 12, &b), GTEXT_MARKUP_OK);
  append(st, a);
  append(st, c);
  append(st, b);
  GTEXT_Markup_Node * child = make(m.markup(), "b");
  set_text(m.markup(), child, "r{}");
  append(st, child);   // an element child: its text is not the style's
  append(m.el("body"), st);
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  GTEXT_CSSOM_Sheet * s = m.sheet(st);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(gtext_cssom_sheet_rule_total(s), 2u) << "the two text children are one text, the comment is skipped, the element child is ignored";
  EXPECT_EQ(m.value("body", "color"), "-");
  gtext_cssom_sheet_release(s);
}

TEST(CssomMarkup, AStyleOfAnotherNamespaceAndOneTheOptionsTurnOffAreNotStyles) {
  {
    Mk m("<!DOCTYPE html><html><head></head><body></body></html>");
    GTEXT_Markup_Node * x = make_style(m.markup(), "p{}", "http://example.org/x");
    append(m.el("body"), x);
    ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
    EXPECT_EQ(m.count(), 0u);
  }
  {
    GTEXT_CSSOM_Markup_Options mo = gtext_cssom_markup_options_default();
    mo.html = false;
    Mk m("<!DOCTYPE html><html><head><style>p{}</style></head><body><svg><style>q{}</style></svg></body></html>", nullptr, nullptr, &mo);
    ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
    EXPECT_EQ(m.count(), 1u) << "only the svg one";
    EXPECT_EQ(m.order(), "q");
  }
  {
    GTEXT_CSSOM_Markup_Options mo = gtext_cssom_markup_options_default();
    mo.svg = false;
    Mk m("<!DOCTYPE html><html><head><style>p{}</style></head><body><svg><style>q{}</style></svg></body></html>", nullptr, nullptr, &mo);
    ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
    EXPECT_EQ(m.order(), "p");
  }
}

TEST(CssomMarkup, ASheetHasTheMediaTheTitleAndTheElementAsItsOwner) {
  Mk m("<!DOCTYPE html><html><head><style id=t media='print and (min-width: 3px)' title='My sheet'>p{}</style></head><body></body></html>");
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  GTEXT_CSSOM_Sheet * s = m.sheet(m.el("#t"));
  ASSERT_NE(s, nullptr);
  GTEXT_CSSOM_Media_List * list = nullptr;
  ASSERT_EQ(gtext_cssom_sheet_media(s, &list), GTEXT_CSS_OK);
  char * text = nullptr;
  size_t n = 0;
  ASSERT_EQ(gtext_cssom_media_text(list, &text, &n), GTEXT_CSS_OK);
  EXPECT_EQ(take(m.doc->core(), text), "print and (min-width: 3px)");
  gtext_cssom_media_release(list);
  ASSERT_EQ(gtext_cssom_sheet_title(s, &text, &n), GTEXT_CSS_OK);
  EXPECT_EQ(take(m.doc->core(), text), "My sheet");
  gtext_cssom_sheet_release(s);
}

TEST(CssomMarkup, TheSheetsAreInTheTreeOrderOfTheirElementsAndCascadeInIt) {
  Mk m;
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  EXPECT_EQ(m.count(), 2u);
  EXPECT_EQ(m.order(), "p,p");
  EXPECT_EQ(m.value("#a", "color"), "red");
  EXPECT_EQ(m.value("#a", "margin-top"), "1px");
  GTEXT_CSSOM_Outcome o{};
  ASSERT_EQ(m.sync(&o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_UNCHANGED);
}

TEST(CssomMarkup, ADocumentCollectedThroughTheBindingCompilesIndexesAndCascadesWithNoMutationApiUsed) {
  Mk m("<!DOCTYPE html><html><head><style>.lead{color:blue} #a{margin-top:3px}</style><style>p{color:green}</style></head><body><p id=a class=lead>x</p></body></html>");
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  EXPECT_EQ(m.value("#a", "color"), "blue") << "`.lead` is more specific than `p`, whatever the order";
  EXPECT_EQ(m.value("#a", "margin-top"), "3px");
}

/* ---- changes ------------------------------------------------------------------------------------------------------------------------------------------------- */

TEST(CssomMarkup, ATextChangeRemovesTheOldSheetAndMakesANewOneAndTheOtherIsKept) {
  Mk m;
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  GTEXT_Markup_Node * s1 = m.el("#s1"), * s2 = m.el("#s2");
  GTEXT_CSSOM_Sheet * old1 = m.sheet(s1), * old2 = m.sheet(s2);
  const uint64_t id1 = gtext_cssom_sheet_id(old1), id2 = gtext_cssom_sheet_id(old2);
  const GTEXT_CSS_Sheet * snap2 = snap(old2);
  Rec rec(m.d());
  set_text(m.markup(), s1, "q{color:blue}");
  GTEXT_CSSOM_Outcome o{};
  ASSERT_EQ(m.sync(&o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_CHANGED);
  GTEXT_CSSOM_Sheet * new1 = m.sheet(s1), * new2 = m.sheet(s2);
  EXPECT_NE(new1, old1);
  EXPECT_NE(gtext_cssom_sheet_id(new1), id1) << "a new sheet_id";
  EXPECT_EQ(new2, old2) << "the second sheet is the same object";
  EXPECT_EQ(gtext_cssom_sheet_id(new2), id2);
  EXPECT_EQ(snap(new2), snap2) << "and its snapshot is the same";
  EXPECT_EQ(rec.kinds_text(), "SHEET_REMOVED,SHEET_ADDED") << "one batch, in order";
  EXPECT_EQ(gtext_cssom_sheet_owner_node(old1), nullptr) << "the old sheet has no owner now";
  EXPECT_EQ(m.order(), "q,p");
  EXPECT_EQ(m.value("#a", "color"), "-");
  for (auto * s : {old1, old2, new1, new2}) gtext_cssom_sheet_release(s);
}

/* HTML Standard, "The style element": the "children changed steps" run "update a style block", which makes a new sheet whenever the children change, an empty text node included (the
   pinned css/cssom/css-style-reparse.html: "style.appendChild(document.createTextNode(""))" drops a rule added through the object model, in both browsers). */
TEST(CssomMarkup, AnEmptyTextNodeAddedToAStyleMakesItsSheetAgain) {
  Mk m("<!DOCTYPE html><html><head><style id=e></style></head><body><p id=a>x</p></body></html>");
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  GTEXT_Markup_Node * st = m.el("#e");
  GTEXT_CSSOM_Sheet * sh = m.sheet(st);
  ASSERT_NE(sh, nullptr);
  ASSERT_EQ(gtext_cssom_sheet_insert_rule(sh, "p{color:red}", 12, 0, nullptr, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_sheet_rule_total(sh), 1u);
  GTEXT_CSSOM_Outcome o{};
  ASSERT_EQ(m.sync(&o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_UNCHANGED) << "nothing changed yet";
  GTEXT_Markup_Node * empty = nullptr;
  ASSERT_EQ(gtext_markup_create_text(m.markup(), "", 0, &empty), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(st, empty), GTEXT_MARKUP_OK);
  ASSERT_EQ(m.sync(&o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_CHANGED);
  GTEXT_CSSOM_Sheet * again = m.sheet(st);
  ASSERT_NE(again, nullptr);
  EXPECT_NE(again, sh) << "a new sheet";
  EXPECT_EQ(gtext_cssom_sheet_rule_total(again), 0u) << "without the rule the object model added";
  gtext_cssom_sheet_release(again);
  gtext_cssom_sheet_release(sh);
}

/* CSSOM, "add a CSS style sheet": "If the title is not the empty string, the alternate flag is unset, and preferred CSS style sheet set name is the empty string change the preferred CSS
   style sheet set name to the title", and the disabled flag is unset when "The title is the empty string" or the title matches the preferred name, and set otherwise (the pinned
   css/cssom/stylesheet-title.html and preferred-stylesheet-order.html). */
TEST(CssomMarkup, TheFirstTitledSheetIsThePreferredSetAndTheOthersWithATitleAreDisabled) {
  Mk m("<!DOCTYPE html><html><head><style></style><style title=\"\"></style><style title=\"Preferred\">p{color:green}</style><style title=\"Not preferred\">p{color:red}</style>"
       "<style title=\"Preferred\">p{margin-top:2px}</style></head><body><p id=a>x</p></body></html>");
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  const bool want[] = {false, false, false, true, false};
  for (size_t i = 0; i < 5; i++) {
    GTEXT_CSSOM_Sheet * sh = m.sheet(m.el("style", i));
    ASSERT_NE(sh, nullptr) << i;
    EXPECT_EQ(gtext_cssom_sheet_disabled(sh), want[i]) << "sheet " << i;
    gtext_cssom_sheet_release(sh);
  }
  EXPECT_EQ(m.value("#a", "color"), "green");
  EXPECT_EQ(m.value("#a", "margin-top"), "2px") << "a second sheet of the preferred set is enabled too";
}

TEST(CssomMarkup, TextSetToWhatItWasChangesNothingAndTheRulesTheObjectModelAddedStay) {
  Mk m;
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  GTEXT_Markup_Node * s1 = m.el("#s1");
  GTEXT_CSSOM_Sheet * sh = m.sheet(s1);
  ASSERT_EQ(gtext_cssom_sheet_insert_rule(sh, "x{}", 3, 0, nullptr, nullptr), GTEXT_CSS_OK);
  set_text(m.markup(), s1, "p{color:red}");   // the same text again, as new nodes
  GTEXT_CSSOM_Outcome o{};
  ASSERT_EQ(m.sync(&o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_UNCHANGED);
  GTEXT_CSSOM_Sheet * again = m.sheet(s1);
  EXPECT_EQ(again, sh) << "the same sheet, with the rule that was added (Chromium does this; Firefox makes a new sheet: browsers-differ)";
  EXPECT_EQ(gtext_cssom_sheet_rule_total(again), 2u);
  gtext_cssom_sheet_release(again);
  gtext_cssom_sheet_release(sh);
}

TEST(CssomMarkup, TheObjectModelDoesNotChangeTheElementsText) {
  Mk m;
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  GTEXT_Markup_Node * s1 = m.el("#s1");
  GTEXT_CSSOM_Sheet * sh = m.sheet(s1);
  ASSERT_EQ(gtext_cssom_sheet_insert_rule(sh, "x{}", 3, 0, nullptr, nullptr), GTEXT_CSS_OK);
  char * text = nullptr;
  ASSERT_EQ(gtext_markup_node_text_content(s1, nullptr, &text, nullptr), GTEXT_MARKUP_OK);
  EXPECT_STREQ(text, "p{color:red}");
  gtext_markup_string_free(nullptr, text);
  gtext_cssom_sheet_release(sh);
}

TEST(CssomMarkup, ARemovedElementsSheetStaysAValidMutableObjectWithNoOwnerAndLeavesTheCascade) {
  Mk m;
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  GTEXT_Markup_Node * s1 = m.el("#s1");
  GTEXT_CSSOM_Sheet * sh = m.sheet(s1);
  EXPECT_EQ(m.value("#a", "color"), "red");
  Rec rec(m.d());
  detach(s1);
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  EXPECT_EQ(rec.kinds_text(), "SHEET_REMOVED");
  EXPECT_EQ(rec.seen[0].sheet, sh);
  EXPECT_EQ(gtext_cssom_sheet_owner_node(sh), nullptr);
  EXPECT_EQ(m.count(), 1u);
  EXPECT_EQ(m.value("#a", "color"), "-") << "and out of the index";
  EXPECT_EQ(gtext_cssom_sheet_rule_total(sh), 1u) << "it keeps its rules";
  EXPECT_EQ(gtext_cssom_sheet_insert_rule(sh, "x{}", 3, 0, nullptr, nullptr), GTEXT_CSS_OK) << "WPT CSSStyleSheet-modify-after-removal: it works";
  EXPECT_EQ(m.value("#a", "color"), "-") << "and affects nothing";
  EXPECT_EQ(m.sheet(s1), nullptr);
  // re-inserted: a new sheet
  append(m.el("head"), s1);
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  GTEXT_CSSOM_Sheet * fresh = m.sheet(s1);
  ASSERT_NE(fresh, nullptr);
  EXPECT_NE(fresh, sh);
  EXPECT_EQ(m.value("#a", "color"), "red");
  gtext_cssom_sheet_release(fresh);
  gtext_cssom_sheet_release(sh);
  GTEXT_CSSOM_Counters c = m.doc->core()->counters;
  (void)c;
}

TEST(CssomMarkup, AMovedElementIsANewSheetAndTheOthersAreKept) {
  Mk m("<!DOCTYPE html><html><head><style id=a>a{}</style><style id=b>b{}</style><style id=c>c{}</style></head><body></body></html>");
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  EXPECT_EQ(m.order(), "a,b,c");
  GTEXT_CSSOM_Sheet * ka = m.sheet(m.el("#a")), * kb = m.sheet(m.el("#b")), * kc = m.sheet(m.el("#c"));
  GTEXT_Markup_Node * a = m.el("#a");
  detach(a);
  append(m.el("head"), a);   // the first moved to the end
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  EXPECT_EQ(m.order(), "b,c,a");
  GTEXT_CSSOM_Sheet * na = m.sheet(m.el("#a"));
  EXPECT_NE(na, ka) << "a moved element is a new sheet (it was removed and inserted)";
  GTEXT_CSSOM_Sheet * nb = m.sheet(m.el("#b")), * nc = m.sheet(m.el("#c"));
  EXPECT_EQ(nb, kb);
  EXPECT_EQ(nc, kc);
  // the last to the front: only it is new
  GTEXT_Markup_Node * ae = m.el("#a");
  detach(ae);
  GTEXT_Markup_Node * firstchild = gtext_markup_node_first_child(m.el("head"));
  ASSERT_EQ(gtext_markup_insert_before(m.el("head"), ae, firstchild), GTEXT_MARKUP_OK);
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  EXPECT_EQ(m.order(), "a,b,c");
  GTEXT_CSSOM_Sheet * nb2 = m.sheet(m.el("#b"));
  EXPECT_EQ(nb2, kb) << "b and c did not move, a did";
  for (auto * s : {ka, kb, kc, na, nb, nc, nb2}) gtext_cssom_sheet_release(s);
}

TEST(CssomMarkup, ANewStyleIsPutWhereTheTreeSaysAndAnInsertedOneInTheMiddleDoesNotDisturbTheRest) {
  Mk m;
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  GTEXT_CSSOM_Sheet * k1 = m.sheet(m.el("#s1")), * k2 = m.sheet(m.el("#s2"));
  GTEXT_Markup_Node * mid = make_style(m.markup(), "mid{}");
  append(m.el("#d"), mid);   // between the two in document order
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  EXPECT_EQ(m.order(), "p,mid,p");
  GTEXT_CSSOM_Sheet * j1 = m.sheet(m.el("#s1")), * j2 = m.sheet(m.el("#s2"));
  EXPECT_EQ(j1, k1);
  EXPECT_EQ(j2, k2);
  for (auto * s : {k1, k2, j1, j2}) gtext_cssom_sheet_release(s);
}

TEST(CssomMarkup, TheMediaAndTheTitleAreChangedInPlace) {
  Mk m("<!DOCTYPE html><html><head><style id=t media=print title=T>p{}</style></head><body></body></html>");
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  GTEXT_Markup_Node * t = m.el("#t");
  GTEXT_CSSOM_Sheet * s = m.sheet(t);
  Rec rec(m.d());
  set_attr(t, "media", "screen");
  set_attr(t, "title", "U");
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  GTEXT_CSSOM_Sheet * now = m.sheet(t);
  EXPECT_EQ(now, s) << "the same sheet (Chromium; Firefox makes a new one: browsers-differ)";
  EXPECT_EQ(rec.kinds_text(), "SHEET_MEDIA_CHANGED");
  GTEXT_CSSOM_Media_List * list = nullptr;
  ASSERT_EQ(gtext_cssom_sheet_media(s, &list), GTEXT_CSS_OK);
  char * text = nullptr;
  size_t n = 0;
  ASSERT_EQ(gtext_cssom_media_text(list, &text, &n), GTEXT_CSS_OK);
  EXPECT_EQ(take(m.doc->core(), text), "screen");
  gtext_cssom_media_release(list);
  ASSERT_EQ(gtext_cssom_sheet_title(s, &text, &n), GTEXT_CSS_OK);
  EXPECT_EQ(take(m.doc->core(), text), "U");
  ASSERT_TRUE(gtext_markup_element_remove_attribute(t, nullptr, "media"));
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_sheet_media(s, &list), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_media_text(list, &text, &n), GTEXT_CSS_OK);
  EXPECT_EQ(take(m.doc->core(), text), "") << "a removed media attribute is the empty list";
  gtext_cssom_media_release(list);
  gtext_cssom_sheet_release(now);
  gtext_cssom_sheet_release(s);
}

TEST(CssomMarkup, TheTypeAttributeMakesAndUnmakesTheSheetAndTheNewOneIsANewObject) {
  Mk m("<!DOCTYPE html><html><head><style id=t>p{}</style></head><body></body></html>");
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  GTEXT_Markup_Node * t = m.el("#t");
  GTEXT_CSSOM_Sheet * s = m.sheet(t);
  set_attr(t, "type", "text/plain");
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  EXPECT_EQ(m.sheet(t), nullptr);
  EXPECT_EQ(m.count(), 0u);
  EXPECT_EQ(gtext_cssom_sheet_owner_node(s), nullptr);
  set_attr(t, "type", "text/css");
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  GTEXT_CSSOM_Sheet * back = m.sheet(t);
  ASSERT_NE(back, nullptr);
  EXPECT_NE(back, s);
  gtext_cssom_sheet_release(back);
  gtext_cssom_sheet_release(s);
}

/* ---- the cost ---------------------------------------------------------------------------------------------------------------------------------------------- */

TEST(CssomMarkup, AnUnchangedGenerationWalksNothing) {
  Mk m;
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  const GTEXT_CSSOM_Markup_Counters c0 = m.counters();
  for (int i = 0; i < 50; i++) {
    ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  }
  const GTEXT_CSSOM_Markup_Counters c1 = m.counters();
  EXPECT_EQ(c1.walks, c0.walks) << "no walk";
  EXPECT_EQ(c1.nodes_visited, c0.nodes_visited);
  EXPECT_EQ(c1.texts_compared, c0.texts_compared) << "and no text compared";
  EXPECT_EQ(c1.shortcuts, c0.shortcuts + 50);
  // a change that is not a style's costs one walk and makes no sheet
  set_attr(m.el("#d"), "data-x", "1");
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  const GTEXT_CSSOM_Markup_Counters c2 = m.counters();
  EXPECT_EQ(c2.walks, c1.walks + 1);
  EXPECT_EQ(c2.sheets_made, c1.sheets_made);
  EXPECT_EQ(c2.sheets_removed, c1.sheets_removed);
  EXPECT_EQ(c2.texts_compared, c1.texts_compared + 2) << "each style's text once";
}

TEST(CssomMarkup, TheOneElementPathLooksAtOneElementOnly) {
  std::string html = "<!DOCTYPE html><html><head>";
  for (int i = 0; i < 40; i++) {
    html += "<style id=s" + std::to_string(i) + ">.c" + std::to_string(i) + "{}</style>";
  }
  html += "</head><body><div id=d></div></body></html>";
  Mk m(html);
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  const GTEXT_CSSOM_Markup_Counters c0 = m.counters();
  GTEXT_Markup_Node * e = m.el("#s17");
  set_text(m.markup(), e, ".changed{}");
  GTEXT_CSSOM_Outcome o{};
  ASSERT_EQ(gtext_cssom_markup_style_element_changed(m.binding, e, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_CHANGED);
  const GTEXT_CSSOM_Markup_Counters c1 = m.counters();
  EXPECT_EQ(c1.walks, c0.walks) << "no walk";
  EXPECT_EQ(c1.nodes_visited, c0.nodes_visited);
  EXPECT_EQ(c1.texts_compared, c0.texts_compared + 1) << "one text";
  EXPECT_EQ(c1.sheets_made, c0.sheets_made + 1);
  EXPECT_EQ(c1.sheets_removed, c0.sheets_removed + 1);
  EXPECT_EQ(m.count(), 40u);
  GTEXT_CSSOM_Sheet * s = gtext_cssom_scope_sheet_at(m.scope(), 17);
  GTEXT_CSSOM_Rule * r = gtext_cssom_rule_list_at([&] { const GTEXT_CSSOM_Rule_List * l = nullptr; gtext_cssom_sheet_rules(s, &l); return l; }(), 0);
  char * text = nullptr;
  size_t n = 0;
  ASSERT_EQ(gtext_cssom_rule_selector_text(r, &text, &n), GTEXT_CSS_OK);
  EXPECT_EQ(take(m.doc->core(), text), ".changed") << "in the same place";
  gtext_cssom_rule_release(r);
  gtext_cssom_sheet_release(s);
  // an element that is not a style is no change
  ASSERT_EQ(gtext_cssom_markup_style_element_changed(m.binding, m.el("#d"), &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_UNCHANGED);
  // a new one, between two: found by looking back from it
  GTEXT_Markup_Node * fresh = make_style(m.markup(), ".fresh{}");
  ASSERT_EQ(gtext_markup_insert_before(m.el("head"), fresh, m.el("#s3")), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_cssom_markup_style_element_changed(m.binding, fresh, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(m.count(), 41u);
  GTEXT_CSSOM_Sheet * at3 = gtext_cssom_scope_sheet_at(m.scope(), 3);
  GTEXT_CSSOM_Sheet * freshs = m.sheet(fresh);
  EXPECT_EQ(at3, freshs);
  gtext_cssom_sheet_release(at3);
  gtext_cssom_sheet_release(freshs);
  // and removed
  detach(fresh);
  ASSERT_EQ(gtext_cssom_markup_style_element_changed(m.binding, fresh, &o), GTEXT_CSS_OK);
  EXPECT_EQ(m.count(), 40u);
  EXPECT_EQ(m.counters().walks, c0.walks);
  // a sync after all that finds the binding's view equal to the tree's
  const std::string before = m.order();
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  EXPECT_EQ(m.order(), before);
}

TEST(CssomMarkup, TheElementPathRefusesANodeOfAnotherDocument) {
  Mk m;
  Mk other;
  EXPECT_EQ(gtext_cssom_markup_style_element_changed(m.binding, other.el("#s1"), nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_markup_style_element_changed(m.binding, nullptr, nullptr), GTEXT_CSS_E_INVALID);
}

/* ---- links ------------------------------------------------------------------------------------------------------------------------------------------------ */

struct Links {
  std::map<std::string, std::string> table;   // href -> text, "pending", or "failed"
  std::vector<std::string> asked;
  std::vector<std::string> rels;
  GTEXT_CSS_Status fail = GTEXT_CSS_OK;
  static GTEXT_CSS_Status call(void * user, const GTEXT_CSSOM_Link_Request * req, GTEXT_CSSOM_Link_Text * text, GTEXT_CSSOM_Link_Outcome * outcome) {
    Links * self = static_cast<Links *>(user);
    self->asked.emplace_back(req->href, req->href_length);
    self->rels.push_back(req->rel);
    if (self->fail != GTEXT_CSS_OK) {
      return self->fail;
    }
    auto it = self->table.find(self->asked.back());
    if (it == self->table.end() || it->second == "failed") {
      *outcome = GTEXT_CSSOM_LINK_FAILED;
    }
    else if (it->second == "pending") {
      *outcome = GTEXT_CSSOM_LINK_PENDING;
    }
    else {
      text->text = it->second.data();
      text->length = it->second.size();
      *outcome = GTEXT_CSSOM_LINK_LOADED;
    }
    return GTEXT_CSS_OK;
  }
};

GTEXT_CSSOM_Markup_Options with_links(Links & l) {
  GTEXT_CSSOM_Markup_Options mo = gtext_cssom_markup_options_default();
  mo.link = &Links::call;
  mo.link_user = &l;
  return mo;
}

TEST(CssomMarkupLinks, AStylesheetLinkIsAskedAndItsTextBecomesTheSheetAndOthersAreNotAsked) {
  Links l;
  l.table["a.css"] = "a{color:red}";
  GTEXT_CSSOM_Markup_Options mo = with_links(l);
  Mk m("<!DOCTYPE html><html><head><link id=a rel='Stylesheet' href=a.css><link id=n rel=icon href=i.png><link id=t rel=stylesheet type=text/plain href=t.css>"
       "<link id=e rel=stylesheet><link id=al rel=alternate href=al.css></head><body></body></html>", nullptr, nullptr, &mo);
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  EXPECT_EQ(l.asked, (std::vector<std::string>{"a.css"})) << "only the one that is a stylesheet link with a type that allows it and an href";
  EXPECT_EQ(m.count(), 1u);
  EXPECT_EQ(m.order(), "a");
  GTEXT_CSSOM_Sheet * s = m.sheet(m.el("#a"));
  ASSERT_NE(s, nullptr);
  EXPECT_FALSE(gtext_cssom_sheet_disabled(s));
  gtext_cssom_sheet_release(s);
  // a second sync asks nobody
  set_attr(m.el("#n"), "data-x", "1");
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  EXPECT_EQ(l.asked.size(), 1u);
}

TEST(CssomMarkupLinks, AnAlternateStylesheetIsAlternateAndDisabledAndTheDisabledAttributeDisables) {
  Links l;
  l.table["a.css"] = "a{}";
  l.table["b.css"] = "b{}";
  GTEXT_CSSOM_Markup_Options mo = with_links(l);
  Mk m("<!DOCTYPE html><html><head><link id=a rel='alternate stylesheet' title=x href=a.css><link id=b rel=stylesheet disabled href=b.css></head><body></body></html>", nullptr, nullptr, &mo);
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  GTEXT_CSSOM_Sheet * a = m.sheet(m.el("#a")), * b = m.sheet(m.el("#b"));
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_TRUE(gtext_cssom_sheet_alternate(a));
  EXPECT_TRUE(gtext_cssom_sheet_disabled(a));
  EXPECT_FALSE(gtext_cssom_sheet_alternate(b));
  EXPECT_TRUE(gtext_cssom_sheet_disabled(b));
  gtext_cssom_sheet_release(a);
  gtext_cssom_sheet_release(b);
}

TEST(CssomMarkupLinks, APendingLinkHasNoSheetUntilItIsCompleteAndAStaleAnswerIsIgnored) {
  Links l;
  l.table["a.css"] = "pending";
  GTEXT_CSSOM_Markup_Options mo = with_links(l);
  Mk m("<!DOCTYPE html><html><head><style>s{}</style><link id=a rel=stylesheet href=a.css><style>t{}</style></head><body></body></html>", nullptr, nullptr, &mo);
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  EXPECT_EQ(m.order(), "s,t");
  EXPECT_EQ(m.sheet(m.el("#a")), nullptr);
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  set_attr(m.el("body"), "x", "1");
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  EXPECT_EQ(l.asked.size(), 1u) << "a pending link is not asked again";
  GTEXT_CSSOM_Link_Text lt;
  std::memset(&lt, 0, sizeof lt);
  lt.size = sizeof lt;
  lt.text = "a{color:red}";
  lt.length = 12;
  lt.origin_clean = true;
  // the link changes before the answer arrives: the answer is stale
  set_attr(m.el("#a"), "href", "b.css");
  GTEXT_CSSOM_Outcome o{};
  ASSERT_EQ(gtext_cssom_markup_link_complete(m.binding, m.el("#a"), &lt, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_UNCHANGED);
  EXPECT_EQ(m.count(), 2u);
  set_attr(m.el("#a"), "href", "a.css");
  ASSERT_EQ(gtext_cssom_markup_link_complete(m.binding, m.el("#a"), &lt, &o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(m.order(), "s,a,t") << "in the place of its element";
  EXPECT_EQ(gtext_cssom_markup_link_complete(m.binding, m.el("#a"), &lt, nullptr), GTEXT_CSS_E_NOT_FOUND) << "once";
}

TEST(CssomMarkupLinks, AFailedLinkIsNotAskedAgainUntilItsAttributesChangeAndACallbackThatFailsFailsTheSyncAndTheNextGoesOn) {
  Links l;
  l.table["a.css"] = "failed";
  l.fail = GTEXT_CSS_E_UNSUPPORTED;
  GTEXT_CSSOM_Markup_Options mo = with_links(l);
  Mk m("<!DOCTYPE html><html><head><link id=a rel=stylesheet href=a.css><style>s{}</style></head><body></body></html>", nullptr, nullptr, &mo);
  EXPECT_EQ(m.sync(), GTEXT_CSS_E_UNSUPPORTED);
  l.fail = GTEXT_CSS_OK;
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK) << "the next call goes on";
  EXPECT_EQ(m.order(), "s");
  EXPECT_EQ(l.asked.size(), 2u);
  set_attr(m.el("body"), "x", "1");
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  EXPECT_EQ(l.asked.size(), 2u) << "failed once, not asked again";
  l.table["a.css"] = "a{}";
  set_attr(m.el("#a"), "href", "a.css?2");
  l.table["a.css?2"] = "a{}";
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  EXPECT_EQ(m.order(), "a,s");
}

/* ---- events and the whole ---------------------------------------------------------------------------------------------------------------------------------- */

TEST(CssomMarkup, TheEventsOfOneSyncArriveAsOneBatchAfterItAndNeverInsideIt) {
  Mk m;
  Rec rec(m.d());
  rec.hook = [&](const GTEXT_CSSOM_Event &, Seen & s) {
    s.note = std::to_string(m.count());
    return GTEXT_CSS_OK;
  };
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  EXPECT_EQ(rec.kinds_text(), "SHEET_ADDED,SHEET_ADDED");
  EXPECT_EQ(rec.seen[0].note, "2") << "delivered after the sync is complete";
  EXPECT_EQ(rec.seen[1].note, "2");
  EXPECT_EQ(gtext_cssom_document_batch_depth(m.d()), 0u);
}

TEST(CssomMarkup, ASyncFromAnObserverIsReentrantAndLeavesTheBindingUsable) {
  Mk m;
  Rec rec(m.d());
  GTEXT_CSS_Status got = GTEXT_CSS_OK;
  rec.hook = [&](const GTEXT_CSSOM_Event &, Seen &) {
    set_attr(m.el("#d"), "data-y", "1");   // the tree moved, so a sync has something to do
    got = m.sync();
    return GTEXT_CSS_OK;
  };
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  EXPECT_EQ(got, GTEXT_CSS_E_REENTRANT);
  rec.hook = nullptr;
  EXPECT_EQ(m.order(), "p,p");
  set_text(m.markup(), m.el("#s1"), "z{}");
  EXPECT_EQ(m.sync(), GTEXT_CSS_OK);
  EXPECT_EQ(m.order(), "z,p");
}

TEST(CssomMarkup, TheBindingOutlivesTheDocumentHandleAndSaysSo) {
  auto m = std::make_unique<Mk>();
  ASSERT_EQ(m->sync(), GTEXT_CSS_OK);
  m->doc->release();
  EXPECT_EQ(m->sync(), GTEXT_CSS_E_INVALID);
  m->doc->d = nullptr;
}

TEST(CssomMarkup, ASyncFollowsAnyEditOfTheTreeToTheStateAFreshBindingMakes) {
  Mk m;
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  // a run of edits, then a comparison with a binding made at the end
  GTEXT_Markup_Node * x = make_style(m.markup(), "x{}");
  append(m.el("body"), x);
  set_text(m.markup(), m.el("#s2"), "y{}");
  detach(m.el("#s1"));
  GTEXT_Markup_Node * z = make_style(m.markup(), "z{}");
  ASSERT_EQ(gtext_markup_insert_before(m.el("body"), z, gtext_markup_node_first_child(m.el("body"))), GTEXT_MARKUP_OK);
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  Mk fresh_unused("<!DOCTYPE html><html><head></head><body></body></html>");
  EXPECT_EQ(m.order(), "z,y,x");
}

/* ---- the style attribute ------------------------------------------------------------------------------------------------------------------------------------ */

TEST(CssomMarkupInline, TheDeclarationOfAMarkupElementWritesTheAttributeAndSeesAnEditMadeThroughTheMarkupApi) {
  Mk m;
  GTEXT_Markup_Node * a = m.el("#a");
  const GTEXT_CSSOM_Attribute_Binding binding = gtext_cssom_markup_inline_binding();
  GTEXT_CSSOM_Decl * d = nullptr;
  ASSERT_EQ(gtext_cssom_inline_style(m.d(), a, &binding, &d), GTEXT_CSS_OK);
  const uint64_t g0 = gtext_markup_document_generation(m.markup());
  ASSERT_EQ(set(d, "color", "red").st, GTEXT_CSS_OK);
  GTEXT_Markup_Attribute attr;
  ASSERT_TRUE(gtext_markup_element_get_attribute(a, nullptr, "style", &attr));
  EXPECT_EQ(std::string(attr.value, attr.value_length), "color: red;");
  EXPECT_GT(gtext_markup_document_generation(m.markup()), g0) << "the write is a change of the tree like any other";
  // an edit behind the declaration's back is seen at the next call
  set_attr(a, "style", "margin-top: 2px");
  EXPECT_EQ(get(d, "margin-top"), "2px");
  EXPECT_EQ(get(d, "color"), "");
  // a declaration of an element with no attribute is empty, and setting it makes the attribute
  GTEXT_Markup_Node * b = m.el("#b");
  GTEXT_CSSOM_Decl * db = nullptr;
  ASSERT_EQ(gtext_cssom_inline_style(m.d(), b, &binding, &db), GTEXT_CSS_OK);
  EXPECT_EQ(length(db), 0u);
  ASSERT_EQ(set(db, "padding-top", "1px").st, GTEXT_CSS_OK);
  ASSERT_TRUE(gtext_markup_element_get_attribute(b, nullptr, "style", &attr));
  gtext_cssom_decl_release(db);
  gtext_cssom_decl_release(d);
}

TEST(CssomMarkupInline, TheStyleAttributeIsNotAStyleElementAndASyncAfterAWriteWalksButMakesNothing) {
  Mk m;
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  const GTEXT_CSSOM_Attribute_Binding binding = gtext_cssom_markup_inline_binding();
  GTEXT_CSSOM_Decl * d = nullptr;
  ASSERT_EQ(gtext_cssom_inline_style(m.d(), m.el("#a"), &binding, &d), GTEXT_CSS_OK);
  const GTEXT_CSSOM_Markup_Counters c0 = m.counters();
  ASSERT_EQ(set(d, "color", "red").st, GTEXT_CSS_OK);
  GTEXT_CSSOM_Outcome o{};
  ASSERT_EQ(m.sync(&o), GTEXT_CSS_OK);
  EXPECT_EQ(o.kind, GTEXT_CSSOM_UNCHANGED);
  const GTEXT_CSSOM_Markup_Counters c1 = m.counters();
  EXPECT_EQ(c1.walks, c0.walks + 1);
  EXPECT_EQ(c1.sheets_made, c0.sheets_made);
  gtext_cssom_decl_release(d);
}

}  // namespace
