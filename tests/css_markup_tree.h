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
 * The element interface (css_element.h) over a Markup tree (an XML or an HTML document), and a computation of the style of every element of it.
 * Shared by the tests of the computed-style call and by the program that answers the browser differential, so that both compute a document the same way.
 *
 * An element of the tree is an @ref cmt::Inst. An SVG `<use>` is instantiated: the element it references is cloned under it (the clone's tree `parent` is the
 * referenced element's own parent, as in a light tree the clone is not under the `use`; its **inheritance parent** is the `use`, which is the case the
 * interface exists for). Header only; included by one translation unit.
 */

#ifndef GHOTI_IO_GTEXT_TESTS_CSS_MARKUP_TREE_H
#define GHOTI_IO_GTEXT_TESTS_CSS_MARKUP_TREE_H

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <ghoti.io/text/css.h>
#include <ghoti.io/text/html.h>
#include <ghoti.io/text/markup.h>
#include <ghoti.io/text/xml.h>

namespace cmt {

struct Inst {
  const GTEXT_Markup_Node * node = nullptr;
  Inst * parent = nullptr;     ///< The tree parent.
  Inst * inherit = nullptr;    ///< The inheritance parent.
  Inst * prev = nullptr;
  Inst * next = nullptr;
  size_t index = 0;            ///< 1-based among the sibling elements.
  size_t siblings = 0;
  size_t order = 0;            ///< Document order.
  std::string id;              ///< The id attribute, and for a clone `use-id>`.
  std::string path;            ///< A name for the instance: the ids of the `use` elements it is under, then its own.
  std::vector<std::string> classes;
  GTEXT_CSS_Computed_Style * style = nullptr;
  bool clone = false;
};

inline std::string attr(const GTEXT_Markup_Node * n, const char * name) {
  GTEXT_Markup_Attribute a;
  if (gtext_markup_element_get_attribute(n, nullptr, name, &a)) {
    return std::string(a.value, a.value_length);
  }
  return "";
}

inline bool has_attr(const GTEXT_Markup_Node * n, const char * name) {
  return gtext_markup_element_get_attribute(n, nullptr, name, nullptr);
}

/// The strings the element interface hands out point into: one set for each thread, so that several threads can ask the same tree at once.
inline std::string * scratch_buffers() {
  static thread_local std::string buffers[4];
  return buffers;
}

struct Tree {
  GTEXT_Markup_Document * doc = nullptr;
  std::vector<std::unique_ptr<Inst>> all;
  std::vector<Inst *> order;
  std::map<std::string, const GTEXT_Markup_Node *> ids;
  GTEXT_CSS_Element_Interface table;

  Tree() {
    memset(&table, 0, sizeof table);
    table.size = sizeof table;
    table.user = this;
    table.parent = [](void *, GTEXT_CSS_Element_Handle e) -> GTEXT_CSS_Element_Handle { return static_cast<const Inst *>(e)->parent; };
    table.inheritance_parent = [](void *, GTEXT_CSS_Element_Handle e) -> GTEXT_CSS_Element_Handle { return static_cast<const Inst *>(e)->inherit; };
    table.local_name = [](void *, GTEXT_CSS_Element_Handle e, GTEXT_CSS_String * out) {
      const char * n = gtext_markup_element_local_name(static_cast<const Inst *>(e)->node);
      if (!n) {
        return false;
      }
      out->data = n;
      out->length = strlen(n);
      return true;
    };
    table.namespace_uri = [](void *, GTEXT_CSS_Element_Handle e, GTEXT_CSS_String * out) {
      const char * n = gtext_markup_element_namespace(static_cast<const Inst *>(e)->node);
      if (!n) {
        return false;
      }
      out->data = n;
      out->length = strlen(n);
      return true;
    };
    table.id = [](void *, GTEXT_CSS_Element_Handle e, GTEXT_CSS_String * out) {
      scratch_buffers()[0] = attr(static_cast<const Inst *>(e)->node, "id");
      if (scratch_buffers()[0].empty()) {
        return false;
      }
      out->data = scratch_buffers()[0].c_str();
      out->length = scratch_buffers()[0].size();
      return true;
    };
    table.class_count = [](void *, GTEXT_CSS_Element_Handle e) -> size_t { return static_cast<const Inst *>(e)->classes.size(); };
    table.class_at = [](void *, GTEXT_CSS_Element_Handle e, size_t i, GTEXT_CSS_String * out) {
      const Inst * x = static_cast<const Inst *>(e);
      if (i >= x->classes.size()) {
        return false;
      }
      out->data = x->classes[i].c_str();
      out->length = x->classes[i].size();
      return true;
    };
    table.attribute = [](void *, GTEXT_CSS_Element_Handle e, const GTEXT_CSS_String * ns, const GTEXT_CSS_String * name, GTEXT_CSS_String * out) {
      GTEXT_Markup_Attribute a;
      const std::string n(name->data, name->length);
      std::string nss;
      if (ns && ns->data) {
        nss.assign(ns->data, ns->length);
      }
      if (!gtext_markup_element_get_attribute(static_cast<const Inst *>(e)->node, ns && ns->data ? nss.c_str() : nullptr, n.c_str(), &a)) {
        return false;
      }
      scratch_buffers()[1].assign(a.value, a.value_length);
      out->data = scratch_buffers()[1].c_str();
      out->length = scratch_buffers()[1].size();
      return true;
    };
    table.previous_sibling = [](void *, GTEXT_CSS_Element_Handle e) -> GTEXT_CSS_Element_Handle { return static_cast<const Inst *>(e)->prev; };
    table.next_sibling = [](void *, GTEXT_CSS_Element_Handle e) -> GTEXT_CSS_Element_Handle { return static_cast<const Inst *>(e)->next; };
    table.sibling_index = [](void *, GTEXT_CSS_Element_Handle e) -> size_t { return static_cast<const Inst *>(e)->index; };
    table.sibling_count = [](void *, GTEXT_CSS_Element_Handle e) -> size_t { return static_cast<const Inst *>(e)->siblings; };
    table.compare_order = [](void *, GTEXT_CSS_Element_Handle a, GTEXT_CSS_Element_Handle b) -> int {
      const size_t x = static_cast<const Inst *>(a)->order, y = static_cast<const Inst *>(b)->order;
      return x < y ? -1 : x > y ? 1 : 0;
    };
    table.state = [](void *, GTEXT_CSS_Element_Handle) -> uint64_t { return 0; };
    table.lang = [](void *, GTEXT_CSS_Element_Handle, GTEXT_CSS_String *) { return false; };
    table.direction = [](void *, GTEXT_CSS_Element_Handle) { return GTEXT_CSS_DIRECTION_LTR; };
  }
  Tree(const Tree &) = delete;
  Tree & operator=(const Tree &) = delete;
  ~Tree() {
    for (auto & i : all) {
      gtext_css_computed_free(i->style);
    }
    gtext_markup_document_free(doc);
  }

  /// Parse an XML document (for SVG) or an HTML document.
  bool parse(const std::string & text, bool html) {
    if (html) {
      GTEXT_HTML_Result r;
      memset(&r, 0, sizeof r);
      const GTEXT_HTML_Status s = gtext_html_parse(text.data(), text.size(), nullptr, &doc, &r);
      gtext_html_result_free(&r);
      return s == GTEXT_HTML_OK && doc;
    }
    GTEXT_XML_Error err;
    return gtext_xml_parse(text.data(), text.size(), nullptr, &doc, &err) == GTEXT_XML_OK;
  }

  static std::vector<std::string> split_classes(const std::string & s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
      while (i < s.size() && strchr(" \t\n\r\f", s[i])) {
        i++;
      }
      size_t j = i;
      while (j < s.size() && !strchr(" \t\n\r\f", s[j])) {
        j++;
      }
      if (j > i) {
        out.push_back(s.substr(i, j - i));
      }
      i = j;
    }
    return out;
  }

  Inst * make(const GTEXT_Markup_Node * n, Inst * parent, Inst * inherit, bool clone, const std::string & path_prefix) {
    all.push_back(std::make_unique<Inst>());
    Inst * x = all.back().get();
    x->node = n;
    x->parent = parent;
    x->inherit = inherit;
    x->clone = clone;
    x->classes = split_classes(attr(n, "class"));
    x->id = attr(n, "id");
    x->path = path_prefix + (x->id.empty() ? std::string(gtext_markup_element_local_name(n)) : x->id);
    x->order = order.size();
    order.push_back(x);
    return x;
  }

  void collect_ids(const GTEXT_Markup_Node * n) {
    for (const GTEXT_Markup_Node * c = gtext_markup_node_first_child(n); c; c = gtext_markup_node_next_sibling(c)) {
      if (gtext_markup_node_kind(c) == GTEXT_MARKUP_KIND_ELEMENT) {
        const std::string id = attr(c, "id");
        if (!id.empty() && !ids.count(id)) {
          ids[id] = c;
        }
        collect_ids(c);
      }
    }
  }

  static bool is_use(const GTEXT_Markup_Node * n) {
    const char * l = gtext_markup_element_local_name(n);
    const char * ns = gtext_markup_element_namespace(n);
    return l && ns && strcmp(l, "use") == 0 && strcmp(ns, GTEXT_CSS_SVG_NAMESPACE) == 0;
  }

  void build_children(const GTEXT_Markup_Node * n, Inst * parent, Inst * inherit_of_children, bool clone, const std::string & prefix, int depth) {
    Inst * prev = nullptr;
    std::vector<Inst *> kids;
    for (const GTEXT_Markup_Node * c = gtext_markup_node_first_child(n); c; c = gtext_markup_node_next_sibling(c)) {
      if (gtext_markup_node_kind(c) != GTEXT_MARKUP_KIND_ELEMENT) {
        continue;
      }
      Inst * x = make(c, parent, inherit_of_children, clone, prefix);
      x->prev = prev;
      if (prev) {
        prev->next = x;
      }
      prev = x;
      kids.push_back(x);
      build_element(x, depth);
    }
    for (size_t i = 0; i < kids.size(); i++) {
      kids[i]->index = i + 1;
      kids[i]->siblings = kids.size();
    }
  }

  void build_element(Inst * x, int depth) {
    build_children(x->node, x, x, x->clone, x->clone ? x->path + ">" : std::string(), depth);
    if (is_use(x->node) && depth < 6) {
      std::string href = attr(x->node, "href");
      if (href.empty() || href[0] != '#') {
        GTEXT_Markup_Attribute a;
        if (gtext_markup_element_get_attribute(x->node, "http://www.w3.org/1999/xlink", "href", &a)) {
          href.assign(a.value, a.value_length);
        }
      }
      if (!href.empty() && href[0] == '#') {
        auto it = ids.find(href.substr(1));
        if (it != ids.end()) {
          /* the clone: its tree parent is where the referenced element is, it inherits from the `use` */
          Inst * ref_parent = nullptr;
          for (auto & o : order) {
            if (o->node == gtext_markup_node_parent(it->second) && !o->clone) {
              ref_parent = o;
              break;
            }
          }
          Inst * c = make(it->second, ref_parent, x, true, x->path + ">");
          build_element(c, depth + 1);
          c->index = 1;
          c->siblings = 1;
        }
      }
    }
  }

  void build() {
    const GTEXT_Markup_Node * root = gtext_markup_document_element(doc);
    if (!root) {
      return;
    }
    collect_ids(doc ? gtext_markup_document_root(doc) : nullptr);
    Inst * r = make(root, nullptr, nullptr, false, "");
    r->index = 1;
    r->siblings = 1;
    build_element(r, 0);
  }

  /// Compute the style of every instance in document order (a parent before its children and a `use` before its clones).
  GTEXT_CSS_Status compute_all(const GTEXT_CSS_Compute_Context * ctx, const GTEXT_CSS_Style_Options * options, GTEXT_CSS_Compute_Result * result = nullptr) {
    for (Inst * i : order) {
      GTEXT_CSS_Computed_Style * s = nullptr;
      const GTEXT_CSS_Status st = gtext_css_style_compute(ctx, i, i->inherit ? i->inherit->style : nullptr, nullptr, 0, options, &s, result);
      if (st != GTEXT_CSS_OK) {
        return st;
      }
      i->style = s;
    }
    return GTEXT_CSS_OK;
  }

  Inst * find(const std::string & path) const {
    for (Inst * i : order) {
      if (i->path == path) {
        return i;
      }
    }
    return nullptr;
  }
};

} // namespace cmt

#endif // GHOTI_IO_GTEXT_TESTS_CSS_MARKUP_TREE_H
