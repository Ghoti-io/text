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
 * libFuzzer harness for the CSS Value Definition Syntax engine (src/css/css_grammar_*.c,
 * css_value*.c): reading a grammar, linking it, matching component values against it, the typed value, and
 * writing the value.
 *
 * The input is three option bytes, then the text of a grammar up to the first NUL, then the CSS to match:
 *
 *   o0  bit 0 the unitless-length quirk, bit 1 a tiny definition limit chosen by o1, bit 2 the grammar hook
 *       instead of the direct call, bits 3-7 unused;
 *   o1  which definition limit is made tiny (none, text, depth, nodes) in bits 0-1 and its value in bits 2-7;
 *   o2  the allocation to fail (0 for none), counted over linking and matching.
 *
 * The properties:
 *
 *   reading a grammar never fails but with GTEXT_CSS_E_GRAMMAR or a definition limit, and is deterministic;
 *   linking, with a resolver for a few types (one recursive on the right, one a composite, one a function,
 *   one a property), is OK, E_REFERENCE, E_LEFT_RECURSION or a node limit, and the same each time;
 *   matching with a work bound of 2^22 is OK or E_LIMIT_WORK, never E_INTERNAL, and gives the same typed value
 *   each time; a value that matched is well formed (slots inside the grammar, text NUL-terminated and
 *   UTF-8, the count of the set the count of the tree, spans inside the input);
 *   **the bounds are exact**: the work the match reported, given as the bound, is accepted, and one less is
 *   E_LIMIT_WORK; the same for the memo entries it held;
 *   **the fixed point**: a value is written, the text parsed and matched again, and it matches and is written
 *   as the same text (not asked of a value that holds a unicode range, which only a declaration tokenizes, of one
 *   that is pending substitution, or of one with nothing in it);
 *   a value with var(), env() or attr() in it is pending substitution and matches;
 *   an allocation that fails is E_OOM with nothing returned, or the same answer, and never a leak or a
 *   free of a block the allocator did not make;
 *   the grammar hook gives the answer of the direct call.
 *
 * Seeds: tests/fuzz/corpus/css-grammar, made by tools/css/make-fuzz-grammar-seeds.py.
 * Build with: make fuzz-css-grammar      Run: make fuzz-run-css-grammar
 */

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

namespace css_gfuzz {

[[noreturn]] void fail(const char * what, const std::string & detail, const std::string & input) {
  std::fprintf(stderr, "fuzz_css_grammar: %s\n%s\ninput (%zu bytes): ", what, detail.c_str(), input.size());
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

struct Resolver {
  std::map<std::string, std::string> types{{"t", "a <t>?"}, {"lp", "<length> | <percentage>"}, {"f()", "f( <number>#{1,3} )"}};
  std::map<std::string, std::string> props{{"p", "<lp>{1,4}"}};
  std::vector<GTEXT_CSS_Definition *> made;
  const GTEXT_Allocator * alloc = nullptr;
  ~Resolver() {
    for (auto * d : made) {
      gtext_css_definition_free(d);
    }
  }
  static const GTEXT_CSS_Definition * resolve(void * user, GTEXT_CSS_Ref_Kind kind, const char * name, size_t length) {
    Resolver * self = static_cast<Resolver *>(user);
    const auto & table = kind == GTEXT_CSS_REF_PROPERTY ? self->props : self->types;
    const auto it = table.find(std::string(name, length));
    if (it == table.end()) {
      return nullptr;
    }
    GTEXT_CSS_Definition * d = nullptr;
    if (gtext_css_definition_parse(it->second.data(), it->second.size(), nullptr, &d, nullptr) != GTEXT_CSS_OK) {
      return nullptr;
    }
    self->made.push_back(d);
    return d;
  }
};

size_t check_value(const GTEXT_CSS_Value * v, size_t node_count, const std::string & input) {
  size_t n = 1;
  if (v->kind >= GTEXT_CSS_VAL_KIND_COUNT || v->slot >= node_count) {
    fail("a value has a kind or a slot that does not exist", std::to_string(v->slot), input);
  }
  if (v->text.data) {
    if (v->text.data[v->text.length] != '\0') {
      fail("a value's text is not terminated", "", input);
    }
    const unsigned char * p = (const unsigned char *)v->text.data;
    for (size_t i = 0; i < v->text.length;) {
      const unsigned char b = p[i];
      const size_t k = b < 0x80 ? 1 : (b >= 0xC2 && b <= 0xDF) ? 2 : (b >= 0xE0 && b <= 0xEF) ? 3 : (b >= 0xF0 && b <= 0xF4) ? 4 : 0;
      if (k == 0 || i + k > v->text.length) {
        fail("a value's text is not UTF-8", "", input);
      }
      i += k;
    }
  }
  if (v->span.offset > v->span.end) {
    fail("a value's span is out of order", "", input);
  }
  if ((v->child_count == 0) != (v->children == nullptr)) {
    fail("a value's children and their count disagree", "", input);
  }
  for (size_t i = 0; i < v->child_count; i++) {
    n += check_value(&v->children[i], node_count, input);
  }
  return n;
}

bool has_urange(const GTEXT_CSS_Value * v) {
  if (v->kind == GTEXT_CSS_VAL_UNICODE_RANGE) {
    return true;
  }
  for (size_t i = 0; i < v->child_count; i++) {
    if (has_urange(&v->children[i])) {
      return true;
    }
  }
  return false;
}

std::string written(const GTEXT_CSS_Value * v, const std::string & input) {
  if (!v) {
    return "<none>";
  }
  char * buf = nullptr;
  size_t len = 0;
  if (gtext_css_serialize_value_buffer(v, nullptr, &buf, &len) != GTEXT_CSS_OK) {
    fail("a value could not be written", "", input);
  }
  std::string s(buf, len);
  gtext_css_serialize_buffer_free(nullptr, buf);
  return s;
}

std::string text_of(const GTEXT_CSS_Value * root, const std::string & input) {
  return written(root, input) + (root && has_urange(root) ? "\n#urange" : "");
}

struct Outcome {
  GTEXT_CSS_Status status = GTEXT_CSS_E_INTERNAL;
  bool matched = false;
  std::string text;
  GTEXT_CSS_Match_Result result{};
  size_t count = 0;
};

Outcome match_once(const GTEXT_CSS_Compiled * c, const GTEXT_CSS_Component_Value * values, size_t n, const GTEXT_CSS_Match_Options & o,
    size_t node_count, const std::string & input) {
  Outcome out;
  GTEXT_CSS_Value_Set * set = nullptr;
  out.status = gtext_css_match(c, values, n, &o, &out.matched, &set, &out.result);
  if (out.status == GTEXT_CSS_OK && out.matched) {
    const GTEXT_CSS_Value * root = gtext_css_value_set_root(set);
    out.count = gtext_css_value_set_count(set);
    if (root) {
      if (check_value(root, node_count, input) != out.count) {
        fail("the count of a value set is not the number of its values", "", input);
      }
    }
    else if (out.count) {
      fail("a set with no root has values", "", input);
    }
    out.text = text_of(root, input);
  }
  else if (set) {
    fail("a set was returned for a match that did not happen", "", input);
  }
  gtext_css_value_set_free(set);
  return out;
}

int entry(const uint8_t * data, size_t size) {
  if (size < 3) {
    return 0;
  }
  const uint8_t o0 = data[0], o1 = data[1], o2 = data[2];
  const std::string input(reinterpret_cast<const char *>(data), size);
  const char * rest = reinterpret_cast<const char *>(data) + 3;
  const char * nul = static_cast<const char *>(std::memchr(rest, 0, size - 3));
  const std::string grammar(rest, nul ? (size_t)(nul - rest) : size - 3);
  const std::string css = nul ? std::string(nul + 1, size - 3 - grammar.size() - 1) : std::string();

  GTEXT_CSS_Grammar_Options go = gtext_css_grammar_options_default();
  if (o0 & 2) {
    const size_t v = (size_t)(o1 >> 2) + 1;
    switch (o1 & 3) {
    case 1: go.max_definition_bytes = v; break;
    case 2: go.max_definition_depth = v; break;
    case 3: go.max_definition_nodes = v; break;
    default: break;
    }
  }

  GTEXT_CSS_Definition * d = nullptr;
  GTEXT_CSS_Grammar_Result r{};
  const GTEXT_CSS_Status ds = gtext_css_definition_parse(grammar.data(), grammar.size(), &go, &d, &r);
  if (ds != GTEXT_CSS_OK) {
    if (d || (ds != GTEXT_CSS_E_GRAMMAR && ds != GTEXT_CSS_E_LIMIT_DEFINITION && ds != GTEXT_CSS_E_LIMIT_DEFINITION_DEPTH
              && ds != GTEXT_CSS_E_LIMIT_DEFINITION_NODES && ds != GTEXT_CSS_E_LIMIT_GROUP)) {
      fail("reading a grammar failed with a status it must not", gtext_css_status_string(ds), input);
    }
    return 0;
  }
  {
    GTEXT_CSS_Definition * d2 = nullptr;
    if (gtext_css_definition_parse(grammar.data(), grammar.size(), &go, &d2, nullptr) != GTEXT_CSS_OK
        || gtext_css_definition_node_count(d2) != gtext_css_definition_node_count(d)) {
      fail("reading a grammar twice gave two answers", "", input);
    }
    gtext_css_definition_free(d2);
  }

  Resolver res;
  GTEXT_CSS_Compiled * c = nullptr;
  const GTEXT_CSS_Status ls = gtext_css_definition_link(d, Resolver::resolve, &res, &go, &c, &r);
  if (ls != GTEXT_CSS_OK) {
    if (c || (ls != GTEXT_CSS_E_REFERENCE && ls != GTEXT_CSS_E_LEFT_RECURSION && ls != GTEXT_CSS_E_LIMIT_DEFINITION_NODES
              && ls != GTEXT_CSS_E_LIMIT_DEFINITION && ls != GTEXT_CSS_E_GRAMMAR)) {
      fail("linking failed with a status it must not", gtext_css_status_string(ls), input);
    }
    gtext_css_definition_free(d);
    return 0;
  }
  gtext_css_definition_free(d);
  const size_t node_count = gtext_css_compiled_node_count(c);

  GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
  po.max_depth = 4096;
  po.max_input_bytes = 1 << 20;
  GTEXT_CSS_Tree * tree = nullptr;
  GTEXT_CSS_Status ps = gtext_css_parse_component_values(css.data(), css.size(), &po, &tree, nullptr);
  if (ps != GTEXT_CSS_OK) {
    gtext_css_compiled_free(c);
    return 0;
  }

  GTEXT_CSS_Match_Options mo = gtext_css_match_options_default();
  mo.unitless_length = (o0 & 1) != 0;
  mo.max_match_work = (size_t)1 << 22;
  const Outcome first = match_once(c, tree->values, tree->value_count, mo, node_count, input);
  if (first.status != GTEXT_CSS_OK && first.status != GTEXT_CSS_E_LIMIT_WORK) {
    fail("a match failed with a status it must not", gtext_css_status_string(first.status), input);
  }
  if (first.status == GTEXT_CSS_OK) {
    const Outcome second = match_once(c, tree->values, tree->value_count, mo, node_count, input);
    if (second.status != first.status || second.matched != first.matched || second.text != first.text
        || second.result.work != first.result.work) {
      fail("matching twice gave two answers", first.text + "\n" + second.text, input);
    }
    // the bounds are exact
    if (first.result.work > 0) {
      GTEXT_CSS_Match_Options at = mo;
      at.max_match_work = first.result.work;
      const Outcome a = match_once(c, tree->values, tree->value_count, at, node_count, input);
      if (a.status != GTEXT_CSS_OK || a.text != first.text) {
        fail("the work the match did is not accepted as the bound", std::to_string(first.result.work), input);
      }
      if (first.result.work > 1) {
        at.max_match_work = first.result.work - 1;
        const Outcome b = match_once(c, tree->values, tree->value_count, at, node_count, input);
        if (b.status != GTEXT_CSS_E_LIMIT_WORK || b.matched) {
          fail("one less than the work the match did is not the limit", std::to_string(first.result.work), input);
        }
      }
    }
    if (first.result.memo_entries > 0) {
      GTEXT_CSS_Match_Options at = mo;
      at.max_memo_entries = first.result.memo_entries;
      const Outcome a = match_once(c, tree->values, tree->value_count, at, node_count, input);
      if (a.status != GTEXT_CSS_OK) {
        fail("the memo entries the match held are not accepted as the bound", std::to_string(first.result.memo_entries), input);
      }
      if (first.result.memo_entries > 1) {
        at.max_memo_entries = first.result.memo_entries - 1;
        const Outcome b = match_once(c, tree->values, tree->value_count, at, node_count, input);
        if (b.status != GTEXT_CSS_E_LIMIT_WORK) {
          fail("one less than the memo entries the match held is not the limit", std::to_string(first.result.memo_entries), input);
        }
      }
    }
    // the substitution
    bool found = false;
    if (gtext_css_has_arbitrary_substitution(tree->values, tree->value_count, nullptr, &found) != GTEXT_CSS_OK) {
      fail("the substitution scan failed", "", input);
    }
    if (found && !first.matched) {
      fail("a value with a substitution function did not match", "", input);
    }
    // the fixed point
    if (first.matched && !found && first.text.find("#urange") == std::string::npos && first.text != "<none>") {
      GTEXT_CSS_Tree * back = nullptr;
      if (gtext_css_parse_component_values(first.text.data(), first.text.size(), &po, &back, nullptr) == GTEXT_CSS_OK) {
        const Outcome again = match_once(c, back->values, back->value_count, mo, node_count, input);
        if (again.status == GTEXT_CSS_OK && !again.matched) {
          fail("a value that was written does not match again", first.text, input);
        }
        if (again.status == GTEXT_CSS_OK && again.text != first.text) {
          fail("a value written and matched again is written differently", first.text + "\n" + again.text, input);
        }
      }
      gtext_css_tree_free(back);
    }
    // the hook
    {
      GTEXT_CSS_Matcher mt;
      gtext_css_matcher_init(&mt, c, &mo);
      const GTEXT_CSS_Grammar hook = gtext_css_matcher_grammar(&mt);
      const bool hit = gtext_css_match_grammar(tree->values, tree->value_count, &hook);
      if (hit != first.matched
          || (hit && text_of(gtext_css_value_set_root(gtext_css_matcher_value_set(&mt, 0)), input) != first.text)) {
        fail("the grammar hook and the direct call disagree", "", input);
      }
      gtext_css_matcher_free(&mt);
    }
  }
  gtext_css_compiled_free(c);

  // An allocation that fails, over linking and matching.
  if (o2) {
    FaultAllocator fa;
    Resolver r2;
    GTEXT_CSS_Grammar_Options fo = go;
    fo.allocator = &fa.alloc;
    GTEXT_CSS_Definition * fd = nullptr;
    GTEXT_CSS_Status st = gtext_css_definition_parse(grammar.data(), grammar.size(), &fo, &fd, nullptr);
    GTEXT_CSS_Compiled * fc = nullptr;
    GTEXT_CSS_Value_Set * set = nullptr;
    bool matched = false;
    fa.arm(o2);
    if (st == GTEXT_CSS_OK) {
      st = gtext_css_definition_link(fd, Resolver::resolve, &r2, &fo, &fc, nullptr);
    }
    if (st == GTEXT_CSS_OK) {
      GTEXT_CSS_Match_Options fm = mo;
      fm.allocator = &fa.alloc;
      st = gtext_css_match(fc, tree->values, tree->value_count, &fm, &matched, &set, nullptr);
    }
    fa.disarm();
    if (st == GTEXT_CSS_E_OOM) {
      if (!fa.failed || set) {
        fail("E_OOM with no allocation failed, or with a set", "", input);
      }
    }
    else if (st == GTEXT_CSS_OK && matched != first.matched && first.status == GTEXT_CSS_OK) {
      fail("an allocation that failed changed the answer", "", input);
    }
    gtext_css_value_set_free(set);
    gtext_css_compiled_free(fc);
    gtext_css_definition_free(fd);
    if (!fa.live.empty() || fa.bad_frees) {
      fail("an allocation that failed leaked a block or freed one it did not make", "", input);
    }
  }
  gtext_css_tree_free(tree);
  return 0;
}

} // namespace css_gfuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  return css_gfuzz::entry(data, size);
}
