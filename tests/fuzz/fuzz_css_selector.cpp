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
 * libFuzzer harness for the selector parser, serializer and specificity (src/css/css_selector_*.c).
 *
 * The input is three option bytes, then the selector text:
 *
 *   o0  bits 0-2 the mode (0 complex, 1 forgiving, 2 relative, 3 compound, 4 real; 5 to 7 are 0, 1 and 2 again);
 *       bit 3 the namespaces are declared (a prefix `svg` and `h`, and a default namespace when bit 4 is also set);
 *       bit 5 the limits are small (a few nodes, a short list, shallow nesting) so that every limit path is reached;
 *   o1  reserved (zero);
 *   o2  the allocation to fail (0 for none), counted over the one parse.
 *
 * The properties:
 *
 *   a status is OK or a limit, or **E_OOM for an allocation that failed**, with no list and nothing leaked or freed that the allocator did not make - never E_INTERNAL;
 *   an invalid selector is OK with no list and at least one diagnostic whose span lies inside the input;
 *   **the serialization is a fixed point**: a valid selector, written and read in the same mode with the same namespaces, gives the same selector - the same writing, the
 *   same specificity, the same dependencies and the same nodes (the counts of complex selectors, compounds and simple selectors);
 *   every node has a distinct id, from 1, and a span inside the input;
 *   the summaries agree with the nodes: the specificity of a list is the highest of its complex selectors' that are not raw entries, the dependencies of a list contain those
 *   of its complex selectors;
 *   a retained list is the same list: its writing through the second owner is the first's, and it outlives the first owner's release;
 *   a nesting selector does not change a selector's writing, and the specificity with `&` as zero is no more than with `&` as (1,0,0).
 *
 * Seeds: tests/fuzz/corpus/css-selector, made by tools/css/make-fuzz-selector-seeds.py.
 * Build with: make fuzz-css-selector      Run: make fuzz-run-css-selector
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <ghoti.io/text/css.h>

#include "../markup_test_support.h"

namespace css_selfuzz {

[[noreturn]] void fail(const char * what, const std::string & detail, const std::string & input) {
  std::fprintf(stderr, "%s: %s\n", "fuzz_css_selector", what);
  for (unsigned char c : detail) {
    if ((c >= 0x20 && c < 0x7f) || c == '\n') {
      std::fputc(c, stderr);
    }
    else {
      std::fprintf(stderr, "\\x%02x", c);
    }
  }
  std::fputc('\n', stderr);
  std::fprintf(stderr, "input (%zu bytes): ", input.size());
  for (unsigned char c : input) {
    std::fprintf(stderr, "%02x", c);
  }
  std::fputc('\n', stderr);
  std::abort();
}

GTEXT_Markup_Status collect(void * user, const char * bytes, size_t len) {
  static_cast<std::string *>(user)->append(bytes, len);
  return GTEXT_MARKUP_OK;
}

std::string write_list(const GTEXT_CSS_Selector_List * l) {
  std::string out;
  GTEXT_Markup_Sink sink{collect, &out};
  gtext_css_selector_serialize(l, &sink);
  return out;
}

struct Counts {
  size_t complexes = 0, compounds = 0, simples = 0;
  bool operator==(const Counts & o) const {
    return complexes == o.complexes && compounds == o.compounds && simples == o.simples;
  }
};

struct Walk {
  std::set<GTEXT_CSS_Id> ids;
  Counts counts;
  size_t input_size = 0;
  const std::string * input = nullptr;
  bool bad_id = false, bad_span = false, bad_spec = false, bad_dep = false;
};

void walk_list(const GTEXT_CSS_Selector_List * l, Walk & w);

void note(Walk & w, GTEXT_CSS_Id id, const GTEXT_CSS_Span & s) {
  if (id == 0 || !w.ids.insert(id).second) {
    w.bad_id = true;
  }
  if (s.end < s.offset || s.end > w.input_size) {
    w.bad_span = true;
  }
}

void walk_list(const GTEXT_CSS_Selector_List * l, Walk & w) {
  note(w, l->id, l->span);
  GTEXT_CSS_Specificity best{0, 0, 0};
  uint32_t deps = 0;
  for (size_t i = 0; i < l->count; i++) {
    const GTEXT_CSS_Complex & c = l->complex[i];
    note(w, c.id, c.span);
    w.counts.complexes++;
    if (!c.raw.data) {
      if (gtext_css_specificity_compare(c.specificity, best) > 0) {
        best = c.specificity;
      }
      if (c.compound_count == 0) {
        w.bad_id = true;
      }
    }
    deps |= c.dependencies;
    for (size_t j = 0; j < c.compound_count; j++) {
      const GTEXT_CSS_Compound & cm = c.compounds[j];
      note(w, cm.id, cm.span);
      w.counts.compounds++;
      for (size_t k = 0; k < cm.simple_count; k++) {
        const GTEXT_CSS_Simple & s = cm.simples[k];
        note(w, s.id, s.span);
        if (!(s.kind == GTEXT_CSS_SIMPLE_UNIVERSAL && cm.simple_count > 1)) {
          w.counts.simples++; /* a universal selector that others follow is not written, so it is not counted */
        }
        if (s.list) {
          walk_list(s.list, w);
          if (s.list->dependencies & ~c.dependencies) {
            w.bad_dep = true;
          }
        }
      }
    }
  }
  if (gtext_css_specificity_compare(best, l->max_specificity) != 0) {
    w.bad_spec = true;
  }
  if (deps != l->dependencies) {
    w.bad_dep = true;
  }
}

const GTEXT_CSS_Namespace kNamespaces[] = {{"svg", "http://www.w3.org/2000/svg"}, {"h", "http://www.w3.org/1999/xhtml"}};

GTEXT_CSS_Selector_Options options_of(uint8_t o0, const GTEXT_Allocator * alloc, GTEXT_CSS_Namespace_Map * map) {
  GTEXT_CSS_Selector_Options o = gtext_css_selector_options_default();
  o.allocator = alloc;
  const unsigned m = o0 & 7u;
  o.mode = static_cast<GTEXT_CSS_Selector_Mode>(m > 4 ? m - 5 : m);
  if (o0 & 8u) {
    map->entries = kNamespaces;
    map->count = 2;
    map->default_uri = (o0 & 16u) ? "http://www.w3.org/1999/xhtml" : nullptr;
    o.namespaces = map;
  }
  if (o0 & 32u) {
    o.max_components = 24;
    o.max_list = 4;
    o.max_depth = 3;
    o.max_diagnostics = 4;
  }
  return o;
}

bool limit_status(GTEXT_CSS_Status st) {
  return st == GTEXT_CSS_E_LIMIT_SELECTOR_COMPONENTS || st == GTEXT_CSS_E_LIMIT_SELECTOR_LIST || st == GTEXT_CSS_E_LIMIT_SELECTOR_DEPTH ||
      st == GTEXT_CSS_E_LIMIT_INPUT || st == GTEXT_CSS_E_LIMIT_TOKENS || st == GTEXT_CSS_E_LIMIT_DEPTH || st == GTEXT_CSS_E_LIMIT_RULES;
}

int run(const uint8_t * data, size_t size) {
  if (size < 3) {
    return 0;
  }
  const std::string input(reinterpret_cast<const char *>(data), size);
  const uint8_t o0 = data[0], o2 = data[2];
  const std::string text(input, 3);
  if (text.size() > 4096) {
    return 0;
  }
  FaultAllocator fa;
  GTEXT_CSS_Namespace_Map map{};
  const GTEXT_CSS_Selector_Options o = options_of(o0, &fa.alloc, &map);
  GTEXT_CSS_Selector_List * list = nullptr;
  GTEXT_CSS_Selector_Result r;
  std::memset(&r, 0, sizeof r);
  fa.arm(o2);
  GTEXT_CSS_Status st = gtext_css_selector_parse(text.data(), text.size(), &o, &list, &r);
  const bool injected = fa.failed;
  fa.disarm();
  if (st == GTEXT_CSS_E_INTERNAL) {
    fail("an internal error", r.message, input);
  }
  if (st != GTEXT_CSS_OK) {
    if (list) {
      fail("a failed parse returned a list", gtext_css_status_string(st), input);
    }
    if (!(st == GTEXT_CSS_E_OOM && injected) && !limit_status(st) && st != GTEXT_CSS_E_OOM) {
      fail("a status that is neither a limit nor an out of memory", gtext_css_status_string(st), input);
    }
    gtext_css_selector_result_free(&r);
  }
  else if (injected && !list && r.diagnostic_total == 0) {
    /* an allocation failed and the parse swallowed it as an answer: possible only if it was a scratch the parse recovered from; it must be a real answer */
    fail("an allocation failed and the parse did not say so", "", input);
  }
  else if (!list) {
    if (gtext_css_selector_result_diagnostic_count(&r) == 0 || r.diagnostic_total == 0) {
      fail("an invalid selector without a diagnostic", text, input);
    }
    GTEXT_CSS_Selector_Diagnostic d;
    for (size_t i = 0; gtext_css_selector_result_diagnostic(&r, i, &d); i++) {
      if (d.span.end < d.span.offset || d.span.end > text.size() || !d.reference) {
        fail("a diagnostic outside the input", text + "\nspan " + std::to_string(d.span.offset) + "-" + std::to_string(d.span.end) + " of " + std::to_string(text.size()) +
            (d.reference ? "" : " with no reference"), input);
      }
    }
    gtext_css_selector_result_free(&r);
  }
  else {
    if (injected) {
      fail("an allocation failed and the parse still returned a list", "", input);
    }
    Walk w;
    w.input_size = text.size();
    w.input = &text;
    walk_list(list, w);
    if (w.bad_id) {
      fail("node ids are not distinct, or a complex selector is empty", text, input);
    }
    if (w.bad_span) {
      fail("a span outside the input", text, input);
    }
    if (w.bad_spec) {
      fail("the specificity of a list is not the highest of its selectors'", text, input);
    }
    if (w.bad_dep) {
      fail("the dependencies of a list are not those of its selectors", text, input);
    }
    const std::string first = write_list(list);
    GTEXT_CSS_Selector_Options o2opt = o;
    o2opt.allocator = nullptr;
    o2opt.max_components = 0;
    o2opt.max_list = 0;
    o2opt.max_depth = 0;
    GTEXT_CSS_Selector_List * again = nullptr;
    GTEXT_CSS_Selector_Result r2;
    std::memset(&r2, 0, sizeof r2);
    const GTEXT_CSS_Status st2 = gtext_css_selector_parse(first.data(), first.size(), &o2opt, &again, &r2);
    if (st2 != GTEXT_CSS_OK && !limit_status(st2)) {
      fail("the writing could not be read", first, input);
    }
    if (st2 == GTEXT_CSS_OK) {
      if (!again) {
        fail("the writing of a selector is not a selector", first, input);
      }
      const std::string second = write_list(again);
      if (second != first) {
        fail("the serialization is not a fixed point", first + "\n" + second, input);
      }
      if (gtext_css_specificity_compare(again->max_specificity, list->max_specificity) != 0) {
        fail("the specificity changed on the round trip", first, input);
      }
      if (again->dependencies != list->dependencies) {
        fail("the dependencies changed on the round trip", first, input);
      }
      Walk w2;
      w2.input_size = first.size();
      w2.input = &first;
      walk_list(again, w2);
      if (!(w2.counts == w.counts)) {
        fail("the nodes changed on the round trip", first, input);
      }
      const auto zero = gtext_css_selector_list_specificity(again, GTEXT_CSS_Specificity{0, 0, 0});
      const auto big = gtext_css_selector_list_specificity(again, GTEXT_CSS_Specificity{1, 0, 0});
      if (gtext_css_specificity_compare(zero, big) > 0) {
        fail("a larger `&` made a selector less specific", first, input);
      }
      gtext_css_selector_list_free(again);
    }
    gtext_css_selector_result_free(&r2);
    GTEXT_CSS_Selector_List * second_owner = gtext_css_selector_list_retain(list);
    gtext_css_selector_list_free(list);
    if (write_list(second_owner) != first) {
      fail("a retained list is not the list", first, input);
    }
    gtext_css_selector_list_free(second_owner);
    gtext_css_selector_result_free(&r);
  }
  if (!fa.live.empty()) {
    fail("an allocation leaked", std::to_string(fa.live.size()) + " blocks", input);
  }
  if (fa.bad_frees) {
    fail("a block the allocator did not make was freed", "", input);
  }
  return 0;
}

}  // namespace css_selfuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  return css_selfuzz::run(data, size);
}
