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
 * Helpers shared by the cascade tests: keys written as a chain (`K().important().layer(a).spec(0, 1, 0)`), a layer registry that freezes, a candidate
 * set that takes `name: value` text, and the answer of a cascade as strings.
 */

#ifndef GHOTI_IO_GTEXT_TESTS_CSS_CASCADE_TEST_SUPPORT_H
#define GHOTI_IO_GTEXT_TESTS_CSS_CASCADE_TEST_SUPPORT_H

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_compute_test_support.h"

namespace cas {

using cmp::R;
using reg::find;

/// The keys of a declaration, written as a chain. The default is a normal author declaration with no layer, specificity zero, order 0.0.0.
struct K {
  GTEXT_CSS_Cascade_Entry e;
  K() {
    std::memset(&e, 0, sizeof e);
    e.origin = GTEXT_CSS_ORIGIN_AUTHOR;
  }
  K & origin(GTEXT_CSS_Origin o) {
    e.origin = o;
    return *this;
  }
  K & ua() { return origin(GTEXT_CSS_ORIGIN_USER_AGENT); }
  K & user() { return origin(GTEXT_CSS_ORIGIN_USER); }
  K & author() { return origin(GTEXT_CSS_ORIGIN_AUTHOR); }
  K & animation() { return origin(GTEXT_CSS_ORIGIN_ANIMATION); }
  K & transition() { return origin(GTEXT_CSS_ORIGIN_TRANSITION); }
  K & important(bool v = true) {
    e.important = v;
    return *this;
  }
  K & hint() {
    e.presentational_hint = true;
    return *this;
  }
  K & attached() {
    e.element_attached = true;
    return *this;
  }
  K & layer(GTEXT_CSS_Layer_Id l) {
    e.layer = l;
    return *this;
  }
  K & ctx(uint32_t c) {
    e.context = c;
    return *this;
  }
  K & spec(uint32_t a, uint32_t b, uint32_t c) {
    e.specificity.a = a;
    e.specificity.b = b;
    e.specificity.c = c;
    return *this;
  }
  K & scope(uint32_t distance) {
    e.scoped = true;
    e.scope_distance = distance;
    return *this;
  }
  K & order(uint32_t sheet, uint32_t rule = 0, uint32_t decl = 0) {
    e.sheet = sheet;
    e.rule = rule;
    e.declaration = decl;
    return *this;
  }
  /// The rule id (provenance), which the tests use to name a declaration.
  K & id(uint64_t rule_id) {
    e.rule_id = rule_id;
    return *this;
  }
};

/// A layer registry, frozen on first use by a candidate set.
struct Layers {
  GTEXT_CSS_Layer_Registry * r = nullptr;
  Layers(const GTEXT_CSS_Layer_Options * o = nullptr) {
    EXPECT_EQ(gtext_css_layer_registry_new(o, &r), GTEXT_CSS_OK);
  }
  Layers(const Layers &) = delete;
  Layers & operator=(const Layers &) = delete;
  ~Layers() {
    gtext_css_layer_registry_free(r);
  }
  GTEXT_CSS_Layer_Id declare(const std::string & path, GTEXT_CSS_Origin origin = GTEXT_CSS_ORIGIN_AUTHOR, GTEXT_CSS_Layer_Id parent = 0) {
    GTEXT_CSS_Layer_Id id = 0;
    EXPECT_EQ(gtext_css_layer_declare(r, origin, parent, path.data(), path.size(), &id, nullptr), GTEXT_CSS_OK) << path;
    return id;
  }
  const GTEXT_CSS_Layer_Registry * frozen() {
    EXPECT_EQ(gtext_css_layer_freeze(r), GTEXT_CSS_OK);
    return r;
  }
};

/// A candidate set that takes `name: value` and frees itself.
struct Set {
  GTEXT_CSS_Candidates * s = nullptr;
  GTEXT_CSS_Cascade_Result last{};
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  explicit Set(const GTEXT_CSS_Layer_Registry * layers = nullptr, const GTEXT_CSS_Candidate_Options * o = nullptr, const GTEXT_CSS_Registry * r = R()) {
    EXPECT_EQ(gtext_css_candidates_new(r, layers, o, &s), GTEXT_CSS_OK);
  }
  Set(const Set &) = delete;
  Set & operator=(const Set &) = delete;
  ~Set() {
    gtext_css_candidates_free(s);
  }
  /// Add a declaration; the keys' `important` is used unless the text ends `!important`.
  Set & add(const std::string & name, const std::string & value, K keys = K()) {
    std::string v = value;
    const size_t bang = v.find("!important");
    if (bang != std::string::npos) {
      keys.e.important = true;
      v.erase(bang);
      while (!v.empty() && v.back() == ' ') {
        v.pop_back();
      }
    }
    GTEXT_CSS_Property_Outcome outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
    status = gtext_css_candidates_add_text(s, &keys.e, name.data(), name.size(), v.data(), v.size(), nullptr, &outcome, &last);
    return *this;
  }
};

/// A cascade that frees itself.
struct Out {
  GTEXT_CSS_Cascaded * c = nullptr;
  GTEXT_CSS_Cascade_Result result{};
  GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;
  Out() = default;
  Out(const Out &) = delete;
  Out & operator=(const Out &) = delete;
  ~Out() {
    gtext_css_cascaded_free(c);
  }
  void run(Set & set, const GTEXT_CSS_Cascade_Options * o = nullptr) {
    gtext_css_cascaded_free(c);
    c = nullptr;
    status = gtext_css_cascade(set.s, o, &c, &result);
  }
  Out(Set & set, const GTEXT_CSS_Cascade_Options * o = nullptr) {
    run(set, o);
  }
  /// The serialization of the winning value of a property ("-" for no winner; "pending:..." for a pending one; the keyword of a CSS-wide one).
  std::string value(const std::string & property) const {
    GTEXT_CSS_Cascade_Entry e;
    if (!gtext_css_cascaded_find(c, find(R(), property), nullptr, 0, &e)) {
      return "-";
    }
    return text(e);
  }
  std::string custom(const std::string & name) const {
    GTEXT_CSS_Cascade_Entry e;
    if (!gtext_css_cascaded_find(c, 0, name.data(), name.size(), &e)) {
      return "-";
    }
    return text(e);
  }
  static std::string text(const GTEXT_CSS_Cascade_Entry & e) {
    if (e.outcome == GTEXT_CSS_PROP_RESULT_WIDE) {
      return gtext_css_wide_keyword_name(e.wide);
    }
    const GTEXT_CSS_Value * root = gtext_css_value_set_root(e.set);
    if (!root) {
      return "";
    }
    if (e.outcome == GTEXT_CSS_PROP_RESULT_PENDING || e.custom_name.data) {
      return std::string(e.outcome == GTEXT_CSS_PROP_RESULT_PENDING ? "pending:" : "") + std::string(root->text.data ? root->text.data : "", root->text.length);
    }
    char * out = nullptr;
    size_t n = 0;
    if (gtext_css_serialize_value_buffer(root, nullptr, &out, &n) != GTEXT_CSS_OK) {
      return "<serialize error>";
    }
    std::string s(out, n);
    gtext_css_serialize_buffer_free(nullptr, out);
    return s;
  }
  /// The rule id of the declaration that won a property (0 for none).
  uint64_t rule_of(const std::string & property) const {
    GTEXT_CSS_Cascade_Entry e;
    if (!gtext_css_cascaded_find(c, find(R(), property), nullptr, 0, &e)) {
      return 0;
    }
    return e.rule_id;
  }
  std::string dump() const {
    std::string out;
    GTEXT_Markup_Sink sink;
    sink.user = &out;
    sink.write = [](void * u, const char * b, size_t n) -> GTEXT_Markup_Status {
      static_cast<std::string *>(u)->append(b, n);
      return GTEXT_MARKUP_OK;
    };
    gtext_css_cascaded_dump(c, &sink);
    return out;
  }
};

} // namespace cas

#endif // GHOTI_IO_GTEXT_TESTS_CSS_CASCADE_TEST_SUPPORT_H
