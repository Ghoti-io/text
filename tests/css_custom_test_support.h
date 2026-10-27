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
 * Helpers shared by the tests of custom properties, substitution and `@property`: a small tree to answer `attr()` and `sibling-index()`, an
 * environment with `env()` variables and a condition callback, and a world (registry, rules, context) that computes the style of an element from
 * declarations written as text and reads back custom properties and ordinary ones as strings.
 */

#ifndef GHOTI_IO_GTEXT_TESTS_CSS_CUSTOM_TEST_SUPPORT_H
#define GHOTI_IO_GTEXT_TESTS_CSS_CUSTOM_TEST_SUPPORT_H

#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_compute_test_support.h"

namespace cust {

/// An element of a test tree: attributes and the place among its siblings.
struct Node {
  std::map<std::string, std::string> attrs;
  size_t index = 1, count = 1;
  const Node * parent = nullptr;
};

inline const Node * as_node(GTEXT_CSS_Element_Handle h) {
  return static_cast<const Node *>(h);
}

inline GTEXT_CSS_Element_Interface node_interface() {
  GTEXT_CSS_Element_Interface t;
  std::memset(&t, 0, sizeof t);
  t.size = sizeof t;
  t.parent = [](void *, GTEXT_CSS_Element_Handle e) -> GTEXT_CSS_Element_Handle { return as_node(e)->parent; };
  t.attribute = [](void *, GTEXT_CSS_Element_Handle e, const GTEXT_CSS_String * ns, const GTEXT_CSS_String * name, GTEXT_CSS_String * out) -> bool {
    std::string key = std::string(ns && ns->data ? std::string(ns->data, ns->length) + "|" : "") + std::string(name->data, name->length);
    const Node * n = as_node(e);
    auto it = n->attrs.find(key);
    if (it == n->attrs.end()) {
      return false;
    }
    out->data = it->second.c_str();
    out->length = it->second.size();
    return true;
  };
  t.sibling_index = [](void *, GTEXT_CSS_Element_Handle e) -> size_t { return as_node(e)->index; };
  t.sibling_count = [](void *, GTEXT_CSS_Element_Handle e) -> size_t { return as_node(e)->count; };
  return t;
}

/// A custom property to register, as the caller would.
struct Reg {
  std::string name, syntax, initial;
  bool inherits = true;
  bool has_initial = true;
  bool no_syntax = false;
};

/// A registry with registrations (and what they were refused with), a context, and the stylesheet's `@property` rules.
class World {
 public:
  std::map<std::string, std::string> env_vars;
  std::function<GTEXT_CSS_Tristate(GTEXT_CSS_Condition_Kind, const std::string &)> condition;
  std::function<bool(const GTEXT_CSS_Random_Key *, double *)> random_base;
  reg::Owned registry;
  GTEXT_CSS_At_Property_Set * rules = nullptr;
  GTEXT_CSS_Compute_Context * ctx = nullptr;
  GTEXT_CSS_Status ctx_status = GTEXT_CSS_E_INTERNAL;
  GTEXT_CSS_Compute_Result ctx_result{};
  std::vector<std::pair<std::string, GTEXT_CSS_Status>> refused;
  std::vector<std::string> refused_detail;
  GTEXT_CSS_Environment env = gtext_css_environment_default();
  GTEXT_CSS_Compute_Options options = gtext_css_compute_options_default();
  GTEXT_CSS_Element_Interface iface = node_interface();
  bool use_iface = true;

  World() = default;
  World(const World &) = delete;
  World & operator=(const World &) = delete;
  ~World() {
    gtext_css_compute_context_free(ctx);
    gtext_css_at_property_set_free(rules);
  }

  /// Register custom properties, freeze, add the stylesheets' rules, make the context. Call once.
  GTEXT_CSS_Status build(const std::vector<Reg> & regs = {}, const std::vector<std::string> & sheets = {}) {
    reg::Builder b;
    for (const Reg & r : regs) {
      GTEXT_CSS_Custom_Def d;
      std::memset(&d, 0, sizeof d);
      d.name = r.name.c_str();
      d.syntax = r.no_syntax ? nullptr : r.syntax.c_str();
      d.inherits = r.inherits;
      d.initial_value = r.has_initial ? r.initial.c_str() : nullptr;
      GTEXT_CSS_Registry_Result rr;
      const GTEXT_CSS_Status st = gtext_css_registry_register_custom(b.b, &d, &rr);
      if (st != GTEXT_CSS_OK) {
        refused.emplace_back(r.name, st);
        refused_detail.push_back(rr.detail ? rr.detail : "");
      }
    }
    GTEXT_CSS_Status st = b.freeze(registry);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    if (!sheets.empty()) {
      st = gtext_css_at_property_set_new(registry.r, nullptr, &rules);
      if (st != GTEXT_CSS_OK) {
        return st;
      }
      for (const std::string & css : sheets) {
        GTEXT_CSS_Tree * tree = nullptr;
        st = gtext_css_parse_stylesheet(css.data(), css.size(), nullptr, &tree, nullptr);
        if (st != GTEXT_CSS_OK) {
          return st;
        }
        GTEXT_CSS_At_Property_Result res;
        st = gtext_css_at_property_collect(rules, tree, &res);
        gtext_css_tree_free(tree);
        if (st != GTEXT_CSS_OK) {
          return st;
        }
      }
    }
    env.user = this;
    env.env_variable = [](void * u, const GTEXT_CSS_String * name, const uint32_t * idx, size_t n, GTEXT_CSS_String * out) -> bool {
      World * w = static_cast<World *>(u);
      std::string key(name->data, name->length);
      for (size_t i = 0; i < n; i++) {
        key += "," + std::to_string(idx[i]);
      }
      auto it = w->env_vars.find(key);
      if (it == w->env_vars.end()) {
        return false;
      }
      out->data = it->second.c_str();
      out->length = it->second.size();
      return true;
    };
    if (condition) {
      env.condition = [](void * u, GTEXT_CSS_Condition_Kind k, const GTEXT_CSS_String * text, GTEXT_CSS_Element_Handle) -> GTEXT_CSS_Tristate {
        World * w = static_cast<World *>(u);
        return w->condition(k, std::string(text->data, text->length));
      };
    }
    if (random_base) {
      env.random_base = [](void * u, const GTEXT_CSS_Random_Key * key, double * base) -> bool {
        return static_cast<World *>(u)->random_base(key, base);
      };
    }
    options.property_rules = rules;
    options.elements = use_iface ? &iface : nullptr;
    ctx_status = gtext_css_compute_context_new(registry.r, &env, &options, &ctx, &ctx_result);
    return ctx_status;
  }
};

/// A computed style that frees itself, with its block.
struct Computed {
  std::unique_ptr<cmp::Block> block;
  GTEXT_CSS_Computed_Style * s = nullptr;
  GTEXT_CSS_Compute_Result result{};
  GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;
  Computed() = default;
  Computed(const Computed &) = delete;
  Computed & operator=(const Computed &) = delete;
  ~Computed() {
    gtext_css_computed_free(s);
  }

  /// A custom property's computed value as text, or "<gi>" for the guaranteed-invalid value.
  std::string custom(const std::string & name) const {
    const GTEXT_CSS_Custom_Value * v = nullptr;
    if (!s || !gtext_css_computed_custom_find(s, name.data(), name.size(), &v)) {
      return "<gi>";
    }
    char * text = nullptr;
    size_t n = 0;
    if (gtext_css_custom_value_text(v, nullptr, &text, &n) != GTEXT_CSS_OK) {
      return "<error>";
    }
    std::string out(text, n);
    gtext_css_serialize_buffer_free(nullptr, text);
    return out;
  }

  /// An ordinary property's computed value as text.
  std::string value(const GTEXT_CSS_Registry * r, const std::string & name) const {
    if (!s) {
      return "<no style>";
    }
    const GTEXT_CSS_Value_Set * set = nullptr;
    const GTEXT_CSS_Status st = gtext_css_computed_value(s, cmp::find(r, name), &set);
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
};

/// Compute the style of an element from declarations written as text (`--a: 1px; width: var(--a)`), under a parent.
inline void compute(World & w, Computed & out, const std::string & decls, const Computed * parent = nullptr, const Node * node = nullptr) {
  out.block = std::make_unique<cmp::Block>(w.registry.r, decls);
  out.status = gtext_css_compute_style(w.ctx, out.block->b, parent ? parent->s : nullptr, node, &out.s, &out.result);
}

/// The same from declarations given as (name, value) pairs, for values that hold a `;` (`if()`).
inline void compute_pairs(World & w, Computed & out, const std::vector<std::pair<std::string, std::string>> & decls, const Computed * parent = nullptr,
    const Node * node = nullptr) {
  out.block = std::make_unique<cmp::Block>(w.registry.r, "");
  for (const auto & d : decls) {
    GTEXT_CSS_Property_Outcome outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
    gtext_css_block_add_text(out.block->b, d.first.data(), d.first.size(), d.second.data(), d.second.size(), false, nullptr, &outcome, nullptr);
  }
  out.status = gtext_css_compute_style(w.ctx, out.block->b, parent ? parent->s : nullptr, node, &out.s, &out.result);
}

}  // namespace cust

#endif  // GHOTI_IO_GTEXT_TESTS_CSS_CUSTOM_TEST_SUPPORT_H
