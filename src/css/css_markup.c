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
 * The element interface over a Markup tree (css_markup.h).
 */

#include <string.h>

#include <ghoti.io/unicode/char.h>

#include <ghoti.io/text/css/css_markup.h>

#include "css_internal.h"

#define MAX_CACHED_CLASSES 32

typedef const GTEXT_Markup_Node Node;

struct GTEXT_CSS_Markup_Adapter {
  GTEXT_Allocator alloc;
  Node * doc_root;
  const GTEXT_Markup_Document * doc;
  GTEXT_CSS_Element_Interface iface;
  GTEXT_CSS_Markup_Options opt;
  bool html;
  bool quirks;
  /* the classes of the element asked about last */
  Node * cnode;
  size_t ccount;
  size_t coff[MAX_CACHED_CLASSES];
  size_t clen[MAX_CACHED_CLASSES];
  size_t clen_total;
  const char * cvalue;
};

#define NS_HTML GTEXT_MARKUP_NS_HTML

static bool is_el(Node * n) {
  return n && gtext_markup_node_kind(n) == GTEXT_MARKUP_KIND_ELEMENT;
}

static bool in_ns(Node * el, const char * ns) {
  const char * e = gtext_markup_element_namespace(el);
  return e && strcmp(e, ns) == 0;
}

/// An HTML element with this local name (HTML names are lower case in the tree).
static bool html_is(Node * el, const char * local) {
  return in_ns(el, NS_HTML) && strcmp(gtext_markup_element_local_name(el), local) == 0;
}

static unsigned char css_sel_lower(unsigned char c) {
  return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c;
}

static bool lc_eq(const char * a, const char * b) {
  for (; *a && *b; a++, b++) {
    if (css_sel_lower((unsigned char)*a) != css_sel_lower((unsigned char)*b)) {
      return false;
    }
  }
  return *a == *b;
}

static bool attr(Node * el, const char * name, GTEXT_Markup_Attribute * out) {
  return gtext_markup_element_get_attribute(el, NULL, name, out);
}

static bool has_attr(Node * el, const char * name) {
  return attr(el, name, NULL);
}

/* ---- the tree ------------------------------------------------------------------------------------------------- */

static GTEXT_CSS_Element_Handle cb_parent(void * user, GTEXT_CSS_Element_Handle h) {
  (void)user;
  Node * p = gtext_markup_node_parent(h);
  return is_el(p) ? p : NULL;
}

static bool cb_local_name(void * user, GTEXT_CSS_Element_Handle h, GTEXT_CSS_String * out) {
  (void)user;
  const char * s = gtext_markup_element_local_name(h);
  if (!s) {
    return false;
  }
  out->data = s;
  out->length = strlen(s);
  return true;
}

static bool cb_namespace_uri(void * user, GTEXT_CSS_Element_Handle h, GTEXT_CSS_String * out) {
  (void)user;
  const char * s = gtext_markup_element_namespace(h);
  if (!s) {
    return false;
  }
  out->data = s;
  out->length = strlen(s);
  return true;
}

static bool cb_id(void * user, GTEXT_CSS_Element_Handle h, GTEXT_CSS_String * out) {
  (void)user;
  GTEXT_Markup_Attribute a;
  if (!attr(h, "id", &a)) {
    return false;
  }
  out->data = a.value;
  out->length = a.value_length;
  return true;
}

static bool class_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

/// Tokenize the class attribute of an element, keeping the offsets of the first MAX_CACHED_CLASSES of the last element asked about (no allocation: an interface callback has no
/// way to say it ran out of memory). Past them a class is found by reading the attribute again.
static bool classes_of(struct GTEXT_CSS_Markup_Adapter * a, Node * el) {
  if (a->cnode == el && a->cvalue) {
    return true;
  }
  a->cnode = el;
  a->ccount = 0;
  a->cvalue = NULL;
  a->clen_total = 0;
  GTEXT_Markup_Attribute at;
  if (!attr(el, "class", &at)) {
    a->cvalue = "";
    return true;
  }
  a->cvalue = at.value;
  a->clen_total = at.value_length;
  size_t i = 0;
  while (i < at.value_length) {
    while (i < at.value_length && class_space(at.value[i])) {
      i++;
    }
    size_t j = i;
    while (j < at.value_length && !class_space(at.value[j])) {
      j++;
    }
    if (j > i) {
      if (a->ccount < MAX_CACHED_CLASSES) {
        a->coff[a->ccount] = i;
        a->clen[a->ccount] = j - i;
      }
      a->ccount++;
    }
    i = j;
  }
  return true;
}

static size_t cb_class_count(void * user, GTEXT_CSS_Element_Handle h) {
  struct GTEXT_CSS_Markup_Adapter * a = user;
  return classes_of(a, h) ? a->ccount : 0;
}

static bool cb_class_at(void * user, GTEXT_CSS_Element_Handle h, size_t index, GTEXT_CSS_String * out) {
  struct GTEXT_CSS_Markup_Adapter * a = user;
  if (!classes_of(a, h) || index >= a->ccount) {
    return false;
  }
  if (index < MAX_CACHED_CLASSES) {
    out->data = a->cvalue + a->coff[index];
    out->length = a->clen[index];
    return true;
  }
  size_t i = 0, k = 0;
  while (i < a->clen_total) {
    while (i < a->clen_total && class_space(a->cvalue[i])) {
      i++;
    }
    size_t j = i;
    while (j < a->clen_total && !class_space(a->cvalue[j])) {
      j++;
    }
    if (j > i) {
      if (k == index) {
        out->data = a->cvalue + i;
        out->length = j - i;
        return true;
      }
      k++;
    }
    i = j;
  }
  return false;
}

static bool cb_attribute(void * user, GTEXT_CSS_Element_Handle h, const GTEXT_CSS_String * ns, const GTEXT_CSS_String * name, GTEXT_CSS_String * out) {
  (void)user;
  GTEXT_Markup_Attribute a;
  if (ns == gtext_css_namespace_any()) {
    const size_t n = gtext_markup_element_attribute_count(h);
    for (size_t i = 0; i < n; i++) {
      if (gtext_markup_element_attribute_at(h, i, &a) == GTEXT_MARKUP_OK && strlen(a.local) == name->length && memcmp(a.local, name->data, name->length) == 0) {
        out->data = a.value;
        out->length = a.value_length;
        return true;
      }
    }
    return false;
  }
  if (!gtext_markup_element_get_attribute(h, ns ? ns->data : NULL, name->data, &a)) {
    return false;
  }
  out->data = a.value;
  out->length = a.value_length;
  return true;
}

static GTEXT_CSS_Element_Handle element_from(Node * n, bool forward) {
  while (n && !is_el(n)) {
    n = forward ? gtext_markup_node_next_sibling(n) : gtext_markup_node_previous_sibling(n);
  }
  return n;
}

static GTEXT_CSS_Element_Handle cb_previous_sibling(void * user, GTEXT_CSS_Element_Handle h) {
  (void)user;
  return element_from(gtext_markup_node_previous_sibling(h), false);
}

static GTEXT_CSS_Element_Handle cb_next_sibling(void * user, GTEXT_CSS_Element_Handle h) {
  (void)user;
  return element_from(gtext_markup_node_next_sibling(h), true);
}

static GTEXT_CSS_Element_Handle cb_first_child(void * user, GTEXT_CSS_Element_Handle h) {
  (void)user;
  return element_from(gtext_markup_node_first_child(h), true);
}

static GTEXT_CSS_Element_Handle cb_last_child(void * user, GTEXT_CSS_Element_Handle h) {
  (void)user;
  return element_from(gtext_markup_node_last_child(h), false);
}

static uint32_t cb_child_content(void * user, GTEXT_CSS_Element_Handle h) {
  (void)user;
  uint32_t bits = 0;
  for (Node * c = gtext_markup_node_first_child(h); c; c = gtext_markup_node_next_sibling(c)) {
    const GTEXT_Markup_Kind k = gtext_markup_node_kind(c);
    if (k == GTEXT_MARKUP_KIND_ELEMENT) {
      bits |= GTEXT_CSS_CHILD_ELEMENT;
    }
    else if (k == GTEXT_MARKUP_KIND_TEXT || k == GTEXT_MARKUP_KIND_CDATA || k == GTEXT_MARKUP_KIND_ENTITY_REFERENCE) {
      size_t n = 0;
      const char * d = gtext_markup_node_data(c, &n);
      if (k == GTEXT_MARKUP_KIND_ENTITY_REFERENCE) {
        bits |= GTEXT_CSS_CHILD_TEXT | GTEXT_CSS_CHILD_TEXT_NONSPACE;
      }
      else if (d && n) {
        bits |= GTEXT_CSS_CHILD_TEXT;
        for (size_t i = 0; i < n; i++) {
          if (!class_space(d[i])) {
            bits |= GTEXT_CSS_CHILD_TEXT_NONSPACE;
            break;
          }
        }
      }
    }
  }
  return bits;
}

static const void * cb_tree_scope(void * user, GTEXT_CSS_Element_Handle h) {
  struct GTEXT_CSS_Markup_Adapter * a = user;
  (void)h;
  return a->doc;
}

static GTEXT_CSS_Element_Handle cb_no_element(void * user, GTEXT_CSS_Element_Handle h) {
  (void)user;
  (void)h;
  return NULL;
}

static const void * cb_no_scope(void * user, GTEXT_CSS_Element_Handle h) {
  (void)user;
  (void)h;
  return NULL;
}

static bool cb_no_part(void * user, GTEXT_CSS_Element_Handle h, GTEXT_CSS_Element_Handle host, const GTEXT_CSS_String * name) {
  (void)user;
  (void)h;
  (void)host;
  (void)name;
  return false;
}

static bool cb_no_state(void * user, GTEXT_CSS_Element_Handle h, const GTEXT_CSS_String * name) {
  (void)user;
  (void)h;
  (void)name;
  return false;
}

static bool cb_no_column(void * user, GTEXT_CSS_Element_Handle column, GTEXT_CSS_Element_Handle cell) {
  (void)user;
  (void)column;
  (void)cell;
  return false;
}

/* ---- the states the markup decides ------------------------------------------------------------------------------ */

static bool one_of(const char * s, const char * const * list, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (lc_eq(s, list[i])) {
      return true;
    }
  }
  return false;
}

/// The `type` of an input, lower-cased by comparison; "text" when it is missing or not a type.
static const char * input_type(Node * el) {
  static const char * const types[] = {"hidden", "text", "search", "tel", "url", "email", "password", "date", "month", "week", "time", "datetime-local", "number", "range",
      "color", "checkbox", "radio", "file", "submit", "image", "reset", "button"};
  GTEXT_Markup_Attribute a;
  if (attr(el, "type", &a)) {
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++) {
      if (lc_eq(a.value, types[i])) {
        return types[i];
      }
    }
  }
  return "text";
}

static bool text_like(const char * t) {
  static const char * const l[] = {"text", "search", "tel", "url", "email", "password", "date", "month", "week", "time", "datetime-local", "number"};
  return one_of(t, l, sizeof l / sizeof l[0]);
}

static bool can_be_disabled(Node * el) {
  static const char * const l[] = {"button", "input", "select", "textarea", "optgroup", "option", "fieldset"};
  return in_ns(el, NS_HTML) && one_of(gtext_markup_element_local_name(el), l, sizeof l / sizeof l[0]);
}

/// "Actually disabled" (the HTML Standard, 4.10.18.5): the attribute, a disabled `fieldset` ancestor outside its first `legend`, or (an `option`) a disabled `optgroup` parent.
static bool actually_disabled(Node * el) {
  if (has_attr(el, "disabled")) {
    return true;
  }
  if (html_is(el, "option")) {
    Node * p = gtext_markup_node_parent(el);
    if (is_el(p) && html_is(p, "optgroup") && has_attr(p, "disabled")) {
      return true;
    }
  }
  if (html_is(el, "optgroup")) {
    return false; // only its own attribute disables it
  }
  Node * child = el;
  for (Node * p = gtext_markup_node_parent(el); is_el(p); child = p, p = gtext_markup_node_parent(p)) {
    if (html_is(p, "fieldset") && has_attr(p, "disabled")) {
      // the first legend child of the fieldset is exempt
      Node * first_legend = NULL;
      for (Node * c = gtext_markup_node_first_child(p); c; c = gtext_markup_node_next_sibling(c)) {
        if (is_el(c) && html_is(c, "legend")) {
          first_legend = c;
          break;
        }
      }
      if (child != first_legend) {
        return true;
      }
    }
  }
  return false;
}

static bool contenteditable(Node * el) {
  for (Node * p = el; is_el(p); p = gtext_markup_node_parent(p)) {
    GTEXT_Markup_Attribute a;
    if (attr(p, "contenteditable", &a)) {
      if (a.value_length == 0 || lc_eq(a.value, "true") || lc_eq(a.value, "plaintext-only")) {
        return true;
      }
      if (lc_eq(a.value, "false")) {
        return false;
      }
    }
  }
  return false;
}

/// A custom element name (the HTML Standard, 4.13.3 "valid custom element name"), as far as ASCII and the ranges of PCENChar go.
static bool valid_custom_name(const char * s) {
  static const char * const reserved[] = {"annotation-xml", "color-profile", "font-face", "font-face-src", "font-face-uri", "font-face-format", "font-face-name",
      "missing-glyph"};
  if (!(s[0] >= 'a' && s[0] <= 'z')) {
    return false;
  }
  bool dash = false;
  for (const char * p = s; *p; p++) {
    const unsigned char c = (unsigned char)*p;
    if (c == '-') {
      dash = true;
    }
    else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c >= 0x80)) {
      return false;
    }
  }
  if (!dash) {
    return false;
  }
  for (size_t i = 0; i < sizeof reserved / sizeof reserved[0]; i++) {
    if (strcmp(s, reserved[i]) == 0) {
      return false;
    }
  }
  return true;
}

static uint64_t derive_state(struct GTEXT_CSS_Markup_Adapter * a, Node * el) {
  uint64_t st = 0;
  const char * local = gtext_markup_element_local_name(el);
  const bool html = in_ns(el, NS_HTML);
  if (!html || !valid_custom_name(local)) {
    st |= GTEXT_CSS_STATE_DEFINED;
  }
  if (html) {
    if ((strcmp(local, "a") == 0 || strcmp(local, "area") == 0) && has_attr(el, "href")) {
      st |= GTEXT_CSS_STATE_LINK;
    }
    const bool input = strcmp(local, "input") == 0;
    const char * type = input ? input_type(el) : "";
    if (input && (strcmp(type, "checkbox") == 0 || strcmp(type, "radio") == 0) && has_attr(el, "checked")) {
      st |= GTEXT_CSS_STATE_CHECKED | GTEXT_CSS_STATE_DEFAULT;
    }
    if (strcmp(local, "option") == 0 && has_attr(el, "selected")) {
      st |= GTEXT_CSS_STATE_CHECKED | GTEXT_CSS_STATE_DEFAULT;
    }
    if (can_be_disabled(el)) {
      st |= actually_disabled(el) ? GTEXT_CSS_STATE_DISABLED : GTEXT_CSS_STATE_ENABLED;
    }
    const bool requirable = (input && !(strcmp(type, "hidden") == 0 || strcmp(type, "range") == 0 || strcmp(type, "color") == 0 || strcmp(type, "submit") == 0 ||
                                 strcmp(type, "image") == 0 || strcmp(type, "reset") == 0 || strcmp(type, "button") == 0)) ||
        strcmp(local, "select") == 0 || strcmp(local, "textarea") == 0;
    if (requirable) {
      st |= has_attr(el, "required") ? GTEXT_CSS_STATE_REQUIRED : GTEXT_CSS_STATE_OPTIONAL;
    }
    bool rw = false;
    if ((input && text_like(type)) || strcmp(local, "textarea") == 0) {
      rw = !has_attr(el, "readonly") && !(can_be_disabled(el) && actually_disabled(el));
    }
    else if (contenteditable(el)) {
      rw = true;
    }
    st |= rw ? GTEXT_CSS_STATE_READ_WRITE : GTEXT_CSS_STATE_READ_ONLY;
    GTEXT_Markup_Attribute ph;
    if (((input && text_like(type)) || strcmp(local, "textarea") == 0) && attr(el, "placeholder", &ph) && ph.value_length) {
      bool empty;
      if (input) {
        GTEXT_Markup_Attribute v;
        empty = !attr(el, "value", &v) || v.value_length == 0;
      }
      else {
        empty = true;
        for (Node * c = gtext_markup_node_first_child(el); c; c = gtext_markup_node_next_sibling(c)) {
          size_t n = 0;
          if (gtext_markup_node_data(c, &n) && n) {
            empty = false;
            break;
          }
        }
      }
      if (empty) {
        st |= GTEXT_CSS_STATE_PLACEHOLDER_SHOWN;
      }
    }
    if (strcmp(local, "progress") == 0 && !has_attr(el, "value")) {
      st |= GTEXT_CSS_STATE_INDETERMINATE;
    }
    if ((strcmp(local, "details") == 0 || strcmp(local, "dialog") == 0) && has_attr(el, "open")) {
      st |= GTEXT_CSS_STATE_OPEN;
    }
  }
  else if (strcmp(local, "a") == 0 && in_ns(el, GTEXT_MARKUP_NS_SVG)) {
    GTEXT_Markup_Attribute h;
    if (attr(el, "href", &h) || gtext_markup_element_get_attribute(el, GTEXT_MARKUP_NS_XLINK, "href", &h)) {
      st |= GTEXT_CSS_STATE_LINK;
    }
  }
  if (a->opt.state) {
    st |= a->opt.state(a->opt.user, el);
  }
  return st;
}

static uint64_t cb_state(void * user, GTEXT_CSS_Element_Handle h) {
  return derive_state(user, h);
}

/* ---- language and direction ---------------------------------------------------------------------------------------- */

static bool cb_lang(void * user, GTEXT_CSS_Element_Handle h, GTEXT_CSS_String * out) {
  (void)user;
  for (Node * p = h; is_el(p); p = gtext_markup_node_parent(p)) {
    GTEXT_Markup_Attribute a;
    if (attr(p, "lang", &a) || gtext_markup_element_get_attribute(p, GTEXT_MARKUP_NS_XML, "lang", &a)) {
      out->data = a.value;
      out->length = a.value_length;
      return true;
    }
  }
  return false;
}

/// The code point at the start of `s` (UTF-8), with its length; 0xFFFD for a byte that is not.
static uint32_t decode(const unsigned char * s, size_t n, size_t * len) {
  if (s[0] < 0x80) {
    *len = 1;
    return s[0];
  }
  size_t need = s[0] >= 0xF0 ? 4 : s[0] >= 0xE0 ? 3 : s[0] >= 0xC0 ? 2 : 0;
  if (!need || need > n) {
    *len = 1;
    return 0xFFFD;
  }
  uint32_t cp = s[0] & (0xFFu >> (need + 1));
  for (size_t i = 1; i < need; i++) {
    if ((s[i] & 0xC0) != 0x80) {
      *len = 1;
      return 0xFFFD;
    }
    cp = (cp << 6) | (s[i] & 0x3F);
  }
  *len = need;
  return cp;
}

/// The first strong character of the text below `el`, as the HTML Standard's "auto directionality" finds it: -1 none, 0 ltr, 1 rtl. Elements with a `dir` attribute
/// of their own, `bdi`, `script`, `style` and `textarea` are skipped with what is in them.
static int strong_below(Node * el) {
  Node * n = el;
  bool descend = true;
  for (;;) {
    n = gtext_markup_node_next_in_order(el, n, !descend);
    if (!n) {
      return -1;
    }
    descend = true;
    const GTEXT_Markup_Kind k = gtext_markup_node_kind(n);
    if (k == GTEXT_MARKUP_KIND_ELEMENT) {
      GTEXT_Markup_Attribute d;
      const char * l = gtext_markup_element_local_name(n);
      if ((attr(n, "dir", &d) && (lc_eq(d.value, "ltr") || lc_eq(d.value, "rtl") || lc_eq(d.value, "auto"))) || strcmp(l, "bdi") == 0 ||
          strcmp(l, "script") == 0 || strcmp(l, "style") == 0 || strcmp(l, "textarea") == 0) {
        descend = false;
      }
    }
    else if (k == GTEXT_MARKUP_KIND_TEXT || k == GTEXT_MARKUP_KIND_CDATA) {
      size_t len = 0;
      const unsigned char * s = (const unsigned char *)gtext_markup_node_data(n, &len);
      for (size_t i = 0; s && i < len;) {
        size_t w;
        const uint32_t cp = decode(s + i, len - i, &w);
        i += w;
        const GUNI_BidiClass c = guni_bidi_class(cp);
        if (c == GUNI_BIDI_L) {
          return 0;
        }
        if (c == GUNI_BIDI_R || c == GUNI_BIDI_AL) {
          return 1;
        }
      }
    }
  }
}

static GTEXT_CSS_Direction cb_direction(void * user, GTEXT_CSS_Element_Handle h) {
  (void)user;
  for (Node * p = h; is_el(p); p = gtext_markup_node_parent(p)) {
    if (!in_ns(p, NS_HTML)) {
      continue;
    }
    GTEXT_Markup_Attribute d;
    if (!attr(p, "dir", &d)) {
      continue;
    }
    if (lc_eq(d.value, "ltr")) {
      return GTEXT_CSS_DIRECTION_LTR;
    }
    if (lc_eq(d.value, "rtl")) {
      return GTEXT_CSS_DIRECTION_RTL;
    }
    if (lc_eq(d.value, "auto")) {
      return strong_below(p) == 1 ? GTEXT_CSS_DIRECTION_RTL : GTEXT_CSS_DIRECTION_LTR;
    }
  }
  return GTEXT_CSS_DIRECTION_LTR;
}

/* ---- the adapter ---------------------------------------------------------------------------------------------------- */

GTEXT_CSS_Status gtext_css_markup_adapter_new(const GTEXT_Markup_Document * document, const GTEXT_CSS_Markup_Options * options,
    GTEXT_CSS_Markup_Adapter ** out) {
  if (!document || !out) {
    return GTEXT_CSS_E_INVALID;
  }
  GTEXT_CSS_Markup_Options o;
  memset(&o, 0, sizeof o);
  if (options) {
    o = *options;
  }
  GTEXT_Allocator alloc = *(o.allocator ? o.allocator : gtext_allocator_default());
  struct GTEXT_CSS_Markup_Adapter * a = gtext_allocator_calloc(&alloc, 1, sizeof *a);
  if (!a) {
    return GTEXT_CSS_E_OOM;
  }
  a->alloc = alloc;
  o.allocator = NULL;
  a->opt = o;
  a->doc = document;
  Node * de = gtext_markup_document_element(document);
  switch (o.kind) {
  case GTEXT_CSS_MARKUP_HTML: a->html = true; break;
  case GTEXT_CSS_MARKUP_XML: a->html = false; break;
  default: a->html = de && in_ns(de, NS_HTML); break;
  }
  a->quirks = gtext_markup_document_mode(document) == GTEXT_MARKUP_MODE_QUIRKS;
  GTEXT_CSS_Element_Interface *i = &a->iface;
  i->size = sizeof *i;
  i->user = a;
  i->parent = cb_parent;
  i->local_name = cb_local_name;
  i->namespace_uri = cb_namespace_uri;
  i->id = cb_id;
  i->class_count = cb_class_count;
  i->class_at = cb_class_at;
  i->attribute = cb_attribute;
  i->previous_sibling = cb_previous_sibling;
  i->next_sibling = cb_next_sibling;
  i->state = cb_state;
  i->lang = cb_lang;
  i->direction = cb_direction;
  i->first_child = cb_first_child;
  i->last_child = cb_last_child;
  i->child_content = cb_child_content;
  i->tree_scope = cb_tree_scope;
  i->containing_host = cb_no_element;
  i->shadow_scope = cb_no_scope;
  i->assigned_slot = cb_no_element;
  i->has_part = cb_no_part;
  i->custom_state = cb_no_state;
  i->column_cell = cb_no_column;
  *out = a;
  return GTEXT_CSS_OK;
}

void gtext_css_markup_adapter_free(GTEXT_CSS_Markup_Adapter * a) {
  if (!a) {
    return;
  }
  GTEXT_Allocator al = a->alloc;
  gtext_allocator_free(&al, a);
}

const GTEXT_CSS_Element_Interface * gtext_css_markup_interface(const GTEXT_CSS_Markup_Adapter * a) {
  return a ? &a->iface : NULL;
}

GTEXT_CSS_Selector_Match_Options gtext_css_markup_match_options(const GTEXT_CSS_Markup_Adapter * a) {
  GTEXT_CSS_Selector_Match_Options o = gtext_css_selector_match_options_default();
  if (a) {
    o.html_document = a->html;
    o.quirks = a->quirks;
    o.visited = GTEXT_CSS_VISITED_NEVER;
    o.root_given = true;
    o.root = gtext_markup_document_element(a->doc);
    o.allocator = &a->alloc;
  }
  return o;
}

GTEXT_CSS_Element_Handle gtext_css_markup_handle(const GTEXT_Markup_Node * element) {
  return element;
}
