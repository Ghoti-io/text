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
 * The web-platform-tests pages of `matchMedia`, `MediaQueryList`, `CSS.supports()` and `adoptedStyleSheets`, ported by hand: each test below is one test of a pinned page, named for it, with
 * the assertions in the page's order and the page's own words in the comments. The op interpreter of the conformance runner reads declarative calls; these pages are script (iframes that are
 * resized, promises, an observable array), so they are carried here as straight-line C++ over the document layer, and the scored count of the CSSOM gate leaves them out. What a binding owns (a
 * `TypeError` for a missing argument, `null` listeners, `instanceof`) is not ported; the shadow-tree variants of the cascade-order page are not either, because the Markup tree has no shadow
 * roots (the order inside a scope is the same code for the document's scope, which is what the document variants assert).
 *
 * An iframe is a document of its own with the conditions of its size; resizing it is gtext_cssom_document_set_conditions().
 */

#include <string>

#include <gtest/gtest.h>

#include "cssom_document_test_support.h"

using namespace cod;

namespace {

bool supports(World & w, const char * property, const char * value) {
  bool r = false;
  EXPECT_EQ(gtext_cssom_supports(w.d(), property, value, &r), GTEXT_CSS_OK);
  return r;
}

bool supports_text(World & w, const std::string & text) {
  bool r = false;
  EXPECT_EQ(gtext_cssom_supports_text(w.d(), text.data(), text.size(), &r), GTEXT_CSS_OK) << text;
  return r;
}

/// `matchMedia(text)` in a document: the pair (media, matches).
std::pair<std::string, bool> mm(World & w, const std::string & text) {
  GTEXT_CSSOM_Media_Query_List * l = nullptr;
  EXPECT_EQ(gtext_cssom_match_media(w.d(), text.data(), text.size(), &l), GTEXT_CSS_OK);
  char * media = nullptr;
  size_t n = 0;
  EXPECT_EQ(gtext_cssom_media_query_list_media(l, &media, &n), GTEXT_CSS_OK);
  const std::string m(media, n);
  gtext_cssom_buffer_free(nullptr, media);
  bool matches = false;
  EXPECT_EQ(gtext_cssom_media_query_list_matches(l, &matches), GTEXT_CSS_OK);
  gtext_cssom_media_query_list_release(l);
  return {m, matches};
}

/* ---- css/cssom/CSS-supports.html ------------------------------------------------------------------------------------------------------------ */

TEST(CssomWptDocument, CssSupportsPropertyValueWithValidInputs) {
  World w;
  EXPECT_TRUE(supports(w, "color", "red")) << "color: red is supported";
  EXPECT_TRUE(supports(w, "display", "block")) << "display: block is supported";
  EXPECT_TRUE(supports(w, "margin", "10px 20px")) << "margin: 10px 20px is supported";
  EXPECT_TRUE(supports(w, "--my-custom-color", "blue")) << "Custom properties are supported";
}

TEST(CssomWptDocument, CssSupportsPropertyValueWithInvalidInputs) {
  World w;
  EXPECT_FALSE(supports(w, "not-a-real-property", "red")) << "Invalid property should return false";
  EXPECT_FALSE(supports(w, "color", "rainbow")) << "Invalid value for color should return false";
  EXPECT_FALSE(supports(w, "display", "10px")) << "Invalid value for display should return false";
}

TEST(CssomWptDocument, CssSupportsPropertyValueEdgeCases) {
  World w;
  EXPECT_FALSE(supports(w, "  color  ", "red")) << "Property names with leading/trailing spaces should return false";
  EXPECT_TRUE(supports(w, "color", "  red  ")) << "Leading and trailing spaces in the value should be ignored";
}

TEST(CssomWptDocument, CssSupportsConditionTextWithParentheses) {
  World w;
  EXPECT_TRUE(supports_text(w, "(color: red)")) << "(color: red) is supported";
  EXPECT_FALSE(supports_text(w, "(color: rainbow)")) << "(color: rainbow) is not supported";
  EXPECT_FALSE(supports_text(w, "(not-a-real-property: red)")) << "Invalid property in condition is not supported";
}

TEST(CssomWptDocument, CssSupportsConditionTextWithLogicalOperators) {
  World w;
  EXPECT_TRUE(supports_text(w, "not (color: rainbow)")) << "'not' operator should work";
  EXPECT_TRUE(supports_text(w, "(color: red) and (display: block)")) << "'and' operator should work";
  EXPECT_TRUE(supports_text(w, "(color: red) or (not-a-real-property: red)")) << "'or' operator should work";
  EXPECT_TRUE(supports_text(w, "NOT (color: rainbow)")) << "Upper cased logical operator should work";
  EXPECT_FALSE(supports_text(w, "(color: red) and (not-a-real-property: red)")) << "'and' operator with one invalid condition should be false";
  EXPECT_FALSE(supports_text(w, "not")) << "'not' operator with unexpected end of input should be false";
  EXPECT_FALSE(supports_text(w, "(color: red) and")) << "'and' operator with unexpected end of input should be false";
}

TEST(CssomWptDocument, CssSupportsConditionTextWithoutParentheses) {
  World w;
  EXPECT_TRUE(supports_text(w, "color: red")) << "Condition text without parentheses (single declaration) should return true";
  EXPECT_FALSE(supports_text(w, "not-a-real-property: red")) << "Invalid single declaration without parentheses should return false";
}

TEST(CssomWptDocument, CssSupportsAndImplementationKeywordAndMinFunction) {
  // The page also reads `getComputedStyle(div).fontSize` ("16px"), which the computed-style tests answer. The `style` half, with the document's own element-owned declaration:
  World w;
  EXPECT_TRUE(supports(w, "font", "revert")) << "CSS.supports";
  EXPECT_TRUE(supports(w, "width", "min(100px, 50%)")) << "value";
}

TEST(CssomWptDocument, CssSupportsAndShorthandLonghand) {
  World w;
  EXPECT_TRUE(supports(w, "margin", "10px")) << "valid shorthand";
  EXPECT_FALSE(supports(w, "margin-top", "!invalid")) << "invalid longhand";
}

TEST(CssomWptDocument, CssSupportsInvalidValueSemi) {
  World w;
  EXPECT_FALSE(supports_text(w, ";")) << "invalid value semi";
}

/* ---- css/cssom/mediaquery-sort-dedup.html ------------------------------------------------------------------------------------------------- */

TEST(CssomWptDocument, MediaQueriesAreNotSortedLexicographically) {
  World w;
  EXPECT_EQ(mm(w, "(min-width: 10px) and (min-height: 10px)").first, "(min-width: 10px) and (min-height: 10px)");
}

TEST(CssomWptDocument, MediaQueriesAreNotDeduplicated) {
  World w;
  EXPECT_EQ(mm(w, "(color) and (color)").first, "(color) and (color)");
}

/* ---- css/cssom-view/matchMedia.html --------------------------------------------------------------------------------------------------------- */

TEST(CssomWptDocument, MatchMediaAllMatches) {
  World w;
  const auto r = mm(w, "all");
  EXPECT_EQ(r.first, "all");
  EXPECT_TRUE(r.second);
}

TEST(CssomWptDocument, MatchMediaEmptyMatches) {
  World w;
  const auto r = mm(w, "");
  EXPECT_EQ(r.first, "");
  EXPECT_TRUE(r.second);
}

TEST(CssomWptDocument, MatchMediaMinWidth1pxMatches) {
  World w;
  const auto r = mm(w, "(min-width: 1px)");
  EXPECT_EQ(r.first, "(min-width: 1px)");
  EXPECT_TRUE(r.second);
}

TEST(CssomWptDocument, MediaQueryWithSyntaxErrorIsSerializedAsNotAll) {
  World w;
  const auto r = mm(w, "::");
  EXPECT_EQ(r.first, "not all");
  EXPECT_FALSE(r.second);
}

/// The page's iframes: a document whose viewport is this size.
struct Frame {
  Cond cond;
  World w;
  Frame(double width, double height) : cond(width, nullptr, height), w([this](GTEXT_CSSOM_Config & c) { c.conditions = cond.c; }) {}
};

TEST(CssomWptDocument, IframeMatchMediaIsSerializedWithoutAll) {
  Frame f(200, 200);
  const auto r = mm(f.w, "(max-width: 199px), all and (min-width: 200px)");
  EXPECT_EQ(r.first, "(max-width: 199px), (min-width: 200px)");
  EXPECT_TRUE(r.second);
}

TEST(CssomWptDocument, IframeMatchMediaMinAspectRatioMatches) {
  Frame f(200, 200);
  EXPECT_TRUE(mm(f.w, "(min-aspect-ratio: 1/1)").second);
}

TEST(CssomWptDocument, IframeMatchMediaWidth200pxMatches) {
  Frame f(200, 200);
  EXPECT_TRUE(mm(f.w, "(width: 200px)").second);
}

TEST(CssomWptDocument, IframeMatchMediaMaxHeight50pxMatches) {
  Frame f(200, 100);
  EXPECT_FALSE(mm(f.w, "(max-height: 50px)").second);
}

TEST(CssomWptDocument, IframeMatchMediaMinWidth150pxMatches) {
  Frame f(200, 100);
  EXPECT_TRUE(mm(f.w, "(min-width: 150px)").second);
}

/* ---- css/cssom-view/MediaQueryList-addListener-removeListener.html and -change-event-matches-value.html ----------------------------------------- */

struct Events {
  std::vector<std::string> calls;
  std::vector<std::pair<std::string, bool>> seen;   ///< (media, matches) of each call
};

struct Hook {
  Events * e;
  std::string tag;
  GTEXT_CSSOM_Media_Query_List * list = nullptr;
};

GTEXT_CSS_Status hook(void * user, GTEXT_CSSOM_Media_Query_List * l, bool matches) {
  Hook * h = static_cast<Hook *>(user);
  h->e->calls.push_back(h->tag);
  char * media = nullptr;
  size_t n = 0;
  gtext_cssom_media_query_list_media(l, &media, &n);
  h->e->seen.emplace_back(std::string(media, n), matches);
  gtext_cssom_buffer_free(nullptr, media);
  return GTEXT_CSS_OK;
}

void resize(World & w, double width, double height, std::vector<std::unique_ptr<Cond>> & keep) {
  keep.push_back(std::make_unique<Cond>(width, nullptr, height));
  ASSERT_EQ(gtext_cssom_document_set_conditions(w.d(), keep.back()->c, nullptr), GTEXT_CSS_OK);
}

TEST(CssomWptDocument, ListenersAreCalledWhenIframeIsResized) {
  Frame f(200, 100);
  std::vector<std::unique_ptr<Cond>> keep;
  GTEXT_CSSOM_Media_Query_List *height_mql = nullptr, *width_mql = nullptr;
  ASSERT_EQ(gtext_cssom_match_media(f.w.d(), "(max-height: 50px)", 18, &height_mql), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_match_media(f.w.d(), "(min-width: 150px)", 18, &width_mql), GTEXT_CSS_OK);
  Events height_event, width_event;
  Hook hh{&height_event, "height"}, hw{&width_event, "width"};
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(height_mql, hook, &hh), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(width_mql, hook, &hw), GTEXT_CSS_OK);
  bool m = true;
  ASSERT_EQ(gtext_cssom_media_query_list_matches(height_mql, &m), GTEXT_CSS_OK);
  EXPECT_FALSE(m);
  ASSERT_EQ(gtext_cssom_media_query_list_matches(width_mql, &m), GTEXT_CSS_OK);
  EXPECT_TRUE(m);
  resize(f.w, 200, 50, keep);   // 200x100 => 200x50
  ASSERT_EQ(height_event.seen.size(), 1u);
  EXPECT_EQ(height_event.seen[0].first, "(max-height: 50px)");   // heightEvent.media == heightMQL.media
  EXPECT_TRUE(height_event.seen[0].second);
  EXPECT_TRUE(width_event.seen.empty()) << "the width query did not change";
  resize(f.w, 100, 50, keep);   // 200x50 => 100x50
  ASSERT_EQ(width_event.seen.size(), 1u);
  EXPECT_EQ(width_event.seen[0].first, "(min-width: 150px)");
  EXPECT_FALSE(width_event.seen[0].second);
  ASSERT_EQ(gtext_cssom_media_query_list_matches(width_mql, &m), GTEXT_CSS_OK);
  EXPECT_FALSE(m);
  ASSERT_EQ(gtext_cssom_media_query_list_matches(height_mql, &m), GTEXT_CSS_OK);
  EXPECT_TRUE(m);
  gtext_cssom_media_query_list_release(height_mql);
  gtext_cssom_media_query_list_release(width_mql);
}

TEST(CssomWptDocument, ListenersAreCalledCorrectNumberOfTimes) {
  Frame f(200, 200);
  std::vector<std::unique_ptr<Cond>> keep;
  GTEXT_CSSOM_Media_Query_List * mql = nullptr;
  ASSERT_EQ(gtext_cssom_match_media(f.w.d(), "(max-width: 200px)", 18, &mql), GTEXT_CSS_OK);
  Events e;
  Hook h{&e, "x"};
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(mql, hook, &h), GTEXT_CSS_OK);
  for (size_t i = 1; i <= 10; i++) {
    resize(f.w, i % 2 ? 300 : 200, 200, keep);   // triggerMQLEvent: the answer changes every time
    EXPECT_EQ(e.calls.size(), i);
  }
  gtext_cssom_media_query_list_release(mql);
}

TEST(CssomWptDocument, ListenersAreCalledInOrderTheyWereAdded) {
  Frame f(200, 200);
  std::vector<std::unique_ptr<Cond>> keep;
  GTEXT_CSSOM_Media_Query_List * mql = nullptr;
  ASSERT_EQ(gtext_cssom_match_media(f.w.d(), "(max-width: 200px)", 18, &mql), GTEXT_CSS_OK);
  Events e;
  Hook first{&e, "1st"}, second{&e, "2nd"};
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(mql, hook, &first), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(mql, hook, &second), GTEXT_CSS_OK);
  resize(f.w, 300, 200, keep);
  EXPECT_EQ(e.calls, (std::vector<std::string>{"1st", "2nd"}));
  gtext_cssom_media_query_list_release(mql);
}

TEST(CssomWptDocument, ListenerThatWasAddedTwiceIsCalledOnlyOnce) {
  Frame f(200, 200);
  std::vector<std::unique_ptr<Cond>> keep;
  GTEXT_CSSOM_Media_Query_List * mql = nullptr;
  ASSERT_EQ(gtext_cssom_match_media(f.w.d(), "(max-width: 200px)", 18, &mql), GTEXT_CSS_OK);
  Events e;
  Hook h{&e, "l"};
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(mql, hook, &h), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(mql, hook, &h), GTEXT_CSS_OK);
  resize(f.w, 300, 200, keep);
  EXPECT_EQ(e.calls.size(), 1u);
  gtext_cssom_media_query_list_release(mql);
}

TEST(CssomWptDocument, ListenersAreCalledInOrderTheirMqlsWereCreated) {
  Frame f(100, 100);
  std::vector<std::unique_ptr<Cond>> keep;
  GTEXT_CSSOM_Media_Query_List *mql1 = nullptr, *mql2 = nullptr;
  ASSERT_EQ(gtext_cssom_match_media(f.w.d(), "(min-width: 200px)", 18, &mql1), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_match_media(f.w.d(), "(min-width: 200px)", 18, &mql2), GTEXT_CSS_OK);
  Events e;
  Hook h2{&e, "mql2"}, h1{&e, "mql1"};
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(mql2, hook, &h2), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(mql1, hook, &h1), GTEXT_CSS_OK);
  resize(f.w, 200, 100, keep);   // 100x100 => 200x100
  EXPECT_EQ(e.calls, (std::vector<std::string>{"mql1", "mql2"}));
  gtext_cssom_media_query_list_release(mql1);
  gtext_cssom_media_query_list_release(mql2);
}

TEST(CssomWptDocument, RemovingListenerFromOneMqlDoesNotRemoveItFromAllMqls) {
  Frame f(200, 200);
  std::vector<std::unique_ptr<Cond>> keep;
  GTEXT_CSSOM_Media_Query_List *mql1 = nullptr, *mql2 = nullptr;
  ASSERT_EQ(gtext_cssom_match_media(f.w.d(), "(max-height: 150px)", 19, &mql1), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_match_media(f.w.d(), "(max-height: 150px)", 19, &mql2), GTEXT_CSS_OK);
  Events e;
  Hook h{&e, "l"};
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(mql1, hook, &h), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_media_query_list_remove_listener(mql2, hook, &h), GTEXT_CSS_OK);
  resize(f.w, 200, 50, keep);   // 200x200 => 200x50
  EXPECT_EQ(e.calls.size(), 1u);
  gtext_cssom_media_query_list_release(mql1);
  gtext_cssom_media_query_list_release(mql2);
}

TEST(CssomWptDocument, RemoveListenerRemovesAddedListener) {
  Frame f(200, 200);
  std::vector<std::unique_ptr<Cond>> keep;
  GTEXT_CSSOM_Media_Query_List * mql = nullptr;
  ASSERT_EQ(gtext_cssom_match_media(f.w.d(), "(max-width: 200px)", 18, &mql), GTEXT_CSS_OK);
  Events e;
  Hook h{&e, "unreached"};
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(mql, hook, &h), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_media_query_list_remove_listener(mql, hook, &h), GTEXT_CSS_OK);
  resize(f.w, 300, 200, keep);
  EXPECT_TRUE(e.calls.empty()) << "should not be called";
  gtext_cssom_media_query_list_release(mql);
}

TEST(CssomWptDocument, TheChangeEventIsFiredInBothIdenticalMediaQueryListsWithTheSameMatchesValue) {
  Frame f(200, 200);
  std::vector<std::unique_ptr<Cond>> keep;
  GTEXT_CSSOM_Media_Query_List *mql = nullptr, *mql2 = nullptr;
  ASSERT_EQ(gtext_cssom_match_media(f.w.d(), "(max-width: 200px)", 18, &mql), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_match_media(f.w.d(), "(max-width: 200px)", 18, &mql2), GTEXT_CSS_OK);
  int change_events = 0;
  struct Check {
    int * count;
    GTEXT_CSSOM_Media_Query_List *a, *b;
  } check{&change_events, mql, mql2};
  auto fn = [](void * u, GTEXT_CSSOM_Media_Query_List *, bool) {
    Check * c = static_cast<Check *>(u);
    ++*c->count;
    bool ma = false, mb = true;
    EXPECT_EQ(gtext_cssom_media_query_list_matches(c->a, &ma), GTEXT_CSS_OK);
    EXPECT_EQ(gtext_cssom_media_query_list_matches(c->b, &mb), GTEXT_CSS_OK);
    EXPECT_EQ(ma, mb) << "Value of .matches should match";
    return GTEXT_CSS_OK;
  };
  // the page adds the same function to both; the pair (function, user) is what is unique here, so two users
  Check check2 = check;
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(mql, fn, &check), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_media_query_list_add_listener(mql2, fn, &check2), GTEXT_CSS_OK);
  resize(f.w, 300, 200, keep);
  EXPECT_EQ(change_events, 2) << "Should've fired the change event in both MediaQueryLists";
  gtext_cssom_media_query_list_release(mql);
  gtext_cssom_media_query_list_release(mql2);
}

/* ---- css/cssom/adoptedstylesheets-cascade-order.html (the document variants) ----------------------------------------------------------------- */

struct Cascade {
  World w;
  Cascade() : w(nullptr, nullptr, "<!DOCTYPE html><div id=target></div><div id=unrelated></div>") {}
  GTEXT_CSSOM_Scope * root() {
    return gtext_cssom_document_scope(w.d());
  }
  void adopt(std::initializer_list<GTEXT_CSSOM_Sheet *> sheets) {
    std::vector<GTEXT_CSSOM_Sheet *> v(sheets);
    ASSERT_EQ(gtext_cssom_scope_adopted_set(root(), v.data(), v.size(), nullptr), GTEXT_CSS_OK);
  }
  std::string color() {
    return w.value("#target", "color");
  }
};

TEST(CssomWptDocument, ReorderingDocumentAdoptedStyleSheetsUpdatesCascadeOrder) {
  Cascade c;
  Constructed red(*c.w.doc, "#target { color: red; }"), green(*c.w.doc, "#target { color: green; }");
  c.adopt({red.s, green.s});
  EXPECT_EQ(c.color(), "green");
  c.adopt({green.s, red.s});
  EXPECT_EQ(c.color(), "red");
  c.adopt({});
}

TEST(CssomWptDocument, ReorderingADocumentAdoptedStyleSheetsSuffixAfterAReplacementUpdatesCascadeOrder) {
  Cascade c;
  Constructed removed(*c.w.doc, "#unrelated { color: black; }"), blue(*c.w.doc, "#target { color: blue; }"), green(*c.w.doc, "#target { color: green; }"),
      inserted(*c.w.doc, "#also-unrelated { color: black; }");
  c.adopt({removed.s, blue.s, green.s});
  EXPECT_EQ(c.color(), "green");
  c.adopt({inserted.s, green.s, blue.s});
  EXPECT_EQ(c.color(), "blue");
  c.adopt({});
}

TEST(CssomWptDocument, RotatingDocumentAdoptedStyleSheetsUpdatesCascadeOrder) {
  Cascade c;
  Constructed red(*c.w.doc, "#target { color: red; }"), green(*c.w.doc, "#target { color: green; }"), blue(*c.w.doc, "#target { color: blue; }");
  c.adopt({red.s, green.s, blue.s});
  EXPECT_EQ(c.color(), "blue");
  c.adopt({green.s, blue.s, red.s});   // rotate left, which moves every sheet without adding or removing any
  EXPECT_EQ(c.color(), "red");
  c.adopt({});
}

TEST(CssomWptDocument, ReorderingAfterAnUnchangedLeadingAdoptedStylesheetUpdatesCascadeOrder) {
  Cascade c;
  Constructed unaffected(*c.w.doc, "#target { font-style: italic; }"), red(*c.w.doc, "#target { color: red; }"), green(*c.w.doc, "#target { color: green; }");
  c.adopt({unaffected.s, red.s, green.s});
  EXPECT_EQ(c.color(), "green");
  c.adopt({unaffected.s, green.s, red.s});   // the leading sheet is untouched, so only the reordered suffix matters
  EXPECT_EQ(c.color(), "red");
  EXPECT_EQ(c.w.value("#target", "font-style"), "italic");
  c.adopt({});
}

TEST(CssomWptDocument, ReorderingAroundAMutatedAdoptedStylesheetUpdatesCascadeOrder) {
  Cascade c;
  Constructed moved(*c.w.doc, "#target { color: green; }"), mutated(*c.w.doc, "#target { color: red; }");
  c.adopt({moved.s, mutated.s});
  EXPECT_EQ(c.color(), "red");
  // mutate one sheet and move the other past it in the same update, so that the moved sheet wins on account of the new order alone
  ASSERT_EQ(gtext_cssom_sheet_replace_sync(mutated.s, "#target { color: blue; }", 24, nullptr), GTEXT_CSS_OK);
  c.adopt({mutated.s, moved.s});
  EXPECT_EQ(c.color(), "green");
  // and back, so that the mutated sheet wins on account of the new order
  ASSERT_EQ(gtext_cssom_sheet_replace_sync(mutated.s, "#target { color: rebeccapurple; }", 34, nullptr), GTEXT_CSS_OK);
  c.adopt({moved.s, mutated.s});
  EXPECT_EQ(c.color(), "rebeccapurple");
  c.adopt({});
}

TEST(CssomWptDocument, ReorderingAdoptedStylesheetsWhileRemovingOneUpdatesCascadeOrder) {
  Cascade c;
  Constructed removed(*c.w.doc, "#unrelated { color: black; }"), red(*c.w.doc, "#target { color: red; }"), green(*c.w.doc, "#target { color: green; }");
  c.adopt({removed.s, red.s, green.s});
  EXPECT_EQ(c.color(), "green");
  c.adopt({green.s, red.s});
  EXPECT_EQ(c.color(), "red");
  c.adopt({});
}

TEST(CssomWptDocument, ReorderingAdoptedStylesheetsWhileInsertingOneUpdatesCascadeOrder) {
  Cascade c;
  Constructed inserted(*c.w.doc, "#unrelated { color: black; }"), red(*c.w.doc, "#target { color: red; }"), green(*c.w.doc, "#target { color: green; }");
  c.adopt({red.s, green.s});
  EXPECT_EQ(c.color(), "green");
  c.adopt({green.s, inserted.s, red.s});
  EXPECT_EQ(c.color(), "red");
  c.adopt({});
}

TEST(CssomWptDocument, ReorderingAdoptedStylesheetsWithMediaQueriesUpdatesCascadeOrder) {
  Cascade c;
  Constructed red(*c.w.doc, "@media (min-width: 1px) { #target { color: red; } }"), green(*c.w.doc, "@media (min-width: 1px) { #target { color: green; } }");
  c.adopt({red.s, green.s});
  EXPECT_EQ(c.color(), "green");
  c.adopt({green.s, red.s});
  EXPECT_EQ(c.color(), "red");
  c.adopt({});
}

TEST(CssomWptDocument, ReorderingDuplicatedAdoptedStylesheetsUpdatesCascadeOrder) {
  Cascade c;
  Constructed red(*c.w.doc, "#target { color: red; }"), green(*c.w.doc, "#target { color: green; }");
  c.adopt({red.s, green.s, red.s});   // the same sheet may be adopted more than once; the last one still wins
  EXPECT_EQ(c.color(), "red");
  c.adopt({green.s, red.s, green.s});
  EXPECT_EQ(c.color(), "green");
  c.adopt({});
}

TEST(CssomWptDocument, MutatingADuplicatedAdoptedStylesheetWhileInsertingALeadingSheetUpdatesCascadeOrder) {
  Cascade c;
  Constructed duplicated(*c.w.doc, "#target { color: red; }"), inserted(*c.w.doc, "#unrelated { color: black; }");
  c.adopt({duplicated.s, duplicated.s});
  EXPECT_EQ(c.color(), "red");
  ASSERT_EQ(gtext_cssom_sheet_replace_sync(duplicated.s, "#target { color: green; }", 25, nullptr), GTEXT_CSS_OK);
  c.adopt({inserted.s, duplicated.s, duplicated.s});
  EXPECT_EQ(c.color(), "green");
  c.adopt({});
}

/* ---- css/cssom/CSSStyleSheet-constructable-duplicate.html (the document variants) ---------------------------------------------------------------- */

TEST(CssomWptDocument, DuplicateStylesheetsHaveTheRightCascadePositionInTheDocument) {
  // Chromium 156.0.8078.47 and Firefox 158.0.1 both answer 1 for [one, zero, one] and 0 for [zero, one, zero]: the last position of a repeated sheet is its position
  Cascade c;
  Constructed zero(*c.w.doc, "div { z-index: 0; color: blue; }"), one(*c.w.doc, "div { z-index: 1; color: blue; }");
  c.adopt({one.s, zero.s, one.s});
  EXPECT_EQ(c.w.value("#target", "z-index"), "1") << "duplicate stylesheet should take right position in the cascade";
  c.adopt({zero.s, one.s, zero.s});
  EXPECT_EQ(c.w.value("#target", "z-index"), "0");
  c.adopt({});
}

TEST(CssomWptDocument, AppendingDuplicateStylesheetsYieldsTheCorrectCascadePositionInTheDocument) {
  Cascade c;
  Constructed zero(*c.w.doc, "div { z-index: 0; color: blue; }"), one(*c.w.doc, "div { z-index: 1; color: blue; }"), red(*c.w.doc, "div { color: red; }");
  c.adopt({one.s, zero.s});
  EXPECT_EQ(c.w.value("#target", "z-index"), "0") << "backmost stylesheet should take precedence";
  EXPECT_EQ(c.color(), "blue") << "backmost stylesheet should take precedence";
  c.adopt({one.s, zero.s, one.s, red.s});
  EXPECT_EQ(c.w.value("#target", "z-index"), "1") << "duplicate stylesheet should take the right position in the cascade";
  EXPECT_EQ(c.color(), "red") << "backmost stylesheet should take precedence";
  c.adopt({});
}

/* ---- css/cssom/adoptedstylesheets-modify-array-and-sheet.html ----------------------------------------------------------------------------------- */

TEST(CssomWptDocument, AdoptedStyleSheetsModifyArrayAndSheet) {
  Cascade c;
  Constructed a(*c.w.doc, "#target { color: red; }"), b(*c.w.doc, "#target {}");
  c.adopt({a.s, b.s});
  EXPECT_EQ(c.color(), "red") << "Add the two sheets. Text should be red.";
  c.adopt({b.s, a.s});   // document.adoptedStyleSheets[1] = sheet_a; [0] = sheet_b
  EXPECT_EQ(c.color(), "red") << "Flip the two sheet. Still red.";
  const GTEXT_CSSOM_Rule_List * rules = nullptr;
  ASSERT_EQ(gtext_cssom_sheet_rules(a.s, &rules), GTEXT_CSS_OK);
  Rule r = at(rules, 0);
  Decl d = style_of(r.r);
  ASSERT_EQ(set(d.d, "color", "green").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(c.color(), "green") << "Modify the color declaration. Should now be green.";
}

/* ---- css/cssom/CSSStyleSheet-template-adoption.html and CSSStyleSheet-constructable.html (the adoption checks) ------------------------------------- */

TEST(CssomWptDocument, AdoptingASheetConstructedByAnotherDocumentOrANonConstructedSheetIsANotAllowedError) {
  Cascade c;
  Cascade other;
  Constructed foreign(*other.w.doc, "#target { color: red; }");
  Sheet plain(*c.w.doc, "#target { color: red; }");
  GTEXT_CSSOM_Sheet * f[] = {foreign.s};
  GTEXT_CSSOM_Sheet * p[] = {plain.s};
  EXPECT_EQ(gtext_cssom_scope_adopted_set(c.root(), f, 1, nullptr), GTEXT_CSS_E_NOT_ALLOWED);
  EXPECT_EQ(gtext_cssom_scope_adopted_set(c.root(), p, 1, nullptr), GTEXT_CSS_E_NOT_ALLOWED);
  EXPECT_EQ(std::string(gtext_cssom_exception_name(GTEXT_CSS_E_NOT_ALLOWED)), "NotAllowedError");
  EXPECT_EQ(c.color(), "-");
}

}  // namespace
