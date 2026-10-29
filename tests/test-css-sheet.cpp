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
 * Style sheets and the rule index (css_sheet.h): the compiled sheet, nesting, layers, imports, `@scope`, the shadow contexts and the entries the index gives the cascade. A test
 * names the rule it holds; the winner of a property is read from the cascade of the entries the index collected, so every test goes through the whole chain a caller does.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_cascade_test_support.h"
#include "css_synth_tree.h"
#include "css_test_support.h"

using namespace cas;

namespace {

bool yes(void *, const GTEXT_CSS_Rule *) {
  return true;
}
bool no(void *, const GTEXT_CSS_Rule *) {
  return false;
}

struct Compiled {
  GTEXT_CSS_Sheet * s = nullptr;
  GTEXT_CSS_Sheet_Result res{};
  GTEXT_CSS_Status st = GTEXT_CSS_E_INTERNAL;
  Compiled(const std::string & css, const GTEXT_CSS_Sheet_Options * o = nullptr) {
    css_test::Parsed p;
    css_test::parse_into(p, gtext_css_parse_stylesheet, css);
    st = gtext_css_sheet_compile(p.tree, R(), o, &s, &res);
  }
  Compiled(const Compiled &) = delete;
  Compiled & operator=(const Compiled &) = delete;
  ~Compiled() {
    gtext_css_sheet_free(s);
  }
  std::string dump() const {
    std::string out;
    GTEXT_Markup_Sink sink;
    sink.user = &out;
    sink.write = [](void * u, const char * d, size_t n) {
      static_cast<std::string *>(u)->append(d, n);
      return GTEXT_MARKUP_OK;
    };
    EXPECT_EQ(gtext_css_sheet_dump(s, &sink), GTEXT_CSS_OK);
    return out;
  }
};

GTEXT_CSS_Sheet_Options with_filter(bool (*f)(void *, const GTEXT_CSS_Rule *)) {
  GTEXT_CSS_Sheet_Options o = gtext_css_sheet_options_default();
  o.active = f;
  return o;
}

struct Index {
  GTEXT_CSS_Rule_Index * x = nullptr;
  GTEXT_CSS_Rule_Index_Result res{};
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  explicit Index(bool quirks = false) {
    GTEXT_CSS_Rule_Index_Options o = gtext_css_rule_index_options_default();
    o.html_document = true;
    o.quirks = quirks;
    EXPECT_EQ(gtext_css_rule_index_new(R(), &o, &x), GTEXT_CSS_OK);
  }
  Index(const Index &) = delete;
  Index & operator=(const Index &) = delete;
  ~Index() {
    gtext_css_rule_index_free(x);
  }
  GTEXT_CSS_Status add(Compiled & c, GTEXT_CSS_Origin origin = GTEXT_CSS_ORIGIN_AUTHOR, int shadow_scope = 0, uint32_t context = 0, GTEXT_CSS_Element_Handle implicit = nullptr) {
    EXPECT_EQ(c.st, GTEXT_CSS_OK);
    GTEXT_CSS_Sheet_Registration reg;
    std::memset(&reg, 0, sizeof reg);
    reg.origin = origin;
    reg.context = context;
    reg.shadow = shadow_scope != 0;
    reg.tree_scope = shadow_scope ? synth::Tree::scope_handle(shadow_scope) : nullptr;
    reg.implicit_root = implicit;
    st = gtext_css_rule_index_add_sheet(x, c.s, &reg, &res);
    return st;
  }
  GTEXT_CSS_Status freeze() {
    st = gtext_css_rule_index_freeze(x, &res);
    return st;
  }
};

struct Page {
  synth::Tree t;
  GTEXT_CSS_Selector_Match_Context * ctx = nullptr;
  bool quirks = false;
  void open() {
    GTEXT_CSS_Selector_Match_Options o = gtext_css_selector_match_options_default();
    o.html_document = true;
    o.quirks = quirks;
    ASSERT_EQ(gtext_css_selector_match_context_new(&t.table, &o, &ctx), GTEXT_CSS_OK);
  }
  ~Page() {
    gtext_css_selector_match_context_free(ctx);
  }
};

/// The winner of a property for an element, as text ("-" for none), and the status of the collection in `st`.
std::string win(Page & pg, Index & ix, int el, const std::string & prop, const char * pseudo = nullptr, GTEXT_CSS_Status * st = nullptr) {
  Set set(nullptr);
  GTEXT_CSS_Selector_List * pl = nullptr;
  if (pseudo) {
    EXPECT_EQ(gtext_css_selector_parse(pseudo, std::strlen(pseudo), nullptr, &pl, nullptr), GTEXT_CSS_OK);
  }
  GTEXT_CSS_Rule_Index_Result r;
  const GTEXT_CSS_Status s = gtext_css_rules_collect(ix.x, pg.ctx, synth::Tree::H(el), pl, set.s, &r);
  gtext_css_selector_list_free(pl);
  if (st) {
    *st = s;
  }
  if (s != GTEXT_CSS_OK) {
    return std::string("!") + gtext_css_status_string(s);
  }
  Out o(set);
  return o.value(prop);
}

}  // namespace

TEST(CssSheet, SpecificityOrderAndImportanceAreTheCascades) {
  /* Cascade 5, 6.1: the most specific selector wins, and then the later rule; `!important` reverses the origins. */
  Compiled c("p { color: red } .a { color: blue } #x { color: green } p { color: black } .b { color: orange !important }");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_sheet_record_count(c.s), 5u);
  Page pg;
  const int p = pg.t.add(0, "p", "x", {"a", "b"});
  const int q = pg.t.add(0, "p", "", {"a"});
  const int r = pg.t.add(0, "p");
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, p, "color"), "orange") << "the important declaration of .b outranks the normal ones";
  EXPECT_EQ(win(pg, ix, q, "color"), "blue") << ".a is (0,1,0), p is (0,0,1)";
  EXPECT_EQ(win(pg, ix, r, "color"), "black") << "the later of two equal rules";
}

TEST(CssSheet, RecordsCarryTheirSpecificityAndTheDumpIsCanonical) {
  Compiled c("a#i.c:hover, b { color: red; width: 1px !important }");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_sheet_record_count(c.s), 2u);
  GTEXT_CSS_Sheet_Record r0, r1;
  ASSERT_TRUE(gtext_css_sheet_record(c.s, 0, &r0));
  ASSERT_TRUE(gtext_css_sheet_record(c.s, 1, &r1));
  EXPECT_EQ(r0.specificity.a, 1u);
  EXPECT_EQ(r0.specificity.b, 2u);
  EXPECT_EQ(r0.specificity.c, 1u);
  EXPECT_EQ(r1.specificity.c, 1u);
  EXPECT_EQ(r0.rule, r1.rule) << "one rule, two complex selectors";
  EXPECT_EQ(r0.declaration_count, 2u);
  GTEXT_CSS_Property_Id props[4];
  bool imp[4];
  EXPECT_EQ(gtext_css_sheet_record_declarations(c.s, 0, props, imp, 4), 2u);
  EXPECT_FALSE(imp[0]);
  EXPECT_TRUE(imp[1]);
  const std::string d = c.dump();
  EXPECT_EQ(d.rfind("sheet 1\n", 0), 0u);
  EXPECT_NE(d.find("record 0 a#i.c:hover spec=1,2,1"), std::string::npos) << d;
  EXPECT_NE(d.find("rule 0 width: 1px !important"), std::string::npos) << d;
  EXPECT_FALSE(gtext_css_sheet_record(c.s, 2, &r0));
}

TEST(CssSheet, InvalidSelectorsAndDeclarationsAreDroppedWithADiagnostic) {
  /* Selectors 4, 3.6: an invalid selector makes the rule invalid; Syntax 5.5.6 and Cascade 5: an invalid declaration is dropped. */
  Compiled c("p:nonesuch(1) { color: red } p { color: nonsense; colour: red; width: 3px }");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_sheet_record_count(c.s), 1u);
  GTEXT_CSS_Sheet_Record r;
  ASSERT_TRUE(gtext_css_sheet_record(c.s, 0, &r));
  EXPECT_EQ(r.declaration_count, 1u);
  EXPECT_EQ(gtext_css_sheet_diagnostic_total(c.s), 3u);
  GTEXT_CSS_Sheet_Diagnostic d;
  ASSERT_TRUE(gtext_css_sheet_diagnostic(c.s, 0, &d));
  EXPECT_EQ(d.kind, GTEXT_CSS_SHEET_DIAG_INVALID_SELECTOR);
  ASSERT_TRUE(gtext_css_sheet_diagnostic(c.s, 1, &d));
  EXPECT_EQ(d.kind, GTEXT_CSS_SHEET_DIAG_INVALID_VALUE);
  ASSERT_TRUE(gtext_css_sheet_diagnostic(c.s, 2, &d));
  EXPECT_EQ(d.kind, GTEXT_CSS_SHEET_DIAG_UNKNOWN_PROPERTY);
  EXPECT_NE(d.reference, nullptr);
}

TEST(CssSheet, OfTwoSelectorsOfOneRuleTheMostSpecificCounts) {
  Compiled c("p, #x { color: red } .a { color: blue }");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  Page pg;
  const int p = pg.t.add(0, "p", "x", {"a"});
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, p, "color"), "red") << "the rule takes the specificity of #x, the more specific of the two selectors that match";
}

TEST(CssSheetNesting, TheNestedSelectorIsRelativeToTheParentAndSpecificityTakesTheParentsMost) {
  /* CSS Nesting 1: a nested rule without `&` is `& ` and the selector; `&` has the specificity of the most specific selector of the parent's list. */
  Compiled c(".a, #b { & .c { color: red } .d { color: blue } > .e { color: green } .f & { color: orange } }");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  const std::string d = c.dump();
  EXPECT_NE(d.find("& .c spec=1,1,0"), std::string::npos) << d << "(& is the :is() of the list: the id)";
  EXPECT_NE(d.find("& .d spec=1,1,0"), std::string::npos) << d;
  EXPECT_NE(d.find("& > .e spec=1,1,0"), std::string::npos) << d;
  EXPECT_NE(d.find(".f & spec=1,1,0"), std::string::npos) << d;
  Page pg;
  const int a = pg.t.add(0, "div", "", {"a"});
  const int c1 = pg.t.add(a, "span", "", {"c"});
  const int d1 = pg.t.add(a, "span", "", {"d"});
  const int e1 = pg.t.add(a, "span", "", {"e"});
  const int w = pg.t.add(0, "p", "", {"f"});
  const int in = pg.t.add(w, "div", "", {"a"});
  const int outsider = pg.t.add(0, "span", "", {"d"});
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, c1, "color"), "red");
  EXPECT_EQ(win(pg, ix, d1, "color"), "blue");
  EXPECT_EQ(win(pg, ix, e1, "color"), "green");
  EXPECT_EQ(win(pg, ix, in, "color"), "orange");
  EXPECT_EQ(win(pg, ix, a, "color"), "-");
  EXPECT_EQ(win(pg, ix, outsider, "color"), "-") << "a nested .d is `& .d`: a .d outside every .a and #b is not styled";
}

TEST(CssSheetNesting, ADeclarationAfterANestedRuleHasAPlaceOfItsOwn) {
  /* CSS Nesting 1: the declarations after a nested rule are a nested declarations rule, which comes after it in the order of appearance. */
  Compiled c(".a { color: red; & { color: blue } color: green }");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  Page pg;
  const int a = pg.t.add(0, "div", "", {"a"});
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, a, "color"), "green");
  Compiled c2(".a { color: red; & { color: blue } }");
  Index ix2;
  ASSERT_EQ(ix2.add(c2), GTEXT_CSS_OK);
  ASSERT_EQ(ix2.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix2, a, "color"), "blue") << "the nested rule is after the parent's own declarations";
}

TEST(CssSheetNesting, NestedDeclarationsTakeTheSpecificityOfTheParentsSelectorWithItsOwnNesting) {
  Compiled c(".x { & .y { color: red; .z { color: blue } color: green } }");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  const std::string d = c.dump();
  size_t n = 0;
  for (size_t at = d.find("& .y spec=0,2,0"); at != std::string::npos; at = d.find("& .y spec=0,2,0", at + 1)) {
    n++;
  }
  EXPECT_EQ(n, 2u) << d << "(the rule and its nested declarations rule)";
}

TEST(CssSheetNesting, AnInvalidParentTakesWhatIsNestedInItWithIt) {
  Compiled c("p:nonesuch(1) { & b { color: red } } i { color: blue }");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_sheet_record_count(c.s), 1u);
}

TEST(CssSheetNesting, TheNestingDepthIsALimit) {
  std::string css;
  for (int i = 0; i < 30; i++) {
    css += "a{";
  }
  css += "color:red";
  for (int i = 0; i < 30; i++) {
    css += "}";
  }
  Compiled c(css);
  EXPECT_EQ(c.st, GTEXT_CSS_OK);
  GTEXT_CSS_Sheet_Options o = gtext_css_sheet_options_default();
  o.max_rule_depth = 16;
  Compiled c2(css, &o);
  EXPECT_EQ(c2.st, GTEXT_CSS_E_LIMIT_RULE_NESTING);
}

TEST(CssSheetLayers, TheLayerOrderDecidesBeforeSpecificityAndImportanceReverses) {
  /* Cascade 5, 6.4: later layers win for normal declarations, earlier layers for important ones; unlayered rules are last. */
  Compiled c("@layer a, b; @layer b { p { color: red } } @layer a { p#x { color: blue } } p { background-color: green } "
             "@layer a { p { margin-top: 1px !important } } @layer b { p { margin-top: 2px !important } }");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  Page pg;
  const int p = pg.t.add(0, "p", "x");
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, p, "color"), "red") << "layer b is after layer a, whatever the specificity";
  EXPECT_EQ(win(pg, ix, p, "margin-top"), "1px") << "important: the earlier layer";
  EXPECT_EQ(win(pg, ix, p, "background-color"), "green");
}

TEST(CssSheetLayers, AnonymousLayersAreUniqueAndNestedOnesAreGrouped) {
  Compiled c("@layer { p { color: red } } @layer { p { color: blue } } @layer x { @layer y { i { color: green } } i { color: orange } }");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_sheet_layer_count(c.s), 4u);
  Page pg;
  const int p = pg.t.add(0, "p");
  const int i = pg.t.add(0, "i");
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, p, "color"), "blue");
  EXPECT_EQ(win(pg, ix, i, "color"), "orange") << "x.y comes before x's own rules";
}

TEST(CssSheetLayers, AConditionalRuleNeedsTheFilterAndALayerInsideADisabledOneIsNotDeclared) {
  const std::string css = "@layer a, b; @media print { @layer c { p { color: red } } } @layer b { p { color: blue } }";
  Compiled none(css);
  EXPECT_EQ(none.st, GTEXT_CSS_E_UNSUPPORTED) << "no filter: never a guess";
  EXPECT_STREQ(none.res.name, "active_rule");
  GTEXT_CSS_Sheet_Options off = with_filter(no);
  Compiled c_off(css, &off);
  ASSERT_EQ(c_off.st, GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_sheet_layer_count(c_off.s), 2u) << "the layer of the rule that does not apply is not declared";
  GTEXT_CSS_Sheet_Options on = with_filter(yes);
  Compiled c_on(css, &on);
  ASSERT_EQ(c_on.st, GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_sheet_layer_count(c_on.s), 3u);
  Compiled styles("@media print { p { color: red } }");
  EXPECT_EQ(styles.st, GTEXT_CSS_E_UNSUPPORTED) << "a conditional rule that holds a style rule and no filter";
  EXPECT_STREQ(styles.res.name, "active_rule");
  Compiled plain("@media print { @font-face { font-family: x } } p { color: red }");
  EXPECT_EQ(plain.st, GTEXT_CSS_OK) << "a conditional rule that holds no styles needs no filter";
}

namespace {
struct Importer {
  std::vector<std::string> urls;
  std::vector<Compiled *> owned;
  Compiled * next = nullptr;
  bool fail = false;
};
GTEXT_CSS_Sheet * give(void * user, const GTEXT_CSS_Import_Request * rq) {
  Importer * im = static_cast<Importer *>(user);
  im->urls.push_back(std::string(rq->url.data, rq->url.length) + (rq->layered ? (rq->layer_anonymous ? " layer" : " layer(" + std::string(rq->layer.data, rq->layer.length) + ")") : ""));
  if (im->fail || !im->next) {
    return nullptr;
  }
  return gtext_css_sheet_retain(im->next->s);
}
}  // namespace

TEST(CssSheetImport, AnImportedSheetIsPlacedBeforeTheImporterAndLayeredImportsNest) {
  /* Cascade 5, 2.1 and 6.4.2: an imported style sheet is placed at the @import; a layered import puts its sheet, and its unlayered rules, in that layer. */
  Compiled lib("p { color: red } @layer inner { p { background-color: red } } i { color: red }");
  ASSERT_EQ(lib.st, GTEXT_CSS_OK);
  Importer im;
  im.next = &lib;
  GTEXT_CSS_Sheet_Options o = gtext_css_sheet_options_default();
  o.import = give;
  o.import_user = &im;
  Compiled top("@import url(lib.css) layer(base); @layer later; p { color: blue } i { background-color: blue }", &o);
  ASSERT_EQ(top.st, GTEXT_CSS_OK);
  ASSERT_EQ(im.urls.size(), 1u);
  EXPECT_EQ(im.urls[0], "lib.css layer(base)");
  EXPECT_EQ(gtext_css_sheet_import_count(top.s), 1u);
  Page pg;
  const int p = pg.t.add(0, "p");
  const int i = pg.t.add(0, "i");
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(top), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_rule_index_sheet_count(ix.x), 2u);
  EXPECT_EQ(win(pg, ix, p, "color"), "blue") << "the importer's unlayered rule beats the layered import";
  EXPECT_EQ(win(pg, ix, i, "color"), "red");
  EXPECT_EQ(win(pg, ix, p, "background-color"), "red") << "base.inner";
  const GTEXT_CSS_Layer_Registry * lr = gtext_css_rule_index_layers(ix.x, 0);
  ASSERT_NE(lr, nullptr);
  EXPECT_NE(gtext_css_layer_find(lr, GTEXT_CSS_ORIGIN_AUTHOR, 0, "base.inner", 10), 0u);
}

TEST(CssSheetImport, ALateImportAndAFailedImportContributeNothing) {
  Importer im;
  GTEXT_CSS_Sheet_Options o = gtext_css_sheet_options_default();
  o.import = give;
  o.import_user = &im;
  Compiled c("@import 'a.css'; p { color: red } @import 'b.css';", &o);
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  ASSERT_EQ(im.urls.size(), 1u) << "the import after a style rule is not read";
  EXPECT_EQ(gtext_css_sheet_import_count(c.s), 0u);
  EXPECT_EQ(gtext_css_sheet_diagnostic_total(c.s), 2u);
  GTEXT_CSS_Sheet_Diagnostic d;
  ASSERT_TRUE(gtext_css_sheet_diagnostic(c.s, 0, &d));
  EXPECT_EQ(d.kind, GTEXT_CSS_SHEET_DIAG_IMPORT_FAILED);
}

TEST(CssSheetImport, ADisabledConditionalImportDoesNotDeclareItsLayer) {
  Importer im;
  GTEXT_CSS_Sheet_Options o = with_filter(no);
  o.import = give;
  o.import_user = &im;
  Compiled c("@import 'a.css' layer(x) supports(display: grid); @layer y; p { color: red }", &o);
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  EXPECT_TRUE(im.urls.empty());
  EXPECT_EQ(gtext_css_sheet_layer_count(c.s), 1u) << "only y";
  GTEXT_CSS_Sheet_Options none = gtext_css_sheet_options_default();
  Compiled c2("@import 'a.css' supports(display: grid);", &none);
  EXPECT_EQ(c2.st, GTEXT_CSS_E_UNSUPPORTED);
}

TEST(CssSheetScope, TheNearerRootWinsBeforeTheOrderOfAppearance) {
  /* Cascade 6, 3.1: "the declaration with the fewest generational or sibling-element hops between the scoping root and the scoped style rule subject wins". */
  Compiled c("@scope (.b) { p { color: blue } } @scope (.a) { p { color: red } } p { background-color: green }");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  Page pg;
  const int a = pg.t.add(0, "div", "", {"a"});
  const int b = pg.t.add(a, "div", "", {"b"});
  const int p = pg.t.add(b, "p");
  const int p2 = pg.t.add(a, "p");
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, p, "color"), "blue") << "the .b root is one hop from the p, the .a root two, and .a is later";
  EXPECT_EQ(win(pg, ix, p2, "color"), "red");
  EXPECT_EQ(win(pg, ix, p, "background-color"), "green");
}

TEST(CssSheetScope, ALimitCutsADonutAndTheRootIsNotItsOwnDescendant) {
  /* Cascade 6, 2.3: the elements in scope are inclusive descendants of the root, and not the limit or its inclusive descendants. */
  Compiled c("@scope (.a) to (.lim) { p { color: red } :scope { background-color: blue } }");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  Page pg;
  const int a = pg.t.add(0, "div", "", {"a"});
  const int p1 = pg.t.add(a, "p");
  const int lim = pg.t.add(a, "section", "", {"lim"});
  const int p2 = pg.t.add(lim, "p");
  const int mid = pg.t.add(lim, "div");
  const int p3 = pg.t.add(mid, "p");
  const int outside = pg.t.add(0, "p");
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, p1, "color"), "red");
  EXPECT_EQ(win(pg, ix, p2, "color"), "-");
  EXPECT_EQ(win(pg, ix, p3, "color"), "-");
  EXPECT_EQ(win(pg, ix, outside, "color"), "-");
  EXPECT_EQ(win(pg, ix, a, "background-color"), "blue") << ":scope is the root, which is in scope";
  EXPECT_EQ(win(pg, ix, lim, "background-color"), "-");
}

TEST(CssSheetScope, ALimitThatIsTheRootLeavesNothingInScopeAndAPseudoElementInThePreludeDropsTheRule) {
  /* Browsers: `to (:scope)` and `to (&)` name the root itself, which is a limit and so out of scope, and with it everything below. */
  Page pg;
  const int a = pg.t.add(0, "div", "", {"a"});
  const int p1 = pg.t.add(a, "p");
  pg.open();
  for (const char * css : {"@scope (.a) to (:scope) { p, :scope { color: red } }", "@scope (.a) to (&) { p, :scope { color: red } }"}) {
    Compiled c(css);
    ASSERT_EQ(c.st, GTEXT_CSS_OK) << css;
    Index ix;
    ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
    ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
    EXPECT_EQ(win(pg, ix, p1, "color"), "-") << css;
    EXPECT_EQ(win(pg, ix, a, "color"), "-") << css;
  }
  for (const char * css : {"@scope (.a) to (.b::before) { p { color: red } }", "@scope (.a::before) { p { color: red } }"}) {
    Compiled c(css);
    ASSERT_EQ(c.st, GTEXT_CSS_OK) << css;
    Index ix;
    ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
    ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
    EXPECT_EQ(win(pg, ix, p1, "color"), "-") << "the rule is dropped: " << css;
  }
}

TEST(CssSheetScope, AScopeWithNoPreludeIsTheRegistrationsRootAndDeclarationsAreWhereScope) {
  Compiled c("@scope { p { color: red } color: blue }");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  Page pg;
  const int r = pg.t.add(0, "div");
  const int p = pg.t.add(r, "p");
  const int other = pg.t.add(0, "div");
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(c, GTEXT_CSS_ORIGIN_AUTHOR, 0, 0, synth::Tree::H(r)), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, p, "color"), "red");
  EXPECT_EQ(win(pg, ix, r, "color"), "blue") << "the declarations match the root with no specificity";
  EXPECT_EQ(win(pg, ix, other, "color"), "-");
}

TEST(CssSheetScope, TheSpecificityOfAScopedRuleDoesNotCountTheImpliedScope) {
  Compiled c("@scope (.a) { p { color: red } } p.x { color: blue }");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  const std::string d = c.dump();
  EXPECT_NE(d.find("& p spec=0,0,1"), std::string::npos) << d;
}

TEST(CssSheetShadow, ForNormalDeclarationsTheOuterContextWinsAndForImportantTheInner) {
  /* Cascade 5, 6.1: for normal rules the declaration from the outer context wins, and for important rules the inner one. */
  Compiled outer("x-host { color: red; background-color: red !important }");
  Compiled inner(":host { color: blue; background-color: blue !important } span { color: green } :host(.k) { margin-top: 1px }");
  ASSERT_EQ(outer.st, GTEXT_CSS_OK);
  ASSERT_EQ(inner.st, GTEXT_CSS_OK);
  Page pg;
  const int h = pg.t.add(0, "x-host", "", {"k"});
  const int sc = pg.t.attach_shadow(h);
  const int s = pg.t.add_in_shadow(sc, "span");
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(outer), GTEXT_CSS_OK);
  ASSERT_EQ(ix.add(inner, GTEXT_CSS_ORIGIN_AUTHOR, sc, 1), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, h, "color"), "red");
  EXPECT_EQ(win(pg, ix, h, "background-color"), "blue");
  EXPECT_EQ(win(pg, ix, h, "margin-top"), "1px");
  EXPECT_EQ(win(pg, ix, s, "color"), "green") << "a rule of the shadow tree applies to its elements";
  EXPECT_EQ(win(pg, ix, s, "background-color"), "-");
}

TEST(CssSheetShadow, ASheetOfTheDocumentDoesNotReachIntoAShadowTreeAndSlottedRulesReachOut) {
  Compiled outer("span { color: red } x-host::part(label) { background-color: orange }");
  Compiled inner("::slotted(i) { color: blue } slot { color: green }");
  ASSERT_EQ(outer.st, GTEXT_CSS_OK);
  ASSERT_EQ(inner.st, GTEXT_CSS_OK);
  Page pg;
  const int h = pg.t.add(0, "x-host");
  const int light = pg.t.add(h, "i");
  const int sc = pg.t.attach_shadow(h);
  const int span = pg.t.add_in_shadow(sc, "span");
  const int slot = pg.t.add(span, "slot");
  pg.t.e[light].slot = slot;
  pg.t.e[span].parts.insert({h, "label"});
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(outer), GTEXT_CSS_OK);
  ASSERT_EQ(ix.add(inner, GTEXT_CSS_ORIGIN_AUTHOR, sc, 1), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, span, "color"), "-") << "the document's `span` rule is not for the shadow tree";
  EXPECT_EQ(win(pg, ix, span, "background-color"), "orange") << "::part()";
  EXPECT_EQ(win(pg, ix, light, "color"), "blue") << "::slotted()";
  EXPECT_EQ(win(pg, ix, slot, "color"), "green");
}

TEST(CssSheetShadow, ASharedSheetIsMatchedInEachTreeScope) {
  /* A sheet adopted by two shadow trees is one compiled object; its :host rule must not leak answers from one tree to the other. */
  Compiled shared(":host(.on) { color: red } p { background-color: blue }");
  ASSERT_EQ(shared.st, GTEXT_CSS_OK);
  Page pg;
  const int h1 = pg.t.add(0, "x-a", "", {"on"});
  const int h2 = pg.t.add(0, "x-a");
  const int s1 = pg.t.attach_shadow(h1);
  const int s2 = pg.t.attach_shadow(h2);
  const int p1 = pg.t.add_in_shadow(s1, "p");
  const int p2 = pg.t.add_in_shadow(s2, "p");
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(shared, GTEXT_CSS_ORIGIN_AUTHOR, s1, 1), GTEXT_CSS_OK);
  ASSERT_EQ(ix.add(shared, GTEXT_CSS_ORIGIN_AUTHOR, s2, 2), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, h1, "color"), "red");
  EXPECT_EQ(win(pg, ix, h2, "color"), "-");
  EXPECT_EQ(win(pg, ix, p1, "background-color"), "blue");
  EXPECT_EQ(win(pg, ix, p2, "background-color"), "blue");
  EXPECT_EQ(win(pg, ix, h1, "color"), "red") << "again, from the memo";
}

TEST(CssSheetShadow, LayersAreScopedToTheirContext) {
  Compiled outer("@layer a, b; @layer a { x-host { color: red } }");
  Compiled inner("@layer b, a; @layer b { :host { background-color: blue } }");
  Page pg;
  const int h = pg.t.add(0, "x-host");
  const int sc = pg.t.attach_shadow(h);
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(outer), GTEXT_CSS_OK);
  ASSERT_EQ(ix.add(inner, GTEXT_CSS_ORIGIN_AUTHOR, sc, 1), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, h, "color"), "red");
  EXPECT_EQ(win(pg, ix, h, "background-color"), "blue");
  EXPECT_NE(gtext_css_rule_index_layers(ix.x, 1), gtext_css_rule_index_layers(ix.x, 0));
}

TEST(CssSheetIndex, PseudoElementsAreAskedForByName) {
  Compiled c("p { color: red } p::before { color: blue } p::after { color: green } p::before::marker { color: orange }");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  Page pg;
  const int p = pg.t.add(0, "p");
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, p, "color"), "red");
  EXPECT_EQ(win(pg, ix, p, "color", "::before"), "blue");
  EXPECT_EQ(win(pg, ix, p, "color", "::after"), "green");
  EXPECT_EQ(win(pg, ix, p, "color", "::before::marker"), "orange");
  EXPECT_EQ(win(pg, ix, p, "color", "::first-line"), "-");
}

TEST(CssSheetIndex, TheRegistrationAndTheLifecycleAreChecked) {
  Compiled c("p { color: red }");
  Compiled d("p { color: blue }");
  Page pg;
  const int h = pg.t.add(0, "x-host");
  const int sc = pg.t.attach_shadow(h);
  pg.open();
  {
    Index ix;
    GTEXT_CSS_Sheet_Registration reg;
    std::memset(&reg, 0, sizeof reg);
    reg.origin = GTEXT_CSS_ORIGIN_ANIMATION;
    EXPECT_EQ(gtext_css_rule_index_add_sheet(ix.x, c.s, &reg, nullptr), GTEXT_CSS_E_INVALID) << "no layers in the animation origin";
    EXPECT_EQ(ix.add(c, GTEXT_CSS_ORIGIN_AUTHOR, sc, 0), GTEXT_CSS_E_INVALID) << "a shadow tree is above context 0";
    EXPECT_EQ(ix.add(c), GTEXT_CSS_OK);
    EXPECT_EQ(ix.add(d, GTEXT_CSS_ORIGIN_AUTHOR, sc, 1), GTEXT_CSS_OK);
    GTEXT_CSS_Rule_Index_Result r;
    Set set(nullptr);
    EXPECT_EQ(gtext_css_rules_collect(ix.x, pg.ctx, synth::Tree::H(h), nullptr, set.s, &r), GTEXT_CSS_E_INVALID) << "not frozen";
    ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
    EXPECT_EQ(ix.add(d), GTEXT_CSS_E_INVALID) << "frozen";
    EXPECT_EQ(gtext_css_rule_index_record_count(ix.x), 2u);
  }
  {
    // the shadow tree's context must be above the context of the scope that holds its host
    Index ix;
    GTEXT_CSS_Sheet_Registration reg;
    std::memset(&reg, 0, sizeof reg);
    reg.origin = GTEXT_CSS_ORIGIN_AUTHOR;
    reg.shadow = true;
    reg.tree_scope = synth::Tree::scope_handle(sc);
    reg.context = 1;
    GTEXT_CSS_Rule_Index_Result r;
    ASSERT_EQ(gtext_css_rule_index_add_sheet(ix.x, c.s, &reg, &r), GTEXT_CSS_OK);
    GTEXT_CSS_Sheet_Registration inner = reg;
    inner.tree_scope = synth::Tree::scope_handle(sc + 1);
    inner.context = 1;
    EXPECT_EQ(gtext_css_rule_index_add_sheet(ix.x, d.s, &inner, &r), GTEXT_CSS_E_INVALID) << "two scopes, one context number";
    inner.context = 2;
    inner.parent_scope = reg.tree_scope;
    ASSERT_EQ(gtext_css_rule_index_add_sheet(ix.x, d.s, &inner, &r), GTEXT_CSS_OK);
    // the outer is registered as context 1, the inner as 2: fine; now make the inner lower
    Index bad;
    GTEXT_CSS_Sheet_Registration a = reg;
    a.context = 2;
    ASSERT_EQ(gtext_css_rule_index_add_sheet(bad.x, c.s, &a, &r), GTEXT_CSS_OK);
    GTEXT_CSS_Sheet_Registration b = inner;
    b.context = 1;
    ASSERT_EQ(gtext_css_rule_index_add_sheet(bad.x, d.s, &b, &r), GTEXT_CSS_OK);
    EXPECT_EQ(gtext_css_rule_index_freeze(bad.x, &r), GTEXT_CSS_E_INVALID);
    EXPECT_STREQ(r.name, "context order");
  }
}

TEST(CssSheetIndex, NonSelectorCallbacksTheSheetNeedsAreNamed) {
  Compiled c("p:nth-child(2) { color: red }");
  Page pg;
  const int p = pg.t.add(0, "p");
  pg.t.table.sibling_index = nullptr;
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  win(pg, ix, p, "color", nullptr, &st);
  EXPECT_NE(st, GTEXT_CSS_E_INTERNAL);
}

TEST(CssSheetIndex, TheBucketsDoNotChangeTheAnswer) {
  /* The index tests the records of the keys an element has; the answer is that of testing every rule. Built here as a reference. */
  const std::string css =
      "p { color: red } .a { color: blue } #i { color: green } [title] { background-color: red } p.a { margin-top: 1px } .a.b > p { margin-top: 2px } "
      "* { margin-left: 3px } :not(.a) { margin-right: 4px } .a ~ .b { padding-top: 5px } P { padding-left: 6px } .b { margin-bottom: 7px }";
  Compiled c(css);
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  Page pg;
  const int r = pg.t.add(0, "div", "", {"a", "b"});
  const int p = pg.t.add(r, "p", "i", {"a"});
  const int s = pg.t.add(r, "span", "", {"b"});
  const int q = pg.t.add(r, "p", "", {"A"});
  pg.t.e[s].attrs["title"] = "t";
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, p, "color"), "green");
  EXPECT_EQ(win(pg, ix, p, "margin-top"), "2px");
  EXPECT_EQ(win(pg, ix, p, "padding-left"), "6px") << "an HTML element name is compared ignoring case";
  EXPECT_EQ(win(pg, ix, s, "background-color"), "red");
  EXPECT_EQ(win(pg, ix, s, "padding-top"), "5px") << ".a ~ .b: the p.a before it";
  EXPECT_EQ(win(pg, ix, q, "padding-top"), "-");
  EXPECT_EQ(win(pg, ix, q, "color"), "red") << "class names are case-sensitive here";
  EXPECT_EQ(win(pg, ix, q, "margin-right"), "4px");
  EXPECT_EQ(win(pg, ix, r, "margin-left"), "3px");
  EXPECT_EQ(win(pg, ix, r, "margin-bottom"), "7px") << "the element's second class is a key too";
}

TEST(CssSheetIndex, AnElementWithNoGivenNamespaceIsHtmlSoNamesAreComparedIgnoringCase) {
  Compiled c("P { padding-left: 6px } p { padding-top: 5px } DIV.a { margin-top: 1px }");
  Page pg;
  const int p = pg.t.add(0, "p");
  const int d = pg.t.add(0, "div", "", {"a"});
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, p, "padding-top"), "5px");
  EXPECT_EQ(win(pg, ix, p, "padding-left"), "6px");
  EXPECT_EQ(win(pg, ix, d, "margin-top"), "1px");
  GTEXT_CSS_Selector_Match_Result r;
  GTEXT_CSS_Sheet_Record rec;
  gtext_css_sheet_record(c.s, 0, &rec);
  EXPECT_EQ(gtext_css_selector_match(rec.selector, pg.ctx, synth::Tree::H(p), nullptr, &r), GTEXT_CSS_OK);
  EXPECT_TRUE(r.matched);
}

TEST(CssSheetStyle, TheStyleOfAnElementIsComputedFromTheRulesThatMatchIt) {
  /* The whole chain: the sheet, the index, the match context, the cascade and the computed value; the `style` attribute outranks the rules, and inheritance still applies. */
  Compiled c("div { color: red; margin-top: 4px } .k { margin-top: 8px } p { padding-top: 2px }");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  Page pg;
  const int d = pg.t.add(0, "div", "", {"k"});
  const int p = pg.t.add(d, "p");
  pg.t.e[p].attrs["style"] = "padding-top: 9px";
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  GTEXT_CSS_Compute_Options co = gtext_css_compute_options_default();
  co.elements = &pg.t.table;
  GTEXT_CSS_Compute_Context * cc = nullptr;
  ASSERT_EQ(gtext_css_compute_context_new(R(), nullptr, &co, &cc, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Style_Options so = gtext_css_style_options_default();
  so.rules = ix.x;
  so.match = pg.ctx;
  GTEXT_CSS_Computed_Style *ds = nullptr, *ps = nullptr;
  GTEXT_CSS_Compute_Result cr;
  ASSERT_EQ(gtext_css_style_compute(cc, synth::Tree::H(d), nullptr, nullptr, 0, &so, &ds, &cr), GTEXT_CSS_OK) << cr.detail;
  ASSERT_EQ(gtext_css_style_compute(cc, synth::Tree::H(p), ds, nullptr, 0, &so, &ps, &cr), GTEXT_CSS_OK) << cr.detail;
  GTEXT_CSS_Cascade_Entry e;
  ASSERT_TRUE(gtext_css_computed_specified(ds, find(R(), "margin-top"), &e));
  EXPECT_EQ(e.specificity.b, 1u) << ".k beats div";
  ASSERT_TRUE(gtext_css_computed_specified(ps, find(R(), "padding-top"), &e));
  EXPECT_TRUE(e.element_attached) << "the style attribute outranks the rules";
  EXPECT_FALSE(gtext_css_computed_specified(ps, find(R(), "color"), &e)) << "color is inherited, not declared on the p";
  so.rules = ix.x;
  so.match = nullptr;
  GTEXT_CSS_Computed_Style * bad = nullptr;
  EXPECT_EQ(gtext_css_style_compute(cc, synth::Tree::H(d), nullptr, nullptr, 0, &so, &bad, &cr), GTEXT_CSS_E_INVALID);
  gtext_css_computed_free(ds);
  gtext_css_computed_free(ps);
  gtext_css_compute_context_free(cc);
}

TEST(CssSheetIndex, CustomPropertiesAreCollectedWithTheirNames) {
  Compiled c("p { --k: red; --Other: 1px; color: var(--k) }");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  Page pg;
  const int p = pg.t.add(0, "p");
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  Set set(nullptr);
  ASSERT_EQ(gtext_css_rules_collect(ix.x, pg.ctx, synth::Tree::H(p), nullptr, set.s, nullptr), GTEXT_CSS_OK);
  Out o(set);
  EXPECT_EQ(o.custom("--k"), "red");
  EXPECT_EQ(o.custom("--Other"), "1px") << "the case of a custom property's name is kept";
  EXPECT_EQ(o.custom("--other"), "-");
}

namespace {

std::vector<GTEXT_CSS_Cascade_Entry> entries_of(Page & pg, Index & ix, int el, GTEXT_CSS_Status * st = nullptr, GTEXT_CSS_Rule_Index_Result * rr = nullptr) {
  std::vector<GTEXT_CSS_Cascade_Entry> out;
  Set set(nullptr);
  GTEXT_CSS_Rule_Index_Result r;
  const GTEXT_CSS_Status s = gtext_css_rules_collect(ix.x, pg.ctx, synth::Tree::H(el), nullptr, set.s, &r);
  if (st) {
    *st = s;
  }
  if (rr) {
    *rr = r;
  }
  for (size_t i = 0; i < gtext_css_candidates_count(set.s); i++) {
    GTEXT_CSS_Cascade_Entry e;
    gtext_css_candidates_entry(set.s, i, &e);
    e.set = nullptr;
    out.push_back(e);
  }
  return out;
}

}  // namespace

TEST(CssSheetShadow, SlottedAndPartEntriesCarryTheContextOfTheSheetThatOwnsTheRule) {
  /* Matrix: `::slotted(p)` in a shadow sheet for a light `p` is an entry with the shadow's context number and (0,0,2); `host::part(a)` in the outer sheet has the outer one. */
  Compiled outer("x-host::part(a) { margin-top: 1px }");
  Compiled inner("::slotted(p) { color: red }");
  ASSERT_EQ(outer.st, GTEXT_CSS_OK);
  ASSERT_EQ(inner.st, GTEXT_CSS_OK);
  Page pg;
  const int h = pg.t.add(0, "x-host");
  const int light = pg.t.add(h, "p");
  const int sc = pg.t.attach_shadow(h);
  const int slot = pg.t.add_in_shadow(sc, "slot");
  const int part = pg.t.add(slot, "span");
  pg.t.e[light].slot = slot;
  pg.t.e[part].parts.insert({h, "a"});
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(outer), GTEXT_CSS_OK);
  ASSERT_EQ(ix.add(inner, GTEXT_CSS_ORIGIN_AUTHOR, sc, 3), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  const auto s = entries_of(pg, ix, light);
  ASSERT_EQ(s.size(), 1u);
  EXPECT_EQ(s[0].context, 3u);
  EXPECT_EQ(s[0].specificity.a, 0u);
  EXPECT_EQ(s[0].specificity.b, 0u);
  EXPECT_EQ(s[0].specificity.c, 2u);
  const auto q = entries_of(pg, ix, part);
  ASSERT_EQ(q.size(), 1u);
  EXPECT_EQ(q[0].context, 0u) << "the outer sheet owns the ::part() rule";
  EXPECT_EQ(q[0].specificity.c, 2u) << "the host, and the pseudo-element";
}

TEST(CssSheetShadow, ASlotThatIsAssignedToASlotPassesTheElementOnToTheNextShadowTree) {
  /* Scoping 1: an element assigned to a slot that is itself assigned is slotted in every shadow tree on the chain, and each contributes its rules. */
  Compiled inner("::slotted(i) { color: red }");
  Compiled outer_shadow("::slotted(i) { background-color: blue }");
  Page pg;
  const int outer_host = pg.t.add(0, "x-outer");
  const int inner_host = pg.t.add(outer_host, "x-inner");
  const int light = pg.t.add(inner_host, "i");
  const int s_outer = pg.t.attach_shadow(outer_host);
  const int slot_outer = pg.t.add_in_shadow(s_outer, "slot");
  const int s_inner = pg.t.attach_shadow(inner_host);
  const int slot_inner = pg.t.add_in_shadow(s_inner, "slot");
  pg.t.e[light].slot = slot_inner;
  pg.t.e[slot_inner].slot = slot_outer;
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(inner, GTEXT_CSS_ORIGIN_AUTHOR, s_inner, 2), GTEXT_CSS_OK);
  ASSERT_EQ(ix.add(outer_shadow, GTEXT_CSS_ORIGIN_AUTHOR, s_outer, 1), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, light, "color"), "red");
  EXPECT_EQ(win(pg, ix, light, "background-color"), "blue");
}

TEST(CssSheetShadow, ASlotThatIsAssignedToASlotIsNotSlottedAndWhatIsAssignedToItIs) {
  /* Scoping 1: ::slotted() matches the elements assigned to a slot "after flattening", which replaces a slot of a shadow tree that is assigned to another slot by what is assigned to
   * it. Both browsers: `::slotted(*)` of the outer shadow tree styles the light element and not the slot in between. */
  Compiled outer_shadow("::slotted(*) { color: red; margin-top: 5px }");
  Page pg;
  const int outer_host = pg.t.add(0, "x-outer");
  const int inner_host = pg.t.add(outer_host, "x-inner");
  const int light = pg.t.add(inner_host, "i");
  const int s_outer = pg.t.attach_shadow(outer_host);
  const int slot_outer = pg.t.add_in_shadow(s_outer, "slot");
  const int s_inner = pg.t.attach_shadow(inner_host);
  const int slot_inner = pg.t.add_in_shadow(s_inner, "slot");
  pg.t.e[light].slot = slot_inner;
  pg.t.e[slot_inner].slot = slot_outer;
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(outer_shadow, GTEXT_CSS_ORIGIN_AUTHOR, s_outer, 1), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, light, "color"), "red") << "the light element is flattened into the outer slot";
  EXPECT_EQ(win(pg, ix, slot_inner, "color"), "-") << "the slot in between is replaced by what is assigned to it";
  EXPECT_EQ(win(pg, ix, slot_inner, "margin-top"), "-");
}

TEST(CssSheetShadow, TheClimbFromAHostStopsAtTheHostEvenWhenTheHostIsTheTopOfAnotherShadowTree) {
  /* The host of a shadow tree is the top of what that tree's selectors see, also when it is the top element of the shadow tree it is in: `x-host span` in the inner sheet does not
   * reach the outer host, which is named x-host too (found by the differential: both browsers leave the span alone). */
  Compiled inner("x-host { margin-left: 5px; & span { padding-left: 1px } } x-host span { margin-top: 2px } :host span { color: red }");
  Page pg;
  const int outer = pg.t.add(0, "x-host");
  const int s_outer = pg.t.attach_shadow(outer);
  const int mid = pg.t.add_in_shadow(s_outer, "x-host");
  const int s_inner = pg.t.attach_shadow(mid);
  const int span = pg.t.add_in_shadow(s_inner, "span");
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(inner, GTEXT_CSS_ORIGIN_AUTHOR, s_inner, 2), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, span, "padding-left"), "-");
  EXPECT_EQ(win(pg, ix, span, "margin-top"), "-");
  EXPECT_EQ(win(pg, ix, span, "color"), "red") << "`:host span` is the one that reaches the host";
  EXPECT_EQ(win(pg, ix, mid, "margin-left"), "-") << "`x-host` is not :host";
}

TEST(CssSheetScope, ADistanceIsReportedAndTheDonutKeepsOutWhatIsBehindTheLimit) {
  /* Matrix: `@scope (.card) to (.slot) { p {} }`: a p inside .slot does not match, a p beside it matches with distance 1. */
  Compiled c("@scope (.card) to (.slot) { p { color: red } }");
  Page pg;
  const int card = pg.t.add(0, "div", "", {"card"});
  const int slot = pg.t.add(card, "div", "", {"slot"});
  const int in_slot = pg.t.add(slot, "p");
  const int beside = pg.t.add(card, "p");
  const int deep = pg.t.add(pg.t.add(card, "section"), "p");
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_TRUE(entries_of(pg, ix, in_slot).empty());
  const auto b = entries_of(pg, ix, beside);
  ASSERT_EQ(b.size(), 1u);
  EXPECT_TRUE(b[0].scoped);
  EXPECT_EQ(b[0].scope_distance, 1u);
  const auto d = entries_of(pg, ix, deep);
  ASSERT_EQ(d.size(), 1u);
  EXPECT_EQ(d[0].scope_distance, 2u);
}

TEST(CssSheetIndex, AMissingCallbackIsNamedNotWorkedAround) {
  /* Matrix: `:empty` with an interface lacking `child_content` is E_UNSUPPORTED naming it. */
  Compiled c("p:empty { color: red }");
  Page pg;
  const int p = pg.t.add(0, "p");
  pg.t.table.child_content = nullptr;
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  GTEXT_CSS_Rule_Index_Result rr;
  entries_of(pg, ix, p, &st, &rr);
  EXPECT_EQ(st, GTEXT_CSS_E_UNSUPPORTED);
  EXPECT_NE(std::string(rr.name).find("child_content"), std::string::npos) << rr.name;
}

TEST(CssSheetIndex, TwoSheetsAreOneSheetInOrderOfAppearance) {
  /* The result of a document depends on the order of appearance and on nothing else: sheets A and B added one after the other collect what the sheet A + B does, over a family of
   * generated rules; added in the other order they differ somewhere (so the comparison can tell). */
  const char * const names[] = {"p", ".a", "#i", "p.a", "div p", "[t]", ":not(.a)", "*"};
  const char * const props[] = {"color", "background-color", "margin-top"};
  const char * const vals[] = {"red", "blue", "green", "orange"};
  uint32_t seed = 12345;
  auto rnd = [&](uint32_t n) {
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) % n;
  };
  Page pg;
  const int d = pg.t.add(0, "div");
  const int p1 = pg.t.add(d, "p", "i", {"a"});
  const int p2 = pg.t.add(d, "p");
  pg.t.e[p2].attrs["t"] = "1";
  pg.open();
  bool order_mattered = false;
  for (int round = 0; round < 60; round++) {
    std::string a, b;
    for (int k = 0; k < 6; k++) {
      const std::string prop = props[rnd(3)];
      const std::string v = prop == "margin-top" ? (rnd(2) ? "1px" : "2px") : vals[rnd(4)];
      const std::string r2 = std::string(names[rnd(8)]) + " { " + prop + ": " + v + (rnd(4) == 0 ? " !important" : "") + " }";
      (k < 3 ? a : b) += r2 + "\n";
    }
    Compiled ca(a), cb(b), both(a + b);
    ASSERT_EQ(ca.st, GTEXT_CSS_OK);
    ASSERT_EQ(cb.st, GTEXT_CSS_OK);
    ASSERT_EQ(both.st, GTEXT_CSS_OK);
    Index separate, merged, swapped;
    ASSERT_EQ(separate.add(ca), GTEXT_CSS_OK);
    ASSERT_EQ(separate.add(cb), GTEXT_CSS_OK);
    ASSERT_EQ(separate.freeze(), GTEXT_CSS_OK);
    ASSERT_EQ(merged.add(both), GTEXT_CSS_OK);
    ASSERT_EQ(merged.freeze(), GTEXT_CSS_OK);
    ASSERT_EQ(swapped.add(cb), GTEXT_CSS_OK);
    ASSERT_EQ(swapped.add(ca), GTEXT_CSS_OK);
    ASSERT_EQ(swapped.freeze(), GTEXT_CSS_OK);
    for (int el : {p1, p2}) {
      for (const char * prop : props) {
        const std::string want = win(pg, merged, el, prop);
        EXPECT_EQ(win(pg, separate, el, prop), want) << "round " << round << " " << prop << "\n" << a << "---\n" << b;
        order_mattered = order_mattered || win(pg, swapped, el, prop) != want;
      }
    }
  }
  EXPECT_TRUE(order_mattered) << "the generated rules never disagreed about the order, so the test could not see an order that did not matter";
}

TEST(CssSheetStyle, APseudoElementsEntriesComeFromTheSameCall) {
  Compiled c("p { color: red } p::before { color: blue; margin-top: 3px }");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  Page pg;
  const int p = pg.t.add(0, "p");
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  GTEXT_CSS_Compute_Options co = gtext_css_compute_options_default();
  co.elements = &pg.t.table;
  GTEXT_CSS_Compute_Context * cc = nullptr;
  ASSERT_EQ(gtext_css_compute_context_new(R(), nullptr, &co, &cc, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Selector_List * before = nullptr;
  ASSERT_EQ(gtext_css_selector_parse("::before", 8, nullptr, &before, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Style_Options so = gtext_css_style_options_default();
  so.rules = ix.x;
  so.match = pg.ctx;
  GTEXT_CSS_Computed_Style *es = nullptr, *bs = nullptr;
  ASSERT_EQ(gtext_css_style_compute(cc, synth::Tree::H(p), nullptr, nullptr, 0, &so, &es, nullptr), GTEXT_CSS_OK);
  so.pseudo = before;
  ASSERT_EQ(gtext_css_style_compute(cc, synth::Tree::H(p), es, nullptr, 0, &so, &bs, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Cascade_Entry e;
  ASSERT_TRUE(gtext_css_computed_specified(bs, find(R(), "margin-top"), &e));
  EXPECT_FALSE(gtext_css_computed_specified(es, find(R(), "margin-top"), &e)) << "the ::before rule is not the element's";
  gtext_css_computed_free(es);
  gtext_css_computed_free(bs);
  gtext_css_selector_list_free(before);
  gtext_css_compute_context_free(cc);
}

TEST(CssSheetShadow, TheHostIsTheParentOfTheTopOfItsShadowTreeForTheCompoundsThatNameIt) {
  /* Scoping 1: `:host > *` and `:host p` select inside the shadow tree; a compound that does not name :host never matches the host from inside. */
  Compiled inner(":host > span { color: red } :host p { background-color: blue } x-host > span { margin-top: 3px } :host(.k) > span { margin-left: 4px } div :host { padding-left: 5px } "
                 ":host :host-context(div) { margin-top: 9px }");
  ASSERT_EQ(inner.st, GTEXT_CSS_OK);
  Page pg;
  const int h = pg.t.add(0, "x-host", "", {"k"});
  const int sc = pg.t.attach_shadow(h);
  const int span = pg.t.add_in_shadow(sc, "span");
  const int p = pg.t.add(span, "p");
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(inner, GTEXT_CSS_ORIGIN_AUTHOR, sc, 1), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, span, "color"), "red");
  EXPECT_EQ(win(pg, ix, p, "background-color"), "blue");
  EXPECT_EQ(win(pg, ix, span, "margin-top"), "-") << "`x-host` is not :host";
  EXPECT_EQ(win(pg, ix, span, "margin-left"), "4px");
  EXPECT_EQ(win(pg, ix, h, "padding-left"), "-");
}

TEST(CssSheetNesting, ANestedRuleUnderAPseudoElementRepresentsNothing) {
  /* CSS Nesting 1: "a parent selector containing a pseudo-element represents nothing" under `&`: the nested rule does not apply to the element. */
  Compiled c("p::before { & { color: red } &.k { margin-top: 3px } } p { & i { margin-left: 2px } }");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  Page pg;
  const int p = pg.t.add(0, "p", "", {"k"});
  const int i = pg.t.add(p, "i");
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, p, "color"), "-");
  EXPECT_EQ(win(pg, ix, p, "margin-top"), "-");
  EXPECT_EQ(win(pg, ix, p, "color", "::before"), "-") << "and not to the pseudo-element either";
  EXPECT_EQ(win(pg, ix, i, "margin-left"), "2px");
}

TEST(CssSheetScope, ANestedScopeHasItsRootsInsideTheOuterRoot) {
  /* The roots of an @scope inside another are inside the outer root: its prelude is read as a nested selector, with the implied `&`. */
  Compiled c("@scope (.a) { @scope (.a) { p { color: red } } }");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  Page pg;
  const int a1 = pg.t.add(0, "div", "", {"a"});
  const int p1 = pg.t.add(a1, "p");
  const int a2 = pg.t.add(0, "div", "", {"a"});
  const int a3 = pg.t.add(a2, "div", "", {"a"});
  const int p2 = pg.t.add(a3, "p");
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, p1, "color"), "-") << "one .a: it is the outer root, and the inner one must be inside it";
  EXPECT_EQ(win(pg, ix, p2, "color"), "red");
}

TEST(CssSheetShadow, TheHostSeenFromInsideItsShadowTreeIsFeatureless) {
  /* CSS Shadow 1: from inside its shadow tree the host is matched by :host, :host() and :host-context() only: `:host.k`, `:host:hover` and `div :host` match nothing, `:host(.k)` matches, and
   * the argument of :host() is matched against the host as the tree it is in sees it. */
  Compiled inner(":host.k { color: red } :host(.k) { background-color: blue } :host:not(.z) { margin-top: 3px } div :host { margin-left: 4px } :host(.k):hover { padding-left: 1px } "
                 ":host(div.k) { margin-top: 9px }");
  ASSERT_EQ(inner.st, GTEXT_CSS_OK);
  Page pg;
  const int d = pg.t.add(0, "div");
  const int h = pg.t.add(d, "x-host", "", {"k"});
  const int sc = pg.t.attach_shadow(h);
  (void)pg.t.add_in_shadow(sc, "span");
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(inner, GTEXT_CSS_ORIGIN_AUTHOR, sc, 1), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, h, "color"), "-") << "a class beside :host matches the featureless host never";
  EXPECT_EQ(win(pg, ix, h, "background-color"), "blue");
  EXPECT_EQ(win(pg, ix, h, "margin-top"), "-") << ":not() is not :host, and `div.k` is not the host (x-host)";
  EXPECT_EQ(win(pg, ix, h, "margin-left"), "-") << "the host's own ancestors are not reachable from its shadow tree";
  EXPECT_EQ(win(pg, ix, h, "padding-left"), "-");
}

TEST(CssSheetShadow, TheHostIsFoundWhereverTheSubjectNamesIt) {
  /* The browsers apply a :host rule whose subject names :host only through :is(), :where(), a nested `&`, and `:host::part(x)` of the shadow tree's own sheet styles that host's parts. */
  Compiled inner(":is(:host) { color: red } :where(:host) { background-color: blue } :is(:host, .r) { margin-top: 1px } :is(:host(.k), .zz) { margin-left: 2px } "
                 ":host { & { padding-left: 3px } } :is(:host) > span { margin-left: 5px } :host::part(a) { margin-top: 7px } :is(.zzz) { color: green }");
  ASSERT_EQ(inner.st, GTEXT_CSS_OK);
  Page pg;
  const int h = pg.t.add(0, "x-host", "", {"k"});
  const int sc = pg.t.attach_shadow(h);
  const int span = pg.t.add_in_shadow(sc, "span");
  pg.t.e[span].parts.insert({h, "a"});
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(inner, GTEXT_CSS_ORIGIN_AUTHOR, sc, 1), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, h, "color"), "red");
  EXPECT_EQ(win(pg, ix, h, "background-color"), "blue");
  EXPECT_EQ(win(pg, ix, h, "margin-top"), "1px");
  EXPECT_EQ(win(pg, ix, h, "margin-left"), "2px");
  EXPECT_EQ(win(pg, ix, h, "padding-left"), "3px");
  EXPECT_EQ(win(pg, ix, span, "margin-left"), "5px") << ":is(:host) > span";
  EXPECT_EQ(win(pg, ix, span, "margin-top"), "7px") << ":host::part(a) in the sheet of the host's own shadow tree";
}

TEST(CssSheetScope, ThePositionsOfNthOfSDependOnTheScopeElement) {
  /* `div:nth-child(1 of :not(:scope))`: the members of `of S` change with the scoping root, and the index changes the root between records and between elements of one context. */
  Compiled c("@scope (div) { div:nth-child(1 of :not(:scope)) { color: red } }");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  Page pg;
  const int outer = pg.t.add(0, "div");
  const int e = pg.t.add(outer, "div");
  const int e2 = pg.t.add(outer, "div");
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, outer, "color"), "-");
  EXPECT_EQ(win(pg, ix, e, "color"), "red") << "with the outer div as the root, e is the first child that is not the root";
  EXPECT_EQ(win(pg, ix, e2, "color"), "-");
  EXPECT_EQ(win(pg, ix, e, "color"), "red") << "asked again, from the memo";
}

/* ---- tests that kill the mutants a mutation pass left standing ------------------------------------------------------------------------------------------- */

TEST(CssSheetMutants, TheCloserScopeRootWinsInEitherOrderAndAnEqualOneGoesToTheLaterRule) {
  /* Cascade 6, 6.6.1: scoping proximity before order of appearance; at equal proximity the later rule. */
  for (const bool outer_first : {true, false}) {
    Compiled c(outer_first ? "@scope (.a) { p { color: red } } @scope (.b) { p { color: blue } }" : "@scope (.b) { p { color: blue } } @scope (.a) { p { color: red } }");
    Page pg;
    const int a = pg.t.add(0, "div", "", {"a"});
    const int b = pg.t.add(a, "div", "", {"b"});
    const int p = pg.t.add(b, "p");
    pg.open();
    Index ix;
    ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
    ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
    EXPECT_EQ(win(pg, ix, p, "color"), "blue") << "the root .b is one step away and .a is two" << (outer_first ? " (outer rule first)" : " (inner rule first)");
  }
  Compiled c("@scope (.a) { p { color: red } } @scope (.a) { p { color: blue } }");
  Page pg;
  const int a = pg.t.add(0, "div", "", {"a"});
  const int p = pg.t.add(a, "p");
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, p, "color"), "blue");
}

TEST(CssSheetMutants, QuirksModeFoldsClassAndIdInTheBucketsAndNotOtherwise) {
  Compiled c(".foo { color: red } .FOO { margin-top: 1px } #bar { background-color: blue } #BAR { margin-left: 2px }");
  Page q;
  q.quirks = true;
  const int e = q.t.add(0, "p", "BAR", {"Foo"});
  q.open();
  Index xq(true);
  ASSERT_EQ(xq.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(xq.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(q, xq, e, "color"), "red");
  EXPECT_EQ(win(q, xq, e, "background-color"), "blue");
  EXPECT_EQ(win(q, xq, e, "margin-top"), "1px") << "the key of `.FOO` is folded when the rule is indexed";
  EXPECT_EQ(win(q, xq, e, "margin-left"), "2px") << "and the key of `#BAR`";
  Page n;
  const int f = n.t.add(0, "p", "BAR", {"Foo"});
  n.open();
  Index xn;
  ASSERT_EQ(xn.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(xn.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(n, xn, f, "color"), "-");
  EXPECT_EQ(win(n, xn, f, "background-color"), "-");
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  EXPECT_EQ(win(n, xq, f, "color", nullptr, &st), std::string("!") + gtext_css_status_string(GTEXT_CSS_E_INVALID)) << "a context that folds otherwise than the index does is refused";
  EXPECT_EQ(st, GTEXT_CSS_E_INVALID);
}

TEST(CssSheetMutants, HostContextClimbsTheFlatTreeThroughShadowRootsAndSlots) {
  /* Scoping 1: `:host-context()` looks at the host and its ancestors in the flat tree: across a shadow root to its host, and from a slotted element to its slot. */
  Compiled inner(":host-context(.ctx) { color: red } :host-context(.dd) { background-color: blue } :host-context(.sl) { margin-top: 5px } :host-context(.nope) { margin-left: 1px }");
  ASSERT_EQ(inner.st, GTEXT_CSS_OK);
  Page pg;
  const int h0 = pg.t.add(0, "x-outer", "", {"ctx"});
  const int s0 = pg.t.attach_shadow(h0);
  const int d = pg.t.add_in_shadow(s0, "div", "", {"dd"});
  const int h1 = pg.t.add(d, "x-inner");
  const int s1 = pg.t.attach_shadow(h1);
  const int d2 = pg.t.add(d, "div", "", {"sl"});
  const int slot = pg.t.add(d2, "slot");
  const int light = pg.t.add(h0, "x-slotted");
  pg.t.e[light].slot = slot;
  const int s2 = pg.t.attach_shadow(light);
  const int i1 = pg.t.add_in_shadow(s1, "span");
  const int i2 = pg.t.add_in_shadow(s2, "span");
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(inner, GTEXT_CSS_ORIGIN_AUTHOR, s1, 1), GTEXT_CSS_OK);
  ASSERT_EQ(ix.add(inner, GTEXT_CSS_ORIGIN_AUTHOR, s2, 2), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  (void)i1;
  (void)i2;
  EXPECT_EQ(win(pg, ix, h1, "color"), "red") << "h1 is inside the shadow tree of x-outer, whose host has .ctx";
  EXPECT_EQ(win(pg, ix, h1, "background-color"), "blue") << "its parent is .dd";
  EXPECT_EQ(win(pg, ix, h1, "margin-top"), "-") << "h1 is not under .sl";
  EXPECT_EQ(win(pg, ix, light, "margin-top"), "5px") << "the slotted element's flat parent is the slot, inside .sl";
  EXPECT_EQ(win(pg, ix, light, "background-color"), "blue") << "and above that .dd";
  EXPECT_EQ(win(pg, ix, light, "margin-left"), "-");
}

TEST(CssSheetMutants, PartMatchesAtEveryHostOfTheChain) {
  /* Shadow Parts 1: an element exported through several shadow trees is a part of each host that names it; the rule of the sheet that owns the outermost reaches it. */
  Compiled doc("x-outer::part(label) { color: red } x-mid::part(label) { background-color: blue }");
  ASSERT_EQ(doc.st, GTEXT_CSS_OK);
  Page pg;
  const int outer = pg.t.add(0, "x-outer");
  const int s_outer = pg.t.attach_shadow(outer);
  const int mid = pg.t.add_in_shadow(s_outer, "x-mid");
  const int s_mid = pg.t.attach_shadow(mid);
  const int part = pg.t.add_in_shadow(s_mid, "span");
  pg.t.e[part].parts.insert({mid, "label"});
  pg.t.e[part].parts.insert({outer, "label"});
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(doc), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, part, "color"), "red") << "the part of the outer host, two shadow trees up";
  EXPECT_EQ(win(pg, ix, part, "background-color"), "-") << "x-mid is in the outer shadow tree, which the document's sheet does not reach";
}

TEST(CssSheetMutants, TheLangTagKeepsSubtagsUpToEightCharacters) {
  /* RFC 5646: a subtag is one to eight alphanumerics, and a range of a longer one is not well-formed (RFC 4647, 2.2): it matches nothing, not even the tag that spells it. */
  Compiled c("p:lang(\"en-abcdefgh\") { color: red } p:lang(\"en-abcdefghi\") { background-color: blue }");
  Page pg;
  const int e = pg.t.add(0, "p");
  pg.t.e[e].lang = "en-abcdefgh";
  pg.t.e[e].has_lang = true;
  const int e2 = pg.t.add(0, "p");
  pg.t.e[e2].lang = "en-abcdefghi";
  pg.t.e[e2].has_lang = true;
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, e, "color"), "red");
  EXPECT_EQ(win(pg, ix, e, "background-color"), "-");
  EXPECT_EQ(win(pg, ix, e2, "background-color"), "-");
}

TEST(CssSheetMutants, TheLimitOfAnOuterScopeAppliesToTheSubjectOfAnInnerOne) {
  /* Cascade 6, 2.3: the subject is in the outer scope only if no limit of it lies between the outer root and the subject, wherever the inner root is. */
  Compiled c("@scope (.o) to (.lim) { @scope (.i) { p { color: red } } }");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  Page pg;
  const int o = pg.t.add(0, "div", "", {"o"});
  const int lim = pg.t.add(o, "div", "", {"lim"});
  const int i = pg.t.add(lim, "div", "", {"i"});
  const int behind = pg.t.add(i, "p");
  const int i2 = pg.t.add(o, "div", "", {"i"});
  const int inside = pg.t.add(i2, "p");
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, behind, "color"), "-") << "the outer limit .lim lies between .o and the p";
  EXPECT_EQ(win(pg, ix, inside, "color"), "red");
}

TEST(CssSheetMutants, APseudoElementIsAskedForByItsNameAndNoOtherOneAnswers) {
  Compiled c("p::before { color: red } p::after { margin-top: 1px } p::first-line { margin-left: 2px } p { padding-left: 3px }");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  Page pg;
  const int p = pg.t.add(0, "p");
  pg.open();
  Index ix;
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, p, "color", "::before"), "red");
  EXPECT_EQ(win(pg, ix, p, "margin-top", "::before"), "-");
  EXPECT_EQ(win(pg, ix, p, "color", "::after"), "-");
  EXPECT_EQ(win(pg, ix, p, "margin-top", "::after"), "1px");
  EXPECT_EQ(win(pg, ix, p, "margin-left", "::marker"), "-");
  EXPECT_EQ(win(pg, ix, p, "color"), "-") << "the element itself has none of the pseudo-elements' rules";
  EXPECT_EQ(win(pg, ix, p, "padding-left"), "3px");
  EXPECT_EQ(win(pg, ix, p, "padding-left", "::before"), "-");
}
