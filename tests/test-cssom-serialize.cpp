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
 * `cssText` of every kind of rule and the serialization of a sheet (css_cssom_rule.h, css_cssom_sheet.h): CSSOM's "serialize a CSS rule", the shapes of a style rule with rules
 * inside it, of a grouping rule and of the rules the typed model writes, the fixed point (parsing the text gives a sheet that writes the same bytes), the cache of a leaf rule
 * and its bound, and the nests and sheets the writer takes with no recursion.
 */

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cssom_test_support.h"

using namespace cot;

namespace {

/// The text of the first rule of a sheet.
std::string first(const std::string & css) {
  Doc doc;
  Sheet s(doc, css);
  EXPECT_EQ(s.status, GTEXT_CSS_OK) << css;
  Rule r = at(s.rules(), 0);
  return r ? ::css(r.r) : "<no rule>";
}

/* ---- the shapes --------------------------------------------------------------------------------------------------------------------------------- */

// CSSOM, CSSStyleRule: "Let s initially be the result of performing serialize a group of selectors ... followed by the string ' {' ... If decls and rules are both null, append
// ' }' ... If rules is null: Append a single SPACE to s, Append decls to s, Append ' }' ... Otherwise: If decls is not null, prepend it to rules. For each rule in rules ... Append
// a newline followed by two spaces to s. Append rule to s. Append a newline followed by RIGHT CURLY BRACKET (U+007D) to s."
TEST(CssomSerializeStyle, TheShapesOfAStyleRule) {
  EXPECT_EQ(first("a {}"), "a { }");
  EXPECT_EQ(first("a {color:red; width:1px}"), "a { color: red; width: 1px; }");
  EXPECT_EQ(first("a { & b {} }"), "a {\n  & b { }\n}");
  EXPECT_EQ(first("a { color:red; & b {} width: 1px; }"), "a {\n  color: red;\n  & b { }\n  width: 1px;\n}");
  EXPECT_EQ(first("a,b>c , d + e{color:red}"), "a, b > c, d + e { color: red; }");
  EXPECT_EQ(first("a { color:red; & b {} }"), "a {\n  color: red;\n  & b { }\n}");
}

// The text of a rule inside a rule is not indented again: "Append a newline followed by two spaces to s. Append rule to s." (both browsers write it so, and the draft calls it
// matching browsers).
TEST(CssomSerializeStyle, TheRulesInsideAreNotIndentedAgain) {
  EXPECT_EQ(first("a { & b { & c { color:red; & d {} } } }"), "a {\n  & b {\n  & c {\n  color: red;\n  & d { }\n}\n}\n}");
  EXPECT_EQ(first("@media print { @media screen { a { color: red; } b{} } c{} }"), "@media print {\n  @media screen {\n  a { color: red; }\n  b { }\n}\n  c { }\n}");
}

// CSS Nesting: a relative selector is written with its implied "&".
TEST(CssomSerializeStyle, ANestedSelectorIsWrittenWithItsAmpersand) {
  EXPECT_EQ(first("x { c{} > d{} & e{} .f & {} &.g{} }"), "x {\n  & c { }\n  & > d { }\n  & e { }\n  .f & { }\n  &.g { }\n}");
  EXPECT_EQ(first("x { c, > d {} }"), "x {\n  & c, & > d { }\n}");
  EXPECT_EQ(first("x { @media print { c{} > d{} } }"), "x {\n  @media print {\n  & c { }\n  & > d { }\n}\n}");
  EXPECT_EQ(first("@media print { & c{} }"), "@media print {\n  & c { }\n}");
}

// A style rule directly in an @scope is read as it is, and a leading combinator is written as the browsers write it.
TEST(CssomSerializeStyle, ARuleInAScopeIsNotWrittenWithAnAmpersand) {
  EXPECT_EQ(first("@scope (.a) { c{} > d{} & e{} :scope f{} }"), "@scope (.a) {\n  c { }\n  > d { }\n  & e { }\n  :scope f { }\n}");
  EXPECT_EQ(first("@scope (.a) { @media print { c{} > d{} } }"), "@scope (.a) {\n  @media print {\n  c { }\n  > d { }\n}\n}");
  EXPECT_EQ(first("@scope (.a) { c, > d {} }"), "@scope (.a) {\n  c, > d { }\n}");
}

// The namespaces a selector is read in: "x|a", and a "*|" only where the default namespace needs it; a rule keeps them after it left its sheet.
TEST(CssomSerializeStyle, SelectorsKeepTheirNamespaces) {
  Doc doc;
  Sheet s(doc, "@namespace x url(u); x|a, *|b, |c, d {color:red}");
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  EXPECT_EQ(css(at(s.rules(), 1).r), "x|a, b, |c, d { color: red; }");
  Sheet t(doc, "@namespace url(u); a{} *|b{} |c{}");
  ASSERT_EQ(t.status, GTEXT_CSS_OK);
  EXPECT_EQ(css_list(t.rules()), (std::vector<std::string>{"@namespace url(\"u\");", "a { }", "*|b { }", "|c { }"}));
  Rule kept = at(t.rules(), 2);
  gtext_cssom_sheet_compact(t.s);
  EXPECT_EQ(css(kept.r), "*|b { }") << "after a compaction";
}

// CSS Nesting: the declarations after a nested rule are a rule of their own, and the grouping rule inside a style rule holds them as rules too.
TEST(CssomSerializeStyle, NestedDeclarationsRulesAreTheirDeclarations) {
  Doc doc;
  Sheet s(doc, "a { & b{} color:red; width:1px }");
  Rule nd = path(s, {0, 1});
  ASSERT_EQ(gtext_cssom_rule_kind(nd.r), GTEXT_CSSOM_RULE_NESTED_DECLARATIONS);
  EXPECT_EQ(css(nd.r), "color: red; width: 1px;");
  EXPECT_EQ(first("a { @media print { color: red; & b {} } }"), "a {\n  @media print {\n  color: red;\n  & b { }\n}\n}");
  EXPECT_EQ(first("a { @media print { color: red; } }"), "a {\n  @media print {\n  color: red;\n}\n}");
}

// CSSOM, CSSMediaRule: "The string '@media', followed by a single SPACE. The result of performing serialize a media query list ... A single SPACE, followed by the string '{', ...
// followed by a newline. The result of performing serialize a CSS rule on each rule in the rule's cssRules list ... indenting each item with two spaces ... A newline, followed by
// the string '}'". An empty list is written as nothing between the two spaces (the pinned serialize-media-rule.html: '@media  {\n}').
TEST(CssomSerializeGroup, TheGroupingRules) {
  EXPECT_EQ(first("@media print {}"), "@media print {\n}");
  EXPECT_EQ(first("@media {}"), "@media  {\n}");
  EXPECT_EQ(first("@media all and (color) {}"), "@media (color) {\n}");
  EXPECT_EQ(first("@media print { a {} }"), "@media print {\n  a { }\n}");
  EXPECT_EQ(first("@supports (display:grid) { a{color:red} }"), "@supports (display:grid) {\n  a { color: red; }\n}") << "the condition as it was written, as both browsers write it";
  EXPECT_EQ(first("@container n (min-width: 1px) { a{color:red} }"), "@container n (min-width: 1px) {\n  a { color: red; }\n}");
  EXPECT_EQ(first("@layer foo.bar { a{color:red} }"), "@layer foo.bar {\n  a { color: red; }\n}");
  EXPECT_EQ(first("@layer { a{color:red} }"), "@layer {\n  a { color: red; }\n}");
  EXPECT_EQ(first("@layer {}"), "@layer {\n}");
  EXPECT_EQ(first("@layer a, b.c;"), "@layer a, b.c;");
  EXPECT_EQ(first("@scope (.a) to (.b) { c{color:red} }"), "@scope (.a) to (.b) {\n  c { color: red; }\n}");
  EXPECT_EQ(first("@scope { c{color:red} }"), "@scope {\n  c { color: red; }\n}");
  EXPECT_EQ(first("@starting-style { a{color:red} }"), "@starting-style {\n  a { color: red; }\n}");
  EXPECT_EQ(first("@media print { @layer x; @media screen {} }"), "@media print {\n  @layer x;\n  @media screen {\n}\n}");
}

// CSSOM, CSSImportRule and CSSNamespaceRule: "The result of performing serialize a URL" (always url("...")), the media list when it is not empty, the semicolon.
TEST(CssomSerializeAtRules, ImportAndNamespaceAreWrittenWithQuotedUrls) {
  EXPECT_EQ(first("@import url(a.css);"), "@import url(\"a.css\");");
  EXPECT_EQ(first("@import 'a.css';"), "@import url(\"a.css\");");
  EXPECT_EQ(first("@import url(\"quote\\\"quote\");"), "@import url(\"quote\\\"quote\");");
  EXPECT_EQ(first("@import url(a.css) print, screen;"), "@import url(\"a.css\") print, screen;");
  EXPECT_EQ(first("@import \"a.css\" layer(x) supports(display:grid) screen and (min-width:1px);"), "@import url(\"a.css\") layer(x) supports(display:grid) screen and (min-width: 1px);");
  EXPECT_EQ(first("@import url(a.css) layer;"), "@import url(\"a.css\") layer;");
  EXPECT_EQ(first("@namespace url(u);"), "@namespace url(\"u\");");
  EXPECT_EQ(first("@namespace x \"u\";"), "@namespace x url(\"u\");");
  EXPECT_EQ(first("@namespace svg url(http://www.w3.org/2000/svg);"), "@namespace svg url(\"http://www.w3.org/2000/svg\");");
}

TEST(CssomSerializeAtRules, TheTypedRulesAreTheirModelsSerialization) {
  EXPECT_EQ(first("@font-face { font-family: x; src: url(a) format(woff2); font-weight: 400 }"), "@font-face { font-family: x; src: url(\"a\") format(woff2); font-weight: 400; }");
  EXPECT_EQ(first("@counter-style c { system: cyclic; symbols: a b; suffix: '. ' }"), "@counter-style c { system: cyclic; symbols: a b; suffix: \". \"; }");
  EXPECT_EQ(first("@property --p { syntax: '<length>'; inherits: false; initial-value: 0px }"), "@property --p { syntax: \"<length>\"; inherits: false; initial-value: 0px; }");
  EXPECT_EQ(first("@position-try --x { top: 1px }"), "@position-try --x { top: 1px; }");
  EXPECT_EQ(first("@font-palette-values --p { font-family: x; base-palette: 1 }"), "@font-palette-values --p { font-family: x; base-palette: 1; }");
  EXPECT_EQ(first("@font-feature-values Foo { @styleset { a: 1 2 } }"), "@font-feature-values Foo {\n  @styleset { a: 1 2; }\n}");
  EXPECT_EQ(first("@font-face {}"), "@font-face { }");
  EXPECT_EQ(first("@page :first { margin: 1px }"), "@page :first { margin: 1px; }");
}

// The keyframes rule and a page rule hold their rules as objects: the text is the typed model's writing with the objects' texts, and it is the typed model's own for a rule nobody
// has changed.
TEST(CssomSerializeAtRules, KeyframesAndPageHoldTheirRules) {
  EXPECT_EQ(first("@keyframes k { from { opacity: 0 } 50%, 60% { opacity: .5; color: red } to { opacity: 1 } }"),
      "@keyframes k {\n  0% { opacity: 0; }\n  50%, 60% { opacity: 0.5; color: red; }\n  100% { opacity: 1; }\n}");
  EXPECT_EQ(first("@keyframes k { }"), "@keyframes k { }");
  EXPECT_EQ(first("@keyframes k { from {} }"), "@keyframes k {\n  0% { }\n}");
  EXPECT_EQ(first("@keyframes \"none\" { }"), "@keyframes \"none\" { }");
  EXPECT_EQ(first("@page :first { margin: 1px; @top-left { content: 'x'; color: red } }"), "@page :first {\n  margin: 1px;\n  @top-left { content: \"x\"; color: red; }\n}");
  EXPECT_EQ(first("@page { @top-left { content: 'x' } }"), "@page {\n  @top-left { content: \"x\"; }\n}");
  Doc doc;
  Sheet s(doc, "@keyframes k { from { opacity: 0 } 50%, 60% { opacity: .5 } }");
  Rule kf = path(s, {0, 1});
  EXPECT_EQ(css(kf.r), "50%, 60% { opacity: 0.5; }");
}

// A rule serialized by the rule layer and the typed model agree, byte for byte, for a rule nobody has changed (one writer of the formatting).
TEST(CssomSerializeAtRules, TheRuleAndTheTypedModelAgree) {
  const char * const texts[] = {"@keyframes k { from { opacity: 0 } 50%, 60% { opacity: .5; color: red } to { opacity: 1 } }", "@keyframes k { }", "@keyframes k { entry 10% { color: red } }",
      "@page :first { margin: 1px; @top-left { content: 'x' } }", "@page { margin: 1px }", "@font-face { font-family: x; src: url(a) }", "@import url(a.css) screen;", "@layer a, b;",
      "@namespace x url(u);", "@font-feature-values F { @swash { s: 1 } }"};
  Doc doc;
  for (const char * t : texts) {
    GTEXT_CSS_Tree * tree = nullptr;
    GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
    ASSERT_EQ(gtext_css_parse_stylesheet(t, strlen(t), &po, &tree, nullptr), GTEXT_CSS_OK);
    GTEXT_CSS_Atrule * typed = nullptr;
    GTEXT_CSS_Atrule_Result res;
    ASSERT_EQ(gtext_css_atrule_parse(R(), &tree->rules[0], nullptr, &typed, &res), GTEXT_CSS_OK) << t;
    ASSERT_NE(typed, nullptr) << t;
    char * buf = nullptr;
    size_t n = 0;
    ASSERT_EQ(gtext_css_atrule_serialize_buffer(typed, nullptr, &buf, &n), GTEXT_CSS_OK);
    EXPECT_EQ(first(t), std::string(buf, n)) << t;
    gtext_css_atrule_buffer_free(nullptr, buf);
    gtext_css_atrule_free(typed);
    gtext_css_tree_free(tree);
  }
}

/* ---- the sheet ---------------------------------------------------------------------------------------------------------------------------------- */

TEST(CssomSerializeSheet, EachRuleFollowedByALineFeed) {
  Doc doc;
  Sheet s(doc, "@import url(a.css); a{color:red} @media print{ b{} } @font-face{font-family:x;src:url(a)}");
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  EXPECT_EQ(sheet_text(s.s), "@import url(\"a.css\");\na { color: red; }\n@media print {\n  b { }\n}\n@font-face { font-family: x; src: url(\"a\"); }\n");
  Sheet empty(doc, "");
  EXPECT_EQ(sheet_text(empty.s), "");
}

// Parse, serialize, parse, serialize is a fixed point for the sheets the parse can make.
TEST(CssomSerializeSheet, ParsingTheTextGivesTheSameText) {
  const char * const sheets[] = {
      "a{color:red} @media print{ b > c{margin:1px 2px} @supports (a:b){ e{} } }",
      "@import url(a.css) screen; @namespace s url(urn:s); s|a{ b{} color:red; c{} width:1px } @keyframes k{ from{a:b} 50%{} } @page :first{ margin:1px; @top-left{ content:\"x\" } }",
      "@layer a, b; @layer c{ z{} } @font-face{font-family:q;src:url(a)} @scope (a) to (b){ b{ & > c{} } } @starting-style{ a{color:red} } @container n (min-width:1px){ a{} }",
      "x{ color:red; & y{} width:1px; @media print{ color:blue; & z{} } @scope (.q){ r{} } }",
      "@counter-style c{system:cyclic;symbols:a b} @property --p{syntax:'*';inherits:false} @font-palette-values --p{font-family:x} @position-try --x{top:1px} @view-transition{navigation:auto}",
      "@custom-media --m (min-width:1px); @when media(width>1px){ a{} } @else { b{} } @function --f(){ result: 1; }",
  };
  for (const char * t : sheets) {
    Doc doc;
    Sheet a(doc, t);
    ASSERT_EQ(a.status, GTEXT_CSS_OK) << t;
    GTEXT_CSS_Status st = GTEXT_CSS_OK;
    const std::string once = sheet_text(a.s, &st);
    ASSERT_EQ(st, GTEXT_CSS_OK);
    Sheet b(doc, once);
    ASSERT_EQ(b.status, GTEXT_CSS_OK);
    EXPECT_EQ(gtext_cssom_sheet_diagnostic_total(b.s), 0u) << t << "\n" << once;
    EXPECT_EQ(sheet_text(b.s), once) << t;
    EXPECT_EQ(gtext_cssom_sheet_rule_total(a.s), gtext_cssom_sheet_rule_total(b.s)) << t;
  }
}

// The text of a sheet that was edited is the text of the rules it holds.
TEST(CssomSerializeSheet, AnEditedSheetWritesWhatItHolds) {
  Doc doc;
  Sheet s(doc, "a{} b{}");
  ASSERT_EQ(insert_rule(s.s, "@media print{ c{ color:red } }", 1).st, GTEXT_CSS_OK);
  ASSERT_EQ(delete_rule(s.s, 0), GTEXT_CSS_OK);
  Rule b = at(s.rules(), 1);
  Decl bs = style_of(b.r);
  EXPECT_EQ(set(bs.d, "color", "blue").st, GTEXT_CSS_OK);
  EXPECT_EQ(sheet_text(s.s), "@media print {\n  c { color: red; }\n}\nb { color: blue; }\n");
}

/* ---- the cache ---------------------------------------------------------------------------------------------------------------------------------- */

// A rule with no rules inside it keeps its text for as long as its version stays.
TEST(CssomSerializeCache, ALeafKeepsItsTextUntilItsVersionMoves) {
  Doc doc;
  Sheet s(doc, "a{color:red} @media print{ b{} }");
  Rule a = at(s.rules(), 0);
  GTEXT_CSSOM_Counters c;
  EXPECT_EQ(css(a.r), "a { color: red; }");
  gtext_cssom_document_counters(doc.d, &c);
  EXPECT_EQ(c.cache_fills, 1u);
  EXPECT_GT(c.cached_text_bytes, 0u);
  EXPECT_EQ(css(a.r), "a { color: red; }");
  gtext_cssom_document_counters(doc.d, &c);
  EXPECT_EQ(c.cache_fills, 1u) << "no second fill";
  EXPECT_EQ(c.cache_hits, 1u);
  Decl style = style_of(a.r);
  ASSERT_EQ(set(style.d, "color", "blue").st, GTEXT_CSS_OK);
  gtext_cssom_document_counters(doc.d, &c);
  EXPECT_EQ(c.cached_text_bytes, 0u) << "the entry is dropped when the version moves";
  EXPECT_EQ(css(a.r), "a { color: blue; }");
  gtext_cssom_document_counters(doc.d, &c);
  EXPECT_EQ(c.cache_fills, 2u);
}

// A rule with rules inside it does not keep its text (it holds every descendant's), and the serialization of a sheet or through a sink fills nothing.
TEST(CssomSerializeCache, ARuleWithRulesInsideAndASheetFillNothing) {
  Doc doc;
  Sheet s(doc, "@media print{ b{} } a{ & c{} }");
  GTEXT_CSSOM_Counters c;
  Rule media = at(s.rules(), 0);
  Rule a = at(s.rules(), 1);
  EXPECT_NE(css(media.r), "");
  EXPECT_NE(css(a.r), "");
  gtext_cssom_document_counters(doc.d, &c);
  EXPECT_EQ(c.cache_fills, 0u);
  EXPECT_EQ(c.cached_text_bytes, 0u);
  (void)sheet_text(s.s);
  std::string out;
  GTEXT_Markup_Sink sink;
  std::memset(&sink, 0, sizeof sink);
  sink.user = &out;
  sink.write = [](void * u, const char * p, size_t n) {
    static_cast<std::string *>(u)->append(p, n);
    return GTEXT_MARKUP_OK;
  };
  Rule leaf = path(s, {0, 0});
  ASSERT_EQ(gtext_cssom_rule_serialize(leaf.r, &sink), GTEXT_CSS_OK);
  EXPECT_EQ(out, "b { }");
  gtext_cssom_document_counters(doc.d, &c);
  EXPECT_EQ(c.cache_fills, 0u) << "through a sink";
  EXPECT_EQ(c.cached_text_bytes, 0u);
  // a rule that gains a rule stops being a leaf; its old text is dropped
  Rule b = path(s, {0, 0});
  EXPECT_EQ(css(b.r), "b { }");
  gtext_cssom_document_counters(doc.d, &c);
  EXPECT_EQ(c.cache_fills, 1u);
  ASSERT_EQ(insert_rule(b.r, "x{}", 0).st, GTEXT_CSS_OK);
  gtext_cssom_document_counters(doc.d, &c);
  EXPECT_EQ(c.cached_text_bytes, 0u);
  EXPECT_EQ(css(b.r), "b {\n  & x { }\n}");
}

// "Cached bytes count in the live-object limit, an entry is dropped when its rule's version moves, and past max_cached_text_bytes the document drops the earliest-added entries".
TEST(CssomSerializeCache, TheBoundDropsTheEarliestAddedEntries) {
  Doc doc([](GTEXT_CSSOM_Config & c) { c.max_cached_text_bytes = 400; });
  std::string css_text;
  for (int i = 0; i < 100; i++) {
    css_text += "a" + std::to_string(i) + "{color:red}";
  }
  Sheet s(doc, css_text);
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  GTEXT_CSSOM_Counters c;
  gtext_cssom_document_counters(doc.d, &c);
  const size_t live0 = c.live_bytes;
  for (size_t i = 0; i < 100; i++) {
    Rule r = at(s.rules(), i);
    (void)css(r.r);
    gtext_cssom_document_counters(doc.d, &c);
    EXPECT_LE(c.cached_text_bytes, 400u) << i;
  }
  gtext_cssom_document_counters(doc.d, &c);
  EXPECT_GT(c.cache_fills, 5u);
  EXPECT_GT(c.cached_text_bytes, 0u);
  EXPECT_EQ(c.live_bytes, live0 + c.cached_text_bytes) << "the cache is counted in the live bytes";
  // the first entries are gone: reading rule 0 fills it again
  const uint64_t fills = c.cache_fills;
  Rule first_rule = at(s.rules(), 0);
  (void)css(first_rule.r);
  gtext_cssom_document_counters(doc.d, &c);
  EXPECT_EQ(c.cache_fills, fills + 1);
  // a text longer than the bound is not kept, and is not an error
  Doc small([](GTEXT_CSSOM_Config & cfg) { cfg.max_cached_text_bytes = 16; });
  Sheet t(small, "a{color:red; width:1px; opacity:1}");
  Rule r = at(t.rules(), 0);
  EXPECT_EQ(css(r.r), "a { color: red; width: 1px; opacity: 1; }");
  gtext_cssom_document_counters(small.d, &c);
  EXPECT_EQ(c.cached_text_bytes, 0u);
}

// A million rules: the sheet's serialization fills nothing, and the cache holds only what was asked for, bounded.
TEST(CssomSerializeCache, AMillionRuleSheetKeepsTheCacheSmall) {
  Doc doc([](GTEXT_CSSOM_Config & c) { c.max_cached_text_bytes = 4096; });
  std::string text;
  text.reserve(5 * 1000000);
  for (int i = 0; i < 1000000; i++) {
    text += "a{}\n";
  }
  Sheet s(doc, text);
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  const std::string out = sheet_text(s.s, &st);
  ASSERT_EQ(st, GTEXT_CSS_OK);
  EXPECT_EQ(out.size(), 6u * 1000000);
  GTEXT_CSSOM_Counters c;
  gtext_cssom_document_counters(doc.d, &c);
  EXPECT_EQ(c.cached_text_bytes, 0u);
  size_t peak = 0;
  for (GTEXT_CSSOM_Rule * r = gtext_cssom_rule_list_first(s.rules()); r;) {
    (void)css(r);
    gtext_cssom_document_counters(doc.d, &c);
    peak = std::max(peak, c.cached_text_bytes);
    GTEXT_CSSOM_Rule * next = gtext_cssom_rule_list_next(s.rules(), r);
    gtext_cssom_rule_release(r);
    r = next;
    if (c.cache_fills > 20000) {
      break;
    }
  }
  EXPECT_LE(peak, 4096u);
}

/* ---- depth and width: no recursion ------------------------------------------------------------------------------------------------------------- */

// A nest 4,096 deep (the limit) is written, with no recursion in the C stack, and its text has the structure the rule shape gives.
TEST(CssomSerializeDeep, ANestAtTheLimitIsWritten) {
  Doc doc([](GTEXT_CSSOM_Config & c) { c.max_rule_nesting = 4096; });
  std::string text;
  const int depth = 4096;
  for (int i = 0; i < depth; i++) {
    text += "a{";
  }
  for (int i = 0; i < depth; i++) {
    text += "}";
  }
  Sheet s(doc, text);
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  Rule top = at(s.rules(), 0);
  const std::string out = css(top.r);
  EXPECT_EQ(std::count(out.begin(), out.end(), '{'), depth);
  EXPECT_EQ(std::count(out.begin(), out.end(), '}'), depth);
  GTEXT_CSSOM_Counters c;
  gtext_cssom_document_counters(doc.d, &c);
  EXPECT_EQ(c.cached_text_bytes, 0u) << "a rule with rules inside it keeps nothing";
  // 100,000 deep is refused with the limit's status, not a crash
  std::string deeper;
  for (int i = 0; i < 100000; i++) {
    deeper += "a{";
  }
  Sheet t(doc, deeper);
  EXPECT_NE(t.status, GTEXT_CSS_OK);
}

TEST(CssomSerializeDeep, AGroupingRuleWithAHundredThousandRulesIsWritten) {
  Doc doc;
  std::string text = "@media print{";
  for (int i = 0; i < 100000; i++) {
    text += "a{}";
  }
  text += "}";
  Sheet s(doc, text);
  ASSERT_EQ(s.status, GTEXT_CSS_OK);
  Rule media = at(s.rules(), 0);
  const std::string out = css(media.r);
  EXPECT_EQ(out.size(), std::string("@media print {").size() + 100000 * std::string("\n  a { }").size() + 2);
}

// The output limit is a status, and the sink's refusal is one.
TEST(CssomSerializeLimits, TheOutputLimitAndTheSinksRefusalAreStatuses) {
  Doc doc([](GTEXT_CSSOM_Config & c) { c.max_output_bytes = 30; });
  Sheet s(doc, "a{color:red} b{color:red} c{color:red}");
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  (void)sheet_text(s.s, &st);
  EXPECT_EQ(st, GTEXT_CSS_E_LIMIT_OUTPUT);
  Rule a = at(s.rules(), 0);
  EXPECT_EQ(css(a.r), "a { color: red; }") << "a rule within the limit";
  Doc doc2;
  Sheet t(doc2, "a{color:red}");
  GTEXT_Markup_Sink refuse;
  std::memset(&refuse, 0, sizeof refuse);
  refuse.write = [](void *, const char *, size_t) { return GTEXT_MARKUP_E_WRITE; };
  EXPECT_EQ(gtext_cssom_sheet_serialize(t.s, &refuse), GTEXT_CSS_E_WRITE);
  EXPECT_EQ(gtext_cssom_sheet_serialize(t.s, nullptr), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_cssom_sheet_serialize(nullptr, &refuse), GTEXT_CSS_E_INVALID);
}

} // namespace
