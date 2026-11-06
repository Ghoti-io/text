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
 * libFuzzer harness for the mutators of the rule layer (src/css/cssom_insert.c, cssom_setters.c, cssom_media.c, cssom_typed.c, cssom_sheet.c): bytes decoded into sequences of
 * mutations on a document of three sheets (two parsed, one constructed), a model beside each, and the rules the model has removed or replaced still held.
 *
 * The input is a flag byte (bit 0: fail one allocation, which one is chosen by the next two bytes) and a list of operations, six bytes each: the operation, the sheet, an index, a
 * text, an argument and a spare. At most 48 run. The operations: `insertRule`, `deleteRule`, `replaceSync` (on the constructed sheet it works, on the others it is NotAllowedError),
 * the `selectorText` setter, `setProperty` on a rule's style, the `mediaText` setter of the sheet's media list, `appendMedium`, and a descriptor attribute of a counter style.
 *
 * The reference is a list per sheet of (rule object, `cssText`) kept by the harness: what a call must answer is decided from a **separate document** that parses the text of one rule
 * alone (the expected `cssText`, or that it is not one rule), and an expected status from the order of checks (the text before the index, then the index). The properties, after every
 * operation:
 *
 *   each sheet's length, the object at each index (the same pointer: identity is stable) and the `cssText` of each equal the model's;
 *   the status is the model's, never E_INTERNAL; an ignored or failed call leaves the sheet's dump byte-identical, and a call that changes something raises the sheet's version once;
 *   a rule the model has removed or replaced is detached (`parentStyleSheet` null) and still reads the text it had; the rules inside a detached rule likewise;
 *   the snapshot of each sheet is the compile of its serialization (the dump of one equals the dump of the other) and the same object when asked again;
 *   with one allocation failed: the call is OOM or untouched by the failure, and an OOM leaves the dump as it was;
 *   nothing leaks, and nothing is read after it is freed (under a sanitizer).
 *
 * Seeds: tests/fuzz/corpus/cssom-sheet, made by tools/css/make-fuzz-sheet-cssom-seeds.py.
 * Build with: make fuzz-cssom-sheet      Run: make fuzz-run-cssom-sheet
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

namespace cssom_sheetfuzz {

[[noreturn]] void fail(const char * what, const std::string & detail, const std::string & input) {
  std::fprintf(stderr, "fuzz_cssom_sheet: %s\n%s\ninput (%zu bytes): ", what, detail.c_str(), input.size());
  for (unsigned char c : input) {
    std::fprintf(stderr, "%02x", c);
  }
  std::fputc('\n', stderr);
  std::abort();
}

/// An allocator that fails the n-th attempt after it is armed and remembers what it holds.
struct Fault {
  GTEXT_Allocator alloc;
  std::set<void *> live;
  size_t calls = 0, fail_on = 0;
  bool failed = false;
  Fault() {
    alloc.ctx = this;
    alloc.malloc_fn = [](void * c, size_t n) -> void * {
      Fault * f = static_cast<Fault *>(c);
      if (f->hit()) {
        return nullptr;
      }
      void * p = std::malloc(n ? n : 1);
      if (p) {
        f->live.insert(p);
      }
      return p;
    };
    alloc.calloc_fn = [](void * c, size_t a, size_t b) -> void * {
      Fault * f = static_cast<Fault *>(c);
      if (f->hit()) {
        return nullptr;
      }
      void * p = std::calloc(a ? a : 1, b ? b : 1);
      if (p) {
        f->live.insert(p);
      }
      return p;
    };
    alloc.realloc_fn = [](void * c, void * old, size_t n) -> void * {
      Fault * f = static_cast<Fault *>(c);
      if (f->hit()) {
        return nullptr;
      }
      void * p = std::realloc(old, n ? n : 1);
      if (p) {
        if (old) {
          f->live.erase(old);
        }
        f->live.insert(p);
      }
      return p;
    };
    alloc.free_fn = [](void * c, void * p) {
      Fault * f = static_cast<Fault *>(c);
      if (p) {
        f->live.erase(p);
        std::free(p);
      }
    };
  }
  bool hit() {
    ++calls;
    if (fail_on != 0 && calls == fail_on) {
      failed = true;
      return true;
    }
    return false;
  }
  void arm(size_t n) {
    calls = 0;
    fail_on = n;
    failed = false;
  }
  void disarm() {
    fail_on = 0;
  }
};

const GTEXT_CSS_Registry * registry() {
  static GTEXT_CSS_Registry * r = [] {
    GTEXT_CSS_Registry * made = nullptr;
    gtext_css_registry_new(nullptr, &made);
    return made;
  }();
  return r;
}

const char * const kInit[] = {"a{color:red} b > c{margin:1px 2px} @media print{d{width:1px}} @keyframes k{from{opacity:0}} @counter-style n{system:cyclic;symbols:a b}",
    "x{y{color:red}} @font-face{font-family:A;src:url(a.woff2)} @page{margin:1px} @supports (display:grid){e{}}", "", "z{color:blue;margin:0}"};
const char * const kRules[] = {"a{color:red}", "b > c{margin:1px 2px}", "@media print{d{width:1px}}", "@supports (display:grid){e{}}", "@keyframes k{from{opacity:0}to{opacity:1}}",
    "@font-face{font-family:A;src:url(a.woff2)}", "@counter-style n{system:cyclic;symbols:a b}", "@page{margin:1px}", "x{y{color:red}}", "@layer l{f{}}", "a{", "", "a{}b{}", "}",
    "@media{", "bogus rule", "@import url(a.css);", "g{margin:1px;margin-top:2px}", "h:is(a,b){}", "@media screen{}"};
const char * const kSheets[] = {"a{} b{c{}}", "@import 'x.css'; d{}", "", "@media print{e{}} @keyframes k{50%{opacity:1}}", "@counter-style c{symbols:a b} @page{size:a3}", "p{q{r{}}}"};
const char * const kSelectors[] = {"d > e", "a, b", "", "a >", "1a", ":bogus", "x y z", ".k:not(.l)", "a{}b", "&", "> c", "*", "h1,h2,h3"};
const char * const kNames[] = {"color", "margin", "margin-top", "width", "bogus", "--v", "background"};
const char * const kValues[] = {"red", "1px", "", "nonsense", "1px 2px", "blue !important", "var(--x)"};
const char * const kMedia[] = {"print", "screen, (min-width: 1px)", "", "(bogus: 1) and", "tv", "not all", "print and (color)"};

/// The declarations of a rule's text, each as written, in the order written (the text between the braces split at its semicolons).
std::vector<std::string> declarations_of(const std::string & rule) {
  std::vector<std::string> out;
  const size_t open = rule.find(" {");
  if (open == std::string::npos) {
    return out;
  }
  std::string body = rule.substr(open + 2);
  while (!body.empty() && (body.back() == ' ' || body.back() == '}')) {
    body.pop_back();
  }
  size_t from = 0;
  while (from < body.size()) {
    const size_t semi = body.find(';', from);
    const size_t end = semi == std::string::npos ? body.size() : semi;
    size_t b = from;
    while (b < end && body[b] == ' ') {
      b++;
    }
    out.push_back(body.substr(b, end - b));
    from = end + 1;
  }
  return out;
}

/// Whether the library's text for a rule is the model's declarations in another order, in the one case the model cannot know the order.
///
/// The model sets the property on a block it makes from the rule's *text*. A shorthand written from its longhands loses the order the block held them in: `g{margin:1px;margin-top:2px}` is kept
/// (a parsed list moves the later declaration after the earlier ones, as both browsers do) as right, bottom, left, top and written `margin: 2px 1px 1px`, which reads back as top, right, bottom, left.
/// Only a longhand that holds `var()` cannot join a shorthand, so only there are the longhands written one by one and the order shows. The declarations must be the same set, each as written.
bool only_the_order_of_pending_longhands_differs(const std::string & actual, const std::string & expected) {
  if (actual == expected || actual.find("var(") == std::string::npos) {
    return false;
  }
  std::vector<std::string> a = declarations_of(actual);
  std::vector<std::string> e = declarations_of(expected);
  if (actual.substr(0, actual.find(" {")) != expected.substr(0, expected.find(" {")) || a.size() != e.size()) {
    return false;
  }
  std::sort(a.begin(), a.end());
  std::sort(e.begin(), e.end());
  return a == e;
}

struct Held {
  GTEXT_CSSOM_Rule * rule = nullptr;
  std::string text;
};

std::string rule_css(GTEXT_CSSOM_Rule * r, const GTEXT_Allocator * a) {
  char * out = nullptr;
  size_t n = 0;
  const GTEXT_CSS_Status st = gtext_cssom_rule_css_text(r, &out, &n);
  if (st != GTEXT_CSS_OK) {
    return std::string("<") + gtext_css_status_string(st) + ">";
  }
  std::string s(out, n);
  gtext_cssom_buffer_free(a, out);
  return s;
}

std::string dump_sheet(GTEXT_CSSOM_Sheet * s) {
  std::string out;
  GTEXT_Markup_Sink sink;
  std::memset(&sink, 0, sizeof sink);
  sink.user = &out;
  sink.write = [](void * u, const char * p, size_t n) {
    static_cast<std::string *>(u)->append(p, n);
    return GTEXT_MARKUP_OK;
  };
  gtext_cssom_sheet_dump(s, &sink);
  return out;
}

std::string dump_compiled(const GTEXT_CSS_Sheet * s) {
  std::string out;
  GTEXT_Markup_Sink sink;
  std::memset(&sink, 0, sizeof sink);
  sink.user = &out;
  sink.write = [](void * u, const char * p, size_t n) {
    static_cast<std::string *>(u)->append(p, n);
    return GTEXT_MARKUP_OK;
  };
  gtext_css_sheet_dump(s, &sink);
  return out;
}

/// The dump of the compile of a text: what the cascade would have from the sheet if it were read from this serialization.
bool rebuild_dump(const std::string & text, std::string & out) {
  GTEXT_CSS_Tree * tree = nullptr;
  if (gtext_css_parse_stylesheet(text.data(), text.size(), nullptr, &tree, nullptr) != GTEXT_CSS_OK) {
    return false;
  }
  GTEXT_CSS_Sheet * compiled = nullptr;
  const GTEXT_CSS_Status st = gtext_css_sheet_compile(tree, registry(), nullptr, &compiled, nullptr);
  gtext_css_tree_free(tree);
  if (st != GTEXT_CSS_OK) {
    return false;
  }
  out = dump_compiled(compiled);
  gtext_css_sheet_free(compiled);
  return true;
}

std::string serialize_sheet(const GTEXT_CSSOM_Sheet * s) {
  std::string out;
  GTEXT_Markup_Sink sink;
  std::memset(&sink, 0, sizeof sink);
  sink.user = &out;
  sink.write = [](void * u, const char * p, size_t n) {
    static_cast<std::string *>(u)->append(p, n);
    return GTEXT_MARKUP_OK;
  };
  gtext_cssom_sheet_serialize(s, &sink);
  return out;
}

struct Sheet {
  GTEXT_CSSOM_Sheet * s = nullptr;
  bool constructed = false;
  std::vector<Held> rules;      ///< The model: the objects and their text, in order.
  uint64_t version = 0;
  std::string media;
};

struct World {
  Fault fa;
  GTEXT_CSSOM_Document * doc = nullptr;
  GTEXT_CSSOM_Document * ref = nullptr;   ///< The separate document that reads one rule's text alone.
  Sheet sheets[3];
  std::vector<Held> detached;
};

GTEXT_CSSOM_Document * make_doc(const GTEXT_Allocator * a) {
  GTEXT_CSSOM_Config c = gtext_cssom_config_default();
  c.registry = registry();
  c.allocator = a;
  GTEXT_CSSOM_Document * d = nullptr;
  gtext_cssom_document_new(&c, &d);
  return d;
}

/// The cssText of every top-level rule of a text read as a constructed sheet in the reference document (the @import rules dropped), or one entry that says the text is not one rule.
std::vector<std::string> ref_rules(World & w, const std::string & text) {
  std::vector<std::string> out;
  GTEXT_CSSOM_Sheet * s = nullptr;
  if (gtext_cssom_sheet_new_constructed(w.ref, nullptr, &s) != GTEXT_CSS_OK) {
    return out;
  }
  gtext_cssom_sheet_replace_sync(s, text.data(), text.size(), nullptr);
  const GTEXT_CSSOM_Rule_List * l = nullptr;
  gtext_cssom_sheet_rules(s, &l);
  for (size_t i = 0; i < gtext_cssom_rule_list_length(l); i++) {
    GTEXT_CSSOM_Rule * r = gtext_cssom_rule_list_at(l, i);
    out.push_back(rule_css(r, nullptr));
    gtext_cssom_rule_release(r);
  }
  gtext_cssom_sheet_release(s);
  return out;
}

/// "parse a rule": one rule, alone. The text is one rule if it parses to one and `insertRule` of it into an empty reference sheet agrees.
bool ref_one_rule(World & w, const std::string & text, std::string & css) {
  GTEXT_CSSOM_Sheet * s = nullptr;
  if (gtext_cssom_sheet_new_parsed(w.ref, "", 0, nullptr, &s) != GTEXT_CSS_OK) {
    return false;
  }
  GTEXT_CSSOM_Rule * made = nullptr;
  const GTEXT_CSS_Status st = gtext_cssom_sheet_insert_rule(s, text.data(), text.size(), 0, &made, nullptr);
  bool ok = false;
  if (st == GTEXT_CSS_OK && made) {
    css = rule_css(made, nullptr);
    ok = true;
  }
  gtext_cssom_rule_release(made);
  gtext_cssom_sheet_release(s);
  return ok;
}

void resync(World & w, Sheet & m) {
  for (Held & h : m.rules) {
    gtext_cssom_rule_release(h.rule);
  }
  m.rules.clear();
  const GTEXT_CSSOM_Rule_List * l = nullptr;
  gtext_cssom_sheet_rules(m.s, &l);
  for (size_t i = 0; i < gtext_cssom_rule_list_length(l); i++) {
    Held h;
    h.rule = gtext_cssom_rule_list_at(l, i);
    h.text = rule_css(h.rule, &w.fa.alloc);
    m.rules.push_back(h);
  }
  m.version = gtext_cssom_sheet_version(m.s);
  (void)w;
}

void detach(World & w, Held h) {
  if (w.detached.size() >= 24) {
    gtext_cssom_rule_release(w.detached.front().rule);
    w.detached.erase(w.detached.begin());
  }
  w.detached.push_back(h);
}

void check(World & w, const std::string & input, const char * after) {
  for (int k = 0; k < 3; k++) {
    Sheet & m = w.sheets[k];
    const GTEXT_CSSOM_Rule_List * l = nullptr;
    gtext_cssom_sheet_rules(m.s, &l);
    if (gtext_cssom_rule_list_length(l) != m.rules.size()) {
      fail("the length differs from the model's", std::string(after) + " sheet " + std::to_string(k), input);
    }
    for (size_t i = 0; i < m.rules.size(); i++) {
      GTEXT_CSSOM_Rule * r = gtext_cssom_rule_list_at(l, i);
      if (r != m.rules[i].rule) {
        fail("the object at an index is not the model's: identity is not stable", std::string(after) + " sheet " + std::to_string(k) + " index " + std::to_string(i), input);
      }
      gtext_cssom_rule_release(r);
      const std::string css = rule_css(m.rules[i].rule, &w.fa.alloc);
      if (css != m.rules[i].text) {
        fail("cssText differs from the model's", std::string(after) + "\n" + css + "\n---\n" + m.rules[i].text, input);
      }
    }
    {
      // the snapshot of the sheet is the compile of its serialization, and is the same object until the sheet changes
      GTEXT_CSS_Sheet *snap = nullptr, *again = nullptr;
      if (gtext_cssom_sheet_snapshot(m.s, &snap) != GTEXT_CSS_OK || gtext_cssom_sheet_snapshot(m.s, &again) != GTEXT_CSS_OK) {
        fail("a snapshot could not be made", std::string(after) + " sheet " + std::to_string(k), input);
      }
      if (snap != again) {
        fail("two snapshots of a sheet that did not change are two objects", std::string(after) + " sheet " + std::to_string(k), input);
      }
      std::string want;
      const std::string text = serialize_sheet(m.s);
      if (!rebuild_dump(text, want)) {
        fail("the serialization of a sheet does not compile", text, input);
      }
      const std::string got = dump_compiled(snap);
      if (got != want) {
        fail("the snapshot of a sheet is not the compile of its serialization", std::string(after) + " sheet " + std::to_string(k) + "\n" + text + "\n--- snapshot\n" + got + "\n--- rebuild\n" + want, input);
      }
      gtext_css_sheet_free(snap);
      gtext_css_sheet_free(again);
    }
    if (gtext_cssom_sheet_version(m.s) != m.version) {
      fail("the sheet's version is not the model's", std::string(after) + " sheet " + std::to_string(k) + ": " + std::to_string(gtext_cssom_sheet_version(m.s)) + " vs " + std::to_string(m.version), input);
    }
  }
  for (const Held & h : w.detached) {
    GTEXT_CSSOM_Sheet * ps = gtext_cssom_rule_parent_sheet(h.rule);
    if (ps) {
      fail("a removed rule still has a parent style sheet", after, input);
    }
    if (rule_css(h.rule, &w.fa.alloc) != h.text) {
      fail("a removed rule does not read the text it had", after, input);
    }
  }
}

}  // namespace cssom_sheetfuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  using namespace cssom_sheetfuzz;
  if (size < 3) {
    return 0;
  }
  const std::string input(reinterpret_cast<const char *>(data), size);
  size_t pos = 0;
  auto next = [&]() -> unsigned { return pos < size ? data[pos++] : 0u; };
  const bool use_fault = (next() & 1) != 0;
  const size_t fault_n = 1 + (next() | (next() << 8)) % 600;
  const size_t fault_op = next() % 48;
  {
    World w;
    w.doc = make_doc(&w.fa.alloc);
    w.ref = make_doc(nullptr);
    if (!w.doc || !w.ref) {
      return 0;
    }
    for (int k = 0; k < 3; k++) {
      Sheet & m = w.sheets[k];
      m.constructed = k == 1;
      if (m.constructed) {
        gtext_cssom_sheet_new_constructed(w.doc, nullptr, &m.s);
        if (m.s) {
          gtext_cssom_sheet_replace_sync(m.s, kInit[3], std::strlen(kInit[3]), nullptr);
        }
      }
      else {
        const char * init = k == 0 ? kInit[0] : kInit[1];
        gtext_cssom_sheet_new_parsed(w.doc, init, std::strlen(init), nullptr, &m.s);
      }
      if (!m.s) {
        return 0;
      }
      resync(w, m);
    }
    size_t ops = 0;
    while (pos < size && ops < 48) {
      const unsigned op = next() % 8;
      const unsigned which = next() % 3;
      const unsigned idx = next();
      const unsigned t = next();
      const unsigned aux = next();
      next();
      Sheet & m = w.sheets[which];
      const bool armed = use_fault && ops == fault_op;
      std::string before = dump_sheet(m.s);
      GTEXT_CSS_Status st = GTEXT_CSS_OK;
      GTEXT_CSSOM_Outcome o;
      std::memset(&o, 0, sizeof o);
      const char * what = "";
      bool unmodelled = false;
      std::vector<std::string> expect_css;   // replaced rules
      std::string expect_one;
      bool expect_valid = false;
      GTEXT_CSS_Status want = GTEXT_CSS_OK;
      bool changed_expected = false;
      Held made;
      if (op == 0) {
        what = "insertRule";
        const std::string text = kRules[t % (sizeof kRules / sizeof *kRules)];
        const size_t at = idx % (m.rules.size() + 3);
        expect_valid = ref_one_rule(w, text, expect_one);
        const bool import = text.rfind("@import", 0) == 0;
        if (import) {
          unmodelled = true;
          want = m.constructed ? GTEXT_CSS_E_SYNTAX : GTEXT_CSS_OK;
        }
        else {
          want = !expect_valid ? GTEXT_CSS_E_SYNTAX : at > m.rules.size() ? GTEXT_CSS_E_INDEX_SIZE : GTEXT_CSS_OK;
        }
        if (armed) {
          w.fa.arm(fault_n);
        }
        GTEXT_CSSOM_Rule * r = nullptr;
        st = gtext_cssom_sheet_insert_rule(m.s, text.data(), text.size(), at, &r, &o);
        w.fa.disarm();
        if (st == GTEXT_CSS_OK && r) {
          made.rule = r;
          made.text = rule_css(r, &w.fa.alloc);
          if (!unmodelled && made.text != expect_one) {
            fail("the inserted rule's cssText is not the separate parse's", made.text + "\n" + expect_one, input);
          }
          m.rules.insert(m.rules.begin() + static_cast<long>(at), made);
          changed_expected = true;
        }
        if (unmodelled && st == GTEXT_CSS_OK) {
          gtext_cssom_rule_release(made.rule);
          m.rules.erase(m.rules.begin() + static_cast<long>(at));
          resync(w, m);
          m.version = gtext_cssom_sheet_version(m.s);
          changed_expected = false;
          want = GTEXT_CSS_OK;
        }
      }
      else if (op == 1) {
        what = "deleteRule";
        const size_t at = idx % (m.rules.size() + 2);
        want = at >= m.rules.size() ? GTEXT_CSS_E_INDEX_SIZE : GTEXT_CSS_OK;
        unmodelled = at < m.rules.size() && m.rules[at].text.rfind("@namespace", 0) == 0;
        if (armed) {
          w.fa.arm(fault_n);
        }
        st = gtext_cssom_sheet_delete_rule(m.s, at, &o);
        w.fa.disarm();
        if (st == GTEXT_CSS_OK) {
          detach(w, m.rules[at]);
          m.rules.erase(m.rules.begin() + static_cast<long>(at));
          changed_expected = true;
        }
      }
      else if (op == 2) {
        what = "replaceSync";
        const std::string text = kSheets[t % (sizeof kSheets / sizeof *kSheets)];
        want = m.constructed ? GTEXT_CSS_OK : GTEXT_CSS_E_NOT_ALLOWED;
        if (m.constructed) {
          expect_css = ref_rules(w, text);
        }
        if (armed) {
          w.fa.arm(fault_n);
        }
        st = gtext_cssom_sheet_replace_sync(m.s, text.data(), text.size(), &o);
        w.fa.disarm();
        if (st == GTEXT_CSS_OK) {
          for (Held & h : m.rules) {
            detach(w, h);
          }
          m.rules.clear();
          const GTEXT_CSSOM_Rule_List * l = nullptr;
          gtext_cssom_sheet_rules(m.s, &l);
          if (gtext_cssom_rule_list_length(l) != expect_css.size()) {
            fail("replaceSync made a different number of rules from the separate parse", std::to_string(gtext_cssom_rule_list_length(l)) + " vs " + std::to_string(expect_css.size()), input);
          }
          for (size_t i = 0; i < expect_css.size(); i++) {
            Held h;
            h.rule = gtext_cssom_rule_list_at(l, i);
            h.text = rule_css(h.rule, &w.fa.alloc);
            if (h.text != expect_css[i]) {
              fail("a rule of replaceSync is not the separate parse's", h.text + "\n" + expect_css[i], input);
            }
            m.rules.push_back(h);
          }
          changed_expected = true;
        }
      }
      else if (op == 3 || op == 4) {
        what = op == 3 ? "selectorText" : "setProperty";
        // a plain style rule with no rules inside it: `sel { decls }`
        size_t at = m.rules.size();
        for (size_t i = 0; i < m.rules.size() && at == m.rules.size(); i++) {
          const size_t j = (idx + i) % m.rules.size();
          GTEXT_CSSOM_Rule * r = m.rules[j].rule;
          if (gtext_cssom_rule_kind(r) == GTEXT_CSSOM_RULE_STYLE && m.rules[j].text.find('\n') == std::string::npos) {
            at = j;
          }
        }
        if (at == m.rules.size()) {
          ops++;
          continue;
        }
        GTEXT_CSSOM_Rule * r = m.rules[at].rule;
        if (op == 4) {
          /* the block of a rule is made the first time it is asked for, which the dump shows: not a change the call is accused of */
          GTEXT_CSSOM_Decl * pre = nullptr;
          if (gtext_cssom_rule_style(r, &pre) == GTEXT_CSS_OK) {
            gtext_cssom_decl_release(pre);
          }
          before = dump_sheet(m.s);
        }
        const std::string old = m.rules[at].text;
        const size_t brace = old.find(" {");
        const std::string body = old.substr(brace);
        std::string expected_text = old;
        if (op == 3) {
          const std::string sel = kSelectors[t % (sizeof kSelectors / sizeof *kSelectors)];
          std::string css;
          const bool valid = ref_one_rule(w, sel + (body == " { }" ? "{}" : body), css);
          if (valid) {
            expected_text = css;
          }
          if (armed) {
            w.fa.arm(fault_n);
          }
          st = gtext_cssom_rule_set_selector_text(r, sel.data(), sel.size(), &o);
          w.fa.disarm();
          changed_expected = st == GTEXT_CSS_OK && valid;
        }
        else {
          const std::string name = kNames[t % (sizeof kNames / sizeof *kNames)];
          const std::string value = kValues[aux % (sizeof kValues / sizeof *kValues)];
          std::string decls = body == " { }" ? "" : body.substr(3, body.size() - 5);
          GTEXT_CSSOM_Decl * rd = nullptr;
          gtext_cssom_decl_new(w.ref, &rd);
          gtext_cssom_decl_set_css_text(rd, decls.data(), decls.size(), nullptr);
          GTEXT_CSSOM_Outcome ro;
          std::memset(&ro, 0, sizeof ro);
          const bool important = value.find("!important") != std::string::npos;
          const std::string v = important ? value.substr(0, value.find(" !")) : value;
          gtext_cssom_decl_set_property(rd, name.data(), name.size(), v.data(), v.size(), important ? "important" : "", important ? 9 : 0, &ro);
          char * dt = nullptr;
          size_t dn = 0;
          gtext_cssom_decl_css_text(rd, &dt, &dn);
          const std::string new_decls(dt ? dt : "", dn);
          gtext_cssom_buffer_free(nullptr, dt);
          gtext_cssom_decl_release(rd);
          expected_text = old.substr(0, brace) + (new_decls.empty() ? " { }" : " { " + new_decls + " }");
          GTEXT_CSSOM_Decl * d = nullptr;
          if (armed) {
            w.fa.arm(fault_n);
          }
          st = gtext_cssom_rule_style(r, &d);
          if (st == GTEXT_CSS_OK) {
            const bool vi = important;
            st = gtext_cssom_decl_set_property(d, name.data(), name.size(), v.data(), v.size(), vi ? "important" : "", vi ? 9 : 0, &o);
            gtext_cssom_decl_release(d);
          }
          w.fa.disarm();
          changed_expected = st == GTEXT_CSS_OK && o.kind == GTEXT_CSSOM_CHANGED;
        }
        if (st == GTEXT_CSS_OK) {
          if (op == 4 && only_the_order_of_pending_longhands_differs(rule_css(r, &w.fa.alloc), expected_text)) {
            expected_text = rule_css(r, &w.fa.alloc);
          }
          m.rules[at].text = expected_text;
        }
      }
      else if (op == 5 || op == 6) {
        what = op == 5 ? "mediaText" : "appendMedium";
        const std::string text = kMedia[t % (sizeof kMedia / sizeof *kMedia)];
        GTEXT_CSSOM_Media_List * list = nullptr;
        if (armed) {
          w.fa.arm(fault_n);
        }
        st = gtext_cssom_sheet_media(m.s, &list);
        if (st == GTEXT_CSS_OK) {
          st = op == 5 ? gtext_cssom_media_set_text(list, text.data(), text.size(), &o) : gtext_cssom_media_append(list, text.data(), text.size(), &o);
        }
        w.fa.disarm();
        changed_expected = st == GTEXT_CSS_OK && o.kind == GTEXT_CSSOM_CHANGED;
        if (list) {
          char * mt = nullptr;
          size_t mn = 0;
          if (gtext_cssom_media_text(list, &mt, &mn) == GTEXT_CSS_OK) {
            const std::string now(mt, mn);
            gtext_cssom_buffer_free(&w.fa.alloc, mt);
            if (st == GTEXT_CSS_OK && op == 5) {
              // the same text set through a second list object (another sheet's, in the reference document) reads back the same
              GTEXT_CSSOM_Sheet * rs = nullptr;
              gtext_cssom_sheet_new_parsed(w.ref, "", 0, nullptr, &rs);
              GTEXT_CSSOM_Media_List * rl = nullptr;
              gtext_cssom_sheet_media(rs, &rl);
              gtext_cssom_media_set_text(rl, text.data(), text.size(), nullptr);
              char * rt = nullptr;
              size_t rn = 0;
              gtext_cssom_media_text(rl, &rt, &rn);
              const std::string expected(rt, rn);
              gtext_cssom_buffer_free(nullptr, rt);
              gtext_cssom_media_release(rl);
              gtext_cssom_sheet_release(rs);
              if (now != expected) {
                fail("mediaText is not the separate parse's", now + "\n" + expected, input);
              }
            }
          }
          gtext_cssom_media_release(list);
        }
      }
      else {
        what = "counter style attribute";
        size_t at = m.rules.size();
        for (size_t i = 0; i < m.rules.size() && at == m.rules.size(); i++) {
          const size_t j = (idx + i) % m.rules.size();
          if (gtext_cssom_rule_kind(m.rules[j].rule) == GTEXT_CSSOM_RULE_COUNTER_STYLE) {
            at = j;
          }
        }
        if (at == m.rules.size()) {
          ops++;
          continue;
        }
        static const char * const values[] = {"x y z", "", "bogus (", "'a' 'b'", "1 2"};
        const std::string v = values[t % 5];
        if (armed) {
          w.fa.arm(fault_n);
        }
        st = gtext_cssom_rule_set_attribute(m.rules[at].rule, "symbols", 7, v.data(), v.size(), &o);
        w.fa.disarm();
        changed_expected = st == GTEXT_CSS_OK && o.kind == GTEXT_CSSOM_CHANGED;
        if (changed_expected) {
          m.rules[at].text = rule_css(m.rules[at].rule, &w.fa.alloc);   // the model of a counter style is the library's own writer, read after: the properties below still hold
        }
      }
      ops++;
      if (st == GTEXT_CSS_E_INTERNAL) {
        fail("E_INTERNAL", what, input);
      }
      if (w.fa.failed || (armed && w.fa.calls >= fault_n)) {
        if (st != GTEXT_CSS_OK && st != GTEXT_CSS_E_OOM && !(op == 0 || op == 1 || op == 2)) {
          fail("an allocation failure was reported as another status", std::string(what) + " " + gtext_css_status_string(st), input);
        }
      }
      bool has_import = false;
      for (const Held & h : m.rules) {
        has_import = has_import || h.text.rfind("@import", 0) == 0;
      }
      /* with an @import in the sheet the order of its top decides where a rule may go: the model does not say */
      const bool order_open = has_import || (op == 0 && st == GTEXT_CSS_E_HIERARCHY && want == GTEXT_CSS_OK && has_import);
      if (!unmodelled && !(order_open && st == GTEXT_CSS_E_HIERARCHY) && (op == 0 || op == 1 || op == 2) && st != GTEXT_CSS_E_OOM && st != want) {
        fail("the status is not the model's", std::string(what) + ": " + gtext_css_status_string(st) + " vs " + gtext_css_status_string(want), input);
      }
      const bool no_change = st != GTEXT_CSS_OK || o.kind >= GTEXT_CSSOM_IGNORED_UNKNOWN_PROPERTY || o.kind == GTEXT_CSSOM_UNCHANGED;
      if (st != GTEXT_CSS_OK && dump_sheet(m.s) != before) {
        fail("a failed call changed the sheet", what, input);
      }
      if (st == GTEXT_CSS_OK && no_change && !changed_expected && dump_sheet(m.s) != before) {
        fail("an ignored call changed the sheet", what, input);
      }
      if (changed_expected && st == GTEXT_CSS_OK) {
        m.version += 1;
      }
      if (st == GTEXT_CSS_OK && !changed_expected && gtext_cssom_sheet_version(m.s) != m.version) {
        // a call the model did not expect to change anything moved the version: resync only for the calls the model leaves open
        if (!unmodelled) {
          fail("a call that changed nothing raised the version", what, input);
        }
        m.version = gtext_cssom_sheet_version(m.s);
      }
      check(w, input, what);
    }
    for (int k = 0; k < 3; k++) {
      for (Held & h : w.sheets[k].rules) {
        gtext_cssom_rule_release(h.rule);
      }
      w.sheets[k].rules.clear();
      gtext_cssom_sheet_release(w.sheets[k].s);
    }
    for (Held & h : w.detached) {
      gtext_cssom_rule_release(h.rule);
    }
    w.detached.clear();
    gtext_cssom_document_release(w.doc);
    gtext_cssom_document_release(w.ref);
    if (!w.fa.live.empty()) {
      fail("memory leaked", std::to_string(w.fa.live.size()) + " blocks", input);
    }
  }
  return 0;
}
