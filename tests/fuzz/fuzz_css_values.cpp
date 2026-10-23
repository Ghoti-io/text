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
 * libFuzzer harness for the CSS value layer built on the grammar engine: the math functions
 * (src/css/css_math*.c), the colors (css_color*.c), and the function types and their views
 * (css_function.c, css_image.c, css_easing.c ...).
 *
 * The input is three option bytes, then CSS text (component values):
 *
 *   o0  bit 0 the unitless-length quirk, bit 1 a tiny math depth limit, bit 2 a tiny math node limit, bit 3 a tiny color
 *       nesting limit, the limit being chosen by o1;
 *   o1  the tiny limit (1 to 64);
 *   o2  the allocation to fail (0 for none), counted over reading one value.
 *
 * The properties, for every component value of the text:
 *
 *   a math function is OK (valid or not) or a limit or OOM, never anything else; reading it twice gives the same answer; a valid
 *   one is written, the text read again is valid and is written as the same text (the fixed point), and has the same category;
 *   **the node bound is exact**: the nodes it made, given as the bound, are accepted, and one less is GTEXT_CSS_E_LIMIT_MATH_NODES;
 *   a calculation that came down to one number resolves, with no environment, to that number;
 *   a color is OK (valid or not) or a limit or OOM; a valid one is written and the text read again is a valid color written as the
 *   same text; a color that is one sRGB value says so consistently with its text;
 *   a text matched against `<color>`, `<image>`, `<transform-list>`, `<easing-function>`, `<basic-shape>`, `<length-percentage>`
 *   and `<number>` is OK or E_LIMIT_WORK or a limit of this layer, never E_INTERNAL, and the typed value that matched is written
 *   and, unless it is pending substitution, matches again and is written as the same text; its typed views (easing, gradient, image
 *   set, transform, shape) answer or refuse without a crash;
 *   an allocation that fails is E_OOM with nothing returned, and never a leak or a free of a block the allocator did not make.
 *
 * Seeds: tests/fuzz/corpus/css-values, made by tools/css/make-fuzz-values-seeds.py.
 * Build with: make fuzz-css-values      Run: make fuzz-run-css-values
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ghoti.io/text/css.h>
#include <ghoti.io/text/css/css_function.h>

#include "../markup_test_support.h"

namespace css_vfuzz {

[[noreturn]] void fail(const char * what, const std::string & detail, const std::string & input) {
  std::fprintf(stderr, "fuzz_css_values: %s\n%s\ninput (%zu bytes): ", what, detail.c_str(), input.size());
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

bool is_hard_limit(GTEXT_CSS_Status s) {
  return s == GTEXT_CSS_E_LIMIT_DEPTH || s == GTEXT_CSS_E_LIMIT_MATH_NODES || s == GTEXT_CSS_E_LIMIT_COLOR_NESTING || s == GTEXT_CSS_E_LIMIT_OUTPUT;
}

/// Equal, or equal as a number is written (at most six decimals: CSSOM's rule, which loses the rest).
bool near(double a, double b) {
  if (a == b || (a != a && b != b)) {
    return true;
  }
  const double d = a > b ? a - b : b - a;
  const double m = a < 0 ? -a : a;
  return d <= 1e-6 + 1e-9 * m;
}

std::string written(const GTEXT_CSS_Math * m, const std::string & input) {
  char * buf = nullptr;
  size_t len = 0;
  if (gtext_css_math_serialize_buffer(m, nullptr, &buf, &len) != GTEXT_CSS_OK) {
    fail("a calculation could not be written", "", input);
  }
  std::string s(buf, len);
  gtext_css_serialize_buffer_free(nullptr, buf);
  return s;
}

std::string written(const GTEXT_CSS_Color * c, const std::string & input) {
  char * buf = nullptr;
  size_t len = 0;
  const GTEXT_CSS_Status st = gtext_css_color_serialize_buffer(c, nullptr, &buf, &len);
  if (st != GTEXT_CSS_OK) {
    fail("a color could not be written", gtext_css_status_string(st), input);
  }
  std::string s(buf, len);
  gtext_css_serialize_buffer_free(nullptr, buf);
  return s;
}

/// The one component value of a text, or null.
const GTEXT_CSS_Component_Value * single(GTEXT_CSS_Tree * tree) {
  const GTEXT_CSS_Component_Value * found = nullptr;
  for (size_t i = 0; i < tree->value_count; i++) {
    const GTEXT_CSS_Component_Value & v = tree->values[i];
    if (v.kind == GTEXT_CSS_VALUE_TOKEN && v.type == GTEXT_CSS_TOKEN_WHITESPACE) {
      continue;
    }
    if (found) {
      return nullptr;
    }
    found = &v;
  }
  return found;
}

void check_math(const GTEXT_CSS_Component_Value & v, const GTEXT_CSS_Math_Options & mo, const GTEXT_CSS_Parse_Options & po, const std::string & input) {
  GTEXT_CSS_Math * m = nullptr;
  bool valid = false;
  GTEXT_CSS_Math_Result r{};
  const GTEXT_CSS_Status st = gtext_css_math_parse(&v, nullptr, &mo, &valid, &m, &r);
  if (st != GTEXT_CSS_OK) {
    if (m || (!is_hard_limit(st) && st != GTEXT_CSS_E_OOM)) {
      fail("a math function failed with a status it must not", gtext_css_status_string(st), input);
    }
    return;
  }
  if (!valid) {
    if (m) {
      fail("an invalid math function was returned", "", input);
    }
    return;
  }
  const std::string text = written(m, input);
  GTEXT_CSS_Math * m2 = nullptr;
  bool valid2 = false;
  if (gtext_css_math_parse(&v, nullptr, &mo, &valid2, &m2, nullptr) != GTEXT_CSS_OK || !valid2 || written(m2, input) != text) {
    fail("reading a math function twice gave two answers", text, input);
  }
  gtext_css_math_free(m2);
  // the fixed point
  GTEXT_CSS_Tree * back = nullptr;
  if (gtext_css_parse_component_values(text.data(), text.size(), &po, &back, nullptr) == GTEXT_CSS_OK) {
    const GTEXT_CSS_Component_Value * bv = single(back);
    GTEXT_CSS_Math * m3 = nullptr;
    bool valid3 = false;
    GTEXT_CSS_Math_Options big = gtext_css_math_options_default();
    big.max_depth = 4096;
    big.max_nodes = (size_t)1 << 22;
    if (!bv || gtext_css_math_parse(bv, nullptr, &big, &valid3, &m3, nullptr) != GTEXT_CSS_OK || !valid3) {
      fail("a written math function is not read again", text, input);
    }
    double v1 = 0, v3 = 0;
    GTEXT_CSS_Unit u1 = GTEXT_CSS_UNIT_NONE, u3 = GTEXT_CSS_UNIT_NONE;
    bool p1 = false, p3 = false;
    const bool n1 = gtext_css_math_numeric(m, &v1, &u1, &p1), n3 = gtext_css_math_numeric(m3, &v3, &u3, &p3);
    if (n1 != n3 || (n1 && (u1 != u3 || p1 != p3 || !near(v1, v3)))) {
      fail("a written calculation reads back as another number", text, input);
    }
    if (written(m3, input) != text || m3->category != m->category) {
      fail("a written math function is not written again as the same text", text + "\n" + written(m3, input), input);
    }
    gtext_css_math_free(m3);
  }
  gtext_css_tree_free(back);
  // the node bound is exact
  if (r.nodes > 1) {
    GTEXT_CSS_Math_Options at = mo;
    at.max_nodes = r.nodes;
    GTEXT_CSS_Math * a = nullptr;
    bool av = false;
    if (gtext_css_math_parse(&v, nullptr, &at, &av, &a, nullptr) != GTEXT_CSS_OK || !av) {
      fail("the nodes a calculation made are not accepted as the bound", std::to_string(r.nodes), input);
    }
    gtext_css_math_free(a);
    at.max_nodes = r.nodes - 1;
    GTEXT_CSS_Math * b = nullptr;
    bool bvld = false;
    if (gtext_css_math_parse(&v, nullptr, &at, &bvld, &b, nullptr) != GTEXT_CSS_E_LIMIT_MATH_NODES || b) {
      fail("one less than the nodes a calculation made is not the limit", std::to_string(r.nodes), input);
    }
  }
  // a number resolves to itself
  double value = 0, resolved_value = 0;
  bool resolved = false;
  if (gtext_css_math_numeric(m, &value, nullptr, nullptr) && !m->has_symbols && !m->has_percent) {
    GTEXT_CSS_Unit u;
    bool pct;
    gtext_css_math_numeric(m, &value, &u, &pct);
    if (!pct && (u == GTEXT_CSS_UNIT_NONE || u == GTEXT_CSS_UNIT_PX)) {
      if (gtext_css_math_resolve(m, nullptr, nullptr, &resolved_value, &resolved) != GTEXT_CSS_OK || !resolved
          || !(resolved_value == value || (resolved_value != resolved_value && value != value))) {
        fail("a calculation that is one number does not resolve to it", "", input);
      }
    }
  }
  gtext_css_math_free(m);
}

void check_color(const GTEXT_CSS_Component_Value & v, const GTEXT_CSS_Color_Options & co, const GTEXT_CSS_Parse_Options & po, const std::string & input) {
  GTEXT_CSS_Color * c = nullptr;
  bool valid = false;
  GTEXT_CSS_Color_Result r{};
  const GTEXT_CSS_Status st = gtext_css_color_parse(&v, &co, &valid, &c, &r);
  if (st != GTEXT_CSS_OK) {
    if (c || (!is_hard_limit(st) && st != GTEXT_CSS_E_OOM)) {
      fail("a color failed with a status it must not", gtext_css_status_string(st), input);
    }
    return;
  }
  if (!valid) {
    if (c) {
      fail("an invalid color was returned", "", input);
    }
    return;
  }
  const std::string text = written(c, input);
  GTEXT_CSS_Tree * back = nullptr;
  if (gtext_css_parse_component_values(text.data(), text.size(), &po, &back, nullptr) == GTEXT_CSS_OK) {
    const GTEXT_CSS_Component_Value * bv = single(back);
    GTEXT_CSS_Color * c2 = nullptr;
    bool valid2 = false;
    GTEXT_CSS_Color_Options big = gtext_css_color_options_default();
    big.max_nesting = 4096;
    big.max_math_depth = 4096;
    big.max_math_nodes = (size_t)1 << 22;
    if (!bv || gtext_css_color_parse(bv, &big, &valid2, &c2, nullptr) != GTEXT_CSS_OK || !valid2) {
      fail("a written color is not read again", text, input);
    }
    double s1[4], s2[4];
    const bool k1 = gtext_css_color_srgb(c, s1), k2 = gtext_css_color_srgb(c2, s2);
    if (k1 != k2 || (k1 && (!near(s1[0], s2[0]) || !near(s1[1], s2[1]) || !near(s1[2], s2[2]) || !(s1[3] - s2[3] < 0.002 && s2[3] - s1[3] < 0.002)))) {
      fail("a written color reads back as another color", text, input);
    }
    if (written(c2, input) != text) {
      fail("a written color is not written again as the same text", text + "\n" + written(c2, input), input);
    }
    gtext_css_color_free(c2);
  }
  gtext_css_tree_free(back);
  double rgba[4];
  if (gtext_css_color_srgb(c, rgba)) {
    (void)rgba;
  }
  gtext_css_color_free(c);
}

const char * const kGrammars[] = {"<color>", "<image>", "<transform-list>", "<easing-function>", "<basic-shape>", "<length-percentage>", "<number>"};

void check_match(const GTEXT_CSS_Compiled * c, GTEXT_CSS_Tree * tree, const GTEXT_CSS_Match_Options & mo, const GTEXT_CSS_Parse_Options & po,
    const std::string & input) {
  bool matched = false;
  GTEXT_CSS_Value_Set * set = nullptr;
  GTEXT_CSS_Match_Result r{};
  const GTEXT_CSS_Status st = gtext_css_match(c, tree->values, tree->value_count, &mo, &matched, &set, &r);
  if (st != GTEXT_CSS_OK) {
    if (set || (st != GTEXT_CSS_E_LIMIT_WORK && st != GTEXT_CSS_E_OOM && !is_hard_limit(st))) {
      fail("a match failed with a status it must not", gtext_css_status_string(st), input);
    }
    return;
  }
  if (!matched) {
    gtext_css_value_set_free(set);
    return;
  }
  const GTEXT_CSS_Value * root = gtext_css_value_set_root(set);
  if (root) {
    char * buf = nullptr;
    size_t len = 0;
    if (gtext_css_serialize_value_buffer(root, nullptr, &buf, &len) != GTEXT_CSS_OK) {
      fail("a matched value could not be written", "", input);
    }
    const std::string text(buf, len);
    gtext_css_serialize_buffer_free(nullptr, buf);
    // the typed views
    bool valid = false;
    GTEXT_CSS_Easing * e = nullptr;
    if (gtext_css_easing_from_value(root, nullptr, &valid, &e) == GTEXT_CSS_OK && valid) {
      char * eb = nullptr;
      size_t el = 0;
      if (gtext_css_easing_serialize_buffer(e, nullptr, &eb, &el) != GTEXT_CSS_OK) {
        fail("an easing function could not be written", text, input);
      }
      gtext_css_serialize_buffer_free(nullptr, eb);
      if (e->kind == GTEXT_CSS_EASING_LINEAR_POINTS) {
        for (size_t i = 1; i < e->point_count; i++) {
          if (e->points[i].input < e->points[i - 1].input) {
            fail("the control points of linear() are not in order", text, input);
          }
        }
      }
    }
    gtext_css_easing_free(e);
    GTEXT_CSS_Gradient * g = nullptr;
    if (gtext_css_gradient_from_value(root, 0, nullptr, &g) == GTEXT_CSS_OK) {
      char * gb = nullptr;
      size_t gl = 0;
      if (gtext_css_gradient_serialize_buffer(g, nullptr, &gb, &gl) != GTEXT_CSS_OK) {
        fail("a gradient could not be written", text, input);
      }
      gtext_css_serialize_buffer_free(nullptr, gb);
      gtext_css_gradient_free(g);
    }
    GTEXT_CSS_Image_Set_Option * opts = nullptr;
    size_t no = 0;
    if (gtext_css_image_set_options(root, nullptr, &opts, &no) == GTEXT_CSS_OK) {
      gtext_allocator_free(gtext_allocator_default(), opts);
    }
    GTEXT_CSS_Shape shape;
    (void)gtext_css_shape_from_value(root, &shape);
    GTEXT_CSS_Transform_Function tf;
    (void)gtext_css_transform_function_from_value(root, &tf);
    if (root->kind == GTEXT_CSS_VAL_LIST) {
      for (size_t i = 0; i < root->child_count; i++) {
        (void)gtext_css_transform_function_from_value(&root->children[i], &tf);
      }
    }
    const bool pending = text.find("var(") != std::string::npos || text.find("env(") != std::string::npos || text.find("attr(") != std::string::npos
        || text.find("VAR(") != std::string::npos;
    if (!pending) {
      GTEXT_CSS_Tree * back = nullptr;
      if (gtext_css_parse_component_values(text.data(), text.size(), &po, &back, nullptr) == GTEXT_CSS_OK) {
        bool m2 = false;
        GTEXT_CSS_Value_Set * s2 = nullptr;
        const GTEXT_CSS_Status st2 = gtext_css_match(c, back->values, back->value_count, &mo, &m2, &s2, nullptr);
        if (st2 == GTEXT_CSS_OK) {
          if (!m2) {
            fail("a written value does not match again", text, input);
          }
          char * b2 = nullptr;
          size_t l2 = 0;
          if (gtext_css_serialize_value_buffer(gtext_css_value_set_root(s2), nullptr, &b2, &l2) != GTEXT_CSS_OK || std::string(b2, l2) != text) {
            fail("a written value is not written again as the same text", text, input);
          }
          gtext_css_serialize_buffer_free(nullptr, b2);
        }
        gtext_css_value_set_free(s2);
      }
      gtext_css_tree_free(back);
    }
  }
  gtext_css_value_set_free(set);
}

struct Compiled {
  GTEXT_CSS_Function_Types * types = nullptr;
  GTEXT_CSS_Compiled * grammars[sizeof kGrammars / sizeof kGrammars[0]] = {};
  Compiled() {
    if (gtext_css_function_types_new(nullptr, &types) != GTEXT_CSS_OK) {
      fail("the function types could not be made", "", "");
    }
    for (size_t i = 0; i < sizeof kGrammars / sizeof kGrammars[0]; i++) {
      const GTEXT_CSS_Status st = gtext_css_compile(kGrammars[i], std::strlen(kGrammars[i]), gtext_css_function_types_resolve, types, nullptr,
          &grammars[i], nullptr);
      if (st != GTEXT_CSS_OK) {
        fail("a grammar of the harness does not compile", kGrammars[i], "");
      }
    }
  }
  ~Compiled() {
    for (auto * g : grammars) {
      gtext_css_compiled_free(g);
    }
    gtext_css_function_types_free(types);
  }
};

int entry(const uint8_t * data, size_t size) {
  static Compiled compiled;
  if (size < 3) {
    return 0;
  }
  const uint8_t o0 = data[0], o1 = data[1], o2 = data[2];
  const std::string input(reinterpret_cast<const char *>(data), size);
  const std::string css(reinterpret_cast<const char *>(data) + 3, size - 3);
  const size_t tiny = (size_t)(o1 % 64) + 1;

  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  po.max_depth = 4096;
  po.max_input_bytes = 1 << 20;
  GTEXT_CSS_Tree * tree = nullptr;
  if (gtext_css_parse_component_values(css.data(), css.size(), &po, &tree, nullptr) != GTEXT_CSS_OK) {
    return 0;
  }
  GTEXT_CSS_Math_Options mo = gtext_css_math_options_default();
  if (o0 & 2) {
    mo.max_depth = tiny;
  }
  if (o0 & 4) {
    mo.max_nodes = tiny * 4;
  }
  GTEXT_CSS_Color_Options co = gtext_css_color_options_default();
  if (o0 & 8) {
    co.max_nesting = tiny;
  }
  for (size_t i = 0; i < tree->value_count && i < 64; i++) {
    const GTEXT_CSS_Component_Value & v = tree->values[i];
    if (v.kind == GTEXT_CSS_VALUE_FUNCTION && gtext_css_math_is_function(v.value.data, v.value.length)) {
      check_math(v, mo, po, input);
    }
    if (v.kind != GTEXT_CSS_VALUE_BLOCK && !(v.kind == GTEXT_CSS_VALUE_TOKEN && v.type == GTEXT_CSS_TOKEN_WHITESPACE)) {
      check_color(v, co, po, input);
    }
  }
  GTEXT_CSS_Match_Options match = gtext_css_match_options_default();
  match.unitless_length = (o0 & 1) != 0;
  match.max_match_work = (size_t)1 << 22;
  if (o0 & 2) {
    match.max_math_depth = tiny;
  }
  if (o0 & 4) {
    match.max_math_nodes = tiny * 4;
  }
  if (o0 & 8) {
    match.max_color_nesting = tiny;
  }
  for (GTEXT_CSS_Compiled * c : compiled.grammars) {
    if (c) {
      check_match(c, tree, match, po, input);
    }
  }
  // an allocation that fails
  if (o2) {
    FaultAllocator fa;
    fa.arm(o2);
    GTEXT_CSS_Match_Options fm = match;
    fm.allocator = &fa.alloc;
    GTEXT_CSS_Math_Options fmo = mo;
    fmo.allocator = &fa.alloc;
    GTEXT_CSS_Color_Options fco = co;
    fco.allocator = &fa.alloc;
    for (size_t i = 0; i < tree->value_count && i < 8; i++) {
      const GTEXT_CSS_Component_Value & v = tree->values[i];
      GTEXT_CSS_Math * m = nullptr;
      bool valid = false;
      if (v.kind == GTEXT_CSS_VALUE_FUNCTION && gtext_css_math_is_function(v.value.data, v.value.length)) {
        const GTEXT_CSS_Status st = gtext_css_math_parse(&v, nullptr, &fmo, &valid, &m, nullptr);
        if (st != GTEXT_CSS_OK && st != GTEXT_CSS_E_OOM && !is_hard_limit(st)) {
          fail("a math function failed under a failing allocation with a status it must not", gtext_css_status_string(st), input);
        }
        gtext_css_math_free(m);
      }
      GTEXT_CSS_Color * col = nullptr;
      if (v.kind != GTEXT_CSS_VALUE_BLOCK) {
        const GTEXT_CSS_Status st = gtext_css_color_parse(&v, &fco, &valid, &col, nullptr);
        if (st != GTEXT_CSS_OK && st != GTEXT_CSS_E_OOM && !is_hard_limit(st)) {
          fail("a color failed under a failing allocation with a status it must not", gtext_css_status_string(st), input);
        }
        if (st == GTEXT_CSS_OK && valid) {
          char * buf = nullptr;
          size_t len = 0;
          GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
          so.allocator = &fa.alloc;
          const GTEXT_CSS_Status ss = gtext_css_color_serialize_buffer(col, &so, &buf, &len);
          if (ss != GTEXT_CSS_OK && ss != GTEXT_CSS_E_OOM) {
            fail("a color could not be written under a failing allocation", gtext_css_status_string(ss), input);
          }
          if (ss == GTEXT_CSS_OK) {
            gtext_css_serialize_buffer_free(&so, buf);
          }
        }
        gtext_css_color_free(col);
      }
    }
    for (GTEXT_CSS_Compiled * c : compiled.grammars) {
      if (!c) {
        continue;
      }
      bool matched = false;
      GTEXT_CSS_Value_Set * set = nullptr;
      const GTEXT_CSS_Status st = gtext_css_match(c, tree->values, tree->value_count, &fm, &matched, &set, nullptr);
      if (st != GTEXT_CSS_OK && st != GTEXT_CSS_E_OOM && st != GTEXT_CSS_E_LIMIT_WORK && !is_hard_limit(st)) {
        fail("a match failed under a failing allocation with a status it must not", gtext_css_status_string(st), input);
      }
      if (st != GTEXT_CSS_OK && set) {
        fail("a failed match returned a set", "", input);
      }
      gtext_css_value_set_free(set);
    }
    fa.disarm();
    if (!fa.live.empty() || fa.bad_frees) {
      fail("an allocation that failed left memory behind or freed a block it did not make", std::to_string(fa.live.size()), input);
    }
  }
  gtext_css_tree_free(tree);
  return 0;
}

}  // namespace css_vfuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  return css_vfuzz::entry(data, size);
}
