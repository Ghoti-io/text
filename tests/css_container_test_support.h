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
 * A small document for the tests of container queries and conditional style sheets: a synthetic tree, a style sheet or more, a conditions object with a container
 * interface, and the computed style of every element, computed top down through gtext_css_style_compute() so that an ancestor's style exists when a descendant asks.
 */

#ifndef GHOTI_IO_GTEXT_TESTS_CSS_CONTAINER_TEST_SUPPORT_H
#define GHOTI_IO_GTEXT_TESTS_CSS_CONTAINER_TEST_SUPPORT_H

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "css_cascade_test_support.h"
#include "css_synth_tree.h"
#include "css_test_support.h"

namespace cq {

using cas::R;

/// The string of a computed value of a property of a style ("-" for none).
inline std::string value_of(const GTEXT_CSS_Computed_Style * style, const char * property) {
  if (!style) {
    return "-";
  }
  const GTEXT_CSS_Value_Set * set = nullptr;
  if (gtext_css_computed_get_by_name(style, property, std::strlen(property), &set, nullptr) != GTEXT_CSS_OK) {
    return "!";
  }
  const GTEXT_CSS_Value * root = gtext_css_value_set_root(set);
  if (!root) {
    return "-";
  }
  GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
  char * buf = nullptr;
  size_t n = 0;
  if (gtext_css_serialize_value_buffer(root, &so, &buf, &n) != GTEXT_CSS_OK) {
    return "?";
  }
  std::string out(buf, n);
  gtext_css_serialize_buffer_free(&so, buf);
  return out;
}

/// The string of a custom property of a style ("-" for the guaranteed-invalid value).
inline std::string custom_of(const GTEXT_CSS_Computed_Style * style, const char * name) {
  const GTEXT_CSS_Custom_Value * v = nullptr;
  if (!style || !gtext_css_computed_custom_find(style, name, std::strlen(name), &v)) {
    return "-";
  }
  GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
  char * buf = nullptr;
  size_t n = 0;
  if (gtext_css_custom_value_text(v, &so, &buf, &n) != GTEXT_CSS_OK) {
    return "?";
  }
  std::string out(buf, n);
  gtext_css_serialize_buffer_free(&so, buf);
  return out;
}

struct Doc {
  synth::Tree t;
  std::map<int, std::string> own;              ///< Declarations of an element beside the sheets' (as a `style` attribute would write them).
  std::map<int, GTEXT_CSS_Container_Metrics> boxes; ///< The content box of an element; none: no principal box.
  std::map<int, GTEXT_CSS_Container_Scroll_State> scroll;
  std::vector<GTEXT_CSS_Computed_Style *> styles;
  std::vector<std::string> sheets;
  std::vector<GTEXT_CSS_Sheet *> compiled;
  GTEXT_CSS_Conditions * cond = nullptr;
  GTEXT_CSS_Rule_Index * index = nullptr;
  GTEXT_CSS_Compute_Context * cc = nullptr;
  GTEXT_CSS_Selector_Match_Context * match = nullptr;
  GTEXT_CSS_Condition_Call call;
  GTEXT_CSS_Container_Interface ci;
  GTEXT_CSS_Element_Interface iface;
  GTEXT_CSS_Media_Environment env;
  bool give_content_box = true;
  bool give_scroll = true;
  bool give_style = true;
  bool starting_style = false;
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  GTEXT_CSS_Rule_Index_Result last;
  std::string anchored_fallback = "none";

  Doc() {
    gtext_css_condition_call_init(&call);
    env = gtext_css_media_environment_default(nullptr);
  }
  Doc(const Doc &) = delete;
  Doc & operator=(const Doc &) = delete;
  ~Doc() {
    release();
  }

  void release() {
    for (GTEXT_CSS_Computed_Style * s : styles) {
      gtext_css_computed_free(s);
    }
    styles.clear();
    gtext_css_selector_match_context_free(match);
    match = nullptr;
    gtext_css_rule_index_free(index);
    index = nullptr;
    for (GTEXT_CSS_Sheet * s : compiled) {
      gtext_css_sheet_free(s);
    }
    compiled.clear();
    gtext_css_conditions_free(cond);
    cond = nullptr;
    gtext_css_compute_context_free(cc);
    cc = nullptr;
    gtext_css_condition_call_free(&call);
  }

  int add(int parent, const std::string & name, const std::string & id = "", const std::vector<std::string> & classes = {}) {
    return t.add(parent, name, id, classes);
  }

  static const GTEXT_CSS_Computed_Style * style_cb(void * u, GTEXT_CSS_Element_Handle h) {
    Doc * d = static_cast<Doc *>(u);
    const size_t i = static_cast<size_t>(synth::Tree::idx(h));
    return d->give_style && i < d->styles.size() ? d->styles[i] : nullptr;
  }

  /// Build the conditions, the index (over `sheets`) and the style of every element, top down.
  void build() {
    release();
    gtext_css_condition_call_init(&call);
    GTEXT_CSS_Environment env2 = gtext_css_environment_default();
    GTEXT_CSS_Compute_Options cmo = gtext_css_compute_options_default();
    ASSERT_EQ(gtext_css_compute_context_new(R(), &env2, &cmo, &cc, nullptr), GTEXT_CSS_OK);
    std::memset(&ci, 0, sizeof ci);
    ci.size = sizeof ci;
    ci.user = this;
    ci.compute_context = cc;
    ci.computed_style = style_cb;
    if (give_content_box) {
      ci.content_box = [](void * u, GTEXT_CSS_Element_Handle h, GTEXT_CSS_Container_Metrics * out) {
        Doc * d = static_cast<Doc *>(u);
        auto it = d->boxes.find(synth::Tree::idx(h));
        if (it == d->boxes.end()) {
          return false;
        }
        *out = it->second;
        return true;
      };
    }
    if (give_scroll) {
      ci.scroll_state = [](void * u, GTEXT_CSS_Element_Handle h, GTEXT_CSS_Container_Scroll_State * out) {
        Doc * d = static_cast<Doc *>(u);
        auto it = d->scroll.find(synth::Tree::idx(h));
        if (it == d->scroll.end()) {
          return false;
        }
        *out = it->second;
        return true;
      };
    }
    ci.anchored_fallback = [](void * u, GTEXT_CSS_Element_Handle, const GTEXT_CSS_String * value, bool * matches) {
      Doc * d = static_cast<Doc *>(u);
      *matches = std::string(value->data, value->length) == d->anchored_fallback;
      return true;
    };
    iface = t.table;
    GTEXT_CSS_Conditions_Options co = gtext_css_conditions_options_default();
    co.media = &env;
    co.container = &ci;
    co.element_interface = &iface;
    ASSERT_EQ(gtext_css_conditions_new(R(), &co, &cond), GTEXT_CSS_OK);
    GTEXT_CSS_Rule_Index_Options io = gtext_css_rule_index_options_default();
    io.html_document = true;
    io.conditions = cond;
    ASSERT_EQ(gtext_css_rule_index_new(R(), &io, &index), GTEXT_CSS_OK);
    for (const std::string & css : sheets) {
      css_test::Parsed p;
      css_test::parse_into(p, gtext_css_parse_stylesheet, css);
      ASSERT_NE(p.tree, nullptr);
      GTEXT_CSS_Sheet * s = nullptr;
      ASSERT_EQ(gtext_css_sheet_compile(p.tree, R(), nullptr, &s, nullptr), GTEXT_CSS_OK);
      compiled.push_back(s);
      GTEXT_CSS_Sheet_Registration reg;
      std::memset(&reg, 0, sizeof reg);
      reg.origin = GTEXT_CSS_ORIGIN_AUTHOR;
      status = gtext_css_rule_index_add_sheet(index, s, &reg, &last);
      if (status != GTEXT_CSS_OK) {
        return;
      }
    }
    status = gtext_css_rule_index_freeze(index, &last);
    if (status != GTEXT_CSS_OK) {
      return;
    }
    GTEXT_CSS_Selector_Match_Options mo = gtext_css_selector_match_options_default();
    mo.html_document = true;
    ASSERT_EQ(gtext_css_selector_match_context_new(&iface, &mo, &match), GTEXT_CSS_OK);
    styles.assign(t.e.size(), nullptr);
    for (size_t i = 1; i < t.e.size() && status == GTEXT_CSS_OK; i++) {
      styles[i] = compute(static_cast<int>(i), nullptr, &status);
    }
  }

  /// The style of one element (or of a pseudo-element of it, given as `::before`), computed against the styles of its ancestors.
  GTEXT_CSS_Computed_Style * compute(int e, const char * pseudo, GTEXT_CSS_Status * st) {
    GTEXT_CSS_Style_Options so = gtext_css_style_options_default();
    so.sources = 0;
    so.rules = index;
    so.match = match;
    so.starting_style = starting_style;
    so.container_call = &call;
    GTEXT_CSS_Selector_List * pl = nullptr;
    if (pseudo) {
      EXPECT_EQ(gtext_css_selector_parse(pseudo, std::strlen(pseudo), nullptr, &pl, nullptr), GTEXT_CSS_OK);
      so.pseudo = pl;
    }
    GTEXT_CSS_Style_Entry entry;
    std::memset(&entry, 0, sizeof entry);
    entry.keys.origin = GTEXT_CSS_ORIGIN_AUTHOR;
    entry.keys.sheet = 100000;
    auto it = own.find(e);
    const GTEXT_CSS_Style_Entry * entries = nullptr;
    size_t count = 0;
    if (it != own.end()) {
      entry.text.data = it->second.c_str();
      entry.text.length = it->second.size();
      entries = &entry;
      count = 1;
    }
    const int parent = t.e[e].parent;
    GTEXT_CSS_Computed_Style * out = nullptr;
    GTEXT_CSS_Compute_Result cr;
    std::memset(&cr, 0, sizeof cr);
    *st = gtext_css_style_compute(cc, synth::Tree::H(e), pseudo ? styles[e] : (parent ? styles[parent] : nullptr), entries, count, &so, &out, &cr);
    last_name = cr.name;
    gtext_css_selector_list_free(pl);
    return out;
  }
  std::string last_name;

  const GTEXT_CSS_Computed_Style * style(int e) const {
    return e < static_cast<int>(styles.size()) ? styles[e] : nullptr;
  }
  std::string get(int e, const char * property) const {
    return value_of(style(e), property);
  }
};

}  // namespace cq

#endif // GHOTI_IO_GTEXT_TESTS_CSS_CONTAINER_TEST_SUPPORT_H
