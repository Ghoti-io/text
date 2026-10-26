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
 * Allocation failure in the registry and the property layer: making a registry, registering (a property, an alias, a shorthand),
 * freezing, compiling a grammar lazily, parsing a property, and writing the result, each with the n-th allocation failed for
 * every n up to the last one the call makes.
 *
 * A failed call is GTEXT_CSS_E_OOM and leaves nothing behind: the allocator holds no block that the call should have freed, and
 * was never asked to free one it did not make. A grammar whose compile ran out of memory is not remembered as a bad one: the
 * same call with memory succeeds.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "css_registry_test_support.h"
#include "markup_test_support.h"

using namespace reg;

namespace {

GTEXT_CSS_Property_Def def(const char * name, const char * grammar, const char * initial) {
  GTEXT_CSS_Property_Def d;
  std::memset(&d, 0, sizeof d);
  d.name = name;
  d.grammar = grammar;
  d.initial = initial;
  return d;
}

GTEXT_CSS_Registry_Options options_with(FaultAllocator & fa) {
  GTEXT_CSS_Registry_Options o = gtext_css_registry_options_default();
  o.allocator = &fa.alloc;
  return o;
}

TEST(CssRegistryOom, MakingARegistrySweptOverEveryAllocation) {
  size_t failures = 0;
  for (size_t n = 1; n < 400; n++) {
    FaultAllocator fa;
    GTEXT_CSS_Registry_Options o = options_with(fa);
    GTEXT_CSS_Registry * r = nullptr;
    fa.arm(n);
    const GTEXT_CSS_Status st = gtext_css_registry_new(&o, &r);
    fa.disarm();
    if (!fa.failed) {
      EXPECT_EQ(st, GTEXT_CSS_OK);
      gtext_css_registry_free(r);
      EXPECT_TRUE(fa.live.empty());
      EXPECT_EQ(fa.bad_frees, 0u);
      break;
    }
    failures++;
    EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
    EXPECT_EQ(r, nullptr) << n;
    EXPECT_TRUE(fa.live.empty()) << n << ": a block is left";
    EXPECT_EQ(fa.bad_frees, 0u) << n;
  }
  EXPECT_GE(failures, 8u);
}

TEST(CssRegistryOom, RegisteringAPropertySweptOverEveryAllocation) {
  size_t failures = 0;
  for (size_t n = 1; n < 6000; n++) {
    FaultAllocator fa;
    GTEXT_CSS_Registry_Options o = options_with(fa);
    GTEXT_CSS_Registry_Builder * b = nullptr;
    ASSERT_EQ(gtext_css_registry_builder_new(&o, &b), GTEXT_CSS_OK);
    GTEXT_CSS_Property_Def d = def("vector-gap", "<'margin-top'> | [ <length-percentage [0,∞]> | auto ]{2} | calc-size( <number> )", "5px");
    d.override = false;
    GTEXT_CSS_Registry_Result res;
    fa.arm(n);
    const GTEXT_CSS_Status st = gtext_css_registry_register_property(b, &d, &res);
    fa.disarm();
    if (!fa.failed) {
      EXPECT_EQ(st, GTEXT_CSS_OK);
      GTEXT_CSS_Registry * r = nullptr;
      EXPECT_EQ(gtext_css_registry_freeze(b, &r, nullptr), GTEXT_CSS_OK);
      gtext_css_registry_free(r);
      EXPECT_TRUE(fa.live.empty());
      break;
    }
    failures++;
    EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n << " " << gtext_css_status_string(st);
    EXPECT_EQ(res.status, GTEXT_CSS_E_OOM) << n;
    // nothing was registered, and the same registration with memory succeeds
    GTEXT_CSS_Registry * r = nullptr;
    ASSERT_EQ(gtext_css_registry_register_property(b, &d, &res), GTEXT_CSS_OK) << n;
    ASSERT_EQ(gtext_css_registry_freeze(b, &r, nullptr), GTEXT_CSS_OK) << n;
    EXPECT_EQ(gtext_css_registry_property_count(r), gtext_css_registry_generated_property_count() + 1) << n;
    gtext_css_registry_free(r);
    EXPECT_TRUE(fa.live.empty()) << n << ": a block is left";
    EXPECT_EQ(fa.bad_frees, 0u) << n;
  }
  EXPECT_GT(failures, 20u);
}

TEST(CssRegistryOom, RegisteringAnAliasAndAShorthandAndOverridingSweptOverEveryAllocation) {
  const char * lh[] = {"width", "height"};
  for (int kind = 0; kind < 3; kind++) {
    size_t failures = 0;
    for (size_t n = 1; n < 3000; n++) {
      FaultAllocator fa;
      GTEXT_CSS_Registry_Options o = options_with(fa);
      GTEXT_CSS_Registry_Builder * b = nullptr;
      ASSERT_EQ(gtext_css_registry_builder_new(&o, &b), GTEXT_CSS_OK);
      GTEXT_CSS_Registry_Result res;
      GTEXT_CSS_Shorthand_Def sh;
      std::memset(&sh, 0, sizeof sh);
      sh.name = "vector-size";
      sh.grammar = "<'width'> <'height'>?";
      sh.longhands = lh;
      sh.longhand_count = 2;
      GTEXT_CSS_Property_Def ov = def("color", "red | blue", "red");
      ov.override = true;
      const auto call = [&]() {
        return kind == 0 ? gtext_css_registry_register_alias(b, "vector-w", "width", false, &res)
             : kind == 1 ? gtext_css_registry_register_shorthand(b, &sh, &res)
                         : gtext_css_registry_register_property(b, &ov, &res);
      };
      fa.arm(n);
      const GTEXT_CSS_Status st = call();
      fa.disarm();
      if (!fa.failed) {
        EXPECT_EQ(st, GTEXT_CSS_OK);
        GTEXT_CSS_Registry * r = nullptr;
        EXPECT_EQ(gtext_css_registry_freeze(b, &r, nullptr), GTEXT_CSS_OK);
        gtext_css_registry_free(r);
        EXPECT_TRUE(fa.live.empty());
        break;
      }
      failures++;
      EXPECT_EQ(st, GTEXT_CSS_E_OOM) << "kind " << kind << " n " << n << " " << gtext_css_status_string(st);
      ASSERT_EQ(call(), GTEXT_CSS_OK) << kind << " " << n;
      GTEXT_CSS_Registry * r = nullptr;
      ASSERT_EQ(gtext_css_registry_freeze(b, &r, nullptr), GTEXT_CSS_OK);
      EXPECT_EQ(gtext_css_registry_override_count(r), kind == 2 ? 1u : 0u) << "an out-of-memory registration left no override behind";
      gtext_css_registry_free(r);
      EXPECT_TRUE(fa.live.empty()) << kind << " " << n << ": a block is left";
      EXPECT_EQ(fa.bad_frees, 0u);
    }
    EXPECT_GT(failures, kind == 0 ? 1u : 5u) << kind;
  }
}

TEST(CssRegistryOom, TheAllocationsOfRegistrationAreBalancedWithNoFailureAtAll) {
  // The name table grows as registrations arrive (the old table is freed), a refused registration (a name that exists) gives back
  // what it made, an override is kept in a list that grows, and the registry gives back everything it took. Whatever the allocator
  // does not see freed is a leak; whatever it is asked to free that it did not make is a bad free.
  FaultAllocator fa;
  {
    GTEXT_CSS_Registry_Options o = options_with(fa);
    GTEXT_CSS_Registry_Builder * b = nullptr;
    ASSERT_EQ(gtext_css_registry_builder_new(&o, &b), GTEXT_CSS_OK);
    GTEXT_CSS_Registry_Result res;
    std::vector<std::string> names;
    for (int i = 0; i < 400; i++) {
      names.push_back("vector-n" + std::to_string(i));
      GTEXT_CSS_Property_Def d = def(names.back().c_str(), "a | b", "a");
      ASSERT_EQ(gtext_css_registry_register_property(b, &d, &res), GTEXT_CSS_OK) << i;
      EXPECT_EQ(gtext_css_registry_register_property(b, &d, &res), GTEXT_CSS_E_EXISTS) << i;
      if (i % 25 == 0) {
        d.override = true;
        d.grammar = "a | b | c";
        ASSERT_EQ(gtext_css_registry_register_property(b, &d, &res), GTEXT_CSS_OK) << i;
      }
    }
    GTEXT_CSS_Property_Def w = def("width", "x", "x");
    EXPECT_EQ(gtext_css_registry_register_property(b, &w, &res), GTEXT_CSS_E_EXISTS);
    EXPECT_EQ(gtext_css_registry_register_alias(b, "vector-n3", "width", false, &res), GTEXT_CSS_E_EXISTS);
    const char * lh[] = {"width"};
    GTEXT_CSS_Shorthand_Def sh;
    std::memset(&sh, 0, sizeof sh);
    sh.name = "vector-n4";
    sh.grammar = "<'width'>";
    sh.longhands = lh;
    sh.longhand_count = 1;
    EXPECT_EQ(gtext_css_registry_register_shorthand(b, &sh, &res), GTEXT_CSS_E_EXISTS);
    GTEXT_CSS_Registry * r = nullptr;
    ASSERT_EQ(gtext_css_registry_freeze(b, &r, &res), GTEXT_CSS_OK);
    EXPECT_EQ(gtext_css_registry_override_count(r), 16u);
    gtext_css_registry_free(r);
  }
  EXPECT_TRUE(fa.live.empty()) << fa.live.size() << " blocks are left";
  EXPECT_EQ(fa.bad_frees, 0u);
}

TEST(CssRegistryOom, AFailedFreezeAndABuilderFreedUnfrozenGiveEverythingBack) {
  FaultAllocator fa;
  {
    GTEXT_CSS_Registry_Options o = options_with(fa);
    GTEXT_CSS_Registry_Builder * b = nullptr;
    ASSERT_EQ(gtext_css_registry_builder_new(&o, &b), GTEXT_CSS_OK);
    GTEXT_CSS_Property_Def w = def("width", "<'flex'> | zzy", "auto");
    w.override = true;
    ASSERT_EQ(gtext_css_registry_register_property(b, &w, nullptr), GTEXT_CSS_OK);
    GTEXT_CSS_Registry * r = nullptr;
    GTEXT_CSS_Registry_Result res;
    EXPECT_EQ(gtext_css_registry_freeze(b, &r, &res), GTEXT_CSS_E_LEFT_RECURSION);
    EXPECT_EQ(gtext_css_registry_freeze(b, &r, &res), GTEXT_CSS_E_LEFT_RECURSION) << "and again, the same";
    gtext_css_registry_builder_free(b);
  }
  EXPECT_TRUE(fa.live.empty()) << fa.live.size() << " blocks are left";
  EXPECT_EQ(fa.bad_frees, 0u);
}

TEST(CssRegistryOom, FreezingSweptOverEveryAllocation) {
  size_t failures = 0;
  for (size_t n = 1; n < 20000; n++) {
    FaultAllocator fa;
    GTEXT_CSS_Registry_Options o = options_with(fa);
    GTEXT_CSS_Registry_Builder * b = nullptr;
    ASSERT_EQ(gtext_css_registry_builder_new(&o, &b), GTEXT_CSS_OK);
    GTEXT_CSS_Property_Def d = def("vector-gap", "<'margin-top'> | [ <length-percentage [0,∞]> | auto ]{2}", "5px");
    ASSERT_EQ(gtext_css_registry_register_property(b, &d, nullptr), GTEXT_CSS_OK);
    GTEXT_CSS_Registry * r = nullptr;
    GTEXT_CSS_Registry_Result res;
    fa.arm(n);
    const GTEXT_CSS_Status st = gtext_css_registry_freeze(b, &r, &res);
    fa.disarm();
    if (!fa.failed) {
      EXPECT_EQ(st, GTEXT_CSS_OK);
      gtext_css_registry_free(r);
      EXPECT_TRUE(fa.live.empty());
      break;
    }
    failures++;
    EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n << " " << gtext_css_status_string(st);
    EXPECT_EQ(r, nullptr);
    // the builder is still a builder after a failed freeze, and freezes with memory
    ASSERT_EQ(gtext_css_registry_freeze(b, &r, nullptr), GTEXT_CSS_OK) << n;
    EXPECT_EQ(parse(r, "vector-gap", "5px"), "valid:5px") << n;
    gtext_css_registry_free(r);
    EXPECT_TRUE(fa.live.empty()) << n << ": a block is left";
    EXPECT_EQ(fa.bad_frees, 0u);
  }
  EXPECT_GT(failures, 10u);
}

TEST(CssRegistryOom, CompilingAGrammarLazilySweptOverEveryAllocationAndNotRememberedAsBad) {
  for (const char * property : {"margin", "color", "font"}) {
    size_t failures = 0;
    for (size_t n = 1; n < 40000; n++) {
      FaultAllocator fa;
      GTEXT_CSS_Registry_Options o = options_with(fa);
      GTEXT_CSS_Registry * r = nullptr;
      ASSERT_EQ(gtext_css_registry_new(&o, &r), GTEXT_CSS_OK);
      GTEXT_CSS_Grammar_State s = GTEXT_CSS_GRAMMAR_NOT_COMPILED;
      fa.arm(n);
      const GTEXT_CSS_Status st = gtext_css_registry_property_state(r, find(r, property), &s, nullptr);
      fa.disarm();
      if (!fa.failed) {
        EXPECT_EQ(st, GTEXT_CSS_OK);
        EXPECT_EQ(s, GTEXT_CSS_GRAMMAR_OK);
        gtext_css_registry_free(r);
        EXPECT_TRUE(fa.live.empty());
        break;
      }
      failures++;
      EXPECT_EQ(st, GTEXT_CSS_E_OOM) << property << " " << n << " " << gtext_css_status_string(st);
      // asking again, with memory, compiles it: the failure was not cached as the grammar being bad
      ASSERT_EQ(gtext_css_registry_property_state(r, find(r, property), &s, nullptr), GTEXT_CSS_OK) << property << " " << n;
      EXPECT_EQ(s, GTEXT_CSS_GRAMMAR_OK) << property << " " << n;
      gtext_css_registry_free(r);
      EXPECT_TRUE(fa.live.empty()) << property << " " << n << ": a block is left";
      EXPECT_EQ(fa.bad_frees, 0u);
    }
    EXPECT_GT(failures, 10u) << property;
  }
}

TEST(CssRegistryOom, ParsingAPropertySweptOverEveryAllocation) {
  const std::vector<std::pair<const char *, const char *>> cases = {
      {"margin", "1px 2px"}, {"color", "rgb(1 2 3 / 50%)"}, {"transform", "translate(1px) rotate(2deg)"}, {"margin", "var(--m)"}, {"all", "inherit"}};
  for (const auto & c : cases) {
    // (the registry is made with the real allocator and prepared, so that the sweep is of the parse alone)
    GTEXT_CSS_Registry * r = nullptr;
    ASSERT_EQ(gtext_css_registry_new(nullptr, &r), GTEXT_CSS_OK);
    ASSERT_EQ(gtext_css_registry_prepare(r), GTEXT_CSS_OK);
    GTEXT_CSS_Property_Value want;
    ASSERT_EQ(gtext_css_parse_property_text(r, c.first, strlen(c.first), c.second, strlen(c.second), nullptr, &want), GTEXT_CSS_OK);
    char * wb = nullptr;
    size_t wn = 0;
    ASSERT_EQ(gtext_css_property_value_serialize_buffer(&want, nullptr, &wb, &wn), GTEXT_CSS_OK);
    const std::string expected(wb, wn);
    gtext_css_serialize_buffer_free(nullptr, wb);
    gtext_css_property_value_free(&want);
    size_t failures = 0;
    for (size_t n = 1; n < 20000; n++) {
      FaultAllocator fa;
      GTEXT_CSS_Property_Options po;
      std::memset(&po, 0, sizeof po);
      po.allocator = &fa.alloc;
      GTEXT_CSS_Property_Value pv;
      fa.arm(n);
      const GTEXT_CSS_Status st = gtext_css_parse_property_text(r, c.first, strlen(c.first), c.second, strlen(c.second), &po, &pv);
      fa.disarm();
      if (!fa.failed) {
        EXPECT_EQ(st, GTEXT_CSS_OK);
        GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
        so.allocator = &fa.alloc;
        char * buf = nullptr;
        size_t bn = 0;
        EXPECT_EQ(gtext_css_property_value_serialize_buffer(&pv, &so, &buf, &bn), GTEXT_CSS_OK);
        EXPECT_EQ(std::string(buf, bn), expected);
        gtext_css_serialize_buffer_free(&so, buf);
        gtext_css_property_value_free(&pv);
        EXPECT_TRUE(fa.live.empty());
        break;
      }
      failures++;
      EXPECT_EQ(st, GTEXT_CSS_E_OOM) << c.first << " " << n << " " << gtext_css_status_string(st);
      EXPECT_EQ(pv.set, nullptr) << n;
      EXPECT_TRUE(fa.live.empty()) << c.first << " " << n << ": a block is left";
      EXPECT_EQ(fa.bad_frees, 0u);
    }
    EXPECT_GT(failures, 3u) << c.first;
    gtext_css_registry_free(r);
  }
}

TEST(CssRegistryOom, WritingAResultSweptOverEveryAllocation) {
  const GTEXT_CSS_Registry * r = shared();
  for (const char * value : {"1px 2px", "inherit"}) {
    GTEXT_CSS_Property_Value pv;
    ASSERT_EQ(gtext_css_parse_property_text(r, "margin", 6, value, strlen(value), nullptr, &pv), GTEXT_CSS_OK);
    size_t failures = 0;
    for (size_t n = 1; n < 200; n++) {
      FaultAllocator fa;
      GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
      so.allocator = &fa.alloc;
      char * buf = nullptr;
      size_t bn = 0;
      fa.arm(n);
      const GTEXT_CSS_Status st = gtext_css_property_value_serialize_buffer(&pv, &so, &buf, &bn);
      fa.disarm();
      if (!fa.failed) {
        EXPECT_EQ(st, GTEXT_CSS_OK);
        gtext_css_serialize_buffer_free(&so, buf);
        EXPECT_TRUE(fa.live.empty());
        break;
      }
      failures++;
      EXPECT_EQ(st, GTEXT_CSS_E_OOM) << n;
      EXPECT_TRUE(fa.live.empty()) << n;
    }
    EXPECT_GE(failures, 1u);
    gtext_css_property_value_free(&pv);
  }
}

TEST(CssRegistryOom, WritingTheCanonicalFormOfAResultSweptOverEveryAllocation) {
  // the canonical-form rules (src/css/css_property_canon.c) rewrite the text through buffers of their own: every one of them runs out of memory
  // cleanly, in the buffer form and the sink form, whichever allocation it is
  const GTEXT_CSS_Registry * r = shared();
  struct Case {
    const char * property;
    const char * value;
  };
  for (const Case & c : {Case{"display", "block flow"}, Case{"filter", "grayscale(300%) hue-rotate(0)"}, Case{"clip-path", "path(nonzero, \"M20,20h60z\")"},
           Case{"animation-name", "\"multi word\", b"}, Case{"counter-reset", "a b 3"}, Case{"rotate", "0 1 0 400grad"}, Case{"scale", "100% 200% 1"},
           Case{"mask-position", "top, left"}, Case{"grid-template-columns", "[] 1fr []"}, Case{"cursor", "image-set(\"a\" 1x), grab"},
           Case{"scale", "2 3"}, Case{"rotate", "0 0 -1 10deg"}, Case{"clip-path", "path(\"M0 0a1 1 0 00.5.5z\")"}, Case{"opacity", "12.5%"},
           Case{"animation-name", "foo\\ , bar"}}) {
    GTEXT_CSS_Property_Value pv;
    ASSERT_EQ(gtext_css_parse_property_text(r, c.property, strlen(c.property), c.value, strlen(c.value), nullptr, &pv), GTEXT_CSS_OK) << c.property;
    ASSERT_EQ(pv.outcome, GTEXT_CSS_PROP_RESULT_VALID) << c.property;
    ASSERT_NE(pv.canonical, 0) << c.property << ": the property has a rule set";
    size_t failures = 0;
    for (int sink_form = 0; sink_form < 2; sink_form++) {
      for (size_t n = 1; n < 300; n++) {
        FaultAllocator fa;
        GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
        so.allocator = &fa.alloc;
        char * buf = nullptr;
        size_t bn = 0;
        std::string via_sink;
        GTEXT_Markup_Sink sink;
        sink.user = &via_sink;
        sink.write = [](void * user, const char * bytes, size_t len) {
          static_cast<std::string *>(user)->append(bytes, len);
          return GTEXT_MARKUP_OK;
        };
        fa.arm(n);
        const GTEXT_CSS_Status st = sink_form ? gtext_css_property_value_serialize(&pv, &so, &sink)
                                              : gtext_css_property_value_serialize_buffer(&pv, &so, &buf, &bn);
        fa.disarm();
        if (!fa.failed) {
          EXPECT_EQ(st, GTEXT_CSS_OK);
          gtext_css_serialize_buffer_free(&so, buf);
          EXPECT_TRUE(fa.live.empty()) << c.property;
          break;
        }
        failures++;
        EXPECT_EQ(st, GTEXT_CSS_E_OOM) << c.property << ": " << n;
        EXPECT_TRUE(fa.live.empty()) << c.property << ": " << n << ": a block is left";
        EXPECT_EQ(fa.bad_frees, 0u) << c.property << ": " << n;
      }
    }
    EXPECT_GE(failures, 2u) << c.property;
    gtext_css_property_value_free(&pv);
  }
}

TEST(CssRegistryOom, AValidityCheckThatRunsOutOfMemoryRecordsItAndKeepsNothing) {
  GTEXT_CSS_Registry * r = nullptr;
  ASSERT_EQ(gtext_css_registry_new(nullptr, &r), GTEXT_CSS_OK);
  ASSERT_EQ(gtext_css_registry_prepare(r), GTEXT_CSS_OK);
  size_t failures = 0;
  for (size_t n = 1; n < 20000; n++) {
    FaultAllocator fa;
    GTEXT_CSS_Property_Options po;
    std::memset(&po, 0, sizeof po);
    po.allocator = &fa.alloc;
    GTEXT_CSS_Property_Validity v;
    ASSERT_EQ(gtext_css_property_validity_init(&v, r, &po), GTEXT_CSS_OK);
    GTEXT_CSS_Parse_Options pa = gtext_css_parse_options_default();
    pa.validity = gtext_css_property_validity(&v);
    GTEXT_CSS_Tree * tree = nullptr;
    const std::string css = "margin: 1px 2px; color: var(--c); width: 5px";
    // only the validity checks allocate through the fault allocator (the parse itself uses the default)
    fa.arm(n);
    const GTEXT_CSS_Status st = gtext_css_parse_declaration_list(css.data(), css.size(), &pa, &tree, nullptr);
    fa.disarm();
    EXPECT_EQ(st, GTEXT_CSS_OK);
    if (!fa.failed) {
      gtext_css_tree_free(tree);
      EXPECT_TRUE(fa.live.empty());
      break;
    }
    failures++;
    EXPECT_EQ(v.status, GTEXT_CSS_E_OOM) << n;
    EXPECT_GE(v.rejected, 1u);
    gtext_css_tree_free(tree);
    EXPECT_TRUE(fa.live.empty()) << n << ": a block is left";
  }
  EXPECT_GT(failures, 3u);
  gtext_css_registry_free(r);
}

}  // namespace
