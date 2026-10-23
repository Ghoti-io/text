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
 * The function types of the drafts (images and gradients, transforms, easing functions, basic shapes, substitution and generated content) as
 * grammars the engine matches, and the typed views of the ones that have a post-parse algorithm: the canonicalization of `linear()` and the
 * range of `steps()` (Easing 2), the stops of a gradient and what its serialization omits (Images 4, section 8), `image-set()` options, the
 * kind of a shape, the arguments of a transform function, and what each substitution function needs.
 */

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css/css_function.h>

#include "css_color_test_support.h"
#include "css_values_test_support.h"
#include "markup_test_support.h"

using namespace gv;

namespace {

// The typed views answer OK with `valid` false for a value that is not what they read (css_function.h); these keep the shape of the
// older assertions: OK for a valid value, E_INVALID for one that is not.
GTEXT_CSS_Status grad_from(const GTEXT_CSS_Value * v, size_t max_stops, GTEXT_CSS_Gradient ** g, const GTEXT_Allocator * a = nullptr) {
  bool valid = false;
  const GTEXT_CSS_Status st = gtext_css_gradient_from_value(v, max_stops, a, &valid, g);
  return st == GTEXT_CSS_OK && !valid ? GTEXT_CSS_E_INVALID : st;
}

GTEXT_CSS_Status set_from(const GTEXT_CSS_Value * v, GTEXT_CSS_Image_Set_Option ** o, size_t * n, const GTEXT_Allocator * a = nullptr) {
  bool valid = false;
  const GTEXT_CSS_Status st = gtext_css_image_set_options(v, a, &valid, o, n);
  return st == GTEXT_CSS_OK && !valid ? GTEXT_CSS_E_INVALID : st;
}

bool tf_from(const GTEXT_CSS_Value * v, GTEXT_CSS_Transform_Function * f) {
  bool valid = false;
  EXPECT_EQ(gtext_css_transform_function_from_value(v, nullptr, &valid, f), v ? GTEXT_CSS_OK : GTEXT_CSS_E_INVALID);
  return valid;
}

bool shape_from(const GTEXT_CSS_Value * v, GTEXT_CSS_Shape * sh) {
  bool valid = false;
  EXPECT_EQ(gtext_css_shape_from_value(v, nullptr, &valid, sh), v ? GTEXT_CSS_OK : GTEXT_CSS_E_INVALID);
  return valid;
}

} // namespace


namespace {

/// The function types, made once.
struct Types {
  GTEXT_CSS_Function_Types * t = nullptr;
  Types() {
    gtext_css_function_types_new(nullptr, &t);
  }
  ~Types() {
    gtext_css_function_types_free(t);
  }
};

Types & types() {
  static Types t;
  return t;
}

/// One match of `input` against `grammar` (type names resolved by the function types), owning everything.
struct One {
  Grammar g;
  Matched m;
  std::string text() const {
    return m.matched ? serialize(m.root()) : "no match";
  }
};

void run_into(One & o, const std::string & grammar, const std::string & input, const GTEXT_CSS_Match_Options * mo = nullptr) {
  o.g.status = gtext_css_compile(grammar.data(), grammar.size(), gtext_css_function_types_resolve, types().t, nullptr, &o.g.c, &o.g.result);
  ASSERT_EQ(o.g.status, GTEXT_CSS_OK) << grammar << " " << o.g.result.name;
  css_test::parse_into(o.m.parsed, gtext_css_parse_component_values, input);
  o.m.status = gtext_css_match(o.g.c, o.m.parsed.tree->values, o.m.parsed.tree->value_count, mo, &o.m.matched, &o.m.set, &o.m.result);
}

std::string run_f(const std::string & grammar, const std::string & input) {
  One o;
  run_into(o, grammar, input);
  if (o.m.status != GTEXT_CSS_OK) {
    return std::string("status:") + gtext_css_status_string(o.m.status);
  }
  return o.text();
}

struct Row {
  const char * grammar;
  const char * input;
  const char * expect;
};

void rows(const std::vector<Row> & rs) {
  for (const Row & r : rs) {
    EXPECT_EQ(run_f(r.grammar, r.input), r.expect) << r.grammar << " | " << r.input;
  }
}

}  // namespace

// ---- the table of types ----------------------------------------------------------------------------------------

TEST(CssFunctionTypes, EveryTypeParsesAndLinksAgainstTheOthers) {
  const size_t n = gtext_css_function_type_count();
  EXPECT_GT(n, 100u);
  for (size_t i = 0; i < n; i++) {
    const char * name = nullptr;
    const char * grammar = nullptr;
    GTEXT_CSS_Ref_Kind kind;
    ASSERT_TRUE(gtext_css_function_type_at(i, &name, &kind, &grammar));
    std::string ref = kind == GTEXT_CSS_REF_PROPERTY ? std::string("<'") + name + "'>" : std::string("<") + name + ">";
    GTEXT_CSS_Compiled * c = nullptr;
    GTEXT_CSS_Grammar_Result r;
    const GTEXT_CSS_Status st = gtext_css_compile(ref.data(), ref.size(), gtext_css_function_types_resolve, types().t, nullptr, &c, &r);
    EXPECT_EQ(st, GTEXT_CSS_OK) << ref << " " << r.name;
    gtext_css_compiled_free(c);
  }
  EXPECT_FALSE(gtext_css_function_type_at(n, nullptr, nullptr, nullptr));
}

TEST(CssFunctionTypes, NamesAreUniqueAndAnUnknownNameIsNotResolved) {
  std::vector<std::string> seen;
  for (size_t i = 0; i < gtext_css_function_type_count(); i++) {
    const char * name;
    GTEXT_CSS_Ref_Kind kind;
    ASSERT_TRUE(gtext_css_function_type_at(i, &name, &kind, nullptr));
    const std::string key = (kind == GTEXT_CSS_REF_PROPERTY ? "'" : "") + std::string(name);
    for (const std::string & s : seen) {
      EXPECT_NE(s, key);
    }
    seen.push_back(key);
  }
  EXPECT_EQ(gtext_css_function_types_resolve(types().t, GTEXT_CSS_REF_TYPE, "nosuch", 6), nullptr);
  EXPECT_NE(gtext_css_function_types_resolve(types().t, GTEXT_CSS_REF_TYPE, "image", 5), nullptr);
  EXPECT_EQ(gtext_css_function_types_resolve(types().t, GTEXT_CSS_REF_PROPERTY, "image", 5), nullptr);
  EXPECT_EQ(gtext_css_function_types_resolve(nullptr, GTEXT_CSS_REF_TYPE, "image", 5), nullptr);
}

TEST(CssFunctionTypes, ACallersResolverIsAskedFirst) {
  struct Ctx {
    int asked = 0;
    GTEXT_CSS_Definition * def = nullptr;
  } ctx;
  const char * mine = "mine";
  gtext_css_definition_parse(mine, 4, nullptr, &ctx.def, nullptr);
  GTEXT_CSS_Function_Types_Options o;
  std::memset(&o, 0, sizeof o);
  o.fallback = [](void * user, GTEXT_CSS_Ref_Kind, const char * name, size_t len) -> const GTEXT_CSS_Definition * {
    Ctx * c = static_cast<Ctx *>(user);
    if (std::string(name, len) == "'fill-rule'") {
      return nullptr;
    }
    if (std::string(name, len) == "fill-rule") {
      c->asked++;
      return c->def;
    }
    return nullptr;
  };
  o.fallback_user = &ctx;
  GTEXT_CSS_Function_Types * t = nullptr;
  ASSERT_EQ(gtext_css_function_types_new(&o, &t), GTEXT_CSS_OK);
  const GTEXT_CSS_Definition * d = gtext_css_function_types_resolve(t, GTEXT_CSS_REF_TYPE, "fill-rule", 9);
  EXPECT_EQ(d, ctx.def);
  EXPECT_EQ(ctx.asked, 1);
  EXPECT_NE(gtext_css_function_types_resolve(t, GTEXT_CSS_REF_TYPE, "image", 5), nullptr);  // not in the caller's: ours
  EXPECT_NE(gtext_css_function_types_resolve(t, GTEXT_CSS_REF_PROPERTY, "fill-rule", 9), nullptr);
  gtext_css_function_types_free(t);
  gtext_css_definition_free(ctx.def);
}

TEST(CssFunctionTypes, ADefinitionIsMadeOnceAndKeptUntilTheTypesAreFreed) {
  const GTEXT_CSS_Definition * a = gtext_css_function_types_resolve(types().t, GTEXT_CSS_REF_TYPE, "transform-function", 18);
  const GTEXT_CSS_Definition * b = gtext_css_function_types_resolve(types().t, GTEXT_CSS_REF_TYPE, "transform-function", 18);
  EXPECT_EQ(a, b);
  ASSERT_NE(a, nullptr);
  EXPECT_GT(gtext_css_definition_node_count(a), 20u);
}

// ---- images and gradients ---------------------------------------------------------------------------------------------

TEST(CssFunctionImage, TheKindsOfImage) {
  rows({{"<image>", "url(a.png)", "url(\"a.png\")"}, {"<image>", "url('a.png' cross-origin(anonymous))", "url(\"a.png\" cross-origin(anonymous))"},
      {"<image>", "src(\"a.png\")", "src(\"a.png\")"}, {"<image>", "image(red)", "image(red)"}, {"<image>", "image(rgb(1 2 3 / 50%))", "image(rgba(1, 2, 3, 0.5))"},
      {"<image>", "element(#foo)", "element(#foo)"}, {"<image>", "element(foo)", "no match"}, {"<image>", "paint(my-painter)", "paint(my-painter)"},
      {"<image>", "paint(my-painter, 1px 2px)", "paint(my-painter, 1px 2px)"}, {"<image>", "cross-fade(url(a.png) 25%, red)", "cross-fade(url(\"a.png\") 25%, red)"},
      {"<image>", "cross-fade(url(a.png) 125%, red)", "no match"}, {"<image>", "cross-fade(25% url(a.png), 25% blue)", "cross-fade(url(\"a.png\") 25%, blue 25%)"},
      {"<image>", "image-set(\"a.png\" 1x, url(b.png) 2x type(\"image/png\"))", "image-set(\"a.png\" 1x, url(\"b.png\") 2x type(\"image/png\"))"},
      {"<image>", "-webkit-image-set(\"a.png\" 1x)", "-webkit-image-set(\"a.png\" 1x)"}, {"<image>", "image-set()", "no match"}, {"<image>", "foo()", "no match"},
      {"<image>", "image-set(url(a) type(\"x\") 2x)", "image-set(url(\"a\") 2x type(\"x\"))"}});
}

TEST(CssFunctionImage, GradientsOfEveryKind) {
  rows({{"<image>", "linear-gradient(red, blue)", "linear-gradient(red, blue)"}, {"<image>", "linear-gradient(to right, red, blue)", "linear-gradient(to right, red, blue)"},
      {"<image>", "linear-gradient(45deg, red 10%, 20%, blue 30px 40px)", "linear-gradient(45deg, red 10%, 20%, blue 30px 40px)"},
      {"<image>", "linear-gradient(to top left, red, blue)", "linear-gradient(to left top, red, blue)"}, {"<image>", "linear-gradient(0, red, blue)", "linear-gradient(0, red, blue)"},
      {"<image>", "linear-gradient(in oklch longer hue to right, red, blue)", "linear-gradient(to right in oklch longer hue, red, blue)"},
      {"<image>", "linear-gradient(to right in srgb, red, blue)", "linear-gradient(to right in srgb, red, blue)"},
      {"<image>", "repeating-linear-gradient(red 0, blue 10px)", "repeating-linear-gradient(red 0px, blue 10px)"},
      {"<image>", "radial-gradient(red, blue)", "radial-gradient(red, blue)"}, {"<image>", "radial-gradient(circle at center, red 10%, blue)", "radial-gradient(circle at center, red 10%, blue)"},
      {"<image>", "radial-gradient(ellipse 10px 20% at 10px 20px, red, blue)", "radial-gradient(ellipse 10px 20% at 10px 20px, red, blue)"},
      {"<image>", "radial-gradient(closest-side, red, blue)", "radial-gradient(closest-side, red, blue)"}, {"<image>", "radial-gradient(at 10px 20px, red, blue)", "radial-gradient(at 10px 20px, red, blue)"},
      {"<image>", "radial-gradient(circle -1px, red, blue)", "no match"}, {"<image>", "repeating-radial-gradient(red, blue)", "repeating-radial-gradient(red, blue)"},
      {"<image>", "conic-gradient(from 45deg at 10px 20px, red, 10%, blue)", "conic-gradient(from 45deg at 10px 20px, red, 10%, blue)"},
      {"<image>", "conic-gradient(red 0deg, blue 90deg 180deg)", "conic-gradient(red 0deg, blue 90deg 180deg)"}, {"<image>", "conic-gradient(red, blue)", "conic-gradient(red, blue)"},
      {"<image>", "repeating-conic-gradient(red 0.25turn, 0, blue)", "repeating-conic-gradient(red 0.25turn, 0, blue)"},
      // the grammar's hint rules: a hint cannot be first, last, or next to another
      {"<image>", "linear-gradient(10%, red, blue)", "no match"}, {"<image>", "linear-gradient(red, blue, 10%)", "no match"},
      {"<image>", "linear-gradient(red, 10%, 20%, blue)", "no match"}, {"<image>", "linear-gradient(red)", "linear-gradient(red)"}, {"<image>", "linear-gradient(red, blue 1 2 3)", "no match"},
      {"<image>", "linear-gradient(red calc(10% + 5px), blue)", "linear-gradient(red calc(10% + 5px), blue)"},
      {"<image>", "linear-gradient(rgb(from red r g b), color-mix(in srgb, red, blue))", "linear-gradient(rgb(from red r g b), color-mix(in srgb, red, blue))"}});
}

TEST(CssFunctionImage, TheKindOfAnImageValueAndTheOptionsOfAnImageSet) {
  One url, grad, set, nope;
  run_into(url, "<image>", "url(a.png)");
  run_into(grad, "<image>", "conic-gradient(red, blue)");
  run_into(set, "<image>", "image-set(\"a.png\", url(b.png) 2x type(\"image/png\"), cross-fade(red) 1.5x)");
  run_into(nope, "<string>", "\"x\"");
  EXPECT_EQ(gtext_css_image_kind(url.m.root()), GTEXT_CSS_IMAGE_URL);
  EXPECT_EQ(gtext_css_image_kind(grad.m.root()), GTEXT_CSS_IMAGE_GRADIENT);
  EXPECT_EQ(gtext_css_image_kind(set.m.root()), GTEXT_CSS_IMAGE_IMAGE_SET);
  EXPECT_EQ(gtext_css_image_kind(nope.m.root()), GTEXT_CSS_IMAGE_NONE);
  EXPECT_EQ(gtext_css_image_kind(nullptr), GTEXT_CSS_IMAGE_NONE);
  GTEXT_CSS_Image_Set_Option * o = nullptr;
  size_t n = 0;
  if (!set.m.matched) {
    GTEST_FAIL() << "no match: " << set.text();
  }
  ASSERT_EQ(set_from(set.m.root(), &o, &n), GTEXT_CSS_OK);
  ASSERT_EQ(n, 3u);
  EXPECT_EQ(o[0].image->kind, GTEXT_CSS_VAL_STRING);
  EXPECT_FALSE(o[0].resolution_written);
  EXPECT_EQ(o[0].resolution, 1.0);  // "it behaves as 1x"
  EXPECT_EQ(o[0].type, nullptr);
  EXPECT_EQ(o[1].image->kind, GTEXT_CSS_VAL_URL);
  EXPECT_EQ(o[1].resolution, 2.0);
  EXPECT_TRUE(o[1].resolution_written);
  ASSERT_NE(o[1].type, nullptr);
  EXPECT_EQ(gtext_css_image_kind(o[2].image), GTEXT_CSS_IMAGE_CROSS_FADE);
  EXPECT_EQ(o[2].resolution, 1.5);
  gtext_allocator_free(gtext_allocator_default(), o);
  EXPECT_EQ(set_from(url.m.root(), &o, &n), GTEXT_CSS_E_INVALID);
}

TEST(CssFunctionImage, ResolutionsOfAnImageSetAreConvertedToDppx) {
  One set;
  run_into(set, "<image>", "image-set(\"a\" 96dpi, \"b\" 2x, \"c\" 192dpi, \"d\" 1dppx)");
  ASSERT_TRUE(set.m.matched);
  GTEXT_CSS_Image_Set_Option * o = nullptr;
  size_t n = 0;
  ASSERT_EQ(set_from(set.m.root(), &o, &n), GTEXT_CSS_OK);
  ASSERT_EQ(n, 4u);
  EXPECT_DOUBLE_EQ(o[0].resolution, 1.0);
  EXPECT_DOUBLE_EQ(o[1].resolution, 2.0);
  EXPECT_DOUBLE_EQ(o[2].resolution, 2.0);
  EXPECT_DOUBLE_EQ(o[3].resolution, 1.0);
  gtext_allocator_free(gtext_allocator_default(), o);
}

TEST(CssFunctionGradient, StopsAndHintsInOrderWithTheirPositions) {
  One o;
  run_into(o, "<gradient>", "repeating-linear-gradient(to right, red 10%, 20%, blue 30px 40px, green)");
  ASSERT_TRUE(o.m.matched);
  GTEXT_CSS_Gradient * g = nullptr;
  ASSERT_EQ(grad_from(o.m.root(), 0, &g), GTEXT_CSS_OK);
  EXPECT_EQ(g->kind, GTEXT_CSS_GRADIENT_LINEAR);
  EXPECT_TRUE(g->repeating);
  ASSERT_EQ(g->stop_count, 4u);
  EXPECT_NE(g->stops[0].color, nullptr);
  ASSERT_NE(g->stops[0].position, nullptr);
  EXPECT_EQ(g->stops[0].position->kind, GTEXT_CSS_VAL_PERCENTAGE);
  EXPECT_EQ(g->stops[1].color, nullptr);  // a hint
  ASSERT_NE(g->stops[1].position, nullptr);
  EXPECT_EQ(g->stops[2].position->kind, GTEXT_CSS_VAL_DIMENSION);
  ASSERT_NE(g->stops[2].position2, nullptr);
  EXPECT_EQ(g->stops[3].position, nullptr);
  gtext_css_gradient_free(g);
  One c, r;
  run_into(c, "<gradient>", "conic-gradient(from 0deg, red, blue)");
  run_into(r, "<gradient>", "radial-gradient(circle, red, blue)");
  ASSERT_EQ(grad_from(c.m.root(), 0, &g), GTEXT_CSS_OK);
  EXPECT_EQ(g->kind, GTEXT_CSS_GRADIENT_CONIC);
  EXPECT_FALSE(g->repeating);
  EXPECT_EQ(g->stop_count, 2u);
  gtext_css_gradient_free(g);
  ASSERT_EQ(grad_from(r.m.root(), 0, &g), GTEXT_CSS_OK);
  EXPECT_EQ(g->kind, GTEXT_CSS_GRADIENT_RADIAL);
  gtext_css_gradient_free(g);
  One notg;
  run_into(notg, "<image>", "url(x)");
  EXPECT_EQ(grad_from(notg.m.root(), 0, &g), GTEXT_CSS_E_INVALID);
}

TEST(CssFunctionGradient, TheNumberOfStopsIsBounded) {
  std::string in = "linear-gradient(red";
  for (int i = 0; i < 20; i++) {
    in += ", blue";
  }
  in += ")";
  One o;
  run_into(o, "<gradient>", in);
  ASSERT_TRUE(o.m.matched);
  GTEXT_CSS_Gradient * g = nullptr;
  ASSERT_EQ(grad_from(o.m.root(), 21, &g), GTEXT_CSS_OK);
  EXPECT_EQ(g->stop_count, 21u);
  gtext_css_gradient_free(g);
  g = nullptr;
  EXPECT_EQ(grad_from(o.m.root(), 20, &g), GTEXT_CSS_E_LIMIT_STOPS);
  EXPECT_EQ(g, nullptr);
}

TEST(CssFunctionGradient, TheSerializationOmitsWhatIsTheDefault) {
  // Images 4, 8: "omitting components when possible without changing the meaning"; the draft's example
  // Linear-Gradient( to bottom, red 0%,yellow,black 100px)
  struct S {
    const char * in;
    const char * out;
  };
  for (const S & s : std::vector<S>{{"Linear-Gradient( to bottom, red 0%,yellow,black 100px)", "linear-gradient(red, yellow, black 100px)"},
           {"linear-gradient(to bottom, red, blue)", "linear-gradient(red, blue)"}, {"linear-gradient(to top, red, blue)", "linear-gradient(to top, red, blue)"},
           {"linear-gradient(red 0px, blue 100%)", "linear-gradient(red, blue)"}, {"linear-gradient(red 10%, blue 90%)", "linear-gradient(red 10%, blue 90%)"},
           {"linear-gradient(red 0% 10%, blue 100% 100%)", "linear-gradient(red 0% 10%, blue 100% 100%)"}, {"radial-gradient(circle, red 0%, blue 100%)", "radial-gradient(circle, red, blue)"},
           {"conic-gradient(from 90deg, red, 50%, blue)", "conic-gradient(from 90deg, red, 50%, blue)"}, {"linear-gradient(red, 40%, blue)", "linear-gradient(red, 40%, blue)"}}) {
    One o;
    run_into(o, "<gradient>", s.in);
    ASSERT_TRUE(o.m.matched) << s.in;
    GTEXT_CSS_Gradient * g = nullptr;
    ASSERT_EQ(grad_from(o.m.root(), 0, &g), GTEXT_CSS_OK);
    char * buf = nullptr;
    size_t len = 0;
    ASSERT_EQ(gtext_css_gradient_serialize_buffer(g, nullptr, &buf, &len), GTEXT_CSS_OK);
    EXPECT_EQ(std::string(buf, len), s.out) << s.in;
    // the omitted form means the same: it matches the same grammar
    EXPECT_NE(run_f("<gradient>", std::string(buf, len)), "no match") << s.in;
    gtext_allocator_free(gtext_allocator_default(), buf);
    gtext_css_gradient_free(g);
  }
}

// ---- transforms -----------------------------------------------------------------------------------------------------------

TEST(CssFunctionTransform, EveryTwoAndThreeDimensionalFunction) {
  rows({{"<transform-list>", "matrix(1,2,3,4,5,6)", "matrix(1, 2, 3, 4, 5, 6)"}, {"<transform-list>", "matrix(1,2,3,4,5)", "no match"},
      {"<transform-list>", "translate(10px)", "translate(10px)"}, {"<transform-list>", "translate(10px, 20%)", "translate(10px, 20%)"},
      {"<transform-list>", "translateX(10px) translateY(1em) translateZ(3pt)", "translateX(10px) translateY(1em) translateZ(3pt)"},
      {"<transform-list>", "translateZ(10%)", "no match"}, {"<transform-list>", "translate3d(1px, 2%, 3px)", "translate3d(1px, 2%, 3px)"},
      {"<transform-list>", "translate3d(1px, 2%, 3%)", "no match"}, {"<transform-list>", "scale(2)", "scale(2)"}, {"<transform-list>", "scale(2, 3)", "scale(2, 3)"},
      {"<transform-list>", "scale(50%, 3)", "scale(50%, 3)"}, {"<transform-list>", "scale(1, 2, 3)", "no match"}, {"<transform-list>", "scaleX(2) scaleY(3) scaleZ(4)", "scaleX(2) scaleY(3) scaleZ(4)"},
      {"<transform-list>", "scale3d(1, 2, 3)", "scale3d(1, 2, 3)"}, {"<transform-list>", "scale3d(1, 2)", "no match"}, {"<transform-list>", "rotate(45deg)", "rotate(45deg)"},
      {"<transform-list>", "rotate(0)", "rotate(0)"}, {"<transform-list>", "rotate(45)", "no match"}, {"<transform-list>", "rotateX(1turn) rotateY(2rad) rotateZ(3grad)", "rotateX(1turn) rotateY(2rad) rotateZ(3grad)"},
      {"<transform-list>", "rotate3d(1, 0, 0, 45deg)", "rotate3d(1, 0, 0, 45deg)"}, {"<transform-list>", "rotate3d(1, 0, 0)", "no match"}, {"<transform-list>", "skew(10deg)", "skew(10deg)"},
      {"<transform-list>", "skew(10deg, 20deg)", "skew(10deg, 20deg)"}, {"<transform-list>", "skewX(0) skewY(1deg)", "skewX(0) skewY(1deg)"},
      {"<transform-list>", "matrix3d(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1)", "matrix3d(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)"},
      {"<transform-list>", "matrix3d(1)", "no match"}, {"<transform-list>", "perspective(100px)", "perspective(100px)"}, {"<transform-list>", "perspective(none)", "perspective(none)"},
      {"<transform-list>", "perspective(-1px)", "no match"}, {"<transform-list>", "perspective(0)", "perspective(0px)"}, {"<transform-list>", "TRANSLATEX(1PX)", "translateX(1px)"},
      {"<transform-list>", "rotate(45deg) translate(1px) scale(2)", "rotate(45deg) translate(1px) scale(2)"}, {"<transform-list>", "", "no match"},
      {"<transform-list>", "foo(1)", "no match"}, {"none | <transform-list>", "none", "none"}});
}

TEST(CssFunctionTransform, MathFunctionsStandInArgumentsByType) {
  rows({{"<transform-list>", "translate(calc(1px + 2%))", "translate(calc(2% + 1px))"}, {"<transform-list>", "rotate(calc(1deg + 1turn))", "rotate(calc(361deg))"},
      {"<transform-list>", "scale(calc(1 + 1))", "scale(calc(2))"}, {"<transform-list>", "scale(calc(1px))", "no match"}, {"<transform-list>", "rotate(calc(1px))", "no match"},
      {"<transform-list>", "rotate(calc(10% + 1deg))", "no match"}, {"<transform-list>", "translateZ(calc(10% + 1px))", "no match"},
      {"<transform-list>", "scale(calc(100% / 4))", "scale(calc(25%))"}, {"<transform-list>", "scale(calc(sign(50%)))", "no match"}});
}

TEST(CssFunctionTransform, ArgumentsAreReadByName) {
  One o;
  run_into(o, "<transform-list>", "translate(10px, 20%) rotate(0.5turn) rotate(0) scale(50%, calc(1 + 1)) perspective(none) translateZ(calc(1px + 1em)) rotate3d(1, 0, 0, 0) skewX(1rad)");
  ASSERT_TRUE(o.m.matched);
  const GTEXT_CSS_Value * l = o.m.root();
  ASSERT_EQ(l->kind, GTEXT_CSS_VAL_LIST);
  ASSERT_EQ(l->child_count, 8u);
  GTEXT_CSS_Transform_Function f;
  ASSERT_TRUE(tf_from(&l->children[0], &f));
  EXPECT_EQ(f.op, GTEXT_CSS_TRANSFORM_TRANSLATE);
  ASSERT_EQ(f.arg_count, 2u);
  EXPECT_EQ(f.args[0].kind, GTEXT_CSS_TARG_LENGTH);
  EXPECT_EQ(f.args[0].unit, GTEXT_CSS_UNIT_PX);
  EXPECT_EQ(f.args[0].value, 10.0);
  EXPECT_EQ(f.args[1].kind, GTEXT_CSS_TARG_PERCENT);
  ASSERT_TRUE(tf_from(&l->children[1], &f));
  EXPECT_EQ(f.op, GTEXT_CSS_TRANSFORM_ROTATE);
  EXPECT_EQ(f.args[0].kind, GTEXT_CSS_TARG_ANGLE);
  EXPECT_DOUBLE_EQ(f.args[0].value, 180.0);  // degrees
  ASSERT_TRUE(tf_from(&l->children[2], &f));
  EXPECT_EQ(f.args[0].kind, GTEXT_CSS_TARG_ANGLE);  // the unitless zero of an angle slot
  EXPECT_EQ(f.args[0].value, 0.0);
  ASSERT_TRUE(tf_from(&l->children[3], &f));
  EXPECT_EQ(f.op, GTEXT_CSS_TRANSFORM_SCALE);
  EXPECT_EQ(f.args[0].kind, GTEXT_CSS_TARG_PERCENT);
  EXPECT_EQ(f.args[1].kind, GTEXT_CSS_TARG_NUMBER);
  EXPECT_EQ(f.args[1].value, 2.0);
  ASSERT_TRUE(tf_from(&l->children[4], &f));
  EXPECT_EQ(f.op, GTEXT_CSS_TRANSFORM_PERSPECTIVE);
  EXPECT_EQ(f.args[0].kind, GTEXT_CSS_TARG_NONE);
  ASSERT_TRUE(tf_from(&l->children[5], &f));
  EXPECT_EQ(f.op, GTEXT_CSS_TRANSFORM_TRANSLATE_Z);
  EXPECT_EQ(f.args[0].kind, GTEXT_CSS_TARG_MATH);
  EXPECT_NE(f.args[0].source->math, nullptr);
  ASSERT_TRUE(tf_from(&l->children[6], &f));
  EXPECT_EQ(f.op, GTEXT_CSS_TRANSFORM_ROTATE_3D);
  ASSERT_EQ(f.arg_count, 4u);
  EXPECT_EQ(f.args[0].kind, GTEXT_CSS_TARG_NUMBER);
  EXPECT_EQ(f.args[3].kind, GTEXT_CSS_TARG_ANGLE);
  ASSERT_TRUE(tf_from(&l->children[7], &f));
  EXPECT_EQ(f.op, GTEXT_CSS_TRANSFORM_SKEW_X);
  EXPECT_NEAR(f.args[0].value, 57.29577951308232, 1e-12);
  EXPECT_STREQ(gtext_css_transform_op_name(GTEXT_CSS_TRANSFORM_TRANSLATE_3D), "translate3d");
  EXPECT_STREQ(gtext_css_transform_op_name(GTEXT_CSS_TRANSFORM_SCALE_X), "scaleX");
  EXPECT_EQ(gtext_css_transform_op_name(GTEXT_CSS_TRANSFORM_COUNT), nullptr);
  EXPECT_FALSE(tf_from(l, &f));
  EXPECT_FALSE(tf_from(nullptr, &f));
}

// ---- easing ----------------------------------------------------------------------------------------------------------------------

namespace {

struct Easing {
  One o;
  GTEXT_CSS_Easing * e = nullptr;
  bool valid = false;
  GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;
  explicit Easing(const std::string & input) {
    run_into(o, "<easing-function>", input);
    if (o.m.matched) {
      status = gtext_css_easing_from_value(o.m.root(), nullptr, &valid, &e);
    }
  }
  ~Easing() {
    gtext_css_easing_free(e);
  }
  std::string text() const {
    if (!e) {
      return o.m.matched ? "invalid" : "no match";
    }
    char * buf = nullptr;
    size_t len = 0;
    EXPECT_EQ(gtext_css_easing_serialize_buffer(e, nullptr, &buf, &len), GTEXT_CSS_OK);
    std::string s(buf, len);
    gtext_allocator_free(gtext_allocator_default(), buf);
    return s;
  }
};

}  // namespace

TEST(CssFunctionEasing, TheKeywordsAreSerializedAsThemselves) {
  // Easing 1, 2.4: "The keyword values ease, linear, ease-in, ease-out, and ease-in-out are serialized as-is"
  for (const char * k : {"linear", "ease", "ease-in", "ease-out", "ease-in-out"}) {
    Easing e(k);
    EXPECT_EQ(e.text(), k);
  }
  Easing ease("ease");
  EXPECT_EQ(ease.e->kind, GTEXT_CSS_EASING_CUBIC_BEZIER);
  EXPECT_EQ(ease.e->keyword, GTEXT_CSS_EASING_KW_EASE);
  EXPECT_DOUBLE_EQ(ease.e->x1, 0.25);
  EXPECT_DOUBLE_EQ(ease.e->y1, 0.1);
  EXPECT_DOUBLE_EQ(ease.e->x2, 0.25);
  EXPECT_DOUBLE_EQ(ease.e->y2, 1.0);
  Easing in_out("EASE-IN-OUT");
  EXPECT_DOUBLE_EQ(in_out.e->x1, 0.42);
  EXPECT_DOUBLE_EQ(in_out.e->x2, 0.58);
  Easing lin("linear");
  EXPECT_EQ(lin.e->kind, GTEXT_CSS_EASING_LINEAR);
}

TEST(CssFunctionEasing, StepsAreSerializedWithoutAJumpEndAndAreCheckedForTheirRange) {
  // Easing 1, 2.4: "If the step position is jump-end or end, serialize as steps(<integer>). Otherwise, serialize as steps(<integer>, <step-position>)"
  struct S {
    const char * in;
    const char * out;
  };
  for (const S & s : std::vector<S>{{"step-start", "steps(1, start)"}, {"step-end", "steps(1)"}, {"steps(4)", "steps(4)"}, {"steps(4, end)", "steps(4)"},
           {"steps(4, jump-end)", "steps(4)"}, {"steps(4, start)", "steps(4, start)"}, {"steps(4, jump-start)", "steps(4, jump-start)"},
           {"steps(2, jump-none)", "steps(2, jump-none)"}, {"steps(1, jump-both)", "steps(1, jump-both)"}, {"steps(calc(2 + 2), jump-both)", "steps(4, jump-both)"}}) {
    Easing e(s.in);
    EXPECT_EQ(e.text(), s.out) << s.in;
    EXPECT_TRUE(e.valid) << s.in;
  }
  // "The first parameter must be a positive integer (meaning that 0 is invalid), unless the second parameter is jump-none, in which case it must be a positive integer greater than 1."
  for (const char * bad : {"steps(0)", "steps(0, jump-start)", "steps(1, jump-none)", "steps(-1)", "steps(calc(0), end)"}) {
    Easing e(bad);
    EXPECT_EQ(e.status, GTEXT_CSS_OK) << bad;
    EXPECT_FALSE(e.valid) << bad;
    EXPECT_EQ(e.e, nullptr) << bad;
  }
  for (const char * bad : {"steps(1.5)", "steps()", "steps(1, 2)", "steps(1, nope)"}) {
    EXPECT_EQ(run_f("<easing-function>", bad), "no match") << bad;
  }
  Easing st("step-start");
  EXPECT_EQ(st.e->kind, GTEXT_CSS_EASING_STEPS);
  EXPECT_EQ(st.e->keyword, GTEXT_CSS_EASING_KW_STEP_START);
  EXPECT_EQ(st.e->steps, 1);
  EXPECT_EQ(st.e->position, GTEXT_CSS_STEP_JUMP_START);
}

TEST(CssFunctionEasing, TheXValuesOfACubicBezierAreInZeroToOne) {
  Easing ok("cubic-bezier(0.1, 0.7, 1, 0.1)");
  EXPECT_EQ(ok.text(), "cubic-bezier(0.1, 0.7, 1, 0.1)");
  Easing y_out("cubic-bezier(0, -5, 1, 7)");
  EXPECT_EQ(y_out.text(), "cubic-bezier(0, -5, 1, 7)");
  EXPECT_EQ(run_f("<easing-function>", "cubic-bezier(1.1, 0, 0, 0)"), "no match");
  EXPECT_EQ(run_f("<easing-function>", "cubic-bezier(0, 0, -0.1, 0)"), "no match");
  EXPECT_EQ(run_f("<easing-function>", "cubic-bezier(0, 0, 0)"), "no match");
  Easing m("cubic-bezier(calc(0.1 + 0.2), 0, 0.5, calc(2))");
  EXPECT_EQ(m.text(), "cubic-bezier(0.3, 0, 0.5, 2)");
  Easing m_out("cubic-bezier(calc(0.5 * 3), 0, 0.5, 2)");  // a calculation outside the range is as invalid as a literal
  EXPECT_FALSE(m_out.valid);
  EXPECT_EQ(run_f("<easing-function>", "cubic-bezier(1.1, 0, 0, 0)").substr(0, 2), "no");
}

TEST(CssFunctionEasing, LinearControlPointsAreCanonicalized) {
  // Easing 2, 2.1: "If the first control point lacks an input progress value, set its input progress value to 0. If the last ... 1. If any control point
  // has an input progress value that is less than the input progress value of any preceding control point, set its input progress value to the largest
  // input progress value of any preceding control point. If any control point still lacks an input progress value, then for each contiguous run ... evenly spaced"
  struct P {
    double input, output;
    bool written;
  };
  struct C {
    const char * in;
    const char * out;
    std::vector<P> points;
  };
  for (const C & c : std::vector<C>{
           {"linear(0, 1)", "linear(0, 1)", {{0, 0, false}, {1, 1, false}}},
           {"linear(0, .25, 1)", "linear(0, 0.25, 1)", {{0, 0, false}, {0.5, 0.25, false}, {1, 1, false}}},
           {"linear(0, .1 25%, .75 50%, 1)", "linear(0, 0.1 25%, 0.75 50%, 1)", {{0, 0, false}, {0.25, 0.1, true}, {0.5, 0.75, true}, {1, 1, false}}},
           // the draft's three examples of the serialization
           {"linear(0 20%, 0.5 10%, 1)", "linear(0 20%, 0.5 20%, 1)", {{0.2, 0, true}, {0.2, 0.5, true}, {1, 1, false}}},
           {"linear(0, 0.25 25% 75%, 1)", "linear(0, 0.25 25%, 0.25 75%, 1)", {{0, 0, false}, {0.25, 0.25, true}, {0.75, 0.25, true}, {1, 1, false}}},
           {"linear(0, 0.25, 0.5, 0.75, 1)", "linear(0, 0.25, 0.5, 0.75, 1)", {{0, 0, false}, {0.25, 0.25, false}, {0.5, 0.5, false}, {0.75, 0.75, false}, {1, 1, false}}},
           {"linear(0 50%, 0.5, 1 20%)", "linear(0 50%, 0.5, 1 50%)", {{0.5, 0, true}, {0.5, 0.5, false}, {0.5, 1, true}}},
           {"linear(1)", "linear(1)", {{0, 1, false}}}}) {
    Easing e(c.in);
    ASSERT_TRUE(e.valid) << c.in;
    EXPECT_EQ(e.text(), c.out) << c.in;
    ASSERT_EQ(e.e->point_count, c.points.size()) << c.in;
    for (size_t i = 0; i < c.points.size(); i++) {
      EXPECT_DOUBLE_EQ(e.e->points[i].input, c.points[i].input) << c.in << " " << i;
      EXPECT_DOUBLE_EQ(e.e->points[i].output, c.points[i].output) << c.in << " " << i;
      EXPECT_EQ(e.e->points[i].input_written, c.points[i].written) << c.in << " " << i;
    }
    // non-decreasing inputs after canonicalization
    for (size_t i = 1; i < e.e->point_count; i++) {
      EXPECT_GE(e.e->points[i].input, e.e->points[i - 1].input) << c.in;
    }
  }
  for (const char * bad : {"linear()", "linear(0, 1 2 3 4)", "linear(10%)", "linear(0 1)", "linear(0 10% 20% 30%)"}) {
    EXPECT_EQ(run_f("<easing-function>", bad), "no match") << bad;
  }
}

TEST(CssFunctionEasing, OnlyABadCallIsAnErrorAndAValueThatIsNoEasingFunctionIsOkAndNotValid) {
  // the one rule of every typed view: E_INVALID for a NULL argument; any value that is not valid is OK with `valid` false and `*out` NULL
  One o;
  run_into(o, "<string>", "\"x\"");
  GTEXT_CSS_Easing * e = reinterpret_cast<GTEXT_CSS_Easing *>(&o);  // junk: it must be overwritten
  bool valid = true;
  EXPECT_EQ(gtext_css_easing_from_value(o.m.root(), nullptr, &valid, &e), GTEXT_CSS_OK);
  EXPECT_FALSE(valid);
  EXPECT_EQ(e, nullptr);
  EXPECT_EQ(gtext_css_easing_from_value(nullptr, nullptr, &valid, &e), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_easing_from_value(o.m.root(), nullptr, nullptr, &e), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_easing_from_value(o.m.root(), nullptr, &valid, nullptr), GTEXT_CSS_E_INVALID);
  One u;
  run_into(u, "<ident>", "wobble");
  e = reinterpret_cast<GTEXT_CSS_Easing *>(&o);
  valid = true;
  EXPECT_EQ(gtext_css_easing_from_value(u.m.root(), nullptr, &valid, &e), GTEXT_CSS_OK);
  EXPECT_FALSE(valid);
  EXPECT_EQ(e, nullptr);
  // a known function with a bad argument and an unknown function are the same answer
  for (const char * text : {"steps(0)", "steps(1, jump-none)", "cubic-bezier(calc(2), 0, 1, 1)"}) {
    One f;
    run_into(f, "<easing-function>", text);
    ASSERT_TRUE(f.m.matched) << text;
    e = reinterpret_cast<GTEXT_CSS_Easing *>(&o);
    valid = true;
    EXPECT_EQ(gtext_css_easing_from_value(f.m.root(), nullptr, &valid, &e), GTEXT_CSS_OK) << text;
    EXPECT_EQ(e, nullptr) << text;
  }
  char * buf = nullptr;
  size_t len = 0;
  EXPECT_EQ(gtext_css_easing_serialize_buffer(nullptr, nullptr, &buf, &len), GTEXT_CSS_E_INVALID);
  gtext_css_easing_free(nullptr);
}

// ---- shapes ------------------------------------------------------------------------------------------------------------------------

TEST(CssFunctionShape, EveryBasicShape) {
  rows({{"<basic-shape>", "inset(10px)", "inset(10px)"}, {"<basic-shape>", "inset(10px 20px 30px 40px round 5px)", "inset(10px 20px 30px 40px round 5px)"},
      {"<basic-shape>", "inset(1px round 1px 2px / 3px 4px)", "inset(1px round 1px 2px / 3px 4px)"}, {"<basic-shape>", "inset(1px 2px 3px 4px 5px)", "no match"},
      {"<basic-shape>", "inset(1px round -1px)", "no match"}, {"<basic-shape>", "xywh(1px 2px 3px 4px)", "xywh(1px 2px 3px 4px)"},
      {"<basic-shape>", "xywh(1px 2px -3px 4px)", "no match"}, {"<basic-shape>", "xywh(1px 2px 3px 4px round 1px)", "xywh(1px 2px 3px 4px round 1px)"},
      {"<basic-shape>", "rect(1px auto 3px auto)", "rect(1px auto 3px auto)"}, {"<basic-shape>", "rect(1px 2px 3px)", "no match"},
      {"<basic-shape>", "circle()", "circle()"}, {"<basic-shape>", "circle(50%)", "circle(50%)"}, {"<basic-shape>", "circle(closest-side at 10px 20px)", "circle(closest-side at 10px 20px)"},
      {"<basic-shape>", "circle(at bottom left)", "circle(at left bottom)"}, {"<basic-shape>", "circle(-1px)", "no match"}, {"<basic-shape>", "ellipse(10px 20px at center)", "ellipse(10px 20px at center)"},
      {"<basic-shape>", "ellipse(farthest-corner closest-side)", "ellipse(farthest-corner closest-side)"},
      {"<basic-shape>", "polygon(0 0, 10px 10px, 5% 5%)", "polygon(0px 0px, 10px 10px, 5% 5%)"}, {"<basic-shape>", "polygon(evenodd, 0 0, 10px 10px)", "polygon(evenodd, 0px 0px, 10px 10px)"},
      {"<basic-shape>", "polygon(nonzero round 5px, 0 0)", "polygon(nonzero round 5px, 0px 0px)"}, {"<basic-shape>", "polygon(round 5px, 0 0)", "polygon(round 5px, 0px 0px)"},
      {"<basic-shape>", "polygon()", "no match"}, {"<basic-shape>", "polygon(evenodd)", "no match"}, {"<basic-shape>", "path(\"M0 0\")", "path(\"M0 0\")"},
      {"<basic-shape>", "path(evenodd, \"M0 0 L1 1\")", "path(evenodd, \"M0 0 L1 1\")"}, {"<basic-shape>", "path(0)", "no match"},
      {"<basic-shape>", "shape(from 10px 10px, line to 20px 20px, close)", "shape(from 10px 10px, line to 20px 20px, close)"},
      {"<basic-shape>", "shape(evenodd from 0 0, move by 1px 2px, hline to 5px, vline by 1px, curve to 1px 2px with 3px 4px / 5px 6px, smooth by 1px 2px, arc to 1px 1px of 2px cw large rotate 5deg, close)",
       "shape(evenodd from 0px 0px, move by 1px 2px, hline to 5px, vline by 1px, curve to 1px 2px with 3px 4px / 5px 6px, smooth by 1px 2px, arc to 1px 1px of 2px cw large rotate 5deg, close)"},
      {"<basic-shape>", "shape(from 0 0)", "no match"}, {"<basic-shape>", "foo()", "no match"}});
}

TEST(CssFunctionShape, GeometryBoxes) {
  rows({{"<shape-box>", "margin-box", "margin-box"}, {"<shape-box>", "content-box", "content-box"}, {"<shape-box>", "half-border-box", "half-border-box"},
      {"<geometry-box>", "fill-box", "fill-box"}, {"<geometry-box>", "stroke-box", "stroke-box"}, {"<geometry-box>", "view-box", "view-box"}, {"<geometry-box>", "border-box", "border-box"},
      {"<geometry-box>", "nope-box", "no match"}, {"<basic-shape> || <shape-box>", "margin-box circle()", "circle() margin-box"}});
}

TEST(CssFunctionShape, TheKindOfAShapeAndItsFillRule) {
  struct S {
    const char * in;
    GTEXT_CSS_Shape_Kind kind;
    bool even_odd, written, round;
  };
  for (const S & s : std::vector<S>{{"inset(1px round 2px)", GTEXT_CSS_SHAPE_INSET, false, false, true}, {"xywh(1px 2px 3px 4px)", GTEXT_CSS_SHAPE_XYWH, false, false, false},
           {"rect(1px 2px 3px 4px)", GTEXT_CSS_SHAPE_RECT, false, false, false}, {"circle(1px)", GTEXT_CSS_SHAPE_CIRCLE, false, false, false},
           {"ellipse()", GTEXT_CSS_SHAPE_ELLIPSE, false, false, false}, {"polygon(evenodd, 0 0)", GTEXT_CSS_SHAPE_POLYGON, true, true, false},
           {"polygon(nonzero round 1px, 0 0)", GTEXT_CSS_SHAPE_POLYGON, false, true, true}, {"path(evenodd, \"M0 0\")", GTEXT_CSS_SHAPE_PATH, true, true, false},
           {"shape(from 0 0, close)", GTEXT_CSS_SHAPE_SHAPE, false, false, false}}) {
    One o;
    run_into(o, "<basic-shape>", s.in);
    ASSERT_TRUE(o.m.matched) << s.in;
    GTEXT_CSS_Shape sh;
    ASSERT_TRUE(shape_from(o.m.root(), &sh)) << s.in;
    EXPECT_EQ(sh.kind, s.kind) << s.in;
    EXPECT_EQ(sh.even_odd, s.even_odd) << s.in;
    EXPECT_EQ(sh.fill_rule_written, s.written) << s.in;
    EXPECT_EQ(sh.has_round, s.round) << s.in;
  }
  One p;
  run_into(p, "<basic-shape>", "path(\"M0 0 L1 1\")");
  GTEXT_CSS_Shape sh;
  ASSERT_TRUE(shape_from(p.m.root(), &sh));
  ASSERT_NE(sh.path, nullptr);
  EXPECT_EQ(css_test::str(sh.path->text), "M0 0 L1 1");
  One u;
  run_into(u, "<ident>", "x");
  EXPECT_FALSE(shape_from(u.m.root(), &sh));
  EXPECT_FALSE(shape_from(nullptr, &sh));
}

// ---- substitution and generated content --------------------------------------------------------------------------------------

TEST(CssFunctionSubstitution, TheNamedFunctionsAreTypes) {
  rows({{"<counter()>", "counter(chapter)", "counter(chapter)"}, {"<counter()>", "counter(chapter, upper-roman)", "counter(chapter, upper-roman)"},
      {"<counter()>", "counter(chapter, symbols(cyclic \"*\" \"†\"))", "counter(chapter, symbols(cyclic \"*\" \"†\"))"}, {"<counter()>", "counter()", "no match"},
      {"<counters()>", "counters(c, \".\")", "counters(c, \".\")"}, {"<counters()>", "counters(c, \".\", decimal)", "counters(c, \".\", decimal)"}, {"<counters()>", "counters(c)", "no match"},
      {"<calc-size()>", "calc-size(auto, size)", "calc-size(auto, size)"}, {"<calc-size()>", "calc-size(min-content, size + 10px)", "calc-size(min-content, size + 10px)"},
      {"<sibling-index()>", "sibling-index()", "sibling-index()"}, {"<sibling-count()>", "sibling-count()", "sibling-count()"}, {"<sibling-index()>", "sibling-index(1)", "no match"},
      {"<first-valid()>", "first-valid(1px, nonsense words, 2px)", "first-valid(1px, nonsense words, 2px)"}, {"<first-valid()>", "first-valid()", "no match"},
      {"<random()>", "random(1px, 2px)", "random(1px, 2px)"}, {"<random()>", "random(--k element-scoped, 1px, 2px, 3px)", "random(--k element-scoped, 1px, 2px, 3px)"},
      {"<progress()>", "progress(1px, 0px, 2px)", "progress(1px, 0px, 2px)"}, {"<progress()>", "progress(no-clamp 1px, 0px, 2px)", "progress(no-clamp 1px, 0px, 2px)"},
      {"<string()>", "string(chapter)", "string(chapter)"}, {"<string()>", "string(chapter, first-except)", "string(chapter, first-except)"}, {"<string()>", "string(chapter, nope)", "no match"},
      {"<var()>", "var(--a)", "var(--a)"}, {"<env()>", "env(safe-area-inset-top)", "env(safe-area-inset-top)"}, {"<attr()>", "attr(data-x type(<length>), 1px)", "attr(data-x type(<length>), 1px)"}});
}

TEST(CssFunctionSubstitution, EachNeedsSomethingBeforeItHasAValue) {
  struct N {
    const char * name;
    GTEXT_CSS_Substitution_Kind kind;
    GTEXT_CSS_Needs needs;
  };
  for (const N & n : std::vector<N>{{"var", GTEXT_CSS_SUBST_VAR, GTEXT_CSS_NEEDS_VARIABLES}, {"ENV", GTEXT_CSS_SUBST_ENV, GTEXT_CSS_NEEDS_ENVIRONMENT},
           {"attr", GTEXT_CSS_SUBST_ATTR, GTEXT_CSS_NEEDS_ELEMENT}, {"if", GTEXT_CSS_SUBST_IF, GTEXT_CSS_NEEDS_ELEMENT},
           {"first-valid", GTEXT_CSS_SUBST_FIRST_VALID, GTEXT_CSS_NEEDS_PROPERTY}, {"random", GTEXT_CSS_SUBST_RANDOM, GTEXT_CSS_NEEDS_ELEMENT},
           {"random-item", GTEXT_CSS_SUBST_RANDOM_ITEM, GTEXT_CSS_NEEDS_ELEMENT}, {"sibling-index", GTEXT_CSS_SUBST_SIBLING_INDEX, GTEXT_CSS_NEEDS_ELEMENT},
           {"sibling-count", GTEXT_CSS_SUBST_SIBLING_COUNT, GTEXT_CSS_NEEDS_ELEMENT}, {"progress", GTEXT_CSS_SUBST_PROGRESS, GTEXT_CSS_NEEDS_NOTHING},
           {"inherit", GTEXT_CSS_SUBST_INHERIT, GTEXT_CSS_NEEDS_ELEMENT}, {"ident", GTEXT_CSS_SUBST_IDENT, GTEXT_CSS_NEEDS_NOTHING},
           {"string", GTEXT_CSS_SUBST_STRING, GTEXT_CSS_NEEDS_ELEMENT}, {"counter", GTEXT_CSS_SUBST_COUNTER, GTEXT_CSS_NEEDS_ELEMENT},
           {"counters", GTEXT_CSS_SUBST_COUNTERS, GTEXT_CSS_NEEDS_ELEMENT}, {"calc-size", GTEXT_CSS_SUBST_CALC_SIZE, GTEXT_CSS_NEEDS_LAYOUT}}) {
    EXPECT_EQ(gtext_css_substitution_kind(n.name, std::strlen(n.name)), n.kind) << n.name;
    EXPECT_EQ(gtext_css_substitution_needs(n.kind), n.needs) << n.name;
  }
  EXPECT_EQ(gtext_css_substitution_kind("calc", 4), GTEXT_CSS_SUBST_NONE);
  EXPECT_EQ(gtext_css_substitution_kind(nullptr, 0), GTEXT_CSS_SUBST_NONE);
  EXPECT_EQ(gtext_css_substitution_needs(GTEXT_CSS_SUBST_NONE), GTEXT_CSS_NEEDS_NOTHING);
}

TEST(CssFunctionFamily, EachNameBelongsToOneDraft) {
  struct F {
    const char * name;
    GTEXT_CSS_Function_Family family;
  };
  for (const F & f : std::vector<F>{{"calc", GTEXT_CSS_FN_FAMILY_MATH}, {"PROGRESS", GTEXT_CSS_FN_FAMILY_MATH}, {"random", GTEXT_CSS_FN_FAMILY_MATH}, {"rgb", GTEXT_CSS_FN_FAMILY_COLOR},
           {"color-mix", GTEXT_CSS_FN_FAMILY_COLOR}, {"url", GTEXT_CSS_FN_FAMILY_IMAGE}, {"image-set", GTEXT_CSS_FN_FAMILY_IMAGE}, {"linear-gradient", GTEXT_CSS_FN_FAMILY_GRADIENT},
           {"repeating-conic-gradient", GTEXT_CSS_FN_FAMILY_GRADIENT}, {"translateX", GTEXT_CSS_FN_FAMILY_TRANSFORM}, {"matrix3d", GTEXT_CSS_FN_FAMILY_TRANSFORM},
           {"steps", GTEXT_CSS_FN_FAMILY_EASING}, {"cubic-bezier", GTEXT_CSS_FN_FAMILY_EASING}, {"polygon", GTEXT_CSS_FN_FAMILY_SHAPE}, {"var", GTEXT_CSS_FN_FAMILY_SUBSTITUTION},
           {"counter", GTEXT_CSS_FN_FAMILY_SUBSTITUTION}, {"foo", GTEXT_CSS_FN_FAMILY_NONE}}) {
    EXPECT_EQ(gtext_css_function_family(f.name, std::strlen(f.name)), f.family) << f.name;
  }
  EXPECT_EQ(gtext_css_function_family(nullptr, 0), GTEXT_CSS_FN_FAMILY_NONE);
}

// ---- engine types: percentages, colors, and math inside the families --------------------------------------------------------------

TEST(CssFunctionEngine, LengthPercentageAndTheOtherMixedTypesAreNative) {
  rows({{"<length-percentage>", "10px", "10px"}, {"<length-percentage>", "10%", "10%"}, {"<length-percentage>", "0", "0px"}, {"<length-percentage>", "10", "no match"},
      {"<length-percentage>", "calc(10% + 5px)", "calc(10% + 5px)"}, {"<length-percentage>", "calc(10%)", "calc(10%)"}, {"<length-percentage>", "calc(10deg)", "no match"},
      {"<length>", "calc(10% + 5px)", "no match"}, {"<length>", "calc(10px)", "calc(10px)"}, {"<angle-percentage>", "calc(10% + 5deg)", "calc(10% + 5deg)"},
      {"<angle-percentage>", "10%", "10%"}, {"<angle-percentage>", "0", "no match"}, {"<time-percentage>", "50%", "50%"}, {"<time-percentage>", "calc(1s + 50%)", "calc(50% + 1s)"},
      {"<frequency-percentage>", "5hz", "5Hz"}, {"<length-percentage [0,100px]>", "50px", "50px"}, {"<length-percentage [0,100px]>", "150px", "no match"},
      {"<length-percentage [0,100px]>", "150%", "150%"}, {"<length-percentage [0%,50%]>", "60%", "no match"}, {"<length-percentage [0%,50%]>", "150px", "150px"},
      {"<length-percentage [0,∞]>", "-1px", "no match"}, {"<length-percentage [0,∞]>", "-1%", "no match"}, {"<length-percentage [0,∞]>", "calc(-1px)", "calc(-1px)"}});
}

TEST(CssFunctionEngine, ColorIsANativeTypeAndMathFunctionsInNumbersCarryThePercentRule) {
  rows({{"<color>", "red", "red"}, {"<color>", "#abc", "rgb(170, 187, 204)"}, {"<color> <length>", "hsl(120 100% 50%) 1px", "rgb(0, 255, 0) 1px"}, {"<color>#", "red, blue", "red, blue"},
      {"<color>", "red blue", "no match"}, {"<color>", "calc(1)", "no match"}, {"<number>", "calc(10% / 20%)", "no match"}, {"<integer>", "calc(1 + 1)", "calc(2)"},
      {"<number>", "calc(sibling-index())", "sibling-index()"}, {"<number>", "sibling-index()", "sibling-index()"}, {"<length>", "sibling-index()", "no match"},
      {"<number>", "progress(10px, 0px, 20px)", "calc(0.5)"}});
  One o;
  run_into(o, "<color>", "color-mix(in srgb, red, blue)");
  ASSERT_TRUE(o.m.matched);
  EXPECT_EQ(o.m.root()->kind, GTEXT_CSS_VAL_COLOR);
  ASSERT_NE(o.m.root()->color, nullptr);
  EXPECT_EQ(o.m.root()->color->kind, GTEXT_CSS_COLOR_MIX);
  EXPECT_EQ(o.m.root()->interp, GTEXT_CSS_INTERP_COLOR);
  EXPECT_STREQ(gtext_css_val_kind_name(GTEXT_CSS_VAL_COLOR), "color");
}

TEST(CssFunctionEngine, ALimitOnTheNestingOfColorsAndOfMathIsItsOwnStatusInAMatch) {
  GTEXT_CSS_Match_Options mo = gtext_css_match_options_default();
  mo.max_color_nesting = 3;
  One ok, over;
  run_into(ok, "<color>", "light-dark(light-dark(red, blue), blue)", &mo);
  EXPECT_EQ(ok.m.status, GTEXT_CSS_OK);
  EXPECT_TRUE(ok.m.matched);
  run_into(over, "<color>", "light-dark(light-dark(light-dark(red, blue), blue), blue)", &mo);
  EXPECT_EQ(over.m.status, GTEXT_CSS_E_LIMIT_COLOR_NESTING);
  GTEXT_CSS_Match_Options mm = gtext_css_match_options_default();
  mm.max_math_depth = 3;
  One m_ok, m_over;
  run_into(m_ok, "<length>", "calc(calc(calc(1px)))", &mm);
  EXPECT_TRUE(m_ok.m.matched);
  run_into(m_over, "<length>", "calc(calc(calc(calc(1px))))", &mm);
  EXPECT_EQ(m_over.m.status, GTEXT_CSS_E_LIMIT_DEPTH);
  GTEXT_CSS_Match_Options mn = gtext_css_match_options_default();
  mn.max_math_nodes = 4;
  One n_over;
  run_into(n_over, "<length>", "calc(1em + 2em * 3 + 4px)", &mn);
  EXPECT_EQ(n_over.m.status, GTEXT_CSS_E_LIMIT_MATH_NODES);
}

TEST(CssFunctionEngine, EveryLimitOfAMatchIsExactAtItsBoundAndOnePast) {
  // function nodes: three functions packed
  const std::string transforms = "translate(1px) translate(2px) translate(3px)";
  GTEXT_CSS_Match_Options mo = gtext_css_match_options_default();
  mo.max_function_nodes = 3;
  One at, over;
  run_into(at, "<transform-list>", transforms, &mo);
  EXPECT_EQ(at.m.status, GTEXT_CSS_OK);
  EXPECT_TRUE(at.m.matched);
  mo.max_function_nodes = 2;
  run_into(over, "<transform-list>", transforms, &mo);
  EXPECT_EQ(over.m.status, GTEXT_CSS_E_LIMIT_FUNCTION_NODES);
  EXPECT_FALSE(over.m.matched);
  // functions inside functions count too: a gradient holds its color functions as colors (not function nodes), an image-set holds an image
  mo.max_function_nodes = 2;
  One nested_at, nested_over;
  run_into(nested_at, "<image>", "image-set(linear-gradient(red, blue) 1x)", &mo);
  EXPECT_EQ(nested_at.m.status, GTEXT_CSS_OK);
  mo.max_function_nodes = 1;
  run_into(nested_over, "<image>", "image-set(linear-gradient(red, blue) 1x)", &mo);
  EXPECT_EQ(nested_over.m.status, GTEXT_CSS_E_LIMIT_FUNCTION_NODES);
  // a bound of 0 is the default
  mo.max_function_nodes = 0;
  One dflt;
  run_into(dflt, "<transform-list>", transforms, &mo);
  EXPECT_TRUE(dflt.m.matched);

  // math nodes: the nodes the calculation made are accepted, one fewer is the limit
  const std::string calc = "calc(1em + 2em * 3 + 4px)";
  GTEXT_CSS_Match_Options mn = gtext_css_match_options_default();
  size_t nodes = 0;
  for (size_t n = 1; n < 100; n++) {
    mn.max_math_nodes = n;
    One probe;
    run_into(probe, "<length>", calc, &mn);
    if (probe.m.status == GTEXT_CSS_OK) {
      nodes = n;
      break;
    }
    EXPECT_EQ(probe.m.status, GTEXT_CSS_E_LIMIT_MATH_NODES) << n;
  }
  ASSERT_GT(nodes, 1u);
  mn.max_math_nodes = nodes - 1;
  One mn_over;
  run_into(mn_over, "<length>", calc, &mn);
  EXPECT_EQ(mn_over.m.status, GTEXT_CSS_E_LIMIT_MATH_NODES);
  // the same bound reached through a color's channel, and the color options' own bounds
  const std::string chan = "rgb(calc(1 + 2 * 3) 0 0)";
  GTEXT_CSS_Color_Options co = gtext_css_color_options_default();
  size_t cn = 0;
  for (size_t n = 1; n < 100; n++) {
    co.max_math_nodes = n;
    gc::Color probe;
    gc::read(probe, chan, &co);
    if (probe.status == GTEXT_CSS_OK) {
      cn = n;
      break;
    }
    EXPECT_EQ(probe.status, GTEXT_CSS_E_LIMIT_MATH_NODES) << n;
  }
  ASSERT_GT(cn, 0u);
  if (cn > 1) {
    co.max_math_nodes = cn - 1;
    gc::Color over_c;
    gc::read(over_c, chan, &co);
    EXPECT_EQ(over_c.status, GTEXT_CSS_E_LIMIT_MATH_NODES);
  }
  co = gtext_css_color_options_default();
  co.max_math_depth = 3;
  gc::Color d_at, d_over;
  gc::read(d_at, "rgb(calc(calc(1)) 0 0)", &co);
  EXPECT_EQ(d_at.status, GTEXT_CSS_OK);
  gc::read(d_over, "rgb(calc(calc(calc(calc(1)))) 0 0)", &co);
  EXPECT_EQ(d_over.status, GTEXT_CSS_E_LIMIT_DEPTH);
  // the match's color nesting: the smallest bound that accepts a nested color, and one fewer
  const std::string nested = "light-dark(light-dark(red, blue), blue)";
  GTEXT_CSS_Match_Options mc = gtext_css_match_options_default();
  size_t nest = 0;
  for (size_t n = 1; n < 20; n++) {
    mc.max_color_nesting = n;
    One probe;
    run_into(probe, "<color>", nested, &mc);
    if (probe.m.status == GTEXT_CSS_OK) {
      nest = n;
      break;
    }
    EXPECT_EQ(probe.m.status, GTEXT_CSS_E_LIMIT_COLOR_NESTING) << n;
  }
  ASSERT_GT(nest, 1u);
  mc.max_color_nesting = nest - 1;
  One c_over;
  run_into(c_over, "<color>", nested, &mc);
  EXPECT_EQ(c_over.m.status, GTEXT_CSS_E_LIMIT_COLOR_NESTING);
}

TEST(CssFunctionEngine, AMatchThatRunsOutOfMemoryLeaksNothing) {
  One base;
  const std::string grammar = "<color> [ <transform-list> | <length-percentage> ]";
  const std::string input = "color-mix(in srgb, red, rgb(1 2 3)) translate(calc(1px + 2em), 3%) ";
  run_into(base, grammar, input);
  ASSERT_TRUE(base.m.matched) << base.text();
  const std::string expect = base.text();
  bool done = false;
  for (size_t n = 1; n < 2000 && !done; n++) {
    FaultAllocator fa;
    fa.arm(n);
    GTEXT_CSS_Match_Options mo = gtext_css_match_options_default();
    mo.allocator = &fa.alloc;
    bool matched = false;
    GTEXT_CSS_Value_Set * set = nullptr;
    const GTEXT_CSS_Status st = gtext_css_match(base.g.c, base.m.parsed.tree->values, base.m.parsed.tree->value_count, &mo, &matched, &set, nullptr);
    if (st == GTEXT_CSS_OK) {
      EXPECT_TRUE(matched);
      EXPECT_EQ(serialize(gtext_css_value_set_root(set)), expect);
      gtext_css_value_set_free(set);
      done = true;
    }
    else {
      EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
      EXPECT_EQ(set, nullptr);
    }
    fa.disarm();
    EXPECT_TRUE(fa.live.empty()) << "leak at failure " << n;
  }
  EXPECT_TRUE(done);
}

TEST(CssFunctionEngine, TheFunctionTypesAreFreedOnAnAllocationThatFails) {
  for (size_t n = 1; n < 40; n++) {
    FaultAllocator fa;
    fa.arm(n);
    GTEXT_CSS_Function_Types_Options o;
    std::memset(&o, 0, sizeof o);
    o.allocator = &fa.alloc;
    GTEXT_CSS_Function_Types * t = nullptr;
    const GTEXT_CSS_Status st = gtext_css_function_types_new(&o, &t);
    if (st == GTEXT_CSS_OK) {
      gtext_css_function_types_resolve(t, GTEXT_CSS_REF_TYPE, "image", 5);
      gtext_css_function_types_free(t);
    }
    else {
      EXPECT_EQ(st, GTEXT_CSS_E_OOM);
      EXPECT_EQ(t, nullptr);
    }
    fa.disarm();
    EXPECT_TRUE(fa.live.empty()) << n;
  }
}
