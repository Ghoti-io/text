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
 * The rule object and the rule list (css_cssom_rule.h): the kinds and what CSSOM says of each, the rules a sheet's parse makes and the ones it drops, the nesting (nested
 * declarations, grouping rules inside style rules, keyframes and margin rules), the stable identity and the parent chain, the detach of a rule with everything under it, the
 * order-statistics list against a model, the lazy declaration block and its hook into the rule's and the sheet's version, and the source positions.
 */

#include <algorithm>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cssom_test_support.h"

using namespace cot;

namespace {

/* ---- the kinds ---------------------------------------------------------------------------------------------------------------------------------- */

// The kinds table has a row for each of the 27 kinds of the C4 enumeration and for the style rule and the nested-declarations rule, in the order of the enumeration, and a
// kind has the name of its at-rule kind.
TEST(CssomRuleKinds, TheTableHasTwentyNineRowsInTheOrderOfTheEnumeration) {
  ASSERT_EQ(static_cast<int>(GTEXT_CSSOM_RULE_KIND_COUNT), 29);
  for (int k = 0; k < GTEXT_CSS_ATRULE_KIND_COUNT; k++) {
    EXPECT_STREQ(gtext_cssom_rule_kind_name(static_cast<GTEXT_CSSOM_Rule_Kind>(k)), gtext_css_atrule_kind_name(static_cast<GTEXT_CSS_Atrule_Kind>(k))) << k;
  }
  EXPECT_STREQ(gtext_cssom_rule_kind_name(GTEXT_CSSOM_RULE_STYLE), "style");
  EXPECT_STREQ(gtext_cssom_rule_kind_name(GTEXT_CSSOM_RULE_NESTED_DECLARATIONS), "nested-declarations");
  EXPECT_STREQ(gtext_cssom_rule_kind_name(GTEXT_CSSOM_RULE_KIND_COUNT), "unknown");
  for (int k = 0; k < GTEXT_CSSOM_RULE_KIND_COUNT; k++) {
    const css_kind_info * info = css_kind(static_cast<GTEXT_CSSOM_Rule_Kind>(k));
    EXPECT_STREQ(info->name, gtext_cssom_rule_kind_name(static_cast<GTEXT_CSSOM_Rule_Kind>(k)));
  }
}

// CSSOM, the legacy `type` constants: "STYLE_RULE 1, CHARSET_RULE 2 ... IMPORT_RULE 3, MEDIA_RULE 4, FONT_FACE_RULE 5, PAGE_RULE 6, KEYFRAMES_RULE 7, KEYFRAME_RULE 8, MARGIN_RULE 9,
// NAMESPACE_RULE 10, COUNTER_STYLE_RULE 11, SUPPORTS_RULE 12, FONT_FEATURE_VALUES_RULE 14"; every newer rule has 0.
TEST(CssomRuleKinds, TheLegacyTypeIsTheConstantCssomListsAndZeroForTheNewerRules) {
  Doc doc;
  Sheet s(doc,
      "@import url(a.css); @namespace svg url(http://www.w3.org/2000/svg); a{} @media print{} @font-face{font-family:x;src:url(a)} @page{} @keyframes k{from{} to{}} @counter-style c{system:cyclic;symbols:a}"
      " @supports (a:b){} @font-feature-values F{} @layer l{} @layer m; @container (width>1px){} @scope (a){} @starting-style{} @property --p{syntax:'*';inherits:false}");
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  struct Want {
    const char * kind;
    unsigned type;
    const char * interface_name;   /* not `interface`: windows.h defines it as struct */
  };
  const Want want[] = {{"import", 3, "CSSImportRule"}, {"namespace", 10, "CSSNamespaceRule"}, {"style", 1, "CSSStyleRule"}, {"media", 4, "CSSMediaRule"},
      {"font-face", 5, "CSSFontFaceRule"}, {"page", 6, "CSSPageRule"}, {"keyframes", 7, "CSSKeyframesRule"}, {"counter-style", 11, "CSSCounterStyleRule"},
      {"supports", 12, "CSSSupportsRule"}, {"font-feature-values", 14, "CSSFontFeatureValuesRule"}, {"layer", 0, "CSSLayerBlockRule"}, {"layer", 0, "CSSLayerStatementRule"},
      {"container", 0, "CSSContainerRule"}, {"scope", 0, "CSSScopeRule"}, {"starting-style", 0, "CSSStartingStyleRule"}, {"property", 0, "CSSPropertyRule"}};
  ASSERT_EQ(gtext_cssom_rule_list_length(s.rules()), sizeof want / sizeof want[0]);
  for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
    Rule r = at(s.rules(), i);
    EXPECT_STREQ(gtext_cssom_rule_kind_name(gtext_cssom_rule_kind(r.r)), want[i].kind) << i;
    EXPECT_EQ(gtext_cssom_rule_type(r.r), want[i].type) << want[i].kind;
    EXPECT_STREQ(gtext_cssom_rule_interface_name(r.r), want[i].interface_name) << want[i].kind;
  }
}

/* ---- what a parse makes ------------------------------------------------------------------------------------------------------------------------- */

// CSSOM "parse a CSS stylesheet": the rules that are valid where they are, in order, and the rules inside a rule in its `cssRules`.
TEST(CssomRuleParse, StyleRulesGroupingRulesAndTheirChildren) {
  Doc doc;
  Sheet s(doc, "a{color:red} @media print{b{color:blue} @supports (a:b){c{}}} d{} @font-face{font-family:x;src:url(a)}");
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  EXPECT_EQ(shape(s), "style,media[style,supports[style]],style,font-face");
  EXPECT_EQ(gtext_cssom_sheet_rule_total(s.s), 7u);
  EXPECT_EQ(gtext_cssom_sheet_diagnostic_total(s.s), 0u);
}

// CSS Nesting: the declarations of a style rule's block that follow a nested rule are a nested-declarations rule in its place; a style rule's `style` holds the ones before.
TEST(CssomRuleParse, NestedDeclarationsFollowANestedRule) {
  Doc doc;
  Sheet s(doc, "a{ color:red; & b{ } color:blue; width:1px } ");
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  EXPECT_EQ(shape(s), "style[style,nested-declarations]");
  Rule a = at(s.rules(), 0);
  Decl style = style_of(a.r);
  EXPECT_EQ(text_of(style.d), "color: red;") << "the leading declarations only";
  Rule nd = at(gtext_cssom_rule_css_rules(a.r), 1);
  Decl nds = style_of(nd.r);
  EXPECT_EQ(text_of(nds.d), "color: blue; width: 1px;");
}

// CSS Nesting: a grouping rule inside a style rule holds nested-declarations rules for its declarations, and its style rules are nested (relative); the same grouping rule at
// the top level holds no declarations (they are not valid there) and its style rules are plain.
TEST(CssomRuleParse, ADeclarationRunInAGroupingRuleIsARuleOnlyInsideAStyleRule) {
  Doc doc;
  Sheet s(doc, "a{ @media print{ color:red; & c{} width:1px } } @media print{ color:red; b{} }");
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  EXPECT_EQ(shape(s), "style[media[nested-declarations,style,nested-declarations]],media[style]");
  Rule a = at(s.rules(), 0);
  Rule media = at(gtext_cssom_rule_css_rules(a.r), 0);
  Rule nd = at(gtext_cssom_rule_css_rules(media.r), 0);
  Decl st = style_of(nd.r);
  EXPECT_EQ(text_of(st.d), "color: red;");
  Rule inner = at(gtext_cssom_rule_css_rules(media.r), 1);
  EXPECT_NE(dump(inner.r).find(" nested "), std::string::npos);
  Rule top_media = at(s.rules(), 1);
  Rule top_style = at(gtext_cssom_rule_css_rules(top_media.r), 0);
  EXPECT_NE(dump(top_style.r).find(" plain "), std::string::npos);
}

// CSS Animations 1: `@keyframes` holds keyframe rules; a block whose selector is not from, to or a percentage is not a rule. CSS Paged Media 3: `@page` holds margin rules.
TEST(CssomRuleParse, KeyframesHoldKeyframesAndPageHoldsMarginRules) {
  Doc doc;
  Sheet s(doc, "@keyframes k{from{a:b} 50%{} bogus{} 10%, 20%{} to{}} @page :first{margin:1px; @top-left{content:\"x\"} @bottom-center{}}");
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  EXPECT_EQ(shape(s), "keyframes[keyframe,keyframe,keyframe,keyframe],page[page-margin,page-margin]");
  bool seen = false;
  for (size_t i = 0; i < gtext_cssom_sheet_diagnostic_total(s.s); i++) {
    GTEXT_CSSOM_Parse_Diagnostic d;
    ASSERT_TRUE(gtext_cssom_sheet_diagnostic(s.s, i, &d));
    seen = seen || d.kind == GTEXT_CSSOM_PARSE_INVALID_KEYFRAME;
  }
  EXPECT_TRUE(seen);
  Rule kf = at(s.rules(), 0);
  Rule frame = at(gtext_cssom_rule_css_rules(kf.r), 0);
  EXPECT_EQ(gtext_cssom_rule_type(frame.r), 8u);
  Rule page = at(s.rules(), 1);
  Rule margin = at(gtext_cssom_rule_css_rules(page.r), 0);
  EXPECT_EQ(gtext_cssom_rule_type(margin.r), 9u);
}

// `@layer` has two forms of one kind; the statement has no `cssRules`.
TEST(CssomRuleParse, ALayerBlockHasRulesAndALayerStatementHasNone) {
  Doc doc;
  Sheet s(doc, "@layer a, b; @layer c{x{}}");
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  EXPECT_EQ(shape(s), "layer,layer[style]");
  Rule statement = at(s.rules(), 0);
  Rule block = at(s.rules(), 1);
  EXPECT_FALSE(gtext_cssom_rule_has_block(statement.r));
  EXPECT_TRUE(gtext_cssom_rule_has_block(block.r));
  EXPECT_EQ(gtext_cssom_rule_css_rules(statement.r), nullptr);
  EXPECT_NE(gtext_cssom_rule_css_rules(block.r), nullptr);
  Rule a = at(s.rules(), 0);
  EXPECT_EQ(gtext_cssom_rule_css_rules(a.r), nullptr);
}

// The rules no browser exposes are rules of the list, generically: `@when`, `@else`, `@custom-media` and the preserved ones.
TEST(CssomRuleParse, TheKindsNoBrowserExposesAreGenericRules) {
  Doc doc;
  Sheet s(doc, "@custom-media --narrow (max-width: 30em); @function --f(){} @when media(width>1px){a{}} @else{b{}}");
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  EXPECT_EQ(shape(s), "custom-media,preserved,when[style],else[style]");
  Rule r = at(s.rules(), 0);
  EXPECT_STREQ(gtext_cssom_rule_interface_name(r.r), "CSSRule");
  EXPECT_EQ(gtext_cssom_rule_type(r.r), 0u);
}

// What is not a rule: `@charset`, an unknown at-rule, a rule invalid where it is, and everything inside a dropped rule. Each is a diagnostic with a kind and a span.
TEST(CssomRuleParse, WhatIsNotARuleIsDroppedWithADiagnostic) {
  Doc doc;
  Sheet s(doc,
      "@charset \"utf-8\";\n"          // 1
      "@bogus x;\n"                    // 2
      "a >{color:red}\n"               // 3
      "@media (min-width:1px){b{} c >{} }\n"   // 4: the media stays, its second rule goes
      "d{}\n"                          // 5
      "@import url(x.css);\n"          // 6: after a style rule
      "@namespace n url(urn:x);\n"     // 7: after a style rule
      "@media;\n");                    // 8: no block
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  EXPECT_EQ(shape(s), "media[style],style");
  std::string seen;
  for (size_t i = 0; i < gtext_cssom_sheet_diagnostic_total(s.s); i++) {
    GTEXT_CSSOM_Parse_Diagnostic d;
    ASSERT_TRUE(gtext_cssom_sheet_diagnostic(s.s, i, &d));
    EXPECT_NE(std::string(d.reference).size(), 0u);
    seen += std::to_string(d.kind) + "@" + std::to_string(d.span.line) + " ";
  }
  // charset 0, unknown at-rule 1, invalid at-rule 2, invalid selector 3, invalid keyframe 4, misplaced import 5, misplaced namespace 6
  EXPECT_EQ(seen, "0@1 1@2 3@3 3@4 5@6 6@7 2@8 ");
  EXPECT_EQ(gtext_cssom_sheet_diagnostic_total(s.s), 7u);
}

// CSS Cascade 4: "@import ... must precede all other valid at-rules and style rules in a style sheet (ignoring @charset and @layer statements)"; CSS Namespaces 3: the
// @namespace rules follow @charset and @import and precede the rest. An invalid rule is no rule, so it does not end the place of the @import rules.
TEST(CssomRuleParse, TheOrderOfImportsAndNamespacesAtTheTop) {
  Doc doc;
  Sheet ok(doc, "@charset \"utf-8\"; @layer a; @import url(a.css); @bogus; @import url(b.css); @namespace url(urn:d); @namespace n url(urn:n); n|x{} @layer b{}");
  ASSERT_EQ(ok.status, GTEXT_CSS_OK);
  EXPECT_EQ(shape(ok), "layer,import,import,namespace,namespace,style,layer") << "a layer statement and an unknown at-rule close nothing; the second import is in its place";
  Sheet late(doc, "@layer b{} @import url(a.css); @namespace n url(urn:n); x{} @namespace m url(urn:m);");
  ASSERT_EQ(late.status, GTEXT_CSS_OK);
  EXPECT_EQ(shape(late), "layer,style") << "a layer block closes the place of imports and namespaces";
  Sheet ns(doc, "@import url(a.css); @namespace n url(urn:n); @import url(b.css); n|x{} m|y{} *|z{} |w{}");
  ASSERT_EQ(ns.status, GTEXT_CSS_OK);
  EXPECT_EQ(shape(ns), "import,namespace,style,style,style") << "an @import after an @namespace is misplaced; a prefix that was not declared makes its rule invalid";
}

// Selectors 4: a selector that is not valid makes its rule invalid, and with it everything nested in it.
TEST(CssomRuleParse, AnInvalidSelectorTakesTheRuleAndWhatIsInItAway) {
  Doc doc;
  Sheet s(doc, "a >{ b{} } c{ d{} e::bogus{} & f{} } g{}");
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  EXPECT_EQ(shape(s), "style[style,style],style");
}

/* ---- identity, parents, detach ---------------------------------------------------------------------------------------------------------------- */

// Identity is stable: the same rule from the same place, whatever happens beside it; `parentRule` and `parentStyleSheet` follow the lists.
TEST(CssomRuleIdentity, TheSameObjectEveryTimeAndTheParentChain) {
  Doc doc;
  Sheet s(doc, "a{} @media print{ b{} @supports (a:b){ c{} } } d{}");
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  Rule m1 = at(s.rules(), 1);
  Rule m2 = at(s.rules(), 1);
  EXPECT_EQ(m1.r, m2.r) << "cssRules[1] === cssRules[1]";
  Rule b = at(gtext_cssom_rule_css_rules(m1.r), 0);
  Rule sup = at(gtext_cssom_rule_css_rules(m1.r), 1);
  Rule c = at(gtext_cssom_rule_css_rules(sup.r), 0);
  Rule pc(gtext_cssom_rule_parent_rule(c.r));
  EXPECT_EQ(pc.r, sup.r);
  Rule pb(gtext_cssom_rule_parent_rule(b.r));
  EXPECT_EQ(pb.r, m1.r);
  Rule top_parent(gtext_cssom_rule_parent_rule(m1.r));
  EXPECT_EQ(top_parent.r, nullptr) << "a top-level rule has no parent rule";
  GTEXT_CSSOM_Sheet * ps = gtext_cssom_rule_parent_sheet(c.r);
  EXPECT_EQ(ps, s.s) << "parentStyleSheet is derived from the chain";
  gtext_cssom_sheet_release(ps);
  EXPECT_FALSE(gtext_cssom_rule_detached(c.r));
  size_t index = 99;
  EXPECT_TRUE(gtext_cssom_rule_list_index_of(gtext_cssom_rule_css_rules(sup.r), c.r, &index));
  EXPECT_EQ(index, 0u);
  EXPECT_TRUE(gtext_cssom_rule_list_index_of(s.rules(), m1.r, &index));
  EXPECT_EQ(index, 1u);
  EXPECT_FALSE(gtext_cssom_rule_list_index_of(s.rules(), c.r, &index)) << "not in that list";
  EXPECT_EQ(gtext_cssom_rule_list_at(s.rules(), 3), nullptr);
}

// CSSOM "remove a CSS rule": "Set old rule's parent CSS rule and parent CSS style sheet to null." Detaching is one pointer, and every rule under the removed one reads
// parent_sheet NULL at once; the removed rules are still valid objects with the same ids.
TEST(CssomRuleIdentity, ADetachedRuleAndEverythingUnderItReadNoSheetAndStayReadable) {
  Doc doc;
  Sheet s(doc, "a{} @media print{ b{ @media screen{ c{} } } } d{}");
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  Rule media = at(s.rules(), 1);
  Rule b = at(gtext_cssom_rule_css_rules(media.r), 0);
  Rule inner = at(gtext_cssom_rule_css_rules(b.r), 0);
  Rule c = at(gtext_cssom_rule_css_rules(inner.r), 0);
  const uint64_t media_id = gtext_cssom_rule_id(media.r), c_id = gtext_cssom_rule_id(c.r);
  const std::string before = dump(media.r);
  css_rule_list_remove(&s.s->rules, media.r);   // the list's count is the caller's: held here
  css_obj_release_internal(&media.r->obj);
  EXPECT_EQ(shape(s), "style,style");
  EXPECT_EQ(s.s->obj.external, 1u) << "the held rules of the removed subtree keep no hold on the sheet they left";
  EXPECT_TRUE(gtext_cssom_rule_detached(media.r));
  EXPECT_TRUE(gtext_cssom_rule_detached(b.r));
  EXPECT_TRUE(gtext_cssom_rule_detached(c.r));
  EXPECT_EQ(gtext_cssom_rule_parent_sheet(c.r), nullptr);
  Rule cp(gtext_cssom_rule_parent_rule(c.r));
  EXPECT_EQ(cp.r, inner.r) << "the chain inside the removed subtree is intact";
  Rule mp(gtext_cssom_rule_parent_rule(media.r));
  EXPECT_EQ(mp.r, nullptr);
  EXPECT_EQ(gtext_cssom_rule_id(media.r), media_id);
  EXPECT_EQ(gtext_cssom_rule_id(c.r), c_id);
  EXPECT_EQ(dump(media.r), before) << "nothing about the removed rules changed";
  EXPECT_EQ(shape(gtext_cssom_rule_css_rules(media.r)), "style[media[style]]");
}

// "a held child keeps its sheet alive": a rule held after every other reference to its sheet is gone still reaches the sheet, and the whole tree is freed when it goes.
TEST(CssomRuleIdentity, AHeldChildKeepsItsSheetAndItsRulesAlive) {
  Doc doc;
  GTEXT_CSSOM_Rule * kept = nullptr;
  {
    Sheet s(doc, "a{} @media print{ b{ } c{} } d{}");
    ASSERT_EQ(s.status, GTEXT_CSS_OK);
    Rule media = at(s.rules(), 1);
    kept = gtext_cssom_rule_list_at(gtext_cssom_rule_css_rules(media.r), 1);
    ASSERT_NE(kept, nullptr);
  }
  GTEXT_CSSOM_Counters c;
  ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c), GTEXT_CSS_OK);
  EXPECT_GT(c.live_objects, 5u) << "the sheet and its rules are alive";
  GTEXT_CSSOM_Sheet * sheet = gtext_cssom_rule_parent_sheet(kept);
  ASSERT_NE(sheet, nullptr);
  EXPECT_EQ(gtext_cssom_sheet_rules(sheet, nullptr), GTEXT_CSS_E_INVALID);
  gtext_cssom_sheet_release(sheet);
  gtext_cssom_rule_release(kept);
  ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c), GTEXT_CSS_OK);
  EXPECT_EQ(c.live_objects, 0u);
  EXPECT_EQ(c.live_bytes, 0u);
  EXPECT_EQ(c.tree_bytes, 0u);
}

/* ---- the list ------------------------------------------------------------------------------------------------------------------------------- */

struct ListModel {
  std::vector<css_rule *> order;
};

/// The list against the model: length, at, index_of, a walk with next, and the sizes of the nodes.
void check_list(GTEXT_CSSOM_Rule_List * list, const std::vector<css_rule *> & model) {
  ASSERT_EQ(list->count, model.size());
  size_t i = 0;
  for (css_rule * r = css_rule_list_first(list); r; r = css_rule_list_next(r), i++) {
    ASSERT_LT(i, model.size());
    ASSERT_EQ(r, model[i]) << i;
    ASSERT_EQ(css_rule_list_index_of(r), i);
  }
  ASSERT_EQ(i, model.size());
  for (size_t k = 0; k < model.size(); k += 1 + model.size() / 50) {
    ASSERT_EQ(css_rule_list_at(list, k), model[k]);
  }
  ASSERT_EQ(css_rule_list_at(list, model.size()), nullptr);
}

// The order-statistics treap against a vector: random insertions and removals, then the shape of it.
TEST(CssomRuleList, ARandomHistoryOfInsertionsAndRemovalsAgainstAModel) {
  Doc doc;
  Sheet s(doc, "");
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  std::mt19937 rng(12345);
  std::vector<css_rule *> model;
  css_source * none = nullptr;
  for (int step = 0; step < 4000; step++) {
    const bool insert = model.empty() || rng() % 3 != 0;
    if (insert) {
      css_rule * r = nullptr;
      ASSERT_EQ(css_rule_create(doc.core(), GTEXT_CSSOM_RULE_PRESERVED, none, nullptr, nullptr, 0, false, &r), GTEXT_CSS_OK);
      const size_t index = rng() % (model.size() + 1);
      css_rule_list_insert(&s.s->rules, index, r);
      model.insert(model.begin() + static_cast<long>(index), r);
    }
    else {
      const size_t index = rng() % model.size();
      css_rule * r = model[index];
      css_rule_list_remove(&s.s->rules, r);
      css_obj_release_internal(&r->obj);
      model.erase(model.begin() + static_cast<long>(index));
    }
    if (step % 250 == 0) {
      check_list(&s.s->rules, model);
    }
  }
  check_list(&s.s->rules, model);
  while (!model.empty()) {
    css_rule * r = model.back();
    model.pop_back();
    css_rule_list_remove(&s.s->rules, r);
    css_obj_release_internal(&r->obj);
  }
  EXPECT_EQ(s.s->rules.count, 0u);
  EXPECT_EQ(s.s->rules.root, nullptr);
}

// The shape follows the ids and nothing else: two lists built by the same calls are the same tree (built side by side, so they live at different addresses), and it is
// balanced (depth within a small multiple of log2 n).
TEST(CssomRuleList, TheShapeIsDeterministicAndBalanced) {
  Doc doc_a, doc_b;
  Sheet a(doc_a, ""), b(doc_b, "");
  ASSERT_EQ(a.status, GTEXT_CSS_OK);
  Sheet * sheets[2] = {&a, &b};
  Doc * docs[2] = {&doc_a, &doc_b};
  std::mt19937 rng(777);
  for (int i = 0; i < 20000; i++) {
    const size_t pick = i % 3 ? SIZE_MAX : rng();
    for (int k = 0; k < 2; k++) {
      css_rule * r = nullptr;
      ASSERT_EQ(css_rule_create(docs[k]->core(), GTEXT_CSSOM_RULE_PRESERVED, nullptr, nullptr, nullptr, 0, false, &r), GTEXT_CSS_OK);
      GTEXT_CSSOM_Rule_List * list = &sheets[k]->s->rules;
      css_rule_list_insert(list, pick == SIZE_MAX ? list->count : pick % (list->count + 1), r);
    }
  }
  std::string text[2];
  size_t depth[2];
  for (int k = 0; k < 2; k++) {
    GTEXT_CSSOM_Rule_List * list = &sheets[k]->s->rules;
    depth[k] = css_rule_list_depth(list);
    for (css_rule * r = css_rule_list_first(list); r; r = css_rule_list_next(r)) {
      text[k] += std::to_string(r->left ? r->left->size : 0) + ",";
    }
  }
  EXPECT_NE(static_cast<void *>(a.s), static_cast<void *>(b.s));
  EXPECT_EQ(depth[0], depth[1]);
  EXPECT_EQ(text[0], text[1]);
  EXPECT_LE(depth[0], 5 * 15u) << "20,000 nodes";
  EXPECT_GE(depth[0], 14u);
}

// Appending in order, the worst case for an unbalanced tree, stays balanced; `at(i)` is O(log n).
TEST(CssomRuleList, AppendingInOrderStaysBalanced) {
  Doc doc;
  Sheet s(doc, "");
  for (int i = 0; i < 50000; i++) {
    css_rule * r = nullptr;
    ASSERT_EQ(css_rule_create(doc.core(), GTEXT_CSSOM_RULE_PRESERVED, nullptr, nullptr, nullptr, 0, false, &r), GTEXT_CSS_OK);
    css_rule_list_insert(&s.s->rules, s.s->rules.count, r);
  }
  EXPECT_LE(css_rule_list_depth(&s.s->rules), 5 * 16u);
  EXPECT_EQ(gtext_cssom_rule_list_length(&s.s->rules), 50000u);
  Rule r = at(&s.s->rules, 31337);
  size_t idx = 0;
  ASSERT_TRUE(gtext_cssom_rule_list_index_of(&s.s->rules, r.r, &idx));
  EXPECT_EQ(idx, 31337u);
}

/* ---- the declarations of a rule ------------------------------------------------------------------------------------------------------------- */

// The block of a rule is built when it is asked for: a parse that holds many style rules builds none, and the same object is returned every time.
TEST(CssomRuleStyle, IsBuiltWhenAskedForAndIsTheSameObject) {
  Doc doc;
  std::string text;
  for (int i = 0; i < 200; i++) {
    text += "r" + std::to_string(i) + "{color:red;margin:1px}";
  }
  Sheet s(doc, text);
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  GTEXT_CSSOM_Counters c0;
  ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c0), GTEXT_CSS_OK);
  EXPECT_EQ(c0.live_objects, 1u + 200u) << "the sheet and its rules, no declaration block";
  Rule r = at(s.rules(), 5);
  Decl a = style_of(r.r);
  Decl b = style_of(r.r);
  ASSERT_NE(a.d, nullptr);
  EXPECT_EQ(a.d, b.d);
  EXPECT_EQ(gtext_cssom_decl_owner(a.d), GTEXT_CSSOM_DECL_RULE);
  EXPECT_EQ(text_of(a.d), "color: red; margin: 1px;");
  GTEXT_CSSOM_Counters c1;
  ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c1), GTEXT_CSS_OK);
  EXPECT_EQ(c1.live_objects, c0.live_objects + 1);
  EXPECT_EQ(gtext_cssom_decl_document(a.d), doc.d);
}

// A kind that has no declaration block does not answer `style` with a guess. (The kinds whose block is typed descriptors answer: test-cssom-setters.cpp.)
TEST(CssomRuleStyle, AKindWithoutABlockIsUnsupportedNotGuessed) {
  Doc doc;
  Sheet s(doc, "@import url(a.css); @namespace x url(u); @media print{}");
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  for (size_t i = 0; i < 3; i++) {
    Rule r = at(s.rules(), i);
    GTEXT_CSS_Status st = GTEXT_CSS_OK;
    Decl d = style_of(r.r, &st);
    EXPECT_EQ(st, GTEXT_CSS_E_UNSUPPORTED) << i;
    EXPECT_EQ(d.d, nullptr);
  }
  GTEXT_CSSOM_Decl * none = nullptr;
  EXPECT_EQ(gtext_cssom_rule_style(nullptr, &none), GTEXT_CSS_E_INVALID);
}

// A mutation of a rule's block is a mutation of the rule and of its sheet: the rule's version, the sheet's version, the generation, and one notification that names both.
TEST(CssomRuleStyle, AMutationBumpsTheRuleAndTheSheetAndSendsOneEvent) {
  Doc doc;
  Sheet s(doc, "a{color:red} b{width:1px}");
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  Rule a = at(s.rules(), 0);
  Rule b = at(s.rules(), 1);
  Decl style = style_of(a.r);
  const uint64_t sv = gtext_cssom_sheet_version(s.s), av = gtext_cssom_rule_version(a.r), bv = gtext_cssom_rule_version(b.r), gen = gtext_cssom_document_generation(doc.d);
  const Result r = set(style.d, "color", "blue");
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(gtext_cssom_rule_version(a.r), av + 1);
  EXPECT_EQ(gtext_cssom_rule_version(b.r), bv) << "another rule is not touched";
  EXPECT_EQ(gtext_cssom_sheet_version(s.s), sv + 1);
  EXPECT_EQ(gtext_cssom_document_generation(doc.d), gen + 1);
  ASSERT_EQ(doc.events.size(), 1u);
  EXPECT_EQ(doc.events[0].kind, GTEXT_CSSOM_EVENT_DECLARATIONS_CHANGED);
  // a set that leaves the declaration as it is changes nothing
  const Result same = set(style.d, "color", "blue");
  EXPECT_EQ(same.o.kind, GTEXT_CSSOM_UNCHANGED);
  EXPECT_EQ(gtext_cssom_rule_version(a.r), av + 1);
  EXPECT_EQ(gtext_cssom_sheet_version(s.s), sv + 1);
  EXPECT_EQ(doc.events.size(), 1u);
}

// The tap sees the ids of the rule and of its sheet.
TEST(CssomRuleStyle, TheEventNamesTheRuleAndTheSheet) {
  Doc doc;
  Sheet s(doc, "a{color:red}");
  Rule a = at(s.rules(), 0);
  Decl style = style_of(a.r);
  uint64_t rule_id = 0, sheet_id = 0;
  void * sheet_ptr = nullptr;
  void * rule_ptr = nullptr;
  std::vector<void *> got;
  css_cssom_set_tap(doc.core(), [](void * u, const css_cssom_event * e) -> GTEXT_CSS_Status {
    auto * v = static_cast<std::vector<void *> *>(u);
    v->push_back(reinterpret_cast<void *>(static_cast<uintptr_t>(e->rule_id)));
    v->push_back(reinterpret_cast<void *>(static_cast<uintptr_t>(e->sheet_id)));
    v->push_back(e->sheet);
    v->push_back(e->rule);
    return GTEXT_CSS_OK;
  }, &got);
  ASSERT_EQ(set(style.d, "width", "2px").o.kind, GTEXT_CSSOM_CHANGED);
  ASSERT_EQ(got.size(), 4u);
  rule_id = reinterpret_cast<uintptr_t>(got[0]);
  sheet_id = reinterpret_cast<uintptr_t>(got[1]);
  sheet_ptr = got[2];
  rule_ptr = got[3];
  EXPECT_EQ(rule_id, gtext_cssom_rule_id(a.r));
  EXPECT_EQ(sheet_id, gtext_cssom_sheet_id(s.s));
  EXPECT_EQ(sheet_ptr, s.s);
  EXPECT_EQ(rule_ptr, a.r);
  EXPECT_EQ(gtext_cssom_decl_document(style.d), doc.d);
}

// "Detached rule: r = cssRules[0]; deleteRule(0); r.style.setProperty(): the rule changes; the sheet's version does not; no event."
TEST(CssomRuleStyle, ADetachedRulesBlockChangesTheRuleAndNothingElse) {
  Doc doc;
  Sheet s(doc, "a{color:red} b{}");
  Rule a = at(s.rules(), 0);
  Decl style = style_of(a.r);
  css_rule_list_remove(&s.s->rules, a.r);
  css_obj_release_internal(&a.r->obj);
  const uint64_t sv = gtext_cssom_sheet_version(s.s), gen = gtext_cssom_document_generation(doc.d), av = gtext_cssom_rule_version(a.r);
  const Result r = set(style.d, "color", "green");
  EXPECT_EQ(r.o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(text_of(style.d), "color: green;");
  EXPECT_EQ(gtext_cssom_rule_version(a.r), av + 1) << "the rule changes";
  EXPECT_EQ(gtext_cssom_sheet_version(s.s), sv);
  EXPECT_EQ(gtext_cssom_document_generation(doc.d), gen) << "no document sees it";
  EXPECT_TRUE(doc.events.empty());
}

// A held declaration block keeps its rule, and its rule's sheet, alive.
TEST(CssomRuleStyle, AHeldBlockKeepsItsRuleAndSheetAlive) {
  Doc doc;
  GTEXT_CSSOM_Decl * held = nullptr;
  {
    Sheet s(doc, "a{color:red} b{}");
    Rule a = at(s.rules(), 0);
    ASSERT_EQ(gtext_cssom_rule_style(a.r, &held), GTEXT_CSS_OK);
  }
  EXPECT_EQ(text_of(held), "color: red;");
  EXPECT_EQ(set(held, "width", "1px").o.kind, GTEXT_CSSOM_CHANGED);
  EXPECT_EQ(doc.events.size(), 1u) << "the rule is still attached to its sheet, which is alive";
  gtext_cssom_decl_release(held);
  GTEXT_CSSOM_Counters c;
  ASSERT_EQ(gtext_cssom_document_counters(doc.d, &c), GTEXT_CSS_OK);
  EXPECT_EQ(c.live_objects, 0u);
}

/* ---- sources and spans ---------------------------------------------------------------------------------------------------------------------- */

// The span of a parsed rule: byte offsets into the text it was read from, the line and the column (in UTF-16 code units); the text of the span is the rule's text.
TEST(CssomRuleSource, ASpanIsAPositionInTheTextTheRuleWasReadFrom) {
  Doc doc;
  const std::string text = "a{}\n\xC3\xA9 b{c:d}\n@media print{\n  e{}\n}\n";
  Sheet s(doc, text);
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  Rule first = at(s.rules(), 0), second = at(s.rules(), 1), third = at(s.rules(), 2);
  GTEXT_CSSOM_Source_Info i1, i2, i3;
  ASSERT_EQ(gtext_cssom_rule_source(first.r, &i1), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_rule_source(second.r, &i2), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_cssom_rule_source(third.r, &i3), GTEXT_CSS_OK);
  EXPECT_EQ(i1.kind, GTEXT_CSSOM_SOURCE_PARSED);
  EXPECT_EQ(i1.source_id, i3.source_id) << "one parse, one source";
  EXPECT_EQ(i1.span.line, 1u);
  EXPECT_EQ(i1.span.column, 1u);
  EXPECT_EQ(i1.span.offset, 0u);
  EXPECT_EQ(i2.span.line, 2u);
  EXPECT_EQ(i3.span.line, 3u);
  char * out = nullptr;
  size_t n = 0;
  ASSERT_EQ(gtext_cssom_rule_source_text(second.r, &out, &n), GTEXT_CSS_OK);
  EXPECT_EQ(take(doc.core(), out), "\xC3\xA9 b{c:d}");
  ASSERT_EQ(gtext_cssom_rule_source_text(third.r, &out, &n), GTEXT_CSS_OK);
  EXPECT_EQ(take(doc.core(), out), "@media print{\n  e{}\n}");
  Rule inner = at(gtext_cssom_rule_css_rules(third.r), 0);
  GTEXT_CSSOM_Source_Info ii;
  ASSERT_EQ(gtext_cssom_rule_source(inner.r, &ii), GTEXT_CSS_OK);
  EXPECT_EQ(ii.span.line, 4u);
  EXPECT_EQ(ii.span.column, 3u);
  Sheet other(doc, "z{}");
  Rule z = at(other.rules(), 0);
  GTEXT_CSSOM_Source_Info zi;
  ASSERT_EQ(gtext_cssom_rule_source(z.r, &zi), GTEXT_CSS_OK);
  EXPECT_NE(zi.source_id, i1.source_id) << "another parse, another source";
  char * whole = nullptr;
  ASSERT_EQ(gtext_cssom_sheet_source_text(s.s, &whole, &n), GTEXT_CSS_OK);
  EXPECT_EQ(take(doc.core(), whole), text);
}

// The columns count UTF-16 code units, the offsets bytes: a rule after a character outside the BMP.
TEST(CssomRuleSource, ColumnsAreInUtf16CodeUnitsAndOffsetsAreBytes) {
  Doc doc;
  Sheet s(doc, "/* \xF0\x9F\x98\x80 */ a{}");   // U+1F600 is two UTF-16 code units and four bytes
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  Rule a = at(s.rules(), 0);
  GTEXT_CSSOM_Source_Info info;
  ASSERT_EQ(gtext_cssom_rule_source(a.r, &info), GTEXT_CSS_OK);
  EXPECT_EQ(info.span.column, 10u) << "3 units of `/* `, 2 of the character, 3 of ` */` and a space: 9 units before it";
  EXPECT_EQ(info.span.offset, 11u) << "the same, with 4 bytes for the character";
}

// A line ending of any of CSS's kinds is one line.
TEST(CssomRuleSource, LinesCountEveryKindOfLineEnding) {
  Doc doc;
  Sheet s(doc, "a{}\r\nb{}\rc{}\fd{}\ne{}");
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  for (size_t i = 0; i < 5; i++) {
    Rule r = at(s.rules(), i);
    GTEXT_CSSOM_Source_Info info;
    ASSERT_EQ(gtext_cssom_rule_source(r.r, &info), GTEXT_CSS_OK);
    EXPECT_EQ(info.span.line, i + 1) << i;
    char * out = nullptr;
    ASSERT_EQ(gtext_cssom_rule_source_text(r.r, &out, nullptr), GTEXT_CSS_OK);
    EXPECT_EQ(take(doc.core(), out).size(), 3u) << "the offsets index the text as it was given";
  }
}

} // namespace
