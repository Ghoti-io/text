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

// This library's side of the computed-style differential (tools/oracle/css_style_diff.py): the computed style of the elements of a document, as
// gtext_css_style_compute() makes it, written as the CSSOM would resolve it where the specification gives a resolved value of its own (css_resolved.h).
//
//   css-style-ours [--metrics FILE] [--dark]            documents and questions on standard input, answers on standard output
//   css-style-ours --properties                         name, computed class and whether the property is inherited, one a line
//   css-style-ours --document FILE.svg                  the full dump of every element of a document with an id (what the fixtures expect)
//
// Input, one tab-separated line each (a field escapes backslash, line feed and tab as \\, \n, \t):
//
//   D mode TEXT      a document: mode h (HTML) or x (XML, for SVG), the text
//   Q id p1,p2,...   the properties of the element with that id (or instance path) to answer
//   X                compute the document and answer every Q since the last X
//
// Output, one line for each (element, property): `id TAB property TAB OK TAB value`, `... TAB NOELEMENT`, `... TAB UNSUPPORTED TAB name`, `... TAB ERR TAB status`
// or `... TAB NOVALUE`. A property of a shorthand answers SHORTHAND. The font metrics (`ex`, `ch`, `cap`, `ic`) are the table of --metrics, which the
// browsers measured: family weight italic size ex ch cap ic, one line each; a font that is not in it makes the element's computation UNSUPPORTED, naming the unit.
//
// The registry is the generated one with the grammars of `fill`, `stroke` and `glyph-orientation-vertical` that an SVG consumer registers
// (tests/css_svg_registry.h). The environment is a 1280 by 800 viewport, `medium` 16px, a device pixel ratio of 1.

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "../../tests/css_markup_tree.h"
#include "css_resolved.h"
#include "../../tests/css_svg_registry.h"

namespace {
using resolved_value::resolved;
using resolved_value::serialize;

std::string unesc(const std::string & s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      const char c = s[++i];
      out += c == 'n' ? '\n' : c == 't' ? '\t' : c;
    }
    else {
      out += s[i];
    }
  }
  return out;
}

std::string esc(const std::string & s) {
  std::string out;
  for (char c : s) {
    out += c == '\\' ? "\\\\" : c == '\n' ? "\\n" : c == '\t' ? "\\t" : std::string(1, c);
  }
  return out;
}

std::vector<std::string> split(const std::string & s, char sep) {
  std::vector<std::string> out;
  size_t pos = 0;
  for (;;) {
    const size_t at = s.find(sep, pos);
    out.push_back(s.substr(pos, at == std::string::npos ? std::string::npos : at - pos));
    if (at == std::string::npos) {
      break;
    }
    pos = at + 1;
  }
  return out;
}

// ---- the font metrics the browsers measured -------------------------------------------------------------------------------

struct MetricKey {
  std::string family;
  int weight;
  bool italic;
  int size;
  bool operator<(const MetricKey & o) const {
    return std::tie(family, weight, italic, size) < std::tie(o.family, o.weight, o.italic, o.size);
  }
};

std::map<MetricKey, std::array<double, 4>> g_metrics;

bool first_family(const GTEXT_CSS_Value * family, std::string * out) {
  if (!family) {
    return false;
  }
  const GTEXT_CSS_Value * v = family;
  if (v->kind == GTEXT_CSS_VAL_LIST && v->child_count > 0) {
    v = &v->children[0];
  }
  char * text = nullptr;
  size_t n = 0;
  if (gtext_css_serialize_value_buffer(v, nullptr, &text, &n) != GTEXT_CSS_OK) {
    return false;
  }
  out->assign(text, n);
  gtext_css_serialize_buffer_free(nullptr, text);
  return true;
}

bool font_metric(void *, const GTEXT_CSS_Font_Query * q, GTEXT_CSS_Font_Metric metric, double * px) {
  std::string fam;
  if (!first_family(q->family, &fam)) {
    return false;
  }
  const MetricKey key{fam, (int)std::lround(q->weight), q->italic, (int)std::lround(q->size_px)};
  if (std::fabs(q->size_px - key.size) > 1e-9) {
    return false;
  }
  auto it = g_metrics.find(key);
  if (it == g_metrics.end()) {
    return false;
  }
  *px = it->second[(int)metric];
  return true;
}

void load_metrics(const char * path) {
  FILE * f = std::fopen(path, "r");
  if (!f) {
    std::fprintf(stderr, "css-style-ours: cannot read %s\n", path);
    std::exit(2);
  }
  char line[512];
  while (std::fgets(line, sizeof line, f)) {
    if (line[0] == '#' || line[0] == '\n') {
      continue;
    }
    char fam[128];
    int weight, italic, size;
    double ex, ch, cap, ic;
    if (std::sscanf(line, "%127[^\t]\t%d\t%d\t%d\t%lf\t%lf\t%lf\t%lf", fam, &weight, &italic, &size, &ex, &ch, &cap, &ic) == 8) {
      g_metrics[MetricKey{fam, weight, italic != 0, size}] = {ex, ch, cap, ic};
    }
  }
  std::fclose(f);
}

// ---- a document ------------------------------------------------------------------------------------------------------------

struct Result {
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  std::string name;
  bool inherited_failure = false;
};

struct Question {
  std::string id;
  std::vector<std::string> properties;
};

/// The color scheme the system colors of an element are resolved for: the used color scheme of CSS Color Adjust 1 (§ 2.3) of its computed `color-scheme`,
/// with the environment preferring dark: `normal` is the UA default (light), a list that holds `dark` is dark, any other list its first scheme.
static bool element_is_dark(const GTEXT_CSS_Computed_Style * style) {
  const GTEXT_CSS_Property_Id id = gtext_css_registry_find_property(svgreg::shared(), "color-scheme", 12);
  std::string text;
  GTEXT_CSS_Compute_Result gr;
  if (id == 0 || resolved_value::read(svgreg::shared(), style, id, &text, &gr) != GTEXT_CSS_OK) {
    return false;
  }
  bool first_dark = false, have = false, has_dark = false;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && text[i] == ' ') {
      i++;
    }
    size_t j = i;
    while (j < text.size() && text[j] != ' ') {
      j++;
    }
    const std::string w = text.substr(i, j - i);
    if (w == "light" || w == "dark") {
      if (!have) {
        first_dark = w == "dark";
        have = true;
      }
      has_dark = has_dark || w == "dark";
    }
    i = j;
  }
  return have && (has_dark || first_dark);
}

static std::string lower(std::string s) {
  for (char & c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = (char)(c + 32);
    }
  }
  return s;
}

/// The CSSOM resolved value of a color that is `currentcolor` is the used color, the element's own `color`: a computed value keeps the keyword (Color 4), and
/// the browsers' `getComputedStyle()` answers the resolved one.
static std::string current_color_resolved(const GTEXT_CSS_Computed_Style * style, const std::string & text) {
  const GTEXT_CSS_Property_Id id = gtext_css_registry_find_property(svgreg::shared(), "color", 5);
  std::string color;
  GTEXT_CSS_Compute_Result gr;
  if (id == 0 || resolved_value::read(svgreg::shared(), style, id, &color, &gr) != GTEXT_CSS_OK || lower(color) == "currentcolor") {
    return text;
  }
  std::string out;
  const std::string l = lower(text);
  size_t i = 0;
  while (i < text.size()) {
    const size_t at = l.find("currentcolor", i);
    const bool word = at != std::string::npos && (at == 0 || !(std::isalnum((unsigned char)l[at - 1]) || l[at - 1] == '-')) &&
        (at + 12 >= l.size() || !(std::isalnum((unsigned char)l[at + 12]) || l[at + 12] == '-'));
    if (at == std::string::npos) {
      out += text.substr(i);
      break;
    }
    out += text.substr(i, at - i);
    out += word ? color : text.substr(at, 12);
    i = at + 12;
  }
  return out;
}

void answer(const std::string & mode, const std::string & text, const std::vector<Question> & questions, bool dark, const std::string & only_dump) {
  cmt::Tree tree;
  if (!tree.parse(text, mode == "h")) {
    for (const Question & q : questions) {
      for (const std::string & p : q.properties) {
        std::printf("%s\t%s\tERR\tparse\n", esc(q.id).c_str(), esc(p).c_str());
      }
    }
    return;
  }
  tree.build();
  GTEXT_CSS_Environment env = gtext_css_environment_default();
  env.font_metric = font_metric;
  if (dark) {
    env.color_scheme = GTEXT_CSS_SCHEME_DARK;
  }
  resolved_value::dark_scheme() = dark;
  GTEXT_CSS_Compute_Options co = gtext_css_compute_options_default();
  co.elements = &tree.table;
  co.max_element_depth = 100000;
  GTEXT_CSS_Compute_Context * ctx = nullptr;
  GTEXT_CSS_Compute_Result cr;
  if (gtext_css_compute_context_new(svgreg::shared(), &env, &co, &ctx, &cr) != GTEXT_CSS_OK) {
    std::fprintf(stderr, "css-style-ours: no context: %s\n", cr.name);
    std::exit(2);
  }
  GTEXT_CSS_Style_Options so = gtext_css_style_options_default();
  std::map<const cmt::Inst *, Result> results;
  for (cmt::Inst * i : tree.order) {
    Result & r = results[i];
    if (i->inherit && results[i->inherit].status != GTEXT_CSS_OK) {
      r.status = GTEXT_CSS_E_UNSUPPORTED;
      r.inherited_failure = true;
      r.name = results[i->inherit].name;
      continue;
    }
    GTEXT_CSS_Computed_Style * s = nullptr;
    GTEXT_CSS_Compute_Result res;
    /* the user agent's style sheet of an HTML element: what the browsers' default sheets agree on for the elements of the population */
    const char * ns = gtext_markup_element_namespace(i->node);
    const char * local = gtext_markup_element_local_name(i->node);
    const bool html = ns && !std::strcmp(ns, "http://www.w3.org/1999/xhtml");
    const char * ua = html && local && (!std::strcmp(local, "html") || !std::strcmp(local, "body") || !std::strcmp(local, "div") || !std::strcmp(local, "p") ||
                                        !std::strcmp(local, "section")) ? "display: block" : "";
    GTEXT_CSS_Style_Entry entry;
    std::memset(&entry, 0, sizeof entry);
    entry.keys.origin = GTEXT_CSS_ORIGIN_USER_AGENT;
    entry.text = {ua, std::strlen(ua)};
    const GTEXT_CSS_Status st = gtext_css_style_compute(ctx, i, i->inherit ? i->inherit->style : nullptr, &entry, 1, &so, &s, &res);
    r.status = st;
    if (st != GTEXT_CSS_OK) {
      r.name = res.name;
      continue;
    }
    i->style = s;
  }
  if (!only_dump.empty()) {
    for (cmt::Inst * i : tree.order) {
      if (i->id.empty() && !i->clone) {
        continue;
      }
      std::printf("## %s\n", i->path.c_str());
      if (i->style) {
        GTEXT_Markup_Sink sink;
        std::memset(&sink, 0, sizeof sink);
        sink.user = nullptr;
        sink.write = [](void *, const char * b, size_t n) -> GTEXT_Markup_Status {
          std::fwrite(b, 1, n, stdout);
          return GTEXT_MARKUP_OK;
        };
        gtext_css_computed_dump(i->style, &sink);
      }
    }
  }
  for (const Question & q : questions) {
    cmt::Inst * inst = tree.find(q.id);
    for (const std::string & p : q.properties) {
      if (!inst) {
        std::printf("%s\t%s\tNOELEMENT\n", esc(q.id).c_str(), esc(p).c_str());
        continue;
      }
      const Result & r = results[inst];
      if (r.status != GTEXT_CSS_OK) {
        std::printf("%s\t%s\t%s\t%s\n", esc(q.id).c_str(), esc(p).c_str(), r.status == GTEXT_CSS_E_UNSUPPORTED ? "UNSUPPORTED" : "ERR",
            esc(r.status == GTEXT_CSS_E_UNSUPPORTED ? r.name : gtext_css_status_string(r.status)).c_str());
        continue;
      }
      if (p.rfind("--", 0) == 0) {
        const GTEXT_CSS_Custom_Value * cv = nullptr;
        if (!gtext_css_computed_custom_find(inst->style, p.data(), p.size(), &cv)) {
          std::printf("%s\t%s\tNOVALUE\n", esc(q.id).c_str(), esc(p).c_str());
          continue;
        }
        char * t = nullptr;
        size_t n = 0;
        gtext_css_custom_value_text(cv, nullptr, &t, &n);
        std::printf("%s\t%s\tOK\t%s\n", esc(q.id).c_str(), esc(p).c_str(), esc(std::string(t, n)).c_str());
        gtext_css_serialize_buffer_free(nullptr, t);
        continue;
      }
      const GTEXT_CSS_Property_Id id = gtext_css_registry_find_property(svgreg::shared(), p.data(), p.size());
      if (id == 0) {
        std::printf("%s\t%s\tUNKNOWN\n", esc(q.id).c_str(), esc(p).c_str());
        continue;
      }
      std::string text;
      GTEXT_CSS_Compute_Result gr;
      resolved_value::dark_scheme() = dark && element_is_dark(inst->style);
      const GTEXT_CSS_Status st = resolved_value::read(svgreg::shared(), inst->style, id, &text, &gr);
      if (st == GTEXT_CSS_E_UNSUPPORTED) {
        std::printf("%s\t%s\tUNSUPPORTED\t%s\n", esc(q.id).c_str(), esc(p).c_str(), esc(gr.name).c_str());
      }
      else if (st != GTEXT_CSS_OK) {
        std::printf("%s\t%s\tERR\t%s\n", esc(q.id).c_str(), esc(p).c_str(), gtext_css_status_string(st));
      }
      else if (text.empty()) {
        std::printf("%s\t%s\tNOVALUE\n", esc(q.id).c_str(), esc(p).c_str());
      }
      else if (p != "color" && lower(text).find("currentcolor") != std::string::npos) {
        std::printf("%s\t%s\tOK\t%s\n", esc(q.id).c_str(), esc(p).c_str(), esc(current_color_resolved(inst->style, text)).c_str());
      }
      else {
        std::printf("%s\t%s\tOK\t%s\n", esc(q.id).c_str(), esc(p).c_str(), esc(text).c_str());
      }
    }
  }
  for (auto & inst : tree.all) {
    gtext_css_computed_free(inst->style);
    inst->style = nullptr;
  }
  gtext_css_compute_context_free(ctx);
}

} // namespace

int main(int argc, char ** argv) {
  bool dark = false;
  std::string document;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--metrics") && i + 1 < argc) {
      load_metrics(argv[++i]);
    }
    else if (!std::strcmp(argv[i], "--dark")) {
      dark = true;
    }
    else if (!std::strcmp(argv[i], "--document") && i + 1 < argc) {
      document = argv[++i];
    }
    else if (!std::strcmp(argv[i], "--properties")) {
      const GTEXT_CSS_Registry * r = svgreg::shared();
      const size_t n = gtext_css_registry_property_count(r);
      for (size_t id = 1; id <= n; id++) {
        GTEXT_CSS_Property_Info info;
        if (!gtext_css_registry_property_info(r, (GTEXT_CSS_Property_Id)id, &info)) {
          continue;
        }
        const GTEXT_CSS_Compute_Class c = gtext_css_compute_class(r, (GTEXT_CSS_Property_Id)id, nullptr, nullptr);
        std::printf("%.*s\t%s\t%d\t%d\n", (int)info.name_length, info.name, gtext_css_compute_class_name(c), info.inherited == GTEXT_CSS_INHERIT_YES,
            (info.flags & (GTEXT_CSS_PROP_SHORTHAND | GTEXT_CSS_PROP_ALIAS)) != 0);
      }
      return 0;
    }
    else {
      std::fprintf(stderr, "css-style-ours: unknown argument %s\n", argv[i]);
      return 2;
    }
  }
  if (!document.empty()) {
    FILE * f = std::fopen(document.c_str(), "rb");
    if (!f) {
      std::fprintf(stderr, "css-style-ours: cannot read %s\n", document.c_str());
      return 2;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
      text.append(buf, n);
    }
    std::fclose(f);
    answer(document.size() > 5 && document.compare(document.size() - 5, 5, ".html") == 0 ? "h" : "x", text, {}, dark, "dump");
    return 0;
  }
  std::string mode, text;
  std::vector<Question> questions;
  char * line = nullptr;
  size_t cap = 0;
  ssize_t len;
  while ((len = getline(&line, &cap, stdin)) >= 0) {
    std::string l(line, (size_t)len);
    if (!l.empty() && l.back() == '\n') {
      l.pop_back();
    }
    const std::vector<std::string> f = split(l, '\t');
    if (f[0] == "D" && f.size() == 3) {
      mode = f[1];
      text = unesc(f[2]);
      questions.clear();
    }
    else if (f[0] == "Q" && f.size() == 3) {
      Question q;
      q.id = unesc(f[1]);
      q.properties = split(unesc(f[2]), ',');
      questions.push_back(q);
    }
    else if (f[0] == "X") {
      answer(mode, text, questions, dark, "");
      std::printf("X\n");
      std::fflush(stdout);
      questions.clear();
    }
  }
  std::free(line);
  return 0;
}
