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
 * The cascade (css_cascade.h): the edge-case matrix of its specification, one test for each criterion of the sort order with the sentence of the draft
 * it rests on (Cascade 5, 6.1, and Cascade 6, 3.1), the rollbacks, `all`, shorthands, logical groups, the output (provenance, losers, canonical order),
 * the hand-off to the computed-value layer, and a permutation test: shuffling the candidates of an element never changes the answer.
 */

#include <algorithm>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "css_cascade_test_support.h"
#include "css_test_support.h"

using namespace cas;

namespace {

/// The colors the tests use as marks: eight distinct values of `color`.
const char * const kMarks[] = {"red", "orange", "yellow", "green", "blue", "indigo", "violet", "black"};

/// The index of the winner of a property among the winners, or the count when it has none.
size_t index_of(const Out & o, const std::string & property) {
  const GTEXT_CSS_Property_Id id = find(R(), property);
  size_t n = gtext_css_cascaded_count(o.c);
  for (size_t i = 0; i < n; i++) {
    GTEXT_CSS_Cascade_Entry e;
    gtext_css_cascaded_at(o.c, i, &e, nullptr);
    if (e.property == id) {
      return i;
    }
  }
  return n;
}

/* ---- specificity --------------------------------------------------------------------------------------------- */

TEST(CssCascadeSpecificity, ComponentsSaturateAndComparisonIsLexicographic) {
  /* Selectors 4, specificity: (A, B, C) compared "from left to right" ("The declaration with the highest specificity wins"). Decided: saturation. */
  const GTEXT_CSS_Specificity big = gtext_css_specificity_make(uint64_t(1) << 40, 0, UINT32_MAX);
  EXPECT_EQ(big.a, UINT32_MAX);
  EXPECT_EQ(big.b, 0u);
  EXPECT_EQ(big.c, UINT32_MAX);
  const GTEXT_CSS_Specificity max = gtext_css_specificity_make(UINT32_MAX, UINT32_MAX, UINT32_MAX);
  const GTEXT_CSS_Specificity one = gtext_css_specificity_make(1, 1, 1);
  const GTEXT_CSS_Specificity sum = gtext_css_specificity_add(max, one);
  EXPECT_EQ(sum.a, UINT32_MAX) << "UINT32_MAX plus one stays at UINT32_MAX, it does not wrap to 0";
  EXPECT_EQ(sum.b, UINT32_MAX);
  EXPECT_EQ(sum.c, UINT32_MAX);
  const GTEXT_CSS_Specificity just_below = gtext_css_specificity_make(UINT32_MAX - 1, 0, 0);
  EXPECT_EQ(gtext_css_specificity_add(just_below, one).a, UINT32_MAX);
  EXPECT_EQ(gtext_css_specificity_add(just_below, gtext_css_specificity_make(1, 0, 0)).a, UINT32_MAX) << "exactly at the bound";
  EXPECT_EQ(gtext_css_specificity_compare(gtext_css_specificity_make(1, 0, 0), gtext_css_specificity_make(0, 99, 99)), 1);
  EXPECT_EQ(gtext_css_specificity_compare(gtext_css_specificity_make(0, 1, 0), gtext_css_specificity_make(0, 0, 99)), 1);
  EXPECT_EQ(gtext_css_specificity_compare(gtext_css_specificity_make(0, 0, 1), gtext_css_specificity_make(0, 0, 2)), -1);
  EXPECT_EQ(gtext_css_specificity_compare(sum, max), 0);
  /* in the cascade: a saturated component beats any below it, and two saturated ones are equal, so the order of appearance decides */
  Set s;
  s.add("color", "red", K().spec(UINT32_MAX, 0, 0).order(0));
  s.add("color", "blue", K().spec(UINT32_MAX - 1, UINT32_MAX, UINT32_MAX).order(1));
  Out o(s);
  EXPECT_EQ(o.value("color"), "red");
  Set t;
  t.add("color", "red", K().spec(gtext_css_specificity_add(max, one).a, 0, 0).order(0));
  t.add("color", "blue", K().spec(UINT32_MAX, 0, 0).order(1));
  Out p(t);
  EXPECT_EQ(p.value("color"), "blue") << "equal at the bound: the later declaration wins";
}

/* ---- the matrix of the specification ---------------------------------------------------------------------------- */

TEST(CssCascadeMatrix, SpecificityTheHigherWins) {
  Set s;
  s.add("color", "red", K().spec(0, 1, 0).order(0));
  s.add("color", "blue", K().spec(0, 0, 2).order(1));
  EXPECT_EQ(Out(s).value("color"), "red");
}

TEST(CssCascadeMatrix, OrderEqualKeysTheLaterWins) {
  Set s;
  s.add("color", "red", K().order(0, 0, 0));
  s.add("color", "blue", K().order(0, 1, 0));
  EXPECT_EQ(Out(s).value("color"), "blue");
  /* two declarations equal in every key (a caller has no reason to make them): the one added last */
  Set t;
  t.add("color", "red", K().order(3, 3, 3));
  t.add("color", "blue", K().order(3, 3, 3));
  EXPECT_EQ(Out(t).value("color"), "blue");
}

TEST(CssCascadeMatrix, ImportantFlipsTheLayers) {
  Layers l;
  const GTEXT_CSS_Layer_Id a = l.declare("a"), b = l.declare("b");
  Set normal(l.frozen());
  normal.add("color", "red", K().layer(a));
  normal.add("color", "blue", K().layer(b));
  EXPECT_EQ(Out(normal).value("color"), "blue") << "normal: the layer latest in the layer order wins";
  Set imp(l.r);
  imp.add("color", "red", K().layer(a).important());
  imp.add("color", "blue", K().layer(b).important());
  EXPECT_EQ(Out(imp).value("color"), "red") << "important: the earlier layer wins";
}

TEST(CssCascadeMatrix, UnlayeredBeatsAnyLayerWhenNormal) {
  Layers l;
  const GTEXT_CSS_Layer_Id a = l.declare("a");
  Set s(l.frozen());
  s.add("color", "red", K().spec(1, 0, 0).layer(a).order(1));
  s.add("color", "blue", K().order(0));
  EXPECT_EQ(Out(s).value("color"), "blue") << "unlayered, though lower in specificity and earlier";
}

TEST(CssCascadeMatrix, TheStyleAttributeBeatsARuleOfHigherSpecificity) {
  Set s;
  s.add("color", "red", K().attached());
  s.add("color", "blue", K().spec(1, 1, 1).order(5));
  EXPECT_EQ(Out(s).value("color"), "red");
}

TEST(CssCascadeMatrix, APresentationalHintLosesToAnyAuthorRule) {
  Layers l;
  const GTEXT_CSS_Layer_Id a = l.declare("a");
  Set s(l.frozen());
  s.add("width", "10px", K().hint().order(0));
  s.add("width", "20px", K().layer(a).order(1));
  EXPECT_EQ(Out(s).value("width"), "20px") << "even a rule in a layer, with specificity zero";
  Set t(l.r);
  t.add("width", "10px", K().hint().order(9));
  t.add("width", "20px", K().order(0));
  EXPECT_EQ(Out(t).value("width"), "20px") << "even a rule that came before it";
}

TEST(CssCascadeMatrix, RevertRollsBackToTheUserValue) {
  Set s;
  s.add("color", "red", K().user());
  s.add("color", "revert", K().author());
  Out o(s);
  EXPECT_EQ(o.value("color"), "red");
  GTEXT_CSS_Cascade_Entry e;
  size_t rb = 0;
  ASSERT_TRUE(gtext_css_cascaded_at(o.c, 0, &e, &rb));
  EXPECT_EQ(e.origin, GTEXT_CSS_ORIGIN_USER);
  EXPECT_EQ(rb, 1u);
}

TEST(CssCascadeMatrix, RevertLayerRollsBackToTheEarlierLayer) {
  Layers l;
  const GTEXT_CSS_Layer_Id a = l.declare("a"), b = l.declare("b");
  Set s(l.frozen());
  s.add("width", "100px", K().layer(a));
  s.add("width", "200px", K().layer(b).order(0, 0));
  s.add("width", "revert-layer", K().layer(b).order(0, 1));
  /* Cascade 5, 7.3.5: "applying revert-layer to the height in layer two, reverts the height property to the value defined in layer one, yielding a
     height of 100px" */
  EXPECT_EQ(Out(s).value("width"), "100px");
}

TEST(CssCascadeMatrix, TooManyCandidatesIsRefusedWithTheLocation) {
  GTEXT_CSS_Candidate_Options o = gtext_css_candidate_options_default();
  o.max_candidates = 3;
  Set s(nullptr, &o);
  for (int i = 0; i < 3; i++) {
    s.add("color", kMarks[i], K().order(i));
    EXPECT_EQ(s.status, GTEXT_CSS_OK) << i;
  }
  K k;
  k.e.declaration_id = 77;
  k.e.span.offset = 10;
  k.e.span.end = 20;
  s.add("color", "blue", k);
  EXPECT_EQ(s.status, GTEXT_CSS_E_LIMIT_CANDIDATES);
  EXPECT_EQ(s.last.declaration, 77u);
  EXPECT_EQ(s.last.span.offset, 10u);
  EXPECT_NE(s.last.detail, nullptr);
  EXPECT_EQ(gtext_css_candidates_count(s.s), 3u) << "the set is as it was";
  /* a shorthand that does not fit adds none of its longhands */
  Set t(nullptr, &o);
  t.add("color", "red");
  t.add("margin", "1px");
  EXPECT_EQ(t.status, GTEXT_CSS_E_LIMIT_CANDIDATES);
  EXPECT_EQ(gtext_css_candidates_count(t.s), 1u);
  Out out(s);
  EXPECT_EQ(out.value("color"), "yellow");
}

/* ---- one test for each criterion of the sort ---------------------------------------------------------------------- */

TEST(CssCascadeSort, OriginAndImportance) {
  /* 6.1: "The precedence of the various origins is, in descending order: Transition declarations; Important user agent declarations; Important user
     declarations; Important author declarations; Animation declarations; Normal author declarations; Normal user declarations; Normal user agent
     declarations. Declarations from origins earlier in this list win over declarations from later origins." */
  const K ascending[] = {K().ua(), K().user(), K().author(), K().animation(), K().author().important(), K().user().important(), K().ua().important(),
      K().transition()};
  const char * const names[] = {"normal ua", "normal user", "normal author", "animation", "important author", "important user", "important ua", "transition"};
  /* all eight, added in reverse, each time with the strongest left removed: the strongest left wins */
  for (int top = 7; top >= 0; top--) {
    Set s;
    for (int i = top; i >= 0; i--) {
      K k = ascending[i];
      k.order(uint32_t(100 - i));  // the weaker ones appear later: order must not decide
      s.add("color", kMarks[i], k);
      ASSERT_EQ(s.status, GTEXT_CSS_OK) << names[i];
    }
    EXPECT_EQ(Out(s).value("color"), kMarks[top]) << names[top];
  }
}

TEST(CssCascadeSort, AnimationAndTransitionDeclarationsAreNotImportantOrLayered) {
  Layers l;
  const GTEXT_CSS_Layer_Id a = l.declare("a");
  Set s(l.frozen());
  s.add("color", "red", K().animation().important());
  EXPECT_EQ(s.status, GTEXT_CSS_E_INVALID);
  s.add("color", "red", K().transition().layer(a));
  EXPECT_EQ(s.status, GTEXT_CSS_E_INVALID);
  s.add("color", "red", K().animation().attached());
  EXPECT_EQ(s.status, GTEXT_CSS_E_INVALID);
  s.add("color", "red", K().animation().hint());
  EXPECT_EQ(s.status, GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_candidates_count(s.s), 0u);
}

TEST(CssCascadeSort, ContextTheOuterWinsWhenNormalAndTheInnerWhenImportant) {
  /* 6.1: "When comparing two declarations that are sourced from different encapsulation contexts, then for normal rules the declaration from the outer
     context wins, and for important rules the declaration from the inner context wins." */
  Set normal;
  normal.add("color", "red", K().ctx(0).spec(0, 0, 1));
  normal.add("color", "blue", K().ctx(1).spec(1, 0, 0));
  normal.add("color", "green", K().ctx(2).order(9));
  EXPECT_EQ(Out(normal).value("color"), "red");
  Set imp;
  imp.add("color", "red", K().ctx(0).important().spec(1, 0, 0));
  imp.add("color", "blue", K().ctx(1).important());
  imp.add("color", "green", K().ctx(2).important().order(0, 0, 0));
  EXPECT_EQ(Out(imp).value("color"), "green");
  /* the context is compared after the origin and before everything else */
  Set s;
  s.add("color", "red", K().ctx(0).user().important());
  s.add("color", "blue", K().ctx(5).important());
  EXPECT_EQ(Out(s).value("color"), "red") << "important user beats important author whatever the contexts";
}

TEST(CssCascadeSort, ElementAttachedStylesAreSeparateForNormalAndImportant) {
  /* 6.1: "Separately for normal and important declarations, declarations that are attached directly to an element (such as the contents of a style
     attribute) rather than indirectly mapped by means of a style rule selector take precedence over declarations the same importance that are mapped via
     style rule." */
  Layers l;
  const GTEXT_CSS_Layer_Id a = l.declare("a");
  Set normal(l.frozen());
  normal.add("color", "red", K().attached());
  normal.add("color", "blue", K().spec(9, 9, 9).order(5));
  normal.add("color", "green", K().layer(a).order(6));
  EXPECT_EQ(Out(normal).value("color"), "red");
  Set imp(l.r);
  imp.add("color", "red", K().attached().important());
  imp.add("color", "blue", K().layer(a).important().spec(9, 9, 9).order(5));
  EXPECT_EQ(Out(imp).value("color"), "red") << "important element-attached over an important rule in an earlier layer";
  /* "the same importance": a normal style attribute does not beat an important rule, and an important one beats a normal one */
  Set mixed;
  mixed.add("color", "red", K().attached());
  mixed.add("color", "blue", K().important());
  EXPECT_EQ(Out(mixed).value("color"), "blue");
  Set both;
  both.add("color", "red", K().attached().important());
  both.add("color", "blue", K().attached());
  EXPECT_EQ(Out(both).value("color"), "red");
  /* the style attribute has no specificity of its own and no layer */
  Set no_spec;
  no_spec.add("color", "red", K().attached().spec(5, 0, 0).order(0));
  no_spec.add("color", "blue", K().attached().order(1));
  EXPECT_EQ(Out(no_spec).value("color"), "blue");
  Set bad(l.r);
  bad.add("color", "red", K().attached().layer(a));
  EXPECT_EQ(bad.status, GTEXT_CSS_E_INVALID);
  /* in a shadow context, element-attached still comes after the context */
  Set ctx;
  ctx.add("color", "red", K().ctx(0));
  ctx.add("color", "blue", K().ctx(1).attached());
  EXPECT_EQ(Out(ctx).value("color"), "red");
}

TEST(CssCascadeSort, LayersTheLaterWinsWhenNormalAndTheEarlierWhenImportant) {
  /* 6.1: "When comparing declarations that belong to different layers, then for normal rules the declaration whose cascade layer is latest in the layer
     order wins, and for important rules the declaration whose cascade layer is earliest wins." Any declaration not assigned to an explicit layer "is
     added to an implicit final layer." */
  Layers l;
  const GTEXT_CSS_Layer_Id a = l.declare("a"), b = l.declare("b"), c = l.declare("c"), ab = l.declare("x", GTEXT_CSS_ORIGIN_AUTHOR, b);
  Set normal(l.frozen());
  normal.add("color", "red", K().layer(a).spec(9, 0, 0));
  normal.add("color", "orange", K().layer(c).order(0));
  normal.add("color", "yellow", K().layer(b).order(1));
  EXPECT_EQ(Out(normal).value("color"), "orange");
  /* a nested layer comes before its parent's own declarations: b.x < b < c */
  Set nested(l.r);
  nested.add("color", "red", K().layer(b));
  nested.add("color", "blue", K().layer(ab).order(5));
  EXPECT_EQ(Out(nested).value("color"), "red");
  Set imp(l.r);
  imp.add("color", "red", K().layer(a).important());
  imp.add("color", "orange", K().layer(c).important());
  imp.add("color", "yellow").add("color", "green", K().important().order(9));  // the first normal, the second unlayered and important
  EXPECT_EQ(Out(imp).value("color"), "red") << "the earliest layer, ahead of the unlayered declaration, which is the last";
  /* the layers of the user and user-agent origins are theirs */
  Layers u;
  const GTEXT_CSS_Layer_Id ua = u.declare("lib", GTEXT_CSS_ORIGIN_USER);
  Set user(u.frozen());
  user.add("color", "red", K().user().layer(ua));
  user.add("color", "blue", K().user());
  EXPECT_EQ(Out(user).value("color"), "blue");
  Set wrong(u.r);
  wrong.add("color", "red", K().author().layer(ua));
  EXPECT_EQ(wrong.status, GTEXT_CSS_E_INVALID) << "a layer of another origin";
  Set none;
  none.add("color", "red", K().layer(3));
  EXPECT_EQ(none.status, GTEXT_CSS_E_INVALID) << "a layer with no layer registry";
}

TEST(CssCascadeSort, SpecificityTheHighestWins) {
  /* 6.1: "The declaration with the highest specificity wins." */
  struct Row {
    GTEXT_CSS_Specificity lo, hi;
  };
  const Row rows[] = {{{0, 0, 1}, {0, 0, 2}}, {{0, 0, 9}, {0, 1, 0}}, {{0, 9, 9}, {1, 0, 0}}, {{0, 0, 0}, {0, 0, 1}}};
  for (const Row & r : rows) {
    for (int lo_first = 0; lo_first < 2; lo_first++) {
      Set s;
      K lo = K().spec(r.lo.a, r.lo.b, r.lo.c), hi = K().spec(r.hi.a, r.hi.b, r.hi.c);
      if (lo_first) {
        s.add("color", "red", lo.order(1));
        s.add("color", "blue", hi.order(0));
      }
      else {
        s.add("color", "blue", hi.order(0));
        s.add("color", "red", lo.order(1));
      }
      EXPECT_EQ(Out(s).value("color"), "blue") << r.hi.a << r.hi.b << r.hi.c;
    }
  }
}

TEST(CssCascadeSort, ScopeProximityTheFewerHopsWin) {
  /* Cascade 6, 3.1: "the declaration with the fewest generational or sibling-element hops between the scoping root and the scoped style rule subject
     wins. For this purpose, style rules without a scoping root are considered to have infinite proximity hops." */
  Set s;
  s.add("color", "red", K().scope(3).order(0));
  s.add("color", "blue", K().scope(1).order(1));
  s.add("color", "green", K().scope(2).order(2));
  EXPECT_EQ(Out(s).value("color"), "blue");
  Set none;
  none.add("color", "red", K().order(5));
  none.add("color", "blue", K().scope(1000000).order(0));
  EXPECT_EQ(Out(none).value("color"), "blue") << "any scope is nearer than none";
  Set zero;
  zero.add("color", "red", K().scope(1));
  zero.add("color", "blue", K().scope(0).order(0));
  EXPECT_EQ(Out(zero).value("color"), "blue") << "the scoping root itself";
  Set big;
  big.add("color", "red", K().scope(UINT32_MAX).order(1));
  big.add("color", "blue", K().order(0));
  EXPECT_EQ(Out(big).value("color"), "red") << "the greatest distance is still a scope";
  /* it is below specificity in the list and above order of appearance */
  Set spec;
  spec.add("color", "red", K().scope(5).spec(0, 0, 1));
  spec.add("color", "blue", K().scope(0).order(1));
  EXPECT_EQ(Out(spec).value("color"), "red");
  Set tie;
  tie.add("color", "red", K().scope(2).order(9));
  tie.add("color", "blue", K().scope(2).order(0));
  EXPECT_EQ(Out(tie).value("color"), "red") << "equal hops: the order of appearance";
}

TEST(CssCascadeSort, OrderOfAppearanceIsSheetThenRuleThenDeclaration) {
  /* 6.1: "The last declaration in document order wins." */
  struct Row {
    uint32_t a[3], b[3];
  };
  for (const Row & r : {Row{{1, 0, 0}, {0, 9, 9}}, Row{{0, 2, 0}, {0, 1, 9}}, Row{{0, 0, 2}, {0, 0, 1}}}) {
    Set s;
    s.add("color", "red", K().order(r.b[0], r.b[1], r.b[2]));
    s.add("color", "blue", K().order(r.a[0], r.a[1], r.a[2]));
    EXPECT_EQ(Out(s).value("color"), "blue");
  }
}

TEST(CssCascadeSort, PresentationalHintsAreAuthorLevelAtTheStartWithSpecificityZero) {
  /* 6.5 Precedence of non-CSS presentational hints: author level, at the start of the author style sheet, with specificity zero. Below every author rule
     and above the user and user-agent origins; an important user declaration still wins. */
  Set s;
  s.add("width", "1px", K().ua().order(0));
  s.add("width", "2px", K().user().order(0));
  s.add("width", "3px", K().hint().order(0));
  EXPECT_EQ(Out(s).value("width"), "3px");
  Set important;
  important.add("width", "2px", K().user().order(0));
  important.add("width", "3px", K().hint().order(0));
  important.add("width", "4px", K().user().important());
  EXPECT_EQ(Out(important).value("width"), "4px");
  Set zero;
  zero.add("width", "1px", K().hint().spec(9, 9, 9).order(5));
  zero.add("width", "2px", K().order(0));
  EXPECT_EQ(Out(zero).value("width"), "2px") << "its specificity is zero whatever it was given";
  Set two;
  two.add("width", "1px", K().hint().order(0));
  two.add("width", "2px", K().hint().order(1));
  EXPECT_EQ(Out(two).value("width"), "2px") << "hints are ordered among themselves";
  Set spec;
  spec.add("width", "1px", K().hint().spec(5, 0, 0).order(0));
  spec.add("width", "2px", K().hint().order(1));
  EXPECT_EQ(Out(spec).value("width"), "2px") << "and a hint has no specificity to be ordered by";
  Set bad;
  bad.add("width", "1px", K().hint().important());
  EXPECT_EQ(bad.status, GTEXT_CSS_E_INVALID);
  bad.add("width", "1px", K().hint().user());
  EXPECT_EQ(bad.status, GTEXT_CSS_E_INVALID);
  bad.add("width", "1px", K().hint().attached());
  EXPECT_EQ(bad.status, GTEXT_CSS_E_INVALID);
  /* 6.5: "this author presentational hint origin is treated as an independent origin" "between the regular user origin and the author origin", and the origin
     is compared before the context: a hint of the outer context is below an author rule of an inner one, and above a user rule of any */
  Set ctx;
  ctx.add("width", "1px", K().hint().ctx(0));
  ctx.add("width", "2px", K().ctx(1));
  EXPECT_EQ(Out(ctx).value("width"), "2px");
  Set inner;
  inner.add("width", "1px", K().hint().ctx(2));
  inner.add("width", "2px", K().user().ctx(0));
  EXPECT_EQ(Out(inner).value("width"), "1px");
  /* among hints the contexts compare as they do for any normal declaration */
  Set hints;
  hints.add("width", "1px", K().hint().ctx(1));
  hints.add("width", "2px", K().hint().ctx(0).order(0, 1));
  EXPECT_EQ(Out(hints).value("width"), "2px");
}

/* ---- rolling back ------------------------------------------------------------------------------------------------- */

TEST(CssCascadeRevert, FromTheAuthorOriginTheUserValueAndFromTheUserOriginTheUserAgentValue) {
  /* 7.3.4: "author origin: Rolls back the cascaded value to the user level, so that the specified value is calculated as if no author-origin rules were
     specified for this property on this element." "user origin: Rolls back the cascaded value to the user-agent level, so that the specified value is
     calculated as if no author-origin or user-origin rules were specified". */
  Set s;
  s.add("color", "red", K().ua());
  s.add("color", "orange", K().user().order(0));
  s.add("color", "yellow", K().author().order(0));
  s.add("color", "revert", K().author().order(1));
  EXPECT_EQ(Out(s).value("color"), "orange");
  Set t;
  t.add("color", "red", K().ua());
  t.add("color", "revert", K().user());
  t.add("color", "yellow", K().author().order(0));
  t.add("color", "revert", K().author().order(1));
  Out o(t);
  EXPECT_EQ(o.value("color"), "red") << "author revert reaches a user revert, which reaches the user agent";
  GTEXT_CSS_Cascade_Entry e;
  size_t rb = 0;
  ASSERT_TRUE(gtext_css_cascaded_at(o.c, 0, &e, &rb));
  EXPECT_EQ(rb, 2u);
}

TEST(CssCascadeRevert, WithNothingLeftThePropertyHasNoDeclaredValueAndTheUserAgentRevertIsUnset) {
  /* 7.3.4: "user-agent origin: Equivalent to unset." Nothing is left to roll back to: the property takes the inherited or initial value. */
  Set only;
  only.add("color", "revert");
  Out a(only);
  EXPECT_EQ(a.status, GTEXT_CSS_OK);
  EXPECT_EQ(a.value("color"), "-");
  EXPECT_EQ(gtext_css_cascaded_count(a.c), 0u);
  Set ua;
  ua.add("color", "revert", K().ua());
  EXPECT_EQ(Out(ua).value("color"), "-");
  Set ua_under;
  ua_under.add("color", "red", K().ua().order(0));
  ua_under.add("color", "revert", K().ua().order(1));
  EXPECT_EQ(Out(ua_under).value("color"), "-") << "a user-agent revert is unset: it does not reach the user-agent declaration before it";
  /* a revert that rolls back past the user agent's own declaration */
  Set past;
  past.add("color", "red", K().ua());
  past.add("color", "revert", K().user());
  EXPECT_EQ(Out(past).value("color"), "red");
}

TEST(CssCascadeRevert, TheAnimationAndTransitionOriginsRollBackWithTheAuthorOrigin) {
  /* 7.3.4: "For the purpose of revert, this origin includes the Animation origin." */
  Set s;
  s.add("color", "red", K().user());
  s.add("color", "blue", K().author());
  s.add("color", "revert", K().animation());
  EXPECT_EQ(Out(s).value("color"), "red");
  Set t;
  t.add("color", "red", K().user());
  t.add("color", "blue", K().author());
  t.add("color", "revert", K().transition());
  EXPECT_EQ(Out(t).value("color"), "red");
}

TEST(CssCascadeRevert, AnImportantDeclarationRollsBackByItsOrigin) {
  Set s;
  s.add("color", "red", K().user());
  s.add("color", "blue", K().author().important().order(0));
  s.add("color", "revert", K().author().important().order(1));
  EXPECT_EQ(Out(s).value("color"), "red");
  /* the important user declaration beats the author's and is what is left */
  Set t;
  t.add("color", "red", K().user().important());
  t.add("color", "revert", K().author().important());
  EXPECT_EQ(Out(t).value("color"), "red");
}

TEST(CssCascadeRevertLayer, RollsBackToTheEarlierLayerAndThenToTheOrigin) {
  /* 7.3.5: "the cascaded value is rolled back to the earlier layer, so that the specified value is calculated as if no rules were specified in the
     current cascade layer". "If there are no lower-precedence declarations in the same cascade origin as the revert-layer value, the cascaded value will
     roll back to the previous origin." */
  Layers l;
  const GTEXT_CSS_Layer_Id one = l.declare("one"), two = l.declare("two"), three = l.declare("three");
  Set s(l.frozen());
  s.add("width", "10px", K().user());
  s.add("width", "20px", K().layer(one));
  s.add("width", "30px", K().layer(two).order(0, 0));
  s.add("width", "revert-layer", K().layer(two).order(0, 1));
  s.add("width", "40px", K().layer(three).spec(1, 0, 0));
  s.add("width", "revert-layer", K().layer(three).spec(2, 0, 0));
  Out o(s);
  EXPECT_EQ(o.value("width"), "20px") << "layer three rolled back past two, which is rolled back too";
  Set none(l.r);
  none.add("width", "10px", K().user());
  none.add("width", "revert-layer", K().layer(one));
  EXPECT_EQ(Out(none).value("width"), "10px") << "no earlier layer: the previous origin";
  Set unlayered(l.r);
  unlayered.add("width", "10px", K().layer(two));
  unlayered.add("width", "revert-layer");
  EXPECT_EQ(Out(unlayered).value("width"), "10px") << "the unlayered declarations are the last layer";
  Set nothing(l.r);
  nothing.add("width", "revert-layer", K().layer(one));
  EXPECT_EQ(Out(nothing).value("width"), "-");
}

TEST(CssCascadeRevertLayer, ARevertLayerInAnImportantLayerRollsBackToTheNormalLevel) {
  /* 7.3.5: "as if no rules were specified in the current cascade layer - or between its normal and important levels in the cascade" */
  Layers l;
  const GTEXT_CSS_Layer_Id one = l.declare("one"), two = l.declare("two");
  Set s(l.frozen());
  s.add("width", "10px", K().layer(one).order(0));
  s.add("width", "20px", K().layer(two).order(1));
  s.add("width", "revert-layer", K().layer(one).important().order(2));
  s.add("width", "30px", K().layer(two).important().order(3));
  /* important: layer one is the strongest; its revert-layer skips everything down to layer one's normal level, so the normal declaration of layer one,
     and the important one of layer two, and the animation origin are all behind it: nothing is left in the layers below */
  EXPECT_EQ(Out(s).value("width"), "-");
  Set t(l.r);
  t.add("width", "5px", K().user());
  t.add("width", "10px", K().layer(one));
  t.add("width", "revert-layer", K().layer(two).important());
  EXPECT_EQ(Out(t).value("width"), "10px") << "the normal declaration of the earlier layer is below layer two's normal level";
}

TEST(CssCascadeRevertLayer, ImportantElementAttachedStylesRevertOnlyThemselves) {
  /* 7.3.5: "For revert-layer in important element-attached styles, however, it only reverts the element-attached styles and the intervening animation
     origin, and not any of the intervening author-origin important rules." */
  Layers l;
  const GTEXT_CSS_Layer_Id a = l.declare("a");
  Set s(l.frozen());
  s.add("width", "10px", K().layer(a).important());
  s.add("width", "revert-layer", K().attached().important());
  s.add("width", "20px", K().attached());
  s.add("width", "30px", K().animation());
  EXPECT_EQ(Out(s).value("width"), "10px") << "the important rule is what is left; the normal style attribute and the animation are behind it";
  Set t(l.r);
  t.add("width", "revert-layer", K().attached());
  t.add("width", "20px", K().layer(a));
  EXPECT_EQ(Out(t).value("width"), "20px") << "a normal style attribute reverts to the rules";
  Set u(l.r);
  u.add("width", "revert-layer", K().attached().important());
  u.add("width", "20px", K().attached());
  u.add("width", "30px", K().animation());
  u.add("width", "40px", K().layer(a));
  EXPECT_EQ(Out(u).value("width"), "40px") << "the attached normal and the animation are skipped on the way down";
}

TEST(CssCascadeRevertLayer, TheAnimationOriginFormsItsOwnLayer) {
  /* 7.3.5: "The animation origin is not collapsed with the author origin for this purpose as it is for revert, and thus effectively forms its own cascade
     layer." */
  Set s;
  s.add("width", "10px", K().author());
  s.add("width", "20px", K().animation().order(0));
  s.add("width", "revert-layer", K().animation().order(1));
  EXPECT_EQ(Out(s).value("width"), "10px");
}

TEST(CssCascadeRevertRule, RollsBackAsIfTheRuleWereAbsent) {
  /* 7.3.6: "the cascaded value is rolled back such that the specified value is calculated as if the current style rule had not been present at all." */
  Layers l;
  const GTEXT_CSS_Layer_Id a = l.declare("a");
  Set s(l.frozen());
  s.add("width", "10px", K().layer(a));
  s.add("width", "20px", K().order(0, 0, 0));
  s.add("width", "30px", K().order(0, 1, 0));
  s.add("width", "revert-rule", K().order(0, 1, 1));
  EXPECT_EQ(Out(s).value("width"), "20px") << "both declarations of rule 1 are gone, the earlier rule is what is left";
  Set t(l.r);
  t.add("width", "10px", K().layer(a));
  t.add("width", "revert-rule", K().order(0, 1, 1));
  EXPECT_EQ(Out(t).value("width"), "10px") << "a layer below is reached, since the rule is the one gone";
  /* "The revert-rule keyword behaves like revert-layer in the animation origin." */
  Set u;
  u.add("width", "10px", K().author());
  u.add("width", "20px", K().animation().order(0));
  u.add("width", "revert-rule", K().animation().order(1));
  EXPECT_EQ(Out(u).value("width"), "10px");
  /* a rolling keyword in a rule below the rolled-back rule rolls back too */
  Set v;
  v.add("width", "10px", K().user());
  v.add("width", "revert-rule", K().order(0, 0, 0));
  v.add("width", "revert-rule", K().order(0, 1, 0));
  EXPECT_EQ(Out(v).value("width"), "10px");
}

TEST(CssCascadeRevert, AChainOfRollbacksIsFollowedThroughAllOfThem) {
  Layers l;
  std::vector<GTEXT_CSS_Layer_Id> ids;
  for (int i = 0; i < 40; i++) {
    ids.push_back(l.declare("l" + std::to_string(i)));
  }
  Set s(l.frozen());
  s.add("width", "7px", K().layer(ids[0]));
  for (int i = 1; i < 40; i++) {
    s.add("width", "revert-layer", K().layer(ids[i]));
  }
  Out o(s);
  EXPECT_EQ(o.value("width"), "7px");
  GTEXT_CSS_Cascade_Entry e;
  size_t rb = 0;
  ASSERT_TRUE(gtext_css_cascaded_at(o.c, 0, &e, &rb));
  EXPECT_EQ(rb, 39u);
}

/* ---- all, shorthands, pending values ------------------------------------------------------------------------------ */

TEST(CssCascadeAll, ApplyToEveryPropertyButDirectionUnicodeBidiAndTheCustomOnes) {
  /* 7.4: the `all` shorthand "resets all properties, except direction and unicode-bidi, and custom properties" */
  Set s;
  s.add("color", "red", K().order(0));
  s.add("direction", "rtl", K().order(1));
  s.add("unicode-bidi", "embed", K().order(2));
  s.add("--x", "1", K().order(3));
  s.add("all", "initial", K().order(4));
  Out o(s);
  EXPECT_EQ(o.value("color"), "initial") << "the later `all` beats the earlier declaration";
  EXPECT_EQ(o.value("direction"), "rtl");
  EXPECT_EQ(o.value("unicode-bidi"), "embed");
  EXPECT_EQ(o.custom("--x"), "1");
  EXPECT_EQ(o.value("margin-top"), "initial");
  EXPECT_EQ(o.value("width"), "initial");
  EXPECT_EQ(o.value("margin"), "-") << "a shorthand is not a property with a value";
  /* the cascade says all, once: one candidate */
  EXPECT_EQ(gtext_css_candidates_count(s.s), 5u);
  GTEXT_CSS_Cascade_Entry e;
  ASSERT_TRUE(gtext_css_cascaded_find(o.c, find(R(), "color"), nullptr, 0, &e));
  EXPECT_TRUE(e.from_all);
  EXPECT_EQ(e.property, find(R(), "color"));
}

TEST(CssCascadeAll, ALaterDeclarationBeatsAllAndAnEarlierOneDoesNot) {
  Set s;
  s.add("all", "inherit", K().order(1));
  s.add("color", "red", K().order(0));
  s.add("width", "10px", K().order(2));
  Out o(s);
  EXPECT_EQ(o.value("color"), "inherit");
  EXPECT_EQ(o.value("width"), "10px");
  /* important and origin apply to `all` as to any declaration */
  Set t;
  t.add("all", "unset", K().important().order(0));
  t.add("color", "red", K().order(1));
  t.add("width", "10px", K().important().order(2));
  Out p(t);
  EXPECT_EQ(p.value("color"), "unset");
  EXPECT_EQ(p.value("width"), "10px");
}

TEST(CssCascadeAll, RevertRollsBackEveryPropertyAndLayersApply) {
  Layers l;
  const GTEXT_CSS_Layer_Id a = l.declare("a"), b = l.declare("b");
  Set s(l.frozen());
  s.add("color", "red", K().user());
  s.add("width", "10px", K().layer(a));
  s.add("height", "20px", K().layer(b));
  s.add("all", "revert-layer", K().layer(b).order(1));
  Out o(s);
  EXPECT_EQ(o.value("color"), "red") << "layer b is gone for every property, and `all: revert-layer` with it";
  EXPECT_EQ(o.value("width"), "10px");
  EXPECT_EQ(o.value("height"), "-");
  Set t;
  t.add("color", "red", K().user());
  t.add("all", "revert", K().author());
  t.add("width", "10px", K().user());
  Out p(t);
  EXPECT_EQ(p.value("color"), "red");
  EXPECT_EQ(p.value("width"), "10px");
  EXPECT_EQ(p.value("margin-top"), "-");
}

TEST(CssCascadeAll, ManyAllDeclarationsAreMergedNotRepeated) {
  Set s;
  for (int i = 0; i < 50; i++) {
    s.add("all", i % 2 ? "initial" : "inherit", K().order(i));
  }
  s.add("color", "red", K().order(25, 0, 0));
  Out o(s);
  EXPECT_EQ(o.value("color"), "initial") << "49 is odd: initial, later than the color";
  EXPECT_EQ(o.value("width"), "initial");
}

TEST(CssCascadeShorthand, EachLonghandTakesTheDeclarationsKeys) {
  Set s;
  s.add("margin", "1px 2px 3px 4px", K().spec(0, 1, 0).order(0));
  s.add("margin-left", "9px", K().spec(0, 0, 1).order(1));
  Out o(s);
  EXPECT_EQ(o.value("margin-top"), "1px");
  EXPECT_EQ(o.value("margin-right"), "2px");
  EXPECT_EQ(o.value("margin-bottom"), "3px");
  EXPECT_EQ(o.value("margin-left"), "4px") << "the shorthand's specificity is higher than the later longhand's";
  GTEXT_CSS_Cascade_Entry e;
  ASSERT_TRUE(gtext_css_cascaded_find(o.c, find(R(), "margin-top"), nullptr, 0, &e));
  EXPECT_EQ(e.shorthand, find(R(), "margin"));
  EXPECT_FALSE(e.from_all);
  /* the longhands of one declaration share its group */
  GTEXT_CSS_Cascade_Entry f;
  ASSERT_TRUE(gtext_css_cascaded_find(o.c, find(R(), "margin-bottom"), nullptr, 0, &f));
  EXPECT_EQ(e.group, f.group);
}

TEST(CssCascadeShorthand, ARollingKeywordOnAShorthandRollsBackEachLonghand) {
  /* 7.3.4: "Thus, reverting a shorthand property reverts all its longhands" */
  Set s;
  s.add("margin", "1px", K().user());
  s.add("margin-top", "5px", K().author().order(0));
  s.add("margin", "revert", K().author().order(1));
  Out o(s);
  EXPECT_EQ(o.value("margin-top"), "1px");
  EXPECT_EQ(o.value("margin-left"), "1px");
}

TEST(CssCascadeShorthand, APendingShorthandStaysPendingLonghandsAndWinsAsOne) {
  Set s;
  s.add("margin", "var(--m)", K().order(1));
  s.add("margin-top", "3px", K().order(0));
  Out o(s);
  EXPECT_EQ(o.status, GTEXT_CSS_OK);
  EXPECT_EQ(o.value("margin-top"), "pending:var(--m)");
  EXPECT_EQ(o.value("margin-left"), "pending:var(--m)");
  GTEXT_CSS_Cascade_Entry e;
  ASSERT_TRUE(gtext_css_cascaded_find(o.c, find(R(), "margin-left"), nullptr, 0, &e));
  EXPECT_EQ(e.outcome, GTEXT_CSS_PROP_RESULT_PENDING);
  EXPECT_EQ(e.shorthand, find(R(), "margin"));
}

/* ---- conflicts ----------------------------------------------------------------------------------------------------- */

TEST(CssCascadeConflicts, AnInvalidDeclarationNeverEntersAndALowerValidOneWins) {
  /* "an invalid declaration (a failed parse) never enters the cascade, so a lower-priority valid declaration wins, as the draft says" */
  Set s;
  s.add("color", "red", K().order(0));
  GTEXT_CSS_Cascade_Entry k = K().order(1).e;
  k.declaration_id = 11;
  k.span.offset = 5;
  k.span.end = 20;
  GTEXT_CSS_Property_Outcome outcome = GTEXT_CSS_PROP_RESULT_VALID;
  GTEXT_CSS_Cascade_Result res;
  EXPECT_EQ(gtext_css_candidates_add_text(s.s, &k, "color", 5, "not-a-color(", 12, nullptr, &outcome, &res), GTEXT_CSS_OK);
  EXPECT_EQ(outcome, GTEXT_CSS_PROP_RESULT_INVALID);
  EXPECT_EQ(gtext_css_candidates_count(s.s), 1u);
  EXPECT_EQ(Out(s).value("color"), "red");
  ASSERT_EQ(gtext_css_candidates_diagnostic_total(s.s), 1u);
  GTEXT_CSS_Cascade_Diagnostic d;
  ASSERT_TRUE(gtext_css_candidates_diagnostic(s.s, 0, &d));
  EXPECT_EQ(d.kind, GTEXT_CSS_DIAG_INVALID_VALUE);
  EXPECT_EQ(std::string(d.name.data, d.name.length), "color");
  EXPECT_EQ(d.declaration, 11u);
  EXPECT_EQ(d.span.offset, 5u);
}

TEST(CssCascadeConflicts, AnUnknownPropertyIsDroppedWithADiagnosticNamingIt) {
  Set s;
  s.add("colour", "red", K().order(0));
  s.add("-vendor-thing", "1", K().order(1));
  s.add("color", "blue", K().order(2));
  EXPECT_EQ(gtext_css_candidates_count(s.s), 1u);
  ASSERT_EQ(gtext_css_candidates_diagnostic_total(s.s), 2u);
  GTEXT_CSS_Cascade_Diagnostic d;
  ASSERT_TRUE(gtext_css_candidates_diagnostic(s.s, 0, &d));
  EXPECT_EQ(d.kind, GTEXT_CSS_DIAG_UNKNOWN_PROPERTY);
  EXPECT_EQ(std::string(d.name.data, d.name.length), "colour");
  ASSERT_TRUE(gtext_css_candidates_diagnostic(s.s, 1, &d));
  EXPECT_EQ(std::string(d.name.data, d.name.length), "-vendor-thing");
  EXPECT_FALSE(gtext_css_candidates_diagnostic(s.s, 2, &d));
  EXPECT_EQ(Out(s).value("color"), "blue");
  /* the diagnostics are bounded, and counted past the bound */
  GTEXT_CSS_Candidate_Options o = gtext_css_candidate_options_default();
  o.max_diagnostics = 2;
  Set t(nullptr, &o);
  for (int i = 0; i < 5; i++) {
    t.add("nope", "1");
  }
  EXPECT_EQ(gtext_css_candidates_diagnostic_total(t.s), 5u);
  EXPECT_TRUE(gtext_css_candidates_diagnostic(t.s, 1, &d));
  EXPECT_FALSE(gtext_css_candidates_diagnostic(t.s, 2, &d));
}

TEST(CssCascadeConflicts, ADeclarationOfATreeCarriesItsIdSpanAndImportance) {
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_declaration_list, "color: red !important; bogus: 1; width: 10px; height: none-such");
  ASSERT_EQ(p.status, GTEXT_CSS_OK);
  ASSERT_EQ(p.tree->declaration_count, 4u);
  Set s;
  K keys;
  keys.e.rule_id = 99;
  for (size_t i = 0; i < p.tree->declaration_count; i++) {
    GTEXT_CSS_Property_Outcome outcome;
    keys.order(0, 0, uint32_t(i));
    EXPECT_EQ(gtext_css_candidates_add_declaration(s.s, &keys.e, &p.tree->declarations[i], nullptr, &outcome, nullptr), GTEXT_CSS_OK) << i;
  }
  EXPECT_EQ(gtext_css_candidates_count(s.s), 2u);
  EXPECT_EQ(gtext_css_candidates_diagnostic_total(s.s), 2u);
  Out o(s);
  GTEXT_CSS_Cascade_Entry e;
  ASSERT_TRUE(gtext_css_cascaded_find(o.c, find(R(), "color"), nullptr, 0, &e));
  EXPECT_TRUE(e.important) << "from the declaration, whatever the keys said";
  EXPECT_EQ(e.declaration_id, p.tree->declarations[0].id);
  EXPECT_EQ(e.span.offset, p.tree->declarations[0].span.offset);
  EXPECT_EQ(e.span.end, p.tree->declarations[0].span.end);
  EXPECT_EQ(e.rule_id, 99u);
  ASSERT_TRUE(gtext_css_cascaded_find(o.c, find(R(), "width"), nullptr, 0, &e));
  EXPECT_FALSE(e.important);
  EXPECT_EQ(e.declaration_id, p.tree->declarations[2].id);
}

/* ---- logical groups ------------------------------------------------------------------------------------------------- */

TEST(CssCascadeLogical, MembersThatShareASlotAreCascadedAsOne) {
  /* Logical Properties 1, 4: "paired properties share a computed value. This shared value is determined by cascading the declarations of both
     properties together as one." In horizontal-tb ltr, margin-inline-start is the left. */
  Set s;
  s.add("margin-inline-start", "1px", K().order(0));
  s.add("margin-left", "2px", K().order(1));
  Out o(s);
  EXPECT_EQ(o.value("margin-left"), "2px");
  EXPECT_EQ(o.value("margin-inline-start"), "2px") << "the member that is asked for finds its slot's winner";
  EXPECT_EQ(gtext_css_cascaded_count(o.c), 1u);
  Set t;
  t.add("margin-left", "2px", K().order(0));
  t.add("margin-inline-start", "1px", K().order(1));
  Out p(t);
  EXPECT_EQ(p.value("margin-left"), "1px");
  GTEXT_CSS_Cascade_Entry e;
  ASSERT_TRUE(gtext_css_cascaded_find(p.c, find(R(), "margin-left"), nullptr, 0, &e));
  EXPECT_EQ(e.property, find(R(), "margin-inline-start")) << "the winner is the declaration of the flow-relative property";
  /* the other slots are the other members' */
  Set u;
  u.add("margin-inline-start", "1px");
  u.add("margin-right", "3px");
  Out q(u);
  EXPECT_EQ(gtext_css_cascaded_count(q.c), 2u);
}

TEST(CssCascadeLogical, TheFlowDecidesWhichSideAMemberIs) {
  struct Row {
    const char * flow;       // declarations of the flow
    const char * logical;    // the flow-relative property
    const char * physical;   // the physical one it should share a slot with
  };
  const Row rows[] = {
    {"", "margin-inline-start", "margin-left"},
    {"direction: rtl", "margin-inline-start", "margin-right"},
    {"direction: rtl", "margin-inline-end", "margin-left"},
    {"writing-mode: vertical-rl", "margin-block-start", "margin-right"},
    {"writing-mode: vertical-rl", "margin-inline-start", "margin-top"},
    {"writing-mode: vertical-lr", "margin-block-start", "margin-left"},
    {"writing-mode: sideways-lr", "margin-inline-start", "margin-bottom"},
    {"writing-mode: vertical-rl; direction: rtl", "margin-inline-start", "margin-bottom"},
    {"writing-mode: vertical-rl; direction: rtl; text-orientation: upright", "margin-inline-start", "margin-top"},
    {"", "inline-size", "width"},
    {"writing-mode: vertical-lr", "inline-size", "height"},
    {"writing-mode: vertical-lr", "block-size", "width"},
    {"", "border-start-start-radius", "border-top-left-radius"},
    {"direction: rtl", "border-start-start-radius", "border-top-right-radius"},
    {"", "overflow-block", "overflow-y"},
    {"writing-mode: vertical-rl", "overflow-block", "overflow-x"},
    {"", "inset-inline-start", "left"},
  };
  for (const Row & r : rows) {
    SCOPED_TRACE(std::string(r.flow) + " / " + r.logical + " / " + r.physical);
    for (int logical_last = 0; logical_last < 2; logical_last++) {
      Set s;
      std::string flow = r.flow;
      size_t pos = 0;
      while (pos < flow.size()) {
        size_t semi = flow.find(';', pos);
        if (semi == std::string::npos) {
          semi = flow.size();
        }
        std::string d = flow.substr(pos, semi - pos);
        pos = semi + 1;
        const size_t colon = d.find(':');
        std::string name = d.substr(0, colon), value = d.substr(colon + 1);
        while (!value.empty() && value[0] == ' ') value.erase(0, 1);
        while (!name.empty() && name[0] == ' ') name.erase(0, 1);
        s.add(name, value, K().order(0, 0, 0));
      }
      const bool length_valued = std::string(r.logical).find("radius") == std::string::npos && std::string(r.logical).find("overflow") == std::string::npos;
      const std::string v1 = length_valued ? "1px" : (std::string(r.logical).find("overflow") != std::string::npos ? "hidden" : "1px");
      const std::string v2 = length_valued ? "2px" : (std::string(r.logical).find("overflow") != std::string::npos ? "scroll" : "2px");
      if (logical_last) {
        s.add(r.physical, v1, K().order(1));
        s.add(r.logical, v2, K().order(2));
      }
      else {
        s.add(r.logical, v1, K().order(1));
        s.add(r.physical, v2, K().order(2));
      }
      ASSERT_EQ(s.status, GTEXT_CSS_OK);
      Out o(s);
      const std::string expect = logical_last ? v2 : v2;
      EXPECT_EQ(o.value(r.physical), expect);
      EXPECT_EQ(o.value(r.logical), expect);
      GTEXT_CSS_Cascade_Entry e;
      ASSERT_TRUE(gtext_css_cascaded_find(o.c, find(R(), r.physical), nullptr, 0, &e));
      EXPECT_EQ(e.property, find(R(), logical_last ? r.logical : r.physical));
    }
  }
}

TEST(CssCascadeLogical, TheFlowIsCascadedFirstWithInheritFromTheParentAndAGivenFlowIsUsed) {
  Set s;
  s.add("margin-inline-start", "1px", K().order(0));
  s.add("margin-left", "2px", K().order(1));
  GTEXT_CSS_Flow rtl{};
  rtl.direction = GTEXT_CSS_DIRECTION_RTL;
  GTEXT_CSS_Cascade_Options o{};
  o.parent_flow = &rtl;
  Out inherited(s, &o);
  EXPECT_EQ(inherited.value("margin-left"), "2px") << "direction is inherited from the parent: inline-start is the right";
  EXPECT_EQ(inherited.value("margin-right"), "1px");
  EXPECT_EQ(gtext_css_cascaded_flow(inherited.c).direction, GTEXT_CSS_DIRECTION_RTL);
  /* the element declares its own, `initial` and `inherit` and `unset` */
  for (const char * decl : {"initial", "inherit", "unset"}) {
    Set t;
    t.add("direction", decl);
    Out out(t, &o);
    EXPECT_EQ(gtext_css_cascaded_flow(out.c).direction, std::string(decl) == "initial" ? GTEXT_CSS_DIRECTION_LTR : GTEXT_CSS_DIRECTION_RTL) << decl;
  }
  Set u;
  u.add("writing-mode", "vertical-rl", K().order(0));
  u.add("all", "revert", K().order(1, 0, 0).user());
  Out reverted(u);
  EXPECT_EQ(gtext_css_cascaded_flow(reverted.c).writing_mode, GTEXT_CSS_WRITING_MODE_VERTICAL_RL) << "an `all` of a lower origin does not undo it";
  Set w;
  w.add("writing-mode", "vertical-rl", K().order(0));
  w.add("all", "initial", K().order(1));
  EXPECT_EQ(gtext_css_cascaded_flow(Out(w).c).writing_mode, GTEXT_CSS_WRITING_MODE_HORIZONTAL_TB) << "`all` resets writing-mode and not direction";
  /* a flow given outright is not cascaded */
  Set g;
  g.add("direction", "rtl");
  g.add("margin-inline-start", "1px");
  GTEXT_CSS_Flow given{};
  GTEXT_CSS_Cascade_Options go{};
  go.flow = &given;
  Out out(g, &go);
  EXPECT_EQ(out.value("margin-left"), "1px");
  EXPECT_EQ(gtext_css_cascaded_flow(out.c).direction, GTEXT_CSS_DIRECTION_LTR);
  /* a pending flow property is `unset` */
  Set p;
  p.add("direction", "var(--d)");
  Out pending(p, &o);
  EXPECT_EQ(gtext_css_cascaded_flow(pending.c).direction, GTEXT_CSS_DIRECTION_RTL);
}

TEST(CssCascadeLogical, ARollbackOfOneMemberRollsBackTheOther) {
  /* 7.3.4: "reverting one of the paired properties in a logical property group also reverts the other one" */
  Set s;
  s.add("margin-left", "1px", K().user());
  s.add("margin-left", "2px", K().author().order(0));
  s.add("margin-inline-start", "revert", K().author().order(1));
  Out o(s);
  EXPECT_EQ(o.value("margin-left"), "1px");
  /* 7.3.5: "since height and block-size are paired properties in a logical property group, the following layer two declaration would also revert to the
     height defined in layer one" */
  Layers l;
  const GTEXT_CSS_Layer_Id one = l.declare("one"), two = l.declare("two");
  Set t(l.frozen());
  t.add("height", "100px", K().layer(one));
  t.add("height", "200px", K().layer(two).order(0));
  t.add("block-size", "revert-layer", K().layer(two).order(1));
  EXPECT_EQ(Out(t).value("height"), "100px");
}

TEST(CssCascadeLogical, AnAllDeclarationAndAMemberShareTheSlot) {
  Set s;
  s.add("all", "initial", K().order(1));
  s.add("margin-inline-start", "5px", K().order(0));
  Out o(s);
  EXPECT_EQ(o.value("margin-left"), "initial");
  Set t;
  t.add("all", "initial", K().order(0));
  t.add("margin-inline-start", "5px", K().order(1));
  Out p(t);
  EXPECT_EQ(p.value("margin-left"), "5px");
  EXPECT_EQ(p.value("margin-right"), "initial");
  GTEXT_CSS_Cascade_Entry e;
  ASSERT_TRUE(gtext_css_cascaded_find(p.c, find(R(), "margin-right"), nullptr, 0, &e));
  EXPECT_TRUE(e.from_all);
  EXPECT_EQ(e.property, find(R(), "margin-right"));
}

/* ---- custom properties ------------------------------------------------------------------------------------------------ */

TEST(CssCascadeCustom, AreCascadedByTheirCaseSensitiveNameAndOrderedByName) {
  Set s;
  s.add("--b", "1", K().order(0));
  s.add("--a", "2", K().order(0, 1));
  s.add("--B", "3", K().order(0, 2));
  s.add("--a", "4", K().order(0, 3));
  s.add("--a", "5", K().order(0, 4).user());
  Out o(s);
  EXPECT_EQ(o.custom("--a"), "4");
  EXPECT_EQ(o.custom("--b"), "1");
  EXPECT_EQ(o.custom("--B"), "3");
  EXPECT_EQ(o.custom("--c"), "-");
  ASSERT_EQ(gtext_css_cascaded_count(o.c), 3u);
  std::vector<std::string> names;
  for (size_t i = 0; i < 3; i++) {
    GTEXT_CSS_Cascade_Entry e;
    ASSERT_TRUE(gtext_css_cascaded_at(o.c, i, &e, nullptr));
    names.emplace_back(e.custom_name.data, e.custom_name.length);
  }
  EXPECT_EQ(names, (std::vector<std::string>{"--B", "--a", "--b"})) << "by bytes";
  /* revert works on a custom property, `all` does not touch it */
  Set t;
  t.add("--x", "1", K().user());
  t.add("--x", "revert", K().author());
  t.add("--y", "2", K().order(0));
  t.add("all", "initial", K().order(1));
  Out p(t);
  EXPECT_EQ(p.custom("--x"), "1");
  EXPECT_EQ(p.custom("--y"), "2");
}

/* ---- the output ---------------------------------------------------------------------------------------------------------- */

TEST(CssCascadeOutput, TheWinnerCarriesItsProvenance) {
  Layers l;
  l.declare("base");
  const GTEXT_CSS_Layer_Id theme = l.declare("theme");
  Set s(l.frozen());
  GTEXT_CSS_Cascade_Entry k = K().layer(theme).ctx(2).spec(0, 2, 3).scope(4).order(5, 6, 7).id(1234).e;
  k.declaration_id = 4321;
  k.span.offset = 100;
  k.span.end = 120;
  k.span.line = 3;
  k.span.column = 9;
  GTEXT_CSS_Property_Outcome outcome;
  ASSERT_EQ(gtext_css_candidates_add_text(s.s, &k, "color", 5, "red", 3, nullptr, &outcome, nullptr), GTEXT_CSS_OK);
  Out o(s);
  GTEXT_CSS_Cascade_Entry e;
  size_t rb = 99;
  ASSERT_TRUE(gtext_css_cascaded_at(o.c, 0, &e, &rb));
  EXPECT_EQ(rb, 0u);
  EXPECT_EQ(e.property, find(R(), "color"));
  EXPECT_EQ(e.origin, GTEXT_CSS_ORIGIN_AUTHOR);
  EXPECT_FALSE(e.important);
  EXPECT_EQ(e.layer, theme);
  EXPECT_EQ(e.context, 2u);
  EXPECT_TRUE(e.scoped);
  EXPECT_EQ(e.scope_distance, 4u);
  EXPECT_EQ(e.specificity.b, 2u);
  EXPECT_EQ(e.specificity.c, 3u);
  EXPECT_EQ(e.sheet, 5u);
  EXPECT_EQ(e.rule, 6u);
  EXPECT_EQ(e.declaration, 7u);
  EXPECT_EQ(e.rule_id, 1234u);
  EXPECT_EQ(e.declaration_id, 4321u);
  EXPECT_EQ(e.span.offset, 100u);
  EXPECT_EQ(e.span.line, 3u);
  EXPECT_EQ(e.span.column, 9u);
  EXPECT_EQ(e.index, 0u);
  EXPECT_EQ(Out::text(e), "red");
  std::string d = o.dump();
  EXPECT_NE(d.find("color: red [author layer=theme ctx=2 spec=0,2,3 scope=4 order=5.6.7 rb=0]"), std::string::npos) << d;
}

TEST(CssCascadeOutput, TheLosersAreKeptInOrderWithTheReasonWhenAsked) {
  Layers l;
  const GTEXT_CSS_Layer_Id a = l.declare("a"), b = l.declare("b");
  Set s(l.frozen());
  s.add("width", "10px", K().layer(a).id(1));
  s.add("width", "20px", K().layer(b).order(0, 0).id(2));
  s.add("width", "revert-layer", K().layer(b).order(0, 1).id(3));
  s.add("width", "30px", K().ua().id(4));
  GTEXT_CSS_Cascade_Options opt{};
  Out without(s, &opt);
  EXPECT_EQ(gtext_css_cascaded_loser_count(without.c, 0), 0u) << "not kept unless asked";
  opt.keep_losers = true;
  Out o(s, &opt);
  EXPECT_EQ(o.value("width"), "10px");
  ASSERT_EQ(gtext_css_cascaded_loser_count(o.c, 0), 3u);
  struct Want {
    uint64_t rule;
    GTEXT_CSS_Loss_Reason reason;
  };
  const Want want[] = {{3, GTEXT_CSS_LOSS_ROLLED_BACK}, {2, GTEXT_CSS_LOSS_ROLLED_BACK}, {4, GTEXT_CSS_LOSS_OUTRANKED}};
  for (size_t i = 0; i < 3; i++) {
    GTEXT_CSS_Cascade_Entry e;
    GTEXT_CSS_Loss_Reason why;
    ASSERT_TRUE(gtext_css_cascaded_loser(o.c, 0, i, &e, &why));
    EXPECT_EQ(e.rule_id, want[i].rule) << i;
    EXPECT_EQ(why, want[i].reason) << i;
  }
  GTEXT_CSS_Cascade_Entry e;
  EXPECT_FALSE(gtext_css_cascaded_loser(o.c, 0, 3, &e, nullptr));
  /* a winner on top has only outranked losers; `all` is a loser of the properties it applies to */
  Set t;
  t.add("color", "red", K().order(1).id(1));
  t.add("color", "blue", K().order(0, 0, 1).id(2));
  t.add("all", "initial", K().order(0, 0, 0).id(3));
  Out p(t, &opt);
  const size_t color = index_of(p, "color");
  ASSERT_LT(color, gtext_css_cascaded_count(p.c));
  ASSERT_EQ(gtext_css_cascaded_loser_count(p.c, color), 2u);
  GTEXT_CSS_Loss_Reason why;
  ASSERT_TRUE(gtext_css_cascaded_loser(p.c, color, 0, &e, &why));
  EXPECT_EQ(e.rule_id, 2u);
  EXPECT_EQ(why, GTEXT_CSS_LOSS_OUTRANKED);
  ASSERT_TRUE(gtext_css_cascaded_loser(p.c, color, 1, &e, &why));
  EXPECT_TRUE(e.from_all);
  EXPECT_EQ(e.property, find(R(), "color"));
  EXPECT_EQ(why, GTEXT_CSS_LOSS_OUTRANKED);
  /* the winner of a property `all` alone sets has `all` as the winner and no loser */
  const size_t width = index_of(p, "width");
  EXPECT_EQ(gtext_css_cascaded_loser_count(p.c, width), 0u);
}

TEST(CssCascadeOutput, TheCandidateSetIsSealedByTheFirstCascadeAndMayBeCascadedAgain) {
  Set s;
  s.add("margin-inline-start", "1px");
  EXPECT_FALSE(gtext_css_candidates_sealed(s.s));
  GTEXT_CSS_Flow rtl{};
  rtl.direction = GTEXT_CSS_DIRECTION_RTL;
  GTEXT_CSS_Cascade_Options o{};
  Out ltr(s);
  EXPECT_TRUE(gtext_css_candidates_sealed(s.s));
  o.flow = &rtl;
  Out second(s, &o);
  EXPECT_EQ(ltr.value("margin-left"), "1px");
  EXPECT_EQ(second.value("margin-right"), "1px") << "the same candidates in another flow";
  s.add("color", "red");
  EXPECT_EQ(s.status, GTEXT_CSS_E_INVALID);
  EXPECT_NE(s.last.detail, nullptr);
  /* a result outlives the handle that made it */
  GTEXT_CSS_Candidates * set = nullptr;
  ASSERT_EQ(gtext_css_candidates_new(R(), nullptr, nullptr, &set), GTEXT_CSS_OK);
  K k;
  GTEXT_CSS_Property_Outcome outcome;
  ASSERT_EQ(gtext_css_candidates_add_text(set, &k.e, "color", 5, "red", 3, nullptr, &outcome, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Cascaded * c = nullptr;
  ASSERT_EQ(gtext_css_cascade(set, nullptr, &c, nullptr), GTEXT_CSS_OK);
  gtext_css_candidates_free(set);
  GTEXT_CSS_Cascade_Entry e;
  ASSERT_TRUE(gtext_css_cascaded_find(c, find(R(), "color"), nullptr, 0, &e));
  EXPECT_EQ(Out::text(e), "red");
  gtext_css_cascaded_free(c);
}

TEST(CssCascadeOutput, ArgumentsAreChecked) {
  GTEXT_CSS_Cascaded * c = nullptr;
  EXPECT_EQ(gtext_css_cascade(nullptr, nullptr, &c, nullptr), GTEXT_CSS_E_INVALID);
  Set s;
  EXPECT_EQ(gtext_css_cascade(s.s, nullptr, nullptr, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_candidates_new(nullptr, nullptr, nullptr, &s.s), GTEXT_CSS_E_INVALID);
  GTEXT_CSS_Cascade_Entry k{};
  EXPECT_EQ(gtext_css_candidates_add(s.s, &k, nullptr, nullptr), GTEXT_CSS_E_INVALID);
  GTEXT_CSS_Property_Value pv{};
  EXPECT_EQ(gtext_css_candidates_add(s.s, &k, &pv, nullptr), GTEXT_CSS_E_INVALID) << "an unknown outcome";
  EXPECT_EQ(gtext_css_candidates_add_custom(s.s, &k, "x", 1, &pv, nullptr), GTEXT_CSS_E_INVALID);
  K bad;
  bad.e.origin = GTEXT_CSS_ORIGIN_COUNT;
  s.add("color", "red", bad);
  EXPECT_EQ(s.status, GTEXT_CSS_E_INVALID);
  Out o(s);
  EXPECT_EQ(o.status, GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_cascaded_count(o.c), 0u);
  GTEXT_CSS_Cascade_Entry e;
  EXPECT_FALSE(gtext_css_cascaded_at(o.c, 0, &e, nullptr));
  EXPECT_FALSE(gtext_css_cascaded_at(nullptr, 0, &e, nullptr));
  EXPECT_FALSE(gtext_css_cascaded_find(o.c, 0, nullptr, 0, &e));
  EXPECT_EQ(gtext_css_cascaded_count(nullptr), 0u);
  EXPECT_EQ(gtext_css_candidates_count(nullptr), 0u);
  gtext_css_cascaded_free(nullptr);
  gtext_css_candidates_free(nullptr);
}

/* ---- the hand-off ---------------------------------------------------------------------------------------------------------- */

TEST(CssCascadeBlock, TheWinnersComputeAsTheDeclarationsOfTheElement) {
  cmp::Context ctx(R());
  ASSERT_EQ(ctx.status, GTEXT_CSS_OK);
  Layers l;
  const GTEXT_CSS_Layer_Id a = l.declare("a");
  Set s(l.frozen());
  s.add("direction", "rtl", K().order(0));
  s.add("margin-inline-start", "10px", K().layer(a).order(1));
  s.add("margin-left", "20px", K().order(2));
  s.add("margin-right", "5px", K().layer(a).order(3));
  s.add("color", "red", K().user());
  s.add("color", "revert", K().author().order(4));
  s.add("width", "50px", K().order(5));
  s.add("width", "99px", K().ua().important());  // the user-agent's important wins over the author's
  s.add("--x", "hello", K().order(6));
  s.add("font-size", "20px", K().attached());
  s.add("height", "2em", K().order(7));
  Out o(s);
  GTEXT_CSS_Declaration_Block * block = nullptr;
  ASSERT_EQ(gtext_css_cascaded_to_block(o.c, nullptr, &block), GTEXT_CSS_OK);
  GTEXT_CSS_Computed_Style * style = nullptr;
  GTEXT_CSS_Compute_Result cr;
  ASSERT_EQ(gtext_css_compute_style(ctx.c, block, nullptr, nullptr, &style, &cr), GTEXT_CSS_OK) << cr.name << ": " << (cr.detail ? cr.detail : "");
  cmp::Style st;
  st.s = style;
  EXPECT_EQ(st.value("direction"), "rtl");
  EXPECT_EQ(st.value("margin-right"), "5px") << "margin-inline-start is the right in rtl, but the unlayered margin-left beats it on the left";
  EXPECT_EQ(st.value("margin-left"), "20px");
  EXPECT_EQ(st.value("color"), "red") << "the author's revert reaches the user's declaration";
  EXPECT_EQ(st.value("width"), "99px");
  EXPECT_EQ(st.value("height"), "40px");
  st.s = nullptr;
  gtext_css_computed_free(style);
  gtext_css_block_free(block);
}

TEST(CssCascadeBlock, AllExpandsToThePropertiesItWon) {
  cmp::Context ctx(R());
  Set s;
  s.add("all", "initial", K().order(1));
  s.add("width", "50px", K().order(0));
  s.add("height", "30px", K().order(2));
  s.add("direction", "rtl", K().order(0));
  Out o(s);
  GTEXT_CSS_Declaration_Block * block = nullptr;
  ASSERT_EQ(gtext_css_cascaded_to_block(o.c, nullptr, &block), GTEXT_CSS_OK);
  EXPECT_GT(gtext_css_block_count(block), 300u);
  bool has_all = false;
  for (size_t i = 0; i < gtext_css_block_count(block); i++) {
    GTEXT_CSS_Declaration_Item it;
    ASSERT_TRUE(gtext_css_block_item(block, i, &it));
    GTEXT_CSS_Property_Info info;
    ASSERT_TRUE(gtext_css_registry_property_info(R(), it.property, &info));
    if (std::string(info.name) == "all") {
      has_all = true;
    }
  }
  EXPECT_FALSE(has_all) << "`all` is expanded, not carried";
  cmp::Style st;
  ASSERT_EQ(gtext_css_compute_style(ctx.c, block, nullptr, nullptr, &st.s, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(st.value("width"), "auto");
  EXPECT_EQ(st.value("height"), "30px");
  EXPECT_EQ(st.value("direction"), "rtl");
  gtext_css_block_free(block);
}

TEST(CssCascadeBlock, TheBlockHoldsEachShorthandsProvenance) {
  Set s;
  s.add("margin", "1px 2px", K().order(0));
  Out o(s);
  GTEXT_CSS_Declaration_Block * block = nullptr;
  ASSERT_EQ(gtext_css_cascaded_to_block(o.c, nullptr, &block), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_block_count(block), 4u);
  GTEXT_CSS_Declaration_Item it;
  ASSERT_TRUE(gtext_css_block_item(block, 0, &it));
  EXPECT_EQ(it.shorthand, find(R(), "margin"));
  char * text = nullptr;
  size_t n = 0;
  ASSERT_EQ(gtext_css_block_shorthand_value(block, find(R(), "margin"), &text, &n, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(std::string(text, n), "1px 2px");
  gtext_css_serialize_buffer_free(nullptr, text);
  gtext_css_block_free(block);
}

/* ---- the answer does not depend on the order of the candidates --------------------------------------------------------------- */

struct Random {
  std::mt19937_64 rng;
  explicit Random(uint64_t seed) : rng(seed) {}
  uint32_t below(uint32_t n) {
    return uint32_t(rng() % n);
  }
};

/// A random set of candidates whose order-of-appearance keys are all different (so that no two are equal in every key), as the keys and the text of each.
struct Item {
  std::string name, value;
  K keys;
};

std::vector<Item> random_items(uint64_t seed, const std::vector<GTEXT_CSS_Layer_Id> & layers, size_t count) {
  Random r(seed);
  static const char * const props[] = {"color", "width", "margin-left", "margin-inline-start", "margin-right", "margin", "all", "--x", "--y", "direction",
      "height", "block-size", "padding-top", "margin-block-start", "writing-mode", "font"};
  std::vector<Item> items;
  for (size_t i = 0; i < count; i++) {
    Item it;
    it.name = props[r.below(sizeof props / sizeof *props)];
    const uint32_t kind = r.below(10);
    const std::string n = std::to_string(1 + r.below(900));
    if (it.name == "color") {
      it.value = kind == 0 ? "revert" : kind == 1 ? "revert-layer" : kind == 2 ? "inherit" : "rgb(" + n + ", 0, 0)";
    }
    else if (it.name == "width" || it.name == "height" || it.name == "block-size" || it.name == "padding-top" || it.name.rfind("margin-", 0) == 0) {
      it.value = kind == 0 ? "revert" : kind == 1 ? "revert-layer" : kind == 2 ? "revert-rule" : kind == 3 ? "initial" : n + "px";
    }
    else if (it.name == "margin") {
      it.value = kind < 2 ? "revert-layer" : kind == 2 ? "var(--m)" : n + "px " + n + "px";
    }
    else if (it.name == "all") {
      static const char * const w[] = {"initial", "inherit", "unset", "revert", "revert-layer", "revert-rule"};
      it.value = w[r.below(6)];
    }
    else if (it.name == "direction") {
      it.value = kind < 5 ? "rtl" : kind < 7 ? "ltr" : "inherit";
    }
    else if (it.name == "writing-mode") {
      static const char * const w[] = {"vertical-rl", "vertical-lr", "horizontal-tb", "sideways-lr"};
      it.value = kind < 2 ? "revert" : w[r.below(4)];
    }
    else if (it.name == "font") {
      it.value = kind < 3 ? "revert" : "12px serif";
    }
    else {
      it.value = kind == 0 ? "revert" : n;
    }
    K k;
    k.e.origin = GTEXT_CSS_Origin(r.below(5));
    const bool timed = k.e.origin >= GTEXT_CSS_ORIGIN_ANIMATION;
    k.e.important = !timed && r.below(4) == 0;
    k.e.context = r.below(3);
    if (!timed) {
      if (k.e.origin == GTEXT_CSS_ORIGIN_AUTHOR && r.below(8) == 0) {
        k.e.element_attached = true;
      }
      else if (k.e.origin == GTEXT_CSS_ORIGIN_AUTHOR && r.below(8) == 0 && !k.e.important) {
        k.e.presentational_hint = true;
      }
      else if (r.below(2)) {
        k.e.layer = layers[r.below(layers.size()) + (k.e.origin == GTEXT_CSS_ORIGIN_AUTHOR ? 0 : 0)];
        if (k.e.origin != GTEXT_CSS_ORIGIN_AUTHOR) {
          k.e.layer = 0;
        }
      }
    }
    k.e.specificity.a = r.below(3);
    k.e.specificity.b = r.below(3);
    k.e.specificity.c = r.below(3);
    if (r.below(3) == 0) {
      k.e.scoped = true;
      k.e.scope_distance = r.below(4);
    }
    k.e.sheet = uint32_t(i / 16);
    k.e.rule = uint32_t(i % 16);
    k.e.declaration = 0;
    k.e.rule_id = i + 1;
    it.keys = k;
    items.push_back(it);
  }
  return items;
}

std::string cascade_dump(const std::vector<Item> & items, const GTEXT_CSS_Layer_Registry * layers, bool losers, GTEXT_CSS_Status * status) {
  Set s(layers);
  for (const Item & it : items) {
    s.add(it.name, it.value, it.keys);
    if (s.status != GTEXT_CSS_OK) {
      *status = s.status;
      return "";
    }
  }
  GTEXT_CSS_Cascade_Options o{};
  o.keep_losers = losers;
  Out out(s, &o);
  *status = out.status;
  if (out.status != GTEXT_CSS_OK) {
    return "";
  }
  std::string d = out.dump();
  if (losers) {
    for (size_t i = 0; i < gtext_css_cascaded_count(out.c); i++) {
      for (size_t k = 0; k < gtext_css_cascaded_loser_count(out.c, i); k++) {
        GTEXT_CSS_Cascade_Entry e;
        GTEXT_CSS_Loss_Reason why;
        gtext_css_cascaded_loser(out.c, i, k, &e, &why);
        d += "  loser " + std::to_string(e.rule_id) + (why == GTEXT_CSS_LOSS_ROLLED_BACK ? " rolled" : " out") + "\n";
      }
    }
  }
  return d;
}

TEST(CssCascadePermutation, ShufflingTheCandidatesNeverChangesTheWinners) {
  Layers l;
  std::vector<GTEXT_CSS_Layer_Id> layers;
  for (const char * n : {"a", "b", "c", "a.x"}) {
    layers.push_back(l.declare(n));
  }
  l.declare("anon-ish", GTEXT_CSS_ORIGIN_AUTHOR);
  l.declare("a", GTEXT_CSS_ORIGIN_USER);
  l.frozen();
  size_t revert_heavy = 0, distinct = 0;
  for (uint64_t seed = 1; seed <= 60; seed++) {
    const size_t count = 3 + seed % 40;
    std::vector<Item> items = random_items(seed, layers, count);
    GTEXT_CSS_Status st;
    const std::string base = cascade_dump(items, l.r, true, &st);
    ASSERT_EQ(st, GTEXT_CSS_OK) << "seed " << seed;
    std::mt19937_64 rng(seed * 7919);
    for (int round = 0; round < 12; round++) {
      std::shuffle(items.begin(), items.end(), rng);
      const std::string again = cascade_dump(items, l.r, true, &st);
      ASSERT_EQ(st, GTEXT_CSS_OK);
      ASSERT_EQ(again, base) << "seed " << seed << " round " << round;
    }
    bool rolled = false;
    for (size_t at = base.find("rb="); at != std::string::npos && !rolled; at = base.find("rb=", at + 3)) {
      rolled = at + 3 < base.size() && base[at + 3] != '0';
    }
    revert_heavy += rolled;
    distinct += base.size() > 40;
  }
  EXPECT_GT(distinct, 40u) << "the generator makes sets with winners";
  EXPECT_GT(revert_heavy, 5u) << "and some of them roll back";
}

TEST(CssCascadeConcurrency, ASealedSetAndItsResultsAreReadByManyThreadsEachCascadingOnItsOwn) {
  Layers l;
  const GTEXT_CSS_Layer_Id a = l.declare("a"), b = l.declare("b");
  Set s(l.frozen());
  for (int i = 0; i < 40; i++) {
    s.add(i % 3 ? "width" : "margin-inline-start", std::to_string(i + 1) + "px", K().layer(i % 2 ? a : b).order(uint32_t(i)));
    s.add("--c" + std::to_string(i % 7), std::to_string(i), K().order(uint32_t(i), 1));
  }
  s.add("height", "revert-layer", K().layer(b).order(50));
  s.add("height", "3px", K().layer(a).order(51));
  Out reference(s);
  const std::string want = reference.dump();
  ASSERT_NE(want.find("width: "), std::string::npos);
  std::vector<std::thread> threads;
  std::vector<std::string> got(8);
  for (size_t t = 0; t < got.size(); t++) {
    threads.emplace_back([&, t] {
      for (int round = 0; round < 25; round++) {
        GTEXT_CSS_Cascade_Options o{};
        o.keep_losers = round % 2 == 0;
        GTEXT_CSS_Cascaded * c = nullptr;
        if (gtext_css_cascade(s.s, &o, &c, nullptr) != GTEXT_CSS_OK) {
          got[t] = "<failed>";
          return;
        }
        Out view;
        view.c = c;
        got[t] = view.dump();
        view.c = nullptr;
        gtext_css_cascaded_free(c);
        // the shared result is read at the same time
        GTEXT_CSS_Cascade_Entry e;
        gtext_css_cascaded_find(reference.c, find(R(), "width"), nullptr, 0, &e);
      }
    });
  }
  for (std::thread & t : threads) {
    t.join();
  }
  for (const std::string & g : got) {
    EXPECT_EQ(g, want);
  }
}

} // namespace
