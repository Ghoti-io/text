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
 * The named defects of the computed-value completion pass: one test for each, quoting the sentence of the served drafts that decides it.
 */

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_compute_test_support.h"
#include "css_custom_test_support.h"

using namespace cmp;

namespace {

const std::string kLimit = "34028234663852886" + std::string(22, '0');

/* Values 4, 10.7.2: "A UA may have an implementation-defined limit for values approaching infinity." and 9.1: "extremely large values (including,
   notably, +-infinity) will clamp to the minimum/maximum value allowed". CSSOM, serializing a <number>: "Note: scientific notation is not used." */
TEST(CssComputedCompletionNumbers, NoPropertyWritesMoreThanTheLimitOfDigits) {
  struct Case {
    const char * decl;
    const char * property;
  };
  for (const Case & c : std::vector<Case>{{"flex-grow: 1e400", "flex-grow"}, {"opacity: 1e400", "opacity"}, {"order: 1e400", "order"},
           {"line-height: 1e400", "line-height"}, {"width: calc(1/0)", "width"}, {"width: calc(infinity * 1px)", "width"},
           {"margin-left: calc(-1/0)", "margin-left"}, {"flex-grow: calc(1e400)", "flex-grow"}, {"flex-shrink: 1e300", "flex-shrink"},
           {"z-index: 99999999999999999999999999", "z-index"}, {"z-index: calc(1/0)", "z-index"}, {"tab-size: 1e400", "tab-size"}}) {
    const std::string first = root_computed(c.decl, c.property);
    EXPECT_LE(first.size(), 41u) << c.decl << " -> " << first;
    EXPECT_EQ(first.find("e+"), std::string::npos) << c.decl << " -> " << first;
    // the text is a fixed point: it parses and computes to itself
    EXPECT_EQ(root_computed(std::string(c.property) + ": " + first, c.property), first) << c.decl << " -> " << first;
  }
}

TEST(CssComputedCompletionNumbers, ANumberPastTheLimitIsTheLimitAndAnIntegerIsTheLargestInteger) {
  EXPECT_EQ(root_computed("flex-grow: 1e400", "flex-grow"), kLimit);
  // "1e400" is a number token and not an integer token (Syntax 3, 4.3.12: an exponent makes the type "number"), so z-index refuses it, as it refuses 1e3
  EXPECT_EQ(root_computed("z-index: 1e400", "z-index"), "auto");
  EXPECT_EQ(root_computed("z-index: 99999999999999999999999999", "z-index"), "2147483647");
  EXPECT_EQ(root_computed("z-index: -99999999999999999999999999", "z-index"), "-2147483648");
}

/* Properties and Values API 1, "The initial-value descriptor": "the initial value must be computationally independent ... and must parse successfully according to the
   syntax"; `<transform-list>` is one or more transform functions and `none` is not one. Chromium registers it (a divergence key of class reference-wrong in the style differential); this library refuses the registration. */
TEST(CssComputedCompletionRegistration, ATransformListDoesNotTakeNoneAsItsInitialValue) {
  using namespace cust;
  World w;
  Reg r;
  r.name = "--t";
  r.syntax = "<transform-list>";
  r.initial = "none";
  r.has_initial = true;
  r.inherits = false;
  ASSERT_EQ(w.build({r}), GTEXT_CSS_OK);
  ASSERT_EQ(w.refused.size(), 1u);
  EXPECT_EQ(w.refused[0].first, "--t");
  // and with a transform function, or with `none` as a keyword of the syntax, the registration stands
  World ok;
  Reg a = r, b = r;
  a.initial = "rotate(1deg)";
  b.name = "--u";
  b.syntax = "<transform-list> | none";
  ASSERT_EQ(ok.build({a, b}), GTEXT_CSS_OK);
  EXPECT_TRUE(ok.refused.empty());
}

/* Color Adjust 1, "the element color scheme is the same as the page color scheme" for `normal`: below a root that declares `dark`, an element with
   `color-scheme: normal` (or one that names no scheme it can use) is dark, and light-dark() takes its second argument. A root that declares none, or `normal`, is the
   user agent default, light, whatever the environment prefers. */
TEST(CssComputedCompletionScheme, NormalBelowADarkRootIsDark) {
  const std::string pick = "color: light-dark(red, blue)";
  EXPECT_EQ(computed("color-scheme: dark", "color-scheme: normal; " + pick, "color"), "blue");
  EXPECT_EQ(computed("color-scheme: dark", pick, "color"), "blue") << "inherited";
  EXPECT_EQ(computed("color-scheme: light", "color-scheme: normal; " + pick, "color"), "red");
  EXPECT_EQ(computed("", "color-scheme: normal; " + pick, "color"), "red") << "no scheme declared anywhere: the user agent default";
  EXPECT_EQ(computed("color-scheme: normal", pick, "color"), "red");
  EXPECT_EQ(computed("color-scheme: dark", "color-scheme: light; " + pick, "color"), "red") << "an element's own scheme still wins";
  EXPECT_EQ(computed("color-scheme: dark", "color-scheme: only light; " + pick, "color"), "red");
  // three levels: the page's scheme is the root's, not the parent's
  Context ctx(R());
  Block rb(R(), "color-scheme: dark"), mb(R(), "color-scheme: light"), cb(R(), "color-scheme: normal; " + pick);
  Style root, mid, leaf;
  root.compute(ctx, &rb);
  mid.compute(ctx, &mb, &root);
  leaf.compute(ctx, &cb, &mid);
  ASSERT_EQ(leaf.status, GTEXT_CSS_OK);
  EXPECT_EQ(leaf.value("color"), "blue");
}

TEST(CssComputedCompletionScheme, ASubtreeTakesTheCallersPageScheme) {
  GTEXT_CSS_Compute_Options o = gtext_css_compute_options_default();
  o.has_root_scheme = true;
  o.root_scheme = GTEXT_CSS_SCHEME_DARK;
  Context ctx(R(), nullptr, &o);
  ASSERT_EQ(ctx.status, GTEXT_CSS_OK);
  Block b(R(), "color: light-dark(red, blue)"), own(R(), "color-scheme: light; color: light-dark(red, blue)");
  Style s, t;
  s.compute(ctx, &b);
  t.compute(ctx, &own);
  EXPECT_EQ(s.value("color"), "blue");
  EXPECT_EQ(t.value("color"), "red");
}

/* Syntax 3, <declaration-value>: it "contains no <bad-string-token>, <bad-url-token>, unmatched <)-token>, <]-token>, or <}-token>"; a line break inside a string makes a
   bad-string token, and a var(), env() or attr() that holds one anywhere (a fallback too) is not a valid value: the declaration is dropped at parse time, it is not pending. */
TEST(CssComputedCompletionVar, ABadStringOrBadUrlInAFunctionMakesTheDeclarationInvalid) {
  for (const char * decl : {"--a: var(--b, \"x\ny\")", "color: var(--b, \"x\ny\")", "width: var(--b, url(x y))", "content: attr(a type(*), \"x\ny\")",
           "color: env(a, 'x\ry')", "--a: var(--b, \"x\fy\")", "background-image: var(--b, url(a\"b))", "margin: calc(var(--b, \"x\ny\") * 1px)"}) {
    Block b(R(), decl);
    EXPECT_EQ(gtext_css_block_count(b.b), 0u) << decl << ": the declaration is dropped";
  }
  // the same without the line break is a valid, pending value
  Block ok(R(), "color: var(--b, \"xy\")");
  EXPECT_EQ(gtext_css_block_count(ok.b), 1u);
}

/* Values 4, 10.1 and 5.1 (<ratio>): a calculation that is a number is a number, and "<ratio> is always serialized with both components"; `sign(1rem - 1px)` is
   a number (Values 4, 10.6, sign()) once the font-relative length is made absolute. */
TEST(CssComputedCompletionRatio, ACalculationInARatioIsComputedToTheNumber) {
  EXPECT_EQ(root_computed("aspect-ratio: sign(1rem - 1px) / sign(1rem - 1px)", "aspect-ratio"), "1 / 1");
  EXPECT_EQ(root_computed("aspect-ratio: calc(1 + 1) / 2", "aspect-ratio"), "2 / 2");
  EXPECT_EQ(root_computed("aspect-ratio: sign(1rem - 1px)", "aspect-ratio"), "1 / 1");
  EXPECT_EQ(root_computed("aspect-ratio: auto calc(3) / calc(2)", "aspect-ratio"), "auto 3 / 2");
  EXPECT_EQ(root_computed("aspect-ratio: calc(-1) / 2", "aspect-ratio"), "0 / 2") << "the numerator is not negative";
  // a computed value is a fixed point
  EXPECT_EQ(root_computed("aspect-ratio: 3 / 2", "aspect-ratio"), "3 / 2");
  EXPECT_EQ(root_computed("aspect-ratio: auto 16 / 9", "aspect-ratio"), "auto 16 / 9");
}

/* Basic UI 4, caret-color: "auto | <color> [auto | <color>]?"; "If the second value is omitted, the behavior is the same as if auto had been specified." The
   computed value is the color alone when the second value is `auto`. */
TEST(CssComputedCompletionCaret, ASecondValueOfAutoIsLeftOut) {
  EXPECT_EQ(root_computed("caret-color: rgb(0, 255, 0) auto", "caret-color"), "rgb(0, 255, 0)");
  EXPECT_EQ(root_computed("caret-color: red auto", "caret-color"), "red");
  EXPECT_EQ(root_computed("caret-color: auto", "caret-color"), "auto");
  EXPECT_EQ(root_computed("caret-color: red blue", "caret-color"), "red blue");
  EXPECT_EQ(root_computed("caret-color: red", "caret-color"), "red");
}

/* Fonts of the page that uses Ahem (a font whose every glyph is a square one em wide) give `ch` a size: with the callback the environment has, a `ch` in a function
   argument or a length is computed, and without one it is GTEXT_CSS_E_UNSUPPORTED naming the unit, never a guess. */
bool ahem_metric(void *, const GTEXT_CSS_Font_Query * q, GTEXT_CSS_Font_Metric m, double * px) {
  if (m != GTEXT_CSS_METRIC_CH) {
    return false;
  }
  *px = q->size_px;
  return true;
}

TEST(CssComputedCompletionEnvironment, AChUnitInAnArgumentTakesTheEnvironmentsMetric) {
  GTEXT_CSS_Environment env = gtext_css_environment_default();
  env.font_metric = ahem_metric;
  EXPECT_EQ(root_computed("font-size: 20px; text-decoration-inset: 1ch -1ch", "text-decoration-inset", &env), "20px -20px");
  EXPECT_EQ(root_computed("font-size: 20px; text-decoration-inset: calc(1em / 4) calc(-1ch)", "text-decoration-inset", &env), "5px -20px");
  EXPECT_EQ(root_computed("font-size: 20px; text-decoration-inset: 1ch", "text-decoration-inset"), "<not supported ch>") << "no callback: not guessed";
}

/* Box Alignment 3, 4.1: "flex-start: A legacy value alias for flow-start." and "flex-end: A legacy value alias for flow-end."; <self-position> and <content-position> have
   flow-start and flow-end. The computed value of the alignment properties is the keyword(s) specified, and an alias is the keyword it stands for. */
TEST(CssComputedCompletionAlignment, TheLegacyKeywordsAreTheirFlowAliases) {
  for (const char * p : {"align-content", "justify-content", "align-self", "justify-self", "align-items", "justify-items"}) {
    const std::string n = p;
    EXPECT_EQ(root_computed(n + ": flex-start", n), "flow-start") << n;
    EXPECT_EQ(root_computed(n + ": flex-end", n), "flow-end") << n;
    EXPECT_EQ(root_computed(n + ": flow-start", n), "flow-start") << n;
    EXPECT_EQ(root_computed(n + ": flow-end", n), "flow-end") << n;
    EXPECT_EQ(root_computed(n + ": safe flex-start", n), "safe flow-start") << n;
    EXPECT_EQ(root_computed(n + ": unsafe flow-end", n), "unsafe flow-end") << n;
    EXPECT_EQ(root_computed(n + ": unsafe flex-end", n), "unsafe flow-end") << n;
    EXPECT_EQ(root_computed(n + ": center", n), "center") << n;
  }
  EXPECT_EQ(root_computed("place-items: flex-end flex-start", "align-items"), "flow-end");
  EXPECT_EQ(root_computed("place-items: flex-end flex-start", "justify-items"), "flow-start");
  EXPECT_EQ(root_computed("place-content: flex-start", "justify-content"), "flow-start");
  EXPECT_EQ(root_computed("place-self: flow-end", "align-self"), "flow-end");
  // a keyword nobody defines stays invalid
  EXPECT_EQ(root_computed("align-content: flow-middle", "align-content"), "normal");
}

/* Overflow 3: "The visible and clip values are known as the non-scrollable values." and "a specified value of visible computes to auto, enabling scrolling in its axis", when the
   other axis is scrollable. clip does not make the other axis scrollable and is not changed. */
TEST(CssComputedCompletionOverflow, VisibleBesideAScrollableAxisIsAuto) {
  struct Row {
    const char * x;
    const char * y;
    const char * cx;
    const char * cy;
  };
  for (const Row & r : std::vector<Row>{{"hidden", "visible", "hidden", "auto"}, {"scroll", "visible", "scroll", "auto"}, {"visible", "auto", "auto", "auto"},
           {"visible", "hidden", "auto", "hidden"}, {"visible", "scroll", "auto", "scroll"}, {"auto", "visible", "auto", "auto"}, {"clip", "visible", "clip", "visible"},
           {"visible", "clip", "visible", "clip"}, {"clip", "auto", "clip", "auto"}, {"visible", "visible", "visible", "visible"}, {"clip", "clip", "clip", "clip"}}) {
    const std::string d = std::string("overflow-x: ") + r.x + "; overflow-y: " + r.y;
    EXPECT_EQ(root_computed(d, "overflow-x"), r.cx) << d;
    EXPECT_EQ(root_computed(d, "overflow-y"), r.cy) << d;
  }
  EXPECT_EQ(root_computed("overflow: hidden visible", "overflow-y"), "auto");
  EXPECT_EQ(root_computed("overflow-x: scroll", "overflow-y"), "auto") << "the initial value of the other axis is visible";
  // the logical longhands map to the axes
  EXPECT_EQ(root_computed("overflow-x: hidden; overflow-block: visible", "overflow-block"), "auto");
  EXPECT_EQ(root_computed("overflow-y: hidden; overflow-inline: visible", "overflow-inline"), "auto");
  EXPECT_EQ(root_computed("overflow-y: hidden; overflow-inline: visible", "overflow-x"), "auto");
  EXPECT_EQ(root_computed("overflow-x: hidden; overflow-inline: visible", "overflow-inline"), "visible") << "the later declaration of the slot decides: both axes are visible";
}

/* Anchor Positioning 2, 2.1: "normal | [ [ size | inline-size ] || scroll-state || anchored ]". The computed value is the specified keywords, in the grammar's order. */
TEST(CssComputedCompletionNewerGrammar, ContainerTypeTakesAnchored) {
  EXPECT_EQ(root_computed("container-type: anchored", "container-type"), "anchored");
  EXPECT_EQ(root_computed("container-type: anchored size", "container-type"), "size anchored");
  EXPECT_EQ(root_computed("container-type: inline-size anchored", "container-type"), "inline-size anchored");
  EXPECT_EQ(root_computed("container-type: anchored scroll-state inline-size", "container-type"), "inline-size scroll-state anchored");
  // a keyword twice, or beside normal, stays invalid
  EXPECT_EQ(root_computed("container-type: anchored anchored", "container-type"), "normal");
  EXPECT_EQ(root_computed("container-type: normal anchored", "container-type"), "normal");
}

/* Text 4, 7.5: "UAs must also support the alternate keyword distribute which must compute to inter-character". */
TEST(CssComputedCompletionNewerGrammar, TextJustifyDistributeComputesToInterCharacter) {
  EXPECT_EQ(root_computed("text-justify: distribute", "text-justify"), "inter-character");
  EXPECT_EQ(root_computed("text-justify: DISTRIBUTE", "text-justify"), "inter-character");
  EXPECT_EQ(root_computed("text-justify: distribute no-compress", "text-justify"), "inter-character no-compress");
  EXPECT_EQ(root_computed("text-justify: inter-word", "text-justify"), "inter-word");
  EXPECT_EQ(root_computed("text-justify: distribute inter-word", "text-justify"), "auto") << "two justification keywords stay invalid";
}

/* View Transitions 2: "The values none, auto, and match-element are excluded from <custom-ident>" and "also accepts an auto keyword"; a keyword is ASCII case-insensitive and
   is written in lower case, a custom identifier keeps its case. */
TEST(CssComputedCompletionNewerGrammar, ViewTransitionNameKeywordsAreLowerCase) {
  EXPECT_EQ(root_computed("view-transition-name: match-element", "view-transition-name"), "match-element");
  EXPECT_EQ(root_computed("view-transition-name: maTch-element", "view-transition-name"), "match-element");
  EXPECT_EQ(root_computed("view-transition-name: AUTO", "view-transition-name"), "auto");
  EXPECT_EQ(root_computed("view-transition-name: NONE", "view-transition-name"), "none");
  EXPECT_EQ(root_computed("view-transition-name: Foo", "view-transition-name"), "Foo");
  EXPECT_EQ(root_computed("view-transition-name: match-element-2", "view-transition-name"), "match-element-2");
}

/* Color HDR 1, 3.3: "Let p1, ..., pN be the mixing percentages, normalized to sum to 100%"; the computed value is dynamic-range-limit-mix() "with parameters standard,
   constrained, and no-limit, in that order ... omitting parameters with a percentage equal to 0%", and a mix that is all of one limit is that limit. */
TEST(CssComputedCompletionMixes, DynamicRangeLimitMixNormalizesAndOrders) {
  struct Row {
    const char * in;
    const char * out;
  };
  for (const Row & r : std::vector<Row>{{"dynamic-range-limit-mix(no-limit 80%, standard 60%, constrained 60%)", "dynamic-range-limit-mix(standard 30%, constrained 30%, no-limit 40%)"},
           {"dynamic-range-limit-mix(constrained 75%, standard 25%)", "dynamic-range-limit-mix(standard 25%, constrained 75%)"},
           {"dynamic-range-limit-mix(constrained 25%, no-limit 25%)", "dynamic-range-limit-mix(constrained 50%, no-limit 50%)"},
           {"dynamic-range-limit-mix(no-limit 10%, standard 30%)", "dynamic-range-limit-mix(standard 75%, no-limit 25%)"},
           {"dynamic-range-limit-mix(no-limit 100%, standard 0%)", "no-limit"},
           {"dynamic-range-limit-mix(no-limit 100%, no-limit 0%)", "no-limit"},
           {"dynamic-range-limit-mix(no-limit 25%, standard 25%, standard 25%, standard 25%)", "dynamic-range-limit-mix(standard 75%, no-limit 25%)"},
           {"dynamic-range-limit-mix(no-limit 100%, standard 100%)", "dynamic-range-limit-mix(standard 50%, no-limit 50%)"},
           {"dynamic-range-limit-mix(dynamic-range-limit-mix(standard 10%, no-limit 30%) 20%, standard 80%)", "dynamic-range-limit-mix(standard 85%, no-limit 15%)"},
           {"dynamic-range-limit-mix(no-limit 10%, dynamic-range-limit-mix(standard 25%, constrained 75%) 20%, dynamic-range-limit-mix(constrained 10%, no-limit 30%) 20%)",
               "dynamic-range-limit-mix(standard 10%, constrained 40%, no-limit 50%)"},
           {"standard", "standard"}}) {
    EXPECT_EQ(root_computed(std::string("dynamic-range-limit: ") + r.in, "dynamic-range-limit"), r.out) << r.in;
  }
}

/* Values 5, 6.1 "Normalizing Mix Percentages": the omitted percentages share what the written ones leave. The equal shares of a mix are not written, and the default
   interpolation method (`in oklab`) is not. Percentages that are all written are kept as written, whatever they sum to (6.2: where the mix is not calculated, "the mix
   notation itself, with its arguments computed individually"). */
TEST(CssComputedCompletionMixes, PaletteMixNormalizesItsPercentages) {
  struct Row {
    const char * in;
    const char * out;
  };
  for (const Row & r : std::vector<Row>{{"palette-mix(light 30%, dark)", "palette-mix(light 30%, dark 70%)"},
           {"palette-mix(in oklab, light 30%, dark)", "palette-mix(light 30%, dark 70%)"},
           {"palette-mix(in srgb, dark 50%, light 50%)", "palette-mix(in srgb, dark, light)"},
           {"palette-mix(in srgb, dark 50%, light)", "palette-mix(in srgb, dark, light)"},
           {"palette-mix(in srgb, dark 10%, light)", "palette-mix(in srgb, dark 10%, light 90%)"},
           {"palette-mix(in srgb, dark, light 90%)", "palette-mix(in srgb, dark 10%, light 90%)"},
           {"palette-mix(in oklab, 30% --foo, normal)", "palette-mix(--foo 30%, normal 70%)"},
           {"palette-mix(in oklab, light 10%, dark 40%)", "palette-mix(light 10%, dark 40%)"},
           {"palette-mix(in srgb, dark 75%, light 75%)", "palette-mix(in srgb, dark 75%, light 75%)"},
           {"palette-mix(in srgb, dark 30%, light 60%, --foo 90%)", "palette-mix(in srgb, dark 30%, light 60%, --foo 90%)"},
           {"palette-mix(in srgb, dark 0%, light 0%)", "palette-mix(in srgb, dark 0%, light 0%)"},
           {"palette-mix(palette-mix(in srgb, light 30%, normal) 20%, dark)", "palette-mix(palette-mix(in srgb, light 30%, normal 70%) 20%, dark 80%)"},
           {"light", "light"}}) {
    EXPECT_EQ(root_computed(std::string("font-palette: ") + r.in, "font-palette"), r.out) << r.in;
  }
}

/* Easing 2, 2.1.1: "control points originally specified with two input progress values are turned into two separate control points, and the input progress values are in
   strictly ascending order"; "If the control point originally lacked an input progress value, return s": a position that was not written is not filled in. */
TEST(CssComputedCompletionEasing, LinearSplitsDoublePositionsAndAscends) {
  struct Row {
    const char * in;
    const char * out;
  };
  for (const Row & r : std::vector<Row>{{"linear(0 0% 50%, 1 50% 100%)", "linear(0 0%, 0 50%, 1 50%, 1 100%)"}, {"linear(0 20%, 0.5 10%, 1)", "linear(0 20%, 0.5 20%, 1)"},
           {"linear(0, 0.25 25% 75%, 1)", "linear(0, 0.25 25%, 0.25 75%, 1)"}, {"linear(0, 0.5 25% 75%, 1 100% 100%)", "linear(0, 0.5 25%, 0.5 75%, 1 100%, 1 100%)"},
           {"linear(0, 1)", "linear(0, 1)"}, {"linear(0 10%, 1 90%)", "linear(0 10%, 1 90%)"}, {"ease-in, linear(0 50% 25%, 1)", "ease-in, linear(0 50%, 0 50%, 1)"}}) {
    EXPECT_EQ(root_computed(std::string("animation-timing-function: ") + r.in, "animation-timing-function"), r.out) << r.in;
    EXPECT_EQ(root_computed(std::string("transition-timing-function: ") + r.in, "transition-timing-function"), r.out) << r.in;
  }
}

/* Easing 2, 2.3.1: "step-start and step-end do not serialize as themselves. Instead, they serialize as steps(1, start) and steps(1)"; a step position of end or jump-end
   is not written. 2.3: jump-none needs at least 2 steps, the others at least 1. */
TEST(CssComputedCompletionEasing, StepsSerializeAsTheTextSays) {
  struct Row {
    const char * in;
    const char * out;
  };
  for (const Row & r : std::vector<Row>{{"step-start", "steps(1, start)"}, {"step-end", "steps(1)"}, {"steps(4, end)", "steps(4)"}, {"steps(4, jump-end)", "steps(4)"},
           {"steps(4)", "steps(4)"}, {"steps(4, jump-both)", "steps(4, jump-both)"}, {"steps(calc(-10), start)", "steps(1, start)"}, {"steps(calc(1), jump-none)", "steps(2, jump-none)"},
           {"ease, step-start, steps(3, END)", "ease, steps(1, start), steps(3)"}, {"cubic-bezier(0, 0, 1, 1)", "cubic-bezier(0, 0, 1, 1)"}}) {
    EXPECT_EQ(root_computed(std::string("transition-timing-function: ") + r.in, "transition-timing-function"), r.out) << r.in;
    EXPECT_EQ(root_computed(std::string("animation-timing-function: ") + r.in, "animation-timing-function"), r.out) << r.in;
  }
}

} // namespace
