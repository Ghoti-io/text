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
 * What the CSS tests share: a parse that keeps its tree, result and errors
 * together and frees them, a short text form of tokens and component values
 * to compare with, and the serializer to a std::string.
 */

#ifndef GHOTI_IO_GTEXT_TESTS_CSS_TEST_SUPPORT_H
#define GHOTI_IO_GTEXT_TESTS_CSS_TEST_SUPPORT_H

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ghoti.io/text/css.h>

#include "../tools/conformance/css_tree_canon.h"

namespace css_test {

/// The factor by which this host is slower than a native one for a known reason that is not the library: GTEXT_TEST_SLOWDOWN (an emulator;
/// suite/tools/xarch/text.sh sets it). 1 when it is not set or is not a factor from 1 to 1000. The bound on a seed replay is multiplied by it.
inline double slowdown() {
  if (const char * slow = std::getenv("GTEXT_TEST_SLOWDOWN")) {
    const double factor = std::atof(slow);
    if (factor >= 1.0 && factor <= 1000.0) {
      return factor;
    }
  }
  return 1.0;
}

using ParseFn = GTEXT_CSS_Status (*)(const char *, size_t, const GTEXT_CSS_Parse_Options *, GTEXT_CSS_Tree **,
    GTEXT_CSS_Result *);

// One parse, owned.
struct Parsed {
  GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;
  GTEXT_CSS_Tree * tree = nullptr;
  GTEXT_CSS_Result result{};

  Parsed() = default;
  Parsed(const Parsed &) = delete;
  Parsed & operator=(const Parsed &) = delete;
  ~Parsed() {
    gtext_css_tree_free(tree);
    gtext_css_result_free(&result);
  }

  std::vector<GTEXT_CSS_Error> errors() const {
    std::vector<GTEXT_CSS_Error> out;
    for (size_t i = 0; i < gtext_css_result_error_count(&result); i++) {
      GTEXT_CSS_Error e;
      gtext_css_result_error_at(&result, i, &e);
      out.push_back(e);
    }
    return out;
  }

  std::vector<std::string> error_names() const {
    std::vector<std::string> out;
    for (const auto & e : errors()) {
      out.push_back(e.name);
    }
    return out;
  }
};

inline void parse_into(Parsed & p, ParseFn fn, const std::string & text, const GTEXT_CSS_Parse_Options * o = nullptr) {
  p.status = fn(text.data(), text.size(), o, &p.tree, &p.result);
}

inline std::string str(const GTEXT_CSS_String & s) {
  return s.data ? std::string(s.data, s.length) : std::string();
}

inline std::string text_of(const GTEXT_CSS_Component_Value * v, size_t n);

// A component value as short text: the type and what it holds.
inline std::string text_of(const GTEXT_CSS_Component_Value & v) {
  char num[64];
  switch (v.kind) {
  case GTEXT_CSS_VALUE_FUNCTION:
    return "f:" + str(v.value) + "(" + text_of(v.children, v.child_count) + ")";
  case GTEXT_CSS_VALUE_BLOCK: {
    const char * o = v.type == GTEXT_CSS_TOKEN_OPEN_CURLY ? "{" : v.type == GTEXT_CSS_TOKEN_OPEN_SQUARE ? "[" : "(";
    const char * c = v.type == GTEXT_CSS_TOKEN_OPEN_CURLY ? "}" : v.type == GTEXT_CSS_TOKEN_OPEN_SQUARE ? "]" : ")";
    return std::string(o) + text_of(v.children, v.child_count) + c;
  }
  default:
    break;
  }
  switch (v.type) {
  case GTEXT_CSS_TOKEN_IDENT: return "ident:" + str(v.value);
  case GTEXT_CSS_TOKEN_AT_KEYWORD: return "at:" + str(v.value);
  case GTEXT_CSS_TOKEN_HASH: return std::string((v.flags & GTEXT_CSS_FLAG_HASH_ID) ? "hash-id:" : "hash:") + str(v.value);
  case GTEXT_CSS_TOKEN_STRING: return "string:" + str(v.value) + ((v.flags & GTEXT_CSS_FLAG_UNCLOSED) ? "<eof>" : "");
  case GTEXT_CSS_TOKEN_BAD_STRING: return "bad-string";
  case GTEXT_CSS_TOKEN_URL: return "url:" + str(v.value) + ((v.flags & GTEXT_CSS_FLAG_UNCLOSED) ? "<eof>" : "");
  case GTEXT_CSS_TOKEN_BAD_URL: return "bad-url";
  case GTEXT_CSS_TOKEN_DELIM: return "delim:" + str(v.value);
  case GTEXT_CSS_TOKEN_NUMBER:
  case GTEXT_CSS_TOKEN_PERCENTAGE:
  case GTEXT_CSS_TOKEN_DIMENSION: {
    std::snprintf(num, sizeof num, "%.17g", v.number);
    std::string t = v.type == GTEXT_CSS_TOKEN_NUMBER ? "number:" : v.type == GTEXT_CSS_TOKEN_PERCENTAGE ? "percentage:" : "dimension:";
    t += str(v.repr) + "=" + num;
    if (v.type != GTEXT_CSS_TOKEN_PERCENTAGE) {
      t += (v.flags & GTEXT_CSS_FLAG_INTEGER) ? "/int" : "/num";
    }
    if (v.flags & GTEXT_CSS_FLAG_SIGN_PLUS) {
      t += "/+";
    }
    if (v.flags & GTEXT_CSS_FLAG_SIGN_MINUS) {
      t += "/-";
    }
    if (v.type == GTEXT_CSS_TOKEN_DIMENSION) {
      t += "/" + str(v.unit);
    }
    return t;
  }
  case GTEXT_CSS_TOKEN_UNICODE_RANGE:
    std::snprintf(num, sizeof num, "urange:%X-%X", (unsigned)v.range_start, (unsigned)v.range_end);
    return num;
  case GTEXT_CSS_TOKEN_WHITESPACE: return "ws";
  case GTEXT_CSS_TOKEN_CDO: return "CDO";
  case GTEXT_CSS_TOKEN_CDC: return "CDC";
  case GTEXT_CSS_TOKEN_COLON: return ":";
  case GTEXT_CSS_TOKEN_SEMICOLON: return ";";
  case GTEXT_CSS_TOKEN_COMMA: return ",";
  case GTEXT_CSS_TOKEN_CLOSE_SQUARE: return "]";
  case GTEXT_CSS_TOKEN_CLOSE_PAREN: return ")";
  case GTEXT_CSS_TOKEN_CLOSE_CURLY: return "}";
  default: return "?";
  }
}

inline std::string text_of(const GTEXT_CSS_Component_Value * v, size_t n) {
  std::string out;
  for (size_t i = 0; i < n; i++) {
    if (i) {
      out += " ";
    }
    out += text_of(v[i]);
  }
  return out;
}

// The component values of a list parse, as short text.
inline std::string tokens(const std::string & css, const GTEXT_CSS_Parse_Options * o = nullptr) {
  Parsed p;
  parse_into(p, gtext_css_parse_component_values, css, o);
  if (p.status != GTEXT_CSS_OK) {
    return std::string("status:") + gtext_css_status_string(p.status);
  }
  return text_of(p.tree->values, p.tree->value_count);
}

inline GTEXT_Markup_Status string_sink(void * user, const char * bytes, size_t len) {
  static_cast<std::string *>(user)->append(bytes, len);
  return GTEXT_MARKUP_OK;
}

inline std::string serialize(const GTEXT_CSS_Tree * t, const GTEXT_CSS_Serialize_Options * o = nullptr) {
  std::string out;
  GTEXT_Markup_Sink sink = {string_sink, &out};
  const GTEXT_CSS_Status st = gtext_css_serialize_tree(t, o, &sink);
  if (st != GTEXT_CSS_OK) {
    return std::string("status:") + gtext_css_status_string(st);
  }
  return out;
}

inline std::string canon(const GTEXT_CSS_Tree * t) {
  char * c = css_tree_canon(t);
  std::string s(c);
  std::free(c);
  return s;
}

// The round trip of section 9; an empty string when it holds.
inline std::string round_trip(const GTEXT_CSS_Tree * t, const GTEXT_CSS_Parse_Options * o = nullptr) {
  char * detail = nullptr;
  const char * why = css_round_trip(t, o, &detail);
  std::string out = why ? std::string(why) + "\n" + (detail ? detail : "") : "";
  std::free(detail);
  return out;
}

} // namespace css_test

#endif // GHOTI_IO_GTEXT_TESTS_CSS_TEST_SUPPORT_H
