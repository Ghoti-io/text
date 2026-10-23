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
 * What the CSS values and grammar tests share: a table of named types for the
 * resolver, a grammar that is compiled and freed, one match with its value set,
 * and a structural dump of a typed value to compare with.
 */

#ifndef GHOTI_IO_GTEXT_TESTS_CSS_VALUES_TEST_SUPPORT_H
#define GHOTI_IO_GTEXT_TESTS_CSS_VALUES_TEST_SUPPORT_H

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <ghoti.io/text/css.h>

#include "css_test_support.h"

namespace gv {

/// The grammars a resolver hands out: type name to definition text; property name to definition text.
struct Defs {
  std::map<std::string, std::string> types, props;
  std::vector<GTEXT_CSS_Definition *> made;
  std::vector<std::string> asked;  // every reference the resolver was asked for, in order
  GTEXT_CSS_Grammar_Options options = gtext_css_grammar_options_default();

  Defs() = default;
  Defs(const Defs &) = delete;
  Defs & operator=(const Defs &) = delete;
  ~Defs() {
    for (auto * d : made) {
      gtext_css_definition_free(d);
    }
  }

  static const GTEXT_CSS_Definition * resolve(void * user, GTEXT_CSS_Ref_Kind kind, const char * name, size_t length) {
    Defs * self = static_cast<Defs *>(user);
    const std::string key(name, length);
    self->asked.push_back((kind == GTEXT_CSS_REF_PROPERTY ? "'" : "") + key);
    const auto & table = kind == GTEXT_CSS_REF_PROPERTY ? self->props : self->types;
    const auto it = table.find(key);
    if (it == table.end()) {
      return nullptr;
    }
    GTEXT_CSS_Definition * d = nullptr;
    if (gtext_css_definition_parse(it->second.data(), it->second.size(), &self->options, &d, nullptr) != GTEXT_CSS_OK) {
      return nullptr;
    }
    self->made.push_back(d);
    return d;
  }
};

/// A compiled grammar, freed with it.
struct Grammar {
  GTEXT_CSS_Compiled * c = nullptr;
  GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;
  GTEXT_CSS_Grammar_Result result{};

  Grammar() = default;
  Grammar(const Grammar &) = delete;
  Grammar & operator=(const Grammar &) = delete;
  ~Grammar() {
    gtext_css_compiled_free(c);
  }
  bool ok() const {
    return status == GTEXT_CSS_OK;
  }
};

inline void compile_into(Grammar & g, const std::string & text, Defs * defs = nullptr, const GTEXT_CSS_Grammar_Options * o = nullptr) {
  g.status = gtext_css_compile(text.data(), text.size(), defs ? Defs::resolve : nullptr, defs, o ? o : (defs ? &defs->options : nullptr),
      &g.c, &g.result);
}

/// A structural dump of a value.
inline std::string num(double v) {
  char b[64];
  std::snprintf(b, sizeof b, "%.10g", v);
  return b;
}

inline std::string dump(const GTEXT_CSS_Value * v) {
  if (!v) {
    return "<none>";
  }
  auto kids = [&]() {
    std::string s;
    for (size_t i = 0; i < v->child_count; i++) {
      if (i) {
        s += " ";
      }
      s += dump(&v->children[i]);
    }
    return s;
  };
  const std::string text = css_test::str(v->text);
  switch (v->kind) {
  case GTEXT_CSS_VAL_KEYWORD: return "kw:" + text;
  case GTEXT_CSS_VAL_CUSTOM_IDENT: return "ci:" + text;
  case GTEXT_CSS_VAL_DASHED_IDENT: return "di:" + text;
  case GTEXT_CSS_VAL_STRING: return "str:" + text;
  case GTEXT_CSS_VAL_URL: return "url:" + text + (v->child_count ? "(" + kids() + ")" : "");
  case GTEXT_CSS_VAL_NUMBER: return "num:" + num(v->number);
  case GTEXT_CSS_VAL_INTEGER: return "int:" + num(v->number);
  case GTEXT_CSS_VAL_PERCENTAGE: return "pct:" + num(v->number);
  case GTEXT_CSS_VAL_DIMENSION: {
    const char * u = gtext_css_unit_name(v->unit);
    return "dim:" + num(v->number) + (u ? u : text.c_str());
  }
  case GTEXT_CSS_VAL_RATIO: return "ratio:" + num(v->number) + "/" + num(v->number2);
  case GTEXT_CSS_VAL_ZERO: return "zero";
  case GTEXT_CSS_VAL_LIST: {
    const char * sep = v->separator == GTEXT_CSS_SEP_COMMA ? "comma" : v->separator == GTEXT_CSS_SEP_SLASH ? "slash" : "space";
    return std::string("list/") + sep + "(" + kids() + ")";
  }
  case GTEXT_CSS_VAL_FUNCTION: return "fn:" + text + "(" + kids() + ")";
  case GTEXT_CSS_VAL_UNICODE_RANGE: {
    char b[40];
    std::snprintf(b, sizeof b, "ur:%X-%X", (unsigned)v->range_start, (unsigned)v->range_end);
    return b;
  }
  case GTEXT_CSS_VAL_PENDING: return "pending:" + text;
  case GTEXT_CSS_VAL_UNPARSED: return "unparsed:" + text;
  case GTEXT_CSS_VAL_TOKEN: return "tok:" + text;
  case GTEXT_CSS_VAL_BLOCK: return "block:" + text + "(" + kids() + ")";
  default: return "?";
  }
}

inline std::string serialize(const GTEXT_CSS_Value * v, const GTEXT_CSS_Serialize_Options * o = nullptr) {
  if (!v) {
    return "";
  }
  char * buf = nullptr;
  size_t len = 0;
  const GTEXT_CSS_Status st = gtext_css_serialize_value_buffer(v, o, &buf, &len);
  if (st != GTEXT_CSS_OK) {
    return std::string("status:") + gtext_css_status_string(st);
  }
  std::string s(buf, len);
  gtext_css_serialize_buffer_free(o, buf);
  return s;
}

/// One match, owned.
struct Matched {
  GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;
  bool matched = false;
  GTEXT_CSS_Value_Set * set = nullptr;
  GTEXT_CSS_Match_Result result{};
  css_test::Parsed parsed; // the component values

  Matched() = default;
  Matched(const Matched &) = delete;
  Matched & operator=(const Matched &) = delete;
  ~Matched() {
    gtext_css_value_set_free(set);
  }
  const GTEXT_CSS_Value * root() const {
    return gtext_css_value_set_root(set);
  }
  std::string dump() const {
    return matched ? gv::dump(root()) : std::string(status == GTEXT_CSS_OK ? "no match" : std::string("status:") + gtext_css_status_string(status));
  }
  std::string text() const {
    return matched ? gv::serialize(root()) : std::string(status == GTEXT_CSS_OK ? "no match" : std::string("status:") + gtext_css_status_string(status));
  }
};

/// Match component values already parsed.
inline void match_values(Matched & m, const Grammar & g, const GTEXT_CSS_Component_Value * v, size_t n, const GTEXT_CSS_Match_Options * o = nullptr) {
  m.status = gtext_css_match(g.c, v, n, o, &m.matched, &m.set, &m.result);
}

/// Parse `css` as a list of component values and match it.
inline void match_into(Matched & m, const Grammar & g, const std::string & css, const GTEXT_CSS_Match_Options * o = nullptr) {
  css_test::parse_into(m.parsed, gtext_css_parse_component_values, css);
  if (m.parsed.status != GTEXT_CSS_OK) {
    m.status = m.parsed.status;
    return;
  }
  match_values(m, g, m.parsed.tree->values, m.parsed.tree->value_count, o);
}

/// Parse `name: value` as a declaration and match its value (the way a unicode-range value is tokenized).
inline void match_declaration(Matched & m, const Grammar & g, const std::string & decl, const GTEXT_CSS_Match_Options * o = nullptr) {
  css_test::parse_into(m.parsed, gtext_css_parse_declaration, decl);
  if (m.parsed.status != GTEXT_CSS_OK || !m.parsed.tree->declaration_count) {
    m.status = m.parsed.status == GTEXT_CSS_OK ? GTEXT_CSS_E_INVALID : m.parsed.status;
    return;
  }
  const GTEXT_CSS_Declaration & d = m.parsed.tree->declarations[0];
  match_values(m, g, d.value, d.value_count, o);
}

/// Compile `grammar` (with `defs`) and match `css`: the dump of the value, "no match", or "status:...".
inline std::string run(const std::string & grammar, const std::string & css, Defs * defs = nullptr, const GTEXT_CSS_Match_Options * o = nullptr) {
  Grammar g;
  compile_into(g, grammar, defs);
  if (!g.ok()) {
    return std::string("compile:") + gtext_css_status_string(g.status) + " " + g.result.name;
  }
  Matched m;
  match_into(m, g, css, o);
  return m.dump();
}

/// The same, as the serialized value.
inline std::string run_text(const std::string & grammar, const std::string & css, Defs * defs = nullptr, const GTEXT_CSS_Match_Options * o = nullptr) {
  Grammar g;
  compile_into(g, grammar, defs);
  if (!g.ok()) {
    return std::string("compile:") + gtext_css_status_string(g.status) + " " + g.result.name;
  }
  Matched m;
  match_into(m, g, css, o);
  return m.text();
}

} // namespace gv

#endif // GHOTI_IO_GTEXT_TESTS_CSS_VALUES_TEST_SUPPORT_H
