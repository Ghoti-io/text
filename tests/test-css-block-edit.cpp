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
 * The editing half of the declaration block (css_shorthand.h): gtext_css_block_set(), _set_custom(), _remove() and _find() with the index behind them, each
 * step of CSSOM's "set a CSS declaration" and `removeProperty()` named by the sentence it comes from, the amortized cost asserted on the block's probe
 * counter, the limit, and allocation failure at every allocation of every call.
 */

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_compute_test_support.h"
#include "markup_test_support.h"

using namespace cmp;

namespace {

/// Parse `value` for `name` and set it in the block. Returns the status of the set; `parsed` says what the parse came to.
GTEXT_CSS_Status set(GTEXT_CSS_Declaration_Block * b, const std::string & name, const std::string & value, bool important = false,
    GTEXT_CSS_Block_Change * change = nullptr, GTEXT_CSS_Property_Outcome * parsed = nullptr, const GTEXT_Allocator * alloc = nullptr) {
  GTEXT_CSS_Property_Options po;
  std::memset(&po, 0, sizeof po);
  po.allocator = alloc;
  GTEXT_CSS_Property_Value pv;
  GTEXT_CSS_Status st = gtext_css_parse_property_text(R(), name.data(), name.size(), value.data(), value.size(), &po, &pv);
  if (parsed) {
    *parsed = pv.outcome;
  }
  if (st == GTEXT_CSS_OK) {
    if (name.size() > 1 && name[0] == '-' && name[1] == '-') {
      st = gtext_css_block_set_custom(b, name.data(), name.size(), &pv, important, change);
    }
    else if (pv.outcome == GTEXT_CSS_PROP_RESULT_VALID || pv.outcome == GTEXT_CSS_PROP_RESULT_WIDE || pv.outcome == GTEXT_CSS_PROP_RESULT_PENDING) {
      st = gtext_css_block_set(b, &pv, important, change, nullptr);
    }
    else {
      st = GTEXT_CSS_E_INVALID;
    }
  }
  gtext_css_property_value_free(&pv);
  return st;
}

GTEXT_CSS_Status remove(GTEXT_CSS_Declaration_Block * b, const std::string & name, GTEXT_CSS_Block_Change * change = nullptr) {
  if (name.size() > 1 && name[0] == '-' && name[1] == '-') {
    return gtext_css_block_remove(b, 0, name.data(), name.size(), change);
  }
  return gtext_css_block_remove(b, find(R(), name), nullptr, 0, change);
}

std::string css_of(const GTEXT_CSS_Declaration_Block * b) {
  char * out = nullptr;
  size_t n = 0;
  if (gtext_css_block_serialize_buffer(b, &out, &n) != GTEXT_CSS_OK) {
    return "<error>";
  }
  std::string s(out, n);
  gtext_css_serialize_buffer_free(nullptr, out);
  return s;
}

/// The names of the changed longhands.
std::string changed_names(const GTEXT_CSS_Block_Change & c) {
  std::string out;
  for (size_t i = 0; i < c.count; i++) {
    GTEXT_CSS_Property_Info info;
    EXPECT_TRUE(gtext_css_registry_property_info(R(), c.ids[i], &info));
    out += (out.empty() ? "" : " ") + std::string(info.name, info.name_length);
  }
  return out;
}

/// The declarations in order as `name:count` of the live items (a dense walk through gtext_css_block_item()).
std::string order_of(const GTEXT_CSS_Declaration_Block * b) {
  std::string out;
  for (size_t i = 0; i < gtext_css_block_count(b); i++) {
    GTEXT_CSS_Declaration_Item it;
    EXPECT_TRUE(gtext_css_block_item(b, i, &it));
    GTEXT_CSS_Property_Info info;
    EXPECT_TRUE(gtext_css_registry_property_info(R(), it.property, &info));
    out += (out.empty() ? "" : " ") + (it.custom_name.data ? std::string(it.custom_name.data, it.custom_name.length) : std::string(info.name, info.name_length));
  }
  return out;
}

/* ---- "set a CSS declaration" ----------------------------------------------------------------------------------------------------- */

// CSSOM, "set a CSS declaration": "If property is a case-sensitive match for a property name of a CSS declaration in declarations, let declaration be that CSS declaration,
// or null otherwise." / "If value is not null... update declaration's value ... return updated." / "Append a new CSS declaration ... return true."
TEST(CssBlockSet, ReplacesTheDeclarationInItsPlaceElseAppends) {
  Block b(R(), "margin-top: 5px; color: red");
  GTEXT_CSS_Block_Change change{};
  ASSERT_EQ(set(b.b, "margin-top", "6px", false, &change), GTEXT_CSS_OK);
  EXPECT_TRUE(change.changed);
  EXPECT_EQ(changed_names(change), "margin-top");
  EXPECT_EQ(order_of(b.b), "margin-top color");
  ASSERT_EQ(set(b.b, "padding-left", "1px", false, &change), GTEXT_CSS_OK);
  EXPECT_EQ(order_of(b.b), "margin-top color padding-left");
  EXPECT_EQ(css_of(b.b), "margin-top: 6px; color: red; padding-left: 1px;");
  gtext_css_block_change_free(&change);
}

// CSSOM, setProperty: "If property is a shorthand property, then for each longhand property longhand that property maps to, in canonical order, follow these substeps: ... set the CSS
// declaration longhand". The longhand already in the block is replaced where it stands; the others are appended in the shorthand's order.
TEST(CssBlockSet, AShorthandReplacesItsLonghandInPlaceAndAppendsTheRestInOrder) {
  Block b(R(), "margin-top: 5px; color: red");
  GTEXT_CSS_Block_Change change{};
  ASSERT_EQ(set(b.b, "margin", "1px 2px", false, &change), GTEXT_CSS_OK);
  EXPECT_EQ(order_of(b.b), "margin-top color margin-right margin-bottom margin-left");
  EXPECT_EQ(changed_names(change), "margin-top margin-right margin-bottom margin-left");
  EXPECT_EQ(css_of(b.b), "margin: 1px 2px; color: red;");
  gtext_css_block_change_free(&change);
}

// "update a CSS declaration": a replacement that is byte-identical (same value text and importance) is not an update.
TEST(CssBlockSet, ASetThatSaysWhatTheBlockAlreadySaysChangesNothing) {
  Block b(R(), "margin: 1px 2px; color: red !important");
  GTEXT_CSS_Block_Change change{};
  const std::string before = css_of(b.b);
  ASSERT_EQ(set(b.b, "margin", "1px 2px", false, &change), GTEXT_CSS_OK);
  EXPECT_FALSE(change.changed);
  EXPECT_EQ(change.count, 0u);
  ASSERT_EQ(set(b.b, "color", "red", true, &change), GTEXT_CSS_OK);
  EXPECT_FALSE(change.changed);
  ASSERT_EQ(set(b.b, "margin-left", "2px", false, &change), GTEXT_CSS_OK);
  EXPECT_FALSE(change.changed) << "margin: 1px 2px has margin-left 2px";
  EXPECT_EQ(css_of(b.b), before);
  // the importance is part of the declaration
  ASSERT_EQ(set(b.b, "color", "red", false, &change), GTEXT_CSS_OK);
  EXPECT_TRUE(change.changed);
  EXPECT_EQ(css_of(b.b), "margin: 1px 2px; color: red;");
  // and so is the spelling of the value as it is written back
  ASSERT_EQ(set(b.b, "color", "RED", false, &change), GTEXT_CSS_OK);
  EXPECT_FALSE(change.changed) << "RED and red are the same specified value";
  gtext_css_block_change_free(&change);
}

TEST(CssBlockSet, OnlyTheLonghandsThatDifferAreChanged) {
  Block b(R(), "margin: 1px 2px");
  GTEXT_CSS_Block_Change change{};
  ASSERT_EQ(set(b.b, "margin", "1px 3px", false, &change), GTEXT_CSS_OK);
  EXPECT_TRUE(change.changed);
  EXPECT_EQ(changed_names(change), "margin-right margin-left");
  gtext_css_block_change_free(&change);
}

TEST(CssBlockSet, ACssWideKeywordAndAPendingValueAreCarried) {
  Block b(R(), "color: red");
  GTEXT_CSS_Block_Change change{};
  ASSERT_EQ(set(b.b, "color", "inherit", false, &change), GTEXT_CSS_OK);
  EXPECT_TRUE(change.changed);
  EXPECT_EQ(css_of(b.b), "color: inherit;");
  ASSERT_EQ(set(b.b, "color", "inherit", false, &change), GTEXT_CSS_OK);
  EXPECT_FALSE(change.changed);
  ASSERT_EQ(set(b.b, "margin", "var(--x)", false, &change), GTEXT_CSS_OK);
  EXPECT_EQ(css_of(b.b), "color: inherit; margin: var(--x);");
  ASSERT_EQ(set(b.b, "margin", "var(--x)", false, &change), GTEXT_CSS_OK);
  EXPECT_FALSE(change.changed) << "the same pending value, written the same";
  ASSERT_EQ(set(b.b, "margin", "var(--y)", false, &change), GTEXT_CSS_OK);
  EXPECT_TRUE(change.changed);
  EXPECT_EQ(css_of(b.b), "color: inherit; margin: var(--y);");
  gtext_css_block_change_free(&change);
}

// CSS Variables 1: a custom property's name is case-sensitive.
TEST(CssBlockSet, ACustomPropertyIsKeyedByItsCaseSensitiveName) {
  Block b(R());
  GTEXT_CSS_Block_Change change{};
  ASSERT_EQ(set(b.b, "--Foo", " a  b ", false, &change), GTEXT_CSS_OK);
  EXPECT_TRUE(change.changed);
  ASSERT_EQ(set(b.b, "--foo", "c", false, &change), GTEXT_CSS_OK);
  EXPECT_TRUE(change.changed);
  EXPECT_EQ(gtext_css_block_count(b.b), 2u) << "--Foo and --foo are two properties";
  ASSERT_EQ(set(b.b, "--Foo", "a  b", false, &change), GTEXT_CSS_OK);
  EXPECT_FALSE(change.changed) << "the value is the token text, trimmed";
  ASSERT_EQ(set(b.b, "--Foo", "z", false, &change), GTEXT_CSS_OK);
  EXPECT_TRUE(change.changed);
  EXPECT_EQ(order_of(b.b), "--Foo --foo");
  EXPECT_EQ(css_of(b.b), "--Foo: z; --foo: c;");
  gtext_css_block_change_free(&change);
}

// A longhand that a shorthand wrote is replaced by a longhand set, and it is then the longhand's declaration: the shorthand no longer reads back.
TEST(CssBlockSet, ALonghandSetAfterAShorthandBreaksTheShorthandReadBack) {
  Block b(R(), "margin: 1px");
  GTEXT_CSS_Block_Change change{};
  ASSERT_EQ(set(b.b, "margin-top", "2px", false, &change), GTEXT_CSS_OK);
  EXPECT_EQ(css_of(b.b), "margin: 2px 1px 1px;");
  gtext_css_block_change_free(&change);
}

TEST(CssBlockSet, AnAliasIsItsProperty) {
  Block b(R(), "word-wrap: break-word");
  GTEXT_CSS_Block_Change change{};
  ASSERT_EQ(set(b.b, "overflow-wrap", "normal", false, &change), GTEXT_CSS_OK);
  EXPECT_TRUE(change.changed);
  EXPECT_EQ(gtext_css_block_count(b.b), 1u);
  gtext_css_block_change_free(&change);
}

TEST(CssBlockSet, ABadArgumentIsInvalidAndChangesNothing) {
  Block b(R(), "color: red");
  GTEXT_CSS_Property_Value pv;
  ASSERT_EQ(gtext_css_parse_property_text(R(), "color", 5, "bogus!", 6, nullptr, &pv), GTEXT_CSS_OK);
  EXPECT_EQ(pv.outcome, GTEXT_CSS_PROP_RESULT_INVALID);
  EXPECT_EQ(gtext_css_block_set(b.b, &pv, false, nullptr, nullptr), GTEXT_CSS_E_INVALID);
  gtext_css_property_value_free(&pv);
  EXPECT_EQ(gtext_css_block_set(nullptr, &pv, false, nullptr, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_block_set_custom(b.b, "x", 1, &pv, false, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(css_of(b.b), "color: red;");
}

/* ---- removeProperty ---------------------------------------------------------------------------------------------------------------- */

// CSSOM, removeProperty: "If property is a shorthand property, for each longhand property longhand that property maps to: ... Remove longhand from declarations"
// and "Otherwise, if property is a case-sensitive match for a property name of a CSS declaration in declarations, remove that CSS declaration".
TEST(CssBlockRemove, ALonghandAShorthandAndACustomProperty) {
  Block b(R(), "margin: 1px 2px; color: red; --a: 1; padding-top: 3px");
  GTEXT_CSS_Block_Change change{};
  ASSERT_EQ(remove(b.b, "color", &change), GTEXT_CSS_OK);
  EXPECT_TRUE(change.changed);
  EXPECT_EQ(changed_names(change), "color");
  EXPECT_EQ(css_of(b.b), "margin: 1px 2px; --a: 1; padding-top: 3px;");
  ASSERT_EQ(remove(b.b, "margin", &change), GTEXT_CSS_OK);
  EXPECT_EQ(changed_names(change), "margin-top margin-right margin-bottom margin-left");
  EXPECT_EQ(css_of(b.b), "--a: 1; padding-top: 3px;");
  ASSERT_EQ(remove(b.b, "--A", &change), GTEXT_CSS_OK);
  EXPECT_FALSE(change.changed) << "--A is not --a";
  ASSERT_EQ(remove(b.b, "--a", &change), GTEXT_CSS_OK);
  EXPECT_TRUE(change.changed);
  EXPECT_EQ(css_of(b.b), "padding-top: 3px;");
  EXPECT_EQ(gtext_css_block_count(b.b), 1u);
  gtext_css_block_change_free(&change);
}

TEST(CssBlockRemove, ANoOpRemovesNothing) {
  Block b(R(), "color: red");
  GTEXT_CSS_Block_Change change{};
  ASSERT_EQ(remove(b.b, "width", &change), GTEXT_CSS_OK);
  EXPECT_FALSE(change.changed);
  EXPECT_EQ(css_of(b.b), "color: red;");
  // removing a shorthand of which only some longhands are present removes those
  Block m(R(), "margin-top: 1px");
  ASSERT_EQ(remove(m.b, "margin", &change), GTEXT_CSS_OK);
  EXPECT_TRUE(change.changed);
  EXPECT_EQ(changed_names(change), "margin-top");
  gtext_css_block_change_free(&change);
}

TEST(CssBlockRemove, EveryDeclarationOfAPropertyGoes) {
  // a block the cascade builds can hold a property twice; removeProperty removes the declaration, which is both
  Block b(R(), "color: red; width: 1px; color: blue");
  GTEXT_CSS_Block_Change change{};
  ASSERT_EQ(remove(b.b, "color", &change), GTEXT_CSS_OK);
  EXPECT_EQ(css_of(b.b), "width: 1px;");
  EXPECT_EQ(gtext_css_block_count(b.b), 1u);
  GTEXT_CSS_Declaration_Item it;
  EXPECT_FALSE(gtext_css_block_find(b.b, find(R(), "color"), nullptr, 0, &it));
  gtext_css_block_change_free(&change);
}

/* The header says of `out`: "Receives the declaration; NULL if not wanted." The call that only wants to know whether the block has the property must be answered. */
TEST(CssBlockFind, AskingWithNoOutputIsAskingWhetherTheBlockHasTheProperty) {
  Block b(R(), "color: red; --p: 1");
  EXPECT_TRUE(gtext_css_block_find(b.b, find(R(), "color"), nullptr, 0, nullptr));
  EXPECT_FALSE(gtext_css_block_find(b.b, find(R(), "width"), nullptr, 0, nullptr));
  EXPECT_TRUE(gtext_css_block_find(b.b, 0, "--p", 3, nullptr));
  EXPECT_FALSE(gtext_css_block_find(b.b, 0, "--q", 3, nullptr));
}

TEST(CssBlockRemove, AShorthandRemovesWhatItResets) {
  Block b(R(), "font: 12px serif; color: red");
  GTEXT_CSS_Block_Change change{};
  ASSERT_EQ(remove(b.b, "font", &change), GTEXT_CSS_OK);
  EXPECT_EQ(css_of(b.b), "color: red;");
  gtext_css_block_change_free(&change);
}

TEST(CssBlockRemove, TheOrderOfWhatIsLeftIsKeptAcrossTheSqueeze) {
  std::string decls;
  for (int i = 0; i < 40; i++) {
    decls += "--p" + std::to_string(i) + ": " + std::to_string(i) + "; ";
  }
  Block b(R(), decls);
  for (int i = 0; i < 40; i += 2) {
    ASSERT_EQ(remove(b.b, "--p" + std::to_string(i)), GTEXT_CSS_OK);
  }
  ASSERT_EQ(gtext_css_block_count(b.b), 20u);
  for (size_t i = 0; i < 20; i++) {
    GTEXT_CSS_Declaration_Item it;
    ASSERT_TRUE(gtext_css_block_item(b.b, i, &it));
    EXPECT_EQ(std::string(it.custom_name.data, it.custom_name.length), "--p" + std::to_string(2 * i + 1));
  }
  GTEXT_CSS_Declaration_Item it;
  EXPECT_FALSE(gtext_css_block_item(b.b, 20, &it));
  // and a set after the removals goes at the end, and the removed names are gone from the index
  ASSERT_EQ(set(b.b, "--p0", "again"), GTEXT_CSS_OK);
  ASSERT_TRUE(gtext_css_block_item(b.b, 20, &it));
  EXPECT_EQ(std::string(it.custom_name.data, it.custom_name.length), "--p0");
  EXPECT_FALSE(gtext_css_block_find(b.b, 0, "--p2", 4, &it));
  EXPECT_TRUE(gtext_css_block_find(b.b, 0, "--p3", 4, &it));
}

/* ---- find -------------------------------------------------------------------------------------------------------------------------- */

TEST(CssBlockFind, TheLastDeclarationOfAPropertyAnAliasAndACustomName) {
  Block b(R(), "color: red; --A: 1; word-wrap: normal; color: blue");
  GTEXT_CSS_Declaration_Item it;
  ASSERT_TRUE(gtext_css_block_find(b.b, find(R(), "color"), nullptr, 0, &it));
  EXPECT_EQ(it.declaration, 3u) << "the last declaration of color";
  ASSERT_TRUE(gtext_css_block_find(b.b, find(R(), "overflow-wrap"), nullptr, 0, &it));
  ASSERT_TRUE(gtext_css_block_find(b.b, find(R(), "word-wrap"), nullptr, 0, &it)) << "an alias is followed";
  EXPECT_TRUE(gtext_css_block_find(b.b, 0, "--A", 3, &it));
  EXPECT_FALSE(gtext_css_block_find(b.b, 0, "--a", 3, &it));
  EXPECT_FALSE(gtext_css_block_find(b.b, find(R(), "width"), nullptr, 0, &it));
  EXPECT_FALSE(gtext_css_block_find(nullptr, find(R(), "width"), nullptr, 0, &it));
  EXPECT_FALSE(gtext_css_block_find(b.b, 0, nullptr, 0, &it));
  // the same answers once the index exists
  GTEXT_CSS_Block_Change change{};
  ASSERT_EQ(set(b.b, "width", "1px", false, &change), GTEXT_CSS_OK);
  ASSERT_TRUE(gtext_css_block_find(b.b, find(R(), "color"), nullptr, 0, &it));
  EXPECT_EQ(it.declaration, 3u);
  EXPECT_TRUE(gtext_css_block_find(b.b, 0, "--A", 3, &it));
  EXPECT_FALSE(gtext_css_block_find(b.b, 0, "--a", 3, &it));
  EXPECT_TRUE(gtext_css_block_find(b.b, find(R(), "width"), nullptr, 0, &it));
  gtext_css_block_change_free(&change);
}

/* ---- the pending shorthand (css_shorthand.h) --------------------------------------------------------------------------- */

// CSS Variables 1 / Values 5: "If a shorthand property ... contains var() ... the shorthand property must serialize to that original (arbitrary substitution function-containing) value".
TEST(CssBlockShorthandValue, APendingShorthandReadsAsItsOriginalValue) {
  Block b(R(), "margin: var(--x)");
  char * out = nullptr;
  size_t n = 0;
  ASSERT_EQ(gtext_css_block_shorthand_value(b.b, find(R(), "margin"), &out, &n, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(std::string(out, n), "var(--x)");
  gtext_css_serialize_buffer_free(nullptr, out);
  // the longhands are pending: a longhand has no value of its own
  ASSERT_EQ(gtext_css_block_get_property_value(b.b, find(R(), "margin-top"), &out, &n, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(std::string(out, n), "");
  gtext_css_serialize_buffer_free(nullptr, out);
  // one longhand replaced: no longer one declaration, no shorthand text
  GTEXT_CSS_Block_Change change{};
  ASSERT_EQ(set(b.b, "margin-top", "1px", false, &change), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_block_shorthand_value(b.b, find(R(), "margin"), &out, &n, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(std::string(out, n), "");
  gtext_css_serialize_buffer_free(nullptr, out);
  gtext_css_block_change_free(&change);
}

TEST(CssBlockShorthandValue, MixedImportanceIsEmptyAndTheSameKeywordIsTheKeyword) {
  Block b(R(), "margin-top: 1px !important; margin-right: 1px; margin-bottom: 1px; margin-left: 1px");
  char * out = nullptr;
  size_t n = 0;
  bool important = true;
  ASSERT_EQ(gtext_css_block_shorthand_value(b.b, find(R(), "margin"), &out, &n, &important), GTEXT_CSS_OK);
  EXPECT_EQ(std::string(out, n), "");
  gtext_css_serialize_buffer_free(nullptr, out);
  Block w(R(), "margin: inherit");
  ASSERT_EQ(gtext_css_block_shorthand_value(w.b, find(R(), "margin"), &out, &n, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(std::string(out, n), "inherit");
  gtext_css_serialize_buffer_free(nullptr, out);
}

/* ---- the index: cost --------------------------------------------------------------------------------------------------------------- */

// n replacements of one property, n distinct sets, n removals: the entries the index looks at grow as n (four times the work for four times the declarations).
TEST(CssBlockCost, ReplacingOnePropertyNTimesIsLinear) {
  uint64_t probes[3] = {};
  const size_t sizes[3] = {2000, 8000, 32000};
  for (int k = 0; k < 3; k++) {
    Block b(R());
    GTEXT_CSS_Block_Change change{};
    for (size_t i = 0; i < sizes[k]; i++) {
      ASSERT_EQ(set(b.b, "color", i % 2 ? "red" : "blue", false, &change), GTEXT_CSS_OK);
    }
    probes[k] = gtext_css_block_probes(b.b);
    EXPECT_EQ(gtext_css_block_count(b.b), 1u);
    gtext_css_block_change_free(&change);
  }
  EXPECT_LE(probes[0], 3 * sizes[0]);
  EXPECT_LE(probes[1], 3 * sizes[1]);
  EXPECT_LE(probes[2], 3 * sizes[2]);
  EXPECT_LE(probes[2], 5 * probes[1]);
}

TEST(CssBlockCost, NDistinctSetsAndNRemovalsAreLinear) {
  const size_t sizes[3] = {1000, 4000, 16000};
  uint64_t set_probes[3] = {}, remove_probes[3] = {};
  for (int k = 0; k < 3; k++) {
    Block b(R());
    GTEXT_CSS_Block_Change change{};
    for (size_t i = 0; i < sizes[k]; i++) {
      ASSERT_EQ(set(b.b, "--p" + std::to_string(i), "1", false, &change), GTEXT_CSS_OK);
    }
    set_probes[k] = gtext_css_block_probes(b.b);
    ASSERT_EQ(gtext_css_block_count(b.b), sizes[k]);
    for (size_t i = 0; i < sizes[k]; i++) {
      ASSERT_EQ(remove(b.b, "--p" + std::to_string(i), &change), GTEXT_CSS_OK);
      ASSERT_TRUE(change.changed);
    }
    remove_probes[k] = gtext_css_block_probes(b.b) - set_probes[k];
    EXPECT_EQ(gtext_css_block_count(b.b), 0u);
    gtext_css_block_change_free(&change);
  }
  for (int k = 0; k < 3; k++) {
    EXPECT_LE(set_probes[k], 12 * sizes[k]) << "set, n = " << sizes[k];
    EXPECT_LE(remove_probes[k], 12 * sizes[k]) << "remove, n = " << sizes[k];
  }
  // four times the declarations is four times the work, not sixteen
  EXPECT_LE(set_probes[1], 5 * set_probes[0]);
  EXPECT_LE(set_probes[2], 5 * set_probes[1]);
  EXPECT_LE(remove_probes[1], 5 * remove_probes[0]);
  EXPECT_LE(remove_probes[2], 5 * remove_probes[1]);
}

// Removals and sets interleaved keep the tombstones of the index and the dead slots of the items bounded, so a long edit of a small block stays small.
TEST(CssBlockCost, AddAndRemoveForeverDoesNotGrowTheBlock) {
  Block b(R());
  GTEXT_CSS_Block_Change change{};
  for (size_t i = 0; i < 20000; i++) {
    ASSERT_EQ(set(b.b, "--p" + std::to_string(i), "1", false, &change), GTEXT_CSS_OK);
    if (i >= 8) {
      ASSERT_EQ(remove(b.b, "--p" + std::to_string(i - 8), &change), GTEXT_CSS_OK);
    }
  }
  EXPECT_EQ(gtext_css_block_count(b.b), 8u);
  EXPECT_LE(gtext_css_block_probes(b.b), 20000u * 12u);
  gtext_css_block_change_free(&change);
}

/* ---- the limit ---------------------------------------------------------------------------------------------------------------------- */

TEST(CssBlockLimit, AReplacementAtTheLimitSucceedsAndAnAppendFailsWholly) {
  GTEXT_CSS_Declaration_Options o = gtext_css_declaration_options_default();
  o.max_declarations = 4;
  Block b(R(), "margin: 1px", &o);
  ASSERT_EQ(gtext_css_block_count(b.b), 4u);
  GTEXT_CSS_Block_Change change{};
  EXPECT_EQ(set(b.b, "margin", "2px", false, &change), GTEXT_CSS_OK) << "four replacements, no new declaration";
  EXPECT_TRUE(change.changed);
  const std::string before = css_of(b.b);
  EXPECT_EQ(set(b.b, "color", "red", false, &change), GTEXT_CSS_E_LIMIT_DECLARATIONS);
  EXPECT_FALSE(change.changed);
  EXPECT_EQ(css_of(b.b), before);
  EXPECT_EQ(set(b.b, "padding", "1px", false, &change), GTEXT_CSS_E_LIMIT_DECLARATIONS) << "a shorthand that needs four places";
  EXPECT_EQ(css_of(b.b), before);
  ASSERT_EQ(remove(b.b, "margin-top", &change), GTEXT_CSS_OK);
  EXPECT_EQ(set(b.b, "color", "red", false, &change), GTEXT_CSS_OK) << "a removed declaration gives its place back";
  gtext_css_block_change_free(&change);
}

/* ---- clone -------------------------------------------------------------------------------------------------------------------------- */

TEST(CssBlockClone, ACopyIsIndependentAndEqual) {
  Block b(R(), "margin: 1px 2px; color: red !important; --a: 1");
  GTEXT_CSS_Block_Change change{};
  ASSERT_EQ(remove(b.b, "margin-left", &change), GTEXT_CSS_OK);
  GTEXT_CSS_Declaration_Block * c = nullptr;
  ASSERT_EQ(gtext_css_block_clone(b.b, &c), GTEXT_CSS_OK);
  EXPECT_EQ(css_of(c), css_of(b.b));
  EXPECT_EQ(gtext_css_block_count(c), gtext_css_block_count(b.b));
  ASSERT_EQ(set(c, "color", "blue", false, &change), GTEXT_CSS_OK);
  EXPECT_EQ(css_of(b.b), "margin-top: 1px; margin-right: 2px; margin-bottom: 1px; color: red !important; --a: 1;");
  EXPECT_EQ(css_of(c), "margin-top: 1px; margin-right: 2px; margin-bottom: 1px; color: blue; --a: 1;");
  gtext_css_block_free(c);
  gtext_css_block_change_free(&change);
  EXPECT_EQ(gtext_css_block_clone(nullptr, &c), GTEXT_CSS_E_INVALID);
}

/* ---- allocation failure ------------------------------------------------------------------------------------------------------------- */

struct EditCase {
  const char * initial;
  const char * op;      // "set", "remove"
  const char * name;
  const char * value;
  bool important;
};

// Every call is swept over its allocations. A failed call is GTEXT_CSS_E_OOM and the block is exactly as it was (its serialization is the same, its count is the same);
// the run that does not fail gives the answer of the unswept call; the allocator holds nothing afterwards.
TEST(CssBlockEditOom, EverySetAndRemoveIsAtomicUnderAllocationFailure) {
  const EditCase cases[] = {
      {"color: red", "set", "color", "blue", false},
      {"color: red", "set", "color", "red", false},
      {"margin-top: 5px; color: red", "set", "margin", "1px 2px 3px 4px", false},
      {"margin: 1px 2px", "set", "margin", "1px 2px", false},
      {"", "set", "border", "1px solid red", false},
      {"border-top-color: blue", "set", "border", "1px solid red", true},
      {"font: 12px serif", "set", "font", "italic bold 14px/1.5 monospace", false},
      {"--a: 1", "set", "--a", "2", false},
      {"--a: 1", "set", "--b", "2", false},
      {"background-color: red", "set", "background", "url(a) 1px 2px / 3px 4px no-repeat, red", false},
      {"margin: var(--x)", "set", "margin", "var(--y)", false},
      {"margin: 1px; color: red; width: 2px", "remove", "margin", "", false},
      {"font: 12px serif; color: red", "remove", "font", "", false},
      {"--a: 1; --b: 2", "remove", "--a", "", false},
      {"color: red", "remove", "width", "", false},
  };
  for (const EditCase & c : cases) {
    std::string want_css;
    bool want_changed = false;
    {
      FaultAllocator fa;
      GTEXT_CSS_Declaration_Options o = gtext_css_declaration_options_default();
      o.allocator = &fa.alloc;
      GTEXT_CSS_Declaration_Block * b = nullptr;
      ASSERT_EQ(gtext_css_block_new(R(), &o, &b), GTEXT_CSS_OK);
      GTEXT_CSS_Property_Options po{};
      po.allocator = &fa.alloc;
      std::string init = c.initial;
      size_t pos = 0;
      while (pos < init.size()) {
        size_t semi = init.find(';', pos);
        if (semi == std::string::npos) {
          semi = init.size();
        }
        std::string d = init.substr(pos, semi - pos);
        pos = semi + 1;
        const size_t colon = d.find(':');
        std::string name = d.substr(0, colon), value = d.substr(colon + 1);
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());
        GTEXT_CSS_Property_Outcome oc;
        ASSERT_EQ(gtext_css_block_add_text(b, name.data(), name.size(), value.data(), value.size(), false, &po, &oc, nullptr), GTEXT_CSS_OK);
      }
      GTEXT_CSS_Block_Change change{};
      const std::string name = c.name;
      ASSERT_EQ(std::string(c.op) == "set" ? set(b, name, c.value, c.important, &change, nullptr, &fa.alloc) : remove(b, name, &change), GTEXT_CSS_OK) << c.name;
      want_css = css_of(b);
      want_changed = change.changed;
      gtext_css_block_change_free(&change);
      gtext_css_block_free(b);
      EXPECT_TRUE(fa.live.empty());
    }
    size_t failures = 0;
    for (size_t n = 1; n < 100000; n++) {
      FaultAllocator fa;
      GTEXT_CSS_Declaration_Options o = gtext_css_declaration_options_default();
      o.allocator = &fa.alloc;
      GTEXT_CSS_Declaration_Block * b = nullptr;
      ASSERT_EQ(gtext_css_block_new(R(), &o, &b), GTEXT_CSS_OK);
      GTEXT_CSS_Property_Options po{};
      po.allocator = &fa.alloc;
      std::string init = c.initial;
      size_t pos = 0;
      while (pos < init.size()) {
        size_t semi = init.find(';', pos);
        if (semi == std::string::npos) {
          semi = init.size();
        }
        std::string d = init.substr(pos, semi - pos);
        pos = semi + 1;
        const size_t colon = d.find(':');
        std::string name = d.substr(0, colon), value = d.substr(colon + 1);
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());
        GTEXT_CSS_Property_Outcome oc;
        ASSERT_EQ(gtext_css_block_add_text(b, name.data(), name.size(), value.data(), value.size(), false, &po, &oc, nullptr), GTEXT_CSS_OK);
      }
      const std::string before = css_of(b);
      const size_t count_before = gtext_css_block_count(b);
      GTEXT_CSS_Block_Change change{};
      const std::string name = c.name;
      fa.arm(n);
      const GTEXT_CSS_Status st = std::string(c.op) == "set" ? set(b, name, c.value, c.important, &change, nullptr, &fa.alloc) : remove(b, name, &change);
      fa.disarm();
      if (!fa.failed) {
        EXPECT_EQ(st, GTEXT_CSS_OK) << c.name << " " << n;
        EXPECT_EQ(css_of(b), want_css) << c.name << " " << n;
        EXPECT_EQ(change.changed, want_changed);
        gtext_css_block_change_free(&change);
        gtext_css_block_free(b);
        EXPECT_TRUE(fa.live.empty()) << c.name << " " << n;
        break;
      }
      failures++;
      EXPECT_EQ(st, GTEXT_CSS_E_OOM) << c.name << ": allocation " << n << " " << gtext_css_status_string(st);
      EXPECT_EQ(css_of(b), before) << c.name << ": allocation " << n << " changed the block";
      EXPECT_EQ(gtext_css_block_count(b), count_before);
      EXPECT_FALSE(change.changed) << c.name << " " << n;
      gtext_css_block_change_free(&change);
      gtext_css_block_free(b);
      EXPECT_TRUE(fa.live.empty()) << c.name << " " << n << ": a block is left";
      EXPECT_EQ(fa.bad_frees, 0u);
    }
    EXPECT_GT(failures, 0u) << c.name;
  }
}

/* ---- declarations the library cannot judge: kept as they were written ---------------------------------------------------------------- */

/// Set a declaration that is kept as written.
GTEXT_CSS_Status set_opaque(GTEXT_CSS_Declaration_Block * b, const std::string & name, const std::string & raw, bool important = false, GTEXT_CSS_Block_Change * change = nullptr) {
  return gtext_css_block_set_opaque(b, find(R(), name), raw.data(), raw.size(), important, change);
}

// CSSOM "serialize a CSS declaration block" writes what the declarations hold; a declaration whose value the library cannot judge holds its value as it was written, in its place.
TEST(CssBlockOpaque, IsKeptInOrderReportedAndWrittenBack) {
  GTEXT_CSS_Declaration_Block * b = nullptr;
  ASSERT_EQ(gtext_css_block_new(R(), nullptr, &b), GTEXT_CSS_OK);
  EXPECT_EQ(set(b, "color", "red"), GTEXT_CSS_OK);
  GTEXT_CSS_Block_Change change{};
  EXPECT_EQ(set_opaque(b, "voice-family", "preserve male", true, &change), GTEXT_CSS_OK);
  EXPECT_TRUE(change.changed);
  EXPECT_EQ(changed_names(change), "voice-family");
  gtext_css_block_change_free(&change);
  EXPECT_EQ(set(b, "width", "1px"), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_block_count(b), 3u);
  EXPECT_EQ(order_of(b), "color voice-family width");
  EXPECT_EQ(css_of(b), "color: red; voice-family: preserve male !important; width: 1px;");
  GTEXT_CSS_Declaration_Item it;
  ASSERT_TRUE(gtext_css_block_item(b, 1, &it));
  EXPECT_TRUE(it.opaque);
  EXPECT_EQ(std::string(it.raw.data, it.raw.length), "preserve male");
  EXPECT_TRUE(it.important);
  ASSERT_TRUE(gtext_css_block_item(b, 0, &it));
  EXPECT_FALSE(it.opaque);
  ASSERT_TRUE(gtext_css_block_find(b, find(R(), "voice-family"), nullptr, 0, &it));
  EXPECT_TRUE(it.opaque);
  // a clone keeps it
  GTEXT_CSS_Declaration_Block * c = nullptr;
  ASSERT_EQ(gtext_css_block_clone(b, &c), GTEXT_CSS_OK);
  EXPECT_EQ(css_of(c), css_of(b));
  gtext_css_block_free(c);
  gtext_css_block_free(b);
}

// "set a CSS declaration": the same property is replaced in its place; one that says the same is not a change; removing it removes it.
TEST(CssBlockOpaque, ReplacedInPlaceUnchangedWhenSameAndRemovable) {
  GTEXT_CSS_Declaration_Block * b = nullptr;
  ASSERT_EQ(gtext_css_block_new(R(), nullptr, &b), GTEXT_CSS_OK);
  ASSERT_EQ(set(b, "color", "red"), GTEXT_CSS_OK);
  ASSERT_EQ(set_opaque(b, "voice-family", "male"), GTEXT_CSS_OK);
  ASSERT_EQ(set(b, "width", "1px"), GTEXT_CSS_OK);
  GTEXT_CSS_Block_Change change{};
  EXPECT_EQ(set_opaque(b, "voice-family", "male", false, &change), GTEXT_CSS_OK);
  EXPECT_FALSE(change.changed) << "the same text, the same importance";
  EXPECT_EQ(set_opaque(b, "voice-family", "male", true, &change), GTEXT_CSS_OK);
  EXPECT_TRUE(change.changed) << "the importance is part of the declaration";
  gtext_css_block_change_free(&change);
  EXPECT_EQ(set_opaque(b, "voice-family", "female"), GTEXT_CSS_OK);
  EXPECT_EQ(css_of(b), "color: red; voice-family: female; width: 1px;") << "replaced in its place";
  EXPECT_EQ(gtext_css_block_remove(b, find(R(), "voice-family"), nullptr, 0, &change), GTEXT_CSS_OK);
  EXPECT_TRUE(change.changed);
  gtext_css_block_change_free(&change);
  EXPECT_EQ(css_of(b), "color: red; width: 1px;");
  gtext_css_block_free(b);
}

// A shorthand the library cannot split (a system font needs the environment's table) is kept as one declaration; a longhand declared before it is overridden by it, so a
// later set of that longhand goes after it, where it takes effect.
TEST(CssBlockOpaque, ASetLonghandOfAnOpaqueShorthandGoesAfterIt) {
  GTEXT_CSS_Declaration_Block * b = nullptr;
  ASSERT_EQ(gtext_css_block_new(R(), nullptr, &b), GTEXT_CSS_OK);
  ASSERT_EQ(set(b, "font-size", "10px"), GTEXT_CSS_OK);
  ASSERT_EQ(set(b, "color", "red"), GTEXT_CSS_OK);
  ASSERT_EQ(set_opaque(b, "font", "caption"), GTEXT_CSS_OK);
  ASSERT_EQ(set(b, "width", "1px"), GTEXT_CSS_OK);
  EXPECT_EQ(css_of(b), "font-size: 10px; color: red; font: caption; width: 1px;");
  EXPECT_EQ(set(b, "font-size", "12px"), GTEXT_CSS_OK);
  EXPECT_EQ(css_of(b), "color: red; font: caption; width: 1px; font-size: 12px;") << "an in-place replacement before the shorthand would have been overridden by it";
  EXPECT_EQ(set(b, "font-size", "13px"), GTEXT_CSS_OK);
  EXPECT_EQ(css_of(b), "color: red; font: caption; width: 1px; font-size: 13px;") << "now after it: replaced in place";
  // a later declaration of the whole shorthand supersedes the opaque one
  ASSERT_EQ(set(b, "font", "12px serif"), GTEXT_CSS_OK);
  EXPECT_EQ(css_of(b).find("caption"), std::string::npos);
  gtext_css_block_free(b);
}

TEST(CssBlockOpaque, OnlyAPropertyOfTheRegistryAndTextThatIsNotEmptyIsTaken) {
  GTEXT_CSS_Declaration_Block * b = nullptr;
  ASSERT_EQ(gtext_css_block_new(R(), nullptr, &b), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_block_set_opaque(b, 0, "x", 1, false, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_block_set_opaque(nullptr, find(R(), "color"), "x", 1, false, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_block_set_opaque(b, find(R(), "voice-family"), nullptr, 3, false, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_block_set_opaque(b, find(R(), "voice-family"), "", 0, false, nullptr), GTEXT_CSS_E_INVALID) << "an empty value is no declaration";
  EXPECT_EQ(gtext_css_block_count(b), 0u);
  gtext_css_block_free(b);
}

// The strong guarantee: an allocation failure at any point of the set leaves the block as it was.
TEST(CssBlockOpaque, AnAllocationFailureLeavesTheBlockAsItWas) {
  size_t failures = 0;
  for (size_t n = 1; n < 1000; n++) {
    FaultAllocator fa;
    GTEXT_CSS_Declaration_Options o = gtext_css_declaration_options_default();
    o.allocator = &fa.alloc;
    GTEXT_CSS_Declaration_Block * b = nullptr;
    ASSERT_EQ(gtext_css_block_new(R(), &o, &b), GTEXT_CSS_OK);
    ASSERT_EQ(set_opaque(b, "voice-family", "male"), GTEXT_CSS_OK);
    ASSERT_EQ(set_opaque(b, "font", "caption"), GTEXT_CSS_OK);
    const std::string before = css_of(b);
    fa.arm(n);
    GTEXT_CSS_Block_Change change{};
    const GTEXT_CSS_Status st = set_opaque(b, n % 2 ? "voice-family" : "voice-pitch", "female", false, &change);
    fa.disarm();
    if (!fa.failed) {
      EXPECT_EQ(st, GTEXT_CSS_OK);
      gtext_css_block_change_free(&change);
      gtext_css_block_free(b);
      EXPECT_TRUE(fa.live.empty());
      break;
    }
    failures++;
    EXPECT_EQ(st, GTEXT_CSS_E_OOM);
    EXPECT_EQ(css_of(b), before);
    gtext_css_block_change_free(&change);
    gtext_css_block_free(b);
    EXPECT_TRUE(fa.live.empty());
  }
  EXPECT_GT(failures, 0u);
}

} // namespace
