/**
 * @file
 *
 * Shared by the HTML serializer tests: parsing to a tree, building a tree by hand, a sink that collects
 * (and can refuse the n-th call), and the serialization of a node to a string.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GTEXT_TESTS_HTML_SERIALIZE_TEST_SUPPORT_H
#define GHOTI_IO_GTEXT_TESTS_HTML_SERIALIZE_TEST_SUPPORT_H

#include <cstddef>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/html.h>

#include "markup_test_support.h"

namespace hs {

using Doc = GTEXT_Markup_Document;
using Node = GTEXT_Markup_Node;
using Opts = GTEXT_HTML_Serialize_Options;

inline Opts defaults() {
  return gtext_html_serialize_options_default();
}

/// A sink that collects, and can be told to refuse the n-th call.
struct Collect {
  std::string data;
  size_t calls = 0;
  size_t fail_on = 0; // 1-based; 0 never
  GTEXT_Markup_Status fail_with = GTEXT_MARKUP_E_WRITE;
  static GTEXT_Markup_Status write(void * user, const char * b, size_t n) {
    auto * self = static_cast<Collect *>(user);
    self->calls++;
    if (self->fail_on != 0 && self->calls == self->fail_on) {
      return self->fail_with;
    }
    self->data.append(b, n);
    return GTEXT_MARKUP_OK;
  }
  GTEXT_Markup_Sink sink() {
    return GTEXT_Markup_Sink{&Collect::write, this};
  }
};

/// A parsed document or fragment, freed with the test.
struct Parsed {
  Doc * doc = nullptr;
  Node * fragment = nullptr; // set for a fragment
  Parsed() = default;
  Parsed(const Parsed &) = delete;
  Parsed & operator=(const Parsed &) = delete;
  Parsed(Parsed && o) noexcept : doc(o.doc), fragment(o.fragment) {
    o.doc = nullptr;
    o.fragment = nullptr;
  }
  ~Parsed() {
    gtext_markup_document_free(doc);
  }
  /// What to serialize the children of: the document root, or the fragment.
  Node * top() const {
    return fragment ? fragment : gtext_markup_document_root(doc);
  }
};

inline Parsed parse_document(const std::string & html, bool scripting = false) {
  Parsed p;
  GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
  o.scripting = scripting;
  o.max_open_elements = 100000;
  GTEXT_HTML_Result r;
  std::memset(&r, 0, sizeof(r));
  EXPECT_EQ(gtext_html_parse(html.data(), html.size(), &o, &p.doc, &r), GTEXT_HTML_OK);
  gtext_html_result_free(&r);
  return p;
}

inline Parsed parse_fragment(const std::string & html, const char * context = "div", bool scripting = false) {
  Parsed p;
  GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
  o.scripting = scripting;
  GTEXT_HTML_Fragment_Context c;
  std::memset(&c, 0, sizeof(c));
  c.ns = GTEXT_HTML_NS_HTML;
  c.local_name = context;
  GTEXT_HTML_Result r;
  std::memset(&r, 0, sizeof(r));
  EXPECT_EQ(gtext_html_parse_fragment(&c, html.data(), html.size(), &o, &p.doc, &p.fragment, &r), GTEXT_HTML_OK);
  gtext_html_result_free(&r);
  return p;
}

/// The first element with this local name, in tree order, looking into template contents too.
inline Node * find(Node * from, const char * name) {
  if (!from) {
    return nullptr;
  }
  if (gtext_markup_node_kind(from) == GTEXT_MARKUP_KIND_ELEMENT && std::strcmp(gtext_markup_element_local_name(from), name) == 0) {
    return from;
  }
  if (Node * c = gtext_markup_element_template_content(from)) {
    if (Node * r = find(c, name)) {
      return r;
    }
  }
  for (Node * c = gtext_markup_node_first_child(from); c; c = gtext_markup_node_next_sibling(c)) {
    if (Node * r = find(c, name)) {
      return r;
    }
  }
  return nullptr;
}

struct Out {
  GTEXT_HTML_Status status;
  std::string bytes;
  GTEXT_HTML_Serialize_Result r;
};

inline Out serialize_with(const Node * n, GTEXT_HTML_Serialize_Scope scope, const Opts & o) {
  Out out;
  char * buf = nullptr;
  size_t len = 0;
  std::memset(&out.r, 0xAB, sizeof(out.r)); // the call must clear it
  out.status = gtext_html_serialize_buffer(n, scope, &o, &buf, &len, &out.r);
  if (out.status == GTEXT_HTML_OK) {
    out.bytes.assign(buf, len);
    EXPECT_EQ(buf[len], '\0');
    EXPECT_EQ(out.r.bytes_written, len);
    EXPECT_EQ(out.r.status, GTEXT_HTML_OK);
    gtext_html_serialize_buffer_free(gtext_markup_node_document(n), &o, buf);
  }
  else {
    EXPECT_EQ(buf, nullptr);
    EXPECT_EQ(out.r.status, out.status);
    EXPECT_NE(out.r.message, nullptr);
  }
  return out;
}

inline std::string children(const Node * n, const Opts & o = defaults()) {
  Out out = serialize_with(n, GTEXT_HTML_SERIALIZE_CHILDREN, o);
  EXPECT_EQ(out.status, GTEXT_HTML_OK);
  return out.bytes;
}

inline std::string outer(const Node * n, const Opts & o = defaults()) {
  Out out = serialize_with(n, GTEXT_HTML_SERIALIZE_OUTER, o);
  EXPECT_EQ(out.status, GTEXT_HTML_OK);
  return out.bytes;
}

/// A tree built by hand, with no limits.
struct Build {
  Doc * doc = nullptr;
  Build() {
    GTEXT_Markup_Options o = gtext_markup_options_default();
    o.max_depth = 0;
    o.max_attributes = 0;
    o.max_nodes = 0;
    EXPECT_EQ(gtext_markup_document_new(&o, &doc), GTEXT_MARKUP_OK);
  }
  explicit Build(const GTEXT_Allocator * a) {
    GTEXT_Markup_Options o = gtext_markup_options_default();
    o.max_depth = 0;
    o.max_nodes = 0;
    o.allocator = a;
    EXPECT_EQ(gtext_markup_document_new(&o, &doc), GTEXT_MARKUP_OK);
  }
  ~Build() {
    gtext_markup_document_free(doc);
  }
  Build(const Build &) = delete;
  Build & operator=(const Build &) = delete;

  Node * root() {
    return gtext_markup_document_root(doc);
  }
  /// A detached element; append it with add().
  Node * make(const char * local, const char * ns = GTEXT_MARKUP_NS_HTML, const char * prefix = nullptr) {
    Node * n = nullptr;
    EXPECT_EQ(gtext_markup_create_element(doc, ns, prefix, local, &n), GTEXT_MARKUP_OK);
    return n;
  }
  Node * el(Node * parent, const char * local, const char * ns = GTEXT_MARKUP_NS_HTML, const char * prefix = nullptr) {
    Node * n = make(local, ns, prefix);
    EXPECT_EQ(gtext_markup_append_child(parent ? parent : root(), n), GTEXT_MARKUP_OK);
    return n;
  }
  void attr(Node * e, const char * local, const std::string & value, const char * ns = nullptr,
      const char * prefix = nullptr) {
    EXPECT_EQ(gtext_markup_element_set_attribute(e, ns, prefix, local, value.data(), value.size()), GTEXT_MARKUP_OK);
  }
  Node * text(Node * parent, const std::string & s) {
    Node * n = nullptr;
    EXPECT_EQ(gtext_markup_create_text(doc, s.data(), s.size(), &n), GTEXT_MARKUP_OK);
    EXPECT_EQ(gtext_markup_append_child(parent, n), GTEXT_MARKUP_OK);
    return n;
  }
  Node * cdata(Node * parent, const std::string & s) {
    Node * n = nullptr;
    EXPECT_EQ(gtext_markup_create_cdata(doc, s.data(), s.size(), &n), GTEXT_MARKUP_OK);
    EXPECT_EQ(gtext_markup_append_child(parent, n), GTEXT_MARKUP_OK);
    return n;
  }
  Node * comment(Node * parent, const std::string & s) {
    Node * n = nullptr;
    EXPECT_EQ(gtext_markup_create_comment(doc, s.data(), s.size(), &n), GTEXT_MARKUP_OK);
    EXPECT_EQ(gtext_markup_append_child(parent ? parent : root(), n), GTEXT_MARKUP_OK);
    return n;
  }
  Node * pi(Node * parent, const std::string & target, const std::string & data) {
    Node * n = nullptr;
    EXPECT_EQ(gtext_markup_create_processing_instruction(doc, target.c_str(), data.data(), data.size(), &n),
        GTEXT_MARKUP_OK);
    EXPECT_EQ(gtext_markup_append_child(parent ? parent : root(), n), GTEXT_MARKUP_OK);
    return n;
  }
  Node * doctype(const char * name, const char * pub, const char * sys) {
    Node * n = nullptr;
    EXPECT_EQ(gtext_markup_create_doctype(doc, name, pub, sys, &n), GTEXT_MARKUP_OK);
    EXPECT_EQ(gtext_markup_append_child(root(), n), GTEXT_MARKUP_OK);
    return n;
  }
  Node * fragment() {
    Node * n = nullptr;
    EXPECT_EQ(gtext_markup_create_fragment(doc, &n), GTEXT_MARKUP_OK);
    return n;
  }
};

} // namespace hs

#endif // GHOTI_IO_GTEXT_TESTS_HTML_SERIALIZE_TEST_SUPPORT_H
