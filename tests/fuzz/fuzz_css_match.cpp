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
 * libFuzzer harness for the matcher (src/css/css_match*.c): a small tree and a selector, matched by the engine with and without its memo and its ancestor filter, by a walk
 * and by single calls, and by a **naive reference matcher** written here from the grammar's meaning (left to right: find every sequence of elements the combinators allow, with no
 * memo and no filter).
 *
 * The input is three option bytes, six bytes for each element of the tree, then the selector text:
 *
 *   o0  bits 0-1 the limits (0 the defaults, 1 a small work limit, 2 a small memo, 3 a small `:has()` budget); bit 2 the tree is read as an HTML document (names compare
 *       ignoring case); bits 3-4 the `:scope` element (0 the root, else the n-th element of the tree); bit 5 quirks;
 *   o1  the number of elements, 1 to 24 (the byte modulo 24, plus one);
 *   o2  reserved (zero);
 *   per element: a parent (0 for a root of the tree, else an earlier element), a name (a, b, c, li, p, DIV), a set of classes (x, y, z), an id (none, i1, i2, i3), an attribute
 *   `t` (none, x, y, "x y", "x-y"), a state word (a few of the bits the pseudo-classes test).
 *
 * The properties:
 *
 *   a status is OK, a limit, or GTEXT_CSS_E_UNSUPPORTED - never E_INTERNAL;
 *   **the memo and the filter change nothing**: the set of elements matched is the same with and without each, when the calls succeed; a walk and single calls agree;
 *   **the engine is the reference**: for a selector made of the forms the reference knows (type, universal, id, class, attribute, the combinators, `:is() :where() :not() :has()`
 *   in all four relations, the structural pseudo-classes with `of S`, `:empty :root :scope`, the state pseudo-classes) the set of elements it matches is the reference's;
 *   the specificity the match reports is that of a selector that matches, and no matching selector has a higher one;
 *   nothing leaks.
 *
 * Seeds: tests/fuzz/corpus/css-match, made by tools/css/make-fuzz-match-seeds.py.
 * Build with: make fuzz-css-match      Run: make fuzz-run-css-match
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ghoti.io/text/css.h>

#include "../css_synth_tree.h"
#include "../markup_test_support.h"

namespace css_matchfuzz {

[[noreturn]] void fail(const char * what, const std::string & detail, const std::string & input) {
  std::fprintf(stderr, "%s: %s\n", "fuzz_css_match", what);
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

int row(const char * name) {
  return gtext_css_selector_find_row(name);
}

/* ---- the reference --------------------------------------------------------------------------------------------- */

struct Ref {
  synth::Tree & t;
  int scope;
  bool html;
  bool unsupported = false;
  std::vector<int> anchors; ///< the anchors of the `:has()` being evaluated: inside it `:scope` and `&` are the anchor (Selectors 4, relative selectors)

  Ref(synth::Tree & tree, int sc, bool h) : t(tree), scope(sc), html(h) {}

  static unsigned char low(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + 32) : c;
  }

  static bool same(const std::string & a, const std::string & b, bool ci) {
    if (a.size() != b.size()) {
      return false;
    }
    for (size_t i = 0; i < a.size(); i++) {
      if (ci ? low(a[i]) != low(b[i]) : a[i] != b[i]) {
        return false;
      }
    }
    return true;
  }

  std::vector<int> children(int e) const {
    std::vector<int> out;
    for (int c = t.e[e].first; c; c = t.e[c].next) {
      out.push_back(c);
    }
    return out;
  }

  void descendants(int e, std::vector<int> & out) const {
    for (int c = t.e[e].first; c; c = t.e[c].next) {
      out.push_back(c);
      descendants(c, out);
    }
  }

  bool attr(const GTEXT_CSS_Simple & s, int e) const {
    std::string an(s.name.data, s.name.length);
    if (html) {
      for (char & c : an) {
        c = static_cast<char>(low(static_cast<unsigned char>(c)));
      }
    }
    auto it = t.e[e].attrs.find(an);
    if (it == t.e[e].attrs.end()) {
      return false;
    }
    if (s.op == GTEXT_CSS_ATTR_EXISTS) {
      return true;
    }
    const std::string & v = it->second;
    const bool ci = s.attr_case == GTEXT_CSS_ATTR_CASE_INSENSITIVE;
    const std::string w(s.value.data, s.value.length);
    switch (s.op) {
    case GTEXT_CSS_ATTR_EQUALS: return same(v, w, ci);
    case GTEXT_CSS_ATTR_INCLUDES: {
      if (w.empty() || w.find_first_of(" \t\n\f\r") != std::string::npos) {
        return false;
      }
      size_t i = 0;
      while (i < v.size()) {
        while (i < v.size() && std::strchr(" \t\n\f\r", v[i])) {
          i++;
        }
        size_t j = i;
        while (j < v.size() && !std::strchr(" \t\n\f\r", v[j])) {
          j++;
        }
        if (j > i && same(v.substr(i, j - i), w, ci)) {
          return true;
        }
        i = j;
      }
      return false;
    }
    case GTEXT_CSS_ATTR_DASH: return same(v, w, ci) || (v.size() > w.size() && v[w.size()] == '-' && same(v.substr(0, w.size()), w, ci));
    case GTEXT_CSS_ATTR_PREFIX: return !w.empty() && v.size() >= w.size() && same(v.substr(0, w.size()), w, ci);
    case GTEXT_CSS_ATTR_SUFFIX: return !w.empty() && v.size() >= w.size() && same(v.substr(v.size() - w.size()), w, ci);
    case GTEXT_CSS_ATTR_SUBSTRING:
      if (w.empty()) {
        return false;
      }
      for (size_t i = 0; i + w.size() <= v.size(); i++) {
        if (same(v.substr(i, w.size()), w, ci)) {
          return true;
        }
      }
      return false;
    default: return false;
    }
  }

  static bool anb(int64_t a, int64_t b, int64_t pos) {
    for (int64_t n = 0; n <= 64; n++) {
      int64_t m = 0, v = 0; // the argument's numbers saturate at the 64-bit limits: a sum past them is no position
      if (__builtin_mul_overflow(a, n, &m) || __builtin_add_overflow(m, b, &v)) {
        continue;
      }
      if (v == pos) {
        return true;
      }
    }
    return false;
  }

  bool any(const GTEXT_CSS_Selector_List * l, int e) {
    for (size_t i = 0; l && i < l->count; i++) {
      if (!l->complex[i].never && !l->complex[i].raw.data && ends_at(l->complex[i], e)) {
        return true;
      }
    }
    return false;
  }

  /// Position of e among its siblings that are members (all, those of its type, or those that match `s`), from the start or the end; false if e is not a member.
  bool position(int e, const GTEXT_CSS_Selector_List * s, bool of_type, bool from_end, int64_t * pos) {
    std::vector<int> sib;
    if (t.e[e].parent) {
      sib = children(t.e[e].parent);
    }
    else {
      sib.push_back(e);
    }
    int64_t k = 0, at = 0, total = 0;
    for (int x : sib) {
      bool member = true;
      if (of_type) {
        member = t.e[x].name == t.e[e].name && t.e[x].ns == t.e[e].ns;
      }
      else if (s) {
        member = any(s, x);
      }
      if (member) {
        total++;
        k++;
        if (x == e) {
          at = k;
        }
      }
    }
    if (!at) {
      return false;
    }
    *pos = from_end ? total - at + 1 : at;
    return true;
  }

  bool pseudo_class(const GTEXT_CSS_Simple & s, int e) {
    static const int IS = row(":is()"), WHERE = row(":where()"), NOT = row(":not()"), HAS = row(":has()"), ROOT = row(":root"), EMPTY = row(":empty"),
        FIRST = row(":first-child"), LAST = row(":last-child"), ONLY = row(":only-child"), NTHC = row(":nth-child()"), NTHLC = row(":nth-last-child()"),
        NTHT = row(":nth-of-type()"), NTHLT = row(":nth-last-of-type()"), FOT = row(":first-of-type"), LOT = row(":last-of-type"), OOT = row(":only-of-type"),
        SCOPE = row(":scope");
    const int p = s.pseudo;
    int64_t pos = 0;
    if (p == IS || p == WHERE) {
      return any(s.list, e);
    }
    if (p == NOT) {
      return !any(s.list, e);
    }
    if (p == HAS) {
      return has(s.list, e);
    }
    if (p == ROOT) {
      return t.e[e].parent == 0 && t.e[e].host == 0;
    }
    if (p == SCOPE) {
      if (!anchors.empty()) {
        return e == anchors.back();
      }
      return scope ? e == scope : (t.e[e].parent == 0 && t.e[e].host == 0);
    }
    if (p == EMPTY) {
      return t.e[e].first == 0 && t.e[e].content == 0;
    }
    if (p == FIRST) {
      return t.e[e].prev == 0;
    }
    if (p == LAST) {
      return t.e[e].next == 0;
    }
    if (p == ONLY) {
      return t.e[e].prev == 0 && t.e[e].next == 0;
    }
    if (p == NTHC || p == NTHLC) {
      return position(e, s.has_of ? s.list : nullptr, false, p == NTHLC, &pos) && anb(s.anb_a, s.anb_b, pos);
    }
    if (p == NTHT || p == NTHLT) {
      return position(e, nullptr, true, p == NTHLT, &pos) && anb(s.anb_a, s.anb_b, pos);
    }
    if (p == FOT) {
      return position(e, nullptr, true, false, &pos) && pos == 1;
    }
    if (p == LOT) {
      return position(e, nullptr, true, true, &pos) && pos == 1;
    }
    if (p == OOT) {
      int64_t a = 0, b = 0;
      return position(e, nullptr, true, false, &a) && position(e, nullptr, true, true, &b) && a == 1 && b == 1;
    }
    GTEXT_CSS_Selector_Row r;
    if (gtext_css_selector_row(p, &r) && r.state && r.callbacks == (1u << GTEXT_CSS_ELEMENT_CB_STATE) && r.implemented) {
      return (t.e[e].state & r.state) != 0;
    }
    unsupported = true; // lang, dir, heading, host and the shadow selectors, :state(), ...
    return false;
  }

  bool simple(const GTEXT_CSS_Simple & s, int e) {
    if (s.never) {
      return false;
    }
    const synth::El & el = t.e[e];
    switch (s.kind) {
    case GTEXT_CSS_SIMPLE_UNIVERSAL:
    case GTEXT_CSS_SIMPLE_TYPE: {
      if (s.ns_kind == GTEXT_CSS_NS_NAMED || s.ns_from_default) {
        unsupported = true;
        return false;
      }
      if (s.ns_kind == GTEXT_CSS_NS_NONE && !el.ns.empty()) {
        return false;
      }
      return s.kind == GTEXT_CSS_SIMPLE_UNIVERSAL || same(el.name, std::string(s.name.data, s.name.length), html); // an element with no namespace is HTML in an HTML document
    }
    case GTEXT_CSS_SIMPLE_ID: return !el.id.empty() && el.id == std::string(s.name.data, s.name.length);
    case GTEXT_CSS_SIMPLE_CLASS: return std::find(el.classes.begin(), el.classes.end(), std::string(s.name.data, s.name.length)) != el.classes.end();
    case GTEXT_CSS_SIMPLE_ATTRIBUTE:
      if (s.ns_kind != GTEXT_CSS_NS_UNSPECIFIED) {
        unsupported = true;
        return false;
      }
      return attr(s, e);
    case GTEXT_CSS_SIMPLE_NESTING:
      if (!anchors.empty()) {
        return e == anchors.back();
      }
      return scope ? e == scope : (t.e[e].parent == 0 && t.e[e].host == 0);
    case GTEXT_CSS_SIMPLE_PSEUDO_CLASS: return pseudo_class(s, e);
    case GTEXT_CSS_SIMPLE_PSEUDO_ELEMENT: unsupported = true; return false;
    }
    return false;
  }

  bool compound(const GTEXT_CSS_Compound & c, int e) {
    for (size_t i = 0; i < c.simple_count; i++) {
      if (!simple(c.simples[i], e)) {
        return false;
      }
    }
    return true;
  }

  /// The compound i matches e, and the compounds after it match elements related as the combinators say, the last being `subject` (or anywhere, with subject 0).
  bool chain(const GTEXT_CSS_Complex & cx, size_t i, int e, int subject) {
    if (!compound(cx.compounds[i], e)) {
      return false;
    }
    if (i + 1 == cx.compound_count) {
      return subject == 0 || e == subject;
    }
    std::vector<int> rel;
    switch (cx.compounds[i + 1].combinator) {
    case GTEXT_CSS_COMBINATOR_CHILD: rel = children(e); break;
    case GTEXT_CSS_COMBINATOR_DESCENDANT: descendants(e, rel); break;
    case GTEXT_CSS_COMBINATOR_NEXT_SIBLING:
      if (t.e[e].next) {
        rel.push_back(t.e[e].next);
      }
      break;
    case GTEXT_CSS_COMBINATOR_SUBSEQUENT_SIBLING:
      for (int x = t.e[e].next; x; x = t.e[x].next) {
        rel.push_back(x);
      }
      break;
    default: return false;
    }
    for (int r : rel) {
      if (chain(cx, i + 1, r, subject)) {
        return true;
      }
    }
    return false;
  }

  bool ends_at(const GTEXT_CSS_Complex & cx, int subject) {
    if (cx.has_pseudo_element) {
      unsupported = true;
      return false;
    }
    for (int e = 1; e < static_cast<int>(t.e.size()); e++) {
      if (chain(cx, 0, e, subject)) {
        return true;
      }
    }
    return false;
  }

  bool has(const GTEXT_CSS_Selector_List * l, int anchor) {
    anchors.push_back(anchor);
    const bool r = has_inner(l, anchor);
    anchors.pop_back();
    return r;
  }

  bool has_inner(const GTEXT_CSS_Selector_List * l, int anchor) {
    for (size_t i = 0; l && i < l->count; i++) {
      const GTEXT_CSS_Complex & rc = l->complex[i];
      if (rc.never || rc.raw.data) {
        continue;
      }
      std::vector<int> start;
      switch (rc.compounds[0].combinator) {
      case GTEXT_CSS_COMBINATOR_CHILD: start = children(anchor); break;
      case GTEXT_CSS_COMBINATOR_DESCENDANT: descendants(anchor, start); break;
      case GTEXT_CSS_COMBINATOR_NEXT_SIBLING:
        if (t.e[anchor].next) {
          start.push_back(t.e[anchor].next);
        }
        break;
      case GTEXT_CSS_COMBINATOR_SUBSEQUENT_SIBLING:
        for (int x = t.e[anchor].next; x; x = t.e[x].next) {
          start.push_back(x);
        }
        break;
      default: break;
      }
      for (int y : start) {
        if (chain(rc, 0, y, 0)) {
          return true;
        }
      }
    }
    return false;
  }
};

/* ---- the engine ------------------------------------------------------------------------------------------------ */

const char * const kNames[] = {"a", "b", "c", "li", "p", "DIV"};
const char * const kIds[] = {"", "i1", "i2", "i3"};
const char * const kAttrs[] = {"", "x", "y", "x y", "x-y"};
const uint64_t kStates[] = {0, GTEXT_CSS_STATE_HOVER, GTEXT_CSS_STATE_CHECKED, GTEXT_CSS_STATE_DISABLED | GTEXT_CSS_STATE_ENABLED, GTEXT_CSS_STATE_LINK,
    GTEXT_CSS_STATE_FOCUS | GTEXT_CSS_STATE_TARGET, GTEXT_CSS_STATE_VISITED, GTEXT_CSS_STATE_OPEN};

struct Outcome {
  std::vector<int> matched;
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
};

struct Probe {
  const GTEXT_CSS_Selector_List * list;
  GTEXT_CSS_Selector_Match_Context * ctx;
  Outcome * out;
  std::vector<GTEXT_CSS_Specificity> best;
};

Outcome engine(synth::Tree & t, const GTEXT_CSS_Selector_List * list, GTEXT_CSS_Selector_Match_Options opt, bool walk, const GTEXT_Allocator * alloc) {
  Outcome out;
  opt.allocator = alloc;
  GTEXT_CSS_Selector_Match_Context * ctx = nullptr;
  if (gtext_css_selector_match_context_new(&t.table, &opt, &ctx) != GTEXT_CSS_OK) {
    out.status = GTEXT_CSS_E_OOM;
    return out;
  }
  Probe p{list, ctx, &out, {}};
  auto visit = [](void * u, GTEXT_CSS_Element_Handle h) {
    Probe * p = static_cast<Probe *>(u);
    GTEXT_CSS_Selector_Match_Result r;
    const GTEXT_CSS_Status st = gtext_css_selector_match(p->list, p->ctx, h, nullptr, &r);
    if (st != GTEXT_CSS_OK) {
      p->out->status = st;
      return false;
    }
    if (r.matched) {
      p->out->matched.push_back(synth::Tree::idx(h));
    }
    return true;
  };
  if (walk) {
    // a walk visits the elements below one root at a time, in document order; the tree's roots are walked one after the other
    for (size_t i = 1; i < t.e.size() && out.status == GTEXT_CSS_OK; i++) {
      if (t.e[i].parent == 0) {
        GTEXT_CSS_Selector_Match_Result r;
        const GTEXT_CSS_Status st = gtext_css_selector_match_walk(ctx, synth::Tree::H(static_cast<int>(i)), visit, &p, &r);
        if (out.status == GTEXT_CSS_OK && st != GTEXT_CSS_OK) {
          out.status = st;
        }
      }
    }
    std::sort(out.matched.begin(), out.matched.end());
  }
  else {
    for (int i = 1; i < static_cast<int>(t.e.size()) && out.status == GTEXT_CSS_OK; i++) {
      visit(&p, synth::Tree::H(i));
    }
  }
  gtext_css_selector_match_context_free(ctx);
  return out;
}

std::string list_of(const std::vector<int> & v) {
  std::string s;
  for (int e : v) {
    s += std::to_string(e) + " ";
  }
  return s;
}

bool limit_status(GTEXT_CSS_Status st) {
  return st == GTEXT_CSS_E_LIMIT_MATCH_MEMO || st == GTEXT_CSS_E_LIMIT_MATCH_WORK || st == GTEXT_CSS_E_UNSUPPORTED;
}

int run(const uint8_t * data, size_t size) {
  if (size < 3) {
    return 0;
  }
  const std::string input(reinterpret_cast<const char *>(data), size);
  const uint8_t o0 = data[0];
  const int count = static_cast<int>(data[1] % 24) + 1;
  if (size < 3 + static_cast<size_t>(count) * 6) {
    return 0;
  }
  const std::string text(input, 3 + static_cast<size_t>(count) * 6);
  if (text.size() > 2048) {
    return 0;
  }
  synth::Tree t;
  for (int i = 0; i < count; i++) {
    const uint8_t * b = data + 3 + i * 6;
    const int parent = i == 0 ? 0 : (b[0] % (i + 1));
    std::vector<std::string> classes;
    for (int k = 0; k < 3; k++) {
      if (b[2] >> k & 1u) {
        classes.push_back(std::string(1, static_cast<char>('x' + k)));
      }
    }
    const int e = t.add(parent, kNames[b[1] % 6], kIds[b[3] % 4], classes);
    if (b[4] % 5) {
      t.e[e].attrs["t"] = kAttrs[b[4] % 5];
    }
    t.e[e].state = kStates[b[5] % 8];
  }
  GTEXT_CSS_Selector_Options so = gtext_css_selector_options_default();
  GTEXT_CSS_Selector_List * list = nullptr;
  GTEXT_CSS_Selector_Result sr;
  std::memset(&sr, 0, sizeof sr);
  const GTEXT_CSS_Status ps = gtext_css_selector_parse(text.data(), text.size(), &so, &list, &sr);
  gtext_css_selector_result_free(&sr);
  if (ps != GTEXT_CSS_OK || !list) {
    return 0;
  }
  GTEXT_CSS_Selector_Match_Options opt = gtext_css_selector_match_options_default();
  switch (o0 & 3) {
  case 1: opt.max_work = 60; break;
  case 2: opt.max_memo_entries = 8; break;
  case 3: opt.max_has_visits = 6; break;
  default: break;
  }
  opt.html_document = (o0 >> 2) & 1u;
  const int scope_pick = (o0 >> 3) & 3;
  const int scope = scope_pick == 0 ? 0 : ((scope_pick - 1) % count) + 1;
  opt.scope = synth::Tree::H(scope);
  opt.quirks = (o0 >> 5) & 1u;
  FaultAllocator fa;
  const Outcome base = engine(t, list, opt, false, &fa.alloc);
  if (base.status == GTEXT_CSS_E_INTERNAL) {
    fail("an internal error", text, input);
  }
  if (base.status != GTEXT_CSS_OK && !limit_status(base.status) && base.status != GTEXT_CSS_E_OOM) {
    fail("a status that is neither a limit nor unsupported", gtext_css_status_string(base.status), input);
  }
  if (base.status == GTEXT_CSS_OK) {
    GTEXT_CSS_Selector_Match_Options variant = opt;
    variant.disable_memo = true;
    variant.max_work = 0;
    const Outcome nomemo = engine(t, list, variant, false, &fa.alloc);
    if (nomemo.status == GTEXT_CSS_OK && nomemo.matched != base.matched) {
      fail("the memo changed the answer", text + "\nwith: " + list_of(base.matched) + "\nwithout: " + list_of(nomemo.matched), input);
    }
    variant = opt;
    variant.disable_bloom = true;
    const Outcome nobloom = engine(t, list, variant, true, &fa.alloc);
    const Outcome withbloom = engine(t, list, opt, true, &fa.alloc);
    if (nobloom.status == GTEXT_CSS_OK && withbloom.status == GTEXT_CSS_OK && nobloom.matched != withbloom.matched) {
      fail("the ancestor filter changed the answer", text + "\nwith: " + list_of(withbloom.matched) + "\nwithout: " + list_of(nobloom.matched), input);
    }
    if (withbloom.status == GTEXT_CSS_OK && withbloom.matched != base.matched) {
      fail("a walk and single calls disagree", text + "\nwalk: " + list_of(withbloom.matched) + "\nsingle: " + list_of(base.matched), input);
    }
    Ref ref(t, scope, opt.html_document);
    std::vector<int> expect;
    for (int e = 1; e < static_cast<int>(t.e.size()); e++) {
      if (ref.any(list, e)) {
        expect.push_back(e);
      }
    }
    if (!ref.unsupported && expect != base.matched && !(opt.quirks)) {
      fail("the engine is not the reference", text + "\nreference: " + list_of(expect) + "\nengine: " + list_of(base.matched), input);
    }
  }
  gtext_css_selector_list_free(list);
  if (!fa.live.empty()) {
    fail("an allocation leaked", std::to_string(fa.live.size()) + " blocks", input);
  }
  if (fa.bad_frees) {
    fail("a block the allocator did not make was freed", "", input);
  }
  return 0;
}

}  // namespace css_matchfuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  return css_matchfuzz::run(data, size);
}
