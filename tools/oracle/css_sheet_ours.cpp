// SPDX-License-Identifier: LGPL-3.0-only
//
// Copyright (C) 2026 Corey Pennycuff
//
// This file is part of Ghoti.io Text.
//
// Ghoti.io Text is free software: you can redistribute it and/or modify it
// under the terms of the GNU Lesser General Public License version 3 as
// published by the Free Software Foundation.
//
// Ghoti.io Text is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
// for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

// This library's side of the style sheet and selector differential (tools/oracle/css_sheet_diff.py): the computed style of the elements of an HTML document with
// `<style>` elements and shadow trees, as gtext_css_style_compute() makes it from the sheets and the rule index, and the elements a selector matches, as
// querySelectorAll() answers.
//
//   css-sheet-ours                     documents and questions on standard input, answers on standard output
//
// Input, one tab-separated line each (a field escapes backslash, line feed and tab as \\, \n, \t):
//
//   D TEXT              a document (HTML; in quirks mode when it has no doctype)
//   S HOST TEXT         the shadow tree of the element with the id HOST: the HTML of what `shadowRoot.innerHTML` is set to
//   Z SELECTOR          the elements of the document (not of its shadow trees) that querySelectorAll() returns, in document order
//   Q ID p1,p2,...      the computed style properties of the element with that id (a shadow tree's too) to answer
//   Y                   write `Q` and the elements of the document as `T` lines (INDEX PARENT LOCAL ID CLASSES ATTRIBUTES TEXT, the input of the Servo selectors driver)
//   X                   answer every Z and Q since the last X
//
// Output: for Z `SELECTOR TAB Z TAB OK TAB id id ...` or `... TAB SYNTAX`; for Q `ID TAB property TAB OK TAB value`, `... TAB NOELEMENT`, `... TAB UNSUPPORTED TAB name`,
// `... TAB ERR TAB status`; then a line `X`.
//
// The tree is the document the HTML parser makes, plus a shadow tree for each S (its elements are the children of a `<body>` of a document of its own, which is not in the
// tree). A light child of a host is assigned to the first slot of the shadow tree whose `name` is the child's `slot` attribute (the default slot is the one with no
// name), and the flat tree is the one CSS Scoping 1 defines, so a slotted element inherits from its slot. A part is an element with the name in its `part` attribute.
// Conditional rules are decided by a table of the conditions the population writes.

#include <cctype>
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

#include "../../tests/css_svg_registry.h"
#include "css_resolved.h"

namespace {

std::string unesc(const std::string & s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      const char c = s[++i];
      out += c == 'n' ? '\n' : c == 't' ? '\t' : c;
    }
    else {
      out += s[i];
    }
  }
  return out;
}

std::string esc(const std::string & s) {
  std::string out;
  for (char c : s) {
    out += c == '\\' ? "\\\\" : c == '\n' ? "\\n" : c == '\t' ? "\\t" : std::string(1, c);
  }
  return out;
}

std::vector<std::string> split(const std::string & s, char sep) {
  std::vector<std::string> out;
  size_t pos = 0;
  for (;;) {
    const size_t at = s.find(sep, pos);
    out.push_back(s.substr(pos, at == std::string::npos ? std::string::npos : at - pos));
    if (at == std::string::npos) {
      break;
    }
    pos = at + 1;
  }
  return out;
}

typedef const GTEXT_Markup_Node Node;

/// A shadow tree: its host, the document its elements are parsed in, and its number (the context of its sheets).
struct Shadow {
  Node * host = nullptr;
  GTEXT_Markup_Document * doc = nullptr;
  Node * body = nullptr;
  int number = 0;
};

struct World {
  GTEXT_Markup_Document * doc = nullptr;
  std::vector<Shadow> shadows;
  std::map<const GTEXT_Markup_Document *, int> shadow_of_doc; // index into shadows
  std::map<const Node *, int> shadow_of_host;
  GTEXT_CSS_Markup_Adapter * adapter = nullptr;
  GTEXT_CSS_Element_Interface iface;
  std::vector<int> scope_tokens;

  static World *& current() {
    static World * w = nullptr;
    return w;
  }

  int scope_of(const Node * e) const {
    auto it = shadow_of_doc.find(gtext_markup_node_document(e));
    return it == shadow_of_doc.end() ? -1 : it->second;
  }

  const void * token(int shadow_index) const {
    return &scope_tokens[shadow_index + 1];
  }
};

const GTEXT_CSS_Element_Interface * g_base = nullptr;

World & W(void * user) {
  return *static_cast<World *>(user);
}

bool is_name(const Node * e, const char * name) {
  const char * l = gtext_markup_element_local_name(e);
  return l && std::strcmp(l, name) == 0;
}

std::string attr_of(const Node * e, const char * name) {
  GTEXT_Markup_Attribute a;
  if (gtext_markup_element_get_attribute(e, nullptr, name, &a)) {
    return std::string(a.value, a.value_length);
  }
  return std::string();
}

bool has_attr(const Node * e, const char * name) {
  GTEXT_Markup_Attribute a;
  return gtext_markup_element_get_attribute(e, nullptr, name, &a);
}

// ---- the element interface: the adapter's, with the shadow trees ----------------------------------------------------------

Node * as_node(GTEXT_CSS_Element_Handle h) {
  return static_cast<Node *>(h);
}

GTEXT_CSS_Element_Handle cb_parent(void * u, GTEXT_CSS_Element_Handle h) {
  World & w = W(u);
  GTEXT_CSS_Element_Handle p = g_base->parent(g_base->user, h);
  if (p && w.scope_of(as_node(h)) >= 0 && is_name(as_node(p), "body")) {
    return nullptr; // the top of a shadow tree
  }
  return p;
}

GTEXT_CSS_Element_Handle cb_containing_host(void * u, GTEXT_CSS_Element_Handle h) {
  World & w = W(u);
  const int s = w.scope_of(as_node(h));
  return s < 0 ? nullptr : w.shadows[s].host;
}

const void * cb_tree_scope(void * u, GTEXT_CSS_Element_Handle h) {
  World & w = W(u);
  const int s = w.scope_of(as_node(h));
  return w.token(s);
}

const void * cb_shadow_scope(void * u, GTEXT_CSS_Element_Handle h) {
  World & w = W(u);
  auto it = w.shadow_of_host.find(as_node(h));
  return it == w.shadow_of_host.end() ? nullptr : w.token(it->second);
}

GTEXT_CSS_Element_Handle find_slot(World & w, int shadow, const std::string & name) {
  Node * root = w.shadows[shadow].body;
  for (Node * n = root; n; n = gtext_markup_node_next_in_order(root, n, false)) {
    if (gtext_markup_node_kind(n) == GTEXT_MARKUP_KIND_ELEMENT && is_name(n, "slot") && attr_of(n, "name") == name) {
      return n;
    }
  }
  return nullptr;
}

GTEXT_CSS_Element_Handle cb_assigned_slot(void * u, GTEXT_CSS_Element_Handle h) {
  World & w = W(u);
  Node * e = as_node(h);
  Node * p = gtext_markup_node_parent(e);
  if (!p || gtext_markup_node_kind(p) != GTEXT_MARKUP_KIND_ELEMENT) {
    return nullptr;
  }
  auto it = w.shadow_of_host.find(p);
  if (it == w.shadow_of_host.end()) {
    return nullptr; // not a child of a host (a shadow tree's top elements are children of its body)
  }
  return find_slot(w, it->second, attr_of(e, "slot"));
}

bool cb_has_part(void * u, GTEXT_CSS_Element_Handle h, GTEXT_CSS_Element_Handle host, const GTEXT_CSS_String * name) {
  World & w = W(u);
  const int s = w.scope_of(as_node(h));
  if (s < 0 || w.shadows[s].host != host) {
    return false;
  }
  const std::string parts = attr_of(as_node(h), "part");
  size_t i = 0;
  while (i < parts.size()) {
    while (i < parts.size() && std::isspace(static_cast<unsigned char>(parts[i]))) {
      i++;
    }
    size_t j = i;
    while (j < parts.size() && !std::isspace(static_cast<unsigned char>(parts[j]))) {
      j++;
    }
    if (j > i && parts.compare(i, j - i, name->data, name->length) == 0 && j - i == name->length) {
      return true;
    }
    i = j;
  }
  return false;
}

bool cb_custom_state(void *, GTEXT_CSS_Element_Handle, const GTEXT_CSS_String *) {
  return false;
}

/// An interface that is the adapter's for everything but the shadow trees. The adapter's table is read once; the callbacks that do not change are the adapter's.
void build_interface(World & w) {
  g_base = gtext_css_markup_interface(w.adapter);
  w.iface = *g_base;
  w.iface.user = &w;
  // the adapter's own callbacks take the adapter as their user: wrap each one that is not overridden
  w.iface.local_name = [](void *, GTEXT_CSS_Element_Handle h, GTEXT_CSS_String * o) { return g_base->local_name(g_base->user, h, o); };
  w.iface.namespace_uri = [](void *, GTEXT_CSS_Element_Handle h, GTEXT_CSS_String * o) { return g_base->namespace_uri(g_base->user, h, o); };
  w.iface.id = [](void *, GTEXT_CSS_Element_Handle h, GTEXT_CSS_String * o) { return g_base->id(g_base->user, h, o); };
  w.iface.class_count = [](void *, GTEXT_CSS_Element_Handle h) { return g_base->class_count(g_base->user, h); };
  w.iface.class_at = [](void *, GTEXT_CSS_Element_Handle h, size_t i, GTEXT_CSS_String * o) { return g_base->class_at(g_base->user, h, i, o); };
  w.iface.attribute = [](void *, GTEXT_CSS_Element_Handle h, const GTEXT_CSS_String * ns, const GTEXT_CSS_String * n, GTEXT_CSS_String * o) {
    return g_base->attribute(g_base->user, h, ns, n, o);
  };
  if (g_base->previous_sibling) {
    w.iface.previous_sibling = [](void *, GTEXT_CSS_Element_Handle h) { return g_base->previous_sibling(g_base->user, h); };
  }
  else {
    w.iface.previous_sibling = nullptr;
  }
  if (g_base->next_sibling) {
    w.iface.next_sibling = [](void *, GTEXT_CSS_Element_Handle h) { return g_base->next_sibling(g_base->user, h); };
  }
  else {
    w.iface.next_sibling = nullptr;
  }
  if (g_base->sibling_index) {
    w.iface.sibling_index = [](void *, GTEXT_CSS_Element_Handle h) { return g_base->sibling_index(g_base->user, h); };
  }
  else {
    w.iface.sibling_index = nullptr;
  }
  if (g_base->sibling_count) {
    w.iface.sibling_count = [](void *, GTEXT_CSS_Element_Handle h) { return g_base->sibling_count(g_base->user, h); };
  }
  else {
    w.iface.sibling_count = nullptr;
  }
  if (g_base->compare_order) {
    w.iface.compare_order = [](void *, GTEXT_CSS_Element_Handle a, GTEXT_CSS_Element_Handle b) { return g_base->compare_order(g_base->user, a, b); };
  }
  else {
    w.iface.compare_order = nullptr;
  }
  if (g_base->state) {
    w.iface.state = [](void *, GTEXT_CSS_Element_Handle h) { return g_base->state(g_base->user, h); };
  }
  else {
    w.iface.state = nullptr;
  }
  if (g_base->lang) {
    w.iface.lang = [](void *, GTEXT_CSS_Element_Handle h, GTEXT_CSS_String * o) { return g_base->lang(g_base->user, h, o); };
  }
  else {
    w.iface.lang = nullptr;
  }
  if (g_base->direction) {
    w.iface.direction = [](void *, GTEXT_CSS_Element_Handle h) { return g_base->direction(g_base->user, h); };
  }
  else {
    w.iface.direction = nullptr;
  }
  if (g_base->first_child) {
    w.iface.first_child = [](void *, GTEXT_CSS_Element_Handle h) { return g_base->first_child(g_base->user, h); };
  }
  else {
    w.iface.first_child = nullptr;
  }
  if (g_base->last_child) {
    w.iface.last_child = [](void *, GTEXT_CSS_Element_Handle h) { return g_base->last_child(g_base->user, h); };
  }
  else {
    w.iface.last_child = nullptr;
  }
  if (g_base->child_content) {
    w.iface.child_content = [](void *, GTEXT_CSS_Element_Handle h) { return g_base->child_content(g_base->user, h); };
  }
  else {
    w.iface.child_content = nullptr;
  }
  w.iface.inheritance_parent = nullptr;
  w.iface.parent = cb_parent;
  w.iface.tree_scope = cb_tree_scope;
  w.iface.containing_host = cb_containing_host;
  w.iface.shadow_scope = cb_shadow_scope;
  w.iface.assigned_slot = cb_assigned_slot;
  w.iface.has_part = cb_has_part;
  w.iface.custom_state = cb_custom_state;
}

// ---- the sheets ------------------------------------------------------------------------------------------------------------

/// The conditions of the population are evaluated by the library, against the environment the browsers run the documents in (a 1280 by 800 screen at 1 dppx, the default of
/// the media environment): `@media`, `@supports` and the rest are data of the sheets, and the index is built for this one conditions object.
GTEXT_CSS_Conditions * g_conditions = nullptr;

std::string text_of(const Node * style) {
  std::string out;
  for (Node * c = gtext_markup_node_first_child(style); c; c = gtext_markup_node_next_sibling(c)) {
    size_t dn = 0;
    const char * d = gtext_markup_node_data(c, &dn);
    if (d && gtext_markup_node_kind(c) == GTEXT_MARKUP_KIND_TEXT) {
      out.append(d, dn);
    }
  }
  return out;
}

struct Answer {
  GTEXT_CSS_Status status = GTEXT_CSS_OK;
  std::string name;
  GTEXT_CSS_Computed_Style * style = nullptr;
};

struct Sheets {
  std::vector<GTEXT_CSS_Sheet *> sheets;
  GTEXT_CSS_Rule_Index * index = nullptr;
  ~Sheets() {
    gtext_css_rule_index_free(index);
    for (auto * s : sheets) {
      gtext_css_sheet_free(s);
    }
  }
};

bool compile_scope(Sheets & out, GTEXT_CSS_Rule_Index * ix, Node * root, const GTEXT_CSS_Registry * reg, int shadow_scope_number, const void * token, const void * parent_scope) {
  bool ok = true;
  for (Node * n = root; n; n = gtext_markup_node_next_in_order(root, n, false)) {
    if (gtext_markup_node_kind(n) != GTEXT_MARKUP_KIND_ELEMENT || !is_name(n, "style")) {
      continue;
    }
    const std::string css = text_of(n);
    GTEXT_CSS_Tree * tree = nullptr;
    GTEXT_CSS_Result pr;
    std::memset(&pr, 0, sizeof pr);
    if (gtext_css_parse_stylesheet(css.data(), css.size(), nullptr, &tree, &pr) != GTEXT_CSS_OK) {
      gtext_css_result_free(&pr);
      return false;
    }
    GTEXT_CSS_Sheet_Options so = gtext_css_sheet_options_default();
    GTEXT_CSS_Sheet * sheet = nullptr;
    GTEXT_CSS_Sheet_Result sr;
    const GTEXT_CSS_Status st = gtext_css_sheet_compile(tree, reg, &so, &sheet, &sr);
    gtext_css_tree_free(tree);
    gtext_css_result_free(&pr);
    if (st != GTEXT_CSS_OK) {
      std::fprintf(stderr, "css-sheet-ours: the sheet did not compile: %s %s\n", gtext_css_status_string(st), sr.name);
      return false;
    }
    out.sheets.push_back(sheet);
    GTEXT_CSS_Sheet_Registration r;
    std::memset(&r, 0, sizeof r);
    r.origin = GTEXT_CSS_ORIGIN_AUTHOR;
    r.shadow = token != nullptr;
    r.tree_scope = token;
    r.context = static_cast<uint32_t>(shadow_scope_number);
    r.parent_scope = parent_scope;
    GTEXT_CSS_Rule_Index_Result ir;
    if (gtext_css_rule_index_add_sheet(ix, sheet, &r, &ir) != GTEXT_CSS_OK) {
      std::fprintf(stderr, "css-sheet-ours: add_sheet: %s %s\n", ir.message, ir.name);
      ok = false;
    }
  }
  return ok;
}

// ---- a document --------------------------------------------------------------------------------------------------------------

struct Question {
  std::string id;
  std::vector<std::string> properties;
};

GTEXT_Markup_Document * parse_html(const std::string & text) {
  GTEXT_Markup_Document * d = nullptr;
  GTEXT_HTML_Result r;
  std::memset(&r, 0, sizeof r);
  const GTEXT_HTML_Status st = gtext_html_parse(text.data(), text.size(), nullptr, &d, &r);
  gtext_html_result_free(&r);
  return st == GTEXT_HTML_OK ? d : nullptr;
}

Node * find_by_id(Node * root, const std::string & id) {
  for (Node * n = root; n; n = gtext_markup_node_next_in_order(root, n, false)) {
    if (gtext_markup_node_kind(n) == GTEXT_MARKUP_KIND_ELEMENT && has_attr(n, "id") && attr_of(n, "id") == id) {
      return n;
    }
  }
  return nullptr;
}

Node * find_element(World & w, const std::string & id) {
  if (Node * n = find_by_id(gtext_markup_document_root(w.doc), id)) {
    return n;
  }
  for (Shadow & s : w.shadows) {
    if (Node * n = find_by_id(s.body, id)) {
      return n;
    }
  }
  return nullptr;
}

/// The parent in the flat tree: the slot an element is assigned to, the host of a shadow tree's top, else the parent.
Node * flat_parent(World & w, Node * e) {
  if (GTEXT_CSS_Element_Handle s = cb_assigned_slot(&w, e)) {
    return as_node(s);
  }
  if (GTEXT_CSS_Element_Handle p = cb_parent(&w, e)) {
    return as_node(p);
  }
  return static_cast<Node *>(cb_containing_host(&w, e));
}

/// The elements of the document in document order as `T INDEX PARENT LOCAL ID CLASSES ATTRIBUTES TEXT` lines: the tree the Servo `selectors` driver (containers/css) matches.
void dump_tree(World & w) {
  std::map<const Node *, int> index;
  Node * root = gtext_markup_document_root(w.doc);
  int next = 0;
  std::printf("Q\t%d\n", gtext_markup_document_mode(w.doc) == GTEXT_MARKUP_MODE_QUIRKS ? 1 : 0);
  for (Node * n = root; n; n = gtext_markup_node_next_in_order(root, n, false)) {
    if (gtext_markup_node_kind(n) != GTEXT_MARKUP_KIND_ELEMENT) {
      continue;
    }
    Node * p = gtext_markup_node_parent(n);
    const int parent = p && index.count(p) ? index[p] : -1;
    index[n] = next;
    std::string attrs;
    const size_t na = gtext_markup_element_attribute_count(n);
    for (size_t i = 0; i < na; i++) {
      GTEXT_Markup_Attribute a;
      if (gtext_markup_element_attribute_at(n, i, &a) == GTEXT_MARKUP_OK) {
        attrs += (attrs.empty() ? "" : "\x1f") + std::string(a.local) + "\x1e" + std::string(a.value, a.value_length);
      }
    }
    bool text = false;
    for (Node * c = gtext_markup_node_first_child(n); c; c = gtext_markup_node_next_sibling(c)) {
      size_t dn = 0;
      if (gtext_markup_node_kind(c) == GTEXT_MARKUP_KIND_TEXT && gtext_markup_node_data(c, &dn) && dn) {
        text = true;
      }
    }
    std::printf("T\t%d\t%d\t%s\t%s\t%s\t%s\t%d\n", next, parent, esc(gtext_markup_element_local_name(n)).c_str(), esc(attr_of(n, "id")).c_str(),
        esc(attr_of(n, "class")).c_str(), esc(attrs).c_str(), text ? 1 : 0);
    next++;
  }
}

void answer(const std::string & text, const std::vector<std::pair<std::string, std::string>> & shadows, const std::vector<std::string> & selectors,
    const std::vector<Question> & questions, bool tree_only) {
  World w;
  World::current() = &w;
  w.doc = parse_html(text);
  if (!w.doc) {
    std::fprintf(stderr, "css-sheet-ours: the document did not parse\n");
    std::exit(2);
  }
  w.scope_tokens.assign(shadows.size() + 2, 0);
  GTEXT_CSS_Markup_Options mopt;
  std::memset(&mopt, 0, sizeof mopt);
  gtext_css_markup_adapter_new(w.doc, &mopt, &w.adapter);
  Node * root_el = gtext_markup_document_element(w.doc);
  for (const auto & s : shadows) {
    // the host is in the document or in a shadow tree made before this one (a shadow tree inside a shadow tree)
    Node * host = find_by_id(gtext_markup_document_root(w.doc), s.first);
    for (size_t k = 0; !host && k < w.shadows.size(); k++) {
      host = find_by_id(w.shadows[k].body, s.first);
    }
    Shadow sh;
    sh.host = host;
    sh.doc = parse_html("<!doctype html><html><head></head><body>" + s.second + "</body></html>");
    if (!host || !sh.doc) {
      std::fprintf(stderr, "css-sheet-ours: a shadow tree has no host or does not parse\n");
      std::exit(2);
    }
    for (Node * n = gtext_markup_document_root(sh.doc); n; n = gtext_markup_node_next_in_order(gtext_markup_document_root(sh.doc), n, false)) {
      if (gtext_markup_node_kind(n) == GTEXT_MARKUP_KIND_ELEMENT && is_name(n, "body")) {
        sh.body = n;
        break;
      }
    }
    sh.number = static_cast<int>(w.shadows.size()) + 1;
    w.shadow_of_doc[sh.doc] = static_cast<int>(w.shadows.size());
    w.shadow_of_host[host] = static_cast<int>(w.shadows.size());
    w.shadows.push_back(sh);
  }
  if (tree_only) {
    dump_tree(w);
    gtext_css_markup_adapter_free(w.adapter);
    for (Shadow & sh : w.shadows) {
      gtext_markup_document_free(sh.doc);
    }
    gtext_markup_document_free(w.doc);
    World::current() = nullptr;
    return;
  }
  build_interface(w);
  const GTEXT_CSS_Registry * reg = svgreg::shared();

  Sheets sheets;
  GTEXT_CSS_Rule_Index_Options io = gtext_css_rule_index_options_default();
  io.html_document = true;
  io.quirks = gtext_markup_document_mode(w.doc) == GTEXT_MARKUP_MODE_QUIRKS;
  GTEXT_CSS_Conditions_Options cndo = gtext_css_conditions_options_default();
  cndo.element_interface = &w.iface;
  if (gtext_css_conditions_new(reg, &cndo, &g_conditions) != GTEXT_CSS_OK) {
    std::fprintf(stderr, "css-sheet-ours: the conditions object could not be made\n");
    std::exit(2);
  }
  io.conditions = g_conditions;
  gtext_css_rule_index_new(reg, &io, &sheets.index);
  gtext_css_conditions_free(g_conditions); /* the index holds its own owner */
  bool ok = compile_scope(sheets, sheets.index, gtext_markup_document_root(w.doc), reg, 0, nullptr, nullptr);
  for (size_t i = 0; i < w.shadows.size() && ok; i++) {
    ok = compile_scope(sheets, sheets.index, w.shadows[i].body, reg, w.shadows[i].number, w.token(static_cast<int>(i)), nullptr);
  }
  if (ok) {
    GTEXT_CSS_Rule_Index_Result ir;
    ok = gtext_css_rule_index_freeze(sheets.index, &ir) == GTEXT_CSS_OK;
    if (!ok) {
      std::fprintf(stderr, "css-sheet-ours: freeze: %s %s\n", ir.message, ir.name);
    }
  }
  if (!ok) {
    std::exit(2);
  }

  GTEXT_CSS_Selector_Match_Options mo = gtext_css_markup_match_options(w.adapter);
  mo.root = root_el;
  mo.root_given = true;
  GTEXT_CSS_Selector_Match_Context * mctx = nullptr;
  gtext_css_selector_match_context_new(&w.iface, &mo, &mctx);

  // the selectors: querySelectorAll() on the document
  for (const std::string & sel : selectors) {
    GTEXT_CSS_Selector_List * list = nullptr;
    GTEXT_CSS_Selector_Result sr;
    std::memset(&sr, 0, sizeof sr);
    GTEXT_CSS_Selector_Options so = gtext_css_selector_options_default();
    const GTEXT_CSS_Status ps = gtext_css_selector_parse(sel.data(), sel.size(), &so, &list, &sr);
    gtext_css_selector_result_free(&sr);
    if (ps != GTEXT_CSS_OK || !list) {
      std::printf("%s\tZ\tSYNTAX\n", esc(sel).c_str());
      continue;
    }
    std::string ids;
    std::string failure;
    Node * droot = gtext_markup_document_root(w.doc);
    gtext_css_selector_match_set_scope(mctx, nullptr, nullptr);
    for (Node * n = droot; n; n = gtext_markup_node_next_in_order(droot, n, false)) {
      if (gtext_markup_node_kind(n) != GTEXT_MARKUP_KIND_ELEMENT) {
        continue;
      }
      GTEXT_CSS_Selector_Match_Result mr;
      const GTEXT_CSS_Status ms = gtext_css_selector_match(list, mctx, n, nullptr, &mr);
      if (ms != GTEXT_CSS_OK) {
        failure = gtext_css_status_string(ms);
        break;
      }
      if (mr.matched) {
        ids += (ids.empty() ? "" : " ") + (has_attr(n, "id") ? attr_of(n, "id") : std::string("?"));
      }
    }
    if (!failure.empty()) {
      std::printf("%s\tZ\tERR\t%s\n", esc(sel).c_str(), failure.c_str());
    }
    else {
      std::printf("%s\tZ\tOK\t%s\n", esc(sel).c_str(), esc(ids).c_str());
    }
    gtext_css_selector_list_free(list);
  }

  // the styles
  GTEXT_CSS_Environment env = gtext_css_environment_default();
  GTEXT_CSS_Compute_Options co = gtext_css_compute_options_default();
  co.elements = &w.iface;
  co.max_element_depth = 100000;
  GTEXT_CSS_Compute_Context * cctx = nullptr;
  GTEXT_CSS_Compute_Result cr;
  if (gtext_css_compute_context_new(reg, &env, &co, &cctx, &cr) != GTEXT_CSS_OK) {
    std::fprintf(stderr, "css-sheet-ours: no context: %s\n", cr.name);
    std::exit(2);
  }
  std::map<Node *, Answer> computed;
  std::vector<Node *> chain;
  auto compute = [&](Node * e) -> Answer & {
    auto it = computed.find(e);
    if (it != computed.end()) {
      return it->second;
    }
    chain.clear();
    for (Node * x = e; x && !computed.count(x); x = flat_parent(w, x)) {
      chain.push_back(x);
    }
    for (size_t k = chain.size(); k-- > 0;) {
      Node * x = chain[k];
      Answer & a = computed[x];
      Node * fp = flat_parent(w, x);
      const Answer * parent = fp ? &computed[fp] : nullptr;
      if (parent && parent->status != GTEXT_CSS_OK) {
        a.status = GTEXT_CSS_E_UNSUPPORTED;
        a.name = parent->name;
        continue;
      }
      GTEXT_CSS_Style_Options sty = gtext_css_style_options_default();
      sty.sources = 0;
      sty.rules = sheets.index;
      sty.match = mctx;
      GTEXT_CSS_Compute_Result res;
      GTEXT_CSS_Computed_Style * s = nullptr;
      gtext_css_selector_match_set_scope(mctx, nullptr, nullptr);
      a.status = gtext_css_style_compute(cctx, x, parent ? parent->style : nullptr, nullptr, 0, &sty, &s, &res);
      if (a.status != GTEXT_CSS_OK) {
        a.name = res.name;
      }
      a.style = s;
    }
    return computed[e];
  };
  for (const Question & q : questions) {
    Node * e = find_element(w, q.id);
    for (const std::string & p : q.properties) {
      if (!e) {
        std::printf("%s\t%s\tNOELEMENT\n", esc(q.id).c_str(), esc(p).c_str());
        continue;
      }
      const Answer & a = compute(e);
      if (a.status != GTEXT_CSS_OK) {
        std::printf("%s\t%s\t%s\t%s\n", esc(q.id).c_str(), esc(p).c_str(), a.status == GTEXT_CSS_E_UNSUPPORTED ? "UNSUPPORTED" : "ERR",
            esc(a.status == GTEXT_CSS_E_UNSUPPORTED ? a.name : gtext_css_status_string(a.status)).c_str());
        continue;
      }
      const GTEXT_CSS_Property_Id id = gtext_css_registry_find_property(reg, p.data(), p.size());
      std::string text;
      GTEXT_CSS_Compute_Result gr;
      resolved_value::dark_scheme() = false;
      const GTEXT_CSS_Status st = id ? resolved_value::read(reg, a.style, id, &text, &gr) : GTEXT_CSS_E_INVALID;
      if (st != GTEXT_CSS_OK) {
        std::printf("%s\t%s\tERR\t%s\n", esc(q.id).c_str(), esc(p).c_str(), gtext_css_status_string(st));
      }
      else {
        std::printf("%s\t%s\tOK\t%s\n", esc(q.id).c_str(), esc(p).c_str(), esc(text).c_str());
      }
    }
  }
  for (auto & kv : computed) {
    gtext_css_computed_free(kv.second.style);
  }
  gtext_css_compute_context_free(cctx);
  gtext_css_selector_match_context_free(mctx);
  gtext_css_markup_adapter_free(w.adapter);
  for (Shadow & s : w.shadows) {
    gtext_markup_document_free(s.doc);
  }
  gtext_markup_document_free(w.doc);
  World::current() = nullptr;
}

}  // namespace

int main() {
  std::string text;
  std::vector<std::pair<std::string, std::string>> shadows;
  std::vector<std::string> selectors;
  std::vector<Question> questions;
  char * line = nullptr;
  size_t cap = 0;
  ssize_t len;
  while ((len = getline(&line, &cap, stdin)) >= 0) {
    std::string l(line, static_cast<size_t>(len));
    if (!l.empty() && l.back() == '\n') {
      l.pop_back();
    }
    const std::vector<std::string> f = split(l, '\t');
    if (f[0] == "D" && f.size() == 2) {
      text = unesc(f[1]);
      shadows.clear();
      selectors.clear();
      questions.clear();
    }
    else if (f[0] == "S" && f.size() == 3) {
      shadows.emplace_back(unesc(f[1]), unesc(f[2]));
    }
    else if (f[0] == "Z" && f.size() == 2) {
      selectors.push_back(unesc(f[1]));
    }
    else if (f[0] == "Q" && f.size() == 3) {
      Question q;
      q.id = unesc(f[1]);
      q.properties = split(unesc(f[2]), ',');
      questions.push_back(q);
    }
    else if (f[0] == "Y") {
      answer(text, {}, {}, {}, true);
    }
    else if (f[0] == "X") {
      answer(text, shadows, selectors, questions, false);
      std::printf("X\n");
      std::fflush(stdout);
      shadows.clear();
      selectors.clear();
      questions.clear();
    }
  }
  std::free(line);
  return 0;
}
