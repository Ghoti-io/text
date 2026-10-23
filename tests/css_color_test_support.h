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
 * What the color tests share: a color read from text and written back.
 */

#ifndef GHOTI_IO_GTEXT_TESTS_CSS_COLOR_TEST_SUPPORT_H
#define GHOTI_IO_GTEXT_TESTS_CSS_COLOR_TEST_SUPPORT_H

#include <string>

#include <ghoti.io/text/css.h>
#include <ghoti.io/text/css/css_color.h>

#include "css_test_support.h"

namespace gc {

struct Color {
  css_test::Parsed parsed;
  GTEXT_CSS_Color * color = nullptr;
  bool valid = false;
  GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;
  GTEXT_CSS_Color_Result result{};

  Color() = default;
  Color(const Color &) = delete;
  Color & operator=(const Color &) = delete;
  ~Color() {
    gtext_css_color_free(color);
  }
};

inline void read(Color & c, const std::string & text, const GTEXT_CSS_Color_Options * o = nullptr) {
  css_test::parse_into(c.parsed, gtext_css_parse_component_values, text);
  c.status = GTEXT_CSS_E_INVALID;
  if (c.parsed.status != GTEXT_CSS_OK) {
    return;
  }
  /* one component value (white space around it is not part of the color) */
  const GTEXT_CSS_Component_Value * first = nullptr;
  size_t count = 0;
  for (size_t i = 0; i < c.parsed.tree->value_count; i++) {
    const GTEXT_CSS_Component_Value & v = c.parsed.tree->values[i];
    if (v.kind == GTEXT_CSS_VALUE_TOKEN && v.type == GTEXT_CSS_TOKEN_WHITESPACE) {
      continue;
    }
    if (!first) {
      first = &v;
    }
    count++;
  }
  if (count != 1) {
    c.status = GTEXT_CSS_OK;
    c.valid = false;
    return;
  }
  c.status = gtext_css_color_parse(first, o, &c.valid, &c.color, &c.result);
}

inline std::string serialize(const GTEXT_CSS_Color * color) {
  char * buf = nullptr;
  size_t len = 0;
  const GTEXT_CSS_Status st = gtext_css_color_serialize_buffer(color, nullptr, &buf, &len);
  if (st != GTEXT_CSS_OK) {
    return std::string("status:") + gtext_css_status_string(st);
  }
  std::string s(buf, len);
  gtext_css_serialize_buffer_free(nullptr, buf);
  return s;
}

/// The declared value of the color in `text`, "invalid" if it is not a color, "status:..." for a limit.
inline std::string color(const std::string & text) {
  Color c;
  read(c, text);
  if (c.status != GTEXT_CSS_OK) {
    return std::string("status:") + gtext_css_status_string(c.status);
  }
  return c.valid ? serialize(c.color) : "invalid";
}

} // namespace gc

#endif // GHOTI_IO_GTEXT_TESTS_CSS_COLOR_TEST_SUPPORT_H
