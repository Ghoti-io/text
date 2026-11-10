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
 * Property-level parsing (css_property.h): the whole-property rules over the generated registry, each test named for the rule
 * it holds. The I/O matrix of the slice (longhand, shorthand, CSS-wide keyword, deferred, invalid, unknown) is the first test.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_registry_test_support.h"

using namespace reg;

namespace {

const GTEXT_CSS_Registry * R() {
  return shared();
}

TEST(CssProperty, TheMatrixOfTheSlice) {
  const GTEXT_CSS_Registry * r = R();
  EXPECT_EQ(parse(r, "margin-top", "10px"), "valid:10px");                                        // longhand: a typed length
  EXPECT_EQ(parse(r, "margin", "1px 2px"), "valid:1px 2px");                                      // shorthand: its grammar's slots
  EXPECT_EQ(parse(r, "width", "revert-layer"), "wide:revert-layer");                              // a CSS-wide keyword
  EXPECT_EQ(parse(r, "margin", "var(--m)"), "pending:var(--m)");                                  // deferred
  EXPECT_EQ(parse(r, "width", "-1px"), "invalid");                                                // invalid
  EXPECT_EQ(parse(r, "-moz-nosuch", "1"), "unknown");                                             // unknown
}

TEST(CssProperty, ALonghandIsATypedLengthAndAShorthandIsOneValueInItsGrammarsSlots) {
  const GTEXT_CSS_Registry * r = R();
  GTEXT_CSS_Property_Value pv;
  ASSERT_EQ(gtext_css_parse_property_text(r, "margin-top", 10, "10px", 4, nullptr, &pv), GTEXT_CSS_OK);
  EXPECT_EQ(pv.outcome, GTEXT_CSS_PROP_RESULT_VALID);
  EXPECT_EQ(pv.property, find(r, "margin-top"));
  EXPECT_EQ(pv.named, pv.property);
  const GTEXT_CSS_Value * v = gtext_css_value_set_root(pv.set);
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->kind, GTEXT_CSS_VAL_DIMENSION);
  EXPECT_EQ(v->unit, GTEXT_CSS_UNIT_PX);
  EXPECT_EQ(v->number, 10.0);
  EXPECT_GT(pv.match.work, 0u);
  gtext_css_property_value_free(&pv);
  EXPECT_EQ(pv.set, nullptr);

  ASSERT_EQ(gtext_css_parse_property_text(r, "margin", 6, "1px 2px 3px", 11, nullptr, &pv), GTEXT_CSS_OK);
  EXPECT_EQ(pv.outcome, GTEXT_CSS_PROP_RESULT_VALID);
  v = gtext_css_value_set_root(pv.set);
  ASSERT_NE(v, nullptr);
  ASSERT_EQ(v->kind, GTEXT_CSS_VAL_LIST);
  ASSERT_EQ(v->child_count, 3u) << "one value, three slots: expansion into the longhands is the cascade's";
  EXPECT_EQ(v->children[2].number, 3.0);
  gtext_css_property_value_free(&pv);
}

// Compatibility Standard: `-webkit-perspective` is `none | <length [0,∞]> | <number [0,∞]>` and the `perspective()` of `-webkit-transform` takes a number beside its length, a number being
// pixels. Chromium 156 and Firefox 158 write `1000px` and `perspective(1000px)` (both accept 1.5, 1e3 and +5 and refuse a negative number; only Chromium accepts calc(1)).
TEST(CssProperty, AWebkitPerspectiveAndAWebkitTransformPerspectiveTakeANumber) {
  const GTEXT_CSS_Registry * r = R();
  EXPECT_EQ(parse(r, "-webkit-perspective", "1000"), "valid:1000px");
  EXPECT_EQ(parse(r, "-webkit-perspective", "25"), "valid:25px");
  EXPECT_EQ(parse(r, "-webkit-perspective", "1.5"), "valid:1.5px");
  EXPECT_EQ(parse(r, "-webkit-perspective", "1e3"), "valid:1000px");
  EXPECT_EQ(parse(r, "-webkit-perspective", "+5"), "valid:5px");
  EXPECT_EQ(parse(r, "-webkit-perspective", "  7  "), "valid:7px");
  EXPECT_EQ(parse(r, "-webkit-perspective", "0"), "valid:0px");
  EXPECT_EQ(parse(r, "-WEBKIT-PERSPECTIVE", "3"), "valid:3px");
  EXPECT_EQ(parse(r, "-webkit-perspective", "none"), "valid:none");
  EXPECT_EQ(parse(r, "-webkit-perspective", "10px"), "valid:10px");
  EXPECT_EQ(parse(r, "-webkit-perspective", "10em"), "valid:10em");
  EXPECT_EQ(parse(r, "-webkit-perspective", "initial"), "wide:initial");
  EXPECT_EQ(parse(r, "-webkit-perspective", "-1"), "invalid") << "<number [0,∞]>";
  EXPECT_EQ(parse(r, "-webkit-perspective", "5%"), "invalid");
  EXPECT_EQ(parse(r, "-webkit-perspective", "1 2"), "invalid");
  EXPECT_EQ(parse(r, "-webkit-perspective", "calc(1)"), "invalid") << "Firefox refuses it and Chromium accepts it: the number alone is the compatibility grammar";
  EXPECT_EQ(parse(r, "perspective", "1000"), "invalid") << "the unprefixed property has no number";
  EXPECT_EQ(parse(r, "perspective", "calc(1)"), "invalid");

  EXPECT_EQ(parse(r, "-webkit-transform", "perspective(1000)"), "valid:perspective(1000px)");
  EXPECT_EQ(parse(r, "-webkit-transform", "perspective(25)"), "valid:perspective(25px)");
  EXPECT_EQ(parse(r, "-webkit-transform", "perspective(1.5)"), "valid:perspective(1.5px)");
  EXPECT_EQ(parse(r, "-webkit-transform", "PERSPECTIVE( 7 )"), "valid:perspective(7px)");
  EXPECT_EQ(parse(r, "-webkit-transform", "translate(10px) perspective(5) rotate(1deg) perspective(6)"), "valid:translate(10px) perspective(5px) rotate(1deg) perspective(6px)");
  EXPECT_EQ(parse(r, "-webkit-transform", "perspective(0)"), "valid:perspective(0px)");
  EXPECT_EQ(parse(r, "-webkit-transform", "perspective(none)"), "valid:perspective(none)");
  EXPECT_EQ(parse(r, "-webkit-transform", "perspective(10px)"), "valid:perspective(10px)");
  EXPECT_EQ(parse(r, "-webkit-transform", "perspective(-1)"), "invalid");
  EXPECT_EQ(parse(r, "-webkit-transform", "perspective(5 6)"), "invalid");
  EXPECT_EQ(parse(r, "-webkit-transform", "perspective(calc(1))"), "invalid");
  EXPECT_EQ(parse(r, "-webkit-transform", "translate(10)"), "invalid") << "only perspective() takes the number";
  EXPECT_EQ(parse(r, "-webkit-transform", "translateZ(5)"), "invalid");
  EXPECT_EQ(parse(r, "-webkit-transform", "translate(perspective(5))"), "invalid") << "a number is not made a length anywhere else";
  EXPECT_EQ(parse(r, "transform", "perspective(1000)"), "invalid") << "the unprefixed property has no number";
  EXPECT_EQ(parse(r, "transform", "perspective(none)"), "valid:perspective(none)");
}

TEST(CssProperty, NamesAreFoldedAndValuesAreValues) {
  const GTEXT_CSS_Registry * r = R();
  EXPECT_EQ(parse(r, "WIDTH", "10PX"), "valid:10px");
  EXPECT_EQ(parse(r, "Display", "BLOCK"), "valid:block");
  EXPECT_EQ(parse(r, "color", "  red  "), "valid:red");
  EXPECT_EQ(parse(r, "color", "RED"), "valid:red");
  EXPECT_EQ(parse(r, "width", ""), "invalid");
  EXPECT_EQ(parse(r, "width", "   "), "invalid");
  EXPECT_EQ(parse(r, "width", "/* c */ 5px /* d */"), "valid:5px");
  EXPECT_EQ(parse(r, "width", "5px 5px"), "invalid");
  EXPECT_EQ(parse(r, "width", "5"), "invalid") << "no quirks";
  EXPECT_EQ(parse(r, "width", "0"), "valid:0px");
}

TEST(CssProperty, CssWideKeywordsAreValidAloneForEveryProperty) {
  const GTEXT_CSS_Registry * r = R();
  const char * names[] = {"width", "margin", "color", "display", "--custom", "all", "background", "-webkit-transform", "-webkit-box-align", "font"};
  const char * keywords[] = {"initial", "inherit", "unset", "revert", "revert-layer", "revert-rule"};
  for (const char * n : names) {
    for (const char * k : keywords) {
      EXPECT_EQ(parse(r, n, k), std::string("wide:") + k) << n << ": " << k;
    }
    EXPECT_EQ(parse(r, n, "INHERIT"), "wide:inherit") << n;
    EXPECT_EQ(parse(r, n, "  Revert-Layer  "), "wide:revert-layer") << n;
  }
  // alone: with anything else it is an ordinary identifier, valid only where the grammar says
  EXPECT_EQ(parse(r, "width", "inherit 1px"), "invalid");
  EXPECT_EQ(parse(r, "width", "1px inherit"), "invalid");
  EXPECT_EQ(parse(r, "margin", "inherit inherit"), "invalid");
  EXPECT_EQ(parse(r, "width", "inherit, inherit"), "invalid");
  EXPECT_EQ(parse(r, "width", "'inherit'"), "invalid") << "a string is not a keyword";
  EXPECT_EQ(parse(r, "width", "inherit(--a)"), "pending:inherit(--a)") << "a function of that name is a substitution function of Values 5, not the keyword";
  EXPECT_EQ(parse(r, "width", "inherit()"), "invalid") << "and it takes a first argument, as var() does";
  EXPECT_EQ(parse(r, "width", "unknownkw"), "invalid");
  GTEXT_CSS_Property_Value pv;
  ASSERT_EQ(gtext_css_parse_property_text(r, "width", 5, "unset", 5, nullptr, &pv), GTEXT_CSS_OK);
  EXPECT_EQ(pv.outcome, GTEXT_CSS_PROP_RESULT_WIDE);
  EXPECT_EQ(pv.wide, GTEXT_CSS_WIDE_UNSET);
  EXPECT_EQ(pv.set, nullptr);
  gtext_css_property_value_free(&pv);
}

TEST(CssProperty, TheWideKeywordsHaveNamesAndAreLookedUpWithoutCase) {
  EXPECT_EQ(gtext_css_wide_keyword_lookup("Revert-Rule", 11), GTEXT_CSS_WIDE_REVERT_RULE);
  EXPECT_EQ(gtext_css_wide_keyword_lookup("initial", 7), GTEXT_CSS_WIDE_INITIAL);
  EXPECT_EQ(gtext_css_wide_keyword_lookup("initia", 6), GTEXT_CSS_WIDE_NONE);
  EXPECT_EQ(gtext_css_wide_keyword_lookup("initials", 8), GTEXT_CSS_WIDE_NONE);
  EXPECT_EQ(gtext_css_wide_keyword_lookup("auto", 4), GTEXT_CSS_WIDE_NONE);
  EXPECT_EQ(gtext_css_wide_keyword_lookup(nullptr, 4), GTEXT_CSS_WIDE_NONE);
  EXPECT_EQ(std::string(gtext_css_wide_keyword_name(GTEXT_CSS_WIDE_REVERT_LAYER)), "revert-layer");
  EXPECT_EQ(gtext_css_wide_keyword_name(GTEXT_CSS_WIDE_NONE), nullptr);
  for (int k = GTEXT_CSS_WIDE_INITIAL; k <= GTEXT_CSS_WIDE_REVERT_RULE; k++) {
    const char * n = gtext_css_wide_keyword_name((GTEXT_CSS_Wide_Keyword)k);
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(gtext_css_wide_keyword_lookup(n, strlen(n)), (GTEXT_CSS_Wide_Keyword)k);
  }
}

TEST(CssProperty, AllAcceptsTheCssWideKeywordsAndNothingElse) {
  const GTEXT_CSS_Registry * r = R();
  EXPECT_EQ(parse(r, "all", "inherit"), "wide:inherit");
  EXPECT_EQ(parse(r, "all", "revert-rule"), "wide:revert-rule");
  EXPECT_EQ(parse(r, "all", "1px"), "invalid");
  EXPECT_EQ(parse(r, "all", "auto"), "invalid");
  EXPECT_EQ(parse(r, "all", "var(--x)"), "pending:var(--x)") << "`all` takes a CSS-wide keyword, which a function may yield: it is pending, invalid at computed-value time otherwise";
  EXPECT_EQ(parse(r, "all", ""), "invalid");
}

TEST(CssProperty, ASubstitutionAnywhereGivesAPendingValueValidAtParseTime) {
  const GTEXT_CSS_Registry * r = R();
  EXPECT_EQ(parse(r, "color", "var(--c)"), "pending:var(--c)");
  EXPECT_EQ(parse(r, "color", "var(--c, red)"), "pending:var(--c, red)");
  EXPECT_EQ(parse(r, "margin", "1px var(--x)"), "pending:1px var(--x)");
  EXPECT_EQ(parse(r, "margin", "1px 2px 3px 4px 5px var(--x)"), "pending:1px 2px 3px 4px 5px var(--x)") << "valid or not, it is not known yet";
  EXPECT_EQ(parse(r, "width", "calc(var(--x) * 2)"), "pending:calc(var(--x) * 2)");
  EXPECT_EQ(parse(r, "width", "calc(1px + calc(2px + env(safe-area-inset-top)))"), "pending:calc(1px + calc(2px + env(safe-area-inset-top)))");
  EXPECT_EQ(parse(r, "content", "attr(title)"), "pending:attr(title)");
  EXPECT_EQ(parse(r, "color", "VAR(--c)"), "pending:VAR(--c)") << "function names fold";
  EXPECT_EQ(parse(r, "font", "12px var(--family)"), "pending:12px var(--family)");
  EXPECT_EQ(parse(r, "-webkit-box-align", "var(--a)"), "pending:var(--a)") << "even where the pin gives no grammar";
  EXPECT_EQ(parse(r, "--x", "var(--y)"), "pending:var(--y)");
  GTEXT_CSS_Property_Value pv;
  ASSERT_EQ(gtext_css_parse_property_text(r, "margin", 6, "var(--m)", 8, nullptr, &pv), GTEXT_CSS_OK);
  EXPECT_EQ(pv.outcome, GTEXT_CSS_PROP_RESULT_PENDING);
  const GTEXT_CSS_Value * v = gtext_css_value_set_root(pv.set);
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->kind, GTEXT_CSS_VAL_PENDING);
  gtext_css_property_value_free(&pv);
}

TEST(CssProperty, CustomPropertiesKeepTheirCaseAndTakeAnyValue) {
  const GTEXT_CSS_Registry * r = R();
  EXPECT_EQ(parse(r, "--foo", "1px solid"), "valid:1px solid");
  EXPECT_EQ(parse(r, "--Foo", "ABC"), "valid:ABC");
  EXPECT_EQ(parse(r, "--foo", ""), "valid:") << "`--foo:;` is the empty value";
  EXPECT_EQ(parse(r, "--foo", "{ a: b }"), "valid:{ a: b }");
  EXPECT_EQ(parse(r, "--foo", "1px ) 2px"), "invalid") << "an unmatched close is not a <declaration-value>";
  GTEXT_CSS_Property_Value a, b;
  ASSERT_EQ(gtext_css_parse_property_text(r, "--Foo", 5, "x", 1, nullptr, &a), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_parse_property_text(r, "--foo", 5, "x", 1, nullptr, &b), GTEXT_CSS_OK);
  EXPECT_EQ(a.property, b.property) << "both are the pattern of the pin; the name is the declaration's";
  gtext_css_property_value_free(&a);
  gtext_css_property_value_free(&b);
}

TEST(CssProperty, ALegacyAliasIsParsedAsItsTarget) {
  const GTEXT_CSS_Registry * r = R();
  GTEXT_CSS_Property_Value pv;
  ASSERT_EQ(gtext_css_parse_property_text(r, "-webkit-transform", 17, "rotate(3deg)", 12, nullptr, &pv), GTEXT_CSS_OK);
  EXPECT_EQ(pv.outcome, GTEXT_CSS_PROP_RESULT_VALID);
  EXPECT_EQ(pv.named, find(r, "-webkit-transform"));
  EXPECT_EQ(pv.property, find(r, "transform"));
  gtext_css_property_value_free(&pv);
  EXPECT_EQ(parse(r, "-webkit-transform", "rotate(3deg)"), "valid:rotate(3deg)");
  EXPECT_EQ(parse(r, "word-wrap", "break-word"), "valid:break-word");
  EXPECT_EQ(parse(r, "-webkit-box-shadow", "1px 2px red"), parse(r, "box-shadow", "1px 2px red"));
  EXPECT_EQ(parse(r, "-webkit-transform", "bogus"), "invalid");
}

TEST(CssProperty, AnUnknownNameIsUnknownWhateverTheValue) {
  const GTEXT_CSS_Registry * r = R();
  EXPECT_EQ(parse(r, "-moz-nosuch", "inherit"), "unknown");
  EXPECT_EQ(parse(r, "nosuch", "1px"), "unknown");
  EXPECT_EQ(parse(r, "-o-transform", "none"), "unknown");
  EXPECT_EQ(parse(r, "width ", "1px"), "unknown");
  EXPECT_EQ(parse(r, "", "1px"), "unknown");
  GTEXT_CSS_Property_Value pv;
  ASSERT_EQ(gtext_css_parse_property_text(r, "nosuch", 6, "1px", 3, nullptr, &pv), GTEXT_CSS_OK);
  EXPECT_EQ(pv.outcome, GTEXT_CSS_PROP_RESULT_UNKNOWN);
  EXPECT_EQ(pv.property, 0u);
  EXPECT_EQ(pv.set, nullptr);
}

TEST(CssProperty, ATypeThePinDefinesInsideOnePropertyIsFoundFromTheOthersThatNameIt) {
  const GTEXT_CSS_Registry * r = R();
  // `fill-opacity` names <opacity-value>, which the pin defines inside `opacity`; `backdrop-filter` names the functions of `filter`
  EXPECT_EQ(parse(r, "fill-opacity", "0.5"), "valid:0.5");
  EXPECT_EQ(parse(r, "fill-opacity", "50%"), "valid:0.5") << "a percentage opacity is written as the number, the canonical form";
  EXPECT_EQ(parse(r, "fill-opacity", "bogus"), "invalid");
  EXPECT_EQ(parse(r, "backdrop-filter", "blur(2px) brightness(50%)"), "valid:blur(2px) brightness(50%)");
  EXPECT_EQ(parse(r, "grid-row", "1 / 3"), "valid:1 / 3") << "<grid-line> is defined in the four grid-row/column start/end properties";
  EXPECT_EQ(parse(r, "grid-area", "a / b"), "valid:a / b");
  // the library's own function types answer before the nested ones of another property: the basic shape rect(), not the CSS 2 clip rect()
  EXPECT_EQ(parse(r, "clip-path", "rect(1px 2px 3px 4px)"), "valid:rect(1px 2px 3px 4px)");
}

TEST(CssProperty, ARangeTheServedDraftGivesAPropertyIsHeldWhereThePinLacksIt) {
  // tools/css/consolidate.tsv replace-property-grammar: "A negative value for r is invalid", "Negative values are invalid", "Values less than 1 are invalid"
  const GTEXT_CSS_Registry * r = R();
  for (const char * property : {"r", "rx", "ry", "stroke-width"}) {
    EXPECT_EQ(parse(r, property, "-1px"), "invalid") << property;
    // Values 4, 6.1: "if a 0 could be parsed as either a <number> or a <length> in a property (such as line-height), it must parse as a <number>"; the computed value is still 0px
    EXPECT_EQ(parse(r, property, "0"), std::string(std::string(property) == "stroke-width" ? "valid:0" : "valid:0px")) << property;
  }
  EXPECT_EQ(parse(r, "rx", "auto"), "valid:auto");
  EXPECT_EQ(parse(r, "stroke-width", "-1"), "invalid");
  EXPECT_EQ(parse(r, "stroke-width", "2px, 3"), "valid:2px, 3");
  EXPECT_EQ(parse(r, "stroke-miterlimit", "0.5"), "invalid");
  EXPECT_EQ(parse(r, "stroke-miterlimit", "-1"), "invalid");
  EXPECT_EQ(parse(r, "stroke-miterlimit", "1"), "valid:1");
}

TEST(CssProperty, ANumberBelowSixDecimalsIsWrittenAsZeroAndReadAgainAsALength) {
  // CSSOM writes at most six decimals, so a tiny <number> is `0`; and a literal `0` is the <number> where the grammar offers both (Values 4, 6.1), so it is settled at once.
  const GTEXT_CSS_Registry * r = R();
  EXPECT_EQ(parse(r, "stroke-width", "0.0000001"), "valid:0");
  EXPECT_EQ(parse(r, "stroke-width", "0"), "valid:0");
  EXPECT_EQ(parse(r, "stroke-width", "0px"), "valid:0px");
}

TEST(CssProperty, AShorthandsTypedValueFollowsItsGrammar) {
  const GTEXT_CSS_Registry * r = R();
  EXPECT_EQ(parse(r, "border", "1px solid red"), "valid:1px solid red");
  EXPECT_EQ(parse(r, "border", "red"), "valid:red");
  EXPECT_EQ(parse(r, "border", "solid 1px"), "valid:1px solid") << "|| is written in the grammar's order";
  EXPECT_EQ(parse(r, "flex", "1 1 0%"), "valid:1 1 0%");
  EXPECT_EQ(parse(r, "font", "italic bold 12px/1.5 serif"), "valid:italic bold 12px / 1.5 serif");
  EXPECT_EQ(parse(r, "margin", "1px 2px 3px 4px 5px"), "invalid");
  EXPECT_EQ(parse(r, "transition", "opacity 1s ease-in, color 2s"), "valid:opacity 1s ease-in, color 2s");
  EXPECT_EQ(parse(r, "background", "red url(a.png) no-repeat"), "valid:url(\"a.png\") no-repeat red");
  EXPECT_EQ(parse(r, "grid-template-columns", "repeat(2, 1fr) minmax(10px, 1fr)"), "valid:repeat(2, 1fr) minmax(10px, 1fr)");
}

TEST(CssProperty, ARegisteredPropertiesAreParsedLikeTheGeneratedOnes) {
  Builder b;
  GTEXT_CSS_Property_Def d;
  std::memset(&d, 0, sizeof d);
  d.name = "vector-effect-width";
  d.grammar = "<length [0,∞]> | auto";
  d.initial = "auto";
  ASSERT_EQ(gtext_css_registry_register_property(b.b, &d, nullptr), GTEXT_CSS_OK);
  Owned o;
  ASSERT_EQ(b.freeze(o), GTEXT_CSS_OK);
  EXPECT_EQ(parse(o.r, "vector-effect-width", "3px"), "valid:3px");
  EXPECT_EQ(parse(o.r, "vector-effect-width", "-3px"), "invalid");
  EXPECT_EQ(parse(o.r, "vector-effect-width", "inherit"), "wide:inherit");
  EXPECT_EQ(parse(o.r, "vector-effect-width", "var(--w)"), "pending:var(--w)");
  EXPECT_EQ(parse(o.r, "width", "10px"), "valid:10px") << "the generated layer is there too";
}

// ---- properties the library cannot decide ------------------------------------------------------------------

TEST(CssProperty, APropertyWithNoUsableGrammarIsNotGuessedAt) {
  const GTEXT_CSS_Registry * r = R();
  // no grammar in the pin
  EXPECT_EQ(parse(r, "-webkit-box-align", "center"), "not supported");
  // a grammar that names a type nothing defines: Speech 1, 11.1 gives <voice-family-name> as examples only
  EXPECT_EQ(parse(r, "voice-family", "male"), "not supported") << "\"Values are specific voice instances (e.g., Mike, comedian, mary, carlos2, \"valley girl\").\" names no token form";
  // a text that is not a definition
  EXPECT_EQ(parse(r, "glyph-orientation-vertical", "90deg"), "not supported");
  GTEXT_CSS_Property_Value pv;
  EXPECT_EQ(gtext_css_parse_property_text(r, "voice-family", 12, "male", 4, nullptr, &pv), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_EQ(pv.property, find(r, "voice-family")) << "the property is named, so the consumer can say which";
  EXPECT_EQ(pv.set, nullptr);
  // the CSS-wide keywords and substitutions are still valid for them
  EXPECT_EQ(parse(r, "voice-family", "inherit"), "wide:inherit");
  EXPECT_EQ(parse(r, "voice-family", "var(--c)"), "pending:var(--c)");
}

TEST(CssProperty, GridTemplateAreasStringsHaveACellAndAreWrittenWithTheirWhiteSpaceElided) {
  const GTEXT_CSS_Registry * r = R();
  // Grid 2, 7.3: "A sequence of whitespace, representing nothing (do not produce a token)." "All strings must define the same number of cell tokens" and "at least one
  // cell token, or else the declaration is invalid"; 7.3.1: "each null cell token is serialized as a single "."", "consecutive cell tokens are separated by a single
  // space (U+0020 SPACE)" "with all other white space elided."
  EXPECT_EQ(parse(r, "grid-template-areas", "none"), "valid:none");
  EXPECT_EQ(parse(r, "grid-template-areas", "\"a b\" \"c d\""), "valid:\"a b\" \"c d\"");
  EXPECT_EQ(parse(r, "grid-template-areas", "\"  a  \t  b  \""), "valid:\"a b\"");
  EXPECT_EQ(parse(r, "grid-template-areas", "\"c\td\""), "valid:\"c d\"");
  EXPECT_EQ(parse(r, "grid-template-areas", "\"first ...\""), "valid:\"first .\"");
  EXPECT_EQ(parse(r, "grid-template-areas", "\". .. ...\""), "valid:\". . .\"");
  EXPECT_EQ(parse(r, "grid-template-areas", "\"\""), "invalid");
  EXPECT_EQ(parse(r, "grid-template-areas", "\" \""), "invalid");
  EXPECT_EQ(parse(r, "grid-template-areas", "\".\" \"\""), "invalid");
  EXPECT_EQ(parse(r, "grid-template-areas", "\"first\" \" \" \"second\""), "invalid");
}

TEST(CssProperty, AFontLanguageOverrideIsATagOfOneToFourPrintableAsciiCharacters) {
  const GTEXT_CSS_Registry * r = R();
  // Fonts 4: a "single four-character case-sensitive OpenType language system tag"; "If the string is shorter than four characters, it is padded at the end with
  // space (U+0020) characters such that the length is 4, before being matched."
  EXPECT_EQ(parse(r, "font-language-override", "normal"), "valid:normal");
  EXPECT_EQ(parse(r, "font-language-override", "\"KSW\""), "valid:\"KSW\"");
  EXPECT_EQ(parse(r, "font-language-override", "\"APPH\""), "valid:\"APPH\"");
  EXPECT_EQ(parse(r, "font-language-override", "\"tr\""), "valid:\"tr\"");
  EXPECT_EQ(parse(r, "font-language-override", "\"ENG \""), "valid:\"ENG\"") << "the padding is not part of the shortest form";
  EXPECT_EQ(parse(r, "font-language-override", "\" en \""), "valid:\" en\"");
  EXPECT_EQ(parse(r, "font-language-override", "\"1 %\""), "valid:\"1 %\"");
  EXPECT_EQ(parse(r, "font-language-override", "\"turkish\""), "invalid") << "longer than four characters";
  EXPECT_EQ(parse(r, "font-language-override", "\"ENG  \""), "invalid") << "five characters";
  EXPECT_EQ(parse(r, "font-language-override", "\"x\xc3\xb8x\""), "invalid") << "outside U+20-7E";
  EXPECT_EQ(parse(r, "font-language-override", "\"\""), "invalid") << "both pinned browsers refuse the empty string";
  EXPECT_EQ(parse(r, "font-language-override", "normal \"ksw\""), "invalid");
}

TEST(CssProperty, TheSpeechPropertiesTakeTheDecibelAndSemitoneDimensionsOfSpeech1) {
  const GTEXT_CSS_Registry * r = R();
  // Speech 1, 6.1: "The <decibel> type denotes a dimension with a "dB" (decibel unit) unit identifier."; 11.3: "The syntax of <semitones> allowed values is a dimension
  // with the unit identifier st (semitones)."
  EXPECT_EQ(parse(r, "voice-volume", "silent"), "valid:silent");
  EXPECT_EQ(parse(r, "voice-volume", "x-loud"), "valid:x-loud");
  EXPECT_EQ(parse(r, "voice-volume", "3dB"), "valid:3dB");
  EXPECT_EQ(parse(r, "voice-volume", "-6DB loud"), "valid:loud -6dB") << "the unit is case-insensitive; `||` is written in the grammar's order";
  EXPECT_EQ(parse(r, "voice-volume", "soft 3dB"), "valid:soft 3dB");
  EXPECT_EQ(parse(r, "voice-volume", "3st"), "invalid") << "a semitone is not a decibel";
  EXPECT_EQ(parse(r, "voice-volume", "3"), "invalid") << "a dimension has a unit";
  EXPECT_EQ(parse(r, "voice-volume", "0"), "invalid") << "and a zero has none to leave out: only <length> takes a bare zero";
  EXPECT_EQ(parse(r, "voice-volume", "silent 3dB"), "invalid");
  EXPECT_EQ(parse(r, "voice-volume", "calc(3dB)"), "invalid") << "no calculation takes the units of Speech 1";
  // voice-pitch and voice-range: "<frequency [0Hz,∞]> && absolute | [ [ x-low | low | medium | high | x-high ] || [ <frequency [0Hz,∞]> | <semitones> | <percentage> ] ]"
  for (const char * p : {"voice-pitch", "voice-range"}) {
    EXPECT_EQ(parse(r, p, "2st"), "valid:2st") << p;
    EXPECT_EQ(parse(r, p, "-2.5st"), "valid:-2.5st") << p;
    EXPECT_EQ(parse(r, p, "high 2st"), "valid:high 2st") << p;
    EXPECT_EQ(parse(r, p, "10% low"), "valid:low 10%") << p;
    EXPECT_EQ(parse(r, p, "200Hz absolute"), "valid:200hz absolute") << p;
    EXPECT_EQ(parse(r, p, "2dB"), "invalid") << p;
    EXPECT_EQ(parse(r, p, "-10Hz"), "invalid") << p << " [0Hz,∞]";
  }
  // cue-before / cue-after: "<url> <decibel>? | none"; cue: "<'cue-before'> <'cue-after'>?"
  EXPECT_EQ(parse(r, "cue-before", "none"), "valid:none");
  EXPECT_EQ(parse(r, "cue-before", "url(a.wav)"), "valid:url(\"a.wav\")");
  EXPECT_EQ(parse(r, "cue-after", "url(a.wav) -3dB"), "valid:url(\"a.wav\") -3dB");
  EXPECT_EQ(parse(r, "cue-after", "url(a.wav) 3st"), "invalid");
  EXPECT_EQ(parse(r, "cue-after", "3dB"), "invalid");
  EXPECT_EQ(parse(r, "cue", "url(a.wav) 3dB url(b.wav)"), "valid:url(\"a.wav\") 3dB url(\"b.wav\")");
  EXPECT_EQ(parse(r, "cue", "none"), "valid:none");
  // the units are Speech 1's, not Values': they are not lengths
  EXPECT_EQ(parse(r, "width", "3dB"), "invalid");
  EXPECT_EQ(parse(r, "width", "3st"), "invalid");
}

TEST(CssProperty, TheDirectionalFocusPropertiesTakeAnIdSelectorAndATargetName) {
  const GTEXT_CSS_Registry * r = R();
  // UI 4, 5.3.1: "auto | <id> [ current | root | <target-name> ]?"; "The <id> value is an ID selector"; "The <target-name> parameter indicates the target frame for
  // the focus navigation. It is a <string> and it MUST NOT start with the underscore "_" character."
  for (const char * p : {"nav-up", "nav-right", "nav-down", "nav-left"}) {
    EXPECT_EQ(parse(r, p, "auto"), "valid:auto") << p;
    EXPECT_EQ(parse(r, p, "#a"), "valid:#a") << p;
    EXPECT_EQ(parse(r, p, "#a current"), "valid:#a current") << p;
    EXPECT_EQ(parse(r, p, "#a root"), "valid:#a root") << p;
    EXPECT_EQ(parse(r, p, "#a \"frame\""), "valid:#a \"frame\"") << p;
    EXPECT_EQ(parse(r, p, "#a \"_blank\""), "invalid") << p << " a target name does not start with an underscore";
    EXPECT_EQ(parse(r, p, "#1"), "invalid") << p << " #1 is not an ID selector (its hash token is not of type id)";
    EXPECT_EQ(parse(r, p, "a"), "invalid") << p;
    EXPECT_EQ(parse(r, p, "current"), "invalid") << p;
    EXPECT_EQ(parse(r, p, "#a #b"), "invalid") << p;
  }
}

TEST(CssProperty, TheEventTriggerPropertiesTakeTheEventFunctionsOfAnimationTriggers1) {
  const GTEXT_CSS_Registry * r = R();
  // Animation Triggers 1, 4: "none | <dashed-ident>#"; "[ none | <event-function>+ [ / <event-function>+ ]? ]#"; "none | [ <'event-trigger-name'> <'event-trigger-source'> ]#";
  // 4.2.1: "click() = click()" (the same of dblclick(), pointerenter(), pointerleave(), pointerup(), pointerdown(), focusin(), focusout(), input(), change()), and
  // "keypress() = keypress( <string>? )"
  EXPECT_EQ(parse(r, "event-trigger-name", "none"), "valid:none");
  EXPECT_EQ(parse(r, "event-trigger-name", "--a, --b"), "valid:--a, --b");
  EXPECT_EQ(parse(r, "event-trigger-name", "a"), "invalid");
  EXPECT_EQ(parse(r, "event-trigger-source", "none"), "valid:none");
  for (const char * f : {"click", "dblclick", "pointerenter", "pointerleave", "pointerup", "pointerdown", "focusin", "focusout", "input", "change", "keypress"}) {
    EXPECT_EQ(parse(r, "event-trigger-source", std::string(f) + "()"), std::string("valid:") + f + "()") << f;
  }
  EXPECT_EQ(parse(r, "event-trigger-source", "click() dblclick() / keypress(\"a\")"), "valid:click() dblclick() / keypress(\"a\")");
  EXPECT_EQ(parse(r, "event-trigger-source", "click(), input()"), "valid:click(), input()");
  EXPECT_EQ(parse(r, "event-trigger-source", "keypress(\" \")"), "valid:keypress(\" \")");
  EXPECT_EQ(parse(r, "event-trigger-source", "click(1)"), "invalid") << "the functions take no arguments";
  EXPECT_EQ(parse(r, "event-trigger-source", "keypress(a)"), "invalid") << "the argument is a <string>";
  EXPECT_EQ(parse(r, "event-trigger-source", "hover()"), "invalid");
  EXPECT_EQ(parse(r, "event-trigger", "none"), "valid:none");
  EXPECT_EQ(parse(r, "event-trigger", "--a click()"), "valid:--a click()");
  EXPECT_EQ(parse(r, "event-trigger", "--a click(), --b keypress(\"x\") / input()"), "valid:--a click(), --b keypress(\"x\") / input()");
}

// ---- quirks -----------------------------------------------------------------------------------------------

TEST(CssProperty, QuirksModeAcceptsAUnitlessLengthInTheListedPropertiesOnly) {
  const GTEXT_CSS_Registry * r = R();
  GTEXT_CSS_Property_Options q;
  std::memset(&q, 0, sizeof q);
  q.quirks = true;
  EXPECT_EQ(parse(r, "width", "10", &q), "valid:10px");
  EXPECT_EQ(parse(r, "width", "10"), "invalid");
  EXPECT_EQ(parse(r, "margin", "10 20", &q), "valid:10px 20px");
  EXPECT_EQ(parse(r, "margin-top", "-5", &q), "valid:-5px");
  EXPECT_EQ(parse(r, "font-size", "12", &q), "valid:12px");
  EXPECT_EQ(parse(r, "background-position", "10 20", &q), "valid:10px 20px");
  EXPECT_EQ(parse(r, "border-width", "2", &q), "valid:2px");
  EXPECT_EQ(parse(r, "vertical-align", "3", &q), "valid:3px");
  // not in the list, and not in the properties that include or reference them
  EXPECT_EQ(parse(r, "background", "10", &q), "invalid");
  EXPECT_EQ(parse(r, "border-radius", "10", &q), "invalid");
  EXPECT_EQ(parse(r, "gap", "10", &q), "invalid");
  EXPECT_EQ(parse(r, "margin-inline", "10", &q), "invalid");
  // a typed unit is unaffected
  EXPECT_EQ(parse(r, "width", "10em", &q), "valid:10em");
  EXPECT_EQ(parse(r, "width", "10foo", &q), "invalid");
  // inside calc() it is not a length
  EXPECT_EQ(parse(r, "width", "calc(10 + 5px)", &q), "invalid");
  // the flag in the registry is the list's
  EXPECT_NE(info(r, "width").flags & GTEXT_CSS_PROP_QUIRK_UNITLESS, 0u);
}

// ---- descriptors --------------------------------------------------------------------------------------------

TEST(CssProperty, ADescriptorIsParsedInTheContextOfItsAtRule) {
  const GTEXT_CSS_Registry * r = R();
  const GTEXT_CSS_Property_Options face = descriptor_of("@font-face");
  EXPECT_EQ(parse(r, "font-display", "swap", &face), "valid:swap");
  EXPECT_EQ(parse(r, "FONT-DISPLAY", "SWAP", &face), "valid:swap");
  EXPECT_EQ(parse(r, "font-display", "bogus", &face), "invalid");
  EXPECT_EQ(parse(r, "font-weight", "400 700", &face), "valid:400 700");
  EXPECT_EQ(parse(r, "font-weight", "bold", &face), "valid:bold");
  EXPECT_EQ(parse(r, "font-weight", "bolder", &face), "invalid") << "a descriptor takes the absolute weights, not the relative ones";
  EXPECT_EQ(parse(r, "size-adjust", "110%", &face), "valid:110%");
  EXPECT_EQ(parse(r, "size-adjust", "-1%", &face), "invalid");
  const GTEXT_CSS_Property_Options face2 = descriptor_of("@FONT-FACE");
  EXPECT_EQ(parse(r, "font-display", "swap", &face2), "valid:swap");
  const GTEXT_CSS_Property_Options bare = descriptor_of("font-face");
  EXPECT_EQ(parse(r, "font-display", "swap", &bare), "valid:swap") << "the at sign may be left off";
  // the same name is another thing in another context
  EXPECT_EQ(parse(r, "font-display", "swap"), "unknown") << "not a property";
  EXPECT_EQ(parse(r, "width", "10px", &face), "unknown") << "not a descriptor of @font-face";
  const GTEXT_CSS_Property_Options none = descriptor_of("@nosuch");
  EXPECT_EQ(parse(r, "font-display", "swap", &none), "unknown");
  const GTEXT_CSS_Property_Options page = descriptor_of("@page");
  EXPECT_EQ(parse(r, "marks", "crop cross", &page), "valid:crop cross");
  EXPECT_EQ(parse(r, "bleed", "3mm", &page), "valid:3mm");
  // `size` names <page-size>, which the pin nests inside the descriptor's own entry (read since the nested types are)
  EXPECT_EQ(parse(r, "size", "auto", &page), "valid:auto");
  EXPECT_EQ(parse(r, "size", "A4 landscape", &page), "valid:a4 landscape");
  EXPECT_EQ(parse(r, "size", "10cm 5cm", &page), "valid:10cm 5cm");
  EXPECT_EQ(parse(r, "size", "A4 A5", &page), "invalid");
  const GTEXT_CSS_Property_Options counter = descriptor_of("@counter-style");
  EXPECT_EQ(parse(r, "system", "cyclic", &counter), "valid:cyclic");
  EXPECT_EQ(parse(r, "negative", "'-' '+'", &counter), "valid:\"-\" \"+\"");
  const GTEXT_CSS_Property_Options prop = descriptor_of("@property");
  EXPECT_EQ(parse(r, "inherits", "true", &prop), "valid:true");
  EXPECT_EQ(parse(r, "syntax", "'<length>'", &prop), "valid:\"<length>\"");
  const GTEXT_CSS_Property_Options media = descriptor_of("@media");
  EXPECT_EQ(parse(r, "orientation", "portrait", &media), "valid:portrait");
  EXPECT_EQ(parse(r, "width", "600px", &media), "valid:600px") << "a media feature is a descriptor of @media";
}

TEST(CssProperty, ADescriptorTakesNoCssWideKeywordAndNoSubstitution) {
  const GTEXT_CSS_Registry * r = R();
  const GTEXT_CSS_Property_Options face = descriptor_of("@font-face");
  EXPECT_EQ(parse(r, "font-display", "inherit", &face), "invalid");
  EXPECT_EQ(parse(r, "font-display", "initial", &face), "invalid");
  EXPECT_EQ(parse(r, "font-display", "var(--x)", &face), "invalid") << "a descriptor is not a property of an element";
  EXPECT_EQ(parse(r, "size-adjust", "calc(var(--x) * 1%)", &face), "invalid");
  // a nested at-rule is not a declaration
  const GTEXT_CSS_Property_Options values = descriptor_of("@font-feature-values");
  EXPECT_EQ(parse(r, "@swash", "x", &values), "unknown");
  EXPECT_EQ(parse(r, "font-display", "swap", &values), "valid:swap");
}

TEST(CssProperty, TheFontSrcDescriptorHasTheGrammarTheServedTextGives) {
  const GTEXT_CSS_Registry * r = R();
  const GTEXT_CSS_Property_Options face = descriptor_of("@font-face");
  // <font-src-list> is prose in the pin; the supplement transcribes it from the draft (url with format() and tech(), or local())
  EXPECT_EQ(parse(r, "src", "url(a.woff2)", &face), "valid:url(\"a.woff2\")");
  EXPECT_EQ(parse(r, "src", "local(A), url(a) format(woff2) tech(variations)", &face), "valid:local(A), url(\"a\") format(woff2) tech(variations)");
  EXPECT_EQ(parse(r, "src", "format(woff2)", &face), "invalid");
}

TEST(CssProperty, ADeclarationRoutesTheUnicodeRangeTokens) {
  const GTEXT_CSS_Registry * r = R();
  GTEXT_CSS_Tree * tree = nullptr;
  const std::string css = "unicode-range: U+0-7F, U+4??";
  ASSERT_EQ(gtext_css_parse_declaration_list(css.data(), css.size(), nullptr, &tree, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(tree->declaration_count, 1u);
  const GTEXT_CSS_Property_Options face = descriptor_of("@font-face");
  GTEXT_CSS_Property_Value pv;
  ASSERT_EQ(gtext_css_parse_property_declaration(r, &tree->declarations[0], &face, &pv), GTEXT_CSS_OK);
  EXPECT_EQ(pv.outcome, GTEXT_CSS_PROP_RESULT_VALID);
  char * buf = nullptr;
  size_t n = 0;
  ASSERT_EQ(gtext_css_property_value_serialize_buffer(&pv, nullptr, &buf, &n), GTEXT_CSS_OK);
  EXPECT_EQ(std::string(buf, n), "U+0-7F, U+400-4FF") << "the range is written as the standard writes it";
  gtext_css_serialize_buffer_free(nullptr, buf);
  gtext_css_property_value_free(&pv);
  gtext_css_tree_free(tree);
}

// ---- the call --------------------------------------------------------------------------------------------

TEST(CssProperty, ArgumentsAreChecked) {
  const GTEXT_CSS_Registry * r = R();
  GTEXT_CSS_Property_Value pv;
  std::memset(&pv, 0xAB, sizeof pv);
  EXPECT_EQ(gtext_css_parse_property(nullptr, "width", 5, nullptr, 0, nullptr, &pv), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(pv.set, nullptr) << "the result is zeroed before anything else";
  EXPECT_EQ(gtext_css_parse_property(r, nullptr, 5, nullptr, 0, nullptr, &pv), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_parse_property(r, "width", 5, nullptr, 3, nullptr, &pv), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_parse_property(r, "width", 5, nullptr, 0, nullptr, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_parse_property_text(r, "width", 5, nullptr, 4, nullptr, &pv), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_parse_property_declaration(r, nullptr, nullptr, &pv), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_parse_property_text(r, "width", 5, nullptr, 0, nullptr, &pv), GTEXT_CSS_OK) << "no text is no component values";
  EXPECT_EQ(pv.outcome, GTEXT_CSS_PROP_RESULT_INVALID);
  gtext_css_property_value_free(nullptr);
  gtext_css_property_value_free(&pv);
}

TEST(CssProperty, TheMatchLimitsAreTheCallersAndAnUnknownAnswerIsNotNoMatch) {
  const GTEXT_CSS_Registry * r = R();
  GTEXT_CSS_Match_Options m = gtext_css_match_options_default();
  m.max_match_work = 10;
  GTEXT_CSS_Property_Options o;
  std::memset(&o, 0, sizeof o);
  o.match = &m;
  GTEXT_CSS_Property_Value pv;
  const std::string v = "1px 2px 3px 4px";
  EXPECT_EQ(gtext_css_parse_property_text(r, "margin", 6, v.data(), v.size(), &o, &pv), GTEXT_CSS_E_LIMIT_WORK);
  EXPECT_EQ(pv.set, nullptr);
  m.max_match_work = 0;  // the default
  EXPECT_EQ(gtext_css_parse_property_text(r, "margin", 6, v.data(), v.size(), &o, &pv), GTEXT_CSS_OK);
  EXPECT_EQ(pv.outcome, GTEXT_CSS_PROP_RESULT_VALID);
  gtext_css_property_value_free(&pv);
}

// ---- serialization ---------------------------------------------------------------------------------------

TEST(CssProperty, ASerializedValueParsesAndSerializesAgain) {
  const GTEXT_CSS_Registry * r = R();
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"margin", "1px auto 2% calc(1px + 2%)"}, {"color", "rgb(1 2 3 / 50%)"}, {"font-family", "'A B', serif"},
      {"transform", "translate(1px, 2px) rotate(.5turn)"}, {"background-image", "linear-gradient(to right, red, blue 50%)"},
      {"grid-template-areas", "'a b' 'c d'"}, {"transition", "all 0.5s ease-in-out 1s"}, {"counter-reset", "a 1 b 2"},
      {"width", "min(10px, 5%)"}, {"aspect-ratio", "16 / 9"}, {"font", "bold 1em/1 'x', y"}, {"border-radius", "1px 2px / 3px 4px"},
      {"quotes", "'«' '»'"}, {"clip-path", "circle(closest-side at 10px 10px)"},
      {"filter", "blur(2px) drop-shadow(1px 2px red)"}, {"text-shadow", "1px 1px red, 2px 2px blue"}, {"--c", "a  b   c"}};
  for (const auto & c : cases) {
    const std::string first = parse(r, c.first, c.second);
    ASSERT_EQ(first.rfind("valid:", 0), 0u) << c.first << ": " << c.second << " -> " << first;
    const std::string second = parse(r, c.first, first.substr(6));
    EXPECT_EQ(second, first) << c.first;
  }
}

TEST(CssProperty, SerializingAResultThatHasNoValueIsRefused) {
  const GTEXT_CSS_Registry * r = R();
  GTEXT_CSS_Property_Value pv;
  ASSERT_EQ(gtext_css_parse_property_text(r, "width", 5, "-1px", 4, nullptr, &pv), GTEXT_CSS_OK);
  ASSERT_EQ(pv.outcome, GTEXT_CSS_PROP_RESULT_INVALID);
  char * buf = nullptr;
  size_t n = 0;
  EXPECT_EQ(gtext_css_property_value_serialize_buffer(&pv, nullptr, &buf, &n), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_property_value_serialize_buffer(nullptr, nullptr, &buf, &n), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_property_value_serialize(&pv, nullptr, nullptr), GTEXT_CSS_E_INVALID);
  gtext_css_property_value_free(&pv);
  ASSERT_EQ(gtext_css_parse_property_text(r, "nosuch", 6, "1", 1, nullptr, &pv), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_property_value_serialize_buffer(&pv, nullptr, &buf, &n), GTEXT_CSS_E_INVALID);
  gtext_css_property_value_free(&pv);
  // an empty custom property serializes to nothing, and is a buffer all the same
  ASSERT_EQ(gtext_css_parse_property_text(r, "--e", 3, "", 0, nullptr, &pv), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_property_value_serialize_buffer(&pv, nullptr, &buf, &n), GTEXT_CSS_OK);
  EXPECT_EQ(n, 0u);
  EXPECT_EQ(buf[0], '\0');
  gtext_css_serialize_buffer_free(nullptr, buf);
  gtext_css_property_value_free(&pv);
}

// ---- the validity callbacks ----------------------------------------------------------------------------------

std::string declarations_kept(const GTEXT_CSS_Registry * r, const std::string & css, GTEXT_CSS_Property_Validity * state,
    bool block = false) {
  GTEXT_CSS_Property_Options o;
  std::memset(&o, 0, sizeof o);
  EXPECT_EQ(gtext_css_property_validity_init(state, r, &o), GTEXT_CSS_OK);
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  po.validity = gtext_css_property_validity(state);
  GTEXT_CSS_Tree * tree = nullptr;
  const GTEXT_CSS_Status st = block ? gtext_css_parse_stylesheet(css.data(), css.size(), &po, &tree, nullptr)
                                    : gtext_css_parse_declaration_list(css.data(), css.size(), &po, &tree, nullptr);
  EXPECT_EQ(st, GTEXT_CSS_OK);
  const std::string out = css_test::serialize(tree);
  gtext_css_tree_free(tree);
  return out;
}

TEST(CssPropertyValidity, ADeclarationThatNoPropertyAcceptsIsDroppedAsTheStandardSays) {
  GTEXT_CSS_Property_Validity v;
  const std::string kept = declarations_kept(R(), "color: red; nosuch: 1; width: -1px; margin: 1px 2px; height: 5px ", &v);
  EXPECT_EQ(kept, "color:red;margin:1px 2px;height:5px;");
  EXPECT_EQ(v.status, GTEXT_CSS_OK);
  EXPECT_EQ(v.checked, 5u);
  EXPECT_EQ(v.rejected, 2u);
  EXPECT_EQ(v.undecided, 0u);
}

TEST(CssPropertyValidity, WideKeywordsPendingValuesAndCustomPropertiesAreValid) {
  GTEXT_CSS_Property_Validity v;
  const std::string kept = declarations_kept(R(), "width: inherit; color: var(--c); --x: 1 2 3; all: revert; all: 1px; -webkit-transform: none", &v);
  EXPECT_EQ(kept, "width:inherit;color:var(--c);--x:1 2 3;all:revert;-webkit-transform:none;");
  EXPECT_EQ(v.rejected, 1u);
}

TEST(CssPropertyValidity, AnUndecidablePropertyIsAcceptedAndCounted) {
  GTEXT_CSS_Property_Validity v;
  const std::string kept = declarations_kept(R(), "-webkit-box-align: center; voice-family: male; color: nope", &v);
  EXPECT_EQ(kept, "-webkit-box-align:center;voice-family:male;");
  EXPECT_EQ(v.undecided, 2u);
  EXPECT_EQ(v.rejected, 1u);
  EXPECT_EQ(v.status, GTEXT_CSS_OK);
}

TEST(CssPropertyValidity, InsideAnAtRuleThatHasDescriptorsTheNamesAreItsDescriptors) {
  GTEXT_CSS_Property_Validity v;
  const std::string kept = declarations_kept(R(),
      "@font-face { font-display: swap; width: 1px; font-weight: 400 700; font-display: bogus }\n"
      "a { color: red; font-display: swap }\n"
      "@media screen { a { color: blue; font-display: swap } }",
      &v, true);
  EXPECT_EQ(kept, "@font-face {font-display:swap;font-weight:400 700;}a {color:red;}@media screen {a {color:blue;}}");
  EXPECT_GE(v.rejected, 4u);
}

TEST(CssPropertyValidity, ADeclarationInAStyleRuleIsAPropertyAndOneThatIsNotValidIsReadAgainAsARule) {
  GTEXT_CSS_Property_Validity v;
  const std::string kept = declarations_kept(R(), "a { width: -1px; height: 5px; &:hover { color: red } nosuch: 1 }", &v, true);
  EXPECT_EQ(kept, "a {height:5px;&:hover {color:red;}}");
}

TEST(CssPropertyValidity, ANestedAtRuleInAStyleRuleHoldsPropertiesNotItsOwnDescriptors) {
  // `@media` has descriptors (the media features), but a declaration inside a rule nested in a style rule is a property of the
  // element: `height` is no media feature, and is kept because it is a property.
  GTEXT_CSS_Property_Validity v;
  const std::string kept = declarations_kept(R(),
      "a { @media screen { height: 5px; color: red; nosuch: 1; width: -1px } }", &v, true);
  EXPECT_EQ(kept, "a {@media screen {height:5px;color:red;}}") << kept;
}


TEST(CssPropertyValidity, ALimitInTheMatchIsRecordedAndTheDeclarationIsNotKept) {
  GTEXT_CSS_Match_Options m = gtext_css_match_options_default();
  m.max_match_work = 10;
  GTEXT_CSS_Property_Options o;
  std::memset(&o, 0, sizeof o);
  o.match = &m;
  GTEXT_CSS_Property_Validity v;
  ASSERT_EQ(gtext_css_property_validity_init(&v, R(), &o), GTEXT_CSS_OK);
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  po.validity = gtext_css_property_validity(&v);
  GTEXT_CSS_Tree * tree = nullptr;
  const std::string css = "margin: 1px 2px 3px 4px; color: red";
  ASSERT_EQ(gtext_css_parse_declaration_list(css.data(), css.size(), &po, &tree, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(v.status, GTEXT_CSS_E_LIMIT_WORK) << "the caller can tell that an answer was not known";
  EXPECT_EQ(css_test::serialize(tree), "color:red;");
  gtext_css_tree_free(tree);
  EXPECT_EQ(gtext_css_property_validity_init(nullptr, R(), nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_property_validity_init(&v, nullptr, nullptr), GTEXT_CSS_E_INVALID);
}


// ---- the fix pass of the review ----------------------------------------------------------------------------

TEST(CssPropertyValidity, AnAtRuleThatTakesPropertiesAndDescriptorsKeepsBoth) {
  // CSS Page 3: "@page rules ... contain declarations: the page-margin boxes' properties and the page descriptors"; a declaration of a
  // property that is not a descriptor of the rule is still a property of it, and is judged as one.
  GTEXT_CSS_Property_Validity v;
  const std::string kept = declarations_kept(R(),
      "@page { margin: 1px 2px; marks: crop; bleed: 3mm; width: -1px; nosuch: 1; color: red }\n"
      "@page :first { margin-top: 5px; size: auto }\n"
      "@function --f() { result: 1px; width: 2px; width: bogus }",
      &v, true);
  EXPECT_NE(kept.find("margin:1px 2px;"), std::string::npos) << kept;
  EXPECT_NE(kept.find("marks:crop;"), std::string::npos) << kept;
  EXPECT_NE(kept.find("bleed:3mm;"), std::string::npos) << kept;
  EXPECT_NE(kept.find("color:red;"), std::string::npos) << kept;
  EXPECT_EQ(kept.find("width:-1px"), std::string::npos) << kept;
  EXPECT_EQ(kept.find("nosuch"), std::string::npos) << kept;
  EXPECT_NE(kept.find("margin-top:5px;"), std::string::npos) << kept;
  EXPECT_NE(kept.find("width:2px;"), std::string::npos) << kept;
  EXPECT_EQ(kept.find("bogus"), std::string::npos) << kept;
  GTEXT_CSS_At_Rule_Info a, b;
  ASSERT_TRUE(gtext_css_registry_at_rule_info(gtext_css_registry_find_at_rule("@page", 5), &a));
  ASSERT_TRUE(gtext_css_registry_at_rule_info(gtext_css_registry_find_at_rule("@font-face", 10), &b));
  const GTEXT_CSS_At_Rule_Info * page = &a;
  const GTEXT_CSS_At_Rule_Info * font = &b;
  EXPECT_TRUE(page->properties);
  EXPECT_FALSE(font->properties) << "a rule that takes only descriptors does not fall through";
}

TEST(CssProperty, FontStretchIsTheLegacyNameOfTheFontWidthDescriptor) {
  // Fonts 4, @font-face: "font-stretch ... the legacy alias of font-width: ... `font-width` ... and the legacy name `font-stretch`
  // are both accepted as descriptors, with the same meaning"
  const GTEXT_CSS_Registry * r = R();
  const GTEXT_CSS_Property_Options face = descriptor_of("@font-face");
  EXPECT_EQ(parse(r, "font-stretch", "50% 150%", &face), "valid:50% 150%");
  EXPECT_EQ(parse(r, "font-width", "50% 150%", &face), "valid:50% 150%");
  EXPECT_EQ(parse(r, "font-stretch", "condensed", &face), "valid:condensed");
  EXPECT_EQ(parse(r, "font-stretch", "-5%", &face), "invalid");
  GTEXT_CSS_Property_Validity v;
  EXPECT_NE(declarations_kept(r, "@font-face { font-stretch: 75%; font-width: 80% }", &v, true).find("font-stretch:75%;"), std::string::npos);
}

TEST(CssProperty, TheTextPathMatchesTheUnicodeRangeDescriptor) {
  const GTEXT_CSS_Registry * r = R();
  const GTEXT_CSS_Property_Options face = descriptor_of("@font-face");
  EXPECT_EQ(parse(r, "unicode-range", "U+0-7F, U+4??", &face), "valid:U+0-7F, U+400-4FF");
  EXPECT_EQ(parse(r, "UNICODE-RANGE", "u+26", &face), "valid:U+26");
  EXPECT_EQ(parse(r, "unicode-range", "U+110000", &face), "invalid");
  EXPECT_EQ(parse(r, "unicode-range", "U+30-20", &face), "invalid") << "the start is after the end";
  EXPECT_EQ(parse(r, "unicode-range", "U+10FFFF", &face), "valid:U+10FFFF");
  EXPECT_EQ(parse(r, "unicode-range", "red", &face), "invalid");
}

TEST(CssProperty, EveryEntryPointLeavesUnknownOnAFailedCall) {
  const GTEXT_CSS_Registry * r = R();
  GTEXT_CSS_Property_Value pv;
  std::memset(&pv, 0, sizeof pv);
  pv.outcome = GTEXT_CSS_PROP_RESULT_INVALID;
  EXPECT_EQ(gtext_css_parse_property_declaration(r, nullptr, nullptr, &pv), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(pv.outcome, GTEXT_CSS_PROP_RESULT_UNKNOWN);
  pv.outcome = GTEXT_CSS_PROP_RESULT_PENDING;
  EXPECT_EQ(gtext_css_parse_property_declaration(nullptr, nullptr, nullptr, &pv), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(pv.outcome, GTEXT_CSS_PROP_RESULT_UNKNOWN);
  pv.outcome = GTEXT_CSS_PROP_RESULT_WIDE;
  EXPECT_EQ(gtext_css_parse_property_text(nullptr, "width", 5, "1px", 3, nullptr, &pv), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(pv.outcome, GTEXT_CSS_PROP_RESULT_UNKNOWN);
  pv.outcome = GTEXT_CSS_PROP_RESULT_WIDE;
  EXPECT_EQ(gtext_css_parse_property_text(r, nullptr, 5, "1px", 3, nullptr, &pv), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(pv.outcome, GTEXT_CSS_PROP_RESULT_UNKNOWN);
  pv.outcome = GTEXT_CSS_PROP_RESULT_WIDE;
  EXPECT_EQ(gtext_css_parse_property_text(r, "width", 5, nullptr, 3, nullptr, &pv), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(pv.outcome, GTEXT_CSS_PROP_RESULT_UNKNOWN);
  pv.outcome = GTEXT_CSS_PROP_RESULT_WIDE;
  GTEXT_CSS_Property_Options face = descriptor_of("@font-face");
  face.match = nullptr;
  EXPECT_EQ(gtext_css_parse_property_text(r, "unicode-range", 13, nullptr, 3, &face, &pv), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(pv.outcome, GTEXT_CSS_PROP_RESULT_UNKNOWN);
  EXPECT_EQ(gtext_css_parse_property_declaration(r, nullptr, nullptr, nullptr), GTEXT_CSS_E_INVALID);
}

TEST(CssProperty, ASubstitutionFunctionThatIsNotSyntacticallyValidMakesTheValueInvalid) {
  // Values 5: "If a property value contains one or more arbitrary substitution functions, and all of those functions are themselves
  // syntactically valid according to their argument grammars, the entire value's grammar must be assumed to be valid at parse time."
  // `var()` has no first argument, which `var( <declaration-value> , <declaration-value>? )` requires.
  const GTEXT_CSS_Registry * r = R();
  EXPECT_EQ(parse(r, "width", "var()"), "invalid");
  EXPECT_EQ(parse(r, "width", "var( )"), "invalid");
  EXPECT_EQ(parse(r, "width", "var(, 1px)"), "invalid");
  EXPECT_EQ(parse(r, "width", "env()"), "invalid");
  EXPECT_EQ(parse(r, "width", "attr()"), "invalid");
  EXPECT_EQ(parse(r, "width", "calc(1px + var())"), "invalid");
  EXPECT_EQ(parse(r, "margin", "1px var(--a) var()"), "invalid");
  EXPECT_EQ(parse(r, "width", "var(--a)"), "pending:var(--a)");
  EXPECT_EQ(parse(r, "width", "var(--a, 1px)"), "pending:var(--a, 1px)");
  EXPECT_EQ(parse(r, "width", "var(--a,)"), "pending:var(--a,)");
  EXPECT_EQ(parse(r, "width", "env(safe-area-inset-top)"), "pending:env(safe-area-inset-top)");
  // Environment Variables 1: "The env() function's argument grammar is: <env-args> = env( <declaration-value>, <declaration-value>? )", and the function's own grammar, "env( <custom-ident>
  // <integer [0,∞]>*, <declaration-value>? )", is the one a substitution reads: "If the name provided by the first argument of the env() function is a recognized environment variable name,
  // [...] replace the env() function by the value of the named environment variable. Otherwise, if the env() function has a fallback value as its second argument, replace the env()
  // function by the fallback value", so a first argument that names nothing is valid when the declaration is read and takes the fallback when it is substituted (the pages css-env/env-parsing.html,
  // indexed-env.tentative.html and syntax.tentative.html of the pin hold the library to it). Chromium 156.0.8078.47 and Firefox 158.0.1 both still drop each of these declarations at parse
  // time, as they drop `var(--x {--y})`.
  EXPECT_EQ(parse(r, "top", "env(test 0, green)"), "pending:env(test 0, green)");
  EXPECT_EQ(parse(r, "top", "env(test 0,)"), "pending:env(test 0,)");
  EXPECT_EQ(parse(r, "top", "env(test 0 1 2 3 4, green)"), "pending:env(test 0 1 2 3 4, green)");
  for (const char * unnamed : {"env(test1 test2, green)", "env(test1 10 20 test2, green)", "env(test 0.1, green)", "env(test -1, green)", "env(1px)", "env(test 1px)", "env(var(--n) 0, 1px)", "env(env(test))",
                               "env(attr(data-x type(*)), blue)", "env(test var(--n))", "env(safe-area-inset-top ())", "env(safe-area-inset-top() )", "env(safe-area-inset-top (),)", "env(10px)"}) {
    EXPECT_EQ(parse(r, "top", unnamed), std::string("pending:") + unnamed) << unnamed << ": valid at parse time, and a fallback or invalid at computed-value time";
  }
  EXPECT_EQ(parse(r, "top", "env(test, env(env(x)))"), "pending:env(test, env(env(x)))");
  EXPECT_EQ(parse(r, "top", "env(test, var(--n))"), "pending:env(test, var(--n))");
  // "A free-form production that does not start with "{" matches neither top-level commas nor {} blocks, so a {} block may not be mixed with other values in the name argument", and
  // "<declaration-value> is one or more tokens, so an empty name argument is invalid whether or not it is {}-wrapped" (css-env/env-parsing.html): for the four functions alike
  for (const char * fn : {"env(%s)", "var(%s)", "attr(%s)", "inherit(%s)"}) {
    char text[96];
    for (const char * bad : {"--x {--y}", "{--x} --y", "--x {--y}, 10px", "", "{}", "{}, 10px", ", 10px", "{ }"}) {
      std::snprintf(text, sizeof text, fn, bad);
      EXPECT_EQ(parse(r, "top", text), "invalid") << text;
    }
    for (const char * good : {"{--x}", "{ --x }", "{--x}, 10px", "--x,"}) {
      std::snprintf(text, sizeof text, fn, good);
      EXPECT_EQ(parse(r, "top", text).substr(0, 8), "pending:") << text;
    }
  }
  EXPECT_EQ(parse(r, "top", "env(test, {})").substr(0, 8), "pending:") << "an empty block is an empty fallback";
  EXPECT_EQ(parse(r, "top", "env(test, {)"), "invalid");
  // Cascade 4, on `all`: "It only accepts the CSS-wide keywords." A substitution may yield one, so `all: var(--x)` is pending (a browser takes it) and is
  // invalid at computed-value time unless it does.
  EXPECT_EQ(parse(r, "all", "var(--x)"), "pending:var(--x)");
  EXPECT_EQ(parse(r, "all", "revert"), "wide:revert");
  // a custom property's value is held to the same rule: a var() that is not valid is not a <declaration-value> that can be
  // substituted, so the declaration is invalid
  EXPECT_EQ(parse(r, "--x", "var()"), "invalid");
  EXPECT_EQ(parse(r, "--x", "var(--y)"), "pending:var(--y)");
}

TEST(CssProperty, AFallbackThatStartsWithABraceIsTheBlockAlone) {
  // Values 5, "Commas in Function Arguments": a free-form production that starts with a "{" token "matches just the {} block that the "{" token opens"; the fallback of var() is a "non-strict
  // free-form production" and "just the contents of the {} block" when it starts with one. So the fallback of var(), env(), attr() and inherit() is the block with nothing but white space beside it:
  // a token after it, or a comma and more, is not matched by the function's grammar and the declaration is invalid at parse time.
// Neither Chromium 156.0.8078.47 nor Firefox 158.0.1 implements it: both keep every one of these declarations (a token after the block included) and compute the property as if the fallback were
// not there. The text decides (the pages css-variables/var-fallback-braces.html, css-env/env-fallback-braces.html and css-values/attr-fallback-braces.html agree with it).
  const GTEXT_CSS_Registry * r = R();
  for (const char * fn : {"var(--a, %s)", "env(a, %s)", "attr(data-a, %s)", "inherit(--a, %s)"}) {
    char text[96];
    auto with = [&](const char * fallback) {
      std::snprintf(text, sizeof text, fn, fallback);
      return std::string(text);
    };
    EXPECT_EQ(parse(r, "color", with("{rgb(1, 2, 3)}").c_str()).substr(0, 8), "pending:") << with("{rgb(1, 2, 3)}");
    EXPECT_EQ(parse(r, "color", with("{ rgb(1, 2, 3) }").c_str()).substr(0, 8), "pending:") << "white space in and around the block";
    EXPECT_EQ(parse(r, "color", with("{rgb(1, 2, 3)} ").c_str()).substr(0, 8), "pending:");
    EXPECT_EQ(parse(r, "color", with("{}").c_str()).substr(0, 8), "pending:") << "an empty block is an empty fallback";
    EXPECT_EQ(parse(r, "color", with("{rgb(1, 2, 3)} rgb(4, 5, 6)").c_str()), "invalid") << "a token after the block";
    EXPECT_EQ(parse(r, "color", with("{rgb(1, 2, 3)}, red").c_str()), "invalid") << "another argument after the block";
    EXPECT_EQ(parse(r, "color", with("{rgb(1, 2, 3)}{}").c_str()), "invalid") << "a second block";
    EXPECT_EQ(parse(r, "font-family", with("{Arial}, sans-serif").c_str()), "invalid");
    EXPECT_EQ(parse(r, "color", with("red {rgb(1, 2, 3)}").c_str()).substr(0, 8), "pending:") << "a block that is not the start of the fallback is a token like any other (non-strict)";
    EXPECT_EQ(parse(r, "color", with("red {rgb(1, 2, 3)} blue").c_str()).substr(0, 8), "pending:") << "and tokens may follow it";
  }
  EXPECT_EQ(parse(r, "font-family", "var(--a, Arial, sans-serif)").substr(0, 8), "pending:") << "non-strict: a fallback that does not start with a brace may hold commas";
  EXPECT_EQ(parse(r, "font-family", "var(--a, {Arial, sans-serif})").substr(0, 8), "pending:");
  EXPECT_EQ(parse(r, "--x", "var(--a, {x} y)"), "invalid") << "a custom property's value is held to the same rule";
}

TEST(CssProperty, AUnitlessNumberIsALengthOnlyWhereTheQuirkAppliesAndNotInsideAnotherFunction) {
  // Values 4, Appendix C: "the unitless length quirk ... applies ... only to the value of the properties listed, in the numeric
  // position of the property's own value, and not inside functions": a number inside fit-content() is a number.
  const GTEXT_CSS_Registry * r = R();
  GTEXT_CSS_Property_Options q;
  std::memset(&q, 0, sizeof q);
  q.quirks = true;
  EXPECT_EQ(parse(r, "width", "fit-content(10)", &q), "invalid");
  EXPECT_EQ(parse(r, "width", "fit-content(10px)", &q), "valid:fit-content(10px)");
  EXPECT_EQ(parse(r, "width", "calc(10)", &q), "invalid");
  EXPECT_EQ(parse(r, "width", "10", &q), "valid:10px");
  EXPECT_EQ(parse(r, "margin", "10 fit-content(5)", &q), "invalid");
  EXPECT_EQ(parse(r, "clip", "rect(1px, 2px, 3px, 4px)", &q).substr(0, 6), "valid:") << "clip is decided, and its lengths carry units";
}

// ---- the definitions the pin lacks (tools/css/supplement-types.tsv, consolidate.tsv) -------------------------------------

TEST(CssPropertySupplements, TheNamedTimelineRangesOfScrollDrivenAnimationsAreDecided) {
  const GTEXT_CSS_Registry * r = R();
  // <timeline-range-name> is prose in the pin; the draft lists the six names. Fourteen properties name it.
  EXPECT_EQ(parse(r, "animation-range-start", "entry 42%"), "valid:entry 42%");
  EXPECT_EQ(parse(r, "animation-range-start", "exit-crossing"), "valid:exit-crossing");
  EXPECT_EQ(parse(r, "animation-range-start", "normal"), "valid:normal");
  EXPECT_EQ(parse(r, "animation-range-start", "peek 50%"), "invalid") << "not a named range";
  EXPECT_EQ(parse(r, "animation-range-start", "50% cover"), "invalid") << "the name comes first";
  EXPECT_EQ(parse(r, "animation-range-start", "cover contain"), "invalid");
  EXPECT_EQ(parse(r, "timeline-trigger-range", "entry 10%").substr(0, 6), "valid:");
  EXPECT_EQ(parse(r, "animation-range", "entry 0% exit 100%").substr(0, 6), "valid:");
}

TEST(CssPropertySupplements, ACursorIsAnImageSetOfUrlsWithAHotspotAndAKeyword) {
  const GTEXT_CSS_Registry * r = R();
  EXPECT_EQ(parse(r, "cursor", "pointer"), "valid:pointer");
  EXPECT_EQ(parse(r, "cursor", "url(a.png) 1 2, pointer").substr(0, 6), "valid:");
  EXPECT_EQ(parse(r, "cursor", "image-set(\"a.png\" 1x, \"b.png\" 2x) 5 6, grab").substr(0, 6), "valid:");
  EXPECT_EQ(parse(r, "cursor", "url(a.png)"), "invalid") << "the list ends in a keyword";
  EXPECT_EQ(parse(r, "cursor", "linear-gradient(red), pointer"), "invalid") << "<url-set> is image-set() restricted to <url>, not <image>";
  EXPECT_EQ(parse(r, "cursor", "en-resize"), "invalid");
}

TEST(CssPropertySupplements, TheCssTwoClipRectangleTakesLengthsAndAuto) {
  const GTEXT_CSS_Registry * r = R();
  EXPECT_EQ(parse(r, "clip", "auto"), "valid:auto");
  EXPECT_EQ(parse(r, "clip", "rect(10px, 20px, -30px, 40px)").substr(0, 6), "valid:");
  EXPECT_EQ(parse(r, "clip", "rect(10px, -20px, auto, auto)").substr(0, 6), "valid:");
  EXPECT_EQ(parse(r, "clip", "rect(10px, 20px, 30px)"), "invalid");
  EXPECT_EQ(parse(r, "clip", "rect(10px 20px, 30px 40px)"), "invalid") << "commas or spaces, not a mixture";
}

TEST(CssPropertySupplements, ContentTakesTheSymbolicGlyphsOfTheDraft) {
  const GTEXT_CSS_Registry * r = R();
  EXPECT_EQ(parse(r, "content", "none"), "valid:none");
  EXPECT_EQ(parse(r, "content", "disclosure-open"), "valid:disclosure-open");
  EXPECT_EQ(parse(r, "content", "\"a\" counter(c)").substr(0, 6), "valid:");
  EXPECT_EQ(parse(r, "content", "decimal"), "invalid") << "only the predefined symbolic styles are glyphs";
}

TEST(CssPropertySupplements, LightDarkOfTwoImagesIsAnImage) {
  const GTEXT_CSS_Registry * r = R();
  EXPECT_EQ(parse(r, "background-image", "light-dark(url(a.png), none)").substr(0, 6), "valid:");
  EXPECT_EQ(parse(r, "list-style-image", "light-dark(linear-gradient(red), url(b.png))").substr(0, 6), "valid:");
  EXPECT_EQ(parse(r, "background-image", "light-dark(url(a.png), red)"), "invalid") << "one image and one color";
  EXPECT_EQ(parse(r, "color", "light-dark(red, blue)").substr(0, 6), "valid:") << "the color form is as it was";
}

TEST(CssProperty, NoneIsNotACounterNameATransitionPropertyAKeyframesNameOrAViewTransitionClass) {
  const GTEXT_CSS_Registry * r = R();
  // Lists 3: "A <counter-name> name cannot match the keyword none;"
  EXPECT_EQ(parse(r, "counter-reset", "none"), "valid:none");
  EXPECT_EQ(parse(r, "counter-reset", "chapter 2"), "valid:chapter 2");
  EXPECT_EQ(parse(r, "counter-reset", "none chapter"), "invalid");
  EXPECT_EQ(parse(r, "counter-reset", "chapter none"), "invalid");
  EXPECT_EQ(parse(r, "counter-reset", "reversed(none)"), "invalid");
  EXPECT_EQ(parse(r, "counter-reset", "reversed(chapter) none"), "invalid");
  EXPECT_EQ(parse(r, "counter-increment", "none chapter"), "invalid");
  EXPECT_EQ(parse(r, "counter-set", "none chapter"), "invalid");
  // Transitions 1: "The <custom-ident> production in <single-transition-property> also excludes the keyword none"
  EXPECT_EQ(parse(r, "transition-property", "none"), "valid:none");
  EXPECT_EQ(parse(r, "transition-property", "all, one"), "valid:all, one");
  EXPECT_EQ(parse(r, "transition-property", "none, one"), "invalid");
  EXPECT_EQ(parse(r, "transition-property", "one, none"), "invalid");
  // View Transitions 2: "none is an invalid <custom-ident> for view-transition-class, even when combined with another <custom-ident>."
  EXPECT_EQ(parse(r, "view-transition-class", "foo bar"), "valid:foo bar");
  EXPECT_EQ(parse(r, "view-transition-class", "foo none"), "invalid");
  EXPECT_EQ(parse(r, "view-transition-class", "none foo"), "invalid");
  // Animations 1: "The <custom-ident> additionally excludes the none keyword. The <string> additionally excludes the empty string (but allows the string "none" ...)."
  EXPECT_EQ(parse(r, "animation-name", "none, a"), "valid:none, a");
  EXPECT_EQ(parse(r, "animation-name", "\"none\""), "valid:\"none\"");
  EXPECT_EQ(parse(r, "animation-name", "\"\""), "invalid");
}

TEST(CssProperty, SymbolsOfTheAlphabeticAndNumericSystemsNeedTwoSymbols) {
  const GTEXT_CSS_Registry * r = R();
  // Counter Styles 3: "If the system is alphabetic or numeric, there must be at least two <string>s or <image>s, or else the function is invalid."
  EXPECT_EQ(parse(r, "list-style-type", "symbols(numeric \"a\")"), "invalid");
  EXPECT_EQ(parse(r, "list-style-type", "symbols(alphabetic \"a\")"), "invalid");
  EXPECT_EQ(parse(r, "list-style-type", "symbols(numeric \"a\" \"b\")"), "valid:symbols(numeric \"a\" \"b\")");
  EXPECT_EQ(parse(r, "list-style-type", "symbols(alphabetic \"a\" \"b\")"), "valid:symbols(alphabetic \"a\" \"b\")");
  EXPECT_EQ(parse(r, "list-style-type", "symbols(cyclic \"a\")"), "valid:symbols(cyclic \"a\")");
  EXPECT_EQ(parse(r, "list-style-type", "symbols(fixed \"a\")"), "valid:symbols(fixed \"a\")");
  EXPECT_EQ(parse(r, "list-style-type", "symbols(\"a\")"), "valid:symbols(\"a\")") << "the system is symbolic when omitted";
}

TEST(CssProperty, ASubgridNameListHasOneAutoFillRepeat) {
  const GTEXT_CSS_Registry * r = R();
  // Grid 2: "On a subgridded axis, the auto-fill keyword is only valid once per <line-name-list>"
  EXPECT_EQ(parse(r, "grid-template-rows", "subgrid repeat(auto-fill, [a]) repeat(auto-fill, [b])"), "invalid");
  EXPECT_EQ(parse(r, "grid-template-columns", "subgrid repeat(auto-fill, [a]) [c] repeat(auto-fill, [b])"), "invalid");
  EXPECT_EQ(parse(r, "grid-template-rows", "subgrid [x] repeat(auto-fill, [a]) [y] repeat(2, [b])"), "valid:subgrid [x] repeat(auto-fill, [a]) [y] repeat(2, [b])");
  EXPECT_EQ(parse(r, "grid-template-rows", "subgrid repeat(2, [a]) repeat(3, [b])"), "valid:subgrid repeat(2, [a]) repeat(3, [b])");
}

TEST(CssProperty, TheMaskShorthandTakesNoMarginBox) {
  const GTEXT_CSS_Registry * r = R();
  // Masking 1, changes: "Remove margin-box as possible value from mask-origin and mask-clip."; mask-layer's <geometry-box> sets those two
  EXPECT_EQ(parse(r, "mask", "margin-box"), "invalid");
  EXPECT_EQ(parse(r, "mask", "border-box"), "valid:border-box");
  EXPECT_EQ(parse(r, "mask", "fill-box"), "valid:fill-box");
  EXPECT_EQ(parse(r, "mask", "url(a.png) padding-box content-box"), "valid:url(\"a.png\") padding-box content-box");
}

TEST(CssProperty, TheScaleFunctionsTakePercentagesAsTransforms2Says) {
  const GTEXT_CSS_Registry * r = R();
  // Transforms 2: "The scale functions defined in [css-transforms-1] now support percentages."; the pin nests the level 1 forms in transform
  EXPECT_EQ(parse(r, "transform", "scale(250%)"), "valid:scale(2.5)");
  EXPECT_EQ(parse(r, "transform", "scale(1, 200%)"), "valid:scale(1, 2)");
  EXPECT_EQ(parse(r, "transform", "scaleX(720%)"), "valid:scaleX(7.2)");
  EXPECT_EQ(parse(r, "transform", "scaleY(-85%)"), "valid:scaleY(-0.85)");
  EXPECT_EQ(parse(r, "transform", "scale(1, 2, 3)"), "invalid");
}

TEST(CssProperty, AUrlModifierByTheNameOfARequestModifierHasItsArguments) {
  const GTEXT_CSS_Registry * r = R();
  // Values 5, 4.1.1: "<cross-origin-modifier> = cross-origin(anonymous | use-credentials)", "<integrity-modifier> = integrity(<string>)", "<referrer-policy-modifier> =
  // referrer-policy(no-referrer | ... | unsafe-url)"; Values 4, 4.5.3: "A <url-modifier> is either an <ident> or a functional notation."
  EXPECT_EQ(parse(r, "background-image", "url(\"a.png\" cross-origin(anonymous))"), "valid:url(\"a.png\" cross-origin(anonymous))");
  EXPECT_EQ(parse(r, "background-image", "url(\"a.png\" cross-origin(use-credentials) integrity(\"sha384-x\") referrer-policy(origin))"),
      "valid:url(\"a.png\" cross-origin(use-credentials) integrity(\"sha384-x\") referrer-policy(origin))") << "in the order they were given";
  EXPECT_EQ(parse(r, "background-image", "url(\"a.png\" integrity(\"\"))"), "valid:url(\"a.png\" integrity(\"\"))");
  EXPECT_EQ(parse(r, "background-image", "url(\"a.png\" cross-origin(anonymous) cross-origin(anonymous))"),
      "valid:url(\"a.png\" cross-origin(anonymous) cross-origin(anonymous))") << "the grammar is <url-modifier>*";
  EXPECT_EQ(parse(r, "background-image", "url(\"a.png\" foo(bar) baz)"), "valid:url(\"a.png\" foo(bar) baz)") << "any other modifier is an ident or a function";
  EXPECT_EQ(parse(r, "background-image", "url(\"a.png\" cross-origin())"), "invalid");
  EXPECT_EQ(parse(r, "background-image", "url(\"a.png\" cross-origin(foo))"), "invalid");
  EXPECT_EQ(parse(r, "background-image", "url(\"a.png\" cross-origin(anonymous foobar))"), "invalid");
  EXPECT_EQ(parse(r, "background-image", "url(\"a.png\" integrity(sha384-foobar))"), "invalid") << "an ident is not a <string>";
  EXPECT_EQ(parse(r, "background-image", "url(\"a.png\" integrity(\"a\" \"b\"))"), "invalid");
  EXPECT_EQ(parse(r, "background-image", "url(\"a.png\" referrer-policy(no-referrer same-origin))"), "invalid");
  EXPECT_EQ(parse(r, "background-image", "url(\"a.png\" referrer-policy(bogus))"), "invalid");
}

TEST(CssProperty, APathStringIsSvgPathDataAndNotEmpty) {
  const GTEXT_CSS_Registry * r = R();
  // Shapes 1: "A path data string that does not conform to the grammar and parsing rules of SVG 1.1, or that does conform but defines an empty path, is invalid and
  // causes the entire path() to be invalid."
  EXPECT_EQ(parse(r, "clip-path", "path(\"M 0 0 L 10 10\")"), "valid:path(\"M 0 0 L 10 10\")");
  EXPECT_EQ(parse(r, "clip-path", "path(nonzero, \"M0 0h5v5z\")"), "valid:path(\"M 0 0 h 5 v 5 Z\")");
  EXPECT_EQ(parse(r, "clip-path", "path(\"M 0 0 A 1 1 0 01 5 5\")"), "valid:path(\"M 0 0 A 1 1 0 0 1 5 5\")") << "the arc flags need no separator";
  EXPECT_EQ(parse(r, "clip-path", "path(\"M 0 0 1 1\")"), "valid:path(\"M 0 0 1 1\")") << "a moveto takes further pairs as linetos";
  EXPECT_EQ(parse(r, "clip-path", "path(\"\")"), "invalid");
  EXPECT_EQ(parse(r, "clip-path", "path(\" \")"), "invalid");
  EXPECT_EQ(parse(r, "clip-path", "path(evenodd, \"\")"), "invalid");
  EXPECT_EQ(parse(r, "clip-path", "path(evenodd, 'not-a-path')"), "invalid");
  EXPECT_EQ(parse(r, "shape-outside", "path('')"), "invalid");
  EXPECT_EQ(parse(r, "offset-path", "path(\"M 20 30 A 60 70 80\")"), "invalid") << "an arc has seven arguments";
  EXPECT_EQ(parse(r, "clip-path", "path(\"L 1 2\")"), "invalid") << "the first command is a moveto";
  EXPECT_EQ(parse(r, "clip-path", "path(\"M 1,2,\")"), "invalid") << "a trailing comma";
  EXPECT_EQ(parse(r, "clip-path", "path(\"M0 0a1 1 0 21 5 5\")"), "invalid") << "an arc flag is 0 or 1";
}

TEST(CssProperty, AFamilyNameDoesNotHoldAGenericKeywordOrACssWideKeyword) {
  const GTEXT_CSS_Registry * r = R();
  // Fonts 4, 2.1.1: "Any identifier which could be misinterpreted as a pre-defined keyword in the font-family value definition, or the CSS-wide keywords, is not
  // allowed."; Cascade 5 makes revert-rule a CSS-wide keyword
  EXPECT_EQ(parse(r, "font-family", "cursive serif"), "invalid");
  EXPECT_EQ(parse(r, "font-family", "Times serif"), "invalid");
  EXPECT_EQ(parse(r, "font-family", "FirstName, LastName, revert-rule"), "invalid");
  EXPECT_EQ(parse(r, "font-family", "serif, cursive"), "valid:serif, cursive");
  EXPECT_EQ(parse(r, "font-family", "\"cursive\", Times New Roman"), "valid:\"cursive\", Times New Roman") << "quoted, a generic name is a family name";
  EXPECT_EQ(parse(r, "animation-name", "a, revert-rule"), "invalid") << "revert-rule is no <custom-ident>";
}

TEST(CssProperty, RandomItemHasAKeyAndAtLeastOneItemAndABraceIsAWholeItem) {
  const GTEXT_CSS_Registry * r = R();
  // Values 5, 10.3: "random-item( <random-key>, [<declaration-value>?]# )"; "wrapping the values in {} allows their commas to be distinguished"; the braces are
  // "only allowed for some function arguments--those defined as free-form productions"
  EXPECT_EQ(parse(r, "font-family", "random-item(auto, serif)").substr(0, 8), "pending:");
  EXPECT_EQ(parse(r, "font-family", "random-item(--x, {Times, serif}, sans-serif, {monospace})").substr(0, 8), "pending:");
  EXPECT_EQ(parse(r, "font-family", "random-item(auto, )").substr(0, 8), "pending:") << "an item may be empty";
  EXPECT_EQ(parse(r, "font-family", "random-item()"), "invalid");
  EXPECT_EQ(parse(r, "font-family", "random-item( )"), "invalid");
  EXPECT_EQ(parse(r, "font-family", "random-item(auto)"), "invalid") << "no item";
  EXPECT_EQ(parse(r, "font-family", "random-item(, serif, sans-serif)"), "invalid") << "no key";
  EXPECT_EQ(parse(r, "font-family", "random-item(auto, !)"), "invalid");
  EXPECT_EQ(parse(r, "font-family", "random-item(auto, ;)"), "invalid");
  EXPECT_EQ(parse(r, "font-family", "random-item(auto, {Times, serif} extra)"), "invalid");
  EXPECT_EQ(parse(r, "font-family", "random-item(auto, extra {Times, serif})"), "invalid");
}

TEST(CssProperty, AnchorFunctionsAreLengthsInACalculationOfThePropertiesThatTakeThem) {
  const GTEXT_CSS_Registry * r = R();
  // Anchor Positioning 1: "anchor() = anchor( <anchor-name>? && <anchor-side>, <length-percentage>? )" and "anchor-size() = anchor-size( [ <anchor-name> || <anchor-size> ]?
  // , <length-percentage>? )", each resolving to a <length>; anchor() is valid in the inset properties and anchor-size() in the sizing, inset and margin ones
  EXPECT_EQ(parse(r, "top", "calc((anchor(--foo top) + anchor(--bar bottom)) / 2)"), "valid:calc(0.5 * (anchor(--foo top) + anchor(--bar bottom)))");
  EXPECT_EQ(parse(r, "top", "anchor(--foo top, calc(0.5 * anchor(--bar bottom)))"), "valid:anchor(--foo top, calc(0.5 * anchor(--bar bottom)))");
  EXPECT_EQ(parse(r, "top", "min(100px, 10%, anchor(--foo top), anchor(--bar bottom))"), "valid:min(100px, 10%, anchor(--foo top), anchor(--bar bottom))");
  EXPECT_EQ(parse(r, "top", "calc(anchor(--foo left, 1px) + 10%)"), "valid:calc(10% + anchor(--foo left, 1px))");
  EXPECT_EQ(parse(r, "top", "calc(anchor(--foo left, 0))"), "valid:anchor(--foo left, 0px)");
  EXPECT_EQ(parse(r, "inset-inline-start", "calc(anchor(self-start) + 1px)"), "valid:calc(1px + anchor(self-start))");
  EXPECT_EQ(parse(r, "top", "calc(anchor(calc(50%)))"), "valid:anchor(calc(50%))") << "a side may be a calculation of a percentage";
  EXPECT_EQ(parse(r, "width", "calc(anchor-size(--foo width, 0))"), "valid:anchor-size(--foo width, 0px)");
  EXPECT_EQ(parse(r, "width", "calc((anchor-size(--foo width) + anchor-size(--bar height)) / 2)"),
      "valid:calc(0.5 * (anchor-size(--foo width) + anchor-size(--bar height)))");
  EXPECT_EQ(parse(r, "margin-left", "calc(anchor-size(--a inline) * 2)"), "valid:calc(2 * anchor-size(--a inline))");
  EXPECT_EQ(parse(r, "width", "min(anchor-size(--foo width), anchor(--baz top))"), "invalid") << "anchor() is for the inset properties";
  EXPECT_EQ(parse(r, "color", "rgb(calc(anchor-size(--a width)), 0, 0)"), "invalid");
  EXPECT_EQ(parse(r, "top", "calc(anchor(--foo))"), "invalid") << "a side is required";
  EXPECT_EQ(parse(r, "top", "calc(anchor(--foo --bar top))"), "invalid");
  EXPECT_EQ(parse(r, "top", "calc(anchor(--foo bogus))"), "invalid");
  EXPECT_EQ(parse(r, "top", "calc(anchor(top, 1s))"), "invalid") << "the fallback is a <length-percentage>";
  EXPECT_EQ(parse(r, "top", "calc(anchor(top,))"), "invalid");
  EXPECT_EQ(parse(r, "width", "calc(anchor-size(--a bogus))"), "invalid");
}

TEST(CssProperty, APolygonRoundsWithARadiusThatIsNotNegative) {
  const GTEXT_CSS_Registry * r = R();
  // Shapes 1: "An optional <length> after a round keyword defines rounding for each vertex of the polygon. The length is the radius of a circle ..."
  EXPECT_EQ(parse(r, "clip-path", "polygon(round 1px, 1px 2px, 3px 4px)"), "valid:polygon(round 1px, 1px 2px, 3px 4px)");
  EXPECT_EQ(parse(r, "clip-path", "polygon(round -1px, 1px 2px)"), "invalid");
  EXPECT_EQ(parse(r, "offset-path", "polygon(round -1px, 1px 2px)"), "invalid");
}

TEST(CssProperty, PaintIsAnImageNotationOfThePaintingApi) {
  const GTEXT_CSS_Registry * r = R();
  // CSS Painting API 1: "The <paint()> function is an additional notation to be supported by the <image> type." with "paint() = paint( <ident>, <declaration-value>? )"
  EXPECT_EQ(parse(r, "background-image", "paint(mypaint)"), "valid:paint(mypaint)");
  EXPECT_EQ(parse(r, "background-image", "paint( mypaint )"), "valid:paint(mypaint)");
  EXPECT_EQ(parse(r, "background-image", "paint(mypaint, blue)"), "valid:paint(mypaint, blue)");
  EXPECT_EQ(parse(r, "background-image", "paint(mypaint, {})"), "valid:paint(mypaint, {})");
  EXPECT_EQ(parse(r, "background-image", "paint(1)"), "invalid");
  EXPECT_EQ(parse(r, "background-image", "paint()"), "invalid");
  EXPECT_EQ(parse(r, "background-image", "light-dark(url(a.png), paint(x))").substr(0, 6), "valid:") << "the light-dark() form of <image> is still there";
}

TEST(CssProperty, ADashedFunctionIsAnArbitrarySubstitutionFunctionWithItsOwnArguments) {
  const GTEXT_CSS_Registry * r = R();
  // Mixins 1, 2: "<dashed-function>s are arbitrary substitution functions, like var()", "<dashed-function> = --*( [ <declaration-value>? ]#? ,? )" (the pin of 2026-11-07;
  // the earlier one had "--*( <declaration-value>#? )") with the Note "That is, a comma-separated list of anything (including an empty value), optionally with a final trailing
  // comma"; Values 5 lets an argument that holds commas be wrapped in {}
  EXPECT_EQ(parse(r, "top", "--func()"), "pending:--func()");
  EXPECT_EQ(parse(r, "top", "--func(auto ,100px ,#fff)"), "pending:--func(auto ,100px ,#fff)");
  EXPECT_EQ(parse(r, "top", "--func({1, 2, 3},{4})"), "pending:--func({1, 2, 3},{4})");
  EXPECT_EQ(parse(r, "top", "--func(--bar(), --baz(--fez()))"), "pending:--func(--bar(), --baz(--fez()))");
  EXPECT_EQ(parse(r, "top", "calc(--f(1px) + 2px)").substr(0, 8), "pending:");
  EXPECT_EQ(parse(r, "top", "--func(a,)"), "pending:--func(a,)") << "a final trailing comma";
  EXPECT_EQ(parse(r, "top", "--func(,)"), "pending:--func(,)") << "\"--solo(,)\" passes a single empty argument";
  EXPECT_EQ(parse(r, "top", "--func(a,,b)"), "pending:--func(a,,b)") << "an empty value";
  EXPECT_EQ(parse(r, "top", "--func( , )"), "pending:--func( , )");
  EXPECT_EQ(parse(r, "top", "--func({})"), "invalid") << "an empty {} holds no value";
  EXPECT_EQ(parse(r, "top", "--func(123 {})"), "invalid") << "a {} is a whole argument";
  EXPECT_EQ(parse(r, "top", "--func({red !important})"), "invalid");
  EXPECT_EQ(parse(r, "top", "--func(;)"), "invalid");
  EXPECT_EQ(parse(r, "top", "--func(!)"), "invalid");
}

TEST(CssProperty, AMixOfDynamicRangeLimitsNeedsPercentagesThatCanBeNormalized) {
  const GTEXT_CSS_Registry * r = R();
  // Color HDR 1, 3.3: "Let p1, ..., pN be the mixing percentages, normalized to sum to 100%": a sum of 0% cannot be, and both pinned browsers refuse it
  EXPECT_EQ(parse(r, "dynamic-range-limit", "dynamic-range-limit-mix(no-limit 80%, standard 20%)"), "valid:dynamic-range-limit-mix(no-limit 80%, standard 20%)");
  EXPECT_EQ(parse(r, "dynamic-range-limit", "dynamic-range-limit-mix(no-limit 1%, standard 0%)").substr(0, 6), "valid:");
  EXPECT_EQ(parse(r, "dynamic-range-limit", "dynamic-range-limit-mix(no-limit 0%, standard 0%)"), "invalid");
  EXPECT_EQ(parse(r, "dynamic-range-limit", "dynamic-range-limit-mix(dynamic-range-limit-mix(no-limit 1%, standard 2%) 3%, dynamic-range-limit-mix(constrained 0%, no-limit 0%) 6%)"),
      "invalid") << "the sum of an inner mix counts too";
  EXPECT_EQ(parse(r, "dynamic-range-limit", "dynamic-range-limit-mix(standard calc(50% * sign(10em - 1px)), constrained 50%)").substr(0, 6), "valid:") << "a calculation is not a written percentage";
}

TEST(CssProperty, AStripeHasAThicknessOfANonNegativeLengthAPercentageOfAtMostOneHundredOrAFlex) {
  const GTEXT_CSS_Registry * r = R();
  // Images 4, 4 (served 2026-10-26): "<color-stripe> = <color> && [ <length-percentage> | <flex> ]?" with the thicknesses read as "<length [0,∞]>" ("Negative length
  // values are invalid.") and "<percentage [0,100]>" ("Only values between 0% and 100% (inclusive) are valid."); the pin keeps the unrestricted <length-percentage>
  EXPECT_EQ(parse(r, "border-color", "stripes(red 1px, blue 50%, green 1fr)"), "valid:stripes(red 1px, blue 50%, green 1fr)");
  EXPECT_EQ(parse(r, "border-color", "stripes(red 100%)"), "valid:stripes(red 100%)");
  EXPECT_EQ(parse(r, "border-color", "stripes(red 0%, blue 0px)"), "valid:stripes(red 0%, blue 0px)");
  EXPECT_EQ(parse(r, "border-color", "stripes(red -1px)"), "invalid") << "a negative length";
  EXPECT_EQ(parse(r, "border-color", "stripes(red -1%)"), "invalid") << "a percentage below 0%";
  EXPECT_EQ(parse(r, "border-color", "stripes(red 100.5%)"), "invalid") << "a percentage above 100%";
  EXPECT_EQ(parse(r, "border-color", "stripes(blue, red 1000%)"), "invalid");
  EXPECT_EQ(parse(r, "border-color", "stripes(red calc(1px - 2px))").substr(0, 6), "valid:") << "a calculation is clamped to the range when it is computed, not refused";
}

}  // namespace
