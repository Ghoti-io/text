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
 * Registered custom properties and `@property` rules (css_registry.h, css_at_property.h): syntax strings, the validation of a registration, which rule is valid
 * and which registration wins, and the computed value of a registered custom property. Each test quotes the served draft (CSS Properties and Values API
 * Level 1, 6 August 2026).
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "../src/css/css_syntax_string.h"
#include "css_custom_test_support.h"

using namespace cust;

namespace {

/// Parse a syntax string: "universal", the components written back (`<length>+ | foo`), or "failure@offset".
std::string syntax_of(const std::string & s) {
  css_syntax * syn = nullptr;
  size_t off = 0;
  const GTEXT_CSS_Status st = css_syntax_parse(gtext_allocator_default(), s.data(), s.size(), &syn, &off);
  if (st != GTEXT_CSS_OK) {
    return "failure@" + std::to_string(off);
  }
  std::string out;
  if (syn->universal) {
    out = "universal";
  }
  for (size_t i = 0; i < syn->ncomps; i++) {
    const css_syn_comp & c = syn->comps[i];
    if (i) {
      out += " | ";
    }
    out += c.is_type ? "<" + std::string(c.name, c.name_len) + ">" : std::string(c.name, c.name_len);
    if (c.mult) {
      out += c.mult;
    }
  }
  css_syntax_free(syn);
  return out;
}

/// What registering `syntax` with an initial value comes to: the status, and the detail of the refusal.
GTEXT_CSS_Status register_one(const char * name, const char * syntax, const char * initial, std::string * detail = nullptr, bool inherits = true) {
  reg::Builder b;
  GTEXT_CSS_Custom_Def d;
  std::memset(&d, 0, sizeof d);
  d.name = name;
  d.syntax = syntax;
  d.inherits = inherits;
  d.initial_value = initial;
  GTEXT_CSS_Registry_Result rr;
  const GTEXT_CSS_Status st = gtext_css_registry_register_custom(b.b, &d, &rr);
  if (detail) {
    *detail = rr.detail ? rr.detail : "";
  }
  return st;
}

/// The computed value of `--x` in a world where it is registered with the syntax (and this initial value, nullptr for none), when the element declares `decls`.
std::string typed(const char * syntax, const char * initial, const std::string & decls, bool inherits = true, const std::string & parent = "") {
  World w;
  Reg r;
  r.name = "--x";
  r.syntax = syntax;
  r.initial = initial ? initial : "";
  r.has_initial = initial != nullptr;
  r.inherits = inherits;
  EXPECT_EQ(w.build({r}), GTEXT_CSS_OK) << syntax;
  EXPECT_TRUE(w.refused.empty()) << syntax << " " << (w.refused_detail.empty() ? "" : w.refused_detail[0]);
  Computed p, c;
  compute(w, p, parent);
  compute(w, c, decls, &p);
  EXPECT_EQ(c.status, GTEXT_CSS_OK) << decls << " " << c.result.name << " " << (c.result.detail ? c.result.detail : "");
  return c.custom("--x");
}

/* ---- syntax strings ------------------------------------------------------------------------------------------------------- */

TEST(CssPropertyRule, ValidSyntaxStrings) {
  // "For example, the following are all valid syntax strings. "<length>" accepts length values ... "<length> | <percentage>" ... "<length>+" ... "*""
  EXPECT_EQ(syntax_of("<length>"), "<length>");
  EXPECT_EQ(syntax_of("<length> | <percentage>"), "<length> | <percentage>");
  EXPECT_EQ(syntax_of("<length-percentage>"), "<length-percentage>");
  EXPECT_EQ(syntax_of("big | bigger | BIGGER"), "big | bigger | BIGGER");
  EXPECT_EQ(syntax_of("<length>+"), "<length>+");
  EXPECT_EQ(syntax_of("<color>#"), "<color>#");
  EXPECT_EQ(syntax_of("<length> | auto"), "<length> | auto");
  EXPECT_EQ(syntax_of("foo | <color># | <integer>"), "foo | <color># | <integer>");
  EXPECT_EQ(syntax_of("*"), "universal");
  // "Strip leading and trailing ASCII whitespace from string."
  EXPECT_EQ(syntax_of("  *\t"), "universal");
  EXPECT_EQ(syntax_of(" \n<length>\f"), "<length>");
  EXPECT_EQ(syntax_of("a|b"), "a | b");
  EXPECT_EQ(syntax_of("<length>|<percentage>"), "<length> | <percentage>");
  // every supported name
  for (const char * n : {"length", "number", "percentage", "length-percentage", "string", "color", "image", "url", "integer", "angle", "time", "resolution",
                         "transform-function", "custom-ident"}) {
    EXPECT_EQ(syntax_of(std::string("<") + n + ">"), std::string("<") + n + ">") << n;
  }
  // "<transform-list> is a pre-multiplied data type name equivalent to <transform-function>+"
  EXPECT_EQ(syntax_of("<transform-list>"), "<transform-list>");
}

TEST(CssPropertyRule, InvalidSyntaxStrings) {
  // "If string's length is 0, return failure."
  EXPECT_EQ(syntax_of("").substr(0, 7), "failure");
  EXPECT_EQ(syntax_of("  ").substr(0, 7), "failure");
  // "<color>" without the angle brackets is an identifier; with a missing bracket it is not a name
  EXPECT_EQ(syntax_of("<length").substr(0, 7), "failure");
  EXPECT_EQ(syntax_of("<nosuch>").substr(0, 7), "failure") << "\"If name is a supported syntax component name, return it. Otherwise return failure.\"";
  EXPECT_EQ(syntax_of("<length>++").substr(0, 7), "failure");
  EXPECT_EQ(syntax_of("<length>#+").substr(0, 7), "failure");
  EXPECT_EQ(syntax_of("<length>+#").substr(0, 7), "failure");
  // "If component's name is a pre-multiplied data type name, return component": a multiplier after it is then not a part of the syntax
  EXPECT_EQ(syntax_of("<transform-list>+").substr(0, 7), "failure");
  EXPECT_EQ(syntax_of("<transform-list>#").substr(0, 7), "failure");
  // components are joined by | and by nothing else
  EXPECT_EQ(syntax_of("<length> <percentage>").substr(0, 7), "failure");
  EXPECT_EQ(syntax_of("foo bar").substr(0, 7), "failure");
  EXPECT_EQ(syntax_of("foo, bar").substr(0, 7), "failure");
  EXPECT_EQ(syntax_of("|<length>").substr(0, 7), "failure") << "a leading bar";
  EXPECT_EQ(syntax_of("<length>|").substr(0, 7), "failure") << "a trailing bar";
  // "the universal syntax definition, which is not a syntax component. Therefore, "*" may not be multiplied or combined with anything else."
  EXPECT_EQ(syntax_of("* | <length>").substr(0, 7), "failure");
  EXPECT_EQ(syntax_of("<length> | *").substr(0, 7), "failure");
  EXPECT_EQ(syntax_of("**").substr(0, 7), "failure");
  EXPECT_EQ(syntax_of("*+").substr(0, 7), "failure");
  // an identifier must be a <custom-ident>
  for (const char * k : {"default", "initial", "inherit", "unset", "revert", "revert-layer", "Initial", "DEFAULT"}) {
    EXPECT_EQ(syntax_of(k).substr(0, 7), "failure") << k;
  }
  EXPECT_EQ(syntax_of("rgb(255, 0, 0)").substr(0, 7), "failure");
  EXPECT_EQ(syntax_of("\"<length>\"").substr(0, 7), "failure") << "quotes are not part of the syntax";
  EXPECT_EQ(syntax_of("/* c */ <length>").substr(0, 7), "failure") << "comments are not white space here";
  EXPECT_EQ(syntax_of("<length> /* c */").substr(0, 7), "failure");
  EXPECT_EQ(syntax_of("<length> | 3px").substr(0, 7), "failure");
}

TEST(CssPropertyRule, ASyntaxFailureSaysWhere) {
  EXPECT_EQ(syntax_of("<length> | <nosuch>"), "failure@12");
  EXPECT_EQ(syntax_of("<length> <percentage>"), "failure@9");
}

/* ---- registering ------------------------------------------------------------------------------------------------------------- */

TEST(CssPropertyRule, RegistrationNames) {
  // "If name is not a custom property name string, throw a SyntaxError": a string that begins with two dashes
  EXPECT_EQ(register_one("--name1", "*", nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(register_one("--name2, no need for escapes", "*", nullptr), GTEXT_CSS_OK) << "(a pinned web-platform-test registers this name)";
  EXPECT_EQ(register_one("no-leading-dash", "*", nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(register_one("-one-dash", "*", nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(register_one("", "*", nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(register_one("--", "*", nullptr), GTEXT_CSS_E_INVALID) << "\"--\" itself is reserved";
  EXPECT_EQ(register_one("\\--name", "*", nullptr), GTEXT_CSS_E_INVALID);
}

TEST(CssPropertyRule, RegisteringTwiceFails) {
  // "If property set already contains an entry with name as its property name (compared codepoint-wise), throw an InvalidModificationError"
  reg::Builder b;
  GTEXT_CSS_Custom_Def d;
  std::memset(&d, 0, sizeof d);
  d.name = "--re-register";
  d.syntax = "<length>";
  d.inherits = true;
  d.initial_value = "0px";
  EXPECT_EQ(gtext_css_registry_register_custom(b.b, &d, nullptr), GTEXT_CSS_OK);
  d.syntax = "<percentage>";
  d.initial_value = "0%";
  GTEXT_CSS_Registry_Result rr;
  EXPECT_EQ(gtext_css_registry_register_custom(b.b, &d, &rr), GTEXT_CSS_E_EXISTS);
  d.name = "--Re-register";
  EXPECT_EQ(gtext_css_registry_register_custom(b.b, &d, nullptr), GTEXT_CSS_OK) << "names are compared codepoint by codepoint";
  reg::Owned o;
  ASSERT_EQ(b.freeze(o), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_registry_custom_count(o.r), 2u);
  GTEXT_CSS_Custom_Info info;
  ASSERT_TRUE(gtext_css_registry_find_custom(o.r, "--re-register", 13, &info));
  EXPECT_EQ(std::string(info.syntax, info.syntax_length), "<length>");
  EXPECT_FALSE(info.universal);
  EXPECT_TRUE(info.inherits);
  EXPECT_EQ(std::string(info.initial_value, info.initial_length), "0px");
  EXPECT_EQ(info.component_count, 1u);
  EXPECT_FALSE(gtext_css_registry_find_custom(o.r, "--RE-register", 13, &info));
}

TEST(CssPropertyRule, NonUniversalNeedsAnInitialValue) {
  // "registerProperty only allows omitting initialValue if syntax is '*'"
  EXPECT_EQ(register_one("--a", "*", nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(register_one("--a", " * ", nullptr), GTEXT_CSS_OK);
  std::string detail;
  EXPECT_EQ(register_one("--a", "<length>", nullptr, &detail), GTEXT_CSS_E_INVALID);
  EXPECT_NE(detail.find("initial value"), std::string::npos) << detail;
  EXPECT_EQ(register_one("--a", "length", nullptr), GTEXT_CSS_E_INVALID) << "an identifier needs one as well";
  EXPECT_EQ(register_one("--a", "length", "length"), GTEXT_CSS_OK);
}

TEST(CssPropertyRule, TheInitialValueIsAValueOfTheSyntax) {
  // "parse initialValue according to <declaration-value>? if syntax definition is the universal syntax definition, and according to syntax definition
  // otherwise. If parsing returns failure, throw a SyntaxError"
  EXPECT_EQ(register_one("--a", "<length>", "10px"), GTEXT_CSS_OK);
  EXPECT_EQ(register_one("--a", "<length>", "red"), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(register_one("--a", "<length>", ""), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(register_one("--a", "<length>", "10px 20px"), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(register_one("--a", "<length>+", "10px 20px"), GTEXT_CSS_OK);
  EXPECT_EQ(register_one("--a", "<length>#", "10px, 20px"), GTEXT_CSS_OK);
  EXPECT_EQ(register_one("--a", "<length>#", "10px 20px"), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(register_one("--a", "<length> | auto", "auto"), GTEXT_CSS_OK);
  EXPECT_EQ(register_one("--a", "Red", "Red"), GTEXT_CSS_OK);
  EXPECT_EQ(register_one("--a", "Red", "red"), GTEXT_CSS_E_INVALID) << "identifiers are compared codepoint by codepoint";
  EXPECT_EQ(register_one("--a", "*", "anything at all, 1px {x}"), GTEXT_CSS_OK);
  EXPECT_EQ(register_one("--a", "*", ""), GTEXT_CSS_OK) << "an empty initial value is a value";
  EXPECT_EQ(register_one("--a", "*", "calc(1 + "), GTEXT_CSS_OK) << "the end of the input closes it";
  EXPECT_EQ(register_one("--a", "*", "\"a\nb\""), GTEXT_CSS_E_INVALID) << "a bad-string token is no <declaration-value>";
  EXPECT_EQ(register_one("--a", "*", "a ) b"), GTEXT_CSS_E_INVALID) << "nor is an unmatched )";
}

TEST(CssPropertyRule, TheInitialValueMustBeComputationallyIndependent) {
  // "A property value is computationally independent if it can be converted into a computed value using only the value of the property on the element, and
  // "global" information that cannot be changed by CSS. For example, 5px is computationally independent ... Similarly, 1in is computationally independent ...
  // On the other hand, 3em is not computationally independent ... Neither is a value with a var() function"
  EXPECT_EQ(register_one("--a", "<length>", "5px"), GTEXT_CSS_OK);
  EXPECT_EQ(register_one("--a", "<length>", "1in"), GTEXT_CSS_OK);
  EXPECT_EQ(register_one("--a", "<length>", "10vw"), GTEXT_CSS_OK) << "viewport units: the environment's, not the element's";
  EXPECT_EQ(register_one("--a", "<length>", "calc(5px * 3px / 6px)"), GTEXT_CSS_OK);
  std::string detail;
  EXPECT_EQ(register_one("--a", "<length>", "3em", &detail), GTEXT_CSS_E_INVALID);
  EXPECT_NE(detail.find("computationally independent"), std::string::npos) << detail;
  for (const char * v : {"3rem", "1ex", "1ch", "1lh", "calc(1em + 1px)", "10cqw"}) {
    EXPECT_EQ(register_one("--a", "<length>", v), GTEXT_CSS_E_INVALID) << v;
  }
  EXPECT_EQ(register_one("--a", "<color>", "currentcolor"), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(register_one("--a", "<color>", "red"), GTEXT_CSS_OK);
  EXPECT_EQ(register_one("--a", "<length>", "var(--x)"), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(register_one("--a", "*", "var(--x)"), GTEXT_CSS_E_INVALID) << "nothing is there to substitute from";
}

TEST(CssPropertyRule, RegistrationsAreCountedAndBounded) {
  GTEXT_CSS_Registry_Options o = gtext_css_registry_options_default();
  o.max_registrations = 3;
  reg::Builder b(&o);
  GTEXT_CSS_Custom_Def d;
  std::memset(&d, 0, sizeof d);
  d.syntax = "*";
  std::string names[4] = {"--a", "--b", "--c", "--d"};
  GTEXT_CSS_Status last = GTEXT_CSS_OK;
  for (int i = 0; i < 4; i++) {
    d.name = names[i].c_str();
    last = gtext_css_registry_register_custom(b.b, &d, nullptr);
    EXPECT_EQ(last, i < 3 ? GTEXT_CSS_OK : GTEXT_CSS_E_LIMIT_REGISTRY) << i;
  }
}

/* ---- @property rules ------------------------------------------------------------------------------------------------------- */

/// A world whose registrations are those of these sheets; `rules` is its set.
struct Sheets {
  World w;
  explicit Sheets(const std::vector<std::string> & sheets, const std::vector<Reg> & regs = {}) {
    EXPECT_EQ(w.build(regs, sheets), GTEXT_CSS_OK);
  }
  GTEXT_CSS_At_Property_Set * set() {
    return w.rules;
  }
  bool has(const char * name, GTEXT_CSS_Custom_Info * info = nullptr) {
    GTEXT_CSS_Custom_Info local;
    return gtext_css_at_property_find(w.rules, name, std::strlen(name), info ? info : &local);
  }
  std::vector<std::string> reasons() {
    std::vector<std::string> out;
    GTEXT_CSS_At_Property_Diagnostic d;
    for (size_t i = 0; gtext_css_at_property_diagnostic(w.rules, i, &d); i++) {
      out.push_back(std::string(gtext_css_at_property_reason_name(d.reason)) + ":" + std::string(d.name.data, d.name.length));
    }
    return out;
  }
};

TEST(CssPropertyRule, AValidRuleRegisters) {
  // "@property --width, --height, --depth { syntax: "<length>"; inherits: false; initial-value: 0px; } The following rule registers three properties with the same
  // syntax, inheritance behavior, and initial value"
  Sheets s({"@property --width, --height, --depth { syntax: \"<length>\"; inherits: false; initial-value: 0px }"});
  EXPECT_EQ(gtext_css_at_property_count(s.set()), 3u);
  GTEXT_CSS_Custom_Info info;
  for (const char * n : {"--width", "--height", "--depth"}) {
    ASSERT_TRUE(s.has(n, &info)) << n;
    EXPECT_EQ(std::string(info.syntax, info.syntax_length), "<length>");
    EXPECT_FALSE(info.inherits);
    EXPECT_EQ(std::string(info.initial_value, info.initial_length), "0px");
  }
  EXPECT_TRUE(s.reasons().empty());
}

TEST(CssPropertyRule, ARuleWithoutSyntaxOrInheritsIsIgnoredAndSaysWhere) {
  // The plan's matrix row: "@property --x { inherits: true } ... rule ignored: diagnostic with location". The served draft says the descriptors are optional;
  // the condition is rule_valid() of css_at_property.c, and the pinned web-platform-tests helper still says "The syntax descriptor is required ... The inherits
  // descriptor is required".
  Sheets s({"@property --x { inherits: true }", "@property --y { syntax: \"*\" }"});
  EXPECT_FALSE(s.has("--x"));
  EXPECT_FALSE(s.has("--y"));
  const auto r = s.reasons();
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(r[0], "no-syntax:--x");
  EXPECT_EQ(r[1], "no-inherits:--y");
  GTEXT_CSS_At_Property_Diagnostic d;
  ASSERT_TRUE(gtext_css_at_property_diagnostic(s.set(), 0, &d));
  EXPECT_EQ(d.span.line, 1u) << "with its location";
  EXPECT_EQ(d.span.offset, 0u);
  EXPECT_EQ(d.sheet, 0u);
  ASSERT_TRUE(gtext_css_at_property_diagnostic(s.set(), 1, &d));
  EXPECT_EQ(d.sheet, 1u) << "the second sheet given";
  EXPECT_EQ(d.span.offset, 0u);
  EXPECT_NE(std::string(d.message).find("inherits"), std::string::npos);
}

TEST(CssPropertyRule, AnInvalidSyntaxDescriptorIsIgnoredAndSoTheRuleHasNone) {
  // "If the provided string is not a valid syntax string (if it returns failure when consume a syntax definition is called on it), the descriptor is invalid and
  // must be ignored."
  Sheets s({"@property --a { syntax: \"<nosuch>\"; inherits: true; initial-value: 1 }", "@property --b { syntax: <length>; inherits: true; initial-value: 1px }"});
  EXPECT_FALSE(s.has("--a"));
  EXPECT_FALSE(s.has("--b")) << "missing quotes";
  const auto r = s.reasons();
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(r[0], "bad-syntax:--a");
  EXPECT_EQ(r[1], "bad-syntax:--b");
}

TEST(CssPropertyRule, ANonUniversalSyntaxNeedsAnInitialValueThatMatches) {
  Sheets s({"@property --a { syntax: \"<length>\"; inherits: true }", "@property --b { syntax: \"<length>\"; inherits: true; initial-value: red }",
            "@property --c { syntax: \"<length>\"; inherits: true; initial-value: 3em }", "@property --d { syntax: \"*\"; inherits: true }",
            "@property --e { syntax: \"<length>\"; inherits: true; initial-value: var(--q) }"});
  EXPECT_FALSE(s.has("--a"));
  EXPECT_FALSE(s.has("--b"));
  EXPECT_FALSE(s.has("--c"));
  EXPECT_TRUE(s.has("--d")) << "\"a syntax of * needs no initial value\"";
  EXPECT_FALSE(s.has("--e"));
  const auto r = s.reasons();
  ASSERT_EQ(r.size(), 4u);
  EXPECT_EQ(r[0], "no-initial-value:--a");
  EXPECT_EQ(r[1], "bad-initial-value:--b");
  EXPECT_EQ(r[2], "not-computationally-independent:--c");
  EXPECT_EQ(r[3], "not-computationally-independent:--e");
}

TEST(CssPropertyRule, AnInvalidInitialValueOfTheUniversalSyntaxIsIgnoredAndTheRuleStands) {
  // "If specified, the value of the initial-value descriptor must successfully parse according to the rule's syntax descriptor, or else the descriptor is invalid
  // and ignored."
  Sheets s({"@property --a { syntax: \"*\"; inherits: true; initial-value: var(--q) }"});
  GTEXT_CSS_Custom_Info info;
  ASSERT_TRUE(s.has("--a", &info));
  EXPECT_FALSE(info.has_initial);
  ASSERT_EQ(s.reasons().size(), 1u);
}

TEST(CssPropertyRule, NamesThatAreNoCustomPropertyNamesIgnoreTheRule) {
  Sheets s({"@property foo { syntax: \"*\"; inherits: true }", "@property -- { syntax: \"*\"; inherits: true }", "@property { syntax: \"*\"; inherits: true }",
            "@property --a --b { syntax: \"*\"; inherits: true }", "@property --a, { syntax: \"*\"; inherits: true }",
            "@property --ok, nope { syntax: \"*\"; inherits: true }"});
  EXPECT_EQ(gtext_css_at_property_count(s.set()), 0u);
  EXPECT_EQ(s.reasons().size(), 6u);
  EXPECT_EQ(s.reasons()[0], "bad-name:");
}

TEST(CssPropertyRule, UnknownDescriptorsAreIgnoredAndTheRuleStands) {
  // "Unknown descriptors are invalid and ignored, but do not invalidate the @property rule."
  Sheets s({"@property --a { syntax: \"*\"; inherits: true; color: red; --b: 1 }"});
  EXPECT_TRUE(s.has("--a"));
  const auto r = s.reasons();
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(r[0], "unknown-descriptor:color");
  EXPECT_EQ(r[1], "unknown-descriptor:--b");
}

TEST(CssPropertyRule, TheLastValidRuleWins) {
  // "if multiple valid @property rules represent a registration for the same <custom-property-name>, the last one in stylesheet order "wins""
  Sheets s({"@property --a { syntax: \"<length>\"; inherits: true; initial-value: 1px } @property --a { syntax: \"<length>\"; inherits: false; initial-value: 2px }",
            "@property --a { syntax: \"<percentage>\"; inherits: true; initial-value: 3% }"});
  GTEXT_CSS_Custom_Info info;
  ASSERT_TRUE(s.has("--a", &info));
  EXPECT_EQ(std::string(info.syntax, info.syntax_length), "<percentage>");
  EXPECT_EQ(std::string(info.initial_value, info.initial_length), "3%");
  EXPECT_EQ(gtext_css_at_property_count(s.set()), 1u);
  const auto r = s.reasons();
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(r[0], "superseded:--a");
  GTEXT_CSS_At_Property_Diagnostic d;
  ASSERT_TRUE(gtext_css_at_property_diagnostic(s.set(), 1, &d));
  EXPECT_EQ(d.sheet, 1u);
}

TEST(CssPropertyRule, AnInvalidLaterRuleDoesNotReplaceAValidEarlierOne) {
  Sheets s({"@property --a { syntax: \"<length>\"; inherits: true; initial-value: 1px } @property --a { syntax: \"<length>\"; inherits: true }"});
  GTEXT_CSS_Custom_Info info;
  ASSERT_TRUE(s.has("--a", &info));
  EXPECT_EQ(std::string(info.initial_value, info.initial_length), "1px");
}

TEST(CssPropertyRule, ACallersRegistrationWinsOverARule) {
  // "A custom property registration from CSS.registerProperty() further wins over any @property rules for the same <custom-property-name>."
  Reg r;
  r.name = "--a";
  r.syntax = "<number>";
  r.initial = "7";
  Sheets s({"@property --a { syntax: \"<length>\"; inherits: true; initial-value: 1px } @property --b { syntax: \"<length>\"; inherits: true; initial-value: 1px }"}, {r});
  EXPECT_FALSE(s.has("--a")) << "the set holds the rules' registrations that are in effect";
  EXPECT_TRUE(s.has("--b"));
  ASSERT_EQ(s.reasons().size(), 1u);
  EXPECT_EQ(s.reasons()[0], "caller-registration-wins:--a");
  // and the context computes with the caller's
  Computed c;
  compute(s.w, c, "");
  EXPECT_EQ(c.custom("--a"), "7");
  EXPECT_EQ(c.custom("--b"), "1px");
}

TEST(CssPropertyRule, RulesInLayersAndConditionalRules) {
  GTEXT_CSS_Registry * r = nullptr;
  ASSERT_EQ(gtext_css_registry_new(nullptr, &r), GTEXT_CSS_OK);
  GTEXT_CSS_At_Property_Options o = gtext_css_at_property_options_default();
  struct Ctx { int answer; int asked; } ctx = {1, 0};
  o.user = &ctx;
  o.active = [](void * u, const GTEXT_CSS_Rule *) { static_cast<Ctx *>(u)->asked++; return static_cast<Ctx *>(u)->answer != 0; };
  GTEXT_CSS_At_Property_Set * set = nullptr;
  ASSERT_EQ(gtext_css_at_property_set_new(r, &o, &set), GTEXT_CSS_OK);
  const std::string css = "@layer a { @property --l { syntax: \"*\"; inherits: true } } @media print { @property --m { syntax: \"*\"; inherits: true } } "
                          "@supports (x: y) { @layer b { @property --n { syntax: \"*\"; inherits: true } } } a { @property --nested { syntax: \"*\"; inherits: true } }";
  GTEXT_CSS_Tree * tree = nullptr;
  ASSERT_EQ(gtext_css_parse_stylesheet(css.data(), css.size(), nullptr, &tree, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_At_Property_Result res;
  EXPECT_EQ(gtext_css_at_property_collect(set, tree, &res), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_at_property_count(set), 3u);
  EXPECT_EQ(ctx.asked, 2);
  GTEXT_CSS_Custom_Info info;
  EXPECT_TRUE(gtext_css_at_property_find(set, "--l", 3, &info));
  EXPECT_TRUE(gtext_css_at_property_find(set, "--m", 3, &info));
  EXPECT_TRUE(gtext_css_at_property_find(set, "--n", 3, &info));
  EXPECT_FALSE(gtext_css_at_property_find(set, "--nested", 8, &info)) << "an @property is not valid inside a style rule";
  gtext_css_at_property_set_free(set);
  // without the callback a conditional rule that holds an @property is not guessed at
  GTEXT_CSS_At_Property_Set * set2 = nullptr;
  ASSERT_EQ(gtext_css_at_property_set_new(r, nullptr, &set2), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_at_property_collect(set2, tree, &res), GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_NE(res.detail, nullptr);
  EXPECT_GT(res.span.offset, 0u) << "the rule concerned";
  gtext_css_at_property_set_free(set2);
  ctx.answer = 0;
  o.active = [](void * u, const GTEXT_CSS_Rule *) { return static_cast<Ctx *>(u)->answer != 0; };
  GTEXT_CSS_At_Property_Set * set3 = nullptr;
  ASSERT_EQ(gtext_css_at_property_set_new(r, &o, &set3), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_at_property_collect(set3, tree, &res), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_at_property_count(set3), 1u) << "(a conditional rule that does not apply is not entered; the layer outside it is)";
  gtext_css_at_property_set_free(set3);
  gtext_css_tree_free(tree);
  gtext_css_registry_free(r);
}

/* ---- computing a registered custom property ------------------------------------------------------------------------------ */

TEST(CssPropertyRule, ARegisteredLengthIsComputedToPx) {
  // The plan's matrix row: `@property --len { syntax: "<length>"; inherits: false; initial-value: 0px }`, `--len: 2em` computes `2em` as px.
  // "If the specified value is a dimension literal (such as 50em or .2s), the computed value is the same value, but with the unit converted to the corresponding
  // canonical unit for the type of value."
  EXPECT_EQ(typed("<length>", "0px", "font-size: 10px; --x: 2em", false), "20px");
  EXPECT_EQ(typed("<length>", "0px", "--x: 1in", false), "96px");
  EXPECT_EQ(typed("<length>", "0px", "--x: 1.5px", false), "1.5px");
  EXPECT_EQ(typed("<length>", "0px", "font-size: 10px; --x: calc(1em + 2px)", false), "12px");
  EXPECT_EQ(typed("<length-percentage>", "0px", "--x: 50%", false), "50%") << "\"In particular, percentages are never resolved against anything.\"";
  EXPECT_EQ(typed("<angle>", "0deg", "--x: 0.5turn", false), "180deg");
  EXPECT_EQ(typed("<time>", "0s", "--x: 500ms", false), "0.5s");
  EXPECT_EQ(typed("<number>", "0", "--x: calc(1 + 2)", false), "3");
  EXPECT_EQ(typed("<integer>", "0", "--x: 5", false), "5");
  EXPECT_EQ(typed("<resolution>", "1dppx", "--x: 96dpi", false), "1dppx");
  EXPECT_EQ(typed("<string>", "\"a\"", "--x: 'b'", false), "\"b\"");
  EXPECT_EQ(typed("<custom-ident>", "none", "--x: Foo", false), "Foo") << "\"For <custom-ident>, ident, or * values, the computed value is as specified.\"";
}

TEST(CssPropertyRule, ARegisteredColorIsAColor) {
  EXPECT_EQ(typed("<color>", "black", "--x: red", false), "red");
  EXPECT_EQ(typed("<color>", "black", "--x: #f00", false), "rgb(255, 0, 0)");
}

TEST(CssPropertyRule, AValueThatIsNotOfTheSyntaxIsInvalidAtComputedValueTime) {
  // "If the specified value for a registered custom property violates the registered syntax, however, the property becomes invalid at computed-value time (and
  // thus resets to the registered initial value)."
  EXPECT_EQ(typed("<length>", "7px", "--x: red"), "7px");
  EXPECT_EQ(typed("<length>", "7px", "--x: 10px 20px"), "7px");
  EXPECT_EQ(typed("<color>", "black", "--x: url(not-a-color)"), "black");
  // the cascade did not change: the last declaration is the one that counts, valid or not
  EXPECT_EQ(typed("<color>", "black", "--x: green; --x: url(\"not-a-color\")"), "black");
}

TEST(CssPropertyRule, TheUniversalSyntaxIsLikeAnUnregisteredProperty) {
  // "If the registration's syntax is the universal syntax definition, the computed value is the same as for unregistered custom properties"
  EXPECT_EQ(typed("*", nullptr, "--x: /* c */ 1.0  x"), "/* c */ 1.0  x");
  EXPECT_EQ(typed("*", nullptr, "--x: var(--nosuch)"), "<gi>") << "\"registered custom property with universal syntax: the computed value is the guaranteed-invalid value\"";
  EXPECT_EQ(typed("*", "", ""), "") << "an initial value that is empty is the empty value";
}

TEST(CssPropertyRule, InheritsDecidesWhetherTheParentsValueIsTaken) {
  // "inherits ... Specifies the inherit flag of the custom property registration, controlling whether or not the property inherits by default."
  EXPECT_EQ(typed("<length>", "1px", "", true, "--x: 5px"), "5px");
  EXPECT_EQ(typed("<length>", "1px", "", false, "--x: 5px"), "1px");
  EXPECT_EQ(typed("<length>", "1px", "--x: inherit", false, "--x: 5px"), "5px") << "inherit works for a property that does not inherit";
  EXPECT_EQ(typed("<length>", "1px", "--x: unset", false, "--x: 5px"), "1px");
  EXPECT_EQ(typed("<length>", "1px", "--x: unset", true, "--x: 5px"), "5px");
  EXPECT_EQ(typed("<length>", "1px", "--x: initial", true, "--x: 5px"), "1px");
  EXPECT_EQ(typed("<length>", "1px", ""), "1px") << "the root has the initial value";
}

TEST(CssPropertyRule, TheInitialValueIsSubstituted) {
  // "Initial values substituted as computed value": the initial value is a computed value
  World w;
  ASSERT_EQ(w.build({{"--c", "<color>", "green", true, true}, {"--l", "<length>", "10vw", true, true}}), GTEXT_CSS_OK);
  Computed c;
  compute(w, c, "color: var(--c); width: var(--l)");
  EXPECT_EQ(c.value(w.registry.r, "color"), "green");
  EXPECT_EQ(c.value(w.registry.r, "width"), "128px") << "10vw of a 1280px viewport";
}

TEST(CssPropertyRule, SubstitutionIsOfTheComputedValue) {
  // "Suppose that --x is registered with <length> syntax, and that --y is an unregistered custom property. div { font-size: 10px; --x: 8em; --y: var(--x); }
  // Because the computed value of --x (when serialized) is "80px", the computed value of --y is a <dimension-token> with a value of "80" and unit "px"."
  World w;
  ASSERT_EQ(w.build({{"--x", "<length>", "0px", true, true}}), GTEXT_CSS_OK);
  Computed c;
  compute(w, c, "font-size: 10px; --x: 8em; --y: var(--x)");
  EXPECT_EQ(c.custom("--x"), "80px");
  EXPECT_EQ(c.custom("--y"), "80px");
}

TEST(CssPropertyRule, ADependencyOnTheFontSizeThatReachesItselfIsACycle) {
  // "CSS.registerProperty({ name: "--my-font-size", syntax: "<length>", initialValue: "0px", inherits: false }); the following will produce a dependency cycle:
  // div { --my-font-size: 10em; font-size: var(--my-font-size); } and font-size will behave as if unset was specified."
  World w;
  ASSERT_EQ(w.build({{"--my-font-size", "<length>", "0px", false, true}}), GTEXT_CSS_OK);
  Computed p, c;
  compute(w, p, "font-size: 20px");
  compute(w, c, "--my-font-size: 10em; font-size: var(--my-font-size)", &p);
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.value(w.registry.r, "font-size"), "20px") << "unset: inherited";
  EXPECT_EQ(c.custom("--my-font-size"), "0px") << "and the property in the cycle is invalid at computed-value time: its initial value";
}

TEST(CssPropertyRule, RelativeUnitsThatDoNotReachThemselvesAreComputedAgainstTheElement) {
  World w;
  ASSERT_EQ(w.build({{"--len", "<length>", "0px", false, true}}), GTEXT_CSS_OK);
  Computed c;
  compute(w, c, "font-size: 12px; --len: 2em; width: var(--len); margin-top: calc(var(--len) + 1px)");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.value(w.registry.r, "width"), "24px");
  EXPECT_EQ(c.value(w.registry.r, "margin-top"), "25px");
  // rem is the root's font size, em the element's own
  Computed root, child;
  compute(w, root, "font-size: 20px; --len: 1rem");
  compute(w, child, "font-size: 10px; --len: 1rem", &root);
  EXPECT_EQ(root.custom("--len"), "20px");
  EXPECT_EQ(child.custom("--len"), "20px");
}

TEST(CssPropertyRule, ListsAndAlternativesAreComputedPerComponent) {
  EXPECT_EQ(typed("<length>#", "1px", "font-size: 10px; --x: 1em, 2in", false), "10px, 192px");
  EXPECT_EQ(typed("<length>+", "1px", "--x: 1in 2px", false), "96px 2px");
  EXPECT_EQ(typed("<length> | auto", "auto", "--x: auto", false), "auto");
  EXPECT_EQ(typed("<length> | auto", "auto", "--x: 1in", false), "96px");
  // "the syntax components are matched in the order specified": "red | <color>" gives the identifier red as an identifier and blue as a color
  EXPECT_EQ(typed("red | <color>", "red", "--x: red", false), "red");
  EXPECT_EQ(typed("red | <color>", "red", "--x: blue", false), "blue");
  EXPECT_EQ(typed("Red | <color>", "Red", "--x: RED", false), "red") << "an identifier is compared codepoint by codepoint: RED is a color";
}

TEST(CssPropertyRule, AUrlIsResolvedAgainstTheBase) {
  World w;
  w.env.base_url = "http://example.com/dir/page.html";
  ASSERT_EQ(w.build({{"--u", "<url>", "url(a.png)", false, true}}), GTEXT_CSS_OK);
  Computed c;
  compute(w, c, "--u: url(b.png)");
  EXPECT_EQ(c.custom("--u"), "url(\"http://example.com/dir/b.png\")");
}

TEST(CssPropertyRule, RegisteredNamesAreCaseSensitive) {
  EXPECT_EQ(typed("<length>", "1px", "--X: 5px"), "1px");
}

TEST(CssPropertyRule, AStyleCountsItsRegisteredPropertiesInOrder) {
  World w;
  ASSERT_EQ(w.build({{"--b", "<length>", "0px", true, true}, {"--d", "*", "", true, false}}), GTEXT_CSS_OK);
  Computed c;
  compute(w, c, "--a: 1; --c: 2; --e: 3");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_computed_custom_count(c.s), 3u + 1u) << "--d has no initial value: no value";
  std::string names;
  for (size_t i = 0; i < gtext_css_computed_custom_count(c.s); i++) {
    GTEXT_CSS_String n;
    ASSERT_TRUE(gtext_css_computed_custom_at(c.s, i, &n, nullptr));
    names += std::string(n.data, n.length) + " ";
  }
  EXPECT_EQ(names, "--a --b --c --e ");
}

} // namespace
