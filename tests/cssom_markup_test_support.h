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
 * What the Markup binding tests share: a Markup page, a CSSOM document over it with the Markup tree-version hook, a binding of the two, and short names for the edits of a Markup tree
 * a `<style>` element sees.
 */

#ifndef GHOTI_IO_GTEXT_TESTS_CSSOM_MARKUP_TEST_SUPPORT_H
#define GHOTI_IO_GTEXT_TESTS_CSSOM_MARKUP_TEST_SUPPORT_H

#include <ghoti.io/text/css/css_cssom_markup.h>

#include "cssom_events_test_support.h"

namespace com {

using namespace coe;

inline const char * kSample =
    "<!DOCTYPE html><html><head><style id=s1>p{color:red}</style></head>"
    "<body><div id=d><p id=a class=lead>x</p><p id=b>y</p></div><style id=s2>p{margin-top:1px}</style></body></html>";

/// A Markup page, a CSSOM document over it, and a binding of the document's own scope.
struct Mk {
  std::unique_ptr<Page> page;
  std::unique_ptr<Doc> doc;
  GTEXT_CSSOM_Markup_Binding * binding = nullptr;
  GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;
  GTEXT_CSSOM_Markup_Options options = gtext_cssom_markup_options_default();

  explicit Mk(const std::string & html = kSample, const std::function<void(GTEXT_CSSOM_Config &)> & tweak = nullptr, const GTEXT_Allocator * alloc = nullptr,
      const GTEXT_CSSOM_Markup_Options * mo = nullptr, bool bind = true) {
    page = std::make_unique<Page>(html);
    doc = std::make_unique<Doc>(
        [&](GTEXT_CSSOM_Config & c) {
          c.element_interface = gtext_css_markup_interface(page->adapter);
          c.match_options = &page->mo;
          c.tree_version = &gtext_cssom_markup_tree_version;
          c.tree_version_user = page->markup;
          if (tweak) {
            tweak(c);
          }
        },
        alloc);
    if (mo) {
      options = *mo;
    }
    if (bind) {
      status = gtext_cssom_markup_binding_new(doc->d, scope(), page->markup, &options, &binding);
    }
  }
  Mk(const Mk &) = delete;
  Mk & operator=(const Mk &) = delete;
  ~Mk() {
    gtext_cssom_markup_binding_free(binding);
  }
  GTEXT_CSSOM_Document * d() const {
    return doc->d;
  }
  GTEXT_CSSOM_Scope * scope() const {
    return gtext_cssom_document_scope(doc->d);
  }
  GTEXT_Markup_Document * markup() const {
    return page->markup;
  }
  GTEXT_CSS_Status sync(GTEXT_CSSOM_Outcome * o = nullptr) {
    return gtext_cssom_markup_sync(binding, o);
  }
  GTEXT_Markup_Node * el(const char * selector, size_t index = 0) const {
    const auto v = page->all(selector);
    return index < v.size() ? v[index] : nullptr;
  }
  GTEXT_CSSOM_Markup_Counters counters() const {
    GTEXT_CSSOM_Markup_Counters c;
    EXPECT_EQ(gtext_cssom_markup_binding_counters(binding, &c), GTEXT_CSS_OK);
    return c;
  }
  /// The sheet of an element (retained: the caller releases), or NULL.
  GTEXT_CSSOM_Sheet * sheet(GTEXT_Markup_Node * e) const {
    return gtext_cssom_markup_sheet_of(binding, e);
  }
  size_t count() const {
    return gtext_cssom_scope_sheet_count(scope());
  }
  /// The first rule's selector of each sheet of `styleSheets`, in order, "-" for a sheet with no rules.
  std::string order() const {
    std::string out;
    for (size_t i = 0; i < count(); i++) {
      GTEXT_CSSOM_Sheet * s = gtext_cssom_scope_sheet_at(scope(), i);
      const GTEXT_CSSOM_Rule_List * l = nullptr;
      EXPECT_EQ(gtext_cssom_sheet_rules(s, &l), GTEXT_CSS_OK);
      GTEXT_CSSOM_Rule * r = gtext_cssom_rule_list_at(l, 0);
      std::string sel = "-";
      if (r) {
        char * t = nullptr;
        size_t n = 0;
        if (gtext_cssom_rule_selector_text(r, &t, &n) == GTEXT_CSS_OK) {
          sel.assign(t, n);
          gtext_cssom_buffer_free(gtext_cssom_document_allocator(doc->d), t);
        }
        gtext_cssom_rule_release(r);
      }
      if (!out.empty()) {
        out += ",";
      }
      out += sel;
      gtext_cssom_sheet_release(s);
    }
    return out;
  }
  std::string value(const char * selector, const char * property) {
    const GTEXT_CSS_Rule_Index * x = nullptr;
    EXPECT_EQ(gtext_cssom_document_index(doc->d, &x), GTEXT_CSS_OK);
    return x ? cascaded(*page, x, selector, property) : "<no index>";
  }
};

/// Replace the children of an element with one text node.
inline void set_text(GTEXT_Markup_Document * m, GTEXT_Markup_Node * e, const std::string & text) {
  while (GTEXT_Markup_Node * c = gtext_markup_node_first_child(e)) {
    EXPECT_EQ(gtext_markup_remove(c), GTEXT_MARKUP_OK);
    gtext_markup_node_free(c);
  }
  GTEXT_Markup_Node * t = nullptr;
  ASSERT_EQ(gtext_markup_create_text(m, text.data(), text.size(), &t), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(e, t), GTEXT_MARKUP_OK);
}

inline void set_attr(GTEXT_Markup_Node * e, const char * name, const std::string & value) {
  EXPECT_EQ(gtext_markup_element_set_attribute(e, nullptr, nullptr, name, value.data(), value.size()), GTEXT_MARKUP_OK);
}

/// A new element (detached) in the HTML namespace, or in `ns`.
inline GTEXT_Markup_Node * make(GTEXT_Markup_Document * m, const char * local, const char * ns = GTEXT_MARKUP_NS_HTML) {
  GTEXT_Markup_Node * e = nullptr;
  EXPECT_EQ(gtext_markup_create_element(m, ns, nullptr, local, &e), GTEXT_MARKUP_OK);
  return e;
}

/// A new `<style>` with this text (detached).
inline GTEXT_Markup_Node * make_style(GTEXT_Markup_Document * m, const std::string & text, const char * ns = GTEXT_MARKUP_NS_HTML) {
  GTEXT_Markup_Node * e = make(m, "style", ns);
  if (!text.empty()) {
    set_text(m, e, text);
  }
  return e;
}

inline void append(GTEXT_Markup_Node * parent, GTEXT_Markup_Node * child) {
  EXPECT_EQ(gtext_markup_append_child(parent, child), GTEXT_MARKUP_OK);
}

inline void detach(GTEXT_Markup_Node * n) {
  EXPECT_EQ(gtext_markup_remove(n), GTEXT_MARKUP_OK);
}

/// The id of a sheet in the scope at an index, "0" if none.
inline uint64_t id_at(GTEXT_CSSOM_Scope * scope, size_t i) {
  GTEXT_CSSOM_Sheet * s = gtext_cssom_scope_sheet_at(scope, i);
  const uint64_t id = s ? gtext_cssom_sheet_id(s) : 0;
  gtext_cssom_sheet_release(s);
  return id;
}

/// The snapshot a sheet has (its identity is the pointer).
inline const GTEXT_CSS_Sheet * snap(GTEXT_CSSOM_Sheet * s) {
  GTEXT_CSS_Sheet * x = nullptr;
  EXPECT_EQ(gtext_cssom_sheet_snapshot(s, &x), GTEXT_CSS_OK);
  gtext_css_sheet_free(x);
  return x;
}

}  // namespace com

#endif
