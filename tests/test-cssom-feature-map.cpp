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
 * CSSFontFeatureValuesMap (CSS Fonts 4, 12.2): the value maps of a `@font-feature-values` rule. "Each value map attribute of CSSFontFeatureValuesRule reflects the values defined via a
 * corresponding feature value block"; "a single unsigned long value is treated as a sequence of a single value"; "If the associated feature value block only allows a limited number of
 * values, the set() method throws an InvalidAccessError exception when the input sequence to set() contains more than the limited number of values." The pinned pages
 * css/css-fonts/font-feature-values-map-live.html and css/cssom/CSSFontFeatureValuesRule.html are carried here.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cssom_test_support.h"

using namespace cot;

namespace {

std::string read_entry(GTEXT_CSSOM_Rule * r, GTEXT_CSS_Feature_Type t, const char * name) {
  GTEXT_CSSOM_Feature_Entry e;
  bool found = false;
  const GTEXT_CSS_Status st = gtext_cssom_feature_map_get(r, t, name, std::strlen(name), &e, &found);
  if (st != GTEXT_CSS_OK) {
    return std::string("<") + gtext_css_status_string(st) + ">";
  }
  if (!found) {
    return "undefined";
  }
  std::string s;
  for (size_t i = 0; i < e.value_count; i++) {
    s += (i ? " " : "") + std::to_string(e.values[i]);
  }
  gtext_cssom_buffer_free(&reinterpret_cast<css_obj *>(r)->core->alloc, static_cast<char *>(e.block));
  return s;
}

std::string keys(GTEXT_CSSOM_Rule * r, GTEXT_CSS_Feature_Type t) {
  size_t n = 0;
  if (gtext_cssom_feature_map_size(r, t, &n) != GTEXT_CSS_OK) {
    return "<error>";
  }
  std::string s;
  for (size_t i = 0; i < n; i++) {
    GTEXT_CSSOM_Feature_Entry e;
    if (gtext_cssom_feature_map_at(r, t, i, &e) != GTEXT_CSS_OK) {
      return "<error>";
    }
    s += (i ? "," : "") + std::string(e.name, e.name_length);
    gtext_cssom_buffer_free(&reinterpret_cast<css_obj *>(r)->core->alloc, static_cast<char *>(e.block));
  }
  return s;
}

Result set_values(GTEXT_CSSOM_Rule * r, GTEXT_CSS_Feature_Type t, const char * name, std::vector<uint32_t> v) {
  Result res;
  res.st = gtext_cssom_feature_map_set(r, t, name, std::strlen(name), v.data(), v.size(), &res.o);
  return res;
}

}  // namespace

TEST(CssomFeatureMap, TheMapsReflectTheBlocksOfTheRule) {
  Doc doc;
  Sheet s(doc, "@font-feature-values test_family { @annotation { the_first: 6; } @styleset { yo: 7; del: 4; di: 10 9 4 5; } }");
  Rule r = path(s, {0});
  EXPECT_EQ(keys(r.r, GTEXT_CSS_FEATURE_ANNOTATION), "the_first");
  EXPECT_EQ(read_entry(r.r, GTEXT_CSS_FEATURE_ANNOTATION, "the_first"), "6");
  EXPECT_EQ(keys(r.r, GTEXT_CSS_FEATURE_STYLESET), "yo,del,di");
  EXPECT_EQ(read_entry(r.r, GTEXT_CSS_FEATURE_STYLESET, "di"), "10 9 4 5");
  EXPECT_EQ(read_entry(r.r, GTEXT_CSS_FEATURE_STYLESET, "nothing"), "undefined");
  EXPECT_EQ(keys(r.r, GTEXT_CSS_FEATURE_SWASH), "") << "a map of a block the rule does not have is empty";
}

TEST(CssomFeatureMap, SetReplacesInPlaceOrAppendsAndTheRuleKeepsItsIdentity) {
  Doc doc;
  Sheet s(doc, "@font-feature-values MyFamily { @annotation { circled: 1; black-boxed: 3; } }");
  Rule r = path(s, {0});
  const uint64_t id = gtext_cssom_rule_id(r.r), version = gtext_cssom_rule_version(r.r), sheet_version = gtext_cssom_sheet_version(s.s);
  doc.events.clear();
  Result res = set_values(r.r, GTEXT_CSS_FEATURE_ANNOTATION, "black-boxed", {42});
  ASSERT_EQ(res.st, GTEXT_CSS_OK);
  EXPECT_EQ(res.o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(read_entry(r.r, GTEXT_CSS_FEATURE_ANNOTATION, "black-boxed"), "42");
  EXPECT_EQ(keys(r.r, GTEXT_CSS_FEATURE_ANNOTATION), "circled,black-boxed") << "the place of a replaced name is kept (the Map's set())";
  EXPECT_EQ(gtext_cssom_rule_id(r.r), id);
  EXPECT_EQ(gtext_cssom_rule_version(r.r), version + 1);
  EXPECT_EQ(gtext_cssom_sheet_version(s.s), sheet_version + 1);
  ASSERT_EQ(doc.events.size(), 1u);
  res = set_values(r.r, GTEXT_CSS_FEATURE_ANNOTATION, "circled", {9});
  ASSERT_EQ(res.st, GTEXT_CSS_OK);
  EXPECT_EQ(keys(r.r, GTEXT_CSS_FEATURE_ANNOTATION), "circled,black-boxed") << "a name set again keeps its place even when it is the first";
  EXPECT_EQ(css(r.r), "@font-feature-values MyFamily {\n  @annotation { circled: 9; black-boxed: 42; }\n}");
  res = set_values(r.r, GTEXT_CSS_FEATURE_ANNOTATION, "circled", {1});
  res = set_values(r.r, GTEXT_CSS_FEATURE_ANNOTATION, "new", {7});
  ASSERT_EQ(res.st, GTEXT_CSS_OK);
  EXPECT_EQ(keys(r.r, GTEXT_CSS_FEATURE_ANNOTATION), "circled,black-boxed,new");
  EXPECT_EQ(css(r.r), "@font-feature-values MyFamily {\n  @annotation { circled: 1; black-boxed: 42; new: 7; }\n}");
  EXPECT_EQ(path(s, {0}).r, r.r) << "the same object";
  res = set_values(r.r, GTEXT_CSS_FEATURE_ANNOTATION, "new", {7});
  EXPECT_EQ(res.o.kind, GTEXT_CSSOM_UNCHANGED) << "a set that says what the map says is not a change";
}

TEST(CssomFeatureMap, TheSecondSheetIsNotAffected) {
  Doc doc;
  Sheet a(doc, "@font-feature-values MyFamily { @annotation { black-boxed: 3; } }");
  Sheet b(doc, "@font-feature-values MyFamily { @annotation { circled: 1; black-boxed: 3; } }");
  ASSERT_EQ(set_values(path(a, {0}).r, GTEXT_CSS_FEATURE_ANNOTATION, "black-boxed", {42}).st, GTEXT_CSS_OK);
  EXPECT_EQ(read_entry(path(a, {0}).r, GTEXT_CSS_FEATURE_ANNOTATION, "black-boxed"), "42");
  EXPECT_EQ(read_entry(path(b, {0}).r, GTEXT_CSS_FEATURE_ANNOTATION, "black-boxed"), "3");
}

TEST(CssomFeatureMap, TheLimitedBlocksRefuseTooManyValuesWithInvalidAccessError) {
  Doc doc;
  Sheet s(doc, "@font-feature-values F { }");
  Rule r = path(s, {0});
  struct Row { GTEXT_CSS_Feature_Type t; size_t most; };
  for (const Row & row : {Row{GTEXT_CSS_FEATURE_STYLISTIC, 1}, Row{GTEXT_CSS_FEATURE_HISTORICAL_FORMS, 1}, Row{GTEXT_CSS_FEATURE_SWASH, 1}, Row{GTEXT_CSS_FEATURE_ORNAMENTS, 1},
          Row{GTEXT_CSS_FEATURE_ANNOTATION, 1}, Row{GTEXT_CSS_FEATURE_CHARACTER_VARIANT, 2}}) {
    std::vector<uint32_t> ok(row.most, 1), too_many(row.most + 1, 1);
    const std::string before = dump(s.s);
    Result res = set_values(r.r, row.t, "x", too_many);
    EXPECT_EQ(res.st, GTEXT_CSS_E_INVALID_ACCESS) << row.t;
    EXPECT_STREQ(gtext_cssom_exception_name(res.st), "InvalidAccessError");
    EXPECT_EQ(dump(s.s), before) << "a refused set changes nothing";
    EXPECT_EQ(set_values(r.r, row.t, "x", ok).st, GTEXT_CSS_OK) << row.t;
    EXPECT_EQ(set_values(r.r, row.t, "x", {}).st, GTEXT_CSS_E_INVALID_ACCESS) << "an empty sequence is an invalid number of values";
    EXPECT_EQ(gtext_cssom_feature_map_delete(r.r, row.t, "x", 1, nullptr, nullptr), GTEXT_CSS_OK);
  }
  std::vector<uint32_t> many(40, 3);
  EXPECT_EQ(set_values(r.r, GTEXT_CSS_FEATURE_STYLESET, "sets", many).st, GTEXT_CSS_OK) << "styleset takes any number of values";
  EXPECT_EQ(read_entry(r.r, GTEXT_CSS_FEATURE_STYLESET, "sets").size(), 40u * 2 - 1);
}

TEST(CssomFeatureMap, DeleteAndClear) {
  Doc doc;
  Sheet s(doc, "@font-feature-values F { @swash { a: 1; b: 2; c: 3; } @ornaments { o: 4; } }");
  Rule r = path(s, {0});
  bool deleted = true;
  Result res;
  res.st = gtext_cssom_feature_map_delete(r.r, GTEXT_CSS_FEATURE_SWASH, "zz", 2, &deleted, &res.o);
  EXPECT_EQ(res.st, GTEXT_CSS_OK);
  EXPECT_FALSE(deleted);
  EXPECT_EQ(res.o.kind, GTEXT_CSSOM_UNCHANGED);
  res.st = gtext_cssom_feature_map_delete(r.r, GTEXT_CSS_FEATURE_SWASH, "b", 1, &deleted, &res.o);
  EXPECT_EQ(res.st, GTEXT_CSS_OK);
  EXPECT_TRUE(deleted);
  EXPECT_EQ(keys(r.r, GTEXT_CSS_FEATURE_SWASH), "a,c");
  res.st = gtext_cssom_feature_map_clear(r.r, GTEXT_CSS_FEATURE_SWASH, &res.o);
  EXPECT_EQ(res.st, GTEXT_CSS_OK);
  EXPECT_EQ(keys(r.r, GTEXT_CSS_FEATURE_SWASH), "");
  EXPECT_EQ(keys(r.r, GTEXT_CSS_FEATURE_ORNAMENTS), "o") << "the other maps stay";
  EXPECT_EQ(css(r.r), "@font-feature-values F {\n  @ornaments { o: 4; }\n}");
  res.st = gtext_cssom_feature_map_clear(r.r, GTEXT_CSS_FEATURE_SWASH, &res.o);
  EXPECT_EQ(res.o.kind, GTEXT_CSSOM_UNCHANGED);
}

TEST(CssomFeatureMap, TheRuleKeepsItsBlocksWhenItsFamilyIsSet) {
  Doc doc;
  Sheet s(doc, "@font-feature-values A { @swash { a: 1; } }");
  Rule r = path(s, {0});
  Result res;
  res.st = gtext_cssom_rule_set_attribute(r.r, "fontFamily", 10, "B, C", 4, &res.o);
  ASSERT_EQ(res.st, GTEXT_CSS_OK);
  EXPECT_EQ(read_entry(r.r, GTEXT_CSS_FEATURE_SWASH, "a"), "1") << "replacing the family list does not drop the feature blocks";
}

TEST(CssomFeatureMap, OtherRulesAndOtherArguments) {
  Doc doc;
  Sheet s(doc, "a {} @font-feature-values F {}");
  size_t n = 0;
  EXPECT_EQ(gtext_cssom_feature_map_size(path(s, {0}).r, GTEXT_CSS_FEATURE_SWASH, &n), GTEXT_CSS_E_UNSUPPORTED) << "a style rule has no value maps";
  EXPECT_EQ(gtext_cssom_feature_map_size(nullptr, GTEXT_CSS_FEATURE_SWASH, &n), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_feature_map_size(path(s, {1}).r, GTEXT_CSS_FEATURE_TYPE_COUNT, &n), GTEXT_CSS_E_INVALID);
  GTEXT_CSS_Feature_Type t;
  EXPECT_TRUE(gtext_cssom_feature_map_type("characterVariant", 16, &t));
  EXPECT_EQ(t, GTEXT_CSS_FEATURE_CHARACTER_VARIANT);
  EXPECT_TRUE(gtext_cssom_feature_map_type("historicalForms", 15, &t));
  EXPECT_EQ(t, GTEXT_CSS_FEATURE_HISTORICAL_FORMS);
  EXPECT_FALSE(gtext_cssom_feature_map_type("swish", 5, &t));
  EXPECT_FALSE(gtext_cssom_feature_map_type("character-variant", 17, &t)) << "the IDL name, not the at-rule's";
}

TEST(CssomFeatureMap, IterationWhileModifyingDoesNotCrash) {
  // css/css-fonts/font_feature_values_map_iteration.html: take the entry at an index, delete it, add a hundred.
  Doc doc;
  Sheet s(doc, "@font-feature-values TestFont { @styleset { a: 1; b: 2; c: 3; } }");
  Rule r = path(s, {0});
  for (size_t count = 0; count < 10; count++) {
    GTEXT_CSSOM_Feature_Entry e;
    if (gtext_cssom_feature_map_at(r.r, GTEXT_CSS_FEATURE_STYLESET, 0, &e) != GTEXT_CSS_OK) {
      break;
    }
    const std::string key(e.name, e.name_length);
    gtext_cssom_buffer_free(&reinterpret_cast<css_obj *>(r.r)->core->alloc, static_cast<char *>(e.block));
    bool deleted = false;
    ASSERT_EQ(gtext_cssom_feature_map_delete(r.r, GTEXT_CSS_FEATURE_STYLESET, key.data(), key.size(), &deleted, nullptr), GTEXT_CSS_OK);
    for (uint32_t i = 0; i < 20; i++) {
      const std::string k = "newkey_" + std::to_string(count) + "_" + std::to_string(i);
      ASSERT_EQ(set_values(r.r, GTEXT_CSS_FEATURE_STYLESET, k.c_str(), {i}).st, GTEXT_CSS_OK);
    }
  }
  size_t n = 0;
  ASSERT_EQ(gtext_cssom_feature_map_size(r.r, GTEXT_CSS_FEATURE_STYLESET, &n), GTEXT_CSS_OK);
  EXPECT_EQ(n, 3u + 10u * 19u) << "each round deletes the first entry and adds twenty (a hundred in the page: the same shape, a fifth of the time)";
}

TEST(CssomFeatureMap, NamesAreCaseSensitiveIdentifiersAndWrittenEscaped) {
  Doc doc;
  Sheet s(doc, "@font-feature-values F { @swash { Foo: 1; foo: 2; } }");
  Rule r = path(s, {0});
  EXPECT_EQ(keys(r.r, GTEXT_CSS_FEATURE_SWASH), "Foo,foo") << "feature value names are case-sensitive";
  ASSERT_EQ(set_values(r.r, GTEXT_CSS_FEATURE_SWASH, "a b", {3}).st, GTEXT_CSS_OK);
  EXPECT_EQ(read_entry(r.r, GTEXT_CSS_FEATURE_SWASH, "a b"), "3");
  EXPECT_EQ(css(r.r), "@font-feature-values F {\n  @swash { Foo: 1; foo: 2; a\\ b: 3; }\n}");
  Result res = set_values(r.r, GTEXT_CSS_FEATURE_SWASH, "", {4});
  EXPECT_EQ(res.st, GTEXT_CSS_OK);
  EXPECT_EQ(res.o.kind, GTEXT_CSSOM_IGNORED_INVALID_VALUE) << "the empty name cannot be written as an identifier";
  EXPECT_EQ(keys(r.r, GTEXT_CSS_FEATURE_SWASH), "Foo,foo,a b");
}
