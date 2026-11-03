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
 * What the observer tests share: a recorder that registers a public observer and copies every event it is called with out of the borrowed pointers (the summaries as text and
 * numbers), and the names of the kinds.
 */

#ifndef GHOTI_IO_GTEXT_TESTS_CSSOM_EVENTS_TEST_SUPPORT_H
#define GHOTI_IO_GTEXT_TESTS_CSSOM_EVENTS_TEST_SUPPORT_H

#include <functional>
#include <string>
#include <vector>

#include "cssom_document_test_support.h"

namespace coe {

using namespace cod;

inline const char * kind_name(GTEXT_CSSOM_Event_Kind k) {
  switch (k) {
  case GTEXT_CSSOM_EVENT_SHEET_ADDED: return "SHEET_ADDED";
  case GTEXT_CSSOM_EVENT_SHEET_REMOVED: return "SHEET_REMOVED";
  case GTEXT_CSSOM_EVENT_SHEET_DISABLED_CHANGED: return "SHEET_DISABLED_CHANGED";
  case GTEXT_CSSOM_EVENT_SHEET_MEDIA_CHANGED: return "SHEET_MEDIA_CHANGED";
  case GTEXT_CSSOM_EVENT_RULE_INSERTED: return "RULE_INSERTED";
  case GTEXT_CSSOM_EVENT_RULE_REMOVED: return "RULE_REMOVED";
  case GTEXT_CSSOM_EVENT_RULE_REPLACED: return "RULE_REPLACED";
  case GTEXT_CSSOM_EVENT_DECLARATIONS_CHANGED: return "DECLARATIONS_CHANGED";
  case GTEXT_CSSOM_EVENT_INLINE_STYLE_CHANGED: return "INLINE_STYLE_CHANGED";
  case GTEXT_CSSOM_EVENT_ADOPTED_CHANGED: return "ADOPTED_CHANGED";
  case GTEXT_CSSOM_EVENT_IMPORT_COMPLETED: return "IMPORT_COMPLETED";
  case GTEXT_CSSOM_EVENT_CONDITIONS_CHANGED: return "CONDITIONS_CHANGED";
  }
  return "?";
}

/// One event, copied out.
struct Seen {
  GTEXT_CSSOM_Event_Kind kind = GTEXT_CSSOM_EVENT_SHEET_ADDED;
  uint64_t generation = 0, sheet_id = 0, rule_id = 0;
  const void * sheet = nullptr, * rule = nullptr, * declaration = nullptr, * scope = nullptr, * element = nullptr;
  std::vector<size_t> path;
  bool has_path = false, detached = false, attribute_written = false, complete = false;
  size_t descendants = 0;
  bool has_selectors = false, has_selectors_before = false;
  std::string selectors, selectors_before;
  uint32_t deps = 0, deps_before = 0;
  std::vector<std::string> attributes;
  bool has_condition = false;
  uint32_t cond_environment = 0;
  uint64_t cond_features0 = 0;
  uint32_t cond_container = 0;
  bool cond_custom = false;
  bool cond_unknown = false;
  std::vector<std::string> props, customs;
  std::vector<uint32_t> ids;
  bool all = false;
  std::string note;   ///< Whatever the hook put there, from inside the call.
};

/// An observer on a document that records what it is called with. `hook` runs inside the call, after the event was copied; its status is the observer's.
struct Rec {
  GTEXT_CSSOM_Document * d = nullptr;
  GTEXT_CSSOM_Observer_Handle handle = 0;
  std::vector<Seen> seen;
  std::function<GTEXT_CSS_Status(const GTEXT_CSSOM_Event &, Seen &)> hook;
  GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;

  explicit Rec(GTEXT_CSSOM_Document * doc) : d(doc) {
    status = gtext_cssom_document_observe(d, &Rec::call, this, &handle);
  }
  Rec(const Rec &) = delete;
  Rec & operator=(const Rec &) = delete;
  ~Rec() {
    if (handle) {
      gtext_cssom_document_unobserve(d, handle);
    }
  }
  void off() {
    EXPECT_EQ(gtext_cssom_document_unobserve(d, handle), GTEXT_CSS_OK);
    handle = 0;
  }

  std::vector<std::string> kinds() const {
    std::vector<std::string> out;
    for (const Seen & s : seen) {
      out.push_back(kind_name(s.kind));
    }
    return out;
  }
  std::string kinds_text() const {
    std::string out;
    for (const Seen & s : seen) {
      if (!out.empty()) {
        out += ",";
      }
      out += kind_name(s.kind);
    }
    return out;
  }

 private:
  static std::string list_text(const GTEXT_CSS_Selector_List * list) {
    char * text = nullptr;
    size_t n = 0;
    const GTEXT_Allocator * a = gtext_allocator_default();   // not the document's: a test that fails the document's allocator must not fail the recorder
    EXPECT_EQ(gtext_css_selector_serialize_buffer(list, a, &text, &n), GTEXT_CSS_OK);
    std::string s(text ? text : "", n);
    gtext_css_selector_buffer_free(a, text);
    return s;
  }
  static GTEXT_CSS_Status call(void * user, const GTEXT_CSSOM_Event * e) {
    Rec * self = static_cast<Rec *>(user);
    Seen s;
    EXPECT_EQ(e->size, sizeof *e);
    s.kind = e->kind;
    s.generation = e->generation;
    s.sheet_id = e->sheet_id;
    s.rule_id = e->rule_id;
    s.sheet = e->sheet;
    s.rule = e->rule;
    s.declaration = e->declaration;
    s.scope = e->scope;
    s.element = e->element;
    s.has_path = e->has_path;
    if (e->has_path) {
      s.path.assign(e->path, e->path + e->path_length);
    }
    s.detached = e->detached_since;
    s.attribute_written = e->attribute_written;
    s.complete = e->summary_complete;
    s.descendants = e->descendants;
    if (e->selectors) {
      s.has_selectors = true;
      s.selectors = list_text(e->selectors);
    }
    if (e->selectors_before) {
      s.has_selectors_before = true;
      s.selectors_before = list_text(e->selectors_before);
    }
    s.deps = e->selector_dependencies.kinds;
    s.deps_before = e->selector_dependencies_before.kinds;
    for (size_t i = 0; i < e->selector_dependencies.attribute_count; i++) {
      s.attributes.emplace_back(e->selector_dependencies.attributes[i].data, e->selector_dependencies.attributes[i].length);
    }
    s.has_condition = e->has_condition;
    s.cond_environment = e->condition_dependencies.environment;
    s.cond_features0 = e->condition_dependencies.features[0];
    s.cond_custom = e->condition_dependencies.custom_media;
    s.cond_container = e->condition_dependencies.container;
    s.cond_unknown = e->condition_dependencies.unknown_features;
    for (size_t i = 0; i < e->property_count; i++) {
      GTEXT_CSS_Property_Info info;
      gtext_css_registry_property_info(R(), e->properties[i], &info);
      s.props.emplace_back(info.name, info.name_length);
      s.ids.push_back(static_cast<uint32_t>(e->properties[i]));
    }
    for (size_t i = 0; i < e->custom_count; i++) {
      s.customs.emplace_back(e->custom_names[i].data, e->custom_names[i].length);
    }
    s.all = e->all_changed;
    GTEXT_CSS_Status st = GTEXT_CSS_OK;
    if (self->hook) {
      st = self->hook(*e, s);
    }
    self->seen.push_back(s);
    return st;
  }
};

/// What a scenario owns: the world, an importer, and everything to let go of before the world goes.
struct Ctx {
  Importer imp;
  std::unique_ptr<World> w;
  std::unique_ptr<World> other;
  std::vector<GTEXT_CSSOM_Sheet *> sheets;
  std::vector<GTEXT_CSSOM_Rule *> rules;
  std::vector<GTEXT_CSSOM_Decl *> decls;
  std::vector<GTEXT_CSSOM_Scope *> scopes;
  std::vector<GTEXT_CSSOM_Media_List *> medias;
  std::vector<Compiled *> compiled;
  std::vector<Cond *> conds;
  int handles[4] = {0, 0, 0, 0};

  explicit Ctx(const GTEXT_Allocator * alloc = nullptr) {
    w = std::make_unique<World>([this](GTEXT_CSSOM_Config & c) { imp.install(c); }, alloc);
  }
  Ctx(const Ctx &) = delete;
  Ctx & operator=(const Ctx &) = delete;
  ~Ctx() {
    for (auto * m : medias) gtext_cssom_media_release(m);
    for (auto * r : rules) gtext_cssom_rule_release(r);
    for (auto * d : decls) gtext_cssom_decl_release(d);
    for (auto * s : scopes) gtext_cssom_scope_release(s);
    for (auto * s : sheets) gtext_cssom_sheet_release(s);
    for (auto * c : compiled) delete c;
    for (auto * c : conds) delete c;
  }
  GTEXT_CSSOM_Document * d() const {
    return w->d();
  }
  GTEXT_CSSOM_Scope * doc_scope() const {
    return gtext_cssom_document_scope(w->d());
  }
  GTEXT_CSSOM_Sheet * sheet(const std::string & text) {
    GTEXT_CSSOM_Sheet * s = nullptr;
    EXPECT_EQ(gtext_cssom_sheet_new_parsed(d(), text.data(), text.size(), nullptr, &s), GTEXT_CSS_OK);
    sheets.push_back(s);
    return s;
  }
  /// A parsed sheet put in `styleSheets`.
  GTEXT_CSSOM_Sheet * owned(const std::string & text) {
    GTEXT_CSSOM_Sheet * s = sheet(text);
    EXPECT_EQ(gtext_cssom_scope_insert_sheet(doc_scope(), s, gtext_cssom_scope_sheet_count(doc_scope()), nullptr), GTEXT_CSS_OK);
    return s;
  }
  GTEXT_CSSOM_Sheet * constructed(const std::string & text) {
    GTEXT_CSSOM_Sheet * s = nullptr;
    EXPECT_EQ(gtext_cssom_sheet_new_constructed(d(), nullptr, &s), GTEXT_CSS_OK);
    EXPECT_EQ(gtext_cssom_sheet_replace_sync(s, text.data(), text.size(), nullptr), GTEXT_CSS_OK);
    sheets.push_back(s);
    return s;
  }
  GTEXT_CSSOM_Rule * rule_at(GTEXT_CSSOM_Sheet * s, size_t i) {
    const GTEXT_CSSOM_Rule_List * l = nullptr;
    EXPECT_EQ(gtext_cssom_sheet_rules(s, &l), GTEXT_CSS_OK);
    GTEXT_CSSOM_Rule * r = gtext_cssom_rule_list_at(l, i);
    EXPECT_NE(r, nullptr);
    rules.push_back(r);
    return r;
  }
  GTEXT_CSSOM_Rule * inner_at(GTEXT_CSSOM_Rule * r, size_t i) {
    GTEXT_CSSOM_Rule * x = gtext_cssom_rule_list_at(gtext_cssom_rule_css_rules(r), i);
    EXPECT_NE(x, nullptr);
    rules.push_back(x);
    return x;
  }
  GTEXT_CSSOM_Decl * style_of(GTEXT_CSSOM_Rule * r) {
    GTEXT_CSSOM_Decl * dd = nullptr;
    EXPECT_EQ(gtext_cssom_rule_style(r, &dd), GTEXT_CSS_OK);
    decls.push_back(dd);
    return dd;
  }
  GTEXT_CSSOM_Decl * decl() {
    GTEXT_CSSOM_Decl * dd = nullptr;
    EXPECT_EQ(gtext_cssom_decl_new(d(), &dd), GTEXT_CSS_OK);
    decls.push_back(dd);
    return dd;
  }
  GTEXT_CSSOM_Scope * scope(int handle) {
    GTEXT_CSSOM_Scope_Options o = gtext_cssom_scope_options_default();
    o.handle = &handles[handle];
    o.shadow = true;
    GTEXT_CSSOM_Scope * s = nullptr;
    EXPECT_EQ(gtext_cssom_scope_new(d(), &o, &s), GTEXT_CSS_OK);
    scopes.push_back(s);
    return s;
  }
  GTEXT_CSSOM_Media_List * media_of(GTEXT_CSSOM_Sheet * s) {
    GTEXT_CSSOM_Media_List * m = nullptr;
    EXPECT_EQ(gtext_cssom_sheet_media(s, &m), GTEXT_CSS_OK);
    medias.push_back(m);
    return m;
  }
  GTEXT_CSSOM_Media_List * media_of(GTEXT_CSSOM_Rule * r) {
    GTEXT_CSSOM_Media_List * m = nullptr;
    EXPECT_EQ(gtext_cssom_rule_media(r, &m), GTEXT_CSS_OK);
    medias.push_back(m);
    return m;
  }
};

inline GTEXT_CSS_Status set_color(GTEXT_CSSOM_Decl * d, const char * value) {
  return gtext_cssom_decl_set_property(d, "color", 5, value, std::strlen(value), "", 0, nullptr);
}

inline std::string join(const std::vector<std::string> & v) {
  std::string out;
  for (const std::string & s : v) {
    if (!out.empty()) {
      out += ",";
    }
    out += s;
  }
  return out;
}

inline std::string path_text(const std::vector<size_t> & p) {
  std::string out;
  for (size_t i : p) {
    if (!out.empty()) {
      out += "/";
    }
    out += std::to_string(i);
  }
  return out;
}

}  // namespace coe

#endif
