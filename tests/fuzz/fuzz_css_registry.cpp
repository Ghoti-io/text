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
 * libFuzzer harness for the CSS registry and property-level parsing (src/css/css_registry.c, css_property.c, the generated tables).
 *
 * The input is four option bytes, then text:
 *
 *   o0  bits 0-1 the mode: 0 a property of the pin, by index; 1 a descriptor of the pin, by at-rule and index; 2 a name taken from the
 *       text (its first line) and the rest of the text as the value; 3 the registration of a property (the first line the grammar,
 *       the rest the initial value);
 *       bit 2 the quirks option; bits 4-7 a tiny limit on the work of a match (1 << n, 0 for the default);
 *   o1, o2  the property or descriptor (modes 0 and 1), or the name and the override flag (mode 3);
 *   o3  the allocation to fail (0 for none), counted over the one call.
 *
 * The properties:
 *
 *   a parse is OK or a limit of the match or of this layer or E_UNSUPPORTED (and only for a property whose grammar the registry
 *   reports as unusable), never E_INTERNAL or E_INVALID; **an allocation that fails is E_OOM**, with nothing returned, and nothing
 *   leaked or freed that the allocator did not make; a valid, pending or wide result serializes, and the text parses again to the
 *   same kind of result and serializes to the same text (the fixed point); a descriptor takes no CSS-wide keyword and no
 *   substitution, `all` takes the CSS-wide keywords only, and every property takes the CSS-wide keywords alone;
 *   a registration is OK or one of the statuses of a refusal, never E_INTERNAL; one that is OK freezes, and the property's initial
 *   value then parses against its grammar as a valid value; a failed registration leaves the builder as it was.
 *
 * Seeds: tests/fuzz/corpus/css-registry, made by tools/css/make-fuzz-registry-seeds.py.
 * Build with: make fuzz-css-registry      Run: make fuzz-run-css-registry
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <ghoti.io/text/css.h>

#include "../markup_test_support.h"

namespace css_rfuzz {

[[noreturn]] void fail(const char * what, const std::string & detail, const std::string & input) {
  std::fprintf(stderr, "fuzz_css_registry: %s\n%s\ninput (%zu bytes): ", what, detail.c_str(), input.size());
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
  case GTEXT_CSS_E_LIMIT_WORK:
  case GTEXT_CSS_E_LIMIT_OUTPUT:
  case GTEXT_CSS_E_LIMIT_DEPTH:
  case GTEXT_CSS_E_LIMIT_MATH_NODES:
  case GTEXT_CSS_E_LIMIT_COLOR_NESTING:
  case GTEXT_CSS_E_LIMIT_STOPS:
  case GTEXT_CSS_E_LIMIT_FUNCTION_NODES:
  case GTEXT_CSS_E_LIMIT_TOKENS:
  case GTEXT_CSS_E_LIMIT_INPUT:
    return true;
  default:
    return false;
  }
}

const GTEXT_CSS_Registry * registry() {
  static GTEXT_CSS_Registry * r = [] {
    GTEXT_CSS_Registry * made = nullptr;
    if (gtext_css_registry_new(nullptr, &made) != GTEXT_CSS_OK || gtext_css_registry_prepare(made) != GTEXT_CSS_OK) {
      fail("the registry could not be made", "", "");
    }
    return made;
  }();
  return r;
}

struct Outcome {
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  GTEXT_CSS_Property_Outcome outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
  std::string text;
  bool has_text = false;
  bool root_pending = false;  ///< the typed value is a pending substitution (whatever the outcome says)
};

/// One parse with the allocation `fault` failed (0 for none); the allocator must be clean afterward.
Outcome parse_once(const std::string & name, const std::string & value, const GTEXT_CSS_Property_Options & base, size_t fault, bool * oom,
    const std::string & input) {
  FaultAllocator fa;
  GTEXT_CSS_Property_Options o = base;
  o.allocator = &fa.alloc;
  GTEXT_CSS_Property_Value pv;
  fa.arm(fault);
  Outcome out;
  out.status = gtext_css_parse_property_text(registry(), name.data(), name.size(), value.data(), value.size(), &o, &pv);
  fa.disarm();
  *oom = fa.failed;
  if (fa.failed && out.status != GTEXT_CSS_E_OOM) {
    fail("an allocation failed and the parse did not say so", gtext_css_status_string(out.status), input);
  }
  if (!allowed_status(out.status)) {
    fail("a status a parse must not return", gtext_css_status_string(out.status), input);
  }
  out.outcome = pv.outcome;
  if (const GTEXT_CSS_Value * root = gtext_css_value_set_root(pv.set)) {
    out.root_pending = root->kind == GTEXT_CSS_VAL_PENDING;
  }
  if (out.status != GTEXT_CSS_OK) {
    if (pv.set) {
      fail("a parse that did not succeed returned a value", gtext_css_status_string(out.status), input);
    }
  }
  else if (pv.outcome == GTEXT_CSS_PROP_RESULT_VALID || pv.outcome == GTEXT_CSS_PROP_RESULT_PENDING || pv.outcome == GTEXT_CSS_PROP_RESULT_WIDE) {
    GTEXT_CSS_Serialize_Options so = gtext_css_serialize_options_default();
    char * buf = nullptr;
    size_t n = 0;
    const GTEXT_CSS_Status ss = gtext_css_property_value_serialize_buffer(&pv, &so, &buf, &n);
    if (ss != GTEXT_CSS_OK) {
      fail("a result could not be written", gtext_css_status_string(ss), input);
    }
    out.text.assign(buf, n);
    out.has_text = true;
    gtext_css_serialize_buffer_free(&so, buf);
  }
  else if (pv.set) {
    fail("a result with no value holds one", "", input);
  }
  gtext_css_property_value_free(&pv);
  if (!fa.live.empty()) {
    fail("the parse leaked", std::to_string(fa.live.size()) + " blocks", input);
  }
  if (fa.bad_frees) {
    fail("the parse freed a block it did not make", "", input);
  }
  return out;
}

void check_property(const std::string & name, const std::string & value, const GTEXT_CSS_Property_Options & base, size_t fault, bool is_descriptor,
    const std::string & input) {
  bool oom = false;
  const Outcome first = parse_once(name, value, base, fault, &oom, input);
  if (first.status == GTEXT_CSS_E_UNSUPPORTED && !is_descriptor) {
    const GTEXT_CSS_Property_Id id = gtext_css_registry_find_property(registry(), name.data(), name.size());
    GTEXT_CSS_Grammar_State s = GTEXT_CSS_GRAMMAR_OK;
    if (!id || gtext_css_registry_property_state(registry(), id, &s, nullptr) != GTEXT_CSS_OK || s == GTEXT_CSS_GRAMMAR_OK) {
      fail("E_UNSUPPORTED for a property whose grammar is usable", name, input);
    }
  }
  if (first.status != GTEXT_CSS_OK) {
    return;
  }
  if (is_descriptor && (first.outcome == GTEXT_CSS_PROP_RESULT_WIDE || first.outcome == GTEXT_CSS_PROP_RESULT_PENDING || first.root_pending)) {
    fail("a descriptor took a CSS-wide keyword or a substitution", value, input);
  }
  if (!is_descriptor && name.size() == 3 && (name[0] | 0x20) == 'a' && (name[1] | 0x20) == 'l' && (name[2] | 0x20) == 'l' && first.outcome != GTEXT_CSS_PROP_RESULT_WIDE &&
      first.outcome != GTEXT_CSS_PROP_RESULT_INVALID) {
    fail("`all` took something that is not a CSS-wide keyword", value, input);
  }
  if (!first.has_text) {
    return;
  }
  // the fixed point: what was written parses to the same kind of result and is written as the same text
  bool oom2 = false;
  GTEXT_CSS_Property_Options plain = base;
  const Outcome second = parse_once(name, first.text, plain, 0, &oom2, input);
  if (second.status == GTEXT_CSS_E_LIMIT_WORK || second.status == GTEXT_CSS_E_LIMIT_OUTPUT) {
    return;  // a tiny limit may be reached by the second text and not the first
  }
  if (second.status != GTEXT_CSS_OK || second.outcome != first.outcome) {
    fail("a written value does not parse as the same kind of result", first.text, input);
  }
  if (second.text != first.text) {
    fail("a written value is not written again as the same text", first.text + "  ->  " + second.text, input);
  }
}

void check_registration(const std::string & css, uint8_t o1, uint8_t o2, size_t fault, const std::string & input) {
  const size_t nl = css.find('\n');
  const std::string grammar = nl == std::string::npos ? css : css.substr(0, nl);
  const std::string initial = nl == std::string::npos ? std::string() : css.substr(nl + 1);
  FaultAllocator fa;
  GTEXT_CSS_Registry_Options ro = gtext_css_registry_options_default();
  ro.allocator = &fa.alloc;
  GTEXT_CSS_Registry_Builder * b = nullptr;
  if (gtext_css_registry_builder_new(&ro, &b) != GTEXT_CSS_OK) {
    fail("a builder could not be made", "", input);
  }
  const std::string name = (o1 & 8) ? "width" : "fuzz-prop-" + std::to_string(o1 & 7);
  GTEXT_CSS_Property_Def d;
  std::memset(&d, 0, sizeof d);
  d.name = name.c_str();
  d.grammar = grammar.c_str();
  d.initial = initial.c_str();
  d.override = (o2 & 1) != 0;
  d.inherited = (o2 & 2) != 0;
  GTEXT_CSS_Registry_Result res;
  fa.arm(fault);
  const GTEXT_CSS_Status st = gtext_css_registry_register_property(b, &d, &res);
  fa.disarm();
  if (fa.failed && st != GTEXT_CSS_E_OOM) {
    fail("an allocation failed and the registration did not say so", gtext_css_status_string(st), input);
  }
  switch (st) {
  case GTEXT_CSS_OK:
  case GTEXT_CSS_E_OOM:
  case GTEXT_CSS_E_INVALID:
  case GTEXT_CSS_E_EXISTS:
  case GTEXT_CSS_E_GRAMMAR:
  case GTEXT_CSS_E_REFERENCE:
  case GTEXT_CSS_E_LEFT_RECURSION:
  case GTEXT_CSS_E_LIMIT_DEFINITION:
  case GTEXT_CSS_E_LIMIT_DEFINITION_DEPTH:
  case GTEXT_CSS_E_LIMIT_DEFINITION_NODES:
  case GTEXT_CSS_E_LIMIT_GROUP:
  case GTEXT_CSS_E_LIMIT_WORK:
  case GTEXT_CSS_E_LIMIT_REGISTRY:
  case GTEXT_CSS_E_LIMIT_ATOMS:
  case GTEXT_CSS_E_LIMIT_OUTPUT:
  case GTEXT_CSS_E_LIMIT_DEPTH:
  case GTEXT_CSS_E_LIMIT_MATH_NODES:
  case GTEXT_CSS_E_LIMIT_COLOR_NESTING:
  case GTEXT_CSS_E_LIMIT_STOPS:
  case GTEXT_CSS_E_LIMIT_FUNCTION_NODES:
    break;
  default:
    fail("a status a registration must not return", gtext_css_status_string(st), input);
  }
  GTEXT_CSS_Registry * r = nullptr;
  const GTEXT_CSS_Status fs = gtext_css_registry_freeze(b, &r, nullptr);
  if (fs != GTEXT_CSS_OK) {
    gtext_css_registry_builder_free(b);
    if (fs != GTEXT_CSS_E_OOM) {
      fail("a builder did not freeze", gtext_css_status_string(fs), input);
    }
  }
  else {
    if (st == GTEXT_CSS_OK) {
      GTEXT_CSS_Property_Value pv;
      const GTEXT_CSS_Status ps = gtext_css_parse_property_text(r, name.data(), name.size(), initial.data(), initial.size(), nullptr, &pv);
      if (ps != GTEXT_CSS_OK || pv.outcome != GTEXT_CSS_PROP_RESULT_VALID) {
        fail("the initial value of a registered property is not a valid value of it", gtext_css_status_string(ps) + std::string(": ") + initial, input);
      }
      gtext_css_property_value_free(&pv);
      if (gtext_css_registry_property_count(r) != gtext_css_registry_generated_property_count() + 1) {
        fail("a registration did not add exactly one property", "", input);
      }
    }
    else if (gtext_css_registry_property_count(r) != gtext_css_registry_generated_property_count()) {
      fail("a refused registration left a property behind", gtext_css_status_string(st), input);
    }
    gtext_css_registry_free(r);
  }
  if (!fa.live.empty()) {
    fail("the registration leaked", std::to_string(fa.live.size()) + " blocks", input);
  }
  if (fa.bad_frees) {
    fail("the registration freed a block it did not make", "", input);
  }
}

int entry(const uint8_t * data, size_t size) {
  if (size < 4) {
    return 0;
  }
  const uint8_t o0 = data[0], o1 = data[1], o2 = data[2], o3 = data[3];
  const std::string input(reinterpret_cast<const char *>(data), size);
  const std::string css(reinterpret_cast<const char *>(data) + 4, size - 4);
  const unsigned mode = o0 & 3;

  if (mode == 3) {
    check_registration(css, o1, o2, o3, input);
    return 0;
  }
  GTEXT_CSS_Match_Options mo = gtext_css_match_options_default();
  GTEXT_CSS_Property_Options po;
  std::memset(&po, 0, sizeof po);
  po.quirks = (o0 & 4) != 0;
  if ((o0 >> 4) != 0) {
    mo.max_match_work = (size_t)1 << (o0 >> 4);
    po.match = &mo;
  }
  const GTEXT_CSS_Registry * r = registry();
  if (mode == 0) {
    const size_t n = gtext_css_registry_property_count(r);
    GTEXT_CSS_Property_Info info;
    if (!gtext_css_registry_property_info(r, (GTEXT_CSS_Property_Id)(((size_t)o1 << 8 | o2) % n + 1), &info)) {
      fail("a property of the registry has no information", "", input);
    }
    check_property(std::string(info.name, info.name_length), css, po, o3, false, input);
  }
  else if (mode == 1) {
    const size_t na = gtext_css_registry_at_rule_count();
    GTEXT_CSS_At_Rule_Info ai;
    size_t a = o1 % na + 1;
    for (size_t k = 0; k < na; k++, a = a % na + 1) {
      if (gtext_css_registry_at_rule_info((GTEXT_CSS_At_Rule_Id)a, &ai) && ai.descriptor_count > 0) {
        break;
      }
    }
    GTEXT_CSS_Descriptor_Info di;
    if (!gtext_css_registry_descriptor_info(gtext_css_registry_descriptor_at((GTEXT_CSS_At_Rule_Id)a, o2 % ai.descriptor_count), &di)) {
      fail("a descriptor has no information", "", input);
    }
    po.at_rule = ai.name;
    check_property(std::string(di.name, di.name_length), css, po, o3, true, input);
  }
  else {
    const size_t nl = css.find('\n');
    const std::string name = css.substr(0, nl == std::string::npos ? css.size() : std::min<size_t>(nl, 64));
    const std::string value = nl == std::string::npos ? std::string() : css.substr(nl + 1);
    check_property(name, value, po, o3, false, input);
  }
  return 0;
}

}  // namespace css_rfuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  return css_rfuzz::entry(data, size);
}
