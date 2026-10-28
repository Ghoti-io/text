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
 * Computed values at their edges: numbers too large for an integer, the color scheme of the element, names chosen against the balance of the custom
 * properties' map, the values a callback of the environment may give, and the properties a root must compute.
 */

#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_compute_test_support.h"

using namespace cmp;

namespace {

TEST(CssComputeEdges, AWidthTooLargeForAnIntegerIsNotNegative) {
  // snapping a border width to a device pixel cast to an integer, which is undefined past 2^63
  EXPECT_EQ(root_computed("border-top-style: solid; border-top-width: 1e300px", "border-top-width").substr(0, 2), "10");
  EXPECT_EQ(root_computed("border-top-style: solid; border-top-width: 1e19px", "border-top-width"), "10000000000000000000px");
  EXPECT_EQ(root_computed("outline-style: solid; outline-width: 9223372036854775808px", "outline-width"), "9223372036854776000px");
  EXPECT_EQ(root_computed("border-top-style: solid; border-top-width: 2.5px", "border-top-width"), "2px") << "whole device pixels below";
  EXPECT_EQ(root_computed("border-top-style: solid; border-top-width: 0.2px", "border-top-width"), "1px");
  // an integer slot rounding a number too large for a long long, and an angle in quarter turns
  EXPECT_EQ(root_computed("z-index: calc(1e30 + 0.5)", "z-index"), "2147483647") << "an integer past what 32 bits hold is clamped, as every browser reads it (no undefined conversion on the way)";
  EXPECT_EQ(root_computed("z-index: calc(-1e30)", "z-index"), "-2147483648");
  EXPECT_EQ(root_computed("image-orientation: 1e30deg", "image-orientation"), "0deg");
  EXPECT_EQ(root_computed("image-orientation: 450deg", "image-orientation"), "90deg");
  EXPECT_EQ(root_computed("image-orientation: -90deg", "image-orientation"), "270deg");
}

TEST(CssComputeEdges, LightDarkTakesTheColorSchemeOfTheElement) {
  // Color Adjust 1, 2.3: the used color scheme is the preference where the element's scheme supports it, else the first scheme the element lists;
  // 2.4: "The element color scheme affects ... the value of the light-dark() function"
  const std::string ld = "; color: light-dark(red, blue)";
  GTEXT_CSS_Environment dark = gtext_css_environment_default();
  dark.color_scheme = GTEXT_CSS_SCHEME_DARK;
  GTEXT_CSS_Environment light = gtext_css_environment_default();
  EXPECT_EQ(root_computed("color-scheme: normal" + ld, "color", &light), "red");
  EXPECT_EQ(root_computed("color-scheme: normal" + ld, "color", &dark), "red") << "Color Adjust 1, 2.3: with no support the user agent default, light, not the preference";
  EXPECT_EQ(root_computed("color-scheme: dark" + ld, "color", &light), "blue") << "the element supports dark only";
  EXPECT_EQ(root_computed("color-scheme: light" + ld, "color", &dark), "red");
  EXPECT_EQ(root_computed("color-scheme: light dark" + ld, "color", &dark), "blue") << "both: the preference";
  EXPECT_EQ(root_computed("color-scheme: light dark" + ld, "color", &light), "red");
  EXPECT_EQ(root_computed("color-scheme: dark light" + ld, "color", &light), "red") << "both: the preference, not the first";
  EXPECT_EQ(root_computed("color-scheme: only dark" + ld, "color", &light), "blue");
  EXPECT_EQ(root_computed("color-scheme: only light" + ld, "color", &dark), "red");
  EXPECT_EQ(root_computed("color-scheme: foo" + ld, "color", &dark), "red") << "no scheme the user agent supports: its default, light";
  EXPECT_EQ(root_computed("color-scheme: foo dark" + ld, "color", &light), "blue");
  EXPECT_EQ(root_computed("color-scheme: DARK" + ld, "color", &light), "blue") << "keywords in any case";
  // color-scheme is inherited, so a child of a dark element is dark, and `normal` on the child is the page's
  EXPECT_EQ(computed("color-scheme: dark", "color: light-dark(red, blue)", "color", &light), "blue");
  EXPECT_EQ(computed("color-scheme: dark", "color-scheme: normal; color: light-dark(red, blue)", "color", &light), "red");
  EXPECT_EQ(computed("color-scheme: light", "color-scheme: dark; color: light-dark(red, blue)", "color", &light), "blue");
  // a background and a border take the same scheme
  EXPECT_EQ(root_computed("color-scheme: dark; background-color: light-dark(white, black)", "background-color", &light), "black");
}

/// The old unkeyed priority (FNV-1a over 32 bits and a finalizer): names chosen against it made a chain.
uint32_t old_priority(const std::string & name) {
  uint32_t h = 2166136261u;
  for (unsigned char c : name) {
    h = (h ^ c) * 16777619u;
  }
  h ^= h >> 15;
  h *= 2246822519u;
  h ^= h >> 13;
  return h;
}

/// The longest run of names that grows in name and in old priority together: each is the child of the one before it in the old tree.
std::vector<std::string> chain_against_the_old_hash() {
  std::vector<std::string> names;
  for (int i = 0; i < 120000; i++) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "--k%06d", i);
    names.emplace_back(buf);
  }
  std::vector<size_t> tails;   // tails[k]: the index of the smallest priority ending a run of k + 1
  std::vector<long> prev(names.size(), -1);
  for (size_t i = 0; i < names.size(); i++) {
    const uint32_t p = old_priority(names[i]);
    size_t lo = 0, hi = tails.size();
    while (lo < hi) {
      const size_t mid = (lo + hi) / 2;
      if (old_priority(names[tails[mid]]) < p) {
        lo = mid + 1;
      }
      else {
        hi = mid;
      }
    }
    prev[i] = lo ? (long)tails[lo - 1] : -1;
    if (lo == tails.size()) {
      tails.push_back(i);
    }
    else {
      tails[lo] = i;
    }
  }
  std::vector<std::string> run;
  for (long i = tails.empty() ? -1 : (long)tails.back(); i >= 0; i = prev[(size_t)i]) {
    run.push_back(names[(size_t)i]);
  }
  return run;
}

TEST(CssComputeEdges, NamesChosenAgainstTheBalanceOfTheCustomPropertiesDoNotMakeAnError) {
  const std::vector<std::string> chain = chain_against_the_old_hash();
  ASSERT_GT(chain.size(), 300u) << "the construction finds a chain longer than the old limit on the depth";
  std::string decls;
  for (const std::string & name : chain) {
    decls += name + ": 1; ";
  }
  for (uint64_t seed : {0ull, 1ull, 42ull}) {
    GTEXT_CSS_Compute_Options o = gtext_css_compute_options_default();
    o.custom_hash_seed = seed;
    Context ctx(R(), nullptr, &o);
    ASSERT_EQ(ctx.status, GTEXT_CSS_OK);
    Block b(R(), decls);
    ASSERT_EQ(gtext_css_block_count(b.b), chain.size());
    Style s;
    s.compute(ctx, &b);
    EXPECT_EQ(s.status, GTEXT_CSS_OK) << "seed " << seed << ": " << s.result.name;
    EXPECT_EQ(gtext_css_computed_custom_count(s.s), chain.size());
    // the same names, in the order of the map
    std::set<std::string> want(chain.begin(), chain.end());
    for (size_t i = 0; i < chain.size() && s.s; i++) {
      GTEXT_CSS_String name;
      ASSERT_TRUE(gtext_css_computed_custom_at(s.s, i, &name, nullptr));
      EXPECT_EQ(std::string(name.data, name.length), *std::next(want.begin(), (long)i));
    }
  }
}

TEST(CssComputeEdges, AnErrorInACustomPropertyNamesNoneOfTheOtherProperties) {
  Context ctx(R());
  Block b(R(), "--a: 1");
  // a custom property declared revert is refused by the cascade's rule, naming the custom property class and not the last property computed
  GTEXT_CSS_Declaration_Block * rb = nullptr;
  ASSERT_EQ(gtext_css_block_new(R(), nullptr, &rb), GTEXT_CSS_OK);
  GTEXT_CSS_Property_Value pv;
  ASSERT_EQ(gtext_css_parse_property_text(R(), "--b", 3, "revert", 6, nullptr, &pv), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_block_add_custom(rb, "--b", 3, &pv, false), GTEXT_CSS_OK);
  gtext_css_property_value_free(&pv);
  GTEXT_CSS_Computed_Style * out = nullptr;
  GTEXT_CSS_Compute_Result r{};
  EXPECT_EQ(gtext_css_compute_style(ctx.c, rb, nullptr, nullptr, &out, &r), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(std::string(r.name), "--*");
  gtext_css_block_free(rb);
}

/// The text of a custom property of a style: "<none>" for a name the style does not have.
std::string custom_text(const Style & s, const std::string & name) {
  for (size_t i = 0; i < gtext_css_computed_custom_count(s.s); i++) {
    GTEXT_CSS_String n;
    const GTEXT_CSS_Custom_Value * v = nullptr;
    if (gtext_css_computed_custom_at(s.s, i, &n, &v) && std::string(n.data, n.length) == name) {
      char * text = nullptr;
      size_t length = 0;
      if (gtext_css_custom_value_text(v, nullptr, &text, &length) != GTEXT_CSS_OK) {
        return "<error>";
      }
      std::string out(text, length);
      gtext_css_serialize_buffer_free(nullptr, text);
      return out;
    }
  }
  return "<none>";
}

TEST(CssComputeEdges, ACustomPropertyDeclaredTwiceTakesTheLastAndUnsetInheritsIt) {
  Context ctx(R());
  Block pb(R(), "--a: parent; --b: 2");
  Style parent;
  parent.compute(ctx, &pb);
  Block twice(R(), "--a: 1; --c: x; --a: 2; --a: 3");
  Style s;
  s.compute(ctx, &twice, &parent);
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  EXPECT_EQ(custom_text(s, "--a"), "3") << "the last declaration of a name";
  EXPECT_EQ(custom_text(s, "--b"), "2") << "inherited";
  EXPECT_EQ(custom_text(s, "--c"), "x");
  // unset on an inherited property inherits: the parent's value, and none where the parent has none
  Block unset(R(), "--a: 9; --a: unset; --d: unset");
  Style u;
  u.compute(ctx, &unset, &parent);
  ASSERT_EQ(u.status, GTEXT_CSS_OK);
  EXPECT_EQ(custom_text(u, "--a"), "parent");
  EXPECT_EQ(custom_text(u, "--d"), "<none>");
  // initial hides what the parent has, and inherit takes it
  Block initial(R(), "--a: initial; --b: inherit");
  Style i;
  i.compute(ctx, &initial, &parent);
  EXPECT_EQ(custom_text(i, "--a"), "<none>");
  EXPECT_EQ(custom_text(i, "--b"), "2");
  // a child of the hidden name sees it hidden
  Block none(R(), "");
  Style grandchild;
  grandchild.compute(ctx, &none, &i);
  EXPECT_EQ(custom_text(grandchild, "--a"), "<none>");
  EXPECT_EQ(Block(R(), "--a: 1; --b: 2; --a: 3").css(), "--b: 2; --a: 3;") << "cssText writes the last declaration of a name, where it was";
  EXPECT_EQ(Block(R(), "--a: 1; --a: 2").css(), "--a: 2;");
  EXPECT_EQ(Block(R(), "--B: 1; --b: 2").css(), "--B: 1; --b: 2;") << "names are case sensitive";
}

TEST(CssComputeEdges, AllResetsEveryPropertyButDirectionAndUnicodeBidi) {
  // CSS Cascade 5, 3.2: "The all property is a shorthand that resets all CSS properties except direction and unicode-bidi."
  Context ctx(R());
  Block b(R(), "unicode-bidi: embed; direction: rtl; margin-left: 5px; color: red; all: initial");
  Style s;
  s.compute(ctx, &b);
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  EXPECT_EQ(s.value("unicode-bidi"), "embed");
  EXPECT_EQ(s.value("direction"), "rtl");
  EXPECT_EQ(s.value("margin-left"), "0px");
  EXPECT_NE(s.value("color"), "red");
  Block c(R(), "unicode-bidi: embed; direction: rtl; all: inherit");
  Style parent, child;
  Block pb(R(), "unicode-bidi: isolate; direction: ltr");
  parent.compute(ctx, &pb);
  child.compute(ctx, &c, &parent);
  EXPECT_EQ(child.value("unicode-bidi"), "embed");
  EXPECT_EQ(child.value("direction"), "rtl");
  // all sets a custom property to nothing: it does not touch them
  Block cust(R(), "--k: 1; all: initial");
  Style k;
  k.compute(ctx, &cust);
  EXPECT_EQ(custom_text(k, "--k"), "1");
}

TEST(CssComputeEdges, FlexFlowIsItsTwoLonghandsInEitherOrder) {
  Context ctx(R());
  struct Case {
    const char * decl;
    const char * direction;
    const char * wrap;
  };
  for (const Case & c : {Case{"flex-flow: row wrap", "row", "wrap"}, Case{"flex-flow: wrap row", "row", "wrap"}, Case{"flex-flow: column", "column", "nowrap"},
           Case{"flex-flow: wrap-reverse", "row", "wrap-reverse"}, Case{"flex-flow: wrap-reverse column-reverse", "column-reverse", "wrap-reverse"},
           Case{"flex-flow: column-reverse nowrap", "column-reverse", "nowrap"}, Case{"flex-flow: initial", "row", "nowrap"}}) {
    Block b(R(), c.decl);
    ASSERT_EQ(gtext_css_block_count(b.b), c.decl == std::string("flex-flow: initial") ? 2u : 2u) << c.decl;
    Style s;
    s.compute(ctx, &b);
    EXPECT_EQ(s.value("flex-direction"), c.direction) << c.decl;
    EXPECT_EQ(s.value("flex-wrap"), c.wrap) << c.decl;
  }
}

TEST(CssComputeEdges, TheInitialColorIsTheElementsOwnSchemeAndAnInitialValueThatDependsOnTheParentIsNotShared) {
  // a property of the caller whose initial value is light-dark(): the initial value is the element's, not one shared by every style
  GTEXT_CSS_Registry_Builder * b = nullptr;
  ASSERT_EQ(gtext_css_registry_builder_new(nullptr, &b), GTEXT_CSS_OK);
  GTEXT_CSS_Property_Def d;
  std::memset(&d, 0, sizeof d);
  d.name = "vector-ink";
  d.grammar = "<color>";
  d.initial = "light-dark(red, blue)";
  d.computed = GTEXT_CSS_COMPUTED_COLOR;
  ASSERT_EQ(gtext_css_registry_register_property(b, &d, nullptr), GTEXT_CSS_OK);
  reg::Owned o;
  ASSERT_EQ(gtext_css_registry_freeze(b, &o.r, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Environment dark = gtext_css_environment_default();
  dark.color_scheme = GTEXT_CSS_SCHEME_DARK;
  GTEXT_CSS_Environment light = gtext_css_environment_default();
  struct Case {
    const char * decls;
    const GTEXT_CSS_Environment * env;
    const char * ink;
  };
  for (const Case & c : {Case{"", &light, "red"}, Case{"", &dark, "red"}, Case{"color-scheme: dark", &light, "blue"}, Case{"color-scheme: light", &dark, "red"},
           Case{"color-scheme: light dark", &dark, "blue"}}) {
    Context ctx(o.r, c.env);
    ASSERT_EQ(ctx.status, GTEXT_CSS_OK);
    Block blk(o.r, c.decls);
    Style s;
    s.compute(ctx, &blk);
    ASSERT_EQ(s.status, GTEXT_CSS_OK) << c.decls;
    const GTEXT_CSS_Value_Set * set = nullptr;
    ASSERT_EQ(gtext_css_computed_value(s.s, find(o.r, "vector-ink"), &set), GTEXT_CSS_OK);
    char * text = nullptr;
    size_t n = 0;
    ASSERT_EQ(gtext_css_serialize_value_buffer(gtext_css_value_set_root(set), nullptr, &text, &n), GTEXT_CSS_OK);
    EXPECT_EQ(std::string(text, n), c.ink) << "{" << c.decls << "} dark page: " << (c.env == &dark);
    gtext_css_serialize_buffer_free(nullptr, text);
  }
  // display's initial value is blockified at the root and not below it
  EXPECT_EQ(root_computed("", "display"), "block");
  EXPECT_EQ(computed("", "", "display"), "inline");
}

TEST(CssComputeEdges, ASiblingFunctionNamesTheCallbackThatAnswersItAndAVarNamesNothingOfTheKind) {
  Context ctx(R());
  struct Case {
    const char * decl;
    GTEXT_CSS_Status status;
    const char * name;
  };
  for (const Case & c : {Case{"width: calc(sibling-index() * 1px)", GTEXT_CSS_E_UNSUPPORTED, "sibling_index"},
           Case{"width: calc(1px * SIBLING-COUNT())", GTEXT_CSS_E_UNSUPPORTED, "sibling_count"}, Case{"z-index: sibling-index()", GTEXT_CSS_E_UNSUPPORTED, "sibling_index"},
           Case{"width: var(--w)", GTEXT_CSS_OK, ""},
           Case{"content: \"sibling-index()\" var(--x)", GTEXT_CSS_OK, ""}}) {
    Block b(R(), c.decl);
    ASSERT_EQ(gtext_css_block_count(b.b), 1u) << c.decl;
    Style s;
    s.compute(ctx, &b);
    EXPECT_EQ(s.status, c.status) << c.decl;
    EXPECT_EQ(std::string(s.result.name), c.name) << c.decl;
  }
}

TEST(CssComputeEdges, ValuesAnEnvironmentCallbackGivesAreCheckedAndNamedWhenTheyAreNotLengths) {
  struct Probe {
    double metric = 8.0, line = 20.0, container = 300.0;
  } probe;
  GTEXT_CSS_Environment e = gtext_css_environment_default();
  e.user = &probe;
  e.font_metric = [](void * u, const GTEXT_CSS_Font_Query *, GTEXT_CSS_Font_Metric, double * px) { *px = static_cast<Probe *>(u)->metric; return true; };
  e.normal_line_height = [](void * u, const GTEXT_CSS_Font_Query *, double * px) { *px = static_cast<Probe *>(u)->line; return true; };
  e.container_size = [](void * u, GTEXT_CSS_Element_Handle, GTEXT_CSS_Container_Axis, double * px) { *px = static_cast<Probe *>(u)->container; return true; };
  e.resolve_color = [](void *, const GTEXT_CSS_Color *, GTEXT_CSS_Color_Scheme, double rgba[4]) { rgba[0] = rgba[1] = rgba[2] = 300; rgba[3] = 2; return true; };
  const double nan = std::nan("");
  const double inf = HUGE_VAL;
  struct Case {
    double * field;
    double value;
    const char * decl;
    const char * name;
  };
  Context ctx(R(), &e);
  ASSERT_EQ(ctx.status, GTEXT_CSS_OK);
  for (const Case & c : {Case{&probe.metric, nan, "width: 2ex", "font_metric"}, Case{&probe.metric, -1, "width: 2ch", "font_metric"},
           Case{&probe.metric, inf, "width: 1cap", "font_metric"}, Case{&probe.line, nan, "width: 1lh", "normal_line_height"},
           Case{&probe.line, -3, "width: 1lh", "normal_line_height"}, Case{&probe.container, nan, "width: 5cqw", "container_size"},
           Case{&probe.container, -10, "width: 5cqw", "container_size"}, Case{&probe.container, inf, "width: 5cqi", "container_size"}}) {
    const double was = *c.field;
    *c.field = c.value;
    Block b(R(), c.decl);
    Style s;
    s.compute(ctx, &b);
    EXPECT_EQ(s.status, GTEXT_CSS_E_UNSUPPORTED) << c.decl << " " << c.value;
    EXPECT_EQ(std::string(s.result.name), c.name) << c.decl;
    *c.field = was;
  }
  // zero is a length, and good values still work
  probe.metric = 0;
  Block zero(R(), "width: 2ex");
  Style z;
  z.compute(ctx, &zero);
  EXPECT_EQ(z.status, GTEXT_CSS_OK);
  EXPECT_EQ(z.value("width"), "0px");
  probe.metric = 8;
  Block good(R(), "width: 2ex");
  Style g;
  g.compute(ctx, &good);
  EXPECT_EQ(g.value("width"), "16px");
  // a color that resolves to something outside the documented ranges is clamped, and to a NaN is refused naming the callback
  Block color(R(), "color: color-mix(in srgb, red, blue)");
  Style cs;
  cs.compute(ctx, &color);
  ASSERT_EQ(cs.status, GTEXT_CSS_OK);
  EXPECT_EQ(cs.value("color"), "rgb(255, 255, 255)");
  e.resolve_color = [](void *, const GTEXT_CSS_Color * c, GTEXT_CSS_Color_Scheme, double rgba[4]) {
    rgba[0] = c->kind == GTEXT_CSS_COLOR_MIX ? std::nan("") : 0;
    rgba[1] = rgba[2] = 0;
    rgba[3] = 1;
    return true;
  };
  Context ctx2(R(), &e);
  Style bad;
  bad.compute(ctx2, &color);
  EXPECT_EQ(bad.status, GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_EQ(std::string(bad.result.name), "resolve_color");
}

TEST(CssComputeEdges, InheritIsTheParentsComputedValueEvenWhereTheBorderStyleWouldHaveMadeItZero) {
  // CSS Cascade 5: "If the cascaded value of a property is the inherit keyword, the property's specified and computed values are the inherited value."
  // The rule that a border width is 0 under `none` and `hidden` is how a width is computed from a length, and an inherited value is not computed again.
  EXPECT_EQ(computed("border-top-style: solid; border-top-width: 5px", "border-top-style: none; border-top-width: inherit", "border-top-width"), "5px");
  EXPECT_EQ(computed("border-top-style: solid; border-top-width: 5px", "border-top-style: none; border-top-width: 7px", "border-top-width"), "0px");
  EXPECT_EQ(computed("border-top-style: solid; border-top-width: 5px", "border-top-style: none; border-top-width: unset", "border-top-width"), "0px")
      << "unset on a property that is not inherited is initial, which is medium, which is 0 under none";
  // a shorthand that holds var() is substituted
  Context ctx(R());
  Block b(R(), "margin: var(--x)");
  Style s;
  s.compute(ctx, &b);
  EXPECT_EQ(s.status, GTEXT_CSS_OK) << "a shorthand that holds var() is substituted whole and expanded (it was refused before the substitution layer)";
  // compute_style takes the block in ascending priority and has no use for importance: the last declaration of a property wins, !important or not
  EXPECT_EQ(root_computed("margin-top: 1px !important; margin-top: 2px", "margin-top"), "2px");
  EXPECT_EQ(root_computed("margin-top: 1px; margin-top: 2px !important", "margin-top"), "2px");
}

TEST(CssComputeEdges, ACalculationOfOneNodeWalksWithNoStackOffsetFromNull) {
  // a fuzzer's finding (the reproducer is in tests/fuzz/corpus/css-compute): the rebuild of a calculation offset a null stack by zero, which UBSan refuses
  EXPECT_EQ(root_computed("row-gap: calc(1em + 1px)", "row-gap"), "17px");
  EXPECT_EQ(root_computed("gap: 1px 2px; row-gap: calc(1em + 1px); column-gap: normal; aspect-ratio: 16 / 9", "row-gap"), "17px");
}

TEST(CssComputeEdges, EveryRequiredPropertyComputesOnAnEmptyRootWhetherOrNotItIsDeclared) {
  // the required properties (inherited, or an SVG 2 presentation attribute, or a longhand of one) are the ones a renderer reads: none of them may be
  // answered with "not implemented", declared as its initial value or not declared at all
  Context ctx(R());
  ASSERT_EQ(ctx.status, GTEXT_CSS_OK);
  Block empty(R(), "");
  Style undeclared;
  undeclared.compute(ctx, &empty);
  EXPECT_EQ(undeclared.status, GTEXT_CSS_OK) << undeclared.result.name;
  const size_t n = gtext_css_registry_property_count(R());
  size_t tried = 0;
  std::vector<std::string> failed;
  for (size_t id = 1; id <= n; id++) {
    GTEXT_CSS_Property_Info info;
    ASSERT_TRUE(gtext_css_registry_property_info(R(), static_cast<GTEXT_CSS_Property_Id>(id), &info));
    if (!gtext_css_compute_required(R(), static_cast<GTEXT_CSS_Property_Id>(id)) || (info.flags & (GTEXT_CSS_PROP_ALIAS | GTEXT_CSS_PROP_SHORTHAND)) ||
        !info.initial || std::string(info.name) == "--*") {
      continue;
    }
    for (const std::string & value : {std::string(info.initial), std::string("inherit"), std::string("initial"), std::string("unset")}) {
      GTEXT_CSS_Declaration_Block * b = nullptr;
      ASSERT_EQ(gtext_css_block_new(R(), nullptr, &b), GTEXT_CSS_OK);
      GTEXT_CSS_Property_Outcome outcome;
      gtext_css_block_add_text(b, info.name, info.name_length, value.data(), value.size(), false, nullptr, &outcome, nullptr);
      if (gtext_css_block_count(b) != 1) {
        gtext_css_block_free(b);   // the initial value is not a value of the grammar (the pin's prose): not this test's to compute
        continue;
      }
      GTEXT_CSS_Computed_Style * out = nullptr;
      GTEXT_CSS_Compute_Result r{};
      const GTEXT_CSS_Status st = gtext_css_compute_style(ctx.c, b, undeclared.s, nullptr, &out, &r);
      tried++;
      if (st != GTEXT_CSS_OK) {
        failed.push_back(std::string(info.name) + ": " + value + " -> " + gtext_css_status_string(st) + " " + r.name + " " + (r.detail ? r.detail : ""));
      }
      gtext_css_computed_free(out);
      gtext_css_block_free(b);
    }
  }
  EXPECT_GT(tried, 800u);
  for (const std::string & f : failed) {
    ADD_FAILURE() << f;
  }
}

/// A registry whose grammars for the properties that the pin leaves unresolved are the caller's (the vector pipeline overrides fill and stroke so).
struct OverrideRegistry {
  reg::Owned owned;
  GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;
  OverrideRegistry() {
    GTEXT_CSS_Registry_Builder * b = nullptr;
    if (gtext_css_registry_builder_new(nullptr, &b) != GTEXT_CSS_OK) {
      return;
    }
    struct Def {
      const char * name;
      const char * grammar;
      const char * initial;
    };
    const char * paint = "none | <color> | <url> [ none | <color> ]? | context-fill | context-stroke";
    const char * pitch = "<frequency [0Hz,\xe2\x88\x9e]> && absolute | [ [ x-low | low | medium | high | x-high ] || [ <frequency [0Hz,\xe2\x88\x9e]> | <percentage> ] ]";
    for (const Def & d : {Def{"fill", paint, "black"}, Def{"stroke", paint, "none"},
             Def{"voice-family", "[ <string> | <custom-ident>+ | male | female | neutral ]# | preserve", "preserve"},
             Def{"voice-pitch", pitch, "medium"}, Def{"voice-range", pitch, "medium"},
             Def{"voice-volume", "silent | [ [ x-soft | soft | medium | loud | x-loud ] || <number> ]", "medium"}}) {
      GTEXT_CSS_Property_Def def;
      std::memset(&def, 0, sizeof def);
      def.name = d.name;
      def.grammar = d.grammar;
      def.initial = d.initial;
      def.override = true;
      def.inherited = true;
      if (gtext_css_registry_register_property(b, &def, nullptr) != GTEXT_CSS_OK) {
        return;
      }
    }
    status = gtext_css_registry_freeze(b, &owned.r, nullptr);
  }
};

TEST(CssComputeEdges, TheRequiredPropertiesWhoseGrammarsThePinLeavesUnresolvedComputeUnderTheCallersGrammar) {
  // fill, stroke, voice-family, voice-pitch, voice-range and voice-volume are inherited (or SVG 2 presentation attributes) and have no grammar the
  // pin resolves: a caller that registers one (vector does for fill and stroke) gets a computed value, not GTEXT_CSS_E_UNSUPPORTED
  OverrideRegistry o;
  ASSERT_EQ(o.status, GTEXT_CSS_OK);
  Context ctx(o.owned.r);
  ASSERT_EQ(ctx.status, GTEXT_CSS_OK) << ctx.result.name << ": " << (ctx.result.detail ? ctx.result.detail : "") << " (voice-pitch's initial value is a keyword)";
  struct Case {
    const char * decl;
    GTEXT_CSS_Status status;
  };
  for (const Case & c : {Case{"fill: red", GTEXT_CSS_OK}, Case{"fill: url(#a) red", GTEXT_CSS_OK}, Case{"fill: context-fill", GTEXT_CSS_OK},
           Case{"fill: currentcolor", GTEXT_CSS_OK}, Case{"stroke: url(#a) none", GTEXT_CSS_OK}, Case{"fill: inherit", GTEXT_CSS_OK},
           Case{"voice-family: preserve", GTEXT_CSS_OK}, Case{"voice-family: male, \"x\", a b", GTEXT_CSS_OK},
           Case{"voice-pitch: medium", GTEXT_CSS_OK}, Case{"voice-pitch: 200Hz absolute", GTEXT_CSS_OK},
           Case{"voice-pitch: absolute 0.2kHz", GTEXT_CSS_OK}, Case{"voice-range: x-high", GTEXT_CSS_OK},
           Case{"voice-volume: loud 3", GTEXT_CSS_OK}, Case{"voice-volume: silent", GTEXT_CSS_OK},
           // the table of the voice's frequencies is not the environment's: named, never guessed
           Case{"voice-pitch: high 10%", GTEXT_CSS_E_UNSUPPORTED}, Case{"voice-range: 50%", GTEXT_CSS_E_UNSUPPORTED},
           Case{"voice-pitch: 20Hz", GTEXT_CSS_E_UNSUPPORTED}}) {
    Block b(o.owned.r, c.decl);
    ASSERT_EQ(gtext_css_block_count(b.b), 1u) << c.decl;
    Style s;
    s.compute(ctx, &b);
    EXPECT_EQ(s.status, c.status) << c.decl << ": " << s.result.name;
    if (c.status == GTEXT_CSS_E_UNSUPPORTED) {
      EXPECT_EQ(std::string(s.result.name).substr(0, 6), "voice-") << c.decl << " names the property";
    }
  }
  // the value computed for a frequency with `absolute` is in hertz
  Block b(o.owned.r, "voice-pitch: absolute 0.2kHz");
  Style s;
  s.compute(ctx, &b);
  const GTEXT_CSS_Value_Set * set = nullptr;
  ASSERT_EQ(gtext_css_computed_value(s.s, find(o.owned.r, "voice-pitch"), &set), GTEXT_CSS_OK);
  char * text = nullptr;
  size_t n = 0;
  ASSERT_EQ(gtext_css_serialize_value_buffer(gtext_css_value_set_root(set), nullptr, &text, &n), GTEXT_CSS_OK);
  EXPECT_EQ(std::string(text, n), "200Hz absolute");
  gtext_css_serialize_buffer_free(nullptr, text);
}

} // namespace
