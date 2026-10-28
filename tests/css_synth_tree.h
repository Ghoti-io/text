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
 * A tree built in memory and an element interface over it, for the tests that need a shape a parser would not give (a chain of 100,000 elements, a shadow tree, slots
 * and parts) and that count callbacks: elements are indices from 1, an element has a name, an id, classes, attributes, a state word, custom states, a language and a
 * direction, and may be a shadow host (its shadow tree's scope is a number), be assigned to a slot, and be a part of a host under a name.
 */

#ifndef GHOTI_IO_GTEXT_TESTS_CSS_SYNTH_TREE_H
#define GHOTI_IO_GTEXT_TESTS_CSS_SYNTH_TREE_H

#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <ghoti.io/text/css.h>

namespace synth {

struct El {
  int parent = 0, first = 0, last = 0, next = 0, prev = 0;
  std::string name, id, ns;
  std::vector<std::string> classes;
  std::map<std::string, std::string> attrs;
  uint64_t state = 0;
  std::set<std::string> custom;
  std::string lang;
  bool has_lang = false;
  GTEXT_CSS_Direction dir = GTEXT_CSS_DIRECTION_LTR;
  int scope = 0;           ///< The tree scope the element is in: 0 the document, else a shadow root's number.
  int shadow = 0;          ///< The scope of the shadow tree this element hosts, or 0.
  int host = 0;            ///< For an element in a shadow tree: its host.
  int slot = 0;            ///< The slot this element is assigned to.
  std::set<std::pair<int, std::string>> parts; ///< (host, name): this element is a part `name` of `host`.
  uint32_t content = 0;    ///< GTEXT_CSS_CHILD_TEXT bits besides the element children.
};

struct Counts {
  size_t parent = 0, local_name = 0, class_at = 0, attribute = 0, prev = 0, next = 0, first = 0, state = 0, total = 0;
};

class Tree {
 public:
  std::vector<El> e{El()};
  Counts calls;
  GTEXT_CSS_Element_Interface table;

  Tree() {
    std::memset(&table, 0, sizeof table);
    table.size = sizeof table;
    table.user = this;
    table.parent = [](void * u, GTEXT_CSS_Element_Handle h) { return T(u)->tick(T(u)->calls.parent), H(T(u)->at(h).parent); };
    table.local_name = [](void * u, GTEXT_CSS_Element_Handle h, GTEXT_CSS_String * out) {
      T(u)->tick(T(u)->calls.local_name);
      return str(T(u)->at(h).name, out);
    };
    table.namespace_uri = [](void * u, GTEXT_CSS_Element_Handle h, GTEXT_CSS_String * out) { return str(T(u)->at(h).ns, out); };
    table.id = [](void * u, GTEXT_CSS_Element_Handle h, GTEXT_CSS_String * out) { return str(T(u)->at(h).id, out); };
    table.class_count = [](void * u, GTEXT_CSS_Element_Handle h) { return T(u)->at(h).classes.size(); };
    table.class_at = [](void * u, GTEXT_CSS_Element_Handle h, size_t i, GTEXT_CSS_String * out) {
      T(u)->tick(T(u)->calls.class_at);
      return i < T(u)->at(h).classes.size() && str(T(u)->at(h).classes[i], out);
    };
    table.attribute = [](void * u, GTEXT_CSS_Element_Handle h, const GTEXT_CSS_String * ns, const GTEXT_CSS_String * name, GTEXT_CSS_String * out) {
      T(u)->tick(T(u)->calls.attribute);
      (void)ns;
      auto & a = T(u)->at(h).attrs;
      auto it = a.find(std::string(name->data, name->length));
      return it != a.end() && str(it->second, out);
    };
    table.previous_sibling = [](void * u, GTEXT_CSS_Element_Handle h) { return T(u)->tick(T(u)->calls.prev), H(T(u)->at(h).prev); };
    table.next_sibling = [](void * u, GTEXT_CSS_Element_Handle h) { return T(u)->tick(T(u)->calls.next), H(T(u)->at(h).next); };
    table.state = [](void * u, GTEXT_CSS_Element_Handle h) -> uint64_t { return T(u)->tick(T(u)->calls.state), T(u)->at(h).state; };
    table.lang = [](void * u, GTEXT_CSS_Element_Handle h, GTEXT_CSS_String * out) {
      for (int i = idx(h); i; i = T(u)->e[i].parent) {
        if (T(u)->e[i].has_lang) {
          return str(T(u)->e[i].lang, out);
        }
      }
      return false;
    };
    table.direction = [](void * u, GTEXT_CSS_Element_Handle h) { return T(u)->at(h).dir; };
    table.first_child = [](void * u, GTEXT_CSS_Element_Handle h) { return T(u)->tick(T(u)->calls.first), H(T(u)->at(h).first); };
    table.last_child = [](void * u, GTEXT_CSS_Element_Handle h) { return H(T(u)->at(h).last); };
    table.child_content = [](void * u, GTEXT_CSS_Element_Handle h) -> uint32_t {
      return T(u)->at(h).content | (T(u)->at(h).first ? GTEXT_CSS_CHILD_ELEMENT : 0u);
    };
    table.tree_scope = [](void * u, GTEXT_CSS_Element_Handle h) -> const void * { return scope_ptr(T(u)->at(h).scope); };
    table.containing_host = [](void * u, GTEXT_CSS_Element_Handle h) { return H(T(u)->at(h).host); };
    table.shadow_scope = [](void * u, GTEXT_CSS_Element_Handle h) -> const void * { return T(u)->at(h).shadow ? scope_ptr(T(u)->at(h).shadow) : nullptr; };
    table.assigned_slot = [](void * u, GTEXT_CSS_Element_Handle h) { return H(T(u)->at(h).slot); };
    table.has_part = [](void * u, GTEXT_CSS_Element_Handle h, GTEXT_CSS_Element_Handle host, const GTEXT_CSS_String * name) {
      return T(u)->at(h).parts.count({idx(host), std::string(name->data, name->length)}) != 0;
    };
    table.custom_state = [](void * u, GTEXT_CSS_Element_Handle h, const GTEXT_CSS_String * name) {
      return T(u)->at(h).custom.count(std::string(name->data, name->length)) != 0;
    };
    table.column_cell = [](void *, GTEXT_CSS_Element_Handle, GTEXT_CSS_Element_Handle) { return false; };
  }

  /// A new element as the last child of `parent` (0: a root of its tree scope).
  int add(int parent, const std::string & name, const std::string & id = "", const std::vector<std::string> & classes = {}) {
    e.emplace_back();
    const int i = static_cast<int>(e.size()) - 1;
    El & n = e[i];
    n.name = name;
    n.id = id;
    n.classes = classes;
    n.parent = parent;
    if (parent) {
      n.scope = e[parent].scope;
      n.host = e[parent].host;
      n.prev = e[parent].last;
      if (e[parent].last) {
        e[e[parent].last].next = i;
      }
      else {
        e[parent].first = i;
      }
      e[parent].last = i;
    }
    return i;
  }

  /// A shadow tree for `host`: the number of its scope. Elements added below its (root) elements are in it.
  int attach_shadow(int host) {
    const int scope = ++scopes_;
    e[host].shadow = scope;
    shadow_host_[scope] = host;
    return scope;
  }

  /// A top element of the shadow tree `scope` (a child of the shadow root).
  int add_in_shadow(int scope, const std::string & name, const std::string & id = "", const std::vector<std::string> & classes = {}) {
    const int i = add(0, name, id, classes);
    e[i].scope = scope;
    e[i].host = shadow_host_[scope];
    return i;
  }

  /// The handle of an element (0 is none).
  static GTEXT_CSS_Element_Handle H(int i) {
    return i ? reinterpret_cast<GTEXT_CSS_Element_Handle>(static_cast<intptr_t>(i)) : nullptr;
  }

  static int idx(GTEXT_CSS_Element_Handle h) {
    return static_cast<int>(reinterpret_cast<intptr_t>(h));
  }

  static const void * scope_ptr(int scope) {
    return scope ? reinterpret_cast<const void *>(static_cast<intptr_t>(scope) + 1000) : reinterpret_cast<const void *>(static_cast<intptr_t>(1));
  }

  static const void * scope_handle(int scope) {
    return scope_ptr(scope);
  }

 private:
  int scopes_ = 0;
  std::map<int, int> shadow_host_;

  static Tree * T(void * u) {
    return static_cast<Tree *>(u);
  }
  El & at(GTEXT_CSS_Element_Handle h) {
    return e[idx(h)];
  }
  void tick(size_t & c) {
    ++c;
    ++calls.total;
  }
  static bool str(const std::string & s, GTEXT_CSS_String * out) {
    if (s.empty()) {
      return false;
    }
    out->data = s.c_str();
    out->length = s.size();
    return true;
  }
};

}  // namespace synth

#endif // GHOTI_IO_GTEXT_TESTS_CSS_SYNTH_TREE_H
