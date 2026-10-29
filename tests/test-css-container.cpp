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
 * Container queries (css_container.h): the parse of `@container` conditions, the query container of an element, the size, style, scroll-state and anchored features,
 * and the sheets and indexes that carry them. Each rule is quoted from Conditional 5 (served 2026-10-28) in the test that checks it.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_container_test_support.h"

using cq::Doc;

namespace {

std::string serialized(const char * css) {
  GTEXT_CSS_Container_Query * q = nullptr;
  GTEXT_CSS_Media_Result r;
  std::memset(&r, 0, sizeof r);
  EXPECT_EQ(gtext_css_container_parse(css, std::strlen(css), nullptr, &q, &r), GTEXT_CSS_OK) << css;
  char * buf = nullptr;
  size_t n = 0;
  EXPECT_EQ(gtext_css_media_list_serialize_buffer(q, nullptr, &buf, &n), GTEXT_CSS_OK);
  std::string out(buf, n);
  gtext_css_media_buffer_free(nullptr, buf);
  gtext_css_media_list_free(q);
  gtext_css_media_result_free(&r);
  return out;
}

/// Whether the text is a valid `<container-query-list>` (the parse is OK and no condition was replaced).
bool valid(const std::string & css) {
  GTEXT_CSS_Container_Query * q = nullptr;
  GTEXT_CSS_Media_Result r;
  std::memset(&r, 0, sizeof r);
  const GTEXT_CSS_Status st = gtext_css_container_parse(css.data(), css.size(), nullptr, &q, &r);
  const bool ok = st == GTEXT_CSS_OK && r.replaced == 0;
  gtext_css_media_list_free(q);
  gtext_css_media_result_free(&r);
  return ok;
}

}  // namespace

TEST(CssContainerParse, TheGrammarOfTheConditionList) {
  /* Conditional 5, 5.4: `@container <container-query-list>`, `<container-condition># = [ <container-name>? <container-query>? ]!`. */
  for (const char * ok : {"(width)", "(min-width: 0px)", "(max-width: 0px)", "(height)", "(aspect-ratio)", "(min-aspect-ratio: 1/2)", "(orientation: portrait)", "(inline-size)",
           "(min-inline-size: 0px)", "(block-size)", "(max-block-size: 0px)", "(width: 100px)", "((width: 100px))", "(not (width: 100px))", "((width: 100px) and (height: 100px))",
           "(((width: 40px) or (width: 50px)) and (height: 100px))", "(width < 100px)", "(width >= 100px)", "(100px < width)", "(100px < width <= 200px)", "(100px > width >= 200px)",
           "(width: calc(10px))", "(width < max(10px, 10em))", "(width: 100px) and (height: 100px)", "(width: 100px) or (height: 100px)", "not (width: 100px)", "name not (width <= 500px)",
           "(width: 100px), (height: 100px)", "(width),(height)  , (inline-size > 20px)", "(width), name (height)", "--foo", "container", "container, container2", "normal", "auto"}) {
    EXPECT_TRUE(valid(ok)) << ok;
  }
  /* A name alone, a query alone and both are valid; nothing at all, an empty entry and a stray comma are not. */
  for (const char * bad : {"", "(width),", ",(width)", "(width),,(height)", "screen and (width: 100px)", "screen or (width: 100px)", "not screen and (width: 100px)", "foo bar (width: 100px)",
           "foo foo", "1px", "\"foo\"", "inherit", "INITIAL", "Unset", "deFAULT", "none", "None", "and", "or", "and (width)", "or (width)", "(width) and (height) or (width)"}) {
    EXPECT_FALSE(valid(bad)) << bad;
  }
  /* "The keywords none, and, not, and or are excluded from this <custom-ident>": `not` begins the query. */
  EXPECT_TRUE(valid("not (width)"));
  EXPECT_FALSE(valid("not not (width)"));
}

TEST(CssContainerParse, AnUnknownFeatureIsGeneralEnclosedAndSelectsNoContainer) {
  /* "If the <container-query> contains unknown or unsupported container features, no query container will be selected for that <container-condition>." They parse. */
  for (const char * unknown : {"foo(width)", "size(width)", "(asdf)", "(resolution > 100dpi)", "(resolution: 150dpi)", "(color)", "(min-color: 1)", "(grid)", "(width == 100px)", "(100px == width)",
           "(100px = width = 200px)", "(100px < width > 200px)", "(100px <= width >= 200px)", "(100px : width : 200px)"}) {
    EXPECT_TRUE(valid(std::string("name ") + unknown)) << unknown;
    GTEXT_CSS_Container_Query * q = nullptr;
    const std::string css = std::string("name ") + unknown;
    ASSERT_EQ(gtext_css_container_parse(css.data(), css.size(), nullptr, &q, nullptr), GTEXT_CSS_OK);
    EXPECT_TRUE(gtext_css_container_query_kinds(q, 0) & GTEXT_CSS_CONTAINER_ASKS_UNKNOWN) << unknown;
    gtext_css_media_list_free(q);
  }
}

TEST(CssContainerParse, SerializationIsCanonicalAndAFixedPoint) {
  EXPECT_EQ(serialized("card (min-width:400px) and (max-width:  900px)"), "card (min-width: 400px) and (max-width: 900px)");
  EXPECT_EQ(serialized("(INLINE-SIZE > 10px), NAME"), "(inline-size > 10px), NAME");
  EXPECT_EQ(serialized("STYLE(--foo: bar)"), "style(--foo: bar)");
  EXPECT_EQ(serialized("style(--a: 1) and style(--b)"), "style(--a: 1) and style(--b)");
  EXPECT_EQ(serialized("not style(--b)"), "not style(--b)");
  EXPECT_EQ(serialized("style((--a: 1) or (--b: 2))"), "style((--a: 1) or (--b: 2))");
  EXPECT_EQ(serialized("scroll-state(stuck: top)"), "scroll-state(stuck: top)");
  EXPECT_EQ(serialized("scroll-state((stuck: top) and (scrollable: block-end))"), "scroll-state((stuck: top) and (scrollable: block-end))");
  EXPECT_EQ(serialized("anchored(fallback: --a flip-block)"), "anchored(fallback: --a flip-block)");
  EXPECT_EQ(serialized("(100px<=width<200px)"), "(100px <= width < 200px)");
  EXPECT_EQ(serialized("(aspect-ratio: 16/9)"), "(aspect-ratio: 16 / 9)");
  for (const char * css : {"card (min-width: 400px) and (max-width: 900px)", "style(--a: 1 < 2)", "style(--a > 5)", "(orientation: landscape), --x", "not (width)"}) {
    const std::string once = serialized(css);
    EXPECT_EQ(serialized(once.c_str()), once) << css;
  }
  /* an invalid rule is `not all` */
  EXPECT_EQ(serialized("screen"), "screen") << "a name";
  EXPECT_EQ(serialized("(width),"), "not all");
}

TEST(CssContainerResolve, TheNearestAncestorThatIsAContainerAnswers) {
  /* Conditional 5, 5.4: a container query is true for an element if it is true for its query container, the nearest ancestor that is a container for the features asked. */
  Doc d;
  const int outer = d.add(0, "div", "outer");
  const int inner = d.add(outer, "div", "inner");
  const int leaf = d.add(inner, "p", "leaf");
  d.own[outer] = "container-type: inline-size";
  d.own[inner] = "container-type: inline-size";
  d.boxes[outer] = {500, 300};
  d.boxes[inner] = {200, 100};
  d.sheets = {"#leaf { color: red } @container (min-width: 400px) { #leaf { color: green } } @container (max-width: 250px) { #leaf { background-color: blue } }"};
  d.build();
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  EXPECT_EQ(d.get(leaf, "color"), "red") << "the inner container is 200px wide: the outer one is not asked";
  EXPECT_EQ(d.get(leaf, "background-color"), "blue");
  d.boxes[inner] = {450, 100};
  d.build();
  EXPECT_EQ(d.get(leaf, "color"), "green");
  EXPECT_NE(d.get(leaf, "background-color"), "blue");
}

TEST(CssContainerResolve, ANamedQueryFiltersTheContainers) {
  /* "The <container-name> filters the set of query containers considered to just those with a matching query container name." */
  Doc d;
  const int far = d.add(0, "div", "far");
  const int near = d.add(far, "div", "near");
  const int leaf = d.add(near, "p", "leaf");
  d.own[far] = "container-type: inline-size; container-name: card";
  d.own[near] = "container-type: inline-size";
  d.boxes[far] = {600, 100};
  d.boxes[near] = {100, 100};
  d.sheets = {"#leaf { color: red } @container card (min-width: 500px) { #leaf { color: green } } @container (min-width: 500px) { #leaf { background-color: blue } }"};
  d.build();
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  EXPECT_EQ(d.get(leaf, "color"), "green") << "the card container answers, not the nearer unnamed one";
  EXPECT_NE(d.get(leaf, "background-color"), "blue") << "the nearer one is 100px wide";
}

TEST(CssContainerResolve, NoContainerIsUnknownSoFalseAndANameAloneNeedsOne) {
  /* "If no ancestor is an eligible query container, then the container query is unknown for that element." */
  Doc d;
  const int a = d.add(0, "div", "a");
  const int leaf = d.add(a, "p", "leaf");
  d.sheets = {"#leaf { color: red } @container (width > 1px) { #leaf { color: green } } @container (not (width > 1px)) { #leaf { background-color: blue } } @container card { #leaf { margin-top: 1px } }"};
  d.build();
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  EXPECT_EQ(d.get(leaf, "color"), "red");
  EXPECT_NE(d.get(leaf, "background-color"), "blue") << "not unknown is unknown";
  d.own[a] = "container-type: size; container-name: card";
  d.boxes[a] = {10, 10};
  d.build();
  EXPECT_EQ(d.get(leaf, "margin-top"), "1px") << "a name alone: eligible as long as the name matches";
}

TEST(CssContainerResolve, AFeatureOfAnAxisTheContainerDoesNotContainIsUnknown) {
  /* "...or the query container does not support container size queries on the relevant axes, then the result of evaluating the size feature is unknown." */
  Doc d;
  const int c = d.add(0, "div", "c");
  const int leaf = d.add(c, "p", "leaf");
  d.own[c] = "container-type: inline-size";
  d.boxes[c] = {500, 300};
  d.sheets = {"#leaf { color: red } @container (height > 100px) { #leaf { color: green } } @container (not (height > 100px)) { #leaf { background-color: blue } } "
              "@container (width > 100px) { #leaf { margin-top: 2px } } @container (block-size > 100px) { #leaf { margin-left: 3px } } @container (inline-size > 100px) { #leaf { margin-right: 4px } } "
              "@container (aspect-ratio > 1) { #leaf { margin-bottom: 5px } } @container (orientation: landscape) { #leaf { padding-top: 6px } }"};
  d.build();
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  EXPECT_EQ(d.get(leaf, "color"), "red");
  EXPECT_NE(d.get(leaf, "background-color"), "blue") << "not unknown is unknown";
  EXPECT_EQ(d.get(leaf, "margin-top"), "2px");
  EXPECT_NE(d.get(leaf, "margin-left"), "3px") << "the block axis is not contained";
  EXPECT_EQ(d.get(leaf, "margin-right"), "4px");
  EXPECT_NE(d.get(leaf, "margin-bottom"), "5px") << "the ratio needs both axes";
  EXPECT_NE(d.get(leaf, "padding-top"), "6px");
}

TEST(CssContainerResolve, LogicalFeaturesFollowTheWritingModeOfTheContainer) {
  /* inline-size is the size in the container's inline axis: the height of a vertical writing mode. */
  Doc d;
  const int c = d.add(0, "div", "c");
  const int leaf = d.add(c, "p", "leaf");
  d.own[c] = "container-type: size; writing-mode: vertical-rl";
  d.boxes[c] = {100, 700};
  d.sheets = {"@container (inline-size > 500px) { #leaf { color: green } } @container (block-size < 200px) { #leaf { background-color: blue } } @container (width < 200px) { #leaf { margin-top: 1px } }"};
  d.build();
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  EXPECT_EQ(d.get(leaf, "color"), "green");
  EXPECT_EQ(d.get(leaf, "background-color"), "blue");
  EXPECT_EQ(d.get(leaf, "margin-top"), "1px");
}

TEST(CssContainerResolve, ASizeQueryWithoutTheContentBoxCallbackIsUnsupportedNamingIt) {
  Doc d;
  const int c = d.add(0, "div", "c");
  const int leaf = d.add(c, "p", "leaf");
  d.own[c] = "container-type: size";
  d.give_content_box = false;
  d.sheets = {"@container (width > 1px) { #leaf { color: green } }"};
  d.build();
  EXPECT_EQ(d.status, GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_EQ(d.last_name, "container.size");
  (void)leaf;
}

TEST(CssContainerResolve, AContainerWithNoPrincipalBoxAnswersUnknown) {
  Doc d;
  const int c = d.add(0, "div", "c");
  const int leaf = d.add(c, "p", "leaf");
  d.own[c] = "container-type: size";
  d.sheets = {"#leaf { color: red } @container (width > 1px) { #leaf { color: green } }"};
  d.build(); /* no box for c */
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  EXPECT_EQ(d.get(leaf, "color"), "red");
}

TEST(CssContainerResolve, TheCostOfAWalkOverATreeIsTheRequestsTimesTheElements) {
  /* The resolution is memoized: the container of an element for a request is its parent's answer when the parent is not one, so n elements in a chain cost O(n) steps for each request. */
  for (int n : {50, 200, 800}) {
    Doc d;
    int parent = 0;
    std::string css;
    for (int i = 0; i < n; i++) {
      parent = d.add(parent, "div", "e" + std::to_string(i));
      if (i % 3 == 0) {
        d.own[parent] = std::string("container-type: inline-size; container-name: n") + std::to_string(i % 9);
        d.boxes[parent] = {100.0 + i, 50};
      }
    }
    const int leaf = parent;
    /* every div asks for its container, for four requests: the answer of an element is its parent's when the parent is not one, which is what the cache keeps */
    css = "@container n0 (min-width: 1px) { div { color: red } } @container n3 (min-width: 1px) { div { margin-top: 1px } } @container n6 (min-width: 1px) { div { margin-left: 1px } } "
          "@container (min-width: 1px) { div { margin-right: 1px } }";
    d.sheets = {css};
    d.build();
    ASSERT_EQ(d.status, GTEXT_CSS_OK);
    EXPECT_LE(d.call.container_requests, 4u);
    EXPECT_LE(d.call.container_steps, static_cast<size_t>(n) * 4 + 8) << "n = " << n << ": at most the elements times the requests";
    (void)leaf;
  }
}

TEST(CssContainerStyle, AStyleQueryComparesComputedValues) {
  /* Conditional 5, 6.2: "A <style-feature-plain> evaluates to true if the computed value of the given property on the query container matches the given value (which is also computed with
     respect to the query container)". "A style feature without a value evaluates to true if the computed value is different from the initial value." */
  Doc d;
  const int c = d.add(0, "div", "c");
  const int leaf = d.add(c, "p", "leaf");
  d.own[c] = "--theme: dark; --n: 7; color: red; font-size: 20px; --w: 2em";
  d.sheets = {"@container style(--theme: dark) { #leaf { margin-top: 1px } } @container style(--theme: light) { #leaf { margin-bottom: 1px } } "
              "@container style(--theme) { #leaf { margin-left: 1px } } @container style(--absent) { #leaf { margin-right: 1px } } @container style(color: red) { #leaf { padding-top: 1px } } "
              "@container style(color: blue) { #leaf { padding-bottom: 1px } } @container style(color) { #leaf { padding-left: 1px } } @container style(font-size: 20px) { #leaf { padding-right: 1px } } "
              "@container style(--n > 5) { #leaf { order: 1 } } @container style(--n < 5) { #leaf { flex-grow: 1 } } @container style(3 < --n <= 7) { #leaf { z-index: 1 } } "
              "@container style(font-size: 30px) { #leaf { text-indent: 5px } } @container style(--w: 2em) { #leaf { flex-shrink: 4 } } @container style(--w: 40px) { #leaf { outline-offset: 1px } } "
              "@container not style(--theme: light) { #leaf { top: 1px } } @container style((--theme: dark) and (color: red)) { #leaf { left: 1px } } @container style(--theme: dark) and style(color: blue) { #leaf { right: 1px } }"};
  d.build();
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  EXPECT_EQ(d.get(leaf, "margin-top"), "1px");
  EXPECT_NE(d.get(leaf, "margin-bottom"), "1px");
  EXPECT_EQ(d.get(leaf, "margin-left"), "1px") << "a custom property that is not the guaranteed-invalid value";
  EXPECT_NE(d.get(leaf, "margin-right"), "1px");
  EXPECT_EQ(d.get(leaf, "padding-top"), "1px");
  EXPECT_NE(d.get(leaf, "padding-bottom"), "1px");
  EXPECT_NE(d.get(leaf, "text-indent"), "5px") << "30px is not 20px, though the two are written with as many characters";
  EXPECT_EQ(d.get(leaf, "padding-left"), "1px") << "the color differs from its initial value";
  EXPECT_EQ(d.get(leaf, "padding-right"), "1px");
  EXPECT_EQ(d.get(leaf, "order"), "1") << "style-range: 7 > 5";
  EXPECT_NE(d.get(leaf, "flex-grow"), "1");
  EXPECT_EQ(d.get(leaf, "z-index"), "1") << "3 < 7 <= 7";
  EXPECT_EQ(d.get(leaf, "flex-shrink"), "4") << "2em is 40px on a 20px container: the value is computed on the container";
  EXPECT_NE(d.get(leaf, "outline-offset"), "1px");
  EXPECT_EQ(d.get(leaf, "top"), "1px");
  EXPECT_EQ(d.get(leaf, "left"), "1px");
  EXPECT_NE(d.get(leaf, "right"), "1px");
}

TEST(CssContainerStyle, ARevertedValueIsFalseAndAWideKeywordIsComputedOnTheContainer) {
  /* "Cascade-dependent keywords, such as revert and revert-layer, are invalid as values in a style feature, and cause the container style query to be false." "The remaining non-cascade-dependent
     CSS-wide keywords are computed with respect to the query container." */
  Doc d;
  const int c = d.add(0, "div", "c");
  const int leaf = d.add(c, "p", "leaf");
  d.own[c] = "color: red";
  d.sheets = {"@container style(color: revert) { #leaf { margin-top: 1px } } @container style(color: inherit) { #leaf { margin-bottom: 1px } } @container style(color: initial) { #leaf { margin-left: 1px } }"};
  d.build();
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  EXPECT_NE(d.get(leaf, "margin-top"), "1px");
  EXPECT_NE(d.get(leaf, "margin-bottom"), "1px") << "the container's parent (the root of the tree) has the initial color, the container red";
  EXPECT_NE(d.get(leaf, "margin-left"), "1px");
}

TEST(CssContainerScroll, ScrollStateQueriesReadThePhysicalStateThroughTheFlow) {
  /* Conditional 5, 6.3: "The logical edges map to physical based on the direction and writing-mode of the query container. None of the values match if the query container is not sticky positioned." */
  Doc d;
  const int c = d.add(0, "div", "c");
  const int leaf = d.add(c, "p", "leaf");
  d.own[c] = "container-type: scroll-state";
  GTEXT_CSS_Container_Scroll_State st;
  std::memset(&st, 0, sizeof st);
  st.sticky = true;
  st.stuck = GTEXT_CSS_EDGE_TOP;
  st.scroll_container = true;
  st.scrollable = GTEXT_CSS_EDGE_BOTTOM;
  d.scroll[c] = st;
  d.sheets = {"@container scroll-state(stuck: top) { #leaf { margin-top: 1px } } @container scroll-state(stuck: left) { #leaf { margin-left: 1px } } @container scroll-state(stuck: block-start) { #leaf { margin-right: 1px } } "
              "@container scroll-state(scrollable: block-end) { #leaf { margin-bottom: 1px } } @container scroll-state(scrollable: y) { #leaf { padding-top: 1px } } @container scroll-state(scrollable: x) { #leaf { padding-left: 1px } } "
              "@container scroll-state(stuck: none) { #leaf { padding-right: 1px } } @container scroll-state((stuck: top) and (scrollable: bottom)) { #leaf { padding-bottom: 1px } } "
              "@container (width > 1px) { #leaf { top: 1px } } @container scroll-state(snapped: none) { #leaf { left: 1px } }"};
  d.build();
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  EXPECT_EQ(d.get(leaf, "margin-top"), "1px");
  EXPECT_NE(d.get(leaf, "margin-left"), "1px");
  EXPECT_EQ(d.get(leaf, "margin-right"), "1px") << "block-start is top in horizontal-tb";
  EXPECT_EQ(d.get(leaf, "margin-bottom"), "1px");
  EXPECT_EQ(d.get(leaf, "padding-top"), "1px");
  EXPECT_NE(d.get(leaf, "padding-left"), "1px");
  EXPECT_NE(d.get(leaf, "padding-right"), "1px") << "the container is stuck, so `none` does not match";
  EXPECT_EQ(d.get(leaf, "padding-bottom"), "1px");
  EXPECT_NE(d.get(leaf, "top"), "1px") << "a size query needs a size container: this one is a scroll-state container";
  EXPECT_EQ(d.get(leaf, "left"), "1px") << "not a snap target";
}

TEST(CssContainerScroll, ScrollStateWithoutTheCallbackIsUnsupportedNamingIt) {
  Doc d;
  const int c = d.add(0, "div", "c");
  d.add(c, "p", "leaf");
  d.own[c] = "container-type: scroll-state";
  d.give_scroll = false;
  d.sheets = {"@container scroll-state(stuck: top) { #leaf { color: green } }"};
  d.build();
  EXPECT_EQ(d.status, GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_EQ(d.last_name, "container.scroll_state");
}

TEST(CssContainerAnchored, AnAnchoredQueryAsksTheCallerAboutTheFallback) {
  Doc d;
  const int c = d.add(0, "div", "c");
  const int leaf = d.add(c, "p", "leaf");
  d.own[c] = "container-type: normal";
  d.sheets = {"@container anchored(fallback: right) { #leaf { margin-top: 1px } } @container anchored(fallback: none) { #leaf { margin-left: 1px } } @container anchored(fallback) { #leaf { margin-right: 1px } }"};
  d.build();
  ASSERT_EQ(d.status, GTEXT_CSS_OK) << "no container has the anchored type: nothing is selected";
  EXPECT_NE(d.get(leaf, "margin-left"), "1px");
}

TEST(CssContainerPseudo, TheOriginatingElementOfAPseudoElementIsACandidateContainer) {
  /* Conditional 5, 5: "::before, ::after, ::marker, and ::backdrop query their originating elements". */
  Doc d;
  const int c = d.add(0, "div", "c");
  d.own[c] = "container-type: inline-size";
  d.boxes[c] = {100, 100};
  d.sheets = {"#c::before { content: 'x' } @container (width < 150px) { #c::before { color: green } } @container (width < 150px) { #c { background-color: blue } }"};
  d.build();
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  GTEXT_CSS_Computed_Style * before = d.compute(c, "::before", &st);
  ASSERT_EQ(st, GTEXT_CSS_OK);
  EXPECT_EQ(cq::value_of(before, "color"), "green") << "the container is the element itself";
  gtext_css_computed_free(before);
  EXPECT_NE(d.get(c, "background-color"), "blue") << "an element is never its own query container";
}

TEST(CssContainerStarting, StartingStyleRecordsTakePartOnlyWhenAsked) {
  /* Transitions 2: the starting style is the style the element would have had with the rules of @starting-style as ordinary rules. */
  Doc d;
  const int a = d.add(0, "div", "a");
  d.sheets = {"#a { opacity: 1 } @starting-style { #a { opacity: 0 } } #a { color: red }"};
  d.build();
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  EXPECT_EQ(d.get(a, "opacity"), "1");
  d.starting_style = true;
  d.build();
  EXPECT_EQ(d.get(a, "opacity"), "0") << "the later of the two in the cascade";
  EXPECT_EQ(d.get(a, "color"), "red");
}

TEST(CssContainerLimits, TooManyDistinctRequestsIsALimitNotAWrongAnswer) {
  Doc d;
  d.add(0, "div", "a");
  std::string css;
  for (int i = 0; i < 12; i++) {
    css += "@container n" + std::to_string(i) + " (width > 1px) { #a { color: red } } ";
  }
  d.sheets = {css};
  GTEXT_CSS_Conditions_Options co = gtext_css_conditions_options_default();
  co.max_container_requests = 8;
  /* the Doc builds its own conditions object; build the index by hand with this one */
  GTEXT_CSS_Conditions * c = nullptr;
  ASSERT_EQ(gtext_css_conditions_new(cq::R(), &co, &c), GTEXT_CSS_OK);
  GTEXT_CSS_Rule_Index_Options io = gtext_css_rule_index_options_default();
  io.conditions = c;
  GTEXT_CSS_Rule_Index * ix = nullptr;
  ASSERT_EQ(gtext_css_rule_index_new(cq::R(), &io, &ix), GTEXT_CSS_OK);
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_stylesheet, css);
  GTEXT_CSS_Sheet * s = nullptr;
  ASSERT_EQ(gtext_css_sheet_compile(p.tree, cq::R(), nullptr, &s, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Sheet_Registration reg;
  std::memset(&reg, 0, sizeof reg);
  reg.origin = GTEXT_CSS_ORIGIN_AUTHOR;
  GTEXT_CSS_Rule_Index_Result res;
  EXPECT_EQ(gtext_css_rule_index_add_sheet(ix, s, &reg, &res), GTEXT_CSS_E_LIMIT_CONTAINER);
  EXPECT_STREQ(res.name, "max_container_requests");
  gtext_css_sheet_free(s);
  gtext_css_rule_index_free(ix);
  gtext_css_conditions_free(c);
}

TEST(CssContainerCache, ACallThatIsSharedAnswersAsOneThatIsNot) {
  Doc d;
  const int a = d.add(0, "div", "a");
  const int b = d.add(a, "div", "b");
  const int c = d.add(b, "p", "c");
  d.own[a] = "container-type: inline-size; container-name: x";
  d.own[b] = "container-type: inline-size";
  d.boxes[a] = {300, 10};
  d.boxes[b] = {120, 10};
  d.sheets = {"@container x (width > 200px) { #c { color: green } } @container (width > 200px) { #c { background-color: blue } }"};
  d.build();
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  const std::string shared = d.get(c, "color") + "/" + d.get(c, "background-color");
  /* the same document with a fresh call for every element */
  GTEXT_CSS_Status st;
  gtext_css_condition_call_free(&d.call);
  GTEXT_CSS_Computed_Style * again = d.compute(c, nullptr, &st);
  ASSERT_EQ(st, GTEXT_CSS_OK);
  EXPECT_EQ(cq::value_of(again, "color") + "/" + cq::value_of(again, "background-color"), shared);
  gtext_css_computed_free(again);
  EXPECT_EQ(shared, "green/" + d.get(c, "background-color"));
  EXPECT_NE(d.get(c, "background-color"), "blue");
}

namespace {

bool eval_transient(Doc & d, const std::string & q, int element, GTEXT_CSS_Condition_Call * call, GTEXT_CSS_Status * st) {
  GTEXT_CSS_Container_Query * query = nullptr;
  EXPECT_EQ(gtext_css_container_parse(q.data(), q.size(), nullptr, &query, nullptr), GTEXT_CSS_OK);
  bool m = false;
  *st = gtext_css_conditions_evaluate_container(d.cond, query, synth::Tree::H(element), false, call, &m);
  gtext_css_media_list_free(query);
  return m;
}

}  // namespace

TEST(CssContainerCache, AQueryFreedAndAnotherMadeInItsPlaceIsNotAnsweredFromTheMemo) {
  /* The memo of a call is keyed by the serial of the list a query is of, not by its address: a parsed query that is freed leaves its address for the next
   * one (the allocator gives the same block back), and an answer remembered for the first must not be given to the second. 200 transient queries, true and
   * false alternately, one call, and the container named by a copy of the name the call owns. */
  Doc d;
  const int outer = d.add(0, "div", "outer");
  const int leaf = d.add(outer, "p", "leaf");
  d.own[outer] = "container-type: inline-size; container-name: card";
  d.boxes[outer] = {500, 300};
  d.build();
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  GTEXT_CSS_Condition_Call call;
  gtext_css_condition_call_init(&call);
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  int wrong = 0;
  for (int i = 0; i < 200; i++) {
    const bool want = (i % 2) != 0;
    /* a name of a different length each time, so that a borrowed name pointer would read freed memory (a sanitizer sees it) */
    const std::string name = i % 3 == 0 ? "card" : (i % 3 == 1 ? "card" : "");
    const std::string q = (name.empty() ? "" : name + " ") + (want ? "(width > 100px)" : "(width > 900px)");
    const bool got = eval_transient(d, q, leaf, &call, &st);
    ASSERT_EQ(st, GTEXT_CSS_OK);
    wrong += got != want;
  }
  EXPECT_EQ(wrong, 0);
  EXPECT_EQ(call.container_requests, 2u) << "two requests: any container, and the one named card";
  gtext_css_condition_call_free(&call);
}

TEST(CssContainerCache, AQueryThatIsKeptKeepsItsMemo) {
  /* The other side: the same parsed query asked twice by one call is answered once. */
  Doc d;
  const int outer = d.add(0, "div", "outer");
  const int leaf = d.add(outer, "p", "leaf");
  d.own[outer] = "container-type: inline-size";
  d.boxes[outer] = {500, 300};
  d.build();
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  GTEXT_CSS_Container_Query * query = nullptr;
  ASSERT_EQ(gtext_css_container_parse("(width > 100px)", 15, nullptr, &query, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Condition_Call call;
  gtext_css_condition_call_init(&call);
  bool m = false;
  ASSERT_EQ(gtext_css_conditions_evaluate_container(d.cond, query, synth::Tree::H(leaf), false, &call, &m), GTEXT_CSS_OK);
  EXPECT_TRUE(m);
  const size_t first = call.container_evaluations;
  ASSERT_EQ(gtext_css_conditions_evaluate_container(d.cond, query, synth::Tree::H(leaf), false, &call, &m), GTEXT_CSS_OK);
  EXPECT_TRUE(m);
  EXPECT_EQ(call.container_evaluations, first) << "answered from the memo";
  gtext_css_condition_call_free(&call);
  gtext_css_media_list_free(query);
}

TEST(CssContainerResolve, TheElementBitOfANodeIsReadForTheRecordsOwnNode) {
  /* The index keeps what it learned of each condition node (does it hold, does it depend on the element) by node, and a record reads the node it is inside: a
     container record that sits after records of other groups is still asked of its container. */
  Doc d;
  const int c = d.add(0, "div", "c");
  const int leaf = d.add(c, "p", "leaf", {"leaf"});
  d.own[c] = "container-type: inline-size";
  d.boxes[c] = {500, 100};
  d.sheets = {".a { color: red } @media (min-width: 1px) { .b { margin-top: 1px } .leaf { margin-left: 2px } } @container (min-width: 99999px) { .leaf { padding-top: 7px } } "
              "@container (min-width: 1px) { .leaf { padding-left: 8px } } @media (min-width: 2px) { .d { margin-right: 3px } }"};
  d.build();
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  EXPECT_EQ(d.get(leaf, "margin-left"), "2px");
  EXPECT_NE(d.get(leaf, "padding-top"), "7px") << "the container is 500px wide";
  EXPECT_EQ(d.get(leaf, "padding-left"), "8px");
}
