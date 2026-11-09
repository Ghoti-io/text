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

TEST(CssPropertyRule, AnyTypeMayOmitTheInitialValue) {
  // "If initialValue is not present, let parsed initial value be the guaranteed-invalid value." (the pinned text no longer says that a syntax other than * needs one)
  EXPECT_EQ(register_one("--a", "*", nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(register_one("--a", " * ", nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(register_one("--a", "<length>", nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(register_one("--a", "length", nullptr), GTEXT_CSS_OK) << "an identifier too";
  EXPECT_EQ(register_one("--a", "length", "length"), GTEXT_CSS_OK);
  // an initial value that is given is still a value of the syntax: the empty string is one for the universal syntax only
  EXPECT_EQ(register_one("--a", "<length>", ""), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(register_one("--a", "*", ""), GTEXT_CSS_OK);
  // and a registration with no initial value reads as one that has none
  reg::Builder b;
  GTEXT_CSS_Custom_Def d;
  std::memset(&d, 0, sizeof d);
  d.name = "--a";
  d.syntax = "<length>";
  d.inherits = true;
  ASSERT_EQ(gtext_css_registry_register_custom(b.b, &d, nullptr), GTEXT_CSS_OK);
  reg::Owned o;
  ASSERT_EQ(b.freeze(o), GTEXT_CSS_OK);
  GTEXT_CSS_Custom_Info info;
  ASSERT_TRUE(gtext_css_registry_find_custom(o.r, "--a", 3, &info));
  EXPECT_FALSE(info.has_initial);
  EXPECT_EQ(info.initial_value, nullptr);
  EXPECT_FALSE(info.universal);
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

TEST(CssPropertyRule, AValidRuleRegistersOneName) {
  // "@property <custom-property-name> { <declaration-list> }": the prelude is one custom property name
  Sheets s({"@property --width { syntax: \"<length>\"; inherits: false; initial-value: 0px }"});
  EXPECT_EQ(gtext_css_at_property_count(s.set()), 1u);
  GTEXT_CSS_Custom_Info info;
  ASSERT_TRUE(s.has("--width", &info));
  EXPECT_EQ(std::string(info.syntax, info.syntax_length), "<length>");
  EXPECT_FALSE(info.inherits);
  EXPECT_EQ(std::string(info.initial_value, info.initial_length), "0px");
  EXPECT_TRUE(s.reasons().empty());
}

TEST(CssPropertyRule, APreludeListsTheNamesItRegisters) {
  // "@property <<custom-property-name>># { <<declaration-list>> }" and "A valid @property rule represents a custom property registration for each <custom-property-name> in the rule's
  // prelude, with the property name being the serialization of that <custom-property-name> and all other fields identical."
  Sheets s({"@property --width, --height,--depth { syntax: \"<length>\"; inherits: false; initial-value: 0px }"});
  EXPECT_EQ(gtext_css_at_property_count(s.set()), 3u);
  for (const char * n : {"--width", "--height", "--depth"}) {
    GTEXT_CSS_Custom_Info info;
    ASSERT_TRUE(s.has(n, &info)) << n;
    EXPECT_EQ(std::string(info.syntax, info.syntax_length), "<length>") << n;
    EXPECT_FALSE(info.inherits) << n;
    EXPECT_EQ(std::string(info.initial_value, info.initial_length), "0px") << n;
  }
  GTEXT_CSS_Custom_Info first;
  ASSERT_TRUE(gtext_css_at_property_at(s.set(), 0, &first));
  EXPECT_EQ(std::string(first.name, first.name_length), "--width") << "in the order of the prelude";
  EXPECT_TRUE(s.reasons().empty());
}

TEST(CssPropertyRule, EachNameOfAListIsSupersededAndOverriddenOnItsOwn) {
  // "if multiple valid @property rules represent a registration for the same <custom-property-name>, the last one in stylesheet order wins", and the caller's registration wins
  // "over any @property rules for the same <custom-property-name>": both are judged per name
  Reg r;
  r.name = "--c";
  r.syntax = "<number>";
  r.initial = "7";
  Sheets s({"@property --a, --b, --c { syntax: \"<length>\"; inherits: true; initial-value: 1px } @property --b, --d { syntax: \"<length>\"; inherits: false; initial-value: 2px }"}, {r});
  GTEXT_CSS_Custom_Info info;
  ASSERT_TRUE(s.has("--a", &info));
  EXPECT_EQ(std::string(info.initial_value, info.initial_length), "1px");
  ASSERT_TRUE(s.has("--b", &info));
  EXPECT_EQ(std::string(info.initial_value, info.initial_length), "2px");
  EXPECT_FALSE(info.inherits);
  EXPECT_TRUE(s.has("--d"));
  EXPECT_FALSE(s.has("--c")) << "the caller's";
  EXPECT_EQ(gtext_css_at_property_count(s.set()), 3u);
  const auto reasons = s.reasons();
  ASSERT_EQ(reasons.size(), 2u);
  EXPECT_EQ(reasons[0], "caller-registration-wins:--c");
  EXPECT_EQ(reasons[1], "superseded:--b");
}

TEST(CssPropertyRule, ANameTwiceInOnePreludeIsOneRegistrationTheLaterWins) {
  Sheets s({"@property --a, --a { inherits: false }"});
  EXPECT_EQ(gtext_css_at_property_count(s.set()), 1u);
  ASSERT_EQ(s.reasons().size(), 1u);
  EXPECT_EQ(s.reasons()[0], "superseded:--a");
}

TEST(CssPropertyRule, EveryDescriptorIsOptionalAndOmittedOnesMatchAnUnregisteredProperty) {
  // "While the <custom-property-name> is required, all of the descriptors are optional. When omitted, it matches the behavior of an unregistered custom property." The initial
  // values of the descriptors: syntax "*", inherits true, initial-value "the guaranteed-invalid value".
  Sheets s({"@property --a {}", "@property --b { inherits: false }", "@property --c { syntax: \"<length>\" }", "@property --d { initial-value: 7 }",
            "@property --e { syntax: \"<length>\"; inherits: false }"});
  GTEXT_CSS_Custom_Info info;
  ASSERT_TRUE(s.has("--a", &info));
  EXPECT_TRUE(info.universal);
  EXPECT_EQ(std::string(info.syntax, info.syntax_length), "*");
  EXPECT_TRUE(info.inherits);
  EXPECT_FALSE(info.has_initial);
  ASSERT_TRUE(s.has("--b", &info));
  EXPECT_TRUE(info.universal);
  EXPECT_FALSE(info.inherits);
  ASSERT_TRUE(s.has("--c", &info));
  EXPECT_EQ(std::string(info.syntax, info.syntax_length), "<length>");
  EXPECT_TRUE(info.inherits) << "inherits: true";
  EXPECT_FALSE(info.has_initial) << "a typed syntax with no initial value has the guaranteed-invalid value";
  ASSERT_TRUE(s.has("--d", &info));
  EXPECT_TRUE(info.universal);
  EXPECT_TRUE(info.has_initial);
  EXPECT_EQ(std::string(info.initial_value, info.initial_length), "7");
  ASSERT_TRUE(s.has("--e", &info));
  EXPECT_FALSE(info.inherits);
  EXPECT_FALSE(info.has_initial);
  EXPECT_EQ(gtext_css_at_property_count(s.set()), 5u);
  EXPECT_TRUE(s.reasons().empty());
}

TEST(CssPropertyRule, ARuleWithNoDescriptorsComputesAsAnUnregisteredProperty) {
  Sheets s({"@property --a {} @property --b { syntax: \"<length>\" }"});
  Computed p, c, none;
  compute(s.w, p, "--a: 1 2; --b: 2em; font-size: 10px");
  compute(s.w, c, "", &p);
  EXPECT_EQ(c.custom("--a"), "1 2") << "inherits, as the tokens";
  EXPECT_EQ(p.custom("--b"), "20px") << "the typed one is computed";
  EXPECT_EQ(c.custom("--b"), "20px") << "and inherits";
  compute(s.w, none, "");
  EXPECT_EQ(none.custom("--a"), "<gi>");
  EXPECT_EQ(none.custom("--b"), "<gi>") << "no initial value: the guaranteed-invalid value";
  Computed bad;
  compute(s.w, bad, "--b: red");
  EXPECT_EQ(bad.custom("--b"), "<gi>") << "invalid at computed-value time, and the property has no initial value to reset to";
}

TEST(CssPropertyRule, AnInvalidDescriptorIsIgnoredAndTheRuleStandsWithTheDefault) {
  // "If the provided string is not a valid syntax string (if it returns failure when consume a syntax definition is called on it), the descriptor is invalid and must be ignored."
  // The rule has no syntax then, which is "*"; an inherits that is not true or false is as invalid.
  Sheets s({"@property --a { syntax: \"<nosuch>\"; inherits: false; initial-value: 1 }", "@property --b { syntax: <length>; initial-value: 1px }",
            "@property --c { inherits: maybe; syntax: \"<length>\" }", "@property --d { syntax: \"<length>\"; syntax: \"<nosuch>\" }"});
  GTEXT_CSS_Custom_Info info;
  ASSERT_TRUE(s.has("--a", &info));
  EXPECT_TRUE(info.universal);
  EXPECT_FALSE(info.inherits);
  EXPECT_EQ(std::string(info.initial_value, info.initial_length), "1");
  ASSERT_TRUE(s.has("--b", &info)) << "missing quotes: the draft's descriptor is a <string>";
  EXPECT_TRUE(info.universal);
  EXPECT_EQ(std::string(info.initial_value, info.initial_length), "1px");
  ASSERT_TRUE(s.has("--c", &info));
  EXPECT_TRUE(info.inherits);
  EXPECT_EQ(std::string(info.syntax, info.syntax_length), "<length>");
  ASSERT_TRUE(s.has("--d", &info));
  EXPECT_EQ(std::string(info.syntax, info.syntax_length), "<length>") << "the earlier valid one stands over a later one that is not";
  const auto r = s.reasons();
  ASSERT_EQ(r.size(), 4u);
  EXPECT_EQ(r[0], "bad-syntax:--a");
  EXPECT_EQ(r[1], "bad-syntax:--b");
  EXPECT_EQ(r[2], "bad-inherits:--c");
  EXPECT_EQ(r[3], "bad-syntax:--d");
}

TEST(CssPropertyRule, AnInitialValueThatIsNotOfTheSyntaxIsIgnoredAndOneThatNeedsTheElementRefusesTheRule) {
  // "If specified, the value of the initial-value descriptor must successfully parse according to the rule's syntax descriptor, or else the descriptor is invalid and ignored":
  // the rule registers with no initial value. A value that parses and cannot be computed from itself (`3em`, a `var()`) is a registration that "register a custom property"
  // refuses ("If parsed initial value is not computationally independent, throw a SyntaxError"): the rule registers nothing (both browsers drop it from the sheet).
  Sheets s({"@property --a { syntax: \"<length>\"; inherits: true }", "@property --b { syntax: \"<length>\"; inherits: true; initial-value: red }",
            "@property --c { syntax: \"<length>\"; inherits: true; initial-value: 3em }", "@property --d { syntax: \"*\"; inherits: true }",
            "@property --e { syntax: \"<length>\"; inherits: true; initial-value: var(--q) }", "@property --f { syntax: \"<length>\"; initial-value: }"});
  GTEXT_CSS_Custom_Info info;
  ASSERT_TRUE(s.has("--a", &info));
  EXPECT_FALSE(info.has_initial);
  ASSERT_TRUE(s.has("--b", &info));
  EXPECT_FALSE(info.has_initial) << "ignored";
  EXPECT_EQ(std::string(info.syntax, info.syntax_length), "<length>");
  EXPECT_FALSE(s.has("--c"));
  EXPECT_TRUE(s.has("--d"));
  EXPECT_FALSE(s.has("--e"));
  ASSERT_TRUE(s.has("--f", &info));
  EXPECT_FALSE(info.has_initial) << "an empty value is no <length>";
  const auto r = s.reasons();
  ASSERT_EQ(r.size(), 4u);
  EXPECT_EQ(r[0], "bad-initial-value:--b");
  EXPECT_EQ(r[1], "not-computationally-independent:--c");
  EXPECT_EQ(r[2], "not-computationally-independent:--e");
  EXPECT_EQ(r[3], "bad-initial-value:--f");
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

TEST(CssPropertyRule, APreludeThatIsNotAListOfCustomPropertyNamesIgnoresTheRule) {
  Sheets s({"@property foo { syntax: \"*\"; inherits: true }", "@property -- { syntax: \"*\"; inherits: true }", "@property { syntax: \"*\"; inherits: true }",
            "@property --a --b { syntax: \"*\"; inherits: true }", "@property --a, { syntax: \"*\"; inherits: true }",
            "@property --ok, nope { syntax: \"*\"; inherits: true }", "@property , --a { }", "@property --a,, --b { }", "@property --a, , --b { }", "@property --a, --b c { }",
            "@property --a (x) { }", "@property \"--a\" { }"});
  EXPECT_EQ(gtext_css_at_property_count(s.set()), 0u);
  EXPECT_EQ(s.reasons().size(), 12u);
  EXPECT_EQ(s.reasons()[0], "bad-prelude:");
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
  Sheets s({"@property --a { syntax: \"<length>\"; inherits: true; initial-value: 1px } @property --a { syntax: \"<length>\"; inherits: true; initial-value: 2em }"});
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

/* ---- the statement form --------------------------------------------------------------------------------------------------- */

/// What a registration of `css` (one sheet) came to for `name`: "-" when there is none, else `syntax|inherits|initial` ("-" for no initial value).
std::string registration(const std::string & css, const char * name = "--a") {
  Sheets s({css});
  GTEXT_CSS_Custom_Info info;
  if (!s.has(name, &info)) {
    return "-";
  }
  return std::string(info.syntax, info.syntax_length) + "|" + (info.inherits ? "inherits" : "own") + "|" + (info.has_initial ? "[" + std::string(info.initial_value, info.initial_length) + "]" : "-");
}

TEST(CssPropertyRule, TheStatementIsExactlyTheRuleWithItsSyntaxAndInitialValue) {
  // "@property <<custom-property-name>> <<css-type>>? [: <<declaration-value>>? ]? ;" is "exactly equivalent to the normal rule form": the <css-type> is the syntax descriptor, the
  // <declaration-value> the initial-value descriptor, "The other descriptors of the @property rule are always treated as omitted. (So, for example, any property registered with this
  // syntax will inherit.)" The draft's examples: `@property --foo <integer>: 0;` is `@property --foo { syntax: "<integer>"; initial-value: 0 }`, and
  // `@property --bar type(narrow | normal | wide);` is `@property --bar { syntax: "narrow | normal | wide" }`.
  EXPECT_EQ(registration("@property --a <integer>: 0;"), "<integer>|inherits|[0]");
  EXPECT_EQ(registration("@property --a { syntax: \"<integer>\"; initial-value: 0 }"), "<integer>|inherits|[0]");
  EXPECT_EQ(registration("@property --a type(narrow | normal | wide);"), "narrow | normal | wide|inherits|-");
  EXPECT_EQ(registration("@property --a { syntax: \"narrow | normal | wide\" }"), "narrow | normal | wide|inherits|-");
  EXPECT_EQ(registration("@property --a <length>;"), "<length>|inherits|-");
  EXPECT_EQ(registration("@property --a;"), "*|inherits|-");
  EXPECT_EQ(registration("@property --a <length> : 5px ;"), "<length>|inherits|[5px]") << "white space around the colon and before the semicolon";
  EXPECT_EQ(registration("@property --a<length>:5px;"), "<length>|inherits|[5px]");
  EXPECT_EQ(registration("@property --a <length>#: 1px, 2px;"), "<length>#|inherits|[1px, 2px]");
  EXPECT_EQ(registration("@property --a <length>+: 1px 2px;"), "<length>+|inherits|[1px 2px]");
  EXPECT_EQ(registration("@property --a auto: auto;"), "auto|inherits|[auto]") << "one keyword is one syntax component";
  EXPECT_EQ(registration("@property --a type(*): 5;"), "*|inherits|[5]") << "the universal syntax is written type(*)";
  EXPECT_EQ(registration("@property --a type(<length> | auto): auto;"), "<length> | auto|inherits|[auto]");
  EXPECT_EQ(registration("@property --a type( <color># ): red, blue;"), "<color>#|inherits|[red, blue]") << "type() of one component";
}

TEST(CssPropertyRule, TheColonDecidesWhetherThereIsAnInitialValue) {
  // "Whether or not this component is omitted is determined by the presence or absence of the colon separator; the <declaration-value> itself can be validly omitted to specify an
  // empty initial value."
  EXPECT_EQ(registration("@property --a;"), "*|inherits|-");
  EXPECT_EQ(registration("@property --a:;"), "*|inherits|[]");
  EXPECT_EQ(registration("@property --a : ;"), "*|inherits|[]");
  EXPECT_EQ(registration("@property --a: 7;"), "*|inherits|[7]");
  EXPECT_EQ(registration("@property --a <length>:;"), "<length>|inherits|-") << "an empty value is no <length>: the initial value is ignored";
  EXPECT_EQ(registration("@property --a { initial-value: }"), "*|inherits|[]") << "as the rule form says it";
}

TEST(CssPropertyRule, AnInvalidInitialValueOfAStatementIsIgnoredAsTheRulesIs) {
  // the statement is the rule with the two descriptors, so the descriptor rules apply: a value that is not of the type is ignored, one that needs the element refuses the registration
  EXPECT_EQ(registration("@property --a <length>: red;"), "<length>|inherits|-");
  EXPECT_EQ(registration("@property --a <length>: 3em;"), "-");
  EXPECT_EQ(registration("@property --a <length>: var(--q);"), "-");
  EXPECT_EQ(registration("@property --a: var(--q);"), "*|inherits|-");
  Sheets s({"@property --a <length>: red; @property --b <length>: 3em;"});
  const auto r = s.reasons();
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(r[0], "bad-initial-value:--a");
  EXPECT_EQ(r[1], "not-computationally-independent:--b");
}

TEST(CssPropertyRule, AStatementThatIsNotOfTheGrammarIsIgnored) {
  // <css-type> = <syntax-component> | type(<syntax>): a syntax that needs a combinator is wrapped, `*` is `type(*)`, a name is one name, and the value is a <declaration-value>
  for (const char * css : {"@property --a, --b <length>;", "@property --a <foo>;", "@property --a <length> | auto;", "@property --a *;", "@property --a <length> 5px;",
                           "@property --a <length>: 5px !important;", "@property <length>;", "@property a <length>;", "@property --a type();", "@property --a type(<length>) <number>;",
                           "@property --a '<length>';", "@property --a <length>: \"a\nb\";",
                           "@property --a type(<length> | <foo>);", "@property --a <length> <number>;", "@property --a: 1 ) 2;", "@property;", "@property --a, --b;",
                           "@property --a <length>: 5px { inherits: false }"}) {
    EXPECT_EQ(registration(std::string(css) + " "), "-") << css;
  }
  Sheets s({"@property --a <foo>;"});
  ASSERT_EQ(s.reasons().size(), 1u);
  EXPECT_EQ(s.reasons()[0], "bad-prelude:");
}

TEST(CssPropertyRule, AStatementIsRegisteredWhereARuleIsAndRankedWithTheRules) {
  // it is a CSSPropertyRule like the rule form: layers and the last one in stylesheet order decide, and a statement inside a style rule is no rule of one
  Sheets s({"@property --a <length>: 1px; @property --a { syntax: \"<length>\"; initial-value: 2px } @property --a <length>: 3px;",
            "@layer l { @property --b <length>: 1px; } @property --b <length>: 2px; @layer m { @property --c <length>: 3px; } a { @property --d <length>: 4px; }"});
  GTEXT_CSS_Custom_Info info;
  ASSERT_TRUE(s.has("--a", &info));
  EXPECT_EQ(std::string(info.initial_value, info.initial_length), "3px");
  ASSERT_TRUE(s.has("--b", &info));
  EXPECT_EQ(std::string(info.initial_value, info.initial_length), "2px");
  EXPECT_TRUE(s.has("--c"));
  EXPECT_FALSE(s.has("--d"));
  Computed c;
  compute(s.w, c, "");
  EXPECT_EQ(c.custom("--a"), "3px");
}

TEST(CssPropertyRule, ARegisteredStatementComputesLikeTheRuleForm) {
  Sheets s({"@property --a <length>: 5px; @property --b <length>;"});
  Computed p, c, d;
  compute(s.w, p, "font-size: 10px");
  EXPECT_EQ(p.custom("--a"), "5px");
  EXPECT_EQ(p.custom("--b"), "<gi>");
  compute(s.w, c, "--a: 2em; --b: 2em", &p);
  EXPECT_EQ(c.custom("--a"), "20px") << "computed against the element's own font-size, which it inherits";
  compute(s.w, d, "", &c);
  EXPECT_EQ(d.custom("--a"), "20px") << "any property registered with this syntax will inherit";
}

TEST(CssPropertyRule, NothingIsRegisteredByPropertyRulesTheDraftDoesNotPlaceThere) {
  // The pinned draft places an `@property` at the top of a style sheet (and inside the group rules the cascade enters) and "directly inside of a @private rule" (private properties,
  // which this library does not have). It says nothing of the body of an `@function` ("accepts conditional group rules ... the result descriptor ... custom properties") or of a
  // `@mixin`: nothing registers there.
  Sheets s({"@private { @property --a <length>: 1px; } @function --f() { @property --b <length>: 1px; result: 1; } @mixin --m() { @property --c <length>: 1px; } "
            "@function --g() { @media all { @property --d <length>: 1px; } result: 1; }"});
  EXPECT_EQ(gtext_css_at_property_count(s.set()), 0u);
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

TEST(CssPropertyRule, ATypedPropertyWithNoInitialValueIsTheGuaranteedInvalidValueUntilSet) {
  // "optionally, an initial value ... (defaulting to the guaranteed-invalid value)"
  EXPECT_EQ(typed("<length>", nullptr, "", false), "<gi>");
  EXPECT_EQ(typed("<length>", nullptr, "font-size: 10px; --x: 2em", false), "20px");
  EXPECT_EQ(typed("<length>", nullptr, "--x: red", false), "<gi>") << "invalid at computed-value time: reset to the initial value, which is the guaranteed-invalid value";
  EXPECT_EQ(typed("<length>", nullptr, "--x: initial", false), "<gi>");
  EXPECT_EQ(typed("<length>", nullptr, "", true, "--x: 3px"), "3px") << "inherits";
}

TEST(CssPropertyRule, ARegisteredColorIsAColor) {
  EXPECT_EQ(typed("<color>", "black", "--x: red", false), "rgb(255, 0, 0)") << "a computed color is an sRGB color written as rgb()";
  EXPECT_EQ(typed("<color>", "black", "--x: #badbee33", false), "rgba(186, 219, 238, 0.2)");
  EXPECT_EQ(typed("<color>", "black", "--x: tomato", false), "rgb(255, 99, 71)");
  EXPECT_EQ(typed("<color>", "black", "--x: red, blue", false), "rgb(0, 0, 0)") << "not a value of the syntax: the initial value";
  EXPECT_EQ(typed("<color>+", "black", "--x: red  tomato", false), "rgb(255, 0, 0) rgb(255, 99, 71)");
  EXPECT_EQ(typed("<color>#", "black", "--x: red ,tomato", false), "rgb(255, 0, 0), rgb(255, 99, 71)");
  EXPECT_EQ(typed("<color>", "black", "--x: #f00", false), "rgb(255, 0, 0)");
}

TEST(CssPropertyRule, AValueThatIsNotOfTheSyntaxIsInvalidAtComputedValueTime) {
  // "If the specified value for a registered custom property violates the registered syntax, however, the property becomes invalid at computed-value time (and
  // thus resets to the registered initial value)."
  EXPECT_EQ(typed("<length>", "7px", "--x: red"), "7px");
  EXPECT_EQ(typed("<length>", "7px", "--x: 10px 20px"), "7px");
  EXPECT_EQ(typed("<color>", "black", "--x: url(not-a-color)"), "rgb(0, 0, 0)");
  // the cascade did not change: the last declaration is the one that counts, valid or not
  EXPECT_EQ(typed("<color>", "black", "--x: green; --x: url(\"not-a-color\")"), "rgb(0, 0, 0)");
}

TEST(CssPropertyRule, TheUniversalSyntaxIsLikeAnUnregisteredProperty) {
  // "If the registration's syntax is the universal syntax definition, the computed value is the same as for unregistered custom properties"
  EXPECT_EQ(typed("*", nullptr, "--x: /* c */ 1.0  x /* d */"), "1.0  x") << "the white space and comments at the ends are not part of the value, those between are";
  EXPECT_EQ(typed("*", nullptr, "--x: 1.0 /* c */ x"), "1.0 /* c */ x");
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
  EXPECT_EQ(c.value(w.registry.r, "color"), "rgb(0, 128, 0)");
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
  EXPECT_EQ(typed("red | <color>", "red", "--x: blue", false), "rgb(0, 0, 255)");
  EXPECT_EQ(typed("Red | <color>", "Red", "--x: RED", false), "rgb(255, 0, 0)") << "an identifier is compared codepoint by codepoint: RED is a color";
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
