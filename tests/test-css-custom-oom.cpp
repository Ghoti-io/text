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
 * Allocation failure in the substitution layer: a style computed from declarations that use every kind of function (`var()` chains and cycles, `env()`, `attr()`,
 * `if()`, `cycle()`, `random()`, a registered property, a shorthand holding a function), the `@property` rules collected, and a custom property value made, each with
 * the n-th allocation failed for every n up to the last one the call makes.
 *
 * A failed run is GTEXT_CSS_E_OOM with no style returned, or the answer of the run that failed nothing; either way the allocator holds no block afterwards and was
 * never asked to free one it did not make. The sweep also checks that a run that failed nothing gives the answer of the first, so a failure that was swallowed and
 * turned into a different value is found.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_custom_test_support.h"
#include "markup_test_support.h"

using namespace cust;

namespace {

struct Scenario {
  const char * name;
  const char * parent;
  const char * decls;
};

const Scenario kScenarios[] = {
    {"chain", "", "--a: 1px; --b: var(--a) 2px; --c: var(--b) var(--b); width: var(--a)"},
    {"cycle", "", "--a: var(--b); --b: var(--a); width: var(--a, 4px)"},
    {"fallback", "", "--x: var(--none, var(--nope, 3px)); margin: var(--x) 1px"},
    {"shorthand", "", "--m: 1px 2px; margin: var(--m); border: var(--m) solid red"},
    {"env", "", "width: env(nosuch, 4px); height: env(also, var(--q, 2px))"},
    {"inherit", "color: red; --p: 5px", "--a: var(--p); color: var(--zz)"},
    {"cycleFn", "margin-top: 1px", "margin-top: cycle(1px, 2px, 3px); margin: cycle(1px 2px, 4px)"},
    {"randomKey", "", "opacity: random(--r, 0.2, 0.6); width: random(--r2 element-scoped, 1px, 3px)"},
    {"randomCalc", "", "width: random(1px, 2em); height: random(--q, 3px, 20%); margin-top: random(1em, 2em)"},
    {"random", "", "width: random(100px, 200px); --r: random-item(--k, 1px, 2px, 3px); height: var(--r)"},
    {"spread", "", "--l: 1px, 2px; --s: random-item(--k2, var(--l)...); width: var(--s)"},
    {"attr", "", "--u: attr(data-u); width: attr(data-w px, 2px); --s: attr(data-s type(*)); height: calc(attr(data-n type(<number>)) * 1px); content: var(--s) var(--u)"},
    {"attrFallback", "", "width: attr(missing px, var(--q, 5px)); --t: attr(data-s raw-string, x); margin-top: var(--none, attr(data-w px))"},
    {"if", "", "@--a|1px@--c|if(style(--a: 1px): 2px; else: 3px)@width|if(media(all): 4px; else: 5px)@height|if(style(--c: 2px): var(--c); else: 6px)"},
    {"ident", "", "--i: ident(foo, \"-\", 3); content: var(--i); counter-reset: ident(a, b)"},
    {"siblings", "", "width: calc(sibling-index() * 1px); height: calc(sibling-count() * 2px); --k: calc(sibling-index() + sibling-count()); margin-top: calc(var(--k) * 1px)"},
    {"registeredColor", "", "--col: tomato; --len: 2em; color: var(--col); width: var(--len); --any: 1px+2px"},
    {"registeredColorAlpha", "", "--col: #badbee33; color: var(--col); --len: calc(1px + 2px); height: var(--len)"},
    {"doubling", "", "--n0: w; --n1: var(--n0) var(--n0); --n2: var(--n1) var(--n1); --n3: var(--n2) var(--n2); content: var(--n3)"},
};

const Node kNode = [] {
  Node n;
  n.attrs = {{"data-u", "http://x/y.png"}, {"data-w", "12"}, {"data-s", "a b c"}, {"data-n", "3.5"}};
  n.index = 2;
  n.count = 5;
  return n;
}();

/// Declarations as text, or, when the text begins with `@`, as `@name|value@name|value` for the values that hold a `;`.
void compute_scenario(World & w, Computed & c, const char * decls, const Computed * parent) {
  if (decls[0] != '@') {
    compute(w, c, decls, parent, &kNode);
    return;
  }
  std::vector<std::pair<std::string, std::string>> pairs;
  const std::string all(decls + 1);
  size_t pos = 0;
  while (pos <= all.size()) {
    size_t end = all.find('@', pos);
    if (end == std::string::npos) {
      end = all.size();
    }
    const std::string item = all.substr(pos, end - pos);
    const size_t bar = item.find('|');
    pairs.emplace_back(item.substr(0, bar), item.substr(bar + 1));
    pos = end + 1;
  }
  compute_pairs(w, c, pairs, parent, &kNode);
}

const std::vector<Reg> kRegs = {
    {"--len", "<length>", "0px", true, true, false},
    {"--col", "<color>", "red", false, true, false},
    {"--any", "*", "", true, false, false},
};

std::string describe(World & w, Computed & c, const std::vector<std::string> & names) {
  std::string out;
  for (const std::string & n : names) {
    out += n + "=" + c.value(w.registry.r, n) + ";";
  }
  for (const char * n : {"--a", "--b", "--c", "--x", "--m", "--p", "--r", "--s", "--n3", "--len", "--col"}) {
    out += std::string(n) + "=" + c.custom(n) + ";";
  }
  return out;
}

TEST(CssCustomOom, ComputingAStyleSweptOverEveryAllocation) {
  // The context is made once, unarmed (its own sweep is the stride sweep of test-css-compute-oom.cpp); the allocation failed is the n-th of the computation.
  const std::vector<std::string> props = {"width", "height", "color", "margin-top", "margin-left", "border-top-width", "content"};
  for (const Scenario & sc : kScenarios) {
    FaultAllocator fa;
    {
      World w;
      w.options.allocator = &fa.alloc;
      w.condition = [](GTEXT_CSS_Condition_Kind, const std::string & text) { return text.find("media") != std::string::npos || text.find("1px") != std::string::npos ? GTEXT_CSS_TRI_TRUE : GTEXT_CSS_TRI_FALSE; };
      ASSERT_EQ(w.build(kRegs), GTEXT_CSS_OK);
      Computed p;
      compute(w, p, sc.parent);
      ASSERT_EQ(p.status, GTEXT_CSS_OK) << sc.name;
      fa.arm(0);
      std::string want;
      {
        Computed c;
        compute_scenario(w, c, sc.decls, &p);
        ASSERT_EQ(c.status, GTEXT_CSS_OK) << sc.name;
        want = describe(w, c, props);
      }
      ASSERT_GT(fa.calls, 10u) << sc.name;
      size_t failures = 0;
      for (size_t n = 1; n < 200000; n++) {
        std::string got;
        GTEXT_CSS_Status st;
        {
          fa.arm(n);
          Computed c;
          compute_scenario(w, c, sc.decls, &p);
          fa.disarm();
          st = c.status;
          if (st == GTEXT_CSS_OK) {
            got = describe(w, c, props);
          }
          else {
            EXPECT_EQ(c.s, nullptr) << sc.name << " " << n;
          }
        }
        if (!fa.failed) {
          EXPECT_EQ(st, GTEXT_CSS_OK) << sc.name << " " << n;
          EXPECT_EQ(got, want) << sc.name << " " << n;
          break;
        }
        failures++;
        if (st != GTEXT_CSS_OK) {
          EXPECT_EQ(st, GTEXT_CSS_E_OOM) << sc.name << ": allocation " << n << " " << gtext_css_status_string(st);
        }
        else {
          EXPECT_EQ(got, want) << sc.name << " " << n << ": a failure that was swallowed changed the answer";
        }
        EXPECT_EQ(fa.bad_frees, 0u) << sc.name << " " << n;
      }
      EXPECT_GT(failures, 10u) << sc.name;
    }
    EXPECT_TRUE(fa.live.empty()) << sc.name << ": a block is left";
    EXPECT_EQ(fa.bad_frees, 0u) << sc.name;
  }
}

TEST(CssCustomOom, CollectingPropertyRulesSweptOverEveryAllocation) {
  const std::string sheet =
      "@property --a { syntax: '<length>'; inherits: true; initial-value: 1px }\n"
      "@layer x { @property --b { syntax: '<color> | auto'; inherits: false; initial-value: red } }\n"
      "@media all { @property --c { syntax: '<number>#'; inherits: false; initial-value: 1, 2 } }\n"
      "@property --bad { syntax: '<length>'; inherits: true }\n"
      "@property --d { syntax: '*'; inherits: false; initial-value: var(--x) }\n";  // the universal syntax keeps the rule, without its initial value
  const size_t want_count = 4;  // --a, --b, --c and --d; --bad has no initial value and is ignored
  size_t failures = 0;
  for (size_t n = 1; n < 100000; n++) {
    FaultAllocator fa;
    bool reached = false;
    {
      reg::Owned registry;
      reg::Builder b;
      ASSERT_EQ(b.freeze(registry), GTEXT_CSS_OK);
      GTEXT_CSS_At_Property_Options o = gtext_css_at_property_options_default();
      o.allocator = &fa.alloc;
      o.active = [](void *, const GTEXT_CSS_Rule *) { return true; };
      GTEXT_CSS_At_Property_Set * set = nullptr;
      GTEXT_CSS_Tree * tree = nullptr;
      ASSERT_EQ(gtext_css_parse_stylesheet(sheet.data(), sheet.size(), nullptr, &tree, nullptr), GTEXT_CSS_OK);
      fa.arm(n);
      GTEXT_CSS_Status st = gtext_css_at_property_set_new(registry.r, &o, &set);
      if (st == GTEXT_CSS_OK) {
        GTEXT_CSS_At_Property_Result res;
        st = gtext_css_at_property_collect(set, tree, &res);
      }
      fa.disarm();
      reached = fa.failed;
      if (!reached) {
        EXPECT_EQ(st, GTEXT_CSS_OK) << n;
        EXPECT_EQ(gtext_css_at_property_count(set), want_count) << n;
      }
      else {
        EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n << " " << gtext_css_status_string(st);
        failures++;
      }
      gtext_css_at_property_set_free(set);
      gtext_css_tree_free(tree);
    }
    EXPECT_TRUE(fa.live.empty()) << n << ": a block is left";
    EXPECT_EQ(fa.bad_frees, 0u) << n;
    if (!reached) {
      break;
    }
  }
  EXPECT_GT(failures, 10u);
}

TEST(CssCustomOom, MakingAValueSweptOverEveryAllocation) {
  const std::string text = "/* a */ 1px  var(--b, 2px) 'x' foo(bar, baz) url(q) #abc";
  for (size_t n = 1; n < 1000; n++) {
    FaultAllocator fa;
    GTEXT_CSS_Custom_Value * v = nullptr;
    fa.arm(n);
    const GTEXT_CSS_Status st = gtext_css_custom_value_new(&fa.alloc, text.data(), text.size(), &v);
    fa.disarm();
    if (!fa.failed) {
      ASSERT_EQ(st, GTEXT_CSS_OK);
      char * out = nullptr;
      size_t len = 0;
      ASSERT_EQ(gtext_css_custom_value_text(v, nullptr, &out, &len), GTEXT_CSS_OK);
      EXPECT_EQ(std::string(out, len), text);
      gtext_css_serialize_buffer_free(nullptr, out);
      gtext_css_custom_value_free(v);
      EXPECT_TRUE(fa.live.empty());
      break;
    }
    EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
    EXPECT_EQ(v, nullptr) << n;
    EXPECT_TRUE(fa.live.empty()) << n;
    EXPECT_EQ(fa.bad_frees, 0u) << n;
  }
}

} // namespace
