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
 * The serializer separates any two tokens that would read back as something else together (CSS Syntax
 * 9). Every ordered pair of a list of tokens with adversarial values, and every triple of a smaller list,
 * is written between two empty comments (so the tokens are adjacent in the tree), serialized, and read
 * again: the tree must be the same. This is how `--` followed by `>` was found to read back as the CDC
 * token, by the fuzzer; the sweep is the table of section 9 held to its own requirement for the whole
 * alphabet and not for the pairs somebody thought of.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_test_support.h"

using namespace css_test;

namespace {

// Each of these is one token (or one function or block) when read alone.
const char * const kTokens[] = {
    "a", "u", "U", "-x", "--", "a--", "--a", "\\-\\-", "e", "E", "f", "x1", "_",
    "1", "1.5", "-1", "+1", ".5", "1e3", "12px", "1em", "1e", "5%", "1--", "1u", "1U",
    "#a", "#1", "#--", "#-", "@a", "@--", "@-x", "@u",
    "\"s\"", "'s'", "url(x)", "url()", "f(x)", "f()", "(x)", "[x]", "{x}", "()",
    "!", "#", "$", "%", "&", "*", "+", ",", "-", ".", "/", ":", ";", "<", "=", ">", "?", "@", "\\\n", "^", "|", "~", "`",
    "<!--", "-->", " ", "\xc2\xb7", "\xe2\x82\xac",
};

std::string write_and_read(const std::string & text, std::string * out_text) {
  Parsed p;
  parse_into(p, gtext_css_parse_component_values, text);
  if (p.status != GTEXT_CSS_OK) {
    return "parse failed";
  }
  const std::string before = canon(p.tree);
  const std::string written = serialize(p.tree);
  *out_text = written;
  Parsed q;
  parse_into(q, gtext_css_parse_component_values, written);
  if (q.status != GTEXT_CSS_OK) {
    return "reparse failed";
  }
  return canon(q.tree) == before ? "" : "different tree";
}

TEST(CssSerializeSeparation, EveryOrderedPairOfTokensReadsBackAsItself) {
  size_t pairs = 0;
  std::vector<std::string> failures;
  for (const char * a : kTokens) {
    for (const char * b : kTokens) {
      const std::string text = std::string(a) + "/**/" + b;
      std::string written;
      const std::string why = write_and_read(text, &written);
      pairs++;
      if (!why.empty()) {
        failures.push_back(text + " -> " + written + ": " + why);
      }
    }
  }
  EXPECT_GT(pairs, 4000u);
  for (const std::string & f : failures) {
    ADD_FAILURE() << f;
  }
}

TEST(CssSerializeSeparation, EveryTripleOfTheEdgeTokensReadsBackAsItself) {
  const char * const edge[] = {"a", "u", "--", "-x", "1", "-", "+", ".", "<", "!", ">", "#", "@", "/", "*", "?", "%", "e", "U", "-->", "<!--", "(x)"};
  size_t triples = 0;
  std::vector<std::string> failures;
  for (const char * a : edge) {
    for (const char * b : edge) {
      for (const char * c : edge) {
        const std::string text = std::string(a) + "/**/" + b + "/**/" + c;
        std::string written;
        const std::string why = write_and_read(text, &written);
        triples++;
        if (!why.empty()) {
          failures.push_back(text + " -> " + written + ": " + why);
        }
      }
    }
  }
  EXPECT_GT(triples, 10000u);
  for (const std::string & f : failures) {
    ADD_FAILURE() << f;
  }
}

TEST(CssSerializeSeparation, AnIdentThatIsTwoHyphensIsNotTheStartOfACdcToken) {
  // The fuzzer's find: the ident "--" (written \-\-> or --) followed by a ">" delim.
  Parsed p;
  parse_into(p, gtext_css_parse_component_values, "\\--/**/>");
  ASSERT_EQ(p.status, GTEXT_CSS_OK);
  EXPECT_EQ(text_of(p.tree->values, p.tree->value_count), "ident:-- delim:>");
  EXPECT_EQ(serialize(p.tree), "--/**/>");
}

} // namespace
