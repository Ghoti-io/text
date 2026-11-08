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
 * The definitions of custom functions (css_custom_function.h): CSS Functions and Mixins 1, "The @function Rule", "The Function Prelude", "The Function Body". The prelude
 * vectors are those of the pinned web-platform-tests page css-mixins/functions/at-function-parsing.html.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_registry_test_support.h"
#include "markup_test_support.h"

namespace {

/// A registry, the trees of some sheets (which the set borrows from) and the set the rules of the sheets define.
struct Defs {
  reg::Owned registry;
  std::vector<GTEXT_CSS_Tree *> trees;
  GTEXT_CSS_Function_Set * set = nullptr;
  GTEXT_CSS_Status status = GTEXT_CSS_OK;

  static bool always(void *, const GTEXT_CSS_Rule *) {
    return true;
  }

  explicit Defs(const std::vector<std::string> & sheets, bool with_active = true, const GTEXT_Allocator * alloc = nullptr, bool own_rules = false) {
    reg::Builder b;
    EXPECT_EQ(b.freeze(registry), GTEXT_CSS_OK);
    GTEXT_CSS_Function_Options o = gtext_css_function_options_default();
    o.allocator = alloc;
    o.own_rules = own_rules;
    if (with_active) {
      o.active = always;
    }
    status = gtext_css_function_set_new(registry.r, &o, &set);
    for (const std::string & css : sheets) {
      if (status != GTEXT_CSS_OK) {
        break;
      }
      GTEXT_CSS_Tree * tree = nullptr;
      status = gtext_css_parse_stylesheet(css.data(), css.size(), nullptr, &tree, nullptr);
      if (status != GTEXT_CSS_OK) {
        break;
      }
      trees.push_back(tree);
      GTEXT_CSS_Function_Result res;
      status = gtext_css_function_collect(set, tree, &res);
    }
  }
  /// Free the trees the set was given, as a caller that does not keep them would (the set then must not read them).
  void release_trees() {
    for (GTEXT_CSS_Tree * t : trees) {
      gtext_css_tree_free(t);
    }
    trees.clear();
  }
  Defs(const Defs &) = delete;
  Defs & operator=(const Defs &) = delete;
  ~Defs() {
    gtext_css_function_set_free(set);
    for (GTEXT_CSS_Tree * t : trees) {
      gtext_css_tree_free(t);
    }
  }
  size_t count() const {
    return gtext_css_function_count(set);
  }
  bool has(const std::string & name) const {
    GTEXT_CSS_Function_Info info;
    return gtext_css_function_find(set, name.data(), name.size(), &info);
  }
  /// The reasons of the diagnostics, in order.
  std::string reasons() const {
    std::string out;
    GTEXT_CSS_Function_Diagnostic d;
    for (size_t i = 0; gtext_css_function_diagnostic(set, i, &d); i++) {
      out += (i ? " " : "") + std::string(gtext_css_function_reason_name(d.reason));
    }
    return out;
  }
};

/// Whether a single rule with this prelude defines a function (the page's `${rule_without_block} {}`).
bool valid_prelude(const std::string & prelude) {
  Defs d({prelude + " {}"});
  EXPECT_EQ(d.status, GTEXT_CSS_OK) << prelude;
  return d.count() == 1;
}

} // namespace

/* The valid and the invalid preludes of the pinned page at-function-parsing.html, one for one. */
TEST(CssFunctionSet, TheValidPreludesOfThePinnedPageDefineAFunction) {
  for (const char * p : {
           "@function --foo()", "@function --foo( )", "@function --foo(--x)", "@function --foo( --x )",
           "@function --foo(--x auto)", "@function --foo(--x <angle>)", "@function --foo(--x <color>)", "@function --foo(--x <custom-ident>)", "@function --foo(--x <image>)",
           "@function --foo(--x <integer>)", "@function --foo(--x <length>)", "@function --foo(--x <length-percentage>)", "@function --foo(--x <number>)",
           "@function --foo(--x <percentage>)", "@function --foo(--x <resolution>)", "@function --foo(--x <string>)", "@function --foo(--x <time>)", "@function --foo(--x <url>)",
           "@function --foo(--x <transform-function>)", "@function --foo(--x <transform-list>)",
           "@function --foo(--x type(auto))", "@function --foo(--x type(<length>))", "@function --foo(--x type(<length> | auto))", "@function --foo(--x type(none | auto))",
           "@function --foo(--x type(*))",
           "@function --foo(--x, --y)", "@function --foo(--x, --y, --z)", "@function --foo(--x <length>, --y, --z)", "@function --foo(--x, --y <number>, --z <angle>)",
           "@function --foo(--x : 10px)", "@function --foo(--x type(*): 10px)", "@function --foo(--x <length>: 10px)", "@function --foo(--x <length>: 10px, --y)",
           "@function --foo(--x, --y <length>: 10px)", "@function --foo(--x type(<length> | auto): auto)", "@function --foo(--x type(<length> | auto) : auto)",
           "@function --foo(--x:1px, --y, --z:2px)",
           "@function --foo(--x: var(--y, 10px))", "@function --foo(--x <color>: rgb(1, 2, 3))", "@function --foo(--x: var(--y, 10px), --z: 2px)",
           "@function --foo(--x <length>: clamp(1px, 2px, 3px))", "@function --foo(--x: {1px, 2px})", "@function --foo(--x: --bar(1px, 2px))",
           "@function --foo(--x <length>#)", "@function --foo(--x <length>+)", "@function --foo(--x type(<length>+))", "@function --foo(--x <transform-function>#)",
           "@function --foo(--x <transform-function>+)",
           "@function --foo(--x) returns type(*)", "@function --foo(--x) returns <length>", "@function --foo(--x) returns <length>+", "@function --foo(--x) returns type(<length>)",
           "@function --foo(--x) returns type(<length> | auto)", "@function --foo(--x) returns type(foo | bar)"}) {
    EXPECT_TRUE(valid_prelude(p)) << p;
  }
}

TEST(CssFunctionSet, TheInvalidPreludesOfThePinnedPageDefineNothing) {
  for (const char * p : {
           "@function --foo ()", "@function --foo (--x)",
           "@function --foo(--x: 10px !important)", "@function --foo(--x: var(--y, 10px)",
           "@function --foo(--x <length>: 10deg)", "@function --foo(--x <angle>: 10px)", "@function --foo(--x <color>: 10px)", "@function --foo(--x type(<color>+): red 5)",
           "@function --foo(--x type(auto | none): thing)",
           "@function --foo(--x <transform-list>#)", "@function --foo(--x <transform-list>+)",
           "@function --foo(--x *)", "@function --foo(--x !)", "@function --foo(--x 50px)", "@function --foo(--x <length> | auto)", "@function --foo(--x none | auto)",
           "@function --foo(--x <dino>)", "@function --foo(!)", "@function --foo(--x: !)", "@function --foo(--x type(!))", "@function --foo(,)", "@function --foo(,,,)",
           "@function --foo(--x, ;)", "@function --foo(;)", "@function --foo(])", "@function --foo(, --x])",
           "@function --foo(--x) !", "@function --foo(--x) ! <length>", "@function --foo(--x) returns <length>!", "@function --foo(--x) length", "@function --foo(--x) returns",
           "@function --foo(--x) returns ", "@function --foo(--x) returns *", "@function --foo(--x) returns <transform-list>#", "@function --foo(--x) returns <transform-list>+",
           "@function --foo(--x) returns auto | none", "@function --foo(--x): <length>", "@function --foo(--x): length", "@function --foo(--x) returneth <length>"}) {
    EXPECT_FALSE(valid_prelude(p)) << p;
  }
}

/* The pin of 2026-11-07 (Mixins 1, 2.1): "<@function> = @function <function-token> <function-parameter>#? ,? )", "<default-value> = <declaration-value>?", "Whether a default
   value is provided or omitted is determined by the presence or absence of the colon; an empty default value (like `@function --x(--arg:){}`) is perfectly valid." and "A final
   trailing comma is allowed after the arguments". A comma that is not after an argument stays invalid (the page at-function-parsing.html has `(,)` and `(,,,)` invalid). */
TEST(CssFunctionSet, AFinalTrailingCommaAndAnEmptyDefaultAreValid) {
  for (const char * p : {"@function --foo(--x,)", "@function --foo(--x, --y,)", "@function --foo( --x , )", "@function --foo(--x:)", "@function --foo(--x: , --y)",
                          "@function --foo(--x type(*):)", "@function --foo(--x:,)", "@function --foo(--x <length>: 1px,) returns <length>"}) {
    EXPECT_TRUE(valid_prelude(p)) << p;
  }
  for (const char * p : {"@function --foo(--x,,)", "@function --foo(,--x)", "@function --foo(--x <length>:)", "@function --foo(--x <color>:,)", "@function --foo(--x,;)"}) {
    EXPECT_FALSE(valid_prelude(p)) << p;
  }
}

/* "The <function-token> production must start with two dashes (U+002D HYPHEN-MINUS), similar to <dashed-ident>, or else the definition is invalid." */
TEST(CssFunctionSet, AFunctionNameWithoutTwoDashesIsInvalidAndReported) {
  Defs d({"@function foo(--x) { result: 1px }", "@function -foo() {}", "@function --() {}"});
  EXPECT_EQ(d.count(), 0u);
  EXPECT_EQ(d.reasons(), "bad-name bad-name bad-name");
}

/* "If the function parameters contain the same <custom-property-name> more than once, then the @function rule is invalid." */
TEST(CssFunctionSet, ADuplicateParameterNameMakesTheRuleInvalid) {
  Defs d({"@function --f(--a, --b, --a) { result: 1px }"});
  EXPECT_EQ(d.count(), 0u);
  EXPECT_EQ(d.reasons(), "duplicate-parameter");
  Defs ok({"@function --f(--a, --A) { result: 1px }"});
  EXPECT_EQ(ok.count(), 1u) << "custom property names are case-sensitive";
}

/* The prelude is kept: the name, each parameter with its type and its default, the return type. */
TEST(CssFunctionSet, TheDefinitionKeepsWhatThePreludeSays) {
  Defs d({"@function --f(--a <length>, --b type(<number> | auto): auto, --c: {1px, 2px}) returns <length>+ { result: 1px; }"});
  ASSERT_EQ(d.count(), 1u);
  GTEXT_CSS_Function_Info info;
  ASSERT_TRUE(gtext_css_function_find(d.set, "--f", 3, &info));
  EXPECT_EQ(std::string(info.name.data, info.name.length), "--f");
  EXPECT_EQ(info.parameter_count, 3u);
  EXPECT_TRUE(info.has_returns);
  EXPECT_EQ(std::string(info.returns.data, info.returns.length), "<length>+");
  EXPECT_TRUE(info.has_result);
  GTEXT_CSS_Function_Parameter p;
  ASSERT_TRUE(gtext_css_function_parameter(d.set, "--f", 3, 0, &p));
  EXPECT_EQ(std::string(p.name.data, p.name.length), "--a");
  EXPECT_TRUE(p.has_type);
  EXPECT_EQ(std::string(p.type.data, p.type.length), "<length>");
  EXPECT_FALSE(p.has_default);
  ASSERT_TRUE(gtext_css_function_parameter(d.set, "--f", 3, 1, &p));
  EXPECT_EQ(std::string(p.type.data, p.type.length), "<number> | auto");
  EXPECT_TRUE(p.has_default);
  EXPECT_EQ(std::string(p.default_value.data, p.default_value.length), "auto");
  ASSERT_TRUE(gtext_css_function_parameter(d.set, "--f", 3, 2, &p));
  EXPECT_FALSE(p.has_type);
  EXPECT_EQ(std::string(p.default_value.data, p.default_value.length), "{1px, 2px}");
  EXPECT_FALSE(gtext_css_function_parameter(d.set, "--f", 3, 3, &p));
}

/* "If no result descriptor exists, the function is valid, but always returns the guaranteed-invalid value." "Unknown descriptors are invalid and ignored, but do not make the @function
   rule itself invalid." */
TEST(CssFunctionSet, TheBodyMayHaveNoResultAndAnUnknownDescriptorIsReported) {
  Defs d({"@function --a() { --local: 1px; }", "@function --b() { result: 1px; color: red; }", "@function --c() { result: 1px !important; }",
          "@function --d() { @media (width > 1px) { result: 2px; width: 1px } }"});
  ASSERT_EQ(d.count(), 4u);
  GTEXT_CSS_Function_Info info;
  ASSERT_TRUE(gtext_css_function_find(d.set, "--a", 3, &info));
  EXPECT_FALSE(info.has_result);
  ASSERT_TRUE(gtext_css_function_find(d.set, "--b", 3, &info));
  EXPECT_TRUE(info.has_result);
  ASSERT_TRUE(gtext_css_function_find(d.set, "--c", 3, &info));
  EXPECT_FALSE(info.has_result) << "a descriptor takes no !important";
  ASSERT_TRUE(gtext_css_function_find(d.set, "--d", 3, &info));
  EXPECT_TRUE(info.has_result) << "a result inside a conditional group rule of the body";
  EXPECT_EQ(d.reasons(), "unknown-descriptor unknown-descriptor unknown-descriptor");
}

/* "If more than one @function exists for a given name, then the rule in the stronger cascade layer wins, and rules defined later win within the same layer." */
TEST(CssFunctionSet, TheLaterRuleWinsAndTheStrongerLayerWins) {
  Defs later({"@function --f() { result: 1px } @function --f(--x) { result: 2px }"});
  GTEXT_CSS_Function_Info info;
  ASSERT_TRUE(gtext_css_function_find(later.set, "--f", 3, &info));
  EXPECT_EQ(info.parameter_count, 1u) << "later in the same layer";
  EXPECT_EQ(later.reasons(), "superseded");
  Defs across({"@function --f() { result: 1px } ", "@function --f(--x) { result: 2px }"});
  ASSERT_TRUE(gtext_css_function_find(across.set, "--f", 3, &info));
  EXPECT_EQ(info.parameter_count, 1u) << "later in a later sheet";
  EXPECT_EQ(info.sheet, 1u);
  Defs layers({"@layer a, b; @layer b { @function --f(--x) { result: 2px } } @layer a { @function --f() { result: 1px } }"});
  ASSERT_TRUE(gtext_css_function_find(layers.set, "--f", 3, &info));
  EXPECT_EQ(info.parameter_count, 1u) << "the stronger layer (b is declared after a) wins though its rule comes first";
  EXPECT_EQ(layers.reasons(), "superseded");
  Defs unlayered({"@function --f(--x) { result: 2px } @layer a { @function --f() { result: 1px } }"});
  ASSERT_TRUE(gtext_css_function_find(unlayered.set, "--f", 3, &info));
  EXPECT_EQ(info.parameter_count, 1u) << "unlayered rules are stronger than layered ones";
}

/* A function is not defined inside a style rule, and a conditional group rule is entered only when it holds. */
TEST(CssFunctionSet, AFunctionInAConditionalRuleIsDefinedWhenTheRuleApplies) {
  Defs d({"@media screen { @function --m() { result: 1px } } @supports (color: red) { @function --s() {} } a { @function --n() {} }"});
  EXPECT_TRUE(d.has("--m"));
  EXPECT_TRUE(d.has("--s"));
  EXPECT_FALSE(d.has("--n"));
  Defs none({"@media screen { @function --m() { result: 1px } }"}, false);
  EXPECT_EQ(none.status, GTEXT_CSS_E_UNSUPPORTED) << "no callback, no guess";
}

TEST(CssFunctionSet, TheSetIsOrderedByNameAndAnUnknownNameIsNotThere) {
  Defs d({"@function --b() {} @function --a() {} @function --c() {}"});
  ASSERT_EQ(d.count(), 3u);
  GTEXT_CSS_Function_Info info;
  std::string order;
  for (size_t i = 0; gtext_css_function_at(d.set, i, &info); i++) {
    order += std::string(info.name.data, info.name.length);
  }
  EXPECT_EQ(order, "--a--b--c");
  EXPECT_FALSE(d.has("--d"));
  EXPECT_FALSE(d.has("--A"));
  EXPECT_FALSE(gtext_css_function_find(nullptr, "--a", 3, &info));
}

TEST(CssFunctionSet, TheCallsAreChecked) {
  GTEXT_CSS_Function_Set * set = nullptr;
  EXPECT_EQ(gtext_css_function_set_new(nullptr, nullptr, &set), GTEXT_CSS_E_INVALID);
  Defs d({});
  GTEXT_CSS_Function_Result res;
  EXPECT_EQ(gtext_css_function_collect(d.set, nullptr, &res), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_function_collect(nullptr, nullptr, &res), GTEXT_CSS_E_INVALID);
  const GTEXT_CSS_Rule * null_rule = nullptr;
  EXPECT_EQ(gtext_css_function_collect_rules(d.set, &null_rule, 1, &res), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_function_count(nullptr), 0u);
  gtext_css_function_set_free(nullptr);
}

/* The limits: one function too many is the registry limit; a prelude with more parameters than the limit is invalid, as one that is not a prelude. */
TEST(CssFunctionSet, TheLimitsAreAssertedByTheirBounds) {
  reg::Owned registry;
  reg::Builder b;
  ASSERT_EQ(b.freeze(registry), GTEXT_CSS_OK);
  GTEXT_CSS_Function_Options o = gtext_css_function_options_default();
  o.max_functions = 2;
  o.max_parameters = 2;
  GTEXT_CSS_Function_Set * set = nullptr;
  ASSERT_EQ(gtext_css_function_set_new(registry.r, &o, &set), GTEXT_CSS_OK);
  const std::string css = "@function --a() {} @function --b(--x, --y) {} @function --p(--x, --y, --z) {} @function --c() {}";
  GTEXT_CSS_Tree * tree = nullptr;
  ASSERT_EQ(gtext_css_parse_stylesheet(css.data(), css.size(), nullptr, &tree, nullptr), GTEXT_CSS_OK);
  GTEXT_CSS_Function_Result res;
  EXPECT_EQ(gtext_css_function_collect(set, tree, &res), GTEXT_CSS_E_LIMIT_REGISTRY);
  EXPECT_EQ(gtext_css_function_count(set), 2u);
  GTEXT_CSS_Function_Diagnostic d;
  ASSERT_TRUE(gtext_css_function_diagnostic(set, 0, &d));
  EXPECT_EQ(d.reason, GTEXT_CSS_FN_BAD_PARAMETER) << "three parameters where two are allowed";
  gtext_css_function_set_free(set);
  gtext_css_tree_free(tree);
}

/* Nothing is partial when memory runs out: every allocation of the collection fails in turn, the status is the out-of-memory one, and nothing is left held. */
TEST(CssFunctionSet, EveryAllocationFailureIsAllOrNothing) {
  const std::string css = "@function --a(--x <length>: 1px, --y type(<number> | auto): auto) returns <length> { result: 1px; --l: 2px; color: red; @media (width > 1px) { result: 3px } } "
                          "@function --a(--z) { result: 2px } @function --b() {}";
  FaultAllocator fa;
  size_t total = 0;
  {
    fa.arm(0);
    Defs d({css}, true, &fa.alloc);
    ASSERT_EQ(d.status, GTEXT_CSS_OK);
    total = fa.calls;
  }
  EXPECT_TRUE(fa.live.empty());
  ASSERT_GT(total, 5u);
  size_t failed = 0;
  for (size_t n = 1; n <= total; n++) {
    fa.arm(n);
    {
      Defs d({css}, true, &fa.alloc);
      if (fa.failed) {
        failed++;
        EXPECT_EQ(d.status, GTEXT_CSS_E_OOM) << "allocation " << n;
      }
    }
    fa.disarm();
    EXPECT_TRUE(fa.live.empty()) << "allocation " << n << " left something held";
  }
  EXPECT_GT(failed, 5u);
}

/* A set made with own_rules keeps a copy of each rule it defines, so the trees it was given need not outlive it: this is what the document does with the live sheets, whose rules are
   edited and removed while a compute context that borrows the set is in use. */
TEST(CssFunctionSet, AnOwningSetReadsItsBodiesAfterTheTreesAreFreed) {
  Defs d({"@function --f(--a: 1) returns <number> { --x: 2; result: var(--a) var(--x); @media (width > 1px) { --y: 3 } } @function --g() {}"}, true, nullptr, true);
  ASSERT_EQ(d.status, GTEXT_CSS_OK);
  d.release_trees();
  GTEXT_CSS_Function_Info info;
  ASSERT_TRUE(gtext_css_function_find(d.set, "--f", 3, &info));
  ASSERT_NE(info.rule, nullptr);
  ASSERT_EQ(info.rule->content_count, 2u);
  ASSERT_EQ(info.rule->contents[0].kind, GTEXT_CSS_ITEM_DECLARATIONS);
  ASSERT_EQ(info.rule->contents[0].declaration_count, 2u);
  EXPECT_EQ(std::string(info.rule->contents[0].declarations[0].name.data, info.rule->contents[0].declarations[0].name.length), "--x");
  EXPECT_EQ(std::string(info.rule->contents[0].declarations[1].name.data, info.rule->contents[0].declarations[1].name.length), "result");
  EXPECT_EQ(info.rule->contents[1].kind, GTEXT_CSS_ITEM_RULE);
  EXPECT_TRUE(info.has_result);
  EXPECT_EQ(d.reasons(), "");
}

TEST(CssFunctionSet, EveryAllocationFailureOfAnOwningSetIsAllOrNothing) {
  const std::string css = "@function --a(--x <length>: 1px) returns <length> { result: 1px; --l: 2px; @media (width > 1px) { result: 3px } } @function --a(--z) { result: 2px } @function --b() {}";
  FaultAllocator fa;
  size_t total = 0;
  {
    fa.arm(0);
    Defs d({css}, true, &fa.alloc, true);
    ASSERT_EQ(d.status, GTEXT_CSS_OK);
    EXPECT_EQ(d.count(), 2u);
    total = fa.calls;
  }
  EXPECT_TRUE(fa.live.empty());
  ASSERT_GT(total, 10u);
  size_t failed = 0;
  for (size_t n = 1; n <= total; n++) {
    fa.arm(n);
    {
      Defs d({css}, true, &fa.alloc, true);
      if (fa.failed) {
        failed++;
        EXPECT_EQ(d.status, GTEXT_CSS_E_OOM) << "allocation " << n;
      }
    }
    fa.disarm();
    EXPECT_TRUE(fa.live.empty()) << "allocation " << n << " left something held";
  }
  EXPECT_GT(failed, 10u);
}
