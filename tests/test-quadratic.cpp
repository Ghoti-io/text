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
 * The quadratic-nesting suite (`make check-html-quadratic`): the inputs that make a naive HTML parser,
 * serializer, decoder or sniffer quadratic, and the XML parser's DTD (declarations, references, entity
 * expansion, start tags against many declarations), each run at three sizes (n, 4n and 16n units) with the work
 * counted, and the growth held to a bound by ratio. No wall clock is read anywhere.
 *
 * **What is counted.** The tree builder's work and the tokenizer's (gtext_html_tree_work() and
 * gtext_html_tree_tokenizer_work(): tokens handled, stack entries walked, formatting elements reopened or
 * shifted, characters decoded, look-ahead steps), the serializer's (gtext_html_serialize_work(): nodes,
 * attributes, bytes examined and written, sort steps), the Encoding module's `steps` (handler steps, the
 * figure its header documents as at most four per byte for a decoder and three for an encoder) and the
 * sniffer's (html_prescan_counted()), and the XML parser's `max_total_work` counter (xml_parse_counted()). A
 * counter sees only what its author put it beside, so
 * `tools/html/quadratic-instructions.py` records the instructions the same shapes execute under callgrind
 * and compares the growth of the two: a counter that stops tracking the machine's work shows there (not a
 * gate: the instruction counts depend on the toolchain, and the counters do not).
 *
 * **The bound.** For each shape the work at 4n over the work at n, and at 16n over 4n, must stay below the
 * shape's `bound`: a linear shape grows by 4 (less, while a constant is amortised), a quadratic one by 16.
 * The default is 5.0, which a quadratic shape cannot meet at any size here (the smallest n is chosen so
 * that the quadratic term dominates the constant at n), and a shape with a larger bound says why in its
 * `note`. A shape also has an absolute bound, `per_byte`: its work may not exceed that many units per
 * byte of input plus a constant, which is the documented bound (GTEXT_HTML_TOKENIZER_WORK_BOUND and
 * GTEXT_HTML_TREE_WORK_BOUND) or the module's own. The shapes whose cost depends on a limit and not on the
 * input (a document past the open-element limit, a tag past the attribute limit, the sniffer's 1024-byte
 * window) are `flat`: the work at 16n may not exceed 1.5 times the work at n.
 *
 * **Controls.** `TheSuiteFailsOnAQuadraticShape` and the others of `QuadraticControls` hand the checker
 * measures that grow quadratically, by a bound's margin, and in the other direction; `make
 * check-html-quadratic-plants` compiles the library with a defect planted in each layer and requires this
 * program to fail.
 *
 * Environment: `GTEXT_QUAD_REPORT=<file>` writes a tab-separated line per shape (name, bytes and work at
 * n, 4n and 16n); `GTEXT_QUAD_ONE=<name>:<units>` makes `Quadratic.RunOne` run one shape once (what the
 * callgrind tool runs).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/css.h>
#include <ghoti.io/text/css/css_function.h>
#include <ghoti.io/text/encoding.h>
#include <ghoti.io/text/html.h>
#include <ghoti.io/text/xml.h>

#include "../src/html/html_sniff_internal.h"
#include "../src/html/html_tree_internal.h"

// (src/html/html_api_internal.h has a flexible array member, which C++ forbids: the one function of it that is used is declared here.)
extern "C" GTEXT_HTML_Status gtext_html_serialize_work(const GTEXT_Markup_Node * node, GTEXT_HTML_Serialize_Scope scope,
    const GTEXT_HTML_Serialize_Options * opts, size_t * work);
// (src/css/css_internal.h has a flexible array member too: the two work counters of the CSS module.)
extern "C" GTEXT_CSS_Status gtext_css_parse_work(GTEXT_CSS_Tree_Kind entry, const char * input, size_t length,
    const GTEXT_CSS_Parse_Options * options, size_t * work);
extern "C" GTEXT_CSS_Status gtext_css_stream_work(GTEXT_CSS_Tree_Kind kind, const void * data, size_t length, size_t chunk,
    const GTEXT_CSS_Bytes_Options * bytes_options, const GTEXT_CSS_Parse_Options * options, size_t * work);
extern "C" GTEXT_CSS_Status gtext_css_parse_bytes_work(GTEXT_CSS_Tree_Kind kind, const void * bytes, size_t length,
    const GTEXT_CSS_Bytes_Options * bytes_options, const GTEXT_CSS_Parse_Options * options, size_t * work);
extern "C" GTEXT_CSS_Status gtext_css_serialize_work(const GTEXT_CSS_Tree * tree, const GTEXT_CSS_Serialize_Options * options,
    const GTEXT_Markup_Sink * sink, size_t * work);
// (src/css/css_registry_internal.h: the registry's work counter, and a lookup that adds its work to one.)
extern "C" size_t css_registry_work(const GTEXT_CSS_Registry * registry);
extern "C" GTEXT_CSS_Property_Id css_registry_find_counted(const GTEXT_CSS_Registry * registry, const char * name, size_t length, size_t * work);
// (src/xml/xml_internal.h is C: the counted parse it declares for this suite is declared here.)
extern "C" GTEXT_XML_Status xml_parse_counted(const void * data, size_t len, const GTEXT_XML_Parse_Options * opts,
    GTEXT_Markup_Document ** out, GTEXT_XML_Error * err, size_t * work);

namespace {

std::string repeat(const std::string & unit, size_t k) {
  std::string out;
  out.reserve(unit.size() * k);
  for (size_t i = 0; i < k; i++) {
    out += unit;
  }
  return out;
}

// What one run of a shape measured.
struct Measured {
  size_t work = 0;    // the shape's counter(s), summed
  size_t bytes = 0;   // the input's size
  bool ok = true;     // the run ended as the shape expects
  std::string why;    // when it did not
};

struct Shape {
  Shape(std::string g, std::string nm, size_t units, std::function<Measured(size_t)> r)
      : group(std::move(g)), name(std::move(nm)), n(units), run(std::move(r)) {}
  std::string group;
  std::string name;
  size_t n;                                // the smallest size, in the shape's units
  std::function<Measured(size_t)> run;     // units -> measurement
  double bound = 5.0;                      // the most the work may grow for 4x the units
  double per_byte = 112.0;                 // the most work per byte of input (plus 8192)
  bool flat = false;                       // the work is independent of the input (a limit decides it)
  std::string note;                        // why a bound other than the default
};

// ---- the tree builder ---------------------------------------------------------------------------

struct Ctx {
  GTEXT_HTML_Namespace ns = GTEXT_HTML_NS_HTML;
  std::string name;
  std::vector<std::pair<std::string, std::string>> attrs;
};

struct TreeRun {
  std::string input;
  const Ctx * ctx = nullptr;
  size_t max_open = 100000000; // "no limit to speak of" unless the shape is about the limit
  size_t max_attributes = 100000000;
  bool expect_ok = true;
  GTEXT_HTML_TreeStatus expect_status = GTEXT_HTML_TREE_OK;
  bool scripting = false;
};

Measured tree_run(const TreeRun & r) {
  Measured m;
  m.bytes = r.input.size();
  GTEXT_HTML_TreeOptions o;
  std::memset(&o, 0, sizeof(o));
  o.max_open_elements = r.max_open;
  o.max_nodes = 1000000000;
  o.max_attributes = r.max_attributes;
  o.max_name_bytes = (size_t)1 << 30; // a name as long as the run asks for: the limit is not what is measured
  o.max_value_bytes = (size_t)1 << 30;
  o.max_comment_bytes = (size_t)1 << 30;
  o.max_text_bytes = (size_t)1 << 30;
  o.scripting = r.scripting;
  o.count_errors_only = true;
  GTEXT_HTML_Tree * tree = nullptr;
  std::vector<char *> owned;
  if (r.ctx) {
    GTEXT_HTML_FragmentContext fc;
    std::memset(&fc, 0, sizeof(fc));
    fc.ns = r.ctx->ns;
    char * name = static_cast<char *>(std::malloc(r.ctx->name.size() + 1));
    std::memcpy(name, r.ctx->name.c_str(), r.ctx->name.size() + 1);
    owned.push_back(name);
    fc.local_name = name;
    fc.local_name_length = r.ctx->name.size();
    std::vector<GTEXT_HTML_Attribute> attrs(r.ctx->attrs.size());
    for (size_t i = 0; i < r.ctx->attrs.size(); i++) {
      char * a = static_cast<char *>(std::malloc(r.ctx->attrs[i].first.size() + 1));
      char * v = static_cast<char *>(std::malloc(r.ctx->attrs[i].second.size() + 1));
      std::memcpy(a, r.ctx->attrs[i].first.c_str(), r.ctx->attrs[i].first.size() + 1);
      std::memcpy(v, r.ctx->attrs[i].second.c_str(), r.ctx->attrs[i].second.size() + 1);
      owned.push_back(a);
      owned.push_back(v);
      std::memset(&attrs[i], 0, sizeof(attrs[i]));
      attrs[i].name = a;
      attrs[i].name_length = r.ctx->attrs[i].first.size();
      attrs[i].value = v;
      attrs[i].value_length = r.ctx->attrs[i].second.size();
    }
    fc.attributes = attrs.empty() ? nullptr : attrs.data();
    fc.attribute_count = attrs.size();
    fc.scripting = r.scripting;
    GTEXT_HTML_TreeStatus st = GTEXT_HTML_TREE_OK;
    tree = gtext_html_tree_new_fragment(&o, &fc, &st);
  }
  else {
    tree = gtext_html_tree_new(&o);
  }
  for (char * p : owned) {
    std::free(p);
  }
  if (!tree) {
    m.ok = false;
    m.why = "no tree builder";
    return m;
  }
  GTEXT_HTML_TreeStatus s = gtext_html_tree_feed(tree, r.input.data(), r.input.size());
  if (s == GTEXT_HTML_TREE_OK) {
    s = gtext_html_tree_finish(tree);
  }
  m.work = gtext_html_tree_work(tree) + gtext_html_tree_tokenizer_work(tree);
  if (s != r.expect_status) {
    m.ok = false;
    m.why = "the parse ended with status " + std::to_string((int)s) + ", not " + std::to_string((int)r.expect_status);
  }
  if (s == GTEXT_HTML_TREE_OK) {
    if (r.ctx) {
      GTEXT_Markup_Document * doc = nullptr;
      gtext_html_tree_take_fragment(tree, &doc);
      gtext_markup_document_free(doc);
    }
    else {
      gtext_markup_document_free(gtext_html_tree_take_document(tree));
    }
  }
  gtext_html_tree_free(tree);
  return m;
}

std::function<Measured(size_t)> doc(std::function<std::string(size_t)> make, size_t max_open = 100000000) {
  return [=](size_t k) {
    TreeRun r;
    r.input = make(k);
    r.max_open = max_open;
    return tree_run(r);
  };
}

std::function<Measured(size_t)> frag(Ctx ctx, std::function<std::string(size_t)> make) {
  return [=](size_t k) {
    TreeRun r;
    r.input = make(k);
    r.ctx = &ctx;
    return tree_run(r);
  };
}

// A parse that must stop at a limit: the work may not depend on how much input follows.
std::function<Measured(size_t)> limited(std::function<std::string(size_t)> make, size_t max_open, size_t max_attributes,
    GTEXT_HTML_TreeStatus status) {
  return [=](size_t k) {
    TreeRun r;
    r.input = make(k);
    r.max_open = max_open;
    r.max_attributes = max_attributes;
    r.expect_status = status;
    return tree_run(r);
  };
}

// ---- the public parser, the serializer ------------------------------------------------------------

// Parse `html` (a document, or a fragment in a div) with the public API, to a tree with no limit in the way.
GTEXT_Markup_Document * parse_public(const std::string & html, bool fragment, GTEXT_Markup_Node ** top) {
  GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
  o.max_open_elements = 100000000;
  o.max_nodes = 1000000000;
  o.max_attributes = 100000000;
  o.max_input_bytes = SIZE_MAX;
  GTEXT_Markup_Document * d = nullptr;
  GTEXT_HTML_Result res;
  std::memset(&res, 0, sizeof(res));
  GTEXT_HTML_Status st;
  if (fragment) {
    GTEXT_HTML_Fragment_Context c;
    std::memset(&c, 0, sizeof(c));
    c.ns = GTEXT_HTML_NS_HTML;
    c.local_name = "div";
    st = gtext_html_parse_fragment(&c, html.data(), html.size(), &o, &d, top, &res);
  }
  else {
    st = gtext_html_parse(html.data(), html.size(), &o, &d, &res);
    if (top && d) {
      *top = const_cast<GTEXT_Markup_Node *>(gtext_markup_document_root(d));
    }
  }
  gtext_html_result_free(&res);
  if (st != GTEXT_HTML_OK) {
    return nullptr;
  }
  return d;
}

enum class SerializeOptions { Default, Omit, Everything, Alphabetical, Strip };

Measured serialize_run(const std::string & html, bool fragment, SerializeOptions which) {
  Measured m;
  m.bytes = html.size();
  GTEXT_Markup_Node * top = nullptr;
  GTEXT_Markup_Document * d = parse_public(html, fragment, &top);
  if (!d || !top) {
    m.ok = false;
    m.why = "the input did not parse";
    gtext_markup_document_free(d);
    return m;
  }
  GTEXT_HTML_Serialize_Options o = gtext_html_serialize_options_default();
  o.max_depth = 100000000;
  o.max_output_bytes = SIZE_MAX;
  switch (which) {
    case SerializeOptions::Default: break;
    case SerializeOptions::Omit: o.omit_optional_tags = true; break;
    case SerializeOptions::Alphabetical: o.alphabetical_attributes = true; break;
    case SerializeOptions::Strip: o.strip_whitespace = true; break;
    case SerializeOptions::Everything:
      o.omit_optional_tags = true;
      o.minimize_boolean_attributes = true;
      o.use_best_quote_char = true;
      o.quote_attr_values = GTEXT_HTML_QUOTE_WHEN_NEEDED;
      o.alphabetical_attributes = true;
      o.strip_whitespace = true;
      o.escape_rcdata = true;
      o.inject_meta_charset = true;
      o.encoding = "utf-8";
      break;
  }
  size_t work = 0;
  const GTEXT_HTML_Serialize_Scope scope = fragment ? GTEXT_HTML_SERIALIZE_CHILDREN : GTEXT_HTML_SERIALIZE_CHILDREN;
  const GTEXT_HTML_Status st = gtext_html_serialize_work(top, scope, &o, &work);
  m.work = work;
  if (st != GTEXT_HTML_OK) {
    m.ok = false;
    m.why = std::string("the serializer ended with ") + gtext_html_status_string(st);
  }
  gtext_markup_document_free(d);
  return m;
}

std::function<Measured(size_t)> ser(std::function<std::string(size_t)> make, bool fragment = true,
    SerializeOptions which = SerializeOptions::Default) {
  return [=](size_t k) { return serialize_run(make(k), fragment, which); };
}

// ---- the public parser, by the memory it asks for ---------------------------------------------------------

// An allocator that adds up what is asked of it: the bytes requested by malloc, calloc and realloc (a realloc may copy as much as
// the size it is given) and the calls. A copy is work no counter of the builder sees (the text node that a builder remembered the
// capacity of wrongly, 3d), and a parser that grows a buffer by a constant instead of a factor shows here as a sum that grows by 16,
// not 4, for 4 times the input.
struct CountingAllocator {
  GTEXT_Allocator alloc;
  size_t bytes = 0;
  size_t calls = 0;
  CountingAllocator() {
    alloc.ctx = this;
    alloc.malloc_fn = [](void * c, size_t n) -> void * {
      auto * self = static_cast<CountingAllocator *>(c);
      self->bytes += n;
      self->calls++;
      return std::malloc(n ? n : 1);
    };
    alloc.calloc_fn = [](void * c, size_t m, size_t n) -> void * {
      auto * self = static_cast<CountingAllocator *>(c);
      if (n != 0 && m > SIZE_MAX / n) {
        return nullptr;
      }
      self->bytes += m * n;
      self->calls++;
      return std::calloc(m ? m : 1, n ? n : 1);
    };
    alloc.realloc_fn = [](void * c, void * p, size_t n) -> void * {
      auto * self = static_cast<CountingAllocator *>(c);
      self->bytes += n;
      self->calls++;
      return std::realloc(p, n ? n : 1);
    };
    alloc.free_fn = [](void *, void * p) { std::free(p); };
  }
};

struct ApiRun {
  std::string input;
  size_t chunk = 0;                  // 0: gtext_html_parse(); else the incremental parser fed this many bytes at a time
  size_t max_errors = 0;             // 0: the default
  const char * transport = nullptr;
  GTEXT_HTML_Encoding_Certainty certainty = GTEXT_HTML_CERTAIN;
  const char * default_encoding = nullptr;
  bool fragment = false;
};

Measured api_run(const ApiRun & r) {
  Measured m;
  m.bytes = r.input.size();
  CountingAllocator counting;
  GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
  o.allocator = &counting.alloc;
  o.max_open_elements = 100000000;
  o.max_nodes = 1000000000;
  o.max_attributes = 100000000;
  o.max_name_bytes = (size_t)1 << 30;
  o.max_attribute_bytes = (size_t)1 << 30;
  o.max_comment_bytes = (size_t)1 << 30;
  o.max_text_bytes = (size_t)1 << 30;
  o.max_input_bytes = SIZE_MAX;
  o.max_reparse_bytes = SIZE_MAX;
  if (r.max_errors) {
    o.max_errors = r.max_errors;
  }
  o.transport_encoding = r.transport;
  o.transport_encoding_certainty = r.certainty;
  o.default_encoding = r.default_encoding;
  GTEXT_Markup_Document * d = nullptr;
  GTEXT_HTML_Result res;
  std::memset(&res, 0, sizeof(res));
  GTEXT_HTML_Status st = GTEXT_HTML_OK;
  GTEXT_HTML_Fragment_Context fc;
  std::memset(&fc, 0, sizeof(fc));
  fc.local_name = "div";
  GTEXT_Markup_Node * frag = nullptr;
  if (r.chunk == 0) {
    st = r.fragment ? gtext_html_parse_fragment(&fc, r.input.data(), r.input.size(), &o, &d, &frag, &res)
                    : gtext_html_parse(r.input.data(), r.input.size(), &o, &d, &res);
  }
  else {
    GTEXT_HTML_Parser * p = nullptr;
    st = r.fragment ? gtext_html_parser_new_fragment(&fc, &o, &p) : gtext_html_parser_new(&o, &p);
    for (size_t at = 0; st == GTEXT_HTML_OK && at < r.input.size(); at += r.chunk) {
      st = gtext_html_parser_feed(p, r.input.data() + at, std::min(r.chunk, r.input.size() - at));
    }
    if (st == GTEXT_HTML_OK) {
      st = r.fragment ? gtext_html_parser_finish_fragment(p, &d, &frag, &res) : gtext_html_parser_finish(p, &d, &res);
    }
    gtext_html_parser_free(p);
  }
  m.work = counting.bytes + 16 * counting.calls;
  if (st != GTEXT_HTML_OK) {
    m.ok = false;
    m.why = std::string("the parse ended with ") + gtext_html_status_string(st);
  }
  gtext_markup_document_free(d);
  gtext_html_result_free(&res);
  return m;
}

std::function<Measured(size_t)> api(std::function<std::string(size_t)> make, size_t chunk, size_t max_errors = 0, bool fragment = false) {
  return [=](size_t k) {
    ApiRun r;
    r.input = make(k);
    r.chunk = chunk;
    r.max_errors = max_errors;
    r.fragment = fragment;
    return api_run(r);
  };
}

// ---- the Encoding module -----------------------------------------------------------------------------

struct Counter {
  size_t bytes = 0;
};

bool count_write(void * user, const char *, size_t len) {
  static_cast<Counter *>(user)->bytes += len;
  return true;
}

Measured decode_run(GTEXT_Encoding e, const std::string & in, size_t chunk) {
  Measured m;
  m.bytes = in.size();
  GTEXT_Decoder_Options o = gtext_decoder_options_default();
  o.max_output_bytes = SIZE_MAX;
  GTEXT_Decoder * dec = nullptr;
  if (gtext_decoder_new(e, &o, &dec) != GTEXT_ENCODING_OK) {
    m.ok = false;
    m.why = "no decoder";
    return m;
  }
  Counter c;
  const GTEXT_Encoding_Sink sink = {count_write, &c};
  size_t steps = 0;
  for (size_t at = 0; at < in.size() || at == 0; at += chunk) {
    const size_t n = std::min(chunk, in.size() - at);
    const bool last = at + n >= in.size();
    GTEXT_Decoder_Result r;
    const GTEXT_Encoding_Status st = gtext_decoder_decode(dec, in.data() + at, n, last, &sink, &r);
    steps += r.steps;
    if (st != GTEXT_ENCODING_OK) {
      m.ok = false;
      m.why = "the decoder ended with status " + std::to_string((int)st);
      break;
    }
    if (last) {
      break;
    }
  }
  m.work = steps;
  gtext_decoder_free(dec);
  return m;
}

Measured encode_run(GTEXT_Encoding e, const std::string & utf8, bool references) {
  Measured m;
  m.bytes = utf8.size();
  GTEXT_Encoder_Options o = gtext_encoder_options_default();
  o.max_output_bytes = SIZE_MAX;
  o.error_mode = references ? GTEXT_ENCODER_HTML : GTEXT_ENCODER_REPLACEMENT;
  GTEXT_Encoder * enc = nullptr;
  if (gtext_encoder_new(e, &o, &enc) != GTEXT_ENCODING_OK) {
    m.ok = false;
    m.why = "no encoder";
    return m;
  }
  Counter c;
  const GTEXT_Encoding_Sink sink = {count_write, &c};
  GTEXT_Encoder_Result r;
  const GTEXT_Encoding_Status st = gtext_encoder_encode(enc, utf8.data(), utf8.size(), true, &sink, &r);
  m.work = r.steps;
  if (st != GTEXT_ENCODING_OK) {
    m.ok = false;
    m.why = "the encoder ended with status " + std::to_string((int)st);
  }
  gtext_encoder_free(enc);
  return m;
}

Measured sniff_run(const std::string & in) {
  Measured m;
  m.bytes = in.size();
  size_t work = 0;
  html_prescan_counted(reinterpret_cast<const unsigned char *>(in.data()), in.size(), &work);
  m.work = work;
  // ... and the public decision (it reads the same window), which must not fail on the input.
  GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
  GTEXT_HTML_Sniff_Result r;
  if (gtext_html_sniff_encoding(in.data(), in.size(), &o, &r) != GTEXT_HTML_OK) {
    m.ok = false;
    m.why = "the sniffer failed";
  }
  return m;
}

// ---- CSS Syntax -----------------------------------------------------------------------------------------

// A CSS parse at the entry point `kind`, with every limit out of the way (the shapes measure the parser, not
// a limit), counted by gtext_css_parse_work(): code points read and looked at, tokens visited, values copied.
bool css_accept_rule(void *, const GTEXT_CSS_Parent *, const GTEXT_CSS_Rule *) {
  return true;
}
bool css_refuse_rule(void *, const GTEXT_CSS_Parent *, const GTEXT_CSS_Rule *) {
  return false;
}
// `validity`: 0 for no callbacks, 1 for a rule callback that accepts everything, 2 for one that refuses every rule (the
// parent it is given is made for each rule, and a walk over the parser's frames to make it was once quadratic in the depth).
std::function<Measured(size_t)> css(GTEXT_CSS_Tree_Kind kind, std::function<std::string(size_t)> make, int validity = 0) {
  return [kind, make, validity](size_t k) {
    Measured m;
    const std::string in = make(k);
    m.bytes = in.size();
    GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
    o.max_depth = (size_t)1 << 30;
    o.max_tokens = (size_t)1 << 30;
    o.max_rules = (size_t)1 << 30;
    o.max_declarations = (size_t)1 << 30;
    o.max_total_errors = (size_t)1 << 30;
    o.max_input_bytes = (size_t)1 << 30;
    o.count_errors_only = true;
    if (validity) {
      o.validity.rule = validity == 1 ? css_accept_rule : css_refuse_rule;
    }
    const GTEXT_CSS_Status st = gtext_css_parse_work(kind, in.data(), in.size(), &o, &m.work);
    if (st != GTEXT_CSS_OK) {
      m.ok = false;
      m.why = std::string("the parse failed: ") + gtext_css_status_string(st);
    }
    return m;
  };
}

// ---- CSS Value Definition Syntax --------------------------------------------------------------------------

// A grammar matched against `make(k)` with the default limits out of the way, counted by the match's own work counter
// (GTEXT_CSS_Match_Result::work): the positions looked at, sets joined, frames run and values made, extraction included.
std::function<Measured(size_t)> css_grammar(const std::string & grammar, std::function<std::string(size_t)> make, bool expect_match = true,
    bool function_types = false) {
  return [grammar, make, expect_match, function_types](size_t k) {
    Measured m;
    GTEXT_CSS_Function_Types * types = nullptr;
    if (function_types && gtext_css_function_types_new(nullptr, &types) != GTEXT_CSS_OK) {
      m.ok = false;
      m.why = "the function types could not be made";
      return m;
    }
    const std::string in = make(k);
    m.bytes = in.size();
    GTEXT_CSS_Compiled * c = nullptr;
    GTEXT_CSS_Status st = gtext_css_compile(grammar.data(), grammar.size(), function_types ? gtext_css_function_types_resolve : nullptr, types,
        nullptr, &c, nullptr);
    if (st != GTEXT_CSS_OK) {
      m.ok = false;
      m.why = std::string("the grammar did not compile: ") + gtext_css_status_string(st);
      gtext_css_function_types_free(types);
      return m;
    }
    GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
    po.max_depth = (size_t)1 << 30;
    po.max_tokens = (size_t)1 << 30;
    po.max_input_bytes = (size_t)1 << 30;
    GTEXT_CSS_Tree * tree = nullptr;
    st = gtext_css_parse_component_values(in.data(), in.size(), &po, &tree, nullptr);
    if (st != GTEXT_CSS_OK) {
      m.ok = false;
      m.why = std::string("the parse failed: ") + gtext_css_status_string(st);
      gtext_css_compiled_free(c);
      gtext_css_function_types_free(types);
      return m;
    }
    GTEXT_CSS_Match_Options mo = gtext_css_match_options_default();
    mo.max_match_work = (size_t)1 << 40;
    mo.max_memo_entries = (size_t)1 << 30;
    mo.max_math_depth = 65536;
    mo.max_math_nodes = (size_t)1 << 24;
    mo.max_color_nesting = 4096;
    mo.max_function_nodes = (size_t)1 << 30;
    bool matched = false;
    GTEXT_CSS_Value_Set * set = nullptr;
    GTEXT_CSS_Match_Result r;
    st = gtext_css_match(c, tree->values, tree->value_count, &mo, &matched, &set, &r);
    m.work = r.work;
    if (st != GTEXT_CSS_OK) {
      m.ok = false;
      m.why = std::string("the match failed: ") + gtext_css_status_string(st);
    }
    else if (matched != expect_match) {
      m.ok = false;
      m.why = "the match ended as it should not";
    }
    gtext_css_value_set_free(set);
    gtext_css_tree_free(tree);
    gtext_css_compiled_free(c);
    gtext_css_function_types_free(types);
    return m;
  };
}

// A math function or a color read on its own: the work its result reports (component values looked at, nodes made and examined; colors made).
std::function<Measured(size_t)> css_value_read(bool color, std::function<std::string(size_t)> make) {
  return [color, make](size_t k) {
    Measured m;
    const std::string in = make(k);
    m.bytes = in.size();
    GTEXT_CSS_Parse_Options po = gtext_css_parse_options_default();
    po.max_depth = (size_t)1 << 30;
    po.max_tokens = (size_t)1 << 30;
    po.max_input_bytes = (size_t)1 << 30;
    GTEXT_CSS_Tree * tree = nullptr;
    GTEXT_CSS_Status st = gtext_css_parse_component_values(in.data(), in.size(), &po, &tree, nullptr);
    if (st != GTEXT_CSS_OK || tree->value_count != 1) {
      m.ok = false;
      m.why = std::string("the parse failed: ") + gtext_css_status_string(st);
      gtext_css_tree_free(tree);
      return m;
    }
    bool valid = false;
    if (color) {
      GTEXT_CSS_Color_Options o = gtext_css_color_options_default();
      o.max_nesting = 4096;
      o.max_math_depth = 65536;
      o.max_math_nodes = (size_t)1 << 24;
      GTEXT_CSS_Color * c = nullptr;
      GTEXT_CSS_Color_Result r{};
      st = gtext_css_color_parse(&tree->values[0], &o, &valid, &c, &r);
      m.work = r.work;
      gtext_css_color_free(c);
    }
    else {
      GTEXT_CSS_Math_Options o = gtext_css_math_options_default();
      o.max_depth = 65536;
      o.max_nodes = (size_t)1 << 24;
      GTEXT_CSS_Math * c = nullptr;
      GTEXT_CSS_Math_Result r{};
      st = gtext_css_math_parse(&tree->values[0], nullptr, &o, &valid, &c, &r);
      m.work = r.work;
      gtext_css_math_free(c);
    }
    if (st != GTEXT_CSS_OK || !valid) {
      m.ok = false;
      m.why = st != GTEXT_CSS_OK ? std::string("the read failed: ") + gtext_css_status_string(st) : "the value is not valid";
    }
    gtext_css_tree_free(tree);
    return m;
  };
}

// Linking a chain of n types, each of which can match nothing and refers to the next: the work of the link (the result's
// work), which a nullable fixed point that settles one level of the chain per pass makes quadratic.
std::function<Measured(size_t)> css_link_chain() {
  return [](size_t n) {
    Measured m;
    std::map<std::string, GTEXT_CSS_Definition *> defs;
    for (size_t i = 0; i < n; i++) {
      const std::string text = i + 1 < n ? "<t" + std::to_string(i + 1) + ">? a?" : "a?";
      GTEXT_CSS_Definition * d = nullptr;
      GTEXT_CSS_Grammar_Options o = gtext_css_grammar_options_default();
      o.max_definition_nodes = (size_t)1 << 24;
      if (gtext_css_definition_parse(text.data(), text.size(), &o, &d, nullptr) != GTEXT_CSS_OK) {
        m.ok = false;
        m.why = "a definition did not parse";
        return m;
      }
      defs["t" + std::to_string(i)] = d;
    }
    auto resolve = [](void * user, GTEXT_CSS_Ref_Kind, const char * name, size_t length) -> const GTEXT_CSS_Definition * {
      auto * table = static_cast<std::map<std::string, GTEXT_CSS_Definition *> *>(user);
      auto it = table->find(std::string(name, length));
      return it == table->end() ? nullptr : it->second;
    };
    const std::string root = "<t0>";
    GTEXT_CSS_Grammar_Options o = gtext_css_grammar_options_default();
    o.max_definition_nodes = (size_t)1 << 24;
    o.max_definition_bytes = (size_t)1 << 28;
    GTEXT_CSS_Compiled * c = nullptr;
    GTEXT_CSS_Grammar_Result r;
    const GTEXT_CSS_Status st = gtext_css_compile(root.data(), root.size(), resolve, &defs, &o, &c, &r);
    m.work = r.work;
    m.bytes = n;
    if (st != GTEXT_CSS_OK) {
      m.ok = false;
      m.why = std::string("the link failed: ") + gtext_css_status_string(st);
    }
    gtext_css_compiled_free(c);
    for (auto & kv : defs) {
      gtext_css_definition_free(kv.second);
    }
    return m;
  };
}

// The CSS registry: n registrations (or lookups) of one shape, counted by css_registry_work() (hash probes, search steps, bytes of names
// folded, hashed and copied). `kind`: 0 distinct short names; 1 names that share a 100-byte prefix; 2 a chain of n overrides of one name;
// 3 n aliases of one property; 4 n shorthands of six longhands each; 5 n lookups of names the registry lacks, in a registry of n; 6 n
// lookups of names it has.
std::function<Measured(size_t)> css_registry_shape(int kind) {
  return [kind](size_t n) {
    Measured m;
    GTEXT_CSS_Registry_Builder * b = nullptr;
    GTEXT_CSS_Registry_Options ro = gtext_css_registry_options_default();
    ro.max_registrations = (size_t)1 << 20;
    if (gtext_css_registry_builder_new(&ro, &b) != GTEXT_CSS_OK) {
      m.ok = false;
      m.why = "no builder";
      return m;
    }
    const std::string prefix(100, 'p');
    std::vector<std::string> names;
    auto name_of = [&](size_t i) {
      return (kind == 1 ? "vp" + prefix : std::string("vp")) + std::to_string(i);
    };
    const char * six[] = {"width", "height", "color", "top", "left", "right"};
    for (size_t i = 0; i < n; i++) {
      GTEXT_CSS_Registry_Result res;
      GTEXT_CSS_Status st = GTEXT_CSS_OK;
      const std::string name = kind == 2 ? "vo" : name_of(i);
      if (kind == 3) {
        st = gtext_css_registry_register_alias(b, name.c_str(), "width", false, &res);
      }
      else if (kind == 4) {
        GTEXT_CSS_Shorthand_Def sh;
        std::memset(&sh, 0, sizeof sh);
        sh.name = name.c_str();
        sh.grammar = "<'width'>{1,6}";
        sh.longhands = six;
        sh.longhand_count = 6;
        st = gtext_css_registry_register_shorthand(b, &sh, &res);
      }
      else {
        GTEXT_CSS_Property_Def d;
        std::memset(&d, 0, sizeof d);
        d.name = name.c_str();
        d.grammar = "a | b";
        d.initial = "a";
        d.override = kind == 2;
        st = gtext_css_registry_register_property(b, &d, &res);
      }
      if (st != GTEXT_CSS_OK) {
        m.ok = false;
        m.why = std::string("a registration failed: ") + gtext_css_status_string(st);
        gtext_css_registry_builder_free(b);
        return m;
      }
      m.bytes += name.size();
      names.push_back(name);
    }
    GTEXT_CSS_Registry * r = nullptr;
    GTEXT_CSS_Status st = gtext_css_registry_freeze(b, &r, nullptr);
    if (st != GTEXT_CSS_OK) {
      m.ok = false;
      m.why = std::string("the freeze failed: ") + gtext_css_status_string(st);
      gtext_css_registry_builder_free(b);
      return m;
    }
    m.work = css_registry_work(r);
    if (kind == 5 || kind == 6) {
      m.work = 0;
      for (size_t i = 0; i < n; i++) {
        const std::string q = kind == 5 ? name_of(i) + "x" : names[i];
        const GTEXT_CSS_Property_Id id = css_registry_find_counted(r, q.data(), q.size(), &m.work);
        if ((id != 0) != (kind == 6)) {
          m.ok = false;
          m.why = "a lookup answered wrongly";
        }
        m.bytes += q.size();
      }
    }
    gtext_css_registry_free(r);
    return m;
  };
}

// Parsing a value for a property of the pin, counted by the match's work: n items of a comma list, n tracks, n tokens of a custom property.
std::function<Measured(size_t)> css_property_value(const char * property, std::function<std::string(size_t)> make,
    GTEXT_CSS_Property_Outcome expected = GTEXT_CSS_PROP_RESULT_VALID) {
  return [property, make, expected](size_t n) {
    Measured m;
    static GTEXT_CSS_Registry * r = [] {
      GTEXT_CSS_Registry * made = nullptr;
      gtext_css_registry_new(nullptr, &made);
      return made;
    }();
    const std::string in = make(n);
    m.bytes = in.size();
    GTEXT_CSS_Match_Options mo = gtext_css_match_options_default();
    mo.max_match_work = (size_t)1 << 32;
    mo.max_memo_entries = (size_t)1 << 28;
    GTEXT_CSS_Property_Options po;
    std::memset(&po, 0, sizeof po);
    po.match = &mo;
    GTEXT_CSS_Property_Value pv;
    const GTEXT_CSS_Status st = gtext_css_parse_property_text(r, property, std::strlen(property), in.data(), in.size(), &po, &pv);
    m.work = pv.match.work;
    if (st != GTEXT_CSS_OK || pv.outcome != expected) {
      m.ok = false;
      m.why = std::string("the value was not as expected: ") + gtext_css_status_string(st);
    }
    gtext_css_property_value_free(&pv);
    return m;
  };
}

// A CSS stylesheet's bytes, parsed in one call or fed to a stream in pieces of `chunk` bytes (0: one piece), counted by
// gtext_css_parse_bytes_work() and gtext_css_stream_work(): the decoding, the map from the text to the bytes, the buffers and
// the parse. `label` is the protocol's label (NULL for none: UTF-8).
std::function<Measured(size_t)> css_bytes(const char * label, size_t chunk, bool stream, std::function<std::string(size_t)> make) {
  return [label, chunk, stream, make](size_t k) {
    Measured m;
    const std::string in = make(k);
    m.bytes = in.size();
    GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
    o.max_depth = (size_t)1 << 30;
    o.max_tokens = (size_t)1 << 30;
    o.max_rules = (size_t)1 << 30;
    o.max_declarations = (size_t)1 << 30;
    o.max_total_errors = (size_t)1 << 30;
    o.max_input_bytes = (size_t)1 << 30;
    o.count_errors_only = true;
    GTEXT_CSS_Bytes_Options b = gtext_css_bytes_options_default();
    b.protocol_label = label;
    const GTEXT_CSS_Status st = stream ? gtext_css_stream_work(GTEXT_CSS_TREE_STYLESHEET, in.data(), in.size(), chunk, &b, &o, &m.work)
                                       : gtext_css_parse_bytes_work(GTEXT_CSS_TREE_STYLESHEET, in.data(), in.size(), &b, &o, &m.work);
    if (st != GTEXT_CSS_OK) {
      m.ok = false;
      m.why = std::string("the parse failed: ") + gtext_css_status_string(st);
    }
    return m;
  };
}

GTEXT_Markup_Status css_count_sink(void * user, const char *, size_t len) {
  *static_cast<size_t *>(user) += len;
  return GTEXT_MARKUP_OK;
}

// The serializer over the tree a stylesheet parse made, counted by gtext_css_serialize_work(): bytes written
// and nodes visited.
std::function<Measured(size_t)> css_serialize(std::function<std::string(size_t)> make) {
  return [make](size_t k) {
    Measured m;
    const std::string in = make(k);
    m.bytes = in.size();
    GTEXT_CSS_Parse_Options o = gtext_css_parse_options_default();
    o.max_depth = (size_t)1 << 30;
    GTEXT_CSS_Tree * t = nullptr;
    if (gtext_css_parse_stylesheet(in.data(), in.size(), &o, &t, nullptr) != GTEXT_CSS_OK) {
      m.ok = false;
      m.why = "the parse failed";
      return m;
    }
    size_t bytes = 0;
    GTEXT_Markup_Sink sink = {css_count_sink, &bytes};
    if (gtext_css_serialize_work(t, nullptr, &sink, &m.work) != GTEXT_CSS_OK) {
      m.ok = false;
      m.why = "the serializer failed";
    }
    gtext_css_tree_free(t);
    return m;
  };
}

// ---- the XML parser's DTD ----------------------------------------------------------------------------

// A DOM parse with the DTD processed (external entities skipped, as a document with no resolver is), by the parser's work
// counter (`max_total_work`'s). The limits that are not what a shape measures are out of the way.
Measured xml_run(const std::string & input, std::function<void(GTEXT_XML_Parse_Options &)> tune = nullptr) {
  Measured m;
  m.bytes = input.size();
  GTEXT_XML_Parse_Options o = gtext_xml_parse_options_default();
  o.dtd_mode = GTEXT_XML_DTD_PROCESS;
  o.dtd_external_policy = GTEXT_XML_DTD_EXTERNAL_SKIP;
  o.max_declarations = 10000000;
  o.max_references = 100000000;
  o.max_total_work = SIZE_MAX;
  if (tune) {
    tune(o);
  }
  GTEXT_Markup_Document * d = nullptr;
  GTEXT_XML_Error e;
  size_t work = 0;
  const GTEXT_XML_Status st = xml_parse_counted(input.data(), input.size(), &o, &d, &e, &work);
  m.work = work;
  if (st != GTEXT_XML_OK) {
    m.ok = false;
    m.why = std::string("the parse ended with ") + gtext_xml_status_string(st);
  }
  gtext_markup_document_free(d);
  return m;
}

std::function<Measured(size_t)> xml_doc(std::function<std::string(size_t)> make) {
  return [=](size_t k) { return xml_run(make(k)); };
}

// `k` declarations, `unit(i)` each, in an internal subset, then `body(k)`.
std::string xml_subset(size_t k, std::function<std::string(size_t)> unit, const std::string & body) {
  std::string out = "<!DOCTYPE svg [";
  for (size_t i = 0; i < k; i++) {
    out += unit(i);
  }
  return out + "]>" + body;
}

// ---- the shapes ---------------------------------------------------------------------------------------

std::string attrs_n(size_t k, const char * prefix = "a", const char * value = "v") {
  std::string out;
  for (size_t i = 0; i < k; i++) {
    out += std::string(" ") + prefix + std::to_string(i) + "=" + value;
  }
  return out;
}

std::vector<Shape> build_shapes() {
  std::vector<Shape> s;
  auto add = [&](const char * group, const char * name, size_t n, std::function<Measured(size_t)> run) -> Shape & {
    s.emplace_back(group, name, n, std::move(run));
    return s.back();
  };

  // ---- deep nesting, up to and past the open-element limit ----
  add("nesting", "div", 800, doc([](size_t k) { return repeat("<div>", k); }));
  add("nesting", "span", 800, doc([](size_t k) { return repeat("<span>", k); }));
  add("nesting", "div with text", 800, doc([](size_t k) { return repeat("<div>x", k); }));
  add("nesting", "div then ends", 800, doc([](size_t k) { return repeat("<div>", k) + repeat("</div>", k); }));
  add("nesting", "span then a stray end tag per level", 800,
      doc([](size_t k) { return repeat("<span>", k) + repeat("</i>", k); }));
  add("nesting", "span and div alternating", 600, doc([](size_t k) { return repeat("<span><div>", k); }));
  add("nesting", "div then p after each", 600, doc([](size_t k) { return repeat("<div><p>x", k); }));
  add("nesting", "past the open-element limit (64)", 400,
      limited([](size_t k) { return repeat("<div>", k); }, 64, 100000000, GTEXT_HTML_TREE_E_LIMIT_DEPTH))
      .flat = true;
  add("nesting", "past the open-element limit (512, the default)", 2000,
      limited([](size_t k) { return repeat("<span>x", k); }, 512, 100000000, GTEXT_HTML_TREE_E_LIMIT_DEPTH))
      .flat = true;
  add("nesting", "at the limit, then a flood of siblings", 500, [](size_t k) {
    TreeRun r;
    r.input = repeat("<div>", 60) + repeat("<p>y</p><span>z</span>", k);
    r.max_open = 64;
    return tree_run(r);
  });

  // ---- formatting elements: mis-nesting, the adoption agency, reconstruction ----
  add("formatting", "b i b ... then the end tags", 600, doc([](size_t k) { return repeat("<b><i><b>", k) + repeat("</b>", 3 * k); }));
  add("formatting", "b i misnested", 600, doc([](size_t k) { return repeat("<b><i>x</b></i>", k); }));
  add("formatting", "b i misnested, ends last", 600, doc([](size_t k) { return repeat("<b><i>", k) + repeat("</b>", k) + repeat("</i>", k); }));
  add("formatting", "adoption: a b then </a>", 600, doc([](size_t k) { return repeat("<a><b></a>", k); }));
  add("formatting", "adoption: chain of a with a block inside", 600,
      doc([](size_t k) { return repeat("<a><div>", k) + repeat("</a>", k); }));
  add("formatting", "adoption: b around many blocks, one end tag", 600,
      doc([](size_t k) { return "<b>" + repeat("<div>x", k) + "</b>y"; }));
  add("formatting", "adoption: b around many blocks, end tag per block", 600,
      doc([](size_t k) { return "<b>" + repeat("<div>x</b>", k); }));
  add("formatting", "adoption: the 8x3 loop (eight formatting elements, three furthest blocks)", 300, doc([](size_t k) {
        return repeat("<b><i><u><s><em><strong><tt><code><div><div><div>x</b>", k);
      }));
  add("formatting", "adoption: nested formatting around a table", 300,
      doc([](size_t k) { return repeat("<b><i><table><tr><td></b>", k); }));
  add("formatting", "adoption: formatting element deep in the stack, end tag per level", 500,
      doc([](size_t k) { return "<b>" + repeat("<span>", k) + repeat("</b>", k); }));
  add("formatting", "adoption: furthest block with many children above the element", 500,
      doc([](size_t k) { return "<a>" + repeat("<span>", k) + "<p>x</a>y"; }));
  add("formatting", "reconstruction: p closes, b reopened each time", 600, doc([](size_t k) { return repeat("<p><b>x</p>", k); }));
  add("formatting", "reconstruction: eight formatting elements, a text per paragraph", 500,
      doc([](size_t k) { return "<b><i><u><s><em><strong><tt><code>" + repeat("<p>x</p>", k); }));
  add("formatting", "reconstruction: the same formatting element three times (the clause for 3)", 500,
      doc([](size_t k) { return repeat("<b class=a><b class=a><b class=a><b class=a>x<p>", k); }));
  add("formatting", "reconstruction: after a table", 500, doc([](size_t k) { return repeat("<i>x<table></table>", k); }));
  add("formatting", "reconstruction: nobr runs", 600, doc([](size_t k) { return repeat("<nobr>x", k); }));
  add("formatting", "reconstruction: a runs", 600, doc([](size_t k) { return repeat("<a href=u>x", k); }));
  add("formatting", "reconstruction: markers (applet, object, marquee) between", 500,
      doc([](size_t k) { return repeat("<b><object><b>x</object>", k); }));
  add("formatting", "reset the insertion mode: end tag table, deep stack of formatting elements", 500,
      doc([](size_t k) { return repeat("<i>x<table></table>", k); }));
  add("formatting", "reset the insertion mode: end tag template, deep stack", 500,
      doc([](size_t k) { return repeat("<span><template></template>", k); }));
  add("formatting", "reset the insertion mode: caption and select, deep stack", 400,
      doc([](size_t k) { return repeat("<span><table><caption></caption></table><select></select>", k); }));
  add("formatting", "end tag body repeated after text, deep stack", 400,
      doc([](size_t k) { return repeat("<span>", 400) + repeat("</body>x", k); }));
  add("formatting", "end tag html repeated after text, deep stack", 400,
      doc([](size_t k) { return repeat("<span>", 400) + repeat("</html>x", k); }));
  add("formatting", "end tag body repeated after text, stack as deep as the repeats", 400,
      doc([](size_t k) { return repeat("<span>", k) + repeat("</body>x", k); }));
  add("formatting", "end tag html repeated after text, stack as deep as the repeats", 400,
      doc([](size_t k) { return repeat("<span>", k) + repeat("</html>x", k); }));
  add("formatting", "end of file with a deep stack, in a template", 400,
      doc([](size_t k) { return repeat("<template><span>", k); }));
  add("formatting", "end tags with no formatting element, deep stack", 600,
      doc([](size_t k) { return repeat("<i>", 1000) + repeat("</b>", k); }));

  // ---- p, li, dd, option: implied ends ----
  add("implied ends", "p closes p", 800, doc([](size_t k) { return repeat("<p>x", k); }));
  add("implied ends", "p in a deep stack", 600, doc([](size_t k) { return repeat("<span>", 500) + repeat("<p>x", k); }));
  add("implied ends", "div closes p", 600, doc([](size_t k) { return repeat("<p>x<div>y", k); }));
  add("implied ends", "end tag p with no p", 800, doc([](size_t k) { return repeat("</p>", k); }));
  add("implied ends", "li closes li", 800, doc([](size_t k) { return "<ul>" + repeat("<li>x", k); }));
  add("implied ends", "li nested in ul", 400, doc([](size_t k) { return repeat("<ul><li>x", k); }));
  add("implied ends", "li in a deep stack", 600, doc([](size_t k) { return repeat("<span>", 500) + repeat("<li>x", k); }));
  add("implied ends", "dd and dt", 600, doc([](size_t k) { return "<dl>" + repeat("<dd>x<dt>y", k); }));
  add("implied ends", "dd in nested dl", 400, doc([](size_t k) { return repeat("<dl><dd>x", k); }));
  add("implied ends", "option closes option", 800, doc([](size_t k) { return "<select>" + repeat("<option>x", k); }));
  add("implied ends", "optgroup and option", 600, doc([](size_t k) { return "<select>" + repeat("<optgroup><option>x", k); }));
  add("implied ends", "select in select", 500, doc([](size_t k) { return repeat("<select><option>x", k); }));
  add("implied ends", "headings", 600, doc([](size_t k) { return repeat("<h1>x<h2>y", k); }));
  add("implied ends", "rb rt rp rtc", 500, doc([](size_t k) { return "<ruby>" + repeat("<rb>x<rt>y<rp>z<rtc>w", k); }));
  add("implied ends", "button in button", 600, doc([](size_t k) { return repeat("<button>x", k); }));
  add("implied ends", "form in form", 600, doc([](size_t k) { return repeat("<form>x", k); }));

  // ---- tables ----
  add("tables", "td in tr in table (no ends)", 400, doc([](size_t k) { return repeat("<table><tr><td>", k); }));
  add("tables", "nested tables closed at the end", 300,
      doc([](size_t k) { return repeat("<table><tr><td>", k) + repeat("</table>", k); }));
  add("tables", "nested tables closed by end tags in order", 300,
      doc([](size_t k) { return repeat("<table><tr><td>", k) + repeat("</td></tr></table>", k); }));
  add("tables", "cells in one row", 800, doc([](size_t k) { return "<table><tr>" + repeat("<td>x", k); }));
  add("tables", "rows in one table", 800, doc([](size_t k) { return "<table>" + repeat("<tr><td>x", k); }));
  add("tables", "sections and rows", 500, doc([](size_t k) { return "<table>" + repeat("<tbody><tr><td>x<thead><tr><th>y", k); }));
  add("tables", "caption and colgroup", 500, doc([](size_t k) { return "<table>" + repeat("<caption>x<colgroup><col><col>", k); }));
  add("tables", "tables nested in cells of a deep stack", 400,
      doc([](size_t k) { return repeat("<span>", 400) + repeat("<table><td>", k); }));
  add("tables", "table in table (stray table start tag)", 500, doc([](size_t k) { return repeat("<table>", k); }));
  add("tables", "end tag table with open cells", 400, doc([](size_t k) { return repeat("<table><td><table><td></table>", k); }));
  add("tables", "foster parenting: text", 800, doc([](size_t k) { return "<table>" + repeat("x", k); }));
  add("tables", "foster parenting: elements", 600, doc([](size_t k) { return "<table>" + repeat("<b>y", k); }));
  add("tables", "foster parenting: text and elements alternating", 600, doc([](size_t k) { return "<table>" + repeat("x<b>y", k); }));
  add("tables", "foster parenting: in rows", 500, doc([](size_t k) { return "<table>" + repeat("<tr>x</tr>", k); }));
  add("tables", "foster parenting: after nested tables", 300,
      doc([](size_t k) { return repeat("<table><tr><td>", 50) + repeat("<table>x<i>y", k); }));
  add("tables", "templates in tables", 300, doc([](size_t k) { return "<table>" + repeat("<template><tr><td>x", k); }));
  add("tables", "tables in templates", 300, doc([](size_t k) { return repeat("<template><table><tr><td>", k); }));
  add("tables", "template nesting", 400, doc([](size_t k) { return repeat("<template>", k); }));
  add("tables", "templates closed in order", 300, doc([](size_t k) { return repeat("<template><tr>", k) + repeat("</template>", k); }));
  add("tables", "template with table parts in a deep stack", 300,
      doc([](size_t k) { return repeat("<span>", 300) + repeat("<template><tr><td>x</template>", k); }));
  add("tables", "select in a table", 400, doc([](size_t k) { return "<table><tr><td>" + repeat("<select><option>x", k); }));

  // ---- foreign content ----
  add("foreign", "svg nesting", 600, doc([](size_t k) { return repeat("<svg>", k); }));
  add("foreign", "svg g nesting", 600, doc([](size_t k) { return "<svg>" + repeat("<g>", k); }));
  add("foreign", "math mrow nesting", 600, doc([](size_t k) { return "<math>" + repeat("<mrow>", k); }));
  add("foreign", "breakout: p in svg", 600, doc([](size_t k) { return repeat("<svg><p>", k); }));
  add("foreign", "breakout: tags popping a deep foreign stack", 500,
      doc([](size_t k) { return "<svg>" + repeat("<g>", k) + "<p>x"; }));
  add("foreign", "breakout: runs of div/b/img in svg", 500, doc([](size_t k) { return repeat("<svg><g><g><b>x<img><div>", k); }));
  add("foreign", "breakout: font with color", 600, doc([](size_t k) { return repeat("<svg><font color=a>x", k); }));
  add("foreign", "integration points: foreignObject, desc, title", 400,
      doc([](size_t k) { return repeat("<svg><foreignObject><p>x<svg><desc><div>", k); }));
  add("foreign", "integration points: mi mo mn ms mtext, annotation-xml", 400,
      doc([](size_t k) { return repeat("<math><mi><b>x</b><mo><i>y<annotation-xml encoding=text/html><p>z", k); }));
  add("foreign", "end tags that match the foreign stack deep down", 500,
      doc([](size_t k) { return "<svg>" + repeat("<g>", k) + repeat("</g>", k); }));
  add("foreign", "end tags that match nothing, deep foreign stack", 600,
      doc([](size_t k) { return "<svg>" + repeat("<g>", 1500) + repeat("</path>", k); }));
  add("foreign", "adjusted names (viewBox, foreignObject, xlink:href)", 600,
      doc([](size_t k) { return "<svg>" + repeat("<foreignobject viewbox=1 xlink:href=a attributename=b>", k); }));
  add("foreign", "cdata sections", 800, doc([](size_t k) { return "<svg>" + repeat("<![CDATA[x]]>", k); }));
  add("foreign", "many distinct element names", 800, doc([](size_t k) {
        std::string out = "<svg>";
        for (size_t i = 0; i < k; i++) {
          out += "<e" + std::to_string(i) + "/>";
        }
        return out;
      }));

  // ---- fragments ----
  {
    Ctx td;
    td.name = "td";
    Ctx tbody;
    tbody.name = "tbody";
    Ctx tmpl;
    tmpl.name = "template";
    Ctx sel;
    sel.name = "select";
    Ctx svg;
    svg.ns = GTEXT_HTML_NS_SVG;
    svg.name = "svg";
    Ctx fo;
    fo.ns = GTEXT_HTML_NS_SVG;
    fo.name = "foreignObject";
    Ctx mi;
    mi.ns = GTEXT_HTML_NS_MATHML;
    mi.name = "mi";
    Ctx many;
    many.name = "div";
    add("fragments", "deep div in td", 600, frag(td, [](size_t k) { return repeat("<div>", k); }));
    add("fragments", "deep span in tbody", 600, frag(tbody, [](size_t k) { return "<tr><td>" + repeat("<span>", k); }));
    add("fragments", "misnested formatting in td", 500, frag(td, [](size_t k) { return repeat("<b><i>x</b></i>", k); }));
    add("fragments", "foster parented text in tbody", 600, frag(tbody, [](size_t k) { return repeat("x<b>y", k); }));
    add("fragments", "templates", 400, frag(tmpl, [](size_t k) { return repeat("<template><tr><td>x</template>", k); }));
    add("fragments", "svg g", 600, frag(svg, [](size_t k) { return repeat("<g>", k); }));
    add("fragments", "foreignObject with html", 400, frag(fo, [](size_t k) { return repeat("<div>x<p>", k); }));
    add("fragments", "mi with breakouts", 400, frag(mi, [](size_t k) { return repeat("<b>x</b><mglyph/>", k); }));
    add("fragments", "select context", 500, frag(sel, [](size_t k) { return repeat("<option>x<select>y", k); }));
    add("fragments", "adoption agency", 400, frag(td, [](size_t k) { return "<b>" + repeat("<div>x</b>", k); }));
    add("fragments", "context element with many attributes", 400, [many](size_t k) mutable {
      Ctx c = many;
      for (size_t i = 0; i < k; i++) {
        c.attrs.push_back({"a" + std::to_string(i), "v"});
      }
      TreeRun r;
      r.input = "<p>x";
      r.ctx = &c;
      Measured m = tree_run(r);
      m.bytes += k * 8; // the context's attributes are input of the same kind
      return m;
    });
  }

  // ---- attributes ----
  add("attributes", "one tag, many attributes", 300, doc([](size_t k) { return "<div" + attrs_n(k) + ">"; }));
  add("attributes", "one tag, many attributes of the same length (hash buckets)", 300,
      doc([](size_t k) { return "<div" + attrs_n(k, "a", "") + ">"; }));
  add("attributes", "one tag, many duplicates of one attribute", 300,
      doc([](size_t k) { return "<div" + repeat(" a=1", k) + ">"; }));
  add("attributes", "one tag, many duplicates alternating two names", 300,
      doc([](size_t k) { return "<div" + repeat(" a=1 b=2", k) + ">"; }));
  add("attributes", "one tag, names that differ in the last byte only", 300, doc([](size_t k) {
        std::string out = "<div";
        for (size_t i = 0; i < k; i++) {
          out += " attribute-name-that-is-long-" + std::string(1, static_cast<char>('a' + i % 26)) + std::to_string(i);
        }
        return out + ">";
      }));
  add("attributes", "one tag, a long value", 800, doc([](size_t k) { return "<div a=\"" + repeat("x", 40 * k) + "\">"; }));
  add("attributes", "one tag, a long unquoted value of references", 800, doc([](size_t k) { return "<div a=" + repeat("&amp;", 8 * k) + ">"; }));
  add("attributes", "many tags, a few attributes each", 500, doc([](size_t k) { return repeat("<div class=a id=b data-x=c>", k); }));
  add("attributes", "many a elements with attributes (Noah's Ark, distinct)", 400,
      doc([](size_t k) {
        std::string out;
        for (size_t i = 0; i < k; i++) {
          out += "<a href=h" + std::to_string(i) + " id=i" + std::to_string(i) + ">x";
        }
        return out;
      }));
  add("attributes", "repeated html and body tags with attributes", 300,
      doc([](size_t k) { return repeat("<html" + attrs_n(8) + "><body" + attrs_n(8) + ">", k); }));
  add("attributes", "past the attribute limit (1024)", 3000,
      limited([](size_t k) { return "<div" + attrs_n(k) + ">"; }, 100000000, 1024, GTEXT_HTML_TREE_E_LIMIT_ATTRIBUTES))
      .flat = true;

  // ---- long runs: comments, NULs, character references, white space ----
  add("runs", "comments", 1000, doc([](size_t k) { return repeat("<!--c-->", k); }));
  add("runs", "empty comments and bogus comments", 1000, doc([](size_t k) { return repeat("<!----><!x><?y>", k); }));
  add("runs", "NUL in data", 4000, doc([](size_t k) { return repeat(std::string("a\0", 2), k); }));
  add("runs", "NUL in body, after a table", 3000, doc([](size_t k) { return "<table>" + repeat(std::string("\0", 1), k); }));
  add("runs", "NUL in attribute values and names", 800, doc([](size_t k) { return repeat(std::string("<a b=\"\0\" c\0=d>", 14), k); }));
  add("runs", "NUL in script and rcdata", 1500,
      doc([](size_t k) { return "<script>" + repeat(std::string("\0", 1), k) + "</script><title>" + repeat(std::string("\0", 1), k); }));
  add("runs", "named references", 1000, doc([](size_t k) { return repeat("&amp;&lt;&gt;&quot;&nbsp;", k); }));
  add("runs", "named references, no semicolon (legacy)", 1000, doc([](size_t k) { return repeat("&amp&lt&gt&copy&notit;", k); }));
  add("runs", "ampersands that name nothing", 2000, doc([](size_t k) { return repeat("&zzz;&x&", k); }));
  add("runs", "a long name that is almost a reference", 800, doc([](size_t k) { return repeat("&CounterClockwiseContourIntegra", k); }));
  add("runs", "a long run of letters after an ampersand", 800, doc([](size_t k) { return "&" + repeat("abcdefghij", k); }));
  add("runs", "numeric references", 1000, doc([](size_t k) { return repeat("&#65;&#x41;&#0;&#x110000;&#xD800;&#128;", k); }));
  add("runs", "numeric references, a long run of digits", 800, doc([](size_t k) { return "&#" + repeat("1234567890", k) + ";"; }));
  add("runs", "numeric references with no digits", 2000, doc([](size_t k) { return repeat("&#;&#x;&#", k); }));
  add("runs", "references in attribute values", 800, doc([](size_t k) { return "<a href=\"" + repeat("?a=1&b=2&copy=3&amp;", k) + "\">"; }));
  add("runs", "white space", 4000, doc([](size_t k) { return "<table>" + repeat(" \n\t", k); }));
  add("runs", "carriage returns", 4000, doc([](size_t k) { return repeat("a\r\nb\rc", k); }));
  add("runs", "text", 6000, doc([](size_t k) { return repeat("lorem ipsum ", k); }));
  add("runs", "text split by ignored tags", 800, doc([](size_t k) { return "<table><tr><td>" + repeat("x</foo>", k); }));
  add("runs", "doctypes", 800, doc([](size_t k) { return repeat("<!DOCTYPE html>", k); }));
  add("runs", "processing instructions", 800, doc([](size_t k) { return repeat("<?php x ?>", k); }));
  add("runs", "meta charset elements", 800, doc([](size_t k) { return repeat("<meta charset=utf-8><meta http-equiv=content-type content='text/html; charset=utf-8'>", k); }));

  // ---- tokenizer states: unterminated constructs, in every escape state ----
  add("tokenizer", "unterminated comment", 8000, doc([](size_t k) { return "<!--" + repeat("x", k); }));
  add("tokenizer", "unterminated comment of dashes", 8000, doc([](size_t k) { return "<!--" + repeat("-", k); }));
  add("tokenizer", "unterminated comment of dash-bang", 4000, doc([](size_t k) { return "<!--" + repeat("--!", k); }));
  add("tokenizer", "unterminated comment of <!-- (nested)", 3000, doc([](size_t k) { return "<!--" + repeat("<!--", k); }));
  add("tokenizer", "unterminated bogus comment", 8000, doc([](size_t k) { return "<!" + repeat("x", k); }));
  add("tokenizer", "unterminated processing instruction", 8000, doc([](size_t k) { return "<?" + repeat("x", k); }));
  add("tokenizer", "unterminated tag name", 4000, doc([](size_t k) { return "<a" + repeat("b", k); }));
  add("tokenizer", "unterminated tag with attributes", 2000, doc([](size_t k) { return "<a" + repeat(" b=c", k); }));
  add("tokenizer", "unterminated tag with quoted value", 8000, doc([](size_t k) { return "<a b=\"" + repeat("c", k); }));
  add("tokenizer", "unterminated tag with solidi", 6000, doc([](size_t k) { return "<a" + repeat("/", k); }));
  add("tokenizer", "unterminated end tag", 6000, doc([](size_t k) { return "</a" + repeat(" b", k); }));
  add("tokenizer", "unterminated doctype", 6000, doc([](size_t k) { return "<!DOCTYPE" + repeat(" x", k); }));
  add("tokenizer", "unterminated doctype public identifier", 6000, doc([](size_t k) { return "<!DOCTYPE html PUBLIC \"" + repeat("x", k); }));
  add("tokenizer", "unterminated doctype system identifier", 6000, doc([](size_t k) { return "<!DOCTYPE html SYSTEM '" + repeat("x", k); }));
  add("tokenizer", "doctype keywords repeated", 2000, doc([](size_t k) { return "<!DOCTYPE html PUBLIC" + repeat(" PUBLIC SYSTEM", k); }));
  add("tokenizer", "script data", 8000, doc([](size_t k) { return "<script>" + repeat("x", k); }));
  add("tokenizer", "script data: lone less-than signs", 4000, doc([](size_t k) { return "<script>" + repeat("<", k); }));
  add("tokenizer", "script data: partial end tags", 2000, doc([](size_t k) { return "<script>" + repeat("</scrip", k); }));
  add("tokenizer", "script data: end tags that do not match", 2000, doc([](size_t k) { return "<script>" + repeat("</scriptx ", k); }));
  add("tokenizer", "script data escaped", 2000, doc([](size_t k) { return "<script><!--" + repeat("x", k); }));
  add("tokenizer", "script data escaped: dashes", 4000, doc([](size_t k) { return "<script><!--" + repeat("-", k); }));
  add("tokenizer", "script data escaped: dash less-than", 3000, doc([](size_t k) { return "<script><!--" + repeat("-<", k); }));
  add("tokenizer", "script data escaped: partial end tags", 2000, doc([](size_t k) { return "<script><!--" + repeat("</scrip", k); }));
  add("tokenizer", "script data double escaped", 2000, doc([](size_t k) { return "<script><!--<script>" + repeat("x", k); }));
  add("tokenizer", "script data double escaped: dashes and less-than", 2000, doc([](size_t k) { return "<script><!--<script>" + repeat("-<", k); }));
  add("tokenizer", "script data double escaped: partial end tags", 2000, doc([](size_t k) { return "<script><!--<script>" + repeat("</scrip", k); }));
  add("tokenizer", "script data double escaped: repeated start", 1500, doc([](size_t k) { return "<script>" + repeat("<!--<script>", k); }));
  add("tokenizer", "script data double escape start/end, unterminated", 1500,
      doc([](size_t k) { return "<script><!--<script>" + repeat("</script", k); }));
  add("tokenizer", "rcdata (title)", 6000, doc([](size_t k) { return "<title>" + repeat("x", k); }));
  add("tokenizer", "rcdata: partial end tags", 2000, doc([](size_t k) { return "<title>" + repeat("</titl", k); }));
  add("tokenizer", "rcdata: references", 1500, doc([](size_t k) { return "<textarea>" + repeat("&amp&lt;&", k); }));
  add("tokenizer", "rawtext (style)", 6000, doc([](size_t k) { return "<style>" + repeat("x", k); }));
  add("tokenizer", "rawtext: partial end tags", 2000, doc([](size_t k) { return "<style>" + repeat("</styl", k); }));
  add("tokenizer", "rawtext: xmp, iframe, noembed", 2000, doc([](size_t k) { return "<xmp>" + repeat("</xm", k) + "<iframe>" + repeat("</ifram", k); }));
  add("tokenizer", "plaintext", 8000, doc([](size_t k) { return "<plaintext>" + repeat("<x>", k); }));
  add("tokenizer", "cdata section unterminated", 6000, doc([](size_t k) { return "<svg><![CDATA[" + repeat("x", k); }));
  add("tokenizer", "cdata section with brackets", 4000, doc([](size_t k) { return "<svg><![CDATA[" + repeat("]", k); }));
  add("tokenizer", "cdata section with partial ends", 2000, doc([](size_t k) { return "<svg><![CDATA[" + repeat("]]", k); }));
  add("tokenizer", "cdata in html content (a bogus comment)", 4000, doc([](size_t k) { return "<![CDATA[" + repeat("x", k); }));
  add("tokenizer", "after-name states: equals signs", 4000, doc([](size_t k) { return "<a" + repeat("=", k); }));
  add("tokenizer", "attribute names of quotes and angle brackets", 2000, doc([](size_t k) { return "<a " + repeat("\"'<", k); }));
  add("tokenizer", "self-closing solidus runs", 2000, doc([](size_t k) { return repeat("<br/ /><div/ x/>", k); }));
  add("tokenizer", "bytes of every value (no state is special)", 800, doc([](size_t k) {
        std::string unit;
        for (int c = 1; c < 256; c++) {
          unit += static_cast<char>(c);
        }
        return repeat(unit, k);
      }));
  add("tokenizer", "mixed tag soup", 800, doc([](size_t k) { return repeat("<a<b<c<!-<?<![<!d<&<", k); }));

  // ---- the serializer ----
  add("serializer", "a tree 16n deep", 800, ser([](size_t k) { return repeat("<div>", k) + "x"; }));
  add("serializer", "a tree 16n wide", 1500, ser([](size_t k) { return repeat("<li>x</li>", k); }));
  add("serializer", "wide and deep", 400, ser([](size_t k) { return repeat("<div><span>x</span><p>y<b>z</b>", k); }));
  add("serializer", "many attributes on one element", 500, ser([](size_t k) { return "<div" + attrs_n(k) + ">"; }));
  add("serializer", "many attributes sorted", 500, ser([](size_t k) { return "<div" + attrs_n(k) + ">"; }, true, SerializeOptions::Alphabetical));
  add("serializer", "many elements with attributes sorted", 500, ser([](size_t k) { return repeat("<a z=1 y=2 x=3 b=4 a=5>x", k); }, true, SerializeOptions::Alphabetical));
  add("serializer", "long text with specials", 1500, ser([](size_t k) { return repeat("a&b<c>d\xc2\xa0", 8 * k); }));
  add("serializer", "long attribute values with specials", 800, ser([](size_t k) { return "<a b=\"" + repeat("&amp;&quot;x", 8 * k) + "\">"; }));
  add("serializer", "comments in a tree", 1500, ser([](size_t k) { return repeat("<!--c-->", k); }));
  add("serializer", "optional tags omitted, p and li", 800, ser([](size_t k) { return repeat("<p>x<ul><li>y<li>z</ul>", k); }, true, SerializeOptions::Omit));
  add("serializer", "optional tags omitted, a table", 500, ser([](size_t k) { return "<table>" + repeat("<tr><td>x<td>y", k); }, true, SerializeOptions::Omit));
  add("serializer", "optional tags omitted, a whole document", 500,
      ser([](size_t k) { return "<!DOCTYPE html><html><head></head><body>" + repeat("<p>x<dl><dt>a<dd>b</dl>", k); }, false, SerializeOptions::Omit));
  add("serializer", "every option on, wide", 800, ser([](size_t k) { return repeat("<a href=u disabled class=x>t <b>u</b> </a>\n", k); }, true, SerializeOptions::Everything));
  add("serializer", "every option on, deep", 600, ser([](size_t k) { return repeat("<div class=a>", k) + "x"; }, true, SerializeOptions::Everything));
  add("serializer", "white space stripped", 1500, ser([](size_t k) { return repeat("a  \n\t b <pre>  x  </pre> ", k); }, true, SerializeOptions::Strip));
  add("serializer", "templates in a tree", 400, ser([](size_t k) { return repeat("<template><div>x</div></template>", k); }));
  add("serializer", "nested templates", 400, ser([](size_t k) { return repeat("<template>", k) + "x"; }));
  add("serializer", "foreign content", 400, ser([](size_t k) { return repeat("<svg><g><path d=a /></g></svg>", k); }));

  // ---- the public parser: the memory it asks for, whole and in pieces ----
  {
    auto note = [&](Shape & sh) -> Shape & {
      sh.per_byte = 0;
      sh.note = "counted in bytes requested of the allocator (a realloc counted at its new size) plus 16 per call";
      return sh;
    };
    note(add("api", "text, whole", 20000, api([](size_t k) { return repeat("lorem ", k); }, 0)));
    note(add("api", "text, a byte at a time", 6000, api([](size_t k) { return repeat("lorem ", k); }, 1)));
    note(add("api", "text, 7 bytes at a time", 6000, api([](size_t k) { return repeat("lorem ", k); }, 7)));
    note(add("api", "text split by an ignored tag, a byte at a time", 20000, api([](size_t k) { return "<table><tr><td>" + repeat("x</foo>", k); }, 1)));
    note(add("api", "one comment, a byte at a time", 40000, api([](size_t k) { return "<!--" + repeat("c", k); }, 1)));
    note(add("api", "one attribute value, a byte at a time", 40000, api([](size_t k) { return "<a b=\"" + repeat("c", k) + "\">"; }, 1)));
    note(add("api", "one tag name, a byte at a time", 40000, api([](size_t k) { return "<a" + repeat("c", k); }, 1)));
    note(add("api", "script data, a byte at a time", 8000, api([](size_t k) { return "<script>" + repeat("<!--<script>x", k / 10); }, 1)));
    note(add("api", "references, a byte at a time", 20000, api([](size_t k) { return repeat("&amp;&notit;&#x41;&", k); }, 1)));
    note(add("api", "elements, whole", 4000, api([](size_t k) { return repeat("<p class=a>x</p>", k); }, 0)));
    note(add("api", "elements, 5 bytes at a time", 2000, api([](size_t k) { return repeat("<p class=a>x</p>", k); }, 5)));
    note(add("api", "deep nesting, a byte at a time", 600, api([](size_t k) { return repeat("<div>", k); }, 1)));
    note(add("api", "parse errors kept (no limit)", 3000, api([](size_t k) { return repeat("</x>", k); }, 0, (size_t)1 << 30)));
    note(add("api", "parse errors kept (no limit), a byte at a time", 3000, api([](size_t k) { return repeat("</x>&zz;", k); }, 1, (size_t)1 << 30)));
    note(add("api", "parse errors past the default limit", 3000, api([](size_t k) { return repeat("</x>", k); }, 0)));
    note(add("api", "head open, the bytes kept for a restart", 20000, api([](size_t k) { return "<html><head><title>" + repeat("t", 2 * k) + "</title>"; }, 1)));
    note(add("api", "head open, a restart at the end", 3000, api([](size_t k) { return "<html><head>" + repeat("<title>x</title>", k) + "<meta charset=windows-1251>"; }, 3)));
    note(add("api", "a document in Shift_JIS, a byte at a time", 20000, [](size_t k) {
      ApiRun r;
      r.input = "<p>" + repeat("\x82\xa0\x83\x41 x ", k) + "</p>";
      r.chunk = 1;
      r.transport = "shift_jis";
      return api_run(r);
    }));
    note(add("api", "a document in UTF-16LE, whole", 30000, [](size_t k) {
      ApiRun r;
      std::string utf16("\xff\xfe", 2);
      for (const char c : std::string("<p>") + repeat("ab ", k) + "</p>") {
        utf16 += c;
        utf16 += '\0';
      }
      r.input = utf16;
      return api_run(r);
    }));
    note(add("api", "fragment, whole", 4000, api([](size_t k) { return repeat("<li>x<b>y", k); }, 0, 0, true)));
    note(add("api", "fragment, a byte at a time", 2000, api([](size_t k) { return repeat("<li>x<b>y", k); }, 1, 0, true)));
  }

  // ---- the Encoding module ----
  struct Dec {
    const char * name;
    GTEXT_Encoding e;
    std::function<std::string(size_t)> make;
  };
  const std::vector<Dec> decs = {
      {"utf-8 truncated sequences", GTEXT_ENCODING_UTF_8, [](size_t k) { return repeat("\xe2\x82", k); }},
      {"utf-8 overlong and surrogates", GTEXT_ENCODING_UTF_8, [](size_t k) { return repeat("\xc0\x80\xed\xa0\x80\xf4\x90\x80\x80", k); }},
      {"utf-8 four-byte lead then ascii", GTEXT_ENCODING_UTF_8, [](size_t k) { return repeat("\xf0\x90\x80x", k); }},
      {"utf-16le odd length and lone surrogates", GTEXT_ENCODING_UTF_16LE, [](size_t k) { return repeat(std::string("\x00\xd8\x41\x00\x00\xdc\x00", 7), k); }},
      {"utf-16be lead surrogates", GTEXT_ENCODING_UTF_16BE, [](size_t k) { return repeat(std::string("\xd8\x00\xd8\x00\xdc", 5), k); }},
      {"windows-1252", GTEXT_ENCODING_WINDOWS_1252, [](size_t k) { return repeat("\x80\x81\xe9\xff", k); }},
      {"iso-8859-2", GTEXT_ENCODING_ISO_8859_2, [](size_t k) { return repeat("\xa1\xb2\xff", k); }},
      {"x-user-defined", GTEXT_ENCODING_X_USER_DEFINED, [](size_t k) { return repeat("\x80\xff", k); }},
      {"shift_jis lead bytes then ascii", GTEXT_ENCODING_SHIFT_JIS, [](size_t k) { return repeat("\x81x\x82\x20\xa1\xe0", k); }},
      {"shift_jis pairs", GTEXT_ENCODING_SHIFT_JIS, [](size_t k) { return repeat("\x82\xa0\x83\x41", k); }},
      {"euc-jp three-byte forms and bad trails", GTEXT_ENCODING_EUC_JP, [](size_t k) { return repeat("\x8f\xa1x\x8e\x41\xa4\xa2", k); }},
      {"euc-kr bad trails", GTEXT_ENCODING_EUC_KR, [](size_t k) { return repeat("\xb0x\xb0\xa1\xff", k); }},
      {"gbk bad trails", GTEXT_ENCODING_GBK, [](size_t k) { return repeat("\x81x\x81\x30\x81\x41", k); }},
      {"gb18030 partial four-byte forms", GTEXT_ENCODING_GB18030, [](size_t k) { return repeat("\x81\x30\x81x\x81\x30x\x81\x35\xf4\x37", k); }},
      {"big5 pairs of two code points and bad trails", GTEXT_ENCODING_BIG5, [](size_t k) { return repeat("\x88\x62\x88\x64\x88x\xa1\xa1", k); }},
      {"iso-2022-jp escapes", GTEXT_ENCODING_ISO_2022_JP, [](size_t k) { return repeat("\x1b$B$\"\x1b(B\x1b(J\x1b$@a\x1b\x1b", k); }},
      {"iso-2022-jp unfinished escape sequences", GTEXT_ENCODING_ISO_2022_JP, [](size_t k) { return repeat("\x1b$\x1b(\x1b$(", k); }},
      {"replacement", GTEXT_ENCODING_REPLACEMENT, [](size_t k) { return repeat("abc", k); }},
  };
  for (const Dec & d : decs) {
    Shape & sh = add("encoding", (std::string("decode ") + d.name).c_str(), 3000,
        [d](size_t k) { return decode_run(d.e, d.make(k), 1 << 20); });
    sh.per_byte = 4.0; // gtext_decoder_decode's documented bound: at most four handler steps per byte
    sh.note = "documented: at most four handler steps per byte";
    Shape & by = add("encoding", (std::string("decode a byte at a time ") + d.name).c_str(), 1500,
        [d](size_t k) { return decode_run(d.e, d.make(k), 1); });
    by.per_byte = 4.0;
    by.note = "documented: at most four handler steps per byte, and the same however the input is cut";
  }
  const std::string text_cjk = std::string("a\xe3\x81\x82\xe6\xbc\xa2\xef\xbf\xbd\xf0\x9f\x98\x80\xc3\xa9", 16);
  for (const auto & e : std::vector<std::pair<const char *, GTEXT_Encoding>>{
           {"shift_jis", GTEXT_ENCODING_SHIFT_JIS}, {"euc-jp", GTEXT_ENCODING_EUC_JP}, {"iso-2022-jp", GTEXT_ENCODING_ISO_2022_JP},
           {"gb18030", GTEXT_ENCODING_GB18030}, {"gbk", GTEXT_ENCODING_GBK}, {"big5", GTEXT_ENCODING_BIG5},
           {"euc-kr", GTEXT_ENCODING_EUC_KR}, {"windows-1252", GTEXT_ENCODING_WINDOWS_1252}, {"utf-8", GTEXT_ENCODING_UTF_8}}) {
    Shape & sh = add("encoding", (std::string("encode with references ") + e.first).c_str(), 2000,
        [e, text_cjk](size_t k) { return encode_run(e.second, repeat(text_cjk, k), true); });
    sh.per_byte = 3.0;
    sh.note = "documented: at most three handler steps per byte";
  }

  // ---- CSS Syntax ----
  // The shapes that make a naive CSS parser quadratic: above all the speculation of "consume a block's
  // contents", which reads an item as a declaration and then again as a rule, so that a block inside a
  // block inside a block is read once for every block around it; and the long runs of every token.
  const GTEXT_CSS_Tree_Kind sheet = GTEXT_CSS_TREE_STYLESHEET;
  add("css", "CSS nested style rules a{a{a{...}}}", 2000, css(sheet, [](size_t k) { return repeat("a{", k) + repeat("}", k); }));
  add("css", "CSS nested rules, never closed", 2000, css(sheet, [](size_t k) { return repeat("a{", k); }));
  add("css", "CSS declaration then nested rule per level", 1500, css(sheet, [](size_t k) { return repeat("a{b:c;", k) + repeat("}", k); }));
  add("css", "CSS a block then more, read as a declaration and then as a rule, nested", 1500,
      css(sheet, [](size_t k) { return "x{" + repeat("a:{", k) + repeat("} c", k) + "}"; }));
  add("css", "CSS a value then a block, nested", 1500, css(sheet, [](size_t k) { return "x{" + repeat("a:b{", k) + repeat("}", k) + "}"; }));
  add("css", "CSS a custom property's value with blocks nested", 1500,
      css(sheet, [](size_t k) { return "x{--a:" + repeat("{b ", k) + repeat("}", k) + "}"; }));
  add("css", "CSS rules that look like custom properties, nested", 1500,
      css(sheet, [](size_t k) { return "x{" + repeat("--a:b{", k) + repeat("}", k) + "}"; }));
  add("css", "CSS nested rules, never closed, a validity callback that accepts", 1500,
      css(sheet, [](size_t k) { return repeat("x{", k); }, 1));
  add("css", "CSS nested rules, never closed, a validity callback that refuses", 1500,
      css(sheet, [](size_t k) { return repeat("x{", k); }, 2));
  add("css", "CSS nested rules a{a{a{...}}}, a validity callback that refuses", 1500,
      css(sheet, [](size_t k) { return repeat("{", k) + repeat("}", k); }, 2));
  add("css", "CSS nested at-rules", 1500, css(sheet, [](size_t k) { return repeat("@m{", k) + repeat("}", k); }));
  add("css", "CSS many rules", 4000, css(sheet, [](size_t k) { return repeat("a b>c{d:e;f:g}", k); }));
  add("css", "CSS many declarations in one block", 4000, css(sheet, [](size_t k) { return "a{" + repeat("b:c d;", k) + "}"; }));
  add("css", "CSS one long declaration value", 8000, css(GTEXT_CSS_TREE_DECLARATION_LIST, [](size_t k) { return "a:" + repeat(" x", k); }));
  add("css", "CSS a long value then !important", 8000,
      css(GTEXT_CSS_TREE_DECLARATION_LIST, [](size_t k) { return "a:" + repeat(" x", k) + " !important"; }));
  add("css", "CSS a long invalid declaration (its remnants)", 8000,
      css(GTEXT_CSS_TREE_DECLARATION_LIST, [](size_t k) { return "a b" + repeat(" (x)", k) + ";c:d"; }));
  add("css", "CSS a long unicode-range value, read twice", 4000,
      css(GTEXT_CSS_TREE_DECLARATION_LIST, [](size_t k) { return "unicode-range:" + repeat(" u+1?-2,", k); }));
  add("css", "CSS nested functions", 2000, css(GTEXT_CSS_TREE_COMPONENT_VALUE_LIST, [](size_t k) { return repeat("f(", k) + repeat(")", k); }));
  add("css", "CSS nested blocks of every kind, never closed", 2000,
      css(GTEXT_CSS_TREE_COMPONENT_VALUE_LIST, [](size_t k) { return repeat("([{", k); }));
  add("css", "CSS closing tokens that match nothing", 8000, css(GTEXT_CSS_TREE_COMPONENT_VALUE_LIST, [](size_t k) { return repeat(")]}", k); }));
  add("css", "CSS comments", 8000, css(GTEXT_CSS_TREE_COMPONENT_VALUE_LIST, [](size_t k) { return repeat("/**/a", k); }));
  add("css", "CSS one long comment, never closed", 8000, css(GTEXT_CSS_TREE_COMPONENT_VALUE_LIST, [](size_t k) { return "/*" + repeat("*x", k); }));
  add("css", "CSS escapes", 8000, css(GTEXT_CSS_TREE_COMPONENT_VALUE_LIST, [](size_t k) { return repeat("\\41 \\x", k); }));
  add("css", "CSS one long string, never closed", 8000, css(GTEXT_CSS_TREE_COMPONENT_VALUE_LIST, [](size_t k) { return "'" + repeat("x\\'", k); }));
  add("css", "CSS bad strings", 8000, css(GTEXT_CSS_TREE_COMPONENT_VALUE_LIST, [](size_t k) { return repeat("'x\n", k); }));
  add("css", "CSS bad urls and their remnants", 4000, css(GTEXT_CSS_TREE_COMPONENT_VALUE_LIST, [](size_t k) { return repeat("url(a b\\) c)", k); }));
  add("css", "CSS numbers and dimensions", 8000, css(GTEXT_CSS_TREE_COMPONENT_VALUE_LIST, [](size_t k) { return repeat("+1.5e-3px ", k); }));
  add("css", "CSS a comma-separated list", 8000, css(GTEXT_CSS_TREE_COMMA_LIST, [](size_t k) { return repeat("a b,", k); }));
  add("css", "CSS ill-formed UTF-8", 8000, css(GTEXT_CSS_TREE_COMPONENT_VALUE_LIST, [](size_t k) { return repeat("a\xe2\x82\xed\xa0\x80", k); }));
  // ---- CSS Value Definition Syntax: the shapes that make a backtracking matcher exponential or quadratic ----
  add("css", "CSS grammar: a repetition of a repetition that can be empty, [ [ a? b? ]* ]+", 600,
      css_grammar("[ [ a? b? ]* ]+", [](size_t k) { return repeat("a ", k); }));
  add("css", "CSS grammar: a repetition of a repetition that cannot match", 600,
      css_grammar("[ [ a? b? ]* ]+", [](size_t k) { return repeat("a ", k) + "c"; }, false));
  add("css", "CSS grammar: four stars then a letter that is not there", 600,
      css_grammar("a* a* a* a* b", [](size_t k) { return repeat("a ", k); }, false));
  add("css", "CSS grammar: four stars then the letter", 600,
      css_grammar("a* a* a* a* b", [](size_t k) { return repeat("a ", k) + "b"; }));
  add("css", "CSS grammar: an ambiguous alternation, [ a | a a ]+ b", 5000,
      css_grammar("[ a | a a ]+ b", [](size_t k) { return repeat("a ", k); }, false));
  add("css", "CSS grammar: nested optionals, [ a? [ a? a? ]? ]* b", 5000,
      css_grammar("[ a? [ a? a? ]? ]* b", [](size_t k) { return repeat("a ", k); }, false));
  add("css", "CSS grammar: a comma list of repetitions, [ a+ ]#", 5000,
      css_grammar("[ a+ ]#", [](size_t k) { return repeat("a a, ", k) + "a"; }));
  add("css", "CSS grammar: one long any-value", 8000, css_grammar("<any-value>", [](size_t k) { return repeat("a (b) ", k); }));
  // (An any-value that is followed by a keyword it can swallow, once for each of many repetitions, is quadratic and is
  // not a shape here: every position the any-value could end at is a candidate for the keyword.)
  add("css", "CSS grammar: an any-value then a keyword", 8000,
      css_grammar("x <any-value> y", [](size_t k) { return "x " + repeat("a b ", k) + "y"; }));
  add("css", "CSS grammar: a long list of lengths", 8000,
      css_grammar("[ <length> | <percentage> ]#", [](size_t k) { return repeat("1px, 2%, ", k) + "2px"; }));
  add("css", "CSS grammar: right recursion through a function, many levels", 2000,
      css_grammar("f( a | b )+", [](size_t k) { return repeat("f(a) ", k); }));
  add("css", "CSS grammar: nested functions that the grammar names once each", 1500,
      css_grammar("[ f( [ g( a* ) ]* ) ]+", [](size_t k) { return repeat("f(g(a a) g(a)) ", k); }));
  add("css", "CSS grammar: linking a chain of types that can match nothing", 500, css_link_chain()).per_byte = 0;
  // ---- the value layer: math functions, colors, and the function types (C1b) ----
  add("css", "CSS math: a sum of many terms of one unit", 8000,
      css_value_read(false, [](size_t k) { return "calc(1px" + repeat(" + 1px", k) + ")"; }));
  add("css", "CSS math: a sum of many terms of unlike units (nothing simplifies away)", 4000,
      css_value_read(false, [](size_t k) {
        std::string out = "calc(1px";
        for (size_t i = 0; i < k; i++) {
          out += i % 2 ? " + 1em" : " + 1%";
        }
        return out + ")";
      }));
  add("css", "CSS math: a product of many factors", 8000,
      css_value_read(false, [](size_t k) { return "calc(1px" + repeat(" * 1", k) + ")"; }));
  add("css", "CSS math: parentheses nested to the depth", 4000,
      css_value_read(false, [](size_t k) { return "calc(" + repeat("(", k) + "1px" + repeat(")", k) + ")"; }));
  add("css", "CSS math: min() nested to the depth", 2000,
      css_value_read(false, [](size_t k) { return repeat("min(1em, ", k) + "1px" + repeat(")", k); }));
  add("css", "CSS math: min() of many arguments", 8000,
      css_value_read(false, [](size_t k) { return "min(1em" + repeat(", 1%", k) + ")"; }));
  add("css", "CSS math: a left-nested sum, ((1px + 1em) + 1%) + ...", 2000,
      css_value_read(false, [](size_t k) {
        std::string out = "1px";
        for (size_t i = 0; i < k; i++) {
          out = "(" + out + (i % 2 ? " + 1em)" : " + 1%)");
        }
        return "calc(" + out + ")";
      }));
  add("css", "CSS color: color-mix() nested to the limit", 200,
      css_value_read(true, [](size_t k) { return repeat("color-mix(in srgb, red, ", k) + "blue" + repeat(")", k); }));
  add("css", "CSS color: a relative color nested to the limit", 200,
      css_value_read(true, [](size_t k) { return repeat("rgb(from ", k) + "red" + repeat(" r g b)", k); }));
  add("css", "CSS color: light-dark() nested to the limit", 200,
      css_value_read(true, [](size_t k) { return repeat("light-dark(red, ", k) + "blue" + repeat(")", k); }));
  add("css", "CSS functions: a gradient of many stops", 4000,
      css_grammar("<gradient>", [](size_t k) { return "linear-gradient(red" + repeat(", blue 10%", k) + ")"; }, true, true));
  add("css", "CSS functions: a transform list of many functions", 4000,
      css_grammar("<transform-list>", [](size_t k) { return repeat("translate(1px, 2%) ", k); }, true, true));
  add("css", "CSS functions: linear() with many points", 4000,
      css_grammar("<easing-function>", [](size_t k) { return "linear(0" + repeat(", 0.5", k) + ", 1)"; }, true, true));
  add("css", "CSS functions: polygon() with many vertices", 4000,
      css_grammar("<basic-shape>", [](size_t k) { return "polygon(0 0" + repeat(", 1px 1px", k) + ")"; }, true, true));
  add("css", "CSS functions: a list of images", 4000,
      css_grammar("<image>#", [](size_t k) { return repeat("linear-gradient(red, blue), ", k) + "url(a)"; }, true, true));
  {
    Shape & quad = add("css", "CSS grammar: two wide any-values joined by && (bounded-quadratic by design)", 300,
        css_grammar("<any-value> && <any-value>", [](size_t k) { return repeat("a ", k); }));
    quad.bound = 17.0;
    quad.per_byte = 0;
    quad.note = "every state of the first member looks at every position after it";
  }
  // ---- the registry: registrations, overrides, aliases, shorthands and lookups; property values ----
  add("css", "CSS registry: n registrations of distinct names", 500, css_registry_shape(0));
  add("css", "CSS registry: n registrations of names that share a 100-byte prefix", 500, css_registry_shape(1));
  add("css", "CSS registry: a chain of n overrides of one name", 500, css_registry_shape(2));
  add("css", "CSS registry: n aliases of one property", 500, css_registry_shape(3));
  add("css", "CSS registry: n shorthands of six longhands", 500, css_registry_shape(4));
  add("css", "CSS registry: n lookups of names it lacks", 500, css_registry_shape(5));
  add("css", "CSS registry: n lookups of names it has", 500, css_registry_shape(6));
  add("css", "CSS property: a comma list of n transitions", 1000,
      css_property_value("transition", [](size_t k) { return repeat("opacity 1s ease-in 2s, ", k) + "color 1s"; }));
  add("css", "CSS property: n grid tracks", 1000, css_property_value("grid-template-columns", [](size_t k) { return repeat("1fr minmax(10px, 2fr) ", k); }));
  add("css", "CSS property: a custom property of n tokens", 2000, css_property_value("--x", [](size_t k) { return repeat("a (b) c, ", k); }));
  // ---- from bytes and from a stream: the decoder, the map back to the bytes, the buffers, the cuts ----
  const std::string sheet_unit = "a.b > c { d: e; f: \"g\" } /* h */\n";
  add("css", "CSS stream, text a byte at a time", 4000, css_bytes(nullptr, 1, true, [sheet_unit](size_t k) { return repeat(sheet_unit, k); }));
  add("css", "CSS stream, UTF-8 bytes a byte at a time", 4000,
      css_bytes(nullptr, 1, true, [sheet_unit](size_t k) { return "\xef\xbb\xbf" + repeat(sheet_unit, k); }));
  add("css", "CSS stream, UTF-8 bytes in pieces of 7", 4000,
      css_bytes(nullptr, 7, true, [sheet_unit](size_t k) { return repeat(sheet_unit, k); }));
  add("css", "CSS stream, one piece", 4000, css_bytes(nullptr, 0, true, [sheet_unit](size_t k) { return repeat(sheet_unit, k); }));
  add("css", "CSS stream, windows-1251 a byte at a time (decoded, mapped)", 4000,
      css_bytes("windows-1251", 1, true, [](size_t k) { return repeat(".\xef\xf0 > c { \xf6: \"\xe9\" } /* \xe2 */\n", k); }));
  add("css", "CSS bytes, windows-1251 in one call (every other code point two bytes of text)", 4000,
      css_bytes("windows-1251", 0, false, [](size_t k) { return repeat("a\xef b{c:\xf0 d}\n", k); }));
  add("css", "CSS stream, UTF-16LE a byte at a time", 4000,
      css_bytes("utf-16le", 1, true, [](size_t k) {
        std::string out;
        for (size_t i = 0; i < k; i++) {
          out += std::string("a\0.\0b\0{\0c\0:\0\xe9\0}\0 \0", 16);
        }
        return out;
      }));
  add("css", "CSS bytes, ISO-2022-JP in one call (a new run at every shift, the worst case of the map)", 3000,
      css_bytes("iso-2022-jp", 0, false, [](size_t k) { return repeat("a{b:\x1b$BF|\x1b(B}", k); }));
  add("css", "CSS stream, ISO-2022-JP in pieces of 3", 3000,
      css_bytes("iso-2022-jp", 3, true, [](size_t k) { return repeat("a{b:\x1b$BF|\x1b(B}", k); }));
  add("css", "CSS bytes, gb18030 four-byte characters in one call", 3000,
      css_bytes("gb18030", 0, false, [](size_t k) { return repeat("a{b:\"\x95" "2\x82" "6\"}", k); }));
  add("css", "CSS bytes, a long invalid sequence run (every byte replaced)", 8000,
      css_bytes("shift_jis", 0, false, [](size_t k) { return "a{b:\"" + repeat("\x81\xff", k) + "\"}"; }));
  add("css", "CSS stream, a charset declaration then text, a byte at a time", 4000,
      css_bytes(nullptr, 1, true, [sheet_unit](size_t k) { return "@charset \"windows-1251\"; " + repeat(sheet_unit, k); }));
  add("css", "CSS a number of 8000 digits", 1, css_bytes(nullptr, 0, false, [](size_t k) { return "a{b:" + std::string(8000 * k, '9') + "}"; }));
  add("css", "CSS many long numbers", 100, css_bytes(nullptr, 0, false, [](size_t k) { return repeat("a{b:" + std::string(300, '7') + "." + std::string(300, '1') + "e-9}", k); }));

  add("css", "CSS serialize nested rules", 1500, css_serialize([](size_t k) { return repeat("a{", k) + repeat("}", k); }));
  add("css", "CSS serialize tokens that need a comment between them", 8000,
      css_serialize([](size_t k) { return "x{y:" + repeat("a/**/b/**/1/**/2", k) + "}"; }));

  // ---- sniffing ----
  const std::vector<std::pair<const char *, std::string>> junk = {
      {"<meta with no attributes", "<meta "},
      {"<meta charset= never closed", "<meta charset="},
      {"<meta http-equiv content never closed", "<meta http-equiv=content-type content=\""},
      {"<meta repeated attribute names", "<meta a=b a=c a=d "},
      {"<meta/ and <meta> forms", "<meta/ x=y><meta>"},
      {"<!-- comment openers", "<!--"},
      {"<! and <? and </ runs", "<!x </ <? <x"},
      {"less-than signs", "<"},
      {"partial meta", "<met"},
      {"long attribute name", "<meta aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
  };
  for (const auto & j : junk) {
    const std::string unit = j.second;
    Shape & sh = add("sniffing", (std::string("1 MiB of ") + j.first).c_str(), 64 * 1024 / unit.size(),
        [unit](size_t k) { return sniff_run(repeat(unit, k)); });
    sh.flat = true;
    sh.per_byte = 0;
    sh.note = "the prescan reads the first 1024 bytes only";
  }
  add("sniffing", "the whole parse of 1 MiB of <meta junk", 2000, [](size_t k) {
    TreeRun r;
    r.input = repeat("<meta http-equiv=content-type content=\"x\" charset=y ", k);
    return tree_run(r);
  });

  // ---- the XML parser's DTD: declarations, references and the start tags that read them ----
  // Each declaration used to be found by walking the ones before it, in the parser (charged a unit a step, so a subset of
  // 16,384 entity declarations ran out of the default work budget) and in the doctype's bindings; each start tag walked every
  // declaration for its defaults; every entity expansion walked the document's physical-source table.
  const std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\"/>";
  add("xml", "entity declarations", 1024, xml_doc([svg](size_t k) {
    return xml_subset(k, [](size_t i) { return "<!ENTITY e" + std::to_string(i) + " \"value " + std::to_string(i) + "\">"; }, svg);
  }));
  add("xml", "parameter entity declarations", 1024, xml_doc([svg](size_t k) {
    return xml_subset(k, [](size_t i) { return "<!ENTITY % p" + std::to_string(i) + " \"value\">"; }, svg);
  }));
  add("xml", "element declarations", 1024, xml_doc([svg](size_t k) {
    return xml_subset(k, [](size_t i) { return "<!ELEMENT e" + std::to_string(i) + " ANY>"; }, svg);
  }));
  add("xml", "notation declarations", 1024, xml_doc([svg](size_t k) {
    return xml_subset(k, [](size_t i) { return "<!NOTATION n" + std::to_string(i) + " SYSTEM \"x\">"; }, svg);
  }));
  add("xml", "attribute-list declarations, one element each", 1024, xml_doc([svg](size_t k) {
    return xml_subset(k, [](size_t i) { return "<!ATTLIST e" + std::to_string(i) + " a CDATA #IMPLIED>"; }, svg);
  }));
  add("xml", "attribute-list declarations of one element", 1024, xml_doc([](size_t k) {
    return xml_subset(k, [](size_t i) { return "<!ATTLIST r a" + std::to_string(i) + " CDATA #IMPLIED>"; }, "<r/>");
  }));
  add("xml", "a declaration repeated (the first binds)", 1024, xml_doc([svg](size_t k) {
    return xml_subset(k, [](size_t) { return std::string("<!ENTITY e \"value\">"); }, svg);
  }));
  add("xml", "start tags against many attribute-list declarations", 1024, xml_doc([](size_t k) {
    std::string body = "<r>";
    for (size_t i = 0; i < k; i++) {
      body += "<e" + std::to_string(i) + "/>";
    }
    return xml_subset(k, [](size_t i) { return "<!ATTLIST e" + std::to_string(i) + " a CDATA \"d\">"; }, body + "</r>");
  }));
  add("xml", "references to many entities", 1024, xml_doc([](size_t k) {
    std::string body = "<r>";
    for (size_t i = 0; i < k; i++) {
      body += "&e" + std::to_string(i) + ";";
    }
    return xml_subset(k, [](size_t i) { return "<!ENTITY e" + std::to_string(i) + " \"v\">"; }, body + "</r>");
  }));
  add("xml", "references to one entity", 4096, xml_doc([](size_t k) {
    return xml_subset(1, [](size_t) { return std::string("<!ENTITY e \"lol\">"); }, "<r>" + repeat("&e;", k) + "</r>");
  }));
  add("xml", "references to one entity in an attribute value", 4096, xml_doc([](size_t k) {
    return xml_subset(1, [](size_t) { return std::string("<!ENTITY e \"lol\">"); }, "<r a=\"" + repeat("&e;", k) + "\"/>");
  }));
  add("xml", "parameter entity references", 4096, xml_doc([svg](size_t k) {
    return xml_subset(1, [k](size_t) { return "<!ENTITY % p \"<!-- c -->\">" + repeat("%p;", k); }, svg);
  }));
  Shape & blowup = add("xml", "one large entity referenced many times", 256, xml_doc([](size_t k) {
    return xml_subset(1, [](size_t) { return "<!ENTITY a \"" + std::string(1000, 'a') + "\">"; }, "<r>" + repeat("&a;", k) + "</r>");
  }));
  const char * expansion_note = "the work is the expansion's, a thousand bytes a reference, and not the input's";
  blowup.per_byte = 0;
  blowup.note = expansion_note; // (set before the next add, which may move the vector)
  Shape & blowup_attribute = add("xml", "one large entity referenced many times in an attribute value", 256, xml_doc([](size_t k) {
    return xml_subset(1, [](size_t) { return "<!ENTITY a \"" + std::string(1000, 'a') + "\">"; }, "<r a=\"" + repeat("&a;", k) + "\"/>");
  }));
  blowup_attribute.per_byte = 0;
  blowup_attribute.note = expansion_note;
  return s;
}

// ---- the checker --------------------------------------------------------------------------------

struct Row {
  const Shape * shape;
  size_t units[3];
  Measured m[3];
};

// The failures of a measured shape, empty if it holds.
std::vector<std::string> judge(const Shape & sh, const Row & row) {
  std::vector<std::string> bad;
  for (int i = 0; i < 3; i++) {
    if (!row.m[i].ok) {
      bad.push_back("at " + std::to_string(row.units[i]) + " units: " + row.m[i].why);
    }
  }
  const double w0 = (double)std::max<size_t>(row.m[0].work, 1);
  const double w1 = (double)std::max<size_t>(row.m[1].work, 1);
  const double w2 = (double)std::max<size_t>(row.m[2].work, 1);
  if (sh.flat) {
    if (w2 / w0 > 1.5) {
      bad.push_back("the work should not depend on the input, and grew " + std::to_string(w2 / w0) + " times for 16 times the input");
    }
  }
  else {
    if (w1 / w0 > sh.bound) {
      bad.push_back("work grew " + std::to_string(w1 / w0) + " times for 4 times the input (n to 4n); the bound is " + std::to_string(sh.bound));
    }
    if (w2 / w1 > sh.bound) {
      bad.push_back("work grew " + std::to_string(w2 / w1) + " times for 4 times the input (4n to 16n); the bound is " + std::to_string(sh.bound));
    }
  }
  if (sh.per_byte > 0) {
    for (int i = 0; i < 3; i++) {
      const double cap = sh.per_byte * (double)row.m[i].bytes + 8192.0;
      if ((double)row.m[i].work > cap) {
        bad.push_back("work " + std::to_string(row.m[i].work) + " for " + std::to_string(row.m[i].bytes) + " bytes exceeds " +
                      std::to_string(sh.per_byte) + " per byte");
      }
    }
  }
  return bad;
}

Row measure(const Shape & sh) {
  Row row;
  row.shape = &sh;
  const size_t mult[3] = {1, 4, 16};
  for (int i = 0; i < 3; i++) {
    row.units[i] = sh.n * mult[i];
    row.m[i] = sh.run(row.units[i]);
  }
  return row;
}

} // namespace

// ---- the suite ----------------------------------------------------------------------------------

TEST(Quadratic, EveryShapeGrowsLinearly) {
  const std::vector<Shape> shapes = build_shapes();
  ASSERT_GE(shapes.size(), 190u);
  std::map<std::string, size_t> per_group;
  FILE * report = nullptr;
  if (const char * path = std::getenv("GTEXT_QUAD_REPORT")) {
    report = std::fopen(path, "w");
  }
  size_t failures = 0;
  std::printf("%-12s %-72s %10s %10s %10s %7s %7s\n", "group", "shape", "work(n)", "work(4n)", "work(16n)", "4n/n", "16n/4n");
  for (const Shape & sh : shapes) {
    per_group[sh.group]++;
    const Row row = measure(sh);
    const double w0 = (double)std::max<size_t>(row.m[0].work, 1);
    std::printf("%-12s %-72s %10zu %10zu %10zu %7.2f %7.2f\n", sh.group.c_str(), sh.name.substr(0, 72).c_str(), row.m[0].work, row.m[1].work,
        row.m[2].work, (double)row.m[1].work / w0, (double)row.m[2].work / (double)std::max<size_t>(row.m[1].work, 1));
    if (report) {
      std::fprintf(report, "%s\t%s\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\n", sh.group.c_str(), sh.name.c_str(), row.units[0], row.m[0].bytes,
          row.m[0].work, row.m[1].bytes, row.m[1].work, row.m[2].bytes, row.m[2].work);
    }
    for (const std::string & why : judge(sh, row)) {
      failures++;
      ADD_FAILURE() << "[" << sh.group << "] " << sh.name << ": " << why << (sh.note.empty() ? "" : " (" + sh.note + ")");
    }
  }
  if (report) {
    std::fclose(report);
  }
  for (const auto & g : per_group) {
    std::printf("group %-12s %zu shapes\n", g.first.c_str(), g.second);
  }
  std::printf("%zu shapes, %zu failures\n", shapes.size(), failures);
}

TEST(Quadratic, EveryGroupTheSpecNamesHasShapes) {
  const std::vector<Shape> shapes = build_shapes();
  std::map<std::string, size_t> count;
  for (const Shape & sh : shapes) {
    count[sh.group]++;
  }
  for (const char * g : {"nesting", "formatting", "implied ends", "tables", "foreign", "fragments", "attributes", "runs", "tokenizer",
           "serializer", "encoding", "sniffing", "api", "css", "xml"}) {
    EXPECT_GT(count[g], 0u) << g;
  }
  // The shape names are unique (the callgrind tool finds a shape by name).
  std::map<std::string, int> names;
  for (const Shape & sh : shapes) {
    EXPECT_EQ(++names[sh.name], 1) << sh.name;
  }
}

// Controls: the checker must say no to a measure that grows too fast, and yes to one that does not.

namespace {
Shape synthetic(const char * name, std::function<size_t(size_t)> work, bool flat = false) {
  Shape s("control", name, 1000, [work](size_t k) {
    Measured m;
    m.bytes = k;
    m.work = work(k);
    return m;
  });
  s.flat = flat;
  s.per_byte = 0;
  return s;
}
} // namespace

TEST(QuadraticControls, TheSuiteFailsOnAQuadraticShape) {
  const Shape sh = synthetic("n squared", [](size_t k) { return k * k; });
  EXPECT_FALSE(judge(sh, measure(sh)).empty());
}

TEST(QuadraticControls, TheSuiteFailsOnAShapeThatIsQuadraticOnlyPastTheSmallestSize) {
  const Shape sh = synthetic("linear then quadratic", [](size_t k) { return k < 5000 ? 8 * k : k * k / 600; });
  EXPECT_FALSE(judge(sh, measure(sh)).empty());
}

TEST(QuadraticControls, TheSuiteFailsOnN4OverNAndN16OverN4Separately) {
  // Linear from n to 4n and quadratic from 4n to 16n, and the other way round.
  EXPECT_FALSE(judge(synthetic("late", [](size_t k) { return k <= 4000 ? 10 * k : 10 * 4000 + (k - 4000) * (k - 4000) / 4; }),
      measure(synthetic("late", [](size_t k) { return k <= 4000 ? 10 * k : 10 * 4000 + (k - 4000) * (k - 4000) / 4; }))).empty());
  EXPECT_FALSE(judge(synthetic("early", [](size_t k) { return k <= 4000 ? k * k / 40 : 400000 + 100 * k; }),
      measure(synthetic("early", [](size_t k) { return k <= 4000 ? k * k / 40 : 400000 + 100 * k; }))).empty());
}

TEST(QuadraticControls, TheSuiteFailsOnAFlatShapeThatGrows) {
  const Shape sh = synthetic("grows", [](size_t k) { return 2000 + k; }, true);
  EXPECT_FALSE(judge(sh, measure(sh)).empty());
}

TEST(QuadraticControls, TheSuiteAcceptsLinearAndFlatShapes) {
  EXPECT_TRUE(judge(synthetic("linear", [](size_t k) { return 7 * k + 300; }), measure(synthetic("linear", [](size_t k) { return 7 * k + 300; }))).empty());
  const Shape flat = synthetic("flat", [](size_t) { return 3000; }, true);
  EXPECT_TRUE(judge(flat, measure(flat)).empty());
  const Shape nlogn = synthetic("n log n", [](size_t k) { return (size_t)((double)k * std::log2((double)k)); });
  EXPECT_TRUE(judge(nlogn, measure(nlogn)).empty()) << "an n log n shape (the attribute sort) is within the default bound at these sizes";
}

TEST(QuadraticControls, TheAbsoluteBoundIsEnforced) {
  Shape sh = synthetic("heavy", [](size_t k) { return 500 * k; });
  sh.per_byte = 112;
  EXPECT_FALSE(judge(sh, measure(sh)).empty());
}

TEST(QuadraticControls, AShapeThatFailsToRunIsAFailureNotAZero) {
  Shape sh("control", "broken", 10, [](size_t) {
    Measured m;
    m.ok = false;
    m.why = "did not run";
    return m;
  });
  EXPECT_FALSE(judge(sh, measure(sh)).empty());
}

// One shape at its three sizes, each in a function of its own that callgrind collects (--toggle-collect): what
// tools/html/quadratic-instructions.py runs. Not volatile and not inlined, so that the name is in the profile.
extern "C" {
__attribute__((noinline, used)) size_t quad_region_1(const void * shape, size_t units) {
  return static_cast<const Shape *>(shape)->run(units).work;
}
__attribute__((noinline, used)) size_t quad_region_4(const void * shape, size_t units) {
  return static_cast<const Shape *>(shape)->run(units).work;
}
__attribute__((noinline, used)) size_t quad_region_16(const void * shape, size_t units) {
  return static_cast<const Shape *>(shape)->run(units).work;
}
}

TEST(Quadratic, ListShapes) {
  if (!std::getenv("GTEXT_QUAD_LIST")) {
    GTEST_SKIP() << "GTEXT_QUAD_LIST=1 lists the shapes, one per line: group, name, units";
  }
  for (const Shape & sh : build_shapes()) {
    std::printf("SHAPE\t%s\t%s\t%zu\n", sh.group.c_str(), sh.name.c_str(), sh.n);
  }
}

TEST(Quadratic, RunOne) {
  const char * which = std::getenv("GTEXT_QUAD_ONE");
  if (!which) {
    GTEST_SKIP() << "GTEXT_QUAD_ONE=<shape name> runs one shape at n, 4n and 16n, each in a function callgrind can collect";
  }
  const std::vector<Shape> shapes = build_shapes();
  for (const Shape & sh : shapes) {
    if (sh.name == which) {
      const size_t a = quad_region_1(&sh, sh.n);
      const size_t b = quad_region_4(&sh, sh.n * 4);
      const size_t c = quad_region_16(&sh, sh.n * 16);
      std::printf("RUNONE\t%s\t%zu\t%zu\t%zu\n", sh.name.c_str(), a, b, c);
      return;
    }
  }
  FAIL() << "no shape named " << which;
}
