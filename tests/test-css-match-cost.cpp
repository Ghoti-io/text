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
 * The cost model of the matcher (css_match.h), asserted by its bound on the context's counters: the matching of any number of selectors against all n elements of a snapshot
 * costs at most (selector nodes) x n memo entries, summed over the run and never their product. Each shape is run at n, 4n and 16n; the counters must stay within the bound
 * and grow no faster than the size, and a plant that drops the memo, the `nth` cache or the `:has()` key is caught here (tools/conformance/CSS_SELECTOR_PLANTS).
 */

#include <string>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_synth_tree.h"

namespace {

struct Outcome {
  GTEXT_CSS_Selector_Match_Counters c;
  size_t matched = 0;
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
};

/// Match `selector` against every element of the tree, walking it from `root` (or each element on its own with `walk` false).
Outcome runq(synth::Tree & t, int root, const char * selector, bool walk = true, GTEXT_CSS_Selector_Match_Options opt = gtext_css_selector_match_options_default()) {
  Outcome out;
  GTEXT_CSS_Selector_Match_Context * ctx = nullptr;
  EXPECT_EQ(gtext_css_selector_match_context_new(&t.table, &opt, &ctx), GTEXT_CSS_OK);
  GTEXT_CSS_Selector_List * l = nullptr;
  EXPECT_EQ(gtext_css_selector_parse(selector, std::strlen(selector), nullptr, &l, nullptr), GTEXT_CSS_OK);
  EXPECT_NE(l, nullptr) << selector;
  struct V {
    GTEXT_CSS_Selector_List * l;
    GTEXT_CSS_Selector_Match_Context * c;
    Outcome * r;
  } v{l, ctx, &out};
  auto visit = [](void * u, GTEXT_CSS_Element_Handle e) {
    V * v = static_cast<V *>(u);
    GTEXT_CSS_Selector_Match_Result r;
    const GTEXT_CSS_Status st = gtext_css_selector_match(v->l, v->c, e, nullptr, &r);
    if (st != GTEXT_CSS_OK) {
      v->r->status = st;
      return false;
    }
    v->r->matched += r.matched ? 1 : 0;
    return true;
  };
  if (walk) {
    GTEXT_CSS_Selector_Match_Result r;
    const GTEXT_CSS_Status st = gtext_css_selector_match_walk(ctx, synth::Tree::H(root), visit, &v, &r);
    if (out.status == GTEXT_CSS_OK) {
      out.status = st;
    }
  }
  else {
    for (size_t i = 1; i < t.e.size() && out.status == GTEXT_CSS_OK; i++) {
      visit(&v, synth::Tree::H(static_cast<int>(i)));
    }
  }
  out.c = gtext_css_selector_match_counters(ctx);
  gtext_css_selector_list_free(l);
  gtext_css_selector_match_context_free(ctx);
  return out;
}

/// A chain of `n` elements named `e`, the last with the class `y`.
int chain(synth::Tree & t, size_t n) {
  int root = 0, p = 0;
  for (size_t i = 0; i < n; i++) {
    p = t.add(p, "e", "", i + 1 == n ? std::vector<std::string>{"y"} : std::vector<std::string>{});
    if (!root) {
      root = p;
    }
  }
  return root;
}

/// A parent with `n` children named `x`, the last with the class `t`.
int wide(synth::Tree & t, size_t n, const char * name = "x") {
  const int root = t.add(0, "r");
  for (size_t i = 0; i < n; i++) {
    t.add(root, name, "", i + 1 == n ? std::vector<std::string>{"t"} : std::vector<std::string>{"c"});
  }
  return root;
}

constexpr size_t kBase = 600;

}  // namespace

TEST(CssMatchCost, DescendantChainsAreLinearAndNeverBacktrackExponentially) {
  // "e e e e e .y" over a chain: a naive right to left matcher tries every way of assigning the five ancestors, which is exponential; the memo makes each (compound, element) once
  const size_t sizes[] = {kBase, 4 * kBase, 16 * kBase};
  size_t prev = 0;
  for (size_t n : sizes) {
    synth::Tree t;
    const int root = chain(t, n);
    const Outcome r = runq(t, root, "e e e e e .y");
    ASSERT_EQ(r.status, GTEXT_CSS_OK);
    EXPECT_EQ(r.matched, 1u);
    EXPECT_LE(r.c.compounds_tested, 6 * n) << "(selector compounds) x elements, summed over the run";
    EXPECT_LE(r.c.memo_entries, 12 * n);
    if (prev) {
      EXPECT_LE(r.c.compounds_tested, prev * 4 + 64) << "grows with the size and no faster";
    }
    prev = r.c.compounds_tested;
  }
}

TEST(CssMatchCost, ADescendantChainThatCannotMatchIsLinearToo) {
  for (size_t n : {kBase, 16 * kBase}) {
    synth::Tree t;
    const int root = chain(t, n);
    const Outcome r = runq(t, root, "e e e e e e e e e e .nothing");
    ASSERT_EQ(r.status, GTEXT_CSS_OK);
    EXPECT_EQ(r.matched, 0u);
    EXPECT_LE(r.c.compounds_tested, 12 * n);
  }
}

TEST(CssMatchCost, SubsequentSiblingChainsOverWideParents) {
  for (size_t n : {kBase, 4 * kBase, 16 * kBase}) {
    synth::Tree t;
    const int root = wide(t, n);
    const Outcome r = runq(t, root, "a ~ b ~ c");
    ASSERT_EQ(r.status, GTEXT_CSS_OK);
    EXPECT_EQ(r.matched, 0u) << "there is no a";
    EXPECT_LE(r.c.compounds_tested, 4 * n);
    const Outcome s = runq(t, root, "x ~ x ~ x ~ .t");
    ASSERT_EQ(s.status, GTEXT_CSS_OK);
    EXPECT_EQ(s.matched, 1u);
    EXPECT_LE(s.c.compounds_tested, 5 * n);
    EXPECT_LE(s.c.memo_entries, 10 * n);
  }
}

TEST(CssMatchCost, NthChildOfSCountsTheSiblingsOnceForTheParent) {
  for (size_t n : {kBase, 4 * kBase, 16 * kBase}) {
    synth::Tree t;
    const int root = wide(t, n);
    const Outcome r = runq(t, root, "x:nth-child(odd of .c)");
    ASSERT_EQ(r.status, GTEXT_CSS_OK);
    EXPECT_EQ(r.matched, (n - 1 + 1) / 2) << "the n-1 siblings with .c, the odd positions";
    EXPECT_EQ(r.c.nth_walks, 1u) << "one walk for the one parent, whatever the number of children";
    EXPECT_LE(r.c.compounds_tested, 8 * n);
    const Outcome s = runq(t, root, "x:nth-last-of-type(3n+1)");
    ASSERT_EQ(s.status, GTEXT_CSS_OK);
    EXPECT_EQ(s.c.nth_walks, 1u);
    const Outcome u = runq(t, root, "x:nth-child(2n+1):nth-last-child(2n)");
    ASSERT_EQ(u.status, GTEXT_CSS_OK);
    EXPECT_LE(u.c.nth_walks, 1u);
  }
}

TEST(CssMatchCost, NthWalksAreOneForEachParentOfAWideTree) {
  synth::Tree t;
  const int root = t.add(0, "r");
  const size_t parents = 300;
  for (size_t i = 0; i < parents; i++) {
    const int p = t.add(root, "p");
    for (int k = 0; k < 8; k++) {
      t.add(p, "c", "", {"x"});
    }
  }
  const Outcome r = runq(t, root, "c:nth-child(3)");
  ASSERT_EQ(r.status, GTEXT_CSS_OK);
  EXPECT_EQ(r.matched, parents);
  EXPECT_EQ(r.c.nth_walks, parents) << "a walk for each parent that has children asked about";
}

TEST(CssMatchCost, HasOfEveryRelationVisitsEachStepAndElementOnce) {
  for (size_t n : {kBase, 4 * kBase, 16 * kBase}) {
    synth::Tree c;
    const int croot = chain(c, n);
    synth::Tree w;
    const int wroot = wide(w, n);
    // :has() of a descendant over a chain: every element has the leaf below it, found through the memo of the elements below
    Outcome r = runq(c, croot, "e:has(.y)");
    ASSERT_EQ(r.status, GTEXT_CSS_OK);
    EXPECT_EQ(r.matched, n - 1);
    EXPECT_LE(r.c.has_visits, 4 * n) << "m x n";
    r = runq(c, croot, "e:has(> .y)");
    ASSERT_EQ(r.status, GTEXT_CSS_OK);
    EXPECT_EQ(r.matched, 1u);
    EXPECT_LE(r.c.has_visits, 4 * n);
    r = runq(c, croot, "e:has(> e > e > .y)");
    ASSERT_EQ(r.status, GTEXT_CSS_OK);
    EXPECT_EQ(r.matched, 1u);
    EXPECT_LE(r.c.has_visits, 8 * n);
    r = runq(c, croot, "e:not(:has(.y))");
    ASSERT_EQ(r.status, GTEXT_CSS_OK);
    EXPECT_EQ(r.matched, 1u);
    EXPECT_LE(r.c.has_visits, 4 * n);
    // siblings
    r = runq(w, wroot, "x:has(~ .t)");
    ASSERT_EQ(r.status, GTEXT_CSS_OK);
    EXPECT_EQ(r.matched, n - 1);
    EXPECT_LE(r.c.has_visits, 6 * n) << "the later siblings are found through the memo of the sibling after";
    r = runq(w, wroot, "x:has(+ .t)");
    ASSERT_EQ(r.status, GTEXT_CSS_OK);
    EXPECT_EQ(r.matched, 1u);
    EXPECT_LE(r.c.has_visits, 4 * n);
    r = runq(w, wroot, "r:has(> .t)");
    ASSERT_EQ(r.status, GTEXT_CSS_OK);
    EXPECT_EQ(r.matched, 1u);
    r = runq(w, wroot, "x:has(~ .c ~ .t)");
    ASSERT_EQ(r.status, GTEXT_CSS_OK);
    EXPECT_EQ(r.matched, n - 2);
    EXPECT_LE(r.c.has_visits, 10 * n);
  }
}

TEST(CssMatchCost, ASelectorListIsLinearInItsLengthAndTheElements) {
  const size_t n = 4 * kBase;
  synth::Tree t;
  const int root = wide(t, n);
  std::string sel;
  const size_t k = 40;
  for (size_t i = 0; i < k; i++) {
    sel += (i ? ", " : "") + std::string("x.c") + (i % 2 ? "" : ":nth-child(" + std::to_string(i + 1) + ")") + " ~ .nothing" + std::to_string(i);
  }
  const Outcome r = runq(t, root, sel.c_str());
  ASSERT_EQ(r.status, GTEXT_CSS_OK);
  EXPECT_LE(r.c.compounds_tested, 3 * k * n);
  EXPECT_LE(r.c.nth_walks, 1u);
}

TEST(CssMatchCost, TheBoundsHoldWithoutTheFilterAndTheFilterOnlyRejects) {
  const size_t n = 4 * kBase;
  synth::Tree t;
  const int root = chain(t, n);
  GTEXT_CSS_Selector_Match_Options off = gtext_css_selector_match_options_default();
  off.disable_bloom = true;
  const Outcome a = runq(t, root, "section e e .y", true);
  const Outcome b = runq(t, root, "section e e .y", true, off);
  ASSERT_EQ(a.status, GTEXT_CSS_OK);
  ASSERT_EQ(b.status, GTEXT_CSS_OK);
  EXPECT_EQ(a.matched, b.matched);
  EXPECT_EQ(a.matched, 0u);
  EXPECT_GE(a.c.bloom_rejects, n - 2) << "no ancestor is a section: the filter rejects every element without matching it";
  EXPECT_LT(a.c.compounds_tested, b.c.compounds_tested);
  EXPECT_EQ(b.c.bloom_rejects, 0u);
}

TEST(CssMatchCost, TheMemoIsWhatMakesItLinearAndWithoutItTheWorkLimitIsTheStatus) {
  const size_t n = 200;
  synth::Tree t;
  const int root = chain(t, n);
  GTEXT_CSS_Selector_Match_Options nomemo = gtext_css_selector_match_options_default();
  nomemo.disable_memo = true;
  nomemo.disable_bloom = true;
  nomemo.max_work = 200000;
  const Outcome r = runq(t, root, "nothing e e e e e e e .y", true, nomemo);
  EXPECT_EQ(r.status, GTEXT_CSS_E_LIMIT_MATCH_WORK) << "without the memo the chain of ancestors is tried in every combination, and the limit is the answer";
  GTEXT_CSS_Selector_Match_Options nobloom = gtext_css_selector_match_options_default();
  nobloom.disable_bloom = true;
  const Outcome m = runq(t, root, "nothing e e e e e e e .y", true, nobloom);
  EXPECT_EQ(m.status, GTEXT_CSS_OK);
  EXPECT_EQ(m.matched, 0u);
  EXPECT_LE(m.c.compounds_tested, 9 * n) << "with the memo: (selector compounds) x elements";
}

TEST(CssMatchCost, AHasThatMentionsScopeIsSharedAcrossAnchors) {
  /* `:scope` inside `:has()` is the scope of the query (web-platform-tests' has-argument-with-explicit-scope.html), not the anchor: the chain below a point does not depend on the
   * anchor, so it is memoized and a chain of 3,000 anchors costs linear work. */
  const size_t n = 3000;
  synth::Tree t;
  const int root = chain(t, n);
  const Outcome r = runq(t, root, "e:has(:scope .y)", true, gtext_css_selector_match_options_default());
  EXPECT_EQ(r.status, GTEXT_CSS_OK);
  EXPECT_EQ(r.matched, 0u) << "the scope is the root, which is no descendant of any anchor";
  EXPECT_LE(r.c.has_visits, 12 * n) << "linear in the elements, as a :has() that does not mention the scope";
}

TEST(CssMatchCost, AHasOverASiblingRunThatMentionsScopeIsLinearToo) {
  const size_t n = 2000;
  synth::Tree t;
  int first = 0;
  for (size_t i = 0; i < n; i++) {
    const int e = t.add(0, "div");
    first = first ? first : e;
  }
  const Outcome r = runq(t, first, "div:has(~ div:not(:is(:scope ~ *)))", false, gtext_css_selector_match_options_default());
  EXPECT_EQ(r.status, GTEXT_CSS_OK) << "it hit the work limit when :scope tied the chain to the anchor";
}

namespace {
TEST(CssMatchCost, TheLongestAllowedChainOverADeepTreeDoesNotExhaustTheStack) {
  // the matcher recurses once per compound of a complex selector, and the parser allows 1,024; 1,024 descendant and child compounds over a 20,000 deep tree must just answer
  synth::Tree t;
  const int root = chain(t, 20000);
  std::string sel = "e";
  for (int i = 1; i < 1023; i++) {
    sel += i % 3 ? " e" : " > e";
  }
  sel += " .y";
  const Outcome r = runq(t, root, sel.c_str());
  EXPECT_EQ(r.status, GTEXT_CSS_OK);
  EXPECT_LE(r.c.compounds_tested, 1100u * 20000u);
}
} // namespace

TEST(CssMatchCost, NthOfTypeOverAWideParentOfDistinctTypesIsLinear) {
  // the types of the siblings were found by a scan of the types seen so far: 20,000 distinct names cost two hundred million steps and reached the work limit
  synth::Tree t;
  const int root = t.add(0, "r");
  int last = 0;
  for (int i = 0; i < 20000; i++) {
    last = t.add(root, "t" + std::to_string(i));
  }
  const Outcome r = runq(t, root, ":nth-of-type(1)");
  EXPECT_EQ(r.status, GTEXT_CSS_OK);
  EXPECT_EQ(r.matched, 20001u);
  (void)last;
}

TEST(CssMatchCost, ALongListOfSingleCompoundSelectorsKeepsNoMemoEntries) {
  // 60,000 class selectors over 150 elements was nine million (selector, element) entries and the memo limit; a selector of one compound is looked up as cheaply as it is found
  synth::Tree t;
  const int root = wide(t, 150);
  std::string sel;
  for (int i = 0; i < 60000; i++) {
    sel += ".k" + std::to_string(i) + ",";
  }
  sel += ".t";
  const Outcome r = runq(t, root, sel.c_str());
  EXPECT_EQ(r.status, GTEXT_CSS_OK);
  EXPECT_EQ(r.matched, 1u);
  EXPECT_LE(r.c.memo_entries, 1000u);
}

TEST(CssMatchCost, TheScopeElementChangedBetweenQueriesOfOneContextIsFollowedByEveryCache) {
  // a style rule per scoping root, or a query on each of several roots, asks one context with another `:scope` each time: the memo, the position caches and the `:has()` memos are keyed by it
  synth::Tree t;
  const int a = t.add(0, "div");
  const int pa1 = t.add(a, "p");
  const int pa2 = t.add(a, "p");
  const int b = t.add(0, "div");
  const int pb1 = t.add(b, "p");
  const int pb2 = t.add(b, "p");
  GTEXT_CSS_Selector_Match_Options opt = gtext_css_selector_match_options_default();
  GTEXT_CSS_Selector_Match_Context * ctx = nullptr;
  ASSERT_EQ(gtext_css_selector_match_context_new(&t.table, &opt, &ctx), GTEXT_CSS_OK);
  const char * sels[] = {":scope > p", "p:nth-child(1 of :scope > *)", "div:has(:scope p) p", ":scope p:not(:scope p + p)", ":has(> :scope)"};
  const int roots[] = {a, b, a, 0, b, a};
  for (const char * sel : sels) {
    GTEXT_CSS_Selector_List * l = nullptr;
    ASSERT_EQ(gtext_css_selector_parse(sel, std::strlen(sel), nullptr, &l, nullptr), GTEXT_CSS_OK);
    ASSERT_NE(l, nullptr) << sel;
    std::vector<std::vector<bool>> first;
    for (int pass = 0; pass < 2; pass++) {
      for (size_t k = 0; k < sizeof roots / sizeof roots[0]; k++) {
        gtext_css_selector_match_set_scope(ctx, synth::Tree::H(roots[k]), nullptr);
        std::vector<bool> row;
        for (int e : {a, pa1, pa2, b, pb1, pb2}) {
          GTEXT_CSS_Selector_Match_Result r;
          ASSERT_EQ(gtext_css_selector_match(l, ctx, synth::Tree::H(e), nullptr, &r), GTEXT_CSS_OK);
          row.push_back(r.matched);
        }
        if (pass == 0) {
          first.push_back(row);
        }
        else {
          EXPECT_EQ(row, first[k]) << sel << " scope " << roots[k] << ": the answer on a second visit to the same scope differs";
        }
      }
    }
    // the same scope asked in a fresh context gives the same answers as the shared one
    for (size_t k = 0; k < sizeof roots / sizeof roots[0]; k++) {
      GTEXT_CSS_Selector_Match_Options o2 = opt;
      o2.scope = synth::Tree::H(roots[k]);
      GTEXT_CSS_Selector_Match_Context * fresh = nullptr;
      ASSERT_EQ(gtext_css_selector_match_context_new(&t.table, &o2, &fresh), GTEXT_CSS_OK);
      std::vector<bool> row;
      for (int e : {a, pa1, pa2, b, pb1, pb2}) {
        GTEXT_CSS_Selector_Match_Result r;
        ASSERT_EQ(gtext_css_selector_match(l, fresh, synth::Tree::H(e), nullptr, &r), GTEXT_CSS_OK);
        row.push_back(r.matched);
      }
      EXPECT_EQ(row, first[k]) << sel << " scope " << roots[k] << ": a context that had other scopes before differs from a fresh one";
      gtext_css_selector_match_context_free(fresh);
    }
    gtext_css_selector_list_free(l);
  }
  gtext_css_selector_match_context_free(ctx);
}
