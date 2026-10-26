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
 * The machinery of computed values (css_computed.h): defaulting (`initial`, `inherit`, `unset`, `all`), the refusal of what the cascade must resolve,
 * the order of computation, the cascade of the members of a logical group as one property, custom properties, sharing and immutability of a
 * computed value, the limits, the dump and the hash, and the classification that every property of the pin is held to.
 */

#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_compute_test_support.h"

using namespace cmp;

namespace {

/* ---- defaulting -------------------------------------------------------------------------------------------- */

TEST(CssComputeDefaulting, InitialInheritAndUnsetAreAppliedAtComputation) {
  EXPECT_EQ(computed("color: red; width: 7px", "color: initial; width: initial", "color"), "canvastext");
  EXPECT_EQ(computed("color: red; width: 7px", "color: initial; width: initial", "width"), "auto");
  EXPECT_EQ(computed("color: red; width: 7px", "color: inherit; width: inherit", "color"), "red");
  EXPECT_EQ(computed("color: red; width: 7px", "color: inherit; width: inherit", "width"), "7px") << "inherit works for a property that is not inherited";
  EXPECT_EQ(computed("color: red; width: 7px", "color: unset; width: unset", "color"), "red");
  EXPECT_EQ(computed("color: red; width: 7px", "color: unset; width: unset", "width"), "auto");
  // the root has no parent: inherit is initial
  EXPECT_EQ(root_computed("color: inherit", "color"), "canvastext");
  EXPECT_EQ(root_computed("width: inherit", "width"), "auto");
  // a property nobody declares: inherited or initial
  EXPECT_EQ(computed("color: red; margin-left: 9px", "", "color"), "red");
  EXPECT_EQ(computed("color: red; margin-left: 9px", "", "margin-left"), "0px");
}

TEST(CssComputeDefaulting, AllIsADeclarationAtItsPlaceInTheOrderAndSparesDirection) {
  // all comes first, then a declaration beats it
  EXPECT_EQ(computed("margin-left: 9px; color: red", "all: initial; color: blue", "color"), "blue");
  EXPECT_EQ(computed("margin-left: 9px; color: red", "all: initial; color: blue", "margin-left"), "0px");
  // all comes last: it beats the declaration before it
  EXPECT_EQ(computed("margin-left: 9px; color: red", "color: blue; all: inherit", "color"), "red");
  EXPECT_EQ(computed("margin-left: 9px; color: red", "color: blue; all: inherit", "margin-left"), "9px");
  EXPECT_EQ(computed("margin-left: 9px; color: red", "all: unset", "margin-left"), "0px");
  EXPECT_EQ(computed("margin-left: 9px; color: red", "all: unset", "color"), "red");
  // direction is not reset by all
  EXPECT_EQ(computed("direction: rtl", "all: initial", "direction"), "rtl") << "`all` does not set direction: it is inherited";
  EXPECT_EQ(computed("direction: rtl", "direction: ltr; all: initial", "direction"), "ltr");
  EXPECT_EQ(computed("direction: rtl", "all: initial", "unicode-bidi"), "normal");
  // custom properties are not reset by all
  Context ctx(R());
  Block pb(R(), "--a: 1"), cb(R(), "all: initial");
  Style p, c;
  p.compute(ctx, &pb);
  c.compute(ctx, &cb, &p);
  EXPECT_EQ(gtext_css_computed_custom_count(c.s), 1u);
}

TEST(CssComputeDefaulting, ARevertAndAPendingValueAreCascadeResultsAndRefusedAsInputs) {
  Context ctx(R());
  {
    Block b(R(), "color: revert");
    Style s;
    s.compute(ctx, &b);
    EXPECT_EQ(s.status, GTEXT_CSS_E_INVALID);
    EXPECT_STREQ(s.result.name, "color");
    ASSERT_NE(s.result.detail, nullptr);
    EXPECT_NE(std::string(s.result.detail).find("cascade"), std::string::npos);
    EXPECT_EQ(s.s, nullptr);
  }
  {
    Block b(R(), "color: revert-layer");
    Style s;
    s.compute(ctx, &b);
    EXPECT_EQ(s.status, GTEXT_CSS_E_INVALID);
  }
  {
    Block b(R(), "all: revert");
    Style s;
    s.compute(ctx, &b);
    EXPECT_EQ(s.status, GTEXT_CSS_E_INVALID);
  }
  {
    Block b(R(), "width: var(--w)");
    Style s;
    s.compute(ctx, &b);
    EXPECT_EQ(s.status, GTEXT_CSS_E_INVALID);
    EXPECT_STREQ(s.result.name, "width");
  }
  {
    // a revert that a later declaration overrode never wins, so it is not an input
    Block b(R(), "color: revert; color: red");
    Style s;
    s.compute(ctx, &b);
    EXPECT_EQ(s.status, GTEXT_CSS_OK);
    EXPECT_EQ(s.value("color"), "red");
  }
}

TEST(CssComputeDefaulting, ADeclaredPropertyWhoseComputedValueIsNotImplementedIsUnsupportedAndNamed) {
  Context ctx(R());
  Block b(R(), "position-area: top left");
  EXPECT_TRUE(b.rejected.empty());
  Style s;
  s.compute(ctx, &b);
  EXPECT_EQ(s.status, GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_STREQ(s.result.name, "position-area");
  EXPECT_EQ(s.s, nullptr);
  // but a style that does not declare it has its inherited or initial value, or none
  Block empty(R(), "");
  Style t;
  t.compute(ctx, &empty);
  ASSERT_EQ(t.status, GTEXT_CSS_OK);
  EXPECT_EQ(t.value("position-area"), "none");
}

/* ---- logical properties cascaded as one ---------------------------------------------------------------------------- */

TEST(CssComputeLogical, TheLastDeclarationOfAPairWinsWhicheverNameItUsedAndBothMembersHaveItsValue) {
  // the example of Logical 1: horizontal-tb ltr, margin-inline-start then margin-left then margin-inline-end
  const std::string decls = "margin-inline-start: 1px; margin-left: 2px; margin-inline-end: 3px";
  EXPECT_EQ(computed("", decls, "margin-left"), "2px");
  EXPECT_EQ(computed("", decls, "margin-inline-start"), "2px") << "paired properties share a computed value";
  EXPECT_EQ(computed("", decls, "margin-right"), "3px");
  EXPECT_EQ(computed("", decls, "margin-inline-end"), "3px");
  // in rtl the same declarations: inline-start is the right
  EXPECT_EQ(computed("", "direction: rtl; " + decls, "margin-left"), "3px");
  EXPECT_EQ(computed("", "direction: rtl; " + decls, "margin-right"), "1px") << "margin-inline-start (1px) is the right margin, and margin-left (2px) is the end";
  // the physical declaration after the logical one
  EXPECT_EQ(computed("", "margin-left: 5px; margin-inline-start: 6px", "margin-left"), "6px");
  // the mapping is by the element's own computed writing mode, even when it is declared later in the block
  EXPECT_EQ(computed("", "margin-inline-start: 4px; writing-mode: vertical-lr", "margin-top"), "4px");
  EXPECT_EQ(computed("", "margin-block-start: 4px; writing-mode: vertical-lr", "margin-left"), "4px");
  EXPECT_EQ(computed("", "margin-block-start: 4px; writing-mode: vertical-rl", "margin-right"), "4px");
  EXPECT_EQ(computed("", "margin-block-end: 4px; writing-mode: sideways-lr", "margin-right"), "4px");
}

TEST(CssComputeLogical, SizesBordersAndRadiiMapAndInheritComesFromTheSameNamedPropertyOfTheParent) {
  EXPECT_EQ(computed("", "writing-mode: vertical-rl; inline-size: 30px; block-size: 40px", "height"), "30px");
  EXPECT_EQ(computed("", "writing-mode: vertical-rl; inline-size: 30px; block-size: 40px", "width"), "40px");
  EXPECT_EQ(computed("", "border-start-start-radius: 7px", "border-top-left-radius"), "7px");
  EXPECT_EQ(computed("", "direction: rtl; border-start-start-radius: 7px", "border-top-right-radius"), "7px");
  EXPECT_EQ(computed("", "border-block-start-style: solid; border-block-start-width: 4px", "border-top-width"), "4px");
  // inherit on a flow-relative property takes the same-named property of the parent
  EXPECT_EQ(computed("margin-inline-start: 5px", "writing-mode: vertical-rl; margin-inline-start: inherit", "margin-top"), "5px")
      << "the parent's margin-inline-start (its left margin) is the child's margin-inline-start (its top)";
  EXPECT_EQ(computed("margin-inline-start: 5px; margin-left: 9px", "margin-left: inherit", "margin-left"), "9px");
  // all: inherit sets the physical members last
  EXPECT_EQ(computed("margin-left: 9px", "all: inherit", "margin-left"), "9px");
}

/* ---- the order, and reading the earlier ------------------------------------------------------------------------------ */

TEST(CssComputeOrder, TheTableIsOrderedWritingModeFirstAndEveryNameIsAPropertyThatIsNotAShorthand) {
  ASSERT_GE(gtext_css_compute_order_count(), 20u);
  EXPECT_STREQ(gtext_css_compute_order_at(0), "writing-mode");
  EXPECT_STREQ(gtext_css_compute_order_at(1), "direction");
  EXPECT_STREQ(gtext_css_compute_order_at(2), "text-orientation");
  EXPECT_EQ(gtext_css_compute_order_at(gtext_css_compute_order_count()), nullptr);
  std::set<std::string> seen;
  for (size_t i = 0; i < gtext_css_compute_order_count(); i++) {
    const std::string n = gtext_css_compute_order_at(i);
    EXPECT_TRUE(seen.insert(n).second) << n;
    const GTEXT_CSS_Property_Id id = find(R(), n);
    ASSERT_NE(id, 0u) << n;
    EXPECT_NE(gtext_css_compute_class(R(), id, nullptr, nullptr), GTEXT_CSS_COMPUTE_SHORTHAND) << n;
  }
  // font-size is before everything that reads it
  size_t font_size = 99999, border_width = 99999, line_height = 99999, display = 99999, float_at = 99999;
  for (size_t i = 0; i < gtext_css_compute_order_count(); i++) {
    const std::string n = gtext_css_compute_order_at(i);
    if (n == "font-size") font_size = i;
    if (n == "line-height") line_height = i;
    if (n == "display") display = i;
    if (n == "float") float_at = i;
    if (n == "border-top-style") border_width = i;
  }
  EXPECT_LT(font_size, line_height);
  EXPECT_LT(float_at, display);
  EXPECT_LT(border_width, 99999u);
}

TEST(CssComputeOrder, EveryPropertyIsComputedWithATrapOnReadingOneThatIsNotYetComputed) {
  // computing with no declaration and with every wide keyword in turn: no property reads one of the same element that is below it
  Context ctx(R());
  ASSERT_EQ(ctx.status, GTEXT_CSS_OK);
  Block empty(R(), "");
  Style root;
  root.compute(ctx, &empty);
  ASSERT_EQ(root.status, GTEXT_CSS_OK) << root.result.name << ": " << (root.result.detail ? root.result.detail : "");
  for (const char * wide : {"all: initial", "all: inherit", "all: unset"}) {
    Block b(R(), wide);
    Style child;
    child.compute(ctx, &b, &root);
    EXPECT_EQ(child.status, GTEXT_CSS_OK) << wide << ": " << child.result.name << " " << (child.result.detail ? child.result.detail : "");
  }
  // and the algorithms, each with a value that makes it read what it reads
  Block b(R(), "font-size: 2em; line-height: 150%; border-top-width: 3px; border-top-style: dashed; display: inline; float: left; position: absolute; "
      "width: 1lh; wrap-flow: both; text-align-all: match-parent; math-depth: auto-add; font-weight: bolder; margin-block-start: 1px");
  Style env;
  GTEXT_CSS_Environment e = gtext_css_environment_default();
  e.normal_line_height = [](void *, const GTEXT_CSS_Font_Query * q, double * px) { *px = q->size_px; return true; };
  Context ctx2(R(), &e);
  Style r2, c2;
  r2.compute(ctx2, &empty);
  c2.compute(ctx2, &b, &r2);
  EXPECT_EQ(c2.status, GTEXT_CSS_OK) << c2.result.name << " " << (c2.result.detail ? c2.result.detail : "");
}

/* ---- every property ------------------------------------------------------------------------------------------------------ */

TEST(CssComputeClasses, EveryPropertyOfThePinIsClassifiedAndTheRequiredOnesAreNeverUnimplemented) {
  const size_t n = gtext_css_registry_property_count(R());
  std::map<GTEXT_CSS_Compute_Class, size_t> counts;
  size_t required = 0;
  std::vector<std::string> unimplemented;
  for (size_t id = 1; id <= n; id++) {
    GTEXT_CSS_Property_Info info;
    ASSERT_TRUE(gtext_css_registry_property_info(R(), static_cast<GTEXT_CSS_Property_Id>(id), &info));
    GTEXT_CSS_Compute_Algorithm algo = GTEXT_CSS_ALGO_NONE;
    bool partial = false;
    const GTEXT_CSS_Compute_Class k = gtext_css_compute_class(R(), static_cast<GTEXT_CSS_Property_Id>(id), &algo, &partial);
    counts[k]++;
    if (info.flags & GTEXT_CSS_PROP_ALIAS) {
      EXPECT_EQ(k, GTEXT_CSS_COMPUTE_ALIAS) << info.name;
    }
    else if ((info.flags & GTEXT_CSS_PROP_SHORTHAND) && info.name != std::string("--*")) {
      EXPECT_EQ(k, GTEXT_CSS_COMPUTE_SHORTHAND) << info.name;
    }
    if ((k == GTEXT_CSS_COMPUTE_ALGORITHM) != (algo != GTEXT_CSS_ALGO_NONE && algo != GTEXT_CSS_ALGO_CUSTOM)) {
      if (k != GTEXT_CSS_COMPUTE_CUSTOM) {
        ADD_FAILURE() << info.name << ": the class is " << gtext_css_compute_class_name(k) << " and the algorithm " << gtext_css_compute_algorithm_name(algo);
      }
    }
    if (gtext_css_compute_required(R(), static_cast<GTEXT_CSS_Property_Id>(id))) {
      required++;
      EXPECT_NE(k, GTEXT_CSS_COMPUTE_UNIMPLEMENTED) << info.name << " is inherited or an SVG 2 presentation attribute";
    }
    if (k == GTEXT_CSS_COMPUTE_UNIMPLEMENTED) {
      unimplemented.push_back(info.name);
    }
    if (info.inherited == GTEXT_CSS_INHERIT_YES && !(info.flags & (GTEXT_CSS_PROP_ALIAS | GTEXT_CSS_PROP_SHORTHAND))) {
      EXPECT_TRUE(gtext_css_compute_required(R(), static_cast<GTEXT_CSS_Property_Id>(id))) << info.name << " is inherited and not required";
    }
  }
  EXPECT_EQ(n, 823u);
  EXPECT_EQ(counts[GTEXT_CSS_COMPUTE_CUSTOM], 1u);
  EXPECT_EQ(counts[GTEXT_CSS_COMPUTE_ALIAS], 64u);
  EXPECT_GT(required, 200u);
  EXPECT_LE(unimplemented.size(), 15u) << "the list of properties whose computed value is not implemented only gets shorter (tools/css/computed_rules.tsv)";
  EXPECT_EQ(gtext_css_compute_class(R(), 0, nullptr, nullptr), GTEXT_CSS_COMPUTE_UNIMPLEMENTED);
  EXPECT_EQ(gtext_css_compute_class(R(), find(R(), "font-size"), nullptr, nullptr), GTEXT_CSS_COMPUTE_ALGORITHM);
  EXPECT_EQ(gtext_css_compute_class(R(), find(R(), "margin"), nullptr, nullptr), GTEXT_CSS_COMPUTE_SHORTHAND);
  EXPECT_EQ(gtext_css_compute_class(R(), find(R(), "-webkit-transform"), nullptr, nullptr), GTEXT_CSS_COMPUTE_ALIAS);
  EXPECT_EQ(gtext_css_compute_class(R(), find(R(), "cursor"), nullptr, nullptr), GTEXT_CSS_COMPUTE_BY_TYPE);
  EXPECT_EQ(gtext_css_compute_class(R(), find(R(), "clip-rule"), nullptr, nullptr), GTEXT_CSS_COMPUTE_IDENTITY);
  EXPECT_STREQ(gtext_css_compute_class_name(GTEXT_CSS_COMPUTE_BY_TYPE), "by-type");
  EXPECT_STREQ(gtext_css_compute_algorithm_name(GTEXT_CSS_ALGO_FONT_SIZE), "FONT_SIZE");
  EXPECT_STREQ(gtext_css_compute_class_name(GTEXT_CSS_COMPUTE_CLASS_COUNT), "unknown");
}

TEST(CssComputeClasses, ARegistrationOfTheCallerIsComputedByTypeOrAsSpecifiedAndAnOverrideHasTheClassOfTheOneItReplaces) {
  GTEXT_CSS_Registry_Builder * b = nullptr;
  ASSERT_EQ(gtext_css_registry_builder_new(nullptr, &b), GTEXT_CSS_OK);
  GTEXT_CSS_Property_Def d;
  std::memset(&d, 0, sizeof d);
  d.name = "vector-width";
  d.grammar = "<length>";
  d.initial = "2px";
  d.computed = GTEXT_CSS_COMPUTED_ABSOLUTE_LENGTHS;
  ASSERT_EQ(gtext_css_registry_register_property(b, &d, nullptr), GTEXT_CSS_OK);
  d.name = "vector-keep";
  d.grammar = "<length> | auto";
  d.initial = "auto";
  d.computed = GTEXT_CSS_COMPUTED_AS_SPECIFIED;
  d.inherited = true;
  ASSERT_EQ(gtext_css_registry_register_property(b, &d, nullptr), GTEXT_CSS_OK);
  d.name = "fill";
  d.grammar = "<color> | none";
  d.initial = "black";
  d.override = true;
  d.computed = GTEXT_CSS_COMPUTED_COLOR;
  ASSERT_EQ(gtext_css_registry_register_property(b, &d, nullptr), GTEXT_CSS_OK);
  reg::Owned o;
  ASSERT_EQ(gtext_css_registry_freeze(b, &o.r, nullptr), GTEXT_CSS_OK);
  Context ctx(o.r);
  ASSERT_EQ(ctx.status, GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_compute_class(o.r, find(o.r, "vector-width"), nullptr, nullptr), GTEXT_CSS_COMPUTE_BY_TYPE);
  EXPECT_EQ(gtext_css_compute_class(o.r, find(o.r, "vector-keep"), nullptr, nullptr), GTEXT_CSS_COMPUTE_IDENTITY);
  EXPECT_EQ(gtext_css_compute_class(o.r, find(o.r, "fill"), nullptr, nullptr), GTEXT_CSS_COMPUTE_BY_TYPE) << "the pin's fill is by-type";
  Block pb(o.r, "font-size: 10px");
  Block cb(o.r, "vector-width: 2em; vector-keep: 3em; fill: red");
  EXPECT_TRUE(cb.rejected.empty());
  Style parent, child;
  parent.compute(ctx, &pb);
  child.compute(ctx, &cb, &parent);
  ASSERT_EQ(child.status, GTEXT_CSS_OK);
  GTEXT_CSS_Property_Info i;
  gtext_css_registry_property_info(o.r, find(o.r, "vector-width"), &i);
  const GTEXT_CSS_Value_Set * set = nullptr;
  ASSERT_EQ(gtext_css_computed_value(child.s, find(o.r, "vector-width"), &set), GTEXT_CSS_OK);
  const GTEXT_CSS_Value * v = gtext_css_value_set_root(set);
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->number, 20.0);
  ASSERT_EQ(gtext_css_computed_value(child.s, find(o.r, "vector-keep"), &set), GTEXT_CSS_OK);
  v = gtext_css_value_set_root(set);
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->unit, GTEXT_CSS_UNIT_EM) << "as specified";
  ASSERT_EQ(gtext_css_computed_value(child.s, find(o.r, "fill"), &set), GTEXT_CSS_OK);
}

/* ---- sharing and immutability -------------------------------------------------------------------------------------------------- */

TEST(CssComputeSharing, AnInheritedValueIsTheParentsSetAndAnUnchangedOneIsTheDeclaredSet) {
  Context ctx(R());
  Block pb(R(), "color: red; width: 10px"), cb(R(), "width: 20px; margin-left: 5px; border-top-color: blue");
  Style parent, child;
  parent.compute(ctx, &pb);
  child.compute(ctx, &cb, &parent);
  ASSERT_EQ(child.status, GTEXT_CSS_OK);
  const GTEXT_CSS_Value_Set *pcolor = nullptr, *ccolor = nullptr;
  ASSERT_EQ(gtext_css_computed_value(parent.s, find(R(), "color"), &pcolor), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_computed_value(child.s, find(R(), "color"), &ccolor), GTEXT_CSS_OK);
  EXPECT_EQ(pcolor, ccolor) << "inherited: the parent's set, retained, not copied";
  // a declared value that no rule changes is the declared set itself
  GTEXT_CSS_Declaration_Item it;
  const GTEXT_CSS_Value_Set *margin = nullptr, *declared = nullptr;
  for (size_t i = 0; i < gtext_css_block_count(cb.b); i++) {
    gtext_css_block_item(cb.b, i, &it);
    if (it.property == find(R(), "margin-left")) {
      declared = it.set;
    }
  }
  ASSERT_EQ(gtext_css_computed_value(child.s, find(R(), "margin-left"), &margin), GTEXT_CSS_OK);
  EXPECT_EQ(margin, declared) << "margin-left: 5px is already a px length: the computed value is the declared set";
  // the initial value is the context's: two styles share it
  Style other;
  other.compute(ctx, nullptr);
  const GTEXT_CSS_Value_Set *w1 = nullptr, *w2 = nullptr;
  ASSERT_EQ(gtext_css_computed_value(other.s, find(R(), "margin-top"), &w1), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_computed_value(child.s, find(R(), "margin-top"), &w2), GTEXT_CSS_OK);
  EXPECT_EQ(w1, w2);
}

TEST(CssComputeSharing, AComputedValueOutlivesTheStyleAndTheBlockAndTheParentThatMadeIt) {
  Context ctx(R());
  GTEXT_CSS_Value_Set * kept_width = nullptr;
  GTEXT_CSS_Value_Set * kept_color = nullptr;
  {
    Block pb(R(), "color: red; font-size: 10px"), cb(R(), "width: 2em; background-position: right 3em top");
    Style parent, child;
    parent.compute(ctx, &pb);
    child.compute(ctx, &cb, &parent);
    ASSERT_EQ(child.status, GTEXT_CSS_OK);
    const GTEXT_CSS_Value_Set * set = nullptr;
    ASSERT_EQ(gtext_css_computed_value(child.s, find(R(), "width"), &set), GTEXT_CSS_OK);
    kept_width = gtext_css_value_set_retain(set);
    ASSERT_EQ(gtext_css_computed_value(child.s, find(R(), "color"), &set), GTEXT_CSS_OK);
    kept_color = gtext_css_value_set_retain(set);
  }
  char * out = nullptr;
  size_t n = 0;
  ASSERT_EQ(gtext_css_serialize_value_buffer(gtext_css_value_set_root(kept_width), nullptr, &out, &n), GTEXT_CSS_OK);
  EXPECT_EQ(std::string(out, n), "20px");
  gtext_css_serialize_buffer_free(nullptr, out);
  ASSERT_EQ(gtext_css_serialize_value_buffer(gtext_css_value_set_root(kept_color), nullptr, &out, &n), GTEXT_CSS_OK);
  EXPECT_EQ(std::string(out, n), "red");
  gtext_css_serialize_buffer_free(nullptr, out);
  gtext_css_value_set_free(kept_width);
  gtext_css_value_set_free(kept_color);
}

TEST(CssComputeSharing, ALongChainOfStylesFreesWithoutRecursing) {
  Context ctx(R());
  Block b(R(), "width: 2em");
  std::vector<GTEXT_CSS_Computed_Style *> chain;
  const GTEXT_CSS_Computed_Style * parent = nullptr;
  for (int i = 0; i < 3000; i++) {
    GTEXT_CSS_Computed_Style * s = nullptr;
    ASSERT_EQ(gtext_css_compute_style(ctx.c, b.b, parent, nullptr, &s, nullptr), GTEXT_CSS_OK);
    chain.push_back(s);
    parent = s;
  }
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    gtext_css_computed_free(*it);
  }
  // and the value sets that point into one another release iteratively
  Style leaf;
  Block big(R(), "font-size: 10px");
  leaf.compute(ctx, &big);
  EXPECT_EQ(leaf.status, GTEXT_CSS_OK);
}

TEST(CssComputeSharing, TheRefcountOfASetCountsOwnersAndAFreeOfTheLastOneFrees) {
  GTEXT_CSS_Property_Value pv;
  ASSERT_EQ(gtext_css_parse_property_text(R(), "width", 5, "10px", 4, nullptr, &pv), GTEXT_CSS_OK);
  GTEXT_CSS_Value_Set * a = gtext_css_value_set_retain(pv.set);
  EXPECT_EQ(a, pv.set);
  gtext_css_property_value_free(&pv);   // one owner gone
  EXPECT_NE(gtext_css_value_set_root(a), nullptr);
  EXPECT_EQ(gtext_css_value_set_retain(nullptr), nullptr);
  gtext_css_value_set_free(a);
  gtext_css_value_set_free(nullptr);
}

/* ---- custom properties ------------------------------------------------------------------------------------------------------- */

TEST(CssComputeCustom, CustomPropertiesAreInheritedOverriddenAndRemovedByInitialInOrderOfTheirNames) {
  Context ctx(R());
  Block pb(R(), "--b: 1; --a: two; --c: 3");
  Block cb(R(), "--b: 20; --d: 4; --c: initial; --a: inherit");
  Style parent, child, grand;
  parent.compute(ctx, &pb);
  ASSERT_EQ(parent.status, GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_computed_custom_count(parent.s), 3u);
  child.compute(ctx, &cb, &parent);
  ASSERT_EQ(child.status, GTEXT_CSS_OK);
  std::vector<std::string> got;
  for (size_t i = 0; i < gtext_css_computed_custom_count(child.s); i++) {
    GTEXT_CSS_String name;
    const GTEXT_CSS_Value_Set * v = nullptr;
    ASSERT_TRUE(gtext_css_computed_custom_at(child.s, i, &name, &v));
    const GTEXT_CSS_Value * root = gtext_css_value_set_root(v);
    got.push_back(std::string(name.data, name.length) + "=" + (root && root->text.data ? std::string(root->text.data, root->text.length) : ""));
  }
  EXPECT_EQ(got, (std::vector<std::string>{"--a=two", "--b=20", "--d=4"}));
  EXPECT_FALSE(gtext_css_computed_custom_at(child.s, 3, nullptr, nullptr));
  // and onward
  grand.compute(ctx, nullptr, &child);
  EXPECT_EQ(gtext_css_computed_custom_count(grand.s), 3u);
}

/* ---- limits ------------------------------------------------------------------------------------------------------------------- */

TEST(CssComputeLimits, TheDepthOfAnInheritanceChainIsBoundedAndTheBoundIsExact) {
  struct Chain {
    int n;
  } chain{20};
  GTEXT_CSS_Element_Interface t;
  std::memset(&t, 0, sizeof t);
  t.size = sizeof t;
  t.user = &chain;
  t.parent = [](void *, GTEXT_CSS_Element_Handle e) { return reinterpret_cast<GTEXT_CSS_Element_Handle>(reinterpret_cast<intptr_t>(e) - 1); };
  GTEXT_CSS_Declaration_Source src;
  src.user = nullptr;
  src.declarations = [](void *, GTEXT_CSS_Element_Handle, const GTEXT_CSS_Declaration_Block ** b) { *b = nullptr; return GTEXT_CSS_OK; };
  GTEXT_CSS_Compute_Options o = gtext_css_compute_options_default();
  o.max_element_depth = 20;
  Context ctx(R(), nullptr, &o);
  GTEXT_CSS_Computed_Style * s = nullptr;
  GTEXT_CSS_Compute_Result r;
  EXPECT_EQ(gtext_css_compute_element(ctx.c, &t, &src, reinterpret_cast<GTEXT_CSS_Element_Handle>(20), &s, &r), GTEXT_CSS_OK) << "20 elements, the bound";
  EXPECT_EQ(r.depth, 20u);
  gtext_css_computed_free(s);
  s = nullptr;
  EXPECT_EQ(gtext_css_compute_element(ctx.c, &t, &src, reinterpret_cast<GTEXT_CSS_Element_Handle>(21), &s, &r), GTEXT_CSS_E_LIMIT_ELEMENTS) << "one more";
  EXPECT_EQ(s, nullptr);
}

TEST(CssComputeLimits, TheLengthOfAListIsBoundedAndTheBoundIsExact) {
  std::string list;
  for (int i = 0; i < 6; i++) {
    list += (i ? ", " : "") + std::to_string(i + 1) + "s";
  }
  GTEXT_CSS_Compute_Options o = gtext_css_compute_options_default();
  o.max_list_items = 6;
  Context ctx(R(), nullptr, &o);
  Block b(R(), "transition-duration: " + list);
  Style s;
  s.compute(ctx, &b);
  EXPECT_EQ(s.status, GTEXT_CSS_OK) << "six items, the bound";
  Block b7(R(), "transition-duration: " + list + ", 7s");
  Style s7;
  s7.compute(ctx, &b7);
  EXPECT_EQ(s7.status, GTEXT_CSS_E_LIMIT_LIST) << "one more";
  EXPECT_EQ(s7.s, nullptr);
}

TEST(CssComputeLimits, ABlockPastItsDeclarationLimitIsRefused) {
  GTEXT_CSS_Declaration_Options o = gtext_css_declaration_options_default();
  o.max_declarations = 3;
  Block b(R(), "color: red; width: 1px; height: 2px", &o);
  EXPECT_TRUE(b.rejected.empty());
  EXPECT_EQ(gtext_css_block_count(b.b), 3u);
  GTEXT_CSS_Property_Value pv;
  ASSERT_EQ(gtext_css_parse_property_text(R(), "margin-top", 10, "1px", 3, nullptr, &pv), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_block_add(b.b, &pv, false, nullptr), GTEXT_CSS_E_LIMIT_DECLARATIONS);
  gtext_css_property_value_free(&pv);
  ASSERT_EQ(gtext_css_parse_property_text(R(), "margin", 6, "1px", 3, nullptr, &pv), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_block_add(b.b, &pv, false, nullptr), GTEXT_CSS_E_LIMIT_DECLARATIONS) << "a shorthand adds nothing when its longhands do not fit";
  EXPECT_EQ(gtext_css_block_count(b.b), 3u);
  gtext_css_property_value_free(&pv);
}

/* ---- the dump and the hash ----------------------------------------------------------------------------------------------------- */

TEST(CssComputeDump, TheDumpIsVersionedCanonicalAndEqualStylesHaveEqualHashes) {
  Context ctx(R());
  Block a(R(), "color: red; width: 2em; --x: y"), b(R(), "width: 2em; --x: y; color: red"), c(R(), "color: blue; width: 2em; --x: y");
  Style sa, sb, sc;
  sa.compute(ctx, &a);
  sb.compute(ctx, &b);
  sc.compute(ctx, &c);
  const std::string da = sa.dump();
  EXPECT_EQ(da.substr(0, 17), "computed-style 1\n");
  EXPECT_EQ(da, sb.dump()) << "the order of the declarations does not change the style";
  EXPECT_NE(da, sc.dump());
  EXPECT_NE(da.find("\ncolor: red\n"), std::string::npos);
  EXPECT_NE(da.find("\nwidth: 32px\n"), std::string::npos);
  EXPECT_NE(da.find("\n--x: y\n"), std::string::npos);
  uint64_t ha = 0, hb = 0, hc = 0;
  ASSERT_EQ(gtext_css_computed_hash(sa.s, &ha), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_computed_hash(sb.s, &hb), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_computed_hash(sc.s, &hc), GTEXT_CSS_OK);
  EXPECT_EQ(ha, hb);
  EXPECT_NE(ha, hc);
  EXPECT_EQ(gtext_css_computed_hash(nullptr, &ha), GTEXT_CSS_E_INVALID);
  // the properties appear in the order of their ids (the sorted names)
  size_t last = 0;
  size_t pos = da.find('\n') + 1;
  std::string prev;
  while (pos < da.size()) {
    const size_t colon = da.find(':', pos);
    const std::string name = da.substr(pos, colon - pos);
    if (name.rfind("--", 0) == 0) {
      break;
    }
    if (!prev.empty()) {
      EXPECT_LT(prev, name);
    }
    prev = name;
    pos = da.find('\n', pos) + 1;
    last = pos;
  }
  EXPECT_GT(last, 0u);
}

TEST(CssComputeAccessors, AnAliasIsItsTargetAShorthandHasNoValueAndTheFlowIsExposed) {
  Context ctx(R());
  Block b(R(), "writing-mode: vertical-lr; direction: rtl; text-orientation: upright; -webkit-transform: none");
  Style s;
  s.compute(ctx, &b);
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  const GTEXT_CSS_Value_Set * set = nullptr;
  EXPECT_EQ(gtext_css_computed_value(s.s, find(R(), "-webkit-transform"), &set), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_computed_value(s.s, find(R(), "margin"), &set), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_computed_value(s.s, 0, &set), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_computed_value(s.s, 99999, &set), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_computed_value(nullptr, find(R(), "width"), &set), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_computed_root(s.s, find(R(), "margin")), nullptr);
  const GTEXT_CSS_Flow f = gtext_css_computed_flow(s.s);
  EXPECT_EQ(f.writing_mode, GTEXT_CSS_WRITING_MODE_VERTICAL_LR);
  EXPECT_EQ(f.direction, GTEXT_CSS_DIRECTION_RTL);
  EXPECT_EQ(f.text_orientation, GTEXT_CSS_TEXT_ORIENTATION_UPRIGHT);
  const GTEXT_CSS_Flow zero = gtext_css_computed_flow(nullptr);
  EXPECT_EQ(zero.writing_mode, GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB);
  // a call with a context of one registry and a block of another is refused
  reg::Owned other;
  ASSERT_EQ(gtext_css_registry_new(nullptr, &other.r), GTEXT_CSS_OK);
  Block foreign(other.r, "width: 1px");
  Style t;
  t.compute(ctx, &foreign);
  EXPECT_EQ(t.status, GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_compute_style(nullptr, nullptr, nullptr, nullptr, &t.s, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_compute_style(ctx.c, nullptr, nullptr, nullptr, nullptr, nullptr), GTEXT_CSS_E_INVALID);
}

TEST(CssComputeContext, AnEnvironmentThatFailsItsCheckIsRefusedAndTheContextIsReusableByAnyNumberOfThreads) {
  GTEXT_CSS_Environment env = gtext_css_environment_default();
  env.default_font_size = 0;
  GTEXT_CSS_Compute_Context * c = nullptr;
  GTEXT_CSS_Compute_Result r;
  EXPECT_EQ(gtext_css_compute_context_new(R(), &env, nullptr, &c, &r), GTEXT_CSS_E_INVALID);
  ASSERT_NE(r.detail, nullptr);
  EXPECT_NE(std::string(r.detail).find("default_font_size"), std::string::npos);
  EXPECT_EQ(c, nullptr);
  EXPECT_EQ(gtext_css_compute_context_new(nullptr, nullptr, nullptr, &c, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_compute_context_new(R(), nullptr, nullptr, nullptr, nullptr), GTEXT_CSS_E_INVALID);
  gtext_css_compute_context_free(nullptr);
  // an environment from an older header (a smaller struct) is read up to its size, the rest defaulting
  GTEXT_CSS_Environment small = gtext_css_environment_default();
  small.size = offsetof(GTEXT_CSS_Environment, default_font_size) + sizeof small.default_font_size;
  small.default_font_size = 20;
  small.device_pixel_ratio = -5;   // beyond the size the caller gave: not read
  Context ctx(R(), &small);
  ASSERT_EQ(ctx.status, GTEXT_CSS_OK);
  Style s;
  s.compute(ctx, nullptr);
  EXPECT_EQ(s.value("font-size"), "20px");
  EXPECT_EQ(gtext_css_compute_context_registry(ctx.c), R());
  EXPECT_EQ(gtext_css_compute_context_registry(nullptr), nullptr);
}

} // namespace
