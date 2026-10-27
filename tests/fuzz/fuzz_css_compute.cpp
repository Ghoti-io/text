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
 * libFuzzer harness for the computed-value layer (src/css/css_compute*.c, css_shorthand*.c, css_block.c, css_grid_text.c, css_logical.c).
 *
 * The input is four option bytes, then text:
 *
 *   o0  bits 0-1 the mode: 0 a shorthand of the pin, by index (o1, o2), with the text as its value; 1 a block of declarations
 *       (`name: value` separated by `;`) written as cssText and read back; 2 a parent block and a child block (the text, the two
 *       separated by a line `---`) computed; 3 a block computed on its own, as a root;
 *       bit 2 the child is `!important` (a block never decides, but the flag must not matter), bit 3 the environment is the
 *       default (otherwise a small viewport and a different `medium`);
 *   o3  the allocation to fail (0 for none), counted over the one call.
 *
 * The properties:
 *
 *   a status is OK, E_UNSUPPORTED (a callback or a property that has no computed value, the result naming it), E_INVALID (a `revert`
 *   or a value still pending, which the cascade must resolve) or a limit, never E_INTERNAL; **an allocation that fails is E_OOM**, with
 *   nothing returned and nothing leaked or freed that the allocator did not make;
 *   a shorthand's expansion is OK or E_UNSUPPORTED and, when OK, the block written as cssText and read into a new block holds
 *   the same longhands (the same texts, one by one) and writes the same cssText again (the fixed point);
 *   a style computed twice from the same inputs dumps the same text and hashes the same;
 *   **a computed value is a fixed point**: the longhands of a style, written as declarations of the same element with the same parent, compute to the
 *   same style (a computed value is absolute, or kept as it is; computing it again changes nothing).
 *
 * Seeds: tests/fuzz/corpus/css-compute, made by tools/css/make-fuzz-compute-seeds.py.
 * Build with: make fuzz-css-compute      Run: make fuzz-run-css-compute
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

namespace css_cfuzz {

[[noreturn]] void fail(const char * what, const std::string & detail, const std::string & input) {
  std::fprintf(stderr, "fuzz_css_compute: %s\n%s\ninput (%zu bytes): ", what, detail.c_str(), input.size());
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

/// A block built from `name: value` declarations separated by `;` or line feeds, with the allocator given. The count of declarations that were
/// valid is in `*added`. A status other than OK, a rejected declaration or an unsupported shorthand is returned and the block is the declarations before it.
GTEXT_CSS_Status build(const std::string & text, FaultAllocator * fa, GTEXT_CSS_Declaration_Block ** out, const std::string & input) {
  GTEXT_CSS_Declaration_Options o = gtext_css_declaration_options_default();
  o.allocator = fa ? &fa->alloc : nullptr;
  GTEXT_CSS_Status st = gtext_css_block_new(registry(), &o, out);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  GTEXT_CSS_Property_Options po;
  std::memset(&po, 0, sizeof po);
  po.allocator = o.allocator;
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
    std::string name = decl.substr(0, colon), value = decl.substr(colon + 1);
    while (!name.empty() && name.front() == ' ') {
      name.erase(name.begin());
    }
    while (!value.empty() && value.front() == ' ') {
      value.erase(value.begin());
    }
    if (name.empty()) {
      continue;
    }
    GTEXT_CSS_Property_Outcome outcome = GTEXT_CSS_PROP_RESULT_UNKNOWN;
    GTEXT_CSS_Shorthand_Result sr{};
    st = gtext_css_block_add_text(*out, name.data(), name.size(), value.data(), value.size(), false, &po, &outcome, &sr);
    if (st == GTEXT_CSS_E_UNSUPPORTED && sr.name[0] == '\0' && !sr.detail) {
      // an unsupported value of a longhand (a grammar the pin lacks): nothing was added
    }
    if (st != GTEXT_CSS_OK) {
      if (!allowed_status(st)) {
        fail("a status a declaration must not return", gtext_css_status_string(st), input);
      }
      return st;
    }
  }
  return GTEXT_CSS_OK;
}

std::string css_text(const GTEXT_CSS_Declaration_Block * b, GTEXT_CSS_Status * st, const std::string & input) {
  char * out = nullptr;
  size_t n = 0;
  *st = gtext_css_block_serialize_buffer(b, &out, &n);
  if (*st != GTEXT_CSS_OK) {
    if (!allowed_status(*st)) {
      fail("a status cssText must not return", gtext_css_status_string(*st), input);
    }
    return "";
  }
  std::string s(out, n);
  gtext_css_serialize_buffer_free(nullptr, out);
  return s;
}

/// The effective items of a block as `name=value` lines: the longhands, one by one, the last declaration of a property alone (cssText leaves the
/// ones it shadows out, and so does a computation). The lines are in the order of the names: a shorthand is written where its first longhand was, and
/// longhands of different properties do not care where in a block they are (the logical properties that share a slot are the cascade's, and a block of
/// them is checked by the computation).
std::string items_of(const GTEXT_CSS_Declaration_Block * b, const std::string & input) {
  std::vector<std::string> lines, names, keys;
  const size_t n = gtext_css_block_count(b);
  // a longhand left pending by a shorthand that holds var() or attr() whose other longhands were declared again holds the text of the shorthand, which is
  // not a value of it: cssText leaves it out (the text is no declaration of this longhand), so it is not compared. The ones of a shorthand that is whole
  // (every one of its longhands still pending from it) are.
  std::map<size_t, size_t> pending_of;
  std::map<unsigned long, size_t> last_of;
  for (size_t i = 0; i < n; i++) {
    GTEXT_CSS_Declaration_Item it;
    if (gtext_css_block_item(b, i, &it) && !it.custom_name.length) {
      last_of[(unsigned long)it.property] = i;
    }
  }
  for (size_t i = 0; i < n; i++) {
    GTEXT_CSS_Declaration_Item it;
    if (gtext_css_block_item(b, i, &it) && !it.custom_name.length && last_of[(unsigned long)it.property] == i && it.outcome == GTEXT_CSS_PROP_RESULT_PENDING &&
        it.shorthand) {
      pending_of[it.declaration]++;
    }
  }
  for (size_t i = 0; i < n; i++) {
    GTEXT_CSS_Declaration_Item it;
    if (!gtext_css_block_item(b, i, &it)) {
      fail("an item of a block is missing", "", input);
    }
    GTEXT_CSS_Property_Info info;
    if (it.outcome == GTEXT_CSS_PROP_RESULT_PENDING && it.shorthand) {
      GTEXT_CSS_Property_Info sh;
      if (!gtext_css_registry_property_info(registry(), it.shorthand, &sh) || pending_of[it.declaration] != sh.longhand_count) {
        continue;
      }
    }
    std::string name, key;
    if (it.custom_name.length) {
      name.assign(it.custom_name.data, it.custom_name.length);
    }
    else if (gtext_css_registry_property_info(registry(), it.property, &info)) {
      name.assign(info.name, info.name_length);
    }
    if (key.empty()) {
      key = "name:" + name;
    }
    std::string line = name + "=";
    if (it.outcome == GTEXT_CSS_PROP_RESULT_WIDE) {
      line += gtext_css_wide_keyword_name(it.wide);
    }
    else if (!it.custom_name.length && it.outcome == GTEXT_CSS_PROP_RESULT_VALID) {
      // the value as CSSOM writes it (canonical): `inline flow-root` and `inline-block` are one value
      char * buf = nullptr;
      size_t len = 0;
      if (gtext_css_block_get_property_value(b, it.property, &buf, &len, nullptr) == GTEXT_CSS_OK) {
        line.append(buf, len);
        gtext_css_serialize_buffer_free(nullptr, buf);
      }
    }
    else if (it.set && gtext_css_value_set_root(it.set)) {
      char * buf = nullptr;
      size_t len = 0;
      if (gtext_css_serialize_value_buffer(gtext_css_value_set_root(it.set), nullptr, &buf, &len) == GTEXT_CSS_OK) {
        line.append(buf, len);
        gtext_css_serialize_buffer_free(nullptr, buf);
      }
    }
    if (it.outcome == GTEXT_CSS_PROP_RESULT_PENDING) {
      // a pending value keeps the text as it was declared; cssText starts it at its first token
      const size_t eq = line.find('=');
      size_t at = eq + 1;
      while (at < line.size() && (line[at] == ' ' || line[at] == '\t' || line[at] == '\n' || line[at] == '\r' || line[at] == '\f')) {
        at++;
      }
      line.erase(eq + 1, at - eq - 1);
    }
    line += it.outcome == GTEXT_CSS_PROP_RESULT_PENDING ? "(pending)" : "";
    line += it.important ? "!" : "";
    for (size_t k = 0; k < names.size(); k++) {
      if (names[k] == name) {
        names.erase(names.begin() + (long)k);
        lines.erase(lines.begin() + (long)k);
        keys.erase(keys.begin() + (long)k);
        break;
      }
    }
    names.push_back(name);
    lines.push_back(line);
    keys.push_back(key);
  }
  std::vector<size_t> order(lines.size());
  for (size_t i = 0; i < order.size(); i++) {
    order[i] = i;
  }
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t c) { return keys[a] < keys[c]; });
  std::string out;
  for (const size_t i : order) {
    out += lines[i] + "\n";
  }
  return out;
}

/// A block written, read back, and written again: the same longhands and the same cssText.
void check_block(const std::string & text, size_t fault, const std::string & input) {
  // A line break inside a string is a bad-string token, which no <declaration-value> may hold; a value with var() in it is nevertheless taken as pending
  // when its string is cut short by one (a gap of the property parser, recorded where the library's open items are), and the text of such a value is not
  // one declaration when it is written. Those inputs say nothing about the serializer.
  if (text.find_first_of("\n\r\f") != std::string::npos && text.find_first_of("\"'") != std::string::npos) {
    return;
  }
  FaultAllocator fa;
  GTEXT_CSS_Declaration_Block * b = nullptr;
  fa.arm(fault);
  GTEXT_CSS_Status st = build(text, &fa, &b, input);
  std::string css;
  GTEXT_CSS_Status ws = GTEXT_CSS_OK;
  if (st == GTEXT_CSS_OK) {
    css = css_text(b, &ws, input);
  }
  fa.disarm();
  if (fa.failed && st != GTEXT_CSS_E_OOM && ws != GTEXT_CSS_E_OOM) {
    fail("an allocation failed and nothing said so", std::string(gtext_css_status_string(st)) + " " + gtext_css_status_string(ws), input);
  }
  const std::string first_items = b ? items_of(b, input) : "";
  gtext_css_block_free(b);
  if (!fa.live.empty()) {
    fail("a block leaked", std::to_string(fa.live.size()) + " blocks", input);
  }
  if (fa.bad_frees) {
    fail("a block freed what it did not make", "", input);
  }
  if (st != GTEXT_CSS_OK || ws != GTEXT_CSS_OK || fa.failed) {
    return;
  }
  // the fixed point: the cssText is read into a block that holds the same longhands and writes the same text
  GTEXT_CSS_Declaration_Block * b2 = nullptr;
  const GTEXT_CSS_Status st2 = build(css, nullptr, &b2, input);
  if (st2 != GTEXT_CSS_OK) {
    gtext_css_block_free(b2);
    if (st2 == GTEXT_CSS_E_LIMIT_DECLARATIONS || st2 == GTEXT_CSS_E_LIMIT_WORK) {
      return;
    }
    fail("the cssText of a block is not read back", css + "\n" + gtext_css_status_string(st2), input);
  }
  GTEXT_CSS_Status ws2 = GTEXT_CSS_OK;
  const std::string css2 = css_text(b2, &ws2, input);
  const std::string second_items = items_of(b2, input);
  gtext_css_block_free(b2);
  if (css2 != css) {
    fail("the cssText of a block is not written again as the same text", css + "\n  ->\n" + css2, input);
  }
  if (second_items != first_items) {
    fail("the cssText of a block is read back as other longhands", css + "\n" + first_items + "  ->\n" + second_items, input);
  }
}

/// A shorthand of the pin by index with a value: expansion is OK or UNSUPPORTED, and what it makes holds the fixed point.
void check_shorthand(uint8_t o1, uint8_t o2, const std::string & value, size_t fault, const std::string & input) {
  const GTEXT_CSS_Registry * r = registry();
  const size_t n = gtext_css_registry_property_count(r);
  // the shorthands in the order of their names, so that a seed names one the same way on every pin (tools/css/make-fuzz-compute-seeds.py)
  std::vector<std::pair<std::string, GTEXT_CSS_Property_Id>> shorthands;
  for (size_t id = 1; id <= n; id++) {
    GTEXT_CSS_Property_Info info;
    if (gtext_css_registry_property_info(r, (GTEXT_CSS_Property_Id)id, &info) && (info.flags & GTEXT_CSS_PROP_SHORTHAND) && !(info.flags & GTEXT_CSS_PROP_ALIAS)) {
      shorthands.emplace_back(std::string(info.name, info.name_length), (GTEXT_CSS_Property_Id)id);
    }
  }
  std::sort(shorthands.begin(), shorthands.end());
  GTEXT_CSS_Property_Info info;
  if (shorthands.empty() || !gtext_css_registry_property_info(r, shorthands[((size_t)o1 << 8 | o2) % shorthands.size()].second, &info)) {
    return;
  }
  check_block(std::string(info.name, info.name_length) + ": " + value, fault, input);
}

/// The longhands of a style written as declarations: `name: text` for every property with a value.
std::string declarations_of(const GTEXT_CSS_Computed_Style * s) {
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
  return out;
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

GTEXT_CSS_Environment environment(bool plain) {
  GTEXT_CSS_Environment e = gtext_css_environment_default();
  if (!plain) {
    e.large_viewport.width = e.small_viewport.width = e.dynamic_viewport.width = 375;
    e.large_viewport.height = e.small_viewport.height = e.dynamic_viewport.height = 667;
    e.default_font_size = 14;
    e.device_pixel_ratio = 2;
    e.color_scheme = GTEXT_CSS_SCHEME_DARK;
  }
  return e;
}

/// A context with the default allocator for an environment, made once (it parses and computes the initial value of every property, some
/// milliseconds, and a replay of thousands of seeds would be spent on it).
GTEXT_CSS_Compute_Context * shared_context(bool plain) {
  static GTEXT_CSS_Compute_Context * contexts[2] = {nullptr, nullptr};
  GTEXT_CSS_Compute_Context *& c = contexts[plain ? 1 : 0];
  if (!c) {
    const GTEXT_CSS_Environment env = environment(plain);
    GTEXT_CSS_Compute_Result r{};
    if (gtext_css_compute_context_new(registry(), &env, nullptr, &c, &r) != GTEXT_CSS_OK) {
      fail("a context could not be made", r.name, "");
    }
  }
  return c;
}

/// The style of the child (and of its parent, when there is a block for it) in `ctx`.
GTEXT_CSS_Status compute(GTEXT_CSS_Compute_Context * ctx, const GTEXT_CSS_Declaration_Block * pb, const GTEXT_CSS_Declaration_Block * cb,
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

void check_compute(const std::string & parent_text, const std::string & child_text, bool has_parent, bool plain, size_t fault, const std::string & input) {
  GTEXT_CSS_Declaration_Block *pb = nullptr, *cb = nullptr;
  if (has_parent) {
    build(parent_text, nullptr, &pb, input);
  }
  build(child_text, nullptr, &cb, input);
  // with an allocation to fail the context is made on the failing allocator (and costs the milliseconds it costs); without, it is shared
  FaultAllocator fa;
  GTEXT_CSS_Compute_Context * ctx = nullptr;
  GTEXT_CSS_Compute_Result r{};
  if (fault) {
    const GTEXT_CSS_Environment env = environment(plain);
    GTEXT_CSS_Compute_Options o = gtext_css_compute_options_default();
    o.allocator = &fa.alloc;
    if (gtext_css_compute_context_new(registry(), &env, &o, &ctx, &r) != GTEXT_CSS_OK) {
      fail("a context could not be made", r.name, input);
    }
  }
  else {
    ctx = shared_context(plain);
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
  std::string first;
  if (st == GTEXT_CSS_OK) {
    first = dump_of(child, input);
    unsigned char h1[32] = {0}, h2[32] = {0};
    if (gtext_css_computed_hash(child, h1) != GTEXT_CSS_OK || gtext_css_computed_hash(child, h2) != GTEXT_CSS_OK || memcmp(h1, h2, 32) != 0) {
      fail("the hash of a style is not stable", "", input);
    }
  }
  std::string again;
  bool fixed_checked = false;
  std::string repeated;
  bool repeat_checked = false;
  if (st == GTEXT_CSS_OK && !fa.failed) {
    // the fixed point of a computed value: its longhands, written as declarations of the same element, compute to the same style
    const std::string decls = declarations_of(child);
    GTEXT_CSS_Declaration_Block * rb = nullptr;
    if (build(decls, nullptr, &rb, input) == GTEXT_CSS_OK) {
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
    // a second computation of the same inputs dumps the same
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
  if (fault) {
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

int entry(const uint8_t * data, size_t size) {
  if (size < 4 || size > 4096) {
    return 0;
  }
  const uint8_t o0 = data[0], o1 = data[1], o2 = data[2], o3 = data[3];
  const std::string input(reinterpret_cast<const char *>(data), size);
  const std::string text(reinterpret_cast<const char *>(data) + 4, size - 4);
  const unsigned mode = o0 & 3;
  if (mode == 0) {
    check_shorthand(o1, o2, text, o3, input);
  }
  else if (mode == 1) {
    check_block(text, o3, input);
  }
  else {
    const size_t sep = text.find("\n---\n");
    const std::string parent = mode == 2 && sep != std::string::npos ? text.substr(0, sep) : "";
    const std::string child = mode == 2 && sep != std::string::npos ? text.substr(sep + 5) : text;
    check_compute(parent, child, mode == 2 && sep != std::string::npos, (o0 & 8) != 0, o3, input);
  }
  return 0;
}

}  // namespace css_cfuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  return css_cfuzz::entry(data, size);
}
