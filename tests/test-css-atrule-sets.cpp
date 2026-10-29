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
 * The at-rule set (css_atrule_set.h): collection through conditions and layers, cascade order, and the finds.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_cascade_test_support.h"
#include "css_test_support.h"

using cas::R;

namespace {

struct Conds {
  GTEXT_CSS_Conditions * c = nullptr;
  explicit Conds(double width = 1280) {
    GTEXT_CSS_Media_Environment env = gtext_css_media_environment_default(nullptr);
    env.viewport.width = width;
    GTEXT_CSS_Conditions_Options o = gtext_css_conditions_options_default();
    o.media = &env;
    EXPECT_EQ(gtext_css_conditions_new(R(), &o, &c), GTEXT_CSS_OK);
  }
  Conds(const Conds &) = delete;
  Conds & operator=(const Conds &) = delete;
  ~Conds() {
    gtext_css_conditions_free(c);
  }
};

struct Sheets {
  std::vector<css_test::Parsed *> trees;
  GTEXT_CSS_Atrule_Set * set = nullptr;
  GTEXT_CSS_Status add_status = GTEXT_CSS_OK;
  Sheets(std::initializer_list<const char *> css, const GTEXT_CSS_Conditions * conds) {
    GTEXT_CSS_Atrule_Set_Options o = gtext_css_atrule_set_options_default();
    o.conditions = conds;
    EXPECT_EQ(gtext_css_atrule_set_new(R(), &o, &set), GTEXT_CSS_OK);
    for (const char * c : css) {
      auto * p = new css_test::Parsed();
      css_test::parse_into(*p, gtext_css_parse_stylesheet, c);
      trees.push_back(p);
      const GTEXT_CSS_Status st = gtext_css_atrule_set_add_tree(set, p->tree);
      if (st != GTEXT_CSS_OK) {
        add_status = st;
      }
    }
    if (add_status == GTEXT_CSS_OK) {
      EXPECT_EQ(gtext_css_atrule_set_freeze(set), GTEXT_CSS_OK);
    }
  }
  Sheets(const Sheets &) = delete;
  Sheets & operator=(const Sheets &) = delete;
  ~Sheets() {
    gtext_css_atrule_set_free(set);
    for (auto * p : trees) {
      delete p;
    }
  }
};

std::string S(const GTEXT_CSS_String & s) {
  return s.data ? std::string(s.data, s.length) : std::string();
}

}  // namespace

TEST(CssAtruleSet, KeyframesLastInHighestLayerWins) {
  Conds cd;
  Sheets s({"@layer a, b; @layer b { @keyframes k { from { color: red } } } @layer a { @keyframes k { from { color: blue } } } @keyframes k { from { color: green } }"}, cd.c);
  const GTEXT_CSS_Keyframes * k = gtext_css_atrule_set_keyframes(s.set, "k", 1);
  ASSERT_NE(k, nullptr);
  ASSERT_EQ(k->frame_count, 1u);
  EXPECT_EQ(S(k->frames[0].properties[0].text), "green");  // unlayered beats every layer
  EXPECT_EQ(gtext_css_atrule_set_keyframes(s.set, "other", 5), nullptr);
}

TEST(CssAtruleSet, LayerOrderBeatsDocumentOrder) {
  Conds cd;
  Sheets s({"@layer a, b; @layer b { @keyframes k { from { color: red } } } @layer a { @keyframes k { from { color: blue } } }"}, cd.c);
  const GTEXT_CSS_Keyframes * k = gtext_css_atrule_set_keyframes(s.set, "k", 1);
  ASSERT_NE(k, nullptr);
  EXPECT_EQ(S(k->frames[0].properties[0].text), "red");  // b is declared after a, so it is the higher layer though it appears first
}

TEST(CssAtruleSet, ConditionsGateRules) {
  Conds narrow(400);
  Sheets s({"@media (min-width: 800px) { @keyframes k { from { color: red } } } @media (max-width: 800px) { @keyframes j { from { color: blue } } }"}, narrow.c);
  EXPECT_EQ(gtext_css_atrule_set_keyframes(s.set, "k", 1), nullptr);
  EXPECT_NE(gtext_css_atrule_set_keyframes(s.set, "j", 1), nullptr);
}

TEST(CssAtruleSet, FalseGroupDeclaresNoLayer) {
  Conds narrow(400);
  Sheets s({"@media (min-width: 800px) { @layer z { } } @layer a { @keyframes k { from { color: red } } }"}, narrow.c);
  EXPECT_EQ(gtext_css_layer_find(gtext_css_atrule_set_layers(s.set), GTEXT_CSS_ORIGIN_AUTHOR, GTEXT_CSS_LAYER_NONE, "z", 1), GTEXT_CSS_LAYER_NONE);
  EXPECT_NE(gtext_css_layer_find(gtext_css_atrule_set_layers(s.set), GTEXT_CSS_ORIGIN_AUTHOR, GTEXT_CSS_LAYER_NONE, "a", 1), GTEXT_CSS_LAYER_NONE);
}

TEST(CssAtruleSet, NoConditionsIsUnsupportedNeverAGuess) {
  Sheets s({"@media screen { @keyframes k { from { color: red } } }"}, nullptr);
  EXPECT_EQ(s.add_status, GTEXT_CSS_E_UNSUPPORTED);
  // a group with nothing the set collects is no problem
  Sheets t({"@media screen { a { color: red } }"}, nullptr);
  EXPECT_EQ(t.add_status, GTEXT_CSS_OK);
}

TEST(CssAtruleSet, FontFacesInCascadeOrder) {
  Conds cd;
  Sheets s({"@font-face { font-family: A; src: local(x); font-weight: 400 }", "@font-face { font-family: a; src: local(y); font-weight: 700 } @font-face { font-family: B; src: local(z) } "
            "@font-face { font-family: A }"}, cd.c);
  char key[16];
  const size_t kn = gtext_css_atrule_set_family_key("A", 1, key, sizeof key);
  const GTEXT_CSS_Font_Face * faces[4];
  ASSERT_EQ(gtext_css_atrule_set_font_faces(s.set, key, kn, faces, 4), 2u);  // the face without src is not defined
  EXPECT_EQ(faces[0]->weight_min, 400);
  EXPECT_EQ(faces[1]->weight_min, 700);
  EXPECT_EQ(gtext_css_atrule_set_font_faces(s.set, "nope", 4, faces, 4), 0u);
}

TEST(CssAtruleSet, FeatureValuesMerge) {
  Conds cd;
  Sheets s({"@font-feature-values A { @styleset { nice: 1; keep: 5 } }", "@font-feature-values A, B { @styleset { nice: 2 } @swash { s: 3 } }"}, cd.c);
  const GTEXT_CSS_Feature_Values * v = gtext_css_atrule_set_feature_values(s.set, "a", 1);
  ASSERT_NE(v, nullptr);
  ASSERT_EQ(v->value_count[GTEXT_CSS_FEATURE_STYLESET], 2u);
  // sorted by name: keep, nice (the later nice replaced the earlier)
  EXPECT_EQ(S(v->values[GTEXT_CSS_FEATURE_STYLESET][0].name), "keep");
  EXPECT_EQ(v->values[GTEXT_CSS_FEATURE_STYLESET][1].values[0], 2u);
  EXPECT_EQ(v->value_count[GTEXT_CSS_FEATURE_SWASH], 1u);
  const GTEXT_CSS_Feature_Values * b = gtext_css_atrule_set_feature_values(s.set, "b", 1);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(b->value_count[GTEXT_CSS_FEATURE_STYLESET], 1u);
}

TEST(CssAtruleSet, PaletteAndProfile) {
  Conds cd;
  Sheets s({"@font-palette-values --p { font-family: A; base-palette: 1 } @font-palette-values --p { font-family: A; base-palette: 2 } @color-profile --c { src: url(a.icc) }"}, cd.c);
  const GTEXT_CSS_Palette_Values * p = gtext_css_atrule_set_palette_values(s.set, "--p", 3, "a", 1);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->base_index, 2u);
  EXPECT_EQ(gtext_css_atrule_set_palette_values(s.set, "--p", 3, "b", 1), nullptr);
  EXPECT_NE(gtext_css_atrule_set_color_profile(s.set, "--c", 3), nullptr);
}

TEST(CssAtruleSet, CounterStyleChains) {
  Conds cd;
  Sheets s({"@counter-style a { system: extends b; suffix: \"1\" } @counter-style b { system: extends c } @counter-style c { system: alphabetic; symbols: x y } "
            "@counter-style p { system: extends q } @counter-style q { system: extends p } @counter-style r { system: extends p } "
            "@counter-style f1 { system: cyclic; symbols: a; fallback: f2 } @counter-style f2 { system: cyclic; symbols: a; fallback: f1 } "
            "@counter-style u { system: extends nowhere }"}, cd.c);
  const GTEXT_CSS_Counter_Style * a = gtext_css_atrule_set_counter_style(s.set, "a", 1);
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->resolved_system, GTEXT_CSS_COUNTER_ALPHABETIC);
  ASSERT_NE(a->base, nullptr);
  EXPECT_EQ(S(a->base->name), "c");
  EXPECT_FALSE(a->extends_cycle);
  const GTEXT_CSS_Counter_Style * p = gtext_css_atrule_set_counter_style(s.set, "p", 1);
  EXPECT_TRUE(p->extends_cycle);
  EXPECT_EQ(p->resolved_system, GTEXT_CSS_COUNTER_NUMERIC);
  const GTEXT_CSS_Counter_Style * r = gtext_css_atrule_set_counter_style(s.set, "r", 1);
  EXPECT_FALSE(r->extends_cycle);  // it extends a style in a cycle, which extends decimal
  EXPECT_EQ(r->resolved_system, GTEXT_CSS_COUNTER_NUMERIC);
  const GTEXT_CSS_Counter_Style * f1 = gtext_css_atrule_set_counter_style(s.set, "f1", 2);
  EXPECT_TRUE(f1->fallback_cycle);
  EXPECT_EQ(f1->fallback_style, nullptr);
  const GTEXT_CSS_Counter_Style * u = gtext_css_atrule_set_counter_style(s.set, "u", 1);
  EXPECT_EQ(u->resolved_system, GTEXT_CSS_COUNTER_NUMERIC);
  EXPECT_EQ(u->base, nullptr);
}

TEST(CssAtruleSet, ViewTransitionAndPositionTry) {
  Conds cd;
  Sheets s({"@view-transition { navigation: auto } @view-transition { navigation: none } @position-try --a { top: 0 } @position-try --a { top: 1px }"}, cd.c);
  const GTEXT_CSS_Atrule * vt = gtext_css_atrule_set_view_transition(s.set);
  ASSERT_NE(vt, nullptr);
  GTEXT_CSS_Atrule_Descriptor d;
  ASSERT_TRUE(gtext_css_atrule_descriptor_find(vt, "navigation", 10, &d));
  EXPECT_EQ(S(d.text), "none");
  const GTEXT_CSS_Atrule * t = gtext_css_atrule_set_position_try(s.set, "--a", 3);
  ASSERT_NE(t, nullptr);
  ASSERT_TRUE(gtext_css_atrule_descriptor_find(t, "top", 3, &d));
  EXPECT_EQ(S(d.text), "1px");
}

TEST(CssAtruleSet, PageCollectSpecificityOrder) {
  Conds cd;
  Sheets s({"@page { margin: 1px } @page :first { margin: 2px } @page :left { margin: 3px } @page named:first { margin: 4px } @page :first { margin: 5px }"}, cd.c);
  GTEXT_CSS_Page_Context pc;
  pc.name.data = "named";
  pc.name.length = 5;
  pc.first = true;
  pc.blank = false;
  pc.side = GTEXT_CSS_PAGE_SIDE_RIGHT;
  GTEXT_CSS_Page_Match m[8];
  const size_t n = gtext_css_atrule_set_page_collect(s.set, &pc, m, 8);
  ASSERT_EQ(n, 4u);  // not :left
  GTEXT_CSS_Atrule_Descriptor d;
  std::vector<std::string> margins;
  for (size_t i = 0; i < n; i++) {
    ASSERT_TRUE(gtext_css_atrule_descriptor_find(m[i].rule, "margin", 6, &d));
    margins.push_back(S(d.text));
  }
  // least specific first: (0,0,0) 1px, (0,1,0) 2px then 5px by order, (1,1,0) 4px last
  EXPECT_EQ((std::vector<std::string>{"1px", "2px", "5px", "4px"}), margins);
  pc.name.data = nullptr;
  pc.name.length = 0;
  pc.first = false;
  pc.side = GTEXT_CSS_PAGE_SIDE_LEFT;
  EXPECT_EQ(gtext_css_atrule_set_page_collect(s.set, &pc, m, 8), 2u);
}

TEST(CssAtruleSet, EntriesAndLimits) {
  Conds cd;
  Sheets s({"@keyframes a { from { color: red } } @layer l { @keyframes b { from { color: red } } }"}, cd.c);
  ASSERT_EQ(gtext_css_atrule_set_count(s.set), 2u);
  GTEXT_CSS_Atrule_Entry e0, e1;
  ASSERT_TRUE(gtext_css_atrule_set_entry(s.set, 0, &e0));
  ASSERT_TRUE(gtext_css_atrule_set_entry(s.set, 1, &e1));
  EXPECT_LT(e0.layer_rank, e1.layer_rank);  // the layered rule first, the unlayered last
  EXPECT_FALSE(gtext_css_atrule_set_entry(s.set, 2, &e0));
  // adds after freeze are refused
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_stylesheet, "@keyframes z { from { color: red } }");
  EXPECT_EQ(gtext_css_atrule_set_add_tree(s.set, p.tree), GTEXT_CSS_E_INVALID);
  // the rule limit
  GTEXT_CSS_Atrule_Set_Options o = gtext_css_atrule_set_options_default();
  o.conditions = cd.c;
  o.max_rules = 1;
  GTEXT_CSS_Atrule_Set * set = nullptr;
  ASSERT_EQ(gtext_css_atrule_set_new(R(), &o, &set), GTEXT_CSS_OK);
  css_test::Parsed q;
  css_test::parse_into(q, gtext_css_parse_stylesheet, "@keyframes a { from { color: red } } @keyframes b { from { color: red } }");
  EXPECT_EQ(gtext_css_atrule_set_add_tree(set, q.tree), GTEXT_CSS_E_LIMIT_DESCRIPTORS);
  gtext_css_atrule_set_free(set);
}

// Deep nesting does not recurse: the depth limit is a status, and a deep legal nest works.
TEST(CssAtruleSet, DeepNesting) {
  Conds cd;
  std::string css;
  const int depth = 200;
  for (int i = 0; i < depth; i++) {
    css += "@media all { ";
  }
  css += "@keyframes k { from { color: red } }";
  for (int i = 0; i < depth; i++) {
    css += " }";
  }
  Sheets s({css.c_str()}, cd.c);
  ASSERT_EQ(s.add_status, GTEXT_CSS_OK);
  EXPECT_NE(gtext_css_atrule_set_keyframes(s.set, "k", 1), nullptr);
}
