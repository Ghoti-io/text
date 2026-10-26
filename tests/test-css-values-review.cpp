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
 * What a review of the value layer found, each as a test that fails without its fix (tools/css/plant-grammar.py restores each defect in a
 * copy of the library and requires this program to fail): the contract of the typed views (OK with `valid` false, never E_INVALID for
 * a value; `*out` always set), allocation failure in every view, the unit and kind of a transform argument, the stops of a gradient, the
 * limits that clear their results, the nodes a color made, which color the builder takes, the exact mod() and rem(), the legacy alpha,
 * the cost of serializing nested colors, the resolution of a long keyword, the syntax of progress(), the tree-counting functions, the
 * lazy definitions of the function types and the families of function names.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css/css_function.h>

#include "css_color_test_support.h"
#include "css_math_test_support.h"
#include "css_values_test_support.h"
#include "markup_test_support.h"

extern "C" {
bool css_math_apply(GTEXT_CSS_Math_Op op, GTEXT_CSS_Math_Round round, const double * a, size_t n, double * out);
double css_math_fmod_exact(double x, double y);
}

using namespace gv;

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
};

void run_into(One & o, const std::string & grammar, const std::string & input, const GTEXT_CSS_Match_Options * mo = nullptr) {
  o.g.status = gtext_css_compile(grammar.data(), grammar.size(), gtext_css_function_types_resolve, types().t, nullptr, &o.g.c, &o.g.result);
  ASSERT_EQ(o.g.status, GTEXT_CSS_OK) << grammar << " " << o.g.result.name;
  css_test::parse_into(o.m.parsed, gtext_css_parse_component_values, input);
  o.m.status = gtext_css_match(o.g.c, o.m.parsed.tree->values, o.m.parsed.tree->value_count, mo, &o.m.matched, &o.m.set, &o.m.result);
}

/// The value of `input` as `grammar` matches it; the test fails when it does not.
const GTEXT_CSS_Value * value_of(One & o, const std::string & grammar, const std::string & input) {
  run_into(o, grammar, input);
  EXPECT_EQ(o.m.status, GTEXT_CSS_OK) << input;
  EXPECT_TRUE(o.m.matched) << input;
  return o.m.root();
}

/// The matched value's first function, whether the root is it or a list of them.
const GTEXT_CSS_Value * first_fn(const GTEXT_CSS_Value * root) {
  return root && root->kind == GTEXT_CSS_VAL_LIST && root->child_count ? &root->children[0] : root;
}

}  // namespace

// ---- 2: every typed view under a failing allocation --------------------------------------------------------------

namespace {

/// Run `view(allocator)` with the nth allocation failing, for n = 1, 2 ... until it succeeds. `view` returns the status, and whether it made a
/// result (which `release` frees). A failure must be E_OOM with nothing made; after every run the allocator holds nothing.
template <typename View, typename Release>
void sweep(const char * what, View view, Release release) {
  bool succeeded = false;
  for (size_t n = 1; n < 400 && !succeeded; n++) {
    FaultAllocator fa;
    fa.arm(n);
    bool made = false;
    bool valid = true;
    const GTEXT_CSS_Status st = view(&fa.alloc, &valid, &made);
    if (st == GTEXT_CSS_OK) {
      EXPECT_TRUE(valid) << what;
      EXPECT_TRUE(made) << what;
      succeeded = !fa.failed;
      EXPECT_EQ(fa.failed, false) << what << ": an allocation failed and the view said OK, at " << n;
    }
    else {
      EXPECT_EQ(st, GTEXT_CSS_E_OOM) << what << " at " << n;
      EXPECT_FALSE(valid) << what << " at " << n;
      EXPECT_FALSE(made) << what << " at " << n;
    }
    release(&fa.alloc);
    fa.disarm();
    EXPECT_TRUE(fa.live.empty()) << what << ": a block is left at failure " << n;
    EXPECT_EQ(fa.bad_frees, 0u) << what;
  }
  EXPECT_TRUE(succeeded) << what << ": it never succeeded";
}

}  // namespace

TEST(CssViewsOom, AnEasingFunctionIsEOomWithNothingMadeAndNothingLeftBehind) {
  for (const char * text : {"linear(0, .5 25%, 1)", "linear(0 0% 10%, 1)", "cubic-bezier(.1, .2, .3, .4)", "steps(4, jump-start)", "ease-in-out", "step-start"}) {
    One o;
    const GTEXT_CSS_Value * v = value_of(o, "<easing-function>", text);
    ASSERT_NE(v, nullptr) << text;
    GTEXT_CSS_Easing * e = nullptr;
    sweep(text, [&](const GTEXT_Allocator * a, bool * valid, bool * made) {
      e = reinterpret_cast<GTEXT_CSS_Easing *>(&o);  // junk: a failure must overwrite it
      const GTEXT_CSS_Status st = gtext_css_easing_from_value(v, a, valid, &e);
      *made = e != nullptr;
      return st;
    }, [&](const GTEXT_Allocator *) { gtext_css_easing_free(e); e = nullptr; });
  }
}

TEST(CssViewsOom, AGradientIsEOomWithNothingMadeAndNothingLeftBehind) {
  for (const char * text : {"linear-gradient(red, blue)", "linear-gradient(to right, red 10%, 20%, blue 30% 40%)", "radial-gradient(circle at 10px 20px, red, blue)",
           "conic-gradient(from 90deg, red, blue)", "repeating-linear-gradient(red 0 10px, blue 10px 20px)"}) {
    One o;
    const GTEXT_CSS_Value * v = value_of(o, "<gradient>", text);
    ASSERT_NE(v, nullptr) << text;
    GTEXT_CSS_Gradient * g = nullptr;
    sweep(text, [&](const GTEXT_Allocator * a, bool * valid, bool * made) {
      g = reinterpret_cast<GTEXT_CSS_Gradient *>(&o);
      const GTEXT_CSS_Status st = gtext_css_gradient_from_value(v, 0, a, valid, &g);
      *made = g != nullptr;
      return st;
    }, [&](const GTEXT_Allocator *) { gtext_css_gradient_free(g); g = nullptr; });
  }
}

TEST(CssViewsOom, AnImageSetIsEOomWithNothingMadeAndNothingLeftBehind) {
  for (const char * text : {"image-set(\"a.png\" 1x, \"b.png\" 2x)", "image-set(url(a.png) 1x type(\"image/png\"))"}) {
    One o;
    const GTEXT_CSS_Value * v = value_of(o, "<image>", text);
    ASSERT_NE(v, nullptr) << text;
    GTEXT_CSS_Image_Set_Option * opts = nullptr;
    size_t count = 7;
    sweep(text, [&](const GTEXT_Allocator * a, bool * valid, bool * made) {
      opts = reinterpret_cast<GTEXT_CSS_Image_Set_Option *>(&o);
      const GTEXT_CSS_Status st = gtext_css_image_set_options(v, a, valid, &opts, &count);
      if (st != GTEXT_CSS_OK) {
        EXPECT_EQ(count, 0u);
      }
      *made = opts != nullptr;
      return st;
    }, [&](const GTEXT_Allocator * a) {
      gtext_allocator_free(a, opts);  // (allocated with the allocator the view was given)
      opts = nullptr;
    });
  }
}

TEST(CssViewsOom, ATransformFunctionIsEOomWithNothingLeftBehind) {
  for (const char * text : {"translate(1px, 2%)", "rotate(45deg)", "matrix(1, 0, 0, 1, 0, 0)", "scale3d(1, 2, 3)", "perspective(none)"}) {
    One o;
    const GTEXT_CSS_Value * v = first_fn(value_of(o, "<transform-list>", text));
    ASSERT_NE(v, nullptr) << text;
    GTEXT_CSS_Transform_Function f;
    sweep(text, [&](const GTEXT_Allocator * a, bool * valid, bool * made) {
      std::memset(&f, 0x5a, sizeof f);
      const GTEXT_CSS_Status st = gtext_css_transform_function_from_value(v, a, valid, &f);
      *made = st == GTEXT_CSS_OK && *valid;
      if (st != GTEXT_CSS_OK) {
        // the result is zeroed, not left with what it held
        EXPECT_EQ(f.arg_count, 0u);
      }
      return st;
    }, [](const GTEXT_Allocator *) {});
  }
}

TEST(CssViewsOom, AShapeIsEOomWithNothingLeftBehind) {
  for (const char * text : {"polygon(evenodd, 0 0, 1px 1px, 2px 0)", "inset(10px round 5px)", "path(\"M 0 0\")", "circle(50% at 10px 20px)"}) {
    One o;
    const GTEXT_CSS_Value * v = value_of(o, "<basic-shape>", text);
    ASSERT_NE(v, nullptr) << text;
    GTEXT_CSS_Shape sh;
    sweep(text, [&](const GTEXT_Allocator * a, bool * valid, bool * made) {
      std::memset(&sh, 0x5a, sizeof sh);
      const GTEXT_CSS_Status st = gtext_css_shape_from_value(v, a, valid, &sh);
      *made = st == GTEXT_CSS_OK && *valid;
      if (st != GTEXT_CSS_OK) {
        EXPECT_EQ(sh.kind, GTEXT_CSS_SHAPE_NONE);
      }
      return st;
    }, [](const GTEXT_Allocator *) {});
  }
}

// ---- 6: one rule for every view ------------------------------------------------------------------------------------

TEST(CssViews, EveryViewAnswersOkAndNotValidForAValueThatIsNotWhatItReads) {
  One word;
  const GTEXT_CSS_Value * kw = value_of(word, "<ident>", "wobble");
  ASSERT_NE(kw, nullptr);
  One fn;
  const GTEXT_CSS_Value * f = value_of(fn, "<any-value>", "wobble(1)");
  ASSERT_NE(f, nullptr);
  for (const GTEXT_CSS_Value * v : {kw, f}) {
    bool valid = true;
    GTEXT_CSS_Easing * easing = reinterpret_cast<GTEXT_CSS_Easing *>(&word);
    EXPECT_EQ(gtext_css_easing_from_value(v, nullptr, &valid, &easing), GTEXT_CSS_OK);
    EXPECT_FALSE(valid);
    EXPECT_EQ(easing, nullptr);
    valid = true;
    GTEXT_CSS_Gradient * g = reinterpret_cast<GTEXT_CSS_Gradient *>(&word);
    EXPECT_EQ(gtext_css_gradient_from_value(v, 0, nullptr, &valid, &g), GTEXT_CSS_OK);
    EXPECT_FALSE(valid);
    EXPECT_EQ(g, nullptr);
    GTEXT_CSS_Image_Set_Option * o = reinterpret_cast<GTEXT_CSS_Image_Set_Option *>(&word);
    size_t n = 5;
    valid = true;
    EXPECT_EQ(gtext_css_image_set_options(v, nullptr, &valid, &o, &n), GTEXT_CSS_OK);
    EXPECT_FALSE(valid);
    EXPECT_EQ(o, nullptr);
    EXPECT_EQ(n, 0u);
    GTEXT_CSS_Transform_Function tf;
    std::memset(&tf, 0x5a, sizeof tf);
    valid = true;
    EXPECT_EQ(gtext_css_transform_function_from_value(v, nullptr, &valid, &tf), GTEXT_CSS_OK);
    EXPECT_FALSE(valid);
    EXPECT_EQ(tf.arg_count, 0u);
    GTEXT_CSS_Shape sh;
    std::memset(&sh, 0x5a, sizeof sh);
    valid = true;
    EXPECT_EQ(gtext_css_shape_from_value(v, nullptr, &valid, &sh), GTEXT_CSS_OK);
    EXPECT_FALSE(valid);
    EXPECT_EQ(sh.kind, GTEXT_CSS_SHAPE_NONE);
  }
  // a NULL argument is the one error
  bool valid = false;
  GTEXT_CSS_Gradient * g = nullptr;
  EXPECT_EQ(gtext_css_gradient_from_value(nullptr, 0, nullptr, &valid, &g), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_gradient_from_value(kw, 0, nullptr, nullptr, &g), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_gradient_from_value(kw, 0, nullptr, &valid, nullptr), GTEXT_CSS_E_INVALID);
}

// ---- 5: steps() ---------------------------------------------------------------------------------------------------

namespace {

/// steps(`count`) as an easing function: the serialization, or "invalid".
std::string steps_of(const std::string & count) {
  One o;
  run_into(o, "<easing-function>", "steps(" + count + ")");
  if (!o.m.matched) {
    return "no match";
  }
  GTEXT_CSS_Easing * e = nullptr;
  bool valid = false;
  EXPECT_EQ(gtext_css_easing_from_value(o.m.root(), nullptr, &valid, &e), GTEXT_CSS_OK);
  if (!valid) {
    EXPECT_EQ(e, nullptr);
    return "invalid";
  }
  char * buf = nullptr;
  size_t len = 0;
  EXPECT_EQ(gtext_css_easing_serialize_buffer(e, nullptr, &buf, &len), GTEXT_CSS_OK);
  std::string s(buf, len);
  gtext_css_serialize_buffer_free(nullptr, buf);
  gtext_css_easing_free(e);
  return s;
}

}  // namespace

TEST(CssSteps, TheCountIsTestedForRangeBeforeItIsConvertedAndIsAnIntegerExactlyUpTo2To53) {
  EXPECT_EQ(steps_of("1"), "steps(1)");
  EXPECT_EQ(steps_of("0"), "invalid");
  EXPECT_EQ(steps_of("-3"), "invalid");
  EXPECT_EQ(steps_of("calc(9007199254740992)"), "steps(9007199254740992)");  // 2^53
  EXPECT_EQ(steps_of("calc(9007199254740992 + 2)"), "invalid");              // the next double, one past
  EXPECT_EQ(steps_of("calc(9007199254740991)"), "steps(9007199254740991)");
  // values no int64_t holds, and the ones that are not numbers: refused without a conversion
  EXPECT_EQ(steps_of("calc(1e300)"), "invalid");
  EXPECT_EQ(steps_of("calc(-1e300)"), "invalid");
  EXPECT_EQ(steps_of("calc(infinity)"), "invalid");
  EXPECT_EQ(steps_of("calc(-infinity)"), "invalid");
  EXPECT_EQ(steps_of("calc(NaN)"), "invalid");
  EXPECT_EQ(steps_of("calc(9223372036854775808)"), "invalid");  // 2^63, the first double past int64_t
  EXPECT_EQ(steps_of("calc(1.5)"), "invalid");
  EXPECT_EQ(steps_of("calc(0.5 + 0.5)"), "steps(1)");
}

TEST(CssSteps, StepStartAndStepEndAreWrittenAsTheDraftSays) {
  // Easing 2, 2.3.1: "step-start and step-end do not serialize as themselves. Instead, they serialize as "steps(1, start)" and "steps(1)""
  for (const auto & row : std::vector<std::pair<const char *, const char *>>{{"step-start", "steps(1, start)"}, {"step-end", "steps(1)"},
           {"steps(1, jump-start)", "steps(1, jump-start)"}, {"steps(1, start)", "steps(1, start)"}}) {
    One o;
    const GTEXT_CSS_Value * v = value_of(o, "<easing-function>", row.first);
    ASSERT_NE(v, nullptr);
    GTEXT_CSS_Easing * e = nullptr;
    bool valid = false;
    ASSERT_EQ(gtext_css_easing_from_value(v, nullptr, &valid, &e), GTEXT_CSS_OK);
    ASSERT_TRUE(valid);
    char * buf = nullptr;
    size_t len = 0;
    ASSERT_EQ(gtext_css_easing_serialize_buffer(e, nullptr, &buf, &len), GTEXT_CSS_OK);
    EXPECT_EQ(std::string(buf, len), row.second) << row.first;
    gtext_css_serialize_buffer_free(nullptr, buf);
    gtext_css_easing_free(e);
  }
}

// ---- 18: linear() ---------------------------------------------------------------------------------------------------

namespace {

std::string linear_of(const std::string & args) {
  One o;
  run_into(o, "<easing-function>", "linear(" + args + ")");
  if (!o.m.matched) {
    return "no match";
  }
  GTEXT_CSS_Easing * e = nullptr;
  bool valid = false;
  EXPECT_EQ(gtext_css_easing_from_value(o.m.root(), nullptr, &valid, &e), GTEXT_CSS_OK);
  if (!valid) {
    return "invalid";
  }
  char * buf = nullptr;
  size_t len = 0;
  EXPECT_EQ(gtext_css_easing_serialize_buffer(e, nullptr, &buf, &len), GTEXT_CSS_OK);
  std::string s(buf, len);
  gtext_css_serialize_buffer_free(nullptr, buf);
  gtext_css_easing_free(e);
  return s;
}

}  // namespace

TEST(CssLinear, ThePercentagesOfASegmentComeBeforeOrAfterItsNumberAndThereIsExactlyOneNumber) {
  // `<number> && <percentage>{0,2}`: either order. (The value of a && is in the order of the grammar, number first, so the view never
  // sees a percentage before its number: these were never reported invalid, and this keeps it so. No plant: nothing to restore.)
  EXPECT_EQ(linear_of("0% 0.5, 1"), "linear(0.5 0%, 1)");
  EXPECT_EQ(linear_of("25% 0"), "linear(0 25%)");
  EXPECT_EQ(linear_of("0 0%, 1"), linear_of("0% 0, 1"));
  EXPECT_EQ(linear_of("25% 75% 0.5, 1"), linear_of("0.5 25% 75%, 1"));
  EXPECT_EQ(linear_of("0, 0.5 25%, 1"), "linear(0, 0.5 25%, 1)");
  // a segment with two numbers, or with none, is not a segment
  EXPECT_EQ(linear_of("0 1"), "no match");
  EXPECT_EQ(linear_of("50%"), "no match");
  EXPECT_EQ(linear_of("0, 50%, 1"), "no match");
}

// ---- 7, 8: what the public wrappers clear and report ----------------------------------------------------------------

TEST(CssMathParse, TheResultsAreClearedOnEntryWhateverComesOfTheCall) {
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_component_values, "calc(1px + 2px)");
  ASSERT_EQ(p.status, GTEXT_CSS_OK);
  // out of memory
  FaultAllocator fa;
  fa.arm(1);
  GTEXT_CSS_Math_Options o = gtext_css_math_options_default();
  o.allocator = &fa.alloc;
  bool valid = true;
  GTEXT_CSS_Math * m = reinterpret_cast<GTEXT_CSS_Math *>(&p);
  EXPECT_EQ(gtext_css_math_parse(&p.tree->values[0], nullptr, &o, &valid, &m, nullptr), GTEXT_CSS_E_OOM);
  EXPECT_FALSE(valid);
  EXPECT_EQ(m, nullptr);
  fa.disarm();
  EXPECT_TRUE(fa.live.empty());
  // a limit
  css_test::Parsed deep;
  css_test::parse_into(deep, gtext_css_parse_component_values, "calc(calc(calc(1)))");
  GTEXT_CSS_Math_Options lim = gtext_css_math_options_default();
  lim.max_depth = 2;
  valid = true;
  m = reinterpret_cast<GTEXT_CSS_Math *>(&p);
  EXPECT_EQ(gtext_css_math_parse(&deep.tree->values[0], nullptr, &lim, &valid, &m, nullptr), GTEXT_CSS_E_LIMIT_DEPTH);
  EXPECT_FALSE(valid);
  EXPECT_EQ(m, nullptr);
  lim = gtext_css_math_options_default();
  lim.max_nodes = 1;
  valid = true;
  m = reinterpret_cast<GTEXT_CSS_Math *>(&p);
  EXPECT_EQ(gtext_css_math_parse(&p.tree->values[0], nullptr, &lim, &valid, &m, nullptr), GTEXT_CSS_E_LIMIT_MATH_NODES);
  EXPECT_FALSE(valid);
  EXPECT_EQ(m, nullptr);
}

TEST(CssColorParse, TheResultsAreClearedOnEntryWhateverComesOfTheCall) {
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_component_values, "color-mix(in srgb, red, blue)");
  ASSERT_EQ(p.status, GTEXT_CSS_OK);
  FaultAllocator fa;
  fa.arm(1);
  GTEXT_CSS_Color_Options o = gtext_css_color_options_default();
  o.allocator = &fa.alloc;
  bool valid = true;
  GTEXT_CSS_Color * c = reinterpret_cast<GTEXT_CSS_Color *>(&p);
  EXPECT_EQ(gtext_css_color_parse(&p.tree->values[0], &o, &valid, &c, nullptr), GTEXT_CSS_E_OOM);
  EXPECT_FALSE(valid);
  EXPECT_EQ(c, nullptr);
  fa.disarm();
  EXPECT_TRUE(fa.live.empty());
  GTEXT_CSS_Color_Options lim = gtext_css_color_options_default();
  lim.max_nesting = 1;
  valid = true;
  c = reinterpret_cast<GTEXT_CSS_Color *>(&p);
  EXPECT_EQ(gtext_css_color_parse(&p.tree->values[0], &lim, &valid, &c, nullptr), GTEXT_CSS_E_LIMIT_COLOR_NESTING);
  EXPECT_FALSE(valid);
  EXPECT_EQ(c, nullptr);
}

TEST(CssColorParse, TheResultReportsTheColorsTheBuildMade) {
  struct Row {
    const char * text;
    size_t nodes;
  };
  for (const Row & r : std::vector<Row>{{"red", 1}, {"#fff", 1}, {"rgb(1 2 3)", 1}, {"light-dark(red, blue)", 3}, {"color-mix(in srgb, red, blue)", 3},
           {"color-mix(in srgb, light-dark(red, blue), rgb(from green r g b))", 6}, {"rgb(from color-mix(in srgb, red, blue) r g b)", 4},
           {"contrast-color(light-dark(red, color-mix(in srgb, red, blue)))", 6}}) {
    gc::Color c;
    gc::read(c, r.text);
    ASSERT_EQ(c.status, GTEXT_CSS_OK) << r.text;
    ASSERT_TRUE(c.valid) << r.text;
    EXPECT_EQ(c.result.nodes, r.nodes) << r.text;
  }
}

// ---- 9: which color the builder takes, and what a function that has no origin does with `from` ---------------------

TEST(CssColorBuild, DeviceCmykHasNoOriginAndANestingLimitInItsFromIsNotReachedAtAll) {
  // "There is no relative device-cmyk() syntax" (Color 5): the text after `from` is no color of this function, so it is not built, and a
  // limit that building it would reach is not reached: the color is not valid, and that is all
  GTEXT_CSS_Color_Options o = gtext_css_color_options_default();
  o.max_nesting = 1;
  gc::Color c;
  gc::read(c, "device-cmyk(from color-mix(in srgb, red, blue) 0 0 0 0)", &o);
  EXPECT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_FALSE(c.valid);
  // the same text with a relative function that does have an origin does reach it
  gc::Color r;
  gc::read(r, "rgb(from color-mix(in srgb, red, blue) r g b)", &o);
  EXPECT_EQ(r.status, GTEXT_CSS_E_LIMIT_COLOR_NESTING);
  // and the forms the grammar has are read
  // Color 5, Serializing device-cmyk Values: numbers separated by spaces ("device-cmyk(0 81% 81% 30%)" is "device-cmyk(0 0.81 0.81 0.3)")
  EXPECT_EQ(gc::color("device-cmyk(0, 0, 0, 1)"), "device-cmyk(0 0 0 1)");
  EXPECT_EQ(gc::color("device-cmyk(0 81% 81% 30%)"), "device-cmyk(0 0.81 0.81 0.3)");
  EXPECT_EQ(gc::color("device-cmyk(0% 0 none 1 / 50%)"), "device-cmyk(0 0 none 1 / 0.5)");
  // the grammar: the legacy form is four numbers (no percentage, no alpha), there is no relative form
  for (const char * bad : {"device-cmyk(0%, 0, 0, 1)", "device-cmyk(0, 0, 0, 1, 0.5)", "device-cmyk(0, 0, none, 1)", "device-cmyk(0 0 0)",
           "device-cmyk(from red 0 0 0 0)", "device-cmyk(0 0 0 0 / )"}) {
    EXPECT_EQ(gc::color(bad), "invalid") << bad;
  }
}

TEST(CssColorBuild, TheKidsAreTakenInTheOrderOfTheComponentValuesTheyWereMadeFrom) {
  gc::Color c;
  gc::read(c, "color-mix(in srgb, 30% rgb(from red r g b), light-dark(blue, color-mix(in srgb, green, rgb(1 2 3))) 20%)");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  ASSERT_TRUE(c.valid);
  EXPECT_EQ(gc::serialize(c.color), "color-mix(in srgb, rgb(from red r g b) 30%, light-dark(blue, color-mix(in srgb, green, rgb(1, 2, 3))) 20%)");
  ASSERT_EQ(c.color->item_count, 2u);
  EXPECT_EQ(c.color->items[0]->kind, GTEXT_CSS_COLOR_FUNCTION);
  ASSERT_NE(c.color->items[0]->origin, nullptr);
  EXPECT_EQ(gc::serialize(c.color->items[0]->origin), "red");
  EXPECT_EQ(c.color->items[1]->kind, GTEXT_CSS_COLOR_LIGHT_DARK);
  // texts that put something that is not a color where a color goes are not valid colors, and are not limits or crashes
  for (const char * bad : {"color-mix(in srgb, red blue, green)", "color-mix(in srgb, 50% 50% red, blue)", "light-dark(red)", "light-dark(red, 1)",
           "light-dark(1, red)", "color-mix(in srgb, foo(1), red)", "rgb(from 1 r g b)", "rgb(from foo(1) r g b)", "contrast-color(1)"}) {
    gc::Color b;
    gc::read(b, bad);
    EXPECT_EQ(b.status, GTEXT_CSS_OK) << bad;
    EXPECT_FALSE(b.valid) << bad;
    EXPECT_EQ(b.color, nullptr) << bad;
  }
}

// ---- 10: gradients -------------------------------------------------------------------------------------------------

namespace {

GTEXT_CSS_Value make_value(GTEXT_CSS_Val_Kind kind, const char * text) {
  GTEXT_CSS_Value v;
  std::memset(&v, 0, sizeof v);
  v.kind = kind;
  v.text.data = text;
  v.text.length = text ? std::strlen(text) : 0;
  return v;
}

}  // namespace

TEST(CssGradientView, TheKindIsFromTheNameByItsLengthAndNotFromWhatFollowsItInMemory) {
  // the name's bytes are followed in memory by other text: a search for "radial" in the name's string would run on into it
  const char name_and_more[] = "linear-gradientradial";
  GTEXT_CSS_Value kids[3];
  kids[0] = make_value(GTEXT_CSS_VAL_COLOR, "red");
  kids[1] = make_value(GTEXT_CSS_VAL_TOKEN, ",");
  kids[2] = make_value(GTEXT_CSS_VAL_COLOR, "blue");
  GTEXT_CSS_Value fn = make_value(GTEXT_CSS_VAL_FUNCTION, name_and_more);
  fn.text.length = 15;  // "linear-gradient"
  fn.children = kids;
  fn.child_count = 3;
  bool valid = false;
  GTEXT_CSS_Gradient * g = nullptr;
  ASSERT_EQ(gtext_css_gradient_from_value(&fn, 0, nullptr, &valid, &g), GTEXT_CSS_OK);
  ASSERT_TRUE(valid);
  EXPECT_EQ(g->kind, GTEXT_CSS_GRADIENT_LINEAR);
  EXPECT_FALSE(g->repeating);
  EXPECT_EQ(g->stop_count, 2u);
  gtext_css_gradient_free(g);
  const char rep[] = "repeating-conic-gradientlinear";
  fn.text.data = rep;
  fn.text.length = 24;
  ASSERT_EQ(gtext_css_gradient_from_value(&fn, 0, nullptr, &valid, &g), GTEXT_CSS_OK);
  ASSERT_TRUE(valid);
  EXPECT_EQ(g->kind, GTEXT_CSS_GRADIENT_CONIC);
  EXPECT_TRUE(g->repeating);
  gtext_css_gradient_free(g);
}

TEST(CssGradientView, ASegmentWithNeitherAColorNorAPositionIsNoStopAndAStrayKeywordIsRefused) {
  GTEXT_CSS_Value kw = make_value(GTEXT_CSS_VAL_KEYWORD, "stray");
  GTEXT_CSS_Value comma = make_value(GTEXT_CSS_VAL_TOKEN, ",");
  GTEXT_CSS_Value red = make_value(GTEXT_CSS_VAL_COLOR, "red");
  GTEXT_CSS_Value blue = make_value(GTEXT_CSS_VAL_COLOR, "blue");
  GTEXT_CSS_Value ten = make_value(GTEXT_CSS_VAL_PERCENTAGE, "10%");
  ten.number = 10;
  auto run = [&](std::vector<GTEXT_CSS_Value> kids) {
    GTEXT_CSS_Value fn = make_value(GTEXT_CSS_VAL_FUNCTION, "linear-gradient");
    fn.children = kids.data();
    fn.child_count = kids.size();
    bool valid = true;
    GTEXT_CSS_Gradient * g = reinterpret_cast<GTEXT_CSS_Gradient *>(&kids);
    EXPECT_EQ(gtext_css_gradient_from_value(&fn, 0, nullptr, &valid, &g), GTEXT_CSS_OK);
    EXPECT_EQ(valid, g != nullptr);
    size_t stops = g ? g->stop_count : 0;
    if (g) {
      for (size_t i = 0; i < g->stop_count; i++) {
        EXPECT_TRUE(g->stops[i].color || g->stops[i].position) << "an empty stop";
      }
    }
    gtext_css_gradient_free(g);
    return valid ? static_cast<int>(stops) : -1;
  };
  EXPECT_EQ(run({red, comma, blue}), 2);
  EXPECT_EQ(run({kw, comma, red, comma, blue}), 2);                 // the first segment is the configuration
  EXPECT_EQ(run({red, comma, ten, comma, blue}), 3);                // a hint
  EXPECT_EQ(run({red, comma, kw, comma, blue}), -1);                // a segment with a keyword and nothing else
  EXPECT_EQ(run({red, comma, blue, kw, comma, red}), -1);           // a stray keyword beside a color
  EXPECT_EQ(run({red, comma, comma, blue}), -1);                    // an empty segment
  EXPECT_EQ(run({red, comma, ten, ten, comma, blue}), -1);          // two positions and no color
  EXPECT_EQ(run({red, blue, comma, red}), -1);                      // two colors in a stop
}

// ---- 11: transform arguments ---------------------------------------------------------------------------------------

namespace {

// `keep`, when given, holds the match the function was read from, so that the sources its arguments point at outlive the call
GTEXT_CSS_Transform_Function transform_of(const std::string & text, bool * valid_out = nullptr, One * keep = nullptr) {
  One local;
  One & o = keep ? *keep : local;
  GTEXT_CSS_Transform_Function f;
  std::memset(&f, 0x5a, sizeof f);
  const GTEXT_CSS_Value * v = first_fn(value_of(o, "<transform-list>", text));
  bool valid = false;
  if (v) {
    EXPECT_EQ(gtext_css_transform_function_from_value(v, nullptr, &valid, &f), GTEXT_CSS_OK) << text;
  }
  if (valid_out) {
    *valid_out = valid;
  }
  return f;
}

}  // namespace

TEST(CssTransformView, ALengthKeepsTheUnitOfTheValueItCameTo) {
  bool valid = false;
  GTEXT_CSS_Transform_Function f = transform_of("translateX(calc(1em + 2em))", &valid);
  ASSERT_TRUE(valid);
  ASSERT_EQ(f.arg_count, 1u);
  EXPECT_EQ(f.args[0].kind, GTEXT_CSS_TARG_LENGTH);
  EXPECT_EQ(f.args[0].unit, GTEXT_CSS_UNIT_EM);  // not px
  EXPECT_EQ(f.args[0].value, 3.0);
  f = transform_of("translateY(calc(1in + 1px))", &valid);
  ASSERT_TRUE(valid);
  EXPECT_EQ(f.args[0].unit, GTEXT_CSS_UNIT_PX);  // an absolute length is in the canonical unit
  EXPECT_EQ(f.args[0].value, 97.0);
  f = transform_of("translate(3rem, 10%)", &valid);
  ASSERT_TRUE(valid);
  EXPECT_EQ(f.args[0].unit, GTEXT_CSS_UNIT_REM);
  EXPECT_EQ(f.args[1].kind, GTEXT_CSS_TARG_PERCENT);
  EXPECT_EQ(f.args[1].unit, GTEXT_CSS_UNIT_NONE);
}

TEST(CssTransformView, ACalculationThatNeedsAnEnvironmentIsMathWithItsSourceAndNoGarbageValue) {
  bool valid = false;
  One keep;  // the arguments' `source` points into it
  GTEXT_CSS_Transform_Function f = transform_of("translate(calc(1em + 1px), calc(10% + 1px))", &valid, &keep);
  ASSERT_TRUE(valid);
  ASSERT_EQ(f.arg_count, 2u);
  for (size_t i = 0; i < 2; i++) {
    EXPECT_EQ(f.args[i].kind, GTEXT_CSS_TARG_MATH) << i;
    EXPECT_EQ(f.args[i].value, 0.0) << i;
    ASSERT_NE(f.args[i].source, nullptr);
    EXPECT_NE(f.args[i].source->math, nullptr);
  }
}

TEST(CssTransformView, NoneKeywordsAndZeroAreEachWhatTheyAre) {
  bool valid = false;
  GTEXT_CSS_Transform_Function f = transform_of("perspective(none)", &valid);
  ASSERT_TRUE(valid);
  ASSERT_EQ(f.arg_count, 1u);
  EXPECT_EQ(f.args[0].kind, GTEXT_CSS_TARG_NONE);
  EXPECT_EQ(f.args[0].value, 0.0);
  f = transform_of("rotate(0)", &valid);
  ASSERT_TRUE(valid);
  EXPECT_EQ(f.args[0].kind, GTEXT_CSS_TARG_ANGLE);
  f = transform_of("translate3d(1px, 2px, 3px)", &valid);
  ASSERT_TRUE(valid);
  EXPECT_EQ(f.arg_count, 3u);
  EXPECT_EQ(f.args[2].kind, GTEXT_CSS_TARG_LENGTH);
}

TEST(CssTransformView, TheCallersAllocatorIsTheOneUsed) {
  One o;
  const GTEXT_CSS_Value * v = first_fn(value_of(o, "<transform-list>", "translate(1px, 2px)"));
  ASSERT_NE(v, nullptr);
  CountingAllocator ca;
  GTEXT_CSS_Transform_Function f;
  bool valid = false;
  EXPECT_EQ(gtext_css_transform_function_from_value(v, &ca.alloc, &valid, &f), GTEXT_CSS_OK);
  EXPECT_TRUE(valid);
  EXPECT_GT(ca.peak, 0u);
  EXPECT_EQ(ca.current, 0u);
}

// ---- 13: progress() and random() ----------------------------------------------------------------------------------

TEST(CssMathProgress, TheSyntaxIsTheServedDraftsAndNoClampIsItsOwnFlag) {
  // Values 5, 10.2: "<progress()> = progress(no-clamp? <calc-sum>, <calc-sum>, <calc-sum>)": commas, and no `from ... to`
  EXPECT_EQ(gm::calc("progress(5px, 0px, 10px)"), "calc(0.5)");
  EXPECT_EQ(gm::calc("progress(15px, 0px, 10px)"), "calc(1)");           // clamped to [0, 1]
  EXPECT_EQ(gm::calc("progress(no-clamp 15px, 0px, 10px)"), "calc(1.5)");
  EXPECT_EQ(gm::calc("progress(5px from 0px to 10px)"), "invalid");
  EXPECT_EQ(gm::calc("progress(5px, 0px)"), "invalid");
  EXPECT_EQ(gm::calc("progress(no-clamp, 5px, 0px, 10px)"), "invalid");
  // not folded (a relative unit): the flag is kept, is written, and is not the flag of round()
  gm::Calc c;
  gm::read(c, "progress(no-clamp 5em, 0px, 10em)");
  ASSERT_TRUE(c.valid);
  const GTEXT_CSS_Math_Node * n = c.math->root;
  ASSERT_EQ(n->op, GTEXT_CSS_MATH_PROGRESS);
  EXPECT_TRUE(n->no_clamp);
  EXPECT_FALSE(n->strategy_written);
  EXPECT_EQ(gm::serialize(c.math), "progress(no-clamp 5em, 0px, 10em)");
  gm::Calc d;
  gm::read(d, "progress(5em, 0px, 10em)");
  ASSERT_TRUE(d.valid);
  EXPECT_FALSE(d.math->root->no_clamp);
  EXPECT_EQ(gm::serialize(d.math), "progress(5em, 0px, 10em)");
  gm::Calc r;
  gm::read(r, "round(up, 7em, 2em)");
  ASSERT_TRUE(r.valid);
  EXPECT_TRUE(r.math->root->strategy_written);
  EXPECT_FALSE(r.math->root->no_clamp);
  // resolving: the flag decides, the strategy of round() does not enter into it
  GTEXT_CSS_Math_Env env;
  std::memset(&env, 0, sizeof env);
  env.unit_size = [](void *, GTEXT_CSS_Unit, double * px) { *px = 16.0; return true; };
  double v = 0;
  bool resolved = false;
  gm::Calc far_clamped, far_free;
  gm::read(far_clamped, "progress(20em, 0px, 10em)");
  gm::read(far_free, "progress(no-clamp 20em, 0px, 10em)");
  ASSERT_EQ(gtext_css_math_resolve(far_clamped.math, &env, nullptr, &v, &resolved), GTEXT_CSS_OK);
  ASSERT_TRUE(resolved);
  EXPECT_EQ(v, 1.0);
  ASSERT_EQ(gtext_css_math_resolve(far_free.math, &env, nullptr, &v, &resolved), GTEXT_CSS_OK);
  ASSERT_TRUE(resolved);
  EXPECT_EQ(v, 2.0);
}

TEST(CssMathRandom, TheKeyIsInTheNodeThatMkFnMadeAndIsWritten) {
  gm::Calc c;
  gm::read(c, "random(--a element-scoped, 1em, 2em)");
  ASSERT_TRUE(c.valid);
  const GTEXT_CSS_Math_Node * n = c.math->root;
  ASSERT_EQ(n->op, GTEXT_CSS_MATH_RANDOM);
  EXPECT_EQ(std::string(n->text.data, n->text.length), "--a element-scoped");
  EXPECT_EQ(gm::serialize(c.math), "random(--a element-scoped, 1em, 2em)");
  gm::Calc plain;
  gm::read(plain, "random(1em, 2em)");
  ASSERT_TRUE(plain.valid);
  EXPECT_EQ(plain.math->root->text.length, 0u);
}

// ---- 20: the tree-counting functions take no arguments --------------------------------------------------------------

TEST(CssMathTreeCounting, SiblingIndexAndCountTakeNothingAndTheDroppedProgressFunctionsAreNotKnown) {
  EXPECT_EQ(gm::calc("calc(sibling-index() * 1px)"), "calc(1px * sibling-index())");
  EXPECT_EQ(gm::calc("calc(sibling-count() + 1)"), "calc(1 + sibling-count())");
  EXPECT_EQ(gm::calc("calc(sibling-index( ) * 1px)"), "calc(1px * sibling-index())");
  EXPECT_EQ(gm::calc("calc(sibling-index(1) * 1px)"), "invalid");
  EXPECT_EQ(gm::calc("calc(sibling-count(foo) + 1)"), "invalid");
  EXPECT_EQ(gm::calc("sibling-index(2)"), "invalid");
  // Values 5 "Dropped media-progess() and container-progress() in favor of using relevant units in progress()"
  EXPECT_EQ(gm::calc("calc(media-progress(width from 1px to 2px) * 1px)"), "invalid");
  EXPECT_EQ(gm::calc("calc(media-progress() * 1px)"), "invalid");
  EXPECT_EQ(gm::calc("calc(container-progress() + 1)"), "invalid");
  EXPECT_EQ(gm::calc("calc(container-progress(width) + 1)"), "invalid");
}

// ---- 19: a ratio takes the math functions a number does --------------------------------------------------------------

TEST(CssRatio, ACalculationWithAPercentageIsNotANumberAndSoNotAPartOfARatioEither) {
  for (const char * text : {"calc(10% / 5%)", "calc(1 + 10% / 5%)"}) {
    One number, ratio, tail;
    run_into(number, "<number>", text);
    run_into(ratio, "<ratio>", text);
    run_into(tail, "<ratio>", std::string("16 / ") + text);
    EXPECT_FALSE(number.m.matched) << text;
    EXPECT_FALSE(ratio.m.matched) << text;
    EXPECT_FALSE(tail.m.matched) << text;
  }
  One ok;
  run_into(ok, "<ratio>", "calc(16) / calc(4 + 5)");
  EXPECT_TRUE(ok.m.matched);
}

// ---- 14: the lazily made definitions ------------------------------------------------------------------------------

TEST(CssFunctionTypesLazy, AFailureForLackOfMemoryIsReportedAndNotRememberedAsABadGrammar) {
  FaultAllocator fa;
  GTEXT_CSS_Function_Types_Options o;
  std::memset(&o, 0, sizeof o);
  o.allocator = &fa.alloc;
  GTEXT_CSS_Function_Types * t = nullptr;
  ASSERT_EQ(gtext_css_function_types_new(&o, &t), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_function_types_status(t), GTEXT_CSS_OK);
  fa.arm(1);
  EXPECT_EQ(gtext_css_function_types_resolve(t, GTEXT_CSS_REF_TYPE, "easing-function", 15), nullptr);
  EXPECT_EQ(gtext_css_function_types_status(t), GTEXT_CSS_E_OOM);
  // asked again with memory, it is made
  fa.disarm();
  EXPECT_NE(gtext_css_function_types_resolve(t, GTEXT_CSS_REF_TYPE, "easing-function", 15), nullptr);
  EXPECT_EQ(gtext_css_function_types_status(t), GTEXT_CSS_OK);
  // a name that is none of the types is NULL with status OK (the link reports the missing reference)
  EXPECT_EQ(gtext_css_function_types_resolve(t, GTEXT_CSS_REF_TYPE, "wobble", 6), nullptr);
  EXPECT_EQ(gtext_css_function_types_status(t), GTEXT_CSS_OK);
  gtext_css_function_types_free(t);
  EXPECT_TRUE(fa.live.empty());
}

TEST(CssFunctionTypesLazy, PrepareMakesEveryDefinitionSoThatTheObjectDoesNotChangeAgain) {
  FaultAllocator fa;
  GTEXT_CSS_Function_Types_Options o;
  std::memset(&o, 0, sizeof o);
  o.allocator = &fa.alloc;
  GTEXT_CSS_Function_Types * t = nullptr;
  ASSERT_EQ(gtext_css_function_types_new(&o, &t), GTEXT_CSS_OK);
  fa.arm(40);
  EXPECT_EQ(gtext_css_function_types_prepare(t), GTEXT_CSS_E_OOM);
  fa.disarm();
  ASSERT_EQ(gtext_css_function_types_prepare(t), GTEXT_CSS_OK);
  // nothing is allocated by a resolve now: any allocation would be the 1st since arm()
  fa.arm(1);
  const size_t n = gtext_css_function_type_count();
  for (size_t i = 0; i < n; i++) {
    const char * name = nullptr;
    GTEXT_CSS_Ref_Kind kind = GTEXT_CSS_REF_TYPE;
    ASSERT_TRUE(gtext_css_function_type_at(i, &name, &kind, nullptr));
    EXPECT_NE(gtext_css_function_types_resolve(t, kind, name, std::strlen(name)), nullptr) << name;
  }
  EXPECT_FALSE(fa.failed);
  EXPECT_EQ(gtext_css_function_types_prepare(nullptr), GTEXT_CSS_E_INVALID);
  gtext_css_function_types_free(t);
}

// ---- 16: the families are a closed list ---------------------------------------------------------------------------

TEST(CssFunctionFamilyBoundary, TheFunctionsOfOtherModulesAreNoneAndUrlIsAnImage) {
  struct Row {
    const char * name;
    GTEXT_CSS_Function_Family family;
  };
  for (const Row & r : std::vector<Row>{{"url", GTEXT_CSS_FN_FAMILY_IMAGE}, {"src", GTEXT_CSS_FN_FAMILY_IMAGE}, {"image", GTEXT_CSS_FN_FAMILY_IMAGE},
           {"symbols", GTEXT_CSS_FN_FAMILY_SUBSTITUTION}, {"counter", GTEXT_CSS_FN_FAMILY_SUBSTITUTION}, {"calc", GTEXT_CSS_FN_FAMILY_MATH},
           {"rgb", GTEXT_CSS_FN_FAMILY_COLOR}, {"linear-gradient", GTEXT_CSS_FN_FAMILY_GRADIENT},
           // other modules' functions: Filter Effects, Anchor Positioning, Motion Path, Grid, vendor prefixes
           {"blur", GTEXT_CSS_FN_FAMILY_NONE}, {"drop-shadow", GTEXT_CSS_FN_FAMILY_NONE}, {"anchor", GTEXT_CSS_FN_FAMILY_NONE},
           {"anchor-size", GTEXT_CSS_FN_FAMILY_NONE}, {"ray", GTEXT_CSS_FN_FAMILY_NONE}, {"minmax", GTEXT_CSS_FN_FAMILY_NONE},
           {"repeat", GTEXT_CSS_FN_FAMILY_NONE}, {"fit-content", GTEXT_CSS_FN_FAMILY_NONE}, {"-webkit-linear-gradient", GTEXT_CSS_FN_FAMILY_NONE},
           {"wobble", GTEXT_CSS_FN_FAMILY_NONE}, {"", GTEXT_CSS_FN_FAMILY_NONE}}) {
    EXPECT_EQ(gtext_css_function_family(r.name, std::strlen(r.name)), r.family) << r.name;
  }
  EXPECT_EQ(gtext_css_function_family(nullptr, 0), GTEXT_CSS_FN_FAMILY_NONE);
  // every function this layer defines a type for is in a family
  const size_t n = gtext_css_function_type_count();
  for (size_t i = 0; i < n; i++) {
    const char * name = nullptr;
    const char * grammar = nullptr;
    GTEXT_CSS_Ref_Kind kind;
    ASSERT_TRUE(gtext_css_function_type_at(i, &name, &kind, &grammar));
    const std::string s = name;
    if (s.size() > 2 && s.compare(s.size() - 2, 2, "()") == 0) {
      const std::string base = s.substr(0, s.size() - 2);
      if (base == "calc-sum" || base == "linear-easing-function") {
        continue;
      }
      const GTEXT_CSS_Function_Family f = gtext_css_function_family(base.data(), base.size());
      // (the gradient functions, `cross-fade()` ... are images; `type()` and `if()` branches are not functions of a family of their own)
      if (base == "type") {
        continue;
      }
      EXPECT_NE(f, GTEXT_CSS_FN_FAMILY_NONE) << s;
    }
  }
}

// ---- 21: nested colors are written in memory proportional to the output ---------------------------------------------

TEST(CssColorSerialize, ANestingToTheLimitIsWrittenWithoutKeepingEveryLevelsText) {
  const size_t depth = 2000;
  std::string text;
  for (size_t i = 0; i < depth; i++) {
    text += "light-dark(";
  }
  text += "red";
  for (size_t i = 0; i < depth; i++) {
    text += ", blue)";
  }
  GTEXT_CSS_Color_Options o = gtext_css_color_options_default();
  o.max_nesting = 4096;
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  po.max_depth = 8192;
  css_test::Parsed parsed;
  css_test::parse_into(parsed, gtext_css_parse_component_values, text, &po);
  ASSERT_EQ(parsed.status, GTEXT_CSS_OK);
  struct {
    GTEXT_CSS_Color * color = nullptr;
    bool valid = false;
    GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;
  } c;
  c.status = gtext_css_color_parse(&parsed.tree->values[0], &o, &c.valid, &c.color, nullptr);
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  ASSERT_TRUE(c.valid);
  CountingAllocator ca;
  GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
  so.allocator = &ca.alloc;
  char * buf = nullptr;
  size_t len = 0;
  ASSERT_EQ(gtext_css_color_serialize_buffer(c.color, &so, &buf, &len), GTEXT_CSS_OK);
  EXPECT_EQ(len, text.size());
  gtext_css_serialize_buffer_free(&so, buf);
  // each level's text holds all the levels inside it: kept for every level it is the sum of an arithmetic series (about 36 MB at this depth),
  // freed once its parent is written it is a few times the output
  EXPECT_LT(ca.peak, 16 * len + ((size_t)1 << 20)) << "peak " << ca.peak << " for an output of " << len;
  EXPECT_EQ(ca.current, 0u);
  gtext_css_color_free(c.color);
}

// ---- 22: a long keyword resolves ------------------------------------------------------------------------------------

TEST(CssMathResolve, AContextKeywordOfAnyLengthIsHandedToTheEnvironmentWhole) {
  const std::string name(200, 'k');
  GTEXT_CSS_Math_Keyword kw;
  kw.name = name.c_str();
  std::memset(&kw.type, 0, sizeof kw.type);
  GTEXT_CSS_Math_Context ctx;
  ctx.keywords = &kw;
  ctx.keyword_count = 1;
  gm::Calc c;
  gm::read(c, "calc(" + name + " * 2)", &ctx);
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  ASSERT_TRUE(c.valid);
  struct Seen {
    std::string name;
  } seen;
  GTEXT_CSS_Math_Env env;
  std::memset(&env, 0, sizeof env);
  env.user = &seen;
  env.symbol = [](void * user, const char * n, double * v) {
    static_cast<Seen *>(user)->name = n;
    *v = 21.0;
    return true;
  };
  double v = 0;
  bool resolved = false;
  ASSERT_EQ(gtext_css_math_resolve(c.math, &env, nullptr, &v, &resolved), GTEXT_CSS_OK);
  EXPECT_TRUE(resolved);
  EXPECT_EQ(v, 42.0);
  EXPECT_EQ(seen.name, name);
}

// ---- 23: mod() and rem() against exact arithmetic -------------------------------------------------------------------

namespace {


double mod_of(double a, double b) {
  double out, in[2] = {a, b};
  EXPECT_TRUE(css_math_apply(GTEXT_CSS_MATH_MOD, GTEXT_CSS_MATH_ROUND_NEAREST, in, 2, &out));
  return out;
}

double rem_of(double a, double b) {
  double out, in[2] = {a, b};
  EXPECT_TRUE(css_math_apply(GTEXT_CSS_MATH_REM, GTEXT_CSS_MATH_ROUND_NEAREST, in, 2, &out));
  return out;
}

/// The next double toward zero.
double toward_zero(double x) {
  return std::nextafter(x, 0.0);
}

/// A double that is `i` * 2^-s, i up to 2^52.
double scaled(int64_t i, int s) {
  return std::ldexp(static_cast<double>(i), -s);
}

}  // namespace

TEST(CssMathModRem, ModIsBelowTheModulusInMagnitudeEvenWhenTheSumRoundsToIt) {
  // mod(-1e-20, 1): the exact result is 1 - 1e-20, which is not a double; the nearest double is 1, which is not below 1
  EXPECT_EQ(mod_of(-1e-20, 1.0), toward_zero(1.0));
  EXPECT_LT(mod_of(-1e-20, 1.0), 1.0);
  EXPECT_EQ(mod_of(1e-20, -1.0), toward_zero(-1.0));
  EXPECT_GT(mod_of(1e-20, -1.0), -1.0);
  EXPECT_EQ(mod_of(-1e-300, 5.0), toward_zero(5.0));
  EXPECT_EQ(mod_of(-5e-324, 1e10), toward_zero(1e10));
  // the ordinary cases
  EXPECT_EQ(mod_of(-7.0, 3.0), 2.0);
  EXPECT_EQ(mod_of(7.0, -3.0), -2.0);
  EXPECT_EQ(mod_of(-7.0, -3.0), -1.0);
  EXPECT_EQ(mod_of(7.5, 2.0), 1.5);
  EXPECT_EQ(mod_of(-0.5, 1.0), 0.5);
  EXPECT_TRUE(std::signbit(mod_of(-4.0, 2.0)) == false);
  EXPECT_TRUE(std::signbit(mod_of(4.0, -2.0)));
  EXPECT_TRUE(std::isnan(mod_of(1.0, 0.0)));
  EXPECT_TRUE(std::isnan(mod_of(INFINITY, 2.0)));
  EXPECT_EQ(mod_of(5.0, INFINITY), 5.0);
  EXPECT_TRUE(std::isnan(mod_of(-5.0, INFINITY)));
  EXPECT_EQ(rem_of(-7.0, 3.0), -1.0);
  EXPECT_EQ(rem_of(7.0, -3.0), 1.0);
}

namespace {

/// Random operands A = ia * 2^-sa and B = ib * 2^-sb, with the integers `bits` wide and the scales `spread` apart, compared with the exact
/// remainders computed in the integer type I (wide enough to hold the integers shifted by the spread). Returns the number of cases in which
/// the exact floor modulus, rounded to a double, reached |B| (the case the correction is for).
template <typename I>
size_t mod_rem_against_exact(unsigned seed, int cases, int bits, int spread) {
  std::mt19937_64 rng(seed);
  size_t reached_b = 0;
  for (int i = 0; i < cases; i++) {
    const int64_t ia = static_cast<int64_t>(rng() >> (64 - bits + static_cast<int>(rng() % 8))) * ((rng() & 1) ? 1 : -1);
    int64_t ib = static_cast<int64_t>(rng() >> (64 - bits + static_cast<int>(rng() % 8))) * ((rng() & 1) ? 1 : -1);
    if (ib == 0) {
      ib = 1;
    }
    // (B's scale is the coarser one, up to `spread` bits: an A of the other sign that is far below B's last bit makes B - |A| round to B)
    const int sa = static_cast<int>(rng() % 20), sb = sa - static_cast<int>(rng() % (spread + 1));
    const int s = sa > sb ? sa : sb;
    const I IA = static_cast<I>(ia) << (s - sa), IB = static_cast<I>(ib) << (s - sb);
    const double A = scaled(ia, sa), B = scaled(ib, sb);
    // truncated remainder: exact and always representable
    const I t = IA % IB;
    EXPECT_EQ(rem_of(A, B), std::ldexp(static_cast<double>(t), -s)) << A << " " << B;
    // floor modulus: the sign of B, below |B|; the exact value rounded once, and when that rounding reaches |B| the double next to it
    I f = t;
    if (f != 0 && ((f < 0) != (IB < 0))) {
      f += IB;
    }
    double expect = std::ldexp(static_cast<double>(f), -s);  // (the conversion rounds to nearest)
    const double result = mod_of(A, B);
    if (f != 0 && std::fabs(expect) >= std::fabs(B)) {
      expect = toward_zero(B);
      reached_b++;
    }
    if (f == 0) {
      EXPECT_EQ(result, 0.0);
      EXPECT_EQ(std::signbit(result), std::signbit(B));
    }
    else {
      EXPECT_EQ(result, expect) << A << " " << B;
    }
    EXPECT_LT(std::fabs(result), std::fabs(B)) << A << " " << B;
    if (result != 0.0) {
      EXPECT_EQ(std::signbit(result), std::signbit(B)) << A << " " << B;
    }
  }
  return reached_b;
}

}  // namespace

TEST(CssMathModRem, RandomOperandsAgreeWithExactIntegerArithmetic) {
  // 64-bit integers: operands of up to 40 bits and scales a few bits apart (every result is representable, so the comparison is exact)
  mod_rem_against_exact<int64_t>(0x6d6f6421, 100000, 40, 12);
#ifdef __SIZEOF_INT128__
  // 128-bit integers: 53-bit operands with scales far apart, where the exact result is not a double and the rounding reaches |B|
  __extension__ typedef __int128 i128;
  const size_t reached = mod_rem_against_exact<i128>(0x6d6f6422, 200000, 53, 56);
  EXPECT_GT(reached, 0u) << "the random operands never reached the case the test is about";
#endif
}

TEST(CssMathModRem, OperandsJustBesideTheBoundaryOfTheModulus) {
  for (double b : {1.0, 3.0, 0.1, 1e10, 1e-10, 12345.678}) {
    for (double tiny : {1e-300, 1e-40, 1e-20, 1e-17, 1e-16, 1e-10}) {
      for (double sign : {1.0, -1.0}) {
        if (tiny >= b) {
          continue;
        }
        const double B = sign * b;
        const double r = mod_of(-sign * tiny, B);  // a tiny operand of the other sign
        EXPECT_LT(std::fabs(r), std::fabs(B)) << tiny << " " << B;
        EXPECT_EQ(std::signbit(r), std::signbit(B)) << tiny << " " << B;
        const double q = mod_of(sign * tiny, B);  // a tiny operand of the sign of the modulus is itself
        EXPECT_EQ(q, sign * tiny);
      }
    }
  }
}

// ---- 25: the legacy alpha -----------------------------------------------------------------------------------------

TEST(CssColorAlpha, AnEightBitAlphaIsWrittenByTheTwoStepsOfTheDraft) {
  // every 8-bit alpha: with an integer p of 0 to 100 whose p * 2.55 rounds to it, p / 100; otherwise alpha / 0.255 rounded, over 1000
  for (int a = 1; a < 255; a++) {
    char hex[16];
    std::snprintf(hex, sizeof hex, "#ff0000%02x", a);
    int p_found = -1;
    for (int p = 0; p <= 100; p++) {
      if (std::floor(p * 2.55 + 0.5 + 1e-9) == a) {
        p_found = p;
        break;
      }
    }
    if (p_found >= 0) {
      // p / 100 as a number: shortest, at most six decimals
      const std::string want = std::string("rgba(255, 0, 0, ") + (p_found == 100 ? "1" : "0." + std::string(p_found < 10 ? "0" : "") + std::to_string(p_found)) + ")";
      std::string w = want;
      while (w.size() > 3 && w[w.size() - 2] == '0' && w.find('.') != std::string::npos && w.back() == ')') {
        w.erase(w.size() - 2, 1);
      }
      EXPECT_EQ(gc::color(hex), w) << hex;
    }
    else {
      const long thousandths = static_cast<long>(std::floor(a / 0.255 + 0.5));
      char want[40];
      std::snprintf(want, sizeof want, "rgba(255, 0, 0, %ld.%03ld)", thousandths / 1000, thousandths % 1000);
      std::string w = want;
      while (w[w.size() - 2] == '0') {
        w.erase(w.size() - 2, 1);
      }
      if (w[w.size() - 2] == '.') {
        w.erase(w.size() - 2, 1);
      }
      EXPECT_EQ(gc::color(hex), w) << hex;
    }
  }
  EXPECT_EQ(gc::color("#ff0000ec"), "rgba(255, 0, 0, 0.925)");
  EXPECT_EQ(gc::color("#ff0000ed"), "rgba(255, 0, 0, 0.93)");
}

// ---- 12: what each substitution function needs ----------------------------------------------------------------------

TEST(CssSubstitutionNeeds, TheFunctionsThatAreMathOrBuildFromTheirArgumentsNeedNothingThemselves) {
  // Values 5: progress() is a math function ("It is a math function, and can be input into other calculations") with literal arguments,
  // ident() builds an identifier from its arguments, and first-valid() picks by the validity of its arguments for the property it is in
  struct Row {
    GTEXT_CSS_Substitution_Kind kind;
    GTEXT_CSS_Needs needs;
  };
  for (const Row & r : std::vector<Row>{{GTEXT_CSS_SUBST_PROGRESS, GTEXT_CSS_NEEDS_NOTHING}, {GTEXT_CSS_SUBST_IDENT, GTEXT_CSS_NEEDS_NOTHING},
           {GTEXT_CSS_SUBST_FIRST_VALID, GTEXT_CSS_NEEDS_PROPERTY}, {GTEXT_CSS_SUBST_SIBLING_INDEX, GTEXT_CSS_NEEDS_ELEMENT},
           {GTEXT_CSS_SUBST_SIBLING_COUNT, GTEXT_CSS_NEEDS_ELEMENT}, {GTEXT_CSS_SUBST_RANDOM, GTEXT_CSS_NEEDS_ELEMENT},
           {GTEXT_CSS_SUBST_VAR, GTEXT_CSS_NEEDS_VARIABLES}, {GTEXT_CSS_SUBST_ENV, GTEXT_CSS_NEEDS_ENVIRONMENT},
           {GTEXT_CSS_SUBST_CALC_SIZE, GTEXT_CSS_NEEDS_LAYOUT}}) {
    EXPECT_EQ(gtext_css_substitution_needs(r.kind), r.needs) << r.kind;
  }
}

// ---- 26: names written as identifiers --------------------------------------------------------------------------------

namespace {

/// The text of a color, read again: valid, and written as the same text.
void color_round_trips(const std::string & text) {
  gc::Color a;
  gc::read(a, text);
  ASSERT_EQ(a.status, GTEXT_CSS_OK) << text;
  ASSERT_TRUE(a.valid) << text;
  const std::string once = gc::serialize(a.color);
  gc::Color b;
  gc::read(b, once);
  ASSERT_EQ(b.status, GTEXT_CSS_OK) << text << " -> " << once;
  ASSERT_TRUE(b.valid) << text << " was written as " << once << ", which is not a color";
  EXPECT_EQ(gc::serialize(b.color), once) << text;
}

}  // namespace

TEST(CssColorNames, ACustomSpaceWithACharacterThatNeedsAnEscapeIsWrittenWithIt) {
  // the minimized find of the fuzzer: `color(--\  3` is the custom space "-- " (an escaped space) and a channel
  EXPECT_EQ(gc::color("color(--\\  3)"), "color(--\\  3)");
  for (const char * name : {"--a\\ b", "--\\31 23", "--a\\;b", "--a\\(b", "--a\\,b", "--\\ ", "--\xc3\xa9", "--a\\/b", "--a\\\"b"}) {
    color_round_trips(std::string("color(") + name + " 1 2 3)");
    color_round_trips(std::string("color-mix(in ") + name + ", red, blue)");
  }
}

TEST(CssMathNames, ARandomKeyWithACharacterThatNeedsAnEscapeIsWrittenWithIt) {
  for (const char * key : {"--a\\ b", "--a\\;b", "--\\31 23"}) {
    const std::string text = std::string("random(") + key + " element-scoped, 1px, 2px)";
    const std::string once = gm::calc(text);
    ASSERT_NE(once, "invalid") << text;
    EXPECT_EQ(gm::calc(once), once) << text << " -> " << once;
  }
}

TEST(CssValueNames, TheNamesOfTheFunctionFamiliesAreWrittenAsIdentifiersToo) {
  // (the value serializer writes every identifier through CSSOM's rule: paint(), element(), counters ...)
  for (const auto & row : std::vector<std::pair<const char *, const char *>>{{"<image>", "paint(a\\ b, 1)"}, {"<image>", "element(#a\\ b)"},
           {"<basic-shape>", "shape(from 0 0, line to 1px 1px)"}}) {
    One o;
    run_into(o, row.first, row.second);
    if (!o.m.matched) {
      continue;  // (not every spelling is in the grammar)
    }
    const std::string once = serialize(o.m.root());
    One again;
    run_into(again, row.first, once);
    ASSERT_TRUE(again.m.matched) << row.second << " -> " << once;
    EXPECT_EQ(serialize(again.m.root()), once) << row.second;
  }
}
