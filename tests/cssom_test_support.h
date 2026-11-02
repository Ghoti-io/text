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
 * What the CSSOM tests share: a document that frees itself and records the events its mutations send, a store of `style` attributes behind the attribute
 * binding (with counters, failures and an attribute version), and short names for the calls a test makes a hundred times.
 */

#ifndef GHOTI_IO_GTEXT_TESTS_CSSOM_TEST_SUPPORT_H
#define GHOTI_IO_GTEXT_TESTS_CSSOM_TEST_SUPPORT_H

#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>

#include "../src/css/cssom_internal.h"
#include "../src/css/cssom_rule_internal.h"
#include "css_registry_test_support.h"
#include "markup_test_support.h"

namespace cot {

inline const GTEXT_CSS_Registry * R() {
  return reg::shared();
}

/// One event, copied out of the borrowed pointers.
struct Ev {
  GTEXT_CSSOM_Event_Kind kind = GTEXT_CSSOM_EVENT_DECLARATIONS_CHANGED;
  std::vector<std::string> props;
  std::vector<std::string> customs;
  bool all = false;
  bool attribute_written = false;
  uint64_t generation = 0;
  const void * declaration = nullptr;
  const void * element = nullptr;
  uint64_t sheet_id = 0, rule_id = 0;
  const void * rule = nullptr;
};

/// A document with a tap that records events. `alloc` (a fault allocator, say) is the document's.
struct Doc {
  GTEXT_CSSOM_Document * d = nullptr;
  std::vector<Ev> events;
  /// Runs inside the tap, after the event was recorded; its status is the observer's.
  std::function<GTEXT_CSS_Status()> inside;
  GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;

  explicit Doc(const std::function<void(GTEXT_CSSOM_Config &)> & tweak = nullptr, const GTEXT_Allocator * alloc = nullptr) {
    GTEXT_CSSOM_Config c = gtext_cssom_config_default();
    c.registry = R();
    c.allocator = alloc;
    if (tweak) {
      tweak(c);
    }
    status = gtext_cssom_document_new(&c, &d);
    if (status == GTEXT_CSS_OK) {
      css_cssom_set_tap(css_document_core(d), &Doc::tap, this);
    }
  }
  Doc(const Doc &) = delete;
  Doc & operator=(const Doc &) = delete;
  ~Doc() {
    gtext_cssom_document_release(d);
  }
  css_core * core() const {
    return css_document_core(d);
  }
  void release() {
    gtext_cssom_document_release(d);
    d = nullptr;
  }

 private:
  static GTEXT_CSS_Status tap(void * user, const css_cssom_event * e) {
    Doc * self = static_cast<Doc *>(user);
    Ev ev;
    ev.kind = e->kind;
    for (size_t i = 0; i < e->property_count; i++) {
      GTEXT_CSS_Property_Info info;
      gtext_css_registry_property_info(R(), e->properties[i], &info);
      ev.props.emplace_back(info.name, info.name_length);
    }
    for (size_t i = 0; i < e->custom_count; i++) {
      ev.customs.emplace_back(e->custom_names[i].data, e->custom_names[i].length);
    }
    ev.all = e->all_changed;
    ev.attribute_written = e->attribute_written;
    ev.generation = e->generation;
    ev.declaration = e->declaration;
    ev.element = e->element;
    ev.sheet_id = e->sheet_id;
    ev.rule_id = e->rule_id;
    ev.rule = e->rule;
    self->events.push_back(ev);
    return self->inside ? self->inside() : GTEXT_CSS_OK;
  }
};

/// The `style` attributes of a set of elements (any pointers), behind the attribute binding, with counters and failures.
struct Attrs {
  std::map<const void *, std::string> value;
  std::set<const void *> present;
  std::map<const void *, uint64_t> version;
  uint64_t tree = 1;   ///< Advances with every change, whoever made it: the caller's tree version.
  size_t gets = 0, sets = 0, removes = 0;
  GTEXT_CSS_Status fail_get = GTEXT_CSS_OK, fail_set = GTEXT_CSS_OK;
  std::string last_written;

  /// A change made behind the declaration's back (`setAttribute('style', text)`).
  void change(const void * e, const std::string & text) {
    value[e] = text;
    present.insert(e);
    version[e]++;
    tree++;
  }
  void drop(const void * e) {
    value.erase(e);
    present.erase(e);
    version[e]++;
    tree++;
  }
  std::string get(const void * e) const {
    auto it = value.find(e);
    return it == value.end() ? "" : it->second;
  }

  GTEXT_CSSOM_Attribute_Binding binding(bool with_version = true) {
    GTEXT_CSSOM_Attribute_Binding b;
    std::memset(&b, 0, sizeof b);
    b.size = sizeof b;
    b.user = this;
    b.get_attribute = [](void * u, const void * e, const char ** v, size_t * n, bool * p) -> GTEXT_CSS_Status {
      Attrs * self = static_cast<Attrs *>(u);
      self->gets++;
      if (self->fail_get != GTEXT_CSS_OK) {
        return self->fail_get;
      }
      auto it = self->value.find(e);
      *p = self->present.count(e) != 0;
      *v = it == self->value.end() ? "" : it->second.c_str();
      *n = it == self->value.end() ? 0 : it->second.size();
      return GTEXT_CSS_OK;
    };
    b.set_attribute = [](void * u, const void * e, const char * v, size_t n) -> GTEXT_CSS_Status {
      Attrs * self = static_cast<Attrs *>(u);
      self->sets++;
      if (self->fail_set != GTEXT_CSS_OK) {
        return self->fail_set;
      }
      self->last_written.assign(v, n);
      self->change(e, std::string(v, n));
      return GTEXT_CSS_OK;
    };
    b.remove_attribute = [](void * u, const void * e) -> GTEXT_CSS_Status {
      Attrs * self = static_cast<Attrs *>(u);
      self->removes++;
      self->drop(e);
      return GTEXT_CSS_OK;
    };
    if (with_version) {
      b.attribute_version = [](void * u, const void * e) -> uint64_t {
        Attrs * self = static_cast<Attrs *>(u);
        return self->version[e];
      };
    }
    return b;
  }
  static uint64_t tree_version(void * u) {
    return static_cast<Attrs *>(u)->tree;
  }
};

/// A declaration that releases itself.
struct Decl {
  GTEXT_CSSOM_Decl * d = nullptr;
  Decl() = default;
  explicit Decl(GTEXT_CSSOM_Decl * made) : d(made) {}
  explicit Decl(Doc & doc) {
    EXPECT_EQ(gtext_cssom_decl_new(doc.d, &d), GTEXT_CSS_OK);
  }
  Decl(const Decl &) = delete;
  Decl & operator=(const Decl &) = delete;
  ~Decl() {
    gtext_cssom_decl_release(d);
  }
  GTEXT_CSSOM_Decl * operator->() const {
    return d;
  }
};

inline std::string take(css_core * core, char * buf) {
  std::string s = buf ? buf : "";
  gtext_cssom_buffer_free(&core->alloc, buf);
  return s;
}

inline std::string text_of(GTEXT_CSSOM_Decl * d) {
  char * out = nullptr;
  size_t n = 0;
  const GTEXT_CSS_Status st = gtext_cssom_decl_css_text(d, &out, &n);
  if (st != GTEXT_CSS_OK) {
    return std::string("<") + gtext_css_status_string(st) + ">";
  }
  std::string s(out, n);
  gtext_cssom_buffer_free(&reinterpret_cast<css_obj *>(d)->core->alloc, out);
  return s;
}

inline std::string get(GTEXT_CSSOM_Decl * d, const std::string & name) {
  char * out = nullptr;
  size_t n = 0;
  const GTEXT_CSS_Status st = gtext_cssom_decl_get_property_value(d, name.data(), name.size(), &out, &n);
  if (st != GTEXT_CSS_OK) {
    return std::string("<") + gtext_css_status_string(st) + ">";
  }
  std::string s(out, n);
  gtext_cssom_buffer_free(&reinterpret_cast<css_obj *>(d)->core->alloc, out);
  return s;
}

inline std::string priority(GTEXT_CSSOM_Decl * d, const std::string & name) {
  const char * p = nullptr;
  const GTEXT_CSS_Status st = gtext_cssom_decl_get_property_priority(d, name.data(), name.size(), &p);
  return st == GTEXT_CSS_OK ? p : std::string("<") + gtext_css_status_string(st) + ">";
}

inline std::string item(GTEXT_CSSOM_Decl * d, size_t i) {
  char * out = nullptr;
  size_t n = 0;
  const GTEXT_CSS_Status st = gtext_cssom_decl_item(d, i, &out, &n);
  if (st != GTEXT_CSS_OK) {
    return std::string("<") + gtext_css_status_string(st) + ">";
  }
  std::string s(out, n);
  gtext_cssom_buffer_free(&reinterpret_cast<css_obj *>(d)->core->alloc, out);
  return s;
}

inline size_t length(GTEXT_CSSOM_Decl * d) {
  size_t n = 0;
  EXPECT_EQ(gtext_cssom_decl_length(d, &n), GTEXT_CSS_OK);
  return n;
}

struct Result {
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  GTEXT_CSSOM_Outcome o{};
  std::string old;   ///< removeProperty's return value.
};

inline Result set(GTEXT_CSSOM_Decl * d, const std::string & name, const std::string & value, const std::string & prio = "") {
  Result r;
  r.st = gtext_cssom_decl_set_property(d, name.data(), name.size(), value.data(), value.size(), prio.data(), prio.size(), &r.o);
  return r;
}

inline Result remove(GTEXT_CSSOM_Decl * d, const std::string & name) {
  Result r;
  char * old = nullptr;
  size_t n = 0;
  r.st = gtext_cssom_decl_remove_property(d, name.data(), name.size(), &old, &n, &r.o);
  if (r.st == GTEXT_CSS_OK) {
    r.old.assign(old, n);
    gtext_cssom_buffer_free(&reinterpret_cast<css_obj *>(d)->core->alloc, old);
  }
  return r;
}

inline Result set_text(GTEXT_CSSOM_Decl * d, const std::string & text) {
  Result r;
  r.st = gtext_cssom_decl_set_css_text(d, text.data(), text.size(), &r.o);
  return r;
}

/// The dump of a declaration.
inline std::string dump(const GTEXT_CSSOM_Decl * d) {
  std::string out;
  GTEXT_Markup_Sink sink;
  std::memset(&sink, 0, sizeof sink);
  sink.user = &out;
  sink.write = [](void * u, const char * s, size_t n) {
    static_cast<std::string *>(u)->append(s, n);
    return GTEXT_MARKUP_OK;
  };
  const GTEXT_CSS_Status st = gtext_cssom_decl_dump(d, &sink);
  return st == GTEXT_CSS_OK ? out : std::string("<dump: ") + gtext_css_status_string(st) + ">";
}

inline const char * kind_name(GTEXT_CSSOM_Outcome_Kind k) {
  switch (k) {
  case GTEXT_CSSOM_UNCHANGED: return "UNCHANGED";
  case GTEXT_CSSOM_CHANGED: return "CHANGED";
  case GTEXT_CSSOM_IGNORED_UNKNOWN_PROPERTY: return "IGNORED_UNKNOWN_PROPERTY";
  case GTEXT_CSSOM_IGNORED_INVALID_VALUE: return "IGNORED_INVALID_VALUE";
  case GTEXT_CSSOM_IGNORED_BAD_PRIORITY: return "IGNORED_BAD_PRIORITY";
  case GTEXT_CSSOM_IGNORED_INVALID_SELECTOR: return "IGNORED_INVALID_SELECTOR";
  case GTEXT_CSSOM_IGNORED_NOT_ALLOWED_HERE: return "IGNORED_NOT_ALLOWED_HERE";
  }
  return "?";
}

/* ---- rules and sheets ------------------------------------------------------------------------------------------------------------------------ */

/// A rule that releases itself (it holds one external reference, which it takes over).
struct Rule {
  GTEXT_CSSOM_Rule * r = nullptr;
  Rule() = default;
  explicit Rule(GTEXT_CSSOM_Rule * made) : r(made) {}
  Rule(const Rule &) = delete;
  Rule & operator=(const Rule &) = delete;
  Rule(Rule && o) noexcept : r(o.r) {
    o.r = nullptr;
  }
  Rule & operator=(Rule && o) noexcept {
    if (this != &o) {
      gtext_cssom_rule_release(r);
      r = o.r;
      o.r = nullptr;
    }
    return *this;
  }
  ~Rule() {
    gtext_cssom_rule_release(r);
  }
  GTEXT_CSSOM_Rule * operator->() const {
    return r;
  }
  explicit operator bool() const {
    return r != nullptr;
  }
};

/// A sheet that releases itself.
struct Sheet {
  GTEXT_CSSOM_Sheet * s = nullptr;
  GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;
  Sheet() = default;
  Sheet(Doc & doc, const std::string & text, const GTEXT_CSSOM_Sheet_Options * options = nullptr) {
    status = gtext_cssom_sheet_new_parsed(doc.d, text.data(), text.size(), options, &s);
  }
  Sheet(const Sheet &) = delete;
  Sheet & operator=(const Sheet &) = delete;
  ~Sheet() {
    gtext_cssom_sheet_release(s);
  }
  const GTEXT_CSSOM_Rule_List * rules() const {
    const GTEXT_CSSOM_Rule_List * l = nullptr;
    EXPECT_EQ(gtext_cssom_sheet_rules(s, &l), GTEXT_CSS_OK);
    return l;
  }
};

/// The rule at an index of a list, retained.
inline Rule at(const GTEXT_CSSOM_Rule_List * list, size_t i) {
  return Rule(gtext_cssom_rule_list_at(list, i));
}

/// The kinds of the rules of a list, with the rules inside a rule between brackets: `style,media[style,nested-declarations],keyframes[keyframe]`.
inline std::string shape(const GTEXT_CSSOM_Rule_List * list) {
  std::string out;
  for (GTEXT_CSSOM_Rule * r = gtext_cssom_rule_list_first(list); r;) {
    if (!out.empty()) {
      out += ",";
    }
    out += gtext_cssom_rule_kind_name(gtext_cssom_rule_kind(r));
    const GTEXT_CSSOM_Rule_List * inner = gtext_cssom_rule_css_rules(r);
    if (inner && gtext_cssom_rule_list_length(inner)) {
      out += "[" + shape(inner) + "]";
    }
    GTEXT_CSSOM_Rule * next = gtext_cssom_rule_list_next(list, r);
    gtext_cssom_rule_release(r);
    r = next;
  }
  return out;
}

inline std::string shape(const Sheet & sheet) {
  return shape(sheet.rules());
}

/// The dump of a sheet or a rule.
inline std::string dump(const GTEXT_CSSOM_Sheet * s) {
  std::string out;
  GTEXT_Markup_Sink sink;
  std::memset(&sink, 0, sizeof sink);
  sink.user = &out;
  sink.write = [](void * u, const char * p, size_t n) {
    static_cast<std::string *>(u)->append(p, n);
    return GTEXT_MARKUP_OK;
  };
  const GTEXT_CSS_Status st = gtext_cssom_sheet_dump(s, &sink);
  return st == GTEXT_CSS_OK ? out : std::string("<dump: ") + gtext_css_status_string(st) + ">";
}

inline std::string dump(const GTEXT_CSSOM_Rule * r) {
  std::string out;
  GTEXT_Markup_Sink sink;
  std::memset(&sink, 0, sizeof sink);
  sink.user = &out;
  sink.write = [](void * u, const char * p, size_t n) {
    static_cast<std::string *>(u)->append(p, n);
    return GTEXT_MARKUP_OK;
  };
  const GTEXT_CSS_Status st = gtext_cssom_rule_dump(r, &sink);
  return st == GTEXT_CSS_OK ? out : std::string("<dump: ") + gtext_css_status_string(st) + ">";
}

/// The `style` of a rule (retained), or an empty Decl.
inline Decl style_of(GTEXT_CSSOM_Rule * r, GTEXT_CSS_Status * status = nullptr) {
  GTEXT_CSSOM_Decl * d = nullptr;
  const GTEXT_CSS_Status st = gtext_cssom_rule_style(r, &d);
  if (status) {
    *status = st;
  }
  return Decl(st == GTEXT_CSS_OK ? d : nullptr);
}


/* ---- insertRule, deleteRule, cssText ------------------------------------------------------------------------------------------------------ */

/// The DOMException name of a status, or the library's own name for one that is not an exception.
inline std::string name(GTEXT_CSS_Status st) {
  if (st == GTEXT_CSS_OK) {
    return "ok";
  }
  const char * n = gtext_cssom_exception_name(st);
  return n ? n : gtext_css_status_string(st);
}

/// What an `insertRule` did.
struct Ins {
  GTEXT_CSS_Status st = GTEXT_CSS_E_INTERNAL;
  GTEXT_CSSOM_Outcome o{};
  Rule rule;
};

inline Ins insert_rule(GTEXT_CSSOM_Sheet * s, const std::string & text, size_t index) {
  Ins r;
  GTEXT_CSSOM_Rule * made = nullptr;
  r.st = gtext_cssom_sheet_insert_rule(s, text.data(), text.size(), index, &made, &r.o);
  r.rule = Rule(made);
  return r;
}

inline Ins insert_rule(GTEXT_CSSOM_Rule * parent, const std::string & text, size_t index) {
  Ins r;
  GTEXT_CSSOM_Rule * made = nullptr;
  r.st = gtext_cssom_rule_insert_rule(parent, text.data(), text.size(), index, &made, &r.o);
  r.rule = Rule(made);
  return r;
}

inline GTEXT_CSS_Status delete_rule(GTEXT_CSSOM_Sheet * s, size_t index, GTEXT_CSSOM_Outcome * o = nullptr) {
  GTEXT_CSSOM_Outcome local;
  return gtext_cssom_sheet_delete_rule(s, index, o ? o : &local);
}

inline GTEXT_CSS_Status delete_rule(GTEXT_CSSOM_Rule * r, size_t index, GTEXT_CSSOM_Outcome * o = nullptr) {
  GTEXT_CSSOM_Outcome local;
  return gtext_cssom_rule_delete_rule(r, index, o ? o : &local);
}

/// `cssText` of a rule, or `<status>` when it cannot be had.
inline std::string css(GTEXT_CSSOM_Rule * r) {
  char * out = nullptr;
  size_t n = 0;
  const GTEXT_CSS_Status st = gtext_cssom_rule_css_text(r, &out, &n);
  if (st != GTEXT_CSS_OK) {
    return std::string("<") + gtext_css_status_string(st) + ">";
  }
  std::string s(out, n);
  gtext_cssom_buffer_free(&reinterpret_cast<css_obj *>(r)->core->alloc, out);
  return s;
}

/// The `cssText` of every rule of a list, in order.
inline std::vector<std::string> css_list(const GTEXT_CSSOM_Rule_List * list) {
  std::vector<std::string> out;
  for (GTEXT_CSSOM_Rule * r = gtext_cssom_rule_list_first(list); r;) {
    out.push_back(css(r));
    GTEXT_CSSOM_Rule * next = gtext_cssom_rule_list_next(list, r);
    gtext_cssom_rule_release(r);
    r = next;
  }
  return out;
}

/// The serialization of a sheet (every rule's `cssText` and a line feed).
inline std::string sheet_text(const GTEXT_CSSOM_Sheet * s, GTEXT_CSS_Status * status = nullptr) {
  std::string out;
  GTEXT_Markup_Sink sink;
  std::memset(&sink, 0, sizeof sink);
  sink.user = &out;
  sink.write = [](void * u, const char * p, size_t n) {
    static_cast<std::string *>(u)->append(p, n);
    return GTEXT_MARKUP_OK;
  };
  const GTEXT_CSS_Status st = gtext_cssom_sheet_serialize(s, &sink);
  if (status) {
    *status = st;
  }
  return st == GTEXT_CSS_OK ? out : std::string("<") + gtext_css_status_string(st) + ">";
}

/// The rules of a list as one string, one `cssText` after another, joined with " | " (for a short assertion).
inline std::string joined(const GTEXT_CSSOM_Rule_List * list) {
  std::string out;
  for (const std::string & t : css_list(list)) {
    out += out.empty() ? t : " | " + t;
  }
  return out;
}

/// A rule reached by indexes from the top of a sheet: `{0, 2}` is the third rule inside the first. Empty if there is none.
inline Rule path(const Sheet & s, std::initializer_list<size_t> indexes) {
  const GTEXT_CSSOM_Rule_List * list = s.rules();
  Rule cur;
  for (size_t i : indexes) {
    cur = at(list, i);
    if (!cur) {
      return cur;
    }
    list = gtext_cssom_rule_css_rules(cur.r);
  }
  return cur;
}

} // namespace cot

#endif // GHOTI_IO_GTEXT_TESTS_CSSOM_TEST_SUPPORT_H
