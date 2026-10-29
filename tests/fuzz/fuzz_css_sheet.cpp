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
 * libFuzzer harness for style sheets and the rule index (src/css/css_sheet.c, css_rule_index.c): a small tree and one or two style sheets of arbitrary text, compiled, indexed and
 * collected for every element.
 *
 * The input is three option bytes, four bytes for each element of the tree, then the text of the sheets (a 0x01 byte ends the first sheet; the second is the shadow tree's):
 *
 *   o0  bit 0 the tree is read as an HTML document; bit 1 quirks; bits 2-3 the filter for conditional rules (1: nothing applies, 0 2 3: all do); bit 4 the second sheet is the
 *       sheet of a shadow tree the first element hosts (else it is a second sheet of the document); bit 5 the first sheet is an author sheet and the second a user sheet;
 *   o1  the number of elements, 1 to 16 (the byte modulo 16, plus one);
 *   o2  reserved (zero);
 *   per element: a parent (0 for a root of the tree, else an earlier element), a name (a, b, c, li, p, DIV), a set of classes (x, y, z), an id (none, i1, i2, i3).
 *
 * The properties:
 *
 *   a status is OK, UNSUPPORTED, a limit or OOM - never E_INTERNAL; compiling the same text twice gives the same dump;
 *   **the index is the brute force**: for each element and each rule of the document sheet that is flat (not nested, not in an `@scope`, with no shadow selector), for the element and for each of `::before`, `::after`, `::first-line` and `::marker`, the rule is among the collected entries exactly when its selector list matches the element by gtext_css_selector_match(), with the specificity that match reports;
 *   **the memo and the ancestor filter change nothing**: the same entries with each disabled;
 *   **a rule of the document does not reach into a shadow tree**: an element in the shadow tree has no entry of context 0;
 *   nothing leaks.
 *
 * Seeds: tests/fuzz/corpus/css-sheet, made by tools/css/make-fuzz-sheet-seeds.py.
 * Build with: make fuzz-css-sheet      Run: make fuzz-run-css-sheet
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <ghoti.io/text/css.h>

#include "../css_synth_tree.h"
#include "../markup_test_support.h"

namespace css_sheetfuzz {

[[noreturn]] void fail(const char * what, const std::string & detail, const std::string & input) {
  std::fprintf(stderr, "fuzz_css_sheet: %s\n", what);
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

const char * const kNames[] = {"a", "b", "c", "li", "p", "DIV"};
const char * const kIds[] = {"", "i1", "i2", "i3"};

bool all_true(void *, const GTEXT_CSS_Rule *) {
  return true;
}
bool all_false(void *, const GTEXT_CSS_Rule *) {
  return false;
}

bool tolerated(GTEXT_CSS_Status st) {
  switch (st) {
  case GTEXT_CSS_OK:
  case GTEXT_CSS_E_UNSUPPORTED:
  case GTEXT_CSS_E_OOM:
  case GTEXT_CSS_E_LIMIT_INPUT:
  case GTEXT_CSS_E_LIMIT_TOKENS:
  case GTEXT_CSS_E_LIMIT_DEPTH:
  case GTEXT_CSS_E_LIMIT_RULES:
  case GTEXT_CSS_E_LIMIT_DECLARATIONS:
  case GTEXT_CSS_E_LIMIT_ERRORS:
  case GTEXT_CSS_E_LIMIT_SELECTOR_COMPONENTS:
  case GTEXT_CSS_E_LIMIT_SELECTOR_LIST:
  case GTEXT_CSS_E_LIMIT_SELECTOR_DEPTH:
  case GTEXT_CSS_E_LIMIT_MATCH_MEMO:
  case GTEXT_CSS_E_LIMIT_MATCH_WORK:
  case GTEXT_CSS_E_LIMIT_RULE_NESTING:
  case GTEXT_CSS_E_LIMIT_SHEET_SIZE:
  case GTEXT_CSS_E_LIMIT_CANDIDATES:
  case GTEXT_CSS_E_LIMIT_LAYERS:
  case GTEXT_CSS_E_LIMIT_LAYER_DEPTH:
  case GTEXT_CSS_E_LIMIT_WORK:
  case GTEXT_CSS_E_LIMIT_LIST: return true;
  default: return false;
  }
}

std::string dump_of(const GTEXT_CSS_Sheet * s) {
  std::string out;
  GTEXT_Markup_Sink sink;
  sink.user = &out;
  sink.write = [](void * u, const char * d, size_t n) {
    static_cast<std::string *>(u)->append(d, n);
    return GTEXT_MARKUP_OK;
  };
  if (gtext_css_sheet_dump(s, &sink) != GTEXT_CSS_OK) {
    out += "<dump failed>";
  }
  return out;
}

GTEXT_CSS_Status compile(const std::string & text, GTEXT_CSS_Sheet_Options so, GTEXT_CSS_Sheet ** out) {
  GTEXT_CSS_Tree * tree = nullptr;
  GTEXT_CSS_Result r;
  std::memset(&r, 0, sizeof r);
  GTEXT_CSS_Status st = gtext_css_parse_stylesheet(text.data(), text.size(), nullptr, &tree, &r);
  gtext_css_result_free(&r);
  if (st != GTEXT_CSS_OK) {
    return st;
  }
  st = gtext_css_sheet_compile(tree, registry(), &so, out, nullptr);
  gtext_css_tree_free(tree);
  return st;
}

struct Rules {
  std::map<std::pair<uint32_t, uint32_t>, GTEXT_CSS_Specificity> seen; ///< (sheet, rule) -> the specificity
  std::set<uint32_t> contexts;
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
};

Rules collect_for(synth::Tree & t, GTEXT_CSS_Rule_Index * ix, GTEXT_CSS_Selector_Match_Options mo, int el, const GTEXT_Allocator * alloc,
    const GTEXT_CSS_Selector_List * pseudo = nullptr) {
  Rules out;
  mo.allocator = alloc;
  GTEXT_CSS_Selector_Match_Context * ctx = nullptr;
  if (gtext_css_selector_match_context_new(&t.table, &mo, &ctx) != GTEXT_CSS_OK) {
    out.status = GTEXT_CSS_E_OOM;
    return out;
  }
  GTEXT_CSS_Candidates * set = nullptr;
  if (gtext_css_candidates_new(registry(), nullptr, nullptr, &set) != GTEXT_CSS_OK) {
    gtext_css_selector_match_context_free(ctx);
    out.status = GTEXT_CSS_E_OOM;
    return out;
  }
  out.status = gtext_css_rules_collect(ix, ctx, synth::Tree::H(el), pseudo, set, nullptr);
  if (out.status == GTEXT_CSS_OK) {
    for (size_t i = 0; i < gtext_css_candidates_count(set); i++) {
      GTEXT_CSS_Cascade_Entry e;
      gtext_css_candidates_entry(set, i, &e);
      out.seen[{e.sheet, e.rule}] = e.specificity;
      out.contexts.insert(e.context);
    }
  }
  gtext_css_candidates_free(set);
  gtext_css_selector_match_context_free(ctx);
  return out;
}

int run(const uint8_t * data, size_t size) {
  if (size < 3) {
    return 0;
  }
  const std::string input(reinterpret_cast<const char *>(data), size);
  const uint8_t o0 = data[0];
  const int count = static_cast<int>(data[1] % 16) + 1;
  if (size < 3 + static_cast<size_t>(count) * 4) {
    return 0;
  }
  const std::string text(input, 3 + static_cast<size_t>(count) * 4);
  if (text.size() > 4096) {
    return 0;
  }
  const size_t cut = text.find('\x01');
  const std::string text_a = text.substr(0, cut);
  const std::string text_b = cut == std::string::npos ? std::string() : text.substr(cut + 1);
  synth::Tree t;
  for (int i = 0; i < count; i++) {
    const uint8_t * b = data + 3 + i * 4;
    const int parent = i == 0 ? 0 : (b[0] % (i + 1));
    std::vector<std::string> classes;
    for (int k = 0; k < 3; k++) {
      if (b[2] >> k & 1u) {
        classes.push_back(std::string(1, static_cast<char>('x' + k)));
      }
    }
    t.add(parent, kNames[b[1] % 6], kIds[b[3] % 4], classes);
  }
  const bool shadow = (o0 >> 4) & 1u;
  int sc = 0;
  std::vector<int> in_shadow;
  if (shadow) {
    sc = t.attach_shadow(1);
    const int top = t.add_in_shadow(sc, "p", "", {"x"});
    in_shadow.push_back(top);
    in_shadow.push_back(t.add(top, "a", "i1", {"y"}));
  }
  FaultAllocator fa;
  GTEXT_CSS_Sheet_Options so = gtext_css_sheet_options_default();
  so.allocator = &fa.alloc;
  so.active = (((o0 >> 2) & 3) == 1) ? all_false : all_true;
  so.max_compiled_bytes = 1u << 22;
  GTEXT_CSS_Sheet *a = nullptr, *b = nullptr, *again = nullptr;
  const GTEXT_CSS_Status sa = compile(text_a, so, &a);
  const GTEXT_CSS_Status sb = compile(text_b, so, &b);
  for (GTEXT_CSS_Status st : {sa, sb}) {
    if (!tolerated(st)) {
      fail("a status that is not tolerated while compiling", gtext_css_status_string(st), input);
    }
  }
  if (a) {
    const GTEXT_CSS_Status s2 = compile(text_a, so, &again);
    if (s2 != GTEXT_CSS_OK) {
      fail("the same text compiled once and not twice", gtext_css_status_string(s2), input);
    }
    if (dump_of(a) != dump_of(again)) {
      fail("two compiles of one text differ", dump_of(a) + "\n---\n" + dump_of(again), input);
    }
    gtext_css_sheet_free(again);
  }
  if (a || b) {
    GTEXT_CSS_Rule_Index_Options io = gtext_css_rule_index_options_default();
    io.allocator = &fa.alloc;
    io.html_document = o0 & 1u;
    io.quirks = (o0 >> 1) & 1u;
    GTEXT_CSS_Rule_Index * ix = nullptr;
    if (gtext_css_rule_index_new(registry(), &io, &ix) != GTEXT_CSS_OK) {
      fail("the index could not be made", "", input);
    }
    bool ok = true;
    if (a) {
      GTEXT_CSS_Sheet_Registration reg;
      std::memset(&reg, 0, sizeof reg);
      reg.origin = GTEXT_CSS_ORIGIN_AUTHOR;
      ok = gtext_css_rule_index_add_sheet(ix, a, &reg, nullptr) == GTEXT_CSS_OK;
    }
    if (ok && b) {
      GTEXT_CSS_Sheet_Registration reg;
      std::memset(&reg, 0, sizeof reg);
      reg.origin = (o0 >> 5) & 1u ? GTEXT_CSS_ORIGIN_USER : GTEXT_CSS_ORIGIN_AUTHOR;
      if (shadow) {
        reg.shadow = true;
        reg.tree_scope = synth::Tree::scope_handle(sc);
        reg.context = 1;
      }
      const GTEXT_CSS_Status st = gtext_css_rule_index_add_sheet(ix, b, &reg, nullptr);
      if (!tolerated(st) && st != GTEXT_CSS_E_INVALID) {
        fail("a status that is not tolerated while adding", gtext_css_status_string(st), input);
      }
      ok = st == GTEXT_CSS_OK;
    }
    if (ok && gtext_css_rule_index_freeze(ix, nullptr) == GTEXT_CSS_OK && a) {
      GTEXT_CSS_Selector_Match_Options mo = gtext_css_selector_match_options_default();
      mo.html_document = o0 & 1u;
      mo.quirks = (o0 >> 1) & 1u;
      GTEXT_CSS_Selector_Match_Context * ref = nullptr;
      GTEXT_CSS_Selector_Match_Options ro = mo;
      ro.allocator = &fa.alloc;
      if (gtext_css_selector_match_context_new(&t.table, &ro, &ref) != GTEXT_CSS_OK) {
        fail("a match context could not be made", "", input);
      }
      // the flat rules of the document sheet: not nested, not scoped, no pseudo-element, no shadow selector (every record of the rule)
      std::map<uint32_t, bool> flat;
      std::map<uint32_t, const GTEXT_CSS_Selector_List *> list_of;
      for (size_t i = 0; i < gtext_css_sheet_record_count(a); i++) {
        GTEXT_CSS_Sheet_Record r;
        gtext_css_sheet_record(a, i, &r);
        const bool f = !r.nested && r.scope_depth == 0 && !(r.dependencies & (GTEXT_CSS_DEP_HOST | GTEXT_CSS_DEP_NESTING | GTEXT_CSS_DEP_SCOPE));
        flat[r.rule] = flat.count(r.rule) ? (flat[r.rule] && f) : f;
        list_of[r.rule] = r.selector;
      }
      for (int el = 1; el < static_cast<int>(t.e.size()); el++) {
        const bool in_sh = std::find(in_shadow.begin(), in_shadow.end(), el) != in_shadow.end() || t.e[el].scope != 0;
        Rules got = collect_for(t, ix, mo, el, &fa.alloc);
        if (!tolerated(got.status)) {
          fail("a status that is not tolerated while collecting", gtext_css_status_string(got.status), input);
        }
        if (got.status != GTEXT_CSS_OK) {
          continue;
        }
        if (in_sh && got.contexts.count(0)) {
          fail("a rule of the document reached into a shadow tree", "element " + std::to_string(el), input);
        }
        GTEXT_CSS_Selector_Match_Options v = mo;
        v.disable_memo = true;
        v.disable_bloom = true;
        const Rules plain = collect_for(t, ix, v, el, &fa.alloc);
        if (plain.status == GTEXT_CSS_OK && plain.seen.size() != got.seen.size()) {
          fail("the memo or the filter changed the entries", "element " + std::to_string(el), input);
        }
        if (in_sh) {
          continue;
        }
        for (const char * pe : {static_cast<const char *>(nullptr), "::before", "::after", "::first-line", "::marker"}) {
          GTEXT_CSS_Selector_List * pl = nullptr;
          if (pe && (gtext_css_selector_parse(pe, std::strlen(pe), nullptr, &pl, nullptr) != GTEXT_CSS_OK || !pl)) {
            fail("a pseudo-element did not parse", pe, input);
          }
          Rules pgot;
          const Rules & use = pe ? (pgot = collect_for(t, ix, mo, el, &fa.alloc, pl)) : got;
          if (pe && !tolerated(use.status)) {
            fail("a status that is not tolerated while collecting a pseudo-element", gtext_css_status_string(use.status), input);
          }
          for (const auto & f : flat) {
            if (!f.second || use.status != GTEXT_CSS_OK) {
              continue;
            }
            GTEXT_CSS_Selector_Match_Result mr;
            const GTEXT_CSS_Status ms = gtext_css_selector_match(list_of[f.first], ref, synth::Tree::H(el), pl, &mr);
            if (ms != GTEXT_CSS_OK) {
              continue;
            }
            auto it = use.seen.find({0, f.first});
            if (mr.matched != (it != use.seen.end())) {
              fail(mr.matched ? "a rule that matches is not collected" : "a rule that does not match is collected",
                  "element " + std::to_string(el) + " pseudo " + (pe ? pe : "-") + " rule " + std::to_string(f.first) + "\n" + dump_of(a), input);
            }
            if (mr.matched && gtext_css_specificity_compare(it->second, mr.specificity) != 0) {
              fail("the specificity of a collected rule is not that of the match", "element " + std::to_string(el) + " rule " + std::to_string(f.first), input);
            }
          }
          gtext_css_selector_list_free(pl);
        }
      }
      gtext_css_selector_match_context_free(ref);
    }
    gtext_css_rule_index_free(ix);
  }
  gtext_css_sheet_free(a);
  gtext_css_sheet_free(b);
  if (!fa.live.empty()) {
    fail("an allocation leaked", std::to_string(fa.live.size()) + " blocks", input);
  }
  if (fa.bad_frees) {
    fail("a block the allocator did not make was freed", "", input);
  }
  return 0;
}

}  // namespace css_sheetfuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  return css_sheetfuzz::run(data, size);
}
