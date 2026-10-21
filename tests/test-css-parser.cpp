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
 * The CSS parser against CSS Syntax section 5 (Editor's Draft of 20 October
 * 2026): every entry point, the nesting of declarations and rules in a
 * block, `!important`, a custom property's original text, the unicode-range
 * reading, the "valid in the current context" seam, identities, spans, the
 * parse errors and their list, the arguments, the allocator, and threads.
 */

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_test_support.h"
#include "markup_test_support.h"

using namespace css_test;

namespace {

std::string decls_text(const GTEXT_CSS_Declaration * d, size_t n) {
  std::string out;
  for (size_t i = 0; i < n; i++) {
    if (i) {
      out += "; ";
    }
    out += str(d[i].name) + ":" + text_of(d[i].value, d[i].value_count) + (d[i].important ? " !important" : "");
  }
  return out;
}

// A rule's structure, as short text.
std::string rule_text(const GTEXT_CSS_Rule & r) {
  if (r.kind == GTEXT_CSS_RULE_NESTED_DECLARATIONS) {
    return "nested{" + decls_text(r.declarations, r.declaration_count) + "}";
  }
  std::string out = r.kind == GTEXT_CSS_RULE_AT ? "@" + str(r.name) + "[" : "q[";
  out += text_of(r.prelude, r.prelude_count) + "]";
  if (!r.has_block) {
    return out + ";";
  }
  out += "{";
  if (r.kind == GTEXT_CSS_RULE_QUALIFIED) {
    out += decls_text(r.declarations, r.declaration_count);
    for (size_t i = 0; i < r.rule_count; i++) {
      out += " | " + rule_text(r.rules[i]);
    }
  }
  else {
    for (size_t i = 0; i < r.content_count; i++) {
      if (i) {
        out += " | ";
      }
      out += r.contents[i].kind == GTEXT_CSS_ITEM_RULE
          ? rule_text(*r.contents[i].rule)
          : "decls{" + decls_text(r.contents[i].declarations, r.contents[i].declaration_count) + "}";
    }
  }
  return out + "}";
}

std::string rules_text(const GTEXT_CSS_Tree * t) {
  std::string out;
  for (size_t i = 0; i < t->rule_count; i++) {
    if (i) {
      out += " || ";
    }
    out += rule_text(t->rules[i]);
  }
  return out;
}

std::string sheet(const std::string & css, const GTEXT_CSS_Parse_Options * o = nullptr) {
  Parsed p;
  parse_into(p, gtext_css_parse_stylesheet, css, o);
  return p.status == GTEXT_CSS_OK ? rules_text(p.tree) : std::string("status:") + gtext_css_status_string(p.status);
}

std::string items_text(const GTEXT_CSS_Tree * t) {
  std::string out;
  for (size_t i = 0; i < t->item_count; i++) {
    if (i) {
      out += " || ";
    }
    out += t->items[i].kind == GTEXT_CSS_ITEM_RULE ? rule_text(*t->items[i].rule)
                                                   : "decls{" + decls_text(t->items[i].declarations, t->items[i].declaration_count) + "}";
  }
  return out;
}

// ---- the entry points (5.4) --------------------------------------------------------------------

TEST(CssParseStylesheet, ReturnsItsRulesAndItsLocation) {
  GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
  o.location = "https://example.com/a.css";
  Parsed p;
  parse_into(p, gtext_css_parse_stylesheet, "a{b:c} @import 'x';", &o);
  ASSERT_EQ(p.status, GTEXT_CSS_OK);
  EXPECT_EQ(p.tree->kind, GTEXT_CSS_TREE_STYLESHEET);
  EXPECT_STREQ(p.tree->location, "https://example.com/a.css");
  EXPECT_NE(p.tree->location, o.location) << "the location is copied";
  EXPECT_EQ(rules_text(p.tree), "q[ident:a]{b:ident:c} || @import[ws string:x];");
}

TEST(CssParseStylesheet, TopLevelWhitespaceCdoAndCdcAreDiscarded) {
  EXPECT_EQ(sheet("<!-- a{} --> <!-- b{} -->"), "q[ident:a]{} || q[ident:b]{}");
  EXPECT_EQ(sheet("a<!---->{}"), "q[ident:a CDO CDC]{}") << "inside a prelude they are tokens";
}

TEST(CssParseStylesheet, AQualifiedRuleTheInputEndsBeforeItsBlockIsDroppedWithAParseError) {
  Parsed p;
  parse_into(p, gtext_css_parse_stylesheet, "a{} b c");
  EXPECT_EQ(rules_text(p.tree), "q[ident:a]{}");
  ASSERT_EQ(p.error_names(), (std::vector<std::string>{"eof-in-qualified-rule"}));
  EXPECT_EQ(p.errors()[0].byte_offset, 7u);
}

TEST(CssParseStylesheet, ACloseBraceInATopLevelPreludeIsATokenAndAParseError) {
  Parsed p;
  parse_into(p, gtext_css_parse_stylesheet, "a } b {}");
  EXPECT_EQ(rules_text(p.tree), "q[ident:a ws } ws ident:b ws]{}");
  EXPECT_EQ(p.error_names(), (std::vector<std::string>{"close-brace-in-qualified-rule"}));
}

TEST(CssParseStylesheet, CharsetIsAnAtRuleLikeAnyOther) {
  // 8.3: "there is no actual at-rule named @charset"; to syntax it is an at-rule, which a
  // grammar then drops as unrecognized.
  EXPECT_EQ(sheet("@charset \"utf-8\"; a{}"), "@charset[ws string:utf-8]; || q[ident:a]{}");
}

TEST(CssParseStylesheetContents, IsAStylesheetWithoutALocation) {
  GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
  o.location = "x";
  Parsed p;
  parse_into(p, gtext_css_parse_stylesheet_contents, "<!-- a{} -->", &o);
  EXPECT_EQ(p.tree->kind, GTEXT_CSS_TREE_STYLESHEET_CONTENTS);
  EXPECT_EQ(p.tree->location, nullptr);
  EXPECT_EQ(rules_text(p.tree), "q[ident:a]{}");
}

TEST(CssParseDeclarationList, ReadsAndDiscardsAtRules) {
  Parsed p;
  parse_into(p, gtext_css_parse_declaration_list, "a:b; @media x { c:d } e:f");
  EXPECT_EQ(decls_text(p.tree->declarations, p.tree->declaration_count), "a:ident:b; e:ident:f");
  EXPECT_EQ(p.error_names(), (std::vector<std::string>{"at-rule-in-declaration-list"}));
}

TEST(CssParseDeclarationList, AnInvalidDeclarationIsDroppedToItsSemicolon) {
  Parsed p;
  parse_into(p, gtext_css_parse_declaration_list, "a:b; c d e; f+:g; h:i");
  EXPECT_EQ(decls_text(p.tree->declarations, p.tree->declaration_count), "a:ident:b; h:ident:i");
  EXPECT_EQ(p.error_names(), (std::vector<std::string>{"invalid-declaration", "invalid-declaration"}));
}

TEST(CssParseDeclarationList, ACloseBraceIsNotTheEndOfAList) {
  // Not nested: a } in a value is a token, and in the remnants of a bad declaration it is discarded and
  // the remnants go on to the semicolon, so `d: e` is part of them.
  Parsed p;
  parse_into(p, gtext_css_parse_declaration_list, "a: b } c; } d: e; f: g");
  EXPECT_EQ(decls_text(p.tree->declarations, p.tree->declaration_count), "a:ident:b ws } ws ident:c; f:ident:g");
  EXPECT_EQ(p.error_names(), (std::vector<std::string>{"unmatched-close-brace", "invalid-declaration"}));
}

TEST(CssParseBlockContents, InterleavesDeclarationsAndRulesInOrder) {
  Parsed p;
  parse_into(p, gtext_css_parse_block_contents, "a:b; c{d:e} f:g; @x; h:i; j{}");
  ASSERT_EQ(p.status, GTEXT_CSS_OK);
  EXPECT_EQ(items_text(p.tree), "decls{a:ident:b} || q[ident:c]{d:ident:e} || decls{f:ident:g} || @x[]; || decls{h:ident:i} || q[ident:j]{}");
}

TEST(CssParseBlockContents, StopsAtATopLevelCloseBrace) {
  // "consume a block's contents" returns at a }: the rest of the input is not its.
  Parsed p;
  parse_into(p, gtext_css_parse_block_contents, "a:b } c:d");
  EXPECT_EQ(items_text(p.tree), "decls{a:ident:b}");
}

TEST(CssParseRule, ReturnsOneRuleOrASyntaxError) {
  struct Case {
    const char * in;
    GTEXT_CSS_Syntax_Error err;
  };
  const Case cases[] = {
      {"", GTEXT_CSS_SYNTAX_EMPTY}, {"  /**/ ", GTEXT_CSS_SYNTAX_EMPTY}, {"a", GTEXT_CSS_SYNTAX_INVALID},
      {"a{} b{}", GTEXT_CSS_SYNTAX_EXTRA_INPUT}, {"@x; @y;", GTEXT_CSS_SYNTAX_EXTRA_INPUT},
      {" a{} ", GTEXT_CSS_SYNTAX_NONE}, {"@x", GTEXT_CSS_SYNTAX_NONE},
  };
  for (const Case & c : cases) {
    Parsed p;
    parse_into(p, gtext_css_parse_rule, c.in);
    ASSERT_EQ(p.status, GTEXT_CSS_OK) << c.in;
    EXPECT_EQ(p.tree->syntax_error, c.err) << c.in;
    EXPECT_EQ(p.tree->rule_count, c.err == GTEXT_CSS_SYNTAX_NONE ? 1u : 0u) << c.in;
  }
}

TEST(CssParseDeclaration, ReturnsOneDeclarationOrASyntaxError) {
  struct Case {
    const char * in;
    GTEXT_CSS_Syntax_Error err;
  };
  const Case cases[] = {
      {"", GTEXT_CSS_SYNTAX_EMPTY}, {"a", GTEXT_CSS_SYNTAX_INVALID}, {"a:b;", GTEXT_CSS_SYNTAX_EXTRA_INPUT},
      {"a:{} b", GTEXT_CSS_SYNTAX_INVALID}, {" a : b ", GTEXT_CSS_SYNTAX_NONE},
  };
  for (const Case & c : cases) {
    Parsed p;
    parse_into(p, gtext_css_parse_declaration, c.in);
    ASSERT_EQ(p.status, GTEXT_CSS_OK) << c.in;
    EXPECT_EQ(p.tree->syntax_error, c.err) << c.in;
  }
}

TEST(CssParseComponentValue, ReturnsOneValueOrASyntaxError) {
  Parsed a;
  parse_into(a, gtext_css_parse_component_value, "  f( x )  ");
  EXPECT_EQ(a.tree->syntax_error, GTEXT_CSS_SYNTAX_NONE);
  EXPECT_EQ(text_of(a.tree->values, a.tree->value_count), "f:f(ws ident:x ws)");
  Parsed b;
  parse_into(b, gtext_css_parse_component_value, "a b");
  EXPECT_EQ(b.tree->syntax_error, GTEXT_CSS_SYNTAX_EXTRA_INPUT);
  EXPECT_EQ(b.tree->value_count, 0u);
  Parsed c;
  parse_into(c, gtext_css_parse_component_value, "}");
  EXPECT_EQ(text_of(c.tree->values, c.tree->value_count), "}");
  EXPECT_EQ(c.error_names(), (std::vector<std::string>{"unmatched-close-brace"}));
}

TEST(CssParseComponentValues, BlocksAndFunctionsNestAndTheEndOfInputClosesThem) {
  Parsed p;
  parse_into(p, gtext_css_parse_component_values, "a(b [c {d ) e");
  ASSERT_EQ(p.tree->value_count, 1u);
  EXPECT_EQ(text_of(p.tree->values, p.tree->value_count), "f:a(ident:b ws [ident:c ws {ident:d ws ) ws ident:e}])");
  EXPECT_TRUE(p.tree->values[0].flags & GTEXT_CSS_FLAG_UNCLOSED);
  EXPECT_EQ(p.error_names(), (std::vector<std::string>{"unmatched-close-paren"}))
      << "the ) does not close the function: only a block's mirror closes it";
}

TEST(CssParseCommaList, SplitsAtTopLevelCommas) {
  Parsed p;
  parse_into(p, gtext_css_parse_comma_list, "a, f(b, c), ,d,");
  ASSERT_EQ(p.tree->list_count, 4u) << "a trailing comma ends the list";
  EXPECT_EQ(text_of(p.tree->lists[0].values, p.tree->lists[0].count), "ident:a");
  EXPECT_EQ(text_of(p.tree->lists[1].values, p.tree->lists[1].count), "ws f:f(ident:b , ws ident:c)");
  EXPECT_EQ(text_of(p.tree->lists[2].values, p.tree->lists[2].count), "ws");
  EXPECT_EQ(text_of(p.tree->lists[3].values, p.tree->lists[3].count), "ident:d");
}

struct GrammarLog {
  std::vector<std::string> seen;
  std::string accept;
};

bool log_grammar(void * user, size_t index, const GTEXT_CSS_Component_Value * v, size_t n) {
  auto * g = static_cast<GrammarLog *>(user);
  const std::string t = text_of(v, n);
  g->seen.push_back(std::to_string(index) + "=" + t);
  return t.find(g->accept) != std::string::npos;
}

TEST(CssParseGrammar, TheGrammarIsAskedAboutTheListOfComponentValues) {
  GrammarLog log;
  log.accept = "ident:red";
  GTEXT_CSS_Grammar g = {log_grammar, &log};
  bool matched = false;
  Parsed p;
  p.status = gtext_css_parse_grammar(" red ", 5, &g, nullptr, &matched, &p.tree, &p.result);
  ASSERT_EQ(p.status, GTEXT_CSS_OK);
  EXPECT_TRUE(matched);
  EXPECT_EQ(log.seen, (std::vector<std::string>{"0=ws ident:red ws"}));
  EXPECT_EQ(gtext_css_parse_grammar("x", 1, nullptr, nullptr, &matched, &p.tree, nullptr), GTEXT_CSS_E_INVALID);
}

TEST(CssParseGrammarList, OnlyWhitespaceIsAnEmptyListAndEachGroupIsMatched) {
  GrammarLog log;
  log.accept = "ident:a";
  GTEXT_CSS_Grammar g = {log_grammar, &log};
  size_t matched = 99;
  {
    Parsed p;
    p.status = gtext_css_parse_grammar_list("  ", 2, &g, nullptr, &matched, &p.tree, &p.result);
    ASSERT_EQ(p.status, GTEXT_CSS_OK);
    EXPECT_EQ(matched, 0u);
    EXPECT_EQ(p.tree->list_count, 0u);
    EXPECT_TRUE(log.seen.empty());
  }
  {
    Parsed p;
    p.status = gtext_css_parse_grammar_list("a, b,a", 6, &g, nullptr, &matched, &p.tree, &p.result);
    ASSERT_EQ(p.status, GTEXT_CSS_OK);
    EXPECT_EQ(matched, 2u);
    EXPECT_EQ(log.seen, (std::vector<std::string>{"0=ident:a", "1=ws ident:b", "2=ident:a"}));
  }
}

TEST(CssMatchGrammar, ParsedValuesAreMatchedAsTheyAreAndSplitAtTopLevelCommasOnly) {
  Parsed p;
  parse_into(p, gtext_css_parse_component_values, "a, f(a, b), , a,");
  GrammarLog log;
  log.accept = "a";
  GTEXT_CSS_Grammar g = {log_grammar, &log};
  EXPECT_TRUE(gtext_css_match_grammar(p.tree->values, p.tree->value_count, &g));
  log.seen.clear();
  size_t groups = 0;
  EXPECT_EQ(gtext_css_match_grammar_list(p.tree->values, p.tree->value_count, &g, &groups), 3u);
  EXPECT_EQ(groups, 4u);
  EXPECT_EQ(log.seen, (std::vector<std::string>{"0=ident:a", "1=ws f:f(ident:a , ws ident:b)", "2=ws", "3=ws ident:a"}));
  EXPECT_FALSE(gtext_css_match_grammar(nullptr, 1, &g));
}

// ---- nesting (5.5.3, 5.5.5) --------------------------------------------------------------------

TEST(CssNesting, ADeclarationANestedRuleAndANestedDeclarationsRuleInOrder) {
  // The matrix's first row.
  EXPECT_EQ(sheet("a{color:red; &:hover{color:blue} b:c}"),
      "q[ident:a]{color:ident:red | q[delim:& : ident:hover]{color:ident:blue} | nested{b:ident:c}}");
}

TEST(CssNesting, AnAtRuleBlockKeepsItsItemsAsTheyCame) {
  EXPECT_EQ(sheet("@media x { a:b; c{} d:e }"), "@media[ws ident:x ws]{decls{a:ident:b} | q[ident:c]{} | decls{d:ident:e}}");
  EXPECT_EQ(sheet("a { b:c; @media x { d:e } f:g }"),
      "q[ident:a ws]{b:ident:c | @media[ws ident:x ws]{decls{d:ident:e}} | nested{f:ident:g}}");
}

TEST(CssNesting, ANestedStatementAtRuleEndsAtTheCloseBrace) {
  EXPECT_EQ(sheet("a{@x}b{}"), "q[ident:a]{ | @x[];} || q[ident:b]{}");
  EXPECT_EQ(sheet("a{@x y; z:w}"), "q[ident:a]{ | @x[ws ident:y]; | nested{z:ident:w}}");
}

TEST(CssNesting, ADeclarationWhoseValueHasABlockAndMoreIsReadAgainAsARule) {
  EXPECT_EQ(sheet("a{b:c{d:e}}"), "q[ident:a]{ | q[ident:b : ident:c]{d:ident:e}}");
  // A leading block followed by more: the rule is `b:` with the block, and ` d` is then read on its own
  // (a qualified rule the close brace ends, dropped).
  EXPECT_EQ(sheet("a{b:{c} d}"), "q[ident:a]{ | q[ident:b :]{}}");
}

TEST(CssNesting, ABlockThatIsTheWholeValueIsADeclaration) {
  EXPECT_EQ(sheet("a{b:{c}; d: { e } !important}"), "q[ident:a]{b:{ident:c}; d:{ws ident:e ws} !important}");
}

TEST(CssNesting, ANestedQualifiedRuleStopsAtASemicolon) {
  Parsed p;
  parse_into(p, gtext_css_parse_stylesheet, "a{b c; d:e}");
  EXPECT_EQ(rules_text(p.tree), "q[ident:a]{d:ident:e}");
  EXPECT_EQ(p.error_names(), (std::vector<std::string>{"semicolon-in-qualified-rule"}));
}

TEST(CssNesting, ANestedQualifiedRuleThatMeetsTheCloseBraceIsDroppedAndTheBlockEnds) {
  Parsed p;
  parse_into(p, gtext_css_parse_stylesheet, "a{b c} d{}");
  EXPECT_EQ(rules_text(p.tree), "q[ident:a]{} || q[ident:d]{}");
  EXPECT_EQ(p.error_names(), (std::vector<std::string>{"close-brace-in-qualified-rule"}));
}

TEST(CssNesting, ATopLevelRuleThatLooksLikeACustomPropertyIsDroppedWithItsBlock) {
  Parsed p;
  parse_into(p, gtext_css_parse_stylesheet, ".a { } --x:hover { b { } } .b { }");
  EXPECT_EQ(rules_text(p.tree), "q[delim:. ident:a ws]{} || q[delim:. ident:b ws]{}");
  EXPECT_EQ(p.error_names(), (std::vector<std::string>{"rule-looks-like-custom-property"}));
}

TEST(CssNesting, NestedARuleThatLooksLikeACustomPropertyIsTheCustomProperty) {
  Parsed p;
  parse_into(p, gtext_css_parse_stylesheet, "div { .a { } --x:hover { }\n .b { } }");
  ASSERT_EQ(p.tree->rule_count, 1u);
  const GTEXT_CSS_Rule & div = p.tree->rules[0];
  ASSERT_EQ(div.rule_count, 2u);
  ASSERT_EQ(div.rules[1].kind, GTEXT_CSS_RULE_NESTED_DECLARATIONS);
  ASSERT_EQ(div.rules[1].declaration_count, 1u);
  EXPECT_EQ(str(div.rules[1].declarations[0].original_text), "hover { }\n .b { }");
}

TEST(CssNesting, TheMaterializedRulesAreTheBlocksItemsWithNoneLost) {
  // A rule that begins with a rule has no declarations of its own; every run after a rule is a
  // nested declarations rule in its place.
  EXPECT_EQ(sheet("a{b{} c:d; e{} f:g}"), "q[ident:a]{ | q[ident:b]{} | nested{c:ident:d} | q[ident:e]{} | nested{f:ident:g}}");
}

// ---- consume a declaration (5.5.6) --------------------------------------------------------------

TEST(CssDeclaration, ImportantIsTheLastTwoTokensCaseInsensitively) {
  Parsed p;
  parse_into(p, gtext_css_parse_declaration_list,
      "a: b ! IMPORTANT ; c: d !/**/important; e: f !important x; g: h !importan; i: !important; j: k ! \\69mportant");
  EXPECT_EQ(decls_text(p.tree->declarations, p.tree->declaration_count),
      "a:ident:b !important; c:ident:d !important; e:ident:f ws delim:! ident:important ws ident:x; "
      "g:ident:h ws delim:! ident:importan; i: !important; j:ident:k !important");
}

TEST(CssDeclaration, WhitespaceBeforeAndAfterTheValueIsNotKept) {
  Parsed p;
  parse_into(p, gtext_css_parse_declaration_list, "a :  b  c  ; d:e /**/ ;f: ");
  EXPECT_EQ(decls_text(p.tree->declarations, p.tree->declaration_count), "a:ident:b ws ident:c; d:ident:e; f:");
}

TEST(CssDeclaration, ACustomPropertyKeepsItsOriginalText) {
  Parsed p;
  static const char css[] = "--x: {a} ; --y:  a /* c */\r\n b\0 !important; --z:; w: v";
  parse_into(p, gtext_css_parse_declaration_list, std::string(css, sizeof css - 1));
  ASSERT_EQ(p.tree->declaration_count, 4u);
  EXPECT_EQ(str(p.tree->declarations[0].original_text), "{a}") << "the matrix's custom property row";
  EXPECT_EQ(text_of(p.tree->declarations[0].value, p.tree->declarations[0].value_count), "{ident:a}");
  EXPECT_EQ(str(p.tree->declarations[1].original_text), "a /* c */\n b\xEF\xBF\xBD") << "preprocessed: CR LF is LF, U+0000 is U+FFFD";
  EXPECT_TRUE(p.tree->declarations[1].important);
  ASSERT_NE(p.tree->declarations[2].original_text.data, nullptr);
  EXPECT_EQ(p.tree->declarations[2].original_text.length, 0u);
  EXPECT_EQ(p.tree->declarations[3].original_text.data, nullptr) << "only a custom property has one";
}

TEST(CssDeclaration, ACustomPropertyMayHoldBlocksAnywhere) {
  Parsed p;
  parse_into(p, gtext_css_parse_declaration_list, "--x: a {b} c; --y: {} {}");
  EXPECT_EQ(decls_text(p.tree->declarations, p.tree->declaration_count), "--x:ident:a ws {ident:b} ws ident:c; --y:{} ws {}");
}

TEST(CssDeclaration, AUnicodeRangeValueIsItsSourceReadAgainAsTheDraftWritesIt) {
  // "consume the value of a unicode-range descriptor from the segment of the original source text
  // string corresponding to the tokens returned by the consume a list of component values call":
  // that segment is the list before !important is removed and white space is trimmed, so they are
  // in the value (the flag is set too).
  Parsed p;
  parse_into(p, gtext_css_parse_declaration_list, "unicode-range: U+0-7F !important ;");
  ASSERT_EQ(p.tree->declaration_count, 1u);
  EXPECT_TRUE(p.tree->declarations[0].important);
  EXPECT_EQ(text_of(p.tree->declarations[0].value, p.tree->declarations[0].value_count),
      "urange:0-7F ws delim:! ident:important ws");
}

TEST(CssDeclaration, AUnicodeRangeValueWithABlockAndMoreIsStillInvalid) {
  Parsed p;
  parse_into(p, gtext_css_parse_declaration_list, "unicode-range: {} u+1; x:y");
  EXPECT_EQ(decls_text(p.tree->declarations, p.tree->declaration_count), "x:ident:y");
}

// ---- valid in the current context ---------------------------------------------------------------

struct Validity {
  std::vector<std::string> asked;
  std::string reject_decl;      // a declaration with this name is invalid
  bool reject_lt_prelude = false; // a rule whose prelude has a < delim is invalid
  bool reject_unmatched_in_custom = false; // a custom property with a ] or ) kept as a token is invalid
};

bool has_closer(const GTEXT_CSS_Component_Value * v, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (v[i].kind == GTEXT_CSS_VALUE_TOKEN && (v[i].type == GTEXT_CSS_TOKEN_CLOSE_SQUARE || v[i].type == GTEXT_CSS_TOKEN_CLOSE_PAREN)) {
      return true;
    }
    if (v[i].kind != GTEXT_CSS_VALUE_TOKEN && has_closer(v[i].children, v[i].child_count)) {
      return true;
    }
  }
  return false;
}

bool check_decl(void * user, const GTEXT_CSS_Parent * parent, const GTEXT_CSS_Declaration * d) {
  auto * v = static_cast<Validity *>(user);
  v->asked.push_back("decl " + str(d->name) + (parent->nested ? " nested" : "") + (parent->top_level ? " top" : "") +
                     " depth " + std::to_string(parent->depth) + (d->id == 0 ? "" : " id!"));
  if (str(d->name) == v->reject_decl) {
    return false;
  }
  return !(v->reject_unmatched_in_custom && d->original_text.data && has_closer(d->value, d->value_count));
}

bool check_rule(void * user, const GTEXT_CSS_Parent * parent, const GTEXT_CSS_Rule * r) {
  auto * v = static_cast<Validity *>(user);
  v->asked.push_back(std::string("rule ") + (r->kind == GTEXT_CSS_RULE_AT ? "@" + str(r->name) : text_of(r->prelude, r->prelude_count)) +
                     (parent->top_level ? " top" : " in " + text_of(parent->prelude, parent->prelude_count)));
  if (v->reject_lt_prelude) {
    for (size_t i = 0; i < r->prelude_count; i++) {
      if (r->prelude[i].type == GTEXT_CSS_TOKEN_DELIM && r->prelude[i].delim == '<') {
        return false;
      }
    }
  }
  return true;
}

GTEXT_CSS_Parse_Options with(Validity & v) {
  GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
  o.validity.user = &v;
  o.validity.declaration = check_decl;
  o.validity.rule = check_rule;
  return o;
}

TEST(CssValidity, AnInvalidDeclarationInABlockIsReadAgainAsARule) {
  Validity v;
  v.reject_decl = "x";
  const GTEXT_CSS_Parse_Options o = with(v);
  Parsed p;
  parse_into(p, gtext_css_parse_stylesheet, "a{x:y; z:w}", &o);
  EXPECT_EQ(rules_text(p.tree), "q[ident:a]{z:ident:w}");
  EXPECT_EQ(p.error_names(), (std::vector<std::string>{"invalid-declaration-in-context", "semicolon-in-qualified-rule"}));
}

TEST(CssValidity, AnInvalidNestedRuleEndsTheRunOfDeclarationsBeforeIt) {
  // The invalid rule error (5.5.5): the declarations after it are a list of their own.
  Validity v;
  v.reject_lt_prelude = true;
  const GTEXT_CSS_Parse_Options o = with(v);
  EXPECT_EQ(sheet("a { b:c; .x <y> {} d:e }", &o), "q[ident:a ws]{b:ident:c | nested{d:ident:e}}");
  // invalid-nested-rules.html: parsing goes on after the invalid rule's block.
  EXPECT_EQ(sheet(".a { .b <::::invalid::::> {} & .c {} }", &o),
      "q[delim:. ident:a ws]{ | q[delim:& ws delim:. ident:c ws]{}}");
}

TEST(CssValidity, AnInvalidCustomPropertyThatLooksLikeARuleIsSkippedToItsEnd) {
  // custom-property-rule-ambiguity.html, stylesheet4: with a grammar that refuses an unmatched ] in a
  // custom property, the declaration is read again as a rule, which looks like a custom property, so the
  // remnants of a bad declaration are consumed to the end of the block: there is no .b rule.
  Validity v;
  v.reject_unmatched_in_custom = true;
  const GTEXT_CSS_Parse_Options o = with(v);
  EXPECT_EQ(sheet("div { .a { } --x:hover { ] } .b { } }", &o), "q[ident:div ws]{ | q[delim:. ident:a ws]{}}");
}

TEST(CssValidity, TheCallbacksAreToldTheContext) {
  Validity v;
  const GTEXT_CSS_Parse_Options o = with(v);
  Parsed p;
  parse_into(p, gtext_css_parse_stylesheet, "@m x { a { b:c } } d:e{}", &o);
  EXPECT_EQ(v.asked, (std::vector<std::string>{"decl b nested depth 2", "rule ident:a ws in ws ident:x ws", "rule @m top",
                         "rule ident:d : ident:e top"}));
  Validity w;
  const GTEXT_CSS_Parse_Options o2 = with(w);
  Parsed q;
  parse_into(q, gtext_css_parse_declaration_list, "a:b", &o2);
  EXPECT_EQ(w.asked, (std::vector<std::string>{"decl a top depth 0"}));
}

// ---- identities and spans -----------------------------------------------------------------------

void collect_ids(const GTEXT_CSS_Component_Value * v, size_t n, std::vector<GTEXT_CSS_Id> & out) {
  for (size_t i = 0; i < n; i++) {
    out.push_back(v[i].id);
    collect_ids(v[i].children, v[i].child_count, out);
  }
}

void collect_ids(const GTEXT_CSS_Rule * r, size_t n, std::vector<GTEXT_CSS_Id> & out);

void collect_decls(const GTEXT_CSS_Declaration * d, size_t n, std::vector<GTEXT_CSS_Id> & out) {
  for (size_t i = 0; i < n; i++) {
    out.push_back(d[i].id);
    collect_ids(d[i].value, d[i].value_count, out);
  }
}

void collect_ids(const GTEXT_CSS_Rule * r, size_t n, std::vector<GTEXT_CSS_Id> & out) {
  for (size_t i = 0; i < n; i++) {
    out.push_back(r[i].id);
    collect_ids(r[i].prelude, r[i].prelude_count, out);
    collect_decls(r[i].declarations, r[i].declaration_count, out);
    collect_ids(r[i].rules, r[i].rule_count, out);
    for (size_t k = 0; k < r[i].content_count; k++) {
      if (r[i].contents[k].kind == GTEXT_CSS_ITEM_RULE) {
        collect_ids(r[i].contents[k].rule, 1, out);
      }
      else {
        collect_decls(r[i].contents[k].declarations, r[i].contents[k].declaration_count, out);
      }
    }
  }
}

TEST(CssIdentity, EveryNodeHasAnIdFromOneInDocumentOrder) {
  Parsed p;
  parse_into(p, gtext_css_parse_stylesheet, "a, f(b) { c: d e; g {} h: i } @m x { j: k; l {} }");
  std::vector<GTEXT_CSS_Id> ids;
  collect_ids(p.tree->rules, p.tree->rule_count, ids);
  ASSERT_EQ(ids.size(), p.tree->node_count);
  for (size_t i = 0; i < ids.size(); i++) {
    EXPECT_EQ(ids[i], i + 1) << i;
  }
}

TEST(CssIdentity, IdsAreAFunctionOfTheTreeAlone) {
  // The same tree from different text, after speculation and dropped constructs, has the same ids.
  Parsed a, b;
  parse_into(a, gtext_css_parse_stylesheet, "x{p:q; y{}}");
  parse_into(b, gtext_css_parse_stylesheet, "x{ p : q ; ;; y{} ; } --z:w{} ");
  std::vector<GTEXT_CSS_Id> ia, ib;
  collect_ids(a.tree->rules, a.tree->rule_count, ia);
  collect_ids(b.tree->rules, b.tree->rule_count, ib);
  EXPECT_EQ(ia, ib);
}

TEST(CssSpans, RulesDeclarationsAndValuesCoverTheirText) {
  const std::string css = "a b {\n  c: d !important;\n  e {}\n}";
  Parsed p;
  parse_into(p, gtext_css_parse_stylesheet, css);
  const GTEXT_CSS_Rule & r = p.tree->rules[0];
  EXPECT_EQ(css.substr(r.span.offset, r.span.end - r.span.offset), css);
  EXPECT_EQ(css.substr(r.prelude_span.offset, r.prelude_span.end - r.prelude_span.offset), "a b ");
  EXPECT_EQ(css.substr(r.block_span.offset, r.block_span.end - r.block_span.offset), "{\n  c: d !important;\n  e {}\n}");
  const GTEXT_CSS_Declaration & d = r.declarations[0];
  EXPECT_EQ(css.substr(d.span.offset, d.span.end - d.span.offset), "c: d !important");
  EXPECT_EQ(css.substr(d.value_span.offset, d.value_span.end - d.value_span.offset), "d");
  EXPECT_EQ(d.span.line, 2u);
  EXPECT_EQ(d.span.column, 3u);
  EXPECT_EQ(d.name_span.end - d.name_span.offset, 1u);
  const GTEXT_CSS_Rule & e = r.rules[0];
  EXPECT_EQ(css.substr(e.span.offset, e.span.end - e.span.offset), "e {}");
  EXPECT_EQ(e.span.line, 3u);
  EXPECT_FALSE(r.unclosed);
}

TEST(CssSpans, AnEmptyValueIsAPlaceAndAnUnclosedBlockEndsAtTheEndOfTheInput) {
  const std::string css = "a { b: ;\n c {";
  Parsed p;
  parse_into(p, gtext_css_parse_stylesheet, css);
  const GTEXT_CSS_Rule & r = p.tree->rules[0];
  EXPECT_TRUE(r.unclosed);
  EXPECT_EQ(r.block_span.end, css.size());
  EXPECT_EQ(r.declarations[0].value_span.offset, 7u);
  EXPECT_EQ(r.declarations[0].value_span.end, 7u);
  EXPECT_TRUE(r.rules[0].unclosed);
}

// ---- parse errors and the result ----------------------------------------------------------------

TEST(CssErrors, AreOrderedByPositionWhoeverRaisedThem) {
  Parsed p;
  parse_into(p, gtext_css_parse_stylesheet, "a{b c} d 'e\n f{");
  std::vector<size_t> offsets;
  for (const auto & e : p.errors()) {
    offsets.push_back(e.byte_offset);
  }
  EXPECT_EQ(p.error_names(), (std::vector<std::string>{"close-brace-in-qualified-rule", "newline-in-string"}));
  EXPECT_TRUE(std::is_sorted(offsets.begin(), offsets.end()));
}

TEST(CssErrors, MaxErrorsKeepsTheFirstAndCountsTheRest) {
  GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
  o.max_errors = 2;
  Parsed p;
  parse_into(p, gtext_css_parse_component_values, ") ] } ) ]", &o);
  EXPECT_EQ(gtext_css_result_error_count(&p.result), 2u);
  EXPECT_EQ(gtext_css_result_errors_dropped(&p.result), 3u);
  EXPECT_EQ(p.errors()[1].byte_offset, 2u);
  o.count_errors_only = true;
  Parsed q;
  parse_into(q, gtext_css_parse_component_values, ") ] } ) ]", &o);
  EXPECT_EQ(gtext_css_result_error_count(&q.result), 0u);
  EXPECT_EQ(gtext_css_result_errors_dropped(&q.result), 5u);
}

TEST(CssErrors, ANullResultIsAllowed) {
  GTEXT_CSS_Tree * t = nullptr;
  ASSERT_EQ(gtext_css_parse_stylesheet("a{) }", 5, nullptr, &t, nullptr), GTEXT_CSS_OK);
  EXPECT_EQ(t->rule_count, 1u);
  gtext_css_tree_free(t);
}

TEST(CssErrors, ErrorAtRefusesAnIndexOutOfRange) {
  Parsed p;
  parse_into(p, gtext_css_parse_component_values, ")");
  GTEXT_CSS_Error e;
  EXPECT_TRUE(gtext_css_result_error_at(&p.result, 0, &e));
  EXPECT_FALSE(gtext_css_result_error_at(&p.result, 1, &e));
  EXPECT_FALSE(gtext_css_result_error_at(nullptr, 0, &e));
  EXPECT_FALSE(gtext_css_result_error_at(&p.result, 0, nullptr));
  EXPECT_EQ(gtext_css_result_error_count(nullptr), 0u);
  EXPECT_EQ(gtext_css_result_errors_dropped(nullptr), 0u);
}

TEST(CssResult, CountsWhatTheParseMadeAndIsSafeToFreeTwice) {
  Parsed p;
  parse_into(p, gtext_css_parse_stylesheet, "a{b:c; d{}}");
  EXPECT_EQ(p.result.status, GTEXT_CSS_OK);
  EXPECT_STREQ(p.result.message, "success");
  EXPECT_EQ(p.result.input_bytes, 11u);
  EXPECT_EQ(p.result.rule_count, 2u);
  EXPECT_EQ(p.result.declaration_count, 1u);
  EXPECT_EQ(p.result.token_count, 11u);
  gtext_css_result_free(&p.result);
  gtext_css_result_free(&p.result);
  gtext_css_result_free(nullptr);
  EXPECT_EQ(p.result.internal, nullptr);
}

// ---- arguments ----------------------------------------------------------------------------------

TEST(CssArguments, AnUnusableArgumentIsInvalidAndNothingIsMade) {
  GTEXT_CSS_Result r{};
  GTEXT_CSS_Tree * t = nullptr;
  EXPECT_EQ(gtext_css_parse_stylesheet("a", 1, nullptr, nullptr, &r), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(r.status, GTEXT_CSS_E_INVALID);
  EXPECT_EQ(gtext_css_parse_stylesheet(nullptr, 1, nullptr, &t, &r), GTEXT_CSS_E_INVALID);
  EXPECT_EQ(t, nullptr);
  ASSERT_EQ(gtext_css_parse_stylesheet(nullptr, 0, nullptr, &t, &r), GTEXT_CSS_OK) << "no input is the empty input";
  EXPECT_EQ(t->rule_count, 0u);
  gtext_css_tree_free(t);
  gtext_css_result_free(&r);
  gtext_css_tree_free(nullptr);
}

TEST(CssOptions, TheDefaultsAreSpelledOutAndZeroedIsTheSame) {
  const GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
  EXPECT_EQ(o.max_input_bytes, (size_t)64 * 1024 * 1024);
  EXPECT_EQ(o.max_tokens, (size_t)4 * 1024 * 1024);
  EXPECT_EQ(o.max_depth, 512u);
  EXPECT_EQ(o.max_rules, (size_t)1024 * 1024);
  EXPECT_EQ(o.max_declarations, (size_t)4 * 1024 * 1024);
  EXPECT_EQ(o.max_total_errors, (size_t)1024 * 1024);
  EXPECT_EQ(o.max_errors, 1024u);
  EXPECT_FALSE(o.count_errors_only);
  EXPECT_FALSE(o.input_wtf8);
  EXPECT_EQ(o.allocator, nullptr);
  GTEXT_CSS_Parse_Options zero;
  std::memset(&zero, 0, sizeof zero);
  const std::string css = "a{b:c; @m{d{}}} e:f";
  Parsed a, b;
  parse_into(a, gtext_css_parse_stylesheet, css, &o);
  parse_into(b, gtext_css_parse_stylesheet, css, &zero);
  EXPECT_EQ(canon(a.tree), canon(b.tree));
}

// ---- the allocator ------------------------------------------------------------------------------

TEST(CssAllocator, EveryAllocationOfAParseGoesThroughTheCallersAllocatorAndIsReturned) {
  FaultAllocator fa;
  GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
  o.allocator = &fa.alloc;
  {
    Parsed p;
    parse_into(p, gtext_css_parse_stylesheet, "a{b:c; --d: {e}; unicode-range: u+1; f{}} @m x;", &o);
    ASSERT_EQ(p.status, GTEXT_CSS_OK);
    EXPECT_FALSE(fa.live.empty()) << "the tree and the result are the allocator's";
  }
  EXPECT_TRUE(fa.live.empty());
  EXPECT_EQ(fa.bad_frees, 0u);
}

// ---- threads ------------------------------------------------------------------------------------

TEST(CssThreads, ParsesRunInParallelAndATreeIsReadFromManyThreadsAtOnce) {
  // Run under ThreadSanitizer by `make test-tsan`: no state is shared between parses, and a tree is
  // never written after the parse returns.
  const std::string css = "a { b: c d !important; --e: {f}; g { h: i } } @media x { j {} } k:l";
  Parsed shared;
  parse_into(shared, gtext_css_parse_stylesheet, css);
  const std::string want = canon(shared.tree);
  std::atomic<int> bad{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; i++) {
    threads.emplace_back([&]() {
      for (int k = 0; k < 50; k++) {
        Parsed p;
        parse_into(p, gtext_css_parse_stylesheet, css);
        if (canon(p.tree) != want || canon(shared.tree) != want || serialize(shared.tree) != serialize(p.tree)) {
          bad++;
        }
      }
    });
  }
  for (auto & t : threads) {
    t.join();
  }
  EXPECT_EQ(bad.load(), 0);
}

// ---- An+B (6) -----------------------------------------------------------------------------------

bool anb(const std::string & text, int64_t * a, int64_t * b) {
  Parsed p;
  parse_into(p, gtext_css_parse_component_values, text);
  return gtext_css_anb_parse(p.tree->values, p.tree->value_count, a, b);
}

TEST(CssAnB, ReadsEachProductionOfTheGrammar) {
  struct Case {
    const char * in;
    bool ok;
    int64_t a, b;
  };
  const Case cases[] = {
      {"odd", true, 2, 1}, {"EVEN", true, 2, 0}, {"5", true, 0, 5}, {"-5", true, 0, -5}, {"3n", true, 3, 0},
      {"+n", true, 1, 0}, {"-n", true, -1, 0}, {"n-3", true, 1, -3}, {"-n-3", true, -1, -3}, {"3n-3", true, 3, -3},
      {"3n +2", true, 3, 2}, {"n- 2", true, 1, -2}, {"-n - 2", true, -1, -2}, {" 2n + 1 ", true, 2, 1},
      {"-n-\\35", true, -1, -5}, {"+ n", false, 0, 0}, {"3 n", false, 0, 0}, {"n +-2", false, 0, 0},
      {"2.0n", false, 0, 0}, {"n 2", false, 0, 0}, {"", false, 0, 0}, {"n- -2", false, 0, 0}, {"(n)", false, 0, 0},
  };
  for (const Case & c : cases) {
    int64_t a = 99, b = 99;
    EXPECT_EQ(anb(c.in, &a, &b), c.ok) << c.in;
    if (c.ok) {
      EXPECT_EQ(a, c.a) << c.in;
      EXPECT_EQ(b, c.b) << c.in;
    }
    else {
      EXPECT_EQ(a, 99) << "nothing is written on failure: " << c.in;
    }
  }
}

TEST(CssAnB, AAndBSaturateAtTheRangeOfInt64) {
  int64_t a = 0, b = 0;
  ASSERT_TRUE(anb("99999999999999999999n-99999999999999999999", &a, &b));
  EXPECT_EQ(a, INT64_MAX);
  EXPECT_EQ(b, -INT64_MAX);
  ASSERT_TRUE(anb("-99999999999999999999", &a, &b));
  EXPECT_EQ(b, INT64_MIN);
  EXPECT_FALSE(gtext_css_anb_parse(nullptr, 1, &a, &b));
}

} // namespace
