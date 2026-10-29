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
 * This library's side of the condition and rule differentials (tools/oracle/css_cond_diff.py): what `matchMedia()`, `CSS.supports()` and `CSSStyleSheet.insertRule()`
 * answer, asked of the conditions object and the at-rule model.
 *
 * Standard input is lines, a field separated by a tab, a tab, a line feed and a backslash written `\t`, `\n` and `\\`:
 *
 *   E name value        one feature of the environment (before the first D): the browser's own measured values (the driver reads them from the page)
 *   D w h dpr           a document: the viewport (CSS px) and the device pixel ratio the query is asked in
 *   M text              matchMedia(text): "M 1|0|E <media>"
 *   S text              CSS.supports(conditionText): "S 1|0|E"
 *   P property value    CSS.supports(property, value): "P 1|0|E"
 *   R text              the conditionText of `@supports <text> {}` inserted by insertRule: "R ok|reject <serialization>"
 *   A text              insertRule(text) of one at-rule: "A ok|reject <serialization>"
 *   X                   end of the document; answered "X"
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <ghoti.io/text/css.h>

namespace {

GTEXT_Markup_Status collect(void * user, const char * bytes, size_t len) {
  static_cast<std::string *>(user)->append(bytes, len);
  return GTEXT_MARKUP_OK;
}

std::string esc(const std::string & s) {
  std::string o;
  for (char c : s) {
    if (c == '\\') {
      o += "\\\\";
    } else if (c == '\n') {
      o += "\\n";
    } else if (c == '\t') {
      o += "\\t";
    } else {
      o += c;
    }
  }
  return o;
}

std::string unesc(const std::string & s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      i++;
      o += s[i] == 'n' ? '\n' : (s[i] == 't' ? '\t' : s[i]);
    } else {
      o += s[i];
    }
  }
  return o;
}

std::vector<std::string> split(const std::string & line) {
  std::vector<std::string> out;
  size_t a = 0;
  for (;;) {
    size_t b = line.find('\t', a);
    out.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
    if (b == std::string::npos) {
      break;
    }
    a = b + 1;
  }
  return out;
}

void set_keyword(GTEXT_CSS_Media_Environment * e, const std::string & k, const std::string & v) {
  auto is = [&](const char * x) { return v == x; };
  if (k == "pointer") {
    e->pointer = is("none") ? GTEXT_CSS_MQ_POINTER_NONE : (is("coarse") ? GTEXT_CSS_MQ_POINTER_COARSE : GTEXT_CSS_MQ_POINTER_FINE);
  } else if (k == "hover") {
    e->hover = is("none") ? GTEXT_CSS_MQ_HOVER_NONE : GTEXT_CSS_MQ_HOVER_HOVER;
  } else if (k == "any-pointer") {
    e->any_pointer = static_cast<uint8_t>(std::atoi(v.c_str()));
  } else if (k == "any-hover") {
    e->any_hover = static_cast<uint8_t>(std::atoi(v.c_str()));
  } else if (k == "transform-3d") {
    e->transform_3d = std::atoi(v.c_str()) != 0;
  } else if (k == "prefers-color-scheme") {
    e->prefers_color_scheme = is("dark") ? GTEXT_CSS_MQ_SCHEME_DARK : GTEXT_CSS_MQ_SCHEME_LIGHT;
  } else if (k == "prefers-reduced-motion") {
    e->prefers_reduced_motion = is("reduce") ? GTEXT_CSS_MQ_REDUCE_REDUCE : GTEXT_CSS_MQ_REDUCE_NO_PREFERENCE;
  } else if (k == "prefers-reduced-transparency") {
    e->prefers_reduced_transparency = is("reduce") ? GTEXT_CSS_MQ_REDUCE_REDUCE : GTEXT_CSS_MQ_REDUCE_NO_PREFERENCE;
  } else if (k == "prefers-reduced-data") {
    e->prefers_reduced_data = is("reduce") ? GTEXT_CSS_MQ_REDUCE_REDUCE : GTEXT_CSS_MQ_REDUCE_NO_PREFERENCE;
  } else if (k == "prefers-contrast") {
    e->prefers_contrast = is("more") ? GTEXT_CSS_MQ_CONTRAST_MORE : (is("less") ? GTEXT_CSS_MQ_CONTRAST_LESS : (is("custom") ? GTEXT_CSS_MQ_CONTRAST_CUSTOM : GTEXT_CSS_MQ_CONTRAST_NO_PREFERENCE));
  } else if (k == "color") {
    e->color_bits = static_cast<uint32_t>(std::atoi(v.c_str()));
  } else if (k == "color-gamut") {
    e->color_gamut = is("p3") ? GTEXT_CSS_MQ_GAMUT_P3 : (is("rec2020") ? GTEXT_CSS_MQ_GAMUT_REC2020 : (is("srgb") ? GTEXT_CSS_MQ_GAMUT_SRGB : GTEXT_CSS_MQ_GAMUT_NONE));
  } else if (k == "dynamic-range") {
    e->dynamic_range = is("high") ? 1 : 0;
  } else if (k == "display-mode") {
    e->display_mode = is("standalone") ? GTEXT_CSS_MQ_DISPLAY_MODE_STANDALONE : (is("fullscreen") ? GTEXT_CSS_MQ_DISPLAY_MODE_FULLSCREEN
                      : (is("minimal-ui") ? GTEXT_CSS_MQ_DISPLAY_MODE_MINIMAL_UI : (is("browser") ? GTEXT_CSS_MQ_DISPLAY_MODE_BROWSER : GTEXT_CSS_MQ_DISPLAY_MODE_PICTURE_IN_PICTURE)));
  } else if (k == "forced-colors") {
    e->forced_colors = is("active");
  } else if (k == "inverted-colors") {
    e->inverted_colors = is("inverted");
  } else if (k == "scripting") {
    e->scripting = is("none") ? GTEXT_CSS_MQ_SCRIPTING_NONE : (is("initial-only") ? GTEXT_CSS_MQ_SCRIPTING_INITIAL_ONLY : GTEXT_CSS_MQ_SCRIPTING_ENABLED);
  } else if (k == "update") {
    e->update = is("none") ? GTEXT_CSS_MQ_UPDATE_NONE : (is("slow") ? GTEXT_CSS_MQ_UPDATE_SLOW : GTEXT_CSS_MQ_UPDATE_FAST);
  } else if (k == "overflow-block") {
    e->overflow_block = is("none") ? GTEXT_CSS_MQ_OVERFLOW_BLOCK_NONE : (is("scroll") ? GTEXT_CSS_MQ_OVERFLOW_BLOCK_SCROLL : GTEXT_CSS_MQ_OVERFLOW_BLOCK_PAGED);
  } else if (k == "overflow-inline") {
    e->overflow_inline = is("none") ? GTEXT_CSS_MQ_OVERFLOW_INLINE_NONE : GTEXT_CSS_MQ_OVERFLOW_INLINE_SCROLL;
  } else if (k == "scan") {
    e->scan = is("interlace") ? GTEXT_CSS_MQ_SCAN_INTERLACE : (is("progressive") ? GTEXT_CSS_MQ_SCAN_PROGRESSIVE : GTEXT_CSS_MQ_SCAN_NONE);
  } else if (k == "monochrome") {
    e->monochrome_bits = static_cast<uint32_t>(std::atoi(v.c_str()));
  } else if (k == "color-index") {
    e->color_index = static_cast<uint32_t>(std::atoi(v.c_str()));
  } else if (k == "grid") {
    e->grid = std::atoi(v.c_str()) != 0;
  } else if (k == "horizontal-viewport-segments") {
    e->horizontal_viewport_segments = static_cast<uint32_t>(std::atoi(v.c_str()));
  } else if (k == "vertical-viewport-segments") {
    e->vertical_viewport_segments = static_cast<uint32_t>(std::atoi(v.c_str()));
  } else if (k == "device-width") {
    e->device.width = std::atof(v.c_str());
  } else if (k == "device-height") {
    e->device.height = std::atof(v.c_str());
  } else if (k == "unsupported") {
    size_t a = 0;
    while (a <= v.size()) {
      size_t b = v.find(',', a);
      const std::string name = v.substr(a, b == std::string::npos ? std::string::npos : b - a);
      if (!name.empty()) {
        gtext_css_media_environment_set_supported(e, name.c_str(), false);
      }
      if (b == std::string::npos) {
        break;
      }
      a = b + 1;
    }
  }
}

uint32_t bits_of(const std::string & list, const char * const * names, const uint32_t * bits, size_t n) {
  uint32_t out = 0;
  size_t a = 0;
  while (a <= list.size()) {
    size_t b = list.find(',', a);
    const std::string name = list.substr(a, b == std::string::npos ? std::string::npos : b - a);
    for (size_t i = 0; i < n; i++) {
      if (name == names[i]) {
        out |= bits[i];
      }
    }
    if (b == std::string::npos) {
      break;
    }
    a = b + 1;
  }
  return out;
}

std::string serialize_rule(const GTEXT_CSS_Atrule * a) {
  std::string out;
  GTEXT_Markup_Sink sk{collect, &out};
  gtext_css_atrule_serialize(a, &sk);
  return out;
}

}  // namespace

int main() {
  GTEXT_CSS_Registry * reg = nullptr;
  if (gtext_css_registry_new(nullptr, &reg) != GTEXT_CSS_OK) {
    std::fprintf(stderr, "registry\n");
    return 2;
  }
  std::vector<std::pair<std::string, std::string>> envs;
  static const char * const tech_names[] = {"features-opentype", "features-aat", "features-graphite", "color-colrv0", "color-colrv1", "color-svg", "color-sbix", "color-cbdt", "variations", "palettes", "incremental"};
  static const uint32_t tech_bits[] = {GTEXT_CSS_FONT_TECH_FEATURES_OPENTYPE, GTEXT_CSS_FONT_TECH_FEATURES_AAT, GTEXT_CSS_FONT_TECH_FEATURES_GRAPHITE, GTEXT_CSS_FONT_TECH_COLOR_COLRV0,
      GTEXT_CSS_FONT_TECH_COLOR_COLRV1, GTEXT_CSS_FONT_TECH_COLOR_SVG, GTEXT_CSS_FONT_TECH_COLOR_SBIX, GTEXT_CSS_FONT_TECH_COLOR_CBDT, GTEXT_CSS_FONT_TECH_VARIATIONS,
      GTEXT_CSS_FONT_TECH_PALETTES, GTEXT_CSS_FONT_TECH_INCREMENTAL};
  static const char * const format_names[] = {"collection", "embedded-opentype", "opentype", "svg", "truetype", "woff", "woff2"};
  static const uint32_t format_bits[] = {GTEXT_CSS_FONT_FORMAT_COLLECTION, GTEXT_CSS_FONT_FORMAT_EMBEDDED_OPENTYPE, GTEXT_CSS_FONT_FORMAT_OPENTYPE, GTEXT_CSS_FONT_FORMAT_SVG,
      GTEXT_CSS_FONT_FORMAT_TRUETYPE, GTEXT_CSS_FONT_FORMAT_WOFF, GTEXT_CSS_FONT_FORMAT_WOFF2};
  GTEXT_CSS_Support_Policy policy;
  std::memset(&policy, 0, sizeof policy);
  GTEXT_CSS_Conditions * conds = nullptr;
  std::string line;
  while (std::getline(std::cin, line)) {
    const std::vector<std::string> f = split(line);
    const std::string & kind = f[0];
    if (kind == "E" && f.size() >= 3) {
      if (f[1] == "font-tech") {
        policy.font_tech = bits_of(f[2], tech_names, tech_bits, sizeof tech_names / sizeof *tech_names);
      } else if (f[1] == "font-format") {
        policy.font_format = bits_of(f[2], format_names, format_bits, sizeof format_names / sizeof *format_names);
      } else {
        envs.emplace_back(f[1], f[2]);
      }
    } else if (kind == "D" && f.size() >= 4) {
      gtext_css_conditions_free(conds);
      conds = nullptr;
      GTEXT_CSS_Media_Environment env = gtext_css_media_environment_default(nullptr);
      env.viewport.width = std::atof(f[1].c_str());
      env.viewport.height = std::atof(f[2].c_str());
      env.resolution = std::atof(f[3].c_str());
      for (const auto & kv : envs) {
        set_keyword(&env, kv.first, kv.second);
      }
      GTEXT_CSS_Conditions_Options o = gtext_css_conditions_options_default();
      o.media = &env;
      o.custom_media = false; /* no browser ships @custom-media */
      o.policy = &policy;
      if (gtext_css_conditions_new(reg, &o, &conds) != GTEXT_CSS_OK) {
        std::fprintf(stderr, "conditions\n");
        return 2;
      }
    } else if (kind == "X") {
      std::puts("X");
    } else if (kind == "M" && f.size() >= 2) {
      const std::string text = unesc(f[1]);
      GTEXT_CSS_Media_List * l = nullptr;
      GTEXT_CSS_Status s = gtext_css_media_list_parse(text.data(), text.size(), nullptr, &l, nullptr);
      bool m = false;
      std::string ser;
      if (s == GTEXT_CSS_OK) {
        GTEXT_Markup_Sink sk{collect, &ser};
        gtext_css_media_list_serialize(l, &sk);
        s = gtext_css_conditions_matches_list(conds, l, nullptr, &m);
        gtext_css_media_list_free(l);
      }
      std::printf("M\t%s\t%s\n", s == GTEXT_CSS_OK ? (m ? "1" : "0") : "E", esc(ser).c_str());
    } else if (kind == "S" && f.size() >= 2) {
      const std::string text = unesc(f[1]);
      bool r = false;
      const GTEXT_CSS_Status s = gtext_css_conditions_supports_text(conds, text.data(), text.size(), &r);
      std::printf("S\t%s\n", s == GTEXT_CSS_OK ? (r ? "1" : "0") : "E");
    } else if (kind == "P" && f.size() >= 3) {
      const std::string prop = unesc(f[1]), val = unesc(f[2]);
      bool r = false;
      const GTEXT_CSS_Status s = gtext_css_conditions_supports(conds, prop.c_str(), val.c_str(), &r);
      std::printf("P\t%s\n", s == GTEXT_CSS_OK ? (r ? "1" : "0") : "E");
    } else if ((kind == "R" || kind == "A") && f.size() >= 2) {
      std::string text = unesc(f[1]);
      if (kind == "R") {
        text = "@supports " + text + " {}";
      }
      GTEXT_CSS_Tree * tree = nullptr;
      GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
      GTEXT_CSS_Result pr;
      std::memset(&pr, 0, sizeof pr);
      GTEXT_CSS_Status s = gtext_css_parse_stylesheet(text.data(), text.size(), &po, &tree, &pr);
      std::string ser;
      const char * verdict = "reject";
      /* insertRule takes exactly one rule */
      if (s == GTEXT_CSS_OK && tree && tree->rule_count == 1 && tree->rules[0].kind == GTEXT_CSS_RULE_AT) {
        GTEXT_CSS_Atrule_Options ao = gtext_css_atrule_options_default();
        GTEXT_CSS_Atrule * a = nullptr;
        GTEXT_CSS_Atrule_Result res;
        std::memset(&res, 0, sizeof res);
        s = gtext_css_atrule_parse(reg, &tree->rules[0], &ao, &a, &res);
        if (s == GTEXT_CSS_OK && res.valid && a) {
          verdict = "ok";
          ser = serialize_rule(a);
        }
        gtext_css_atrule_free(a);
      }
      gtext_css_tree_free(tree);
      gtext_css_result_free(&pr);
      std::printf("%s\t%s\t%s\n", kind.c_str(), verdict, esc(ser).c_str());
    }
    std::fflush(stdout);
  }
  gtext_css_conditions_free(conds);
  return 0;
}
