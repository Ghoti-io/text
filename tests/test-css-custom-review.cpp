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
 * What the review of the substitution layer found, one named test for each: the taint scan past the thirty-second argument, the byte budget of an element, the
 * registered values that are shared, the registered integers, colors and lists of identifiers, the depth of a chain, the nesting the parser refuses, `all: var()`,
 * the trimming of a custom property's value, a cycle through the font properties, and the numbers that are out of range.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "../src/css/css_rope.h"
#include "css_custom_test_support.h"

using namespace cust;


namespace {

/// The value of `prop` for an element with the attributes given, declared `decls`, in a world with the registrations given.
struct Scene {
  World w;
  Node node;
  explicit Scene(std::map<std::string, std::string> attrs = {}, const std::vector<Reg> & regs = {}, const std::vector<std::string> & sheets = {}) {
    node.attrs = std::move(attrs);
    EXPECT_EQ(w.build(regs, sheets), GTEXT_CSS_OK);
  }
};

std::string image_set(size_t options, const std::string & last) {
  std::string v = "image-set(";
  for (size_t i = 0; i < options; i++) {
    v += "\"a" + std::to_string(i) + ".png\" " + std::to_string(i + 1) + "x, ";
  }
  return v + last + " 5000x)";
}

TEST(CssCustomReview, AnAttrTaintedUrlIsFoundAmongAnyNumberOfOptions) {
  // "Using an attr()-tainted value as or in a <url> makes a declaration invalid at computed-value time ... as are functions that contain an attr()-tainted value":
  // the scan once held thirty-two operands and let the thirty-third through.
  Scene s({{"href", "http://evil/x.png"}});
  for (const size_t n : {1u, 5u, 31u, 32u, 33u, 40u, 100u, 1000u}) {
    Computed c;
    compute_pairs(s.w, c, {{"background-image", image_set(n, "attr(href)")}}, nullptr, &s.node);
    ASSERT_EQ(c.status, GTEXT_CSS_OK) << n;
    EXPECT_EQ(c.value(s.w.registry.r, "background-image"), "none") << n << " options and then a tainted one";
  }
  // the control: the same options with no taint are kept, however many
  Computed ok;
  compute_pairs(s.w, ok, {{"background-image", image_set(40, "\"b.png\"")}}, nullptr, &s.node);
  EXPECT_NE(ok.value(s.w.registry.r, "background-image"), "none");
  // and a function name is a name in any case, whatever the spelling: each form is valid with a string, and invalid-at-computed-value-time with the attribute
  for (const char * form : {"IMAGE-SET(%s 1x)", "Image-Set(%s 1x)", "image-set(%s 1x)", "SRC(%s)", "src(%s)", "Image(%s)", "image(%s)", "url(%s)"}) {
    char with_attr[96], with_string[96];
    std::snprintf(with_attr, sizeof with_attr, form, "attr(href)");
    std::snprintf(with_string, sizeof with_string, form, "\"a.png\"");
    Computed ok2, bad;
    compute_pairs(s.w, ok2, {{"background-image", with_string}}, nullptr, &s.node);
    compute_pairs(s.w, bad, {{"background-image", with_attr}}, nullptr, &s.node);
    EXPECT_NE(ok2.value(s.w.registry.r, "background-image"), "none") << with_string << ": the control is valid";
    EXPECT_EQ(bad.value(s.w.registry.r, "background-image"), "none") << with_attr;
  }
}

TEST(CssCustomReview, ATaintThatOnlyTouchesTheEdgeOfAnArgumentIsNotInIt) {
  // a string that follows the tainted one without overlapping it is not tainted; one that begins where it ends does not count either
  Scene s({{"u", "x"}});
  Computed c;
  compute_pairs(s.w, c, {{"background-image", "image-set(\"fine.png\" 1x, \"also.png\" 2x)"}, {"--t", "attr(u)"}}, nullptr, &s.node);
  EXPECT_NE(c.value(s.w.registry.r, "background-image"), "none");
  Computed d;
  compute_pairs(s.w, d, {{"background-image", "image-set(attr(u) 1x, \"fine.png\" 2x)"}}, nullptr, &s.node);
  EXPECT_EQ(d.value(s.w.registry.r, "background-image"), "none");
  Computed e;
  compute_pairs(s.w, e, {{"background-image", "image-set(\"fine.png\" 1x, attr(u) 2x)"}}, nullptr, &s.node);
  EXPECT_EQ(e.value(s.w.registry.r, "background-image"), "none") << "the last argument";
}

/* ---- the byte budget ------------------------------------------------------------------------------------------------------------- */

TEST(CssCustomReview, AValueOfFewLongWordsIsBoundedByItsBytesNotItsTokens) {
  // 16,000 bytes doubled ten times is sixteen megabytes and few tokens; used by every property of an element, it was flattened and parsed for each.
  World w;
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  std::vector<std::pair<std::string, std::string>> d;
  d.push_back({"--d0", std::string(16000, 'a')});
  for (int i = 1; i <= 10; i++) {
    d.push_back({"--d" + std::to_string(i), "var(--d" + std::to_string(i - 1) + ") var(--d" + std::to_string(i - 1) + ")"});
  }
  for (int i = 0; i < 40; i++) {
    d.push_back({"--u" + std::to_string(i), "var(--d10)"});
  }
  Computed c;
  compute_pairs(w, c, d);
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_GE(c.result.substitution_limited, 1u);
  EXPECT_EQ(c.custom("--d10"), "<gi>") << "past 1 MiB";
  EXPECT_NE(c.custom("--d5"), "<gi>");
  EXPECT_LE(c.result.work, 200000u);

  // a value under the per-value limit, used by many: the element's bytes run out, and the later uses are the guaranteed-invalid value
  std::vector<std::pair<std::string, std::string>> e;
  e.push_back({"--b0", std::string(8000, 'a')});
  for (int i = 1; i <= 6; i++) {
    e.push_back({"--b" + std::to_string(i), "var(--b" + std::to_string(i - 1) + ") var(--b" + std::to_string(i - 1) + ")"});
  }
  for (int i = 0; i < 100; i++) {
    e.push_back({"--v" + std::to_string(i), "var(--b6)"});
  }
  Computed f;
  compute_pairs(w, f, e);
  ASSERT_EQ(f.status, GTEXT_CSS_OK);
  EXPECT_NE(f.custom("--v0"), "<gi>");
  EXPECT_EQ(f.custom("--v99"), "<gi>") << "the element has used its 8 MiB";
  EXPECT_GE(f.result.substitution_limited, 1u);
  EXPECT_LE(f.result.work, 8u * (8u << 20) / 16) << "a sixteenth of a step for each byte, up to the budget, read a few times over: " << f.result.work;
}

TEST(CssCustomReview, TheByteBudgetOfAnElementIsAnOptionAndTheBoundaryIsExact) {
  World w;
  w.options.substitute.max_element_substituted_bytes = 1000;
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  // a value of 400 bytes is charged when it is complete: the first two fit in 1000 (800), the third is 1200
  const std::string v(400, 'x');
  Computed c;
  compute_pairs(w, c, {{"--a", v}, {"--b", "var(--a)"}, {"--c", "var(--a)"}});
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.custom("--a"), v);
  EXPECT_EQ(c.custom("--b"), v);
  EXPECT_EQ(c.custom("--c"), "<gi>");
  Computed d;
  compute_pairs(w, d, {{"--a", std::string(500, 'x')}, {"--b", "var(--a)"}});
  EXPECT_NE(d.custom("--a"), "<gi>") << "500";
  EXPECT_EQ(d.custom("--b"), std::string(500, 'x')) << "exactly 1000 bytes between them";
  Computed e;
  compute_pairs(w, e, {{"--a", std::string(501, 'x')}, {"--b", "var(--a)"}});
  EXPECT_EQ(e.custom("--b"), "<gi>") << "1002";
}

/* ---- the registered values that are shared ------------------------------------------------------------------------------------------- */

struct CountingAllocator {
  GTEXT_Allocator a;
  size_t bytes = 0;
  CountingAllocator() {
    a.ctx = this;
    a.malloc_fn = [](void * c, size_t n) -> void * {
      static_cast<CountingAllocator *>(c)->bytes += n;
      return std::malloc(n);
    };
    a.calloc_fn = [](void * c, size_t n, size_t m) -> void * {
      static_cast<CountingAllocator *>(c)->bytes += n * m;
      return std::calloc(n, m);
    };
    a.realloc_fn = [](void * c, void * p, size_t n) -> void * {
      static_cast<CountingAllocator *>(c)->bytes += n;
      return std::realloc(p, n);
    };
    a.free_fn = [](void *, void * p) { std::free(p); };
  }
};

TEST(CssCustomReview, AStyleThatChangesNoRegisteredPropertyHoldsAReferenceNotAnArray) {
  CountingAllocator counting;
  std::vector<Reg> regs;
  for (int i = 0; i < 6000; i++) {
    regs.push_back({"--r" + std::to_string(i), "<length>", "0px", true, true, false});
  }
  World w;
  w.options.allocator = &counting.a;
  ASSERT_EQ(w.build(regs), GTEXT_CSS_OK);
  Computed parent;
  compute(w, parent, "--r5: 3px; --r7: 2px");
  ASSERT_EQ(parent.status, GTEXT_CSS_OK);
  const size_t before = counting.bytes;
  Computed child;
  compute(w, child, "width: 4px", &parent);
  ASSERT_EQ(child.status, GTEXT_CSS_OK);
  const size_t shared = counting.bytes - before;
  // a style of its own would hold an array of the registrations (6,000 pointers); this one holds a reference to the parent's, and whatever else a style is made of
  // (about 12 KB on a 64-bit host, less on a 32-bit one)
  EXPECT_LT(shared, 6000u * sizeof(void *)) << "the registered values of the parent are shared: " << shared << " bytes";
  EXPECT_EQ(child.custom("--r5"), "3px");
  EXPECT_EQ(child.custom("--r6"), "0px");
  // a child that declares one has its own array, and the parent's is as it was
  Computed other;
  compute(w, other, "--r6: 9px", &parent);
  EXPECT_EQ(other.custom("--r6"), "9px");
  EXPECT_EQ(other.custom("--r5"), "3px");
  EXPECT_EQ(parent.custom("--r6"), "0px");
  EXPECT_EQ(child.custom("--r6"), "0px");
  // a property that does not inherit is the initial value again in the child: nothing is shared that would be wrong
  Computed noinh_parent, noinh_child;
  Scene s2({}, {{"--n", "<length>", "1px", false, true, false}});
  compute(s2.w, noinh_parent, "--n: 8px");
  compute(s2.w, noinh_child, "", &noinh_parent);
  EXPECT_EQ(noinh_parent.custom("--n"), "8px");
  EXPECT_EQ(noinh_child.custom("--n"), "1px");
}

/* ---- registered values computed by their type ------------------------------------------------------------------------------------ */

std::string registered(const std::string & syntax, const std::string & initial, const std::string & value, const std::string & parent_decls = "font-size: 10px") {
  Scene s({}, {{"--p", syntax, initial, false, true, false}});
  Computed p, c;
  compute(s.w, p, parent_decls);
  compute(s.w, c, "--p: " + value, &p);
  EXPECT_EQ(c.status, GTEXT_CSS_OK) << value;
  return c.custom("--p");
}

TEST(CssCustomReview, ARegisteredIntegerIsAnInteger) {
  // "<integer>": a calculation is rounded to the nearest integer ("half toward +infinity"), and a value past what 32 bits hold is clamped
  EXPECT_EQ(registered("<integer>", "0", "calc(2.4)"), "2");
  EXPECT_EQ(registered("<integer>", "0", "calc(2.6)"), "3");
  EXPECT_EQ(registered("<integer>", "0", "calc(1.5 + 1)"), "3");
  EXPECT_EQ(registered("<integer>", "0", "calc(15 + 15)"), "30");
  EXPECT_EQ(registered("<integer>", "0", "2147483648"), "2147483647");
  EXPECT_EQ(registered("<integer>", "0", "-2147483649"), "-2147483648");
  EXPECT_EQ(registered("<integer>+", "0", "calc(1.4) calc(2.5)"), "1 3");
  EXPECT_EQ(registered("<number> | <integer>", "0", "calc(2.4)"), "2.4") << "the number matches first";
}

TEST(CssCustomReview, ARegisteredColorIsAnSrgbColorWrittenAsAComputedColorIs) {
  EXPECT_EQ(registered("<color>", "red", "tomato"), "rgb(255, 99, 71)");
  EXPECT_EQ(registered("<color>", "red", "#ff0000"), "rgb(255, 0, 0)");
  EXPECT_EQ(registered("<color>", "red", "#badbee33"), "rgba(186, 219, 238, 0.2)");
  EXPECT_EQ(registered("<color>", "red", "rgb(10 20 30 / 50%)"), "rgba(10, 20, 30, 0.5)");
  EXPECT_EQ(registered("<color>", "red", "currentcolor", "font-size: 10px; color: blue"), "rgb(0, 0, 255)") << "\"currentcolor\" is the element's color";
  EXPECT_EQ(registered("<color>", "red", "light-dark(lime, red)"), "rgb(0, 255, 0)");
  EXPECT_EQ(registered("<color>#", "red", "red ,tomato"), "rgb(255, 0, 0), rgb(255, 99, 71)");
  EXPECT_EQ(registered("tomato | plum | <color>", "plum", "plum"), "plum") << "an identifier of the syntax is not converted";
}

TEST(CssCustomReview, ARegisteredListOfLengthsKeepsEveryItemComputed) {
  EXPECT_EQ(registered("<length>#", "0px", "10px, 3em"), "10px, 30px");
  EXPECT_EQ(registered("<length>+", "0px", "4em 9px"), "40px 9px");
  EXPECT_EQ(registered("<transform-list>", "scale(1)", "translate(2em) rotate(0.5turn)"), "translate(20px) rotate(180deg)");
  EXPECT_EQ(registered("<transform-function>+", "scale(1)", "translateX(10em) scale(2)"), "translateX(100px) scale(2)");
}

TEST(CssCustomReview, ARegisteredListOfIdentifiersIsWrittenCanonically) {
  EXPECT_EQ(registered("foo+", "foo", "foo   foo"), "foo foo");
  EXPECT_EQ(registered("foo#", "foo", "foo ,foo"), "foo, foo");
  EXPECT_EQ(registered("foo#", "foo", "/*a*/ foo /*b*/ ,  foo /*c*/"), "foo, foo");
  EXPECT_EQ(registered("foo | bar", "foo", "  bar "), "bar");
  EXPECT_EQ(registered("<custom-ident>+", "a", "Foo   bar"), "Foo bar") << "case is kept";
}

TEST(CssCustomReview, AnInitialValueOfTheUniversalSyntaxKeepsItsSpelling) {
  // as an unregistered property would: the tokens exactly as specified, but for what is before the first and after the last
  Scene s({}, {{"--u", "*", "1px+2px", false, true, false}, {"--v", "*", "/*c*/ 1px  /*d*/", false, true, false}});
  Computed c;
  compute(s.w, c, "");
  EXPECT_EQ(c.custom("--u"), "1px+2px");
  EXPECT_EQ(c.custom("--v"), "1px");
}

/* ---- trimming ------------------------------------------------------------------------------------------------------------------------ */

TEST(CssCustomReview, TheWhiteSpaceAndCommentsAtBothEndsAreNotPartOfAValue) {
  // the text of a custom property is that of its tokens, and a comment is no token: the same at both ends
  World w;
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  Computed c;
  compute_pairs(w, c, {{"--a", "/*c*/ x /*d*/"}, {"--b", "/*c*/x/*d*/"}, {"--c", "  x  "}, {"--d", "/* only */"}, {"--e", "a /* kept */ b"}, {"--f", "\"s /* in */\" /*x*/"}});
  EXPECT_EQ(c.custom("--a"), "x");
  EXPECT_EQ(c.custom("--b"), "x");
  EXPECT_EQ(c.custom("--c"), "x");
  EXPECT_EQ(c.custom("--d"), "") << "empty, not guaranteed-invalid";
  EXPECT_EQ(c.custom("--e"), "a /* kept */ b");
  EXPECT_EQ(c.custom("--f"), "\"s /* in */\"") << "a comment inside a string is the string";
}

/* ---- depth and nesting ---------------------------------------------------------------------------------------------------------------- */

/* ---- all: var() ---------------------------------------------------------------------------------------------------------------------- */

TEST(CssCustomReview, AllTakesAFunctionThatYieldsACssWideKeyword) {
  // `all` takes only a CSS-wide keyword; `var()` may give one ("a browser takes it": a declaration with a function in it is valid at parse time)
  World w;
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  Computed parent;
  compute(w, parent, "color: red; margin-top: 4px");
  ASSERT_EQ(parent.status, GTEXT_CSS_OK);
  EXPECT_EQ(parent.value(w.registry.r, "margin-top"), "4px");
  Computed d;
  compute_pairs(w, d, {{"margin-top", "9px"}, {"all", "inherit"}}, &parent);
  EXPECT_EQ(d.value(w.registry.r, "margin-top"), "4px") << "the control, with no function";
  Computed c;
  compute_pairs(w, c, {{"color", "green"}, {"margin-top", "9px"}, {"all", "var(--none, inherit)"}}, &parent);
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.value(w.registry.r, "color"), "red") << "all: inherit";
  EXPECT_EQ(c.value(w.registry.r, "margin-top"), "4px") << "inherit takes the parent's own value of a property that does not inherit";
}


/* ---- where two tokens would glue ----------------------------------------------------------------------------------------------- */

TEST(CssCustomReview, TwoTokensThatWouldGlueAreSeparatedByAComment) {
  // CSS Syntax, "Serialization": the pairs of tokens that need a comment between them, row by row, written out here and checked against every pair of classes
  using TC = css_tokclass;
  struct Row {
    std::vector<TC> lasts;
    std::vector<TC> firsts;
  };
  const std::vector<Row> rows = {
      {{CSS_TC_IDENT}, {CSS_TC_IDENT, CSS_TC_FUNC, CSS_TC_URL, CSS_TC_BADURL, CSS_TC_D_MINUS, CSS_TC_NUM, CSS_TC_PCT, CSS_TC_DIM, CSS_TC_CDC, CSS_TC_LPAREN}},
      {{CSS_TC_ATKW, CSS_TC_HASH, CSS_TC_DIM}, {CSS_TC_IDENT, CSS_TC_FUNC, CSS_TC_URL, CSS_TC_BADURL, CSS_TC_D_MINUS, CSS_TC_NUM, CSS_TC_PCT, CSS_TC_DIM, CSS_TC_CDC}},
      {{CSS_TC_D_HASH, CSS_TC_D_MINUS}, {CSS_TC_IDENT, CSS_TC_FUNC, CSS_TC_URL, CSS_TC_BADURL, CSS_TC_D_MINUS, CSS_TC_NUM, CSS_TC_PCT, CSS_TC_DIM}},
      {{CSS_TC_NUM}, {CSS_TC_IDENT, CSS_TC_FUNC, CSS_TC_URL, CSS_TC_BADURL, CSS_TC_NUM, CSS_TC_PCT, CSS_TC_DIM, CSS_TC_D_PCT}},
      {{CSS_TC_D_AT}, {CSS_TC_IDENT, CSS_TC_FUNC, CSS_TC_URL, CSS_TC_BADURL, CSS_TC_D_MINUS}},
      {{CSS_TC_D_DOT, CSS_TC_D_PLUS}, {CSS_TC_NUM, CSS_TC_PCT, CSS_TC_DIM}},
      {{CSS_TC_D_SLASH}, {CSS_TC_D_STAR}},
  };
  for (int l = CSS_TC_NONE; l <= CSS_TC_D_STAR; l++) {
    for (int f = CSS_TC_NONE; f <= CSS_TC_D_STAR; f++) {
      bool want = false;
      for (const Row & row : rows) {
        if (std::find(row.lasts.begin(), row.lasts.end(), (TC)l) != row.lasts.end() && std::find(row.firsts.begin(), row.firsts.end(), (TC)f) != row.firsts.end()) {
          want = true;
        }
      }
      EXPECT_EQ(css_tokclass_needs_comment((TC)l, (TC)f), want) << "last " << l << " first " << f;
    }
  }
}

TEST(CssCustomReview, ASubstitutionThatWouldGlueTwoTokensIsSeparatedByAComment) {
  World w;
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  Computed c;
  compute_pairs(w, c, {{"--a", "1"}, {"--b", "var(--a)var(--a)"}, {"--c", "var(--a)%"}, {"--d", "foo"}, {"--e", "var(--d)var(--d)"}, {"--g", "var(--a) var(--a)"}});
  EXPECT_EQ(c.custom("--b"), "1/**/1") << "two numbers would be one";
  EXPECT_EQ(c.custom("--c"), "1/**/%") << "a number and a percent sign would be a percentage";
  EXPECT_EQ(c.custom("--e"), "foo/**/foo");
  EXPECT_EQ(c.custom("--g"), "1 1") << "white space between them: nothing to add";
}

/* ---- if() with a condition that is not known ---------------------------------------------------------------------------------- */

std::string if_of(const std::string & expr) {
  World w;
  w.condition = [](GTEXT_CSS_Condition_Kind, const std::string & t) {
    return t == "y" ? GTEXT_CSS_TRI_TRUE : t == "n" ? GTEXT_CSS_TRI_FALSE : GTEXT_CSS_TRI_UNKNOWN;
  };
  EXPECT_EQ(w.build(), GTEXT_CSS_OK);
  Computed c;
  compute_pairs(w, c, {{"--r", expr}});
  EXPECT_EQ(c.status, GTEXT_CSS_OK) << expr;
  return c.custom("--r");
}

TEST(CssCustomReview, AnUnknownConditionIsNeitherTrueNorFalseAndIsFalseAtTheTop) {
  // "Unknown at the top of an if() condition is false", and the three-valued logic of and, or and not before that
  EXPECT_EQ(if_of("if(media(u): A; else: B)"), "B");
  EXPECT_EQ(if_of("if(media(y) and media(u): A; else: B)"), "B") << "true and unknown is unknown";
  EXPECT_EQ(if_of("if(media(u) and media(y): A; else: B)"), "B");
  EXPECT_EQ(if_of("if(not (media(y) and media(u)): A; else: B)"), "B") << "not unknown is unknown";
  EXPECT_EQ(if_of("if(not (media(n) and media(u)): A; else: B)"), "A") << "false and unknown is false";
  EXPECT_EQ(if_of("if(media(n) or media(u): A; else: B)"), "B");
  EXPECT_EQ(if_of("if(not (media(n) or media(u)): A; else: B)"), "B") << "false or unknown is unknown";
  EXPECT_EQ(if_of("if(media(y) or media(u): A; else: B)"), "A") << "true or unknown is true";
  EXPECT_EQ(if_of("if(not media(u): A; else: B)"), "B");
  EXPECT_EQ(if_of("if(not media(n): A; else: B)"), "A");
}

/* ---- random() with a base that is not in [0, 1) ------------------------------------------------------------------------------- */

std::string random_of(double base, bool answer) {
  World w;
  w.random_base = [base, answer](const GTEXT_CSS_Random_Key *, double * out) {
    *out = base;
    return answer;
  };
  EXPECT_EQ(w.build(), GTEXT_CSS_OK);
  Computed c;
  compute_pairs(w, c, {{"width", "random(0px, 100px)"}});
  EXPECT_EQ(c.status, GTEXT_CSS_OK);
  return c.value(w.registry.r, "width");
}

TEST(CssCustomReview, ABaseFromTheEnvironmentIsUsedOnlyWhenItIsInZeroToOne) {
  EXPECT_EQ(random_of(0.25, true), "25px");
  EXPECT_EQ(random_of(0, true), "0px");
  // a base of 1 or more, below 0, or not a number is not one; the seeded one stands in, and is in [0, 100) px
  for (const double bad : {1.0, 1.5, -0.25, std::nan("")}) {
    const std::string got = random_of(bad, true);
    ASSERT_TRUE(got.size() > 2 && got.substr(got.size() - 2) == "px") << got;
    const double v = std::stod(got);
    EXPECT_GE(v, 0.0) << bad;
    EXPECT_LT(v, 100.0) << bad;
  }
  // the callback that declines is the seeded one as well
  const std::string declined = random_of(0.25, false);
  EXPECT_NE(declined, "25px");
}

/* ---- an undeclared registered property in a child ---------------------------------------------------------------------------- */

TEST(CssCustomReview, AnUndeclaredRegisteredPropertyThatDoesNotInheritIsItsInitialValueEvenWhenTheParentDeclaredIt) {
  Scene s({}, {{"--n", "<length>", "1px", false, true, false}, {"--i", "<length>", "2px", true, true, false}});
  Computed parent;
  compute(s.w, parent, "--n: 8px; --i: 9px");
  Computed child;
  compute(s.w, child, "width: var(--n); height: var(--i)", &parent);
  ASSERT_EQ(child.status, GTEXT_CSS_OK);
  EXPECT_EQ(child.value(s.w.registry.r, "width"), "1px") << "does not inherit";
  EXPECT_EQ(child.value(s.w.registry.r, "height"), "9px") << "inherits";
}


/* ---- ident() ------------------------------------------------------------------------------------------------------------------- */

TEST(CssCustomReview, IdentTakesStringsIdentifiersAndIntegersAndWritesTheJoinAsAnIdentifier) {
  World w;
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  auto custom = [&](const std::string & text) {
    Computed c;
    compute_pairs(w, c, {{"--r", text}});
    EXPECT_EQ(c.status, GTEXT_CSS_OK) << text;
    return c.custom("--r");
  };
  EXPECT_EQ(custom("ident(foo \"bar\" 12)"), "foobar12");
  EXPECT_EQ(custom("ident(foo 1.5)"), "<gi>") << "a number that is not an integer is not an item";
  EXPECT_EQ(custom("ident(foo 1e1)"), "<gi>");
  EXPECT_EQ(custom("ident(\"a b\")"), "a\\ b") << "the join is written as an identifier: the space is escaped";
  EXPECT_EQ(custom("ident(\"\")"), "<gi>");
  EXPECT_EQ(custom("ident()"), "<gi>");
  EXPECT_EQ(custom("ident(foo, bar)"), "<gi>");
}


TEST(CssCustomReview, TheElementTokenBudgetIsInclusiveAndAValueIsHeldToTheLimitOfOne) {
  // a value of three words is five tokens (the white space counts); it is charged once where it is declared and once where it is used: ten for the element
  auto run = [](uint64_t element, uint64_t value) {
    World w;
    w.options.substitute.max_element_substituted_tokens = element;
    w.options.substitute.max_substituted_tokens = value;
    EXPECT_EQ(w.build(), GTEXT_CSS_OK);
    auto c = std::make_unique<Computed>();
    compute_pairs(w, *c, {{"--a", "w w w"}, {"--b", "var(--a)"}});
    EXPECT_EQ(c->status, GTEXT_CSS_OK);
    return c->custom("--b");
  };
  EXPECT_EQ(run(10, 100), "w w w") << "exactly what the element may use";
  EXPECT_EQ(run(9, 100), "<gi>") << "one token more than it may";
  // a single value that is longer than the limit of a value is refused where it is used, though no join of it was made
  EXPECT_EQ(run(100, 5), "w w w");
  EXPECT_EQ(run(100, 4), "<gi>");
}

TEST(CssCustomReview, TheResultOfAFunctionIsHeldToTheBytesOfAValue) {
  // ident() writes what it is given: a result past the limit of a value is the guaranteed-invalid value, the argument being the use of a value that fits
  World w;
  w.options.substitute.max_value_bytes = 100;
  ASSERT_EQ(w.build(), GTEXT_CSS_OK);
  Computed c;
  compute_pairs(w, c, {{"--s", "\"" + std::string(60, 'a') + "\""}, {"--ok", "ident(var(--s))"}, {"--long", "ident(var(--s) var(--s))"}});
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.custom("--ok"), std::string(60, 'a'));
  EXPECT_EQ(c.custom("--long"), "<gi>") << "120 bytes";
}


/* ---- attr(): namespaces and self reference ------------------------------------------------------------------------------------ */

TEST(CssCustomReview, AnAttributeIsLookedUpInTheNamespaceThatIsWritten) {
  Scene s({{"svg|href", "NS"}, {"href", "PLAIN"}});
  Computed c;
  compute_pairs(s.w, c, {{"--n1", "attr(svg|href)"}, {"--n2", "attr(|href)"}, {"--n3", "attr(href)"}}, nullptr, &s.node);
  EXPECT_EQ(c.custom("--n1"), "\"NS\"");
  EXPECT_EQ(c.custom("--n2"), "\"PLAIN\"") << "`|name` is no namespace, said explicitly";
  EXPECT_EQ(c.custom("--n3"), "\"PLAIN\"");
}

TEST(CssCustomReview, AnAttributeThatNamesItselfIsACycleAndAFallbackTakesItsPlace) {
  // the text of the attribute is parsed as CSS and replaced under the context of its name; an attribute that holds attr() of itself, or two that hold each other
  Scene s({{"a", "attr(a type(*))"}, {"b", "attr(c type(*))"}, {"c", "attr(b type(*), 9px)"}});
  Computed c;
  compute_pairs(s.w, c, {{"--x", "attr(a type(*), 7px)"}, {"--y", "attr(a type(*))"}, {"--z", "attr(b type(*), 5px)"}}, nullptr, &s.node);
  ASSERT_EQ(c.status, GTEXT_CSS_OK);
  EXPECT_EQ(c.custom("--x"), "7px");
  EXPECT_EQ(c.custom("--y"), "<gi>");
  EXPECT_EQ(c.custom("--z"), "5px");
}

/* ---- registered properties that depend on each other ------------------------------------------------------------------------- */

TEST(CssCustomReview, RegisteredPropertiesInACycleAreTheirInitialValues) {
  Scene s({}, {{"--a", "<length>", "5px", true, true, false}, {"--b", "<length>", "6px", true, true, false}});
  Computed c;
  compute_pairs(s.w, c, {{"--a", "var(--b)"}, {"--b", "var(--a)"}, {"width", "var(--a)"}});
  EXPECT_EQ(c.custom("--a"), "5px");
  EXPECT_EQ(c.custom("--b"), "6px");
  EXPECT_EQ(c.value(s.w.registry.r, "width"), "5px");
}

TEST(CssCustomReview, AnInitialValueThatNeedsTheElementIsRefused) {
  for (const char * initial : {"currentcolor", "CurrentColor", "color-mix(in srgb, currentcolor, red)"}) {
    World w;
    ASSERT_EQ(w.build({{"--c", "<color>", initial, true, true, false}}), GTEXT_CSS_OK);
    EXPECT_EQ(w.refused.size(), 1u) << initial;
  }
  World ok;
  ASSERT_EQ(ok.build({{"--c", "<color>", "red", true, true, false}, {"--d", "<color>", "light-dark(red, blue)", true, true, false}}), GTEXT_CSS_OK);
  EXPECT_TRUE(ok.refused.empty());
}

/* ---- the depth, to the frame ------------------------------------------------------------------------------------------------- */

/// A chain of `n` custom properties that each use the one before, named so that the last link sorts first (it is the one the computation reaches first, and every
/// link under it is then reached from it: the whole depth of the chain at once), used by `width`, in a block that may hold that many declarations.
bool chain_is_refused(size_t depth, int n) {
  World w;
  w.options.substitute.max_substitution_depth = depth;
  EXPECT_EQ(w.build(), GTEXT_CSS_OK);
  auto name = [n](int i) {
    char b[32];
    std::snprintf(b, sizeof b, "--c%07d", n - i);
    return std::string(b);
  };
  GTEXT_CSS_Declaration_Options o = gtext_css_declaration_options_default();
  o.max_declarations = (size_t)1 << 22;
  Computed c;
  c.block = std::make_unique<cmp::Block>(w.registry.r, "", &o);
  auto add = [&](const std::string & k, const std::string & v) {
    GTEXT_CSS_Property_Outcome outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
    gtext_css_block_add_text(c.block->b, k.data(), k.size(), v.data(), v.size(), false, nullptr, &outcome, nullptr);
  };
  add(name(0), "1px");
  for (int i = 1; i <= n; i++) {
    add(name(i), "var(" + name(i - 1) + ")");
  }
  add("width", "var(" + name(n) + ")");
  add("height", "6px");
  c.status = gtext_css_compute_style(w.ctx, c.block->b, nullptr, nullptr, &c.s, &c.result);
  EXPECT_EQ(c.status, GTEXT_CSS_OK) << "the element is computed, whatever the depth of its chains";
  EXPECT_EQ(c.value(w.registry.r, "height"), "6px");
  const bool refused = c.value(w.registry.r, "width") == "auto";
  EXPECT_EQ(refused, c.result.substitution_limited >= 1) << "a chain that is refused is counted";
  return refused;
}

TEST(CssCustomReview, TheDepthIsInFramesAndAChainPastItIsTheGuaranteedInvalidValueNotAFailure) {
  // a short chain takes four frames a link and a long one three (the sequence of the first links is deeper than the rest): the boundary is exact, and the check is
  // "at": a chain that makes the depth in frames is refused
  EXPECT_FALSE(chain_is_refused(4001, 1000));
  EXPECT_TRUE(chain_is_refused(4000, 1000));
  EXPECT_FALSE(chain_is_refused(17049, 5000));
  EXPECT_TRUE(chain_is_refused(17048, 5000));
}

TEST(CssCustomReview, FunctionsNestedPastTheDepthAreALimitAndTheBoundaryIsExact) {
  // a nested var() is two frames a level; "nested deeper than max_depth" is an error of the element, unlike a chain of custom properties
  auto status = [](size_t depth, int levels) {
    World w;
    w.options.substitute.max_substitution_depth = depth;
    EXPECT_EQ(w.build(), GTEXT_CSS_OK);
    std::string v;
    for (int i = 0; i < levels; i++) {
      v += "var(--n, ";
    }
    v += "1px" + std::string((size_t)levels, ')');
    Computed c;
    compute_pairs(w, c, {{"width", v}});
    return c.status;
  };
  EXPECT_EQ(status(50, 24), GTEXT_CSS_OK);
  EXPECT_EQ(status(50, 25), GTEXT_CSS_E_LIMIT_DEPTH);
  EXPECT_EQ(status(51, 25), GTEXT_CSS_OK);
  EXPECT_EQ(status(51, 26), GTEXT_CSS_E_LIMIT_DEPTH);
}

TEST(CssCustomReview, TheDefaultDepthTakesAboutThirtyThreeThousandLinks) {
  // documented: 100,000 frames, three to a link in a long chain
  EXPECT_FALSE(chain_is_refused(100000, 32650));
  EXPECT_TRUE(chain_is_refused(100000, 32651));
}

}  // namespace
