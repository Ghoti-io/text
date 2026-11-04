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
 * `@property` rules of a CSSOM document's style sheets, registered in the document's computed styles (css_cssom_computed.h, css_at_property.h). Each test quotes the served text:
 * CSS Properties and Values API 1, 6 August 2026 ("@property rule", "Registering a custom property with the @property rule", "Computed value of a registered custom property"),
 * CSS Cascade ("the rule is in the document's cascade order"), and the browsers' answers recorded for the page tests this is measured by (css/css-values/if-conditionals.html and the
 * pages of css-properties-values-api).
 */

#include <string>

#include <gtest/gtest.h>

#include "css_registry_test_support.h"
#include "cssom_computed_test_support.h"

using namespace cc;

namespace {

const char * kLength = "@property --l { syntax: \"<length>\"; inherits: false; initial-value: 3px }";

std::string page(const std::string & sheet, const std::string & body = "<div id=a><p id=b>x</p></div>", const std::string & attrs = "") {
  return "<!DOCTYPE html><html><head><style id=s" + attrs + ">" + sheet + "</style></head><body>" + body + "</body></html>";
}

/* Properties and Values 1: "The computed value of a registered custom property is the specified value with its computed-value-time substitutions ... and, for a <length>, calc() resolved
   to an absolute length". The browsers answer `3px` (Chromium 156.0.8078.47, Firefox 158.0.1). */
TEST(CssomAtProperty, ARegisteredLengthIsComputedMathResolved) {
  Cs m(page(std::string(kLength) + "#a { --l: calc(1px + 2px) }"));
  EXPECT_EQ(m.value("#a", "--l"), "3px");
}

TEST(CssomAtProperty, AFontRelativeLengthIsAbsoluteAtTheElementsFontSize) {
  Cs m(page(std::string(kLength) + "#a { --l: 1em; font-size: 10px }"));
  EXPECT_EQ(m.value("#a", "--l"), "10px");
}

/* Properties and Values 1: "If the custom property is registered, and it is not set on the element, its computed value is its initial value" - a non-inherited registered property
   an element does not set is read as the initial value, where an unregistered one has none. */
TEST(CssomAtProperty, ANonInheritedRegisteredPropertyThatIsNotSetReadsItsInitialValue) {
  Cs m(page(std::string(kLength) + "#a { --l: 7px }"));
  EXPECT_EQ(m.value("#b", "--l"), "3px");
  EXPECT_EQ(m.value("#a", "--l"), "7px");
}

TEST(CssomAtProperty, ARegisteredPropertyThatInheritsPassesItsComputedValueDown) {
  Cs m(page("@property --l { syntax: \"<length>\"; inherits: true; initial-value: 3px } #a { --l: 2em; font-size: 10px } #b { font-size: 50px }"));
  EXPECT_EQ(m.value("#b", "--l"), "20px") << "the parent's computed value, not the child's font size";
}

TEST(CssomAtProperty, TheSameRulesPropertyIsUnregisteredWhereThereIsNoRule) {
  Cs m(page("#a { --l: calc(1px + 2px) }"));
  EXPECT_EQ(m.value("#a", "--l"), "calc(1px + 2px)");
  EXPECT_EQ(m.value("#b", "--l"), "calc(1px + 2px)") << "an unregistered custom property inherits, as the tokens";
}

/* the I/O row: a style query on a registered property compares computed values */
TEST(CssomAtProperty, AStyleQueryOfAnIfCompareComputedValues) {
  Cs m(page(std::string(kLength) + "#a { --l: calc(1px + 2px); --r: if(style(--l: 3px): T; else: F) }"));
  EXPECT_EQ(m.value("#a", "--r"), "T");
}

TEST(CssomAtProperty, AStyleQueryOfAnUnregisteredPropertyComparesTokens) {
  Cs m(page("#a { --l: calc(1px + 2px); --r: if(style(--l: 3px): T; else: F) }"));
  EXPECT_EQ(m.value("#a", "--r"), "F");
}

/* Properties and Values 1: "If multiple valid @property rules are defined for the same name, the last one wins." */
TEST(CssomAtProperty, TheLastValidRuleOfANameWins) {
  Cs m(page("@property --l { syntax: \"<length>\"; inherits: false; initial-value: 3px } @property --l { syntax: \"<length>\"; inherits: false; initial-value: 9px }"));
  EXPECT_EQ(m.value("#b", "--l"), "9px");
}

TEST(CssomAtProperty, AnInvalidRuleAfterAValidOneDoesNotReplaceIt) {
  Cs m(page("@property --l { syntax: \"<length>\"; inherits: false; initial-value: 3px } @property --l { syntax: \"<length>\"; inherits: false }"));
  EXPECT_EQ(m.value("#b", "--l"), "3px");
}

/* "initial-value ... is required unless the syntax is universal": the rule is invalid, the property is not registered */
TEST(CssomAtProperty, ARuleWithNoInitialValueRegistersNothing) {
  Cs m(page("@property --l { syntax: \"<length>\"; inherits: false } #a { --l: calc(1px + 2px) }"));
  EXPECT_EQ(m.value("#a", "--l"), "calc(1px + 2px)");
}

/* the cascade order: a later sheet's rule wins over an earlier sheet's, as for any at-rule that cascades by name */
TEST(CssomAtProperty, ALaterSheetsRuleWinsOverAnEarlierOnes) {
  Cs m("<!DOCTYPE html><html><head><style>@property --l { syntax: \"<length>\"; inherits: false; initial-value: 3px }</style>"
       "<style>@property --l { syntax: \"<length>\"; inherits: false; initial-value: 5px }</style></head><body><p id=b>x</p></body></html>");
  EXPECT_EQ(m.value("#b", "--l"), "5px");
}

TEST(CssomAtProperty, ARuleInsideAnActiveMediaRuleRegistersAndOneInsideAnInactiveOneDoesNot) {
  Cs m(page("@media (min-width: 1px) { @property --a { syntax: \"<length>\"; inherits: false; initial-value: 1px } }"
            "@media (max-width: 0px) { @property --b { syntax: \"<length>\"; inherits: false; initial-value: 2px } }"));
  EXPECT_EQ(m.value("#b", "--a"), "1px");
  EXPECT_EQ(m.value("#b", "--b"), "");
}

TEST(CssomAtProperty, ARuleInsideALayerRegisters) {
  Cs m(page("@layer base { @property --l { syntax: \"<length>\"; inherits: false; initial-value: 3px } }"));
  EXPECT_EQ(m.value("#b", "--l"), "3px");
}

/* ---- the cascade: layers, and the rules the reader enters ----------------------------------------------------------------------------------------------------- */

std::string rule_p(const char * name, const char * px) {
  return std::string("@property ") + name + " { syntax: \"<length>\"; inherits: false; initial-value: " + px + " }";
}

/* Cascade 5, 6.4.3: "Unlayered rules are sorted later than any layered rules within the same parent layer (if any)". The rule that wins is the one the cascade puts last: an unlayered
   rule over a layered one that comes after it (Chromium 156.0.8078.47 and Firefox 158.0.1 both answer the unlayered `1px`). */
TEST(CssomAtProperty, AnUnlayeredRuleWinsOverALayeredOneThatComesAfterIt) {
  Cs m(page(rule_p("--p", "1px") + "@layer x { " + rule_p("--p", "2px") + " }"));
  EXPECT_EQ(m.value("#b", "--p"), "1px");
  const GTEXT_CSS_At_Property_Set * set = nullptr;
  ASSERT_EQ(gtext_cssom_document_property_rules(m.d(), &set, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_at_property_diagnostic_total(set), 1u) << "the later rule, in a lower layer, is the one that is superseded";
  GTEXT_CSS_At_Property_Diagnostic diag;
  ASSERT_TRUE(gtext_css_at_property_diagnostic(set, 0, &diag));
  EXPECT_EQ(diag.reason, GTEXT_CSS_AT_PROP_SUPERSEDED);
}

/* "Cascade layers (like declarations) are sorted by order of appearance": `@layer a, b;` puts b after a whatever the order of the blocks (both browsers answer `2px`, the rule of b). */
TEST(CssomAtProperty, TheLayerOrderIsTheOrderTheLayersAreFirstNamedNotThePlaceOfTheRules) {
  Cs m(page("@layer a, b; @layer b { " + rule_p("--q", "2px") + " } @layer a { " + rule_p("--q", "1px") + " }"));
  EXPECT_EQ(m.value("#b", "--q"), "2px");
}

TEST(CssomAtProperty, ALayerStatementInAnEarlierSheetOrdersTheLayersOfALaterOne) {
  Cs m("<!DOCTYPE html><html><head><style>@layer a, b;</style><style>@layer b { " + rule_p("--q", "2px") + " } @layer a { " + rule_p("--q", "1px") + " }</style></head><body><p id=b>x</p></body></html>");
  EXPECT_EQ(m.value("#b", "--q"), "2px");
}

/* "with nested layers grouped within their parent layer": a layer's own rules come after its sub-layers (both browsers: `2px`, the parent's own rule). */
TEST(CssomAtProperty, ALayersOwnRuleWinsOverOneInItsSubLayer) {
  Cs m(page("@layer l1 { @layer inner { " + rule_p("--n", "1px") + " } " + rule_p("--n", "2px") + " }"));
  EXPECT_EQ(m.value("#b", "--n"), "2px");
}

TEST(CssomAtProperty, AnUnlayeredRuleWinsOverOneInALayerAndItsSubLayer) {
  Cs m(page("@layer l2 { " + rule_p("--o", "1px") + " } @layer l2.sub { " + rule_p("--o", "2px") + " } " + rule_p("--o", "3px")));
  EXPECT_EQ(m.value("#b", "--o"), "3px");
}

TEST(CssomAtProperty, AnAnonymousLayersRuleRegisters) {
  Cs m(page("@layer { " + rule_p("--an", "4px") + " }"));
  EXPECT_EQ(m.value("#b", "--an"), "4px");
}

/* Browsers: `@supports`, `@container`, `@scope` and `@starting-style` blocks hold registrations (a false `@supports` does not): 5px, 6px, 8px, 4px. */
TEST(CssomAtProperty, TheGroupRulesTheBrowsersEnterRegisterTheirRules) {
  Cs m(page("@supports (display: grid) { " + rule_p("--s", "5px") + " } @supports (display: nonsense) { " + rule_p("--n", "5px") + " } @container (min-width: 1px) { " + rule_p("--c", "6px") +
            " } @scope (div) { " + rule_p("--sc", "8px") + " } @starting-style { " + rule_p("--ss", "4px") + " }"));
  EXPECT_EQ(m.value("#b", "--s"), "5px");
  EXPECT_EQ(m.value("#b", "--n"), "");
  EXPECT_EQ(m.value("#b", "--c"), "6px");
  EXPECT_EQ(m.value("#b", "--sc"), "8px");
  EXPECT_EQ(m.value("#b", "--ss"), "4px");
}

TEST(CssomAtProperty, ARuleInsideAStyleRuleRegistersNothing) {
  Cs m(page("p { " + rule_p("--in", "1px") + " }"));
  EXPECT_EQ(m.value("#b", "--in"), "");
}

/* ---- the sheets change ---------------------------------------------------------------------------------------------------------------------------------- */

TEST(CssomAtProperty, ARuleInsertedIntoASheetRegistersAtTheNextRead) {
  Cs m(page("#a { --l: calc(1px + 2px) }"));
  EXPECT_EQ(m.value("#a", "--l"), "calc(1px + 2px)");
  GTEXT_CSSOM_Sheet * sh = m.sheet(m.el("#s"));
  const std::string rule = kLength;
  ASSERT_EQ(gtext_cssom_sheet_insert_rule(sh, rule.data(), rule.size(), 0, nullptr, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(m.value("#a", "--l"), "3px");
  EXPECT_EQ(m.value("#b", "--l"), "3px");
  ASSERT_EQ(gtext_cssom_sheet_delete_rule(sh, 0, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(m.value("#a", "--l"), "calc(1px + 2px)") << "the rule is gone, the property is unregistered";
  EXPECT_EQ(m.value("#b", "--l"), "calc(1px + 2px)");
  gtext_cssom_sheet_release(sh);
}

TEST(CssomAtProperty, ADisabledSheetsRulesAreNotRegisteredAndANewlyEnabledOneIs) {
  Cs m(page(std::string(kLength) + "#a { --l: calc(1px + 2px) }"));
  GTEXT_CSSOM_Sheet * sh = m.sheet(m.el("#s"));
  Decl before = m.computed("#a");
  EXPECT_EQ(get(before.d, "--l"), "3px");
  ASSERT_EQ(gtext_cssom_sheet_set_disabled(sh, true, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(m.value("#a", "--l"), "") << "the sheet takes no part, so its declaration is gone too";
  EXPECT_EQ(get(before.d, "--l"), "") << "a computed declaration that was read before reads again";
  ASSERT_EQ(gtext_cssom_sheet_set_disabled(sh, false, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(m.value("#a", "--l"), "3px");
  gtext_cssom_sheet_release(sh);
}

TEST(CssomAtProperty, ARemovedStyleElementUnregistersItsProperties) {
  Cs m(page(std::string(kLength) + "#a { --l: calc(1px + 2px) }"));
  EXPECT_EQ(m.value("#b", "--l"), "3px");
  detach(m.el("#s"));
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  EXPECT_EQ(m.value("#b", "--l"), "");
}

TEST(CssomAtProperty, ASheetWhoseMediaIsFalseRegistersNothingUntilTheMediaHolds) {
  Cs m(page(std::string(kLength), "<p id=b>x</p>", " media=\"(max-width: 0px)\""));
  EXPECT_EQ(m.value("#b", "--l"), "");
  set_attr(m.el("#s"), "media", "all");
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  EXPECT_EQ(m.value("#b", "--l"), "3px");
}

TEST(CssomAtProperty, ASecondSheetAddedLaterRegistersAfterTheFirst) {
  Cs m(page(std::string(kLength)));
  EXPECT_EQ(m.value("#b", "--l"), "3px");
  GTEXT_Markup_Node * st = make_style(m.markup(), "@property --l { syntax: \"<length>\"; inherits: false; initial-value: 8px }");
  ASSERT_NE(st, nullptr);
  append(m.el("head"), st);
  ASSERT_EQ(m.sync(), GTEXT_CSS_OK);
  EXPECT_EQ(m.value("#b", "--l"), "8px");
}

/* ---- the set, the counters, the caller's context --------------------------------------------------------------------------------------------------------- */

std::vector<std::string> registered(GTEXT_CSSOM_Document * d, bool * applied = nullptr, bool * none = nullptr) {
  const GTEXT_CSS_At_Property_Set * set = nullptr;
  EXPECT_EQ(gtext_cssom_document_property_rules(d, &set, applied), GTEXT_CSS_OK);
  if (none) {
    *none = set == nullptr;
  }
  std::vector<std::string> out;
  for (size_t i = 0; i < gtext_css_at_property_count(set); i++) {
    GTEXT_CSS_Custom_Info info;
    EXPECT_TRUE(gtext_css_at_property_at(set, i, &info));
    out.push_back(std::string(info.name, info.name_length) + " " + std::string(info.syntax, info.syntax_length) + " " + (info.inherits ? "inherits" : "own") + " " +
        (info.has_initial ? std::string(info.initial_value, info.initial_length) : "-"));
  }
  return out;
}

TEST(CssomAtProperty, TheDocumentExposesTheRegistrationsItsRulesMake) {
  Cs m(page(std::string(kLength) + "@property --c { syntax: \"*\"; inherits: true }"));
  bool applied = false, none = true;
  EXPECT_EQ(registered(m.d(), &applied, &none), (std::vector<std::string>{"--l <length> own 3px", "--c * inherits -"}));
  EXPECT_TRUE(applied);
  EXPECT_FALSE(none);
  const GTEXT_CSSOM_Counters c = m.dc();
  EXPECT_EQ(c.property_registrations, 2u);
  EXPECT_EQ(c.property_diagnostics, 0u);
  EXPECT_EQ(c.property_unapplied, 0u);
  EXPECT_GE(c.property_collects, 1u);
}

/* the I/O row: "invalid rule | no initial-value for <length> | not registered". Properties and Values 1, "the @property rule": a rule without the descriptors it requires "is invalid"; the
   parse of a sheet does not keep an invalid @property rule (it is not in `cssRules`, as in the browsers), so it never reaches the set and there is no diagnostic to report for it. What is
   reported is the valid rule that another replaced ("the last one wins"). */
TEST(CssomAtProperty, AnInvalidRuleIsNotInTheSheetAndRegistersNothing) {
  Cs m(page("@property --l { syntax: \"<length>\"; inherits: false } p { color: red }"));
  GTEXT_CSSOM_Sheet * sh = m.sheet(m.el("#s"));
  const GTEXT_CSSOM_Rule_List * list = nullptr;
  ASSERT_EQ(gtext_cssom_sheet_rules(sh, &list), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_cssom_rule_list_length(list), 1u) << "only the style rule";
  gtext_cssom_sheet_release(sh);
  bool none = false;
  EXPECT_TRUE(registered(m.d(), nullptr, &none).empty());
  EXPECT_TRUE(none);
}

TEST(CssomAtProperty, ARuleAnotherReplacedIsReportedAsSuperseded) {
  Cs m(page(std::string(kLength) + "@property --l { syntax: \"<length>\"; inherits: false; initial-value: 9px }"));
  const GTEXT_CSS_At_Property_Set * set = nullptr;
  ASSERT_EQ(gtext_cssom_document_property_rules(m.d(), &set, nullptr), GTEXT_CSS_OK);
  ASSERT_NE(set, nullptr);
  EXPECT_EQ(gtext_css_at_property_count(set), 1u);
  ASSERT_EQ(gtext_css_at_property_diagnostic_total(set), 1u);
  GTEXT_CSS_At_Property_Diagnostic diag;
  ASSERT_TRUE(gtext_css_at_property_diagnostic(set, 0, &diag));
  EXPECT_EQ(diag.reason, GTEXT_CSS_AT_PROP_SUPERSEDED);
  EXPECT_EQ(std::string(diag.name.data, diag.name.length), "--l");
  EXPECT_EQ(m.dc().property_diagnostics, 1u);
}

TEST(CssomAtProperty, ADocumentWithNoPropertyRuleHasNoSetAndPaysNothing) {
  Cs m(page("#a { --l: 1px; color: red } @media (min-width: 1px) { #b { color: blue } }"));
  EXPECT_EQ(m.value("#a", "color"), "rgb(255, 0, 0)");
  bool none = false;
  EXPECT_TRUE(registered(m.d(), nullptr, &none).empty());
  EXPECT_TRUE(none) << "no registration and no diagnostic: no set";
  Cs plain(page("#a { color: red } #b { color: blue }"));
  EXPECT_EQ(plain.value("#a", "color"), "rgb(255, 0, 0)");
  EXPECT_TRUE(registered(plain.d(), nullptr, &none).empty());
  EXPECT_TRUE(none);
  const GTEXT_CSSOM_Counters c = plain.dc();
  EXPECT_EQ(c.property_collects, 0u) << "a sheet of style rules alone is not even read";
  EXPECT_EQ(c.property_context_rebuilds, 0u);
  GTEXT_CSSOM_Sheet * sh = plain.sheet(plain.el("#s"));
  ASSERT_EQ(gtext_cssom_sheet_insert_rule(sh, "p{color:green}", 14, 0, nullptr, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(plain.value("#a", "color"), "rgb(255, 0, 0)");
  EXPECT_EQ(plain.dc().property_context_rebuilds, 0u) << "the context is not made again for a change that is no registration";
  gtext_cssom_sheet_release(sh);
}

TEST(CssomAtProperty, TheContextIsMadeAgainOnlyWhenTheRegistrationsChange) {
  Cs m(page(std::string(kLength) + "#a { --l: calc(1px + 2px) }"));
  EXPECT_EQ(m.value("#a", "--l"), "3px");
  EXPECT_EQ(m.dc().property_context_rebuilds, 0u);
  GTEXT_CSSOM_Sheet * sh = m.sheet(m.el("#s"));
  ASSERT_EQ(gtext_cssom_sheet_insert_rule(sh, "p{color:green}", 14, 1, nullptr, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(m.value("#a", "--l"), "3px");
  EXPECT_EQ(m.dc().property_context_rebuilds, 0u) << "an unrelated rule changes no registration";
  ASSERT_EQ(gtext_cssom_sheet_insert_rule(sh, "@property --m { syntax: \"<number>\"; inherits: false; initial-value: 1 }", 69, 0, nullptr, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(m.value("#a", "--m"), "1");
  EXPECT_EQ(m.dc().property_context_rebuilds, 1u);
  gtext_cssom_sheet_release(sh);
}

/* "A custom property registration from CSS.registerProperty() further wins over any @property rules for the same <custom-property-name>." */
TEST(CssomAtProperty, ARegistrationTheCallerMadeInTheRegistryWinsOverARule) {
  reg::Builder b;
  GTEXT_CSS_Custom_Def def;
  std::memset(&def, 0, sizeof def);
  def.name = "--l";
  def.syntax = "<length>";
  def.inherits = false;
  def.initial_value = "9px";
  GTEXT_CSS_Registry_Result rr;
  ASSERT_EQ(gtext_css_registry_register_custom(b.b, &def, &rr), GTEXT_CSS_OK);
  reg::Owned owned;
  ASSERT_EQ(b.freeze(owned), GTEXT_CSS_OK);
  Cs m(page(std::string(kLength) + "#a { --l: calc(1px + 2px) }"), [&](GTEXT_CSSOM_Config & c) { c.registry = owned.r; });
  EXPECT_EQ(m.value("#b", "--l"), "9px") << "the registry's initial value, not the rule's";
  EXPECT_EQ(m.value("#a", "--l"), "3px") << "and its syntax: the specified value is computed";
  const GTEXT_CSS_At_Property_Set * set = nullptr;
  ASSERT_EQ(gtext_cssom_document_property_rules(m.d(), &set, nullptr), GTEXT_CSS_OK);
  ASSERT_NE(set, nullptr);
  EXPECT_EQ(gtext_css_at_property_count(set), 0u);
  GTEXT_CSS_At_Property_Diagnostic diag;
  ASSERT_TRUE(gtext_css_at_property_diagnostic(set, 0, &diag));
  EXPECT_EQ(diag.reason, GTEXT_CSS_AT_PROP_CALLER_WINS);
}

TEST(CssomAtProperty, ACallerSuppliedComputeContextKeepsItsOwnRegistrationsAndTheRulesAreReported) {
  GTEXT_CSS_Compute_Context * ctx = nullptr;
  Cs m(page(std::string(kLength) + "#a { --l: calc(1px + 2px) }"), [&](GTEXT_CSSOM_Config & c) {
    GTEXT_CSS_Compute_Options co = gtext_css_compute_options_default();
    co.elements = c.element_interface;
    GTEXT_CSS_Environment env = gtext_css_environment_default();
    GTEXT_CSS_Compute_Result res;
    std::memset(&res, 0, sizeof res);
    ASSERT_EQ(gtext_css_compute_context_new(R(), &env, &co, &ctx, &res), GTEXT_CSS_OK);
    c.compute_context = ctx;
  });
  EXPECT_EQ(m.value("#a", "--l"), "calc(1px + 2px)") << "the caller's context has no registration of --l";
  bool applied = true;
  EXPECT_EQ(registered(m.d(), &applied), (std::vector<std::string>{"--l <length> own 3px"})) << "the rules are reported";
  EXPECT_FALSE(applied);
  const GTEXT_CSSOM_Counters c = m.dc();
  EXPECT_EQ(c.property_registrations, 1u);
  EXPECT_EQ(c.property_unapplied, 1u);
  EXPECT_EQ(c.property_context_rebuilds, 0u);
  /* back to the document's own context: the rules apply */
  ASSERT_EQ(gtext_cssom_document_set_compute_context(m.d(), nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(m.value("#a", "--l"), "3px");
  bool a2 = false;
  registered(m.d(), &a2);
  EXPECT_TRUE(a2);
  gtext_css_compute_context_free(ctx);
}

/* the interface of css_cssom_computed.h hands the compute context to a conditions object; a registration change makes another one, and the one handed out stays valid */
TEST(CssomAtProperty, AContextHandedToAConditionsObjectSurvivesAChangeOfTheRegistrations) {
  Cs m(page(std::string(kLength)));
  EXPECT_EQ(m.value("#b", "--l"), "3px");
  GTEXT_CSS_Container_Interface first;
  ASSERT_EQ(gtext_cssom_document_container_interface(m.d(), &first), GTEXT_CSS_OK);
  ASSERT_NE(first.compute_context, nullptr);
  GTEXT_CSSOM_Sheet * sh = m.sheet(m.el("#s"));
  ASSERT_EQ(gtext_cssom_sheet_delete_rule(sh, 0, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(m.value("#b", "--l"), "") << "the rule is gone";
  GTEXT_CSS_Container_Interface second;
  ASSERT_EQ(gtext_cssom_document_container_interface(m.d(), &second), GTEXT_CSS_OK);
  EXPECT_NE(second.compute_context, first.compute_context);
  /* the first is still a context a read can be made with (the sanitizers see a use after free here) */
  GTEXT_CSS_Computed_Style * style = nullptr;
  GTEXT_CSS_Style_Options so = gtext_css_style_options_default();
  GTEXT_CSS_Compute_Result res;
  std::memset(&res, 0, sizeof res);
  EXPECT_EQ(gtext_css_style_compute(first.compute_context, m.handle("#b"), nullptr, nullptr, 0, &so, &style, &res), GTEXT_CSS_OK);
  gtext_css_computed_free(style);
  gtext_cssom_sheet_release(sh);
}

/* A consumer with an environment of its own (a viewport, a palette) gives it to the document, which makes its context with it and with the registrations of the rules: the environment's
   callbacks are the caller's, and the registrations are the document's. */
TEST(CssomAtProperty, ADocumentMadeWithAnEnvironmentComputesItsOwnContextInThatEnvironmentWithTheRules) {
  struct Env {
    int asked = 0;
  } env_state;
  GTEXT_CSS_Environment env = gtext_css_environment_default();
  env.user = &env_state;
  env.resolve_color = [](void * user, const GTEXT_CSS_Color *, GTEXT_CSS_Color_Scheme, double rgba[4]) -> bool {
    static_cast<Env *>(user)->asked++;
    rgba[0] = 1; rgba[1] = 2; rgba[2] = 3; rgba[3] = 1;
    return true;
  };
  Cs m(page(std::string(kLength) + "#a { --l: calc(1px + 2px); color: canvastext }"), [&](GTEXT_CSSOM_Config & c) { c.environment = &env; });
  EXPECT_EQ(m.value("#a", "--l"), "3px") << "the rule is applied";
  EXPECT_EQ(m.value("#a", "color"), "rgb(1, 2, 3)") << "and the caller's palette answers";
  EXPECT_GT(env_state.asked, 0);
  bool applied = false;
  registered(m.d(), &applied);
  EXPECT_TRUE(applied);
  EXPECT_EQ(m.dc().property_unapplied, 0u);
}

/* ---- allocation failure ---------------------------------------------------------------------------------------------------------------------------------- */

std::string everything_custom(Cs & m) {
  return m.value("#a", "--l") + "|" + m.value("#b", "--l") + "|" + m.value("#a", "--r");
}

/* Every allocation of reading the rules and of making the context with them is failed in turn: a failed read is an error and leaves the document as it was (the next read, with memory,
   gives the healthy answer), and nothing leaks. */
TEST(CssomAtProperty, EveryAllocationOfReadingTheRulesAndMakingTheContextIsFailedInTurn) {
  const std::string html = page(std::string(kLength) + "@media (min-width: 1px) { @property --k { syntax: \"<number>\"; inherits: true; initial-value: 2 } }"
                                "#a { --l: calc(1px + 2px); --r: if(style(--l: 3px): T; else: F) }");
  std::string want;
  {
    Cs m(html);
    want = everything_custom(m);
    ASSERT_EQ(want, "3px|3px|T");
  }
  size_t failed = 0;
  for (size_t n = 1; n < 100000; n += n < 60 ? 1 : 31) {
    FaultAllocator fa;
    bool fired = false;
    {
      Cs m(html, nullptr, &fa.alloc);
      GTEXT_CSSOM_Decl * made = nullptr;
      EXPECT_EQ(gtext_cssom_computed_style(m.d(), m.handle("#a"), nullptr, 0, &made), GTEXT_CSS_OK);
      fa.arm(n);
      size_t len = 0;
      const GTEXT_CSS_Status st = gtext_cssom_decl_length(made, &len);
      fa.disarm();
      fired = fa.failed;
      if (st != GTEXT_CSS_OK) {
        failed++;
        EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
      }
      gtext_cssom_decl_release(made);
      EXPECT_EQ(everything_custom(m), want) << "allocation " << n << ": with memory again, the healthy answer";
    }
    EXPECT_TRUE(fa.live.empty()) << "a leak after failing allocation " << n;
    EXPECT_EQ(fa.bad_frees, 0u);
    if (!fired) {
      break;
    }
  }
  EXPECT_GT(failed, 20u) << "the sweep reached the allocations of the reading";
}

}  // namespace
