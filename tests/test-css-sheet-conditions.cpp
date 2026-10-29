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
 * Style sheets that keep their conditions as data (css_sheet.h): the condition nodes, `@when` and `@else`, the layers of the groups that do and do not apply, the
 * conditions of an `@import` and `scope()`, and an index built for one conditions object. The rules are quoted from Conditional 3 and 5, Cascade 5 and 6 as served 2026-10-28.
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

struct Compiled {
  GTEXT_CSS_Sheet * s = nullptr;
  GTEXT_CSS_Sheet_Result res{};
  GTEXT_CSS_Status st = GTEXT_CSS_E_INTERNAL;
  explicit Compiled(const std::string & css, const GTEXT_CSS_Sheet_Options * o = nullptr) {
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

/// A conditions object for a screen (or print medium) of the given width.
struct Conds {
  GTEXT_CSS_Conditions * c = nullptr;
  explicit Conds(double width = 1280, bool print = false) {
    GTEXT_CSS_Media_Environment env = gtext_css_media_environment_default(nullptr);
    env.viewport.width = width;
    env.medium = print ? GTEXT_CSS_MEDIUM_PRINT : GTEXT_CSS_MEDIUM_SCREEN;
    GTEXT_CSS_Conditions_Options o = gtext_css_conditions_options_default();
    o.media = &env;
    EXPECT_EQ(gtext_css_conditions_new(R(), &o, &c), GTEXT_CSS_OK);
  }
  Conds(const Conds &) = delete;
  Conds & operator=(const Conds &) = delete;
  ~Conds() {
    gtext_css_conditions_free(c);
  }
};

struct Index {
  GTEXT_CSS_Rule_Index * x = nullptr;
  GTEXT_CSS_Rule_Index_Result res{};
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  explicit Index(const GTEXT_CSS_Conditions * conditions) {
    GTEXT_CSS_Rule_Index_Options o = gtext_css_rule_index_options_default();
    o.html_document = true;
    o.conditions = conditions;
    EXPECT_EQ(gtext_css_rule_index_new(R(), &o, &x), GTEXT_CSS_OK);
  }
  Index(const Index &) = delete;
  Index & operator=(const Index &) = delete;
  ~Index() {
    gtext_css_rule_index_free(x);
  }
  GTEXT_CSS_Status add(Compiled & c, GTEXT_CSS_Element_Handle implicit = nullptr) {
    EXPECT_EQ(c.st, GTEXT_CSS_OK);
    GTEXT_CSS_Sheet_Registration reg;
    std::memset(&reg, 0, sizeof reg);
    reg.origin = GTEXT_CSS_ORIGIN_AUTHOR;
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
  void open() {
    GTEXT_CSS_Selector_Match_Options o = gtext_css_selector_match_options_default();
    o.html_document = true;
    ASSERT_EQ(gtext_css_selector_match_context_new(&t.table, &o, &ctx), GTEXT_CSS_OK);
  }
  ~Page() {
    gtext_css_selector_match_context_free(ctx);
  }
};

std::string win(Page & pg, Index & ix, int el, const std::string & prop) {
  Set set(nullptr);
  const GTEXT_CSS_Status s = gtext_css_rules_collect(ix.x, pg.ctx, synth::Tree::H(el), nullptr, set.s, nullptr, nullptr);
  if (s != GTEXT_CSS_OK) {
    return std::string("!") + gtext_css_status_string(s);
  }
  Out o(set);
  return o.value(prop);
}

/// The color of a `p` in a page with one `p`, for a sheet and a conditions object.
std::string color_of(const std::string & css, const GTEXT_CSS_Conditions * c) {
  Compiled sheet(css);
  EXPECT_EQ(sheet.st, GTEXT_CSS_OK) << css;
  Page pg;
  const int p = pg.t.add(0, "p");
  pg.open();
  Index ix(c);
  EXPECT_EQ(ix.add(sheet), GTEXT_CSS_OK) << css;
  EXPECT_EQ(ix.freeze(), GTEXT_CSS_OK);
  return win(pg, ix, p, "color");
}

}  // namespace

TEST(CssSheetConditions, TheSameSheetServesTwoEnvironmentsAndIsNotTouched) {
  /* Cascade 5, 6.4.3 and Conditional 3: a conditional group rule applies when its condition is true; the sheet keeps the condition, so that a resize is a new conditions object and a new index. */
  Compiled c("p { color: black } @media (min-width: 600px) { p { color: red } } @media (max-width: 600px) { p { color: blue } }");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  const std::string before = c.dump();
  EXPECT_EQ(gtext_css_sheet_condition_count(c.s), 2u);
  Page pg;
  const int p = pg.t.add(0, "p");
  pg.open();
  Conds narrow(400), wide(900);
  Index a(narrow.c), b(wide.c);
  ASSERT_EQ(a.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(a.freeze(), GTEXT_CSS_OK);
  ASSERT_EQ(b.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(b.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, a, p, "color"), "blue");
  EXPECT_EQ(win(pg, b, p, "color"), "red");
  EXPECT_EQ(gtext_css_rule_index_record_count(a.x), 2u);
  EXPECT_EQ(gtext_css_rule_index_record_count(b.x), 2u);
  EXPECT_EQ(c.dump(), before) << "the sheet is untouched";
  EXPECT_NE(before.find("cond 1 media "), std::string::npos);
  EXPECT_NE(before.find("(min-width: 600px)"), std::string::npos);
}

TEST(CssSheetConditions, ANodeIsEvaluatedOnceWhateverNumberOfRecordsItHas) {
  /* The index evaluates each node once, as the sheet is added: a node shared by m records, and n nested nodes, cost O(nodes). */
  std::string css;
  for (int i = 0; i < 200; i++) {
    css += "@media (min-width: " + std::to_string(i) + "px) { ";
  }
  css += "p { color: red } .a { color: blue } .b { color: green } .c { color: black } ";
  for (int i = 0; i < 200; i++) {
    css += "} ";
  }
  GTEXT_CSS_Sheet_Options so = gtext_css_sheet_options_default();
  so.max_rule_depth = 400;
  Compiled c(css, &so);
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_sheet_condition_count(c.s), 200u);
  Conds wide(1280);
  Index ix(wide.c);
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_rule_index_record_count(ix.x), 4u);
}

TEST(CssSheetConditions, ConditionalRulesNestInAStyleRuleAndDeclarationsDirectlyInOneAreTheImpliedRule) {
  /* Nesting 1: `.a { @media (...) { color: red; & b { } } }`: a declaration directly in a nested group rule is `& { }`. */
  Compiled c(".a { color: black; @media (min-width: 600px) { color: red; & b { color: blue } } @supports (display: nonesuch) { color: green } }");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  Page pg;
  const int a = pg.t.add(0, "div", "", {"a"});
  const int b = pg.t.add(a, "b");
  pg.open();
  Conds narrow(400), wide(900);
  Index n(narrow.c), w(wide.c);
  ASSERT_EQ(n.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(n.freeze(), GTEXT_CSS_OK);
  ASSERT_EQ(w.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(w.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, n, a, "color"), "black");
  EXPECT_EQ(win(pg, w, a, "color"), "red");
  EXPECT_EQ(win(pg, n, b, "color"), "-");
  EXPECT_EQ(win(pg, w, b, "color"), "blue");
}

TEST(CssSheetConditions, ASupportsConditionIsEvaluatedByTheConditionsObject) {
  Conds c;
  EXPECT_EQ(color_of("p { color: black } @supports (display: grid) { p { color: red } }", c.c), "red");
  EXPECT_EQ(color_of("p { color: black } @supports (display: nonesuch) { p { color: red } }", c.c), "black");
  EXPECT_EQ(color_of("p { color: black } @supports not (display: nonesuch) { p { color: red } }", c.c), "red");
  EXPECT_EQ(color_of("p { color: black } @supports display: grid { p { color: red } }", c.c), "black") << "no parentheses: the rule never applies";
}

TEST(CssSheetConditions, AnInvalidConditionIsANodeThatNeverAppliesWithADiagnostic) {
  Conds c;
  Compiled bad("p { color: black } @media only (min-width: 5px) { p { color: red } } @supports garbage { p { color: blue } } @document url(x) { p { color: green } } @starting-style x { p { color: orange } }");
  ASSERT_EQ(bad.st, GTEXT_CSS_OK);
  EXPECT_GE(gtext_css_sheet_diagnostic_total(bad.s), 4u);
  Page pg;
  const int p = pg.t.add(0, "p");
  pg.open();
  Index ix(c.c);
  ASSERT_EQ(ix.add(bad), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, p, "color"), "black");
}

TEST(CssSheetWhen, WhenAndElseFormAChainAndTheFirstTrueRuleWins) {
  /* Conditional 5, 4: "If one of them is true, the conditions of all following conditional group rules in the chain evaluate to false, regardless of their stated condition." */
  const std::string css = "@when media(width >= 600px) { p { color: red } } @else supports(display: grid) { p { color: blue } } @else { p { color: green } }";
  Conds wide(900), narrow(400);
  EXPECT_EQ(color_of(css, wide.c), "red");
  EXPECT_EQ(color_of(css, narrow.c), "blue") << "the second is true; the bare @else is not reached";
  const std::string none = "@when media(width >= 600px) { p { color: red } } @else supports(display: nonesuch) { p { color: blue } } @else { p { color: green } }";
  EXPECT_EQ(color_of(none, narrow.c), "green") << "a bare @else is always true when no earlier rule is";
  EXPECT_EQ(color_of(none, wide.c), "red");
  /* the example of the draft: only one of A, B and C applies, however the conditions overlap */
  const std::string over = "@when media(width >= 100px) { p { color: red } } @else media(width >= 50px) { p { background-color: blue } } @else { p { margin-top: 1px } }";
  Compiled c(over);
  Page pg;
  const int p = pg.t.add(0, "p");
  pg.open();
  Index ix(wide.c);
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, p, "color"), "red");
  EXPECT_EQ(win(pg, ix, p, "background-color"), "-") << "exactly one of the chain applies";
  EXPECT_EQ(win(pg, ix, p, "margin-top"), "-");
}

TEST(CssSheetWhen, BooleanAlgebraAndTheLeaves) {
  Conds wide(900), narrow(400);
  EXPECT_EQ(color_of("@when media(width >= 600px) and supports(display: grid) { p { color: red } }", wide.c), "red");
  EXPECT_EQ(color_of("@when media(width >= 600px) and supports(display: grid) { p { color: red } }", narrow.c), "-");
  EXPECT_EQ(color_of("@when media(width >= 600px) or supports(display: nonesuch) { p { color: red } }", wide.c), "red");
  EXPECT_EQ(color_of("@when not media(width >= 600px) { p { color: red } }", narrow.c), "red");
  EXPECT_EQ(color_of("@when (media(width >= 600px) and (supports(display: grid))) { p { color: red } }", wide.c), "red");
  EXPECT_EQ(color_of("@when media(width >= 600px) and media(width < 800px) or supports(display: grid) { p { color: red } }", wide.c), "-") << "and beside or without parentheses is invalid";
  EXPECT_EQ(color_of("@when garbage { p { color: red } }", wide.c), "-");
  EXPECT_EQ(color_of("@when media(foo: bar) { p { color: red } }", wide.c), "-") << "an unknown media feature is unknown, so false";
  EXPECT_EQ(color_of("@when not media(foo: bar) { p { color: red } }", wide.c), "-") << "and not unknown is unknown";
  EXPECT_EQ(color_of("@when font-tech(color-COLRv1) { p { color: red } } @else { p { color: blue } }", wide.c), "blue") << "a zeroed policy supports no font technology";
  EXPECT_EQ(color_of("@when supports(display: grid) { p { color: red } }", wide.c), "red");
  EXPECT_EQ(color_of("@when supports((display: grid) and (color: red)) { p { color: red } }", wide.c), "red");
}

TEST(CssSheetWhen, AnElseThatIsNotInAChainIsInvalid) {
  /* "An @else rule that is not part of a conditional rule chain is invalid and must be ignored." "There cannot be anything between the successive conditional group rules other than whitespace and/or comments." */
  Conds wide(900);
  EXPECT_EQ(color_of("@else { p { color: red } }", wide.c), "-");
  EXPECT_EQ(color_of("@media print { p { color: blue } } @else { p { color: red } }", wide.c), "-") << "a chain begins with a conditional group rule other than @else: @media is not one of the chain's";
  EXPECT_EQ(color_of("@when media(width > 5000px) { p { color: blue } } p { color: black } @else { p { color: red } }", wide.c), "black") << "a style rule breaks the chain";
  EXPECT_EQ(color_of("@when media(width > 5000px) { p { color: blue } } /* a comment */ @else { p { color: red } }", wide.c), "red") << "a comment does not";
  Compiled c("@else { p { color: red } } p { color: black }");
  EXPECT_GE(gtext_css_sheet_diagnostic_total(c.s), 1u);
}

TEST(CssSheetLayers, AGroupThatDoesNotApplyDeclaresNothingAndALaterOccurrenceDeclaresAtItsOwnPlace) {
  /* Cascade 5, 6.4.3: the layers of a conditional group rule contribute only if the condition is true, or can differ by element. A layer first named in a group that is false and then in one that is true is declared
     at the second place. */
  Compiled c("@media (width > 9999px) { @layer x { p { color: red } } } @layer y { p { color: blue } } @media (width > 1px) { @layer x { p { background-color: green } } } @layer z;");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  Conds screen;
  Index ix(screen.c);
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  const GTEXT_CSS_Layer_Registry * lr = gtext_css_rule_index_layers(ix.x, 0);
  ASSERT_NE(lr, nullptr);
  ASSERT_EQ(gtext_css_layer_count(lr, GTEXT_CSS_ORIGIN_AUTHOR), 3u);
  const GTEXT_CSS_Layer_Id y = gtext_css_layer_find(lr, GTEXT_CSS_ORIGIN_AUTHOR, 0, "y", 1);
  const GTEXT_CSS_Layer_Id x = gtext_css_layer_find(lr, GTEXT_CSS_ORIGIN_AUTHOR, 0, "x", 1);
  const GTEXT_CSS_Layer_Id z = gtext_css_layer_find(lr, GTEXT_CSS_ORIGIN_AUTHOR, 0, "z", 1);
  ASSERT_NE(x, 0u);
  ASSERT_NE(y, 0u);
  ASSERT_NE(z, 0u);
  EXPECT_LT(y, x) << "x is declared at the second @media, after y";
  EXPECT_LT(x, z);
}

TEST(CssSheetLayers, AContainerGroupDeclaresItsLayersBecauseTheConditionDiffersByElement) {
  Compiled c("@container (width > 1px) { @layer x { p { color: red } } } @layer y;");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  Conds screen;
  Index ix(screen.c);
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  const GTEXT_CSS_Layer_Registry * lr = gtext_css_rule_index_layers(ix.x, 0);
  EXPECT_EQ(gtext_css_layer_count(lr, GTEXT_CSS_ORIGIN_AUTHOR), 2u);
  EXPECT_LT(gtext_css_layer_find(lr, GTEXT_CSS_ORIGIN_AUTHOR, 0, "x", 1), gtext_css_layer_find(lr, GTEXT_CSS_ORIGIN_AUTHOR, 0, "y", 1));
}

TEST(CssSheetLayers, ALayerInANestedStatementIsDeclaredWithItsAncestors) {
  Compiled c("@media (width > 1px) { @layer a.b; } @media (width > 9999px) { @layer c.d; } @layer e;");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  Conds screen;
  Index ix(screen.c);
  ASSERT_EQ(ix.add(c), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  const GTEXT_CSS_Layer_Registry * lr = gtext_css_rule_index_layers(ix.x, 0);
  EXPECT_EQ(gtext_css_layer_count(lr, GTEXT_CSS_ORIGIN_AUTHOR), 3u) << "a, a.b and e";
  EXPECT_NE(gtext_css_layer_find(lr, GTEXT_CSS_ORIGIN_AUTHOR, 0, "a.b", 3), 0u);
  EXPECT_EQ(gtext_css_layer_find(lr, GTEXT_CSS_ORIGIN_AUTHOR, 0, "c", 1), 0u);
}

TEST(CssSheetImport, TheRequestCarriesTheParsedConditionsAndTheCallbackMayDecline) {
  /* Cascade 5, 2.1 and Cascade 6: `@import <url> [ layer ] [ scope ] [ supports(...) ] <media-query-list>`: the conditions are parsed and handed over. */
  struct Seen {
    std::vector<std::string> urls;
    bool media_matches = false;
    bool supports = false;
    bool has_supports_condition = false;
    bool has_scope = false;
    std::string scope;
    GTEXT_CSS_Conditions * c = nullptr;
  } seen;
  Conds wide(900);
  seen.c = wide.c;
  GTEXT_CSS_Sheet_Options o = gtext_css_sheet_options_default();
  o.import = [](void * u, const GTEXT_CSS_Import_Request * rq) -> GTEXT_CSS_Sheet * {
    Seen * s = static_cast<Seen *>(u);
    s->urls.push_back(std::string(rq->url.data, rq->url.length));
    if (rq->media_list) {
      EXPECT_EQ(gtext_css_conditions_matches_list(s->c, rq->media_list, nullptr, &s->media_matches), GTEXT_CSS_OK);
    }
    s->has_supports_condition = rq->supports_condition != nullptr;
    if (rq->supports_condition) {
      GTEXT_CSS_Tristate t;
      EXPECT_EQ(gtext_css_conditions_evaluate_supports(s->c, rq->supports_condition, nullptr, &t), GTEXT_CSS_OK);
      s->supports = t == GTEXT_CSS_TRI_TRUE;
    }
    s->has_scope = rq->has_scope;
    s->scope = std::string(rq->scope.data ? rq->scope.data : "", rq->scope.length);
    return nullptr;
  };
  o.import_user = &seen;
  Compiled c("@import url(a.css) supports(display: grid) screen and (min-width: 600px); @import 'b.css' scope((.card) to (.lim)) layer(x); @import 'c.css' layer scope; @import 'd.css' supports(display: grid) layer(y) print;", &o);
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  ASSERT_EQ(seen.urls.size(), 4u);
  EXPECT_EQ(seen.urls[0], "a.css");
  EXPECT_EQ(gtext_css_sheet_condition_count(c.s), 2u) << "the first and the last have conditions of their own";
  GTEXT_CSS_Sheet_Condition sc;
  ASSERT_TRUE(gtext_css_sheet_condition(c.s, 1, &sc));
  EXPECT_EQ(sc.kind, GTEXT_CSS_SHEET_COND_IMPORT);
  EXPECT_NE(sc.list, nullptr);
  EXPECT_NE(sc.supports, nullptr);
  EXPECT_FALSE(sc.element_dependent);
  EXPECT_EQ(gtext_css_sheet_layer_count(c.s), 3u) << "x, the anonymous layer of `c.css`, and y: the any-order grammar finds `layer(y)` after `supports()`";
}

TEST(CssSheetImport, ImportsWithScopeWrapTheImportedRulesInAnImplicitScope) {
  /* Cascade 6: "@import url() scope((.scope))" wraps the imported style sheet in @scope (.scope). */
  Compiled lib("p { color: red } .x { color: green }");
  ASSERT_EQ(lib.st, GTEXT_CSS_OK);
  struct Give {
    GTEXT_CSS_Sheet * s;
  } g{lib.s};
  GTEXT_CSS_Sheet_Options o = gtext_css_sheet_options_default();
  o.import = [](void * u, const GTEXT_CSS_Import_Request *) { return gtext_css_sheet_retain(static_cast<Give *>(u)->s); };
  o.import_user = &g;
  Compiled top("@import url(lib.css) scope((.card) to (.limit)); .x { background-color: blue }", &o);
  ASSERT_EQ(top.st, GTEXT_CSS_OK);
  Page pg;
  const int card = pg.t.add(0, "div", "", {"card"});
  const int inside = pg.t.add(card, "p");
  const int limit = pg.t.add(card, "div", "", {"limit"});
  const int below = pg.t.add(limit, "p");
  const int outside = pg.t.add(0, "p");
  const int x_in = pg.t.add(card, "span", "", {"x"});
  const int x_out = pg.t.add(0, "span", "", {"x"});
  pg.open();
  Conds c;
  Index ix(c.c);
  ASSERT_EQ(ix.add(top), GTEXT_CSS_OK);
  ASSERT_EQ(ix.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ix, inside, "color"), "red");
  EXPECT_EQ(win(pg, ix, below, "color"), "-") << "below the scoping limit";
  EXPECT_EQ(win(pg, ix, outside, "color"), "-") << "outside the scoping root";
  EXPECT_EQ(win(pg, ix, x_in, "color"), "green");
  EXPECT_EQ(win(pg, ix, x_out, "color"), "-");
  EXPECT_EQ(win(pg, ix, x_out, "background-color"), "blue") << "the importer's own rule is not scoped";
  EXPECT_EQ(win(pg, ix, card, "color"), "-") << "the scoping root is not its own descendant";
}

TEST(CssSheetImport, AnImportThatIsFalseDropsItsSheet) {
  Compiled lib("p { color: red }");
  struct Give {
    GTEXT_CSS_Sheet * s;
  } g{lib.s};
  GTEXT_CSS_Sheet_Options o = gtext_css_sheet_options_default();
  o.import = [](void * u, const GTEXT_CSS_Import_Request *) { return gtext_css_sheet_retain(static_cast<Give *>(u)->s); };
  o.import_user = &g;
  Compiled top("@import url(lib.css) (min-width: 600px); @import url(lib.css) layer(a) supports(display: nonesuch); p { background-color: blue }", &o);
  ASSERT_EQ(top.st, GTEXT_CSS_OK);
  Page pg;
  const int p = pg.t.add(0, "p");
  pg.open();
  Conds wide(900), narrow(400);
  Index w(wide.c), n(narrow.c);
  ASSERT_EQ(w.add(top), GTEXT_CSS_OK);
  ASSERT_EQ(w.freeze(), GTEXT_CSS_OK);
  ASSERT_EQ(n.add(top), GTEXT_CSS_OK);
  ASSERT_EQ(n.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, w, p, "color"), "red");
  EXPECT_EQ(win(pg, n, p, "color"), "-");
  EXPECT_EQ(gtext_css_rule_index_sheet_count(w.x), 2u);
  EXPECT_EQ(gtext_css_rule_index_sheet_count(n.x), 1u);
  EXPECT_EQ(gtext_css_layer_count(gtext_css_rule_index_layers(w.x, 0), GTEXT_CSS_ORIGIN_AUTHOR), 0u) << "layer a is behind a false supports()";
}

TEST(CssSheetConditions, TheNumberOfConditionNodesIsLimited) {
  GTEXT_CSS_Sheet_Options o = gtext_css_sheet_options_default();
  o.max_conditions = 3;
  Compiled c("@media print { p { color: red } } @media screen { p { color: red } } @media all { p { color: red } } @media (width) { p { color: red } }", &o);
  EXPECT_EQ(c.st, GTEXT_CSS_E_LIMIT_CONDITIONS);
  EXPECT_STREQ(c.res.name, "max_conditions");
}

TEST(CssSheetConditions, TheActiveRuleFilterFeedsTheWalkersThatStayFilters) {
  /* Cascade 5, 6.4.3: the layer walker asks of each conditional rule whether it applies; @container and @starting-style are true because they depend on the element. */
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_stylesheet, "@media print { @layer a { } } @media screen { @layer b { } } @container (width > 1px) { @layer c { } } @supports (display: nonesuch) { @layer d { } }");
  Conds screen;
  GTEXT_CSS_Layer_Options lopt = gtext_css_layer_options_default();
  GTEXT_CSS_Layer_Registry * reg = nullptr;
  ASSERT_EQ(gtext_css_layer_registry_new(&lopt, &reg), GTEXT_CSS_OK);
  GTEXT_CSS_Layer_Collect_Options lo;
  std::memset(&lo, 0, sizeof lo);
  lo.user = screen.c;
  lo.active = gtext_css_conditions_active_rule;
  GTEXT_CSS_Layer_Result lr;
  ASSERT_EQ(gtext_css_cascade_collect_layers(reg, GTEXT_CSS_ORIGIN_AUTHOR, GTEXT_CSS_LAYER_NONE, p.tree, &lo, &lr), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_layer_count(reg, GTEXT_CSS_ORIGIN_AUTHOR), 2u);
  EXPECT_EQ(gtext_css_layer_find(reg, GTEXT_CSS_ORIGIN_AUTHOR, 0, "a", 1), 0u);
  EXPECT_NE(gtext_css_layer_find(reg, GTEXT_CSS_ORIGIN_AUTHOR, 0, "b", 1), 0u);
  EXPECT_NE(gtext_css_layer_find(reg, GTEXT_CSS_ORIGIN_AUTHOR, 0, "c", 1), 0u);
  EXPECT_EQ(gtext_css_layer_find(reg, GTEXT_CSS_ORIGIN_AUTHOR, 0, "d", 1), 0u);
  gtext_css_layer_registry_free(reg);
}

TEST(CssSheetConditions, TheOrderSheetsAreAddedInDecidesNothingButTheOrderOfAppearance) {
  /* The winners depend on the order of appearance the sheets were added in and on nothing else: a condition does not reorder. */
  Compiled a("@media (min-width: 600px) { p { color: red } } p { background-color: blue }");
  Compiled b("p { color: green } @when media(width >= 600px) { p { margin-top: 1px } } @else { p { margin-top: 2px } }");
  Page pg;
  const int p = pg.t.add(0, "p");
  pg.open();
  Conds wide(900);
  Index ab(wide.c), ba(wide.c);
  ASSERT_EQ(ab.add(a), GTEXT_CSS_OK);
  ASSERT_EQ(ab.add(b), GTEXT_CSS_OK);
  ASSERT_EQ(ab.freeze(), GTEXT_CSS_OK);
  ASSERT_EQ(ba.add(b), GTEXT_CSS_OK);
  ASSERT_EQ(ba.add(a), GTEXT_CSS_OK);
  ASSERT_EQ(ba.freeze(), GTEXT_CSS_OK);
  EXPECT_EQ(win(pg, ab, p, "color"), "green") << "b is the later";
  EXPECT_EQ(win(pg, ba, p, "color"), "red");
  EXPECT_EQ(win(pg, ab, p, "margin-top"), "1px");
  EXPECT_EQ(win(pg, ba, p, "margin-top"), "1px");
  EXPECT_EQ(win(pg, ab, p, "background-color"), "blue");
}

TEST(CssSheetConditions, TheDumpRecordsTheNodesTheirChainsAndTheLayerOccurrences) {
  Compiled c("@media (min-width: 600px) { @layer a; p { color: red } } @when media(width > 1px) { p { color: blue } } @else { p { color: green } }");
  ASSERT_EQ(c.st, GTEXT_CSS_OK);
  const std::string d = c.dump();
  EXPECT_NE(d.find("cond 1 media (min-width: 600px)\n"), std::string::npos) << d;
  EXPECT_NE(d.find("cond 2 when media(width > 1px)\n"), std::string::npos) << d;
  EXPECT_NE(d.find("cond 3 else prev=2\n"), std::string::npos) << d;
  EXPECT_NE(d.find("event layer a cond=1\n"), std::string::npos) << d;
  EXPECT_NE(d.find(" cond=1\n"), std::string::npos);
}
