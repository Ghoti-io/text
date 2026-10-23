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
 * The named colors and the system colors of CSS Color 4 (sections 6.1 and 6.3), as tables.
 *
 * The named colors are the 148 keywords of the served draft's table (retrieved 2026-10-22; the table is
 * the one of CSS Color 3 and `rebeccapurple`), sorted by name so that a lookup is a bisection.
 * The system colors are the draft's list of `<system-color>` and, after them, its `<deprecated-color>`.
 * A system color's value is the user agent's and the environment's, so this table holds only the names.
 */

#include <string.h>

#include "css_color_internal.h"

static const struct { const char * name; uint32_t rgb; } NAMED[] = {
  {"aliceblue", 0xF0F8FF},
  {"antiquewhite", 0xFAEBD7},
  {"aqua", 0x00FFFF},
  {"aquamarine", 0x7FFFD4},
  {"azure", 0xF0FFFF},
  {"beige", 0xF5F5DC},
  {"bisque", 0xFFE4C4},
  {"black", 0x000000},
  {"blanchedalmond", 0xFFEBCD},
  {"blue", 0x0000FF},
  {"blueviolet", 0x8A2BE2},
  {"brown", 0xA52A2A},
  {"burlywood", 0xDEB887},
  {"cadetblue", 0x5F9EA0},
  {"chartreuse", 0x7FFF00},
  {"chocolate", 0xD2691E},
  {"coral", 0xFF7F50},
  {"cornflowerblue", 0x6495ED},
  {"cornsilk", 0xFFF8DC},
  {"crimson", 0xDC143C},
  {"cyan", 0x00FFFF},
  {"darkblue", 0x00008B},
  {"darkcyan", 0x008B8B},
  {"darkgoldenrod", 0xB8860B},
  {"darkgray", 0xA9A9A9},
  {"darkgreen", 0x006400},
  {"darkgrey", 0xA9A9A9},
  {"darkkhaki", 0xBDB76B},
  {"darkmagenta", 0x8B008B},
  {"darkolivegreen", 0x556B2F},
  {"darkorange", 0xFF8C00},
  {"darkorchid", 0x9932CC},
  {"darkred", 0x8B0000},
  {"darksalmon", 0xE9967A},
  {"darkseagreen", 0x8FBC8F},
  {"darkslateblue", 0x483D8B},
  {"darkslategray", 0x2F4F4F},
  {"darkslategrey", 0x2F4F4F},
  {"darkturquoise", 0x00CED1},
  {"darkviolet", 0x9400D3},
  {"deeppink", 0xFF1493},
  {"deepskyblue", 0x00BFFF},
  {"dimgray", 0x696969},
  {"dimgrey", 0x696969},
  {"dodgerblue", 0x1E90FF},
  {"firebrick", 0xB22222},
  {"floralwhite", 0xFFFAF0},
  {"forestgreen", 0x228B22},
  {"fuchsia", 0xFF00FF},
  {"gainsboro", 0xDCDCDC},
  {"ghostwhite", 0xF8F8FF},
  {"gold", 0xFFD700},
  {"goldenrod", 0xDAA520},
  {"gray", 0x808080},
  {"green", 0x008000},
  {"greenyellow", 0xADFF2F},
  {"grey", 0x808080},
  {"honeydew", 0xF0FFF0},
  {"hotpink", 0xFF69B4},
  {"indianred", 0xCD5C5C},
  {"indigo", 0x4B0082},
  {"ivory", 0xFFFFF0},
  {"khaki", 0xF0E68C},
  {"lavender", 0xE6E6FA},
  {"lavenderblush", 0xFFF0F5},
  {"lawngreen", 0x7CFC00},
  {"lemonchiffon", 0xFFFACD},
  {"lightblue", 0xADD8E6},
  {"lightcoral", 0xF08080},
  {"lightcyan", 0xE0FFFF},
  {"lightgoldenrodyellow", 0xFAFAD2},
  {"lightgray", 0xD3D3D3},
  {"lightgreen", 0x90EE90},
  {"lightgrey", 0xD3D3D3},
  {"lightpink", 0xFFB6C1},
  {"lightsalmon", 0xFFA07A},
  {"lightseagreen", 0x20B2AA},
  {"lightskyblue", 0x87CEFA},
  {"lightslategray", 0x778899},
  {"lightslategrey", 0x778899},
  {"lightsteelblue", 0xB0C4DE},
  {"lightyellow", 0xFFFFE0},
  {"lime", 0x00FF00},
  {"limegreen", 0x32CD32},
  {"linen", 0xFAF0E6},
  {"magenta", 0xFF00FF},
  {"maroon", 0x800000},
  {"mediumaquamarine", 0x66CDAA},
  {"mediumblue", 0x0000CD},
  {"mediumorchid", 0xBA55D3},
  {"mediumpurple", 0x9370DB},
  {"mediumseagreen", 0x3CB371},
  {"mediumslateblue", 0x7B68EE},
  {"mediumspringgreen", 0x00FA9A},
  {"mediumturquoise", 0x48D1CC},
  {"mediumvioletred", 0xC71585},
  {"midnightblue", 0x191970},
  {"mintcream", 0xF5FFFA},
  {"mistyrose", 0xFFE4E1},
  {"moccasin", 0xFFE4B5},
  {"navajowhite", 0xFFDEAD},
  {"navy", 0x000080},
  {"oldlace", 0xFDF5E6},
  {"olive", 0x808000},
  {"olivedrab", 0x6B8E23},
  {"orange", 0xFFA500},
  {"orangered", 0xFF4500},
  {"orchid", 0xDA70D6},
  {"palegoldenrod", 0xEEE8AA},
  {"palegreen", 0x98FB98},
  {"paleturquoise", 0xAFEEEE},
  {"palevioletred", 0xDB7093},
  {"papayawhip", 0xFFEFD5},
  {"peachpuff", 0xFFDAB9},
  {"peru", 0xCD853F},
  {"pink", 0xFFC0CB},
  {"plum", 0xDDA0DD},
  {"powderblue", 0xB0E0E6},
  {"purple", 0x800080},
  {"rebeccapurple", 0x663399},
  {"red", 0xFF0000},
  {"rosybrown", 0xBC8F8F},
  {"royalblue", 0x4169E1},
  {"saddlebrown", 0x8B4513},
  {"salmon", 0xFA8072},
  {"sandybrown", 0xF4A460},
  {"seagreen", 0x2E8B57},
  {"seashell", 0xFFF5EE},
  {"sienna", 0xA0522D},
  {"silver", 0xC0C0C0},
  {"skyblue", 0x87CEEB},
  {"slateblue", 0x6A5ACD},
  {"slategray", 0x708090},
  {"slategrey", 0x708090},
  {"snow", 0xFFFAFA},
  {"springgreen", 0x00FF7F},
  {"steelblue", 0x4682B4},
  {"tan", 0xD2B48C},
  {"teal", 0x008080},
  {"thistle", 0xD8BFD8},
  {"tomato", 0xFF6347},
  {"turquoise", 0x40E0D0},
  {"violet", 0xEE82EE},
  {"wheat", 0xF5DEB3},
  {"white", 0xFFFFFF},
  {"whitesmoke", 0xF5F5F5},
  {"yellow", 0xFFFF00},
  {"yellowgreen", 0x9ACD32},
};

static const char * const SYSTEM[] = {
  "AccentColor",
  "AccentColorText",
  "ActiveText",
  "ButtonBorder",
  "ButtonFace",
  "ButtonText",
  "Canvas",
  "CanvasText",
  "Field",
  "FieldText",
  "GrayText",
  "Highlight",
  "HighlightText",
  "LinkText",
  "Mark",
  "MarkText",
  "SelectedItem",
  "SelectedItemText",
  "VisitedText",
};

static const char * const DEPRECATED[] = {
  "ActiveBorder",
  "ActiveCaption",
  "AppWorkspace",
  "Background",
  "ButtonHighlight",
  "ButtonShadow",
  "CaptionText",
  "InactiveBorder",
  "InactiveCaption",
  "InactiveCaptionText",
  "InfoBackground",
  "InfoText",
  "Menu",
  "MenuText",
  "Scrollbar",
  "ThreeDDarkShadow",
  "ThreeDFace",
  "ThreeDHighlight",
  "ThreeDLightShadow",
  "ThreeDShadow",
  "Window",
  "WindowFrame",
  "WindowText",
};

static int fold_cmp(const char * a, size_t alen, const char * b) {
  /* ASCII case-insensitive compare of a (alen bytes) with the lower-case b */
  size_t i = 0;
  for (; i < alen && b[i]; i++) {
    char x = a[i];
    if (x >= 'A' && x <= 'Z') {
      x = (char)(x + 32);
    }
    const char y = (char)((b[i] >= 'A' && b[i] <= 'Z') ? b[i] + 32 : b[i]);
    if (x != y) {
      return (unsigned char)x < (unsigned char)y ? -1 : 1;
    }
  }
  if (i < alen) {
    return 1;
  }
  return b[i] ? -1 : 0;
}

bool css_color_named_lookup(const char * name, size_t length, uint32_t * rgb, const char ** canonical) {
  size_t lo = 0, hi = sizeof NAMED / sizeof NAMED[0];
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const int c = fold_cmp(name, length, NAMED[mid].name);
    if (c == 0) {
      *rgb = NAMED[mid].rgb;
      *canonical = NAMED[mid].name;
      return true;
    }
    if (c < 0) {
      hi = mid;
    }
    else {
      lo = mid + 1;
    }
  }
  return false;
}

/// 1 for a system color, 2 for a deprecated one, 0 for neither; `lower` receives the keyword in lower case.
int css_color_system_lookup(const char * name, size_t length, char * lower, size_t cap) {
  for (size_t i = 0; i < sizeof SYSTEM / sizeof SYSTEM[0]; i++) {
    if (fold_cmp(name, length, SYSTEM[i]) == 0 && strlen(SYSTEM[i]) + 1 <= cap) {
      for (size_t k = 0; k <= length; k++) {
        lower[k] = k < length ? (char)((name[k] >= 'A' && name[k] <= 'Z') ? name[k] + 32 : name[k]) : '\0';
      }
      return 1;
    }
  }
  for (size_t i = 0; i < sizeof DEPRECATED / sizeof DEPRECATED[0]; i++) {
    if (fold_cmp(name, length, DEPRECATED[i]) == 0 && strlen(DEPRECATED[i]) + 1 <= cap) {
      for (size_t k = 0; k <= length; k++) {
        lower[k] = k < length ? (char)((name[k] >= 'A' && name[k] <= 'Z') ? name[k] + 32 : name[k]) : '\0';
      }
      return 2;
    }
  }
  return 0;
}

size_t gtext_css_color_named_count(void) {
  return sizeof NAMED / sizeof NAMED[0];
}

bool gtext_css_color_named_at(size_t index, const char ** name, uint32_t * rgb) {
  if (index >= sizeof NAMED / sizeof NAMED[0] || !name || !rgb) {
    return false;
  }
  *name = NAMED[index].name;
  *rgb = NAMED[index].rgb;
  return true;
}
