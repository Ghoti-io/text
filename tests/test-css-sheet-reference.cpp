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
 * The rule index against a brute force for the rules the flat reference of the fuzzer cannot take: nested style rules and `@scope`. Each case is built from a template, so
 * that the expected answer is written by a second route: a nested rule is the flat selector with `&` replaced by `:is(parent)` (CSS Nesting 1), matched by the selector engine
 * on every element; an `@scope` rule matches the elements for which some root (an element the start selector matches) has them as an inclusive descendant, they match
 * `:where(:scope) S` with that root as `:scope`, and no limit element (matched with that root as `:scope`, the root included) has them as an inclusive descendant (Cascade 6, 2.3).
 * The trees and the picks come from a fixed generator, so a failure is reproducible by its seed.
 */

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_cascade_test_support.h"
#include "css_synth_tree.h"
#include "css_test_support.h"

using namespace cas;

namespace {

struct Lcg {
  uint64_t s;
  explicit Lcg(uint64_t seed) : s(seed * 6364136223846793005ull + 1442695040888963407ull) {
  }
  uint32_t next(uint32_t n) {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<uint32_t>((s >> 33) % n);
  }
};

const char * const kNames[] = {"a", "b", "p", "div"};

void build_tree(synth::Tree & t, Lcg & g, int count) {
  for (int i = 0; i < count; i++) {
    const int parent = i == 0 ? 0 : static_cast<int>(g.next(static_cast<uint32_t>(i) + 1));
    std::vector<std::string> classes;
    if (g.next(2)) {
      classes.push_back("x");
    }
    if (g.next(3) == 0) {
      classes.push_back("y");
    }
    t.add(parent, kNames[g.next(4)], "", classes);
  }
}

bool matches(const std::string & selector, synth::Tree & , GTEXT_CSS_Selector_Match_Context * ctx, int el, GTEXT_CSS_Specificity * spec = nullptr) {
  GTEXT_CSS_Selector_List * l = nullptr;
  GTEXT_CSS_Selector_Result r;
  std::memset(&r, 0, sizeof r);
  EXPECT_EQ(gtext_css_selector_parse(selector.data(), selector.size(), nullptr, &l, &r), GTEXT_CSS_OK);
  gtext_css_selector_result_free(&r);
  EXPECT_NE(l, nullptr) << selector;
  if (!l) {
    return false;
  }
  GTEXT_CSS_Selector_Match_Result mr;
  const GTEXT_CSS_Status st = gtext_css_selector_match(l, ctx, synth::Tree::H(el), nullptr, &mr);
  EXPECT_EQ(st, GTEXT_CSS_OK) << selector;
  gtext_css_selector_list_free(l);
  if (spec && mr.matched) {
    *spec = mr.specificity;
  }
  return st == GTEXT_CSS_OK && mr.matched;
}

std::string describe(const synth::Tree & t) {
  std::string out;
  for (size_t i = 1; i < t.e.size(); i++) {
    out += std::to_string(i) + "=" + t.e[i].name;
    for (const std::string & c : t.e[i].classes) {
      out += "." + c;
    }
    out += "<" + std::to_string(t.e[i].parent) + " ";
  }
  return out;
}

std::string replace_all(std::string s, const std::string & from, const std::string & to) {
  for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) {
    s.replace(at, from.size(), to);
  }
  return s;
}

struct Result {
  std::vector<int> elements;
  std::vector<GTEXT_CSS_Specificity> spec;
};

/// The elements the index collects an entry for, with the specificity of the entry.
Result collect_all(const std::string & css, synth::Tree & t, GTEXT_CSS_Selector_Match_Context * ctx, int shadow_scope = 0) {
  Result out;
  css_test::Parsed p;
  css_test::parse_into(p, gtext_css_parse_stylesheet, css);
  GTEXT_CSS_Sheet * sheet = nullptr;
  EXPECT_EQ(gtext_css_sheet_compile(p.tree, R(), nullptr, &sheet, nullptr), GTEXT_CSS_OK) << css;
  GTEXT_CSS_Rule_Index * ix = nullptr;
  GTEXT_CSS_Rule_Index_Options io = gtext_css_rule_index_options_default();
  io.html_document = true;
  EXPECT_EQ(gtext_css_rule_index_new(R(), &io, &ix), GTEXT_CSS_OK);
  GTEXT_CSS_Sheet_Registration reg;
  std::memset(&reg, 0, sizeof reg);
  reg.origin = GTEXT_CSS_ORIGIN_AUTHOR;
  if (shadow_scope) {
    reg.shadow = true;
    reg.tree_scope = synth::Tree::scope_handle(shadow_scope);
    reg.context = 1;
  }
  EXPECT_EQ(gtext_css_rule_index_add_sheet(ix, sheet, &reg, nullptr), GTEXT_CSS_OK) << css;
  EXPECT_EQ(gtext_css_rule_index_freeze(ix, nullptr), GTEXT_CSS_OK);
  for (int el = 1; el < static_cast<int>(t.e.size()); el++) {
    Set set(nullptr);
    EXPECT_EQ(gtext_css_rules_collect(ix, ctx, synth::Tree::H(el), nullptr, set.s, nullptr, nullptr), GTEXT_CSS_OK) << css;
    if (gtext_css_candidates_count(set.s)) {
      GTEXT_CSS_Cascade_Entry e;
      gtext_css_candidates_entry(set.s, 0, &e);
      out.elements.push_back(el);
      out.spec.push_back(e.specificity);
    }
  }
  gtext_css_rule_index_free(ix);
  gtext_css_sheet_free(sheet);
  return out;
}

}  // namespace

TEST(CssSheetReference, NestedRulesAreTheFlatSelectorWithIsOfTheParent) {
  const char * const parents[] = {"a", ".x", "p.y", "div > a", "a, .y", "div p", ":is(a, b) .x", "*"};
  const char * const children[] = {"&", "& p", "&.x", "> p", "+ b", "~ .x", ".y &", "p", "& > .x", "div &", "&:first-child", "&:not(.y)", "&, .x", ":is(&, a) p", "& &"};
  for (uint64_t seed = 1; seed <= 40; seed++) {
    Lcg g(seed);
    synth::Tree t;
    build_tree(t, g, 6 + static_cast<int>(g.next(8)));
    GTEXT_CSS_Selector_Match_Options mo = gtext_css_selector_match_options_default();
    mo.html_document = true;
    GTEXT_CSS_Selector_Match_Context * ctx = nullptr;
    ASSERT_EQ(gtext_css_selector_match_context_new(&t.table, &mo, &ctx), GTEXT_CSS_OK);
    for (const char * parent : parents) {
      for (const char * child : children) {
        // each selector of the child list is relative to the parent: with a leading combinator, or without a `&`, it is `& ` and the text
        std::string flat;
        {
          const std::string cl = child;
          size_t from = 0;
          int depth = 0;
          for (size_t i = 0; i <= cl.size(); i++) {
            if (i < cl.size() && cl[i] == '(') {
              depth++;
            }
            else if (i < cl.size() && cl[i] == ')') {
              depth--;
            }
            if (i == cl.size() || (cl[i] == ',' && depth == 0)) {
              std::string one = cl.substr(from, i - from);
              while (!one.empty() && one[0] == ' ') {
                one.erase(0, 1);
              }
              if (one[0] == '>' || one[0] == '+' || one[0] == '~' || one.find('&') == std::string::npos) {
                one = "& " + one;
              }
              flat += (flat.empty() ? "" : ", ") + replace_all(one, "&", ":is(" + std::string(parent) + ")");
              from = i + 1;
            }
          }
        }
        const std::string css = std::string(parent) + " { " + child + " { color: red } }";
        const Result got = collect_all(css, t, ctx);
        std::vector<int> want;
        for (int el = 1; el < static_cast<int>(t.e.size()); el++) {
          GTEXT_CSS_Specificity sp{};
          if (matches(flat, t, ctx, el, &sp)) {
            want.push_back(el);
          }
        }
        EXPECT_EQ(got.elements, want) << "seed " << seed << ": " << css << "  ==  " << flat << "\n" << describe(t);
      }
    }
    gtext_css_selector_match_context_free(ctx);
  }
}

TEST(CssSheetReference, ScopeRulesAreTheRootsTheirDescendantsAndTheLimits) {
  const char * const starts[] = {"a", ".x", "div", "p.y", ":is(a, div)", "div > a"};
  const char * const limits[] = {"", "p", ".y", "b", "a", ":scope", "&", "div > p"};
  const char * const subjects[] = {"p", "*", ".x", "a p", "& > a", ":scope > b", ":scope", "b + p", "div", "&"};
  for (uint64_t seed = 1; seed <= 30; seed++) {
    Lcg g(seed + 1000);
    synth::Tree t;
    build_tree(t, g, 7 + static_cast<int>(g.next(8)));
    GTEXT_CSS_Selector_Match_Options mo = gtext_css_selector_match_options_default();
    mo.html_document = true;
    GTEXT_CSS_Selector_Match_Context * ctx = nullptr;
    ASSERT_EQ(gtext_css_selector_match_context_new(&t.table, &mo, &ctx), GTEXT_CSS_OK);
    GTEXT_CSS_Selector_Match_Context * refc = nullptr;
    ASSERT_EQ(gtext_css_selector_match_context_new(&t.table, &mo, &refc), GTEXT_CSS_OK);
    const int n = static_cast<int>(t.e.size());
    auto inside = [&](int e, int root) { // e is root or below it
      for (int x = e; x; x = t.e[x].parent) {
        if (x == root) {
          return true;
        }
      }
      return false;
    };
    for (const char * start : starts) {
      for (const char * limit : limits) {
        for (const char * subject : subjects) {
          const std::string css = std::string("@scope (") + start + ")" + (limit[0] ? std::string(" to (") + limit + ")" : "") + " { " + subject + " { color: red } }";
          const Result got = collect_all(css, t, ctx);
          // the reference
          std::string rel = subject;
          const bool has_self = rel.find('&') != std::string::npos || rel.find(":scope") != std::string::npos;
          rel = replace_all(rel, "&", ":where(:scope)");
          if (!has_self) {
            rel = ":where(:scope) " + rel;
          }
          // the end selector is relative too (Cascade 6, 2.1): it has the implied `:scope ` unless it names the root itself
          std::string lim = limit;
          if (lim.find('&') != std::string::npos || lim.find(":scope") != std::string::npos) {
            lim = replace_all(lim, "&", ":scope");
          }
          else if (lim[0]) {
            lim = ":scope " + lim;
          }
          std::vector<int> want;
          for (int el = 1; el < n; el++) {
            bool in_scope = false;
            for (int root = 1; root < n && !in_scope; root++) {
              gtext_css_selector_match_set_scope(refc, nullptr, nullptr);
              if (!inside(el, root) || !matches(start, t, refc, root)) {
                continue;
              }
              gtext_css_selector_match_set_scope(refc, synth::Tree::H(root), nullptr);
              bool limited = false;
              if (limit[0]) {
                for (int l = el; l && !limited; l = t.e[l].parent) {
                  limited = inside(l, root) && matches(lim, t, refc, l);
                  if (l == root) {
                    break;
                  }
                }
              }
              if (!limited && matches(rel, t, refc, el)) {
                in_scope = true;
              }
            }
            if (in_scope) {
              want.push_back(el);
            }
          }
          EXPECT_EQ(got.elements, want) << "seed " << seed << ": " << css << "  ==  " << rel << "\n" << describe(t);
        }
      }
    }
    gtext_css_selector_match_context_free(refc);
    gtext_css_selector_match_context_free(ctx);
  }
}

TEST(CssSheetReference, TheShadowSelectorsHaveEquivalentSpellingsThatCollectTheSameElements) {
  // `:is(:host)` is `:host`, a rule nested under `:host` with `&` is the flat rule with `:host`, and the same through `:where()`; the index finds the host through its buckets and the
  // matcher through the featureless host, and the two routes have to give one answer, on trees of several shapes
  struct Pair {
    const char * a;
    const char * b;
  };
  const Pair pairs[] = {
      {":host > span", ":is(:host) > span"},
      {":host > span", ":where(:host) > span"},
      {":host .in", ":is(:host) .in"},
      {":host .in", ":host { & .in { color: red } }"},
      {":host > *", ":host { & > * { color: red } }"},
      {":host > *", ":host { > * { color: red } }"},
      {":host(.c0) span", ":is(:host(.c0)) span"},
      {":host(.c0) span", ":host(.c0) { & span { color: red } }"},
      {":host-context(.c1) span", ":is(:host-context(.c1)) span"},
      {":host-context(.c1) .in", ":host-context(.c1) { & .in { color: red } }"},
      {":host", ":is(:host)"},
      {":host(.c0)", ":is(:host(.c0), .zz)"},
      {":host", ":host { & { color: red } }"},
      {":host(.c0)::part(a)", ":is(:host(.c0))::part(a)"},
  };
  for (uint64_t seed = 1; seed <= 25; seed++) {
    Lcg g(seed + 77);
    synth::Tree t;
    const int host = t.add(0, "x-host", "", g.next(2) ? std::vector<std::string>{"c0", "c1"} : std::vector<std::string>{"c1"});
    const int light = t.add(host, "i");
    const int outer = t.add(0, "div", "", {"c1"});
    const int host2 = t.add(outer, "x-host", "", {g.next(2) ? "c0" : "c9"});
    (void)light;
    const int sc = t.attach_shadow(host);
    const int top = t.add_in_shadow(sc, "span", "", {"in"});
    t.add(top, "span", "", {"in"});
    const int slot = t.add_in_shadow(sc, "slot");
    t.e[light].slot = slot;
    t.e[top].parts.insert({host, "a"});
    const int sc2 = t.attach_shadow(host2);
    const int top2 = t.add_in_shadow(sc2, "span", "", {"in"});
    (void)top2;
    GTEXT_CSS_Selector_Match_Options mo = gtext_css_selector_match_options_default();
    mo.html_document = true;
    GTEXT_CSS_Selector_Match_Context * ctx = nullptr;
    ASSERT_EQ(gtext_css_selector_match_context_new(&t.table, &mo, &ctx), GTEXT_CSS_OK);
    for (const Pair & p : pairs) {
      const std::string a = std::strstr(p.a, "{") ? p.a : std::string(p.a) + " { color: red }";
      const std::string b = std::strstr(p.b, "{") ? p.b : std::string(p.b) + " { color: red }";
      EXPECT_EQ(collect_all(a, t, ctx, sc).elements, collect_all(b, t, ctx, sc).elements) << "seed " << seed << ": " << a << "  vs  " << b << "\n" << describe(t);
      // the rule is found for the other host and its tree too, and never for the document
      EXPECT_EQ(collect_all(a, t, ctx, sc2).elements, collect_all(b, t, ctx, sc2).elements) << "seed " << seed << " (second host): " << a << "  vs  " << b;
    }
    // not vacuous: the host's top element is found by `:host > span`, the host by `:host`
    EXPECT_FALSE(collect_all(":host > span { color: red }", t, ctx, sc).elements.empty()) << "seed " << seed;
    EXPECT_FALSE(collect_all(":host { color: red }", t, ctx, sc).elements.empty()) << "seed " << seed;
    gtext_css_selector_match_context_free(ctx);
  }
}
