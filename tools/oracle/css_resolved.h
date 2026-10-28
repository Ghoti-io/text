// SPDX-License-Identifier: LGPL-3.0-only
//
// Copyright (C) 2026 Corey Pennycuff
//
// This file is part of Ghoti.io Text.
//
// Ghoti.io Text is free software: you can redistribute it and/or modify it
// under the terms of the GNU Lesser General Public License version 3 as
// published by the Free Software Foundation.
//
// Ghoti.io Text is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
// for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

// The computed value of a property as the CSSOM resolves it, for the two programs that compare this library with a browser's `getComputedStyle()`
// (tools/oracle/css_style_ours.cpp, tools/conformance/css_computed_suite.cpp): a value serialized, a color written as the browsers write a resolved one, and a
// shorthand written from the computed values of its longhands. Header only.

#ifndef GHOTI_IO_GTEXT_TOOLS_CSS_RESOLVED_H
#define GHOTI_IO_GTEXT_TOOLS_CSS_RESOLVED_H

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

#include <ghoti.io/text/css.h>

namespace resolved_value {

inline std::string serialize(const GTEXT_CSS_Value * root) {
  if (!root) {
    return "";
  }
  char * text = nullptr;
  size_t n = 0;
  if (gtext_css_serialize_value_buffer(root, nullptr, &text, &n) != GTEXT_CSS_OK) {
    return "<serialize error>";
  }
  std::string s(text, n);
  gtext_css_serialize_buffer_free(nullptr, text);
  return s;
}

// ---- the resolved value of a color ----------------------------------------------------------------------------------------

/// Properties whose values are names of the author's and never colors, so that a word in them that is a color's name is left alone.
inline const std::set<std::string> kNameProperties = {"font-family", "animation-name", "counter-reset", "counter-set", "counter-increment", "grid-template-areas", "grid-area",
    "grid-row-start", "grid-row-end", "grid-column-start", "grid-column-end", "view-transition-name", "container-name", "will-change", "transition-property", "scroll-timeline-name",
    "view-timeline-name", "anchor-name", "position-anchor", "timeline-scope", "font-palette", "link-parameters", "list-style-type", "counter-style", "content", "quotes", "src", "page", "string-set",
    "font-feature-settings", "font-variation-settings", "font-variant-alternates", "scroll-marker-group", "position-try-fallbacks", "position-try", "--*"};

inline std::string rgb_text(const double rgba[4]) {
  char buf[128];
  const long r = std::lround(rgba[0]), g = std::lround(rgba[1]), b = std::lround(rgba[2]);
  if (rgba[3] == 1.0) {
    std::snprintf(buf, sizeof buf, "rgb(%ld, %ld, %ld)", r, g, b);
  }
  else {
    std::snprintf(buf, sizeof buf, "rgba(%ld, %ld, %ld, %.10g)", r, g, b, rgba[3]);
  }
  return buf;
}

/// The scheme the system colors below are resolved for (the program sets it).
inline bool & dark_scheme() {
  static bool dark = false;
  return dark;
}

/// The system colors whose used value both browsers agree on: CSS Color 4 leaves a system color to the user agent, and the CSSOM resolved value of a color is its used value.
inline bool system_color(const std::string & word, double rgba[4]) {
  std::string w;
  for (char c : word) {
    w += (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
  }
  const bool dark = dark_scheme();
  if (w == "canvastext" || w == "fieldtext" || w == "buttontext") {
    rgba[0] = rgba[1] = rgba[2] = dark ? 255 : 0;
    rgba[3] = 1;
    return true;
  }
  if (w == "canvas" || w == "field" || w == "buttonface") {
    rgba[0] = rgba[1] = rgba[2] = dark ? 18 : 255;
    rgba[3] = 1;
    return true;
  }
  return false;
}

/// Replace the colors in the component values by their sRGB form, where the component value is a color that is one (`gtext_css_color_srgb`).
inline void replace_colors(const GTEXT_CSS_Component_Value * values, size_t count, const std::string & text, std::string * out, size_t * from, size_t base) {
  for (size_t i = 0; i < count; i++) {
    const GTEXT_CSS_Component_Value & v = values[i];
    const bool maybe = (v.kind == GTEXT_CSS_VALUE_TOKEN && (v.type == GTEXT_CSS_TOKEN_IDENT || v.type == GTEXT_CSS_TOKEN_HASH)) ||
        (v.kind == GTEXT_CSS_VALUE_FUNCTION && v.value.length >= 3);
    if (v.kind == GTEXT_CSS_VALUE_TOKEN && v.type == GTEXT_CSS_TOKEN_IDENT) {
      double sys[4] = {0, 0, 0, 1};
      if (system_color(std::string(v.value.data, v.value.length), sys) && v.span.offset >= *from && v.span.end <= text.size()) {
        *out += text.substr(*from, v.span.offset - *from);
        *out += rgb_text(sys);
        *from = v.span.end;
        continue;
      }
    }
    if (maybe) {
      GTEXT_CSS_Color_Options co = gtext_css_color_options_default();
      bool valid = false;
      GTEXT_CSS_Color * color = nullptr;
      double rgba[4];
      if (gtext_css_color_parse(&v, &co, &valid, &color, nullptr) == GTEXT_CSS_OK && valid && color) {
        const bool srgb = gtext_css_color_srgb(color, rgba);
        gtext_css_color_free(color);
        if (srgb && v.span.offset >= *from && v.span.end <= text.size()) {
          *out += text.substr(*from, v.span.offset - *from);
          *out += rgb_text(rgba);
          *from = v.span.end;
          continue;
        }
      }
      else if (color) {
        gtext_css_color_free(color);
      }
    }
    if (v.child_count) {
      replace_colors(v.children, v.child_count, text, out, from, base);
    }
  }
}

/// The value of a property as the CSSOM resolves it where it holds colors: an sRGB color (a named one, `transparent`, a hex one, `rgb()`, `hsl()`, `hwb()`) is
/// `rgb(r, g, b)` or `rgba(r, g, b, a)` wherever it is in the value (a gradient's stop, a shadow's color), and `currentcolor` stays.
inline std::string resolved(const std::string & property, const GTEXT_CSS_Value * root, const std::string & text) {
  (void)root;
  if (kNameProperties.count(property) || text.empty() || text.find_first_of("#(abcdefghijklmnopqrstuvwxyz") == std::string::npos) {
    return text;
  }
  GTEXT_CSS_Tree * tree = nullptr;
  if (gtext_css_parse_component_values(text.data(), text.size(), nullptr, &tree, nullptr) != GTEXT_CSS_OK) {
    return text;
  }
  std::string out;
  size_t from = 0;
  replace_colors(tree->values, tree->value_count, text, &out, &from, 0);
  out += text.substr(from);
  gtext_css_tree_free(tree);
  return out;
}

inline bool canonical_off() {
  static const bool off = std::getenv("CSS_RESOLVED_NO_CANON") != nullptr;
  return off;
}

/// The computed value of any property of a style as `getComputedStyle()` writes it: a longhand's value (a color as `rgb()`), a shorthand's from its longhands
/// (CSSOM `getPropertyValue`: empty when the longhands cannot be written as one value). Returns a status of the library.
inline GTEXT_CSS_Status read(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Computed_Style * style, GTEXT_CSS_Property_Id id, std::string * out,
    GTEXT_CSS_Compute_Result * result) {
  GTEXT_CSS_Property_Info info;
  if (!gtext_css_registry_property_info(registry, id, &info)) {
    return GTEXT_CSS_E_INVALID;
  }
  const GTEXT_CSS_Property_Id target = gtext_css_registry_resolve_alias(registry, id);
  GTEXT_CSS_Property_Info t;
  gtext_css_registry_property_info(registry, target, &t);
  if (!(t.flags & GTEXT_CSS_PROP_SHORTHAND)) {
    const GTEXT_CSS_Value_Set * set = nullptr;
    const GTEXT_CSS_Status st = gtext_css_computed_get(style, target, &set, result);
    if (st != GTEXT_CSS_OK) {
      return st;
    }
    const GTEXT_CSS_Value * root = gtext_css_value_set_root(set);
    std::string text = resolved(std::string(t.name, t.name_length), root, serialize(root));
    if (!canonical_off()) {
      /* CSSOM writes a resolved value in the shortest form its property's serialization rules give (`scale: 1`, `rotate: 0deg`), which is what the library does for a specified one */
      GTEXT_CSS_Property_Value pv;
      if (gtext_css_parse_property_text(registry, t.name, t.name_length, text.data(), text.size(), nullptr, &pv) == GTEXT_CSS_OK && pv.outcome == GTEXT_CSS_PROP_RESULT_VALID) {
        char * buf = nullptr;
        size_t n = 0;
        if (gtext_css_property_value_serialize_buffer(&pv, nullptr, &buf, &n) == GTEXT_CSS_OK) {
          text.assign(buf, n);
          gtext_css_serialize_buffer_free(nullptr, buf);
        }
      }
      gtext_css_property_value_free(&pv);
    }
    *out = text;
    return GTEXT_CSS_OK;
  }
  GTEXT_CSS_Declaration_Block * block = nullptr;
  GTEXT_CSS_Status st = gtext_css_block_new(registry, nullptr, &block);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  for (size_t i = 0;; i++) {
    const GTEXT_CSS_Property_Id lh = gtext_css_registry_longhand(registry, target, i);
    if (!lh) {
      break;
    }
    GTEXT_CSS_Property_Info li;
    gtext_css_registry_property_info(registry, lh, &li);
    std::string text;
    st = read(registry, style, lh, &text, result);
    if (st != GTEXT_CSS_OK) {
      gtext_css_block_free(block);
      return st;
    }
    GTEXT_CSS_Property_Outcome outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
    st = gtext_css_block_add_text(block, li.name, li.name_length, text.data(), text.size(), false, nullptr, &outcome, nullptr);
    if (st != GTEXT_CSS_OK) {
      gtext_css_block_free(block);
      return st;
    }
  }
  char * buf = nullptr;
  size_t n = 0;
  st = gtext_css_block_shorthand_value(block, target, &buf, &n, nullptr);
  gtext_css_block_free(block);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  out->assign(buf, n);
  gtext_css_serialize_buffer_free(nullptr, buf);
  return GTEXT_CSS_OK;
}

} // namespace resolved_value

#endif // GHOTI_IO_GTEXT_TOOLS_CSS_RESOLVED_H
