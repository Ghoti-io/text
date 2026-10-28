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
 * The matcher (css_match.h) and the Markup queries (markup_query.h), each test named for the rule it holds and quoting the sentence of the served drafts that decides it
 * where it is short. The tree is parsed from HTML or XML text and queried by selector, so every test goes through the adapter and the queries as a caller does.
 */

#include <string>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>
#include <ghoti.io/text/markup/markup_query.h>

#include "css_match_test_support.h"

using cmatch::all;
using cmatch::Doc;
using cmatch::matches;
using cmatch::within;

namespace {

const char * const kPage = R"(<!DOCTYPE html>
<html lang="en"><head><title>t</title></head>
<body id="body">
<div id="root" class="a b">
 <p id="p1" class="x">one</p>
 <p id="p2" class="x y" title="Hello World">two</p>
 <span id="s1" data-k="a-b" data-t="Foo">s</span>
 <p id="p3">three <b id="b1">b</b></p>
 <ul id="ul"><li id="l1" class="x">1</li><li id="l2">2</li><li id="l3" class="x">3</li><li id="l4">4</li><li id="l5" class="x">5</li></ul>
 <div id="empty"></div>
 <div id="wsonly"> </div>
 <a id="a1" href="u">link</a><a id="a2">nolink</a>
</div>
<form id="f"><input id="i1" type="checkbox" checked><input id="i2" type="TEXT" disabled required placeholder="p"><fieldset id="fs" disabled><legend id="lg"><input id="i3"></legend><input id="i4"></fieldset><input id="i5" readonly></form>
<my-el id="ce"></my-el>
<p id="pde" lang="de-CH">de</p><p id="pun" lang="">un</p>
<div id="rtl" dir="rtl"><p id="pr">x</p></div><p id="pa" dir="auto">&#x5E9;&#x5DC;&#x5D5;&#x5DD;</p>
</body></html>
)";

class CssMatch : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(d.html(kPage));
  }
  Doc d;
};

}  // namespace

TEST_F(CssMatch, TypeClassIdAndAttributeSelectors) {
  EXPECT_EQ(all(d, ".x"), "p1 p2 l1 l3 l5");
  EXPECT_EQ(all(d, "p.x"), "p1 p2");
  EXPECT_EQ(all(d, ".x.y"), "p2");
  EXPECT_EQ(all(d, "#p3"), "p3");
  EXPECT_EQ(all(d, "div#root.a.b"), "root");
  EXPECT_EQ(all(d, "[title]"), "p2");
  EXPECT_EQ(all(d, "[title=\"Hello World\"]"), "p2");
  EXPECT_EQ(all(d, "[title|=Hello]"), "") << "\"Hello World\" is neither \"Hello\" nor \"Hello-\" and more";
  EXPECT_EQ(all(d, "[data-k|=a]"), "s1");
  EXPECT_EQ(all(d, "[title~=World]"), "p2");
  EXPECT_EQ(all(d, "[title~=\"Hello World\"]"), "") << "a word has no white space";
  EXPECT_EQ(all(d, "[title^=Hel]"), "p2");
  EXPECT_EQ(all(d, "[title$=ld]"), "p2");
  EXPECT_EQ(all(d, "[title*=\"o W\"]"), "p2");
  EXPECT_EQ(all(d, "[title*=\"\"]"), "") << "\"If the value is the empty string then the selector does not represent anything\"";
  EXPECT_EQ(all(d, "[title^=\"\"]"), "");
  EXPECT_EQ(all(d, "[title$=\"\"]"), "");
  EXPECT_EQ(all(d, "[class~=\"\"]"), "");
  EXPECT_EQ(all(d, "*|p.x"), "p1 p2");
  EXPECT_EQ(all(d, "|p"), "") << "no namespace: an HTML element is in the HTML namespace";
}

TEST_F(CssMatch, TheHtmlCaseRulesForNamesAndTheValuesOfAListedAttribute) {
  // HTML Standard, "Case-sensitivity of selectors": the type selector and the attribute name are lowercased for an HTML element in an HTML document
  EXPECT_EQ(all(d, "P.x"), "p1 p2");
  EXPECT_EQ(all(d, "[TITLE]"), "p2");
  EXPECT_EQ(all(d, "[data-t=foo]"), "") << "data-t is not on the list of attributes whose values are compared ignoring case";
  EXPECT_EQ(all(d, "[data-t=foo i]"), "s1");
  EXPECT_EQ(all(d, "[data-t=Foo s]"), "s1");
  EXPECT_EQ(all(d, "[data-t=FOO s]"), "");
  EXPECT_EQ(all(d, "input[type=text]"), "i2") << "type is on the list, and the value is TEXT";
  EXPECT_EQ(all(d, "input[type=text s]"), "") << "\"An explicit flag, such as s, can force case-sensitive matching\"";
  EXPECT_EQ(all(d, "input[type=TEXT]"), "i2");
  EXPECT_EQ(all(d, ".X"), "") << "classes are case-sensitive in standards mode";
  EXPECT_EQ(all(d, "#ROOT"), "");
  Doc x;
  ASSERT_TRUE(x.xml("<r><P id=\"u\"/><p id=\"l\" Foo=\"1\"/></r>"));
  EXPECT_EQ(all(x, "p"), "l") << "an XML document compares names exactly";
  EXPECT_EQ(all(x, "P"), "u");
  EXPECT_EQ(all(x, "[foo]"), "");
  EXPECT_EQ(all(x, "[Foo]"), "l");
}

TEST_F(CssMatch, QuirksModeCompareClassAndIdIgnoringCaseAndStandardsModeDoesNot) {
  Doc q;
  ASSERT_TRUE(q.html("<html><body><div id=\"Box\" class=\"Foo bar\"></div></body></html>"));
  EXPECT_EQ(gtext_markup_document_mode(q.doc), GTEXT_MARKUP_MODE_QUIRKS) << "no doctype";
  EXPECT_EQ(all(q, "#box"), "Box");
  EXPECT_EQ(all(q, ".foo"), "Box");
  EXPECT_EQ(all(q, "[class=\"foo bar\"]"), "") << "\"This doesn't apply to [id] or [class] attribute selectors\"";
  EXPECT_EQ(all(d, "#BODY"), "");
}

TEST_F(CssMatch, Combinators) {
  EXPECT_EQ(within(d, "root", ":scope > p"), "p1 p2 p3");
  EXPECT_EQ(all(d, "#root > p"), "p1 p2 p3");
  EXPECT_EQ(all(d, "#root p"), "p1 p2 p3");
  EXPECT_EQ(all(d, "body p"), "p1 p2 p3 pde pun pr pa");
  EXPECT_EQ(all(d, "div > p"), "p1 p2 p3 pr");
  EXPECT_EQ(all(d, "p + span"), "s1");
  EXPECT_EQ(all(d, "#p1 + p"), "p2");
  EXPECT_EQ(all(d, "#p1 ~ p"), "p2 p3");
  EXPECT_EQ(all(d, "#p2 ~ *"), "s1 p3 ul empty wsonly a1 a2");
  EXPECT_EQ(all(d, "a + a"), "a2");
  EXPECT_EQ(all(d, "html body div ul li"), "l1 l2 l3 l4 l5");
  EXPECT_EQ(all(d, "div ul > li"), "l1 l2 l3 l4 l5");
  EXPECT_EQ(all(d, "#root > li"), "") << "an li is not a child of the root";
  EXPECT_EQ(all(d, "p ~ p + a"), "") << "after a p comes a span";
  EXPECT_EQ(all(d, "span ~ ul > li + li"), "l2 l3 l4 l5");
  EXPECT_EQ(all(d, "a || b"), "") << "\"the column combinator ... never matches\" here: no tree interface has columns";
}

TEST(CssMatchMemo, AnAnswerKeptForAnAncestorIsNotWrittenOverTheOnesAboveIt) {
  // found by the fuzz harness with a naive reference: the climb past an ancestor whose answer was already kept stored its own answer for every element above that one too
  Doc d;
  ASSERT_TRUE(d.html("<!DOCTYPE html><c><p id=\"p\"><a id=\"a\"><b id=\"b1\"><b id=\"b2\"></b></b></a><b id=\"b3\"></b></p></c>"));
  EXPECT_EQ(all(d, "a b"), "b1 b2");
  EXPECT_EQ(all(d, ":is(a b)"), "b1 b2");
  EXPECT_EQ(all(d, "a ~ b"), "b3") << "the same climb along the earlier siblings";
  EXPECT_EQ(all(d, "a ~ b ~ b"), "");
  Doc s;
  ASSERT_TRUE(s.html("<!DOCTYPE html><x><a id=\"a\"></a><b id=\"b1\"></b><b id=\"b2\"></b></x><x><b id=\"b3\"></b><b id=\"b4\"></b></x>"));
  EXPECT_EQ(all(s, "a ~ b"), "b1 b2");
  EXPECT_EQ(all(s, "a ~ b ~ b"), "b2");
}

TEST_F(CssMatch, StructuralPseudoClasses) {
  EXPECT_EQ(all(d, "li:first-child"), "l1");
  EXPECT_EQ(all(d, "li:last-child"), "l5");
  EXPECT_EQ(all(d, "li:only-child"), "");
  EXPECT_EQ(all(d, "#pr:only-child"), "pr");
  EXPECT_EQ(all(d, "li:nth-child(2n+1)"), "l1 l3 l5");
  EXPECT_EQ(all(d, "li:nth-child(even)"), "l2 l4");
  EXPECT_EQ(all(d, "li:nth-child(-n+2)"), "l1 l2");
  EXPECT_EQ(all(d, "li:nth-child(n+4)"), "l4 l5");
  EXPECT_EQ(all(d, "li:nth-child(0n+3)"), "l3");
  EXPECT_EQ(all(d, "li:nth-child(3n-1)"), "l2 l5");
  EXPECT_EQ(all(d, "li:nth-last-child(1)"), "l5");
  EXPECT_EQ(all(d, "li:nth-last-child(2n)"), "l2 l4");
  EXPECT_EQ(all(d, "li:nth-child(2n+1 of .x)"), "l1 l5") << "positions among the siblings that match S: l1, l3, l5 are 1, 2, 3";
  EXPECT_EQ(all(d, "li:nth-child(2 of .x)"), "l3");
  EXPECT_EQ(all(d, "li:nth-last-child(1 of .x)"), "l5");
  EXPECT_EQ(all(d, "li:nth-child(1 of :not(.x))"), "l2");
  EXPECT_EQ(all(d, "li:nth-child(1 of .x, #l4)"), "l1");
  EXPECT_EQ(all(d, ":nth-child(1 of .x)"), "p1 l1") << "an element that does not match S is not counted and has no position: the others in the document order";
  EXPECT_EQ(all(d, "li:nth-of-type(3)"), "l3");
  EXPECT_EQ(all(d, "#root > p:nth-of-type(2)"), "p2");
  EXPECT_EQ(all(d, "#root > p:nth-last-of-type(1)"), "p3");
  EXPECT_EQ(all(d, "#root > p:first-of-type"), "p1");
  EXPECT_EQ(all(d, "#root > p:last-of-type"), "p3");
  EXPECT_EQ(all(d, "#root span:only-of-type"), "s1");
  EXPECT_EQ(all(d, "#root > div:only-of-type"), "") << "two divs below the root";
  EXPECT_EQ(all(d, ":root"), "<html>");
  EXPECT_EQ(all(d, "div:empty"), "empty") << "\"only element nodes and content nodes whose data has a non-zero length must be considered as affecting emptiness\"";
  EXPECT_EQ(all(d, "#wsonly:empty"), "");
  EXPECT_EQ(all(d, "html:first-child"), "<html>") << "the root element is first among its (one) siblings";
}

TEST_F(CssMatch, LogicalPseudoClasses) {
  EXPECT_EQ(all(d, ":is(p, li).x"), "p1 p2 l1 l3 l5");
  EXPECT_EQ(all(d, ":where(p, li):not(.x)"), "p3 l2 l4 pde pun pr pa");
  EXPECT_EQ(all(d, "li:not(.x, #l2)"), "l4");
  EXPECT_EQ(all(d, ":is()"), "");
  EXPECT_EQ(all(d, "li:is(.x, ::bogus)"), "l1 l3 l5") << "the entry that is not a selector never matches";
  EXPECT_EQ(all(d, "li:not(:is(.x))"), "l2 l4");
  EXPECT_EQ(all(d, ":is(ul, ol) > li:nth-child(1)"), "l1");
  EXPECT_EQ(all(d, ":not(:not(#l1))#l1"), "l1");
  EXPECT_EQ(all(d, ":is(div p, section p) b"), "b1");
}

TEST_F(CssMatch, ScopeIsTheElementTheQueryIsOnAndTheRootElseWhere) {
  EXPECT_EQ(within(d, "root", ":scope > p"), "p1 p2 p3");
  EXPECT_EQ(within(d, "root", ":scope"), "") << "the results are descendants, not the scope itself";
  EXPECT_EQ(all(d, ":scope"), "<html>") << "\"if there is no scoping root, :scope is :root\"";
  EXPECT_EQ(within(d, "ul", ":scope > li:first-child"), "l1");
  EXPECT_EQ(within(d, "ul", "div :scope > li"), "l1 l2 l3 l4 l5") << "a selector may reach above the scope; only its results may not";
  EXPECT_EQ(within(d, "ul", ":not(:scope) li"), "l1 l2 l3 l4 l5");
  const GTEXT_Markup_Node * el = d.at("l3");
  EXPECT_TRUE(matches(el, ":scope"));
  EXPECT_TRUE(matches(el, ":scope.x"));
  EXPECT_TRUE(matches(el, "ul > :scope"));
  EXPECT_FALSE(matches(el, "ul > :scope + li")) << "matches() has :scope = the element itself, and l3 follows l2";
  GTEXT_Markup_Node * out = nullptr;
  ASSERT_EQ(gtext_markup_closest(el, ":scope", 6, nullptr, &out, nullptr), GTEXT_MARKUP_OK);
  EXPECT_EQ(out, el);
}

TEST_F(CssMatch, ClosestIsInclusiveOfTheElementAndMatchesAndNoMatchAreAnswers) {
  GTEXT_Markup_Node * out = nullptr;
  const GTEXT_Markup_Node * l3 = d.at("l3");
  ASSERT_EQ(gtext_markup_closest(l3, "li", 2, nullptr, &out, nullptr), GTEXT_MARKUP_OK);
  EXPECT_EQ(out, l3) << "\"closest\" of a matching element is itself";
  ASSERT_EQ(gtext_markup_closest(l3, "ul", 2, nullptr, &out, nullptr), GTEXT_MARKUP_OK);
  EXPECT_EQ(cmatch::id_of(out), "ul");
  ASSERT_EQ(gtext_markup_closest(l3, "div", 3, nullptr, &out, nullptr), GTEXT_MARKUP_OK);
  EXPECT_EQ(cmatch::id_of(out), "root");
  ASSERT_EQ(gtext_markup_closest(l3, "table", 5, nullptr, &out, nullptr), GTEXT_MARKUP_OK);
  EXPECT_EQ(out, nullptr);
  ASSERT_EQ(gtext_markup_closest(l3, "#root > ul > :scope", 19, nullptr, &out, nullptr), GTEXT_MARKUP_OK);
  EXPECT_EQ(out, l3);
  EXPECT_TRUE(matches(l3, "li.x"));
  EXPECT_FALSE(matches(l3, "li.y"));
  EXPECT_FALSE(matches(l3, "li::before")) << "a pseudo-element never matches in a query";
  EXPECT_FALSE(matches(l3, "li:visited"));
}

TEST_F(CssMatch, SyntaxErrorsAreReportedAsAnErrorWithTheDiagnosticAndNothingElse) {
  GTEXT_Markup_Node * out = nullptr;
  GTEXT_Markup_Query_Result r;
  std::memset(&r, 0, sizeof r);
  EXPECT_EQ(gtext_markup_query_selector(d.root(), "p:", 2, nullptr, &out, &r), GTEXT_MARKUP_E_SELECTOR);
  EXPECT_EQ(out, nullptr);
  EXPECT_GE(gtext_css_selector_result_diagnostic_count(&r.selector), 1u);
  gtext_markup_query_result_free(&r);
  EXPECT_EQ(all(d, ""), "!syntax");
  EXPECT_EQ(all(d, "div,"), "!syntax");
  EXPECT_EQ(all(d, "ns|div"), "!syntax") << "an undeclared namespace is a syntax error";
  EXPECT_EQ(all(d, ">*"), "!syntax") << "a relative selector is not a selector";
  EXPECT_EQ(all(d, "div::example"), "!syntax");
  EXPECT_EQ(all(d, ".5cm"), "!syntax");
  EXPECT_EQ(all(d, "div % address, p"), "!syntax");
  EXPECT_EQ(all(d, ":is(.a, ::b), .x"), "root p1 p2 l1 l3 l5") << "a forgiving list inside does not make the query fail";
}

TEST_F(CssMatch, LinkStatesWithVisitedNeverMatching) {
  EXPECT_EQ(all(d, "a:link"), "a1");
  EXPECT_EQ(all(d, ":any-link"), "a1");
  EXPECT_EQ(all(d, "a:visited"), "");
  EXPECT_EQ(all(d, "a:not(:link)"), "a2");
  EXPECT_EQ(all(d, ":hover"), "") << "hover is never set by the adapter";
  EXPECT_EQ(all(d, ":focus, :active, :target, :focus-within"), "");
}

TEST_F(CssMatch, FormStatesFromTheAttributes) {
  EXPECT_EQ(all(d, ":checked"), "i1");
  EXPECT_EQ(all(d, "input:disabled"), "i2 i4") << "i4 is in a disabled fieldset; i3 is in its first legend, which is exempt";
  EXPECT_EQ(all(d, "fieldset:disabled"), "fs");
  EXPECT_EQ(all(d, "input:enabled"), "i1 i3 i5");
  EXPECT_EQ(all(d, ":required"), "i2");
  EXPECT_EQ(all(d, "input:optional"), "i1 i3 i4 i5");
  EXPECT_EQ(all(d, "input:read-write"), "i3");
  EXPECT_EQ(all(d, "input:read-only"), "i1 i2 i4 i5");
  EXPECT_EQ(all(d, ":placeholder-shown"), "i2");
  EXPECT_EQ(all(d, ":default"), "i1");
  EXPECT_EQ(all(d, "my-el:defined, p:defined"), "p1 p2 p3 pde pun pr pa");
  EXPECT_EQ(all(d, ":not(:defined)"), "ce") << "no custom element is upgraded in a static tree";
  Doc o;
  ASSERT_TRUE(o.html("<!DOCTYPE html><details id=\"dt\" open></details><details id=\"dn\"></details><progress id=\"pg\"></progress><progress id=\"pv\" value=\"1\"></progress>"));
  EXPECT_EQ(all(o, ":open"), "dt");
  EXPECT_EQ(all(o, ":indeterminate"), "pg");
}

TEST_F(CssMatch, LangUsesExtendedFilteringOnTheNearestLanguage) {
  // Selectors 4, 7.2: "`:lang()` performs implicit wildcard matching on trailing subtags"; "`:lang(\"\")` matches only elements whose language is not tagged"
  EXPECT_EQ(all(d, "#root:lang(en)"), "root");
  EXPECT_EQ(all(d, "p:lang(en)"), "p1 p2 p3 pr pa");
  EXPECT_EQ(all(d, "p:lang(de)"), "pde");
  EXPECT_EQ(all(d, "p:lang(de-CH)"), "pde");
  EXPECT_EQ(all(d, "p:lang(de-CH-1996)"), "");
  EXPECT_EQ(all(d, "p:lang(d)"), "") << "a range matches whole subtags";
  EXPECT_EQ(all(d, "p:lang(\"*-CH\")"), "pde");
  EXPECT_EQ(all(d, "p:lang(\"*\")"), "p1 p2 p3 pde pr pa") << "\"The wildcard `*` does not match elements whose language is not tagged\"";
  EXPECT_EQ(all(d, "p:lang(\"\")"), "pun");
  EXPECT_EQ(all(d, "p:lang(en, de)"), "p1 p2 p3 pde pr pa");
  EXPECT_EQ(all(d, "p:lang(EN)"), "p1 p2 p3 pr pa") << "ASCII case-insensitive";
  EXPECT_EQ(all(d, "p:lang(\"de--CH\")"), "") << "a range that is not well-formed matches nothing";
  EXPECT_EQ(all(d, "p:lang(\"waytoolongsubtag\")"), "");
  Doc l;
  ASSERT_TRUE(l.html("<!DOCTYPE html><p id=\"a\" lang=\"zh-Hant-TW\">x</p><p id=\"b\" lang=\"zh-x-private\">y</p><p id=\"c\" lang=\"und\">z</p>"));
  EXPECT_EQ(all(l, "p:lang(zh)"), "a b");
  EXPECT_EQ(all(l, "p:lang(zh-TW)"), "a") << "RFC 4647 3.3.2: a range subtag may match later in the tag";
  EXPECT_EQ(all(l, "p:lang(\"zh-*-TW\")"), "a");
  EXPECT_EQ(all(l, "p:lang(zh-private)"), "") << "a singleton subtag ends the matching";
  EXPECT_EQ(all(l, "p:lang(\"*\")"), "a b c") << "\"The wildcard does match elements tagged as undetermined\"";
}

TEST_F(CssMatch, DirIsTheDirAttributeInheritedAndAutoLooksForTheFirstStrongCharacter) {
  EXPECT_EQ(all(d, "#rtl:dir(rtl)"), "rtl");
  EXPECT_EQ(all(d, "#pr:dir(rtl)"), "pr") << "inherited";
  EXPECT_EQ(all(d, "#pa:dir(rtl)"), "pa") << "dir=auto and Hebrew text";
  EXPECT_EQ(all(d, "#p1:dir(ltr)"), "p1");
  EXPECT_EQ(all(d, "p:dir(ltr)"), "p1 p2 p3 pde pun");
}

TEST_F(CssMatch, HasAndItsFourRelations) {
  EXPECT_EQ(all(d, "div:has(> p)"), "root rtl");
  EXPECT_EQ(all(d, "div:has(p)"), "root rtl");
  EXPECT_EQ(all(d, "ul:has(li.x)"), "ul");
  EXPECT_EQ(all(d, "li:has(+ li.x)"), "l2 l4");
  EXPECT_EQ(all(d, "li:has(~ li.x)"), "l1 l2 l3 l4");
  EXPECT_EQ(all(d, "p:has(b)"), "p3");
  EXPECT_EQ(all(d, "body:has(#i3)"), "body");
  EXPECT_EQ(all(d, "div:not(:has(*))"), "empty wsonly");
  EXPECT_EQ(all(d, "ul:not(:has(.zzz))"), "ul");
  EXPECT_EQ(all(d, "#root:has(> p + span + p > b)"), "root") << "a chain below the anchor";
  EXPECT_EQ(all(d, "#root:has(> p ~ ul > li:nth-child(5))"), "root");
  EXPECT_EQ(all(d, "#root:has(> ul > li + li + li + li + li + li)"), "");
  EXPECT_EQ(all(d, "li:has(> :scope)"), "") << "an anchor-dependent chain";
  EXPECT_EQ(within(d, "l2", ":scope:has(+ li)"), "");
  EXPECT_TRUE(matches(d.at("l2"), ":scope:has(+ li.x)"));
  EXPECT_EQ(all(d, ":has(> a:link)"), "root");
  EXPECT_EQ(all(d, "li:has(:is(.x, .y))"), "");
  EXPECT_EQ(all(d, "ul:has(:is(li.x))"), "ul");
  EXPECT_EQ(all(d, "ul:has(.zzz, .x)"), "ul") << "the answer for a step of one relative selector is not the answer for the step of the next";
  EXPECT_EQ(all(d, "ul:has(> li.y, > li.x)"), "ul");
  EXPECT_EQ(all(d, "ul:has(.x ~ .y, .y ~ .x, li + li.x)"), "ul");
  EXPECT_EQ(all(d, "ul:has(.y ~ .x)"), "");
}

TEST_F(CssMatch, ShadowSelectorsMatchNothingInATreeWithNoShadowTrees) {
  EXPECT_EQ(all(d, ":host"), "");
  EXPECT_EQ(all(d, ":host(div)"), "");
  EXPECT_EQ(all(d, ":host-context(body) p"), "");
  EXPECT_EQ(all(d, "::slotted(p)"), "");
  EXPECT_EQ(all(d, "div::part(x)"), "");
  EXPECT_EQ(all(d, ":state(foo)"), "");
}

TEST_F(CssMatch, HeadingLevels) {
  Doc h;
  ASSERT_TRUE(h.html("<!DOCTYPE html><h1 id=\"a\">x</h1><h2 id=\"b\">x</h2><h6 id=\"c\">x</h6><p id=\"d\">x</p>"));
  EXPECT_EQ(all(h, ":heading"), "a b c");
  EXPECT_EQ(all(h, ":heading(2, 6)"), "b c");
  EXPECT_EQ(all(h, ":heading(0, 7)"), "");
}

TEST_F(CssMatch, NamespacesThroughACompiledSelectorWithAMap) {
  Doc x;
  ASSERT_TRUE(x.xml("<r xmlns=\"urn:a\" xmlns:b=\"urn:b\"><x id=\"a1\"/><b:x id=\"b1\"/><y xmlns=\"\" id=\"n1\"/></r>"));
  const GTEXT_CSS_Namespace entries[] = {{"a", "urn:a"}, {"b", "urn:b"}};
  GTEXT_CSS_Namespace_Map map{entries, 2, nullptr};
  auto run = [&](const char * sel, const GTEXT_CSS_Namespace_Map * m) {
    GTEXT_CSS_Selector_Options o = gtext_css_selector_options_default();
    o.namespaces = m;
    GTEXT_CSS_Selector_List * l = nullptr;
    GTEXT_CSS_Selector_Result r;
    std::memset(&r, 0, sizeof r);
    EXPECT_EQ(gtext_css_selector_parse(sel, std::strlen(sel), &o, &l, &r), GTEXT_CSS_OK);
    gtext_css_selector_result_free(&r);
    std::string s;
    if (!l) {
      return std::string("!invalid");
    }
    GTEXT_Markup_Node ** out = nullptr;
    size_t n = 0;
    EXPECT_EQ(gtext_markup_query_selector_all_compiled(x.root(), l, nullptr, &out, &n, nullptr), GTEXT_MARKUP_OK);
    for (size_t i = 0; i < n; i++) {
      s += (i ? " " : "") + cmatch::id_of(out[i]);
    }
    gtext_markup_query_free(nullptr, out);
    gtext_css_selector_list_free(l);
    return s;
  };
  EXPECT_EQ(run("x", &map), "a1 b1") << "no default namespace: a type selector with no prefix matches any namespace";
  EXPECT_EQ(run("a|x", &map), "a1");
  EXPECT_EQ(run("b|x", &map), "b1");
  EXPECT_EQ(run("*|x", &map), "a1 b1");
  EXPECT_EQ(run("|y", &map), "n1") << "an empty prefix is no namespace";
  EXPECT_EQ(run("|x", &map), "");
  EXPECT_EQ(run("a|*", &map), "<r> a1") << "the root (in urn:a) and its x";
  EXPECT_EQ(run("*|*", &map), "<r> a1 b1 n1");
  GTEXT_CSS_Namespace_Map withdefault{entries, 2, "urn:a"};
  EXPECT_EQ(run("x", &withdefault), "a1") << "\"A type selector with no prefix uses the default namespace\"";
  EXPECT_EQ(run("*|x", &withdefault), "a1 b1");
  EXPECT_EQ(run("y", &withdefault), "");
  EXPECT_EQ(run("b|x", nullptr), "!invalid");
}

TEST(CssMatchInterface, ACallbackThatIsNeededAndAbsentIsUnsupportedAndNamedNeverWorkedAround) {
  Doc d;
  ASSERT_TRUE(d.html("<!DOCTYPE html><div id=\"a\"><p id=\"b\">x</p></div>"));
  GTEXT_CSS_Markup_Adapter * ad = nullptr;
  ASSERT_EQ(gtext_css_markup_adapter_new(d.doc, nullptr, &ad), GTEXT_CSS_OK);
  GTEXT_CSS_Element_Interface table = *gtext_css_markup_interface(ad);
  table.child_content = nullptr;
  GTEXT_CSS_Selector_Match_Context * ctx = nullptr;
  GTEXT_CSS_Selector_Match_Options mo = gtext_css_markup_match_options(ad);
  ASSERT_EQ(gtext_css_selector_match_context_new(&table, &mo, &ctx), GTEXT_CSS_OK);
  GTEXT_CSS_Selector_List * l = nullptr;
  ASSERT_EQ(gtext_css_selector_parse("p:empty", 7, nullptr, &l, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Selector_Match_Result r;
  EXPECT_EQ(gtext_css_selector_match(l, ctx, d.at("b"), nullptr, &r), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_STREQ(r.callback, "child_content");
  EXPECT_FALSE(r.matched);
  gtext_css_selector_list_free(l);
  ASSERT_EQ(gtext_css_selector_parse("p:current(.a)", 13, nullptr, &l, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_selector_match(l, ctx, d.at("b"), nullptr, &r), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_STREQ(r.selector, ":current()");
  gtext_css_selector_list_free(l);
  // an older table, from before the callbacks of selectors
  table = *gtext_css_markup_interface(ad);
  table.size = offsetof(GTEXT_CSS_Element_Interface, direction) + sizeof table.direction;
  gtext_css_selector_match_context_free(ctx);
  ASSERT_EQ(gtext_css_selector_match_context_new(&table, &mo, &ctx), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_selector_parse("div p", 5, nullptr, &l, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_selector_match(l, ctx, d.at("b"), nullptr, &r), GTEXT_CSS_OK) << "what a caller built before needs is still there";
  EXPECT_TRUE(r.matched);
  gtext_css_selector_list_free(l);
  ASSERT_EQ(gtext_css_selector_parse("p:has(a)", 8, nullptr, &l, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_selector_match(l, ctx, d.at("b"), nullptr, &r), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_STREQ(r.callback, "first_child");
  gtext_css_selector_list_free(l);
  gtext_css_selector_match_context_free(ctx);
  gtext_css_markup_adapter_free(ad);
}

TEST(CssMatchOptions, VisitedAsStateAndTheHoverAndActiveQuirk) {
  Doc d;
  ASSERT_TRUE(d.html("<!DOCTYPE html><a id=\"l\" href=\"x\">a</a><span id=\"s\">b</span><a id=\"n\">c</a>"));
  GTEXT_CSS_Markup_Options mo;
  std::memset(&mo, 0, sizeof mo);
  mo.state = [](void *, const GTEXT_Markup_Node * e) -> uint64_t {
    GTEXT_Markup_Attribute a;
    const bool l = gtext_markup_element_get_attribute(e, nullptr, "id", &a) && a.value[0] == 'l';
    return GTEXT_CSS_STATE_HOVER | (l ? GTEXT_CSS_STATE_VISITED : 0u);
  };
  GTEXT_CSS_Markup_Adapter * ad = nullptr;
  ASSERT_EQ(gtext_css_markup_adapter_new(d.doc, &mo, &ad), GTEXT_CSS_OK);
  auto run = [&](const char * sel, bool quirks, GTEXT_CSS_Visited_Mode visited) {
    GTEXT_CSS_Selector_Match_Options o = gtext_css_markup_match_options(ad);
    o.quirks = quirks;
    o.visited = visited;
    GTEXT_CSS_Selector_Match_Context * ctx = nullptr;
    EXPECT_EQ(gtext_css_selector_match_context_new(gtext_css_markup_interface(ad), &o, &ctx), GTEXT_CSS_OK);
    GTEXT_CSS_Selector_List * l = nullptr;
    EXPECT_EQ(gtext_css_selector_parse(sel, std::strlen(sel), nullptr, &l, nullptr), GTEXT_CSS_OK);
    std::string out;
    for (const char * id : {"l", "s", "n"}) {
      GTEXT_CSS_Selector_Match_Result r;
      EXPECT_EQ(gtext_css_selector_match(l, ctx, d.at(id), nullptr, &r), GTEXT_CSS_OK);
      if (r.matched) {
        out += id;
      }
    }
    gtext_css_selector_list_free(l);
    gtext_css_selector_match_context_free(ctx);
    return out;
  };
  EXPECT_EQ(run(":hover", false, GTEXT_CSS_VISITED_NEVER), "lsn");
  EXPECT_EQ(run(":hover", true, GTEXT_CSS_VISITED_NEVER), "l") << "Quirks Mode Standard 4.1: \"must not match elements that wouldn't also match :any-link\"";
  EXPECT_EQ(run("a:hover", true, GTEXT_CSS_VISITED_NEVER), "ln") << "the quirk applies only to a compound of nothing but :hover and :active";
  EXPECT_EQ(run(".z, :hover:active", true, GTEXT_CSS_VISITED_NEVER), "") << ":active is not set";
  EXPECT_EQ(run(":visited", false, GTEXT_CSS_VISITED_AS_STATE), "l") << "the caller says visited";
  EXPECT_EQ(run(":visited", false, GTEXT_CSS_VISITED_NEVER), "");
  EXPECT_EQ(run(":link", false, GTEXT_CSS_VISITED_AS_STATE), "l");
  EXPECT_EQ(run(":link", false, GTEXT_CSS_VISITED_NEVER), "l") << "never: \":link\" matches any link, a visited one too";
  EXPECT_EQ(run(":any-link", false, GTEXT_CSS_VISITED_AS_STATE), "l");
  gtext_css_markup_adapter_free(ad);
}

TEST(CssMatchOptions, TheMemoAndTheFilterNeverChangeAnAnswer) {
  Doc d;
  ASSERT_TRUE(d.html(kPage));
  GTEXT_CSS_Markup_Adapter * ad = nullptr;
  ASSERT_EQ(gtext_css_markup_adapter_new(d.doc, nullptr, &ad), GTEXT_CSS_OK);
  const char * const sels[] = {"body div p", "html body div ul li:nth-child(2n+1)", "#root .x ~ a", "div > p + span ~ p b", "ul li.x:has(~ li)", "body :not(.x) + li",
      "div div p", ".a .b", "#root :is(p, li):nth-of-type(2)", "html > body p:last-child", "form input:not(:disabled)"};
  std::vector<std::string> baseline;
  for (int mode = 0; mode < 4; mode++) {
    GTEXT_CSS_Selector_Match_Options o = gtext_css_markup_match_options(ad);
    o.disable_memo = mode & 1;
    o.disable_bloom = mode & 2;
    GTEXT_CSS_Selector_Match_Context * ctx = nullptr;
    ASSERT_EQ(gtext_css_selector_match_context_new(gtext_css_markup_interface(ad), &o, &ctx), GTEXT_CSS_OK);
    std::vector<std::string> got;
    for (const char * sel : sels) {
      GTEXT_CSS_Selector_List * l = nullptr;
      ASSERT_EQ(gtext_css_selector_parse(sel, std::strlen(sel), nullptr, &l, nullptr), GTEXT_CSS_OK);
      std::string out;
      struct V {
        GTEXT_CSS_Selector_List * l;
        GTEXT_CSS_Selector_Match_Context * c;
        std::string * out;
      } v{l, ctx, &out};
      GTEXT_CSS_Selector_Match_Result r;
      ASSERT_EQ(gtext_css_selector_match_walk(ctx, gtext_css_markup_handle(gtext_markup_document_element(d.doc)),
                    [](void * u, GTEXT_CSS_Element_Handle e) {
                      V * v = static_cast<V *>(u);
                      GTEXT_CSS_Selector_Match_Result r;
                      EXPECT_EQ(gtext_css_selector_match(v->l, v->c, e, nullptr, &r), GTEXT_CSS_OK);
                      if (r.matched) {
                        *v->out += cmatch::id_of(static_cast<const GTEXT_Markup_Node *>(e)) + " ";
                      }
                      return true;
                    },
                    &v, &r),
          GTEXT_CSS_OK);
      got.push_back(out);
      gtext_css_selector_list_free(l);
    }
    if (mode == 0) {
      baseline = got;
    }
    EXPECT_EQ(got, baseline) << "mode " << mode;
    if (mode == 0) {
      EXPECT_GT(gtext_css_selector_match_counters(ctx).bloom_rejects, 0u) << "the filter did reject something, so the comparison shows it harmless";
      EXPECT_GT(gtext_css_selector_match_counters(ctx).memo_hits, 0u);
    }
    if (mode & 2) {
      EXPECT_EQ(gtext_css_selector_match_counters(ctx).bloom_rejects, 0u);
    }
    gtext_css_selector_match_context_free(ctx);
  }
  EXPECT_NE(baseline[0], "");
  gtext_css_markup_adapter_free(ad);
}

TEST(CssMatchLimits, WorkAndMemoEntriesAreStatusesAndNeverAWrongAnswer) {
  std::string html = "<!DOCTYPE html><div>";
  for (int i = 0; i < 400; i++) {
    html += "<p class=\"x\"></p>";
  }
  html += "</div>";
  Doc d;
  ASSERT_TRUE(d.html(html));
  GTEXT_Markup_Query_Options o = gtext_markup_query_options_default();
  o.max_work = 300;
  EXPECT_EQ(all(d.root(), "div p.x:nth-child(odd) ~ p", &o), "!" + std::string(gtext_markup_status_string(GTEXT_MARKUP_E_LIMIT)));
  o = gtext_markup_query_options_default();
  o.max_memo_entries = 20;
  EXPECT_EQ(all(d.root(), "div p.x:nth-child(odd) ~ p", &o), "!" + std::string(gtext_markup_status_string(GTEXT_MARKUP_E_LIMIT)));
  GTEXT_Markup_Node ** out = nullptr;
  size_t n = 0;
  GTEXT_Markup_Query_Result r;
  std::memset(&r, 0, sizeof r);
  EXPECT_EQ(gtext_markup_query_selector_all(d.root(), "p:nth-child(odd)", 16, &o, &out, &n, &r), GTEXT_MARKUP_E_LIMIT);
  EXPECT_EQ(r.css_status, GTEXT_CSS_E_LIMIT_MATCH_MEMO);
  EXPECT_EQ(out, nullptr);
  gtext_markup_query_result_free(&r);
}

TEST(CssMatchQueries, TheArrayTheIteratorAndTheFirstAreTheSameMatchesInDocumentOrderWithNoDuplicates) {
  Doc d;
  ASSERT_TRUE(d.html(kPage));
  const std::string sel = ".x, li, p.x, #l2";
  GTEXT_Markup_Node ** arr = nullptr;
  size_t n = 0;
  ASSERT_EQ(gtext_markup_query_selector_all(d.root(), sel.data(), sel.size(), nullptr, &arr, &n, nullptr), GTEXT_MARKUP_OK);
  std::string a;
  for (size_t i = 0; i < n; i++) {
    a += (i ? " " : "") + cmatch::id_of(arr[i]);
  }
  gtext_markup_query_free(nullptr, arr);
  EXPECT_EQ(a, "p1 p2 l1 l2 l3 l4 l5") << "document order, each once, whatever order the selector lists them";
  std::string it;
  ASSERT_EQ(gtext_markup_query_selector_each(d.root(), sel.data(), sel.size(), nullptr, [](void * u, const GTEXT_Markup_Node * e) {
    *static_cast<std::string *>(u) += cmatch::id_of(e) + " ";
    return true;
  }, &it, nullptr), GTEXT_MARKUP_OK);
  EXPECT_EQ(it, "p1 p2 l1 l2 l3 l4 l5 ");
  GTEXT_Markup_Node * first = nullptr;
  ASSERT_EQ(gtext_markup_query_selector(d.root(), sel.data(), sel.size(), nullptr, &first, nullptr), GTEXT_MARKUP_OK);
  EXPECT_EQ(cmatch::id_of(first), "p1");
  ASSERT_EQ(gtext_markup_query_selector(d.at("l1"), "ul", 2, nullptr, &first, nullptr), GTEXT_MARKUP_OK);
  EXPECT_EQ(first, nullptr) << "an ancestor is not a descendant";
  // a compiled selector is used many times
  GTEXT_CSS_Selector_List * l = nullptr;
  ASSERT_EQ(gtext_css_selector_parse("li", 2, nullptr, &l, nullptr), GTEXT_CSS_OK);
  for (const char * id : {"ul", "root", "body"}) {
    ASSERT_EQ(gtext_markup_query_selector_all_compiled(d.at(id), l, nullptr, &arr, &n, nullptr), GTEXT_MARKUP_OK);
    EXPECT_EQ(n, 5u) << id;
    gtext_markup_query_free(nullptr, arr);
  }
  gtext_css_selector_list_free(l);
  // the scope may be a fragment or the document, but not a text node
  ASSERT_EQ(gtext_markup_query_selector_all(gtext_markup_node_first_child(d.at("p1")), "x", 1, nullptr, &arr, &n, nullptr), GTEXT_MARKUP_E_INVALID);
}
