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
 * Helpers shared by the tests of computed values, shorthands, logical properties and the element interface: a registry made once, a declaration
 * block built from text, a context and a style that free themselves, and the computed value of a property as a string.
 */

#ifndef GHOTI_IO_GTEXT_TESTS_CSS_COMPUTE_TEST_SUPPORT_H
#define GHOTI_IO_GTEXT_TESTS_CSS_COMPUTE_TEST_SUPPORT_H

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <ghoti.io/text/css.h>

#include "css_registry_test_support.h"

namespace cmp {

using reg::find;

inline const GTEXT_CSS_Registry * R() {
  return reg::shared();
}

/// A declaration block built from `name: value; name: value !important`, freed with it. A declaration that does not parse is recorded in `rejected`.
struct Block {
  GTEXT_CSS_Declaration_Block * b = nullptr;
  std::vector<std::string> rejected;
  explicit Block(const GTEXT_CSS_Registry * r, const std::string & text = "", const GTEXT_CSS_Declaration_Options * o = nullptr) {
    EXPECT_EQ(gtext_css_block_new(r, o, &b), GTEXT_CSS_OK);
    size_t pos = 0;
    while (pos < text.size()) {
      size_t semi = text.find(';', pos);
      if (semi == std::string::npos) {
        semi = text.size();
      }
      std::string decl = text.substr(pos, semi - pos);
      pos = semi + 1;
      const size_t colon = decl.find(':');
      if (colon == std::string::npos) {
        continue;
      }
      std::string name = decl.substr(0, colon);
      std::string value = decl.substr(colon + 1);
      auto trim = [](std::string & s) {
        while (!s.empty() && s.front() == ' ') s.erase(s.begin());
        while (!s.empty() && s.back() == ' ') s.pop_back();
      };
      trim(name);
      trim(value);
      bool important = false;
      const size_t bang = value.find("!important");
      if (bang != std::string::npos) {
        important = true;
        value.erase(bang);
        trim(value);
      }
      GTEXT_CSS_Property_Outcome outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
      const GTEXT_CSS_Status st = gtext_css_block_add_text(b, name.data(), name.size(), value.data(), value.size(), important, nullptr, &outcome, nullptr);
      if (st != GTEXT_CSS_OK || (outcome != GTEXT_CSS_PROP_RESULT_VALID && outcome != GTEXT_CSS_PROP_RESULT_WIDE && outcome != GTEXT_CSS_PROP_RESULT_PENDING)) {
        rejected.push_back(name + ": " + value + " (" + gtext_css_status_string(st) + ")");
      }
    }
  }
  Block(const Block &) = delete;
  Block & operator=(const Block &) = delete;
  ~Block() {
    gtext_css_block_free(b);
  }
  std::string css() const {
    char * out = nullptr;
    size_t n = 0;
    if (gtext_css_block_serialize_buffer(b, &out, &n) != GTEXT_CSS_OK) {
      return "<error>";
    }
    std::string s(out, n);
    gtext_css_serialize_buffer_free(nullptr, out);
    return s;
  }
};

/// A compute context that frees itself.
struct Context {
  GTEXT_CSS_Compute_Context * c = nullptr;
  GTEXT_CSS_Compute_Result result{};
  GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;
  explicit Context(const GTEXT_CSS_Registry * r, const GTEXT_CSS_Environment * env = nullptr, const GTEXT_CSS_Compute_Options * o = nullptr) {
    status = gtext_css_compute_context_new(r, env, o, &c, &result);
  }
  Context(const Context &) = delete;
  Context & operator=(const Context &) = delete;
  ~Context() {
    gtext_css_compute_context_free(c);
  }
};

/// A style that frees itself.
struct Style {
  GTEXT_CSS_Computed_Style * s = nullptr;
  GTEXT_CSS_Compute_Result result{};
  GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;
  Style() = default;
  Style(const Style &) = delete;
  Style & operator=(const Style &) = delete;
  ~Style() {
    gtext_css_computed_free(s);
  }
  void compute(const Context & ctx, const Block * block, const Style * parent = nullptr, GTEXT_CSS_Element_Handle element = nullptr) {
    gtext_css_computed_free(s);
    s = nullptr;
    status = gtext_css_compute_style(ctx.c, block ? block->b : nullptr, parent ? parent->s : nullptr, element, &s, &result);
  }
  std::string value(const std::string & name) const {
    if (!s) {
      return "<no style>";
    }
    const GTEXT_CSS_Registry * r = R();
    const GTEXT_CSS_Property_Id id = find(r, name);
    const GTEXT_CSS_Value_Set * set = nullptr;
    const GTEXT_CSS_Status st = gtext_css_computed_value(s, id, &set);
    if (st == GTEXT_CSS_E_UNSUPPORTED) {
      return "<unsupported>";
    }
    if (st != GTEXT_CSS_OK) {
      return std::string("<") + gtext_css_status_string(st) + ">";
    }
    const GTEXT_CSS_Value * root = gtext_css_value_set_root(set);
    if (!root) {
      return "";
    }
    char * out = nullptr;
    size_t n = 0;
    if (gtext_css_serialize_value_buffer(root, nullptr, &out, &n) != GTEXT_CSS_OK) {
      return "<serialize error>";
    }
    std::string text(out, n);
    gtext_css_serialize_buffer_free(nullptr, out);
    return text;
  }
  std::string dump() const {
    std::string out;
    GTEXT_Markup_Sink sink;
    sink.user = &out;
    sink.write = [](void * u, const char * b, size_t n) -> GTEXT_Markup_Status {
      static_cast<std::string *>(u)->append(b, n);
      return GTEXT_MARKUP_OK;
    };
    gtext_css_computed_dump(s, &sink);
    return out;
  }
};

/// The computed value of one property of an element declared `decls`, with an optional parent's declarations: the whole matrix in one call.
inline std::string computed(const std::string & parent_decls, const std::string & decls, const std::string & property,
    const GTEXT_CSS_Environment * env = nullptr) {
  Context ctx(R(), env);
  if (ctx.status != GTEXT_CSS_OK) {
    return std::string("<context: ") + gtext_css_status_string(ctx.status) + ">";
  }
  Block pb(R(), parent_decls), cb(R(), decls);
  Style parent, child;
  parent.compute(ctx, &pb);
  if (parent.status != GTEXT_CSS_OK) {
    return std::string("<parent: ") + gtext_css_status_string(parent.status) + " " + parent.result.name + ">";
  }
  child.compute(ctx, &cb, &parent);
  if (child.status != GTEXT_CSS_OK) {
    return std::string("<child: ") + gtext_css_status_string(child.status) + " " + child.result.name + ">";
  }
  return child.value(property);
}

/// The same for a root element (no parent).
inline std::string root_computed(const std::string & decls, const std::string & property, const GTEXT_CSS_Environment * env = nullptr) {
  Context ctx(R(), env);
  if (ctx.status != GTEXT_CSS_OK) {
    return std::string("<context: ") + gtext_css_status_string(ctx.status) + ">";
  }
  Block cb(R(), decls);
  Style child;
  child.compute(ctx, &cb);
  if (child.status != GTEXT_CSS_OK) {
    return std::string("<") + gtext_css_status_string(child.status) + " " + child.result.name + ">";
  }
  return child.value(property);
}

} // namespace cmp

#endif // GHOTI_IO_GTEXT_TESTS_CSS_COMPUTE_TEST_SUPPORT_H
