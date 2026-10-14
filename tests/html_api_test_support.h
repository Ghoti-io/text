/**
 * @file
 *
 * Shared by the tests of the public HTML API (tests/test-html-api*.cpp): one way to call each entry
 * point behind a uniform shape, the tree in the tree-construction dump format, and the result
 * flattened to something comparable.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GTEXT_TESTS_HTML_API_TEST_SUPPORT_H
#define GHOTI_IO_GTEXT_TESTS_HTML_API_TEST_SUPPORT_H

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <ghoti.io/text/html.h>

#include "../src/markup/markup_internal.h"
#include "../tools/conformance/html_tree_dump.h"

namespace html_api_test {

inline std::string dump_doc(const GTEXT_Markup_Document * doc) {
  size_t n = 0;
  char * d = htd_dump(doc, &n);
  std::string s(d ? d : "", n);
  std::free(d);
  return s;
}

inline std::string dump_fragment(const GTEXT_Markup_Node * fragment) {
  size_t n = 0;
  char * d = htd_dump_children(fragment, &n);
  std::string s(d ? d : "", n);
  std::free(d);
  return s;
}

struct Err {
  int code;
  std::string name;
  size_t line, column, offset;
  bool operator==(const Err & o) const {
    return code == o.code && name == o.name && line == o.line && column == o.column && offset == o.offset;
  }
};

/// Everything a call produced, flattened. The document and fragment are freed by the destructor of
/// the owner, not by this.
struct Outcome {
  GTEXT_HTML_Status status = GTEXT_HTML_OK;
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_Markup_Node * fragment = nullptr;
  std::string tree;     // dump of the document, or of the fragment's children
  GTEXT_HTML_Status result_status = GTEXT_HTML_OK;
  GTEXT_Markup_Mode mode = GTEXT_MARKUP_MODE_NO_QUIRKS;
  std::string encoding;
  GTEXT_Markup_Encoding_Source source = GTEXT_MARKUP_ENCODING_UNKNOWN;
  std::string message;
  size_t input_bytes = 0;
  size_t node_count = 0;
  GTEXT_HTML_Encoding_Certainty confidence = GTEXT_HTML_CERTAIN;
  bool reparsed = false;
  bool reparse_abandoned = false;
  size_t bom_length = 0;
  bool transport_ignored = false;
  bool default_ignored = false;
  std::vector<Err> errors;
  size_t dropped = 0;

  Outcome() = default;
  Outcome(const Outcome &) = delete;
  Outcome & operator=(const Outcome &) = delete;
  Outcome(Outcome && o) noexcept {
    *this = std::move(o);
  }
  Outcome & operator=(Outcome && o) noexcept {
    if (this != &o) {
      release();
      status = o.status;
      doc = o.doc;
      fragment = o.fragment;
      tree = std::move(o.tree);
      result_status = o.result_status;
      mode = o.mode;
      encoding = std::move(o.encoding);
      source = o.source;
      message = std::move(o.message);
      input_bytes = o.input_bytes;
      node_count = o.node_count;
      confidence = o.confidence;
      reparsed = o.reparsed;
      reparse_abandoned = o.reparse_abandoned;
      bom_length = o.bom_length;
      transport_ignored = o.transport_ignored;
      default_ignored = o.default_ignored;
      errors = std::move(o.errors);
      dropped = o.dropped;
      o.doc = nullptr;
      o.fragment = nullptr;
    }
    return *this;
  }
  ~Outcome() {
    release();
  }
  void release() {
    if (doc) {
      gtext_markup_document_free(doc);
    }
    doc = nullptr;
    fragment = nullptr;
  }
  bool ok() const {
    return status == GTEXT_HTML_OK;
  }
};

/// Fill the result-derived fields of an Outcome (and free the result).
inline void harvest(Outcome & o, GTEXT_HTML_Result & r) {
  o.result_status = r.status;
  o.mode = r.mode;
  o.encoding = r.encoding ? r.encoding : "";
  o.source = r.encoding_source;
  o.message = r.message ? r.message : "";
  o.input_bytes = r.input_bytes;
  o.node_count = r.node_count;
  o.confidence = r.encoding_confidence;
  o.reparsed = r.reparsed;
  o.reparse_abandoned = r.reparse_abandoned;
  o.bom_length = r.bom_length;
  o.transport_ignored = r.transport_encoding_ignored;
  o.default_ignored = r.default_encoding_ignored;
  for (size_t i = 0; i < gtext_html_result_error_count(&r); i++) {
    GTEXT_HTML_Error e;
    if (gtext_html_result_error_at(&r, i, &e)) {
      o.errors.push_back({(int)e.code, e.name, e.line, e.column, e.byte_offset});
    }
  }
  o.dropped = gtext_html_result_errors_dropped(&r);
  gtext_html_result_free(&r);
}

inline void finish_outcome(Outcome & o) {
  if (o.status == GTEXT_HTML_OK) {
    o.tree = o.fragment ? dump_fragment(o.fragment) : dump_doc(o.doc);
  }
}

/// The contexts the entry-point table uses for fragments.
inline GTEXT_HTML_Fragment_Context ctx(GTEXT_HTML_Namespace ns, const char * name) {
  GTEXT_HTML_Fragment_Context c;
  std::memset(&c, 0, sizeof(c));
  c.ns = ns;
  c.local_name = name;
  return c;
}

/// How a piece of input is cut for an incremental parser.
struct Cut {
  enum Kind { WHOLE, BYTES, SPLIT, PIECES, RANDOM } kind = WHOLE;
  size_t at = 0; // SPLIT: the offset; PIECES: the size; RANDOM: the seed
};

inline std::vector<std::pair<const char *, size_t>> pieces_of(const std::string & s, const Cut & cut) {
  std::vector<std::pair<const char *, size_t>> out;
  const char * d = s.data();
  const size_t n = s.size();
  switch (cut.kind) {
    case Cut::WHOLE:
      out.push_back({d, n});
      break;
    case Cut::BYTES:
      for (size_t i = 0; i < n; i++) {
        out.push_back({d + i, 1});
      }
      break;
    case Cut::SPLIT: {
      const size_t at = std::min(cut.at, n);
      out.push_back({d, at});
      out.push_back({d + at, n - at});
      break;
    }
    case Cut::PIECES:
      for (size_t i = 0; i < n; i += cut.at) {
        out.push_back({d + i, std::min(cut.at, n - i)});
      }
      break;
    case Cut::RANDOM: {
      uint64_t state = cut.at * 6364136223846793005ULL + 1442695040888963407ULL;
      for (size_t i = 0; i < n;) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const size_t take = std::min<size_t>(1 + (state >> 33) % 9, n - i);
        out.push_back({d + i, take});
        i += take;
      }
      break;
    }
  }
  return out;
}

/// One way to parse: document, or fragment in a context; whole or by parser with a cut.
struct Call {
  const char * name;
  bool fragment;
  bool via_parser;
  Cut cut;
};

inline Outcome run_call(const Call & c, const std::string & input, const GTEXT_HTML_Parse_Options * opts,
    const GTEXT_HTML_Fragment_Context * context, bool want_result = true) {
  Outcome o;
  GTEXT_HTML_Result r_storage;
  std::memset(&r_storage, 0, sizeof(r_storage));
  GTEXT_HTML_Result * const result = want_result ? &r_storage : nullptr;
  GTEXT_HTML_Result & r = r_storage;
  if (!c.via_parser) {
    if (c.fragment) {
      o.status = gtext_html_parse_fragment(context, input.data(), input.size(), opts, &o.doc, &o.fragment, result);
    }
    else {
      o.status = gtext_html_parse(input.data(), input.size(), opts, &o.doc, result);
    }
  }
  else {
    GTEXT_HTML_Parser * p = nullptr;
    GTEXT_HTML_Status s = c.fragment ? gtext_html_parser_new_fragment(context, opts, &p)
                                      : gtext_html_parser_new(opts, &p);
    o.status = s;
    if (s == GTEXT_HTML_OK) {
      for (const auto & piece : pieces_of(input, c.cut)) {
        s = gtext_html_parser_feed(p, piece.first, piece.second);
        if (s != GTEXT_HTML_OK) {
          break;
        }
      }
      o.status = s;
      if (c.fragment) {
        o.status = gtext_html_parser_finish_fragment(p, &o.doc, &o.fragment, result);
      }
      else {
        o.status = gtext_html_parser_finish(p, &o.doc, result);
      }
      gtext_html_parser_free(p);
    }
    else {
      std::memset(&r, 0, sizeof(r));
      r.status = s; // there is no result from a parser that was not made
      r.message = gtext_html_status_string(s);
    }
  }
  if (want_result) {
    harvest(o, r);
  }
  finish_outcome(o);
  return o;
}

/// The eight limits, each with an input that reaches it and the smallest limit under which that input parses.
struct LimitRow {
  const char * name;
  GTEXT_HTML_Status status;
  void (*set)(GTEXT_HTML_Parse_Options *, size_t);
  const char * input;
  size_t threshold; // the smallest limit under which the parse succeeds
};

inline const LimitRow kLimitRows[] = {
    {"input", GTEXT_HTML_E_LIMIT_INPUT, [](GTEXT_HTML_Parse_Options * o, size_t n) { o->max_input_bytes = n; },
        "<p>hello</p>", 12},
    {"open_elements", GTEXT_HTML_E_LIMIT_DEPTH,
        [](GTEXT_HTML_Parse_Options * o, size_t n) { o->max_open_elements = n; }, "<div><div>", 0 /* per entry */},
    {"name", GTEXT_HTML_E_LIMIT_NAME, [](GTEXT_HTML_Parse_Options * o, size_t n) { o->max_name_bytes = n; },
        "<abcdefghij>", 10},
    {"value", GTEXT_HTML_E_LIMIT_VALUE,
        [](GTEXT_HTML_Parse_Options * o, size_t n) { o->max_attribute_bytes = n; }, "<a href=\"abcdefgh\">", 8},
    {"comment", GTEXT_HTML_E_LIMIT_COMMENT,
        [](GTEXT_HTML_Parse_Options * o, size_t n) { o->max_comment_bytes = n; }, "<!--abcdefgh-->", 8},
    {"text", GTEXT_HTML_E_LIMIT_TEXT, [](GTEXT_HTML_Parse_Options * o, size_t n) { o->max_text_bytes = n; },
        "<p>abcdefgh</p>", 8},
    {"attributes", GTEXT_HTML_E_LIMIT_ATTRIBUTES,
        [](GTEXT_HTML_Parse_Options * o, size_t n) { o->max_attributes = n; }, "<a b c d>", 3},
    {"nodes", GTEXT_HTML_E_LIMIT_NODES, [](GTEXT_HTML_Parse_Options * o, size_t n) { o->max_nodes = n; },
        "<p>a<b>b</b><i>c</i></p>", 0 /* found, below */},
};


} // namespace html_api_test

#endif // GHOTI_IO_GTEXT_TESTS_HTML_API_TEST_SUPPORT_H
