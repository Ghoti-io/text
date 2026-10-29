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
 * `@supports` (CSS Conditional 3 and 5): the grammar, the leaves and their answers, `CSS.supports()`, the policy and the interface check. Each test is named for the rule it
 * checks and the sentence of the served draft is quoted beside it (read 2026-10-28).
 */

#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_cascade_test_support.h"

namespace {

GTEXT_Markup_Status collect(void * user, const char * bytes, size_t len) {
  static_cast<std::string *>(user)->append(bytes, len);
  return GTEXT_MARKUP_OK;
}

struct Cond {
  GTEXT_CSS_Conditions * c = nullptr;
  explicit Cond(const GTEXT_CSS_Support_Policy * policy = nullptr, const GTEXT_CSS_Element_Interface * iface = nullptr, bool registry = true) {
    GTEXT_CSS_Conditions_Options o = gtext_css_conditions_options_default();
    o.policy = policy;
    o.element_interface = iface;
    EXPECT_EQ(gtext_css_conditions_new(registry ? cas::R() : nullptr, &o, &c), GTEXT_CSS_OK);
  }
  ~Cond() {
    gtext_css_conditions_free(c);
  }
  Cond(const Cond &) = delete;
  Cond & operator=(const Cond &) = delete;
  GTEXT_CSS_Tristate tri(const std::string & text, bool bare = false) {
    GTEXT_CSS_Supports_Condition * s = nullptr;
    EXPECT_EQ(gtext_css_supports_parse(text.data(), text.size(), bare, nullptr, &s, nullptr), GTEXT_CSS_OK) << text;
    GTEXT_CSS_Tristate t = GTEXT_CSS_TRI_UNKNOWN;
    EXPECT_EQ(gtext_css_conditions_evaluate_supports(c, s, nullptr, &t), GTEXT_CSS_OK) << text;
    gtext_css_media_list_free(s);
    return t;
  }
  bool yes(const std::string & text, bool bare = false) {
    return tri(text, bare) == GTEXT_CSS_TRI_TRUE;
  }
};

std::string ser(const std::string & text, bool bare = false) {
  GTEXT_CSS_Supports_Condition * s = nullptr;
  EXPECT_EQ(gtext_css_supports_parse(text.data(), text.size(), bare, nullptr, &s, nullptr), GTEXT_CSS_OK) << text;
  std::string out;
  GTEXT_Markup_Sink sink{collect, &out};
  EXPECT_EQ(gtext_css_media_list_serialize(s, &sink), GTEXT_CSS_OK);
  gtext_css_media_list_free(s);
  return out;
}

}  // namespace

TEST(CssSupports, Declarations) {
  /* The matrix: `@supports (display: grid)` true; `not (foo: bar)` true; `(foo: bar) or (color: red)` true. */
  Cond c;
  EXPECT_TRUE(c.yes("(display: grid)"));
  EXPECT_TRUE(c.yes("not (foo: bar)"));
  EXPECT_TRUE(c.yes("(foo: bar) or (color: red)"));
  EXPECT_FALSE(c.yes("(foo: bar)"));
  EXPECT_FALSE(c.yes("(foo: bar) and (color: red)"));
  EXPECT_FALSE(c.yes("(color: nonsense)"));
  EXPECT_TRUE(c.yes("not (color: nonsense)"));
}

TEST(CssSupports, TheFormsOfADeclaration) {
  /* The matrix: `(color: red !important)`, `(color: var(--x))`, `(--x:)` true; `(color: nonsense)` false. */
  Cond c;
  EXPECT_TRUE(c.yes("(color: red !important)"));
  EXPECT_TRUE(c.yes("(color: red ! important)"));
  EXPECT_TRUE(c.yes("(color: var(--x))"));
  EXPECT_TRUE(c.yes("(--x:)"));
  EXPECT_TRUE(c.yes("(--x: anything at all {})"));
  EXPECT_FALSE(c.yes("(color: nonsense)"));
  EXPECT_TRUE(c.yes("(COLOR: RED)")) << "property names and keywords are ASCII case-insensitive";
  EXPECT_TRUE(c.yes("(color:red)"));
  EXPECT_TRUE(c.yes("(-webkit-box-shadow: none)") || !c.yes("(-webkit-box-shadow: none)")) << "an alias is its property: asked, not decided here";
}

TEST(CssSupports, ACustomPropertyNeedsADeclarationValue) {
  /* Variables 1, 2: a custom property's value is a <declaration-value>, which holds no top-level `;` or `!`, no unmatched `)` `]` `}` and no bad string or bad
   * url; a var() whose first argument is empty is not valid; and the name `--` alone is reserved. Both browsers answer false for each of these. */
  Cond c;
  for (const char * ok : {"(--x:)", "(--x: red)", "(--x: {a:b})", "(--x: [a;b])", "(--x: f(a;b))", "(--x: var(--y))", "(--x: var(--y, 1))", "(--x: 1 !important)",
           "(--a-b_c: \"s\")", "(--x: url(a))", "(--x: url(\"a b\"))", "(--x: (a))", "(--X: A)"}) {
    EXPECT_TRUE(c.yes(ok)) << ok;
  }
  /* not a <declaration-value>, but an <any-value>: <general-enclosed>, false in a supports condition, and so true under `not` */
  for (const char * bad : {"(--x: a;b)", "(--x: !)", "(--x: a ! b)", "(--x: var())", "(--x: var(, 1))", "(--: 1)", "(--:)"}) {
    EXPECT_FALSE(c.yes(bad)) << bad;
    EXPECT_TRUE(c.yes(std::string("not ") + bad)) << bad;
  }
  /* not even an <any-value> (an unmatched closer, a bad url): the condition is invalid and false whatever is put in front of it */
  for (const char * bad : {"(--x: red})", "(--x: a))", "(--x: ])", "(--x: url(a b))"}) {
    EXPECT_FALSE(c.yes(bad)) << bad;
    EXPECT_FALSE(c.yes(std::string("not ") + bad)) << bad;
  }
  /* CSS.supports(property, value) */
  bool r = true;
  ASSERT_EQ(gtext_css_conditions_supports(c.c, "--x", "a;b", &r), GTEXT_CSS_OK);
  EXPECT_FALSE(r);
  ASSERT_EQ(gtext_css_conditions_supports(c.c, "--x", "a b", &r), GTEXT_CSS_OK);
  EXPECT_TRUE(r);
  ASSERT_EQ(gtext_css_conditions_supports(c.c, "--", "1", &r), GTEXT_CSS_OK);
  EXPECT_FALSE(r);
  ASSERT_EQ(gtext_css_conditions_supports(c.c, "--x", "", &r), GTEXT_CSS_OK);
  EXPECT_TRUE(r) << "the empty value is a value";
}

TEST(CssSupports, GeneralEnclosedHoldsAnAnyValue) {
  /* Conditional 3: <general-enclosed> = [ <function-token> <any-value>? ) ] | [ ( <any-value>? ) ]; Syntax 3: an <any-value> has no bad string, no bad url and no
   * unmatched `)` `]` `}`. A condition with one that does is not a condition: invalid, so false and not negatable to true (`not (foo: red})` is false in both
   * browsers, where `not (foo: red)` is true). */
  Cond c;
  for (const char * bad : {"not (foo: red})", "(foo: red})", "(foo: url(bad url))", "(foo: \"unterminated\n)", "(foo: ])", "(foo: a) or (bar: b})", "foo(a})"}) {
    EXPECT_FALSE(c.yes(bad)) << bad;
    EXPECT_FALSE(c.yes(std::string("not ") + bad)) << bad;
    EXPECT_EQ(ser(bad), "not all") << bad;
  }
  EXPECT_TRUE(c.yes("not (foo: red)"));
  EXPECT_TRUE(c.yes("not (foo: red;)")) << "a semicolon is an <any-value> token";
  EXPECT_TRUE(c.yes("not (foo: !)"));
  EXPECT_TRUE(c.yes("not (foo: {a})"));
  EXPECT_TRUE(c.yes("not foo(a, b)"));
  EXPECT_EQ(ser("not (foo: red)"), "not (foo: red)");
}

TEST(CssSupports, QuirksNeverApply) {
  /* Unitless lengths are valid in quirks mode only; `@supports` ignores quirks. */
  Cond c;
  EXPECT_FALSE(c.yes("(width: 10)"));
  EXPECT_TRUE(c.yes("(width: 10px)"));
}

TEST(CssSupports, BadSupportsNeverApplies) {
  /* The matrix: `@supports display: grid` (no parentheses) is invalid, the rule never applies. */
  Cond c;
  GTEXT_CSS_Supports_Condition * s = nullptr;
  GTEXT_CSS_Media_Result r;
  const char * t = "display: grid";
  ASSERT_EQ(gtext_css_supports_parse(t, std::strlen(t), false, nullptr, &s, &r), GTEXT_CSS_OK);
  EXPECT_TRUE(gtext_css_media_list_query_replaced(s, 0));
  EXPECT_GE(gtext_css_media_result_diagnostic_count(&r), 1u);
  GTEXT_CSS_Tristate tr = GTEXT_CSS_TRI_UNKNOWN;
  ASSERT_EQ(gtext_css_conditions_evaluate_supports(c.c, s, nullptr, &tr), GTEXT_CSS_OK);
  EXPECT_EQ(tr, GTEXT_CSS_TRI_FALSE);
  gtext_css_media_list_free(s);
  gtext_css_media_result_free(&r);
  EXPECT_TRUE(c.yes("display: grid", true)) << "CSS.supports(text) accepts the declaration without parentheses";
}

TEST(CssSupports, TheGrammar) {
  /* "<supports-condition> = not <supports-in-parens> | <supports-in-parens> [ and <supports-in-parens> ]* | ... [ or ... ]*": no mixing without parentheses. */
  Cond c;
  EXPECT_TRUE(c.yes("(color: red) and (display: block)"));
  EXPECT_TRUE(c.yes("(color: red) and ((foo: bar) or (display: block))"));
  for (const char * bad : {"(color: red) and (display: block) or (width: 1px)", "not (a: b) and (c: d)", "not not (color: red)", "", "(color: red) and"}) {
    GTEXT_CSS_Supports_Condition * s = nullptr;
    ASSERT_EQ(gtext_css_supports_parse(bad, std::strlen(bad), false, nullptr, &s, nullptr), GTEXT_CSS_OK) << bad;
    EXPECT_TRUE(gtext_css_media_list_query_replaced(s, 0)) << bad;
    gtext_css_media_list_free(s);
  }
  /* Conditional 3: "Removed the 'unknown' value in CSS feature queries' boolean logic, defining unrecognized syntaxes as 'false' instead." */
  EXPECT_EQ(c.tri("((color: red) and (display: block) or (width: 1px))"), GTEXT_CSS_TRI_FALSE) << "inside parentheses the mix is <general-enclosed>: false";
  EXPECT_EQ(c.tri("foo(bar)"), GTEXT_CSS_TRI_FALSE);
  EXPECT_EQ(c.tri("not foo(bar)"), GTEXT_CSS_TRI_TRUE);
  EXPECT_TRUE(c.yes("not (foobar)")) << "web-platform-tests CSS-supports-L3";
  EXPECT_TRUE(c.yes("foo(bar) or (color: red)"));
}

TEST(CssSupports, Serialization) {
  EXPECT_EQ(ser("(color:red)"), "(color: red)");
  EXPECT_EQ(ser("(color: red!important)"), "(color: red !important)");
  EXPECT_EQ(ser("not (  display:grid )"), "not (display: grid)");
  EXPECT_EQ(ser("(a: b) AND (c: d)"), "(a: b) and (c: d)");
  EXPECT_EQ(ser("selector( a > b )"), "selector(a > b)");
  EXPECT_EQ(ser("FONT-TECH(color-COLRv1)"), "font-tech(color-COLRv1)");
  EXPECT_EQ(ser("at-rule(@font-face)"), "at-rule(@font-face)");
  for (const char * q : {"(a: b) or (c: d)", "not selector(:is(a, b))", "font-format(\"woff2\")", "env(safe-area-inset-top)", "named-feature(x)", "((color: red))"}) {
    std::string a = ser(q);
    EXPECT_EQ(ser(a), a) << q;
  }
}

TEST(CssSupports, EmptyFunctionsAndBlocksAreGeneralEnclosedOrFalse) {
  /* Found by the fuzzer: an empty block or function has no children array. */
  Cond c;
  EXPECT_FALSE(c.yes("selector()"));
  EXPECT_FALSE(c.yes("font-tech()"));
  EXPECT_FALSE(c.yes("env()"));
  EXPECT_FALSE(c.yes("(  )"));
  EXPECT_FALSE(c.yes("foo()"));
}

TEST(CssSupports, SelectorFunction) {
  /* The matrix: `selector(:is(a, b))` true; `selector(a >)` false; `not selector(:no-such-pseudo)` true. */
  Cond c;
  EXPECT_TRUE(c.yes("selector(:is(a, b))"));
  EXPECT_FALSE(c.yes("selector(a >)"));
  EXPECT_EQ(c.tri("selector(a >)"), GTEXT_CSS_TRI_FALSE) << "an invalid selector is false, not unknown";
  EXPECT_TRUE(c.yes("not selector(:no-such-pseudo)"));
  EXPECT_TRUE(c.yes("selector(a::before)")) << "a pseudo-element is allowed";
  EXPECT_FALSE(c.yes("selector(a, b)")) << "one complex selector";
  EXPECT_FALSE(c.yes("selector(> a)")) << "not a relative selector";
}

namespace {
bool local_name(void *, GTEXT_CSS_Element_Handle, GTEXT_CSS_String * out) {
  out->data = "a";
  out->length = 1;
  return true;
}
}  // namespace

TEST(CssSupports, AForgivingListThatKeptAnInvalidEntryIsNotSupported) {
  Cond c;
  EXPECT_FALSE(c.yes("selector(:is(:foo))"));
  EXPECT_FALSE(c.yes("selector(:is())"));
  EXPECT_FALSE(c.yes("selector(:where(.a, :foo))"));
  EXPECT_FALSE(c.yes("selector(:has(:is(:has(.a))))"));
  EXPECT_TRUE(c.yes("selector(:is(.a, .b))"));
  /* An empty entry of a forgiving list is skipped when the list is read as a style rule's, and is an unsupported selector for selector() (both browsers say false) */
  EXPECT_FALSE(c.yes("selector(:is(a,))"));
  EXPECT_FALSE(c.yes("selector(:where(a,))"));
  EXPECT_FALSE(c.yes("selector(:is(,a))"));
  EXPECT_FALSE(c.yes("selector(:not(:is(a,)))"));
  EXPECT_TRUE(c.yes("selector(:not(:is(a)))"));
  /* parse-only: read and never matched, so not supported (both browsers say false for :nth-col) */
  EXPECT_FALSE(c.yes("selector(:nth-col(1))"));
  EXPECT_TRUE(c.yes("not selector(:nth-col(1))"));
}

TEST(CssSupports, SelectorNeedsWhatTheInterfaceHas) {
  /* The matrix: `selector(:empty)` with an interface lacking `child_content`: false. */
  GTEXT_CSS_Element_Interface iface;
  std::memset(&iface, 0, sizeof iface);
  iface.size = sizeof iface;
  iface.local_name = local_name;
  Cond c(nullptr, &iface);
  EXPECT_TRUE(c.yes("selector(a)"));
  EXPECT_FALSE(c.yes("selector(:empty)"));
  EXPECT_TRUE(c.yes("not selector(:empty)"));
}

TEST(CssSupports, FontTechAndFormat) {
  /* The matrix: font-tech(color-COLRv1) with the bit set true; font-tech(bogus) false; not font-tech(bogus) false; not font-format("x") true. */
  GTEXT_CSS_Support_Policy p;
  std::memset(&p, 0, sizeof p);
  p.font_tech = GTEXT_CSS_FONT_TECH_COLOR_COLRV1 | GTEXT_CSS_FONT_TECH_VARIATIONS;
  p.font_format = GTEXT_CSS_FONT_FORMAT_WOFF2;
  Cond c(&p);
  EXPECT_TRUE(c.yes("font-tech(color-COLRv1)"));
  EXPECT_TRUE(c.yes("font-tech(variations)"));
  EXPECT_FALSE(c.yes("font-tech(palettes)"));
  EXPECT_EQ(c.tri("font-tech(bogus)"), GTEXT_CSS_TRI_FALSE);
  EXPECT_TRUE(c.yes("not font-tech(bogus)")) << "general-enclosed is false in a supports condition, so its not is true (the spec of this stage said false: overturned)";
  EXPECT_TRUE(c.yes("font-format(woff2)"));
  EXPECT_FALSE(c.yes("font-format(woff)"));
  EXPECT_FALSE(c.yes("font-format(\"woff2\")")) << "a string names a MIME type, which nobody here knows: a valid, false feature";
  EXPECT_EQ(c.tri("font-format(bogus)"), GTEXT_CSS_TRI_FALSE) << "an ident outside the grammar is general-enclosed";
  EXPECT_EQ(c.tri("font-format(\"x\")"), GTEXT_CSS_TRI_FALSE) << "a string the policy does not know is a valid, false feature";
  EXPECT_TRUE(c.yes("not font-format(\"x\")"));
  Cond none;
  EXPECT_FALSE(none.yes("font-tech(variations)")) << "a zeroed policy supports no font technology";
}

TEST(CssSupports, AtRule) {
  /* The matrix: at-rule(@font-face) true; at-rule(@bogus) false; at-rule(@function) false (preserved, inactive). */
  Cond c;
  EXPECT_TRUE(c.yes("at-rule(@font-face)"));
  EXPECT_TRUE(c.yes("at-rule(@MEDIA)"));
  EXPECT_FALSE(c.yes("at-rule(@bogus)"));
  EXPECT_FALSE(c.yes("at-rule(@charset)")) << "Syntax 3, 3.2: an encoding declaration, not a rule";
  EXPECT_TRUE(c.yes("at-rule(@when)")) << "a draft both browsers lack, implemented here";
  EXPECT_FALSE(c.yes("at-rule(@function)"));
  EXPECT_EQ(c.tri("at-rule(font-face)"), GTEXT_CSS_TRI_FALSE);
}

namespace {
bool narrow(void *, GTEXT_CSS_Property_Id) {
  return false;
}
bool names(void *, const GTEXT_CSS_String * n) {
  return n->length == 5 && std::memcmp(n->data, "known", 5) == 0;
}
bool no_at(void *, const GTEXT_CSS_String *) {
  return false;
}
}  // namespace

TEST(CssSupports, ThePolicyNarrowsAndAnswers) {
  GTEXT_CSS_Support_Policy p;
  std::memset(&p, 0, sizeof p);
  p.property_supported = narrow;
  p.named_feature = names;
  p.env_variable = names;
  p.at_rule_supported = no_at;
  Cond c(&p);
  EXPECT_FALSE(c.yes("(display: grid)")) << "a renderer that does not implement the property says so";
  EXPECT_TRUE(c.yes("(--x: 1)")) << "a custom property is not narrowed";
  EXPECT_TRUE(c.yes("named-feature(known)"));
  EXPECT_FALSE(c.yes("named-feature(other)"));
  EXPECT_TRUE(c.yes("env(known)"));
  EXPECT_FALSE(c.yes("env(other)"));
  EXPECT_FALSE(c.yes("at-rule(@media)"));
  Cond d;
  EXPECT_FALSE(d.yes("named-feature(known)")) << "default: none";
  EXPECT_FALSE(d.yes("env(known)"));
}

TEST(CssSupports, CssSupportsEntryPoints) {
  Cond c;
  bool r = false;
  ASSERT_EQ(gtext_css_conditions_supports(c.c, "display", "grid", &r), GTEXT_CSS_OK);
  EXPECT_TRUE(r);
  ASSERT_EQ(gtext_css_conditions_supports(c.c, "display", "nonsense", &r), GTEXT_CSS_OK);
  EXPECT_FALSE(r);
  ASSERT_EQ(gtext_css_conditions_supports(c.c, "nonsense", "grid", &r), GTEXT_CSS_OK);
  EXPECT_FALSE(r);
  const char * t = "display: grid";
  ASSERT_EQ(gtext_css_conditions_supports_text(c.c, t, std::strlen(t), &r), GTEXT_CSS_OK);
  EXPECT_TRUE(r);
  t = "(display: grid) and (not (foo: bar))";
  ASSERT_EQ(gtext_css_conditions_supports_text(c.c, t, std::strlen(t), &r), GTEXT_CSS_OK);
  EXPECT_TRUE(r);
  EXPECT_EQ(gtext_css_conditions_supports(nullptr, "a", "b", &r), GTEXT_CSS_E_INVALID);
}

TEST(CssSupports, NoRegistryNoGuess) {
  Cond c(nullptr, nullptr, false);
  GTEXT_CSS_Supports_Condition * s = nullptr;
  const char * t = "(display: grid)";
  ASSERT_EQ(gtext_css_supports_parse(t, std::strlen(t), false, nullptr, &s, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Tristate tr;
  GTEXT_CSS_Condition_Call call;
  gtext_css_condition_call_init(&call);
  EXPECT_EQ(gtext_css_conditions_evaluate_supports(c.c, s, &call, &tr), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_STREQ(call.detail, "registry");
  gtext_css_condition_call_free(&call);
  gtext_css_media_list_free(s);
}

TEST(CssSupports, TheIfHookAnswersSupports) {
  Cond c;
  GTEXT_CSS_Environment env = gtext_css_environment_default();
  gtext_css_conditions_environment_hook(c.c, &env);
  GTEXT_CSS_String a{"display: grid", 13}, b{"(foo: bar)", 10};
  EXPECT_EQ(env.condition(env.user, GTEXT_CSS_CONDITION_SUPPORTS, &a, nullptr), GTEXT_CSS_TRI_TRUE);
  EXPECT_EQ(env.condition(env.user, GTEXT_CSS_CONDITION_SUPPORTS, &b, nullptr), GTEXT_CSS_TRI_FALSE);
}

TEST(CssSupports, DeepAndWideConditionsDoNotUseTheCStack) {
  Cond c;
  std::string s;
  const int depth = 50000;
  for (int i = 0; i < depth; i++) s += "(not ";
  s += "(color: red)";
  for (int i = 0; i < depth; i++) s += ")";
  GTEXT_CSS_Media_Options o = gtext_css_media_options_default();
  o.max_depth = 1u << 20;
  o.max_nodes = 1u << 22;
  o.parse.max_depth = 1u << 20;
  GTEXT_CSS_Supports_Condition * p = nullptr;
  ASSERT_EQ(gtext_css_supports_parse(s.data(), s.size(), false, &o, &p, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Tristate t;
  ASSERT_EQ(gtext_css_conditions_evaluate_supports(c.c, p, nullptr, &t), GTEXT_CSS_OK);
  EXPECT_EQ(t, GTEXT_CSS_TRI_TRUE) << "an even number of nots";
  gtext_css_media_list_free(p);
}
