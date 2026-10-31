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

/* Box Alignment 3 (served 2026-10-30), justify-items: "if the inherited value of justify-items includes the legacy keyword, this value computes to the inherited value";
   otherwise a lone legacy is normal. Both pinned browsers write normal under a root (recorded case justify-items-legacy). */
TEST(CssComputedCompletionAlignment, ALoneLegacyIsTheInheritedLegacyValueOrNormal) {
  EXPECT_EQ(root_computed("justify-items: legacy", "justify-items"), "normal");
  EXPECT_EQ(computed("justify-items: normal", "justify-items: legacy", "justify-items"), "normal");
  EXPECT_EQ(computed("justify-items: legacy left", "justify-items: legacy", "justify-items"), "legacy left");
  EXPECT_EQ(computed("justify-items: legacy", "justify-items: legacy", "justify-items"), "normal") << "the parent's own lone legacy computed to normal";
  EXPECT_EQ(root_computed("justify-items: legacy left", "justify-items"), "legacy left");
  EXPECT_EQ(root_computed("place-items: flex-end legacy", "justify-items"), "normal");
}

/* Viewport 1 (served 2026-10-30), zoom: "A 0 value is treated as if it was 1." and "A 0 percentage is treated as if it was 100%."; the computed value line ("as specified, but
   with <percentage> converted to the equivalent <number>") does not say whether the 1 is written, and both pinned browsers write it (recorded cases zoom.1, zoom.2). */
TEST(CssComputedCompletionZoom, ZeroAndNormalAreOne) {
  EXPECT_EQ(root_computed("zoom: normal", "zoom"), "1");
  EXPECT_EQ(root_computed("zoom: 0", "zoom"), "1");
  EXPECT_EQ(root_computed("zoom: 0%", "zoom"), "1");
  EXPECT_EQ(root_computed("zoom: 50%", "zoom"), "0.5");
  EXPECT_EQ(root_computed("zoom: 2", "zoom"), "2");
}

/* Borders 4 (served 2026-10-30), corner-top-left-shape and its seven siblings: the computed value is "the corresponding superellipse() value"; the keywords are round 1,
   scoop -1, bevel 0, notch -infinity, square infinity and squircle 2. Both pinned browsers write these (recorded cases corner-shape.*). */
TEST(CssComputedCompletionCorner, EveryKeywordIsItsSuperellipse) {
  const struct { const char * keyword; const char * computed; } table[] = {
    {"round", "superellipse(1)"}, {"scoop", "superellipse(-1)"}, {"bevel", "superellipse(0)"},
    {"notch", "superellipse(-infinity)"}, {"square", "superellipse(infinity)"}, {"squircle", "superellipse(2)"},
    {"superellipse(5)", "superellipse(5)"}, {"superellipse( -1)", "superellipse(-1)"}, {"superellipse(0.2)", "superellipse(0.2)"},
    {"superellipse(-infinity)", "superellipse(-infinity)"}, {"superellipse(infinity)", "superellipse(infinity)"},
  };
  for (const char * property : {"corner-top-left-shape", "corner-top-right-shape", "corner-bottom-left-shape", "corner-bottom-right-shape",
                                "corner-start-start-shape", "corner-start-end-shape", "corner-end-start-shape", "corner-end-end-shape"}) {
    for (const auto & row : table) {
      EXPECT_EQ(root_computed(std::string(property) + ": " + row.keyword, property), row.computed) << property << ": " << row.keyword;
    }
    EXPECT_EQ(root_computed("margin: 0", property), "superellipse(1)") << property << " initial";
  }
}

TEST(CssComputedCompletionCorner, TheShapeShorthandDistributesAndComputesEachCorner) {
  EXPECT_EQ(root_computed("corner-shape: superellipse(5) round", "corner-top-left-shape"), "superellipse(5)");
  EXPECT_EQ(root_computed("corner-shape: superellipse(5) round", "corner-top-right-shape"), "superellipse(1)");
  EXPECT_EQ(root_computed("corner-shape: superellipse(5) round", "corner-bottom-right-shape"), "superellipse(5)");
  EXPECT_EQ(root_computed("corner-shape: bevel superellipse(0.1) round squircle", "corner-bottom-left-shape"), "superellipse(2)");
  EXPECT_EQ(root_computed("corner-shape: superellipse(-1) superellipse(3) square", "corner-bottom-right-shape"), "superellipse(infinity)");
}

/* Anchor Positioning 1 (served 2026-10-30), 3.1.3: "It serializes in the order given in the grammar (above), with the logical keywords serialized in their short forms (e.g. start
   start instead of block-start inline-start)." The collapse of a pair that says one keyword, and of a keyword with span-all for the other axis, is the answer of both pinned
   browsers (recorded cases position-area.*). */
TEST(CssComputedCompletionPositionArea, SerializesInTheOrderOfTheGrammarAndShortens) {
  const struct { const char * specified; const char * computed; } rows[] = {
    {"none", "none"}, {"span-all", "span-all"}, {"center", "center"}, {"span-all span-all", "span-all"}, {"span-all center", "span-all center"},
    {"center span-all", "center span-all"}, {"center center", "center"}, {"top left", "left top"}, {"y-start x-end", "x-end y-start"},
    {"top center", "center top"}, {"span-bottom center", "center span-bottom"}, {"top span-all", "top"}, {"bottom span-all", "bottom"},
    {"block-start inline-end", "start end"}, {"self-block-end self-inline-start", "self-end self-start"}, {"inline-end block-start", "start end"},
    {"left", "left"}, {"self-start", "self-start"}, {"start", "start"},
  };
  for (const auto & row : rows) {
    EXPECT_EQ(root_computed(std::string("position-area: ") + row.specified, "position-area"), row.computed) << row.specified;
  }
}

/* Positioned Layout 4 (served 2026-10-30), overlay: "none | auto"; an element that is not in the top layer has none whichever was specified (recorded case overlay.1). */
TEST(CssComputedCompletionOverlay, AutoIsNoneOutsideTheTopLayer) {
  EXPECT_EQ(root_computed("overlay: auto", "overlay"), "none");
  EXPECT_EQ(root_computed("overlay: none", "overlay"), "none");
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

/* Color 5, 7: "For the image form, this function returns the first image, if the element color scheme is light, or the second image, if the element color scheme is dark."
   `none` is not an <image> in the served text; both pinned browsers write the transparent image for `light-dark(none, ...)` (cases light-dark-none.1, .2, light-dark-img.1). */
TEST(CssComputedCompletionLightDarkImage, TheImageOfTheElementsScheme) {
  EXPECT_EQ(root_computed("background-image: light-dark(url(a), url(b))", "background-image"), "url(\"a\")");
  EXPECT_EQ(root_computed("color-scheme: dark; background-image: light-dark(url(a), url(b))", "background-image"), "url(\"b\")");
  EXPECT_EQ(root_computed("background-image: light-dark(none, none)", "background-image"), "image(rgba(0, 0, 0, 0))");
  EXPECT_EQ(root_computed("color-scheme: dark; list-style-image: light-dark(url(a), none)", "list-style-image"), "image(rgba(0, 0, 0, 0))");
  EXPECT_EQ(root_computed("background-image: url(x), light-dark(none, url(b))", "background-image"), "url(\"x\"), image(rgba(0, 0, 0, 0))");
  EXPECT_EQ(root_computed("mask-image: light-dark(linear-gradient(red, blue), url(b))", "mask-image"), "linear-gradient(red, blue)");
}

/* Values 5, 8.8: "ident( <ident-arg>+ )", "<ident-arg> = <string> | <integer> | <ident>"; the items are replaced "with its computed value" and serialized, then
   concatenated; a result that "can't be represented as an ident" is the guaranteed-invalid value. A calculation of numbers is the integer it computes to, and a unit in
   it is made absolute against the element (font-size 16px here). */
TEST(CssComputedCompletionIdent, ACalculationIsTheIntegerItComputesTo) {
  EXPECT_EQ(root_computed("animation-name: ident(\"a\" calc(2 + 3))", "animation-name"), "a5");
  EXPECT_EQ(root_computed("animation-name: ident(\"a\" calc(1em / 1px))", "animation-name"), "a16");
  EXPECT_EQ(root_computed("font-size: 10px; animation-name: ident(\"a\" calc(1em / 1px))", "animation-name"), "a10");
  // Values 4, 6.1.1: a font-relative length in a font-affecting property is made absolute against the parent's font (here the initial 16px, whatever the element's own size is)
  EXPECT_EQ(root_computed("font-size: 10px; font-palette: ident(\"--test-\" calc(1em / 1px))", "font-palette"), "--test-16");
  EXPECT_EQ(root_computed("font-size: 10px; font-variant-alternates: stylistic(ident(\"test-\" calc(1em / 1px)))", "font-variant-alternates"), "stylistic(test-16)");
  EXPECT_EQ(root_computed("animation-name: ident(\"a\" calc(2.5))", "animation-name"), "a3") << "an <integer> calculation is rounded";
  // a calculation that is not a number makes the function the guaranteed-invalid value, so the declaration is not a value
  EXPECT_EQ(root_computed("animation-name: ident(\"a\" calc(2px))", "animation-name"), "none");
}

/* Link Parameters 1 (served 2026-10-30), 2.1: "a completely empty second argument is valid, and is indicated by specifying the comma" and "param(--foo,) explicitly gives
   the link parameter an empty value"; the first argument is "color | accent-color | [ <dashed-ident> <css-type>? ]", and a <css-type> is `<color>` (CSS Values 5,
   "<syntax-single-component> = '<' <syntax-type-name> '>'"). The commas of other functions stay omissible (Values 4, 2). */
TEST(CssComputedCompletionLinkParameters, AnEmptySecondArgumentNeedsItsCommaAndATypeIsWrittenWithoutSpaces) {
  EXPECT_EQ(root_computed("link-parameters: param(--a, )", "link-parameters"), "param(--a, )");
  EXPECT_EQ(root_computed("link-parameters: param(--a,)", "link-parameters"), "param(--a, )");
  EXPECT_EQ(root_computed("link-parameters: param(--a, ), param(--b, )", "link-parameters"), "param(--a, ), param(--b, )");
  EXPECT_EQ(root_computed("link-parameters: param(--a <color>, orange)", "link-parameters"), "param(--a <color>, orange)");
  EXPECT_EQ(root_computed("link-parameters: param(--a <length>, currentcolor)", "link-parameters"), "param(--a <length>, currentcolor)");
  EXPECT_EQ(root_computed("link-parameters: param(--a <color>, orange), param(--b, blue)", "link-parameters"), "param(--a <color>, orange), param(--b, blue)");
  // without a comma the second argument is not given: not a value of the property
  EXPECT_EQ(root_computed("link-parameters: param(--a)", "link-parameters"), "none");
  // other functions keep the omission rule: a comma that nothing follows is not written
  EXPECT_EQ(root_computed("animation-timing-function: steps(2,)", "animation-timing-function"), "ease");
}

/* Values 5, 4.2 (served 2026-10-30): x-start, x-end, y-start and y-end "compute the same as the physical edge keyword corresponding to the start/end side in the y/x
   axis"; Backgrounds 4, 2.6: the grammar of background-position-x is "[ center | [ [ left | right | x-start | x-end ]? <length-percentage>? ]! ]#". */
TEST(CssComputedCompletionPosition, TheAxisKeywordsAreTheirPhysicalEdges) {
  EXPECT_EQ(root_computed("background-position-x: x-start", "background-position-x"), "left 0%");
  EXPECT_EQ(root_computed("background-position-x: x-end 10px", "background-position-x"), "right 10px");
  EXPECT_EQ(root_computed("background-position-x: 0.5em, x-start, x-end", "background-position-x"), "left 8px, left 0%, right 0%");
  EXPECT_EQ(root_computed("background-position-y: y-end", "background-position-y"), "bottom 0%");
  EXPECT_EQ(root_computed("background-position-y: y-start 3px", "background-position-y"), "top 3px");
  EXPECT_EQ(root_computed("background-position: x-end y-end", "background-position-x"), "right 0%");
  EXPECT_EQ(root_computed("object-position: x-end y-start", "object-position"), "100% 0%");
  // an axis keyword of the other axis is not a value of the property: the initial value, in the form of the property (the members of a logical group write the
  // origin keyword differently, so the one computed first does not stand for the others)
  EXPECT_EQ(root_computed("background-position-x: y-start", "background-position-x"), "left 0%");
  EXPECT_EQ(root_computed("", "background-position-x"), "left 0%");
  EXPECT_EQ(root_computed("", "background-position-y"), "top 0%");
  EXPECT_EQ(root_computed("", "background-position-inline"), "start 0%");
  EXPECT_EQ(root_computed("", "background-position-block"), "start 0%");
  EXPECT_EQ(root_computed("background-position-inline: 5px", "background-position-inline"), "start 5px");
  // the members of one slot are one declaration: a winner declared in one member's words is read in the others' (horizontal ltr: x is inline, y is block)
  EXPECT_EQ(root_computed("background-position-inline: end 5px", "background-position-x"), "right 5px");
  EXPECT_EQ(root_computed("background-position-x: right 5px", "background-position-inline"), "end 5px");
  EXPECT_EQ(root_computed("background-position-y: bottom 5px", "background-position-block"), "end 5px");
  EXPECT_EQ(root_computed("direction: rtl; background-position-inline: end 5px", "background-position-x"), "left 5px");
  EXPECT_EQ(root_computed("direction: rtl; background-position-x: left 5px", "background-position-inline"), "end 5px");
  // in a vertical writing mode the block axis is the horizontal one: the initial value is one position, said in each member's words (vertical-rl starts at the right)
  EXPECT_EQ(root_computed("writing-mode: vertical-rl", "background-position-x"), "left 0%");
  EXPECT_EQ(root_computed("writing-mode: vertical-rl", "background-position-block"), "end 0%");
  EXPECT_EQ(root_computed("writing-mode: vertical-rl", "background-position-inline"), "start 0%");
  EXPECT_EQ(root_computed("writing-mode: vertical-lr", "background-position-block"), "start 0%");
  // below a parent that sets the writing mode, the child's initial values are said in its (inherited) flow
  EXPECT_EQ(computed("writing-mode: vertical-rl", "color: red", "background-position-block"), "end 0%");
  EXPECT_EQ(computed("writing-mode: vertical-rl", "color: red", "background-position-x"), "left 0%");
  EXPECT_EQ(computed("font-size: 20px; color: red; writing-mode: vertical-rl", "color: color-mix(in oklch, red 30%, blue)", "background-position-block"), "end 0%");
  // a later declaration of the other member wins, as before
  EXPECT_EQ(root_computed("background-position-inline: end 5px; background-position-x: left 1px", "background-position-inline"), "start 1px");
}

/* Masking 1, Appendix A: "User agents must support separation with commas, but may also support separation without commas"; both pinned browsers write the commas for the
   other form (case clip.4), which the text does not contradict. This is the specified value's text (the computed value is "as specified"). */
std::string specified_text(const std::string & decl, const char * property) {
  GTEXT_CSS_Declaration_Options o = gtext_css_declaration_options_default();
  GTEXT_CSS_Declaration_Block * b = nullptr;
  if (gtext_css_block_new(R(), &o, &b) != GTEXT_CSS_OK) {
    return "<block>";
  }
  GTEXT_CSS_Property_Outcome oc = GTEXT_CSS_PROP_RESULT_UNKNOWN;
  const std::string value = decl;
  gtext_css_block_add_text(b, property, strlen(property), value.data(), value.size(), false, nullptr, &oc, nullptr);
  std::string out = "<invalid>";
  if (oc == GTEXT_CSS_PROP_RESULT_VALID) {
    char * t = nullptr;
    size_t n = 0;
    if (gtext_css_block_get_property_value(b, find(R(), property), &t, &n, nullptr) == GTEXT_CSS_OK) {
      out.assign(t, n);
      gtext_css_serialize_buffer_free(nullptr, t);
    }
  }
  gtext_css_block_free(b);
  return out;
}

TEST(CssComputedCompletionClip, RectWithoutCommasIsWrittenWithThem) {
  EXPECT_EQ(specified_text("rect(1px 2px 3px 4px)", "clip"), "rect(1px, 2px, 3px, 4px)");
  EXPECT_EQ(specified_text("rect(auto 2px calc(1px + 2em) 4px)", "clip"), "rect(auto, 2px, calc(2em + 1px), 4px)");
  EXPECT_EQ(specified_text("rect(1px, 2px, 3px, 4px)", "clip"), "rect(1px, 2px, 3px, 4px)");
  EXPECT_EQ(specified_text("rect(1px 2px 3px)", "clip"), "<invalid>") << "three offsets are not a value";
  EXPECT_EQ(specified_text("rect(1px 2px, 3px, 4px)", "clip"), "<invalid>") << "commas between some of the offsets only are not a value";
}

/* Easing 2, 2.1: "linear() = linear( [ <number> && <percentage>{0,2} ] # )" has no minimum number of control points, and 2.1.2 step 2 handles "a single item": linear(1) is a
   value, though both pinned browsers refuse it (case linear.17; the text decides). */
TEST(CssComputedCompletionEasing, ASingleControlPointIsAValue) {
  EXPECT_EQ(root_computed("animation-timing-function: linear(1)", "animation-timing-function"), "linear(1)");
  EXPECT_EQ(root_computed("animation-timing-function: linear(0.5 30%)", "animation-timing-function"), "linear(0.5 30%)");
}

/* CSS Conditional 5, 5.2: "The keywords none, and, not, and or are excluded from this <custom-ident>."; Will Change 1, 2: the <custom-ident> "excludes the keywords
   will-change, none, all, auto, scroll-position, and contents"; Counter Styles 3, 2.1: "The keyword none (including any ASCII case-insensitive permutation of it) is not a
   valid <counter-style-name>." An excluded word makes the declaration invalid, so the property has its initial value. */
TEST(CssComputedCompletionIdentExclusions, TheKeywordsEachTypeExcludes) {
  EXPECT_EQ(root_computed("container-name: a b", "container-name"), "a b");
  for (const char * bad : {"none none", "not", "a and", "OR b"}) {
    EXPECT_EQ(root_computed(std::string("container-name: ") + bad, "container-name"), "none") << bad;
  }
  EXPECT_EQ(root_computed("will-change: transform, opacity", "will-change"), "transform, opacity");
  EXPECT_EQ(root_computed("will-change: scroll-position, contents", "will-change"), "scroll-position, contents");
  for (const char * bad : {"auto, transform", "contents, auto", "all", "none", "will-change", "foo, NONE"}) {
    EXPECT_EQ(root_computed(std::string("will-change: ") + bad, "will-change"), "auto") << bad;
  }
  EXPECT_EQ(root_computed("content: counter(foo, decimal)", "content"), "counter(foo, decimal)");
  EXPECT_EQ(root_computed("content: counter(foo, none)", "content"), "normal");
  EXPECT_EQ(root_computed("content: counters(foo, \"\", NoNe)", "content"), "normal");
}

/* Fonts 4, <opentype-tag>: "feature tags contain four ASCII characters. Tag strings longer or shorter than four characters, or containing characters outside the U+20-7E
   codepoint range are invalid." (and the same of the axis names of font-variation-settings). */
TEST(CssComputedCompletionFontTags, AFeatureOrAxisTagIsFourPrintableAsciiCharacters) {
  EXPECT_EQ(root_computed("font-variation-settings: \"wght\" 700", "font-variation-settings"), "\"wght\" 700");
  EXPECT_EQ(root_computed("font-variation-settings: \"ab  \" 1", "font-variation-settings"), "\"ab  \" 1");
  for (const char * bad : {"\"wgt\" 700", "\"abcde\" 1", "'a' 1234", "\"wgh\\e9\" 1"}) {
    EXPECT_EQ(root_computed(std::string("font-variation-settings: ") + bad, "font-variation-settings"), "normal") << bad;
  }
  EXPECT_EQ(root_computed("font-feature-settings: \"liga\" off", "font-feature-settings"), "\"liga\" off");
  EXPECT_EQ(root_computed("font-feature-settings: \"lig\"", "font-feature-settings"), "normal");
  EXPECT_EQ(root_computed("font-feature-settings: \"ligat\" 1", "font-feature-settings"), "normal");
}

/* Grid 2 (served 2026-10-30), 8.3: "In all the above productions, the <custom-ident> additionally excludes the keywords span and auto." and "Negative integers or zero are
   invalid" after span. So `span` is no line name: `span -1` and a lone `span` are invalid, and a calculation after `span` is clamped to [1,inf], not to the union of the ranges of
   the line-number alternative. */
TEST(CssComputedCompletionGrid, SpanIsNoLineNameAndItsIntegerIsPositive) {
  EXPECT_EQ(root_computed("grid-row-start: span calc(-1)", "grid-row-start"), "span 1");
  EXPECT_EQ(root_computed("grid-row-start: span calc(0)", "grid-row-start"), "span 1");
  EXPECT_EQ(root_computed("grid-row-start: span calc(3)", "grid-row-start"), "span 3");
  EXPECT_EQ(root_computed("grid-row-start: span -1", "grid-row-start"), "auto") << "invalid, so the initial value";
  EXPECT_EQ(root_computed("grid-row-start: span", "grid-row-start"), "auto");
  EXPECT_EQ(root_computed("grid-row-end: SPAN foo", "grid-row-end"), "span foo");
  EXPECT_EQ(root_computed("grid-row-start: foo span 2", "grid-row-start"), "auto") << "a line named span is not allowed beside an integer";
  // the line-number alternative keeps the union of its two ranges
  EXPECT_EQ(root_computed("grid-row-start: calc(3)", "grid-row-start"), "3");
  EXPECT_EQ(root_computed("grid-row-start: calc(-2) foo", "grid-row-start"), "-2 foo");
  // <line-names>: "the <custom-ident> in the <line-names> production excludes the keywords span and auto"
  EXPECT_EQ(root_computed("grid-template-columns: [span] 1fr", "grid-template-columns"), "none");
  EXPECT_EQ(root_computed("grid-template-columns: [a b] 1fr", "grid-template-columns"), "[a b] 1fr");
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

/* Values 4, 6.1 (Lengths): "if a 0 could be parsed as either a <number> or a <length> in a property (such as line-height), it must parse as a <number>". The grammar of
   border-image-width writes <length-percentage> before <number>; the literal 0 is the number. */
TEST(CssComputedCompletionNewerGrammar, ALiteralZeroIsTheNumberWhereTheGrammarHasBoth) {
  EXPECT_EQ(root_computed("border-image-width: 0", "border-image-width"), "0 0 0 0");
  EXPECT_EQ(root_computed("border-image-width: 0 1px 0", "border-image-width"), "0 1px 0 1px");
  EXPECT_EQ(root_computed("border-image-width: 0px", "border-image-width"), "0px 0px 0px 0px") << "a zero with a unit is the length";
  EXPECT_EQ(root_computed("border-image-width: calc(0)", "border-image-width"), "0 0 0 0");
}

/* Text Decoration 4, 3.3 (served 2026-10-30): the computed value of text-emphasis-style is "a pair of keywords representing the shape and fill"; "If a shape keyword is
   specified but neither of filled nor open is specified, filled is assumed. If only filled or open is specified, the shape keyword computes to circle in horizontal
   typographic modes and sesame in vertical typographic modes." */
TEST(CssComputedCompletionNewerGrammar, TextEmphasisStyleIsAPair) {
  EXPECT_EQ(root_computed("text-emphasis-style: filled", "text-emphasis-style"), "filled circle");
  EXPECT_EQ(root_computed("text-emphasis-style: open", "text-emphasis-style"), "open circle");
  EXPECT_EQ(root_computed("text-emphasis-style: triangle", "text-emphasis-style"), "filled triangle");
  EXPECT_EQ(root_computed("text-emphasis-style: sesame open", "text-emphasis-style"), "open sesame");
  EXPECT_EQ(root_computed("text-emphasis-style: filled dot", "text-emphasis-style"), "filled dot");
  EXPECT_EQ(root_computed("writing-mode: vertical-rl; text-emphasis-style: filled", "text-emphasis-style"), "filled sesame");
  EXPECT_EQ(root_computed("writing-mode: vertical-lr; text-emphasis-style: open", "text-emphasis-style"), "open sesame");
  EXPECT_EQ(root_computed("text-emphasis-style: none", "text-emphasis-style"), "none");
  EXPECT_EQ(root_computed("text-emphasis-style: 'x'", "text-emphasis-style"), "\"x\"");
}

/* Anchor Positioning 1: position-try-fallbacks is "none | [ [<dashed-ident> || <try-tactic>] | <position-area> ]#", computed "as specified"; an item that is a position-area
   is computed as position-area is (Anchor Positioning 1, 3.1.3), and the recorded answers of both pinned browsers write `start start` as `start`. */
TEST(CssComputedCompletionNewerGrammar, PositionTryFallbacksComputeTheirPositionAreas) {
  EXPECT_EQ(root_computed("position-try-fallbacks: start start", "position-try-fallbacks"), "start");
  EXPECT_EQ(root_computed("position-try-fallbacks: top span-all, start start", "position-try-fallbacks"), "top, start");
  EXPECT_EQ(root_computed("position-try-fallbacks: flip-block, --a flip-x, block-start span-all", "position-try-fallbacks"), "flip-block, --a flip-x, block-start span-all");
  EXPECT_EQ(root_computed("position-try-fallbacks: none", "position-try-fallbacks"), "none");
}

/* Values 4, 6.1.1: "Properties that affect the font size or font metrics of an element are font-affecting properties. When used in the value of any font-affecting
   property on the element they refer to, the font-relative lengths resolve against the computed metrics of the parent element". The element's own font-size is 100px
   here and its parent's 10px; the other properties of the element are not font-affecting and use its own. */
TEST(CssComputedCompletionNewerGrammar, FontRelativeLengthsInFontPropertiesUseTheParent) {
  Context ctx(R());
  Block pb(R(), "font-size: 10px");
  Block cb(R(), "font-size: 100px; font-weight: calc(1em / 1px); font-width: calc(1em / 1px * 1%); font-size-adjust: calc(1em / 1px); width: 2em");
  Style parent, child;
  parent.compute(ctx, &pb);
  child.compute(ctx, &cb, &parent);
  EXPECT_EQ(child.value("font-weight"), "10");
  EXPECT_EQ(child.value("font-width"), "10%");
  EXPECT_EQ(child.value("font-size-adjust"), "ex-height 10");
  EXPECT_EQ(child.value("width"), "200px") << "width is not a font-affecting property";
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
   strictly ascending order". The text is silent on whether a computed value shows the positions it supplies when evaluating ("If the control point originally lacked an input
   progress value, return s"); both pinned browsers write them (tools/conformance/css_browser_answers.jsonl, cases linear.*), so the standing rule fills them in. */
TEST(CssComputedCompletionEasing, LinearSplitsDoublePositionsAndAscends) {
  struct Row {
    const char * in;
    const char * out;
  };
  for (const Row & r : std::vector<Row>{{"linear(0 0% 50%, 1 50% 100%)", "linear(0 0%, 0 50%, 1 50%, 1 100%)"}, {"linear(0 20%, 0.5 10%, 1)", "linear(0 20%, 0.5 20%, 1 100%)"},
           {"linear(0, 0.25 25% 75%, 1)", "linear(0 0%, 0.25 25%, 0.25 75%, 1 100%)"}, {"linear(0, 0.5 25% 75%, 1 100% 100%)", "linear(0 0%, 0.5 25%, 0.5 75%, 1 100%, 1 100%)"},
           {"linear(0, 1)", "linear(0 0%, 1 100%)"}, {"linear(0 10%, 1 90%)", "linear(0 10%, 1 90%)"}, {"ease-in, linear(0 50% 25%, 1)", "ease-in, linear(0 50%, 0 50%, 1 100%)"},
           {"linear(0, 0.5 20%, 0.7, 1)", "linear(0 0%, 0.5 20%, 0.7 60%, 1 100%)"}, {"linear(0, 0.25, 0.5, 0.75, 1)", "linear(0 0%, 0.25 25%, 0.5 50%, 0.75 75%, 1 100%)"},
           {"linear(0 30% 70%, 1)", "linear(0 30%, 0 70%, 1 100%)"}, {"linear(0 10%, 0.5, 1 90%)", "linear(0 10%, 0.5 50%, 1 90%)"},
           // a position that is a min(), max() or clamp() of percentages is the percentage it picks (both browsers, case linear.7)
           {"linear(0 calc(min(50%, 60%)), 0 100%)", "linear(0 50%, 0 100%)"}, {"linear(0 max(10%, 5%), 1)", "linear(0 10%, 1 100%)"},
           {"linear(0, 1 clamp(10%, 5%, 20%))", "linear(0 0%, 1 10%)"}}) {
    EXPECT_EQ(root_computed(std::string("animation-timing-function: ") + r.in, "animation-timing-function"), r.out) << r.in;
    EXPECT_EQ(root_computed(std::string("transition-timing-function: ") + r.in, "transition-timing-function"), r.out) << r.in;
  }
}

/* Easing 2, 2.3: "If the <step-position> is jump-none, the <integer> must be at least 2, or the function is invalid" and, for the others, "at least 1, or the function
   is invalid": a literal count below the least is not a steps(), so the declaration is dropped (both pinned browsers refuse it too: cases steps.*). */
TEST(CssComputedCompletionEasing, StepsBelowTheLeastAreInvalid) {
  for (const char * bad : {"steps(1, jump-none)", "steps(0, jump-none)", "steps(0)", "steps(0, jump-start)", "steps(-1, end)"}) {
    EXPECT_EQ(root_computed(std::string("animation-timing-function: ") + bad, "animation-timing-function"), "ease") << bad;
  }
  EXPECT_EQ(root_computed("animation-timing-function: steps(2, jump-none)", "animation-timing-function"), "steps(2, jump-none)");
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

/* The standing rule: where the served text is silent, the recorded answers of both pinned browsers decide (tools/conformance/css_browser_answers.jsonl; the case ids are
   in each comment). Each row below is one the two browsers answer alike. */
TEST(CssComputedCompletionBrowserRecorded, OpacityOfPercentagesInMinMaxClamp) {
  // cases opacity.*: Color 4's <alpha-value> is "<number> | <percentage>", the computed value "specified number, clamped to the range [0,1]"
  for (const auto & r : std::vector<std::pair<const char *, const char *>>{{"clamp(50%, 0%, 70%)", "0.5"}, {"clamp(0%, 120%, 70%)", "0.7"}, {"min(10%, 20%)", "0.1"},
           {"min(50%, 60%)", "0.5"}, {"calc(min(50%, 60%))", "0.5"}, {"max(20%, 10%)", "0.2"}, {"50%", "0.5"}, {"0.25", "0.25"}}) {
    EXPECT_EQ(root_computed(std::string("opacity: ") + r.first, "opacity"), r.second) << r.first;
  }
}

TEST(CssComputedCompletionBrowserRecorded, FontVariationSettingsAreSortedByTagAndTheLastOfARepeatedTagIsKept) {
  // cases fontvar.*
  for (const auto & r : std::vector<std::pair<const char *, const char *>>{{"\"wght\" 400, \"wdth\" 100", "\"wdth\" 100, \"wght\" 400"},
           {"\"wght\" 400, \"wght\" 500", "\"wght\" 500"}, {"\"wght\" 400, \"wdth\" 100, \"wght\" 300", "\"wdth\" 100, \"wght\" 300"},
           {"\"zzzz\" 1, \"aaaa\" 2", "\"aaaa\" 2, \"zzzz\" 1"}, {"normal", "normal"}}) {
    EXPECT_EQ(root_computed(std::string("font-variation-settings: ") + r.first, "font-variation-settings"), r.second) << r.first;
  }
}

TEST(CssComputedCompletionBrowserRecorded, GridTemplateAreasCollapseWhiteSpaceAndRunsOfFullStops) {
  // cases gta.*
  for (const auto & r : std::vector<std::pair<const char *, const char *>>{{"\"a   b\" \"c   d\"", "\"a b\" \"c d\""}, {"\"a ... b\"", "\"a . b\""},
           {"\"  a  .  b  \" \"c d e\"", "\"a . b\" \"c d e\""}, {"none", "none"}}) {
    EXPECT_EQ(root_computed(std::string("grid-template-areas: ") + r.first, "grid-template-areas"), r.second) << r.first;
  }
}

TEST(CssComputedCompletionBrowserRecorded, OffsetRotateReverseIsAutoHalfTurn) {
  // cases angle-other.*
  EXPECT_EQ(root_computed("offset-rotate: reverse", "offset-rotate"), "auto 180deg");
  EXPECT_EQ(root_computed("offset-rotate: reverse 10deg", "offset-rotate"), "auto 190deg");
  EXPECT_EQ(root_computed("offset-rotate: auto 30deg", "offset-rotate"), "auto 30deg");
  EXPECT_EQ(root_computed("offset-rotate: 45deg", "offset-rotate"), "45deg");
}

TEST(CssComputedCompletionBrowserRecorded, InsetShortensEqualOffsetsAndRadii) {
  // cases radii.*
  for (const auto & r : std::vector<std::pair<const char *, const char *>>{{"inset(0 round 10px 10px 10px 10px)", "inset(0px round 10px)"},
           {"inset(0 round 1px 2px 1px 2px)", "inset(0px round 1px 2px)"}, {"inset(1px round 3px 3px / 3px 3px)", "inset(1px round 3px)"},
           {"inset(10px 10px 10px 10px)", "inset(10px)"}, {"inset(10px 20px 10px 20px)", "inset(10px 20px)"}, {"inset(0 round 5px / 5px 5px 5px 5px)", "inset(0px round 5px)"},
           {"inset(10px round 10px 20px / 10px 20px)", "inset(10px round 10px 20px)"}, {"inset(10px round 5px / 10px)", "inset(10px round 5px / 10px)"},
           {"inset(1px round 1px 2px 3px 4px / 5px 6px)", "inset(1px round 1px 2px 3px 4px / 5px 6px)"}}) {
    EXPECT_EQ(root_computed(std::string("clip-path: ") + r.first, "clip-path"), r.second) << r.first;
  }
}

TEST(CssComputedCompletionBrowserRecorded, StepsCountsBelowTheLeastAreInvalid) {
  // cases steps.*; Easing 2, 2.3
  for (const char * bad : {"steps(1, jump-none)", "steps(0, jump-none)", "steps(0)", "steps(0, jump-start)", "steps(-1, end)"}) {
    EXPECT_EQ(root_computed(std::string("transition-timing-function: ") + bad, "transition-timing-function"), "ease") << bad;
  }
  EXPECT_EQ(root_computed("transition-timing-function: steps(2, jump-none)", "transition-timing-function"), "steps(2, jump-none)");
  EXPECT_EQ(root_computed("transition-timing-function: steps(1, jump-both)", "transition-timing-function"), "steps(1, jump-both)");
}

} // namespace
