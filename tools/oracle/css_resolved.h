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
// shorthand written from the computed values of its longhands (gtext_css_computed_shorthand_text()). Header only.

#ifndef GHOTI_IO_GTEXT_TOOLS_CSS_RESOLVED_H
#define GHOTI_IO_GTEXT_TOOLS_CSS_RESOLVED_H

#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

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

/// Replace each `currentcolor` in the component values by `current`, the resolved value of the element's `color` (CSSOM, "Resolved Values": the resolved value of a
/// color property is its used value, in which `currentcolor` is the element's color). Color 5, 10.1 and 10.2 keep it as a keyword in the *computed* value of a mix
/// or a relative color; the value asked for here is the used one.
inline void substitute_current(const GTEXT_CSS_Component_Value * values, size_t count, const std::string & text, const std::string & current, std::string * out, size_t * from) {
  for (size_t i = 0; i < count; i++) {
    const GTEXT_CSS_Component_Value & v = values[i];
    if (v.kind == GTEXT_CSS_VALUE_TOKEN && v.type == GTEXT_CSS_TOKEN_IDENT && v.value.length == 12 && v.span.offset >= *from && v.span.end <= text.size()) {
      std::string word(v.value.data, v.value.length);
      for (char & c : word) {
        c = (char)std::tolower((unsigned char)c);
      }
      if (word == "currentcolor") {
        *out += text.substr(*from, v.span.offset - *from);
        *out += current;
        *from = v.span.end;
        continue;
      }
    }
    if (v.child_count) {
      substitute_current(v.children, v.child_count, text, current, out, from);
    }
  }
}

/// Replace the colors in the component values by their sRGB form, where the component value is a color that is one (`gtext_css_color_srgb`), and a mix, a
/// relative color, `alpha()` or `contrast-color()` by the text it computes to (`gtext_css_color_compute_text`).
inline void replace_colors(const GTEXT_CSS_Component_Value * values, size_t count, const std::string & text, std::string * out, size_t * from, size_t base,
    bool gradients_only = false) {
  for (size_t i = 0; i < count; i++) {
    const GTEXT_CSS_Component_Value & v = values[i];
    if (gradients_only) {
      /* `content` holds names (counters, strings, quotes) and, as an <image>, gradients: only a gradient's arguments hold colors */
      static const char suffix[] = "gradient";
      if (v.kind == GTEXT_CSS_VALUE_FUNCTION && v.value.length >= sizeof suffix - 1 && v.child_count &&
          std::string(v.value.data + v.value.length - (sizeof suffix - 1), sizeof suffix - 1) == suffix) {
        replace_colors(v.children, v.child_count, text, out, from, base);
      }
      continue;
    }
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
        char computed[GTEXT_CSS_COLOR_COMPUTED_TEXT_MAX + 1];
        size_t computed_length = 0;
        if (!srgb && v.kind == GTEXT_CSS_VALUE_FUNCTION) {
          if (gtext_css_color_compute_text(color, dark_scheme(), nullptr, computed, &computed_length) != GTEXT_CSS_OK) {
            computed_length = 0;
          }
        }
        gtext_css_color_free(color);
        if (srgb && v.span.offset >= *from && v.span.end <= text.size()) {
          *out += text.substr(*from, v.span.offset - *from);
          *out += rgb_text(rgba);
          *from = v.span.end;
          continue;
        }
        if (computed_length && v.span.offset >= *from && v.span.end <= text.size()) {
          *out += text.substr(*from, v.span.offset - *from);
          out->append(computed, computed_length);
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
inline std::string resolved(const std::string & property, const GTEXT_CSS_Value * root, const std::string & original, const std::string & current = std::string()) {
  (void)root;
  std::string text = original;
  const bool gradients_only = property == "content";
  if ((kNameProperties.count(property) && !gradients_only) || text.empty() || text.find_first_of("#(abcdefghijklmnopqrstuvwxyz") == std::string::npos) {
    return text;
  }
  if (!current.empty() && text.size() >= 12) {
    GTEXT_CSS_Tree * pre = nullptr;
    if (gtext_css_parse_component_values(text.data(), text.size(), nullptr, &pre, nullptr) == GTEXT_CSS_OK) {
      std::string subst;
      size_t at = 0;
      substitute_current(pre->values, pre->value_count, text, current, &subst, &at);
      subst += text.substr(at);
      text = subst;
    }
    gtext_css_tree_free(pre);
  }
  GTEXT_CSS_Tree * tree = nullptr;
  if (gtext_css_parse_component_values(text.data(), text.size(), nullptr, &tree, nullptr) != GTEXT_CSS_OK) {
    return text;
  }
  std::string out;
  size_t from = 0;
  replace_colors(tree->values, tree->value_count, text, &out, &from, 0, gradients_only);
  out += text.substr(from);
  gtext_css_tree_free(tree);
  return out;
}

/// The resolved value of the element's `color`, which stands for `currentcolor` in the value of any other property; empty when `target` is `color` itself or when there is none.
inline std::string current_color_text(const GTEXT_CSS_Registry * registry, const GTEXT_CSS_Computed_Style * style, GTEXT_CSS_Property_Id target) {
  const GTEXT_CSS_Property_Id color = gtext_css_registry_find_property(registry, "color", 5);
  if (!color || color == target) {
    return std::string();
  }
  const GTEXT_CSS_Value_Set * set = nullptr;
  if (gtext_css_computed_get(style, color, &set, nullptr) != GTEXT_CSS_OK) {
    return std::string();
  }
  const GTEXT_CSS_Value * root = gtext_css_value_set_root(set);
  return resolved("color", root, serialize(root));
}

/// The items of a value at its top level (white space outside parentheses separates them).
inline std::vector<std::string> top_level_items(const std::string & value) {
  std::vector<std::string> items;
  std::string item;
  int depth = 0;
  for (const char c : value) {
    if (c == '(') {
      depth++;
    }
    else if (c == ')') {
      depth--;
    }
    if (c == ' ' && depth == 0) {
      if (!item.empty()) {
        items.push_back(item);
        item.clear();
      }
      continue;
    }
    item += c;
  }
  if (!item.empty()) {
    items.push_back(item);
  }
  return items;
}

/// The sides of a `border-*-color` shorthand written as few times as they can be (CSSOM, "serialize a CSS declaration block": a pair or a box of equal sides is
/// written once): a color that became the color of its neighbor (`currentcolor lime` under `color: lime`) needs the library's longhand comparison to run again, and
/// the longhands are not changed here.
inline std::string collapse_sides(const std::string & property, const std::string & value) {
  if (property.rfind("border-", 0) != 0 || property.size() < 6 || property.compare(property.size() - 6, 6, "-color") != 0) {
    return value;
  }
  std::vector<std::string> v = top_level_items(value);
  if (v.size() == 4) {
    if (v[3] == v[1]) {
      v.pop_back();
    }
    if (v.size() == 3 && v[2] == v[0]) {
      v.pop_back();
    }
    if (v.size() == 2 && v[1] == v[0]) {
      v.pop_back();
    }
  }
  else if (v.size() == 2 && v[1] == v[0]) {
    v.pop_back();
  }
  else {
    return value;
  }
  std::string out;
  for (const std::string & item : v) {
    out += out.empty() ? item : " " + item;
  }
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
    std::string text = resolved(std::string(t.name, t.name_length), root, serialize(root), current_color_text(registry, style, target));
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
  /* Both pinned browsers write the resolved value of these two shorthands from all their longhands, the initial ones included (`text-emphasis: black` is
     `none rgb(0, 0, 0)`, `-webkit-text-stroke: green` is `0px rgb(0, 128, 0)`; recorded cases text-emphasis.1 and webkit-text-stroke.1): a resolved
     longhand is a used value, and a color is never the initial `currentcolor`. The served text is silent. */
  static const char * const full_shorthands[][3] = {{"text-emphasis", "text-emphasis-style", "text-emphasis-color"}, {"-webkit-text-stroke", "-webkit-text-stroke-width", "-webkit-text-stroke-color"}};
  for (const auto & full : full_shorthands) {
    if (std::string(t.name, t.name_length) == full[0]) {
      std::string joined;
      for (int k = 1; k < 3; k++) {
        const GTEXT_CSS_Property_Id leaf = gtext_css_registry_find_property(registry, full[k], std::strlen(full[k]));
        std::string part;
        const GTEXT_CSS_Status ls = leaf ? read(registry, style, leaf, &part, result) : GTEXT_CSS_E_INVALID;
        if (ls != GTEXT_CSS_OK) {
          return ls;
        }
        joined += (k > 1 ? " " : "") + part;
      }
      *out = joined;
      return GTEXT_CSS_OK;
    }
  }
  char * buf = nullptr;
  size_t n = 0;
  const GTEXT_CSS_Status st = gtext_css_computed_shorthand_text(style, target, &buf, &n, result);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  std::string text(buf, n);
  gtext_css_serialize_buffer_free(nullptr, buf);
  const std::string current = current_color_text(registry, style, target);
  std::string value = resolved(std::string(t.name, t.name_length), nullptr, text, current);
  if (value != text && !current.empty()) {
    value = collapse_sides(std::string(t.name, t.name_length), value);
  }
  *out = value;
  return GTEXT_CSS_OK;
}

} // namespace resolved_value

#endif // GHOTI_IO_GTEXT_TOOLS_CSS_RESOLVED_H
