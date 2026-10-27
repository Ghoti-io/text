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
 * What the review of the cascade found, one test each, and the mutants it showed the first tests did not kill: `revert-rule` and the importance of a rule's
 * declarations, a chain of important element-attached `revert-layer`, the scope key on declarations that have no rule, a layer registry for each context, the
 * places `@import` may be and the statements a style rule may hold, the path of a layer with a dot in its name, the origin of a presentational hint, and the
 * fields of the dump.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_cascade_test_support.h"
#include "css_test_support.h"

using namespace cas;

namespace {

/* ---- revert-rule ------------------------------------------------------------------------------------------------ */

TEST(CssCascadeFixes, RevertRuleTakesOutTheWholeRuleWhateverTheImportanceOfItsDeclarations) {
  /* 7.3.6: "the cascaded value is rolled back such that the specified value is calculated as if the current style rule had not been present at all." A rule's
     declarations are of both importances; the normal one of the same rule is as absent as the important one that rolls it back. */
  Set s;
  s.add("width", "1px", K().order(0, 0, 0));
  s.add("width", "5px", K().order(0, 1, 0));
  s.add("width", "revert-rule !important", K().order(0, 1, 1));
  EXPECT_EQ(Out(s).value("width"), "1px");
  /* a normal revert-rule, and another rule's important declaration, which is not taken out */
  Set t;
  t.add("width", "1px", K().order(0, 0, 0));
  t.add("width", "revert-rule", K().order(0, 1, 1));
  t.add("width", "5px !important", K().order(0, 2, 0));
  EXPECT_EQ(Out(t).value("width"), "5px");
  /* a rule is its origin, its hint-or-attached kind, its context, its sheet and its rule: the same ordinals elsewhere are another rule */
  Set context;
  context.add("width", "2px", K().order(0, 1, 0).ctx(1));
  context.add("width", "revert-rule", K().order(0, 1, 1));
  context.add("width", "9px", K().order(0, 1, 0));
  EXPECT_EQ(Out(context).value("width"), "2px") << "the rule of the other context";
  Set origin;
  origin.add("width", "3px", K().user().order(0, 1, 2));
  origin.add("width", "revert-rule", K().order(0, 1, 1));
  origin.add("width", "9px", K().order(0, 1, 0));
  EXPECT_EQ(Out(origin).value("width"), "3px") << "the rule of the other origin";
  Set attached;
  attached.add("width", "4px", K().attached().order(0, 1, 3));
  attached.add("width", "revert-rule !important", K().order(0, 1, 1));
  attached.add("width", "9px", K().order(0, 1, 0));
  EXPECT_EQ(Out(attached).value("width"), "4px") << "the style attribute is not a rule of the same ordinals";
  Set hint;
  hint.add("width", "5px", K().hint().order(0, 1, 4));
  hint.add("width", "revert-rule !important", K().order(0, 1, 1));
  hint.add("width", "9px", K().order(0, 1, 0));
  EXPECT_EQ(Out(hint).value("width"), "5px") << "nor is a hint";
}

TEST(CssCascadeFixes, ManyRevertRulesAreEachTakenOutOnce) {
  Set s;
  s.add("width", "7px", K().user());
  for (uint32_t i = 0; i < 200; i++) {
    s.add("width", "4px", K().order(0, i, 0));
    s.add("width", "revert-rule", K().order(0, i, 1));
  }
  /* every rule has a 4px and a revert-rule: each is taken out in turn, down to the user's */
  Out o(s);
  EXPECT_EQ(o.value("width"), "7px");
  GTEXT_CSS_Cascade_Entry e;
  size_t rollbacks = 0;
  ASSERT_TRUE(gtext_css_cascaded_at(o.c, 0, &e, &rollbacks));
  EXPECT_EQ(rollbacks, 200u);
}

/* ---- chained important element-attached revert-layer ------------------------------------------------------------- */

TEST(CssCascadeFixes, ChainedImportantElementAttachedRevertLayerKeepsEarlierSkips) {
  /* 7.3.5: "For revert-layer in important element-attached styles, however, it only reverts the element-attached styles and the intervening animation origin"
     -- of each context the walk passes through. */
  Set s;
  s.add("z-index", "4", K().ua());
  s.add("z-index", "5", K().attached().ctx(2).order(0, 0));
  s.add("z-index", "revert-layer", K().attached().important().ctx(2).order(0, 1));
  s.add("z-index", "revert-layer", K().attached().important().ctx(1).order(0, 2));
  EXPECT_EQ(Out(s).value("z-index"), "4");
  /* one roll only passes its own context */
  Set t;
  t.add("z-index", "4", K().ua());
  t.add("z-index", "5", K().attached().ctx(2).order(0, 0));
  t.add("z-index", "6", K().attached().ctx(1).order(0, 1));
  t.add("z-index", "revert-layer", K().attached().important().ctx(2).order(0, 2));
  EXPECT_EQ(Out(t).value("z-index"), "6") << "the normal element-attached declaration of the other context is not behind it";
  /* in the user origin the animation origin is not the one between */
  Set u;
  u.add("z-index", "1", K().user());
  u.add("z-index", "2", K().user().attached());
  u.add("z-index", "3", K().animation());
  u.add("z-index", "revert-layer", K().user().attached().important());
  EXPECT_EQ(Out(u).value("z-index"), "3");
  Set a;
  a.add("z-index", "1", K());
  a.add("z-index", "2", K().attached());
  a.add("z-index", "3", K().animation());
  a.add("z-index", "revert-layer", K().attached().important());
  EXPECT_EQ(Out(a).value("z-index"), "1") << "the author's: the animation origin and the normal attached declaration are behind it";
}

TEST(CssCascadeFixes, AnImportantElementAttachedRevertLayerOfOneOriginDoesNotSkipAnotherOriginsAttachedStyles) {
  Set s;
  s.add("z-index", "1", K().ua());
  s.add("z-index", "2", K().user().attached());
  s.add("z-index", "revert-layer", K().attached().important());
  EXPECT_EQ(Out(s).value("z-index"), "2") << "the user origin's own element-attached declaration is another layer";
}

/* ---- scope on declarations that have no rule ---------------------------------------------------------------------- */

TEST(CssCascadeFixes, ScopeIsIgnoredOnHintsStyleAttributesAndTimedDeclarationsAndClampedAtTheTop) {
  /* Cascade 6, 3.1: proximity compares "declarations that appear in style rules with different scoping roots"; a hint, a style attribute and an animation
     declaration appear in none, so a scope given to one gains nothing. */
  for (int kind = 0; kind < 3; kind++) {
    SCOPED_TRACE(kind);
    Set s;
    K first = kind == 0 ? K().hint() : kind == 1 ? K().attached() : K().animation();
    K second = kind == 0 ? K().hint() : kind == 1 ? K().attached() : K().animation();
    s.add("z-index", "5", first.scope(0).order(0, 0));
    s.add("z-index", "6", second.order(0, 1));
    EXPECT_EQ(Out(s).value("z-index"), "6") << "distance 0 on a rule-less declaration gains nothing";
    GTEXT_CSS_Cascade_Entry e;
    ASSERT_TRUE(gtext_css_candidates_entry(s.s, 0, &e));
    EXPECT_FALSE(e.scoped) << "and is not kept";
    EXPECT_EQ(e.scope_distance, 0u);
  }
  /* a style rule that is not scoped keeps no distance either */
  Set u;
  K k = K();
  k.e.scope_distance = 5;
  u.add("z-index", "1", k);
  GTEXT_CSS_Cascade_Entry e;
  ASSERT_TRUE(gtext_css_candidates_entry(u.s, 0, &e));
  EXPECT_FALSE(e.scoped);
  EXPECT_EQ(e.scope_distance, 0u);
  /* the largest distance is still a scope: it beats none, and ties the one below it */
  Set t;
  t.add("z-index", "1", K().scope(UINT32_MAX).order(0, 0));
  t.add("z-index", "2", K().scope(UINT32_MAX - 1).order(0, 1));
  t.add("z-index", "3", K().order(0, 2));
  EXPECT_EQ(Out(t).value("z-index"), "2");
  Set v;
  v.add("z-index", "1", K().scope(UINT32_MAX).order(0, 0));
  v.add("z-index", "2", K().order(0, 1));
  EXPECT_EQ(Out(v).value("z-index"), "1");
  Set w;
  w.add("z-index", "1", K().scope(UINT32_MAX - 1).order(0, 0));
  w.add("z-index", "2", K().scope(UINT32_MAX).order(0, 1));
  EXPECT_EQ(Out(w).value("z-index"), "2") << "the clamp makes them equal: the later wins";
  Set x;
  x.add("z-index", "1", K().hint().scope(3).order(0, 0));
  EXPECT_NE(Out(x).dump().find("scope=-"), std::string::npos);
  /* animation and transition declarations are refused a hint flag, as they are an important one */
  Set bad;
  bad.add("z-index", "1", K().animation().hint());
  EXPECT_EQ(bad.status, GTEXT_CSS_E_INVALID);
  bad.add("z-index", "1", K().transition().hint());
  EXPECT_EQ(bad.status, GTEXT_CSS_E_INVALID);
}

/* ---- a layer registry for each context --------------------------------------------------------------------------- */

TEST(CssCascadeFixes, EachContextHasItsOwnLayerOrder) {
  /* 6.4.3: "Cascade layers are scoped to their origin and context, so the ordering of layers in the light DOM has no impact on the order of identically-named
     layers in the shadow DOM (and vice versa)." */
  Layers outer, inner;
  const GTEXT_CSS_Layer_Id oa = outer.declare("a"), ob = outer.declare("b");
  const GTEXT_CSS_Layer_Id ib = inner.declare("b"), ia = inner.declare("a");
  outer.frozen();
  inner.frozen();
  Set s;
  ASSERT_EQ(gtext_css_candidates_use_layers(s.s, 0, outer.r), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_candidates_use_layers(s.s, 1, inner.r), GTEXT_CSS_OK);
  s.add("width", "1px", K().ctx(0).layer(oa).order(0, 0));
  s.add("width", "2px", K().ctx(0).layer(ob).order(0, 1));
  s.add("height", "1px", K().ctx(1).layer(ia).order(0, 2));
  s.add("height", "2px", K().ctx(1).layer(ib).order(0, 3));
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  Out o(s);
  EXPECT_EQ(o.value("width"), "2px") << "b is after a in the outer context";
  EXPECT_EQ(o.value("height"), "1px") << "and before it in the inner one";
  const std::string dump = o.dump();
  EXPECT_NE(dump.find("height: 1px [author layer=a ctx=1"), std::string::npos) << dump;
  /* important turns each round */
  Set t;
  ASSERT_EQ(gtext_css_candidates_use_layers(t.s, 1, inner.r), GTEXT_CSS_OK);
  t.add("height", "1px !important", K().ctx(1).layer(ia).order(0, 0));
  t.add("height", "2px !important", K().ctx(1).layer(ib).order(0, 1));
  EXPECT_EQ(Out(t).value("height"), "2px");
}

TEST(CssCascadeFixes, TheLayerRegistryOfAContextIsGivenBeforeItsCandidatesAndMustBeFrozen) {
  Layers l, other;
  const GTEXT_CSS_Layer_Id a = l.declare("a");
  l.frozen();
  Set s(l.r);  // the default registry
  s.add("width", "1px", K().ctx(3).layer(a));
  EXPECT_EQ(s.status, GTEXT_CSS_OK) << "a context with no registry of its own uses the default";
  EXPECT_EQ(gtext_css_candidates_use_layers(s.s, 3, other.frozen()), GTEXT_CSS_E_INVALID) << "after the context's first candidate";
  EXPECT_EQ(gtext_css_candidates_use_layers(s.s, 4, other.r), GTEXT_CSS_OK);
  Layers unfrozen;
  EXPECT_EQ(gtext_css_candidates_use_layers(s.s, 5, unfrozen.r), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_candidates_use_layers(nullptr, 5, l.r), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_candidates_use_layers(s.s, 5, nullptr), GTEXT_CSS_E_INVALID);
  /* a layer that is not one of the registry of the entry's context is refused, whatever its number */
  Layers two;
  two.declare("x");
  const GTEXT_CSS_Layer_Id deep = two.declare("y");
  two.declare("z");
  two.frozen();
  Set t(l.r);
  ASSERT_EQ(gtext_css_candidates_use_layers(t.s, 7, two.r), GTEXT_CSS_OK);
  t.add("width", "1px", K().ctx(7).layer(deep + 50));
  EXPECT_EQ(t.status, GTEXT_CSS_E_INVALID);
  t.add("width", "1px", K().ctx(7).layer(deep));
  EXPECT_EQ(t.status, GTEXT_CSS_OK);
  /* replacing the registry of a context that has no candidates yet is allowed; a set that is sealed takes none */
  EXPECT_EQ(gtext_css_candidates_use_layers(t.s, 8, l.r), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_candidates_use_layers(t.s, 8, two.r), GTEXT_CSS_OK);
  Out o(t);
  EXPECT_EQ(gtext_css_candidates_use_layers(t.s, 9, l.r), GTEXT_CSS_E_INVALID);
  /* a layer in a context with no registry at all */
  Set none;
  none.add("width", "1px", K().ctx(2).layer(1));
  EXPECT_EQ(none.status, GTEXT_CSS_E_INVALID);
}

/* ---- the layers of a sheet: where an @import may be, and the statements a style rule may hold -------------------- */

struct Read {
  Layers layers;
  css_test::Parsed parsed;
  GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;
  GTEXT_CSS_Layer_Result result{};
  int asked = 0;
  explicit Read(const std::string & css, int answer = -1, size_t max_depth = 0) {
    css_test::parse_into(parsed, gtext_css_parse_stylesheet, css);
    EXPECT_EQ(parsed.status, GTEXT_CSS_OK);
    GTEXT_CSS_Layer_Collect_Options o;
    std::memset(&o, 0, sizeof o);
    o.user = this;
    o.max_rule_depth = max_depth;
    struct Hook {
      static bool yes(void * u, const GTEXT_CSS_Rule *) {
        static_cast<Read *>(u)->asked++;
        return true;
      }
      static bool no(void * u, const GTEXT_CSS_Rule *) {
        static_cast<Read *>(u)->asked++;
        return false;
      }
    };
    if (answer == 1) o.active = Hook::yes;
    if (answer == 0) o.active = Hook::no;
    status = gtext_css_cascade_collect_layers(layers.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, parsed.tree, &o, &result);
  }
  std::string order() {
    layers.frozen();
    size_t n = gtext_css_layer_count(layers.r, GTEXT_CSS_ORIGIN_AUTHOR);
    std::vector<std::string> by(n + 1);
    for (GTEXT_CSS_Layer_Id id = 1; id < 4096; id++) {
      GTEXT_CSS_Layer_Info info;
      if (!gtext_css_layer_info(layers.r, id, &info) || info.origin != GTEXT_CSS_ORIGIN_AUTHOR) continue;
      char buf[128];
      gtext_css_layer_path(layers.r, id, buf, sizeof buf);
      by[info.rank] = buf;
    }
    std::string out;
    for (size_t i = 0; i < n; i++) out += (out.empty() ? "" : ", ") + by[i];
    return out;
  }
};

TEST(CssLayerFixes, AnImportAfterARuleIsInvalidAndDeclaresNoLayer) {
  /* 6.2: "Any @import rules must precede all other valid at-rules and style rules in a style sheet (ignoring @charset, @supports-condition, and @layer statement
     rules) and must not have any other valid at-rules or style rules between it and previous @import rules, or else the @import rule is invalid." */
  EXPECT_EQ(Read("h1 { color: red } @import url(a.css) layer(late); @layer b;").order(), "b");
  EXPECT_EQ(Read("@media print { } @import url(a.css) layer(late);").order(), "");
  EXPECT_EQ(Read("@layer q { } @import url(a.css) layer(late);").order(), "q");
  EXPECT_EQ(Read("@font-face { } @import url(a.css) layer(late);").order(), "");
  /* an @import must name a <url> or a <string> first: with another token there it is invalid, and the layer() after it declares nothing */
  EXPECT_EQ(Read("@import foo layer(b);").order(), "");
  EXPECT_EQ(Read("@import layer(b);").order(), "");
  /* "@charset" and "@layer statement rules" are ignored, and other imports do not close the start */
  EXPECT_EQ(Read("@charset \"utf-8\"; @layer a; @import url(a.css) layer(b);").order(), "a, b");
  EXPECT_EQ(Read("@import url(a.css) layer(a); @import url(b.css) layer(b); @import url(c.css) layer(c);").order(), "a, b, c");
  /* 6.4.4.2: "Any @layer rule that comes after an @import or @namespace rule will cause any subsequent @import or @namespace rules to be ignored" (the example
     the section calls invalid) */
  EXPECT_EQ(Read("@import url(default.css) layer(default); @layer theme; @import url(components.css) layer(components); @layer theme { a { color: red } }")
                .order(),
      "default, theme");
  EXPECT_EQ(Read("@layer default, theme, components; @import url(theme.css) layer(theme); @layer default { a { color: red } }").order(), "default, theme, components");
  EXPECT_EQ(Read("@layer default; @import url(theme.css) layer(theme); @layer components; @layer default { a { color: red } }").order(), "default, theme, components");
  /* @import and @namespace must be consecutive: an @import after an @namespace is invalid */
  EXPECT_EQ(Read("@namespace svg url(http://www.w3.org/2000/svg); @import url(a.css) layer(late);").order(), "");
  EXPECT_EQ(Read("@import url(a.css) layer(a); @namespace svg url(http://www.w3.org/2000/svg); @import url(b.css) layer(b);").order(), "a");
  /* an invalid layer statement does not close the start, and neither does an invalid import */
  EXPECT_EQ(Read("@layer unset; @import url(a.css) layer(a);").order(), "a");
  EXPECT_EQ(Read("@import layer; @import url(a.css) layer(a);").order(), "a");
}

TEST(CssLayerFixes, AnImportNeedsAUrlAndIsReadOnlyAtTheTopOfTheSheet) {
  /* 6.2: "@import [ <url> | <string> ] [ layer | layer(<layer-name>) ]? <import-conditions>" */
  EXPECT_EQ(Read("@import layer; @layer q;").order(), "q") << "no <url> or <string>: no anonymous layer";
  EXPECT_EQ(Read("@import layer(x);").order(), "");
  EXPECT_EQ(Read("@import \"a.css\" layer(s); @import url(b.css) layer(t); @import url('c.css') layer;").order(), "s, t, (anonymous)");
  EXPECT_EQ(Read("@IMPORT url(a.css) LAYER(x); @Import \"b.css\" Layer;").order(), "x, (anonymous)") << "names and the layer keyword are ASCII case-insensitive";
  /* an @import in a block is invalid (the top level only): it declares nothing, in a layer or a conditional rule or a style rule */
  EXPECT_EQ(Read("@layer a { @import url(x.css) layer(in); }").order(), "a");
  EXPECT_EQ(Read("@layer a { @import url(x.css) layer; }").order(), "a");
  EXPECT_EQ(Read("@media print { @import url(x.css) layer(in); }", 1).order(), "");
  EXPECT_EQ(Read(".a { @import url(x.css) layer(in); }").order(), "");
}

TEST(CssLayerFixes, AnImportsConditionsGateItsLayer) {
  /* 6.2: "The layer is added to the layer order even if the import fails to load the stylesheet, but is subject to any import conditions (just as if declared by
     an @layer rule wrapped in @media and/or @supports blocks)." */
  Read yes("@import url(a.css) layer(l) screen; @import url(b.css) layer(m) supports(display: grid);", 1);
  EXPECT_EQ(yes.order(), "l, m");
  EXPECT_EQ(yes.asked, 2);
  Read no("@import url(a.css) layer(l) screen; @import url(b.css) layer(m);", 0);
  EXPECT_EQ(no.order(), "m");
  EXPECT_EQ(no.asked, 1) << "an import with no condition is not asked about";
  Read none("@import url(a.css) layer(l) screen;");
  EXPECT_EQ(none.status, GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_STREQ(none.result.name, "active_rule");
}

TEST(CssLayerFixes, AStatementIsNotAllowedInAStyleRuleButABlockIs) {
  /* CSS Nesting 3.3: "Specifically, these rules are capable of being nested group rules: all the conditional group rules (@container, @media, @supports), @layer,
     @scope". A statement has no block, so it is not a group rule, and inside a style rule it is invalid. */
  EXPECT_EQ(Read(".a { @layer s; }").order(), "");
  EXPECT_EQ(Read(".a { @layer s, t; color: red; @layer u { color: blue } }").order(), "u");
  EXPECT_EQ(Read(".a { @media print { @layer s; } }", 1).order(), "") << "nor in a group rule nested in one";
  EXPECT_EQ(Read(".a { @media print { @layer s { color: red } } }", 1).order(), "s");
  /* a statement in a layer block, in a group rule at the top, and in @scope is allowed */
  EXPECT_EQ(Read("@layer a { @layer b; }").order(), "a.b, a");
  EXPECT_EQ(Read("@media print { @layer s; }", 1).order(), "s");
  EXPECT_EQ(Read("@scope (.x) { @layer s; }").order(), "s");
}

TEST(CssLayerFixes, ConditionalRulesAndNamesAreASCIICaseInsensitiveAndEachConditionalRuleIsAsked) {
  EXPECT_EQ(Read("@LAYER A; @Layer B { x { color: red } }").order(), "A, B");
  for (const char * name : {"media", "supports", "container", "document", "-moz-document", "MEDIA", "Container", "-MOZ-Document"}) {
    SCOPED_TRACE(name);
    const std::string css = std::string("@") + name + " (x) { @layer s { x { color: red } } }";
    Read yes(css, 1);
    EXPECT_EQ(yes.order(), "s");
    EXPECT_EQ(yes.asked, 1);
    Read no(css, 0);
    EXPECT_EQ(no.order(), "");
    EXPECT_EQ(no.asked, 1);
    Read refused(css);
    EXPECT_EQ(refused.status, GTEXT_CSS_E_UNSUPPORTED);
  }
  /* a statement in a conditional rule the caller cannot evaluate is refused too, and a statement with an invalid name is not */
  Read statement("@media print { @layer s; }");
  EXPECT_EQ(statement.status, GTEXT_CSS_E_UNSUPPORTED);
  Read invalid("@media print { @layer unset; }");
  EXPECT_EQ(invalid.status, GTEXT_CSS_OK);
  Read second_invalid("@media print { @layer a, unset; }");
  EXPECT_EQ(second_invalid.status, GTEXT_CSS_OK) << "a statement with one invalid name is invalid whole: nothing to ask about";
}

TEST(CssLayerFixes, TheRuleDepthIsExactlyTheLimit) {
  const std::string css = "@layer a { @layer b { @layer c { x { color: red } } } }";
  EXPECT_EQ(Read(css, -1, 4).status, GTEXT_CSS_OK) << "the top and three blocks are four frames";
  Read over(css, -1, 3);
  EXPECT_EQ(over.status, GTEXT_CSS_E_LIMIT_DEPTH);
  EXPECT_GT(over.result.span.end, over.result.span.offset);
  EXPECT_EQ(Read(css, -1, 1).status, GTEXT_CSS_E_LIMIT_DEPTH);
  EXPECT_EQ(Read("x { color: red }", -1, 1).status, GTEXT_CSS_OK) << "the top alone";
}

/* ---- paths ------------------------------------------------------------------------------------------------------- */

TEST(CssLayerFixes, ADotInANameIsEscapedInThePathAndTheOneLayerIsFoundByIt) {
  Layers l;
  GTEXT_CSS_Layer_Id dotted = 0, nested = 0, backslash = 0;
  ASSERT_EQ(gtext_css_layer_declare_segment(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, "a.b", 3, &dotted, nullptr), GTEXT_CSS_OK);
  nested = l.declare("a.b");
  ASSERT_EQ(gtext_css_layer_declare_segment(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, "c\\d", 3, &backslash, nullptr), GTEXT_CSS_OK);
  EXPECT_NE(dotted, nested);
  char buf[64];
  gtext_css_layer_path(l.r, dotted, buf, sizeof buf);
  EXPECT_STREQ(buf, "a\\.b");
  gtext_css_layer_path(l.r, nested, buf, sizeof buf);
  EXPECT_STREQ(buf, "a.b");
  gtext_css_layer_path(l.r, backslash, buf, sizeof buf);
  EXPECT_STREQ(buf, "c\\\\d");
  /* what is written declares and finds what it names */
  EXPECT_EQ(l.declare("a\\.b"), dotted);
  EXPECT_EQ(l.declare("c\\\\d"), backslash);
  EXPECT_EQ(gtext_css_layer_find(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, "a\\.b", 4), dotted);
  EXPECT_EQ(gtext_css_layer_find(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, "a.b", 3), nested);
  EXPECT_EQ(gtext_css_layer_find(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, "c\\\\d", 4), backslash);
  EXPECT_EQ(gtext_css_layer_find(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, "c\\d", 3), backslash) << "a backslash before another character is itself";
  /* a nested layer with an escaped dot in a segment, and a long one that needs the heap */
  const GTEXT_CSS_Layer_Id deep = l.declare("x.y\\.z");
  gtext_css_layer_path(l.r, deep, buf, sizeof buf);
  EXPECT_STREQ(buf, "x.y\\.z");
  std::string longname = "p";
  for (int i = 0; i < 600; i++) longname += "\\.";
  const GTEXT_CSS_Layer_Id big = l.declare(longname);
  GTEXT_CSS_Layer_Info info;
  ASSERT_TRUE(gtext_css_layer_info(l.r, big, &info));
  EXPECT_EQ(info.segment_length, 601u);
  EXPECT_EQ(gtext_css_layer_find(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, longname.data(), longname.size()), big);
  /* the names from a sheet: an escape in an identifier is decoded by the tokenizer, and the path writes it back escaped */
  Read sheet("@layer a\\.b, c;");
  EXPECT_EQ(sheet.order(), "a\\.b, c");
}

TEST(CssLayerFixes, TheRankOfEveryOriginStartsAtZeroAndTheTopLayerHasNoParent) {
  Layers l;
  const GTEXT_CSS_Layer_Id u = l.declare("u", GTEXT_CSS_ORIGIN_USER);
  const GTEXT_CSS_Layer_Id u2 = l.declare("u2", GTEXT_CSS_ORIGIN_USER);
  const GTEXT_CSS_Layer_Id ua = l.declare("g", GTEXT_CSS_ORIGIN_USER_AGENT);
  const GTEXT_CSS_Layer_Id a = l.declare("a");
  const GTEXT_CSS_Layer_Id b = l.declare("b");
  l.frozen();
  size_t rank = 99;
  ASSERT_EQ(gtext_css_layer_rank(l.r, GTEXT_CSS_ORIGIN_AUTHOR, a, &rank), GTEXT_CSS_OK);
  EXPECT_EQ(rank, 0u);
  ASSERT_EQ(gtext_css_layer_rank(l.r, GTEXT_CSS_ORIGIN_AUTHOR, b, &rank), GTEXT_CSS_OK);
  EXPECT_EQ(rank, 1u);
  ASSERT_EQ(gtext_css_layer_rank(l.r, GTEXT_CSS_ORIGIN_USER, u, &rank), GTEXT_CSS_OK);
  EXPECT_EQ(rank, 0u);
  ASSERT_EQ(gtext_css_layer_rank(l.r, GTEXT_CSS_ORIGIN_USER, u2, &rank), GTEXT_CSS_OK);
  EXPECT_EQ(rank, 1u);
  ASSERT_EQ(gtext_css_layer_rank(l.r, GTEXT_CSS_ORIGIN_USER_AGENT, ua, &rank), GTEXT_CSS_OK);
  EXPECT_EQ(rank, 0u);
  ASSERT_EQ(gtext_css_layer_rank(l.r, GTEXT_CSS_ORIGIN_USER, GTEXT_CSS_LAYER_NONE, &rank), GTEXT_CSS_OK);
  EXPECT_EQ(rank, 2u);
  ASSERT_EQ(gtext_css_layer_rank(l.r, GTEXT_CSS_ORIGIN_USER_AGENT, GTEXT_CSS_LAYER_NONE, &rank), GTEXT_CSS_OK);
  EXPECT_EQ(rank, 1u);
  ASSERT_EQ(gtext_css_layer_rank(l.r, GTEXT_CSS_ORIGIN_AUTHOR, GTEXT_CSS_LAYER_NONE, &rank), GTEXT_CSS_OK);
  EXPECT_EQ(rank, 2u);
  GTEXT_CSS_Layer_Info info;
  ASSERT_TRUE(gtext_css_layer_info(l.r, a, &info));
  EXPECT_EQ(info.parent, GTEXT_CSS_LAYER_NONE);
  EXPECT_EQ(info.depth, 1u);
}

/* ---- presentational hints --------------------------------------------------------------------------------------- */

TEST(CssCascadeFixes, APresentationalHintIsAnOriginBetweenTheUserAndTheAuthor) {
  /* 6.5: "this author presentational hint origin is treated as an independent origin; however for the purpose of the revert keyword (but not for the revert-layer
     keyword) it is considered part of the author origin." */
  Set s;
  s.add("z-index", "1", K().user().order(0, 0));
  s.add("z-index", "2", K().hint().order(0, 1));
  s.add("z-index", "revert", K().order(0, 2));
  EXPECT_EQ(Out(s).value("z-index"), "1") << "revert takes the hint out with the author origin";
  Set t;
  t.add("z-index", "1", K().user().order(0, 0));
  t.add("z-index", "2", K().hint().order(0, 1));
  t.add("z-index", "revert-layer", K().order(0, 2));
  EXPECT_EQ(Out(t).value("z-index"), "2") << "revert-layer rolls back to the hint origin";
  Layers l;
  const GTEXT_CSS_Layer_Id a = l.declare("a");
  Set u(l.frozen());
  u.add("z-index", "2", K().hint().order(0, 1));
  u.add("z-index", "3", K().layer(a).order(0, 2));
  u.add("z-index", "revert-layer", K().layer(a).order(0, 3));
  EXPECT_EQ(Out(u).value("z-index"), "2");
  /* the hint beats a user declaration and a user-agent one, and an important one beats it */
  Set v;
  v.add("z-index", "1", K().user().order(0, 0));
  v.add("z-index", "2", K().hint().ctx(5).order(0, 1));
  EXPECT_EQ(Out(v).value("z-index"), "2");
  Set w;
  w.add("z-index", "1", K().user().important().order(0, 0));
  w.add("z-index", "2", K().hint().order(0, 1));
  EXPECT_EQ(Out(w).value("z-index"), "1");
  /* revert-layer from a hint skips the other hints of its context only */
  Set x;
  x.add("z-index", "1", K().user());
  x.add("z-index", "2", K().hint().ctx(1).order(0, 0));
  x.add("z-index", "3", K().hint().ctx(0).order(0, 1));
  x.add("z-index", "revert-layer", K().hint().ctx(0).order(0, 2));
  EXPECT_EQ(Out(x).value("z-index"), "2");
}

TEST(CssCascadeFixes, ALayerIsFoundOnlyUnderItsOwnParentWhateverTheTableHolds) {
  /* the same name under many parents lands in the registry's one table, and the probe chains of a table half full are long: a lookup that did not compare the
     parent would answer with another parent's layer */
  Layers l;
  const int n = 300;
  std::vector<GTEXT_CSS_Layer_Id> ids;
  for (int i = 0; i < n; i++) {
    const std::string path = "p" + std::to_string(i) + ".x";
    GTEXT_CSS_Layer_Id id = 0;
    ASSERT_EQ(gtext_css_layer_declare(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, path.data(), path.size(), &id, nullptr), GTEXT_CSS_OK);
    ids.push_back(id);
  }
  for (int i = 0; i < n; i++) {
    const std::string path = "p" + std::to_string(i) + ".x";
    EXPECT_EQ(gtext_css_layer_find(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, path.data(), path.size()), ids[(size_t)i]) << path;
    for (int j = 0; j < i; j++) {
      ASSERT_NE(ids[(size_t)i], ids[(size_t)j]) << path << " is the layer of another parent";
    }
  }
}

TEST(CssCascadeFixes, ACustomPropertyHoldingVarIsRefusedAndNothingAroundItIsAffected) {
  /* found by the cascade fuzzer: `--a: var(--m)` parses as PENDING, and a custom property carries VALID or WIDE until the substitution layer (C2c) exists. The call
     says GTEXT_CSS_E_INVALID, names the reason, and adds nothing; the candidates before and after it cascade as they would without it. */
  Set s;
  s.add("--a", "5", K().order(0, 0, 0));
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  const size_t before = gtext_css_candidates_count(s.s);
  GTEXT_CSS_Property_Outcome outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
  K k = K().order(0, 1, 0);
  const std::string name = "--a", value = "x var(--m)";
  GTEXT_CSS_Cascade_Result res{};
  EXPECT_EQ(gtext_css_candidates_add_text(s.s, &k.e, name.data(), name.size(), value.data(), value.size(), nullptr, &outcome, &res), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(outcome, GTEXT_CSS_PROP_RESULT_PENDING);
  ASSERT_NE(res.detail, nullptr);
  EXPECT_NE(std::string(res.detail).find("custom property"), std::string::npos);
  EXPECT_EQ(gtext_css_candidates_count(s.s), before);
  s.add("--b", "1", K().order(0, 2, 0));
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  EXPECT_EQ(Out(s).custom("--a"), "5");
  EXPECT_EQ(Out(s).custom("--b"), "1");
}

/* ---- what the browsers do (tools/oracle/cascade_probe_answers.txt: Chromium 156.0.8078.47 and Firefox 158.0.1 agree on every case) ------------------------ */

TEST(CssCascadeFixes, ImportantRevertLayerRollsBackPastEverythingBetweenItsNormalAndImportantLevelsAsTheBrowsersDo) {
  /* 7.3.5: "as if no rules were specified in the current cascade layer - or between its normal and important levels in the cascade". Both browsers compute
     margin-left 0px (no declared value) in cases A1 to A3 and B1 of the probe: the important declarations of later layers, the normal declarations of later
     layers and of the unlayered styles, and those of an outer context, all between the two levels, are out. */
  Layers l;
  const GTEXT_CSS_Layer_Id a = l.declare("a"), b = l.declare("b");
  l.frozen();
  Set a1(l.r);  // A1
  a1.add("margin-left", "2px !important", K().layer(b).order(0, 0, 0));
  a1.add("margin-left", "1px !important", K().layer(a).order(0, 1, 0));
  a1.add("margin-left", "revert-layer !important", K().layer(a).order(0, 1, 1));
  a1.add("margin-left", "3px", K().order(0, 2, 0));
  EXPECT_EQ(Out(a1).value("margin-left"), "-");
  Set a2(l.r);  // A2
  a2.add("margin-left", "2px !important", K().layer(b).order(0, 0, 0));
  a2.add("margin-left", "revert-layer !important", K().layer(a).order(0, 1, 0));
  EXPECT_EQ(Out(a2).value("margin-left"), "-") << "importance is not kept: the floor is the normal level";
  Set a3(l.r);  // A3
  a3.add("margin-left", "1px", K().layer(a).order(0, 0, 0));
  a3.add("margin-left", "revert-layer !important", K().layer(a).order(0, 0, 1));
  a3.add("margin-left", "3px", K().order(0, 1, 0));
  EXPECT_EQ(Out(a3).value("margin-left"), "-");
  Set b1;  // B1: the outer context's normal declaration
  b1.add("margin-left", "1px", K().ctx(0).order(0, 0, 0));
  Layers inner;
  const GTEXT_CSS_Layer_Id ia = inner.declare("a");
  ASSERT_EQ(gtext_css_candidates_use_layers(b1.s, 1, inner.frozen()), GTEXT_CSS_OK);
  b1.add("margin-left", "revert-layer !important", K().ctx(1).layer(ia).order(0, 1, 0));
  EXPECT_EQ(Out(b1).value("margin-left"), "-");
  Set b2;  // B2: a normal one rolls back within its layer, and the outer context wins anyway
  ASSERT_EQ(gtext_css_candidates_use_layers(b2.s, 1, inner.r), GTEXT_CSS_OK);
  b2.add("margin-left", "1px", K().ctx(0).order(0, 0, 0));
  b2.add("margin-left", "revert-layer", K().ctx(1).layer(ia).order(0, 1, 0));
  EXPECT_EQ(Out(b2).value("margin-left"), "1px");
}

TEST(CssCascadeFixes, APresentationalHintIsBelowAnAuthorRuleOfAnInnerContextAndRevertLayerRollsBackToIt) {
  Set d1;  // D1: <img width=200> with ::slotted(img) { width: 100px } from the shadow tree
  d1.add("width", "200px", K().hint().ctx(0).order(0, 0, 0));
  d1.add("width", "100px", K().ctx(1).order(0, 1, 0));
  EXPECT_EQ(Out(d1).value("width"), "100px");
  Set d2;  // D2: a rule of the light tree
  d2.add("width", "200px", K().hint().order(0, 0, 0));
  d2.add("width", "100px", K().order(0, 1, 0));
  EXPECT_EQ(Out(d2).value("width"), "100px");
  Set e1;  // E1: style="width: revert-layer"
  e1.add("width", "200px", K().hint().order(0, 0, 0));
  e1.add("width", "revert-layer", K().attached().order(0, 1, 0));
  EXPECT_EQ(Out(e1).value("width"), "200px");
}

TEST(CssCascadeFixes, RevertRuleOfAnImportantDeclarationTakesOutTheNormalOneOfItsRuleAsTheBrowsersDo) {
  Set g1;  // G1
  g1.add("margin-left", "1px", K().order(0, 0, 0));
  g1.add("margin-left", "5px", K().order(0, 1, 0));
  g1.add("margin-left", "revert-rule !important", K().order(0, 1, 1));
  EXPECT_EQ(Out(g1).value("margin-left"), "1px");
  Set g2;  // G2: the important declaration wins over the normal revert-rule, which is not looked at
  g2.add("margin-left", "1px", K().order(0, 0, 0));
  g2.add("margin-left", "5px !important", K().order(0, 1, 0));
  g2.add("margin-left", "revert-rule", K().order(0, 1, 1));
  EXPECT_EQ(Out(g2).value("margin-left"), "5px");
}

/* ---- the fields of the dump and the custom property order -------------------------------------------------------- */

TEST(CssCascadeFixes, TheDumpNamesImportanceHintAttachedAndAll) {
  Set s;
  s.add("color", "red !important", K().order(0, 0));
  s.add("width", "1px", K().hint().order(0, 1));
  s.add("height", "2px", K().attached().order(0, 2));
  s.add("all", "initial", K().ua().order(0, 3));
  Out o(s);
  const std::string d = o.dump();
  EXPECT_NE(d.find("color: red !important [author "), std::string::npos) << d;
  EXPECT_NE(d.find("width: 1px [author hint "), std::string::npos) << d;
  EXPECT_NE(d.find("height: 2px [author attached "), std::string::npos) << d;
  EXPECT_NE(d.find("margin-top: initial [user-agent all "), std::string::npos) << d;
  EXPECT_EQ(d.rfind("color: red [", 0), std::string::npos);
  EXPECT_EQ(d.find("width: 1px !important"), std::string::npos);
}

TEST(CssCascadeFixes, CustomPropertyNamesThatArePrefixesOfOneAnotherAreSortedAndFound) {
  Set s;
  s.add("--abc", "3", K().order(0, 0));
  s.add("--a", "1", K().order(0, 1));
  s.add("--b", "4", K().order(0, 2));
  s.add("--ab", "2", K().order(0, 3));
  s.add("width", "1px", K().order(1, 0));
  s.add("height", "1px", K().order(1, 1));
  s.add("color", "red", K().order(1, 2));
  s.add("top", "1px", K().order(1, 3));
  s.add("margin-left", "1px", K().order(1, 4));
  Out o(s);
  const char * want[] = {"--a", "--ab", "--abc", "--b"};
  ASSERT_EQ(gtext_css_cascaded_count(o.c), 9u);
  for (size_t i = 0; i < 4; i++) {
    GTEXT_CSS_Cascade_Entry e;
    ASSERT_TRUE(gtext_css_cascaded_at(o.c, 5 + i, &e, nullptr));
    EXPECT_EQ(std::string(e.custom_name.data, e.custom_name.length), want[i]) << i;
    EXPECT_EQ(o.custom(want[i]).empty(), false);
  }
  EXPECT_EQ(o.custom("--a"), "1");
  EXPECT_EQ(o.custom("--ab"), "2");
  EXPECT_EQ(o.custom("--abc"), "3");
  EXPECT_EQ(o.custom("--b"), "4");
  EXPECT_EQ(o.custom("--ac"), "-");
  EXPECT_EQ(o.custom("--"), "-");
  /* a registered property is never found as a custom one, and a custom one is never found below the registered winners */
  GTEXT_CSS_Cascade_Entry e;
  EXPECT_FALSE(gtext_css_cascaded_find(o.c, 0, "width", 5, &e));
  EXPECT_FALSE(gtext_css_cascaded_find(o.c, 0, "--width", 7, &e));
  EXPECT_FALSE(gtext_css_cascaded_find(o.c, 0, "--zzz", 5, &e));
  EXPECT_FALSE(gtext_css_cascaded_find(o.c, 0, "--", 2, &e));
}

TEST(CssCascadeFixes, AllTakesTheCssWideKeywordsOnlyAndATextCanBeEmpty) {
  Set s;
  GTEXT_CSS_Property_Value pv;
  ASSERT_EQ(gtext_css_parse_property_text(R(), "color", 5, "red", 3, nullptr, &pv), GTEXT_CSS_OK);
  pv.property = find(R(), "all");
  K k;
  EXPECT_EQ(gtext_css_candidates_add(s.s, &k.e, &pv, nullptr), GTEXT_CSS_E_INVALID);
  gtext_css_property_value_free(&pv);
  EXPECT_EQ(gtext_css_candidates_count(s.s), 0u);
  GTEXT_CSS_Property_Outcome outcome;
  EXPECT_EQ(gtext_css_candidates_add_text(s.s, &k.e, "color", 5, nullptr, 0, nullptr, &outcome, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(outcome, GTEXT_CSS_PROP_RESULT_INVALID);
  EXPECT_EQ(gtext_css_candidates_add_text(s.s, &k.e, "color", 5, nullptr, 3, nullptr, &outcome, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_candidates_add_text(s.s, &k.e, nullptr, 0, "red", 3, nullptr, &outcome, nullptr), GTEXT_CSS_E_INVALID);
}

TEST(CssCascadeFixes, DiagnosticsAreClampedToTheirLimitAndCountedPast) {
  GTEXT_CSS_Candidate_Options o = gtext_css_candidate_options_default();
  o.max_diagnostics = SIZE_MAX;
  Set s(nullptr, &o);
  const size_t limit = (size_t)1 << 20;
  K k;
  GTEXT_CSS_Property_Outcome outcome;
  for (size_t i = 0; i < limit + 5; i++) {
    ASSERT_EQ(gtext_css_candidates_add_text(s.s, &k.e, "nope", 4, "1", 1, nullptr, &outcome, nullptr), GTEXT_CSS_OK);
  }
  EXPECT_EQ(gtext_css_candidates_diagnostic_total(s.s), limit + 5);
  GTEXT_CSS_Cascade_Diagnostic d;
  EXPECT_TRUE(gtext_css_candidates_diagnostic(s.s, limit - 1, &d));
  EXPECT_FALSE(gtext_css_candidates_diagnostic(s.s, limit, &d)) << "kept up to the limit";
}

TEST(CssCascadeFixes, TheFreezeNumbersEachOriginFromZeroAndAFrozenRegistryIsReadByManyContexts) {
  /* a registry shared by every context is the simple case; the number of layers it counts is the origin's */
  Layers l;
  for (const char * n : {"a", "b", "c"}) l.declare(n, GTEXT_CSS_ORIGIN_USER);
  l.declare("only");
  l.frozen();
  EXPECT_EQ(gtext_css_layer_count(l.r, GTEXT_CSS_ORIGIN_USER), 3u);
  EXPECT_EQ(gtext_css_layer_count(l.r, GTEXT_CSS_ORIGIN_AUTHOR), 1u);
  EXPECT_EQ(gtext_css_layer_count(l.r, GTEXT_CSS_ORIGIN_USER_AGENT), 0u);
  EXPECT_EQ(gtext_css_layer_count(l.r, GTEXT_CSS_ORIGIN_ANIMATION), 0u);
}

} // namespace
