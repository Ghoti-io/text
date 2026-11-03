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
 * `MediaList` (a sheet's, an `@import`'s and an `@media`'s) and constructed style sheets with `replace()` and `replaceSync()`: that a list is a view of its owner, that a text always
 * parses, what is ignored and what is an error, that a rule keeps its identity when its media change, and that replacing the rules of a sheet detaches every old rule, the nested ones
 * too, and changes nothing when it fails.
 */

#include <string>

#include <gtest/gtest.h>

#include "cssom_test_support.h"

using namespace cot;

namespace {

const GTEXT_CSSOM_Rule_List * rules_of(GTEXT_CSSOM_Sheet * s) {
  const GTEXT_CSSOM_Rule_List * l = nullptr;
  EXPECT_EQ(gtext_cssom_sheet_rules(s, &l), GTEXT_CSS_OK);
  return l;
}

struct Media {
  GTEXT_CSSOM_Media_List * m = nullptr;
  explicit Media(GTEXT_CSSOM_Media_List * made) : m(made) {}
  Media(const Media &) = delete;
  Media & operator=(const Media &) = delete;
  ~Media() {
    gtext_cssom_media_release(m);
  }
};

std::string text(GTEXT_CSSOM_Media_List * m) {
  char * out = nullptr;
  size_t n = 0;
  const GTEXT_CSS_Status st = gtext_cssom_media_text(m, &out, &n);
  if (st != GTEXT_CSS_OK) {
    return std::string("<") + gtext_css_status_string(st) + ">";
  }
  std::string s(out, n);
  gtext_cssom_buffer_free(&reinterpret_cast<css_obj *>(m)->core->alloc, out);
  return s;
}

size_t count(GTEXT_CSSOM_Media_List * m) {
  size_t n = 0;
  EXPECT_EQ(gtext_cssom_media_length(m, &n), GTEXT_CSS_OK);
  return n;
}

std::string item(GTEXT_CSSOM_Media_List * m, size_t i, bool * is_null = nullptr) {
  char * out = nullptr;
  size_t n = 0;
  bool nul = false;
  EXPECT_EQ(gtext_cssom_media_item(m, i, &out, &n, &nul), GTEXT_CSS_OK);
  if (is_null) {
    *is_null = nul;
  }
  std::string s(out, n);
  gtext_cssom_buffer_free(&reinterpret_cast<css_obj *>(m)->core->alloc, out);
  return s;
}

Result set_text(GTEXT_CSSOM_Media_List * m, const std::string & t) {
  Result r;
  r.st = gtext_cssom_media_set_text(m, t.data(), t.size(), &r.o);
  return r;
}

Result append(GTEXT_CSSOM_Media_List * m, const std::string & t) {
  Result r;
  r.st = gtext_cssom_media_append(m, t.data(), t.size(), &r.o);
  return r;
}

Result del(GTEXT_CSSOM_Media_List * m, const std::string & t) {
  Result r;
  r.st = gtext_cssom_media_delete(m, t.data(), t.size(), &r.o);
  return r;
}

Media sheet_media(GTEXT_CSSOM_Sheet * s) {
  GTEXT_CSSOM_Media_List * m = nullptr;
  EXPECT_EQ(gtext_cssom_sheet_media(s, &m), GTEXT_CSS_OK);
  return Media(m);
}

Media rule_media(GTEXT_CSSOM_Rule * r) {
  GTEXT_CSSOM_Media_List * m = nullptr;
  EXPECT_EQ(gtext_cssom_rule_media(r, &m), GTEXT_CSS_OK);
  return Media(m);
}

}  // namespace

TEST(CssomMedia, ASheetsListStartsAsTheOptionSaidAndIsTheSameObject) {
  Doc doc;
  GTEXT_CSSOM_Sheet_Options o = gtext_cssom_sheet_options_default();
  o.media = "screen,  (min-width:100px)";
  Sheet s(doc, "a{}", &o);
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  Media m = sheet_media(s.s);
  Media again = sheet_media(s.s);
  EXPECT_EQ(m.m, again.m) << "sheet.media === sheet.media";
  EXPECT_EQ(text(m.m), "screen, (min-width: 100px)") << "\"serialize a media query list\"";
  EXPECT_EQ(count(m.m), 2u);
  EXPECT_EQ(item(m.m, 0), "screen");
  EXPECT_EQ(item(m.m, 1), "(min-width: 100px)");
  bool nul = false;
  EXPECT_EQ(item(m.m, 2, &nul), "");
  EXPECT_TRUE(nul) << "item() past the end is null";
  Sheet plain(doc, "a{}");
  Media none = sheet_media(plain.s);
  EXPECT_EQ(text(none.m), "");
  EXPECT_EQ(count(none.m), 0u) << "the empty list, which matches everything";
}

TEST(CssomMedia, TheTextAlwaysParses) {
  Doc doc;
  Sheet s(doc, "a{}");
  Media m = sheet_media(s.s);
  const uint64_t v = gtext_cssom_sheet_version(s.s);
  doc.events.clear();
  Result r = set_text(m.m, "screen, (bogus: 1) and, print");
  ASSERT_EQ(r.st, GTEXT_CSS_OK);
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(text(m.m), "screen, not all, print") << "Media Queries 4, 3.2: a query that is not valid is `not all`";
  EXPECT_EQ(gtext_cssom_sheet_version(s.s), v + 1);
  ASSERT_EQ(doc.events.size(), 1u);
  EXPECT_EQ(doc.events[0].kind, GTEXT_CSSOM_EVENT_SHEET_MEDIA_CHANGED);
  EXPECT_EQ(set_text(m.m, "screen,not all,  print").o.kind, GTEXT_CSSOM_UNCHANGED) << "what the list already says is not a change";
  EXPECT_EQ(gtext_cssom_sheet_version(s.s), v + 1);
  EXPECT_EQ(doc.events.size(), 1u);
  EXPECT_EQ(set_text(m.m, "").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(text(m.m), "");
  EXPECT_EQ(set_text(m.m, "(color").o.kind, GTEXT_CSSOM_CHANGED) << "an unclosed ( ends at the end of the input";
  EXPECT_EQ(text(m.m), "(color)");
}

TEST(CssomMedia, AppendAndDeleteCompareBySerialization) {
  Doc doc;
  Sheet s(doc, "a{}");
  Media m = sheet_media(s.s);
  EXPECT_EQ(append(m.m, "screen").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(append(m.m, "  SCREEN ").o.kind, GTEXT_CSSOM_UNCHANGED) << "already in the list";
  EXPECT_EQ(append(m.m, "print and (min-width:1px)").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(text(m.m), "screen, print and (min-width: 1px)");
  // CSSOM 4.1: "To parse a media query for a given string s means to follow the parse a media query list steps and return null if more than one media query is returned", and
  // appendMedium(): "Let m be the result of parsing the given value. If m is null, then return."
  EXPECT_EQ(append(m.m, "screen, print").o.kind, GTEXT_CSSOM_UNCHANGED) << "two media: null, nothing happens";
  EXPECT_EQ(append(m.m, "").o.kind, GTEXT_CSSOM_UNCHANGED) << "none: null as well";
  EXPECT_EQ(text(m.m), "screen, print and (min-width: 1px)");
  // "A media query that ends up being 'ignored' will turn into 'not all'": it is a medium, and it is appended
  EXPECT_EQ(append(m.m, "(bogus: 1) and").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(text(m.m), "screen, print and (min-width: 1px), not all");
  EXPECT_EQ(append(m.m, "bogus!!").o.kind, GTEXT_CSSOM_UNCHANGED) << "and `not all` is already there";
  EXPECT_EQ(del(m.m, "screen, print").st, GTEXT_CSS_OK) << "deleteMedium(): \"If m is null, then return.\" (two media)";
  EXPECT_EQ(del(m.m, "").st, GTEXT_CSS_OK) << "(none)";
  EXPECT_EQ(text(m.m), "screen, print and (min-width: 1px), not all");
  EXPECT_EQ(del(m.m, "(bogus: 1) and").st, GTEXT_CSS_OK) << "an ignored query is `not all`, which the list holds";
  EXPECT_EQ(text(m.m), "screen, print and (min-width: 1px)");
  EXPECT_EQ(del(m.m, "bogus!!").st, GTEXT_CSS_E_NOT_FOUND) << "and now it holds none";
  EXPECT_EQ(del(m.m, "print and (min-width:1px)").st, GTEXT_CSS_OK);
  EXPECT_EQ(text(m.m), "screen");
  EXPECT_EQ(del(m.m, "tv").st, GTEXT_CSS_E_NOT_FOUND) << "\"throw a NotFoundError\"";
  EXPECT_STREQ(gtext_cssom_exception_name(GTEXT_CSS_E_NOT_FOUND), "NotFoundError");
  EXPECT_EQ(del(m.m, "screen").st, GTEXT_CSS_OK);
  EXPECT_EQ(text(m.m), "");
}

TEST(CssomMedia, AMediaRulesListIsItsPreludeAndTheRuleKeepsItsIdentity) {
  Doc doc;
  Sheet s(doc, "@media screen { a { color: red } b {} }");
  Rule r = path(s, {0});
  Rule a = path(s, {0, 0});
  Media m = rule_media(r.r);
  EXPECT_EQ(text(m.m), "screen");
  const uint64_t id = gtext_cssom_rule_id(r.r), v = gtext_cssom_rule_version(r.r);
  doc.events.clear();
  EXPECT_EQ(set_text(m.m, "print, (min-width: 10px)").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(css(r.r), "@media print, (min-width: 10px) {\n  a { color: red; }\n  b { }\n}");
  EXPECT_EQ(gtext_cssom_rule_id(r.r), id);
  EXPECT_EQ(gtext_cssom_rule_version(r.r), v + 1);
  EXPECT_EQ(path(s, {0, 0}).r, a.r) << "the rules inside are the same objects";
  ASSERT_EQ(doc.events.size(), 1u);
  EXPECT_EQ(doc.events[0].kind, GTEXT_CSSOM_EVENT_RULE_REPLACED);
  EXPECT_EQ(append(m.m, "tv").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(text(m.m), "print, (min-width: 10px), tv");
  EXPECT_EQ(set_text(m.m, "").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(css(r.r), "@media  {\n  a { color: red; }\n  b { }\n}");
  Media again = rule_media(r.r);
  EXPECT_EQ(again.m, m.m);
  Sheet t(doc, "a {}");
  Rule plain = path(t, {0});
  GTEXT_CSSOM_Media_List * none = nullptr;
  EXPECT_EQ(gtext_cssom_rule_media(plain.r, &none), GTEXT_CSS_E_UNSUPPORTED);
}

TEST(CssomMedia, AnImportsMediaAreTheQueriesAfterItsUrlLayerAndSupports) {
  Doc doc;
  Sheet s(doc, "@import url(a.css) layer(x) supports(display: grid) screen and (min-width: 1px); @import 'b.css'; a {}");
  Rule r = path(s, {0});
  Media m = rule_media(r.r);
  EXPECT_EQ(text(m.m), "screen and (min-width: 1px)");
  EXPECT_EQ(set_text(m.m, "print").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(css(r.r), "@import url(\"a.css\") layer(x) supports(display: grid) print;");
  EXPECT_EQ(set_text(m.m, "").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(css(r.r), "@import url(\"a.css\") layer(x) supports(display: grid);");
  Rule b = path(s, {1});
  Media bm = rule_media(b.r);
  EXPECT_EQ(text(bm.m), "");
  EXPECT_EQ(append(bm.m, "tv").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(css(b.r), "@import url(\"b.css\") tv;");
  EXPECT_EQ(gtext_cssom_rule_id(b.r), gtext_cssom_rule_id(path(s, {1}).r));
  EXPECT_EQ(shape(s), "import,import,style");
}

namespace {

std::string cond(GTEXT_CSSOM_Rule * r) {
  char * out = nullptr;
  size_t n = 0;
  bool nul = false;
  const GTEXT_CSS_Status st = gtext_cssom_rule_get_attribute(r, "conditionText", 13, &out, &n, &nul);
  if (st != GTEXT_CSS_OK) {
    return std::string("<") + gtext_css_status_string(st) + ">";
  }
  std::string s(out, n);
  gtext_cssom_buffer_free(&reinterpret_cast<css_obj *>(r)->core->alloc, out);
  return s;
}

}  // namespace

// Chromium 156 and Firefox 158 write the media of a list as they were read: a <general-enclosed> keeps its white space and comments, and an unclosed parenthesis is not closed. The
// rule stores the closed text (an unclosed `(` would take the `{` after it), so the library keeps both.
TEST(CssomMedia, AnUnclosedParenthesisIsWrittenAsItWasReadAndTheRuleKeepsItsBlock) {
  Doc doc;
  Sheet s(doc, "@media screen { a { color: red } b {} } c {}");
  Rule r = path(s, {0});
  Media m = rule_media(r.r);
  EXPECT_EQ(set_text(m.m, "screen and (").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(text(m.m), "screen and (");
  EXPECT_EQ(cond(r.r), "screen and (");
  EXPECT_EQ(css(r.r), "@media screen and ( {\n  a { color: red; }\n  b { }\n}") << "the block is still the rule's";
  EXPECT_EQ(shape(s), "media[style,style],style");
  EXPECT_NE(path(s, {0, 1}).r, nullptr);
  EXPECT_EQ(count(m.m), 1u);
  EXPECT_EQ(item(m.m, 0), "screen and (");
  // a list that is read the same is not a change, and one that is closed the same way is: the text a script reads moves
  EXPECT_EQ(set_text(m.m, "screen and (").o.kind, GTEXT_CSSOM_UNCHANGED);
  EXPECT_EQ(set_text(m.m, "screen and ()").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(text(m.m), "screen and ()");
  EXPECT_EQ(css(r.r), "@media screen and () {\n  a { color: red; }\n  b { }\n}");
  // an ordinary text ends it
  EXPECT_EQ(set_text(m.m, "(a (b ( c").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(text(m.m), "(a (b ( c");
  EXPECT_EQ(set_text(m.m, "print").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(text(m.m), "print");
  EXPECT_EQ(css(r.r), "@media print {\n  a { color: red; }\n  b { }\n}");
  // the white space and the comments of a leaf, and its escapes
  EXPECT_EQ(set_text(m.m, "screen and ( a ), print and (x/**/y)").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(text(m.m), "screen and ( a ), print and (x/**/y)");
  EXPECT_EQ(count(m.m), 2u);
  EXPECT_EQ(item(m.m, 0), "screen and ( a )");
  EXPECT_EQ(item(m.m, 1), "print and (x/**/y)");
  EXPECT_EQ(css(r.r), "@media screen and ( a ), print and (x/**/y) {\n  a { color: red; }\n  b { }\n}");
  EXPECT_EQ(set_text(m.m, "screen and (a)").o.kind, GTEXT_CSSOM_CHANGED) << "(a) is not ( a )";
  EXPECT_EQ(text(m.m), "screen and (a)");
  EXPECT_EQ(set_text(m.m, "x and ( a\\ b )").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(text(m.m), "x and ( a\\ b )");
}

TEST(CssomMedia, AnUnclosedParenthesisInTheMediaOfASheetAnImportAndAConstructedSheet) {
  Doc doc;
  Sheet s(doc, "@import url(a.css) print; @media screen { a {} }");
  Media sm = sheet_media(s.s);
  EXPECT_EQ(set_text(sm.m, "tv and (a:b").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(text(sm.m), "tv and (a:b");
  EXPECT_EQ(count(sm.m), 1u);
  EXPECT_EQ(set_text(sm.m, "tv and (a:b").o.kind, GTEXT_CSSOM_UNCHANGED);
  EXPECT_EQ(set_text(sm.m, "tv and (a:b)").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(text(sm.m), "tv and (a:b)");
  Rule imp = path(s, {0});
  Media im = rule_media(imp.r);
  EXPECT_EQ(set_text(im.m, "screen and (").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(text(im.m), "screen and (");
  EXPECT_EQ(css(imp.r), "@import url(\"a.css\") screen and (;");
  EXPECT_EQ(shape(s), "import,media[style]") << "the semicolon is the rule's";
  EXPECT_EQ(append(im.m, "tv").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(css(imp.r), "@import url(\"a.css\") screen and (, tv;");
  EXPECT_EQ(set_text(im.m, "tv").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(css(imp.r), "@import url(\"a.css\") tv;");
  GTEXT_CSSOM_Sheet_Options o = gtext_cssom_sheet_options_default();
  o.media = "screen and ( a ), (b (c";
  Sheet made(doc, "a{}", &o);
  ASSERT_EQ(made.status, GTEXT_CSS_OK);
  Media mm = sheet_media(made.s);
  EXPECT_EQ(text(mm.m), "screen and ( a ), (b (c") << "the option of a new sheet is read as a script sets it";
  EXPECT_EQ(count(mm.m), 2u);
}

TEST(CssomMedia, AppendAndDeleteCompareMediaAsTheyWereRead) {
  Doc doc;
  Sheet s(doc, "@media screen, print { a {} }");
  Rule r = path(s, {0});
  Media m = rule_media(r.r);
  EXPECT_EQ(set_text(m.m, "print, screen and (").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(append(m.m, "tv").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(text(m.m), "print, screen and (, tv") << "the unclosed query is a query of its own, and the new one follows it";
  EXPECT_EQ(count(m.m), 3u);
  EXPECT_EQ(item(m.m, 1), "screen and (");
  EXPECT_EQ(append(m.m, "screen and (").o.kind, GTEXT_CSSOM_UNCHANGED) << "the same medium as it was read";
  EXPECT_EQ(append(m.m, "screen and ()").o.kind, GTEXT_CSSOM_CHANGED) << "the closed one is another medium";
  EXPECT_EQ(text(m.m), "print, screen and (, tv, screen and ()");
  EXPECT_EQ(css(r.r), "@media print, screen and (, tv, screen and () {\n  a { }\n}");
  EXPECT_EQ(del(m.m, "screen and ()").st, GTEXT_CSS_OK);
  EXPECT_EQ(text(m.m), "print, screen and (, tv");
  EXPECT_EQ(del(m.m, "tv").st, GTEXT_CSS_OK);
  EXPECT_EQ(text(m.m), "print, screen and (");
  EXPECT_EQ(del(m.m, "print").st, GTEXT_CSS_OK);
  EXPECT_EQ(text(m.m), "screen and (") << "what is left keeps the way it was read";
  EXPECT_EQ(del(m.m, "screen and ()").st, GTEXT_CSS_E_NOT_FOUND) << "closed, it is another medium";
  EXPECT_EQ(del(m.m, "screen and (").st, GTEXT_CSS_OK);
  EXPECT_EQ(text(m.m), "");
  EXPECT_EQ(css(r.r), "@media  {\n  a { }\n}");
  // two whose white space differs are two media; the second one does not match the first when they are read
  EXPECT_EQ(set_text(m.m, "(a  b)").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(append(m.m, "(a b)").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(text(m.m), "(a  b), (a b)");
}

// The queries are told apart in the canonical text: a comma or a parenthesis in a string or after a backslash is not one.
TEST(CssomMedia, ALeafWithAStringOrAnEscapeIsOneQuery) {
  Doc doc;
  Sheet s(doc, "@media screen { a {} }");
  Rule r = path(s, {0});
  Media m = rule_media(r.r);
  EXPECT_EQ(set_text(m.m, "x and (a\\), b), print").st, GTEXT_CSS_OK);
  EXPECT_EQ(count(m.m), 2u) << "an escaped parenthesis closes nothing, and the comma after it is inside the leaf";
  EXPECT_EQ(item(m.m, 0), "x and (a\\), b)");
  EXPECT_EQ(set_text(m.m, "x and (a: \", ) b\"), print").st, GTEXT_CSS_OK);
  EXPECT_EQ(count(m.m), 2u) << "a comma and a parenthesis in a string";
  EXPECT_EQ(append(m.m, "tv").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(text(m.m), "x and (a: \", ) b\"), print, tv");
  EXPECT_EQ(del(m.m, "print").st, GTEXT_CSS_OK);
  EXPECT_EQ(text(m.m), "x and (a: \", ) b\"), tv");
}

TEST(CssomMedia, AListFollowsItsOwnerAndOutlivesIt) {
  Doc doc;
  GTEXT_CSSOM_Media_List * held = nullptr;
  {
    Sheet s(doc, "@media screen { }");
    Rule r = path(s, {0});
    ASSERT_EQ(gtext_cssom_rule_media(r.r, &held), GTEXT_CSS_OK);
    ASSERT_EQ(delete_rule(s.s, 0), GTEXT_CSS_OK);
    EXPECT_EQ(set_text(held, "print").o.kind, GTEXT_CSSOM_CHANGED) << "a removed rule's list still works, and affects no sheet";
    EXPECT_EQ(css(r.r), "@media print {\n}");
  }
  EXPECT_EQ(text(held), "print") << "the list keeps its rule";
  gtext_cssom_media_release(held);
}

TEST(CssomMedia, ASheetThatIsNotOriginCleanRefusesAChange) {
  Doc doc;
  GTEXT_CSSOM_Sheet_Options o = gtext_cssom_sheet_options_default();
  o.origin_clean = false;
  Sheet s(doc, "a{}", &o);
  Media m = sheet_media(s.s);
  EXPECT_EQ(set_text(m.m, "print").st, GTEXT_CSS_E_SECURITY);
}

TEST(CssomMedia, AChangeFromInsideACallbackIsRefused) {
  Doc doc;
  Sheet s(doc, "a{}");
  Media m = sheet_media(s.s);
  Result inner;
  doc.inside = [&]() {
    inner = set_text(m.m, "print");
    return GTEXT_CSS_OK;
  };
  ASSERT_EQ(set_text(m.m, "screen").st, GTEXT_CSS_OK);
  EXPECT_EQ(inner.st, GTEXT_CSS_E_REENTRANT);
  EXPECT_EQ(text(m.m), "screen");
}

/* ---- constructed sheets ----------------------------------------------------------------------------------------------------------------------- */

namespace {

struct Constructed {
  GTEXT_CSSOM_Sheet * s = nullptr;
  GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;
  explicit Constructed(Doc & doc, const GTEXT_CSSOM_Sheet_Options * o = nullptr) {
    status = gtext_cssom_sheet_new_constructed(doc.d, o, &s);
  }
  Constructed(const Constructed &) = delete;
  Constructed & operator=(const Constructed &) = delete;
  ~Constructed() {
    gtext_cssom_sheet_release(s);
  }
};

Result replace_sync(GTEXT_CSSOM_Sheet * s, const std::string & t) {
  Result r;
  r.st = gtext_cssom_sheet_replace_sync(s, t.data(), t.size(), &r.o);
  return r;
}

}  // namespace

TEST(CssomConstructed, ANewSheetIsEmptyConstructedAndHasNoOwner) {
  Doc doc;
  GTEXT_CSSOM_Sheet_Options o = gtext_cssom_sheet_options_default();
  o.media = "print";
  o.disabled = true;
  o.base_url = "http://x/";
  o.owner_node = &o;
  Constructed c(doc, &o);
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_TRUE(gtext_cssom_sheet_constructed(c.s));
  EXPECT_TRUE(gtext_cssom_sheet_disabled(c.s));
  EXPECT_EQ(gtext_cssom_sheet_owner_node(c.s), nullptr) << "a constructed sheet has no owner node";
  EXPECT_EQ(gtext_cssom_rule_list_length(rules_of(c.s)), 0u);
  Media m = sheet_media(c.s);
  EXPECT_EQ(text(m.m), "print");
  Sheet parsed(doc, "a{}");
  EXPECT_FALSE(gtext_cssom_sheet_constructed(parsed.s));
}

TEST(CssomConstructed, ReplaceSyncDropsImportsAndDetachesEveryOldRule) {
  Doc doc;
  Constructed c(doc);
  ASSERT_EQ(replace_sync(c.s, "a { b { color: red } } @media print { c {} }").st, GTEXT_CSS_OK);
  const GTEXT_CSSOM_Rule_List * l = rules_of(c.s);
  Rule a = at(l, 0);
  Rule b = at(gtext_cssom_rule_css_rules(a.r), 0);
  Rule media = at(l, 1);
  Rule inner = at(gtext_cssom_rule_css_rules(media.r), 0);
  EXPECT_EQ(shape(l), "style[style],media[style]");
  const uint64_t v = gtext_cssom_sheet_version(c.s);
  doc.events.clear();
  Result r = replace_sync(c.s, "@import 'x.css'; x { } y { }");
  ASSERT_EQ(r.st, GTEXT_CSS_OK);
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(shape(rules_of(c.s)), "style,style") << "\"@import rules are ignored\"";
  EXPECT_EQ(gtext_cssom_sheet_diagnostic_total(c.s), 1u) << "a diagnostic for the import";
  for (Rule * old : {&a, &b, &media, &inner}) {
    GTEXT_CSSOM_Sheet * ps = gtext_cssom_rule_parent_sheet(old->r);
    EXPECT_EQ(ps, nullptr) << "parentStyleSheet is null at once, for the nested rules too";
    EXPECT_TRUE(gtext_cssom_rule_detached(old->r));
  }
  GTEXT_CSSOM_Rule * parent = gtext_cssom_rule_parent_rule(b.r);
  EXPECT_EQ(parent, a.r) << "a nested rule keeps its parent rule, which is itself detached";
  gtext_cssom_rule_release(parent);
  EXPECT_EQ(css(a.r), "a {\n  & b { color: red; }\n}") << "a detached rule is still readable";
  EXPECT_EQ(gtext_cssom_sheet_version(c.s), v + 1);
  ASSERT_EQ(doc.events.size(), 1u);
  EXPECT_EQ(doc.events[0].kind, GTEXT_CSSOM_EVENT_RULE_REPLACED);
  EXPECT_EQ(doc.events[0].sheet_id, gtext_cssom_sheet_id(c.s));
  EXPECT_EQ(doc.events[0].rule_id, 0u) << "the event is about all of the sheet's rules";
  Rule x = at(rules_of(c.s), 0);
  GTEXT_CSSOM_Sheet * ps = gtext_cssom_rule_parent_sheet(x.r);
  EXPECT_EQ(ps, c.s);
  gtext_cssom_sheet_release(ps);
}

TEST(CssomConstructed, ReplaceSyncIsOnlyForAConstructedSheet) {
  Doc doc;
  Sheet s(doc, "a{}");
  const std::string before = dump(s.s);
  Result r = replace_sync(s.s, "b{}");
  EXPECT_EQ(r.st, GTEXT_CSS_E_NOT_ALLOWED) << "\"throw a NotAllowedError\"";
  EXPECT_STREQ(gtext_cssom_exception_name(r.st), "NotAllowedError");
  EXPECT_EQ(dump(s.s), before);
}

TEST(CssomConstructed, ReplaceHasATwoStepFormAndForbidsModificationBetween) {
  Doc doc;
  Constructed c(doc);
  ASSERT_EQ(replace_sync(c.s, "a{}").st, GTEXT_CSS_OK);
  EXPECT_FALSE(gtext_cssom_sheet_replace_pending(c.s));
  ASSERT_EQ(gtext_cssom_sheet_replace_begin(c.s), GTEXT_CSS_OK);
  EXPECT_TRUE(gtext_cssom_sheet_replace_pending(c.s));
  EXPECT_EQ(gtext_cssom_sheet_replace_begin(c.s), GTEXT_CSS_E_NOT_ALLOWED) << "a second replace while one is pending";
  EXPECT_EQ(replace_sync(c.s, "z{}").st, GTEXT_CSS_E_NOT_ALLOWED);
  EXPECT_EQ(insert_rule(c.s, "b{}", 0).st, GTEXT_CSS_E_NOT_ALLOWED) << "\"disallow modification\"";
  EXPECT_EQ(delete_rule(c.s, 0), GTEXT_CSS_E_NOT_ALLOWED);
  EXPECT_EQ(shape(rules_of(c.s)), "style");
  Result r;
  r.st = gtext_cssom_sheet_replace_complete(c.s, "p{} q{}", 7, &r.o);
  ASSERT_EQ(r.st, GTEXT_CSS_OK);
  EXPECT_EQ(shape(rules_of(c.s)), "style,style");
  EXPECT_FALSE(gtext_cssom_sheet_replace_pending(c.s));
  EXPECT_EQ(insert_rule(c.s, "b{}", 0).st, GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_sheet_replace_complete(c.s, "", 0, nullptr), GTEXT_CSS_E_INVALID_STATE) << "nothing was begun";
  // a replace whose text cannot be read still ends the pending state, and leaves the rules
  ASSERT_EQ(gtext_cssom_sheet_replace_begin(c.s), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_sheet_replace_complete(c.s, "a{}", 3, nullptr), GTEXT_CSS_OK);
  EXPECT_FALSE(gtext_cssom_sheet_replace_pending(c.s));
}

TEST(CssomConstructed, ASheetReplacedWithTheSameTextGetsNewRules) {
  Doc doc;
  Constructed c(doc);
  ASSERT_EQ(replace_sync(c.s, "a{}").st, GTEXT_CSS_OK);
  Rule first = at(rules_of(c.s), 0);
  ASSERT_EQ(replace_sync(c.s, "a{}").st, GTEXT_CSS_OK);
  Rule second = at(rules_of(c.s), 0);
  EXPECT_NE(first.r, second.r);
  EXPECT_NE(gtext_cssom_rule_id(first.r), gtext_cssom_rule_id(second.r));
  EXPECT_TRUE(gtext_cssom_rule_detached(first.r));
  EXPECT_FALSE(gtext_cssom_rule_detached(second.r));
}

TEST(CssomConstructed, AnImportCannotBeInsertedIntoAConstructedSheet) {
  Doc doc;
  Constructed c(doc);
  EXPECT_EQ(insert_rule(c.s, "@import 'a.css'", 0).st, GTEXT_CSS_E_SYNTAX);
}
