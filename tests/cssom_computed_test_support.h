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
 * What the tests of `getComputedStyle()` share (css_cssom_computed.h): a Markup page with a `<style>` sheet behind the Markup binding, a document over it with the tree-version hook, a
 * layout stub that answers by a table, and short names for the reads of a computed declaration.
 */

#ifndef GHOTI_IO_GTEXT_TESTS_CSSOM_COMPUTED_TEST_SUPPORT_H
#define GHOTI_IO_GTEXT_TESTS_CSSOM_COMPUTED_TEST_SUPPORT_H

#include <map>
#include <set>

#include "cssom_markup_test_support.h"

namespace cc {

using namespace com;

/// A layout stub: the used value of a property is the text in the table (or "n/a" for "does not apply"); every call is recorded.
struct Layout {
  std::map<std::string, std::string> used;
  std::vector<std::string> asked;
  std::set<const void *> unrendered;
  GTEXT_CSS_Status fail = GTEXT_CSS_OK;
  bool has_used = true;
  bool has_rendered = true;
  GTEXT_CSSOM_Layout_Interface iface{};
  /// The time of every animation, in milliseconds, when `timeline` is set (a document whose animations all began at the timeline's 0 and are read there).
  bool timeline = false;
  double now = 0.0;

  Layout() {
    iface.size = sizeof iface;
    iface.user = this;
    iface.is_rendered = &Layout::rendered;
    iface.used_value = &Layout::value;
  }
  void install(GTEXT_CSSOM_Config & c) {
    if (!has_used) {
      iface.used_value = nullptr;
    }
    if (!has_rendered) {
      iface.is_rendered = nullptr;
    }
    if (timeline) {
      iface.animation_time = &Layout::anim_time;
      iface.timeline_version = &Layout::anim_version;
    }
    c.layout = &iface;
  }
  static GTEXT_CSS_Status anim_time(void * user, const void *, const GTEXT_CSS_Selector_List *, const GTEXT_CSS_Anim_Effect *, double * ms, bool * known) {
    *ms = static_cast<Layout *>(user)->now;
    *known = true;
    return GTEXT_CSS_OK;
  }
  static uint64_t anim_version(void *) {
    return 0;
  }
  static GTEXT_CSS_Status rendered(void * user, const void * element, const GTEXT_CSS_Selector_List *, bool * out) {
    Layout * self = static_cast<Layout *>(user);
    *out = !self->unrendered.count(element);
    return GTEXT_CSS_OK;
  }
  static GTEXT_CSS_Status value(void * user, const void *, const GTEXT_CSS_Selector_List *, GTEXT_CSS_Property_Id property, bool * applies, GTEXT_CSS_Value_Set ** out) {
    Layout * self = static_cast<Layout *>(user);
    GTEXT_CSS_Property_Info info;
    gtext_css_registry_property_info(R(), property, &info);
    const std::string name(info.name, info.name_length);
    self->asked.push_back(name);
    if (self->fail != GTEXT_CSS_OK) {
      return self->fail;
    }
    auto it = self->used.find(name);
    if (it == self->used.end() || it->second == "n/a") {
      *applies = false;
      return GTEXT_CSS_OK;
    }
    GTEXT_CSS_Property_Value pv;
    std::memset(&pv, 0, sizeof pv);
    const GTEXT_CSS_Status st = gtext_css_parse_property_text(R(), name.data(), name.size(), it->second.data(), it->second.size(), nullptr, &pv);
    if (st != GTEXT_CSS_OK || pv.outcome != GTEXT_CSS_PROP_RESULT_VALID) {
      gtext_css_property_value_free(&pv);
      return GTEXT_CSS_E_INTERNAL;
    }
    *out = pv.set;   /* the call takes it */
    *applies = true;
    return GTEXT_CSS_OK;
  }
};

/// A page with `html`, a document over it (the Markup tree version hook), the binding synced once.
struct Cs : Mk {
  explicit Cs(const std::string & html, const std::function<void(GTEXT_CSSOM_Config &)> & tweak = nullptr, const GTEXT_Allocator * alloc = nullptr) : Mk(html, tweak, alloc) {
    EXPECT_EQ(status, GTEXT_CSS_OK);
    EXPECT_EQ(sync(), GTEXT_CSS_OK);
  }
  const void * handle(const char * selector, size_t index = 0) const {
    GTEXT_Markup_Node * e = el(selector, index);
    return e ? gtext_css_markup_handle(e) : nullptr;
  }
  GTEXT_CSSOM_Counters dc() const {
    GTEXT_CSSOM_Counters c;
    EXPECT_EQ(gtext_cssom_document_counters(doc->d, &c), GTEXT_CSS_OK);
    return c;
  }
  /// getComputedStyle(element, pseudo): a declaration that releases itself.
  Decl computed(const char * selector, const char * pseudo = nullptr, GTEXT_CSS_Status * status = nullptr) const {
    GTEXT_CSSOM_Decl * d = nullptr;
    const void * h = handle(selector);
    EXPECT_NE(h, nullptr) << selector;
    const GTEXT_CSS_Status st = gtext_cssom_computed_style(doc->d, h, pseudo, pseudo ? std::strlen(pseudo) : 0, &d);
    if (status) {
      *status = st;
    }
    else {
      EXPECT_EQ(st, GTEXT_CSS_OK);
    }
    return Decl(d);
  }
  /// The resolved value of a property of an element, through a fresh declaration.
  std::string value(const char * selector, const std::string & property) const {
    Decl d = computed(selector);
    return d.d ? get(d.d, property) : "<no declaration>";
  }
};

/// The origin a computed declaration reports for a property, as a word.
inline std::string origin(GTEXT_CSSOM_Decl * d, const std::string & property) {
  GTEXT_CSSOM_Origin o = GTEXT_CSSOM_ORIGIN_COMPUTED;
  bool found = false;
  const GTEXT_CSS_Status st = gtext_cssom_computed_origin(d, property.data(), property.size(), &o, &found);
  if (st != GTEXT_CSS_OK) {
    return std::string("<") + gtext_css_status_string(st) + ">";
  }
  if (!found) {
    return "absent";
  }
  return o == GTEXT_CSSOM_ORIGIN_USED ? "used" : o == GTEXT_CSSOM_ORIGIN_ABSENT_LAYOUT ? "absent-layout" : o == GTEXT_CSSOM_ORIGIN_ABSENT_TIMELINE ? "absent-timeline" : "computed";
}

/// Every item name of a declaration, in order.
inline std::vector<std::string> names(GTEXT_CSSOM_Decl * d) {
  std::vector<std::string> out;
  const size_t n = length(d);
  for (size_t i = 0; i < n; i++) {
    out.push_back(item(d, i));
  }
  return out;
}

}  // namespace cc

#endif
