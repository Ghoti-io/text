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
 * The declaration object (css_cssom_decl.h): every operation of `CSSStyleDeclaration` in the order of the steps of CSSOM, named in the test of each step; the
 * outcomes of the ignores; the no-op elision and the events of the mutators; the status codes of the exceptions; the IDL attribute names; the typed value held
 * across a mutation; the limits and the counter exhaustion.
 */

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cssom_test_support.h"

using namespace cot;

namespace {

/* ---- reading --------------------------------------------------------------------------------------------------------------------------------- */

// CSSOM, "The CSSStyleDeclaration Interface": "The length attribute must return the number of CSS declarations in the declarations." / "The item(index) method must
// return the property name of the CSS declaration at position index." / past the end: the empty string.
TEST(CssomDeclRead, LengthItemAndTheEmptyStringPastTheEnd) {
  Doc doc;
  Decl d(doc);
  EXPECT_EQ(length(d.d), 0u);
  EXPECT_EQ(item(d.d, 0), "");
  ASSERT_EQ(set_text(d.d, "color: red; margin: 1px 2px; --Foo: x").st, GTEXT_CSS_OK);
  EXPECT_EQ(length(d.d), 6u) << "color, the four longhands of margin, and the custom property";
  EXPECT_EQ(item(d.d, 0), "color");
  EXPECT_EQ(item(d.d, 1), "margin-top");
  EXPECT_EQ(item(d.d, 4), "margin-left");
  EXPECT_EQ(item(d.d, 5), "--Foo");
  EXPECT_EQ(item(d.d, 6), "");
  EXPECT_EQ(item(d.d, SIZE_MAX), "");
}

// "getPropertyValue(property): If property is not a custom property, let property be property converted to ASCII lowercase. If property is a shorthand property, then ..."
TEST(CssomDeclRead, GetPropertyValueOfALonghandAShorthandAndACustomProperty) {
  Doc doc;
  Decl d(doc);
  ASSERT_EQ(set_text(d.d, "margin: 1px 2px; COLOR: Red; --Foo:  a  b ; width: 10PX").st, GTEXT_CSS_OK);
  EXPECT_EQ(get(d.d, "margin-top"), "1px");
  EXPECT_EQ(get(d.d, "MARGIN-TOP"), "1px") << "a property that is not custom is lowercased";
  EXPECT_EQ(get(d.d, "margin"), "1px 2px");
  EXPECT_EQ(get(d.d, "color"), "red");
  EXPECT_EQ(get(d.d, "width"), "10px");
  EXPECT_EQ(get(d.d, "--Foo"), "a  b") << "a custom property is its trimmed token text";
  EXPECT_EQ(get(d.d, "--foo"), "") << "and its name is case-sensitive";
  EXPECT_EQ(get(d.d, "bogus"), "");
  EXPECT_EQ(get(d.d, "height"), "");
  EXPECT_EQ(get(d.d, "padding"), "") << "a shorthand with a longhand missing is the empty string";
  EXPECT_EQ(get(d.d, ""), "");
  EXPECT_EQ(get(d.d, "--"), "");
}

// The matrix: "Get shorthand mixed: margin-top:1px !important; margin-right:1px ... getPropertyValue("margin")  empty (importance differs)".
TEST(CssomDeclRead, AShorthandWhoseLonghandsDifferInImportanceIsEmpty) {
  Doc doc;
  Decl d(doc);
  ASSERT_EQ(set_text(d.d, "margin-top: 1px !important; margin-right: 1px; margin-bottom: 1px; margin-left: 1px").st, GTEXT_CSS_OK);
  EXPECT_EQ(get(d.d, "margin"), "");
  EXPECT_EQ(priority(d.d, "margin"), "");
  EXPECT_EQ(priority(d.d, "margin-top"), "important");
  EXPECT_EQ(priority(d.d, "margin-right"), "");
}

// "getPropertyPriority(property): ... If property is a shorthand property, if all of the CSS declarations that it maps to have their important flag set, return "important"."
TEST(CssomDeclRead, GetPropertyPriority) {
  Doc doc;
  Decl d(doc);
  ASSERT_EQ(set_text(d.d, "margin: 1px !important; color: red; --a: 1 !important; width: 1px !important; padding-top: 1px !important").st, GTEXT_CSS_OK);
  EXPECT_EQ(priority(d.d, "margin"), "important");
  EXPECT_EQ(priority(d.d, "margin-left"), "important");
  EXPECT_EQ(priority(d.d, "color"), "");
  EXPECT_EQ(priority(d.d, "--a"), "important");
  EXPECT_EQ(priority(d.d, "--A"), "");
  EXPECT_EQ(priority(d.d, "WIDTH"), "important");
  EXPECT_EQ(priority(d.d, "padding"), "") << "only one of the four is declared";
  EXPECT_EQ(priority(d.d, "bogus"), "");
}

// Pending: "margin: var(--x)": the shorthand reads as its original value, and its longhands as the empty string.
TEST(CssomDeclRead, APendingShorthandIsItsOriginalValueAndItsLonghandsAreEmpty) {
  Doc doc;
  Decl d(doc);
  ASSERT_EQ(set(d.d, "margin", "var(--x)").st, GTEXT_CSS_OK);
  EXPECT_EQ(get(d.d, "margin"), "var(--x)");
  EXPECT_EQ(get(d.d, "margin-top"), "");
  EXPECT_EQ(text_of(d.d), "margin: var(--x);");
  EXPECT_EQ(length(d.d), 4u);
}

/* ---- setProperty: the steps in order -------------------------------------------------------------------------------------------------------- */

// "1. If the readonly flag is set, then throw a NoModificationAllowedError exception."
TEST(CssomDeclSet, AReadOnlyDeclarationThrowsNoModificationAllowedForEveryMutator) {
  Doc doc;
  GTEXT_CSSOM_Decl * raw = nullptr;
  ASSERT_EQ(css_decl_create(doc.core(), GTEXT_CSSOM_DECL_COMPUTED, 0, 1, 0, &raw), GTEXT_CSS_OK);
  Decl d(raw);
  EXPECT_TRUE(gtext_cssom_decl_readonly(d.d));
  EXPECT_EQ(set(d.d, "color", "red").st, GTEXT_CSS_E_NO_MODIFICATION);
  EXPECT_EQ(set(d.d, "bogus", "").st, GTEXT_CSS_E_NO_MODIFICATION) << "the readonly check is the first step";
  EXPECT_EQ(remove(d.d, "color").st, GTEXT_CSS_E_NO_MODIFICATION);
  EXPECT_EQ(set_text(d.d, "color: red").st, GTEXT_CSS_E_NO_MODIFICATION);
  EXPECT_STREQ(gtext_cssom_exception_name(GTEXT_CSS_E_NO_MODIFICATION), "NoModificationAllowedError");
  EXPECT_EQ(length(d.d), 0u);
  EXPECT_TRUE(doc.events.empty());
}

// "2. ... If property is not a case-sensitive match for a supported CSS property, then return." Before the value is looked at.
TEST(CssomDeclSet, AnUnsupportedPropertyReturnsBeforeTheValueIsLookedAt) {
  Doc doc;
  Decl d(doc);
  ASSERT_EQ(set(d.d, "color", "red").st, GTEXT_CSS_OK);
  const std::string before = dump(d.d);
  Result r = set(d.d, "bogus", "");
  EXPECT_EQ(r.st, GTEXT_CSS_OK);
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_IGNORED_UNKNOWN_PROPERTY) << "an empty value would be removeProperty, which would have removed nothing and said UNCHANGED";
  r = set(d.d, "bogus", "1");
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_IGNORED_UNKNOWN_PROPERTY);
  r = set(d.d, "--", "1");
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_IGNORED_UNKNOWN_PROPERTY) << "-- is no custom property";
  r = set(d.d, "--a b", "1");
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_IGNORED_UNKNOWN_PROPERTY) << "a name that cannot be written in a cssText and read back is no property";
  EXPECT_EQ(dump(d.d), before);
}

// "3. If value is the empty string, invoke removeProperty() with property as argument and return."
TEST(CssomDeclSet, AnEmptyValueIsRemoveProperty) {
  Doc doc;
  Decl d(doc);
  ASSERT_EQ(set_text(d.d, "color: red; width: 1px").st, GTEXT_CSS_OK);
  Result r = set(d.d, "color", "");
  EXPECT_EQ(r.st, GTEXT_CSS_OK);
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(text_of(d.d), "width: 1px;");
  r = set(d.d, "color", "");
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_UNCHANGED);
  // a priority that is not important is not looked at when the value is empty (step 3 comes before step 4)
  r = set(d.d, "width", "", "nonsense");
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(text_of(d.d), "");
}

// "4. If priority is not the empty string and is not an ASCII case-insensitive match for the string "important", then return."
TEST(CssomDeclSet, APriorityThatIsNotImportantIsIgnored) {
  Doc doc;
  Decl d(doc);
  Result r = set(d.d, "color", "red", "IMPORTANT");
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(text_of(d.d), "color: red !important;");
  r = set(d.d, "color", "blue", "ImPoRtAnT");
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(text_of(d.d), "color: blue !important;");
  const std::string before = dump(d.d);
  for (const char * bad : {"important!", "!important", " important", "importan", "impotant", "important ", "x"}) {
    r = set(d.d, "color", "green", bad);
    EXPECT_EQ(r.st, GTEXT_CSS_OK) << bad;
    EXPECT_EQ(r.o.kind, GTEXT_CSSOM_IGNORED_BAD_PRIORITY) << bad;
  }
  EXPECT_EQ(dump(d.d), before);
  r = set(d.d, "color", "green", "");
  EXPECT_EQ(text_of(d.d), "color: green;") << "an empty priority is not important";
}

// "5. Let component value list be the result of parsing value for property property. 6. If component value list is null, then return."
TEST(CssomDeclSet, AnInvalidValueAndAValueWithImportantAreIgnoredWithNothingChanged) {
  Doc doc;
  Decl d(doc);
  ASSERT_EQ(set(d.d, "color", "red").st, GTEXT_CSS_OK);
  const std::string before = dump(d.d);
  for (const char * bad : {"nonsense", "red !important", "!important", "1px", "red blue", "#12", "rgb(1,2)"}) {
    const Result r = set(d.d, "color", bad);
    EXPECT_EQ(r.st, GTEXT_CSS_OK) << bad;
    EXPECT_EQ(r.o.kind, GTEXT_CSSOM_IGNORED_INVALID_VALUE) << bad;
    EXPECT_NE(r.o.property, 0u) << "the outcome names the property";
  }
  EXPECT_EQ(dump(d.d), before) << "the bytes, the version and the order are as they were";
  EXPECT_TRUE(doc.events.size() == 1) << "only the first set sent an event";
}

// The matrix: "Custom property: setProperty("--Foo","  a  b ")": stored per CSS Variables 1; a registered custom property's value is not validated at set time.
TEST(CssomDeclSet, ACustomPropertyIsStoredByItsTokenText) {
  Doc doc;
  Decl d(doc);
  Result r = set(d.d, "--Foo", "  a  b ");
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(get(d.d, "--Foo"), "a  b");
  EXPECT_EQ(get(d.d, "--foo"), "");
  EXPECT_EQ(text_of(d.d), "--Foo: a  b;");
  r = set(d.d, "--Foo", "a  b");
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_UNCHANGED);
  r = set(d.d, "--foo", "c", "important");
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(length(d.d), 2u);
  EXPECT_EQ(text_of(d.d), "--Foo: a  b; --foo: c !important;");
  r = set(d.d, "--x", "var(--y)");
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(get(d.d, "--x"), "var(--y)");
  r = set(d.d, "--e", "initial");
  EXPECT_EQ(get(d.d, "--e"), "initial");
}

// The matrix: "Set property: style.setProperty("margin","1px 2px") on margin-top:5px: four longhands, margin-top replaced in place, others appended; cssText margin: 1px 2px;".
TEST(CssomDeclSet, AShorthandReplacesALonghandInPlaceAndAppendsTheRest) {
  Doc doc;
  Decl d(doc);
  ASSERT_EQ(set_text(d.d, "margin-top: 5px; color: red").st, GTEXT_CSS_OK);
  const Result r = set(d.d, "margin", "1px 2px");
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(length(d.d), 5u);
  EXPECT_EQ(item(d.d, 0), "margin-top") << "replaced in its place";
  EXPECT_EQ(item(d.d, 1), "color");
  EXPECT_EQ(item(d.d, 2), "margin-right");
  EXPECT_EQ(item(d.d, 4), "margin-left");
  EXPECT_EQ(text_of(d.d), "margin: 1px 2px; color: red;");
}

TEST(CssomDeclSet, ASetOfTheCurrentValueIsUnchangedAndEveryChangeBumpsOnce) {
  Doc doc;
  Decl d(doc);
  EXPECT_EQ(gtext_cssom_decl_version(d.d), 0u);
  EXPECT_EQ(set(d.d, "color", "red").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(gtext_cssom_decl_version(d.d), 1u);
  const uint64_t gen = gtext_cssom_document_generation(doc.d);
  EXPECT_EQ(set(d.d, "color", "red").o.kind, GTEXT_CSSOM_UNCHANGED);
  EXPECT_EQ(set(d.d, "color", "RED").o.kind, GTEXT_CSSOM_UNCHANGED);
  EXPECT_EQ(gtext_cssom_decl_version(d.d), 1u);
  EXPECT_EQ(gtext_cssom_document_generation(doc.d), gen);
  EXPECT_EQ(doc.events.size(), 1u);
  EXPECT_EQ(set(d.d, "margin", "1px 2px 3px").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(gtext_cssom_decl_version(d.d), 2u) << "four longhands, one version";
  EXPECT_EQ(set(d.d, "margin-left", "2px").o.kind, GTEXT_CSSOM_UNCHANGED) << "margin: 1px 2px 3px has margin-left 2px";
  EXPECT_EQ(set(d.d, "margin-left", "2px", "important").o.kind, GTEXT_CSSOM_CHANGED) << "the importance is part of the declaration";
  EXPECT_EQ(gtext_cssom_decl_version(d.d), 3u);
}

// The property the registry has and the library cannot judge is reported on a set, never guessed; a declaration of it that is read is kept as it was written ("preserved as
// the cascade does": the cascade drops it only for computing).
TEST(CssomDeclSet, APropertyTheLibraryCannotJudgeIsReportedNamingItAndKeptWhenRead) {
  Doc doc;
  Decl d(doc);
  const Result r = set(d.d, "voice-family", "male");
  EXPECT_EQ(r.st, GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_NE(r.o.property, 0u);
  EXPECT_NE(r.o.detail, nullptr);
  EXPECT_EQ(length(d.d), 0u);
  EXPECT_TRUE(doc.events.empty());
  // a parse of a list that has one keeps it, in its place, and counts it
  ASSERT_EQ(set_text(d.d, "color: red; voice-family: male !important; width: 1px").st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d), "color: red; voice-family: male !important; width: 1px;");
  EXPECT_EQ(length(d.d), 3u);
  EXPECT_EQ(item(d.d, 1), "voice-family");
  EXPECT_EQ(get(d.d, "voice-family"), "male");
  EXPECT_EQ(priority(d.d, "voice-family"), "important");
  GTEXT_CSSOM_Counters c;
  ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c), GTEXT_CSS_OK);
  EXPECT_EQ(c.unjudgeable_preserved, 1u);
  // it can be set only as the error, and it is removed like any declaration
  EXPECT_EQ(set(d.d, "voice-family", "female").st, GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_EQ(text_of(d.d), "color: red; voice-family: male !important; width: 1px;");
  EXPECT_EQ(set(d.d, "color", "blue").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(text_of(d.d), "color: blue; voice-family: male !important; width: 1px;") << "another property's edit leaves it alone";
  const Result rm = remove(d.d, "voice-family");
  EXPECT_EQ(rm.o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(rm.old, "male");
  EXPECT_EQ(text_of(d.d), "color: blue; width: 1px;");
  // the same text again is no change
  ASSERT_EQ(set_text(d.d, "voice-family: male").st, GTEXT_CSS_OK);
  EXPECT_EQ(set_text(d.d, "voice-family: male").o.kind, GTEXT_CSSOM_UNCHANGED);
  // a shorthand the library cannot split is kept too, and is its own value
  ASSERT_EQ(set_text(d.d, "font: caption; color: red").st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d), "font: caption; color: red;");
  EXPECT_EQ(get(d.d, "font"), "caption");
  EXPECT_EQ(length(d.d), 2u);
}

// A shorthand and its CSS-wide keywords, `revert` kept as written, the legacy alias as its property.
TEST(CssomDeclSet, CssWideKeywordsRevertAndAliases) {
  Doc doc;
  Decl d(doc);
  EXPECT_EQ(set(d.d, "margin", "inherit").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(get(d.d, "margin"), "inherit");
  EXPECT_EQ(get(d.d, "margin-top"), "inherit");
  EXPECT_EQ(set(d.d, "color", "revert-layer").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(get(d.d, "color"), "revert-layer");
  EXPECT_EQ(set(d.d, "word-wrap", "break-word").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(get(d.d, "overflow-wrap"), "break-word");
  EXPECT_EQ(get(d.d, "word-wrap"), "break-word");
  EXPECT_EQ(length(d.d), 6u);
  EXPECT_EQ(set(d.d, "all", "unset").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(get(d.d, "all"), "unset");
}

/* ---- removeProperty ------------------------------------------------------------------------------------------------------------------------------ */

// "removeProperty(property): ... Let value be the return value of invoking getPropertyValue() with property as argument."
TEST(CssomDeclRemove, ReturnsTheOldValueAndRemovesAShorthandsLonghands) {
  Doc doc;
  Decl d(doc);
  ASSERT_EQ(set_text(d.d, "margin: 1px 2px; color: red; --a: 1").st, GTEXT_CSS_OK);
  Result r = remove(d.d, "margin");
  EXPECT_EQ(r.st, GTEXT_CSS_OK);
  EXPECT_EQ(r.old, "1px 2px");
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(text_of(d.d), "color: red; --a: 1;");
  r = remove(d.d, "COLOR");
  EXPECT_EQ(r.old, "red");
  r = remove(d.d, "--A");
  EXPECT_EQ(r.old, "");
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_UNCHANGED);
  r = remove(d.d, "--a");
  EXPECT_EQ(r.old, "1");
  EXPECT_EQ(text_of(d.d), "");
  r = remove(d.d, "bogus");
  EXPECT_EQ(r.st, GTEXT_CSS_OK);
  EXPECT_EQ(r.old, "");
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_UNCHANGED);
}

TEST(CssomDeclRemove, ALonghandRemovedLeavesTheShorthandUnreadable) {
  Doc doc;
  Decl d(doc);
  ASSERT_EQ(set(d.d, "margin", "1px").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(remove(d.d, "margin-top").old, "1px");
  EXPECT_EQ(get(d.d, "margin"), "");
  EXPECT_EQ(text_of(d.d), "margin-right: 1px; margin-bottom: 1px; margin-left: 1px;");
  // a shorthand of which only some longhands are present: the value is empty and the longhands go
  const Result r = remove(d.d, "margin");
  EXPECT_EQ(r.old, "");
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(length(d.d), 0u);
}

/* ---- cssText -------------------------------------------------------------------------------------------------------------------------------------- */

// The matrix: style.cssText = "color:red;bogus:1;margin:1px": two declarations; the old ones gone; nested rules in the text dropped.
TEST(CssomDeclCssText, TheSetterReplacesTheWholeListAndDropsWhatDoesNotParse) {
  Doc doc;
  Decl d(doc);
  ASSERT_EQ(set(d.d, "width", "1px").o.kind, GTEXT_CSSOM_CHANGED);
  Result r = set_text(d.d, "color:red;bogus:1;margin:1px");
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(text_of(d.d), "color: red; margin: 1px;");
  EXPECT_EQ(get(d.d, "width"), "");
  r = set_text(d.d, "color: red; & b { color: blue }; width: 2px; @media print { a { b: c } } z-index: 3");
  EXPECT_EQ(text_of(d.d), "color: red; width: 2px; z-index: 3;") << "nested rules in the text are dropped";
  r = set_text(d.d, "");
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(text_of(d.d), "");
  r = set_text(d.d, "nonsense");
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_UNCHANGED) << "nothing parsed, and there was nothing";
}

TEST(CssomDeclCssText, ABytewiseIdenticalTextIsUnchangedWithNoVersionAndNoEvent) {
  Doc doc;
  Decl d(doc);
  ASSERT_EQ(set_text(d.d, "color: red; margin: 1px").o.kind, GTEXT_CSSOM_CHANGED);
  const uint64_t v = gtext_cssom_decl_version(d.d);
  const size_t events = doc.events.size();
  const Result r = set_text(d.d, "margin:1px; color:red;");
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_CHANGED) << "the order is part of the cssText";
  EXPECT_EQ(gtext_cssom_decl_version(d.d), v + 1);
  const Result same = set_text(d.d, "margin: 1px; color: red;");
  EXPECT_EQ(same.o.kind, GTEXT_CSSOM_UNCHANGED);
  EXPECT_EQ(gtext_cssom_decl_version(d.d), v + 1);
  EXPECT_EQ(doc.events.size(), events + 1);
}

// A list of declarations: of two declarations of a property the later one wins; an important one is not overridden by a later one that is not.
TEST(CssomDeclCssText, TheLaterDeclarationOfAPropertyWinsAndImportantIsKept) {
  Doc doc;
  Decl d(doc);
  ASSERT_EQ(set_text(d.d, "color: red; width: 1px; color: blue").st, GTEXT_CSS_OK);
  EXPECT_EQ(get(d.d, "color"), "blue");
  EXPECT_EQ(length(d.d), 2u);
  ASSERT_EQ(set_text(d.d, "color: red !important; color: blue; --a: 1 !important; --a: 2").st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d), "color: red !important; --a: 1 !important;");
  ASSERT_EQ(set_text(d.d, "color: red; color: blue !important").st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d), "color: blue !important;");
  ASSERT_EQ(set_text(d.d, "margin-top: 1px !important; margin: 2px").st, GTEXT_CSS_OK);
  EXPECT_EQ(get(d.d, "margin-top"), "1px");
  EXPECT_EQ(priority(d.d, "margin-top"), "important");
  EXPECT_EQ(get(d.d, "margin-left"), "2px");
}

/* ---- the events ----------------------------------------------------------------------------------------------------------------------------------- */

TEST(CssomDeclEvents, OneEventPerChangeWithTheSortedChangedPropertiesAndNoneForANoOp) {
  Doc doc;
  Decl d(doc);
  ASSERT_EQ(set(d.d, "margin", "1px 2px").o.kind, GTEXT_CSSOM_CHANGED);
  ASSERT_EQ(doc.events.size(), 1u);
  EXPECT_EQ(doc.events[0].kind, GTEXT_CSSOM_EVENT_DECLARATIONS_CHANGED);
  EXPECT_EQ(doc.events[0].props.size(), 4u);
  EXPECT_TRUE(std::is_sorted(doc.events[0].props.begin(), doc.events[0].props.end(), [](const std::string & a, const std::string & b) {
    GTEXT_CSS_Property_Id x = reg::find(R(), a), y = reg::find(R(), b);
    return x < y;
  }));
  EXPECT_EQ(doc.events[0].declaration, d.d);
  EXPECT_EQ(doc.events[0].generation, gtext_cssom_document_generation(doc.d));
  ASSERT_EQ(set(d.d, "margin", "1px 2px").o.kind, GTEXT_CSSOM_UNCHANGED);
  ASSERT_EQ(set(d.d, "bogus", "x").o.kind, GTEXT_CSSOM_IGNORED_UNKNOWN_PROPERTY);
  ASSERT_EQ(set(d.d, "color", "nonsense").o.kind, GTEXT_CSSOM_IGNORED_INVALID_VALUE);
  EXPECT_EQ(doc.events.size(), 1u);
  ASSERT_EQ(set(d.d, "--b", "1").o.kind, GTEXT_CSSOM_CHANGED);
  ASSERT_EQ(doc.events.size(), 2u);
  EXPECT_TRUE(doc.events[1].props.empty());
  EXPECT_EQ(doc.events[1].customs, std::vector<std::string>{"--b"});
  ASSERT_EQ(set(d.d, "all", "inherit").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_TRUE(doc.events[2].all);
  ASSERT_EQ(remove(d.d, "margin").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(doc.events[3].props.size(), 4u);
  ASSERT_EQ(set_text(d.d, "color: red; --c: 2").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(doc.events[4].customs, (std::vector<std::string>{"--b", "--c"}));
  EXPECT_NE(std::find(doc.events[4].props.begin(), doc.events[4].props.end(), "color"), doc.events[4].props.end());
  EXPECT_GT(doc.events[4].props.size(), 500u) << "the text without `all` removes the longhands it set, and the list names them";
  EXPECT_NE(std::find(doc.events[4].props.begin(), doc.events[4].props.end(), "width"), doc.events[4].props.end());
  EXPECT_FALSE(doc.events[4].all) << "the margin had been removed before: the change does not reach every longhand of all";
}

// A mutation from inside a callback is refused, and the first mutation completes.
TEST(CssomDeclEvents, AMutationFromInsideACallbackIsReentrantAndTheFirstCompletes) {
  Doc doc;
  Decl d(doc);
  GTEXT_CSS_Status inner = GTEXT_CSS_OK, inner2 = GTEXT_CSS_OK, inner3 = GTEXT_CSS_OK;
  doc.inside = [&]() {
    inner = set(d.d, "width", "2px").st;
    inner2 = remove(d.d, "color").st;
    inner3 = set_text(d.d, "width: 1px").st;
    return GTEXT_CSS_E_WRITE;   // an observer that fails
  };
  const Result r = set(d.d, "color", "red");
  EXPECT_EQ(r.st, GTEXT_CSS_OK);
  EXPECT_EQ(inner, GTEXT_CSS_E_REENTRANT);
  EXPECT_EQ(inner2, GTEXT_CSS_E_REENTRANT);
  EXPECT_EQ(inner3, GTEXT_CSS_E_REENTRANT);
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_CHANGED) << "a failing observer never undoes the mutation";
  EXPECT_EQ(r.o.observer_status, GTEXT_CSS_E_WRITE);
  EXPECT_EQ(text_of(d.d), "color: red;");
  // reads are allowed inside a callback
  doc.inside = [&]() {
    EXPECT_EQ(get(d.d, "color"), "blue") << "the state is complete and consistent when the event goes out";
    return GTEXT_CSS_OK;
  };
  EXPECT_EQ(set(d.d, "color", "blue").o.observer_status, GTEXT_CSS_OK);
}

/* ---- the IDL attribute names ---------------------------------------------------------------------------------------------------------------------- */

// The matrix: "IDL names: style.cssFloat, style.webkitTransform, style['font-size'], style.fontSize  float, -webkit-transform, font-size, font-size".
TEST(CssomDeclIdl, AttributeNamesAreTheGeneratedTable) {
  struct Row {
    const char * attribute;
    const char * property;
  };
  const Row rows[] = {{"cssFloat", "float"}, {"float", "float"}, {"webkitTransform", "-webkit-transform"}, {"WebkitTransform", "-webkit-transform"},
      {"-webkit-transform", "-webkit-transform"}, {"font-size", "font-size"}, {"fontSize", "font-size"}, {"backgroundColor", "background-color"},
      {"color", "color"}, {"wordWrap", "word-wrap"}, {"insetBlockStart", "inset-block-start"}, {"webkitBorderRadius", "-webkit-border-radius"}};
  for (const Row & r : rows) {
    const char * prop = nullptr;
    size_t n = 0;
    ASSERT_TRUE(gtext_cssom_idl_property(r.attribute, strlen(r.attribute), &prop, &n)) << r.attribute;
    EXPECT_EQ(std::string(prop, n), r.property) << r.attribute;
  }
  for (const char * not_css : {"foo", "FontSize", "font_size", "cssText", "length", "getPropertyValue", "", "fontsize", "Float"}) {
    EXPECT_FALSE(gtext_cssom_idl_property(not_css, strlen(not_css), nullptr, nullptr)) << not_css;
  }
}

TEST(CssomDeclIdl, GetAndSetThroughAnAttribute) {
  Doc doc;
  Decl d(doc);
  Result r;
  r.st = gtext_cssom_decl_idl_set(d.d, "backgroundColor", 15, "red", 3, &r.o);
  EXPECT_EQ(r.st, GTEXT_CSS_OK);
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_CHANGED);
  char * out = nullptr;
  size_t n = 0;
  ASSERT_EQ(gtext_cssom_decl_idl_get(d.d, "background-color", 16, &out, &n), GTEXT_CSS_OK);
  EXPECT_EQ(take(doc.core(), out), "red");
  ASSERT_EQ(gtext_cssom_decl_idl_set(d.d, "cssFloat", 8, "left", 4, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(get(d.d, "float"), "left");
  ASSERT_EQ(gtext_cssom_decl_idl_set(d.d, "backgroundColor", 15, "", 0, &r.o), GTEXT_CSS_OK) << "the empty string removes the property";
  EXPECT_EQ(text_of(d.d), "float: left;");
  EXPECT_EQ(gtext_cssom_decl_idl_get(d.d, "cssText", 7, &out, &n), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_decl_idl_set(d.d, "length", 6, "x", 1, nullptr), GTEXT_CSS_E_INVALID);
}

// "CSS property to IDL attribute", the other direction, over every property of the registry: the camel-cased attribute is in the table and names the property, and so is the dashed one.
TEST(CssomDeclIdl, EveryPropertyOfTheRegistryRoundTrips) {
  const size_t count = gtext_css_registry_property_count(R());
  size_t checked = 0;
  for (size_t i = 1; i <= count; i++) {
    GTEXT_CSS_Property_Info info;
    ASSERT_TRUE(gtext_css_registry_property_info(R(), static_cast<GTEXT_CSS_Property_Id>(i), &info));
    if (info.flags & GTEXT_CSS_PROP_CUSTOM) {
      continue;
    }
    char * camel = nullptr;
    size_t cl = 0;
    ASSERT_EQ(gtext_cssom_idl_attribute(info.name, info.name_length, nullptr, &camel, &cl), GTEXT_CSS_OK);
    const char * prop = nullptr;
    size_t pl = 0;
    ASSERT_TRUE(gtext_cssom_idl_property(camel, cl, &prop, &pl)) << info.name << " -> " << camel;
    EXPECT_EQ(std::string(prop, pl), std::string(info.name, info.name_length)) << camel;
    gtext_cssom_buffer_free(nullptr, camel);
    ASSERT_TRUE(gtext_cssom_idl_property(info.name, info.name_length, &prop, &pl)) << info.name;
    EXPECT_EQ(std::string(prop, pl), std::string(info.name, info.name_length));
    checked++;
  }
  EXPECT_GT(checked, 800u);
}

/* ---- the typed value ------------------------------------------------------------------------------------------------------------------------------ */

// The matrix: "Held typed value: v = decl_value_set(decl, "color"); removeProperty("color"); read v: v valid and unchanged".
TEST(CssomDeclValue, ATypedValueIsRetainedAndOutlivesAnyMutation) {
  Doc doc;
  Decl d(doc);
  ASSERT_EQ(set(d.d, "color", "red").o.kind, GTEXT_CSSOM_CHANGED);
  GTEXT_CSS_Value_Set * v = nullptr;
  GTEXT_CSS_Wide_Keyword wide = GTEXT_CSS_WIDE_UNSET;
  bool important = true, found = false;
  ASSERT_EQ(gtext_cssom_decl_value_set(d.d, "COLOR", 5, &v, &wide, &important, &found), GTEXT_CSS_OK);
  ASSERT_TRUE(found);
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(wide, GTEXT_CSS_WIDE_NONE);
  EXPECT_FALSE(important);
  ASSERT_EQ(remove(d.d, "color").old, "red");
  ASSERT_EQ(set(d.d, "color", "blue").o.kind, GTEXT_CSSOM_CHANGED);
  ASSERT_EQ(set_text(d.d, "").o.kind, GTEXT_CSSOM_CHANGED);
  const GTEXT_CSS_Value * root = gtext_css_value_set_root(v);
  ASSERT_NE(root, nullptr);
  char * buf = nullptr;
  size_t n = 0;
  ASSERT_EQ(gtext_css_serialize_value_buffer(root, nullptr, &buf, &n), GTEXT_CSS_OK);
  EXPECT_EQ(std::string(buf, n), "red");
  gtext_css_serialize_buffer_free(nullptr, buf);
  gtext_css_value_set_free(v);
  // a CSS-wide keyword has no set; none found
  ASSERT_EQ(set(d.d, "color", "inherit").o.kind, GTEXT_CSSOM_CHANGED);
  v = nullptr;
  ASSERT_EQ(gtext_cssom_decl_value_set(d.d, "color", 5, &v, &wide, &important, &found), GTEXT_CSS_OK);
  EXPECT_TRUE(found);
  EXPECT_EQ(v, nullptr);
  EXPECT_EQ(wide, GTEXT_CSS_WIDE_INHERIT);
  ASSERT_EQ(gtext_cssom_decl_value_set(d.d, "width", 5, &v, &wide, &important, &found), GTEXT_CSS_OK);
  EXPECT_FALSE(found);
  ASSERT_EQ(gtext_cssom_decl_value_set(d.d, "margin", 6, &v, &wide, &important, &found), GTEXT_CSS_OK);
  EXPECT_FALSE(found) << "a shorthand has no value of its own";
}

/* ---- limits and counters ---------------------------------------------------------------------------------------------------------------------------- */

TEST(CssomDeclLimits, DeclarationsPerBlockAndTheInputAreExactAndAFailedCallChangesNothing) {
  Doc doc([](GTEXT_CSSOM_Config & c) {
    c.max_declarations = 4;
    c.max_input_bytes = 32;
  });
  Decl d(doc);
  ASSERT_EQ(set(d.d, "margin", "1px").o.kind, GTEXT_CSSOM_CHANGED);
  const std::string before = dump(d.d);
  Result r = set(d.d, "color", "red");
  EXPECT_EQ(r.st, GTEXT_CSS_E_LIMIT_DECLARATIONS);
  r = set(d.d, "padding", "1px");
  EXPECT_EQ(r.st, GTEXT_CSS_E_LIMIT_DECLARATIONS);
  r = set(d.d, "margin", "2px");
  EXPECT_EQ(r.st, GTEXT_CSS_OK) << "replacing needs no room";
  const std::string mid = dump(d.d);
  r = set(d.d, "--a", std::string(40, 'x'));
  EXPECT_EQ(r.st, GTEXT_CSS_E_LIMIT_INPUT);
  r = set_text(d.d, std::string(40, ' ') + "color: red");
  EXPECT_EQ(r.st, GTEXT_CSS_E_LIMIT_INPUT);
  r = set_text(d.d, "margin: 1px; color: red");
  EXPECT_EQ(r.st, GTEXT_CSS_E_LIMIT_DECLARATIONS);
  EXPECT_EQ(dump(d.d), mid);
  EXPECT_NE(before, mid);
}

TEST(CssomDeclLimits, LiveObjectsAndBytes) {
  Doc doc([](GTEXT_CSSOM_Config & c) { c.max_live_objects = 3; });
  Decl a(doc), b(doc), c(doc);
  GTEXT_CSSOM_Decl * x = nullptr;
  EXPECT_EQ(gtext_cssom_decl_new(doc.d, &x), GTEXT_CSS_E_LIMIT_CSSOM);
  EXPECT_EQ(x, nullptr);
  gtext_cssom_decl_release(c.d);
  c.d = nullptr;
  ASSERT_EQ(gtext_cssom_decl_new(doc.d, &x), GTEXT_CSS_OK) << "a freed object gives its place back";
  gtext_cssom_decl_release(x);
  Doc small([](GTEXT_CSSOM_Config & cfg) { cfg.max_live_bytes = 100; });
  EXPECT_EQ(gtext_cssom_decl_new(small.d, &x), GTEXT_CSS_E_LIMIT_CSSOM);
}

// The matrix: "Counter exhaustion: first_generation 2^64 - 2, two mutations: the second refuses, nothing changed."
TEST(CssomDeclLimits, TheGenerationCounterIsSpentAtTheEndAndRefusesWholly) {
  Doc doc([](GTEXT_CSSOM_Config & c) { c.first_generation = UINT64_MAX - 2; });
  Decl d(doc);
  EXPECT_EQ(gtext_cssom_document_generation(doc.d), UINT64_MAX - 2);
  EXPECT_EQ(set(d.d, "color", "red").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(gtext_cssom_document_generation(doc.d), UINT64_MAX - 1);
  EXPECT_EQ(set(d.d, "width", "1px").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(gtext_cssom_document_generation(doc.d), UINT64_MAX);
  const std::string before = dump(d.d);
  const size_t events = doc.events.size();
  EXPECT_EQ(set(d.d, "height", "1px").st, GTEXT_CSS_E_LIMIT_CSSOM);
  EXPECT_EQ(remove(d.d, "width").st, GTEXT_CSS_E_LIMIT_CSSOM);
  EXPECT_EQ(set_text(d.d, "").st, GTEXT_CSS_E_LIMIT_CSSOM);
  EXPECT_EQ(dump(d.d), before);
  EXPECT_EQ(doc.events.size(), events);
  EXPECT_EQ(get(d.d, "width"), "1px") << "reads still work";
  // ids start where the config says
  Doc ids([](GTEXT_CSSOM_Config & c) { c.first_id = 1000; });
  Decl e(ids);
  EXPECT_EQ(reinterpret_cast<css_obj *>(e.d)->id, 1000u);
}

/* ---- the owner hook (what a rule fills) ------------------------------------------------------------------------------------------------------------- */

struct OwnerLog {
  GTEXT_CSS_Status refuse = GTEXT_CSS_OK;
  int prepares = 0, commits = 0;
  uint64_t sheet_id = 0, rule_id = 0;
  std::vector<std::string> order;
};

TEST(CssomDeclOwner, PrepareMayRefuseBeforeAnythingChangesAndCommitSeesTheEvent) {
  Doc doc;
  GTEXT_CSSOM_Decl * raw = nullptr;
  ASSERT_EQ(css_decl_create(doc.core(), GTEXT_CSSOM_DECL_RULE, 0, 1, 0, &raw), GTEXT_CSS_OK);
  Decl d(raw);
  static OwnerLog log;
  log = OwnerLog();
  static const css_decl_owner_ops ops = {
      [](css_obj *, GTEXT_CSSOM_Decl *) -> GTEXT_CSS_Status {
        log.prepares++;
        log.order.push_back("prepare");
        return log.refuse;
      },
      [](css_obj *, GTEXT_CSSOM_Decl *, css_cssom_event * ev) {
        log.commits++;
        log.order.push_back("commit");
        ev->sheet_id = 7;
        ev->rule_id = 9;
      }};
  raw->ops = &ops;
  raw->owner = reinterpret_cast<css_obj *>(raw);   // any object will do for the double
  ASSERT_EQ(set(d.d, "color", "red").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(log.prepares, 1);
  EXPECT_EQ(log.commits, 1);
  EXPECT_EQ(log.order, (std::vector<std::string>{"prepare", "commit"}));
  EXPECT_EQ(set(d.d, "color", "red").o.kind, GTEXT_CSSOM_UNCHANGED);
  EXPECT_EQ(log.commits, 1) << "a no-op is no commit";
  log.refuse = GTEXT_CSS_E_SECURITY;
  const std::string before = dump(d.d);
  EXPECT_EQ(set(d.d, "color", "blue").st, GTEXT_CSS_E_SECURITY);
  EXPECT_EQ(remove(d.d, "color").st, GTEXT_CSS_E_SECURITY);
  EXPECT_EQ(set_text(d.d, "").st, GTEXT_CSS_E_SECURITY);
  EXPECT_EQ(dump(d.d), before);
  EXPECT_EQ(log.commits, 1);
  EXPECT_EQ(gtext_cssom_decl_owner(d.d), GTEXT_CSSOM_DECL_RULE);
  raw->ops = nullptr;
}

/* ---- the exception names -------------------------------------------------------------------------------------------------------------------------------- */

TEST(CssomStatus, TheNineContentStatusesHaveTheirDomExceptionNamesAndNoOtherDoes) {
  struct Row {
    GTEXT_CSS_Status st;
    const char * name;
  };
  const Row rows[] = {{GTEXT_CSS_E_SYNTAX, "SyntaxError"}, {GTEXT_CSS_E_HIERARCHY, "HierarchyRequestError"}, {GTEXT_CSS_E_INDEX_SIZE, "IndexSizeError"},
      {GTEXT_CSS_E_INVALID_STATE, "InvalidStateError"}, {GTEXT_CSS_E_NOT_ALLOWED, "NotAllowedError"},
      {GTEXT_CSS_E_NO_MODIFICATION, "NoModificationAllowedError"}, {GTEXT_CSS_E_NOT_FOUND, "NotFoundError"}, {GTEXT_CSS_E_SECURITY, "SecurityError"},
      {GTEXT_CSS_E_INVALID_ACCESS, "InvalidAccessError"}};
  for (const Row & r : rows) {
    ASSERT_NE(gtext_cssom_exception_name(r.st), nullptr);
    EXPECT_STREQ(gtext_cssom_exception_name(r.st), r.name);
    EXPECT_STRNE(gtext_css_status_string(r.st), "unknown status");
  }
  for (int s = 0; s < GTEXT_CSS_STATUS_COUNT; s++) {
    bool content = false;
    for (const Row & r : rows) {
      content = content || r.st == s;
    }
    if (!content) {
      EXPECT_EQ(gtext_cssom_exception_name(static_cast<GTEXT_CSS_Status>(s)), nullptr) << gtext_css_status_string(static_cast<GTEXT_CSS_Status>(s));
    }
  }
  EXPECT_EQ(gtext_cssom_exception_name(GTEXT_CSS_E_INVALID), nullptr) << "a misuse of the API is never a DOMException";
  EXPECT_EQ(gtext_cssom_exception_name(GTEXT_CSS_E_REENTRANT), nullptr);
  EXPECT_EQ(gtext_cssom_exception_name(GTEXT_CSS_E_LIMIT_CSSOM), nullptr);
  EXPECT_EQ(gtext_cssom_exception_name(GTEXT_CSS_OK), nullptr);
  EXPECT_EQ(gtext_cssom_exception_name(static_cast<GTEXT_CSS_Status>(GTEXT_CSS_STATUS_COUNT + 5)), nullptr);
  // every status has its own string
  std::set<std::string> seen;
  for (int s = 0; s < GTEXT_CSS_STATUS_COUNT; s++) {
    EXPECT_TRUE(seen.insert(gtext_css_status_string(static_cast<GTEXT_CSS_Status>(s))).second) << s;
  }
  EXPECT_STREQ(gtext_css_status_string(GTEXT_CSS_E_LIMIT_WORK), "a call did more than its max_work (max_match_work or max_memo_entries of a match)");
}

TEST(CssomDeclArguments, NullAndMisuseAreInvalidAndNeverAContentError) {
  Doc doc;
  Decl d(doc);
  char * out = nullptr;
  size_t n = 0;
  EXPECT_EQ(gtext_cssom_decl_new(nullptr, &d.d), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_decl_get_property_value(nullptr, "a", 1, &out, &n), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_decl_get_property_value(d.d, nullptr, 3, &out, &n), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_decl_set_property(d.d, nullptr, 3, "x", 1, "", 0, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_decl_set_css_text(d.d, nullptr, 3, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_decl_length(d.d, nullptr), GTEXT_CSS_E_INVALID);
  GTEXT_CSSOM_Config bad = gtext_cssom_config_default();
  GTEXT_CSSOM_Document * doc2 = nullptr;
  EXPECT_EQ(gtext_cssom_document_new(&bad, &doc2), GTEXT_CSS_E_INVALID) << "no registry";
  EXPECT_EQ(gtext_cssom_document_new(nullptr, &doc2), GTEXT_CSS_E_INVALID);
  gtext_cssom_document_release(nullptr);
  gtext_cssom_decl_release(nullptr);
  EXPECT_EQ(gtext_cssom_object_kind(nullptr), GTEXT_CSSOM_OBJECT_NONE);
  EXPECT_EQ(gtext_cssom_object_kind(d.d), GTEXT_CSSOM_OBJECT_DECLARATION);
}

} // namespace

// CSSOM leaves the place of a repeated property in a parsed list open; the recorded answers of Chromium 156 and Firefox 158 (css_cssom_gen.py, tools/oracle/css_cssom_divergences.txt) are the
// same: the later declaration is appended and the earlier one is gone, where setProperty() replaces in place.
TEST(CssomDecl, AParsedListAppendsTheLaterDeclarationAndDropsTheEarlierOne) {
  Doc doc;
  Decl d(doc);
  ASSERT_EQ(set_text(d.d, "color:red;margin:1px;color:blue").st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d), "margin: 1px; color: blue;");
  ASSERT_EQ(set_text(d.d, "padding-top:1px;margin-left:2px;padding-top:3px").st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d), "margin-left: 2px; padding-top: 3px;");
  ASSERT_EQ(set_text(d.d, "color:red;margin:1px;color:blue;margin-left:3px;color:green").st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d), "margin: 1px 1px 1px 3px; color: green;");
  ASSERT_EQ(set_text(d.d, "margin-top:1px;color:red;margin:2px").st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d), "color: red; margin: 2px;") << "a shorthand after a longhand it sets: the group goes to the end";
  ASSERT_EQ(set_text(d.d, "--a:1;color:red;--a:2").st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d), "color: red; --a: 2;");
  ASSERT_EQ(set_text(d.d, "color:red !important;margin:1px;color:blue").st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d), "color: red !important; margin: 1px;") << "an important declaration is not replaced by one that is not, and keeps its place (Firefox's; Chromium moves it: browsers-differ)";
  // setProperty() is the other algorithm: it replaces in place
  ASSERT_EQ(set_text(d.d, "color:red;margin:1px").st, GTEXT_CSS_OK);
  ASSERT_EQ(set(d.d, "color", "blue").st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d), "color: blue; margin: 1px;");
}

// found by the libFuzzer soak of fuzz_cssom_decl (reproducer-set-over-opaque-longhand.seed): a longhand kept as written because the library cannot judge it (voice-family) that is set
// to a value it can judge is updated where it stands, as "set a CSS declaration" says, and not moved to the end
TEST(CssomDecl, ALonghandKeptAsWrittenIsUpdatedInPlaceWhenItIsSetToAValueTheLibraryCanJudge) {
  Doc doc;
  Decl d(doc);
  ASSERT_EQ(set_text(d.d, "voice-family: male !important; color: red; margin: 1px").st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d), "voice-family: male !important; color: red; margin: 1px;");
  ASSERT_EQ(set(d.d, "voice-family", "var(--x)").st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d), "voice-family: var(--x); color: red; margin: 1px;");
  ASSERT_EQ(set(d.d, "color", "blue").st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d), "voice-family: var(--x); color: blue; margin: 1px;");
}

// found by the libFuzzer soak of fuzz_cssom_decl (reproducer-item-order-after-css-text.seed): cssText set to text that serializes the same as the declarations, but whose longhands stand in
// another order (a complete margin is written as one shorthand whatever the order), left the old order in place; item(i) showed it
TEST(CssomDecl, CssTextSetToTheSameTextStillTakesTheOrderOfTheNewList) {
  Doc doc;
  Decl d(doc);
  ASSERT_EQ(set_text(d.d, "margin-right:1px;margin-bottom:1px;margin-left:1px;margin-top:1px").st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d), "margin: 1px;");
  ASSERT_EQ(set_text(d.d, "margin: 1px").st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d), "margin: 1px;");
  char * name = nullptr;
  size_t len = 0;
  ASSERT_EQ(gtext_cssom_decl_item(d.d, 0, &name, &len), GTEXT_CSS_OK);
  EXPECT_EQ(std::string(name, len), "margin-top");
  gtext_cssom_buffer_free(gtext_cssom_document_allocator(doc.d), name);
}

// found by the libFuzzer soak of fuzz_cssom_decl (reproducer-set-over-a-wide-keyword-shorthand.seed): setProperty() of a shorthand whose earlier declaration was kept as written replaces that
// declaration; a longhand declared before it is set where it stands, since the shorthand that would have overridden it is the one being replaced
TEST(CssomDecl, SettingAShorthandKeptAsWrittenLeavesTheLonghandsAlreadyThereInPlace) {
  Doc doc;
  Decl d(doc);
  ASSERT_EQ(set_text(d.d, "color: red; font: caption; font-style: italic; font: caption !important; --a: 1").st, GTEXT_CSS_OK);
  ASSERT_EQ(set(d.d, "font", "revert").st, GTEXT_CSS_OK);
  char * name = nullptr;
  size_t len = 0;
  ASSERT_EQ(gtext_cssom_decl_item(d.d, 1, &name, &len), GTEXT_CSS_OK);
  EXPECT_EQ(std::string(name, len), "font-style") << "font-style keeps its place, after color";
  gtext_cssom_buffer_free(gtext_cssom_document_allocator(doc.d), name);
  ASSERT_EQ(gtext_cssom_decl_item(d.d, 2, &name, &len), GTEXT_CSS_OK);
  EXPECT_EQ(std::string(name, len), "--a") << "the other longhands of font are new declarations: they go behind --a";
  gtext_cssom_buffer_free(gtext_cssom_document_allocator(doc.d), name);
}

// found by the libFuzzer soak of fuzz_cssom_decl (reproducer-later-shorthand-over-an-important-one-kept-as-written.seed): in a list of declarations a later `font` that is not important does
// not override an earlier `font: caption !important`, which the library keeps as written; it used to replace it, so text written with both and read back was not the text written
TEST(CssomDecl, ALaterShorthandThatIsNotImportantDoesNotReplaceAnImportantOneKeptAsWritten) {
  Doc doc;
  Decl d(doc);
  ASSERT_EQ(set_text(d.d, "font: caption !important; font: revert; color: red").st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d), "font: caption !important; color: red;");
  ASSERT_EQ(set_text(d.d, "font: caption !important; font: revert !important").st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d), "font: revert !important;") << "an important one does replace it";
  ASSERT_EQ(set_text(d.d, "font: caption; font: revert").st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d), "font: revert;") << "and so does a later one when the earlier is not important";
  ASSERT_EQ(set_text(d.d, "color: red; font: caption !important; all: revert; color: red").st, GTEXT_CSS_OK);
  const std::string once = text_of(d.d);
  ASSERT_EQ(set_text(d.d, once.c_str()).st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d) == once, true) << "written, read and written again is the same text";
  // the same state made by setProperty(): `all` set over a shorthand kept as written leaves it where it stands, and the shorthand its longhands would form is not written beside it
  ASSERT_EQ(set_text(d.d, "color: red; font: caption; font-style: italic; font: caption !important").st, GTEXT_CSS_OK);
  ASSERT_EQ(set(d.d, "all", "revert").st, GTEXT_CSS_OK);
  ASSERT_EQ(set(d.d, "color", "red").st, GTEXT_CSS_OK);
  const std::string made = text_of(d.d);
  EXPECT_EQ(made.find("font: revert"), std::string::npos) << "font is written once, as it was kept";
  EXPECT_NE(made.find("font: caption !important"), std::string::npos);
  ASSERT_EQ(set_text(d.d, made.c_str()).st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d) == made, true) << "written, read and written again is the same text";
}

/* ---- cssText: the shorthand form, and what stands between the longhands ----------------------------------------------------------------- */

// CSSOM, "serialize into a shorthand form": "If any of the intermixed properties in decls between used longhands belong to the same logical property group as a longhand in used
// longhands but have a different mapping logic, and are not in used longhands, continue." The case is web-platform-tests' cssom/cssstyledeclaration-csstext.html; Firefox 158 writes it
// so, and Chromium 156 writes the shorthands anyway (a divergence of the CSSOM differential, listed).
TEST(CssomDecl, AShorthandIsNotWrittenWhenAPropertyOfItsGroupWithAnotherMappingStandsBetweenItsLonghands) {
  Doc doc;
  Decl d(doc);
  ASSERT_EQ(set_text(d.d, "margin: 10px; margin-inline: 10px; margin-block: 10px; margin-inline-end: 10px; margin-bottom: 10px;").st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d), "margin-top: 10px; margin-right: 10px; margin-left: 10px; margin-inline-start: 10px; margin-block: 10px; margin-inline-end: 10px; margin-bottom: 10px;");
  EXPECT_EQ(get(d.d, "margin"), "10px") << "getPropertyValue is not cssText: the four longhands are all there";
  ASSERT_EQ(set_text(d.d, "margin: 10px; margin-inline: 10px; margin-block: 10px; margin-bottom: 10px;").st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d), "margin-top: 10px; margin-right: 10px; margin-left: 10px; margin-inline: 10px; margin-block: 10px; margin-bottom: 10px;");
  // with no property of another mapping between them the shorthands are written
  ASSERT_EQ(set_text(d.d, "margin-top: 10px; margin-left: 10px; margin-right: 10px; margin-bottom: 10px; margin-inline-start: 10px; margin-inline-end: 10px; "
                          "margin-block-start: 10px; margin-block-end: 10px;").st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d), "margin: 10px; margin-inline: 10px; margin-block: 10px;");
  ASSERT_EQ(set_text(d.d, "margin: 10px; margin-inline-end: 10px;").st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d), "margin: 10px; margin-inline-end: 10px;") << "a property after the shorthand is not between its longhands";
  ASSERT_EQ(set_text(d.d, "padding-top: 1px; padding-inline-start: 2px; padding-left: 1px; padding-right: 1px; padding-bottom: 1px;").st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d), "padding-top: 1px; padding-inline-start: 2px; padding-left: 1px; padding-right: 1px; padding-bottom: 1px;");
  ASSERT_EQ(set_text(d.d, "top: 1px; inset-block-start: 2px; right: 1px; bottom: 1px; left: 1px;").st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d), "top: 1px; inset-block-start: 2px; right: 1px; bottom: 1px; left: 1px;");
  ASSERT_EQ(set_text(d.d, "color: red; margin-top: 1px; width: 2px; margin-left: 1px; margin-right: 1px; margin-bottom: 1px;").st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d), "color: red; margin: 1px; width: 2px;") << "a property of no group does not stand in the way: the shorthand is written where its first longhand was";
}

// Fonts 4: the `font` shorthand takes the keywords of `font-width`, and a percentage a keyword names is that keyword. A calculation that simplifies to such a percentage is it as well:
// both Chromium 156 and Firefox 158 write `ultra-condensed medium serif` for `calc(50%)`, and web-platform-tests' font-shorthand-serialization-font-stretch.html says the case is not
// well specified and consistent across browsers. One that does not reduce to a percentage names none, so the shorthand is not written.
TEST(CssomDecl, FontTakesAFontStretchCalculationThatIsAPercentageAKeywordNames) {
  Doc doc;
  Decl d(doc);
  ASSERT_EQ(set_text(d.d, "font: medium serif").st, GTEXT_CSS_OK);
  ASSERT_EQ(set(d.d, "font-stretch", "calc(50%)").st, GTEXT_CSS_OK);
  EXPECT_EQ(get(d.d, "font-stretch"), "calc(50%)");
  EXPECT_EQ(get(d.d, "font"), "ultra-condensed medium serif");
  ASSERT_EQ(set(d.d, "font-stretch", "calc(25% * 2 + 0%)").st, GTEXT_CSS_OK);
  EXPECT_EQ(get(d.d, "font"), "ultra-condensed medium serif");
  ASSERT_EQ(set(d.d, "font-stretch", "calc(100%)").st, GTEXT_CSS_OK);
  EXPECT_EQ(get(d.d, "font"), "medium serif") << "calc(100%) is normal, which the shorthand leaves out";
  ASSERT_EQ(set(d.d, "font-stretch", "calc(112.5%)").st, GTEXT_CSS_OK);
  EXPECT_EQ(get(d.d, "font"), "semi-expanded medium serif");
  ASSERT_EQ(set(d.d, "font-stretch", "calc(60%)").st, GTEXT_CSS_OK);
  EXPECT_EQ(get(d.d, "font"), "") << "no keyword names 60%";
  ASSERT_EQ(set(d.d, "font-stretch", "calc(50% + 25% * sign(100em - 1px))").st, GTEXT_CSS_OK);
  EXPECT_EQ(get(d.d, "font"), "") << "a calculation that is not a percentage names none";
  EXPECT_EQ(get(d.d, "font-stretch"), "calc(50% + (25% * sign(100em - 1px)))");
  ASSERT_EQ(set(d.d, "font-stretch", "calc(50%)").st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(d.d).find("font: ultra-condensed medium serif"), 0u) << text_of(d.d) << " (cssText writes the shorthand too)";
}

// CSS Layout API 1 (the Editor's Draft): "A new alternative value is added to the <display-inside> production: layout(<ident>)." Neither pinned browser takes it (the library follows
// the sentence; the CSSOM differential lists it), and the empty `layout()` and a second argument stay invalid.
TEST(CssomDecl, DisplayTakesLayoutWithOneName) {
  Doc doc;
  Decl d(doc);
  ASSERT_EQ(set(d.d, "display", "layout(test1)").st, GTEXT_CSS_OK);
  EXPECT_EQ(get(d.d, "display"), "layout(test1)");
  ASSERT_EQ(set(d.d, "display", "block").st, GTEXT_CSS_OK);
  ASSERT_EQ(set(d.d, "display", "layout()").st, GTEXT_CSS_OK);
  EXPECT_EQ(get(d.d, "display"), "block") << "layout() is not a value";
  ASSERT_EQ(set(d.d, "display", "layout(test3, invalid)").st, GTEXT_CSS_OK);
  EXPECT_EQ(get(d.d, "display"), "block");
  ASSERT_EQ(set(d.d, "display", "inline layout(test1)").st, GTEXT_CSS_OK);
  EXPECT_EQ(get(d.d, "display"), "inline layout(test1)");
}

// found by the libFuzzer soak of fuzz_cssom_decl (reproducer-shorthand-set-over-a-pending-all*.seed): `all: var(--x)` leaves a pending longhand for every property, the logical
// ones among them, and `border: red` replaces only the border ones. The pending ones are not written (the text they hold is `all`'s, and only `all` can say it), so they are not in
// the text that is read again; the logical ones among them must not stop the physical `border` from being written, or the text and the text of that text differ. Chromium 156 and
// Firefox 158 both write `border: red` there (and write the rest differently from each other and from this library, which leaves it out).
TEST(CssomDeclCssText, APendingLonghandThatIsNotWrittenDoesNotStopAShorthandBeingWritten) {
  for (const char * value : {"red", "inherit", "initial", "1px solid red"}) {
    Doc doc;
    Decl d(doc);
    ASSERT_EQ(set(d.d, "all", "var(--x)").st, GTEXT_CSS_OK);
    ASSERT_EQ(set(d.d, "border", value).st, GTEXT_CSS_OK);
    const std::string text = text_of(d.d);
    EXPECT_EQ(text, std::string("border: ") + value + ";") << "the pending longhands of `all` are left out, and the border longhands make `border`";
    Decl again(doc);
    ASSERT_EQ(set_text(again.d, text).st, GTEXT_CSS_OK);
    EXPECT_EQ(text_of(again.d), text) << "serialize, parse and serialize is a fixed point";
  }
  // the same for the other shorthands that overlap a logical group, and for a side shorthand
  for (const char * name : {"border-width", "border-color", "margin", "padding", "inset", "border-radius"}) {
    Doc doc;
    Decl d(doc);
    ASSERT_EQ(set(d.d, "all", "var(--x)").st, GTEXT_CSS_OK);
    ASSERT_EQ(set(d.d, name, "inherit").st, GTEXT_CSS_OK);
    EXPECT_EQ(text_of(d.d), std::string(name) + ": inherit;");
  }
}

// The decision to write a shorthand over a logical property that sits between its longhands is kept where that logical property is written: a pending shorthand that is whole
// (`border-inline: var(--x)`) is written as itself, and it is a declaration the text holds.
TEST(CssomDeclCssText, AWrittenPendingShorthandStillCountsAsAnIntermixedLogicalProperty) {
  Doc doc;
  Decl d(doc);
  ASSERT_EQ(set_text(d.d, "border-top-width: 1px; border-inline-width: var(--x); border-right-width: 1px; border-bottom-width: 1px; border-left-width: 1px").st, GTEXT_CSS_OK);
  const std::string text = text_of(d.d);
  EXPECT_NE(text.find("border-inline-width: var(--x)"), std::string::npos);
  EXPECT_EQ(text.find("border-width"), std::string::npos) << "border-inline-width, written, sits between the longhands of border-width";
  Decl again(doc);
  ASSERT_EQ(set_text(again.d, text).st, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(again.d), text);
}
