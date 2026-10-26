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
 * Helpers shared by the registry and property tests.
 */

#ifndef GHOTI_IO_GTEXT_TESTS_CSS_REGISTRY_TEST_SUPPORT_H
#define GHOTI_IO_GTEXT_TESTS_CSS_REGISTRY_TEST_SUPPORT_H

#include <cstring>
#include <string>
#include <vector>

#include <ghoti.io/text/css.h>

#include "css_test_support.h"

namespace reg {

/// One registry of the generated layer alone, made on first use and shared by the tests that only read.
inline const GTEXT_CSS_Registry * shared() {
  static GTEXT_CSS_Registry * r = [] {
    GTEXT_CSS_Registry * made = nullptr;
    if (gtext_css_registry_new(nullptr, &made) != GTEXT_CSS_OK) {
      return static_cast<GTEXT_CSS_Registry *>(nullptr);
    }
    return made;
  }();
  return r;
}

/// A frozen registry that is freed with it.
struct Owned {
  GTEXT_CSS_Registry * r = nullptr;
  Owned() = default;
  Owned(const Owned &) = delete;
  Owned & operator=(const Owned &) = delete;
  ~Owned() {
    gtext_css_registry_free(r);
  }
};

/// A builder that is freed with it unless it was frozen.
struct Builder {
  GTEXT_CSS_Registry_Builder * b = nullptr;
  Builder(const GTEXT_CSS_Registry_Options * o = nullptr) {
    EXPECT_EQ(gtext_css_registry_builder_new(o, &b), GTEXT_CSS_OK);
  }
  Builder(const Builder &) = delete;
  Builder & operator=(const Builder &) = delete;
  ~Builder() {
    gtext_css_registry_builder_free(b);
  }
  GTEXT_CSS_Status freeze(Owned & out, GTEXT_CSS_Registry_Result * res = nullptr) {
    const GTEXT_CSS_Status st = gtext_css_registry_freeze(b, &out.r, res);
    if (st == GTEXT_CSS_OK) {
      b = nullptr;
    }
    return st;
  }
};

inline GTEXT_CSS_Property_Id find(const GTEXT_CSS_Registry * r, const std::string & name) {
  return gtext_css_registry_find_property(r, name.data(), name.size());
}

inline GTEXT_CSS_Property_Info info(const GTEXT_CSS_Registry * r, const std::string & name) {
  GTEXT_CSS_Property_Info i;
  std::memset(&i, 0, sizeof i);
  gtext_css_registry_property_info(r, find(r, name), &i);
  return i;
}

/// What parsing a value for a property came to: "valid:<serialization>", "wide:<keyword>", "pending:<text>", "invalid", "unknown",
/// or the status when it is not OK.
inline std::string parse(const GTEXT_CSS_Registry * r, const std::string & name, const std::string & value,
    const GTEXT_CSS_Property_Options * o = nullptr) {
  GTEXT_CSS_Property_Value pv;
  const GTEXT_CSS_Status st = gtext_css_parse_property_text(r, name.data(), name.size(), value.data(), value.size(), o, &pv);
  std::string out;
  if (st != GTEXT_CSS_OK) {
    out = gtext_css_status_string(st);
  }
  else {
    switch (pv.outcome) {
    case GTEXT_CSS_PROP_RESULT_UNKNOWN: out = "unknown"; break;
    case GTEXT_CSS_PROP_RESULT_INVALID: out = "invalid"; break;
    default: {
      char * buf = nullptr;
      size_t n = 0;
      const GTEXT_CSS_Status ss = gtext_css_property_value_serialize_buffer(&pv, nullptr, &buf, &n);
      out = ss == GTEXT_CSS_OK ? std::string(buf, n) : std::string("serialize:") + gtext_css_status_string(ss);
      gtext_css_serialize_buffer_free(nullptr, buf);
      out = std::string(pv.outcome == GTEXT_CSS_PROP_RESULT_WIDE ? "wide:" : pv.outcome == GTEXT_CSS_PROP_RESULT_PENDING ? "pending:" : "valid:") + out;
    }
    }
  }
  gtext_css_property_value_free(&pv);
  return out;
}

inline GTEXT_CSS_Property_Options descriptor_of(const char * at_rule) {
  GTEXT_CSS_Property_Options o;
  std::memset(&o, 0, sizeof o);
  o.at_rule = at_rule;
  return o;
}

} // namespace reg

#endif // GHOTI_IO_GTEXT_TESTS_CSS_REGISTRY_TEST_SUPPORT_H
