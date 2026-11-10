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
 * Applying a mixin when a style sheet is compiled (css_sheet.h): the compiled sheet of a rule with an `@apply` is the sheet of the rule with the body of the mixin in the place of the `@apply`, the
 * set of mixins a compile is given, the limits of the expansion and the failure of every allocation. The reference is CSS Functions and Mixins 1 as pinned ("The @apply Rule": "@apply rules are
 * processed before any styles are applied, as they effectively modify the stylesheet itself").
 */

#include <cstring>
#include <iostream>
#include <string>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_cascade_test_support.h"
#include "css_test_support.h"
#include "markup_test_support.h"

using namespace cas;

namespace {

std::string dump(const GTEXT_CSS_Sheet * s) {
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

/// A sheet compiled from text.
struct Compiled {
  css_test::Parsed p;
  GTEXT_CSS_Sheet * sheet = nullptr;
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  GTEXT_CSS_Sheet_Result result;
  explicit Compiled(const std::string & text, const GTEXT_CSS_Sheet_Options * options = nullptr) {
    std::memset(&result, 0, sizeof result);
    css_test::parse_into(p, gtext_css_parse_stylesheet, text);
    status = gtext_css_sheet_compile(p.tree, R(), options, &sheet, &result);
  }
  ~Compiled() {
    gtext_css_sheet_free(sheet);
  }
  std::string dump() const {
    return ::dump(sheet);
  }
};

}  // namespace

const char * const kMark = "\xF4\x8F\xBF\xBD";

/// A chain of `n` mixins: `--m<i>` applies `--m<i+1>`, the last sets a declaration, and a rule applies `--m0`.
std::string chain(size_t n) {
  std::string css;
  for (size_t i = 0; i < n; i++) {
    css += "@mixin --m" + std::to_string(i) + " { ";
    css += i + 1 < n ? "@apply --m" + std::to_string(i + 1) + ";" : "--leaf: 1;";
    css += " }\n";
  }
  return css + "p { @apply --m0; }";
}

size_t records(const Compiled & c) {
  return gtext_css_sheet_record_count(c.sheet);
}

// "The @apply rule applies a mixin, causing it to substitute into the rule in place of the @apply rule itself": the compiled sheet of the rule with the @apply has the declarations of the body, in
// the place of the @apply, and the parameters are custom properties of a name no author can write (the sheet number is left out of a dump).
TEST(CssSheetMixin, AnApplyIsReplacedByTheBodyOfTheMixin) {
  Compiled c("@mixin --m(--a: 2) { color: red; --b: var(--a); } p { margin-top: 1px; @apply --m(1); margin-left: 2px; }");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  const std::string d = c.dump();
  EXPECT_NE(d.find("margin-top: 1px"), std::string::npos) << d;
  EXPECT_NE(d.find(std::string("--") + kMark + "p_0_--a: 1"), std::string::npos) << d;
  EXPECT_NE(d.find("color: red"), std::string::npos) << d;
  EXPECT_NE(d.find(std::string("--b: var(--") + kMark + "p_0_--a)"), std::string::npos) << "the reference in the body names the parameter's hidden name: " << d;
  EXPECT_NE(d.find("margin-left: 2px"), std::string::npos) << d;
  EXPECT_LT(d.find("margin-top"), d.find("color: red"));
  EXPECT_LT(d.find("color: red"), d.find("margin-left"));
}

// Two compiles of one text dump alike: the numbers of the hidden names are the rule's order, not the compile's.
TEST(CssSheetMixin, TwoCompilesDumpAlike) {
  const std::string css = "@mixin --m(--a: 2) { --b: var(--a); & > i { --c: var(--a); } } p { @apply --m(1); } q { @apply --m; }";
  Compiled a(css), b(css);
  ASSERT_EQ(a.status, GTEXT_CSS_OK);
  EXPECT_EQ(a.dump(), b.dump());
  EXPECT_NE(a.dump().find(std::string("p_0_--a")), std::string::npos);
  EXPECT_NE(a.dump().find(std::string("p_1_--a")), std::string::npos) << "each application has its own private property";
}

// A mixin is found among the sheet's own rules: at the top and in an @layer block, not in a conditional group rule, and the rule that is invalid is not one.
TEST(CssSheetMixin, AMixinIsFoundAmongTheSheetsOwnRules) {
  Compiled top("@mixin --m { --x: 1; } p { @apply --m; }");
  EXPECT_NE(top.dump().find("--x: 1"), std::string::npos);
  Compiled layered("@layer l { @mixin --m { --x: 1; } } p { @apply --m; }");
  EXPECT_NE(layered.dump().find("--x: 1"), std::string::npos);
  Compiled conditional("@media all { @mixin --m { --x: 1; } } p { @apply --m; }");
  EXPECT_EQ(conditional.dump().find("--x: 1"), std::string::npos) << "a mixin in a conditional group rule is not read";
  Compiled invalid("@mixin m { --x: 1; } @mixin --a(--b, --b) { --x: 1; } p { @apply --m; @apply --a(1); }");
  EXPECT_EQ(invalid.dump().find("--x: 1"), std::string::npos);
  Compiled nested("p { @mixin --m { --x: 1; } @apply --m; }");
  EXPECT_EQ(nested.dump().find("--x: 1"), std::string::npos) << "\"it is invalid within the body of a style rule\"";
}

// The set of mixins a compile is given: asked for once, at the first @apply, and its answer is the sheet's; a status it fails with is the compile's.
struct Source {
  GTEXT_CSS_Function_Set * set = nullptr;
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  int calls = 0;
};

const GTEXT_CSS_Function_Set * source(void * user, GTEXT_CSS_Status * status) {
  Source * s = static_cast<Source *>(user);
  s->calls++;
  *status = s->status;
  return s->set;
}

TEST(CssSheetMixin, TheCallersSetIsAskedOnceAtTheFirstApply) {
  css_test::Parsed lib;
  css_test::parse_into(lib, gtext_css_parse_stylesheet, "@mixin --m(--v) { --x: var(--v); }");
  GTEXT_CSS_Function_Options fo = gtext_css_function_options_default();
  fo.mixins = true;
  fo.no_functions = true;
  Source src;
  ASSERT_EQ(gtext_css_function_set_new(R(), &fo, &src.set), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_function_collect(src.set, lib.tree, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_mixin_count(src.set), 1u);
  GTEXT_CSS_Sheet_Options o = gtext_css_sheet_options_default();
  o.mixins = source;
  o.mixins_user = &src;
  Compiled none("p { color: red }", &o);
  EXPECT_EQ(src.calls, 0) << "a sheet with no @apply never asks";
  Compiled two("p { @apply --m(1); } q { @apply --m(2); @apply --none; }", &o);
  ASSERT_EQ(two.status, GTEXT_CSS_OK);
  EXPECT_EQ(src.calls, 1);
  EXPECT_NE(two.dump().find("--x: var("), std::string::npos) << two.dump();
  EXPECT_EQ(records(two), 4u) << "a run for the parameters and a run for the body, in each of the two rules";
  src.status = GTEXT_CSS_E_LIMIT_WORK;
  Compiled failing("p { @apply --m(1); }", &o);
  EXPECT_EQ(failing.status, GTEXT_CSS_E_LIMIT_WORK);
  src.status = GTEXT_CSS_OK;
  Compiled own("@mixin --m { --y: own; } p { @apply --m; }", &o);
  EXPECT_NE(own.dump().find("--x"), std::string::npos) << "with a set given, the sheet's own @mixin rules are not read";
  gtext_css_function_set_free(src.set);
}

// The set API: a mixin is found by name with its parameters, the later rule of a name and the stronger layer win.
TEST(CssSheetMixin, TheSetFindsMixinsByName) {
  css_test::Parsed t;
  css_test::parse_into(t, gtext_css_parse_stylesheet,
      "@layer a, b; @layer b { @mixin --m(--p <length>: 1px) { } } @layer a { @mixin --m(--q) { } } @mixin --n(--r, --s: 3,) { } @media all { @mixin --hidden { } } @mixin --bad(x) { }");
  GTEXT_CSS_Function_Options fo = gtext_css_function_options_default();
  fo.mixins = true;
  GTEXT_CSS_Function_Set * set = nullptr;
  ASSERT_EQ(gtext_css_function_set_new(R(), &fo, &set), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_function_collect(set, t.tree, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(gtext_css_mixin_count(set), 2u);
  GTEXT_CSS_Function_Info info;
  ASSERT_TRUE(gtext_css_mixin_find(set, "--m", 3, &info));
  EXPECT_EQ(info.parameter_count, 1u) << "the rule in the stronger layer (b) wins";
  GTEXT_CSS_Function_Parameter p;
  ASSERT_TRUE(gtext_css_mixin_parameter(set, "--m", 3, 0, &p));
  EXPECT_EQ(css_test::str(p.name), "--p");
  EXPECT_TRUE(p.has_type);
  EXPECT_EQ(css_test::str(p.default_value), "1px");
  ASSERT_TRUE(gtext_css_mixin_find(set, "--n", 3, &info));
  EXPECT_EQ(info.parameter_count, 2u);
  EXPECT_FALSE(info.has_returns);
  EXPECT_FALSE(gtext_css_mixin_find(set, "--hidden", 8, &info)) << "not read in a conditional group rule";
  EXPECT_FALSE(gtext_css_mixin_find(set, "--bad", 5, &info));
  EXPECT_FALSE(gtext_css_mixin_find(set, "--m", 2, &info));
  EXPECT_FALSE(gtext_css_mixin_parameter(set, "--m", 3, 1, &p));
  EXPECT_EQ(gtext_css_function_count(set), 0u) << "no function was asked for";
  EXPECT_EQ(gtext_css_mixin_count(nullptr), 0u);
  gtext_css_function_set_free(set);
}

/* ---- limits ---------------------------------------------------------------------------------------------------------------------------------------------- */

// "mixins applied inside one another": a chain of the limit's length expands, one more is the limit's error and names it. The default is 16.
TEST(CssSheetMixin, TheDepthOfMixinsInsideOneAnotherIsBounded) {
  GTEXT_CSS_Sheet_Options o = gtext_css_sheet_options_default();
  o.max_mixin_depth = 4;
  Compiled ok(chain(4), &o);
  ASSERT_EQ(ok.status, GTEXT_CSS_OK);
  EXPECT_NE(ok.dump().find("--leaf: 1"), std::string::npos);
  Compiled over(chain(5), &o);
  EXPECT_EQ(over.status, GTEXT_CSS_E_LIMIT_RULE_NESTING);
  EXPECT_STREQ(over.result.name, "max_mixin_depth");
  Compiled def16(chain(16));
  EXPECT_EQ(def16.status, GTEXT_CSS_OK) << "the default admits 16";
  Compiled def17(chain(17));
  EXPECT_EQ(def17.status, GTEXT_CSS_E_LIMIT_RULE_NESTING);
}

// "at most 256" and the stack of the rules ("max_rule_depth") is the other bound: a chain the stack cannot hold is its error.
TEST(CssSheetMixin, TheStackOfRulesBoundsTheExpansionToo) {
  GTEXT_CSS_Sheet_Options o = gtext_css_sheet_options_default();
  o.max_mixin_depth = 256;
  o.max_rule_depth = 8;
  Compiled c(chain(20), &o);
  EXPECT_EQ(c.status, GTEXT_CSS_E_LIMIT_RULE_NESTING);
}

// Each application of a mixin counts, those inside a mixin's body too: the amplification of three applications in every level is the limit's error, quickly, and not a sheet of 2^30 rules.
TEST(CssSheetMixin, TheNumberOfExpansionsIsBounded) {
  GTEXT_CSS_Sheet_Options o = gtext_css_sheet_options_default();
  o.max_mixin_expansions = 5;
  Compiled five("@mixin --m { --x: 1; } p { @apply --m; @apply --m; @apply --m; @apply --m; @apply --m; }", &o);
  EXPECT_EQ(five.status, GTEXT_CSS_OK);
  Compiled six("@mixin --m { --x: 1; } p { @apply --m; @apply --m; @apply --m; @apply --m; @apply --m; @apply --m; }", &o);
  EXPECT_EQ(six.status, GTEXT_CSS_E_LIMIT_WORK);
  EXPECT_STREQ(six.result.name, "max_mixin_expansions");
  std::string css;
  for (int i = 0; i < 11; i++) {
    css += "@mixin --d" + std::to_string(i) + " { @apply --d" + std::to_string(i + 1) + "; @apply --d" + std::to_string(i + 1) + "; @apply --d" + std::to_string(i + 1) + "; }\n";
  }
  css += "@mixin --d11 { --leaf: 1; } p { @apply --d0; }";
  Compiled bomb(css);
  EXPECT_EQ(bomb.status, GTEXT_CSS_E_LIMIT_WORK) << "3^12/2 applications are more than the default 65,536 allows";
  GTEXT_CSS_Sheet_Options big = gtext_css_sheet_options_default();
  big.max_mixin_expansions = 1000000;
  big.max_compiled_bytes = 1 << 20;
  Compiled bytes(css, &big);
  EXPECT_EQ(bytes.status, GTEXT_CSS_E_LIMIT_SHEET_SIZE) << "and the memory of the compiled form is bounded as it is for any sheet";
}

// "mixins that apply each other" are not an error: the application that would be a cycle does nothing.
TEST(CssSheetMixin, ACycleIsNotAnError) {
  Compiled c("@mixin --a { --a: 1; @apply --b; } @mixin --b { --b: 1; @apply --a; } @mixin --s { @apply --s; --s: 1; } p { @apply --a; @apply --s; }");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  const std::string d = c.dump();
  EXPECT_NE(d.find("--a: 1"), std::string::npos);
  EXPECT_NE(d.find("--b: 1"), std::string::npos);
  EXPECT_NE(d.find("--s: 1"), std::string::npos);
}


// What a body makes the compile read is charged to max_compiled_bytes, also when the items are dropped: a body of many items applied many times is stopped by the limit, not run to the end.
TEST(CssSheetMixin, TheWorkOfReadingABodyIsCharged) {
  std::string css = "@mixin --m {";
  for (int i = 0; i < 200; i++) {
    css += " @layer x;";
  }
  css += " } p {";
  for (int i = 0; i < 200; i++) {
    css += " @apply --m;";
  }
  css += " }";
  Compiled free(css);
  EXPECT_EQ(free.status, GTEXT_CSS_OK);
  GTEXT_CSS_Sheet_Options o = gtext_css_sheet_options_default();
  o.max_compiled_bytes = 100000;
  Compiled capped(css, &o);
  EXPECT_EQ(capped.status, GTEXT_CSS_E_LIMIT_SHEET_SIZE) << "40,000 items of dropped rules, 16 bytes each, are more than 100,000";
}

/* ---- where what is read is said to come from ------------------------------------------------------------------------------------------------------------- */

// A diagnostic of a rule inside a mixin's body (a selector that is not valid) is the @apply's: the id and the span are the rule's of the sheet's own tree, which is what a caller can look up
// ("the same number in another tree" is what the ids of a mixin's tree would be).
TEST(CssSheetMixin, ADiagnosticOfAMixinBodyIsTheApplys) {
  const std::string css = "@mixin --m { & :bogus-pseudo-class(1) { color: red } } p { @apply --m; }";
  Compiled c(css);
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  ASSERT_GE(gtext_css_sheet_diagnostic_total(c.sheet), 1u);
  GTEXT_CSS_Sheet_Diagnostic d;
  ASSERT_TRUE(gtext_css_sheet_diagnostic(c.sheet, 0, &d));
  EXPECT_EQ(d.kind, GTEXT_CSS_SHEET_DIAG_INVALID_SELECTOR);
  const GTEXT_CSS_Rule * style = &c.p.tree->rules[1];
  ASSERT_EQ(style->rule_count, 1u);
  const GTEXT_CSS_Rule * apply = &style->rules[0];
  EXPECT_EQ(d.id, apply->id);
  EXPECT_EQ(d.span.offset, apply->span.offset);
  EXPECT_EQ(d.span.end, apply->span.end);
}

// `@layer` in a body is dropped ("The contents of any @layer within a mixin body are dropped when the mixin is applied"), at any depth, and what is around it stays.
TEST(CssSheetMixin, ALayerInABodyIsDropped) {
  Compiled c("@mixin --m { --a: 1; @layer x { --b: 1; } @media all { @layer y { --c: 1; } --d: 1; } } p { @apply --m; }");
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  const std::string d = c.dump();
  EXPECT_NE(d.find("--a: 1"), std::string::npos);
  EXPECT_EQ(d.find("--b: 1"), std::string::npos);
  EXPECT_EQ(d.find("--c: 1"), std::string::npos);
  EXPECT_NE(d.find("--d: 1"), std::string::npos);
  EXPECT_EQ(gtext_css_sheet_layer_count(c.sheet), 0u) << "no layer of the mixin is declared";
}

/* ---- allocation failure ---------------------------------------------------------------------------------------------------------------------------------- */

const char * const kApplied =
    "@mixin --box(--w <length>: 2px, --c: red, --n) { @private { --t: 1; @property --u <length>: 3px; } width: var(--w); --o: var(--c) var(--t) var(--u); & > i { color: var(--c); @contents; }"
    " @media all { @contents { --f: fallback; } } @apply --inner(var(--c)); }\n"
    "@mixin --inner(--z) { --in: var(--z); }\n"
    "p { @private { --mine: m; } @apply --box(1em, { red, blue }, var(--mine)) { margin-top: var(--mine); } @apply --box; @apply --none; }";

// Every allocation of expanding mixins (the set the compile makes of the sheet's rules, the scopes and the names of the parameters, the argument texts, the declarations and the registrations) is
// failed in turn: a failed compile is an out-of-memory error and the next compile gives the sheet that was never failed, with nothing leaked.
TEST(CssSheetMixinOom, EveryAllocationOfExpandingMixinsFailsCleanly) {
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_stylesheet, kApplied);
  ASSERT_NE(p.tree, nullptr);
  GTEXT_CSS_Sheet_Options base = gtext_css_sheet_options_default();
  GTEXT_CSS_Sheet * clean = nullptr;
  ASSERT_EQ(gtext_css_sheet_compile(p.tree, R(), &base, &clean, nullptr), GTEXT_CSS_OK);
  const std::string want = dump(clean);
  gtext_css_sheet_free(clean);
  ASSERT_NE(want.find("--f: fallback"), std::string::npos) << want;
  ASSERT_NE(want.find("--in: var("), std::string::npos) << want;
  size_t failures = 0;
  bool answered = false;
  for (size_t n = 1; n < 30000 && !answered; n++) {
    FaultAllocator fa;
    GTEXT_CSS_Sheet_Options o = base;
    o.allocator = &fa.alloc;
    GTEXT_CSS_Sheet * s = nullptr;
    GTEXT_CSS_Sheet_Result r;
    fa.arm(n);
    const GTEXT_CSS_Status st = gtext_css_sheet_compile(p.tree, R(), &o, &s, &r);
    const bool failed = fa.failed;
    fa.disarm();
    if (st == GTEXT_CSS_OK) {
      EXPECT_FALSE(failed) << "at " << n << ": an allocation failed and the call did not say so";
      EXPECT_EQ(dump(s), want) << "at " << n;
      answered = true;
    }
    else {
      failures++;
      EXPECT_EQ(st, GTEXT_CSS_E_OOM) << "at " << n;
      EXPECT_EQ(s, nullptr);
    }
    gtext_css_sheet_free(s);
    EXPECT_TRUE(fa.live.empty()) << "at " << n << ": " << fa.live.size() << " blocks leaked";
    EXPECT_EQ(fa.bad_frees, 0u);
  }
  EXPECT_TRUE(answered);
  EXPECT_GT(failures, 200u) << "the sweep reached the allocations of the expansion";
}
