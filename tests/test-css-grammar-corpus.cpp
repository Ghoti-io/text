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
 * The grammar-corpus gate: tools/css/grammar-corpus.txt is a seed list of
 * grammars in the shape the CSS specifications give (every construct of the
 * Value Definition Syntax, the longest property grammars, recursive types),
 * each with witnesses and counter-witnesses.
 *
 * Every grammar must compile and link; every witness must match (and, where the
 * list says how, be written as that text, which must match again and be written
 * the same); no counter-witness may match. The gate is shown to be able to fail
 * in two ways: the construct census (below) fails when a construct is taken out
 * of the list, and tools/css/plant-grammar.py plants a defect in the engine for
 * each construct and runs this program.
 */

#include <algorithm>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_values_test_support.h"

using namespace gv;

namespace {

struct Case {
  std::string input;
  std::string text;  // empty: not checked
  bool has_text = false;
  bool witness = true;
};

struct Entry {
  std::string grammar;
  std::vector<Case> cases;
  size_t line = 0;
};

struct Corpus {
  std::map<std::string, std::string> types, props;
  std::vector<Entry> entries;
  std::vector<std::string> definition_lines;  // type and prop lines, for the census
};

std::string slurp(const std::string & path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string find_file(const std::string & relative) {
  const std::string here = __FILE__;
  const size_t slash = here.rfind('/');
  for (const std::string & root : {slash == std::string::npos ? std::string(".") : here.substr(0, slash), std::string("tests"), std::string("..")}) {
    const std::string path = root + "/" + relative;
    std::ifstream in(path, std::ios::binary);
    if (in) {
      return path;
    }
  }
  return "";
}

const std::regex & cached_regex(const char * pattern) {
  static std::map<const char *, std::regex> cache;
  auto it = cache.find(pattern);
  if (it == cache.end()) {
    it = cache.emplace(pattern, std::regex(pattern)).first;
  }
  return it->second;
}

std::string trim(const std::string & s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t')) {
    a++;
  }
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) {
    b--;
  }
  return s.substr(a, b - a);
}

Corpus parse_corpus(const std::string & text, std::vector<std::string> * problems) {
  Corpus c;
  std::istringstream in(text);
  std::string line;
  size_t n = 0;
  while (std::getline(in, line)) {
    n++;
    if (trim(line).empty() || line[0] == '#') {
      continue;
    }
    auto starts = [&](const char * p) { return line.compare(0, strlen(p), p) == 0; };
    if (starts("type ") || starts("prop ")) {
      const size_t eq = line.find(" = ");
      if (eq == std::string::npos) {
        problems->push_back("line " + std::to_string(n) + ": no ' = '");
        continue;
      }
      (starts("type ") ? c.types : c.props)[trim(line.substr(5, eq - 5))] = trim(line.substr(eq + 3));
      c.definition_lines.push_back(line);
    }
    else if (starts("grammar ")) {
      Entry e;
      e.grammar = trim(line.substr(8));
      e.line = n;
      c.entries.push_back(e);
    }
    else if ((line[0] == '+' || line[0] == '-') && !c.entries.empty()) {
      Case k;
      k.witness = line[0] == '+';
      std::string rest = line.substr(1);
      const size_t arrow = rest.find(" => ");
      if (arrow != std::string::npos) {
        k.text = trim(rest.substr(arrow + 4));
        k.has_text = true;
        rest = rest.substr(0, arrow);
      }
      k.input = trim(rest);
      c.entries.back().cases.push_back(k);
    }
    else {
      problems->push_back("line " + std::to_string(n) + ": not understood: " + line);
    }
  }
  return c;
}

/// Every construct of the notation that a text uses; the census of the list.
std::set<std::string> constructs_of(const std::string & t) {
  std::set<std::string> s;
  auto has = [&](const char * x) { return t.find(x) != std::string::npos; };
  if (t.find(' ') != std::string::npos) s.insert("juxtaposition");
  if (has("&&")) s.insert("&&");
  if (has("||")) s.insert("||");
  if (std::regex_search(t, cached_regex("[^|]\\|[^|]"))) s.insert("|");
  if (has("[ ")) s.insert("group");
  if (std::regex_search(t, cached_regex("[a-z>\\]\\)]\\?"))) s.insert("?");
  if (std::regex_search(t, cached_regex("[a-z>\\]\\)]\\*"))) s.insert("*");
  if (std::regex_search(t, cached_regex("[a-z>\\]\\)]\\+[^a-z]"))) s.insert("+");
  if (std::regex_search(t, cached_regex("[a-z>\\]\\)]#"))) s.insert("#");
  if (std::regex_search(t, cached_regex("\\{[0-9]+\\}"))) s.insert("{A}");
  if (std::regex_search(t, cached_regex("\\{[0-9]+,[0-9]+\\}"))) s.insert("{A,B}");
  if (std::regex_search(t, cached_regex("\\{[0-9]+,\\}"))) s.insert("{A,}");
  if (std::regex_search(t, cached_regex("#\\{[0-9]+(,[0-9]+)?\\}"))) s.insert("#{A,B}");
  if (has("]!")) s.insert("!");
  if (has("\xE2\x88\x9E")) s.insert("range with infinity");
  if (std::regex_search(t, cached_regex("\\[[-0-9.]+[a-z%]+,"))) s.insert("range with units");
  if (std::regex_search(t, cached_regex("<[a-z-]+ \\["))) s.insert("range");
  if (std::regex_search(t, cached_regex("<[a-z-]+>"))) s.insert("<type>");
  if (std::regex_search(t, cached_regex("<'[a-z-]+'>"))) s.insert("<'property'>");
  if (std::regex_search(t, cached_regex("<[a-z-]+\\(\\)>"))) s.insert("<function()>");
  if (std::regex_search(t, cached_regex("[a-zA-Z]\\( "))) s.insert("function notation");
  if (std::regex_search(t, cached_regex("[a-zA-Z]\\(\\)"))) s.insert("empty function notation");
  if (has("{ <")) s.insert("{ } block");
  if (has("( <")) s.insert("( ) block");
  if (has(" , ")) s.insert("literal ,");
  if (has(" / ")) s.insert("literal /");
  if (has("':'") || has("'='")) s.insert("quoted literal");
  if (std::regex_search(t, cached_regex("(^| )[a-z][a-z-]*( |$)"))) s.insert("keyword");
  for (const char * native : {"integer", "number", "percentage", "dimension", "length", "angle", "time", "frequency", "resolution", "flex", "ratio",
           "zero", "string", "url", "ident", "custom-ident", "dashed-ident", "any-value", "declaration-value", "urange", "an+b",
           "declaration-list", "ident-token", "function-token", "at-keyword-token", "hash-token", "string-token", "bad-string-token",
           "url-token", "delim-token", "number-token", "percentage-token", "dimension-token", "whitespace-token", "CDO-token", "CDC-token",
           "colon-token", "semicolon-token", "comma-token", "[-token", "]-token", "(-token", ")-token", "{-token", "}-token"}) {
    if (t.find(std::string("<") + native + ">") != std::string::npos || t.find(std::string("<") + native + " [") != std::string::npos) {
      s.insert(std::string("<") + native + ">");
    }
  }
  return s;
}

std::vector<std::string> all_constructs() {
  std::vector<std::string> v = {"juxtaposition", "&&", "||", "|", "group", "?", "*", "+", "#", "{A}", "{A,B}", "{A,}", "#{A,B}", "!",
      "range", "range with infinity", "range with units", "<type>", "<'property'>", "<function()>", "function notation",
      "empty function notation", "{ } block", "( ) block", "literal ,", "literal /", "quoted literal", "keyword"};
  for (const char * native : {"integer", "number", "percentage", "dimension", "length", "angle", "time", "frequency", "resolution", "flex", "ratio",
           "zero", "string", "url", "ident", "custom-ident", "dashed-ident", "any-value", "declaration-value", "urange", "an+b",
           "ident-token", "function-token", "at-keyword-token", "hash-token", "string-token", "bad-string-token", "url-token",
           "delim-token", "number-token", "percentage-token", "dimension-token", "whitespace-token", "CDO-token", "CDC-token", "colon-token",
           "semicolon-token", "comma-token", "[-token", "]-token", "(-token", ")-token", "{-token", "}-token", "declaration-list"}) {
    v.push_back(std::string("<") + native + ">");
  }
  return v;
}

/// The constructs that the list uses, from the text of its grammars and definitions.
std::set<std::string> census(const Corpus & c) {
  std::set<std::string> used;
  for (const Entry & e : c.entries) {
    for (const std::string & x : constructs_of(e.grammar)) used.insert(x);
  }
  for (const auto & t : c.types) for (const std::string & x : constructs_of(t.second)) used.insert(x);
  for (const auto & t : c.props) for (const std::string & x : constructs_of(t.second)) used.insert(x);
  return used;
}

std::vector<std::string> missing(const Corpus & c) {
  const std::set<std::string> used = census(c);
  std::vector<std::string> out;
  for (const std::string & k : all_constructs()) {
    if (!used.count(k)) {
      out.push_back(k);
    }
  }
  return out;
}

std::string with_decl(const std::string & input, bool * is_decl) {
  *is_decl = input.compare(0, 6, "@decl ") == 0;
  return *is_decl ? input.substr(6) : input;
}

/// Match `input` (a value, or an "@decl " declaration); the match, and the written text.
void match_case(Matched & m, const Grammar & g, const std::string & input) {
  bool decl;
  const std::string body = with_decl(input, &decl);
  if (decl) {
    match_declaration(m, g, body);
  }
  else {
    match_into(m, g, body);
  }
}

class Corpus_ : public ::testing::Test {
 protected:
  void SetUp() override {
    const std::string path = find_file("../tools/css/grammar-corpus.txt");
    ASSERT_NE(path, "") << "tools/css/grammar-corpus.txt is not where the tests can find it";
    std::vector<std::string> problems;
    corpus = parse_corpus(slurp(path), &problems);
    ASSERT_TRUE(problems.empty()) << problems[0];
  }
  Corpus corpus;
};

TEST_F(Corpus_, EveryGrammarCompilesAndLinksEveryWitnessMatchesAndNoCounterWitnessDoes) {
  Defs defs;
  defs.types = corpus.types;
  defs.props = corpus.props;
  size_t witnesses = 0, counters = 0;
  for (const Entry & e : corpus.entries) {
    Grammar g;
    compile_into(g, e.grammar, &defs);
    ASSERT_TRUE(g.ok()) << "line " << e.line << ": " << e.grammar << " -> " << gtext_css_status_string(g.status) << " " << g.result.name;
    for (const Case & k : e.cases) {
      Matched m;
      match_case(m, g, k.input);
      ASSERT_EQ(m.status, GTEXT_CSS_OK) << "line " << e.line << ": " << e.grammar << " | " << k.input;
      if (!k.witness) {
        counters++;
        EXPECT_FALSE(m.matched) << "line " << e.line << ": " << e.grammar << " | counter-witness " << k.input << " -> " << m.dump();
        continue;
      }
      witnesses++;
      ASSERT_TRUE(m.matched) << "line " << e.line << ": " << e.grammar << " | witness " << k.input;
      const std::string text = m.text();
      if (k.has_text) {
        EXPECT_EQ(text, k.text) << "line " << e.line << ": " << e.grammar << " | " << k.input;
      }
      // the fixed point
      bool decl;
      with_decl(k.input, &decl);
      Matched again;
      match_case(again, g, decl ? "@decl " + std::string(k.input.compare(6, 4, "unic") == 0 ? "unicode-range: " : "x: ") + text.substr(0) : text);
      if (decl && k.input.compare(6, 4, "unic") != 0) {
        continue;  // a block value is written without its declaration around it
      }
      ASSERT_TRUE(again.matched) << "line " << e.line << ": the written text does not match again: " << text;
      EXPECT_EQ(again.text(), text) << "line " << e.line << ": the fixed point";
    }
  }
  EXPECT_GE(corpus.entries.size(), 45u);
  EXPECT_GE(witnesses, 65u);
  EXPECT_GE(counters, 45u);
}

TEST_F(Corpus_, EveryConstructOfTheNotationAndEveryNativeTypeIsUsed) {
  EXPECT_TRUE(missing(corpus).empty()) << "not used: " << [&]() {
    std::string s;
    for (const std::string & m : missing(corpus)) {
      s += m + "  ";
    }
    return s;
  }();
}

TEST_F(Corpus_, TheCensusFailsWhenAConstructIsTakenOutOfTheList) {
  // control: remove every grammar and definition that uses a construct, and the census must name it
  for (const std::string & construct : all_constructs()) {
    Corpus smaller = corpus;
    smaller.entries.clear();
    for (const Entry & e : corpus.entries) {
      if (!constructs_of(e.grammar).count(construct)) {
        smaller.entries.push_back(e);
      }
    }
    for (auto it = smaller.types.begin(); it != smaller.types.end();) {
      it = constructs_of(it->second).count(construct) ? smaller.types.erase(it) : std::next(it);
    }
    for (auto it = smaller.props.begin(); it != smaller.props.end();) {
      it = constructs_of(it->second).count(construct) ? smaller.props.erase(it) : std::next(it);
    }
    const std::vector<std::string> gone = missing(smaller);
    EXPECT_NE(std::find(gone.begin(), gone.end(), construct), gone.end()) << "taking out " << construct << " went unnoticed";
  }
}

TEST_F(Corpus_, EveryGrammarOfTheListCompilesToTheSameGrammarEveryTime) {
  Defs defs;
  defs.types = corpus.types;
  defs.props = corpus.props;
  for (const Entry & e : corpus.entries) {
    Grammar a, b;
    compile_into(a, e.grammar, &defs);
    compile_into(b, e.grammar, &defs);
    ASSERT_TRUE(a.ok() && b.ok()) << e.grammar;
    EXPECT_EQ(gtext_css_compiled_node_count(a.c), gtext_css_compiled_node_count(b.c)) << e.grammar;
  }
}

} // namespace
