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
 * Contracts of the XML, HTML, markup and Encoding code that no other test states, found by the coverage review of
 * Phase 3k (`tools/coverage-dirs.py`): argument validation of entry points, the statuses of a file that cannot be
 * read, the copy of a prefixed element and of a namespaced attribute, and the code points above U+FFFF written
 * by the tokenizer. Each test names the line it was written for in a comment only where that helps; the claim is
 * the contract.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include <ghoti.io/text/html.h>
#include <ghoti.io/text/markup.h>
#include <ghoti.io/text/xml.h>

#include "../src/html/html_tokenizer_internal.h"

namespace {

#ifndef _WIN32
// A file with no permission at all, in the temporary directory; removed by the destructor.
struct Unreadable {
  std::string path;
  Unreadable() {
    char name[] = "/tmp/gtext-gaps-XXXXXX";
    const int fd = mkstemp(name);
    EXPECT_GE(fd, 0);
    if (fd >= 0) {
      EXPECT_EQ(write(fd, "<a/>", 4), 4);
      close(fd);
    }
    path = name;
    EXPECT_EQ(chmod(path.c_str(), 0), 0);
  }
  ~Unreadable() {
    chmod(path.c_str(), 0600);
    unlink(path.c_str());
  }
};
#endif

}  // namespace

TEST(CoverageGapsXml, AReaderFedNothingAtAllOrNullBytesSaysInvalidAndTheErrorIsFilled) {
  EXPECT_EQ(gtext_xml_reader_feed(nullptr, "<a/>", 4, nullptr), GTEXT_XML_E_INVALID);
  GTEXT_XML_Reader * r = gtext_xml_reader_new(nullptr);
  ASSERT_NE(r, nullptr);
  GTEXT_XML_Error err;
  memset(&err, 0x55, sizeof(err));
  EXPECT_EQ(gtext_xml_reader_feed(r, nullptr, 3, &err), GTEXT_XML_E_INVALID);
  EXPECT_EQ(err.code, GTEXT_XML_E_INVALID);
  EXPECT_STREQ(err.message, gtext_xml_status_string(GTEXT_XML_E_INVALID));
  EXPECT_EQ(err.line, 1u);
  EXPECT_EQ(err.column, 1u);
  // The refusal did not fail the reader: the document can still be fed.
  EXPECT_EQ(gtext_xml_reader_feed(r, "<a/>", 4, &err), GTEXT_XML_OK);
  EXPECT_EQ(gtext_xml_reader_feed(r, nullptr, 0, &err), GTEXT_XML_OK);
  // After the end, null bytes with a length are the same refusal as any bytes after the end: a state error first.
  EXPECT_EQ(gtext_xml_reader_feed(r, nullptr, 3, &err), GTEXT_XML_E_STATE);
  gtext_xml_reader_free(r);
}

TEST(CoverageGapsXml, AFileWithoutPermissionIsAnInvalidArgumentThatSaysSo) {
#ifdef _WIN32
  GTEST_SKIP() << "a file with no permission at all is a POSIX idea";
#else
  if (geteuid() == 0) {
    GTEST_SKIP() << "root reads any file";
  }
  Unreadable f;
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_XML_Error err;
  EXPECT_EQ(gtext_xml_parse_file(f.path.c_str(), nullptr, &doc, &err), GTEXT_XML_E_INVALID);
  EXPECT_EQ(doc, nullptr);
  EXPECT_STREQ(err.message, "Permission denied opening file");
#endif
}

TEST(CoverageGapsHtml, AFileWithoutPermissionIsAnInvalidArgumentThatSaysSo) {
#ifdef _WIN32
  GTEST_SKIP() << "a file with no permission at all is a POSIX idea";
#else
  if (geteuid() == 0) {
    GTEST_SKIP() << "root reads any file";
  }
  Unreadable f;
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_HTML_Result result;
  EXPECT_EQ(gtext_html_parse_file(f.path.c_str(), nullptr, &doc, &result), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(doc, nullptr);
  EXPECT_STREQ(result.message, "Permission denied opening file");
  gtext_html_result_free(&result);
#endif
}

TEST(CoverageGapsMarkup, ACopyOfAPrefixedElementKeepsItsPrefixAndThoseOfItsAttributes) {
  const char * text = "<p:a xmlns:p=\"urn:p\" xmlns:q=\"urn:q\" p:x=\"1\" q:y=\"2\" z=\"3\"><p:b/></p:a>";
  GTEXT_Markup_Document * from = nullptr;
  GTEXT_XML_Error err;
  ASSERT_EQ(gtext_xml_parse(text, strlen(text), nullptr, &from, &err), GTEXT_XML_OK);
  const GTEXT_Markup_Node * a = gtext_markup_document_element(from);
  ASSERT_NE(a, nullptr);

  GTEXT_Markup_Document * to = nullptr;
  ASSERT_EQ(gtext_markup_document_new(nullptr, &to), GTEXT_MARKUP_OK);
  for (bool deep : {false, true}) {
    GTEXT_Markup_Node * copy = nullptr;
    ASSERT_EQ(gtext_markup_clone(to, a, deep, &copy), GTEXT_MARKUP_OK);
    EXPECT_STREQ(gtext_markup_element_prefix(copy), "p");
    EXPECT_STREQ(gtext_markup_element_namespace(copy), "urn:p");
    EXPECT_STREQ(gtext_markup_element_local_name(copy), "a");
    ASSERT_EQ(gtext_markup_element_attribute_count(copy), gtext_markup_element_attribute_count(a));
    for (size_t i = 0; i < gtext_markup_element_attribute_count(a); i++) {
      GTEXT_Markup_Attribute x, y;
      ASSERT_EQ(gtext_markup_element_attribute_at(a, i, &x), GTEXT_MARKUP_OK);
      ASSERT_EQ(gtext_markup_element_attribute_at(copy, i, &y), GTEXT_MARKUP_OK);
      EXPECT_STREQ(x.local, y.local);
      EXPECT_STREQ(x.prefix ? x.prefix : "", y.prefix ? y.prefix : "");
      EXPECT_STREQ(x.ns ? x.ns : "", y.ns ? y.ns : "");
      EXPECT_EQ(std::string(x.value, x.value_length), std::string(y.value, y.value_length));
    }
    EXPECT_EQ(gtext_markup_node_first_child(copy) != nullptr, deep);
    if (deep) {
      EXPECT_TRUE(gtext_markup_equal(a, copy));
    }
  }
  gtext_markup_document_free(to);
  gtext_markup_document_free(from);
}

TEST(CoverageGapsMarkup, AnAttributeIndexPastTheEndIsInvalidAndWritesNothing) {
  const char * text = "<a x=\"1\"/>";
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_XML_Error err;
  ASSERT_EQ(gtext_xml_parse(text, strlen(text), nullptr, &doc, &err), GTEXT_XML_OK);
  const GTEXT_Markup_Node * a = gtext_markup_document_element(doc);
  static const char sentinel[] = "sentinel";
  GTEXT_Markup_Attribute out = {nullptr, nullptr, sentinel, nullptr, 7};
  EXPECT_EQ(gtext_markup_element_attribute_at(a, 1, &out), GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_element_attribute_at(a, SIZE_MAX, &out), GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_element_attribute_at(a, 0, nullptr), GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(gtext_markup_element_attribute_at(nullptr, 0, &out), GTEXT_MARKUP_E_INVALID);
  EXPECT_EQ(out.local, sentinel) << "a refusal writes nothing";
  EXPECT_EQ(out.value_length, 7u);
  EXPECT_EQ(gtext_markup_element_attribute_at(a, 0, &out), GTEXT_MARKUP_OK);
  gtext_markup_document_free(doc);
}

TEST(CoverageGapsHtml, ACharacterReferenceAboveTheBasicPlaneIsFourBytesOfUtf8InTextAndInAValue) {
  const char * text = "<p title=\"&#x1F600;&#128512;\">&#x1F600;&#128512;&#x10FFFF;</p>";
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_HTML_Result result;
  ASSERT_EQ(gtext_html_parse(text, strlen(text), nullptr, &doc, &result), GTEXT_HTML_OK);
  gtext_html_result_free(&result);
  const GTEXT_Markup_Node * root = gtext_markup_document_root(doc);
  const GTEXT_Markup_Node * p = nullptr;
  for (const GTEXT_Markup_Node * n = gtext_markup_node_next_in_order(root, root, false); n;
       n = gtext_markup_node_next_in_order(root, n, false)) {
    if (gtext_markup_node_kind(n) == GTEXT_MARKUP_KIND_ELEMENT && gtext_markup_element_is(n, GTEXT_MARKUP_NS_HTML, "p")) {
      p = n;
    }
  }
  ASSERT_NE(p, nullptr);
  GTEXT_Markup_Attribute title;
  ASSERT_TRUE(gtext_markup_element_get_attribute(p, nullptr, "title", &title));
  EXPECT_EQ(std::string(title.value, title.value_length), "\xF0\x9F\x98\x80\xF0\x9F\x98\x80");
  char * content = nullptr;
  ASSERT_EQ(gtext_markup_node_text_content(p, nullptr, &content, nullptr), GTEXT_MARKUP_OK);
  EXPECT_EQ(std::string(content), "\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF4\x8F\xBF\xBF");
  gtext_markup_string_free(nullptr, content);
  gtext_markup_document_free(doc);
}

TEST(CoverageGapsHtml, FinishingNoParserIsInvalidForADocumentAndForAFragmentAndTheResultIsFilled) {
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_HTML_Result r;
  memset(&r, 0x55, sizeof(r));
  EXPECT_EQ(gtext_html_parser_finish(nullptr, &doc, &r), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(doc, nullptr);
  EXPECT_EQ(r.status, GTEXT_HTML_E_INVALID);
  gtext_html_result_free(&r);
  GTEXT_Markup_Node * fragment = nullptr;
  EXPECT_EQ(gtext_html_parser_finish_fragment(nullptr, &doc, &fragment, &r), GTEXT_HTML_E_INVALID);
  EXPECT_EQ(fragment, nullptr);
  gtext_html_result_free(&r);
  EXPECT_EQ(gtext_html_parser_finish(nullptr, &doc, nullptr), GTEXT_HTML_E_INVALID);
}

namespace {

size_t errors_named(const std::string & html, const char * name) {
  GTEXT_Markup_Document * doc = nullptr;
  GTEXT_HTML_Result result;
  EXPECT_EQ(gtext_html_parse(html.data(), html.size(), nullptr, &doc, &result), GTEXT_HTML_OK) << html;
  size_t n = 0;
  for (size_t i = 0; i < gtext_html_result_error_count(&result); i++) {
    GTEXT_HTML_Error e;
    EXPECT_TRUE(gtext_html_result_error_at(&result, i, &e));
    if (std::string(e.name) == name) {
      n++;
    }
  }
  gtext_html_result_free(&result);
  gtext_markup_document_free(doc);
  return n;
}

}  // namespace

// The tree-construction vectors compare the tree, and their error lists are held only to "never fewer" than the vector's,
// so these four arms of "in body" (an option, an optgroup, an hr or a ruby part that finds the element it should have closed
// still open, after the implied end tags) had no test that notices if they stop reporting. The standard's text for each is
// "if ... is in scope, then this is a parse error" / "if the current node is not a ruby element".
TEST(CoverageGapsHtml, AnElementThatTheImpliedEndTagsDidNotCloseIsReportedWhereTheStandardSaysAndNotWhereItDoesNot) {
  const char * e = "unexpected-element-in-context";
  const std::string d = "<!DOCTYPE html>";
  EXPECT_EQ(errors_named(d + "<select><option><div><option>", e), 1u);
  EXPECT_EQ(errors_named(d + "<select><optgroup><div><optgroup>", e), 1u);
  EXPECT_EQ(errors_named(d + "<select><option><div><hr>", e), 1u);
  EXPECT_EQ(errors_named(d + "<ruby><span><rb>", e), 1u);
  EXPECT_EQ(errors_named(d + "<ruby><span><rtc>", e), 1u);
  // The same elements where the implied end tags do close what is open: no such error.
  EXPECT_EQ(errors_named(d + "<select><option><option>", e), 0u);
  EXPECT_EQ(errors_named(d + "<select><optgroup><optgroup>", e), 0u);
  EXPECT_EQ(errors_named(d + "<select><option><hr>", e), 0u);
  EXPECT_EQ(errors_named(d + "<ruby><rb>", e), 0u);
  EXPECT_EQ(errors_named(d + "<ruby><rb><rtc>", e), 0u);
}

// The tokenizer's own interface (internal: the tree builder is its client), the arms that say no.
TEST(CoverageGapsTokenizer, EveryStateTheBuilderCanSetIsTheStateAskedForAndNoOtherValueIsAccepted) {
  GTEXT_HTML_Tokenizer * t = gtext_html_tokenizer_new(nullptr);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(gtext_html_tokenizer_get_state(nullptr), GTEXT_HTML_STATE_DATA);
  for (GTEXT_HTML_TokenizerState st : {GTEXT_HTML_STATE_RCDATA, GTEXT_HTML_STATE_RAWTEXT, GTEXT_HTML_STATE_SCRIPT_DATA,
           GTEXT_HTML_STATE_PLAINTEXT, GTEXT_HTML_STATE_CDATA_SECTION, GTEXT_HTML_STATE_DATA}) {
    EXPECT_EQ(gtext_html_tokenizer_set_state(t, st), GTEXT_HTML_TOKENIZER_OK) << st;
    EXPECT_EQ(gtext_html_tokenizer_get_state(t), st);
  }
  EXPECT_EQ(gtext_html_tokenizer_set_state(t, static_cast<GTEXT_HTML_TokenizerState>(99)), GTEXT_HTML_TOKENIZER_E_INVALID);
  EXPECT_EQ(gtext_html_tokenizer_get_state(t), GTEXT_HTML_STATE_DATA) << "a refused state changes nothing";
  const GTEXT_HTML_Token * tok = nullptr;
  EXPECT_EQ(gtext_html_tokenizer_next(nullptr, &tok), GTEXT_HTML_TOKENIZER_E_INVALID);
  EXPECT_EQ(gtext_html_tokenizer_next(t, nullptr), GTEXT_HTML_TOKENIZER_E_INVALID);
  gtext_html_tokenizer_free(t);
}

TEST(CoverageGapsTokenizer, InputOfTheOtherKindAfterTheFirstOrAfterTheEndIsRefused) {
  // A tokenizer reads either its own input (feed) or a decoder's with its source map (feed_mapped), from its first byte to its last.
  {
    GTEXT_HTML_Tokenizer * t = gtext_html_tokenizer_new(nullptr);
    ASSERT_NE(t, nullptr);
    const size_t map[3] = {0, 1, 2};
    EXPECT_EQ(gtext_html_tokenizer_feed_mapped(t, "abc", 3, nullptr), GTEXT_HTML_TOKENIZER_E_INVALID) << "a map is required with bytes";
    EXPECT_EQ(gtext_html_tokenizer_feed_mapped(t, "abc", 3, map), GTEXT_HTML_TOKENIZER_OK);
    const GTEXT_HTML_Token * tok = nullptr;
    while (gtext_html_tokenizer_next(t, &tok) == GTEXT_HTML_TOKENIZER_TOKEN) {
    }
    EXPECT_EQ(gtext_html_tokenizer_feed(t, "def", 3), GTEXT_HTML_TOKENIZER_E_INVALID) << "plain input after mapped input";
    gtext_html_tokenizer_free(t);
  }
  {
    GTEXT_HTML_Tokenizer * t = gtext_html_tokenizer_new(nullptr);
    ASSERT_NE(t, nullptr);
    const size_t map[3] = {0, 1, 2};
    EXPECT_EQ(gtext_html_tokenizer_feed(t, "abc", 3), GTEXT_HTML_TOKENIZER_OK);
    const GTEXT_HTML_Token * tok = nullptr;
    while (gtext_html_tokenizer_next(t, &tok) == GTEXT_HTML_TOKENIZER_TOKEN) {
    }
    EXPECT_EQ(gtext_html_tokenizer_feed_mapped(t, "def", 3, map), GTEXT_HTML_TOKENIZER_E_INVALID) << "mapped input after plain input";
    gtext_html_tokenizer_finish(t);
    EXPECT_EQ(gtext_html_tokenizer_feed(t, "g", 1), GTEXT_HTML_TOKENIZER_E_STATE) << "input after the end";
    gtext_html_tokenizer_free(t);
  }
  {
    GTEXT_HTML_Tokenizer * t = gtext_html_tokenizer_new(nullptr);
    ASSERT_NE(t, nullptr);
    // Borrowed input not yet taken: the next feed has to wait for the tokenizer to have read the last.
    EXPECT_EQ(gtext_html_tokenizer_feed(t, "abc", 3), GTEXT_HTML_TOKENIZER_OK);
    EXPECT_EQ(gtext_html_tokenizer_feed(t, "def", 3), GTEXT_HTML_TOKENIZER_E_STATE);
    gtext_html_tokenizer_free(t);
  }
}

namespace {

std::string sniffed(const std::string & bytes) {
  GTEXT_HTML_Sniff_Result r;
  EXPECT_EQ(gtext_html_sniff_encoding(bytes.data(), bytes.size(), nullptr, &r), GTEXT_HTML_OK);
  return r.encoding_name ? r.encoding_name : "";
}

}  // namespace

// The prescan of the standard ends at the end of what it is given. A declaration it has not been given the end of is no declaration
// ("If there is no next character: return nothing", "an unmatched quote: return nothing"), so the default stands; the same bytes with the
// declaration finished name their encoding. Cut at every one of the places the algorithm's loops can run out of input.
TEST(CoverageGapsHtml, ADeclarationTheInputEndsInsideDeclaresNothingAndTheFinishedOneDoes) {
  const struct {
    const char * cut;
    const char * whole;
  } cases[] = {
      {"<meta charset", "<meta charset=shift_jis>"},
      {"<meta charset ", "<meta charset=shift_jis>"},
      {"<meta charset=", "<meta charset=shift_jis>"},
      {"<meta charset= ", "<meta charset=shift_jis>"},
      {"<meta charset=\"shift_jis", "<meta charset=\"shift_jis\">"},
      {"<meta charset='shift_jis", "<meta charset='shift_jis'>"},
      {"<meta http-equiv=content-type content=\"text/html; charset", "<meta http-equiv=content-type content=\"text/html; charset=shift_jis\">"},
      {"<meta http-equiv=content-type content=\"text/html; charset=", "<meta http-equiv=content-type content=\"text/html; charset=shift_jis\">"},
      {"<meta http-equiv=content-type content=\"text/html; charset=\"", "<meta http-equiv=content-type content=\"text/html; charset=shift_jis\">"},
      {"<meta http-equiv=content-type content=\"text/html; charset=shift_jis", "<meta http-equiv=content-type content=\"text/html; charset=shift_jis\">"},
      {"<?xml version=\"1.0\" encoding=\"shift_jis", "<?xml version=\"1.0\" encoding=\"shift_jis\"?>"},
  };
  for (const auto & c : cases) {
    SCOPED_TRACE(c.cut);
    // The cut ones still end in a tag the standard calls unfinished; only the finished ones declare. (An XML declaration is read only
    // when there is no `<meta>`, and only for the encodings the standard lets it name.)
    const std::string cut = sniffed(c.cut);
    EXPECT_EQ(cut, "UTF-8") << "an unfinished declaration declares nothing";
    const std::string whole = sniffed(c.whole);
    EXPECT_NE(whole, "") << c.whole;
  }
  EXPECT_EQ(sniffed("<meta charset=shift_jis>"), "Shift_JIS");
  EXPECT_EQ(sniffed("<meta http-equiv=content-type content=\"text/html; charset=shift_jis\">"), "Shift_JIS");
  // A byte order mark of one byte is not one.
  EXPECT_EQ(sniffed(std::string("\xFE", 1)), "UTF-8");
  EXPECT_EQ(sniffed(std::string("\xFF", 1)), "UTF-8");
  EXPECT_EQ(sniffed(std::string("\xFE\xFF", 2)), "UTF-16BE");
  EXPECT_EQ(sniffed(std::string("\xFF\xFE", 2)), "UTF-16LE");
}

// A limit met while the decoder is feeding the builder (the document is not UTF-8, so its bytes reach the tree through the decoder's
// sink) is the same status as in the UTF-8 route, for the whole parse and the incremental one, and the failure is sticky.
TEST(CoverageGapsHtml, ALimitMetThroughTheDecodersSinkIsTheSameStatusAsInTheNativeRoute) {
  std::string html = "<!DOCTYPE html>";
  for (int i = 0; i < 40; i++) {
    html += "<p>\xE9";
  }
  for (const char * label : {"utf-8", "windows-1252", "utf-16le"}) {
    SCOPED_TRACE(label);
    std::string bytes = html;
    if (std::string(label) == "utf-16le") {
      bytes.clear();
      for (char c : html) {
        bytes += (c == (char)0xE9) ? std::string("\xE9\x00", 2) : std::string(1, c) + '\0';
      }
    }
    else if (std::string(label) == "utf-8") {
      std::string u;
      for (char c : html) {
        u += c == (char)0xE9 ? std::string("\xC3\xA9") : std::string(1, c);
      }
      bytes = u;
    }
    GTEXT_HTML_Parse_Options o = gtext_html_parse_options_default();
    o.max_nodes = 12;
    o.transport_encoding = label;
    GTEXT_Markup_Document * doc = nullptr;
    GTEXT_HTML_Result r;
    EXPECT_EQ(gtext_html_parse(bytes.data(), bytes.size(), &o, &doc, &r), GTEXT_HTML_E_LIMIT_NODES);
    EXPECT_EQ(doc, nullptr);
    gtext_html_result_free(&r);
    GTEXT_HTML_Parser * p = nullptr;
    ASSERT_EQ(gtext_html_parser_new(&o, &p), GTEXT_HTML_OK);
    GTEXT_HTML_Status st = GTEXT_HTML_OK;
    for (size_t i = 0; i < bytes.size() && st == GTEXT_HTML_OK; i++) {
      st = gtext_html_parser_feed(p, bytes.data() + i, 1);
    }
    EXPECT_EQ(st, GTEXT_HTML_E_LIMIT_NODES);
    EXPECT_EQ(gtext_html_parser_feed(p, "x", 1), GTEXT_HTML_E_LIMIT_NODES) << "sticky";
    EXPECT_EQ(gtext_html_parser_finish(p, &doc, &r), GTEXT_HTML_E_LIMIT_NODES);
    gtext_html_result_free(&r);
    gtext_html_parser_free(p);
  }
}

#include <ghoti.io/text/encoding.h>

// A multi-byte sequence that fails on an ASCII byte puts that byte back to be read again (the standard's "restore"), with no source map
// asked for. Valgrind reported a read of an uninitialised offset on exactly this path (870,838 times in testEncodingCjk) and no
// sanitizer of this build can: the answer here is the standard's, and the Valgrind run of this program is what shows the read is gone.
TEST(CoverageGapsEncoding, ARestoredAsciiByteIsReadAgainWithoutASourceMap) {
  struct Case {
    GTEXT_Encoding encoding;
    std::string bytes;
  };
  const Case cases[] = {
      {GTEXT_ENCODING_SHIFT_JIS, "\x81 z"},
      {GTEXT_ENCODING_EUC_JP, "\xA1 z"},
      {GTEXT_ENCODING_EUC_KR, "\x81 z"},
      {GTEXT_ENCODING_BIG5, "\x81 z"},
      {GTEXT_ENCODING_GBK, "\x81 z"},
      {GTEXT_ENCODING_GB18030, "\x81\x30 z"},
      {GTEXT_ENCODING_GB18030, "\x81\x30\x81 z"},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(gtext_encoding_name(c.encoding));
    char * out = nullptr;
    size_t len = 0;
    ASSERT_EQ(gtext_encoding_decode_buffer(c.encoding, nullptr, c.bytes.data(), c.bytes.size(), &out, &len, nullptr), GTEXT_ENCODING_OK);
    const std::string got(out, len);
    gtext_encoding_buffer_free(out);
    EXPECT_NE(got.find("\xEF\xBF\xBD"), std::string::npos) << "the broken sequence is one replacement character";
    EXPECT_EQ(got.back(), 'z') << "and the ASCII byte after it was not swallowed";
  }
}
