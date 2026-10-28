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
 * The selector parser, serializer and specificity (css_selector.h), each test named for the rule it holds and quoting the sentence of the served drafts (read
 * 2026-10-28) that decides it where the sentence is short. Selectors 4 is the draft unless a test says another.
 */

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_selector_rows.h"

namespace {

GTEXT_Markup_Status collect(void * user, const char * bytes, size_t len) {
  static_cast<std::string *>(user)->append(bytes, len);
  return GTEXT_MARKUP_OK;
}

struct Parsed {
  GTEXT_CSS_Selector_List * list = nullptr;
  GTEXT_CSS_Selector_Result result;
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  Parsed() {
    std::memset(&result, 0, sizeof result);
  }
  ~Parsed() {
    gtext_css_selector_list_free(list);
    gtext_css_selector_result_free(&result);
  }
  Parsed(const Parsed &) = delete;
  Parsed & operator=(const Parsed &) = delete;
  bool valid() const {
    return status == GTEXT_CSS_OK && list != nullptr;
  }
  std::string text() const {
    std::string out;
    GTEXT_Markup_Sink sink{collect, &out};
    if (list) {
      gtext_css_selector_serialize(list, &sink);
    }
    return out;
  }
  GTEXT_CSS_Specificity spec() const {
    return list ? list->max_specificity : GTEXT_CSS_Specificity{0, 0, 0};
  }
};

void parse(Parsed & p, const std::string & s, GTEXT_CSS_Selector_Mode mode = GTEXT_CSS_SELECTOR_COMPLEX_LIST,
    const GTEXT_CSS_Namespace_Map * ns = nullptr) {
  GTEXT_CSS_Selector_Options o = gtext_css_selector_options_default();
  o.mode = mode;
  o.namespaces = ns;
  p.status = gtext_css_selector_parse(s.data(), s.size(), &o, &p.list, &p.result);
}

std::string written(const std::string & s, GTEXT_CSS_Selector_Mode mode = GTEXT_CSS_SELECTOR_COMPLEX_LIST, const GTEXT_CSS_Namespace_Map * ns = nullptr) {
  Parsed p;
  parse(p, s, mode, ns);
  return p.valid() ? p.text() : "INVALID";
}

std::string spec_of(const std::string & s) {
  Parsed p;
  parse(p, s);
  if (!p.valid()) {
    return "INVALID";
  }
  const auto sp = p.spec();
  return "(" + std::to_string(sp.a) + "," + std::to_string(sp.b) + "," + std::to_string(sp.c) + ")";
}

}  // namespace

/* ---- every row ---------------------------------------------------------------------------------------------- */

TEST(CssSelector, EveryRowOfTheTablesHasAProbeThatParsesWritesAndCounts) {
  for (const SelectorProbe & pr : selector_probes) {
    Parsed p;
    parse(p, pr.probe);
    ASSERT_TRUE(p.valid()) << pr.name << ": " << pr.probe;
    EXPECT_EQ(p.text(), pr.serialized) << pr.name;
    EXPECT_EQ(p.spec().a, pr.a) << pr.name;
    EXPECT_EQ(p.spec().b, pr.b) << pr.name;
    EXPECT_EQ(p.spec().c, pr.c) << pr.name;
    Parsed again;
    parse(again, p.text());
    ASSERT_TRUE(again.valid()) << pr.name;
    EXPECT_EQ(again.text(), p.text()) << pr.name << ": the serialization is a fixed point";
    EXPECT_EQ(again.spec().a, p.spec().a);
    EXPECT_EQ(again.spec().b, p.spec().b);
    EXPECT_EQ(again.spec().c, p.spec().c);
  }
}

TEST(CssSelector, TheTablesNameEverySelectorOfTheSelectorsPinAndNothingUnknownParses) {
  const size_t n = gtext_css_selector_row_count();
  EXPECT_GE(n, 159u) << "the 159 selectors of the pin";
  for (size_t i = 0; i < n; i++) {
    GTEXT_CSS_Selector_Row row;
    ASSERT_TRUE(gtext_css_selector_row(i, &row));
    EXPECT_EQ(gtext_css_selector_find_row(row.name), static_cast<int>(i)) << row.name;
  }
  GTEXT_CSS_Selector_Row row;
  EXPECT_FALSE(gtext_css_selector_row(n, &row));
  EXPECT_EQ(gtext_css_selector_find_row(":nonesuch"), -1);
  // "An unknown selector (including a vendor-prefixed one the Compatibility Standard does not alias) makes its selector invalid"
  for (const char * bad : {":nonesuch", "::nonesuch", ":-webkit-any-link", "::-webkit-scrollbar", ":-moz-focusring", ":hover()", ":not", ":is", "::before()", ":nth-child"}) {
    EXPECT_EQ(written(bad), "INVALID") << bad;
  }
}

/* ---- specificity -------------------------------------------------------------------------------------------- */

TEST(CssSelectorSpecificity, TheDraftsExamples) {
  // Selectors 4, 17: "count the number of ID selectors in the selector (= A)", class, attribute and pseudo-class (= B), type and pseudo-element (= C)
  EXPECT_EQ(spec_of("*"), "(0,0,0)");
  EXPECT_EQ(spec_of("li"), "(0,0,1)");
  EXPECT_EQ(spec_of("ul li"), "(0,0,2)");
  EXPECT_EQ(spec_of("ul ol+li"), "(0,0,3)");
  EXPECT_EQ(spec_of("h1 + *[rel=up]"), "(0,1,1)");
  EXPECT_EQ(spec_of("ul ol li.red"), "(0,1,3)");
  EXPECT_EQ(spec_of("li.red.level"), "(0,2,1)");
  EXPECT_EQ(spec_of("#x34y"), "(1,0,0)");
  EXPECT_EQ(spec_of("li:first-child h2 .title"), "(0,2,2)");
  EXPECT_EQ(spec_of("#s12:not(p)"), "(1,0,1)");
  EXPECT_EQ(spec_of("a:not(.c, #d)"), "(1,0,1)");
  EXPECT_EQ(spec_of("#a .b:not(.c, #d) > p"), "(2,1,1)") << "`:not` adds `#d`, the most specific of its argument, to #a, .b and p";
}

TEST(CssSelectorSpecificity, IsNotAndHasTakeTheMostSpecificArgumentAndWhereIsZero) {
  EXPECT_EQ(spec_of(":where(#a, .b) p"), "(0,0,1)");
  EXPECT_EQ(spec_of(":is(#a, .b)"), "(1,0,0)") << "\"The specificity of an :is() pseudo-class is replaced by the specificity of the most specific complex selector in its selector list argument.\"";
  EXPECT_EQ(spec_of(":is(.a .b, #c)"), "(1,0,0)");
  EXPECT_EQ(spec_of(":is(.a .b, .c)"), "(0,2,0)") << "the complex selector is counted whole, not summed over the list";
  EXPECT_EQ(spec_of(":not(.a, .b .c)"), "(0,2,0)");
  EXPECT_EQ(spec_of("a:has(> #b, .c)"), "(1,0,1)");
  EXPECT_EQ(spec_of(":is()"), "(0,0,0)") << "the empty forgiving list is zero";
  EXPECT_EQ(spec_of(":where()"), "(0,0,0)");
  EXPECT_EQ(spec_of("a:where()"), "(0,0,1)");
  EXPECT_EQ(spec_of(":is(:where(#a))"), "(0,0,0)");
  EXPECT_EQ(spec_of(":matches(#a)"), "(1,0,0)") << "the legacy alias of :is()";
}

TEST(CssSelectorSpecificity, NthPseudoClassesAddAClassAndOfSTakesTheArgumentOfS) {
  EXPECT_EQ(spec_of(":nth-child(2n+1)"), "(0,1,0)");
  EXPECT_EQ(spec_of(":nth-child(2n+1 of .a)"), "(0,2,0)") << "\"the specificity of the :nth-child() pseudo-class is the specificity of a pseudo-class, plus the specificity of the most specific complex selector in S\"";
  EXPECT_EQ(spec_of("li:nth-last-child(odd of #a, .b)"), "(1,1,1)");
  EXPECT_EQ(spec_of(":nth-of-type(2)"), "(0,1,0)");
}

TEST(CssSelectorSpecificity, ShadowAndPseudoElementSelectors) {
  EXPECT_EQ(spec_of(":host"), "(0,1,0)");
  EXPECT_EQ(spec_of(":host(.a)"), "(0,2,0)") << "\"The specificity of :host() is that of a pseudo-class, plus the specificity of its argument\"";
  EXPECT_EQ(spec_of(":host(#a)"), "(1,1,0)");
  EXPECT_EQ(spec_of(":host-context(#a)"), "(1,1,0)");
  EXPECT_EQ(spec_of("::slotted(p)"), "(0,0,2)") << "a pseudo-element plus the specificity of its argument";
  EXPECT_EQ(spec_of("::slotted(.a)"), "(0,1,1)");
  EXPECT_EQ(spec_of("::part(a b)"), "(0,0,1)");
  EXPECT_EQ(spec_of("::before"), "(0,0,1)");
  EXPECT_EQ(spec_of("::part(a)::before"), "(0,0,2)") << "a tree-abiding chain counts every pseudo-element";
  EXPECT_EQ(spec_of("::before::marker"), "(0,0,2)");
  EXPECT_EQ(spec_of("p::first-line"), "(0,0,2)");
  EXPECT_EQ(spec_of("p:first-line"), "(0,0,2)") << "the legacy single colon spelling";
}

TEST(CssSelectorSpecificity, TheNestingSelectorIsTheMostSpecificSelectorOfTheListItStandsFor) {
  Parsed p;
  parse(p, "& .c");
  ASSERT_TRUE(p.valid());
  EXPECT_EQ(p.spec().b, 2u) << "a parse alone takes `&` as `:scope`, (0,1,0), as at the top level";
  EXPECT_TRUE(p.list->has_nesting);
  EXPECT_TRUE(p.list->complex[0].has_nesting);
  const auto parent = gtext_css_specificity_make(1, 1, 0);
  const auto s = gtext_css_complex_specificity(&p.list->complex[0], parent);
  EXPECT_EQ(s.a, 1u);
  EXPECT_EQ(s.b, 2u);
  const auto z = gtext_css_complex_specificity(&p.list->complex[0], gtext_css_specificity_make(0, 0, 0));
  EXPECT_EQ(z.b, 1u) << "inside @scope `&` is :where(:scope), zero";
  Parsed q;
  parse(q, ":is(&, .x) > a");
  ASSERT_TRUE(q.valid());
  EXPECT_EQ(gtext_css_selector_list_specificity(q.list, parent).a, 1u) << "the recomputation reaches a `&` inside a functional pseudo-class";
}

TEST(CssSelectorSpecificity, ComponentsSaturateAtTheMaximumAndDoNotWrap) {
  std::string s;
  for (int i = 0; i < 70; i++) {
    s += "#a";
  }
  Parsed p;
  parse(p, s);
  ASSERT_TRUE(p.valid());
  EXPECT_EQ(p.spec().a, 70u);
  const auto big = gtext_css_specificity_add(gtext_css_specificity_make(UINT32_MAX, 0, 0), gtext_css_specificity_make(5, 0, 0));
  EXPECT_EQ(big.a, UINT32_MAX);
}

/* ---- the modes of the grammar ------------------------------------------------------------------------------- */

TEST(CssSelectorModes, AComplexListIsUnforgivingOneBadSelectorInvalidatesTheList) {
  EXPECT_EQ(written(".a, ::bogus"), "INVALID");
  EXPECT_EQ(written(".a,, .b"), "INVALID");
  EXPECT_EQ(written(".a,"), "INVALID");
  EXPECT_EQ(written(""), "INVALID") << "an empty list is not a selector";
  EXPECT_EQ(written("   "), "INVALID");
  EXPECT_EQ(written(".a, .b"), ".a, .b");
  Parsed p;
  parse(p, ".a, ::bogus");
  EXPECT_EQ(p.status, GTEXT_CSS_OK) << "\"a selector that is not valid is an answer, not a failure\"";
  EXPECT_EQ(p.list, nullptr);
  EXPECT_FALSE(p.result.valid);
  ASSERT_GE(gtext_css_selector_result_diagnostic_count(&p.result), 1u);
  GTEXT_CSS_Selector_Diagnostic d;
  ASSERT_TRUE(gtext_css_selector_result_diagnostic(&p.result, 0, &d));
  EXPECT_EQ(d.kind, GTEXT_CSS_SELDIAG_UNKNOWN_SELECTOR);
  EXPECT_EQ(d.span.offset, 4u);
  EXPECT_EQ(d.span.end, 11u);
  EXPECT_NE(std::string(d.reference).find("Selectors 4"), std::string::npos);
}

TEST(CssSelectorModes, AForgivingListKeepsWhatIsInvalidAsWrittenAndNeverMatchesIt) {
  // Selectors 4, 18.2: the forgiving list; csswg-drafts 8356: "preserve invalid selectors inside forgiving selector lists"
  Parsed p;
  parse(p, ":is(.a, ::bogus, .b)");
  ASSERT_TRUE(p.valid());
  EXPECT_EQ(p.text(), ":is(.a, ::bogus, .b)");
  const GTEXT_CSS_Simple & is = p.list->complex[0].compounds[0].simples[0];
  ASSERT_NE(is.list, nullptr);
  ASSERT_EQ(is.list->count, 3u);
  EXPECT_FALSE(is.list->complex[0].never);
  EXPECT_TRUE(is.list->complex[1].never);
  EXPECT_NE(is.list->complex[1].raw.data, nullptr);
  EXPECT_FALSE(is.list->complex[2].never);
  EXPECT_EQ(is.list->max_specificity.b, 1u) << "the raw entry has no specificity";
  EXPECT_EQ(written(":is()"), ":is()");
  EXPECT_EQ(written(":is(,)"), ":is()");
  EXPECT_EQ(written(":where( )"), ":where()");
  EXPECT_EQ(written(":is(123)"), ":is(123)");
  EXPECT_EQ(written(":is(.a, 123)"), ":is(.a, 123)");
  EXPECT_EQ(written("a:is(:unknown)"), "a:is(:unknown)");
  Parsed bad;
  parse(bad, ":is(.a", GTEXT_CSS_SELECTOR_COMPLEX_LIST);
  EXPECT_TRUE(bad.valid()) << "end of input closes the function";
}

TEST(CssSelectorModes, ARelativeListMayBeginWithACombinator) {
  EXPECT_EQ(written("> a, + b, ~ c, d", GTEXT_CSS_SELECTOR_RELATIVE_LIST), "> a, + b, ~ c, d");
  EXPECT_EQ(written(":has(> a b)"), ":has(> a b)");
  EXPECT_EQ(written(":has(a)"), ":has(a)");
  EXPECT_EQ(written("a:has(  >  b )"), "a:has(> b)");
  EXPECT_EQ(written("> a"), "INVALID") << "a leading combinator is only for a relative list";
  Parsed p;
  parse(p, "> a", GTEXT_CSS_SELECTOR_RELATIVE_LIST);
  ASSERT_TRUE(p.valid());
  EXPECT_TRUE(p.list->complex[0].relative);
  EXPECT_EQ(p.list->complex[0].compounds[0].combinator, GTEXT_CSS_COMBINATOR_CHILD);
  EXPECT_EQ(written(":has()"), "INVALID") << "\"The :has() pseudo-class ... taking a <relative-selector-list>\", which is unforgiving";
  EXPECT_EQ(written(":has(a, 123)"), "INVALID");
}

TEST(CssSelectorModes, HasDoesNotNestAndTakesNoPseudoElement) {
  // Selectors 4, 4.5: "The :has() pseudo-class cannot be nested inside another :has()"; the pseudo-elements are not valid in it
  EXPECT_EQ(written(":has(:has(a))"), "INVALID");
  EXPECT_EQ(written(":has(::before)"), "INVALID");
  EXPECT_EQ(written(":has(:not(:has(a)))"), "INVALID");
  EXPECT_EQ(written(":not(:has(a))"), ":not(:has(a))");
  EXPECT_EQ(written(":has(:is(:has(*)))"), ":has(:is(:has(*)))") << "the forgiving list keeps the entry that does not nest";
}

TEST(CssSelectorModes, ACompoundArgumentIsOneCompoundAndNoPseudoElement) {
  EXPECT_EQ(written(":host(.a.b)"), ":host(.a.b)");
  EXPECT_EQ(written(":host(.a .b)"), "INVALID");
  EXPECT_EQ(written(":host(.a, .b)"), "INVALID");
  EXPECT_EQ(written(":host()"), "INVALID");
  EXPECT_EQ(written(":host(::before)"), "INVALID");
  EXPECT_EQ(written(":host(:not(.a .b))"), "INVALID") << "a logical argument inside a compound argument is a compound";
  EXPECT_EQ(written(":host(:not(.a))"), ":host(:not(.a))");
  EXPECT_EQ(written("::slotted(:has(:first-child:last-child))"), "::slotted(:has(:first-child:last-child))");
  EXPECT_EQ(written(":host-context(div.a)"), ":host-context(div.a)");
  EXPECT_EQ(written(":host-context(.a, .b)"), "INVALID");
}

TEST(CssSelectorModes, ARealListHasNoPseudoElementAndTheModeCanBeAskedFor) {
  EXPECT_EQ(written(":not(::before)"), "INVALID");
  EXPECT_EQ(written(":not()"), "INVALID");
  EXPECT_EQ(written(":not(:not())"), "INVALID");
  EXPECT_EQ(written(":not(.a, :unknownpseudo)"), "INVALID") << "unforgiving";
  EXPECT_EQ(written(".a, .b", GTEXT_CSS_SELECTOR_REAL_LIST), ".a, .b");
  EXPECT_EQ(written("::before", GTEXT_CSS_SELECTOR_REAL_LIST), "INVALID");
  EXPECT_EQ(written("::before", GTEXT_CSS_SELECTOR_COMPLEX_LIST), "::before");
  EXPECT_EQ(written(".a.b", GTEXT_CSS_SELECTOR_COMPOUND), ".a.b");
  EXPECT_EQ(written(".a .b", GTEXT_CSS_SELECTOR_COMPOUND), "INVALID");
  EXPECT_EQ(written("a, junk!", GTEXT_CSS_SELECTOR_FORGIVING_LIST), "a, junk!");
}

/* ---- the simple selectors ---------------------------------------------------------------------------------- */

TEST(CssSelectorSimple, TypeUniversalIdClassAndTheirWriting) {
  EXPECT_EQ(written("div"), "div");
  EXPECT_EQ(written("DIV"), "DIV") << "the case is kept: whether it matters is the document's";
  EXPECT_EQ(written("*"), "*");
  EXPECT_EQ(written("*.a"), ".a") << "a universal selector that others follow is not written";
  EXPECT_EQ(written("*:hover"), ":hover");
  EXPECT_EQ(written("div :first-child"), "div :first-child");
  EXPECT_EQ(written("div *:first-child"), "div :first-child") << "the universal selector is not written before another simple selector";
  EXPECT_EQ(written("div#a.b.c"), "div#a.b.c");
  EXPECT_EQ(written("#1"), "INVALID") << "an id selector is a hash token with the id type flag";
  EXPECT_EQ(written(".1"), "INVALID");
  EXPECT_EQ(written("a.b.c#d"), "a.b.c#d");
  EXPECT_EQ(written("a*"), "INVALID");
  EXPECT_EQ(written(".a*"), "INVALID");
  EXPECT_EQ(written("\\31 23"), "\\31 23") << "an identifier is written as CSSOM writes one";
  EXPECT_EQ(written(".\\@x"), ".\\@x");
  EXPECT_EQ(written("a>b"), "a > b");
  EXPECT_EQ(written("a   +   b ~ c"), "a + b ~ c");
  EXPECT_EQ(written("a >"), "INVALID");
  EXPECT_EQ(written("> a"), "INVALID");
  EXPECT_EQ(written("a > > b"), "INVALID");
}

TEST(CssSelectorSimple, AttributeSelectorsOfEveryOperatorAndFlag) {
  EXPECT_EQ(written("[a]"), "[a]");
  EXPECT_EQ(written("[ a ]"), "[a]");
  EXPECT_EQ(written("[a=b]"), "[a=\"b\"]");
  EXPECT_EQ(written("[a~=b]"), "[a~=\"b\"]");
  EXPECT_EQ(written("[a|=b]"), "[a|=\"b\"]");
  EXPECT_EQ(written("[a^=b]"), "[a^=\"b\"]");
  EXPECT_EQ(written("[a$=b]"), "[a$=\"b\"]");
  EXPECT_EQ(written("[a*=b]"), "[a*=\"b\"]");
  EXPECT_EQ(written("[a=\"b c\" i]"), "[a=\"b c\" i]");
  EXPECT_EQ(written("[a=b s]"), "[a=\"b\" s]");
  EXPECT_EQ(written("[a=b I]"), "[a=\"b\" i]") << "the flag is an ASCII case-insensitive keyword";
  EXPECT_EQ(written("[a=\"b\"i]"), "[a=\"b\" i]");
  EXPECT_EQ(written("[a=1]"), "INVALID") << "the value is an identifier or a string";
  EXPECT_EQ(written("[a=b x]"), "INVALID");
  EXPECT_EQ(written("[a=b i s]"), "INVALID");
  EXPECT_EQ(written("[=b]"), "INVALID");
  EXPECT_EQ(written("[a b]"), "INVALID");
  EXPECT_EQ(written("[a!=b]"), "INVALID");
  EXPECT_EQ(written("[a=\"x\\\"y\"]"), "[a=\"x\\\"y\"]");
  Parsed p;
  parse(p, "[a$=v i]");
  ASSERT_TRUE(p.valid());
  const GTEXT_CSS_Simple & s = p.list->complex[0].compounds[0].simples[0];
  EXPECT_EQ(s.kind, GTEXT_CSS_SIMPLE_ATTRIBUTE);
  EXPECT_EQ(s.op, GTEXT_CSS_ATTR_SUFFIX);
  EXPECT_EQ(s.attr_case, GTEXT_CSS_ATTR_CASE_INSENSITIVE);
  EXPECT_EQ(std::string(s.value.data, s.value.length), "v");
}

TEST(CssSelectorSimple, NamespacePrefixesNeedTheMapThatDeclaresThem) {
  const GTEXT_CSS_Namespace entries[] = {{"svg", "http://www.w3.org/2000/svg"}, {"h", "http://www.w3.org/1999/xhtml"}};
  GTEXT_CSS_Namespace_Map map{entries, 2, nullptr};
  EXPECT_EQ(written("svg|rect", GTEXT_CSS_SELECTOR_COMPLEX_LIST, &map), "svg|rect");
  EXPECT_EQ(written("svg|*", GTEXT_CSS_SELECTOR_COMPLEX_LIST, &map), "svg|*");
  EXPECT_EQ(written("nope|rect", GTEXT_CSS_SELECTOR_COMPLEX_LIST, &map), "INVALID") << "\"a selector with an undeclared prefix is invalid\"";
  EXPECT_EQ(written("svg|rect"), "INVALID") << "no map: no prefix is declared";
  EXPECT_EQ(written("*|rect"), "rect") << "any namespace is what no default namespace means";
  EXPECT_EQ(written("*|*"), "*");
  EXPECT_EQ(written("*|*:hover"), ":hover");
  EXPECT_EQ(written("|rect"), "|rect");
  EXPECT_EQ(written("[|a]"), "[a]");
  EXPECT_EQ(written("[*|a]"), "[*|a]");
  EXPECT_EQ(written("[svg|a=b]", GTEXT_CSS_SELECTOR_COMPLEX_LIST, &map), "[svg|a=\"b\"]");
  EXPECT_EQ(written("[nope|a]", GTEXT_CSS_SELECTOR_COMPLEX_LIST, &map), "INVALID");
  EXPECT_EQ(written("a|"), "INVALID");
  EXPECT_EQ(written("a| b"), "INVALID");
  EXPECT_EQ(written("a |b"), "a |b") << "white space ends the qualified name: this is a descendant `|b`";
  EXPECT_EQ(written("a | b"), "INVALID");
  Parsed d;
  GTEXT_CSS_Namespace_Map withdefault{entries, 2, "http://www.w3.org/2000/svg"};
  parse(d, "rect, *|g, *", GTEXT_CSS_SELECTOR_COMPLEX_LIST, &withdefault);
  ASSERT_TRUE(d.valid());
  const GTEXT_CSS_Simple & r = d.list->complex[0].compounds[0].simples[0];
  EXPECT_TRUE(r.ns_from_default);
  EXPECT_EQ(std::string(r.ns_uri.data, r.ns_uri.length), "http://www.w3.org/2000/svg");
  EXPECT_EQ(d.text(), "rect, *|g, *");
  EXPECT_EQ(written("*.a", GTEXT_CSS_SELECTOR_COMPLEX_LIST, &withdefault), "*.a") << "with a default namespace the universal selector is what keeps the others in it";
  EXPECT_EQ(written("*|*.a", GTEXT_CSS_SELECTOR_COMPLEX_LIST, &withdefault), ".a");
}

TEST(CssSelectorSimple, TheNthFamilyReadsAnPlusBAndOfS) {
  EXPECT_EQ(written(":nth-child(2n+1)"), ":nth-child(2n+1)");
  EXPECT_EQ(written(":nth-child( 2n + 1 )"), ":nth-child(2n+1)");
  EXPECT_EQ(written(":nth-child(odd)"), ":nth-child(2n+1)");
  EXPECT_EQ(written(":nth-child(even)"), ":nth-child(2n)");
  EXPECT_EQ(written(":nth-child(-n+3)"), ":nth-child(-n+3)");
  EXPECT_EQ(written(":nth-child(5)"), ":nth-child(5)");
  EXPECT_EQ(written(":nth-child(n)"), ":nth-child(n)");
  EXPECT_EQ(written(":nth-child(+n)"), ":nth-child(n)");
  EXPECT_EQ(written(":nth-child(2n+1 of li.x, p)"), ":nth-child(2n+1 of li.x, p)");
  EXPECT_EQ(written(":nth-child(of .a)"), "INVALID");
  EXPECT_EQ(written(":nth-child()"), "INVALID");
  EXPECT_EQ(written(":nth-child(2n+)"), "INVALID");
  EXPECT_EQ(written(":nth-child(1.5)"), "INVALID");
  EXPECT_EQ(written(":nth-child(2n of ::before)"), "INVALID");
  EXPECT_EQ(written(":nth-of-type(2n of .a)"), "INVALID") << "`of S` is for the child forms only";
  EXPECT_EQ(written(":nth-last-of-type(3n-1)"), ":nth-last-of-type(3n-1)");
  Parsed p;
  parse(p, ":nth-child(-3n+7 of .a)");
  ASSERT_TRUE(p.valid());
  const GTEXT_CSS_Simple & s = p.list->complex[0].compounds[0].simples[0];
  EXPECT_EQ(s.anb_a, -3);
  EXPECT_EQ(s.anb_b, 7);
  EXPECT_TRUE(s.has_of);
}

TEST(CssSelectorSimple, LangDirHeadingAndStateArguments) {
  EXPECT_EQ(written(":lang(en)"), ":lang(en)");
  EXPECT_EQ(written(":lang(\"*-CH\", de)"), ":lang(\"*-CH\", de)");
  EXPECT_EQ(written(":lang(\"\")"), ":lang(\"\")");
  EXPECT_EQ(written(":lang()"), "INVALID");
  EXPECT_EQ(written(":lang(1)"), "INVALID");
  EXPECT_EQ(written(":lang(en,)"), "INVALID");
  EXPECT_EQ(written(":dir(LTR)"), ":dir(ltr)");
  EXPECT_EQ(written(":dir(up)"), "INVALID");
  EXPECT_EQ(written(":heading(1, 2)"), ":heading(1, 2)");
  EXPECT_EQ(written(":heading(1.5)"), "INVALID");
  EXPECT_EQ(written(":state(--foo)"), ":state(--foo)");
  EXPECT_EQ(written(":state(0)"), "INVALID");
  EXPECT_EQ(written(":state(a b)"), "INVALID");
}

TEST(CssSelectorSimple, TheNestingSelectorIsAKindOfItsOwn) {
  Parsed p;
  parse(p, "& > .a, .b &");
  ASSERT_TRUE(p.valid());
  EXPECT_EQ(p.text(), "& > .a, .b &");
  EXPECT_EQ(p.list->complex[0].compounds[0].simples[0].kind, GTEXT_CSS_SIMPLE_NESTING);
  EXPECT_TRUE(p.list->complex[0].anchor_dependent);
  EXPECT_FALSE(p.list->complex[1].compounds[0].simples[0].kind == GTEXT_CSS_SIMPLE_NESTING);
  Parsed q;
  parse(q, ".a:is(&)");
  ASSERT_TRUE(q.valid());
  EXPECT_TRUE(q.list->has_nesting) << "a nesting selector inside a functional pseudo-class counts";
}

/* ---- pseudo-elements ---------------------------------------------------------------------------------------- */

TEST(CssSelectorPseudoElements, TheyEndTheComplexSelectorAndFollowTheirRules) {
  EXPECT_EQ(written("a::before"), "a::before");
  EXPECT_EQ(written("a:before"), "a::before") << "the legacy single colon spelling is the same pseudo-element";
  EXPECT_EQ(written("a:AFTER"), "a::after");
  EXPECT_EQ(written("a::before b"), "INVALID");
  EXPECT_EQ(written("::before::marker"), "::before::marker");
  EXPECT_EQ(written("::marker::marker"), "INVALID");
  EXPECT_EQ(written("::before::before"), "INVALID");
  EXPECT_EQ(written("::before .a"), "INVALID");
  EXPECT_EQ(written("::before.a"), "INVALID");
  EXPECT_EQ(written("::before:hover"), "::before:hover") << "a user action pseudo-class may follow";
  EXPECT_EQ(written("::before:first-child"), "INVALID");
  EXPECT_EQ(written("::before:not(:hover)"), "::before:not(:hover)");
  EXPECT_EQ(written("::before:not(.a)"), "INVALID");
  EXPECT_EQ(written("::before:before"), "INVALID") << "the legacy spelling is only for the first";
  EXPECT_EQ(written("::-webkit-foo"), "INVALID");
}

TEST(CssSelectorPseudoElements, PartAndSlottedTakeWhatTheShadowDraftsAllow) {
  // CSS Shadow Parts 1: "The ::part() pseudo-element can take additional pseudo-classes after it ... but never matches the structural pseudo-classes"
  EXPECT_EQ(written("::part(a):hover"), "::part(a):hover");
  EXPECT_EQ(written("::part(a):state(b)"), "::part(a):state(b)");
  EXPECT_EQ(written("::part(a):first-child"), "INVALID");
  EXPECT_EQ(written("::part(a):nth-child(2)"), "INVALID");
  EXPECT_EQ(written("::part(a):has(:hover)"), "INVALID");
  EXPECT_EQ(written("::part(a):not(:hover)"), "::part(a):not(:hover)");
  EXPECT_EQ(written("::part(a):not(:first-child)"), "INVALID");
  EXPECT_EQ(written("::part(a):is(:first-child)"), "::part(a):is(:first-child)") << "accepted, and it can never match";
  Parsed p;
  parse(p, "::part(a):is(:first-child)");
  ASSERT_TRUE(p.valid());
  const GTEXT_CSS_Simple & is = p.list->complex[0].compounds[0].simples[1];
  EXPECT_TRUE(is.list->complex[0].compounds[0].simples[0].never);
  EXPECT_EQ(written("::part(a)::before"), "::part(a)::before");
  EXPECT_EQ(written("::part(a)::part(b)"), "INVALID");
  EXPECT_EQ(written("::part()"), "INVALID");
  EXPECT_EQ(written("::part(a, b)"), "INVALID");
  EXPECT_EQ(written("::part('a')"), "INVALID");
  EXPECT_EQ(written("::part(0)"), "INVALID");
  EXPECT_EQ(written("::slotted(*)::before"), "::slotted(*)::before");
  EXPECT_EQ(written("::slotted(*):hover"), "INVALID");
  EXPECT_EQ(written("::slotted(*)::slotted(*)"), "INVALID");
  EXPECT_EQ(written("::slotted(*)::first-line"), "INVALID");
  EXPECT_EQ(written("::slotted(*).a"), "INVALID");
  EXPECT_EQ(written("::slotted()"), "INVALID");
  EXPECT_EQ(written("::slotted(*):is(#id)"), "::slotted(*):is(#id)");
  EXPECT_EQ(written("::slotted(*):not(.a)"), "INVALID");
}

TEST(CssSelectorPseudoElements, TheArgumentsOfTheOthers) {
  EXPECT_EQ(written("::picker(SELECT)"), "::picker(select)");
  EXPECT_EQ(written("::picker(foo)"), "INVALID");
  EXPECT_EQ(written("::scroll-button(*)"), "::scroll-button(*)");
  EXPECT_EQ(written("::scroll-button( up )"), "::scroll-button(up)");
  EXPECT_EQ(written("::scroll-button(north)"), "INVALID");
  EXPECT_EQ(written("::scroll-button(up):disabled"), "::scroll-button(up):disabled");
  EXPECT_EQ(written("::highlight(*)"), "::highlight(*)");
  EXPECT_EQ(written("::highlight(\\*)"), "::highlight(\\*)");
  EXPECT_EQ(written("::highlight(\\31\\32\\33)"), "::highlight(\\31 23)");
  EXPECT_EQ(written("::highlight()"), "INVALID");
  EXPECT_EQ(written("::view-transition-group(root.a.b)"), "::view-transition-group(root.a.b)");
  EXPECT_EQ(written("::view-transition-group(*)"), "::view-transition-group(*)");
  EXPECT_EQ(written("::view-transition(*)"), "INVALID");
  EXPECT_EQ(written("::view-transition:only-child"), "INVALID");
  EXPECT_EQ(written("::column"), "::column");
  EXPECT_EQ(written("::column::scroll-marker"), "::column::scroll-marker");
  EXPECT_EQ(written("::column:hover"), "INVALID");
}

TEST(CssSelectorPseudoElements, ThePageSelectorsAreNotSelectorsOfAStyleRule) {
  for (const char * s : {":first", ":left", ":right", ":nth(2)", ":first-of-page", ":link-to(x)"}) {
    EXPECT_EQ(written(s), "INVALID") << s;
  }
}

/* ---- ids, spans, diagnostics, limits ------------------------------------------------------------------------- */

TEST(CssSelectorIdentity, EveryNodeHasAnIdAndASpanInTheBytesItWasReadFrom) {
  const std::string text = "ul > li.a:not(.b), p";
  Parsed p;
  parse(p, text);
  ASSERT_TRUE(p.valid());
  EXPECT_EQ(p.list->count, 2u);
  std::vector<GTEXT_CSS_Id> ids;
  const GTEXT_CSS_Complex & c0 = p.list->complex[0];
  ids.push_back(c0.id);
  EXPECT_EQ(c0.span.offset, 0u);
  EXPECT_EQ(c0.span.end, 17u);
  ASSERT_EQ(c0.compound_count, 2u);
  EXPECT_EQ(c0.compounds[1].combinator, GTEXT_CSS_COMBINATOR_CHILD);
  EXPECT_EQ(c0.compounds[1].span.offset, 5u);
  for (size_t i = 0; i < c0.compound_count; i++) {
    ids.push_back(c0.compounds[i].id);
    for (size_t k = 0; k < c0.compounds[i].simple_count; k++) {
      const GTEXT_CSS_Simple & s = c0.compounds[i].simples[k];
      ids.push_back(s.id);
      EXPECT_LE(s.span.offset, s.span.end);
      EXPECT_LE(s.span.end, text.size());
    }
  }
  const GTEXT_CSS_Simple & li = c0.compounds[1].simples[0];
  EXPECT_EQ(text.substr(li.span.offset, li.span.end - li.span.offset), "li");
  const GTEXT_CSS_Simple & cls = c0.compounds[1].simples[1];
  EXPECT_EQ(text.substr(cls.span.offset, cls.span.end - cls.span.offset), ".a");
  const GTEXT_CSS_Simple & nt = c0.compounds[1].simples[2];
  EXPECT_EQ(text.substr(nt.span.offset, nt.span.end - nt.span.offset), ":not(.b)");
  EXPECT_EQ(nt.span.column, 10u);
  std::sort(ids.begin(), ids.end());
  EXPECT_TRUE(std::adjacent_find(ids.begin(), ids.end()) == ids.end()) << "ids are unique";
  Parsed again;
  parse(again, text);
  ASSERT_TRUE(again.valid());
  EXPECT_EQ(again.list->complex[0].compounds[1].simples[2].id, nt.id) << "an id is stable: the same text gives the same ids";
  EXPECT_NE(again.list->uid, p.list->uid) << "every parse has a serial of its own";
}

TEST(CssSelectorIdentity, ASelectorReadFromAPreludeKeepsTheSpansOfTheStyleSheet) {
  const std::string sheet = "/* x */ a > b:hover { color: red }";
  GTEXT_CSS_Tree * tree = nullptr;
  ASSERT_EQ(gtext_css_parse_stylesheet(sheet.data(), sheet.size(), nullptr, &tree, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(tree->rule_count, 1u);
  const GTEXT_CSS_Rule & rule = tree->rules[0];
  GTEXT_CSS_Selector_List * list = nullptr;
  GTEXT_CSS_Selector_Result result;
  std::memset(&result, 0, sizeof result);
  ASSERT_EQ(gtext_css_selector_parse_values(rule.prelude, rule.prelude_count, nullptr, &list, &result), GTEXT_CSS_OK);
  ASSERT_NE(list, nullptr);
  const GTEXT_CSS_Simple & b = list->complex[0].compounds[1].simples[0];
  EXPECT_EQ(sheet.substr(b.span.offset, b.span.end - b.span.offset), "b");
  EXPECT_EQ(b.span.offset, 12u) << "offsets are into the style sheet's bytes";
  gtext_css_selector_list_free(list);
  gtext_css_selector_result_free(&result);
  gtext_css_tree_free(tree);
}

TEST(CssSelectorLimits, EachLimitIsItsOwnStatusAndNoListComesOfIt) {
  GTEXT_CSS_Selector_Options o = gtext_css_selector_options_default();
  GTEXT_CSS_Selector_List * l = nullptr;
  GTEXT_CSS_Selector_Result r;
  std::memset(&r, 0, sizeof r);
  o.max_components = 10;
  std::string many = "a";
  for (int i = 0; i < 20; i++) {
    many += ".c";
  }
  EXPECT_EQ(gtext_css_selector_parse(many.data(), many.size(), &o, &l, &r), GTEXT_CSS_E_LIMIT_SELECTOR_COMPONENTS);
  EXPECT_EQ(l, nullptr);
  EXPECT_GT(r.failure.end, 0u) << "the limit has a place";
  gtext_css_selector_result_free(&r);
  o = gtext_css_selector_options_default();
  o.max_list = 3;
  EXPECT_EQ(gtext_css_selector_parse("a,b,c,d", 7, &o, &l, &r), GTEXT_CSS_E_LIMIT_SELECTOR_LIST);
  gtext_css_selector_result_free(&r);
  o = gtext_css_selector_options_default();
  o.max_depth = 4;
  std::string deep;
  for (int i = 0; i < 10; i++) {
    deep += ":is(";
  }
  deep += "a";
  for (int i = 0; i < 10; i++) {
    deep += ")";
  }
  EXPECT_EQ(gtext_css_selector_parse(deep.data(), deep.size(), &o, &l, &r), GTEXT_CSS_E_LIMIT_SELECTOR_DEPTH);
  gtext_css_selector_result_free(&r);
  o = gtext_css_selector_options_default();
  deep.clear();
  for (int i = 0; i < 60; i++) {
    deep += ":not(";
  }
  deep += "a";
  for (int i = 0; i < 60; i++) {
    deep += ")";
  }
  Parsed ok;
  parse(ok, deep);
  EXPECT_TRUE(ok.valid()) << "60 deep is within the default of 64";
  std::string deeper;
  for (int i = 0; i < 300; i++) {
    deeper += ":is(";
  }
  Parsed d2;
  parse(d2, deeper);
  EXPECT_EQ(d2.status, GTEXT_CSS_E_LIMIT_SELECTOR_DEPTH) << "a selector nested beyond the limit never recurses far";
  o = gtext_css_selector_options_default();
  o.max_depth = 100000;
  EXPECT_EQ(gtext_css_selector_parse(deeper.data(), deeper.size(), &o, &l, &r), GTEXT_CSS_E_LIMIT_SELECTOR_DEPTH) << "the depth is capped at 256, whatever is asked";
  std::string far;
  for (int i = 0; i < 5000; i++) {
    far += ":is(";
  }
  o.parse.max_depth = 100000;
  EXPECT_EQ(gtext_css_selector_parse(far.data(), far.size(), &o, &l, &r), GTEXT_CSS_E_LIMIT_SELECTOR_DEPTH);
  gtext_css_selector_result_free(&r);
}

TEST(CssSelectorLimits, DiagnosticsAreKeptUpToTheLimitAndTheTotalIsExact) {
  std::string s = ":is(";
  for (int i = 0; i < 50; i++) {
    s += "::x,";
  }
  s += ")";
  GTEXT_CSS_Selector_Options o = gtext_css_selector_options_default();
  o.max_diagnostics = 8;
  GTEXT_CSS_Selector_List * l = nullptr;
  GTEXT_CSS_Selector_Result r;
  std::memset(&r, 0, sizeof r);
  ASSERT_EQ(gtext_css_selector_parse(s.data(), s.size(), &o, &l, &r), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_selector_result_diagnostic_count(&r), 8u);
  EXPECT_EQ(r.diagnostic_total, 50u);
  GTEXT_CSS_Selector_Diagnostic d;
  ASSERT_TRUE(gtext_css_selector_result_diagnostic(&r, 0, &d));
  EXPECT_EQ(d.kind, GTEXT_CSS_SELDIAG_DROPPED);
  EXPECT_FALSE(gtext_css_selector_result_diagnostic(&r, 8, &d));
  gtext_css_selector_list_free(l);
  gtext_css_selector_result_free(&r);
}

TEST(CssSelectorLimits, BadArgumentsAreRefused) {
  GTEXT_CSS_Selector_List * l = nullptr;
  EXPECT_EQ(gtext_css_selector_parse("a", 1, nullptr, nullptr, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_selector_parse(nullptr, 1, nullptr, &l, nullptr), GTEXT_CSS_E_INVALID);
  GTEXT_CSS_Selector_Options o = gtext_css_selector_options_default();
  o.mode = static_cast<GTEXT_CSS_Selector_Mode>(99);
  EXPECT_EQ(gtext_css_selector_parse("a", 1, &o, &l, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_selector_serialize(nullptr, nullptr), GTEXT_CSS_E_INVALID);
  gtext_css_selector_list_free(nullptr);
  gtext_css_selector_result_free(nullptr);
}

/* ---- what a selector asks of the caller --------------------------------------------------------------------- */

TEST(CssSelectorRequires, TheCallbacksAndStateBitsAMatchWillAskFor) {
  Parsed p;
  parse(p, "div.a > p:hover:not([x]) + :empty");
  ASSERT_TRUE(p.valid());
  GTEXT_CSS_Selector_Requires req;
  gtext_css_selector_requires(p.list, &req);
  auto has = [&](GTEXT_CSS_Element_Callback cb) { return (req.callbacks >> cb) & 1u; };
  EXPECT_TRUE(has(GTEXT_CSS_ELEMENT_CB_LOCAL_NAME));
  EXPECT_TRUE(has(GTEXT_CSS_ELEMENT_CB_CLASS_AT));
  EXPECT_TRUE(has(GTEXT_CSS_ELEMENT_CB_PARENT));
  EXPECT_TRUE(has(GTEXT_CSS_ELEMENT_CB_PREVIOUS_SIBLING));
  EXPECT_TRUE(has(GTEXT_CSS_ELEMENT_CB_STATE));
  EXPECT_TRUE(has(GTEXT_CSS_ELEMENT_CB_ATTRIBUTE));
  EXPECT_TRUE(has(GTEXT_CSS_ELEMENT_CB_CHILD_CONTENT));
  EXPECT_FALSE(has(GTEXT_CSS_ELEMENT_CB_LANG));
  EXPECT_EQ(req.state, GTEXT_CSS_STATE_HOVER);
  EXPECT_FALSE(req.unmatchable);
  Parsed q;
  parse(q, "a:has(> b):lang(de):dir(ltr)::part(x)");
  ASSERT_TRUE(q.valid());
  gtext_css_selector_requires(q.list, &req);
  EXPECT_TRUE(has(GTEXT_CSS_ELEMENT_CB_FIRST_CHILD));
  EXPECT_TRUE(has(GTEXT_CSS_ELEMENT_CB_LANG));
  EXPECT_TRUE(has(GTEXT_CSS_ELEMENT_CB_DIRECTION));
  Parsed r;
  parse(r, "a || b, :nth-col(1), :link, :any-link");
  ASSERT_TRUE(r.valid());
  gtext_css_selector_requires(r.list, &req);
  EXPECT_TRUE(req.unmatchable) << "the column combinator never matches here";
  EXPECT_EQ(req.state, GTEXT_CSS_STATE_LINK | GTEXT_CSS_STATE_VISITED);
}

TEST(CssSelectorRequires, AnInterfaceThatLacksACallbackIsUnsupportedNamingIt) {
  Parsed p;
  parse(p, "a:first-child");
  ASSERT_TRUE(p.valid());
  GTEXT_CSS_Element_Interface iface;
  std::memset(&iface, 0, sizeof iface);
  iface.size = sizeof iface;
  iface.local_name = [](void *, GTEXT_CSS_Element_Handle, GTEXT_CSS_String *) { return false; };
  GTEXT_CSS_Element_Result res;
  EXPECT_EQ(gtext_css_selector_check_interface(&iface, p.list, &res), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_STREQ(res.callback, "previous_sibling");
  iface.previous_sibling = [](void *, GTEXT_CSS_Element_Handle) -> GTEXT_CSS_Element_Handle { return nullptr; };
  EXPECT_EQ(gtext_css_selector_check_interface(&iface, p.list, &res), GTEXT_CSS_OK);
  iface.size = offsetof(GTEXT_CSS_Element_Interface, direction) + sizeof iface.direction;
  Parsed q;
  parse(q, "a:empty");
  EXPECT_EQ(gtext_css_selector_check_interface(&iface, q.list, &res), GTEXT_CSS_E_UNSUPPORTED) << "a table from before the callbacks has none of them";
  EXPECT_STREQ(res.callback, "child_content");
}

TEST(CssSelectorDependencies, TheSummaryNamesWhatAChangeCanAffect) {
  Parsed p;
  parse(p, "div.a[x][y=z] > #i:hover + :nth-child(2) :not(.b)");
  ASSERT_TRUE(p.valid());
  GTEXT_CSS_Selector_Dependencies dep;
  gtext_css_selector_dependencies(p.list, &dep);
  const uint32_t want = GTEXT_CSS_DEP_NAME | GTEXT_CSS_DEP_CLASS | GTEXT_CSS_DEP_ATTR | GTEXT_CSS_DEP_ID | GTEXT_CSS_DEP_STATE | GTEXT_CSS_DEP_TREE |
      GTEXT_CSS_DEP_SIBLING | GTEXT_CSS_DEP_NTH | GTEXT_CSS_DEP_LOGICAL;
  EXPECT_EQ(dep.kinds, want);
  EXPECT_EQ(dep.state, GTEXT_CSS_STATE_HOVER);
  ASSERT_EQ(dep.attribute_count, 2u);
  EXPECT_EQ(std::string(dep.attributes[0].data, dep.attributes[0].length), "x");
  EXPECT_EQ(std::string(dep.attributes[1].data, dep.attributes[1].length), "y");
  Parsed q;
  parse(q, "a:has(> b), c:empty, d:lang(en), e:first-child, f:dir(rtl)");
  ASSERT_TRUE(q.valid());
  gtext_css_selector_dependencies(q.list, &dep);
  EXPECT_TRUE(dep.kinds & GTEXT_CSS_DEP_HAS);
  EXPECT_TRUE(dep.kinds & GTEXT_CSS_DEP_EMPTY);
  EXPECT_TRUE(dep.kinds & GTEXT_CSS_DEP_LANG);
  EXPECT_TRUE(dep.kinds & GTEXT_CSS_DEP_FIRST_LAST);
  EXPECT_TRUE(dep.kinds & GTEXT_CSS_DEP_DIR);
}

/* ---- reference counting, buffers, the element interface ------------------------------------------------------ */

TEST(CssSelectorLifetime, ARetainedListOutlivesTheFirstOwnerAndTheBufferFormsWork) {
  Parsed p;
  parse(p, "a > b");
  ASSERT_TRUE(p.valid());
  GTEXT_CSS_Selector_List * second = gtext_css_selector_list_retain(p.list);
  ASSERT_EQ(second, p.list);
  gtext_css_selector_list_free(p.list);
  char * buf = nullptr;
  size_t n = 0;
  ASSERT_EQ(gtext_css_selector_serialize_buffer(second, nullptr, &buf, &n), GTEXT_CSS_OK);
  EXPECT_EQ(std::string(buf, n), "a > b");
  gtext_css_selector_buffer_free(nullptr, buf);
  Parsed e;
  parse(e, ":is()");
  ASSERT_TRUE(e.valid());
  gtext_css_selector_list_free(second);
  p.list = nullptr;
}

TEST(CssSelectorLifetime, ASinkThatRefusesFailsTheWriteNotThePrint) {
  Parsed p;
  parse(p, "a > b");
  ASSERT_TRUE(p.valid());
  GTEXT_Markup_Sink sink{[](void *, const char *, size_t) { return GTEXT_MARKUP_E_OOM; }, nullptr};
  EXPECT_EQ(gtext_css_selector_serialize(p.list, &sink), GTEXT_CSS_E_WRITE);
}

TEST(CssSelectorElementInterface, AGrownTableKeepsTheOldFieldsAndTheNewCallbacksHaveNames) {
  EXPECT_STREQ(gtext_css_element_callback_name(GTEXT_CSS_ELEMENT_CB_FIRST_CHILD), "first_child");
  EXPECT_STREQ(gtext_css_element_callback_name(GTEXT_CSS_ELEMENT_CB_COLUMN_CELL), "column_cell");
  EXPECT_STREQ(gtext_css_element_callback_name(GTEXT_CSS_ELEMENT_CB_COUNT), "unknown");
  GTEXT_CSS_Element_Interface table;
  std::memset(&table, 0, sizeof table);
  table.size = sizeof table;
  table.first_child = [](void *, GTEXT_CSS_Element_Handle) -> GTEXT_CSS_Element_Handle { return nullptr; };
  EXPECT_TRUE(gtext_css_element_has(&table, GTEXT_CSS_ELEMENT_CB_FIRST_CHILD));
  table.size = offsetof(GTEXT_CSS_Element_Interface, direction) + sizeof table.direction;
  EXPECT_FALSE(gtext_css_element_has(&table, GTEXT_CSS_ELEMENT_CB_FIRST_CHILD)) << "a table built before the callback is smaller, and it is absent";
  GTEXT_CSS_Element_Result r;
  GTEXT_CSS_Element_Handle out;
  EXPECT_EQ(gtext_css_element_get_first_child(&table, nullptr, &out, &r), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_STREQ(r.callback, "first_child");
  const void * scope;
  EXPECT_EQ(gtext_css_element_get_tree_scope(&table, nullptr, &scope, &r), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_STREQ(r.callback, "tree_scope");
  EXPECT_EQ(GTEXT_CSS_STATE_DEFINED, 0x2000000u) << "the first table does not change";
  EXPECT_EQ(GTEXT_CSS_STATE_TARGET_WITHIN, 0x4000000ull) << "the later bits follow";
  EXPECT_NE(GTEXT_CSS_STATE_HAS_SLOTTED, 0ull);
  EXPECT_EQ(gtext_css_namespace_any()->length, 1u);
}
