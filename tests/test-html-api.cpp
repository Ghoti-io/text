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
 * The public HTML API (include/ghoti.io/text/html.h), entry point by entry point.
 *
 * Four things are held to the same table: gtext_html_parse(), gtext_html_parse_fragment(), the
 * incremental parser (whole, a byte at a time, cut in the middle), and the fragment form of the
 * incremental parser. gtext_html_parse_file() is in test-html-api-file.cpp and the allocation sweeps
 * in test-html-api-oom.cpp. What the table checks for each: the arguments (NULL, empty, huge), every
 * limit (its own status, the boundary exact, no document), the options (defaults, the error cap, the
 * count-only mode, the scripting flag, the encoding labels), the result (status, message, mode, the
 * errors with their positions, the encoding and its source), the ownership of the document and the
 * result, the sticky failure of the incremental parser, and that however the input is cut the answer
 * is the same.
 *
 * What the tree is, and that it is the standard's, is held by the vectors (make
 * conformance-html-tree runs them through this API as well as through the builder's own); this file
 * checks that the layer over the builder passes the builder's answer through unchanged.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/text/html.h>

#include "../src/html/html_tree_internal.h"
#include "html_api_test_support.h"
#include "markup_test_support.h"

using namespace html_api_test;
using namespace std::string_view_literals;

namespace {

// The four ways in, as one table. A fragment call parses in a `div`.
const Call kCalls[] = {
    {"parse", false, false, {}},
    {"parser-whole", false, true, {Cut::WHOLE, 0}},
    {"parser-bytes", false, true, {Cut::BYTES, 0}},
    {"parser-split", false, true, {Cut::SPLIT, 3}},
    {"fragment", true, false, {}},
    {"parser-fragment-whole", true, true, {Cut::WHOLE, 0}},
    {"parser-fragment-bytes", true, true, {Cut::BYTES, 0}},
};

std::string call_name(const ::testing::TestParamInfo<Call> & info) {
  std::string n = info.param.name;
  for (char & c : n) {
    if (c == '-') {
      c = '_';
    }
  }
  return n;
}

GTEXT_Markup_Node * find_element(const GTEXT_Markup_Node * start, const char * ns, const char * local) {
  for (GTEXT_Markup_Node * n = gtext_markup_node_next_in_order(start, start, false); n;
       n = gtext_markup_node_next_in_order(start, n, false)) {
    if (gtext_markup_node_kind(n) == GTEXT_MARKUP_KIND_ELEMENT && gtext_markup_element_is(n, ns, local)) {
      return n;
    }
  }
  return nullptr;
}

class HtmlApi : public ::testing::TestWithParam<Call> {
 protected:
  GTEXT_HTML_Fragment_Context div_ = ctx(GTEXT_HTML_NS_HTML, "div");

  Outcome run(const std::string & input, const GTEXT_HTML_Parse_Options * opts = nullptr) {
    Outcome o = run_call(GetParam(), input, opts, &div_);
    check_invariants(o);
    return o;
  }

  // What every outcome must satisfy, whatever the input.
  static void check_invariants(const Outcome & o) {
    EXPECT_EQ(o.result_status, o.status);
    EXPECT_FALSE(o.message.empty());
    if (o.ok()) {
      EXPECT_NE(o.doc, nullptr);
      const char * problem = markup_check_invariants(o.doc);
      EXPECT_STREQ(problem ? problem : "ok", "ok");
    }
    else {
      EXPECT_EQ(o.doc, nullptr) << "a failed parse must not write its output";
      EXPECT_EQ(o.fragment, nullptr);
    }
  }
};

INSTANTIATE_TEST_SUITE_P(EntryPoints, HtmlApi, ::testing::ValuesIn(kCalls), call_name);

// ---- what comes back -------------------------------------------------------------------------------

TEST_P(HtmlApi, ParsesAndReportsTheResult) {
  const Outcome o = run("<p>a<b>b</p>c");
  ASSERT_TRUE(o.ok());
  if (GetParam().fragment) {
    // Parsed as the contents of a div: no html, head or body.
    EXPECT_EQ(o.tree,
        "| <p>\n|   \"a\"\n|   <b>\n|     \"b\"\n| <b>\n|   \"c\"\n");
    EXPECT_EQ(o.mode, GTEXT_MARKUP_MODE_NO_QUIRKS);
    // `</p>` closes a p with the b still open (offset 8), and the b is still open at the end (13).
    ASSERT_EQ(o.errors.size(), 2u);
    EXPECT_EQ(o.errors[0], (Err{(int)GTEXT_HTML_ERROR_OPEN_ELEMENTS_AT_END_TAG, "open-elements-at-end-tag", 1, 9, 8}));
    EXPECT_EQ(o.errors[1], (Err{(int)GTEXT_HTML_ERROR_OPEN_ELEMENTS_AT_EOF, "open-elements-at-eof", 1, 14, 13}));
  }
  else {
    EXPECT_EQ(o.tree,
        "| <html>\n|   <head>\n|   <body>\n|     <p>\n|       \"a\"\n|       <b>\n|         \"b\"\n|     <b>\n|       \"c\"\n");
    EXPECT_EQ(o.mode, GTEXT_MARKUP_MODE_QUIRKS) << "no doctype";
    ASSERT_GE(o.errors.size(), 1u);
    EXPECT_EQ(o.errors[0].name, "missing-doctype");
    EXPECT_EQ(o.errors[0].code, (int)GTEXT_HTML_ERROR_MISSING_DOCTYPE);
    EXPECT_EQ(o.errors[0].line, 1u);
    EXPECT_EQ(o.errors[0].column, 1u);
    EXPECT_EQ(o.errors[0].offset, 0u);
    EXPECT_EQ(gtext_markup_document_mode(o.doc), GTEXT_MARKUP_MODE_QUIRKS) << "also set on the document";
  }
  EXPECT_EQ(o.dropped, 0u);
  EXPECT_EQ(o.input_bytes, 13u);
  EXPECT_GE(o.node_count, 4u);
  EXPECT_EQ(o.encoding, "UTF-8");
  EXPECT_EQ(o.source, GTEXT_MARKUP_ENCODING_DEFAULT);
  EXPECT_EQ(o.result_status, GTEXT_HTML_OK);
}

TEST_P(HtmlApi, TheDocumentModeComesFromTheDoctype) {
  if (GetParam().fragment) {
    GTEST_SKIP() << "a fragment has no doctype; its mode is its context's (see AttributesFormAndTheMode)";
  }
  struct Row { const char * input; GTEXT_Markup_Mode mode; };
  const Row rows[] = {
      {"<!DOCTYPE html><p>x", GTEXT_MARKUP_MODE_NO_QUIRKS},
      {"<p>x", GTEXT_MARKUP_MODE_QUIRKS},
      {"<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\"><p>x", GTEXT_MARKUP_MODE_QUIRKS},
      {"<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\" \"x\"><p>x",
          GTEXT_MARKUP_MODE_LIMITED_QUIRKS},
      {"<!DOCTYPE foo><p>x", GTEXT_MARKUP_MODE_QUIRKS},
  };
  for (const Row & r : rows) {
    const Outcome o = run(r.input);
    ASSERT_TRUE(o.ok()) << r.input;
    EXPECT_EQ(o.mode, r.mode) << r.input;
    EXPECT_EQ(gtext_markup_document_mode(o.doc), r.mode) << r.input;
  }
}

TEST_P(HtmlApi, TheEmptyInputHasATree) {
  const Outcome o = run("");
  ASSERT_TRUE(o.ok());
  if (GetParam().fragment) {
    EXPECT_EQ(o.tree, "");
    EXPECT_TRUE(o.errors.empty());
  }
  else {
    EXPECT_EQ(o.tree, "| <html>\n|   <head>\n|   <body>\n");
    EXPECT_EQ(o.mode, GTEXT_MARKUP_MODE_QUIRKS);
    ASSERT_EQ(o.errors.size(), 1u);
    EXPECT_EQ(o.errors[0].name, "missing-doctype");
  }
  EXPECT_EQ(o.input_bytes, 0u);
}

TEST_P(HtmlApi, NullDataIsAnEmptyInputOnlyWhenTheLengthIsZero) {
  // run_call passes input.data(); the NULL cases need the raw calls.
  const Call c = GetParam();
  for (int with_length = 0; with_length < 2; with_length++) {
    GTEXT_Markup_Document * doc = nullptr;
    GTEXT_Markup_Node * frag = nullptr;
    GTEXT_HTML_Result r;
    GTEXT_HTML_Status s;
    if (!c.via_parser) {
      s = c.fragment ? gtext_html_parse_fragment(&div_, nullptr, with_length ? 3 : 0, nullptr, &doc, &frag, &r)
                     : gtext_html_parse(nullptr, with_length ? 3 : 0, nullptr, &doc, &r);
    }
    else {
      GTEXT_HTML_Parser * p = nullptr;
      ASSERT_EQ(c.fragment ? gtext_html_parser_new_fragment(&div_, nullptr, &p) : gtext_html_parser_new(nullptr, &p),
          GTEXT_HTML_OK);
      s = gtext_html_parser_feed(p, nullptr, with_length ? 3 : 0);
      EXPECT_EQ(s, with_length ? GTEXT_HTML_E_INVALID : GTEXT_HTML_OK);
      s = c.fragment ? gtext_html_parser_finish_fragment(p, &doc, &frag, &r) : gtext_html_parser_finish(p, &doc, &r);
      if (with_length) {
        EXPECT_EQ(s, GTEXT_HTML_OK) << "a refused feed is the caller's mistake, not a failure of the parse";
      }
      gtext_html_parser_free(p);
    }
    if (with_length && !c.via_parser) {
      EXPECT_EQ(s, GTEXT_HTML_E_INVALID);
      EXPECT_EQ(doc, nullptr);
      EXPECT_EQ(r.status, GTEXT_HTML_E_INVALID);
    }
    else {
      EXPECT_EQ(s, GTEXT_HTML_OK);
      EXPECT_NE(doc, nullptr);
    }
    gtext_markup_document_free(doc);
    gtext_html_result_free(&r);
  }
}

TEST_P(HtmlApi, ANullOutputIsRefusedAndTheResultSaysSo) {
  const Call c = GetParam();
  GTEXT_HTML_Result r;
  std::memset(&r, 0xAB, sizeof(r)); // garbage: the call must overwrite it, not read it
  GTEXT_HTML_Status s;
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_Markup_Node * frag = nullptr;
  if (!c.via_parser) {
    s = c.fragment ? gtext_html_parse_fragment(&div_, "x", 1, nullptr, nullptr, &frag, &r)
                   : gtext_html_parse("x", 1, nullptr, nullptr, &r);
  }
  else {
    GTEXT_HTML_Parser * p = nullptr;
    ASSERT_EQ(c.fragment ? gtext_html_parser_new_fragment(&div_, nullptr, &p) : gtext_html_parser_new(nullptr, &p),
        GTEXT_HTML_OK);
    EXPECT_EQ(gtext_html_parser_feed(p, "x", 1), GTEXT_HTML_OK);
    s = c.fragment ? gtext_html_parser_finish_fragment(p, nullptr, &frag, &r) : gtext_html_parser_finish(p, nullptr, &r);
    // The parser survives a refused finish: the caller can still finish it properly.
    GTEXT_HTML_Result r2;
    const GTEXT_HTML_Status again = c.fragment ? gtext_html_parser_finish_fragment(p, &doc, &frag, &r2)
                                               : gtext_html_parser_finish(p, &doc, &r2);
    EXPECT_EQ(again, GTEXT_HTML_OK);
    gtext_html_result_free(&r2);
    gtext_html_parser_free(p);
    gtext_markup_document_free(doc);
  }
  EXPECT_EQ(s, GTEXT_HTML_E_INVALID);
  EXPECT_EQ(r.status, GTEXT_HTML_E_INVALID);
  ASSERT_NE(r.message, nullptr);
  EXPECT_EQ(r.internal, nullptr);
  gtext_html_result_free(&r);
}

TEST_P(HtmlApi, ANullResultIsAllowed) {
  const Call c = GetParam();
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_Markup_Node * frag = nullptr;
  GTEXT_HTML_Status s;
  if (!c.via_parser) {
    s = c.fragment ? gtext_html_parse_fragment(&div_, "<p>x", 4, nullptr, &doc, &frag, nullptr)
                   : gtext_html_parse("<p>x", 4, nullptr, &doc, nullptr);
  }
  else {
    GTEXT_HTML_Parser * p = nullptr;
    ASSERT_EQ(c.fragment ? gtext_html_parser_new_fragment(&div_, nullptr, &p) : gtext_html_parser_new(nullptr, &p),
        GTEXT_HTML_OK);
    ASSERT_EQ(gtext_html_parser_feed(p, "<p>x", 4), GTEXT_HTML_OK);
    s = c.fragment ? gtext_html_parser_finish_fragment(p, &doc, &frag, nullptr)
                   : gtext_html_parser_finish(p, &doc, nullptr);
    gtext_html_parser_free(p);
  }
  EXPECT_EQ(s, GTEXT_HTML_OK);
  ASSERT_NE(doc, nullptr);
  gtext_markup_document_free(doc);
}

TEST_P(HtmlApi, AHugeLengthIsALimitAndNothingIsRead) {
  // A valid pointer to four bytes and a length of SIZE_MAX (and of SIZE_MAX / 2): the input limit
  // refuses it before any byte is touched, so sanitizers see no over-read.
  const Call c = GetParam();
  if (c.via_parser && c.cut.kind != Cut::WHOLE) {
    GTEST_SKIP() << "the huge length is one feed";
  }
  for (size_t len : {SIZE_MAX, SIZE_MAX / 2, (size_t)1 << 40}) {
    GTEXT_Markup_Document * doc = nullptr;
    GTEXT_Markup_Node * frag = nullptr;
    GTEXT_HTML_Result r;
    GTEXT_HTML_Status s;
    const char four[4] = {'<', 'p', '>', 'x'};
    if (!c.via_parser) {
      s = c.fragment ? gtext_html_parse_fragment(&div_, four, len, nullptr, &doc, &frag, &r)
                     : gtext_html_parse(four, len, nullptr, &doc, &r);
    }
    else {
      GTEXT_HTML_Parser * p = nullptr;
      ASSERT_EQ(c.fragment ? gtext_html_parser_new_fragment(&div_, nullptr, &p) : gtext_html_parser_new(nullptr, &p),
          GTEXT_HTML_OK);
      EXPECT_EQ(gtext_html_parser_feed(p, four, len), GTEXT_HTML_E_LIMIT_INPUT);
      s = c.fragment ? gtext_html_parser_finish_fragment(p, &doc, &frag, &r) : gtext_html_parser_finish(p, &doc, &r);
      gtext_html_parser_free(p);
    }
    EXPECT_EQ(s, GTEXT_HTML_E_LIMIT_INPUT) << len;
    EXPECT_EQ(doc, nullptr);
    EXPECT_EQ(r.status, GTEXT_HTML_E_LIMIT_INPUT);
    gtext_html_result_free(&r);
  }
}

// ---- limits ---------------------------------------------------------------------------------------

TEST_P(HtmlApi, EveryLimitIsItsOwnStatusAndItsBoundaryIsExact) {
  std::set<GTEXT_HTML_Status> seen;
  for (const LimitRow & row : kLimitRows) {
    // The roomy parse, for the comparison and for the threshold of the two limits that depend on the tree.
    const Outcome roomy = run(row.input);
    ASSERT_TRUE(roomy.ok()) << row.name;
    size_t threshold = row.threshold;
    if (std::string(row.name) == "nodes") {
      threshold = roomy.node_count;
    }
    if (std::string(row.name) == "open_elements") {
      // html, body, div, div; a fragment's root html, div, div.
      threshold = GetParam().fragment ? 3 : 4;
    }
    if (std::string(row.name) == "input" && GetParam().fragment) {
      threshold = 12;
    }
    ASSERT_GT(threshold, 1u) << row.name;
    for (size_t n : {threshold - 1, threshold, threshold + 1, threshold * 1000}) {
      GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
      row.set(&o, n);
      const Outcome got = run(row.input, &o);
      if (n >= threshold) {
        ASSERT_TRUE(got.ok()) << row.name << " " << n << ": " << gtext_html_status_string(got.status);
        EXPECT_EQ(got.tree, roomy.tree) << row.name << " " << n;
      }
      else {
        EXPECT_EQ(got.status, row.status) << row.name << " " << n;
        EXPECT_EQ(got.doc, nullptr);
        EXPECT_EQ(got.result_status, row.status);
        seen.insert(got.status);
      }
    }
    // A zero limit is the default, never "none": the parse behaves as the roomy one.
    GTEXT_HTML_Parse_Options zero = {};
    row.set(&zero, 0);
    const Outcome z = run(row.input, &zero);
    EXPECT_TRUE(z.ok()) << row.name;
    EXPECT_EQ(z.tree, roomy.tree);
  }
  EXPECT_EQ(seen.size(), sizeof(kLimitRows) / sizeof(kLimitRows[0])) << "eight limits, eight statuses";
}

// ---- options --------------------------------------------------------------------------------------

TEST(HtmlApiOptions, TheDefaultsAreSpelledOutAndEqualToZero) {
  const GTEXT_HTML_Parse_Options d = gtext_html_parse_options_default();
  EXPECT_EQ(d.allocator, nullptr);
  EXPECT_EQ(d.max_input_bytes, (size_t)64 * 1024 * 1024);
  EXPECT_EQ(d.max_open_elements, 512u);
  EXPECT_EQ(d.max_nodes, (size_t)10 * 1000 * 1000);
  EXPECT_EQ(d.max_name_bytes, 8192u);
  EXPECT_EQ(d.max_attribute_bytes, (size_t)16 * 1024 * 1024);
  EXPECT_EQ(d.max_comment_bytes, (size_t)16 * 1024 * 1024);
  EXPECT_EQ(d.max_text_bytes, (size_t)64 * 1024 * 1024);
  EXPECT_EQ(d.max_attributes, 1024u);
  EXPECT_EQ(d.max_errors, 1024u);
  EXPECT_FALSE(d.count_errors_only);
  EXPECT_FALSE(d.scripting);
  EXPECT_EQ(d.transport_encoding, nullptr);
  EXPECT_EQ(d.default_encoding, nullptr);
  EXPECT_FALSE(d.ignore_meta);
  // Zeroed behaves as the defaults, and so does NULL.
  const std::string text = "<!DOCTYPE html><title>t</title><p class=a>x &amp; y<!--c-->";
  GTEXT_HTML_Parse_Options zero = {};
  Outcome a = run_call({"parse", false, false, {}}, text, &zero, nullptr);
  Outcome b = run_call({"parse", false, false, {}}, text, &d, nullptr);
  Outcome c = run_call({"parse", false, false, {}}, text, nullptr, nullptr);
  ASSERT_TRUE(a.ok() && b.ok() && c.ok());
  EXPECT_EQ(a.tree, b.tree);
  EXPECT_EQ(a.tree, c.tree);
  EXPECT_EQ(a.errors, b.errors);
  EXPECT_EQ(a.errors, c.errors);
}

TEST_P(HtmlApi, TheErrorCapKeepsTheFirstAndCountsTheRest) {
  std::string input;
  for (int i = 0; i < 1000; i++) {
    input += "</x>";
  }
  const Outcome all = run(input);
  ASSERT_TRUE(all.ok());
  const size_t total = all.errors.size() + all.dropped;
  EXPECT_GE(total, 1000u);
  EXPECT_LE(all.errors.size(), 1024u) << "the default cap is 1024";
  for (size_t cap : {(size_t)10, (size_t)1, (size_t)999}) {
    GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
    o.max_errors = cap;
    const Outcome capped = run(input, &o);
    ASSERT_TRUE(capped.ok());
    EXPECT_EQ(capped.errors.size(), cap);
    EXPECT_EQ(capped.dropped, total - cap) << "kept and dropped together are every error";
    // The kept ones are the first ones, in the order raised.
    for (size_t i = 0; i < cap; i++) {
      EXPECT_EQ(capped.errors[i], all.errors[i]) << i;
    }
    EXPECT_EQ(capped.tree, all.tree) << "the cap never changes the tree";
  }
}

TEST_P(HtmlApi, CountOnlyKeepsNothingAndCountsEverything) {
  std::string input;
  for (int i = 0; i < 100; i++) {
    input += "</x>";
  }
  const Outcome all = run(input);
  GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
  o.count_errors_only = true;
  const Outcome counted = run(input, &o);
  ASSERT_TRUE(counted.ok());
  EXPECT_TRUE(counted.errors.empty());
  EXPECT_EQ(counted.dropped, all.errors.size() + all.dropped);
  EXPECT_EQ(counted.tree, all.tree);
  // max_errors is then irrelevant.
  o.max_errors = 5;
  const Outcome again = run(input, &o);
  EXPECT_TRUE(again.errors.empty());
  EXPECT_EQ(again.dropped, counted.dropped);
}

TEST_P(HtmlApi, TheScriptingFlagChangesHowNoscriptParses) {
  // In the body (a noscript in the head is a different mode, where the p ends it).
  const std::string input = "<body><noscript><p>x</p></noscript>";
  GTEXT_HTML_Parse_Options off = gtext_html_parse_options_default();
  GTEXT_HTML_Parse_Options on = off;
  on.scripting = true;
  const Outcome without = run(input, &off);
  const Outcome with = run(input, &on);
  ASSERT_TRUE(without.ok() && with.ok());
  // Without the flag the noscript holds a p element holding "x"; with it, one text node, the raw text.
  const GTEXT_Markup_Node * root_off = GetParam().fragment ? without.fragment : gtext_markup_document_root(without.doc);
  const GTEXT_Markup_Node * root_on = GetParam().fragment ? with.fragment : gtext_markup_document_root(with.doc);
  const GTEXT_Markup_Node * ns_off = find_element(root_off, GTEXT_MARKUP_NS_HTML, "noscript");
  const GTEXT_Markup_Node * ns_on = find_element(root_on, GTEXT_MARKUP_NS_HTML, "noscript");
  ASSERT_NE(ns_off, nullptr);
  ASSERT_NE(ns_on, nullptr);
  ASSERT_EQ(gtext_markup_node_child_count(ns_off), 1u);
  const GTEXT_Markup_Node * child_off = gtext_markup_node_first_child(ns_off);
  ASSERT_EQ(gtext_markup_node_kind(child_off), GTEXT_MARKUP_KIND_ELEMENT);
  EXPECT_TRUE(gtext_markup_element_is(child_off, GTEXT_MARKUP_NS_HTML, "p"));
  ASSERT_EQ(gtext_markup_node_child_count(ns_on), 1u);
  const GTEXT_Markup_Node * child_on = gtext_markup_node_first_child(ns_on);
  ASSERT_EQ(gtext_markup_node_kind(child_on), GTEXT_MARKUP_KIND_TEXT);
  size_t length = 0;
  const char * data = gtext_markup_node_data(child_on, &length);
  EXPECT_EQ(std::string(data, length), "<p>x</p>");
}

TEST_P(HtmlApi, EncodingLabels) {
  const std::string input = "<p>x";
  const bool fragment = GetParam().fragment;
  for (const char * label : {"utf-8", "UTF-8", "Utf8", "  utf-8\n", "unicode-1-1-utf-8", "unicode11utf8",
           "unicode20utf8", "x-unicode20utf8"}) {
    for (int which = 0; which < 2; which++) {
      GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
      (which == 0 ? o.transport_encoding : o.default_encoding) = label;
      const Outcome got = run(input, &o);
      ASSERT_TRUE(got.ok()) << label << " " << which;
      EXPECT_EQ(got.encoding, "UTF-8");
      // A certain label is the caller's word; a default is its guess (a fragment has no guess to make: its characters are decoded).
      EXPECT_EQ(got.source, which == 0 ? GTEXT_MARKUP_ENCODING_CALLER
                                       : (fragment ? GTEXT_MARKUP_ENCODING_DEFAULT : GTEXT_MARKUP_ENCODING_CALLER_DEFAULT))
          << label;
      EXPECT_EQ(got.confidence, fragment ? GTEXT_HTML_IRRELEVANT : (which == 0 ? GTEXT_HTML_CERTAIN : GTEXT_HTML_TENTATIVE));
      EXPECT_FALSE(got.transport_ignored);
      EXPECT_FALSE(got.default_ignored);
    }
  }
  // A tentative label is the caller's guess; ignore_meta changes nothing about the source.
  for (GTEXT_HTML_Encoding_Certainty certainty : {GTEXT_HTML_CERTAIN, GTEXT_HTML_TENTATIVE}) {
    GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
    o.transport_encoding = "utf-8";
    o.transport_encoding_certainty = certainty;
    o.ignore_meta = true;
    const Outcome got = run(input, &o);
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.source, fragment || certainty == GTEXT_HTML_CERTAIN ? GTEXT_MARKUP_ENCODING_CALLER
                                                                       : GTEXT_MARKUP_ENCODING_CALLER_DEFAULT);
  }
  // Every other encoding of the standard is read now; a document names it as the standard does.
  struct Name { const char * label; const char * name; };
  for (const Name & n : {Name{"windows-1252", "windows-1252"}, Name{"latin1", "windows-1252"}, Name{"ascii", "windows-1252"},
           Name{"iso-8859-1", "windows-1252"}, Name{"utf-16", "UTF-16LE"}, Name{"UTF-16LE", "UTF-16LE"}, Name{"unicodefffe", "UTF-16BE"},
           Name{"shift_jis", "Shift_JIS"}, Name{"sjis", "Shift_JIS"}, Name{"EUC-KR", "EUC-KR"}, Name{"gbk", "GBK"}, Name{"big5", "Big5"},
           Name{"iso-2022-jp", "ISO-2022-JP"}, Name{"x-user-defined", "x-user-defined"}, Name{"hz-gb-2312", "replacement"}}) {
    for (int which = 0; which < 2; which++) {
      GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
      (which == 0 ? o.transport_encoding : o.default_encoding) = n.label;
      const Outcome got = run(input, &o);
      ASSERT_TRUE(got.ok()) << n.label;
      EXPECT_EQ(got.encoding, fragment ? "UTF-8" : n.name) << n.label << " " << which;
      EXPECT_FALSE(got.transport_ignored);
    }
  }
  // A label that names no encoding is ignored, whichever option carries it, and the result says so.
  for (const char * label : {"utf-80", "", " ", "utf-8x", "utf 8", "nonsense", "utf-7", "cp1252x"}) {
    for (int which = 0; which < 2; which++) {
      GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
      (which == 0 ? o.transport_encoding : o.default_encoding) = label;
      const Outcome got = run(input, &o);
      ASSERT_TRUE(got.ok()) << "[" << label << "] " << which;
      EXPECT_EQ(got.encoding, "UTF-8");
      EXPECT_EQ(got.source, GTEXT_MARKUP_ENCODING_DEFAULT);
      EXPECT_EQ(got.transport_ignored, which == 0) << label;
      EXPECT_EQ(got.default_ignored, which == 1) << label;
    }
  }
  // Both labels: the transport label's word stands, and an unknown default beside it is still reported.
  GTEXT_HTML_Parse_Options both = gtext_html_parse_options_default();
  both.transport_encoding = "windows-1252";
  both.default_encoding = "nonsense";
  const Outcome two = run(input, &both);
  ASSERT_TRUE(two.ok());
  EXPECT_EQ(two.encoding, fragment ? "UTF-8" : "windows-1252");
  EXPECT_TRUE(two.default_ignored);
  EXPECT_FALSE(two.transport_ignored);
}

TEST_P(HtmlApi, ABomIsSkippedAndCountedInTheOffsets) {
  const std::string bom = "\xEF\xBB\xBF";
  const std::string body = "<!DOCTYPE html><p>x</b>";
  const Outcome plain = run(body);
  const Outcome with = run(bom + body);
  ASSERT_TRUE(plain.ok() && with.ok());
  if (GetParam().fragment) {
    // Characters already decoded: a U+FEFF is text.
    EXPECT_NE(with.tree.find("\xEF\xBB\xBF"), std::string::npos) << with.tree;
    EXPECT_EQ(with.source, GTEXT_MARKUP_ENCODING_DEFAULT);
    return;
  }
  EXPECT_EQ(with.tree, plain.tree) << "the mark is not text";
  EXPECT_EQ(with.source, GTEXT_MARKUP_ENCODING_BOM);
  EXPECT_EQ(with.encoding, "UTF-8");
  EXPECT_EQ(plain.source, GTEXT_MARKUP_ENCODING_DEFAULT);
  EXPECT_EQ(with.input_bytes, body.size() + 3);
  ASSERT_EQ(with.errors.size(), plain.errors.size());
  for (size_t i = 0; i < plain.errors.size(); i++) {
    EXPECT_EQ(with.errors[i].name, plain.errors[i].name);
    EXPECT_EQ(with.errors[i].line, plain.errors[i].line);
    EXPECT_EQ(with.errors[i].column, plain.errors[i].column) << "the mark takes no column";
    EXPECT_EQ(with.errors[i].offset, plain.errors[i].offset + 3) << "but it takes three bytes";
  }
  GTEXT_Markup_Encoding_Source source;
  EXPECT_STREQ(gtext_markup_document_encoding(with.doc, &source), "UTF-8");
  EXPECT_EQ(source, GTEXT_MARKUP_ENCODING_BOM);
  // The BOM beats a transport label's source.
  GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
  o.transport_encoding = "utf-8";
  EXPECT_EQ(run(bom + body, &o).source, GTEXT_MARKUP_ENCODING_BOM);
  // The input limit counts the mark.
  o = gtext_html_parse_options_default();
  o.max_input_bytes = body.size() + 3;
  EXPECT_TRUE(run(bom + body, &o).ok());
  o.max_input_bytes = body.size() + 2;
  EXPECT_EQ(run(bom + body, &o).status, GTEXT_HTML_E_LIMIT_INPUT);
  // Not a mark: a prefix of it, or the bytes later on, are read as the bytes they are.
  for (const std::string & text : {std::string("\xEF\xBB<p>x"), std::string("\xEF<p>x"), std::string("\xEF\xBB"),
           std::string("\xEF"), std::string("<p>\xEF\xBB\xBF""x"), std::string("\xEF\xBB\xBF\xEF\xBB\xBF<p>x")}) {
    const Outcome got = run(text);
    ASSERT_TRUE(got.ok());
    const bool starts_with_mark = text.compare(0, 3, bom) == 0;
    EXPECT_EQ(got.source, starts_with_mark ? GTEXT_MARKUP_ENCODING_BOM : GTEXT_MARKUP_ENCODING_DEFAULT);
  }
}

// ---- what the tree is -----------------------------------------------------------------------------

TEST_P(HtmlApi, TheTreeIsWhatTheManualSays) {
  const std::string input = "<p a=1 a=2 B=3>x<svg viewBox=\"0 0 1 1\" xlink:href=u><foreignObject/></svg>"
                            "<math definitionURL=d><mi/></math><template><i>t</template>";
  const Outcome o = run(input);
  ASSERT_TRUE(o.ok());
  const GTEXT_Markup_Node * root = GetParam().fragment ? o.fragment : gtext_markup_document_root(o.doc);

  // HTML elements, lower case, attributes in no namespace, source order, the first of two equal names kept.
  const GTEXT_Markup_Node * p = find_element(root, GTEXT_MARKUP_NS_HTML, "p");
  ASSERT_NE(p, nullptr);
  ASSERT_EQ(gtext_markup_element_attribute_count(p), 2u);
  GTEXT_Markup_Attribute a0, a1;
  ASSERT_EQ(gtext_markup_element_attribute_at(p, 0, &a0), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_element_attribute_at(p, 1, &a1), GTEXT_MARKUP_OK);
  EXPECT_STREQ(a0.local, "a");
  EXPECT_STREQ(a0.value, "1");
  EXPECT_STREQ(a1.local, "b") << "lower-cased";
  EXPECT_EQ(a0.ns, nullptr);
  bool saw_duplicate = false;
  for (const Err & e : o.errors) {
    saw_duplicate = saw_duplicate || e.name == "duplicate-attribute";
  }
  EXPECT_TRUE(saw_duplicate);

  // Foreign content: the namespaces, the standard's names and attributes.
  const GTEXT_Markup_Node * svg = find_element(root, GTEXT_MARKUP_NS_SVG, "svg");
  ASSERT_NE(svg, nullptr);
  GTEXT_Markup_Attribute view, href;
  ASSERT_TRUE(gtext_markup_element_get_attribute(svg, nullptr, "viewBox", &view));
  EXPECT_STREQ(view.value, "0 0 1 1");
  ASSERT_TRUE(gtext_markup_element_get_attribute(svg, GTEXT_MARKUP_NS_XLINK, "href", &href));
  EXPECT_STREQ(href.prefix, "xlink");
  EXPECT_NE(find_element(root, GTEXT_MARKUP_NS_SVG, "foreignObject"), nullptr);
  const GTEXT_Markup_Node * math = find_element(root, GTEXT_MARKUP_NS_MATHML, "math");
  ASSERT_NE(math, nullptr);
  EXPECT_TRUE(gtext_markup_element_get_attribute(math, nullptr, "definitionURL", nullptr));
  EXPECT_NE(find_element(root, GTEXT_MARKUP_NS_MATHML, "mi"), nullptr);

  // A template owns its contents.
  const GTEXT_Markup_Node * tmpl = find_element(root, GTEXT_MARKUP_NS_HTML, "template");
  ASSERT_NE(tmpl, nullptr);
  EXPECT_EQ(gtext_markup_node_child_count(tmpl), 0u);
  const GTEXT_Markup_Node * content = gtext_markup_element_template_content(tmpl);
  ASSERT_NE(content, nullptr);
  EXPECT_NE(find_element(content, GTEXT_MARKUP_NS_HTML, "i"), nullptr);

  // Locations, and the byte offsets that the errors use.
  GTEXT_Markup_Location where;
  ASSERT_TRUE(gtext_markup_node_location(p, &where));
  EXPECT_EQ(where.offset, 0u);
  EXPECT_EQ(where.line, 1u);
  EXPECT_EQ(where.column, 1u);
  ASSERT_TRUE(gtext_markup_node_location(svg, &where));
  EXPECT_EQ(where.offset, input.find("<svg"));
}

// ---- failures are not leaks and are not partial ---------------------------------------------------

TEST_P(HtmlApi, AFailedCallLeavesTheOutputsAlone) {
  const Call c = GetParam();
  GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
  o.max_nodes = 3;
  GTEXT_Markup_Document * doc = reinterpret_cast<GTEXT_Markup_Document *>(0x1234);
  GTEXT_Markup_Node * frag = reinterpret_cast<GTEXT_Markup_Node *>(0x5678);
  GTEXT_HTML_Result r;
  const std::string input = "<p>a</p><p>b</p><p>c</p>";
  GTEXT_HTML_Status s;
  if (!c.via_parser) {
    s = c.fragment ? gtext_html_parse_fragment(&div_, input.data(), input.size(), &o, &doc, &frag, &r)
                   : gtext_html_parse(input.data(), input.size(), &o, &doc, &r);
  }
  else {
    GTEXT_HTML_Parser * p = nullptr;
    ASSERT_EQ(c.fragment ? gtext_html_parser_new_fragment(&div_, &o, &p) : gtext_html_parser_new(&o, &p), GTEXT_HTML_OK);
    for (const auto & piece : pieces_of(input, c.cut)) {
      if (gtext_html_parser_feed(p, piece.first, piece.second) != GTEXT_HTML_OK) {
        break;
      }
    }
    s = c.fragment ? gtext_html_parser_finish_fragment(p, &doc, &frag, &r) : gtext_html_parser_finish(p, &doc, &r);
    gtext_html_parser_free(p);
  }
  EXPECT_EQ(s, GTEXT_HTML_E_LIMIT_NODES);
  EXPECT_EQ(doc, reinterpret_cast<GTEXT_Markup_Document *>(0x1234)) << "written only on success";
  EXPECT_EQ(frag, reinterpret_cast<GTEXT_Markup_Node *>(0x5678));
  EXPECT_EQ(r.status, GTEXT_HTML_E_LIMIT_NODES);
  EXPECT_EQ(r.mode, GTEXT_MARKUP_MODE_NO_QUIRKS);
  EXPECT_EQ(r.node_count, 0u);
  // The errors raised before the failure are still reported.
  EXPECT_EQ(gtext_html_result_error_count(&r) > 0, !c.fragment);
  gtext_html_result_free(&r);
}

// ---- ownership -----------------------------------------------------------------------------------

TEST_P(HtmlApi, TheDocumentAndTheResultAreIndependent) {
  const Call c = GetParam();
  for (int result_first = 0; result_first < 2; result_first++) {
    GTEXT_Markup_Document * doc = nullptr;
    GTEXT_Markup_Node * frag = nullptr;
    GTEXT_HTML_Result r;
    GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
    ASSERT_EQ(c.fragment ? gtext_html_parse_fragment(&div_, "<p>x</b>", 8, &o, &doc, &frag, &r)
                         : gtext_html_parse("<p>x</b>", 8, &o, &doc, &r),
        GTEXT_HTML_OK);
    ASSERT_GT(gtext_html_result_error_count(&r), 0u);
    if (result_first) {
      gtext_html_result_free(&r);
      EXPECT_NE(gtext_markup_document_root(doc), nullptr);
      EXPECT_NE(gtext_markup_node_first_child(c.fragment ? frag : gtext_markup_document_root(doc)), nullptr);
      gtext_markup_document_free(doc);
    }
    else {
      gtext_markup_document_free(doc);
      GTEXT_HTML_Error e;
      EXPECT_TRUE(gtext_html_result_error_at(&r, 0, &e)) << "the errors outlive the document";
      EXPECT_NE(std::string(e.name), "");
      gtext_html_result_free(&r);
    }
  }
}

TEST_P(HtmlApi, AResultCanBeFreedTwiceAndReused) {
  GTEXT_HTML_Result r;
  std::memset(&r, 0, sizeof(r));
  gtext_html_result_free(&r); // zeroed
  gtext_html_result_free(nullptr);
  for (int i = 0; i < 3; i++) {
    GTEXT_Markup_Document * doc = nullptr;
    GTEXT_Markup_Node * frag = nullptr;
    const Call c = GetParam();
    GTEXT_HTML_Status s = GTEXT_HTML_OK;
    if (!c.via_parser) {
      s = c.fragment ? gtext_html_parse_fragment(&div_, "<p>x</b>", 8, nullptr, &doc, &frag, &r)
                     : gtext_html_parse("<p>x</b>", 8, nullptr, &doc, &r);
    }
    else {
      GTEXT_HTML_Parser * p = nullptr;
      ASSERT_EQ(c.fragment ? gtext_html_parser_new_fragment(&div_, nullptr, &p) : gtext_html_parser_new(nullptr, &p),
          GTEXT_HTML_OK);
      ASSERT_EQ(gtext_html_parser_feed(p, "<p>x</b>", 8), GTEXT_HTML_OK);
      s = c.fragment ? gtext_html_parser_finish_fragment(p, &doc, &frag, &r) : gtext_html_parser_finish(p, &doc, &r);
      gtext_html_parser_free(p);
    }
    ASSERT_EQ(s, GTEXT_HTML_OK);
    EXPECT_GT(gtext_html_result_error_count(&r), 0u);
    gtext_markup_document_free(doc);
    gtext_html_result_free(&r);
    EXPECT_EQ(gtext_html_result_error_count(&r), 0u) << "freeing resets the struct";
    gtext_html_result_free(&r);
  }
}

// ---- cutting the input ---------------------------------------------------------------------------

// Documents that exercise the places a cut could matter: a multi-byte character, a tag, an attribute
// value, a character reference, a comment, a doctype, the mark, a table, foreign content, a template,
// raw text, a line end, an error at each of several positions.
const std::string_view kCutDocuments[] = {
    "<!DOCTYPE html><html><head><title>t\xc3\xa9t\xe2\x82\xac</title></head><body><p class=\"a b\" id=x>caf\xc3\xa9 "
    "&amp; &lt;&copy &notin; &#x1F600; &#128512; &#0; &#xD800;</p></body></html>"sv,
    "\xEF\xBB\xBF<!DOCTYPE html><p>a<b>b</p>c<table><tr><td>x<tr><td>y</table>"sv,
    "<p>a\r\nb\rc\n\nd</p><!-- c -- > --><!----><![CDATA[x]]><?pi x?><!doctype>"sv,
    "<svg viewBox=\"0 0 1 1\"><foreignObject><p>x</p></foreignObject><path d=\"M0\"/></svg><math><mi>x</mi></math>"sv,
    "<template><tr><td>x</td></tr></template><select><option>a<option>b</select><textarea>\n&amp;</textarea>"sv,
    "<script>if (a<b && c>d) { x = '</scr' + 'ipt>'; }</script><style>a{b:c}</style><noscript><p>n</p></noscript>"sv,
    "<a href=1><div><a href=2></div><b><i>x</b>y</i><form><form><input><br/></p></br>"sv,
    "<p>\xff\xfe\xc0\x80\xed\xa0\x80 bad \xf0\x9f\x98 utf-8 \x00 nul \x01\x7f</p>"sv,
    "\xEF\xBB"sv,
    "\xEF"sv,
    ""sv,
    "plain"sv,
};

std::vector<Cut> all_cuts(size_t n) {
  std::vector<Cut> cuts;
  cuts.push_back({Cut::BYTES, 0});
  for (size_t k = 0; k <= n; k++) {
    cuts.push_back({Cut::SPLIT, k});
  }
  for (size_t p = 2; p <= 9; p++) {
    cuts.push_back({Cut::PIECES, p});
  }
  return cuts;
}

void expect_same(const Outcome & a, const Outcome & b, const std::string & what) {
  EXPECT_EQ(a.status, b.status) << what;
  EXPECT_EQ(a.tree, b.tree) << what;
  EXPECT_EQ(a.errors, b.errors) << what;
  EXPECT_EQ(a.dropped, b.dropped) << what;
  EXPECT_EQ(a.mode, b.mode) << what;
  EXPECT_EQ(a.encoding, b.encoding) << what;
  EXPECT_EQ(a.source, b.source) << what;
  EXPECT_EQ(a.input_bytes, b.input_bytes) << what;
  EXPECT_EQ(a.node_count, b.node_count) << what;
}

TEST(HtmlApiChunking, TheAnswerIsTheSameHoweverTheInputIsCut) {
  const GTEXT_HTML_Fragment_Context contexts[] = {
      ctx(GTEXT_HTML_NS_HTML, "div"), ctx(GTEXT_HTML_NS_HTML, "td"), ctx(GTEXT_HTML_NS_HTML, "table"),
      ctx(GTEXT_HTML_NS_HTML, "select"), ctx(GTEXT_HTML_NS_HTML, "template"), ctx(GTEXT_HTML_NS_SVG, "svg"),
      ctx(GTEXT_HTML_NS_MATHML, "mi"), ctx(GTEXT_HTML_NS_HTML, "textarea"), ctx(GTEXT_HTML_NS_HTML, "script"),
  };
  size_t runs = 0;
  for (const std::string_view doc_text : kCutDocuments) {
    const std::string text(doc_text);
    const Outcome whole = run_call({"parse", false, false, {}}, text, nullptr, nullptr);
    ASSERT_TRUE(whole.ok());
    EXPECT_EQ(whole.input_bytes, text.size()) << "the parser saw every byte of the document, a NUL included";
    for (const Cut & cut : all_cuts(text.size())) {
      const Outcome got = run_call({"parser", false, true, cut}, text, nullptr, nullptr);
      expect_same(whole, got, "document " + std::to_string(text.size()) + " bytes, cut " + std::to_string(cut.kind) + "/" +
                                  std::to_string(cut.at));
      runs++;
    }
    for (const auto & context : contexts) {
      const Outcome fwhole = run_call({"fragment", true, false, {}}, text, nullptr, &context);
      ASSERT_TRUE(fwhole.ok());
      for (const Cut & cut : all_cuts(text.size())) {
        const Outcome got = run_call({"parser", true, true, cut}, text, nullptr, &context);
        expect_same(fwhole, got, std::string("fragment in ") + context.local_name + ", " + std::to_string(text.size()) +
                                     " bytes, cut " + std::to_string(cut.kind) + "/" + std::to_string(cut.at));
        runs++;
      }
    }
  }
  EXPECT_GT(runs, 5000u);
}

TEST(HtmlApiChunking, ARandomCutIsTheSameAsWell) {
  uint64_t state = 12345;
  auto next = [&]() {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return state >> 33;
  };
  for (const std::string_view doc_text : kCutDocuments) {
    const std::string text(doc_text);
    const Outcome whole = run_call({"parse", false, false, {}}, text, nullptr, nullptr);
    for (int round = 0; round < 20; round++) {
      GTEXT_HTML_Parser * p = nullptr;
      ASSERT_EQ(gtext_html_parser_new(nullptr, &p), GTEXT_HTML_OK);
      size_t at = 0;
      while (at < text.size()) {
        const size_t take = std::min<size_t>(1 + next() % 7, text.size() - at);
        ASSERT_EQ(gtext_html_parser_feed(p, text.data() + at, take), GTEXT_HTML_OK);
        at += take;
        if (next() % 5 == 0) {
          ASSERT_EQ(gtext_html_parser_feed(p, nullptr, 0), GTEXT_HTML_OK) << "an empty feed is nothing";
        }
      }
      Outcome got;
      GTEXT_HTML_Result r;
      got.status = gtext_html_parser_finish(p, &got.doc, &r);
      gtext_html_parser_free(p);
      harvest(got, r);
      finish_outcome(got);
      expect_same(whole, got, "random cut");
    }
  }
}

// ---- the incremental parser's states -------------------------------------------------------------

TEST(HtmlApiParser, ConstructionAndTheOrderOfCalls) {
  GTEXT_HTML_Parser * p = reinterpret_cast<GTEXT_HTML_Parser *>(0x1);
  EXPECT_EQ(gtext_html_parser_new(nullptr, nullptr), GTEXT_HTML_E_INVALID);
  GTEXT_HTML_Parse_Options bad = gtext_html_parse_options_default();
  bad.transport_encoding_certainty = static_cast<GTEXT_HTML_Encoding_Certainty>(7);
  EXPECT_EQ(gtext_html_parser_new(&bad, &p), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(p, nullptr) << "set to NULL on failure";
  EXPECT_EQ(gtext_html_parser_new_fragment(nullptr, nullptr, &p), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(p, nullptr);
  const GTEXT_HTML_Fragment_Context div = ctx(GTEXT_HTML_NS_HTML, "div");
  EXPECT_EQ(gtext_html_parser_new_fragment(&div, nullptr, nullptr), GTEXT_HTML_E_INVALID);
  gtext_html_parser_free(nullptr);
  EXPECT_EQ(gtext_html_parser_feed(nullptr, "x", 1), GTEXT_HTML_E_INVALID);
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_HTML_Result r;
  EXPECT_EQ(gtext_html_parser_finish(nullptr, &doc, &r), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(r.status, GTEXT_HTML_E_INVALID);
  gtext_html_result_free(&r);

  // Finished once, the document is the first caller's; every later call is a state error.
  ASSERT_EQ(gtext_html_parser_new(nullptr, &p), GTEXT_HTML_OK);
  EXPECT_EQ(gtext_html_parser_feed(p, nullptr, 3), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(gtext_html_parser_feed(p, "<p>x", 4), GTEXT_HTML_OK) << "a refused call does not end the parser";
  ASSERT_EQ(gtext_html_parser_finish(p, &doc, &r), GTEXT_HTML_OK);
  GTEXT_Markup_Document * first = doc;
  gtext_html_result_free(&r);
  EXPECT_EQ(gtext_html_parser_feed(p, "x", 1), GTEXT_HTML_E_STATE);
  EXPECT_EQ(gtext_html_parser_feed(p, nullptr, 0), GTEXT_HTML_E_STATE) << "even an empty feed: the input has ended";
  GTEXT_Markup_Document * second = nullptr;
  EXPECT_EQ(gtext_html_parser_finish(p, &second, &r), GTEXT_HTML_E_STATE);
  EXPECT_EQ(second, nullptr);
  EXPECT_EQ(r.status, GTEXT_HTML_E_STATE);
  gtext_html_result_free(&r);
  GTEXT_Markup_Node * frag = nullptr;
  EXPECT_EQ(gtext_html_parser_finish_fragment(p, &second, &frag, nullptr), GTEXT_HTML_E_STATE);
  gtext_html_parser_free(p);
  EXPECT_NE(gtext_markup_document_root(first), nullptr) << "freeing the parser does not touch the document";
  gtext_markup_document_free(first);

  // The wrong kind of finish is a state error and leaves the parser usable.
  ASSERT_EQ(gtext_html_parser_new_fragment(&div, nullptr, &p), GTEXT_HTML_OK);
  ASSERT_EQ(gtext_html_parser_feed(p, "<b>x", 4), GTEXT_HTML_OK);
  EXPECT_EQ(gtext_html_parser_finish(p, &second, &r), GTEXT_HTML_E_STATE);
  gtext_html_result_free(&r);
  ASSERT_EQ(gtext_html_parser_finish_fragment(p, &doc, &frag, &r), GTEXT_HTML_OK);
  gtext_html_result_free(&r);
  EXPECT_EQ(dump_fragment(frag), "| <b>\n|   \"x\"\n");
  gtext_markup_document_free(doc);
  gtext_html_parser_free(p);
  ASSERT_EQ(gtext_html_parser_new(nullptr, &p), GTEXT_HTML_OK);
  EXPECT_EQ(gtext_html_parser_finish_fragment(p, &doc, &frag, &r), GTEXT_HTML_E_STATE);
  gtext_html_result_free(&r);
  gtext_html_parser_free(p);
}

TEST(HtmlApiParser, AFailureIsStickyAndItsResultIsRepeated) {
  GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
  o.max_text_bytes = 4;
  o.transport_encoding = "utf-8"; // a certain label: the parser need not hold the first 1024 bytes back to decide
  GTEXT_HTML_Parser * p = nullptr;
  ASSERT_EQ(gtext_html_parser_new(&o, &p), GTEXT_HTML_OK);
  EXPECT_EQ(gtext_html_parser_feed(p, "<p>ab", 5), GTEXT_HTML_OK);
  EXPECT_EQ(gtext_html_parser_feed(p, "cdefgh</p>", 10), GTEXT_HTML_E_LIMIT_TEXT);
  EXPECT_EQ(gtext_html_parser_feed(p, "x", 1), GTEXT_HTML_E_LIMIT_TEXT) << "repeats the failure";
  EXPECT_EQ(gtext_html_parser_feed(p, nullptr, 0), GTEXT_HTML_E_LIMIT_TEXT);
  for (int i = 0; i < 2; i++) {
    GTEXT_Markup_Document * doc = nullptr;
    GTEXT_HTML_Result r;
    EXPECT_EQ(gtext_html_parser_finish(p, &doc, &r), GTEXT_HTML_E_LIMIT_TEXT);
    EXPECT_EQ(doc, nullptr);
    EXPECT_EQ(r.status, GTEXT_HTML_E_LIMIT_TEXT);
    EXPECT_GT(gtext_html_result_error_count(&r), 0u) << "the errors raised before the failure, again";
    gtext_html_result_free(&r);
  }
  gtext_html_parser_free(p);

  // An unfinished parser can be freed at any point.
  ASSERT_EQ(gtext_html_parser_new(nullptr, &p), GTEXT_HTML_OK);
  ASSERT_EQ(gtext_html_parser_feed(p, "<div><a href='", 14), GTEXT_HTML_OK);
  gtext_html_parser_free(p);
  ASSERT_EQ(gtext_html_parser_new(nullptr, &p), GTEXT_HTML_OK);
  ASSERT_EQ(gtext_html_parser_feed(p, "\xEF\xBB", 2), GTEXT_HTML_OK) << "holding back the start of a mark";
  gtext_html_parser_free(p);
}

// ---- the fragment API ----------------------------------------------------------------------------

TEST(HtmlApiFragment, TheContextIsChecked) {
  struct Row {
    const char * what;
    GTEXT_HTML_Fragment_Context c;
    bool ok;
  };
  GTEXT_HTML_Context_Attribute good_attr = {"encoding", "text/html"};
  GTEXT_HTML_Context_Attribute no_name = {nullptr, "x"};
  GTEXT_HTML_Context_Attribute no_value = {"a", nullptr};
  auto make = [](GTEXT_HTML_Namespace ns, const char * name) { return ctx(ns, name); };
  std::vector<Row> rows;
  rows.push_back({"a div", make(GTEXT_HTML_NS_HTML, "div"), true});
  rows.push_back({"no name", make(GTEXT_HTML_NS_HTML, nullptr), false});
  rows.push_back({"empty name", make(GTEXT_HTML_NS_HTML, ""), false});
  GTEXT_HTML_Fragment_Context c = make(GTEXT_HTML_NS_HTML, "div");
  c.ns = (GTEXT_HTML_Namespace)3;
  rows.push_back({"namespace out of range", c, false});
  c = make(GTEXT_HTML_NS_HTML, "div");
  c.quirks = (GTEXT_Markup_Mode)3;
  rows.push_back({"mode out of range", c, false});
  c = make(GTEXT_HTML_NS_MATHML, "annotation-xml");
  c.attributes = &good_attr;
  c.attribute_count = 1;
  rows.push_back({"an attribute", c, true});
  c.attributes = nullptr;
  rows.push_back({"attributes NULL with a count", c, false});
  c.attributes = &no_name;
  rows.push_back({"attribute with no name", c, false});
  c.attributes = &no_value;
  rows.push_back({"attribute with no value", c, false});
  c = make(GTEXT_HTML_NS_HTML, "div");
  c.attribute_count = 5; // attributes NULL
  rows.push_back({"count with no array", c, false});
  c = make(GTEXT_HTML_NS_HTML, "div");
  c.attribute_count = 0;
  c.attributes = &no_name; // never read
  rows.push_back({"a count of zero reads nothing", c, true});
  for (const Row & row : rows) {
    GTEXT_Markup_Document * doc = nullptr;
    GTEXT_Markup_Node * frag = nullptr;
    GTEXT_HTML_Result r;
    const GTEXT_HTML_Status s = gtext_html_parse_fragment(&row.c, "x", 1, nullptr, &doc, &frag, &r);
    EXPECT_EQ(s, row.ok ? GTEXT_HTML_OK : GTEXT_HTML_E_INVALID) << row.what;
    EXPECT_EQ(doc != nullptr, row.ok) << row.what;
    gtext_markup_document_free(doc);
    gtext_html_result_free(&r);
    GTEXT_HTML_Parser * p = reinterpret_cast<GTEXT_HTML_Parser *>(0x1);
    const GTEXT_HTML_Status ps = gtext_html_parser_new_fragment(&row.c, nullptr, &p);
    EXPECT_EQ(ps, row.ok ? GTEXT_HTML_OK : GTEXT_HTML_E_INVALID) << row.what;
    EXPECT_EQ(p != nullptr, row.ok) << row.what;
    gtext_html_parser_free(p);
  }
  // NULL context, NULL outputs.
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_Markup_Node * frag = nullptr;
  EXPECT_EQ(gtext_html_parse_fragment(nullptr, "x", 1, nullptr, &doc, &frag, nullptr), GTEXT_HTML_E_INVALID);
  const GTEXT_HTML_Fragment_Context div = make(GTEXT_HTML_NS_HTML, "div");
  EXPECT_EQ(gtext_html_parse_fragment(&div, "x", 1, nullptr, &doc, nullptr, nullptr), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(gtext_html_parse_fragment(&div, "x", 1, nullptr, nullptr, &frag, nullptr), GTEXT_HTML_E_INVALID);
}

TEST(HtmlApiFragment, TheContextChangesTheParse) {
  struct Row {
    GTEXT_HTML_Namespace ns;
    const char * name;
    const char * input;
    const char * tree;
  };
  const Row rows[] = {
      {GTEXT_HTML_NS_HTML, "td", "<p>x", "| <p>\n|   \"x\"\n"},
      {GTEXT_HTML_NS_HTML, "tr", "<td>x", "| <td>\n|   \"x\"\n"},
      {GTEXT_HTML_NS_HTML, "div", "<td>x", "| \"x\"\n"},
      {GTEXT_HTML_NS_HTML, "table", "<td>x", "| <tbody>\n|   <tr>\n|     <td>\n|       \"x\"\n"},
      {GTEXT_HTML_NS_HTML, "select", "<option>a<option>b",
          "| <option>\n|   \"a\"\n| <option>\n|   \"b\"\n"},
      {GTEXT_HTML_NS_HTML, "textarea", "<b>&amp;", "| \"<b>&\"\n"},
      {GTEXT_HTML_NS_HTML, "title", "<b>&amp;", "| \"<b>&\"\n"},
      {GTEXT_HTML_NS_HTML, "script", "<b>&amp;", "| \"<b>&amp;\"\n"},
      {GTEXT_HTML_NS_HTML, "plaintext", "<b>&amp;", "| \"<b>&amp;\"\n"},
      {GTEXT_HTML_NS_SVG, "svg", "<path d=x><title>t", "| <svg path>\n|   d=\"x\"\n|   <svg title>\n|     \"t\"\n"},
      {GTEXT_HTML_NS_MATHML, "mi", "<b>x", "| <b>\n|   \"x\"\n"},
      {GTEXT_HTML_NS_HTML, "template", "<td>x", "| <td>\n|   \"x\"\n"},
  };
  for (const Row & row : rows) {
    const GTEXT_HTML_Fragment_Context c = ctx(row.ns, row.name);
    const Outcome o = run_call({"fragment", true, false, {}}, row.input, nullptr, &c);
    ASSERT_TRUE(o.ok()) << row.name;
    EXPECT_EQ(o.tree, row.tree) << row.name << " " << row.input;
  }
}

TEST(HtmlApiFragment, AttributesFormAndTheMode) {
  // annotation-xml is an HTML integration point only with a matching encoding attribute: an HTML tag
  // inside it is HTML (not foreign) then.
  GTEXT_HTML_Context_Attribute enc = {"encoding", "text/html"};
  GTEXT_HTML_Fragment_Context with = ctx(GTEXT_HTML_NS_MATHML, "annotation-xml");
  with.attributes = &enc;
  with.attribute_count = 1;
  GTEXT_HTML_Fragment_Context without = ctx(GTEXT_HTML_NS_MATHML, "annotation-xml");
  const Outcome a = run_call({"fragment", true, false, {}}, "<section>x", nullptr, &with);
  const Outcome b = run_call({"fragment", true, false, {}}, "<section>x", nullptr, &without);
  ASSERT_TRUE(a.ok() && b.ok());
  EXPECT_EQ(a.tree, "| <section>\n|   \"x\"\n");
  EXPECT_EQ(b.tree, "| <math section>\n|   \"x\"\n");

  // A form ancestor stops a nested form: with one, `<form>` is ignored and the p stands alone; without,
  // the form is made and holds the p.
  GTEXT_HTML_Fragment_Context in_form = ctx(GTEXT_HTML_NS_HTML, "div");
  in_form.form_ancestor = true;
  const GTEXT_HTML_Fragment_Context no_form = ctx(GTEXT_HTML_NS_HTML, "div");
  const Outcome f1 = run_call({"fragment", true, false, {}}, "<form><p>x</form>", nullptr, &in_form);
  const Outcome f2 = run_call({"fragment", true, false, {}}, "<form><p>x</form>", nullptr, &no_form);
  ASSERT_TRUE(f1.ok() && f2.ok());
  EXPECT_EQ(f1.tree, "| <p>\n|   \"x\"\n");
  EXPECT_EQ(f2.tree, "| <form>\n|   <p>\n|     \"x\"\n");

  // The mode of the context's document is the fragment's mode, and matters: a table in quirks mode
  // does not close a p.
  for (GTEXT_Markup_Mode m : {GTEXT_MARKUP_MODE_NO_QUIRKS, GTEXT_MARKUP_MODE_LIMITED_QUIRKS, GTEXT_MARKUP_MODE_QUIRKS}) {
    GTEXT_HTML_Fragment_Context c = ctx(GTEXT_HTML_NS_HTML, "div");
    c.quirks = m;
    const Outcome o = run_call({"fragment", true, false, {}}, "<p><table>", nullptr, &c);
    ASSERT_TRUE(o.ok());
    EXPECT_EQ(o.mode, m) << "the result's mode";
    EXPECT_EQ(gtext_markup_document_mode(o.doc), m) << "and the scratch document's";
    const bool nested = o.tree.find("|   <table>") != std::string::npos;
    EXPECT_EQ(nested, m == GTEXT_MARKUP_MODE_QUIRKS) << o.tree;
  }
}

TEST(HtmlApiFragment, TheFragmentBelongsToItsDocumentAndCanBeCloned) {
  GTEXT_Markup_Document * scratch = nullptr;
  GTEXT_Markup_Node * frag = nullptr;
  const GTEXT_HTML_Fragment_Context td = ctx(GTEXT_HTML_NS_HTML, "td");
  ASSERT_EQ(gtext_html_parse_fragment(&td, "<p>x</p>y<!--c-->", 17, nullptr, &scratch, &frag, nullptr), GTEXT_HTML_OK);
  EXPECT_EQ(gtext_markup_node_kind(frag), GTEXT_MARKUP_KIND_FRAGMENT);
  EXPECT_EQ(gtext_markup_node_document(frag), scratch);
  EXPECT_EQ(gtext_markup_node_parent(frag), nullptr);
  EXPECT_EQ(gtext_markup_node_child_count(frag), 3u);
  const GTEXT_Markup_Node * html = gtext_markup_document_element(scratch);
  ASSERT_NE(html, nullptr);
  EXPECT_EQ(gtext_markup_node_first_child(html), nullptr) << "the scratch document's root is empty";

  // Adopt it into a document of the caller's: clone, insert, free the scratch.
  Outcome target = run_call({"parse", false, false, {}}, "<!DOCTYPE html><div id=t></div>", nullptr, nullptr);
  ASSERT_TRUE(target.ok());
  GTEXT_Markup_Node * div = nullptr;
  const GTEXT_Markup_Node * root = gtext_markup_document_root(target.doc);
  for (GTEXT_Markup_Node * n = gtext_markup_node_next_in_order(root, root, false); n;
       n = gtext_markup_node_next_in_order(root, n, false)) {
    if (gtext_markup_node_kind(n) == GTEXT_MARKUP_KIND_ELEMENT && gtext_markup_element_is(n, GTEXT_MARKUP_NS_HTML, "div")) {
      div = n;
    }
  }
  ASSERT_NE(div, nullptr);
  GTEXT_Markup_Node * copy = nullptr;
  ASSERT_EQ(gtext_markup_clone(target.doc, frag, true, &copy), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(div, copy), GTEXT_MARKUP_OK) << "a fragment inserts its children";
  gtext_markup_document_free(scratch);
  EXPECT_EQ(gtext_markup_node_child_count(div), 3u);
  EXPECT_STREQ(markup_check_invariants(target.doc) ? markup_check_invariants(target.doc) : "ok", "ok");
  EXPECT_EQ(dump_doc(target.doc),
      "| <!DOCTYPE html>\n| <html>\n|   <head>\n|   <body>\n|     <div>\n|       id=\"t\"\n|       <p>\n|         \"x\"\n"
      "|       \"y\"\n|       <!-- c -->\n");
}

TEST(HtmlApiFragment, ItAgreesWithTheBuilderForEveryContextInTheFuzzerTable) {
  // The public layer over the builder passes its answer through unchanged: the same input and
  // context, once through gtext_html_tree_parse_fragment() and once through the API.
  struct C {
    GTEXT_HTML_Namespace ns;
    const char * name;
    bool form;
    GTEXT_Markup_Mode quirks;
  };
  const C cs[] = {
      {GTEXT_HTML_NS_HTML, "div", false, GTEXT_MARKUP_MODE_NO_QUIRKS}, {GTEXT_HTML_NS_HTML, "td", false, GTEXT_MARKUP_MODE_QUIRKS},
      {GTEXT_HTML_NS_HTML, "table", true, GTEXT_MARKUP_MODE_NO_QUIRKS}, {GTEXT_HTML_NS_HTML, "select", false, GTEXT_MARKUP_MODE_NO_QUIRKS},
      {GTEXT_HTML_NS_HTML, "template", false, GTEXT_MARKUP_MODE_LIMITED_QUIRKS}, {GTEXT_HTML_NS_SVG, "foreignObject", false, GTEXT_MARKUP_MODE_NO_QUIRKS},
      {GTEXT_HTML_NS_MATHML, "mtext", false, GTEXT_MARKUP_MODE_NO_QUIRKS}, {GTEXT_HTML_NS_HTML, "noscript", false, GTEXT_MARKUP_MODE_NO_QUIRKS},
  };
  for (const C & c : cs) {
    for (int scripting = 0; scripting < 2; scripting++) {
      for (const std::string_view doc_text : kCutDocuments) {
        const std::string text(doc_text);
        GTEXT_HTML_Fragment_Context pub = ctx(c.ns, c.name);
        pub.form_ancestor = c.form;
        pub.quirks = c.quirks;
        GTEXT_HTML_Parse_Options po = gtext_html_parse_options_default();
        po.scripting = scripting;
        const Outcome api = run_call({"fragment", true, false, {}}, text, &po, &pub);

        GTEXT_HTML_FragmentContext in;
        std::memset(&in, 0, sizeof(in));
        in.ns = c.ns;
        in.local_name = c.name;
        in.local_name_length = std::strlen(c.name);
        in.form_ancestor = c.form;
        in.quirks = c.quirks;
        in.scripting = scripting;
        GTEXT_HTML_TreeOptions to;
        std::memset(&to, 0, sizeof(to));
        to.scripting = scripting;
        GTEXT_Markup_Document * doc = nullptr;
        GTEXT_Markup_Node * frag = nullptr;
        GTEXT_HTML_Tree * tree = nullptr;
        ASSERT_EQ(gtext_html_tree_parse_fragment(&in, text.data(), text.size(), &to, &doc, &frag, &tree),
            GTEXT_HTML_TREE_OK);
        ASSERT_TRUE(api.ok());
        // A leading mark is text to the builder and to the fragment API.
        EXPECT_EQ(api.tree, dump_fragment(frag)) << c.name << " " << scripting;
        EXPECT_EQ(api.errors.size() + api.dropped, gtext_html_tree_error_count(tree) + gtext_html_tree_errors_dropped(tree));
        gtext_markup_document_free(doc);
        gtext_html_tree_free(tree);
      }
    }
  }
}

// ---- the result and the names --------------------------------------------------------------------

TEST(HtmlApiNames, EveryStatusAndErrorHasAnotherName) {
  std::set<std::string> seen;
  for (int s = GTEXT_HTML_OK; s <= GTEXT_HTML_E_OPTIONS; s++) {
    const std::string n = gtext_html_status_string((GTEXT_HTML_Status)s);
    EXPECT_NE(n, "");
    EXPECT_NE(n, "unknown status") << s;
    EXPECT_TRUE(seen.insert(n).second) << "two statuses are called " << n;
  }
  EXPECT_STREQ(gtext_html_status_string((GTEXT_HTML_Status)99), "unknown status");
  EXPECT_STREQ(gtext_html_status_string((GTEXT_HTML_Status)-1), "unknown status");

  seen.clear();
  size_t codes = 0;
  for (int c = -3; c < 200; c++) {
    const std::string n = gtext_html_error_name((GTEXT_HTML_Error_Code)c);
    const bool valid = (c >= 0 && c <= 52) || (c >= 64 && c <= 79);
    if (valid) {
      codes++;
      EXPECT_NE(n, "unknown") << c;
      EXPECT_TRUE(seen.insert(n).second) << "two error codes are called " << n;
      // Lower case words and hyphens, as the standard spells them.
      for (char ch : n) {
        EXPECT_TRUE((ch >= 'a' && ch <= 'z') || ch == '-' || (ch >= '0' && ch <= '9')) << n;
      }
    }
    else {
      EXPECT_EQ(n, "unknown") << c;
    }
  }
  EXPECT_EQ(codes, 69u);
  // The numbers are part of the contract.
  EXPECT_STREQ(gtext_html_error_name(GTEXT_HTML_ERROR_ABRUPT_CLOSING_OF_EMPTY_COMMENT), "abrupt-closing-of-empty-comment");
  EXPECT_EQ((int)GTEXT_HTML_ERROR_ABRUPT_CLOSING_OF_EMPTY_COMMENT, 0);
  EXPECT_EQ((int)GTEXT_HTML_ERROR_INVALID_UTF_8_IN_INPUT_STREAM, 52);
  EXPECT_EQ((int)GTEXT_HTML_ERROR_MISSING_DOCTYPE, 64);
  EXPECT_EQ((int)GTEXT_HTML_ERROR_XMLNS_ATTRIBUTE_MISMATCH, 79);
  EXPECT_STREQ(gtext_html_error_name(GTEXT_HTML_ERROR_UNEXPECTED_NULL_CHARACTER), "unexpected-null-character");
  EXPECT_STREQ(gtext_html_error_name(GTEXT_HTML_ERROR_UNEXPECTED_NULL_CHARACTER_IN_CONTENT),
      "unexpected-null-character-in-content");
  // The public names are the builder's, except for that one.
  for (int c = 0; c < 80; c++) {
    if (c == (int)GTEXT_HTML_ERROR_UNEXPECTED_NULL_CHARACTER_IN_CONTENT) {
      continue;
    }
    EXPECT_STREQ(gtext_html_error_name((GTEXT_HTML_Error_Code)c), gtext_html_tree_error_name(c)) << c;
  }
}

TEST(HtmlApiResult, TheAccessorsRefuseWhatIsNotThere) {
  GTEXT_HTML_Error e;
  EXPECT_EQ(gtext_html_result_error_count(nullptr), 0u);
  EXPECT_EQ(gtext_html_result_errors_dropped(nullptr), 0u);
  EXPECT_FALSE(gtext_html_result_error_at(nullptr, 0, &e));
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_HTML_Result r;
  ASSERT_EQ(gtext_html_parse("</x>", 4, nullptr, &doc, &r), GTEXT_HTML_OK);
  const size_t n = gtext_html_result_error_count(&r);
  ASSERT_GE(n, 2u);
  EXPECT_TRUE(gtext_html_result_error_at(&r, n - 1, &e));
  GTEXT_HTML_Error before = e;
  EXPECT_FALSE(gtext_html_result_error_at(&r, n, &e));
  EXPECT_FALSE(gtext_html_result_error_at(&r, SIZE_MAX, &e));
  EXPECT_FALSE(gtext_html_result_error_at(&r, 0, nullptr));
  EXPECT_EQ(std::memcmp(&before, &e, sizeof(e)), 0) << "written only on success";
  // The positions of this one are hand-counted: no doctype at the start, then the end tag `</x>` at
  // column 1 offset 0 with nothing to close.
  EXPECT_TRUE(gtext_html_result_error_at(&r, 0, &e));
  EXPECT_STREQ(e.name, "missing-doctype");
  EXPECT_EQ(e.line, 1u);
  EXPECT_EQ(e.column, 1u);
  EXPECT_EQ(e.byte_offset, 0u);
  gtext_html_result_free(&r);
  gtext_markup_document_free(doc);
}

TEST(HtmlApiResult, PositionsCountLinesAndUtf16Units) {
  // A line feed starts a line; CR LF is one line end; a character outside the BMP is two columns; the
  // offset counts bytes.
  const std::string input = "\n\r\n\xF0\x9F\x98\x80</x>";
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_HTML_Result r;
  ASSERT_EQ(gtext_html_parse(input.data(), input.size(), nullptr, &doc, &r), GTEXT_HTML_OK);
  bool found = false;
  for (size_t i = 0; i < gtext_html_result_error_count(&r); i++) {
    GTEXT_HTML_Error e;
    ASSERT_TRUE(gtext_html_result_error_at(&r, i, &e));
    if (std::string(e.name) == "unexpected-end-tag") {
      found = true;
      EXPECT_EQ(e.line, 3u);
      EXPECT_EQ(e.column, 3u) << "the emoji is two UTF-16 units";
      EXPECT_EQ(e.byte_offset, 7u);
    }
  }
  EXPECT_TRUE(found);
  gtext_html_result_free(&r);
  gtext_markup_document_free(doc);
}

// ---- threads -------------------------------------------------------------------------------------

TEST(HtmlApiThreads, IndependentParsesShareNothing) {
  // N threads parse different documents, whole and by parts, with the default allocator, and each gets
  // exactly the answer a single-threaded parse gives. (Under make test-tsan this is the race check.)
  std::vector<std::string> inputs;
  for (const std::string_view d : kCutDocuments) {
    inputs.emplace_back(d);
  }
  std::vector<std::string> expected;
  for (const std::string & in : inputs) {
    Outcome o = run_call({"parse", false, false, {}}, in, nullptr, nullptr);
    expected.push_back(o.tree + "#" + std::to_string(o.errors.size()));
  }
  std::vector<std::string> failures(8);
  std::vector<std::thread> threads;
  for (size_t t = 0; t < 8; t++) {
    threads.emplace_back([&, t]() {
      for (int round = 0; round < 40; round++) {
        for (size_t i = 0; i < inputs.size(); i++) {
          const bool by_parts = (round + t) % 2 == 1;
          Outcome o = run_call({"parse", false, by_parts, {Cut::PIECES, 1 + (t % 5)}}, inputs[i], nullptr, nullptr);
          if (o.tree + "#" + std::to_string(o.errors.size()) != expected[i]) {
            failures[t] = "a thread got another answer for document " + std::to_string(i);
          }
          const GTEXT_HTML_Fragment_Context c = ctx(GTEXT_HTML_NS_HTML, "td");
          Outcome f = run_call({"fragment", true, by_parts, {Cut::PIECES, 3}}, inputs[i], nullptr, &c);
          if (!f.ok()) {
            failures[t] = "a fragment failed";
          }
        }
      }
    });
  }
  for (auto & th : threads) {
    th.join();
  }
  for (const std::string & f : failures) {
    EXPECT_EQ(f, "");
  }
}


// ---- the sticky failure, and what a failed result says ---------------------------------------------

TEST(HtmlApiParser, AFailureIsStickyForEveryCallAndEveryKindOfParser) {
  GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
  o.max_text_bytes = 4;
  o.transport_encoding = "utf-8"; // (see AFailureIsStickyAndItsResultIsRepeated)
  const GTEXT_HTML_Fragment_Context td = ctx(GTEXT_HTML_NS_HTML, "td");
  for (int fragment = 0; fragment < 2; fragment++) {
    GTEXT_HTML_Parser * p = nullptr;
    ASSERT_EQ(fragment ? gtext_html_parser_new_fragment(&td, &o, &p) : gtext_html_parser_new(&o, &p), GTEXT_HTML_OK);
    ASSERT_EQ(gtext_html_parser_feed(p, "abcdefgh", 8), GTEXT_HTML_E_LIMIT_TEXT) << "(text is held until it ends, so it fails at the end)";
    ASSERT_EQ(gtext_html_parser_feed(p, "<b>", 3), GTEXT_HTML_E_LIMIT_TEXT);
    // A NULL buffer with a length is the caller's mistake, but the parser is already finished: the failure wins.
    EXPECT_EQ(gtext_html_parser_feed(p, nullptr, 5), GTEXT_HTML_E_LIMIT_TEXT) << fragment;
    // So does the wrong kind of finish, and a NULL output.
    for (int wrong = 0; wrong < 2; wrong++) {
      GTEXT_Markup_Document * doc = nullptr;
      GTEXT_Markup_Node * frag = nullptr;
      GTEXT_HTML_Result r;
      GTEXT_HTML_Status s;
      if (wrong) {
        s = fragment ? gtext_html_parser_finish(p, &doc, &r) : gtext_html_parser_finish_fragment(p, &doc, &frag, &r);
      }
      else {
        s = fragment ? gtext_html_parser_finish_fragment(p, nullptr, &frag, &r) : gtext_html_parser_finish(p, nullptr, &r);
      }
      EXPECT_EQ(s, GTEXT_HTML_E_LIMIT_TEXT) << fragment << " " << wrong;
      EXPECT_EQ(r.status, GTEXT_HTML_E_LIMIT_TEXT);
      EXPECT_EQ(doc, nullptr);
      gtext_html_result_free(&r);
    }
    gtext_html_parser_free(p);
  }
}

TEST(HtmlApiParser, WhatAFailedResultSays) {
  const std::string input = "<!DOCTYPE html><p>abcdefghij</p>";
  const GTEXT_HTML_Fragment_Context div = ctx(GTEXT_HTML_NS_HTML, "div");
  struct Row { const char * what; bool fragment; bool parser; std::string prefix; GTEXT_Markup_Encoding_Source source; size_t bytes; };
  const std::string bom = "\xEF\xBB\xBF";
  const Row rows[] = {
      // A whole input refused for its length: nothing was accepted.
      {"whole", false, false, "", GTEXT_MARKUP_ENCODING_DEFAULT, 0},
      {"whole fragment", true, false, "", GTEXT_MARKUP_ENCODING_DEFAULT, 0},
      {"whole, with a mark", false, false, bom, GTEXT_MARKUP_ENCODING_DEFAULT, 0},
      // The parser takes the pieces that fit: the mark and the first piece.
      {"parser, with a mark", false, true, bom, GTEXT_MARKUP_ENCODING_BOM, 3 + 5},
  };
  for (const Row & row : rows) {
    GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
    o.max_input_bytes = row.parser ? 3 + 8 : 8;
    const std::string text = row.prefix + input;
    GTEXT_Markup_Document * doc = nullptr;
    GTEXT_Markup_Node * frag = nullptr;
    GTEXT_HTML_Result r;
    GTEXT_HTML_Status s;
    if (!row.parser) {
      s = row.fragment ? gtext_html_parse_fragment(&div, text.data(), text.size(), &o, &doc, &frag, &r)
                       : gtext_html_parse(text.data(), text.size(), &o, &doc, &r);
    }
    else {
      GTEXT_HTML_Parser * p = nullptr;
      ASSERT_EQ(gtext_html_parser_new(&o, &p), GTEXT_HTML_OK);
      ASSERT_EQ(gtext_html_parser_feed(p, text.data(), 3 + 5), GTEXT_HTML_OK);
      ASSERT_EQ(gtext_html_parser_feed(p, text.data() + 8, text.size() - 8), GTEXT_HTML_E_LIMIT_INPUT);
      s = gtext_html_parser_finish(p, &doc, &r);
      gtext_html_parser_free(p);
    }
    EXPECT_EQ(s, GTEXT_HTML_E_LIMIT_INPUT) << row.what;
    ASSERT_NE(r.encoding, nullptr) << row.what << ": a parser was made, so an encoding was decided";
    EXPECT_STREQ(r.encoding, "UTF-8") << row.what;
    EXPECT_EQ(r.encoding_source, row.source) << row.what;
    EXPECT_EQ(r.input_bytes, row.bytes) << row.what;
    EXPECT_EQ(r.node_count, 0u);
    gtext_html_result_free(&r);
  }
  // A transport label is the source of a failure too.
  GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
  o.max_input_bytes = 4;
  o.transport_encoding = "utf-8";
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_HTML_Result r;
  EXPECT_EQ(gtext_html_parse("<p>abc</p>", 10, &o, &doc, &r), GTEXT_HTML_E_LIMIT_INPUT);
  EXPECT_EQ(r.encoding_source, GTEXT_MARKUP_ENCODING_CALLER);
  gtext_html_result_free(&r);

  // A failure before there was a parser has no encoding at all.
  o = gtext_html_parse_options_default();
  o.transport_encoding_certainty = static_cast<GTEXT_HTML_Encoding_Certainty>(9);
  EXPECT_EQ(gtext_html_parse("x", 1, &o, &doc, &r), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(r.encoding, nullptr);
  EXPECT_EQ(r.encoding_source, GTEXT_MARKUP_ENCODING_UNKNOWN);
  EXPECT_EQ(r.input_bytes, 0u);
  gtext_html_result_free(&r);
  EXPECT_EQ(gtext_html_parse(nullptr, 1, nullptr, &doc, &r), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(r.encoding, nullptr);
  gtext_html_result_free(&r);
  // A failed fragment parser's result, and a failed file parse's, are the same shape.
  GTEXT_HTML_Fragment_Context bad = ctx(GTEXT_HTML_NS_HTML, "");
  GTEXT_Markup_Node * frag = nullptr;
  EXPECT_EQ(gtext_html_parse_fragment(&bad, "x", 1, nullptr, &doc, &frag, &r), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(r.encoding, nullptr);
  gtext_html_result_free(&r);
}

TEST(HtmlApiResult, AResultIsFreedBeforeItIsReusedAndNothingLeaks) {
  FaultAllocator fa;
  GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
  o.allocator = &fa.alloc;
  GTEXT_HTML_Result r;
  std::memset(&r, 0, sizeof(r));
  for (int i = 0; i < 3; i++) {
    GTEXT_Markup_Document * doc = nullptr;
    ASSERT_EQ(gtext_html_parse("</x></y>", 8, &o, &doc, &r), GTEXT_HTML_OK);
    EXPECT_GT(gtext_html_result_error_count(&r), 0u);
    gtext_markup_document_free(doc);
    gtext_html_result_free(&r); // the contract: free it before the next call
  }
  EXPECT_TRUE(fa.live.empty());
  EXPECT_EQ(fa.bad_frees, 0u);
}

TEST(HtmlApiOptions, TheCertaintyIsCheckedAndKeptForLater) {
  for (int bad : {2, 7, -1}) {
    GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
    o.transport_encoding = "utf-8";
    o.transport_encoding_certainty = (GTEXT_HTML_Encoding_Certainty)bad;
    GTEXT_Markup_Document * doc = nullptr;
    GTEXT_HTML_Result r;
    EXPECT_EQ(gtext_html_parse("x", 1, &o, &doc, &r), GTEXT_HTML_E_INVALID) << bad;
    EXPECT_EQ(doc, nullptr);
    gtext_html_result_free(&r);
    GTEXT_HTML_Parser * p = reinterpret_cast<GTEXT_HTML_Parser *>(0x1);
    EXPECT_EQ(gtext_html_parser_new(&o, &p), GTEXT_HTML_E_INVALID);
    EXPECT_EQ(p, nullptr);
  }
  // Both real values are accepted, with or without a label (without one the field is not read).
  for (GTEXT_HTML_Encoding_Certainty c : {GTEXT_HTML_CERTAIN, GTEXT_HTML_TENTATIVE}) {
    GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
    o.transport_encoding_certainty = c;
    EXPECT_TRUE(run_call({"parse", false, false, {}}, "x", &o, nullptr).ok());
    o.transport_encoding = "utf-8";
    EXPECT_TRUE(run_call({"parse", false, false, {}}, "x", &o, nullptr).ok());
  }
  // An attribute of the context with an empty name is no attribute.
  GTEXT_HTML_Context_Attribute empty = {"", "v"};
  GTEXT_HTML_Fragment_Context c = ctx(GTEXT_HTML_NS_MATHML, "annotation-xml");
  c.attributes = &empty;
  c.attribute_count = 1;
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_Markup_Node * frag = nullptr;
  EXPECT_EQ(gtext_html_parse_fragment(&c, "x", 1, nullptr, &doc, &frag, nullptr), GTEXT_HTML_E_INVALID);
}

TEST(HtmlApiLimits, TheDefaultStackHoldsFiveHundredAndTenDivs) {
  // html and body count: 510 divs fit under the default of 512, the 511th does not.
  for (size_t n : {(size_t)510, (size_t)511}) {
    std::string input;
    for (size_t i = 0; i < n; i++) {
      input += "<div>";
    }
    GTEXT_Markup_Document * doc = nullptr;
    GTEXT_HTML_Result r;
    const GTEXT_HTML_Status s = gtext_html_parse(input.data(), input.size(), nullptr, &doc, &r);
    EXPECT_EQ(s, n == 510 ? GTEXT_HTML_OK : GTEXT_HTML_E_LIMIT_DEPTH) << n;
    gtext_markup_document_free(doc);
    gtext_html_result_free(&r);
  }
}

// ---- the byte order mark's guards in the layers under the API ------------------------------------

TEST(HtmlApiSkipInput, TheTokenizersGuards) {
  GTEXT_HTML_TokenizerOptions o = {};
  o.max_input_bytes = 10;
  GTEXT_HTML_Tokenizer * t = gtext_html_tokenizer_new(&o);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(gtext_html_tokenizer_skip_input(nullptr, 3), GTEXT_HTML_TOKENIZER_E_INVALID);
  EXPECT_EQ(gtext_html_tokenizer_skip_input(t, 3), GTEXT_HTML_TOKENIZER_OK);
  EXPECT_EQ(gtext_html_tokenizer_skip_input(t, 3), GTEXT_HTML_TOKENIZER_E_STATE) << "only before the first byte, and once";
  // The three bytes count against the limit: seven more fit and an eighth does not.
  EXPECT_EQ(gtext_html_tokenizer_feed(t, "1234567", 7), GTEXT_HTML_TOKENIZER_OK);
  gtext_html_tokenizer_free(t);

  t = gtext_html_tokenizer_new(&o);
  EXPECT_EQ(gtext_html_tokenizer_feed(t, "x", 1), GTEXT_HTML_TOKENIZER_OK);
  EXPECT_EQ(gtext_html_tokenizer_skip_input(t, 3), GTEXT_HTML_TOKENIZER_E_STATE) << "after a feed";
  gtext_html_tokenizer_free(t);

  t = gtext_html_tokenizer_new(&o);
  EXPECT_EQ(gtext_html_tokenizer_skip_input(t, 11), GTEXT_HTML_TOKENIZER_E_LIMIT_INPUT);
  EXPECT_EQ(gtext_html_tokenizer_skip_input(t, 1), GTEXT_HTML_TOKENIZER_E_LIMIT_INPUT) << "sticky";
  EXPECT_EQ(gtext_html_tokenizer_feed(t, "x", 1), GTEXT_HTML_TOKENIZER_E_LIMIT_INPUT);
  gtext_html_tokenizer_free(t);
}

TEST(HtmlApiSkipInput, TheBuildersGuardsAndTheOffsets) {
  EXPECT_EQ(gtext_html_tree_skip_input(nullptr, 3), GTEXT_HTML_TREE_E_INVALID);
  GTEXT_HTML_TreeOptions o = {};
  o.max_input_bytes = 10;
  o.max_errors = 100;
  GTEXT_HTML_Tree * t = gtext_html_tree_new(&o);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(gtext_html_tree_skip_input(t, 3), GTEXT_HTML_TREE_OK);
  EXPECT_EQ(gtext_html_tree_skip_input(t, 3), GTEXT_HTML_TREE_E_STATE);
  EXPECT_EQ(gtext_html_tree_feed(t, "</x>", 4), GTEXT_HTML_TREE_OK);
  EXPECT_EQ(gtext_html_tree_finish(t), GTEXT_HTML_TREE_OK);
  // The errors are at offsets that include the three bytes that were not read: the end tag is at 3.
  bool found = false;
  for (size_t i = 0; i < gtext_html_tree_error_count(t); i++) {
    const GTEXT_HTML_TreeError * e = gtext_html_tree_error_at(t, i);
    if (std::string(gtext_html_tree_error_name(e->code)) == "unexpected-end-tag") {
      found = true;
      EXPECT_EQ(e->offset, 3u);
      EXPECT_EQ(e->column, 1u) << "the skipped bytes take no column";
    }
  }
  EXPECT_TRUE(found);
  gtext_html_tree_free(t);

  t = gtext_html_tree_new(&o);
  EXPECT_EQ(gtext_html_tree_feed(t, "x", 1), GTEXT_HTML_TREE_OK);
  EXPECT_EQ(gtext_html_tree_skip_input(t, 3), GTEXT_HTML_TREE_E_STATE);
  gtext_html_tree_free(t);

  t = gtext_html_tree_new(&o);
  EXPECT_EQ(gtext_html_tree_skip_input(t, 11), GTEXT_HTML_TREE_E_LIMIT_INPUT);
  EXPECT_EQ(gtext_html_tree_skip_input(t, 1), GTEXT_HTML_TREE_E_LIMIT_INPUT) << "a failed builder repeats its failure";
  EXPECT_EQ(gtext_html_tree_feed(t, "x", 1), GTEXT_HTML_TREE_E_LIMIT_INPUT);
  gtext_html_tree_free(t);
}

} // namespace
