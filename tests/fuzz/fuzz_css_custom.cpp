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
 * libFuzzer harness for arbitrary substitution, custom properties and `@property` (src/css/css_substitute.c, css_rope.c, css_custom.c, css_creg.c,
 * css_syntax_string.c, css_at_property.c).
 *
 * The input is four option bytes, then text:
 *
 *   o0  bits 0-1 the mode: 0 a block of declarations (`name: value`, separated by `;` or a line feed) computed as a root; 1 a parent block and a child block
 *       (the text, the two separated by a line `---`) computed; 2 a style sheet (the text, up to a line `---`) whose `@property` rules are collected and then
 *       used to compute the declarations after the line; 3 the same declarations given to a block and to the cascade, which must compute alike;
 *       bit 2 the limits are small (a few hundred tokens, a few dozen custom properties) so that every limit path is reached; bit 3 the environment has an
 *       `env()` variable, a condition callback and a random-base callback;
 *   o1, o2 reserved (zero);
 *   o3  the allocation to fail (0 for none), counted over the one computation (the context is made first, on the same allocator, unarmed).
 *
 * The properties:
 *
 *   a status is OK, E_UNSUPPORTED, or a limit - never E_INTERNAL; **an allocation that fails is E_OOM**, with no style returned and nothing leaked or freed that
 *   the allocator did not make; a failed computation returns no style;
 *   a style computed twice from the same inputs dumps the same text and hashes the same (nothing depends on a pointer, a clock or the order of a table);
 *   the counters keep their promises: no substituted value is longer than the budget allows (`substitution_peak`), and the work is linear in the text written
 *   (a constant times its length, with the room a log factor needs);
 *   **a computed value is a fixed point**: the properties and the custom properties of a style, written as declarations of the same element, compute to the
 *   same style (skipped where the text of a value holds a character the block reader splits on);
 *   the block API and the cascade agree: the same declarations through gtext_css_candidates_add_text() and the cascade, and through gtext_css_block_add_text(),
 *   compute to the same custom properties and the same values of a few ordinary properties;
 *   the `@property` rules of a style sheet are collected deterministically, every rule found by name is the one at its index, and the diagnostics are no more
 *   than were counted.
 *
 * Seeds: tests/fuzz/corpus/css-custom, made by tools/css/make-fuzz-custom-seeds.py.
 * Build with: make fuzz-css-custom      Run: make fuzz-run-css-custom
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <ghoti.io/text/css.h>

#include "../markup_test_support.h"

namespace css_custfuzz {

[[noreturn]] void fail(const char * what, const std::string & detail, const std::string & input) {
  std::fprintf(stderr, "fuzz_css_custom: %s\n%s\ninput (%zu bytes): ", what, detail.c_str(), input.size());
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
  case GTEXT_CSS_E_LIMIT_RULES:
    return true;
  default:
    return false;
  }
}

// The limits the small mode uses, and the ones it is held to.
constexpr uint64_t kSmallTokens = 400, kSmallElement = 3000;
constexpr size_t kSmallCustoms = 24, kSmallBytes = 2048, kSmallDepth = 40;
constexpr uint64_t kDefaultTokens = 1048576;

const GTEXT_CSS_Registry * registry() {
  static GTEXT_CSS_Registry * r = [] {
    GTEXT_CSS_Registry_Builder * b = nullptr;
    GTEXT_CSS_Registry * made = nullptr;
    if (gtext_css_registry_builder_new(nullptr, &b) != GTEXT_CSS_OK) {
      fail("the registry builder could not be made", "", "");
    }
    struct Def { const char * name; const char * syntax; bool inherits; const char * initial; };
    const Def defs[] = {
        {"--len", "<length>", true, "0px"},
        {"--col", "<color>", false, "red"},
        {"--num", "<number> | auto", true, "auto"},
        {"--list", "<length>#", false, "1px, 2px"},
        {"--any", "*", true, nullptr},
        {"--id", "<custom-ident>+", false, "x"},
    };
    for (const Def & d : defs) {
      GTEXT_CSS_Custom_Def def;
      std::memset(&def, 0, sizeof def);
      def.name = d.name;
      def.syntax = d.syntax;
      def.inherits = d.inherits;
      def.initial_value = d.initial;
      if (gtext_css_registry_register_custom(b, &def, nullptr) != GTEXT_CSS_OK) {
        fail("a registration was refused", d.name, "");
      }
    }
    if (gtext_css_registry_freeze(b, &made, nullptr) != GTEXT_CSS_OK) {
      fail("the registry could not be frozen", "", "");
    }
    return made;
  }();
  return r;
}

bool env_variable(void *, const GTEXT_CSS_String * name, const uint32_t *, size_t, GTEXT_CSS_String * out) {
  static const char five[] = "5px";
  static const char pair[] = "1px 2px";
  if (name->length == 2 && std::memcmp(name->data, "e1", 2) == 0) {
    out->data = five;
    out->length = sizeof five - 1;
    return true;
  }
  if (name->length == 2 && std::memcmp(name->data, "e2", 2) == 0) {
    out->data = pair;
    out->length = sizeof pair - 1;
    return true;
  }
  return false;
}

GTEXT_CSS_Tristate condition(void *, GTEXT_CSS_Condition_Kind, const GTEXT_CSS_String * text, GTEXT_CSS_Element_Handle) {
  return text->length % 3 == 0 ? GTEXT_CSS_TRI_TRUE : text->length % 3 == 1 ? GTEXT_CSS_TRI_FALSE : GTEXT_CSS_TRI_UNKNOWN;
}

bool random_base(void *, const GTEXT_CSS_Random_Key * key, double * base) {
  *base = (double)(key->name.length % 8) / 8.0;
  return true;
}

GTEXT_CSS_Compute_Options options_of(uint8_t o0, const GTEXT_Allocator * alloc, const GTEXT_CSS_At_Property_Set * rules) {
  GTEXT_CSS_Compute_Options o = gtext_css_compute_options_default();
  o.allocator = alloc;
  o.property_rules = rules;
  if (o0 & 4) {
    o.substitute.max_substituted_tokens = kSmallTokens;
    o.substitute.max_element_substituted_tokens = kSmallElement;
    o.substitute.max_custom_properties = kSmallCustoms;
    o.substitute.max_value_bytes = kSmallBytes;
    o.substitute.max_substitution_depth = kSmallDepth;
  }
  return o;
}

GTEXT_CSS_Environment environment_of(uint8_t o0) {
  GTEXT_CSS_Environment e = gtext_css_environment_default();
  if (o0 & 8) {
    e.env_variable = env_variable;
    e.condition = condition;
    e.random_base = random_base;
    e.random_seed = 7;
  }
  return e;
}

/// A context for the options, made once per combination (it computes the initial value of every property).
GTEXT_CSS_Compute_Context * shared_context(uint8_t o0) {
  static GTEXT_CSS_Compute_Context * contexts[4] = {nullptr, nullptr, nullptr, nullptr};
  GTEXT_CSS_Compute_Context *& c = contexts[(o0 >> 2) & 3];
  if (!c) {
    const GTEXT_CSS_Environment env = environment_of(o0);
    const GTEXT_CSS_Compute_Options o = options_of(o0, nullptr, nullptr);
    GTEXT_CSS_Compute_Result r{};
    if (gtext_css_compute_context_new(registry(), &env, &o, &c, &r) != GTEXT_CSS_OK) {
      fail("a context could not be made", r.name, "");
    }
  }
  return c;
}

struct Decl {
  std::string name, value;
};

/// The declarations of a text: `name: value` separated by `;` or a line feed.
std::vector<Decl> declarations(const std::string & text) {
  std::vector<Decl> out;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find_first_of(";\n", pos);
    if (end == std::string::npos) {
      end = text.size();
    }
    const std::string decl = text.substr(pos, end - pos);
    pos = end + 1;
    const size_t colon = decl.find(':');
    if (colon == std::string::npos || colon > 64) {
      continue;
    }
    Decl d{decl.substr(0, colon), decl.substr(colon + 1)};
    while (!d.name.empty() && d.name.front() == ' ') {
      d.name.erase(d.name.begin());
    }
    while (!d.value.empty() && d.value.front() == ' ') {
      d.value.erase(d.value.begin());
    }
    if (!d.name.empty()) {
      out.push_back(d);
    }
  }
  return out;
}

GTEXT_CSS_Status build(const std::vector<Decl> & decls, const GTEXT_Allocator * alloc, GTEXT_CSS_Declaration_Block ** out, const std::string & input) {
  GTEXT_CSS_Declaration_Options o = gtext_css_declaration_options_default();
  o.allocator = alloc;
  GTEXT_CSS_Status st = gtext_css_block_new(registry(), &o, out);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSS_Property_Options po;
  std::memset(&po, 0, sizeof po);
  po.allocator = alloc;
  for (const Decl & d : decls) {
    GTEXT_CSS_Property_Outcome outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
    st = gtext_css_block_add_text(*out, d.name.data(), d.name.size(), d.value.data(), d.value.size(), false, &po, &outcome, nullptr);
    if (st != GTEXT_CSS_OK) {
      if (!allowed_status(st)) {
        fail("a status a declaration must not return", gtext_css_status_string(st), input);
      }
      return st;
    }
  }
  return GTEXT_CSS_OK;
}

std::string dump_of(const GTEXT_CSS_Computed_Style * s, const std::string & input) {
  std::string out;
  GTEXT_Markup_Sink sink;
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

/// The properties and the custom properties of a style, written as declarations. `ok` is false when a value holds a character that the block reader splits on.
std::string declarations_of(const GTEXT_CSS_Computed_Style * s, bool * ok) {
  std::string out;
  const GTEXT_CSS_Registry * r = registry();
  const size_t n = gtext_css_registry_property_count(r);
  for (size_t id = 1; id <= n; id++) {
    GTEXT_CSS_Property_Info info;
    if (!gtext_css_registry_property_info(r, (GTEXT_CSS_Property_Id)id, &info) || (info.flags & (GTEXT_CSS_PROP_SHORTHAND | GTEXT_CSS_PROP_ALIAS))) {
      continue;
    }
    const GTEXT_CSS_Value * root = gtext_css_computed_root(s, (GTEXT_CSS_Property_Id)id);
    if (!root) {
      continue;
    }
    char * buf = nullptr;
    size_t len = 0;
    if (gtext_css_serialize_value_buffer(root, nullptr, &buf, &len) != GTEXT_CSS_OK) {
      continue;
    }
    out.append(info.name, info.name_length);
    out += ": ";
    out.append(buf, len);
    out += ";\n";
    gtext_css_serialize_buffer_free(nullptr, buf);
  }
  for (size_t i = 0; i < gtext_css_computed_custom_count(s); i++) {
    GTEXT_CSS_String name;
    const GTEXT_CSS_Custom_Value * v = nullptr;
    if (!gtext_css_computed_custom_at(s, i, &name, &v)) {
      continue;
    }
    char * buf = nullptr;
    size_t len = 0;
    if (gtext_css_custom_value_text(v, nullptr, &buf, &len) != GTEXT_CSS_OK) {
      continue;
    }
    const std::string text(buf, len);
    gtext_css_serialize_buffer_free(nullptr, buf);
    if (text.find_first_of(";\n\r\f") != std::string::npos) {
      *ok = false;
    }
    out.append(name.data, name.length);
    out += ": ";
    out += text;
    out += ";\n";
  }
  return out;
}

void check_counters(const GTEXT_CSS_Compute_Result & r, uint8_t o0, size_t input_size, const std::string & input) {
  const uint64_t budget = (o0 & 4) ? kSmallTokens : kDefaultTokens;
  // a value is built only when it fits the budget, and a declared value is as long as the text that declared it
  if (r.substitution_peak > 2 * budget + input_size + 64) {
    fail("a substituted value is longer than the budget allows", std::to_string(r.substitution_peak), input);
  }
  if (r.work > 4000ull * (input_size + 256)) {
    fail("the work is not linear in the text", std::to_string(r.work) + " for " + std::to_string(input_size) + " bytes", input);
  }
}

/// Compute the child (and its parent when there is a block for it).
GTEXT_CSS_Status compute(const GTEXT_CSS_Compute_Context * ctx, const GTEXT_CSS_Declaration_Block * pb, const GTEXT_CSS_Declaration_Block * cb,
    GTEXT_CSS_Computed_Style ** parent_out, GTEXT_CSS_Computed_Style ** out, GTEXT_CSS_Compute_Result * r) {
  GTEXT_CSS_Status st = GTEXT_CSS_OK;
  if (pb) {
    st = gtext_css_compute_style(ctx, pb, nullptr, nullptr, parent_out, r);
  }
  if (st == GTEXT_CSS_OK) {
    st = gtext_css_compute_style(ctx, cb, *parent_out, nullptr, out, r);
  }
  return st;
}

void check_compute(const std::vector<Decl> & parent_decls, const std::vector<Decl> & child_decls, bool has_parent, uint8_t o0, size_t fault, size_t size,
    const GTEXT_CSS_At_Property_Set * rules, const std::string & input) {
  GTEXT_CSS_Declaration_Block *pb = nullptr, *cb = nullptr;
  if (has_parent) {
    build(parent_decls, nullptr, &pb, input);
  }
  build(child_decls, nullptr, &cb, input);
  FaultAllocator fa;
  GTEXT_CSS_Compute_Context * ctx = nullptr;
  GTEXT_CSS_Compute_Result r{};
  if (fault || rules) {
    const GTEXT_CSS_Environment env = environment_of(o0);
    const GTEXT_CSS_Compute_Options o = options_of(o0, fault ? &fa.alloc : nullptr, rules);
    if (gtext_css_compute_context_new(registry(), &env, &o, &ctx, &r) != GTEXT_CSS_OK) {
      fail("a context could not be made", r.name, input);
    }
  }
  else {
    ctx = shared_context(o0);
  }
  GTEXT_CSS_Computed_Style *parent = nullptr, *child = nullptr;
  fa.arm(fault);
  const GTEXT_CSS_Status st = compute(ctx, pb, cb, &parent, &child, &r);
  fa.disarm();
  if (!allowed_status(st)) {
    fail("a status a computation must not return", gtext_css_status_string(st) + std::string(" ") + r.name, input);
  }
  if (fa.failed && st != GTEXT_CSS_E_OOM) {
    fail("an allocation failed and the computation did not say so", gtext_css_status_string(st), input);
  }
  if (st != GTEXT_CSS_OK && child) {
    fail("a computation that did not succeed returned a style", gtext_css_status_string(st), input);
  }
  std::string first, again, repeated;
  bool fixed_checked = false, repeat_checked = false;
  if (st == GTEXT_CSS_OK) {
    check_counters(r, o0, size, input);
    first = dump_of(child, input);
    uint64_t h1 = 0, h2 = 0;
    if (gtext_css_computed_hash(child, &h1) != GTEXT_CSS_OK || gtext_css_computed_hash(child, &h2) != GTEXT_CSS_OK || h1 != h2) {
      fail("the hash of a style is not stable", "", input);
    }
  }
  if (st == GTEXT_CSS_OK && !fa.failed) {
    bool splittable = true;
    const std::string decls = declarations_of(child, &splittable);
    GTEXT_CSS_Declaration_Block * rb = nullptr;
    if (splittable && build(declarations(decls), nullptr, &rb, input) == GTEXT_CSS_OK) {
      GTEXT_CSS_Computed_Style * second = nullptr;
      GTEXT_CSS_Compute_Result r2{};
      const GTEXT_CSS_Status s2 = gtext_css_compute_style(ctx, rb, parent, nullptr, &second, &r2);
      if (s2 == GTEXT_CSS_OK) {
        again = dump_of(second, input);
        fixed_checked = true;
      }
      else if (!allowed_status(s2)) {
        fail("a status a computation must not return", gtext_css_status_string(s2), input);
      }
      gtext_css_computed_free(second);
    }
    gtext_css_block_free(rb);
    GTEXT_CSS_Computed_Style *p2 = nullptr, *c2 = nullptr;
    GTEXT_CSS_Compute_Result r3{};
    if (compute(ctx, pb, cb, &p2, &c2, &r3) == GTEXT_CSS_OK) {
      repeated = dump_of(c2, input);
      repeat_checked = true;
    }
    gtext_css_computed_free(c2);
    gtext_css_computed_free(p2);
  }
  gtext_css_computed_free(child);
  gtext_css_computed_free(parent);
  if (fault || rules) {
    gtext_css_compute_context_free(ctx);
  }
  gtext_css_block_free(pb);
  gtext_css_block_free(cb);
  if (!fa.live.empty()) {
    fail("a computation leaked", std::to_string(fa.live.size()) + " blocks", input);
  }
  if (fa.bad_frees) {
    fail("a computation freed what it did not make", "", input);
  }
  if (repeat_checked && repeated != first) {
    fail("two computations of the same inputs differ", "", input);
  }
  if (fixed_checked && again != first) {
    size_t i = 0;
    while (i < first.size() && i < again.size() && first[i] == again[i]) {
      i++;
    }
    const size_t from = first.rfind('\n', i == 0 ? 0 : i - 1);
    const size_t a0 = from == std::string::npos ? 0 : from + 1;
    const size_t from2 = again.rfind('\n', i == 0 ? 0 : i - 1);
    const size_t a1 = from2 == std::string::npos ? 0 : from2 + 1;
    fail("a computed value is not a fixed point", "first:  " + first.substr(a0, 160) + "\nagain:  " + again.substr(a1, 160), input);
  }
}

/// The value of a few ordinary properties and every custom property a style has, as text.
std::string observe(const GTEXT_CSS_Computed_Style * s, const std::string & input) {
  std::string out;
  const GTEXT_CSS_Registry * r = registry();
  for (const char * name : {"width", "height", "color", "margin-top", "margin-left", "content", "font-size", "line-height", "opacity"}) {
    const GTEXT_CSS_Property_Id id = gtext_css_registry_find_property(r, name, std::strlen(name));
    const GTEXT_CSS_Value * root = id ? gtext_css_computed_root(s, id) : nullptr;
    out += name;
    out += "=";
    if (root) {
      char * buf = nullptr;
      size_t len = 0;
      if (gtext_css_serialize_value_buffer(root, nullptr, &buf, &len) == GTEXT_CSS_OK) {
        out.append(buf, len);
        gtext_css_serialize_buffer_free(nullptr, buf);
      }
    }
    out += "\n";
  }
  for (size_t i = 0; i < gtext_css_computed_custom_count(s); i++) {
    GTEXT_CSS_String name;
    const GTEXT_CSS_Custom_Value * v = nullptr;
    if (!gtext_css_computed_custom_at(s, i, &name, &v)) {
      fail("a custom property of a style is missing", "", input);
    }
    char * buf = nullptr;
    size_t len = 0;
    if (gtext_css_custom_value_text(v, nullptr, &buf, &len) == GTEXT_CSS_OK) {
      out.append(name.data, name.length);
      out += "=";
      out.append(buf, len);
      out += "\n";
      gtext_css_serialize_buffer_free(nullptr, buf);
    }
  }
  return out;
}

/// The same declarations through the cascade and through a block compute alike.
void check_agreement(const std::vector<Decl> & decls, uint8_t o0, const std::string & input) {
  // the rolling keywords are the cascade's alone, and `all` and the logical properties are decided by the flow in the cascade
  for (const Decl & d : decls) {
    if (d.value.find("revert") != std::string::npos || d.name == "all" || d.value.find("!") != std::string::npos) {
      return;
    }
  }
  GTEXT_CSS_Candidates * set = nullptr;
  if (gtext_css_candidates_new(registry(), nullptr, nullptr, &set) != GTEXT_CSS_OK) {
    return;
  }
  GTEXT_CSS_Declaration_Block * direct = nullptr;
  GTEXT_CSS_Declaration_Options bo = gtext_css_declaration_options_default();
  if (gtext_css_block_new(registry(), &bo, &direct) != GTEXT_CSS_OK) {
    gtext_css_candidates_free(set);
    return;
  }
  bool both = true;
  uint32_t order = 0;
  for (const Decl & d : decls) {
    GTEXT_CSS_Cascade_Entry k;
    std::memset(&k, 0, sizeof k);
    k.origin = GTEXT_CSS_ORIGIN_AUTHOR;
    k.rule = order++;
    GTEXT_CSS_Property_Outcome o1 = GTEXT_CSS_PROP_RESULT_UNKNOWN, o2 = GTEXT_CSS_PROP_RESULT_UNKNOWN;
    const GTEXT_CSS_Status s1 = gtext_css_candidates_add_text(set, &k, d.name.data(), d.name.size(), d.value.data(), d.value.size(), nullptr, &o1, nullptr);
    const GTEXT_CSS_Status s2 = gtext_css_block_add_text(direct, d.name.data(), d.name.size(), d.value.data(), d.value.size(), false, nullptr, &o2, nullptr);
    if (!allowed_status(s1) || !allowed_status(s2)) {
      fail("a status a declaration must not return", std::string(gtext_css_status_string(s1)) + " " + gtext_css_status_string(s2), input);
    }
    if (s1 != s2 && !(s1 == GTEXT_CSS_E_LIMIT_DECLARATIONS || s2 == GTEXT_CSS_E_LIMIT_DECLARATIONS)) {
      fail("the cascade and a block do not take a declaration alike", d.name + ": " + d.value, input);
    }
    if (s1 != GTEXT_CSS_OK || s2 != GTEXT_CSS_OK) {
      both = false;
    }
  }
  GTEXT_CSS_Cascaded * cas = nullptr;
  GTEXT_CSS_Declaration_Block * via = nullptr;
  if (both && gtext_css_cascade(set, nullptr, &cas, nullptr) == GTEXT_CSS_OK && gtext_css_cascaded_to_block(cas, nullptr, &via) == GTEXT_CSS_OK) {
    const GTEXT_CSS_Compute_Context * ctx = shared_context(o0);
    GTEXT_CSS_Computed_Style *a = nullptr, *b = nullptr;
    GTEXT_CSS_Compute_Result ra{}, rb{};
    const GTEXT_CSS_Status sa = gtext_css_compute_style(ctx, via, nullptr, nullptr, &a, &ra);
    const GTEXT_CSS_Status sb = gtext_css_compute_style(ctx, direct, nullptr, nullptr, &b, &rb);
    if (!allowed_status(sa) || !allowed_status(sb)) {
      fail("a status a computation must not return", std::string(gtext_css_status_string(sa)) + " " + gtext_css_status_string(sb), input);
    }
    if (sa == GTEXT_CSS_OK && sb == GTEXT_CSS_OK) {
      const std::string oa = observe(a, input), ob = observe(b, input);
      if (oa != ob) {
        fail("the cascade and a block compute differently", "cascade:\n" + oa + "block:\n" + ob, input);
      }
    }
    else if ((sa == GTEXT_CSS_OK) != (sb == GTEXT_CSS_OK) && !(sa == GTEXT_CSS_E_LIMIT_CUSTOM_PROPERTIES || sb == GTEXT_CSS_E_LIMIT_CUSTOM_PROPERTIES)) {
      fail("the cascade and a block do not agree that a style computes", std::string(gtext_css_status_string(sa)) + " " + gtext_css_status_string(sb), input);
    }
    gtext_css_computed_free(a);
    gtext_css_computed_free(b);
  }
  gtext_css_block_free(via);
  gtext_css_cascaded_free(cas);
  gtext_css_block_free(direct);
  gtext_css_candidates_free(set);
}

/// A style sheet whose `@property` rules are collected: deterministic, found by name, bounded, and leak free under a failed allocation.
void check_rules(const std::string & sheet, const std::string & rest, uint8_t o0, size_t fault, const std::string & input) {
  GTEXT_CSS_Tree * tree = nullptr;
  GTEXT_CSS_Status st = gtext_css_parse_stylesheet(sheet.data(), sheet.size(), nullptr, &tree, nullptr);
  if (st != GTEXT_CSS_OK) {
    if (!allowed_status(st)) {
      fail("a status parsing a style sheet must not return", gtext_css_status_string(st), input);
    }
    return;
  }
  size_t count_a = 0;
  GTEXT_CSS_At_Property_Set * kept = nullptr;
  for (int pass = 0; pass < 3; pass++) {
    FaultAllocator fa;
    GTEXT_CSS_At_Property_Options o = gtext_css_at_property_options_default();
    o.allocator = pass == 2 ? &fa.alloc : nullptr;
    o.active = [](void *, const GTEXT_CSS_Rule *) { return true; };
    GTEXT_CSS_At_Property_Set * set = nullptr;
    if (pass == 2) {
      if (fault == 0) {
        break;
      }
      fa.arm(fault);
    }
    st = gtext_css_at_property_set_new(registry(), &o, &set);
    GTEXT_CSS_At_Property_Result res;
    std::memset(&res, 0, sizeof res);
    if (st == GTEXT_CSS_OK) {
      st = gtext_css_at_property_collect(set, tree, &res);
    }
    fa.disarm();
    if (!allowed_status(st)) {
      fail("a status collecting @property rules must not return", gtext_css_status_string(st), input);
    }
    if (fa.failed && st != GTEXT_CSS_E_OOM) {
      fail("an allocation failed and the collection did not say so", gtext_css_status_string(st), input);
    }
    if (st == GTEXT_CSS_OK) {
      const size_t count = gtext_css_at_property_count(set);
      if (pass == 0) {
        count_a = count;
      }
      else if (pass == 1 && count != count_a) {
        fail("two collections of one sheet differ", std::to_string(count_a) + " " + std::to_string(count), input);
      }
      for (size_t i = 0; i < count; i++) {
        GTEXT_CSS_Custom_Info at, found;
        if (!gtext_css_at_property_at(set, i, &at)) {
          fail("a collected rule is missing", std::to_string(i), input);
        }
        if (!gtext_css_at_property_find(set, at.name, at.name_length, &found) || found.name_length != at.name_length || found.syntax_length != at.syntax_length ||
            found.inherits != at.inherits) {
          fail("a collected rule found by name is not the one at its index", std::string(at.name, at.name_length), input);
        }
      }
      const size_t total = gtext_css_at_property_diagnostic_total(set);
      size_t seen = 0;
      GTEXT_CSS_At_Property_Diagnostic d;
      while (gtext_css_at_property_diagnostic(set, seen, &d)) {
        seen++;
      }
      if (seen > total) {
        fail("more diagnostics than were counted", std::to_string(seen) + " " + std::to_string(total), input);
      }
    }
    if (pass == 1) {
      kept = set;
    }
    else {
      gtext_css_at_property_set_free(set);
    }
    if (pass == 2 && !fa.live.empty()) {
      fail("a collection leaked", std::to_string(fa.live.size()) + " blocks", input);
    }
    if (pass == 2 && fa.bad_frees) {
      fail("a collection freed what it did not make", "", input);
    }
  }
  if (kept) {
    // the rules, used: the declarations after the sheet compute with them
    const std::vector<Decl> decls = declarations(rest);
    if (!decls.empty()) {
      check_compute({}, decls, false, o0, 0, sheet.size() + rest.size(), kept, input);
    }
  }
  gtext_css_at_property_set_free(kept);
  gtext_css_tree_free(tree);
}

int entry(const uint8_t * data, size_t size) {
  if (size < 4 || size > 4096) {
    return 0;
  }
  const uint8_t o0 = data[0], o3 = data[3];
  const std::string input(reinterpret_cast<const char *>(data), size);
  const std::string text(reinterpret_cast<const char *>(data) + 4, size - 4);
  const unsigned mode = o0 & 3;
  const size_t sep = text.find("\n---\n");
  if (mode == 0) {
    check_compute({}, declarations(text), false, o0, o3, text.size(), nullptr, input);
  }
  else if (mode == 1) {
    const bool two = sep != std::string::npos;
    check_compute(two ? declarations(text.substr(0, sep)) : std::vector<Decl>(), declarations(two ? text.substr(sep + 5) : text), two, o0, o3, text.size(), nullptr,
        input);
  }
  else if (mode == 2) {
    check_rules(sep == std::string::npos ? text : text.substr(0, sep), sep == std::string::npos ? std::string() : text.substr(sep + 5), o0, o3, input);
  }
  else {
    check_agreement(declarations(text), o0, input);
  }
  return 0;
}

}  // namespace css_custfuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  return css_custfuzz::entry(data, size);
}
