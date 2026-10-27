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

// The runner of the computed-value gate (tools/conformance/css_computed_suite.py): the question `test_computed_value(property, specified)` puts to `#target` of a page
// of web-platform-tests, put to this library. The page's document is read on standard input as data (tools/css/extract-wpt-computed-pages.py made it):
//
//   P TAB file TAB target-index        a page; the elements and the questions of it follow
//   E TAB index TAB parent TAB ns TAB local TAB attrs TAB rules
//                                      an element (ns h, s), its attributes (name=value joined by a record separator, escaped) and the rules that matched it
//                                      (a,b,c,order,declarations joined by a record separator)
//   T TAB n TAB property TAB value     `target.style[property] = value`, then `getComputedStyle(target)[property]`
//   X                                  end of the page: compute and answer
//
// and one line is answered for each T: `R TAB n TAB OK TAB text TAB again` (the computed value as `getComputedStyle()` writes it, and the same asked again with that
// text as the value, which the harness requires to give the same), `R TAB n TAB INVALID` (the value is not one of the property: the browser ignores the assignment),
// `R TAB n TAB UNSUPPORTED TAB name` or `R TAB n TAB ERR TAB status`. The user agent's style sheet of the page is a few rules: `display: block` for html, body,
// div and p, and the body's margin. The registry is the generated one with the SVG grammars of tests/css_svg_registry.h. The environment is a 1280 by 800
// viewport, `medium` 16px, a device pixel ratio of 1, with no font metrics (a page that needs one is UNSUPPORTED).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <ghoti.io/text/css.h>

#include "../../tests/css_svg_registry.h"
#include "../oracle/css_resolved.h"

namespace {

std::string unesc(const std::string & s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      const char c = s[++i];
      out += c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\x1e' : c;
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

struct Rule {
  uint32_t a, b, c, order;
  std::string decls;
};

struct El {
  bool svg = false;
  std::string local;
  std::string id;
  int parent = -1;
  std::vector<std::pair<std::string, std::string>> attrs;
  std::vector<Rule> rules;
  std::vector<size_t> kids;
  size_t index = 0, siblings = 1;
  const El * prev = nullptr;
  const El * next = nullptr;
  size_t position = 0;
  GTEXT_CSS_Computed_Style * style = nullptr;
  bool failed = false;
  std::string failure;
};

struct Test {
  int n;
  std::string property, value;
};

struct Page {
  std::vector<El> els;
  size_t target = 0;
  std::vector<Test> tests;
};

const El * as_el(GTEXT_CSS_Element_Handle h) {
  return static_cast<const El *>(h);
}

std::vector<El> * g_all = nullptr;
std::string g_scratch[4];

GTEXT_CSS_Element_Interface make_table(std::vector<El> * all) {
  g_all = all;
  GTEXT_CSS_Element_Interface t;
  std::memset(&t, 0, sizeof t);
  t.size = sizeof t;
  t.parent = [](void *, GTEXT_CSS_Element_Handle e) -> GTEXT_CSS_Element_Handle {
    const El * x = as_el(e);
    return x->parent < 0 ? nullptr : &(*g_all)[(size_t)x->parent];
  };
  t.local_name = [](void *, GTEXT_CSS_Element_Handle e, GTEXT_CSS_String * out) {
    out->data = as_el(e)->local.c_str();
    out->length = as_el(e)->local.size();
    return true;
  };
  t.namespace_uri = [](void *, GTEXT_CSS_Element_Handle e, GTEXT_CSS_String * out) {
    static const char html[] = "http://www.w3.org/1999/xhtml";
    out->data = as_el(e)->svg ? GTEXT_CSS_SVG_NAMESPACE : html;
    out->length = std::strlen(out->data);
    return true;
  };
  t.id = [](void *, GTEXT_CSS_Element_Handle e, GTEXT_CSS_String * out) {
    if (as_el(e)->id.empty()) {
      return false;
    }
    out->data = as_el(e)->id.c_str();
    out->length = as_el(e)->id.size();
    return true;
  };
  t.class_count = [](void *, GTEXT_CSS_Element_Handle) -> size_t { return 0; };
  t.class_at = [](void *, GTEXT_CSS_Element_Handle, size_t, GTEXT_CSS_String *) { return false; };
  t.attribute = [](void *, GTEXT_CSS_Element_Handle e, const GTEXT_CSS_String *, const GTEXT_CSS_String * name, GTEXT_CSS_String * out) {
    for (const auto & a : as_el(e)->attrs) {
      if (a.first.size() == name->length && std::memcmp(a.first.data(), name->data, name->length) == 0) {
        out->data = a.second.c_str();
        out->length = a.second.size();
        return true;
      }
    }
    return false;
  };
  t.previous_sibling = [](void *, GTEXT_CSS_Element_Handle e) -> GTEXT_CSS_Element_Handle { return as_el(e)->prev; };
  t.next_sibling = [](void *, GTEXT_CSS_Element_Handle e) -> GTEXT_CSS_Element_Handle { return as_el(e)->next; };
  t.sibling_index = [](void *, GTEXT_CSS_Element_Handle e) -> size_t { return as_el(e)->index; };
  t.sibling_count = [](void *, GTEXT_CSS_Element_Handle e) -> size_t { return as_el(e)->siblings; };
  t.compare_order = [](void *, GTEXT_CSS_Element_Handle a, GTEXT_CSS_Element_Handle b) -> int {
    return (int)as_el(a)->position - (int)as_el(b)->position;
  };
  t.state = [](void *, GTEXT_CSS_Element_Handle) -> uint64_t { return 0; };
  t.lang = [](void *, GTEXT_CSS_Element_Handle, GTEXT_CSS_String *) { return false; };
  t.direction = [](void *, GTEXT_CSS_Element_Handle) { return GTEXT_CSS_DIRECTION_LTR; };
  return t;
}

/// The size of the query container of an element (Containment 3): the nearest ancestor whose computed `container-type` is `size` (both axes) or `inline-size`
/// (the inline one), and its computed `width` and `height` when those are lengths; false where there is none or the size is not known.
bool container_size(void *, GTEXT_CSS_Element_Handle element, GTEXT_CSS_Container_Axis axis, double * px) {
  const GTEXT_CSS_Registry * reg = svgreg::shared();
  for (int p = as_el(element)->parent; p >= 0; p = (*g_all)[(size_t)p].parent) {
    const El & a = (*g_all)[(size_t)p];
    if (!a.style) {
      continue;
    }
    std::string type;
    const GTEXT_CSS_Property_Id ct = gtext_css_registry_find_property(reg, "container-type", 14);
    if (!ct || resolved_value::read(reg, a.style, ct, &type, nullptr) != GTEXT_CSS_OK) {
      continue;
    }
    const bool inline_only = type.find("inline-size") != std::string::npos;
    const bool both = !inline_only && type.find("size") != std::string::npos;
    if (!inline_only && !both) {
      continue;
    }
    const bool horizontal = axis == GTEXT_CSS_CONTAINER_WIDTH || axis == GTEXT_CSS_CONTAINER_INLINE;
    if (!horizontal && !both) {
      return false;
    }
    std::string len;
    const char * name = horizontal ? "width" : "height";
    const GTEXT_CSS_Property_Id id = gtext_css_registry_find_property(reg, name, std::strlen(name));
    if (!id || resolved_value::read(reg, a.style, id, &len, nullptr) != GTEXT_CSS_OK || len.size() < 3 || len.compare(len.size() - 2, 2, "px") != 0) {
      return false;
    }
    *px = std::atof(len.c_str());
    return true;
  }
  return false;
}

/// The user agent's rules for an element of the page: what the pages assume of `div`, `body` and `html`.
std::string ua_rules(const El & e) {
  if (e.svg) {
    return "";
  }
  if (e.local == "body") {
    return "display: block; margin: 8px";
  }
  if (e.local == "html" || e.local == "div" || e.local == "p") {
    return "display: block";
  }
  return "";
}


/// Compute the style of one element, with `extra` appended to its `style` attribute (the assignment of the test).
GTEXT_CSS_Status compute(const GTEXT_CSS_Compute_Context * ctx, El & e, const El * parent, const std::string & extra, GTEXT_CSS_Computed_Style ** out,
    GTEXT_CSS_Compute_Result * res) {
  std::vector<std::string> texts;
  std::vector<GTEXT_CSS_Style_Entry> entries;
  texts.push_back(ua_rules(e));
  {
    GTEXT_CSS_Style_Entry en;
    std::memset(&en, 0, sizeof en);
    en.keys.origin = GTEXT_CSS_ORIGIN_USER_AGENT;
    entries.push_back(en);
  }
  for (const Rule & r : e.rules) {
    texts.push_back(r.decls);
    GTEXT_CSS_Style_Entry en;
    std::memset(&en, 0, sizeof en);
    en.keys.origin = GTEXT_CSS_ORIGIN_AUTHOR;
    en.keys.rule = r.order;
    en.keys.specificity = gtext_css_specificity_make(r.a, r.b, r.c);
    entries.push_back(en);
  }
  for (size_t i = 0; i < entries.size(); i++) {
    entries[i].text = {texts[i].data(), texts[i].size()};
  }
  std::vector<std::pair<std::string, std::string>> saved = e.attrs;
  if (!extra.empty()) {
    bool found = false;
    for (auto & a : e.attrs) {
      if (a.first == "style") {
        a.second += (a.second.empty() || a.second.back() == ';' ? "" : "; ") + extra;
        found = true;
      }
    }
    if (!found) {
      e.attrs.push_back({"style", extra});
    }
  }
  GTEXT_CSS_Style_Options so = gtext_css_style_options_default();
  const GTEXT_CSS_Status st = gtext_css_style_compute(ctx, &e, parent ? parent->style : nullptr, entries.data(), entries.size(), &so, out, res);
  e.attrs = saved;
  return st;
}

void run_page(Page & page) {
  std::vector<El> & els = page.els;
  for (size_t i = 0; i < els.size(); i++) {
    els[i].position = i;
    els[i].kids.clear();
  }
  for (size_t i = 0; i < els.size(); i++) {
    if (els[i].parent >= 0) {
      els[(size_t)els[i].parent].kids.push_back(i);
    }
  }
  for (size_t i = 0; i < els.size(); i++) {
    for (size_t k = 0; k < els[i].kids.size(); k++) {
      El & c = els[els[i].kids[k]];
      c.index = k + 1;
      c.siblings = els[i].kids.size();
      c.prev = k ? &els[els[i].kids[k - 1]] : nullptr;
      c.next = k + 1 < els[i].kids.size() ? &els[els[i].kids[k + 1]] : nullptr;
    }
  }
  GTEXT_CSS_Element_Interface table = make_table(&els);
  GTEXT_CSS_Environment env = gtext_css_environment_default();
  env.container_size = container_size;
  GTEXT_CSS_Compute_Options co = gtext_css_compute_options_default();
  co.elements = &table;
  GTEXT_CSS_Compute_Context * ctx = nullptr;
  GTEXT_CSS_Compute_Result cr;
  if (gtext_css_compute_context_new(svgreg::shared(), &env, &co, &ctx, &cr) != GTEXT_CSS_OK) {
    std::fprintf(stderr, "css-computed-runner: no context: %s\n", cr.name);
    std::exit(2);
  }
  const GTEXT_CSS_Registry * reg = svgreg::shared();
  for (size_t i = 0; i < els.size(); i++) {
    if (i == page.target) {
      continue;
    }
    El & e = els[i];
    const El * parent = e.parent < 0 ? nullptr : &els[(size_t)e.parent];
    if (parent && parent->failed) {
      e.failed = true;
      e.failure = parent->failure;
      continue;
    }
    GTEXT_CSS_Compute_Result res;
    const GTEXT_CSS_Status st = compute(ctx, e, parent, "", &e.style, &res);
    if (st != GTEXT_CSS_OK) {
      e.failed = true;
      e.failure = res.name[0] ? res.name : gtext_css_status_string(st);
    }
  }
  El & target = els[page.target];
  const El * parent = target.parent < 0 ? nullptr : &els[(size_t)target.parent];
  for (const Test & t : page.tests) {
    const GTEXT_CSS_Property_Id id = gtext_css_registry_find_property(reg, t.property.data(), t.property.size());
    if (!id) {
      std::printf("R\t%d\tUNKNOWN\n", t.n);
      continue;
    }
    if (parent && parent->failed) {
      std::printf("R\t%d\tUNSUPPORTED\t%s\n", t.n, esc(parent->failure).c_str());
      continue;
    }
    // `target.style[property] = value`: a value that is not one of the property is ignored by the browser, and the property then keeps its earlier value
    GTEXT_CSS_Property_Value pv;
    GTEXT_CSS_Status st = gtext_css_parse_property_text(reg, t.property.data(), t.property.size(), t.value.data(), t.value.size(), nullptr, &pv);
    const GTEXT_CSS_Property_Outcome outcome = pv.outcome;
    gtext_css_property_value_free(&pv);
    if (st == GTEXT_CSS_E_UNSUPPORTED) {
      std::printf("R\t%d\tUNSUPPORTED\t%s\n", t.n, esc(t.property).c_str());
      continue;
    }
    if (st != GTEXT_CSS_OK || (outcome != GTEXT_CSS_PROP_RESULT_VALID && outcome != GTEXT_CSS_PROP_RESULT_WIDE && outcome != GTEXT_CSS_PROP_RESULT_PENDING)) {
      std::printf("R\t%d\tINVALID\n", t.n);
      continue;
    }
    std::string text, again;
    bool bad = false;
    for (int pass = 0; pass < 2 && !bad; pass++) {
      GTEXT_CSS_Computed_Style * style = nullptr;
      GTEXT_CSS_Compute_Result res;
      st = compute(ctx, target, parent, t.property + ": " + (pass == 0 ? t.value : text), &style, &res);
      if (st == GTEXT_CSS_E_UNSUPPORTED) {
        std::printf("R\t%d\tUNSUPPORTED\t%s\n", t.n, esc(res.name).c_str());
        bad = true;
      }
      else if (st != GTEXT_CSS_OK) {
        std::printf("R\t%d\tERR\t%s\n", t.n, gtext_css_status_string(st));
        bad = true;
      }
      else {
        std::string got;
        GTEXT_CSS_Compute_Result gr;
        const GTEXT_CSS_Status rs = resolved_value::read(reg, style, id, &got, &gr);
        if (rs == GTEXT_CSS_E_UNSUPPORTED) {
          std::printf("R\t%d\tUNSUPPORTED\t%s\n", t.n, esc(gr.name).c_str());
          bad = true;
        }
        else if (rs != GTEXT_CSS_OK) {
          std::printf("R\t%d\tERR\t%s\n", t.n, gtext_css_status_string(rs));
          bad = true;
        }
        else if (pass == 0) {
          text = got;
        }
        else {
          again = got;
        }
      }
      gtext_css_computed_free(style);
    }
    if (!bad) {
      std::printf("R\t%d\tOK\t%s\t%s\n", t.n, esc(text).c_str(), esc(again).c_str());
    }
  }
  for (El & e : els) {
    gtext_css_computed_free(e.style);
  }
  gtext_css_compute_context_free(ctx);
}

} // namespace

int main() {
  Page page;
  bool open = false;
  char * line = nullptr;
  size_t cap = 0;
  ssize_t len;
  while ((len = getline(&line, &cap, stdin)) >= 0) {
    std::string l(line, (size_t)len);
    if (!l.empty() && l.back() == '\n') {
      l.pop_back();
    }
    const std::vector<std::string> f = split(l, '\t');
    if (f[0] == "P" && f.size() == 3) {
      page = Page();
      page.target = (size_t)std::atol(f[2].c_str());
      open = true;
    }
    else if (f[0] == "E" && f.size() == 7 && open) {
      El e;
      e.svg = f[3] == "s";
      e.local = unesc(f[4]);
      e.parent = std::atoi(f[2].c_str());
      for (const std::string & a : split(unesc(f[5]), '\x1e')) {
        const size_t eq = a.find('=');
        if (eq != std::string::npos) {
          e.attrs.push_back({a.substr(0, eq), a.substr(eq + 1)});
          if (a.substr(0, eq) == "id") {
            e.id = a.substr(eq + 1);
          }
        }
      }
      for (const std::string & r : split(unesc(f[6]), '\x1e')) {
        if (r.empty()) {
          continue;
        }
        const std::vector<std::string> p = split(r, ',');
        if (p.size() >= 5) {
          const size_t head = p[0].size() + p[1].size() + p[2].size() + p[3].size() + 4;
          e.rules.push_back({(uint32_t)std::atol(p[0].c_str()), (uint32_t)std::atol(p[1].c_str()), (uint32_t)std::atol(p[2].c_str()),
              (uint32_t)std::atol(p[3].c_str()), r.substr(head)});
        }
      }
      page.els.push_back(e);
    }
    else if (f[0] == "T" && f.size() == 4 && open) {
      page.tests.push_back({std::atoi(f[1].c_str()), unesc(f[2]), unesc(f[3])});
    }
    else if (f[0] == "X" && open) {
      run_page(page);
      std::printf("X\n");
      std::fflush(stdout);
      open = false;
    }
  }
  std::free(line);
  return 0;
}
