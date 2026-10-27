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
 * The registry of an SVG consumer: the generated registry with the grammars of the properties that the pin leaves unresolved (`fill`, `stroke` and
 * `glyph-orientation-vertical`) registered as SVG 2 writes them, as `vector` registers them. Shared by the tests of the computed-style call and by the
 * program that answers the browser differential.
 */

#ifndef GHOTI_IO_GTEXT_TESTS_CSS_SVG_REGISTRY_H
#define GHOTI_IO_GTEXT_TESTS_CSS_SVG_REGISTRY_H

#include <cstring>

#include <ghoti.io/text/css.h>

namespace svgreg {

inline GTEXT_CSS_Registry * make() {
  GTEXT_CSS_Registry_Builder * b = nullptr;
  if (gtext_css_registry_builder_new(nullptr, &b) != GTEXT_CSS_OK) {
    return nullptr;
  }
  struct Def {
    const char * name;
    const char * grammar;
    const char * initial;
  };
  const char * paint = "none | <color> | <url> [ none | <color> ]? | context-fill | context-stroke";
  const Def defs[] = {{"fill", paint, "black"}, {"stroke", paint, "none"}, {"glyph-orientation-vertical", "auto | <angle> | <number>", "auto"}};
  for (const Def & d : defs) {
    GTEXT_CSS_Property_Def def;
    std::memset(&def, 0, sizeof def);
    def.name = d.name;
    def.grammar = d.grammar;
    def.initial = d.initial;
    def.override = true;
    def.inherited = true;
    if (gtext_css_registry_register_property(b, &def, nullptr) != GTEXT_CSS_OK) {
      gtext_css_registry_builder_free(b);
      return nullptr;
    }
  }
  GTEXT_CSS_Registry * r = nullptr;
  if (gtext_css_registry_freeze(b, &r, nullptr) != GTEXT_CSS_OK) {
    gtext_css_registry_builder_free(b);
    return nullptr;
  }
  return r;
}

/// One registry for a program, made on first use.
inline const GTEXT_CSS_Registry * shared() {
  static GTEXT_CSS_Registry * r = make();
  return r;
}

} // namespace svgreg

#endif // GHOTI_IO_GTEXT_TESTS_CSS_SVG_REGISTRY_H
