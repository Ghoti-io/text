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
 * libFuzzer harness for the document layer (src/css/cssom_markup.c, cssom_events.c, cssom_scope.c, cssom_index.c): bytes decoded into a sequence of edits of a Markup tree
 * interleaved with CSSOM operations, `sync` of the Markup binding, the one-element path, batches and adopted sheets.
 *
 * The input is a flag byte (bit 0: fail one allocation, chosen by the next two bytes, in the operation the fourth byte names) and a list of operations, six bytes each: the operation,
 * four arguments and a spare. At most 40 run. The operations: set a style element's text, detach a style element, put it back, set its `media`, `title` or `type`, make a new style
 * element, set an unrelated attribute or the `style` attribute of an element, `insertRule`, `deleteRule` and `setProperty` on the sheet of a style element, the same through an inline
 * `style` declaration, `sync`, the one-element path, a batch begun or ended, and a constructed sheet adopted or taken out.
 *
 * The reference is written apart from the code under test: a walk of the Markup tree that decides which elements are style elements and what their text is, a document built **afresh from
 * the serialized state** of the live one (each sheet of `styleSheets` and `adoptedStyleSheets` made again from its serialization, its media and its disabled flag), and a model of the
 * events. The properties:
 *
 *   after a successful sync, `styleSheets` lists exactly the sheets of the elements the walk finds, in tree order, each owned by its element, and a sheet no CSSOM call touched
 *     serializes as the sheet made from the element's text;
 *   the cascaded value of probe properties of probe elements through the live document's index equals the fresh document's;
 *   a sync with the Markup generation unchanged walks nothing;
 *   the binding and the scope agree after every operation, a failed one included (each listed sheet is its element's; no element's sheet is unlisted);
 *   the declaration of an element reads what its `style` attribute says, whoever wrote it;
 *   the events: generations strictly increase, the last is the document's, the set of sheets they say are listed is the scope's, and the path of an inserted rule found at its event
 *     leads to the rule when no mutation came after it;
 *   with one allocation failed: the call is OOM or untouched by the failure, and the next sync ends where an unswept one would;
 *   nothing leaks, and nothing is read after it is freed (under a sanitizer).
 *
 * Seeds: tests/fuzz/corpus/cssom-document, made by tools/css/make-fuzz-document-cssom-seeds.py.
 * Build with: make fuzz-cssom-document      Run: make fuzz-run-cssom-document
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <ghoti.io/text/css.h>
#include <ghoti.io/text/html.h>
#include <ghoti.io/text/markup.h>

namespace cssom_docfuzz {

[[noreturn]] void fail(const char * what, const std::string & detail, const std::string & input) {
  std::fprintf(stderr, "fuzz_cssom_document: %s\n%s\ninput (%zu bytes): ", what, detail.c_str(), input.size());
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

const char kHtml[] =
    "<!DOCTYPE html><html><head><style id=s0>p{color:red}</style><style id=s1 media=screen>.a{margin-top:1px}</style></head>"
    "<body><div id=d><p id=p1 class=a>x</p><p id=p2>y</p></div><style id=s2>#p2{color:blue}</style></body></html>";

const char * const kTexts[] = {"p{color:red}", ".a{margin-top:2px}", "#p2{color:green} p{margin-top:3px}", "", "@media print{p{color:red}} div p{color:blue}", "x{", "@import url(a.css); p{width:1px}", ".a, #p1{color:orange}",
    "@keyframes k{from{opacity:0}} p{animation-name:k}", "p{color:red} p{color:blue !important}"};
const char * const kMedia[] = {"screen", "print", "", "(min-width: 1px)", "not all", "(bogus:"};
const char * const kTypes[] = {"text/css", "TEXT/CSS", "", "text/plain", "text/css; x", " text/css"};
const char * const kRules[] = {"p{color:red}", ".a{margin-top:9px}", "@media screen{p{color:pink}}", "div p{width:2px}", "}", "@import url(b.css);", "#p2{color:gold}"};
const char * const kProps[] = {"color", "margin-top", "width", "bogus", "--v"};
const char * const kValues[] = {"red", "1px", "", "nonsense", "blue !important", "var(--x)"};
const char * const kInline[] = {"color: red", "margin-top: 1px; color: blue", "", "bogus: 1", "width: 3px !important"};
const char * const kProbeProps[] = {"color", "margin-top", "width"};
const char * const kProbeIds[] = {"p1", "p2", "d"};

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

std::string media_of(GTEXT_CSSOM_Sheet * s, const GTEXT_Allocator * a) {
  GTEXT_CSSOM_Media_List * m = nullptr;
  if (gtext_cssom_sheet_media(s, &m) != GTEXT_CSS_OK) {
    return "";
  }
  char * t = nullptr;
  size_t n = 0;
  std::string out;
  if (gtext_cssom_media_text(m, &t, &n) == GTEXT_CSS_OK) {
    out.assign(t, n);
    gtext_cssom_buffer_free(a, t);
  }
  gtext_cssom_media_release(m);
  return out;
}

struct Mirror;

struct World {
  Fault fa;
  GTEXT_Markup_Document * markup = nullptr;
  GTEXT_CSS_Markup_Adapter * adapter = nullptr;
  GTEXT_CSS_Selector_Match_Options mo{};
  GTEXT_CSSOM_Document * doc = nullptr;
  GTEXT_CSSOM_Markup_Binding * binding = nullptr;
  std::vector<GTEXT_Markup_Node *> styles;     ///< Every style element the harness knows, attached or not.
  std::set<GTEXT_Markup_Node *> detached;
  std::set<const void *> touched;              ///< Sheets a CSSOM call changed.
  std::vector<GTEXT_CSSOM_Decl *> inlines;     ///< Inline declarations held, by element.
  std::vector<GTEXT_Markup_Node *> inline_elements;
  std::vector<GTEXT_CSSOM_Sheet *> adopted;    ///< Constructed sheets made.
  std::set<uint64_t> ids;                      ///< The sheets the events say are listed.
  uint64_t last_event_generation = 0;
  uint64_t synced_generation = 0;
  bool synced = false;
  bool event_failure = false;
  std::string event_detail;
  std::string input;
};

GTEXT_CSS_Status observer(void * user, const GTEXT_CSSOM_Event * e) {
  World & w = *static_cast<World *>(user);
  if (e->generation <= w.last_event_generation && w.last_event_generation != 0) {
    w.event_failure = true;
    w.event_detail = "event generations do not increase";
  }
  w.last_event_generation = e->generation;
  GTEXT_CSSOM_Scope * own = gtext_cssom_document_scope(w.doc);
  if (e->scope == own && e->sheet) {
    if (e->kind == GTEXT_CSSOM_EVENT_SHEET_ADDED) {
      w.ids.insert(e->sheet_id);
    }
    if (e->kind == GTEXT_CSSOM_EVENT_SHEET_REMOVED) {
      w.ids.erase(e->sheet_id);
      w.touched.erase(e->sheet);
    }
  }
  if (e->kind == GTEXT_CSSOM_EVENT_RULE_INSERTED && e->has_path && e->sheet && e->rule && e->generation == gtext_cssom_document_generation(w.doc)) {
    // no mutation came after this one (delivered at once, or the last of a batch): the path found at the event leads to the rule
    const GTEXT_CSSOM_Rule_List * l = nullptr;
    gtext_cssom_sheet_rules(e->sheet, &l);
    GTEXT_CSSOM_Rule * r = nullptr;
    for (size_t i = 0; i < e->path_length && l; i++) {
      GTEXT_CSSOM_Rule * next = gtext_cssom_rule_list_at(l, e->path[i]);
      gtext_cssom_rule_release(r);
      r = next;
      l = r ? gtext_cssom_rule_css_rules(r) : nullptr;
    }
    if (r != e->rule) {
      w.event_failure = true;
      w.event_detail = "the path of an inserted rule does not lead to it";
    }
    gtext_cssom_rule_release(r);
  }
  return GTEXT_CSS_OK;
}

/* ---- the model: which elements are style elements, and what they say ------------------------------------------------------------------------------ */

struct Expect {
  GTEXT_Markup_Node * el;
  std::string text;
};

bool attr_of(GTEXT_Markup_Node * e, const char * name, std::string & v) {
  GTEXT_Markup_Attribute a;
  if (!gtext_markup_element_get_attribute(e, nullptr, name, &a)) {
    return false;
  }
  v.assign(a.value, a.value_length);
  return true;
}

/// The style elements of the tree in document order, by their own reading of the HTML Standard (written here, not taken from the binding).
std::vector<Expect> model(World & w) {
  std::vector<Expect> out;
  GTEXT_Markup_Node * root = gtext_markup_document_root(w.markup);
  for (GTEXT_Markup_Node * x = gtext_markup_node_next_in_order(root, root, false); x; x = gtext_markup_node_next_in_order(root, x, false)) {
    if (gtext_markup_node_kind(x) != GTEXT_MARKUP_KIND_ELEMENT || !gtext_markup_element_is(x, GTEXT_MARKUP_NS_HTML, "style")) {
      continue;
    }
    std::string type;
    if (attr_of(x, "type", type)) {
      std::string lower;
      for (char c : type) {
        lower += (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
      }
      if (!lower.empty() && lower != "text/css") {
        continue;
      }
    }
    Expect e;
    e.el = x;
    for (GTEXT_Markup_Node * c = gtext_markup_node_first_child(x); c; c = gtext_markup_node_next_sibling(c)) {
      if (gtext_markup_node_kind(c) == GTEXT_MARKUP_KIND_TEXT) {
        size_t n = 0;
        const char * d = gtext_markup_node_data(c, &n);
        e.text.append(d, n);
      }
    }
    out.push_back(e);
  }
  return out;
}

/* ---- the cascade through an index ------------------------------------------------------------------------------------------------------------------ */

std::string cascaded(World & w, const GTEXT_CSS_Rule_Index * index, GTEXT_Markup_Node * el, const char * property) {
  GTEXT_CSS_Selector_Match_Context * ctx = nullptr;
  if (gtext_css_selector_match_context_new(gtext_css_markup_interface(w.adapter), &w.mo, &ctx) != GTEXT_CSS_OK) {
    return "<no context>";
  }
  GTEXT_CSS_Candidates * set = nullptr;
  std::string out = "<error>";
  if (gtext_css_candidates_new(registry(), nullptr, nullptr, &set) == GTEXT_CSS_OK) {
    if (gtext_css_rules_collect(index, ctx, gtext_css_markup_handle(el), nullptr, set, nullptr, nullptr) == GTEXT_CSS_OK) {
      GTEXT_CSS_Cascaded * c = nullptr;
      if (gtext_css_cascade(set, nullptr, &c, nullptr) == GTEXT_CSS_OK) {
        GTEXT_CSS_Cascade_Entry e;
        out = "-";
        if (gtext_css_cascaded_find(c, gtext_css_registry_find_property(registry(), property, std::strlen(property)), nullptr, 0, &e)) {
          const GTEXT_CSS_Value * root = gtext_css_value_set_root(e.set);
          char * t = nullptr;
          size_t n = 0;
          out = "?";
          if (root && gtext_css_serialize_value_buffer(root, nullptr, &t, &n) == GTEXT_CSS_OK) {
            out.assign(t, n);
            gtext_css_serialize_buffer_free(nullptr, t);
          }
        }
        gtext_css_cascaded_free(c);
      }
    }
    gtext_css_candidates_free(set);
  }
  gtext_css_selector_match_context_free(ctx);
  return out;
}

GTEXT_Markup_Node * by_id(World & w, const char * id) {
  GTEXT_Markup_Node * root = gtext_markup_document_root(w.markup);
  for (GTEXT_Markup_Node * x = gtext_markup_node_next_in_order(root, root, false); x; x = gtext_markup_node_next_in_order(root, x, false)) {
    std::string v;
    if (gtext_markup_node_kind(x) == GTEXT_MARKUP_KIND_ELEMENT && attr_of(x, "id", v) && v == id) {
      return x;
    }
  }
  return nullptr;
}

/// A document built again from the serialized state of the live one: its `styleSheets` and its adopted sheets as new sheets made from their serializations.
GTEXT_CSSOM_Document * rebuild(World & w) {
  GTEXT_CSSOM_Config c = gtext_cssom_config_default();
  c.registry = registry();
  c.element_interface = gtext_css_markup_interface(w.adapter);
  c.match_options = &w.mo;
  GTEXT_CSSOM_Document * fresh = nullptr;
  if (gtext_cssom_document_new(&c, &fresh) != GTEXT_CSS_OK) {
    return nullptr;
  }
  GTEXT_CSSOM_Scope * from = gtext_cssom_document_scope(w.doc);
  GTEXT_CSSOM_Scope * to = gtext_cssom_document_scope(fresh);
  for (size_t i = 0; i < gtext_cssom_scope_sheet_count(from); i++) {
    GTEXT_CSSOM_Sheet * s = gtext_cssom_scope_sheet_at(from, i);
    const std::string text = serialize_sheet(s);
    const std::string media = media_of(s, gtext_cssom_document_allocator(w.doc));
    GTEXT_CSSOM_Sheet_Options o = gtext_cssom_sheet_options_default();
    o.media = media.c_str();
    o.disabled = gtext_cssom_sheet_disabled(s);
    GTEXT_CSSOM_Sheet * made = nullptr;
    if (gtext_cssom_sheet_new_parsed(fresh, text.data(), text.size(), &o, &made) == GTEXT_CSS_OK) {
      gtext_cssom_scope_insert_sheet(to, made, gtext_cssom_scope_sheet_count(to), nullptr);
      gtext_cssom_sheet_release(made);
    }
    gtext_cssom_sheet_release(s);
  }
  std::map<const void *, GTEXT_CSSOM_Sheet *> made_for;   // a sheet listed twice is one sheet, listed twice
  for (size_t i = 0; i < gtext_cssom_scope_adopted_count(from); i++) {
    GTEXT_CSSOM_Sheet * s = gtext_cssom_scope_adopted_at(from, i);
    GTEXT_CSSOM_Sheet *& made = made_for[s];
    if (!made && gtext_cssom_sheet_new_constructed(fresh, nullptr, &made) == GTEXT_CSS_OK) {
      const std::string text = serialize_sheet(s);
      gtext_cssom_sheet_replace_sync(made, text.data(), text.size(), nullptr);
    }
    if (made) {
      gtext_cssom_scope_adopted_insert(to, made, gtext_cssom_scope_adopted_count(to), nullptr);
    }
    gtext_cssom_sheet_release(s);
  }
  for (auto & kv : made_for) {
    gtext_cssom_sheet_release(kv.second);
  }
  return fresh;
}

/* ---- the properties ------------------------------------------------------------------------------------------------------------------------------------- */

void check_consistent(World & w, const char * after) {
  GTEXT_CSSOM_Scope * scope = gtext_cssom_document_scope(w.doc);
  std::set<const void *> listed;
  for (size_t i = 0; i < gtext_cssom_scope_sheet_count(scope); i++) {
    GTEXT_CSSOM_Sheet * s = gtext_cssom_scope_sheet_at(scope, i);
    listed.insert(s);
    const void * owner = gtext_cssom_sheet_owner_node(s);
    if (!owner) {
      fail("a listed sheet has no owner", after, w.input);
    }
    GTEXT_CSSOM_Sheet * mine = gtext_cssom_markup_sheet_of(w.binding, static_cast<const GTEXT_Markup_Node *>(owner));
    if (mine != s) {
      fail("a listed sheet is not its element's sheet in the binding", after, w.input);
    }
    gtext_cssom_sheet_release(mine);
    gtext_cssom_sheet_release(s);
  }
  for (GTEXT_Markup_Node * e : w.styles) {
    GTEXT_CSSOM_Sheet * s = gtext_cssom_markup_sheet_of(w.binding, e);
    if (s && !listed.count(s)) {
      fail("the binding has a sheet for an element that is not listed", after, w.input);
    }
    gtext_cssom_sheet_release(s);
  }
  if (w.event_failure) {
    fail("an event property failed", w.event_detail + " after " + after, w.input);
  }
  if (gtext_cssom_document_batch_depth(w.doc) == 0) {
    if (w.last_event_generation != gtext_cssom_document_generation(w.doc)) {
      fail("the last event is not the document's generation", std::string(after) + " " + std::to_string(w.last_event_generation) + " vs " + std::to_string(gtext_cssom_document_generation(w.doc)), w.input);
    }
    std::set<uint64_t> real;
    for (size_t i = 0; i < gtext_cssom_scope_sheet_count(scope); i++) {
      GTEXT_CSSOM_Sheet * s = gtext_cssom_scope_sheet_at(scope, i);
      real.insert(gtext_cssom_sheet_id(s));
      gtext_cssom_sheet_release(s);
    }
    if (real != w.ids) {
      fail("the sheets the events say are listed are not the scope's", after, w.input);
    }
  }
}

/// After a successful sync: the model, the reference sheets, and the fresh document's cascade.
void check_synced(World & w, const char * after) {
  GTEXT_CSSOM_Scope * scope = gtext_cssom_document_scope(w.doc);
  const std::vector<Expect> want = model(w);
  if (gtext_cssom_scope_sheet_count(scope) != want.size()) {
    fail("styleSheets does not list the elements the walk finds", std::string(after) + ": " + std::to_string(gtext_cssom_scope_sheet_count(scope)) + " vs " + std::to_string(want.size()), w.input);
  }
  for (size_t i = 0; i < want.size(); i++) {
    GTEXT_CSSOM_Sheet * s = gtext_cssom_scope_sheet_at(scope, i);
    if (gtext_cssom_sheet_owner_node(s) != want[i].el) {
      fail("the sheets are not in the tree order of their elements", std::string(after) + " index " + std::to_string(i), w.input);
    }
    if (!w.touched.count(s)) {
      GTEXT_CSSOM_Sheet * ref = nullptr;
      if (gtext_cssom_sheet_new_parsed(w.doc, want[i].text.data(), want[i].text.size(), nullptr, &ref) == GTEXT_CSS_OK) {
        if (serialize_sheet(ref) != serialize_sheet(s)) {
          fail("a sheet no call touched is not the sheet of its element's text", std::string(after) + "\n" + want[i].text + "\n---\n" + serialize_sheet(s), w.input);
        }
        gtext_cssom_sheet_release(ref);
      }
    }
    gtext_cssom_sheet_release(s);
  }
  GTEXT_CSSOM_Document * fresh = rebuild(w);
  if (!fresh) {
    return;
  }
  const GTEXT_CSS_Rule_Index * live_index = nullptr;
  const GTEXT_CSS_Rule_Index * fresh_index = nullptr;
  if (gtext_cssom_document_index(w.doc, &live_index) == GTEXT_CSS_OK && gtext_cssom_document_index(fresh, &fresh_index) == GTEXT_CSS_OK) {
    if (gtext_css_rule_index_record_count(live_index) != gtext_css_rule_index_record_count(fresh_index)) {
      fail("the index of the live document has not the records of the document built again", after, w.input);
    }
    for (const char * id : kProbeIds) {
      GTEXT_Markup_Node * el = by_id(w, id);
      if (!el) {
        continue;
      }
      for (const char * prop : kProbeProps) {
        const std::string a = cascaded(w, live_index, el, prop), b = cascaded(w, fresh_index, el, prop);
        if (a != b) {
          fail("a cascaded value differs from the document built again from the serialized state", std::string(after) + " " + id + " " + prop + ": " + a + " vs " + b, w.input);
        }
      }
    }
  }
  gtext_cssom_document_release(fresh);
}

}  // namespace cssom_docfuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  using namespace cssom_docfuzz;
  if (size < 4) {
    return 0;
  }
  const std::string input(reinterpret_cast<const char *>(data), size);
  const bool armed = (data[0] & 1) != 0;
  const size_t fault_n = 1 + ((static_cast<size_t>(data[1]) | (static_cast<size_t>(data[2]) << 8)) % 3000);
  const size_t fault_op = data[3] % 40;
  size_t ops = 0;
  {
    World w;
    w.input = input;
    GTEXT_HTML_Result hr;
    std::memset(&hr, 0, sizeof hr);
    if (gtext_html_parse(kHtml, sizeof kHtml - 1, nullptr, &w.markup, &hr) != GTEXT_HTML_OK) {
      return 0;
    }
    gtext_html_result_free(&hr);
    gtext_css_markup_adapter_new(w.markup, nullptr, &w.adapter);
    w.mo = gtext_css_markup_match_options(w.adapter);
    GTEXT_CSSOM_Config c = gtext_cssom_config_default();
    c.registry = registry();
    c.allocator = &w.fa.alloc;
    c.element_interface = gtext_css_markup_interface(w.adapter);
    c.match_options = &w.mo;
    c.tree_version = &gtext_cssom_markup_tree_version;
    c.tree_version_user = w.markup;
    if (gtext_cssom_document_new(&c, &w.doc) != GTEXT_CSS_OK) {
      return 0;
    }
    GTEXT_CSSOM_Markup_Options mo = gtext_cssom_markup_options_default();
    gtext_cssom_markup_binding_new(w.doc, gtext_cssom_document_scope(w.doc), w.markup, &mo, &w.binding);
    gtext_cssom_document_observe(w.doc, observer, &w, nullptr);
    {
      const std::vector<Expect> first = model(w);
      for (const Expect & e : first) {
        w.styles.push_back(e.el);
      }
    }
    const GTEXT_CSSOM_Attribute_Binding inline_binding = gtext_cssom_markup_inline_binding();
    int batches = 0;
    GTEXT_CSS_Status st_sync = GTEXT_CSS_OK;
    for (size_t at = 4; at + 6 <= size && ops < 40; at += 6) {
      const uint8_t op = data[at] % 17, a = data[at + 1], b = data[at + 2], t = data[at + 3], d = data[at + 4];
      const bool this_op_faults = armed && ops == fault_op;
      auto arm = [&]() {
        if (this_op_faults) {
          w.fa.arm(fault_n);
        }
      };
      auto disarm = [&]() { w.fa.disarm(); };
      const char * what = "?";
      bool did_sync = false;
      GTEXT_Markup_Node * style = w.styles.empty() ? nullptr : w.styles[a % w.styles.size()];
      switch (op) {
      case 0: {   // set a style's text
        what = "set text";
        if (!style) break;
        while (GTEXT_Markup_Node * ch = gtext_markup_node_first_child(style)) {
          gtext_markup_remove(ch);
          gtext_markup_node_free(ch);
        }
        const char * txt = kTexts[t % (sizeof kTexts / sizeof *kTexts)];
        if (*txt) {
          GTEXT_Markup_Node * tn = nullptr;
          if (gtext_markup_create_text(w.markup, txt, std::strlen(txt), &tn) == GTEXT_MARKUP_OK) {
            gtext_markup_append_child(style, tn);
          }
        }
        break;
      }
      case 1: {   // detach
        what = "detach";
        if (!style || w.detached.count(style)) break;
        gtext_markup_remove(style);
        w.detached.insert(style);
        break;
      }
      case 2: {   // put one back, before an element of the body or at the end of the head
        what = "reinsert";
        if (w.detached.empty()) break;
        auto it = w.detached.begin();
        std::advance(it, a % w.detached.size());
        GTEXT_Markup_Node * parent = (b & 1) ? by_id(w, "d") : gtext_markup_node_parent(by_id(w, "s0"));
        if (!parent) break;
        GTEXT_Markup_Node * ref = nullptr;
        size_t k = t % (gtext_markup_node_child_count(parent) + 1);
        for (GTEXT_Markup_Node * ch = gtext_markup_node_first_child(parent); ch && k; ch = gtext_markup_node_next_sibling(ch), k--) {
          if (k == 1) {
            ref = gtext_markup_node_next_sibling(ch);
          }
        }
        if (gtext_markup_insert_before(parent, *it, ref) == GTEXT_MARKUP_OK) {
          w.detached.erase(it);
        }
        break;
      }
      case 3: {   // media, title or type
        what = "attribute of a style";
        if (!style) break;
        const char * name = (d % 3 == 0) ? "media" : (d % 3 == 1) ? "title" : "type";
        const char * v = (d % 3 == 0) ? kMedia[t % 6] : (d % 3 == 1) ? kTexts[t % 3] : kTypes[t % 6];
        gtext_markup_element_set_attribute(style, nullptr, nullptr, name, v, std::strlen(v));
        if (b & 1) {
          gtext_markup_element_remove_attribute(style, nullptr, name);
        }
        break;
      }
      case 4: {   // a new style element
        what = "new style";
        GTEXT_Markup_Node * e = nullptr;
        if (gtext_markup_create_element(w.markup, GTEXT_MARKUP_NS_HTML, nullptr, "style", &e) != GTEXT_MARKUP_OK) break;
        const char * txt = kTexts[t % (sizeof kTexts / sizeof *kTexts)];
        GTEXT_Markup_Node * tn = nullptr;
        if (*txt && gtext_markup_create_text(w.markup, txt, std::strlen(txt), &tn) == GTEXT_MARKUP_OK) {
          gtext_markup_append_child(e, tn);
        }
        GTEXT_Markup_Node * parent = (b & 1) ? by_id(w, "d") : by_id(w, "p2");
        if (!parent) parent = gtext_markup_node_parent(by_id(w, "s0"));
        gtext_markup_append_child(parent, e);
        w.styles.push_back(e);
        break;
      }
      case 5: {   // an unrelated attribute, or the style attribute of a probe element
        what = "attribute";
        GTEXT_Markup_Node * el = by_id(w, kProbeIds[a % 3]);
        if (!el) break;
        if (b & 1) {
          const char * v = kInline[t % (sizeof kInline / sizeof *kInline)];
          gtext_markup_element_set_attribute(el, nullptr, nullptr, "style", v, std::strlen(v));
        }
        else {
          gtext_markup_element_set_attribute(el, nullptr, nullptr, "data-x", "1", 1);
        }
        break;
      }
      case 6: {   // insertRule on the sheet of a style element
        what = "insertRule";
        if (!style) break;
        GTEXT_CSSOM_Sheet * s = gtext_cssom_markup_sheet_of(w.binding, style);
        if (!s) break;
        const char * txt = kRules[t % (sizeof kRules / sizeof *kRules)];
        size_t n = 0;
        const GTEXT_CSSOM_Rule_List * l = nullptr;
        gtext_cssom_sheet_rules(s, &l);
        n = gtext_cssom_rule_list_length(l);
        arm();
        GTEXT_CSSOM_Outcome o{};
        const GTEXT_CSS_Status st = gtext_cssom_sheet_insert_rule(s, txt, std::strlen(txt), b % (n + 1), nullptr, &o);
        disarm();
        if (st == GTEXT_CSS_OK && o.kind == GTEXT_CSSOM_CHANGED) {
          w.touched.insert(s);
        }
        gtext_cssom_sheet_release(s);
        break;
      }
      case 7: {   // deleteRule
        what = "deleteRule";
        if (!style) break;
        GTEXT_CSSOM_Sheet * s = gtext_cssom_markup_sheet_of(w.binding, style);
        if (!s) break;
        const GTEXT_CSSOM_Rule_List * l = nullptr;
        gtext_cssom_sheet_rules(s, &l);
        const size_t n = gtext_cssom_rule_list_length(l);
        if (n) {
          arm();
          const GTEXT_CSS_Status st = gtext_cssom_sheet_delete_rule(s, b % n, nullptr);
          disarm();
          if (st == GTEXT_CSS_OK) {
            w.touched.insert(s);
          }
        }
        gtext_cssom_sheet_release(s);
        break;
      }
      case 8: {   // setProperty on the style of the first rule
        what = "setProperty";
        if (!style) break;
        GTEXT_CSSOM_Sheet * s = gtext_cssom_markup_sheet_of(w.binding, style);
        if (!s) break;
        const GTEXT_CSSOM_Rule_List * l = nullptr;
        gtext_cssom_sheet_rules(s, &l);
        GTEXT_CSSOM_Rule * r = gtext_cssom_rule_list_at(l, b % (gtext_cssom_rule_list_length(l) + 1));
        if (r && gtext_cssom_rule_kind(r) == GTEXT_CSSOM_RULE_STYLE) {
          GTEXT_CSSOM_Decl * dd = nullptr;
          if (gtext_cssom_rule_style(r, &dd) == GTEXT_CSS_OK) {
            const char * pn = kProps[t % 5];
            const char * pv = kValues[d % 6];
            arm();
            GTEXT_CSSOM_Outcome o{};
            const GTEXT_CSS_Status st = gtext_cssom_decl_set_property(dd, pn, std::strlen(pn), pv, std::strlen(pv), "", 0, &o);
            disarm();
            if (st == GTEXT_CSS_OK && o.kind == GTEXT_CSSOM_CHANGED) {
              w.touched.insert(s);
            }
            gtext_cssom_decl_release(dd);
          }
        }
        gtext_cssom_rule_release(r);
        gtext_cssom_sheet_release(s);
        break;
      }
      case 9: {   // an inline declaration of a probe element, set through the object model
        what = "inline set";
        GTEXT_Markup_Node * el = by_id(w, kProbeIds[a % 3]);
        if (!el) break;
        GTEXT_CSSOM_Decl * dd = nullptr;
        arm();
        const GTEXT_CSS_Status st = gtext_cssom_inline_style(w.doc, el, &inline_binding, &dd);
        if (st == GTEXT_CSS_OK) {
          const char * pn = kProps[t % 3];
          const char * pv = kValues[d % 6];
          gtext_cssom_decl_set_property(dd, pn, std::strlen(pn), pv, std::strlen(pv), "", 0, nullptr);
          disarm();
          if (w.inlines.size() < 8) {
            w.inlines.push_back(dd);
            w.inline_elements.push_back(el);
          }
          else {
            gtext_cssom_decl_release(dd);
          }
        }
        disarm();
        break;
      }
      case 10:
      case 11: {  // sync
        what = "sync";
        const GTEXT_CSSOM_Markup_Counters c0 = [&] { GTEXT_CSSOM_Markup_Counters x; gtext_cssom_markup_binding_counters(w.binding, &x); return x; }();
        const uint64_t g = gtext_markup_document_generation(w.markup);
        arm();
        st_sync = gtext_cssom_markup_sync(w.binding, nullptr);
        disarm();
        did_sync = true;
        GTEXT_CSSOM_Markup_Counters c1;
        gtext_cssom_markup_binding_counters(w.binding, &c1);
        if (w.synced && g == w.synced_generation && st_sync == GTEXT_CSS_OK && c1.walks != c0.walks) {
          fail("a sync with an unchanged generation walked the tree", "", w.input);
        }
        if (st_sync == GTEXT_CSS_OK && gtext_cssom_document_batch_depth(w.doc) == 0) {
          w.synced = true;
          w.synced_generation = g;
        }
        break;
      }
      case 12: {  // the one-element path
        what = "element changed";
        if (!style) break;
        arm();
        gtext_cssom_markup_style_element_changed(w.binding, style, nullptr);
        disarm();
        w.synced = false;
        break;
      }
      case 13: {  // batches
        what = "batch";
        if ((a & 1) && batches > 0) {
          gtext_cssom_document_end_batch(w.doc, nullptr);
          batches--;
        }
        else if (batches < 3) {
          gtext_cssom_document_begin_batch(w.doc);
          batches++;
        }
        break;
      }
      case 14: {  // a constructed sheet, adopted
        what = "adopt";
        if (w.adopted.size() < 4) {
          GTEXT_CSSOM_Sheet * s = nullptr;
          if (gtext_cssom_sheet_new_constructed(w.doc, nullptr, &s) == GTEXT_CSS_OK) {
            const char * txt = kTexts[t % (sizeof kTexts / sizeof *kTexts)];
            gtext_cssom_sheet_replace_sync(s, txt, std::strlen(txt), nullptr);
            w.adopted.push_back(s);
          }
        }
        if (!w.adopted.empty()) {
          GTEXT_CSSOM_Scope * scope = gtext_cssom_document_scope(w.doc);
          arm();
          gtext_cssom_scope_adopted_insert(scope, w.adopted[a % w.adopted.size()], b % (gtext_cssom_scope_adopted_count(scope) + 1), nullptr);
          disarm();
        }
        break;
      }
      case 15: {  // an adopted sheet taken out
        what = "unadopt";
        GTEXT_CSSOM_Scope * scope = gtext_cssom_document_scope(w.doc);
        const size_t n = gtext_cssom_scope_adopted_count(scope);
        if (n) {
          arm();
          gtext_cssom_scope_adopted_remove(scope, b % n, nullptr);
          disarm();
        }
        break;
      }
      default: {  // disabled
        what = "disabled";
        if (!style) break;
        GTEXT_CSSOM_Sheet * s = gtext_cssom_markup_sheet_of(w.binding, style);
        if (s) {
          arm();
          gtext_cssom_sheet_set_disabled(s, (b & 1) != 0, nullptr);
          disarm();
          gtext_cssom_sheet_release(s);
        }
        break;
      }
      }
      ops++;
      check_consistent(w, what);
      // the declaration of an element reads what its attribute says
      for (size_t i = 0; i < w.inlines.size(); i++) {
        std::string attr;
        const bool present = attr_of(w.inline_elements[i], "style", attr);
        GTEXT_CSSOM_Decl * ref = nullptr;
        if (present && gtext_cssom_decl_new(w.doc, &ref) == GTEXT_CSS_OK) {
          w.fa.disarm();
          gtext_cssom_decl_set_css_text(ref, attr.data(), attr.size(), nullptr);
          char *a1 = nullptr, *a2 = nullptr;
          size_t n1 = 0, n2 = 0;
          w.fa.disarm();
          if (gtext_cssom_decl_css_text(ref, &a1, &n1) == GTEXT_CSS_OK && gtext_cssom_decl_css_text(w.inlines[i], &a2, &n2) == GTEXT_CSS_OK) {
            if (std::string(a1, n1) != std::string(a2, n2)) {
              fail("an element's declaration does not read its style attribute", std::string(what) + "\n" + attr + "\n" + std::string(a2, n2), w.input);
            }
          }
          gtext_cssom_buffer_free(&w.fa.alloc, a1);
          gtext_cssom_buffer_free(&w.fa.alloc, a2);
          gtext_cssom_decl_release(ref);
        }
      }
      if (did_sync && st_sync == GTEXT_CSS_OK && batches == 0) {
        check_synced(w, what);
      }
    }
    while (batches > 0) {
      gtext_cssom_document_end_batch(w.doc, nullptr);
      batches--;
    }
    // the next sync, with a healthy allocator, ends where the tree says
    w.fa.disarm();
    if (gtext_cssom_markup_sync(w.binding, nullptr) != GTEXT_CSS_OK) {
      fail("a healthy sync failed", "", w.input);
    }
    check_consistent(w, "the final sync");
    check_synced(w, "the final sync");
    for (GTEXT_CSSOM_Decl * dd : w.inlines) {
      gtext_cssom_decl_release(dd);
    }
    for (GTEXT_CSSOM_Sheet * s : w.adopted) {
      gtext_cssom_sheet_release(s);
    }
    gtext_cssom_markup_binding_free(w.binding);
    gtext_cssom_document_release(w.doc);
    for (GTEXT_Markup_Node * n : w.detached) {
      gtext_markup_node_free(n);
    }
    gtext_css_markup_adapter_free(w.adapter);
    gtext_markup_document_free(w.markup);
    if (!w.fa.live.empty()) {
      fail("memory leaked", std::to_string(w.fa.live.size()) + " blocks", input);
    }
  }
  return 0;
}
