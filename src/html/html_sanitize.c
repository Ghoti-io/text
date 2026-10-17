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
 */
#include <ghoti.io/text/macros.h>

#include <ghoti.io/text/html/html_sanitize.h>
#include <ghoti.io/text/markup/markup_html.h>
#include <ctype.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#define SANITIZE_DEFAULT_WORK ((size_t)20000000)
#define SANITIZE_DEFAULT_OUTPUT ((size_t)64 * 1024 * 1024)
#define SANITIZE_POLICY_VERSION "ghoti-html-flow-1"

static void result_set(GTEXT_HTML_Sanitize_Result *r, GTEXT_HTML_Status s,
    size_t work) {
  if (r) {
    r->status = s;
    r->message = gtext_html_status_string(s);
    r->policy_version = SANITIZE_POLICY_VERSION;
    r->work_units = work;
  }
}

GTEXT_HTML_Sanitize_Options gtext_html_sanitize_options_default(void) {
  GTEXT_HTML_Sanitize_Options o;
  memset(&o, 0, sizeof(o));
  o.parse = gtext_html_parse_options_default();
  o.allow_links = true;
  o.max_work_units = SANITIZE_DEFAULT_WORK;
  o.max_output_bytes = SANITIZE_DEFAULT_OUTPUT;
  return o;
}

static bool charge(size_t *used, size_t max, size_t amount) {
  if (amount > max - *used) return false;
  *used += amount;
  return true;
}

static bool eq(const char *a, const char *b) {
  return a && b && strcmp(a, b) == 0;
}

static bool eq_ascii_ci(const char *a, const char *b) {
  return a && b && strcasecmp(a, b) == 0;
}

static bool in(const char *s, const char *const *set, size_t n) {
  for (size_t i = 0; i < n; ++i) if (eq(s, set[i])) return true;
  return false;
}

static bool valid_web_authority(const char *s, size_t n) {
  size_t host_start = 0;
  for (size_t i = 0; i < n; ++i) if (s[i] == '@') host_start = i + 1;
  if (host_start == n) return false;
  size_t host_end = n;
  for (size_t i = host_start; i < n; ++i) {
    if (s[i] == ':') { host_end = i; break; }
    if ((s[i] == '[' || s[i] == ']') && s[host_start] == '[') continue;
    if (s[i] == '[' || s[i] == ']' || s[i] == ' ' || s[i] == '\\') return false;
  }
  size_t port_start = host_end < n ? host_end + 1 : n;
  if (s[host_start] == '[') {
    size_t close = host_start + 1;
    while (close < n && s[close] != ']') {
      unsigned char c = (unsigned char)s[close];
      if (!(isxdigit(c) || c == ':' || c == '.')) return false;
      ++close;
    }
    if (close == host_start + 1 || close == n) return false;
    if (close + 1 < n) {
      if (s[close + 1] != ':') return false;
      port_start = close + 2;
    } else if (close + 1 == n) port_start = n;
    host_end = close + 1;
  } else {
    if (host_end == host_start) return false;
    for (size_t i = host_start; i < host_end; ++i) {
      unsigned char c = (unsigned char)s[i];
      if (!(isalnum(c) || c == '-' || c == '.' || c >= 0x80)) return false;
    }
    if (host_end < n) port_start = host_end + 1;
  }
  if (port_start == n && host_end < n) return false;
  if (port_start < n) {
    unsigned port = 0;
    for (size_t i = port_start; i < n; ++i) {
      if (s[i] < '0' || s[i] > '9') return false;
      unsigned d = (unsigned)(s[i] - '0');
      if (port > (65535u - d) / 10u) return false;
      port = port * 10u + d;
    }
  }
  return true;
}

static const char *const base_elements[] = {
  "a", "p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "blockquote",
  "ul", "ol", "li", "br", "strong", "em", "b", "i", "code", "pre"
};
static const char *const table_elements[] = {
  "table", "caption", "colgroup", "col", "thead", "tbody", "tfoot",
  "tr", "th", "td"
};
static const char *const unwrap_elements[] = {
  "abbr", "bdi", "bdo", "cite", "del", "dfn", "font", "ins", "kbd",
  "mark", "q", "s", "samp", "small", "span", "strike", "sub", "sup",
  "time", "tt", "u", "var"
};
/* Elements whose descendants must not be exposed as ordinary flow content. */
static const char *const drop_elements[] = {
  "script", "style", "noscript", "template", "title", "textarea", "xmp",
  "plaintext", "iframe", "frame", "frameset", "noframes", "noembed",
  "object", "embed", "applet", "portal", "fencedframe", "audio", "video",
  "source", "track", "canvas", "form", "input", "button", "select",
  "option", "optgroup", "textarea", "fieldset", "legend", "label",
  "output", "progress", "meter", "base", "basefont", "link", "meta",
  "area", "map", "param", "keygen", "isindex", "header", "head",
  "html", "body"
};

static bool dangerous_element(const GTEXT_Markup_Node *node, const char *name) {
  const char *ns = gtext_markup_element_namespace(node);
  if (!eq(ns, GTEXT_MARKUP_NS_HTML)) return true;
  if (gtext_markup_html_is_raw_text(ns, name) ||
      gtext_markup_html_is_escapable_raw_text(ns, name)) return true;
  if (in(name, drop_elements, sizeof(drop_elements) / sizeof(drop_elements[0]))) return true;
  /* Custom elements may run application callbacks as soon as inserted. */
  return strchr(name, '-') != NULL;
}

static bool safe_url(const char *value, size_t length, bool image,
    const char **start_out, size_t *length_out) {
  size_t first = 0, last = length;
  while (first < last && (value[first] == ' ' || value[first] == '\t' ||
      value[first] == '\r' || value[first] == '\n' || value[first] == '\f')) ++first;
  while (last > first && (value[last - 1] == ' ' || value[last - 1] == '\t' ||
      value[last - 1] == '\r' || value[last - 1] == '\n' || value[last - 1] == '\f')) --last;
  /* Control characters and NUL are ambiguous across URL parsers. */
  for (size_t i = first; i < last; ++i) {
    unsigned char c = (unsigned char)value[i];
    if (c == 0 || c < 0x20 || c == 0x7f) return false;
  }
  size_t colon = last;
  for (size_t i = first; i < last; ++i) {
    if (value[i] == ':') { colon = i; break; }
    if (value[i] == '/' || value[i] == '?' || value[i] == '#') break;
  }
  if (colon != last) {
    if (colon == first) return false;
    for (size_t i = first; i < colon; ++i) {
      unsigned char c = (unsigned char)value[i];
      if (!(isalnum(c) || c == '+' || c == '.' || c == '-')) return false;
    }
    size_t n = colon - first;
    bool http = n == 4 && strncasecmp(value + first, "http", 4) == 0;
    bool https = n == 5 && strncasecmp(value + first, "https", 5) == 0;
    bool mailto = n == 6 && strncasecmp(value + first, "mailto", 6) == 0;
    if (!(http || https || (!image && mailto))) return false;
    if ((http || https) && colon + 2 < last &&
        (value[colon + 1] == '/' || value[colon + 1] == '\\') &&
        (value[colon + 2] == '/' || value[colon + 2] == '\\')) {
      size_t authority = colon + 1;
      while (authority < last && (value[authority] == '/' || value[authority] == '\\')) ++authority;
      size_t end = authority;
      while (end < last && value[end] != '/' && value[end] != '\\' && value[end] != '?' && value[end] != '#') {
        if (value[end] == ' ' || value[end] == '\t') return false;
        ++end;
      }
      if (!valid_web_authority(value + authority, end - authority)) return false;
    }
  }
  *start_out = value + first;
  *length_out = last - first;
  return true;
}

static bool unsigned_decimal(const char *s, size_t n, unsigned long max,
    unsigned long min, unsigned long *out) {
  if (!n) return false;
  unsigned long v = 0;
  for (size_t i = 0; i < n; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    unsigned d = (unsigned)(s[i] - '0');
    if (v > (max - d) / 10) return false;
    v = v * 10 + d;
  }
  if (v < min || v > max) return false;
  *out = v;
  return true;
}

static bool id_list(const char *s, size_t n) {
  size_t i = 0, items = 0;
  while (i < n) {
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == '\f')) ++i;
    if (i == n) break;
    size_t start = i;
    while (i < n && !(s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == '\f')) ++i;
    if (start == i) return false;
    for (size_t j = start; j < i; ++j) {
      unsigned char c = (unsigned char)s[j];
      if (!(isalnum(c) || c == '_' || c == '-' || c == ':' || c == '.')) return false;
    }
    ++items;
  }
  return items != 0;
}

static bool allowed_attribute(const char *element, const char *name,
    const char *value, size_t value_length, const GTEXT_HTML_Sanitize_Options *o,
    const char **normalized, size_t *normalized_length) {
  static const char *const align_values[] = {"left", "right", "center", "justify", "char"};
  static const char *const valign_values[] = {"top", "middle", "bottom", "baseline"};
  static const char *const scope_values[] = {"row", "col", "rowgroup", "colgroup"};
  *normalized = value;
  *normalized_length = value_length;
  if (eq(element, "a") && o->allow_links && eq(name, "href"))
    return safe_url(value, value_length, false, normalized, normalized_length);
  if (eq(element, "img") && o->allow_images) {
    if (eq(name, "alt")) return true;
    if (eq(name, "src")) return safe_url(value, value_length, true, normalized, normalized_length);
    if (eq(name, "loading")) return (value_length == 4 && strncasecmp(value, "lazy", 4) == 0) ||
        (value_length == 5 && strncasecmp(value, "eager", 5) == 0);
    if (eq(name, "width") || eq(name, "height") || eq(name, "border")) {
      unsigned long ignored;
      return unsigned_decimal(value, value_length, 1000000, 0, &ignored);
    }
  }
  if (o->allow_tables && in(element, table_elements,
      sizeof(table_elements) / sizeof(table_elements[0]))) {
    if (eq(name, "summary") && eq(element, "table")) return true;
    if (eq(name, "align")) {
      for (size_t j = 0; j < sizeof(align_values) / sizeof(align_values[0]); ++j)
        if (eq_ascii_ci(value, align_values[j])) return true;
      return false;
    }
    if (eq(name, "valign")) {
      for (size_t j = 0; j < sizeof(valign_values) / sizeof(valign_values[0]); ++j)
        if (eq_ascii_ci(value, valign_values[j])) return true;
      return false;
    }
    if ((eq(name, "colspan") || eq(name, "rowspan")) && (eq(element, "td") || eq(element, "th"))) {
      unsigned long span;
      bool ok = eq(name, "colspan") ? unsigned_decimal(value, value_length, 1000, 1, &span) :
          unsigned_decimal(value, value_length, 65534, 0, &span);
      return ok;
    }
    if (eq(name, "headers") && (eq(element, "td") || eq(element, "th"))) return id_list(value, value_length);
    if (eq(name, "scope") && eq(element, "th")) {
      for (size_t j = 0; j < sizeof(scope_values) / sizeof(scope_values[0]); ++j)
        if (eq_ascii_ci(value, scope_values[j])) return true;
      return false;
    }
  }
  return false;
}

static GTEXT_Markup_Node *after_subtree(GTEXT_Markup_Node *root, GTEXT_Markup_Node *node) {
  while (node && node != root) {
    GTEXT_Markup_Node *next = gtext_markup_node_next_sibling(node);
    if (next) return next;
    node = gtext_markup_node_parent(node);
  }
  return NULL;
}

static GTEXT_HTML_Status sanitize_tree(GTEXT_Markup_Node *root,
    const GTEXT_HTML_Sanitize_Options *o, size_t *work) {
  const size_t max_work = o->max_work_units ? o->max_work_units : SANITIZE_DEFAULT_WORK;
  GTEXT_Markup_Node *node = gtext_markup_node_first_child(root);
  while (node) {
    if (!charge(work, max_work, 1)) return GTEXT_HTML_E_LIMIT_SANITIZE_WORK;
    GTEXT_Markup_Kind kind = gtext_markup_node_kind(node);
    if (kind != GTEXT_MARKUP_KIND_ELEMENT) {
      if (kind != GTEXT_MARKUP_KIND_TEXT) {
        GTEXT_Markup_Node *next = after_subtree(root, node);
        (void)gtext_markup_remove(node);
        (void)gtext_markup_node_free(node);
        node = next;
      } else {
        node = after_subtree(root, node);
      }
      continue;
    }
    const char *name = gtext_markup_element_local_name(node);
    bool image = eq(name, "img") && o->allow_images;
    bool table = o->allow_tables && in(name, table_elements, sizeof(table_elements) / sizeof(table_elements[0]));
    bool allowed = in(name, base_elements, sizeof(base_elements) / sizeof(base_elements[0])) || image || table;
    if (!allowed && dangerous_element(node, name)) {
      GTEXT_Markup_Node *next = after_subtree(root, node);
      (void)gtext_markup_remove(node);
      (void)gtext_markup_node_free(node);
      node = next;
      continue;
    }
    if (!allowed && !in(name, unwrap_elements, sizeof(unwrap_elements) / sizeof(unwrap_elements[0]))) {
      /* Unknown/structural elements are not a reviewed transparent wrapper. */
      GTEXT_Markup_Node *next = after_subtree(root, node);
      (void)gtext_markup_remove(node);
      (void)gtext_markup_node_free(node);
      node = next;
      continue;
    }
    if (allowed) {
      size_t i = 0;
      while (i < gtext_markup_element_attribute_count(node)) {
        if (!charge(work, max_work, 1)) return GTEXT_HTML_E_LIMIT_SANITIZE_WORK;
        GTEXT_Markup_Attribute a;
        if (gtext_markup_element_attribute_at(node, i, &a) != GTEXT_MARKUP_OK) return GTEXT_HTML_E_INTERNAL;
        if (!charge(work, max_work, a.value_length)) return GTEXT_HTML_E_LIMIT_SANITIZE_WORK;
        const char *norm = a.value;
        size_t norm_len = a.value_length;
        bool keep = !a.ns && allowed_attribute(name, a.local, a.value, a.value_length, o, &norm, &norm_len);
        if (keep && norm != a.value) {
          if (!charge(work, max_work, norm_len)) return GTEXT_HTML_E_LIMIT_SANITIZE_WORK;
          GTEXT_Markup_Status set_status = gtext_markup_element_set_attribute(node, NULL, NULL,
              a.local, norm, norm_len);
          if (set_status != GTEXT_MARKUP_OK)
            return set_status == GTEXT_MARKUP_E_LIMIT ? GTEXT_HTML_E_LIMIT_ATTRIBUTES : GTEXT_HTML_E_OOM;
        }
        if (!keep) (void)gtext_markup_element_remove_attribute(node, a.ns, a.local);
        else ++i;
      }
      if (eq(name, "a")) {
        GTEXT_Markup_Attribute href;
        if (o->allow_links && gtext_markup_element_get_attribute(node, NULL, "href", &href)) {
          static const char rel[] = "nofollow";
          if (o->parse.max_attributes &&
              gtext_markup_element_attribute_count(node) >= o->parse.max_attributes)
            return GTEXT_HTML_E_LIMIT_ATTRIBUTES;
          if (!charge(work, max_work, sizeof(rel) - 1)) return GTEXT_HTML_E_LIMIT_SANITIZE_WORK;
          GTEXT_Markup_Status set_status = gtext_markup_element_set_attribute(node, NULL, NULL,
              "rel", rel, sizeof(rel) - 1);
          if (set_status != GTEXT_MARKUP_OK)
            return set_status == GTEXT_MARKUP_E_LIMIT ? GTEXT_HTML_E_LIMIT_ATTRIBUTES : GTEXT_HTML_E_OOM;
        }
      }
    } else {
      /* Transparent formatting wrapper: move its children into its place. */
      GTEXT_Markup_Node *parent = gtext_markup_node_parent(node);
      GTEXT_Markup_Node *child;
      GTEXT_Markup_Node *first_moved = gtext_markup_node_first_child(node);
      while ((child = gtext_markup_node_first_child(node)) != NULL) {
        if (!charge(work, max_work, 1)) return GTEXT_HTML_E_LIMIT_SANITIZE_WORK;
        if (gtext_markup_insert_before(parent, child, node) != GTEXT_MARKUP_OK) return GTEXT_HTML_E_INTERNAL;
      }
      GTEXT_Markup_Node *next = gtext_markup_node_next_sibling(node);
      (void)gtext_markup_remove(node);
      (void)gtext_markup_node_free(node);
      node = first_moved ? first_moved : next;
      continue;
    }
    GTEXT_Markup_Node *child = gtext_markup_node_first_child(node);
    node = child ? child : after_subtree(root, node);
  }
  return GTEXT_HTML_OK;
}

GTEXT_HTML_Status gtext_html_sanitize_fragment(const void *data, size_t length,
    const GTEXT_HTML_Sanitize_Options *options, GTEXT_Markup_Document **doc_out,
    GTEXT_Markup_Node **fragment_out, GTEXT_HTML_Sanitize_Result *result) {
  if (doc_out) *doc_out = NULL;
  if (fragment_out) *fragment_out = NULL;
  size_t work = 0;
  if (!doc_out || !fragment_out || (!data && length)) {
    result_set(result, GTEXT_HTML_E_INVALID, 0);
    return GTEXT_HTML_E_INVALID;
  }
  GTEXT_HTML_Sanitize_Options defaults;
  if (!options) { defaults = gtext_html_sanitize_options_default(); options = &defaults; }
  GTEXT_HTML_Fragment_Context context = {0};
  context.local_name = "div";
  context.ns = GTEXT_HTML_NS_HTML;
  GTEXT_HTML_Result parse_result = {0};
  GTEXT_Markup_Document *doc = NULL;
  GTEXT_Markup_Node *fragment = NULL;
  GTEXT_HTML_Status s = gtext_html_parse_fragment(&context, data, length,
      &options->parse, &doc, &fragment, &parse_result);
  gtext_html_result_free(&parse_result);
  if (s != GTEXT_HTML_OK) { result_set(result, s, work); return s; }
  s = sanitize_tree(fragment, options, &work);
  if (s != GTEXT_HTML_OK) {
    gtext_markup_document_free(doc);
    result_set(result, s, work);
    return s;
  }
  *doc_out = doc;
  *fragment_out = fragment;
  result_set(result, GTEXT_HTML_OK, work);
  return GTEXT_HTML_OK;
}

GTEXT_HTML_Status gtext_html_sanitize_buffer(const void *data, size_t length,
    const GTEXT_HTML_Sanitize_Options *options, char **output,
    size_t *output_length, GTEXT_HTML_Sanitize_Result *result) {
  if (output) *output = NULL;
  if (output_length) *output_length = 0;
  if (!output) { result_set(result, GTEXT_HTML_E_INVALID, 0); return GTEXT_HTML_E_INVALID; }
  GTEXT_HTML_Sanitize_Options defaults;
  if (!options) { defaults = gtext_html_sanitize_options_default(); options = &defaults; }
  GTEXT_Markup_Document *doc = NULL;
  GTEXT_Markup_Node *fragment = NULL;
  GTEXT_HTML_Status s = gtext_html_sanitize_fragment(data, length, options,
      &doc, &fragment, result);
  if (s != GTEXT_HTML_OK) return s;
  GTEXT_HTML_Serialize_Options so = gtext_html_serialize_options_default();
  so.allocator = options->parse.allocator;
  so.max_output_bytes = options->max_output_bytes ? options->max_output_bytes : SANITIZE_DEFAULT_OUTPUT;
  so.context_name = "div";
  so.context_namespace = GTEXT_MARKUP_NS_HTML;
  char *buffer = NULL;
  size_t buffer_length = 0;
  GTEXT_HTML_Serialize_Result sr;
  s = gtext_html_serialize_buffer(fragment, GTEXT_HTML_SERIALIZE_CHILDREN,
      &so, &buffer, &buffer_length, &sr);
  if (s == GTEXT_HTML_OK) {
    *output = buffer;
    if (output_length) *output_length = buffer_length;
  }
  size_t work = result ? result->work_units : 0;
  gtext_markup_document_free(doc);
  result_set(result, s, work);
  return s;
}

void gtext_html_sanitize_buffer_free(char *output) {
  gtext_html_serialize_buffer_free(NULL, NULL, output);
}
