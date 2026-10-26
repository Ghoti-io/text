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
 * The CSS registry: the generated tables (what the pin says, and how the library consolidated it), the caller's registrations
 * with their override rules and limits, freezing, and the lazy compilation of grammars.
 *
 * The tables are checked against the properties of the data itself where a test can say them without copying the data: the
 * relations (an alias has a target, a longhand is a property), the order the lookups rely on, and the named facts the
 * consolidation rule and the normalization mapping decide (tools/css/webref_model.py, tools/css/normalize.tsv).
 */

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "css_registry_test_support.h"

using namespace reg;

namespace {

std::string str(const char * s) {
  return s ? s : "<null>";
}

// ---- the generated layer: relations and order -----------------------------------------------------------------

TEST(CssRegistryTables, ThePinIsNamedAndItsCountsAreTheMeasuredOnes) {
  EXPECT_EQ(std::string(gtext_css_registry_pin_commit()), "d20af004eff967a9b8e562ea3423c29402a5ce61");
  // 892 property entries in 822 names (the pin, measured when it was chosen)
  EXPECT_EQ(gtext_css_registry_generated_property_count(), 823u) << "the pin's 822 names and the custom property pattern (tools/css/supplement.tsv)";
  EXPECT_EQ(gtext_css_registry_at_rule_count(), 48u);
  EXPECT_EQ(gtext_css_registry_selector_count(), 159u);
  EXPECT_GT(gtext_css_registry_type_count(), 600u);
  EXPECT_GT(gtext_css_registry_descriptor_count(), 100u);
  EXPECT_GT(gtext_css_registry_keyword_count(), 1000u);
}

TEST(CssRegistryTables, PropertyNamesAreSortedLowerCaseAndUnique) {
  const GTEXT_CSS_Registry * r = shared();
  ASSERT_NE(r, nullptr);
  std::string prev;
  for (size_t i = 1; i <= gtext_css_registry_generated_property_count(); i++) {
    GTEXT_CSS_Property_Info info;
    ASSERT_TRUE(gtext_css_registry_property_info(r, (GTEXT_CSS_Property_Id)i, &info));
    const std::string name(info.name, info.name_length);
    EXPECT_LT(prev, name) << i;
    for (char c : name) {
      EXPECT_FALSE(c >= 'A' && c <= 'Z') << name;
    }
    EXPECT_NE(info.flags & GTEXT_CSS_PROP_GENERATED, 0u);
    // the lookup finds each one, in any case
    EXPECT_EQ(find(r, name), (GTEXT_CSS_Property_Id)i) << name;
    prev = name;
  }
}

TEST(CssRegistryTables, EveryRelationPointsAtAProperty) {
  const GTEXT_CSS_Registry * r = shared();
  size_t aliases = 0, shorthands = 0;
  for (size_t i = 1; i <= gtext_css_registry_generated_property_count(); i++) {
    GTEXT_CSS_Property_Info info;
    ASSERT_TRUE(gtext_css_registry_property_info(r, (GTEXT_CSS_Property_Id)i, &info));
    if (info.flags & GTEXT_CSS_PROP_ALIAS) {
      aliases++;
      ASSERT_NE(info.alias_of, 0u) << info.name;
      GTEXT_CSS_Property_Info t;
      ASSERT_TRUE(gtext_css_registry_property_info(r, info.alias_of, &t));
      EXPECT_EQ(gtext_css_registry_resolve_alias(r, (GTEXT_CSS_Property_Id)i), gtext_css_registry_resolve_alias(r, info.alias_of)) << info.name;
      EXPECT_EQ(info.grammar, nullptr) << info.name << ": an alias has its target's grammar and none of its own";
    }
    else {
      EXPECT_EQ(info.alias_of, 0u);
    }
    if (info.flags & GTEXT_CSS_PROP_SHORTHAND) {
      shorthands++;
    }
    for (size_t k = 0; k < info.longhand_count; k++) {
      EXPECT_NE(gtext_css_registry_longhand(r, (GTEXT_CSS_Property_Id)i, k), 0u) << info.name;
    }
    EXPECT_EQ(gtext_css_registry_longhand(r, (GTEXT_CSS_Property_Id)i, info.longhand_count), 0u);
    for (size_t k = 0; k < info.reset_longhand_count; k++) {
      EXPECT_NE(gtext_css_registry_reset_longhand(r, (GTEXT_CSS_Property_Id)i, k), 0u) << info.name;
    }
    for (size_t k = 0; k < info.style_name_count; k++) {
      EXPECT_NE(gtext_css_registry_style_name(r, (GTEXT_CSS_Property_Id)i, k), nullptr) << info.name;
    }
    if (!(info.flags & GTEXT_CSS_PROP_CUSTOM)) {
      EXPECT_GE(info.style_name_count, 1u) << info.name << ": every property has a CSSStyleDeclaration name";
    }
  }
  EXPECT_EQ(aliases, 64u);
  EXPECT_GT(shorthands, 80u);
}

TEST(CssRegistryTables, AliasesAreTheCompatibilityStandardsAndThePinsOwn) {
  const GTEXT_CSS_Registry * r = shared();
  const auto target = [&](const char * alias) {
    return gtext_css_registry_resolve_alias(r, find(r, alias));
  };
  EXPECT_EQ(target("-webkit-transform"), find(r, "transform"));
  EXPECT_EQ(target("-webkit-box-shadow"), find(r, "box-shadow"));
  EXPECT_EQ(target("word-wrap"), find(r, "overflow-wrap"));
  EXPECT_EQ(target("grid-gap"), find(r, "gap"));
  EXPECT_EQ(target("font-stretch"), find(r, "font-width"));
  EXPECT_EQ(target("-webkit-mask-box-image"), find(r, "mask-border"));
  EXPECT_EQ(target("margin"), find(r, "margin"));
  GTEXT_CSS_Property_Info i = info(r, "-webkit-transform");
  EXPECT_NE(i.flags & GTEXT_CSS_PROP_ALIAS, 0u);
  EXPECT_EQ(gtext_css_registry_resolve_alias(r, 0), 0u);
  EXPECT_EQ(gtext_css_registry_resolve_alias(r, 1u << 20), 0u);
  EXPECT_EQ(gtext_css_registry_resolve_alias(nullptr, 1), 0u);
}

TEST(CssRegistryTables, UnlistedVendorPrefixesAreUnknownProperties) {
  const GTEXT_CSS_Registry * r = shared();
  EXPECT_EQ(find(r, "-moz-nosuch"), 0u);
  EXPECT_EQ(find(r, "-webkit-nosuch"), 0u);
  EXPECT_EQ(find(r, "-moz-appearance"), 0u) << "the pin lists -webkit-appearance and no -moz- name";
  EXPECT_NE(find(r, "-webkit-appearance"), 0u);
  EXPECT_EQ(find(r, ""), 0u);
  EXPECT_EQ(gtext_css_registry_find_property(r, nullptr, 3), 0u);
  EXPECT_EQ(gtext_css_registry_find_property(nullptr, "color", 5), 0u);
  EXPECT_EQ(gtext_css_registry_find_property(r, "color", 0), 0u);
  const std::string long_name(5000, 'a');
  EXPECT_EQ(find(r, long_name), 0u);
}

TEST(CssRegistryTables, NamesAreCaseInsensitiveAndCustomPropertiesAreNot) {
  const GTEXT_CSS_Registry * r = shared();
  EXPECT_EQ(find(r, "COLOR"), find(r, "color"));
  EXPECT_EQ(find(r, "Margin-Top"), find(r, "margin-top"));
  const GTEXT_CSS_Property_Id custom = find(r, "--my-var");
  ASSERT_NE(custom, 0u) << "a custom property is the pattern --*";
  EXPECT_EQ(find(r, "--MY-VAR"), custom);
  GTEXT_CSS_Property_Info i = info(r, "--x");
  EXPECT_EQ(std::string(i.name, i.name_length), "--*");
  EXPECT_NE(i.flags & GTEXT_CSS_PROP_CUSTOM, 0u);
  EXPECT_EQ(find(r, "--"), 0u) << "`--` alone is reserved";
  EXPECT_EQ(find(r, "-"), 0u);
}

TEST(CssRegistryTables, ShorthandsAndTheirLonghands) {
  const GTEXT_CSS_Registry * r = shared();
  const GTEXT_CSS_Property_Info margin = info(r, "margin");
  EXPECT_NE(margin.flags & GTEXT_CSS_PROP_SHORTHAND, 0u);
  ASSERT_EQ(margin.longhand_count, 4u);
  const char * want[] = {"margin-top", "margin-right", "margin-bottom", "margin-left"};
  for (size_t k = 0; k < 4; k++) {
    EXPECT_EQ(gtext_css_registry_longhand(r, margin.id, k), find(r, want[k])) << k;
  }
  // reset-only longhands: font resets properties it does not list as longhands
  const GTEXT_CSS_Property_Info font = info(r, "font");
  EXPECT_GE(font.longhand_count, 7u);
  EXPECT_GE(font.reset_longhand_count, 6u);
  std::set<GTEXT_CSS_Property_Id> resets;
  for (size_t k = 0; k < font.reset_longhand_count; k++) {
    resets.insert(gtext_css_registry_reset_longhand(r, font.id, k));
  }
  EXPECT_TRUE(resets.count(find(r, "font-kerning")));
  EXPECT_TRUE(resets.count(find(r, "font-feature-settings")));
  for (size_t k = 0; k < font.longhand_count; k++) {
    EXPECT_FALSE(resets.count(gtext_css_registry_longhand(r, font.id, k))) << "a longhand is not also a reset-only longhand";
  }
  // `all` is a shorthand with no list
  const GTEXT_CSS_Property_Info all = info(r, "all");
  EXPECT_NE(all.flags & GTEXT_CSS_PROP_SHORTHAND, 0u);
  EXPECT_EQ(all.longhand_count, 0u);
  EXPECT_EQ(all.inherited, GTEXT_CSS_INHERIT_MIXED);
}

TEST(CssRegistryTables, LogicalGroups) {
  const GTEXT_CSS_Registry * r = shared();
  const GTEXT_CSS_Property_Info top = info(r, "margin-top");
  ASSERT_NE(top.logical_group, nullptr);
  EXPECT_EQ(std::string(top.logical_group), "margin");
  const size_t n = gtext_css_registry_logical_group_size("margin", 6);
  EXPECT_GE(n, 8u) << "margin-top/-right/-bottom/-left and the four logical ones";
  std::set<GTEXT_CSS_Property_Id> members;
  for (size_t k = 0; k < n; k++) {
    members.insert(gtext_css_registry_logical_group_member(r, "margin", 6, k));
  }
  EXPECT_EQ(members.size(), n);
  EXPECT_TRUE(members.count(find(r, "margin-top")));
  EXPECT_TRUE(members.count(find(r, "margin-block-start")));
  EXPECT_FALSE(members.count(find(r, "padding-top")));
  EXPECT_EQ(gtext_css_registry_logical_group_member(r, "margin", 6, n), 0u);
  EXPECT_EQ(gtext_css_registry_logical_group_size("nosuch", 6), 0u);
  EXPECT_EQ(gtext_css_registry_logical_group_size(nullptr, 6), 0u);
}

// ---- consolidation (tools/css/webref_model.py) --------------------------------------------------------------------

TEST(CssRegistryConsolidation, TheHighestLevelOfASeriesGivesTheBaseDefinition) {
  const GTEXT_CSS_Registry * r = shared();
  // css-animations-2 is level 2 of css-animations: its `<time>` range differs from level 1's
  const GTEXT_CSS_Property_Info d = info(r, "animation-duration");
  EXPECT_EQ(str(d.source), "css-animations-2");
  EXPECT_EQ(str(info(r, "animation-name").source), "css-animations") << "defined at level 1 only";
  EXPECT_EQ(str(info(r, "margin-top").source), "css-box");
  EXPECT_EQ(str(info(r, "text-align").source), "css-text-4");
  EXPECT_EQ(str(info(r, "background-color").source), "css-backgrounds-4");
}

TEST(CssRegistryConsolidation, NewValuesExtendTheBaseInSeriesOrder) {
  const GTEXT_CSS_Registry * r = shared();
  // css-sizing gives `auto | <box-size>`; css-anchor-position adds `<anchor-size()>`
  EXPECT_EQ(str(info(r, "width").grammar), "auto | <box-size> | <anchor-size()>");
  // css-display, then css-grid-3 and mathml-core append (series alphabetical: css-grid before mathml-core)
  const std::string display = str(info(r, "display").grammar);
  EXPECT_NE(display.find("<display-box> | <display-legacy> | grid-lanes | inline-grid-lanes | <display-outside> || [ <display-inside> | math ]"),
      std::string::npos) << display;
  EXPECT_NE(str(info(r, "justify-self").grammar).find(" | anchor-center"), std::string::npos);
  // the order is the series order and is not reversed: the base, then css-grid-3's, then mathml-core's, each after the one before
  const size_t lanes = display.find(" | grid-lanes | inline-grid-lanes");
  const size_t math = display.find(" | <display-outside> || [ <display-inside> | math ]");
  ASSERT_NE(lanes, std::string::npos) << display;
  ASSERT_NE(math, std::string::npos) << display;
  EXPECT_LT(lanes, math) << display;
  const std::string tail = " | <display-outside> || [ <display-inside> | math ]";
  ASSERT_GE(display.size(), tail.size());
  EXPECT_EQ(display.compare(display.size() - tail.size(), tail.size(), tail), 0) << "mathml-core's alternative is the last: " << display;
}

TEST(CssRegistryConsolidation, AFieldTheBaseLacksIsAbsentAndNotTakenFromAnotherLevel) {
  const GTEXT_CSS_Registry * r = shared();
  // css-overflow-5 names `auto` as the initial value of `continue` and has no grammar of its own; the base keeps `normal`
  const GTEXT_CSS_Property_Info c = info(r, "continue");
  EXPECT_EQ(str(c.initial), "normal");
  EXPECT_EQ(str(c.grammar), "normal | discard | collapse | -webkit-legacy | overflow | paginate | fragments");
  // the pin's `column-width` and the legacy -webkit-user-select have a grammar and no initial value
  EXPECT_EQ(info(r, "column-width").initial, nullptr);
  EXPECT_EQ(info(r, "-webkit-user-select").initial, nullptr);
  EXPECT_EQ(info(r, "-webkit-user-select").inherited, GTEXT_CSS_INHERIT_UNKNOWN);
  EXPECT_EQ(info(r, "-webkit-user-select").animation, GTEXT_CSS_ANIM_UNSPECIFIED);
}

TEST(CssRegistryConsolidation, ANameWithoutAGrammarIsListedAsSuch) {
  const GTEXT_CSS_Registry * r = shared();
  for (const char * name : {"-webkit-box-align", "-webkit-box-flex", "-webkit-box-ordinal-group", "-webkit-box-orient", "-webkit-box-pack"}) {
    const GTEXT_CSS_Property_Info i = info(r, name);
    EXPECT_NE(i.flags & GTEXT_CSS_PROP_NO_GRAMMAR, 0u) << name;
    EXPECT_EQ(i.grammar, nullptr) << name;
  }
}

TEST(CssRegistryConsolidation, ProseFieldsAreMappedToClosedEnumsAndKeptAsWritten) {
  const GTEXT_CSS_Registry * r = shared();
  const GTEXT_CSS_Property_Info z = info(r, "z-index");
  EXPECT_EQ(z.inherited, GTEXT_CSS_INHERIT_NO);
  EXPECT_EQ(str(z.inherited_raw), "no");
  EXPECT_EQ(info(r, "color").inherited, GTEXT_CSS_INHERIT_YES);
  EXPECT_EQ(info(r, "fill-break").inherited, GTEXT_CSS_INHERIT_YES) << "`yes?`";
  EXPECT_EQ(str(info(r, "fill-break").inherited_raw), "yes?");
  EXPECT_EQ(info(r, "stroke-break").inherited, GTEXT_CSS_INHERIT_UNKNOWN) << "`?`";
  EXPECT_EQ(info(r, "text-decoration-line").inherited, GTEXT_CSS_INHERIT_NO) << "`no (but see prose, above)`";
  EXPECT_EQ(info(r, "opacity").animation, GTEXT_CSS_ANIM_BY_COMPUTED_VALUE);
  EXPECT_EQ(info(r, "display").animation, GTEXT_CSS_ANIM_PROSE);
  EXPECT_EQ(info(r, "flex-shrink").animation, GTEXT_CSS_ANIM_NUMBER);
  EXPECT_EQ(info(r, "text-shadow").animation, GTEXT_CSS_ANIM_SHADOW_LIST);
  EXPECT_EQ(str(info(r, "flex-shrink").animation_raw), "number");
}

TEST(CssRegistryConsolidation, AShorthandWhoseInheritanceThePinLeavesToItsLonghandsDerivesIt) {
  const GTEXT_CSS_Registry * r = shared();
  EXPECT_EQ(str(info(r, "border-color").inherited_raw), "see individual properties");
  EXPECT_EQ(info(r, "border-color").inherited, GTEXT_CSS_INHERIT_NO);
  EXPECT_EQ(info(r, "text-emphasis").inherited, GTEXT_CSS_INHERIT_YES);
  EXPECT_EQ(info(r, "margin").inherited, GTEXT_CSS_INHERIT_NO);
  EXPECT_EQ(info(r, "font").inherited, GTEXT_CSS_INHERIT_YES);
  // longhands that differ: the shorthand is MIXED (line-clamp sets max-lines, which is not inherited, and block-ellipsis, which is)
  EXPECT_EQ(info(r, "line-clamp").inherited, GTEXT_CSS_INHERIT_MIXED);
  EXPECT_EQ(info(r, "max-lines").inherited, GTEXT_CSS_INHERIT_NO);
  EXPECT_EQ(info(r, "block-ellipsis").inherited, GTEXT_CSS_INHERIT_YES);
  EXPECT_EQ(info(r, "border-color").animation, GTEXT_CSS_ANIM_SEE_LONGHANDS);
  EXPECT_EQ(info(r, "animation").animation, GTEXT_CSS_ANIM_NOT_ANIMATABLE);
  // an alias reports its target's inheritance and none of the other data
  EXPECT_EQ(info(r, "-webkit-transform").inherited, GTEXT_CSS_INHERIT_NO);
  EXPECT_EQ(info(r, "-webkit-transform").initial, nullptr);
}

TEST(CssRegistryConsolidation, TheQuirksListIsValuesFourAppendixC) {
  const GTEXT_CSS_Registry * r = shared();
  size_t n = 0;
  for (size_t i = 1; i <= gtext_css_registry_generated_property_count(); i++) {
    GTEXT_CSS_Property_Info p;
    gtext_css_registry_property_info(r, (GTEXT_CSS_Property_Id)i, &p);
    if (p.flags & GTEXT_CSS_PROP_QUIRK_UNITLESS) {
      n++;
    }
  }
  EXPECT_EQ(n, 33u);
  for (const char * name : {"width", "height", "margin", "margin-top", "padding", "font-size", "background-position", "border-width", "vertical-align",
           "word-spacing", "text-indent", "clip", "letter-spacing", "border-spacing"}) {
    EXPECT_NE(info(r, name).flags & GTEXT_CSS_PROP_QUIRK_UNITLESS, 0u) << name;
  }
  for (const char * name : {"background", "border", "line-height", "flex-basis", "margin-inline", "gap", "top-left", "border-radius"}) {
    EXPECT_EQ(info(r, name).flags & GTEXT_CSS_PROP_QUIRK_UNITLESS, 0u) << name << ": not in the list";
  }
}

// ---- at-rules, descriptors, types, selectors, keywords -----------------------------------------------------------

TEST(CssRegistryAtRules, DescriptorsPerAtRule) {
  const GTEXT_CSS_At_Rule_Id face = gtext_css_registry_find_at_rule("@font-face", 10);
  ASSERT_NE(face, 0u);
  EXPECT_EQ(gtext_css_registry_find_at_rule("@FONT-FACE", 10), face);
  EXPECT_EQ(gtext_css_registry_find_at_rule("font-face", 9), 0u) << "the name includes the at sign";
  GTEXT_CSS_At_Rule_Info ai;
  ASSERT_TRUE(gtext_css_registry_at_rule_info(face, &ai));
  EXPECT_TRUE(ai.declarations);
  EXPECT_GE(ai.descriptor_count, 20u);
  const GTEXT_CSS_Descriptor_Id src = gtext_css_registry_find_descriptor(face, "SRC", 3);
  ASSERT_NE(src, 0u);
  GTEXT_CSS_Descriptor_Info di;
  ASSERT_TRUE(gtext_css_registry_descriptor_info(src, &di));
  EXPECT_EQ(di.at_rule, face);
  EXPECT_EQ(str(di.grammar), "<font-src-list>");
  EXPECT_EQ(di.kind, GTEXT_CSS_DESC_DECLARATION);
  // the highest level of css-fonts gives the descriptor; css-fonts-5 adds those it adds
  const GTEXT_CSS_Descriptor_Id range = gtext_css_registry_find_descriptor(face, "unicode-range", 13);
  ASSERT_NE(range, 0u);
  gtext_css_registry_descriptor_info(range, &di);
  EXPECT_EQ(str(di.grammar), "<unicode-range-token>#");
  EXPECT_EQ(str(di.initial), "U+0-10FFFF");
  EXPECT_EQ(gtext_css_registry_find_descriptor(face, "width", 5), 0u);
  EXPECT_EQ(gtext_css_registry_find_descriptor(0, "src", 3), 0u);
  // the descriptors of one at-rule are contiguous and sorted
  std::string prev;
  for (size_t k = 0; k < ai.descriptor_count; k++) {
    const GTEXT_CSS_Descriptor_Id id = gtext_css_registry_descriptor_at(face, k);
    ASSERT_NE(id, 0u);
    gtext_css_registry_descriptor_info(id, &di);
    EXPECT_LT(prev, std::string(di.name, di.name_length));
    prev.assign(di.name, di.name_length);
  }
  EXPECT_EQ(gtext_css_registry_descriptor_at(face, ai.descriptor_count), 0u);
}

TEST(CssRegistryAtRules, FeaturesAndNestedAtRulesAreKinds) {
  const GTEXT_CSS_At_Rule_Id media = gtext_css_registry_find_at_rule("@media", 6);
  ASSERT_NE(media, 0u);
  GTEXT_CSS_At_Rule_Info ai;
  gtext_css_registry_at_rule_info(media, &ai);
  EXPECT_FALSE(ai.declarations) << "a @media block holds rules and declarations of properties; its descriptors are features";
  GTEXT_CSS_Descriptor_Info di;
  gtext_css_registry_descriptor_info(gtext_css_registry_find_descriptor(media, "width", 5), &di);
  EXPECT_EQ(di.kind, GTEXT_CSS_DESC_FEATURE_RANGE);
  gtext_css_registry_descriptor_info(gtext_css_registry_find_descriptor(media, "orientation", 11), &di);
  EXPECT_EQ(di.kind, GTEXT_CSS_DESC_FEATURE_DISCRETE);
  const GTEXT_CSS_At_Rule_Id values = gtext_css_registry_find_at_rule("@font-feature-values", 20);
  ASSERT_NE(values, 0u);
  gtext_css_registry_descriptor_info(gtext_css_registry_find_descriptor(values, "@swash", 6), &di);
  EXPECT_EQ(di.kind, GTEXT_CSS_DESC_AT_RULE);
  EXPECT_EQ(str(di.grammar), "@swash { <declaration-list> }");
  // features from several series: mediaqueries-5 (level 5) and compat
  EXPECT_NE(gtext_css_registry_find_descriptor(media, "-webkit-device-pixel-ratio", 26), 0u);
  EXPECT_NE(gtext_css_registry_find_descriptor(media, "prefers-color-scheme", 20), 0u);
  EXPECT_EQ(gtext_css_registry_find_at_rule("@nosuch", 7), 0u);
}

TEST(CssRegistryAtRules, EveryDescriptorBelongsToAnAtRuleAndEveryAtRuleCountsItsOwn) {
  size_t total = 0;
  for (size_t a = 1; a <= gtext_css_registry_at_rule_count(); a++) {
    GTEXT_CSS_At_Rule_Info ai;
    ASSERT_TRUE(gtext_css_registry_at_rule_info((GTEXT_CSS_At_Rule_Id)a, &ai));
    for (size_t k = 0; k < ai.descriptor_count; k++) {
      GTEXT_CSS_Descriptor_Info di;
      ASSERT_TRUE(gtext_css_registry_descriptor_info(gtext_css_registry_descriptor_at((GTEXT_CSS_At_Rule_Id)a, k), &di));
      EXPECT_EQ(di.at_rule, a);
      total++;
    }
  }
  EXPECT_EQ(total, gtext_css_registry_descriptor_count());
  GTEXT_CSS_Descriptor_Info di;
  EXPECT_FALSE(gtext_css_registry_descriptor_info(0, &di));
  EXPECT_FALSE(gtext_css_registry_descriptor_info((GTEXT_CSS_Descriptor_Id)(total + 1), &di));
  GTEXT_CSS_At_Rule_Info ai;
  EXPECT_FALSE(gtext_css_registry_at_rule_info(0, &ai));
}

TEST(CssRegistryTypes, GlobalAndScopedAndNative) {
  GTEXT_CSS_Type_Info t;
  ASSERT_TRUE(gtext_css_registry_find_type("length-percentage", 17, 0, &t));
  EXPECT_EQ(str(t.grammar), "[ <length> | <percentage> ]");
  EXPECT_EQ(t.scope, 0u);
  EXPECT_NE(t.flags & GTEXT_CSS_TYPE_NATIVE, 0u) << "the engine implements <length-percentage> in code; the pin's text is not used";
  ASSERT_TRUE(gtext_css_registry_find_type("rgb()", 5, 0, &t));
  EXPECT_NE(t.flags & GTEXT_CSS_TYPE_FUNCTION, 0u);
  EXPECT_FALSE(gtext_css_registry_find_type("nosuch", 6, 0, &t));
  EXPECT_FALSE(gtext_css_registry_find_type("LENGTH", 6, 0, &t)) << "type names are case-sensitive";
  EXPECT_TRUE(gtext_css_registry_find_type("CDO-token", 9, 0, &t));
  // the type the pin nests inside a property is found for that property
  const GTEXT_CSS_Registry * r = shared();
  ASSERT_TRUE(gtext_css_registry_find_type("rect()", 6, 0, &t)) << "the global rect() is the basic shape's, which the pin nests in <basic-shape>";
  EXPECT_NE(str(t.grammar).find("round"), std::string::npos);
  ASSERT_TRUE(gtext_css_registry_find_type("rect()", 6, find(r, "clip"), &t));
  EXPECT_EQ(t.scope, find(r, "clip"));
  EXPECT_NE(t.flags & GTEXT_CSS_TYPE_SCOPED, 0u);
  EXPECT_EQ(str(t.grammar), "rect( <top>, <right>, <bottom>, <left> )");
  EXPECT_FALSE(gtext_css_registry_find_type("rect()", 6, find(r, "width"), &t) && t.scope != 0) << "no definition of the name is scoped to width";
  // the order the lookup relies on
  std::string prev;
  GTEXT_CSS_Property_Id prev_scope = 0;
  for (size_t i = 0; i < gtext_css_registry_type_count(); i++) {
    ASSERT_TRUE(gtext_css_registry_type_at(i, &t));
    const std::string name(t.name, t.name_length);
    if (name == prev) {
      EXPECT_LT(prev_scope, t.scope) << name;
    }
    else {
      EXPECT_LT(prev, name);
    }
    prev = name;
    prev_scope = t.scope;
  }
  EXPECT_FALSE(gtext_css_registry_type_at(gtext_css_registry_type_count(), &t));
}

TEST(CssRegistryTypes, ThePinsDepartureListIsApplied) {
  const GTEXT_CSS_Registry * r = shared();
  GTEXT_CSS_Type_Info t;
  // ignore-grammar: the pin's `minmax(min, max)` is a prototype, not a grammar
  ASSERT_TRUE(gtext_css_registry_find_type("minmax()", 8, find(r, "grid-template-columns"), &t));
  EXPECT_EQ(t.grammar, nullptr);
  EXPECT_NE(t.flags & GTEXT_CSS_TYPE_NO_GRAMMAR, 0u);
  // enum-from-values: the type the pin lists only through its keyword values
  ASSERT_TRUE(gtext_css_registry_find_type("animation-action", 16, 0, &t));
  EXPECT_EQ(str(t.grammar), "none | play | play-once | play-forwards | play-backwards | pause | reset | replay");
  ASSERT_TRUE(gtext_css_registry_find_type("timeline-range-center-subject", 29, 0, &t));
  EXPECT_EQ(str(t.grammar), "source | target");
  // a function whose value is its own name is prose
  ASSERT_TRUE(gtext_css_registry_find_type("click()", 7, 0, &t));
  EXPECT_EQ(t.grammar, nullptr);
}

// ---- the types the pin nests inside something that is not a property -------------------------------------------

/** The registry's grammar state of the global type of that name. */
GTEXT_CSS_Grammar_State type_state_of(const GTEXT_CSS_Registry * r, const char * name) {
  for (size_t i = 0; i < gtext_css_registry_type_count(); i++) {
    GTEXT_CSS_Type_Info t;
    gtext_css_registry_type_at(i, &t);
    if (t.scope == 0 && std::string(t.name, t.name_length) == name) {
      GTEXT_CSS_Grammar_State s = GTEXT_CSS_GRAMMAR_NONE;
      EXPECT_EQ(gtext_css_registry_type_state(r, i, &s, nullptr), GTEXT_CSS_OK);
      return s;
    }
  }
  ADD_FAILURE() << "no global type " << name;
  return GTEXT_CSS_GRAMMAR_NONE;
}

TEST(CssRegistryNestedTypes, ATypeNestedInAnotherTypeIsAGlobalDefinition) {
  GTEXT_CSS_Type_Info t;
  // <alpha-value> is nested in the entry of <color>; alpha() names it
  ASSERT_TRUE(gtext_css_registry_find_type("alpha-value", 11, 0, &t));
  EXPECT_EQ(str(t.grammar), "<number> | <percentage>");
  EXPECT_EQ(t.scope, 0u);
  EXPECT_EQ(t.flags & GTEXT_CSS_TYPE_SCOPED, 0u);
  EXPECT_EQ(type_state_of(shared(), "alpha-value"), GTEXT_CSS_GRAMMAR_OK);
  // <keypress()> is nested in <event-function>; the functions beside it have no grammar in the pin
  ASSERT_TRUE(gtext_css_registry_find_type("keypress()", 10, 0, &t));
  EXPECT_EQ(str(t.grammar), "keypress( <string>? )");
  EXPECT_NE(t.flags & GTEXT_CSS_TYPE_FUNCTION, 0u);
  ASSERT_TRUE(gtext_css_registry_find_type("click()", 7, 0, &t));
  EXPECT_EQ(t.grammar, nullptr);
  // a type nested two deep: the command types of shape(), which is nested in <basic-shape>
  ASSERT_TRUE(gtext_css_registry_find_type("shape()", 7, 0, &t));
  EXPECT_NE(str(t.grammar).find("<shape-command>"), std::string::npos);
  ASSERT_TRUE(gtext_css_registry_find_type("arc-command", 11, 0, &t));
  EXPECT_NE(str(t.grammar).find("arc <command-end-point>"), std::string::npos);
  ASSERT_TRUE(gtext_css_registry_find_type("relative-control-point", 22, 0, &t));
  EXPECT_EQ(str(t.grammar), "<coordinate-pair> [ from [ start | end | origin ] ]?");
  // the basic shapes the library defines in code keep their own definition: the pin's text is a global entry, and the library's function
  // types are asked first
  ASSERT_TRUE(gtext_css_registry_find_type("inset()", 7, 0, &t));
  EXPECT_NE(str(t.grammar).find("round"), std::string::npos);
}

TEST(CssRegistryNestedTypes, ATypeNestedInADescriptorOrAnAtRuleIsFoundToo) {
  GTEXT_CSS_Type_Info t;
  // <page-size> is nested in `size` of @page
  ASSERT_TRUE(gtext_css_registry_find_type("page-size", 9, 0, &t));
  EXPECT_NE(str(t.grammar).find("A4"), std::string::npos);
  EXPECT_EQ(type_state_of(shared(), "page-size"), GTEXT_CSS_GRAMMAR_OK);
  // the descriptor that names it now compiles
  const GTEXT_CSS_Registry * r = shared();
  GTEXT_CSS_Grammar_State s;
  const GTEXT_CSS_At_Rule_Id page = gtext_css_registry_find_at_rule("@page", 5);
  ASSERT_EQ(gtext_css_registry_descriptor_state(r, gtext_css_registry_find_descriptor(page, "size", 4), &s, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(s, GTEXT_CSS_GRAMMAR_OK);
  // <font-feature-value-type> is nested in @font-feature-values, which has no grammar for it
  ASSERT_TRUE(gtext_css_registry_find_type("font-feature-value-type", 23, 0, &t));
  EXPECT_EQ(t.grammar, nullptr);
  EXPECT_NE(t.flags & GTEXT_CSS_TYPE_NO_GRAMMAR, 0u);
}

TEST(CssRegistryNestedTypes, AFunctionTheTreeNestsInSeveralEntriesIsOneEntryWhenTheyAgreeAndTheDisagreeingCopyIsListed) {
  GTEXT_CSS_Type_Info t;
  // type() is nested in @function and attr() as `type( <syntax> )` and in image-set() as `type( <string> )` (a different thing):
  // consolidate.tsv drops the image-set() copy (nested-ignore) and the global entry is the one <css-type> names
  ASSERT_TRUE(gtext_css_registry_find_type("type()", 6, 0, &t));
  EXPECT_EQ(str(t.grammar), "type( <syntax> )");
  EXPECT_EQ(type_state_of(shared(), "css-type"), GTEXT_CSS_GRAMMAR_OK) << "<css-type> names <type()>";
  // repeat() is nested in nine entries that say the same thing (nothing: prose), and is one entry
  size_t seen = 0;
  for (size_t i = 0; i < gtext_css_registry_type_count(); i++) {
    ASSERT_TRUE(gtext_css_registry_type_at(i, &t));
    if (std::string(t.name, t.name_length) == "repeat()" && t.scope == 0) {
      seen++;
    }
  }
  EXPECT_EQ(seen, 1u);
  // leader() is a type of its own and also nested in <content-list>, the same text: one entry
  seen = 0;
  for (size_t i = 0; i < gtext_css_registry_type_count(); i++) {
    ASSERT_TRUE(gtext_css_registry_type_at(i, &t));
    seen += std::string(t.name, t.name_length) == "leader()";
  }
  EXPECT_EQ(seen, 1u);
}

TEST(CssRegistryNestedTypes, ThePropertiesThatNameANestedTypeAreDecided) {
  // link-parameters names <css-type> through param(), and <css-type> names <type()>, which the pin nests in @function, attr() and
  // image-set(): until the nested types were read the property could not be judged at all
  const GTEXT_CSS_Registry * r = shared();
  GTEXT_CSS_Grammar_State s;
  ASSERT_EQ(gtext_css_registry_property_state(r, find(r, "link-parameters"), &s, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(s, GTEXT_CSS_GRAMMAR_OK);
  EXPECT_EQ(parse(r, "link-parameters", "none"), "valid:none");
  // (what the value is written as is the serialization layer's: these only hold that the pin's text is now judged)
  EXPECT_EQ(parse(r, "link-parameters", "param(--a type(<color>), orange)").substr(0, 6), "valid:");
  EXPECT_EQ(parse(r, "link-parameters", "param(--a type(<color> | <length>), orange), param(--b, blue)").substr(0, 6), "valid:");
  EXPECT_EQ(parse(r, "link-parameters", "param(a, orange)"), "invalid") << "the name is a <dashed-ident>";
}

TEST(CssRegistrySelectorsAndKeywords, TheyAreSortedAndFound) {
  GTEXT_CSS_Selector_Info s;
  ASSERT_TRUE(gtext_css_registry_find_selector(":FIRST-LINE", 11, &s));
  EXPECT_EQ(std::string(s.name, s.name_length), ":first-line");
  EXPECT_TRUE(gtext_css_registry_find_selector("::before", 8, &s));
  EXPECT_TRUE(gtext_css_registry_find_selector("&", 1, &s));
  EXPECT_FALSE(gtext_css_registry_find_selector(":nosuch", 7, &s));
  std::string prev;
  for (size_t i = 0; i < gtext_css_registry_selector_count(); i++) {
    ASSERT_TRUE(gtext_css_registry_selector_at(i, &s));
    EXPECT_LT(prev, std::string(s.name, s.name_length));
    prev.assign(s.name, s.name_length);
  }
  EXPECT_FALSE(gtext_css_registry_selector_at(gtext_css_registry_selector_count(), &s));

  const uint32_t id = gtext_css_registry_keyword_lookup("AUTO", 4);
  ASSERT_NE(id, 0u);
  size_t len = 0;
  EXPECT_EQ(std::string(gtext_css_registry_keyword_at(id - 1, &len)), "auto");
  EXPECT_EQ(len, 4u);
  EXPECT_EQ(gtext_css_registry_keyword_lookup("nosuchkeyword", 13), 0u);
  EXPECT_NE(gtext_css_registry_keyword_lookup("space-between", 13), 0u);
  EXPECT_EQ(gtext_css_registry_keyword_at(gtext_css_registry_keyword_count(), &len), nullptr);
  // every keyword is lower case, once, in order
  prev.clear();
  for (size_t i = 0; i < gtext_css_registry_keyword_count(); i++) {
    const std::string k = gtext_css_registry_keyword_at(i, nullptr);
    EXPECT_LT(prev, k);
    EXPECT_EQ(gtext_css_registry_keyword_lookup(k.data(), k.size()), i + 1);
    prev = k;
  }
}

TEST(CssRegistryTypes, EveryKeywordOfAGrammarIsInTheAtomTable) {
  const GTEXT_CSS_Registry * r = shared();
  for (const char * name : {"display", "flex-direction", "text-align", "cursor", "list-style-type"}) {
    const GTEXT_CSS_Property_Info i = info(r, name);
    ASSERT_NE(i.grammar, nullptr) << name;
    const std::string g = i.grammar;
    size_t pos = 0;
    while (pos < g.size()) {
      if (g[pos] == '<') {
        pos = g.find('>', pos);
        if (pos == std::string::npos) break;
        pos++;
        continue;
      }
      size_t e = pos;
      while (e < g.size() && (std::isalnum((unsigned char)g[e]) || g[e] == '-')) e++;
      if (e > pos && std::isalpha((unsigned char)g[pos]) && (e >= g.size() || g[e] != '(')) {
        EXPECT_NE(gtext_css_registry_keyword_lookup(g.data() + pos, e - pos), 0u) << name << ": " << g.substr(pos, e - pos);
      }
      pos = e > pos ? e : pos + 1;
    }
  }
}

// ---- the units and functions of the css-values definitions ----------------------------------------------------------

TEST(CssRegistryPin, EveryUnitTheValuesDefinitionsListIsAUnitOfTheLibrary) {
  std::set<std::string> pin;
  for (size_t i = 0; i < gtext_css_registry_pin_unit_count(); i++) {
    const char * name;
    const char * measures;
    ASSERT_TRUE(gtext_css_registry_pin_unit_at(i, &name, &measures));
    pin.insert(name);
    const GTEXT_CSS_Unit u = gtext_css_unit_lookup(name, strlen(name));
    ASSERT_NE(u, GTEXT_CSS_UNIT_NONE) << name;
    const GTEXT_CSS_Unit_Type t = gtext_css_unit_type(u);
    const std::string m = measures;
    const GTEXT_CSS_Unit_Type want = m == "length" ? GTEXT_CSS_UNIT_TYPE_LENGTH : m == "angle" ? GTEXT_CSS_UNIT_TYPE_ANGLE
        : m == "time" ? GTEXT_CSS_UNIT_TYPE_TIME : m == "frequency" ? GTEXT_CSS_UNIT_TYPE_FREQUENCY : m == "resolution" ? GTEXT_CSS_UNIT_TYPE_RESOLUTION
        : GTEXT_CSS_UNIT_TYPE_FLEX;
    EXPECT_EQ(t, want) << name;
  }
  // the units of the library that the Values definitions do not list: container query lengths (Containment 3) and the flex unit (Grid)
  std::set<std::string> extras;
  for (int u = 1; u < GTEXT_CSS_UNIT_COUNT; u++) {
    const char * n = gtext_css_unit_name((GTEXT_CSS_Unit)u);
    if (n && !pin.count(n)) {
      extras.insert(n);
    }
  }
  EXPECT_EQ(extras, (std::set<std::string>{"cqb", "cqh", "cqi", "cqmax", "cqmin", "cqw", "fr"}));
}

TEST(CssRegistryPin, TheFunctionsOfTheValuesDefinitionsAndWhatTheLibraryDoesWithThem) {
  // each is a math function, a substitution function, a function of another family, or a gap listed here
  std::set<std::string> gaps;
  size_t seen = 0;
  for (size_t i = 0; i < gtext_css_registry_pin_function_count(); i++) {
    const char * f = gtext_css_registry_pin_function_at(i);
    ASSERT_NE(f, nullptr);
    seen++;
    const std::string name(f, strlen(f) - 2);  // without the "()"
    const bool math = gtext_css_math_is_function(name.data(), name.size());
    const bool family = gtext_css_function_family(name.data(), name.size()) != GTEXT_CSS_FN_FAMILY_NONE;
    const bool subst = gtext_css_substitution_kind(name.data(), name.size()) != GTEXT_CSS_SUBST_NONE;
    if (!math && !family && !subst) {
      gaps.insert(f);
    }
  }
  EXPECT_EQ(seen, 44u);
  EXPECT_EQ(gaps, (std::set<std::string>{"calc-interpolate()", "calc-mix()", "color-interpolate()", "cross-origin()", "cycle()", "integrity()",
                                         "interpolate()", "referrer-policy()", "transform-interpolate()", "transform-mix()", "type()"}))
      << "the Values 5 functions the engine has no semantics for yet; each is matched as a function by a grammar that writes it";
  EXPECT_EQ(gtext_css_registry_pin_function_at(gtext_css_registry_pin_function_count()), nullptr);
  const char * name;
  const char * measures;
  EXPECT_FALSE(gtext_css_registry_pin_unit_at(gtext_css_registry_pin_unit_count(), &name, &measures));
}

// ---- the caller layer -------------------------------------------------------------------------------------------------

GTEXT_CSS_Property_Def def(const char * name, const char * grammar, const char * initial) {
  GTEXT_CSS_Property_Def d;
  std::memset(&d, 0, sizeof d);
  d.name = name;
  d.grammar = grammar;
  d.initial = initial;
  return d;
}

TEST(CssRegistryCaller, ARegistrationIsFoundParsedAndReported) {
  Builder b;
  GTEXT_CSS_Property_Def d = def("Vector-Weight", "<number [0,∞]> | auto", "1");
  d.inherited = true;
  d.animation = GTEXT_CSS_ANIM_NUMBER;
  d.computed = GTEXT_CSS_COMPUTED_AS_SPECIFIED;
  GTEXT_CSS_Registry_Result res;
  ASSERT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_OK);
  const GTEXT_CSS_Property_Id id = res.id;
  EXPECT_EQ(id, gtext_css_registry_generated_property_count() + 1);
  Owned o;
  ASSERT_EQ(b.freeze(o), GTEXT_CSS_OK);
  EXPECT_EQ(find(o.r, "vector-weight"), id);
  EXPECT_EQ(find(o.r, "VECTOR-WEIGHT"), id);
  GTEXT_CSS_Property_Info i = info(o.r, "vector-weight");
  EXPECT_EQ(std::string(i.name, i.name_length), "vector-weight");
  EXPECT_EQ(str(i.grammar), "<number [0,∞]> | auto");
  EXPECT_EQ(str(i.initial), "1");
  EXPECT_EQ(i.inherited, GTEXT_CSS_INHERIT_YES);
  EXPECT_EQ(i.animation, GTEXT_CSS_ANIM_NUMBER);
  EXPECT_EQ(i.computed, GTEXT_CSS_COMPUTED_AS_SPECIFIED);
  EXPECT_EQ(i.flags & GTEXT_CSS_PROP_GENERATED, 0u);
  EXPECT_EQ(i.flags & GTEXT_CSS_PROP_OVERRIDE, 0u);
  EXPECT_EQ(gtext_css_registry_property_count(o.r), gtext_css_registry_generated_property_count() + 1);
  EXPECT_EQ(parse(o.r, "vector-weight", "2.5"), "valid:2.5");
  EXPECT_EQ(parse(o.r, "vector-weight", "auto"), "valid:auto");
  EXPECT_EQ(parse(o.r, "vector-weight", "-1"), "invalid");
  EXPECT_EQ(gtext_css_registry_override_count(o.r), 0u);
}

TEST(CssRegistryCaller, ARegistrationMayUseThePinsTypesAndProperties) {
  Builder b;
  GTEXT_CSS_Property_Def d = def("vector-gap", "<'margin-top'> | <length-percentage [0,∞]>{2}", "0");
  ASSERT_EQ(gtext_css_registry_register_property(b.b, &d, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Property_Def e = def("vector-paint", "<'vector-gap'> | none", "none");
  ASSERT_EQ(gtext_css_registry_register_property(b.b, &e, nullptr), GTEXT_CSS_OK) << "a registered property can name an earlier one";
  GTEXT_CSS_Property_Def f = def("vector-shape", "<basic-shape> | <gradient>", "circle(10px)");
  ASSERT_EQ(gtext_css_registry_register_property(b.b, &f, nullptr), GTEXT_CSS_OK) << "the pin's types and the library's function types";
  Owned o;
  ASSERT_EQ(b.freeze(o), GTEXT_CSS_OK);
  EXPECT_EQ(parse(o.r, "vector-gap", "5px"), "valid:5px");
  EXPECT_EQ(parse(o.r, "vector-gap", "5px 6px"), "valid:5px 6px");
  EXPECT_EQ(parse(o.r, "vector-paint", "none"), "valid:none");
  EXPECT_EQ(parse(o.r, "vector-shape", "circle(5px)"), "valid:circle(5px)");
}

TEST(CssRegistryCaller, RedefiningAGeneratedPropertyIsRefusedUnlessItOverridesAndThenItIsReported) {
  Builder b;
  const GTEXT_CSS_Registry * dflt = shared();
  const GTEXT_CSS_Property_Id fill_rule = find(dflt, "fill-rule");
  ASSERT_NE(fill_rule, 0u);
  GTEXT_CSS_Property_Def d = def("fill-rule", "nonzero | evenodd | zzz", "nonzero");
  GTEXT_CSS_Registry_Result res;
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_E_EXISTS);
  EXPECT_EQ(res.status, GTEXT_CSS_E_EXISTS);
  EXPECT_EQ(res.id, fill_rule) << "the result names what is already there";
  EXPECT_EQ(std::string(res.name), "fill-rule");
  d.name = "FILL-RULE";
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_E_EXISTS) << "names are folded";
  d.override = true;
  ASSERT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_OK);
  const GTEXT_CSS_Property_Id mine = res.id;
  Owned o;
  ASSERT_EQ(b.freeze(o), GTEXT_CSS_OK);
  EXPECT_EQ(find(o.r, "fill-rule"), mine) << "a lookup finds the override";
  EXPECT_NE(mine, fill_rule);
  EXPECT_NE(info(o.r, "fill-rule").flags & GTEXT_CSS_PROP_OVERRIDE, 0u);
  ASSERT_EQ(gtext_css_registry_override_count(o.r), 1u);
  GTEXT_CSS_Property_Id by = 0, was = 0;
  ASSERT_TRUE(gtext_css_registry_override_at(o.r, 0, &by, &was));
  EXPECT_EQ(by, mine);
  EXPECT_EQ(was, fill_rule);
  EXPECT_FALSE(gtext_css_registry_override_at(o.r, 1, &by, &was));
  EXPECT_EQ(parse(o.r, "fill-rule", "zzz"), "valid:zzz");
  // the generated registry is unchanged: the pin's property is still there under its own id
  GTEXT_CSS_Property_Info orig;
  ASSERT_TRUE(gtext_css_registry_property_info(o.r, fill_rule, &orig));
  EXPECT_NE(orig.flags & GTEXT_CSS_PROP_GENERATED, 0u);
  EXPECT_EQ(parse(shared(), "fill-rule", "zzz"), "invalid");
}

TEST(CssRegistryCaller, AnOverrideIsSeenByTheGrammarsThatNameTheProperty) {
  Builder b;
  GTEXT_CSS_Property_Def d = def("color", "red | blue", "red");
  d.override = true;
  ASSERT_EQ(gtext_css_registry_register_property(b.b, &d, nullptr), GTEXT_CSS_OK);
  Owned o;
  ASSERT_EQ(b.freeze(o), GTEXT_CSS_OK);
  // `stop-color` is `<'color'>` in the pin
  EXPECT_EQ(parse(o.r, "stop-color", "red"), "valid:red");
  EXPECT_EQ(parse(o.r, "stop-color", "green"), "invalid");
  EXPECT_EQ(parse(shared(), "stop-color", "green"), "valid:green");
}

TEST(CssRegistryCaller, AnOverrideMayNameTheDefinitionItReplaces) {
  Builder b;
  GTEXT_CSS_Property_Def d = def("width", "<'width'> | zzwide", "auto");
  d.override = true;
  ASSERT_EQ(gtext_css_registry_register_property(b.b, &d, nullptr), GTEXT_CSS_OK);
  Owned o;
  ASSERT_EQ(b.freeze(o), GTEXT_CSS_OK);
  EXPECT_EQ(parse(o.r, "width", "zzwide"), "valid:zzwide");
  EXPECT_EQ(parse(o.r, "width", "10px"), "valid:10px") << "`<'width'>` is the pin's width, which the override extends";
  EXPECT_EQ(parse(o.r, "width", "zzother"), "invalid");
  EXPECT_EQ(parse(shared(), "width", "zzwide"), "invalid");
}

TEST(CssRegistryCaller, ACycleThroughAnOverrideIsRefusedAtTheFreezeAndNamed) {
  // `foo` names `width`, and the override of `width` names `foo`: at the time of each registration it links (the override of `width`
  // sees `foo`, which sees the pin's `width`), and in the frozen registry `width` is the override, so the two are one cycle
  Builder b;
  GTEXT_CSS_Property_Def foo = def("zzfoo", "<'width'> | zzx", "zzx");
  ASSERT_EQ(gtext_css_registry_register_property(b.b, &foo, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Property_Def w = def("width", "<'zzfoo'> | zzy", "zzy");
  w.override = true;
  ASSERT_EQ(gtext_css_registry_register_property(b.b, &w, nullptr), GTEXT_CSS_OK);
  Owned o;
  GTEXT_CSS_Registry_Result res;
  EXPECT_EQ(b.freeze(o, &res), GTEXT_CSS_E_LEFT_RECURSION);
  EXPECT_EQ(res.status, GTEXT_CSS_E_LEFT_RECURSION);
  EXPECT_FALSE(std::string(res.name).empty()) << "the property that cannot be linked is named";
  EXPECT_EQ(o.r, nullptr);
  EXPECT_NE(b.b, nullptr) << "a failed freeze leaves the builder as it was";
}

TEST(CssRegistryCaller, ARegistrationAfterAnOverrideCanBeRedefinedAgainWithAnOverride) {
  Builder b;
  GTEXT_CSS_Property_Def d = def("vector-a", "a | b", "a");
  ASSERT_EQ(gtext_css_registry_register_property(b.b, &d, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, nullptr), GTEXT_CSS_E_EXISTS);
  d.grammar = "a | b | c";
  d.override = true;
  ASSERT_EQ(gtext_css_registry_register_property(b.b, &d, nullptr), GTEXT_CSS_OK);
  Owned o;
  ASSERT_EQ(b.freeze(o), GTEXT_CSS_OK);
  EXPECT_EQ(parse(o.r, "vector-a", "c"), "valid:c");
  ASSERT_EQ(gtext_css_registry_override_count(o.r), 1u);
  GTEXT_CSS_Property_Id by, was;
  gtext_css_registry_override_at(o.r, 0, &by, &was);
  EXPECT_GT(was, gtext_css_registry_generated_property_count()) << "it replaced an earlier registration";
}

TEST(CssRegistryCaller, AnOverrideThatNamesItselfLeavesTheGeneratedPropertiesThatNameItWorking) {
  // `margin-top` is named by `margin`, `margin-block`, `margin-inline` ... ; `width` by `flex`, `size` and others. An override that
  // extends the definition it replaces (`<'margin-top'> | zz`) names itself, and a generated property that names the name gets the
  // new definition, which must still link.
  Builder b;
  GTEXT_CSS_Property_Def mt = def("margin-top", "<'margin-top'> | zzm", "0");
  mt.override = true;
  ASSERT_EQ(gtext_css_registry_register_property(b.b, &mt, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Property_Def w = def("width", "<'width'> | zzw", "auto");
  w.override = true;
  ASSERT_EQ(gtext_css_registry_register_property(b.b, &w, nullptr), GTEXT_CSS_OK);
  w.grammar = "<'width'> | zzv";
  ASSERT_EQ(gtext_css_registry_register_property(b.b, &w, nullptr), GTEXT_CSS_OK) << "a second override of the same name, naming the first";
  Owned o;
  GTEXT_CSS_Registry_Result res;
  ASSERT_EQ(b.freeze(o, &res), GTEXT_CSS_OK) << res.name << " " << (res.detail ? res.detail : "");
  EXPECT_EQ(parse(o.r, "margin-top", "zzm"), "valid:zzm");
  EXPECT_EQ(parse(o.r, "margin-top", "3px"), "valid:3px");
  EXPECT_EQ(parse(o.r, "margin", "zzm 1px"), "valid:zzm 1px") << "margin names margin-top";
  EXPECT_EQ(parse(o.r, "margin-block", "zzm 1px"), "valid:zzm 1px");
  EXPECT_EQ(parse(o.r, "width", "zzv"), "valid:zzv");
  EXPECT_EQ(parse(o.r, "width", "zzw"), "valid:zzw") << "the first override is what the second extends";
  EXPECT_EQ(parse(o.r, "width", "5px"), "valid:5px");
  EXPECT_EQ(parse(o.r, "flex", "1 1 zzv"), "valid:1 1 zzv") << "flex-basis names width";
  EXPECT_EQ(parse(shared(), "margin", "zzm 1px"), "invalid");
  EXPECT_EQ(gtext_css_registry_override_count(o.r), 3u);
}

TEST(CssRegistryCaller, AnOverrideIsSeenByTheRelationsAndAliasesOfTheProperty) {
  // After `overflow-wrap` is overridden, `word-wrap` (its alias in the pin) means the override, and `margin`'s longhand list and the
  // logical groups name the override and not the property it replaced.
  Builder b;
  GTEXT_CSS_Property_Def ow = def("overflow-wrap", "normal | anywhere | break-word | zzwrap", "normal");
  ow.override = true;
  ASSERT_EQ(gtext_css_registry_register_property(b.b, &ow, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Property_Def mt = def("margin-top", "<length> | zzm", "0");
  mt.override = true;
  ASSERT_EQ(gtext_css_registry_register_property(b.b, &mt, nullptr), GTEXT_CSS_OK);
  Owned o;
  ASSERT_EQ(b.freeze(o), GTEXT_CSS_OK);
  const GTEXT_CSS_Property_Id wrap = find(o.r, "overflow-wrap");
  EXPECT_NE(wrap, find(shared(), "overflow-wrap"));
  EXPECT_EQ(gtext_css_registry_resolve_alias(o.r, find(o.r, "word-wrap")), wrap);
  EXPECT_EQ(parse(o.r, "word-wrap", "zzwrap"), "valid:zzwrap");
  const GTEXT_CSS_Property_Id margin = find(o.r, "margin");
  const GTEXT_CSS_Property_Info mi = info(o.r, "margin");
  ASSERT_GE(mi.longhand_count, 4u);
  bool has_override = false, has_original = false;
  for (size_t k = 0; k < mi.longhand_count; k++) {
    const GTEXT_CSS_Property_Id l = gtext_css_registry_longhand(o.r, margin, k);
    has_override = has_override || l == find(o.r, "margin-top");
    has_original = has_original || l == find(shared(), "margin-top");
  }
  EXPECT_TRUE(has_override) << "the shorthand's longhands are the properties of this registry";
  EXPECT_FALSE(has_original);
}

TEST(CssRegistryCaller, AnOverrideThatBreaksAGeneratedPropertyIsRefusedAtTheFreezeNamingTheOverride) {
  // `flex-basis` is `content | <'width'>` and `flex` names `flex-basis`: an override of `width` that names `flex` closes a cycle
  // through generated properties that no registration saw.
  Builder b;
  GTEXT_CSS_Property_Def w = def("width", "<'flex'> | zzy", "auto");
  w.override = true;
  ASSERT_EQ(gtext_css_registry_register_property(b.b, &w, nullptr), GTEXT_CSS_OK);
  Owned o;
  GTEXT_CSS_Registry_Result res;
  const GTEXT_CSS_Status st = b.freeze(o, &res);
  EXPECT_EQ(st, GTEXT_CSS_E_LEFT_RECURSION);
  EXPECT_EQ(o.r, nullptr);
  EXPECT_EQ(std::string(res.name), "width") << "the failure is blamed on the override";
  ASSERT_NE(res.detail, nullptr);
  EXPECT_NE(std::string(res.detail).find("override"), std::string::npos) << res.detail;
  // the builder is still a builder, and what the failed attempt compiled is not remembered: with `flex` overridden by a definition that
  // does not name `width`, the cycle is gone and the same builder freezes
  GTEXT_CSS_Property_Def f = def("flex", "none | zzflex", "none");
  f.override = true;
  ASSERT_EQ(gtext_css_registry_register_property(b.b, &f, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(b.freeze(o, &res), GTEXT_CSS_OK) << res.name << " " << (res.detail ? res.detail : "");
  EXPECT_EQ(parse(o.r, "width", "zzflex"), "valid:zzflex");
  EXPECT_EQ(parse(o.r, "width", "zzy"), "valid:zzy");
}

TEST(CssRegistryCaller, ARegistrationOfTheNameOfACssWideKeywordOrABadShapeIsRefusedWithItsReason) {
  Builder b;
  GTEXT_CSS_Registry_Result res;
  for (const char * name : {"inherit", "INITIAL", "unset", "revert", "revert-layer", "revert-rule"}) {
    GTEXT_CSS_Property_Def d = def(name, "a", "a");
    EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_E_INVALID) << name;
    ASSERT_NE(res.detail, nullptr);
    EXPECT_NE(std::string(res.detail).find("CSS-wide"), std::string::npos) << res.detail;
    EXPECT_EQ(gtext_css_registry_register_alias(b.b, name, "width", false, &res), GTEXT_CSS_E_INVALID) << name;
  }
  GTEXT_CSS_Property_Def d = def("9bad", "a", "a");
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_E_INVALID);
  ASSERT_NE(res.detail, nullptr);
  EXPECT_NE(std::string(res.detail).find("name"), std::string::npos) << res.detail;
  d = def("vector-x", "a | b", "c");
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_E_INVALID);
  ASSERT_NE(res.detail, nullptr);
  EXPECT_NE(std::string(res.detail).find("initial value"), std::string::npos) << res.detail;
  d = def("vector-x", "a | b | inherit", "inherit");
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_E_INVALID);
  ASSERT_NE(res.detail, nullptr);
  EXPECT_NE(std::string(res.detail).find("CSS-wide"), std::string::npos) << res.detail;
  // an alias of a custom property, and shorthands whose longhands cannot be expanded to
  EXPECT_EQ(gtext_css_registry_register_alias(b.b, "vector-alias", "--custom", false, &res), GTEXT_CSS_E_INVALID);
  const char * dup[] = {"width", "height", "WIDTH"};
  GTEXT_CSS_Shorthand_Def sh;
  std::memset(&sh, 0, sizeof sh);
  sh.name = "vector-dup";
  sh.grammar = "<'width'>";
  sh.longhands = dup;
  sh.longhand_count = 3;
  EXPECT_EQ(gtext_css_registry_register_shorthand(b.b, &sh, &res), GTEXT_CSS_E_INVALID) << "a longhand named twice";
  const char * custom[] = {"width", "--foo"};
  sh.name = "vector-custom";
  sh.longhands = custom;
  sh.longhand_count = 2;
  EXPECT_EQ(gtext_css_registry_register_shorthand(b.b, &sh, &res), GTEXT_CSS_E_INVALID) << "a custom property is no longhand";
  // a shorthand of a shorthand is allowed: it expands to what the inner expands to (the pin's `border` names `border-width`)
  const char * nested[] = {"margin", "padding"};
  sh.name = "vector-box";
  sh.grammar = "<'margin'> <'padding'>";
  sh.longhands = nested;
  sh.longhand_count = 2;
  EXPECT_EQ(gtext_css_registry_register_shorthand(b.b, &sh, &res), GTEXT_CSS_OK);
}

TEST(CssRegistryCaller, AShorthandMayNameItselfAsAGrammarTypeOnlyThroughTheProperty) {
  // A shorthand cannot name itself as a longhand, and a registered shorthand's grammar is linked against what is there already.
  Builder b;
  GTEXT_CSS_Registry_Result res;
  const char * self[] = {"width", "vector-self"};
  GTEXT_CSS_Shorthand_Def sh;
  std::memset(&sh, 0, sizeof sh);
  sh.name = "vector-self";
  sh.grammar = "<'width'>";
  sh.longhands = self;
  sh.longhand_count = 2;
  EXPECT_EQ(gtext_css_registry_register_shorthand(b.b, &sh, &res), GTEXT_CSS_E_REFERENCE) << "it does not exist yet";
  const char * w[] = {"width"};
  sh.name = "width";
  sh.longhands = w;
  sh.longhand_count = 1;
  sh.override = true;
  EXPECT_EQ(gtext_css_registry_register_shorthand(b.b, &sh, &res), GTEXT_CSS_E_INVALID) << "a shorthand cannot be its own longhand";
  ASSERT_NE(res.detail, nullptr);
}

TEST(CssRegistryCaller, ATypeTheDifferentPropertiesDefineDifferentlyIsNotGuessedAt) {
  // The pin defines some types inside one property and names them from others. With no global type of the name, a definition nested in
  // other properties is used when every one says the same, and a name they define differently resolves to nothing: a grammar that
  // names it is a reference error, not a guess.
  std::map<std::string, std::vector<std::string>> nested;
  std::set<std::string> global;
  const size_t n = gtext_css_registry_type_count();
  for (size_t i = 0; i < n; i++) {
    GTEXT_CSS_Type_Info t;
    ASSERT_TRUE(gtext_css_registry_type_at(i, &t));
    if (t.flags & (GTEXT_CSS_TYPE_FUNCTION | GTEXT_CSS_TYPE_NATIVE)) {
      global.insert(std::string(t.name, t.name_length));
      continue;
    }
    if (t.scope == 0) {
      global.insert(std::string(t.name, t.name_length));
    }
    else if (t.grammar) {
      nested[std::string(t.name, t.name_length)].push_back(t.grammar);
    }
  }
  std::vector<std::string> same;
  std::string differ;
  for (const auto & kv : nested) {
    if (global.count(kv.first)) {
      continue;
    }
    bool all_same = true;
    for (const auto & g : kv.second) {
      all_same = all_same && g == kv.second[0];
    }
    if (all_same) {
      same.push_back(kv.first);
    }
    else if (differ.empty()) {
      differ = kv.first;
    }
  }
  ASSERT_FALSE(same.empty()) << "the pin nests a type that no global type has";
  Builder b;
  GTEXT_CSS_Registry_Result res;
  size_t resolved = 0;
  for (const std::string & name : same) {
    const std::string g = "<" + name + ">";
    GTEXT_CSS_Property_Def d = def("vector-same", g.c_str(), "zz");
    const GTEXT_CSS_Status st = gtext_css_registry_register_property(b.b, &d, &res);
    resolved += st == GTEXT_CSS_OK || st == GTEXT_CSS_E_INVALID;  // INVALID: found, and the initial value `zz` is not one of it
  }
  EXPECT_GT(resolved, 0u) << "at least one is a usable definition";
  if (differ.empty()) {
    // The pinned data has no such name, so the refusal below has nothing to refuse today; it is exercised by the first pin that has
    // one (this test then holds it to the rule), and the branch that decides it is read, not run, until then.
    return;
  }
  GTEXT_CSS_Property_Def d = def("vector-differ", ("<" + differ + "> | zzdiffer").c_str(), "zzdiffer");
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_E_REFERENCE) << differ;
  EXPECT_EQ(std::string(res.name), "<" + differ + ">");
}

TEST(CssRegistryCaller, ARegisteredAllIsNotTheGeneratedAll) {
  // The rule that `all` takes only the CSS-wide keywords is the pin's `all`; a caller who overrides it defines their own.
  Builder b;
  GTEXT_CSS_Property_Def a = def("all", "zzall | <length>", "zzall");
  a.override = true;
  ASSERT_EQ(gtext_css_registry_register_property(b.b, &a, nullptr), GTEXT_CSS_OK);
  Owned o;
  ASSERT_EQ(b.freeze(o), GTEXT_CSS_OK);
  EXPECT_EQ(parse(o.r, "all", "zzall"), "valid:zzall");
  EXPECT_EQ(parse(o.r, "all", "5px"), "valid:5px");
  EXPECT_EQ(parse(o.r, "all", "revert"), "wide:revert");
  EXPECT_EQ(parse(shared(), "all", "5px"), "invalid");
}

TEST(CssRegistryCaller, ManyRegistrationsAndAnOverrideAmongThemAreAllFound) {
  // Past the first growths of the name table, a name registered twice (the second an override) finds the second, and every
  // other name is still found.
  Builder b;
  std::vector<std::string> names;
  for (int i = 0; i < 300; i++) {
    names.push_back("vector-n" + std::to_string(i));
    GTEXT_CSS_Property_Def d = def(names.back().c_str(), "a | b", "a");
    ASSERT_EQ(gtext_css_registry_register_property(b.b, &d, nullptr), GTEXT_CSS_OK) << i;
    if (i == 17 || i == 150) {
      GTEXT_CSS_Property_Def e = def(names.back().c_str(), "a | b | c", "a");
      e.override = true;
      ASSERT_EQ(gtext_css_registry_register_property(b.b, &e, nullptr), GTEXT_CSS_OK) << i;
    }
    if (i == 40) {
      GTEXT_CSS_Property_Def w = def("width", "<'width'> | zzw", "auto");
      w.override = true;
      ASSERT_EQ(gtext_css_registry_register_property(b.b, &w, nullptr), GTEXT_CSS_OK);
    }
  }
  Owned o;
  ASSERT_EQ(b.freeze(o), GTEXT_CSS_OK);
  for (int i = 0; i < 300; i++) {
    const GTEXT_CSS_Property_Id id = find(o.r, names[i]);
    ASSERT_NE(id, 0u) << names[i];
    EXPECT_EQ(parse(o.r, names[i], "c"), (i == 17 || i == 150) ? "valid:c" : "invalid") << names[i] << ": the override, and only it, takes c";
  }
  EXPECT_EQ(parse(o.r, "width", "zzw"), "valid:zzw");
  EXPECT_NE(find(o.r, "width"), find(shared(), "width"));
  EXPECT_EQ(gtext_css_registry_override_count(o.r), 3u);
}

TEST(CssRegistryCaller, ARegistrationIsValidated) {
  Builder b;
  GTEXT_CSS_Registry_Result res;
  // the name
  for (const char * bad : {"", "--custom", "9lives", "has space", "a.b", "a:b", "-", "-1x", "ünï"}) {
    GTEXT_CSS_Property_Def d = def(bad, "a", "a");
    EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_E_INVALID) << bad;
  }
  // the grammar
  GTEXT_CSS_Property_Def d = def("vector-x", "a |", "a");
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_E_GRAMMAR);
  EXPECT_EQ(res.status, GTEXT_CSS_E_GRAMMAR);
  EXPECT_EQ(res.grammar.status, GTEXT_CSS_E_GRAMMAR);
  d = def("vector-x", "<nosuch-type> | a", "a");
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_E_REFERENCE);
  EXPECT_EQ(std::string(res.name), "<nosuch-type>") << "the missing type is named";
  EXPECT_EQ(std::string(res.grammar.name), "<nosuch-type>");
  d = def("vector-x", "<'nosuch-property'>", "a");
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_E_REFERENCE);
  EXPECT_EQ(std::string(res.name), "<'nosuch-property'>");
  d = def("vector-x", "<self-type>", "a");
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_E_REFERENCE);
  // a property that does not exist yet cannot be named by its own grammar
  d = def("vector-x", "a | <'vector-x'>", "a");
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_E_REFERENCE);
  // the initial value
  d = def("vector-x", "a | b", "c");
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_E_INVALID) << "it does not match the grammar";
  d = def("vector-x", "a | b | inherit", "inherit");
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_E_INVALID) << "a CSS-wide keyword is not a value of the property";
  d = def("vector-x", "a | <length>", "var(--x)");
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_E_INVALID) << "a substitution is not a concrete value";
  d = def("vector-x", "a | <length>", "");
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_E_INVALID) << "an empty initial value matches nothing here";
  d = def("vector-x", "a | b", "a b");
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_E_INVALID);
  // arguments
  EXPECT_EQ(gtext_css_registry_register_property(nullptr, &d, &res), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_registry_register_property(b.b, nullptr, &res), GTEXT_CSS_E_INVALID);
  d = def(nullptr, "a", "a");
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, nullptr), GTEXT_CSS_E_INVALID);
  d = def("vector-x", nullptr, "a");
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, nullptr), GTEXT_CSS_E_INVALID);
  d = def("vector-x", "a", nullptr);
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, nullptr), GTEXT_CSS_E_INVALID);
  // nothing was registered by any of that
  Owned o;
  ASSERT_EQ(b.freeze(o), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_registry_property_count(o.r), gtext_css_registry_generated_property_count());
  EXPECT_EQ(find(o.r, "vector-x"), 0u);
}

TEST(CssRegistryCaller, ALongGrammarIsHeldToTheGrammarLimit) {
  GTEXT_CSS_Grammar_Options go = gtext_css_grammar_options_default();
  go.max_definition_bytes = 64;
  GTEXT_CSS_Registry_Options ro = gtext_css_registry_options_default();
  ro.grammar = &go;
  Builder b(&ro);
  // (the text of a definition is held to the bound together with the NUL after it when it is linked)
  const std::string at(63, 'a');
  GTEXT_CSS_Property_Def d = def("vector-g", at.c_str(), at.c_str());
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, nullptr), GTEXT_CSS_OK) << "63 bytes and the NUL are the bound";
  const std::string over(64, 'a');
  d = def("vector-h", over.c_str(), over.c_str());
  GTEXT_CSS_Registry_Result res;
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_E_LIMIT_DEFINITION) << "one more is refused";
  EXPECT_EQ(res.grammar.status, GTEXT_CSS_E_LIMIT_DEFINITION);
}

TEST(CssRegistryCaller, TheLimitsAreTheirOwnStatusAtTheBoundAndOnePast) {
  GTEXT_CSS_Registry_Options ro = gtext_css_registry_options_default();
  ro.max_registrations = 3;
  ro.max_name_bytes = 8;
  ro.max_initial_bytes = 4;
  ro.max_longhands = 2;
  Builder b(&ro);
  GTEXT_CSS_Registry_Result res;
  // a name of exactly 8 bytes is fine, 9 is not
  GTEXT_CSS_Property_Def d = def("abcdefgh", "a", "a");
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_OK);
  d = def("abcdefghi", "a", "a");
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_E_LIMIT_REGISTRY);
  EXPECT_EQ(res.status, GTEXT_CSS_E_LIMIT_REGISTRY);
  // an initial value of exactly 4 bytes is fine, 5 is not
  d = def("v1", "aaaa | bbbbb", "aaaa");
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_OK);
  d = def("v2", "aaaa | bbbbb", "bbbbb");
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_E_LIMIT_REGISTRY);
  // three registrations are fine (one more is refused), whatever kind
  d = def("v3", "a", "a");
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_OK);
  d = def("v4", "a", "a");
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_E_LIMIT_REGISTRY);
  EXPECT_EQ(gtext_css_registry_register_alias(b.b, "v5", "width", false, &res), GTEXT_CSS_E_LIMIT_REGISTRY);
  const char * lh3[] = {"width", "height", "color"};
  GTEXT_CSS_Shorthand_Def sh;
  std::memset(&sh, 0, sizeof sh);
  sh.name = "v6";
  sh.grammar = "a";
  sh.longhands = lh3;
  sh.longhand_count = 3;
  EXPECT_EQ(gtext_css_registry_register_shorthand(b.b, &sh, &res), GTEXT_CSS_E_LIMIT_REGISTRY) << "more longhands than the limit, and no room";
  Owned o;
  ASSERT_EQ(b.freeze(o), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_registry_property_count(o.r), gtext_css_registry_generated_property_count() + 3);
}

TEST(CssRegistryCaller, TheCompiledGrammarsOfAllTheRegistrationsAreHeldToANodeBudget) {
  // A chain of properties that each name the one before holds the square of its length in nodes (a compiled grammar carries what it
  // names); the budget is what bounds the memory of that, at the bound and one past, and a refused registration charges nothing.
  // the nodes of one such grammar: the least budget that takes it
  size_t per = 0;
  for (size_t budget = 1; budget < 100 && per == 0; budget++) {
    GTEXT_CSS_Registry_Options probe_options = gtext_css_registry_options_default();
    probe_options.max_grammar_nodes = budget;
    Builder probe(&probe_options);
    GTEXT_CSS_Property_Def d = def("budget-a", "a | b | c", "a");
    if (gtext_css_registry_register_property(probe.b, &d, nullptr) == GTEXT_CSS_OK) {
      per = budget;
    }
  }
  ASSERT_GT(per, 0u);
  GTEXT_CSS_Registry_Result res;
  GTEXT_CSS_Registry_Options ro = gtext_css_registry_options_default();
  ro.max_grammar_nodes = per * 3;
  Builder b(&ro);
  for (const char * name : {"budget-a", "budget-b", "budget-c"}) {
    GTEXT_CSS_Property_Def d = def(name, "a | b | c", "a");
    EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_OK) << name;
  }
  GTEXT_CSS_Property_Def d = def("budget-d", "a | b | c", "a");
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, &res), GTEXT_CSS_E_LIMIT_REGISTRY) << "one past";
  ASSERT_NE(res.detail, nullptr);
  EXPECT_NE(std::string(res.detail).find("max_grammar_nodes"), std::string::npos) << res.detail;
  Owned o;
  ASSERT_EQ(b.freeze(o), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_registry_property_count(o.r), gtext_css_registry_generated_property_count() + 3);
  EXPECT_EQ(find(o.r, "budget-d"), 0u);
  // a chain runs into it, and the registrations before the refused one stand
  GTEXT_CSS_Registry_Options chain = gtext_css_registry_options_default();
  chain.max_grammar_nodes = 5000;
  Builder c(&chain);
  size_t made = 0;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  for (size_t i = 0; i < 200 && st == GTEXT_CSS_OK; i++) {
    const std::string name = "chain-" + std::to_string(i);
    const std::string g = i ? "<'chain-" + std::to_string(i - 1) + "'> | a" : "a";
    GTEXT_CSS_Property_Def e = def(name.c_str(), g.c_str(), "a");
    st = gtext_css_registry_register_property(c.b, &e, &res);
    made += st == GTEXT_CSS_OK;
  }
  EXPECT_EQ(st, GTEXT_CSS_E_LIMIT_REGISTRY);
  EXPECT_GT(made, 10u);
  EXPECT_LT(made, 200u);
  Owned co;
  ASSERT_EQ(c.freeze(co), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_registry_property_count(co.r), gtext_css_registry_generated_property_count() + made);
}

TEST(CssRegistryCaller, TheLonghandLimitIsItsOwn) {
  GTEXT_CSS_Registry_Options ro = gtext_css_registry_options_default();
  ro.max_longhands = 2;
  Builder b(&ro);
  const char * two[] = {"width", "height"};
  const char * three[] = {"width", "height", "color"};
  GTEXT_CSS_Shorthand_Def sh;
  std::memset(&sh, 0, sizeof sh);
  sh.name = "vector-size";
  sh.grammar = "<'width'> <'height'>?";
  sh.longhands = two;
  sh.longhand_count = 2;
  EXPECT_EQ(gtext_css_registry_register_shorthand(b.b, &sh, nullptr), GTEXT_CSS_OK);
  sh.name = "vector-size3";
  sh.longhands = three;
  sh.longhand_count = 3;
  EXPECT_EQ(gtext_css_registry_register_shorthand(b.b, &sh, nullptr), GTEXT_CSS_E_LIMIT_REGISTRY);
}

TEST(CssRegistryCaller, ExtremeOptionsAreClampedToTheirMaximum) {
  GTEXT_CSS_Registry_Options ro = gtext_css_registry_options_default();
  ro.max_name_bytes = 1u << 30;
  Builder b(&ro);
  const std::string name(4096, 'a');
  GTEXT_CSS_Property_Def d = def(name.c_str(), "a", "a");
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, nullptr), GTEXT_CSS_OK) << "4096 is the longest a name can be";
  const std::string over(4097, 'b');
  d = def(over.c_str(), "a", "a");
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &d, nullptr), GTEXT_CSS_E_LIMIT_REGISTRY);
  Owned o;
  ASSERT_EQ(b.freeze(o), GTEXT_CSS_OK);
  EXPECT_NE(find(o.r, name), 0u);
  EXPECT_EQ(find(o.r, over), 0u);
}

TEST(CssRegistryCaller, Aliases) {
  Builder b;
  GTEXT_CSS_Registry_Result res;
  ASSERT_EQ(gtext_css_registry_register_alias(b.b, "vector-width", "width", false, &res), GTEXT_CSS_OK);
  const GTEXT_CSS_Property_Id first = res.id;
  EXPECT_EQ(gtext_css_registry_register_alias(b.b, "vector-width", "height", false, &res), GTEXT_CSS_E_EXISTS);
  EXPECT_EQ(gtext_css_registry_register_alias(b.b, "vector-w2", "vector-width", false, &res), GTEXT_CSS_OK) << "an alias of an alias is its target's alias";
  const GTEXT_CSS_Property_Id second = res.id;
  EXPECT_EQ(gtext_css_registry_register_alias(b.b, "vector-w3", "nosuch-target", false, &res), GTEXT_CSS_E_REFERENCE);
  EXPECT_EQ(std::string(res.name), "nosuch-target");
  EXPECT_EQ(gtext_css_registry_register_alias(b.b, "-webkit-transform", "transform", false, &res), GTEXT_CSS_E_EXISTS) << "the pin's own alias is a name taken";
  EXPECT_EQ(gtext_css_registry_register_alias(b.b, "-webkit-transform", "rotate", true, &res), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_registry_register_alias(b.b, "width", "width", true, &res), GTEXT_CSS_E_INVALID) << "a name cannot override itself into an alias of itself";
  EXPECT_EQ(gtext_css_registry_register_alias(b.b, nullptr, "width", false, &res), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_registry_register_alias(b.b, "--x", "width", false, &res), GTEXT_CSS_E_INVALID);
  Owned o;
  ASSERT_EQ(b.freeze(o), GTEXT_CSS_OK);
  const GTEXT_CSS_Property_Id width = find(o.r, "width");
  EXPECT_EQ(gtext_css_registry_resolve_alias(o.r, first), width);
  EXPECT_EQ(gtext_css_registry_resolve_alias(o.r, second), width);
  GTEXT_CSS_Property_Info i = info(o.r, "vector-width");
  EXPECT_NE(i.flags & GTEXT_CSS_PROP_ALIAS, 0u);
  EXPECT_EQ(i.alias_of, width);
  EXPECT_EQ(parse(o.r, "vector-w2", "10px"), "valid:10px");
  EXPECT_EQ(gtext_css_registry_resolve_alias(o.r, find(o.r, "-webkit-transform")), find(o.r, "rotate"));
  EXPECT_EQ(gtext_css_registry_override_count(o.r), 1u);
}

GTEXT_CSS_Status expand_nothing(void *, const GTEXT_CSS_Value *, const GTEXT_CSS_Longhand_Sink *) {
  return GTEXT_CSS_OK;
}

TEST(CssRegistryCaller, ShorthandsCarryTheirLonghandsAndHook) {
  Builder b;
  GTEXT_CSS_Registry_Result res;
  const char * lh[] = {"width", "min-width", "MAX-WIDTH"};
  int marker = 0;
  GTEXT_CSS_Shorthand_Def sh;
  std::memset(&sh, 0, sizeof sh);
  sh.name = "vector-widths";
  sh.grammar = "<'width'>{1,3}";
  sh.longhands = lh;
  sh.longhand_count = 3;
  sh.hook = expand_nothing;
  sh.hook_user = &marker;
  ASSERT_EQ(gtext_css_registry_register_shorthand(b.b, &sh, &res), GTEXT_CSS_OK);
  const GTEXT_CSS_Property_Id id = res.id;
  EXPECT_EQ(gtext_css_registry_register_shorthand(b.b, &sh, &res), GTEXT_CSS_E_EXISTS);
  const char * bad[] = {"width", "nosuch"};
  sh.name = "vector-widths2";
  sh.longhands = bad;
  sh.longhand_count = 2;
  EXPECT_EQ(gtext_css_registry_register_shorthand(b.b, &sh, &res), GTEXT_CSS_E_REFERENCE);
  EXPECT_EQ(std::string(res.name), "nosuch");
  sh.longhands = lh;
  sh.longhand_count = 0;
  EXPECT_EQ(gtext_css_registry_register_shorthand(b.b, &sh, &res), GTEXT_CSS_E_INVALID);
  sh.longhand_count = 3;
  sh.grammar = "<'width'>{1,3";
  EXPECT_EQ(gtext_css_registry_register_shorthand(b.b, &sh, &res), GTEXT_CSS_E_GRAMMAR);
  Owned o;
  ASSERT_EQ(b.freeze(o), GTEXT_CSS_OK);
  const GTEXT_CSS_Property_Info i = info(o.r, "vector-widths");
  EXPECT_EQ(i.id, id);
  EXPECT_NE(i.flags & GTEXT_CSS_PROP_SHORTHAND, 0u);
  ASSERT_EQ(i.longhand_count, 3u);
  EXPECT_EQ(gtext_css_registry_longhand(o.r, id, 0), find(o.r, "width"));
  EXPECT_EQ(gtext_css_registry_longhand(o.r, id, 2), find(o.r, "max-width"));
  EXPECT_EQ(i.inherited, GTEXT_CSS_INHERIT_NO) << "derived from the longhands";
  GTEXT_CSS_Shorthand_Hook hook = nullptr;
  void * user = nullptr;
  ASSERT_TRUE(gtext_css_registry_shorthand_hook(o.r, id, &hook, &user));
  EXPECT_EQ(hook, &expand_nothing);
  EXPECT_EQ(user, &marker);
  EXPECT_FALSE(gtext_css_registry_shorthand_hook(o.r, find(o.r, "margin"), &hook, &user)) << "a generated shorthand has none";
  EXPECT_EQ(parse(o.r, "vector-widths", "1px 2px 3px"), "valid:1px 2px 3px");
  EXPECT_EQ(parse(o.r, "vector-widths", "1px 2px 3px 4px"), "invalid");
}

TEST(CssRegistryCaller, AFrozenBuilderIsConsumedAndTheRegistryIsImmutable) {
  Builder b;
  GTEXT_CSS_Property_Def d = def("vector-q", "a | b", "a");
  ASSERT_EQ(gtext_css_registry_register_property(b.b, &d, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Registry_Builder * raw = b.b;
  Owned o;
  ASSERT_EQ(b.freeze(o), GTEXT_CSS_OK);
  (void)raw;  // consumed: freeing it again would be a double free, and Builder does not
  EXPECT_EQ(b.b, nullptr);
  EXPECT_EQ(gtext_css_registry_freeze(nullptr, &o.r, nullptr), GTEXT_CSS_E_INVALID);
  GTEXT_CSS_Registry * unused = nullptr;
  EXPECT_EQ(gtext_css_registry_freeze(nullptr, &unused, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_registry_builder_new(nullptr, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_registry_new(nullptr, nullptr), GTEXT_CSS_E_INVALID);
  gtext_css_registry_builder_free(nullptr);
  gtext_css_registry_free(nullptr);
  EXPECT_EQ(gtext_css_registry_prepare(nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_registry_property_count(nullptr), 0u);
  EXPECT_EQ(gtext_css_registry_override_count(nullptr), 0u);
}

TEST(CssRegistryCaller, AFreezeFailsWhenARegisteredGrammarNoLongerLinks) {
  // `vector-b` names `vector-a`; then `vector-a` is overridden by a grammar that names a type nothing has: the grammar of
  // `vector-b` links no more, and the freeze says so rather than leaving a property that cannot match.
  Builder b;
  GTEXT_CSS_Property_Def a = def("vector-a", "x | y", "x");
  ASSERT_EQ(gtext_css_registry_register_property(b.b, &a, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Property_Def d = def("vector-b", "<'vector-a'> | z", "z");
  ASSERT_EQ(gtext_css_registry_register_property(b.b, &d, nullptr), GTEXT_CSS_OK);
  // the second registration of vector-a is itself refused for naming a missing type, so the freeze never sees it
  GTEXT_CSS_Property_Def bad = def("vector-a", "<nosuch>", "x");
  bad.override = true;
  EXPECT_EQ(gtext_css_registry_register_property(b.b, &bad, nullptr), GTEXT_CSS_E_REFERENCE);
  Owned o;
  EXPECT_EQ(b.freeze(o), GTEXT_CSS_OK);
  EXPECT_EQ(parse(o.r, "vector-b", "x"), "valid:x");
}

TEST(CssRegistryCaller, ThousandsOfRegistrationsAreFoundInConstantTime) {
  Builder b;
  const size_t n = 3000;
  for (size_t i = 0; i < n; i++) {
    const std::string name = "vector-p" + std::to_string(i);
    GTEXT_CSS_Property_Def d = def(name.c_str(), "a | b", "a");
    ASSERT_EQ(gtext_css_registry_register_property(b.b, &d, nullptr), GTEXT_CSS_OK) << i;
  }
  Owned o;
  ASSERT_EQ(b.freeze(o), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_registry_property_count(o.r), gtext_css_registry_generated_property_count() + n);
  for (size_t i = 0; i < n; i += 97) {
    const std::string name = "vector-p" + std::to_string(i);
    EXPECT_EQ(find(o.r, name), gtext_css_registry_generated_property_count() + 1 + i) << name;
  }
  EXPECT_EQ(find(o.r, "vector-p3000"), 0u);
}

// ---- grammars: lazy, locked, the state ---------------------------------------------------------------------------------

TEST(CssRegistryGrammars, AGrammarIsCompiledWhenAskedForAndItsStateIsReported) {
  GTEXT_CSS_Registry * r = nullptr;
  ASSERT_EQ(gtext_css_registry_new(nullptr, &r), GTEXT_CSS_OK);
  GTEXT_CSS_Grammar_State s;
  GTEXT_CSS_Grammar_Result gr;
  ASSERT_EQ(gtext_css_registry_property_state(r, find(r, "margin"), &s, &gr), GTEXT_CSS_OK);
  EXPECT_EQ(s, GTEXT_CSS_GRAMMAR_OK);
  // the pin's `clip` names <top> and nothing defines it
  ASSERT_EQ(gtext_css_registry_property_state(r, find(r, "clip"), &s, &gr), GTEXT_CSS_OK);
  EXPECT_EQ(s, GTEXT_CSS_GRAMMAR_UNRESOLVED);
  EXPECT_EQ(std::string(gr.name), "<top>");
  // a property the pin names and gives no grammar
  ASSERT_EQ(gtext_css_registry_property_state(r, find(r, "-webkit-box-align"), &s, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(s, GTEXT_CSS_GRAMMAR_NONE);
  // a grammar whose text is not a definition
  ASSERT_EQ(gtext_css_registry_property_state(r, find(r, "glyph-orientation-vertical"), &s, &gr), GTEXT_CSS_OK);
  EXPECT_EQ(s, GTEXT_CSS_GRAMMAR_FAILED);
  EXPECT_EQ(gr.status, GTEXT_CSS_E_GRAMMAR);
  // an alias reports its target's
  ASSERT_EQ(gtext_css_registry_property_state(r, find(r, "-webkit-transform"), &s, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(s, GTEXT_CSS_GRAMMAR_OK);
  EXPECT_EQ(gtext_css_registry_property_state(r, 0, &s, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_registry_property_state(r, 1u << 20, &s, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_registry_property_state(nullptr, 1, &s, nullptr), GTEXT_CSS_E_INVALID);
  // the answer is the same the second time
  ASSERT_EQ(gtext_css_registry_property_state(r, find(r, "clip"), &s, &gr), GTEXT_CSS_OK);
  EXPECT_EQ(s, GTEXT_CSS_GRAMMAR_UNRESOLVED);
  EXPECT_EQ(std::string(gr.name), "<top>");
  gtext_css_registry_free(r);
}

TEST(CssRegistryGrammars, TypesAndDescriptorsReportTheirStateToo) {
  const GTEXT_CSS_Registry * r = shared();
  GTEXT_CSS_Grammar_State s;
  GTEXT_CSS_Grammar_Result gr;
  size_t ok = 0, native = 0;
  for (size_t i = 0; i < gtext_css_registry_type_count(); i++) {
    ASSERT_EQ(gtext_css_registry_type_state(r, i, &s, &gr), GTEXT_CSS_OK) << i;
    GTEXT_CSS_Type_Info t;
    gtext_css_registry_type_at(i, &t);
    if (t.flags & GTEXT_CSS_TYPE_NATIVE) {
      EXPECT_EQ(s, GTEXT_CSS_GRAMMAR_NONE) << t.name;
      native++;
    }
    else if (!t.grammar) {
      EXPECT_EQ(s, GTEXT_CSS_GRAMMAR_NONE) << t.name;
    }
    ok += s == GTEXT_CSS_GRAMMAR_OK;
  }
  EXPECT_GT(ok, 440u);
  EXPECT_GE(native, 50u);
  EXPECT_EQ(gtext_css_registry_type_state(r, gtext_css_registry_type_count(), &s, nullptr), GTEXT_CSS_E_INVALID);
  const GTEXT_CSS_At_Rule_Id face = gtext_css_registry_find_at_rule("@font-face", 10);
  ASSERT_EQ(gtext_css_registry_descriptor_state(r, gtext_css_registry_find_descriptor(face, "font-display", 12), &s, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(s, GTEXT_CSS_GRAMMAR_OK);
  ASSERT_EQ(gtext_css_registry_descriptor_state(r, gtext_css_registry_find_descriptor(face, "src", 3), &s, &gr), GTEXT_CSS_OK);
  EXPECT_EQ(s, GTEXT_CSS_GRAMMAR_UNRESOLVED);
  EXPECT_EQ(std::string(gr.name), "<font-src-list>");
  const GTEXT_CSS_At_Rule_Id values = gtext_css_registry_find_at_rule("@font-feature-values", 20);
  ASSERT_EQ(gtext_css_registry_descriptor_state(r, gtext_css_registry_find_descriptor(values, "@swash", 6), &s, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(s, GTEXT_CSS_GRAMMAR_NONE) << "a nested at-rule is syntax and not a value";
  EXPECT_EQ(gtext_css_registry_descriptor_state(r, 0, &s, nullptr), GTEXT_CSS_E_INVALID);
}

TEST(CssRegistryGrammars, PrepareCompilesEveryGrammarAndTheRegistryIsThenReadOnly) {
  GTEXT_CSS_Registry * r = nullptr;
  ASSERT_EQ(gtext_css_registry_new(nullptr, &r), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_registry_prepare(r), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_registry_prepare(r), GTEXT_CSS_OK) << "it may be repeated";
  size_t ok = 0;
  for (size_t i = 1; i <= gtext_css_registry_property_count(r); i++) {
    GTEXT_CSS_Grammar_State s;
    ASSERT_EQ(gtext_css_registry_property_state(r, (GTEXT_CSS_Property_Id)i, &s, nullptr), GTEXT_CSS_OK);
    EXPECT_NE(s, GTEXT_CSS_GRAMMAR_NOT_COMPILED) << i;
    ok += s == GTEXT_CSS_GRAMMAR_OK;
  }
  EXPECT_GT(ok, 780u) << "including the aliases of the properties that compile";
  gtext_css_registry_free(r);
}

// A thread-safe counting allocator: every block made and freed, so that a grammar compiled twice (one copy overwritten) shows as a block
// never freed.
struct AtomicAllocator {
  GTEXT_Allocator alloc;
  std::atomic<long> made{0}, freed{0};
  AtomicAllocator() {
    alloc.ctx = this;
    alloc.malloc_fn = [](void * c, size_t n) { static_cast<AtomicAllocator *>(c)->made++; return std::malloc(n ? n : 1); };
    alloc.calloc_fn = [](void * c, size_t k, size_t n) { static_cast<AtomicAllocator *>(c)->made++; return std::calloc(k ? k : 1, n ? n : 1); };
    alloc.realloc_fn = [](void * c, void * p, size_t n) {
      if (!p) {
        static_cast<AtomicAllocator *>(c)->made++;
      }
      return std::realloc(p, n ? n : 1);
    };
    alloc.free_fn = [](void * c, void * p) {
      if (p) {
        static_cast<AtomicAllocator *>(c)->freed++;
      }
      std::free(p);
    };
  }
};

extern "C" GTEXT_CSS_Status css_registry_property_compiled(const GTEXT_CSS_Registry *, GTEXT_CSS_Property_Id, GTEXT_CSS_Grammar_State *,
    const GTEXT_CSS_Compiled **, GTEXT_CSS_Grammar_Result *);

TEST(CssRegistryGrammars, ManyThreadsAskingAtOnceGetOneCompiledGrammarEach) {
  // Fresh registries, all threads released together by a barrier, the same few grammars asked for by every thread: each is compiled
  // once (every thread sees the same object, and nothing is left unfreed), however the threads interleave. Repeated, because a race
  // that is missing a lock shows in some interleavings only.
  const char * names[] = {"margin", "background", "font", "transform", "grid-template-columns", "border", "color", "width"};
  const int nthreads = 8;
  long bad = 0;
  for (int round = 0; round < 60; round++) {
    AtomicAllocator counting;
    GTEXT_CSS_Registry_Options ro = gtext_css_registry_options_default();
    ro.allocator = &counting.alloc;
    GTEXT_CSS_Registry * r = nullptr;
    ASSERT_EQ(gtext_css_registry_new(&ro, &r), GTEXT_CSS_OK);
    std::vector<GTEXT_CSS_Property_Id> ids;
    for (const char * n : names) {
      ids.push_back(find(r, n));
    }
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::vector<const GTEXT_CSS_Compiled *>> seen(nthreads, std::vector<const GTEXT_CSS_Compiled *>(ids.size(), nullptr));
    std::vector<std::thread> threads;
    for (int t = 0; t < nthreads; t++) {
      threads.emplace_back([&, t] {
        ready++;
        while (!go.load()) {
        }
        for (size_t k = 0; k < ids.size(); k++) {
          GTEXT_CSS_Grammar_State s;
          const size_t at = (k + (size_t)t) % ids.size();
          const GTEXT_CSS_Compiled * c = nullptr;
          if (css_registry_property_compiled(r, ids[at], &s, &c, nullptr) != GTEXT_CSS_OK || s != GTEXT_CSS_GRAMMAR_OK) {
            bad++;
          }
          seen[t][at] = c;
        }
      });
    }
    while (ready.load() < nthreads) {
    }
    go = true;
    for (auto & th : threads) {
      th.join();
    }
    for (size_t k = 0; k < ids.size(); k++) {
      for (int t = 1; t < nthreads; t++) {
        if (seen[t][k] != seen[0][k] || !seen[0][k]) {
          bad++;
        }
      }
    }
    gtext_css_registry_free(r);
    if (counting.made.load() != counting.freed.load()) {
      bad++;
    }
  }
  EXPECT_EQ(bad, 0) << "a grammar was compiled twice, or a thread saw an incomplete one";
}

TEST(CssRegistryGrammars, ManyThreadsParsingAtOnce) {
  GTEXT_CSS_Registry * r = nullptr;
  ASSERT_EQ(gtext_css_registry_new(nullptr, &r), GTEXT_CSS_OK);
  const std::vector<std::pair<const char *, const char *>> cases = {
      {"margin", "1px 2px"}, {"background", "red url(a.png) no-repeat"}, {"font", "12px serif"}, {"transform", "rotate(3deg) scale(2)"},
      {"grid-template-columns", "1fr 2fr"}, {"color", "red"}, {"border", "1px solid red"}, {"width", "calc(10px + 5%)"}};
  std::atomic<int> bad{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([&, t] {
      for (int round = 0; round < 3; round++) {
        for (size_t k = 0; k < cases.size(); k++) {
          const auto & c = cases[(k + t) % cases.size()];
          if (parse(r, c.first, c.second).rfind("valid:", 0) != 0) {
            bad++;
          }
        }
      }
    });
  }
  for (auto & th : threads) {
    th.join();
  }
  EXPECT_EQ(bad.load(), 0);
  gtext_css_registry_free(r);
}

TEST(CssRegistryGrammars, ThePendingGrammarServesEveryProperty) {
  const GTEXT_CSS_Registry * r = shared();
  EXPECT_EQ(parse(r, "margin", "var(--m)"), "pending:var(--m)");
  EXPECT_EQ(parse(r, "color", "env(x)"), "pending:env(x)");
}

}  // namespace
