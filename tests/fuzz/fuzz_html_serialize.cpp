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
 * libFuzzer harness for the HTML serializer (src/html/html_serialize.c, html_file_io.c).
 *
 * The trees are the HTML parser's output on random bytes - which is a tree of every shape the parser can make, template
 * contents, foreign content and quirks included - written under options chosen from the first bytes. The input is six option
 * bytes and then the bytes to parse. o0: bit 0 the scripting flag (for the parse and the write), bit 1 a fragment, bits 2 to 7
 * its context element. o1: omit_optional_tags, quote_attr_values, use_best_quote_char, use_trailing_solidus,
 * space_before_trailing_solidus, minimize_boolean_attributes, alphabetical_attributes, strip_whitespace. o2: escape_rcdata,
 * doctype_identifiers, inject_meta_charset (with the label utf-8), the scope (children or the first child itself), and the
 * quote character (default, `"`, `'`). o3: the call of the sink that fails, and the size of the pieces the token stream is
 * fed text in. o4: the failure sweep of the allocator (bit 0 through the sink, bit 1 through the buffer, bit 2 the token
 * stream). o5: the output limit (bit 7 on; bits 0 to 2 pick it: the length, one less, half, one more, and small ones; the depth
 * limit is always tried at the depth of the tree and one less).
 *
 * The properties:
 *
 *     writing succeeds for every valid option combination and every tree the parser built with its defaults (HTML serialization
 *     does not fail on content, and a tree the parser's default limits admit fits the serializer's), and an invalid combination
 *     (a quote character with the best-quote option, `space_before_trailing_solidus` without `use_trailing_solidus`) is GTEXT_HTML_E_OPTIONS with nothing
 *     written, through every entry point;
 *     with the standard's defaults the output is the output of a model in this file that shares no code with the library (the
 *     algorithm of the standard, written a second time, recursively);
 *     the result agrees with the call: the bytes written are the output's length, the nodes visited are the nodes of the tree
 *     the algorithm visits (the children of a void element and of a template are not), and the optional tags left out are
 *     counted only with the option on;
 *     the token-stream serializer, fed the tokens the tree gives, in pieces of any size cut between characters, writes the same
 *     bytes as the walk, under every option;
 *     the output is HTML: it parses without an error status; and a tree that comes back as an equal tree is written again as
 *     the same bytes (idempotence, for the options that do not change what a parser reads);
 *     the scope OUTER of an element is its start tag, what CHILDREN writes, and its end tag (unless it is void), with the
 *     options that do not look at the neighbours (no optional tags);
 *     a sink that refuses the n-th call gets exactly the chunks before it and is not called again, and the status is E_WRITE;
 *     the output limit is exact (the length passes, one byte less is E_LIMIT_OUTPUT with a prefix of the output no longer than
 *     the limit), and so is the depth (the depth of the tree passes, one less is E_LIMIT_DEPTH with the start tags down to it);
 *     an allocation that fails - the first to the last of a call, through the sink, the buffer or the token stream - is
 *     E_OOM, or the normal answer, and never a leak or a free of a block the allocator did not make.
 *
 * Build with: make fuzz-html-serialize      Run: make fuzz-run-html-serialize
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ghoti.io/text/html.h>

#include "../markup_test_support.h"

namespace {

[[noreturn]] void fail(const char * what, const std::string & detail, const std::string & input) {
  std::fprintf(stderr, "fuzz_html_serialize: %s\n%s\ninput (%zu bytes): ", what, detail.c_str(), input.size());
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

struct Collect {
  std::string data;
  size_t calls = 0;
  size_t fail_on = 0;
  bool failed = false;
  size_t calls_after_failure = 0;
  static GTEXT_Markup_Status write(void * user, const char * b, size_t n) {
    auto * self = static_cast<Collect *>(user);
    if (self->failed) {
      self->calls_after_failure++;
    }
    self->calls++;
    if (self->fail_on != 0 && self->calls == self->fail_on) {
      self->failed = true;
      return GTEXT_MARKUP_E_WRITE;
    }
    self->data.append(b, n);
    return GTEXT_MARKUP_OK;
  }
  GTEXT_Markup_Sink sink() {
    return GTEXT_Markup_Sink{&Collect::write, this};
  }
};

struct FuzzCtx {
  GTEXT_HTML_Namespace ns;
  const char * name;
};
const FuzzCtx kContexts[16] = {
    {GTEXT_HTML_NS_HTML, "div"}, {GTEXT_HTML_NS_HTML, "td"}, {GTEXT_HTML_NS_HTML, "tr"}, {GTEXT_HTML_NS_HTML, "table"},
    {GTEXT_HTML_NS_HTML, "select"}, {GTEXT_HTML_NS_HTML, "ul"}, {GTEXT_HTML_NS_HTML, "body"}, {GTEXT_HTML_NS_HTML, "head"},
    {GTEXT_HTML_NS_HTML, "textarea"}, {GTEXT_HTML_NS_HTML, "template"}, {GTEXT_HTML_NS_HTML, "colgroup"},
    {GTEXT_HTML_NS_HTML, "noscript"}, {GTEXT_HTML_NS_SVG, "g"}, {GTEXT_HTML_NS_SVG, "foreignObject"},
    {GTEXT_HTML_NS_MATHML, "mi"}, {GTEXT_HTML_NS_HTML, "html"}};

using Node = GTEXT_Markup_Node;

// ---- the model: the standard's algorithm, written again --------------------------------------------------------------

bool is_void(const Node * e) {
  if (!gtext_markup_element_is(e, GTEXT_MARKUP_NS_HTML, gtext_markup_element_local_name(e))) {
    return false;
  }
  static const char * const names[] = {"area", "base", "basefont", "bgsound", "br", "col", "embed", "frame", "hr", "img",
      "input", "keygen", "link", "meta", "param", "source", "track", "wbr"};
  const char * n = gtext_markup_element_local_name(e);
  for (const char * v : names) {
    if (std::strcmp(n, v) == 0) {
      return true;
    }
  }
  return false;
}

bool is_html(const Node * e) {
  const char * ns = gtext_markup_element_namespace(e);
  return ns && std::strcmp(ns, GTEXT_MARKUP_NS_HTML) == 0;
}

void escape(std::string & out, const char * s, size_t n, bool attribute) {
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = s[i];
    if (c == '&') {
      out += "&amp;";
    }
    else if (c == 0xC2 && i + 1 < n && static_cast<unsigned char>(s[i + 1]) == 0xA0) {
      out += "&nbsp;";
      i++;
    }
    else if (c == '<') {
      out += "&lt;";
    }
    else if (c == '>') {
      out += "&gt;";
    }
    else if (c == '"' && attribute) {
      out += "&quot;";
    }
    else {
      out += static_cast<char>(c);
    }
  }
}

void model_node(std::string & out, const Node * n, const Node * parent, bool scripting);

void model_children(std::string & out, const Node * parent, bool scripting) {
  const Node * from = parent;
  if (gtext_markup_node_kind(parent) == GTEXT_MARKUP_KIND_ELEMENT && is_html(parent)
      && std::strcmp(gtext_markup_element_local_name(parent), "template") == 0) {
    from = gtext_markup_element_template_content(parent);
  }
  for (const Node * c = gtext_markup_node_first_child(from); c; c = gtext_markup_node_next_sibling(c)) {
    model_node(out, c, parent, scripting);
  }
}

void model_node(std::string & out, const Node * n, const Node * parent, bool scripting) {
  size_t len = 0;
  switch (gtext_markup_node_kind(n)) {
    case GTEXT_MARKUP_KIND_ELEMENT: {
      const char * ns = gtext_markup_element_namespace(n);
      const char * prefix = gtext_markup_element_prefix(n);
      const char * local = gtext_markup_element_local_name(n);
      const bool three = ns && (std::strcmp(ns, GTEXT_MARKUP_NS_HTML) == 0 || std::strcmp(ns, GTEXT_MARKUP_NS_SVG) == 0
                                   || std::strcmp(ns, GTEXT_MARKUP_NS_MATHML) == 0);
      const std::string tag = three || !prefix ? std::string(local) : std::string(prefix) + ":" + local;
      out += "<" + tag;
      for (size_t i = 0; i < gtext_markup_element_attribute_count(n); i++) {
        GTEXT_Markup_Attribute a;
        gtext_markup_element_attribute_at(n, i, &a);
        std::string name;
        if (!a.ns) {
          name = a.local;
        }
        else if (!std::strcmp(a.ns, GTEXT_MARKUP_NS_XML)) {
          name = std::string("xml:") + a.local;
        }
        else if (!std::strcmp(a.ns, GTEXT_MARKUP_NS_XMLNS)) {
          name = !std::strcmp(a.local, "xmlns") ? "xmlns" : std::string("xmlns:") + a.local;
        }
        else if (!std::strcmp(a.ns, GTEXT_MARKUP_NS_XLINK)) {
          name = std::string("xlink:") + a.local;
        }
        else {
          name = a.prefix ? std::string(a.prefix) + ":" + a.local : std::string(a.local);
        }
        out += " " + name + "=\"";
        escape(out, a.value, a.value_length, true);
        out += "\"";
      }
      out += ">";
      if (is_void(n)) {
        return;
      }
      model_children(out, n, scripting);
      out += "</" + tag + ">";
      return;
    }
    case GTEXT_MARKUP_KIND_TEXT:
    case GTEXT_MARKUP_KIND_CDATA: {
      const char * d = gtext_markup_node_data(n, &len);
      bool raw = false;
      if (parent && gtext_markup_node_kind(parent) == GTEXT_MARKUP_KIND_ELEMENT && is_html(parent)) {
        const char * p = gtext_markup_element_local_name(parent);
        for (const char * r : {"style", "script", "xmp", "iframe", "noembed", "noframes", "plaintext"}) {
          raw = raw || std::strcmp(p, r) == 0;
        }
        raw = raw || (scripting && std::strcmp(p, "noscript") == 0);
      }
      if (raw) {
        out.append(d, len);
      }
      else {
        escape(out, d, len, false);
      }
      return;
    }
    case GTEXT_MARKUP_KIND_COMMENT: {
      const char * d = gtext_markup_node_data(n, &len);
      out += "<!--";
      out.append(d, len);
      out += "-->";
      return;
    }
    case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION: {
      const char * d = gtext_markup_node_data(n, &len);
      out += std::string("<?") + gtext_markup_pi_target(n) + " ";
      out.append(d, len);
      out += "?>";
      return;
    }
    case GTEXT_MARKUP_KIND_DOCTYPE:
      out += std::string("<!DOCTYPE ") + gtext_markup_doctype_name(n) + ">";
      return;
    default:
      return;
  }
}

// ---- walking the tree for the token stream, the counts and the depth -------------------------------------------------

struct Feed {
  GTEXT_HTML_Serializer * ser;
  size_t piece;     // the size text is fed in, at most
  uint32_t seed;
  GTEXT_HTML_Status status = GTEXT_HTML_OK;
  size_t nodes = 0; // nodes visited as the walk visits them
  size_t depth = 0, max_depth = 0;
};

size_t cut(const char * d, size_t len, size_t at, size_t piece) {
  // the end of a piece of about `piece` bytes (all of it if 0), moved forward to the next character boundary
  size_t end = at + (piece ? piece : len);
  if (end >= len) {
    return len;
  }
  while (end < len && (static_cast<unsigned char>(d[end]) & 0xC0) == 0x80) {
    end++;
  }
  return end;
}

void feed_node(Feed & f, const Node * n);

void feed_children(Feed & f, const Node * parent) {
  const Node * from = parent;
  if (gtext_markup_node_kind(parent) == GTEXT_MARKUP_KIND_ELEMENT && is_html(parent)
      && std::strcmp(gtext_markup_element_local_name(parent), "template") == 0) {
    from = gtext_markup_element_template_content(parent);
  }
  for (const Node * c = gtext_markup_node_first_child(from); c && f.status == GTEXT_HTML_OK; c = gtext_markup_node_next_sibling(c)) {
    feed_node(f, c);
  }
}

void feed_node(Feed & f, const Node * n) {
  f.nodes++;
  size_t len = 0;
  switch (gtext_markup_node_kind(n)) {
    case GTEXT_MARKUP_KIND_ELEMENT: {
      const char * ns = gtext_markup_element_namespace(n);
      const char * prefix = gtext_markup_element_prefix(n);
      const char * local = gtext_markup_element_local_name(n);
      const bool three = ns && (!std::strcmp(ns, GTEXT_MARKUP_NS_HTML) || !std::strcmp(ns, GTEXT_MARKUP_NS_SVG)
                                   || !std::strcmp(ns, GTEXT_MARKUP_NS_MATHML));
      const std::string tag = three || !prefix ? std::string(local) : std::string(prefix) + ":" + local;
      std::vector<GTEXT_Markup_Attribute> attrs(gtext_markup_element_attribute_count(n));
      for (size_t i = 0; i < attrs.size(); i++) {
        gtext_markup_element_attribute_at(n, i, &attrs[i]);
      }
      // An element in no namespace is not an HTML element; the token API's NULL means HTML.
      const char * tns = ns ? ns : "";
      f.status = gtext_html_serializer_start_tag(f.ser, tns, tag.c_str(), attrs.data(), attrs.size());
      if (f.status != GTEXT_HTML_OK) {
        return;
      }
      f.depth++;
      if (f.depth > f.max_depth && !is_void(n)) {
        f.max_depth = f.depth;
      }
      if (!is_void(n)) {
        feed_children(f, n);
      }
      f.depth--;
      if (f.status == GTEXT_HTML_OK) {
        f.status = gtext_html_serializer_end_tag(f.ser, tns, tag.c_str());
      }
      return;
    }
    case GTEXT_MARKUP_KIND_TEXT:
    case GTEXT_MARKUP_KIND_CDATA: {
      const char * d = gtext_markup_node_data(n, &len);
      size_t at = 0;
      while (at < len && f.status == GTEXT_HTML_OK) {
        f.seed = f.seed * 1103515245u + 12345u;
        const size_t piece = f.piece ? 1 + (f.seed >> 16) % f.piece : 0;
        const size_t end = cut(d, len, at, piece);
        f.status = gtext_html_serializer_characters(f.ser, d + at, end - at);
        at = end;
      }
      return;
    }
    case GTEXT_MARKUP_KIND_COMMENT: {
      const char * d = gtext_markup_node_data(n, &len);
      f.status = gtext_html_serializer_comment(f.ser, d ? d : "", len);
      return;
    }
    case GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION: {
      const char * d = gtext_markup_node_data(n, &len);
      f.status = gtext_html_serializer_processing_instruction(f.ser, gtext_markup_pi_target(n), d ? d : "", len);
      return;
    }
    case GTEXT_MARKUP_KIND_DOCTYPE:
      f.status = gtext_html_serializer_doctype(f.ser, gtext_markup_doctype_name(n), gtext_markup_doctype_public_id(n),
          gtext_markup_doctype_system_id(n));
      return;
    default:
      return;
  }
}

// How deep the tree nests and how many nodes the walk visits, counted by a second walk.
void measure(const Node * parent, size_t depth, size_t & max_depth, size_t & nodes) {
  const Node * from = parent;
  if (gtext_markup_node_kind(parent) == GTEXT_MARKUP_KIND_ELEMENT && is_html(parent)
      && std::strcmp(gtext_markup_element_local_name(parent), "template") == 0) {
    from = gtext_markup_element_template_content(parent);
  }
  for (const Node * c = gtext_markup_node_first_child(from); c; c = gtext_markup_node_next_sibling(c)) {
    nodes++;
    if (gtext_markup_node_kind(c) == GTEXT_MARKUP_KIND_ELEMENT && !is_void(c)) {
      if (depth + 1 > max_depth) {
        max_depth = depth + 1;
      }
      measure(c, depth + 1, max_depth, nodes);
    }
    else if (gtext_markup_node_kind(c) == GTEXT_MARKUP_KIND_ELEMENT && depth + 1 > max_depth) {
      // a void element is written and not entered: it holds no level open
    }
  }
}

std::string serialize_tokens(const Node * root, const GTEXT_HTML_Serialize_Options & o, size_t piece, uint32_t seed,
    GTEXT_HTML_Status * status, size_t * depth, size_t * nodes) {
  Collect c;
  GTEXT_Markup_Sink s = c.sink();
  GTEXT_HTML_Serializer * ser = nullptr;
  *status = gtext_html_serializer_new(&o, &s, &ser);
  if (*status != GTEXT_HTML_OK) {
    return "";
  }
  if (gtext_markup_node_kind(root) == GTEXT_MARKUP_KIND_ELEMENT) {
    // The stream does not contain the element it is the contents of; say so, as the tree walk does.
    const char * ns = gtext_markup_element_namespace(root);
    *status = gtext_html_serializer_context(ser, ns && std::strcmp(ns, GTEXT_MARKUP_NS_HTML) != 0 ? ns : nullptr,
        gtext_markup_element_local_name(root));
    if (*status != GTEXT_HTML_OK) {
      gtext_html_serializer_free(ser);
      return "";
    }
  }
  Feed f{ser, piece, seed};
  feed_children(f, root);
  GTEXT_HTML_Serialize_Result r;
  const GTEXT_HTML_Status fs = gtext_html_serializer_finish(ser, &r);
  *status = f.status != GTEXT_HTML_OK ? f.status : fs;
  *depth = f.max_depth;
  *nodes = f.nodes;
  gtext_html_serializer_free(ser);
  return c.data;
}

}  // namespace

/// How many inputs got as far as a tree that was serialized and checked (read by the seed replay test, which asserts
/// that its seeds did not all stop at the parser).
static size_t g_serialized_inputs = 0;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  constexpr size_t kOptionBytes = 6;
  if (size < kOptionBytes || size > 4096) {
    return 0;
  }
  const uint8_t o0 = data[0], o1 = data[1], o2 = data[2], o3 = data[3], o4 = data[4], o5 = data[5];
  const std::string input(reinterpret_cast<const char *>(data), size);
  const bool scripting = (o0 & 1) != 0;
  const bool fragment = (o0 & 2) != 0;
  const FuzzCtx & ctx = kContexts[(o0 >> 2) & 15];

  // ---- the tree
  GTEXT_HTML_Parse_Options po = gtext_html_parse_options_default();
  po.scripting = scripting;
  po.count_errors_only = true;
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_Markup_Node * frag = nullptr;
  GTEXT_HTML_Result res;
  std::memset(&res, 0, sizeof(res));
  GTEXT_HTML_Status ps;
  if (fragment) {
    GTEXT_HTML_Fragment_Context fc;
    std::memset(&fc, 0, sizeof(fc));
    fc.ns = ctx.ns;
    fc.local_name = ctx.name;
    ps = gtext_html_parse_fragment(&fc, data + kOptionBytes, size - kOptionBytes, &po, &doc, &frag, &res);
  }
  else {
    ps = gtext_html_parse(data + kOptionBytes, size - kOptionBytes, &po, &doc, &res);
  }
  gtext_html_result_free(&res);
  if (ps != GTEXT_HTML_OK) {
    return 0; // a limit of the parser: no tree
  }
  const Node * root = fragment ? frag : gtext_markup_document_root(doc);

  g_serialized_inputs++;
  // ---- the options
  GTEXT_HTML_Serialize_Options o = gtext_html_serialize_options_default();
  o.scripting = scripting;
  o.omit_optional_tags = (o1 & 1) != 0;
  o.quote_attr_values = (o1 & 2) ? GTEXT_HTML_QUOTE_WHEN_NEEDED : GTEXT_HTML_QUOTE_ALWAYS;
  o.use_best_quote_char = (o1 & 4) != 0;
  o.use_trailing_solidus = (o1 & 8) != 0;
  o.space_before_trailing_solidus = (o1 & 16) != 0;
  o.minimize_boolean_attributes = (o1 & 32) != 0;
  o.alphabetical_attributes = (o1 & 64) != 0;
  o.strip_whitespace = (o1 & 128) != 0;
  o.escape_rcdata = (o2 & 1) != 0;
  o.doctype_identifiers = (o2 & 2) != 0;
  if (o2 & 4) {
    o.inject_meta_charset = true;
    o.encoding = "utf-8";
  }
  const GTEXT_HTML_Serialize_Scope scope = (o2 & 8) ? GTEXT_HTML_SERIALIZE_OUTER : GTEXT_HTML_SERIALIZE_CHILDREN;
  o.quote_char = ((o2 >> 4) & 3) == 1 ? '"' : ((o2 >> 4) & 3) == 2 ? '\'' : 0;
  const bool valid = !(o.use_best_quote_char && o.quote_char) && !(o.space_before_trailing_solidus && !o.use_trailing_solidus);

  Collect plain;
  GTEXT_Markup_Sink ps_sink = plain.sink();
  GTEXT_HTML_Serialize_Result r;
  const Node * target = root;
  if (scope == GTEXT_HTML_SERIALIZE_OUTER) {
    target = gtext_markup_node_first_child(root);
    if (!target) {
      gtext_markup_document_free(doc);
      return 0;
    }
  }
  const GTEXT_HTML_Status st = gtext_html_serialize(target, scope, &o, &ps_sink, &r);
  if (!valid) {
    if (st != GTEXT_HTML_E_OPTIONS || !plain.data.empty() || plain.calls != 0 || r.status != GTEXT_HTML_E_OPTIONS) {
      fail("an unusable combination of options was not refused", "status " + std::to_string(st), input);
    }
    char * buf = nullptr;
    if (gtext_html_serialize_buffer(target, scope, &o, &buf, nullptr, nullptr) != GTEXT_HTML_E_OPTIONS || buf) {
      fail("an unusable combination of options was not refused (buffer)", "", input);
    }
    GTEXT_HTML_Serializer * ser = nullptr;
    if (gtext_html_serializer_new(&o, &ps_sink, &ser) != GTEXT_HTML_E_OPTIONS || ser) {
      fail("an unusable combination of options was not refused (stream)", "", input);
    }
    gtext_markup_document_free(doc);
    return 0;
  }
  if (st != GTEXT_HTML_OK) {
    fail("a tree the parser built was not written", std::string(gtext_html_status_string(st)), input);
  }
  const std::string full = plain.data;
  if (r.status != GTEXT_HTML_OK || r.bytes_written != full.size()) {
    fail("the result does not say what was written", "bytes " + std::to_string(r.bytes_written) + " of " + std::to_string(full.size()), input);
  }
  if (!o.omit_optional_tags && (r.omitted_start_tags || r.omitted_end_tags)) {
    fail("tags were omitted without the option", "", input);
  }
  if (!o.minimize_boolean_attributes && r.minimized_attributes) {
    fail("attributes were minimized without the option", "", input);
  }
  if (!o.inject_meta_charset && (r.meta_charset_injected || r.meta_charset_replaced)) {
    fail("a meta was changed without the option", "", input);
  }

  // ---- the model, for the standard's defaults and the children of the root
  const bool defaults_only = !o.omit_optional_tags && o.quote_attr_values == GTEXT_HTML_QUOTE_ALWAYS && !o.use_best_quote_char
      && !o.use_trailing_solidus && !o.minimize_boolean_attributes && !o.alphabetical_attributes && !o.strip_whitespace
      && !o.escape_rcdata && !o.doctype_identifiers && !o.inject_meta_charset && o.quote_char == 0;
  if (defaults_only) {
    std::string model;
    if (scope == GTEXT_HTML_SERIALIZE_CHILDREN) {
      model_children(model, root, scripting);
    }
    else {
      model_node(model, target, nullptr, scripting);
    }
    if (model != full) {
      fail("the output is not the model's", "library: " + full + "\nmodel:   " + model, input);
    }
  }

  // ---- the counts
  if (scope == GTEXT_HTML_SERIALIZE_CHILDREN) {
    size_t max_depth = 0, nodes = 0;
    measure(root, 0, max_depth, nodes);
    if (r.nodes_visited != nodes) {
      fail("the nodes visited are not the nodes of the tree", std::to_string(r.nodes_visited) + " of " + std::to_string(nodes), input);
    }

    // ---- the token stream writes what the walk writes
    GTEXT_HTML_Status ts;
    size_t tdepth = 0, tnodes = 0;
    const size_t piece = (o3 >> 4) ? 1 + (o3 >> 4) : 0;
    const std::string streamed = serialize_tokens(root, o, piece, o3, &ts, &tdepth, &tnodes);
    if (ts != GTEXT_HTML_OK || streamed != full) {
      fail("the token stream does not write what the walk writes", "walk:   " + full + "\nstream: " + streamed + "\nstatus " + std::to_string(ts), input);
    }

    // ---- the limits
    if (max_depth >= 1) {
      Collect c1;
      GTEXT_Markup_Sink s1 = c1.sink();
      GTEXT_HTML_Serialize_Options d = o;
      d.max_depth = max_depth;
      if (gtext_html_serialize(root, scope, &d, &s1, nullptr) != GTEXT_HTML_OK || c1.data != full) {
        fail("a tree as deep as the limit was refused", "depth " + std::to_string(max_depth), input);
      }
      if (max_depth >= 2) {
        Collect c2;
        GTEXT_Markup_Sink s2 = c2.sink();
        d.max_depth = max_depth - 1;
        GTEXT_HTML_Status ds = d.max_depth == 0 ? GTEXT_HTML_E_LIMIT_DEPTH : gtext_html_serialize(root, scope, &d, &s2, nullptr);
        if (d.max_depth != 0 && (ds != GTEXT_HTML_E_LIMIT_DEPTH || c2.data.size() > full.size()
                || full.compare(0, c2.data.size(), c2.data) != 0)) {
          fail("a tree one level too deep for the limit was not refused with a prefix", "depth " + std::to_string(max_depth) + " status " + std::to_string(ds), input);
        }
      }
    }
    if (!full.empty() && (o5 & 0x80)) {
      // the output limit: exactly the length, one less, half, one more, and a few small ones
      const size_t n = full.size();
      const size_t picks[8] = {n, n - 1, n / 2, n + 1, 1, n > 1 ? n - 2 : 1, 16, n / 4};
      const size_t limit = picks[o5 & 7];
      Collect c3;
      GTEXT_Markup_Sink s3 = c3.sink();
      GTEXT_HTML_Serialize_Options l = o;
      l.max_output_bytes = limit;
      if (limit != 0) { // zero is the default, not "none"
        const GTEXT_HTML_Status ls = gtext_html_serialize(root, scope, &l, &s3, nullptr);
        if (limit >= n) {
          if (ls != GTEXT_HTML_OK || c3.data != full) {
            fail("an output exactly at the limit was refused", "limit " + std::to_string(limit), input);
          }
        }
        else if (ls != GTEXT_HTML_E_LIMIT_OUTPUT || c3.data.size() > limit || full.compare(0, c3.data.size(), c3.data) != 0) {
          fail("an output over the limit was not refused with a prefix", "limit " + std::to_string(limit) + " got " + std::to_string(c3.data.size()), input);
        }
      }
    }
  }

  // ---- the sink
  if (o3 & 15) {
    Collect c;
    // the first call to one past the last (which is never refused)
    c.fail_on = 1 + ((o3 & 15) - 1) % (1 + (full.size() + 4095) / 4096);
    GTEXT_Markup_Sink cs = c.sink();
    const GTEXT_HTML_Status fs = gtext_html_serialize(target, scope, &o, &cs, nullptr);
    if (c.fail_on <= (full.size() + 4095) / 4096) {
      if (fs != GTEXT_HTML_E_WRITE || c.calls_after_failure != 0 || c.data != full.substr(0, (c.fail_on - 1) * 4096)) {
        fail("a sink that refused was not handled", "call " + std::to_string(c.fail_on) + " status " + std::to_string(fs), input);
      }
    }
    else if (fs != GTEXT_HTML_OK || c.data != full) {
      fail("a sink that was never refused saw something else", "", input);
    }
  }

  // ---- idempotence and "the output is HTML"
  if (scope == GTEXT_HTML_SERIALIZE_CHILDREN && !fragment && !o.escape_rcdata && !o.strip_whitespace && !o.alphabetical_attributes
      && !o.minimize_boolean_attributes && !o.inject_meta_charset) {
    GTEXT_Markup_Document * back = nullptr;
    GTEXT_HTML_Parse_Options bo = gtext_html_parse_options_default();
    bo.scripting = scripting;
    bo.count_errors_only = true;
    bo.max_open_elements = 100000;
    const GTEXT_HTML_Status bs = gtext_html_parse(full.data(), full.size(), &bo, &back, nullptr);
    if (bs != GTEXT_HTML_OK) {
      fail("the output does not parse", std::string(gtext_html_status_string(bs)), input);
    }
    if (gtext_markup_equal(root, gtext_markup_document_root(back))) {
      Collect again;
      GTEXT_Markup_Sink as = again.sink();
      if (gtext_html_serialize(gtext_markup_document_root(back), scope, &o, &as, nullptr) != GTEXT_HTML_OK || again.data != full) {
        fail("an equal tree is written as other bytes", "first:  " + full + "\nsecond: " + again.data, input);
      }
    }
    gtext_markup_document_free(back);
  }

  // ---- OUTER: the start tag, the children, the end tag
  if (scope == GTEXT_HTML_SERIALIZE_OUTER && !o.omit_optional_tags && !o.inject_meta_charset && !o.strip_whitespace
      && gtext_markup_node_kind(target) == GTEXT_MARKUP_KIND_ELEMENT && !is_void(target)) {
    Collect inner;
    GTEXT_Markup_Sink is = inner.sink();
    if (gtext_html_serialize(target, GTEXT_HTML_SERIALIZE_CHILDREN, &o, &is, nullptr) == GTEXT_HTML_OK) {
      // the token stream told which element it is the contents of (gtext_html_serializer_context) writes the same
      GTEXT_HTML_Status cs2;
      size_t cd = 0, cn = 0;
      const std::string cstream = serialize_tokens(target, o, 0, o3, &cs2, &cd, &cn);
      if (cs2 != GTEXT_HTML_OK || cstream != inner.data) {
        fail("the token stream with a context does not write the children the walk writes", "walk:   " + inner.data + "\nstream: " + cstream, input);
      }
      const size_t open = full.find('>');
      const std::string tail = "</";
      // the outer text is: <start tag> inner </end tag>
      if (open == std::string::npos || full.size() < open + 1 + inner.data.size() || full.compare(open + 1, inner.data.size(), inner.data) != 0
          || full.compare(open + 1 + inner.data.size(), 2, tail) != 0) {
        // the children are written with the element's own context (raw text) in CHILDREN and the same in OUTER
        fail("the outer text is not the start tag, the children and the end tag", full + "\n" + inner.data, input);
      }
    }
  }

  // ---- the allocator
  if (o4 & 7) {
    for (int form = 0; form < 3; form++) {
      if (!(o4 & (1 << form))) {
        continue;
      }
      bool finished = false;
      // Allocation 301 is armed to fail nothing: a sweep that ends on the cap must still end on a run that is checked
      // against the normal answer, so that its tail is not a loop that stopped.
      for (size_t n = 1; n <= 301 && !finished; n++) {
        FaultAllocator fa;
        GTEXT_HTML_Serialize_Options fo = o;
        fo.allocator = &fa.alloc;
        std::string got;
        GTEXT_HTML_Status fst;
        fa.arm(n == 301 ? 0 : n);
        if (form == 0) {
          Collect c;
          GTEXT_Markup_Sink cs = c.sink();
          fst = gtext_html_serialize(target, scope, &fo, &cs, nullptr);
          got = c.data;
        }
        else if (form == 1) {
          char * buf = nullptr;
          size_t len = 0;
          fst = gtext_html_serialize_buffer(target, scope, &fo, &buf, &len, nullptr);
          if (fst == GTEXT_HTML_OK) {
            got.assign(buf, len);
            gtext_html_serialize_buffer_free(doc, &fo, buf);
          }
          else if (buf) {
            fail("a failed call left a buffer", "", input);
          }
        }
        else if (scope == GTEXT_HTML_SERIALIZE_CHILDREN) {
          size_t d = 0, nn = 0;
          got = serialize_tokens(root, fo, 3, o3, &fst, &d, &nn);
        }
        else {
          break;
        }
        fa.disarm();
        if (!fa.failed) {
          if (fst != GTEXT_HTML_OK || got != full) {
            fail("a run that failed no allocation differs from the normal answer", "", input);
          }
          finished = true;
        }
        else if (fst != GTEXT_HTML_E_OOM && !(fst == GTEXT_HTML_OK && got == full)) {
          fail("a failed allocation is neither out of memory nor the normal answer", "allocation " + std::to_string(n) + " status " + std::to_string(fst), input);
        }
        if (!fa.live.empty() || fa.bad_frees) {
          fail("an allocation failure leaked or freed what was not its own", "allocation " + std::to_string(n), input);
        }
      }
    }
  }
  gtext_markup_document_free(doc);
  return 0;
}
