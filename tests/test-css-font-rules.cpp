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
 * The typed views for the font at-rules, counter styles and keyframes (css_font_rules.h, css_counter_style.h, css_keyframes.h).
 */

#include <string>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_cascade_test_support.h"
#include "css_test_support.h"

using cas::R;

namespace {

struct Rule {
  css_test::Parsed p;
  GTEXT_CSS_Atrule * a = nullptr;
  explicit Rule(const std::string & css, GTEXT_CSS_Atrule_Parent parent = GTEXT_CSS_ATRULE_PARENT_TOP) {
    css_test::parse_into(p, gtext_css_parse_stylesheet, css);
    GTEXT_CSS_Atrule_Options o = gtext_css_atrule_options_default();
    o.parent = parent;
    GTEXT_CSS_Atrule_Result res;
    EXPECT_EQ(gtext_css_atrule_parse(R(), &p.tree->rules[0], &o, &a, &res), GTEXT_CSS_OK);
  }
  Rule(const Rule &) = delete;
  Rule & operator=(const Rule &) = delete;
  ~Rule() {
    gtext_css_atrule_free(a);
  }
};

std::string S(const GTEXT_CSS_String & s) {
  return s.data ? std::string(s.data, s.length) : std::string();
}

}  // namespace

TEST(CssFontRules, FaceDescriptors) {
  Rule r("@font-face { font-family: \"My Font\"; src: url(a.woff2) format(woff2) tech(variations), local(\"A b\"); font-weight: 400 700; font-style: oblique 10deg 20deg; "
         "unicode-range: U+25-FF, U+4??, U+30-40; font-feature-settings: \"liga\" 1, \"kern\"; font-variation-settings: \"wght\" 300; font-display: swap; "
         "ascent-override: 90% 80%; size-adjust: 110%; font-width: 75% 100%; font-size: 10 20 }");
  GTEXT_CSS_Font_Face * f = nullptr;
  ASSERT_EQ(gtext_css_font_face_from_atrule(r.a, &f), GTEXT_CSS_OK);
  EXPECT_TRUE(gtext_css_font_face_defined(f));
  EXPECT_EQ(S(f->family), "My Font");
  EXPECT_EQ(S(f->family_key), "my font");
  ASSERT_EQ(f->src_count, 2u);
  EXPECT_EQ(f->src[0].kind, GTEXT_CSS_FONT_SRC_URL);
  EXPECT_EQ(S(f->src[0].url), "a.woff2");
  EXPECT_TRUE(f->src[0].has_format);
  EXPECT_NE(f->src[0].format_bit, 0u);
  EXPECT_NE(f->src[0].tech, 0u);
  EXPECT_EQ(f->src[1].kind, GTEXT_CSS_FONT_SRC_LOCAL);
  EXPECT_EQ(S(f->src[1].local), "A b");
  EXPECT_FALSE(f->weight_auto);
  EXPECT_EQ(f->weight_min, 400);
  EXPECT_EQ(f->weight_max, 700);
  EXPECT_EQ(f->style, GTEXT_CSS_FACE_STYLE_OBLIQUE);
  EXPECT_EQ(f->oblique_min, 10);
  EXPECT_EQ(f->oblique_max, 20);
  // 25-FF, 30-40 (inside) and 400-4FF: merged, sorted, non-overlapping
  ASSERT_EQ(f->range_count, 2u);
  EXPECT_EQ(f->ranges[0].first, 0x25u);
  EXPECT_EQ(f->ranges[0].last, 0xFFu);
  EXPECT_EQ(f->ranges[1].first, 0x400u);
  EXPECT_EQ(f->ranges[1].last, 0x4FFu);
  ASSERT_EQ(f->feature_count, 2u);
  EXPECT_STREQ(f->features[0].tag, "liga");
  EXPECT_EQ(f->features[0].value, 1);
  EXPECT_EQ(f->features[1].value, 1);
  ASSERT_EQ(f->variation_count, 1u);
  EXPECT_EQ(f->variations[0].value, 300);
  EXPECT_EQ(f->display, GTEXT_CSS_FONT_DISPLAY_SWAP);
  EXPECT_EQ(f->ascent[0].kind, 2);
  EXPECT_EQ(f->ascent[0].percent, 90);
  EXPECT_EQ(f->ascent[1].percent, 80);
  EXPECT_EQ(f->size_adjust, 110);
  EXPECT_FALSE(f->width_auto);
  EXPECT_EQ(f->width_min, 75);
  EXPECT_EQ(f->width_max, 100);
  EXPECT_FALSE(f->font_size_auto);
  EXPECT_EQ(f->font_size_max, 20);
  EXPECT_TRUE(f->present & GTEXT_CSS_FACE_HAS_DISPLAY);
  EXPECT_FALSE(f->present & GTEXT_CSS_FACE_HAS_NAMED_INSTANCE);
  gtext_css_font_face_free(f);
}

TEST(CssFontRules, FaceDefaults) {
  Rule r("@font-face { font-family: A; src: local(A) }");
  GTEXT_CSS_Font_Face * f = nullptr;
  ASSERT_EQ(gtext_css_font_face_from_atrule(r.a, &f), GTEXT_CSS_OK);
  EXPECT_TRUE(f->weight_auto);
  EXPECT_TRUE(f->width_auto);
  EXPECT_EQ(f->style, GTEXT_CSS_FACE_STYLE_AUTO);
  ASSERT_EQ(f->range_count, 1u);
  EXPECT_EQ(f->ranges[0].first, 0u);
  EXPECT_EQ(f->ranges[0].last, 0x10FFFFu);
  EXPECT_EQ(f->size_adjust, 100);
  EXPECT_EQ(f->present, GTEXT_CSS_FACE_HAS_FAMILY | GTEXT_CSS_FACE_HAS_SRC);
  gtext_css_font_face_free(f);
}

TEST(CssFontRules, FaceNotDefinedWithoutSrc) {
  Rule r("@font-face { font-family: A }");
  GTEXT_CSS_Font_Face * f = nullptr;
  ASSERT_EQ(gtext_css_font_face_from_atrule(r.a, &f), GTEXT_CSS_OK);
  EXPECT_FALSE(gtext_css_font_face_defined(f));
  gtext_css_font_face_free(f);
}

// Full case folding: the family key of a name is the folded name.
TEST(CssFontRules, FamilyKeyFolds) {
  Rule r("@font-face { font-family: \"Stra\\DF e\"; src: local(A) }");
  GTEXT_CSS_Font_Face * f = nullptr;
  ASSERT_EQ(gtext_css_font_face_from_atrule(r.a, &f), GTEXT_CSS_OK);
  EXPECT_EQ(S(f->family_key), "strasse");
  char buf[64];
  const size_t n = gtext_css_atrule_set_family_key("STRASSE", 7, buf, sizeof buf);
  EXPECT_EQ(std::string(buf, n), "strasse");
  gtext_css_font_face_free(f);
}

TEST(CssFontRules, WrongKindRefused) {
  Rule r("@keyframes k { from { color: red } }");
  GTEXT_CSS_Font_Face * f = nullptr;
  EXPECT_EQ(gtext_css_font_face_from_atrule(r.a, &f), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(f, nullptr);
  GTEXT_CSS_Counter_Style * c = nullptr;
  EXPECT_EQ(gtext_css_counter_style_from_atrule(r.a, &c), GTEXT_CSS_E_INVALID);
}

TEST(CssFontRules, FeatureValues) {
  Rule r("@font-feature-values A, \"B c\" { font-display: swap; @styleset { nice: 12 3; other: 4 } @swash { s: 1 } @styleset { nice: 9 } }");
  GTEXT_CSS_Feature_Values * v = nullptr;
  ASSERT_EQ(gtext_css_feature_values_from_atrule(r.a, &v), GTEXT_CSS_OK);
  ASSERT_EQ(v->family_count, 2u);
  EXPECT_EQ(S(v->families[1]), "B c");
  EXPECT_EQ(S(v->family_keys[1]), "b c");
  EXPECT_TRUE(v->has_display);
  ASSERT_EQ(v->value_count[GTEXT_CSS_FEATURE_STYLESET], 2u);
  EXPECT_EQ(S(v->values[GTEXT_CSS_FEATURE_STYLESET][0].name), "nice");
  // a later block of the same type replaces a name
  ASSERT_EQ(v->values[GTEXT_CSS_FEATURE_STYLESET][0].value_count, 1u);
  EXPECT_EQ(v->values[GTEXT_CSS_FEATURE_STYLESET][0].values[0], 9u);
  ASSERT_EQ(v->value_count[GTEXT_CSS_FEATURE_SWASH], 1u);
  gtext_css_feature_values_free(v);
}

TEST(CssFontRules, PaletteValues) {
  Rule r("@font-palette-values --x { font-family: Bixa, \"Foo Bar\"; base-palette: 1; override-colors: 0 red, 3 #00f }");
  GTEXT_CSS_Palette_Values * v = nullptr;
  ASSERT_EQ(gtext_css_palette_values_from_atrule(r.a, &v), GTEXT_CSS_OK);
  EXPECT_EQ(S(v->name), "--x");
  ASSERT_EQ(v->family_count, 2u);
  EXPECT_EQ(S(v->family_keys[1]), "foo bar");
  EXPECT_EQ(v->base_kind, GTEXT_CSS_BASE_PALETTE_INDEX);
  EXPECT_EQ(v->base_index, 1u);
  ASSERT_EQ(v->override_count, 2u);
  EXPECT_EQ(v->overrides[0].index, 0u);
  EXPECT_EQ(S(v->overrides[0].color), "red");
  EXPECT_EQ(S(v->overrides[1].color), "rgb(0, 0, 255)");
  gtext_css_palette_values_free(v);
}

TEST(CssFontRules, ColorProfile) {
  Rule r("@color-profile --p { src: url(a.icc); rendering-intent: perceptual; components: a, b }");
  GTEXT_CSS_Color_Profile * v = nullptr;
  ASSERT_EQ(gtext_css_color_profile_from_atrule(r.a, &v), GTEXT_CSS_OK);
  EXPECT_EQ(S(v->name), "--p");
  EXPECT_EQ(S(v->src), "a.icc");
  EXPECT_EQ(v->intent, GTEXT_CSS_INTENT_PERCEPTUAL);
  EXPECT_EQ(v->component_count, 2u);
  gtext_css_color_profile_free(v);
}

TEST(CssCounterStyle, Descriptors) {
  Rule r("@counter-style x { system: fixed 5; symbols: a \"b\" c; range: 1 3, infinite 0; pad: 3 \"0\"; negative: \"-\" \"!\"; prefix: \"<\"; fallback: y; speak-as: words }");
  GTEXT_CSS_Counter_Style * c = nullptr;
  ASSERT_EQ(gtext_css_counter_style_from_atrule(r.a, &c), GTEXT_CSS_OK);
  EXPECT_EQ(S(c->name), "x");
  EXPECT_EQ(c->system, GTEXT_CSS_COUNTER_FIXED);
  EXPECT_EQ(c->fixed_first, 5);
  ASSERT_EQ(c->symbol_count, 3u);
  EXPECT_FALSE(c->symbols[0].string);
  EXPECT_TRUE(c->symbols[1].string);
  EXPECT_FALSE(c->range_auto);
  ASSERT_EQ(c->range_count, 2u);
  EXPECT_EQ(c->ranges[0].lower, 1);
  EXPECT_EQ(c->ranges[0].upper, 3);
  EXPECT_TRUE(c->ranges[1].lower_infinite);
  EXPECT_EQ(c->pad_length, 3u);
  EXPECT_EQ(S(c->pad_symbol.text), "0");
  EXPECT_EQ(S(c->negative_suffix.text), "!");
  EXPECT_EQ(S(c->prefix.text), "<");
  EXPECT_EQ(S(c->fallback), "y");
  EXPECT_EQ(S(c->speak_as), "words");
  EXPECT_TRUE(c->valid);
  gtext_css_counter_style_free(c);
}

TEST(CssCounterStyle, ValidityByCounts) {
  struct { const char * css; bool valid; } rows[] = {
    {"@counter-style x { system: cyclic; symbols: a }", true},
    {"@counter-style x { system: alphabetic; symbols: a }", false},
    {"@counter-style x { system: alphabetic; symbols: a b }", true},
    {"@counter-style x { system: additive; additive-symbols: 2 b, 1 a }", true},
    {"@counter-style x { system: additive; additive-symbols: 1 b, 2 a }", false},
    {"@counter-style x { system: extends decimal }", true},
  };
  for (const auto & row : rows) {
    Rule r(row.css);
    if (!r.a) {
      EXPECT_FALSE(row.valid) << row.css;
      continue;
    }
    GTEXT_CSS_Counter_Style * c = nullptr;
    ASSERT_EQ(gtext_css_counter_style_from_atrule(r.a, &c), GTEXT_CSS_OK);
    EXPECT_EQ(c->valid, row.valid) << row.css;
    gtext_css_counter_style_free(c);
  }
}

TEST(CssKeyframes, Normalized) {
  Rule r("@keyframes \"k\" { to { color: blue } 50% { color: red; width: 1px } from { color: green } 50% { width: 2px } 0%, 100% { height: 3px } }");
  GTEXT_CSS_Keyframes * k = nullptr;
  ASSERT_EQ(gtext_css_keyframes_from_atrule(r.a, &k), GTEXT_CSS_OK);
  EXPECT_EQ(S(k->name), "k");
  EXPECT_TRUE(k->name_is_string);
  EXPECT_EQ(k->block_count, 5u);
  ASSERT_EQ(k->frame_count, 3u);
  EXPECT_EQ(k->frames[0].offset, 0);
  EXPECT_EQ(k->frames[1].offset, 0.5);
  EXPECT_EQ(k->frames[2].offset, 1);
  // 0%: from { color: green } then 0%, 100% { height }
  EXPECT_EQ(k->frames[0].property_count, 2u);
  // 50%: color, width (the later width replaces the earlier)
  ASSERT_EQ(k->frames[1].property_count, 2u);
  EXPECT_EQ(S(k->frames[1].properties[1].text), "2px");
  gtext_css_keyframes_free(k);
}

// A huge number in a palette index or a counter value is clamped, not converted by an undefined cast.
TEST(CssFontRules, HugeIntegersAreClamped) {
  Rule r("@font-palette-values --x { font-family: A; base-palette: 1e300; override-colors: 1e300 red }");
  GTEXT_CSS_Palette_Values * v = nullptr;
  ASSERT_EQ(gtext_css_palette_values_from_atrule(r.a, &v), GTEXT_CSS_OK);
  gtext_css_palette_values_free(v);
  Rule c("@counter-style x { system: fixed 1e300; symbols: a; pad: 1e300 \"0\" }");
  GTEXT_CSS_Counter_Style * s = nullptr;
  ASSERT_EQ(gtext_css_counter_style_from_atrule(c.a, &s), GTEXT_CSS_OK);
  gtext_css_counter_style_free(s);
}
