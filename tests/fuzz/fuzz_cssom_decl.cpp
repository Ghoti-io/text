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
 * libFuzzer harness for the declaration object (src/css/cssom_decl.c over css_block.c): bytes decoded into operations on two declarations of one document, a free-standing one
 * and an element's `style` attribute, with a model of CSSOM's list algorithms beside each.
 *
 * The input is a list of operations, each a byte that selects it and the bytes that select its arguments from the tables below: `setProperty(name, value, priority)`,
 * `removeProperty(name)`, the `cssText` setter, a style IDL attribute set, and `setProperty(p, getPropertyValue(p))`. At most 64 are run.
 *
 * The reference is an ordered list of (key, importance) built by the harness from gtext_css_parse_property_text() and the shorthand expansion for each call: CSSOM's "set a CSS
 * declaration" (a key in the list is replaced where it stands, else appended, each longhand of a shorthand in turn), `removeProperty` (every longhand a shorthand maps to), and
 * the list algorithm of `cssText` (a later declaration removes an earlier one and is appended, and is ignored if the earlier is important and it is not). It is written here, not read from the
 * block. The properties, after every operation:
 *
 *   the list of the declaration is the model's: `length`, the names `item(i)` gives in order, and the priority of each;
 *   a status is OK or a limit, never E_INTERNAL; an operation the outcome reports as ignored leaves the dump byte-identical;
 *   `parse(serialize(x))` is a fixed point: a fresh declaration given the cssText has the same cssText;
 *   `setProperty(p, getPropertyValue(p), priority)` of a longhand with a value changes nothing;
 *   the element's attribute is the declaration's serialization once it was written;
 *   nothing leaks.
 *
 * Seeds: tests/fuzz/corpus/cssom-decl, made by tools/css/make-fuzz-cssom-seeds.py.
 * Build with: make fuzz-cssom-decl      Run: make fuzz-run-cssom-decl
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <ghoti.io/text/css.h>
#include <ghoti.io/text/markup.h>

namespace cssom_declfuzz {

[[noreturn]] void fail(const char * what, const std::string & detail, const std::string & input) {
  std::fprintf(stderr, "fuzz_cssom_decl: %s\n%s\ninput (%zu bytes): ", what, detail.c_str(), input.size());
  for (unsigned char c : input) {
    std::fprintf(stderr, "%02x", c);
  }
  std::fputc('\n', stderr);
  std::abort();
}

const char * const kNames[] = {"color", "margin", "margin-top", "padding", "border", "background", "font", "--a", "--A", "word-wrap", "overflow-wrap", "all", "width", "bogus", "MARGIN",
    "grid-template", "flex", "border-radius", "text-decoration", "animation", "inset", "margin-inline", "z-index", "--",
    "voice-family", "font-size", "glyph-orientation-vertical"};
const char * const kValues[] = {"red", "1px", "1px 2px", "1px 2px 3px 4px", "var(--x)", "inherit", "", "nonsense", "red !important", "10", "url(a) no-repeat", "italic bold 12px/1.5 serif",
    "a b", "  x  ", "solid red 2px", "5", "break-word", "0", "initial", "revert", "none", "1 1 10px", "10px / 20px", "underline", "caption", "preserve male"};
const char * const kPriorities[] = {"", "important", "IMPORTANT", "bad", ""};
const char * const kTexts[] = {"color: red; margin: 1px", "", "color: red; color: blue !important; color: green", "margin-top: 1px !important; margin: 2px", "a{color:red}", "--a: 1; --A: 2; --a: 3",
    "all: inherit; color: red", "width: 1px; bogus: 2; height: 3px", "border: 1px solid red; border-top-color: blue", "font: 12px serif; font-size: 20px", "color: red;;; width: 5px;",
    "margin: var(--x); margin-top: 1px", "font-size: 1px; font: caption; font-size: 2px", "voice-family: male !important; color: red; voice-family: female",
    "color: red; font: caption; font-style: italic; font: caption !important"};

struct Entry {
  std::string key;
  bool important;
  bool opaque_shorthand = false;   ///< A shorthand the library cannot split, kept as written: atomic, and it overrides the longhands before it.
};
using Model = std::vector<Entry>;

const GTEXT_CSS_Registry * registry() {
  static GTEXT_CSS_Registry * r = [] {
    GTEXT_CSS_Registry * made = nullptr;
    gtext_css_registry_new(nullptr, &made);
    return made;
  }();
  return r;
}

std::string name_of(GTEXT_CSS_Property_Id id) {
  GTEXT_CSS_Property_Info info;
  gtext_css_registry_property_info(registry(), id, &info);
  return std::string(info.name, info.name_length);
}

bool is_shorthand(GTEXT_CSS_Property_Id id) {
  GTEXT_CSS_Property_Info info;
  gtext_css_registry_property_info(registry(), id, &info);
  return (info.flags & GTEXT_CSS_PROP_SHORTHAND) && name_of(id) != "all";
}

void leaves(GTEXT_CSS_Property_Id id, std::vector<GTEXT_CSS_Property_Id> & out) {
  GTEXT_CSS_Property_Info info;
  gtext_css_registry_property_info(registry(), id, &info);
  std::vector<GTEXT_CSS_Property_Id> kids;
  for (size_t i = 0; i < info.longhand_count; i++) {
    kids.push_back(gtext_css_registry_resolve_alias(registry(), gtext_css_registry_longhand(registry(), id, i)));
  }
  for (size_t i = 0; i < info.reset_longhand_count; i++) {
    kids.push_back(gtext_css_registry_resolve_alias(registry(), gtext_css_registry_reset_longhand(registry(), id, i)));
  }
  for (GTEXT_CSS_Property_Id k : kids) {
    if (is_shorthand(k)) {
      leaves(k, out);
    }
    else {
      bool seen = false;
      for (GTEXT_CSS_Property_Id o : out) {
        seen = seen || o == k;
      }
      if (!seen) {
        out.push_back(k);
      }
    }
  }
}

int find(Model & m, const std::string & key) {
  for (size_t i = 0; i < m.size(); i++) {
    if (m[i].key == key) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

/// A declaration the library cannot judge, in a list of declarations: kept as written; a later one of the same property removes the earlier and is appended (the place both browsers give).
void model_opaque(Model & m, const std::string & name, bool important, bool shorthand_kept) {
  GTEXT_CSS_Property_Id id = gtext_css_registry_find_property(registry(), name.data(), name.size());
  if (!id) {
    return;
  }
  id = gtext_css_registry_resolve_alias(registry(), id);
  const std::string key = name_of(id);
  const int at = find(m, key);
  if (at >= 0) {
    if (m[at].important && !important) {
      return;
    }
    m.erase(m.begin() + at);
  }
  m.push_back({key, important, shorthand_kept});
}

/// Whether an opaque shorthand after position `at` sets the longhand `key`.
bool opaque_covers_after(const Model & m, int at, const std::string & key) {
  for (size_t j = static_cast<size_t>(at) + 1; j < m.size(); j++) {
    if (!m[j].opaque_shorthand) {
      continue;
    }
    std::vector<GTEXT_CSS_Property_Id> ls;
    leaves(gtext_css_registry_find_property(registry(), m[j].key.data(), m[j].key.size()), ls);
    for (GTEXT_CSS_Property_Id l : ls) {
      if (name_of(l) == key) {
        return true;
      }
    }
  }
  return false;
}

/// CSSOM "set a CSS declaration" for the longhands the parse and the expansion give. `keep_important` is the list rule of cssText.
void model_set(Model & m, const std::string & name, const std::string & value, bool important, bool keep_important) {
  GTEXT_CSS_Property_Value pv;
  const bool custom = name.size() >= 2 && name[0] == '-' && name[1] == '-';
  const GTEXT_CSS_Status st = gtext_css_parse_property_text(registry(), name.data(), name.size(), value.data(), value.size(), nullptr, &pv);
  if (st == GTEXT_CSS_E_UNSUPPORTED && keep_important) {
    gtext_css_property_value_free(&pv);
    model_opaque(m, name, important, false);   // a list of declarations keeps what it cannot judge
    return;
  }
  if (st != GTEXT_CSS_OK || (pv.outcome != GTEXT_CSS_PROP_RESULT_VALID && pv.outcome != GTEXT_CSS_PROP_RESULT_WIDE && pv.outcome != GTEXT_CSS_PROP_RESULT_PENDING)) {
    gtext_css_property_value_free(&pv);
    return;
  }
  std::vector<std::string> keys;
  if (custom) {
    if (name.size() < 3) {
      gtext_css_property_value_free(&pv);
      return;
    }
    keys.push_back(name);
  }
  else if (is_shorthand(pv.property)) {
    GTEXT_CSS_Longhand_List * list = nullptr;
    const GTEXT_CSS_Status ex = gtext_css_shorthand_expand(registry(), &pv, nullptr, &list, nullptr);
    if (ex == GTEXT_CSS_OK) {
      for (size_t i = 0; i < gtext_css_longhand_list_count(list); i++) {
        GTEXT_CSS_Longhand lh;
        gtext_css_longhand_list_at(list, i, &lh);
        keys.push_back(name_of(lh.property));
      }
      gtext_css_longhand_list_free(list);
      const int sup = find(m, name_of(pv.property));
      if (sup >= 0 && m[sup].opaque_shorthand) {
        m.erase(m.begin() + sup);   // a declaration of the whole shorthand supersedes the one kept as written
      }
    }
    else if (ex == GTEXT_CSS_E_UNSUPPORTED && keep_important) {
      const std::string kept = name_of(pv.property);
      gtext_css_property_value_free(&pv);
      model_opaque(m, kept, important, true);
      return;
    }
  }
  else {
    keys.push_back(name_of(pv.property));
  }
  gtext_css_property_value_free(&pv);
  for (const std::string & k : keys) {
    int at = find(m, k);
    if (at >= 0 && opaque_covers_after(m, at, k)) {
      m.erase(m.begin() + at);   // declared before an opaque shorthand that sets it: the new declaration goes after the shorthand
      at = -1;
    }
    if (at >= 0) {
      if (keep_important && m[at].important && !important) {
        continue;
      }
      if (keep_important) {
        m.erase(m.begin() + at);   // a parsed list: the later declaration is appended and the earlier one is gone (setProperty replaces in place)
        m.push_back({k, important});
      }
      else {
        m[at].important = important;
      }
    }
    else {
      m.push_back({k, important});
    }
  }
}

void model_remove(Model & m, const std::string & name) {
  std::vector<std::string> keys;
  if (name.size() >= 2 && name[0] == '-' && name[1] == '-') {
    keys.push_back(name);
  }
  else {
    GTEXT_CSS_Property_Id id = gtext_css_registry_find_property(registry(), name.data(), name.size());
    if (!id) {
      return;
    }
    id = gtext_css_registry_resolve_alias(registry(), id);
    if (is_shorthand(id)) {
      std::vector<GTEXT_CSS_Property_Id> ls;
      leaves(id, ls);
      for (GTEXT_CSS_Property_Id l : ls) {
        keys.push_back(name_of(l));
      }
      keys.push_back(name_of(id));   // a declaration of the shorthand itself, kept as written
    }
    else {
      keys.push_back(name_of(id));
    }
  }
  for (const std::string & k : keys) {
    const int at = find(m, k);
    if (at >= 0) {
      m.erase(m.begin() + at);
    }
  }
}

void model_css_text(Model & m, const std::string & text) {
  m.clear();
  GTEXT_CSS_Tree * tree = nullptr;
  if (gtext_css_parse_declaration_list(text.data(), text.size(), nullptr, &tree, nullptr) != GTEXT_CSS_OK) {
    return;
  }
  for (size_t i = 0; i < tree->declaration_count; i++) {
    const GTEXT_CSS_Declaration & d = tree->declarations[i];
    std::string value;
    for (size_t k = 0; k < d.value_count; k++) {
      (void)k;
    }
    // the value text as the cascade reads it: the source between the first and the last value token
    value = d.value_count ? text.substr(d.value_span.offset, d.value_span.end - d.value_span.offset) : "";
    model_set(m, std::string(d.name.data, d.name.length), value, d.important, true);
  }
  gtext_css_tree_free(tree);
}

struct World {
  GTEXT_CSSOM_Document * doc = nullptr;
  GTEXT_CSSOM_Decl * decl[2] = {nullptr, nullptr};   // free-standing, element's
  Model model[2];
  std::map<const void *, std::string> attr;
  uint64_t tree = 1;
  size_t writes = 0;
};

GTEXT_CSS_Status get_attribute(void * u, const void * e, const char ** v, size_t * n, bool * p) {
  World * w = static_cast<World *>(u);
  auto it = w->attr.find(e);
  *p = it != w->attr.end();
  *v = *p ? it->second.c_str() : "";
  *n = *p ? it->second.size() : 0;
  return GTEXT_CSS_OK;
}

GTEXT_CSS_Status set_attribute(void * u, const void * e, const char * v, size_t n) {
  World * w = static_cast<World *>(u);
  w->attr[e].assign(v, n);
  w->tree++;
  w->writes++;
  return GTEXT_CSS_OK;
}

uint64_t tree_version(void * u) {
  return static_cast<World *>(u)->tree;
}

std::string css_text(GTEXT_CSSOM_Decl * d, const std::string & input) {
  char * out = nullptr;
  size_t n = 0;
  if (gtext_cssom_decl_css_text(d, &out, &n) != GTEXT_CSS_OK) {
    fail("cssText failed", "", input);
  }
  std::string s(out, n);
  gtext_cssom_buffer_free(gtext_cssom_document_allocator(gtext_cssom_decl_document(d)), out);
  return s;
}

std::string dump_of(GTEXT_CSSOM_Decl * d) {
  std::string out;
  GTEXT_Markup_Sink sink;
  std::memset(&sink, 0, sizeof sink);
  sink.user = &out;
  sink.write = [](void * u, const char * s, size_t n) {
    static_cast<std::string *>(u)->append(s, n);
    return GTEXT_MARKUP_OK;
  };
  gtext_cssom_decl_dump(d, &sink);
  return out;
}

void check(World & w, int which, const std::string & input, const char * after) {
  GTEXT_CSSOM_Decl * d = w.decl[which];
  const Model & m = w.model[which];
  size_t n = 0;
  if (gtext_cssom_decl_length(d, &n) != GTEXT_CSS_OK) {
    fail("length failed", after, input);
  }
  if (n != m.size()) {
    fail("the length is not the model's", std::string(after) + ": " + std::to_string(n) + " against " + std::to_string(m.size()) + "\n" + css_text(d, input), input);
  }
  for (size_t i = 0; i < n; i++) {
    char * name = nullptr;
    size_t len = 0;
    gtext_cssom_decl_item(d, i, &name, &len);
    const std::string got(name, len);
    gtext_cssom_buffer_free(gtext_cssom_document_allocator(w.doc), name);
    if (got != m[i].key) {
      fail("an item is not the model's", std::string(after) + ": item " + std::to_string(i) + " is " + got + ", the model has " + m[i].key + "\n" + css_text(d, input), input);
    }
    const char * pr = nullptr;
    gtext_cssom_decl_get_property_priority(d, got.data(), got.size(), &pr);
    if ((std::string(pr) == "important") != m[i].important) {
      fail("an importance is not the model's", std::string(after) + ": " + got, input);
    }
  }
  // parse(serialize(x)) is a fixed point
  const std::string text = css_text(d, input);
  GTEXT_CSSOM_Decl * fresh = nullptr;
  if (gtext_cssom_decl_new(w.doc, &fresh) != GTEXT_CSS_OK) {
    return;
  }
  gtext_cssom_decl_set_css_text(fresh, text.data(), text.size(), nullptr);
  const std::string again = css_text(fresh, input);
  gtext_cssom_decl_release(fresh);
  if (again != text) {
    fail("serialize, parse and serialize is not a fixed point", std::string(after) + ":\n" + text + "\n" + again, input);
  }
  if (which == 1 && w.writes > 0 && w.attr.count("e") && w.attr["e"] != text) {
    fail("the attribute is not the serialization", std::string(after) + ":\n" + w.attr["e"] + "\n" + text, input);
  }
}

} // namespace cssom_declfuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  using namespace cssom_declfuzz;
  const std::string input(reinterpret_cast<const char *>(data), size);
  World w;
  GTEXT_CSSOM_Config c = gtext_cssom_config_default();
  c.registry = registry();
  c.tree_version = tree_version;
  c.tree_version_user = &w;
  if (gtext_cssom_document_new(&c, &w.doc) != GTEXT_CSS_OK) {
    return 0;
  }
  GTEXT_CSSOM_Attribute_Binding b;
  std::memset(&b, 0, sizeof b);
  b.size = sizeof b;
  b.user = &w;
  b.get_attribute = get_attribute;
  b.set_attribute = set_attribute;
  if (gtext_cssom_decl_new(w.doc, &w.decl[0]) != GTEXT_CSS_OK || gtext_cssom_inline_style(w.doc, "e", &b, &w.decl[1]) != GTEXT_CSS_OK) {
    gtext_cssom_decl_release(w.decl[0]);
    gtext_cssom_document_release(w.doc);
    return 0;
  }
  size_t pos = 0;
  auto next = [&]() -> unsigned { return pos < size ? data[pos++] : 0; };
  for (int step = 0; step < 64 && pos < size; step++) {
    const unsigned op = next() % 5;
    const std::string name = kNames[next() % (sizeof kNames / sizeof kNames[0])];
    const std::string value = kValues[next() % (sizeof kValues / sizeof kValues[0])];
    const std::string prio = kPriorities[next() % (sizeof kPriorities / sizeof kPriorities[0])];
    const std::string text = kTexts[next() % (sizeof kTexts / sizeof kTexts[0])];
    for (int which = 0; which < 2; which++) {
      GTEXT_CSSOM_Decl * d = w.decl[which];
      Model & m = w.model[which];
      GTEXT_CSSOM_Outcome o;
      std::memset(&o, 0, sizeof o);
      const std::string before = dump_of(d);
      GTEXT_CSS_Status st = GTEXT_CSS_OK;
      const char * after = "";
      if (op == 0) {
        after = "setProperty";
        st = gtext_cssom_decl_set_property(d, name.data(), name.size(), value.data(), value.size(), prio.data(), prio.size(), &o);
        const bool lower_important = prio == "important" || prio == "IMPORTANT";
        if (st == GTEXT_CSS_OK && !value.empty() && (prio.empty() || lower_important)) {
          model_set(m, name, value, !prio.empty(), false);
        }
        else if (st == GTEXT_CSS_OK && value.empty()) {
          model_remove(m, name);
        }
      }
      else if (op == 1) {
        after = "removeProperty";
        st = gtext_cssom_decl_remove_property(d, name.data(), name.size(), nullptr, nullptr, &o);
        if (st == GTEXT_CSS_OK) {
          model_remove(m, name);
        }
      }
      else if (op == 2) {
        after = "cssText";
        st = gtext_cssom_decl_set_css_text(d, text.data(), text.size(), &o);
        if (st == GTEXT_CSS_OK) {
          model_css_text(m, text);
        }
      }
      else if (op == 3) {
        after = "idl set";
        st = gtext_cssom_decl_idl_set(d, "color", 5, value.data(), value.size(), &o);
        if (st == GTEXT_CSS_OK && !value.empty()) {
          model_set(m, "color", value, false, false);
        }
        else if (st == GTEXT_CSS_OK) {
          model_remove(m, "color");
        }
      }
      else {
        after = "set to the value read";
        char * got = nullptr;
        size_t n = 0;
        if (gtext_cssom_decl_get_property_value(d, "width", 5, &got, &n) == GTEXT_CSS_OK) {
          const std::string v(got, n);
          gtext_cssom_buffer_free(gtext_cssom_document_allocator(w.doc), got);
          if (!v.empty()) {
            const std::string was = css_text(d, input);
            gtext_cssom_decl_set_property(d, "width", 5, v.data(), v.size(), "", 0, &o);
            if (css_text(d, input) != was) {
              fail("setProperty(p, getPropertyValue(p)) changed the declaration", was + "\n" + css_text(d, input), input);
            }
          }
        }
      }
      if (st == GTEXT_CSS_E_INTERNAL) {
        fail("E_INTERNAL", after, input);
      }
      if (o.kind >= GTEXT_CSSOM_IGNORED_UNKNOWN_PROPERTY && dump_of(d) != before) {
        fail("an ignored operation changed the declaration", after, input);
      }
      if (st != GTEXT_CSS_OK && dump_of(d) != before) {
        fail("a failed operation changed the declaration", after, input);
      }
      check(w, which, input, after);
    }
  }
  gtext_cssom_decl_release(w.decl[0]);
  gtext_cssom_decl_release(w.decl[1]);
  gtext_cssom_document_release(w.doc);
  return 0;
}
