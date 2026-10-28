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
 * libFuzzer harness for the computed-style call (src/css/css_style.c, css_presentation.c, css_dump.c): a tree of two elements with presentation
 * attributes, `style` attributes and entries, computed.
 *
 * The input is four option bytes, then text in sections separated by a line `---`:
 *
 *   o0  bits 0-1 the shape: 0 the child alone; 1 a parent and a child; 2 the child inherits from a style made of the same text as its own;
 *       bit 2 the limits are small (a few candidates, a few hundred tokens of substitution) so that every limit path is reached;
 *       bit 3 the child is an HTML element (not in the SVG namespace: it has no presentation attributes); bit 4 the child is a `circle`;
 *       bit 5 the substitution environment answers (`env()`, a condition, the sibling position);
 *   o1  bits 0-3 the attribute-only spellings (unitless lengths, SVG transform syntax, path data, trimming); bit 4 the presentation attributes are read;
 *       bit 5 the `style` attribute is read (a zero byte reads neither: only the entries count);
 *   o2  reserved (zero);
 *   o3  the allocation to fail (0 for none), counted over the one computation of the child (the context is made first, on the same allocator, unarmed).
 *
 * The sections: 0 the parent's `style` attribute; 1 the parent's presentation attributes (`name=value`, one a line); 2 the child's `style` attribute;
 * 3 the child's presentation attributes; 4 the entries (`rule|declarations`, one a line, `rule` a digit).
 *
 * The properties:
 *
 *   a status is OK, E_UNSUPPORTED, or a limit - never E_INTERNAL; **an allocation that fails is E_OOM**, with no style returned and nothing leaked or freed that the
 *   allocator did not make; a failed computation returns no style;
 *   a style computed twice from the same inputs dumps the same text and hashes the same, and the hash is the SHA-256 of the dump;
 *   **the dump is one line a property**: the number of its line feeds is the number of properties the style iterates, plus the custom properties, plus two;
 *   **the order the entries are given in does not matter** (their keys are distinct): the entries reversed give the same dump;
 *   **an attribute and a declaration are one thing**: with the spellings that only attributes have off, `name="value"` and `style="name: value"` compute to the same
 *   style (the value holding none of the characters that split a declaration);
 *   the diagnostics: the kept ones are no more than the total, each has a name, and the total is no more than the declarations and attributes there were;
 *   a retained style is the same style: the dump through the second owner is the first's, and the style outlives the first owner's release.
 *
 * Seeds: tests/fuzz/corpus/css-style, made by tools/css/make-fuzz-style-seeds.py.
 * Build with: make fuzz-css-style      Run: make fuzz-run-css-style
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ghoti.io/security/sha256.h>
#include <ghoti.io/text/css.h>

#include "../css_svg_registry.h"
#include "../markup_test_support.h"

namespace css_stylefuzz {

[[noreturn]] void fail(const char * what, const std::string & detail, const std::string & input) {
  std::fprintf(stderr, "%s: %s\n", "fuzz_css_style", what);
  for (unsigned char c : detail) {
    if ((c >= 0x20 && c < 0x7f) || c == '\n') {
      std::fputc(c, stderr);
    }
    else {
      std::fprintf(stderr, "\\x%02x", c);
    }
  }
  std::fprintf(stderr, "\ninput (%zu bytes): ", input.size());
  for (unsigned char c : input) {
    if (c >= 0x20 && c < 0x7f && c != '\\') {
      std::fputc(c, stderr);
    }
    else {
      std::fprintf(stderr, "\\x%02x", c);
    }
  }
  std::fputc('\n', stderr);
  std::fflush(stderr);
  std::abort();
}

bool allowed_status(GTEXT_CSS_Status s) {
  switch (s) {
  case GTEXT_CSS_OK:
  case GTEXT_CSS_E_OOM:
  case GTEXT_CSS_E_UNSUPPORTED:
  case GTEXT_CSS_E_INVALID:
  case GTEXT_CSS_E_LIMIT_WORK:
  case GTEXT_CSS_E_LIMIT_OUTPUT:
  case GTEXT_CSS_E_LIMIT_DEPTH:
  case GTEXT_CSS_E_LIMIT_MATH_NODES:
  case GTEXT_CSS_E_LIMIT_COLOR_NESTING:
  case GTEXT_CSS_E_LIMIT_STOPS:
  case GTEXT_CSS_E_LIMIT_FUNCTION_NODES:
  case GTEXT_CSS_E_LIMIT_TOKENS:
  case GTEXT_CSS_E_LIMIT_INPUT:
  case GTEXT_CSS_E_LIMIT_DECLARATIONS:
  case GTEXT_CSS_E_LIMIT_LIST:
  case GTEXT_CSS_E_LIMIT_ELEMENTS:
  case GTEXT_CSS_E_LIMIT_CUSTOM_PROPERTIES:
  case GTEXT_CSS_E_LIMIT_CUSTOM_VALUE:
  case GTEXT_CSS_E_LIMIT_CANDIDATES:
  case GTEXT_CSS_E_LIMIT_REGISTRY:
    return true;
  default:
    return false;
  }
}

struct Elem {
  std::vector<std::pair<std::string, std::string>> attrs;
  const char * local = "rect";
  const char * ns = GTEXT_CSS_SVG_NAMESPACE;
};

const GTEXT_CSS_Element_Interface * element_interface() {
  static const GTEXT_CSS_Element_Interface iface = [] {
    GTEXT_CSS_Element_Interface t;
    std::memset(&t, 0, sizeof t);
    t.size = sizeof t;
    t.parent = [](void *, GTEXT_CSS_Element_Handle) -> GTEXT_CSS_Element_Handle { return nullptr; };
    t.local_name = [](void *, GTEXT_CSS_Element_Handle e, GTEXT_CSS_String * out) {
      out->data = static_cast<const Elem *>(e)->local;
      out->length = std::strlen(out->data);
      return true;
    };
    t.namespace_uri = [](void *, GTEXT_CSS_Element_Handle e, GTEXT_CSS_String * out) {
      out->data = static_cast<const Elem *>(e)->ns;
      out->length = std::strlen(out->data);
      return true;
    };
    t.attribute = [](void *, GTEXT_CSS_Element_Handle e, const GTEXT_CSS_String *, const GTEXT_CSS_String * name, GTEXT_CSS_String * out) {
      for (const auto & a : static_cast<const Elem *>(e)->attrs) {
        if (a.first.size() == name->length && std::memcmp(a.first.data(), name->data, name->length) == 0) {
          out->data = a.second.data();
          out->length = a.second.size();
          return true;
        }
      }
      return false;
    };
    t.sibling_index = [](void *, GTEXT_CSS_Element_Handle) -> size_t { return 3; };
    t.sibling_count = [](void *, GTEXT_CSS_Element_Handle) -> size_t { return 5; };
    return t;
  }();
  return &iface;
}

bool env_variable(void *, const GTEXT_CSS_String * name, const uint32_t *, size_t, GTEXT_CSS_String * out) {
  static const char five[] = "5px";
  if (name->length == 2 && std::memcmp(name->data, "e1", 2) == 0) {
    out->data = five;
    out->length = sizeof five - 1;
    return true;
  }
  return false;
}

GTEXT_CSS_Tristate condition(void *, GTEXT_CSS_Condition_Kind, const GTEXT_CSS_String * text, GTEXT_CSS_Element_Handle) {
  return text->length % 3 == 0 ? GTEXT_CSS_TRI_TRUE : text->length % 3 == 1 ? GTEXT_CSS_TRI_FALSE : GTEXT_CSS_TRI_UNKNOWN;
}

GTEXT_CSS_Compute_Options options_of(uint8_t o0, const GTEXT_Allocator * alloc) {
  GTEXT_CSS_Compute_Options o = gtext_css_compute_options_default();
  o.elements = element_interface();
  o.allocator = alloc;
  o.custom_hash_seed = 99;
  if (o0 & 4) {
    o.substitute.max_substituted_tokens = 400;
    o.substitute.max_element_substituted_tokens = 3000;
    o.substitute.max_custom_properties = 24;
    o.substitute.max_value_bytes = 2048;
    o.substitute.max_substitution_depth = 40;
  }
  return o;
}

GTEXT_CSS_Environment environment_of(uint8_t o0) {
  GTEXT_CSS_Environment e = gtext_css_environment_default();
  if (o0 & 32) {
    e.env_variable = env_variable;
    e.condition = condition;
    e.random_seed = 7;
  }
  return e;
}

GTEXT_CSS_Compute_Context * shared_context(uint8_t o0) {
  static GTEXT_CSS_Compute_Context * contexts[8] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  const unsigned slot = ((o0 >> 2) & 1) | (((o0 >> 5) & 1) << 1);
  GTEXT_CSS_Compute_Context *& c = contexts[slot];
  if (!c) {
    const GTEXT_CSS_Environment env = environment_of(o0);
    const GTEXT_CSS_Compute_Options o = options_of(o0, nullptr);
    GTEXT_CSS_Compute_Result r{};
    if (gtext_css_compute_context_new(svgreg::shared(), &env, &o, &c, &r) != GTEXT_CSS_OK) {
      fail("a context could not be made", r.name, "");
    }
  }
  return c;
}

std::string dump_of(const GTEXT_CSS_Computed_Style * s, const std::string & input) {
  std::string out;
  GTEXT_Markup_Sink sink;
  std::memset(&sink, 0, sizeof sink);
  sink.user = &out;
  sink.write = [](void * u, const char * b, size_t n) -> GTEXT_Markup_Status {
    static_cast<std::string *>(u)->append(b, n);
    return GTEXT_MARKUP_OK;
  };
  const GTEXT_CSS_Status st = gtext_css_computed_dump(s, &sink);
  if (st != GTEXT_CSS_OK) {
    fail("a style could not be dumped", gtext_css_status_string(st), input);
  }
  return out;
}

std::vector<std::string> sections(const std::string & text) {
  std::vector<std::string> out(1);
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t end = text.find('\n', pos);
    const std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    if (line == "---") {
      out.emplace_back();
    }
    else {
      out.back() += line;
      if (end != std::string::npos) {
        out.back() += '\n';
      }
    }
    if (end == std::string::npos) {
      break;
    }
    pos = end + 1;
  }
  out.resize(std::max<size_t>(out.size(), 5));
  return out;
}

std::vector<std::pair<std::string, std::string>> attribute_lines(const std::string & text) {
  std::vector<std::pair<std::string, std::string>> out;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) {
      end = text.size();
    }
    const std::string line = text.substr(pos, end - pos);
    pos = end + 1;
    const size_t eq = line.find('=');
    if (eq == std::string::npos || eq == 0 || eq > 64) {
      continue;
    }
    out.emplace_back(line.substr(0, eq), line.substr(eq + 1));
  }
  return out;
}

struct Entries {
  std::vector<std::string> texts;
  std::vector<GTEXT_CSS_Style_Entry> list;
  void make(const std::string & section) {
    size_t pos = 0;
    while (pos < section.size() && texts.size() < 16) {
      size_t end = section.find('\n', pos);
      if (end == std::string::npos) {
        end = section.size();
      }
      std::string line = section.substr(pos, end - pos);
      pos = end + 1;
      const size_t bar = line.find('|');
      if (bar == std::string::npos) {
        continue;
      }
      texts.push_back(line.substr(bar + 1));
      GTEXT_CSS_Style_Entry e;
      std::memset(&e, 0, sizeof e);
      e.keys.origin = GTEXT_CSS_ORIGIN_AUTHOR;
      e.keys.rule = (uint32_t)(texts.size() - 1);
      e.keys.specificity.b = (uint32_t)(line.empty() ? 0 : (unsigned char)line[0] % 3);
      list.push_back(e);
    }
    for (size_t i = 0; i < list.size(); i++) {
      list[i].text = {texts[i].data(), texts[i].size()};
    }
  }
};

GTEXT_CSS_Style_Options style_options_of(uint8_t o1) {
  GTEXT_CSS_Style_Options o = gtext_css_style_options_default();
  o.sources = ((o1 & 16) ? GTEXT_CSS_STYLE_PRESENTATION_ATTRIBUTES : 0) | ((o1 & 32) ? GTEXT_CSS_STYLE_STYLE_ATTRIBUTE : 0);
  o.presentation.unitless_lengths = o1 & 1;
  o.presentation.svg_transform = o1 & 2;
  o.presentation.path_data = o1 & 4;
  o.presentation.trim_whitespace = o1 & 8;
  return o;
}

size_t count_lines(const std::string & s) {
  size_t n = 0;
  for (char c : s) {
    n += c == '\n';
  }
  return n;
}

void check_dump(const GTEXT_CSS_Computed_Style * s, const std::string & dump, const std::string & input) {
  size_t props = 0, cursor = 0;
  while (gtext_css_computed_next(s, &cursor, nullptr, nullptr)) {
    props++;
  }
  const size_t want = props + gtext_css_computed_custom_count(s) + 2;
  if (count_lines(dump) != want) {
    fail("the dump is not one line a property", std::to_string(count_lines(dump)) + " lines, " + std::to_string(want) + " expected\n" + dump, input);
  }
  unsigned char h[32], w[32];
  if (gtext_css_computed_hash(s, h) != GTEXT_CSS_OK || gsec_sha256(dump.data(), dump.size(), w) != GSEC_OK || std::memcmp(h, w, 32) != 0) {
    fail("the hash of a style is not the SHA-256 of its dump", "", input);
  }
}

/// An attribute and a declaration are one thing, with the spellings only attributes have off.
void check_attribute_is_declaration(const std::string & input, uint8_t o0) {
  const std::vector<std::string> sec = sections(input.substr(4));
  for (const auto & a : attribute_lines(sec[3])) {
    const GTEXT_CSS_Presentation_Row * row = gtext_css_presentation_find(nullptr, GTEXT_CSS_SVG_NAMESPACE, std::strlen(GTEXT_CSS_SVG_NAMESPACE), "rect", 4,
        a.first.data(), a.first.size());
    if (!row || row->syntax != GTEXT_CSS_PRESENTATION_CSS || std::strcmp(row->attribute, row->property) != 0 || a.second.find_first_of(";!{}/\\\"'\n\r\f") != std::string::npos ||
        a.second.size() > 200) {
      continue;
    }
    Elem as_attr, as_style;
    as_attr.attrs = {{a.first, a.second}};
    as_style.attrs = {{"style", a.first + ": " + a.second}};
    GTEXT_CSS_Style_Options oa = gtext_css_style_options_default(), os = gtext_css_style_options_default();
    oa.presentation = {};
    oa.presentation.trim_whitespace = true;
    os.presentation = oa.presentation;
    oa.sources = GTEXT_CSS_STYLE_PRESENTATION_ATTRIBUTES;
    os.sources = GTEXT_CSS_STYLE_STYLE_ATTRIBUTE;
    GTEXT_CSS_Computed_Style *sa = nullptr, *ss = nullptr;
    const GTEXT_CSS_Compute_Context * ctx = shared_context(o0 & ~(uint8_t)0);
    const GTEXT_CSS_Status ra = gtext_css_style_compute(ctx, &as_attr, nullptr, nullptr, 0, &oa, &sa, nullptr);
    const GTEXT_CSS_Status rs = gtext_css_style_compute(ctx, &as_style, nullptr, nullptr, 0, &os, &ss, nullptr);
    if (ra != rs && !((ra == GTEXT_CSS_E_OOM) || (rs == GTEXT_CSS_E_OOM))) {
      fail("an attribute and a declaration do not compute alike (status)", a.first + "=" + a.second + " " + gtext_css_status_string(ra) + " " + gtext_css_status_string(rs), input);
    }
    if (ra == GTEXT_CSS_OK && rs == GTEXT_CSS_OK && dump_of(sa, input) != dump_of(ss, input)) {
      fail("an attribute and a declaration do not compute alike", a.first + "=" + a.second + "\n" + dump_of(sa, input) + "---\n" + dump_of(ss, input), input);
    }
    gtext_css_computed_free(sa);
    gtext_css_computed_free(ss);
  }
}

} // namespace css_stylefuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  using namespace css_stylefuzz;
  if (size < 4 || size > 8192) {
    return 0;
  }
  const uint8_t o0 = data[0], o1 = data[1], o3 = data[3];
  const std::string input(reinterpret_cast<const char *>(data), size);
  const std::vector<std::string> sec = sections(input.substr(4));

  Elem parent_e, child_e;
  parent_e.attrs = attribute_lines(sec[1]);
  if (!sec[0].empty()) {
    parent_e.attrs.push_back({"style", sec[0]});
  }
  child_e.attrs = attribute_lines(sec[3]);
  if (!sec[2].empty()) {
    child_e.attrs.push_back({"style", sec[2]});
  }
  if (o0 & 8) {
    child_e.ns = "http://www.w3.org/1999/xhtml";
  }
  if (o0 & 16) {
    child_e.local = "circle";
  }
  Entries entries;
  entries.make(sec[4]);
  const GTEXT_CSS_Style_Options so = style_options_of(o1);

  FaultAllocator fa;
  GTEXT_CSS_Compute_Context * ctx = nullptr;
  GTEXT_CSS_Compute_Result r{};
  if (o3) {
    const GTEXT_CSS_Environment env = environment_of(o0);
    const GTEXT_CSS_Compute_Options o = options_of(o0, &fa.alloc);
    if (gtext_css_compute_context_new(svgreg::shared(), &env, &o, &ctx, &r) != GTEXT_CSS_OK) {
      fail("a context could not be made", r.name, input);
    }
  }
  else {
    ctx = shared_context(o0);
  }

  GTEXT_CSS_Computed_Style *parent = nullptr, *child = nullptr;
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  const unsigned shape = o0 & 3;
  GTEXT_CSS_Style_Options co = so;
  if (o0 & 4) {
    co.candidates.max_candidates = 12;
    co.parse.max_declarations = 40;
  }
  if (shape == 1 || shape == 2) {
    Elem * pe = shape == 1 ? &parent_e : &child_e;
    st = gtext_css_style_compute(ctx, pe, nullptr, nullptr, 0, &co, &parent, &r);
  }
  fa.arm(o3);
  if (st == GTEXT_CSS_OK) {
    st = gtext_css_style_compute(ctx, &child_e, parent, entries.list.data(), entries.list.size(), &co, &child, &r);
  }
  fa.disarm();
  if (!allowed_status(st)) {
    fail("a status a computation must not return", std::string(gtext_css_status_string(st)) + " " + r.name, input);
  }
  if (fa.failed && st != GTEXT_CSS_E_OOM) {
    /* the allocation that failed was the child's, or a swallowed one in the parent's context: either way the call must say so */
    fail("an allocation failed and the computation did not say so", gtext_css_status_string(st), input);
  }
  if (st != GTEXT_CSS_OK && child) {
    fail("a computation that did not succeed returned a style", gtext_css_status_string(st), input);
  }
  if (st == GTEXT_CSS_OK) {
    const std::string first = dump_of(child, input);
    check_dump(child, first, input);
    GTEXT_CSS_Computed_Style * again = nullptr;
    if (!o3) {
      GTEXT_CSS_Computed_Style * p2 = nullptr;
      GTEXT_CSS_Status s2 = GTEXT_CSS_OK;
      if (shape == 1 || shape == 2) {
        s2 = gtext_css_style_compute(ctx, shape == 1 ? &parent_e : &child_e, nullptr, nullptr, 0, &co, &p2, &r);
      }
      if (s2 == GTEXT_CSS_OK) {
        s2 = gtext_css_style_compute(ctx, &child_e, p2, entries.list.data(), entries.list.size(), &co, &again, &r);
      }
      if (s2 != GTEXT_CSS_OK || !again || dump_of(again, input) != first) {
        fail("a style computed twice is not the same", "", input);
      }
      gtext_css_computed_free(p2);
      /* the entries in the other order */
      if (entries.list.size() > 1) {
        std::vector<GTEXT_CSS_Style_Entry> reversed(entries.list.rbegin(), entries.list.rend());
        GTEXT_CSS_Computed_Style * other = nullptr;
        GTEXT_CSS_Computed_Style * p3 = nullptr;
        GTEXT_CSS_Status s3 = GTEXT_CSS_OK;
        if (shape == 1 || shape == 2) {
          s3 = gtext_css_style_compute(ctx, shape == 1 ? &parent_e : &child_e, nullptr, nullptr, 0, &co, &p3, &r);
        }
        if (s3 == GTEXT_CSS_OK) {
          s3 = gtext_css_style_compute(ctx, &child_e, p3, reversed.data(), reversed.size(), &co, &other, &r);
        }
        /* the declaration ordinals follow the order given, and a tie in every other key is broken by them: the keys here differ in `rule` */
        if (s3 == GTEXT_CSS_OK && dump_of(other, input) != first) {
          fail("the order the entries are given in changes the style", dump_of(other, input) + "---\n" + first, input);
        }
        gtext_css_computed_free(other);
        gtext_css_computed_free(p3);
      }
      /* the diagnostics */
      const size_t total = gtext_css_computed_diagnostic_total(child);
      size_t kept = 0;
      GTEXT_CSS_Style_Diagnostic d;
      while (gtext_css_computed_diagnostic(child, kept, &d)) {
        if (!d.name.data) {
          fail("a diagnostic has no name", "", input);
        }
        kept++;
      }
      if (kept > total) {
        fail("more diagnostics are kept than were counted", std::to_string(kept) + " " + std::to_string(total), input);
      }
    }
    GTEXT_CSS_Computed_Style * second = gtext_css_computed_retain(child);
    gtext_css_computed_free(child);
    child = nullptr;
    if (dump_of(second, input) != first) {
      fail("a retained style is not the style", "", input);
    }
    gtext_css_computed_free(second);
    gtext_css_computed_free(again);
  }
  gtext_css_computed_free(child);
  gtext_css_computed_free(parent);
  if (!o3) {
    check_attribute_is_declaration(input, o0);
  }
  if (o3) {
    gtext_css_compute_context_free(ctx);
    if (!fa.live.empty()) {
      fail("an allocation leaked", std::to_string(fa.live.size()) + " blocks", input);
    }
    if (fa.bad_frees) {
      fail("a block the allocator did not make was freed", "", input);
    }
  }
  return 0;
}
