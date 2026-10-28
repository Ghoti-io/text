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
 * Computed values (css_computed.h), each test named for the rule it holds. The first is the I/O matrix of the slice; then each value type
 * (lengths, angles, times, calculations, colors, URLs), each property algorithm of tools/css/computed_rules.tsv with the sentence it is
 * built from, defaulting, the order of computation, sharing and immutability, the limits, and the gates that hold the classification.
 */

#include <cmath>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_compute_test_support.h"

using namespace cmp;

namespace {

struct Case {
  const char * parent;
  const char * decls;
  const char * property;
  const char * expected;
};

void run_cases(const std::vector<Case> & cases, const GTEXT_CSS_Environment * env = nullptr) {
  for (const Case & c : cases) {
    EXPECT_EQ(computed(c.parent, c.decls, c.property, env), c.expected) << "parent {" << c.parent << "} child {" << c.decls << "} " << c.property;
  }
}

/* ---- the matrix of the slice ---------------------------------------------------------------------------------- */

TEST(CssCompute, TheMatrixOfTheSlice) {
  run_cases({
    {"font-size: 20px", "width: 2em", "width", "40px"},                           // a relative length against the computed font-size
    {"", "width: 50%", "width", "50%"},                                           // a percentage is kept
    {"color: red", "color: unset", "color", "red"},                               // unset inherits an inherited property
    {"", "width: 10px", "margin-left", "0px"},                                    // and takes the initial value otherwise
    {"", "border-style: none; border-width: 5px", "border-top-width", "0px"},     // a border width under none
    {"", "border-style: solid; border-width: 5px", "border-top-width", "5px"},
    {"", "margin: 1px 2px", "margin-bottom", "1px"},                              // a shorthand is four longhands
    {"", "margin: 1px 2px", "margin-left", "2px"},
  });
  // the logical row: margin-inline-start in vertical-rl is the top
  EXPECT_EQ(computed("", "writing-mode: vertical-rl; margin-inline-start: 4px", "margin-top"), "4px");
  EXPECT_EQ(computed("", "writing-mode: vertical-rl; margin-inline-start: 4px", "margin-left"), "0px");
  // re-serialize: four equal longhands are one shorthand
  Block b(R(), "margin-top: 1px; margin-right: 1px; margin-bottom: 1px; margin-left: 1px");
  EXPECT_EQ(b.css(), "margin: 1px;");
  // a missing metric
  Context ctx(R());
  Block pb(R(), "");
  Block cb(R(), "width: 2ex");
  Style parent, child;
  parent.compute(ctx, &pb);
  child.compute(ctx, &cb, &parent);
  EXPECT_EQ(child.status, GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_STREQ(child.result.name, "ex") << "the unit that needed the callback";
  EXPECT_EQ(child.s, nullptr) << "no partial value";
}

/* ---- inheritance through an inheritance parent -------------------------------------------------------------------- */

struct Doc {
  std::vector<int> parent, inherit_from;
  std::vector<std::string> decls;
  std::vector<Block *> blocks;
  ~Doc() {
    for (Block * b : blocks) {
      delete b;
    }
  }
};

int E(GTEXT_CSS_Element_Handle h) {
  return static_cast<int>(reinterpret_cast<intptr_t>(h));
}

GTEXT_CSS_Element_Handle H(int i) {
  return reinterpret_cast<GTEXT_CSS_Element_Handle>(static_cast<intptr_t>(i));
}

GTEXT_CSS_Element_Interface table_for(Doc & d, bool with_inheritance_parent) {
  GTEXT_CSS_Element_Interface t;
  std::memset(&t, 0, sizeof t);
  t.size = sizeof t;
  t.user = &d;
  t.parent = [](void * u, GTEXT_CSS_Element_Handle e) { return H(static_cast<Doc *>(u)->parent[E(e)]); };
  if (with_inheritance_parent) {
    t.inheritance_parent = [](void * u, GTEXT_CSS_Element_Handle e) { return H(static_cast<Doc *>(u)->inherit_from[E(e)]); };
  }
  return t;
}

GTEXT_CSS_Declaration_Source source_for(Doc & d) {
  GTEXT_CSS_Declaration_Source s;
  s.user = &d;
  s.declarations = [](void * u, GTEXT_CSS_Element_Handle e, const GTEXT_CSS_Declaration_Block ** out) {
    Doc * doc = static_cast<Doc *>(u);
    *out = doc->blocks[E(e)] ? doc->blocks[E(e)]->b : nullptr;
    return GTEXT_CSS_OK;
  };
  return s;
}

TEST(CssCompute, AnElementInheritsFromItsInheritanceParentNotItsTreeParent) {
  // 1 is the root, 2 a child of it with a font-size of its own, 3 a `<use>` clone: its tree parent is 2 and its inheritance parent is 1
  Doc d;
  d.parent = {0, 0, 1, 2};
  d.inherit_from = {0, 0, 1, 1};
  d.blocks = {nullptr, new Block(R(), "font-size: 10px; color: red"), new Block(R(), "font-size: 99px; color: blue"), new Block(R(), "width: 2em")};
  Context ctx(R());
  ASSERT_EQ(ctx.status, GTEXT_CSS_OK);
  GTEXT_CSS_Declaration_Source src = source_for(d);
  {
    GTEXT_CSS_Element_Interface t = table_for(d, true);
    GTEXT_CSS_Computed_Style * s = nullptr;
    ASSERT_EQ(gtext_css_compute_element(ctx.c, &t, &src, H(3), &s, nullptr), GTEXT_CSS_OK);
    Style out;
    out.s = s;
    EXPECT_EQ(out.value("font-size"), "10px") << "from the root, not from the tree parent";
    EXPECT_EQ(out.value("width"), "20px");
    EXPECT_EQ(out.value("color"), "red");
    out.s = nullptr;
    gtext_css_computed_free(s);
  }
  {
    GTEXT_CSS_Element_Interface t = table_for(d, false);   // no inheritance_parent: the parent
    GTEXT_CSS_Computed_Style * s = nullptr;
    ASSERT_EQ(gtext_css_compute_element(ctx.c, &t, &src, H(3), &s, nullptr), GTEXT_CSS_OK);
    Style out;
    out.s = s;
    EXPECT_EQ(out.value("font-size"), "99px");
    EXPECT_EQ(out.value("width"), "198px");
    EXPECT_EQ(out.value("color"), "blue");
    out.s = nullptr;
    gtext_css_computed_free(s);
  }
  {
    // neither callback
    GTEXT_CSS_Element_Interface t;
    std::memset(&t, 0, sizeof t);
    t.size = sizeof t;
    GTEXT_CSS_Computed_Style * s = nullptr;
    GTEXT_CSS_Compute_Result r;
    EXPECT_EQ(gtext_css_compute_element(ctx.c, &t, &src, H(3), &s, &r), GTEXT_CSS_E_UNSUPPORTED);
    EXPECT_STREQ(r.name, "parent");
    EXPECT_EQ(s, nullptr);
  }
}

/* ---- by value type ----------------------------------------------------------------------------------------------- */

TEST(CssCompute, AbsoluteLengthsAreInPixels) {
  run_cases({
    {"", "width: 1in", "width", "96px"},
    {"", "width: 2.54cm", "width", "96px"},
    {"", "width: 25.4mm", "width", "96px"},
    {"", "width: 101.6Q", "width", "96px"},
    {"", "width: 72pt", "width", "96px"},
    {"", "width: 6pc", "width", "96px"},
    {"", "width: 10px", "width", "10px"},
    {"", "width: 0", "width", "0px"},
  });
}

TEST(CssCompute, FontRelativeLengthsAreAgainstTheFontSizeOfTheElementAndRemAgainstTheRoot) {
  run_cases({
    {"font-size: 10px", "font-size: 20px; width: 2em", "width", "40px"},          // the element's own size, not the parent's
    {"font-size: 10px", "font-size: 2em", "font-size", "20px"},                    // font-size itself: the parent's
    {"font-size: 10px", "font-size: 150%", "font-size", "15px"},
    {"font-size: 10px", "width: 3rem", "width", "30px"},                           // the parent here is the root: rem is its 10px
  });
  // the root element: rem is its own font-size (but in font-size itself, the initial one)
  EXPECT_EQ(root_computed("font-size: 20px; width: 2rem", "width"), "40px");
  EXPECT_EQ(root_computed("font-size: 2rem", "font-size"), "32px") << "rem in the root's font-size is the initial font size";
  // a deeper element: rem is still the root's
  Context ctx(R());
  Block rb(R(), "font-size: 10px"), mb(R(), "font-size: 40px"), lb(R(), "width: 2rem; padding-top: 2em");
  Style root, mid, leaf;
  root.compute(ctx, &rb);
  mid.compute(ctx, &mb, &root);
  leaf.compute(ctx, &lb, &mid);
  EXPECT_EQ(leaf.value("width"), "20px");
  EXPECT_EQ(leaf.value("padding-top"), "80px");
}

TEST(CssCompute, ViewportLengthsAreAgainstTheEnvironmentSmallLargeAndDynamic) {
  GTEXT_CSS_Environment env = gtext_css_environment_default();
  env.small_viewport.width = 300;
  env.small_viewport.height = 200;
  env.large_viewport.width = 500;
  env.large_viewport.height = 400;
  env.dynamic_viewport.width = 400;
  env.dynamic_viewport.height = 300;
  run_cases({
    {"", "width: 10vw", "width", "50px"},
    {"", "width: 10lvw", "width", "50px"},
    {"", "width: 10svw", "width", "30px"},
    {"", "width: 10dvw", "width", "40px"},
    {"", "height: 10vh", "height", "40px"},
    {"", "height: 10svh", "height", "20px"},
    {"", "height: 10dvh", "height", "30px"},
    {"", "width: 10vmin", "width", "40px"},
    {"", "width: 10vmax", "width", "50px"},
    {"", "width: 10svmin", "width", "20px"},
    {"", "width: 10dvmax", "width", "40px"},
    {"", "width: 10vi", "width", "50px"},                                          // the inline axis of the root: horizontal
    {"", "width: 10vb", "width", "40px"},
  }, &env);
  // a vertical root writing mode swaps vi and vb
  EXPECT_EQ(root_computed("writing-mode: vertical-rl; width: 10vi", "width", &env), "40px");
  EXPECT_EQ(root_computed("writing-mode: vertical-rl; width: 10vb", "width", &env), "50px");
}

TEST(CssCompute, AnglesAreInDegreesTimesInSecondsFrequenciesInHertzAndResolutionsInDppx) {
  run_cases({
    {"", "rotate: 0.5turn", "rotate", "0 0 1 180deg"},
    {"", "rotate: 100grad", "rotate", "0 0 1 90deg"},
    {"", "rotate: 3.141592653589793rad", "rotate", "0 0 1 180deg"},
    {"", "transition-duration: 500ms", "transition-duration", "0.5s"},
    {"", "transition-delay: 2s, 250ms", "transition-delay", "2s, 0.25s"},
    {"", "image-resolution: 2x", "image-resolution", "2dppx"},
    {"", "image-resolution: 96dpi", "image-resolution", "1dppx"},
  });
}

TEST(CssCompute, ACalculationIsSimplifiedAndOneThatResolvesToAValueIsThatValue) {
  run_cases({
    {"font-size: 10px", "width: calc(10px + 2em)", "width", "30px"},
    {"font-size: 10px", "width: calc(10% + 2em)", "width", "calc(10% + 20px)"},    // a percentage keeps it a calculation
    {"", "width: calc(1px + 2px)", "width", "3px"},
    {"", "width: calc(100% - 100%)", "width", "0%"},                                 // a percentage that is one value is that value
    {"", "padding-top: calc(-5px)", "padding-top", "0px"},                          // clamped to the range of the slot [0, inf)
    {"", "z-index: calc(1.5)", "z-index", "2"},                                      // an integer slot rounds
    {"", "z-index: calc(2.5)", "z-index", "3"},
    {"", "opacity: calc(150%)", "opacity", "1"},                                     // a number clamped to [0,1]
    {"", "margin-left: min(10px, 2em)", "margin-left", "10px"},
    {"font-size: 3px", "margin-left: max(10px, 2em)", "margin-left", "10px"},
    {"font-size: 10px", "margin-left: clamp(1px, 3em, 20px)", "margin-left", "20px"},
  });
}

TEST(CssCompute, AColorHasLightDarkResolvedFromTheSchemeAndCurrentcolorKept) {
  GTEXT_CSS_Environment dark = gtext_css_environment_default();
  dark.color_scheme = GTEXT_CSS_SCHEME_DARK;
  EXPECT_EQ(computed("", "color: light-dark(red, blue)", "color"), "red");
  EXPECT_EQ(computed("", "color: light-dark(red, blue)", "color", &dark), "red") << "the page declares no scheme: the user agent default, whatever the preference";
  EXPECT_EQ(computed("color-scheme: light dark", "color: light-dark(red, blue)", "color", &dark), "blue");
  EXPECT_EQ(computed("color-scheme: light dark", "background-color: light-dark(red, light-dark(green, blue))", "background-color", &dark), "blue");
  EXPECT_EQ(computed("", "background-color: currentcolor", "background-color"), "currentcolor");
  EXPECT_EQ(computed("color: green", "color: currentcolor", "color"), "green") << "color: currentcolor is inherit";
  EXPECT_EQ(computed("", "background-color: rgb(1 2 3)", "background-color"), "rgb(1, 2, 3)");
}

TEST(CssCompute, AColorTheLibraryCannotComputeWithoutConversionIsFlaggedOrHandedToTheCallback) {
  Context ctx(R());
  Block cb(R(), "background-color: color-mix(in srgb, red, blue)");
  Style s;
  s.compute(ctx, &cb);
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  const GTEXT_CSS_Value * v = gtext_css_computed_root(s.s, find(R(), "background-color"));
  ASSERT_NE(v, nullptr);
  EXPECT_NE(v->flags & GTEXT_CSS_VFLAG_COLOR_UNRESOLVED, 0u) << "without the callback the color is returned as it was written, flagged";
  EXPECT_EQ(s.value("background-color"), "color-mix(in srgb, red, blue)");
  // with a callback it is a color
  GTEXT_CSS_Environment env = gtext_css_environment_default();
  env.resolve_color = [](void *, const GTEXT_CSS_Color *, GTEXT_CSS_Color_Scheme, double rgba[4]) {
    rgba[0] = 128; rgba[1] = 0; rgba[2] = 127; rgba[3] = 1;
    return true;
  };
  EXPECT_EQ(computed("", "background-color: color-mix(in srgb, red, blue)", "background-color", &env), "rgb(128, 0, 127)");
  // a system color is the callback's too, and flagged without
  Style sys;
  Block sb(R(), "background-color: Canvas");
  sys.compute(ctx, &sb);
  const GTEXT_CSS_Value * sv = gtext_css_computed_root(sys.s, find(R(), "background-color"));
  ASSERT_NE(sv, nullptr);
  EXPECT_NE(sv->flags & GTEXT_CSS_VFLAG_COLOR_UNRESOLVED, 0u);
  // a color the callback refuses stays as written
  env.resolve_color = [](void *, const GTEXT_CSS_Color *, GTEXT_CSS_Color_Scheme, double[4]) { return false; };
  Context ctx2(R(), &env);
  Style s2;
  s2.compute(ctx2, &cb);
  const GTEXT_CSS_Value * v2 = gtext_css_computed_root(s2.s, find(R(), "background-color"));
  ASSERT_NE(v2, nullptr);
  EXPECT_NE(v2->flags & GTEXT_CSS_VFLAG_COLOR_UNRESOLVED, 0u);
}

TEST(CssCompute, ARelativeUrlIsResolvedAgainstTheBaseAndAFragmentIsNot) {
  GTEXT_CSS_Environment env = gtext_css_environment_default();
  env.base_url = "http://example.com/a/b/c.css?q#f";
  run_cases({
    {"", "background-image: url(d.png)", "background-image", "url(\"http://example.com/a/b/d.png\")"},
    {"", "background-image: url(../d.png)", "background-image", "url(\"http://example.com/a/d.png\")"},
    {"", "background-image: url(/d.png)", "background-image", "url(\"http://example.com/d.png\")"},
    {"", "background-image: url(//other.org/d.png)", "background-image", "url(\"http://other.org/d.png\")"},
    {"", "background-image: url(?x=1)", "background-image", "url(\"http://example.com/a/b/c.css?x=1\")"},
    {"", "background-image: url(https://x.org/y.png)", "background-image", "url(\"https://x.org/y.png\")"},
    {"", "background-image: url(#frag)", "background-image", "url(\"#frag\")"},
    {"", "background-image: url(./e/../f.png)", "background-image", "url(\"http://example.com/a/b/f.png\")"},
    {"", "background-image: url(../../../../g.png)", "background-image", "url(\"http://example.com/g.png\")"},
  }, &env);
  // no base: the URL stays
  EXPECT_EQ(computed("", "background-image: url(d.png)", "background-image"), "url(\"d.png\")");
  // a resolver of the caller's wins
  env.resolve_url = [](void *, const char *, size_t, const char *, size_t, GTEXT_CSS_String * out) {
    out->data = "x://resolved";
    out->length = 12;
    return true;
  };
  EXPECT_EQ(computed("", "background-image: url(d.png)", "background-image", &env), "url(\"x://resolved\")");
}

/* ---- the callbacks of the environment ---------------------------------------------------------------------------------- */

struct Metrics {
  std::vector<GTEXT_CSS_Font_Query> queries;
  std::vector<GTEXT_CSS_Font_Metric> asked;
  bool refuse = false;
};

GTEXT_CSS_Environment env_with_metrics(Metrics & m) {
  GTEXT_CSS_Environment env = gtext_css_environment_default();
  env.user = &m;
  env.font_metric = [](void * u, const GTEXT_CSS_Font_Query * q, GTEXT_CSS_Font_Metric metric, double * px) {
    Metrics * mm = static_cast<Metrics *>(u);
    mm->queries.push_back(*q);
    mm->asked.push_back(metric);
    if (mm->refuse) {
      return false;
    }
    switch (metric) {
    case GTEXT_CSS_METRIC_EX: *px = q->size_px * 0.5; break;
    case GTEXT_CSS_METRIC_CH: *px = q->size_px * 0.6; break;
    case GTEXT_CSS_METRIC_CAP: *px = q->size_px * 0.7; break;
    default: *px = q->size_px; break;
    }
    return true;
  };
  env.normal_line_height = [](void *, const GTEXT_CSS_Font_Query * q, double * px) {
    *px = q->size_px * 1.2;
    return true;
  };
  return env;
}

TEST(CssCompute, FontMetricsAndTheNormalLineHeightComeFromTheEnvironmentsCallbacks) {
  Metrics m;
  GTEXT_CSS_Environment env = env_with_metrics(m);
  EXPECT_EQ(computed("font-size: 20px", "width: 3ex", "width", &env), "30px");
  EXPECT_EQ(computed("font-size: 20px", "width: 2ch", "width", &env), "24px");
  EXPECT_EQ(computed("font-size: 20px", "width: 1cap", "width", &env), "14px");
  EXPECT_EQ(computed("font-size: 20px", "width: 1ic", "width", &env), "20px");
  EXPECT_EQ(computed("font-size: 10px", "font-size: 20px; width: 2rex", "width", &env), "10px") << "rex: the x-height of the root's font";
  ASSERT_FALSE(m.queries.empty());
  EXPECT_EQ(m.asked.front(), GTEXT_CSS_METRIC_EX);
  EXPECT_EQ(m.queries.front().size_px, 20.0);
  // lh and rlh: normal goes to the callback, a number or a length does not
  EXPECT_EQ(computed("font-size: 10px", "width: 2lh", "width", &env), "24px");
  EXPECT_EQ(computed("font-size: 10px", "line-height: 2; width: 3lh", "width", &env), "60px");
  EXPECT_EQ(computed("font-size: 10px; line-height: 30px", "font-size: 20px; line-height: 5px; width: 1rlh", "width", &env), "30px");
  // a callback that gives no metric is unsupported, naming the unit
  m.refuse = true;
  Context ctx(R(), &env);
  Block cb(R(), "width: 2ex");
  Style s;
  s.compute(ctx, &cb);
  EXPECT_EQ(s.status, GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_STREQ(s.result.name, "ex");
}

TEST(CssCompute, ALengthThatNeedsACallbackTheEnvironmentLacksIsUnsupportedAndNamesTheUnit) {
  const char * units[] = {"ex", "ch", "cap", "ic", "rex", "rch", "rcap", "ric", "lh", "rlh", "cqw", "cqh", "cqi", "cqb", "cqmin", "cqmax"};
  for (const char * unit : units) {
    Context ctx(R());
    Block cb(R(), std::string("width: 1") + unit);
    ASSERT_TRUE(cb.rejected.empty()) << unit;
    Style s;
    s.compute(ctx, &cb);
    EXPECT_EQ(s.status, GTEXT_CSS_E_UNSUPPORTED) << unit;
    EXPECT_STREQ(s.result.name, unit);
    EXPECT_EQ(s.s, nullptr);
  }
}

TEST(CssCompute, ContainerLengthsComeFromTheCallbackAndFallBackToTheSmallViewportWhereThereIsNoContainer) {
  struct Containers { bool have = true; } cont;
  GTEXT_CSS_Environment env = gtext_css_environment_default();
  env.small_viewport.width = 200;
  env.small_viewport.height = 100;
  env.user = &cont;
  env.container_size = [](void * u, GTEXT_CSS_Element_Handle, GTEXT_CSS_Container_Axis axis, double * px) {
    if (!static_cast<Containers *>(u)->have) {
      return false;
    }
    switch (axis) {
    case GTEXT_CSS_CONTAINER_WIDTH: *px = 500; break;
    case GTEXT_CSS_CONTAINER_HEIGHT: *px = 300; break;
    case GTEXT_CSS_CONTAINER_INLINE: *px = 500; break;
    default: *px = 300; break;
    }
    return true;
  };
  run_cases({
    {"", "width: 10cqw", "width", "50px"},
    {"", "width: 10cqh", "width", "30px"},
    {"", "width: 10cqi", "width", "50px"},
    {"", "width: 10cqb", "width", "30px"},
    {"", "width: 10cqmin", "width", "30px"},
    {"", "width: 10cqmax", "width", "50px"},
  }, &env);
  cont.have = false;
  run_cases({
    {"", "width: 10cqw", "width", "20px"},
    {"", "width: 10cqh", "width", "10px"},
  }, &env);
}

/* ---- the algorithms ------------------------------------------------------------------------------------------------ */

TEST(CssCompute, FontSizeKeywordsLargerSmallerAndPercentagesFollowTheTable) {
  run_cases({
    {"", "font-size: medium", "font-size", "16px"},
    {"", "font-size: xx-small", "font-size", "9.6px"},
    {"", "font-size: x-small", "font-size", "12px"},
    {"", "font-size: small", "font-size", "14.222222px"},
    {"", "font-size: large", "font-size", "19.2px"},
    {"", "font-size: x-large", "font-size", "24px"},
    {"", "font-size: xx-large", "font-size", "32px"},
    {"", "font-size: xxx-large", "font-size", "48px"},
    {"font-size: 20px", "font-size: larger", "font-size", "24px"},
    {"font-size: 24px", "font-size: smaller", "font-size", "20px"},
    {"font-size: 20px", "font-size: 50%", "font-size", "10px"},
    {"font-size: 20px", "font-size: calc(50% + 2px)", "font-size", "12px"},
    {"font-size: 20px", "font-size: 2em", "font-size", "40px"},
    {"font-size: 20px", "font-size: inherit", "font-size", "20px"},
    {"font-size: 20px", "", "font-size", "20px"},
  });
  GTEXT_CSS_Environment env = gtext_css_environment_default();
  env.default_font_size = 20;
  EXPECT_EQ(computed("", "font-size: medium", "font-size", &env), "20px") << "medium is the environment's default size";
  EXPECT_EQ(computed("", "font-size: large", "font-size", &env), "24px");
  EXPECT_EQ(computed("", "", "font-size", &env), "20px") << "the initial value of font-size is medium";
}

TEST(CssCompute, FontWeightBolderAndLighterFollowTheTableOfFonts4) {
  struct Row { int inherited; int bolder; int lighter; };
  const Row table[] = {{50, 400, 50}, {99, 400, 99}, {100, 400, 100}, {349, 400, 100}, {350, 700, 100}, {549, 700, 100}, {550, 900, 400},
      {749, 900, 400}, {750, 900, 700}, {899, 900, 700}, {900, 900, 700}, {1000, 1000, 700}};
  for (const Row & r : table) {
    const std::string parent = "font-weight: " + std::to_string(r.inherited);
    EXPECT_EQ(computed(parent, "font-weight: bolder", "font-weight"), std::to_string(r.bolder)) << "bolder from " << r.inherited;
    EXPECT_EQ(computed(parent, "font-weight: lighter", "font-weight"), std::to_string(r.lighter)) << "lighter from " << r.inherited;
  }
  run_cases({
    {"", "font-weight: normal", "font-weight", "400"},
    {"", "font-weight: bold", "font-weight", "700"},
    {"", "font-weight: 650", "font-weight", "650"},
    {"font-weight: bold", "", "font-weight", "700"},
    {"", "", "font-weight", "400"},
  });
}

TEST(CssCompute, FontWidthKeywordsArePercentagesAndFontStyleObliqueHasItsAngleInDegrees) {
  run_cases({
    {"", "font-width: ultra-condensed", "font-width", "50%"},
    {"", "font-width: extra-condensed", "font-width", "62.5%"},
    {"", "font-width: condensed", "font-width", "75%"},
    {"", "font-width: semi-condensed", "font-width", "87.5%"},
    {"", "font-width: normal", "font-width", "100%"},
    {"", "font-width: semi-expanded", "font-width", "112.5%"},
    {"", "font-width: expanded", "font-width", "125%"},
    {"", "font-width: extra-expanded", "font-width", "150%"},
    {"", "font-width: ultra-expanded", "font-width", "200%"},
    {"", "font-width: 130%", "font-width", "130%"},
    {"", "font-style: oblique", "font-style", "oblique"},
    {"", "font-style: oblique 20deg", "font-style", "oblique 20deg"},
    {"", "font-style: oblique 0.25turn", "font-style", "oblique 90deg"},
    {"", "font-style: italic", "font-style", "italic"},
    {"", "font-style: normal", "font-style", "normal"},
  });
}

TEST(CssCompute, LineHeightKeepsAKeywordAndANumberAndMakesALengthOfAPercentage) {
  run_cases({
    {"", "line-height: normal", "line-height", "normal"},
    {"", "line-height: 1.5", "line-height", "1.5"},
    {"", "line-height: 24px", "line-height", "24px"},
    {"font-size: 10px", "line-height: 2em", "line-height", "20px"},
    {"font-size: 10px", "line-height: 150%", "line-height", "15px"},
    {"font-size: 10px", "font-size: 20px; line-height: 150%", "line-height", "30px"},
    {"font-size: 10px; line-height: 1.4", "", "line-height", "1.4"},
    {"font-size: 10px; line-height: 150%", "font-size: 40px", "line-height", "15px"},                   // inherited as a length
  });
}

TEST(CssCompute, BorderWidthsAreKeywordsOrLengthsSnappedToDevicePixelsAndZeroUnderNoneOrHidden) {
  run_cases({
    {"", "border-top-style: solid; border-top-width: thin", "border-top-width", "1px"},
    {"", "border-top-style: solid; border-top-width: medium", "border-top-width", "3px"},
    {"", "border-top-style: solid; border-top-width: thick", "border-top-width", "5px"},
    {"", "border-top-style: dashed; border-top-width: 2.7px", "border-top-width", "2px"},
    {"", "border-top-style: dashed; border-top-width: 0.4px", "border-top-width", "1px"},
    {"", "border-top-style: dashed; border-top-width: 0", "border-top-width", "0px"},
    {"", "border-top-style: hidden; border-top-width: 5px", "border-top-width", "0px"},
    {"", "border-top-style: none; border-top-width: thick", "border-top-width", "0px"},
    {"", "border-left-width: 5px", "border-left-width", "0px"},                                    // the initial style is none
    {"", "outline-style: solid; outline-width: 4px", "outline-width", "4px"},
    {"", "outline-width: 4px", "outline-width", "0px"},
    {"", "border-style: solid; border-block-start-width: 6px", "border-top-width", "6px"},
    {"", "border-top-style: solid; border-top-width: 1em", "border-top-width", "16px"},
  });
  GTEXT_CSS_Environment env = gtext_css_environment_default();
  env.device_pixel_ratio = 2.0;
  run_cases({
    {"", "border-top-style: solid; border-top-width: 1.7px", "border-top-width", "1.5px"},     // 3.4 device px -> 3 -> 1.5px
    {"", "border-top-style: solid; border-top-width: 0.2px", "border-top-width", "0.5px"},     // less than a device pixel is one
    {"", "border-top-style: solid; border-top-width: 3px", "border-top-width", "3px"},
  }, &env);
}

TEST(CssCompute, DisplayIsBlockifiedUnderFloatPositionTheRootAndAFlexOrGridParent) {
  run_cases({
    {"", "display: inline", "display", "inline"},
    {"display: block", "display: inline", "display", "inline"},
    {"display: block", "display: inline-block", "display", "inline-block"},
    {"display: block", "display: inline; float: left", "display", "block"},
    // Display 3, 2.7: "For legacy reasons, if an inline block box (inline flow-root) is blockified, it becomes a block box (losing its flow-root nature)."
    {"display: block", "display: inline-block; float: right", "display", "block"},
    {"display: block", "display: inline-block; position: absolute", "display", "block"},
    {"display: block", "display: inline flow-root; position: fixed", "display", "block"},
    {"display: block", "display: flow-root; float: left", "display", "flow-root"},
    {"display: block", "display: inline-flex; position: absolute", "display", "flex"},
    {"display: block", "display: inline-table; position: fixed", "display", "table"},
    {"display: block", "display: inline-grid; float: left", "display", "grid"},
    {"display: block", "display: inline; position: relative", "display", "inline"},
    {"display: flex", "display: inline", "display", "block"},
    {"display: inline-flex", "display: inline-block", "display", "block"},
    // "sets the box's computed outer display type to block": the inner type stays, and `math` is one
    {"display: block", "display: inline math; float: left", "display", "block math"},
    {"display: block", "display: inline math; position: absolute", "display", "block math"},
    {"display: block", "display: inline math", "display", "inline math"},
    {"display: block", "display: math", "display", "block math"},
    // "If a layout-internal box is blockified, its inner display type converts to flow so that it becomes a block container."
    {"display: block", "display: table-cell; float: left", "display", "block"},
    {"display: block", "display: table-row; position: absolute", "display", "block"},
    {"display: flex", "display: table-caption", "display", "block"},
    {"display: block", "display: ruby-text; float: right", "display", "block"},
    {"display: block", "display: table-cell", "display", "table-cell"},
    // CSS Positioned Layout 3: "A position value of absolute or fixed blockifies the box, causes float to compute to none, ..."
    {"", "position: absolute; float: left", "float", "none"},
    {"", "position: fixed; float: right", "float", "none"},
    {"", "position: relative; float: right", "float", "right"},
    {"", "position: sticky; float: left", "float", "left"},
    {"", "float: left; position: absolute", "float", "none"},
    {"", "position: absolute; float: inline-start", "float", "none"},
    {"", "position: absolute; float: left; wrap-flow: both", "wrap-flow", "both"},
    {"", "float: left", "wrap-flow", "auto"},
    {"display: grid", "display: inline", "display", "block"},
    {"display: block", "display: none; float: left", "display", "none"},
    {"display: block", "display: contents; float: left", "display", "contents"},
    {"display: block", "display: inline list-item; float: left", "display", "list-item"},
    {"display: block", "display: inline flow; float: left", "display", "block"},
    {"display: block", "display: block ruby", "display", "block ruby"},
    {"display: block", "display: ruby", "display", "ruby"},
    {"display: block", "display: run-in", "display", "run-in"},
  });
  EXPECT_EQ(root_computed("display: inline", "display"), "block") << "the root element is blockified";
  EXPECT_EQ(root_computed("display: inline-flex", "display"), "flex");
  EXPECT_EQ(root_computed("", "display"), "block") << "the initial display of the root: inline, blockified";
}

TEST(CssCompute, ColorCurrentcolorIsInheritAndOpacityIsANumberClampedToZeroToOne) {
  run_cases({
    {"color: red", "color: currentcolor", "color", "red"},
    {"", "color: currentcolor", "color", "canvastext"},
    {"", "opacity: 50%", "opacity", "0.5"},
    {"", "opacity: 2", "opacity", "1"},
    {"", "opacity: -1", "opacity", "0"},
    {"", "opacity: 0.25", "opacity", "0.25"},
    {"", "fill-opacity: 40%", "fill-opacity", "0.4"},
    {"", "stroke-opacity: 7", "stroke-opacity", "1"},
    {"", "flood-opacity: 25%", "flood-opacity", "0.25"},
    {"", "stop-opacity: 70%", "stop-opacity", "0.7"},
    {"", "", "stop-opacity", "1"},
    {"", "", "stop-color", "black"},
    {"", "zoom: 150%", "zoom", "1.5"},
    {"", "zoom: 2", "zoom", "2"},
  });
}

TEST(CssCompute, MathDepthAutoAddAndAddFollowTheParentAndItsMathStyle) {
  run_cases({
    {"math-depth: 2", "math-depth: auto-add", "math-depth", "2"},
    {"math-depth: 2; math-style: compact", "math-depth: auto-add", "math-depth", "3"},
    {"math-depth: 2", "math-depth: add(3)", "math-depth", "5"},
    {"math-depth: 2", "math-depth: add(-5)", "math-depth", "-3"},
    {"math-depth: 2", "math-depth: 7", "math-depth", "7"},
    {"math-depth: 2", "", "math-depth", "2"},
    {"", "", "math-depth", "0"},
  });
}

TEST(CssCompute, TextAlignMatchParentTakesTheParentsValueAndMakesStartAndEndLeftAndRightByTheParentsDirection) {
  run_cases({
    {"text-align-all: center", "text-align-all: match-parent", "text-align-all", "center"},
    {"text-align-all: start", "text-align-all: match-parent", "text-align-all", "left"},
    {"text-align-all: end", "text-align-all: match-parent", "text-align-all", "right"},
    {"direction: rtl; text-align-all: start", "text-align-all: match-parent", "text-align-all", "right"},
    {"direction: rtl; text-align-all: end", "text-align-all: match-parent", "text-align-all", "left"},
    {"text-align-last: justify", "text-align-last: match-parent", "text-align-last", "justify"},
    {"", "text-align-last: end", "text-align-last", "end"},
  });
  EXPECT_EQ(root_computed("text-align-all: match-parent", "text-align-all"), "start") << "no parent: start";
}

TEST(CssCompute, VoiceBalanceIsANumberBetweenMinus100And100AndPitchNeedsATableTheEnvironmentLacks) {
  run_cases({
    {"", "voice-balance: left", "voice-balance", "-100"},
    {"", "voice-balance: right", "voice-balance", "100"},
    {"", "voice-balance: center", "voice-balance", "0"},
    {"", "voice-balance: 500", "voice-balance", "100"},
    {"", "voice-balance: -30", "voice-balance", "-30"},
    {"voice-balance: 10", "voice-balance: leftwards", "voice-balance", "-10"},
    {"voice-balance: 90", "voice-balance: rightwards", "voice-balance", "100"},
  });
}

TEST(CssCompute, ListsAndPairsAreFilledToTheirComputedShape) {
  run_cases({
    {"", "hyphenate-limit-chars: 5", "hyphenate-limit-chars", "5 auto auto"},
    {"", "hyphenate-limit-chars: 5 2", "hyphenate-limit-chars", "5 2 auto"},
    {"", "hyphenate-limit-chars: auto 3 4", "hyphenate-limit-chars", "auto 3 4"},
    {"", "image-orientation: 100deg", "image-orientation", "90deg"},
    {"", "image-orientation: 350deg flip", "image-orientation", "0deg flip"},
    {"", "image-orientation: -90deg", "image-orientation", "270deg"},
    {"", "image-orientation: flip", "image-orientation", "0deg flip"},
    {"", "image-orientation: none", "image-orientation", "none"},
    {"", "border-spacing: 3px", "border-spacing", "3px 3px"},
    {"", "border-spacing: 3px 4px", "border-spacing", "3px 4px"},
    {"", "border-image-width: 10px", "border-image-width", "10px 10px 10px 10px"},
    {"", "border-image-width: 10px auto", "border-image-width", "10px auto 10px auto"},
    {"", "border-image-outset: 1 2 3", "border-image-outset", "1 2 3 2"},
    {"", "border-image-slice: 10 20% fill", "border-image-slice", "10 20% 10 20% fill"},
    {"", "border-image-repeat: stretch", "border-image-repeat", "stretch stretch"},
    {"", "background-repeat: repeat-x", "background-repeat", "repeat no-repeat"},
    {"", "background-repeat: repeat-y, space", "background-repeat", "no-repeat repeat, space space"},
    {"", "background-repeat: no-repeat round", "background-repeat", "no-repeat round"},
    {"", "background-size: 10px", "background-size", "10px auto"},
    {"", "background-size: cover, 5em 20%", "background-size", "cover, 80px 20%"},
    {"", "background-size: auto", "background-size", "auto auto"},
    {"", "translate: 10px", "translate", "10px 0px 0px"},
    {"", "translate: 1em 20%", "translate", "16px 20% 0px"},
    {"", "translate: none", "translate", "none"},
    {"", "scale: 2", "scale", "2 2 1"},
    {"", "scale: 50% 3", "scale", "0.5 3 1"},
    {"", "scale: 1 2 3", "scale", "1 2 3"},
    {"", "rotate: x 45deg", "rotate", "1 0 0 45deg"},
    {"", "rotate: 1 2 3 0.25turn", "rotate", "1 2 3 90deg"},
    {"", "rotate: none", "rotate", "none"},
    {"", "text-shadow: 1px 2px red", "text-shadow", "red 1px 2px 0px 0px"},
    {"", "text-shadow: 1px 2px 3px, 4px 5px 6px 7px blue inset", "text-shadow", "currentcolor 1px 2px 3px 0px, blue 4px 5px 6px 7px inset"},
    {"", "text-shadow: none", "text-shadow", "none"},
    {"", "font-size-adjust: 0.5", "font-size-adjust", "ex-height 0.5"},
    {"", "font-size-adjust: cap-height 0.7", "font-size-adjust", "cap-height 0.7"},
    {"", "font-size-adjust: none", "font-size-adjust", "none"},
    {"", "stroke-dasharray: 1 2em 3", "stroke-dasharray", "1px, 32px, 3px"},
    {"", "stroke-dashoffset: 4", "stroke-dashoffset", "4px"},
    {"", "stroke-width: 3", "stroke-width", "3px"},
    {"", "stroke-width: thick", "stroke-width", "5px"},
    {"", "stroke-width: 10%", "stroke-width", "10%"},
    {"float: left", "wrap-flow: both", "wrap-flow", "both"},
    {"", "float: left; wrap-flow: both", "wrap-flow", "auto"},
  });
}

TEST(CssCompute, PositionsAreOffsetsFromTheTopLeftAsLengthsAndPercentages) {
  run_cases({
    {"", "object-position: center", "object-position", "50% 50%"},
    {"", "object-position: left", "object-position", "0% 50%"},
    {"", "object-position: right", "object-position", "100% 50%"},
    {"", "object-position: top", "object-position", "50% 0%"},
    {"", "object-position: bottom", "object-position", "50% 100%"},
    {"", "object-position: 10px", "object-position", "10px 50%"},
    {"", "object-position: 10px 20px", "object-position", "10px 20px"},
    {"", "object-position: top left", "object-position", "0% 0%"},
    {"", "object-position: bottom right", "object-position", "100% 100%"},
    {"", "object-position: left 10px top 20px", "object-position", "10px 20px"},
    {"", "object-position: right 10px bottom 20%", "object-position", "calc(100% - 10px) 80%"},
    {"", "object-position: right 3em top 4px", "object-position", "calc(100% - 48px) 4px"},
    {"", "object-position: left 5% bottom 4px", "object-position", "5% calc(100% - 4px)"},
    {"", "background-position-x: right 10px", "background-position-x", "right 10px"},
    {"", "background-position-x: center", "background-position-x", "left 50%"},
    {"", "background-position-x: 30%", "background-position-x", "left 30%"},
    {"", "background-position-x: right", "background-position-x", "right 0%"},
    {"", "background-position-y: bottom 5px, top", "background-position-y", "bottom 5px, top 0%"},
    {"", "transform-origin: left top 5px", "transform-origin", "0% 0% 5px"},
    {"", "transform-origin: bottom", "transform-origin", "50% 100%"},
    {"", "transform-origin: 10px 1em 3px", "transform-origin", "10px 16px 3px"},
    {"", "perspective-origin: right", "perspective-origin", "100% 50%"},
  });
}

} // namespace
