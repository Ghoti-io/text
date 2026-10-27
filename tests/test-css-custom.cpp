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
 * Custom properties and arbitrary substitution functions (css_custom.h, css_substitute.h), each test named for the rule it holds and quoting the
 * served draft: the I/O matrix of the slice first, then custom property values, `var()`, cycles, invalid substitution, shorthands, the other functions
 * (`env()`, `attr()`, `if()`, `inherit()`, `ident()`, `random-item()`, `random()`, the tree counting functions), the spread syntax, the budget and the limits.
 */

#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_custom_test_support.h"

using namespace cust;

namespace {

/// The computed value of `prop` of an element declared `decls`, under a parent declared `parent`, in a world with no registrations.
std::string prop_of(const std::string & parent, const std::string & decls, const std::string & prop) {
  World w;
  EXPECT_EQ(w.build(), GTEXT_CSS_OK);
  Computed p, c;
  compute(w, p, parent);
  EXPECT_EQ(p.status, GTEXT_CSS_OK) << parent;
  compute(w, c, decls, &p);
  EXPECT_EQ(c.status, GTEXT_CSS_OK) << decls << " " << c.result.name << " " << (c.result.detail ? c.result.detail : "");
  return c.value(w.registry.r, prop);
}

/// The computed value of the custom property `name` of an element declared `decls` under a parent.
std::string custom_of(const std::string & parent, const std::string & decls, const std::string & name) {
  World w;
  EXPECT_EQ(w.build(), GTEXT_CSS_OK);
  Computed p, c;
  compute(w, p, parent);
  compute(w, c, decls, &p);
  EXPECT_EQ(c.status, GTEXT_CSS_OK) << decls;
  return c.custom(name);
}

/* ---- the matrix of the slice ----------------------------------------------------------------------------------- */

TEST(CssCustom, TheMatrixOfTheSlice) {
  // "Fallback": the fallback is used when the property is missing
  EXPECT_EQ(prop_of("", "width: var(--w, 10px)", "width"), "10px");
  // "Guaranteed-invalid": invalid at computed-value time, so `unset`: width is not inherited, so initial
  EXPECT_EQ(prop_of("width: 5px", "width: var(--w)", "width"), "auto");
  // ... and for an inherited property, the inherited value
  EXPECT_EQ(prop_of("color: red", "color: var(--w)", "color"), "red");
  // "Cycle": both are guaranteed-invalid
  EXPECT_EQ(custom_of("", "--a: var(--b); --b: var(--a)", "--a"), "<gi>");
  EXPECT_EQ(custom_of("", "--a: var(--b); --b: var(--a)", "--b"), "<gi>");
  // "Empty": --x:; then margin: var(--x) 1px is 1px
  EXPECT_EQ(prop_of("", "--x:; margin: var(--x) 1px", "margin-top"), "1px");
  EXPECT_EQ(prop_of("", "--x:; margin: var(--x) 1px", "margin-left"), "1px");
  // "Shorthand": margin: var(--m), --m: 1px 2px gives four longhands
  EXPECT_EQ(prop_of("", "--m: 1px 2px; margin: var(--m)", "margin-top"), "1px");
  EXPECT_EQ(prop_of("", "--m: 1px 2px; margin: var(--m)", "margin-right"), "2px");
  EXPECT_EQ(prop_of("", "--m: 1px 2px; margin: var(--m)", "margin-bottom"), "1px");
  EXPECT_EQ(prop_of("", "--m: 1px 2px; margin: var(--m)", "margin-left"), "2px");
  // "Missing env": env(nosuch, 4px) is 4px
  EXPECT_EQ(prop_of("", "width: env(nosuch, 4px)", "width"), "4px");
}

/* ---- custom property values -------------------------------------------------------------------------------------- */

TEST(CssCustom, TheValueIsTheOriginalText) {
  // "Specified values of custom properties must be serialized exactly as specified by the author" ... "--uuid: 12345678-12e3-8d9b-a456-426614174000"
  EXPECT_EQ(custom_of("", "--uuid: 12345678-12e3-8d9b-a456-426614174000", "--uuid"), "12345678-12e3-8d9b-a456-426614174000");
  EXPECT_EQ(custom_of("", "--c: /* a */ 1.0   2.50 /* b */ x /* c */", "--c"), "1.0   2.50 /* b */ x") << "a comment is no token: the text of the tokens runs from the first to the last";
  EXPECT_EQ(custom_of("", "--c: Red", "--c"), "Red") << "\"The values of custom properties ... are case-sensitive\"";
}

TEST(CssCustom, NamesAreCaseSensitive) {
  // "Unlike other CSS properties, custom property names are not ASCII case-insensitive."
  EXPECT_EQ(prop_of("", "--foo: 1px; --FOO: 2px; width: var(--foo)", "width"), "1px");
  EXPECT_EQ(prop_of("", "--foo: 1px; --FOO: 2px; width: var(--FOO)", "width"), "2px");
  EXPECT_EQ(prop_of("", "--foo: 1px; width: var(--Foo, 3px)", "width"), "3px");
}

TEST(CssCustom, EmptyIsNotTheGuaranteedInvalidValue) {
  // "The guaranteed-invalid value serializes as the empty string, but actually writing an empty value into a custom property, like --foo:;, is a valid
  // (empty) value, not the guaranteed-invalid value."
  World w;
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  Computed c;
  compute(w, c, "--empty:; --reset: 1px");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  const GTEXT_CSS_Custom_Value * v = nullptr;
  EXPECT_TRUE(gtext_css_computed_custom_find(c.s, "--empty", 7, &v));
  EXPECT_EQ(gtext_css_custom_value_length(v), 0u);
  EXPECT_FALSE(gtext_css_computed_custom_find(c.s, "--never", 7, &v));
  Computed d;
  compute(w, d, "--reset: initial", &c);
  EXPECT_FALSE(gtext_css_computed_custom_find(d.s, "--reset", 7, &v)) << "initial is the guaranteed-invalid value";
  EXPECT_TRUE(gtext_css_computed_custom_find(d.s, "--empty", 7, &v)) << "and the empty value is inherited";
}

TEST(CssCustom, ExplicitDefaulting) {
  // "The CSS-wide keywords can be used in custom properties, with the same meaning as in any another property."
  EXPECT_EQ(custom_of("--p: 1px", "--p: inherit", "--p"), "1px");
  EXPECT_EQ(custom_of("--p: 1px", "--p: unset", "--p"), "1px") << "custom properties are inherited";
  EXPECT_EQ(custom_of("--p: 1px", "--p: initial", "--p"), "<gi>");
  EXPECT_EQ(custom_of("--p: 1px", "", "--p"), "1px");
  // "(they are) not preserved as the custom property's value, and thus are not substituted in by the corresponding variable"
  EXPECT_EQ(prop_of("--p: 1px", "--p: initial; width: var(--p, 7px)", "width"), "7px");
}

TEST(CssCustom, CustomPropertiesInherit) {
  EXPECT_EQ(prop_of("--w: 12px", "width: var(--w)", "width"), "12px");
  // "Variables always draw from the computed value of the associated custom property on the same element"
  EXPECT_EQ(prop_of("--c: blue", "--c: green; color: var(--c)", "color"), "green");
}

/* ---- var() ------------------------------------------------------------------------------------------------------------- */

TEST(CssCustom, FallbackMayHoldCommasAndBeEmptyAndNest) {
  // "For example, var(--foo, red, blue) defines a fallback of red, blue; that is, anything between the first comma and the end of the function is considered a
  // fallback value."
  EXPECT_EQ(prop_of("", "font-family: var(--f, Arial, serif)", "font-family"), "Arial, serif");
  // "var(--a,) is a valid function, specifying that if the --a custom property is invalid or missing, the var() should be replaced with nothing."
  EXPECT_EQ(prop_of("", "margin: 1px var(--a,) 2px", "margin-left"), "2px");
  EXPECT_EQ(prop_of("", "--b: 4px; width: var(--a, var(--b))", "width"), "4px");
  EXPECT_EQ(prop_of("", "width: var(--a, var(--b, 6px))", "width"), "6px");
}

TEST(CssCustom, TheNameMayComeFromAVarFunction) {
  // "Note that the custom property name looked up by a var() may itself come from a var() function: :root { --other: 10px; --myvar: --other; --result:
  // var(var(--myvar)); } ... the computed value of --result becomes 10px."
  EXPECT_EQ(custom_of("", "--other: 10px; --myvar: --other; --result: var(var(--myvar))", "--result"), "10px");
}

TEST(CssCustom, SubstitutionIsOfTokens) {
  // ".foo { --gap: 20; margin-top: var(--gap)px; } This is not equivalent to setting margin-top: 20px; ... it's equivalent to margin-top: 20 px"
  EXPECT_EQ(prop_of("", "--gap: 20; margin-top: var(--gap)px", "margin-top"), "0px");
  // "calc() can be used to validly achieve the same thing"
  EXPECT_EQ(prop_of("", "--gap: 20; margin-top: calc(var(--gap) * 1px)", "margin-top"), "20px");
  // "--not-px-length: var(--gap)px ... will serialize with a comment between the two tokens, like 20/**/px"
  EXPECT_EQ(custom_of("", "--gap: 20; --not-px-length: var(--gap)px", "--not-px-length"), "20/**/px");
}

TEST(CssCustom, ComputedValueReplacesTheFunctionsAndNothingElse) {
  // "Computed values of custom properties must similarly be serialized exactly as specified by the author ... save for the replacement of any var() functions."
  EXPECT_EQ(custom_of("", "--y: 1px; --x: a  var(--y)   b", "--x"), "a  1px   b");
  EXPECT_EQ(custom_of("", "--y:; --x: [var(--y)]", "--x"), "[]");
}

TEST(CssCustom, ACustomPropertyThatIsAWideKeywordAfterSubstitutionIsThatKeyword) {
  // "If a property value, after property replacement, contains only a single CSS-wide keyword (and possibly whitespace/comments), its value is determined as if
  // that keyword were its specified value all along." A custom property is a property: `--c: var(--x, unset)` is `--c: unset`, which inherits.
  const std::string parent = "--c: green; --d: green; --f: blue";
  const std::string child = "--c: var(--x, unset); --d: var(--x, initial); --e: var(--x, inherit); --f: var(--x, /* a */ inherit /* b */); --g: var(--x, revert)";
  EXPECT_EQ(custom_of(parent, child, "--c"), "green");
  EXPECT_EQ(custom_of(parent, child, "--d"), "<gi>");
  EXPECT_EQ(custom_of(parent, child, "--e"), "<gi>");
  EXPECT_EQ(custom_of(parent, child, "--f"), "blue");
  EXPECT_EQ(custom_of(parent, child, "--g"), "<gi>") << "the cascade is over: revert is invalid at computed-value time";
}

TEST(CssCustom, AnArgumentThatIsNotADeclarationValueIsInvalidAtParseTimeAndTheDeclarationBeforeItStands) {
  // "<declaration-value> ... does not contain ... top-level <semicolon-token> tokens or <delim-token> tokens with a value of "!"": the fallback of var() is
  // one, so `var(--b,!)` is not a valid substitution function and the declaration that holds it is invalid at parse time ("all of those functions are themselves
  // syntactically valid according to their argument grammars"). The pairs are given whole, since a `;` in a value is not a separator of declarations.
  auto after = [](const std::string & value, const std::string & name) {
    World w;
    EXPECT_EQ(w.build(), GTEXT_CSS_OK);
    Computed c;
    compute_pairs(w, c, {{"--a", "green"}, {"width", "5px"}, {"--a", value}, {"width", value}});
    EXPECT_EQ(c.status, GTEXT_CSS_OK) << value;
    return name == "--a" ? c.custom("--a") : c.value(w.registry.r, name);
  };
  for (const char * bad : {"var(--b,!)", "var(--b,;)", "var(--b,!important)", "var(--b) !important !important", "var(--b);", "var()"}) {
    EXPECT_EQ(after(bad, "--a"), "green") << bad;
    EXPECT_EQ(after(bad, "width"), "5px") << bad;
  }
  EXPECT_EQ(after("var(--b, foo(!))", "width"), "auto") << "a `!` inside a nested function is not top level: the declaration is valid, and invalid at computed-value time";
}

TEST(CssCustom, ACycleMakesEveryPropertyInItInvalid) {
  // ":root { --one: var(--two); --two: var(--three, baz); --three: var(--one); } ... The presence of a fallback in var(--three, baz) does not affect the
  // outcome."
  const std::string css = "--one: var(--two); --two: var(--three, baz); --three: var(--one)";
  EXPECT_EQ(custom_of("", css, "--one"), "<gi>");
  EXPECT_EQ(custom_of("", css, "--two"), "<gi>");
  EXPECT_EQ(custom_of("", css, "--three"), "<gi>");
  // ":root { --one: calc(var(--two) + 20px); --two: calc(var(--one) - 20px); } In the above, both --one and --two compute to the guaranteed-invalid value"
  EXPECT_EQ(custom_of("", "--one: calc(var(--two) + 20px); --two: calc(var(--one) - 20px)", "--one"), "<gi>");
  EXPECT_EQ(custom_of("", "--one: calc(var(--two) + 20px); --two: calc(var(--one) - 20px)", "--two"), "<gi>");
  // a self reference
  EXPECT_EQ(custom_of("", "--a: var(--a)", "--a"), "<gi>");
}

TEST(CssCustom, ALongCycleCostsWhatItHolds) {
  World w;
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  std::string css;
  const int n = 2000;
  for (int i = 0; i < n; i++) {
    css += "--p" + std::to_string(i) + ": var(--p" + std::to_string((i + 1) % n) + "); ";
  }
  css += "width: var(--p0, 9px)";
  Computed c;
  compute(w, c, css);
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.custom("--p0"), "<gi>");
  EXPECT_EQ(c.custom("--p1999"), "<gi>");
  EXPECT_EQ(c.value(w.registry.r, "width"), "9px");
}

TEST(CssCustom, ReferencesToACycleThatIsNotInItAreOrdinaryReferences) {
  // --a depends on a cycle but is not part of it: it sees the guaranteed-invalid value and uses its fallback
  const std::string css = "--b: var(--c); --c: var(--b); --a: var(--b, ok)";
  EXPECT_EQ(custom_of("", css, "--a"), "ok");
  EXPECT_EQ(custom_of("", css, "--b"), "<gi>");
}

/* ---- invalid substitution ------------------------------------------------------------------------------------------------- */

TEST(CssCustom, InvalidAtComputedValueTimeIsUnsetNotTheEarlierDeclaration) {
  // ":root { --not-a-color: 20px; } p { background-color: red; } p { background-color: var(--not-a-color); } the <p> elements will have transparent backgrounds
  // (the initial value for background-color), rather than red backgrounds."
  EXPECT_EQ(prop_of("", "--not-a-color: 20px; background-color: red; background-color: var(--not-a-color)", "background-color"), "transparent");
  // "If the property was one that's inherited by default, such as color, it would compute to the inherited value rather than the initial value."
  EXPECT_EQ(prop_of("color: blue", "--not-a-color: 20px; color: var(--not-a-color)", "color"), "blue");
}

TEST(CssCustom, AWideKeywordAfterSubstitutionIsTheKeyword) {
  // "If a property value, after property replacement, contains only a single CSS-wide keyword (and possibly whitespace/comments), its value is determined as if
  // that keyword were its specified value all along."
  EXPECT_EQ(prop_of("color: blue", "color: var(--x, initial)", "color"), "canvastext");
  EXPECT_EQ(prop_of("width: 5px", "width: var(--x, inherit)", "width"), "5px");
  EXPECT_EQ(prop_of("color: blue", "--k: inherit; color: var(--k, red)", "color"), "red") << "a custom property cannot hold a keyword: it is the property's own defaulting";
}

TEST(CssCustom, TheValueNeedsTheGrammarOfTheProperty) {
  EXPECT_EQ(prop_of("", "--v: 5; width: var(--v)", "width"), "auto") << "a number is no width";
  EXPECT_EQ(prop_of("", "--v: 5px; width: var(--v)", "width"), "5px");
  EXPECT_EQ(prop_of("", "--v: red; width: var(--v)", "width"), "auto");
}

/* ---- shorthands ------------------------------------------------------------------------------------------------------------ */

TEST(CssCustom, AShorthandIsSubstitutedWholeAndExpanded) {
  // "after substitution, the shorthand must be parsed and the longhands must be given their appropriate values at that point"
  EXPECT_EQ(prop_of("", "--b: 2px solid red; border: var(--b)", "border-top-width"), "2px");
  EXPECT_EQ(prop_of("", "--b: 2px solid red; border: var(--b)", "border-left-color"), "red");
  EXPECT_EQ(prop_of("", "--s: solid; border: 3px var(--s) blue", "border-bottom-style"), "solid");
  // an invalid result makes every longhand invalid at computed-value time
  EXPECT_EQ(prop_of("", "--b: nonsense; border-top-style: solid; border: var(--b)", "border-top-style"), "none") << "unset: the initial value";
  // a longhand declared after the shorthand wins
  EXPECT_EQ(prop_of("", "--m: 1px; margin: var(--m); margin-left: 5px", "margin-left"), "5px");
  EXPECT_EQ(prop_of("", "--m: 1px; margin-left: 5px; margin: var(--m)", "margin-left"), "1px");
}

TEST(CssCustom, AShorthandOfWideKeyword) {
  EXPECT_EQ(prop_of("margin-top: 7px", "margin: var(--x, inherit)", "margin-top"), "7px");
}

/* ---- env() ------------------------------------------------------------------------------------------------------------------ */

TEST(CssCustom, EnvTakesTheEnvironmentsValueOrTheFallback) {
  World w;
  w.env_vars["safe-area-inset-top"] = "12px";
  w.env_vars["keyboard-inset,1,2"] = "3px";
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  Computed c;
  compute(w, c, "width: env(safe-area-inset-top, 1px); height: env(keyboard-inset 1 2); margin-top: env(nosuch); padding-top: env(nosuch 5, 4px)");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.value(w.registry.r, "width"), "12px");
  EXPECT_EQ(c.value(w.registry.r, "height"), "3px") << "the indices are part of the name";
  EXPECT_EQ(c.value(w.registry.r, "margin-top"), "0px") << "an unknown name with no fallback is guaranteed-invalid: unset";
  EXPECT_EQ(c.value(w.registry.r, "padding-top"), "4px");
}

TEST(CssCustom, EnvWithoutACallbackKnowsNoName) {
  EXPECT_EQ(prop_of("", "width: env(anything, 9px)", "width"), "9px");
}

/* ---- attr() ------------------------------------------------------------------------------------------------------------------ */

struct AttrWorld {
  World w;
  Node node;
  explicit AttrWorld(std::map<std::string, std::string> attrs) {
    node.attrs = std::move(attrs);
    EXPECT_EQ(w.build(), GTEXT_CSS_OK);
  }
  std::string value(const std::string & decl, const std::string & prop) {
    Computed c;
    compute(w, c, decl, nullptr, &node);
    EXPECT_EQ(c.status, GTEXT_CSS_OK) << decl << " " << c.result.name << " " << (c.result.detail ? c.result.detail : "");
    return c.value(w.registry.r, prop);
  }
  std::string custom(const std::string & decl, const std::string & name) {
    Computed c;
    compute(w, c, decl, nullptr, &node);
    EXPECT_EQ(c.status, GTEXT_CSS_OK) << decl;
    return c.custom(name);
  }
};

TEST(CssCustom, AttrTypedAsALength) {
  // "If given as an <attr-unit> value, the value is first parsed as if number keyword was specified ... the number is turned into a dimension"
  AttrWorld a({{"length", "12"}});
  EXPECT_EQ(a.value("width: attr(length em, 0px)", "width"), "192px");
  EXPECT_EQ(a.value("width: attr(length px)", "width"), "12px");
  EXPECT_EQ(a.value("width: attr(length %)", "width"), "12%");
  // "does not match a known CSS unit, it triggers fallback"
  EXPECT_EQ(a.value("width: attr(length nosuchunit, 3px)", "width"), "3px");
  // "If the attribute is missing ... fallback"
  EXPECT_EQ(a.value("width: attr(missing px, 4px)", "width"), "4px");
}

TEST(CssCustom, AttrNumberRequiresANumberToken) {
  AttrWorld a({{"n", " 7 "}, {"bad", "7px"}});
  EXPECT_EQ(a.value("z-index: attr(n number, 1)", "z-index"), "7") << "leading and trailing white space are stripped";
  EXPECT_EQ(a.value("z-index: attr(bad number, 2)", "z-index"), "2");
  EXPECT_EQ(a.value("z-index: attr(bad number)", "z-index"), "auto") << "no fallback and a type: guaranteed-invalid";
}

TEST(CssCustom, AttrRawStringIsAStringWithoutParsing) {
  // "If given as the raw-string keyword, or omitted entirely, it causes the attribute's literal value to be treated as the value of a CSS string, with no CSS
  // parsing performed at all"
  AttrWorld a({{"label", "a \"b\" /* c */"}});
  EXPECT_EQ(a.custom("--s: attr(label raw-string)", "--s"), "\"a \\\"b\\\" /* c */\"");
  EXPECT_EQ(a.custom("--s: attr(label)", "--s"), "\"a \\\"b\\\" /* c */\"");
  // "If the <attr-type> argument is omitted, the fallback defaults to the empty string"
  EXPECT_EQ(a.custom("--s: attr(nope)", "--s"), "\"\"");
  // "otherwise, it defaults to the guaranteed-invalid value if omitted"
  EXPECT_EQ(a.custom("--s: attr(nope raw-string)", "--s"), "<gi>");
}

TEST(CssCustom, AttrTypeOfASyntax) {
  AttrWorld a({{"w", "10px"}, {"c", "red"}, {"x", "10"}});
  EXPECT_EQ(a.value("width: attr(w type(<length>))", "width"), "10px");
  EXPECT_EQ(a.value("color: attr(c type(<color>), blue)", "color"), "red");
  EXPECT_EQ(a.value("color: attr(x type(<color>), blue)", "color"), "blue") << "\"Values that fail to parse according to the syntax trigger fallback.\"";
  EXPECT_EQ(a.custom("--t: attr(w type(*))", "--t"), "10px");
  EXPECT_EQ(a.custom("--t: attr(w type(<length> | auto))", "--t"), "10px");
  EXPECT_EQ(a.custom("--t: attr(w type(<number>), none)", "--t"), "none");
}

TEST(CssCustom, AttrWithAnUrlSyntaxIsInvalid) {
  // "For this purpose, <url> is invalid as a <syntax-single-component>."
  AttrWorld a({{"u", "url(x.png)"}});
  EXPECT_EQ(a.custom("--t: attr(u type(<url>), none)", "--t"), "none");
}

TEST(CssCustom, AttrValuesAreSubstitutedAsCss) {
  // "Substitute arbitrary substitution functions in attr value, with «"attribute", attr name» as the substitution context"
  AttrWorld a({{"w", "var(--v)"}});
  EXPECT_EQ(a.custom("--v: 5px; --t: attr(w type(<length>))", "--t"), "5px");
}

TEST(CssCustom, AttrCyclesThroughTheAttribute) {
  // "preventing any nested functions from cyclicly depending on the foo attribute as well"
  AttrWorld a({{"w", "attr(w type(*))"}});
  EXPECT_EQ(a.custom("--t: attr(w type(*), fb)", "--t"), "fb");
}

TEST(CssCustom, AttrNeedsTheElementTable) {
  World w;
  w.use_iface = false;
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  Computed c;
  Node n;
  compute(w, c, "width: attr(a px)", nullptr, &n);
  EXPECT_EQ(c.status, GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_STREQ(c.result.name, "attribute");
  EXPECT_EQ(c.s, nullptr);
}

TEST(CssCustom, AttrTaintsWhatItMakes) {
  // "Using an attr()-tainted value as or in a <url> makes a declaration invalid at computed-value time."
  AttrWorld a({{"foo", "x.png"}});
  EXPECT_EQ(a.value("background-image: src(attr(foo))", "background-image"), "none");
  // "background-image: image("foo.jpg", attr(bgcolor type(<color>))) is fine"
  AttrWorld b({{"bgcolor", "red"}});
  EXPECT_NE(b.value("background-image: image(\"foo.jpg\", attr(bgcolor type(<color>)))", "background-image"), "none");
  // "--foo: attr(foo); background-image: src(var(--foo))" -- laundering through a custom property is no way round it
  EXPECT_EQ(a.value("--foo: attr(foo); background-image: src(var(--foo))", "background-image"), "none");
}

/* ---- inherit() -------------------------------------------------------------------------------------------------------------- */

TEST(CssCustom, InheritFunctionReadsTheParentsComputedValue) {
  // "Like the inherit keyword, the inherit() functional notation resolves to the computed value of a property on the parent."
  EXPECT_EQ(custom_of("--x: 4px", "--x: 9px; --y: inherit(--x)", "--y"), "4px");
  // "Its second argument, if present, is used as a fallback in case the first argument resolves to the guaranteed-invalid value."
  EXPECT_EQ(custom_of("", "--y: inherit(--x, 2px)", "--y"), "2px");
  EXPECT_EQ(custom_of("", "--y: inherit(--x)", "--y"), "<gi>");
  EXPECT_EQ(custom_of("", "--y: inherit(--x,)", "--y"), "") << "inherit(--foo,) passes an empty fallback";
}

/* ---- ident() ---------------------------------------------------------------------------------------------------------------- */

TEST(CssCustom, IdentBuildsAnIdentifier) {
  Node n;
  n.index = 3;
  World w;
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  Computed c;
  compute(w, c, "--a: ident(\"vtl-\" sibling-index()); --b: ident(-- \"x\"); --c: ident(1 foo); --d: ident(\"\"); --e: ident(2px)", nullptr, &n);
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.custom("--a"), "vtl-3");
  EXPECT_EQ(c.custom("--b"), "--x");
  EXPECT_EQ(c.custom("--c"), "\\31 foo") << "the identifier is written escaped where it must be";
  EXPECT_EQ(c.custom("--d"), "<gi>") << "\"If the result can't be represented as an ident (for example, the empty string) ... guaranteed-invalid\"";
  EXPECT_EQ(c.custom("--e"), "<gi>") << "a dimension is no string, integer or ident";
}

/* ---- sibling-index(), sibling-count() ------------------------------------------------------------------------------------ */

TEST(CssCustom, TreeCountingFunctionsAreNumbersFromTheTree) {
  Node n;
  n.index = 2;
  n.count = 5;
  World w;
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  Computed c;
  compute(w, c, "--i: sibling-index(); --n: sibling-count(); width: calc(var(--i) * 10px); height: calc(sibling-count() * 1px)", nullptr, &n);
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.custom("--i"), "2");
  EXPECT_EQ(c.custom("--n"), "5");
  EXPECT_EQ(c.value(w.registry.r, "width"), "20px");
  EXPECT_EQ(c.value(w.registry.r, "height"), "5px") << "also when no function holds it: a calculation reads it from the tree";
}

TEST(CssCustom, TreeCountingFunctionsNameTheirCallbackWhenTheTreeHasNone) {
  World w;
  w.use_iface = false;
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  Computed c;
  Node n;
  compute(w, c, "--i: sibling-index()", nullptr, &n);
  EXPECT_EQ(c.status, GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_STREQ(c.result.name, "sibling_index");
  Computed d;
  compute(w, d, "width: calc(sibling-count() * 1px)", nullptr, &n);
  EXPECT_EQ(d.status, GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_STREQ(d.result.name, "sibling_count");
}

/* ---- if() ---------------------------------------------------------------------------------------------------------------- */

/// The custom property --r of an element declared `--r: <expr>` (which may hold `;`), in a world whose media() and supports() answers are given.
std::string if_value(const std::string & expr, const std::string & before = "", const std::function<GTEXT_CSS_Tristate(GTEXT_CSS_Condition_Kind, const std::string &)> & cond = nullptr) {
  World w;
  w.condition = cond;
  EXPECT_EQ(w.build(), GTEXT_CSS_OK);
  Computed c;
  std::vector<std::pair<std::string, std::string>> d;
  if (!before.empty()) {
    d.emplace_back("--p", before);
  }
  d.emplace_back("--r", expr);
  compute_pairs(w, c, d);
  EXPECT_EQ(c.status, GTEXT_CSS_OK) << expr << " " << c.result.name << " " << (c.result.detail ? c.result.detail : "");
  return c.custom("--r");
}

TEST(CssCustom, IfPicksTheFirstTrueBranchAndElseIsAlwaysTrue) {
  // "An if() function represents the value corresponding to the first condition in its argument list to be true; if no condition matches, then the if()
  // function represents an empty token stream."
  auto media = [](GTEXT_CSS_Condition_Kind k, const std::string & t) { return k == GTEXT_CSS_CONDITION_MEDIA && t == "width >= 600px" ? GTEXT_CSS_TRI_TRUE : GTEXT_CSS_TRI_FALSE; };
  EXPECT_EQ(if_value("if(media(width >= 600px): blue; else: red)", "", media), "blue");
  EXPECT_EQ(if_value("if(media(width >= 700px): blue; else: red)", "", media), "red");
  EXPECT_EQ(if_value("if(media(width >= 700px): blue)", "", media), "") << "no branch is true: an empty token stream";
  EXPECT_EQ(if_value("if(else: green; media(width >= 600px): blue)", "", media), "green") << "the first true one";
  EXPECT_EQ(if_value("if(media(width >= 600px): blue;)", "", media), "blue") << "\"if( [ <if-branch> ; ]* <if-branch> ;? )\": a trailing semicolon";
}

TEST(CssCustom, IfWithoutAColonInABranchIsInvalid) {
  // "<if-args-branch> = <declaration-value> : <declaration-value>?"
  EXPECT_EQ(if_value("if(else red)"), "<gi>");
}

TEST(CssCustom, IfConditionsAreBooleanExpressions) {
  auto cond = [](GTEXT_CSS_Condition_Kind k, const std::string & t) {
    if (k == GTEXT_CSS_CONDITION_SUPPORTS) {
      return t == "display: grid" ? GTEXT_CSS_TRI_TRUE : GTEXT_CSS_TRI_FALSE;
    }
    return t == "unknown" ? GTEXT_CSS_TRI_UNKNOWN : (t == "yes" ? GTEXT_CSS_TRI_TRUE : GTEXT_CSS_TRI_FALSE);
  };
  EXPECT_EQ(if_value("if(supports(display: grid) and media(yes): a; else: b)", "", cond), "a");
  EXPECT_EQ(if_value("if(supports(display: grid) and media(no): a; else: b)", "", cond), "b");
  EXPECT_EQ(if_value("if(supports(display: grid) or media(no): a; else: b)", "", cond), "a");
  EXPECT_EQ(if_value("if(not media(no): a; else: b)", "", cond), "a");
  EXPECT_EQ(if_value("if(not (media(no) or media(yes)): a; else: b)", "", cond), "b");
  EXPECT_EQ(if_value("if((media(yes) and media(yes)): a; else: b)", "", cond), "a");
  // "If a 'top-level' <boolean-expr[]> is unknown ... it evaluates to false."
  EXPECT_EQ(if_value("if(media(unknown): a; else: b)", "", cond), "b");
  // 3-valued: unknown or true is true, unknown and false is false, not unknown is unknown
  EXPECT_EQ(if_value("if(media(unknown) or media(yes): a; else: b)", "", cond), "a");
  EXPECT_EQ(if_value("if(media(unknown) and media(no): a; else: b)", "", cond), "b");
  EXPECT_EQ(if_value("if(not media(unknown): a; else: b)", "", cond), "b");
  // and and or do not mix without parentheses
  EXPECT_EQ(if_value("if(media(yes) and media(yes) or media(yes): a; else: b)", "", cond), "b");
  // a general-enclosed is unknown
  EXPECT_EQ(if_value("if(foo(bar): a; else: b)", "", cond), "b");
  EXPECT_EQ(if_value("if(not foo(bar): a; else: b)", "", cond), "b");
}

TEST(CssCustom, IfNeedsAConditionCallbackForMediaAndSupports) {
  World w;
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  Computed c;
  compute_pairs(w, c, {{"--r", "if(media(width > 1px): a; else: b)"}});
  EXPECT_EQ(c.status, GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_STREQ(c.result.name, "condition");
  Computed d;
  compute_pairs(w, d, {{"--r", "if(else: b; media(width > 1px): a)"}});
  EXPECT_EQ(d.status, GTEXT_CSS_OK) << "a branch that is not reached is not asked";
  EXPECT_EQ(d.custom("--r"), "b");
}

TEST(CssCustom, IfStyleQueriesReadCustomProperties) {
  // "style( <style-query> )" ... "A <style-feature-plain> evaluates to true if the computed value of the given property on the query container matches the
  // given value"
  EXPECT_EQ(if_value("if(style(--x: 1px): yes; else: no)", "--x: 1px;"), "no") << "the variable is --p here, not --x";
  World w;
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  Computed c;
  compute_pairs(w, c, {{"--x", "1px"}, {"--y", "if(style(--x: 1px): yes; else: no)"}, {"--z", "if(style(--x: 2px): yes; else: no)"}, {"--w", "if(style(--x): yes; else: no)"},
                       {"--v", "if(style(--nope): yes; else: no)"}, {"--u", "if(style(--x: 1px) and style(--x): yes; else: no)"},
                       {"--t", "if(style((--x: 1px) and (--x: 2px)): yes; else: no)"}, {"--s", "if(style(not (--x: 2px)): yes; else: no)"}});
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.custom("--y"), "yes");
  EXPECT_EQ(c.custom("--z"), "no");
  EXPECT_EQ(c.custom("--w"), "yes") << "\"A style feature without a value evaluates to true if the computed value is different from the initial value\"";
  EXPECT_EQ(c.custom("--v"), "no");
  EXPECT_EQ(c.custom("--u"), "yes");
  EXPECT_EQ(c.custom("--t"), "no");
  EXPECT_EQ(c.custom("--s"), "yes");
}

TEST(CssCustom, IfStyleQueryOfThePropertyBeingComputedIsFalse) {
  // "For example, in --foo: if(style(--foo: bar): baz); the style() query is automatically false, since property replacement has already established a
  // «"property", "--foo"» substitution context."
  EXPECT_EQ(custom_of("", "--foo: bar", "--foo"), "bar");
  World w;
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  Computed c;
  compute_pairs(w, c, {{"--foo", "if(style(--foo: bar): baz; else: qux)"}});
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.custom("--foo"), "qux");
}

TEST(CssCustom, IfBranchesThatAreNotChosenAreNotEvaluated) {
  // ".foo { --color: if(media(width >= 600px): blue; else: var(--color)); } Only when the viewport is narrower than 600px does the if() trigger cyclic behavior"
  auto wide = [](GTEXT_CSS_Condition_Kind, const std::string &) { return GTEXT_CSS_TRI_TRUE; };
  auto narrow = [](GTEXT_CSS_Condition_Kind, const std::string &) { return GTEXT_CSS_TRI_FALSE; };
  EXPECT_EQ(if_value("if(media(width >= 600px): blue; else: var(--r))", "", wide), "blue");
  EXPECT_EQ(if_value("if(media(width >= 600px): blue; else: var(--r))", "", narrow), "<gi>");
}

/* ---- the spread syntax -------------------------------------------------------------------------------------------------------- */

TEST(CssCustom, SpreadSubstitutesBeforeTheArgumentsAreDivided) {
  // "to get the desired behavior of 'spreading' the variable into the function's arguments, use ...: color: if(...var(--if-clause); else: green;);"
  auto media = [](GTEXT_CSS_Condition_Kind, const std::string &) { return GTEXT_CSS_TRI_TRUE; };
  EXPECT_EQ(if_value("if(...var(--c); else: green)", "--c: media(width >= 600px): blue", media), "<gi>") << "the variable is --p here";
  World w;
  w.condition = media;
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  Computed c;
  compute_pairs(w, c, {{"--c", "media(width >= 600px): blue"}, {"--r", "if(...var(--c); else: green)"}, {"--q", "if(var(--c); else: green)"}});
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.custom("--r"), "blue");
  // "The if() function entirely fails to parse according to its argument grammar: there's no : character separating the test from the value in the first branch."
  EXPECT_EQ(c.custom("--q"), "<gi>");
}

TEST(CssCustom, SpreadNeedsTheDotsAdjacent) {
  // "... var(--foo) is not [a valid use of the spread syntax], nor . . .var(--foo)."
  World w;
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  Computed c;
  compute_pairs(w, c, {{"--c", "else: blue"}, {"--r", "if(... var(--c); else: green)"}});
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.custom("--r"), "<gi>");
}

/* ---- random-item() and random() ------------------------------------------------------------------------------------------------ */

TEST(CssCustom, RandomItemChoosesAmongItsArguments) {
  World w;
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  Computed c;
  compute_pairs(w, c, {{"--a", "random-item(fixed 0, a, b, c)"}, {"--b", "random-item(fixed 0.5, a, b, c)"}, {"--c", "random-item(fixed 0.99, a, b, c)"},
                       {"--d", "random-item(fixed 0.6, {x, y}, {z, w})"}, {"--e", "random-item(fixed 0, var(--none, q), r)"}, {"--f", "random-item(nonsense nonsense, a, b)"}});
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.custom("--a"), "a");
  EXPECT_EQ(c.custom("--b"), "b") << "\"Let index be a random integer less than N ... round(down, R * limit, 1)\"";
  EXPECT_EQ(c.custom("--c"), "c");
  EXPECT_EQ(c.custom("--d"), "z, w") << "{} keeps the commas inside";
  EXPECT_EQ(c.custom("--e"), "q");
  EXPECT_EQ(c.custom("--f"), "<gi>") << "a key that does not parse is guaranteed-invalid";
}

TEST(CssCustom, RandomBaseValuesComeFromTheEnvironment) {
  // "The random cache name and random base value must be determined ... Two random functions with the same random cache name must also have the same random base value"
  World w;
  w.random_base = [](const GTEXT_CSS_Random_Key * key, double * base) {
    *base = key->name.data && std::string(key->name.data, key->name.length) == "--foo" ? 0.75 : 0.0;
    return true;
  };
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  Computed c;
  compute_pairs(w, c, {{"--a", "random-item(--foo, a, b, c, d)"}, {"--b", "random-item(--bar, a, b, c, d)"}, {"--c", "random-item(--foo element-scoped, a, b, c, d)"}});
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.custom("--a"), "d");
  EXPECT_EQ(c.custom("--b"), "a");
  EXPECT_EQ(c.custom("--c"), "d");
}

TEST(CssCustom, RandomWithoutACallbackIsAFunctionOfTheSeed) {
  auto run = [](uint64_t seed) {
    World w;
    w.env.random_seed = seed;
    EXPECT_EQ(w.build(), GTEXT_CSS_OK);
    Computed c;
    compute_pairs(w, c, {{"--a", "random-item(--k, a, b, c, d, e, f, g, h)"}, {"--b", "random-item(--k, a, b, c, d, e, f, g, h)"},
                         {"--c", "random-item(--other, a, b, c, d, e, f, g, h)"}});
    return c.custom("--a") + c.custom("--b") + c.custom("--c");
  };
  const std::string one = run(1);
  EXPECT_EQ(one, run(1)) << "reproducible";
  EXPECT_EQ(one[0], one[1]) << "the same key, the same value";
  bool differs = false;
  for (uint64_t s = 2; s < 40 && !differs; s++) {
    differs = run(s) != one;
  }
  EXPECT_TRUE(differs) << "and the seed matters";
}

TEST(CssCustom, RandomIsANumberBetweenItsBounds) {
  World w;
  w.random_base = [](const GTEXT_CSS_Random_Key *, double * base) {
    *base = 0.25;
    return true;
  };
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  Computed c;
  compute_pairs(w, c, {{"--a", "random(100px, 300px)"}, {"--b", "random(100px, 300px, 50px)"}, {"--c", "random(--k, 1, 10)"}, {"--d", "random(300px, 100px)"},
                       {"--e", "random(100px, 200px, 30px)"}, {"--f", "random(1em, 2em)"}, {"--g", "random(50px, 180deg)"}});
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.custom("--a"), "150px") << "\"Return min + R * (max - min)\"";
  EXPECT_EQ(c.custom("--b"), "150px") << "\"random(100px, 300px, 50px) can only resolve to 100px, 150px, 200px, 250px, or 300px\"";
  EXPECT_EQ(c.custom("--c"), "3.25");
  EXPECT_EQ(c.custom("--d"), "300px") << "\"If the maximum value is less than the minimum value, it behaves as if it's equal to the minimum value.\"";
  EXPECT_EQ(c.custom("--e"), "130px") << "R = 0.25 of 4 steps (100, 130, 160, 190): floor(0.25 * 4) is the second. \"the max value might not actually show up\"";
  EXPECT_EQ(c.custom("--f"), "1.25em");
  EXPECT_EQ(c.custom("--g"), "calc((50px) + 0.25 * (max((50px), (180deg)) - (50px)))") << "not one type: the calculation is the math layer's to refuse";
}

/* ---- the budget ----------------------------------------------------------------------------------------------------------------- */

/// `levels` levels of `--n_i: var(--n_{i-1}) var(--n_{i-1})` over `--n0: lol`, computed with the options, and the result.
struct Blowup {
  World w;
  Computed c;
  explicit Blowup(int levels, const std::function<void(GTEXT_CSS_Compute_Options &)> & tune = nullptr) {
    if (tune) {
      tune(w.options);
    }
    EXPECT_EQ(w.build(), GTEXT_CSS_OK);
    std::string css = "--n0: lol;";
    for (int i = 1; i <= levels; i++) {
      css += "--n" + std::to_string(i) + ": var(--n" + std::to_string(i - 1) + ") var(--n" + std::to_string(i - 1) + ");";
    }
    css += "content: var(--n" + std::to_string(levels) + ")";
    compute(w, c, css);
  }
};

TEST(CssCustom, TheDoublingShapeStopsAtTheBudgetNotAtTwoToTheN) {
  // "UAs must impose a UA-defined limit on the allowed length of the token stream that an arbitrary substitution function expands into. If an arbitrary
  // substitution function would expand into a longer token stream than this limit, it instead is replaced with the guaranteed-invalid value."
  // Twice: with the defaults, where the bytes of a value (1 MiB) are past the budget before its tokens (2^20) are; and with the bytes out of the way, where the tokens are.
  for (const bool tokens_only : {false, true}) {
    for (int levels : {10, 20, 40}) {
      Blowup b(levels, [tokens_only](GTEXT_CSS_Compute_Options & o) {
        if (tokens_only) {
          o.substitute.max_value_bytes = (size_t)1 << 28;
          o.substitute.max_element_substituted_bytes = (uint64_t)1 << 40;
        }
      });
      ASSERT_EQ(b.c.status, GTEXT_CSS_OK) << levels;
      EXPECT_LE(b.c.result.work, 12000u + (tokens_only ? (size_t)(1u << 24) / 16 : (size_t)(8u << 20) / 16))
          << "the work is the text that was written (and, at 10 levels, the 2047 nodes of the value that is written out) and a sixteenth of a step for each byte a value "
             "holds, not 2^" << levels;
      EXPECT_LE(b.c.result.substitution_peak, 2u * 1048576u) << "the counter stops at the budget";
      if (levels == 10) {
        EXPECT_EQ(b.c.result.substitution_limited, 0u);
        EXPECT_EQ(b.c.custom("--n10").size(), 1024u * 3 + 1023u) << "1024 words and the spaces between them";
      }
      else if (levels == 20) {
        EXPECT_EQ(b.c.result.substitution_limited, 1u);
        EXPECT_EQ(b.c.custom("--n20"), "<gi>");
        if (tokens_only) {
          EXPECT_NE(b.c.custom("--n19"), "<gi>") << "2^20 - 1 tokens";
        }
        else {
          EXPECT_NE(b.c.custom("--n18"), "<gi>") << "2^18 words of three bytes and the spaces: 1 MiB less one";
          EXPECT_EQ(b.c.custom("--n19"), "<gi>") << "past 1 MiB";
        }
      }
      else {
        EXPECT_EQ(b.c.custom("--n40"), "<gi>") << "all dependents are guaranteed-invalid";
        EXPECT_EQ(b.c.custom("--n21"), "<gi>");
        EXPECT_EQ(b.c.value(b.w.registry.r, "content"), "normal");
      }
    }
  }
}

/* ---- cycle() ---------------------------------------------------------------------------------------------------------------- */

TEST(CssCustom, CycleTakesTheNextValueAfterTheInheritedOne) {
  // "em { font-style: cycle(italic, normal); } The following example makes <em> elements italic in general, but makes them normal if they're inside something that's
  // italic"
  EXPECT_EQ(prop_of("", "font-style: cycle(italic, normal)", "font-style"), "italic") << "no match: the first";
  EXPECT_EQ(prop_of("font-style: italic", "font-style: cycle(italic, normal)", "font-style"), "normal");
  EXPECT_EQ(prop_of("font-style: normal", "font-style: cycle(italic, normal)", "font-style"), "italic") << "\"If the match was the last argument ... the first\"";
  // "For example cycle(1em, 2em, 1em, 4em) will be equivalent to cycle(1em, 2em)." 
  EXPECT_EQ(prop_of("font-size: 16px", "font-size: cycle(1em, 2em, 1em, 4em)", "font-size"), "32px") << "16px is 1em of itself... the parent's size is the basis";
  EXPECT_EQ(prop_of("list-style-type: disc", "list-style-type: cycle(disc, circle, square, decimal)", "list-style-type"), "circle");
  EXPECT_EQ(prop_of("list-style-type: square", "list-style-type: cycle(disc, circle, square, decimal)", "list-style-type"), "decimal");
  EXPECT_EQ(prop_of("list-style-type: decimal", "list-style-type: cycle(disc, circle, square, decimal)", "list-style-type"), "disc");
}

TEST(CssCustom, CycleWorksOnAPropertyThatDoesNotInherit) {
  // "That cycle() explicitly looks at the computed value of the parent, so it works even on non-inherited properties."
  EXPECT_EQ(prop_of("background-repeat: no-repeat", "background-repeat: cycle(repeat-x, no-repeat, repeat-y)", "background-repeat"), "no-repeat repeat")
      << "repeat-y, written as the pair it computes to: the parent's value is read, whatever the property inherits";
}

TEST(CssCustom, CycleArgumentsAreWholeValuesAndMayHoldVariables) {
  EXPECT_EQ(prop_of("margin-top: 1px", "--a: 1px; --b: 5px; margin-top: cycle(var(--a), var(--b))", "margin-top"), "5px");
  EXPECT_EQ(prop_of("", "margin-top: cycle(5px, 6px)", "margin-top"), "5px");
}

TEST(CssCustom, CycleOnAShorthandCyclesEachLonghand) {
  // "margin: cycle(1px 2px, 4px, 1px 5px 4px); is equivalent to the following longhand declarations: margin-top: cycle(1px, 4px, 1px); margin-right: cycle(2px, 4px, 5px);
  // margin-bottom: cycle(1px, 4px, 4px); margin-left: cycle(2px, 4px, 5px);"
  const std::string d = "margin: cycle(1px 2px, 4px, 1px 5px 4px)";
  EXPECT_EQ(prop_of("", d, "margin-top"), "1px");
  EXPECT_EQ(prop_of("", d, "margin-right"), "2px");
  EXPECT_EQ(prop_of("margin-top: 1px; margin-right: 2px; margin-bottom: 4px; margin-left: 4px", d, "margin-top"), "4px");
  EXPECT_EQ(prop_of("margin-top: 1px; margin-right: 2px; margin-bottom: 4px; margin-left: 4px", d, "margin-right"), "4px") << "cycle(2px, 4px, 5px) at 2px";
  EXPECT_EQ(prop_of("margin-top: 1px; margin-right: 2px; margin-bottom: 4px; margin-left: 4px", d, "margin-bottom"), "4px") << "cycle(1px, 4px, 4px) at 4px: the earliest match is the second, so the third";
}

TEST(CssCustom, CycleMustBeTheWholeValueAndHoldNoAttrCalcOrCycle) {
  // "background-position: 10px cycle(50px, 100px); /* cycle() must be the sole value of the property */"
  EXPECT_EQ(prop_of("", "background-position-x: 10px cycle(50px, 100px)", "background-position-x"), "start 0%");
  // "it is not allowed to be nested, nor may it contain attr() or calc() notations; declarations containing such constructs are invalid."
  EXPECT_EQ(prop_of("", "margin-top: cycle(calc(1px + 1px), 2px)", "margin-top"), "0px");
  EXPECT_EQ(prop_of("", "margin-top: cycle(cycle(1px, 2px), 3px)", "margin-top"), "0px");
  // "list-style-type: cycle(disc, 50px); /* 50px isn't a valid value of list-style-type */"
  EXPECT_EQ(prop_of("list-style-type: square", "list-style-type: cycle(disc, 50px)", "list-style-type"), "square");
}

/* ---- random() in a value that holds nothing else to replace ------------------------------------------------------------ */

TEST(CssCustom, RandomInACalculationIsTheNumberItsBaseValuePicks) {
  World w;
  w.random_base = [](const GTEXT_CSS_Random_Key *, double * base) {
    *base = 0.5;
    return true;
  };
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  Computed c;
  compute(w, c, "width: random(100px, 300px); height: calc(random(1px, 3px) + 1px); opacity: random(0.2, 0.6)");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.value(w.registry.r, "width"), "200px");
  EXPECT_EQ(c.value(w.registry.r, "height"), "3px");
  EXPECT_EQ(c.value(w.registry.r, "opacity"), "0.4");
}

/* ---- the block API and the cascade carry a pending custom value --------------------------------------------------------- */

TEST(CssCustom, TheBlockAndTheCascadeAgreeOnAPendingCustomValue) {
  // The carry-over of C2b: `--a: var(--b)` was refused by both. Now both take it, hand it on as PENDING, and the computed styles agree.
  World w;
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  const std::string decls = "--a: var(--b) 2px; --b: 3px; width: var(--a, 9px)";
  Computed viaBlock;
  compute(w, viaBlock, decls);
  ASSERT_EQ(viaBlock.status, GTEXT_CSS_OK);
  EXPECT_EQ(viaBlock.custom("--a"), "3px 2px");

  // the block API: a PENDING item
  GTEXT_CSS_Property_Value pv;
  std::memset(&pv, 0, sizeof pv);
  ASSERT_EQ(gtext_css_parse_property_text(w.registry.r, "--a", 3, "var(--b)", 8, nullptr, &pv), GTEXT_CSS_OK);
  ASSERT_EQ(pv.outcome, GTEXT_CSS_PROP_RESULT_PENDING);
  GTEXT_CSS_Declaration_Block * blk = nullptr;
  ASSERT_EQ(gtext_css_block_new(w.registry.r, nullptr, &blk), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_block_add_custom(blk, "--a", 3, &pv, false), GTEXT_CSS_OK);
  GTEXT_CSS_Declaration_Item item;
  ASSERT_TRUE(gtext_css_block_item(blk, 0, &item));
  EXPECT_EQ(item.outcome, GTEXT_CSS_PROP_RESULT_PENDING);
  gtext_css_property_value_free(&pv);
  gtext_css_block_free(blk);

  // the cascade: the same declarations, handed on to a block, compute the same
  GTEXT_CSS_Candidates * set = nullptr;
  ASSERT_EQ(gtext_css_candidates_new(w.registry.r, nullptr, nullptr, &set), GTEXT_CSS_OK);
  const std::pair<const char *, const char *> list[] = {{"--a", "var(--b) 2px"}, {"--b", "3px"}, {"width", "var(--a, 9px)"}};
  uint32_t order = 0;
  for (const auto & d : list) {
    GTEXT_CSS_Cascade_Entry k;
    std::memset(&k, 0, sizeof k);
    k.origin = GTEXT_CSS_ORIGIN_AUTHOR;
    k.rule = order++;
    GTEXT_CSS_Property_Outcome o = GTEXT_CSS_PROP_RESULT_UNKNOWN;
    EXPECT_EQ(gtext_css_candidates_add_text(set, &k, d.first, std::strlen(d.first), d.second, std::strlen(d.second), nullptr, &o, nullptr), GTEXT_CSS_OK) << d.first;
  }
  GTEXT_CSS_Cascaded * cas = nullptr;
  ASSERT_EQ(gtext_css_cascade(set, nullptr, &cas, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Cascade_Entry e;
  ASSERT_TRUE(gtext_css_cascaded_find(cas, 0, "--a", 3, &e));
  EXPECT_EQ(e.outcome, GTEXT_CSS_PROP_RESULT_PENDING);
  GTEXT_CSS_Declaration_Block * fromCascade = nullptr;
  ASSERT_EQ(gtext_css_cascaded_to_block(cas, nullptr, &fromCascade), GTEXT_CSS_OK);
  GTEXT_CSS_Computed_Style * s = nullptr;
  ASSERT_EQ(gtext_css_compute_style(w.ctx, fromCascade, nullptr, nullptr, &s, nullptr), GTEXT_CSS_OK);
  Computed viaCascade;
  viaCascade.s = s;
  EXPECT_EQ(viaCascade.custom("--a"), viaBlock.custom("--a"));
  EXPECT_EQ(viaCascade.value(w.registry.r, "width"), viaBlock.value(w.registry.r, "width"));
  EXPECT_EQ(viaCascade.value(w.registry.r, "width"), "auto") << "width does not accept a pair: invalid at computed-value time, so unset";
  gtext_css_block_free(fromCascade);
  gtext_css_cascaded_free(cas);
  gtext_css_candidates_free(set);
}

/* ---- the limits, each its own status ------------------------------------------------------------------------------------ */

TEST(CssCustom, TooManyCustomPropertiesIsItsOwnStatus) {
  World w;
  w.options.substitute.max_custom_properties = 4;
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  Computed ok, over;
  compute(w, ok, "--a:1; --b:2; --c:3; --d:4");
  EXPECT_EQ(ok.status, GTEXT_CSS_OK);
  compute(w, over, "--a:1; --b:2; --c:3; --d:4; --e:5");
  EXPECT_EQ(over.status, GTEXT_CSS_E_LIMIT_CUSTOM_PROPERTIES);
  EXPECT_EQ(over.s, nullptr);
}

TEST(CssCustom, ADeclaredValueLongerThanTheLimitIsItsOwnStatus) {
  World w;
  w.options.substitute.max_value_bytes = 64;
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  Computed ok, over;
  compute(w, ok, "--a:" + std::string(60, 'x'));
  EXPECT_EQ(ok.status, GTEXT_CSS_OK);
  compute(w, over, "--a:" + std::string(100, 'x'));
  EXPECT_EQ(over.status, GTEXT_CSS_E_LIMIT_CUSTOM_VALUE);
}

TEST(CssCustom, ASubstitutedValueLongerThanTheLimitIsGuaranteedInvalidNotAnError) {
  // "it instead is replaced with the guaranteed-invalid value": the byte limit is the same kind of limit as the token budget.
  World w;
  w.options.substitute.max_value_bytes = 64;
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  Computed c;
  const std::string x(40, 'x');
  compute(w, c, "--a:" + x + "; --b: var(--a) var(--a)");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.custom("--b"), "<gi>");
  EXPECT_EQ(c.result.substitution_limited, 1u);
  EXPECT_EQ(c.custom("--a"), x);
}

TEST(CssCustom, ADeepChainPastTheStackIsTheGuaranteedInvalidValueAndNotAFailureOfTheElement) {
  World w;
  w.options.substitute.max_substitution_depth = 200;
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  auto chain = [](int n) {
    std::string css = "--n0: 1px;";
    for (int i = 1; i <= n; i++) {
      css += "--n" + std::to_string(i) + ": var(--n" + std::to_string(i - 1) + ");";
    }
    return css + "width: var(--n" + std::to_string(n) + "); height: 7px";
  };
  Computed ok, over;
  compute(w, ok, chain(20));
  ASSERT_EQ(ok.status, GTEXT_CSS_OK);
  EXPECT_EQ(ok.value(w.registry.r, "width"), "1px");
  EXPECT_EQ(ok.result.substitution_limited, 0u);
  compute(w, over, chain(500));
  ASSERT_EQ(over.status, GTEXT_CSS_OK) << "the element is computed";
  EXPECT_EQ(over.value(w.registry.r, "width"), "auto") << "the property that reaches past the stack is the guaranteed-invalid value, and so is what is made of it";
  EXPECT_EQ(over.value(w.registry.r, "height"), "7px");
  EXPECT_EQ(over.custom("--n500"), "<gi>");
  EXPECT_GE(over.result.substitution_limited, 1u);
}

TEST(CssCustom, ADeepChainAtTheDefaultLimitsRunsOnAFlatStack) {
  World w;
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  size_t work[2] = {0, 0};
  const int sizes[2] = {10000, 20000};
  for (int k = 0; k < 2; k++) {
    std::string css = "--n0: 1px;";
    for (int i = 1; i <= sizes[k]; i++) {
      css += "--n" + std::to_string(i) + ": var(--n" + std::to_string(i - 1) + ");";
    }
    css += "width: var(--n" + std::to_string(sizes[k]) + ")";
    Computed c;
    compute(w, c, css);
    ASSERT_EQ(c.status, GTEXT_CSS_OK);
    EXPECT_EQ(c.value(w.registry.r, "width"), "1px");
    work[k] = c.result.work;
  }
  EXPECT_LE(work[1], work[0] * 23 / 10) << "twice the chain, twice the work (and the treap's log): linear, not four times";
}

TEST(CssCustom, TheElementBudgetIsSharedByAllTheValuesOfOneElement) {
  // Each value is within its own budget; together they are not.
  World w;
  w.options.substitute.max_substituted_tokens = 1000;
  w.options.substitute.max_element_substituted_tokens = 2500;
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  std::string base = "--w:";
  for (int i = 0; i < 200; i++) {
    base += " w";
  }
  std::string css = base + ";";
  for (int i = 0; i < 8; i++) {
    css += "--c" + std::to_string(i) + ": var(--w) var(--w);";
  }
  Computed c;
  compute(w, c, css);
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_GE(c.result.substitution_limited, 1u);
  EXPECT_NE(c.custom("--c0"), "<gi>");
  EXPECT_EQ(c.custom("--c7"), "<gi>") << "the element's budget was used up by the ones before it";
  EXPECT_GT(c.result.substituted_tokens, 2500u) << "the counter is what the values would have been, summed; the budget is what refused them";
}

TEST(CssCustom, ComputedStylesAreImmutableAndShareableAcrossThreadsOfRead) {
  World w;
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  Computed c;
  compute(w, c, "--a: 1px; --b: var(--a) 2px; width: var(--a)");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  const std::string before = c.custom("--b");
  Computed child;
  compute(w, child, "--a: 9px", &c);
  EXPECT_EQ(c.custom("--b"), before) << "a child computed from it does not change it";
  EXPECT_EQ(child.custom("--b"), "1px 2px") << "the computed value is what is inherited, not the declaration it came from";
  EXPECT_EQ(child.custom("--a"), "9px");
}

} // namespace
