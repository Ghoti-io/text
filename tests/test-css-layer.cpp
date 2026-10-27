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
 * Cascade layers (css_layer.h): the order the specification gives (Cascade 5, 6.4.3, with its example), nesting, anonymous layers, origins, the
 * reserved names, the limits at the bound and one past, and the reading of a sheet's `@layer` statements and blocks and its `@import ... layer()`.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_cascade_test_support.h"
#include "css_test_support.h"

using namespace cas;

namespace {

/// The layers of the registry in the order of their ranks, as paths ("(unlayered)" last).
std::string order_of(const Layers & l, GTEXT_CSS_Origin origin = GTEXT_CSS_ORIGIN_AUTHOR) {
  EXPECT_TRUE(gtext_css_layer_is_frozen(l.r));
  const size_t n = gtext_css_layer_count(l.r, origin);
  std::vector<std::string> by_rank(n + 1);
  for (GTEXT_CSS_Layer_Id id = 1; id < 4096; id++) {
    GTEXT_CSS_Layer_Info info;
    if (!gtext_css_layer_info(l.r, id, &info) || info.origin != origin) {
      continue;
    }
    char buf[256];
    gtext_css_layer_path(l.r, id, buf, sizeof buf);
    EXPECT_LT(info.rank, n);
    by_rank[info.rank] = buf;
  }
  by_rank[n] = "(unlayered)";
  std::string out;
  for (const std::string & s : by_rank) {
    out += (out.empty() ? "" : ", ") + s;
  }
  return out;
}

TEST(CssLayer, TheOrderIsTheSpecificationsExample) {
  /* Cascade 5, 6.4.3: "Cascade layers are sorted by the order in which they first are declared, with nested layers grouped within their parent layer.
     Unlayered rules are sorted later than any layered rules within the same parent layer (if any)." The sheet of the example declares reset.type,
     framework (with framework.theme inside) and reset, and the answer it gives is "reset.type, reset (implicit sub-layer), framework.theme, framework
     (implicit sub-layer), (implicit outer layer)". */
  Layers l;
  l.declare("reset.type");
  l.declare("framework");
  l.declare("theme", GTEXT_CSS_ORIGIN_AUTHOR, l.declare("framework"));
  l.declare("reset");
  l.frozen();
  EXPECT_EQ(order_of(l), "reset.type, reset, framework.theme, framework, (unlayered)");
}

TEST(CssLayer, AFlatListIsInOrderOfFirstDeclaration) {
  Layers l;
  l.declare("c");
  l.declare("a");
  l.declare("b");
  l.declare("a");  // again: the same layer, in its first place
  l.frozen();
  EXPECT_EQ(order_of(l), "c, a, b, (unlayered)");
  size_t rank = 0;
  ASSERT_EQ(gtext_css_layer_rank(l.r, GTEXT_CSS_ORIGIN_AUTHOR, GTEXT_CSS_LAYER_NONE, &rank), GTEXT_CSS_OK);
  EXPECT_EQ(rank, 3u) << "the unlayered styles are after every layer";
}

TEST(CssLayer, DeclaringAPathIsIdempotentAndMakesItsParents) {
  Layers l;
  const GTEXT_CSS_Layer_Id abc = l.declare("a.b.c");
  EXPECT_EQ(l.declare("a.b.c"), abc);
  const GTEXT_CSS_Layer_Id ab = l.declare("a.b");
  GTEXT_CSS_Layer_Info info;
  ASSERT_TRUE(gtext_css_layer_info(l.r, abc, &info));
  EXPECT_EQ(info.parent, ab);
  EXPECT_EQ(info.depth, 3u);
  EXPECT_EQ(std::string(info.segment, info.segment_length), "c");
  EXPECT_EQ(gtext_css_layer_count(l.r, GTEXT_CSS_ORIGIN_AUTHOR), 3u);
  EXPECT_EQ(gtext_css_layer_find(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, "a.b.c", 5), abc);
  EXPECT_EQ(gtext_css_layer_find(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, "a.x", 3), GTEXT_CSS_LAYER_NONE);
  EXPECT_EQ(gtext_css_layer_find(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, "", 0), GTEXT_CSS_LAYER_NONE);
  char buf[8];
  EXPECT_EQ(gtext_css_layer_path(l.r, abc, buf, sizeof buf), 5u);
  EXPECT_STREQ(buf, "a.b.c");
  EXPECT_EQ(gtext_css_layer_path(l.r, abc, buf, 4), 5u) << "the length of the whole path, as snprintf";
  EXPECT_STREQ(buf, "a.b");
}

TEST(CssLayer, ANestedLayerDoesNotEscapeItsParent) {
  /* "A nested layer cannot 'escape' its parent layer to reference layers outside itself." */
  Layers l;
  const GTEXT_CSS_Layer_Id base = l.declare("base");
  const GTEXT_CSS_Layer_Id framework = l.declare("framework");
  const GTEXT_CSS_Layer_Id inner = l.declare("base", GTEXT_CSS_ORIGIN_AUTHOR, framework);
  EXPECT_NE(inner, base) << "framework.base is not the top-level base";
  l.frozen();
  EXPECT_EQ(order_of(l), "base, framework.base, framework, (unlayered)");
}

TEST(CssLayer, EveryAnonymousLayerIsUnique) {
  /* "Each occurrence of an anonymous layer declaration represents a unique cascade layer" */
  Layers l;
  GTEXT_CSS_Layer_Id a1 = 0, a2 = 0;
  ASSERT_EQ(gtext_css_layer_declare_anonymous(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, &a1, nullptr), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_layer_declare_anonymous(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, &a2, nullptr), GTEXT_CSS_OK);
  EXPECT_NE(a1, a2);
  /* "Within a single unnamed layer, child layers with the same name refer to the same cascade layer ... Whereas in separate unnamed layers, child layers
     with the same name refer to different cascade layers" */
  const GTEXT_CSS_Layer_Id f1 = l.declare("foo", GTEXT_CSS_ORIGIN_AUTHOR, a1);
  EXPECT_EQ(l.declare("foo", GTEXT_CSS_ORIGIN_AUTHOR, a1), f1);
  EXPECT_NE(l.declare("foo", GTEXT_CSS_ORIGIN_AUTHOR, a2), f1);
  GTEXT_CSS_Layer_Info info;
  ASSERT_TRUE(gtext_css_layer_info(l.r, a1, &info));
  EXPECT_TRUE(info.anonymous);
  EXPECT_EQ(info.segment, nullptr);
  char buf[64];
  gtext_css_layer_path(l.r, f1, buf, sizeof buf);
  EXPECT_STREQ(buf, "(anonymous).foo");
  /* an anonymous layer cannot be named from outside: no path finds it */
  EXPECT_EQ(gtext_css_layer_find(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, "(anonymous)", 11), GTEXT_CSS_LAYER_NONE);
}

TEST(CssLayer, TheOriginsHaveLayersOfTheirOwn) {
  /* "Cascade layers are scoped to their origin and context" */
  Layers l;
  const GTEXT_CSS_Layer_Id author_a = l.declare("a");
  const GTEXT_CSS_Layer_Id user_a = l.declare("a", GTEXT_CSS_ORIGIN_USER);
  const GTEXT_CSS_Layer_Id ua_b = l.declare("b", GTEXT_CSS_ORIGIN_USER_AGENT);
  EXPECT_NE(author_a, user_a);
  l.frozen();
  EXPECT_EQ(gtext_css_layer_count(l.r, GTEXT_CSS_ORIGIN_AUTHOR), 1u);
  EXPECT_EQ(gtext_css_layer_count(l.r, GTEXT_CSS_ORIGIN_USER), 1u);
  EXPECT_EQ(gtext_css_layer_count(l.r, GTEXT_CSS_ORIGIN_USER_AGENT), 1u);
  size_t rank = 0;
  EXPECT_EQ(gtext_css_layer_rank(l.r, GTEXT_CSS_ORIGIN_AUTHOR, ua_b, &rank), GTEXT_CSS_E_INVALID) << "a layer of another origin";
  int order = 0;
  EXPECT_EQ(gtext_css_layer_compare(l.r, GTEXT_CSS_ORIGIN_AUTHOR, author_a, GTEXT_CSS_LAYER_NONE, &order), GTEXT_CSS_OK);
  EXPECT_EQ(order, -1);
  EXPECT_EQ(gtext_css_layer_compare(l.r, GTEXT_CSS_ORIGIN_AUTHOR, GTEXT_CSS_LAYER_NONE, author_a, &order), GTEXT_CSS_OK);
  EXPECT_EQ(order, 1);
  EXPECT_EQ(gtext_css_layer_compare(l.r, GTEXT_CSS_ORIGIN_AUTHOR, author_a, author_a, &order), GTEXT_CSS_OK);
  EXPECT_EQ(order, 0);
  /* the animation and transition origins have none */
  GTEXT_CSS_Layer_Result res;
  GTEXT_CSS_Layer_Registry * fresh = nullptr;
  ASSERT_EQ(gtext_css_layer_registry_new(nullptr, &fresh), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_layer_declare(fresh, GTEXT_CSS_ORIGIN_ANIMATION, 0, "a", 1, nullptr, &res), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_layer_declare(fresh, GTEXT_CSS_ORIGIN_AUTHOR, author_a, "a", 1, nullptr, &res), GTEXT_CSS_E_INVALID) << "a parent that is not there";
  gtext_css_layer_registry_free(fresh);
}

TEST(CssLayer, TheCssWideKeywordsAndEmptyNamesAreRefused) {
  /* "The CSS-wide keywords are reserved for future use, and cause the rule to be invalid at parse time if used as an <ident> in the <layer-name>." */
  Layers l;
  GTEXT_CSS_Layer_Result res;
  for (const char * bad : {"initial", "inherit", "unset", "revert", "revert-layer", "REVERT", "a.unset", "unset.a"}) {
    EXPECT_EQ(gtext_css_layer_declare(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, bad, strlen(bad), nullptr, &res), GTEXT_CSS_E_INVALID) << bad;
    EXPECT_NE(res.detail, nullptr) << bad;
  }
  for (const char * bad : {"", ".", "a.", ".a", "a..b"}) {
    EXPECT_EQ(gtext_css_layer_declare(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, bad, strlen(bad), nullptr, &res), GTEXT_CSS_E_INVALID) << bad;
  }
  EXPECT_EQ(gtext_css_layer_declare(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, "initialize", 10, nullptr, &res), GTEXT_CSS_OK) << "a name that only begins with one";
  /* a segment taken as it is may hold a dot (an escaped one in the source) */
  GTEXT_CSS_Layer_Id id = 0;
  ASSERT_EQ(gtext_css_layer_declare_segment(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, "a.b", 3, &id, &res), GTEXT_CSS_OK);
  EXPECT_NE(id, l.declare("a.b"));
}

TEST(CssLayer, AFrozenRegistryTakesNoNewLayerAndStillFindsTheOldOnes) {
  Layers l;
  const GTEXT_CSS_Layer_Id a = l.declare("a");
  l.frozen();
  GTEXT_CSS_Layer_Result res;
  GTEXT_CSS_Layer_Id id = 0;
  EXPECT_EQ(gtext_css_layer_declare(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, "a", 1, &id, &res), GTEXT_CSS_OK);
  EXPECT_EQ(id, a);
  EXPECT_EQ(gtext_css_layer_declare(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, "b", 1, &id, &res), GTEXT_CSS_E_INVALID);
  EXPECT_NE(res.detail, nullptr);
  EXPECT_EQ(gtext_css_layer_declare_anonymous(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, &id, &res), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_layer_freeze(l.r), GTEXT_CSS_OK) << "freezing again is a no-op";
  /* an unfrozen registry has no ranks */
  Layers u;
  const GTEXT_CSS_Layer_Id ua = u.declare("a");
  size_t rank = 0;
  EXPECT_EQ(gtext_css_layer_rank(u.r, GTEXT_CSS_ORIGIN_AUTHOR, ua, &rank), GTEXT_CSS_E_INVALID);
  GTEXT_CSS_Candidates * set = nullptr;
  EXPECT_EQ(gtext_css_candidates_new(R(), u.r, nullptr, &set), GTEXT_CSS_E_INVALID) << "a candidate set needs a frozen registry";
}

TEST(CssLayer, TheLimitsAreExactAndEachItsOwnStatus) {
  GTEXT_CSS_Layer_Options o = gtext_css_layer_options_default();
  o.max_layers = 5;
  o.max_depth = 3;
  Layers l(&o);
  GTEXT_CSS_Layer_Result res;
  for (const char * name : {"a", "b", "c", "d", "e"}) {
    EXPECT_EQ(gtext_css_layer_declare(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, name, 1, nullptr, &res), GTEXT_CSS_OK) << name;
  }
  EXPECT_EQ(gtext_css_layer_declare(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, "f", 1, nullptr, &res), GTEXT_CSS_E_LIMIT_LAYERS);
  EXPECT_STREQ(res.name, "f");
  EXPECT_NE(res.detail, nullptr);
  EXPECT_EQ(gtext_css_layer_declare(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, "a", 1, nullptr, &res), GTEXT_CSS_OK) << "a layer that is there is not a new one";
  GTEXT_CSS_Layer_Id anon = 0;
  EXPECT_EQ(gtext_css_layer_declare_anonymous(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, &anon, &res), GTEXT_CSS_E_LIMIT_LAYERS) << "anonymous ones count";

  o.max_layers = 100;
  Layers d2(&o);
  EXPECT_EQ(gtext_css_layer_declare(d2.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, "a.b.c", 5, nullptr, &res), GTEXT_CSS_OK) << "three deep";
  EXPECT_EQ(gtext_css_layer_declare(d2.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, "a.b.c.d", 7, nullptr, &res), GTEXT_CSS_E_LIMIT_LAYER_DEPTH) << "four deep";
  EXPECT_NE(res.detail, nullptr);
  EXPECT_NE(gtext_css_layer_find(d2.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, "a.b.c", 5), GTEXT_CSS_LAYER_NONE) << "the layers made before the failure remain";
}

/* ---- reading a sheet ------------------------------------------------------------------------------------------ */

struct Collected {
  Layers layers;
  css_test::Parsed parsed;
  GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;
  GTEXT_CSS_Layer_Result result{};
  std::vector<std::pair<std::string, std::string>> callbacks;  // (at-rule name, layer path)
  bool conditional_answer = true;
  int asked = 0;

  explicit Collected(const std::string & css, bool with_filter = true, GTEXT_CSS_Layer_Id base = 0) {
    css_test::parse_into(parsed, gtext_css_parse_stylesheet, css);
    EXPECT_EQ(parsed.status, GTEXT_CSS_OK);
    GTEXT_CSS_Layer_Collect_Options o;
    std::memset(&o, 0, sizeof o);
    o.user = this;
    if (with_filter) {
      o.active = [](void * u, const GTEXT_CSS_Rule *) {
        Collected * self = static_cast<Collected *>(u);
        self->asked++;
        return self->conditional_answer;
      };
    }
    o.on_layer = [](void * u, const GTEXT_CSS_Rule * rule, GTEXT_CSS_Layer_Id layer) {
      Collected * self = static_cast<Collected *>(u);
      char buf[256];
      gtext_css_layer_path(self->layers.r, layer, buf, sizeof buf);
      self->callbacks.emplace_back(std::string(rule->name.data, rule->name.length), buf);
    };
    status = gtext_css_cascade_collect_layers(layers.r, GTEXT_CSS_ORIGIN_AUTHOR, base, parsed.tree, &o, &result);
  }
  std::string order() {
    layers.frozen();
    return order_of(layers);
  }
};

TEST(CssLayerCollect, StatementsAndBlocksDeclareInOrder) {
  /* the example of 6.4.3 read from its sheet */
  Collected c(
      "h1 { color: darkslateblue; }\n"
      "@layer reset.type { strong { font-weight: bold; } }\n"
      "@layer framework { .title { font-weight: 100; } @layer theme { h1, h2 { color: maroon; } } }\n"
      "@layer reset { [hidden] { display: none; } }\n");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.order(), "reset.type, reset, framework.theme, framework, (unlayered)");
  ASSERT_EQ(c.callbacks.size(), 4u);
  EXPECT_EQ(c.callbacks[0].second, "reset.type");
  EXPECT_EQ(c.callbacks[1].second, "framework");
  EXPECT_EQ(c.callbacks[2].second, "framework.theme");
  EXPECT_EQ(c.callbacks[3].second, "reset");
}

TEST(CssLayerCollect, AStatementDeclaresSeveralNamesAndFixesTheOrderBeforeTheBlocks) {
  /* "The @layer rule can also be used to define new layers without assigning any style rules, by providing only the layer name: @layer <layer-name>#;" */
  Collected c("@layer framework, override.deep;\n@layer override { a { color: red } }\n@layer framework.theme { a { color: red } }");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.order(), "framework.theme, framework, override.deep, override, (unlayered)");
  EXPECT_EQ(c.callbacks.size(), 2u) << "statements are not reported, blocks are";
}

TEST(CssLayerCollect, AnonymousBlocksAreEachALayerAndNestAsWritten) {
  Collected c("@layer { @layer foo { a {color: red} } } @layer { @layer foo { a {color: red} } } @layer { @layer foo {} @layer foo {} }");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_layer_count(c.layers.r, GTEXT_CSS_ORIGIN_AUTHOR), 6u) << "three anonymous layers, and foo in each: two foo's in the third are one";
  ASSERT_EQ(c.callbacks.size(), 7u);
  EXPECT_EQ(c.callbacks[0].second, "(anonymous)");
  EXPECT_EQ(c.callbacks[1].second, "(anonymous).foo");
}

TEST(CssLayerCollect, AnImportWithALayerDeclaresIt) {
  /* "using an @import rule with the layer keyword or layer() function, assigning the contents of the imported file into that layer" */
  Collected c(
      "@import url(headings.css) layer(default);\n"
      "@import url(links.css) layer(default);\n"
      "@import url(base.css) layer;\n"
      "@import url(plain.css);\n"
      "@import url(theme.css) layer(theme.dark) supports(display: grid);\n"
      "@layer default { audio[controls] { display: block; } }\n");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.order(), "default, (anonymous), theme.dark, theme, (unlayered)");
  ASSERT_EQ(c.callbacks.size(), 5u);
  EXPECT_EQ(c.callbacks[0].first, "import");
  EXPECT_EQ(c.callbacks[0].second, "default");
  EXPECT_EQ(c.callbacks[1].second, "default");
  EXPECT_EQ(c.callbacks[2].second, "(anonymous)");
  EXPECT_EQ(c.callbacks[3].second, "theme.dark");
  EXPECT_EQ(c.callbacks[4].first, "layer");
}

TEST(CssLayerCollect, ALayeredImportNestsInsideTheLayerItIsImportedInto) {
  Layers l;
  const GTEXT_CSS_Layer_Id lib = l.declare("lib");
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_stylesheet, "@layer base { a { color: red } } @import url(x.css) layer(extra);");
  GTEXT_CSS_Layer_Result res;
  ASSERT_EQ(gtext_css_cascade_collect_layers(l.r, GTEXT_CSS_ORIGIN_AUTHOR, lib, p.tree, nullptr, &res), GTEXT_CSS_OK);
  l.frozen();
  EXPECT_EQ(order_of(l), "lib.base, lib.extra, lib, (unlayered)");
}

TEST(CssLayerCollect, InvalidRulesDeclareNothing) {
  /* an invalid @layer rule is dropped as the standard says an invalid rule is: the CSS-wide keywords, a name with white space inside, a number */
  Collected c(
      "@layer a, unset, b;\n"
      "@layer unset { x { color: red } @layer inside {} }\n"
      "@layer a b { x { color: red } }\n"
      "@layer 123 { x { color: red } }\n"
      "@layer foo.123 { x { color: red } }\n"
      "@layer a, b { x { color: red } }\n"
      "@layer ;\n"
      "@layer ok;\n"
      "@import url(a.css) layer(unset);\n");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.order(), "ok, (unlayered)") << "only the one valid rule declared anything";
  EXPECT_EQ(c.callbacks.size(), 0u);
}

TEST(CssLayerCollect, AStatementMayHaveWhiteSpaceAroundItsNamesAndCommas) {
  Collected c("@layer  a , b.c ,\n d ;");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.order(), "a, b.c, b, d, (unlayered)");
}

TEST(CssLayerCollect, ConditionalRulesAreGatedByTheFilter) {
  /* "Layers that are defined inside of a conditional group rule do not contribute to the layer order unless the condition is true ..." */
  const std::string css =
      "@media (min-width: 30em) { @layer layout { .title { font-size: x-large; } } }\n"
      "@media (prefers-color-scheme: dark) { @layer theme { .title { color: white; } } }\n"
      "@layer theme, layout;\n";
  struct Case {
    const char * name;
    bool answer;
    const char * order;
  };
  /* the filter is given at construction, so build it by hand for each answer */
  for (const Case & k : {Case{"both conditions true", true, "layout, theme, (unlayered)"}, Case{"both false", false, "theme, layout, (unlayered)"}}) {
    Layers l;
    css_test::Parsed p;
    css_test::parse_into(p, gtext_css_parse_stylesheet, css);
    struct State {
      bool answer;
      int asked = 0;
    } state{k.answer};
    GTEXT_CSS_Layer_Collect_Options o;
    std::memset(&o, 0, sizeof o);
    o.user = &state;
    o.active = [](void * u, const GTEXT_CSS_Rule *) {
      State * s = static_cast<State *>(u);
      s->asked++;
      return s->answer;
    };
    GTEXT_CSS_Layer_Result res;
    ASSERT_EQ(gtext_css_cascade_collect_layers(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, p.tree, &o, &res), GTEXT_CSS_OK) << k.name;
    EXPECT_EQ(state.asked, 2) << k.name;
    l.frozen();
    EXPECT_EQ(order_of(l), k.order) << k.name;
  }
}

TEST(CssLayerCollect, WithoutAFilterAConditionalRuleThatHoldsALayerIsRefusedAndOneThatDoesNotIsSkipped) {
  Collected refused("@layer a;\n@media print { @layer b { x { color: red } } }", /*with_filter=*/false);
  EXPECT_EQ(refused.status, GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_STREQ(refused.result.name, "active_rule");
  EXPECT_GT(refused.result.span.end, refused.result.span.offset) << "the rule that needed an answer";
  EXPECT_NE(refused.result.detail, nullptr);
  Collected skipped("@layer a;\n@media print { x { color: red } }\n@supports (display: grid) { y { color: red } }", false);
  EXPECT_EQ(skipped.status, GTEXT_CSS_OK);
  Collected import("@media print { @import url(x.css) layer(z); }", false);
  EXPECT_EQ(import.status, GTEXT_CSS_E_UNSUPPORTED) << "an import under a condition";
  /* @scope and @starting-style are not conditions the caller evaluates: their layers count */
  Collected scoped("@scope (.a) { @layer s { x { color: red } } } @starting-style { @layer t { x { color: red } } }", false);
  ASSERT_EQ(scoped.status, GTEXT_CSS_OK);
  EXPECT_EQ(scoped.order(), "s, t, (unlayered)");
}

TEST(CssLayerCollect, LayersInsideStyleRulesAreFound) {
  /* CSS Nesting: a nested group rule inside a style rule */
  Collected c(".a { color: red; @layer n { color: blue; } @media print { @layer m { color: green; } } }");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.order(), "n, m, (unlayered)");
}

TEST(CssLayerCollect, DepthIsBoundedWithoutRecursion) {
  std::string css;
  const size_t depth = 20000;
  for (size_t i = 0; i < depth; i++) {
    css += "@layer a{";
  }
  css += "x{color:red}";
  for (size_t i = 0; i < depth; i++) {
    css += "}";
  }
  css_test::Parsed p;
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  po.max_depth = 100000;
  css_test::parse_into(p, gtext_css_parse_stylesheet, css, &po);
  if (p.status != GTEXT_CSS_OK) {
    GTEST_SKIP() << "the parser's own depth limit is lower than this shape";
  }
  Layers l;
  GTEXT_CSS_Layer_Result res;
  GTEXT_CSS_Layer_Collect_Options o;
  std::memset(&o, 0, sizeof o);
  o.max_rule_depth = 100;
  EXPECT_EQ(gtext_css_cascade_collect_layers(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, p.tree, &o, &res), GTEXT_CSS_E_LIMIT_DEPTH);
  EXPECT_NE(res.detail, nullptr);
  o.max_rule_depth = 0;
  GTEXT_CSS_Layer_Options lo = gtext_css_layer_options_default();
  lo.max_depth = 50;
  Layers l3(&lo);
  EXPECT_EQ(gtext_css_cascade_collect_layers(l3.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, p.tree, &o, &res), GTEXT_CSS_E_LIMIT_LAYER_DEPTH) << "same-named blocks nest as one chain";
  EXPECT_GT(res.span.end, res.span.offset);
}

TEST(CssLayerCollect, TheLayerRuleVectorsOfWebPlatformTests) {
  /* web-platform-tests css/css-cascade/parsing/layer.html at the commit pinned in tools/css/WPT_COMMIT: test_valid_rule() and test_invalid_rule() of @layer.
     A valid rule declares a layer (a block, also an anonymous one, is reported); an invalid one declares nothing. */
  struct Row {
    const char * rule;
    bool valid;
  };
  const Row rows[] = {
      {"@layer A;", true}, {"@layer A, B, C;", true}, {"@layer A.A;", true}, {"@layer A, B.C.D, C;", true}, {"@layer;", false}, {"@layer A . A;", false},
      {"@layer {\n}", true}, {"@layer A {\n}", true}, {"@layer A.B {\n}", true}, {"@layer A . B {\n}", false}, {"@layer A, B, C {\n}", false},
  };
  for (const Row & r : rows) {
    SCOPED_TRACE(r.rule);
    Collected c(r.rule);
    ASSERT_EQ(c.status, GTEXT_CSS_OK);
    const size_t declared = gtext_css_layer_count(c.layers.r, GTEXT_CSS_ORIGIN_AUTHOR);
    EXPECT_EQ(declared > 0, r.valid) << declared << " layers";
  }
}

TEST(CssLayerCollect, ArgumentsAreChecked) {
  Layers l;
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_stylesheet, "@layer a;");
  GTEXT_CSS_Layer_Result res;
  EXPECT_EQ(gtext_css_cascade_collect_layers(nullptr, GTEXT_CSS_ORIGIN_AUTHOR, 0, p.tree, nullptr, &res), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_cascade_collect_layers(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, nullptr, nullptr, &res), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_cascade_collect_layers(l.r, GTEXT_CSS_ORIGIN_ANIMATION, 0, p.tree, nullptr, &res), GTEXT_CSS_E_INVALID);
  l.frozen();
  EXPECT_EQ(gtext_css_cascade_collect_layers(l.r, GTEXT_CSS_ORIGIN_AUTHOR, 0, p.tree, nullptr, &res), GTEXT_CSS_E_INVALID) << "frozen";
}

} // namespace
