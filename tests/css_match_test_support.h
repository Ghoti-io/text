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
 * Shared by the tests of matching and of the Markup queries: a document parsed from HTML or XML text, an id lookup, and queries by selector text that return the ids of the
 * elements in document order.
 */

#ifndef GHOTI_IO_GTEXT_TESTS_CSS_MATCH_TEST_SUPPORT_H
#define GHOTI_IO_GTEXT_TESTS_CSS_MATCH_TEST_SUPPORT_H

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <ghoti.io/text/css.h>
#include <ghoti.io/text/html.h>
#include <ghoti.io/text/markup.h>
#include <ghoti.io/text/xml.h>

namespace cmatch {

struct Doc {
  GTEXT_Markup_Document * doc = nullptr;
  std::map<std::string, const GTEXT_Markup_Node *> ids;

  Doc() = default;
  Doc(const Doc &) = delete;
  Doc & operator=(const Doc &) = delete;
  ~Doc() {
    gtext_markup_document_free(doc);
  }

  bool html(const std::string & text) {
    GTEXT_HTML_Result r;
    std::memset(&r, 0, sizeof r);
    const GTEXT_HTML_Status s = gtext_html_parse(text.data(), text.size(), nullptr, &doc, &r);
    gtext_html_result_free(&r);
    index();
    return s == GTEXT_HTML_OK && doc;
  }

  bool xml(const std::string & text) {
    GTEXT_XML_Error err;
    const bool ok = gtext_xml_parse(text.data(), text.size(), nullptr, &doc, &err) == GTEXT_XML_OK;
    index();
    return ok;
  }

  void index() {
    ids.clear();
    if (!doc) {
      return;
    }
    const GTEXT_Markup_Node * root = gtext_markup_document_root(doc);
    for (const GTEXT_Markup_Node * n = root; n; n = gtext_markup_node_next_in_order(root, n, false)) {
      if (gtext_markup_node_kind(n) == GTEXT_MARKUP_KIND_ELEMENT) {
        GTEXT_Markup_Attribute a;
        if (gtext_markup_element_get_attribute(n, nullptr, "id", &a)) {
          ids[std::string(a.value, a.value_length)] = n;
        }
      }
    }
  }

  const GTEXT_Markup_Node * root() const {
    return gtext_markup_document_root(doc);
  }

  const GTEXT_Markup_Node * at(const std::string & id) const {
    auto it = ids.find(id);
    return it == ids.end() ? nullptr : it->second;
  }
};

inline std::string id_of(const GTEXT_Markup_Node * n) {
  GTEXT_Markup_Attribute a;
  if (n && gtext_markup_element_get_attribute(n, nullptr, "id", &a)) {
    return std::string(a.value, a.value_length);
  }
  return n ? std::string("<") + gtext_markup_element_local_name(n) + ">" : "(null)";
}

/// The ids of the descendants of `scope` that the selector matches, joined by spaces; "!syntax" for a syntax error, "!<status>" for any other failure.
inline std::string all(const GTEXT_Markup_Node * scope, const std::string & sel, const GTEXT_Markup_Query_Options * options = nullptr) {
  GTEXT_Markup_Node ** out = nullptr;
  size_t n = 0;
  GTEXT_Markup_Query_Result r;
  std::memset(&r, 0, sizeof r);
  const GTEXT_Markup_Status st = gtext_markup_query_selector_all(scope, sel.data(), sel.size(), options, &out, &n, &r);
  std::string s;
  if (st == GTEXT_MARKUP_E_SELECTOR) {
    s = "!syntax";
  }
  else if (st != GTEXT_MARKUP_OK) {
    s = std::string("!") + gtext_markup_status_string(st);
  }
  else {
    for (size_t i = 0; i < n; i++) {
      s += (i ? " " : "") + id_of(out[i]);
    }
    gtext_markup_query_free(options, out);
  }
  gtext_markup_query_result_free(&r);
  return s;
}

inline std::string all(const Doc & d, const std::string & sel) {
  return all(d.root(), sel);
}

inline std::string within(const Doc & d, const std::string & scope_id, const std::string & sel) {
  return all(d.at(scope_id), sel);
}

inline bool matches(const GTEXT_Markup_Node * el, const std::string & sel, const GTEXT_Markup_Query_Options * options = nullptr) {
  bool out = false;
  GTEXT_Markup_Query_Result r;
  std::memset(&r, 0, sizeof r);
  gtext_markup_matches(el, sel.data(), sel.size(), options, &out, &r);
  gtext_markup_query_result_free(&r);
  return out;
}

}  // namespace cmatch

#endif // GHOTI_IO_GTEXT_TESTS_CSS_MATCH_TEST_SUPPORT_H
