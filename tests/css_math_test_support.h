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
 * What the math tests share: a math function read from text and written back, and
 * the category and numeric value it came to.
 */

#ifndef GHOTI_IO_GTEXT_TESTS_CSS_MATH_TEST_SUPPORT_H
#define GHOTI_IO_GTEXT_TESTS_CSS_MATH_TEST_SUPPORT_H

#include <cmath>
#include <string>

#include <ghoti.io/text/css.h>
#include <ghoti.io/text/css/css_math.h>

#include "css_test_support.h"

namespace gm {

struct Calc {
  css_test::Parsed parsed;
  GTEXT_CSS_Math * math = nullptr;
  bool valid = false;
  GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;
  GTEXT_CSS_Math_Result result{};

  Calc() = default;
  Calc(const Calc &) = delete;
  Calc & operator=(const Calc &) = delete;
  ~Calc() {
    gtext_css_math_free(math);
  }
};

/// Read `text` (one function) as a math function.
inline void read(Calc & c, const std::string & text, const GTEXT_CSS_Math_Context * ctx = nullptr,
    const GTEXT_CSS_Math_Options * o = nullptr) {
  css_test::parse_into(c.parsed, gtext_css_parse_component_values, text);
  c.status = GTEXT_CSS_E_INVALID;
  if (c.parsed.status != GTEXT_CSS_OK || c.parsed.tree->value_count != 1) {
    return;
  }
  c.status = gtext_css_math_parse(&c.parsed.tree->values[0], ctx, o, &c.valid, &c.math, &c.result);
}

inline std::string serialize(const GTEXT_CSS_Math * m) {
  char * buf = nullptr;
  size_t len = 0;
  const GTEXT_CSS_Status st = gtext_css_math_serialize_buffer(m, nullptr, &buf, &len);
  if (st != GTEXT_CSS_OK) {
    return std::string("status:") + gtext_css_status_string(st);
  }
  std::string s(buf, len);
  gtext_css_serialize_buffer_free(nullptr, buf);
  return s;
}

/// The serialization of the math function, "invalid" if it is not one, or "status:..." for a limit.
inline std::string calc(const std::string & text, const GTEXT_CSS_Math_Context * ctx = nullptr) {
  Calc c;
  read(c, text, ctx);
  if (c.status != GTEXT_CSS_OK) {
    return std::string("status:") + gtext_css_status_string(c.status);
  }
  if (!c.valid) {
    return "invalid";
  }
  return serialize(c.math);
}

} // namespace gm

#endif // GHOTI_IO_GTEXT_TESTS_CSS_MATH_TEST_SUPPORT_H
