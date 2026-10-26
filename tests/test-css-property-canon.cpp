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
 * The canonical form of a specified value (src/css/css_property_canon.c): a value is written the way the standard's tests and the browsers
 * write it, in the shortest form (`display: block flow` is `block`), and what is written parses and is written the same again. Each
 * row is a property, a value as an author writes it and how it is written; the rules are the ones of the file and the tests of
 * web-platform-tests that give them.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_registry_test_support.h"
#include "markup_test_support.h"

using namespace reg;

namespace {

struct Row {
  const char * property;
  const char * input;
  const char * canonical;
};

const Row kRows[] = {
    {"display", "block flow", "block"},
    {"display", "inline flow", "inline"},
    {"display", "flow", "block"},
    {"display", "inline flow-root", "inline-block"},
    {"display", "block flow-root", "flow-root"},
    {"display", "flex inline", "inline-flex"},
    {"display", "table block", "table"},
    {"display", "inline ruby", "ruby"},
    {"display", "block ruby", "block ruby"},
    {"display", "list-item flow", "list-item"},
    {"display", "inline flow list-item", "inline list-item"},
    {"display", "flow-root list-item block", "flow-root list-item"},
    {"display", "run-in flow", "run-in"},
    {"display", "run-in flex", "run-in flex"},
    {"display", "none", "none"},
    {"display", "contents", "contents"},
    {"display", "inline-block", "inline-block"},
    {"align-content", "first baseline", "baseline"},
    {"align-content", "last baseline", "last baseline"},
    {"justify-self", "first baseline", "baseline"},
    {"animation-timing-function", "steps(2, end)", "steps(2)"},
    {"animation-timing-function", "steps(2, jump-end)", "steps(2)"},
    {"animation-timing-function", "steps(2, start)", "steps(2, start)"},
    {"transition-timing-function", "ease, steps(3, end)", "ease, steps(3)"},
    {"animation-name", "\"something\"", "something"},
    {"animation-name", "\"multi word string\"", "multi\\ word\\ string"},
    {"animation-name", "\"none\"", "\"none\""},
    {"animation-name", "a, \"b\"", "a, b"},
    {"background-size", "1px", "1px auto"},
    {"background-size", "auto auto", "auto"},
    {"background-size", "cover", "cover"},
    {"background-size", "auto", "auto"},
    {"background-size", "1px 2px", "1px 2px"},
    {"background-size", "1px, auto auto", "1px auto, auto"},
    {"mask-size", "10% ", "10% auto"},
    {"mask-repeat", "repeat no-repeat", "repeat-x"},
    {"mask-repeat", "no-repeat repeat", "repeat-y"},
    {"mask-repeat", "space space", "space"},
    {"mask-repeat", "repeat, no-repeat repeat", "repeat, repeat-y"},
    {"mask-repeat", "space round", "space round"},
    {"border-image-repeat", "space space", "space"},
    {"border-image-repeat", "stretch round", "stretch round"},
    {"scroll-snap-align", "start start", "start"},
    {"scroll-snap-align", "start end", "start end"},
    {"position-area", "center center", "center"},
    {"position-area", "top left", "left top"},
    {"mask-position", "left", "left center"},
    {"mask-position", "top", "center top"},
    {"mask-position", "center", "center center"},
    {"mask-position", "10%", "10% center"},
    {"mask-position", "left top", "left top"},
    {"mask-position", "top, center, left", "center top, center center, left center"},
    {"object-position", "right", "right center"},
    {"perspective-origin", "bottom", "center bottom"},
    {"background-position", "left", "left center"},
    {"color-scheme", "only light", "light only"},
    {"color-scheme", "light dark only", "light dark only"},
    {"color-scheme", "normal", "normal"},
    {"counter-reset", "chapter", "chapter 0"},
    {"counter-reset", "chapter 5", "chapter 5"},
    {"counter-reset", "a b 3", "a 0 b 3"},
    {"counter-reset", "reversed(c)", "reversed(c)"},
    {"counter-reset", "none", "none"},
    {"counter-reset", "section calc(1)", "section calc(1)"},
    {"counter-reset", "chapter reversed(chapter)", "chapter 0 reversed(chapter)"},
    {"counter-increment", "chapter", "chapter 1"},
    {"counter-set", "chapter -2 b", "chapter -2 b 0"},
    {"filter", "grayscale(300%)", "grayscale(100%)"},
    {"filter", "hue-rotate(0)", "hue-rotate(0deg)"},
    {"filter", "opacity(2)", "opacity(1)"},
    {"filter", "brightness(300%)", "brightness(300%)"},
    {"filter", "blur(2px) invert(500%)", "blur(2px) invert(100%)"},
    {"filter", "sepia(50%)", "sepia(50%)"},
    {"filter", "grayscale(100%)", "grayscale(100%)"},
    {"filter", "grayscale(100.5%)", "grayscale(100%)"},
    {"backdrop-filter", "sepia(300%)", "sepia(100%)"},
    {"font-family", "Serif", "serif"},
    {"font-family", "\"Serif\"", "\"Serif\""},
    {"font-family", "Georgia, SANS-SERIF", "Georgia, sans-serif"},
    {"font-feature-settings", "\"dlig\" 1", "\"dlig\""},
    {"font-feature-settings", "\"smcp\" on", "\"smcp\""},
    {"font-feature-settings", "\"liga\" off", "\"liga\" 0"},
    {"font-feature-settings", "\"a\" 2, \"b\" 1", "\"a\" 2, \"b\""},
    {"font-language-override", "\"ENG \"", "\"ENG\""},
    {"font-language-override", "\" en \"", "\" en\""},
    {"font-size-adjust", "ex-height 0.5", "0.5"},
    {"font-size-adjust", "cap-height 0.5", "cap-height 0.5"},
    {"font-size-adjust", "ex-height from-font", "from-font"},
    {"font-style", "oblique 0deg", "normal"},
    {"font-style", "oblique 10deg", "oblique 10deg"},
    {"font-style", "italic", "italic"},
    {"grid-auto-flow", "row dense", "dense"},
    {"grid-auto-flow", "column dense", "column dense"},
    {"grid-auto-flow", "row", "row"},
    {"grid-column-start", "2 SpAn", "span 2"},
    {"grid-row-end", "span 3", "span 3"},
    {"grid-column-end", "auto", "auto"},
    {"grid-template-columns", "[] 150px [] 1fr []", "150px 1fr"},
    {"grid-template-rows", "repeat(1, [] 10px [])", "repeat(1, 10px)"},
    {"grid-template-columns", "[a] 1fr [b]", "[a] 1fr [b]"},
    {"grid-template-rows", "subgrid []", "subgrid []"},
    {"list-style-type", "symbols(symbolic \"a\")", "symbols(\"a\")"},
    {"list-style-type", "symbols(cyclic \"a\")", "symbols(cyclic \"a\")"},
    {"scroll-snap-type", "x proximity", "x"},
    {"scroll-snap-type", "x mandatory", "x mandatory"},
    {"opacity", "50%", "0.5"},
    {"opacity", "-100%", "-1"},
    {"opacity", "300%", "3"},
    {"opacity", "0.5", "0.5"},
    {"shape-image-threshold", "50%", "0.5"},
    {"translate", "100px 0px", "100px"},
    {"translate", "100px 200px 0px", "100px 200px"},
    {"translate", "1px 2px 0", "1px 2px"},
    {"translate", "0px 5px", "0px 5px"},
    {"translate", "0px 0px 5px", "0px 0px 5px"},
    {"rotate", "0 1 0 400grad", "y 400grad"},
    {"rotate", "-1 0 0 400grad", "x -400grad"},
    {"rotate", "0 0 1 400grad", "400grad"},
    {"rotate", "z 400grad", "400grad"},
    {"rotate", "0 0 -0.5 400grad", "-400grad"},
    {"rotate", "1 1 0 10deg", "1 1 0 10deg"},
    {"rotate", "x 5deg", "x 5deg"},
    {"rotate", "90deg", "90deg"},
    {"scale", "100%", "1"},
    {"scale", "100 100", "100"},
    {"scale", "100% 200% 1", "1 2"},
    {"scale", "1 2 3", "1 2 3"},
    {"scale", "1 1.0", "1"},
    {"scale", "50% 25%", "0.5 0.25"},
    {"transform", "scale3d(50%, 250%, 300%)", "scale3d(0.5, 2.5, 3)"},
    {"transform", "rotate(0)", "rotate(0deg)"},
    {"transform", "skew(0)", "skew(0deg)"},
    {"transform", "skew(90deg, 0)", "skew(90deg)"},
    {"transform", "skew(0, -90deg)", "skew(0deg, -90deg)"},
    {"transform", "translate(10px) rotate(0)", "translate(10px) rotate(0deg)"},
    {"transform", "scale(2)", "scale(2)"},
    {"text-box", "none", "normal"},
    {"text-box", "auto", "trim-both"},
    {"text-box", "trim-start auto", "trim-start"},
    {"text-box", "trim-both text", "text"},
    {"text-box", "text text trim-end", "trim-end text"},
    {"text-box", "none auto", "normal"},
    {"text-box", "trim-both cap alphabetic", "cap alphabetic"},
    {"margin-trim", "block-start block-end", "block"},
    {"margin-trim", "block-start", "block-start"},
    {"flex-wrap", "wrap balance", "balance"},
    {"flex-wrap", "wrap-reverse balance", "wrap-reverse balance"},
    {"object-fit", "contain scale-down", "scale-down"},
    {"object-fit", "cover scale-down", "cover scale-down"},
    {"color-interpolation-filters", "sRGB", "srgb"},
    {"color-interpolation-filters", "LiNeArRgB", "linearrgb"},
    {"cursor", "image-set(\"a.png\" 1x) 5 6, grab", "image-set(url(\"a.png\") 1x) 5 6, grab"},
    {"cursor", "pointer", "pointer"},
    {"clip-path", "circle(closest-side)", "circle()"},
    {"clip-path", "circle(closest-side at 20px 30%)", "circle(at 20px 30%)"},
    {"clip-path", "circle(farthest-side at center)", "circle(farthest-side at center center)"},
    {"clip-path", "circle(at left)", "circle(at left center)"},
    {"clip-path", "ellipse(at 10% 20%)", "ellipse(at 10% 20%)"},
    {"clip-path", "polygon(nonzero, 1px 2px, 3px 4px)", "polygon(1px 2px, 3px 4px)"},
    {"clip-path", "polygon(evenodd, 1px 2px, 3px 4px)", "polygon(evenodd, 1px 2px, 3px 4px)"},
    {"clip-path", "polygon(round 0px, 1px 2px, 3px 4px)", "polygon(1px 2px, 3px 4px)"},
    {"clip-path", "polygon(nonzero round 1px, 1px 2px, 3em 4em)", "polygon(round 1px, 1px 2px, 3em 4em)"},
    {"clip-path", "xywh(0px 1% 2px 3em round 0)", "xywh(0px 1% 2px 3em)"},
    {"clip-path", "inset(1px round 2px)", "inset(1px round 2px)"},
    {"clip-path", "path(nonzero, \"M20,20h60 v60 h-60z\")", "path(\"M 20 20 h 60 v 60 h -60 Z\")"},
    {"clip-path", "path(evenodd, \"M10,10h80z\")", "path(evenodd, \"M 10 10 h 80 Z\")"},
    {"clip-path", "path(\"m 10.0 170.0 h 90.00\")", "path(\"m 10 170 h 90\")"},
    {"shape-outside", "circle(20px at center)", "circle(20px at center center)"},
    {"offset-path", "circle(closest-side)", "circle()"},
};

TEST(CssPropertyCanon, EveryRowIsWrittenInItsCanonicalForm) {
  const GTEXT_CSS_Registry * r = shared();
  for (const Row & row : kRows) {
    EXPECT_EQ(parse(r, row.property, row.input), std::string("valid:") + row.canonical) << row.property << ": " << row.input;
  }
}

TEST(CssPropertyCanon, ACanonicalFormIsReadAndWrittenTheSameAgain) {
  const GTEXT_CSS_Registry * r = shared();
  for (const Row & row : kRows) {
    EXPECT_EQ(parse(r, row.property, row.canonical), std::string("valid:") + row.canonical) << row.property << ": " << row.canonical;
  }
}

TEST(CssPropertyCanon, ACssWideKeywordAndASubstitutionAreNotRewritten) {
  const GTEXT_CSS_Registry * r = shared();
  EXPECT_EQ(parse(r, "display", "INHERIT"), "wide:inherit");
  EXPECT_EQ(parse(r, "display", "var(--d)"), "pending:var(--d)");
  EXPECT_EQ(parse(r, "mask-position", "left var(--p)"), "pending:left var(--p)");
  EXPECT_EQ(parse(r, "scale", "env(x) 100%"), "pending:env(x) 100%");
}

TEST(CssPropertyCanon, ACustomPropertyAndADescriptorHaveNoRules) {
  const GTEXT_CSS_Registry * r = shared();
  EXPECT_EQ(parse(r, "--display", "block flow"), "valid:block flow");
  const GTEXT_CSS_Property_Options face = descriptor_of("@font-face");
  EXPECT_EQ(parse(r, "font-feature-settings", "\"dlig\" 1", &face), "valid:\"dlig\" 1") << "a descriptor is written as it is matched";
}

TEST(CssPropertyCanon, AnAliasIsWrittenAsItsTargetIs) {
  const GTEXT_CSS_Registry * r = shared();
  EXPECT_EQ(parse(r, "-webkit-align-content", "first baseline"), "valid:baseline");
  EXPECT_EQ(parse(r, "-webkit-mask-position", "left"), "valid:left center");
}

TEST(CssPropertyCanon, TheSinkAndTheBufferWriteTheSameBytes) {
  const GTEXT_CSS_Registry * r = shared();
  for (const Row & row : kRows) {
    GTEXT_CSS_Property_Value pv;
    ASSERT_EQ(gtext_css_parse_property_text(r, row.property, strlen(row.property), row.input, strlen(row.input), nullptr, &pv), GTEXT_CSS_OK)
        << row.property << ": " << row.input;
    char * buf = nullptr;
    size_t n = 0;
    ASSERT_EQ(gtext_css_property_value_serialize_buffer(&pv, nullptr, &buf, &n), GTEXT_CSS_OK);
    std::string via_sink;
    GTEXT_Markup_Sink sink;
    sink.user = &via_sink;
    sink.write = [](void * user, const char * bytes, size_t len) {
      static_cast<std::string *>(user)->append(bytes, len);
      return GTEXT_MARKUP_OK;
    };
    ASSERT_EQ(gtext_css_property_value_serialize(&pv, nullptr, &sink), GTEXT_CSS_OK);
    EXPECT_EQ(std::string(buf, n), via_sink) << row.property << ": " << row.input;
    EXPECT_EQ(via_sink, row.canonical);
    gtext_css_serialize_buffer_free(nullptr, buf);
    gtext_css_property_value_free(&pv);
  }
}

}  // namespace
