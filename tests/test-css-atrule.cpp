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
 * The typed at-rule model (css_atrule.h): the prelude and the descriptors of each kind, the required descriptors, the places, the diagnostics and the serialization.
 */

#include <string>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_cascade_test_support.h"
#include "css_test_support.h"

using cas::R;

namespace {

struct Parsed {
  css_test::Parsed p;
  GTEXT_CSS_Atrule * a = nullptr;
  GTEXT_CSS_Atrule_Result res{};
  GTEXT_CSS_Status st = GTEXT_CSS_E_INTERNAL;
  explicit Parsed(const std::string & css, GTEXT_CSS_Atrule_Parent parent = GTEXT_CSS_ATRULE_PARENT_TOP) {
    css_test::parse_into(p, gtext_css_parse_stylesheet, css);
    EXPECT_NE(p.tree, nullptr);
    EXPECT_GE(p.tree->rule_count, 1u);
    GTEXT_CSS_Atrule_Options o = gtext_css_atrule_options_default();
    o.parent = parent;
    st = gtext_css_atrule_parse(R(), &p.tree->rules[0], &o, &a, &res);
  }
  Parsed(const Parsed &) = delete;
  Parsed & operator=(const Parsed &) = delete;
  ~Parsed() {
    gtext_css_atrule_free(a);
  }
  std::string css() const {
    if (!a) {
      return "<none>";
    }
    char * buf = nullptr;
    size_t n = 0;
    EXPECT_EQ(gtext_css_atrule_serialize_buffer(a, nullptr, &buf, &n), GTEXT_CSS_OK);
    std::string out(buf, n);
    gtext_css_atrule_buffer_free(nullptr, buf);
    return out;
  }
  std::string desc(const char * name) const {
    GTEXT_CSS_Atrule_Descriptor d;
    if (!a || !gtext_css_atrule_descriptor_find(a, name, std::strlen(name), &d)) {
      return "-";
    }
    return std::string(d.text.data, d.text.length);
  }
};

}  // namespace

// Kinds, by the name of the rule.
TEST(CssAtrule, Kinds) {
  struct { const char * css; GTEXT_CSS_Atrule_Kind kind; } rows[] = {
    {"@font-face { font-family: A; src: url(a) }", GTEXT_CSS_ATRULE_FONT_FACE},
    {"@counter-style x { system: cyclic; symbols: a }", GTEXT_CSS_ATRULE_COUNTER_STYLE},
    {"@keyframes k { from { color: red } }", GTEXT_CSS_ATRULE_KEYFRAMES},
    {"@-webkit-keyframes k { from { color: red } }", GTEXT_CSS_ATRULE_KEYFRAMES},
    {"@view-transition { navigation: auto }", GTEXT_CSS_ATRULE_VIEW_TRANSITION},
    {"@position-try --a { top: 0 }", GTEXT_CSS_ATRULE_POSITION_TRY},
    {"@page { margin: 1in }", GTEXT_CSS_ATRULE_PAGE},
    {"@color-profile --p { src: url(a.icc) }", GTEXT_CSS_ATRULE_COLOR_PROFILE},
    {"@font-palette-values --x { font-family: A }", GTEXT_CSS_ATRULE_FONT_PALETTE_VALUES},
    {"@font-feature-values A { }", GTEXT_CSS_ATRULE_FONT_FEATURE_VALUES},
    {"@property --a { syntax: \"*\"; inherits: false }", GTEXT_CSS_ATRULE_PROPERTY},
  };
  for (const auto & r : rows) {
    Parsed p(r.css);
    ASSERT_EQ(p.st, GTEXT_CSS_OK) << r.css;
    ASSERT_NE(p.a, nullptr) << r.css;
    EXPECT_EQ(gtext_css_atrule_kind(p.a), r.kind) << r.css;
    EXPECT_TRUE(p.res.valid) << r.css;
  }
}

// The required descriptors: a rule without them is ignored (or defines nothing), as the served text says.
TEST(CssAtrule, Required) {
  {
    Parsed p("@keyframes none { from { color: red } }");
    EXPECT_FALSE(p.res.valid);
    EXPECT_EQ(p.a, nullptr);
  }
  {
    Parsed p("@font-palette-values --x { base-palette: 1 }");
    EXPECT_FALSE(p.res.valid);
  }
  {
    Parsed p("@font-face { font-family: A }");
    EXPECT_TRUE(p.res.valid);  // valid, but not considered: gtext_css_font_face_defined() says so
    EXPECT_EQ(p.desc("src"), "-");
  }
  {
    Parsed p("@counter-style x { system: cyclic }");
    EXPECT_TRUE(p.res.valid);
    EXPECT_TRUE(gtext_css_atrule_defines_nothing(p.a));
  }
}

// An invalid or unknown descriptor is dropped with a diagnostic; the rule stands.
TEST(CssAtrule, DescriptorsDropped) {
  Parsed p("@font-face { font-family: A; src: url(a); font-weight: bogus; nonsense: 1; font-display: swap }");
  ASSERT_NE(p.a, nullptr);
  EXPECT_EQ(p.desc("font-weight"), "-");
  EXPECT_EQ(p.desc("font-display"), "swap");
  EXPECT_EQ(gtext_css_atrule_diagnostic_total(p.a), 2u);
  GTEXT_CSS_Atrule_Diagnostic d;
  ASSERT_TRUE(gtext_css_atrule_diagnostic(p.a, 0, &d));
  EXPECT_EQ(d.kind, GTEXT_CSS_ATRULE_DIAG_INVALID_DESCRIPTOR);
  ASSERT_TRUE(gtext_css_atrule_diagnostic(p.a, 1, &d));
  EXPECT_EQ(d.kind, GTEXT_CSS_ATRULE_DIAG_UNKNOWN_DESCRIPTOR);
  EXPECT_NE(d.reference, nullptr);
}

// A later descriptor of one name replaces an earlier one.
TEST(CssAtrule, LaterDescriptorWins) {
  Parsed p("@font-face { font-family: A; src: url(a); font-display: swap; font-display: block }");
  EXPECT_EQ(p.desc("font-display"), "block");
}

// The places an at-rule may be read.
TEST(CssAtrule, Parents) {
  Parsed p("@font-face { font-family: A; src: url(a) }", GTEXT_CSS_ATRULE_PARENT_STYLE);
  EXPECT_FALSE(p.res.valid);
  Parsed q("@top-left { content: \"x\" }", GTEXT_CSS_ATRULE_PARENT_PAGE);
  EXPECT_TRUE(q.res.valid);
  EXPECT_EQ(gtext_css_atrule_kind(q.a), GTEXT_CSS_ATRULE_PAGE_MARGIN);
  Parsed r("@top-left { content: \"x\" }");
  EXPECT_FALSE(r.res.valid);
  Parsed f("@styleset { nice: 12 }", GTEXT_CSS_ATRULE_PARENT_FEATURE_VALUES);
  EXPECT_TRUE(f.res.valid);
  EXPECT_EQ(gtext_css_atrule_kind(f.a), GTEXT_CSS_ATRULE_FEATURE_BLOCK);
}

// Keyframes: from and to are 0% and 100%; !important and the animation properties are ignored; an invalid selector drops its block.
TEST(CssAtrule, KeyframesBlocks) {
  Parsed p("@keyframes k { from { color: red } 50%, 60% { color: blue; animation-name: x } to { color: green !important } 150% { color: red } bad { color: red } }");
  ASSERT_NE(p.a, nullptr);
  EXPECT_EQ(p.css(), "@keyframes k {\n  0% { color: red; }\n  50%, 60% { color: blue; }\n  100% { }\n}");
  EXPECT_EQ(gtext_css_atrule_child_count(p.a), 3u);
}

// Serialization is a fixed point.
TEST(CssAtrule, SerializeFixedPoint) {
  for (const char * css : {"@font-face { font-family: A; src: url(a.woff2) format(woff2), local(\"X y\"); unicode-range: U+4??, U+25-FF; font-weight: 200 900 }",
           "@counter-style x { system: additive; additive-symbols: 2 b, 1 a; negative: \"-\" \"!\"; pad: 3 \"0\" }",
           "@page :first { size: A4 landscape; margin: 1in; @top-left { content: \"x\" } }",
           "@font-feature-values A, \"B c\" { font-display: swap; @styleset { nice: 12 3 } @swash { s: 1 } }",
           "@view-transition { navigation: auto; types: a b }", "@position-try --a { top: anchor(bottom) }"}) {
    Parsed p(css);
    ASSERT_NE(p.a, nullptr) << css;
    const std::string once = p.css();
    Parsed q(once);
    ASSERT_NE(q.a, nullptr) << once;
    EXPECT_EQ(q.css(), once) << css;
  }
}

// @position-try and keyframes drop the declarations the served text does not allow.
TEST(CssAtrule, PositionTryProperties) {
  Parsed p("@position-try --a { top: anchor(bottom); color: red }");
  ASSERT_NE(p.a, nullptr);
  EXPECT_EQ(p.desc("top"), "anchor(bottom)");
  EXPECT_EQ(p.desc("color"), "-");
}

// The ledger says which kinds are parsed, validated, serialized and typed.
TEST(CssAtrule, Ledger) {
  EXPECT_GT(gtext_css_atrule_ledger_count(), 20u);
  for (size_t i = 0; i < gtext_css_atrule_ledger_count(); i++) {
    const char * name = nullptr;
    bool a, b, c, d, e, f;
    ASSERT_TRUE(gtext_css_atrule_ledger_row(i, &name, &a, &b, &c, &d, &e, &f));
    EXPECT_NE(name, nullptr);
  }
}

// The validity seam: with the callback a parse drops an at-rule that is invalid where it stands, and keeps the same tree as a parse without it when every one is valid.
TEST(CssAtrule, ValiditySeam) {
  const std::string valid = "@font-face { font-family: A; src: url(a) } @keyframes k { from { color: red } } @page :first { margin: 1in; @top-left { content: \"x\" } } "
                            "@counter-style c { system: cyclic; symbols: a } a { color: red } @media print { @font-face { font-family: B; src: url(b) } }";
  GTEXT_CSS_Atrule_Validity v;
  gtext_css_atrule_validity_init(&v, R());
  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  po.validity = v.validity;
  css_test::Parsed with, without;
  with.status = gtext_css_parse_stylesheet(valid.data(), valid.size(), &po, &with.tree, &with.result);
  css_test::parse_into(without, gtext_css_parse_stylesheet, valid);
  ASSERT_NE(with.tree, nullptr);
  ASSERT_NE(without.tree, nullptr);
  EXPECT_EQ(with.tree->node_count, without.tree->node_count);
  EXPECT_EQ(with.tree->rule_count, without.tree->rule_count);
  EXPECT_GT(v.checked, 4u);
  EXPECT_EQ(v.refused, 0u);
  // invalid where they stand: an @keyframes named by a CSS-wide keyword, a margin rule at the top, an @font-face in a style rule
  const std::string bad = "@keyframes none { from { color: red } } @top-left { content: \"x\" } a { @font-face { font-family: A; src: url(a) } color: red } @keyframes ok { to { color: red } }";
  GTEXT_CSS_Atrule_Validity w;
  gtext_css_atrule_validity_init(&w, R());
  po.validity = w.validity;
  css_test::Parsed dropped, kept;
  dropped.status = gtext_css_parse_stylesheet(bad.data(), bad.size(), &po, &dropped.tree, &dropped.result);
  css_test::parse_into(kept, gtext_css_parse_stylesheet, bad);
  ASSERT_NE(dropped.tree, nullptr);
  EXPECT_EQ(kept.tree->rule_count, 4u);
  EXPECT_EQ(dropped.tree->rule_count, 2u);  // `a {...}` and @keyframes ok
  EXPECT_EQ(w.refused, 3u);
}

// Fonts 4, 4.3.1: a parse error in one component of the source list does not invalidate the descriptor; it is invalid only when every component is.
TEST(CssAtrule, SrcListIsForgiving) {
  struct { const char * src; bool valid; const char * canon; } rows[] = {
    {"local(inherit), url(foo.ttf)", true, "url(\"foo.ttf\")"},
    {"local(\"myfont\"), local(unset)", true, "local(\"myfont\")"},
    {"local(), url(foo.ttf)", true, "url(\"foo.ttf\")"},
    {"url(foo.ttf) format(bad), url(foo.ttf)", true, "url(\"foo.ttf\")"},
    {"/* an empty component */, url(foo.ttf)", true, "url(\"foo.ttf\")"},
    {"local(\"\"), url(foo.ttf), unparseable-garbage, local(\"another font name\")", true, "local(\"\"), url(\"foo.ttf\"), local(\"another font name\")"},
    {"local(), local(initial)", false, ""},
    {"local(\"textfont\") format(opentype), local(\"emoji\") tech(color-COLRv0)", false, ""},
    {"url(foo.ttf) tech(color-COLRv0) otherfunc(othervalue), junk", false, ""},
  };
  for (const auto & r : rows) {
    Parsed p(std::string("@font-face { src: ") + r.src + " }");
    ASSERT_NE(p.a, nullptr) << r.src;
    const std::string got = p.desc("src");
    if (r.valid) {
      EXPECT_EQ(got, r.canon) << r.src;
    }
    else {
      EXPECT_EQ(got, "-") << r.src;
    }
  }
}

// Animations 2: a keyframe selector may be a timeline range with a percentage or a calc(); it is kept as written and has no offset in the normalized list.
TEST(CssAtrule, KeyframeSelectorsOfTimelines) {
  Parsed p("@keyframes k { entry 10% { color: red } calc(10%) { color: blue } 50% { color: green } exit calc(60%) { width: 1px } }");
  ASSERT_NE(p.a, nullptr);
  EXPECT_EQ(gtext_css_atrule_child_count(p.a), 4u);
  EXPECT_EQ(p.css(), "@keyframes k {\n  entry 10% { color: red; }\n  calc(10%) { color: blue; }\n  50% { color: green; }\n  exit calc(60%) { width: 1px; }\n}");
  GTEXT_CSS_Keyframes * k = nullptr;
  ASSERT_EQ(gtext_css_keyframes_from_atrule(p.a, &k), GTEXT_CSS_OK);
  EXPECT_EQ(k->block_count, 4u);
  EXPECT_EQ(k->frame_count, 1u);
  gtext_css_keyframes_free(k);
  Parsed e("@keyframes \"\" { }");
  EXPECT_FALSE(e.res.valid);
  Parsed bad("@keyframes k { entry { color: red } 101% { color: red } cover 1% 2% { color: red } }");
  EXPECT_EQ(gtext_css_atrule_child_count(bad.a), 0u);
}

// An empty source list has no value to take components from (a NULL plus zero once reached the sanitizer).
TEST(CssAtrule, EmptySrcIsDropped) {
  Parsed p("@font-face { font-family: A; src: }");
  ASSERT_NE(p.a, nullptr);
  EXPECT_EQ(p.desc("src"), "-");
}

// An at-rule name that is not a plain identifier (an escape that decodes to a line feed) is written escaped, so the text reads back as the same rule.
TEST(CssAtrule, UnknownNameIsSerializedEscaped) {
  Parsed p("@p\\\\\\a ge :bogus { }");
  ASSERT_NE(p.a, nullptr);
  const std::string once = p.css();
  Parsed q(once);
  ASSERT_NE(q.a, nullptr) << once;
  EXPECT_EQ(q.css(), once);
}

// @charset is exactly one string; a bad string or anything else is not an @charset rule.
TEST(CssAtrule, CharsetIsOneString) {
  Parsed ok("@charset \"utf-8\";");
  EXPECT_TRUE(ok.res.valid);
  EXPECT_EQ(ok.css(), "@charset \"utf-8\";");
  Parsed bad("@charset \"utf-8\n;");
  EXPECT_FALSE(bad.res.valid);
  Parsed two("@charset \"a\" \"b\";");
  EXPECT_FALSE(two.res.valid);
  Parsed ident("@charset utf-8;");
  EXPECT_FALSE(ident.res.valid);
}

// A bad string in a prelude cannot be written back as text (it would swallow what follows): the rule is invalid, at any depth.
TEST(CssAtrule, BadStringInPreludeIsInvalid) {
  Parsed a("@char]set \"utf-8interese\n { }");
  EXPECT_FALSE(a.res.valid);
  Parsed b("@unknown-thing (a \"b\n) { }");
  EXPECT_FALSE(b.res.valid);
  Parsed c("@unknown-thing (a \"b\") { }");
  EXPECT_TRUE(c.res.valid);
}

// A family name that is one identifier holding an escaped space reads back as the same name.
TEST(CssAtrule, EscapedSpaceInAFamilyName) {
  Parsed p("@font-feature-values \\  { @styleset { a: 2 } }");
  ASSERT_NE(p.a, nullptr);
  const std::string once = p.css();
  Parsed q(once);
  ASSERT_NE(q.a, nullptr) << once;
  EXPECT_EQ(q.css(), once);
}

// A feature value name or a custom name that needs an escape is written escaped.
TEST(CssAtrule, ChosenNamesAreEscapedOnOutput) {
  Parsed p("@font-feature-values A { @styleset { n\\1 e: 2 } }");
  ASSERT_NE(p.a, nullptr);
  const std::string once = p.css();
  Parsed q(once);
  ASSERT_NE(q.a, nullptr) << once;
  EXPECT_EQ(q.css(), once);
}
