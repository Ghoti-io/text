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
 * The HTML tree builder (src/html/html_tree*.c), stages 3c and 3d. (Foreign content and the fragment parsing
 * algorithm, stage 3e, are in tests/test-html-tree-foreign.cpp.)
 *
 * Every expectation here is worked out by hand from the standard's tree
 * construction chapter (13.2.6, read 2026-10-11) and written as the tree-
 * construction dump, not taken from what the code returned. The pinned
 * web-platform-tests vectors are the conformance gate (`make
 * conformance-html-tree`); this file is what that gate cannot be: the rows of
 * the specification's matrix by name, hand-checked adoption agency cases, the
 * tag table checked against the HTML vocabulary, "reset the insertion mode"
 * over stacks the stage cannot reach, the quirks tables, every way of cutting
 * the corpus of vector inputs in two and a byte at a time, an allocation failure
 * at every point, the limits, and the work counted on the inputs that make a
 * naive tree builder quadratic (compared at two sizes by ratio, not by clock).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <clocale>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "../src/html/html_tree_internal.h"
#include "../src/html/html_tree_private.h"
#include "../src/markup/markup_internal.h"
#include "../tools/conformance/html_tree_dump.h"
#include "markup_test_support.h"

namespace {

struct Parsed {
  GTEXT_HTML_TreeStatus status = GTEXT_HTML_TREE_OK;
  bool has_tree = false;
  std::string dump;
  std::string invariants; // empty when they hold
  GTEXT_Markup_Mode mode = GTEXT_MARKUP_MODE_NO_QUIRKS;
  std::vector<GTEXT_HTML_TreeError> errors;
  size_t dropped = 0;
  size_t work = 0;
  size_t tokenizer_work = 0;

  std::vector<std::string> error_names() const {
    std::vector<std::string> out;
    for (const auto & e : errors) {
      out.push_back(gtext_html_tree_error_name(e.code));
    }
    return out;
  }
  bool same_as(const Parsed & o) const {
    if (status != o.status || has_tree != o.has_tree || dump != o.dump
        || mode != o.mode || dropped != o.dropped || errors.size() != o.errors.size()) {
      return false;
    }
    for (size_t i = 0; i < errors.size(); i++) {
      if (errors[i].code != o.errors[i].code || errors[i].line != o.errors[i].line
          || errors[i].column != o.errors[i].column || errors[i].offset != o.errors[i].offset) {
        return false;
      }
    }
    return true;
  }
};

// cuts: sizes of the pieces to feed in turn (the rest in one piece), empty for the whole input; one_byte
// feeds a byte at a time instead.
Parsed parse(const std::string & input, const GTEXT_HTML_TreeOptions & opts = {},
    const std::vector<size_t> & cuts = {}, bool one_byte = false) {
  Parsed r;
  GTEXT_HTML_TreeOptions o = opts;
  if (o.max_errors == 0) {
    o.max_errors = 100000;
  }
  GTEXT_HTML_Tree * tree = gtext_html_tree_new(&o);
  if (!tree) {
    r.status = GTEXT_HTML_TREE_E_OOM;
    return r;
  }
  size_t at = 0;
  GTEXT_HTML_TreeStatus s = GTEXT_HTML_TREE_OK;
  auto feed = [&](size_t n) {
    if (s == GTEXT_HTML_TREE_OK) {
      // A heap copy per piece, freed at once: a builder that kept a pointer into it is a use-after-free.
      std::vector<char> piece(input.begin() + at, input.begin() + at + n);
      s = gtext_html_tree_feed(tree, piece.data(), n);
    }
    at += n;
  };
  if (one_byte) {
    while (at < input.size()) {
      feed(1);
    }
  }
  else {
    for (size_t n : cuts) {
      n = std::min(n, input.size() - at);
      feed(n);
    }
    if (at < input.size()) {
      feed(input.size() - at);
    }
  }
  if (s == GTEXT_HTML_TREE_OK) {
    s = gtext_html_tree_finish(tree);
  }
  r.status = s;
  for (size_t i = 0; i < gtext_html_tree_error_count(tree); i++) {
    r.errors.push_back(*gtext_html_tree_error_at(tree, i));
  }
  r.dropped = gtext_html_tree_errors_dropped(tree);
  r.work = gtext_html_tree_work(tree);
  r.tokenizer_work = gtext_html_tree_tokenizer_work(tree);
  GTEXT_Markup_Document * doc = gtext_html_tree_take_document(tree);
  if (s == GTEXT_HTML_TREE_OK) {
    EXPECT_NE(doc, nullptr);
  }
  else {
    EXPECT_EQ(doc, nullptr);
  }
  if (doc) {
    r.has_tree = true;
    const char * bad = markup_check_invariants(doc);
    r.invariants = bad ? bad : "";
    r.mode = gtext_markup_document_mode(doc);
    size_t n = 0;
    char * d = htd_dump(doc, &n);
    r.dump = d ? std::string(d, n) : "(dump failed)";
    std::free(d);
    gtext_markup_document_free(doc);
  }
  gtext_html_tree_free(tree);
  return r;
}

std::string tree(const std::vector<std::string> & lines) {
  std::string out;
  for (const auto & l : lines) {
    out += "| " + l + "\n";
  }
  return out;
}

// What parse() builds for these bytes, with the invariants checked.
std::string dump_of(const std::string & input, bool scripting = false) {
  GTEXT_HTML_TreeOptions o = {};
  o.scripting = scripting;
  const Parsed r = parse(input, o);
  EXPECT_EQ(r.status, GTEXT_HTML_TREE_OK) << input;
  EXPECT_EQ(r.invariants, "") << input;
  return r.dump;
}

std::string repeat(const std::string & s, size_t n) {
  std::string out;
  out.reserve(s.size() * n);
  for (size_t i = 0; i < n; i++) {
    out += s;
  }
  return out;
}

const std::vector<std::string> kEmptyBody = {"<html>", "  <head>", "  <body>"};

} // namespace

namespace {

// An allocator that adds up the sizes asked of realloc: a block that is grown by a copy every time is a
// sum that grows with the square of the text, one that grows geometrically a sum that grows with the text.
struct ReallocBytes {
  GTEXT_Allocator alloc;
  size_t bytes = 0;
  ReallocBytes() {
    alloc.ctx = this;
    alloc.malloc_fn = [](void *, size_t n) { return std::malloc(n ? n : 1); };
    alloc.calloc_fn = [](void *, size_t a, size_t b) { return std::calloc(a ? a : 1, b ? b : 1); };
    alloc.realloc_fn = [](void * ctx, void * p, size_t n) {
      static_cast<ReallocBytes *>(ctx)->bytes += n;
      return std::realloc(p, n ? n : 1);
    };
    alloc.free_fn = [](void *, void * p) { std::free(p); };
  }
};

size_t realloc_bytes_of(const std::string & input) {
  ReallocBytes counter;
  GTEXT_HTML_TreeOptions o = {};
  o.allocator = &counter.alloc;
  o.max_open_elements = 100000;
  o.max_nodes = 100000000;
  const Parsed r = parse(input, o);
  EXPECT_EQ(r.status, GTEXT_HTML_TREE_OK);
  // parse() builds its own tree with the options' allocator, so the counter saw all of it.
  return counter.bytes;
}

} // namespace

// ---- The matrix of the specification --------------------------------------

TEST(HtmlTree, AnEmptyDocumentHasHtmlHeadAndBodyAndIsInQuirksMode) {
  const Parsed r = parse("");
  ASSERT_EQ(r.status, GTEXT_HTML_TREE_OK);
  EXPECT_EQ(r.dump, tree({"<html>", "  <head>", "  <body>"}));
  EXPECT_EQ(r.mode, GTEXT_MARKUP_MODE_QUIRKS);
  EXPECT_EQ(r.invariants, "");
  ASSERT_EQ(r.errors.size(), 1u);
  EXPECT_STREQ(gtext_html_tree_error_name(r.errors[0].code), "missing-doctype");
}

TEST(HtmlTree, ADoctypeOfHtmlMakesNoQuirksAndNoError) {
  const Parsed r = parse("<!DOCTYPE html>");
  EXPECT_EQ(r.dump, tree({"<!DOCTYPE html>", "<html>", "  <head>", "  <body>"}));
  EXPECT_EQ(r.mode, GTEXT_MARKUP_MODE_NO_QUIRKS);
  EXPECT_TRUE(r.errors.empty());
}

TEST(HtmlTree, MisnestedFormattingIsRepairedAsTheAdoptionAgencySays) {
  // `<b><i></b></i>`: </b> pops i and b (no furthest block); the i is still in the list but not
  // open, and </i> finds it there, outside the stack: a parse error, the entry is dropped.
  EXPECT_EQ(dump_of("<b><i></b></i>"), tree({"<html>", "  <head>", "  <body>", "    <b>", "      <i>"}));
  // `<a><a>`: the second a start tag closes the first first.
  EXPECT_EQ(dump_of("<a><a>"), tree({"<html>", "  <head>", "  <body>", "    <a>", "    <a>"}));
  // `<p><b></p>x`: </p> pops b and p; b is still in the list, so the text reopens it.
  EXPECT_EQ(dump_of("<p><b></p>x"),
      tree({"<html>", "  <head>", "  <body>", "    <p>", "      <b>", "    <b>", "      \"x\""}));
  Parsed r = parse("<p><b></p>x");
  EXPECT_FALSE(r.errors.empty());
}

TEST(HtmlTree, ImpliedStructureAndContentAfterTheBodyEnds) {
  // The text before <html> starts the structure; <html> and <body> later only add attributes (none here);
  // text after </body> reopens the body; the text after </html> too.
  EXPECT_EQ(dump_of("x<html>y<body>z</body>w</html>v"),
      tree({"<html>", "  <head>", "  <body>", "    \"xyzwv\""}));
  // A comment after </body> goes into the html element, one after </html> into the document.
  EXPECT_EQ(dump_of("<!DOCTYPE html><body></body><!--a--></html><!--b-->"),
      tree({"<!DOCTYPE html>", "<html>", "  <head>", "  <body>", "  <!-- a -->", "<!-- b -->"}));
  // Attributes of a repeated <html> and <body> are added only if absent.
  EXPECT_EQ(dump_of("<html a=1><body b=2><html a=9 c=3><body b=9 d=4>"),
      tree({"<html>", "  a=\"1\"", "  c=\"3\"", "  <head>", "  <body>", "    b=\"2\"", "    d=\"4\""}));
}

TEST(HtmlTree, AFramesetReplacesTheBodyWhenTheDocumentHasNoContentYet) {
  EXPECT_EQ(dump_of("<frameset><frame></frameset>"),
      tree({"<html>", "  <head>", "  <frameset>", "    <frame>"}));
  Parsed r = parse("<frameset><frame></frameset>");
  EXPECT_TRUE(r.errors.size() == 1u); // only the missing doctype
  // Text in a frameset: white space is kept, the rest is dropped with an error each.
  EXPECT_EQ(dump_of("<frameset> a b "), tree({"<html>", "  <head>", "  <frameset>", "    \"   \""}));
  // After text, a frameset start tag in the body is ignored: frameset-ok is "not ok".
  EXPECT_EQ(dump_of("<body>x<frameset>"), tree({"<html>", "  <head>", "  <body>", "    \"x\""}));
  EXPECT_EQ(dump_of("<b></b><frameset>"), tree({"<html>", "  <head>", "  <frameset>"}));
}

TEST(HtmlTree, RawTextAndEscapableRawTextElementsHoldText) {
  EXPECT_EQ(dump_of("<title>t&lt;<b></title><style>c<d</style><script>if (a<b) x</script>"),
      tree({"<html>", "  <head>", "    <title>", "      \"t<<b>\"", "    <style>", "      \"c<d\"",
          "    <script>", "      \"if (a<b) x\"", "  <body>"}));
  EXPECT_EQ(dump_of("<textarea>\n\n&amp;<b></textarea><xmp><b></xmp><iframe><b></iframe><noembed><b></noembed>"),
      tree({"<html>", "  <head>", "  <body>", "    <textarea>", "      \"\n&<b>\"", "    <xmp>", "      \"<b>\"",
          "    <iframe>", "      \"<b>\"", "    <noembed>", "      \"<b>\""}));
  // The first line feed after <pre> and <listing> is dropped, not the second.
  EXPECT_EQ(dump_of("<pre>\n\nx</pre><listing>\ny</listing>"),
      tree({"<html>", "  <head>", "  <body>", "    <pre>", "      \"\nx\"", "    <listing>", "      \"y\""}));
  // plaintext: everything after it is text.
  EXPECT_EQ(dump_of("<plaintext><b></plaintext>"),
      tree({"<html>", "  <head>", "  <body>", "    <plaintext>", "      \"<b></plaintext>\""}));
}

TEST(HtmlTree, TheScriptingFlagDecidesWhatNoscriptInTheHeadIs) {
  const std::string input = "<noscript><p>x</noscript>";
  // Off: "in head noscript" treats <p> as the end of the noscript; the p is in the body and the
  // </noscript> there matches nothing.
  EXPECT_EQ(dump_of(input, false),
      tree({"<html>", "  <head>", "    <noscript>", "  <body>", "    <p>", "      \"x\""}));
  // On: raw text.
  EXPECT_EQ(dump_of(input, true),
      tree({"<html>", "  <head>", "    <noscript>", "      \"<p>x\"", "  <body>"}));
  // Allowed content in the head's noscript with the flag off.
  EXPECT_EQ(dump_of("<head><noscript><link></noscript>x", false),
      tree({"<html>", "  <head>", "    <noscript>", "      <link>", "  <body>", "    \"x\""}));
}

TEST(HtmlTree, SvgAndMathAreParsedWhereverTheyStartATag) {
  for (const char * input : {"<table><tr><td><svg>", "<template><math>", "<svg>", "<p><math>"}) {
    SCOPED_TRACE(input);
    const Parsed r = parse(input);
    EXPECT_EQ(r.status, GTEXT_HTML_TREE_OK);
    EXPECT_TRUE(r.has_tree);
    EXPECT_EQ(r.invariants, "");
  }
  // select is not one of them: the standard has no "in select" mode, so it is parsed by "in body".
  EXPECT_EQ(dump_of("<select><option>a<option>b</select>c"),
      tree({"<html>", "  <head>", "  <body>", "    <select>", "      <option>", "        \"a\"", "      <option>",
          "        \"b\"", "    \"c\""}));
  // An end tag for a construct that is not open is an ordinary ignored end tag.
  EXPECT_EQ(dump_of("</table></template></svg></math>"), tree(kEmptyBody));
  // The table modes and template are stage 3d, built.
  for (const char * in : {"<table>", "x<div><table><tr>", "<template>", "<head><template>", "<head></head><template>"}) {
    EXPECT_EQ(parse(in).status, GTEXT_HTML_TREE_OK) << in;
  }
}

TEST(HtmlTree, TheOpenElementLimitIsDistinctAndLeavesNoTree) {
  // html and body count: 510 divs fit in 512, the 511th does not.
  EXPECT_EQ(parse(repeat("<div>", 510)).status, GTEXT_HTML_TREE_OK);
  const Parsed over = parse(repeat("<div>", 511));
  EXPECT_EQ(over.status, GTEXT_HTML_TREE_E_LIMIT_DEPTH);
  EXPECT_FALSE(over.has_tree);
  EXPECT_EQ(parse(repeat("<div>", 513)).status, GTEXT_HTML_TREE_E_LIMIT_DEPTH);
  // The option moves it.
  GTEXT_HTML_TreeOptions o = {};
  o.max_open_elements = 10;
  EXPECT_EQ(parse(repeat("<div>", 8), o).status, GTEXT_HTML_TREE_OK);
  EXPECT_EQ(parse(repeat("<div>", 9), o).status, GTEXT_HTML_TREE_E_LIMIT_DEPTH);
  // Reconstructing formatting elements is bounded too.
  o.max_open_elements = 6;
  EXPECT_EQ(parse("<b><i><u><s>x</div>y", o).status, GTEXT_HTML_TREE_OK);
  EXPECT_EQ(parse("<p><b><i><u><s><strong>x</p>y", o).status, GTEXT_HTML_TREE_E_LIMIT_DEPTH);
}

TEST(HtmlTree, TheNodeLimitIsDistinct) {
  GTEXT_HTML_TreeOptions o = {};
  o.max_nodes = 5;
  const Parsed r = parse("<p>a<p>b<p>c", o);
  EXPECT_EQ(r.status, GTEXT_HTML_TREE_E_LIMIT_NODES);
  EXPECT_FALSE(r.has_tree);
  o.max_nodes = 1000;
  EXPECT_EQ(parse("<p>a<p>b<p>c", o).status, GTEXT_HTML_TREE_OK);
}

TEST(HtmlTree, TokenizerLimitsAreTheirOwnStatuses) {
  GTEXT_HTML_TreeOptions o = {};
  o.max_text_bytes = 4;
  EXPECT_EQ(parse("<p>abcdefgh", o).status, GTEXT_HTML_TREE_E_LIMIT_TEXT);
  o = {};
  o.max_name_bytes = 3;
  EXPECT_EQ(parse("<abcdef>", o).status, GTEXT_HTML_TREE_E_LIMIT_NAME);
  o = {};
  o.max_attributes = 2;
  EXPECT_EQ(parse("<p a b c>", o).status, GTEXT_HTML_TREE_E_LIMIT_ATTRIBUTES);
  o = {};
  o.max_input_bytes = 5;
  EXPECT_EQ(parse("<p>abcdefgh", o).status, GTEXT_HTML_TREE_E_LIMIT_INPUT);
  o = {};
  o.max_comment_bytes = 2;
  EXPECT_EQ(parse("<!--abcdef-->", o).status, GTEXT_HTML_TREE_E_LIMIT_COMMENT);
  o = {};
  o.max_value_bytes = 2;
  EXPECT_EQ(parse("<p a=abcdef>", o).status, GTEXT_HTML_TREE_E_LIMIT_VALUE);
  // Sticky: a failed builder says the same on every later call and gives no tree.
  GTEXT_HTML_TreeOptions t = {};
  t.max_text_bytes = 4;
  GTEXT_HTML_Tree * tree = gtext_html_tree_new(&t);
  ASSERT_NE(tree, nullptr);
  EXPECT_EQ(gtext_html_tree_feed(tree, "<p>abcdefgh", 11), GTEXT_HTML_TREE_E_LIMIT_TEXT);
  EXPECT_EQ(gtext_html_tree_feed(tree, "x", 1), GTEXT_HTML_TREE_E_LIMIT_TEXT);
  EXPECT_EQ(gtext_html_tree_finish(tree), GTEXT_HTML_TREE_E_LIMIT_TEXT);
  EXPECT_EQ(gtext_html_tree_take_document(tree), nullptr);
  gtext_html_tree_free(tree);
}

TEST(HtmlTree, TheInterfaceRefusesMisuse) {
  EXPECT_EQ(gtext_html_tree_feed(nullptr, "x", 1), GTEXT_HTML_TREE_E_INVALID);
  EXPECT_EQ(gtext_html_tree_finish(nullptr), GTEXT_HTML_TREE_E_INVALID);
  EXPECT_EQ(gtext_html_tree_take_document(nullptr), nullptr);
  gtext_html_tree_free(nullptr);
  GTEXT_HTML_Tree * tree = gtext_html_tree_new(nullptr);
  ASSERT_NE(tree, nullptr);
  EXPECT_EQ(gtext_html_tree_feed(tree, nullptr, 3), GTEXT_HTML_TREE_E_INVALID);
  EXPECT_EQ(gtext_html_tree_take_document(tree), nullptr); // not finished
  EXPECT_EQ(gtext_html_tree_feed(tree, "<p>", 3), GTEXT_HTML_TREE_OK);
  EXPECT_EQ(gtext_html_tree_feed(tree, nullptr, 0), GTEXT_HTML_TREE_OK);
  EXPECT_EQ(gtext_html_tree_finish(tree), GTEXT_HTML_TREE_OK);
  EXPECT_EQ(gtext_html_tree_finish(tree), GTEXT_HTML_TREE_OK);
  EXPECT_EQ(gtext_html_tree_feed(tree, "x", 1), GTEXT_HTML_TREE_E_STATE);
  GTEXT_Markup_Document * doc = gtext_html_tree_take_document(tree);
  EXPECT_NE(doc, nullptr);
  EXPECT_EQ(gtext_html_tree_take_document(tree), nullptr); // once
  gtext_html_tree_free(tree); // does not free what it gave away
  gtext_markup_document_free(doc);
  GTEXT_Markup_Document * out = nullptr;
  GTEXT_HTML_Tree * kept = reinterpret_cast<GTEXT_HTML_Tree *>(0x1);
  EXPECT_EQ(gtext_html_tree_parse(nullptr, 3, nullptr, &out, &kept), GTEXT_HTML_TREE_E_INVALID);
  EXPECT_EQ(kept, nullptr) << "the builder out-parameter is cleared at entry";
  EXPECT_EQ(out, nullptr);
  // A failed parse leaves *out alone and hands the builder back for its status.
  GTEXT_HTML_TreeOptions tiny = {};
  tiny.max_open_elements = 3;
  out = nullptr;
  EXPECT_EQ(gtext_html_tree_parse("<div><div><div>", 15, &tiny, &out, &kept), GTEXT_HTML_TREE_E_LIMIT_DEPTH);
  EXPECT_EQ(out, nullptr);
  ASSERT_NE(kept, nullptr);
  EXPECT_EQ(gtext_html_tree_status(kept), GTEXT_HTML_TREE_E_LIMIT_DEPTH);
  gtext_html_tree_free(kept);
  EXPECT_EQ(gtext_html_tree_parse("x", 1, nullptr, nullptr, nullptr), GTEXT_HTML_TREE_E_INVALID);
  EXPECT_EQ(gtext_html_tree_parse("<p>x", 4, nullptr, &out, nullptr), GTEXT_HTML_TREE_OK);
  ASSERT_NE(out, nullptr);
  gtext_markup_document_free(out);
}

// ---- Insertion modes, hand-checked ----------------------------------------

TEST(HtmlTree, CommentsProcessingInstructionsAndTextLandWhereTheModesPutThem) {
  EXPECT_EQ(dump_of("<!--a--><!DOCTYPE html><!--b--><html><!--c--><head><!--d--></head><!--e--><body><!--f-->"
                    "</body><!--g--></html><!--h-->"),
      tree({"<!-- a -->", "<!DOCTYPE html>", "<!-- b -->", "<html>", "  <!-- c -->", "  <head>", "    <!-- d -->",
          "  <!-- e -->", "  <body>", "    <!-- f -->", "  <!-- g -->", "<!-- h -->"}));
  EXPECT_EQ(dump_of("<?php echo?><body><?x y?>"),
      tree({"<?php echo?>", "<html>", "  <head>", "  <body>", "    <?x y?>"}));
  // Adjacent text merges across ignored tokens.
  EXPECT_EQ(dump_of("<p>a</foo>b<!DOCTYPE html>c"),
      tree({"<html>", "  <head>", "  <body>", "    <p>", "      \"abc\""}));
  // NUL in the body is dropped.
  EXPECT_EQ(dump_of(std::string("<p>a\0b", 6)), tree({"<html>", "  <head>", "  <body>", "    <p>", "      \"ab\""}));
}

TEST(HtmlTree, TheHeadAndTheBodyAreMadeWhenTheyAreNeeded) {
  EXPECT_EQ(dump_of("<title>t</title>x"),
      tree({"<html>", "  <head>", "    <title>", "      \"t\"", "  <body>", "    \"x\""}));
  EXPECT_EQ(dump_of("<meta><link><base><body>"),
      tree({"<html>", "  <head>", "    <meta>", "    <link>", "    <base>", "  <body>"}));
  EXPECT_EQ(dump_of("<head></head> <body></body>"), tree({"<html>", "  <head>", "  \" \"", "  <body>"}));
  EXPECT_EQ(dump_of("<html> <head> <title>x</title> </head> <body> y </body> </html> "),
      tree({"<html>", "  <head>", "    \" \"", "    <title>", "      \"x\"", "    \" \"", "  \" \"", "  <body>",
          "    \" y   \""}));
  // A script start tag after the head is parsed as if the head were open, then the head closes again.
  EXPECT_EQ(dump_of("<head></head><script>x</script>y"),
      tree({"<html>", "  <head>", "    <script>", "      \"x\"", "  <body>", "    \"y\""}));
}

TEST(HtmlTree, HeadingsListsAndDefinitionsCloseTheirNeighbours) {
  EXPECT_EQ(dump_of("<h1>a<h2>b"), tree({"<html>", "  <head>", "  <body>", "    <h1>", "      \"a\"", "    <h2>", "      \"b\""}));
  EXPECT_EQ(dump_of("<ul><li>a<li>b<ul><li>c</ul>d</ul>"),
      tree({"<html>", "  <head>", "  <body>", "    <ul>", "      <li>", "        \"a\"", "      <li>", "        \"b\"",
          "        <ul>", "          <li>", "            \"c\"", "        \"d\""}));
  EXPECT_EQ(dump_of("<dl><dt>a<dd>b<dt>c"),
      tree({"<html>", "  <head>", "  <body>", "    <dl>", "      <dt>", "        \"a\"", "      <dd>", "        \"b\"",
          "      <dt>", "        \"c\""}));
  // A p start tag closes a p in button scope; a button stops the search.
  EXPECT_EQ(dump_of("<p>a<p>b"), tree({"<html>", "  <head>", "  <body>", "    <p>", "      \"a\"", "    <p>", "      \"b\""}));
  EXPECT_EQ(dump_of("<p><button><p>b"),
      tree({"<html>", "  <head>", "  <body>", "    <p>", "      <button>", "        <p>", "          \"b\""}));
  // </p> with no p makes one.
  EXPECT_EQ(dump_of("<body></p>"), tree({"<html>", "  <head>", "  <body>", "    <p>"}));
  EXPECT_EQ(dump_of("<body></br>"), tree({"<html>", "  <head>", "  <body>", "    <br>"}));
  // Before the body exists the same end tags are ignored.
  EXPECT_EQ(dump_of("</p>"), tree(kEmptyBody));
  EXPECT_EQ(dump_of("<image src=a>"), tree({"<html>", "  <head>", "  <body>", "    <img>", "      src=\"a\""}));
}

TEST(HtmlTree, SelectOptionAndHrFollowTheRelaxedParsing) {
  EXPECT_EQ(dump_of("<select><option>a<optgroup><option>b</select>"),
      tree({"<html>", "  <head>", "  <body>", "    <select>", "      <option>", "        \"a\"", "      <optgroup>",
          "        <option>", "          \"b\""}));
  EXPECT_EQ(dump_of("<select><select>x"),
      tree({"<html>", "  <head>", "  <body>", "    <select>", "    \"x\""}));
  EXPECT_EQ(dump_of("<select><input>x"),
      tree({"<html>", "  <head>", "  <body>", "    <select>", "    <input>", "    \"x\""}));
}

TEST(HtmlTree, RubyAndTheFormPointer) {
  EXPECT_EQ(dump_of("<ruby>a<rb>b<rt>c<rp>d"),
      tree({"<html>", "  <head>", "  <body>", "    <ruby>", "      \"a\"", "      <rb>", "        \"b\"", "      <rt>",
          "        \"c\"", "      <rp>", "        \"d\""}));
  // A form start tag while the form pointer is set is ignored; the pointer clears at </form>.
  // </form> removes the form from the stack of open elements, not the div above it: y follows x.
  EXPECT_EQ(dump_of("<form><div><form>x</form></form>y"),
      tree({"<html>", "  <head>", "  <body>", "    <form>", "      <div>", "        \"xy\""}));
}

TEST(HtmlTree, MarkersKeepFormattingFromLeakingOutOfAMarquee) {
  EXPECT_EQ(dump_of("<b><marquee><i>x</marquee>y"),
      tree({"<html>", "  <head>", "  <body>", "    <b>", "      <marquee>", "        <i>", "          \"x\"", "      \"y\""}));
  EXPECT_EQ(dump_of("<b>1<p>2</b>3</p>"),
      tree({"<html>", "  <head>", "  <body>", "    <b>", "      \"1\"", "    <p>", "      <b>", "        \"2\"", "      \"3\""}));
}

TEST(HtmlTree, TheNoahsArkClauseKeepsThreeIdenticalFormattingElements) {
  // Four identical <i>s: the earliest leaves the list, so after the paragraph closes and text comes,
  // only three are reopened.
  EXPECT_EQ(dump_of("<p><i><i><i><i></p>x"),
      tree({"<html>", "  <head>", "  <body>", "    <p>", "      <i>", "        <i>", "          <i>", "            <i>",
          "    <i>", "      <i>", "        <i>", "          \"x\""}));
  // A different attribute is a different element: all four stay.
  EXPECT_EQ(dump_of("<p><i a><i a><i a><i b></p>x"),
      tree({"<html>", "  <head>", "  <body>", "    <p>", "      <i>", "        a=\"\"", "        <i>", "          a=\"\"",
          "          <i>", "            a=\"\"", "            <i>", "              b=\"\"", "    <i>", "      a=\"\"",
          "      <i>", "        a=\"\"", "        <i>", "          a=\"\"", "          <i>", "            b=\"\"",
          "            \"x\""}));
  // Attribute order does not matter; a marker starts a new count.
  EXPECT_EQ(dump_of("<p><i a b><i b a><i a b><i b a></p>x"),
      tree({"<html>", "  <head>", "  <body>", "    <p>", "      <i>", "        a=\"\"", "        b=\"\"", "        <i>",
          "          a=\"\"", "          b=\"\"", "          <i>", "            a=\"\"", "            b=\"\"", "            <i>",
          "              a=\"\"", "              b=\"\"", "    <i>", "      a=\"\"", "      b=\"\"", "      <i>",
          "        a=\"\"", "        b=\"\"", "        <i>", "          a=\"\"", "          b=\"\"", "          \"x\""}));
}

TEST(HtmlTree, AdoptionAgencyCasesWorkedByHand) {
  // <a><b></a></b>: </a> has no furthest block: pops b and a. </b> then finds the b in the list but not
  // open: an error and no tree change.
  EXPECT_EQ(dump_of("<a><b></a></b>"), tree({"<html>", "  <head>", "  <body>", "    <a>", "      <b>"}));
  // <a>1<div>2</a>3: div is the furthest block; a is replaced by a copy inside the div that holds the div's
  // children; the second iteration closes it. 3 is in the b-less div.
  EXPECT_EQ(dump_of("<a>1<div>2</a>3"),
      tree({"<html>", "  <head>", "  <body>", "    <a>", "      \"1\"", "    <div>", "      <a>", "        \"2\"", "      \"3\""}));
  // A formatting element between the formatting element and the furthest block is cloned and kept.
  EXPECT_EQ(dump_of("<a><b><div>x</a>y"),
      tree({"<html>", "  <head>", "  <body>", "    <a>", "      <b>", "    <b>", "      <div>", "        <a>",
          "          \"x\"", "        \"y\""}));
  // The outer loop runs 8 times and stops: nine nested blocks leave the ninth copy open.
  const std::string nine = "<a>" + repeat("<div>", 9) + "x</a>y";
  const Parsed r = parse(nine);
  ASSERT_EQ(r.status, GTEXT_HTML_TREE_OK);
  EXPECT_EQ(r.invariants, "");
  EXPECT_NE(r.dump.find("xy"), std::string::npos);
  // nobr in nobr runs the algorithm.
  EXPECT_EQ(dump_of("<nobr>a<nobr>b"),
      tree({"<html>", "  <head>", "  <body>", "    <nobr>", "      \"a\"", "    <nobr>", "      \"b\""}));
  // Formatting is reopened after a block ends it.
  EXPECT_EQ(dump_of("<p><b><i>a</p>b"),
      tree({"<html>", "  <head>", "  <body>", "    <p>", "      <b>", "        <i>", "          \"a\"", "    <b>", "      <i>",
          "        \"b\""}));
}

TEST(HtmlTree, AStartTagWithATrailingSolidusIsAnErrorUnlessTheElementIsVoid) {
  const Parsed div = parse("<div/>");
  const Parsed br = parse("<br/>");
  auto count = [](const Parsed & r) {
    size_t n = 0;
    for (const auto & e : r.errors) {
      if (e.code == GTEXT_HTML_ERR_NON_VOID_HTML_ELEMENT_START_TAG_WITH_TRAILING_SOLIDUS) {
        n++;
      }
    }
    return n;
  };
  EXPECT_EQ(count(div), 1u);
  EXPECT_EQ(count(br), 0u);
  EXPECT_EQ(count(parse("<input/><meta/><hr/>")), 0u);
  // A frame in the body is ignored, and an ignored token is not acknowledged.
  EXPECT_EQ(count(parse("<frame/>")), 1u);
  EXPECT_EQ(count(parse("<span/><p/>")), 2u);
}

TEST(HtmlTree, ErrorsAreBothStagesInTheOrderRaisedAndAreCapped) {
  GTEXT_HTML_TreeOptions o = {};
  // `<!DOCTYPE>` tokenizer error missing-doctype-name, then the tree's non-conforming doctype error,
  // then the end tag's.
  const Parsed r = parse("<!DOCTYPE></p>", o);
  const auto names = r.error_names();
  ASSERT_GE(names.size(), 3u);
  EXPECT_EQ(names[0], "missing-doctype-name");
  EXPECT_EQ(names[1], "non-conforming-doctype");
  o.max_errors = 2;
  const Parsed capped = parse(repeat("</x>", 50), o);
  EXPECT_EQ(capped.errors.size(), 2u);
  EXPECT_GT(capped.dropped, 40u);
  EXPECT_EQ(capped.status, GTEXT_HTML_TREE_OK);
  for (const auto & e : r.errors) {
    EXPECT_GE(e.line, 1u);
    EXPECT_GE(e.column, 1u);
  }
  EXPECT_STREQ(gtext_html_tree_error_name(9999), "unknown");
  EXPECT_STREQ(gtext_html_tree_error_name(-1), "unknown");
  for (int c = 0; c < (int)GTEXT_HTML_TREE_ERR_COUNT; c++) {
    EXPECT_STRNE(gtext_html_tree_error_name(GTEXT_HTML_TREE_ERROR_BASE + c), "unknown");
  }
}

// ---- The tag table, the vocabulary, the quirks tables, reset ----------------

TEST(HtmlTreeTables, TheTagTableIsInOrderAndAgreesWithTheVocabulary) {
  const char * xhtml = GTEXT_MARKUP_NS_HTML;
  std::set<std::string> in_table;
  for (size_t i = 0; i < T__COUNT; i++) {
    const std::string name = gtext_html_tag_table[i].name;
    SCOPED_TRACE(name);
    in_table.insert(name);
    if (i > 0) {
      EXPECT_LT(std::string(gtext_html_tag_table[i - 1].name), name) << "the table is searched in byte order";
    }
    const uint32_t f = gtext_html_tag_table[i].flags;
    EXPECT_EQ((f & F_SPECIAL) != 0, gtext_markup_html_is_special(xhtml, name.c_str()));
    EXPECT_EQ((f & F_FMT) != 0, gtext_markup_html_is_formatting(xhtml, name.c_str()));
  }
  // The standard's special category, listed here again from its text, is all in the table (so none is
  // treated as an ordinary element), and the vocabulary agrees about each.
  const char * special[] = {"address", "applet", "area", "article", "aside", "base", "basefont", "bgsound",
      "blockquote", "body", "br", "button", "caption", "center", "col", "colgroup", "dd", "details", "dir", "div",
      "dl", "dt", "embed", "fieldset", "figcaption", "figure", "footer", "form", "frame", "frameset", "h1", "h2",
      "h3", "h4", "h5", "h6", "head", "header", "hgroup", "hr", "html", "iframe", "img", "input", "keygen", "li",
      "link", "listing", "main", "marquee", "menu", "meta", "nav", "noembed", "noframes", "noscript", "object",
      "ol", "p", "param", "plaintext", "pre", "script", "search", "section", "select", "source", "style", "summary",
      "table", "tbody", "td", "template", "textarea", "tfoot", "th", "thead", "title", "tr", "track", "ul", "wbr",
      "xmp"};
  EXPECT_EQ(sizeof(special) / sizeof(special[0]), 83u);
  for (const char * n : special) {
    SCOPED_TRACE(n);
    EXPECT_TRUE(in_table.count(n));
    EXPECT_TRUE(gtext_markup_html_is_special(xhtml, n));
  }
  for (const char * n : {"dialog", "image", "ruby", "rb", "rp", "rt", "rtc", "optgroup", "option", "math", "svg", "a"}) {
    SCOPED_TRACE(n);
    EXPECT_FALSE(gtext_markup_html_is_special(xhtml, n));
  }
  const char * formatting[] = {"a", "b", "big", "code", "em", "font", "i", "nobr", "s", "small", "strike", "strong", "tt", "u"};
  for (const char * n : formatting) {
    EXPECT_TRUE(in_table.count(n));
    EXPECT_TRUE(gtext_markup_html_is_formatting(xhtml, n));
  }
  // The scope boundaries and implied end tags, from the standard's text.
  auto flags = [&](const char * n) {
    for (size_t i = 0; i < T__COUNT; i++) {
      if (std::string(gtext_html_tag_table[i].name) == n) {
        return gtext_html_tag_table[i].flags;
      }
    }
    return (uint32_t)0xFFFFFFFF;
  };
  for (const char * n : {"applet", "caption", "html", "table", "td", "th", "marquee", "object", "select", "template"}) {
    EXPECT_TRUE(flags(n) & F_SC_GEN) << n;
  }
  for (const char * n : {"ol", "ul"}) {
    EXPECT_TRUE(flags(n) & F_SC_LIST) << n;
  }
  EXPECT_TRUE(flags("button") & F_SC_BUTTON);
  for (const char * n : {"html", "table", "template"}) {
    EXPECT_TRUE(flags(n) & F_SC_TABLE) << n;
  }
  for (const char * n : {"dd", "dt", "li", "optgroup", "option", "p", "rb", "rp", "rt", "rtc"}) {
    EXPECT_TRUE(flags(n) & F_IMPLIED) << n;
  }
  // The vocabulary answers for the foreign members of the category, with their namespaces.
  EXPECT_TRUE(gtext_markup_html_is_special(GTEXT_MARKUP_NS_MATHML, "mi"));
  EXPECT_TRUE(gtext_markup_html_is_special(GTEXT_MARKUP_NS_MATHML, "annotation-xml"));
  EXPECT_TRUE(gtext_markup_html_is_special(GTEXT_MARKUP_NS_SVG, "foreignObject"));
  EXPECT_TRUE(gtext_markup_html_is_special(GTEXT_MARKUP_NS_SVG, "title"));
  EXPECT_FALSE(gtext_markup_html_is_special(GTEXT_MARKUP_NS_SVG, "div"));
  EXPECT_FALSE(gtext_markup_html_is_special(nullptr, "div"));
  EXPECT_FALSE(gtext_markup_html_is_special(xhtml, nullptr));
  EXPECT_FALSE(gtext_markup_html_is_formatting(GTEXT_MARKUP_NS_SVG, "b"));
  EXPECT_EQ(gtext_markup_html_category("urn:x", "div"), 0u);
}

TEST(HtmlTreeTables, ResetTheInsertionModeAppropriately) {
  struct Case {
    std::vector<const char *> stack;
    bool head_set;
    const char * mode;
  };
  const Case cases[] = {
      {{"html", "body"}, true, "in body"},
      {{"html", "body", "div"}, true, "in body"},
      {{"html", "body", "table", "tbody", "tr", "td"}, true, "in cell"},
      {{"html", "body", "table", "tbody", "tr", "th"}, true, "in cell"},
      {{"html", "body", "table", "tbody", "tr"}, true, "in row"},
      {{"html", "body", "table", "tbody"}, true, "in table body"},
      {{"html", "body", "table", "thead"}, true, "in table body"},
      {{"html", "body", "table", "tfoot"}, true, "in table body"},
      {{"html", "body", "table", "caption"}, true, "in caption"},
      {{"html", "body", "table", "colgroup"}, true, "in column group"},
      {{"html", "body", "table"}, true, "in table"},
      {{"html", "body", "table", "tbody", "tr", "td", "div"}, true, "in cell"},
      {{"html", "body", "table", "tbody", "tr", "td", "table", "tr"}, true, "in row"},
      {{"html", "head"}, true, "in head"},
      {{"html", "head", "style"}, true, "in head"},
      {{"html", "frameset"}, true, "in frameset"},
      {{"html", "frameset", "frameset"}, true, "in frameset"},
      {{"html"}, false, "before head"},
      {{"html"}, true, "after head"},
      {{"html", "x"}, true, "after head"}, // x matches nothing; html is the first node, with a head pointer
      {{"td"}, true, "in body"},        // a td that is the first node is not "in cell" (fragment case)
      {{"head"}, true, "in body"},
      {{"html", "body", "template"}, true, "in template"},
      {{"html", "body", "template/in table"}, true, "in table"},
      {{"html", "body", "template/in column group"}, true, "in column group"},
      {{"html", "body", "template/in table body"}, true, "in table body"},
      {{"html", "body", "template/in row"}, true, "in row"},
      {{"html", "body", "template/in body"}, true, "in body"},
      {{"html", "body", "template/in table", "template/in row", "div"}, true, "in row"}, // the innermost template
      {{"html", "body", "template/in row", "table"}, true, "in table"},                  // a table nearer than the template
      {{"html", "body", "table", "tbody", "tr", "td", "template/in body"}, true, "in body"},
      {{"html", "body", "template/in table", "td"}, true, "in cell"},                    // td nearer than the template
      {{"html", "template/in table", "head"}, true, "in head"},
  };
  for (const Case & c : cases) {
    std::string label;
    for (const char * n : c.stack) {
      label += std::string(n) + " ";
    }
    SCOPED_TRACE(label);
    const char * got = gtext_html_tree_test_reset_mode(c.stack.data(), c.stack.size(), c.head_set);
    ASSERT_NE(got, nullptr);
    EXPECT_STREQ(got, c.mode);
  }
}

// "Reset the insertion mode appropriately" is found from the topmost open element of each name the loop stops at, not by the
// standard's walk down the stack (which costs the depth for every end tag table: tests/test-quadratic.cpp). The walk, written
// out here from the standard's steps, must give the same mode for every stack of a few names, in every order.
TEST(HtmlTreeTables, ResettingTheModeFromTheNameIndexesIsTheStandardsWalk) {
  const char * pool[] = {"html", "head", "body", "table", "tbody", "thead", "tfoot", "tr", "td", "th", "caption", "colgroup",
      "template", "template/in table", "template/in row", "template/in body", "div", "span", "select", "frameset", "p", "i"};
  const size_t np = sizeof(pool) / sizeof(pool[0]);
  uint64_t state = 12345;
  auto next = [&]() {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (size_t)(state >> 33);
  };
  size_t checked = 0;
  for (size_t round = 0; round < 6000; round++) {
    std::vector<const char *> stack;
    const size_t n = 1 + next() % 9;
    for (size_t i = 0; i < n; i++) {
      stack.push_back(pool[next() % np]);
    }
    const bool head_set = next() % 2;
    // The standard's loop: node is the last node in the stack; "if node is the first node in the stack, set last to true";
    // td and th (if not last), head (if not last), tr, tbody/thead/tfoot, caption, colgroup, table, template (the current
    // template insertion mode), body, frameset, html (before head, or after head once the head element pointer is set); else
    // the node before it; at last: in body.
    std::string want = "in body";
    std::vector<std::string> modes; // template insertion modes, innermost last
    for (const char * name : stack) {
      const std::string nm = name;
      if (nm.rfind("template", 0) == 0) {
        modes.push_back(nm.size() > 9 ? nm.substr(9) : "in template");
      }
    }
    for (size_t i = stack.size(); i > 0; i--) {
      const bool last = i == 1;
      std::string nm = stack[i - 1];
      if (nm.rfind("template", 0) == 0) {
        nm = "template";
      }
      std::string found;
      if ((nm == "td" || nm == "th") && !last) {
        found = "in cell";
      }
      else if (nm == "tr") {
        found = "in row";
      }
      else if (nm == "tbody" || nm == "thead" || nm == "tfoot") {
        found = "in table body";
      }
      else if (nm == "caption") {
        found = "in caption";
      }
      else if (nm == "colgroup") {
        found = "in column group";
      }
      else if (nm == "table") {
        found = "in table";
      }
      else if (nm == "template") {
        found = modes.back();
      }
      else if (nm == "head" && !last) {
        found = "in head";
      }
      else if (nm == "body") {
        found = "in body";
      }
      else if (nm == "frameset") {
        found = "in frameset";
      }
      else if (nm == "html") {
        found = head_set ? "after head" : "before head";
      }
      if (!found.empty()) {
        want = found;
        break;
      }
      if (last) {
        want = "in body";
      }
    }
    std::string label;
    for (const char * nm : stack) {
      label += std::string(nm) + " ";
    }
    SCOPED_TRACE(label + (head_set ? "(head set)" : "(no head)"));
    const char * got = gtext_html_tree_test_reset_mode(stack.data(), stack.size(), head_set);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(std::string(got), want);
    checked++;
  }
  EXPECT_EQ(checked, 6000u);
}

TEST(HtmlTreeTables, TheDoctypeDecidesTheDocumentMode) {
  struct Case {
    const char * name;
    const char * pub;
    const char * sys;
    bool force;
    int mode;
  };
  const int N = GTEXT_MARKUP_MODE_NO_QUIRKS, L = GTEXT_MARKUP_MODE_LIMITED_QUIRKS, Q = GTEXT_MARKUP_MODE_QUIRKS;
  const Case cases[] = {
      {"html", nullptr, nullptr, false, N},
      {"html", nullptr, "about:legacy-compat", false, N},
      {"html", "", "", false, N},
      {nullptr, nullptr, nullptr, false, Q},
      {"HTML", nullptr, nullptr, false, Q}, // the tokenizer lower-cases; an upper-case name is not "html"
      {"html", nullptr, nullptr, true, Q},
      {"xhtml", nullptr, nullptr, false, Q},
      {"html", "-//W3O//DTD W3 HTML Strict 3.0//EN//", nullptr, false, Q},
      {"html", "-//w3o//dtd w3 html strict 3.0//en//", nullptr, false, Q},
      {"html", "-//W3O//DTD W3 HTML Strict 3.0//EN//x", nullptr, false, N}, // exact, not a prefix
      {"html", "-/W3C/DTD HTML 4.0 Transitional/EN", nullptr, false, Q},
      {"html", "HTML", nullptr, false, Q},
      {"html", "html", nullptr, false, Q},
      {"html", "HTML x", nullptr, false, N},
      {"html", "-//IETF//DTD HTML 2.0//EN", nullptr, false, Q},
      {"html", "-//IETF//DTD HTML//EN", nullptr, false, Q},
      {"html", "-//Microsoft//DTD Internet Explorer 3.0 Tables//EN", nullptr, false, Q},
      {"html", "-//W3C//DTD HTML 3.2 Final//EN", nullptr, false, Q},
      {"html", "-//W3C//DTD HTML 3.2//EN", nullptr, false, Q},
      {"html", "-//W3C//DTD HTML 4.0 Frameset//EN", nullptr, false, Q},
      {"html", "-//W3C//DTD HTML 4.0 Transitional//EN", nullptr, false, Q},
      {"html", "-//W3C//DTD HTML 4.0//EN", nullptr, false, N},
      {"html", "-//W3C//DTD HTML 4.01//EN", "http://www.w3.org/TR/html4/strict.dtd", false, N},
      {"html", "-//W3C//DTD HTML 4.01//EN", nullptr, false, N},
      {"html", "-//W3C//DTD HTML 4.01 Transitional//EN", nullptr, false, Q},
      {"html", "-//W3C//DTD HTML 4.01 Transitional//EN", "", false, Q},
      {"html", "-//W3C//DTD HTML 4.01 Transitional//EN", "http://www.w3.org/TR/html4/loose.dtd", false, L},
      {"html", "-//W3C//DTD HTML 4.01 Frameset//EN", nullptr, false, Q},
      {"html", "-//W3C//DTD HTML 4.01 Frameset//EN", "http://www.w3.org/TR/html4/frameset.dtd", false, L},
      {"html", "-//W3C//DTD XHTML 1.0 Transitional//EN", "http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd", false, L},
      {"html", "-//W3C//DTD XHTML 1.0 Frameset//EN", nullptr, false, L},
      {"html", "-//W3C//DTD XHTML 1.0 Strict//EN", "http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd", false, N},
      {"html", "-//W3C//DTD XHTML 1.1//EN", "http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd", false, N},
      {"html", nullptr, "http://www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd", false, Q},
      {"html", nullptr, "HTTP://WWW.IBM.COM/DATA/DTD/V11/IBMXHTML1-TRANSITIONAL.DTD", false, Q},
      {"html", nullptr, "http://www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd2", false, N},
      {"html", "+//Silmaril//dtd html Pro v0r11 19970101//", nullptr, false, Q},
      {"html", "-//Netscape Comm. Corp.//DTD HTML//", nullptr, false, Q},
      {"html", "-//SoftQuad Software//DTD HoTMetaL PRO 6.0::19990601::extensions to HTML 4.0//", nullptr, false, Q},
      {"html", "-//WebTechs//DTD Mozilla HTML//", nullptr, false, Q},
      {"html", "-//W3C//DTD W3 HTML//", nullptr, false, Q},
      {"html", "-//AS//DTD HTML 3.0 asWedit + extensions//", nullptr, false, Q},
      {"html", "-//Sun Microsystems Corp.//DTD HotJava Strict HTML//", nullptr, false, Q},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(std::string(c.name ? c.name : "(no name)") + " | " + (c.pub ? c.pub : "(none)") + " | "
        + (c.sys ? c.sys : "(none)"));
    EXPECT_EQ(gtext_html_tree_test_quirks_mode(c.name, c.pub, c.sys, c.force), c.mode);
  }
  // Through the parser: the mode reaches the document.
  EXPECT_EQ(parse("<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\"><p>").mode,
      GTEXT_MARKUP_MODE_QUIRKS);
  EXPECT_EQ(parse("<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\" \"x\"><p>").mode,
      GTEXT_MARKUP_MODE_LIMITED_QUIRKS);
  EXPECT_EQ(parse("<!DOCTYPE html><p>").mode, GTEXT_MARKUP_MODE_NO_QUIRKS);
  EXPECT_EQ(parse("<!DOCTYPE html").mode, GTEXT_MARKUP_MODE_QUIRKS); // eof in doctype forces quirks
  EXPECT_EQ(parse("<p>").mode, GTEXT_MARKUP_MODE_QUIRKS);
  EXPECT_EQ(parse("<!--c--><!DOCTYPE html>").mode, GTEXT_MARKUP_MODE_NO_QUIRKS);
  // A second DOCTYPE after the first changes nothing.
  EXPECT_EQ(parse("<!DOCTYPE html><!DOCTYPE foo>").mode, GTEXT_MARKUP_MODE_NO_QUIRKS);
}

// ---- Chunking -------------------------------------------------------------

namespace {

// Where the vectors are: GTEXT_WPT_DIR, else next to this file, else the working directory's third_party.
std::string wpt_dir() {
  std::vector<std::string> candidates;
  if (const char * env = std::getenv("GTEXT_WPT_DIR")) {
    candidates.push_back(env);
  }
  const std::string here = __FILE__;
  const size_t slash = here.rfind('/');
  if (slash != std::string::npos) {
    candidates.push_back(here.substr(0, slash) + "/../third_party/html/wpt/html/syntax/parsing/resources");
  }
  candidates.push_back("third_party/html/wpt/html/syntax/parsing/resources");
  for (const std::string & c : candidates) {
    if (DIR * d = opendir(c.c_str())) {
      closedir(d);
      return c;
    }
  }
  return candidates.back();
}

// The input of every initial-document vector (not a fragment vector, not a scripted file).
std::vector<std::string> corpus_inputs() {
  std::vector<std::string> out;
  const std::string dir = wpt_dir();
  DIR * d = opendir(dir.c_str());
  EXPECT_NE(d, nullptr) << dir << " is not fetched: run tools/html/fetch.sh";
  if (!d) {
    return out;
  }
  std::vector<std::string> names;
  while (dirent * e = readdir(d)) {
    const std::string n = e->d_name;
    if (n.size() > 4 && n.substr(n.size() - 4) == ".dat" && n.rfind("scripted_", 0) != 0) {
      names.push_back(n);
    }
  }
  closedir(d);
  std::sort(names.begin(), names.end());
  for (const std::string & name : names) {
    std::ifstream in(dir + "/" + name, std::ios::binary);
    std::string line;
    std::string section;
    std::string data;
    bool first_line = true;
    bool have = false;
    bool fragment = false;
    auto finish = [&]() {
      if (have && !fragment) {
        out.push_back(data);
      }
    };
    while (std::getline(in, line)) {
      if (line == "#data") {
        finish();
        have = true;
        data.clear();
        first_line = true;
        fragment = false;
        section = "data";
      }
      else if (line == "#errors" || line == "#new-errors" || line == "#document" || line == "#document-fragment"
          || line == "#script-on" || line == "#script-off") {
        section = line.substr(1);
        fragment = fragment || section == "document-fragment";
      }
      else if (section == "data") {
        data += (first_line ? "" : "\n") + line;
        first_line = false;
      }
    }
    finish();
  }
  return out;
}

} // namespace

TEST(HtmlTreeChunking, EveryTwoWaySplitAndAByteAtATimeGiveTheWholeInputsAnswer) {
  const std::vector<std::string> inputs = corpus_inputs();
  // The vectors that are initial documents and not scripted: 1,600 and more in the pinned tree.
  constexpr size_t kMinInputs = 1500;
  ASSERT_GT(inputs.size(), kMinInputs);
  size_t cut_runs = 0;
  size_t stopped = 0;
  for (size_t scripting = 0; scripting < 2; scripting++) {
    GTEXT_HTML_TreeOptions o = {};
    o.scripting = scripting != 0;
    for (const std::string & in : inputs) {
      const Parsed whole = parse(in, o);
      if (whole.status != GTEXT_HTML_TREE_OK) {
        stopped++;
      }
      ASSERT_EQ(whole.invariants, "") << in;
      for (size_t k = 0; k <= in.size(); k++) {
        const Parsed split = parse(in, o, {k});
        cut_runs++;
        if (!whole.same_as(split)) {
          FAIL() << "cut at " << k << " changed the answer for " << ::testing::PrintToString(in);
        }
      }
      const Parsed bytes = parse(in, o, {}, true);
      if (!whole.same_as(bytes)) {
        FAIL() << "a byte at a time changed the answer for " << ::testing::PrintToString(in);
      }
    }
  }
  EXPECT_GT(cut_runs, 100000u);
  EXPECT_EQ(stopped, 0u) << "every vector of the corpus is parsed to a tree: foreign content is built";
}

TEST(HtmlTreeChunking, ThreeWayCutsOfTheAwkwardInputsToo) {
  const std::string awkward[] = {
      "<!DOCTYPE html><b><i>1<p>2</b>3</i>4", "<pre>\n\nx</pre><textarea>\n\ny</textarea>",
      "<a><b><div>x</a>y</div>z", "<p>a\r\nb\rc&amp;d&#x1F600;e", "x<!--y-->z<?pi q?>w", "<frameset><frame>a b</frameset>",
      "<table> x <b>y</b><tr><td>z</table>", "<template><tr><td>a</template>b", std::string("<table>\0 \0 x</table>", 20),
      "<table><colgroup> a <col> b <caption>c</table>"};
  for (const std::string & in : awkward) {
    const Parsed whole = parse(in);
    for (size_t a = 0; a <= in.size(); a++) {
      for (size_t b = a; b <= in.size(); b++) {
        const Parsed r = parse(in, {}, {a, b - a});
        ASSERT_TRUE(whole.same_as(r)) << a << "," << b << " " << ::testing::PrintToString(in);
      }
    }
  }
}

// ---- Out of memory --------------------------------------------------------

TEST(HtmlTreeOom, AnAllocationFailureAtEveryPointIsOomWithNoTreeAndNoLeak) {
  const char * inputs[] = {
      "<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01//EN\" \"http://x\"><html a=1><head><title>t</title></head>"
      "<body b=2><b>1<p>2</b>3</p><a href=x>y<a>z<i class=a>q<i class=a>q<i class=a>q<i class=a>q</div>"
      "<!--c--><?pi d?><select><option>a<optgroup>b</select><ul><li>a<li>b</ul><foo><bar></foo>text</body></html>x",
      "<frameset><frame></frameset>",
      "<p><b><i>a</p>b<marquee><u>x</marquee>y<form><div><form></form></form>",
      "<!DOCTYPE html><table><caption>c<b>d</caption><colgroup><col><tr><td>x<td><i>y</table><template><tr><td>z</template>w",
      "<table>x<b>y</b><tr><td></td>text<template><tr><td>u</template></table><b><table><i>q</b>",
  };
  for (const char * in : inputs) {
    for (const std::vector<size_t> & cuts : {std::vector<size_t>{}, std::vector<size_t>{7}}) {
      const Parsed expected = parse(in, {}, cuts);
      ASSERT_EQ(expected.status, GTEXT_HTML_TREE_OK);
      size_t failures = 0;
      for (size_t n = 1;; n++) {
        FaultAllocator fa;
        GTEXT_HTML_TreeOptions o = {};
        o.allocator = &fa.alloc;
        fa.arm(n);
        const Parsed r = parse(in, o, cuts);
        EXPECT_EQ(fa.bad_frees, 0u);
        EXPECT_TRUE(fa.live.empty()) << "leak after failing allocation " << n;
        if (!fa.failed) {
          EXPECT_TRUE(expected.same_as(r)) << "allocation " << n;
          break;
        }
        failures++;
        EXPECT_EQ(r.status, GTEXT_HTML_TREE_E_OOM) << "allocation " << n;
        EXPECT_FALSE(r.has_tree);
        ASSERT_LT(n, 100000u);
      }
      EXPECT_GT(failures, in == inputs[0] ? 100u : 10u);
    }
  }
}

TEST(HtmlTreeOom, NewReturnsNullWhenTheAllocatorFailsAnywhereInSetup) {
  for (size_t n = 1; n < 12; n++) {
    FaultAllocator fa;
    GTEXT_HTML_TreeOptions o = {};
    o.allocator = &fa.alloc;
    fa.arm(n);
    GTEXT_HTML_Tree * tree = gtext_html_tree_new(&o);
    if (!fa.failed) {
      EXPECT_NE(tree, nullptr);
      gtext_html_tree_free(tree);
      EXPECT_TRUE(fa.live.empty());
      break;
    }
    EXPECT_EQ(tree, nullptr) << n;
    EXPECT_TRUE(fa.live.empty()) << n;
    EXPECT_EQ(fa.bad_frees, 0u);
  }
}

// ---- Work: the inputs that make a naive tree builder quadratic -------------

namespace {

struct Work {
  size_t tree;
  size_t tokenizer;
};

// The work of one parse fed whole, and of the same input fed a byte at a time: the tree builder's share is
// the same however the input is cut (the tokens are), so the two must agree; the tokenizer's is reported
// for the whole feed.
Work measure(const std::string & input, size_t max_open = 0, bool tolerate_failure = false) {
  Work out = {0, 0};
  size_t whole_tree = 0;
  for (int byte_at_a_time = 0; byte_at_a_time < 2; byte_at_a_time++) {
    GTEXT_HTML_TreeOptions o = {};
    o.max_open_elements = max_open ? max_open : 10000000;
    o.max_nodes = 100000000;
    GTEXT_HTML_Tree * tree = gtext_html_tree_new(&o);
    EXPECT_NE(tree, nullptr);
    GTEXT_HTML_TreeStatus s = GTEXT_HTML_TREE_OK;
    if (byte_at_a_time) {
      for (size_t i = 0; i < input.size() && s == GTEXT_HTML_TREE_OK; i++) {
        s = gtext_html_tree_feed(tree, input.data() + i, 1);
      }
    }
    else {
      s = gtext_html_tree_feed(tree, input.data(), input.size());
    }
    if (s == GTEXT_HTML_TREE_OK) {
      s = gtext_html_tree_finish(tree);
    }
    if (!tolerate_failure) {
      EXPECT_EQ(s, GTEXT_HTML_TREE_OK);
    }
    const size_t tree_work = gtext_html_tree_work(tree);
    if (byte_at_a_time) {
      EXPECT_EQ(tree_work, whole_tree) << "the tree builder's work depends on how the input was cut";
    }
    else {
      whole_tree = tree_work;
      out = Work{tree_work, gtext_html_tree_tokenizer_work(tree)};
    }
    GTEXT_Markup_Document * doc = gtext_html_tree_take_document(tree);
    if (doc) {
      const char * bad = markup_check_invariants(doc);
      EXPECT_EQ(std::string(bad ? bad : ""), "");
      gtext_markup_document_free(doc);
    }
    gtext_html_tree_free(tree);
  }
  return out;
}

// The tree builder's work for n and for 4n repetitions must grow by about 4 (linear), never about 16.
void expect_linear(const char * label, const std::string & unit, size_t n, const std::string & prefix = "",
    const std::string & suffix = "", const std::string & between = "", double limit = 6.0) {
  SCOPED_TRACE(label);
  auto make = [&](size_t k) { return prefix + repeat(unit + between, k) + suffix; };
  const Work a = measure(make(n));
  const Work b = measure(make(4 * n));
  ASSERT_GT(a.tree, 0u);
  const double ratio = (double)b.tree / (double)a.tree;
  EXPECT_LT(ratio, limit) << "work " << a.tree << " -> " << b.tree;
  EXPECT_LE(b.tree, GTEXT_HTML_TREE_WORK_BOUND(make(4 * n).size())) << "work is bounded by a multiple of the input";
}

} // namespace

TEST(HtmlTreeWork, DeepDivNestingIsLinear) {
  expect_linear("div", "<div>", 2000);
  expect_linear("div then p", "<div>", 2000, "", "<p>x");
  expect_linear("div with text", "<div>x", 2000);
  expect_linear("div and ends", "<div>", 2000, "", repeat("</div>", 4));
}

TEST(HtmlTreeWork, ManyParagraphsClosingEachOtherAfterADeepStackIsLinear) {
  // The p-in-button-scope check walks the whole stack for a naive builder.
  const std::string deep = repeat("<span>", 3000);
  expect_linear("p after spans", "<p>x", 2000, deep);
  expect_linear("p and end p after spans", "<p>x</p>", 2000, deep);
  expect_linear("li after spans", "<li>x", 2000, deep);
  expect_linear("dd dt after spans", "<dd>x<dt>y", 1000, deep);
  expect_linear("stray end tags after spans", "</foo>", 4000, deep);
  expect_linear("stray end tags of an open name", "</span></bar>", 2000, deep);
  expect_linear("headings", "<h1>x", 2000, deep);
}

TEST(HtmlTreeWork, MisnestedFormattingIsLinear) {
  expect_linear("b i then end tags", "<b><i>", 2000, "", repeat("</b>", 4));
  expect_linear("b i with end tags", "<b><i>x</b></i>", 1500);
  expect_linear("b with divs", "<b>x<div>", 1000, "", repeat("</b>y", 2));
  expect_linear("end tags with no formatting element", "</b>", 4000, repeat("<i>", 3000));
  expect_linear("p closing b", "<p><b>x</p>", 1500);
  expect_linear("b distinct attributes", "<b id=\"1\">", 800, "", "x", "", 6.0);
}

TEST(HtmlTreeWork, LongRunsOfAAndIAreLinear) {
  expect_linear("a", "<a>x", 3000);
  expect_linear("a href", "<a href=x>y", 3000);
  expect_linear("i", "<i>x", 3000);
  expect_linear("i reconstructed", "<p><i>x", 2000);
  expect_linear("i in p after ends", "<p><i>x</p>", 2000);
  expect_linear("nobr", "<nobr>x", 3000);
  expect_linear("many formatting kinds", "<b><i><u><s><em>x</p><p>", 600);
}

TEST(HtmlTreeWork, AttributesOnManyElementsAreLinear) {
  std::string attrs;
  for (int i = 0; i < 30; i++) {
    attrs += " a" + std::to_string(i) + "=v";
  }
  expect_linear("many elements with attributes", "<div" + attrs + ">", 1000);
  expect_linear("repeated html tags", "<html" + attrs + ">", 400, "", "", "", 6.0);
  expect_linear("many a elements", "<a" + attrs + ">x", 800);
}

TEST(HtmlTreeWork, TheTokenizersShareIsLinearToo) {
  const Work a = measure(repeat("<div class=x>text</div>", 2000));
  const Work b = measure(repeat("<div class=x>text</div>", 8000));
  EXPECT_LT((double)b.tokenizer / (double)a.tokenizer, 5.0);
}

TEST(HtmlTreeWork, AdversarialStacksFailOnlyByTheLimit) {
  const Work w = measure(repeat("<div>", 5000), 100, true);
  EXPECT_LT(w.tree, 200000u);
}

// ---- A text node grows geometrically -----------------------------------------

TEST(HtmlTree, ManySmallInsertionsIntoOneTextNodeStayLinear) {
  // Text split by tokens that are ignored: each piece is appended to the same node.
  const Work a = measure("<p>" + repeat("a</x>", 2000));
  const Work b = measure("<p>" + repeat("a</x>", 8000));
  EXPECT_LT((double)b.tree / (double)a.tree, 5.0);
  const Parsed r = parse("<p>" + repeat("a</x>", 100));
  EXPECT_EQ(r.dump, tree({"<html>", "  <head>", "  <body>", "    <p>", "      \"" + repeat("a", 100) + "\""}));
}

// ---- Threads, and the markup helpers the builder is made of ----------------

TEST(HtmlTree, ParsersInSeveralThreadsShareNothing) {
  const std::vector<std::string> inputs = {
      "<b>1<p>2</b>3</p>", "<!DOCTYPE html><ul><li>a<li>b</ul>", "<a><b><div>x</a>y</div>z",
      "<frameset><frame></frameset>", "<p><i><i><i><i></p>x", "x<foo><bar></foo>y"};
  std::vector<std::string> expected;
  for (const auto & in : inputs) {
    expected.push_back(parse(in).dump);
  }
  std::vector<std::thread> threads;
  std::vector<int> bad(8, 0);
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([&, t]() {
      for (int round = 0; round < 200; round++) {
        for (size_t i = 0; i < inputs.size(); i++) {
          GTEXT_Markup_Document * doc = nullptr;
          if (gtext_html_tree_parse(inputs[i].data(), inputs[i].size(), nullptr, &doc, nullptr) != GTEXT_HTML_TREE_OK) {
            bad[t]++;
            continue;
          }
          size_t n = 0;
          char * d = htd_dump(doc, &n);
          if (!d || std::string(d, n) != expected[i]) {
            bad[t]++;
          }
          std::free(d);
          gtext_markup_document_free(doc);
        }
      }
    });
  }
  for (auto & th : threads) {
    th.join();
  }
  for (int t = 0; t < 8; t++) {
    EXPECT_EQ(bad[t], 0) << "thread " << t;
  }
}

TEST(HtmlTreeMarkupHelpers, ShallowCloneKeepsNameAttributesAndLocationAndNoChildren) {
  GTEXT_Markup_Options mo = gtext_markup_options_default();
  GTEXT_Markup_Document * doc = nullptr;
  ASSERT_EQ(gtext_markup_document_new(&mo, &doc), GTEXT_MARKUP_OK);
  GTEXT_Markup_Node * el = nullptr;
  ASSERT_EQ(gtext_markup_create_element(doc, GTEXT_MARKUP_NS_HTML, nullptr, "b", &el), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_element_set_attribute(el, nullptr, nullptr, "class", "a b", 3), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_element_set_attribute(el, nullptr, nullptr, "id", "", 0), GTEXT_MARKUP_OK);
  GTEXT_Markup_Location where = {7, 2, 3};
  gtext_markup_node_set_location(el, &where);
  GTEXT_Markup_Node * kid = nullptr;
  ASSERT_EQ(gtext_markup_create_text(doc, "x", 1, &kid), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(el, kid), GTEXT_MARKUP_OK);
  GTEXT_Markup_Node * copy = nullptr;
  ASSERT_EQ(markup_element_clone_shallow(doc, el, &copy), GTEXT_MARKUP_OK);
  EXPECT_TRUE(gtext_markup_element_is(copy, GTEXT_MARKUP_NS_HTML, "b"));
  EXPECT_EQ(gtext_markup_node_first_child(copy), nullptr);
  GTEXT_Markup_Attribute a;
  ASSERT_TRUE(gtext_markup_element_get_attribute(copy, nullptr, "class", &a));
  EXPECT_EQ(std::string(a.value, a.value_length), "a b");
  ASSERT_TRUE(gtext_markup_element_get_attribute(copy, nullptr, "id", &a));
  EXPECT_EQ(a.value_length, 0u);
  GTEXT_Markup_Location got = {};
  ASSERT_TRUE(gtext_markup_node_location(copy, &got));
  EXPECT_EQ(got.line, 2u);
  // Moving every child of one node to another keeps their order and sets their parent.
  GTEXT_Markup_Node * kid2 = nullptr;
  ASSERT_EQ(gtext_markup_create_text(doc, "y", 1, &kid2), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(el, kid2), GTEXT_MARKUP_OK);
  markup_move_children_unchecked(el, copy);
  EXPECT_EQ(gtext_markup_node_first_child(el), nullptr);
  EXPECT_EQ(gtext_markup_node_child_count(copy), 2u);
  EXPECT_EQ(gtext_markup_node_parent(kid), copy);
  EXPECT_EQ(gtext_markup_node_next_sibling(kid), kid2);
  markup_move_children_unchecked(el, copy); // from an empty node: nothing
  EXPECT_EQ(gtext_markup_node_child_count(copy), 2u);
  // Text appended in pieces keeps the data NUL-terminated and grows geometrically.
  size_t cap = 0;
  for (int i = 0; i < 1000; i++) {
    ASSERT_EQ(markup_text_append(doc, kid, &cap, "ab", 2), GTEXT_MARKUP_OK);
  }
  size_t len = 0;
  const char * data = gtext_markup_node_data(kid, &len);
  EXPECT_EQ(len, 2001u);
  EXPECT_EQ(data[len], '\0');
  EXPECT_GE(cap, len + 1);
  EXPECT_LT(cap, 4 * len);
  ASSERT_EQ(gtext_markup_append_child(gtext_markup_document_root(doc), copy), GTEXT_MARKUP_OK);
  EXPECT_EQ(std::string(markup_check_invariants(doc) ? markup_check_invariants(doc) : ""), "");
  gtext_markup_document_free(doc);
}

// ---- HTML parsing never fails for content ------------------------------------

TEST(HtmlTreeContent, NamesAndCharactersTheMarkupTreeWouldRefuseAreStillBuilt) {
  struct Case {
    std::string input;
    std::string must_contain; // a line of the dump
  };
  const std::vector<Case> cases = {
      {"<a$b>", "<a$b>"},
      {"<x:y>", "<x:y>"},
      {"<a-b.c_d>", "<a-b.c_d>"},
      {"<\xC3\xA9>", "<\xC3\xA9>"},
      {"<a\xF0\x9F\x98\x80>", "<a\xF0\x9F\x98\x80>"},
      {"<p a\"b=1 a<b=2 \xC3\xA9=3 a'b=4 a=5>", "a\"b=\"1\""},
      {"<p a<b=2>", "a<b=\"2\""},
      {"<p \xC3\xA9=3>", "\xC3\xA9=\"3\""},
      {"<p>\x01\x02\x7f\xC2\x85</p>", "\"\x01\x02\x7f\xC2\x85\""},
      {"<p>\xEF\xBF\xBE\xEF\xBF\xBF\xEF\xB7\x90\xF4\x8F\xBF\xBF</p>", "\"\xEF\xBF\xBE\xEF\xBF\xBF\xEF\xB7\x90\xF4\x8F\xBF\xBF\""},
      {"<!--\x01\xEF\xBF\xBE-->", "<!-- \x01\xEF\xBF\xBE -->"},
      {"<!DOCTYPE a$b \xC3\xA9 PUBLIC \"\xEF\xBF\xBE\" \"\x01\">", "<!DOCTYPE a$b"},
      {"<!DOCTYPE \xEF\xBF\xBE>", "<!DOCTYPE \xEF\xBF\xBE>"},
      {"<?php data\xEF\xBF\xBE?>", "<?php data\xEF\xBF\xBE?>"},
      {"<?\xC3\xA9 data?>", "<!-- ?\xC3\xA9 data? -->"}, // a target must be ASCII: a bogus comment
      {"<?a$><?b>", "<!-- ?a$ -->"},
      {"<b \xC3\xA9=1><i>x</b>y", "\"y\""},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.input);
    const Parsed r = parse(c.input);
    EXPECT_EQ(r.status, GTEXT_HTML_TREE_OK);
    EXPECT_EQ(r.invariants, "");
    EXPECT_NE(r.dump.find(c.must_contain), std::string::npos) << r.dump;
    // And through the other doors.
    EXPECT_EQ(parse(c.input, {}, {}, true).dump, r.dump);
    GTEXT_HTML_TreeOptions o = {};
    o.scripting = true;
    EXPECT_EQ(parse(c.input, o).status, GTEXT_HTML_TREE_OK);
  }
}

// ---- Parse errors: name, line, column, byte offset ---------------------------

namespace {

struct Err {
  const char * name;
  size_t line, column, offset;
};

std::string errors_text(const Parsed & r) {
  std::string out;
  for (const auto & e : r.errors) {
    out += std::string(gtext_html_tree_error_name(e.code)) + " " + std::to_string(e.line) + ":"
        + std::to_string(e.column) + ":" + std::to_string(e.offset) + "; ";
  }
  return out;
}

std::string want_text(const std::vector<Err> & want) {
  std::string out;
  for (const auto & e : want) {
    out += std::string(e.name) + " " + std::to_string(e.line) + ":" + std::to_string(e.column) + ":"
        + std::to_string(e.offset) + "; ";
  }
  return out;
}

} // namespace

TEST(HtmlTreeErrors, ParseErrorsAreNamedAndPositioned) {
  // A tree-stage error is at the token that caused it: its first character (line and column 1-based,
  // the column in UTF-16 units, the offset in bytes). `P` is 15 bytes, so the first token after it is at
  // column 16, offset 15, and the end of the file is at the length of the input.
  const std::string P = "<!DOCTYPE html>";
  struct Case {
    std::string input;
    std::vector<Err> want;
  };
  const std::vector<Case> cases = {
      {P + "<body><body>", {{"unexpected-start-tag", 1, 22, 21}}},
      {P + "</x>", {{"unexpected-end-tag", 1, 16, 15}}},
      {P + "<div>", {{"open-elements-at-eof", 1, 21, 20}}},
      {P + "<div><span></div>", {{"open-elements-at-end-tag", 1, 27, 26}}},
      {P + "<a><a>", {{"misnested-formatting", 1, 19, 18}, {"open-elements-at-eof", 1, 22, 21}}},
      {P + "<form><form>", {{"nested-form", 1, 22, 21}, {"open-elements-at-eof", 1, 28, 27}}},
      {P + "<h1><h2>", {{"unexpected-element-in-context", 1, 20, 19}, {"open-elements-at-eof", 1, 24, 23}}},
      {P + "<image>", {{"deprecated-tag", 1, 16, 15}}},
      {P + "<frameset>x", {{"unexpected-character", 1, 26, 25}, {"open-elements-at-eof", 1, 27, 26}}},
      {P + std::string("<p>a\0b", 6),
          {{"unexpected-null-character", 1, 20, 19}, {"unexpected-null-character", 1, 19, 18}}},
      // Lines: the second line's first token is at column 1.
      {P + "\n</x>", {{"unexpected-end-tag", 2, 1, 16}}},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.input);
    const Parsed r = parse(c.input);
    ASSERT_EQ(r.status, GTEXT_HTML_TREE_OK);
    EXPECT_EQ(errors_text(r), want_text(c.want));
  }
}

TEST(HtmlTreeErrors, TheFramesetModesCountCharactersNotBytes) {
  // Each character that is not white space is one error, whatever its length in bytes.
  auto count = [](const std::string & input) {
    size_t n = 0;
    for (const auto & e : parse(input).errors) {
      if (e.code == GTEXT_HTML_TREE_ERROR_BASE + GTEXT_HTML_TREE_ERR_UNEXPECTED_CHARACTER) {
        n++;
      }
    }
    return n;
  };
  const std::string P = "<!DOCTYPE html><frameset>";
  EXPECT_EQ(count(P + "abc"), 3u);
  EXPECT_EQ(count(P + "\xC3\xA9"), 1u);                  // é
  EXPECT_EQ(count(P + "\xE2\x82\xAC \xF0\x9F\x98\x80"), 2u); // € and an emoji, a space between
  EXPECT_EQ(count(P + "</frameset>\xC3\xA9\xC3\xA9"), 2u);   // after frameset
  EXPECT_EQ(count(P + "</frameset></html>\xC3\xA9"), 1u);    // after after frameset
}

TEST(HtmlTreeErrors, AnAbsentSystemIdentifierAndAnEmptyOneAreNotTheSame) {
  // The standard as served (2026-10-11): the HTML 4.01 Frameset and Transitional rules for quirks mode say
  // "the system identifier is missing or the empty string"; the rules for limited quirks say "neither missing
  // nor the empty string". An empty one therefore counts as absent for both. What the tokenizer says is kept:
  // absent and empty are two states of the token, and reach the builder as two.
  const std::string pub = "-//W3C//DTD HTML 4.01 Transitional//EN";
  EXPECT_EQ(parse("<!DOCTYPE html PUBLIC \"" + pub + "\">").mode, GTEXT_MARKUP_MODE_QUIRKS);
  EXPECT_EQ(parse("<!DOCTYPE html PUBLIC \"" + pub + "\" \"\">").mode, GTEXT_MARKUP_MODE_QUIRKS);
  EXPECT_EQ(parse("<!DOCTYPE html PUBLIC \"" + pub + "\" \"x\">").mode, GTEXT_MARKUP_MODE_LIMITED_QUIRKS);
  // The distinction is kept in the tree: a missing identifier is NULL, an empty one is "".
  GTEXT_Markup_Document * doc = nullptr;
  std::string a = "<!DOCTYPE html PUBLIC \"p\">", b = "<!DOCTYPE html PUBLIC \"p\" \"\">";
  ASSERT_EQ(gtext_html_tree_parse(a.data(), a.size(), nullptr, &doc, nullptr), GTEXT_HTML_TREE_OK);
  EXPECT_EQ(gtext_markup_doctype_system_id(gtext_markup_document_doctype(doc)), nullptr);
  gtext_markup_document_free(doc);
  ASSERT_EQ(gtext_html_tree_parse(b.data(), b.size(), nullptr, &doc, nullptr), GTEXT_HTML_TREE_OK);
  ASSERT_NE(gtext_markup_doctype_system_id(gtext_markup_document_doctype(doc)), nullptr);
  EXPECT_STREQ(gtext_markup_doctype_system_id(gtext_markup_document_doctype(doc)), "");
  gtext_markup_document_free(doc);
}

// ---- Source locations --------------------------------------------------------

namespace {

struct Where {
  bool has;
  size_t offset, line, column;
};

Where where(const GTEXT_Markup_Node * n) {
  GTEXT_Markup_Location l = {};
  const bool has = gtext_markup_node_location(n, &l);
  return {has, l.offset, l.line, l.column};
}

::testing::AssertionResult is_at(const GTEXT_Markup_Node * n, size_t line, size_t column, size_t offset) {
  const Where w = where(n);
  if (w.has && w.line == line && w.column == column && w.offset == offset) {
    return ::testing::AssertionSuccess();
  }
  return ::testing::AssertionFailure() << "at " << w.line << ":" << w.column << ":" << w.offset << " (has "
                                       << w.has << "), wanted " << line << ":" << column << ":" << offset;
}

} // namespace

TEST(HtmlTreeLocations, EveryNodeKnowsWhereItsTokenStarted) {
  // line 1 `<!DOCTYPE html>` (0..14), line 2 `<p>x` from 16, line 3 `<!--c--><?pi d?>` from 21,
  // line 4 `  <b>t</b>` from 38.
  const std::string in = "<!DOCTYPE html>\n<p>x\n<!--c--><?pi d?>\n  <b>t</b>";
  GTEXT_Markup_Document * doc = nullptr;
  ASSERT_EQ(gtext_html_tree_parse(in.data(), in.size(), nullptr, &doc, nullptr), GTEXT_HTML_TREE_OK);
  const GTEXT_Markup_Node * root = gtext_markup_document_root(doc);
  const GTEXT_Markup_Node * doctype = gtext_markup_node_first_child(root);
  EXPECT_TRUE(is_at(doctype, 1, 1, 0));
  const GTEXT_Markup_Node * html = gtext_markup_node_next_sibling(doctype);
  ASSERT_NE(html, nullptr);
  const GTEXT_Markup_Node * body = gtext_markup_node_last_child(html);
  const GTEXT_Markup_Node * p = gtext_markup_node_first_child(body);
  EXPECT_TRUE(is_at(p, 2, 1, 16));
  const GTEXT_Markup_Node * text = gtext_markup_node_first_child(p);
  EXPECT_TRUE(is_at(text, 2, 4, 19));
  const GTEXT_Markup_Node * comment = gtext_markup_node_next_sibling(text);
  EXPECT_TRUE(is_at(comment, 3, 1, 21));
  const GTEXT_Markup_Node * pi = gtext_markup_node_next_sibling(comment);
  EXPECT_EQ(gtext_markup_node_kind(pi), GTEXT_MARKUP_KIND_PROCESSING_INSTRUCTION);
  EXPECT_TRUE(is_at(pi, 3, 9, 29));
  const GTEXT_Markup_Node * text2 = gtext_markup_node_next_sibling(pi);
  EXPECT_TRUE(is_at(text2, 3, 17, 37));
  const GTEXT_Markup_Node * b = gtext_markup_node_next_sibling(text2);
  EXPECT_TRUE(is_at(b, 4, 3, 40));
  EXPECT_TRUE(is_at(gtext_markup_node_first_child(b), 4, 6, 43));
  // The implied html, head and body take the place of the token that made them (here the p).
  EXPECT_TRUE(is_at(html, 2, 1, 16));
  gtext_markup_document_free(doc);
}

TEST(HtmlTreeLocations, ACopyMadeByReconstructionOrAdoptionKeepsTheOriginalsPlace) {
  // `<p><b></p>x`: the b is at offset 3; the b that holds the x is a copy of it, made when x arrived.
  std::string in = "<p><b></p>x";
  GTEXT_Markup_Document * doc = nullptr;
  ASSERT_EQ(gtext_html_tree_parse(in.data(), in.size(), nullptr, &doc, nullptr), GTEXT_HTML_TREE_OK);
  const GTEXT_Markup_Node * body = gtext_markup_node_last_child(gtext_markup_document_element(doc));
  const GTEXT_Markup_Node * p = gtext_markup_node_first_child(body);
  const GTEXT_Markup_Node * b1 = gtext_markup_node_first_child(p);
  const GTEXT_Markup_Node * b2 = gtext_markup_node_next_sibling(p);
  EXPECT_TRUE(is_at(b1, 1, 4, 3));
  EXPECT_TRUE(is_at(b2, 1, 4, 3));
  EXPECT_TRUE(is_at(gtext_markup_node_first_child(b2), 1, 11, 10));
  gtext_markup_document_free(doc);
  // `<a><div>x</a>`: the adoption agency's copy of the a is at the a's place.
  in = "<a><div>x</a>";
  ASSERT_EQ(gtext_html_tree_parse(in.data(), in.size(), nullptr, &doc, nullptr), GTEXT_HTML_TREE_OK);
  body = gtext_markup_node_last_child(gtext_markup_document_element(doc));
  const GTEXT_Markup_Node * div = gtext_markup_node_next_sibling(gtext_markup_node_first_child(body));
  const GTEXT_Markup_Node * copy = gtext_markup_node_first_child(div);
  EXPECT_TRUE(is_at(div, 1, 4, 3));
  EXPECT_TRUE(is_at(copy, 1, 1, 0));
  gtext_markup_document_free(doc);
}

// ---- The ASCII fold, whatever the locale -------------------------------------

TEST(HtmlTreeFold, CaseFoldingIsAsciiOnlyInAnyLocale) {
  const char * saved = setlocale(LC_ALL, nullptr);
  const std::string keep = saved ? saved : "C";
  bool turkish = false;
  for (const char * name : {"tr_TR.UTF-8", "tr_TR.utf8", "tr_TR", "az_AZ.UTF-8"}) {
    if (setlocale(LC_ALL, name)) {
      turkish = true;
      break;
    }
  }
  // In a Turkish locale a locale-aware fold sends I to a dotless i, which would make "HTML" unequal to "html".
  EXPECT_EQ(gtext_html_tree_test_quirks_mode("html", "-//W3C//DTD HTML 4.01 TRANSITIONAL//EN", nullptr, false),
      GTEXT_MARKUP_MODE_QUIRKS);
  EXPECT_EQ(gtext_html_tree_test_quirks_mode("html", "html", nullptr, false), GTEXT_MARKUP_MODE_QUIRKS);
  EXPECT_EQ(gtext_html_tree_test_quirks_mode("html", "-//IETF//DTD HTML//EN", nullptr, false), GTEXT_MARKUP_MODE_QUIRKS);
  EXPECT_EQ(gtext_html_tree_test_quirks_mode("html", "-//ietf//dtd html//en", nullptr, false), GTEXT_MARKUP_MODE_QUIRKS);
  // type=hidden (in any case) leaves frameset-ok alone, so a frameset can still replace the body.
  EXPECT_EQ(dump_of("<input type=HIDDEN><frameset>"), tree({"<html>", "  <head>", "  <frameset>"}));
  EXPECT_EQ(dump_of("<input type=hIdDeN><frameset>"), tree({"<html>", "  <head>", "  <frameset>"}));
  EXPECT_EQ(dump_of("<input type=text><frameset>"),
      tree({"<html>", "  <head>", "  <body>", "    <input>", "      type=\"text\""}));
  EXPECT_EQ(dump_of("<input><frameset>"), tree({"<html>", "  <head>", "  <body>", "    <input>"}));
  // Non-ASCII look-alikes are not folded: a Kelvin sign is not a K.
  EXPECT_EQ(gtext_html_tree_test_quirks_mode("html", "-//W3C//DTD HTML 3.2 FINAL//EN", nullptr, false), GTEXT_MARKUP_MODE_QUIRKS);
  EXPECT_EQ(gtext_html_tree_test_quirks_mode("html", "-//\xE2\x84\xAA//", nullptr, false), GTEXT_MARKUP_MODE_NO_QUIRKS);
  setlocale(LC_ALL, keep.c_str());
  if (!turkish) {
    GTEST_LOG_(INFO) << "no Turkish locale on this machine: the fold is checked in the C locale only";
  }
}

// ---- In-body rules, by name ---------------------------------------------------

TEST(HtmlTree, ListAndDefinitionLoopsStopAtSpecialElementsExceptAddressDivAndP) {
  // blockquote is special and not address, div or p: the search for an li ends there, so nothing is closed.
  EXPECT_EQ(dump_of("<ul><li><blockquote><li>x"),
      tree({"<html>", "  <head>", "  <body>", "    <ul>", "      <li>", "        <blockquote>", "          <li>",
          "            \"x\""}));
  // div is special but is skipped: the li is found, and everything above it is closed.
  EXPECT_EQ(dump_of("<ul><li><div><li>x"),
      tree({"<html>", "  <head>", "  <body>", "    <ul>", "      <li>", "        <div>", "      <li>", "        \"x\""}));
  EXPECT_EQ(dump_of("<dl><dd><address><dt>"),
      tree({"<html>", "  <head>", "  <body>", "    <dl>", "      <dd>", "        <address>", "      <dt>"}));
  EXPECT_EQ(dump_of("<dl><dt><blockquote><dd>"),
      tree({"<html>", "  <head>", "  <body>", "    <dl>", "      <dt>", "        <blockquote>", "          <dd>"}));
  // An ordinary element does not stop the search either.
  EXPECT_EQ(dump_of("<ul><li><span><li>x"),
      tree({"<html>", "  <head>", "  <body>", "    <ul>", "      <li>", "        <span>", "      <li>", "        \"x\""}));
}

TEST(HtmlTree, VoidElementsHeadingsAndTheirEndTags) {
  EXPECT_EQ(dump_of("<area><embed><wbr><param><source><track>x"),
      tree({"<html>", "  <head>", "  <body>", "    <area>", "    <embed>", "    <wbr>", "    <param>", "    <source>",
          "    <track>", "    \"x\""}));
  // An end tag of another heading closes the open one.
  EXPECT_EQ(dump_of("<h1>a</h3>b"),
      tree({"<html>", "  <head>", "  <body>", "    <h1>", "      \"a\"", "    \"b\""}));
  // A heading start tag closes an open heading first.
  EXPECT_EQ(dump_of("<h2><h3>x</h2>y"),
      tree({"<html>", "  <head>", "  <body>", "    <h2>", "    <h3>", "      \"x\"", "    \"y\""}));
  // With no heading open the end tag is ignored.
  EXPECT_EQ(dump_of("<div></h4>x"), tree({"<html>", "  <head>", "  <body>", "    <div>", "      \"x\""}));
}

TEST(HtmlTree, TheNineBlockAdoptionExactTree) {
  // Eight rounds of the adoption agency's outer loop move the a down eight blocks; the ninth block is
  // left holding the text. (html5lib and parse5 give this tree too.)
  const std::string in = "<a>" + repeat("<div>", 9) + "x</a>y";
  std::vector<std::string> want = {"<html>", "  <head>", "  <body>"};
  size_t depth = 2;
  for (int i = 0; i < 8; i++) {
    want.push_back(std::string(depth * 2, ' ') + "<a>");
    want.push_back(std::string(depth * 2, ' ') + "<div>");
    depth++;
  }
  want.push_back(std::string(depth * 2, ' ') + "<a>");
  want.push_back(std::string((depth + 1) * 2, ' ') + "<div>");
  want.push_back(std::string((depth + 2) * 2, ' ') + "\"xy\"");
  EXPECT_EQ(dump_of(in), tree(want));
}

TEST(HtmlTreeTables, OnlyTheIntendedNamesHaveScopeAndImpliedFlags) {
  // The standard's lists, written again here: the boundaries of "in scope", of list item scope (extras), of
  // button scope (extras), of table scope, and the elements generate-implied-end-tags closes.
  const std::set<std::string> general = {"applet", "caption", "html", "table", "td", "th", "marquee", "object", "select", "template"};
  const std::set<std::string> list_extra = {"ol", "ul"};
  const std::set<std::string> button_extra = {"button"};
  const std::set<std::string> table = {"html", "table", "template"};
  const std::set<std::string> implied = {"dd", "dt", "li", "optgroup", "option", "p", "rb", "rp", "rt", "rtc"};
  for (size_t i = 0; i < T__COUNT; i++) {
    const std::string name = gtext_html_tag_table[i].name;
    const uint32_t f = gtext_html_tag_table[i].flags;
    SCOPED_TRACE(name);
    EXPECT_EQ((f & F_SC_GEN) != 0, general.count(name) != 0);
    EXPECT_EQ((f & F_SC_LIST) != 0, list_extra.count(name) != 0);
    EXPECT_EQ((f & F_SC_BUTTON) != 0, button_extra.count(name) != 0);
    EXPECT_EQ((f & F_SC_TABLE) != 0, table.count(name) != 0);
    EXPECT_EQ((f & F_IMPLIED) != 0, implied.count(name) != 0);
    EXPECT_EQ(f & ~(uint32_t)(F_SPECIAL | F_FMT | F_SC_GEN | F_SC_LIST | F_SC_BUTTON | F_SC_TABLE | F_IMPLIED), 0u);
  }
}

// ---- The table of element names the vocabulary does not know ---------------------

TEST(HtmlTree, ManyDistinctCustomElementNamesBuildTheExpectedTree) {
  std::string in;
  for (int i = 0; i < 250; i++) {
    in += "<c-" + std::to_string(i) + ">t";
  }
  in += "</c-100><c-5>y</c-5></c-0>z";
  std::vector<std::string> want = {"<html>", "  <head>", "  <body>"};
  auto line = [&](size_t depth, const std::string & text) { want.push_back(std::string(depth * 2, ' ') + text); };
  for (int i = 0; i < 250; i++) {
    line(2 + i, "<c-" + std::to_string(i) + ">");
    line(3 + i, "\"t\"");
    if (i == 99) {
      // c-100 and everything under it is closed by </c-100>; c-5 opens after it, as c-99's child.
    }
  }
  // The new c-5 is the last child of c-99, after c-100 (which holds the rest). Its lines come after the
  // whole c-100 subtree and at c-100's depth.
  line(2 + 100, "<c-5>");
  line(3 + 100, "\"y\"");
  // z follows </c-0>: it is in the body, after c-0.
  line(2, "\"z\"");
  EXPECT_EQ(dump_of(in), tree(want));
  // Names repeated after the table has grown, and end tags of names never opened.
  EXPECT_EQ(dump_of("<c-1><c-2></c-3></c-1>x"),
      tree({"<html>", "  <head>", "  <body>", "    <c-1>", "      <c-2>", "    \"x\""}));
  // An allocation failure at every point of the same parse is OOM with nothing leaked, or the same tree.
  const Parsed expected = parse(in);
  for (size_t n = 1;; n++) {
    FaultAllocator fa;
    GTEXT_HTML_TreeOptions o = {};
    o.allocator = &fa.alloc;
    fa.arm(n);
    const Parsed r = parse(in, o);
    EXPECT_EQ(fa.bad_frees, 0u);
    EXPECT_TRUE(fa.live.empty()) << n;
    if (!fa.failed) {
      EXPECT_TRUE(expected.same_as(r));
      break;
    }
    // The atom table's growth is best effort and may absorb a failure; then the tree is the same.
    if (r.status == GTEXT_HTML_TREE_OK) {
      EXPECT_TRUE(expected.same_as(r)) << n;
    }
    else {
      EXPECT_EQ(r.status, GTEXT_HTML_TREE_E_OOM) << n;
    }
    ASSERT_LT(n, 200000u);
  }
}

// ---- Merging attributes into html and body is O(1) each ------------------------------

TEST(HtmlTreeWork, RepeatedHtmlAndBodyStartTagsWithNewNamesAreLinear) {
  auto make = [](size_t k) {
    std::string in = "<body>";
    for (size_t i = 0; i < k; i++) {
      in += "<html h" + std::to_string(i) + "=v><body b" + std::to_string(i) + "=w>";
    }
    return in;
  };
  const Work a = measure(make(2000));
  const Work b = measure(make(8000));
  EXPECT_LT((double)b.tree / (double)a.tree, 5.0) << a.tree << " -> " << b.tree;
  EXPECT_LE(b.tree, GTEXT_HTML_TREE_WORK_BOUND(make(8000).size()));
  // Every distinct name is kept, once, and the first value wins for a repeated name.
  const Parsed r = parse("<html a=1 b=2><html b=3 c=4><body d=5><body d=6 e=7>");
  EXPECT_EQ(r.dump,
      tree({"<html>", "  a=\"1\"", "  b=\"2\"", "  c=\"4\"", "  <head>", "  <body>", "    d=\"5\"", "    e=\"7\""}));
  const Parsed many = parse(make(500));
  size_t attrs = 0;
  for (size_t at = 0; (at = many.dump.find("=\"", at)) != std::string::npos; at += 2) {
    attrs++;
  }
  EXPECT_EQ(attrs, 1000u);
}

// ---- Distinct attributes on formatting elements: the exact bound ---------------------

TEST(HtmlTreeWork, FormattingElementsWithDistinctAttributesCostAtMostTheStackDepthEach) {
  // The Noah's Ark clause compares a new formatting element with the entries of its name after the last
  // marker. Every formatting start tag reconstructs first, so all earlier entries are open: the walk is at
  // most the depth of the stack (512 by default). With the limit raised the cost per token is bounded by the
  // depth the input builds, and a unit of fixed depth repeated is linear in the repetitions.
  const int d = 300;
  auto nested = [&](size_t reps) {
    std::string unit;
    for (int i = 0; i < d; i++) {
      unit += "<b id=" + std::to_string(i) + ">";
    }
    unit += repeat("</b>", d);
    return repeat("<marquee>" + unit + "</marquee>", reps);
  };
  auto misnested = [&](size_t reps) {
    std::string unit;
    for (int i = 0; i < d / 2; i++) {
      unit += "<b id=" + std::to_string(i) + "><i id=" + std::to_string(i) + ">";
    }
    unit += repeat("</b>", d / 2);
    unit += repeat("</i>", d / 2);
    return repeat("<marquee>" + unit + "</marquee>", reps); // the marker forgets what the unit left in the list
  };
  auto reconstructed = [&](size_t reps) {
    std::string unit;
    for (int i = 0; i < d; i++) {
      unit += "<p><b id=" + std::to_string(i) + ">x</p>";
    }
    unit += "<p>";
    return repeat("<marquee>" + unit + "</marquee>", reps);
  };
  struct Variant {
    const char * name;
    std::function<std::string(size_t)> make;
  };
  const Variant variants[] = {{"nested", nested}, {"misnested", misnested}, {"reconstructed", reconstructed}};
  for (const Variant & v : variants) {
    SCOPED_TRACE(v.name);
    const Work a = measure(v.make(2), 10000);
    const Work b = measure(v.make(8), 10000);
    EXPECT_LT((double)b.tree / (double)a.tree, 5.0) << a.tree << " -> " << b.tree;
    // At most (depth + a constant) per token; a token is at least 4 bytes.
    EXPECT_LE(b.tree, (size_t)(4 * (d + 8)) * (v.make(8).size() / 4)) << b.tree;
  }
}

// ---- Tables, foster parenting and templates (stage 3d) ----------------------------

TEST(HtmlTreeTables, TheMatrixRowsOfTheSpecification) {
  // Each expectation is worked out from the standard's text (13.2.6.4.9 to 13.2.6.4.16) before it is written here.
  // implied tbody: table > tbody > tr > td
  EXPECT_EQ(dump_of("<table><tr><td>x"), R"(| <html>
|   <head>
|   <body>
|     <table>
|       <tbody>
|         <tr>
|           <td>
|             "x"
)");
  // foster parenting: the text and the b go before the table
  EXPECT_EQ(dump_of("<table>x<b>y</b></table>"), R"(| <html>
|   <head>
|   <body>
|     "x"
|     <b>
|       "y"
|     <table>
)");
  // a nested table sits in the cell
  EXPECT_EQ(dump_of("<table><td><table><td>"), R"(| <html>
|   <head>
|   <body>
|     <table>
|       <tbody>
|         <tr>
|           <td>
|             <table>
|               <tbody>
|                 <tr>
|                   <td>
)");
  // a td closes the open cell; a tr closes the row
  EXPECT_EQ(dump_of("<table><tr><td>a<td>b<tr><td>c"), R"(| <html>
|   <head>
|   <body>
|     <table>
|       <tbody>
|         <tr>
|           <td>
|             "a"
|           <td>
|             "b"
|         <tr>
|           <td>
|             "c"
)");
  // a td end tag outside a cell is ignored; text after a row is foster parented and joins the text before the table
  EXPECT_EQ(dump_of("</td>x<table><tr><td>a</tr>b"), R"(| <html>
|   <head>
|   <body>
|     "xb"
|     <table>
|       <tbody>
|         <tr>
|           <td>
|             "a"
)");
  // the form is inserted and popped at once; a hidden input is inserted in the table
  EXPECT_EQ(dump_of("<table><form><input type=hidden>"), R"(| <html>
|   <head>
|   <body>
|     <table>
|       <form>
|       <input>
|         type="hidden"
)");
  // the form element pointer is set, so the second form is ignored
  EXPECT_EQ(dump_of("<table><form><form><input type=hidden>"), R"(| <html>
|   <head>
|   <body>
|     <table>
|       <form>
|       <input>
|         type="hidden"
)");
  // a tr in a template is not given a tbody
  EXPECT_EQ(dump_of("<template><tr><td>x</td></tr></template>"), R"(| <html>
|   <head>
|     <template>
|       content
|         <tr>
|           <td>
|             "x"
|   <body>
)");
  // a col in a template is in the content, without a colgroup
  EXPECT_EQ(dump_of("<template><col>"), R"(| <html>
|   <head>
|     <template>
|       content
|         <col>
|   <body>
)");
  // a caption in a template is in the content, without a table
  EXPECT_EQ(dump_of("<template><caption>c"), R"(| <html>
|   <head>
|     <template>
|       content
|         <caption>
|           "c"
|   <body>
)");
  // EOF in a table
  EXPECT_EQ(dump_of("<table>"), R"(| <html>
|   <head>
|   <body>
|     <table>
)");
  // EOF in a template pops it
  EXPECT_EQ(dump_of("<template><tr>"), R"(| <html>
|   <head>
|     <template>
|       content
|         <tr>
|   <body>
)");
}

TEST(HtmlTreeTables, ImpliedSectionsRowsAndCells) {
  EXPECT_EQ(dump_of("<table><td>x"), R"(| <html>
|   <head>
|   <body>
|     <table>
|       <tbody>
|         <tr>
|           <td>
|             "x"
)");
  EXPECT_EQ(dump_of("<table><col>"), R"(| <html>
|   <head>
|   <body>
|     <table>
|       <colgroup>
|         <col>
)");
  EXPECT_EQ(dump_of("<table><colgroup></colgroup><col>"), R"(| <html>
|   <head>
|   <body>
|     <table>
|       <colgroup>
|       <colgroup>
|         <col>
)");
  EXPECT_EQ(dump_of("<table><thead><tr><th>a<tbody><tr><td>b<tfoot><tr><td>c"), R"(| <html>
|   <head>
|   <body>
|     <table>
|       <thead>
|         <tr>
|           <th>
|             "a"
|       <tbody>
|         <tr>
|           <td>
|             "b"
|       <tfoot>
|         <tr>
|           <td>
|             "c"
)");
  EXPECT_EQ(dump_of("<table><caption>c<tr><td>d"), R"(| <html>
|   <head>
|   <body>
|     <table>
|       <caption>
|         "c"
|       <tbody>
|         <tr>
|           <td>
|             "d"
)");
  EXPECT_EQ(dump_of("<table><tbody><tr><th>a</th></tr></tbody><td>b"), R"(| <html>
|   <head>
|   <body>
|     <table>
|       <tbody>
|         <tr>
|           <th>
|             "a"
|       <tbody>
|         <tr>
|           <td>
|             "b"
)");
  EXPECT_EQ(dump_of("<table></tbody><tr><td>a</table>"), R"(| <html>
|   <head>
|   <body>
|     <table>
|       <tbody>
|         <tr>
|           <td>
|             "a"
)");
  EXPECT_EQ(dump_of("<table><tr><td>a</td></tr><caption>c"), R"(| <html>
|   <head>
|   <body>
|     <table>
|       <tbody>
|         <tr>
|           <td>
|             "a"
|       <caption>
|         "c"
)");
}

TEST(HtmlTreeTables, ForeignChildrenOfATableAreFosterParented) {
  // Everything the table modes do not take goes in front of the table, through the rules of "in body".
  EXPECT_EQ(dump_of("<table><b>x</b></table>"), R"(| <html>
|   <head>
|   <body>
|     <b>
|       "x"
|     <table>
)");
  EXPECT_EQ(dump_of("<table><div>a</div></table>"), R"(| <html>
|   <head>
|   <body>
|     <div>
|       "a"
|     <table>
)");
  // the formatting element is reopened after the table
  EXPECT_EQ(dump_of("<b><table><i>x</table>y"), R"(| <html>
|   <head>
|   <body>
|     <b>
|       <i>
|         "x"
|       <table>
|       <i>
|         "y"
)");
  EXPECT_EQ(dump_of("<table> x <tr>"), R"(| <html>
|   <head>
|   <body>
|     " x "
|     <table>
|       <tbody>
|         <tr>
)");
  // comments and white space are not foster parented
  EXPECT_EQ(dump_of("<table><!--c--> <tr>"), R"(| <html>
|   <head>
|   <body>
|     <table>
|       <!-- c -->
|       " "
|       <tbody>
|         <tr>
)");
  // hidden is matched without regard to case; another input is foster parented
  EXPECT_EQ(dump_of("<table><input type=HIDDEN><input type=text>"), R"(| <html>
|   <head>
|   <body>
|     <input>
|       type="text"
|     <table>
|       <input>
|         type="HIDDEN"
)");
  EXPECT_EQ(dump_of("<table><style>a</style><script>b</script>"), R"(| <html>
|   <head>
|   <body>
|     <table>
|       <style>
|         "a"
|       <script>
|         "b"
)");
  EXPECT_EQ(dump_of("<table><select><tr>"), R"(| <html>
|   <head>
|   <body>
|     <select>
|     <table>
|       <tbody>
|         <tr>
)");
  // the pointer was set outside the table: the table's form is ignored
  EXPECT_EQ(dump_of("<form><table><form><input type=hidden></table></form>"), R"(| <html>
|   <head>
|   <body>
|     <form>
|       <table>
|         <input>
|           type="hidden"
)");
}

TEST(HtmlTreeTables, CellsCaptionsAndColumnGroups) {
  EXPECT_EQ(dump_of("<table><tr><td><b>x<td>y"), R"(| <html>
|   <head>
|   <body>
|     <table>
|       <tbody>
|         <tr>
|           <td>
|             <b>
|               "x"
|           <td>
|             "y"
)");
  // closing the caption clears the formatting list to its marker
  EXPECT_EQ(dump_of("<table><caption><b>x</caption>y</table>z"), R"(| <html>
|   <head>
|   <body>
|     "y"
|     <table>
|       <caption>
|         <b>
|           "x"
|     "z"
)");
  // ...and so does closing a cell
  EXPECT_EQ(dump_of("<table><tr><td><b>x</td></tr></table><i>y"), R"(| <html>
|   <head>
|   <body>
|     <table>
|       <tbody>
|         <tr>
|           <td>
|             <b>
|               "x"
|     <i>
|       "y"
)");
  EXPECT_EQ(dump_of("<table><colgroup><col><div>"), R"(| <html>
|   <head>
|   <body>
|     <div>
|     <table>
|       <colgroup>
|         <col>
)");
  // white space is a child of the colgroup; x leaves it
  EXPECT_EQ(dump_of("<table><colgroup> <col>x"), R"(| <html>
|   <head>
|   <body>
|     "x"
|     <table>
|       <colgroup>
|         " "
|         <col>
)");
  EXPECT_EQ(dump_of("<table><col></colgroup>x"), R"(| <html>
|   <head>
|   <body>
|     "x"
|     <table>
|       <colgroup>
|         <col>
)");
  // characters that are not white space leave a colgroup
  EXPECT_EQ(dump_of("<table><colgroup> x <col> y"), R"(| <html>
|   <head>
|   <body>
|     "x y"
|     <table>
|       <colgroup>
|         " "
|       <colgroup>
|         <col>
|         " "
)");
}

TEST(HtmlTreeTables, TemplatesHoldTheirChildrenInTheirContent) {
  EXPECT_EQ(dump_of("<template><div>x</div></template>"), R"(| <html>
|   <head>
|     <template>
|       content
|         <div>
|           "x"
|   <body>
)");
  // the inner template ends, the outer one is back in its mode
  EXPECT_EQ(dump_of("<template><template><tr></template><td>"), R"(| <html>
|   <head>
|     <template>
|       content
|         <template>
|           content
|             <tr>
|         <td>
|   <body>
)");
  // a template in a table; the table goes on in "in table"
  EXPECT_EQ(dump_of("<table><template><tr><td>x</template><tr>"), R"(| <html>
|   <head>
|   <body>
|     <table>
|       <template>
|         content
|           <tr>
|             <td>
|               "x"
|       <tbody>
|         <tr>
)");
  // a table in a template; the text after it is in the template, z after it
  EXPECT_EQ(dump_of("<template><table><tr><td>x</table>y</template>z"), R"(| <html>
|   <head>
|     <template>
|       content
|         <table>
|           <tbody>
|             <tr>
|               <td>
|                 "x"
|         "y"
|   <body>
|     "z"
)");
  EXPECT_EQ(dump_of("<template>x<tr>"), R"(| <html>
|   <head>
|     <template>
|       content
|         "x"
|         <tr>
|   <body>
)");
  // html and body start tags are ignored in a template
  EXPECT_EQ(dump_of("<template><html a=1><body b=2><p>"), R"(| <html>
|   <head>
|     <template>
|       content
|         <p>
|   <body>
)");
  // forms nest in a template, and no pointer is kept
  EXPECT_EQ(dump_of("<template><form><form></form></form></template>"), R"(| <html>
|   <head>
|     <template>
|       content
|         <form>
|           <form>
|   <body>
)");
  EXPECT_EQ(dump_of("<template><template>"), R"(| <html>
|   <head>
|     <template>
|       content
|         <template>
|           content
|   <body>
)");
  EXPECT_EQ(dump_of("<p><template>x</template>"), R"(| <html>
|   <head>
|   <body>
|     <p>
|       <template>
|         content
|           "x"
)");
  EXPECT_EQ(dump_of("<head><template></head>x"), R"(| <html>
|   <head>
|     <template>
|       content
|         "x"
|   <body>
)");
  EXPECT_EQ(dump_of("</template>"), R"(| <html>
|   <head>
|   <body>
)");
  EXPECT_EQ(dump_of("<template></div></template>y"), R"(| <html>
|   <head>
|     <template>
|       content
|   <body>
|     "y"
)");
  EXPECT_EQ(dump_of("<table><template></template></table>"), R"(| <html>
|   <head>
|   <body>
|     <table>
|       <template>
|         content
)");
  EXPECT_EQ(dump_of("<template><caption><template><td></template></caption>"), R"(| <html>
|   <head>
|     <template>
|       content
|         <caption>
|           <template>
|             content
|               <td>
|   <body>
)");
  EXPECT_EQ(dump_of("<table><tr><td><template><td>x</template>y"), R"(| <html>
|   <head>
|   <body>
|     <table>
|       <tbody>
|         <tr>
|           <td>
|             <template>
|               content
|                 <td>
|                   "x"
|             "y"
)");
  EXPECT_EQ(dump_of("<table><td><template><tr></table><template>"), R"(| <html>
|   <head>
|   <body>
|     <table>
|       <tbody>
|         <tr>
|           <td>
|             <template>
|               content
|                 <tr>
|                 <template>
|                   content
)");
  EXPECT_EQ(dump_of("<template><col>y z"), R"(| <html>
|   <head>
|     <template>
|       content
|         <col>
|         " "
|   <body>
)");
}

// ---- The table modes, template, foster parenting: the rest of the matrix -----

namespace {

// The insertion mode after a prefix of tags (no text: a character run is held until something ends it).
std::string mode_after(const std::string & input) {
  GTEXT_HTML_TreeOptions o = {};
  GTEXT_HTML_Tree * tree = gtext_html_tree_new(&o);
  EXPECT_NE(tree, nullptr);
  EXPECT_EQ(gtext_html_tree_feed(tree, input.data(), input.size()), GTEXT_HTML_TREE_OK);
  const std::string mode = gtext_html_tree_mode_name(tree);
  gtext_html_tree_free(tree);
  return mode;
}

std::vector<std::string> error_names_of(const std::string & input) {
  return parse(input).error_names();
}

} // namespace

TEST(HtmlTreeTables, TheQuirksModeDecidesWhetherATableClosesAParagraph) {
  // 13.2.6.4.7, start tag "table": "If the Document is not set to quirks mode, and the stack of open
  // elements has a p element in button scope, then close a p element."
  EXPECT_EQ(dump_of("<p>a<table>"), R"(| <html>
|   <head>
|   <body>
|     <p>
|       "a"
|       <table>
)");
  EXPECT_EQ(dump_of("<!DOCTYPE html><p>a<table>"), R"(| <!DOCTYPE html>
| <html>
|   <head>
|   <body>
|     <p>
|       "a"
|     <table>
)");
  // Limited quirks mode is not quirks mode.
  EXPECT_EQ(dump_of("<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\" \"x\"><p>a<table>"),
      R"(| <!DOCTYPE html "-//W3C//DTD XHTML 1.0 Transitional//EN" "x">
| <html>
|   <head>
|   <body>
|     <p>
|       "a"
|     <table>
)");
}

TEST(HtmlTreeTables, TheInsertionModeFollowsTheStandardThroughATableAndATemplate) {
  EXPECT_EQ(mode_after("<table>"), "in table");
  EXPECT_EQ(mode_after("<table><caption>"), "in caption");
  EXPECT_EQ(mode_after("<table><caption></caption>"), "in table");
  EXPECT_EQ(mode_after("<table><colgroup>"), "in column group");
  EXPECT_EQ(mode_after("<table><col>"), "in column group");
  EXPECT_EQ(mode_after("<table><colgroup></colgroup>"), "in table");
  EXPECT_EQ(mode_after("<table><tbody>"), "in table body");
  EXPECT_EQ(mode_after("<table><tr>"), "in row");
  EXPECT_EQ(mode_after("<table><td>"), "in cell");
  EXPECT_EQ(mode_after("<table><td></td>"), "in row");
  EXPECT_EQ(mode_after("<table><td></td></tr>"), "in table body");
  EXPECT_EQ(mode_after("<table><td></td></tr></tbody>"), "in table");
  EXPECT_EQ(mode_after("<table><td></table>"), "in body");
  // The template insertion modes: the first start tag after <template> picks the mode.
  EXPECT_EQ(mode_after("<template>"), "in template");
  EXPECT_EQ(mode_after("<template><div>"), "in body");
  EXPECT_EQ(mode_after("<template><col>"), "in column group");
  EXPECT_EQ(mode_after("<template><colgroup>"), "in column group");
  EXPECT_EQ(mode_after("<template><caption>"), "in caption");
  EXPECT_EQ(mode_after("<template><tbody>"), "in table body");
  EXPECT_EQ(mode_after("<template><tr>"), "in row");
  EXPECT_EQ(mode_after("<template><td>"), "in cell");
  EXPECT_EQ(mode_after("<template><th>"), "in cell");
  EXPECT_EQ(mode_after("<template><tr></template>"), "in head");
  EXPECT_EQ(mode_after("<template><template><tr></template>"), "in template"); // the outer template's mode again
  EXPECT_EQ(mode_after("<template><template></template>"), "in template");
  EXPECT_EQ(mode_after("<table><template>"), "in template");
  EXPECT_EQ(mode_after("<table><template></template>"), "in table");
  EXPECT_EQ(mode_after("<table><tr><template></template>"), "in row");
  EXPECT_EQ(mode_after("<table><td><template></template>"), "in cell");
  EXPECT_EQ(mode_after("<body><template><tr></template>"), "in body");
  EXPECT_EQ(mode_after("<head></head><template>"), "in template");
  EXPECT_EQ(mode_after("<head></head><template></template>"), "after head");
}

TEST(HtmlTreeTables, ErrorsInTablesAreNamedAndPositioned) {
  auto at = [](const std::string & in, size_t i) {
    const Parsed r = parse(in);
    EXPECT_GT(r.errors.size(), i) << in;
    return i < r.errors.size() ? r.errors[i] : GTEXT_HTML_TreeError{};
  };
  // Characters that foster parent: one error for each.
  GTEXT_HTML_TreeError e = at("<table>xy", 1);
  EXPECT_STREQ(gtext_html_tree_error_name(e.code), "unexpected-character");
  EXPECT_EQ(e.line, 1u);
  EXPECT_EQ(e.column, 8u);
  EXPECT_EQ(e.offset, 7u);
  // ...each character at its own place: the second is one column and one byte on.
  EXPECT_STREQ(gtext_html_tree_error_name(at("<table>xy", 2).code), "unexpected-character");
  EXPECT_EQ(at("<table>xy", 2).line, 1u);
  EXPECT_EQ(at("<table>xy", 2).column, 9u);
  EXPECT_EQ(at("<table>xy", 2).offset, 8u);
  EXPECT_STREQ(gtext_html_tree_error_name(at("<table>xy", 3).code), "open-elements-at-eof");
  // A start tag that foster parents, an end tag that does not close what the table has open.
  e = at("<table><div>", 1);
  EXPECT_STREQ(gtext_html_tree_error_name(e.code), "unexpected-start-tag");
  EXPECT_EQ(e.column, 8u);
  e = at("<table><tr></td>", 1);
  EXPECT_STREQ(gtext_html_tree_error_name(e.code), "unexpected-end-tag");
  EXPECT_EQ(e.column, 12u);
  // A table end tag with elements still open in it (the vectors list one; the chapter names none).
  e = at("<table><marquee></table>", 2);
  EXPECT_STREQ(gtext_html_tree_error_name(e.code), "open-elements-at-end-tag");
  EXPECT_EQ(e.column, 17u);
  // A second table start tag inside a table is an error and closes the first.
  e = at("<table>x<table>", 2);
  EXPECT_STREQ(gtext_html_tree_error_name(e.code), "unexpected-start-tag");
  EXPECT_EQ(e.column, 9u);
  // White space alone is fine; a character that is not white space is an error even when the white space
  // before it is not (the whole pending run is foster parented).
  EXPECT_EQ(error_names_of("<!DOCTYPE html><table> <tr> </table>"), std::vector<std::string>{});
  EXPECT_EQ(error_names_of("<!DOCTYPE html><table> x "),
      (std::vector<std::string>{"unexpected-character", "unexpected-character", "unexpected-character", "open-elements-at-eof"}));
}

TEST(HtmlTreeTables, AdoptionAgencyAndReconstructionFosterParent) {
  // The furthest block is moved in front of the table: foster parenting is enabled while "in table" hands
  // the end tag to "in body". Here the b is not in scope (the table is a boundary), and nothing moves.
  EXPECT_EQ(dump_of("<table><tr><b><div></b>"), R"(| <html>
|   <head>
|   <body>
|     <b>
|     <div>
|       <b>
|     <table>
|       <tbody>
|         <tr>
)");
  // A formatting element whose copy is made after the table was left is reopened after it.
  EXPECT_EQ(dump_of("<table><b><i></table>x"), R"(| <html>
|   <head>
|   <body>
|     <b>
|       <i>
|     <table>
|     <b>
|       <i>
|         "x"
)");
  EXPECT_EQ(dump_of("<table><b>x<tr>y"), R"(| <html>
|   <head>
|   <body>
|     <b>
|       "x"
|     <b>
|       "y"
|     <table>
|       <tbody>
|         <tr>
)");
  EXPECT_EQ(dump_of("<a><table><a>"), R"(| <html>
|   <head>
|   <body>
|     <a>
|       <a>
|       <table>
)");
  EXPECT_EQ(dump_of("<table><tr><b><tr></b>"), R"(| <html>
|   <head>
|   <body>
|     <b>
|     <table>
|       <tbody>
|         <tr>
|         <tr>
)");
  // The adoption agency algorithm's last step goes to the appropriate place: a template's content.
  EXPECT_EQ(dump_of("<template><b><div></b>y"), R"(| <html>
|   <head>
|     <template>
|       content
|         <b>
|         <div>
|           <b>
|           "y"
|   <body>
)");
  EXPECT_EQ(dump_of("<b><table><template><i>x</b>"), R"(| <html>
|   <head>
|   <body>
|     <b>
|       <table>
|         <template>
|           content
|             <i>
|               "x"
)");
  // The adoption agency algorithm moves the furthest block in front of a table: the b was foster parented,
  // so it is above the table in the stack and in scope; its common ancestor is the table, foster parenting
  // is on, and the div goes before the table, out of the b.
  EXPECT_EQ(dump_of("<table><b><div>x</b>y"), R"(| <html>
|   <head>
|   <body>
|     <b>
|     <div>
|       <b>
|         "x"
|       "y"
|     <table>
)");
}

TEST(HtmlTreeTables, TextAndElementsAlternateBetweenTheTableAndTheFosterParent) {
  // The text before the table and the text in it are two nodes appended to in turn; ignored end tags
  // between the pieces keep the runs apart.
  const std::string in = "<table> </z>x</z> </z>y</z> </z>z</z></table>";
  EXPECT_EQ(dump_of(in), R"(| <html>
|   <head>
|   <body>
|     "xyz"
|     <table>
|       "   "
)");
}

TEST(HtmlTreeTables, PendingTableTextIsFlushedByWhateverEndsIt) {
  // White space only: it is inserted in the table, whatever follows.
  EXPECT_EQ(dump_of("<table> <!--c-->"), R"(| <html>
|   <head>
|   <body>
|     <table>
|       " "
|       <!-- c -->
)");
  EXPECT_EQ(dump_of("<table> "), R"(| <html>
|   <head>
|   <body>
|     <table>
|       " "
)");
  // NUL is dropped (and is an error) without ending the run, so a run of NUL and white space is white space.
  EXPECT_EQ(dump_of(std::string("<table> ") + std::string(1, '\0') + " </table>"), R"(| <html>
|   <head>
|   <body>
|     <table>
|       "  "
)");
  // The text of a pending run that holds something else is foster parented as a whole, white space first
  // included, and the element that ends the run comes after.
  EXPECT_EQ(dump_of("<table> x <b>y"), R"(| <html>
|   <head>
|   <body>
|     " x "
|     <b>
|       "y"
|     <table>
)");
  // The run is the same however it is cut.
  for (size_t k = 0; k <= 12; k++) {
    EXPECT_TRUE(parse("<table> x <b>y").same_as(parse("<table> x <b>y", {}, {k}))) << k;
  }
}

TEST(HtmlTreeTables, TemplateContentsAreADocumentFragmentTheElementOwns) {
  const std::string in = "<template><p>a</p><template>b</template></template><div>";
  GTEXT_HTML_Tree * tree = gtext_html_tree_new(nullptr);
  ASSERT_NE(tree, nullptr);
  ASSERT_EQ(gtext_html_tree_feed(tree, in.data(), in.size()), GTEXT_HTML_TREE_OK);
  ASSERT_EQ(gtext_html_tree_finish(tree), GTEXT_HTML_TREE_OK);
  GTEXT_Markup_Document * doc = gtext_html_tree_take_document(tree);
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(std::string(markup_check_invariants(doc) ? markup_check_invariants(doc) : ""), "");
  const GTEXT_Markup_Node * html = gtext_markup_node_first_child(gtext_markup_document_root(doc));
  const GTEXT_Markup_Node * head = gtext_markup_node_first_child(html);
  const GTEXT_Markup_Node * tpl = gtext_markup_node_first_child(head);
  ASSERT_NE(tpl, nullptr);
  EXPECT_STREQ(gtext_markup_element_local_name(tpl), "template");
  // The element has no children of its own: what the standard puts in the template is in its content.
  EXPECT_EQ(gtext_markup_node_first_child(tpl), nullptr);
  const GTEXT_Markup_Node * content = gtext_markup_element_template_content(tpl);
  ASSERT_NE(content, nullptr);
  EXPECT_EQ(gtext_markup_node_kind(content), GTEXT_MARKUP_KIND_FRAGMENT);
  const GTEXT_Markup_Node * p = gtext_markup_node_first_child(content);
  ASSERT_NE(p, nullptr);
  EXPECT_STREQ(gtext_markup_element_local_name(p), "p");
  const GTEXT_Markup_Node * inner = gtext_markup_node_next_sibling(p);
  ASSERT_NE(inner, nullptr);
  EXPECT_STREQ(gtext_markup_element_local_name(inner), "template");
  EXPECT_NE(gtext_markup_element_template_content(inner), nullptr);
  gtext_markup_document_free(doc);
  gtext_html_tree_free(tree);
}

TEST(HtmlTreeTables, DepthIsLimitedByTheSameStatusInTablesAndTemplates) {
  GTEXT_HTML_TreeOptions o = {};
  for (const std::string & unit : {std::string("<template>"), std::string("<table><td>"),
           std::string("<table><tr><td>"), std::string("<table><template><tr><td>"),
           std::string("<template><table><tbody><tr><td><template>")}) {
    SCOPED_TRACE(unit);
    // 600 of them: more than the 512 open elements the default allows.
    const Parsed r = parse(repeat(unit, 600), o);
    EXPECT_EQ(r.status, GTEXT_HTML_TREE_E_LIMIT_DEPTH);
    EXPECT_FALSE(r.has_tree);
    // Cut a byte at a time, the same answer.
    const Parsed bytes = parse(repeat(unit, 600), o, {}, true);
    EXPECT_TRUE(r.same_as(bytes));
    // Under the limit it is a tree.
    EXPECT_EQ(parse(repeat(unit, 60), o).status, GTEXT_HTML_TREE_OK);
  }
  // The limit counts the open elements, so a closed template does not count.
  EXPECT_EQ(parse(repeat("<template></template>", 2000), o).status, GTEXT_HTML_TREE_OK);
  EXPECT_EQ(parse(repeat("<table><tr><td></td></tr></table>", 2000), o).status, GTEXT_HTML_TREE_OK);
}

TEST(HtmlTreeTables, TheEndOfTheFileEndsTemplatesOneByOne) {
  // Nested templates are closed by the end of the file one at a time, each reprocessing the token: the
  // chain is as long as the nesting.
  const Parsed r = parse(repeat("<template>", 480));
  EXPECT_EQ(r.status, GTEXT_HTML_TREE_OK);
  EXPECT_EQ(r.invariants, "");
  size_t templates = 0;
  for (size_t at = r.dump.find("<template>"); at != std::string::npos; at = r.dump.find("<template>", at + 1)) {
    templates++;
  }
  EXPECT_EQ(templates, 480u);
  // The end of the file in a table inside a template ends the template too.
  EXPECT_EQ(dump_of("<template><table><tr><td>x"), R"(| <html>
|   <head>
|     <template>
|       content
|         <table>
|           <tbody>
|             <tr>
|               <td>
|                 "x"
|   <body>
)");
  const std::vector<std::string> names = error_names_of("<!DOCTYPE html><template><tr>");
  EXPECT_EQ(names, std::vector<std::string>{"open-elements-at-eof"});
}

TEST(HtmlTreeTables, ATemplateEndTagClosesTheTableStuffInsideIt) {
  // "Generate all implied end tags thoroughly": the cell, the row, the section close without an error.
  EXPECT_EQ(error_names_of("<!DOCTYPE html><template><tr><td>x</template>"), std::vector<std::string>{});
  EXPECT_EQ(error_names_of("<!DOCTYPE html><template><tbody><tr><td><p>x</template>"), std::vector<std::string>{});
  // ...but a div is not implied: an error.
  EXPECT_EQ(error_names_of("<!DOCTYPE html><template><div>x</template>"),
      std::vector<std::string>{"open-elements-at-end-tag"});
  // The list of active formatting elements is cleared to the template's marker.
  EXPECT_EQ(dump_of("<b><template><i>x</template>y"), R"(| <html>
|   <head>
|   <body>
|     <b>
|       <template>
|         content
|           <i>
|             "x"
|       "y"
)");
  // A template end tag with no template open is an error and ignored, in every mode that hands it on.
  EXPECT_EQ(error_names_of("<!DOCTYPE html></template>"), std::vector<std::string>{"unexpected-end-tag"});
  EXPECT_EQ(error_names_of("<!DOCTYPE html><table></template>"),
      (std::vector<std::string>{"unexpected-end-tag", "open-elements-at-eof"}));
  EXPECT_EQ(error_names_of("<!DOCTYPE html><table><colgroup></template>"),
      (std::vector<std::string>{"unexpected-end-tag", "open-elements-at-eof"}));
}

TEST(HtmlTreeTables, EveryCharacterOfAFosterParentedRunHasItsOwnPlace) {
  auto errors_of = [](const std::string & in) {
    std::vector<std::array<size_t, 3>> out;
    for (const auto & e : parse(in).errors) {
      if (std::string(gtext_html_tree_error_name(e.code)) == "unexpected-character") {
        out.push_back({e.line, e.column, e.offset});
      }
    }
    return out;
  };
  using P = std::vector<std::array<size_t, 3>>;
  const std::string d = "<!DOCTYPE html><table>"; // 22 bytes
  // Plain characters: a column and a byte each.
  EXPECT_EQ(errors_of(d + "xyz"), (P{{1, 23, 22}, {1, 24, 23}, {1, 25, 24}}));
  // A character reference is one character at the place of its `&`, and what follows counts from the `;`.
  EXPECT_EQ(errors_of(d + "a&amp;b"), (P{{1, 23, 22}, {1, 24, 23}, {1, 29, 28}}));
  // A carriage return and a line feed are one line feed, at the place of the carriage return; the next
  // line starts at column 1 two bytes on.
  EXPECT_EQ(errors_of(d + "a\r\nb"), (P{{1, 23, 22}, {1, 24, 23}, {2, 1, 25}}));
  // White space that waited is foster parented with what ended its wait, each at its own place.
  EXPECT_EQ(errors_of(d + " \n x"), (P{{1, 23, 22}, {1, 24, 23}, {2, 1, 24}, {2, 2, 25}}));
  // Columns are UTF-16 units, bytes are bytes: two code points of two bytes, one of four (two units).
  EXPECT_EQ(errors_of(d + "\xC3\xA9\xC3\xA9\xF0\x9F\x98\x80z"), (P{{1, 23, 22}, {1, 24, 24}, {1, 25, 26}, {1, 27, 30}}));
  // A NUL is one error from the tree builder ("in body" ignores it), not two, and the characters either
  // side of it keep their places.
  const Parsed nul = parse(d + std::string("a\0b", 3));
  size_t nuls = 0;
  for (const auto & e : nul.errors) {
    if (std::string(gtext_html_tree_error_name(e.code)) == "unexpected-null-character"
        && e.code >= GTEXT_HTML_TREE_ERROR_BASE) {
      nuls++;
    }
  }
  EXPECT_EQ(nuls, 1u);
  EXPECT_EQ(errors_of(d + std::string("a\0b", 3)), (P{{1, 23, 22}, {1, 25, 24}}));
  // The same through "anything else" with an element open (not through the pending text), and in a
  // column group that is not the current node.
  EXPECT_EQ(errors_of("<!DOCTYPE html><table><b>xy"), (P{{1, 26, 25}, {1, 27, 26}}));
  EXPECT_EQ(error_names_of("<!DOCTYPE html><table><b>\xE2\x80\x8B").size(), 3u); // unexpected-start-tag, character, eof
  EXPECT_EQ(errors_of("<!DOCTYPE html><template><col>ab"), (P{{1, 31, 30}, {1, 32, 31}}));
  // However the input is fed, the places are the same; a run with many pieces (a reference every other
  // character) is exact for its first 1,024 and counted on after that.
  std::string many = d;
  for (int i = 0; i < 700; i++) {
    many += "&amp;x";
  }
  const Parsed whole = parse(many);
  EXPECT_TRUE(whole.same_as(parse(many, {}, {1, 2, 3, 5, 7})));
  EXPECT_TRUE(whole.same_as(parse(many, {}, {}, true)));
  std::vector<GTEXT_HTML_TreeError> chars;
  for (const auto & e : whole.errors) {
    if (std::string(gtext_html_tree_error_name(e.code)) == "unexpected-character") {
      chars.push_back(e);
    }
  }
  ASSERT_EQ(chars.size(), 1400u);
  // The characters alternate: a reference (6 bytes) and an x, each a piece of its own.
  for (size_t k = 0; k < 512; k++) {
    EXPECT_EQ(chars[2 * k].offset, 22u + 6 * k) << k;      // the `&`
    EXPECT_EQ(chars[2 * k + 1].offset, 27u + 6 * k) << k;  // the x after `&amp;`
  }
  for (size_t k = 1; k < chars.size(); k++) {
    EXPECT_GT(chars[k].offset, chars[k - 1].offset);       // past the 1,024th piece: counted on, still forward
  }
}

TEST(HtmlTreeTables, WhiteSpaceThatWaitsInATableIsOneTextNodeOfItsFullLength) {
  const std::string ws = repeat(" \n\t", 700); // 2,100 bytes, over the pending buffer's first size
  const std::string in = "<!DOCTYPE html><table>" + ws + "<tr>";
  for (const auto & cuts : {std::vector<size_t>{}, std::vector<size_t>{30, 31, 100, 1000}}) {
    const Parsed r = parse(in, {}, cuts);
    ASSERT_EQ(r.status, GTEXT_HTML_TREE_OK);
    const std::string want = "|       \"" + ws + "\"\n";
    EXPECT_NE(r.dump.find(want), std::string::npos);
    EXPECT_EQ(std::count(r.dump.begin(), r.dump.end(), '"'), 2) << "one text node";
  }
  // Fed a byte at a time too, and the pending text is the same node.
  EXPECT_TRUE(parse(in).same_as(parse(in, {}, {}, true)));
  // The bytes asked of realloc grow with the text, not with its square (the pending buffer doubles).
  auto bytes_for = [](size_t n) { return realloc_bytes_of("<table>" + repeat(" ", n) + "<tr>"); };
  EXPECT_LT((double)bytes_for(40000) / (double)bytes_for(10000), 6.0);
  // With a white space run longer than the text limit, the tokenizer's limit is the answer and there is no tree.
  GTEXT_HTML_TreeOptions o = {};
  o.max_text_bytes = 1000;
  const Parsed over = parse("<table>" + repeat(" ", 1001) + "<tr>", o);
  EXPECT_EQ(over.status, GTEXT_HTML_TREE_E_LIMIT_TEXT);
  EXPECT_FALSE(over.has_tree);
  // And an allocation failure while the white space waits is OOM with nothing left over.
  for (size_t n = 1;; n++) {
    FaultAllocator fa;
    GTEXT_HTML_TreeOptions fo = {};
    fo.allocator = &fa.alloc;
    fa.arm(n);
    const Parsed r = parse("<table> \x00 \n <tr>", fo, {5});
    EXPECT_TRUE(fa.live.empty()) << n;
    if (!fa.failed) {
      EXPECT_EQ(r.status, GTEXT_HTML_TREE_OK);
      break;
    }
    EXPECT_EQ(r.status, GTEXT_HTML_TREE_E_OOM);
    ASSERT_LT(n, 10000u);
  }
}

TEST(HtmlTreeTables, TheReprocessLimitIsItsOwnStickyStatus) {
  // Normal documents reprocess a token at most a handful of times; the limit exists for a defect.
  // Lowered to one, the first token that needs two reprocessings (a td in a table: tbody, then tr) hits it.
  FaultAllocator fa;
  GTEXT_HTML_TreeOptions o = {};
  o.allocator = &fa.alloc;
  GTEXT_HTML_Tree * tree = gtext_html_tree_new(&o);
  ASSERT_NE(tree, nullptr);
  gtext_html_tree_test_set_reprocess_limit(tree, 1);
  const std::string in = "<table><td>x</td></table>";
  GTEXT_HTML_TreeStatus st = gtext_html_tree_feed(tree, in.data(), in.size());
  EXPECT_EQ(st, GTEXT_HTML_TREE_E_REPROCESS);
  EXPECT_EQ(gtext_html_tree_feed(tree, "y", 1), GTEXT_HTML_TREE_E_REPROCESS); // sticky
  EXPECT_EQ(gtext_html_tree_finish(tree), GTEXT_HTML_TREE_E_REPROCESS);
  EXPECT_EQ(gtext_html_tree_take_document(tree), nullptr);
  gtext_html_tree_free(tree);
  EXPECT_TRUE(fa.live.empty());
  EXPECT_EQ(fa.bad_frees, 0u);
  // The default is not near any real chain: the deepest chain of a document (a cell closed by the
  // end tag of its table) is 5, whatever the nesting.
  EXPECT_EQ(parse(repeat("<table><tr><td>", 100) + "x" + repeat("</table>", 100)).status, GTEXT_HTML_TREE_OK);
  // The characters' own guard: a limit of zero stops the first mode switch that consumes nothing.
  GTEXT_HTML_Tree * t2 = gtext_html_tree_new(nullptr);
  gtext_html_tree_test_set_reprocess_limit(t2, 0);
  const std::string text = "plain text";
  EXPECT_EQ(gtext_html_tree_feed(t2, text.data(), text.size()), GTEXT_HTML_TREE_OK);
  EXPECT_EQ(gtext_html_tree_finish(t2), GTEXT_HTML_TREE_E_REPROCESS);
  gtext_html_tree_free(t2);
}

TEST(HtmlTreeTables, ClosingNestedTemplatesAtTheEndOfTheFileIsALoopNotAChain) {
  // A raised open-element limit must not make the end of the file reprocess once for every template.
  GTEXT_HTML_TreeOptions o = {};
  o.max_open_elements = 60000;
  // (No dump: a tree 50,000 deep written with indentation is gigabytes.)
  GTEXT_HTML_Tree * big = gtext_html_tree_new(&o);
  const std::string templates = repeat("<template>", 50000);
  EXPECT_EQ(gtext_html_tree_feed(big, templates.data(), templates.size()), GTEXT_HTML_TREE_OK);
  EXPECT_EQ(gtext_html_tree_finish(big), GTEXT_HTML_TREE_OK);
  EXPECT_GE(gtext_html_tree_error_count(big) + gtext_html_tree_errors_dropped(big), 50000u);
  GTEXT_Markup_Document * doc = gtext_html_tree_take_document(big);
  ASSERT_NE(doc, nullptr);
  gtext_markup_document_free(doc);
  gtext_html_tree_free(big);
  // ...and with the limit at the chain's real length (it is only a few) a document still parses.
  GTEXT_HTML_Tree * tree = gtext_html_tree_new(&o);
  gtext_html_tree_test_set_reprocess_limit(tree, 8);
  const std::string in = repeat("<template><table><tr><td>", 100);
  EXPECT_EQ(gtext_html_tree_feed(tree, in.data(), in.size()), GTEXT_HTML_TREE_OK);
  EXPECT_EQ(gtext_html_tree_finish(tree), GTEXT_HTML_TREE_OK);
  gtext_html_tree_free(tree);
}

TEST(HtmlTreeTables, AMarkupTextNodeForgetsItsBufferSizeWhenItsDataIsReplaced) {
  // cap_log2 audit: the data of a text node is made by create (size 0 = exact), grown by markup_text_append
  // (size recorded), replaced by gtext_markup_node_set_data (size forgotten), and copied by
  // gtext_markup_node_clone, which makes a new node. Each is followed by an append that would write
  // past the buffer if the size were stale (the suites run under AddressSanitizer).
  GTEXT_Markup_Options mo = gtext_markup_options_default();
  GTEXT_Markup_Document * doc = nullptr;
  ASSERT_EQ(gtext_markup_document_new(&mo, &doc), GTEXT_MARKUP_OK);
  auto grown = [&](GTEXT_Markup_Node ** node, GTEXT_Markup_Kind kind) {
    ASSERT_EQ(kind == GTEXT_MARKUP_KIND_TEXT ? gtext_markup_create_text(doc, "a", 1, node)
            : kind == GTEXT_MARKUP_KIND_CDATA ? gtext_markup_create_cdata(doc, "a", 1, node)
                                              : gtext_markup_create_comment(doc, "a", 1, node),
        GTEXT_MARKUP_OK);
    size_t cap = 0;
    for (int i = 0; i < 100; i++) {
      ASSERT_EQ(markup_text_append(doc, *node, &cap, "bcde", 4), GTEXT_MARKUP_OK);
    }
    EXPECT_GE(cap, 401u);
  };
  for (GTEXT_Markup_Kind kind : {GTEXT_MARKUP_KIND_TEXT, GTEXT_MARKUP_KIND_CDATA, GTEXT_MARKUP_KIND_COMMENT}) {
    GTEXT_Markup_Node * n = nullptr;
    grown(&n, kind);
    ASSERT_NE(n, nullptr);
    // Replace the data with a short string: the buffer is now exactly that, whatever it was.
    ASSERT_EQ(gtext_markup_node_set_data(n, "z", 1), GTEXT_MARKUP_OK);
    size_t cap = 0;
    for (int i = 0; i < 300; i++) {
      ASSERT_EQ(markup_text_append(doc, n, &cap, "0123456789", 10), GTEXT_MARKUP_OK);
    }
    size_t len = 0;
    const char * data = gtext_markup_node_data(n, &len);
    EXPECT_EQ(len, 3001u);
    EXPECT_EQ(data[len], '\0');
    EXPECT_EQ(std::string(data, 11), "z012345678" "9");
    // A copy is a new node with an exact buffer, grown on its own.
    GTEXT_Markup_Node * copy = nullptr;
    ASSERT_EQ(gtext_markup_clone(doc, n, false, &copy), GTEXT_MARKUP_OK);
    ASSERT_EQ(gtext_markup_node_set_data(n, "q", 1), GTEXT_MARKUP_OK);
    size_t cap2 = 0;
    for (int i = 0; i < 300; i++) {
      ASSERT_EQ(markup_text_append(doc, copy, &cap2, "abc", 3), GTEXT_MARKUP_OK);
      ASSERT_EQ(markup_text_append(doc, n, &cap, "xy", 2), GTEXT_MARKUP_OK);
    }
    size_t l2 = 0, l3 = 0;
    gtext_markup_node_data(copy, &l2);
    gtext_markup_node_data(n, &l3);
    EXPECT_EQ(l2, 3001u + 900u);
    EXPECT_EQ(l3, 1u + 600u);
  }
  gtext_markup_document_free(doc);
  // The byte sits in padding (markup_tree.c asserts it): no node got bigger.
  EXPECT_EQ(offsetof(struct GTEXT_Markup_Node, line), 4u);
}

TEST(HtmlTreeTables, TheResetTestHelperRefusesAModeItDoesNotKnow) {
  const char * names[] = {"html", "body", "template/in the middle of nowhere"};
  EXPECT_EQ(gtext_html_tree_test_reset_mode(names, 3, true), nullptr);
  const char * ok[] = {"html", "body", "template/in row"};
  ASSERT_NE(gtext_html_tree_test_reset_mode(ok, 3, true), nullptr);
}

TEST(HtmlTreeTables, AFormInATableInATemplateIsInsertedAsTheServedStandardSays) {
  // 13.2.6.4.9, "in table", start tag "form", as served 2026-10-12: "Parse error. If the form element pointer
  // is not null, and the parser is not parsing template contents, then ignore the token. Otherwise: Insert an
  // HTML element for the token, and, if the parser is not parsing template contents, set the form element
  // pointer to point to the element created. Pop that form element off the stack of open elements."
  // So in a template the pointer does not matter: the form is inserted (and popped at once), set or not.
  EXPECT_EQ(dump_of("<template><table><form></table></template>"), R"(| <html>
|   <head>
|     <template>
|       content
|         <table>
|           <form>
|   <body>
)");
  EXPECT_EQ(dump_of("<form><template><table><form>"), R"(| <html>
|   <head>
|   <body>
|     <form>
|       <template>
|         content
|           <table>
|             <form>
)");
  // ...and it sets no pointer there, so a second form in the same table is inserted too (both references
  // ignore both, which is why no vector or extra case holds this).
  EXPECT_EQ(dump_of("<template><table><form><form>"), R"(| <html>
|   <head>
|     <template>
|       content
|         <table>
|           <form>
|           <form>
|   <body>
)");
  // Outside a template the pointer decides: the second form is ignored.
  EXPECT_EQ(dump_of("<table><form><form>"), R"(| <html>
|   <head>
|   <body>
|     <table>
|       <form>
)");
  // "After head", anything else: "Insert an HTML element for a "body" start tag token with no attributes. Set
  // the frameset-ok flag to "ok"." and the template start tag had set it to "not ok": the frameset replaces
  // the body that the p made.
  EXPECT_EQ(dump_of("<head><template></template></head><p><frameset>"), R"(| <html>
|   <head>
|     <template>
|       content
|   <frameset>
)");
}

TEST(HtmlTreeTables, ATemplateStartTagPutsAMarkerInTheFormattingList) {
  // The b is closed with its p but still in the list of active formatting elements; the marker keeps it
  // from being reopened inside the template, and it comes back after the template ends. (parse5 and
  // this library agree; html5lib 1.1 reopens it before the template.)
  EXPECT_EQ(dump_of("<p><b>x</p><template>y</template>z"), R"(| <html>
|   <head>
|   <body>
|     <p>
|       <b>
|         "x"
|     <template>
|       content
|         "y"
|     <b>
|       "z"
)");
  EXPECT_EQ(dump_of("<p><b>x</p><template><div><p>y"), R"(| <html>
|   <head>
|   <body>
|     <p>
|       <b>
|         "x"
|     <template>
|       content
|         <div>
|           <p>
|             "y"
)");
}

TEST(HtmlTreeTables, TheFormElementPointerIsLeftAloneInATemplate) {
  EXPECT_EQ(dump_of("<template><form></template><form><form>"), R"(| <html>
|   <head>
|     <template>
|       content
|         <form>
|   <body>
|     <form>
)");
  // In a template, the end tag closes the nearest form in scope.
  EXPECT_EQ(dump_of("<template><form><div></form>x</template>"), R"(| <html>
|   <head>
|     <template>
|       content
|         <form>
|           <div>
|         "x"
|   <body>
)");
  // ...and a stray one is an error.
  EXPECT_EQ(error_names_of("<!DOCTYPE html><template></form></template>"), std::vector<std::string>{"unexpected-end-tag"});
}

TEST(HtmlTreeTables, ASelectInATableIsParsedByTheBodyRules) {
  // The standard as served has no "in select" mode: a select in a cell is closed by the next cell like
  // any open element. (A tr start tag with a select open leaves the select the same way: "in table" clears
  // the stack back to the table, see ForeignChildrenOfATableAreFosterParented.)
  EXPECT_EQ(dump_of("<table><tr><td><select><option>a<td>b"), R"(| <html>
|   <head>
|   <body>
|     <table>
|       <tbody>
|         <tr>
|           <td>
|             <select>
|               <option>
|                 "a"
|           <td>
|             "b"
)");
}

TEST(HtmlTreeTables, ForeignContentInATableCellIsAnElementOfTheCell) {
  EXPECT_EQ(dump_of("<table><tr><td><svg><g>x</g></svg>y"),
      tree({"<html>", "  <head>", "  <body>", "    <table>", "      <tbody>", "        <tr>", "          <td>",
          "            <svg svg>", "              <svg g>", "                \"x\"", "            \"y\""}));
}

// ---- Work: tables, templates and foster parenting ------------------------------

TEST(HtmlTreeWork, TablesAndTemplatesAreLinear) {
  expect_linear("nested tables", "<table><tr><td>", 400);
  expect_linear("many rows", "<tr><td>x</td></tr>", 3000, "<table>");
  expect_linear("many cells left open", "<td>x", 4000, "<table><tr>");
  expect_linear("many rows left open", "<tr>", 4000, "<table>");
  expect_linear("many sections", "<tbody><tr><td>x", 2000, "<table>");
  expect_linear("many row closes", "</tr><tr>", 4000, "<table><tr>");
  expect_linear("stray cell end tags", "</td>", 4000, "<table><tr><td>");
  expect_linear("stray row end tags in a cell", "</tr><td>", 3000, "<table><tr><td>");
  expect_linear("tables one after another", "<table><tr><td>x</td></tr></table>", 1500);
  expect_linear("cells with formatting", "<td><b>x", 1500, "<table><tr>");
  expect_linear("cells with a select", "<td><select><option>x", 1500, "<table><tr>");
  expect_linear("captions", "<caption>x", 3000, "<table>");
  expect_linear("column groups", "<colgroup><col>", 3000, "<table>");
  expect_linear("col after col", "<col>", 4000, "<table>");
  expect_linear("a table start inside a table", "<table>", 3000);
}

TEST(HtmlTreeWork, FosterParentingIsLinear) {
  // Text before the table, in front of it: one node, appended to.
  expect_linear("foster parented text", "x</z>", 4000, "<table>");
  expect_linear("a long run of text", "xxxxxxxxxxxxxxxx", 4000, "<table>");
  // The text before the table and the text in it, in turn.
  expect_linear("foster text and table text alternate", " </z>x</z>", 3000, "<table>");
  expect_linear("foster parented elements", "<b>x", 2000, "<table>");
  expect_linear("foster parented blocks", "<div>x</div>", 2000, "<table>");
  expect_linear("foster parented blocks in a row", "<div>x", 1500, "<table><tr>");
  expect_linear("formatting reopened after a table", "<b></table>x<table>", 1500);
  expect_linear("the adoption agency moves blocks in front of a table", "<b><div>x</b>y", 1200, "<table>");
  expect_linear("end tags in a table", "</b>", 4000, "<table><tr><td></td>");
  expect_linear("foster parenting inside a template", "x</z>", 3000, "<template><table>");
}

TEST(HtmlTreeWork, TemplatesAreLinear) {
  expect_linear("nested templates", "<template>", 400);
  expect_linear("templates with rows", "<template><tr><td>x</template>", 2000);
  expect_linear("template end tags with nothing open", "</template>", 4000);
  expect_linear("templates in cells", "<td><template><td>x</template>", 1500, "<table><tr>");
  expect_linear("a mode for every template", "<template><col></template><template><tr></template>", 1000);
  expect_linear("many children", "<tr><td>x</td></tr>", 3000, "<template>");
}


TEST(HtmlTreeWork, TextThatAlternatesBetweenNodesIsNotCopiedOnEveryAppend) {
  // The text before the table and the text in it grow in turn, and a new element with a new text node
  // comes between the pieces: a builder that remembered only the last node or two for the size of its
  // buffer would grow the old ones by a copy each time.
  auto make = [](size_t n) { return "<table>" + repeat(" </z>x</z><i>y</i>", n); };
  const size_t a = realloc_bytes_of(make(1500));
  const size_t b = realloc_bytes_of(make(6000));
  ASSERT_GT(a, 0u);
  EXPECT_LT((double)b / (double)a, 7.0) << a << " -> " << b;
  // Text in one node alone, as a control.
  auto one = [](size_t n) { return "<p>" + repeat("a</z>", n); };
  EXPECT_LT((double)realloc_bytes_of(one(24000)) / (double)realloc_bytes_of(one(6000)), 7.0);
}

TEST(HtmlTreeWork, NestedTablesCloseInLinearTime) {
  // n tables open, then n end tags: each pops one table's worth.
  auto make = [](size_t n) { return repeat("<table><tr><td>x", n) + repeat("</table>", n); };
  const Work a = measure(make(300));
  const Work b = measure(make(1200));
  EXPECT_LT((double)b.tree / (double)a.tree, 6.0) << a.tree << " -> " << b.tree;
  EXPECT_LE(b.tree, GTEXT_HTML_TREE_WORK_BOUND(make(1200).size()));
  auto templates = [](size_t n) { return repeat("<template><div>", n) + repeat("</template>", n); };
  const Work c = measure(templates(300));
  const Work d = measure(templates(1200));
  EXPECT_LT((double)d.tree / (double)c.tree, 6.0) << c.tree << " -> " << d.tree;
}

// ---- The dump writer ---------------------------------------------------------

TEST(HtmlTreeDump, ATemplatesContentIsWrittenUnderContentAndAnEmptyValueIsSafe) {
  GTEXT_Markup_Options mo = gtext_markup_options_default();
  GTEXT_Markup_Document * doc = nullptr;
  ASSERT_EQ(gtext_markup_document_new(&mo, &doc), GTEXT_MARKUP_OK);
  GTEXT_Markup_Node *tpl = nullptr, *content = nullptr, *p = nullptr, *text = nullptr, *deep = nullptr;
  ASSERT_EQ(gtext_markup_create_element(doc, GTEXT_MARKUP_NS_HTML, nullptr, "template", &tpl), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(gtext_markup_document_root(doc), tpl), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_element_ensure_template_content(tpl, &content), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_create_element(doc, GTEXT_MARKUP_NS_HTML, nullptr, "p", &p), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(content, p), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_element_set_attribute(p, nullptr, nullptr, "a", nullptr, 0), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_create_element(doc, GTEXT_MARKUP_NS_HTML, nullptr, "template", &deep), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(p, deep), GTEXT_MARKUP_OK);
  GTEXT_Markup_Node * inner = nullptr;
  ASSERT_EQ(gtext_markup_element_ensure_template_content(deep, &inner), GTEXT_MARKUP_OK);
  GTEXT_Markup_Node * leaf = nullptr;
  ASSERT_EQ(gtext_markup_create_text(doc, "in", 2, &leaf), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(inner, leaf), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_create_text(doc, "x", 1, &text), GTEXT_MARKUP_OK);
  ASSERT_EQ(gtext_markup_append_child(tpl, text), GTEXT_MARKUP_OK);
  size_t n = 0;
  char * d = htd_dump(doc, &n);
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(std::string(d, n), tree({"<template>", "  content", "    <p>", "      a=\"\"", "      <template>",
                                  "        content", "          \"in\"", "  \"x\""}));
  std::free(d);
  gtext_markup_document_free(doc);
}
